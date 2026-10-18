#ifndef TREE_SITTER_DASH_SOURCE_H_
#define TREE_SITTER_DASH_SOURCE_H_

#include "tree_sitter/alloc.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SOURCE_STATE_CAPACITY 1024

enum SourceResult {
  SOURCE_CHARACTER,
  SOURCE_END,
  SOURCE_FAILURE,
};

enum SourceStageKind {
  SOURCE_REMOVE_CONTINUATIONS,
  SOURCE_BACKQUOTE_DECODE,
  SOURCE_STRIP_LEADING_TABS,
  SOURCE_HERE_DOCUMENT,
};

struct SourceOrigin {
  /* Resumed units may have advance ordinals before this callback. */
  int64_t start;
  int64_t end;
  int64_t character;
};

struct SourceCharacter {
  int32_t value;
  struct SourceOrigin origin;
  size_t boundary_stage;
  bool physical_tail;
};

struct SourceStage {
  enum SourceStageKind kind;
  bool disabled;
  bool quoted;
  bool at_line_start;
  bool strip_tabs;
  bool tab_prefix;
  bool has_pending;
  struct SourceCharacter pending;
};

enum SourceStageFlag {
  SOURCE_STAGE_FLAG_DISABLED = 1,
  SOURCE_STAGE_FLAG_QUOTED = 2,
  SOURCE_STAGE_FLAG_AT_LINE_START = 4,
  SOURCE_STAGE_FLAG_HAS_PENDING = 8,
  SOURCE_STAGE_FLAG_STRIP_TABS = 16,
  SOURCE_STAGE_FLAG_TAB_PREFIX = 32,
  SOURCE_STAGE_FLAG_PHYSICAL_TAIL = 64,
  SOURCE_STAGE_FLAG_MASK = 127,
  SOURCE_STAGE_FLAG_HERE_DOCUMENT_ONLY = SOURCE_STAGE_FLAG_STRIP_TABS |
    SOURCE_STAGE_FLAG_TAB_PREFIX,
};

enum SourceRemovalKind {
  SOURCE_REMOVED_CONTINUATION,
  SOURCE_REMOVED_TAB,
};

struct SourceRemoval {
  enum SourceRemovalKind kind;
  struct SourceOrigin origin;
  int64_t slash;
  size_t stage;
};

struct SourceSnapshot {
  struct SourceStage *stages;
  size_t stage_count;
  int64_t raw_position;
  size_t logical_count;
  size_t removal_count;
  bool ended;
};

typedef enum SourceResult (*SourceRead)(void *, int32_t *);

struct SourceCursor {
  SourceRead read;
  void *payload;
  struct SourceStage *initial;
  struct SourceStage *stages;
  size_t stage_count;
  int32_t *raw;
  size_t raw_count;
  size_t raw_capacity;
  struct SourceCharacter *logical;
  size_t logical_count;
  size_t logical_capacity;
  size_t logical_position;
  struct SourceRemoval *removals;
  size_t removal_count;
  size_t removal_capacity;
  bool ended;
  bool failed;
};

static inline struct SourceStage source_stage(enum SourceStageKind kind) {
  struct SourceStage stage = {
    .kind = kind,
    .at_line_start =
      kind == SOURCE_STRIP_LEADING_TABS || kind == SOURCE_HERE_DOCUMENT,
  };
  return stage;
}

static inline void *
source_grow(void *data, size_t *capacity, size_t count, size_t width) {
  if (count <= *capacity) {
    return data;
  }
  if (width == 0 || count > SIZE_MAX / width) {
    return NULL;
  }
  size_t maximum = SIZE_MAX / width;
  size_t next = *capacity == 0 ? (maximum < 16 ? maximum : 16) : *capacity;
  while (next < count) {
    next = next > maximum / 2 ? maximum : next * 2;
  }
  void *grown = ts_realloc(data, next * width);
  if (grown != NULL) {
    *capacity = next;
  }
  return grown;
}

