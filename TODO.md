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
      fallback su buffer interno. Collegato a `tinyedit.c` (Ctrl-C/X/V).

- [x] **Selezione testo con Shift+Frecce**
  - Stato "anchor" (`E.sel_active`, `E.sel_anchor_x/y`) in `editorConfig`,
    evidenziazione visiva nel render tramite inversione foreground/background
    (`\x1b[7m`) sui caratteri selezionati. `editorReadKey` riconosce il
    modificatore Shift (`;2`) sulle sequenze CSI, stesso meccanismo già
    usato per Alt (`;3`).
  - **Limite noto**: Terminal.app su macOS manda lo stesso identico byte
    per freccia semplice e Shift+freccia (verificato con un key logger
    dedicato), quindi Shift+Frecce non funziona lì — non è un bug
    dell'editor, il terminale perde l'informazione prima di mandarla.
  - **Fix universale — `Ctrl-T`**: attiva/disattiva `E.sel_pinned`, che fa
    sì che le frecce semplici (senza Shift) estendano la selezione
    esattamente come Shift+Frecce, riusando lo stesso stato/logica.
    Funziona su ogni terminale perché non dipende dal rilevamento di
    Shift. `main()` rileva `$TERM_PROGRAM == "Apple_Terminal"` e mostra
    un messaggio di stato che indirizza a Ctrl-T invece di lasciare
    l'utente a chiedersi perché Shift+Frecce non risponde.
  - `Esc` disattiva anche `sel_pinned`, non solo la selezione visibile
    del momento — altrimenti la freccia successiva avrebbe silenziosamente
    avviato una nuova selezione con la modalità ancora "invisibilmente" attiva.

- [x] **Selezione con PageUp/PageDown**
  - Stesso pattern di Shift+Frecce: `Shift+PageUp`/`Shift+PageDown`
    riconosciuti da `editorReadKey` (`ESC[5;2~`/`ESC[6;2~`, formato CSI
    standard xterm — verificato su Ghostty). **Anche qui Terminal.app non
    manda alcun modificatore** (`ESC[5~`/`ESC[6~`, identico al tasto
    semplice, verificato con key logger), quindi la via universale resta
    `Ctrl-T` + PageUp/PageDown semplice, che riusa lo stesso `sel_pinned`
    già implementato per le frecce.

- [x] **Ctrl-A: seleziona tutto**

- [x] **Ctrl-C / Ctrl-X / Ctrl-V** (copia / taglia / incolla)
  - Collegano il modulo `clipboard.c` alla selezione testo tramite
    `editorGetSelection`/`editorSerializeRange`/`editorDeleteRange`.
  - Ctrl-C copia il testo selezionato, Ctrl-X copia e cancella, Ctrl-V
    sostituisce l'eventuale selezione e inserisce il contenuto della
    clipboard (gestendo inserimento multi-riga via `editorInsertText`).
    Deciso: senza selezione attiva, Ctrl-C/X non fanno nulla (niente
    fallback "riga intera" stile VS Code).

- [x] **Undo / Redo** (`Ctrl-Z` per undo, `Ctrl-Y` per redo — scelto lo
      standard Windows/Linux; `Ctrl-Shift-Z` non implementato)
  - Stack di snapshot dell'intero buffer prima di ogni modifica atomica,
    con coalescenza delle modifiche dello stesso tipo entro 1 secondo
    (`editorPushUndo`) così digitare una parola conta come un solo passo
    di undo. Profondità massima `UNDO_MAX_DEPTH` (200).

- [x] **Ricerca incrementale (`Ctrl-F`)**
  - Prompt di ricerca (`editorPromptCB`, estensione di `editorPrompt` con
    callback per side-effect ad ogni keystroke), evidenziazione live del
    match. Frecce per prossimo/precedente match, wrap-around su tutto il
    file.

- [x] **Cerca e sostituisci (`Ctrl-R` dentro il prompt di ricerca)**
  - Deciso: bind su `Ctrl-R` all'interno del prompt Ctrl-F invece di
    `Ctrl-Shift-F`, la cui sequenza è indistinguibile da Ctrl-F sulla
    maggior parte dei terminali raw. Prompt per il testo di sostituzione,
    poi conferma per-occorrenza y/n/a(ll)/q(uit).

- [x] **Numeri di riga (gutter)**
  - Colonna a sinistra con il numero riga, larghezza dinamica
    (`editorGutterWidth`) che cresce con `E.numrows`; conteggiata nei
    `E.screencols` disponibili per il testo (`editorTextCols`). Attivo di
    default, ora configurabile tramite il file di impostazioni sotto.

- [x] **File di configurazione** (`~/.tinyeditrc`, formato INI-style
      `chiave = valore`)
  - Modulo `settings.c`/`settings.h`: `struct editorSettings` +
    `settingDescriptors[]` (tabella statica che pilota sia il parser
    file sia lo schermo F2, così aggiungere un'opzione è una riga nella
    tabella). Opzioni: `show_line_numbers`, `tab_stop`, `redo_key`,
    `undo_max_depth`, `color_gutter`, `color_selection`,
    `color_statusbar`. `settingsLoad()` tollerante (righe
    sconosciute/malformate ignorate silenziosamente, chiavi mancanti
    restano ai default).
  - Deciso: `redo_key = ctrl-shift-z` è solo informativo — `Ctrl-Y`
    resta sempre attivo come redo perché Ctrl-Shift-Z è spesso
    indistinguibile da Ctrl-Z su tty raw; lo schermo F2 mostra una nota
    esplicita quando quell'opzione è selezionata invece di far finta
    che sia garantita.

