import assert from "node:assert/strict";
import { test } from "node:test";
import { sourceNodes } from "./support/cst.js";
import {
  assertIncrementalEqualsFresh,
  assertRepeatedColdParse,
  runParse,
  writeSource,
} from "./support/parser.js";

const recoveryCases = [
  ["backquote pending token", ": `cat <<A)$(printf X)\n`\n", ")$", ";;$"],
  ["parameter colon before close", `: \${a:-}\n`, ":-", ":"],
  ["unquoted parameter", `echo \${x:-value}\n`, ":-", "!"],
  ["quoted parameter", `echo "\${x:-value}"\n`, ":-", "!"],
  ["nested parameter", `echo \${outer:-\${x:-value}}\n`, "x:-", "x!"],
  ["quoted substitution", `echo "$(echo \${x:-value})"\n`, ":-", "!"],
  ["here parameter", `cat <<END\n\${x:-value}\nEND\n`, ":-", "!"],
  [
    "here substitution parameter",
    `cat <<END\n$(echo \${x:-value})\nEND\n`,
    ":-",
    "!",
  ],
];
for (const [name, opening, closing] of [
  ["arithmetic", "echo ", "\n"],
  ["quoted arithmetic", 'echo "', '"\n'],
  ["here arithmetic", "cat <<END\n", "\nEND\n"],
  ["function arithmetic", "f() { echo ", "; }\n"],
  ["command arithmetic", "echo $(echo ", ")\n"],
  ["backquote arithmetic", "echo `echo ", "`\n"],
]) {
  for (const depth of [1, 4]) {
    const body = `${opening}${"$((1+".repeat(depth)}\${x:-1}${"))".repeat(depth)}${closing}`;
    for (const [removed, inserted] of [
      [":-", "!"],
      ["}", ""],
    ]) {
      recoveryCases.push([
        `${name} depth ${depth} damaged ${removed}`,
        body,
        removed,
        inserted,
      ]);
    }
  }
}
for (const { name, opening, closing, depth } of [
  {
    name: "here arithmetic open quote",
    opening: "cat <<END\n",
    closing: "\nEND\n",
    depth: 4,
  },
  {
    name: "function arithmetic open quote",
    opening: "f() { echo ",
    closing: "; }\n",
    depth: 1,
  },
]) {
  const parentheses = "))".repeat(depth);
  const parameter = `\${x:-$(echo "a")}`;
  const body = `${opening}${"$((1+".repeat(depth)}${parameter}${parentheses}${closing}`;
  for (const closed of [true, false]) {
    recoveryCases.push([
      `${name} ${closed ? "closed" : "unclosed"}`,
      body,
      `${parameter}${parentheses}`,
      `\${x!$(echo "a)}${closed ? parentheses : ""}`,
    ]);
  }
}
const nestingDepth = 3;
for (const [name, opening, closing] of [
  ["while", "while :; do\n", "done\n"],
  ["until", "until :; do\n", "done\n"],
  ["case", "case x\nin\nx)\n", ";; esac\n"],
  ["assignment", 'x="$(\n', ')"\n'],
  ["here-document", "cat <<END # body\n$(\n", ")\nEND\n"],
  ["word parts", 'echo prefix"quoted"$(\n', ")\n"],
  ["quoted parts", `echo "one\${x:-a}$(\n`, ')"\n'],
  ["initial quoted parts", `"one\${x:-a}$(\n`, ')"\n'],
]) {
  const prefix = opening.repeat(nestingDepth);
  const suffix = closing.repeat(nestingDepth);
  recoveryCases.push([
    `nested ${name}`,
    `${prefix}echo \${x:-value}\n${suffix}`,
    `}\n${suffix}`,
    "\n",
  ]);
}
for (const [name, body, removed, inserted] of recoveryCases) {
  test(`repairs restore parameter contexts and nested structures: ${name}`, () => {
    const contents = `echo BEFORE\n${body}echo AFTER\n`;
    const offset = contents.indexOf(removed);
    assert.notEqual(offset, -1, name);
    const broken =
      contents.slice(0, offset) +
      inserted +
      contents.slice(offset + removed.length);
    const initial = writeSource(`${name}-valid`, contents);
    const damaged = writeSource(`${name}-damaged`, broken);
    assert.equal(
      runParse({ source: damaged, mode: "recovery" }).recovery,
      true,
      name,
    );
    assertRepeatedColdParse("recovery", damaged, name);
    const edit = {
      byte: Buffer.byteLength(contents.slice(0, offset)),
      deleteBytes: Buffer.byteLength(removed),
      insert: inserted,
    };
    const repair = {
      byte: edit.byte,
      deleteBytes: Buffer.byteLength(inserted),
      insert: removed,
    };
    assertIncrementalEqualsFresh(damaged, initial, name, repair);
    assertIncrementalEqualsFresh(initial, initial, name, edit, repair);
  });
}

