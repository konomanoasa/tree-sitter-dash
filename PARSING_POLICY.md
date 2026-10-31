# Parsing Policy

`dash` parses source using [dash 0.5.13.5](https://manpages.debian.org/experimental/dash/dash.1.en.html)
default settings.

The following commands can affect how subsequent source is parsed. The parser
recognizes these commands but does not apply their effects.

| Command            | Effects not applied                                 |
| ------------------ | --------------------------------------------------- |
| `alias`, `unalias` | Alias substitution or changes to alias definitions. |
| `set`              | Changes to shell option settings.                   |
