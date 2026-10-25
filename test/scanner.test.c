#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/scanner.c"

#define ARRAY_LENGTH(array) (sizeof(array) / sizeof((array)[0]))

static const size_t two_byte_varint_minimum = 128;
static const size_t three_byte_varint_minimum = 16384;
static const size_t initial_delimiter_bytes =
  TREE_SITTER_SERIALIZATION_BUFFER_SIZE / 2;
static const size_t repeated_context_count = 2000;
static const unsigned char state_guard_byte = 0x5a;

#ifdef TREE_SITTER_REUSE_ALLOCATOR
static size_t reuse_malloc_calls;
static size_t reuse_calloc_calls;
static size_t reuse_realloc_calls;
static size_t reuse_free_calls;
static size_t reuse_live_allocations;
static size_t reuse_allocation_calls;
static size_t reuse_fail_allocation_call;
static bool reuse_fail_next_calloc;
static bool reuse_fail_next_realloc;

static bool reuse_allocation_fails(void) {
  reuse_allocation_calls += 1;
  return reuse_fail_allocation_call != 0 &&
    reuse_allocation_calls == reuse_fail_allocation_call;
}

static void *reuse_malloc(size_t size) {
  reuse_malloc_calls += 1;
  if (reuse_allocation_fails()) {
    return NULL;
  }
  void *result = malloc(size);
  if (result != NULL) {
    reuse_live_allocations += 1;
  }
  return result;
}

static void *reuse_calloc(size_t count, size_t size) {
  reuse_calloc_calls += 1;
  if (reuse_allocation_fails() || reuse_fail_next_calloc) {
    reuse_fail_next_calloc = false;
    return NULL;
  }

  void *result = calloc(count, size);
  if (result != NULL) {
    reuse_live_allocations += 1;
  }
  return result;
}

static void *reuse_realloc(void *allocation, size_t size) {
  reuse_realloc_calls += 1;
  if (reuse_allocation_fails() || reuse_fail_next_realloc) {
    reuse_fail_next_realloc = false;
    return NULL;
  }

  if (allocation == NULL) {
    void *result = malloc(size);
    if (result != NULL) {
      reuse_live_allocations += 1;
    }
    return result;
  }
  if (size == 0) {
    free(allocation);
    assert(reuse_live_allocations > 0);
    reuse_live_allocations -= 1;
    return NULL;
  }
  return realloc(allocation, size);
}

static void reuse_free(void *allocation) {
  reuse_free_calls += 1;
  if (allocation != NULL) {
    assert(reuse_live_allocations > 0);
    reuse_live_allocations -= 1;
  }
  free(allocation);
}

void *(*ts_current_malloc)(size_t) = reuse_malloc;
void *(*ts_current_calloc)(size_t, size_t) = reuse_calloc;
void *(*ts_current_realloc)(void *, size_t) = reuse_realloc;
void (*ts_current_free)(void *) = reuse_free;
#endif

struct SourceTestInput {
  const int32_t *characters;
  size_t length;
  size_t position;
  size_t calls;
  bool fail;
};

static enum SourceResult source_test_read(void *payload, int32_t *value) {
  struct SourceTestInput *input = payload;
  input->calls += 1;
  if (input->fail) {
    return SOURCE_FAILURE;
  }
  if (input->position == input->length) {
    return SOURCE_END;
  }
  *value = input->characters[input->position++];
  return SOURCE_CHARACTER;
}

static void source_expect(
  struct SourceCursor *cursor,
  const int32_t *expected,
  size_t length
) {
  for (size_t index = 0; index < length; index += 1) {
    struct SourceCharacter character;
    assert(source_next(cursor, &character) == SOURCE_CHARACTER);
    assert(character.value == expected[index]);
  }
  struct SourceCharacter character;
  assert(source_next(cursor, &character) == SOURCE_END);
}

static void test_source_reading_omits_nul_and_preserves_scalar_origins(void) {
  const int32_t input[] = {'a', 0, 0x1f642, '\\', '\n', 'b'};
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  struct SourceCursor cursor;
  assert(source_cursor_init(&cursor, &stage, 1, source_test_read, &reader));
  const int32_t expected[] = {'a', 0x1f642, 'b'};
  source_expect(&cursor, expected, ARRAY_LENGTH(expected));
  assert(cursor.logical[1].origin.start == 2);
  assert(cursor.logical[1].origin.end == 3);
  assert(cursor.logical[2].origin.start == 5);
  assert(cursor.removal_count == 1);
  assert(cursor.removals[0].slash == 3);
  assert(cursor.removals[0].origin.start == 4);
  assert(cursor.removals[0].origin.end == 5);
  source_cursor_clear(&cursor);

  reader = (struct SourceTestInput){
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  stage.disabled = true;
  assert(source_cursor_init(&cursor, &stage, 1, source_test_read, &reader));
  const int32_t quoted[] = {'a', 0x1f642, '\\', '\n', 'b'};
  source_expect(&cursor, quoted, ARRAY_LENGTH(quoted));
  assert(cursor.removal_count == 0);
  source_cursor_clear(&cursor);
}

static void test_ancestor_removal_survives_local_quoting(void) {
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
  };
  stages[1].disabled = true;
  const int32_t input[] = {'x', '\\', '\n', 'y'};
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  struct SourceCursor cursor;
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  const int32_t expected[] = {'x', 'y'};
  source_expect(&cursor, expected, ARRAY_LENGTH(expected));
  assert(cursor.removal_count == 1);
  assert(cursor.removals[0].stage == 0);
  assert(cursor.removals[0].slash == 1);
  source_cursor_clear(&cursor);
}

static void test_disabled_here_document_stage_only_tracks_line_starts(void) {
  struct SourceStage stage = source_stage(SOURCE_HERE_DOCUMENT);
  stage.strip_tabs = true;
  stage.disabled = true;
  const int32_t input[] = {'\t', 'a', '\\', '\n', '\t', 'b', '\n'};
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  struct SourceCursor cursor;
  assert(source_cursor_init(&cursor, &stage, 1, source_test_read, &reader));
  source_expect(&cursor, input, ARRAY_LENGTH(input));
  assert(cursor.removal_count == 0);
  assert(cursor.stages[0].at_line_start);
  source_cursor_clear(&cursor);
}

static void test_backquote_source_records_units_and_boundaries(void) {
  const int32_t input[] = {'\\', '\\', '\\', '$', '\\', '`', '`'};
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  struct SourceStage stage = source_stage(SOURCE_BACKQUOTE_DECODE);
  struct SourceCursor cursor;
  assert(source_cursor_init(&cursor, &stage, 1, source_test_read, &reader));
  const int32_t expected[] = {'\\', '$', '`', '`'};
  source_expect(&cursor, expected, ARRAY_LENGTH(expected));
  assert(cursor.logical[0].origin.start == 0);
  assert(cursor.logical[0].origin.end == 2);
  assert(cursor.logical[0].origin.character == 1);
  assert(cursor.logical[1].origin.start == 2);
  assert(cursor.logical[1].origin.character == 3);
  assert(cursor.logical[2].boundary_stage == SIZE_MAX);
  assert(cursor.logical[3].boundary_stage == 0);
  source_cursor_clear(&cursor);

  struct SourceStage stages[] = {stage, stage};
  const int32_t nested[] = {'\\', '`'};
  reader = (struct SourceTestInput){
    .characters = nested,
    .length = ARRAY_LENGTH(nested)
  };
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  const int32_t tick[] = {'`'};
  source_expect(&cursor, tick, ARRAY_LENGTH(tick));
  assert(cursor.logical[0].boundary_stage == 1);
  assert(cursor.logical[0].origin.start == 0);
  assert(cursor.logical[0].origin.character == 1);
  source_cursor_clear(&cursor);

  const int32_t nested_opener[] = {'\\', '\\', '\\', '`'};
  reader = (struct SourceTestInput){
    .characters = nested_opener,
    .length = ARRAY_LENGTH(nested_opener)
  };
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  source_expect(&cursor, tick, ARRAY_LENGTH(tick));
  assert(cursor.logical[0].boundary_stage == SIZE_MAX);
  assert(cursor.logical[0].origin.start == 0);
  assert(cursor.logical[0].origin.end == 4);
  assert(cursor.logical[0].origin.character == 3);
  source_cursor_clear(&cursor);
}

static void test_source_pass_order_controls_removal(void) {
  const int32_t input[] = {'\\', '\\', '\n'};
  const int32_t literal[] = {'\\', '\n'};
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
  };
  struct SourceCursor cursor;
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  source_expect(&cursor, literal, ARRAY_LENGTH(literal));
  assert(cursor.removal_count == 0);
  source_cursor_clear(&cursor);

  stages[0] = source_stage(SOURCE_BACKQUOTE_DECODE);
  stages[1] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  reader = (struct SourceTestInput){
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  source_expect(&cursor, NULL, 0);
  assert(cursor.removal_count == 1);
  assert(cursor.removals[0].slash == 1);
  source_cursor_clear(&cursor);
}

static void test_adjacent_runs_and_bare_newlines_follow_each_pass(void) {
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
  };
  stages[1].quoted = true;
  stages[3].quoted = true;
  const int32_t retained[] = {'\\', '\\', '\\', '\n', '\\', '\\', '\n', 'w'};
  const int32_t continued[] = {
    '\\',
    '\\',
    '\\',
    '\n',
    '\\',
    '\\',
    '\\',
    '\\',
    '\n',
    'w',
  };
  const int32_t bare[] = {'\\', '\\', '\\', '\n', '\n', 'w'};
  const struct {
    const int32_t *input;
    size_t length;
    int32_t output[3];
    size_t output_length;
    size_t removal_count;
    int64_t second_slash;
  } cases[] = {
    {retained, ARRAY_LENGTH(retained), {'\\', '\n', 'w'}, 3, 1, 0},
    {continued, ARRAY_LENGTH(continued), {'\\', 'w'}, 2, 2, 7},
    {bare, ARRAY_LENGTH(bare), {'w'}, 1, 2, INT64_MIN},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct SourceTestInput reader =
      {cases[index].input, cases[index].length, 0, 0, false};
    struct SourceCursor cursor;
    assert(source_cursor_init(
      &cursor,
      stages,
      ARRAY_LENGTH(stages),
      source_test_read,
      &reader
    ));
    source_expect(&cursor, cases[index].output, cases[index].output_length);
    assert(cursor.removal_count == cases[index].removal_count);
    assert(cursor.removals[0].slash == 2);
    if (cursor.removal_count == 2) {
      assert(cursor.removals[1].slash == cases[index].second_slash);
      assert(cursor.removals[1].stage == 2);
    }
    source_cursor_clear(&cursor);
  }
}

static void test_tab_stripping_follows_joined_line_starts(void) {
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_STRIP_LEADING_TABS),
  };
  const int32_t input[] = {'\t', '\\', '\n', '\t', 'I', 'N', '\n'};
  const int32_t expected[] = {'I', 'N', '\n'};
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input)
  };
  struct SourceCursor cursor;
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  source_expect(&cursor, expected, ARRAY_LENGTH(expected));
  assert(cursor.removal_count == 3);
  assert(cursor.removals[0].kind == SOURCE_REMOVED_TAB);
  assert(cursor.removals[1].kind == SOURCE_REMOVED_CONTINUATION);
  assert(cursor.removals[2].kind == SOURCE_REMOVED_TAB);
  source_cursor_clear(&cursor);

  const int32_t spaced[] = {' ', '\\', '\n', '\t', 'I', 'N', '\n'};
  const int32_t spaced_expected[] = {' ', '\t', 'I', 'N', '\n'};
  reader = (struct SourceTestInput){
    .characters = spaced,
    .length = ARRAY_LENGTH(spaced)
  };
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  source_expect(&cursor, spaced_expected, ARRAY_LENGTH(spaced_expected));
  assert(cursor.removal_count == 1);
  source_cursor_clear(&cursor);
}

static void assert_source_resumes_at_every_raw_cut(
  const int32_t *input,
  size_t input_length,
  const struct SourceStage *stages,
  size_t stage_count
) {
  struct SourceTestInput reader = {.characters = input, .length = input_length};
  struct SourceCursor cursor;
  assert(
    source_cursor_init(&cursor, stages, stage_count, source_test_read, &reader)
  );
  struct SourceCharacter character;
  while (source_next(&cursor, &character) == SOURCE_CHARACTER) {
  }
  assert(!cursor.failed);
  size_t reads = reader.calls;
  for (size_t cut = 0; cut <= input_length; cut += 1) {
    struct SourceSnapshot snapshot = {0};
    assert(source_snapshot_at(&cursor, cut, &snapshot));
    assert(reader.calls == reads);
    assert(snapshot.raw_position == (int64_t)cut);
    uint8_t encoded[SOURCE_STATE_CAPACITY];
    size_t length;
    assert(
      source_snapshot_serialize(&snapshot, encoded, sizeof(encoded), &length)
    );
    struct SourceSnapshot restored = {0};
    assert(source_snapshot_deserialize(&restored, encoded, length));
    struct SourceTestInput suffix = {
      .characters = input + cut,
      .length = input_length - cut
    };
    struct SourceCursor resumed;
    assert(
      source_cursor_resume(&resumed, &restored, source_test_read, &suffix)
    );
    size_t index = snapshot.logical_count;
    while (source_next(&resumed, &character) == SOURCE_CHARACTER) {
      assert(index < cursor.logical_count);
      const struct SourceCharacter *expected = &cursor.logical[index++];
      assert(character.value == expected->value);
      assert(character.origin.start + (int64_t)cut == expected->origin.start);
      assert(character.origin.end + (int64_t)cut == expected->origin.end);
      assert(
        character.origin.character + (int64_t)cut == expected->origin.character
      );
      assert(character.boundary_stage == expected->boundary_stage);
    }
    assert(!resumed.failed);
    assert(index == cursor.logical_count);
    assert(
      snapshot.removal_count + resumed.removal_count == cursor.removal_count
    );
    for (size_t event = 0; event < resumed.removal_count; event += 1) {
      const struct SourceRemoval *actual = &resumed.removals[event];
      const struct SourceRemoval *expected =
        &cursor.removals[snapshot.removal_count + event];
      assert(actual->kind == expected->kind);
      assert(actual->origin.start + (int64_t)cut == expected->origin.start);
      assert(
        actual->slash == INT64_MIN
          ? expected->slash == INT64_MIN
          : actual->slash + (int64_t)cut == expected->slash
      );
    }
    source_cursor_clear(&resumed);
    source_snapshot_clear(&restored);
    source_snapshot_clear(&snapshot);
  }
  source_cursor_clear(&cursor);
}

static void test_every_raw_cut_restores_pending_source_without_rereading(void) {
  const int32_t input[] =
    {'a', '\\', '\\', '\\', '\n', '\\', '\\', '\n', 0x1f642, '\\'};
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
  };
  assert_source_resumes_at_every_raw_cut(
    input,
    ARRAY_LENGTH(input),
    stages,
    ARRAY_LENGTH(stages)
  );
  const int32_t document[] = {
    0,
    '\t',
    0,
    '\\',
    0,
    '\n',
    'E',
    '\n',
    'a',
    '\\',
    0,
    '\n',
    '\t',
    'b',
    '\n',
    '\\',
    0,
    '\n',
    '\t',
    'E',
    '\n'
  };
  for (unsigned quoted = 0; quoted < 2; quoted += 1) {
    for (unsigned tabs = 0; tabs < 2; tabs += 1) {
      struct SourceStage stage = source_stage(SOURCE_HERE_DOCUMENT);
      stage.quoted = quoted != 0;
      stage.strip_tabs = tabs != 0;
      assert_source_resumes_at_every_raw_cut(
        document,
        ARRAY_LENGTH(document),
        &stage,
        1
      );
    }
  }
}

static void test_snapshot_capacity_and_invalid_data_are_transactional(void) {
  struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  struct SourceCursor cursor;
  assert(source_cursor_init(&cursor, &stage, 1, NULL, NULL));
  assert(source_feed(&cursor, '\\'));
  struct SourceSnapshot snapshot = {0};
  assert(source_checkpoint(&cursor, &snapshot));
  uint8_t encoded[SOURCE_STATE_CAPACITY];
  size_t length;
  assert(
    source_snapshot_serialize(&snapshot, encoded, sizeof(encoded), &length)
  );
  size_t unchanged = length;
  uint8_t shortened[SOURCE_STATE_CAPACITY];
  assert(!source_snapshot_serialize(&snapshot, shortened, length - 1, &length));
  assert(length == 0);
  assert(snapshot.stages[0].has_pending);
  for (size_t prefix = 0; prefix < unchanged; prefix += 1) {
    assert(!source_snapshot_deserialize(&snapshot, encoded, prefix));
    assert(snapshot.stages[0].has_pending);
  }
  const uint8_t invalid_stage_kind = 9;
  const uint8_t invalid[] = {1, 0, invalid_stage_kind, 0};
  assert(!source_snapshot_deserialize(&snapshot, invalid, sizeof(invalid)));
  assert(snapshot.stages[0].has_pending);
  source_snapshot_clear(&snapshot);
  source_cursor_clear(&cursor);
}

static void test_source_reader_failure_releases_owned_memory(void) {
  const int32_t input[] = {'a', '\\', '\n', 'b'};
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
  };
  struct SourceTestInput reader = {
    .characters = input,
    .length = ARRAY_LENGTH(input),
    .fail = true,
  };
  struct SourceCursor cursor;
  assert(source_cursor_init(
    &cursor,
    stages,
    ARRAY_LENGTH(stages),
    source_test_read,
    &reader
  ));
  struct SourceCharacter character;
  assert(source_next(&cursor, &character) == SOURCE_FAILURE);
  source_cursor_clear(&cursor);
}

#ifdef TREE_SITTER_REUSE_ALLOCATOR
static void test_source_failures_release_owned_memory(void) {
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
  };
  const int32_t input[] = {'a', '\\', '\n', 'b'};
  size_t allocation_count = 0;
  for (size_t failure = 0; failure <= allocation_count; failure += 1) {
    assert(reuse_live_allocations == 0);
    size_t calls = reuse_allocation_calls;
    if (failure > 0) reuse_fail_allocation_call = calls + failure;
    struct SourceTestInput reader = {
      .characters = input,
      .length = ARRAY_LENGTH(input)
    };
    struct SourceCursor cursor;
    bool initialized = source_cursor_init(
      &cursor,
      stages,
      ARRAY_LENGTH(stages),
      source_test_read,
      &reader
    );
    bool completed = false;
    bool saved = false;
    if (initialized) {
      struct SourceCharacter character;
      enum SourceResult result;
      do {
        result = source_next(&cursor, &character);
      } while (result == SOURCE_CHARACTER);
      completed = result == SOURCE_END;
      struct SourceSnapshot snapshot = {0};
      saved = source_checkpoint(&cursor, &snapshot);
      source_snapshot_clear(&snapshot);
    }
    if (failure == 0) {
      allocation_count = reuse_allocation_calls - calls;
      assert(allocation_count > 0);
      assert(initialized && completed && saved);
    } else {
      assert(reuse_allocation_calls >= reuse_fail_allocation_call);
      assert(!initialized || !completed || !saved);
      reuse_fail_allocation_call = 0;
    }
    source_cursor_clear(&cursor);
    assert(reuse_live_allocations == 0);
  }
}
#endif

struct SourceNativeMock {
  TSLexer lexer;
  const int32_t *input;
  size_t length;
  size_t position;
  size_t advances;
  size_t marks;
};

static void source_native_advance(TSLexer *lexer, bool skip) {
  struct SourceNativeMock *mock = (struct SourceNativeMock *)lexer;
  assert(!skip);
  assert(mock->position < mock->length);
  mock->position += 1;
  mock->advances += 1;
  lexer->lookahead =
    mock->position < mock->length ? mock->input[mock->position] : 0;
}

static void source_native_mark(TSLexer *lexer) {
  struct SourceNativeMock *mock = (struct SourceNativeMock *)lexer;
  mock->marks += 1;
}

static bool source_native_eof(const TSLexer *lexer) {
  const struct SourceNativeMock *mock = (const struct SourceNativeMock *)lexer;
  return mock->position == mock->length;
}

static void source_native_init(
  struct SourceNativeMock *mock,
  const int32_t *input,
  size_t length
) {
  *mock = (struct SourceNativeMock){
    .lexer =
      {
        .lookahead = length > 0 ? input[0] : 0,
        .advance = source_native_advance,
        .mark_end = source_native_mark,
        .eof = source_native_eof,
      },
    .input = input,
    .length = length,
  };
}

static void test_logical_context_changes_reclassify_cached_lookahead(void) {
  const int32_t input[] = {'\'', '\\', '\n', 'x', '\'', '\\', '\n', 'y'};
  struct SourceNativeMock mock;
  source_native_init(&mock, input, ARRAY_LENGTH(input));
  struct NativeInput native = {.lexer = &mock.lexer};
  struct SourceStage local = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  struct SourceSnapshot initial = {.stages = &local, .stage_count = 1};
  struct LogicalLexer logical;
  assert(logical_init(&logical, &native, &initial));
  assert(logical.lexer.lookahead == '\'');
  logical.lexer.mark_end(&logical.lexer);
  logical.lexer.advance(&logical.lexer, false);
  assert(logical.lexer.lookahead == 'x');
  assert(logical.consumed == 1);
  assert(mock.position == 4);

  struct SourceStage quoted = local;
  quoted.disabled = true;
  assert(logical_replace_view(&logical, 0, &quoted, 1));
  assert(logical.lexer.lookahead == '\\');
  assert(logical.base == 1);
  assert(logical.mark == 0);
  assert(mock.position == 4);
  logical.lexer.advance(&logical.lexer, false);
  assert(logical.lexer.lookahead == '\n');
  logical.lexer.advance(&logical.lexer, false);
  assert(logical.lexer.lookahead == 'x');
  logical.lexer.advance(&logical.lexer, false);
  assert(logical.lexer.lookahead == '\'');
  logical.lexer.advance(&logical.lexer, false);
  assert(logical.lexer.lookahead == '\\');
  assert(logical.consumed == 5);

  assert(logical_replace_view(&logical, 0, &local, 1));
  assert(logical.lexer.lookahead == 'y');
  assert(logical.base == 5);
  assert(logical.cursor.removal_count == 1);
  assert(logical.cursor.removals[0].slash == 0);
  logical.lexer.advance(&logical.lexer, false);
  assert(logical.lexer.eof(&logical.lexer));
  assert(mock.advances == 8);
  assert(mock.marks == 0);
  logical_clear(&logical);
  native_input_clear(&native);
}

