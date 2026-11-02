import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import load from "node-gyp-build";

const binding = load(fileURLToPath(new URL("../..", import.meta.url)));
binding.nodeTypeInfo = JSON.parse(
  readFileSync(new URL("../../src/node-types.json", import.meta.url), "utf8"),
);
binding.HIGHLIGHTS_QUERY = readFileSync(
  new URL("../../queries/highlights.scm", import.meta.url),
  "utf8",
);

export default binding;
