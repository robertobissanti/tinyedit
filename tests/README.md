# Test fixtures

Run all automated checks from the repository root with:

```sh
make test
```

The files in `fixtures/` are also intended for manual inspection in
tinyedit. For example:

```sh
bin/tinyedit tests/fixtures/demo.css
```

Check comments, strings, keywords, Unicode identifiers and multi-line
constructs. `F1`, `F2`, and `F3` should open Help, Settings, and Info.
F3 accepts both the SS3 sequence commonly emitted by terminal emulators
and the CSI `ESC[13~` form.

`test_core.c` exercises the actual private core by including its translation
unit with a renamed application entry point. It checks prompt growth, bar
clipping, empty page navigation and UTF-8 tab layout and selection. The render
tests cover graphemes wider than the wrap width. PTY regressions check long
paths entered by typing, clipboard and bracketed paste, and empty page keys.

`test_fileio.c` compiles the actual I/O implementation with controlled syscall
failures: partial reads, close errors, short/interrupted/failed writes, file
sync, rename and directory sync. It verifies exact content, cleanup, symlinks
and modes. Core tests check transaction state and backup ownership; PTY tests
check that failed Open/Save as retain text, undo and the next save destination.

Mouse dispatch regressions feed real queued bytes through the terminal decoder
and check exact-once text/navigation/paste handling, routing of later mouse
reports to menus, the redraw burst limit and gesture lifetime across reset and
recovery. PTY tests verify saved bytes, commands following mouse bursts and
unfinished gestures across close/open.

ASCII auto-close regressions verify the shared policy and the real core
dispatch for every pair, matching closer skipping, single/multiline selection
wrapping, both settings and disabled-pair selection replacement. PTY cases
exercise the same typing paths with auto-close enabled and disabled.

`test_search.c` verifica il motore di ricerca senza terminale: coordinate byte,
regex inversa condivisa, wrap, UTF-8, match vuoti, cache della compilazione
(con conteggio delle chiamate effettive a `regcomp`) e invalidazione del testo.
I test nel core verificano applicazione alla sessione, annullamento della vista
e invalidazione dopo edit, undo/redo e cambio documento.

`make benchmark` compila `benchmark_core.c` e misura il core senza I/O terminale.
Usa `clock()` (tempo CPU), fixture Unicode di 1.000, 10.000 e 50.000 righe,
viewport 80×40, soft wrap 72 e cache di wrap preparate. Draw, conteggio e pair
sono medie di 20 chiamate; con i cache di conteggio/pair includono una chiamata
iniziale e 19 riusi. Gli snapshot sono cinque cicli di copia e rilascio.
Il benchmark esclude I/O terminale, avvio e caricamento, e non misura RSS.
Undo conserva snapshot completi; il payload scala con documento e profondità.
Alcune conversioni e la prima scansione delle parentesi restano lineari.
`test_backup_paths.c` verifica gli errori recuperabili della risoluzione POSIX.
