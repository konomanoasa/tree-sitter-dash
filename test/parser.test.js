import assert from "node:assert/strict";
import { test } from "node:test";
import { hasRecovery, sourceNodes } from "./support/cst.js";
import {
  assertIncrementalEqualsFresh,
  parseValidCst,
  runParse,
  writeSource,
} from "./support/parser.js";

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
