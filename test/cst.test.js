import assert from "node:assert/strict";
import { readdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { nodeIdentity, parseCst, sourceNodes } from "./support/cst.js";
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
