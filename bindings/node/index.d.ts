import type { Language } from "tree-sitter";

declare const binding: Language & {
  HIGHLIGHTS_QUERY: string;
};

export default binding;
