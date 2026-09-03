# tinyedit

Un piccolo editor di testo a schermo intero per terminale, scritto in C puro
(stile [kilo](https://github.com/antirez/kilo) di Salvatore Sanfilippo),
senza dipendenze esterne oltre alla libreria standard POSIX.

## Perché esiste

Nato come esempio didattico per esplorare come funzionano i terminali TUI
"da zero" (raw mode, escape ANSI, redraw manuale), invece di usare un
framework come Bubble Tea, Ink o Textual. Il primo prototipo usava
[linenoise](https://github.com/antirez/linenoise) (antirez) per un editor
riga-per-riga a comandi; l'attuale versione è invece un editor a schermo
intero con cursore libero, che richiede raw mode gestito a mano — linenoise
resta nel repo come riferimento storico ma non è più una dipendenza di
build.

## Build

```sh
make        # produce il binario ./tinyedit
make clean  # rimuove il binario
```

Richiede solo un compilatore C99 e un sistema POSIX (macOS o Linux).

## Uso

```sh
./tinyedit [file]
```

| Tasto | Azione |
|---|---|
| Frecce, Home, End, PageUp/Down | Movimento cursore |
| `Alt+←` / `Alt+→` (anche `Esc b` / `Esc f`) | Salto di parola |
| Invio | Nuova riga |
| Backspace / Canc | Cancella carattere (gestisce correttamente UTF-8 multi-byte) |
| `Ctrl-S` | Salva (chiede il nome file se non impostato) |
| `Ctrl-Q` | Esci (chiede conferma se ci sono modifiche non salvate) |

Supporto UTF-8 di base: caratteri multi-byte (es. `è`, `à`) vengono
visualizzati e cancellati correttamente come singola unità.

## Struttura del codice

- `tinyedit.c` — l'editor: terminale raw mode, buffer di righe, rendering,
  gestione input. Un solo file, stile kilo.
- `clipboard.c` / `clipboard.h` — modulo indipendente per l'integrazione
  con la clipboard di sistema (macOS `pbcopy`/`pbpaste`, Linux
  `wl-clipboard` o `xclip`), con fallback su un buffer interno quando
  nessun backend di sistema è disponibile. Non ancora collegato
  all'editor.
- `linenoise.c` / `linenoise.h` — sorgenti originali di linenoise
  (antirez), tenuti per riferimento storico dal primo prototipo
  riga-per-riga. Non compilati nel binario attuale.

## Stato del progetto

Vedi [`TODO.md`](TODO.md) per il lavoro pianificato e in corso, e
[`IDEAS.md`](IDEAS.md) per feature future non ancora prioritarie.