static inline void source_snapshot_clear(struct SourceSnapshot *snapshot) {
  ts_free(snapshot->stages);
  memset(snapshot, 0, sizeof(*snapshot));
}

static inline void source_cursor_clear(struct SourceCursor *cursor) {
  ts_free(cursor->initial);
  ts_free(cursor->stages);
  ts_free(cursor->raw);
  ts_free(cursor->logical);
  ts_free(cursor->removals);
  memset(cursor, 0, sizeof(*cursor));
}

static inline bool source_cursor_init(
  struct SourceCursor *cursor,
  const struct SourceStage *stages,
  size_t count,
  SourceRead read,
  void *payload
) {
  memset(cursor, 0, sizeof(*cursor));
  if (
    count > (SOURCE_STATE_CAPACITY - 2) / 2 ||
    count > SIZE_MAX / sizeof(*stages) ||
    (count > 0 && stages == NULL)
  ) {
    return false;
  }
  for (size_t index = 0; index < count; index += 1) {
    if (
      stages[index].kind < SOURCE_REMOVE_CONTINUATIONS ||
      stages[index].kind > SOURCE_HERE_DOCUMENT
    ) {
      return false;
    }
  }
  if (count > 0) {
    cursor->initial = ts_malloc(count * sizeof(*stages));
    cursor->stages = ts_malloc(count * sizeof(*stages));
    if (cursor->initial == NULL || cursor->stages == NULL) {
      source_cursor_clear(cursor);
      return false;
    }
    memcpy(cursor->initial, stages, count * sizeof(*stages));
    memcpy(cursor->stages, stages, count * sizeof(*stages));
  }
  cursor->stage_count = count;
  cursor->read = read;
  cursor->payload = payload;
  return true;
}

static inline bool source_append_logical(
  struct SourceCursor *cursor,
  struct SourceCharacter character
) {
  if (cursor->logical_count == SIZE_MAX) {
    return false;
  }
  struct SourceCharacter *grown = source_grow(
    cursor->logical,
    &cursor->logical_capacity,
    cursor->logical_count + 1,
    sizeof(*grown)
  );
  if (grown == NULL) {
    return false;
  }
  cursor->logical = grown;
  cursor->logical[cursor->logical_count++] = character;
  return true;
}

static inline bool source_append_removal(
  struct SourceCursor *cursor,
  enum SourceRemovalKind kind,
  struct SourceOrigin origin,
  int64_t slash,
  size_t stage
) {
  if (cursor->removal_count == SIZE_MAX) {
    return false;
  }
  struct SourceRemoval *grown = source_grow(
    cursor->removals,
    &cursor->removal_capacity,
    cursor->removal_count + 1,
    sizeof(*grown)
  );
  if (grown == NULL) {
    return false;
  }
  cursor->removals = grown;
  cursor->removals[cursor->removal_count++] = (struct SourceRemoval){
    .kind = kind,
    .origin = origin,
    .slash = slash,
    .stage = stage,
  };
  return true;
}

static inline bool source_remove_continuation(
  struct SourceCursor *cursor,
  struct SourceCharacter pending,
  struct SourceCharacter newline,
  size_t stage
) {
  int64_t slash = pending.physical_tail ? pending.origin.character : INT64_MIN;
  return source_append_removal(
    cursor,
    SOURCE_REMOVED_CONTINUATION,
    newline.origin,
    slash,
    stage
  );
}

static inline bool source_remove_tab(
  struct SourceCursor *cursor,
  struct SourceCharacter tab,
  size_t stage
) {
  return source_append_removal(
    cursor,
    SOURCE_REMOVED_TAB,
    tab.origin,
    INT64_MIN,
    stage
  );
}

static inline bool source_feed_stage(
  struct SourceCursor *cursor,
  size_t index,
  struct SourceCharacter character
);

