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
  assertRepeatedColdParse,
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

test("a lone word before a parenthesis starts a function definition instead of a remainder", () => {
  for (const word of ['\\"', "a-b", "$v", "'a'b", "~", '\\"$(:)']) {
    for (const layout of ["", " "]) {
      const source = writeSource(
        "lone-word",
        `: \`${word}${layout}(ignored\`\n`,
      );
      assert.equal(
        hasRecovery(runParse({ source, mode: "recovery" }).output),
        true,
        `${word}${layout}(`,
      );
    }
  }
  for (const command of ["printf 2", "a=2", ">f", '\\" >f', '{ \\"; }']) {
    for (const layout of ["", " "]) {
      const source = `: \`${command}${layout}(ignored\`\n`;
      const output = parseValidCst(writeSource("remainder", source));
      assert.match(output, /backquote_remainder_text `\(ignored`/, source);
    }
  }
});

test("static command words obey function name rules across layout and enclosing contexts", () => {
  for (const [name, accepted] of [
    ["command", true],
    ["printf", true],
    ["read", true],
    ["export", false],
    ["readonly", false],
    ["local", false],
    ["'command'", false],
    ['"command"', false],
    ["c'ommand'", false],
    ["\\command", false],
    ["$'command'", false],
    ["'ex'port", false],
    ["$'export'", false],
  ]) {
    for (const layout of ["", " ", "\t", " \t ", "\\\n", " \\\n\t"]) {
      const definition = `${name}${layout}() :`;
      const legacy = `\`${definition.replaceAll("\\", "\\\\")}\``;
      for (const source of [
        `${definition}\n`,
        `: $( ${definition})\n`,
        `: ${legacy}\n`,
        `: "${legacy}"\n`,
        `cat <<E\n${legacy}\nE\n`,
      ]) {
        const parsed = runParse({
          source: writeSource("function-name-boundary", source),
          mode: accepted ? "valid" : "recovery",
        });
        assert.equal(parsed.recovery, !accepted, source);
        if (accepted) {
          const nodes = sourceNodes(parsed.output, source);
          assert.deepEqual(
            nodes
              .filter(({ type }) => type === "fname")
              .map(({ text }) => text),
            [name],
            source,
          );
          assert.equal(
            nodes.filter(({ type }) => type === "function_definition").length,
            1,
            source,
          );
          assert.equal(
            nodes.some(({ type }) => type === "backquote_remainder"),
            false,
            source,
          );
        }
      }
    }
  }
});

test("edits across a static command and function boundary agree with fresh parsing", () => {
  const functionNameGapBytes = 4096;
  for (const [source, removed, inserted, mode] of [
    ["command () :\n", "command", "export", "recovery"],
    [": $(command () :)\n", "command", "'command'", "recovery"],
    [": `command () :`\n", "command", "local", "recovery"],
    [": `command () :`\n", "command", "$'command'", "recovery"],
    [": `command () :`\n", " ()", " x()", "valid"],
    [": `command () :`\n", "command", "command -- command", "valid"],
    [": `command () :`\n", "command", "v=X command", "valid"],
    [": `export x() ignored`\n", " x", "", "recovery"],
    [": `command() :`\n", "()", " \t()", "valid"],
    [": `command () :`\n", " ()", "\n()", "recovery"],
    [": $(command () :)\n", " ()", " \\\n ()", "valid"],
    [": $(command () :)\n", "()", "(x)", "recovery"],
    [": `command () :`\n", "()", "(", "recovery"],
    [
      `command${" ".repeat(functionNameGapBytes)}() :\n`,
      "()",
      "x()",
      "recovery",
    ],
  ]) {
    const byte = source.indexOf(removed);
    assert.notEqual(byte, -1);
    const edit = { byte, deleteBytes: removed.length, insert: inserted };
    const repair = { byte, deleteBytes: inserted.length, insert: removed };
    const changed =
      source.slice(0, byte) + inserted + source.slice(byte + removed.length);
    const initial = writeSource("function-name-edit", source);
    const final = writeSource("function-name-edited", changed);
    assertIncrementalEqualsFresh(initial, initial, source, edit, repair);
    if (mode === "valid") {
      assertIncrementalEqualsFresh(initial, final, source, edit);
    } else {
      assert.equal(
        runParse({
          source: initial,
          edits: [edit],
          expectedSource: final,
          mode,
        }).recovery,
        true,
        source,
      );
    }
  }
});

