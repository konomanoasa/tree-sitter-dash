import assert from "node:assert/strict";
import { test } from "node:test";
import {
  hasRecovery,
  nodeIdentity,
  parseCst,
  sourceNodes,
} from "./support/cst.js";
import {
  applyEdits,
  assertIncrementalEqualsFresh,
  parseValidCst,
  runParse,
  writeSource,
} from "./support/parser.js";

test("pending legacy tokens close only the first command substitution across enclosing contexts", () => {
  for (const [token, body] of [
    [")", "$(first)$(second)"],
    [")", "$(\nfirst\n$(second)"],
    [")", `\${v:-$(first)}$(second)`],
    [")", "$((1+$(first)))$(second)"],
    [")", "$((1))$(first)$(second)"],
    [")", '\\`ignored \\"\\`$(second)'],
    [")", "\\`\\`$(second)"],
    [";;", '\\`ignored \\"\\`$(second)'],
    [";;", "\\`\\`$(second)"],
  ]) {
    const substitution = `\`cat <<A${token}${body}\n\``;
    for (const source of [
      `: ${substitution}\n`,
      `: "${substitution}"\n`,
      `: $((1+${substitution}))\n`,
      `: [${substitution}]\n`,
      `: \${v:-${substitution}}\n`,
      `cat <<OUT\n${substitution}\nOUT\n`,
    ]) {
      const nodes = sourceNodes(
        parseValidCst(writeSource("pending-legacy-token", source)),
        source,
      );
      const commands = nodes.filter(({ type }) => type === "cmd_name");
      assert.deepEqual(
        commands.map(({ text }) => text),
        [source.startsWith("cat") ? "cat" : ":", "cat", "second"],
        source,
      );
      const substitutions = nodes.filter(
        ({ type }) => type === "command_substitution",
      );
      assert.deepEqual(
        substitutions.map(({ text }) => text),
        body.startsWith("\\`") ? ["$(second)"] : ["$(", "$(second)"],
        source,
      );
    }
  }
});

test("legacy remainder boundaries are preserved across enclosing expansion contexts", () => {
  for (const inner of [
    'printf 2) "',
    '(printf 2) ignored "',
    'printf 2 (ignored "',
    'a=2 (ignored "',
    "cat <<E)2\nE\n",
  ]) {
    const substitution = `\`${inner}\``;
    for (const source of [
      `: ${substitution}\n`,
      `: "${substitution}"\n`,
      `: $((1+${substitution}))\n`,
      `: [${substitution}]\n`,
      `: \${v:-${substitution}}\n`,
      `cat <<OUT\n${substitution}\nOUT\n`,
    ]) {
      const output = parseValidCst(writeSource("legacy-context", source));
      assert.equal(
        parseCst(output).filter(
          (entry) => nodeIdentity(entry).type === "backquote_remainder",
        ).length,
        1,
        source,
      );
    }
  }
});

test("colon-brace operators retain their source across enclosing contexts and source views", () => {
  for (const [expansion, operator] of [
    ...[
      "a",
      "_name2",
      "1",
      "01",
      "10",
      "00",
      "0",
      "?",
      "-",
      "!",
      "*",
      "@",
      "$",
    ].map((parameter) => [`\${${parameter}:}}`, ":}"]),
    [`\${a:}word}`, ":}"],
    [`\${a:}\${b:-x}}`, ":}"],
    [`\${a:}$(printf x)}`, ":}"],
    [`\${a:}$((1+2))}`, ":}"],
    [`\${a:})}`, ":}"],
    [`\${a:}"}"}`, ":}"],
    [`\${a:}\\}}`, ":}"],
    [`\${a:}}tail`, ":}"],
    [`\${a:}}$(printf tail)`, ":}"],
    [`\${a:\\\n}}`, ":\\\n}"],
    [`\${a:\0}}`, ":\0}"],
    [`\${a:\\\0\n}}`, ":\\\0\n}"],
  ]) {
    for (const source of [
      `: ${expansion}\n`,
      `: "${expansion}"\n`,
      `v=${expansion}\n`,
      `: \${outer:-${expansion}}\n`,
      `cat <<E\n${expansion}\nE\n`,
      `: $(printf '%s' ${expansion})\n`,
      `: \`printf '%s' ${expansion}\`\n`,
      `: $((${expansion}))\n`,
      `: $((@ ${expansion}))\n`,
    ]) {
      const nodes = sourceNodes(
        parseValidCst(writeSource("colon-brace-parameter", source)),
        source,
      );
      const expansionStart = source.indexOf(expansion);
      const expansionText = expansion.slice(0, expansion.lastIndexOf("}") + 1);
      assert.deepEqual(
        nodes
          .filter(
            ({ type, start }) =>
              type === "parameter_expansion" && start === expansionStart,
          )
          .map(({ end, text }) => [end, text]),
        [[expansionStart + expansionText.length, expansionText]],
        source,
      );
      const start = expansionStart + expansion.indexOf(":");
      assert.deepEqual(
        nodes
          .filter(
            ({ type, start: position }) =>
              type === "parameter_value_operator" && position === start,
          )
          .map(({ field, start: position, end, text }) => [
            field,
            position,
            end,
            text,
          ]),
        [["operator", start, start + operator.length, operator]],
        source,
      );
    }
  }
});