static inline bool
source_flush_pending(struct SourceCursor *cursor, size_t index) {
  for (; index < cursor->stage_count; index += 1) {
    struct SourceStage *stage = &cursor->stages[index];
    if (stage->has_pending) {
      struct SourceCharacter pending = stage->pending;
      stage->has_pending = false;
      if (!source_feed_stage(cursor, index + 1, pending)) {
        return false;
      }
    }
  }
  return true;
}

static inline bool source_feed_stage(
  struct SourceCursor *cursor,
  size_t index,
  struct SourceCharacter character
) {
  if (index == cursor->stage_count) {
    return source_append_logical(cursor, character);
  }
  if (character.boundary_stage != SIZE_MAX) {
    /* Pending backslashes cannot pair across this source boundary. */
    return source_flush_pending(cursor, index) &&
      source_append_logical(cursor, character);
  }
  struct SourceStage *stage = &cursor->stages[index];
  if (stage->kind == SOURCE_HERE_DOCUMENT) {
    if (stage->disabled) {
      stage->at_line_start = character.value == '\n';
      stage->tab_prefix = false;
      return source_feed_stage(cursor, index + 1, character);
    }
    if (stage->has_pending) {
      struct SourceCharacter pending = stage->pending;
      stage->has_pending = false;
      if (character.value == '\n' && !stage->tab_prefix) {
        return source_remove_continuation(cursor, pending, character, index);
      }
      stage->at_line_start = false;
      stage->tab_prefix = false;
      return source_feed_stage(cursor, index + 1, pending) &&
        source_feed_stage(cursor, index + 1, character);
    }
    if (stage->strip_tabs && stage->at_line_start && character.value == '\t') {
      stage->tab_prefix = true;
      return source_remove_tab(cursor, character, index);
    }
    if (!stage->quoted && character.value == '\\') {
      stage->pending = character;
      stage->has_pending = true;
      return true;
    }
    stage->at_line_start = character.value == '\n';
    stage->tab_prefix = false;
    return source_feed_stage(cursor, index + 1, character);
  }
  if (stage->kind == SOURCE_STRIP_LEADING_TABS) {
    if (!stage->disabled && stage->at_line_start && character.value == '\t') {
      return source_remove_tab(cursor, character, index);
    }
    stage->at_line_start = character.value == '\n';
    return source_feed_stage(cursor, index + 1, character);
  }
  if (stage->disabled) {
    return source_feed_stage(cursor, index + 1, character);
  }
  if (stage->has_pending) {
    struct SourceCharacter pending = stage->pending;
    stage->has_pending = false;
    if (stage->kind == SOURCE_REMOVE_CONTINUATIONS && character.value == '\n') {
      return source_remove_continuation(cursor, pending, character, index);
    }
    if (
      stage->kind == SOURCE_BACKQUOTE_DECODE &&
      (character.value == '\\' ||
        character.value == '$' ||
        character.value == 96 ||
        (stage->quoted && character.value == '"'))
    ) {
      character.origin.start = pending.origin.start;
      return source_feed_stage(cursor, index + 1, character);
    }
    return source_feed_stage(cursor, index + 1, pending) &&
      source_feed_stage(cursor, index + 1, character);
  }
  if (character.value == '\\') {
    stage->pending = character;
    stage->has_pending = true;
    return true;
  }
  if (stage->kind == SOURCE_BACKQUOTE_DECODE && character.value == 96) {
    character.boundary_stage = index;
  }
  return source_feed_stage(cursor, index + 1, character);
}

static inline bool source_feed(struct SourceCursor *cursor, int32_t value) {
  if (
    cursor->failed ||
    cursor->ended ||
    cursor->raw_count == SIZE_MAX ||
    cursor->raw_count >= INT64_MAX
  ) {
    return false;
  }
  int32_t *grown = source_grow(
    cursor->raw,
    &cursor->raw_capacity,
    cursor->raw_count + 1,
    sizeof(*grown)
  );
  if (grown == NULL) {
    cursor->failed = true;
    return false;
  }
  cursor->raw = grown;
  int64_t position = (int64_t)cursor->raw_count;
  cursor->raw[cursor->raw_count++] = value;
  if (value == 0) return true;
  struct SourceCharacter character = {
    .value = value,
    .origin = {position, position + 1, position},
    .boundary_stage = SIZE_MAX,
    .physical_tail = true,
  };
  if (!source_feed_stage(cursor, 0, character)) {
    cursor->failed = true;
    return false;
  }
  for (size_t index = 0; index < cursor->stage_count; index += 1) {
    struct SourceStage *stage = &cursor->stages[index];
    if (stage->has_pending && stage->pending.origin.character != position)
      stage->pending.physical_tail = false;
  }
  return true;
}

