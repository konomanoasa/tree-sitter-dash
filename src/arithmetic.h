#ifndef TREE_SITTER_DASH_ARITHMETIC_H_
#define TREE_SITTER_DASH_ARITHMETIC_H_

#include <stdbool.h>
#include <stdint.h>

enum ArithmeticValidation {
  ARITHMETIC_VALIDATION_INVALID,
  ARITHMETIC_VALIDATION_INCOMPLETE,
  ARITHMETIC_VALIDATION_VALID,
  ARITHMETIC_VALIDATION_RESOURCE_FAILURE,
};

static bool arithmetic_whitespace(int32_t character) {
  return character == ' ' ||
    character == '\t' ||
    character == '\n' ||
    character == '\v' ||
    character == '\f';
}

static bool is_decimal_digit(int32_t character) {
  return character >= '0' && character <= '9';
}

static bool is_hexadecimal_digit(int32_t character) {
  return is_decimal_digit(character) ||
    (character >= 'A' && character <= 'F') ||
    (character >= 'a' && character <= 'f');
}

static bool is_name_start_character(int32_t character) {
  return (character >= 'A' && character <= 'Z') ||
    (character >= 'a' && character <= 'z') ||
    character == '_';
}

static bool is_name_character(int32_t character) {
  return is_name_start_character(character) || is_decimal_digit(character);
}

#endif
