# Changelog

## Unreleased

### New features

### Important fixes

- Reject opening or saving through a symlink whose target is missing, preserving
  the link and unsaved document instead of replacing the link with a new file.

- Make `make install` include color schemes, additional syntax definitions and
  guides for every user, including installations through `sudo`. Load shared
  resources from `PREFIX/share/tinyedit` with personal resources taking priority;
  retain `make install-binary` for executable-only installations.

### Other minor fixing

- Display editor message-bar errors in red, keeping normal prompts and success
  messages distinct; document the error-color and missing-symlink policies.

- Clarify that recovery backups are opt-in and exclude unnamed documents by
  design. Explain the input/edit/display/save flow, how to read function
  contracts, and row-level undo storage separately from save/search copies.

## 0.3.8 — 2026-10-09

### New features

- Start an empty unnamed document with `tinyedit folder` instead of rejecting
  the directory. Show it as the sidebar root, keep focus in the document,
  and resolve relative Open/Save paths from that folder.

- Open inline Markdown links and web URLs with Alt+Enter or a double click;
  show a contextual shortcut hint, navigate local heading anchors, and protect
  modified documents with save/discard/cancel before switching files.
- Double-click ordinary text to select a word while preserving UTF-8 boundaries.
- Open installed Markdown guides from Help → Documentation. `make install-docs`
  installs the index and linked resources under `~/.tinyedit`, separately from
  active syntax definitions and color schemes.

- Configure Markdown italic text independently from keywords in ANSI and RGB,
  with a Colors preview. One Dark uses `#D19A67`; older configurations and
  presets without the new color retain their keyword color for italics.

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

- Ignore unbound control keys instead of inserting them into the document.
  Pressing Ctrl-D, Ctrl-Space (NUL), Ctrl-J (LF) or F10 with the menu disabled
  used to add raw bytes that were saved: a LF split the line on disk and a NUL
  made the file unopenable as binary by tinyedit itself.
- Never send control bytes from a document to the terminal. A stray carriage
  return (for example `\r\r\n` line endings) erased the whole visible row, and
  ESC or BEL bytes were emitted as terminal commands, also when hidden behind a
  zero-width joiner. The file bytes are unchanged; controls now occupy no
  cells, as before.
- Show the reason when a file named on the command line cannot be opened
  (directory, permission denied, binary). The message used to be printed on the
  alternate screen and vanished when tinyedit exited.
- Keep `#` inside `filetype.<ext>` labels such as `C#`. Opening Settings and
  saving used to truncate the label in `~/.tinyeditrc` to `C`; only ` #`
  (preceded by whitespace) starts a comment there.
- Sanitize and column-align the file name in the crash-recovery screen, so a name
  containing escape bytes cannot control the terminal.
- Stop an unsupported function key such as F5 (`ESC[15~`) from swallowing the
  next typed character.
- Ask before Save as replaces a different existing file (`y` confirms, any other
  key keeps the file and the document name). Saving over the file already open
  never asks.
- Stop the editor from hanging when a modified file's pathname has been replaced
  by a FIFO: the comparison with the disk now opens non-blockingly and treats
  anything but a regular file as different.
- Keep a symlinked `~/.tinyeditrc` (dotfile managers) as a link when Settings
  are saved: the file it points to is rewritten.
- Report an `xdg-open`/`open` launch failure reliably: the status pipe now
  retries interrupted and short writes instead of discarding their result
  (GCC warns about ignored `write()` results on Linux). The browser is still started
  asynchronously; a permanent status-pipe failure in the detached launcher
  remains unobservable.
- Retry an interrupted system-clipboard paste (for example during a window
  resize) instead of silently pasting the older internal clipboard.

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

- Explain Save as overwrite confirmation in the user guide and F1 help,
  settings saves through resolvable symlinks, combined resource installation,
  and the integrity and installer regression coverage.
- Record the future ideas for unified fatal-error handling and a general CSI key
  parser in `IDEAS.md`.
- Add `make install-colorschemes` (never overwrites; `-force` variant),
  `COLORSCHEME_DIR`, and `make install-resources` for color schemes plus syntax
  definitions. Stop and report failure if a preset cannot be copied, even when
  later files could be installed. `make install` still installs only the binary,
  because per-user files belong to the invoking user's home, and now says so. This fixes
  "No schemes in ~/.tinyedit/color-scheme/" after installing.
- Correct the contracts of `historyInsertRow` and `historyDeleteRow`: 1 means
  "handled", even when a recorded failure left the buffer unchanged.
  Clarify separately that `historyPrepareRow` returns permission to mutate.
- Clamp oversized numbers typed in Settings instead of wrapping them
  (`4294967297` for Tab width became 1).
- Open Help → Documentation even when `$HOME` contains `#` or a percent escape.
- Correct the LaTeX screenshot paths in the syntax guide, check HTML image
  references in the documentation test, list F10 in the F1 help screen, and
  document the `filetype.*` comment rule.
- Add the missing English Doxygen contracts for `links.c`, `links.h` and the link
  and word-selection helpers in `tinyedit.c`; correct stale function names in the
  `backup.h` overview.

- Recognize Alt+Enter in Ghostty with Kitty keyboard mode enabled (including
  experimental macOS Command keys), so links open with either verified key
  sequence and subsequent typing is preserved.

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