for (const [name, body, closing] of [
  ["single quote", "echo '日本語", "'"],
  ["double quote", 'echo "日本語', '"'],
  ["dollar single quote", "echo $'\\u0041", "'"],
  ["command substitution", "echo $(printf 日本語", ")"],
  ["backquote", "echo `printf 日本語", "`"],
  ["parameter operand", "echo ${value:-日本語", "}"],
]) {
  test(`unterminated ${name}: closing at EOF restores the full CST after deletion`, () => {
    const prefix = "echo BEFORE\n";
    const brokenText = prefix + body;
    const validText = `${brokenText}${closing}\necho AFTER\n`;
    const valid = writeSource(`${name}-closed`, validText);
    const broken = writeSource(`${name}-open`, brokenText);
    assert.equal(runParse({ source: broken, mode: "recovery" }).recovery, true);
    assertRepeatedColdParse("recovery", broken, name);
    const tail = `${closing}\necho AFTER\n`;
    const byte = Buffer.byteLength(brokenText);
    const remove = { byte, deleteBytes: Buffer.byteLength(tail), insert: "" };
    const repair = { byte, deleteBytes: 0, insert: tail };
    assertIncrementalEqualsFresh(broken, valid, name, repair);
    assertIncrementalEqualsFresh(valid, valid, name, remove, repair);
  });
}

for (const { name, source, removed, inserted, owner } of [
  {
    name: "if condition",
    source: "if CHECK; then :; fi",
    removed: "CHECK",
    inserted: "|",
    owner: "if_clause",
  },
  {
    name: "then body",
    source: "if :; then BODY; fi",
    removed: "BODY",
    inserted: "&&",
    owner: "if_clause",
  },
  {
    name: "else body",
    source: "if :; then :; else BODY; fi",
    removed: "BODY",
    inserted: "|",
    owner: "if_clause",
  },
  {
    name: "elif condition",
    source: "if :; then :; elif CHECK; then :; fi",
    removed: "CHECK",
    inserted: "||",
    owner: "if_clause",
  },
  {
    name: "while condition",
    source: "while CHECK; do :; done",
    removed: "CHECK",
    inserted: "|",
    owner: "while_clause",
  },
  {
    name: "while body",
    source: "while :; do BODY; done",
    removed: "BODY",
    inserted: "&&",
    owner: "while_clause",
  },
  {
    name: "until condition",
    source: "until CHECK; do :; done",
    removed: "CHECK",
    inserted: "||",
    owner: "until_clause",
  },
  {
    name: "until body",
    source: "until :; do BODY; done",
    removed: "BODY",
    inserted: "|",
    owner: "until_clause",
  },
  {
    name: "for body",
    source: "for x in a b; do BODY; done",
    removed: "BODY",
    inserted: "&&",
    owner: "for_clause",
  },
  {
    name: "brace body",
    source: "{ BODY; }",
    removed: "BODY",
    inserted: "|",
    owner: "brace_group",
  },
  {
    name: "subshell body",
    source: "(BODY)",
    removed: "BODY",
    inserted: "&&",
    owner: "subshell",
  },
  {
    name: "case selector keyword",
    source: "case x in a) :;; esac",
    removed: "in",
    inserted: "",
    owner: "case_clause",
  },
  {
    name: "function closing parenthesis",
    source: "f() { :; }",
    removed: ")",
    inserted: "",
    owner: "function_definition",
  },
  {
    name: "pipeline right operand",
    source: "printf x | RIGHT",
    removed: "RIGHT",
    inserted: "|",
    owner: "pipeline",
  },
  {
    name: "and-list right operand",
    source: "printf x && RIGHT",
    removed: "RIGHT",
    inserted: "&&",
    owner: "and_or",
  },
  {
    name: "or-list right operand",
    source: "printf x || RIGHT",
    removed: "RIGHT",
    inserted: "||",
    owner: "and_or",
  },
  {
    name: "output file",
    source: "printf x >TARGET; :",
    removed: "TARGET",
    inserted: "",
    owner: "io_file",
  },
  {
    name: "input file",
    source: "cat <TARGET; :",
    removed: "TARGET",
    inserted: "",
    owner: "io_file",
  },
  {
    name: "descriptor target",
    source: "printf x 2>&TARGET; :",
    removed: "TARGET",
    inserted: "",
    owner: "io_file",
  },
  {
    name: "case pattern separator",
    source: "case x in a) :;; esac",
    removed: ")",
    inserted: "",
    owner: "case_clause",
  },
]) {
  test(`repairing ${name} restores its owner and surrounding commands`, () => {
    const text = `echo BEFORE\n${source}\necho AFTER\n`;
    const offset = text.indexOf(removed);
    assert.notEqual(offset, -1);
    const damagedText =
      text.slice(0, offset) + inserted + text.slice(offset + removed.length);
    const valid = writeSource(`${name}-valid`, text);
    const damaged = writeSource(`${name}-damaged`, damagedText);
    const edit = {
      byte: Buffer.byteLength(text.slice(0, offset)),
      deleteBytes: Buffer.byteLength(removed),
      insert: inserted,
    };
    const repair = {
      byte: edit.byte,
      deleteBytes: Buffer.byteLength(inserted),
      insert: removed,
    };
    assert.equal(
      runParse({ source: damaged, mode: "recovery" }).recovery,
      true,
      name,
    );
    assert.equal(
      runParse({
        source: valid,
        edits: [edit],
        expectedSource: damaged,
        mode: "recovery",
      }).recovery,
      true,
      name,
    );
    assertRepeatedColdParse("recovery", damaged, name);
    assertIncrementalEqualsFresh(damaged, valid, `${name}: repair`, repair);
    assertIncrementalEqualsFresh(
      valid,
      valid,
      `${name}: roundtrip`,
      edit,
      repair,
    );
    const nodes = sourceNodes(runParse({ source: valid }).output, text);
    assert.ok(
      nodes.some(({ type }) => type === owner),
      `${name}: ${owner}`,
    );
    for (const sentinel of ["BEFORE", "AFTER"]) {
      assert.ok(
        nodes.some(({ type, text }) => type === "word" && text === sentinel),
        `${name}: ${sentinel}`,
      );
    }
  });
}

