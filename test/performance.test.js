import assert from "node:assert/strict";
import { test } from "node:test";
import { grammars } from "../scripts/tree-sitter.js";
import {
  applyEdits,
  assertIncrementalEqualsFresh,
  assertRepeatedColdParse,
  parserLibrary,
  parseValidCst,
  runParse,
  runParserCommand,
  writeSource,
} from "./support/parser.js";

const measurementSamples = 5;
const warmupSamples = 1;
const ordinaryDurationLimitMs = 1_000;
const scalingInputMultiplier = 4;
const maximumDurationMultiplier = 8;
const scalingSlackMs = 100;
const commandCounts = [50, 500];
const structureEditLineCount = 500;
const nestingDepths = [16, 32];
const commandLine = `printf '%s\\n' "\${name:-value}" $((x+2*3)) [a-z]*\n`;

function medianDurations(source, edits = [], mode = "valid") {
  const arguments_ = [
    "parse",
    "--lib-path",
    parserLibrary,
    "--lang-name",
    grammars[0].name,
    "--time",
    "--quiet",
  ];
  if (edits.length > 0) {
    arguments_.push(
      "--edits",
      ...edits.map(
        ({ byte, deleteBytes, insert }) => `${byte} ${deleteBytes} ${insert}`,
      ),
    );
  }
  arguments_.push("--", source);
  const samples = [];
  for (let run = 0; run < measurementSamples + warmupSamples; run += 1) {
    const { stdout } = runParserCommand(
      arguments_,
      mode === "valid" ? [0] : [0, 1],
    );
    const durations = [
      ...stdout.matchAll(/(Parse|Edit):[ ]+([0-9.]+) ms/g),
    ].map((match) => Number(match[2]));
    assert.equal(durations.length, edits.length > 0 ? 2 : 1, stdout);
    assert.ok(durations.every(Number.isFinite), stdout);
    if (run >= warmupSamples) samples.push(durations);
  }
  return samples[0].map((_, index) => {
    const sorted = samples.map((sample) => sample[index]).sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)];
  });
}

function reportOrdinaryDurations(context, name, durations) {
  context.diagnostic(
    `${name}: parse/edit sequence times ${durations.join(" / ")}ms`,
  );
  for (const duration of durations) {
    assert.ok(
      duration <= ordinaryDurationLimitMs,
      `${name}: ${duration}ms exceeds ${ordinaryDurationLimitMs}ms`,
    );
  }
}

test("dash: local edits retain fresh CSTs within the ordinary editing budget", (context) => {
  for (const count of commandCounts) {
    const contents = commandLine.repeat(count);
    const source = writeSource(`commands-${count}`, contents);
    for (const index of [0, Math.floor(count / 2), count - 1]) {
      const name = `${count} commands, edit at ${index}`;
      const edit = {
        byte: commandLine.length * index + commandLine.indexOf("value"),
        deleteBytes: "value".length,
        insert: "other",
      };
      const final = writeSource(name, applyEdits(contents, [edit]));
      assertIncrementalEqualsFresh(source, final, name, edit);
      reportOrdinaryDurations(context, name, medianDurations(source, [edit]));
    }
  }
});

test("dash: nested expansion edits stay within the ordinary editing budget", (context) => {
  for (const depth of nestingDepths) {
    for (const [name, open, close, leaf] of [
      ["command", "$(", ")", "printf value"],
      ["quoted parameter", '"${x:-', '}"', "value"],
      ["arithmetic", "$((", "))", "1"],
    ]) {
      const prefix = `: ${open.repeat(depth)}`;
      const contents = `${prefix}${leaf}${close.repeat(depth)}\n`;
      const source = writeSource(`${name}-${depth}`, contents);
      const edit = {
        byte: prefix.length + leaf.length - 1,
        deleteBytes: 1,
        insert: "2",
      };
      const final = writeSource(`${name}-edited`, applyEdits(contents, [edit]));
      assertIncrementalEqualsFresh(source, final, name, edit);
      reportOrdinaryDurations(
        context,
        `${name} depth ${depth}`,
        medianDurations(source, [edit]),
      );
    }
  }
});

test("dash: here-document edits and structure repairs stay within the ordinary editing budget", (context) => {
  const bodyLineCount = 500;
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
    const source = writeSource(name, contents);
    const final = writeSource(`${name}-edited`, applyEdits(contents, [edit]));
    if (name === "function closer") {
      runParse({
        source,
        edits: [edit],
        expectedSource: final,
        mode: "recovery",
      });
    } else {
      assertIncrementalEqualsFresh(source, final, name, edit);
    }
    assertIncrementalEqualsFresh(
      source,
      source,
      `${name} repaired`,
      edit,
      repair,
    );
    reportOrdinaryDurations(
      context,
      `${name} changed`,
      medianDurations(source, [edit], "recovery"),
    );
    reportOrdinaryDurations(
      context,
      `${name} repaired`,
      medianDurations(source, [edit, repair]),
    );
  }
});

test("dash: large commands and here-document bodies stay within the growth budget", (context) => {
  for (const fixture of [
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
      source: (count) =>
        `cat <<'END'\n${"$name body text\n".repeat(count)}END\n`,
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
  ]) {
    const durations = [];
    for (const count of [
      fixture.units,
      fixture.units * scalingInputMultiplier,
    ]) {
      const source = writeSource(
        `${fixture.name}-${count}`,
        fixture.source(count),
      );
      durations.push(medianDurations(source)[0]);
    }
    const [small, large] = durations;
    context.diagnostic(`${fixture.name}: ${small}ms to ${large}ms`);
    assert.ok(
      large <= small * maximumDurationMultiplier + scalingSlackMs,
      `${fixture.name}: ${small}ms to ${large}ms exceeds growth budget`,
    );
  }
});

test("here-document body size is independent of serialized delimiter capacity", () => {
  const bodyLineCount = 4096;
  const scannerStateCapacityBytes = 1024;
  for (const quoted of [false, true]) {
    const body = "ordinary body text\n".repeat(bodyLineCount);
    const output = parseValidCst(
      writeSource(
        "long-body",
        `cat <<${quoted ? "'END'" : "END"}\n${body}END\n`,
      ),
    );
    assert.match(output, /end: here_document_end/);
    const retained = `: \`cat <<${quoted ? "'A'" : "A"} <<B)v=${body}A\n$(printf X)\nB\n\`\n`;
    const initial = writeSource("long-retained-body", retained);
    const byte = retained.indexOf("v=");
    const edit = { byte, deleteBytes: 1, insert: "1" };
    const final = writeSource(
      "long-retained-body-edited",
      applyEdits(retained, [edit]),
    );
    assertIncrementalEqualsFresh(initial, final, "long retained body", edit);
  }
  const source = writeSource(
    "delimiter-capacity",
    `cat <<'${"D".repeat(scannerStateCapacityBytes)}'\nbody\n`,
  );
  assertRepeatedColdParse("resource", source, "oversized delimiter");
});

test("dash: limit inputs complete with deterministic resource recovery", () => {
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
    assertRepeatedColdParse("resource", writeSource(name, contents), name);
  }
});