static void
test_logical_backquote_closers_belong_to_the_innermost_active_view(void) {
  const struct {
    int32_t input[4];
    size_t length;
    size_t decoders;
    bool disable_inner;
    size_t boundary;
    bool closes;
  } fixtures[] = {
    {{'`'}, 1, 1, false, 0, true},
    {{'`'}, 1, 2, false, 0, false},
    {{'\\', '`'}, 2, 2, false, 2, true},
    {{'\\', '\\', '\\', '`'}, 4, 2, false, SIZE_MAX, false},
    {{'\\', '`'}, 2, 3, false, 2, false},
    {{'\\', '\\', '\\', '`'}, 4, 3, false, 4, true},
    {{'`'}, 1, 2, true, 0, true},
    {{'\\', '`'}, 2, 2, true, SIZE_MAX, false},
  };
  assert(!logical_backquote_end(NULL));
  for (size_t fixture = 0; fixture < ARRAY_LENGTH(fixtures); fixture += 1) {
    enum { maximum_decoders = 3, stages_per_decoder = 2 };
    struct SourceStage stages[maximum_decoders * stages_per_decoder];
    size_t count = stages_per_decoder * fixtures[fixture].decoders;
    for (size_t index = 0; index < count; index += 2) {
      stages[index] = source_stage(SOURCE_BACKQUOTE_DECODE);
      stages[index + 1] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    }
    stages[count - 2].disabled = fixtures[fixture].disable_inner;
    struct SourceSnapshot initial = {.stages = stages, .stage_count = count};
    struct SourceNativeMock mock;
    source_native_init(
      &mock,
      fixtures[fixture].input,
      fixtures[fixture].length
    );
    struct NativeInput native = {.lexer = &mock.lexer};
    struct LogicalLexer logical;
    assert(logical_init(&logical, &native, &initial));
    assert(logical.character.value == '`');
    assert(logical.character.boundary_stage == fixtures[fixture].boundary);
    assert(logical_backquote_end(&logical) == fixtures[fixture].closes);
    assert(logical.character.origin.start == 0);
    assert(logical.character.origin.end == (int64_t)fixtures[fixture].length);
    for (size_t cut = 0; cut < fixtures[fixture].length; cut += 1) {
      struct SourceSnapshot snapshot = {0};
      assert(source_snapshot_at(&logical.cursor, cut, &snapshot));
      uint8_t bytes[SOURCE_STATE_CAPACITY];
      size_t length;
      assert(
        source_snapshot_serialize(&snapshot, bytes, sizeof(bytes), &length)
      );
      struct SourceSnapshot restored = {0};
      assert(source_snapshot_deserialize(&restored, bytes, length));
      struct SourceNativeMock resumed_mock;
      source_native_init(
        &resumed_mock,
        fixtures[fixture].input + cut,
        fixtures[fixture].length - cut
      );
      struct NativeInput resumed_native = {.lexer = &resumed_mock.lexer};
      struct LogicalLexer resumed;
      assert(logical_init(&resumed, &resumed_native, &restored));
      assert(resumed.character.boundary_stage == fixtures[fixture].boundary);
      assert(logical_backquote_end(&resumed) == fixtures[fixture].closes);
      assert(resumed.character.origin.start == -(int64_t)cut);
      assert(
        resumed.character.origin.end ==
        (int64_t)(fixtures[fixture].length - cut)
      );
      resumed.lexer.advance(&resumed.lexer, false);
      assert(resumed.lexer.eof(&resumed.lexer));
      assert(!logical_backquote_end(&resumed));
      assert(resumed_mock.advances == fixtures[fixture].length - cut);
      assert(resumed_mock.marks == 0);
      logical_clear(&resumed);
      native_input_clear(&resumed_native);
      source_snapshot_clear(&restored);
      source_snapshot_clear(&snapshot);
    }
    if (fixture == 1) {
      struct LogicalLexer outer;
      assert(logical_fork(&outer, &logical, 0, 2, NULL, 0));
      assert(logical_backquote_end(&outer));
      assert(!logical_backquote_end(&logical));
      logical_clear(&outer);
    }
    logical_clear(&logical);
    native_input_clear(&native);
  }
}

static void test_logical_forks_preserve_ancestor_pending_units(void) {
  const int32_t input[] = {'\\', '\n', 'x'};
  struct SourceNativeMock mock;
  source_native_init(&mock, input, ARRAY_LENGTH(input));
  struct NativeInput native = {.lexer = &mock.lexer};
  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
  };
  struct SourceSnapshot initial = {
    .stages = stages,
    .stage_count = ARRAY_LENGTH(stages)
  };
  struct LogicalLexer parent;
  assert(logical_init(&parent, &native, &initial));
  assert(parent.lexer.lookahead == 'x');
  struct SourceStage quoted = stages[1];
  quoted.disabled = true;
  struct LogicalLexer child;
  assert(logical_fork(&child, &parent, 0, 1, &quoted, 1));
  assert(child.lexer.lookahead == 'x');
  assert(child.cursor.removal_count == 1);
  assert(child.cursor.removals[0].stage == 0);
  assert(parent.base == 0);
  assert(parent.lexer.lookahead == 'x');
  logical_clear(&child);

  assert(logical_fork(&child, &parent, 1, 2, NULL, 0));
  assert(child.lexer.lookahead == 'x');
  assert(child.base == 1);
  assert(child.cursor.removals[0].slash == -1);
  assert(child.cursor.removals[0].origin.start == 0);
  logical_clear(&child);
  assert(!logical_fork(&child, &parent, 1, 0, &quoted, 1));
  logical_clear(&child);
  logical_rewind(&parent);
  assert(parent.lexer.lookahead == 'x');
  assert(mock.advances == 3);
  assert(mock.marks == 0);
  logical_clear(&parent);
  native_input_clear(&native);
}

#ifdef TREE_SITTER_REUSE_ALLOCATOR
static void test_logical_fork_allocation_failures_preserve_parent(void) {
  const int32_t input[] = {'a', '\\', '\n', 'b'};
  size_t allocation_count = 0;
  for (size_t failure = 0; failure <= allocation_count; failure += 1) {
    struct SourceNativeMock mock;
    source_native_init(&mock, input, ARRAY_LENGTH(input));
    struct NativeInput native = {.lexer = &mock.lexer};
    struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    struct SourceSnapshot initial = {.stages = &stage, .stage_count = 1};
    struct LogicalLexer parent;
    assert(logical_init(&parent, &native, &initial));
    assert(parent.lexer.lookahead == 'a');
    size_t calls = reuse_allocation_calls;
    if (failure > 0) reuse_fail_allocation_call = calls + failure;
    struct LogicalLexer child;
    bool accepted = logical_fork(&child, &parent, 0, 0, &stage, 1);
    if (failure == 0) {
      allocation_count = reuse_allocation_calls - calls;
      assert(allocation_count > 0);
      assert(accepted);
    } else {
      assert(reuse_allocation_calls >= reuse_fail_allocation_call);
      assert(!accepted);
      reuse_fail_allocation_call = 0;
    }
    assert(parent.lexer.lookahead == 'a');
    assert(parent.base == 0);
    logical_clear(&child);
    logical_clear(&parent);
    native_input_clear(&native);
    assert(reuse_live_allocations == 0);
  }
}

#endif

static void test_logical_resume_does_not_repeat_already_emitted_source(void) {
  const int32_t prefix[] = {'\\', '\\', '\\'};
  struct SourceStage stages[] = {
    source_stage(SOURCE_BACKQUOTE_DECODE),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
  };
  struct SourceCursor cursor;
  assert(source_cursor_init(&cursor, stages, ARRAY_LENGTH(stages), NULL, NULL));
  for (size_t index = 0; index < ARRAY_LENGTH(prefix); index += 1) {
    assert(source_feed(&cursor, prefix[index]));
  }
  struct SourceSnapshot snapshot = {0};
  assert(source_checkpoint(&cursor, &snapshot));
  const int32_t suffix[] = {']'};
  struct SourceNativeMock mock;
  source_native_init(&mock, suffix, ARRAY_LENGTH(suffix));
  struct NativeInput native = {.lexer = &mock.lexer};
  struct LogicalLexer input;
  assert(logical_init(&input, &native, &snapshot));
  assert(input.lexer.lookahead == ']');
  assert(input.character.origin.start == 0);
  assert(input.character.origin.end == 1);
  input.lexer.advance(&input.lexer, false);
  assert(input.lexer.eof(&input.lexer));
  assert(mock.advances == 1);
  logical_clear(&input);
  native_input_clear(&native);
  source_snapshot_clear(&snapshot);
  source_cursor_clear(&cursor);
}

struct MockLexer {
  TSLexer lexer;
  const int32_t *input;
  size_t length;
  size_t offset;
  size_t mark;
  uint32_t column;
};

static void mock_advance(TSLexer *lexer, bool skip) {
  (void)skip;
  struct MockLexer *mock = (struct MockLexer *)lexer;
  if (mock->offset >= mock->length) {
    return;
  }

  if (mock->input[mock->offset] == '\n') {
    mock->column = 0;
  } else {
    mock->column += 1;
  }
  mock->offset += 1;
  lexer->lookahead =
    mock->offset < mock->length ? mock->input[mock->offset] : 0;
}

static void mock_mark_end(TSLexer *lexer) {
  struct MockLexer *mock = (struct MockLexer *)lexer;
  mock->mark = mock->offset;
}

static uint32_t mock_get_column(TSLexer *lexer) {
  return ((struct MockLexer *)lexer)->column;
}

static bool mock_eof(const TSLexer *lexer) {
  const struct MockLexer *mock = (const struct MockLexer *)lexer;
  return mock->offset >= mock->length;
}

static void
init_mock_lexer(struct MockLexer *mock, const int32_t *input, size_t length) {
  *mock = (struct MockLexer){
    .lexer =
      {
        .lookahead = length == 0 ? 0 : input[0],
        .result_symbol = UINT16_MAX,
        .advance = mock_advance,
        .mark_end = mock_mark_end,
        .get_column = mock_get_column,
        .eof = mock_eof,
      },
    .input = input,
    .length = length,
  };
}

static void test_lookahead_replays_raw_cuts_in_distinct_quote_views(void) {
  const int32_t source[] = {'\'', '\\', '\n', 'x', '\'', '\\', '\n', 'y'};
  struct MockLexer mock;
  init_mock_lexer(&mock, source, ARRAY_LENGTH(source));
  struct NativeInput native = {.lexer = &mock.lexer};
  struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  struct SourceSnapshot initial = {.stages = &stage, .stage_count = 1};
  struct LogicalLexer input;
  assert(logical_init(&input, &native, &initial));
  struct LookaheadLexer lookahead;
  assert(lookahead_init(&lookahead, &input.lexer));
  TSLexer *lexer = &lookahead.lexer;
  lexer->advance(lexer, false);
  assert(lexer->lookahead == 'x');
  size_t parent = lookahead.position;
  assert(lookahead_local_policy(lexer, true));
  size_t quoted = lookahead.position;
  const int32_t quoted_source[] = {'\\', '\n', 'x', '\''};
  for (size_t index = 0; index < 4; index += 1) {
    assert(lexer->lookahead == quoted_source[index]);
    lexer->advance(lexer, false);
  }
  assert(lexer->lookahead == '\\');
  assert(lookahead_local_policy(lexer, false));
  assert(lexer->lookahead == 'y');
  lexer->mark_end(lexer);
  assert(input.mark == 5);
  lexer->advance(lexer, false);
  assert(lexer->eof(lexer));
  size_t views = lookahead.view_count;
  lookahead_seek(&lookahead, parent);
  assert(lexer->lookahead == 'x');
  assert(lookahead_local_policy(lexer, true));
  assert(lookahead.position == quoted);
  assert(lookahead.view_count == views);
  assert(lexer->lookahead == '\\');
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  assert(input.mark == 2);
  assert(mock.offset == 8);
  assert(mock.mark == 0);
  lookahead_clear(&lookahead);
  logical_clear(&input);
  native_input_clear(&native);
}

struct SourceFixture {
  int32_t *characters;
  struct MockLexer mock;
  struct NativeInput native;
  struct LogicalLexer input;
  struct LookaheadLexer lookahead;
};

static void init_source_fixture(
  struct SourceFixture *fixture,
  const char *source,
  struct SourceStage *stages,
  size_t count
) {
  *fixture = (struct SourceFixture){0};
  size_t length = strlen(source);
  fixture->characters = ts_malloc(length * sizeof(*fixture->characters));
  assert(fixture->characters != NULL);
  for (size_t index = 0; index < length; index += 1) {
    fixture->characters[index] = (unsigned char)source[index];
  }
  init_mock_lexer(&fixture->mock, fixture->characters, length);
  fixture->native.lexer = &fixture->mock.lexer;
  struct SourceSnapshot initial = {.stages = stages, .stage_count = count};
  assert(logical_init(&fixture->input, &fixture->native, &initial));
  assert(lookahead_init(&fixture->lookahead, &fixture->input.lexer));
}

static void clear_source_fixture(struct SourceFixture *fixture) {
  lookahead_clear(&fixture->lookahead);
  logical_clear(&fixture->input);
  native_input_clear(&fixture->native);
  ts_free(fixture->characters);
}

static void test_delimiter_source_views_preserve_quotedness_and_bytes(void) {
  const struct {
    const char *source;
    const char *delimiter;
    bool quoted;
  } fixtures[] = {
    {"E\\\nOF ", "EOF", false},
    {"'E\\\nOF' ", "E\\\nOF", true},
    {"\"E\\\nOF\" ", "EOF", true},
    {"a\\ b ", "a b", true},
    {"${\"} ", "${}", false},
    {"${a'} ", "${a'}", false},
    {"${:} ", "${}", false},
    {"${:\\}} ", "${}}", true},
    {"${'\\}} ", "${}}", true},
    {"${a:'} ", "${a:'}", false},
    {"${#'x} ", "${#'x}", false},
    {"${#\\x} ", "${#\\x}", false},
    {"${#%} ", "${#}", false},
    {"${#:} ", "${#}", false},
    {"${#x''} ", "${#x}", true},
    {"$((\\))) ", "$((\\)))", true},
    {"`'a`b'` ", "`a`b`", true},
    {"`a\\\\b` ", "`a\\b`", true},
    {"`\"x` ", "`x`", false},
    {"`\"x\"` ", "`x`", true},
    {"'p'`\"x` ", "p`x`", true},
    {"`\"x`\"s\" ", "`x`s", true},
    {"`\"\"\"x` ", "`x`", true},
    {"`\"x\\$y` ", "`x$y`", true},
    {"`\"x\\qy` ", "`x\\qy`", true},
    {"`\"x'\"y` ", "`x'y`", true},
    {"`E ${x:-\"` ", "`E ${x:-`", false},
    {"`printf \\`x\\`#tag END` ", "`printf `x`#tag END`", true},
    {"`printf \\\\\nEOF` ", "`printf \\\nEOF`", true},
    {"`\\'a` ", "`\\'a`", true},
    {"`\\a\\$\\\"` ", "`\\a$\"`", true},
    {"$(\\a\\\\\\') ", "$(\\a\\\\')", true},
    {"\"$(\\a)\" ", "$(\\a)", true},
    {"${x:-\\a} ", "${x:-a}", true},
    {"`${x:-\\a}` ", "`${x:-\\a}`", true},
    {"`${x:-${y:-\\a}}` ", "`${x:-${y:-\\a}}`", true},
    {"$(${x:-\\#}\\}) ", "$(${x:-\\#}\\})", true},
    {"$(${x:-\\}}) ", "$(${x:-}})", true},
    {"$(\"${x#\\a}\") ", "$(${x#a})", true},
    {"$(\"${x#${y:-\\a}}\") ", "$(${x#${y:-a}})", true},
    {"$(${x#\"${y:-\\a}\"}) ", "$(${x#${y:-\\a}})", true},
    {"$((\\a)) ", "$((\\a))", true},
    {"$'\\q\\`\\\"\\'\\\\' ", "\\q\\`\"'\\", true},
    {"E$(${x:-)} ", "E$(${x:-)}", false},
    {"E$(${x#)} ", "E$(${x#)}", false},
    {"E`${x:-${y:-` ", "E`${x:-${y:-`", false},
    {"E$(`${x:-)`) ", "E$(`${x:-)`)", false},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    struct SourceFixture fixture;
    init_source_fixture(&fixture, fixtures[index].source, &stage, 1);
    struct HereDocument document = {0};
    assert(
      read_here_document_delimiter(
        &fixture.lookahead.lexer,
        false,
        &document
      ) == DELIMITER_READ_WORD
    );
    size_t length = strlen(fixtures[index].delimiter);
    assert(document.delimiter_length == length);
    assert(memcmp(document.delimiter, fixtures[index].delimiter, length) == 0);
    assert(document.quoted == fixtures[index].quoted);
    assert(fixture.lookahead.lexer.lookahead == ' ');
    clear_document(&document);
    clear_source_fixture(&fixture);
  }
}

static void
test_embedded_readers_apply_source_views_before_comments_and_closers(void) {
  const struct {
    const char *source;
    size_t stages;
    int32_t next;
  } fixtures[] = {
    {"x=$((1 + $((2)))) ) tail", 1, ' '},
    {"x=$((1) 2)) ) tail", 1, ' '},
    {"x=$( (echo one); (echo two)) ) tail", 1, ' '},
    {"x=${a:})} ) tail", 1, '}'},
    {"x=${a:\\\n})} ) tail", 1, '}'},
    {"x=\"${a:}\"}\"}\" ) tail", 1, ' '},
    {"x=${a:-\\})} ) tail", 1, ' '},
    {"x=${a:-\"}\"} ) tail", 1, ' '},
    {"printf '\\\n)'; : ) tail", 1, ' '},
    {": `printf '%s' x` ) tail", 1, ' '},
    {"case x in x) : `case y in esac`;; z) :;; esac)tail", 1, 't'},
    {": #x\\\n)ignored\n) tail", 1, 'i'},
    {": #x\\\n)ignored\n) tail", 2, ' '},
    {"\"`printf #x\\\n) ignored\n`\" ) tail", 1, ' '},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct SourceStage stages[] = {
      source_stage(SOURCE_REMOVE_CONTINUATIONS),
      source_stage(SOURCE_REMOVE_CONTINUATIONS),
    };
    struct SourceFixture fixture;
    init_source_fixture(
      &fixture,
      fixtures[index].source,
      stages,
      fixtures[index].stages
    );
    assert(
      skip_embedded_construct(NULL, &fixture.lookahead.lexer, ')') ==
      ARITHMETIC_VALIDATION_VALID
    );
    assert(fixture.lookahead.lexer.lookahead == fixtures[index].next);
    clear_source_fixture(&fixture);
  }
}

static void test_here_document_readers_stop_at_backquote_boundaries(void) {
  for (unsigned quoted = 0; quoted < 2; quoted += 1) {
    struct SourceStage stages[] = {
      source_stage(SOURCE_REMOVE_CONTINUATIONS),
      source_stage(SOURCE_BACKQUOTE_DECODE),
      source_stage(SOURCE_REMOVE_CONTINUATIONS),
    };
    struct SourceFixture fixture;
    init_source_fixture(&fixture, "`tail", stages, 3);
    struct HereDocument document = {
      .delimiter = (uint8_t *)"END",
      .delimiter_length = 3,
      .quoted = quoted != 0,
    };
    assert(fixture.lookahead.lexer.lookahead == SOURCE_BACKQUOTE_BOUNDARY);
    size_t position = fixture.lookahead.position;
    struct CommandStart start;
    assert(
      read_here_document_line(
        NULL,
        &fixture.lookahead.lexer,
        &document,
        &start
      ) == HERE_DOCUMENT_LINE_END_OF_INPUT
    );
    assert(fixture.lookahead.position == position);
    assert(!fixture.lookahead.failed);
    clear_source_fixture(&fixture);
  }
}

static void test_here_document_readers_keep_the_owning_source_view(void) {
  const struct {
    const char *source;
    const char *delimiter;
    bool quoted;
    bool tabs;
    enum HereDocumentLineKind kind;
    const char *first_word;
  } fixtures[] = {
    {"E\\\nOF\nrest", "EOF", false, false, HERE_DOCUMENT_LINE_CONTENT, ""},
    {"E\\\nOF\nrest", "EOF", true, false, HERE_DOCUMENT_LINE_CONTENT, ""},
    {"\t\\\n\tEOF\nrest", "EOF", false, true, HERE_DOCUMENT_LINE_LAYOUT, NULL},
    {"then\nrest", "END", true, false, HERE_DOCUMENT_LINE_CONTENT, "then"},
    {"a\nb\nrest", "a\nb", true, false, HERE_DOCUMENT_LINE_DELIMITER, NULL},
    {"a\nb\nrest", "a\nb", false, false, HERE_DOCUMENT_LINE_DELIMITER, NULL},
    {"\ta\n\tb\nrest",
      "a\n\tb",
      true,
      true,
      HERE_DOCUMENT_LINE_DELIMITER,
      NULL},
    {"\ta\n\tb\nrest", "a\nb", true, true, HERE_DOCUMENT_LINE_CONTENT, NULL},
    {"a\nX\na\nb\nrest", "a\nb", true, false, HERE_DOCUMENT_LINE_CONTENT, NULL},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    struct SourceFixture fixture;
    init_source_fixture(&fixture, fixtures[index].source, &stage, 1);
    struct HereDocument document = {
      .delimiter = (uint8_t *)fixtures[index].delimiter,
      .delimiter_length = strlen(fixtures[index].delimiter),
      .quoted = fixtures[index].quoted,
      .strip_tabs = fixtures[index].tabs,
    };
    struct CommandStart start = {0};
    assert(
      read_here_document_line(
        NULL,
        &fixture.lookahead.lexer,
        &document,
        &start
      ) == fixtures[index].kind
    );
    if (fixtures[index].first_word != NULL) {
      assert(strcmp(start.first_word, fixtures[index].first_word) == 0);
    }
    clear_source_fixture(&fixture);
  }

  struct SourceStage stages[] = {
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_STRIP_LEADING_TABS),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
    source_stage(SOURCE_BACKQUOTE_DECODE),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
  };
  for (size_t index = 0; index < 4; index += 1) stages[index].disabled = true;
  struct SourceFixture fixture;
  init_source_fixture(&fixture, "\\\\\n world\n", stages, 6);
  struct HereDocument document =
    {.delimiter = (uint8_t *)"END", .delimiter_length = 3, .quoted = true};
  struct SourceContext context = {
    .opener = QUOTED_HERE_DOCUMENT_BODY_START,
    .stage_count = 1,
    .count = 1,
  };
  struct Scanner scanner = {
    .active_frame = {.documents = &document, .count = 1},
    .contexts = &context,
    .context_count = 1
  };
  struct CommandStart start;
  assert(
    read_here_document_line(
      &scanner,
      &fixture.lookahead.lexer,
      &document,
      &start
    ) == HERE_DOCUMENT_LINE_LAYOUT
  );
  assert(start.first_character == 0);
  assert(fixture.lookahead.lexer.lookahead == ' ');
  clear_source_fixture(&fixture);
}

static struct HereDocument make_document_bytes(
  const uint8_t *delimiter,
  size_t length,
  bool quoted,
  bool strip_tabs
) {
  uint8_t *copy = NULL;
  if (length > 0) {
    copy = ts_malloc(length);
    assert(copy != NULL);
    memcpy(copy, delimiter, length);
  }
  return (struct HereDocument){
    .delimiter = copy,
    .delimiter_length = length,
    .quoted = quoted,
    .strip_tabs = strip_tabs,
  };
}

static struct HereDocument
make_document(const char *delimiter, bool quoted, bool strip_tabs) {
  return make_document_bytes(
    (const uint8_t *)delimiter,
    strlen(delimiter),
    quoted,
    strip_tabs
  );
}

