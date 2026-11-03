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

test("pending heredocs preserve hash suffixes across word boundary edits and EOF", () => {
  const command = "cat <<EOF; echo ";
  for (const part of [
    "a",
    '"$@"',
    '""',
    "'a'",
    "$'a'",
    `\${v}`,
    "$((1))",
    "$(printf a)",
    "`printf a`",
    "\\a",
    "[a]",
    "[a",
    "*",
  ]) {
    for (const [name, ending, bodies, ends] of [
      ["body", "\nBODY\nEOF\n", ["BODY\n"], ["EOF"]],
      ["EOF", "", [], []],
    ]) {
      const prefix = command + part;
      const gaps = ["", " ", "\\\n"];
      const files = gaps.map((gap) => {
        const source = `${prefix}${gap}# c${ending}`;
        const file = writeSource("heredoc-hash-boundary", source);
        const nodes = sourceNodes(parseValidCst(file), source);
        const description = `${name}: ${JSON.stringify(part)} with ${JSON.stringify(gap)}`;
        assert.deepEqual(
          nodes
            .filter(
              ({ type, start }) => type === "word" && start === command.length,
            )
            .map(({ text }) => text),
          [gap === " " ? part : `${part}${gap}#`],
          description,
        );
        assert.deepEqual(
          nodes
            .filter(({ type }) => type === "comment")
            .map(({ start, end, text }) => [start, end, text]),
          gap === " " ? [[prefix.length + 1, prefix.length + 4, "# c"]] : [],
          description,
        );
        for (const [type, expected] of [
          ["here_document_body", bodies],
          ["here_document_end_text", ends],
        ]) {
          assert.deepEqual(
            nodes.filter((node) => node.type === type).map(({ text }) => text),
            expected,
            `${description}: ${type}`,
          );
        }
        assert.equal(
          nodes.filter(({ type }) => type === "here_document").length,
          1,
        );
        return file;
      });
      for (let index = 1; index < gaps.length; index += 1) {
        const gap = gaps[index];
        const description = `${name}: ${JSON.stringify(part)} with ${JSON.stringify(gap)}`;
        assertIncrementalEqualsFresh(files[0], files[index], description, {
          byte: prefix.length,
          deleteBytes: 0,
          insert: gap,
        });
        assertIncrementalEqualsFresh(files[index], files[0], description, {
          byte: prefix.length,
          deleteBytes: gap.length,
          insert: "",
        });
      }
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

for (const [name, prefix, suffix] of [
  ["top-level", "", ""],
  ["command substitution", "echo $(\n", ")\n"],
]) {
  test(`${name}: emptying a queued delimiter preserves the next document and command`, () => {
    const namedText = `${prefix}cat <<-'A' <<-'B'\nbody-a\n\tA\nbody-b\n\tB\necho AFTER\n${suffix}`;
    const emptyText = `${prefix}cat <<-'' <<-'B'\nbody-a\n\t\nbody-b\n\tB\necho AFTER\n${suffix}`;
    const named = writeSource("queued-named", namedText);
    const empty = writeSource("queued-empty", emptyText);
    for (const [file, text, ends] of [
      [named, namedText, ["A", "B"]],
      [empty, emptyText, ["B"]],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      for (const [type, expected] of [
        ["quoted_here_document_body", ["body-a\n", "body-b\n"]],
        ["here_document_end_text", ends],
      ]) {
        assert.deepEqual(
          nodes.filter((node) => node.type === type).map(({ text }) => text),
          expected,
        );
      }
      assert.equal(
        nodes.filter(({ type }) => type === "here_document_end").length,
        2,
      );
      assert.ok(
        nodes.some(({ type, text }) => type === "cmd_name" && text === "echo"),
      );
      assert.ok(
        nodes.some(({ type, text }) => type === "word" && text === "AFTER"),
      );
    }
    const declaration = namedText.indexOf("A");
    const terminator = namedText.indexOf("\tA") + 1;
    const edits = [
      { byte: terminator, deleteBytes: 1, insert: "" },
      { byte: declaration, deleteBytes: 1, insert: "" },
    ];
    const restore = [
      { byte: declaration, deleteBytes: 0, insert: "A" },
      { byte: terminator, deleteBytes: 0, insert: "A" },
    ];
    assertIncrementalEqualsFresh(named, empty, name, ...edits);
    assertIncrementalEqualsFresh(empty, named, name, ...restore);
    assertIncrementalEqualsFresh(named, named, name, ...edits, ...restore);
  });
}

for (const [name, initialWord, finalWord, initialType, finalType] of [
  [
    "file redirect to here-document",
    "<~END",
    "<<~END",
    "tilde_expansion",
    "here_end_text",
  ],
  ["tab stripping enabled", "<<END", "<<-END", "dless", "dlessdash"],
]) {
  test(`${name}: redirect edits update delimiter ownership and restore fresh CSTs`, () => {
    const initialText = `cat ${initialWord}\n\tEND\necho AFTER\n`;
    const finalText = `cat ${finalWord}\n\tEND\necho AFTER\n`;
    const initial = writeSource(`${name}-before`, initialText);
    const final = writeSource(`${name}-after`, finalText);
    for (const [file, text, present, absent] of [
      [initial, initialText, initialType, finalType],
      [final, finalText, finalType, initialType],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      assert.equal(nodes.filter(({ type }) => type === present).length, 1);
      assert.equal(nodes.filter(({ type }) => type === absent).length, 0);
    }
    const edit = {
      byte: 4,
      deleteBytes: initialWord.length,
      insert: finalWord,
    };
    const restore = {
      byte: 4,
      deleteBytes: finalWord.length,
      insert: initialWord,
    };
    assertIncrementalEqualsFresh(initial, final, name, edit);
    assertIncrementalEqualsFresh(final, initial, name, restore);
    assertIncrementalEqualsFresh(initial, initial, name, edit, restore);
  });
}

for (const [name, prefix, suffix] of [
  ["top-level", "", ""],
  ["substitution", "echo $(\n", ")\n"],
  ["backquote", "echo `\n", "`\n"],
]) {
  test(`${name}: quoting the first queued delimiter suppresses only its own expansions`, () => {
    const body =
      "\n$v $(printf first) $((1+2))\nA\n$v $(printf second)\nB\necho AFTER\n";
    const initialText = `${prefix}cat <<A <<B${body}${suffix}`;
    const finalText = `${prefix}cat <<'A' <<B${body}${suffix}`;
    const initial = writeSource("queued-expanded", initialText);
    const final = writeSource("queued-quoted", finalText);
    for (const [file, text, quoted] of [
      [initial, initialText, false],
      [final, finalText, true],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      assert.deepEqual(
        nodes
          .filter(({ type }) => type === "here_document_end_text")
          .map(({ text }) => text),
        ["A", "B"],
      );
      assert.equal(
        nodes.filter(({ type }) => type === "quoted_here_document_body").length,
        quoted ? 1 : 0,
      );
      assert.equal(
        nodes.filter(({ type }) => type === "here_document_body").length,
        quoted ? 1 : 2,
      );
      assert.equal(
        nodes.filter(({ type }) => type === "arithmetic_expansion").length,
        quoted ? 0 : 1,
      );
      assert.equal(
        nodes.filter(
          ({ type, text }) => type === "cmd_name" && text === "printf",
        ).length,
        quoted ? 1 : 2,
      );
      assert.ok(
        nodes.some(({ type, text }) => type === "word" && text === "AFTER"),
      );
    }
    const byte = Buffer.byteLength(prefix) + 6;
    const edit = { byte, deleteBytes: 1, insert: "'A'" };
    const restore = { byte, deleteBytes: 3, insert: "A" };
    assertIncrementalEqualsFresh(initial, final, name, edit);
    assertIncrementalEqualsFresh(final, initial, name, restore);
    assertIncrementalEqualsFresh(initial, initial, name, edit, restore);
  });
}

for (const { name, beforeWord, afterWord, beforeEnd, afterEnd } of [
  {
    name: "parameter operand single quotes survive outer double quotes",
    beforeWord: `"\${x:-"ab"}"`,
    afterWord: `"\${x:-'ab'}"`,
    beforeEnd: `\${x:-ab}`,
    afterEnd: `\${x:-'ab'}`,
  },
  {
    name: "command delimiter outer quotes can be removed",
    beforeWord: "\"$(printf 'ab')\"",
    afterWord: "$(printf 'ab')",
    beforeEnd: "$(printf ab)",
    afterEnd: "$(printf ab)",
  },
  {
    name: "command delimiter inner quote kind leaves matching unchanged",
    beforeWord: "\"$(printf 'ab')\"",
    afterWord: '"$(printf "ab")"',
    beforeEnd: "$(printf ab)",
    afterEnd: "$(printf ab)",
  },
  {
    name: "a quoted suffix extends a command-shaped delimiter",
    beforeWord: "\"$(printf 'ab')\"",
    afterWord: "$(printf 'ab')\"tail\"",
    beforeEnd: "$(printf ab)",
    afterEnd: "$(printf ab)tail",
  },
]) {
  test(`${name}: coordinated declaration and terminator edits preserve quoted body ownership`, () => {
    const beforeText = `cat <<${beforeWord}\n$literal\n${beforeEnd}\necho AFTER\n`;
    const afterText = `cat <<${afterWord}\n$literal\n${afterEnd}\necho AFTER\n`;
    const before = writeSource(`${name}-before`, beforeText);
    const after = writeSource(`${name}-after`, afterText);
    for (const [file, text, word, end] of [
      [before, beforeText, beforeWord, beforeEnd],
      [after, afterText, afterWord, afterEnd],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      for (const [type, expected] of [
        ["here_end_text", [word]],
        ["here_document_end_text", [end]],
        ["quoted_here_document_body", ["$literal\n"]],
        ["cmd_name", ["cat", "echo"]],
        ["parameter_expansion", []],
        ["command_substitution", []],
      ]) {
        assert.deepEqual(
          nodes.filter((node) => node.type === type).map(({ text }) => text),
          expected,
          `${name}: ${type}`,
        );
      }
    }
    const edits = [
      {
        byte: Buffer.byteLength(`cat <<${beforeWord}\n$literal\n`),
        deleteBytes: Buffer.byteLength(beforeEnd),
        insert: afterEnd,
      },
      {
        byte: 6,
        deleteBytes: Buffer.byteLength(beforeWord),
        insert: afterWord,
      },
    ];
    const restore = [
      {
        byte: Buffer.byteLength(`cat <<${afterWord}\n$literal\n`),
        deleteBytes: Buffer.byteLength(afterEnd),
        insert: beforeEnd,
      },
      {
        byte: 6,
        deleteBytes: Buffer.byteLength(afterWord),
        insert: beforeWord,
      },
    ];
    assertIncrementalEqualsFresh(before, after, name, ...edits);
    assertIncrementalEqualsFresh(after, before, name, ...restore);
    assertIncrementalEqualsFresh(before, before, name, ...edits, ...restore);
  });
}

for (const { name, beforeWord, afterWord, beforeEnd, afterEnd } of [
  {
    name: "hexadecimal escape digit limit",
    beforeWord: "$'\\x41'",
    afterWord: "$'\\x414'",
    beforeEnd: "A",
    afterEnd: "A4",
  },
  {
    name: "octal escape digit limit",
    beforeWord: "$'\\101'",
    afterWord: "$'\\1012'",
    beforeEnd: "A",
    afterEnd: "A2",
  },
  {
    name: "short Unicode escape digit limit",
    beforeWord: "$'\\u0041'",
    afterWord: "$'\\u00412'",
    beforeEnd: "A",
    afterEnd: "A2",
  },
  {
    name: "long Unicode escape digit limit",
    beforeWord: "$'\\U00000041'",
    afterWord: "$'\\U000000412'",
    beforeEnd: "A",
    afterEnd: "A2",
  },
  {
    name: "NUL escape truncates the delimiter value",
    beforeWord: "$'A\\0ignored'",
    afterWord: "$'A\\102ignored'",
    beforeEnd: "A",
    afterEnd: "ABignored",
  },
  {
    name: "escaped Unicode changes terminator byte width",
    beforeWord: "$'\\u00e9'",
    afterWord: "$'\\U0001f600'",
    beforeEnd: "é",
    afterEnd: "😀",
  },
]) {
  test(`${name}: delimiter value edits retain quoted spelling and the following document`, () => {
    const body = "\n$v\n";
    const tail = "\n$second\nB\necho AFTER\n";
    const beforeText = `cat <<${beforeWord} <<'B'${body}${beforeEnd}${tail}`;
    const afterText = `cat <<${afterWord} <<'B'${body}${afterEnd}${tail}`;
    const before = writeSource(`${name}-before`, beforeText);
    const after = writeSource(`${name}-after`, afterText);
    for (const [file, text, word, end] of [
      [before, beforeText, beforeWord, beforeEnd],
      [after, afterText, afterWord, afterEnd],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      for (const [type, expected] of [
        ["here_end_text", [word, "'B'"]],
        ["here_document_end_text", [end, "B"]],
        ["quoted_here_document_body", ["$v\n", "$second\n"]],
        ["cmd_name", ["cat", "echo"]],
        ["parameter_expansion", []],
      ]) {
        assert.deepEqual(
          nodes.filter((node) => node.type === type).map(({ text }) => text),
          expected,
          `${name}: ${type}`,
        );
      }
      const firstEnd = nodes.find(
        ({ type }) => type === "here_document_end_text",
      );
      assert.ok(firstEnd);
      assert.equal(firstEnd.end - firstEnd.start, Buffer.byteLength(end));
      const declaration = nodes.find(({ type }) => type === "here_end_text");
      assert.ok(declaration);
      assert.equal(
        declaration.end - declaration.start,
        Buffer.byteLength(word),
      );
    }
    const edits = [
      {
        byte: Buffer.byteLength(`cat <<${beforeWord} <<'B'${body}`),
        deleteBytes: Buffer.byteLength(beforeEnd),
        insert: afterEnd,
      },
      {
        byte: 6,
        deleteBytes: Buffer.byteLength(beforeWord),
        insert: afterWord,
      },
    ];
    const restore = [
      {
        byte: Buffer.byteLength(`cat <<${afterWord} <<'B'${body}`),
        deleteBytes: Buffer.byteLength(afterEnd),
        insert: beforeEnd,
      },
      {
        byte: 6,
        deleteBytes: Buffer.byteLength(afterWord),
        insert: beforeWord,
      },
    ];
    assertIncrementalEqualsFresh(before, after, `${name}: edit`, ...edits);
    assertIncrementalEqualsFresh(after, before, `${name}: restore`, ...restore);
    assertIncrementalEqualsFresh(
      before,
      before,
      `${name}: roundtrip`,
      ...edits,
      ...restore,
    );
  });
}

for (const [name, innerWord, innerBodyType] of [
  ["expanded inner document", "INNER", "here_document_body"],
  ["quoted inner document", "'INNER'", "quoted_here_document_body"],
]) {
  test(`${name}: inserting an outer delimiter into the inner body preserves document boundaries`, () => {
    const prefix = `cat <<OUTER\n$(cat <<${innerWord}\n`;
    const middle = "INNER\n)\nOUTER\necho AFTER\n";
    const initialText = `${prefix}$value\n${middle}`;
    const finalText = `${prefix}$value\nOUTER\n${middle}`;
    const initial = writeSource(`${name}-initial`, initialText);
    const final = writeSource(`${name}-final`, finalText);
    for (const [file, text, expectedBody] of [
      [initial, initialText, "$value\n"],
      [final, finalText, "$value\nOUTER\n"],
    ]) {
      const nodes = sourceNodes(parseValidCst(file), text);
      assert.deepEqual(
        nodes
          .filter(({ type }) => type === "here_document_end_text")
          .map(({ text }) => text),
        ["INNER", "OUTER"],
      );
      assert.ok(
        nodes.some(
          ({ type, text }) => type === innerBodyType && text === expectedBody,
        ),
      );
      assert.deepEqual(
        nodes.filter(({ type }) => type === "cmd_name").map(({ text }) => text),
        ["cat", "cat", "echo"],
      );
      assert.equal(
        nodes.filter(({ type }) => type === "parameter_expansion").length,
        innerWord === "INNER" ? 1 : 0,
      );
    }
    const byte = Buffer.byteLength(`${prefix}$value\n`);
    const insert = { byte, deleteBytes: 0, insert: "OUTER\n" };
    const remove = { byte, deleteBytes: 6, insert: "" };
    assertIncrementalEqualsFresh(initial, final, name, insert);
    assertIncrementalEqualsFresh(final, initial, name, remove);
    assertIncrementalEqualsFresh(initial, initial, name, insert, remove);
  });
}