static inline bool source_finish(struct SourceCursor *cursor) {
  if (cursor->failed) {
    return false;
  }
  if (cursor->ended) {
    return true;
  }
  if (!source_flush_pending(cursor, 0)) {
    cursor->failed = true;
    return false;
  }
  cursor->ended = true;
  return true;
}

static inline enum SourceResult
source_next(struct SourceCursor *cursor, struct SourceCharacter *character) {
  while (
    !cursor->failed &&
    cursor->logical_position == cursor->logical_count &&
    !cursor->ended
  ) {
    int32_t value;
    enum SourceResult result =
      cursor->read == NULL ? SOURCE_END : cursor->read(cursor->payload, &value);
    if (result == SOURCE_CHARACTER) {
      if (!source_feed(cursor, value)) {
        return SOURCE_FAILURE;
      }
    } else if (result == SOURCE_END) {
      if (!source_finish(cursor)) {
        return SOURCE_FAILURE;
      }
    } else {
      cursor->failed = true;
    }
  }
  if (cursor->failed) {
    return SOURCE_FAILURE;
  }
  if (cursor->logical_position < cursor->logical_count) {
    *character = cursor->logical[cursor->logical_position++];
    return SOURCE_CHARACTER;
  }
  return SOURCE_END;
}

static inline bool source_checkpoint(
  const struct SourceCursor *cursor,
  struct SourceSnapshot *snapshot
) {
  struct SourceSnapshot next = {
    .stage_count = cursor->stage_count,
    .raw_position = (int64_t)cursor->raw_count,
    .logical_count = cursor->logical_count,
    .removal_count = cursor->removal_count,
    .ended = cursor->ended,
  };
  if (cursor->failed) {
    return false;
  }
  if (next.stage_count > 0) {
    next.stages = ts_malloc(next.stage_count * sizeof(*next.stages));
    if (next.stages == NULL) {
      return false;
    }
    memcpy(
      next.stages,
      cursor->stages,
      next.stage_count * sizeof(*next.stages)
    );
  }
  source_snapshot_clear(snapshot);
  *snapshot = next;
  return true;
}

static inline bool source_snapshot_at(
  const struct SourceCursor *cursor,
  size_t raw_position,
  struct SourceSnapshot *snapshot
) {
  if (cursor->failed || raw_position > cursor->raw_count) {
    return false;
  }
  if (raw_position == cursor->raw_count && !cursor->ended) {
    return source_checkpoint(cursor, snapshot);
  }
  struct SourceCursor replay;
  if (!source_cursor_init(
        &replay,
        cursor->initial,
        cursor->stage_count,
        NULL,
        NULL
      )) {
    return false;
  }
  bool result = true;
  for (size_t index = 0; result && index < raw_position; index += 1) {
    result = source_feed(&replay, cursor->raw[index]);
  }
  if (result) {
    result = source_checkpoint(&replay, snapshot);
  }
  source_cursor_clear(&replay);
  return result;
}

