# IDEAS

Feature future, non pianificate né prioritarie. Idee da valutare quando
il core (vedi `TODO.md`) sarà completo e stabile — o da scartare se al
momento buono non convincono più.

## Editing avanzato

- **Syntax highlighting** basico per pochi linguaggi comuni (C, Python,
  Markdown, shell) — tokenizer semplice per keyword/stringhe/commenti,
  come fa kilo per il C. Richiede una tabella di colori e mapping
  linguaggio-per-estensione file.
- **Multi-file / buffer switching** — aprire più file nella stessa
  sessione, passare tra buffer con una combinazione tipo `Ctrl-Tab`.
  Cambia parecchio l'architettura attuale (che assume un solo buffer
  globale in `editorConfig`) — da valutare se vale la complessità o se
  è meglio lasciare "un processo, un file" e affidarsi a `tmux`/finestre
  multiple del terminale.
- **Copia/incolla di riga intera con scorciatoia dedicata** (stile
  vim `dd`/`yy`/`p`) come alternativa più rapida alla selezione manuale.
- **Auto-indent intelligente** — Invio che mantiene l'indentazione della
  riga precedente; eventualmente auto-chiusura di parentesi/graffe.
- **Rilevamento e preservazione line-ending** (CRLF vs LF) — utile se il
  progetto verrà mai usato per editare file misti Windows/Unix.

## Robustezza

- **Backup automatico / recovery da crash** (`.nomefile.swp` o simile,
  stile vim) — scrittura periodica di un file di recovery mentre si
  edita, cancellato alla chiusura pulita.
- **Grandi file**: il buffer attuale è un array statico (`MAX_LINES`
  storico del prototipo riga-per-riga, non più applicabile alla versione
  a schermo intero — verificare che non ci siano limiti residui simili
  non necessari) — da profilare su file di migliaia di righe prima di
  aggiungere altre feature che assumono O(n) su tutto il buffer ad ogni
  keypress (es. syntax highlighting ricalcolato ogni redraw).

## Interfaccia

- **Mouse support opzionale** — vedi nota in `TODO.md`: possibile solo
  come opzione esplicita via config, mai default, perché disattiva la
  selezione nativa del terminale.
- **Temi colore** per la TUI delle impostazioni e per il syntax
  highlighting, selezionabili da config.
- **Status bar personalizzabile** (mostrare/nascondere backend clipboard
  attivo, encoding, posizione cursore in formato riga:colonna invece di
  solo riga).

## Portabilità

- **Windows/WSL**: al momento il progetto assume POSIX (termios, raw
  mode via `tcsetattr`). Un porting Windows nativo richiederebbe la
  Console API di Windows (non termios) — fuori scope a meno di richiesta
  esplicita futura. Sotto WSL funziona già come Linux normale.
