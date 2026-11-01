# Parsing Policy

`dash` parses source using [dash 0.5.13.5](https://manpages.debian.org/experimental/dash/dash.1.en.html)
default settings.

These commands are parsed without changing how subsequent source is parsed.

| Command            | Effects not applied                 |
| ------------------ | ----------------------------------- |
| `alias`, `unalias` | Alias substitution and definitions. |
| `set`              | Shell option changes.               |
