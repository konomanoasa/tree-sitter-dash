import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { after, before } from "node:test";
import { createTreeSitter, grammars, root } from "../../scripts/tree-sitter.js";
import {
  assertCstOutputsEqual,
  assertCstSourceContract,
  assertFreshOutputsEqual,
  hasRecovery,
  nodeIdentity,
  parseCst,
  sourcePoint,
} from "./cst.js";

const grammarName = grammars[0].name;

const parseTimeoutMicros = 10_000_000;
const parserProcessTimeoutMs = 60_000;

let runtimeDirectory;
let treeSitter;
let parserLibrary;
let sourceSequence = 0;

before(() => {
  treeSitter = createTreeSitter();
  runtimeDirectory = treeSitter.directory;
  parserLibrary = path.join(
    runtimeDirectory,
    process.platform === "win32" ? "parser.dll" : "parser",
  );
  runParserCommand([
    "build",
    "--output",
    parserLibrary,
    path.join(root, grammars[0].path),
  ]);
});

after(() => {
  treeSitter?.close();
});

function applyEdits(source, edits) {
  let bytes = Buffer.from(source);
  for (const edit of edits) {
    const { byte, deleteBytes, insert } = edit;
    const description = JSON.stringify(edit);
    assert.ok(
      Number.isSafeInteger(byte) && byte >= 0,
      `invalid byte offset: ${description}`,
    );
    assert.ok(
      Number.isSafeInteger(deleteBytes) && deleteBytes >= 0,
      `invalid deletion length: ${description}`,
    );
    assert.equal(typeof insert, "string", `invalid insertion: ${description}`);
    assert.ok(
      byte <= bytes.length && deleteBytes <= bytes.length - byte,
      `edit exceeds ${bytes.length} source bytes: ${description}`,
    );
    bytes = Buffer.concat([
      bytes.subarray(0, byte),
      Buffer.from(insert),
      bytes.subarray(byte + deleteBytes),
    ]);
  }
  return bytes;
}

function formatEdit({ byte, deleteBytes, insert }) {
  return `${byte} ${deleteBytes} ${insert}`;
}

function runParserCommand(arguments_, allowedStatuses = [0]) {
  const result = treeSitter.run(arguments_, {
    timeout: parserProcessTimeoutMs,
  });
  assert.ifError(result.error);
  assert.ok(
    allowedStatuses.includes(result.status),
    result.stdout + result.stderr,
  );
  return result;
}

function writeSource(name, contents) {
  sourceSequence += 1;
  const sourcePath = path.join(
    runtimeDirectory,
    `${sourceSequence}-${name.replace(/[^A-Za-z0-9_.-]/g, "-")}.sh`,
  );
  fs.writeFileSync(sourcePath, contents);
  return sourcePath;
}

function runParse(options) {
  const {
    source,
    description = path.basename(source),
    edits = [],
    mode = "valid",
    expectedSource = source,
  } = options;
  const arguments_ = [
    "parse",
    "--lib-path",
    parserLibrary,
    "--lang-name",
    grammarName,
    "--cst",
    "--timeout",
    String(parseTimeoutMicros),
  ];
  if (edits.length > 0) {
    arguments_.push("--edits", ...edits.map(formatEdit));
  }
  arguments_.push("--", source);

  const result = runParserCommand(arguments_, [0, 1]);
  const output = result.stdout;
  const entries = parseCst(output);
  assert.notEqual(entries.length, 0, `${description}: parser produced no CST`);
  const rootType = nodeIdentity(entries[0]).type;
  const recovery = hasRecovery(output);

  if (mode === "valid") {
    assert.equal(
      result.status,
      0,
      `${description}: expected a valid parse\n${output}`,
    );
    assert.equal(
      rootType,
      "program",
      `${description}: missing program root\n${output}`,
    );
    assert.equal(
      recovery,
      false,
      `${description}: unexpected recovery\n${output}`,
    );
  } else if (mode === "resource") {
    assert.ok(
      rootType === "program" || rootType === "ERROR",
      `${description}: resource-bounded parse has no program or ERROR root\n${output}`,
    );
  } else {
    assert.equal(mode, "recovery", `${description}: unknown parse mode`);
  }

  const expectedBytes = fs.readFileSync(expectedSource);
  const expectedEnd = sourcePoint(expectedBytes, expectedBytes.length);
  const actualEnd = entries[0].range.split("-")[1];
  assert.equal(
    actualEnd,
    expectedEnd,
    `${description}: parser stopped at ${actualEnd} before source EOF ${expectedEnd}`,
  );
  if (mode === "valid") assertCstSourceContract(output, expectedBytes);
  return { output, status: result.status, recovery };
}

function parseValidCst(source, description = path.basename(source)) {
  return runParse({ description, source }).output;
}

function assertRepeatedColdParse(mode, source, name) {
  const first = runParse({
    description: `${name} first cold parse`,
    mode,
    source,
  });
  const second = runParse({
    description: `${name} second cold parse`,
    mode,
    source,
  });
  assertFreshOutputsEqual(name, first, second);
}

function assertIncrementalEqualsFresh(
  initialSource,
  finalSource,
  name,
  ...edits
) {
  assert.ok(
    edits.length > 0,
    `${name}: incremental parse requires an edit history`,
  );
  assert.ok(
    applyEdits(fs.readFileSync(initialSource), edits).equals(
      fs.readFileSync(finalSource),
    ),
    `${name}: edit sequence does not produce the final source`,
  );
  const incremental = runParse({
    description: `${name} incremental`,
    edits,
    expectedSource: finalSource,
    source: initialSource,
  });
  const fresh = runParse({ description: `${name} fresh`, source: finalSource });
  assertCstOutputsEqual(
    `${name} incremental and fresh parses`,
    incremental,
    fresh,
  );
}

export {
  applyEdits,
  assertIncrementalEqualsFresh,
  assertRepeatedColdParse,
  parserLibrary,
  parseValidCst,
  runParse,
  runParserCommand,
  writeSource,
};
