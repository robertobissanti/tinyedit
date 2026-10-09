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

```sh
mkdir -p ~/.tinyedit/color-scheme
cp colorschemes/*.conf ~/.tinyedit/color-scheme/
```

ANSI files are `one-dark-ansi.conf` and `catppuccin-mocha-ansi.conf`.
Their shades depend on the terminal palette and approximate the RGB originals.

In **F2 → Colors**, choose **Mode** first. **Choose Color Scheme >** sits
immediately below it and lists only valid presets declaring that mode.
Switching Mode changes the available schemes without replacing either palette.
Activate **Choose Color Scheme >** with Enter or Right.
The same row becomes **Choose Color Scheme (use < > to change) name**.
Left/Right (or mouse wheel) cycles the installed presets and updates the color
samples on the Colors page. Enter applies the displayed preset to the draft;
Esc cancels and restores the previous draft. There is no separate screen or
second confirmation. Invalid or incompatible presets are excluded from the list.

Confirming a preset does not write configuration immediately. Ctrl-S/F2 in
Settings saves all draft changes to `~/.tinyeditrc`; discarding Settings keeps
the previous colors. A failed save retains the draft for retry or discard.

## File format

Each `.conf` is a plain `key = value` file using the same color names and
hex syntax as `~/.tinyeditrc`. Comments start with `#`; RGB hex values retain
their leading `#`. `color_mode = ansi` or `color_mode = rgb` is required.

An RGB preset must define every `SETTING_RGB` key, including
`rgb_markdown_heading_background`. An ANSI preset must define every ANSI
`color_*` role plus `markdown_heading_reverse`. Missing roles, duplicate keys,
invalid values, unknown keys and non-color settings reject the entire file.
The other palette is preserved unless its entries are explicitly included.

For compatibility with standalone snippets, `rgb_output` is accepted and
validated, but its value is not applied: terminal truecolor/fallback remains
an independent user choice. Non-color settings and filetype overrides are
preserved. Settings labels always use terminal defaults.

You can also replace the corresponding entries in `~/.tinyeditrc` manually.
