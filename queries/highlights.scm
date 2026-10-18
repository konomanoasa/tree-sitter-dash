[
  (literal)
  (pattern_star_source)
  (pattern_question_source)
  (pattern_bracket_character_source)
  (pattern_bracket_hyphen_source)
  (pattern_bracket_range_operator_source)
  (pattern_bracket_negation_source)
  (pattern_character_class_content_source)
  (single_quote_content)
  (double_quote_text)
  (dollar_single_quote_text)
  (here_document_text)
  (quoted_here_document_text)
] @string

(pattern_bracket_source
  [
    "["
    "]"
  ] @string)

(pattern_character_class_source
  [
    "["
    ":"
    "]"
  ] @string)

[
  (escaped_character)
  (double_quote_escape)
  (dollar_single_quote_escape)
  (here_document_escape)
] @string.escape

(comment_text) @comment

[
  (arithmetic_number)
  (io_number)
] @number

[
  (arithmetic_variable)
  (variable_name)
  (name)
] @variable

(positional_parameter) @variable.parameter

(special_parameter) @variable.builtin

(fname) @function

[
  (if_keyword)
  (then_keyword)
  (elif_keyword)
  (else_keyword)
  (fi_keyword)
  (for_keyword)
  (in_keyword)
  (do_keyword)
  (done_keyword)
  (case_keyword)
  (esac_keyword)
  (while_keyword)
  (until_keyword)
] @keyword

[
  (arithmetic_operator)
  (parameter_length_operator)
  (parameter_value_operator)
  (parameter_pattern_operator)
  (and_if)
  (or_if)
  (bang)
  (dsemi)
] @operator

[
  (lessand)
  (greatand)
  (dgreat)
  (lessgreat)
  (clobber)
  (dless)
  (dlessdash)
] @operator

(io_file
  operator: [
    "<"
    ">"
  ] @operator)

(assignment_word
  "=" @operator)

(word
  name: (variable_name)
  "=" @operator)

[
  "&"
  "|"
] @operator

";" @punctuation.delimiter

[
  "("
  ")"
  "{"
  "}"
] @punctuation.bracket

(parameter_expansion
  "$" @variable
  parameter: [
    (variable_name)
    (positional_parameter)
    (special_parameter)
  ])

(command_substitution
  "$" @punctuation.special)

(arithmetic_expansion
  "$" @punctuation.special)

(single_quoted
  "'" @punctuation.delimiter)

(double_quoted
  "\"" @punctuation.delimiter)

(dollar_single_quoted
  [
    "$"
    "'"
  ] @punctuation.delimiter)

(backquote_substitution
  "`" @punctuation.delimiter)

(tilde_expansion
  "~" @string.special.path)

(tilde_expansion
  user: (tilde_user
    [
      (literal)
      (pattern_star_source)
      (pattern_question_source)
    ] @string.special.path))

(tilde_user
  (pattern_bracket_source
    [
      "[" @string.special.path
      "]" @string.special.path
      (pattern_bracket_negation_source) @string.special.path
    ]))

(tilde_user
  (pattern_bracket_source
    (pattern_bracket_members_source
      [
        (pattern_bracket_character_source) @string.special.path
        (pattern_bracket_hyphen_source) @string.special.path
        (pattern_bracket_range_source
          [
            (pattern_bracket_character_source) @string.special.path
            (pattern_bracket_hyphen_source) @string.special.path
            (pattern_bracket_range_operator_source) @string.special.path
          ])
        (pattern_character_class_source
          [
            "["
            ":"
            "]"
            (pattern_character_class_content_source)
          ] @string.special.path)
      ])))

(here_end_text) @label

(here_document_end_text) @label

(cmd_name
  (word
    (literal) @function.call))

(cmd_word
  (word
    (literal) @function.call))

(parameter_pattern
  [
    (literal) @string.regexp
    (pattern_star_source) @character.special
    (pattern_question_source) @character.special
  ])

(pattern_list
  (word
    [
      (literal) @string.regexp
      (pattern_star_source) @character.special
      (pattern_question_source) @character.special
    ]))

(cmd_name
  (word
    [
      (pattern_star_source) @character.special
      (pattern_question_source) @character.special
    ]))

(cmd_word
  (word
    [
      (pattern_star_source) @character.special
      (pattern_question_source) @character.special
    ]))

(cmd_suffix
  word: (word
    [
      (pattern_star_source) @character.special
      (pattern_question_source) @character.special
    ]))

(wordlist
  word: (word
    [
      (pattern_star_source) @character.special
      (pattern_question_source) @character.special
    ]))

(filename
  word: (word
    [
      (pattern_star_source) @character.special
      (pattern_question_source) @character.special
    ]))

