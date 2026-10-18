import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";

const directory = path.resolve(import.meta.dirname, "../corpus");
const header = "==================\n";
const separator = "\n------------------\n";

export function corpusCases() {
  return fs
    .readdirSync(directory)
    .sort()
    .flatMap((file) => {
      const parts = fs
        .readFileSync(path.join(directory, file), "utf8")
        .split(header);
      assert.equal(parts.shift(), "", file);
      assert.equal(parts.length % 2, 0, file);
      const cases = [];
      for (let index = 0; index < parts.length; index += 2) {
        const [name, ...attributes] = parts[index].trimEnd().split("\n");
        assert.ok(
          attributes.every(
            (attribute) => attribute === ":cst" || attribute === ":error",
          ),
          name,
        );
        const end = parts[index + 1].indexOf(separator);
        assert.notEqual(end, -1, name);
        cases.push({
          name: `${file}: ${name}`,
          source: parts[index + 1].slice(0, end),
          mode: attributes.includes(":error") ? "recovery" : "valid",
        });
      }
      return cases;
    });
}
