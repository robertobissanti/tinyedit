# Changelog

## Unreleased

### New features

- Toggle cursor blinking for block and bar shapes from F2 or the View menu;
  save the choice as `cursor_blink`, off by default.

- Include the personal `zshrc.conf` example and synchronize the installed
  color scheme files, including the customized One Dark palette. The Zsh
  example is retained verbatim and is not loadable by the current syntax format.

- Place Color Scheme below Mode and show only valid presets matching ANSI or
  RGB. Include separate ANSI adaptations of One Dark and Catppuccin Mocha.

- Add an installable basic Vimscript definition for `.vim`, `.vimrc` and
  `.gvimrc`, with commands, single-quoted strings and comments.

- Select complete ANSI/RGB presets from `~/.tinyedit/color-scheme/` in F2 Colors,
  preview and confirm them in the shared draft, then save to update configuration.
  Invalid presets leave settings intact; terminal output preference is preserved.
- Include Catppuccin Mocha alongside the One Dark preset.

- Ship a complete One Dark RGB colorscheme adapted from Vim One, with
  instructions to apply it while preserving other editor settings.

- Configure a separate Markdown heading background in RGB mode, with preview
  in Colors; ANSI retains heading inversion.

- Add an installable Bash syntax definition for `.sh`, `.bash` and Bash/Zsh startup dotfiles with reserved
  words, builtin commands, strings, comments and function declarations.

- Choose optional RGB colors with validated #RRGGBB input, live samples and
  independent ANSI/RGB palettes; select true color or an explicit ANSI fallback.
- Configure colors in one F2 Colors page with Interface / Syntax highlighting groups,
  with shared drafts, Back navigation, mouse interaction and scroll. Settings
  labels always use terminal defaults, keeping color mistakes recoverable.

- Create an empty unnamed document with File → New or Ctrl-N, with the shared
  save/discard/cancel protection and preserved sidebar tree. Cmd-N is available
  in the opt-in experimental Ghostty Command-key mode; real-terminal validation
  of the new shortcuts remains required.

- Show the source build identifier with --version, in the startup splash and F3;
  distinguish Git commits, local changes and source archives, with a packaging override.

### Important fixes

- Match One Dark RGB Markdown to the personal Vim One colors: blue headings,
  orange-red bold and orange math, with a more distinct blue-gray heading
  background (`#354151`). Heading
  and preprocessor text share the blue role.

- Use a light gray selection in One Dark ANSI so selected text remains readable.

- Recognize startup dotfiles such as `.vimrc` and `.zshrc` when opened by a
  relative name, consistently with absolute paths. Dots in parent directories
  no longer count as a file extension.

- Keep the Settings draft open and live settings unchanged when saving fails,
  allowing retry or discard instead of applying an unsaved configuration.

- Navigate into directory symlinks from the sidebar with double-click or Right,
  instead of failing with “Can't open file: Is a directory”. Linked directories
  have the same triangle and on-demand expansion as ordinary folders, while
  retaining their distinct symlink color.

### Other minor fixing

- Split the full user, configuration, syntax and development guides into
  `docs/`, keeping a shorter README with quick reference and documentation links.
  Add C99, supported-platform and runtime-dependency badges.

- Refresh the README feature overview, Settings and color scheme walkthrough
  with new screenshots, retaining the previous Settings image. Document cursor
  blinking, mode-specific presets and startup-dotfile syntax support.

- Simplify color scheme selection to an inline Left/Right picker in Colors,
  with Enter to accept and Esc to cancel, removing the separate list and
  second confirmation.

- Move Markdown reverse heading colors into Colors alongside syntax colors.

- Keep color setting names and order identical in ANSI and RGB mode, including
  function names and Markdown highlighting.

- Clear residual search mode and navigation state when resetting a document
  through New, Close or Open.

- Refresh the installed build identity even for rapid successive commits on
  make implementations with coarse timestamp resolution; identify source
  archives independently of any enclosing Git repository.

- Apply swapped top bar colors explicitly so dark palette choices are sent
  as dark backgrounds instead of relying on terminal reverse-video rendering.

- Handle interrupted and partial terminal writes instead of ignoring their
  results, resolving fortified Linux build warnings.
- Document installation permissions, user-local installation, and the optional
  Linux clipboard tools, including wl-clipboard for Arch Linux on Wayland.
  Clarify that make install never runs a package manager and clipboard tools
  must be installed separately for sharing text with other applications.

- Show the Ctrl-E shortcut beside “Show/hide file tree” in the View menu.

- Reverse the top bar’s configured text and background colors to distinguish
  the document title from the menu and bottom status bar, with or without menus.

- Keep the current document name and modified indicator in the top bar while
  browsing the sidebar, instead of replacing them with the selected path.

- Reuse configured syntax colors in the sidebar instead of fixed ANSI colors:
  keyword for folders, preprocessor for links and the root, normal text for files.

- Update the README contents, give the sidebar its own feature row, and link
  every feature area to the relevant README section.

## 0.3.7 — 2026-10-08

### New features

- Record bounded differential undo/redo history with recoverable allocation
  failures and a memory budget, avoiding full-document copies for each edit.
- Configure bracket colors independently and use a dedicated JSON key color.

- Complete file paths with Tab in Open, Save and Save As prompts.
- Browse a lazy filesystem sidebar with keyboard and mouse navigation,
  directory rooting, focus switching and protection for unsaved edits.

### Important fixes

- Treat percent signs in search queries as literal replacement-prompt text,
  preventing crashes and memory corruption when replacing queries such as `%n`.
- Recover backup content exactly for filenames containing newlines, and reject
  mismatched path metadata while retaining compatibility with existing backups.
- Reject overflowing terminal numeric fields and invalid mouse coordinates.
- Reject named pipes and device files promptly instead of blocking the editor
  or reading an unbounded stream.
- Reject files containing NUL bytes without replacing the current document.
- Keep Markdown selections visible over heading and inline styles.

### Other minor fixing

- Check allocation-size arithmetic and render UTF-8 menu labels correctly.
- Keep private planning and analysis files under the ignored `local/` folder,
  and update public documentation links.

- Clip messages at grapheme boundaries, preserve flag-pair navigation, and
  display filenames without emitting terminal controls or malformed UTF-8.
- Correct and complete private helper and public API documentation.
- Document the sidebar with a screenshot and define release-note and commit
  conventions for contributors.
