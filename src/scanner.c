#include "arithmetic.h"
#include "input.h"
#include "tokens.h"
#include "tree_sitter/alloc.h"
#include "tree_sitter/parser.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifndef TREE_SITTER_SERIALIZATION_BUFFER_SIZE
#define TREE_SITTER_SERIALIZATION_BUFFER_SIZE 1024
#endif

#define SCANNER_SERIALIZATION_VERSION 41
#define SCANNER_STATE_CAPACITY (TREE_SITTER_SERIALIZATION_BUFFER_SIZE - 1)
#define COUNT_OF(array) (sizeof(array) / sizeof((array)[0]))
#define RESERVED_WORD_CAPACITY 6
#define NAME_CANDIDATE_CAPACITY 9

enum LexicalSourceKind {
  LEXICAL_SOURCE_NONE,
  LEXICAL_SOURCE_TEXT,
  LEXICAL_SOURCE_PHYSICAL,
  LEXICAL_SOURCE_WHOLE,
};

struct LexicalSource {
  enum LexicalSourceKind kind;
  enum TokenType piece;
  enum TokenType prefix;
};

static const struct LexicalSource LEXICAL_SOURCES[TOKEN_COUNT] = {
#define LEXICAL_SOURCE_ENTRY(begin, piece) \
  [begin] = {LEXICAL_SOURCE_TEXT, piece, TOKEN_COUNT},
  DASH_LEXICAL_SOURCE_TOKENS(LEXICAL_SOURCE_ENTRY)
#undef LEXICAL_SOURCE_ENTRY
#define PHYSICAL_SOURCE_ENTRY(begin, prefix, character) \
  [begin] = {LEXICAL_SOURCE_PHYSICAL, character, prefix},
    DASH_PHYSICAL_SOURCE_TOKENS(PHYSICAL_SOURCE_ENTRY)
#undef PHYSICAL_SOURCE_ENTRY
#define WHOLE_SOURCE_ENTRY(begin, whole) \
  [whole] = {LEXICAL_SOURCE_WHOLE, whole, TOKEN_COUNT},
      DASH_WHOLE_SOURCE_TOKENS(WHOLE_SOURCE_ENTRY)
#undef WHOLE_SOURCE_ENTRY
};

static const enum TokenType WHOLE_SOURCES[TOKEN_COUNT] = {
#define WHOLE_SOURCE_SYMBOL(begin, whole) [begin] = whole,
  DASH_WHOLE_SOURCE_TOKENS(WHOLE_SOURCE_SYMBOL)
#undef WHOLE_SOURCE_SYMBOL
};

static const enum TokenType WORD_INITIAL_SYMBOLS[] = {
  LITERAL_BEGIN,
  WORD_FALLBACK_LITERAL_BEGIN,
  WORD_PATTERN_BRACKET_OPEN,
  PATTERN_STAR_BEGIN,
  PATTERN_QUESTION_BEGIN,
  ESCAPED_CHARACTER_BEGIN,
  SQ_OPEN,
  DQ_OPEN,
  DOLLAR_SQ_DOLLAR,
  DOLLAR_EXPANSION_START,
  BACKQUOTE_START,
};

static bool is_word_initial_symbol(enum TokenType symbol) {
  return symbol >= WORD_INITIAL_LITERAL_BEGIN &&
    symbol <= WORD_INITIAL_BACKQUOTE_START;
}

static enum TokenType word_initial_source_symbol(enum TokenType symbol) {
  switch (symbol) {
#define WHOLE_SOURCE_BEGIN(begin, whole) \
  case whole: \
    symbol = begin; \
    break;
    DASH_WHOLE_SOURCE_TOKENS(WHOLE_SOURCE_BEGIN)
#undef WHOLE_SOURCE_BEGIN
    default:
      break;
  }
  return is_word_initial_symbol(symbol)
    ? WORD_INITIAL_SYMBOLS[symbol - WORD_INITIAL_LITERAL_BEGIN]
    : symbol;
}

enum ArithmeticOperatorCategory {
  ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT,
  ARITHMETIC_OPERATOR_CATEGORY_QUESTION,
  ARITHMETIC_OPERATOR_CATEGORY_COLON,
  ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_OR,
  ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_AND,
  ARITHMETIC_OPERATOR_CATEGORY_BITWISE_OR,
  ARITHMETIC_OPERATOR_CATEGORY_BITWISE_XOR,
  ARITHMETIC_OPERATOR_CATEGORY_BITWISE_AND,
  ARITHMETIC_OPERATOR_CATEGORY_EQUALITY,
  ARITHMETIC_OPERATOR_CATEGORY_RELATIONAL,
  ARITHMETIC_OPERATOR_CATEGORY_SHIFT,
  ARITHMETIC_OPERATOR_CATEGORY_ADDITIVE,
  ARITHMETIC_OPERATOR_CATEGORY_MULTIPLICATIVE,
  ARITHMETIC_OPERATOR_CATEGORY_COUNT,
};

static const enum TokenType
  ARITHMETIC_OPERATOR_BOUNDARIES[ARITHMETIC_OPERATOR_CATEGORY_COUNT] = {
    [ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT] =
      ARITHMETIC_ASSIGNMENT_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_QUESTION] =
      ARITHMETIC_QUESTION_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_COLON] = ARITHMETIC_COLON_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_OR] =
      ARITHMETIC_LOGICAL_OR_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_AND] =
      ARITHMETIC_LOGICAL_AND_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_BITWISE_OR] =
      ARITHMETIC_BITWISE_OR_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_BITWISE_XOR] =
      ARITHMETIC_BITWISE_XOR_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_BITWISE_AND] =
      ARITHMETIC_BITWISE_AND_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_EQUALITY] =
      ARITHMETIC_EQUALITY_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_RELATIONAL] =
      ARITHMETIC_RELATIONAL_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_SHIFT] = ARITHMETIC_SHIFT_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_ADDITIVE] =
      ARITHMETIC_ADDITIVE_OPERATOR_BOUNDARY,
    [ARITHMETIC_OPERATOR_CATEGORY_MULTIPLICATIVE] =
      ARITHMETIC_MULTIPLICATIVE_OPERATOR_BOUNDARY,
};

struct HereDocument {
  uint8_t *delimiter;
  size_t delimiter_length;
  size_t declaration_depth;
  bool quoted;
  bool strip_tabs;
  bool ending;
  bool end_text_consumed;
};

struct HereDocumentFrame {
  struct HereDocument *documents;
  size_t count;
  size_t body_backquote_depth;
  bool at_line_start;
};

enum BackquoteToken {
  BACKQUOTE_TOKEN_NONE,
  BACKQUOTE_TOKEN_RIGHT_PARENTHESIS,
  BACKQUOTE_TOKEN_STOP,
  BACKQUOTE_TOKEN_INVALID,
  BACKQUOTE_TOKEN_LEFT_PARENTHESIS,
  BACKQUOTE_TOKEN_WORD,
  BACKQUOTE_TOKEN_NAME_WORD,
  BACKQUOTE_TOKEN_DECLARATION,
  BACKQUOTE_TOKEN_FORWARDING,
  BACKQUOTE_TOKEN_NAME_FORWARDING,
  BACKQUOTE_TOKEN_IF,
  BACKQUOTE_TOKEN_FOR,
  BACKQUOTE_TOKEN_CASE,
  BACKQUOTE_TOKEN_WHILE,
  BACKQUOTE_TOKEN_UNTIL,
  BACKQUOTE_TOKEN_BRACE,
  BACKQUOTE_TOKEN_NEGATION,
  BACKQUOTE_TOKEN_ASSIGNMENT,
};

enum HereDocumentWord {
  HERE_WORD_START,
  HERE_WORD_NAME,
  HERE_WORD_OTHER,
  HERE_WORD_ASSIGNMENT,
};

struct SourceContext {
  enum TokenType opener;
  size_t stage_count;
  size_t count;
  size_t substitution_depth;
  uint16_t bracket_chains;
  enum BackquoteToken backquote_token;
  enum HereDocumentWord here_word;
  bool local_disabled;
  bool lone_word;
  bool suspends_body;
};

struct LexicalEmission {
  enum TokenType symbol;
  size_t remaining;
  uint16_t bracket_chains;
  bool active;
  bool removed_newline;
  bool ends_assignment_colon;
  bool tracks_brackets;
};

struct Scanner {
  struct HereDocument *pending_documents;
  size_t pending_count;
  struct HereDocumentFrame active_frame;
  struct HereDocumentFrame *suspended_frames;
  size_t suspended_frame_count;
  bool sequence_end_pending;
  size_t backquote_depth;
  size_t substitution_depth;
  struct SourceSnapshot source;
  struct SourceContext *contexts;
  size_t context_count;
  struct LexicalEmission emission;
  bool assignment_tilde_allowed;
  bool here_document_end_line_consumed;
};

struct ByteBuffer {
  char *data;
  size_t length;
  size_t capacity;
  size_t limit;
  bool failed;
};

static bool scanner_state_fits(const struct Scanner *scanner);

static struct SourceContext *top_context(struct Scanner *scanner) {
  return scanner->context_count == 0
    ? NULL
    : &scanner->contexts[scanner->context_count - 1];
}

static enum TokenType top_context_opener(const struct Scanner *scanner) {
  return scanner->context_count == 0
    ? TOKEN_COUNT
    : scanner->contexts[scanner->context_count - 1].opener;
}

static bool is_here_document_context(enum TokenType opener) {
  return opener == HERE_DOCUMENT_BODY_START ||
    opener == QUOTED_HERE_DOCUMENT_BODY_START;
}

static bool is_arithmetic_context(enum TokenType opener) {
  return opener == ARITHMETIC_LEFT_PARENTHESIS ||
    opener == ARITHMETIC_SOURCE_LEFT_PARENTHESIS;
}

static bool is_backquote_context(enum TokenType opener) {
  return opener == BACKQUOTE_START || opener == DOUBLE_QUOTED_BACKQUOTE_START;
}

static bool is_substitution_context(enum TokenType opener) {
  return opener == COMMAND_OPEN || is_backquote_context(opener);
}

static size_t pending_backquote_context(const struct Scanner *scanner) {
  for (size_t depth = scanner->context_count; depth > 0; depth -= 1) {
    enum TokenType opener = scanner->contexts[depth - 1].opener;
    if (is_here_document_context(opener)) {
      return depth > 1 &&
          is_backquote_context(scanner->contexts[depth - 2].opener) &&
          scanner->contexts[depth - 2].backquote_token != BACKQUOTE_TOKEN_NONE
        ? depth - 2
        : SIZE_MAX;
    }
    if (depth < scanner->context_count && is_substitution_context(opener)) {
      break;
    }
  }
  return SIZE_MAX;
}

static bool source_context_pop(struct Scanner *scanner);

struct ReservedWord {
  const char *text;
  enum TokenType symbol;
};

static const struct ReservedWord RESERVED_WORDS[] = {
  {"if", IF_KEYWORD},
  {"then", THEN_KEYWORD},
  {"elif", ELIF_KEYWORD},
  {"else", ELSE_KEYWORD},
  {"fi", FI_KEYWORD},
  {"for", FOR_KEYWORD},
  {"in", IN_KEYWORD},
  {"do", DO_KEYWORD},
  {"done", DONE_KEYWORD},
  {"case", CASE_KEYWORD},
  {"esac", ESAC_KEYWORD},
  {"while", WHILE_KEYWORD},
  {"until", UNTIL_KEYWORD},
};

static const struct ReservedWord *find_reserved_word(const char *word) {
  size_t word_count = COUNT_OF(RESERVED_WORDS);
  for (size_t index = 0; index < word_count; index += 1) {
    if (strcmp(word, RESERVED_WORDS[index].text) == 0) {
      return &RESERVED_WORDS[index];
    }
  }
  return NULL;
}

static bool forbidden_function_name(const char *word) {
  static const char *const forbidden[] = {
    "break",
    "continue",
    "eval",
    "exec",
    "exit",
    "export",
    "local",
    "readonly",
    "return",
    "set",
    "shift",
    "times",
    "trap",
    "unset",
  };
  for (size_t index = 0; index < COUNT_OF(forbidden); index += 1) {
    if (strcmp(word, forbidden[index]) == 0) return true;
  }
  return false;
}

static void backquote_stages(bool quoted, struct SourceStage *stages) {
  stages[0] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  stages[1] = source_stage(SOURCE_BACKQUOTE_DECODE);
  stages[1].quoted = quoted;
  stages[2] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
}

static void here_document_body_stages(
  const struct HereDocument *document,
  struct SourceStage *stages
) {
  stages[0] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  stages[0].disabled = true;
  stages[1] = source_stage(SOURCE_HERE_DOCUMENT);
  stages[1].quoted = document->quoted;
  stages[1].strip_tabs = document->strip_tabs;
  stages[2] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  stages[2].disabled = true;
}

static size_t
here_document_stage_index(const struct SourceStage *stages, size_t count) {
  while (count > 0) {
    count -= 1;
    if (stages[count].kind == SOURCE_HERE_DOCUMENT) {
      return count;
    }
  }
  return SIZE_MAX;
}

static void clear_document(struct HereDocument *document) {
  ts_free(document->delimiter);
  *document = (struct HereDocument){0};
}

static void
clear_document_array(struct HereDocument **documents, size_t *count) {
  for (size_t index = 0; index < *count; index += 1) {
    clear_document(&(*documents)[index]);
  }

  ts_free(*documents);
  *documents = NULL;
  *count = 0;
}

static void discard_documents_above(
  struct HereDocument **documents,
  size_t *count,
  size_t depth
) {
  size_t retained = 0;
  for (size_t index = 0; index < *count; index += 1) {
    struct HereDocument *document = &(*documents)[index];
    if (document->declaration_depth > depth) {
      clear_document(document);
    } else {
      (*documents)[retained] = *document;
      retained += 1;
    }
  }
  if (retained == 0) {
    ts_free(*documents);
    *documents = NULL;
  }
  *count = retained;
}

static void clear_scanner(struct Scanner *scanner) {
  source_snapshot_clear(&scanner->source);
  ts_free(scanner->contexts);
  clear_document_array(&scanner->pending_documents, &scanner->pending_count);
  clear_document_array(
    &scanner->active_frame.documents,
    &scanner->active_frame.count
  );
  for (size_t index = 0; index < scanner->suspended_frame_count; index += 1) {
    clear_document_array(
      &scanner->suspended_frames[index].documents,
      &scanner->suspended_frames[index].count
    );
  }
  ts_free(scanner->suspended_frames);
  *scanner = (struct Scanner){0};
}

static size_t enclosing_substitution_depth(const struct Scanner *scanner) {
  return scanner->substitution_depth + scanner->backquote_depth;
}

static bool here_document_is_startable(
  const struct Scanner *scanner,
  const struct HereDocument *document
) {
  return document->declaration_depth >= enclosing_substitution_depth(scanner);
}

static bool has_startable_pending_document(const struct Scanner *scanner) {
  for (size_t index = 0; index < scanner->pending_count; index += 1) {
    if (
      here_document_is_startable(scanner, &scanner->pending_documents[index])
    ) {
      return true;
    }
  }
  return false;
}

static void discard_enclosed_pending_documents(struct Scanner *scanner) {
  discard_documents_above(
    &scanner->pending_documents,
    &scanner->pending_count,
    enclosing_substitution_depth(scanner)
  );
}

static bool append_document(
  struct HereDocument **documents,
  size_t *count,
  struct HereDocument document
) {
  if (*count >= SIZE_MAX / sizeof(struct HereDocument)) {
    return false;
  }

  size_t next_count = *count + 1;
  struct HereDocument *resized =
    ts_realloc(*documents, next_count * sizeof(struct HereDocument));
  if (resized == NULL) {
    return false;
  }

  resized[*count] = document;
  *documents = resized;
  *count = next_count;
  return true;
}

static bool
append_pending_document(struct Scanner *scanner, struct HereDocument document) {
  document.declaration_depth = enclosing_substitution_depth(scanner);

  if (!append_document(
        &scanner->pending_documents,
        &scanner->pending_count,
        document
      )) {
    return false;
  }

  if (!scanner_state_fits(scanner)) {
    scanner->pending_count -= 1;
    scanner->pending_documents[scanner->pending_count] =
      (struct HereDocument){0};
    return false;
  }
  return true;
}

static bool partition_pending_documents(
  const struct Scanner *scanner,
  struct HereDocument **startable,
  size_t *startable_count,
  struct HereDocument **retained,
  size_t *retained_count
) {
  *startable_count = 0;
  for (size_t index = 0; index < scanner->pending_count; index += 1) {
    if (
      here_document_is_startable(scanner, &scanner->pending_documents[index])
    ) {
      *startable_count += 1;
    }
  }
  *retained_count = scanner->pending_count - *startable_count;
  *startable = NULL;
  *retained = NULL;
  if (*startable_count == 0) {
    return false;
  }
  if (*retained_count == 0) {
    *startable = scanner->pending_documents;
    return true;
  }
  *startable = ts_calloc(*startable_count, sizeof(struct HereDocument));
  *retained = ts_calloc(*retained_count, sizeof(struct HereDocument));
  if (*startable == NULL || *retained == NULL) {
    ts_free(*startable);
    ts_free(*retained);
    return false;
  }
  size_t startable_index = 0;
  size_t retained_index = 0;
  for (size_t index = 0; index < scanner->pending_count; index += 1) {
    struct HereDocument *document = &scanner->pending_documents[index];
    if (here_document_is_startable(scanner, document)) {
      (*startable)[startable_index++] = *document;
    } else {
      (*retained)[retained_index++] = *document;
    }
  }
  return true;
}

/* Rollback must retain reallocated frames. */
static bool
suspend_active_documents(struct Scanner *scanner, struct Scanner *next) {
  if (
    scanner->suspended_frame_count >=
    SIZE_MAX / sizeof(struct HereDocumentFrame)
  ) {
    return false;
  }
  struct HereDocumentFrame *frames = ts_realloc(
    scanner->suspended_frames,
    (scanner->suspended_frame_count + 1) * sizeof(struct HereDocumentFrame)
  );
  if (frames == NULL) {
    return false;
  }
  scanner->suspended_frames = frames;
  frames[scanner->suspended_frame_count] = scanner->active_frame;
  next->suspended_frames = frames;
  next->suspended_frame_count = scanner->suspended_frame_count + 1;
  next->active_frame.at_line_start = false;
  return true;
}

static bool activate_startable_pending_documents(struct Scanner *scanner) {
  struct HereDocument *startable;
  struct HereDocument *retained;
  size_t startable_count;
  size_t retained_count;
  if (!partition_pending_documents(
        scanner,
        &startable,
        &startable_count,
        &retained,
        &retained_count
      )) {
    return false;
  }

  struct Scanner next = *scanner;
  next.pending_documents = retained;
  next.pending_count = retained_count;
  next.active_frame.documents = startable;
  next.active_frame.count = startable_count;
  next.active_frame.body_backquote_depth = scanner->backquote_depth;
  bool fits = (scanner->active_frame.count == 0 ||
                suspend_active_documents(scanner, &next)) &&
    scanner_state_fits(&next);
  if (!fits) {
    if (next.suspended_frame_count > scanner->suspended_frame_count) {
      scanner->suspended_frames[scanner->suspended_frame_count] =
        (struct HereDocumentFrame){0};
    }
    if (retained_count > 0) {
      ts_free(startable);
      ts_free(retained);
    }
    return false;
  }

  if (retained_count > 0) {
    ts_free(scanner->pending_documents);
  }
  *scanner = next;
  return true;
}

static void restore_suspended_documents(struct Scanner *scanner) {
  if (scanner->suspended_frame_count == 0) {
    return;
  }

  clear_document_array(
    &scanner->active_frame.documents,
    &scanner->active_frame.count
  );
  scanner->suspended_frame_count -= 1;
  scanner->active_frame =
    scanner->suspended_frames[scanner->suspended_frame_count];

  if (scanner->suspended_frame_count == 0) {
    ts_free(scanner->suspended_frames);
    scanner->suspended_frames = NULL;
  }
}

static void finish_active_document(struct Scanner *scanner) {
  struct HereDocumentFrame *frame = &scanner->active_frame;
  scanner->backquote_depth = frame->body_backquote_depth;
  clear_document(&frame->documents[0]);
  frame->count -= 1;

  if (frame->count > 0) {
    memmove(
      frame->documents,
      frame->documents + 1,
      frame->count * sizeof(struct HereDocument)
    );
    return;
  }

  ts_free(frame->documents);
  frame->documents = NULL;
  scanner->sequence_end_pending = true;
}

static bool grow_byte_buffer(struct ByteBuffer *buffer, size_t minimum) {
  if (buffer->limit != 0 && minimum > buffer->limit) {
    buffer->failed = true;
    return false;
  }

  size_t capacity = buffer->capacity == 0 ? 32 : buffer->capacity;
  while (capacity < minimum) {
    if (capacity > SIZE_MAX / 2) {
      capacity = SIZE_MAX;
      break;
    }
    capacity *= 2;
  }

  if (buffer->limit != 0 && capacity > buffer->limit) {
    capacity = buffer->limit;
  }

  if (capacity < minimum) {
    buffer->failed = true;
    return false;
  }

  char *resized = ts_realloc(buffer->data, capacity);
  if (resized == NULL) {
    buffer->failed = true;
    return false;
  }

  buffer->data = resized;
  buffer->capacity = capacity;
  return true;
}

static bool
append_bytes(struct ByteBuffer *buffer, const uint8_t *bytes, size_t length) {
  if (buffer == NULL) {
    return true;
  }

  if (length > SIZE_MAX - buffer->length) {
    buffer->failed = true;
    return false;
  }

  size_t next_length = buffer->length + length;
  if (
    next_length > buffer->capacity && !grow_byte_buffer(buffer, next_length)
  ) {
    return false;
  }

  if (length > 0) {
    memcpy(buffer->data + buffer->length, bytes, length);
  }
  buffer->length = next_length;
  return true;
}

static bool append_byte(struct ByteBuffer *buffer, uint8_t byte) {
  return append_bytes(buffer, &byte, 1);
}

static bool
encode_utf8_scalar(int32_t character, uint8_t bytes[4], size_t *length) {
  if (character < 0 || character > 0x10ffff) {
    return false;
  }

  if (character <= 0x7f) {
    bytes[0] = (uint8_t)character;
    *length = 1;
    return true;
  }

  if (character <= 0x7ff) {
    bytes[0] = (uint8_t)(0xc0 | (character >> 6));
    bytes[1] = (uint8_t)(0x80 | (character & 0x3f));
    *length = 2;
    return true;
  }

  // dash encodes surrogate escapes as three bytes.
  if (character <= 0xffff) {
    bytes[0] = (uint8_t)(0xe0 | (character >> 12));
    bytes[1] = (uint8_t)(0x80 | ((character >> 6) & 0x3f));
    bytes[2] = (uint8_t)(0x80 | (character & 0x3f));
    *length = 3;
    return true;
  }

  bytes[0] = (uint8_t)(0xf0 | (character >> 18));
  bytes[1] = (uint8_t)(0x80 | ((character >> 12) & 0x3f));
  bytes[2] = (uint8_t)(0x80 | ((character >> 6) & 0x3f));
  bytes[3] = (uint8_t)(0x80 | (character & 0x3f));
  *length = 4;
  return true;
}

static bool append_codepoint(struct ByteBuffer *buffer, int32_t character) {
  uint8_t bytes[4];
  size_t length;
  return encode_utf8_scalar(character, bytes, &length) &&
    append_bytes(buffer, bytes, length);
}

static bool append_quoted_escape(struct ByteBuffer *buffer, int32_t character) {
  switch (character) {
    case 'a':
      return append_byte(buffer, '\a');
    case 'b':
      return append_byte(buffer, '\b');
    case 'e':
      return append_byte(buffer, 0x1b);
    case 'f':
      return append_byte(buffer, '\f');
    case 'n':
      return append_byte(buffer, '\n');
    case 'r':
      return append_byte(buffer, '\r');
    case 't':
      return append_byte(buffer, '\t');
    case 'v':
      return append_byte(buffer, '\v');
    case '\\':
    case '\'':
    case '"':
      return append_codepoint(buffer, character);
    default:
      return append_byte(buffer, '\\') && append_codepoint(buffer, character);
  }
}

static bool is_horizontal_blank(int32_t character) {
  return character == ' ' || character == '\t';
}

static bool is_one_of(int32_t character, const char *set) {
  return character > 0 &&
    character < 128 &&
    strchr(set, (int)character) != NULL;
}

static bool is_special_parameter_character(int32_t character) {
  return is_one_of(character, "0*@#?$!-");
}

static bool skip_parameter_name(TSLexer *lexer, int32_t introducer) {
  if (is_name_start_character(introducer)) {
    while (is_name_character(lexer->lookahead)) {
      lexer->advance(lexer, false);
    }
    return true;
  }
  if (
    is_decimal_digit(introducer) || is_special_parameter_character(introducer)
  ) {
    lexer->advance(lexer, false);
    return true;
  }
  return false;
}

static bool is_parameter_start_character(int32_t character) {
  return (
    is_name_start_character(character) ||
    is_decimal_digit(character) ||
    is_special_parameter_character(character)
  );
}

static bool is_lowercase_letter(int32_t character) {
  return character >= 'a' && character <= 'z';
}

static bool is_control_operator_start(int32_t character) {
  return is_one_of(character, "&();<>|");
}

static bool is_redirect_operator_suffix(int32_t first, int32_t second) {
  return is_one_of(second, first == '>' ? ">|&" : ">&");
}

static bool emit_symbol(TSLexer *lexer, enum TokenType symbol) {
  lexer->mark_end(lexer);
  lexer->result_symbol = (TSSymbol)symbol;
  return true;
}

static bool accept_character(TSLexer *lexer, enum TokenType symbol) {
  lexer->advance(lexer, false);
  return emit_symbol(lexer, symbol);
}

static bool skip_escape_run(TSLexer *lexer, bool *continuation) {
  size_t count = 0;
  while (lexer->lookahead == '\\') {
    if (count == SIZE_MAX) {
      return false;
    }
    count += 1;
    lexer->advance(lexer, false);
  }
  *continuation = count == 1 && lexer->lookahead == '\n';
  if ((count & 1) != 0 && lexer->lookahead != SOURCE_BACKQUOTE_BOUNDARY) {
    lexer->advance(lexer, false);
  }
  return true;
}

static bool lexer_at_eof(const TSLexer *lexer) {
  return lexer->lookahead == 0 && lexer->eof(lexer);
}

static bool lexer_at_input_end(const TSLexer *lexer) {
  return lexer_at_eof(lexer) || lexer->lookahead == SOURCE_BACKQUOTE_BOUNDARY;
}

static struct LogicalLexer *lookahead_input(TSLexer *lexer);
static bool lookahead_local_policy(TSLexer *lexer, bool disabled);

static bool advance_source(TSLexer *lexer, struct ByteBuffer *source) {
  if (source != NULL && !append_codepoint(source, lexer->lookahead)) {
    return false;
  }
  lexer->advance(lexer, false);
  return true;
}

static bool skip_dollar_parameter(TSLexer *lexer, struct ByteBuffer *source) {
  return lexer->lookahead != '$' || advance_source(lexer, source);
}

static bool lookahead_local_disabled(TSLexer *lexer) {
  struct LogicalLexer *input = lookahead_input(lexer);
  return input != NULL &&
    input->cursor.stage_count > 0 &&
    input->cursor.initial[input->cursor.stage_count - 1].disabled;
}

static bool advance_to_comment_end(TSLexer *lexer, struct ByteBuffer *source) {
  bool disabled = lookahead_local_disabled(lexer);
  if (lexer->lookahead != '#' || !advance_source(lexer, source)) {
    return false;
  }
  if (!lookahead_local_policy(lexer, true)) {
    return false;
  }
  while (!lexer_at_eof(lexer) && lexer->lookahead != '\n') {
    if (lexer->lookahead == SOURCE_BACKQUOTE_BOUNDARY) {
      break;
    }
    if (!advance_source(lexer, source)) {
      return false;
    }
  }
  return lookahead_local_policy(lexer, disabled);
}

static bool is_token_delimiter_character(int32_t character) {
  return is_horizontal_blank(character) ||
    character == '\n' ||
    is_control_operator_start(character) ||
    character == SOURCE_BACKQUOTE_BOUNDARY;
}

static bool is_token_delimiter(const TSLexer *lexer) {
  return lexer_at_eof(lexer) || is_token_delimiter_character(lexer->lookahead);
}

static bool pattern_boundary(const TSLexer *lexer, bool parameter) {
  if (!parameter) {
    return is_token_delimiter(lexer);
  }
  return lexer_at_eof(lexer) ||
    lexer->lookahead == '}' ||
    lexer->lookahead == SOURCE_BACKQUOTE_BOUNDARY;
}

static bool is_quote_or_expansion_start(int32_t character) {
  return is_one_of(character, "'\"$`");
}

struct ValidationToken {
  uint8_t kind;
  uint8_t category;
};

struct ValidationTokenBuffer {
  struct ValidationToken *data;
  size_t length;
  size_t capacity;
};

struct ArithmeticScan {
  struct ValidationTokenBuffer tokens;
  struct EmbeddedSkip *embedded;
  char embedded_closer;
  size_t group_depth;
  bool unstructured;
};

struct AmbiguousSubstitution {
  size_t start;
  size_t end;
  size_t parent;
  size_t next;
  struct Scanner context;
  struct ArithmeticScan scan;
  struct EmbeddedSkip *command;
  size_t command_resume;
  size_t resume;
  enum ArithmeticValidation result;
  bool scan_started;
  bool scan_complete;
  bool raw;
  bool arithmetic;
  bool complete;
};

enum PatternMode {
  PATTERN_WORD,
  PATTERN_ASSIGNMENT,
  PATTERN_PARAMETER,
  PATTERN_WORD_TILDE,
  PATTERN_ASSIGNMENT_TILDE,
  PATTERN_PARAMETER_TILDE,
};

// Tree-sitter reserves -1 for undecodable bytes.
#define LOOKAHEAD_END INT32_MIN

struct LookaheadCharacter {
  int32_t value;
  size_t substitution;
  size_t view;
  size_t logical_position;
  size_t raw_cut;
  size_t next;
  size_t fork;
};

struct LookaheadView {
  struct LogicalLexer *input;
  size_t inherited;
  size_t suffix_count;
  size_t entry;
  size_t next;
};

struct LookaheadLexer {
  TSLexer lexer;
  TSLexer *source;
  struct LookaheadCharacter *characters;
  size_t length;
  size_t capacity;
  size_t position;
  struct LookaheadView *views;
  size_t view_count;
  size_t view_capacity;
  struct AmbiguousSubstitution *substitutions;
  size_t substitution_count;
  size_t substitution_capacity;
  size_t requested;
  size_t active;
  bool failed;
};

static void lookahead_fail(struct LookaheadLexer *lookahead) {
  lookahead->failed = true;
  lookahead->lexer.lookahead = 0;
  if (
    lookahead->source != NULL && lookahead->source->advance == logical_advance
  ) {
    ((struct LogicalLexer *)lookahead->source)->native->failed = true;
  }
}

static void lookahead_seek(struct LookaheadLexer *lookahead, size_t position) {
  lookahead->position = position;
  if (lookahead->failed) {
    return;
  }
  int32_t character = lookahead->characters[position].value;
  lookahead->lexer.lookahead = character == LOOKAHEAD_END ? 0 : character;
}

static bool lookahead_append(struct LookaheadLexer *lookahead, size_t view) {
  struct LogicalLexer *input = lookahead->views[view].input;
  if (input->result == SOURCE_FAILURE) {
    lookahead_fail(lookahead);
    return false;
  }
  struct LookaheadCharacter *characters = source_grow(
    lookahead->characters,
    &lookahead->capacity,
    lookahead->length + 1,
    sizeof(struct LookaheadCharacter)
  );
  if (characters == NULL) {
    lookahead_fail(lookahead);
    return false;
  }
  lookahead->characters = characters;
  lookahead->characters[lookahead->length++] = (struct LookaheadCharacter){
    .value =
      lexer_at_eof(&input->lexer) ? LOOKAHEAD_END : input->lexer.lookahead,
    .substitution = SIZE_MAX,
    .view = view,
    .logical_position = input->result == SOURCE_CHARACTER
      ? input->cursor.logical_position - 1
      : input->cursor.logical_position,
    .raw_cut = input->consumed,
    .next = SIZE_MAX,
    .fork = SIZE_MAX,
  };
  return true;
}

static struct LogicalLexer *
lookahead_restore(struct LookaheadLexer *lookahead) {
  const struct LookaheadCharacter *character =
    &lookahead->characters[lookahead->position];
  struct LogicalLexer *input = lookahead->views[character->view].input;
  input->cursor.logical_position = character->logical_position;
  input->consumed = character->raw_cut;
  logical_load(input);
  if (input->result == SOURCE_FAILURE) {
    lookahead_fail(lookahead);
  }
  return input;
}

static void lookahead_advance(TSLexer *lexer, bool skip) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  if (lookahead->failed) {
    return;
  }
  if (lookahead->length == 0 && !lookahead_append(lookahead, 0)) {
    return;
  }
  size_t position = lookahead->position;
  size_t next = lookahead->characters[position].next;
  if (next == SIZE_MAX) {
    struct LogicalLexer *input = lookahead_restore(lookahead);
    if (lexer_at_eof(&input->lexer)) {
      return;
    }
    input->lexer.advance(&input->lexer, skip);
    next = lookahead->length;
    if (!lookahead_append(lookahead, lookahead->characters[position].view)) {
      return;
    }
    lookahead->characters[position].next = next;
  }
  lookahead_seek(lookahead, next);
}

static void lookahead_mark_end(TSLexer *lexer) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  if (!lookahead->failed) {
    struct LogicalLexer *input = lookahead->views[0].input;
    input->mark = lookahead->length == 0
      ? input->consumed
      : lookahead->characters[lookahead->position].raw_cut;
  }
}

static bool lookahead_eof(const TSLexer *lexer) {
  const struct LookaheadLexer *lookahead = (const struct LookaheadLexer *)lexer;
  return lookahead->failed ||
    (lookahead->length == 0
        ? lexer_at_eof(lookahead->source)
        : lookahead->characters[lookahead->position].value == LOOKAHEAD_END);
}

static bool lookahead_has_failed(const TSLexer *lexer) {
  return lexer->advance == lookahead_advance &&
    ((const struct LookaheadLexer *)lexer)->failed;
}

static bool lookahead_is_pending(const TSLexer *lexer) {
  const struct LookaheadLexer *lookahead = (const struct LookaheadLexer *)lexer;
  return lookahead->failed || lookahead->requested != SIZE_MAX;
}

static struct LogicalLexer *lookahead_input(TSLexer *lexer) {
  if (lexer->advance == logical_advance) {
    return (struct LogicalLexer *)lexer;
  }
  if (lexer->advance != lookahead_advance) {
    return NULL;
  }
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  if (lookahead->length == 0 && !lookahead_append(lookahead, 0)) {
    return NULL;
  }
  return lookahead_restore(lookahead);
}

static bool stage_matches(
  const struct SourceStage *existing,
  const struct SourceStage *wanted
) {
  return existing->kind == wanted->kind &&
    existing->disabled == wanted->disabled &&
    existing->quoted == wanted->quoted &&
    existing->at_line_start == wanted->at_line_start;
}

static size_t lookahead_find_view(
  const struct LookaheadLexer *lookahead,
  size_t position,
  size_t raw_cut,
  size_t inherited,
  const struct SourceStage *suffix,
  size_t suffix_count
) {
  for (
    size_t index = lookahead->characters[position].fork; index != SIZE_MAX;
    index = lookahead->views[index].next
  ) {
    const struct LookaheadView *view = &lookahead->views[index];
    if (
      view->inherited != inherited ||
      view->suffix_count != suffix_count ||
      lookahead->characters[view->entry].raw_cut != raw_cut
    ) {
      continue;
    }
    bool matches = true;
    for (size_t stage = 0; stage < suffix_count; stage += 1) {
      matches = matches &&
        stage_matches(
          &view->input->cursor.initial[inherited + stage],
          &suffix[stage]
        );
    }
    if (matches) {
      return index;
    }
  }
  return SIZE_MAX;
}