test("incomplete and invalid colon-brace parameters require recovery across contexts", () => {
  for (const expansion of [
    `\${a:}`,
    `\${a:}word`,
    `\${#:}}`,
    `\${#:}word}`,
    `\${#a:}}`,
    `\${#1:}}`,
    `\${:}}`,
    `\${#:\\\n}}`,
    `\${a:!}`,
    `\${a: }x}`,
    `\${a::}x}`,
    `\${a:\\}x}`,
    `\${a:}X}\${b`,
    `\${a:}X}$(if)`,
  ]) {
    for (const source of [
      `: ${expansion}\n`,
      `: "${expansion}"\n`,
      `v=${expansion}\n`,
      `: \${outer:-${expansion}}\n`,
      `cat <<E\n${expansion}\nE\n`,
      `: $(printf '%s' ${expansion})\n`,
      `: \`printf '%s' ${expansion}\`\n`,
      `: $((${expansion}))\n`,
      `: $((@ ${expansion}))\n`,
    ]) {
      assert.equal(
        runParse({
          source: writeSource("colon-brace-parameter", source),
          mode: "recovery",
        }).recovery,
        true,
        source,
      );
    }
  }
});

test("colon-brace edits preserve boundaries and repair to the fresh CST", () => {
  for (const [source, removed, inserted, mode] of [
    [`: \${a:-word}tail\n`, ":-", ":}", "valid"],
    [`: \${a:}word}tail\n`, ":}", ":-", "valid"],
    [`: "\${a:}word}tail"\n`, ":}", ":+", "valid"],
    [`: \${a:}word}tail\n`, ":}", ":\\\n}", "valid"],
    [`: \${a:\0}word}tail\n`, "}word", "-word", "valid"],
    [`: \${a:\\\0\n}word}tail\n`, "}word", "-word", "valid"],
    [`: \${a:}word}tail\n`, "a", "01", "valid"],
    [`: \${a:}word}tail\n`, "a", "#", "recovery"],
    [`: \${a:}word}tail\n`, "a", "#a", "recovery"],
    [`: \${a:}word}tail\n`, ":}", ":", "recovery"],
    [`: \${a:}word}tail\n`, "word}", "word", "recovery"],
    [`: \${a:}word}tail\n`, "word", `\${b:}x}`, "valid"],
    [`: \${outer:-\${a:}word}tail}\n`, ":}word", ":-word", "valid"],
    [`v=\${a:-word}\n`, ":-", ":}", "valid"],
    [`: $((1+\${a:-2}))\n`, ":-", ":}", "valid"],
    [`: $((@ \${a:})} ))\n`, ")}", "\\)}", "valid"],
    [`: $(printf '%s' \${a:})})\n`, ":}", ":-", "valid"],
    [`: \`printf '%s' \${a:})}\`\n`, ":}", ":-", "valid"],
    [`cat <<E\n\${a:}word}tail\nE\n`, ":}", ":-", "valid"],
    [`cat <<E\n\${a:}word}tail\nE\n`, ":}", ":", "recovery"],
  ]) {
    const byte = source.indexOf(removed);
    assert.notEqual(byte, -1);
    const edit = { byte, deleteBytes: removed.length, insert: inserted };
    const repair = { byte, deleteBytes: inserted.length, insert: removed };
    const initial = writeSource("colon-brace-edit", source);
    const changed = writeSource(
      "colon-brace-edited",
      applyEdits(source, [edit]),
    );
    if (mode === "valid") {
      assertIncrementalEqualsFresh(initial, changed, source, edit);
    } else {
      assert.equal(runParse({ source: changed, mode }).recovery, true, source);
      assert.equal(
        runParse({
          source: initial,
          edits: [edit],
          expectedSource: changed,
          mode,
        }).recovery,
        true,
        source,
      );
    }
    assertIncrementalEqualsFresh(initial, initial, source, edit, repair);
  }
});

