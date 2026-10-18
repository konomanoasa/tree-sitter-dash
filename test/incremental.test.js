import assert from "node:assert/strict";
import { test } from "node:test";
import {
  assertIncrementalEqualsFresh,
  assertRepeatedColdParse,
  runParse,
  writeSource,
} from "./support/parser.js";

test("edits and repairs converge across commands, expansions and here-documents", () => {
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
    ["parameter colon-brace operator", `: \${a:}word}\n: after\n`, ":}", ":-"],
    ["parameter colon-brace continuation", `: \${a:}}\n`, ":}", ":\\\n}"],
    [
      "parameter colon-brace lookahead",
      `: $((\${a:}2}))\n: after\n`,
      "2",
      "')'",
    ],
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
    [
      "backquote extra word repair",
      ': `(printf x) ignored "`\n',
      "ignored",
      "",
    ],
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
    ["function close", "f() { :; }\n: after\n", "}", ""],
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
    const offset = source.indexOf(removed);
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
  }
});

test("repairs restore parameter contexts and nested structures", () => {
  const fixtures = [
    ["backquote pending token", ": `cat <<A)$(printf X)\n`\n", ")$", ";;$"],
    ["parameter colon-brace close", `: \${a:}word}\n`, "word}", "word"],
    ["parameter colon-brace hash", `: \${a:}}\n`, "a", "#"],
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
        fixtures.push([
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
      fixtures.push([
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
    fixtures.push([
      `nested ${name}`,
      `${prefix}echo \${x:-value}\n${suffix}`,
      `}\n${suffix}`,
      "\n",
    ]);
  }
  for (const [name, body, removed, inserted] of fixtures) {
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
  }
});
