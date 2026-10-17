# Colorschemes

Complete RGB presets and separate ANSI adaptations, adapted to tinyedit's color roles:

- `one-dark.conf`: [Vim One](https://github.com/rakr/vim-one), `background=dark`.
- `catppuccin-mocha.conf`: [Catppuccin Mocha](https://github.com/catppuccin/catppuccin).

One Dark includes the personal Vim Markdown overrides: blue headings
(`#61AFEF`), orange-red bold (`#DE4000`), orange math (`#D19A66`). Tinyedit retains a heading background (`#354151`),
using a medium blue-gray shade to separate headings from the editor. Heading text shares tinyedit's preprocessor color,
so preprocessors also use blue. These overrides differ from stock Vim One.

These palettes do not add Vim's syntax parsers or duplicate every Vim
highlight group. One uses its own status-line colors, not an Airline theme.

## Install and select

`make install` includes shared resources under `$(PREFIX)/share/tinyedit`.
Personal files in `~/.tinyedit/` take precedence. The following commands are
optional for personal copies and should be run without `sudo`.

```sh
make install-colorschemes   # from the repository root; never overwrites existing files
# or: mkdir -p ~/.tinyedit/color-scheme && cp colorschemes/*.conf ~/.tinyedit/color-scheme/
```

ANSI files are `one-dark-ansi.conf` and `catppuccin-mocha-ansi.conf`.
Their shades depend on the terminal palette and approximate the RGB originals.

In **F2 → Colors**, choose **Mode** first. The **Color scheme** row always
shows the name of the installed preset matching the active palette, or
**Custom** for a palette that does not match. Left/Right immediately cycles
compatible presets in the Settings draft. There is no separate selector or
Enter confirmation; you can move to another row at once. Enter/Space or a click
also advances one preset; the mouse wheel over the row cycles in either direction.
Switching Mode changes the available schemes without replacing either palette.
Invalid or incompatible presets are excluded.

Ctrl-S/F2 saves all draft changes to `~/.tinyeditrc`; Esc returns from Colors
to the main Settings page while retaining the draft. Discarding Settings keeps
the previous colors. A failed save retains the draft for retry or discard.

Both RGB presets give parentheses and brackets a distinct yellow:
One Dark uses `#E5C07B`, Catppuccin Mocha uses `#F9E2AF`. Existing personal copies
are preserved by normal installation. To replace them with the updated shipped
presets, run `make install-colorschemes-force` (this overwrites personal preset
files), then re-select the theme and save Settings. Already saved palette values
are not changed by copying preset files alone.

## File format

Each `.conf` is a plain `key = value` file using the same color names and
hex syntax as `~/.tinyeditrc`. Comments start with `#`; RGB hex values retain
their leading `#`. `color_mode = ansi` or `color_mode = rgb` is required.

For compatibility, missing `color_syntax_italic` / `rgb_syntax_italic` inherit
the corresponding keyword color. New presets should include them explicitly.

An RGB preset must otherwise define every `SETTING_RGB` key, including
`rgb_markdown_heading_background`. An ANSI preset must define every ANSI
`color_*` role plus `markdown_heading_reverse`. Missing roles, duplicate keys,
invalid values, unknown keys and non-color settings reject the entire file.
The other palette is preserved unless its entries are explicitly included.

For compatibility with standalone snippets, `rgb_output` is accepted and
validated, but its value is not applied: terminal truecolor/fallback remains
an independent user choice. Non-color settings and filetype overrides are
preserved. Settings labels always use terminal defaults.

You can also replace the corresponding entries in `~/.tinyeditrc` manually.