(pattern_list
  (word
    (pattern_bracket_source
      [
        "[" @punctuation.bracket
        "]" @punctuation.bracket
        (pattern_bracket_negation_source) @operator
        (pattern_bracket_members_source
          [
            (pattern_bracket_character_source) @character
            (pattern_bracket_hyphen_source) @character
            (pattern_bracket_range_source
              [
                (pattern_bracket_character_source) @character
                (pattern_bracket_hyphen_source) @character
                (pattern_bracket_range_operator_source) @operator
              ])
            (pattern_character_class_source
              [
                "[" @punctuation.bracket
                ":" @punctuation.delimiter
                "]" @punctuation.bracket
                (pattern_character_class_content_source) @character.special
              ])
          ])
      ])))

(parameter_pattern
  (pattern_bracket_source
    [
      "[" @punctuation.bracket
      "]" @punctuation.bracket
      (pattern_bracket_negation_source) @operator
      (pattern_bracket_members_source
        [
          (pattern_bracket_character_source) @character
          (pattern_bracket_hyphen_source) @character
          (pattern_bracket_range_source
            [
              (pattern_bracket_character_source) @character
              (pattern_bracket_hyphen_source) @character
              (pattern_bracket_range_operator_source) @operator
            ])
          (pattern_character_class_source
            [
              "[" @punctuation.bracket
              ":" @punctuation.delimiter
              "]" @punctuation.bracket
              (pattern_character_class_content_source) @character.special
            ])
        ])
    ]))

(cmd_name
  (word
    (pattern_bracket_source
      [
        "[" @punctuation.bracket
        "]" @punctuation.bracket
        (pattern_bracket_negation_source) @operator
        (pattern_bracket_members_source
          [
            (pattern_bracket_character_source) @character
            (pattern_bracket_hyphen_source) @character
            (pattern_bracket_range_source
              [
                (pattern_bracket_character_source) @character
                (pattern_bracket_hyphen_source) @character
                (pattern_bracket_range_operator_source) @operator
              ])
            (pattern_character_class_source
              [
                "[" @punctuation.bracket
                ":" @punctuation.delimiter
                "]" @punctuation.bracket
                (pattern_character_class_content_source) @character.special
              ])
          ])
      ])))

(cmd_word
  (word
    (pattern_bracket_source
      [
        "[" @punctuation.bracket
        "]" @punctuation.bracket
        (pattern_bracket_negation_source) @operator
        (pattern_bracket_members_source
          [
            (pattern_bracket_character_source) @character
            (pattern_bracket_hyphen_source) @character
            (pattern_bracket_range_source
              [
                (pattern_bracket_character_source) @character
                (pattern_bracket_hyphen_source) @character
                (pattern_bracket_range_operator_source) @operator
              ])
            (pattern_character_class_source
              [
                "[" @punctuation.bracket
                ":" @punctuation.delimiter
                "]" @punctuation.bracket
                (pattern_character_class_content_source) @character.special
              ])
          ])
      ])))

(cmd_suffix
  word: (word
    (pattern_bracket_source
      [
        "[" @punctuation.bracket
        "]" @punctuation.bracket
        (pattern_bracket_negation_source) @operator
        (pattern_bracket_members_source
          [
            (pattern_bracket_character_source) @character
            (pattern_bracket_hyphen_source) @character
            (pattern_bracket_range_source
              [
                (pattern_bracket_character_source) @character
                (pattern_bracket_hyphen_source) @character
                (pattern_bracket_range_operator_source) @operator
              ])
            (pattern_character_class_source
              [
                "[" @punctuation.bracket
                ":" @punctuation.delimiter
                "]" @punctuation.bracket
                (pattern_character_class_content_source) @character.special
              ])
          ])
      ])))

(wordlist
  word: (word
    (pattern_bracket_source
      [
        "[" @punctuation.bracket
        "]" @punctuation.bracket
        (pattern_bracket_negation_source) @operator
        (pattern_bracket_members_source
          [
            (pattern_bracket_character_source) @character
            (pattern_bracket_hyphen_source) @character
            (pattern_bracket_range_source
              [
                (pattern_bracket_character_source) @character
                (pattern_bracket_hyphen_source) @character
                (pattern_bracket_range_operator_source) @operator
              ])
            (pattern_character_class_source
              [
                "[" @punctuation.bracket
                ":" @punctuation.delimiter
                "]" @punctuation.bracket
                (pattern_character_class_content_source) @character.special
              ])
          ])
      ])))

(filename
  word: (word
    (pattern_bracket_source
      [
        "[" @punctuation.bracket
        "]" @punctuation.bracket
        (pattern_bracket_negation_source) @operator
        (pattern_bracket_members_source
          [
            (pattern_bracket_character_source) @character
            (pattern_bracket_hyphen_source) @character
            (pattern_bracket_range_source
              [
                (pattern_bracket_character_source) @character
                (pattern_bracket_hyphen_source) @character
                (pattern_bracket_range_operator_source) @operator
              ])
            (pattern_character_class_source
              [
                "[" @punctuation.bracket
                ":" @punctuation.delimiter
                "]" @punctuation.bracket
                (pattern_character_class_content_source) @character.special
              ])
          ])
      ])))

(line_continuation) @punctuation.special