static inline bool source_cursor_resume(
  struct SourceCursor *cursor,
  const struct SourceSnapshot *snapshot,
  SourceRead read,
  void *payload
) {
  if (
    snapshot->raw_position < 0 ||
    !source_cursor_init(
      cursor,
      snapshot->stages,
      snapshot->stage_count,
      read,
      payload
    )
  ) {
    return false;
  }
  for (size_t index = 0; index < cursor->stage_count; index += 1) {
    struct SourceStage *stage = &cursor->stages[index];
    if (stage->has_pending) {
      if (stage->pending.origin.start < INT64_MIN + snapshot->raw_position) {
        source_cursor_clear(cursor);
        return false;
      }
      stage->pending.origin.start -= snapshot->raw_position;
      stage->pending.origin.end -= snapshot->raw_position;
      stage->pending.origin.character -= snapshot->raw_position;
    }
    cursor->initial[index] = *stage;
  }
  cursor->ended = snapshot->ended;
  return true;
}

static inline bool source_write_unsigned(
  uint8_t *data,
  size_t capacity,
  size_t *position,
  uint64_t value
) {
  do {
    if (*position == capacity) {
      return false;
    }
    uint8_t byte = (uint8_t)(value & UINT64_C(127));
    value >>= 7;
    data[(*position)++] = byte | (value > 0 ? UINT8_C(128) : 0);
  } while (value > 0);
  return true;
}

static inline bool source_read_unsigned(
  const uint8_t *data,
  size_t length,
  size_t *position,
  uint64_t *value
) {
  *value = 0;
  for (unsigned shift = 0; shift < 64; shift += 7) {
    if (*position == length) {
      return false;
    }
    uint8_t byte = data[(*position)++];
    if (shift == 63 && (byte & UINT8_C(254)) != 0) {
      return false;
    }
    *value |= (uint64_t)(byte & UINT8_C(127)) << shift;
    if ((byte & UINT8_C(128)) == 0) {
      return shift == 0 || byte != 0;
    }
  }
  return false;
}

static inline bool
source_pending_is_valid(const struct SourceStage *stage, bool ended) {
  const struct SourceOrigin *origin = &stage->pending.origin;
  return !ended &&
    !stage->disabled &&
    stage->kind != SOURCE_STRIP_LEADING_TABS &&
    origin->start <= origin->character &&
    origin->character < origin->end;
}

static inline bool source_snapshot_serialize(
  const struct SourceSnapshot *snapshot,
  uint8_t *data,
  size_t capacity,
  size_t *length
) {
  size_t position = 0;
  *length = 0;
  if (capacity > SOURCE_STATE_CAPACITY) {
    capacity = SOURCE_STATE_CAPACITY;
  }
  if (
    snapshot->raw_position < 0 ||
    !source_write_unsigned(data, capacity, &position, snapshot->stage_count) ||
    !source_write_unsigned(data, capacity, &position, snapshot->ended ? 1 : 0)
  ) {
    return false;
  }
  for (size_t index = 0; index < snapshot->stage_count; index += 1) {
    const struct SourceStage *stage = &snapshot->stages[index];
    uint8_t flags = (stage->disabled ? SOURCE_STAGE_FLAG_DISABLED : 0) |
      (stage->quoted ? SOURCE_STAGE_FLAG_QUOTED : 0) |
      (stage->at_line_start ? SOURCE_STAGE_FLAG_AT_LINE_START : 0) |
      (stage->has_pending ? SOURCE_STAGE_FLAG_HAS_PENDING : 0) |
      (stage->strip_tabs ? SOURCE_STAGE_FLAG_STRIP_TABS : 0) |
      (stage->tab_prefix ? SOURCE_STAGE_FLAG_TAB_PREFIX : 0) |
      (stage->has_pending && stage->pending.physical_tail
          ? SOURCE_STAGE_FLAG_PHYSICAL_TAIL
          : 0);
    if (
      stage->kind < SOURCE_REMOVE_CONTINUATIONS ||
      stage->kind > SOURCE_HERE_DOCUMENT ||
      !source_write_unsigned(data, capacity, &position, stage->kind) ||
      !source_write_unsigned(data, capacity, &position, flags)
    ) {
      return false;
    }
    if (stage->has_pending) {
      const struct SourceOrigin *origin = &stage->pending.origin;
      if (
        !source_pending_is_valid(stage, snapshot->ended) ||
        stage->pending.value != '\\' ||
        stage->pending.boundary_stage != SIZE_MAX ||
        origin->end > snapshot->raw_position ||
        origin->start < snapshot->raw_position - INT64_MAX
      ) {
        return false;
      }
      const int64_t positions[] =
        {origin->start, origin->end, origin->character};
      for (size_t field = 0; field < 3; field += 1) {
        if (!source_write_unsigned(
              data,
              capacity,
              &position,
              (uint64_t)(snapshot->raw_position - positions[field])
            )) {
          return false;
        }
      }
    }
  }
  *length = position;
  return true;
}