static struct HereDocument make_repeated_document(size_t length) {
  uint8_t *delimiter = ts_malloc(length);
  assert(delimiter != NULL);
  memset(delimiter, 'D', length);
  return (struct HereDocument){
    .delimiter = delimiter,
    .delimiter_length = length,
    .quoted = true,
    .strip_tabs = true,
  };
}

static void assert_document(
  const struct HereDocument *document,
  const char *delimiter,
  bool quoted,
  bool strip_tabs
) {
  size_t length = strlen(delimiter);
  assert(document->delimiter_length == length);
  assert(memcmp(document->delimiter, delimiter, length) == 0);
  assert(document->quoted == quoted);
  assert(document->strip_tabs == strip_tabs);
}

static void assert_document_bytes(
  const struct HereDocument *document,
  const uint8_t *delimiter,
  size_t delimiter_length,
  bool quoted
) {
  assert(document->delimiter_length == delimiter_length);
  assert(memcmp(document->delimiter, delimiter, delimiter_length) == 0);
  assert(document->quoted == quoted);
  assert(!document->strip_tabs);
}

static void assert_repeated_document(
  const struct HereDocument *document,
  uint8_t delimiter,
  size_t delimiter_length
) {
  assert(document->delimiter_length == delimiter_length);
  for (size_t index = 0; index < delimiter_length; index += 1) {
    assert(document->delimiter[index] == delimiter);
  }
  assert(document->quoted);
  assert(document->strip_tabs);
}

static unsigned snapshot_scanner(
  const struct Scanner *scanner,
  char buffer[TREE_SITTER_SERIALIZATION_BUFFER_SIZE]
) {
  char guarded[TREE_SITTER_SERIALIZATION_BUFFER_SIZE + 2];
  memset(guarded, state_guard_byte, sizeof(guarded));
  unsigned length =
    tree_sitter_dash_external_scanner_serialize((void *)scanner, guarded + 1);
  assert(length > 0 && length <= TREE_SITTER_SERIALIZATION_BUFFER_SIZE);
  assert(guarded[0] == state_guard_byte);
  for (size_t index = length + 1; index < sizeof(guarded); index++) {
    assert(guarded[index] == state_guard_byte);
  }
  memcpy(buffer, guarded + 1, length);
  return length;
}

static void assert_scanner_matches_snapshot(
  const struct Scanner *scanner,
  const char expected[TREE_SITTER_SERIALIZATION_BUFFER_SIZE],
  unsigned expected_length
) {
  char actual[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned actual_length = snapshot_scanner(scanner, actual);
  assert(actual_length == expected_length);
  assert(memcmp(actual, expected, expected_length) == 0);
}

static size_t
fill_pending_document_to_capacity(struct Scanner *scanner, size_t index) {
  char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned used = snapshot_scanner(scanner, state);
  struct HereDocument *document = &scanner->pending_documents[index];
  size_t length = document->delimiter_length + sizeof(state) - used;
  assert(
    document->delimiter_length >= two_byte_varint_minimum &&
    length < three_byte_varint_minimum
  );
  uint8_t *delimiter = ts_realloc(document->delimiter, length);
  assert(delimiter != NULL);
  memset(delimiter, 'D', length);
  document->delimiter = delimiter;
  document->delimiter_length = length;
  assert(snapshot_scanner(scanner, state) == sizeof(state));
  return length;
}

static struct Scanner *make_exact_fit_scanner(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  assert(append_pending_document(
    scanner,
    make_repeated_document(initial_delimiter_bytes)
  ));
  fill_pending_document_to_capacity(scanner, 0);
  return scanner;
}

static void
test_here_document_boundaries_suspend_only_for_command_substitutions(void) {
  const struct {
    const char *source;
    const char *expected;
    size_t end;
    bool tabs;
    bool nested;
    bool line_start;
  } cases[] = {
    {"before\nEND\n}\nEND\n", "before\n", 7, false, false, true},
    {"before\n\tE\\\nND\n}\nEND\n", "before\nEND\n}\n", 16, true, false, true},
    {"before\\\nEND\nEND\n", "beforeEND\n", 12, false, false, true},
    {"before\nEND x\nEND\n", "before\nEND x\n", 13, false, false, true},
    {"before\nEND\nINNER\n", "before\nEND\n", 11, false, true, true},
    {"END\nEND\n", "END\n", 4, false, false, false},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(append_pending_document(
      &scanner,
      make_document("END", false, cases[index].tabs)
    ));
    assert(activate_startable_pending_documents(&scanner));
    assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
    scanner.source.stages[1].at_line_start = cases[index].line_start;
    if (cases[index].nested) {
      assert(source_context_push(&scanner, COMMAND_OPEN));
      scanner.substitution_depth = 1;
      assert(
        append_pending_document(&scanner, make_document("INNER", true, false))
      );
      assert(activate_startable_pending_documents(&scanner));
      assert(source_context_push(&scanner, QUOTED_HERE_DOCUMENT_BODY_START));
    } else {
      assert(source_context_push(&scanner, PARAMETER_OPEN));
    }
    int32_t source[64];
    size_t length = strlen(cases[index].source);
    assert(length <= ARRAY_LENGTH(source));
    for (size_t character = 0; character < length; character += 1) {
      source[character] = (unsigned char)cases[index].source[character];
    }
    struct MockLexer lexer;
    init_mock_lexer(&lexer, source, length);
    struct NativeInput native = {.lexer = &lexer.lexer};
    struct HereDocumentInput documents;
    assert(here_document_input_init(&documents, &native, &scanner));
    native.boundary = here_document_input_read;
    native.boundary_payload = &documents;
    struct LogicalLexer input;
    assert(logical_init(&input, &native, &scanner.source));
    for (
      size_t character = 0; character < strlen(cases[index].expected);
      character += 1
    ) {
      assert(input.result == SOURCE_CHARACTER);
      assert(input.lexer.lookahead == cases[index].expected[character]);
      input.lexer.advance(&input.lexer, false);
    }
    assert(input.result == SOURCE_END);
    assert(input.consumed == cases[index].end);
    assert(documents.end != NULL);
    assert(documents.end->context == (cases[index].nested ? 2 : 0));
    assert(documents.end->end == cases[index].end);
    assert(!native.failed);
    logical_clear(&input);
    here_document_input_clear(&documents);
    native_input_clear(&native);
    clear_scanner(&scanner);
  }
}

static void
test_here_document_end_restores_its_source_and_serialized_depth(void) {
  struct Scanner scanner = {0};
  assert(scanner_source_ready(&scanner));
  scanner.substitution_depth = 2;
  assert(append_pending_document(&scanner, make_document("OUT", false, false)));
  assert(activate_startable_pending_documents(&scanner));
  assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
  assert(source_context_push(&scanner, PARAMETER_OPEN));
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(&scanner, before);
  clear_scanner(&scanner);
  tree_sitter_dash_external_scanner_deserialize(
    &scanner,
    before,
    before_length
  );

  const int32_t source[] = {'O', 'U', 'T', '\n'};
  struct MockLexer lexer;
  init_mock_lexer(&lexer, source, 4);
  bool valid[TOKEN_COUNT] = {false};
  valid[HERE_DOCUMENT_END_BEGIN] = true;
#ifdef TREE_SITTER_REUSE_ALLOCATOR
  size_t first_allocation = reuse_allocation_calls;
#endif
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
#ifdef TREE_SITTER_REUSE_ALLOCATOR
  size_t allocation_count = reuse_allocation_calls - first_allocation;
#endif
  assert(lexer.lexer.result_symbol == HERE_DOCUMENT_END_BEGIN);
  assert(lexer.mark == 0);
  assert(scanner.context_count == 1);
  assert(scanner.source.stage_count == 3);
  assert(scanner.substitution_depth == 2);
  assert(scanner.suspended_frame_count == 0);
  assert(scanner.active_frame.count == 1);
  assert_document(&scanner.active_frame.documents[0], "OUT", false, false);
  assert(scanner.active_frame.documents[0].ending);

  char after[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned after_length = snapshot_scanner(&scanner, after);
  clear_scanner(&scanner);
  tree_sitter_dash_external_scanner_deserialize(&scanner, after, after_length);
  assert_scanner_matches_snapshot(&scanner, after, after_length);
  assert(scanner.active_frame.documents[0].ending);
  valid[HERE_DOCUMENT_END_BEGIN] = false;
  valid[HERE_DOCUMENT_END_TEXT_BEGIN] = true;
  init_mock_lexer(&lexer, source, 4);
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  assert(lexer.lexer.result_symbol == HERE_DOCUMENT_END_TEXT_BEGIN);
  clear_scanner(&scanner);

#ifdef TREE_SITTER_REUSE_ALLOCATOR
  valid[HERE_DOCUMENT_END_TEXT_BEGIN] = false;
  valid[HERE_DOCUMENT_END_BEGIN] = true;
  for (size_t failure = 1; failure <= allocation_count; failure += 1) {
    tree_sitter_dash_external_scanner_deserialize(
      &scanner,
      before,
      before_length
    );
    reuse_fail_allocation_call = reuse_allocation_calls + failure;
    init_mock_lexer(&lexer, source, 4);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    reuse_fail_allocation_call = 0;
    assert_scanner_matches_snapshot(&scanner, before, before_length);
    clear_scanner(&scanner);
    assert(reuse_live_allocations == 0);
  }
#endif
}

static void test_disabled_and_rejected_recovery_scans_preserve_state(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);

  scanner->sequence_end_pending = true;
  scanner->backquote_depth = 3;
  assert(append_pending_document(scanner, make_document("EOF", false, false)));

  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);

  bool valid_symbols[TOKEN_COUNT];
  for (size_t index = 0; index < TOKEN_COUNT; index += 1) {
    valid_symbols[index] = true;
  }

  const struct {
    int32_t source[8];
    size_t length;
  } inputs[] = {
    {{'2', '>', 'x'}, 3},
    {{'>'}, 1},
    {{0}, 0},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(inputs); index += 1) {
    struct MockLexer mock;
    init_mock_lexer(&mock, inputs[index].source, inputs[index].length);
    assert(!tree_sitter_dash_external_scanner_scan(
      scanner,
      &mock.lexer,
      valid_symbols
    ));
    assert_scanner_matches_snapshot(scanner, before, before_length);
    bool disabled_symbols[TOKEN_COUNT] = {false};
    init_mock_lexer(&mock, inputs[index].source, inputs[index].length);
    assert(!tree_sitter_dash_external_scanner_scan(
      scanner,
      &mock.lexer,
      disabled_symbols
    ));
    assert_scanner_matches_snapshot(scanner, before, before_length);
  }
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void
test_recovery_emits_newlines_without_reclassifying_continuations(void) {
  const int32_t input[] = {'\n', '\\', '\n', '\\', '\n', '\n', 'x'};
  const struct {
    enum TokenType symbol;
    size_t length;
  } expected[] = {
    {LOGICAL_NEWLINE_BEGIN, 0},
    {LOGICAL_NEWLINE_BEGIN_CHARACTER, 1},
    {SOURCE_BEGIN, 0},
    {LINE_CONTINUATION, 1},
    {REMOVED_NEWLINE, 1},
    {LINE_CONTINUATION, 1},
    {REMOVED_NEWLINE, 1},
    {LOGICAL_NEWLINE_BEGIN, 0},
    {LOGICAL_NEWLINE_BEGIN_CHARACTER, 1},
  };
  struct Scanner scanner = {0};
  bool valid[TOKEN_COUNT];
  for (size_t index = 0; index < TOKEN_COUNT; index += 1) {
    valid[index] = true;
  }
  size_t offset = 0;
  char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned state_length = 0;
  for (size_t index = 0; index < ARRAY_LENGTH(expected); index += 1) {
    struct MockLexer lexer;
    init_mock_lexer(&lexer, input + offset, ARRAY_LENGTH(input) - offset);
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == expected[index].symbol);
    assert(lexer.mark == expected[index].length);
    offset += lexer.mark;
    state_length = snapshot_scanner(&scanner, state);
    tree_sitter_dash_external_scanner_deserialize(
      &scanner,
      state,
      state_length
    );
    assert_scanner_matches_snapshot(&scanner, state, state_length);
  }
  assert(offset == 6);
  assert(!scanner.emission.active);
  struct MockLexer lexer;
  init_mock_lexer(&lexer, input + offset, 1);
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  assert(lexer.lexer.result_symbol == WORD_INITIAL_LITERAL_BEGIN);
  assert(lexer.mark == 0);
  assert(scanner.emission.symbol == WORD_INITIAL_LITERAL_BEGIN_WHOLE);
  assert(scanner.emission.remaining == 1);
  clear_scanner(&scanner);
}

static void
test_recovery_preserves_reserved_and_punctuation_token_identity(void) {
  const struct {
    int32_t source[8];
    size_t length;
    enum TokenType begin;
    enum TokenType piece;
    size_t width;
  } cases[] = {
    {{'{', ' '}, 2, LEFT_BRACE, LEFT_BRACE_CHARACTER, 1},
    {{'}', '\n'}, 2, RIGHT_BRACE, RIGHT_BRACE_CHARACTER, 1},
    {{'!', ' '}, 2, PIPELINE_NEGATION, BANG_BEGIN_PIECE, 1},
    {{'(', 'x'},
      2,
      PUNCT_LEFT_PARENTHESIS,
      PUNCT_LEFT_PARENTHESIS_CHARACTER,
      1},
    {{')', 'x'},
      2,
      PUNCT_RIGHT_PARENTHESIS,
      PUNCT_RIGHT_PARENTHESIS_CHARACTER,
      1},
    {{';', 'x'}, 2, PUNCT_SEMICOLON, PUNCT_SEMICOLON_CHARACTER, 1},
    {{'&', 'x'}, 2, PUNCT_AMPERSAND, PUNCT_AMPERSAND_CHARACTER, 1},
    {{'|', 'x'}, 2, PUNCT_PIPE, PUNCT_PIPE_CHARACTER, 1},
    {{'&', '&', 'x'}, 3, AND_IF_BEGIN, AND_IF_BEGIN_PIECE, 2},
    {{'|', '|', 'x'}, 3, OR_IF_BEGIN, OR_IF_BEGIN_PIECE, 2},
    {{'i', 'f', ' '}, 3, IF_KEYWORD, IF_KEYWORD_BEGIN_WHOLE, 2},
    {{'t', 'h', 'e', 'n', ';'}, 5, THEN_KEYWORD, THEN_KEYWORD_BEGIN_WHOLE, 4},
    {{'e', 'l', 'i', 'f', '\n'}, 5, ELIF_KEYWORD, ELIF_KEYWORD_BEGIN_WHOLE, 4},
    {{'e', 'l', 's', 'e', ' '}, 5, ELSE_KEYWORD, ELSE_KEYWORD_BEGIN_WHOLE, 4},
    {{'f', 'i', ';'}, 3, FI_KEYWORD, FI_KEYWORD_BEGIN_WHOLE, 2},
    {{'f', 'o', 'r', ' '}, 4, FOR_KEYWORD, FOR_KEYWORD_BEGIN_WHOLE, 3},
    {{'i', 'n', '\n'}, 3, IN_KEYWORD, IN_KEYWORD_BEGIN_WHOLE, 2},
    {{'d', 'o', '\n'}, 3, DO_KEYWORD, DO_KEYWORD_BEGIN_WHOLE, 2},
    {{'d', 'o', 'n', 'e', ';'}, 5, DONE_KEYWORD, DONE_KEYWORD_BEGIN_WHOLE, 4},
    {{'c', 'a', 's', 'e', ' '}, 5, CASE_KEYWORD, CASE_KEYWORD_BEGIN_WHOLE, 4},
    {{'e', 's', 'a', 'c', '\n'}, 5, ESAC_KEYWORD, ESAC_KEYWORD_BEGIN_WHOLE, 4},
    {{'w', 'h', 'i', 'l', 'e', ' '},
      6,
      WHILE_KEYWORD,
      WHILE_KEYWORD_BEGIN_WHOLE,
      5},
    {{'u', 'n', 't', 'i', 'l', ' '},
      6,
      UNTIL_KEYWORD,
      UNTIL_KEYWORD_BEGIN_WHOLE,
      5},
  };
  bool valid[TOKEN_COUNT];
  for (size_t index = 0; index < TOKEN_COUNT; index += 1) {
    valid[index] = true;
  }
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    struct MockLexer lexer;
    init_mock_lexer(&lexer, cases[index].source, cases[index].length);
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == cases[index].begin);
    assert(lexer.mark == 0);
    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, state);
    tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
    init_mock_lexer(&lexer, cases[index].source, cases[index].length);
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == cases[index].piece);
    assert(lexer.mark == cases[index].width);
    assert(!scanner.emission.active);
    clear_scanner(&scanner);
  }

  const int32_t continued[] = {'f', '\\', '\n', 'i', ';'};
  const struct {
    enum TokenType symbol;
    size_t width;
  } expected[][5] = {
    {
      {FI_KEYWORD, 0},
      {FI_KEYWORD_BEGIN_PIECE, 1},
      {LINE_CONTINUATION, 1},
      {REMOVED_NEWLINE, 1},
      {FI_KEYWORD_BEGIN_PIECE, 1},
    },
    {{FI_KEYWORD, 0}, {FI_KEYWORD_BEGIN_WHOLE, 4}},
  };
  const size_t counts[] = {5, 2};
  for (size_t recovery = 0; recovery < 2; recovery += 1) {
    for (size_t token = 0; token < TOKEN_COUNT; token += 1) {
      valid[token] = recovery != 0;
    }
    valid[FI_KEYWORD] = true;
    valid[FI_KEYWORD_BEGIN_WHOLE] = true;
    valid[FI_KEYWORD_BEGIN_PIECE] = true;
    valid[LINE_CONTINUATION] = true;
    valid[REMOVED_NEWLINE] = true;
    struct Scanner scanner = {0};
    size_t offset = 0;
    for (size_t index = 0; index < counts[recovery]; index += 1) {
      struct MockLexer lexer;
      init_mock_lexer(
        &lexer,
        continued + offset,
        ARRAY_LENGTH(continued) - offset
      );
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == expected[recovery][index].symbol);
      assert(lexer.mark == expected[recovery][index].width);
      offset += lexer.mark;
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
    }
    assert(offset == 4);
    assert(!scanner.emission.active);
    clear_scanner(&scanner);
  }
}

static void
test_recovery_parameter_escapes_leave_the_closing_brace_available(void) {
  const int32_t source[] = {'\\', 'q', '}'};
  const struct {
    enum TokenType contexts[3];
    size_t count;
    bool quoted;
  } cases[] = {
    {{PARAMETER_OPEN}, 1, false},
    {{PARAMETER_OPEN, PARAMETER_OPEN}, 2, false},
    {{DQ_OPEN, PARAMETER_OPEN}, 2, true},
    {{DQ_OPEN, PARAMETER_OPEN, PARAMETER_OPEN}, 3, true},
    {{DQ_OPEN, COMMAND_OPEN, PARAMETER_OPEN}, 3, false},
    {{HERE_DOCUMENT_BODY_START, PARAMETER_OPEN}, 2, true},
    {{HERE_DOCUMENT_BODY_START, COMMAND_OPEN, PARAMETER_OPEN}, 3, false},
    {{ARITHMETIC_LEFT_PARENTHESIS, PARAMETER_OPEN}, 2, true},
    {{ARITHMETIC_LEFT_PARENTHESIS, PARAMETER_OPEN, PARAMETER_OPEN}, 3, true},
    {{ARITHMETIC_LEFT_PARENTHESIS, COMMAND_OPEN, PARAMETER_OPEN}, 3, false},
    {{ARITHMETIC_LEFT_PARENTHESIS, BACKQUOTE_START, PARAMETER_OPEN}, 3, false},
    {{ARITHMETIC_SOURCE_LEFT_PARENTHESIS, PARAMETER_OPEN}, 2, true},
    {{ARITHMETIC_SOURCE_LEFT_PARENTHESIS, PARAMETER_OPEN, PARAMETER_OPEN},
      3,
      true},
    {{ARITHMETIC_SOURCE_LEFT_PARENTHESIS, COMMAND_OPEN, PARAMETER_OPEN},
      3,
      false},
    {{ARITHMETIC_SOURCE_LEFT_PARENTHESIS, BACKQUOTE_START, PARAMETER_OPEN},
      3,
      false},
  };
  bool valid[TOKEN_COUNT];
  for (size_t token = 0; token < TOKEN_COUNT; token += 1) {
    valid[token] = true;
  }
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    for (size_t context = 0; context < cases[index].count; context += 1) {
      enum TokenType opener = cases[index].contexts[context];
      if (opener == HERE_DOCUMENT_BODY_START) {
        assert(
          append_pending_document(&scanner, make_document("END", false, false))
        );
        assert(activate_startable_pending_documents(&scanner));
      } else if (opener == COMMAND_OPEN) {
        scanner.substitution_depth += 1;
      }
      assert(source_context_push(&scanner, opener));
    }
    const enum TokenType symbols[] = {
      cases[index].quoted ? DOUBLE_QUOTED_PARAMETER_TEXT_BEGIN
                          : ESCAPED_CHARACTER_BEGIN,
      cases[index].quoted ? DOUBLE_QUOTED_PARAMETER_TEXT_BEGIN_PIECE
                          : ESCAPED_CHARACTER_BEGIN_PIECE,
      cases[index].quoted ? DOUBLE_QUOTED_PARAMETER_TEXT_BEGIN_PIECE
                          : ESCAPED_CHARACTER_BEGIN_PIECE,
      PARAMETER_CLOSE,
      PARAMETER_CLOSE_CHARACTER,
    };
    const size_t widths[] = {0, 1, 1, 0, 1};
    size_t offset = 0;
    for (size_t step = 0; step < ARRAY_LENGTH(symbols); step += 1) {
      struct MockLexer lexer;
      init_mock_lexer(&lexer, source + offset, ARRAY_LENGTH(source) - offset);
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == symbols[step]);
      assert(lexer.mark == widths[step]);
      offset += lexer.mark;
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
      assert_scanner_matches_snapshot(&scanner, state, length);
    }
    assert(offset == ARRAY_LENGTH(source));
    size_t depth = 0;
    for (size_t context = 0; context < scanner.context_count; context += 1) {
      depth += scanner.contexts[context].count;
    }
    assert(depth == cases[index].count - 1);
    assert(!scanner.emission.active);
    clear_scanner(&scanner);
  }
}

static void
test_parenthesis_recovery_defers_substitution_state_to_grammar(void) {
  const enum TokenType ends[] = {
    COMMAND_SUBSTITUTION_END,
    ARITHMETIC_EXPANSION_END
  };
  for (size_t index = 0; index < ARRAY_LENGTH(ends); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(source_context_push(&scanner, DQ_OPEN));
    assert(source_context_push(&scanner, COMMAND_OPEN));
    scanner.substitution_depth = 1;
    bool valid[TOKEN_COUNT];
    for (size_t token = 0; token < TOKEN_COUNT; token += 1) {
      valid[token] = true;
    }
    const int32_t source[] = {')'};
    const enum TokenType expected[] = {
      PUNCT_RIGHT_PARENTHESIS,
      PUNCT_RIGHT_PARENTHESIS_CHARACTER
    };
    for (size_t part = 0; part < ARRAY_LENGTH(expected); part += 1) {
      struct MockLexer lexer;
      init_mock_lexer(&lexer, source, 1);
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == expected[part]);
      assert(lexer.mark == part);
      assert(scanner.context_count == 2);
      assert(scanner.substitution_depth == 1);
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
    }
    struct MockLexer lexer;
    init_mock_lexer(&lexer, NULL, 0);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(scanner.context_count == 2);
    memset(valid, 0, sizeof(valid));
    valid[ends[index]] = true;
    init_mock_lexer(&lexer, NULL, 0);
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == ends[index]);
    assert(lexer.mark == 0);
    assert(scanner.context_count == 1);
    assert(scanner.contexts[0].opener == DQ_OPEN);
    assert(
      scanner.substitution_depth ==
      (ends[index] == COMMAND_SUBSTITUTION_END ? 0 : 1)
    );
    clear_scanner(&scanner);
  }
}