for (const [name, prefix, suffix, closing] of [
  ["conditional", "if :; then\n", "fi\n", "fi"],
  ["while", "while :; do\n", "done\n", "done"],
  ["until", "until :; do\n", "done\n", "done"],
  ["for", "for x in a; do\n", "done\n", "done"],
  ["case", "case x in a)\n", ";; esac\n", "esac"],
]) {
  test(`a continued ${name} closing keyword survives body damage and repair`, () => {
    const continued = `${closing[0]}\\\n${closing.slice(1)}`;
    const text = `echo BEFORE\n${prefix}printf 日本語\n${suffix.replace(closing, continued)}echo AFTER\n`;
    const initial = writeSource(`${name}-continued`, text);
    const byte = Buffer.byteLength(text.slice(0, text.indexOf("printf")));
    const removed = "printf 日本語";
    const brokenText = text.replace(removed, "&&");
    const broken = writeSource(`${name}-continued-broken`, brokenText);
    const damage = {
      byte,
      deleteBytes: Buffer.byteLength(removed),
      insert: "&&",
    };
    const repair = { byte, deleteBytes: 2, insert: removed };
    assert.equal(runParse({ source: broken, mode: "recovery" }).recovery, true);
    assertIncrementalEqualsFresh(broken, initial, name, repair);
    assertIncrementalEqualsFresh(initial, initial, name, damage, repair);
    const nodes = sourceNodes(runParse({ source: initial }).output, text);
    const keyword = nodes.find(({ type }) => type === `${closing}_keyword`);
    assert.ok(keyword);
    assert.equal(keyword.text, continued);
    const continuation = nodes.filter(
      ({ type }) => type === "line_continuation",
    );
    assert.equal(continuation.length, 1);
    assert.equal(continuation[0].text, "\\");
    assert.ok(
      continuation[0].start >= keyword.start &&
        continuation[0].end <= keyword.end,
    );
  });
}
