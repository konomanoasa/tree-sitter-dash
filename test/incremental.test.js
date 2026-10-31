import assert from "node:assert/strict";
import { test } from "node:test";
import {
  applyEdits,
  assertIncrementalEqualsFresh,
  runParse,
  writeSource,
} from "./support/parser.js";
import { editingWorkloads } from "./support/workloads.js";

test("NUL removal preserves incremental CSTs across source contexts", () => {
  for (const contents of [
    "if :; then :; fi\n",
    "cat <<'E'\nbody\nE\n",
    "cat <<E\nbody\n",
    ": a\\\nb\n",
  ]) {
    const decorated = `\0${Array.from(contents).join("\0")}\0`;
    const initial = writeSource("nul-edit-initial", decorated);
    const final = writeSource("nul-edit-final", contents);
    const remove = {
      byte: 0,
      deleteBytes: Buffer.byteLength(decorated),
      insert: contents,
    };
    assertIncrementalEqualsFresh(initial, final, contents, remove);
  }
});

for (const [name, source, removed, inserted] of [
  ["here delimiter", "cat <<E\nbody\nE\n", "E", "END"],
  ["here quote", "cat <<E\n$v\nE\n", "<<E", "<<'E'"],
  ["here terminator", "cat <<E\nbody\nE\n", "body\nE", "body"],
  [
    "here multiline delimiter",
    'cat <<"a\nb"\nbody\na\nb\n',
    '"a\nb"',
    '"c\nd"',
  ],
  [
    "here multiline terminator",
    'cat <<"a\nb"\nbody\na\nb\n',
    "body\na\nb",
    "body\na\nX",
  ],
  [
    "here multiline tab",
    'cat <<-"a\n\tb"\nbody\n\ta\n\tb\n',
    "body\n\ta\n\tb",
    "body\n\ta\nb",
  ],
  [
    "here newline-only delimiter",
    "cat <<$'\\n'\nbody\n\n\n",
    "$'\\n'",
    "$'a\\n'",
  ],
  ["here continuation", "cat <<E\nE\n", "\nE", "\nE\\\n"],
  ["here nested", "cat <<E\n$(printf x)\nE\n", "x", "'a\nE\nb'"],
  ["here empty delimiter", "cat <<E\nbody\nE\n", "<<E", "<<''"],
  ["here literal command", "cat <<'$(if)'\nbody\n$(if)\n", "if", "a|"],
  ["here EOF backslash", "cat <<E", "E", "E\\"],
  ["here arithmetic escape", "cat <<$((1))", "1", "\\)"],
  ["here quoted backquote", "cat <<`'ab'`", "ab", "a`b"],
  ["here unfinished double quote", 'cat <<`"x`\n$v\n`x`\n', '"x', '"x"'],
  ["here earlier quote", 'cat <<""`"x`\n$v\n`x`\n', '""', ""],
  ["here parameter prefix", `cat <<\${a}`, `\${a}`, `\${a'}`],
  ["parameter value operator", `: \${a:-word}\n: after\n`, ":-", ":+"],
  ["parameter operator continuation", `: \${a:-word}\n`, ":-", ":\\\n-"],
  ["parameter operand lookahead", `: $((\${a:-2}))\n: after\n`, "2", "')'"],
  [
    "here multibyte parameter prefix",
    `cat <<"\${é}"\nBODY\n\${}\nprintf AFTER\n`,
    "é",
    "'",
  ],
  [
    "here multibyte length prefix",
    `cat <<"\${#é}"\nBODY\n\${#é}\nprintf AFTER\n`,
    "é",
    "😀",
  ],
  ["backquote remainder repair", ': `printf x) ignored "`\n', ")", ""],
  ["backquote remainder separator", ": `printf x)ignored`\n", ")", ";"],
  ["backquote extra word repair", ': `(printf x) ignored "`\n', "ignored", ""],
  ["backquote extra word", ': `(printf x) ignored "`\n', "ignored", "'word'"],
  ["backquote here boundary", ": `cat <<E)E\n`\n", ")", ";"],
  ["backquote pending EOF", ": `cat <<E)`\n", ")", ""],
  [
    "backquote pending quoted body",
    ": `cat <<A)$(printf X)\n`\n",
    "<<A",
    "<<'A'",
  ],
  [
    "backquote pending first substitution",
    ": `cat <<A)$(:)$(printf X)\n`\n",
    "$(:)",
    "",
  ],
  [
    "backquote pending arithmetic",
    ": `cat <<A)$((1))$(printf X)\n`\n",
    "$((1))",
    "$(first)",
  ],
  [
    "backquote pending nested parameter",
    `: \`cat <<A)\${v:-$(first)}$(second)\n\`\n`,
    "$(first)",
    "literal",
  ],
  ["backquote pending newline", ": `cat <<A)$(printf X)\n`\n", ")$", "\n$"],
  ["word", "printf alpha\n", "alpha", "beta"],
  ["descriptor", ": 9>out\n", "9", "10"],
  ["function body", "f() printf x\n", "printf", "echo"],
  [
    "function close after Unicode content",
    "f() { printf '終😀'; }\n: after\n",
    "}",
    "",
  ],
  ["if keyword", "if :; then :; fi\n: after\n", "then", ""],
  ["for header newline", "for x; do :; done\n", ";", "\n;"],
  [
    "for header comments",
    "for x\n; do :; done\n",
    "\n",
    " # header\n\n# gap\n",
  ],
  ["continued keyword", "i\\\nf :; then :; fi\n", "i\\\nf", "if"],
  ["UTF-8 word", "printf 日本語\n", "日本語", "文字列"],
  ["arithmetic evaluation", ": $((1+2))\n", "2", "@"],
  ["arithmetic substitution", ": $((1+2))\n", "2", `\${v:-3}`],
  ["arithmetic empty", ": $((1))\n", "1", " "],
  ["arithmetic close", ": $((1+2))\n", "))", ""],
  ["declaration name", "export x=~\n", "export", "printf"],
  ["quoted declaration", "export x=~\n", "export", "'export'"],
  ["forwarding option", "command -- export x=~\n", "--", "-v"],
  ["pattern negation", ": [!a]\n", "!", "^"],
]) {
  test(`${name}: edits and repairs match fresh parsing`, () => {
    const offset = source.indexOf(removed);
    assert.notEqual(offset, -1, name);
    const byte = Buffer.byteLength(source.slice(0, offset));
    const initial = writeSource(`${name}-initial`, source);
    const edit = {
      byte,
      deleteBytes: Buffer.byteLength(removed),
      insert: inserted,
    };
    const repair = {
      byte,
      deleteBytes: Buffer.byteLength(inserted),
      insert: removed,
    };
    assertIncrementalEqualsFresh(initial, initial, name, edit, repair);
    if (inserted) {
      const final =
        source.slice(0, offset) +
        inserted +
        source.slice(offset + removed.length);
      assertIncrementalEqualsFresh(
        initial,
        writeSource(`${name}-final`, final),
        name,
        edit,
      );
    }
  });
}

