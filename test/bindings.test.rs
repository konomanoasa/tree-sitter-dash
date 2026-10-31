use tree_sitter::{Parser, Query, QueryCursor, StreamingIterator};
use tree_sitter_dash as grammar;

#[test]
fn parses_valid_source() {
  let source = "cat <<'EOF'\n$HOME\nEOF\n";
  let language = grammar::LANGUAGE.into();
  let mut parser = Parser::new();
  parser.set_language(&language).unwrap();
  let tree = parser.parse(source, None).unwrap();
  let root = tree.root_node();
  assert_eq!(root.kind(), "program");
  assert_eq!(root.byte_range(), 0..source.len());
  assert!(!root.has_error(), "{}", root.to_sexp());

  assert!(grammar::NODE_TYPES.contains("\"program\""));
  let query = Query::new(&language, grammar::HIGHLIGHTS_QUERY).unwrap();
  let mut cursor = QueryCursor::new();
  let mut captures = cursor.captures(&query, root, source.as_bytes());
  let mut terminators = 0;
  while let Some((matched, index)) = captures.next() {
    let capture = matched.captures()[*index];
    if capture.node.kind() == "here_document_end_text" {
      assert_eq!(query.capture_names()[capture.index as usize], "label");
      assert_eq!(capture.node.byte_range(), 18..21);
      assert_eq!(capture.node.utf8_text(source.as_bytes()).unwrap(), "EOF");
      terminators += 1;
    }
  }
  assert_eq!(terminators, 1);
}
