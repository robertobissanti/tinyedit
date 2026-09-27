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
- **Multi-file / buffer switching** — tenere aperti più file nella stessa
  sessione e passare tra i buffer, per esempio con `Ctrl-Tab`. Ogni buffer
  dovrebbe conservare il proprio stato (modifiche non salvate, cursore,
  selezione, scroll e undo). Richiede di superare l'attuale modello con un
  solo buffer globale in `editorConfig`; valutare interfaccia e complessità
  prima di pianificarlo.
- **Copia/incolla di riga intera con scorciatoia dedicata** (stile
  vim `dd`/`yy`/`p`) come alternativa più rapida alla selezione manuale.
- **Reindentazione dell'intero file** — comando esplicito che ricostruisce
  l'indentazione delle righe in base alla struttura del linguaggio, distinto
  dall'auto-indentazione già applicata quando si crea una nuova riga.
- **Rilevamento e preservazione line-ending** (CRLF vs LF) — utile se il
  progetto verrà mai usato per editare file misti Windows/Unix.

## Robustezza

- **Decoder UTF-8 rigoroso, bounded e con recupero non distruttivo
  (implementato; vedi `TODO.md`)** — l'idea è stata suggerita da Juuso
  Alasuutari (`imaami`) durante una discussione sul parser di tinyedit; la
  [mappa degli stati](https://i.imgur.com/nVfDRT8.png) del suo progetto
  [`c.utf-8`](https://github.com/imaami/c.utf-8) è stata usata solo per
  comprendere i casi limite.
  Nella mappa il nodo ASCII parte per errore da `0x01`: anche `0x00` è UTF-8
  valido. Decoder e test sono stati scritti indipendentemente dal codice di
  `c.utf-8`, sulla base della Table 3-7 dello Unicode Standard e della
  sezione 4 di RFC 3629.

- **Grandi file**: il buffer attuale è un array statico (`MAX_LINES`
  storico del prototipo riga-per-riga, non più applicabile alla versione
  a schermo intero — verificare che non ci siano limiti residui simili
  non necessari) — da profilare su file di migliaia di righe prima di
  aggiungere altre feature che assumono O(n) su tutto il buffer ad ogni
  keypress (es. syntax highlighting ricalcolato ogni redraw).

## Interfaccia

- **Barra laterale di navigazione file** — un albero di cartelle e file,
  richiamabile e richiudibile, per aprire documenti senza digitare il
  percorso. Da valutare insieme al multi-buffer: la navigazione resta
  utile anche con un solo documento, ma aprire più file senza perdere il
  precedente ne aumenterebbe l'utilità. La barra deve lasciare spazio
  sufficiente al testo nei terminali stretti.
- **Completamento dei percorsi con Tab nei prompt Open e Save as** —
  completare nomi di cartelle e file a partire dal percorso digitato,
  mostrando o facendo scorrere le alternative quando ce n'è più di una.
  Riutilizzare lo stesso comportamento anche nel prompt del primo
  salvataggio di un buffer senza nome.
- **Mouse support opzionale** — vedi nota in `TODO.md`: possibile solo
  come opzione esplicita via config, mai default, perché disattiva la
  selezione nativa del terminale.
- **Temi colore** per la TUI delle impostazioni e per il syntax
  highlighting, selezionabili da config.
- **Status bar personalizzabile** (mostrare/nascondere backend clipboard
  attivo, encoding, posizione cursore in formato riga:colonna invece di
  solo riga).

## Portabilità

- **Build `make vanilla`** — compilare un secondo eseguibile essenziale
  dallo stesso codice, usando un flag di compilazione (per esempio
  `TE_VANILLA`) che escluda le funzioni non necessarie: inizialmente
  menu e syntax highlighting a colori. Dove possibile, il Makefile
  esclude i relativi moduli; nei file condivisi, sezioni condizionali
  impediscono di compilare chiamate e stato associati. Non basta
  disattivare le funzioni a runtime: il codice deve mancare dal binario
  vanilla. Definire il resto del perimetro prima di implementarlo,
  conservando le basi di editing, apertura e salvataggio, protezione
  dalle modifiche non salvate e gestione corretta di UTF-8. Misurare
  dimensione del binario e complessità aggiunta dai `#ifdef`.
- **Windows/WSL**: al momento il progetto assume POSIX (termios, raw
  mode via `tcsetattr`). Un porting Windows nativo richiederebbe la
  Console API di Windows (non termios) — fuori scope a meno di richiesta
  esplicita futura. Sotto WSL funziona già come Linux normale.
