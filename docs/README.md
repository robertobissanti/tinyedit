# tinyedit documentation

[Project overview and installation](../README.md)

| Guide | Contents |
|---|---|
| [User guide](usage.md) | Shortcuts, files, sidebar, menus, mouse, editing, search and recovery |
| [Settings, colors and themes](configuration.md) | F2, ANSI/RGB palettes, color schemes, previews, cursor and line-ending settings |
| [Syntax highlighting](syntax-highlighting.md) | Built-in languages, Markdown, custom definitions and filetype detection |
| [Development and internals](development.md) | Module responsibilities, memory contracts and function documentation |

Ready-made files: [color schemes](../colorschemes/README.md) and
[syntax definitions](../syntax-configs/README.md).
Contributor workflow: [CONTRIBUTING.md](../CONTRIBUTING.md).
Testing and benchmarks: [tests/README.md](../tests/README.md).

## Markdown and mouse navigation

- [Markdown display and live preview](usage.md#markdown-display-and-live-preview):
  compact emphasis and link labels, source editing from link boundaries,
  hover colors and single-click opening.
- [Heading, Pandoc and Obsidian targets](usage.md#markdown-heading-attribute-and-block-targets):
  local references, cross-file fragments, exact IDs, duplicate errors and limits.
- [Images](usage.md#markdown-images): open a visible source path in the default
  desktop application while keeping the current document open.
- [Fenced code blocks](syntax-highlighting.md#fenced-code-blocks): language tags,
  installed syntax definitions, multiline comments and fallback code styling.
- [Mouse support](usage.md#mouse-support): selection and wheel scrolling with
  a stable viewport while moving the pointer.