static bool lookahead_push_view(
  struct LookaheadLexer *lookahead,
  struct LogicalLexer *parent,
  size_t position,
  size_t raw_cut,
  size_t inherited,
  const struct SourceStage *suffix,
  size_t suffix_count
) {
  struct LogicalLexer *input = ts_malloc(sizeof(*input));
  if (input == NULL) {
    lookahead_fail(lookahead);
    return false;
  }
  if (!logical_fork(input, parent, raw_cut, inherited, suffix, suffix_count)) {
    ts_free(input);
    lookahead_fail(lookahead);
    return false;
  }
  struct LookaheadView *views = source_grow(
    lookahead->views,
    &lookahead->view_capacity,
    lookahead->view_count + 1,
    sizeof(*views)
  );
  if (views == NULL) {
    logical_clear(input);
    ts_free(input);
    lookahead_fail(lookahead);
    return false;
  }
  lookahead->views = views;
  size_t index = lookahead->view_count++;
  size_t entry = lookahead->length;
  lookahead->views[index] = (struct LookaheadView){
    .input = input,
    .inherited = inherited,
    .suffix_count = suffix_count,
    .entry = entry,
    .next = lookahead->characters[position].fork,
  };
  lookahead->characters[position].fork = index;
  if (!lookahead_append(lookahead, index)) {
    return false;
  }
  lookahead_seek(lookahead, entry);
  return true;
}

static bool lookahead_set_stages_at(
  TSLexer *lexer,
  size_t raw_cut,
  size_t inherited,
  const struct SourceStage *suffix,
  size_t suffix_count
) {
  if (lexer->advance == logical_advance) {
    if (((struct LogicalLexer *)lexer)->consumed != raw_cut) {
      return false;
    }
    return logical_replace_view(
      (struct LogicalLexer *)lexer,
      inherited,
      suffix,
      suffix_count
    );
  }
  if (lexer->advance != lookahead_advance) {
    return false;
  }
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  struct LogicalLexer *parent = lookahead_input(lexer);
  if (parent == NULL) {
    return false;
  }
  while (parent->base + parent->cursor.raw_count < raw_cut) {
    int32_t value;
    if (
      logical_read(parent, &value) != SOURCE_CHARACTER ||
      !source_feed(&parent->cursor, value)
    ) {
      lookahead_fail(lookahead);
      return false;
    }
  }
  size_t position = lookahead->position;
  size_t existing = lookahead_find_view(
    lookahead,
    position,
    raw_cut,
    inherited,
    suffix,
    suffix_count
  );
  if (existing != SIZE_MAX) {
    lookahead_seek(lookahead, lookahead->views[existing].entry);
    return true;
  }
  return lookahead_push_view(
    lookahead,
    parent,
    position,
    raw_cut,
    inherited,
    suffix,
    suffix_count
  );
}

static bool lookahead_set_stages(
  TSLexer *lexer,
  size_t inherited,
  const struct SourceStage *suffix,
  size_t suffix_count
) {
  struct LogicalLexer *input = lookahead_input(lexer);
  return input != NULL &&
    lookahead_set_stages_at(
      lexer,
      input->consumed,
      inherited,
      suffix,
      suffix_count
    );
}

static bool lookahead_local_policy(TSLexer *lexer, bool disabled) {
  struct LogicalLexer *input = lookahead_input(lexer);
  if (input == NULL || input->cursor.stage_count == 0) {
    return false;
  }
  size_t local = input->cursor.stage_count - 1;
  if (input->cursor.initial[local].kind != SOURCE_REMOVE_CONTINUATIONS) {
    return false;
  }
  if (input->cursor.initial[local].disabled == disabled) {
    return true;
  }
  struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  stage.disabled = disabled;
  return lookahead_set_stages(lexer, local, &stage, 1);
}

struct LookaheadContext {
  size_t stage_count;
  bool local_disabled;
  bool entered;
  bool suspends_body;
};

static bool lookahead_set_body_stages(
  TSLexer *lexer,
  size_t body,
  size_t end,
  bool disabled,
  const struct SourceStage *tail,
  size_t tail_count
) {
  struct LogicalLexer *input = lookahead_input(lexer);
  if (input == NULL) {
    return false;
  }
  size_t copied = end - body;
  struct SourceStage *suffix =
    ts_malloc((copied + tail_count) * sizeof(*suffix));
  if (suffix == NULL) {
    return false;
  }
  for (size_t index = 0; index < copied; index += 1) {
    const struct SourceStage *initial = &input->cursor.initial[body + index];
    suffix[index] = source_stage(initial->kind);
    suffix[index].disabled = initial->disabled;
    suffix[index].quoted = initial->quoted;
    suffix[index].strip_tabs = initial->strip_tabs;
  }
  suffix[0].disabled = disabled;
  suffix[0].at_line_start = false;
  memcpy(suffix + copied, tail, tail_count * sizeof(*suffix));
  bool result = lookahead_set_stages(lexer, body, suffix, copied + tail_count);
  ts_free(suffix);
  return result;
}

static bool lookahead_substitution_begin(
  TSLexer *lexer,
  bool backquote,
  bool quoted,
  struct LookaheadContext *context
) {
  struct LogicalLexer *input = lookahead_input(lexer);
  if (input == NULL || input->cursor.stage_count == 0) {
    return false;
  }
  size_t count = input->cursor.stage_count;
  const struct SourceStage *initial = input->cursor.initial;
  size_t body = here_document_stage_index(initial, count - 1);
  *context = (struct LookaheadContext){
    .stage_count = count,
    .local_disabled = initial[count - 1].disabled,
    .suspends_body = body != SIZE_MAX && !initial[body].disabled,
  };
  struct SourceStage tail[3];
  size_t tail_count = 1;
  if (backquote) {
    backquote_stages(quoted, tail);
    tail_count = 3;
  } else {
    tail[0] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    if (!context->suspends_body && !context->local_disabled) {
      return true;
    }
  }
  context->entered = true;
  return context->suspends_body
    ? lookahead_set_body_stages(lexer, body, count - 1, true, tail, tail_count)
    : lookahead_set_stages(lexer, count - 1, tail, tail_count);
}

static bool
lookahead_context_end(TSLexer *lexer, const struct LookaheadContext *context) {
  if (!context->entered) {
    return true;
  }
  struct SourceStage stage = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  stage.disabled = context->local_disabled;
  if (!context->suspends_body) {
    return lookahead_set_stages(lexer, context->stage_count - 1, &stage, 1);
  }
  struct LogicalLexer *input = lookahead_input(lexer);
  if (input == NULL) {
    return false;
  }
  size_t body =
    here_document_stage_index(input->cursor.initial, context->stage_count - 1);
  return body != SIZE_MAX &&
    lookahead_set_body_stages(
      lexer,
      body,
      context->stage_count - 1,
      false,
      &stage,
      1
    );
}

static void complete_ambiguous_substitutions(struct LookaheadLexer *lookahead);

static const struct AmbiguousSubstitution *
request_ambiguous_substitution(struct LookaheadLexer *lookahead, size_t index) {
  if (lookahead->active != SIZE_MAX) {
    lookahead->requested = index;
    return NULL;
  }
  size_t position = lookahead->position;
  lookahead->active = index;
  complete_ambiguous_substitutions(lookahead);
  lookahead_seek(lookahead, position);
  return lookahead->failed ? NULL : &lookahead->substitutions[index];
}

static const struct AmbiguousSubstitution *
lookup_ambiguous_substitution(const struct Scanner *scanner, TSLexer *lexer) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  if (lookahead->length == 0 && !lookahead_append(lookahead, 0)) {
    return NULL;
  }
  for (
    size_t index = lookahead->characters[lookahead->position].substitution;
    index != SIZE_MAX;
    index = lookahead->substitutions[index].next
  ) {
    struct AmbiguousSubstitution *substitution =
      &lookahead->substitutions[index];
    if (
      substitution->start == lookahead->position &&
      substitution->raw == (scanner == NULL) &&
      (scanner == NULL ||
        (substitution->context.backquote_depth == scanner->backquote_depth))
    ) {
      if (!substitution->complete) {
        return request_ambiguous_substitution(lookahead, index);
      }
      return substitution;
    }
  }
  struct AmbiguousSubstitution *substitutions = source_grow(
    lookahead->substitutions,
    &lookahead->substitution_capacity,
    lookahead->substitution_count + 1,
    sizeof(struct AmbiguousSubstitution)
  );
  if (substitutions == NULL) {
    lookahead_fail(lookahead);
    return NULL;
  }
  lookahead->substitutions = substitutions;
  size_t index = lookahead->substitution_count++;
  lookahead->substitutions[index] = (struct AmbiguousSubstitution){
    .start = lookahead->position,
    .parent = lookahead->active,
    .next = lookahead->characters[lookahead->position].substitution,
    .context = scanner == NULL ? (struct Scanner){0} : *scanner,
    .raw = scanner == NULL,
  };
  lookahead->characters[lookahead->position].substitution = index;
  return request_ambiguous_substitution(lookahead, index);
}

static enum ArithmeticValidation
skip_ambiguous_substitution(const struct Scanner *scanner, TSLexer *lexer) {
  const struct AmbiguousSubstitution *substitution =
    lookup_ambiguous_substitution(scanner, lexer);
  if (substitution == NULL) {
    return ARITHMETIC_VALIDATION_INVALID;
  }
  enum ArithmeticValidation result = substitution->result;
  lookahead_seek((struct LookaheadLexer *)lexer, substitution->end);
  return result;
}

static enum ArithmeticValidation skip_embedded_construct(
  const struct Scanner *scanner,
  TSLexer *lexer,
  char initial_closer
);
static bool scan_delimiter_single_quoted_segment(
  TSLexer *lexer,
  struct ByteBuffer *delimiter,
  bool dollar
);

static bool
skip_bracket_member_expansion(const struct Scanner *scanner, TSLexer *lexer) {
  int32_t character = lexer->lookahead;
  if (character == '\'') {
    lexer->advance(lexer, false);
    return scan_delimiter_single_quoted_segment(lexer, NULL, false);
  }
  if (character == '"' || character == '`') {
    lexer->advance(lexer, false);
    return skip_embedded_construct(scanner, lexer, (char)character) ==
      ARITHMETIC_VALIDATION_VALID;
  }
  if (character == '$') {
    lexer->advance(lexer, false);
    int32_t introducer = lexer->lookahead;
    if (introducer == '\'') {
      lexer->advance(lexer, false);
      return scan_delimiter_single_quoted_segment(lexer, NULL, true);
    }
    if (introducer == '(' || introducer == '{') {
      lexer->advance(lexer, false);
      return skip_embedded_construct(
               scanner,
               lexer,
               introducer == '(' ? ')' : '}'
             ) == ARITHMETIC_VALIDATION_VALID;
    }
    skip_parameter_name(lexer, introducer);
  }
  return true;
}

enum BracketAtom {
  BRACKET_ATOM_PLAIN,
  BRACKET_ATOM_MEMBER,
  BRACKET_ATOM_END,
};

static enum BracketAtom
skip_bracket_quoted_atom(const struct Scanner *scanner, TSLexer *lexer) {
  int32_t character = lexer->lookahead;
  if (character == '\\') {
    lexer->advance(lexer, false);
    if (lexer_at_input_end(lexer) || lexer->lookahead == '\n') {
      return BRACKET_ATOM_END;
    }
    lexer->advance(lexer, false);
    return BRACKET_ATOM_MEMBER;
  }
  if (is_quote_or_expansion_start(character)) {
    return skip_bracket_member_expansion(scanner, lexer) ? BRACKET_ATOM_MEMBER
                                                         : BRACKET_ATOM_END;
  }
  return BRACKET_ATOM_PLAIN;
}

static bool pattern_mode_is_parameter(enum PatternMode mode) {
  return mode == PATTERN_PARAMETER || mode == PATTERN_PARAMETER_TILDE;
}

static bool pattern_mode_is_tilde(enum PatternMode mode) {
  return mode == PATTERN_WORD_TILDE ||
    mode == PATTERN_ASSIGNMENT_TILDE ||
    mode == PATTERN_PARAMETER_TILDE;
}

static bool assignment_tilde_follows(
  const TSLexer *lexer,
  enum PatternMode mode,
  int32_t previous
) {
  return mode == PATTERN_ASSIGNMENT &&
    previous == ':' &&
    lexer->lookahead == '~';
}

static bool
is_bracket_literal_boundary(const TSLexer *lexer, enum PatternMode mode) {
  return pattern_boundary(lexer, pattern_mode_is_parameter(mode)) ||
    (mode == PATTERN_ASSIGNMENT_TILDE && lexer->lookahead == ':') ||
    (pattern_mode_is_tilde(mode) && lexer->lookahead == '/');
}

enum BracketInput {
  BRACKET_INPUT_COLON,
  BRACKET_INPUT_OPEN,
  BRACKET_INPUT_CLOSE,
  BRACKET_INPUT_NEGATION,
  BRACKET_INPUT_HYPHEN,
  BRACKET_INPUT_MEMBER,
};

enum BracketClassStage {
  BRACKET_CLASS_EMPTY,
  BRACKET_CLASS_FILLED,
  BRACKET_CLASS_MARKED,
  BRACKET_CLASS_STAGE_COUNT,
};

enum BracketPhase {
  BRACKET_PHASE_NEGATION,
  BRACKET_PHASE_FIRST,
  BRACKET_PHASE_MEMBERS,
  BRACKET_PHASE_RANGE,
  BRACKET_PHASE_ENDPOINT,
  BRACKET_PHASE_OPENED,
  BRACKET_PHASE_CLASS,
  BRACKET_PHASE_COUNT = BRACKET_PHASE_CLASS + BRACKET_CLASS_STAGE_COUNT,
  BRACKET_PHASE_INCOMPLETE = BRACKET_PHASE_COUNT,
  BRACKET_PHASE_COMPLETE,
};

enum { BRACKET_CHAINS_LIMIT = 1 << BRACKET_PHASE_COUNT };

static enum BracketInput bracket_input(int32_t character) {
  switch (character) {
    case ':':
      return BRACKET_INPUT_COLON;
    case '[':
      return BRACKET_INPUT_OPEN;
    case ']':
      return BRACKET_INPUT_CLOSE;
    case '!':
    case '^':
      return BRACKET_INPUT_NEGATION;
    case '-':
      return BRACKET_INPUT_HYPHEN;
    default:
      return BRACKET_INPUT_MEMBER;
  }
}

static unsigned bracket_phase_after(unsigned phase, enum BracketInput input) {
  if (phase >= BRACKET_PHASE_CLASS) {
    unsigned marker = (phase - BRACKET_PHASE_CLASS) / BRACKET_CLASS_STAGE_COUNT;
    unsigned stage = (phase - BRACKET_PHASE_CLASS) % BRACKET_CLASS_STAGE_COUNT;
    if (input == BRACKET_INPUT_CLOSE) {
      if (stage == BRACKET_CLASS_MARKED) {
        return BRACKET_PHASE_ENDPOINT;
      }
      return BRACKET_PHASE_INCOMPLETE;
    }
    return BRACKET_PHASE_CLASS +
      marker * BRACKET_CLASS_STAGE_COUNT +
      (stage != BRACKET_CLASS_EMPTY && input == (enum BracketInput)marker
          ? BRACKET_CLASS_MARKED
          : BRACKET_CLASS_FILLED);
  }
  if (phase == BRACKET_PHASE_RANGE) {
    return input == BRACKET_INPUT_CLOSE ? BRACKET_PHASE_COMPLETE
                                        : BRACKET_PHASE_ENDPOINT;
  }
  bool leading =
    phase == BRACKET_PHASE_NEGATION || phase == BRACKET_PHASE_FIRST;
  switch (input) {
    case BRACKET_INPUT_OPEN:
      return BRACKET_PHASE_OPENED;
    case BRACKET_INPUT_CLOSE:
      return leading ? BRACKET_PHASE_MEMBERS : BRACKET_PHASE_COMPLETE;
    case BRACKET_INPUT_NEGATION:
      return phase == BRACKET_PHASE_NEGATION ? BRACKET_PHASE_FIRST
                                             : BRACKET_PHASE_MEMBERS;
    case BRACKET_INPUT_HYPHEN:
      return leading || phase == BRACKET_PHASE_ENDPOINT ? BRACKET_PHASE_MEMBERS
                                                        : BRACKET_PHASE_RANGE;
    case BRACKET_INPUT_MEMBER:
      return BRACKET_PHASE_MEMBERS;
    default:
      return phase == BRACKET_PHASE_OPENED
        ? BRACKET_PHASE_CLASS + input * BRACKET_CLASS_STAGE_COUNT
        : BRACKET_PHASE_MEMBERS;
  }
}

static uint16_t bracket_chains_after(uint16_t chains, enum BracketInput input) {
  uint16_t next = 0;
  for (unsigned phase = 0; phase < BRACKET_PHASE_COUNT; phase += 1) {
    if ((chains & (1u << phase)) != 0) {
      unsigned after = bracket_phase_after(phase, input);
      if (after < BRACKET_PHASE_COUNT) {
        next |= (uint16_t)(1u << after);
      }
    }
  }
  return next;
}

static uint16_t
bracket_chains_after_literal(uint16_t chains, int32_t character) {
  chains = bracket_chains_after(chains, bracket_input(character));
  return character == '[' ? chains | (1u << BRACKET_PHASE_NEGATION) : chains;
}

static bool is_bracket_fallback_context(enum TokenType opener) {
  return opener == WORD_FALLBACK_LITERAL_BEGIN ||
    opener == ASSIGNMENT_FALLBACK_LITERAL_BEGIN ||
    opener == PARAMETER_FALLBACK_LITERAL_BEGIN;
}

static bool in_bracket_fallback(const struct Scanner *scanner) {
  return is_bracket_fallback_context(top_context_opener(scanner));
}

static uint16_t fallback_chains(const struct Scanner *scanner) {
  return in_bracket_fallback(scanner)
    ? scanner->contexts[scanner->context_count - 1].bracket_chains
    : 0;
}

static int32_t lookahead_peek(TSLexer *lexer) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  size_t position = lookahead->position;
  lexer->advance(lexer, false);
  int32_t value = lexer->lookahead;
  lookahead_seek(lookahead, position);
  return value;
}

static bool scan_class_terminator(TSLexer *lexer, bool parameter) {
  lexer->advance(lexer, false);
  lexer->advance(lexer, false);
  int32_t previous = 0;
  size_t length = 0;
  while (!pattern_boundary(lexer, parameter) && lexer->lookahead != ']') {
    previous = lexer->lookahead;
    lexer->advance(lexer, false);
    length += 1;
  }
  return lexer->lookahead == ']' && previous == ':' && length >= 2;
}

static bool class_terminator_follows(TSLexer *lexer, bool parameter) {
  bool lookahead = lexer->advance == lookahead_advance;
  size_t position = lookahead ? ((struct LookaheadLexer *)lexer)->position : 0;
  struct LogicalPosition logical = {0};
  if (!lookahead) {
    logical = logical_position((struct LogicalLexer *)lexer);
  }
  bool found = scan_class_terminator(lexer, parameter);
  if (lookahead) {
    lookahead_seek((struct LookaheadLexer *)lexer, position);
  } else {
    logical_restore_position((struct LogicalLexer *)lexer, &logical);
  }
  return found;
}

struct ClassTerminatorMemo {
  size_t closing;
  bool follows;
  bool scanned;
};

static bool class_terminator_ahead(
  struct LookaheadLexer *lookahead,
  bool parameter,
  struct ClassTerminatorMemo *memo
) {
  size_t opener = lookahead->position;
  size_t raw = lookahead->characters[opener].raw_cut;
  if (!memo->scanned || memo->closing <= raw) {
    memo->follows = scan_class_terminator(&lookahead->lexer, parameter);
    memo->closing = lookahead->lexer.lookahead == ']'
      ? lookahead->characters[lookahead->position].raw_cut
      : SIZE_MAX;
    memo->scanned = true;
    lookahead_seek(lookahead, opener);
  }
  return memo->follows;
}

static bool scan_pattern_bracket(
  const struct Scanner *scanner,
  TSLexer *lexer,
  enum PatternMode mode,
  uint16_t *chains,
  bool *complete
) {
  if (lexer->lookahead != '[') {
    return false;
  }
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  lexer->advance(lexer, false);
  size_t opener_end = lookahead->position;
  unsigned phase = BRACKET_PHASE_NEGATION;
  struct ClassTerminatorMemo terminator = {0};
  *chains = bracket_chains_after(*chains, BRACKET_INPUT_OPEN);
  while (!is_bracket_literal_boundary(lexer, mode)) {
    int32_t character = lexer->lookahead;
    enum BracketAtom atom = skip_bracket_quoted_atom(scanner, lexer);
    if (atom == BRACKET_ATOM_END) {
      break;
    }
    enum BracketInput input = atom == BRACKET_ATOM_MEMBER
      ? BRACKET_INPUT_MEMBER
      : bracket_input(character);
    if (
      input == BRACKET_INPUT_OPEN &&
      phase < BRACKET_PHASE_CLASS &&
      lookahead_peek(lexer) == ':' &&
      !class_terminator_ahead(
        lookahead,
        pattern_mode_is_parameter(mode),
        &terminator
      )
    ) {
      input = BRACKET_INPUT_MEMBER;
    }
    phase = bracket_phase_after(phase, input);
    *chains = bracket_chains_after(*chains, input);
    if (phase >= BRACKET_PHASE_COUNT) {
      break;
    }
    if (atom == BRACKET_ATOM_PLAIN) {
      lexer->advance(lexer, false);
      if (assignment_tilde_follows(lexer, mode, character)) {
        break;
      }
    }
    if ((*chains & (1u << phase)) != 0) {
      break;
    }
  }
  *complete = phase == BRACKET_PHASE_COMPLETE;
  size_t end = lookahead->position;
  lookahead_seek(lookahead, opener_end);
  lexer->mark_end(lexer);
  lookahead_seek(lookahead, end);
  return true;
}

static bool scan_pattern_bracket_hyphen(TSLexer *lexer, const bool *valid) {
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  enum TokenType symbol = PATTERN_BRACKET_HYPHEN;
  if (
    valid[PATTERN_BRACKET_RANGE_OPERATOR] &&
    !pattern_boundary(lexer, valid[PARAMETER_PATTERN_BRACKET_CHARACTER]) &&
    lexer->lookahead != ']'
  ) {
    symbol = PATTERN_BRACKET_RANGE_OPERATOR;
  }
  if (!valid[symbol]) {
    return false;
  }
  lexer->result_symbol = (TSSymbol)symbol;
  return true;
}

static bool scan_pattern_bracket_character(
  struct Scanner *scanner,
  TSLexer *lexer,
  enum TokenType symbol
) {
  int32_t character = lexer->lookahead;
  if (
    is_one_of(character, "[]-!^*?:.=\\") ||
    is_quote_or_expansion_start(character) ||
    pattern_boundary(lexer, symbol == PARAMETER_PATTERN_BRACKET_CHARACTER)
  ) {
    return false;
  }
  if (character == '\n' && scanner->active_frame.count > 0) {
    scanner->active_frame.at_line_start = true;
  }
  return accept_character(lexer, symbol);
}

enum DelimiterQuote {
  DELIMITER_UNQUOTED,
  DELIMITER_SINGLE_QUOTED,
  DELIMITER_DOUBLE_QUOTED,
  DELIMITER_PARAMETER_DOUBLE_QUOTED,
  DELIMITER_ARITHMETIC,
  DELIMITER_DOLLAR_SINGLE_QUOTED,
};

enum ParameterQuoteState {
  PARAMETER_QUOTE_START,
  PARAMETER_QUOTE_NAME,
  PARAMETER_QUOTE_POSITIONAL,
  PARAMETER_QUOTE_OPERATOR,
  PARAMETER_QUOTE_COLON,
  PARAMETER_QUOTE_WORD,
  PARAMETER_QUOTE_PATTERN,
};

static enum ParameterQuoteState
parameter_quote_state_after(enum ParameterQuoteState state, int32_t character) {
  switch (state) {
    case PARAMETER_QUOTE_START:
      if (is_name_start_character(character)) {
        return PARAMETER_QUOTE_NAME;
      }
      if (is_decimal_digit(character)) {
        return PARAMETER_QUOTE_POSITIONAL;
      }
      return is_special_parameter_character(character)
        ? PARAMETER_QUOTE_OPERATOR
        : PARAMETER_QUOTE_WORD;
    case PARAMETER_QUOTE_NAME:
      if (is_name_character(character)) {
        return state;
      }
      break;
    case PARAMETER_QUOTE_POSITIONAL:
      if (is_decimal_digit(character)) {
        return state;
      }
      break;
    case PARAMETER_QUOTE_OPERATOR:
      break;
    case PARAMETER_QUOTE_COLON:
      return PARAMETER_QUOTE_WORD;
    case PARAMETER_QUOTE_WORD:
    case PARAMETER_QUOTE_PATTERN:
      return state;
  }
  if (character == ':') return PARAMETER_QUOTE_COLON;
  return character == '#' || character == '%' ? PARAMETER_QUOTE_PATTERN
                                              : PARAMETER_QUOTE_WORD;
}

enum DelimiterGroupKind {
  DELIMITER_GROUP_PARAMETER,
  DELIMITER_GROUP_DOUBLE_QUOTE,
  DELIMITER_GROUP_COMMAND,
  DELIMITER_GROUP_ARITHMETIC,
  DELIMITER_GROUP_ARITHMETIC_PAREN,
  DELIMITER_GROUP_BACKQUOTE,
};

struct CommandWord {
  char text[6];
  uint8_t length;
  bool active;
  bool candidate;
  bool digits;
  bool name;
};

enum CommandPosition {
  COMMAND_POSITION_START,
  COMMAND_POSITION_WORD,
  COMMAND_POSITION_CLOSED,
  COMMAND_POSITION_FOR_NAME,
  COMMAND_POSITION_FOR_IN,
};

struct CommandTracker {
  struct CommandWord word;
  enum CommandPosition position;
  bool redirect_operand;
  bool function_name;
};

struct DelimiterGroupFrame {
  int32_t closing;
  enum DelimiterGroupKind kind;
  enum DelimiterQuote parent_quote;
  /* ${...} inherits backslash handling except in double-quoted patterns. */
  bool keeps_backslash;
};

struct DelimiterGroupBuffer {
  struct DelimiterGroupFrame *data;
  size_t length;
  size_t capacity;
  bool failed;
};

enum CaseTrackerState {
  CASE_TRACKER_EXPECT_WORD,
  CASE_TRACKER_EXPECT_IN,
  CASE_TRACKER_EXPECT_PATTERN,
  CASE_TRACKER_IN_PATTERN,
  CASE_TRACKER_BODY,
};

struct CaseTracker {
  size_t depth;
  uint8_t state;
};

static bool case_tracker_in_pattern(uint8_t state) {
  return (
    state == CASE_TRACKER_EXPECT_PATTERN || state == CASE_TRACKER_IN_PATTERN
  );
}

struct CaseTrackerBuffer {
  struct CaseTracker *data;
  size_t length;
  size_t capacity;
  bool failed;
};

static struct CaseTracker *
active_case_tracker(const struct CaseTrackerBuffer *cases, size_t depth) {
  if (cases->length == 0) {
    return NULL;
  }
  struct CaseTracker *tracker = &cases->data[cases->length - 1];
  return tracker->depth == depth ? tracker : NULL;
}

static void
pop_case_trackers_at_depth(struct CaseTrackerBuffer *cases, size_t depth) {
  while (cases->length > 0 && cases->data[cases->length - 1].depth >= depth) {
    cases->length -= 1;
  }
}

enum CaseWordKind {
  CASE_WORD_GENERIC,
  CASE_WORD_IN,
  CASE_WORD_ESAC,
  CASE_WORD_CASE,
  CASE_WORD_FOR,
  CASE_WORD_DO,
  CASE_WORD_COMMAND_CLOSE,
  CASE_WORD_COMMAND_PREFIX,
};

static enum CaseWordKind classify_case_word(const char *word) {
  static const struct {
    const char *text;
    enum CaseWordKind kind;
  } CASE_WORDS[] = {
    {"in", CASE_WORD_IN},
    {"esac", CASE_WORD_ESAC},
    {"case", CASE_WORD_CASE},
    {"for", CASE_WORD_FOR},
    {"do", CASE_WORD_DO},
    {"fi", CASE_WORD_COMMAND_CLOSE},
    {"done", CASE_WORD_COMMAND_CLOSE},
    {"}", CASE_WORD_COMMAND_CLOSE},
    {"!", CASE_WORD_COMMAND_PREFIX},
    {"{", CASE_WORD_COMMAND_PREFIX},
    {"if", CASE_WORD_COMMAND_PREFIX},
    {"then", CASE_WORD_COMMAND_PREFIX},
    {"elif", CASE_WORD_COMMAND_PREFIX},
    {"else", CASE_WORD_COMMAND_PREFIX},
    {"while", CASE_WORD_COMMAND_PREFIX},
    {"until", CASE_WORD_COMMAND_PREFIX},
  };
  for (size_t index = 0; index < COUNT_OF(CASE_WORDS); index += 1) {
    if (strcmp(word, CASE_WORDS[index].text) == 0) {
      return CASE_WORDS[index].kind;
    }
  }
  return CASE_WORD_GENERIC;
}

enum CaseTrackerNote {
  CASE_TRACKER_NOTE_WORD,
  CASE_TRACKER_NOTE_COMMAND_PREFIX,
  CASE_TRACKER_NOTE_END,
  CASE_TRACKER_NOTE_BEGIN,
};

static enum CaseTrackerNote case_tracker_note_word(
  struct CaseTracker *tracker,
  enum CaseWordKind kind,
  bool at_command_start
) {
  if (tracker != NULL && tracker->state != CASE_TRACKER_BODY) {
    switch (tracker->state) {
      case CASE_TRACKER_EXPECT_WORD:
        tracker->state = CASE_TRACKER_EXPECT_IN;
        break;
      case CASE_TRACKER_EXPECT_IN:
        if (kind == CASE_WORD_IN) {
          tracker->state = CASE_TRACKER_EXPECT_PATTERN;
        }
        break;
      case CASE_TRACKER_EXPECT_PATTERN:
        if (kind == CASE_WORD_ESAC) {
          return CASE_TRACKER_NOTE_END;
        }
        tracker->state = CASE_TRACKER_IN_PATTERN;
        break;
      default:
        break;
    }
    return CASE_TRACKER_NOTE_WORD;
  }

  if (!at_command_start) {
    return CASE_TRACKER_NOTE_WORD;
  }
  if (kind == CASE_WORD_ESAC && tracker != NULL) {
    return CASE_TRACKER_NOTE_END;
  }
  if (kind == CASE_WORD_COMMAND_PREFIX || kind == CASE_WORD_DO) {
    return CASE_TRACKER_NOTE_COMMAND_PREFIX;
  }
  if (kind == CASE_WORD_CASE) {
    return CASE_TRACKER_NOTE_BEGIN;
  }
  return CASE_TRACKER_NOTE_WORD;
}

static bool append_case_tracker(struct CaseTrackerBuffer *cases, size_t depth) {
  struct CaseTracker *data = source_grow(
    cases->data,
    &cases->capacity,
    cases->length + 1,
    sizeof(struct CaseTracker)
  );
  if (data == NULL) {
    cases->failed = true;
    return false;
  }
  cases->data = data;
  cases->data[cases->length] = (struct CaseTracker){
    .depth = depth,
    .state = CASE_TRACKER_EXPECT_WORD,
  };
  cases->length += 1;
  return true;
}

static bool push_delimiter_group(
  struct DelimiterGroupBuffer *groups,
  int32_t closing,
  enum DelimiterGroupKind kind,
  enum DelimiterQuote parent_quote
) {
  if (groups->length >= SCANNER_STATE_CAPACITY) {
    groups->failed = true;
    return false;
  }
  struct DelimiterGroupFrame *data = source_grow(
    groups->data,
    &groups->capacity,
    groups->length + 1,
    sizeof(struct DelimiterGroupFrame)
  );
  if (data == NULL) {
    groups->failed = true;
    return false;
  }
  groups->data = data;
  groups->data[groups->length] = (struct DelimiterGroupFrame){
    .closing = closing,
    .kind = kind,
    .parent_quote = parent_quote,
    .keeps_backslash = kind == DELIMITER_GROUP_COMMAND ||
      kind == DELIMITER_GROUP_BACKQUOTE ||
      (groups->length > 0 && data[groups->length - 1].keeps_backslash),
  };
  groups->length += 1;
  return true;
}

static bool is_delimiter_word_character(int32_t character) {
  return is_name_character(character) || is_one_of(character, "!{}");
}

static void reset_command_word(struct CommandWord *word) {
  word->length = 0;
  word->active = false;
  word->candidate = true;
  word->digits = true;
  word->name = true;
}

static void reset_command_position(
  struct CommandTracker *command,
  enum CommandPosition position
) {
  reset_command_word(&command->word);
  command->position = position;
  command->redirect_operand = false;
  command->function_name = false;
}

static bool finish_command_word(
  struct CaseTrackerBuffer *cases,
  size_t depth,
  struct CommandTracker *command,
  bool before_redirect
) {
  struct CommandWord *word = &command->word;
  if (!word->active) {
    reset_command_word(word);
    return true;
  }
  if (word->digits && before_redirect) {
    reset_command_word(word);
    return true;
  }
  command->function_name = false;
  if (command->redirect_operand) {
    command->redirect_operand = false;
    reset_command_word(word);
    return true;
  }

  char text[sizeof(word->text) + 1] = {0};
  if (word->candidate) {
    memcpy(text, word->text, word->length);
  }
  enum CaseWordKind kind =
    word->candidate ? classify_case_word(text) : CASE_WORD_GENERIC;
  struct CaseTracker *tracker = active_case_tracker(cases, depth);
  bool in_case_header = tracker != NULL && tracker->state != CASE_TRACKER_BODY;
  enum CommandPosition position = command->position;
  bool at_command_start =
    position == COMMAND_POSITION_START || position == COMMAND_POSITION_CLOSED;
  command->position = COMMAND_POSITION_WORD;
  switch (case_tracker_note_word(tracker, kind, at_command_start)) {
    case CASE_TRACKER_NOTE_COMMAND_PREFIX:
      command->position = COMMAND_POSITION_START;
      break;
    case CASE_TRACKER_NOTE_END:
      cases->length -= 1;
      command->position = COMMAND_POSITION_CLOSED;
      break;
    case CASE_TRACKER_NOTE_BEGIN:
      if (!append_case_tracker(cases, depth)) {
        return false;
      }
      break;
    default:
      if (in_case_header) {
        break;
      }
      if (position == COMMAND_POSITION_FOR_NAME) {
        command->position = COMMAND_POSITION_FOR_IN;
      } else if (position == COMMAND_POSITION_FOR_IN) {
        if (kind == CASE_WORD_DO) {
          command->position = COMMAND_POSITION_START;
        }
      } else if (at_command_start) {
        if (kind == CASE_WORD_FOR) {
          command->position = COMMAND_POSITION_FOR_NAME;
        } else if (kind == CASE_WORD_COMMAND_CLOSE) {
          command->position = COMMAND_POSITION_CLOSED;
        } else {
          command->function_name = word->name;
        }
      }
      break;
  }
  reset_command_word(word);
  return true;
}

static void begin_command_redirect(struct CommandTracker *command) {
  if (command->position != COMMAND_POSITION_CLOSED) {
    command->position = COMMAND_POSITION_WORD;
  }
  command->redirect_operand = true;
  command->function_name = false;
}

static void
track_command_word_character(struct CommandWord *word, int32_t character) {
  if (!word->active) {
    reset_command_word(word);
    word->name = is_name_start_character(character);
    word->active = true;
  } else if (!is_name_character(character)) {
    word->name = false;
  }
  if (!is_decimal_digit(character)) {
    word->digits = false;
  }

  if (!is_delimiter_word_character(character)) {
    word->candidate = false;
    return;
  }

  if (word->length < sizeof(word->text)) {
    word->text[word->length] = (char)character;
    word->length += 1;
  } else {
    word->candidate = false;
  }
}

static uint8_t hexadecimal_value(int32_t character) {
  if (character >= '0' && character <= '9') {
    return (uint8_t)(character - '0');
  }

  if (character >= 'A' && character <= 'F') {
    return (uint8_t)(character - 'A' + 10);
  }

  return (uint8_t)(character - 'a' + 10);
}

static bool defined_control_escape_byte(int32_t character, uint8_t *value) {
  if (character >= 'a' && character <= 'z') {
    *value = (uint8_t)(character - 'a' + 1);
    return true;
  }
  if (character >= 'A' && character <= 'Z') {
    *value = (uint8_t)(character - 'A' + 1);
    return true;
  }

  switch (character) {
    case '[':
      *value = 0x1b;
      return true;
    case '\\':
      *value = 0x1c;
      return true;
    case ']':
      *value = 0x1d;
      return true;
    case '^':
      *value = 0x1e;
      return true;
    case '_':
      *value = 0x1f;
      return true;
    case '?':
      *value = 0x7f;
      return true;
    default:
      return false;
  }
}

