import assert from "node:assert/strict";

const commandLine = `printf '%s\\n' "\${name:-value}" $((x+2*3)) [a-z]*\n`;
export const scaleFactors = [1, 2, 4];
export const scalingWorkloads = [
  {
    name: "commands",
    units: 1_250,
    source: (count) => commandLine.repeat(count),
  },
  {
    name: "expanded here body",
    units: 1_024,
    source: (count) => `cat <<END\n${"$name body text\n".repeat(count)}END\n`,
  },
  {
    name: "quoted here body",
    units: 1_024,
    source: (count) => `cat <<'END'\n${"$name body text\n".repeat(count)}END\n`,
  },
  {
    name: "multiline delimiter partial matches",
    units: 1_024,
    source: (count) => `cat <<'a\na\nb'\n${"a\n".repeat(count)}a\na\nb\n`,
  },
  {
    name: "retained quoted body",
    units: 1_024,
    source: (count) =>
      `: \`cat <<'A' <<B)v=${"body text\n".repeat(count)}A\n$(printf X)\nB\n\`\n`,
  },
];

const commandLineCounts = [50, 250, 500];
const expansionDepths = [16, 24, 32];

export function* editingWorkloads() {
  for (const count of commandLineCounts) {
    const source = commandLine.repeat(count);
    for (const index of [0, Math.floor(count / 2), count - 1]) {
      yield {
        name: `commands/${count}/${index}`,
        source,
        edits: [
          {
            byte: commandLine.length * index + commandLine.indexOf("value"),
            deleteBytes: 5,
            insert: "other",
          },
        ],
      };
    }
  }
  for (const depth of expansionDepths) {
    for (const [name, open, close, leaf] of [
      ["command", "$(", ")", "printf value"],
      ["quoted parameter", '"${x:-', '}"', "value"],
      ["arithmetic", "$((", "))", "1"],
    ]) {
      const prefix = `: ${open.repeat(depth)}`;
      yield {
        name: `${name}/${depth}`,
        source: `${prefix}${leaf}${close.repeat(depth)}\n`,
        edits: [
          {
            byte: prefix.length + leaf.length - 1,
            deleteBytes: 1,
            insert: "2",
          },
        ],
      };
    }
  }
  const bodyLineCount = 500;
  const structureEditLineCount = 500;
  const functionLine = "alpha() { : value; }\n";
  for (const [name, contents, removed, inserted] of [
    [
      "here body",
      `cat <<END\n${"$name body\n".repeat(bodyLineCount)}END\n`,
      "body",
      "text",
    ],
    [
      "here delimiter",
      `cat <<END\n${"$name body\n".repeat(bodyLineCount)}END\n`,
      "<<END",
      "<<OTHER",
    ],
    [
      "here quote",
      `cat <<END\n${"$name body\n".repeat(bodyLineCount)}END\n`,
      "<<END",
      "<<'END'",
    ],
    ["function closer", functionLine.repeat(structureEditLineCount), "}", ""],
  ]) {
    const byte = contents.indexOf(
      removed,
      name === "function closer"
        ? functionLine.length * Math.floor(structureEditLineCount / 2)
        : 0,
    );
    assert.ok(byte >= 0, name);
    const edit = { byte, deleteBytes: removed.length, insert: inserted };
    const repair = { byte, deleteBytes: inserted.length, insert: removed };
    yield {
      name: `${name}/changed`,
      source: contents,
      edits: [edit],
      recovery: name === "function closer",
    };
    yield { name: `${name}/repaired`, source: contents, edits: [edit, repair] };
  }
}
