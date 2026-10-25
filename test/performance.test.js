import assert from "node:assert/strict";
import { test } from "node:test";
import { grammars } from "../scripts/tree-sitter.js";
import {
  parserLibrary,
  runParserCommand,
  writeSource,
} from "./support/parser.js";
import {
  editingWorkloads,
  scaleFactors,
  scalingWorkloads,
} from "./support/workloads.js";

const measurementSamples = 3;
const warmupSamples = 1;
const ordinaryDurationLimitMs = 1000;
const maximumDurationMultiplier = 8;
const scalingSlackMs = 100;

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

for (const { name, source, edits } of editingWorkloads()) {
  test(`dash: ${name} stays within the ordinary editing budget`, (context) => {
    reportOrdinaryDurations(
      context,
      name,
      medianDurations(writeSource(name, source), edits, "recovery"),
    );
  });
}
for (const { name, units, source } of scalingWorkloads) {
  test(`dash: ${name} stays within the growth budget`, (context) => {
    const durations = scaleFactors.map(
      (factor) =>
        medianDurations(
          writeSource(`${name}-${factor}`, source(units * factor)),
        )[0],
    );
    context.diagnostic(`${name}: ${durations.join(" / ")}ms`);
    for (const duration of durations.slice(1)) {
      assert.ok(
        duration <= durations[0] * maximumDurationMultiplier + scalingSlackMs,
        name,
      );
    }
  });
}
