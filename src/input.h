#ifndef TREE_SITTER_DASH_INPUT_H
#define TREE_SITTER_DASH_INPUT_H

#include "source.h"
#include "tree_sitter/parser.h"

#define SOURCE_BACKQUOTE_BOUNDARY INT32_C(0x110000)

struct NativeInput {
  TSLexer *lexer;
  int32_t *characters;
  size_t count;
  size_t capacity;
  enum SourceResult (*boundary)(void *, size_t);
  void *boundary_payload;
  bool failed;
};

struct LogicalLexer {
  TSLexer lexer;
  struct NativeInput *native;
  size_t read_position;
  size_t base;
  struct SourceCursor cursor;
  struct SourceCharacter character;
  enum SourceResult result;
  size_t consumed;
  size_t mark;
  bool bounded;
};

struct LogicalPosition {
  size_t position;
  size_t consumed;
  size_t mark;
  struct SourceCharacter character;
  enum SourceResult result;
  int32_t lookahead;
  TSSymbol symbol;
};

static inline struct LogicalPosition
logical_position(const struct LogicalLexer *input) {
  return (struct LogicalPosition){
    .position = input->cursor.logical_position,
    .consumed = input->consumed,
    .mark = input->mark,
    .character = input->character,
    .result = input->result,
    .lookahead = input->lexer.lookahead,
    .symbol = input->lexer.result_symbol,
  };
}

static inline void logical_restore_position(
  struct LogicalLexer *input,
  const struct LogicalPosition *position
) {
  input->cursor.logical_position = position->position;
  input->consumed = position->consumed;
  input->mark = position->mark;
  input->character = position->character;
  input->result = position->result;
  input->lexer.lookahead = position->lookahead;
  input->lexer.result_symbol = position->symbol;
}

static inline int32_t logical_peek(struct LogicalLexer *input) {
  struct LogicalPosition position = logical_position(input);
  input->lexer.advance(&input->lexer, false);
  int32_t value =
    input->result == SOURCE_CHARACTER ? input->lexer.lookahead : 0;
  logical_restore_position(input, &position);
  return value;
}

static enum SourceResult logical_read(void *payload, int32_t *value) {
  struct LogicalLexer *input = payload;
  struct NativeInput *native = input->native;
  if (native->failed) {
    return SOURCE_FAILURE;
  }
  if (input->bounded && native->boundary != NULL) {
    enum SourceResult result =
      native->boundary(native->boundary_payload, input->read_position);
    if (result != SOURCE_CHARACTER) {
      native->failed = result == SOURCE_FAILURE;
      return result;
    }
  }
  if (input->read_position > native->count) {
    return SOURCE_FAILURE;
  }
  if (input->read_position == native->count) {
    if (native->lexer->eof(native->lexer)) {
      return SOURCE_END;
    }
    int32_t *characters = source_grow(
      native->characters,
      &native->capacity,
      native->count + 1,
      sizeof(*characters)
    );
    if (characters == NULL) {
      native->failed = true;
      return SOURCE_FAILURE;
    }
    native->characters = characters;
    native->characters[native->count++] = native->lexer->lookahead;
    native->lexer->advance(native->lexer, false);
  }
  *value = native->characters[input->read_position++];
  return SOURCE_CHARACTER;
}

static void logical_load(struct LogicalLexer *input) {
  do {
    input->result = source_next(&input->cursor, &input->character);
  } while (
    input->result == SOURCE_CHARACTER && input->character.origin.end <= 0
  );
  input->lexer.lookahead = input->result != SOURCE_CHARACTER ? 0
    : input->character.boundary_stage == SIZE_MAX ? input->character.value
                                                  : SOURCE_BACKQUOTE_BOUNDARY;
}

static bool logical_backquote_end(const struct LogicalLexer *input) {
  if (
    input == NULL ||
    input->result != SOURCE_CHARACTER ||
    input->character.boundary_stage == SIZE_MAX
  ) {
    return false;
  }
  for (size_t index = input->cursor.stage_count; index > 0; index -= 1) {
    const struct SourceStage *stage = &input->cursor.stages[index - 1];
    if (stage->kind == SOURCE_BACKQUOTE_DECODE && !stage->disabled) {
      return input->character.boundary_stage == index - 1;
    }
  }
  return false;
}

static void logical_advance(TSLexer *lexer, bool skip) {
  (void)skip;
  struct LogicalLexer *input = (struct LogicalLexer *)lexer;
  if (input->result == SOURCE_CHARACTER) {
    input->consumed = input->base +
      (input->character.origin.end > 0 ? (size_t)input->character.origin.end
                                       : 0);
    logical_load(input);
  }
}

