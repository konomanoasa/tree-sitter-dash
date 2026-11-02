import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { copyFileSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { packageName, root } from "../../scripts/tree-sitter.js";

const buildTimeoutMs = 120_000;
const commandOutputBytes = 64 * 1024 * 1024;

function run(command, args, cwd) {
  const result = spawnSync(command, args, {
    cwd,
    encoding: "utf8",
    timeout: buildTimeoutMs,
    maxBuffer: commandOutputBytes,
    killSignal: "SIGKILL",
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  return result.stdout;
}

test("npm archives build installable Node.js bindings with working queries and node metadata", () => {
  const directory = mkdtempSync(join(tmpdir(), `${packageName}-node-`));
  try {
    const npm = process.env.npm_execpath;
    assert.ok(npm, "Run Node.js build checks through npm exec.");
    const [archive] = Object.values(
      JSON.parse(
        run(
          process.execPath,
          [
            npm,
            "pack",
            "--json",
            "--ignore-scripts",
            "--pack-destination",
            directory,
          ],
          root,
        ),
      ),
    );
    writeFileSync(
      join(directory, "package.json"),
      JSON.stringify({
        private: true,
        type: "module",
        allowScripts: {
          "tree-sitter-dash": true,
          "tree-sitter": true,
        },
      }),
    );
    run(
      process.execPath,
      [npm, "install", join(directory, archive.filename)],
      directory,
    );
    copyFileSync(
      join(root, "test/bindings.test.js"),
      join(directory, "bindings.test.js"),
    );
    run(process.execPath, ["--test", "bindings.test.js"], directory);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