for (const { name, source: contents, edits, recovery } of editingWorkloads()) {
  test(`dash: ${name} preserves structure after editing`, () => {
    const source = writeSource(`${name}-initial`, contents);
    const final = writeSource(`${name}-edited`, applyEdits(contents, edits));
    if (recovery) {
      runParse({ source, edits, expectedSource: final, mode: "recovery" });
    } else {
      assertIncrementalEqualsFresh(source, final, name, ...edits);
    }
  });
}

test("here-document layout roles stay consistent after inserting and removing a terminator", () => {
  const sources = [
    "<<ZZQ\n$(<<E\nE\nuntil(z);do>f;x&(<<'E'\nE\nf ) done)\nZZQ\n",
    "<<OUT\n$(<<E\nE\nprintf x)\nOUT\n",
    "cat <<OUT\n$(if :; then printf x; fi)\nOUT\n",
    "<<'OUT'\n$(if :; then printf x; fi)\nOUT\n",
    "<<-OUT\n\t$(printf x)\n\tOUT\n",
    "<<A <<B\n$(printf first)\nA\n$(printf second)\nB\n",
    "<<OUT\n$(printf x)\nOUT\nprintf after\n",
    "<<OUT\n$(printf x)\nOUT\n# trailing comment\n",
    "echo $(<<OUT\n$(printf x)\nOUT\n)\n",
  ];
  for (const [index, source] of sources.entries()) {
    const byte = source.indexOf("\n");
    const modified = `${source.slice(0, byte)};${source.slice(byte)}`;
    const before = writeSource(`document-role-${index}-before`, source);
    const after = writeSource(`document-role-${index}-after`, modified);
    const insert = { byte, deleteBytes: 0, insert: ";" };
    const remove = { byte, deleteBytes: 1, insert: "" };
    assertIncrementalEqualsFresh(
      before,
      after,
      `insert terminator ${index}`,
      insert,
    );
    assertIncrementalEqualsFresh(
      after,
      before,
      `remove terminator ${index}`,
      remove,
    );
    assertIncrementalEqualsFresh(
      before,
      before,
      `roundtrip terminator ${index}`,
      insert,
      remove,
    );
  }
});
