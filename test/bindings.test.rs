use tree_sitter::{InputEdit, Parser, Point, Query};
use tree_sitter_dash as grammar;

fn parser() -> Parser {
  let mut parser = Parser::new();
  parser.set_language(&grammar::LANGUAGE.into()).unwrap();
  parser
}

#[test]
fn parses_valid_source() {
  let source = "f() printf '%s\\n' hello; f\n";
  let tree = parser().parse(source, None).unwrap();
  let root = tree.root_node();
  assert_eq!(root.kind(), "program");
  assert_eq!(root.byte_range(), 0..source.len());
  assert!(!root.has_error(), "{}", root.to_sexp());
  assert!(grammar::NODE_TYPES.contains("\"program\""));
  Query::new(&grammar::LANGUAGE.into(), grammar::HIGHLIGHTS_QUERY).unwrap();
}

#[test]
fn independent_and_reused_parsers_recover_after_function_body_edits() {
  let source = "f() { printf '終😀'; }\n";
  let byte = source.find('}').unwrap();
  let column = byte;
  let mut incremental = parser();
  let mut tree = incremental.parse(source, None).unwrap();
  tree.edit(&InputEdit {
    start_byte: byte,
    old_end_byte: byte + 1,
    new_end_byte: byte,
    start_position: Point::new(0, column),
    old_end_position: Point::new(0, column + 1),
    new_end_position: Point::new(0, column),
  });
  let broken = source.replacen('}', "", 1);
  let mut broken_tree = incremental.parse(&broken, Some(&tree)).unwrap();
  assert!(broken_tree.root_node().has_error());
  let fresh = parser().parse(source, None).unwrap();
  assert!(!fresh.root_node().has_error());
  broken_tree.edit(&InputEdit {
    start_byte: byte,
    old_end_byte: byte,
    new_end_byte: byte + 1,
    start_position: Point::new(0, column),
    old_end_position: Point::new(0, column),
    new_end_position: Point::new(0, column + 1),
  });
  let old_tree = if broken_tree.root_node().has_error() {
    None
  } else {
    Some(&broken_tree)
  };
  let repaired = incremental.parse(source, old_tree).unwrap();
  let reused = incremental.parse(source, None).unwrap();
  for tree in [&repaired, &reused] {
    assert!(!tree.root_node().has_error());
    assert_eq!(tree.root_node().to_sexp(), fresh.root_node().to_sexp());
    assert_eq!(tree.root_node().byte_range(), 0..source.len());
  }
}

#[test]
fn nul_edits_and_independent_here_document_parsers_match_fresh_trees() {
  fn point(bytes: &[u8]) -> Point {
    let mut point = Point::new(0, 0);
    for byte in bytes {
      if *byte == b'\n' {
        point.row += 1;
        point.column = 0;
      } else {
        point.column += 1;
      }
    }
    point
  }
  fn signature(node: tree_sitter::Node<'_>) -> String {
    let mut result =
      format!("{}:{}:{:?}", node.kind(), node.is_named(), node.range());
    for index in 0..node.child_count() {
      result.push_str(&format!("{:?}", node.field_name_for_child(index)));
      result.push_str(&signature(node.child(index).unwrap()));
    }
    result
  }
  let mut incremental = parser();
  let mut independent = parser();
  for source in [
    "if :; then :; fi\n",
    "cat <<'E'\nbody\nE\n",
    "cat <<E\nbody\n",
    ": a\\\nb\n",
  ] {
    let mut tree = incremental.parse(source, None).unwrap();
    assert!(!tree.root_node().has_error());
    let decorated = format!(
      "\0{}\0",
      source
        .chars()
        .map(|c| c.to_string())
        .collect::<Vec<_>>()
        .join("\0")
    );
    tree.edit(&InputEdit {
      start_byte: 0,
      old_end_byte: source.len(),
      new_end_byte: decorated.len(),
      start_position: Point::new(0, 0),
      old_end_position: point(source.as_bytes()),
      new_end_position: point(decorated.as_bytes()),
    });
    let fresh = independent.parse(&decorated, None).unwrap();
    let edited = incremental.parse(&decorated, Some(&tree)).unwrap();
    assert!(
      !fresh.root_node().has_error(),
      "{}",
      fresh.root_node().to_sexp()
    );
    assert_eq!(signature(edited.root_node()), signature(fresh.root_node()));
  }
}
