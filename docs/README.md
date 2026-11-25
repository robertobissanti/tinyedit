# tinyedit documentation

tinyedit is a terminal text editor with familiar shortcuts, UTF-8 editing,
mouse selection, a file tree and Markdown navigation. These guides explain
how to use it, customize it and develop it.

## Start here

- Open a file with `Ctrl-O`, save with `Ctrl-S`, quit with `Ctrl-Q`.
- Use `Ctrl-F` to find text, `Ctrl-Z` to undo and `Ctrl-Y` to redo.
- Press `F2` for settings, `F1` for the shortcut reference, `F10` for menus.
- Show the file tree with `Ctrl-E`; double-click a file to open it.

The installed guides are read-only: you can search, select and copy text.
Use Alt+Enter on a link to follow it. With mouse support enabled, double-click
links, or enable Markdown live preview to use hover and a single click.
Ctrl-O opens your own file; Ctrl-W closes the guide and leaves a new document.

For installation and the project background, read the
[project overview](../README.md).

[Back to top](#tinyedit-documentation)

## Choose a guide

- [User guide](usage.md): Shortcuts, files, sidebar, menus, mouse, editing,
  search and recovery

- [Settings, colors and themes](configuration.md): F2, ANSI/RGB palettes,
  color schemes, previews, cursor and line-ending settings

- [Syntax highlighting](syntax-highlighting.md): Built-in languages, Markdown,
  custom definitions and filetype detection

- [Development and internals](development.md): Module responsibilities, memory
  contracts and function documentation


Ready-made files: [color schemes](../colorschemes/README.md) and
[syntax definitions](../syntax-configs/README.md).
Contributor workflow: [CONTRIBUTING.md](../CONTRIBUTING.md).
Testing and benchmarks: [tests/README.md](../tests/README.md).

[Back to top](#tinyedit-documentation)

## Markdown and mouse navigation

- [Smart Table](usage.md#smart-table): create and realign Markdown tables,
  choose column alignment and navigate cells, with before/after screenshots.
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

[Back to top](#tinyedit-documentation)