static void test_state_round_trip(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  struct Scanner *restored = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  assert(restored != NULL);

  char initial[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned initial_length = snapshot_scanner(scanner, initial);

  assert(scanner_source_ready(scanner));
  assert(source_context_push(scanner, SQ_OPEN));
  scanner->emission = (struct LexicalEmission){
    .symbol = SINGLE_QUOTE_CONTENT_BEGIN,
    .remaining = two_byte_varint_minimum,
    .active = true,
  };

  scanner->sequence_end_pending = true;
  scanner->active_frame.at_line_start = true;
  scanner->backquote_depth = 7;
  scanner->substitution_depth = 5;
  scanner->active_frame.body_backquote_depth = 3;

  assert(append_pending_document(scanner, make_document("first", true, false)));
  assert(
    append_pending_document(scanner, make_document("second", false, true))
  );
  assert(append_document(
    &scanner->active_frame.documents,
    &scanner->active_frame.count,
    make_document("active", true, true)
  ));

  scanner->suspended_frames = ts_calloc(1, sizeof(struct HereDocumentFrame));
  assert(scanner->suspended_frames != NULL);
  scanner->suspended_frame_count = 1;
  scanner->suspended_frames[0].at_line_start = true;
  scanner->suspended_frames[0].body_backquote_depth = 6;
  assert(append_document(
    &scanner->suspended_frames[0].documents,
    &scanner->suspended_frames[0].count,
    make_document("suspended", false, true)
  ));
  scanner->pending_documents[1].declaration_depth = 9;

  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(scanner, serialized);
  assert((uint8_t)serialized[0] == SCANNER_SERIALIZATION_VERSION);

  tree_sitter_dash_external_scanner_deserialize(restored, serialized, length);

  assert(restored->sequence_end_pending);
  assert(restored->active_frame.at_line_start);
  assert(restored->backquote_depth == 7);
  assert(restored->substitution_depth == 5);
  assert(restored->active_frame.body_backquote_depth == 3);
  assert(restored->source.stage_count == 1);
  assert(restored->source.stages[0].disabled);
  assert(restored->context_count == 1);
  assert(restored->contexts[0].opener == SQ_OPEN);
  assert(restored->contexts[0].stage_count == 1);
  assert(restored->contexts[0].count == 1);
  assert(!restored->contexts[0].local_disabled);
  assert(restored->emission.active);
  assert(restored->emission.symbol == SINGLE_QUOTE_CONTENT_BEGIN);
  assert(restored->emission.remaining == two_byte_varint_minimum);
  assert(!restored->emission.removed_newline);
  assert(restored->pending_count == 2);
  assert_document(&restored->pending_documents[0], "first", true, false);
  assert(restored->pending_documents[0].declaration_depth == 12);
  assert_document(&restored->pending_documents[1], "second", false, true);
  assert(restored->pending_documents[1].declaration_depth == 9);
  assert(restored->active_frame.count == 1);
  assert_document(&restored->active_frame.documents[0], "active", true, true);
  assert(restored->suspended_frame_count == 1);
  assert(restored->suspended_frames[0].at_line_start);
  assert(restored->suspended_frames[0].body_backquote_depth == 6);
  assert(restored->suspended_frames[0].count == 1);
  assert_document(
    &restored->suspended_frames[0].documents[0],
    "suspended",
    false,
    true
  );
  assert_scanner_matches_snapshot(restored, serialized, length);

  tree_sitter_dash_external_scanner_deserialize(restored, NULL, 0);
  assert_scanner_matches_snapshot(restored, initial, initial_length);

  tree_sitter_dash_external_scanner_destroy(restored);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_equal_source_contexts_preserve_each_policy_restore(void) {
  const size_t context_count = repeated_context_count;
  struct Scanner scanner = {0};
  assert(scanner_source_ready(&scanner));
  scanner.source.stages[0].disabled = true;
  for (size_t index = 0; index < context_count; index += 1) {
    assert(source_context_push(&scanner, COMMAND_OPEN));
  }
  assert(scanner.context_count == 2);
  assert(scanner.contexts[0].count == 1);
  assert(scanner.contexts[0].local_disabled);
  assert(scanner.contexts[1].count == context_count - 1);
  assert(!scanner.contexts[1].local_disabled);
  assert(source_context_push(&scanner, PARAMETER_OPEN));
  assert(source_context_push(&scanner, COMMAND_OPEN));
  assert(scanner.context_count == 4);
  assert(source_context_pop(&scanner));
  assert(source_context_pop(&scanner));
  assert(scanner.context_count == 2);

  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(&scanner, serialized);
  struct Scanner restored = {0};
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, serialized, length);
  assert(restored.contexts[1].count == context_count - 1);
  for (size_t index = 0; index < context_count - 1; index += 1) {
    assert(source_context_pop(&restored));
    assert(!restored.source.stages[0].disabled);
  }
  assert(restored.context_count == 1);
  assert(source_context_pop(&restored));
  assert(restored.context_count == 0);
  assert(restored.source.stages[0].disabled);
  assert(!source_context_pop(&restored));
  clear_scanner(&restored);
  clear_scanner(&scanner);
}

static void test_substitution_contexts_suspend_the_enclosing_body_stage(void) {
  struct Scanner scanner = {0};
  assert(scanner_source_ready(&scanner));
  assert(append_pending_document(&scanner, make_document("END", false, true)));
  assert(activate_startable_pending_documents(&scanner));
  assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
  assert(scanner.source.stages[1].kind == SOURCE_HERE_DOCUMENT);
  assert(source_context_push(&scanner, PARAMETER_OPEN));
  assert(source_context_push(&scanner, ARITHMETIC_LEFT_PARENTHESIS));
  assert(!scanner.source.stages[1].disabled);
  assert(source_context_pop(&scanner));
  assert(source_context_push(&scanner, COMMAND_OPEN));
  assert(scanner.source.stages[1].disabled);
  assert(scanner.contexts[2].suspends_body);
  assert(source_context_push(&scanner, COMMAND_OPEN));
  assert(source_context_push(&scanner, COMMAND_OPEN));
  assert(scanner.context_count == 4);
  assert(scanner.contexts[3].count == 2);
  assert(!scanner.contexts[3].suspends_body);
  assert(source_context_push(&scanner, BACKQUOTE_START));
  assert(scanner.context_count == 5);
  assert(!scanner.contexts[4].suspends_body);
  assert(scanner.source.stages[1].disabled);

  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(&scanner, serialized);
  struct Scanner restored = {0};
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, serialized, length);
  assert(restored.contexts[2].suspends_body);
  assert(restored.source.stages[1].disabled);
  assert(source_context_pop(&restored));
  assert(source_context_pop(&restored));
  assert(source_context_pop(&restored));
  assert(restored.context_count == 3);
  assert(restored.source.stages[1].disabled);
  assert(source_context_pop(&restored));
  assert(!restored.source.stages[1].disabled);
  assert(source_context_pop(&restored));
  assert(restored.context_count == 1);
  assert(!restored.source.stages[1].disabled);
  clear_scanner(&restored);
  clear_scanner(&scanner);
}

static void test_source_context_counts_round_trip_and_reject_overflow(void) {
  const size_t counts[] = {
    1,
    two_byte_varint_minimum - 1,
    two_byte_varint_minimum,
    repeated_context_count,
    SIZE_MAX,
  };
  for (size_t index = 0; index < ARRAY_LENGTH(counts); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(source_context_push(&scanner, COMMAND_OPEN));
    scanner.contexts[0].count = counts[index];
    char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, serialized);
    struct Scanner restored = {0};
    tree_sitter_dash_external_scanner_deserialize(
      &restored,
      serialized,
      length
    );
    assert_scanner_matches_snapshot(&restored, serialized, length);
    assert(restored.context_count == 1);
    assert(restored.contexts[0].count == counts[index]);
    if (counts[index] == SIZE_MAX) {
      assert(!source_context_push(&restored, COMMAND_OPEN));
      assert_scanner_matches_snapshot(&restored, serialized, length);
    }
    clear_scanner(&restored);
    clear_scanner(&scanner);
  }

  struct Scanner scanner = {0};
  struct Scanner empty = {0};
  struct Scanner restored = {0};
  char empty_state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned empty_length = snapshot_scanner(&empty, empty_state);
  assert(scanner_source_ready(&scanner));
  assert(source_context_push(&scanner, COMMAND_OPEN));
  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(&scanner, serialized);
  assert(serialized[length - 3] == 1);
  serialized[length - 3] = 0;
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, empty_state, empty_length);

  assert(source_context_push(&scanner, PARAMETER_OPEN));
  scanner.contexts[0].count = SIZE_MAX - 1;
  length = snapshot_scanner(&scanner, serialized);
  assert(!source_context_push(&scanner, COMMAND_OPEN));
  assert_scanner_matches_snapshot(&scanner, serialized, length);
  assert(serialized[length - 3] == 1);
  serialized[length - 3] = 2;
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, empty_state, empty_length);
  clear_scanner(&restored);
  clear_scanner(&scanner);
}

static void test_bracket_chains_round_trip_and_reject_unknown_phases(void) {
  struct Scanner scanner = {0};
  struct Scanner empty = {0};
  struct Scanner restored = {0};
  char empty_state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned empty_length = snapshot_scanner(&empty, empty_state);
  assert(scanner_source_ready(&scanner));
  assert(source_context_push(&scanner, WORD_FALLBACK_LITERAL_BEGIN));
  scanner.contexts[0].bracket_chains = BRACKET_CHAINS_LIMIT - 1;
  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(&scanner, serialized);
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, serialized, length);
  assert(restored.context_count == 1);
  assert(restored.contexts[0].bracket_chains == BRACKET_CHAINS_LIMIT - 1);

  scanner.contexts[0].bracket_chains = BRACKET_CHAINS_LIMIT;
  length = snapshot_scanner(&scanner, serialized);
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, empty_state, empty_length);
  clear_scanner(&restored);
  clear_scanner(&scanner);
}

static void test_lone_word_contexts_round_trip_and_block_the_remainder(void) {
  struct Scanner scanner = {0};
  struct Scanner empty = {0};
  struct Scanner restored = {0};
  char empty_state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned empty_length = snapshot_scanner(&empty, empty_state);
  assert(scanner_source_ready(&scanner));
  assert(source_context_push(&scanner, BACKQUOTE_START));
  scanner.contexts[0].lone_word = true;
  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(&scanner, serialized);
  assert(serialized[length - 2] == 2);
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, serialized, length);
  assert(restored.context_count == 1);
  assert(restored.contexts[0].lone_word);
  assert(!restored.contexts[0].local_disabled);

  const int32_t parenthesis[] = {'(', 'x'};
  bool valid[TOKEN_COUNT] = {false};
  valid[BACKQUOTE_REMAINDER_PARENTHESIS_BEGIN] = true;
  struct MockLexer lexer;
  init_mock_lexer(&lexer, parenthesis, ARRAY_LENGTH(parenthesis));
  assert(
    !tree_sitter_dash_external_scanner_scan(&restored, &lexer.lexer, valid)
  );
  assert_scanner_matches_snapshot(&restored, serialized, length);
  restored.contexts[0].lone_word = false;
  init_mock_lexer(&lexer, parenthesis, ARRAY_LENGTH(parenthesis));
  assert(
    tree_sitter_dash_external_scanner_scan(&restored, &lexer.lexer, valid)
  );
  assert(lexer.lexer.result_symbol == BACKQUOTE_REMAINDER_PARENTHESIS_BEGIN);
  assert(lexer.mark == 0);
  clear_scanner(&restored);

  const int32_t escaped[] = {'\\', '"', '('};
  memset(valid, 0, sizeof(valid));
  valid[WORD_INITIAL_ESCAPED_CHARACTER_BEGIN] = true;
  valid[FNAME_TOKEN] = true;
  scanner.contexts[0].lone_word = false;
  init_mock_lexer(&lexer, escaped, ARRAY_LENGTH(escaped));
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  assert(lexer.lexer.result_symbol == WORD_INITIAL_ESCAPED_CHARACTER_BEGIN);
  assert(scanner.contexts[0].lone_word);
  valid[FNAME_TOKEN] = false;
  scanner.emission = (struct LexicalEmission){0};
  init_mock_lexer(&lexer, escaped, ARRAY_LENGTH(escaped));
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  assert(!scanner.contexts[0].lone_word);

  const int32_t redirect[] = {'>', 'f'};
  memset(valid, 0, sizeof(valid));
  valid[PUNCT_GREATER] = true;
  scanner.contexts[0].lone_word = true;
  scanner.emission = (struct LexicalEmission){0};
  init_mock_lexer(&lexer, redirect, ARRAY_LENGTH(redirect));
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  assert(lexer.lexer.result_symbol == PUNCT_GREATER);
  assert(!scanner.contexts[0].lone_word);
  clear_scanner(&scanner);

  assert(scanner_source_ready(&scanner));
  assert(source_context_push(&scanner, COMMAND_OPEN));
  length = snapshot_scanner(&scanner, serialized);
  assert(serialized[length - 2] == 0);
  serialized[length - 2] = 2;
  tree_sitter_dash_external_scanner_deserialize(&restored, serialized, length);
  assert_scanner_matches_snapshot(&restored, empty_state, empty_length);
  clear_scanner(&restored);
  clear_scanner(&scanner);
}

static void test_document_eof_follows_removed_source_at_line_start(void) {
  const struct {
    int32_t source[3];
    size_t length;
    enum TokenType removed;
  } fixtures[] = {
    {{'\t'}, 1, REMOVED_SOURCE},
    {{'\\', '\n'}, 2, LINE_CONTINUATION},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(append_pending_document(&scanner, make_document("X", false, true)));
    assert(activate_startable_pending_documents(&scanner));
    assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
    scanner.active_frame.at_line_start = true;
    bool valid[TOKEN_COUNT] = {false};
    valid[HERE_DOCUMENT_EOF] = true;
    valid[HERE_DOCUMENT_END_BEGIN] = true;
    valid[HERE_DOCUMENT_CONTENT_LINE_START] = true;
    valid[SOURCE_BEGIN] = true;
    valid[REMOVED_SOURCE] = true;
    valid[LINE_CONTINUATION] = true;
    valid[REMOVED_NEWLINE] = true;
    const enum TokenType expected[] = {
      HERE_DOCUMENT_CONTENT_LINE_START,
      SOURCE_BEGIN,
      fixtures[index].removed,
      REMOVED_NEWLINE,
      HERE_DOCUMENT_EOF,
    };
    size_t offset = 0;
    for (size_t step = 0; step < ARRAY_LENGTH(expected); step += 1) {
      if (
        expected[step] == REMOVED_NEWLINE &&
        fixtures[index].removed != LINE_CONTINUATION
      ) {
        continue;
      }
      struct MockLexer lexer;
      init_mock_lexer(
        &lexer,
        fixtures[index].source + offset,
        fixtures[index].length - offset
      );
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == expected[step]);
      offset += lexer.mark;
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned state_length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(
        &scanner,
        state,
        state_length
      );
    }
    assert(offset == fixtures[index].length);
    assert(scanner.active_frame.count == 0);
    clear_scanner(&scanner);
  }
}

static void test_bracket_probe_takes_range_endpoints_literally(void) {
  const struct {
    const char *source;
    bool complete;
  } fixtures[] = {
    {"[x-[:alpha:]]x", true},
    {"[x-[:alpha:]", true},
    {"[[:alpha:]-[:alpha:]", false},
    {"[[:alpha:]-a]x", true},
    {"[a-]x", true},
    {"[[-a]x", true},
    {"[a-c-]x", true},
    {"[--[:alpha:]x", true},
    {"[a-", false},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    struct SourceFixture fixture;
    init_source_fixture(&fixture, fixtures[index].source, &stage, 1);
    uint16_t chains = 0;
    bool complete = false;
    assert(scan_pattern_bracket(
      NULL,
      &fixture.lookahead.lexer,
      PATTERN_WORD,
      &chains,
      &complete
    ));
    assert(complete == fixtures[index].complete);
    if (complete) {
      assert(fixture.lookahead.lexer.lookahead == ']');
    }
    assert(!fixture.lookahead.failed);
    clear_source_fixture(&fixture);
  }
}

static void test_here_document_sequence_requires_a_line_boundary(void) {
  const struct {
    const char *source;
    bool enabled;
    bool accepted;
  } cases[] = {
    {"", true, true},
    {" ", true, true},
    {"\n", true, true},
    {" \n", true, true},
    {"\t \n", true, true},
    {"#comment", true, true},
    {" #comment\n", true, true},
    {";", true, false},
    {" argument\n", true, false},
    {" >file\n", true, false},
    {"", false, false},
    {" \n", false, false},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(
      append_pending_document(&scanner, make_document("END", false, false))
    );
    char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, before);
    struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    struct SourceFixture fixture;
    init_source_fixture(&fixture, cases[index].source, &stage, 1);
    bool valid[TOKEN_COUNT] = {false};
    valid[HERE_DOCUMENT_SEQUENCE_BEGIN] = cases[index].enabled;
    assert(
      scan_dispatch(&scanner, &fixture.lookahead.lexer, valid) ==
      cases[index].accepted
    );
    assert_scanner_matches_snapshot(&scanner, before, length);
    if (cases[index].accepted) {
      assert(
        fixture.lookahead.lexer.result_symbol == HERE_DOCUMENT_SEQUENCE_BEGIN
      );
      assert(fixture.input.mark == 0);
    } else {
      assert(fixture.lookahead.position == 0);
      assert(fixture.lookahead.lexer.lookahead == cases[index].source[0]);
    }
    assert(!fixture.lookahead.failed);
    clear_source_fixture(&fixture);
    clear_scanner(&scanner);
  }
}

static void test_here_document_line_start_only_emits_valid_tokens(void) {
  const struct {
    const char *source;
    bool content_valid;
    bool end_valid;
    enum TokenType expected;
  } fixtures[] = {
    {"body\n", false, false, TOKEN_COUNT},
    {"EOF\n", false, false, TOKEN_COUNT},
    {"body\n", false, true, TOKEN_COUNT},
    {"EOF\n", false, true, HERE_DOCUMENT_END_BEGIN},
    {"body\n", true, false, HERE_DOCUMENT_CONTENT_LINE_START},
    {"EOF\n", true, false, TOKEN_COUNT},
    {"body\n", true, true, HERE_DOCUMENT_CONTENT_LINE_START},
    {"EOF\n", true, true, HERE_DOCUMENT_END_BEGIN},
  };
  for (size_t quoted = 0; quoted < 2; quoted += 1) {
    for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
      struct Scanner scanner = {0};
      assert(scanner_source_ready(&scanner));
      assert(append_pending_document(
        &scanner,
        make_document("EOF", quoted != 0, false)
      ));
      assert(activate_startable_pending_documents(&scanner));
      assert(source_context_push(
        &scanner,
        quoted != 0 ? QUOTED_HERE_DOCUMENT_BODY_START : HERE_DOCUMENT_BODY_START
      ));
      scanner.active_frame.at_line_start = true;
      char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned before_length = snapshot_scanner(&scanner, before);
      int32_t input[5];
      size_t length = strlen(fixtures[index].source);
      for (size_t character = 0; character < length; character += 1) {
        input[character] = (unsigned char)fixtures[index].source[character];
      }
      struct MockLexer mock;
      init_mock_lexer(&mock, input, length);
      enum TokenType end =
        quoted != 0 ? QUOTED_HERE_DOCUMENT_END_BEGIN : HERE_DOCUMENT_END_BEGIN;
      bool valid_symbols[TOKEN_COUNT] = {false};
      valid_symbols[HERE_DOCUMENT_CONTENT_LINE_START] =
        fixtures[index].content_valid;
      valid_symbols[end] = fixtures[index].end_valid;
      bool accepted = tree_sitter_dash_external_scanner_scan(
        &scanner,
        &mock.lexer,
        valid_symbols
      );
      assert(accepted == (fixtures[index].expected != TOKEN_COUNT));
      if (accepted) {
        enum TokenType expected =
          fixtures[index].expected == HERE_DOCUMENT_END_BEGIN
          ? end
          : fixtures[index].expected;
        assert(mock.lexer.result_symbol == expected);
        assert(valid_symbols[mock.lexer.result_symbol]);
        assert(!scanner.active_frame.at_line_start);
        assert(mock.mark == 0);
        assert(scanner.active_frame.count == 1);
        assert_document(
          &scanner.active_frame.documents[0],
          "EOF",
          quoted != 0,
          false
        );
      } else {
        assert_scanner_matches_snapshot(&scanner, before, before_length);
      }
      clear_scanner(&scanner);
    }
  }
}

static void assert_boundary_scan(
  struct Scanner *scanner,
  const char *source,
  const bool *valid,
  enum TokenType expected
) {
  int32_t input[64];
  size_t length = strlen(source);
  assert(length <= ARRAY_LENGTH(input));
  for (size_t index = 0; index < length; index += 1) {
    input[index] = (unsigned char)source[index];
  }
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);
  struct MockLexer lexer;
  init_mock_lexer(&lexer, input, length);
  bool accepted =
    tree_sitter_dash_external_scanner_scan(scanner, &lexer.lexer, valid);
  assert(accepted == (expected != TOKEN_COUNT));
  if (accepted) {
    assert(lexer.lexer.result_symbol == expected);
    assert(lexer.mark == 0);
  }
  assert_scanner_matches_snapshot(scanner, before, before_length);
}