- [x] **TUI a widget per le impostazioni**
  - `editorSettingsScreen()` in `tinyedit.c`, aperta con **F2** (byte
    `ESC O Q`, verificato sul terminale dell'utente prima di
    implementare). Frecce su/giù per navigare, Invio/Spazio per
    editare (toggle diretto per bool, ciclo per enum, mini-prompt
    numerico con range clampato per int via `editorPrompt`). Ctrl-S
    salva su `~/.tinyeditrc` e rende attive le modifiche; Esc annulla
    scartando la copia locale editata, lo stato live (`S`) resta
    invariato.

- [x] **Resize della finestra terminale (SIGWINCH)**
  - Bug: `getWindowSize()` veniva chiamato una sola volta all'avvio
    (`initEditor`), quindi ridimensionare la finestra mentre l'editor
    era aperto lasciava `E.screenrows`/`E.screencols` bloccati al
    valore iniziale, causando disallineamento/sovrapposizione del
    rendering.
  - Fix: signal handler per `SIGWINCH` (`handleWinch`, imposta solo un
    flag `sig_atomic_t`, come richiesto per la sicurezza in un signal
    handler — il vero ridisegno resta nel main loop). `editorReadKey`
    non muore più su `EINTR` e ritorna un tasto no-op se causato da
    resize, così il loop principale richiama subito
    `editorRefreshScreen()` invece di restare bloccato in attesa del
    prossimo tasto reale. `editorRefreshScreen()` rilegge le dimensioni
    e forza un `\x1b[2J` (clear intero schermo) quando rileva un
    resize, necessario perché restringere la finestra lascia righe
    vecchie visibili oltre la nuova area che il clear per-riga non
    coprirebbe. Verificato via pty (resize reale del pty + `SIGWINCH`
    al processo) sia in crescita che in restringimento.

- [x] **Conteggio caratteri e tipo file nella status bar**
  - `editorCountChars()` in `tinyedit.c`: conta grapheme cluster (non
    byte, non code point) su tutto il buffer via `utf8NextCharLen`,
    coerente con come cursore/backspace già trattano un'emoji con
    modificatore come 1 unità. Newline tra righe contano 1 ciascuno.
  - **Tipo file** derivato dall'estensione, mostrato a destra vicino a
    riga/colonna (es. `C | 1/1659`). Tabella built-in ~30 voci in
    `settings.c` (`builtinFiletypes[]`), estendibile/sovrascrivibile da
    utente con righe `filetype.<ext> = <Nome>` dentro `~/.tinyeditrc`
    (stesso file, stesso formato chiave=valore — deciso esplicitamente
    di non introdurre YAML per restare coerenti col vincolo "zero
    dipendenze": niente parser YAML nella libc, e un parser scritto ad
    hoc non sarebbe comunque YAML valido). Gli override sono un modulo
    separato (`filetypeOverrides[]`) da quello dei `settingDescriptor`
    a slot fisso, perché è una lista aperta di lunghezza variabile.
    `settingsSave()` (F2, Ctrl-S) preserva questa sezione anche se il
    pannello F2 non la edita ancora direttamente.

- [x] **Fix: messaggio di aiuto iniziale scompariva dopo 5s** e **`F1` help screen**
  - Bug: `editorDrawMessageBar()` nascondeva qualunque messaggio di
    stato dopo 5 secondi dal timeout `E.statusmsg_time`, pensato per
    conferme transitorie ("Settings saved", "Undo") ma applicato anche
    al messaggio di aiuto persistente all'avvio — spariva al primo
    redraw dopo il timeout, dando l'impressione di sparire "al primo
    tasto premuto".
  - Fix: nuovo flag `E.statusmsg_sticky` e `editorSetStatusMessageSticky()`
    accanto a `editorSetStatusMessage()` esistente — i messaggi sticky
    restano finché non sostituiti da un altro messaggio (sticky o no),
    ignorando il timeout. Usato per il messaggio di startup.
  - **`F1`**: nuova schermata `editorHelpScreen()`, contenuto statico
    (`helpEntries[]`, organizzato per categoria: movimento, editing,
    selezione/clipboard, ricerca, file/editor) con tutte le scorciatoie
    implementate finora. Scroll con frecce/PageUp/Down se il contenuto
    supera l'altezza schermo, qualunque tasto chiude (nessuno stato da
    salvare, a differenza di F2).
  - **Deciso**: `Ctrl-H` scartato per l'help perché è già mappato a
    Backspace — non per limite di un terminale specifico (come i casi
    Alt/Shift/F2 già documentati sopra), ma perché `Ctrl-H = 0x08` è la
    definizione aritmetica universale del carattere di controllo
    (`CTRL_KEY('h')`), identica su ogni piattaforma/terminale per
    costruzione. `F1` (byte `ESC O P`, SS3, verificato identico su
    Ghostty e Terminal.app) non ha questo conflitto.

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