test("arithmetic boundaries are independent of evaluation success and surrounding quotes", () => {
  for (const expression of [
    "",
    "1+",
    "@",
    '"1"',
    "'1'",
    "1,2",
    "++1",
    "\\(",
    "\\)",
    "x) y",
    "1) ",
    "(1) 2",
    "$",
    `\${x:-2}+3`,
    "$(printf 2)+3",
  ]) {
    for (const quoted of [false, true]) {
      const expansion = `$((${expression}))`;
      const source = `: ${quoted ? `"${expansion}"` : expansion}\n`;
      const output = parseValidCst(writeSource("arithmetic-source", source));
      assert.match(output, /arithmetic_expansion/);
      if (!expression.includes("$(printf"))
        assert.doesNotMatch(output, /command_substitution/);
    }
  }
});

test("arithmetic layout is limited to blanks and newlines", () => {
  for (const [separator, structured] of [
    [" ", true],
    ["\t", true],
    ["\n", true],
    ["\v", false],
    ["\f", false],
    ["\r", false],
  ]) {
    const source = `: $((1${separator}+${separator}1${separator}))\n`;
    const output = parseValidCst(writeSource("arithmetic-layout", source));
    if (structured) {
      assert.match(output, /arithmetic_binary_expression/);
      assert.doesNotMatch(output, /arithmetic_source/);
    } else {
      assert.match(output, /arithmetic_source_fragment/);
      assert.doesNotMatch(output, /arithmetic_binary_expression/);
    }
  }
});

test("static declaration names retain assignment arguments through quotes, options and redirects", () => {
  for (const name of [
    "export",
    "readonly",
    "local",
    '"export"',
    "'ex'port",
    "\\local",
    "$'ex\\160ort'",
    "command export",
    "command -p -- export",
    "command -- command local",
    "command >out -p export",
  ]) {
    const output = parseValidCst(
      writeSource("declaration", `${name} a=~ -p >out b=$v\n`),
    );
    assert.equal((output.match(/name: variable_name/g) ?? []).length, 2, name);
    assert.equal((output.match(/tilde_expansion/g) ?? []).length, 1, name);
  }
  for (const name of [
    "printf",
    "$cmd",
    "command -- -- export",
    "command -v export",
  ]) {
    const output = parseValidCst(
      writeSource("ordinary-command", `${name} x=~\n`),
    );
    assert.doesNotMatch(output, /name: variable_name|tilde_expansion/, name);
  }
});

test("bracket contexts distinguish character classes, negation and ordinary punctuation", () => {
  for (const word of ["[^a]", "[!a]", "[[:alpha:]]", "[[.a.]]", "[[=a=]]"]) {
    for (const source of [
      `: ${word}\n`,
      `case x in ${word}) :;; esac\n`,
      `: "\${x#${word}}"\n`,
    ]) {
      const output = parseValidCst(writeSource("pattern-source", source));
      assert.match(output, /pattern_bracket_source/);
      if (word.includes("alpha"))
        assert.match(output, /pattern_character_class_source/);
      else assert.doesNotMatch(output, /pattern_character_class_source/);
      if (word[1] === "^" || word[1] === "!")
        assert.match(output, /pattern_bracket_negation_source/);
    }
  }
});

test("range operators take the next character literally and classes end a range", () => {
  for (const { word, classes } of [
    { word: "[x-[:alpha:]]", classes: 0 },
    { word: "[a-[:alpha:]*", classes: 0 },
    { word: "[[:alpha:]-a]", classes: 1 },
    { word: "[[:alpha:]-[:alpha:]]", classes: 2 },
    { word: "[[-a]", classes: 0 },
    { word: "[a-c-[:alpha:]]", classes: 1 },
  ]) {
    const output = parseValidCst(
      writeSource("range-endpoint", `case x in ${word}) :;; esac\n`),
    );
    assert.equal(
      (output.match(/pattern_bracket_source/g) ?? []).length,
      1,
      word,
    );
    assert.equal(
      (output.match(/pattern_character_class_source/g) ?? []).length,
      classes,
      word,
    );
  }
  const output = parseValidCst(
    writeSource("open-range", "case x in [[:alpha:]-[:alpha:]) :;; esac\n"),
  );
  assert.match(output, /literal `\[`/);
  assert.equal(
    (output.match(/pattern_character_class_source/g) ?? []).length,
    0,
  );
});

test("backquotes remove backslash-newline before decoding their body", () => {
  for (const [name, source, type, text] of [
    [
      "backquote-single-quote",
      "x=`printf '%s' 'a\\\nb'`\n",
      "single_quote_content",
      "a\\\nb",
    ],
    [
      "backquote-comment",
      "x=`printf x # c\\\nprintf y`\n",
      "comment_text",
      "# c\\\nprintf y",
    ],
    [
      "backquote-dollar-single",
      "x=`printf '%s' $'a\\\nb'`\n",
      "dollar_single_quote_text",
      "a\\\nb",
    ],
  ]) {
    const nodes = sourceNodes(parseValidCst(writeSource(name, source)), source);
    assert.ok(
      nodes.some((node) => node.type === type && node.text === text),
      source,
    );
    assert.equal(
      nodes.filter((node) => node.type === "line_continuation").length,
      1,
      source,
    );
  }
});

