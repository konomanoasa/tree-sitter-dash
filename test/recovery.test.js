import assert from "node:assert/strict";
import { test } from "node:test";
import {
  assertIncrementalEqualsFresh,
  assertRepeatedColdParse,
  runParse,
  writeSource,
} from "./support/parser.js";

const recoveryCases = [
  ["backquote pending token", ": `cat <<A)$(printf X)\n`\n", ")$", ";;$"],
  ["parameter colon before close", `: \${a:-}\n`, ":-", ":"],
  ["parameter colon-brace", `: \${a:-word}\n`, ":-", ":}"],
  ["parameter colon-brace continuation", `: \${a:-word}\n`, ":-", ":\\\n}"],
  ["parameter colon-brace NUL", `: \${a:\0-word}\n`, "-", "}"],
  ["quoted parameter colon-brace", `: "\${a:-word}"\n`, ":-", ":}"],
  ["assigned parameter colon-brace", `v=\${a:-word}\n`, ":-", ":}"],
  ["here parameter colon-brace", `cat <<END\n\${a:-word}\nEND\n`, ":-", ":}"],
  ["command parameter colon-brace", `: $(echo \${a:-word})\n`, ":-", ":}"],
  ["backquote parameter colon-brace", `: \`echo \${a:-word}\`\n`, ":-", ":}"],
  ["arithmetic parameter colon-brace", `: $((\${a:-2}))\n`, ":-", ":}"],
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