static void test_newline_boundaries_require_a_valid_enclosing_terminator(void) {
  const struct {
    const char *source;
    enum TokenType closer;
    bool closes;
    bool delimiter;
  } cases[] = {
    {"\n)\n", PUNCT_RIGHT_PARENTHESIS, true, false},
    {"\n}\n", RIGHT_BRACE, true, false},
    {"\nthen\n", THEN_KEYWORD, true, false},
    {"\nelif\n", ELIF_KEYWORD, true, false},
    {"\nelse\n", ELSE_KEYWORD, true, false},
    {"\nfi\n", FI_KEYWORD, true, false},
    {"\ndo\n", DO_KEYWORD, true, false},
    {"\ndone\n", DONE_KEYWORD, true, false},
    {"\nesac\n", ESAC_KEYWORD, true, false},
    {"\n;;\n", CASE_ITEM_END, true, false},
    {"\n;&\n", CASE_ITEM_END, false, false},
    {"\nf\\\ni\n", FI_KEYWORD, true, false},
    {"\n;\\\n;\n", CASE_ITEM_END, true, false},
    {"\n}word\n", RIGHT_BRACE, false, false},
    {"\nfinally\n", FI_KEYWORD, false, false},
    {"\n;\n", CASE_ITEM_END, false, false},
    {"\n&\n", CASE_ITEM_END, false, false},
    {"\n|\n", CASE_ITEM_END, false, false},
    {"\n&&\n", CASE_ITEM_END, false, false},
    {"\n||\n", CASE_ITEM_END, false, false},
    {"\nEND\n", PUNCT_RIGHT_PARENTHESIS, false, true},
    {"\nEND\n)\n", PUNCT_RIGHT_PARENTHESIS, false, true},
  };
  enum { NO_DOCUMENT, SUSPENDED_DOCUMENT, MATCHING_DOCUMENT };
  for (
    unsigned document = NO_DOCUMENT; document <= MATCHING_DOCUMENT;
    document += 1
  ) {
    for (unsigned enabled = 0; enabled < 2; enabled += 1) {
      for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
        struct Scanner scanner = {0};
        assert(scanner_source_ready(&scanner));
        if (document != NO_DOCUMENT) {
          assert(append_pending_document(
            &scanner,
            make_document("END", false, false)
          ));
          assert(activate_startable_pending_documents(&scanner));
          assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
          if (document == SUSPENDED_DOCUMENT) {
            assert(source_context_push(&scanner, COMMAND_OPEN));
            scanner.substitution_depth = 1;
          } else {
            assert(source_context_push(&scanner, PARAMETER_OPEN));
          }
        }
        bool valid[TOKEN_COUNT] = {false};
        valid[TERM_CONTINUATION] = true;
        valid[TERM_BOUNDARY] = true;
        valid[SEPARATOR_NEWLINE] = true;
        valid[cases[index].closer] = enabled != 0;
        bool closes = enabled != 0 && cases[index].closes;
        bool rejected = document != NO_DOCUMENT &&
          (closes || (document == MATCHING_DOCUMENT && cases[index].delimiter));
        enum TokenType expected = rejected ? TOKEN_COUNT
          : closes                         ? TERM_BOUNDARY
                                           : SEPARATOR_NEWLINE;
        assert_boundary_scan(&scanner, cases[index].source, valid, expected);
        clear_scanner(&scanner);
      }
    }
  }
}

static void test_static_command_words_preserve_quotes_and_scanner_state(void) {
  const struct {
    const char *source;
    enum TokenType token;
  } fixtures[] = {
    {"export x=1", DECLARATION_COMMAND},
    {"'ex'port x=1", DECLARATION_COMMAND},
    {"\"local\" x=1", DECLARATION_COMMAND},
    {"\\readonly x=1", DECLARATION_COMMAND},
    {"$'ex\\160ort' x=1", DECLARATION_COMMAND},
    {"command export", FORWARDING_COMMAND},
    {"-p export", FORWARDING_OPTION},
    {"-- export", FORWARDING_OPTION_END},
    {"printf x", TOKEN_COUNT},
    {"$cmd x=1", TOKEN_COUNT},
    {"'unclosed", TOKEN_COUNT},
    {"'a long command' x", TOKEN_COUNT},
    {"", TOKEN_COUNT},
    {"\n", TOKEN_COUNT},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    bool valid[TOKEN_COUNT] = {false};
    valid[DECLARATION_COMMAND] = true;
    valid[FORWARDING_COMMAND] = true;
    valid[FORWARDING_OPTION] = true;
    valid[FORWARDING_OPTION_END] = true;
    assert_boundary_scan(
      &scanner,
      fixtures[index].source,
      valid,
      fixtures[index].token
    );
    clear_scanner(&scanner);
  }
}

static void test_layout_boundaries_do_not_turn_invalid_source_into_end(void) {
  const struct {
    const char *source;
    enum TokenType continuation;
    enum TokenType closer;
    enum TokenType expected;
  } cases[] = {
    {"\n", TERM_CONTINUATION, TOKEN_COUNT, TOKEN_COUNT},
    {"\n\n", TERM_CONTINUATION, TOKEN_COUNT, TERM_BOUNDARY},
    {" \n)\n", TERM_CONTINUATION, TOKEN_COUNT, TERM_CONTINUATION},
    {" \n)\n", TERM_CONTINUATION, PUNCT_RIGHT_PARENTHESIS, TERM_BOUNDARY},
    {" # note\n|\n", TERM_CONTINUATION, TOKEN_COUNT, TERM_CONTINUATION},
    {";;\n", TERM_CONTINUATION, TOKEN_COUNT, TOKEN_COUNT},
    {";&\n", TERM_CONTINUATION, TOKEN_COUNT, TOKEN_COUNT},
    {";;\n", TERM_CONTINUATION, CASE_ITEM_END, CASE_ITEM_END},
    {";&\n", TERM_CONTINUATION, CASE_ITEM_END, TOKEN_COUNT},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    bool valid[TOKEN_COUNT] = {false};
    valid[cases[index].continuation] = true;
    valid[TERM_BOUNDARY] = true;
    valid[SEPARATOR_NEWLINE] = true;
    if (cases[index].closer != TOKEN_COUNT) {
      valid[cases[index].closer] = true;
    }
    assert_boundary_scan(
      &scanner,
      cases[index].source,
      valid,
      cases[index].expected
    );
    clear_scanner(&scanner);
  }
}

static void test_document_newline_boundaries_preserve_document_ownership(void) {
  enum { PENDING, SUSPENDED, MATCHING };
  const struct {
    const char *source;
    int document;
    enum TokenType expected;
  } cases[] = {
    {"\necho next\n", PENDING, TOKEN_COUNT},
    {" \necho next\n", PENDING, TOKEN_COUNT},
    {"\nEND\necho next\n", SUSPENDED, SEPARATOR_NEWLINE},
    {" \nEND\necho next\n", SUSPENDED, TERM_CONTINUATION},
    {"\nEND\necho next\n", MATCHING, TOKEN_COUNT},
    {" \nEND\necho next\n", MATCHING, TERM_BOUNDARY},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(
      append_pending_document(&scanner, make_document("END", false, false))
    );
    if (cases[index].document != PENDING) {
      assert(activate_startable_pending_documents(&scanner));
      assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
      if (cases[index].document == SUSPENDED) {
        assert(source_context_push(&scanner, COMMAND_OPEN));
        scanner.substitution_depth = 1;
      } else {
        assert(source_context_push(&scanner, PARAMETER_OPEN));
      }
    }
    bool valid[TOKEN_COUNT] = {false};
    valid[TERM_CONTINUATION] = true;
    valid[TERM_BOUNDARY] = true;
    valid[SEPARATOR_NEWLINE] = true;
    valid[PRE_NEWLINE_BLANK] = true;
    assert_boundary_scan(
      &scanner,
      cases[index].source,
      valid,
      cases[index].expected
    );
    clear_scanner(&scanner);
  }
}

static void test_old_state_is_rejected(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  struct Scanner *restored = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  assert(restored != NULL);

  scanner->backquote_depth = 4;
  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(scanner, serialized);
  serialized[0] = SCANNER_SERIALIZATION_VERSION - 1;

  restored->assignment_tilde_allowed = true;
  restored->backquote_depth = 9;
  tree_sitter_dash_external_scanner_deserialize(restored, serialized, length);
  assert(!restored->assignment_tilde_allowed);
  assert(restored->backquote_depth == 0);
  assert(restored->pending_count == 0);

  tree_sitter_dash_external_scanner_destroy(restored);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_exact_fit_state_round_trip(void) {
  struct Scanner *scanner = make_exact_fit_scanner();
  struct Scanner *restored = tree_sitter_dash_external_scanner_create();
  assert(restored != NULL);
  size_t delimiter_length = scanner->pending_documents[0].delimiter_length;

  char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(scanner, serialized);
  assert(length == TREE_SITTER_SERIALIZATION_BUFFER_SIZE);

  tree_sitter_dash_external_scanner_deserialize(restored, serialized, length);
  assert(restored->pending_count == 1);
  assert_repeated_document(
    &restored->pending_documents[0],
    'D',
    delimiter_length
  );
  assert_scanner_matches_snapshot(restored, serialized, length);

  tree_sitter_dash_external_scanner_destroy(restored);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_pending_document_rejects_oversized_state(void) {
  struct Scanner *scanner = make_exact_fit_scanner();
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);

  struct HereDocument rejected = make_document("x", false, false);
  assert(!append_pending_document(scanner, rejected));
  assert_scanner_matches_snapshot(scanner, before, before_length);

  clear_document(&rejected);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_pending_activation_fits_after_depth_growth(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  scanner->substitution_depth = three_byte_varint_minimum;
  scanner->backquote_depth = three_byte_varint_minimum;
  assert(append_pending_document(
    scanner,
    make_repeated_document(initial_delimiter_bytes)
  ));
  size_t delimiter_length = fill_pending_document_to_capacity(scanner, 0);

  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);
  assert(before_length == TREE_SITTER_SERIALIZATION_BUFFER_SIZE);

  assert(activate_startable_pending_documents(scanner));
  assert(scanner->pending_count == 0);
  assert(scanner->active_frame.count == 1);
  assert(
    scanner->active_frame.body_backquote_depth == three_byte_varint_minimum
  );
  assert_repeated_document(
    &scanner->active_frame.documents[0],
    'D',
    delimiter_length
  );
  char after[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  assert(snapshot_scanner(scanner, after) == before_length - 1);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_pending_documents_suspend_and_resume_at_capacity(void) {
  const size_t pending_delimiter_bytes = 200;
  const size_t suspended_delimiter_bytes = 503;
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  assert(append_pending_document(
    scanner,
    make_repeated_document(suspended_delimiter_bytes)
  ));
  assert(activate_startable_pending_documents(scanner));
  scanner->active_frame.at_line_start = true;
  scanner->substitution_depth = 1;
  assert(append_pending_document(
    scanner,
    make_repeated_document(pending_delimiter_bytes)
  ));
  assert(append_pending_document(
    scanner,
    make_repeated_document(pending_delimiter_bytes)
  ));
  size_t last_length = fill_pending_document_to_capacity(scanner, 1);

  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);
  assert(before_length == TREE_SITTER_SERIALIZATION_BUFFER_SIZE);
  assert(activate_startable_pending_documents(scanner));
  assert(scanner->pending_count == 0);
  assert(scanner->active_frame.count == 2);
  assert_repeated_document(
    &scanner->active_frame.documents[0],
    'D',
    pending_delimiter_bytes
  );
  assert_repeated_document(
    &scanner->active_frame.documents[1],
    'D',
    last_length
  );
  assert(scanner->suspended_frame_count == 1);
  assert(scanner->suspended_frames[0].count == 1);
  assert(scanner->suspended_frames[0].at_line_start);
  assert_repeated_document(
    &scanner->suspended_frames[0].documents[0],
    'D',
    suspended_delimiter_bytes
  );
  char after[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned after_length = snapshot_scanner(scanner, after);
  assert(after_length == TREE_SITTER_SERIALIZATION_BUFFER_SIZE);
  struct Scanner restored = {0};
  tree_sitter_dash_external_scanner_deserialize(&restored, after, after_length);
  assert_scanner_matches_snapshot(&restored, after, after_length);
  finish_active_document(&restored);
  assert(restored.active_frame.count == 1);
  assert_repeated_document(
    &restored.active_frame.documents[0],
    'D',
    last_length
  );
  assert(!restored.sequence_end_pending);
  finish_active_document(&restored);
  assert(restored.active_frame.count == 0);
  assert(restored.sequence_end_pending);
  struct MockLexer lexer;
  init_mock_lexer(&lexer, NULL, 0);
  bool valid[TOKEN_COUNT] = {false};
  valid[HERE_DOCUMENT_SEQUENCE_END] = true;
  assert(scan_here_document_sequence_end(&restored, &lexer.lexer, valid));
  assert(lexer.lexer.result_symbol == HERE_DOCUMENT_SEQUENCE_END);
  assert(!restored.sequence_end_pending);
  assert(restored.suspended_frame_count == 0);
  assert(restored.active_frame.at_line_start);
  assert(restored.active_frame.count == 1);
  assert_repeated_document(
    &restored.active_frame.documents[0],
    'D',
    suspended_delimiter_bytes
  );
  clear_scanner(&restored);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_pending_activation_rejects_oversized_state(void) {
  const size_t pending_delimiter_bytes = 400;
  const size_t suspended_delimiter_bytes = 503;
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  assert(append_pending_document(
    scanner,
    make_repeated_document(suspended_delimiter_bytes)
  ));
  assert(activate_startable_pending_documents(scanner));
  scanner->active_frame.at_line_start = true;
  scanner->substitution_depth = 1;
  assert(append_pending_document(
    scanner,
    make_repeated_document(pending_delimiter_bytes)
  ));
  fill_pending_document_to_capacity(scanner, 0);
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);
  assert(before_length == TREE_SITTER_SERIALIZATION_BUFFER_SIZE);
  assert(!activate_startable_pending_documents(scanner));
  assert_scanner_matches_snapshot(scanner, before, before_length);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_code_point_encoding(void) {
  uint8_t bytes[4];
  size_t length = 0;
  assert(encode_utf8_scalar(0x7f, bytes, &length));
  assert(length == 1 && bytes[0] == 0x7f);
  assert(encode_utf8_scalar(0xff, bytes, &length));
  assert(length == 2 && bytes[0] == 0xc3 && bytes[1] == 0xbf);
  assert(encode_utf8_scalar(0x10ffff, bytes, &length));
  assert(length == 4);
  assert(encode_utf8_scalar(0xd800, bytes, &length));
  assert(
    length == 3 && bytes[0] == 0xed && bytes[1] == 0xa0 && bytes[2] == 0x80
  );
  assert(!encode_utf8_scalar(-1, bytes, &length));
  assert(!encode_utf8_scalar(0x110000, bytes, &length));

  struct ByteBuffer exact = {.limit = 1};
  assert(!append_codepoint(&exact, 0xff));
  assert(exact.length == 0);
  ts_free(exact.data);
}

static void assert_control_escape_bytes(
  int32_t character,
  const uint8_t *expected,
  size_t expected_length
) {
  struct ByteBuffer buffer = {0};
  assert(append_control_escape(&buffer, character));
  assert(buffer.length == expected_length);
  assert(memcmp(buffer.data, expected, expected_length) == 0);
  ts_free(buffer.data);
}

static void test_control_escape_table(void) {
  uint8_t value = 0;
  for (int32_t index = 0; index < 26; index += 1) {
    uint8_t control = (uint8_t)(index + 1);
    assert(defined_control_escape_byte('a' + index, &value));
    assert(value == control);
    assert_control_escape_bytes('a' + index, &control, 1);
    assert(defined_control_escape_byte('A' + index, &value));
    assert(value == control);
    assert_control_escape_bytes('A' + index, &control, 1);
  }

  const int32_t punctuation[] = {'[', '\\', ']', '^', '_', '?'};
  const uint8_t expected[] = {0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x7f};
  for (size_t index = 0; index < sizeof(expected); index += 1) {
    assert(defined_control_escape_byte(punctuation[index], &value));
    assert(value == expected[index]);
    assert_control_escape_bytes(punctuation[index], &expected[index], 1);
  }

  const int32_t unspecified[] = {'@', '`', '{'};
  const uint8_t fallback[] = {0x00, 0x00, 0x1b};
  for (size_t index = 0; index < sizeof(fallback); index += 1) {
    assert(!defined_control_escape_byte(unspecified[index], &value));
    assert_control_escape_bytes(unspecified[index], &fallback[index], 1);
  }

  const uint8_t masked_two_byte[] = {0x03, 0xbf};
  assert_control_escape_bytes(0xff, masked_two_byte, sizeof(masked_two_byte));
  const uint8_t masked_four_byte[] = {0x10, 0x9f, 0x98, 0x80};
  assert_control_escape_bytes(
    0x1f600,
    masked_four_byte,
    sizeof(masked_four_byte)
  );

  struct ByteBuffer buffer = {0};
  assert(!append_control_escape(&buffer, -1));
  assert(buffer.length == 0);
  ts_free(buffer.data);
}

static bool line_matches_document(
  const uint8_t *delimiter,
  size_t delimiter_length,
  bool quoted,
  bool strip_tabs,
  const int32_t *line,
  size_t line_length
) {
  struct Scanner scanner = {0};
  assert(append_pending_document(
    &scanner,
    make_document_bytes(delimiter, delimiter_length, quoted, strip_tabs)
  ));
  assert(activate_startable_pending_documents(&scanner));
  struct MockLexer mock;
  init_mock_lexer(&mock, line, line_length);
  bool body_valid[TOKEN_COUNT] = {false};
  body_valid
    [quoted ? QUOTED_HERE_DOCUMENT_BODY_START : HERE_DOCUMENT_BODY_START] =
      true;
  assert(
    tree_sitter_dash_external_scanner_scan(&scanner, &mock.lexer, body_valid)
  );
  assert(mock.mark == 0);
  init_mock_lexer(&mock, line, line_length);
  bool end_valid[TOKEN_COUNT] = {false};
  end_valid[quoted ? QUOTED_HERE_DOCUMENT_END_BEGIN : HERE_DOCUMENT_END_BEGIN] =
    true;
  bool result =
    tree_sitter_dash_external_scanner_scan(&scanner, &mock.lexer, end_valid);
  clear_scanner(&scanner);
  return result;
}

static void test_byte_delimiter_matching(void) {
  const uint8_t utf8_ff[] = {0xc3, 0xbf};
  const int32_t source_ff[] = {0xff, '\n'};
  assert(line_matches_document(
    utf8_ff,
    sizeof(utf8_ff),
    true,
    false,
    source_ff,
    ARRAY_LENGTH(source_ff)
  ));

  const uint8_t invalid_ff[] = {0xff};
  const int32_t mismatched_lines[][2] = {
    {0x07, '\n'},
    {0xff, '\n'},
    {0xfffd, '\n'},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(mismatched_lines); index += 1) {
    assert(!line_matches_document(
      invalid_ff,
      sizeof(invalid_ff),
      true,
      false,
      mismatched_lines[index],
      ARRAY_LENGTH(mismatched_lines[index])
    ));
  }

  const uint8_t truncated[] = {0xc3};
  assert(!line_matches_document(
    truncated,
    sizeof(truncated),
    true,
    false,
    source_ff,
    ARRAY_LENGTH(source_ff)
  ));
  const uint8_t overlong[] = {0xc0, 0xaf};
  const int32_t slash[] = {'/', '\n'};
  assert(!line_matches_document(
    overlong,
    sizeof(overlong),
    true,
    false,
    slash,
    ARRAY_LENGTH(slash)
  ));
  const uint8_t nul[] = {0};
  assert(!line_matches_document(nul, sizeof(nul), true, false, NULL, 0));
  const int32_t empty_line[] = {'\n'};
  assert(line_matches_document(NULL, 0, true, false, empty_line, 1));

  const uint8_t del[] = {0x7f};
  const int32_t del_line[] = {0x7f, '\n'};
  const int32_t unit_separator_line[] = {0x1f, '\n'};
  assert(line_matches_document(del, sizeof(del), true, false, del_line, 2));
  assert(!line_matches_document(
    del,
    sizeof(del),
    true,
    false,
    unit_separator_line,
    2
  ));

  const uint8_t joined[] = {'A', 'B'};
  const int32_t joined_line[] = {'A', '\\', '\n', 'B', '\n'};
  assert(!line_matches_document(
    joined,
    sizeof(joined),
    false,
    false,
    joined_line,
    ARRAY_LENGTH(joined_line)
  ));
  const int32_t continued_middle_tab[] = {'A', '\\', '\n', '\t', 'B', '\n'};
  assert(!line_matches_document(
    joined,
    sizeof(joined),
    false,
    true,
    continued_middle_tab,
    ARRAY_LENGTH(continued_middle_tab)
  ));
  const int32_t continued_leading_tab[] = {'\\', '\n', '\t', 'A', 'B', '\n'};
  assert(line_matches_document(
    joined,
    sizeof(joined),
    false,
    true,
    continued_leading_tab,
    ARRAY_LENGTH(continued_leading_tab)
  ));
  const uint8_t tabbed[] = {'X'};
  const int32_t tabbed_line[] = {'\t', '\t', 'X', '\n'};
  assert(line_matches_document(
    tabbed,
    sizeof(tabbed),
    true,
    true,
    tabbed_line,
    ARRAY_LENGTH(tabbed_line)
  ));

  const int32_t nul_suffix[] = {'A', 0, 'X', '\n'};
  const uint8_t a[] = {'A'};
  assert(!line_matches_document(
    a,
    sizeof(a),
    true,
    false,
    nul_suffix,
    ARRAY_LENGTH(nul_suffix)
  ));
}

static bool scan_delimiter_fixture(
  struct Scanner *scanner,
  const int32_t *input,
  size_t length
) {
  struct MockLexer mock;
  init_mock_lexer(&mock, input, length);
  bool valid_symbols[TOKEN_COUNT] = {false};
  valid_symbols[HERE_END_BEGIN] = true;
  bool result =
    tree_sitter_dash_external_scanner_scan(scanner, &mock.lexer, valid_symbols);
  if (result) {
    assert(mock.lexer.result_symbol == HERE_END_BEGIN);
    assert(mock.mark == 0);
  }
  return result;
}

static void assert_text_delimiter_fixture(
  struct Scanner *scanner,
  const char *input,
  const char *expected_delimiter,
  bool quoted
) {
  size_t length = strlen(input);
  assert(length > 0 && input[length - 1] == '\n');

  int32_t *characters = malloc(length * sizeof(int32_t));
  assert(characters != NULL);
  for (size_t index = 0; index < length; index += 1) {
    characters[index] = (unsigned char)input[index];
  }

  assert(scan_delimiter_fixture(scanner, characters, length));
  assert(scanner->pending_count == 1);
  assert_document(
    &scanner->pending_documents[0],
    expected_delimiter,
    quoted,
    false
  );

  free(characters);
  clear_scanner(scanner);
}

static void test_double_quoted_parameter_delimiters(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);

  const char *fixtures[][2] = {
    {"\"${x:-\"ab\"}\"\n", "${x:-ab}"},
    {"\"${x:-\"a}b\"}tail\"\n", "${x:-a}b}tail"},
    {"\"${x:-\"${y:-\"ab\"}\"}\"\n", "${x:-${y:-ab}}"},
    {"\"${x:-\"$(printf 'ab')\"}\"\n", "${x:-$(printf ab)}"},
    {"\"${x:-\"\"}\"\n", "${x:-}"},
    {"\"${x:-'EOF'}\"\n", "${x:-'EOF'}"},
    {"\"${x:-'}\"\n", "${x:-'}"},
    {"\"${x:-a\\qb}\"\n", "${x:-a\\qb}"},
    {"\"${x:-a\\}b}\"\n", "${x:-a}b}"},
    {"\"${x:-${y:-'EOF'}}\"\n", "${x:-${y:-'EOF'}}"},
    {"\"${x:-$(printf 'EOF')}\"\n", "${x:-$(printf EOF)}"},
    {"\"${x#'EOF'}\"\n", "${x#EOF}"},
    {"\"${12%%'EOF'}\"\n", "${12%%EOF}"},
    {"\"${?#'EOF'}\"\n", "${?#EOF}"},
    {"\"${x:-${y#'EOF'}'tail'}\"\n", "${x:-${y#EOF}'tail'}"},
    {"\"${#'x}\"\n", "${#'x}"},
    {"\"${#\\}}\"\n", "${#}}"},
    {"\"${'x}\"\n", "${x}"},
    {"\"${a:%'x}\"\n", "${a:%'x}"},
    {"\"${a:#\\x}\"\n", "${a:#\\x}"},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    assert_text_delimiter_fixture(
      scanner,
      fixtures[index][0],
      fixtures[index][1],
      true
    );
  }

  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_parameter_delimiter_bytes_survive_state_restore(void) {
  const struct {
    int32_t source[7];
    size_t source_length;
    uint8_t delimiter[8];
    size_t delimiter_length;
  } fixtures[] = {
    {{'$', '{', 0xe9, '}', '\n'}, 5, {'$', '{', 0xa9, '}'}, 4},
    {{'$', '{', 0x3042, '}', '\n'}, 5, {'$', '{', 0x81, 0x82, '}'}, 5},
    {{'$', '{', 0x1f600, '}', '\n'}, 5, {'$', '{', 0x9f, 0x98, 0x80, '}'}, 6},
    {{'$', '{', '#', 0xe9, '}', '\n'}, 6, {'$', '{', '#', 0xc3, 0xa9, '}'}, 6},
    {{'$', '{', 0x7f, '}', '\n'}, 5, {'$', '{', '}'}, 3},
    {{'$', '{', 0x80, '}', '\n'}, 5, {'$', '{', 0x80, '}'}, 4},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct Scanner scanner = {0};
    assert(scan_delimiter_fixture(
      &scanner,
      fixtures[index].source,
      fixtures[index].source_length
    ));
    char serialized[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, serialized);
    tree_sitter_dash_external_scanner_deserialize(&scanner, serialized, length);
    assert(scanner.pending_count == 1);
    assert_document_bytes(
      &scanner.pending_documents[0],
      fixtures[index].delimiter,
      fixtures[index].delimiter_length,
      false
    );
    clear_scanner(&scanner);
  }
}

static void test_double_quoted_substitution_delimiter_spelling(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  const char *fixtures[][2] = {
    {"\"$(printf 'ab')\"\n", "$(printf ab)"},
    {"\"$(printf \"ab\")\"\n", "$(printf ab)"},
    {"\"${x##'ab'}\"\n", "${x##ab}"},
    {"\"${x%'ab'}\"\n", "${x%ab}"},
    {"\"$(printf ')')\"\n", "$(printf ))"},
    {"\"$(printf $(printf 'ab'))\"\n", "$(printf $(printf ab))"},
    {"\"`printf 'ab'`\"\n", "`printf ab`"},
    {"\"$((1+$(printf '2')))\"\n", "$((1+$(printf 2)))"},
    {"pre\"$(printf 'ab')\"post\n", "pre$(printf ab)post"},
    {"\"$(printf 'ab')\"'tail'\n", "$(printf ab)tail"},
    {"\"$(printf 'ab')\"$(printf 'cd')\n", "$(printf ab)$(printf cd)"},
    {"$(printf 'ab')\n", "$(printf ab)"},
    {"${x#'ab'}\n", "${x#ab}"},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    assert_text_delimiter_fixture(
      scanner,
      fixtures[index][0],
      fixtures[index][1],
      true
    );
  }
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_arithmetic_delimiters_preserve_nested_quote_context(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);
  const struct {
    const char *source;
    const char *delimiter;
    bool quoted;
  } fixtures[] = {
    {"\"$((1+${x:-'2'}))\"\n", "$((1+${x:-'2'}))", true},
    {"$((1+${x:-'2'}))\n", "$((1+${x:-'2'}))", false},
    {"$((1+${x:-'}))\n", "$((1+${x:-'}))", false},
    {"$((1+${x:-a\\qb}))\n", "$((1+${x:-a\\qb}))", true},
    {"$((1+${x:-\"a\"}))\n", "$((1+${x:-a}))", true},
    {"$((1+${x:-a\\}b}))\n", "$((1+${x:-a}b}))", true},
    {"$((1+${x:-\\$y}))\n", "$((1+${x:-$y}))", true},
    {"$((1+${x#'2'}))\n", "$((1+${x#2}))", true},
    {"$((1+$((2+${x:-'3'}))))\n", "$((1+$((2+${x:-'3'}))))", false},
    {"$((1+$(printf '2')))\n", "$((1+$(printf 2)))", true},
    {"$((1+`printf \\\"2\\\"`))\n", "$((1+`printf \"2\"`))", true},
    {"$((\\\"a))\n", "$((\"a))", true},
    {"\"$((a\\\"b))\"\n", "$((a\"b))", true},
    {"`$((\\\"a))`\n", "`$((\"a))`", true},
    {"$((\\'a))\n", "$((\\'a))", true},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    assert_text_delimiter_fixture(
      scanner,
      fixtures[index].source,
      fixtures[index].delimiter,
      fixtures[index].quoted
    );
  }
  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_substitution_hash_delimiter_words(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);

  assert_text_delimiter_fixture(
    scanner,
    "`printf $x#tag END`\n",
    "`printf $x#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf ${x}#tag END`\n",
    "`printf ${x}#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf $(:)#tag END`\n",
    "`printf $(:)#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf $((1))#tag END`\n",
    "`printf $((1))#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf $'x'#tag END`\n",
    "`printf x#tag END`",
    true
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf \\`x\\`#tag END`\n",
    "`printf `x`#tag END`",
    true
  );

  assert_text_delimiter_fixture(
    scanner,
    "`printf ${x}\\\n#tag END`\n",
    "`printf ${x}#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf $(:)\\\n#tag END`\n",
    "`printf $(:)#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf $((1))\\\n#tag END`\n",
    "`printf $((1))#tag END`",
    false
  );
  assert_text_delimiter_fixture(
    scanner,
    "`printf \\`x\\`\\\n#tag END`\n",
    "`printf `x`#tag END`",
    true
  );

  assert_text_delimiter_fixture(
    scanner,
    "`ca\\\nse value in <<X) printf END;; esac`\n",
    "`case value in <<X) printf END;; esac`",
    false
  );

  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_dollar_single_quote_escape_units(void) {
  const struct {
    int32_t source[11];
    size_t length;
    size_t end;
    int32_t following;
  } cases[] = {
    {{'\\', 'x', '1', '2', '3', 'g'}, 6, 4, '3'},
    {{'\\', 'x', 'a', 'B', 'C', 'D', 'E', 'f', '\''}, 9, 4, 'C'},
    {{'\\', 'x', '4', '1', '\''}, 5, 4, '\''},
    {{'\\', 'x', 'F', 'g'}, 4, 3, 'g'},
    {{'\\', '1', '2', '3', '4'}, 5, 4, '4'},
    {{'\\', 'x', '1', '2', '3'}, 5, 4, '3'},
    {{'\\', 'u', '0', '0', '4', '1', '2'}, 7, 6, '2'},
    {{'\\', 'u', 'd', '8', '0', '0', 'x'}, 7, 6, 'x'},
    {{'\\', 'U', '0', '0', '1', '1', '0', '0', '0', '0', 'b'}, 11, 10, 'b'},
    {{'\\', 'c', '\\', 'q'}, 4, 3, 'q'},
    {{'\\', 'c', '\\', '\\', '\''}, 5, 4, '\''},
    {{'\\', 'c', 0xe9, 'b'}, 4, 3, 'b'},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct MockLexer mock;
    init_mock_lexer(&mock, cases[index].source, cases[index].length);
    assert(scan_dollar_single_quote_token(&mock.lexer));
    assert(mock.lexer.result_symbol == DOLLAR_SINGLE_QUOTE_ESCAPE);
    assert(mock.offset == cases[index].end);
    assert(mock.mark == cases[index].end);
    assert(mock.lexer.lookahead == cases[index].following);
    assert(mock_eof(&mock.lexer) == (cases[index].end == cases[index].length));
  }
}

static void
test_dollar_single_quote_delimiter_bytes_survive_state_restore(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);

  const struct {
    const char *source;
    uint8_t delimiter[4];
    size_t delimiter_length;
  } fixtures[] = {
    {"$'\\c?'\n", {0x7f}, 1},
    {"$'\\xC3\\xBF'\n", {0xc3, 0xbf}, 2},
    {"$'\\303\\277'\n", {0xc3, 0xbf}, 2},
    {"$'\\xC3'$'\\277'\n", {0xc3, 0xbf}, 2},
    {"$'\\xFF'\n", {0xff}, 1},
    {"$'\\377'\n", {0xff}, 1},
    {"$'\\xC3'\n", {0xc3}, 1},
    {"$'\\277'\n", {0xbf}, 1},
    {"a$'\\xC3'b\n", {'a', 0xc3, 'b'}, 3},
    {"$'\\xC0\\xAF'\n", {0xc0, 0xaf}, 2},
    {"$'\\xED\\xA0\\x80'\n", {0xed, 0xa0, 0x80}, 3},
    {"$'\\xF4\\x90\\x80\\x80'\n", {0xf4, 0x90, 0x80, 0x80}, 4},
    {"$'\\ud800'\n", {0xed, 0xa0, 0x80}, 3},
    {"$'E\\U00110000F'\n", {'E', 'F'}, 2},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    size_t length = strlen(fixtures[index].source);
    int32_t *input = malloc(length * sizeof(*input));
    assert(input != NULL);
    for (size_t offset = 0; offset < length; offset += 1) {
      input[offset] = (unsigned char)fixtures[index].source[offset];
    }
    assert(scan_delimiter_fixture(scanner, input, length));
    free(input);
    assert(scanner->pending_count == 1);
    assert_document_bytes(
      &scanner->pending_documents[0],
      fixtures[index].delimiter,
      fixtures[index].delimiter_length,
      true
    );

    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned state_length = snapshot_scanner(scanner, state);
    clear_scanner(scanner);
    tree_sitter_dash_external_scanner_deserialize(scanner, state, state_length);
    assert(scanner->pending_count == 1);
    assert_document_bytes(
      &scanner->pending_documents[0],
      fixtures[index].delimiter,
      fixtures[index].delimiter_length,
      true
    );
    assert_scanner_matches_snapshot(scanner, state, state_length);
    clear_scanner(scanner);
  }

  const int32_t control_operand_input[] = {
    '$',
    '\'',
    '\\',
    'c',
    0xe9,
    '\\',
    'c',
    '\\',
    'q',
    '\'',
    '\n',
  };
  assert(scan_delimiter_fixture(
    scanner,
    control_operand_input,
    ARRAY_LENGTH(control_operand_input)
  ));
  const uint8_t control_operand_bytes[] = {0x03, 0xa9, 0x1c, 'q'};
  assert_document_bytes(
    &scanner->pending_documents[0],
    control_operand_bytes,
    sizeof(control_operand_bytes),
    true
  );
  clear_scanner(scanner);

  const int32_t unspecified_control_input[] = {
    '$',
    '\'',
    '\\',
    'c',
    '@',
    '\\',
    'c',
    '`',
    '\\',
    'c',
    '{',
    '\\',
    'c',
    0xff,
    '\'',
    '\n',
  };
  assert(scan_delimiter_fixture(
    scanner,
    unspecified_control_input,
    ARRAY_LENGTH(unspecified_control_input)
  ));
  assert_document_bytes(&scanner->pending_documents[0], NULL, 0, true);

  tree_sitter_dash_external_scanner_destroy(scanner);
}

static void test_delimiter_scan_resource_rollback(void) {
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);

  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);
  size_t length = SCANNER_STATE_CAPACITY + 2;
  int32_t *input = malloc(length * sizeof(int32_t));
  assert(input != NULL);
  for (size_t index = 0; index + 1 < length; index += 1) {
    input[index] = 'x';
  }
  input[length - 1] = '\n';

  assert(!scan_delimiter_fixture(scanner, input, length));
  assert_scanner_matches_snapshot(scanner, before, before_length);
  free(input);
  tree_sitter_dash_external_scanner_destroy(scanner);
}

#ifdef TREE_SITTER_REUSE_ALLOCATOR
static void test_delimiter_allocation_failures_preserve_state(void) {
  const char delimiter[] = "$((1))"
                           "abcdefghijklmnopqrstuvwxyz"
                           "abcdefghijklmnopqrstuvwxyz"
                           "abcdefghijklmnopqrstuvwxyz";
  int32_t input[sizeof(delimiter)];
  for (size_t index = 0; index + 1 < sizeof(delimiter); index += 1) {
    input[index] = (unsigned char)delimiter[index];
  }
  input[sizeof(delimiter) - 1] = '\n';
  bool valid_symbols[TOKEN_COUNT] = {false};
  valid_symbols[HERE_END_STRIP_BEGIN] = true;
  size_t allocation_count = 0;
  for (size_t failure = 0; failure <= allocation_count; failure += 1) {
    assert(reuse_live_allocations == 0);
    struct Scanner scanner = {0};
    char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, before);
    struct MockLexer mock;
    init_mock_lexer(&mock, input, ARRAY_LENGTH(input));
    size_t calls = reuse_allocation_calls;
    if (failure > 0) {
      reuse_fail_allocation_call = calls + failure;
    }
    bool accepted = tree_sitter_dash_external_scanner_scan(
      &scanner,
      &mock.lexer,
      valid_symbols
    );
    if (failure == 0) {
      allocation_count = reuse_allocation_calls - calls;
      assert(allocation_count > 0);
      assert(accepted);
      assert(mock.lexer.result_symbol == HERE_END_STRIP_BEGIN);
      assert(scanner.pending_count == 1);
      assert_document(&scanner.pending_documents[0], delimiter, false, true);
    } else {
      assert(reuse_allocation_calls >= reuse_fail_allocation_call);
      reuse_fail_allocation_call = 0;
      assert(!accepted);
      assert_scanner_matches_snapshot(&scanner, before, length);
    }
    clear_scanner(&scanner);
    assert(reuse_live_allocations == 0);
  }
}

static void test_pending_activation_allocation_failures_preserve_state(void) {
  size_t allocation_count = 0;
  for (size_t failure = 0; failure <= allocation_count; failure += 1) {
    assert(reuse_live_allocations == 0);
    struct Scanner scanner = {0};
    assert(
      append_pending_document(&scanner, make_document("retained", false, false))
    );
    scanner.substitution_depth = 1;
    assert(
      append_pending_document(&scanner, make_document("suspended", true, false))
    );
    assert(activate_startable_pending_documents(&scanner));
    scanner.substitution_depth = 2;
    assert(
      append_pending_document(&scanner, make_document("activated", false, true))
    );
    char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, before);
    size_t calls = reuse_allocation_calls;
    if (failure > 0) {
      reuse_fail_allocation_call = calls + failure;
    }
    bool accepted = activate_startable_pending_documents(&scanner);
    if (failure == 0) {
      allocation_count = reuse_allocation_calls - calls;
      assert(allocation_count > 0);
      assert(accepted);
      assert(scanner.pending_count == 1);
      assert_document(&scanner.pending_documents[0], "retained", false, false);
      assert(scanner.active_frame.count == 1);
      assert_document(
        &scanner.active_frame.documents[0],
        "activated",
        false,
        true
      );
      assert(scanner.suspended_frame_count == 1);
      assert(scanner.suspended_frames[0].count == 1);
      assert_document(
        &scanner.suspended_frames[0].documents[0],
        "suspended",
        true,
        false
      );
    } else {
      assert(reuse_allocation_calls >= reuse_fail_allocation_call);
      reuse_fail_allocation_call = 0;
      assert(!accepted);
      assert_scanner_matches_snapshot(&scanner, before, length);
    }
    clear_scanner(&scanner);
    assert(reuse_live_allocations == 0);
  }
}