test("retained words expose only local operators, redirects and command arguments", () => {
  for (const [suffix, type, text] of [
    ["", "command_substitution", "$()"],
    ["| next", "pipe_sequence", "| next"],
    ["&& next", "and_or", "&& next"],
    ["; next", "list", "; next"],
    ["&", "complete_command", "&"],
    [">out", "cmd_suffix", ">out"],
  ]) {
    const source = `: \`cat <<A <<B)probe\nA\n$(${suffix})\nB\n\`\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("retained-source", source)),
      source,
    );
    assert.ok(
      nodes.some((node) => node.type === type && node.text === text),
      source,
    );
    const local = nodes.filter(
      ({ start, end }) =>
        start >= source.indexOf("$(") && end <= source.indexOf("\nB"),
    );
    assert.ok(
      local.every(({ start, end }) => end > start),
      source,
    );
    assert.deepEqual(
      local.filter(({ type }) => type === "cmd_name").map(({ text }) => text),
      suffix.endsWith("next") ? ["next"] : [],
      source,
    );
  }
  const source = ": `cat <<A($(printf X))\n`\n";
  const nodes = sourceNodes(
    parseValidCst(writeSource("retained-subshell", source)),
    source,
  );
  const subshell = nodes.find(({ type }) => type === "subshell");
  assert.equal(subshell?.text, "printf X)");
  assert.deepEqual(
    nodes
      .filter(
        ({ type, start, end }) =>
          type.startsWith('"') &&
          start >= subshell.start &&
          end <= subshell.end,
      )
      .map(({ type }) => type),
    ['")"'],
  );
});

test("compound remainders preserve word classes, reserved starts and body overwrites", () => {
  for (const header of [
    "(cat <<A)",
    "{ cat <<A; } ",
    "if :; then cat <<A; fi ",
  ]) {
    for (const [word, commandWord] of [
      ["probe", false],
      ["1", false],
      ["12", false],
      ["v=VALUE", true],
      ["'v=VALUE'", false],
      ["v\\\n=VALUE", true],
    ]) {
      const source = `: \`${header}${word}\n$(printf X)\nA\n\`\n`;
      const nodes = sourceNodes(
        parseValidCst(writeSource("compound-retained-word", source)),
        source,
      );
      const local = nodes.filter(
        ({ start, end }) =>
          start >= source.indexOf("$(") && end <= source.indexOf("\nA\n"),
      );
      assert.equal(
        local.some(({ type }) => type === "cmd_name"),
        false,
        source,
      );
      assert.deepEqual(
        local.filter(({ type }) => type === "cmd_word").map(({ text }) => text),
        commandWord ? ["printf"] : [],
        source,
      );
    }
  }
  for (const [token, suffix, owner] of [
    ["if", ":; then :; fi", "if_clause"],
    ["for", "x; do :; done", "for_clause"],
    ["case", "x in x) :;; esac", "case_clause"],
    ["while", ":; do :; done", "while_clause"],
    ["until", ":; do :; done", "until_clause"],
    ["{", ":; }", "brace_group"],
    ["!", "printf X", "pipeline"],
    ["probe", " () { :; }", "function_definition"],
  ]) {
    const source = `: \`(cat <<A)${token}\n$(${suffix})\nA\n\`\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("compound-retained-opener", source)),
      source,
    );
    const local = nodes.filter(
      ({ start, end }) =>
        start >= source.indexOf("$(") && end <= source.indexOf("\nA\n"),
    );
    assert.ok(
      local.some(({ type }) => type === owner),
      source,
    );
    assert.equal(
      local.some(({ text }) => text === token),
      false,
      source,
    );
    assert.ok(
      local.every(({ start, end }) => end > start),
      source,
    );
  }
  const source = ": `(cat <<A <<B)if\nA\n$(printf X)\nB\n`\n";
  const nodes = sourceNodes(
    parseValidCst(writeSource("compound-retained-overwrite", source)),
    source,
  );
  assert.deepEqual(
    nodes.filter(({ type }) => type === "cmd_name").map(({ text }) => text),
    [":", "cat"],
  );
  assert.equal(
    nodes.some(({ type }) => type === "if_clause"),
    false,
  );
});

test("pending tokens are overwritten by each body until the first parsed substitution consumes them", () => {
  for (const [inner, names, words] of [
    [
      "cat <<A <<'B' <<C)probe\nA\nv=VALUE\nB\n$(printf X)$(later)\nC\n",
      [":", "cat", "later"],
      ["printf"],
    ],
    ["cat <<A <<B)$(:)\nA\n$(printf X)\nB\n", [":", "cat", "printf"], []],
    [
      `cat <<A <<B)\${v:-$(:)}\nA\n$(printf X)\nB\n`,
      [":", "cat", "printf"],
      [],
    ],
    [
      "cat <<A <<B)probe\nA\n\\`printf X)ignored\\`$(later)\nB\n",
      [":", "cat", "later"],
      [],
    ],
    ["cat <<A <<B\nprobe\nA\n$(printf X)\nB\n", [":", "cat", "printf"], []],
  ]) {
    const source = `: \`${inner}\`\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("retained-consumption", source)),
      source,
    );
    assert.deepEqual(
      nodes.filter(({ type }) => type === "cmd_name").map(({ text }) => text),
      names,
      source,
    );
    assert.deepEqual(
      nodes.filter(({ type }) => type === "cmd_word").map(({ text }) => text),
      words,
      source,
    );
  }
  for (const suffix of ["next (ignored", "next;;ignored", "next; fi ignored"]) {
    const source = `: \`cat <<A <<B)probe\nA\n\\\`${suffix}\\\`\nB\n\`\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("retained-remainder", source)),
      source,
    );
    assert.equal(
      nodes.filter(({ type }) => type === "backquote_remainder").length,
      2,
      source,
    );
  }
});

test("retained declaration and forwarding commands keep assignment arguments and tilde context", () => {
  for (const [command, suffix, names] of [
    ["export", "x=$v y=~", ["x", "y"]],
    ["'ex'port", "x=$v y=~", ["x", "y"]],
    ["readonly", "x=$v y=~", ["x", "y"]],
    ["local", ">out y=~", ["y"]],
    ["command", "export x=$v y=~", ["x", "y"]],
    ["command", "-p -- export x=$v y=~", ["x", "y"]],
    ["'command'", "-- readonly y=~", ["y"]],
    ["command", ">out -p export y=~", ["y"]],
    ["command", "-v export y=~", []],
    ["command", "-- -- export y=~", []],
    ["probe", "x=$v y=~", []],
  ]) {
    const source = `: \`(cat <<A)${command}\n$(${suffix})\nA\n\`\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("retained-declaration", source)),
      source,
    );
    assert.deepEqual(
      nodes
        .filter(
          ({ type, field }) => type === "variable_name" && field === "name",
        )
        .map(({ text }) => text),
      names,
      source,
    );
    assert.equal(
      nodes.filter(({ type }) => type === "tilde_expansion").length,
      names.length ? 1 : 0,
      source,
    );
  }
});

test("retained tokens preserve invalid command and function boundaries", () => {
  for (const inner of [
    "cat <<A($(printf X)\n",
    "cat <<A <<B)probe\nA\n$( () { :; })\nB\n",
    "cat <<A <<B)probe\nA\n$(if :; then :; fi)\nB\n",
    "(cat <<A)in\n$(printf X)\nA\n",
    "(cat <<A)in\n\\`printf X\\`\nA\n",
    "(cat <<A)export\n$( () { :; })\nA\n",
    "(cat <<A)'probe'\n$( () { :; })\nA\n",
    ...[
      ";;",
      "; then ",
      "; elif ",
      "; else ",
      "; fi ",
      "; do ",
      "; done ",
      "; esac ",
      "; } ",
    ].map((token) => `cat <<A${token}$(:)\n`),
  ]) {
    const source = `: \`${inner}\`\n`;
    assert.equal(
      hasRecovery(
        runParse({
          source: writeSource("invalid-retained-token", source),
          mode: "recovery",
        }).output,
      ),
      true,
      source,
    );
  }
});

