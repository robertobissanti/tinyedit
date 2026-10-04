# Contributing to tinyedit

Thank you for helping improve tinyedit. Contributions can be bug reports,
feature proposals, documentation corrections, tests, or code.

tinyedit is a small, full-screen terminal editor written in C99 for POSIX
systems. Its central design goals are a familiar editing experience, a small
and understandable codebase, predictable terminal cleanup, correct UTF-8
handling, and no third-party runtime dependencies.

## Before you start

Please read these documents before proposing or implementing a change:

- [`README.md`](README.md) explains the editor, build process, behavior, and
  source layout.
- [`IDEAS.md`](IDEAS.md) collects possible future features that have not been
  prioritized.

Check existing issues and pull requests as well, so that work is not
duplicated. For a substantial feature, a behavior change, a new dependency,
or a change to an open design decision, start a discussion in an issue before
writing the implementation. Small bug fixes, tests, and documentation fixes
can usually go directly to a pull request.

## Reporting a bug

Open a GitHub issue and include enough information for another person to
reproduce the problem:

- what you expected and what happened instead;
- the smallest reproducible sequence of steps;
- the tinyedit version or commit;
- operating system and version;
- terminal emulator and version;
- relevant settings from `~/.tinyeditrc`;
- a small sample file, when the problem depends on its contents;
- terminal output, screenshots, or a recording when they clarify the issue.