static void test_reuse_allocator_realloc_failure_rolls_back(void) {
  assert(reuse_live_allocations == 0);
  struct Scanner *scanner = tree_sitter_dash_external_scanner_create();
  assert(scanner != NULL);

  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(scanner, before);
  const int32_t input[] = {'x', '\n'};
  size_t realloc_calls_before = reuse_realloc_calls;

  reuse_fail_next_realloc = true;
  assert(!scan_delimiter_fixture(scanner, input, ARRAY_LENGTH(input)));
  assert(!reuse_fail_next_realloc);
  assert(reuse_realloc_calls == realloc_calls_before + 1);
  assert_scanner_matches_snapshot(scanner, before, before_length);
  assert(reuse_live_allocations == 1);

  tree_sitter_dash_external_scanner_destroy(scanner);
  assert(reuse_live_allocations == 0);
}

static void test_reuse_allocator_contract(void) {
  assert(reuse_malloc_calls > 0);
  assert(reuse_calloc_calls > 0);
  assert(reuse_realloc_calls > 0);
  assert(reuse_free_calls > 0);
  assert(reuse_live_allocations == 0);

  reuse_fail_next_calloc = true;
  assert(tree_sitter_dash_external_scanner_create() == NULL);
  assert(!reuse_fail_next_calloc);
  assert(reuse_live_allocations == 0);
}
#endif

static void assert_source_step(
  struct Scanner *scanner,
  const int32_t *input,
  size_t length,
  enum TokenType symbol,
  size_t mark
) {
  bool valid[TOKEN_COUNT] = {false};
  valid[symbol] = true;
  struct MockLexer lexer;
  init_mock_lexer(&lexer, input, length);
  assert(tree_sitter_dash_external_scanner_scan(scanner, &lexer.lexer, valid));
  assert(lexer.lexer.result_symbol == symbol);
  assert(lexer.mark == mark);
  char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned state_length = snapshot_scanner(scanner, state);
  tree_sitter_dash_external_scanner_deserialize(scanner, state, state_length);
  assert_scanner_matches_snapshot(scanner, state, state_length);
}

static void
test_separators_preserve_operator_identity_and_pending_documents(void) {
  const struct {
    const char *source;
    enum TokenType expected;
    enum TokenType piece;
    size_t width;
  } cases[] = {
    {";", PUNCT_SEMICOLON, PUNCT_SEMICOLON_CHARACTER, 1},
    {"; echo next\n", PUNCT_SEMICOLON, PUNCT_SEMICOLON_CHARACTER, 1},
    {";\nfi\n", PUNCT_SEMICOLON, PUNCT_SEMICOLON_CHARACTER, 1},
    {"; )\n", PUNCT_SEMICOLON, PUNCT_SEMICOLON_CHARACTER, 1},
    {"&", PUNCT_AMPERSAND, PUNCT_AMPERSAND_CHARACTER, 1},
    {"& # note\necho next\n", PUNCT_AMPERSAND, PUNCT_AMPERSAND_CHARACTER, 1},
    {"& )\n", PUNCT_AMPERSAND, PUNCT_AMPERSAND_CHARACTER, 1},
    {";;", DSEMI_BEGIN, DSEMI_BEGIN_PIECE, 2},
    {"&&", AND_IF_BEGIN, AND_IF_BEGIN_PIECE, 2},
    {";\\\n;", DSEMI_BEGIN, DSEMI_BEGIN_PIECE, 4},
    {"&\\\n&", AND_IF_BEGIN, AND_IF_BEGIN_PIECE, 4},
  };
  for (unsigned document = 0; document < 3; document += 1) {
    for (unsigned recovery = 0; recovery < 2; recovery += 1) {
      for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
        struct Scanner scanner = {0};
        assert(scanner_source_ready(&scanner));
        if (document != 0) {
          assert(append_pending_document(
            &scanner,
            make_document("END", false, false)
          ));
          if (document == 2) {
            assert(activate_startable_pending_documents(&scanner));
            assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
            assert(source_context_push(&scanner, COMMAND_OPEN));
            scanner.source.stages[1].at_line_start = false;
            scanner.substitution_depth = 1;
          }
        }
        int32_t input[32];
        size_t length = strlen(cases[index].source);
        assert(length <= ARRAY_LENGTH(input));
        for (size_t offset = 0; offset < length; offset += 1) {
          input[offset] = (unsigned char)cases[index].source[offset];
        }
        bool valid[TOKEN_COUNT];
        for (size_t symbol = 0; symbol < TOKEN_COUNT; symbol += 1) {
          valid[symbol] = recovery != 0;
        }
        valid[PUNCT_SEMICOLON] = true;
        valid[PUNCT_AMPERSAND] = true;
        char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
        unsigned before_length = snapshot_scanner(&scanner, before);
        struct MockLexer lexer;
        init_mock_lexer(&lexer, input, length);
        bool accepted =
          tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid);
        assert(accepted == (recovery != 0 || cases[index].width == 1));
        if (accepted) {
          assert(lexer.lexer.result_symbol == cases[index].expected);
          assert(lexer.mark == 0);
          char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
          unsigned state_length = snapshot_scanner(&scanner, state);
          tree_sitter_dash_external_scanner_deserialize(
            &scanner,
            state,
            state_length
          );
          if (cases[index].width == 4) {
            assert_source_step(&scanner, input, length, cases[index].piece, 1);
            assert_source_step(
              &scanner,
              input + 1,
              length - 1,
              LINE_CONTINUATION,
              1
            );
            assert_source_step(
              &scanner,
              input + 2,
              length - 2,
              REMOVED_NEWLINE,
              1
            );
            assert_source_step(
              &scanner,
              input + 3,
              length - 3,
              cases[index].piece,
              1
            );
          } else {
            assert_source_step(
              &scanner,
              input,
              length,
              cases[index].piece,
              cases[index].width
            );
          }
        }
        assert_scanner_matches_snapshot(&scanner, before, before_length);
        clear_scanner(&scanner);
      }
    }
  }
}

static void
test_logical_lexical_emission_keeps_individual_physical_pairs(void) {
  const int32_t input[] = {'f', 'o', '\\', '\n', '\\', '\n', 'o', ' '};
  struct Scanner scanner = {0};
  const struct {
    enum TokenType symbol;
    size_t mark;
  } steps[] = {
    {WORD_INITIAL_LITERAL_BEGIN, 0},
    {WORD_INITIAL_LITERAL_BEGIN_PIECE, 2},
    {LINE_CONTINUATION, 1},
    {REMOVED_NEWLINE, 1},
    {LINE_CONTINUATION, 1},
    {REMOVED_NEWLINE, 1},
    {WORD_INITIAL_LITERAL_BEGIN_PIECE, 1},
  };
  size_t offset = 0;
  for (size_t index = 0; index < ARRAY_LENGTH(steps); index += 1) {
    assert_source_step(
      &scanner,
      input + offset,
      ARRAY_LENGTH(input) - offset,
      steps[index].symbol,
      steps[index].mark
    );
    offset += steps[index].mark;
    if (offset == 7) {
      assert(!scanner.emission.active);
      assert(scanner.emission.remaining == 0);
      assert(scanner.emission.symbol == 0);
    }
  }
  assert(offset == 7);
  assert(!scanner.emission.active);
  assert(scanner.context_count == 0);
  clear_scanner(&scanner);
}

