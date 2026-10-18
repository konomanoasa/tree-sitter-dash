import assert from "node:assert/strict";
import { test } from "node:test";
import { corpusCases } from "./support/corpus.js";
import { nodeIdentity, parseCst, sourceNodes } from "./support/cst.js";
import {
  assertRepeatedColdParse,
  parseValidCst,
  writeSource,
} from "./support/parser.js";

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

test("valid corpus trees preserve source ranges and lexical boundaries", () => {
  for (const { name, source, mode } of corpusCases()) {
    if (mode === "valid") parseValidCst(writeSource(name, source), name);
  }
});

test("fresh recovery is deterministic across malformed corpus inputs", () => {
  for (const { name, source, mode } of corpusCases()) {
    if (mode === "recovery")
      assertRepeatedColdParse(mode, writeSource(name, source), name);
  }
});
