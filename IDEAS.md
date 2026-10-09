# IDEAS

Feature future, non pianificate né prioritarie. Idee da valutare quando
il core sarà completo e stabile — o da scartare se al
momento buono non convincono più.

## Stato delle idee implementate (2026-10-08)

Il completamento dei percorsi Open/Save e la sidebar filesystem sono ora
implementati nel fork locale. Comandi e comportamento attuale sono descritti
nel README; decisioni e verifiche sono registrate in `local/TODO.md`.
La sidebar può rimanere aperta mentre si modifica il documento: Ctrl-E la
mostra/nasconde e Ctrl-B cambia focus. Non abilita ancora buffer multipli.

## Editing avanzato

- **Syntax highlighting: linguaggi oltre agli 8 già coperti** (vedi
  `README.md`: C, C++, Python, Shell, JS/TS, Markdown,
  HTML/XML, CSS). Un linguaggio "C-like" (keyword + stringhe +
  commenti, es. Go, Rust, Java, PHP, Lua) può essere aggiunto in due
  modi ormai entrambi disponibili (vedi `README.md`): built-in
  (`struct syntaxLang` in `syntax.c`, aggiunto a `syntaxLangTable[]`) o
  utente, senza ricompilare, da un file
  `~/.tinyedit/syntax/<nome>.conf` (stesso formato `chiave = valore` di
  `~/.tinyeditrc`, vedi `README.md`). Un linguaggio con grammatica
  strutturalmente diversa (come Markdown/HTML/CSS oggi) richiede invece
  un tokenizer dedicato in `syntax.c`, agganciato nel dispatcher
  `syntaxHighlightRow()` — non estendibile da file esterno.
  - **Python — stringhe triple-quote** (`"""`/`'''`): oggi renderizzano
    come tre stringhe a carattere singolo consecutive invece di un
    blocco unico multi-riga (limite attuale del tokenizer) — un
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
- **Preservazione dei terminatori di ogni singola riga nei file misti** —
  LF/CRLF, rilevamento dei file misti e newline finale sono già gestiti.
  Il salvataggio usa però un solo stile per documento; conservare esattamente
  una mescolanza originale richiederebbe metadati per riga.

## Robustezza

- **Decoder UTF-8 rigoroso, bounded e con recupero non distruttivo
  (implementato)** — l'idea è stata suggerita da Juuso
  Alasuutari (`imaami`) durante una discussione sul parser di tinyedit; la
  [mappa degli stati](https://i.imgur.com/nVfDRT8.png) del suo progetto
  [`c.utf-8`](https://github.com/imaami/c.utf-8) è stata usata solo per
  comprendere i casi limite.
  Nella mappa il nodo ASCII parte per errore da `0x01`: anche `0x00` è UTF-8
  valido. Decoder e test sono stati scritti indipendentemente dal codice di
  `c.utf-8`, sulla base della Table 3-7 dello Unicode Standard e della
  sezione 4 di RFC 3629.

- **Grandi file**: il buffer è un array dinamico di righe, senza il vecchio
  `MAX_LINES`. Il tokenizer viene aggiornato sulle modifiche, non a ogni
  redraw; wrap e conteggi hanno cache e il rendering percorre la viewport
  in sequenza. Misure riproducibili: `make benchmark` e
  `tests/README.md`. Restano lineari alcune conversioni
  riga/video e il primo matching di una parentesi distante. Undo/Redo
  conserva le righe modificate con budget condiviso e rollback in caso di OOM
  nei percorsi della cronologia. Restano da misurare le copie intermedie
  delle righe lunghe durante l’accorpamento e i picchi delle cache temporanee.
  Il vettore delle righe merita misure specifiche su caricamenti
  e modifiche strutturali; non confondere questo costo con i redraw ripetuti.

- **Gestione sistematica dei fallimenti fatali** — oggi `terminalDie`,
  `teOutOfMemory`, gli errori di `write` e i messaggi di stato seguono percorsi
  separati, e ciascuno decide da sé quando ripristinare il terminale e quando
  stampare (un messaggio stampato nell'alternate screen va perso: corretto per
  l'apertura da riga di comando, non ancora per l'esaurimento di memoria).
  Un unico punto di uscita fatale — ripristino del terminale, messaggio, codice
  di uscita — richiamabile anche da `alloc.c` senza dipendere da `terminal.c`
  (per esempio tramite un hook registrabile) renderebbe il comportamento
  uniforme. Da valutare insieme alla gestione di SIGTERM/SIGHUP.

- **Decoder dei tasti basato su un parser CSI generale** — oggi `terminal.c`
  legge un numero fisso di byte per ogni forma nota (Kitty, modifyOtherKeys,
  tasti funzione) e scarta il resto con un "drain": una forma non prevista può
  consumare il byte sbagliato. Un parser a stati che accumula parametri
  (`;`/`:`), byte intermedi e byte finale, e decide solo a sequenza completa,
  scarterebbe le sequenze sconosciute per intero. Idee collegate:
  - negoziare il protocollo Kitty (`CSI ? u`) invece di decodificare ogni forma
    in ogni caso, ricordando la modalità attiva e mantenendo la tabella legacy
    come ripiego;
  - descrivere i tasti come tabella dati (`codepoint + modificatori → azione`,
    con le equivalenze legacy accanto), così una nuova scorciatoia è una riga e
    un test;
  - fixture con i byte realmente catturati nei terminali (Ghostty, iTerm2,
    Terminal.app, Konsole, kitty, tmux; si può usare il logger di tasti) che il
    decoder deve tradurre nell'azione attesa, al posto di soli test che
    simulano le sequenze;
  - una pagina di compatibilità generata dalle fixture: per ogni scorciatoia,
    quali terminali la inviano distinta e quali no;
  - un ripiego universale per ogni azione che dipende da una combinazione non
    disponibile ovunque (come `Ctrl-T` per la selezione). L'obiettivo realistico
    è la stessa esperienza dei programmi desktop dove il terminale lo permette,
    con l'alternativa dichiarata dove no.

## Interfaccia

- **Mouse support opzionale (implementato)** — attivabile da F2/config,
  disabilitato per default perché sostituisce la selezione nativa del
  terminale. Supporta anche focus, espansione e cambio radice della sidebar.
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