Before reporting lost modifier keys or an unexpected shortcut, see
[Testing terminal input](#testing-terminal-input). Different terminal
emulators can send different byte sequences for the same physical keys.

Do not include private files, secrets, tokens, or other sensitive information
in an issue or attachment.

## Proposing a feature

Open an issue describing:

- the problem or editing workflow the feature would improve;
- the proposed user-visible behavior;
- alternatives or workarounds considered;
- effects on portability, terminal compatibility, memory use, and code size;
- any new key binding, configuration option, or file-format change.

Keep proposals consistent with tinyedit's focused scope. In particular:

- C99 and POSIX are the supported foundation;
- the editor remains a single binary with no third-party runtime libraries;
- `ncurses` and other TUI frameworks are deliberately not used;
- new shortcuts must account for differences among terminal emulators;
- a small feature specific to the editor core does not automatically need a
  new module.

Do not add third-party code or dependencies without prior maintainer
agreement.

## Development setup

You need a C99 compiler, `make`, Python 3 for the PTY test suite, and a POSIX
system such as macOS or Linux.

Build the editor from the repository root:

```sh
make
```

The resulting executable is `bin/tinyedit`. Run the complete automated suite
with:

```sh
make test
```

`make test` builds with stricter warnings than the normal target and runs the
unit and PTY tests. A contribution is expected to build without new warnings
and to keep this command passing.

Add or update tests for changed behavior whenever it can be exercised
reliably. Features tied to a real terminal may also require documented manual
testing.

## Coding guidelines

### Types

Use explicit-width types consistently by concept:

- Use signed `int32_t` for positions, indices, lengths, and dimensions in the
  file or on screen: cursor coordinates, row counts, terminal dimensions,
  scroll offsets, `erow` lengths, wrap columns, and similar values. Signed
  values are required because the editor uses sentinels such as `-1` and
  directly compares or subtracts related values.
- Use `uint8_t` for boolean flags outside `struct editorSettings`.
- Use `uint8_t` for raw bytes read from the terminal or stored in UTF-8
  buffers.
- Use `uint32_t` for decoded Unicode code points.
- Keep byte lengths and offsets used with C or POSIX interfaces such as
  `memcpy()` and `strlen()` as `size_t`.
- Preserve types required by C and POSIX APIs. Examples include
  `int main(int argc, char **argv)`, `void handleWinch(int sig)`, and `int`
  file descriptors returned by `open()`.

`struct editorSettings` is a deliberate exception: every field in that
structure is `int32_t`, including boolean settings. `settingSlot()` and
`settingsScreenSlot()` access its fields generically through offsets and
`int32_t *`; do not change individual fields to `uint8_t` without redesigning
those accessors too.

These rules must reduce, not create, casts. If assigning two values to the
same nominal type would require an otherwise unnecessary cast, choose the
type that keeps the operation natural and reconsider which type family the
value belongs to.

### UTF-8 and text handling

`row->chars` and `row->render` contain raw bytes intended to represent UTF-8,
but a file may also contain malformed or truncated sequences. Never walk these
buffers byte by byte while assuming that one byte is one character or one
terminal column, and never assume that every stored sequence is valid UTF-8.

Use the helpers in `utf8.c` and `utf8.h`:

- `utf8ByteLen()`
- `utf8DecodeChar()`
- `utf8PrevCharLen()`
- `utf8NextCharLen()`
- `utf8CharWidth()`
- `utf8StrWidth()`
- `utf8SingleCharWidth()`

`utf8DecodeChar()` receives the number of bytes available and returns a
`struct utf8DecodeResult` with the code point, consumed byte count, and a
validity flag. It consumes zero bytes for empty input and one byte for a
malformed or truncated sequence. Callers must check validity before using
the code point for grapheme or width calculations.

A character can occupy one to four bytes and a different number of display
columns. When an operation needs both a byte offset and its visual column,
track both explicitly while advancing by the length returned from
`utf8NextCharLen()` or `utf8PrevCharLen()`. Do not assume that the two offsets
are interchangeable.

UTF-8 decoding must always be bounded by the number of bytes remaining in the
buffer. A leading byte alone is not proof that the announced two-, three-, or
four-byte sequence is available, and decoding must never read past the supplied
length. A decoder must reject isolated continuation bytes, truncated sequences,
overlong encodings, UTF-16 surrogate code points, and values above `U+10FFFF`.

Strict validation must not make an otherwise editable file impossible to open.
On malformed input, navigation and rendering must make progress safely, using
one offending byte as the recovery unit, while preserving that original byte in
the document buffer. A replacement glyph may be used for display, but opening
or rendering a file must not silently replace or discard its bytes. This keeps
valid bytes immediately after an error independently decodable and allows an
unchanged file to be saved byte-for-byte. Keep this recovery policy separate
from grapheme-cluster segmentation and terminal-width calculation, which apply
after successful decoding.

Test text changes with more than ASCII. Useful valid cases include accented
characters, combining marks, CJK characters, emoji, and text near a soft-wrap
boundary. Malformed-input tests must also cover invalid leading and continuation
bytes, overlong encodings, surrogate encodings, values above `U+10FFFF`, and
sequences truncated at every possible byte boundary.

### Dynamic memory

Application code must use `teMalloc()`, `teRealloc()`, and `teStrdup()` from
`alloc.c` and `alloc.h`. Do not call `malloc()`, `realloc()`, or `strdup()`
directly.

These helpers fail fast on allocation failure and call `exit()`, allowing the
registered `atexit()` handlers to restore raw mode, mouse tracking, bracketed
paste, the alternate screen, and other terminal state. Because `teRealloc()`
does not return `NULL`, its result may replace the original pointer directly.

If a new API must recover from an allocation failure instead of terminating,
document that contract explicitly. Such an API must keep the original pointer
until allocation succeeds and propagate the error to its caller.

### Source organization

Keep one clear responsibility per module without over-engineering. Every C
module follows this order:

1. feature-test macros, then project and system `#include` directives;
2. `#define` constants and macros;
3. types;
4. global and static variables;
5. functions, grouped into functional sections.

The module's own header comes first among includes so it is checked for
self-containment, followed by other project headers and then system headers.
Feature-test macros such as `_DEFAULT_SOURCE` must remain before every include.

Place shared macros in headers and keep implementation-only macros in the
`.c` file. Put `enum`, `struct`, and `typedef` declarations in headers, even
when currently used by a single translation unit. Organize functions with the
section style already used by the project, for example
`/* ---- terminal ---- */`.

When adding or removing an application module, or substantially changing a
module's responsibility, update the English **Module map** at the top of
`src/tinyedit.c` in the same change. Keep `src/linenoise.c` listed as
historical for as long as it remains in the repository, even though it is not
compiled.

When modifying an area that does not yet follow the prescribed order, bring
that area into order as part of the change instead of appending new code at
the end of the file.

### Comments and scope

Write comments where the reason for a choice is not evident from the code.
Avoid comments that merely restate an operation.

Keep pull requests focused. Do not combine an unrelated refactor, formatting
rewrite, or cleanup with a behavioral change. Preserve established behavior
unless changing it is part of the proposal.

## Testing terminal input

Do not infer modified-key sequences from one terminal or from documentation
alone. Ghostty, iTerm2, Terminal.app, xterm, Linux VTE terminals, and terminal
multiplexers can encode the same key combination differently.

For a new or changed shortcut:

- capture the actual bytes in each terminal claimed to be supported;
- report the operating system, terminal, and version tested;
- verify the full interaction in a real terminal;
- document terminals that cannot distinguish the requested combination;
- avoid changing an existing binding based only on an emulated PTY test.

On macOS, Python harnesses based on `pty.openpty()` may silently consume
control bytes such as `0x11` and `0x13` because of XON/XOFF processing before
they reach tinyedit. A failed automated attempt to send `Ctrl-Q` or `Ctrl-S`
through that harness is therefore not sufficient evidence of an editor bug;
repeat the check in a real terminal.

## Documentation

Update user-facing documentation in the same pull request as the behavior it
describes. Depending on the change, this may include:

- shortcuts, settings, features, or code layout in `README.md`;
- planned work and design decisions in the related issue or pull request;
- non-prioritized future directions in `IDEAS.md`;
- sample syntax configurations and `syntax-configs/README.md`;
- the Module map in `src/tinyedit.c`.

Do not mark planned work complete until its implementation and relevant tests
or manual verification are included.

## Submitting a pull request

Create a topic branch from the current default branch, make focused commits,
and open a pull request with a concise title. The description should state:

- what changed and why;
- related issue numbers;
- important design choices and tradeoffs;
- automated tests run and their results;
- manual tests, including operating systems and terminal emulators;
- known limitations or follow-up work;
- screenshots or recordings for visible interface changes, when useful.

Before submitting, check that:

- [ ] the change is focused and agrees with an accepted issue or a documented
      design decision;
- [ ] `make` succeeds without new warnings;
- [ ] `make test` passes;
- [ ] new behavior has appropriate automated or documented manual coverage;
- [ ] UTF-8-sensitive code was tested with non-ASCII input;
- [ ] terminal key sequences were verified in real terminals when relevant;
- [ ] documentation and the Module map were updated where needed;
- [ ] no third-party dependency or copied code was added without prior
      agreement and attribution;
- [ ] temporary files, generated binaries, editor settings, and unrelated
      changes are not included.

Review may ask for changes to keep the implementation portable, focused, or
consistent with the project's architecture. A pull request may be declined
when its goal does not fit the project even if the implementation is sound.

## Licensing and attribution

By contributing, you agree that your contribution may be distributed under
the repository's [MIT License](LICENSE).

Do not copy code, tests, documentation, or assets from another project unless
their license is compatible and the contribution includes every required
notice and attribution. Identify the source and license in the pull request.
When compatibility or attribution is uncertain, discuss it before including
the material.

## Community expectations

Be respectful, constructive, and specific in issues, reviews, and pull
requests. Discuss the code and the design rather than the person, assume good
intent, and make room for contributors with different levels of experience.
Harassment, personal attacks, and discriminatory behavior are not acceptable.
