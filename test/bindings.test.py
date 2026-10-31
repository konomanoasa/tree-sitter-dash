import unittest
from types import CapsuleType

import tree_sitter_dash

from tree_sitter import Language, Parser, Query, QueryCursor


class BindingsTest(unittest.TestCase):
    def test_language_capsule_parses_source_with_external_scanner(self):
        capsule = tree_sitter_dash.language()
        self.assertIsInstance(capsule, CapsuleType)
        language = Language(capsule)
        self.assertEqual(language.name, "dash")
        source = b"cat <<'EOF'\n$HOME\nEOF\n"
        tree = Parser(language).parse(source)
        self.assertEqual(tree.root_node.type, "program")
        self.assertFalse(tree.root_node.has_error)
        self.assertEqual(tree.root_node.text, source)

    def test_packaged_highlights_capture_here_document_terminator(self):
        language = Language(tree_sitter_dash.language())
        tree = Parser(language).parse(b"cat <<'EOF'\n$HOME\nEOF\n")
        query = Query(language, tree_sitter_dash.HIGHLIGHTS_QUERY)
        captures = QueryCursor(query).captures(tree.root_node)
        self.assertEqual(
            [
                (node.text, node.byte_range)
                for node in captures["label"]
                if node.type == "here_document_end_text"
            ],
            [(b"EOF", (18, 21))],
        )


if __name__ == "__main__":
    unittest.main()
