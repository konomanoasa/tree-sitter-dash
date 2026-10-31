import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdtempSync, readdirSync, readFileSync, rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { test } from "node:test";
import { copyFiles, packageName, root } from "../../scripts/tree-sitter.js";

const python = process.env.UV_PYTHON ?? "3.15";
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

test("Python metadata matches the grammar and license", {
  skip: Boolean(process.env.CI) && process.platform !== "darwin",
}, () => {
  const { project } = JSON.parse(
    run(
      "uv",
      [
        "run",
        "--no-project",
        "--python",
        python,
        "python",
        "-c",
        "import json, tomllib; print(json.dumps(tomllib.load(open('pyproject.toml', 'rb'))))",
      ],
      root,
    ),
  );
  const { metadata } = JSON.parse(
    readFileSync(join(root, "tree-sitter.json"), "utf8"),
  );
  assert.equal(project.name, `konomanoasa-${packageName}`);
  for (const key of ["version", "description", "license"]) {
    assert.equal(project[key], metadata[key]);
  }
  assert.equal(project.urls.Repository, metadata.links.repository);
});

test("sdist and npm archives build installable Python wheels with working bindings and queries", () => {
  const treeSitter = run(
    "uv",
    [
      "run",
      "--no-project",
      "--python",
      python,
      "python",
      "-c",
      "import tomllib; deps = tomllib.load(open('pyproject.toml', 'rb'))['dependency-groups']['dev']; print(next(dep for dep in deps if dep.startswith('tree-sitter==')))",
    ],
    root,
  ).trim();
  const directory = mkdtempSync(join(tmpdir(), `${packageName}-python-`));
  try {
    const source = join(directory, "source");
    copyFiles(
      [
        "pyproject.toml",
        "setup.py",
        "LICENSE",
        "README.md",
        "bindings/python",
        "src",
        "queries",
      ],
      source,
    );
    const npm = process.env.npm_execpath;
    assert.ok(npm, "Run Python build checks through npm exec.");
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
    run("tar", ["-xf", archive.filename], directory);
    for (const { name, input, options } of [
      { name: "sdist", input: source, options: [] },
      { name: "npm", input: join(directory, "package"), options: ["--wheel"] },
    ]) {
      const output = join(input, "dist");
      run(
        "uv",
        ["build", "--python", python, "--no-sources", ...options, input],
        directory,
      );
      const archives = readdirSync(output)
        .filter((file) => file.endsWith(".whl") || file.endsWith(".tar.gz"))
        .map((file) => join(output, file));
      run(
        "uv",
        [
          "run",
          "--no-sync",
          "--project",
          root,
          "twine",
          "check",
          "--strict",
          ...archives,
        ],
        directory,
      );
      const wheels = archives.filter((file) => file.endsWith(".whl"));
      assert.equal(wheels.length, 1);
      const environment = join(directory, `${name}-env`);
      run("uv", ["venv", "--python", python, environment], directory);
      const executable = join(
        environment,
        process.platform === "win32" ? "Scripts/python.exe" : "bin/python",
      );
      run(
        "uv",
        ["pip", "install", "--python", executable, wheels[0], treeSitter],
        directory,
      );
      run(executable, ["-I", join(root, "test/bindings.test.py")], directory);
      run(
        executable,
        [
          "-I",
          "-c",
          "from importlib.resources import files; p = files('tree_sitter_dash'); assert p.joinpath('py.typed').is_file(); assert p.joinpath('binding.pyi').is_file(); assert not p.joinpath('binding.c').is_file()",
        ],
        directory,
      );
    }
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
