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
La colonna snapshot misura il vecchio costo completo come riferimento;
l'applicazione usa invece differenze per riga.
Alcune conversioni e la prima scansione delle parentesi restano lineari.
`test_backup_paths.c` verifica gli errori recuperabili della risoluzione POSIX.


`test_history.c` injects allocation failures into the actual journal and buffer
code. It checks compound rollback, preservation of redo after a failed edit,
allocation-free source undo/redo, malformed UTF-8 and embedded NUL, coalescing,
depth and memory limits, deferred budget changes and grouped replace sessions.
A 20 MiB / 20,000-row fixture measures requested bytes and allocation count for
one changed row; these values exclude allocator overhead, caches and RSS.
`test_core.c` also injects rendering OOM before edits and both replay directions,
checks retry behavior and oversized selection replacement rollback.

Benchmark locale 2026-10-06 (una esecuzione, tempo CPU): su 50.000 righe
lo snapshot completo richiede 4.700.002 byte e 0,355 ms circa; 200 edit
differenziali distinti della stessa riga trattengono 77.500 byte in totale,
con circa 0,00009 ms per edit (esclusi rendering e I/O). I risultati servono
al confronto dei costi del journal, non alla previsione della latenza UI.

`test_memory_contracts.c` verifica somme/prodotti e crescita ai limiti di
SIZE_MAX, rifiuto prima dell'allocazione mediante processi figli, clipboard
interna vuota, NUL finale e copie indipendenti con byte malformati/NUL interni.
Non accede alla clipboard di sistema e non provoca esaurimento reale di RAM.

## Colors, Settings, build identity and New

The settings tests cover old ANSI/legacy configurations, independent RGB
palette round trips, strict hex validation, explicit foreground/background
escapes and the manual ANSI fallback. Core tests exercise both color groups
on one Settings page, navigation that skips headings, cancelling exit, discarding a draft, cancelling/correcting RGB
input and failed-save isolation. PTY tests verify terminal-default Settings
labels, RGB/swapped bars, samples, mouse wheel/click navigation and complete
build identifiers in splash and F3.

`test_build.py` builds temporary source copies and covers clean/dirty Git,
archives (including inside unrelated repositories), override validation and
commit changes followed by make without clean. It also checks --version with
no terminal input. The source checksum is not a binary or cryptographic ID.

New tests exercise cancellation, failed and successful saves, backup removal,
selection/view/history preservation on cancellation, reset of all document
state and the experimental macOS Cmd-N decoder mapping. These simulated bytes
are not evidence that a particular real terminal emits them.

Real-terminal checks remain required on Ghostty/macOS and Konsole/ArchLinux:
verify actual Ctrl-N bytes and, for Ghostty's opt-in Command-key mode, Cmd-N
passthrough and restoration of the terminal binding on exit. Check RGB output
and manual fallback, Settings readability with equal foreground/background
colors, mouse/back navigation, scroll and save/discard/cancel. This session's
automatic PTY checks do not certify those terminal-specific behaviors.
