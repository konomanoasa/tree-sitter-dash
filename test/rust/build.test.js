import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  mkdirSync,
  mkdtempSync,
  readdirSync,
  readFileSync,
  rmSync,
} from "node:fs";
import { join } from "node:path";
import { test } from "node:test";
import { copyFiles, packageName, root } from "../../scripts/tree-sitter.js";

const archiveTimeoutMs = 120_000;
const commandOutputBytes = 64 * 1024 * 1024;

function run(command, arguments_, cwd = root) {
  const result = spawnSync(command, arguments_, {
    cwd,
    encoding: "utf8",
    timeout: archiveTimeoutMs,
    maxBuffer: commandOutputBytes,
    killSignal: "SIGKILL",
  });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
  return result.stdout;
}

test("Rust metadata matches the grammar and license", {
  skip: Boolean(process.env.CI) && process.platform !== "darwin",
}, () => {
  const { packages } = JSON.parse(
    run("cargo", [
      "metadata",
      "--locked",
      "--offline",
      "--no-deps",
      "--format-version",
      "1",
    ]),
  );
  const pkg = packages.find((item) => item.name === packageName);
  assert.ok(pkg);
  const { metadata } = JSON.parse(
    readFileSync(join(root, "tree-sitter.json"), "utf8"),
  );
  for (const key of ["version", "description", "license"])
    assert.equal(pkg[key], metadata[key]);
  assert.equal(pkg.repository, metadata.links.repository);
});

test("npm and Cargo archives contain buildable bindings for every language", () => {
  const cache = join(root, "node_modules", ".cache");
  mkdirSync(cache, { recursive: true });
  const directory = mkdtempSync(
    join(cache, `${packageName}-rust-distribution-`),
  );
  try {
    const npm = process.env.npm_execpath;
    assert.ok(npm, "Run Rust build checks through npm exec.");
    const [archive] = Object.values(
      JSON.parse(
        run(process.execPath, [
          npm,
          "pack",
          "--json",
          "--ignore-scripts",
          "--pack-destination",
          directory,
        ]),
      ),
    );
    const npmRoot = join(directory, "npm");
    mkdirSync(npmRoot);
    run("tar", ["-xf", archive.filename, "-C", "npm"], directory);
    const npmSource = join(npmRoot, "package");
    const cargoTarget = join(directory, "cargo-package");
    run("cargo", [
      "package",
      "--locked",
      "--offline",
      "--allow-dirty",
      "--no-verify",
      "--target-dir",
      cargoTarget,
    ]);
    const packageDirectory = join(cargoTarget, "package");
    const archives = readdirSync(packageDirectory).filter((name) =>
      name.endsWith(".crate"),
    );
    assert.equal(archives.length, 1);
    const cargoRoot = join(packageDirectory, "cargo");
    mkdirSync(cargoRoot);
    run("tar", ["-xf", archives[0], "-C", "cargo"], packageDirectory);
    const cargoSource = join(cargoRoot, archives[0].slice(0, -".crate".length));

    for (const source of [npmSource, cargoSource]) {
      copyFiles(["Cargo.lock"], source);
      const output = run("cargo", [
        "test",
        "--locked",
        "--offline",
        "--manifest-path",
        join(source, "Cargo.toml"),
        "--target-dir",
        join(directory, "build"),
        "--test",
        "bindings",
        "parses_valid_source",
        "--",
        "--exact",
      ]);
      assert.match(output, /^test result: ok[.] 1 passed;/m);
    }
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