static void logical_mark_end(TSLexer *lexer) {
  struct LogicalLexer *input = (struct LogicalLexer *)lexer;
  input->mark = input->consumed;
}

static bool logical_eof(const TSLexer *lexer) {
  const struct LogicalLexer *input = (const struct LogicalLexer *)lexer;
  return input->result != SOURCE_CHARACTER;
}

static inline bool logical_init_at(
  struct LogicalLexer *input,
  struct NativeInput *native,
  const struct SourceSnapshot *snapshot,
  size_t base,
  bool bounded
) {
  *input = (struct LogicalLexer){
    .lexer = *native->lexer,
    .native = native,
    .read_position = base,
    .base = base,
    .consumed = base,
    .mark = base,
    .bounded = bounded,
  };
  input->lexer.advance = logical_advance;
  input->lexer.mark_end = logical_mark_end;
  input->lexer.eof = logical_eof;
  if (!source_cursor_resume(&input->cursor, snapshot, logical_read, input)) {
    return false;
  }
  logical_load(input);
  if (input->result == SOURCE_FAILURE) {
    source_cursor_clear(&input->cursor);
  }
  return input->result != SOURCE_FAILURE;
}

static inline bool logical_init(
  struct LogicalLexer *input,
  struct NativeInput *native,
  const struct SourceSnapshot *snapshot
) {
  return logical_init_at(input, native, snapshot, 0, true);
}

static inline bool logical_fork(
  struct LogicalLexer *fork,
  const struct LogicalLexer *parent,
  size_t raw_cut,
  size_t inherited_stage_count,
  const struct SourceStage *suffix,
  size_t suffix_count
) {
  if (fork == parent) {
    return false;
  }
  *fork = (struct LogicalLexer){0};
  if (
    raw_cut < parent->base ||
    raw_cut > parent->native->count ||
    inherited_stage_count > parent->cursor.stage_count ||
    suffix_count > (SOURCE_STATE_CAPACITY - 2) / 2 ||
    inherited_stage_count > (SOURCE_STATE_CAPACITY - 2) / 2 - suffix_count ||
    (suffix_count > 0 && suffix == NULL)
  ) {
    return false;
  }
  struct SourceSnapshot snapshot = {0};
  if (!source_snapshot_at(&parent->cursor, raw_cut - parent->base, &snapshot)) {
    return false;
  }
  for (
    size_t index = inherited_stage_count; index < snapshot.stage_count;
    index += 1
  ) {
    if (snapshot.stages[index].has_pending) {
      source_snapshot_clear(&snapshot);
      return false;
    }
  }
  size_t count = inherited_stage_count + suffix_count;
  struct SourceStage *stages =
    count == 0 ? NULL : ts_malloc(count * sizeof(*stages));
  if (count > 0 && stages == NULL) {
    source_snapshot_clear(&snapshot);
    return false;
  }
  if (inherited_stage_count > 0) {
    memcpy(stages, snapshot.stages, inherited_stage_count * sizeof(*stages));
  }
  if (suffix_count > 0) {
    memcpy(
      stages + inherited_stage_count,
      suffix,
      suffix_count * sizeof(*stages)
    );
  }
  ts_free(snapshot.stages);
  snapshot.stages = stages;
  snapshot.stage_count = count;
  bool result =
    logical_init_at(fork, parent->native, &snapshot, raw_cut, parent->bounded);
  source_snapshot_clear(&snapshot);
  return result;
}

static inline bool logical_replace_view(
  struct LogicalLexer *input,
  size_t inherited_stage_count,
  const struct SourceStage *suffix,
  size_t suffix_count
) {
  struct LogicalLexer next;
  if (!logical_fork(
        &next,
        input,
        input->consumed,
        inherited_stage_count,
        suffix,
        suffix_count
      )) {
    return false;
  }
  next.mark = input->mark;
  source_cursor_clear(&input->cursor);
  *input = next;
  input->cursor.payload = input;
  return true;
}

static inline size_t logical_raw_position(const struct LogicalLexer *input) {
  return input->base +
    (input->result == SOURCE_END ? input->cursor.raw_count
        : input->character.origin.start > 0
        ? (size_t)input->character.origin.start
        : 0);
}

static inline void logical_rewind(struct LogicalLexer *input) {
  input->cursor.logical_position = 0;
  input->consumed = input->base;
  input->mark = input->base;
  logical_load(input);
}

static inline void logical_clear(struct LogicalLexer *input) {
  source_cursor_clear(&input->cursor);
}

static inline void native_input_clear(struct NativeInput *input) {
  ts_free(input->characters);
  *input = (struct NativeInput){0};
}

#endif
