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
| `Shift+Frecce` | Estende/crea la selezione di testo (non funziona su Terminal.app di macOS — usa `Ctrl-T`) |
| `Ctrl-T` | Attiva/disattiva la modalità selezione: con la modalità attiva le frecce semplici estendono la selezione come farebbe Shift+Frecce — funziona su ogni terminale |
| Invio | Nuova riga |
| Backspace / Canc | Cancella carattere (gestisce correttamente UTF-8 multi-byte) |
| `Ctrl-A` | Seleziona tutto |
| `Ctrl-C` / `Ctrl-X` / `Ctrl-V` | Copia / taglia / incolla (clipboard di sistema, richiede una selezione per C/X) |
| `Ctrl-Z` | Undo |
| `Ctrl-Y` | Redo |
| `Ctrl-F` | Ricerca incrementale (Frecce per prossimo/precedente match, `Ctrl-R` per passare a cerca-e-sostituisci, Esc per annullare) |
| `F2` | Pannello impostazioni (Frecce per navigare, Invio/Spazio per editare, `Ctrl-S` salva, Esc annulla) |
| `Ctrl-S` | Salva (chiede il nome file se non impostato) |
| `Ctrl-Q` | Esci (chiede conferma se ci sono modifiche non salvate) |

Numeri di riga (gutter), tab width, tasto di redo e colori
dell'interfaccia sono configurabili dal pannello `F2` e salvati in
`~/.tinyeditrc` — vedi il file stesso (generato al primo salvataggio)
per il formato.

Supporto UTF-8 completo (portato da
[linenoise](https://github.com/antirez/linenoise)): decodifica dei
code point, confini di grapheme cluster (emoji con modificatori, ZWJ,
combining marks) e larghezza display reale (0/1/2 colonne) per
cursore, backspace e rendering — non solo caratteri accentati europei
ma anche CJK ed emoji.

## Struttura del codice

- `tinyedit.c` / `tinyedit.h` — l'editor: terminale raw mode, buffer di
  righe, rendering, gestione input, pannello impostazioni. Tipi/macro
  condivisi nell'header, logica nel `.c` (vedi `CLAUDE.md` per lo
  standard di ordine include/define/tipi/globali/funzioni seguito in
  ogni modulo del progetto).
- `clipboard.c` / `clipboard.h` — modulo per l'integrazione con la
  clipboard di sistema (macOS `pbcopy`/`pbpaste`, Linux `wl-clipboard` o
  `xclip`), con fallback su un buffer interno quando nessun backend di
  sistema è disponibile. Collegato all'editor tramite `Ctrl-C`/`Ctrl-X`/
  `Ctrl-V`.
- `utf8.c` / `utf8.h` — decodifica UTF-8, confini di grapheme cluster e
  calcolo della larghezza display in colonne terminale, portato da
  linenoise (vedi sopra). Usato per cursore/backspace/rendering.
- `settings.c` / `settings.h` — persistenza delle impostazioni utente
  (`~/.tinyeditrc`), tabella descrittori che pilota sia il parser file
  sia il pannello `F2`.
- `linenoise.c` / `linenoise.h` — sorgenti originali di linenoise
  (antirez), tenuti per riferimento storico dal primo prototipo
  riga-per-riga. Non compilati nel binario attuale (eccetto la logica
  UTF-8, portata separatamente in `utf8.c`).

## Stato del progetto

Vedi [`TODO.md`](TODO.md) per il lavoro pianificato e in corso, e
[`IDEAS.md`](IDEAS.md) per feature future non ancora prioritarie.