static inline bool source_snapshot_deserialize(
  struct SourceSnapshot *snapshot,
  const uint8_t *data,
  size_t length
) {
  struct SourceSnapshot next = {0};
  size_t position = 0;
  uint64_t count;
  uint64_t ended;
  if (
    length > SOURCE_STATE_CAPACITY ||
    !source_read_unsigned(data, length, &position, &count) ||
    !source_read_unsigned(data, length, &position, &ended) ||
    count > (length - position) / 2 ||
    count > SIZE_MAX / sizeof(*next.stages) ||
    ended > 1
  ) {
    return false;
  }
  next.stage_count = (size_t)count;
  next.ended = ended != 0;
  if (next.stage_count > 0) {
    next.stages = ts_calloc(next.stage_count, sizeof(*next.stages));
    if (next.stages == NULL) {
      return false;
    }
  }
  bool valid = true;
  for (size_t index = 0; valid && index < next.stage_count; index += 1) {
    uint64_t kind;
    uint64_t flags;
    if (
      !source_read_unsigned(data, length, &position, &kind) ||
      !source_read_unsigned(data, length, &position, &flags) ||
      kind > SOURCE_HERE_DOCUMENT ||
      flags > SOURCE_STAGE_FLAG_MASK ||
      (kind != SOURCE_HERE_DOCUMENT &&
        (flags & SOURCE_STAGE_FLAG_HERE_DOCUMENT_ONLY) != 0) ||
      ((flags & SOURCE_STAGE_FLAG_PHYSICAL_TAIL) != 0 &&
        (flags & SOURCE_STAGE_FLAG_HAS_PENDING) == 0)
    ) {
      valid = false;
      break;
    }
    struct SourceStage *stage = &next.stages[index];
    stage->kind = (enum SourceStageKind)kind;
    stage->disabled = (flags & SOURCE_STAGE_FLAG_DISABLED) != 0;
    stage->quoted = (flags & SOURCE_STAGE_FLAG_QUOTED) != 0;
    stage->at_line_start = (flags & SOURCE_STAGE_FLAG_AT_LINE_START) != 0;
    stage->has_pending = (flags & SOURCE_STAGE_FLAG_HAS_PENDING) != 0;
    stage->strip_tabs = (flags & SOURCE_STAGE_FLAG_STRIP_TABS) != 0;
    stage->tab_prefix = (flags & SOURCE_STAGE_FLAG_TAB_PREFIX) != 0;
    stage->pending.physical_tail =
      (flags & SOURCE_STAGE_FLAG_PHYSICAL_TAIL) != 0;
    if (stage->has_pending) {
      int64_t *positions[] = {
        &stage->pending.origin.start,
        &stage->pending.origin.end,
        &stage->pending.origin.character
      };
      for (size_t field = 0; valid && field < 3; field += 1) {
        uint64_t distance;
        if (
          !source_read_unsigned(data, length, &position, &distance) ||
          distance > INT64_MAX
        ) {
          valid = false;
        } else {
          *positions[field] = -(int64_t)distance;
        }
      }
      stage->pending.value = '\\';
      stage->pending.boundary_stage = SIZE_MAX;
      if (!source_pending_is_valid(stage, next.ended)) {
        valid = false;
      }
    }
  }
  if (!valid || position != length) {
    source_snapshot_clear(&next);
    return false;
  }
  source_snapshot_clear(snapshot);
  *snapshot = next;
  return true;
}

#endif
