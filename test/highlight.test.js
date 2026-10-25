import assert from "node:assert/strict";
import {
  mkdirSync,
  mkdtempSync,
  rmSync,
  symlinkSync,
  writeFileSync,
} from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { after, before, test } from "node:test";
import {
  createTreeSitter,
  grammars,
  packageName,
  root,
} from "../scripts/tree-sitter.js";

const highlightTimeoutMs = 60_000;
const firstThemeColor = 17;

function decodeEntities(text) {
  return text
    .replaceAll("&lt;", "<")
    .replaceAll("&gt;", ">")
    .replaceAll("&quot;", '"')
    .replaceAll("&#39;", "'")
    .replaceAll("&amp;", "&");
}

function renderedCaptures(html, source) {
  const start = html.indexOf("<pre><code>");
  const end = html.indexOf("</code></pre>");
  assert.ok(start >= 0 && end >= start, html);
  const content = html.slice(start + "<pre><code>".length, end);
  const stack = [];
  const captures = [];
  let text = "";
  for (const part of content.matchAll(
    /<span class='([^']*)'>|<[/]span>|([^<]+)/g,
  )) {
    if (part[1] !== undefined) stack.push(part[1].replaceAll(" ", "."));
    else if (part[0] === "</span>") assert.notEqual(stack.pop(), undefined);
    else {
      const decoded = decodeEntities(part[2]);
      text += decoded;
      captures.push(
        ...Array(Buffer.byteLength(decoded)).fill(stack.at(-1) ?? ""),
      );
    }
  }
  assert.equal(stack.length, 0, "unclosed highlight span");
  assert.equal(
    text.replace(/\n$/, ""),
    source.replace(/\n$/, ""),
    "rendered source differs from the input",
  );
  return captures;
}

function createHighlighter({ directory, root, run, captureNames }) {
  const parserDirectory = join(directory, "parsers");
  mkdirSync(parserDirectory);
  symlinkSync(root, join(parserDirectory, "tree-sitter-test"), "junction");
  const configPath = join(directory, "highlight.json");
  const capturePath = join(directory, "captures.txt");
  writeFileSync(
    configPath,
    JSON.stringify({
      "parser-directories": [parserDirectory],
      theme: Object.fromEntries(
        captureNames.map((name, index) => [name, index + firstThemeColor]),
      ),
    }),
  );
  writeFileSync(capturePath, `${captureNames.join("\n")}\n`);

  return (scope, source, valid = true) => {
    const path = join(directory, "highlight.txt");
    writeFileSync(path, source);
    if (valid) {
      const parsed = run(["parse", "--cst", "--scope", scope, path]);
      assert.doesNotMatch(parsed, /^[0-9: \t-]+•/m, parsed);
    }
    const captures = renderedCaptures(
      run([
        "highlight",
        "--check",
        "--captures-path",
        capturePath,
        "--config-path",
        configPath,
        "--html",
        "--layout",
        "fragment",
        "--style",
        "classes",
        "--scope",
        scope,
        path,
      ]),
      source,
    );
    for (const capture of captures) {
      assert.ok(
        capture === "" || captureNames.includes(capture),
        `unexpected final capture: ${capture}`,
      );
    }
    return captures;
  };
}

function assertCaptures(source, actual, ranges) {
  const bytes = Buffer.from(source);
  const expected = Array(bytes.length).fill("");
  let previousEnd = 0;
  for (const [start, end, capture] of ranges) {
    assert.ok(
      Number.isSafeInteger(start) && start >= previousEnd,
      "expected ranges must be ordered and disjoint",
    );
    assert.ok(
      Number.isSafeInteger(end) && end > start && end <= bytes.length,
      "expected range exceeds source bytes",
    );
    expected.fill(capture, start, end);
    previousEnd = end;
  }
  for (const [index, byte] of bytes.entries()) {
    if (byte !== 10)
      assert.equal(
        actual[index],
        expected[index],
        `byte ${index} in ${JSON.stringify(source)}`,
      );
  }
}

const captureNames = [
  "character",
  "character.special",
  "comment",
  "function",
  "function.call",
  "keyword",
  "label",
  "number",
  "operator",
  "punctuation.bracket",
  "punctuation.delimiter",
  "punctuation.special",
  "string",
  "string.escape",
  "string.regexp",
  "string.special.path",
  "variable",
  "variable.builtin",
  "variable.parameter",
];
let highlight;

let directory;
let runner;
before(() => {
  directory = mkdtempSync(join(tmpdir(), `${packageName}-highlight-`));
  runner = createTreeSitter();
  highlight = createHighlighter({
    directory,
    root,
    run: assertCommand,
    captureNames,
  });
});
after(() => {
  try {
    runner?.close();
  } finally {
    if (directory) rmSync(directory, { recursive: true, force: true });
  }
});