static void
test_word_entry_classifies_and_owns_source_in_normal_and_recovery_scans(void) {
  const struct {
    const char *source;
    enum TokenType symbol;
    enum TokenType piece;
    size_t remaining;
    size_t first_width;
  } fixtures[] = {
    {"echo ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      4,
      4},
    {"e\\\ncho ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_PIECE,
      6,
      1},
    {"esacx;",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      5,
      5},
    {"fi\\\nx;",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_PIECE,
      5,
      2},
    {"donex\n",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      5,
      5},
    {"thenx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      5,
      5},
    {"elifx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      5,
      5},
    {"elsex ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      5,
      5},
    {"dox ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      3,
      3},
    {"ifx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      3,
      3},
    {"inx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      3,
      3},
    {"forx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      4,
      4},
    {"casex ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      5,
      5},
    {"whilex ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      6,
      6},
    {"untilx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      6,
      6},
    {"{x ", WORD_INITIAL_LITERAL_BEGIN, WORD_INITIAL_LITERAL_BEGIN_WHOLE, 2, 2},
    {"}x ", WORD_INITIAL_LITERAL_BEGIN, WORD_INITIAL_LITERAL_BEGIN_WHOLE, 2, 2},
    {"}\\\nx ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_PIECE,
      4,
      1},
    {"!x ", WORD_INITIAL_LITERAL_BEGIN, WORD_INITIAL_LITERAL_BEGIN_WHOLE, 2, 2},
    {"'x' ", WORD_INITIAL_SQ_OPEN, WORD_INITIAL_SQ_OPEN_CHARACTER, 1, 1},
    {"\"x\" ", WORD_INITIAL_DQ_OPEN, WORD_INITIAL_DQ_OPEN_CHARACTER, 1, 1},
    {"${x} ",
      WORD_INITIAL_DOLLAR_EXPANSION_START,
      WORD_INITIAL_DOLLAR_EXPANSION_START_CHARACTER,
      1,
      1},
    {"$'x' ",
      WORD_INITIAL_DOLLAR_SQ_DOLLAR,
      WORD_INITIAL_DOLLAR_SQ_DOLLAR_CHARACTER,
      1,
      1},
    {"`x` ",
      WORD_INITIAL_BACKQUOTE_START,
      WORD_INITIAL_BACKQUOTE_START_CHARACTER,
      1,
      1},
    {"\\x ",
      WORD_INITIAL_ESCAPED_CHARACTER_BEGIN,
      WORD_INITIAL_ESCAPED_CHARACTER_BEGIN_PIECE,
      2,
      1},
    {"* ",
      WORD_INITIAL_PATTERN_STAR_BEGIN,
      WORD_INITIAL_PATTERN_STAR_BEGIN_PIECE,
      1,
      1},
    {"? ",
      WORD_INITIAL_PATTERN_QUESTION_BEGIN,
      WORD_INITIAL_PATTERN_QUESTION_BEGIN_PIECE,
      1,
      1},
    {"[ab] ",
      WORD_INITIAL_PATTERN_BRACKET_OPEN,
      WORD_INITIAL_PATTERN_BRACKET_OPEN_CHARACTER,
      1,
      1},
    {"[ab ",
      WORD_INITIAL_FALLBACK_LITERAL_BEGIN,
      WORD_INITIAL_FALLBACK_LITERAL_BEGIN_PIECE,
      3,
      3},
  };
  for (size_t recovery = 0; recovery < 2; recovery += 1) {
    for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
      int32_t input[16];
      size_t length = strlen(fixtures[index].source);
      assert(length <= ARRAY_LENGTH(input));
      for (size_t character = 0; character < length; character += 1) {
        input[character] = (unsigned char)fixtures[index].source[character];
      }
      struct Scanner scanner = {0};
      bool valid[TOKEN_COUNT] = {false};
      for (size_t symbol = 0; symbol < TOKEN_COUNT; symbol += 1) {
        valid[symbol] = recovery != 0 ||
          (symbol >= WORD_INITIAL_LITERAL_BEGIN &&
            symbol <= WORD_INITIAL_BACKQUOTE_START);
      }
      valid[WORD_INITIAL_LITERAL_BEGIN_WHOLE] = true;
      struct MockLexer lexer;
      init_mock_lexer(&lexer, input, length);
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == fixtures[index].symbol);
      assert(lexer.mark == 0);
      assert(scanner.emission.active);
      assert(scanner.emission.remaining == fixtures[index].remaining);
      assert(scanner.context_count == 0);
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned state_length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(
        &scanner,
        state,
        state_length
      );
      assert_scanner_matches_snapshot(&scanner, state, state_length);

      bool disabled[TOKEN_COUNT] = {false};
      disabled[fixtures[index].symbol] = true;
      init_mock_lexer(&lexer, input, length);
      assert(!tree_sitter_dash_external_scanner_scan(
        &scanner,
        &lexer.lexer,
        disabled
      ));
      assert_scanner_matches_snapshot(&scanner, state, state_length);

      bool recovery_literal =
        recovery != 0 && fixtures[index].symbol == WORD_INITIAL_LITERAL_BEGIN;
      enum TokenType piece = recovery_literal ? WORD_INITIAL_LITERAL_BEGIN_WHOLE
                                              : fixtures[index].piece;
      size_t first_width = recovery_literal ? fixtures[index].remaining
                                            : fixtures[index].first_width;
      valid[piece] = true;
      init_mock_lexer(&lexer, input, length);
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == piece);
      assert(lexer.mark == first_width);
      assert(
        scanner.emission.remaining == fixtures[index].remaining - lexer.mark
      );
      state_length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(
        &scanner,
        state,
        state_length
      );
      assert_scanner_matches_snapshot(&scanner, state, state_length);
      clear_scanner(&scanner);
    }
  }
}

static void
test_whole_source_requires_its_classification_and_nonempty_range(void) {
  const struct {
    const char *source;
    enum TokenType begin;
    enum TokenType whole;
    enum TokenType piece;
    size_t width;
  } fixtures[] = {
    {"echo ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      WORD_INITIAL_LITERAL_BEGIN_PIECE,
      4},
    {"word\\\n next",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_WHOLE,
      WORD_INITIAL_LITERAL_BEGIN_PIECE,
      4},
    {"fi;", FI_KEYWORD, FI_KEYWORD_BEGIN_WHOLE, FI_KEYWORD_BEGIN_PIECE, 2},
    {"fi\\\n;", FI_KEYWORD, FI_KEYWORD_BEGIN_WHOLE, FI_KEYWORD_BEGIN_PIECE, 2},
    {":-word",
      PARAMETER_VALUE_OPERATOR_BEGIN,
      PARAMETER_VALUE_OPERATOR_BEGIN_WHOLE,
      PARAMETER_VALUE_OPERATOR_BEGIN_PIECE,
      2},
    {"%%word",
      PARAMETER_PATTERN_OPERATOR_BEGIN,
      PARAMETER_PATTERN_OPERATOR_BEGIN_WHOLE,
      PARAMETER_PATTERN_OPERATOR_BEGIN_PIECE,
      2},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    const size_t length = strlen(fixtures[index].source);
    int32_t source[16];
    assert(length <= ARRAY_LENGTH(source));
    for (size_t offset = 0; offset < length; offset += 1) {
      source[offset] = (unsigned char)fixtures[index].source[offset];
    }
    struct Scanner scanner = {0};
    bool valid[TOKEN_COUNT] = {false};
    valid[fixtures[index].whole] = true;
    struct MockLexer lexer;
    init_mock_lexer(&lexer, source, length);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(!scanner.emission.active);
    valid[fixtures[index].begin] = true;
    init_mock_lexer(&lexer, source, 0);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    init_mock_lexer(&lexer, source, length);
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == fixtures[index].begin);
    assert(lexer.mark == 0);
    assert(scanner.emission.symbol == fixtures[index].whole);
    assert(scanner.emission.remaining == fixtures[index].width);
    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned state_length = snapshot_scanner(&scanner, state);
    tree_sitter_dash_external_scanner_deserialize(
      &scanner,
      state,
      state_length
    );
    assert_scanner_matches_snapshot(&scanner, state, state_length);

    valid[fixtures[index].whole] = false;
    valid[fixtures[index].piece] = true;
    init_mock_lexer(&lexer, source, length);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert_scanner_matches_snapshot(&scanner, state, state_length);
    valid[fixtures[index].whole] = true;
    init_mock_lexer(&lexer, source, fixtures[index].width - 1);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert_scanner_matches_snapshot(&scanner, state, state_length);
    init_mock_lexer(&lexer, source, length);
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == fixtures[index].whole);
    assert(lexer.mark == fixtures[index].width);
    assert(!scanner.emission.active);
    assert(scanner.context_count == 0);
    clear_scanner(&scanner);
  }
}

static void test_tilde_word_entry_uses_the_grammar_owned_lexical_context(void) {
  const int32_t source[] = {'~', 'h', 'o', 'm', 'e', ' '};
  const struct {
    bool tilde;
    bool recovery;
    enum TokenType symbol;
    enum TokenType emission;
    size_t remaining;
  } fixtures[] = {
    {false, false, WORD_INITIAL_LITERAL_BEGIN, WORD_INITIAL_LITERAL_BEGIN, 5},
    {true, false, WORD_TILDE_START, WORD_TILDE_START, 1},
    {true, true, WORD_TILDE_START, WORD_TILDE_START, 1},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
    struct Scanner scanner = {0};
    bool valid[TOKEN_COUNT] = {false};
    for (size_t symbol = 0; symbol < TOKEN_COUNT; symbol += 1) {
      valid[symbol] = fixtures[index].recovery;
    }
    valid[WORD_INITIAL_LITERAL_BEGIN] = true;
    valid[WORD_TILDE_START] = fixtures[index].tilde;
    struct MockLexer lexer;
    init_mock_lexer(&lexer, source, ARRAY_LENGTH(source));
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(lexer.lexer.result_symbol == fixtures[index].symbol);
    assert(lexer.mark == 0);
    assert(scanner.emission.symbol == fixtures[index].emission);
    assert(scanner.emission.remaining == fixtures[index].remaining);
    assert(scanner.context_count == 0);
    clear_scanner(&scanner);
  }
}

static void test_pending_emission_rejects_disabled_and_stale_source(void) {
  const int32_t source[] = {'x', '\\', '\n', 'y', ' '};
  struct Scanner scanner = {0};
  assert_source_step(&scanner, source, 5, LITERAL_BEGIN, 0);
  assert_source_step(&scanner, source, 5, LITERAL_BEGIN_PIECE, 1);
  assert_source_step(&scanner, source + 1, 4, LINE_CONTINUATION, 1);
  assert(scanner.emission.removed_newline);
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(&scanner, before);
  const int32_t stale[] = {'x', 'y', ' '};
  const struct {
    const int32_t *input;
    size_t length;
    enum TokenType valid;
  } rejected[] = {
    {source + 2, 3, LITERAL_BEGIN_PIECE},
    {source + 2, 3, LINE_CONTINUATION},
    {stale, 3, REMOVED_NEWLINE},
    {stale, 0, REMOVED_NEWLINE},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(rejected); index += 1) {
    bool valid[TOKEN_COUNT] = {false};
    valid[rejected[index].valid] = true;
    struct MockLexer lexer;
    init_mock_lexer(&lexer, rejected[index].input, rejected[index].length);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert_scanner_matches_snapshot(&scanner, before, before_length);
  }
  assert_source_step(&scanner, source + 2, 3, REMOVED_NEWLINE, 1);
  assert_source_step(&scanner, source + 3, 2, LITERAL_BEGIN_PIECE, 1);
  assert(!scanner.emission.active);
  clear_scanner(&scanner);
}

static void test_lexical_source_rejects_another_begin_after_restore(void) {
  const struct {
    const char *source;
    enum TokenType begin;
    enum TokenType piece;
    enum TokenType other;
    size_t width;
    size_t contexts;
  } fixtures[] = {
    {"fi ",
      FI_KEYWORD,
      FI_KEYWORD_BEGIN_PIECE,
      LOGICAL_BLANK_BEGIN_PIECE,
      2,
      0},
    {")file",
      PUNCT_RIGHT_PARENTHESIS,
      PUNCT_RIGHT_PARENTHESIS_CHARACTER,
      PUNCT_GREATER_CHARACTER,
      1,
      0},
    {"echo ",
      WORD_INITIAL_LITERAL_BEGIN,
      WORD_INITIAL_LITERAL_BEGIN_PIECE,
      LITERAL_BEGIN_PIECE,
      4,
      0},
    {"value ",
      ASSIGNMENT_LITERAL_BEGIN,
      ASSIGNMENT_LITERAL_BEGIN_PIECE,
      PARAMETER_LITERAL_BEGIN_PIECE,
      5,
      0},
    {"'x'",
      WORD_INITIAL_SQ_OPEN,
      WORD_INITIAL_SQ_OPEN_CHARACTER,
      SQ_CLOSE_CHARACTER,
      1,
      1},
  };
  for (unsigned recovery = 0; recovery < 2; recovery += 1) {
    for (size_t index = 0; index < ARRAY_LENGTH(fixtures); index += 1) {
      int32_t source[8];
      size_t length = strlen(fixtures[index].source);
      assert(length <= ARRAY_LENGTH(source));
      for (size_t offset = 0; offset < length; offset += 1) {
        source[offset] = (unsigned char)fixtures[index].source[offset];
      }
      struct Scanner scanner = {0};
      assert_source_step(&scanner, source, length, fixtures[index].begin, 0);
      assert(scanner.emission.symbol == fixtures[index].begin);
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned state_length = snapshot_scanner(&scanner, state);
      bool valid[TOKEN_COUNT] = {false};
      valid[fixtures[index].other] = true;
      struct MockLexer lexer;
      init_mock_lexer(&lexer, source, length);
      assert(
        !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert_scanner_matches_snapshot(&scanner, state, state_length);
      for (size_t token = 0; token < TOKEN_COUNT; token += 1) {
        valid[token] = recovery != 0;
      }
      valid[fixtures[index].piece] = true;
      init_mock_lexer(&lexer, source, length);
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == fixtures[index].piece);
      assert(lexer.mark == fixtures[index].width);
      assert(!scanner.emission.active);
      assert(scanner.context_count == fixtures[index].contexts);
      clear_scanner(&scanner);
    }
  }
}

static void test_physical_prefix_and_character_keep_the_same_begin(void) {
  const int32_t source[] = {'\\', '$', 'x'};
  struct Scanner scanner = {0};
  assert(scanner_source_ready(&scanner));
  assert(source_context_push(&scanner, BACKQUOTE_START));
  assert_source_step(
    &scanner,
    source,
    3,
    WORD_INITIAL_DOLLAR_EXPANSION_START,
    0
  );
  assert(scanner.emission.remaining == 2);
  const struct {
    enum TokenType source;
    enum TokenType other;
  } pieces[] = {
    {WORD_INITIAL_DOLLAR_EXPANSION_START_PREFIX,
      WORD_INITIAL_DOLLAR_SQ_DOLLAR_PREFIX},
    {WORD_INITIAL_DOLLAR_EXPANSION_START_CHARACTER,
      WORD_INITIAL_DOLLAR_SQ_DOLLAR_CHARACTER},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(pieces); index += 1) {
    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned state_length = snapshot_scanner(&scanner, state);
    bool valid[TOKEN_COUNT] = {false};
    valid[pieces[index].other] = true;
    struct MockLexer lexer;
    init_mock_lexer(&lexer, source + index, 3 - index);
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert_scanner_matches_snapshot(&scanner, state, state_length);
    assert_source_step(
      &scanner,
      source + index,
      3 - index,
      pieces[index].source,
      1
    );
    assert(scanner.context_count == 1);
    assert(scanner.contexts[0].opener == BACKQUOTE_START);
  }
  assert(!scanner.emission.active);
  clear_scanner(&scanner);
}

static void test_saved_source_requires_a_begin_token(void) {
  const int32_t source[] = {'e', 'c', 'h', 'o', ' '};
  const enum TokenType owners[] = {
    HERE_DOCUMENT_SEQUENCE_END,
    WORD_INITIAL_LITERAL_BEGIN_PIECE,
    PUNCT_RIGHT_PARENTHESIS_PREFIX,
  };
  struct Scanner scanner = {0};
  assert_source_step(&scanner, source, 5, WORD_INITIAL_LITERAL_BEGIN, 0);
  for (size_t index = 0; index < ARRAY_LENGTH(owners); index += 1) {
    scanner.emission.symbol = owners[index];
    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned state_length = snapshot_scanner(&scanner, state);
    struct Scanner restored = {0};
    tree_sitter_dash_external_scanner_deserialize(
      &restored,
      state,
      state_length
    );
    assert(!restored.emission.active);
    assert(restored.source.stage_count == 0);
    assert(restored.context_count == 0);
    clear_scanner(&restored);
  }
  clear_scanner(&scanner);
}

static void test_quote_context_commits_after_physical_punctuation(void) {
  const int32_t source[] = {'\'', 'a', '\\', '\n', 'b', '\'', ' '};
  struct Scanner scanner = {0};
  assert_source_step(&scanner, source, 7, WORD_INITIAL_SQ_OPEN, 0);
  assert(scanner.context_count == 0);
  assert_source_step(&scanner, source, 7, WORD_INITIAL_SQ_OPEN_CHARACTER, 1);
  assert(scanner.context_count == 1);
  assert(scanner.source.stages[0].disabled);
  assert_source_step(&scanner, source + 1, 6, SINGLE_QUOTE_CONTENT_BEGIN, 0);
  assert_source_step(
    &scanner,
    source + 1,
    6,
    SINGLE_QUOTE_CONTENT_BEGIN_PIECE,
    1
  );
  assert_source_step(
    &scanner,
    source + 2,
    5,
    SINGLE_QUOTE_CONTENT_BEGIN_PIECE,
    1
  );
  assert_source_step(
    &scanner,
    source + 3,
    4,
    SINGLE_QUOTE_CONTENT_BEGIN_PIECE,
    2
  );
  assert(!scanner.emission.active);
  assert_source_step(&scanner, source + 5, 2, SQ_CLOSE, 0);
  assert(scanner.context_count == 1);
  assert_source_step(&scanner, source + 5, 2, SQ_CLOSE_CHARACTER, 1);
  assert(scanner.context_count == 0);
  assert(!scanner.source.stages[0].disabled);
  clear_scanner(&scanner);
}

static void
test_arithmetic_classification_restores_the_enclosing_context(void) {
  const struct {
    int32_t source[4];
    enum TokenType symbol;
  } cases[] = {
    {{'(', '1', ')', ')'}, ARITHMETIC_LEFT_PARENTHESIS},
    {{'(', '@', ')', ')'}, ARITHMETIC_SOURCE_LEFT_PARENTHESIS},
  };
  const enum TokenType parents[] = {DQ_OPEN, SQ_OPEN, COMMAND_OPEN};
  for (size_t parent = 0; parent < ARRAY_LENGTH(parents); parent += 1) {
    for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
      struct Scanner scanner = {0};
      assert(scanner_source_ready(&scanner));
      assert(source_context_push(&scanner, parents[parent]));
      char enclosing[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned enclosing_length = snapshot_scanner(&scanner, enclosing);
      assert(source_context_push(&scanner, COMMAND_OPEN));
      char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned before_length = snapshot_scanner(&scanner, before);
      bool valid[TOKEN_COUNT] = {false};
      valid[COMMAND_SUBSTITUTION_BODY_BEGIN] = true;
      valid[ARITHMETIC_LEFT_PARENTHESIS] =
        cases[index].symbol != ARITHMETIC_LEFT_PARENTHESIS;
      valid[ARITHMETIC_SOURCE_LEFT_PARENTHESIS] =
        cases[index].symbol != ARITHMETIC_SOURCE_LEFT_PARENTHESIS;
      struct MockLexer lexer;
      init_mock_lexer(
        &lexer,
        cases[index].source,
        ARRAY_LENGTH(cases[index].source)
      );
      assert(
        !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert_scanner_matches_snapshot(&scanner, before, before_length);

      valid[cases[index].symbol] = true;
      init_mock_lexer(
        &lexer,
        cases[index].source,
        ARRAY_LENGTH(cases[index].source)
      );
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == cases[index].symbol);
      assert(lexer.mark == 0);
      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
      assert_scanner_matches_snapshot(&scanner, state, length);
      assert(scanner.context_count == 2);
      assert(top_context_opener(&scanner) == cases[index].symbol);
      assert(!scanner.source.stages[0].disabled);

      assert_source_step(&scanner, NULL, 0, ARITHMETIC_EXPANSION_END, 0);
      assert_scanner_matches_snapshot(&scanner, enclosing, enclosing_length);
      clear_scanner(&scanner);
    }
  }
}

static void test_pending_backquote_tokens_round_trip_and_consume_once(void) {
  const struct {
    int32_t source[2];
    size_t length;
    enum BackquoteToken token;
  } cases[] = {
    {{')', 0}, 1, BACKQUOTE_TOKEN_RIGHT_PARENTHESIS},
    {{';', ';'}, 2, BACKQUOTE_TOKEN_STOP},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(source_context_push(&scanner, BACKQUOTE_START));
    assert(append_pending_document(&scanner, make_document("A", false, false)));
    assert_source_step(
      &scanner,
      cases[index].source,
      cases[index].length,
      BACKQUOTE_REMAINDER_BEGIN,
      0
    );
    assert(scanner.contexts[0].backquote_token == cases[index].token);
    assert_source_step(
      &scanner,
      cases[index].source,
      cases[index].length,
      BACKQUOTE_REMAINDER_BEGIN_PIECE,
      cases[index].length
    );
    assert(activate_startable_pending_documents(&scanner));
    assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
    assert(source_context_push(&scanner, PARAMETER_OPEN));
    assert(source_context_push(&scanner, COMMAND_OPEN));

    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, state);
    tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
    assert_scanner_matches_snapshot(&scanner, state, length);
    assert(scanner.contexts[0].backquote_token == cases[index].token);
    const int32_t body[] = {'x', ')', '\n'};
    struct MockLexer lexer;
    bool valid[TOKEN_COUNT] = {false};
    valid[COMMAND_SUBSTITUTION_BODY_BEGIN] = true;
    init_mock_lexer(&lexer, body, ARRAY_LENGTH(body));
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert_scanner_matches_snapshot(&scanner, state, length);

    valid[COMMAND_SUBSTITUTION_REMAINDER_END] = true;
    init_mock_lexer(&lexer, body, ARRAY_LENGTH(body));
    bool accepted =
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid);
    assert(
      accepted == (cases[index].token == BACKQUOTE_TOKEN_RIGHT_PARENTHESIS)
    );
    if (accepted) {
      assert(lexer.lexer.result_symbol == COMMAND_SUBSTITUTION_REMAINDER_END);
      assert(lexer.mark == 0);
      assert(scanner.context_count == 3);
      assert(scanner.substitution_depth == 0);
      assert(scanner.contexts[0].backquote_token == BACKQUOTE_TOKEN_NONE);
      assert(source_context_push(&scanner, COMMAND_OPEN));
      init_mock_lexer(&lexer, body, ARRAY_LENGTH(body));
      assert(
        tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      assert(lexer.lexer.result_symbol == COMMAND_SUBSTITUTION_BODY_BEGIN);
      assert(scanner.substitution_depth == 1);
    } else {
      assert_scanner_matches_snapshot(&scanner, state, length);
    }
    clear_scanner(&scanner);
  }
}

