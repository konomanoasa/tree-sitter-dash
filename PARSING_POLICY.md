# Parsing Policy

The parser analyzes source as written. It parses the following commands but
does not apply their effects to parsing.

| Command            | Effects not applied                                               |
| ------------------ | ----------------------------------------------------------------- |
| `.`                | Loading another file or applying changes made by its execution.   |
| `alias`, `unalias` | Alias substitution or changes to alias definitions.               |
| `eval`             | Reparsing code constructed from its arguments.                    |
| `set`              | Changing parsing conditions in response to shell option settings. |