function assertCommand(arguments_) {
  const result = runner.run(arguments_, {
    timeout: highlightTimeoutMs,
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  assert.doesNotMatch(result.stderr, /Non-standard highlight captures/);
  return result.stdout;
}

const finalCaptureCases = [
  {
    name: "a default-value operator stays distinct from its operand and closing brace",
    source: `echo \${a:-word}\n`,
    captures: [
      [0, 4, "function.call"],
      [5, 6, "variable"],
      [6, 7, "punctuation.bracket"],
      [7, 8, "variable"],
      [8, 10, "operator"],
      [10, 14, "string"],
      [14, 15, "punctuation.bracket"],
    ],
  },
  {
    name: "declaration words expose names and operators while pattern values remain strings",
    source: "readonly x=[a-z]* y=~\n",
    captures: [
      [0, 8, "function.call"],
      [9, 10, "variable"],
      [10, 11, "operator"],
      [11, 17, "string"],
      [18, 19, "variable"],
      [19, 20, "operator"],
      [20, 21, "string.special.path"],
    ],
  },
  {
    name: "forwarded utility names retain argument captures and assignments survive redirects",
    source: "command export x=sh >out y=\n",
    captures: [
      [0, 7, "function.call"],
      [8, 14, "string"],
      [15, 16, "variable"],
      [16, 17, "operator"],
      [17, 19, "string"],
      [20, 21, "operator"],
      [21, 24, "string"],
      [25, 26, "variable"],
      [26, 27, "operator"],
    ],
  },
  {
    name: "HTML-sensitive Unicode literals retain source bytes and captures",
    source: 'printf "é😀<&>\'"\n',
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 18, "string"],
      [18, 19, "punctuation.delimiter"],
    ],
  },
  {
    name: "empty input has no captures",
    source: "",
    captures: [],
  },
  {
    name: "command names override literals and UTF-8 arguments retain byte ranges",
    source: "printf é😀\n",
    captures: [
      [0, 6, "function.call"],
      [7, 13, "string"],
    ],
  },
  {
    name: "assignment names, operators and quote delimiters keep separate roles",
    source: 'value="é"\n',
    captures: [
      [0, 5, "variable"],
      [5, 6, "operator"],
      [6, 7, "punctuation.delimiter"],
      [7, 9, "string"],
      [9, 10, "punctuation.delimiter"],
    ],
  },
  {
    name: "quoted text and escaped spaces keep distinct final captures",
    source: "printf 'a b' a\\ b\n",
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 11, "string"],
      [11, 12, "punctuation.delimiter"],
      [13, 14, "string"],
      [14, 16, "string.escape"],
      [16, 17, "string"],
    ],
  },
  {
    name: "parameter expansions distinguish positional, special and named variables",
    source: `printf $1 $? \${x}\n`,
    captures: [
      [0, 6, "function.call"],
      [7, 8, "variable"],
      [8, 9, "variable.parameter"],
      [10, 11, "variable"],
      [11, 12, "variable.builtin"],
      [13, 14, "variable"],
      [14, 15, "punctuation.bracket"],
      [15, 16, "variable"],
      [16, 17, "punctuation.bracket"],
    ],
  },
  {
    name: "unquoted here-documents retain expansion captures between labels",
    source: "cat <<EOF\n$x\nEOF\n",
    captures: [
      [0, 3, "function.call"],
      [4, 6, "operator"],
      [6, 9, "label"],
      [10, 12, "variable"],
      [13, 16, "label"],
    ],
  },
  {
    name: "a delimiter double quote closed by a backquote keeps body expansion captures",
    source: 'cat <<`"x`\n$v\n`x`\n',
    captures: [
      [0, 3, "function.call"],
      [4, 6, "operator"],
      [6, 10, "label"],
      [11, 13, "variable"],
      [14, 17, "label"],
    ],
  },
  {
    name: "continuations and UTF-8 comments retain their source ranges",
    source: "printf \\\nvalue # é😀\n",
    captures: [
      [0, 6, "function.call"],
      [7, 9, "punctuation.special"],
      [9, 14, "string"],
      [15, 23, "comment"],
    ],
  },
  {
    name: "comments retain their complete source range",
    source: "# comment\n",
    captures: [[0, 9, "comment"]],
  },
  {
    name: "ancestor-removed continuations separate comment text captures",
    source: 'echo "`a #x\\\nb\n`"\n',
    captures: [
      [0, 4, "function.call"],
      [5, 7, "punctuation.delimiter"],
      [7, 8, "function.call"],
      [9, 11, "comment"],
      [11, 13, "punctuation.special"],
      [13, 14, "comment"],
      [15, 17, "punctuation.delimiter"],
    ],
  },
  {
    name: "unquoted assignment values retain string captures",
    source: "name=value\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
    ],
  },
  {
    name: "bracket expressions color command patterns",
    source: "[a]\n",
    captures: [
      [0, 1, "punctuation.bracket"],
      [1, 2, "character"],
      [2, 3, "punctuation.bracket"],
    ],
  },
  {
    name: "literal dollar signs remain bracket characters",
    source: "[a$]\n",
    captures: [
      [0, 1, "punctuation.bracket"],
      [1, 3, "character"],
      [3, 4, "punctuation.bracket"],
    ],
  },
  {
    name: "bracket commands after assignments retain pattern captures",
    source: "name=value [a]\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
      [11, 12, "punctuation.bracket"],
      [12, 13, "character"],
      [13, 14, "punctuation.bracket"],
    ],
  },
  {
    name: "bracket words in for loops retain pattern captures",
    source: "for item in [a]; do :; done\n",
    captures: [
      [0, 3, "keyword"],
      [4, 8, "variable"],
      [9, 11, "keyword"],
      [12, 13, "punctuation.bracket"],
      [13, 14, "character"],
      [14, 15, "punctuation.bracket"],
      [15, 16, "punctuation.delimiter"],
      [17, 19, "keyword"],
      [20, 21, "function.call"],
      [21, 22, "punctuation.delimiter"],
      [23, 27, "keyword"],
    ],
  },
  {
    name: "bracket filenames retain pattern captures",
    source: "cat >[a]\n",
    captures: [
      [0, 3, "function.call"],
      [4, 5, "operator"],
      [5, 6, "punctuation.bracket"],
      [6, 7, "character"],
      [7, 8, "punctuation.bracket"],
    ],
  },
  {
    name: "implicit for iteration preserves captures with a newline before its semicolon",
    source: "for x\n; do :; done\n",
    captures: [
      [0, 3, "keyword"],
      [4, 5, "variable"],
      [6, 7, "punctuation.delimiter"],
      [8, 10, "keyword"],
      [11, 12, "function.call"],
      [12, 13, "punctuation.delimiter"],
      [14, 18, "keyword"],
    ],
  },
  {
    name: "for loops distinguish keywords, names, patterns and quoted expansions",
    source: "for item in one *.txt; do\n\n  printf '%s\\n' \"$item\"\ndone\n",
    captures: [
      [0, 3, "keyword"],
      [4, 8, "variable"],
      [9, 11, "keyword"],
      [12, 15, "string"],
      [16, 17, "character.special"],
      [17, 21, "string"],
      [21, 22, "punctuation.delimiter"],
      [23, 25, "keyword"],
      [29, 35, "function.call"],
      [36, 37, "punctuation.delimiter"],
      [37, 41, "string"],
      [41, 42, "punctuation.delimiter"],
      [43, 44, "punctuation.delimiter"],
      [44, 49, "variable"],
      [49, 50, "punctuation.delimiter"],
      [51, 55, "keyword"],
    ],
  },
  {
    name: "arithmetic and parameter expansions retain their distinct roles in quotes",
    source: `printf '%s\\n' "$((count += 2 * 3))" "\${name:-fallback}" "$@" "$10"\n`,
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 12, "string"],
      [12, 13, "punctuation.delimiter"],
      [14, 15, "punctuation.delimiter"],
      [15, 16, "punctuation.special"],
      [16, 18, "punctuation.bracket"],
      [18, 23, "variable"],
      [24, 26, "operator"],
      [27, 28, "number"],
      [29, 30, "operator"],
      [31, 32, "number"],
      [32, 34, "punctuation.bracket"],
      [34, 35, "punctuation.delimiter"],
      [36, 37, "punctuation.delimiter"],
      [37, 38, "variable"],
      [38, 39, "punctuation.bracket"],
      [39, 43, "variable"],
      [43, 45, "operator"],
      [45, 53, "string"],
      [53, 54, "punctuation.bracket"],
      [54, 55, "punctuation.delimiter"],
      [56, 57, "punctuation.delimiter"],
      [57, 58, "variable"],
      [58, 59, "variable.builtin"],
      [59, 60, "punctuation.delimiter"],
      [61, 62, "punctuation.delimiter"],
      [62, 63, "variable"],
      [63, 64, "variable.parameter"],
      [64, 65, "string"],
      [65, 66, "punctuation.delimiter"],
    ],
  },
  {
    name: "removal patterns and filename classes distinguish text from active markers",
    source: `printf '%s\\n' "\${name#[!a-c]}" file-[[:digit:]]?\n`,
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 12, "string"],
      [12, 13, "punctuation.delimiter"],
      [14, 15, "punctuation.delimiter"],
      [15, 16, "variable"],
      [16, 17, "punctuation.bracket"],
      [17, 21, "variable"],
      [21, 22, "operator"],
      [22, 23, "punctuation.bracket"],
      [23, 24, "operator"],
      [24, 25, "character"],
      [25, 26, "operator"],
      [26, 27, "character"],
      [27, 29, "punctuation.bracket"],
      [29, 30, "punctuation.delimiter"],
      [31, 36, "string"],
      [36, 38, "punctuation.bracket"],
      [38, 39, "punctuation.delimiter"],
      [39, 44, "character.special"],
      [44, 45, "punctuation.delimiter"],
      [45, 47, "punctuation.bracket"],
      [47, 48, "character.special"],
    ],
  },
  {
    name: "tilde paths and escaped spaces retain distinct argument captures",
    source: "printf '%s\\n' ~user file\\ name\n",
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 12, "string"],
      [12, 13, "punctuation.delimiter"],
      [14, 19, "string.special.path"],
      [20, 24, "string"],
      [24, 26, "string.escape"],
      [26, 30, "string"],
    ],
  },
  {
    name: "dollar quotes, double quotes and escaped markers retain escape captures",
    source: "printf '%s\\n' $'a\\n' \"a\\$b\" \\*\n",
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 12, "string"],
      [12, 13, "punctuation.delimiter"],
      [14, 16, "punctuation.delimiter"],
      [16, 17, "string"],
      [17, 19, "string.escape"],
      [19, 20, "punctuation.delimiter"],
      [21, 22, "punctuation.delimiter"],
      [22, 23, "string"],
      [23, 25, "string.escape"],
      [25, 26, "string"],
      [26, 27, "punctuation.delimiter"],
      [28, 30, "string.escape"],
    ],
  },
  {
    name: "command and backquote substitutions preserve their delimiters and command names",
    source: "printf '%s\\n' \"$(date)\" `date`\n",
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 12, "string"],
      [12, 13, "punctuation.delimiter"],
      [14, 15, "punctuation.delimiter"],
      [15, 16, "punctuation.special"],
      [16, 17, "punctuation.bracket"],
      [17, 21, "function.call"],
      [21, 22, "punctuation.bracket"],
      [22, 23, "punctuation.delimiter"],
      [24, 25, "punctuation.delimiter"],
      [25, 29, "function.call"],
      [29, 30, "punctuation.delimiter"],
    ],
  },
  {
    name: "unparsed backquote tails do not highlight quote or expansion spellings",
    source: ': `printf x) $v "`\n',
    captures: [
      [0, 1, "function.call"],
      [2, 3, "punctuation.delimiter"],
      [3, 9, "function.call"],
      [10, 11, "string"],
      [17, 18, "punctuation.delimiter"],
    ],
  },
  {
    name: "negation, pipelines, boolean lists and background commands retain operators",
    source: "! true && false || true | cat &\n",
    captures: [
      [0, 1, "operator"],
      [2, 6, "function.call"],
      [7, 9, "operator"],
      [10, 15, "function.call"],
      [16, 18, "operator"],
      [19, 23, "function.call"],
      [24, 25, "operator"],
      [26, 29, "function.call"],
      [30, 31, "operator"],
    ],
  },
  {
    name: "numeric file descriptors retain number captures beside redirections",
    source: "cat 2>output\n",
    captures: [
      [0, 3, "function.call"],
      [4, 5, "number"],
      [5, 6, "operator"],
      [6, 12, "string"],
    ],
  },
  {
    name: "brace words remain arguments before redirection operators",
    source: "cat {fd}>output\n",
    captures: [
      [0, 3, "function.call"],
      [4, 8, "string"],
      [8, 9, "operator"],
      [9, 15, "string"],
    ],
  },
  {
    name: "continued command arguments keep layout separate from string text",
    source: "printf '%s' \\\n  value\n",
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.delimiter"],
      [8, 10, "string"],
      [10, 11, "punctuation.delimiter"],
      [12, 14, "punctuation.special"],
      [16, 21, "string"],
    ],
  },
  {
    name: "quoted here-documents label their ends and preserve literal expansion text",
    source: "cat <<'EOF'\n$name \\$ text\nEOF\n",
    captures: [
      [0, 3, "function.call"],
      [4, 6, "operator"],
      [6, 11, "label"],
      [12, 25, "string"],
      [26, 29, "label"],
    ],
  },
  {
    name: "command names retain call captures around quotes and asterisks",
    source: "pre\"quoted\"mid*tail'quoted'end\n",
    captures: [
      [0, 3, "function.call"],
      [3, 4, "punctuation.delimiter"],
      [4, 10, "string"],
      [10, 11, "punctuation.delimiter"],
      [11, 14, "function.call"],
      [14, 15, "character.special"],
      [15, 19, "function.call"],
      [19, 20, "punctuation.delimiter"],
      [20, 26, "string"],
      [26, 27, "punctuation.delimiter"],
      [27, 30, "function.call"],
    ],
  },
  {
    name: "command names retain call captures around quotes and question marks",
    source: "pre\"quoted\"mid?tail'quoted'end\n",
    captures: [
      [0, 3, "function.call"],
      [3, 4, "punctuation.delimiter"],
      [4, 10, "string"],
      [10, 11, "punctuation.delimiter"],
      [11, 14, "function.call"],
      [14, 15, "character.special"],
      [15, 19, "function.call"],
      [19, 20, "punctuation.delimiter"],
      [20, 26, "string"],
      [26, 27, "punctuation.delimiter"],
      [27, 30, "function.call"],
    ],
  },
  {
    name: "command names retain call captures around quotes and bracket expressions",
    source: "pre\"quoted\"mid[a]tail'quoted'end\n",
    captures: [
      [0, 3, "function.call"],
      [3, 4, "punctuation.delimiter"],
      [4, 10, "string"],
      [10, 11, "punctuation.delimiter"],
      [11, 14, "function.call"],
      [14, 15, "punctuation.bracket"],
      [15, 16, "character"],
      [16, 17, "punctuation.bracket"],
      [17, 21, "function.call"],
      [21, 22, "punctuation.delimiter"],
      [22, 28, "string"],
      [28, 29, "punctuation.delimiter"],
      [29, 32, "function.call"],
    ],
  },
  {
    name: "commands after assignments retain call captures around quotes and asterisks",
    source: "name=value pre\"quoted\"mid*tail'quoted'end\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
      [11, 14, "function.call"],
      [14, 15, "punctuation.delimiter"],
      [15, 21, "string"],
      [21, 22, "punctuation.delimiter"],
      [22, 25, "function.call"],
      [25, 26, "character.special"],
      [26, 30, "function.call"],
      [30, 31, "punctuation.delimiter"],
      [31, 37, "string"],
      [37, 38, "punctuation.delimiter"],
      [38, 41, "function.call"],
    ],
  },
  {
    name: "commands after assignments retain call captures around quotes and question marks",
    source: "name=value pre\"quoted\"mid?tail'quoted'end\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
      [11, 14, "function.call"],
      [14, 15, "punctuation.delimiter"],
      [15, 21, "string"],
      [21, 22, "punctuation.delimiter"],
      [22, 25, "function.call"],
      [25, 26, "character.special"],
      [26, 30, "function.call"],
      [30, 31, "punctuation.delimiter"],
      [31, 37, "string"],
      [37, 38, "punctuation.delimiter"],
      [38, 41, "function.call"],
    ],
  },
  {
    name: "commands after assignments retain call captures around quotes and bracket expressions",
    source: "name=value pre\"quoted\"mid[a]tail'quoted'end\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
      [11, 14, "function.call"],
      [14, 15, "punctuation.delimiter"],
      [15, 21, "string"],
      [21, 22, "punctuation.delimiter"],
      [22, 25, "function.call"],
      [25, 26, "punctuation.bracket"],
      [26, 27, "character"],
      [27, 28, "punctuation.bracket"],
      [28, 32, "function.call"],
      [32, 33, "punctuation.delimiter"],
      [33, 39, "string"],
      [39, 40, "punctuation.delimiter"],
      [40, 43, "function.call"],
    ],
  },
  {
    name: "command arguments retain string captures around quotes and asterisks",
    source: "echo pre\"quoted\"mid*tail'quoted'end\n",
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 19, "string"],
      [19, 20, "character.special"],
      [20, 24, "string"],
      [24, 25, "punctuation.delimiter"],
      [25, 31, "string"],
      [31, 32, "punctuation.delimiter"],
      [32, 35, "string"],
    ],
  },
  {
    name: "command arguments retain string captures around quotes and question marks",
    source: "echo pre\"quoted\"mid?tail'quoted'end\n",
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 19, "string"],
      [19, 20, "character.special"],
      [20, 24, "string"],
      [24, 25, "punctuation.delimiter"],
      [25, 31, "string"],
      [31, 32, "punctuation.delimiter"],
      [32, 35, "string"],
    ],
  },
  {
    name: "command arguments retain string captures around quotes and bracket expressions",
    source: "echo pre\"quoted\"mid[a]tail'quoted'end\n",
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 19, "string"],
      [19, 20, "punctuation.bracket"],
      [20, 21, "character"],
      [21, 22, "punctuation.bracket"],
      [22, 26, "string"],
      [26, 27, "punctuation.delimiter"],
      [27, 33, "string"],
      [33, 34, "punctuation.delimiter"],
      [34, 37, "string"],
    ],
  },
  {
    name: "for word lists retain string captures around quotes and asterisks",
    source: "for item in pre\"quoted\"mid*tail'quoted'end; do :; done\n",
    captures: [
      [0, 3, "keyword"],
      [4, 8, "variable"],
      [9, 11, "keyword"],
      [12, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 22, "string"],
      [22, 23, "punctuation.delimiter"],
      [23, 26, "string"],
      [26, 27, "character.special"],
      [27, 31, "string"],
      [31, 32, "punctuation.delimiter"],
      [32, 38, "string"],
      [38, 39, "punctuation.delimiter"],
      [39, 42, "string"],
      [42, 43, "punctuation.delimiter"],
      [44, 46, "keyword"],
      [47, 48, "function.call"],
      [48, 49, "punctuation.delimiter"],
      [50, 54, "keyword"],
    ],
  },
  {
    name: "for word lists retain string captures around quotes and question marks",
    source: "for item in pre\"quoted\"mid?tail'quoted'end; do :; done\n",
    captures: [
      [0, 3, "keyword"],
      [4, 8, "variable"],
      [9, 11, "keyword"],
      [12, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 22, "string"],
      [22, 23, "punctuation.delimiter"],
      [23, 26, "string"],
      [26, 27, "character.special"],
      [27, 31, "string"],
      [31, 32, "punctuation.delimiter"],
      [32, 38, "string"],
      [38, 39, "punctuation.delimiter"],
      [39, 42, "string"],
      [42, 43, "punctuation.delimiter"],
      [44, 46, "keyword"],
      [47, 48, "function.call"],
      [48, 49, "punctuation.delimiter"],
      [50, 54, "keyword"],
    ],
  },
  {
    name: "for word lists retain string captures around quotes and bracket expressions",
    source: "for item in pre\"quoted\"mid[a]tail'quoted'end; do :; done\n",
    captures: [
      [0, 3, "keyword"],
      [4, 8, "variable"],
      [9, 11, "keyword"],
      [12, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 22, "string"],
      [22, 23, "punctuation.delimiter"],
      [23, 26, "string"],
      [26, 27, "punctuation.bracket"],
      [27, 28, "character"],
      [28, 29, "punctuation.bracket"],
      [29, 33, "string"],
      [33, 34, "punctuation.delimiter"],
      [34, 40, "string"],
      [40, 41, "punctuation.delimiter"],
      [41, 44, "string"],
      [44, 45, "punctuation.delimiter"],
      [46, 48, "keyword"],
      [49, 50, "function.call"],
      [50, 51, "punctuation.delimiter"],
      [52, 56, "keyword"],
    ],
  },
  {
    name: "redirection filenames retain string captures around quotes and asterisks",
    source: "cat >pre\"quoted\"mid*tail'quoted'end\n",
    captures: [
      [0, 3, "function.call"],
      [4, 5, "operator"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 19, "string"],
      [19, 20, "character.special"],
      [20, 24, "string"],
      [24, 25, "punctuation.delimiter"],
      [25, 31, "string"],
      [31, 32, "punctuation.delimiter"],
      [32, 35, "string"],
    ],
  },
  {
    name: "redirection filenames retain string captures around quotes and question marks",
    source: "cat >pre\"quoted\"mid?tail'quoted'end\n",
    captures: [
      [0, 3, "function.call"],
      [4, 5, "operator"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 19, "string"],
      [19, 20, "character.special"],
      [20, 24, "string"],
      [24, 25, "punctuation.delimiter"],
      [25, 31, "string"],
      [31, 32, "punctuation.delimiter"],
      [32, 35, "string"],
    ],
  },
  {
    name: "redirection filenames retain string captures around quotes and bracket expressions",
    source: "cat >pre\"quoted\"mid[a]tail'quoted'end\n",
    captures: [
      [0, 3, "function.call"],
      [4, 5, "operator"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 19, "string"],
      [19, 20, "punctuation.bracket"],
      [20, 21, "character"],
      [21, 22, "punctuation.bracket"],
      [22, 26, "string"],
      [26, 27, "punctuation.delimiter"],
      [27, 33, "string"],
      [33, 34, "punctuation.delimiter"],
      [34, 37, "string"],
    ],
  },
  {
    name: "multiple active markers leave surrounding literals as strings",
    source: "echo pre*mid?tail[a]end\n",
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "character.special"],
      [9, 12, "string"],
      [12, 13, "character.special"],
      [13, 17, "string"],
      [17, 18, "punctuation.bracket"],
      [18, 19, "character"],
      [19, 20, "punctuation.bracket"],
      [20, 23, "string"],
    ],
  },
  {
    name: "substitutions retain their captures between pathname literals",
    source: `echo pre$(printf x)mid*tail\${value}end\n`,
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "punctuation.special"],
      [9, 10, "punctuation.bracket"],
      [10, 16, "function.call"],
      [17, 18, "string"],
      [18, 19, "punctuation.bracket"],
      [19, 22, "string"],
      [22, 23, "character.special"],
      [23, 27, "string"],
      [27, 28, "variable"],
      [28, 29, "punctuation.bracket"],
      [29, 34, "variable"],
      [34, 35, "punctuation.bracket"],
      [35, 38, "string"],
    ],
  },
  {
    name: "plain words retain string captures around quoted text",
    source: 'echo pre"quoted"tail\n',
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 15, "string"],
      [15, 16, "punctuation.delimiter"],
      [16, 20, "string"],
    ],
  },
  {
    name: "quoted and escaped markers leave surrounding literals as strings",
    source: "echo pre'*'tail pre\\*tail\n",
    captures: [
      [0, 4, "function.call"],
      [5, 8, "string"],
      [8, 9, "punctuation.delimiter"],
      [9, 10, "string"],
      [10, 11, "punctuation.delimiter"],
      [11, 15, "string"],
      [16, 19, "string"],
      [19, 21, "string.escape"],
      [21, 25, "string"],
    ],
  },
  {
    name: "assignment literals do not activate pathname patterns",
    source: "name=pre*tail\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 13, "string"],
    ],
  },
  {
    name: "escaped bracket members retain their escape and enclosing punctuation",
    source: "echo [\\a]\n",
    captures: [
      [0, 4, "function.call"],
      [5, 6, "punctuation.bracket"],
      [6, 8, "string.escape"],
      [8, 9, "punctuation.bracket"],
    ],
  },
  {
    name: "chained ranges distinguish operators from remaining hyphens",
    source: "printf [a-b--] [a-c-e-g]\n",
    captures: [
      [0, 6, "function.call"],
      [7, 8, "punctuation.bracket"],
      [8, 9, "character"],
      [9, 10, "operator"],
      [10, 13, "character"],
      [13, 14, "punctuation.bracket"],
      [15, 16, "punctuation.bracket"],
      [16, 17, "character"],
      [17, 18, "operator"],
      [18, 21, "character"],
      [21, 22, "operator"],
      [22, 23, "character"],
      [23, 24, "punctuation.bracket"],
    ],
  },
  {
    name: "command ranges retain escaped endpoints",
    source: "pre[a-\\c]\n",
    captures: [
      [0, 3, "function.call"],
      [3, 4, "punctuation.bracket"],
      [4, 5, "character"],
      [5, 6, "operator"],
      [6, 8, "string.escape"],
      [8, 9, "punctuation.bracket"],
    ],
  },
  {
    name: "commands after assignments retain quoted range endpoints",
    source: "name=value pre['a'-c]\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
      [11, 14, "function.call"],
      [14, 15, "punctuation.bracket"],
      [15, 16, "punctuation.delimiter"],
      [16, 17, "string"],
      [17, 18, "punctuation.delimiter"],
      [18, 19, "operator"],
      [19, 20, "character"],
      [20, 21, "punctuation.bracket"],
    ],
  },
  {
    name: "filenames retain class markers around substituted class names",
    source: `cat >[[:\${kind}:]]\n`,
    captures: [
      [0, 3, "function.call"],
      [4, 5, "operator"],
      [5, 7, "punctuation.bracket"],
      [7, 8, "punctuation.delimiter"],
      [8, 9, "variable"],
      [9, 10, "punctuation.bracket"],
      [10, 14, "variable"],
      [14, 15, "punctuation.bracket"],
      [15, 16, "punctuation.delimiter"],
      [16, 18, "punctuation.bracket"],
    ],
  },
  {
    name: "command pattern literals retain call captures beside plain arguments",
    source: "pre*tail plain\n",
    captures: [
      [0, 3, "function.call"],
      [3, 4, "character.special"],
      [4, 8, "function.call"],
      [9, 14, "string"],
    ],
  },
  {
    name: "command pattern literals after assignments retain call captures beside plain arguments",
    source: "name=value pre?tail plain\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 10, "string"],
      [11, 14, "function.call"],
      [14, 15, "character.special"],
      [15, 19, "function.call"],
      [20, 25, "string"],
    ],
  },
  {
    name: "argument pattern literals retain string captures beside plain words",
    source: "echo plain pre[a]tail plain\n",
    captures: [
      [0, 4, "function.call"],
      [5, 10, "string"],
      [11, 14, "string"],
      [14, 15, "punctuation.bracket"],
      [15, 16, "character"],
      [16, 17, "punctuation.bracket"],
      [17, 21, "string"],
      [22, 27, "string"],
    ],
  },
  {
    name: "for pattern literals retain string captures beside plain words",
    source: "for item in plain pre*tail plain; do :; done\n",
    captures: [
      [0, 3, "keyword"],
      [4, 8, "variable"],
      [9, 11, "keyword"],
      [12, 17, "string"],
      [18, 21, "string"],
      [21, 22, "character.special"],
      [22, 26, "string"],
      [27, 32, "string"],
      [32, 33, "punctuation.delimiter"],
      [34, 36, "keyword"],
      [37, 38, "function.call"],
      [38, 39, "punctuation.delimiter"],
      [40, 44, "keyword"],
    ],
  },
  {
    name: "filename pattern literals retain string captures beside command names",
    source: "cat >pre?tail\n",
    captures: [
      [0, 3, "function.call"],
      [4, 5, "operator"],
      [5, 8, "string"],
      [8, 9, "character.special"],
      [9, 13, "string"],
    ],
  },
  {
    name: "tilde users retain path captures across all active pattern markers",
    source: "echo ~a*b ~a?b ~[!a-z[:alpha:][.x.][=y=]-] ~[[.a.]-[.z.]] ~[-a]\n",
    captures: [
      [0, 4, "function.call"],
      [5, 9, "string.special.path"],
      [10, 14, "string.special.path"],
      [15, 42, "string.special.path"],
      [43, 57, "string.special.path"],
      [58, 63, "string.special.path"],
    ],
  },
  {
    name: "colon-separated assignment tildes retain path captures",
    source: "name=~a*b:~[a-z]\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 9, "string.special.path"],
      [9, 10, "string"],
      [10, 16, "string.special.path"],
    ],
  },
  {
    name: "nested parameter defaults preserve tilde path captures",
    source: `echo \${x:-\${y:-~a?b}}\n`,
    captures: [
      [0, 4, "function.call"],
      [5, 6, "variable"],
      [6, 7, "punctuation.bracket"],
      [7, 8, "variable"],
      [8, 10, "operator"],
      [10, 11, "variable"],
      [11, 12, "punctuation.bracket"],
      [12, 13, "variable"],
      [13, 15, "operator"],
      [15, 19, "string.special.path"],
      [19, 21, "punctuation.bracket"],
    ],
  },
  {
    name: "tilde brackets retain escapes, quotes and substitutions inside path captures",
    source: "echo ~[a-\\c] ~[[.$element.]-[.\"z\".]] ~['q'] ~[$(printf x)]\n",
    captures: [
      [0, 4, "function.call"],
      [5, 9, "string.special.path"],
      [9, 11, "string.escape"],
      [11, 12, "string.special.path"],
      [13, 17, "string.special.path"],
      [17, 25, "variable"],
      [25, 30, "string.special.path"],
      [30, 31, "punctuation.delimiter"],
      [31, 32, "string"],
      [32, 33, "punctuation.delimiter"],
      [33, 36, "string.special.path"],
      [37, 39, "string.special.path"],
      [39, 40, "punctuation.delimiter"],
      [40, 41, "string"],
      [41, 42, "punctuation.delimiter"],
      [42, 43, "string.special.path"],
      [44, 46, "string.special.path"],
      [46, 47, "punctuation.special"],
      [47, 48, "punctuation.bracket"],
      [48, 54, "function.call"],
      [55, 56, "string"],
      [56, 57, "punctuation.bracket"],
      [57, 58, "string.special.path"],
    ],
  },
  {
    name: "pathname literals retain string captures outside the tilde user",
    source: "echo ~a*b/pre*tail\n",
    captures: [
      [0, 4, "function.call"],
      [5, 9, "string.special.path"],
      [9, 13, "string"],
      [13, 14, "character.special"],
      [14, 18, "string"],
    ],
  },
  {
    name: "assignment bracket structures have no active pattern captures",
    source: "name=[a]*\n",
    captures: [
      [0, 4, "variable"],
      [4, 5, "operator"],
      [5, 9, "string"],
    ],
  },
  {
    name: "case subjects retain strings before the pattern list",
    source: "case *[a]? in x) :;; esac\n",
    captures: [
      [0, 4, "keyword"],
      [5, 10, "string"],
      [11, 13, "keyword"],
      [14, 15, "string.regexp"],
      [15, 16, "punctuation.bracket"],
      [17, 18, "function.call"],
      [18, 20, "operator"],
      [21, 25, "keyword"],
    ],
  },
  {
    name: "continued command and argument keep parent captures around slash leaves",
    source: "pri\\\nntf va\\\nlue\n",
    captures: [
      [0, 3, "function.call"],
      [3, 4, "punctuation.special"],
      [5, 8, "function.call"],
      [9, 11, "string"],
      [11, 12, "punctuation.special"],
      [13, 16, "string"],
    ],
  },
  {
    name: "continued reserved words keep keyword captures around slash leaves",
    source: "i\\\nf :; th\\\nen :; f\\\ni\n",
    captures: [
      [0, 1, "keyword"],
      [1, 2, "punctuation.special"],
      [3, 4, "keyword"],
      [5, 6, "function.call"],
      [6, 7, "punctuation.delimiter"],
      [8, 10, "keyword"],
      [10, 11, "punctuation.special"],
      [12, 14, "keyword"],
      [15, 16, "function.call"],
      [16, 17, "punctuation.delimiter"],
      [18, 19, "keyword"],
      [19, 20, "punctuation.special"],
      [21, 22, "keyword"],
    ],
  },
  {
    name: "continued multi-digit words remain arguments beside the redirect",
    source: ": 1\\\n2>out\n",
    captures: [
      [0, 1, "function.call"],
      [2, 3, "string"],
      [3, 4, "punctuation.special"],
      [5, 6, "string"],
      [6, 7, "operator"],
      [7, 10, "string"],
    ],
  },
  {
    name: "continued parameter names retain variable captures around the slash leaf",
    source: `: \${va\\\nr}\n`,
    captures: [
      [0, 1, "function.call"],
      [2, 3, "variable"],
      [3, 4, "punctuation.bracket"],
      [4, 6, "variable"],
      [6, 7, "punctuation.special"],
      [8, 9, "variable"],
      [9, 10, "punctuation.bracket"],
    ],
  },
  {
    name: "continued dollar quote prefixes retain separate punctuation captures",
    source: ": $\\\n'abc'\n",
    captures: [
      [0, 1, "function.call"],
      [2, 3, "punctuation.delimiter"],
      [3, 4, "punctuation.special"],
      [5, 6, "punctuation.delimiter"],
      [6, 9, "string"],
      [9, 10, "punctuation.delimiter"],
    ],
  },
  {
    name: "quoted local names still introduce assignment arguments",
    source: "'local' x=1\n",
    captures: [
      [0, 1, "punctuation.delimiter"],
      [1, 6, "string"],
      [6, 7, "punctuation.delimiter"],
      [8, 9, "variable"],
      [9, 10, "operator"],
      [10, 11, "string"],
    ],
  },
  {
    name: "nonexpanded delimiter command spelling receives only a label",
    source: "cat <<$(if)\nbody\n$(if)\n",
    captures: [
      [0, 3, "function.call"],
      [4, 6, "operator"],
      [6, 11, "label"],
      [12, 16, "string"],
      [17, 22, "label"],
    ],
  },
  {
    name: "unclassified arithmetic fragments preserve recognized operators and numbers",
    source: ": $((@ + 1))\n",
    captures: [
      [0, 1, "function.call"],
      [2, 3, "punctuation.special"],
      [3, 5, "punctuation.bracket"],
      [7, 8, "operator"],
      [9, 10, "number"],
      [10, 12, "punctuation.bracket"],
    ],
  },
  {
    name: "caret negates a bracket pattern",
    source: ": [^a]\n",
    captures: [
      [0, 1, "function.call"],
      [2, 3, "punctuation.bracket"],
      [3, 4, "operator"],
      [4, 5, "character"],
      [5, 6, "punctuation.bracket"],
    ],
  },
  {
    name: "a pending parenthesis leaves the apparent command in here-document text",
    source: ": `cat <<A)$(printf X)\n`\n",
    captures: [
      [0, 1, "function.call"],
      [2, 3, "punctuation.delimiter"],
      [3, 6, "function.call"],
      [7, 9, "operator"],
      [9, 10, "label"],
      [11, 12, "punctuation.special"],
      [12, 13, "punctuation.bracket"],
      [13, 22, "string"],
      [23, 24, "punctuation.delimiter"],
    ],
  },
  {
    name: "EOF-terminated bodies retain expansion captures",
    source: "cat <<E\n$x\n",
    captures: [
      [0, 3, "function.call"],
      [4, 6, "operator"],
      [6, 7, "label"],
      [8, 10, "variable"],
    ],
  },
];

for (const grammar of grammars) {
  for (const { name, source, captures } of finalCaptureCases) {
    test(`${grammar.name}: ${name}`, () => {
      assertCaptures(source, highlight(grammar.scope, source), captures);
    });
  }
  test(`${grammar.name}: incomplete input preserves source without error colors`, () => {
    for (const source of ["if x; then", 'echo "', "cat <<EOF\nbody\n"]) {
      highlight(grammar.scope, source, false);
    }
  });
}

test("recognized leaves retain their captures through incomplete input and repair", () => {
  for (const source of [
    "if :; then :; fi\n",
    "if :; then",
    "if :; then :; fi\n",
  ]) {
    const captures = highlight("source.dash", source, false);
    assert.deepEqual(captures.slice(0, 2), ["keyword", "keyword"]);
    assert.equal(captures[2], "");
    assert.equal(captures[3], "function.call");
  }
});