// dash masks only the first byte of a multibyte operand.
static bool
append_control_escape(struct ByteBuffer *buffer, int32_t character) {
  uint8_t bytes[4];
  size_t length = 1;
  if (!defined_control_escape_byte(character, bytes)) {
    if (!encode_utf8_scalar(character, bytes, &length)) {
      return false;
    }
    bytes[0] &= UINT8_C(0x1f);
  }
  return append_bytes(buffer, bytes, length);
}

static bool
scan_dollar_single_quote_escape(TSLexer *lexer, struct ByteBuffer *delimiter) {
  int32_t character = lexer->lookahead;
  if (lexer_at_eof(lexer)) {
    return false;
  }

  if (character == 'x' || character == 'u' || character == 'U') {
    unsigned limit = character == 'x' ? 2 : character == 'u' ? 4 : 8;
    lexer->advance(lexer, false);
    uint32_t value = 0;
    for (
      unsigned digits = 0;
      digits < limit && is_hexadecimal_digit(lexer->lookahead);
      digits += 1
    ) {
      value = (value << 4) | hexadecimal_value(lexer->lookahead);
      lexer->advance(lexer, false);
    }
    if (character == 'x') {
      return append_byte(delimiter, (uint8_t)value);
    }
    return value > 0x10ffff || append_codepoint(delimiter, (int32_t)value);
  }

  if (character >= '0' && character <= '7') {
    uint8_t value = 0;
    uint8_t digits = 0;
    while (digits < 3 && lexer->lookahead >= '0' && lexer->lookahead <= '7') {
      value = (uint8_t)((value << 3) | (lexer->lookahead - '0'));
      digits += 1;
      lexer->advance(lexer, false);
    }
    return append_byte(delimiter, value);
  }

  if (character == 'c') {
    lexer->advance(lexer, false);
    if (lexer_at_input_end(lexer)) {
      return false;
    }
    int32_t operand = lexer->lookahead;
    lexer->advance(lexer, false);
    if (operand == '\\' && lexer->lookahead == '\\') {
      lexer->advance(lexer, false);
    }
    return append_control_escape(delimiter, operand);
  }

  lexer->advance(lexer, false);
  return append_quoted_escape(delimiter, character);
}

enum HereDocumentLineKind {
  HERE_DOCUMENT_LINE_DELIMITER,
  HERE_DOCUMENT_LINE_LAYOUT,
  HERE_DOCUMENT_LINE_CONTENT,
  HERE_DOCUMENT_LINE_END_OF_INPUT,
  HERE_DOCUMENT_LINE_INVALID,
};

struct CommandStart {
  int32_t first_character;
  int32_t second_character;
  char first_word[RESERVED_WORD_CAPACITY];
  bool first_is_delimited;
  bool first_word_is_reserved_candidate;
};

static bool match_here_document_delimiter_character(
  const struct HereDocument *document,
  size_t *offset,
  int32_t character
) {
  uint8_t bytes[4];
  size_t length;
  if (
    !encode_utf8_scalar(character, bytes, &length) ||
    length > document->delimiter_length - *offset ||
    memcmp(document->delimiter + *offset, bytes, length) != 0
  ) {
    return false;
  }
  *offset += length;
  return true;
}

struct HereDocumentBoundary {
  struct LogicalLexer view;
  const struct HereDocument *document;
  size_t context;
  size_t frame;
  size_t end;
  bool at_line_start;
  bool matched;
};

struct HereDocumentInput {
  struct HereDocumentBoundary *boundaries;
  size_t count;
  struct HereDocumentBoundary *end;
};

static void here_document_input_clear(struct HereDocumentInput *input) {
  for (size_t index = 0; index < input->count; index += 1) {
    logical_clear(&input->boundaries[index].view);
  }
  ts_free(input->boundaries);
  *input = (struct HereDocumentInput){0};
}

static const struct HereDocumentFrame *
here_document_frame_at(const struct Scanner *scanner, size_t index) {
  if (index < scanner->suspended_frame_count) {
    return &scanner->suspended_frames[index];
  }
  if (
    index == scanner->suspended_frame_count && scanner->active_frame.count > 0
  ) {
    return &scanner->active_frame;
  }
  return NULL;
}

static bool here_document_frame_applies(
  const struct Scanner *scanner,
  const struct HereDocumentFrame *frame,
  const struct SourceContext *context
) {
  return !frame->documents[0].ending &&
    scanner->substitution_depth <= context->substitution_depth &&
    scanner->backquote_depth <= frame->body_backquote_depth;
}

static size_t
here_document_context_at(const struct Scanner *scanner, size_t frame) {
  size_t ordinal = 0;
  for (size_t index = 0; index < scanner->context_count; index += 1) {
    if (
      is_here_document_context(scanner->contexts[index].opener) &&
      ordinal++ == frame
    ) {
      return index;
    }
  }
  return SIZE_MAX;
}

static bool active_document_applies(const struct Scanner *scanner) {
  size_t context =
    here_document_context_at(scanner, scanner->suspended_frame_count);
  return scanner->active_frame.count > 0 &&
    context != SIZE_MAX &&
    here_document_frame_applies(
      scanner,
      &scanner->active_frame,
      &scanner->contexts[context]
    );
}

static bool here_document_input_init(
  struct HereDocumentInput *input,
  struct NativeInput *native,
  const struct Scanner *scanner
) {
  *input = (struct HereDocumentInput){0};
  if (scanner->active_frame.count == 0 && scanner->suspended_frame_count == 0) {
    return true;
  }
  if (scanner->suspended_frame_count >= SIZE_MAX / sizeof(*input->boundaries)) {
    return false;
  }
  input->boundaries =
    ts_calloc(scanner->suspended_frame_count + 1, sizeof(*input->boundaries));
  if (input->boundaries == NULL) {
    return false;
  }
  size_t frame_count = 0;
  for (size_t index = 0; index < scanner->context_count; index += 1) {
    const struct SourceContext *context = &scanner->contexts[index];
    if (!is_here_document_context(context->opener)) {
      continue;
    }
    size_t document_frame = frame_count++;
    const struct HereDocumentFrame *frame =
      here_document_frame_at(scanner, document_frame);
    if (frame == NULL) {
      goto failure;
    }
    if (!here_document_frame_applies(scanner, frame, context)) {
      continue;
    }
    struct SourceSnapshot snapshot = scanner->source;
    snapshot.stage_count = context->stage_count + 1;
    if (snapshot.stage_count > scanner->source.stage_count) {
      goto failure;
    }
    struct HereDocumentBoundary *boundary = &input->boundaries[input->count];
    *boundary = (struct HereDocumentBoundary){
      .document = frame->documents,
      .context = index,
      .frame = document_frame,
      .at_line_start = snapshot.stages[snapshot.stage_count - 1].at_line_start,
    };
    if (!logical_init_at(&boundary->view, native, &snapshot, 0, false)) {
      goto failure;
    }
    input->count += 1;
  }
  return true;

failure:
  here_document_input_clear(input);
  return false;
}

static void here_document_boundary_advance(struct HereDocumentBoundary *bound) {
  bound->at_line_start = bound->view.lexer.lookahead == '\n' &&
    bound->view.cursor.stages[bound->view.cursor.stage_count - 1].at_line_start;
  bound->view.lexer.advance(&bound->view.lexer, false);
}

struct HereDocumentMatch {
  size_t end;
  bool matched;
};

/* Only the first line's tabs are stripped while matching a delimiter. */
static bool match_here_document_delimiter(
  const struct LogicalLexer *view,
  const struct HereDocument *document,
  struct HereDocumentMatch *match
) {
  *match = (struct HereDocumentMatch){0};
  if (view->result == SOURCE_FAILURE) return false;
  if (view->result == SOURCE_END) return true;
  size_t offset = 0;
  if (
    document->delimiter_length > 0 &&
    !match_here_document_delimiter_character(
      document,
      &offset,
      view->lexer.lookahead
    )
  )
    return true;

  size_t inherited =
    here_document_stage_index(view->cursor.initial, view->cursor.stage_count);
  if (inherited == SIZE_MAX || view->cursor.initial[inherited].disabled) {
    inherited = view->cursor.stage_count;
  }
  struct SourceStage raw = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  raw.disabled = true;
  struct LogicalLexer probe;
  if (!logical_fork(
        &probe,
        view,
        logical_raw_position(view),
        inherited,
        &raw,
        1
      )) {
    return false;
  }
  offset = 0;
  bool matches = true;
  while (
    offset < document->delimiter_length && probe.result == SOURCE_CHARACTER
  ) {
    if (
      probe.lexer.lookahead == SOURCE_BACKQUOTE_BOUNDARY ||
      !match_here_document_delimiter_character(
        document,
        &offset,
        probe.lexer.lookahead
      )
    ) {
      matches = false;
      break;
    }
    probe.lexer.advance(&probe.lexer, false);
  }
  match->matched = matches &&
    offset == document->delimiter_length &&
    (probe.result == SOURCE_END ||
      probe.lexer.lookahead == '\n' ||
      probe.lexer.lookahead == SOURCE_BACKQUOTE_BOUNDARY);
  if (match->matched && probe.lexer.lookahead == '\n') {
    probe.lexer.advance(&probe.lexer, false);
  }
  match->end = probe.consumed;
  bool valid = probe.result != SOURCE_FAILURE;
  logical_clear(&probe);
  return valid;
}

static enum SourceResult here_document_boundary_read(
  struct HereDocumentBoundary *bound,
  size_t position
) {
  struct LogicalLexer *view = &bound->view;
  while (!bound->matched && position >= view->consumed) {
    if (view->result == SOURCE_FAILURE) {
      return SOURCE_FAILURE;
    }
    if (bound->at_line_start) {
      size_t start = view->consumed;
      struct HereDocumentMatch match;
      if (!match_here_document_delimiter(view, bound->document, &match)) {
        return SOURCE_FAILURE;
      }
      if (match.matched) {
        bound->end = start;
        bound->matched = true;
        break;
      }
    }
    if (
      view->result == SOURCE_END ||
      view->lexer.lookahead == SOURCE_BACKQUOTE_BOUNDARY
    ) {
      break;
    }
    here_document_boundary_advance(bound);
  }
  return bound->matched && position >= bound->end ? SOURCE_END
                                                  : SOURCE_CHARACTER;
}

static enum SourceResult
here_document_input_read(void *payload, size_t position) {
  struct HereDocumentInput *input = payload;
  for (size_t index = 0; index < input->count; index += 1) {
    struct HereDocumentBoundary *bound = &input->boundaries[index];
    enum SourceResult result = here_document_boundary_read(bound, position);
    if (result != SOURCE_CHARACTER) {
      if (result == SOURCE_END) {
        input->end = bound;
      }
      return result;
    }
  }
  return SOURCE_CHARACTER;
}

static bool here_document_source_view(
  struct LogicalLexer *view,
  struct LogicalLexer *input,
  const struct Scanner *scanner,
  const struct HereDocument *document
) {
  if (input == NULL || input->cursor.stage_count == 0) {
    return false;
  }
  size_t active = scanner != NULL &&
      scanner->active_frame.count > 0 &&
      document == &scanner->active_frame.documents[0]
    ? here_document_context_at(scanner, scanner->suspended_frame_count)
    : SIZE_MAX;
  if (active != SIZE_MAX) {
    const struct SourceContext *context = &scanner->contexts[active];
    const struct SourceStage *body =
      &input->cursor.initial[context->stage_count];
    if (body->kind == SOURCE_HERE_DOCUMENT && body->disabled) {
      return logical_fork(
        view,
        input,
        input->consumed,
        input->cursor.stage_count,
        NULL,
        0
      );
    }
    struct SourceStage local = source_stage(SOURCE_REMOVE_CONTINUATIONS);
    local.disabled = true;
    return logical_fork(
      view,
      input,
      input->consumed,
      context->stage_count + 1,
      &local,
      1
    );
  }
  struct SourceStage stages[3];
  here_document_body_stages(document, stages);
  return logical_fork(
    view,
    input,
    input->consumed,
    input->cursor.stage_count - 1,
    stages,
    3
  );
}

static bool here_document_line_start(
  struct LogicalLexer *input,
  size_t end,
  struct CommandStart *start
) {
  *start = (struct CommandStart){0};
  struct LogicalLexer view;
  if (!logical_fork(
        &view,
        input,
        input->consumed,
        input->cursor.stage_count,
        NULL,
        0
      )) {
    return false;
  }
  size_t word_length = 0;
  bool has_content = false;
  bool has_second_character = false;
  bool in_word = false;
  bool awaiting_delimiter = false;
  while (
    view.result == SOURCE_CHARACTER &&
    view.base + (size_t)view.character.origin.character < end
  ) {
    int32_t current = view.lexer.lookahead;
    if (current == '\n') {
      break;
    }
    if (has_content && !has_second_character) {
      start->second_character = current;
      has_second_character = true;
    }
    if (!has_content && !is_horizontal_blank(current)) {
      has_content = true;
      start->first_character = current;
      in_word = is_lowercase_letter(current);
      start->first_word_is_reserved_candidate = in_word;
      awaiting_delimiter = !in_word;
      if (in_word) {
        start->first_word[word_length++] = (char)current;
      }
    } else if (in_word) {
      if (is_lowercase_letter(current)) {
        if (word_length < sizeof(start->first_word) - 1) {
          start->first_word[word_length++] = (char)current;
        } else {
          start->first_word_is_reserved_candidate = false;
        }
      } else {
        in_word = false;
        start->first_is_delimited = is_token_delimiter_character(current);
      }
    } else if (awaiting_delimiter) {
      awaiting_delimiter = false;
      start->first_is_delimited = is_token_delimiter_character(current);
    }
    view.lexer.advance(&view.lexer, false);
  }
  if (in_word || awaiting_delimiter) {
    start->first_is_delimited = true;
  }
  bool valid = view.result != SOURCE_FAILURE;
  logical_clear(&view);
  return valid;
}

static enum HereDocumentLineKind read_here_document_line(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const struct HereDocument *document,
  struct CommandStart *start
) {
  struct LogicalLexer *input = lookahead_input(lexer);
  if (input == NULL) {
    return HERE_DOCUMENT_LINE_INVALID;
  }
  if (start != NULL) {
    *start = (struct CommandStart){0};
  }
  struct LogicalLexer view;
  if (!here_document_source_view(&view, input, scanner, document)) {
    input->native->failed = true;
    return HERE_DOCUMENT_LINE_INVALID;
  }
  bool at_end = view.result == SOURCE_END ||
    view.lexer.lookahead == SOURCE_BACKQUOTE_BOUNDARY;
  if (
    view.result == SOURCE_END || (at_end && document->delimiter_length != 0)
  ) {
    logical_clear(&view);
    return HERE_DOCUMENT_LINE_END_OF_INPUT;
  }
  struct HereDocumentMatch match;
  bool valid = match_here_document_delimiter(&view, document, &match);
  while (valid && view.result == SOURCE_CHARACTER && !match.matched) {
    int32_t character = view.lexer.lookahead;
    if (character == SOURCE_BACKQUOTE_BOUNDARY) {
      break;
    }
    view.lexer.advance(&view.lexer, false);
    if (character == '\n') {
      break;
    }
  }
  size_t end = match.matched ? match.end
    : at_end                 ? logical_raw_position(&view)
                             : view.consumed;
  valid = valid &&
    view.result != SOURCE_FAILURE &&
    (start == NULL || at_end || here_document_line_start(input, end, start));
  logical_clear(&view);
  if (
    !valid ||
    !lookahead_set_stages_at(lexer, end, input->cursor.stage_count, NULL, 0)
  ) {
    input->native->failed = true;
    return HERE_DOCUMENT_LINE_INVALID;
  }
  if (match.matched) {
    return HERE_DOCUMENT_LINE_DELIMITER;
  }
  if (
    start != NULL &&
    (start->first_character == 0 || start->first_character == '#')
  ) {
    return HERE_DOCUMENT_LINE_LAYOUT;
  }
  return HERE_DOCUMENT_LINE_CONTENT;
}

static bool scan_dollar_single_quote_token(TSLexer *lexer) {
  lexer->advance(lexer, false);
  if (!scan_dollar_single_quote_escape(lexer, NULL)) {
    return false;
  }
  return emit_symbol(lexer, DOLLAR_SINGLE_QUOTE_ESCAPE);
}

static bool scan_delimiter_single_quoted_segment(
  TSLexer *lexer,
  struct ByteBuffer *delimiter,
  bool dollar
) {
  bool disabled = lookahead_local_disabled(lexer);
  if (!lookahead_local_policy(lexer, true)) {
    return false;
  }
  bool terminated = false;
  while (!lexer_at_eof(lexer)) {
    int32_t character = lexer->lookahead;
    if (character == '\'') {
      lexer->advance(lexer, false);
      return lookahead_local_policy(lexer, disabled);
    }
    if (dollar && character == '\\') {
      lexer->advance(lexer, false);
      size_t start = delimiter == NULL ? 0 : delimiter->length;
      if (!scan_dollar_single_quote_escape(
            lexer,
            terminated ? NULL : delimiter
          )) {
        return false;
      }
      if (delimiter != NULL && !terminated) {
        for (size_t index = start; index < delimiter->length; index += 1) {
          if (delimiter->data[index] == 0) {
            delimiter->length = index;
            terminated = true;
            break;
          }
        }
      }
      continue;
    }
    if (!append_codepoint(terminated ? NULL : delimiter, character)) {
      return false;
    }
    lexer->advance(lexer, false);
  }
  return false;
}

enum DelimiterReadResult {
  DELIMITER_READ_INVALID,
  DELIMITER_READ_RESOURCE_FAILURE,
  DELIMITER_READ_INCOMPLETE,
  DELIMITER_READ_WORD,
};

struct DelimiterReader {
  TSLexer *lexer;
  struct ByteBuffer delimiter;
  struct DelimiterGroupBuffer groups;
  enum DelimiterQuote quote;
  bool has_word_content;
  bool quoted;
  bool valid;
};

enum DelimiterStep {
  DELIMITER_STEP_NEXT,
  DELIMITER_STEP_END,
};

static struct DelimiterGroupFrame *
delimiter_top_group(struct DelimiterReader *reader) {
  return &reader->groups.data[reader->groups.length - 1];
}

static void pop_delimiter_group(struct DelimiterReader *reader) {
  struct DelimiterGroupBuffer *groups = &reader->groups;
  reader->quote = groups->data[groups->length - 1].parent_quote;
  groups->length -= 1;
}

static bool delimiter_close_nearest_group(
  struct DelimiterReader *reader,
  enum DelimiterGroupKind kind,
  uint8_t closing
) {
  size_t depth = reader->groups.length;
  while (
    depth > 0 &&
    reader->groups.data[depth - 1].kind == DELIMITER_GROUP_PARAMETER
  ) {
    depth -= 1;
  }
  if (depth == 0 || reader->groups.data[depth - 1].kind != kind) {
    return false;
  }
  reader->valid = append_byte(&reader->delimiter, closing);
  while (reader->groups.length >= depth) {
    pop_delimiter_group(reader);
  }
  reader->lexer->advance(reader->lexer, false);
  return true;
}

static bool delimiter_open_backquote(struct DelimiterReader *reader) {
  if (!append_byte(&reader->delimiter, '`')) {
    return false;
  }
  reader->lexer->advance(reader->lexer, false);
  if (!push_delimiter_group(
        &reader->groups,
        '`',
        DELIMITER_GROUP_BACKQUOTE,
        reader->quote
      )) {
    return false;
  }
  reader->quote = DELIMITER_UNQUOTED;
  return true;
}

static bool delimiter_parameter_operator(
  struct DelimiterReader *reader,
  int32_t character
) {
  TSLexer *lexer = reader->lexer;
  if (character == '#' || character == '%') {
    if (reader->quote == DELIMITER_PARAMETER_DOUBLE_QUOTED) {
      delimiter_top_group(reader)->keeps_backslash = false;
    }
    reader->quote = DELIMITER_UNQUOTED;
  } else if (character == ':') {
    if (!append_byte(&reader->delimiter, ':') || lexer_at_input_end(lexer)) {
      return false;
    }
    character = lexer->lookahead;
    lexer->advance(lexer, false);
  }
  return append_codepoint(&reader->delimiter, character);
}

/* In the C locale, dash drops one invalid initial byte. After `${#`, it only
   drops an invalid byte immediately followed by `}`. */
static bool delimiter_parameter_prefix(struct DelimiterReader *reader) {
  TSLexer *lexer = reader->lexer;
  struct ByteBuffer *delimiter = &reader->delimiter;
  if (lexer_at_input_end(lexer)) return false;
  int32_t character = lexer->lookahead;
  if (character == '}') return true;
  if (character == '#') {
    if (!advance_source(lexer, delimiter) || lexer_at_input_end(lexer)) {
      return false;
    }
    character = lexer->lookahead;
    if (character == '}') return true;
    lexer->advance(lexer, false);
    if (character <= 0x7f && lexer->lookahead == '}') {
      return !is_parameter_start_character(character) ||
        append_codepoint(delimiter, character);
    }
    return delimiter_parameter_operator(reader, character);
  }
  if (!is_parameter_start_character(character)) {
    uint8_t bytes[4];
    size_t length;
    lexer->advance(lexer, false);
    return encode_utf8_scalar(character, bytes, &length) &&
      append_bytes(delimiter, bytes + 1, length - 1);
  }
  bool name = is_name_start_character(character);
  bool positional = is_decimal_digit(character);
  do {
    if (!advance_source(lexer, delimiter)) return false;
  } while (
    (name && is_name_character(lexer->lookahead)) ||
    (positional && is_decimal_digit(lexer->lookahead))
  );
  if (lexer_at_input_end(lexer)) return false;
  character = lexer->lookahead;
  if (character == '}') return true;
  lexer->advance(lexer, false);
  return delimiter_parameter_operator(reader, character);
}

static bool push_dollar_delimiter_group(struct DelimiterReader *reader) {
  TSLexer *lexer = reader->lexer;
  enum DelimiterQuote parent_quote = reader->quote;
  int32_t opening = lexer->lookahead;
  char closing = opening == '(' ? ')' : '}';
  enum DelimiterGroupKind kind =
    opening == '(' ? DELIMITER_GROUP_COMMAND : DELIMITER_GROUP_PARAMETER;
  if (
    !push_delimiter_group(&reader->groups, closing, kind, parent_quote) ||
    !append_codepoint(&reader->delimiter, opening)
  ) {
    return false;
  }
  lexer->advance(lexer, false);
  reader->quote = opening == '{' &&
      (parent_quote == DELIMITER_DOUBLE_QUOTED ||
        parent_quote == DELIMITER_PARAMETER_DOUBLE_QUOTED ||
        parent_quote == DELIMITER_ARITHMETIC)
    ? DELIMITER_PARAMETER_DOUBLE_QUOTED
    : DELIMITER_UNQUOTED;

  if (opening == '{') return delimiter_parameter_prefix(reader);

  if (opening == '(' && lexer->lookahead == '(') {
    delimiter_top_group(reader)->kind = DELIMITER_GROUP_ARITHMETIC;
    if (!append_byte(&reader->delimiter, '(')) return false;
    lexer->advance(lexer, false);
    reader->quote = DELIMITER_ARITHMETIC;
  }

  return true;
}

static bool delimiter_dollar_tail(struct DelimiterReader *reader) {
  TSLexer *lexer = reader->lexer;
  if (lexer->lookahead == '(' || lexer->lookahead == '{') {
    return push_dollar_delimiter_group(reader);
  }
  return skip_dollar_parameter(lexer, &reader->delimiter);
}

static bool scan_delimiter_double_quoted_character(
  struct DelimiterReader *reader,
  bool *quoted
) {
  TSLexer *lexer = reader->lexer;
  struct ByteBuffer *delimiter = &reader->delimiter;
  *quoted = false;
  int32_t character = lexer->lookahead;
  if (lexer_at_eof(lexer)) {
    return false;
  }

  if (character == '"') {
    if (reader->quote == DELIMITER_PARAMETER_DOUBLE_QUOTED) {
      if (!push_delimiter_group(
            &reader->groups,
            '"',
            DELIMITER_GROUP_DOUBLE_QUOTE,
            reader->quote
          )) {
        return false;
      }
      reader->quote = DELIMITER_DOUBLE_QUOTED;
    } else if (
      reader->groups.length > 0 &&
      delimiter_top_group(reader)->kind == DELIMITER_GROUP_DOUBLE_QUOTE
    ) {
      *quoted = true;
      pop_delimiter_group(reader);
    } else {
      *quoted = true;
      reader->quote = DELIMITER_UNQUOTED;
    }
    lexer->advance(lexer, false);
    return true;
  }

  if (character == '$') {
    lexer->advance(lexer, false);
    return append_byte(delimiter, '$') && delimiter_dollar_tail(reader);
  }

  if (character == '`') {
    return delimiter_open_backquote(reader);
  }

  if (character == '\\') {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '\n') {
      lexer->advance(lexer, false);
      return true;
    }
    *quoted = true;
    if (
      is_one_of(lexer->lookahead, "$`\"\\") ||
      (reader->quote == DELIMITER_PARAMETER_DOUBLE_QUOTED &&
        lexer->lookahead == '}')
    ) {
      if (!append_codepoint(delimiter, lexer->lookahead)) {
        return false;
      }
      lexer->advance(lexer, false);
      return true;
    }
    if (!append_byte(delimiter, '\\')) return false;
    if (reader->quote == DELIMITER_ARITHMETIC && !lexer_at_input_end(lexer)) {
      if (!append_codepoint(delimiter, lexer->lookahead)) return false;
      lexer->advance(lexer, false);
    }
    return true;
  }

  if (!append_codepoint(delimiter, character)) {
    return false;
  }
  lexer->advance(lexer, false);
  return true;
}

static bool delimiter_parameter_quote_step(
  struct DelimiterReader *reader,
  int32_t character
) {
  if (character != '}') {
    return false;
  }
  reader->valid = append_byte(&reader->delimiter, '}');
  pop_delimiter_group(reader);
  reader->lexer->advance(reader->lexer, false);
  return true;
}

static void delimiter_single_quoted_step(struct DelimiterReader *reader) {
  reader->valid = scan_delimiter_single_quoted_segment(
    reader->lexer,
    &reader->delimiter,
    reader->quote == DELIMITER_DOLLAR_SINGLE_QUOTED
  );
  if (reader->valid) {
    reader->quote = DELIMITER_UNQUOTED;
  }
}

static void delimiter_double_quoted_step(struct DelimiterReader *reader) {
  bool quoted;
  reader->valid = scan_delimiter_double_quoted_character(reader, &quoted);
  if (quoted) {
    reader->quoted = true;
  }
}

static void delimiter_dollar(struct DelimiterReader *reader) {
  TSLexer *lexer = reader->lexer;
  reader->has_word_content = true;
  lexer->advance(lexer, false);

  if (lexer->lookahead == '\'') {
    reader->quoted = true;
    reader->quote = DELIMITER_DOLLAR_SINGLE_QUOTED;
    lexer->advance(lexer, false);
    return;
  }

  reader->valid =
    append_byte(&reader->delimiter, '$') && delimiter_dollar_tail(reader);
}

static void delimiter_backslash(struct DelimiterReader *reader) {
  TSLexer *lexer = reader->lexer;
  lexer->advance(lexer, false);
  reader->has_word_content = true;
  if (lexer_at_input_end(lexer)) {
    reader->valid = append_byte(&reader->delimiter, '\\');
    return;
  }
  reader->quoted = true;
  struct DelimiterGroupFrame *group =
    reader->groups.length > 0 ? delimiter_top_group(reader) : NULL;
  bool escape = is_one_of(lexer->lookahead, "\\$`\"") ||
    (group != NULL &&
      group->kind == DELIMITER_GROUP_PARAMETER &&
      lexer->lookahead == '}');
  reader->valid = (group == NULL ||
                    !group->keeps_backslash ||
                    escape ||
                    append_byte(&reader->delimiter, '\\')) &&
    append_codepoint(&reader->delimiter, lexer->lookahead);
  lexer->advance(lexer, false);
}

static void
delimiter_plain_character(struct DelimiterReader *reader, int32_t character) {
  TSLexer *lexer = reader->lexer;
  reader->has_word_content = true;
  reader->valid = append_codepoint(&reader->delimiter, character);
  lexer->advance(lexer, false);
}

static bool
delimiter_structured_start(struct DelimiterReader *reader, int32_t character) {
  TSLexer *lexer = reader->lexer;
  if (
    (character == '\'' || character == '"') &&
    reader->quote != DELIMITER_ARITHMETIC
  ) {
    reader->has_word_content = true;
    if (character == '\'') reader->quoted = true;
    reader->quote =
      character == '\'' ? DELIMITER_SINGLE_QUOTED : DELIMITER_DOUBLE_QUOTED;
    lexer->advance(lexer, false);
    return true;
  }
  if (character == '\\') {
    delimiter_backslash(reader);
    return true;
  }
  if (character == '`') {
    reader->has_word_content = true;
    reader->valid = delimiter_open_backquote(reader);
    return true;
  }
  if (character == '$') {
    delimiter_dollar(reader);
    return true;
  }
  return false;
}

static enum DelimiterStep
delimiter_unquoted_step(struct DelimiterReader *reader, int32_t character) {
  TSLexer *lexer = reader->lexer;
  if (reader->groups.length == 0 && is_token_delimiter(lexer)) {
    return DELIMITER_STEP_END;
  }

  if (
    !reader->has_word_content && reader->groups.length == 0 && character == '#'
  ) {
    reader->valid = false;
    return DELIMITER_STEP_END;
  }

  if (delimiter_structured_start(reader, character)) {
    return DELIMITER_STEP_NEXT;
  }

  if (
    character == ')' &&
    delimiter_close_nearest_group(reader, DELIMITER_GROUP_COMMAND, ')')
  ) {
    return DELIMITER_STEP_NEXT;
  }

  if (
    reader->groups.length > 0 &&
    character == delimiter_top_group(reader)->closing
  ) {
    if (delimiter_top_group(reader)->kind == DELIMITER_GROUP_ARITHMETIC) {
      reader->valid = append_byte(&reader->delimiter, ')');
      lexer->advance(lexer, false);
      if (lexer->lookahead != ')') return DELIMITER_STEP_NEXT;
    }
    reader->valid = append_codepoint(&reader->delimiter, character);
    pop_delimiter_group(reader);
    lexer->advance(lexer, false);
    return DELIMITER_STEP_NEXT;
  }

  if (
    reader->groups.length > 0 &&
    reader->quote == DELIMITER_ARITHMETIC &&
    character == '('
  ) {
    if (!push_delimiter_group(
          &reader->groups,
          ')',
          DELIMITER_GROUP_ARITHMETIC_PAREN,
          reader->quote
        )) {
      reader->valid = false;
      return DELIMITER_STEP_NEXT;
    }
  }

  if (lexer_at_eof(lexer)) {
    return DELIMITER_STEP_END;
  }
  delimiter_plain_character(reader, character);
  return DELIMITER_STEP_NEXT;
}

static enum DelimiterStep delimiter_step(struct DelimiterReader *reader) {
  TSLexer *lexer = reader->lexer;
  int32_t character = lexer->lookahead;
  if (character == SOURCE_BACKQUOTE_BOUNDARY) {
    return DELIMITER_STEP_END;
  }
  if (
    character == '`' &&
    reader->quote != DELIMITER_SINGLE_QUOTED &&
    reader->quote != DELIMITER_DOLLAR_SINGLE_QUOTED &&
    delimiter_close_nearest_group(reader, DELIMITER_GROUP_BACKQUOTE, '`')
  ) {
    return DELIMITER_STEP_NEXT;
  }
  if (
    reader->quote == DELIMITER_PARAMETER_DOUBLE_QUOTED &&
    delimiter_parameter_quote_step(reader, character)
  ) {
    return DELIMITER_STEP_NEXT;
  }
  if (
    reader->quote == DELIMITER_SINGLE_QUOTED ||
    reader->quote == DELIMITER_DOLLAR_SINGLE_QUOTED
  ) {
    delimiter_single_quoted_step(reader);
    return DELIMITER_STEP_NEXT;
  }
  if (
    reader->quote == DELIMITER_DOUBLE_QUOTED ||
    reader->quote == DELIMITER_PARAMETER_DOUBLE_QUOTED ||
    (reader->quote == DELIMITER_ARITHMETIC &&
      (character == '$' || character == '`' || character == '\\'))
  ) {
    delimiter_double_quoted_step(reader);
    return DELIMITER_STEP_NEXT;
  }
  return delimiter_unquoted_step(reader, character);
}

static enum DelimiterReadResult read_here_document_delimiter(
  TSLexer *lexer,
  bool strip_tabs,
  struct HereDocument *document
) {
  struct LogicalLexer *input = lookahead_input(lexer);
  struct DelimiterReader reader = {
    .lexer = lexer,
    .delimiter = {.limit = SCANNER_STATE_CAPACITY},
    .valid = true,
  };
  if (input == NULL) return DELIMITER_READ_RESOURCE_FAILURE;

  while (reader.valid && delimiter_step(&reader) == DELIMITER_STEP_NEXT) {
  }

  bool complete = reader.valid &&
    reader.has_word_content &&
    reader.quote == DELIMITER_UNQUOTED &&
    reader.groups.length == 0;
  ts_free(reader.groups.data);
  if (!complete) {
    ts_free(reader.delimiter.data);
    if (reader.delimiter.failed || reader.groups.failed) {
      return DELIMITER_READ_RESOURCE_FAILURE;
    }
    return lexer_at_eof(lexer) ? DELIMITER_READ_INCOMPLETE
                               : DELIMITER_READ_INVALID;
  }

  *document = (struct HereDocument){
    .delimiter = (uint8_t *)reader.delimiter.data,
    .delimiter_length = reader.delimiter.length,
    .quoted = reader.quoted,
    .strip_tabs = strip_tabs,
  };
  return DELIMITER_READ_WORD;
}

static bool scan_here_document_delimiter(
  struct Scanner *scanner,
  TSLexer *lexer,
  bool strip_tabs
) {
  lexer->mark_end(lexer);
  struct HereDocument document;
  enum DelimiterReadResult result =
    read_here_document_delimiter(lexer, strip_tabs, &document);
  if (result != DELIMITER_READ_WORD) {
    if (result == DELIMITER_READ_RESOURCE_FAILURE) {
      lookahead_fail((struct LookaheadLexer *)lexer);
    }
    return false;
  }
  if (
    lookahead_is_pending(lexer) || !append_pending_document(scanner, document)
  ) {
    clear_document(&document);
    if (!lookahead_is_pending(lexer)) {
      lookahead_fail((struct LookaheadLexer *)lexer);
    }
    return false;
  }
  return emit_symbol(lexer, strip_tabs ? HERE_END_STRIP_BEGIN : HERE_END_BEGIN);
}

static bool
scan_delimited_character_token(TSLexer *lexer, enum TokenType symbol) {
  return accept_character(lexer, symbol) && is_token_delimiter(lexer);
}

static bool read_reserved_word(TSLexer *lexer, char *word) {
  unsigned length = 0;

  while (is_lowercase_letter(lexer->lookahead)) {
    if (length == RESERVED_WORD_CAPACITY - 1) {
      return false;
    }
    word[length] = (char)lexer->lookahead;
    length += 1;
    lexer->advance(lexer, false);
  }

  word[length] = '\0';
  return length > 0 && is_token_delimiter(lexer);
}

static bool word_source_can_continue(const bool *valid_symbols) {
  return valid_symbols[LITERAL_BEGIN] ||
    valid_symbols[ASSIGNMENT_LITERAL_BEGIN] ||
    valid_symbols[PARAMETER_LITERAL_BEGIN] ||
    valid_symbols[WORD_FALLBACK_LITERAL_BEGIN] ||
    valid_symbols[ASSIGNMENT_FALLBACK_LITERAL_BEGIN] ||
    valid_symbols[PARAMETER_FALLBACK_LITERAL_BEGIN];
}

static bool scan_reserved_word(TSLexer *lexer, const bool *valid_symbols) {
  if (word_source_can_continue(valid_symbols)) {
    return false;
  }
  lexer->mark_end(lexer);
  char word[RESERVED_WORD_CAPACITY];
  if (!read_reserved_word(lexer, word)) {
    return false;
  }
  const struct ReservedWord *reserved_word = find_reserved_word(word);
  if (reserved_word == NULL || !valid_symbols[reserved_word->symbol]) {
    return false;
  }
  return emit_symbol(lexer, reserved_word->symbol);
}

static bool scan_horizontal_blanks(TSLexer *lexer);