test("edits to earlier bodies update retained classifications and repairs restore fresh CSTs", () => {
  for (const [source, removed, inserted, mode] of [
    [
      ": `cat <<A <<B)v=VALUE\nA\n$(v=NEXT printf X)\nB\n`\n",
      "<<A",
      "<<'A'",
      "valid",
    ],
    [
      ": `cat <<'A' <<B)v=VALUE\nA\n$(v=NEXT printf X)\nB\n`\n",
      "v=VALUE",
      "v+=VALUE",
      "valid",
    ],
    [": `cat <<A <<B)A\n$( () { :; })\nB\n`\n", ")A", ")probe\nA", "recovery"],
    [": `cat <<A <<B)probe\nA\n$(printf X)\nB\n`\n", "probe", "$(:)", "valid"],
    [": `cat <<A($(printf X))\n`\n", "A(", "A)", "valid"],
    [": `(cat <<A)probe\n$(printf X)\nA\n`\n", "probe", "v=VALUE", "valid"],
    [": `(cat <<A)probe\n$(x=$v y=~)\nA\n`\n", "probe", "export", "valid"],
    [": `(cat <<A)command\n$(-p export y=~)\nA\n`\n", "-p", "-v", "valid"],
    [": `(cat <<A)if\n$(:; then :; fi)\nA\n`\n", ")if", ")probe", "recovery"],
    [": `(cat <<A)probe\n$()\nA\n`\n", "probe", "}", "recovery"],
    [": `(cat <<A)probe\n\\`\\`\nA\n`\n", "probe", "in", "recovery"],
    [": `(cat <<A)probe\n\\`:\\`\nA\n`\n", "probe", "in", "recovery"],
    [": `(cat <<A)probe\n\\`v=X\\`\nA\n`\n", "probe", "in", "recovery"],
    [": `(cat <<A)probe\n\\`printf X\\`\nA\n`\n", "probe", "}", "valid"],
    [": `(cat <<A)probe\n$(\nnext)\nA\n`\n", "next", "", "valid"],
    [": `(cat <<A)v=X\n$(\nnext)\nA\n`\n", "$(\n", "$( ", "valid"],
    [": `(cat <<A)export\n$(\nnext)\nA\n`\n", "$(\n", "$( # note\n", "valid"],
    [": `(cat <<A)command\n\\`\nnext\\`\nA\n`\n", "next", "", "valid"],
    [
      ": `cat <<A <<B)probe\nA\n\\`printf X)ignored\\`$(later)\nB\n`\n",
      "X)ignored",
      "X (ignored",
      "valid",
    ],
  ]) {
    const byte = source.indexOf(removed);
    const initial = writeSource("retained-edit", source);
    const edit = { byte, deleteBytes: removed.length, insert: inserted };
    const repair = { byte, deleteBytes: inserted.length, insert: removed };
    assertIncrementalEqualsFresh(initial, initial, source, edit, repair);
    const final =
      source.slice(0, byte) + inserted + source.slice(byte + removed.length);
    const finalSource = writeSource("retained-edited", final);
    if (mode === "valid") {
      assertIncrementalEqualsFresh(initial, finalSource, source, edit);
    } else {
      assert.equal(
        runParse({
          source: initial,
          edits: [edit],
          expectedSource: finalSource,
          mode: "recovery",
        }).recovery,
        true,
        source,
      );
    }
  }
});

