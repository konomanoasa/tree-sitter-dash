import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import {
  assertCstOutputsEqual,
  assertCstSourceContract,
  assertFreshOutputsEqual,
  cstFingerprint,
  hasRecovery,
  nodeIdentity,
  parseCst,
  sourceNodes,
  sourcePoint,
} from "./support/cst.js";
import {
  assertRepeatedColdParse,
  parseValidCst,
  writeSource,
} from "./support/parser.js";
import { scaleFactors, scalingWorkloads } from "./support/workloads.js";

test("NUL insertion preserves syntax across lexical and here-document boundaries", () => {
  for (const source of [
    "if :; then :; fi\n",
    ": a && : b || : c\n",
    `: \${v:-word} $((1+2)) \`: a\`\n`,
    ": 'a b' \"a b\" $'ab'\n",
    "cat <<END\na\\\nb\nEND\n",
    "cat <<-'END'\n\tbody\n\tEND\n",
    ": ab\\\ncd\n",
    "cat <<''\n",
    ': `printf x) $v "`\n',
    ': `(printf x) ignored "`\n',
    ": `cat <<E)E\n`\n",
  ]) {
    const decorated = `\0${Array.from(source).join("\0")}\0`;
    const projection = (output, text) =>
      sourceNodes(output, text).map(({ type, field, text }) => ({
        type,
        field,
        text: text.replaceAll("\0", ""),
      }));
    const original = parseValidCst(writeSource("plain", source));
    const withNul = parseValidCst(writeSource("nul", decorated));
    assert.deepEqual(
      projection(withNul, decorated),
      projection(original, source),
      source,
    );
  }
});

test("NUL-only input and long ignored runs retain complete source coverage", () => {
  const ignoredNulBytes = 4096;
  const commandCount = 256;
  for (const source of [
    "",
    "\0",
    "\0".repeat(ignoredNulBytes),
    `\0${": a\0b\n".repeat(commandCount)}\0`,
  ]) {
    parseValidCst(writeSource("nul-run", source));
  }
});

test("one leading BOM is excluded and later U+FEFF remains word source", () => {
  for (const source of [": text\n", "# comment\n", "cat <<E\nbody\nE\n"]) {
    const ordinary = parseValidCst(writeSource("without-bom", source));
    const prefixed = parseValidCst(writeSource("with-bom", `\uFEFF${source}`));
    const structure = (output) => parseCst(output).map(nodeIdentity);
    assert.deepEqual(structure(prefixed), structure(ordinary));
  }
  const source = ": \uFEFFx\n";
  const output = parseValidCst(writeSource("interior-bom", source));
  assert.ok(
    sourceNodes(output, source).some(
      ({ type, text, start, end }) =>
        type === "literal" && text === "\uFEFFx" && start === 2 && end === 6,
    ),
  );
});

const corpusDirectory = join(import.meta.dirname, "corpus");
const corpusHeader = "==================\n";
const corpusSeparator = "\n------------------\n";

function corpusCases() {
  return readdirSync(corpusDirectory)
    .sort()
    .flatMap((file) => {
      const parts = readFileSync(join(corpusDirectory, file), "utf8").split(
        corpusHeader,
      );
      assert.equal(parts.shift(), "", file);
      assert.equal(parts.length % 2, 0, file);
      const cases = [];
      for (let index = 0; index < parts.length; index += 2) {
        const [name, ...attributes] = parts[index].trimEnd().split("\n");
        assert.ok(
          attributes.every(
            (attribute) => attribute === ":cst" || attribute === ":error",
          ),
          name,
        );
        const end = parts[index + 1].indexOf(corpusSeparator);
        assert.notEqual(end, -1, name);
        cases.push({
          name: `${file}: ${name}`,
          source: parts[index + 1].slice(0, end),
          mode: attributes.includes(":error") ? "recovery" : "valid",
        });
      }
      return cases;
    });
}