test("colon-brace parameter forms require recovery across enclosing contexts and source views", () => {
  for (const expansion of [
    `\${a:}`,
    `\${a:}}`,
    `\${a:}word}`,
    `\${1:}}`,
    `\${?:}}`,
    `\${#:}}`,
    `\${#:}word}`,
    `\${a:}\${b:-x}}`,
    `\${a:}$(printf x)}`,
    `\${a:})}`,
    `\${a:}"}"}`,
    `\${a:}\\}}`,
    `\${a:}}tail`,
    `\${a:}}$(printf tail)`,
    `\${a:\\\n}}`,
    `\${a:\0}}`,
    `\${a:\\\0\n}}`,
    `\${#:\\\n}}`,
    `\${a:!}`,
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

test("one-digit descriptors remain distinct from multi-digit argument words", () => {
  for (const descriptor of ["0", "1", "9"]) {
    const source = `: ${descriptor}>out\n`;
    const output = parseValidCst(
      writeSource(`descriptor-${descriptor}`, source),
    );
    assert.match(output, /io_number/);
  }
  for (const digits of ["10", "01", "2147483648"]) {
    const output = parseValidCst(
      writeSource(`argument-${digits}`, `: ${digits}>out\n`),
    );
    assert.doesNotMatch(output, /io_number/);
    const source = writeSource(`compound-${digits}`, `{ :; } ${digits}>out\n`);
    assert.equal(
      hasRecovery(runParse({ source, mode: "recovery" }).output),
      true,
    );
  }
});

test("here-document EOF preserves declaration order without fabricated end nodes", () => {
  for (const delimiter of ["E", "'E'", "''"]) {
    for (const count of [1, 2, 4]) {
      for (const suffix of ["", "\n", "\nbody\n", "\nbody"]) {
        const source = `: ${`<<${delimiter} `.repeat(count).trimEnd()}${suffix}`;
        const output = parseValidCst(writeSource("here-eof", source));
        const types = parseCst(output).map((entry) => nodeIdentity(entry).type);
        assert.equal(
          types.filter((type) => type === "here_document").length,
          count,
          source,
        );
        assert.equal(
          types.filter((type) => type === "here_document_end").length,
          0,
          source,
        );
      }
    }
  }
});

test("removed source before EOF stays inside the current here-document", () => {
  for (const source of [
    "cat <<-X <<E\n\t",
    "cat <<-X <<E\n\t\t",
    "cat <<E <<X\nbody\n\\\n",
    "cat <<-X\n\t",
    "cat <<E\nbody\n\\\n",
  ]) {
    const documents =
      source.includes("<<E <<X") || source.includes("<<-X <<E") ? 2 : 1;
    const output = parseValidCst(writeSource("removed-eof", source));
    const nodes = sourceNodes(output, source).filter(
      ({ type }) => type === "here_document",
    );
    assert.equal(nodes.length, documents, source);
    assert.equal(nodes[0].end, Buffer.byteLength(source), source);
    for (const node of nodes.slice(1)) {
      assert.equal(node.start, Buffer.byteLength(source), source);
      assert.equal(node.end, Buffer.byteLength(source), source);
    }
  }
});

test("multiline delimiter spelling and ranges survive quoting, tab stripping and EOF", () => {
  for (const [word, delimiter] of [
    ['"a\nb"', "a\nb"],
    ["'a\nb'", "a\nb"],
    ["$'a\\nb'", "a\nb"],
    ["`x\ny`", "`x\ny`"],
    ["$(x\ny)", "$(x\ny)"],
    ["$'\\na'", "\na"],
    ["$'a\\n'", "a\n"],
    ["$'\\n'", "\n"],
    ['"あ\nい"', "あ\nい"],
    ['"a\n\tb"', "a\n\tb"],
  ]) {
    for (const operator of ["<<", "<<-"]) {
      for (const newline of ["", "\n"]) {
        const prefix = `cat ${operator}${word}\nBODY\n${operator === "<<-" ? "\t" : ""}`;
        const source = `${prefix}${delimiter}${newline}${newline ? "printf AFTER\n" : ""}`;
        const nodes = sourceNodes(
          parseValidCst(writeSource("multiline-delimiter", source)),
          source,
        );
        const ends = nodes.filter(
          ({ type }) => type === "here_document_end_text",
        );
        assert.equal(ends.length, 1, source);
        assert.equal(ends[0].text, delimiter, source);
        assert.equal(ends[0].start, Buffer.byteLength(prefix), source);
        assert.equal(
          ends[0].end,
          Buffer.byteLength(prefix + delimiter),
          source,
        );
        assert.equal(
          nodes.filter(({ type }) => type === "cmd_name").length,
          newline ? 2 : 1,
          source,
        );
      }
    }
  }
});

test("multiline delimiter probes preserve mismatches and enclosing substitution boundaries", () => {
  for (const operator of ["<<", "<<-"]) {
    for (const body of [
      "a\nX\n",
      "a\nbX\n",
      "a\na\n",
      "a\\\n\nb\n",
      "a\n\tb\n",
    ]) {
      const prefix = `cat ${operator}'a\nb'\n${body}`;
      const source = `${prefix}a\nb\nprintf AFTER\n`;
      const nodes = sourceNodes(
        parseValidCst(writeSource("multiline-mismatch", source)),
        source,
      );
      const ends = nodes.filter(
        ({ type }) => type === "here_document_end_text",
      );
      assert.equal(ends.length, 1, source);
      assert.equal(ends[0].start, Buffer.byteLength(prefix), source);
      assert.equal(ends[0].text, "a\nb", source);
      assert.equal(
        nodes.filter(({ type }) => type === "cmd_name").length,
        2,
        source,
      );
    }
  }
  for (const [opening, closing] of [
    ["echo $(", ")\n"],
    ["echo `", "`\n"],
    ["echo $((1+$(", ")))\n"],
  ]) {
    const source = `${opening}cat <<'a\nb'\nBODY\na\nb\n${closing}printf AFTER\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("multiline-substitution", source)),
      source,
    );
    assert.deepEqual(
      nodes
        .filter(({ type }) => type === "here_document_end_text")
        .map(({ text }) => text),
      ["a\nb"],
      source,
    );
    assert.equal(
      nodes.filter(({ type }) => type === "cmd_name").length,
      3,
      source,
    );
  }
});

test("here-document command substitutions suspend the outer delimiter", () => {
  for (const substitution of [
    '$(printf "%s" "a\nE\nb")',
    '`printf "a\nE\nb"`',
    "$(cat <<IN\nE\nIN\n)",
    "$(\n:\nE\nE\n)",
    "`\n:\nE\n`",
  ]) {
    const output = parseValidCst(
      writeSource("here-nested", `cat <<E\n${substitution}\nE\n`),
    );
    assert.equal(
      parseCst(output).filter(
        (entry) => nodeIdentity(entry).type === "here_document_end",
      ).length,
      substitution.includes("<<IN") ? 2 : 1,
    );
  }
  for (const substitution of [`\${x:-a\nE\nb}`, "$((1+\nE\n2))"]) {
    const source = writeSource("here-bounded", `cat <<E\n${substitution}\nE\n`);
    assert.equal(
      hasRecovery(runParse({ source, mode: "recovery" }).output),
      true,
    );
  }
});

test("here-document delimiter matching precedes continuation joining", () => {
  for (const operator of ["<<", "<<-"]) {
    for (const body of ["E\\\n", "a\\\nE", "EO\\\nF"]) {
      const output = parseValidCst(
        writeSource("here-joined", `cat ${operator}E\n${body}\n`),
      );
      assert.doesNotMatch(output, /end: here_document_end/);
    }
    const output = parseValidCst(
      writeSource(
        "here-empty-continuation",
        `cat ${operator}E\n\\\nE\nprintf after\n`,
      ),
    );
    assert.match(output, /end: here_document_end/);
    assert.equal((output.match(/name: cmd_name/g) ?? []).length, 2);
  }
  for (const body of ["\t\\\nE", "\t\\\n\tE"]) {
    const output = parseValidCst(
      writeSource("here-tab-prefix", `cat <<-E\n${body}\n`),
    );
    assert.doesNotMatch(output, /end: here_document_end/);
    assert.doesNotMatch(output, /line_continuation/);
  }
});

test("delimiter source uses quote removal without command or parameter validation", () => {
  for (const [word, end] of [
    ["$(if)", "$(if)"],
    ["$(a|)", "$(a|)"],
    ["$(# literal)", "$(# literal)"],
    ["\"$(printf 'E')\"", "$(printf E)"],
    [`"\${x:-'E'}"`, `\${x:-'E'}`],
    [`"\${x#'E'}"`, `\${x#E}`],
    ["$'E\\0ignored'", "E"],
    ["$((case x in x)esac))", "$((case x in x)esac))"],
    ["$((\\)))", "$((\\)))"],
    ["`'a`b'`", "`a`b`"],
    ["`a\\\\b`", "`a\\b`"],
    [`\${"}`, `\${}`],
    [`\${'}`, `\${}`],
    [`\${\\}`, `\${}`],
    [`\${a'}`, `\${a'}`],
    [`\${#'}`, `\${#}`],
    [`\${#'x}`, `\${#'x}`],
    [`\${#%}`, `\${#}`],
    [`\${:}`, `\${}`],
    [`\${:\\}}`, `\${}}`],
    ["`\\'a`", "`\\'a`"],
    ["$(\\a\\\\\\')", "$(\\a\\\\')"],
    [`$(\${x:-\\a})`, `$(\${x:-\\a})`],
    [`\`\${x:-\\a}\``, `\`\${x:-\\a}\``],
    ["$((\\a))", "$((\\a))"],
    ['$((\\"a))', '$(("a))'],
    ["$((\\'a))", "$((\\'a))"],
    ["$'\\q\\`'", "\\q\\`"],
    [`$(\${x:-)}`, `$(\${x:-)}`],
    [`\`\${x:-\``, `\`\${x:-\``],
  ]) {
    for (const operator of ["<<", "<<-"]) {
      const output = parseValidCst(
        writeSource(
          "delimiter-source",
          `cat ${operator}${word}\nbody\n${end}\n`,
        ),
      );
      assert.match(output, /word: here_end_text/);
      assert.match(output, /end: here_document_end/);
      assert.doesNotMatch(
        output,
        /command_substitution|arithmetic_expansion|parameter_expansion/,
      );
    }
  }
});

test("editing delimiter spellings switches between EOF body and a matching terminator", () => {
  for (const [name, unmatchedWord, matchedWord, delimiter] of [
    ["hex-byte", "$'\\xFF'", "$'\\xC3\\xBF'", "ÿ"],
    ["octal-byte", "$'\\377'", "$'\\303\\277'", "ÿ"],
    ["joined-quotes", "$'\\xC3'", "$'\\xC3'$'\\277'", "ÿ"],
    ["two-byte-parameter", `"\${é}"`, `'\${é}'`, `\${é}`],
    ["three-byte-parameter", `"\${あ}"`, `'\${あ}'`, `\${あ}`],
    ["four-byte-parameter", `"\${😀}"`, `'\${😀}'`, `\${😀}`],
  ]) {
    const sources = [
      {
        word: unmatchedWord,
        ends: [],
        body: `body\n${delimiter}\necho AFTER\n`,
        commands: ["cat"],
      },
      {
        word: matchedWord,
        ends: [delimiter],
        body: "body\n",
        commands: ["cat", "echo"],
      },
    ].map(({ word, ends, body, commands }) => {
      const source = `cat <<${word}\nbody\n${delimiter}\necho AFTER\n`;
      const file = writeSource(name, source);
      const nodes = sourceNodes(parseValidCst(file), source);
      for (const [type, expected] of [
        ["here_end_text", [word]],
        ["quoted_here_document_body", [body]],
        ["here_document_end_text", ends],
        ["cmd_name", commands],
      ]) {
        assert.deepEqual(
          nodes.filter((node) => node.type === type).map(({ text }) => text),
          expected,
          `${name}: ${type}`,
        );
      }
      return file;
    });
    const [unmatched, matched] = sources;
    const remove = {
      byte: 6,
      deleteBytes: Buffer.byteLength(matchedWord),
      insert: unmatchedWord,
    };
    const repair = {
      byte: 6,
      deleteBytes: Buffer.byteLength(unmatchedWord),
      insert: matchedWord,
    };
    assertIncrementalEqualsFresh(
      matched,
      unmatched,
      `${name}-remove-matching-bytes`,
      remove,
    );
    assertIncrementalEqualsFresh(
      unmatched,
      matched,
      `${name}-restore-matching-bytes`,
      repair,
    );
    assertIncrementalEqualsFresh(
      matched,
      matched,
      `${name}-roundtrip`,
      remove,
      repair,
    );
  }
});

test("delimiter parameter prefixes follow C-locale byte handling across UTF-8 widths and quoting", () => {
  for (const character of ["\u0080", "é", "あ", "😀"]) {
    for (const prefix of ["", "#", "a", "a:", "a:-"]) {
      const spelling = `\${${prefix}${character}}`;
      for (const [word, matches] of [
        [spelling, prefix !== ""],
        [`"${spelling}"`, prefix !== ""],
        [`'${spelling}'`, true],
        [`\\${spelling}`, true],
      ]) {
        const source = `cat <<${word}\nBODY\n${spelling}\nprintf AFTER\n`;
        const file = writeSource("multibyte-delimiter", source);
        if (!matches && word === spelling) {
          assert.equal(
            runParse({ source: file, mode: "recovery" }).recovery,
            true,
            source,
          );
          continue;
        }
        const nodes = sourceNodes(parseValidCst(file), source);
        assert.deepEqual(
          nodes
            .filter(({ type }) => type === "here_document_end_text")
            .map(({ text }) => text),
          matches ? [spelling] : [],
          source,
        );
        assert.deepEqual(
          nodes
            .filter(({ type }) => type === "cmd_name")
            .map(({ text }) => text),
          matches ? ["cat", "printf"] : ["cat"],
          source,
        );
      }
    }
    for (const prefix of ["", "#"]) {
      const source = `cat <<"\${${prefix}${character}}"\nBODY\n\${${prefix}}\nprintf AFTER\n`;
      const nodes = sourceNodes(
        parseValidCst(writeSource("multibyte-false-end", source)),
        source,
      );
      assert.equal(
        nodes.filter(({ type }) => type === "here_document_end_text").length,
        0,
        source,
      );
      assert.equal(
        nodes.filter(({ type }) => type === "cmd_name").length,
        1,
        source,
      );
    }
  }
});

test("substitutions inside here-document bodies read raw source", () => {
  for (const { name, source, ends, continuations } of [
    {
      name: "here-raw-tabs",
      source: "cat <<-E\n$(\ncat <<I\n\ta\n\tI\nI\n)\n\tE\n",
      ends: ["I", "E"],
      continuations: 0,
    },
    {
      name: "here-raw-quoted",
      source: "cat <<E\n$(\ncat <<'I'\na\\\nI\n)\nE\n",
      ends: ["I", "E"],
      continuations: 0,
    },
    {
      name: "here-raw-single",
      source: "cat <<E\n$(printf '%s' 'a\\\nb')\nE\n",
      ends: ["E"],
      continuations: 0,
    },
    {
      name: "here-raw-backquote",
      source: "cat <<E\n`cat <<'I'\na\\\nb\nI\n`\nE\n",
      ends: ["I", "E"],
      continuations: 1,
    },
  ]) {
    const nodes = sourceNodes(parseValidCst(writeSource(name, source)), source);
    assert.deepEqual(
      nodes
        .filter((node) => node.type === "here_document_end_text")
        .map((node) => node.text),
      ends,
      source,
    );
    assert.equal(
      nodes.filter((node) => node.type === "line_continuation").length,
      continuations,
      source,
    );
  }
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

test("retained body words distinguish arguments, assignments and empty function names", () => {
  for (const [delimiter, body, assignment] of [
    ["A", "", false],
    ["'A'", "", false],
    ["A", "probe ARG\n", false],
    ["A", "v=VALUE\n", false],
    ["'A'", "v=VALUE\n", true],
    ["'A'", "v='VALUE'\nsecond\n", true],
    ["'A'", "1v=VALUE\n", false],
    ["'A'", "v+=VALUE\n", false],
    ["'A'", "v\n=VALUE\n", false],
    ["'A'", "$v=VALUE\n", false],
  ]) {
    const source = `: \`cat <<${delimiter} <<B)${body}A\n$(v=NEXT printf X)$(later)\nB\n\`\n`;
    const nodes = sourceNodes(
      parseValidCst(writeSource("retained-word-classification", source)),
      source,
    );
    assert.deepEqual(
      nodes.filter(({ type }) => type === "cmd_name").map(({ text }) => text),
      [":", "cat", "later"],
      source,
    );
    assert.deepEqual(
      nodes.filter(({ type }) => type === "cmd_word").map(({ text }) => text),
      assignment ? ["printf"] : [],
      source,
    );
    assert.deepEqual(
      nodes
        .filter(({ type }) => type === "assignment_word")
        .map(({ text }) => text),
      assignment ? ["v=NEXT"] : [],
      source,
    );
    const first = nodes.find(
      ({ type, text }) =>
        type === "simple_command" && text === "v=NEXT printf X",
    );
    assert.ok(first, source);
    const children = nodes.filter(
      ({ depth, start, end }) =>
        depth === first.depth + 2 && start >= first.start && end <= first.end,
    );
    assert.deepEqual(
      children.map(({ field }) => field),
      assignment ? ["prefix", "word", "suffix"] : ["suffix"],
      source,
    );
  }

  const source = ": `cat <<A <<B)A\n$( () { :; })\nB\n`\n";
  const nodes = sourceNodes(
    parseValidCst(writeSource("retained-empty-function", source)),
    source,
  );
  assert.equal(
    nodes.filter(({ type }) => type === "function_definition").length,
    1,
  );
  assert.equal(nodes.filter(({ type }) => type === "fname").length, 0);
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

test("here-document body size is independent of serialized delimiter capacity", () => {
  const bodyLineCount = 4096;
  const scannerStateCapacityBytes = 1024;
  for (const quoted of [false, true]) {
    const body = "ordinary body text\n".repeat(bodyLineCount);
    const output = parseValidCst(
      writeSource(
        "long-body",
        `cat <<${quoted ? "'END'" : "END"}\n${body}END\n`,
      ),
    );
    assert.match(output, /end: here_document_end/);
    const retained = `: \`cat <<${quoted ? "'A'" : "A"} <<B)v=${body}A\n$(printf X)\nB\n\`\n`;
    const initial = writeSource("long-retained-body", retained);
    const byte = retained.indexOf("v=");
    const edit = { byte, deleteBytes: 1, insert: "1" };
    const final = writeSource(
      "long-retained-body-edited",
      applyEdits(retained, [edit]),
    );
    assertIncrementalEqualsFresh(initial, final, "long retained body", edit);
  }
  const source = writeSource(
    "delimiter-capacity",
    `cat <<'${"D".repeat(scannerStateCapacityBytes)}'\nbody\n`,
  );
  assertRepeatedColdParse("resource", source, "oversized delimiter");
});