test("retained command classes give intervening and trailing newlines their ordinary owners", () => {
  const backquote = (body) =>
    `\`${body.replaceAll("\\", "\\\\").replaceAll("`", "\\`")}\``;
  for (const kind of ["command", "backquote"]) {
    for (const word of ["probe", "v=X", "export", "command"]) {
      for (const layout of [
        "\n",
        "\n\n",
        " # note\n",
        "\n# note\n",
        "\n \n",
        " \\\n\n",
        "\n\\\n ",
      ]) {
        for (const [tail, names] of [
          ["", []],
          ["next", ["next"]],
          ["next\nlater", ["next", "later"]],
          ["cat <<C\ninner\nC\nnext", ["cat", "next"]],
        ]) {
          const body = layout + tail;
          const substitution =
            kind === "command" ? `$(${body})` : backquote(body);
          const source = `: ${backquote(`(cat <<A)${word}\n${substitution}\nA\n`)}\n`;
          const nodes = sourceNodes(
            parseValidCst(writeSource("retained-newline-owner", source)),
            source,
          );
          const owner = nodes
            .filter(({ type }) => type === `${kind}_substitution_body`)
            .at(-1);
          assert.ok(owner, source);
          const local = nodes.filter(
            ({ depth, start, end }) =>
              depth > owner.depth && start >= owner.start && end <= owner.end,
          );
          const commands = local.find(
            ({ type, depth }) =>
              type === "complete_commands" && depth === owner.depth + 2,
          );
          assert.equal(Boolean(commands), names.length > 0, source);
          assert.deepEqual(
            local
              .filter(({ type }) => type === "cmd_name")
              .map(({ text }) => text),
            names,
            source,
          );
          if (commands) {
            const first = local.find(
              ({ depth }) => depth === commands.depth + 2,
            );
            assert.equal(first?.type, "newline_list", source);
            assert.equal(first?.field, "separator", source);
          } else {
            assert.ok(
              local.some(
                ({ type, depth }) =>
                  type === "linebreak" && depth === owner.depth + 2,
              ),
              source,
            );
          }
          assert.ok(
            local.every(({ start, end }) => end > start),
            source,
          );
        }
      }
    }
  }
});