for (const { name, source, mode } of corpusCases()) {
  if (mode === "valid") {
    test(`corpus source ranges and lexical boundaries: ${name}`, () => {
      parseValidCst(writeSource(name, source), name);
    });
  } else {
    test(`fresh recovery is deterministic: ${name}`, () => {
      assertRepeatedColdParse(mode, writeSource(name, source), name);
    });
  }
}

const substitutionDepth = 128;
const wordBytes = 262_144;
const unfinishedLoopCount = 500;
const pendingDocumentCount = 256;
for (const [name, contents] of [
  [
    "nested substitutions",
    `: ${"$(".repeat(substitutionDepth)}printf value${")".repeat(substitutionDepth)}\n`,
  ],
  ["long word", `: ${"x".repeat(wordBytes)}\n`],
  ["unfinished loops", "for x; do :\n".repeat(unfinishedLoopCount)],
  [
    "pending documents",
    `cat${" <<E".repeat(pendingDocumentCount)}\n${"body\nE\n".repeat(pendingDocumentCount)}`,
  ],
]) {
  test(`limit input completes with deterministic recovery: ${name}`, () => {
    assertRepeatedColdParse("resource", writeSource(name, contents), name);
  });
}

for (const { name, units, source } of scalingWorkloads) {
  for (const factor of scaleFactors) {
    test(`${name} preserves source through EOF at scale ${factor}`, () => {
      parseValidCst(writeSource(`${name}-${factor}`, source(units * factor)));
    });
  }
}

test("CST comparison detects changed punctuation, fields, hierarchy, ranges and cardinality", () => {
  const original = [
    "0:0 - 0:3   program",
    "0:0 - 0:3     body: list",
    "0:0 - 0:1       word `a`",
    '0:1 - 0:2       ";"',
    "0:2 - 0:3       word `b`",
  ].join("\n");
  for (const [name, changed] of [
    ["punctuation", original.replace('";"', '"&"')],
    ["field", original.replace("body: list", "commands: list")],
    ["hierarchy", original.replace("       word `b`", "     word `b`")],
    ["range", original.replace("0:2 - 0:3", "0:1 - 0:3")],
    ["duplicate node", `${original}\n0:2 - 0:3       word`],
    ["recovery", original.replace("       word `b`", "      •ERROR")],
  ]) {
    assert.notEqual(cstFingerprint(changed), cstFingerprint(original), name);
    assert.throws(
      () =>
        assertCstOutputsEqual(
          name,
          { status: 0, output: original },
          { status: 0, output: changed },
        ),
      /public CST contracts differ/,
    );
  }
  assert.throws(
    () =>
      assertCstOutputsEqual(
        "exit status",
        { status: 0, output: original },
        { status: 1, output: original },
      ),
    /parse statuses differ/,
  );
  assert.equal(
    cstFingerprint(original.replace("word `a`", "word")),
    cstFingerprint(original),
  );
  assert.throws(
    () =>
      assertFreshOutputsEqual(
        "fresh rendering",
        { status: 0, output: original },
        { status: 0, output: original.replace("word `a`", "word") },
      ),
    /fresh CST is not deterministic/,
  );
});

test("continuation comparison ignores ownership but retains every range and its order", () => {
  const root = "0:0 - 2:3   program";
  const word = "0:0 - 2:3     word";
  const first = "0:3 - 0:4       line_continuation";
  const second = "1:0 - 1:1       line_continuation";
  const original = [root, word, first, second].join("\n");
  const relocated = [
    root,
    word,
    first.replace("       ", "     leading: "),
    second,
  ].join("\n");
  assert.equal(cstFingerprint(original), cstFingerprint(relocated));
  for (const { name, changed } of [
    { name: "missing", changed: [root, word, first] },
    { name: "duplicate", changed: [root, word, first, first, second] },
    { name: "reordered", changed: [root, word, second, first] },
    {
      name: "extended",
      changed: [root, word, first, second.replace("1:1", "1:2")],
    },
  ]) {
    assert.notEqual(
      cstFingerprint(original),
      cstFingerprint(changed.join("\n")),
      name,
    );
  }
});