static void read_name_candidate(TSLexer *lexer, char *word) {
  size_t length = 0;
  bool candidate = true;
  do {
    if (
      candidate &&
      is_lowercase_letter(lexer->lookahead) &&
      length < NAME_CANDIDATE_CAPACITY - 1
    ) {
      word[length] = (char)lexer->lookahead;
      length += 1;
    } else {
      candidate = false;
    }
    lexer->advance(lexer, false);
  } while (is_name_character(lexer->lookahead));

  word[candidate ? length : 0] = '\0';
}

static bool
scan_name_or_reserved_word(TSLexer *lexer, const bool *valid_symbols) {
  char word[NAME_CANDIDATE_CAPACITY];
  lexer->mark_end(lexer);
  read_name_candidate(lexer, word);
  lexer->mark_end(lexer);
  const struct ReservedWord *reserved_word = find_reserved_word(word);

  if (lexer->lookahead == '=') {
    if (!valid_symbols[ASSIGNMENT_NAME_TOKEN]) {
      return false;
    }
    lexer->result_symbol = ASSIGNMENT_NAME_TOKEN;
    return true;
  }

  if (!is_token_delimiter(lexer)) {
    return false;
  }

  if (valid_symbols[FNAME_TOKEN] && reserved_word == NULL) {
    scan_horizontal_blanks(lexer);
    if (lexer->lookahead == '(') {
      if (forbidden_function_name(word)) return false;
      lexer->result_symbol = FNAME_TOKEN;
      return true;
    }
  }

  if (
    reserved_word == NULL ||
    (!valid_symbols[reserved_word->symbol] && !valid_symbols[FNAME_TOKEN])
  ) {
    return false;
  }
  lexer->result_symbol = (TSSymbol)reserved_word->symbol;
  return true;
}

static bool scan_case_item_terminator(TSLexer *lexer) {
  if (lexer->lookahead != ';') {
    return false;
  }
  lexer->advance(lexer, false);

  return lexer->lookahead == ';';
}

static bool
classify_comment_boundary(TSLexer *lexer, const bool *valid_symbols);

static bool classify_scanned_comment_boundary(
  TSLexer *lexer,
  const bool *valid_symbols,
  bool comment_reaches_input_end
);

static bool is_term_closing_keyword(enum TokenType symbol) {
  switch (symbol) {
    case THEN_KEYWORD:
    case ELIF_KEYWORD:
    case ELSE_KEYWORD:
    case FI_KEYWORD:
    case DO_KEYWORD:
    case DONE_KEYWORD:
    case ESAC_KEYWORD:
      return true;
    default:
      return false;
  }
}

static bool
closing_reserved_word_ends_term(const char *word, const bool *valid_symbols) {
  const struct ReservedWord *reserved_word = find_reserved_word(word);
  return reserved_word != NULL &&
    valid_symbols[reserved_word->symbol] &&
    is_term_closing_keyword(reserved_word->symbol);
}

enum TermContinuation {
  TERM_SOURCE_CONTINUES,
  TERM_SOURCE_CLOSES,
  TERM_SOURCE_END,
  TERM_SOURCE_DEFER,
  TERM_SOURCE_FAILURE,
};

static bool is_backquote_terminator_word(const char *word) {
  const struct ReservedWord *reserved_word = find_reserved_word(word);
  return reserved_word != NULL &&
    is_term_closing_keyword(reserved_word->symbol);
}

static enum TermContinuation classify_term_start(
  const struct CommandStart *start,
  const bool *valid_symbols
) {
  int32_t character = start->first_character;
  if (valid_symbols[BACKQUOTE_REMAINDER_TAIL_BEGIN]) return TERM_SOURCE_CLOSES;
  if (
    valid_symbols[BACKQUOTE_REMAINDER_BEGIN] &&
    (character == ')' ||
      (character == ';' && start->second_character == ';') ||
      (start->first_is_delimited &&
        (character == '}' ||
          (start->first_word_is_reserved_candidate &&
            is_backquote_terminator_word(start->first_word)))))
  )
    return TERM_SOURCE_CLOSES;
  if (character == SOURCE_BACKQUOTE_BOUNDARY) {
    return TERM_SOURCE_END;
  }
  switch (character) {
    case ')':
      return valid_symbols[PUNCT_RIGHT_PARENTHESIS] ? TERM_SOURCE_CLOSES
                                                    : TERM_SOURCE_CONTINUES;
    case ';':
      return valid_symbols[CASE_ITEM_END] && start->second_character == ';'
        ? TERM_SOURCE_CLOSES
        : TERM_SOURCE_CONTINUES;
    case '}':
      return valid_symbols[RIGHT_BRACE] && start->first_is_delimited
        ? TERM_SOURCE_CLOSES
        : TERM_SOURCE_CONTINUES;
    default:
      break;
  }
  return start->first_word_is_reserved_candidate &&
      start->first_is_delimited &&
      closing_reserved_word_ends_term(start->first_word, valid_symbols)
    ? TERM_SOURCE_CLOSES
    : TERM_SOURCE_CONTINUES;
}

static enum TermContinuation probe_here_document_continuation(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  const struct HereDocument *document = &scanner->active_frame.documents[0];
  while (true) {
    struct CommandStart start;
    enum HereDocumentLineKind kind =
      read_here_document_line(scanner, lexer, document, &start);
    switch (kind) {
      case HERE_DOCUMENT_LINE_LAYOUT:
        continue;
      case HERE_DOCUMENT_LINE_CONTENT:
        return classify_term_start(&start, valid_symbols);
      case HERE_DOCUMENT_LINE_DELIMITER:
        return TERM_SOURCE_DEFER;
      case HERE_DOCUMENT_LINE_END_OF_INPUT:
        return TERM_SOURCE_END;
      case HERE_DOCUMENT_LINE_INVALID:
        return TERM_SOURCE_FAILURE;
    }
  }
}

static bool scan_horizontal_blanks(TSLexer *lexer) {
  bool found = false;
  while (is_horizontal_blank(lexer->lookahead)) {
    found = true;
    lexer->advance(lexer, false);
  }
  return found;
}

static bool
scan_command_continuation_operator(TSLexer *lexer, const bool *valid_symbols) {
  int32_t character = lexer->lookahead;
  if (character != '|' && character != '&') {
    return false;
  }
  lexer->advance(lexer, false);
  enum TokenType symbol = PIPE_CONTINUATION;
  if (lexer->lookahead == character) {
    lexer->advance(lexer, false);
    symbol = AND_OR_CONTINUATION;
  } else if (character == '&') {
    return false;
  }
  if (!valid_symbols[symbol]) {
    return false;
  }
  lexer->result_symbol = (TSSymbol)symbol;
  return true;
}

static bool scan_pipe_boundary(TSLexer *lexer, const bool *valid_symbols) {
  if (valid_symbols[PATTERN_CONTINUATION]) {
    lexer->result_symbol = PATTERN_CONTINUATION;
    return true;
  }
  return scan_command_continuation_operator(lexer, valid_symbols);
}

static bool is_word_element_start(const TSLexer *lexer) {
  return !lexer_at_input_end(lexer) && !is_one_of(lexer->lookahead, "\n#;&|()");
}

static enum TermContinuation finish_term_continuation(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
);

static bool classify_shell_boundary(
  TSLexer *lexer,
  const bool *valid_symbols,
  bool crossed_layout
);

static bool classify_word_separator(TSLexer *lexer, const bool *valid_symbols);

static bool scan_layout_newline_boundary(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  bool term_choice =
    valid_symbols[TERM_BOUNDARY] && valid_symbols[TERM_CONTINUATION];
  if (!term_choice) {
    lexer->mark_end(lexer);
  }
  enum TermContinuation continuation =
    finish_term_continuation(scanner, lexer, valid_symbols);
  if (
    continuation == TERM_SOURCE_DEFER || continuation == TERM_SOURCE_FAILURE
  ) {
    return false;
  }
  if (
    continuation == TERM_SOURCE_CONTINUES && valid_symbols[TERM_CONTINUATION]
  ) {
    lexer->result_symbol = TERM_CONTINUATION;
    return true;
  }
  if (
    continuation != TERM_SOURCE_CONTINUES &&
    valid_symbols[TERM_BOUNDARY] &&
    (term_choice || valid_symbols[PRE_NEWLINE_BLANK])
  ) {
    lexer->result_symbol = TERM_BOUNDARY;
    return true;
  }
  if (valid_symbols[PRE_NEWLINE_BLANK]) {
    lexer->result_symbol = PRE_NEWLINE_BLANK;
    return true;
  }
  return false;
}

static bool scan_separator_newline_boundary(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (!valid_symbols[SEPARATOR_NEWLINE]) {
    return false;
  }
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);
  int32_t following = lexer->lookahead;
  enum TermContinuation continuation =
    finish_term_continuation(scanner, lexer, valid_symbols);
  if (continuation == TERM_SOURCE_CONTINUES) {
    lexer->result_symbol = SEPARATOR_NEWLINE;
    return true;
  }
  if (
    (continuation == TERM_SOURCE_CLOSES || continuation == TERM_SOURCE_END) &&
    valid_symbols[TERM_BOUNDARY] &&
    (!lexer_at_eof(lexer) ||
      is_horizontal_blank(following) ||
      following == '\n' ||
      following == '#')
  ) {
    lexer->result_symbol = TERM_BOUNDARY;
    return true;
  }
  return false;
}

static bool scan_comment_term_boundary(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (
    !valid_symbols[TERM_CONTINUATION] || has_startable_pending_document(scanner)
  ) {
    return classify_comment_boundary(lexer, valid_symbols);
  }
  advance_to_comment_end(lexer, NULL);
  bool reaches_end = lexer_at_input_end(lexer);
  if (!reaches_end) {
    enum TermContinuation continuation =
      finish_term_continuation(scanner, lexer, valid_symbols);
    if (continuation == TERM_SOURCE_FAILURE) {
      return false;
    }
    if (continuation == TERM_SOURCE_CONTINUES) {
      lexer->result_symbol = TERM_CONTINUATION;
      return true;
    }
    // Preserve the term-ending lookahead for incremental reparsing.
    if (continuation != TERM_SOURCE_DEFER && valid_symbols[TERM_BOUNDARY]) {
      lexer->result_symbol = TERM_BOUNDARY;
      return true;
    }
  }
  return classify_scanned_comment_boundary(lexer, valid_symbols, reaches_end);
}

static bool scan_element_boundary(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  lexer->mark_end(lexer);
  bool crossed_layout = scan_horizontal_blanks(lexer);
  int32_t character = lexer->lookahead;
  if (character == '|') {
    return scan_pipe_boundary(lexer, valid_symbols);
  }
  if (character == '&') {
    return scan_command_continuation_operator(lexer, valid_symbols);
  }
  if (character == ';') {
    if (scan_case_item_terminator(lexer)) {
      lexer->result_symbol = CASE_ITEM_END;
      return valid_symbols[CASE_ITEM_END];
    }
    return false;
  }
  if (character == '\n') {
    if (has_startable_pending_document(scanner)) {
      return false;
    }
    return crossed_layout
      ? scan_layout_newline_boundary(scanner, lexer, valid_symbols)
      : scan_separator_newline_boundary(scanner, lexer, valid_symbols);
  }
  if (character == '#') {
    if (!crossed_layout && word_source_can_continue(valid_symbols)) {
      return false;
    }
    return scan_comment_term_boundary(scanner, lexer, valid_symbols);
  }
  if (
    crossed_layout &&
    (valid_symbols[WORD_SEPARATOR_BEGIN] ||
      valid_symbols[ASSIGNMENT_SEPARATOR_BEGIN] ||
      valid_symbols[REDIRECT_SEPARATOR_BEGIN]) &&
    is_word_element_start(lexer)
  ) {
    return classify_word_separator(lexer, valid_symbols);
  }
  return classify_shell_boundary(lexer, valid_symbols, crossed_layout);
}

static bool advance_file_descriptor(TSLexer *lexer) {
  if (!is_decimal_digit(lexer->lookahead)) {
    return false;
  }
  lexer->advance(lexer, false);
  return lexer->lookahead == '<' || lexer->lookahead == '>';
}

struct SeparatorTarget {
  bool redirect;
  bool assignment;
};

static struct SeparatorTarget scan_separator_target(TSLexer *lexer) {
  struct SeparatorTarget target = {0};
  int32_t character = lexer->lookahead;
  if (character == '<' || character == '>') {
    target.redirect = true;
  } else if (is_decimal_digit(character)) {
    target.redirect = advance_file_descriptor(lexer);
  } else if (is_name_start_character(character)) {
    do {
      lexer->advance(lexer, false);
    } while (is_name_character(lexer->lookahead));
    target.assignment = lexer->lookahead == '=';
  }
  return target;
}

static bool classify_word_separator(TSLexer *lexer, const bool *valid_symbols) {
  lexer->mark_end(lexer);
  struct SeparatorTarget target = scan_separator_target(lexer);

  if (target.assignment && valid_symbols[ASSIGNMENT_SEPARATOR_BEGIN]) {
    lexer->result_symbol = ASSIGNMENT_SEPARATOR_BEGIN;
    return true;
  }
  if (target.redirect && valid_symbols[REDIRECT_SEPARATOR_BEGIN]) {
    lexer->result_symbol = REDIRECT_SEPARATOR_BEGIN;
    return true;
  }
  if (valid_symbols[WORD_SEPARATOR_BEGIN]) {
    lexer->result_symbol = WORD_SEPARATOR_BEGIN;
    return true;
  }
  return false;
}

static enum TermContinuation finish_term_continuation(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  while (true) {
    scan_horizontal_blanks(lexer);
    if (lexer->lookahead == '#') {
      advance_to_comment_end(lexer, NULL);
      continue;
    }
    if (lexer->lookahead == '\n') {
      lexer->advance(lexer, false);
      if (active_document_applies(scanner)) {
        return probe_here_document_continuation(scanner, lexer, valid_symbols);
      }
      continue;
    }

    if (lookahead_has_failed(lexer)) {
      return TERM_SOURCE_FAILURE;
    }
    if (lexer_at_eof(lexer)) {
      return TERM_SOURCE_END;
    }
    struct CommandStart start = {
      .first_character = lexer->lookahead,
    };
    if (is_lowercase_letter(start.first_character)) {
      start.first_word_is_reserved_candidate =
        read_reserved_word(lexer, start.first_word);
      start.first_is_delimited = start.first_word_is_reserved_candidate;
    } else if (start.first_character == '}' || start.first_character == ';') {
      lexer->advance(lexer, false);
      start.second_character = lexer->lookahead;
      start.first_is_delimited = is_token_delimiter(lexer);
    }
    if (lookahead_has_failed(lexer)) {
      return TERM_SOURCE_FAILURE;
    }
    return classify_term_start(&start, valid_symbols);
  }
}

static bool scan_file_descriptor(TSLexer *lexer) {
  if (!advance_file_descriptor(lexer)) {
    return false;
  }
  return emit_symbol(lexer, FILE_DESCRIPTOR);
}

static bool classify_shell_boundary(
  TSLexer *lexer,
  const bool *valid_symbols,
  bool crossed_layout
) {
  int32_t character = lexer->lookahead;
  if (character == '|') {
    return scan_pipe_boundary(lexer, valid_symbols);
  }

  if (character == ')' && valid_symbols[PATTERN_END]) {
    lexer->result_symbol = PATTERN_END;
    return true;
  }

  if (character == ';' && valid_symbols[CASE_ITEM_END]) {
    if (!scan_case_item_terminator(lexer)) {
      return false;
    }
    lexer->result_symbol = CASE_ITEM_END;
    return true;
  }

  if (
    character == '#' &&
    (crossed_layout || !word_source_can_continue(valid_symbols)) &&
    classify_comment_boundary(lexer, valid_symbols)
  ) {
    return true;
  }

  if (
    valid_symbols[REDIRECT_LIST_BEGIN] &&
    (character == '<' || character == '>' || is_decimal_digit(character))
  ) {
    lexer->result_symbol = REDIRECT_LIST_BEGIN;
    return true;
  }

  return false;
}

static bool scan_shell_boundary(TSLexer *lexer, const bool *valid_symbols) {
  lexer->mark_end(lexer);
  bool crossed_layout = scan_horizontal_blanks(lexer);
  return classify_shell_boundary(lexer, valid_symbols, crossed_layout);
}

static bool classify_scanned_comment_boundary(
  TSLexer *lexer,
  const bool *valid_symbols,
  bool comment_reaches_input_end
) {
  if (comment_reaches_input_end && valid_symbols[TRAILING_COMMENT_BOUNDARY]) {
    lexer->result_symbol = TRAILING_COMMENT_BOUNDARY;
    return true;
  }
  if (valid_symbols[COMMENT_BOUNDARY]) {
    lexer->result_symbol = COMMENT_BOUNDARY;
    return true;
  }
  return false;
}

static bool
classify_comment_boundary(TSLexer *lexer, const bool *valid_symbols) {
  if (
    !valid_symbols[COMMENT_BOUNDARY] &&
    !valid_symbols[TRAILING_COMMENT_BOUNDARY]
  ) {
    return false;
  }
  bool comment_reaches_input_end = false;
  if (valid_symbols[TRAILING_COMMENT_BOUNDARY]) {
    advance_to_comment_end(lexer, NULL);
    comment_reaches_input_end = lexer_at_input_end(lexer);
  }
  return classify_scanned_comment_boundary(
    lexer,
    valid_symbols,
    comment_reaches_input_end
  );
}

static bool classify_layout_run(TSLexer *lexer, const bool *valid_symbols) {
  int32_t character = lexer->lookahead;
  if (
    character == '#' &&
    (valid_symbols[COMMENT_BOUNDARY] ||
      valid_symbols[TRAILING_COMMENT_BOUNDARY])
  ) {
    return classify_comment_boundary(lexer, valid_symbols);
  }
  if (character == '\n' && valid_symbols[PRE_NEWLINE_BLANK]) {
    lexer->result_symbol = PRE_NEWLINE_BLANK;
    return true;
  }
  if (
    character == ';' &&
    valid_symbols[CASE_ITEM_END] &&
    scan_case_item_terminator(lexer)
  ) {
    lexer->result_symbol = CASE_ITEM_END;
    return true;
  }
  return false;
}

static enum ArithmeticOperatorCategory classify_arithmetic_operator(
  int32_t first,
  int32_t second,
  int32_t third,
  size_t *length
) {
  bool equals = second == '=';
  bool repeated = second == first;
  *length = 1;
  switch (first) {
    case '=':
      *length = equals ? 2 : 1;
      return equals ? ARITHMETIC_OPERATOR_CATEGORY_EQUALITY
                    : ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT;
    case '!':
      *length = equals ? 2 : 1;
      return equals ? ARITHMETIC_OPERATOR_CATEGORY_EQUALITY
                    : ARITHMETIC_OPERATOR_CATEGORY_COUNT;
    case '|':
    case '&':
      *length = equals || repeated ? 2 : 1;
      if (equals) {
        return ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT;
      }
      if (repeated) {
        return first == '|' ? ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_OR
                            : ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_AND;
      }
      return first == '|' ? ARITHMETIC_OPERATOR_CATEGORY_BITWISE_OR
                          : ARITHMETIC_OPERATOR_CATEGORY_BITWISE_AND;
    case '^':
    case '*':
    case '/':
    case '%':
      *length = equals ? 2 : 1;
      if (equals) {
        return ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT;
      }
      return first == '^' ? ARITHMETIC_OPERATOR_CATEGORY_BITWISE_XOR
                          : ARITHMETIC_OPERATOR_CATEGORY_MULTIPLICATIVE;
    case '<':
    case '>':
      if (repeated) {
        *length = third == '=' ? 3 : 2;
        return third == '=' ? ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT
                            : ARITHMETIC_OPERATOR_CATEGORY_SHIFT;
      }
      *length = equals ? 2 : 1;
      return ARITHMETIC_OPERATOR_CATEGORY_RELATIONAL;
    case '+':
    case '-':
      *length = equals ? 2 : 1;
      return equals ? ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT
                    : ARITHMETIC_OPERATOR_CATEGORY_ADDITIVE;
    case '?':
      return ARITHMETIC_OPERATOR_CATEGORY_QUESTION;
    case ':':
      return ARITHMETIC_OPERATOR_CATEGORY_COLON;
    default:
      return ARITHMETIC_OPERATOR_CATEGORY_COUNT;
  }
}

static bool arithmetic_operator_boundary_is_valid(const bool *valid_symbols) {
  for (
    size_t category = 0; category < ARITHMETIC_OPERATOR_CATEGORY_COUNT;
    category += 1
  ) {
    enum TokenType symbol = ARITHMETIC_OPERATOR_BOUNDARIES[category];
    if (valid_symbols[symbol]) {
      return true;
    }
  }
  return false;
}

static bool is_arithmetic_operator_start(int32_t character) {
  return is_one_of(character, "=!|&^<>+-*/%?:");
}

static bool
scan_arithmetic_boundary(TSLexer *lexer, const bool *valid_symbols) {
  lexer->mark_end(lexer);
  while (arithmetic_whitespace(lexer->lookahead)) {
    lexer->advance(lexer, false);
  }

  if (valid_symbols[ARITHMETIC_CLOSING_BOUNDARY] && lexer->lookahead == ')') {
    lexer->result_symbol = ARITHMETIC_CLOSING_BOUNDARY;
    return true;
  }
  if (
    valid_symbols[ARITHMETIC_EXPANSION_CLOSING_BOUNDARY] &&
    lexer->lookahead == ')'
  ) {
    lexer->advance(lexer, false);
    if (lexer->lookahead != ')') {
      return false;
    }
    lexer->result_symbol = ARITHMETIC_EXPANSION_CLOSING_BOUNDARY;
    return true;
  }

  int32_t first = lexer->lookahead;
  lexer->advance(lexer, false);
  int32_t second = lexer->lookahead;
  lexer->advance(lexer, false);
  size_t length;
  enum ArithmeticOperatorCategory category =
    classify_arithmetic_operator(first, second, lexer->lookahead, &length);
  if (category != ARITHMETIC_OPERATOR_CATEGORY_COUNT) {
    enum TokenType operator_symbol = ARITHMETIC_OPERATOR_BOUNDARIES[category];
    if (valid_symbols[operator_symbol]) {
      lexer->result_symbol = (TSSymbol)operator_symbol;
      return true;
    }
  }

  return false;
}

// Validate first to keep arithmetic subtree reuse deterministic.
enum ValidationTokenKind {
  VALIDATION_TOKEN_NUMBER,
  VALIDATION_TOKEN_VARIABLE,
  VALIDATION_TOKEN_OPERATOR,
  VALIDATION_TOKEN_LEFT_PARENTHESIS,
  VALIDATION_TOKEN_RIGHT_PARENTHESIS,
};

enum {
  VALIDATION_OPERATOR_BANG = ARITHMETIC_OPERATOR_CATEGORY_COUNT,
  VALIDATION_OPERATOR_TILDE,
  VALIDATION_OPERATOR_REPEATED_SIGN,
};

static bool append_validation_token(
  struct ValidationTokenBuffer *tokens,
  uint8_t kind,
  uint8_t category
) {
  struct ValidationToken *data = source_grow(
    tokens->data,
    &tokens->capacity,
    tokens->length + 1,
    sizeof(struct ValidationToken)
  );
  if (data == NULL) {
    return false;
  }
  tokens->data = data;
  tokens->data[tokens->length] =
    (struct ValidationToken){.kind = kind, .category = category};
  tokens->length += 1;
  return true;
}

enum EmbeddedFrameKind {
  EMBEDDED_SOURCE,
  EMBEDDED_COMMAND_SUBSTITUTION,
  EMBEDDED_SUBSHELL,
  EMBEDDED_FUNCTION_HEADER,
};

struct EmbeddedFrame {
  char closer;
  enum EmbeddedFrameKind kind;
  struct CommandTracker command;
  bool double_quoted_parameter;
  enum ParameterQuoteState parameter_quote;
  struct LookaheadContext source_context;
};

struct EmbeddedSkip {
  struct EmbeddedFrame *frames;
  size_t frame_count;
  size_t frame_capacity;
  struct CaseTrackerBuffer cases;
  struct HereDocument *pending;
  size_t pending_count;
  bool started;
};

static void clear_embedded_skip(struct EmbeddedSkip *skip) {
  ts_free(skip->frames);
  ts_free(skip->cases.data);
  clear_document_array(&skip->pending, &skip->pending_count);
  *skip = (struct EmbeddedSkip){0};
}

static void clear_arithmetic_scan(struct ArithmeticScan *scan) {
  ts_free(scan->tokens.data);
  if (scan->embedded != NULL) {
    clear_embedded_skip(scan->embedded);
    ts_free(scan->embedded);
  }
  *scan = (struct ArithmeticScan){0};
}

static size_t embedded_substitution_depth(const struct EmbeddedSkip *skip) {
  size_t depth = skip->frame_count;
  while (
    depth > 0 && skip->frames[depth - 1].kind != EMBEDDED_COMMAND_SUBSTITUTION
  ) {
    depth -= 1;
  }
  return depth;
}

static void embedded_pop_frame(struct EmbeddedSkip *skip) {
  enum EmbeddedFrameKind kind = skip->frames[skip->frame_count - 1].kind;
  skip->frame_count -= 1;
  if (kind == EMBEDDED_COMMAND_SUBSTITUTION) {
    discard_documents_above(
      &skip->pending,
      &skip->pending_count,
      skip->frame_count
    );
  }
}

static bool embedded_push_frame(
  struct EmbeddedSkip *skip,
  char closer,
  enum EmbeddedFrameKind kind,
  bool double_quoted
) {
  struct EmbeddedFrame *frames = source_grow(
    skip->frames,
    &skip->frame_capacity,
    skip->frame_count + 1,
    sizeof(struct EmbeddedFrame)
  );
  if (frames == NULL) {
    return false;
  }
  skip->frames = frames;
  skip->frames[skip->frame_count] = (struct EmbeddedFrame){
    .closer = closer,
    .kind = kind,
    .double_quoted_parameter = closer == '}' && double_quoted,
  };
  reset_command_position(
    &skip->frames[skip->frame_count].command,
    COMMAND_POSITION_START
  );
  skip->frame_count += 1;
  return true;
}

static bool embedded_push_source(
  struct EmbeddedSkip *skip,
  TSLexer *lexer,
  char closer,
  bool quoted
) {
  if (!embedded_push_frame(
        skip,
        closer,
        closer == '`' ? EMBEDDED_COMMAND_SUBSTITUTION : EMBEDDED_SOURCE,
        false
      )) {
    return false;
  }
  return closer != '`' ||
    lookahead_substitution_begin(
      lexer,
      true,
      quoted,
      &skip->frames[skip->frame_count - 1].source_context
    );
}

static void embedded_note_word(struct EmbeddedSkip *skip, bool in_command) {
  if (!in_command) {
    return;
  }
  track_command_word_character(
    &skip->frames[skip->frame_count - 1].command.word,
    '\\'
  );
}

static enum ArithmeticValidation embedded_push_dollar_group(
  const struct Scanner *scanner,
  struct EmbeddedSkip *skip,
  TSLexer *lexer,
  bool double_quoted
) {
  bool command_context = lexer->lookahead == '(';
  lexer->advance(lexer, false);
  if (command_context && lexer->lookahead == '(') {
    return skip_ambiguous_substitution(scanner, lexer);
  }
  bool pushed = embedded_push_frame(
    skip,
    command_context ? ')' : '}',
    command_context ? EMBEDDED_COMMAND_SUBSTITUTION : EMBEDDED_SOURCE,
    double_quoted
  );
  if (pushed && command_context) {
    pushed = lookahead_substitution_begin(
      lexer,
      false,
      false,
      &skip->frames[skip->frame_count - 1].source_context
    );
  }
  return pushed ? ARITHMETIC_VALIDATION_VALID
                : ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
}

static enum ArithmeticValidation read_embedded_here_document_delimiter(
  TSLexer *lexer,
  bool strip_tabs,
  struct HereDocument *document
) {
  while (is_horizontal_blank(lexer->lookahead)) {
    lexer->advance(lexer, false);
  }

  enum DelimiterReadResult result =
    read_here_document_delimiter(lexer, strip_tabs, document);
  if (result == DELIMITER_READ_WORD) {
    return ARITHMETIC_VALIDATION_VALID;
  }
  if (result == DELIMITER_READ_RESOURCE_FAILURE) {
    return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
  }
  return result == DELIMITER_READ_INCOMPLETE ? ARITHMETIC_VALIDATION_INCOMPLETE
                                             : ARITHMETIC_VALIDATION_INVALID;
}

static enum ArithmeticValidation skip_embedded_here_document_bodies(
  const struct Scanner *scanner,
  TSLexer *lexer,
  struct EmbeddedSkip *skip
) {
  const struct Scanner unquoted_scanner = {0};
  if (scanner == NULL) {
    scanner = &unquoted_scanner;
  }
  size_t depth = embedded_substitution_depth(skip);
  size_t retained = 0;
  enum ArithmeticValidation result = ARITHMETIC_VALIDATION_VALID;
  for (size_t index = 0; index < skip->pending_count; index += 1) {
    struct HereDocument *document = &skip->pending[index];
    if (
      result != ARITHMETIC_VALIDATION_VALID ||
      document->declaration_depth < depth
    ) {
      skip->pending[retained] = *document;
      retained += 1;
      continue;
    }
    while (true) {
      enum HereDocumentLineKind kind =
        read_here_document_line(scanner, lexer, document, NULL);
      if (kind == HERE_DOCUMENT_LINE_DELIMITER) {
        break;
      }
      if (kind == HERE_DOCUMENT_LINE_END_OF_INPUT) {
        result = ARITHMETIC_VALIDATION_INCOMPLETE;
        break;
      }
      if (kind == HERE_DOCUMENT_LINE_INVALID) {
        result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
        break;
      }
    }
    clear_document(document);
  }
  skip->pending_count = retained;
  return result;
}

struct EmbeddedWalk {
  const struct Scanner *scanner;
  TSLexer *lexer;
  struct EmbeddedSkip *skip;
  enum ArithmeticValidation result;
};

static void embedded_backquote_boundary(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame
) {
  TSLexer *lexer = walk->lexer;
  if (frame->closer != '`' || !logical_backquote_end(lookahead_input(lexer))) {
    walk->result = ARITHMETIC_VALIDATION_INCOMPLETE;
    return;
  }
  lexer->advance(lexer, false);
  if (!lookahead_context_end(lexer, &frame->source_context)) {
    walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    return;
  }
  embedded_pop_frame(walk->skip);
}

static void embedded_escape(struct EmbeddedWalk *walk, bool in_command) {
  bool continuation;
  if (!skip_escape_run(walk->lexer, &continuation)) {
    walk->result = ARITHMETIC_VALIDATION_INCOMPLETE;
  } else if (!continuation) {
    embedded_note_word(walk->skip, in_command);
  }
}

static void embedded_quoted_step(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame,
  int32_t character
) {
  TSLexer *lexer = walk->lexer;
  if (character == '\\') {
    embedded_escape(walk, false);
    return;
  }
  if (character == frame->closer) {
    lexer->advance(lexer, false);
    embedded_pop_frame(walk->skip);
    return;
  }
  if (character == '`' || character == '"') {
    lexer->advance(lexer, false);
    if (!embedded_push_source(walk->skip, lexer, (char)character, true)) {
      walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    }
    return;
  }
  if (character == '$') {
    lexer->advance(lexer, false);
    if (lexer->lookahead == '(' || lexer->lookahead == '{') {
      walk->result =
        embedded_push_dollar_group(walk->scanner, walk->skip, lexer, true);
    } else {
      skip_dollar_parameter(lexer, NULL);
    }
    return;
  }
  lexer->advance(lexer, false);
}

static void embedded_dollar(struct EmbeddedWalk *walk, bool in_command) {
  TSLexer *lexer = walk->lexer;
  lexer->advance(lexer, false);
  embedded_note_word(walk->skip, in_command);
  if (lexer->lookahead == '(' || lexer->lookahead == '{') {
    walk->result =
      embedded_push_dollar_group(walk->scanner, walk->skip, lexer, false);
    return;
  }
  if (lexer->lookahead == '\'') {
    lexer->advance(lexer, false);
    if (!scan_delimiter_single_quoted_segment(lexer, NULL, true)) {
      walk->result = ARITHMETIC_VALIDATION_INCOMPLETE;
    }
    return;
  }
  skip_dollar_parameter(lexer, NULL);
}

static void embedded_redirect(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame,
  int32_t character
) {
  TSLexer *lexer = walk->lexer;
  struct EmbeddedSkip *skip = walk->skip;
  begin_command_redirect(&frame->command);
  lexer->advance(lexer, false);
  if (character != '<' || lexer->lookahead != '<') {
    if (is_redirect_operator_suffix(character, lexer->lookahead)) {
      lexer->advance(lexer, false);
    }
    return;
  }
  lexer->advance(lexer, false);
  bool strip_tabs = lexer->lookahead == '-';
  if (strip_tabs) {
    lexer->advance(lexer, false);
  }
  struct HereDocument document;
  walk->result =
    read_embedded_here_document_delimiter(lexer, strip_tabs, &document);
  if (walk->result != ARITHMETIC_VALIDATION_VALID) {
    return;
  }
  document.declaration_depth = embedded_substitution_depth(skip);
  if (!append_document(&skip->pending, &skip->pending_count, document)) {
    clear_document(&document);
    walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    return;
  }
  frame->command.redirect_operand = false;
}

static void embedded_open_parenthesis(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame,
  struct CaseTracker *active_case
) {
  walk->lexer->advance(walk->lexer, false);
  if (active_case != NULL && case_tracker_in_pattern(active_case->state)) {
    active_case->state = CASE_TRACKER_IN_PATTERN;
    reset_command_position(&frame->command, COMMAND_POSITION_START);
    return;
  }
  enum EmbeddedFrameKind kind =
    frame->command.function_name ? EMBEDDED_FUNCTION_HEADER : EMBEDDED_SUBSHELL;
  if (!embedded_push_frame(walk->skip, ')', kind, false)) {
    walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
  }
}

static void embedded_close_parenthesis(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame,
  struct CaseTracker *active_case
) {
  struct EmbeddedSkip *skip = walk->skip;
  walk->lexer->advance(walk->lexer, false);
  if (active_case != NULL) {
    if (case_tracker_in_pattern(active_case->state)) {
      active_case->state = CASE_TRACKER_BODY;
      reset_command_position(&frame->command, COMMAND_POSITION_START);
    }
    return;
  }
  enum EmbeddedFrameKind kind = frame->kind;
  struct LookaheadContext source_context = frame->source_context;
  embedded_pop_frame(skip);
  pop_case_trackers_at_depth(&skip->cases, skip->frame_count + 1);
  if (
    kind == EMBEDDED_COMMAND_SUBSTITUTION &&
    !lookahead_context_end(walk->lexer, &source_context)
  ) {
    walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    return;
  }
  if (
    skip->frame_count > 0 &&
    (kind == EMBEDDED_FUNCTION_HEADER || kind == EMBEDDED_SUBSHELL)
  ) {
    reset_command_position(
      &skip->frames[skip->frame_count - 1].command,
      kind == EMBEDDED_FUNCTION_HEADER ? COMMAND_POSITION_START
                                       : COMMAND_POSITION_CLOSED
    );
  }
}

static void embedded_plain_character(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame,
  bool in_command,
  int32_t character
) {
  walk->lexer->advance(walk->lexer, false);
  if (!in_command || is_horizontal_blank(character)) {
    return;
  }
  if (
    character == '\n' ||
    character == ';' ||
    character == '&' ||
    character == '|'
  ) {
    enum CommandPosition position =
      character == '\n' && frame->command.position == COMMAND_POSITION_FOR_IN
      ? COMMAND_POSITION_FOR_IN
      : COMMAND_POSITION_START;
    reset_command_position(&frame->command, position);
  } else {
    track_command_word_character(&frame->command.word, character);
  }
}