for (const [name, escapeText, extra] of [
  ["hexadecimal two-digit limit", "\\x41", "2"],
  ["octal three-digit limit", "\\123", "4"],
  ["short Unicode four-digit limit", "\\u0041", "2"],
  ["long Unicode eight-digit limit", "\\U00000041", "2"],
]) {
  for (const [context, prefix, suffix, spelling] of [
    ["ordinary", ": $'", "'\n", escapeText],
    ["backquote", ": `: $'", "'`\n", `\\${escapeText}`],
  ]) {
    test(`${context} ${name}: inserting and removing a digit preserves the escape boundary`, () => {
      const initialText = `${prefix}${spelling}g${suffix}`;
      const finalText = `${prefix}${spelling}${extra}g${suffix}`;
      const initial = writeSource(`${name}-before`, initialText);
      const final = writeSource(`${name}-after`, finalText);
      const byte = Buffer.byteLength(prefix + spelling);
      const edit = { byte, deleteBytes: 0, insert: extra };
      const restore = { byte, deleteBytes: 1, insert: "" };
      for (const [file, text, tail] of [
        [initial, initialText, "g"],
        [final, finalText, `${extra}g`],
      ]) {
        const nodes = sourceNodes(parseValidCst(file), text);
        assert.deepEqual(
          nodes
            .filter(({ type }) => type === "dollar_single_quote_escape")
            .map(({ text, start, end }) => [text, start, end]),
          [[spelling, Buffer.byteLength(prefix), byte]],
        );
        assert.deepEqual(
          nodes
            .filter(({ type }) => type === "dollar_single_quote_text")
            .map(({ text }) => text),
          [tail],
        );
      }
      assertIncrementalEqualsFresh(initial, final, name, edit);
      assertIncrementalEqualsFresh(final, initial, name, restore);
      assertIncrementalEqualsFresh(initial, initial, name, edit, restore);
    });
  }
}

