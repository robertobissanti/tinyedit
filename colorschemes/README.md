# Colorschemes

Complete RGB presets adapted to tinyedit's color roles:

- `one-dark.conf`: [Vim One](https://github.com/rakr/vim-one), `background=dark`.
- `catppuccin-mocha.conf`: [Catppuccin Mocha](https://github.com/catppuccin/catppuccin).

These palettes do not add Vim's syntax parsers or duplicate every Vim
highlight group. One uses its own status-line colors, not an Airline theme.

## Install and select

```sh
mkdir -p ~/.tinyedit/color-scheme
cp colorschemes/*.conf ~/.tinyedit/color-scheme/
```

In **F2 → Colors**, activate **Choose Color Scheme >** with Enter or Right.
The same row becomes **Choose Color Scheme (use < > to change) name**.
Left/Right (or mouse wheel) cycles the installed presets and updates the color
samples on the Colors page. Enter applies the displayed preset to the draft;
Esc cancels and restores the previous draft. There is no separate screen or
second confirmation. Invalid presets are reported and cannot be applied.

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