static void embedded_step(
  struct EmbeddedWalk *walk,
  struct EmbeddedFrame *frame,
  bool in_command,
  struct CaseTracker *active_case,
  int32_t character
) {
  TSLexer *lexer = walk->lexer;
  struct EmbeddedSkip *skip = walk->skip;
  if (frame->closer == '}') {
    frame->parameter_quote =
      parameter_quote_state_after(frame->parameter_quote, character);
  }
  bool quoted_parameter = frame->double_quoted_parameter &&
    frame->parameter_quote != PARAMETER_QUOTE_PATTERN;

  if (character == SOURCE_BACKQUOTE_BOUNDARY) {
    embedded_backquote_boundary(walk, frame);
    return;
  }
  if (frame->closer == '"' || quoted_parameter) {
    embedded_quoted_step(walk, frame, character);
    return;
  }
  if (character == '\\') {
    embedded_escape(walk, in_command);
    return;
  }
  if (character == '\'') {
    lexer->advance(lexer, false);
    if (!scan_delimiter_single_quoted_segment(lexer, NULL, false)) {
      walk->result = ARITHMETIC_VALIDATION_INCOMPLETE;
      return;
    }
    embedded_note_word(skip, in_command);
    return;
  }
  if (character == '"' || character == '`') {
    lexer->advance(lexer, false);
    embedded_note_word(skip, in_command);
    if (!embedded_push_source(skip, lexer, (char)character, false)) {
      walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    }
    return;
  }
  if (character == '$') {
    embedded_dollar(walk, in_command);
    return;
  }
  if (character == '#' && in_command && !frame->command.word.active) {
    if (!advance_to_comment_end(lexer, NULL)) {
      walk->result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    }
    return;
  }
  if (
    in_command &&
    (character == '<' || character == '>') &&
    (active_case == NULL || active_case->state == CASE_TRACKER_BODY)
  ) {
    embedded_redirect(walk, frame, character);
    return;
  }
  if (character == '\n' && in_command && skip->pending_count > 0) {
    lexer->advance(lexer, false);
    walk->result =
      skip_embedded_here_document_bodies(walk->scanner, lexer, skip);
    reset_command_position(&frame->command, COMMAND_POSITION_START);
    return;
  }
  if (character == '(' && in_command) {
    embedded_open_parenthesis(walk, frame, active_case);
    return;
  }
  if (character == ')' && in_command) {
    embedded_close_parenthesis(walk, frame, active_case);
    return;
  }
  if (character == '}' && frame->closer == '}') {
    lexer->advance(lexer, false);
    embedded_pop_frame(skip);
    return;
  }
  if (
    in_command &&
    active_case != NULL &&
    active_case->state == CASE_TRACKER_BODY &&
    character == ';'
  ) {
    lexer->advance(lexer, false);
    if (lexer->lookahead == ';') {
      lexer->advance(lexer, false);
      active_case->state = CASE_TRACKER_EXPECT_PATTERN;
    }
    reset_command_position(&frame->command, COMMAND_POSITION_START);
    return;
  }
  embedded_plain_character(walk, frame, in_command, character);
}

static enum ArithmeticValidation resume_embedded_construct(
  const struct Scanner *scanner,
  TSLexer *lexer,
  char initial_closer,
  bool double_quoted,
  struct EmbeddedSkip *skip
) {
  struct EmbeddedWalk walk = {
    .scanner = scanner,
    .lexer = lexer,
    .skip = skip,
    .result = ARITHMETIC_VALIDATION_VALID,
  };
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  size_t resume = lookahead->position;
  size_t previous_depth = 0;
  struct EmbeddedFrame previous_frame = {0};
  size_t previous_case_count = 0;
  uint8_t previous_case_state = 0;

  if (!skip->started) {
    if (!embedded_push_frame(
          skip,
          initial_closer,
          initial_closer == ')' ? EMBEDDED_COMMAND_SUBSTITUTION
                                : EMBEDDED_SOURCE,
          double_quoted
        )) {
      lookahead_fail(lookahead);
      return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    }
    skip->started = true;
    if (initial_closer == '`') {
      skip->frames[0].kind = EMBEDDED_COMMAND_SUBSTITUTION;
    }
    if (
      initial_closer != '}' &&
      !lookahead_substitution_begin(
        lexer,
        initial_closer == '`',
        double_quoted,
        &skip->frames[0].source_context
      )
    ) {
      lookahead_fail(lookahead);
      return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
    }
  }

  while (walk.result == ARITHMETIC_VALIDATION_VALID && skip->frame_count > 0) {
    if (lexer_at_eof(lexer)) {
      walk.result = ARITHMETIC_VALIDATION_INCOMPLETE;
      break;
    }

    struct EmbeddedFrame *frame = &skip->frames[skip->frame_count - 1];
    bool in_command = frame->kind != EMBEDDED_SOURCE;
    int32_t character = lexer->lookahead;
    resume = lookahead->position;
    previous_depth = skip->frame_count;
    previous_frame = *frame;
    previous_case_count = skip->cases.length;
    if (previous_case_count > 0) {
      previous_case_state = skip->cases.data[previous_case_count - 1].state;
    }

    if (
      in_command &&
      is_token_delimiter(lexer) &&
      !finish_command_word(
        &skip->cases,
        skip->frame_count,
        &frame->command,
        character == '<' || character == '>'
      )
    ) {
      walk.result = ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      break;
    }
    struct CaseTracker *active_case =
      in_command ? active_case_tracker(&skip->cases, skip->frame_count) : NULL;
    embedded_step(&walk, frame, in_command, active_case, character);
  }

  if (!lookahead->failed && lookahead->requested != SIZE_MAX) {
    lookahead_seek(lookahead, resume);
    if (previous_depth > 0) {
      skip->frames[previous_depth - 1] = previous_frame;
    }
    skip->cases.length = previous_case_count;
    if (previous_case_count > 0) {
      skip->cases.data[previous_case_count - 1].state = previous_case_state;
    }
  }
  if (walk.result == ARITHMETIC_VALIDATION_RESOURCE_FAILURE) {
    lookahead_fail(lookahead);
  }
  return walk.result;
}

static enum ArithmeticValidation skip_embedded_construct(
  const struct Scanner *scanner,
  TSLexer *lexer,
  char initial_closer
) {
  if (initial_closer == ')' && lexer->lookahead == '(') {
    return skip_ambiguous_substitution(scanner, lexer);
  }
  struct EmbeddedSkip skip = {0};
  enum ArithmeticValidation result =
    resume_embedded_construct(scanner, lexer, initial_closer, false, &skip);
  clear_embedded_skip(&skip);
  return result;
}

static enum TokenType scan_arithmetic_word(TSLexer *lexer) {
  bool name = is_name_start_character(lexer->lookahead);
  bool number = !name;
  unsigned base = lexer->lookahead == '0' ? 8 : 10;
  lexer->advance(lexer, false);
  if (
    !name && base == 8 && (lexer->lookahead == 'x' || lexer->lookahead == 'X')
  ) {
    base = 16;
    lexer->advance(lexer, false);
    number = is_hexadecimal_digit(lexer->lookahead);
  }
  while (is_name_character(lexer->lookahead)) {
    int32_t character = lexer->lookahead;
    number = number &&
      (base == 16
          ? is_hexadecimal_digit(character)
          : is_decimal_digit(character) && (base != 8 || character <= '7'));
    lexer->advance(lexer, false);
  }
  return name ? ARITHMETIC_VARIABLE_BEGIN
    : number  ? ARITHMETIC_NUMBER_BEGIN
              : ARITHMETIC_SOURCE_FRAGMENT_BEGIN;
}

static bool validate_operator_source(
  TSLexer *lexer,
  struct ArithmeticScan *scan,
  int32_t first
) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  int32_t second = lexer->lookahead;
  size_t position = lookahead->position;
  lexer->advance(lexer, false);
  int32_t third = lexer->lookahead;
  lookahead_seek(lookahead, position);
  size_t length;
  enum ArithmeticOperatorCategory classified =
    classify_arithmetic_operator(first, second, third, &length);
  uint8_t category = classified != ARITHMETIC_OPERATOR_CATEGORY_COUNT
    ? (uint8_t)classified
    : first == '~' ? VALIDATION_OPERATOR_TILDE
    : first == '!' ? VALIDATION_OPERATOR_BANG
                   : VALIDATION_OPERATOR_REPEATED_SIGN;
  for (; length > 1; length -= 1) {
    lexer->advance(lexer, false);
  }
  return append_validation_token(
    &scan->tokens,
    VALIDATION_TOKEN_OPERATOR,
    category
  );
}

static bool begin_embedded_skip(struct ArithmeticScan *scan, char closer) {
  scan->embedded_closer = closer;
  scan->embedded = ts_calloc(1, sizeof(struct EmbeddedSkip));
  return scan->embedded != NULL;
}

static enum ArithmeticValidation validate_embedded_source(
  const struct Scanner *scanner,
  TSLexer *lexer,
  struct ArithmeticScan *scan
) {
  enum ArithmeticValidation nested = !scan->embedded->started &&
      scan->embedded_closer == ')' &&
      lexer->lookahead == '('
    ? skip_ambiguous_substitution(scanner, lexer)
    : resume_embedded_construct(
        scanner,
        lexer,
        scan->embedded_closer,
        true,
        scan->embedded
      );
  if (lookahead_is_pending(lexer)) {
    return nested;
  }
  clear_embedded_skip(scan->embedded);
  ts_free(scan->embedded);
  scan->embedded = NULL;
  if (nested != ARITHMETIC_VALIDATION_VALID) {
    return nested;
  }
  scan->unstructured = true;
  return ARITHMETIC_VALIDATION_VALID;
}

static enum ArithmeticValidation
validate_dollar_source(TSLexer *lexer, struct ArithmeticScan *scan) {
  lexer->advance(lexer, false);
  int32_t introducer = lexer->lookahead;
  if (introducer == '(' || introducer == '{') {
    lexer->advance(lexer, false);
    return begin_embedded_skip(scan, introducer == '(' ? ')' : '}')
      ? ARITHMETIC_VALIDATION_VALID
      : ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
  }
  if (!skip_parameter_name(lexer, introducer) && lexer_at_eof(lexer)) {
    return ARITHMETIC_VALIDATION_INCOMPLETE;
  }
  scan->unstructured = true;
  return ARITHMETIC_VALIDATION_VALID;
}

static enum ArithmeticValidation validate_arithmetic_content(
  const struct Scanner *scanner,
  TSLexer *lexer,
  struct ArithmeticScan *scan
) {
  struct ValidationTokenBuffer *tokens = &scan->tokens;

  while (true) {
    if (scan->embedded != NULL) {
      enum ArithmeticValidation nested =
        validate_embedded_source(scanner, lexer, scan);
      if (
        lookahead_is_pending(lexer) || nested != ARITHMETIC_VALIDATION_VALID
      ) {
        return nested;
      }
      continue;
    }
    int32_t character = lexer->lookahead;

    if (character == '\\') {
      scan->unstructured = true;
      lexer->advance(lexer, false);
      if (lexer_at_eof(lexer)) return ARITHMETIC_VALIDATION_INCOMPLETE;
      lexer->advance(lexer, false);
      continue;
    }

    if (lexer_at_eof(lexer)) {
      return ARITHMETIC_VALIDATION_INCOMPLETE;
    }

    if (arithmetic_whitespace(character)) {
      lexer->advance(lexer, false);
      continue;
    }

    if (character == '(') {
      if (scan->group_depth == SIZE_MAX) {
        return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      }
      scan->group_depth += 1;
      lexer->advance(lexer, false);
      if (!append_validation_token(
            tokens,
            VALIDATION_TOKEN_LEFT_PARENTHESIS,
            0
          )) {
        return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      }
      continue;
    }

    if (character == ')') {
      lexer->advance(lexer, false);
      if (scan->group_depth > 0) {
        scan->group_depth -= 1;
        if (!append_validation_token(
              tokens,
              VALIDATION_TOKEN_RIGHT_PARENTHESIS,
              0
            )) {
          return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
        }
        continue;
      }

      if (lexer_at_eof(lexer)) {
        return ARITHMETIC_VALIDATION_INCOMPLETE;
      }
      if (lexer->lookahead == ')') {
        return ARITHMETIC_VALIDATION_VALID;
      }
      scan->unstructured = true;
      continue;
    }

    if (is_name_character(character)) {
      enum TokenType word = scan_arithmetic_word(lexer);
      if (word == ARITHMETIC_SOURCE_FRAGMENT_BEGIN) {
        scan->unstructured = true;
      } else if (!append_validation_token(
                   tokens,
                   word == ARITHMETIC_NUMBER_BEGIN ? VALIDATION_TOKEN_NUMBER
                                                   : VALIDATION_TOKEN_VARIABLE,
                   0
                 )) {
        return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      }
      continue;
    }

    if (character == '$') {
      enum ArithmeticValidation dollar = validate_dollar_source(lexer, scan);
      if (dollar != ARITHMETIC_VALIDATION_VALID) {
        return dollar;
      }
      continue;
    }

    if (character == '`') {
      lexer->advance(lexer, false);
      if (!begin_embedded_skip(scan, '`')) {
        return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      }
      continue;
    }

    if (is_arithmetic_operator_start(character) || character == '~') {
      lexer->advance(lexer, false);
      if (!validate_operator_source(lexer, scan, character)) {
        return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      }
      continue;
    }

    scan->unstructured = true;
    lexer->advance(lexer, false);
  }
}

struct StructuredValidation {
  const struct ValidationToken *tokens;
  size_t count;
};

static bool validation_token_is_operator(
  const struct StructuredValidation *validation,
  size_t index,
  uint8_t category
) {
  return (
    index < validation->count &&
    validation->tokens[index].kind == VALIDATION_TOKEN_OPERATOR &&
    validation->tokens[index].category == category
  );
}

enum StructuredContext {
  STRUCTURED_CONTEXT_GROUP,
  STRUCTURED_CONTEXT_TERNARY,
};

static size_t structured_assignment_head_end(
  const struct StructuredValidation *validation,
  size_t index
) {
  size_t opens = 0;
  while (
    index < validation->count &&
    validation->tokens[index].kind == VALIDATION_TOKEN_LEFT_PARENTHESIS
  ) {
    index += 1;
    opens += 1;
  }
  if (
    index >= validation->count ||
    validation->tokens[index].kind != VALIDATION_TOKEN_VARIABLE
  ) {
    return SIZE_MAX;
  }
  index += 1;
  while (
    opens > 0 &&
    index < validation->count &&
    validation->tokens[index].kind == VALIDATION_TOKEN_RIGHT_PARENTHESIS
  ) {
    index += 1;
    opens -= 1;
  }
  if (
    opens > 0 ||
    !validation_token_is_operator(
      validation,
      index,
      ARITHMETIC_OPERATOR_CATEGORY_ASSIGNMENT
    )
  ) {
    return SIZE_MAX;
  }
  return index + 1;
}

static bool push_structured_context(
  uint8_t **contexts,
  size_t *count,
  size_t *capacity,
  enum StructuredContext context
) {
  uint8_t *grown =
    source_grow(*contexts, capacity, *count + 1, sizeof(**contexts));
  if (grown == NULL) {
    return false;
  }
  *contexts = grown;
  (*contexts)[*count] = (uint8_t)context;
  *count += 1;
  return true;
}

static enum ArithmeticValidation
validate_structured_expression(const struct StructuredValidation *validation) {
  uint8_t *contexts = NULL;
  size_t context_count = 0;
  size_t context_capacity = 0;
  size_t index = 0;
  bool expecting_operand = true;
  bool at_expression_start = true;
  bool valid = true;

  while (valid && index < validation->count) {
    const struct ValidationToken *token = &validation->tokens[index];

    if (expecting_operand) {
      if (at_expression_start) {
        size_t after_head = structured_assignment_head_end(validation, index);
        if (after_head != SIZE_MAX) {
          index = after_head;
          continue;
        }
      }
      if (
        token->kind == VALIDATION_TOKEN_NUMBER ||
        token->kind == VALIDATION_TOKEN_VARIABLE
      ) {
        expecting_operand = false;
        index += 1;
        continue;
      }
      if (token->kind == VALIDATION_TOKEN_LEFT_PARENTHESIS) {
        if (!push_structured_context(
              &contexts,
              &context_count,
              &context_capacity,
              STRUCTURED_CONTEXT_GROUP
            )) {
          ts_free(contexts);
          return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
        }
        at_expression_start = true;
        index += 1;
        continue;
      }
      if (
        token->kind == VALIDATION_TOKEN_OPERATOR &&
        (token->category == ARITHMETIC_OPERATOR_CATEGORY_ADDITIVE ||
          token->category == VALIDATION_OPERATOR_BANG ||
          token->category == VALIDATION_OPERATOR_TILDE)
      ) {
        at_expression_start = false;
        index += 1;
        continue;
      }
      valid = false;
      break;
    }

    if (token->kind == VALIDATION_TOKEN_RIGHT_PARENTHESIS) {
      if (
        context_count == 0 ||
        contexts[context_count - 1] != STRUCTURED_CONTEXT_GROUP
      ) {
        valid = false;
        break;
      }
      context_count -= 1;
      index += 1;
      continue;
    }
    if (token->kind != VALIDATION_TOKEN_OPERATOR) {
      valid = false;
      break;
    }
    if (
      token->category >= ARITHMETIC_OPERATOR_CATEGORY_LOGICAL_OR &&
      token->category <= ARITHMETIC_OPERATOR_CATEGORY_MULTIPLICATIVE
    ) {
      expecting_operand = true;
      at_expression_start = false;
      index += 1;
      continue;
    }
    if (token->category == ARITHMETIC_OPERATOR_CATEGORY_QUESTION) {
      if (!push_structured_context(
            &contexts,
            &context_count,
            &context_capacity,
            STRUCTURED_CONTEXT_TERNARY
          )) {
        ts_free(contexts);
        return ARITHMETIC_VALIDATION_RESOURCE_FAILURE;
      }
      expecting_operand = true;
      at_expression_start = true;
      index += 1;
      continue;
    }
    if (token->category == ARITHMETIC_OPERATOR_CATEGORY_COLON) {
      if (
        context_count == 0 ||
        contexts[context_count - 1] != STRUCTURED_CONTEXT_TERNARY
      ) {
        valid = false;
        break;
      }
      context_count -= 1;
      expecting_operand = true;
      at_expression_start = false;
      index += 1;
      continue;
    }
    valid = false;
    break;
  }

  valid = valid && !expecting_operand && context_count == 0;
  ts_free(contexts);
  return valid ? ARITHMETIC_VALIDATION_VALID : ARITHMETIC_VALIDATION_INVALID;
}

static bool increase_substitution_depth(struct Scanner *scanner) {
  if (scanner->substitution_depth == SIZE_MAX) {
    return false;
  }

  scanner->substitution_depth += 1;
  if (!scanner_state_fits(scanner)) {
    scanner->substitution_depth -= 1;
    return false;
  }
  return true;
}

static bool finish_command_substitution_body_begin(
  struct Scanner *scanner,
  TSLexer *lexer
) {
  if (!increase_substitution_depth(scanner)) {
    return false;
  }

  lexer->result_symbol = COMMAND_SUBSTITUTION_BODY_BEGIN;
  return true;
}

static enum TokenType classify_arithmetic_source(
  const struct Scanner *scanner,
  TSLexer *lexer,
  struct ArithmeticScan *scan,
  enum ArithmeticValidation *result
) {
  *result = validate_arithmetic_content(scanner, lexer, scan);

  enum TokenType symbol = ARITHMETIC_SOURCE_LEFT_PARENTHESIS;
  if (*result == ARITHMETIC_VALIDATION_VALID && !scan->unstructured) {
    struct StructuredValidation validation = {
      .tokens = scan->tokens.data,
      .count = scan->tokens.length,
    };
    enum ArithmeticValidation structured =
      validate_structured_expression(&validation);
    if (structured == ARITHMETIC_VALIDATION_RESOURCE_FAILURE) {
      *result = structured;
    } else if (structured == ARITHMETIC_VALIDATION_VALID) {
      symbol = ARITHMETIC_LEFT_PARENTHESIS;
    }
  }

  if (*result == ARITHMETIC_VALIDATION_RESOURCE_FAILURE) {
    lookahead_fail((struct LookaheadLexer *)lexer);
  }
  return symbol;
}

static bool scan_arithmetic_left_parenthesis(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  lexer->mark_end(lexer);
  lexer->advance(lexer, false);
  struct ArithmeticScan scan = {0};
  enum ArithmeticValidation result;
  enum TokenType symbol =
    classify_arithmetic_source(scanner, lexer, &scan, &result);
  clear_arithmetic_scan(&scan);
  if (lookahead_is_pending(lexer)) {
    return false;
  }
  if (!valid_symbols[symbol]) {
    return false;
  }

  lexer->result_symbol = (TSSymbol)symbol;
  return true;
}

static bool scan_command_substitution_body_begin(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  lexer->mark_end(lexer);

  if (
    lexer->lookahead == '(' &&
    (valid_symbols[ARITHMETIC_LEFT_PARENTHESIS] ||
      valid_symbols[ARITHMETIC_SOURCE_LEFT_PARENTHESIS])
  ) {
    return scan_arithmetic_left_parenthesis(scanner, lexer, valid_symbols);
  }

  size_t pending = pending_backquote_context(scanner);
  if (pending != SIZE_MAX) {
    enum BackquoteToken token = scanner->contexts[pending].backquote_token;
    if (token == BACKQUOTE_TOKEN_STOP || token == BACKQUOTE_TOKEN_INVALID) {
      return false;
    }
    if (token == BACKQUOTE_TOKEN_RIGHT_PARENTHESIS) {
      if (!valid_symbols[COMMAND_SUBSTITUTION_REMAINDER_END]) return false;
      scanner->contexts[pending].backquote_token = BACKQUOTE_TOKEN_NONE;
      lexer->result_symbol = COMMAND_SUBSTITUTION_REMAINDER_END;
      return source_context_pop(scanner);
    }
  }

  return finish_command_substitution_body_begin(scanner, lexer);
}

static bool
scan_pattern_special_left_bracket(TSLexer *lexer, const bool *valid) {
  if (
    lexer->lookahead != '[' ||
    !class_terminator_follows(lexer, valid[PARAMETER_PATTERN_BRACKET_CHARACTER])
  ) {
    return false;
  }

  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  if (lexer->lookahead != ':') {
    return false;
  }

  lexer->result_symbol = PATTERN_SPECIAL_LEFT_BRACKET;
  return true;
}

static bool scan_dollar_expansion_start(TSLexer *lexer) {
  if (lexer->lookahead != '$') {
    return false;
  }

  lexer->advance(lexer, false);
  if (
    is_parameter_start_character(lexer->lookahead) ||
    lexer->lookahead == '{' ||
    lexer->lookahead == '('
  ) {
    return emit_symbol(lexer, DOLLAR_EXPANSION_START);
  }

  return false;
}

static bool
scan_braced_numeric_parameter_start(TSLexer *lexer, const bool *valid_symbols) {
  if (!is_decimal_digit(lexer->lookahead)) {
    return false;
  }

  lexer->mark_end(lexer);
  bool has_digit = false;
  bool has_multiple_digits = false;
  bool has_nonzero_digit = false;

  while (is_decimal_digit(lexer->lookahead)) {
    if (has_digit) {
      has_multiple_digits = true;
    }
    has_digit = true;
    if (lexer->lookahead != '0') {
      has_nonzero_digit = true;
    }
    lexer->advance(lexer, false);
  }

  if (
    (has_nonzero_digit || has_multiple_digits) &&
    valid_symbols[BRACED_POSITIONAL_PARAMETER_START]
  ) {
    return emit_symbol(lexer, BRACED_POSITIONAL_PARAMETER_START);
  }

  return false;
}

static bool
command_element_boundary_symbols_are_valid(const bool *valid_symbols) {
  return (
    valid_symbols[ASSIGNMENT_SEPARATOR_BEGIN] ||
    valid_symbols[REDIRECT_SEPARATOR_BEGIN] ||
    valid_symbols[PIPE_CONTINUATION] ||
    valid_symbols[AND_OR_CONTINUATION] ||
    valid_symbols[TERM_CONTINUATION]
  );
}

static bool element_boundary_symbols_are_valid(const bool *valid_symbols) {
  return valid_symbols[WORD_SEPARATOR_BEGIN] ||
    command_element_boundary_symbols_are_valid(valid_symbols);
}

static bool here_document_end_is_ready(
  const struct Scanner *scanner,
  const TSLexer *lexer
) {
  return scanner->active_frame.count > 0 &&
    !scanner->active_frame.at_line_start &&
    (scanner->here_document_end_line_consumed || lexer_at_input_end(lexer));
}

static bool
scan_here_document_end_commit(struct Scanner *scanner, TSLexer *lexer) {
  if (!here_document_end_is_ready(scanner, lexer)) {
    return false;
  }
  scanner->here_document_end_line_consumed = false;
  lexer->mark_end(lexer);
  lexer->result_symbol = HERE_DOCUMENT_END_COMMIT;
  finish_active_document(scanner);
  return true;
}

static bool scan_here_document_content_line_start(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (
    !scanner->active_frame.at_line_start ||
    !valid_symbols[HERE_DOCUMENT_CONTENT_LINE_START]
  ) {
    return false;
  }
  lexer->mark_end(lexer);
  scanner->active_frame.at_line_start = false;
  lexer->result_symbol = HERE_DOCUMENT_CONTENT_LINE_START;
  return true;
}

static bool scan_here_document_body_newline(
  const struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  lexer->mark_end(lexer);
  if (
    finish_term_continuation(scanner, lexer, valid_symbols) !=
    TERM_SOURCE_CONTINUES
  ) {
    return false;
  }
  lexer->result_symbol = SEPARATOR_NEWLINE;
  return true;
}

static bool scan_here_document_sequence_end(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (!scanner->sequence_end_pending) {
    return false;
  }

  scanner->sequence_end_pending = false;
  restore_suspended_documents(scanner);
  lexer->mark_end(lexer);
  finish_term_continuation(scanner, lexer, valid_symbols);
  lexer->result_symbol = HERE_DOCUMENT_SEQUENCE_END;
  return true;
}

void *tree_sitter_dash_external_scanner_create(void) {
  return ts_calloc(1, sizeof(struct Scanner));
}

void tree_sitter_dash_external_scanner_destroy(void *payload) {
  struct Scanner *scanner = payload;
  if (scanner == NULL) {
    return;
  }

  clear_scanner(scanner);
  ts_free(scanner);
}

enum ScannerFlag {
  SCANNER_FLAG_SEQUENCE_END_PENDING = 1,
  SCANNER_FLAG_AT_LINE_START = 2,
  SCANNER_FLAG_ASSIGNMENT_TILDE = 4,
  SCANNER_FLAG_MASK = 7,
};

enum DocumentFlag {
  DOCUMENT_FLAG_QUOTED = 1,
  DOCUMENT_FLAG_STRIP_TABS = 2,
  DOCUMENT_FLAG_ENDING = 4,
  DOCUMENT_FLAG_END_TEXT_CONSUMED = 8,
  DOCUMENT_FLAG_MASK = 15,
};

enum ContextFlag {
  CONTEXT_FLAG_LOCAL_DISABLED = 1,
  CONTEXT_FLAG_LONE_WORD = 2,
  CONTEXT_FLAG_SUSPENDS_BODY = 4,
  CONTEXT_FLAG_MASK = 7,
};

enum EmissionFlag {
  EMISSION_FLAG_ACTIVE = 1,
  EMISSION_FLAG_REMOVED_NEWLINE = 2,
  EMISSION_FLAG_ASSIGNMENT_COLON = 4,
  EMISSION_FLAG_END_LINE_CONSUMED = 8,
  EMISSION_FLAG_TRACKS_BRACKETS = 16,
  EMISSION_FLAG_MASK = 31,
};

struct StateWriter {
  char *data;
  size_t length;
  size_t capacity;
};

static bool write_state_bytes(
  struct StateWriter *writer,
  const char *bytes,
  size_t length
) {
  if (length > writer->capacity - writer->length) {
    return false;
  }

  if (length > 0) {
    memcpy(writer->data + writer->length, bytes, length);
  }
  writer->length += length;
  return true;
}

static bool write_state_byte(struct StateWriter *writer, uint8_t byte) {
  char data = (char)byte;
  return write_state_bytes(writer, &data, 1);
}

static bool write_state_size(struct StateWriter *writer, size_t value) {
  return source_write_unsigned(
    (uint8_t *)writer->data,
    writer->capacity,
    &writer->length,
    value
  );
}

static uint8_t document_flags(const struct HereDocument *document) {
  return (uint8_t)((document->quoted ? DOCUMENT_FLAG_QUOTED : 0) |
    (document->strip_tabs ? DOCUMENT_FLAG_STRIP_TABS : 0) |
    (document->ending ? DOCUMENT_FLAG_ENDING : 0) |
    (document->end_text_consumed ? DOCUMENT_FLAG_END_TEXT_CONSUMED : 0));
}

static bool serialize_document(
  struct StateWriter *writer,
  const struct HereDocument *document,
  bool with_declaration_depth
) {
  return write_state_byte(writer, document_flags(document)) &&
    (!with_declaration_depth ||
      write_state_size(writer, document->declaration_depth)) &&
    write_state_size(writer, document->delimiter_length) &&
    write_state_bytes(
      writer,
      (const char *)document->delimiter,
      document->delimiter_length
    );
}

static bool serialize_document_run(
  struct StateWriter *writer,
  const struct HereDocument *documents,
  size_t count,
  bool with_declaration_depth
) {
  for (size_t index = 0; index < count; index += 1) {
    if (!serialize_document(
          writer,
          &documents[index],
          with_declaration_depth
        )) {
      return false;
    }
  }
  return true;
}

static bool serialize_document_array(
  struct StateWriter *writer,
  const struct HereDocument *documents,
  size_t count,
  bool with_declaration_depth
) {
  return (
    write_state_size(writer, count) &&
    serialize_document_run(writer, documents, count, with_declaration_depth)
  );
}

static bool serialize_suspended_frames(
  const struct Scanner *scanner,
  struct StateWriter *writer
) {
  if (!write_state_size(writer, scanner->suspended_frame_count)) {
    return false;
  }
  for (
    size_t frame_index = 0; frame_index < scanner->suspended_frame_count;
    frame_index += 1
  ) {
    const struct HereDocumentFrame *frame =
      &scanner->suspended_frames[frame_index];
    if (
      frame->count > SIZE_MAX >> 1 ||
      !write_state_size(
        writer,
        (frame->count << 1) | (frame->at_line_start ? 1 : 0)
      ) ||
      !write_state_size(writer, frame->body_backquote_depth) ||
      !serialize_document_run(writer, frame->documents, frame->count, false)
    ) {
      return false;
    }
  }
  return true;
}

static bool
serialize_source(const struct Scanner *scanner, struct StateWriter *writer) {
  uint8_t source_data[SCANNER_STATE_CAPACITY];
  size_t source_length;
  return source_snapshot_serialize(
           &scanner->source,
           source_data,
           sizeof(source_data),
           &source_length
         ) &&
    write_state_size(writer, source_length) &&
    write_state_bytes(writer, (const char *)source_data, source_length);
}

static bool serialize_source_contexts(
  const struct Scanner *scanner,
  struct StateWriter *writer
) {
  if (!write_state_size(writer, scanner->context_count)) {
    return false;
  }
  size_t context_depth = 0;
  for (size_t index = 0; index < scanner->context_count; index += 1) {
    const struct SourceContext *context = &scanner->contexts[index];
    if (
      context->count == 0 ||
      context->count > SIZE_MAX - context_depth ||
      !write_state_size(writer, context->opener) ||
      !write_state_size(writer, context->stage_count) ||
      !write_state_size(writer, context->count) ||
      (is_bracket_fallback_context(context->opener) &&
        !write_state_size(writer, context->bracket_chains)) ||
      (is_here_document_context(context->opener) &&
        (!write_state_size(writer, context->substitution_depth) ||
          !write_state_byte(writer, context->here_word))) ||
      (is_backquote_context(context->opener) &&
        !write_state_byte(writer, context->backquote_token)) ||
      !write_state_byte(
        writer,
        (context->local_disabled ? CONTEXT_FLAG_LOCAL_DISABLED : 0) |
          (context->lone_word ? CONTEXT_FLAG_LONE_WORD : 0) |
          (context->suspends_body ? CONTEXT_FLAG_SUSPENDS_BODY : 0)
      )
    ) {
      return false;
    }
    context_depth += context->count;
  }
  return true;
}

static bool
serialize_emission(const struct Scanner *scanner, struct StateWriter *writer) {
  const struct LexicalEmission *emission = &scanner->emission;
  uint8_t flags = (emission->active ? EMISSION_FLAG_ACTIVE : 0) |
    (emission->removed_newline ? EMISSION_FLAG_REMOVED_NEWLINE : 0) |
    (emission->ends_assignment_colon ? EMISSION_FLAG_ASSIGNMENT_COLON : 0) |
    (scanner->here_document_end_line_consumed ? EMISSION_FLAG_END_LINE_CONSUMED
                                              : 0) |
    (emission->tracks_brackets ? EMISSION_FLAG_TRACKS_BRACKETS : 0);
  return write_state_byte(writer, flags) &&
    (!emission->active ||
      (write_state_size(writer, emission->symbol) &&
        write_state_size(writer, emission->remaining))) &&
    (!emission->tracks_brackets ||
      write_state_size(writer, emission->bracket_chains));
}

static bool serialize_scanner_state(
  const struct Scanner *scanner,
  struct StateWriter *writer
) {
  uint8_t flags =
    (scanner->sequence_end_pending ? SCANNER_FLAG_SEQUENCE_END_PENDING : 0) |
    (scanner->active_frame.at_line_start ? SCANNER_FLAG_AT_LINE_START : 0) |
    (scanner->assignment_tilde_allowed ? SCANNER_FLAG_ASSIGNMENT_TILDE : 0);
  return write_state_byte(writer, flags) &&
    write_state_size(writer, scanner->backquote_depth) &&
    write_state_size(writer, scanner->substitution_depth) &&
    write_state_size(writer, scanner->active_frame.body_backquote_depth) &&
    serialize_document_array(
      writer,
      scanner->pending_documents,
      scanner->pending_count,
      true
    ) &&
    serialize_document_array(
      writer,
      scanner->active_frame.documents,
      scanner->active_frame.count,
      false
    ) &&
    serialize_suspended_frames(scanner, writer) &&
    serialize_source(scanner, writer) &&
    serialize_source_contexts(scanner, writer) &&
    serialize_emission(scanner, writer);
}

static bool scanner_state_fits(const struct Scanner *scanner) {
  char data[SCANNER_STATE_CAPACITY];
  struct StateWriter writer = {
    .data = data,
    .capacity = sizeof(data),
  };
  return serialize_scanner_state(scanner, &writer);
}

struct SerializedScannerState {
  const char *data;
  size_t length;
  size_t offset;
};

static bool read_byte(struct SerializedScannerState *state, uint8_t *byte) {
  if (state->offset >= state->length) {
    return false;
  }
  *byte = (uint8_t)state->data[state->offset];
  state->offset += 1;
  return true;
}

static bool read_size(struct SerializedScannerState *state, size_t *value) {
  uint64_t decoded;
  if (
    !source_read_unsigned(
      (const uint8_t *)state->data,
      state->length,
      &state->offset,
      &decoded
    ) ||
    decoded > SIZE_MAX
  ) {
    return false;
  }
  *value = (size_t)decoded;
  return true;
}

static bool deserialize_document(
  struct SerializedScannerState *state,
  struct HereDocument *document,
  bool with_declaration_depth
) {
  uint8_t flags;
  size_t declaration_depth = 0;
  size_t delimiter_length;
  if (
    !read_byte(state, &flags) ||
    (flags & ~(uint8_t)DOCUMENT_FLAG_MASK) != 0 ||
    ((flags & DOCUMENT_FLAG_END_TEXT_CONSUMED) != 0 &&
      (flags & DOCUMENT_FLAG_ENDING) == 0) ||
    (with_declaration_depth && !read_size(state, &declaration_depth)) ||
    !read_size(state, &delimiter_length) ||
    delimiter_length > state->length - state->offset
  ) {
    return false;
  }

  uint8_t *delimiter = NULL;
  if (delimiter_length > 0) {
    delimiter = ts_malloc(delimiter_length);
    if (delimiter == NULL) {
      return false;
    }
    memcpy(delimiter, state->data + state->offset, delimiter_length);
  }
  state->offset += delimiter_length;

  *document = (struct HereDocument){
    .delimiter = delimiter,
    .delimiter_length = delimiter_length,
    .declaration_depth = declaration_depth,
    .quoted = (flags & DOCUMENT_FLAG_QUOTED) != 0,
    .strip_tabs = (flags & DOCUMENT_FLAG_STRIP_TABS) != 0,
    .ending = (flags & DOCUMENT_FLAG_ENDING) != 0,
    .end_text_consumed = (flags & DOCUMENT_FLAG_END_TEXT_CONSUMED) != 0,
  };
  return true;
}

static bool deserialize_document_run(
  struct SerializedScannerState *state,
  struct HereDocument **documents,
  size_t *actual_count,
  size_t count,
  bool with_declaration_depth
) {
  if (
    count > SIZE_MAX / sizeof(struct HereDocument) ||
    count > state->length - state->offset
  ) {
    return false;
  }

  if (count == 0) {
    return true;
  }

  *documents = ts_calloc(count, sizeof(struct HereDocument));
  if (*documents == NULL) {
    return false;
  }

  for (size_t index = 0; index < count; index += 1) {
    if (!deserialize_document(
          state,
          &(*documents)[index],
          with_declaration_depth
        )) {
      return false;
    }
    *actual_count += 1;
  }
  return true;
}

