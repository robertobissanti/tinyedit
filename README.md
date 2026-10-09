

<div align="center">

<img src="imgs/tinyedit_logo_colore128x128.png" alt="Logo">


# tinyedit

[![GitHub release](https://img.shields.io/github/v/release/robertobissanti/tinyedit?sort=semver)](https://github.com/robertobissanti/tinyedit/releases)
[![License](https://img.shields.io/github/license/robertobissanti/tinyedit)](LICENSE)
[![C99](https://img.shields.io/badge/standard-C99-blue)](#build)
[![Platforms](https://img.shields.io/badge/platform-macOS%20%7C%20Linux-lightgrey)](#build)
[![No runtime dependencies](https://img.shields.io/badge/runtime%20dependencies-none-brightgreen)](#build)
[![Written in C](https://img.shields.io/github/languages/top/robertobissanti/tinyedit)](https://github.com/robertobissanti/tinyedit)
[![Repository size](https://img.shields.io/github/repo-size/robertobissanti/tinyedit)](https://github.com/robertobissanti/tinyedit)

</div>

A small full-screen terminal text editor written in plain C (kilo-style,
after [kilo](https://github.com/antirez/kilo) by Salvatore Sanfilippo),
with no dependencies beyond the POSIX standard library.

tinyedit brings the familiar ease of a desktop text editor to the terminal.
It uses the terminal's alternate screen while open, then returns to the main
screen on exit with the shell prompt and scrollback intact.

Traditional terminal editors can require learning modal editing (Vim),
memorizing non-standard key sequences (Emacs), or working around limited
navigation and selection (Nano). tinyedit reduces that friction with familiar
shortcuts such as `Ctrl-C`, `Ctrl-V`, `Ctrl-Z`, and `Ctrl-F`, Shift+Arrow text
selection, and optional mouse support for clicking and scrolling.

## Contents

- [Why it exists](#why-it-exists)
- [Features at a glance](#features-at-a-glance)
- [Homebrew](#homebrew-macos--linux)
- [Build and build identification](#build)
- [Usage and installation](#usage)
- [Quick reference](#quick-reference)
- [Documentation](#documentation)
- [Project status](#project-status)
- [Author and license](#author-and-license)

## Why it exists

I kept building tinyedit because I became convinced that a terminal editor
could be simple enough to use every day. While implementing it, I paid close
attention to carrying over the mouse gestures and keyboard shortcuts people
already know from desktop text editors and word processors. The goal is not
to invent another editing language: it is to make opening a terminal file
feel immediately familiar.

There is still room to grow, such as adding multiple buffers to keep several
files open in one session. Future additions will be evaluated carefully to
keep the editor simple and focused.

![tinyedit editing a Python file with syntax highlighting](imgs/python-syntax-highlighting.png)

*Python source in tinyedit, with line numbers, soft wrapping, file statistics,
and configurable syntax colors.*

## Features at a glance

tinyedit is intentionally small, but it is meant to be comfortable enough
for real editing rather than just demonstrating how a terminal works.

| Area | What you get |
|---|---|
| [Editing](docs/usage.md#keyboard-shortcuts) | Familiar cursor movement, word jumps, selection, cut/copy/paste, automatic indentation, block indent/outdent with Tab, configurable pair and XML/HTML tag closing, matching-bracket highlighting, and an undo history of up to 2,000 steps (200 by default). |
| [Files](docs/usage.md#opening-and-closing-files) | Start an empty document with File → New / Ctrl-N, open or switch files without restarting tinyedit, complete file paths with Tab, start a named file before it exists, save atomically, and recover unsaved work from automatic backups after a crash. |
| [Sidebar](docs/usage.md#file-tree-sidebar) | Browse a persistent filesystem tree with keyboard or mouse, expand folders lazily, change its root, and open files with protection for unsaved edits. |
| [Search](docs/usage.md#find-and-replace) | Incremental literal or POSIX regular-expression search, match navigation, and interactive search and replace. |
| [Syntax highlighting](docs/syntax-highlighting.md#syntax-highlighting) | Built-in support for C/C++, Python, Shell, JavaScript/TypeScript, Markdown, HTML/XML, CSS, and JSON, including function names. Simple C-like languages and HTML-based templates (Nunjucks, Jinja, Liquid, Twig) can be added with a user configuration file; ready-made ones ship in `syntax-configs/`. |
| [UTF-8](docs/usage.md#utf-8-text) | Cursor movement, deletion, display width, wrapping, and character counts understand combining marks, CJK text, and multi-code-point emoji. |
| [Long lines](docs/usage.md#soft-wrapping-and-navigation) | Lines wrap at the terminal edge, preferably at word boundaries. Navigation follows the visible wrapped rows, without imposing a fixed line-length limit. |
| [Clipboard](docs/usage.md#keyboard-shortcuts) | Uses the native macOS clipboard or the available Wayland/X11 clipboard tool directly, without sending commands through a shell. |
| [Terminal input](docs/usage.md#fast-terminal-paste) | Fast bracketed paste, optional mouse selection and scrolling, and key-sequence handling for common macOS and Linux terminals. |
| [Menus](docs/usage.md#menus) | A persistent menu bar with TinyEdit, File, Edit, View, and Help menus; keyboard and optional mouse navigation; shortcuts beside commands; and checked View settings. |
| [Interface](docs/usage.md#top-and-status-bars) | Optional line numbers and top bar, visible whitespace, file statistics, in-editor help, and a settings panel. |
| [Configuration](docs/configuration.md#settings-and-appearance) | Settings live in `~/.tinyeditrc`; tabs, wrapping, mouse behavior, cursor shape and blinking, interface elements, and editing assists can all be changed from `F2`. |
| [Colors and themes](docs/configuration.md#colors-page-and-rgb) | ANSI by default or optional RGB with validated hex colors, previews, separate palettes, and One Dark / Catppuccin Mocha presets selected from one Colors page. |
| [Build identification](#build-identification) | The same release and source build identifier appears in `--version`, the startup splash, and F3. |
| [Portability](#build) | One C99 binary and no third-party runtime libraries. The supported targets are POSIX systems such as macOS and Linux. |
| [Testing](docs/development.md#code-layout) | Syntax, settings, backup, terminal-input, key-binding, and very-long-line behavior are covered by `make test`; sample files are included for hands-on checks. |

Settings can be changed from the built-in `F2` panel or by editing
`~/.tinyeditrc`, which tinyedit creates automatically on first launch.

The project also remains an exploration of how terminal interfaces work from
scratch: raw mode, ANSI escape sequences, input decoding, and manual redraw,
without reaching for a TUI framework. Its first prototype used
[linenoise](https://github.com/antirez/linenoise) for a command-driven line
editor. The current version is a true full-screen editor with a freely moving
cursor; linenoise remains in the repository for historical reference but is
not a build dependency.


## Homebrew (macOS / Linux)

If you use Homebrew on macOS or Linux, install tinyedit with

```sh
brew install robertobissanti/tinyedit/tinyedit

```

## Build

```sh
make        # produces the bin/tinyedit binary
make clean  # removes it
```

Needs only a C99 compiler and a POSIX system (macOS or Linux).

### Build identification

`bin/tinyedit --version`, the startup splash and F3 display the same complete
version and build identifier. Git builds use `g` plus twelve commit digits;
local source changes add `-dirty-s<checksum>`. Archives without Git use
`source-s<checksum>`. The POSIX checksum covers source/header files, the
Makefile and build generator; it is not cryptographic and collisions are possible.
Identical sources retain the same ID; compiler, flags and platform are not encoded.
The generated header is checked on every make, without requiring make clean.
The application is rebuilt each time, including on make implementations with
coarse timestamp resolution; unchanged generated header content is retained.
Packagers may override it with `make BUILD_ID=package-0.3.7-r2` (up to 120
letters, digits, dots, underscores, plus signs or hyphens). The packager owns
the accuracy and uniqueness of this override.


## Usage

```sh
bin/tinyedit [file]
bin/tinyedit --version  # print version and build ID without starting the editor
```

### Optional command installation

The build output stays in `bin/tinyedit` without modifying your shell
configuration. To run `tinyedit` from any directory, install or symlink it
into a location in your `PATH`, such as `/usr/local/bin`:

```sh
make
sudo make install PREFIX=/usr/local
```

For an installation without administrator privileges, use
`make install PREFIX="$HOME/.local"` and add `~/.local/bin` to your `PATH`.
A permission error for `/usr/local/bin` is an installation failure; the compiled
binary remains available as `bin/tinyedit`.

## Quick reference

| Key | Action |
|---|---|
| `Ctrl-N` / `Ctrl-O` | New document / open file |
| `Ctrl-S` / `F4` | Save / Save as |
| `Ctrl-W` / `Ctrl-Q` | Close document / quit, with unsaved-change protection |
| `Ctrl-Z` / `Ctrl-Y` | Undo / redo |
| `Ctrl-F` | Find and replace |
| `Ctrl-E` | Show or hide the file tree |
| `F1` / `F2` / `F3` | Help / Settings / Info |
| `F10` | Open the menu |

F2 includes cursor shape and blinking, editing preferences, and a **Colors**
page with ANSI/RGB palettes and live previews. One Dark and Catppuccin Mocha
presets are included. ANSI is the default; RGB output is an explicit choice.
See the [settings and themes guide](docs/configuration.md) for installation
and configuration.

## Documentation

- [User guide](docs/usage.md): full shortcuts, file operations, sidebar,
  mouse, menus, editing, search and backup recovery.
- [Settings, colors and themes](docs/configuration.md): the shared F2 draft,
  ANSI/RGB, presets, cursor and line endings.
- [Syntax highlighting](docs/syntax-highlighting.md): supported languages,
  Markdown, startup dotfiles and custom syntax definitions.
- [Development and internals](docs/development.md): code layout, memory
  behavior and function documentation.
- [Contributing](CONTRIBUTING.md) and [tests and benchmarks](tests/README.md).

Browse the [documentation index](docs/README.md) for all guides and preset files.

## Project status

See [`IDEAS.md`](IDEAS.md) for future features not yet prioritized, and
the repository issues and pull requests for publicly discussed work.

## Author and license

Copyright © 2026 Roberto Bissanti <roberto.bissanti@gmail.com>.
Released under the MIT license; see [`LICENSE`](LICENSE). The code
ported from linenoise (`src/utf8.c`/`inc/utf8.h`, plus
`src/linenoise.c`/`inc/linenoise.h` vendored for historical reference,
see above) stays
under Salvatore Sanfilippo and Pieter Noordhuis's original BSD
2-Clause license; see
[`LICENSE-THIRD-PARTY`](LICENSE-THIRD-PARTY).
