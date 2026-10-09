# Ready-made syntax configurations

Drop-in language definitions for tinyedit's user syntax mechanism. These
are plain configuration files, not compiled code: they cover languages
that aren't built into `syntax.c` but whose grammar reduces to the
"C-like" shape the generic tokenizer already handles (keywords, strings,
comments, numbers).

## Installing

Copy the ones you want into `~/.tinyedit/syntax/`, which tinyedit scans
at startup:

```bash
mkdir -p ~/.tinyedit/syntax && cp syntax-configs/*.conf ~/.tinyedit/syntax/
```

Or just one:

```bash
mkdir -p ~/.tinyedit/syntax && cp syntax-configs/nunjucks.conf ~/.tinyedit/syntax/
```

Restart tinyedit and open a file with a matching extension. Every
`.conf` in that directory is loaded; a malformed file is skipped
silently, and a file missing `extensions` or `keywords` is ignored
entirely. A user file may redefine an extension that ships built in, and
it wins.

## What's here

| File | Languages | Extensions |
| --- | --- | --- |
| `bash.conf` | Bash (overrides built-in Shell; generic tokenizer) | `.sh` `.bash`, Bash/Zsh startup dotfiles |
| `vimrc.conf` | Basic Vimscript (generic tokenizer) | `.vim` `.vimrc` `.gvimrc` |
| `latex.conf` | LaTeX | `.tex` `.latex` `.sty` `.cls` |
| `matlab.conf` | Matlab / Octave | `.m` `.mat` |
| `json.conf` | JSON (also built in; keys and string values use separate colors) | `.json` |
| `nunjucks.conf` | Nunjucks (Eleventy) | `.njk` `.nunjucks` |
| `jinja.conf` | Jinja2 (Flask, Ansible) | `.jinja` `.jinja2` `.j2` |
| `liquid.conf` | Liquid (Shopify, Jekyll) | `.liquid` |
| `twig.conf` | Twig (Symfony) | `.twig` |

Bash covers reserved words, builtins, quoted strings, comments and function
declarations. It uses the generic tokenizer: heredocs, nested expansions and
shell-specific quoting/comment edge cases are not fully parsed. Zsh keeps its
built-in definition. The override also recognizes `.bashrc`, `.bash_profile`,
`.bash_login`, `.bash_logout`, `.profile`, `.zshrc`, `.zshenv`, `.zprofile`,
`.zlogin` and `.zlogout`. `.vimrc` is Vimscript and is not claimed. Other
files without an extension are not detected by shebang.

The last four are markup templates: they set `base_tokenizer = xml`, so
HTML tags and attributes highlight as in an `.html` file, plus
`template_delimiters` for the `{{ }}` / `{% %}` / `{# #}` blocks.

Each file also declares a `filetype` name, so installing it is enough
to get both the highlighting and the language name in the status bar —
no `~/.tinyeditrc` editing needed. The first time you open a matching
file, tinyedit records the name in `~/.tinyeditrc` for you. One
consequence worth knowing: since `~/.tinyeditrc` is consulted first,
changing a `filetype` value here later won't override the name already
recorded there.

## Writing your own

The format and every available key are documented in the main
[`README.md`](../README.md), under "Extending syntax highlighting".

Three things are worth knowing before you start, because none of them
fails loudly — the file loads fine and the highlighting is just wrong:

- **Pick the right base.** The default tokenizer only knows keywords,
  strings, comments and numbers. If your language is markup with
  something embedded in it, set `base_tokenizer = xml` and describe the
  embedded parts with `template_delimiters`; trying to express it as
  keywords leaves all the surrounding tags uncolored.
- **`#` needs escaping in values.** It starts a comment in this file
  format, so a delimiter like `{# #}` has to be written `{\# \#}`.
- **`keyword_prefix_chars` merges tokens.** Those characters become
  identifier characters, so a delimiter listed that way fuses with the
  word after it into a single token and the word is never matched
  against the keyword list. Put delimiters *or* words in `keywords`,
  not both.

The practical way to check a config is to open a representative file
and look at it. A language needing structure neither base provides —
indentation-sensitive blocks, nested sub-languages — still needs a
dedicated tokenizer in `syntax.c`, the way Markdown and CSS are
handled.

`vimrc.conf` recognizes `.vimrc`, `./.vimrc` and absolute paths equally,
using extension `vimrc` (no leading dot in
configuration). Its generic tokenizer treats double quotes as comments and
single quotes as strings; context-dependent double-quoted Vimscript strings,
command abbreviations and full Vim9 syntax are not parsed.

`zshrc.conf` is a verbatim personal example, retained for reference. It is
currently ignored: extension values omit the leading dot, `base_tokenizer = bash`
is unsupported, and a generic definition requires `keywords`. Use `bash.conf`
for working shell highlighting, including `.zshrc`.