static bool deserialize_document_array(
  struct SerializedScannerState *state,
  struct HereDocument **documents,
  size_t *actual_count,
  bool with_declaration_depth
) {
  size_t count;
  return read_size(state, &count) &&
    deserialize_document_run(
      state,
      documents,
      actual_count,
      count,
      with_declaration_depth
    );
}

static bool deserialize_suspended_frames(
  struct SerializedScannerState *state,
  struct Scanner *scanner
) {
  size_t count;
  if (
    !read_size(state, &count) ||
    count > SIZE_MAX / sizeof(struct HereDocumentFrame) ||
    count > state->length - state->offset
  ) {
    return false;
  }
  if (count > 0) {
    scanner->suspended_frames =
      ts_calloc(count, sizeof(struct HereDocumentFrame));
    if (scanner->suspended_frames == NULL) {
      return false;
    }
  }
  scanner->suspended_frame_count = count;
  for (size_t index = 0; index < count; index += 1) {
    size_t packed_count;
    struct HereDocumentFrame *frame = &scanner->suspended_frames[index];
    if (
      !read_size(state, &packed_count) ||
      !read_size(state, &frame->body_backquote_depth) ||
      !deserialize_document_run(
        state,
        &frame->documents,
        &frame->count,
        packed_count >> 1,
        false
      )
    ) {
      return false;
    }
    frame->at_line_start = (packed_count & 1) != 0;
  }
  return true;
}

static bool deserialize_source(
  struct SerializedScannerState *state,
  struct Scanner *scanner
) {
  size_t source_length;
  if (
    !read_size(state, &source_length) ||
    source_length > state->length - state->offset ||
    !source_snapshot_deserialize(
      &scanner->source,
      (const uint8_t *)state->data + state->offset,
      source_length
    )
  ) {
    return false;
  }
  state->offset += source_length;
  return true;
}

static bool deserialize_source_contexts(
  struct SerializedScannerState *state,
  struct Scanner *scanner
) {
  size_t count;
  if (
    !read_size(state, &count) ||
    count > (state->length - state->offset) / 4 ||
    count > SIZE_MAX / sizeof(*scanner->contexts)
  ) {
    return false;
  }
  if (count > 0) {
    scanner->contexts = ts_calloc(count, sizeof(*scanner->contexts));
    if (scanner->contexts == NULL) {
      return false;
    }
  }
  scanner->context_count = count;
  size_t depth = 0;
  for (size_t index = 0; index < count; index += 1) {
    size_t symbol;
    size_t stage_count;
    size_t repeat;
    size_t chains = 0;
    size_t substitution_depth = 0;
    uint8_t here_word = HERE_WORD_START;
    uint8_t backquote_token = BACKQUOTE_TOKEN_NONE;
    uint8_t disabled;
    if (
      !read_size(state, &symbol) ||
      symbol >= TOKEN_COUNT ||
      !read_size(state, &stage_count) ||
      stage_count == 0 ||
      stage_count > scanner->source.stage_count ||
      !read_size(state, &repeat) ||
      repeat == 0 ||
      repeat > SIZE_MAX - depth ||
      (is_bracket_fallback_context((enum TokenType)symbol) &&
        (!read_size(state, &chains) || chains >= BRACKET_CHAINS_LIMIT)) ||
      (is_here_document_context((enum TokenType)symbol) &&
        (!read_size(state, &substitution_depth) ||
          !read_byte(state, &here_word) ||
          here_word > HERE_WORD_ASSIGNMENT)) ||
      (is_backquote_context((enum TokenType)symbol) &&
        (!read_byte(state, &backquote_token) ||
          backquote_token > BACKQUOTE_TOKEN_ASSIGNMENT)) ||
      !read_byte(state, &disabled) ||
      disabled > CONTEXT_FLAG_MASK ||
      ((disabled & CONTEXT_FLAG_LONE_WORD) != 0 &&
        !is_backquote_context((enum TokenType)symbol)) ||
      ((disabled & CONTEXT_FLAG_SUSPENDS_BODY) != 0 &&
        !is_substitution_context((enum TokenType)symbol))
    ) {
      return false;
    }
    scanner->contexts[index] = (struct SourceContext){
      .opener = (enum TokenType)symbol,
      .stage_count = stage_count,
      .count = repeat,
      .substitution_depth = substitution_depth,
      .bracket_chains = (uint16_t)chains,
      .local_disabled = (disabled & CONTEXT_FLAG_LOCAL_DISABLED) != 0,
      .lone_word = (disabled & CONTEXT_FLAG_LONE_WORD) != 0,
      .suspends_body = (disabled & CONTEXT_FLAG_SUSPENDS_BODY) != 0,
      .backquote_token = (enum BackquoteToken)backquote_token,
      .here_word = (enum HereDocumentWord)here_word,
    };
    depth += repeat;
  }
  return true;
}

static bool deserialize_emission(
  struct SerializedScannerState *state,
  struct Scanner *scanner
) {
  uint8_t flags;
  if (!read_byte(state, &flags) || flags > EMISSION_FLAG_MASK) {
    return false;
  }
  scanner->emission.active = (flags & EMISSION_FLAG_ACTIVE) != 0;
  scanner->emission.removed_newline =
    (flags & EMISSION_FLAG_REMOVED_NEWLINE) != 0;
  scanner->emission.ends_assignment_colon =
    (flags & EMISSION_FLAG_ASSIGNMENT_COLON) != 0;
  scanner->here_document_end_line_consumed =
    (flags & EMISSION_FLAG_END_LINE_CONSUMED) != 0;
  scanner->emission.tracks_brackets =
    (flags & EMISSION_FLAG_TRACKS_BRACKETS) != 0;
  if (!scanner->emission.active) {
    return !scanner->emission.tracks_brackets;
  }
  size_t symbol;
  if (
    !read_size(state, &symbol) ||
    symbol >= TOKEN_COUNT ||
    (symbol != SOURCE_BEGIN &&
      LEXICAL_SOURCES[symbol].kind == LEXICAL_SOURCE_NONE) ||
    !read_size(state, &scanner->emission.remaining) ||
    scanner->emission.remaining == 0
  ) {
    return false;
  }
  scanner->emission.symbol = (enum TokenType)symbol;
  size_t chains = 0;
  if (
    scanner->emission.tracks_brackets &&
    (!read_size(state, &chains) || chains >= BRACKET_CHAINS_LIMIT)
  ) {
    return false;
  }
  scanner->emission.bracket_chains = (uint16_t)chains;
  return true;
}

static bool deserialize_scanner_state(
  struct Scanner *scanner,
  const char *data,
  size_t length
) {
  struct SerializedScannerState state = {
    .data = data,
    .length = length,
  };
  uint8_t flags;
  if (
    !read_byte(&state, &flags) ||
    flags > SCANNER_FLAG_MASK ||
    !read_size(&state, &scanner->backquote_depth) ||
    !read_size(&state, &scanner->substitution_depth) ||
    !read_size(&state, &scanner->active_frame.body_backquote_depth) ||
    !deserialize_document_array(
      &state,
      &scanner->pending_documents,
      &scanner->pending_count,
      true
    ) ||
    !deserialize_document_array(
      &state,
      &scanner->active_frame.documents,
      &scanner->active_frame.count,
      false
    ) ||
    !deserialize_suspended_frames(&state, scanner) ||
    !deserialize_source(&state, scanner) ||
    !deserialize_source_contexts(&state, scanner) ||
    !deserialize_emission(&state, scanner) ||
    state.offset != state.length
  ) {
    return false;
  }
  scanner->sequence_end_pending =
    (flags & SCANNER_FLAG_SEQUENCE_END_PENDING) != 0;
  scanner->active_frame.at_line_start =
    (flags & SCANNER_FLAG_AT_LINE_START) != 0;
  scanner->assignment_tilde_allowed =
    (flags & SCANNER_FLAG_ASSIGNMENT_TILDE) != 0;
  return true;
}

unsigned
tree_sitter_dash_external_scanner_serialize(void *payload, char *buffer) {
  const struct Scanner *scanner = payload;
  if (scanner == NULL) {
    return 0;
  }

  struct StateWriter writer = {
    .data = buffer + 1,
    .capacity = SCANNER_STATE_CAPACITY,
  };
  if (!serialize_scanner_state(scanner, &writer)) {
    return 0;
  }

  buffer[0] = SCANNER_SERIALIZATION_VERSION;
  return (unsigned)writer.length + 1;
}

void tree_sitter_dash_external_scanner_deserialize(
  void *payload,
  const char *buffer,
  unsigned length
) {
  struct Scanner *scanner = payload;
  if (scanner == NULL) {
    return;
  }

  clear_scanner(scanner);
  if (
    length < 1 ||
    length > TREE_SITTER_SERIALIZATION_BUFFER_SIZE ||
    (uint8_t)buffer[0] != SCANNER_SERIALIZATION_VERSION
  ) {
    return;
  }

  if (!deserialize_scanner_state(scanner, buffer + 1, length - 1)) {
    clear_scanner(scanner);
  }
}

static bool scan_here_document_sequence_begin(TSLexer *lexer) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  size_t position = lookahead->position;
  lexer->mark_end(lexer);
  scan_horizontal_blanks(lexer);
  if (lexer->lookahead == '#') {
    advance_to_comment_end(lexer, NULL);
  }
  if (lexer->lookahead == '\n' || lexer_at_eof(lexer)) {
    lexer->result_symbol = HERE_DOCUMENT_SEQUENCE_BEGIN;
    return true;
  }
  if (lookahead->position != position) {
    lookahead_seek(lookahead, position);
  }
  return false;
}

static bool element_boundary_starts(TSLexer *lexer, const bool *valid) {
  return element_boundary_symbols_are_valid(valid) &&
    (is_horizontal_blank(lexer->lookahead) ||
      is_one_of(lexer->lookahead, ";&|\n#"));
}

static bool shell_boundary_starts(TSLexer *lexer, const bool *valid) {
  int32_t character = lexer->lookahead;
  bool comment_boundary =
    valid[COMMENT_BOUNDARY] || valid[TRAILING_COMMENT_BOUNDARY];
  bool shell_boundary = valid[PATTERN_CONTINUATION] ||
    valid[PATTERN_END] ||
    valid[PIPE_CONTINUATION] ||
    valid[AND_OR_CONTINUATION] ||
    valid[REDIRECT_LIST_BEGIN] ||
    valid[CASE_ITEM_END] ||
    comment_boundary;
  return shell_boundary &&
    (is_horizontal_blank(character) ||
      (character == '|' &&
        (valid[PATTERN_CONTINUATION] ||
          valid[PIPE_CONTINUATION] ||
          valid[AND_OR_CONTINUATION])) ||
      (character == ')' && valid[PATTERN_END]) ||
      (character == ';' && valid[CASE_ITEM_END]) ||
      (character == '#' && comment_boundary) ||
      ((character == '<' || character == '>' || is_decimal_digit(character)) &&
        valid[REDIRECT_LIST_BEGIN]));
}

static enum TokenType static_command_word(TSLexer *lexer) {
  struct ByteBuffer word = {.limit = 8};
  bool double_quoted = false;
  bool valid = true;
  while (
    !lexer_at_eof(lexer) && (double_quoted || !is_token_delimiter(lexer))
  ) {
    int32_t character = lexer->lookahead;
    lexer->advance(lexer, false);
    if (character == '"') {
      double_quoted = !double_quoted;
      continue;
    }
    if (!double_quoted && character == '\'') {
      valid = scan_delimiter_single_quoted_segment(lexer, &word, false);
    } else if (!double_quoted && character == '$' && lexer->lookahead == '\'') {
      lexer->advance(lexer, false);
      valid = scan_delimiter_single_quoted_segment(lexer, &word, true);
    } else if (character == '$' || character == '`') {
      valid = false;
    } else if (character == '\\') {
      if (lexer_at_eof(lexer)) {
        valid = false;
      } else {
        if (double_quoted && !is_one_of(lexer->lookahead, "$`\\\"")) {
          valid = append_byte(&word, '\\');
        }
        valid = valid && append_codepoint(&word, lexer->lookahead);
        lexer->advance(lexer, false);
      }
    } else {
      valid = append_codepoint(&word, character);
    }
    if (!valid) break;
  }
  enum TokenType symbol = TOKEN_COUNT;
  if (valid && !double_quoted) {
    static const struct {
      const char *text;
      enum TokenType symbol;
    } words[] = {
      {"export", DECLARATION_COMMAND},
      {"readonly", DECLARATION_COMMAND},
      {"local", DECLARATION_COMMAND},
      {"command", FORWARDING_COMMAND},
      {"-p", FORWARDING_OPTION},
      {"--", FORWARDING_OPTION_END},
    };
    for (size_t index = 0; index < COUNT_OF(words); index += 1) {
      if (
        word.length == strlen(words[index].text) &&
        memcmp(word.data, words[index].text, word.length) == 0
      ) {
        symbol = words[index].symbol;
        break;
      }
    }
  }
  ts_free(word.data);
  return symbol;
}

/* Removed source before the end still belongs to the current document. */
static bool at_document_input_end(TSLexer *lexer) {
  if (!lexer->eof(lexer) && lexer->lookahead != SOURCE_BACKQUOTE_BOUNDARY) {
    return false;
  }
  struct LogicalLexer *input = lookahead_input(lexer);
  return input != NULL && logical_raw_position(input) == input->consumed;
}

enum ScanStep {
  SCAN_STEP_SKIP,
  SCAN_STEP_ACCEPT,
  SCAN_STEP_REJECT,
};

static enum ScanStep scan_outcome(bool accepted) {
  return accepted ? SCAN_STEP_ACCEPT : SCAN_STEP_REJECT;
}

static enum ScanStep accept_zero_width(TSLexer *lexer, enum TokenType symbol) {
  return scan_outcome(emit_symbol(lexer, symbol));
}

