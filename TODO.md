# TODO

Lavoro pianificato, in ordine di dipendenza (non necessariamente di
priorità — alcune voci sbloccano altre). Le decisioni di design già
prese in conversazione sono segnate esplicitamente; dove manca una
decisione, è segnalato come "DA DECIDERE".

## Fatto

- [x] Editor a schermo intero stile kilo (raw mode, redraw, scrolling)
- [x] Movimento cursore: frecce, Home/End, PageUp/PageDown
- [x] Alt+←/→ per salto parola (`ESC b` / `ESC f`, formato Meta/readline
      — è quello che manda Ghostty; il formato CSI `ESC[1;3D` è comunque
      gestito in parallelo per altri terminali)
- [x] Ctrl-S salva, con prompt "Save as" se il file non ha ancora nome
- [x] Ctrl-Q esce, chiede conferma se ci sono modifiche non salvate
- [x] Gestione corretta di caratteri UTF-8 multi-byte (cursore e
      backspace non si disallineano più su `è`, `à`, ecc.)
- [x] Modulo `clipboard.c`/`clipboard.h`: copia/incolla su clipboard di
      sistema (pbcopy/pbpaste su macOS, wl-clipboard o xclip su Linux),
      fallback su buffer interno. Testato standalone, **non ancora
      collegato a tinyedit.c**.

## Da fare — core editing

- [ ] **Undo / Redo** (`Ctrl-Z` / redo — tasto da decidere: `Ctrl-Y` è lo
      standard Windows/Linux, `Ctrl-Shift-Z` lo standard macOS/Vim-plugin;
      DA DECIDERE quale usare, o supportare entrambi)
  - Approccio più semplice: stack di snapshot dell'intero buffer prima di
    ogni modifica "atomica" (inserimento/cancellazione carattere,
    riga, incolla). Poco efficiente in memoria su file grandi ma semplice
    da ragionare; alternativa più complessa è un diff/patch stack.

- [ ] **Selezione testo con Shift+Frecce**
  - Richiede: stato "anchor" (punto di inizio selezione) in `editorConfig`,
    evidenziazione visiva nel render (probabilmente invertendo
    foreground/background sui caratteri selezionati), estensione di
    `editorMoveCursor`/`editorMoveCursorWord` per aggiornare la selezione
    quando Shift è premuto.
  - Nota tecnica: serve rilevare Shift+Freccia separatamente dalla
    freccia semplice — verificare quale sequenza manda il terminale
    dell'utente (probabilmente CSI con modificatore `;2`, stesso
    meccanismo già gestito per Alt che usa `;3` — va esteso il parser di
    `editorReadKey` per riconoscere anche il modificatore Shift).

- [ ] **Ctrl-A: seleziona tutto**
  - Banale una volta che esiste lo stato di selezione sopra.

- [ ] **Ctrl-C / Ctrl-X / Ctrl-V** (copia / taglia / incolla)
  - Collega il modulo `clipboard.c` già pronto alla selezione testo.
  - Ctrl-C: copia il testo selezionato (o l'intera riga corrente se non
    c'è selezione — comportamento stile VS Code; DA DECIDERE se questo
    fallback è desiderato o se senza selezione Ctrl-C non fa nulla).
  - Ctrl-X: come sopra ma cancella anche il testo dal buffer.
  - Ctrl-V: inserisce il contenuto della clipboard alla posizione del
    cursore, gestendo correttamente inserimento multi-riga.

- [ ] **Ricerca incrementale (`Ctrl-F`)**
  - Prompt di ricerca (riusa `editorPrompt` già esistente, esteso per
    aggiornare l'evidenziazione a ogni carattere digitato).
  - Frecce Su/Giù o Ctrl-F/Ctrl-Shift-F ripetuto per prossimo/precedente
    match.

- [ ] **Cerca e sostituisci (`Ctrl-Shift-F`)**
  - Come sopra più un secondo prompt per il testo di sostituzione, e
    conferma per-occorrenza o "sostituisci tutto".

## Da fare — interfaccia

- [ ] **Numeri di riga (gutter)**
  - Colonna a sinistra con il numero riga; deve essere conteggiata in
    `E.screencols` disponibili per il testo (attualmente tutto lo
    schermo è testo). Attivabile/disattivabile da config.

- [ ] **File di configurazione** (es. `~/.tinyeditrc` o
      `~/.config/tinyedit/config`)
  - Opzioni minime: numeri di riga on/off, tab width, tasto di redo.
  - DA DECIDERE: formato del file (INI-style semplice coerente con lo
    stile "zero dipendenze" del progetto, piuttosto che introdurre un
    parser JSON/YAML).

- [ ] **TUI a widget per le impostazioni**
  - Schermata dedicata (tasto da assegnare, es. `Ctrl-,`) con lista
    opzioni navigabile a frecce e editabile inline, che legge/scrive il
    file di configurazione sopra.
  - Dipende dal file di config essendo già definito nella sua forma
    finale (struttura dati delle opzioni condivisa tra parser file e
    editor TUI).

## Note tecniche aperte

- Il mouse (click per posizionare il cursore) **non è nel piano attuale**:
  abilitare il mouse-reporting del terminale disattiverebbe la selezione
  nativa del terminale che l'utente già usa con Cmd+C/Cmd+V su Ghostty.
  Se in futuro serve, va reintrodotto come opzione disattivabile via
  config, non come default.
- Ctrl-C/X/V nell'editor non confliggono con le scorciatoie di sistema:
  su macOS il sistema usa Cmd+C/V (Ctrl-C è libero in raw mode), su
  Linux i terminali grafici tipicamente usano Ctrl-Shift-C/V per non
  rompere la convenzione storica di Ctrl-C come SIGINT.
