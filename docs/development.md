# Development and internals

[Documentation index](README.md) · [tinyedit overview](../README.md)

## Contents

- [Code layout](#code-layout)
- [Function documentation](#function-documentation)

## Code layout

- `src/`, `inc/`: C source files and corresponding public/internal headers.
  `Makefile` supplies `-Iinc` to every compilation.
- `src/tinyedit.c`, `inc/tinyedit.h`: Application flow, editing commands,
  session/view orchestration, frame drawing and the settings panel.
  Shared types and macros live in the header, while implementation logic stays
  in the `.c` file.
- `src/alloc.c`, `inc/alloc.h`: Checked application allocations. Failure exits
  through the registered terminal cleanup handlers.
- `src/buffer.c`, `inc/buffer.h`: Dynamic logical rows and source-text mutations.
- `src/history.c`, `inc/history.h`: Differential undo/redo for changed rows,
  owned source swaps, cursor restoration, edit coalescing and transactional
  rollback. Both stacks share a memory budget.
- `src/render.c`, `inc/render.h`: UTF-8 layout, wrapping caches and conversions
  between source positions and visual rows/columns.
- `src/terminal.c`, `inc/terminal.h`: Raw mode, terminal feature setup/cleanup,
  key decoding and terminal I/O.
- `src/tree.c`, `inc/tree.h`: Owned filesystem tree, lazy expansion, sorting,
  collapse, root replacement and sidebar width calculation.
- `src/search.c`, `inc/search.h`: Literal and POSIX regex matching, with
  reusable compiled queries and cached text for multiline search. Returns
  source-byte coordinates; the editor owns session navigation and applies
  results to its cursor and highlight. Text caches are invalidated after edits,
  undo/redo and document replacement.
- `src/clipboard.c`, `inc/clipboard.h`: System clipboard integration (macOS
  `pbcopy`/`pbpaste`, Linux `wl-clipboard` or `xclip`), falling back
  to an internal buffer when no system backend is available. Wired to
  the editor via `Ctrl-C`/`Ctrl-X`/`Ctrl-V`.
- `src/utf8.c`, `inc/utf8.h`: UTF-8 decoding, grapheme cluster boundaries,
  and terminal display-width calculation, ported from linenoise. Used for cursor movement, backspace, and rendering.
- `src/settings.c`, `inc/settings.h`: Persistence of user settings
  (`~/.tinyeditrc`), independent ANSI/RGB palettes, shared color escape
  generation and a descriptor table driving the parser and F2 submenus.
- `src/command.c`, `inc/command.h`: Shared command labels, shortcuts, and
  links to settings toggles.
- `src/menu.c`, `inc/menu.h`: Menu bar and popup drawing, plus keyboard and
  mouse navigation.
- `src/syntax.c`, `inc/syntax.h`: Syntax highlighting, with a generic tokenizer
  for "C-like" languages driven by per-language tables (including
  ones loaded at runtime from `~/.tinyedit/syntax/*.conf`), plus
  dedicated tokenizers for Markdown, HTML/XML, and CSS. A `.conf` can
  route its language through the markup tokenizer (`base_tokenizer =
  xml`) and declare embedded `template_delimiters`, which is how the
  HTML-template languages are supported without compiled-in code.
- `src/editor_state.c`, `inc/editor_state.h`: Selection range normalization
  and the shared ASCII auto-close pair policy used by typing and closer skipping.
- `src/fileio.c`, `inc/fileio.h`: Staged document loading, exact LF/CRLF
  terminator handling, atomic file replacement and directory synchronization.
  Open and Save as expand a leading `~` or `~/` using `HOME`, keeping the
  expanded document identity for future saves and recovery. Other filename
  characters stay literal. A failed load keeps the current document; Save as commits its name only
  after successful file and directory synchronization. If replacement succeeds
  but directory synchronization fails, the editor retains its old name, dirty
  state and recovery copies and reports that durability is unconfirmed.
- `src/backup.c`, `inc/backup.h`: Periodic crash-recovery backups, saved to
  `~/.tinyedit/backup/` (never next to the original file). Wired to
  the editor via the `backup_interval` setting.
- `syntax-configs/`: Ready-made `.conf` language definitions to copy
  into `~/.tinyedit/syntax/` (or install with `make install-syntax`):
  Bash/startup dotfiles, basic Vimscript, LaTeX, Matlab/Octave, and the markup
  templates Nunjucks, Jinja,
  Liquid and Twig. They are data, not code; see its own
  [`README.md`](../syntax-configs/README.md).
- `src/linenoise.c`, `inc/linenoise.h`: linenoise's original sources
  (antirez), kept for historical reference from the first
  line-editor prototype. Not compiled into the current binary (except
  for the UTF-8 logic, ported separately into `utf8.c`).

## Function documentation

Functions use Doxygen-style comments in English, including private helpers and
historical linenoise code. `@brief` explains the purpose in one sentence;
`@details` describes how to call the function, its side effects and ownership;
`@param` clarifies arguments whose units or roles need explanation; `@return`
describes results and failure values; `@note` preserves useful design rationale.
Only include tags that add information, rather than restating the signature.

Benchmark methodology and limits are described in
[`tests/README.md`](../tests/README.md).
`make benchmark` measures CPU work without terminal I/O. Wrapped drawing locates
its first visible segment once, then walks the viewport; character counts and
matching pairs reuse document caches, invalidated after source changes and
history restoration. Pair results also depend on the cursor position. Output
buffers grow geometrically. Undo records only modified rows and structural row
changes. A one-byte edit retains the affected row, so very long single lines
still cost their full row size. Undo/redo transfer owned source spans without
allocating source text; replay display text is prepared before changing the
history position.

`undo_memory_mb` defaults to **64 MiB** and limits the combined undo/redo
journal. It is available in F2 and `~/.tinyeditrc`; changes take effect after
closing/opening a document or restarting the editor. `undo_max_depth` remains
a second limit. Successful edits may remove the oldest complete actions, with
a status message. An individual action exceeding the budget is cancelled and
its text, cursor, selection and existing history are restored.

The journal charges metadata and the maximum source capacity required by each
record in either direction. A pending edit has a separate reservation, also
bounded by the same budget, so retained plus pending journal reservations can
reach twice the setting. The live document, row-vector capacity, temporary
render/highlight caches and allocator overhead are additional costs; this
setting is not a limit on process RSS or a guarantee against kernel OOM kills.
No compression or external dependencies are used.

Application allocations use `teMalloc`/`teRealloc`/`teStrdup` and terminate with
terminal cleanup on failure. Recording and replay preparation use documented
recoverable `teTryMalloc`/`teTryRealloc`: failed journal/source allocations roll
back the entire edit; failed replay rendering leaves undo/redo unchanged and
can be retried. Highlight allocation failure falls back to unstyled text.
Other application allocations retain the existing controlled-exit contract. POSIX APIs that allocate internally (`getline`,
`realpath`) have a separate recoverable error contract: load/save/backup report
failure and preserve the active document. Only `ENOENT` permits the path fallback
for a target that has not been created yet.

Public contracts are available in `inc/` and beside their definitions in `src/`;
private helpers are documented at their definitions. State explicitly whether
positions are source-byte offsets, render-byte offsets, display columns or
visual rows, and whether range ends are exclusive. For returned allocations,
explain who releases them and whether they are NUL-terminated. Editing helpers
also say who records undo and refreshes derived row data. Keep comments aligned
with behavior when changing a function; Doxygen is optional and is not a build
dependency.