test("CST decoding distinguishes multiline display fragments and literal recovery bullets", () => {
  const output = [
    "0:0 - 1:3   program",
    "0:0 - 1:3     single_quote_content `a",
    "1:0 - 1:3       `•`",
  ].join("\n");
  assert.deepEqual(parseCst(output).map(nodeIdentity), [
    { type: "program", field: undefined, recovery: false },
    { type: "single_quote_content", field: undefined, recovery: false },
  ]);
  assert.equal(hasRecovery(output), false);
  assert.equal(hasRecovery("0:0 - 0:3   literal `•`"), false);
  assert.equal(hasRecovery("0:0 - 0:3  •ERROR"), true);
  const separators = "0:0 - 0:9   word: literal `a\u2028b\u2029c`";
  assert.deepEqual(nodeIdentity(parseCst(separators)[0]), {
    type: "literal",
    field: "word",
    recovery: false,
  });
  assert.equal(
    sourceNodes(separators, "a\u2028b\u2029c")[0].text,
    "a\u2028b\u2029c",
  );
});

test("source contract rejects malformed lexical children and continuation byte ranges", () => {
  const source = "foo\\\nbar";
  const original = [
    "0:0 - 1:3   program",
    "0:0 - 1:3     literal",
    "0:3 - 0:4       line_continuation",
  ].join("\n");
  assertCstSourceContract(original, source);
  for (const [name, output, message] of [
    [
      "multibyte continuation",
      original.replace("0:3 - 0:4", "0:3 - 1:0"),
      /continuation is not one byte/,
    ],
    [
      "duplicate continuation",
      `${original}\n0:3 - 0:4       line_continuation`,
      /duplicated or out of order/,
    ],
    [
      "ordinary lexical child",
      `${original}\n0:0 - 0:3       literal`,
      /internal lexical child/,
    ],
    [
      "continuation child",
      `${original}\n0:3 - 0:4         word`,
      /continuation has a child/,
    ],
    ["reversed range", "0:2 - 0:1   word", /reversed source range/],
    ["invalid row", "2:0 - 2:0   word", /point exceeds source/],
    ["invalid column", "0:0 - 0:5   word", /column exceeds source/],
    [
      "child outside parent",
      "0:0 - 0:1   word\n0:0 - 0:2     word",
      /child exceeds parent/,
    ],
    ["wrong punctuation", '0:0 - 0:1   ";"', /anonymous source differs/],
    [
      "wrong continuation byte",
      original.replace("0:3 - 0:4", "0:2 - 0:3"),
      /continuation is not a backslash/,
    ],
  ]) {
    assert.throws(
      () => assertCstSourceContract(output, source),
      { message },
      String(name),
    );
  }
  assert.throws(
    () => assertCstSourceContract(original, "foo\\xbar"),
    /point exceeds source|column exceeds source|continuation has no physical newline/,
  );
});

test("UTF-8 source points count bytes while NUL-separated continuations remain one byte", () => {
  const source = "é😀\\\0\n終";
  for (const [byte, point] of [
    [0, "0:0"],
    [2, "0:2"],
    [6, "0:6"],
    [8, "0:8"],
    [9, "1:0"],
    [12, "1:3"],
  ]) {
    assert.equal(sourcePoint(source, byte), point);
  }
  const output = [
    "0:0 - 1:3   program",
    "0:0 - 1:3     literal",
    "0:6 - 0:7       line_continuation",
  ].join("\n");
  assertCstSourceContract(output, source);
  const nodes = sourceNodes(output, source);
  assert.deepEqual(
    nodes.map(({ text, start, end }) => [text, start, end]),
    [
      [source, 0, 12],
      [source, 0, 12],
      ["\\", 6, 7],
    ],
  );
  assert.throws(
    () => assertCstSourceContract(`${output}\n0:8 - 1:0     newline`, source),
    /removed newline is public/,
  );
});
