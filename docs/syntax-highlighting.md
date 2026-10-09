# Syntax highlighting guide

[Documentation index](README.md) · [tinyedit overview](../README.md)

## Contents

- [Syntax highlighting](#syntax-highlighting)
- [Extending syntax highlighting](#extending-syntax-highlighting)
- [Markup templates](#markup-templates)
- [Ready-made syntax configurations](#ready-made-syntax-configurations)

## Syntax highlighting

With `syntax_highlight` on (default), files get highlighted based on
their extension: keywords/types, strings, comments (line and
multi-line block where applicable), and numbers, each with its own
configurable color (`color_syntax_keyword`, `color_syntax_string`,
`color_syntax_comment`, `color_syntax_number`,
`color_syntax_preprocessor`, plus `color_syntax_emphasis_strong` for
Markdown bold text, kept distinct from italic which uses
`color_syntax_italic`, and `color_syntax_function` for function
names), using the active ANSI or RGB palette. Function
names are recognized by the same heuristic other lightweight editors
use, an identifier immediately followed by `(`, which covers both
calls and definitions. Text with no class at all (identifiers, punctuation,
whitespace) uses `color_syntax_normal`, defaulting to
`terminal-default`, a palette sentinel rather than a real hue meaning "no color forced, terminal's own
foreground", the same behavior this had before the setting existed;
set it to any real hue to recolor plain text explicitly. Natively
supported languages: C/C++ (`.c` `.h` `.cpp` `.cc` `.cxx` `.hpp` `.hh`
`.hxx`), Python (`.py`), Shell (`.sh` `.bash` `.zsh`), JavaScript/
TypeScript (`.js` `.jsx` `.ts` `.tsx`), Markdown (`.md` `.markdown`, with
headings, `` `inline code` ``, multi-line code fences, italic
`*...*`/`_..._`, bold `**...**`/`__...__` (both may span several lines),
links and images, YAML front matter, and
embedded HTML tags), HTML/XML (`.html` `.htm`
`.xml`, with tags, attributes, and `<!-- -->` comments), and CSS (`.css`, with
properties, values, comments).

Two constructs common in static-site Markdown get their own handling.
A YAML **front matter** block, the `---` delimited metadata header
used by Jekyll, Eleventy and Hugo, is highlighted as structured data
rather than prose: delimiters and the `:` as markers, keys as keywords,
values as strings. It's recognized only when the opening `---` is the
file's first line, so a `---` further down stays a horizontal rule.
Blank lines inside the block don't end it.

**HTML tags embedded in the document** (`<div class="box">`,
`<strong>`) are highlighted like they would be in an `.html` file,
tag names and attributes as keywords, quoted values as strings. The
scanner uses a conservative rule: a `<` must be followed by a
letter and reach a `>` on the same line, so prose like `5 < 7` is left
alone, and a tag inside a code span (`` `<div>` ``) or a fenced block
stays code.

**Links and images** are highlighted with the label and the
destination in different colors, so a row of badges stays readable
instead of drowning in URL text. The nested `[![alt](img)](url)` form
that badges use is handled, as are parentheses inside a URL. An
unmatched `[text]` is left as prose.

**Emphasis may span several lines**, which is common when a caption or
an italic sentence is wrapped across rows. A span is closed by its
matching marker or by a blank line. Bounding it at the paragraph
means a stray `*` in prose (`filetype.*`, `5 * 3`) can't recolor the
rest of the document. A marker followed by whitespace isn't treated as
an opener at all.

![Markdown editing and syntax highlighting in tinyedit](../imgs/markdown-editing.png)

*Editing this README demonstrates Markdown highlighting, line numbers, word
wrapping, and the persistent top and status bars.*

JSON object keys have their own color (`color_syntax_json_key`, cyan by
default); string values use `color_syntax_string`. A quoted string followed
by a colon on the same logical line is recognized as a key, including escaped
quotes and Unicode text. JSON highlighting is built in; existing `json.conf`
definitions remain compatible.

The **Syntax: bracket color (all files)** option in `F2`
(`color_syntax_bracket`, yellow by default) colors `()`, `[]`, and `{}`,
including in unnamed files and files without a recognized syntax. It also
works with syntax highlighting disabled. With syntax highlighting enabled,
strings, comments, and other classified spans keep their own colors.
Selection, search matches, and matching-bracket highlighting take priority.

## Extending syntax highlighting

To add a "C-like" language (keywords + strings + comments, e.g.
Matlab, Go, Rust, Java) without recompiling, drop a file at
`~/.tinyedit/syntax/<name>.conf`; the filename itself doesn't matter,
only its contents, in the same format as `~/.tinyeditrc` (`key =
value`, `#` for comments):

```
extensions = m,mat
keywords = function,end,if,else,elseif,for,while,switch,case,return
quote_chars = '"
line_comment = %
block_comment_start = %{
block_comment_end = %}
```

`extensions`/`keywords` are comma-separated lists; both are required
(a file missing either is ignored entirely), everything else is
optional. `hash_line_is_preprocessor = true` highlights a line
starting with `#` in full, as a preprocessor directive (used by C/C++;
doesn't make sense for most other languages). `keyword_prefix_chars`
extends which characters can start a keyword beyond letters/
underscore, useful for languages where keywords have a special
prefix, e.g. LaTeX (`keyword_prefix_chars = \`, keywords like
`\begin`, `\section`, etc.). `math_mode = true` recognizes and
highlights `$formula$`/`$$formula$$` and the equivalent
`\(formula\)`/`\[formula\]` forms (same mechanism used for Markdown
above) anywhere in the text, not just inside keywords.
`\[...\]` is recognized even when its delimiters sit on separate lines
from the formula's content (common in LaTeX); the other three forms
stay single-line. `filetype = <Name>` sets the status-bar language name
for the extensions this file claims (see below). A full LaTeX example,
`~/.tinyedit/syntax/latex.conf`:

```
extensions = tex,latex,sty,cls
keywords = \begin,\end,\section,\label,\ref,\cite,\textbf,\textit
line_comment = %
keyword_prefix_chars = \
math_mode = true
```

<p align="center">
  <img src="../imgs/latex-syntax-highlighting.png" alt="LaTeX command syntax highlighting in tinyedit" width="49%">
  <img src="../imgs/latex-math-highlighting.png" alt="LaTeX mathematics syntax highlighting in tinyedit" width="49%">
</p>

*A user-defined LaTeX syntax configuration highlights commands and mathematical
expressions without adding a compiled-in language or an external dependency.*

### Markup templates

A template format like Nunjucks, Jinja, Liquid or Twig is HTML with a
second language embedded in it, which the C-like tokenizer above can't
express because it has no notion of a tag. Two extra keys route such a
language through the markup tokenizer instead:

```
extensions = njk,nunjucks
filetype = Nunjucks
base_tokenizer = xml
template_delimiters = {{ }}, {% %}, {\# \#}
```

`base_tokenizer = xml` highlights tags and attributes exactly as in an
`.html` file, and `template_delimiters` lists the embedded expression
delimiters as comma-separated `open close` pairs. Those blocks are highlighted
as a unit, with delimiters in the preprocessor color and contents in the
function color, even inside attribute values. For example,
`href="{{ url }}"` highlights the dynamic expression rather than rendering it
as a single string.
A pair whose opener starts with `{#` is treated as that language's
comment and colored like every other comment.

Note the `\#` escapes: `#` normally starts a comment in a `.conf` file,
so a `#` that's part of a value has to be escaped. With
`base_tokenizer = xml`, the `keywords` key isn't required (the markup
tokenizer doesn't use it).

Every `.conf` file in `~/.tinyedit/syntax/` gets loaded at startup
(silently skipped if malformed, same tolerance as `~/.tinyeditrc`); a
user file can redefine an extension already covered natively, and it
wins. Languages with a grammar that doesn't reduce to
keywords/strings/comments, where prefixed keywords and math mode
still are not enough, such as Markdown/HTML/CSS, cannot be extended
from an external file; they need a dedicated tokenizer in `syntax.c`.

## Ready-made syntax configurations

The [`syntax-configs/`](../syntax-configs/) directory includes a Bash override
for `.sh`/`.bash` and shell startup files such as `.bashrc`, `.profile` and
`.zshrc`, with extended builtin keywords (generic tokenizer; no full
heredoc or expansion parsing). The basic `vimrc.conf` definition covers
`.vim`, `.vimrc` and `.gvimrc`: commands, single-quoted strings and comments.
Double quotes are treated as comments; context-dependent strings, command
abbreviations and full Vim9 syntax need a more complete parser. Startup
dotfiles match with relative names (`.vimrc`, `./.zshrc`) and absolute paths
alike; `extensions` values omit the leading dot.

The personal `zshrc.conf` example is retained verbatim for reference and is
ignored by the current loader; install `bash.conf` for working `.zshrc`
highlighting. `base_tokenizer = bash` is not supported.

The directory also ships configuration
files for a few languages that aren't compiled in, so they can be used
without writing one from scratch: LaTeX (`.tex`, `.latex`, `.sty`,
`.cls`), Matlab/Octave (`.m`, `.mat`), and the markup templates
Nunjucks (`.njk`), Jinja (`.jinja`, `.j2`), Liquid (`.liquid`) and Twig
(`.twig`). Install them by copying into the directory tinyedit scans:

```bash
mkdir -p ~/.tinyedit/syntax && cp syntax-configs/*.conf ~/.tinyedit/syntax/
```

or, equivalently, from the repository root:

```bash
make install-syntax
```

To install both syntax definitions and color schemes, run `make install-resources`
as your normal user. It preserves existing files in both resource directories.

That syntax target never overwrites a file you already have (it prints
`skip` for those); use `make install-syntax-force` to replace them with
the shipped versions. Syntax installation is separate from the default build
step so compiling the editor never touches your home directory or modifies
your existing configurations.

Copy a single file instead of the whole set if you only want one. See
[`syntax-configs/README.md`](../syntax-configs/README.md) for what each one
covers and for two non-obvious constraints of the format: comment
delimiters are matched before keywords, and `keyword_prefix_chars` can
merge adjacent tokens. Both can produce wrong highlighting rather than a
load error when you hit them.

The status bar also shows the filetype detected from the extension
(e.g. `C`, `Python`, `Markdown`) next to the line/column position. It
covers roughly 30 common extensions; to add more or override a name,
add `filetype.<extension> = <Name>` lines to `~/.tinyeditrc` (e.g.
`filetype.m = Matlab/Octave`).

A syntax `.conf` can also carry that name itself, with a `filetype`
key, so one file defines both how a language is highlighted and what
it's called:

```
filetype = Nunjucks
```

When a file is opened, the name is resolved in this order:

1. **`~/.tinyeditrc` (or the built-in table) already knows the
   extension.** That name is used. If it came from a user override but
   no syntax `.conf` (and no compiled-in language) covers the
   extension, the status bar reports `Filetype 'X': highlight config
   missing`; the name shows but nothing gets colored, and this says
   why.
2. **Nothing knows it, but an installed `.conf` claims the extension
   and declares `filetype`.** The name is applied, highlighting works,
   and the entry is written to `~/.tinyeditrc` so the extension is
   recorded from then on.
3. **Neither.** The field is omitted, as before.

Because step 2 writes to `~/.tinyeditrc` and step 1 reads it first,
editing a `.conf`'s `filetype` afterwards won't change the name already
recorded there. Update the `filetype.<extension>` line in
`~/.tinyeditrc` (or delete it to let the `.conf` be consulted again).