for (const { name, source, removed, inserted, counts } of [
  {
    name: "quoted command spelling enables declaration assignments",
    source: '"exporx" X=~\n',
    removed: 'x"',
    inserted: 't"',
    counts: [
      ["assignment_value", 0, 1],
      ["tilde_expansion", 0, 1],
    ],
  },
  {
    name: "quoting the tilde removes expansion classification",
    source: ": ~root/tail\n",
    removed: "~",
    inserted: '"~"',
    counts: [["tilde_expansion", 1, 0]],
  },
  {
    name: "a quoted wildcard becomes active after quote removal",
    source: ": '*'\n",
    removed: "'*'",
    inserted: "*",
    counts: [
      ["pattern_star_source", 0, 1],
      ["single_quoted", 1, 0],
    ],
  },
  {
    name: "a closing bracket changes literal source into a pattern",
    source: ": [ab\n",
    removed: "ab",
    inserted: "ab]",
    counts: [["pattern_bracket_source", 0, 1]],
  },
  {
    name: "a command separator resets declaration argument context",
    source: "export x=~ y=~\n",
    removed: " y=",
    inserted: "; printf y=",
    counts: [
      ["assignment_value", 2, 1],
      ["tilde_expansion", 2, 1],
    ],
  },
  {
    name: "an assignment colon opens a second tilde prefix",
    source: "x=~root/path\n",
    removed: "/path",
    inserted: ":~user",
    counts: [["tilde_expansion", 1, 2]],
  },
]) {
  test(`${name}: edits and restoration preserve word classification`, () => {
    const offset = source.indexOf(removed);
    assert.notEqual(offset, -1);
    const finalText =
      source.slice(0, offset) +
      inserted +
      source.slice(offset + removed.length);
    const initial = writeSource(`${name}-before`, source);
    const final = writeSource(`${name}-after`, finalText);
    for (const [file, text, index] of [
      [initial, source, 1],
      [final, finalText, 2],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      for (const [type, before, after] of counts) {
        assert.equal(
          nodes.filter((node) => node.type === type).length,
          index === 1 ? before : after,
          `${name}: ${type}`,
        );
      }
    }
    const byte = Buffer.byteLength(source.slice(0, offset));
    const edit = {
      byte,
      deleteBytes: Buffer.byteLength(removed),
      insert: inserted,
    };
    const restore = {
      byte,
      deleteBytes: Buffer.byteLength(inserted),
      insert: removed,
    };
    assertIncrementalEqualsFresh(initial, final, name, edit);
    assertIncrementalEqualsFresh(final, initial, name, restore);
    assertIncrementalEqualsFresh(initial, initial, name, edit, restore);
  });
}

function assertClassificationEdit(name, source, edit, counts) {
  const finalText = applyEdits(source, [edit]).toString();
  const initial = writeSource(`${name}-initial`, source);
  const final = writeSource(`${name}-final`, finalText);
  for (const [file, text, changed] of [
    [initial, source, false],
    [final, finalText, true],
  ]) {
    const nodes = sourceNodes(parseValidCst(file), text);
    for (const [type, before, after] of counts) {
      assert.equal(
        nodes.filter((node) => node.type === type).length,
        changed ? after : before,
        `${name}: ${changed ? "edited" : "initial"} ${type}`,
      );
    }
  }
  const restore = {
    byte: edit.byte,
    deleteBytes: Buffer.byteLength(edit.insert),
    insert: Buffer.from(source)
      .subarray(edit.byte, edit.byte + edit.deleteBytes)
      .toString(),
  };
  assertIncrementalEqualsFresh(initial, final, `${name}: edit`, edit);
  assertIncrementalEqualsFresh(final, initial, `${name}: restore`, restore);
  assertIncrementalEqualsFresh(
    initial,
    initial,
    `${name}: roundtrip`,
    edit,
    restore,
  );
}

for (const { name, pattern, edit, counts } of [
  {
    name: "trailing hyphen gains a range endpoint",
    pattern: "[a-]",
    edit: { byte: 3, deleteBytes: 0, insert: "z" },
    counts: [["pattern_bracket_range_operator_source", 0, 1]],
  },
  {
    name: "range endpoint becomes a literal hyphen",
    pattern: "[a-z]",
    edit: { byte: 3, deleteBytes: 1, insert: "-" },
    counts: [["pattern_bracket_range_operator_source", 1, 1]],
  },
  {
    name: "negation disappears after inserting a first member",
    pattern: "[!a]",
    edit: { byte: 1, deleteBytes: 0, insert: "b" },
    counts: [["pattern_bracket_negation_source", 1, 0]],
  },
  {
    name: "character class becomes ordinary bracket punctuation",
    pattern: "[[:alpha:]]",
    edit: { byte: 2, deleteBytes: 7, insert: ".alpha." },
    counts: [["pattern_character_class_source", 1, 0]],
  },
  {
    name: "removing a class delimiter preserves the outer bracket",
    pattern: "[[:alpha:]]",
    edit: { byte: 8, deleteBytes: 1, insert: "" },
    counts: [
      ["pattern_character_class_source", 1, 0],
      ["pattern_bracket_source", 1, 1],
    ],
  },
  {
    name: "an unclosed bracket keeps a following wildcard active",
    pattern: "[ab*",
    edit: { byte: 4, deleteBytes: 0, insert: "]" },
    counts: [
      ["pattern_bracket_source", 0, 1],
      ["pattern_star_source", 1, 0],
    ],
  },
]) {
  for (const [context, prefix, suffix] of [
    ["argument", "printf ", "\n"],
    ["case", "case x in ", ") :;; esac\n"],
    ["quoted parameter pattern", ': "${v#', '}"\n'],
  ]) {
    test(`${context}: ${name} after editing and restoration`, () => {
      assertClassificationEdit(
        `${context}: ${name}`,
        prefix + pattern + suffix,
        { ...edit, byte: Buffer.byteLength(prefix) + edit.byte },
        counts,
      );
    });
  }
}

for (const { name, expression, edit, counts } of [
  {
    name: "parameter bytes complete a hexadecimal literal",
    expression: `0x\${digits}`,
    edit: { byte: 2, deleteBytes: 9, insert: "F" },
    counts: [
      ["arithmetic_source", 1, 0],
      ["arithmetic_number", 0, 1],
    ],
  },
  {
    name: "a numeric assignment target becomes a variable",
    expression: "1=2",
    edit: { byte: 0, deleteBytes: 1, insert: "x" },
    counts: [
      ["arithmetic_source", 1, 0],
      ["arithmetic_assignment_expression", 0, 1],
    ],
  },
  {
    name: "a missing binary operand is completed",
    expression: "1+",
    edit: { byte: 2, deleteBytes: 0, insert: "2" },
    counts: [
      ["arithmetic_source", 1, 0],
      ["arithmetic_binary_expression", 0, 1],
    ],
  },
  {
    name: "quotes change a structured sum into arithmetic source",
    expression: "1+2",
    edit: { byte: 2, deleteBytes: 1, insert: '"2"' },
    counts: [
      ["arithmetic_source", 0, 1],
      ["arithmetic_binary_expression", 1, 0],
    ],
  },
  {
    name: "a vertical tab invalidates arithmetic layout",
    expression: "1 +2",
    edit: { byte: 1, deleteBytes: 1, insert: "\v" },
    counts: [
      ["arithmetic_source", 0, 1],
      ["arithmetic_binary_expression", 1, 0],
    ],
  },
]) {
  for (const [context, prefix, suffix] of [
    ["ordinary arithmetic", "echo $((", "))\n"],
    ["quoted arithmetic", 'echo "$((', '))"\n'],
    ["parameter operand arithmetic", 'echo "${v:-$((', '))}"\n'],
    ["here-document arithmetic", "cat <<E\n$((", "))\nE\n"],
  ]) {
    test(`${context}: ${name} after editing and restoration`, () => {
      assertClassificationEdit(
        `${context}: ${name}`,
        prefix + expression + suffix,
        { ...edit, byte: Buffer.byteLength(prefix) + edit.byte },
        counts,
      );
    });
  }
}