static enum ScanStep dispatch_document_starts(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  if (
    valid[HERE_DOCUMENT_START_AFTER_REMAINDER] &&
    (!word_source_can_continue(valid) || is_token_delimiter(lexer)) &&
    activate_startable_pending_documents(scanner)
  ) {
    return accept_zero_width(lexer, HERE_DOCUMENT_START_AFTER_REMAINDER);
  }
  if (!at_document_input_end(lexer)) {
    return SCAN_STEP_SKIP;
  }
  if (
    valid[HERE_DOCUMENT_START_AT_EOF] &&
    activate_startable_pending_documents(scanner)
  ) {
    return accept_zero_width(lexer, HERE_DOCUMENT_START_AT_EOF);
  }
  if (valid[HERE_DOCUMENT_EOF] && scanner->active_frame.count > 0) {
    finish_active_document(scanner);
    return accept_zero_width(lexer, HERE_DOCUMENT_EOF);
  }
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_static_command_word(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  (void)scanner;
  if (
    !valid[DECLARATION_COMMAND] &&
    !valid[FORWARDING_COMMAND] &&
    !valid[FORWARDING_OPTION] &&
    !valid[FORWARDING_OPTION_END]
  ) {
    return SCAN_STEP_SKIP;
  }
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  size_t start = lookahead->position;
  lexer->mark_end(lexer);
  enum TokenType word = static_command_word(lexer);
  if (word != TOKEN_COUNT && valid[word]) {
    if (valid[FNAME_TOKEN]) scan_horizontal_blanks(lexer);
    if (!valid[FNAME_TOKEN] || lexer->lookahead != '(') {
      lexer->result_symbol = (TSSymbol)word;
      return SCAN_STEP_ACCEPT;
    }
  }
  if (lookahead->length > 0) lookahead_seek(lookahead, start);
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_document_boundaries(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  if (
    valid[COMMAND_SUBSTITUTION_BODY_BEGIN] ||
    valid[COMMAND_SUBSTITUTION_REMAINDER_END]
  ) {
    return scan_outcome(
      scan_command_substitution_body_begin(scanner, lexer, valid)
    );
  }
  if (
    valid[HERE_DOCUMENT_SEQUENCE_END] &&
    scan_here_document_sequence_end(scanner, lexer, valid)
  ) {
    return SCAN_STEP_ACCEPT;
  }
  if (
    valid[HERE_DOCUMENT_END_COMMIT] &&
    scanner->active_frame.count > 0 &&
    scan_here_document_end_commit(scanner, lexer)
  ) {
    return SCAN_STEP_ACCEPT;
  }
  if (scanner->active_frame.count > 0 && scanner->active_frame.at_line_start) {
    return scan_outcome(
      scan_here_document_content_line_start(scanner, lexer, valid)
    );
  }
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_names_and_arithmetic(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  (void)scanner;
  if (
    (valid[ASSIGNMENT_NAME_TOKEN] || valid[FNAME_TOKEN]) &&
    is_name_start_character(lexer->lookahead)
  ) {
    return scan_outcome(scan_name_or_reserved_word(lexer, valid));
  }
  bool arithmetic_boundary = arithmetic_operator_boundary_is_valid(valid) ||
    valid[ARITHMETIC_CLOSING_BOUNDARY] ||
    valid[ARITHMETIC_EXPANSION_CLOSING_BOUNDARY];
  if (
    arithmetic_boundary &&
    (is_arithmetic_operator_start(lexer->lookahead) ||
      lexer->lookahead == ')' ||
      arithmetic_whitespace(lexer->lookahead))
  ) {
    return scan_outcome(scan_arithmetic_boundary(lexer, valid));
  }
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_document_line_tokens(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  if (
    scanner->active_frame.count > 0 &&
    lexer->lookahead == '\n' &&
    valid[SEPARATOR_NEWLINE] &&
    !has_startable_pending_document(scanner) &&
    !valid[HERE_END_BEGIN] &&
    !valid[HERE_END_STRIP_BEGIN] &&
    !valid[FUNCTION_BODY_CONTINUATION_BOUNDARY]
  ) {
    return scan_outcome(scan_here_document_body_newline(scanner, lexer, valid));
  }
  if (valid[HERE_END_BEGIN] || valid[HERE_END_STRIP_BEGIN]) {
    return scan_outcome(
      scan_here_document_delimiter(scanner, lexer, valid[HERE_END_STRIP_BEGIN])
    );
  }
  if (valid[DOLLAR_SINGLE_QUOTE_ESCAPE] && lexer->lookahead == '\\') {
    return scan_outcome(scan_dollar_single_quote_token(lexer));
  }
  if (valid[FUNCTION_BODY_CONTINUATION_BOUNDARY]) {
    return accept_zero_width(lexer, FUNCTION_BODY_CONTINUATION_BOUNDARY);
  }
  if (
    valid[HERE_DOCUMENT_SEQUENCE_BEGIN] &&
    has_startable_pending_document(scanner) &&
    scan_here_document_sequence_begin(lexer)
  ) {
    return SCAN_STEP_ACCEPT;
  }
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_boundaries(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  if (element_boundary_starts(lexer, valid)) {
    return scan_outcome(scan_element_boundary(scanner, lexer, valid));
  }
  if (valid[PRE_NEWLINE_BLANK] && is_horizontal_blank(lexer->lookahead)) {
    scan_horizontal_blanks(lexer);
    lexer->mark_end(lexer);
    return scan_outcome(classify_layout_run(lexer, valid));
  }
  if (shell_boundary_starts(lexer, valid)) {
    return scan_outcome(scan_shell_boundary(lexer, valid));
  }
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_pattern_tokens(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  int32_t character = lexer->lookahead;
  if (character == '$' && valid[DOLLAR_EXPANSION_START]) {
    return scan_outcome(scan_dollar_expansion_start(lexer));
  }
  if (character == ':' && valid[PATTERN_CHARACTER_CLASS_END_COLON]) {
    return scan_outcome(
      accept_character(lexer, PATTERN_CHARACTER_CLASS_END_COLON) &&
      lexer->lookahead == ']'
    );
  }
  if (character == '[' && valid[PATTERN_SPECIAL_LEFT_BRACKET]) {
    return scan_outcome(scan_pattern_special_left_bracket(lexer, valid));
  }
  if (
    character == '-' &&
    (valid[PATTERN_BRACKET_HYPHEN] || valid[PATTERN_BRACKET_RANGE_OPERATOR])
  ) {
    return scan_outcome(scan_pattern_bracket_hyphen(lexer, valid));
  }
  static const enum TokenType bracket_characters[] = {
    PARAMETER_PATTERN_BRACKET_CHARACTER,
    PATTERN_BRACKET_CHARACTER,
  };
  for (size_t index = 0; index < COUNT_OF(bracket_characters); index += 1) {
    if (
      valid[bracket_characters[index]] &&
      scan_pattern_bracket_character(scanner, lexer, bracket_characters[index])
    ) {
      return SCAN_STEP_ACCEPT;
    }
  }
  return SCAN_STEP_SKIP;
}

static enum ScanStep dispatch_punctuation_and_words(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  (void)scanner;
  int32_t character = lexer->lookahead;
  if (character == '{') {
    return scan_outcome(
      valid[LEFT_BRACE] && scan_delimited_character_token(lexer, LEFT_BRACE)
    );
  }
  if (character == '}') {
    bool reachable = valid[RIGHT_BRACE] &&
      (!valid[TERM_CONTINUATION] ||
        !(valid[WORD_SEPARATOR_BEGIN] ||
          valid[ASSIGNMENT_SEPARATOR_BEGIN] ||
          valid[REDIRECT_SEPARATOR_BEGIN]));
    return scan_outcome(
      (reachable || valid[FNAME_TOKEN]) &&
      scan_delimited_character_token(lexer, RIGHT_BRACE)
    );
  }
  if (is_decimal_digit(character) && valid[BRACED_POSITIONAL_PARAMETER_START]) {
    return scan_outcome(scan_braced_numeric_parameter_start(lexer, valid));
  }
  if (is_decimal_digit(character)) {
    return scan_outcome(valid[FILE_DESCRIPTOR] && scan_file_descriptor(lexer));
  }
  if (character == '!') {
    return scan_outcome(
      (valid[PIPELINE_NEGATION] || valid[FNAME_TOKEN]) &&
      scan_delimited_character_token(lexer, PIPELINE_NEGATION)
    );
  }
  return scan_outcome(
    is_lowercase_letter(character) && scan_reserved_word(lexer, valid)
  );
}

static bool
scan_dispatch(struct Scanner *scanner, TSLexer *lexer, const bool *valid) {
  static enum ScanStep (*const steps[])(
    struct Scanner *,
    TSLexer *,
    const bool *
  ) = {
    dispatch_document_starts,
    dispatch_static_command_word,
    dispatch_document_boundaries,
    dispatch_names_and_arithmetic,
    dispatch_document_line_tokens,
    dispatch_boundaries,
    dispatch_pattern_tokens,
    dispatch_punctuation_and_words,
  };
  for (size_t index = 0; index < COUNT_OF(steps); index += 1) {
    enum ScanStep step = steps[index](scanner, lexer, valid);
    if (step != SCAN_STEP_SKIP) {
      return step == SCAN_STEP_ACCEPT;
    }
  }
  return false;
}

static void complete_ambiguous_substitutions(struct LookaheadLexer *lookahead) {
  while (!lookahead->failed && lookahead->active != SIZE_MAX) {
    lookahead->requested = SIZE_MAX;
    size_t index = lookahead->active;
    struct AmbiguousSubstitution substitution = lookahead->substitutions[index];
    if (!substitution.scan_complete) {
      if (substitution.scan_started) {
        lookahead_seek(lookahead, substitution.resume);
      } else {
        lookahead_seek(lookahead, substitution.start);
        lookahead->lexer.advance(&lookahead->lexer, false);
        substitution.scan_started = true;
      }
      enum TokenType symbol = classify_arithmetic_source(
        &substitution.context,
        &lookahead->lexer,
        &substitution.scan,
        &substitution.result
      );
      substitution.resume = lookahead->position;
      if (!lookahead_is_pending(&lookahead->lexer)) {
        substitution.arithmetic = symbol != TOKEN_COUNT;
        substitution.scan_complete = true;
        clear_arithmetic_scan(&substitution.scan);
      }
    }
    if (!lookahead_is_pending(&lookahead->lexer)) {
      if (substitution.arithmetic) {
        if (substitution.result == ARITHMETIC_VALIDATION_VALID) {
          lookahead->lexer.advance(&lookahead->lexer, false);
        }
      } else {
        if (substitution.command == NULL) {
          substitution.command = ts_calloc(1, sizeof(struct EmbeddedSkip));
          substitution.command_resume = substitution.start;
        }
        if (substitution.command == NULL) {
          lookahead_fail(lookahead);
        } else {
          lookahead_seek(lookahead, substitution.command_resume);
          substitution.result = resume_embedded_construct(
            substitution.raw ? NULL : &substitution.context,
            &lookahead->lexer,
            ')',
            false,
            substitution.command
          );
          substitution.command_resume = lookahead->position;
        }
      }
      if (!lookahead_is_pending(&lookahead->lexer)) {
        substitution.end = lookahead->position;
        substitution.complete = true;
        if (substitution.command != NULL) {
          clear_embedded_skip(substitution.command);
          ts_free(substitution.command);
          substitution.command = NULL;
        }
      }
    }
    lookahead->substitutions[index] = substitution;
    lookahead->active =
      substitution.complete ? substitution.parent : lookahead->requested;
  }
  lookahead->requested = SIZE_MAX;
}

static bool lookahead_init(struct LookaheadLexer *lookahead, TSLexer *lexer) {
  *lookahead = (struct LookaheadLexer){0};
  if (lexer->advance != logical_advance) {
    return false;
  }
  *lookahead = (struct LookaheadLexer){
    .lexer = *lexer,
    .source = lexer,
    .requested = SIZE_MAX,
    .active = SIZE_MAX,
  };
  lookahead->lexer.advance = lookahead_advance;
  lookahead->lexer.mark_end = lookahead_mark_end;
  lookahead->lexer.eof = lookahead_eof;
  lookahead->views = ts_calloc(1, sizeof(*lookahead->views));
  if (lookahead->views == NULL) {
    lookahead_fail(lookahead);
    return false;
  }
  lookahead->view_count = 1;
  lookahead->view_capacity = 1;
  lookahead->views[0].input = (struct LogicalLexer *)lexer;
  return true;
}

static void lookahead_clear(struct LookaheadLexer *lookahead) {
  ts_free(lookahead->characters);
  for (size_t index = 0; index < lookahead->substitution_count; index += 1) {
    clear_arithmetic_scan(&lookahead->substitutions[index].scan);
    if (lookahead->substitutions[index].command != NULL) {
      clear_embedded_skip(lookahead->substitutions[index].command);
      ts_free(lookahead->substitutions[index].command);
    }
  }
  ts_free(lookahead->substitutions);
  for (size_t index = 1; index < lookahead->view_count; index += 1) {
    logical_clear(lookahead->views[index].input);
    ts_free(lookahead->views[index].input);
  }
  ts_free(lookahead->views);
  *lookahead = (struct LookaheadLexer){0};
}

static enum BackquoteToken retained_word_token(TSLexer *lexer) {
  struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
  size_t position = lookahead->position;
  enum TokenType command = static_command_word(lexer);
  if (lookahead->position != position) lookahead_seek(lookahead, position);
  if (command == DECLARATION_COMMAND) return BACKQUOTE_TOKEN_DECLARATION;
  int32_t first = lexer->lookahead;
  if (first == '!' || first == '{' || first == '}') {
    lexer->advance(lexer, false);
    return !is_token_delimiter(lexer) ? BACKQUOTE_TOKEN_WORD
      : first == '!'                  ? BACKQUOTE_TOKEN_NEGATION
      : first == '{'                  ? BACKQUOTE_TOKEN_BRACE
                                      : BACKQUOTE_TOKEN_STOP;
  }
  if (!is_name_start_character(first))
    return command == FORWARDING_COMMAND ? BACKQUOTE_TOKEN_FORWARDING
                                         : BACKQUOTE_TOKEN_WORD;
  char word[NAME_CANDIDATE_CAPACITY];
  read_name_candidate(lexer, word);
  if (lexer->lookahead == '=') return BACKQUOTE_TOKEN_ASSIGNMENT;
  if (!is_token_delimiter(lexer))
    return command == FORWARDING_COMMAND ? BACKQUOTE_TOKEN_FORWARDING
                                         : BACKQUOTE_TOKEN_WORD;
  const struct ReservedWord *reserved = find_reserved_word(word);
  if (reserved != NULL) {
    switch (reserved->symbol) {
      case IF_KEYWORD:
        return BACKQUOTE_TOKEN_IF;
      case FOR_KEYWORD:
        return BACKQUOTE_TOKEN_FOR;
      case CASE_KEYWORD:
        return BACKQUOTE_TOKEN_CASE;
      case WHILE_KEYWORD:
        return BACKQUOTE_TOKEN_WHILE;
      case UNTIL_KEYWORD:
        return BACKQUOTE_TOKEN_UNTIL;
      case IN_KEYWORD:
        return BACKQUOTE_TOKEN_INVALID;
      default:
        return BACKQUOTE_TOKEN_STOP;
    }
  }
  return forbidden_function_name(word) ? BACKQUOTE_TOKEN_WORD
    : command == FORWARDING_COMMAND    ? BACKQUOTE_TOKEN_NAME_FORWARDING
                                       : BACKQUOTE_TOKEN_NAME_WORD;
}

static bool
scan_backquote_compound_remainder(struct Scanner *scanner, TSLexer *lexer) {
  lexer->mark_end(lexer);
  scan_horizontal_blanks(lexer);
  int32_t first = lexer->lookahead;
  if (first == ';') {
    lexer->advance(lexer, false);
    if (lexer->lookahead != ';') return false;
  } else if (
    first != ')' &&
    first != '(' &&
    (!is_word_element_start(lexer) ||
      first == '<' ||
      first == '>' ||
      (is_decimal_digit(first) && advance_file_descriptor(lexer)))
  ) {
    return false;
  }
  if (
    has_startable_pending_document(scanner) &&
    (is_decimal_digit(first) || is_word_element_start(lexer))
  ) {
    top_context(scanner)->backquote_token = is_decimal_digit(first)
      ? BACKQUOTE_TOKEN_WORD
      : retained_word_token(lexer);
  }
  lexer->result_symbol = BACKQUOTE_COMPOUND_REMAINDER_BEGIN;
  return true;
}

static bool
advance_backquote_terminator(TSLexer *lexer, int32_t first, bool parenthesis) {
  if (first == ')' || (first == '(' && parenthesis)) {
    lexer->advance(lexer, false);
    return true;
  }
  if (first == ';' || first == '}') {
    lexer->advance(lexer, false);
    bool terminator =
      first == ';' ? lexer->lookahead == ';' : is_token_delimiter(lexer);
    if (terminator && first == ';') lexer->advance(lexer, false);
    return terminator;
  }
  if (is_lowercase_letter(first)) {
    char word[RESERVED_WORD_CAPACITY];
    return read_reserved_word(lexer, word) &&
      is_backquote_terminator_word(word);
  }
  return false;
}

static bool scan_backquote_remainder(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  struct SourceContext *context = top_context(scanner);
  if (
    context == NULL ||
    !is_backquote_context(context->opener) ||
    !(valid[BACKQUOTE_REMAINDER_BEGIN] ||
      valid[BACKQUOTE_REMAINDER_TAIL_BEGIN] ||
      valid[BACKQUOTE_REMAINDER_PARENTHESIS_BEGIN] ||
      valid[BACKQUOTE_COMPOUND_REMAINDER_BEGIN]) ||
    lexer_at_input_end(lexer)
  )
    return false;
  size_t inherited = pending_backquote_context(scanner);
  if (inherited != SIZE_MAX && valid[BACKQUOTE_REMAINDER_BEGIN]) {
    scanner->contexts[inherited].backquote_token = BACKQUOTE_TOKEN_NONE;
    do {
      lexer->advance(lexer, false);
    } while (!lexer_at_input_end(lexer));
    return emit_symbol(lexer, BACKQUOTE_REMAINDER_BEGIN);
  }
  if (valid[BACKQUOTE_COMPOUND_REMAINDER_BEGIN]) {
    return scan_backquote_compound_remainder(scanner, lexer);
  }
  bool pending = has_startable_pending_document(scanner);
  enum TokenType symbol = BACKQUOTE_REMAINDER_TAIL_BEGIN;
  if (pending && valid[symbol]) return false;
  if (!valid[symbol]) {
    struct LookaheadLexer *lookahead = (struct LookaheadLexer *)lexer;
    size_t start = lookahead->position;
    int32_t first = lexer->lookahead;
    enum TokenType begin = first == '(' ? BACKQUOTE_REMAINDER_PARENTHESIS_BEGIN
                                        : BACKQUOTE_REMAINDER_BEGIN;
    bool terminator = advance_backquote_terminator(
      lexer,
      first,
      valid[begin] && !context->lone_word
    );
    if (terminator && pending && valid[begin]) {
      context->backquote_token = first == ')'
        ? BACKQUOTE_TOKEN_RIGHT_PARENTHESIS
        : first == '(' ? BACKQUOTE_TOKEN_LEFT_PARENTHESIS
                       : BACKQUOTE_TOKEN_STOP;
      return emit_symbol(lexer, begin);
    }
    if (lookahead->length > start) lookahead_seek(lookahead, start);
    if (!terminator || !valid[begin]) return false;
    symbol = begin;
  }
  do {
    lexer->advance(lexer, false);
  } while (!lexer_at_input_end(lexer));
  return emit_symbol(lexer, symbol);
}

static bool scan_with_lookahead(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid_symbols,
  bool (*scan)(struct Scanner *, TSLexer *, const bool *)
) {
  struct LookaheadLexer lookahead;
  if (!lookahead_init(&lookahead, lexer)) {
    return false;
  }
  bool accepted = scan(scanner, &lookahead.lexer, valid_symbols);
  lexer->result_symbol = lookahead.lexer.result_symbol;
  accepted = accepted && !lookahead.failed;
  lookahead_clear(&lookahead);
  return accepted;
}

static bool scanner_source_ready(struct Scanner *scanner) {
  if (scanner->source.stage_count > 0) {
    return true;
  }
  scanner->source.stages = ts_malloc(sizeof(*scanner->source.stages));
  if (scanner->source.stages == NULL) {
    return false;
  }
  scanner->source.stages[0] = source_stage(SOURCE_REMOVE_CONTINUATIONS);
  scanner->source.stage_count = 1;
  return true;
}

static struct SourceStage *
grow_source_stages(struct Scanner *scanner, size_t added) {
  size_t count = scanner->source.stage_count;
  if (count > SIZE_MAX / sizeof(struct SourceStage) - added) {
    return NULL;
  }
  struct SourceStage *stages =
    ts_realloc(scanner->source.stages, (count + added) * sizeof(*stages));
  if (stages == NULL) {
    return NULL;
  }
  scanner->source.stages = stages;
  scanner->source.stage_count += added;
  return stages + count - 1;
}

/* Reject before mutation to keep failed pushes serializable. */
static bool
source_context_push(struct Scanner *scanner, enum TokenType opener) {
  size_t depth = 0;
  for (size_t index = 0; index < scanner->context_count; index += 1) {
    size_t count = scanner->contexts[index].count;
    if (count == 0 || count >= SIZE_MAX - depth) {
      return false;
    }
    depth += count;
  }
  size_t count = scanner->source.stage_count;
  struct SourceStage *local = &scanner->source.stages[count - 1];
  struct SourceContext *previous = top_context(scanner);
  if (
    previous != NULL &&
    previous->opener == opener &&
    previous->stage_count == count &&
    previous->local_disabled == local->disabled
  ) {
    previous->count += 1;
  } else {
    if (scanner->context_count >= SIZE_MAX / sizeof(*scanner->contexts)) {
      return false;
    }
    struct SourceContext *contexts = ts_realloc(
      scanner->contexts,
      (scanner->context_count + 1) * sizeof(*contexts)
    );
    if (contexts == NULL) {
      return false;
    }
    scanner->contexts = contexts;
    contexts[scanner->context_count++] = (struct SourceContext){
      .opener = opener,
      .stage_count = count,
      .count = 1,
      .substitution_depth =
        is_here_document_context(opener) ? scanner->substitution_depth : 0,
      .local_disabled = local->disabled,
    };
  }
  if (is_backquote_context(opener)) {
    struct SourceStage *stages = grow_source_stages(scanner, 2);
    if (stages == NULL) {
      return false;
    }
    backquote_stages(opener == DOUBLE_QUOTED_BACKQUOTE_START, stages);
    scanner->backquote_depth += 1;
  } else if (is_here_document_context(opener)) {
    if (scanner->active_frame.count == 0) {
      return false;
    }
    struct SourceStage *stages = grow_source_stages(scanner, 2);
    if (stages == NULL) {
      return false;
    }
    here_document_body_stages(&scanner->active_frame.documents[0], stages);
  } else if (opener == SQ_OPEN || opener == DOLLAR_SQ_OPEN) {
    local->disabled = true;
  } else if (
    opener == DQ_OPEN || opener == COMMAND_OPEN || is_arithmetic_context(opener)
  ) {
    local->disabled = false;
  }
  if (is_substitution_context(opener)) {
    size_t body = here_document_stage_index(scanner->source.stages, count - 1);
    if (body != SIZE_MAX && !scanner->source.stages[body].disabled) {
      scanner->source.stages[body].disabled = true;
      top_context(scanner)->suspends_body = true;
    }
  }
  return scanner_state_fits(scanner);
}

static bool source_context_pop(struct Scanner *scanner) {
  struct SourceContext *top = top_context(scanner);
  if (top == NULL || top->count == 0) {
    return false;
  }
  struct SourceContext context = *top;
  if (is_here_document_context(context.opener)) {
    size_t pending = pending_backquote_context(scanner);
    if (pending != SIZE_MAX) {
      scanner->contexts[pending].backquote_token =
        context.here_word == HERE_WORD_ASSIGNMENT ? BACKQUOTE_TOKEN_ASSIGNMENT
        : context.here_word == HERE_WORD_START    ? BACKQUOTE_TOKEN_NAME_WORD
                                                  : BACKQUOTE_TOKEN_WORD;
    }
  }
  bool removed = --top->count == 0;
  if (removed) {
    scanner->context_count -= 1;
  }
  scanner->source.stage_count = context.stage_count;
  scanner->source.stages[context.stage_count - 1].disabled =
    context.local_disabled;
  if (removed && context.suspends_body) {
    size_t body = here_document_stage_index(
      scanner->source.stages,
      context.stage_count - 1
    );
    if (body == SIZE_MAX) {
      return false;
    }
    scanner->source.stages[body].disabled = false;
  }
  if (is_backquote_context(context.opener)) {
    if (scanner->backquote_depth == 0) {
      return false;
    }
    scanner->backquote_depth -= 1;
  }
  return true;
}

static bool source_context_pop_comment(struct Scanner *scanner) {
  return top_context_opener(scanner) != COMMENT_START ||
    source_context_pop(scanner);
}

static bool begin_here_document_end(
  struct Scanner *scanner,
  TSLexer *lexer,
  const struct HereDocumentBoundary *bound,
  const bool *valid
) {
  enum TokenType symbol = bound->document->quoted
    ? QUOTED_HERE_DOCUMENT_END_BEGIN
    : HERE_DOCUMENT_END_BEGIN;
  if (!valid[symbol]) {
    return false;
  }
  while (scanner->context_count > bound->context + 1) {
    if (!source_context_pop(scanner)) {
      return false;
    }
  }
  while (scanner->suspended_frame_count > bound->frame) {
    restore_suspended_documents(scanner);
  }
  scanner->substitution_depth =
    scanner->contexts[bound->context].substitution_depth;
  scanner->backquote_depth = scanner->active_frame.body_backquote_depth;
  discard_enclosed_pending_documents(scanner);
  scanner->active_frame.documents[0].ending = true;
  scanner->active_frame.at_line_start = false;
  scanner->sequence_end_pending = false;
  lexer->result_symbol = (TSSymbol)symbol;
  return true;
}

static bool
source_context_commit(struct Scanner *scanner, enum TokenType symbol) {
  switch (symbol) {
    case SQ_OPEN:
    case DQ_OPEN:
    case DOLLAR_SQ_OPEN:
    case PARAMETER_OPEN:
    case COMMAND_OPEN:
    case BACKQUOTE_START:
    case DOUBLE_QUOTED_BACKQUOTE_START:
    case WORD_TILDE_START:
    case ASSIGNMENT_TILDE_START:
    case PARAMETER_TILDE_START:
    case WORD_FALLBACK_LITERAL_BEGIN:
    case ASSIGNMENT_FALLBACK_LITERAL_BEGIN:
    case PARAMETER_FALLBACK_LITERAL_BEGIN:
      return source_context_push(scanner, symbol);
    case SQ_CLOSE:
    case DQ_CLOSE:
    case DOLLAR_SQ_CLOSE:
    case PARAMETER_CLOSE:
    case ARITHMETIC_EXPANSION_END:
      return source_context_pop(scanner);
    case BACKQUOTE_END: {
      size_t pending = pending_backquote_context(scanner);
      if (pending != SIZE_MAX) {
        scanner->contexts[pending].backquote_token = BACKQUOTE_TOKEN_NONE;
      }
      if (
        !source_context_pop_comment(scanner) || !source_context_pop(scanner)
      ) {
        return false;
      }
      discard_enclosed_pending_documents(scanner);
      return true;
    }
    case COMMAND_SUBSTITUTION_END:
      if (scanner->substitution_depth == 0 || !source_context_pop(scanner)) {
        return false;
      }
      scanner->substitution_depth -= 1;
      discard_enclosed_pending_documents(scanner);
      return true;
    case HERE_DOCUMENT_LINE_END:
    case NEWLINE:
    case COMMENT_LINE_END:
    case LOGICAL_NEWLINE_BEGIN:
      if (!source_context_pop_comment(scanner)) {
        return false;
      }
      if (
        symbol == HERE_DOCUMENT_LINE_END &&
        !activate_startable_pending_documents(scanner)
      ) {
        return false;
      }
      if (scanner->active_frame.count > 0) {
        scanner->active_frame.at_line_start = true;
      }
      return true;
    default:
      return true;
  }
}

static uint16_t
bracket_chains_after_token(const struct LogicalLexer *input, uint16_t chains) {
  for (size_t index = 0; index < input->cursor.logical_count; index += 1) {
    const struct SourceCharacter *character = &input->cursor.logical[index];
    if (character->origin.end <= 0) {
      continue;
    }
    if ((size_t)character->origin.end + input->base > input->mark) {
      break;
    }
    chains = bracket_chains_after_literal(chains, character->value);
  }
  return chains;
}

static bool begin_lexical_emission(
  struct Scanner *scanner,
  const struct LogicalLexer *input,
  enum TokenType symbol,
  const bool *valid,
  bool recovering
) {
  if (input->mark == 0) {
    return false;
  }
  scanner->emission = (struct LexicalEmission){
    .symbol = symbol,
    .remaining = input->mark,
    .active = true,
  };
  enum TokenType whole = WHOLE_SOURCES[symbol];
  if (whole != 0 && valid[whole]) {
    bool fragmented = false;
    for (size_t index = 0; index < input->cursor.removal_count; index += 1) {
      const struct SourceRemoval *removal = &input->cursor.removals[index];
      if (
        removal->kind == SOURCE_REMOVED_CONTINUATION &&
        removal->slash >= 0 &&
        (size_t)removal->slash + input->base < input->mark
      ) {
        fragmented = true;
        break;
      }
    }
    if (!fragmented || recovering) scanner->emission.symbol = whole;
  }
  symbol = word_initial_source_symbol(symbol);
  if (
    symbol == ASSIGNMENT_LITERAL_BEGIN ||
    symbol == ASSIGNMENT_FALLBACK_LITERAL_BEGIN
  ) {
    for (size_t index = 0; index < input->cursor.logical_count; index += 1) {
      const struct SourceCharacter *character = &input->cursor.logical[index];
      if (
        character->origin.end > 0 &&
        (size_t)character->origin.end + input->base <= input->mark
      ) {
        scanner->emission.ends_assignment_colon = character->value == ':';
      }
    }
  }
  if (symbol == COMMENT_START) {
    if (!source_context_push(scanner, COMMENT_START)) {
      return false;
    }
    scanner->source.stages[scanner->source.stage_count - 1].disabled = true;
  }
  return true;
}

static bool emission_checkpoint(
  struct Scanner *scanner,
  const struct SourceCursor *cursor,
  size_t cut
) {
  struct SourceSnapshot snapshot = {0};
  if (!source_snapshot_at(cursor, cut, &snapshot)) {
    return false;
  }
  source_snapshot_clear(&scanner->source);
  scanner->source = snapshot;
  return true;
}

static bool
removal_has_slash(const struct SourceCursor *cursor, int64_t slash) {
  for (size_t index = 0; index < cursor->removal_count; index += 1) {
    const struct SourceRemoval *removal = &cursor->removals[index];
    if (
      removal->kind == SOURCE_REMOVED_CONTINUATION && removal->slash == slash
    ) {
      return true;
    }
  }
  return false;
}

static bool
removal_has_newline(const struct SourceCursor *cursor, int64_t newline) {
  for (size_t index = 0; index < cursor->removal_count; index += 1) {
    const struct SourceRemoval *removal = &cursor->removals[index];
    if (
      removal->kind == SOURCE_REMOVED_CONTINUATION &&
      removal->origin.character == newline
    ) {
      return true;
    }
  }
  return false;
}

struct EmissionStep {
  enum TokenType symbol;
  size_t cut;
  bool accepted;
};

static struct EmissionStep emit_whole_source(
  TSLexer *lexer,
  struct SourceCursor *cursor,
  enum TokenType symbol,
  size_t remaining
) {
  struct EmissionStep step = {.symbol = symbol, .accepted = true};
  while (step.accepted && step.cut < remaining) {
    step.accepted = !lexer->eof(lexer) && source_feed(cursor, lexer->lookahead);
    if (step.accepted) {
      lexer->advance(lexer, false);
      step.cut += 1;
    }
  }
  lexer->mark_end(lexer);
  return step;
}

static struct EmissionStep
emit_removed_newline(TSLexer *lexer, struct SourceCursor *cursor) {
  struct EmissionStep step = {
    .symbol = REMOVED_NEWLINE,
    .cut = 1,
    .accepted = source_feed(cursor, '\n') && removal_has_newline(cursor, 0),
  };
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  return step;
}

static struct EmissionStep emit_removed_run(
  TSLexer *lexer,
  struct SourceCursor *cursor,
  size_t remaining
) {
  struct EmissionStep step = {.symbol = REMOVED_SOURCE, .accepted = true};
  do {
    step.accepted = source_feed(cursor, 0);
    lexer->advance(lexer, false);
    step.cut += 1;
  } while (
    step.accepted &&
    step.cut < remaining &&
    lexer->lookahead == 0 &&
    !lexer->eof(lexer)
  );
  lexer->mark_end(lexer);
  return step;
}

static struct EmissionStep emit_single_character(
  TSLexer *lexer,
  struct SourceCursor *cursor,
  enum TokenType symbol
) {
  struct EmissionStep step = {
    .symbol = symbol,
    .cut = 1,
    .accepted = !lexer->eof(lexer) && source_feed(cursor, lexer->lookahead),
  };
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  return step;
}

static struct EmissionStep emit_backslash(
  TSLexer *lexer,
  struct SourceCursor *cursor,
  enum TokenType symbol,
  bool *removed_newline
) {
  struct EmissionStep step = {
    .symbol = symbol,
    .cut = 1,
    .accepted = source_feed(cursor, '\\'),
  };
  lexer->advance(lexer, false);
  lexer->mark_end(lexer);
  while (step.accepted && lexer->lookahead == 0 && !lexer->eof(lexer)) {
    step.accepted = source_feed(cursor, 0);
    lexer->advance(lexer, false);
  }
  if (step.accepted && lexer->lookahead == '\n') {
    step.accepted = source_feed(cursor, '\n');
    lexer->advance(lexer, false);
    if (step.accepted && removal_has_slash(cursor, 0)) {
      step.symbol = LINE_CONTINUATION;
      *removed_newline = true;
    }
  }
  return step;
}

static struct EmissionStep emit_source_run(
  TSLexer *lexer,
  struct SourceCursor *cursor,
  enum TokenType symbol,
  size_t remaining,
  bool punctuation
) {
  struct EmissionStep step = {.symbol = symbol, .accepted = true};
  do {
    if (lexer->eof(lexer) || !source_feed(cursor, lexer->lookahead)) {
      step.accepted = false;
      break;
    }
    bool removed_newline = lexer->lookahead == '\n' &&
      removal_has_newline(cursor, (int64_t)step.cut);
    lexer->advance(lexer, false);
    step.cut += 1;
    lexer->mark_end(lexer);
    if (removed_newline) {
      step.symbol = REMOVED_NEWLINE;
      break;
    }
  } while (
    step.cut < remaining &&
    lexer->lookahead != '\\' &&
    lexer->lookahead != '\n' &&
    !punctuation
  );
  return step;
}

static bool
finish_lexical_emission(struct Scanner *scanner, enum TokenType completed) {
  struct LexicalEmission *emission = &scanner->emission;
  bool punctuation =
    LEXICAL_SOURCES[emission->symbol].kind == LEXICAL_SOURCE_PHYSICAL;
  if (punctuation) {
    scanner->assignment_tilde_allowed = completed == PUNCT_EQUALS;
  } else if (completed != SOURCE_BEGIN) {
    scanner->assignment_tilde_allowed = emission->ends_assignment_colon;
  }
  if (completed == HERE_DOCUMENT_END_LINE_END) {
    scanner->here_document_end_line_consumed = true;
  }
  bool opens_fallback = is_bracket_fallback_context(completed);
  if (emission->tracks_brackets && !opens_fallback) {
    if (!in_bracket_fallback(scanner)) {
      return false;
    }
    top_context(scanner)->bracket_chains = emission->bracket_chains;
  }
  uint16_t chains = emission->bracket_chains;
  *emission = (struct LexicalEmission){0};
  if (!source_context_commit(scanner, completed)) {
    return false;
  }
  if (opens_fallback) {
    top_context(scanner)->bracket_chains = chains;
  }
  return true;
}

static bool emit_physical_source(
  struct Scanner *scanner,
  TSLexer *lexer,
  const bool *valid
) {
  struct LexicalEmission *emission = &scanner->emission;
  if (
    emission->symbol >= TOKEN_COUNT ||
    (emission->symbol != SOURCE_BEGIN &&
      LEXICAL_SOURCES[emission->symbol].kind == LEXICAL_SOURCE_NONE)
  ) {
    return false;
  }
  const struct LexicalSource *source = &LEXICAL_SOURCES[emission->symbol];
  if (emission->removed_newline) {
    if (
      lexer->eof(lexer) ||
      (lexer->lookahead != '\n' && lexer->lookahead != 0) ||
      !valid[REMOVED_NEWLINE]
    ) {
      return false;
    }
  } else if (emission->remaining == 0) {
    return false;
  }

  struct SourceCursor cursor;
  if (!source_cursor_resume(&cursor, &scanner->source, NULL, NULL)) {
    return false;
  }
  bool punctuation = source->kind == LEXICAL_SOURCE_PHYSICAL;
  enum TokenType symbol = emission->symbol == SOURCE_BEGIN ? REMOVED_SOURCE
    : punctuation                                          ? source->prefix
                                                           : source->piece;
  struct EmissionStep step;
  if (source->kind == LEXICAL_SOURCE_WHOLE) {
    step = emit_whole_source(lexer, &cursor, symbol, emission->remaining);
  } else if (lexer->lookahead == 0 && !lexer->eof(lexer)) {
    step = emit_removed_run(lexer, &cursor, emission->remaining);
  } else if (emission->removed_newline) {
    step = emit_removed_newline(lexer, &cursor);
    emission->removed_newline = false;
  } else if (punctuation && emission->remaining == 1) {
    step = emit_single_character(lexer, &cursor, source->piece);
  } else if (lexer->lookahead == '\\') {
    step = emit_backslash(lexer, &cursor, symbol, &emission->removed_newline);
  } else {
    step =
      emit_source_run(lexer, &cursor, symbol, emission->remaining, punctuation);
  }
  bool accepted = step.accepted &&
    step.cut <= emission->remaining &&
    valid[step.symbol] &&
    emission_checkpoint(scanner, &cursor, step.cut);
  source_cursor_clear(&cursor);
  if (!accepted) {
    return false;
  }
  emission->remaining -= step.cut;
  lexer->result_symbol = (TSSymbol)step.symbol;
  return emission->remaining > 0 ||
    finish_lexical_emission(
      scanner,
      word_initial_source_symbol(emission->symbol)
    );
}

struct LexicalOperator {
  const char *spelling;
  enum TokenType symbol;
};

static const struct LexicalOperator LEXICAL_OPERATORS[] = {
  {"&&", AND_IF_BEGIN},
  {"||", OR_IF_BEGIN},
  {";;", DSEMI_BEGIN},
  {"<&", LESSAND_BEGIN},
  {">&", GREATAND_BEGIN},
  {">>", DGREAT_BEGIN},
  {"<>", LESSGREAT_BEGIN},
  {">|", CLOBBER_BEGIN},
  {"<<", DLESS_OPERATOR_BEGIN},
  {"<<-", DLESSDASH_OPERATOR_BEGIN},
  {":-", PARAMETER_VALUE_OPERATOR_BEGIN},
  {":=", PARAMETER_VALUE_OPERATOR_BEGIN},
  {":?", PARAMETER_VALUE_OPERATOR_BEGIN},
  {":+", PARAMETER_VALUE_OPERATOR_BEGIN},
  {"=", PARAMETER_VALUE_OPERATOR_BEGIN},
  {"+", PARAMETER_VALUE_OPERATOR_BEGIN},
  {"%%", PARAMETER_PATTERN_OPERATOR_BEGIN},
  {"%", PARAMETER_PATTERN_OPERATOR_BEGIN},
  {"+", ARITHMETIC_UNARY_OPERATOR_BEGIN},
  {"-", ARITHMETIC_UNARY_OPERATOR_BEGIN},
  {"!", ARITHMETIC_UNARY_OPERATOR_BEGIN},
  {"~", ARITHMETIC_UNARY_OPERATOR_BEGIN},
  {"<<=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {">>=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"*=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"/=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"%=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"+=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"-=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"&=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"^=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"|=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"=", ARITHMETIC_ASSIGNMENT_OPERATOR_BEGIN},
  {"?", ARITHMETIC_QUESTION_OPERATOR_BEGIN},
  {":", ARITHMETIC_COLON_OPERATOR_BEGIN},
  {"||", ARITHMETIC_LOGICAL_OR_OPERATOR_BEGIN},
  {"&&", ARITHMETIC_LOGICAL_AND_OPERATOR_BEGIN},
  {"|", ARITHMETIC_BITWISE_OR_OPERATOR_BEGIN},
  {"^", ARITHMETIC_BITWISE_XOR_OPERATOR_BEGIN},
  {"&", ARITHMETIC_BITWISE_AND_OPERATOR_BEGIN},
  {"==", ARITHMETIC_EQUALITY_OPERATOR_BEGIN},
  {"!=", ARITHMETIC_EQUALITY_OPERATOR_BEGIN},
  {"<=", ARITHMETIC_RELATIONAL_OPERATOR_BEGIN},
  {">=", ARITHMETIC_RELATIONAL_OPERATOR_BEGIN},
  {"<", ARITHMETIC_RELATIONAL_OPERATOR_BEGIN},
  {">", ARITHMETIC_RELATIONAL_OPERATOR_BEGIN},
  {"<<", ARITHMETIC_SHIFT_OPERATOR_BEGIN},
  {">>", ARITHMETIC_SHIFT_OPERATOR_BEGIN},
  {"+", ARITHMETIC_ADDITIVE_OPERATOR_BEGIN},
  {"-", ARITHMETIC_ADDITIVE_OPERATOR_BEGIN},
  {"*", ARITHMETIC_MULTIPLICATIVE_OPERATOR_BEGIN},
  {"/", ARITHMETIC_MULTIPLICATIVE_OPERATOR_BEGIN},
  {"%", ARITHMETIC_MULTIPLICATIVE_OPERATOR_BEGIN},
};

enum { LEXICAL_OPERATOR_COUNT = COUNT_OF(LEXICAL_OPERATORS) };

static bool lexical_operator(TSLexer *lexer, const bool *valid) {
  bool candidates[LEXICAL_OPERATOR_COUNT];
  for (size_t index = 0; index < LEXICAL_OPERATOR_COUNT; index += 1) {
    candidates[index] = valid[LEXICAL_OPERATORS[index].symbol];
  }
  enum TokenType symbol = TOKEN_COUNT;
  for (size_t length = 0;; length += 1) {
    bool matched = false;
    for (size_t index = 0; index < LEXICAL_OPERATOR_COUNT; index += 1) {
      const struct LexicalOperator *candidate = &LEXICAL_OPERATORS[index];
      if (candidates[index]) {
        candidates[index] = candidate->spelling[length] != '\0' &&
          candidate->spelling[length] == lexer->lookahead;
        matched = matched || candidates[index];
      }
    }
    if (!matched || lexer_at_eof(lexer)) {
      break;
    }
    lexer->advance(lexer, false);
    for (size_t index = 0; index < LEXICAL_OPERATOR_COUNT; index += 1) {
      if (
        candidates[index] &&
        LEXICAL_OPERATORS[index].spelling[length + 1] == '\0'
      ) {
        symbol = LEXICAL_OPERATORS[index].symbol;
        lexer->mark_end(lexer);
        break;
      }
    }
  }
  if (symbol == TOKEN_COUNT) {
    return false;
  }
  lexer->result_symbol = (TSSymbol)symbol;
  return true;
}

static bool lexical_name(TSLexer *lexer, enum TokenType symbol) {
  if (!is_name_start_character(lexer->lookahead)) {
    return false;
  }
  do {
    lexer->advance(lexer, false);
  } while (is_name_character(lexer->lookahead));
  if (symbol == NAME_BEGIN && !is_token_delimiter(lexer)) {
    return false;
  }
  return emit_symbol(lexer, symbol);
}

static bool lexical_arithmetic_word(TSLexer *lexer, const bool *valid) {
  if (!is_name_character(lexer->lookahead)) {
    return false;
  }
  enum TokenType symbol = scan_arithmetic_word(lexer);
  if (!valid[symbol]) {
    return false;
  }
  return emit_symbol(lexer, symbol);
}

static bool lexical_arithmetic_fragment(TSLexer *lexer) {
  int32_t character = lexer->lookahead;
  if (
    lexer_at_eof(lexer) ||
    arithmetic_whitespace(character) ||
    character == '(' ||
    character == '`' ||
    character == SOURCE_BACKQUOTE_BOUNDARY
  ) {
    return false;
  }
  lexer->advance(lexer, false);
  if (character == ')' && lexer->lookahead == ')') {
    return false;
  }
  if (
    character == '$' &&
    (is_parameter_start_character(lexer->lookahead) ||
      lexer->lookahead == '(' ||
      lexer->lookahead == '{')
  ) {
    return false;
  }
  if (character == '\\' && !lexer_at_eof(lexer)) {
    lexer->advance(lexer, false);
  }
  return emit_symbol(lexer, ARITHMETIC_SOURCE_FRAGMENT_BEGIN);
}

static bool
quoted_escape_character(int32_t character, bool double_quoted, bool parameter) {
  return character == '\\' ||
    character == '$' ||
    character == '`' ||
    (double_quoted && character == '"') ||
    (parameter && character == '}');
}

static bool lexical_escape(TSLexer *lexer, enum TokenType symbol) {
  if (lexer->lookahead != '\\') {
    return false;
  }
  lexer->advance(lexer, false);
  if (lexer_at_eof(lexer) && symbol == ESCAPED_CHARACTER_BEGIN) {
    return emit_symbol(lexer, symbol);
  }
  int32_t character = lexer->lookahead;
  if (
    lexer_at_eof(lexer) ||
    character == '\n' ||
    character == SOURCE_BACKQUOTE_BOUNDARY
  ) {
    return false;
  }
  bool parameter = symbol == DOUBLE_QUOTED_PARAMETER_ESCAPE_BEGIN;
  bool quoted = symbol == DOUBLE_QUOTE_ESCAPE_BEGIN || parameter;
  if (
    symbol != ESCAPED_CHARACTER_BEGIN &&
    !quoted_escape_character(character, quoted, parameter)
  ) {
    return false;
  }
  lexer->advance(lexer, false);
  return emit_symbol(lexer, symbol);
}

static bool lexical_text(struct LogicalLexer *input, enum TokenType symbol) {
  TSLexer *lexer = &input->lexer;
  bool consumed = false;
  bool parameter = symbol == DOUBLE_QUOTED_PARAMETER_TEXT_BEGIN;
  bool quoted = symbol == DOUBLE_QUOTE_TEXT_BEGIN || parameter;
  while (!lexer_at_eof(lexer)) {
    int32_t character = lexer->lookahead;
    if (character == SOURCE_BACKQUOTE_BOUNDARY) {
      break;
    }
    if (symbol == SINGLE_QUOTE_CONTENT_BEGIN) {
      if (character == '\'') {
        break;
      }
    } else if (symbol == DOLLAR_SINGLE_QUOTE_TEXT_BEGIN) {
      if (character == '\'' || character == '\\') {
        break;
      }
    } else if (symbol == QUOTED_HERE_DOCUMENT_TEXT) {
      if (character == '\n') {
        break;
      }
    } else {
      if (
        (quoted && character == '"') ||
        character == '`' ||
        (parameter && character == '}') ||
        (symbol == HERE_DOCUMENT_TEXT_BEGIN && character == '\n')
      ) {
        break;
      }
      if (character == '$') {
        int32_t next = logical_peek(input);
        if (is_parameter_start_character(next) || next == '{' || next == '(') {
          break;
        }
      }
      if (
        character == '\\' &&
        quoted_escape_character(logical_peek(input), quoted, parameter)
      ) {
        break;
      }
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    consumed = true;
  }
  lexer->result_symbol = (TSSymbol)symbol;
  return consumed;
}

struct LexicalPunctuation {
  int32_t character;
  enum TokenType symbol;
};

static const struct LexicalPunctuation LEXICAL_PUNCTUATION[] = {
  {'\'', SQ_CLOSE},
  {'\'', DOLLAR_SQ_CLOSE},
  {'"', DQ_CLOSE},
  {'\'', SQ_OPEN},
  {'\'', DOLLAR_SQ_OPEN},
  {'"', DQ_OPEN},
  {'{', PARAMETER_OPEN},
  {'}', PARAMETER_CLOSE},
  {'(', COMMAND_OPEN},
  {'(', PUNCT_LEFT_PARENTHESIS},
  {')', PUNCT_RIGHT_PARENTHESIS},
  {';', PUNCT_SEMICOLON},
  {'&', PUNCT_AMPERSAND},
  {'|', PUNCT_PIPE},
  {'<', PUNCT_LESS},
  {'>', PUNCT_GREATER},
  {'=', PUNCT_EQUALS},
  {']', PUNCT_RIGHT_BRACKET},
  {':', PUNCT_COLON},
  {'~', WORD_TILDE_START},
  {'~', ASSIGNMENT_TILDE_START},
  {'~', PARAMETER_TILDE_START},
  {'\n', LOGICAL_NEWLINE_BEGIN},
  {SOURCE_BACKQUOTE_BOUNDARY, BACKQUOTE_END},
  {'`', DOUBLE_QUOTED_BACKQUOTE_START},
  {'`', BACKQUOTE_START},
};

static bool lexical_punctuation(TSLexer *lexer, const bool *valid) {
  for (size_t index = 0; index < COUNT_OF(LEXICAL_PUNCTUATION); index += 1) {
    const struct LexicalPunctuation *punctuation = &LEXICAL_PUNCTUATION[index];
    if (
      lexer->lookahead == punctuation->character && valid[punctuation->symbol]
    ) {
      if (
        punctuation->symbol == BACKQUOTE_END &&
        !logical_backquote_end((struct LogicalLexer *)lexer)
      ) {
        return false;
      }
      lexer->advance(lexer, false);
      if (
        (punctuation->symbol == PUNCT_SEMICOLON && lexer->lookahead == ';') ||
        (punctuation->symbol == PUNCT_AMPERSAND && lexer->lookahead == '&')
      ) {
        return false;
      }
      return emit_symbol(lexer, punctuation->symbol);
    }
  }
  if (lexer->lookahead == '$' && valid[DOLLAR_SQ_DOLLAR]) {
    return accept_character(lexer, DOLLAR_SQ_DOLLAR) &&
      lexer->lookahead == '\'';
  }
  return false;
}

static enum TokenType parameter_hash_symbol(TSLexer *lexer) {
  lexer->advance(lexer, false);
  int32_t character = lexer->lookahead;
  if (is_name_start_character(character)) {
    do {
      lexer->advance(lexer, false);
    } while (is_name_character(lexer->lookahead));
  } else if (is_decimal_digit(character)) {
    do {
      lexer->advance(lexer, false);
    } while (is_decimal_digit(lexer->lookahead));
  } else if (is_special_parameter_character(character)) {
    lexer->advance(lexer, false);
  } else {
    if (
      character == '+' ||
      character == '=' ||
      character == '%' ||
      character == ':'
    ) {
      lexer->advance(lexer, false);
      if (lexer->lookahead == '}') {
        return TOKEN_COUNT;
      }
    }
    return SPECIAL_PARAMETER_HASH_BEGIN;
  }
  return lexer->lookahead == '}' ? PARAMETER_LENGTH_OPERATOR_BEGIN
                                 : SPECIAL_PARAMETER_HASH_BEGIN;
}

static bool lexical_attempt(struct LogicalLexer *input, bool accepted) {
  if (!accepted) {
    logical_rewind(input);
  }
  return accepted;
}

static bool lexical_classify(struct LogicalLexer *input, const bool *valid) {
  TSLexer *lexer = &input->lexer;
  if (
    lexical_attempt(input, lexical_operator(lexer, valid)) ||
    lexical_attempt(input, lexical_punctuation(lexer, valid))
  ) {
    return true;
  }
  static const enum TokenType names[] = {NAME_BEGIN, VARIABLE_NAME_BEGIN};
  for (size_t index = 0; index < COUNT_OF(names); index += 1) {
    if (
      valid[names[index]] &&
      lexical_attempt(input, lexical_name(lexer, names[index]))
    ) {
      return true;
    }
  }
  if (
    (valid[ARITHMETIC_NUMBER_BEGIN] ||
      valid[ARITHMETIC_VARIABLE_BEGIN] ||
      valid[ARITHMETIC_SOURCE_FRAGMENT_BEGIN]) &&
    lexical_attempt(input, lexical_arithmetic_word(lexer, valid))
  ) {
    return true;
  }
  if (
    valid[ARITHMETIC_SOURCE_FRAGMENT_BEGIN] &&
    lexical_attempt(input, lexical_arithmetic_fragment(lexer))
  ) {
    return true;
  }
  static const enum TokenType escapes[] = {
    ESCAPED_CHARACTER_BEGIN,
    DOUBLE_QUOTE_ESCAPE_BEGIN,
    DOUBLE_QUOTED_PARAMETER_ESCAPE_BEGIN,
    HERE_DOCUMENT_ESCAPE_BEGIN
  };
  for (size_t index = 0; index < COUNT_OF(escapes); index += 1) {
    if (
      valid[escapes[index]] &&
      lexical_attempt(input, lexical_escape(lexer, escapes[index]))
    ) {
      return true;
    }
  }
  if (is_horizontal_blank(lexer->lookahead) && valid[LOGICAL_BLANK_BEGIN]) {
    scan_horizontal_blanks(lexer);
    lexer->mark_end(lexer);
    // Following source must invalidate the preceding separator on edits.
    scan_separator_target(lexer);
    lexer->result_symbol = LOGICAL_BLANK_BEGIN;
    return true;
  }
  if (
    valid[ARITHMETIC_BLANK_BEGIN] &&
    lexer->lookahead != '\n' &&
    arithmetic_whitespace(lexer->lookahead)
  ) {
    do {
      lexer->advance(lexer, false);
    } while (
      lexer->lookahead != '\n' && arithmetic_whitespace(lexer->lookahead)
    );
    return emit_symbol(lexer, ARITHMETIC_BLANK_BEGIN);
  }
  if (lexer->lookahead == '#' && valid[PARAMETER_LENGTH_OPERATOR_BEGIN]) {
    enum TokenType symbol = parameter_hash_symbol(lexer);
    logical_rewind(input);
    if (symbol == TOKEN_COUNT) {
      return false;
    }
    if (symbol == PARAMETER_LENGTH_OPERATOR_BEGIN) {
      return accept_character(lexer, PARAMETER_LENGTH_OPERATOR_BEGIN);
    }
  }
  static const struct LexicalPunctuation characters[] = {
    {'*', PATTERN_STAR_BEGIN},
    {'?', PATTERN_QUESTION_BEGIN},
    {'!', PATTERN_NEGATION_BEGIN},
    {'^', PATTERN_NEGATION_BEGIN},
    {']', PATTERN_INITIAL_RIGHT_BRACKET_BEGIN},
    {'#', SPECIAL_PARAMETER_HASH_BEGIN},
    {'-', PARAMETER_HYPHEN_BEGIN},
    {'?', PARAMETER_QUESTION_BEGIN},
  };
  for (size_t index = 0; index < COUNT_OF(characters); index += 1) {
    if (
      valid[characters[index].symbol] &&
      lexer->lookahead == characters[index].character
    ) {
      return accept_character(lexer, characters[index].symbol);
    }
  }
  bool special = valid[SPECIAL_PARAMETER_BEGIN] &&
    is_special_parameter_character(lexer->lookahead);
  if (
    special ||
    (valid[UNBRACED_POSITIONAL_PARAMETER_BEGIN] &&
      lexer->lookahead >= '1' &&
      lexer->lookahead <= '9')
  ) {
    return accept_character(
      lexer,
      special ? SPECIAL_PARAMETER_BEGIN : UNBRACED_POSITIONAL_PARAMETER_BEGIN
    );
  }
  static const enum TokenType texts[] = {
    SINGLE_QUOTE_CONTENT_BEGIN,
    DOUBLE_QUOTE_TEXT_BEGIN,
    DOUBLE_QUOTED_PARAMETER_TEXT_BEGIN,
    DOLLAR_SINGLE_QUOTE_TEXT_BEGIN,
    QUOTED_HERE_DOCUMENT_TEXT,
    HERE_DOCUMENT_TEXT_BEGIN
  };
  for (size_t index = 0; index < COUNT_OF(texts); index += 1) {
    if (valid[texts[index]] && lexical_text(input, texts[index])) {
      return true;
    }
  }
  return false;
}

static bool pattern_structured_start(struct LogicalLexer *input) {
  int32_t character = input->lexer.lookahead;
  if (character == '\\') {
    return logical_peek(input) != SOURCE_BACKQUOTE_BOUNDARY;
  }
  if (character != '$') {
    return character == '\'' || character == '"' || character == '`';
  }
  int32_t follower = logical_peek(input);
  return is_parameter_start_character(follower) ||
    follower == '(' ||
    follower == '{' ||
    follower == '\'';
}

static bool is_tilde_context(enum TokenType opener) {
  return opener == WORD_TILDE_START ||
    opener == ASSIGNMENT_TILDE_START ||
    opener == PARAMETER_TILDE_START;
}

static enum TokenType pattern_context(const struct Scanner *scanner) {
  size_t count =
    scanner->context_count - (in_bracket_fallback(scanner) ? 1 : 0);
  return count == 0 ? TOKEN_COUNT : scanner->contexts[count - 1].opener;
}

static bool lexical_pattern_probe(
  const struct Scanner *scanner,
  struct LogicalLexer *input,
  enum TokenType literal_symbol,
  uint16_t *chains,
  bool *complete
) {
  struct LogicalPosition position = logical_position(input);
  bool parameter = literal_symbol == PARAMETER_LITERAL_BEGIN;
  bool assignment = literal_symbol == ASSIGNMENT_LITERAL_BEGIN;
  enum PatternMode mode = parameter ? PATTERN_PARAMETER
    : assignment                    ? PATTERN_ASSIGNMENT
                                    : PATTERN_WORD;
  enum TokenType context = pattern_context(scanner);
  if (context == ASSIGNMENT_TILDE_START) {
    mode = PATTERN_ASSIGNMENT_TILDE;
  } else if (context == WORD_TILDE_START) {
    mode = PATTERN_WORD_TILDE;
  } else if (context == PARAMETER_TILDE_START) {
    mode = PATTERN_PARAMETER_TILDE;
  }
  struct LookaheadLexer lookahead = {0};
  bool accepted = lookahead_init(&lookahead, &input->lexer) &&
    lookahead_append(&lookahead, 0) &&
    scan_pattern_bracket(scanner, &lookahead.lexer, mode, chains, complete) &&
    !lookahead_is_pending(&lookahead.lexer);
  lookahead_clear(&lookahead);
  logical_restore_position(input, &position);
  return accepted;
}

static bool lexical_pattern_content(
  struct LogicalLexer *input,
  enum TokenType symbol,
  const bool *valid
) {
  TSLexer *lexer = &input->lexer;
  bool parameter = symbol == PARAMETER_PATTERN_CLASS_CONTENT_BEGIN;
  int32_t marker = ':';
  enum TokenType closer = PATTERN_CHARACTER_CLASS_END_COLON;
  bool has_content = valid[closer];
  bool consumed = false;
  while (
    !pattern_boundary(lexer, parameter) && !pattern_structured_start(input)
  ) {
    int32_t character = lexer->lookahead;
    if (
      (has_content && character == marker && logical_peek(input) == ']') ||
      character == ']'
    ) {
      break;
    }
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    consumed = true;
    has_content = true;
  }
  lexer->result_symbol = (TSSymbol)symbol;
  return consumed;
}

static bool lexical_pattern_literal(
  const struct Scanner *scanner,
  struct LogicalLexer *input,
  enum TokenType symbol,
  const bool *valid
) {
  TSLexer *lexer = &input->lexer;
  bool parameter = symbol == PARAMETER_LITERAL_BEGIN;
  bool assignment = symbol == ASSIGNMENT_LITERAL_BEGIN;
  enum TokenType fallback_symbol = parameter ? PARAMETER_FALLBACK_LITERAL_BEGIN
    : assignment                             ? ASSIGNMENT_FALLBACK_LITERAL_BEGIN
                                             : WORD_FALLBACK_LITERAL_BEGIN;
  bool tilde = is_tilde_context(pattern_context(scanner));
  bool consumed = false;
  uint16_t chains = fallback_chains(scanner);
  bool enters_fallback = false;
  while (
    !pattern_boundary(lexer, parameter) && !pattern_structured_start(input)
  ) {
    int32_t character = lexer->lookahead;
    if (
      character == '*' ||
      character == '?' ||
      (tilde && (character == '/' || (assignment && character == ':')))
    ) {
      break;
    }
    if (character == '[') {
      uint16_t after = chains;
      bool complete;
      if (
        !lexical_pattern_probe(scanner, input, symbol, &after, &complete) ||
        complete
      ) {
        break;
      }
      enters_fallback = !in_bracket_fallback(scanner) && valid[fallback_symbol];
    }
    chains = bracket_chains_after_literal(chains, character);
    bool tilde_prefix =
      assignment && character == ':' && logical_peek(input) == '~';
    lexer->advance(lexer, false);
    lexer->mark_end(lexer);
    consumed = true;
    if (tilde_prefix) {
      break;
    }
  }
  lexer->result_symbol = (TSSymbol)(enters_fallback ? fallback_symbol : symbol);
  return consumed;
}

static bool lexical_pattern_classify(
  const struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  TSLexer *lexer = &input->lexer;
  if (lexer->lookahead == '#' && valid[SPECIAL_PARAMETER_HASH_BEGIN]) {
    return false;
  }
  enum TokenType context = top_context_opener(scanner);
  if (is_bracket_fallback_context(context)) {
    enum TokenType parent = pattern_context(scanner);
    bool boundary =
      pattern_boundary(lexer, context == PARAMETER_FALLBACK_LITERAL_BEGIN) ||
      (context == ASSIGNMENT_FALLBACK_LITERAL_BEGIN &&
        lexer->lookahead == '~' &&
        scanner->assignment_tilde_allowed) ||
      (is_tilde_context(parent) &&
        (lexer->lookahead == '/' ||
          (parent == ASSIGNMENT_TILDE_START && lexer->lookahead == ':')));
    if (boundary) {
      static const enum TokenType ends[] = {
        WORD_BRACKET_FALLBACK_END,
        ASSIGNMENT_BRACKET_FALLBACK_END,
        PARAMETER_BRACKET_FALLBACK_END
      };
      for (size_t index = 0; index < COUNT_OF(ends); index += 1) {
        if (valid[ends[index]]) {
          return emit_symbol(lexer, ends[index]);
        }
      }
      return false;
    }
  }
  static const enum TokenType contents[] = {
    PARAMETER_PATTERN_CLASS_CONTENT_BEGIN,
    PATTERN_CLASS_CONTENT_BEGIN,
  };
  for (size_t index = 0; index < COUNT_OF(contents); index += 1) {
    if (
      valid[contents[index]] &&
      lexical_pattern_content(input, contents[index], valid)
    ) {
      return true;
    }
  }
  if (
    lexer->lookahead == '[' &&
    (valid[WORD_PATTERN_BRACKET_OPEN] || valid[PARAMETER_PATTERN_BRACKET_OPEN])
  ) {
    enum TokenType literal = valid[PARAMETER_PATTERN_BRACKET_OPEN]
      ? PARAMETER_LITERAL_BEGIN
      : valid[ASSIGNMENT_LITERAL_BEGIN] ? ASSIGNMENT_LITERAL_BEGIN
                                        : LITERAL_BEGIN;
    uint16_t chains = fallback_chains(scanner);
    bool complete;
    if (!lexical_pattern_probe(scanner, input, literal, &chains, &complete)) {
      return false;
    }
    if (complete) {
      return accept_character(
        lexer,
        valid[PARAMETER_PATTERN_BRACKET_OPEN] ? PARAMETER_PATTERN_BRACKET_OPEN
                                              : WORD_PATTERN_BRACKET_OPEN
      );
    }
  }
  enum TokenType member = valid[PARAMETER_PATTERN_BRACKET_CHARACTER]
    ? PARAMETER_PATTERN_BRACKET_CHARACTER
    : PATTERN_BRACKET_CHARACTER;
  bool parameter = member == PARAMETER_PATTERN_BRACKET_CHARACTER;
  int32_t character = lexer->lookahead;
  if (valid[member]) {
    if (
      pattern_boundary(lexer, parameter) ||
      pattern_structured_start(input) ||
      character == ']' ||
      character == '-' ||
      ((character == '!' || character == '^') && valid[PATTERN_NEGATION_BEGIN])
    ) {
      return false;
    }
    if (
      character == '[' &&
      valid[PATTERN_SPECIAL_LEFT_BRACKET] &&
      logical_peek(input) == ':' &&
      class_terminator_follows(lexer, parameter)
    ) {
      return false;
    }
    return accept_character(lexer, member);
  }
  if (
    character == '~' &&
    (valid[WORD_TILDE_START] ||
      valid[ASSIGNMENT_TILDE_START] ||
      valid[PARAMETER_TILDE_START])
  ) {
    return false;
  }
  static const enum TokenType literals[] = {
    ASSIGNMENT_LITERAL_BEGIN,
    PARAMETER_LITERAL_BEGIN,
    LITERAL_BEGIN,
  };
  for (size_t index = 0; index < COUNT_OF(literals); index += 1) {
    if (
      valid[literals[index]] &&
      lexical_pattern_literal(scanner, input, literals[index], valid)
    ) {
      return true;
    }
  }
  return false;
}

static bool scanner_copy(const struct Scanner *scanner, struct Scanner *copy) {
  char state[SCANNER_STATE_CAPACITY];
  struct StateWriter writer = {.data = state, .capacity = sizeof(state)};
  *copy = (struct Scanner){0};
  if (
    !serialize_scanner_state(scanner, &writer) ||
    !deserialize_scanner_state(copy, state, writer.length)
  ) {
    clear_scanner(copy);
    return false;
  }
  return true;
}

static bool classify_word_start(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
);

static bool scan_tilde_end(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  enum TokenType context = top_context_opener(scanner);
  if (!is_tilde_context(context)) {
    return false;
  }
  enum TokenType end =
    context == ASSIGNMENT_TILDE_START ? ASSIGNMENT_TILDE_END : WORD_TILDE_END;
  int32_t character = input->lexer.lookahead;
  if (
    !valid[end] ||
    !(character == '/' ||
      (context == ASSIGNMENT_TILDE_START && character == ':') ||
      pattern_boundary(&input->lexer, context == PARAMETER_TILDE_START))
  ) {
    return false;
  }
  input->lexer.mark_end(&input->lexer);
  input->lexer.result_symbol = (TSSymbol)end;
  return source_context_pop(scanner);
}

static bool classify_backquote_remainder(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  logical_rewind(input);
  return scan_with_lookahead(
    scanner,
    &input->lexer,
    valid,
    scan_backquote_remainder
  );
}

static bool classify_here_document_end_text(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  struct HereDocument *document = &scanner->active_frame.documents[0];
  enum TokenType symbol = document->quoted ? QUOTED_HERE_DOCUMENT_END_TEXT
                                           : HERE_DOCUMENT_END_TEXT_BEGIN;
  const struct SourceContext *context = top_context(scanner);
  if (
    !valid[symbol] ||
    context == NULL ||
    !is_here_document_context(context->opener)
  )
    return false;
  size_t body = context->stage_count;
  struct SourceStage stages[2] = {
    source_stage(SOURCE_HERE_DOCUMENT),
    source_stage(SOURCE_REMOVE_CONTINUATIONS),
  };
  stages[0].disabled = true;
  stages[1].disabled = true;
  if (!logical_replace_view(input, body, stages, 2)) return false;
  size_t offset = 0;
  while (
    offset < document->delimiter_length && input->result == SOURCE_CHARACTER
  ) {
    if (!match_here_document_delimiter_character(
          document,
          &offset,
          input->lexer.lookahead
        ))
      return false;
    input->lexer.advance(&input->lexer, false);
  }
  if (offset != document->delimiter_length) return false;
  scanner->source.stages[body].disabled = true;
  document->end_text_consumed = true;
  return emit_symbol(&input->lexer, symbol);
}

static bool classify_logical_source(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  if (
    scanner->active_frame.count > 0 &&
    scanner->active_frame.documents[0].ending &&
    !scanner->active_frame.documents[0].end_text_consumed &&
    scanner->active_frame.documents[0].delimiter_length > 0
  ) {
    return classify_here_document_end_text(scanner, input, valid);
  }
  if (valid[HERE_DOCUMENT_END_LINE_END] && input->lexer.lookahead == '\n') {
    input->lexer.advance(&input->lexer, false);
    return emit_symbol(&input->lexer, HERE_DOCUMENT_END_LINE_END);
  }
  if (scan_tilde_end(scanner, input, valid)) {
    return true;
  }
  if (
    is_backquote_context(top_context_opener(scanner)) &&
    pending_backquote_context(scanner) != SIZE_MAX
  ) {
    if (classify_backquote_remainder(scanner, input, valid)) return true;
    logical_rewind(input);
  }
  if (
    (valid[BACKQUOTE_COMPOUND_REMAINDER_BEGIN] ||
      valid[BACKQUOTE_REMAINDER_TAIL_BEGIN]) &&
    (!word_source_can_continue(valid) || is_token_delimiter(&input->lexer)) &&
    classify_backquote_remainder(scanner, input, valid)
  ) {
    return true;
  }
  logical_rewind(input);
  struct Scanner control;
  if (!scanner_copy(scanner, &control)) {
    return false;
  }
  bool control_recognized =
    scan_with_lookahead(&control, &input->lexer, valid, scan_dispatch);
  if (control_recognized) {
    if (
      input->lexer.result_symbol >= TOKEN_COUNT ||
      !valid[input->lexer.result_symbol]
    ) {
      clear_scanner(&control);
      return classify_backquote_remainder(scanner, input, valid);
    }
    clear_scanner(scanner);
    *scanner = control;
    return true;
  }
  clear_scanner(&control);
  logical_rewind(input);
  if (
    !(valid[WORD_TILDE_START] && input->lexer.lookahead == '~') &&
    lexical_attempt(input, classify_word_start(scanner, input, valid))
  ) {
    return true;
  }
  if (input->lexer.lookahead == '\n') {
    enum TokenType symbol =
      valid[HERE_DOCUMENT_LINE_END] && has_startable_pending_document(scanner)
      ? HERE_DOCUMENT_LINE_END
      : valid[COMMENT_LINE_END] ? COMMENT_LINE_END
      : valid[NEWLINE]          ? NEWLINE
                                : TOKEN_COUNT;
    if (symbol != TOKEN_COUNT) {
      input->lexer.advance(&input->lexer, false);
      return emit_symbol(&input->lexer, symbol);
    }
  }
  if (valid[COMMENT_START] && input->lexer.lookahead == '#') {
    if (!advance_to_comment_end(&input->lexer, NULL)) {
      return false;
    }
    return emit_symbol(&input->lexer, COMMENT_START);
  }
  bool lexical_valid[TOKEN_COUNT];
  memcpy(lexical_valid, valid, sizeof(lexical_valid));
  lexical_valid[ASSIGNMENT_TILDE_START] =
    valid[ASSIGNMENT_TILDE_START] && scanner->assignment_tilde_allowed;
  return lexical_attempt(
           input,
           lexical_pattern_classify(scanner, input, lexical_valid)
         ) ||
    lexical_classify(input, lexical_valid) ||
    classify_backquote_remainder(scanner, input, valid);
}

static bool classify_word_start(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  bool word_valid[TOKEN_COUNT] = {false};
  bool available = false;
  for (size_t index = 0; index < COUNT_OF(WORD_INITIAL_SYMBOLS); index += 1) {
    word_valid[WORD_INITIAL_SYMBOLS[index]] =
      valid[WORD_INITIAL_LITERAL_BEGIN + index];
    available = available || valid[WORD_INITIAL_LITERAL_BEGIN + index];
  }
  if (
    !available ||
    is_token_delimiter(&input->lexer) ||
    !is_word_element_start(&input->lexer)
  ) {
    return false;
  }
  if (is_decimal_digit(input->lexer.lookahead)) {
    if (advance_file_descriptor(&input->lexer)) {
      return false;
    }
    logical_rewind(input);
  }
  if (!classify_logical_source(scanner, input, word_valid)) {
    return false;
  }
  for (size_t index = 0; index < COUNT_OF(WORD_INITIAL_SYMBOLS); index += 1) {
    if (input->lexer.result_symbol == WORD_INITIAL_SYMBOLS[index]) {
      input->lexer.result_symbol =
        (TSSymbol)(WORD_INITIAL_LITERAL_BEGIN + index);
      return true;
    }
  }
  return false;
}

static bool recovery_word_context(const struct Scanner *scanner) {
  enum TokenType opener = top_context_opener(scanner);
  return opener == PARAMETER_OPEN ||
    opener == DQ_OPEN ||
    opener == SQ_OPEN ||
    opener == DOLLAR_SQ_OPEN;
}

static bool recovery_parameter_is_quoted(const struct Scanner *scanner) {
  for (size_t index = scanner->context_count; index > 0; index -= 1) {
    enum TokenType opener = scanner->contexts[index - 1].opener;
    if (opener != PARAMETER_OPEN) {
      return opener == DQ_OPEN ||
        opener == HERE_DOCUMENT_BODY_START ||
        is_arithmetic_context(opener);
    }
  }
  return false;
}

static void
select_recovery_symbols(const struct Scanner *scanner, bool *valid) {
  static const bool base_symbols[TOKEN_COUNT] = {
    [LEFT_BRACE] = true,
    [PIPELINE_NEGATION] = true,
    [IF_KEYWORD] = true,
    [FOR_KEYWORD] = true,
    [IN_KEYWORD] = true,
    [CASE_KEYWORD] = true,
    [WHILE_KEYWORD] = true,
    [UNTIL_KEYWORD] = true,
    [RIGHT_BRACE] = true,
    [THEN_KEYWORD] = true,
    [ELIF_KEYWORD] = true,
    [ELSE_KEYWORD] = true,
    [FI_KEYWORD] = true,
    [DO_KEYWORD] = true,
    [DONE_KEYWORD] = true,
    [ESAC_KEYWORD] = true,
    [BACKQUOTE_END] = true,
    [PUNCT_LEFT_PARENTHESIS] = true,
    [PUNCT_RIGHT_PARENTHESIS] = true,
    [PUNCT_SEMICOLON] = true,
    [PUNCT_AMPERSAND] = true,
    [PUNCT_PIPE] = true,
    [OR_IF_BEGIN] = true,
    [AND_IF_BEGIN] = true,
    [DSEMI_BEGIN] = true,
    [LOGICAL_NEWLINE_BEGIN] = true,
#define LEXICAL_RECOVERY_SYMBOL(begin, piece) [piece] = true,
    DASH_LEXICAL_SOURCE_TOKENS(LEXICAL_RECOVERY_SYMBOL)
#undef LEXICAL_RECOVERY_SYMBOL
#define PHYSICAL_RECOVERY_SYMBOL(begin, prefix, character) \
  [prefix] = true, [character] = true,
      DASH_PHYSICAL_SOURCE_TOKENS(PHYSICAL_RECOVERY_SYMBOL)
#undef PHYSICAL_RECOVERY_SYMBOL
#define WHOLE_RECOVERY_SYMBOL(begin, whole) [whole] = true,
        DASH_WHOLE_SOURCE_TOKENS(WHOLE_RECOVERY_SYMBOL)
#undef WHOLE_RECOVERY_SYMBOL
          [LINE_CONTINUATION] = true,
    [REMOVED_NEWLINE] = true,
    [SOURCE_BEGIN] = true,
    [REMOVED_SOURCE] = true,
    [HERE_DOCUMENT_END_BEGIN] = true,
    [QUOTED_HERE_DOCUMENT_END_BEGIN] = true,
    [WORD_INITIAL_LITERAL_BEGIN] = true,
    [WORD_INITIAL_FALLBACK_LITERAL_BEGIN] = true,
    [WORD_INITIAL_PATTERN_BRACKET_OPEN] = true,
    [WORD_INITIAL_PATTERN_STAR_BEGIN] = true,
    [WORD_INITIAL_PATTERN_QUESTION_BEGIN] = true,
    [WORD_INITIAL_ESCAPED_CHARACTER_BEGIN] = true,
    [WORD_INITIAL_SQ_OPEN] = true,
    [WORD_INITIAL_DQ_OPEN] = true,
    [WORD_INITIAL_DOLLAR_SQ_DOLLAR] = true,
    [WORD_INITIAL_DOLLAR_EXPANSION_START] = true,
    [WORD_INITIAL_BACKQUOTE_START] = true,
    [WORD_TILDE_START] = true,
  };
  memcpy(valid, base_symbols, sizeof(base_symbols));
  if (scanner->emission.active || !recovery_word_context(scanner)) {
    return;
  }
  for (size_t index = 0; index < TOKEN_COUNT; index += 1) {
    if (LEXICAL_SOURCES[index].kind != LEXICAL_SOURCE_NONE) {
      valid[index] = false;
    }
  }
  enum TokenType opener = top_context_opener(scanner);
  if (opener == SQ_OPEN) {
    valid[SQ_CLOSE] = true;
    valid[SINGLE_QUOTE_CONTENT_BEGIN] = true;
    return;
  }
  if (opener == DOLLAR_SQ_OPEN) {
    valid[DOLLAR_SQ_CLOSE] = true;
    valid[DOLLAR_SINGLE_QUOTE_TEXT_BEGIN] = true;
    valid[DOLLAR_SINGLE_QUOTE_ESCAPE] = true;
    return;
  }
  bool quoted = opener == DQ_OPEN || recovery_parameter_is_quoted(scanner);
  valid[DOLLAR_EXPANSION_START] = true;
  valid[DOUBLE_QUOTED_BACKQUOTE_START] = quoted;
  valid[BACKQUOTE_START] = !quoted;
  if (opener == DQ_OPEN) {
    valid[DQ_CLOSE] = true;
    valid[DOUBLE_QUOTE_TEXT_BEGIN] = true;
    valid[DOUBLE_QUOTE_ESCAPE_BEGIN] = true;
    return;
  }
  valid[PARAMETER_CLOSE] = true;
  valid[VARIABLE_NAME_BEGIN] = true;
  valid[SPECIAL_PARAMETER_BEGIN] = true;
  valid[SPECIAL_PARAMETER_HASH_BEGIN] = true;
  valid[PARAMETER_LENGTH_OPERATOR_BEGIN] = true;
  valid[PARAMETER_VALUE_OPERATOR_BEGIN] = true;
  valid[PARAMETER_PATTERN_OPERATOR_BEGIN] = true;
  valid[DOUBLE_QUOTED_PARAMETER_TEXT_BEGIN] = quoted;
  valid[DOUBLE_QUOTED_PARAMETER_ESCAPE_BEGIN] = quoted;
  valid[PARAMETER_LITERAL_BEGIN] = !quoted;
  valid[ESCAPED_CHARACTER_BEGIN] = !quoted;
  valid[SQ_OPEN] = !quoted;
  valid[DOLLAR_SQ_DOLLAR] = !quoted;
  valid[DQ_OPEN] = true;
}

static bool classify_recovery_source(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid
) {
  if (recovery_word_context(scanner)) {
    return classify_logical_source(scanner, input, valid);
  }
  static const struct LexicalPunctuation reserved_punctuation[] = {
    {'{', LEFT_BRACE},
    {'}', RIGHT_BRACE},
    {'!', PIPELINE_NEGATION},
  };
  for (size_t index = 0; index < COUNT_OF(reserved_punctuation); index += 1) {
    const struct LexicalPunctuation *entry = &reserved_punctuation[index];
    if (input->lexer.lookahead == entry->character && valid[entry->symbol]) {
      if (scan_delimited_character_token(&input->lexer, entry->symbol)) {
        return true;
      }
      logical_rewind(input);
      break;
    }
  }
  if (
    is_lowercase_letter(input->lexer.lookahead) &&
    scan_reserved_word(&input->lexer, valid)
  ) {
    return true;
  }
  logical_rewind(input);
  return lexical_attempt(input, lexical_classify(input, valid)) ||
    classify_word_start(scanner, input, valid);
}

static bool track_bracket_fallback(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  enum TokenType symbol,
  const bool *valid
) {
  struct LexicalEmission *emission = &scanner->emission;
  if (is_bracket_fallback_context(word_initial_source_symbol(symbol))) {
    emission->bracket_chains = bracket_chains_after_token(input, 0);
    emission->tracks_brackets = true;
    return true;
  }
  if (!in_bracket_fallback(scanner)) {
    return true;
  }
  enum TokenType opener = top_context_opener(scanner);
  enum TokenType literal = opener == PARAMETER_FALLBACK_LITERAL_BEGIN
    ? PARAMETER_LITERAL_BEGIN
    : opener == ASSIGNMENT_FALLBACK_LITERAL_BEGIN ? ASSIGNMENT_LITERAL_BEGIN
                                                  : LITERAL_BEGIN;
  if (!valid[literal]) {
    return true;
  }
  uint16_t chains = fallback_chains(scanner);
  switch (symbol) {
    case LITERAL_BEGIN:
    case ASSIGNMENT_LITERAL_BEGIN:
    case PARAMETER_LITERAL_BEGIN:
      chains = bracket_chains_after_token(input, chains);
      break;
    case PATTERN_STAR_BEGIN:
    case PATTERN_QUESTION_BEGIN:
    case ESCAPED_CHARACTER_BEGIN:
    case SQ_OPEN:
    case DQ_OPEN:
    case DOLLAR_SQ_DOLLAR:
    case DOLLAR_EXPANSION_START:
    case BACKQUOTE_START:
      chains = bracket_chains_after(chains, BRACKET_INPUT_MEMBER);
      break;
    case WORD_PATTERN_BRACKET_OPEN:
    case PARAMETER_PATTERN_BRACKET_OPEN: {
      struct LogicalPosition position = logical_position(input);
      logical_rewind(input);
      bool complete;
      bool scanned =
        lexical_pattern_probe(scanner, input, literal, &chains, &complete);
      logical_restore_position(input, &position);
      if (!scanned || !complete) {
        return false;
      }
      break;
    }
    default:
      return true;
  }
  emission->bracket_chains = chains;
  emission->tracks_brackets = true;
  return true;
}

/* Removed leading source must not displace a pending here-document token. */
static bool here_document_boundary_token_pending(
  const struct Scanner *scanner,
  const TSLexer *lexer,
  const bool *valid,
  size_t leading
) {
  if (scanner->sequence_end_pending && valid[HERE_DOCUMENT_SEQUENCE_END]) {
    return true;
  }
  if (scanner->active_frame.count == 0) {
    return false;
  }
  if (
    valid[HERE_DOCUMENT_END_COMMIT] &&
    here_document_end_is_ready(scanner, lexer) &&
    (scanner->here_document_end_line_consumed || leading == 0)
  ) {
    return true;
  }
  return scanner->active_frame.at_line_start &&
    (valid[HERE_DOCUMENT_END_BEGIN] ||
      valid[QUOTED_HERE_DOCUMENT_END_BEGIN] ||
      valid[HERE_DOCUMENT_CONTENT_LINE_START]);
}

static bool is_word_start_symbol(enum TokenType symbol) {
  return is_word_initial_symbol(symbol) || symbol == WORD_TILDE_START;
}

static void note_here_document_word(
  struct Scanner *scanner,
  const struct LogicalLexer *input,
  enum TokenType symbol
) {
  struct SourceContext *context = top_context(scanner);
  if (
    context == NULL ||
    !is_here_document_context(context->opener) ||
    context->here_word == HERE_WORD_OTHER ||
    context->here_word == HERE_WORD_ASSIGNMENT ||
    pending_backquote_context(scanner) == SIZE_MAX ||
    (symbol != QUOTED_HERE_DOCUMENT_TEXT &&
      symbol != NEWLINE &&
      symbol != HERE_DOCUMENT_TEXT_BEGIN &&
      symbol != HERE_DOCUMENT_ESCAPE_BEGIN &&
      symbol != DOLLAR_EXPANSION_START &&
      symbol != DOUBLE_QUOTED_BACKQUOTE_START)
  ) {
    return;
  }
  for (size_t index = 0; index < input->cursor.logical_count; index += 1) {
    const struct SourceCharacter *character = &input->cursor.logical[index];
    if (
      character->origin.end <= 0 ||
      (size_t)character->origin.end + input->base > input->mark
    )
      continue;
    if (context->here_word == HERE_WORD_START) {
      context->here_word = context->opener == QUOTED_HERE_DOCUMENT_BODY_START &&
          is_name_start_character(character->value)
        ? HERE_WORD_NAME
        : HERE_WORD_OTHER;
    } else if (context->here_word == HERE_WORD_NAME) {
      if (character->value == '=') context->here_word = HERE_WORD_ASSIGNMENT;
      else if (!is_name_character(character->value))
        context->here_word = HERE_WORD_OTHER;
    }
    if (
      context->here_word != HERE_WORD_START &&
      context->here_word != HERE_WORD_NAME
    )
      break;
  }
}

static void note_lone_word(
  struct Scanner *scanner,
  enum TokenType symbol,
  const bool *valid
) {
  struct SourceContext *context = top_context(scanner);
  if (context == NULL || !is_backquote_context(context->opener)) {
    return;
  }
  if (is_word_start_symbol(symbol)) {
    context->lone_word = valid[FNAME_TOKEN];
    return;
  }
  switch (symbol) {
    case ASSIGNMENT_NAME_TOKEN:
    case FILE_DESCRIPTOR:
    case PUNCT_LESS:
    case PUNCT_GREATER:
    case LESSAND_BEGIN:
    case GREATAND_BEGIN:
    case DGREAT_BEGIN:
    case LESSGREAT_BEGIN:
    case CLOBBER_BEGIN:
    case DLESS_OPERATOR_BEGIN:
    case DLESSDASH_OPERATOR_BEGIN:
    case WORD_SEPARATOR_BEGIN:
    case ASSIGNMENT_SEPARATOR_BEGIN:
    case REDIRECT_SEPARATOR_BEGIN:
    case REDIRECT_LIST_BEGIN:
    case RIGHT_BRACE:
    case PUNCT_RIGHT_PARENTHESIS:
    case FI_KEYWORD:
    case DONE_KEYWORD:
    case ESAC_KEYWORD:
      context->lone_word = false;
      break;
    default:
      break;
  }
}

static bool accept_classified_source(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  const bool *valid,
  bool recovering
) {
  enum TokenType symbol = (enum TokenType)input->lexer.result_symbol;
  bool accepted = true;
  note_here_document_word(scanner, input, symbol);
  note_lone_word(scanner, symbol, valid);
  if (is_arithmetic_context(symbol)) {
    accepted =
      source_context_pop(scanner) && source_context_push(scanner, symbol);
  } else if (
    symbol == HERE_DOCUMENT_END_COMMIT ||
    symbol == HERE_DOCUMENT_EOF ||
    symbol == WORD_BRACKET_FALLBACK_END ||
    symbol == ASSIGNMENT_BRACKET_FALLBACK_END ||
    symbol == PARAMETER_BRACKET_FALLBACK_END
  ) {
    accepted = source_context_pop(scanner);
  }
  if (
    symbol < TOKEN_COUNT && LEXICAL_SOURCES[symbol].kind != LEXICAL_SOURCE_NONE
  ) {
    accepted =
      begin_lexical_emission(scanner, input, symbol, valid, recovering) &&
      track_bracket_fallback(scanner, input, symbol, valid);
  }
  return accepted;
}

static enum ScanStep scan_retained_token(
  struct Scanner *scanner,
  struct LogicalLexer *input,
  TSLexer *lexer,
  const bool *valid,
  bool recovering
) {
  size_t pending = pending_backquote_context(scanner);
  if (
    pending == SIZE_MAX ||
    (recovering &&
      scanner->contexts[pending].backquote_token != BACKQUOTE_TOKEN_INVALID) ||
    !is_substitution_context(top_context_opener(scanner)) ||
    valid[COMMAND_SUBSTITUTION_BODY_BEGIN] ||
    valid[COMMAND_SUBSTITUTION_REMAINDER_END]
  ) {
    return SCAN_STEP_SKIP;
  }
  static const enum TokenType replay_tokens[] = {
    [BACKQUOTE_TOKEN_NONE] = TOKEN_COUNT,
    [BACKQUOTE_TOKEN_RIGHT_PARENTHESIS] = TOKEN_COUNT,
    [BACKQUOTE_TOKEN_STOP] = TOKEN_COUNT,
    [BACKQUOTE_TOKEN_INVALID] = TOKEN_COUNT,
    [BACKQUOTE_TOKEN_LEFT_PARENTHESIS] = RETAINED_LEFT_PARENTHESIS,
    [BACKQUOTE_TOKEN_WORD] = RETAINED_WORD_BEGIN,
    [BACKQUOTE_TOKEN_NAME_WORD] = RETAINED_WORD_BEGIN,
    [BACKQUOTE_TOKEN_ASSIGNMENT] = RETAINED_ASSIGNMENT_BEGIN,
    [BACKQUOTE_TOKEN_DECLARATION] = RETAINED_DECLARATION_BEGIN,
    [BACKQUOTE_TOKEN_FORWARDING] = RETAINED_FORWARDING_BEGIN,
    [BACKQUOTE_TOKEN_NAME_FORWARDING] = RETAINED_FORWARDING_BEGIN,
    [BACKQUOTE_TOKEN_IF] = RETAINED_IF,
    [BACKQUOTE_TOKEN_FOR] = RETAINED_FOR,
    [BACKQUOTE_TOKEN_CASE] = RETAINED_CASE,
    [BACKQUOTE_TOKEN_WHILE] = RETAINED_WHILE,
    [BACKQUOTE_TOKEN_UNTIL] = RETAINED_UNTIL,
    [BACKQUOTE_TOKEN_BRACE] = RETAINED_BRACE,
    [BACKQUOTE_TOKEN_NEGATION] = RETAINED_NEGATION,
  };
  enum BackquoteToken token = scanner->contexts[pending].backquote_token;
  enum TokenType replay = replay_tokens[token];
  if (
    token == BACKQUOTE_TOKEN_NAME_WORD ||
    token == BACKQUOTE_TOKEN_NAME_FORWARDING
  ) {
    scan_horizontal_blanks(&input->lexer);
    if (input->lexer.lookahead == '(') replay = RETAINED_FUNCTION_NAME;
  }
  if (replay == TOKEN_COUNT) {
    return token == BACKQUOTE_TOKEN_INVALID ? SCAN_STEP_REJECT : SCAN_STEP_SKIP;
  }
  if (!valid[replay]) {
    return SCAN_STEP_REJECT;
  }
  scanner->contexts[pending].backquote_token = BACKQUOTE_TOKEN_NONE;
  top_context(scanner)->lone_word = (replay == RETAINED_WORD_BEGIN ||
                                      replay == RETAINED_DECLARATION_BEGIN ||
                                      replay == RETAINED_FORWARDING_BEGIN) &&
    is_backquote_context(top_context_opener(scanner));
  lexer->result_symbol = (TSSymbol)replay;
  return SCAN_STEP_ACCEPT;
}

static bool scan_source(
  struct Scanner *scanner,
  const struct Scanner *initial,
  TSLexer *lexer,
  const bool *valid,
  bool recovering
) {
  if (!scanner_source_ready(scanner)) {
    return false;
  }
  if (scanner->emission.active) {
    return emit_physical_source(scanner, lexer, valid);
  }
  lexer->mark_end(lexer);
  static const enum TokenType ends[] = {
    COMMAND_SUBSTITUTION_END,
    ARITHMETIC_EXPANSION_END,
  };
  for (size_t index = 0; index < COUNT_OF(ends); index += 1) {
    if (valid[ends[index]]) {
      lexer->result_symbol = (TSSymbol)ends[index];
      return source_context_commit(scanner, ends[index]);
    }
  }
  if (scanner->active_frame.count > 0) {
    enum TokenType body = scanner->active_frame.documents[0].quoted
      ? QUOTED_HERE_DOCUMENT_BODY_START
      : HERE_DOCUMENT_BODY_START;
    if (valid[body]) {
      scanner->active_frame.at_line_start = true;
      lexer->result_symbol = (TSSymbol)body;
      return source_context_push(scanner, body);
    }
  }
  struct NativeInput native = {.lexer = lexer};
  struct HereDocumentInput documents;
  if (!here_document_input_init(&documents, &native, initial)) {
    native_input_clear(&native);
    return false;
  }
  if (documents.count > 0) {
    native.boundary = here_document_input_read;
    native.boundary_payload = &documents;
  }
  struct LogicalLexer input;
  if (!logical_init(&input, &native, &scanner->source)) {
    here_document_input_clear(&documents);
    native_input_clear(&native);
    return false;
  }
  size_t leading = logical_raw_position(&input);
  bool accepted = false;
  enum ScanStep retained =
    scan_retained_token(scanner, &input, lexer, valid, recovering);
  if (retained != SCAN_STEP_SKIP) {
    accepted = retained == SCAN_STEP_ACCEPT;
  } else if (
    native.count == 0 &&
    lexer->eof(lexer) &&
    valid[HERE_DOCUMENT_EOF] &&
    scanner->active_frame.count > 0
  ) {
    lexer->result_symbol = HERE_DOCUMENT_EOF;
    accepted = source_context_pop(scanner);
    if (accepted) finish_active_document(scanner);
  } else if (documents.end != NULL && documents.end->end == 0) {
    accepted = begin_here_document_end(scanner, lexer, documents.end, valid);
  } else if (
    leading > 0 &&
    valid[SOURCE_BEGIN] &&
    !here_document_boundary_token_pending(scanner, &input.lexer, valid, leading)
  ) {
    scanner->emission = (struct LexicalEmission){
      .symbol = SOURCE_BEGIN,
      .remaining = leading,
      .active = true,
    };
    lexer->result_symbol = SOURCE_BEGIN;
    accepted = true;
  } else if (
    recovering ? classify_recovery_source(scanner, &input, valid)
               : classify_logical_source(scanner, &input, valid)
  ) {
    accepted = accept_classified_source(scanner, &input, valid, recovering);
    lexer->result_symbol = input.lexer.result_symbol;
  }
  accepted = accepted && !native.failed && !input.cursor.failed;
  logical_clear(&input);
  here_document_input_clear(&documents);
  native_input_clear(&native);
  return accepted;
}

bool tree_sitter_dash_external_scanner_scan(
  void *payload,
  TSLexer *lexer,
  const bool *valid_symbols
) {
  if (payload == NULL) {
    return false;
  }
  bool recovering = true;
  for (size_t index = 0; index < TOKEN_COUNT; index += 1) {
    if (!valid_symbols[index]) {
      recovering = false;
      break;
    }
  }
  bool contextual_symbols[TOKEN_COUNT];
  if (recovering) {
    select_recovery_symbols(payload, contextual_symbols);
    valid_symbols = contextual_symbols;
  }
  struct Scanner next;
  if (!scanner_copy(payload, &next)) {
    return false;
  }
  if (
    !scan_source(&next, payload, lexer, valid_symbols, recovering) ||
    lexer->result_symbol >= TOKEN_COUNT ||
    !valid_symbols[lexer->result_symbol] ||
    !scanner_state_fits(&next)
  ) {
    clear_scanner(&next);
    return false;
  }
  clear_scanner(payload);
  *(struct Scanner *)payload = next;
  return true;
}
