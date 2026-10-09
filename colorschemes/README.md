# Colorschemes

Complete RGB color presets for tinyedit. `one-dark.conf` adapts
[rakr/vim-one](https://github.com/rakr/vim-one) with `background=dark`.
It includes every configurable RGB color role, using One's status-line colors
rather than a separate Airline theme. Syntax groups are adapted to tinyedit;
this does not add Vim's syntax parsers or reproduce every Vim highlight group.

## Apply

These files are configuration snippets, not automatically discovered themes.
Copy the entries from `one-dark.conf` into `~/.tinyeditrc`, replacing matching
entries and keeping unrelated settings, then restart tinyedit.

Alternatively, append the preset (later entries take precedence):

```sh
cat colorschemes/one-dark.conf >> ~/.tinyeditrc
```

Repeated application this way adds duplicate entries; replacing matching
entries is preferable for ongoing maintenance. F2 saves a normalized config.

True color output is selected explicitly. On a terminal without it, use
`rgb_output = ansi-fallback`; the approximation depends on its ANSI palette.
The preset preserves the independently saved ANSI colors and leaves Markdown
bold/italic preferences unchanged. Settings labels retain terminal defaults.
