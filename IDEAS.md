# IDEAS

Feature future, non pianificate né prioritarie. Idee da valutare quando
il core (vedi `TODO.md`) sarà completo e stabile — o da scartare se al
momento buono non convincono più.

## Editing avanzato

- **Syntax highlighting: linguaggi oltre agli 8 già coperti** (vedi
  `TODO.md`, completato: C, C++, Python, Shell, JS/TS, Markdown,
  HTML/XML, CSS). Un linguaggio "C-like" (keyword + stringhe +
  commenti, es. Go, Rust, Java, PHP, Lua) può essere aggiunto in due
  modi ormai entrambi disponibili (vedi `TODO.md`): built-in
  (`struct syntaxLang` in `syntax.c`, aggiunto a `syntaxLangTable[]`) o
  utente, senza ricompilare, da un file
  `~/.tinyedit/syntax/<nome>.conf` (stesso formato `chiave = valore` di
  `~/.tinyeditrc`, vedi `README.md`). Un linguaggio con grammatica
  strutturalmente diversa (come Markdown/HTML/CSS oggi) richiede invece
  un tokenizer dedicato in `syntax.c`, agganciato nel dispatcher
  `syntaxHighlightRow()` — non estendibile da file esterno.
  - **Python — stringhe triple-quote** (`"""`/`'''`): oggi renderizzano
    come tre stringhe a carattere singolo consecutive invece di un
    blocco unico multi-riga (compromesso accettato in `TODO.md`) — un
    tokenizer Python dedicato (invece di riuso della tabella generica)
    risolverebbe correttamente, non giustificato finora dal beneficio.
  - **HTML — nome del tag colorato separatamente** dai suoi delimitatori
    `<`/`>`/`/` (oggi solo questi ultimi hanno colore dedicato).
  - **Markdown — evidenziazione annidata del linguaggio dentro un code
    fence** (es. ` ```python ` colora il contenuto come Python) —
    deliberatamente fuori scope per ora, stessa politica "niente
    sintassi annidata" della versione C-only originale.
- **Multi-file / buffer switching** — aprire più file nella stessa
  sessione, passare tra buffer con una combinazione tipo `Ctrl-Tab`.
  Cambia parecchio l'architettura attuale (che assume un solo buffer
  globale in `editorConfig`) — da valutare se vale la complessità o se
  è meglio lasciare "un processo, un file" e affidarsi a `tmux`/finestre
  multiple del terminale.
- **Copia/incolla di riga intera con scorciatoia dedicata** (stile
  vim `dd`/`yy`/`p`) come alternativa più rapida alla selezione manuale.
- **Auto-chiusura tag HTML** (`<div>` → `</div>`) — a differenza
  dell'auto-chiusura a coppia-singola già in `TODO.md` (che include
  anche `$$...$$` LaTeX come caso speciale gestito con successo,
  vedi `TODO.md`), richiede riconoscere un pattern più lungo di un
  carattere (il nome del tag), guardando indietro nel buffer prima di
  decidere, e sapere il linguaggio della riga corrente. Il triplo
  backtick Markdown (`` ``` ``, code fence) è stato deliberatamente
  escluso da questo tipo di trattamento speciale (vedi `TODO.md`):
  VS Code l'aveva provato e gli utenti l'hanno trovato più fastidioso
  che utile, comportamento poi corretto da Microsoft. Stessa famiglia
  di complessità del syntax highlighting sotto — probabile che
  convenga farli insieme, condividendo la stessa infrastruttura di
  "conoscenza del linguaggio".
- **Rilevamento e preservazione line-ending** (CRLF vs LF) — utile se il
  progetto verrà mai usato per editare file misti Windows/Unix.

## Robustezza

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