static void
test_multiline_end_text_keeps_internal_tabs_after_each_restore(void) {
  const int32_t source[] = {'a', '\n', '\t', 'b', '\n'};
  for (unsigned quoted = 0; quoted < 2; quoted += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(append_pending_document(
      &scanner,
      make_document("a\n\tb", quoted != 0, true)
    ));
    assert(activate_startable_pending_documents(&scanner));
    assert(source_context_push(
      &scanner,
      quoted ? QUOTED_HERE_DOCUMENT_BODY_START : HERE_DOCUMENT_BODY_START
    ));
    assert_source_step(
      &scanner,
      source,
      5,
      quoted ? QUOTED_HERE_DOCUMENT_END_BEGIN : HERE_DOCUMENT_END_BEGIN,
      0
    );
    assert_source_step(
      &scanner,
      source,
      5,
      quoted ? QUOTED_HERE_DOCUMENT_END_TEXT : HERE_DOCUMENT_END_TEXT_BEGIN,
      0
    );
    assert(scanner.active_frame.documents[0].end_text_consumed);
    enum TokenType piece = quoted ? QUOTED_HERE_DOCUMENT_END_TEXT_BEGIN_PIECE
                                  : HERE_DOCUMENT_END_TEXT_BEGIN_PIECE;
    assert_source_step(&scanner, source, 5, piece, 1);
    assert_source_step(&scanner, source + 1, 4, piece, 3);
    assert_source_step(&scanner, source + 4, 1, HERE_DOCUMENT_END_LINE_END, 0);
    assert_source_step(
      &scanner,
      source + 4,
      1,
      HERE_DOCUMENT_END_LINE_END_BEGIN_PIECE,
      1
    );
    assert_source_step(&scanner, NULL, 0, HERE_DOCUMENT_END_COMMIT, 0);
    assert(scanner.active_frame.count == 0);
    assert(scanner.context_count == 0);
    clear_scanner(&scanner);
  }
}

static void test_retained_command_headers_restore_and_consume_once(void) {
  const struct {
    enum BackquoteToken token;
    enum TokenType expected;
    const char *source;
  } cases[] = {
    {BACKQUOTE_TOKEN_WORD, RETAINED_WORD_BEGIN, "next"},
    {BACKQUOTE_TOKEN_NAME_WORD, RETAINED_WORD_BEGIN, "next"},
    {BACKQUOTE_TOKEN_NAME_WORD, RETAINED_FUNCTION_NAME, " () { :; }"},
    {BACKQUOTE_TOKEN_ASSIGNMENT, RETAINED_ASSIGNMENT_BEGIN, "next"},
    {BACKQUOTE_TOKEN_DECLARATION, RETAINED_DECLARATION_BEGIN, "x=value"},
    {BACKQUOTE_TOKEN_FORWARDING, RETAINED_FORWARDING_BEGIN, "export"},
    {BACKQUOTE_TOKEN_NAME_FORWARDING, RETAINED_FORWARDING_BEGIN, "export"},
    {BACKQUOTE_TOKEN_NAME_FORWARDING, RETAINED_FUNCTION_NAME, " () { :; }"},
    {BACKQUOTE_TOKEN_LEFT_PARENTHESIS, RETAINED_LEFT_PARENTHESIS, "next)"},
    {BACKQUOTE_TOKEN_IF, RETAINED_IF, ":; then :; fi"},
    {BACKQUOTE_TOKEN_FOR, RETAINED_FOR, "x; do :; done"},
    {BACKQUOTE_TOKEN_CASE, RETAINED_CASE, "x in x) :;; esac"},
    {BACKQUOTE_TOKEN_WHILE, RETAINED_WHILE, ":; do :; done"},
    {BACKQUOTE_TOKEN_UNTIL, RETAINED_UNTIL, ":; do :; done"},
    {BACKQUOTE_TOKEN_BRACE, RETAINED_BRACE, ":; }"},
    {BACKQUOTE_TOKEN_NEGATION, RETAINED_NEGATION, "next"},
  };
  for (size_t index = 0; index < ARRAY_LENGTH(cases); index += 1) {
    for (unsigned modern = 0; modern < 2; modern += 1) {
      struct Scanner scanner = {0};
      assert(scanner_source_ready(&scanner));
      assert(source_context_push(&scanner, BACKQUOTE_START));
      assert(
        append_pending_document(&scanner, make_document("A", false, false))
      );
      assert(activate_startable_pending_documents(&scanner));
      assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
      assert(
        source_context_push(&scanner, modern ? COMMAND_OPEN : BACKQUOTE_START)
      );
      scanner.contexts[0].backquote_token = cases[index].token;
      if (modern)
        assert_source_step(
          &scanner,
          NULL,
          0,
          COMMAND_SUBSTITUTION_BODY_BEGIN,
          0
        );

      char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
      unsigned length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
      assert_scanner_matches_snapshot(&scanner, state, length);
      bool valid[TOKEN_COUNT] = {false};
      valid[WORD_INITIAL_LITERAL_BEGIN] = true;
      assert_boundary_scan(&scanner, cases[index].source, valid, TOKEN_COUNT);
      assert_scanner_matches_snapshot(&scanner, state, length);
      int32_t source[32];
      size_t source_length = strlen(cases[index].source);
      assert(source_length <= ARRAY_LENGTH(source));
      for (size_t offset = 0; offset < source_length; offset += 1)
        source[offset] = cases[index].source[offset];
      assert_source_step(
        &scanner,
        source,
        source_length,
        cases[index].expected,
        0
      );
      assert(scanner.contexts[0].backquote_token == BACKQUOTE_TOKEN_NONE);
      length = snapshot_scanner(&scanner, state);
      tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
      assert_scanner_matches_snapshot(&scanner, state, length);
      assert(source_context_pop(&scanner));
      assert(source_context_pop(&scanner));
      assert(scanner.contexts[0].backquote_token == BACKQUOTE_TOKEN_NONE);
      clear_scanner(&scanner);
    }
  }
}

static void
test_invalid_retained_token_rejects_recovery_without_consumption(void) {
  for (unsigned modern = 0; modern < 2; modern += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(source_context_push(&scanner, BACKQUOTE_START));
    assert(append_pending_document(&scanner, make_document("A", false, false)));
    assert(activate_startable_pending_documents(&scanner));
    assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
    assert(
      source_context_push(&scanner, modern ? COMMAND_OPEN : BACKQUOTE_START)
    );
    scanner.contexts[0].backquote_token = BACKQUOTE_TOKEN_INVALID;

    char state[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, state);
    tree_sitter_dash_external_scanner_deserialize(&scanner, state, length);
    assert_scanner_matches_snapshot(&scanner, state, length);
    for (unsigned recovery = 0; recovery < 2; recovery += 1) {
      bool valid[TOKEN_COUNT];
      for (size_t token = 0; token < TOKEN_COUNT; token += 1)
        valid[token] = recovery != 0;
      valid[WORD_INITIAL_LITERAL_BEGIN] = modern == 0 || recovery != 0;
      valid[BACKQUOTE_END] = modern == 0 || recovery != 0;
      valid[COMMAND_SUBSTITUTION_BODY_BEGIN] = modern != 0 || recovery != 0;
      const char *inputs[] = {"", "`", "next", "v=X"};
      for (size_t index = 0; index < ARRAY_LENGTH(inputs); index += 1)
        assert_boundary_scan(&scanner, inputs[index], valid, TOKEN_COUNT);
    }
    clear_scanner(&scanner);
  }
}

static void test_retained_quoted_word_prefix_survives_partial_scans(void) {
  for (unsigned newline = 0; newline < 2; newline += 1) {
    struct Scanner scanner = {0};
    assert(scanner_source_ready(&scanner));
    assert(source_context_push(&scanner, BACKQUOTE_START));
    scanner.contexts[0].backquote_token = BACKQUOTE_TOKEN_STOP;
    assert(append_pending_document(&scanner, make_document("A", true, false)));
    assert(activate_startable_pending_documents(&scanner));
    assert(source_context_push(&scanner, QUOTED_HERE_DOCUMENT_BODY_START));
    const int32_t name[] = {'n', 'a', 'm', 'e'};
    assert_source_step(&scanner, name, 4, QUOTED_HERE_DOCUMENT_TEXT, 0);
    assert_source_step(
      &scanner,
      name,
      4,
      QUOTED_HERE_DOCUMENT_TEXT_BEGIN_PIECE,
      4
    );
    assert(top_context(&scanner)->here_word == HERE_WORD_NAME);
    if (newline) {
      const int32_t line[] = {'\n'};
      assert_source_step(&scanner, line, 1, NEWLINE, 0);
      assert_source_step(&scanner, line, 1, NEWLINE_BEGIN_PIECE, 1);
      assert(top_context(&scanner)->here_word == HERE_WORD_OTHER);
    }
    const int32_t value[] = {'=', 'v'};
    assert_source_step(&scanner, value, 2, QUOTED_HERE_DOCUMENT_TEXT, 0);
    assert_source_step(
      &scanner,
      value,
      2,
      QUOTED_HERE_DOCUMENT_TEXT_BEGIN_PIECE,
      2
    );
    assert(source_context_pop(&scanner));
    assert(
      scanner.contexts[0].backquote_token ==
      (newline ? BACKQUOTE_TOKEN_WORD : BACKQUOTE_TOKEN_ASSIGNMENT)
    );
    clear_scanner(&scanner);
  }
}

static void test_source_context_growth_at_capacity_preserves_state(void) {
  const int32_t source[] = {'`', 'x'};
  struct Scanner scanner = {0};
  assert_source_step(&scanner, source, 2, BACKQUOTE_START, 0);
  assert(append_pending_document(
    &scanner,
    make_repeated_document(initial_delimiter_bytes)
  ));
  fill_pending_document_to_capacity(&scanner, 0);
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned length = snapshot_scanner(&scanner, before);
  bool valid[TOKEN_COUNT] = {false};
  valid[BACKQUOTE_START_CHARACTER] = true;
  struct MockLexer lexer;
  init_mock_lexer(&lexer, source, 2);
  assert(
    !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
  );
  assert_scanner_matches_snapshot(&scanner, before, length);
  clear_scanner(&scanner);
}

#ifdef TREE_SITTER_REUSE_ALLOCATOR
static void test_embedded_here_document_read_failures_terminate(void) {
  assert(reuse_live_allocations == 0);
  size_t allocation_count = 0;
  for (size_t failure = 0; failure <= allocation_count; failure += 1) {
    struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    struct SourceFixture fixture;
    init_source_fixture(&fixture, "body\nEND\ntail", &stage, 1);
    struct EmbeddedSkip skip = {0};
    assert(append_document(
      &skip.pending,
      &skip.pending_count,
      make_document("END", false, false)
    ));
    size_t first_allocation = reuse_allocation_calls;
    if (failure > 0) reuse_fail_allocation_call = first_allocation + failure;
    enum ArithmeticValidation result =
      skip_embedded_here_document_bodies(NULL, &fixture.lookahead.lexer, &skip);
    reuse_fail_allocation_call = 0;
    if (failure == 0) {
      allocation_count = reuse_allocation_calls - first_allocation;
      assert(allocation_count > 0);
      assert(result == ARITHMETIC_VALIDATION_VALID);
      assert(fixture.lookahead.lexer.lookahead == 't');
    } else {
      assert(result == ARITHMETIC_VALIDATION_RESOURCE_FAILURE);
      assert(fixture.native.failed);
    }
    assert(skip.pending_count == 0);
    clear_embedded_skip(&skip);
    clear_source_fixture(&fixture);
    assert(reuse_live_allocations == 0);
  }
}

static void test_source_callback_allocation_failures_are_transactional(void) {
  assert(reuse_live_allocations == 0);
  const int32_t source[] = {'f', 'o', '\\', '\n', 'o', ' '};
  const enum TokenType symbols[] = {
    WORD_INITIAL_LITERAL_BEGIN,
    WORD_INITIAL_LITERAL_BEGIN_PIECE,
    LINE_CONTINUATION,
    REMOVED_NEWLINE,
  };
  const size_t offsets[] = {0, 0, 2, 3};
  const size_t marks[] = {0, 2, 1, 1};
  for (size_t phase = 0; phase < ARRAY_LENGTH(symbols); phase += 1) {
    struct Scanner scanner = {0};
    for (size_t previous = 0; previous < phase; previous += 1) {
      assert_source_step(
        &scanner,
        source + offsets[previous],
        6 - offsets[previous],
        symbols[previous],
        marks[previous]
      );
    }
    char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned before_length = snapshot_scanner(&scanner, before);
    bool valid[TOKEN_COUNT] = {false};
    valid[symbols[phase]] = true;
    struct MockLexer lexer;
    init_mock_lexer(&lexer, source + offsets[phase], 6 - offsets[phase]);
    size_t first_allocation = reuse_allocation_calls;
    assert(
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    size_t allocation_count = reuse_allocation_calls - first_allocation;
    assert(allocation_count > 0);
    clear_scanner(&scanner);
    for (size_t failure = 1; failure <= allocation_count; failure += 1) {
      tree_sitter_dash_external_scanner_deserialize(
        &scanner,
        before,
        before_length
      );
      reuse_fail_allocation_call = reuse_allocation_calls + failure;
      init_mock_lexer(&lexer, source + offsets[phase], 6 - offsets[phase]);
      assert(
        !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
      );
      reuse_fail_allocation_call = 0;
      assert_scanner_matches_snapshot(&scanner, before, before_length);
      clear_scanner(&scanner);
      assert(reuse_live_allocations == 0);
    }
  }
}

static void
test_reserved_word_allocation_failures_do_not_fall_back_to_word(void) {
  const int32_t source[] = {'i', 'f', ' '};
  bool valid[TOKEN_COUNT] = {false};
  valid[IF_KEYWORD] = true;
  valid[WORD_INITIAL_LITERAL_BEGIN] = true;
  size_t allocation_count = 0;
  for (size_t failure = 0; failure <= allocation_count; failure += 1) {
    struct Scanner scanner = {0};
    char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
    unsigned length = snapshot_scanner(&scanner, before);
    struct MockLexer lexer;
    init_mock_lexer(&lexer, source, 3);
    size_t calls = reuse_allocation_calls;
    if (failure > 0) reuse_fail_allocation_call = calls + failure;
    bool accepted =
      tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid);
    reuse_fail_allocation_call = 0;
    if (failure == 0) {
      allocation_count = reuse_allocation_calls - calls;
      assert(allocation_count > 0);
      assert(accepted);
      assert(lexer.lexer.result_symbol == IF_KEYWORD);
      assert(lexer.mark == 0);
    } else {
      assert(!accepted);
      assert_scanner_matches_snapshot(&scanner, before, length);
    }
    clear_scanner(&scanner);
    assert(reuse_live_allocations == 0);
  }
}

static void test_comment_boundary_allocation_failures_terminate(void) {
  const int32_t source[] = {' ', '#', ' ', '`', '\n'};
  bool valid[TOKEN_COUNT] = {false};
  valid[TERM_CONTINUATION] = true;
  valid[TRAILING_COMMENT_BOUNDARY] = true;
  struct Scanner scanner = {0};
  assert(scanner_source_ready(&scanner));
  assert(source_context_push(&scanner, BACKQUOTE_START));
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(&scanner, before);
  struct MockLexer lexer;
  init_mock_lexer(&lexer, source, ARRAY_LENGTH(source));
  size_t first_allocation = reuse_allocation_calls;
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  size_t allocation_count = reuse_allocation_calls - first_allocation;
  assert(allocation_count > 0);
  assert(lexer.lexer.result_symbol == TRAILING_COMMENT_BOUNDARY);
  assert(lexer.mark == 0);
  clear_scanner(&scanner);
  assert(reuse_live_allocations == 0);
  for (size_t failure = 1; failure <= allocation_count; failure += 1) {
    tree_sitter_dash_external_scanner_deserialize(
      &scanner,
      before,
      before_length
    );
    init_mock_lexer(&lexer, source, ARRAY_LENGTH(source));
    reuse_fail_allocation_call = reuse_allocation_calls + failure;
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(reuse_allocation_calls >= reuse_fail_allocation_call);
    reuse_fail_allocation_call = 0;
    assert_scanner_matches_snapshot(&scanner, before, before_length);
    clear_scanner(&scanner);
    assert(reuse_live_allocations == 0);
  }
}

static void
test_document_boundary_allocation_failures_do_not_close_the_term(void) {
  const int32_t source[] =
    {' ', '\n', 'w', 'o', 'r', 'd', '\n', 'E', 'N', 'D', '\n'};
  bool valid[TOKEN_COUNT] = {false};
  valid[TERM_CONTINUATION] = true;
  valid[TERM_BOUNDARY] = true;
  valid[PRE_NEWLINE_BLANK] = true;
  struct Scanner scanner = {0};
  assert(scanner_source_ready(&scanner));
  assert(append_pending_document(&scanner, make_document("END", false, false)));
  assert(activate_startable_pending_documents(&scanner));
  assert(source_context_push(&scanner, HERE_DOCUMENT_BODY_START));
  assert(source_context_push(&scanner, COMMAND_OPEN));
  scanner.substitution_depth = 1;
  char before[TREE_SITTER_SERIALIZATION_BUFFER_SIZE];
  unsigned before_length = snapshot_scanner(&scanner, before);
  struct MockLexer lexer;
  init_mock_lexer(&lexer, source, ARRAY_LENGTH(source));
  size_t first_allocation = reuse_allocation_calls;
  assert(tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid));
  size_t allocation_count = reuse_allocation_calls - first_allocation;
  assert(allocation_count > 0);
  assert(lexer.lexer.result_symbol == TERM_CONTINUATION);
  assert(lexer.mark == 0);
  assert_scanner_matches_snapshot(&scanner, before, before_length);
  clear_scanner(&scanner);
  for (size_t failure = 1; failure <= allocation_count; failure += 1) {
    tree_sitter_dash_external_scanner_deserialize(
      &scanner,
      before,
      before_length
    );
    reuse_fail_allocation_call = reuse_allocation_calls + failure;
    init_mock_lexer(&lexer, source, ARRAY_LENGTH(source));
    assert(
      !tree_sitter_dash_external_scanner_scan(&scanner, &lexer.lexer, valid)
    );
    assert(reuse_allocation_calls >= reuse_fail_allocation_call);
    reuse_fail_allocation_call = 0;
    assert_scanner_matches_snapshot(&scanner, before, before_length);
    clear_scanner(&scanner);
    assert(reuse_live_allocations == 0);
  }
}
#endif

int main(void) {
  test_source_reading_omits_nul_and_preserves_scalar_origins();
  test_ancestor_removal_survives_local_quoting();
  test_disabled_here_document_stage_only_tracks_line_starts();
  test_backquote_source_records_units_and_boundaries();
  test_source_pass_order_controls_removal();
  test_adjacent_runs_and_bare_newlines_follow_each_pass();
  test_tab_stripping_follows_joined_line_starts();
  test_every_raw_cut_restores_pending_source_without_rereading();
  test_snapshot_capacity_and_invalid_data_are_transactional();
  test_logical_context_changes_reclassify_cached_lookahead();
  test_logical_backquote_closers_belong_to_the_innermost_active_view();
  test_logical_forks_preserve_ancestor_pending_units();
  test_logical_resume_does_not_repeat_already_emitted_source();
  test_source_reader_failure_releases_owned_memory();
  test_lookahead_replays_raw_cuts_in_distinct_quote_views();
  test_delimiter_source_views_preserve_quotedness_and_bytes();
  test_embedded_readers_apply_source_views_before_comments_and_closers();
  test_here_document_readers_keep_the_owning_source_view();
  test_here_document_readers_stop_at_backquote_boundaries();
  test_here_document_boundaries_suspend_only_for_command_substitutions();
  test_here_document_end_restores_its_source_and_serialized_depth();
  test_disabled_and_rejected_recovery_scans_preserve_state();
  test_recovery_emits_newlines_without_reclassifying_continuations();
  test_recovery_preserves_reserved_and_punctuation_token_identity();
  test_recovery_parameter_escapes_leave_the_closing_brace_available();
  test_parenthesis_recovery_defers_substitution_state_to_grammar();
  test_state_round_trip();
  test_equal_source_contexts_preserve_each_policy_restore();
  test_substitution_contexts_suspend_the_enclosing_body_stage();
  test_source_context_counts_round_trip_and_reject_overflow();
  test_bracket_chains_round_trip_and_reject_unknown_phases();
  test_lone_word_contexts_round_trip_and_block_the_remainder();
  test_document_eof_follows_removed_source_at_line_start();
  test_bracket_probe_takes_range_endpoints_literally();
  test_here_document_sequence_requires_a_line_boundary();
  test_here_document_line_start_only_emits_valid_tokens();
  test_newline_boundaries_require_a_valid_enclosing_terminator();
  test_static_command_words_preserve_quotes_and_scanner_state();
  test_layout_boundaries_do_not_turn_invalid_source_into_end();
  test_document_newline_boundaries_preserve_document_ownership();
  test_old_state_is_rejected();
  test_exact_fit_state_round_trip();
  test_pending_document_rejects_oversized_state();
  test_pending_activation_fits_after_depth_growth();
  test_pending_documents_suspend_and_resume_at_capacity();
  test_pending_activation_rejects_oversized_state();
  test_code_point_encoding();
  test_control_escape_table();
  test_byte_delimiter_matching();
  test_double_quoted_parameter_delimiters();
  test_parameter_delimiter_bytes_survive_state_restore();
  test_double_quoted_substitution_delimiter_spelling();
  test_arithmetic_delimiters_preserve_nested_quote_context();
  test_substitution_hash_delimiter_words();
  test_dollar_single_quote_escape_units();
  test_dollar_single_quote_delimiter_bytes_survive_state_restore();
  test_delimiter_scan_resource_rollback();
  test_logical_lexical_emission_keeps_individual_physical_pairs();
  test_word_entry_classifies_and_owns_source_in_normal_and_recovery_scans();
  test_whole_source_requires_its_classification_and_nonempty_range();
  test_separators_preserve_operator_identity_and_pending_documents();
  test_tilde_word_entry_uses_the_grammar_owned_lexical_context();
  test_pending_emission_rejects_disabled_and_stale_source();
  test_lexical_source_rejects_another_begin_after_restore();
  test_physical_prefix_and_character_keep_the_same_begin();
  test_saved_source_requires_a_begin_token();
  test_quote_context_commits_after_physical_punctuation();
  test_arithmetic_classification_restores_the_enclosing_context();
  test_pending_backquote_tokens_round_trip_and_consume_once();
  test_retained_command_headers_restore_and_consume_once();
  test_invalid_retained_token_rejects_recovery_without_consumption();
  test_retained_quoted_word_prefix_survives_partial_scans();
  test_multiline_end_text_keeps_internal_tabs_after_each_restore();
  test_source_context_growth_at_capacity_preserves_state();
#ifdef TREE_SITTER_REUSE_ALLOCATOR
  test_embedded_here_document_read_failures_terminate();
  test_delimiter_allocation_failures_preserve_state();
  test_pending_activation_allocation_failures_preserve_state();
  test_reuse_allocator_realloc_failure_rolls_back();
  test_source_callback_allocation_failures_are_transactional();
  test_reserved_word_allocation_failures_do_not_fall_back_to_word();
  test_comment_boundary_allocation_failures_terminate();
  test_document_boundary_allocation_failures_do_not_close_the_term();
  test_source_failures_release_owned_memory();
  test_logical_fork_allocation_failures_preserve_parent();
  test_reuse_allocator_contract();
#endif
  return 0;
}
