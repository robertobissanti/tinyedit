# TODO

Lavoro pianificato, in ordine di dipendenza (non necessariamente di
priorità — alcune voci sbloccano altre). Le decisioni di design già
prese in conversazione sono segnate esplicitamente; dove manca una
decisione, è segnalato come "DA DECIDERE".

Ogni voce riporta `_Inserito: YYYY-MM-DD · Completato: YYYY-MM-DD_`
subito sotto il titolo, per poter ricostruire un log delle modifiche
release per release. Le date delle voci precedenti all'introduzione di
questa convenzione (2026-09-04) sono valorizzate a quel giorno per
entrambi i campi — non riflettono quando il lavoro è realmente
avvenuto (i commit del repository condividono tutti la stessa data,
non utile per una ricostruzione storica accurata), sono un punto di
partenza da cui il log sarà accurato in avanti.

## Fatto

- _Inserito: 2026-09-04 · Completato: 2026-09-04_ (nucleo iniziale
  dell'editor, voci seguenti fino alla prima con titolo in grassetto)
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
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
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
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Stesso pattern di Shift+Frecce: `Shift+PageUp`/`Shift+PageDown`
    riconosciuti da `editorReadKey` (`ESC[5;2~`/`ESC[6;2~`, formato CSI
    standard xterm — verificato su Ghostty). **Anche qui Terminal.app non
    manda alcun modificatore** (`ESC[5~`/`ESC[6~`, identico al tasto
    semplice, verificato con key logger), quindi la via universale resta
    `Ctrl-T` + PageUp/PageDown semplice, che riusa lo stesso `sel_pinned`
    già implementato per le frecce.

- [x] **Ctrl-A: seleziona tutto**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_

- [x] **Ctrl-C / Ctrl-X / Ctrl-V** (copia / taglia / incolla)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Collegano il modulo `clipboard.c` alla selezione testo tramite
    `editorGetSelection`/`editorSerializeRange`/`editorDeleteRange`.
  - Ctrl-C copia il testo selezionato, Ctrl-X copia e cancella, Ctrl-V
    sostituisce l'eventuale selezione e inserisce il contenuto della
    clipboard (gestendo inserimento multi-riga via `editorInsertText`).
    Deciso: senza selezione attiva, Ctrl-C/X non fanno nulla (niente
    fallback "riga intera" stile VS Code).

- [x] **Undo / Redo** (`Ctrl-Z` per undo, `Ctrl-Y` per redo — scelto lo
      standard Windows/Linux; `Ctrl-Shift-Z` non implementato)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Stack di snapshot dell'intero buffer prima di ogni modifica atomica,
    con coalescenza delle modifiche dello stesso tipo entro 1 secondo
    (`editorPushUndo`) così digitare una parola conta come un solo passo
    di undo. Profondità massima `UNDO_MAX_DEPTH` (200).

- [x] **Ricerca incrementale (`Ctrl-F`)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Prompt di ricerca (`editorPromptCB`, estensione di `editorPrompt` con
    callback per side-effect ad ogni keystroke), evidenziazione live del
    match. Frecce per prossimo/precedente match, wrap-around su tutto il
    file.

- [x] **Cerca e sostituisci (`Ctrl-R` dentro il prompt di ricerca)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Deciso: bind su `Ctrl-R` all'interno del prompt Ctrl-F invece di
    `Ctrl-Shift-F`, la cui sequenza è indistinguibile da Ctrl-F sulla
    maggior parte dei terminali raw. Prompt per il testo di sostituzione,
    poi conferma per-occorrenza y/n/a(ll)/q(uit).

- [x] **Numeri di riga (gutter)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Colonna a sinistra con il numero riga, larghezza dinamica
    (`editorGutterWidth`) che cresce con `E.numrows`; conteggiata nei
    `E.screencols` disponibili per il testo (`editorTextCols`). Attivo di
    default, ora configurabile tramite il file di impostazioni sotto.

- [x] **File di configurazione** (`~/.tinyeditrc`, formato INI-style
      `chiave = valore`)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
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
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - `editorSettingsScreen()` in `tinyedit.c`, aperta con **F2** (byte
    `ESC O Q`, verificato sul terminale dell'utente prima di
    implementare). Frecce su/giù per navigare, Invio/Spazio per
    editare (toggle diretto per bool, ciclo per enum, mini-prompt
    numerico con range clampato per int). Ctrl-S salva su
    `~/.tinyeditrc` e rende attive le modifiche; Esc annulla scartando
    la copia locale editata, lo stato live (`S`) resta invariato.
  - **Bug fix**: l'editing dei campi int (`tab_stop`, `undo_max_depth`,
    `soft_wrap`) inizialmente usava `editorPrompt()`, che ridisegna
    sempre lo schermo principale dell'editor (`editorDrawRows()`) ad
    ogni tasto — risultato: digitare un nuovo valore faceva "sparire"
    il pannello F2 mostrando il testo del file sotto. Fix:
    `editorSettingsRender()` fattorizza il disegno del pannello,
    riusato sia dal loop principale di F2 sia da un nuovo
    `editorSettingsEditInt()` — un mini prompt numerico che ridisegna
    il pannello (non il buffer) ad ogni tasto, così il pannello resta
    visibile per l'intera durata dell'editing.

- [x] **Resize della finestra terminale (SIGWINCH)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
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
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
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
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
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

- [x] **Fix: Freccia Destra/Sinistra facevano wrap del documento intero**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Bug: a fine documento, Freccia Destra portava `E.cy` a
    `E.numrows` (uno stato interno "una riga oltre l'ultima"), ma
    `editorScroll()`/il posizionamento cursore in
    `editorRefreshScreen()` calcolavano male la riga video per quello
    stato in modalità wrap (default ora, vedi sotto) — trattandolo come
    riga video 0 invece che come "subito dopo l'ultima riga", il
    cursore visivamente saltava altrove nel documento invece di sparire
    sotto l'ultima riga.
  - Fix: Freccia Destra ora si ferma sull'ultimo carattere del
    documento invece di avanzare oltre — simmetrico a Freccia Sinistra,
    che già si fermava all'inizio. `editorScroll()` e il calcolo
    posizione cursore corretti comunque per il caso `E.cy >= E.numrows`
    (restano raggiungibili per altre vie, es. cancellare l'ultima riga
    col cursore lì), usando `editorTotalVideoRows()` invece di 0.

- [x] **Fix: digitare a fine riga wrappata inseriva il carattere sulla riga video successiva**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Bug più serio scoperto verificando il fix precedente: quando una
    riga va a capo dopo uno spazio (es. `"...per"` | `"scrivere..."`),
    `editorRowSegments()` include quello spazio nel segmento
    *corrente* (non disegnato, perché cade a fine riga video) ma
    `End`/il calcolo `cx` per l'inserimento usavano `seg_start[i+1]`
    come "fine segmento" — che punta all'inizio del segmento
    *successivo*, cioè dopo lo spazio. Risultato: premere End e
    digitare un carattere lo inseriva all'inizio della riga video
    successiva (`".scrivere"`) invece che alla fine di quella corrente
    (`"per."`) — visibile anche come cursore che sembrava "un
    carattere indietro" rispetto a dove ci si aspettava scrivesse.
  - Fix: nuova `editorSegVisibleEnd()` — calcola la fine *visibile* di
    un segmento scartando eventuali spazi finali fino al prossimo
    segmento, usata al posto di `seg_start[i+1]` sia nel render
    (`editorDrawRows()`) sia in `HOME_KEY`/`END_KEY`.

- [x] **Fix: bug di fondo — `seg_start[]` mischiava offset BYTE e COLONNA, rompeva il wrap su testo UTF-8**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Bug più serio ancora, riemerso testando col file reale (righe con
    "è" e altri accenti): il fix precedente copriva solo l'ASCII.
    `editorRowSegments()` calcola `seg_start[]` camminando in *byte*
    dentro `row->render` (necessario per indicizzare render), ma il
    commento e diversi chiamanti — soprattutto `editorRxToSegment()`,
    che confronta `seg_start[]` con `E.rx` — lo trattavano come se
    fosse già una *colonna* (rx). Per ASCII puro byte e colonna
    coincidono, quindi "funzionava per caso"; con qualunque carattere
    multi-byte (es. "è", 2 byte ma 1 colonna) prima di un punto di
    wrap, i due valori divergono e da lì in poi tutta la segmentazione
    per quella riga scala male — End dopo "però" seguito da wrap
    piazzava il cursore/il carattere digitato un'intera parola più
    avanti nel testo.
  - Fix: `editorRowSegments()` ora riempie **due** array paralleli —
    `seg_start[]` (byte, per indicizzare/copiare `render`) e
    `seg_start_rx[]` (colonna, per confronti con `E.rx`/altre colonne)
    — calcolati nello stesso scan, senza passate aggiuntive. Nuova
    `editorSegVisibleEndRx()` gemella di `editorSegVisibleEnd()` ma in
    colonne. Tutti i 7 chiamanti (`editorRxToSegment`,
    `editorRowVideoHeight`, il render, `editorMoveCursorWrapped`,
    `HOME_KEY`/`END_KEY`, `PAGE_UP`/`PAGE_DOWN`) aggiornati per usare
    l'array giusto a seconda che debbano indicizzare byte o confrontare
    colonne — **mai l'uno al posto dell'altro**, usando sempre le
    funzioni UTF-8 condivise di `utf8.c`.

- [x] **Soft-wrap con mappatura del cursore**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Il wrap è sempre attivo, nessuno scroll orizzontale: il testo va a
    capo almeno al bordo della finestra. Il parametro `soft_wrap`
    (nuova opzione in `struct editorSettings`, pannello F2) è un limite
    massimo *opzionale* più stretto del bordo finestra (0 = nessun
    limite extra, wrap solo al bordo) — utile per tenere il testo
    leggibile su terminali molto larghi, non per disattivare il wrap
    (vedi `editorSoftWrapCols()`). **Deciso** dopo due iterazioni:
    prima versione disattivava il wrap del tutto sotto una soglia,
    seconda versione lo riattivava al bordo finestra solo se
    `soft_wrap > 0` — la versione finale lo rende sempre attivo
    indipendentemente dal valore configurato.
  - **Deciso**: le frecce Su/Giù (senza modificatori) si muovono per
    riga *video*, non per riga *logica* — coerente col comportamento
    atteso da editor moderni; con wrap attivo "giù" su una riga lunga
    scende di una riga visuale invece di saltare l'intero paragrafo.
    Implica calcolare, in `editorScroll()`/nel render, una mappatura
    `rx` (colonna nella riga logica) → `(offset riga video dentro la
    riga logica, colonna video)` — calcolata al volo, non memorizzata in
    `erow`, per non doverla invalidare ad ogni modifica del buffer.
  - **Deciso**: anche Home/End e PageUp/PageDown operano per riga video,
    non logica — coerente con Su/Giù. Home/End vanno a inizio/fine del
    segmento visuale sotto il cursore; PageUp/PageDown scorrono di
    `screenrows` righe video indipendentemente da quante righe logiche
    attraversano.

- [x] **Barra informativa in alto** (in aggiunta a quella in basso)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - **Deciso contenuto**: nome file, percorso, indicatore modifiche non
    salvate (dirty) — pensata per restare visibile come titolo mentre si
    scrolla un file lungo, a differenza della barra in basso che mostra
    stato/posizione cursore.
  - **Deciso**: senza nome file (buffer nuovo non salvato) mostra
    `[No Name]` + indicatore dirty, stile vim/kilo.
  - **Deciso**: disattivabile da config, nuova opzione `show_top_bar`
    (stesso pattern di `show_line_numbers`), editabile dal pannello F2.

- [x] **Refinement barra alta/bassa: niente duplicazione, colonna reale, Ctrl-W**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - **Deciso**: quando `show_top_bar` è attivo (nome file già lì), la
    barra bassa non ripete il nome file — mostra solo `N lines, M
    chars (modified)`. Quando `show_top_bar` è disattivo, il nome file
    torna a comparire nella barra bassa (comportamento originale), così
    resta sempre visibile da qualche parte.
  - **Deciso**: l'indicatore "(modified)" resta sempre nella barra
    bassa indipendentemente da `show_top_bar`, non solo in quella alta
    — se l'utente disattiva la barra alta lo stato di modifica resta
    comunque visibile.
  - **Deciso**: a destra nella barra bassa, accanto a riga/righe totali,
    ora c'è anche la colonna *reale* (posizione carattere nella riga
    logica, `E.cx + 1`, non `E.rx`/colonna video che tiene conto di
    wrap/tab) — formato `riga/totale: C colonna` (es. `5/10: C 12`).
  - **`Ctrl-W`**: nuovo alias di `Ctrl-Q` (stesso identico
    comportamento oggi). Pensato per diventare in futuro "chiudi
    questo file" se si aggiungerà multi-file/buffer, distinto da "esci
    dal programma" — vedi commento su `editorQuit()` in `tinyedit.c`.
  - **Cambiato il flusso di conferma uscita** per entrambi (era "premi
    ancora N volte per uscire senza salvare"): ora, se ci sono modifiche
    non salvate, chiede esplicitamente `y/n/Esc` — `y` salva ed esce
    (se il salvataggio fallisce, resta nell'editor con il messaggio
    d'errore invece di uscire comunque), `n` esce senza salvare, `Esc`
    (o qualunque altro tasto) annulla l'uscita. Rimossa la vecchia
    macro `TE_QUIT_TIMES`/lo stato `quit_times`, non più necessari.

- [x] **Setting `home_end_visual_line`: Home/End su riga visiva o logica**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Non esiste uno standard unico tra editor: VS Code/Sublime muovono
    Home/End sul segmento *visivo* (riga wrappata sotto il cursore,
    comportamento già presente in tinyedit, coerente con Su/Giù che
    si muovono per riga video); vim muove sempre sulla riga *logica*
    intera per default (`0`/`$`), indipendentemente da quante righe
    video occupa per il wrap. **Deciso**: invece di sceglierne uno,
    nuovo setting `home_end_visual_line` (bool, pannello F2, default
    `true` = comportamento invariato/visivo) per lasciare scegliere.
    A `false`, Home/End ignorano la segmentazione del wrap e vanno
    sempre a `cx=0`/`cx=row->size` della riga logica, anche se il
    cursore è su un segmento visivo diverso dal primo/ultimo.

- [x] **Conferma di salvataggio uscendo dal pannello F2 con Esc**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Prima, `Esc` in F2 scartava sempre le modifiche non salvate senza
    avvisare — solo `Ctrl-S` salvava. **Deciso**: `Ctrl-S` resta
    invariato (salva ed esce direttamente, nessun prompt). `Esc` ora
    confronta la copia locale editata con le impostazioni live
    (`memcmp` sull'intera struct, omogenea a `int32_t`) — se sono uguali esce
    subito come prima; se ci sono modifiche non salvate mostra
    "Salvare le modifiche? y/n/Esc to cancel" nel pannello stesso
    (riusa `editorSettingsRender()`, stesso pattern già usato da
    `editorSettingsEditInt()` per non far sparire il pannello dietro
    un prompt): `y` salva ed esce, `n` scarta ed esce, `Esc` (o
    qualunque altro tasto) annulla e resta nel pannello con le
    modifiche ancora presenti.

- [x] **Backup automatico / recovery da crash** (da IDEAS.md)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Nuovo modulo `backup.c`/`backup.h`, stesso pattern di
    `clipboard.c`/`settings.c` (un file per responsabilità, API
    pubblica minimale in header).
  - **Deciso**: backup salvati in `~/.tinyedit/backup/<hash>.swp`
    (cartella dedicata, mai accanto al file originale — niente da
    aggiungere a `.gitignore`), non nello stile vim `.nomefile.swp`.
    `<hash>` è FNV-1a a 64 bit del path assoluto del file (nessuna
    dipendenza esterna, non serve resistenza a collisioni deliberate,
    solo a evitare collisioni tra file omonimi in cartelle diverse) —
    permette un lookup diretto (`open()` sul path costruito dall'hash)
    invece di scandire la cartella linearmente quando ci sono migliaia
    di backup. La prima riga *dentro* il file `.swp` contiene il path
    assoluto in chiaro, per poterlo identificare aprendolo a mano.
  - **Deciso**: frequenza configurabile da `~/.tinyeditrc`/F2 —
    `backup_interval` in secondi, `0` disattiva (default), altrimenti
    clampato a un minimo di 5s all'uso (non nel parser generico dei
    settings, che validerebbe un range fisso 0-3600 non "0 oppure
    ≥5" — abbassato da 10 a 5s su richiesta esplicita dopo il primo
    giro di test). Scrive un backup al primo giro del loop dopo che il
    buffer diventa dirty, poi rispetta l'intervallo per le scritture
    successive — non ad ogni tasto, per non martellare il disco.
  - Scrittura via file temporaneo + `rename()` (atomico sullo stesso
    filesystem), così un crash a metà scrittura del backup stesso non
    lascia mai un `.swp` corrotto che il prossimo avvio scambierebbe
    per un recupero valido.
  - Il backup viene rimosso (`backupRemove()`) dopo un salvataggio
    riuscito (`editorSave()`) e all'uscita pulita (`editorQuit()`,
    sia perché il buffer era già pulito sia dopo `n` che scarta
    esplicitamente le modifiche) — la sua sola presenza al prossimo
    avvio è il segnale di crash che `editorOfferBackupRecovery()`
    controlla: se c'è un backup per il file appena aperto, chiede
    "Restore it? (y/n)" prima di procedere; su `y` sostituisce
    l'intero buffer già caricato da `editorOpen()` col contenuto del
    backup (`editorClearRows()` + `editorLoadLines()`, quest'ultima
    fattorizzata da `editorOpen()` per condividere il parsing riga per
    riga CRLF-tollerante tra file su disco e contenuto di backup) e
    marca il buffer dirty (differisce ora da quanto su disco).
  - Un buffer nuovo senza nome file (mai salvato con Ctrl-S) non ha
    un path stabile da cui derivare l'hash, quindi non viene
    backuppato finché non riceve un nome.
  - **Fix: prompt di recovery poco visibile**. Prima il prompt viveva
    solo nella barra messaggi in basso (`editorSetStatusMessageSticky`),
    facile da non notare aprendo un file — perdere silenziosamente
    l'occasione di recuperare lavoro non salvato è un esito peggiore
    di un avviso invadente. Fix: `editorRecoveryScreen()`, overlay a
    tutto schermo stile F1/F2 con il messaggio centrato ("UNSAVED
    CHANGES FOUND", path del file, istruzioni y/n) — impossibile non
    notarlo all'avvio, mostrato solo quando `backupExists()` è vero.
  - **Deciso**: colore della barra `\x1b[1;7m` (grassetto + video
    invertito, scambia foreground/background del terminale corrente),
    non una coppia di colori fissa tipo nero-su-giallo — evita di
    indovinare una combinazione leggibile su ogni possibile tema di
    terminale, ed è lo stesso escape già usato per l'evidenziazione
    della selezione e le intestazioni F1/F2, coerente col resto dell'UI.
  - **Deciso**: intervallo minimo abbassato da 10 a 5 secondi.

- [x] **Creazione automatica di `~/.tinyeditrc` al primo avvio**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Prima il file appariva su disco solo al primo `Ctrl-S` dentro il
    pannello F2 — se l'utente non apriva mai le impostazioni, non
    c'era mai un file ispezionabile/modificabile a mano. Fix:
    `settingsLoad()` ora chiama `settingsSave()` sui default appena
    calcolati quando il file non esiste ancora, quindi
    `~/.tinyeditrc` compare già dal primissimo avvio. Fallimento
    silenzioso e non fatale (es. `$HOME` in sola lettura): i default
    in memoria restano comunque validi per la sessione corrente,
    come già accadeva prima di questo fix.

- [x] **Ripristino impostazioni di default dal pannello F2 (`Ctrl-D`)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - **Deciso**: `Ctrl-D` (libero, non ancora usato in nessuna
    schermata) resetta solo la copia locale in editing (`edited`),
    non scrive subito su disco — stesso principio di ogni altra
    modifica nel pannello: serve poi `Ctrl-S` per renderla live/
    persistente, oppure `Esc` la scarta (che ora, essendo `edited`
    diverso da `S` dopo il reset, chiede conferma come per qualunque
    altra modifica non salvata). Non tocca gli override
    `filetype.*`, che non fanno parte di `struct editorSettings` e
    non sono editabili da F2.

- [x] **Fix: pannello F2 non scrollava, sforava lo schermo con molte voci**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Bug latente riemerso aggiungendo `auto_indent`/`auto_close_pairs`
    (13 voci totali): `editorSettingsRender()` disegnava sempre tutte
    le voci senza controllare se stavano nello schermo — su terminali
    bassi il pannello sforava, rompendo il redraw (stesso bug già
    risolto una volta per il caso "poche voci + terminale piccolo",
    ripresentato perché la lista è cresciuta). Fix: `scroll` come
    parametro esplicito di `editorSettingsRender()` (e propagato a
    `editorSettingsEditInt()`), con `editorSettingsVisibleRows()` a
    calcolare quante voci entrano; `editorSettingsScreen()` clampa lo
    scroll per tenere il cursore sempre visibile ad ogni frame, stesso
    principio di `editorScroll()` per il buffer principale.

- [x] **Auto-indent** (da IDEAS.md)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Nuovo setting `auto_indent` (bool, default `true`). Invio
    (`editorInsertNewlineAutoIndent()`, nuova funzione distinta da
    `editorInsertNewline()`) copia lo spazio bianco iniziale (spazi e
    tab, non altro) della riga da cui si è spezzato sulla riga nuova.
    **Deciso**: `editorInsertNewline()` di per sé resta invariata e
    NON reindenta — è usata anche per gli a-capo dentro testo
    incollato/recuperato da backup (`editorInsertText()`), che non
    deve alterare l'indentazione già presente riga per riga.

- [x] **Auto-chiusura parentesi/virgolette** (da IDEAS.md)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Nuovo setting `auto_close_pairs` (bool, default `true`).
    `editorInsertCharAutoClose()` sostituisce la chiamata diretta a
    `editorInsertChar()` nel case `default:` di
    `editorProcessKeypress()`.
  - **Deciso** coppie gestite (tutte generabili da tastiera, 1 byte
    ASCII): asimmetriche `(` `)`, `{` `}`, `[` `]` (aprire inserisce
    sempre la coppia col cursore in mezzo, mai skip; chiudere fa
    skip-over se il carattere successivo è già la chiusura attesa);
    simmetriche `"`, `'`, `$` (stesso carattere apre/chiude — digitarlo
    quando il carattere successivo è identico fa skip-over invece di
    duplicare, altrimenti apre una nuova coppia). `$` copre anche
    `$...$` LaTeX inline; `$$...$$` (display math) emerge naturalmente
    digitando `$` due volte di fila grazie allo skip-over, senza
    bisogno di un caso speciale.
  - **Deciso**: con una selezione attiva, digitare un carattere di
    apertura avvolge (wrap) la selezione invece di sostituirla — la
    selezione, essendo azzerata da `editorProcessKeypress()` prima
    ancora di arrivare al case `default:`, viene catturata in variabili
    locali (`had_sel`/`had_sel_*`) subito dopo la lettura del tasto,
    prima di quel reset.
  - **Deciso** — virgolette tipografiche curve (`«»`, `""`, `''`):
    NON generate digitando un tasto fisico (non esistono su una
    tastiera standard, solo raggiungibili con sequenze di composizione
    del sistema operativo o incollando testo) — ma se già presenti nel
    testo, digitarne la chiusura a mano quando il cursore le precede fa
    comunque skip-over, stessa cortesia delle coppie ASCII.
  - **Bug evitato in corso d'opera**: il codice esistente legge
    l'input un byte alla volta (`editorReadKey()`), e caratteri UTF-8
    come `»` occupano 2-3 byte — un primo tentativo di skip-over
    confrontava solo l'ultimo byte digitato con la sequenza attesa,
    scattando anche se l'utente non aveva davvero digitato quella
    sequenza (falso positivo su qualunque byte ≥0x80). Fix corretto:
    nuova `editorReadMultiByteKey()` assembla l'intero carattere
    (byte guida + byte di continuazione, letti con ulteriori chiamate
    a `editorReadKey()`, che già li restituisce grezzi per definizione
    — vedi il suo switch, solo `\x1b` ha gestione speciale) PRIMA di
    decidere, così il confronto è sempre sul carattere realmente
    digitato, non su un singolo byte isolato.
  - **Fuori scope, rimandato a IDEAS.md**: auto-chiusura di TAG HTML
    (`<div>` → `</div>`) — richiede riconoscere un pattern più lungo
    di un singolo carattere (il nome del tag), guardando indietro nel
    buffer prima di decidere, e sapere il linguaggio della riga
    corrente — stessa famiglia di complessità del syntax highlighting
    (altra voce in IDEAS.md), non un'estensione naturale della tabella
    coppia-singola di oggi. (`$$...$$` LaTeX invece **non** serve
    trattarlo come token speciale: emerge già correttamente dallo
    skip-over della coppia singola `$` — digitare `$` due volte di
    fila salta oltre il primo `$` di chiusura invece di duplicarlo.)

- [x] **Fix: `$$$$` invece di `$$|$$` digitando `$` quattro volte di fila**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - **Correzione della nota sopra**: si è rivelata sbagliata quando
    testata — `$` `$` `$` `$` dava effettivamente `$$$|$` al terzo
    carattere, non `$$|$$`. Lo skip-over del secondo `$` porta il
    cursore a fine riga (niente più `$` a destra), quindi il terzo
    `$`, non trovando nulla da saltare, apriva una *nuova* coppia
    singola invece di essere riconosciuto come parte di un `$$` in
    formazione.
  - **Primo tentativo fallito, per la cronaca**: intercettare "cursore
    tra due `$`" (`chars[cx-1]=='$' && chars[cx]=='$'`) per espandere
    a `$$|$$` — sbagliato perché quella condizione è già vera **dopo
    il primo** `$` digitato (l'auto-chiusura crea subito `$|$`), non
    solo al terzo: risultato, ogni carattere digitato raddoppiava i
    `$` invece che solo la sequenza esatta.
  - **Fix corretto**: la condizione giusta guarda **due** caratteri a
    sinistra del cursore (`chars[cx-2]` e `chars[cx-1]`, entrambi
    `$`) *e* verifica che non ci sia nulla da saltare a destra
    (`nothing_to_skip`) — combinazione che si presenta solo dopo il
    1°+2° `$` di questa identica sequenza (skip-over normale già
    consumato il 2°), mai da coppie `$...$` scritte separatamente
    altrove nella riga. Trasforma `$$|` in `$$|$$`.
  - **Compromesso accettato esplicitamente**: il 4° `$` (dopo
    l'espansione) fa comunque skip-over di **un solo livello**
    (struttura annidata `$$|$$` → `$$$|$`, non `$$$$|` come sarebbe
    "perfetto") — un vero doppio-skip per uscire da entrambi i livelli
    in un colpo richiederebbe rilevare "i due caratteri di chiusura
    rimanenti sono adiacenti senza contenuto scritto in mezzo",
    un'altra fetta di casistica specifica per un beneficio marginale.
    Basta una Freccia Destra in più per uscire del tutto — comunque
    un netto miglioramento rispetto al bug originale (struttura
    corretta, solo cursore su un livello intermedio, non caratteri
    duplicati/mal posizionati).
  - Deliberatamente ristretto a `$` (non generalizzato a `"`/`'`):
    `$$` è una costruzione LaTeX reale e diffusa, `""`/`''` raddoppiati
    non hanno una convenzione equivalente che valga la complessità.

- [x] **Auto-apertura completa per virgolette curve e backtick**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - **Virgolette curve (`«»`, `""`, `''`)**: prima solo skip-over su
    testo già presente (deciso così inizialmente perché non digitabili
    da tastiera fisica standard). Richiesta esplicita di estendere
    anche all'auto-apertura completa (come `(`/`{`/`[`): una volta che
    il carattere di apertura arriva comunque tramite composizione
    OS/incolla e viene assemblato da `editorReadMultiByteKey()`, non
    c'è motivo di trattarlo diversamente da un tasto ASCII normale a
    quel punto — `autoCloseMultiByteTable` esteso con anche l'apertura
    (prima solo la chiusura), stessa logica di auto-chiusura/wrap-
    selezione delle coppie ASCII applicata ai byte multi-carattere.
  - **Backtick singolo `` ` `` (code inline Markdown)**: aggiunto alla
    tabella delle coppie simmetriche, stesso meccanismo di `"`/`'`/`$`,
    **con un'eccezione esplicita allo skip-over** (richiesta utente
    successiva): a differenza di virgolette/`$`, un backtick isolato è
    anche sintassi Markdown valida ripetuta di per sé (più code-inline
    consecutivi sulla stessa riga), quindi digitarlo mentre il cursore
    è già prima di una chiusura auto-inserita NON salta oltre — apre
    sempre una coppia nuova. Verificato via pty: `` ` `` → `` `|` ``,
    poi `` ` `` `` ` `` → `` ```` `` `` `` (quattro backtick, due coppie
    annidate, nessuno skip).
  - **Backtick triplo ` ``` ` (code fence) deliberatamente NON
    implementato come caso speciale**: verificato che VS Code aveva
    provato esattamente questo (auto-chiusura del code fence
    Markdown), e gli utenti l'hanno segnalato come comportamento più
    fastidioso che utile — Microsoft l'ha riconosciuto come probabile
    bug e ha rilasciato un fix per correggerlo. Conferma che "N
    ripetizioni dello stesso carattere = token speciale" è
    strutturalmente scivoloso anche per editor maturi con parser
    dedicati (vedi anche il compromesso accettato per `$$` sopra).
    Il triplo backtick resta tre digitazioni indipendenti sulla
    coppia semplice.

- [x] **Setting `insert_spaces_for_tab`: Tab inserisce spazi invece del carattere tab**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita: preferenza per spazi invece di tab
    nell'indentazione. Nuovo setting (bool, default `true`) — quando
    attivo, il tasto Tab inserisce `S.tab_stop` spazi invece del byte
    `\t`; quando disattivo, resta il comportamento originale (inserisce
    `\t`, poi espanso visivamente da `editorUpdateRow()` secondo
    `tab_stop`). **Deciso**: `auto_indent` copia sempre fedelmente
    l'indentazione già presente nella riga sorgente (se ha tab, copia
    tab; se ha spazi, copia spazi) — non normalizza/converte, per non
    alterare a sorpresa l'indentazione di file che non ha scritto
    l'utente in questa sessione.

- [x] **Setting per mostrare i caratteri invisibili** (spazi, tab, fine riga)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Nuovi setting `show_invisibles` (bool, default `false`) e
    `color_invisibles` (enum colore, stessa palette a 8 colori ANSI di
    gutter/selezione/statusbar, default `gray`).
  - **Vincolo tecnico scoperto e rispettato**: `row->render` è un
    buffer di byte che tutto il resto dell'editor (segmentazione wrap,
    mapping cursore/rx, selezione) indicizza assumendo che 1 byte in
    `chars` corrisponda alla stessa quantità di byte in `render` — un
    vero glifo Unicode (es. `·` middle dot, 2 byte UTF-8) romperebbe
    silenziosamente `row->rsize` e ogni offset calcolato da esso.
    **Deciso**: placeholder ASCII a singolo byte — spazio → `.`
    (`INVISIBLE_SPACE_GLYPH`), tab → `>` seguito dagli spazi di
    riempimento normali fino al tab-stop (`INVISIBLE_TAB_GLYPH`, solo
    il primo carattere della sequenza tab cambia). Fine riga → `$`,
    ma quello è disegnato *separatamente* dopo il testo in
    `editorDrawRows()` (non sostituisce un byte esistente), quindi non
    soggetto allo stesso vincolo — teoricamente potrebbe essere un
    glifo Unicode, tenuto ASCII per coerenza visiva con gli altri due.
  - I glifi vengono scritti direttamente in `row->render` da
    `editorUpdateRow()` (non con un percorso di disegno parallelo
    chars+render, scartato per rischio/complessità maggiore) — quindi
    riusano intatta tutta la logica di wrap/selezione/ricerca già
    esistente. Contropartita: nuova `editorUpdateAllRows()`, chiamata
    quando `show_invisibles` (o `tab_stop`) cambia da F2, per
    ricalcolare `render` su tutte le righe già esistenti — altrimenti
    le righe non toccate da un edit dopo il cambio setting
    continuerebbero a mostrare lo stato vecchio finché non vengono
    modificate. **Nota collaterale**: questo chiude anche un gap
    preesistente per `tab_stop` da solo (cambiarlo da F2 non
    ricalcolava mai le righe già caricate).
  - **Deciso**: `color_invisibles` si applica sia al fine riga sia agli
    spazi/tab in linea (`editorDrawRowSegment()` esteso per riconoscere
    i byte glifo), ma la selezione/il match di ricerca hanno sempre la
    precedenza — un glifo invisibile dentro una selezione attiva prende
    il colore di selezione uniforme, non il suo colore dedicato
    (comportamento verificato: coerente con come ogni altro editor
    tratta i glifi placeholder, mai in competizione visiva con un
    highlight attivo).
  - **Verificato**: aggiungere nuove chiavi al formato di
    `~/.tinyeditrc` non rompe i file di configurazione scritti da
    versioni precedenti — `settingsLoad()` parte sempre da
    `settingsDefaults()` e sovrascrive solo le chiavi effettivamente
    presenti nel file, quindi le voci mancanti (introdotte da un
    aggiornamento) restano automaticamente ai valori di default senza
    bisogno di alcuna migrazione esplicita. Comportamento già garantito
    dal design esistente, testato esplicitamente in questa sessione
    con un `.tinyeditrc` "vecchio stile" (senza le chiavi di oggi).

- [x] **Palette colori chiara/scura/dim per ogni selettore** (gutter, selezione, status bar, invisibili)
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - `enum settingColor` passa da 8 a 24 voci: ogni tinta base (gray,
    blue, green, yellow, cyan, magenta, red, white) ha tre varianti,
    tutte ANSI standard (non 256-color/truecolor, rispettando il
    vincolo già documentato per la palette): `*_LIGHT` (bright,
    `\x1b[9Xm`), `*_DARK` (intensità normale, `\x1b[3Xm`), `*_DIM`
    (faint, `\x1b[2;3Xm` — richiesto esplicitamente dopo il primo giro
    a 16 colori; supporto meno uniforme tra terminali di light/dark,
    alcuni lo rendono identico a dark invece di attenuarlo davvero, ma
    resta ANSI base). Chiavi nel file di config in formato
    `<colore>-light`/`<colore>-dark`/`<colore>-dim` (es. `cyan-dim`).
  - **Bug di migrazione trovato e corretto prima di essere rilasciato**
    (già alla prima versione a 16 colori, poi esteso a 24 con lo
    stesso meccanismo): un `~/.tinyeditrc` scritto da una versione
    precedente (formato vecchio, es. `color_gutter = cyan` senza
    suffisso) non avrebbe trovato corrispondenza esatta nella nuova
    tabella nomi — il parser esistente (`enumIndexOf()` senza match →
    nessuna scrittura, lascia il default) avrebbe **silenziosamente
    perso** la personalizzazione dell'utente al primo avvio dopo
    l'aggiornamento, proprio il tipo di rottura che l'altra voce sopra
    (fill dei default per chiavi mancanti) mira ad evitare — qui però
    la chiave *c'è*, solo con un valore nel vecchio formato, caso
    diverso non coperto da quel meccanismo. Fix: nuova
    `settingColorFromLegacyName()`, usata come fallback in
    `settingsLoad()` quando il nome esatto non matcha e il descrittore
    è uno dei quattro color enum — mappa ciascuno degli 8 nomi vecchi
    alla propria variante `*_LIGHT` (quella che il vecchio codice
    ANSI-unico rendeva già visivamente, vedi `ansiColorCode()`); lo
    stride tra varianti (`SETTING_COLOR_COUNT / 8`) è calcolato invece
    che hardcoded, così un'eventuale quarta variante futura non
    richiederà toccare di nuovo questa funzione. Verificato end-to-end
    sul file reale dell'utente (formato vecchio, caricato
    correttamente come `cyan-light`/`yellow-light`/`green-light`;
    salvataggio da F2 riscrive nel nuovo formato).
  - **Usabilità**: con la palette cresciuta a 24 voci, ciclare un
    colore con solo Invio/Spazio (avanti di uno per volta) rendeva
    scomodo tornare indietro se si superava il valore voluto — fino a
    23 pressioni per fare un giro completo. Fix: `Freccia Sinistra`/
    `Freccia Destra` ora ciclano un valore `SETTING_ENUM` indietro/
    avanti quando il cursore è su quella riga (no-op su righe
    BOOL/INT, dove Su/Giù restano l'unica navigazione); Invio/Spazio
    restano "avanti" per compatibilità con l'uso già preso. La stringa
    di help del pannello aggiornata di conseguenza (menzionava solo
    Invio/Spazio, non le frecce).

- [x] **Indicatori di scroll `^`/`v` nel pannello F2**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita: con lo scroll del pannello (introdotto quando
    la lista è cresciuta oltre l'altezza terminale, vedi sopra) non
    era ovvio a colpo d'occhio se ci fossero altre voci sopra/sotto
    quelle visibili. **Deciso**: colonna a sinistra come un piccolo
    gutter (stesso principio del gutter numeri di riga dell'editor
    principale) — `^` sulla prima riga visibile se ci sono voci
    scrollate sopra, `v` sull'ultima riga visibile se ce ne sono
    sotto, spazio altrimenti. `editorSettingsDrawRow()` prende un
    nuovo parametro `scroll_indicator` invece di calcolarlo da sé, dato
    che serve sapere se la riga corrente è la prima/ultima visibile —
    informazione che solo il chiamante (`editorSettingsRender()`, che
    già conosce `scroll`/`visible`) possiede.

- [x] **Sequenze escape sconosciute che finiscono come testo letterale nel buffer**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente (originariamente per Shift+Invio, es.
    `;2;13~` visibile nel testo), poi esteso: non solo Shift+Invio ma
    "altri comandi escape" generico. **Deciso**: hardening generico in
    `editorReadKey()` invece di riconoscere ogni combinazione una per
    una (approccio scartato: avrebbe richiesto continue segnalazioni
    dell'utente combinazione per combinazione).
  - **Causa root confermata**: per sequenze CSI con layout a 3 campi
    `;`-separati (es. `ESC[27;2;13~`, il formato "modifyOtherKeys" che
    alcuni terminali usano per Invio/tasti modificati), il parsing
    esistente legge solo `mod` e assume che il byte successivo sia già
    il terminatore (`term`). Se invece è un altro `;` o cifra (terzo
    campo), lo switch su `term` non trova match e la funzione ritorna
    `\x1b` — ma i byte residui della sequenza (es. `13~`) restano nello
    stream e vengono letti uno a uno dalla chiamata *successiva* a
    `editorReadKey()`, finendo inseriti come testo letterale. Non è un
    problema isolato di Shift+Invio: qualunque sequenza CSI con un
    layout non previsto dal parser innesca lo stesso sintomo.
  - **Fix**: nuova `editorDrainUnknownCsiSequence(max)` — quando il
    parsing di una sequenza CSI numerica (`ESC[<cifra>...`) non
    riconosce il layout, drena i byte residui dello stream fino al
    prossimo terminatore CSI (byte 0x40-0x7E, definizione ANSI
    X3.64/ECMA-48) o fino a un tetto di 16 byte, invece di lasciarli
    per la prossima lettura. Applicato in due punti:
    1. nel ramo a 2+ parametri (`ESC[N;mod<term>`) quando `term` non
       matcha nessun caso noto — il caso che causava il bug originale;
    2. nel ramo `ESC[<lettera>` quando la lettera è `<` (SGR mouse
       reporting) o `?` (risposta DEC private-mode) — prefissi noti di
       sequenze multi-byte, non terminatori essi stessi.
  - **Deliberatamente NON esteso a ogni lettera sconosciuta dopo
    `ESC[`**: per le sequenze già a 2 byte totali (`ESC[` + una
    lettera, es. ogni caso già gestito nello switch esistente) drenare
    "a scopo precauzionale" rischierebbe di consumare il prossimo vero
    tasto dell'utente in attesa di un terminatore che è già passato —
    il drain scatta solo per i prefissi (`<`, `?`) o i layout (3+
    campi `;`) di cui sappiamo per certo che ci sono altri byte
    pendenti, mai su una semplice ipotesi.
  - Verificato via pty (`HOME` isolato): inviata `ESC[27;2;13~` (il
    formato a 3 campi) seguita da testo normale (`world`) — nessun
    byte residuo finisce nel buffer, il testo digitato dopo appare
    correttamente carattere per carattere. Verificato anche che le
    sequenze di navigazione esistenti (frecce, Shift+frecce,
    Shift+PageUp) continuino a funzionare senza regressioni.

- [x] **Syntax highlighting basico per C**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Nuovo modulo `syntax.c`/`syntax.h` (coerente con la convenzione
    modulare del progetto), scope deliberatamente ristretto al solo C
    (il linguaggio del progetto stesso) per una prima versione — non un
    dispatcher multi-linguaggio, vedi `IDEAS.md` per Python/Markdown/
    shell come estensione futura. Tokenizer per riga stile kilo:
    keyword/tipi standard C, stringhe (`"`/`'`, gestisce `\"` escaped),
    commenti `//` e `/* */` (inclusi multi-riga, con lo stato "dentro
    un commento aperto" propagato riga-per-riga via
    `erow.hl_open_comment`), numeri, direttive preprocessore (riga che
    inizia con `#`).
  - **Attivazione**: nuovo setting `syntax_highlight` (bool, default
    `true`) come interruttore generale, più auto-detect per file
    tramite l'estensione (`.c`/`.h` — non riusa la tabella `~30`
    linguaggi di `filetypeForExtension()`, che serve solo a etichettare
    la barra di stato: applicare il tokenizer C a un linguaggio che non
    è C lo renderebbe male, quindi il controllo è ristretto). Cinque
    colori indipendenti (`color_syntax_keyword/string/comment/number/
    preprocessor`), stessa palette a 24 colori di gutter/selezione/
    invisibili — nessuno schema fisso.
  - **Storage/costo**: un array `erow.hl` (un byte `enum
    syntaxHighlight` per colonna di `render`, `NULL` quando
    l'evidenziazione non è attiva per quella riga) ricalcolato da
    `syntaxHighlightRow()` dentro `editorUpdateRow()` — quindi solo
    quando una riga cambia davvero, non ad ogni redraw a schermo intero
    (vedi la nota costo-O(n) in `IDEAS.md`). Un cambio che apre/chiude
    un commento a blocco può invalidare tutte le righe successive: nuova
    `editorRehighlightFrom()` ripropaga in avanti finché lo stato
    "dentro un commento aperto" non torna uguale a quello di prima
    (oltre quel punto, per costruzione, nessuna riga successiva può
    cambiare).
  - **Priorità di disegno** in `editorDrawRowSegment()`: selezione/match
    di ricerca > colore invisibili > colore sintassi > colore di
    default — stessa gerarchia già stabilita per gli invisibili, il
    syntax highlighting non compete mai visivamente con una selezione
    attiva.
  - Undo/redo: gli snapshot non portano `hl` (sarebbe stato ridondante
    da ricalcolare comunque), `editorRestoreSnapshot()` richiama
    `editorRehighlightFrom(0)` dopo aver ripristinato le righe.
  - Verificato via pty (`HOME` isolato): `#include`/`#define` in giallo,
    commenti `//` e `/* */` in grigio attenuato, `int`/`char` in blu,
    stringhe in verde, numeri in magenta, su un file `.c` di prova.

- [x] **Syntax highlighting: estensione a C++, Python, Shell, JS/TS, Markdown, HTML/XML, CSS**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita di estendere oltre al solo C. **Architettura**:
    prima del refactoring, `syntaxHighlightRow()` aveva C hardcoded;
    ora è parametrizzata da `struct syntaxLang` (estensioni, tabella
    keyword/tipi, caratteri di quote, commento a riga, delimitatori
    commento a blocco, flag `#`-preprocessore) e un tokenizer condiviso
    `syntaxHighlightRowGeneric()` la usa per ogni linguaggio "C-like".
    Aggiungere C++/Python/Shell/JS è stato solo popolare
    `syntaxLangTable[]` con un descrittore ciascuno — zero nuovo codice
    di scansione.
  - **C++**: stesse regole del C, tabella keyword estesa con le parole
    chiave specifiche (`class`, `template`, `namespace`, `nullptr`,
    ecc.), tolte `register`/`restrict` (deprecate/assenti in C++).
  - **Python**: niente commenti a blocco (solo `#` a riga), niente
    trattamento speciale delle stringhe triple-quote (`"""`/`'''`) —
    **compromesso accettato**: renderizzano come tre stringhe a
    carattere singolo consecutive invece di un blocco unico; un
    tokenizer Python dedicato per gestirle correttamente è rimandato,
    non giustificato dal beneficio per una prima versione.
  - **Shell**: niente commenti a blocco, `#` è commento a riga (non
    preprocessore, a differenza di C — flag `hash_line_is_preprocessor`
    a `false`).
  - **JS/TS**: aggiunto il backtick alle quote (template string), tipi
    TypeScript (`interface`, `type`, `boolean`, ecc.) nella stessa
    tabella keyword — niente distinzione JS/TS separata, condividono
    l'estensione `.js`/`.jsx`/`.ts`/`.tsx`.
  - **Markdown, HTML/XML, CSS**: NON forzati nella tabella C-like
    (grammatica strutturalmente diversa — markup/selettori, non
    keyword+stringhe+commenti; `#fff` esadecimale CSS o `.class`
    selettore avrebbero fatto scattare erroneamente le regole
    numero/preprocessore). Tre tokenizer dedicati in `syntax.c`,
    agganciati nel dispatcher `syntaxHighlightRow()` prima del
    controllo sulla tabella generica:
    - **Markdown**: intestazioni ATX (`#` a inizio riga) → colore
      preprocessore (riuso della classe come "marcatore strutturale"),
      code span `` `...` `` e code fence ` ``` ` (multi-riga, stesso
      meccanismo di propagazione `hl_open_comment` del commento a
      blocco C) → colore stringa, enfasi `*...*`/`**...**`/`_..._` →
      colore keyword. Nessun evidenziamento annidato del linguaggio
      dentro un code fence (fuori scope, stessa politica "niente
      sintassi annidata" della versione C-only originale).
    - **HTML/XML**: `<`/`>`/`/` → colore keyword (marcatori di tag),
      valori attributo tra virgolette → colore stringa, `<!-- -->` →
      colore commento (multi-riga). Il nome del tag stesso non è
      colorato separatamente (prima versione), solo i suoi delimitatori.
    - **CSS**: commenti `/* */` (CSS non ha commenti a riga), valori
      stringa tra virgolette, nomi di proprietà (parola seguita da `:`)
      → colore keyword.
  - Verificato via pty (`HOME` isolato) su un file di prova per
    ciascuno dei 7 nuovi linguaggi: colori corretti per keyword/
    stringhe/commenti/numeri in tutti i casi.

- [x] **Markdown: distinguere corsivo (`*...*`/`_..._`) da grassetto (`**...**`/`__...__`)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente: entrambi rendevano con lo stesso colore
    (`HL_KEYWORD`). Nuova classe `HL_EMPHASIS_STRONG` (enum
    `syntaxHighlight`, `syntax.h`) e relativo setting
    `color_syntax_emphasis_strong` (default rosso chiaro), usata dal
    tokenizer Markdown quando il "run" di marcatori aperto/chiuso è
    ≥2 caratteri; il marcatore singolo (corsivo) continua a usare
    `color_syntax_keyword` come prima. Verificato via pty: `*italic*`
    e `_italic2_` in blu (keyword), `**bold**` e `__bold2__` in rosso
    (nuova classe), sulla stessa riga.

- [x] **Linguaggi di sintassi custom da file esterni (`~/.tinyedit/syntax/*.conf`)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita: possibilità di aggiungere un linguaggio "C-like"
    (Matlab è stato l'esempio guida) senza ricompilare l'editor.
    **Deciso**: stesso formato INI-style di `~/.tinyeditrc` (`chiave =
    valore`, `#` commento) invece di YAML — evita di scrivere/vendorizzare
    un parser YAML in C puro solo per questo, che avrebbe violato il
    vincolo "zero dipendenze esterne", introducendo
    comunque un parser nuovo da mantenere. Riusa lo stile di parsing già
    presente in `settings.c` (trim, split su `=`, tolleranza a righe
    malformate), non il codice stesso (dati diversi: `struct syntaxLang`
    invece di `struct editorSettings`).
  - **Formato file**: `extensions`/`keywords` come liste separate da
    virgola (obbligatorie: senza entrambe il file è ignorato),
    `quote_chars`/`line_comment`/`block_comment_start`/
    `block_comment_end`/`hash_line_is_preprocessor` opzionali — stessi
    campi di `struct syntaxLang` usati dai linguaggi built-in, quindi
    zero nuova logica di tokenizzazione: un file utente produce lo
    stesso identico struct che alimenta `syntaxHighlightRowGeneric()`.
  - **Caricamento**: nuova `syntaxLoadUserLangs()` in `syntax.c`, lazy
    (alla prima `syntaxLangForFilename()`, non all'avvio a prescindere —
    zero costo se `syntax_highlight` non serve mai in quella sessione)
    e una tantum (flag `userLangsLoaded`, non ri-scansiona la directory
    ad ogni tasto). Scansiona `~/.tinyedit/syntax/*.conf` con
    `opendir()`/`readdir()`, un file malformato o senza
    `extensions`/`keywords` viene scartato silenziosamente (stessa
    tolleranza di `~/.tinyeditrc`). Ogni entry utente è heap-allocata
    (a differenza dei linguaggi built-in, `static const` in memoria
    statica) e mai liberata — vive per tutta la durata del processo,
    come le tabelle built-in.
  - **Precedenza**: un file utente per un'estensione già coperta
    nativamente (es. ridefinire `.c`) vince sul built-in — stessa
    filosofia "ultima definizione vince" già usata per `filetype.*` in
    `~/.tinyeditrc`, nessun avviso di conflitto.
  - **Fuori scope, dichiarato esplicitamente**: Markdown/HTML/CSS (i tre
    tokenizer dedicati, non nella tabella C-like) non sono estendibili/
    sostituibili da file esterno — solo un linguaggio con grammatica
    riconducibile a keyword+stringhe+commenti può essere aggiunto così.
  - Verificato via pty (`HOME` isolato): file `~/.tinyedit/syntax/matlab.conf`
    di prova (`extensions = m`, keyword Matlab, `quote_chars = '"`,
    `line_comment = %`, `block_comment_start/end = %{`/`%}`) applicato
    correttamente a un file `.m` — `function`/`end` blu, `%` commento
    grigio, stringa `'hi'` verde, numero magenta.

- [x] **Colore configurabile per il testo "normale" (senza classe di highlight)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita: identificatori/punteggiatura/spazi (`HL_NORMAL`)
    non avevano un colore configurabile — `syntaxColorFor()` tornava
    sempre `NULL` per quel caso, quindi restavano sempre nel colore di
    default del terminale.
  - **Deciso**: nuovo valore `COLOR_TERMINAL_DEFAULT` in `enum
    settingColor` (`settings.h`), aggiunto **in coda** ai 24 colori
    reali (non interfogliato nello schema light/dark/dim) per non
    spostare l'indice di nessuna tinta esistente — importante perché
    `SETTING_COLOR_COUNT` era usato come base per la migrazione dei
    nomi legacy (`settingColorFromLegacyName()`, ora ricalcolata su
    `SETTING_COLOR_COUNT - 1`, non più su `SETTING_COLOR_COUNT`
    direttamente) e ogni `~/.tinyeditrc` già scritto usa gli indici
    esistenti impliciti nel nome. Nuovo setting `color_syntax_normal`,
    default `COLOR_TERMINAL_DEFAULT` — con questo default il
    comportamento è identico a prima (nessun escape ANSI emesso per il
    testo non classificato); impostandolo a una delle 24 tinte reali lo
    ricolora esplicitamente.
  - `syntaxColorFor(HL_NORMAL, ...)` ritorna `NULL` solo se il colore è
    `COLOR_TERMINAL_DEFAULT`, altrimenti l'escape della tinta scelta —
    `editorDrawRowSegment()` non ha richiesto modifiche, tratta già
    `NULL` come "non toccare il colore".
  - Verificato via pty (`HOME` isolato): default invariato rispetto a
    prima della modifica; con `color_syntax_normal = white-dark` in
    `~/.tinyeditrc`, spazi/parentesi/punteggiatura si colorano di
    bianco scuro mentre keyword/numeri restano nei loro colori dedicati.

- [x] **Ricerca con espressioni regolari**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita. **Deciso** (indicazione diretta dell'utente,
    non dedotto): attivazione/disattivazione con una combinazione
    `Ctrl-<tasto>` dentro il prompt `Ctrl-F` esistente, motore
    `<regex.h>` POSIX (`regcomp`/`regexec`, libc, zero dipendenze
    esterne). Tasto scelto: **`Ctrl-G`** (mnemonico
    "grep"/pattern generico), libero da collisioni con le combinazioni
    già usate nel prompt (`Ctrl-R` per cerca-e-sostituisci, frecce per
    prossimo/precedente).
  - **Stato**: nuovo `search_regex_mode` (globale, non un
    `editorSettings` — è una scelta per-ricerca, non una preferenza
    persistente, resettato a 0 a ogni nuovo `Ctrl-F`, coerente con
    `search_dir` che già funziona così). Ctrl-G lo inverte e rilancia
    subito la ricerca dalla posizione di partenza, cosicché il
    cambio di modalità sia visibile immediatamente sul match evidenziato
    invece di aspettare il prossimo tasto.
  - **Prompt dinamico**: `editorPromptCB()` esteso con un parametro
    opzionale `status_fn` (`const char *(*)(void)`, richiamato a ogni
    redraw) invece di un secondo parametro statico — permette al
    prompt di ricerca di mostrare `[regex]`/`[literal]` che si aggiorna
    dal vivo quando si preme Ctrl-G, senza dover ricostruire l'intera
    stringa di prompt a ogni tasto per gli altri chiamanti (`Ctrl-S
    salva come`, il prompt di sostituzione) che non ne hanno bisogno.
  - **Ricerca all'indietro (Frecce Su/Sinistra) con regex**: POSIX
    `<regex.h>` non ha ricerca nativa all'indietro, e l'estensione
    `REG_STARTEND` che l'avrebbe resa diretta è solo BSD/macOS, assente
    su glibc/Linux (il progetto punta a entrambi).
    Nuova `editorRegexFindLastOnRow()`: rilancia `regexec()` da offset
    crescenti sulla riga e tiene l'ultimo match che inizia entro il
    limite richiesto — stessa strategia già usata dal ramo di ricerca
    letterale all'indietro esistente (scansione con `memcmp()` ad ogni
    offset), non la più veloce possibile ma le righe sono tipicamente
    corte e il caso non è nel percorso critico (tasto singolo, non
    per-carattere-digitato).
  - **Bug di lunghezza match evitato in cerca-e-sostituisci**: il
    codice di replace usava `strlen(query)` sia per cancellare il
    testo matchato sia per calcolare l'avanzamento quando si salta
    un'occorrenza — corretto in modalità letterale (pattern e match
    hanno sempre la stessa lunghezza) ma sbagliato in modalità regex,
    dove un pattern come `[0-9]+` può matchare porzioni di lunghezza
    variabile. Sostituito con `E.search_match_len` (la lunghezza del
    match REALE, non del pattern) in entrambi i punti. Aggiunta anche
    una guardia contro loop infinito nel caso limite di match vuoto
    (es. `a*` che matcha zero `a`) combinato con sostituzione vuota,
    che altrimenti non avanzerebbe mai la posizione di scansione.
  - **Indicatore anche nel prompt di sostituzione**: `Ctrl-R` (passa a
    cerca-e-sostituisci) eredita `search_regex_mode` così com'è
    dall'ultimo stato del prompt di ricerca — nessun reset esplicito
    necessario, ma senza un indicatore visivo l'utente non saprebbe se
    il replace sta interpretando il pattern come regex o testo
    letterale. Prompt di sostituzione esteso per mostrare lo stesso
    `[regex]`/`[literal]`.
  - Verificato via pty (`HOME` isolato): ricerca `[0-9]+` con Ctrl-G
    attivo trova/evidenzia "123" (lunghezza 3) in "foo123 bar456";
    cerca-e-sostituisci con lo stesso pattern e sostituzione "NUM",
    "replace all" produce correttamente "fooNUM barNUM" / "bazNUM" (3
    occorrenze, ciascuna di lunghezza diversa dal pattern originale)
    — "Replaced 3 occurrence(s)".

## Da fare

- [x] **Bug: testo digitato nel prompt di ricerca invisibile su schermi stretti**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente con screenshot: con la modalità regex
    (`[regex]`) il prompt di ricerca era diventato abbastanza lungo da
    non entrare più nella riga della barra di messaggi su terminali
    stretti, e il testo effettivamente digitato dall'utente (in coda
    al messaggio) spariva — `editorDrawMessageBar()` troncava sempre
    dalla FINE del messaggio (`msglen = E.screencols`), tagliando via
    esattamente la parte che l'utente sta guardando mentre scrive.
  - **Deciso** (istruzione diretta dell'utente, raffinata in un secondo
    giro dopo un primo tentativo — vedi sotto): tre stadi progressivi,
    non un solo accorciamento fisso —
    1. **Help completo** all'apertura del prompt (`"Search [modo] (Esc
       cancel, Arrows jump, Ctrl-R replace, Ctrl-G regex): "`), finché
       c'è spazio per mostrarlo insieme al testo digitato.
    2. **Forma breve** (`"Search [modo]: "` / `"Replace [modo]: "`)
       quando la combinazione help-completo + query supererebbe la
       larghezza dello schermo — l'elenco scorciatoie sparisce (resta
       comunque raggiungibile da F1), non va ripetuto ad ogni ricerca
       una volta che l'utente lo conosce.
    3. **Scroll della barra** (vedi il fix precedente più sotto, ancora
       valido invariato) quando anche la forma breve non basta più.
  - **Primo tentativo, poi corretto**: inizialmente avevo eliminato
    l'help completo del tutto, tenendo solo la forma breve fissa fin
    dall'apertura del prompt — l'utente ha chiarito che voleva
    l'help completo *come primo stadio*, non la sua rimozione
    permanente; il fix finale reintroduce entrambe le forme con
    passaggio automatico dall'una all'altra.
  - **Implementazione**: `editorPromptCB()` accetta ora un
    `short_prompt` opzionale oltre al prompt principale; ad ogni
    redraw formatta il prompt lungo in un buffer di prova e sceglie
    quello corto solo se il lungo supererebbe `E.screencols` —
    ricalcolato ad ogni tasto, quindi il passaggio tra le tre forme è
    fluido mentre si digita, non deciso una sola volta all'apertura.
    `editorFind()` e `editorFindAndReplace()` passano entrambe le
    varianti; `editorPrompt()` (usato da "Save as:", che non ha
    bisogno di questo) passa `NULL` come `short_prompt` e si comporta
    come prima.
  - Verificato via pty (`HOME` isolato): a 90 colonne, query vuota
    mostra l'help completo; una query di ~18 caratteri fa scattare
    automaticamente la forma breve; una query molto più lunga la
    riempie quasi per intero, con lo scroll-to-tail pronto a subentrare
    oltre quel punto (già verificato nel fix precedente a 40 colonne).

- [x] **Bug: query lunga nel prompt di ricerca tagliata a 5 caratteri sul display (ma il find funzionava)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente con screenshot: digitando `12345678` come
    query, il prompt mostrava solo `...): 12345` — ma la ricerca
    trovava comunque correttamente tutte le occorrenze, segno che il
    buffer interno della query era intatto e il problema era solo nel
    DISPLAY.
  - **Causa root**: due limiti di dimensione indipendenti che non si
    parlavano. Il fix precedente (help completo → forma breve → scroll)
    sceglieva tra prompt lungo/breve confrontando la lunghezza
    formattata solo con `E.screencols` (la larghezza visibile) — ma
    `editorSetStatusMessage()` scrive il risultato in `E.statusmsg`,
    un buffer fisso da soli 80 byte. Il prompt lungo da solo
    (`"Search [literal] (Esc cancel, Arrows jump, Ctrl-R replace,
    Ctrl-G regex): "`) è già 76 caratteri: con 5 caratteri di query si
    arriva a 81, un byte oltre il limite, e `vsnprintf()` tronca in
    silenzio la coda — indipendentemente da quanto è largo il
    terminale. Su un terminale largo il prompt "ci stava" per il
    controllo `E.screencols`, quindi restava sempre nella forma lunga,
    e ogni carattere di query oltre l'80° byte spariva dal display pur
    restando nel buffer di ricerca vero (`buf` dentro
    `editorPromptCB()`, mai toccato da questo bug).
  - **Fix**: 1) `E.statusmsg` ingrandito da 80 a 512 byte
    (`tinyedit.h`) — comodamente più grande di qualunque combinazione
    realistica di larghezza terminale + lunghezza query; 2)
    `editorPromptCB()` ora sceglie tra prompt lungo/breve confrontando
    con **il minimo** tra `E.screencols` e `sizeof(E.statusmsg) - 1`,
    non solo il primo — così anche su un terminale larghissimo il
    passaggio alla forma breve scatta comunque prima che
    `vsnprintf()` possa troncare qualunque cosa.
  - Verificato via pty (`HOME` isolato, terminale a 200 colonne): la
    query `12345678` digitata per intero compare ora integralmente nel
    prompt (`...): 12345678`), nessun troncamento.

- [x] **Markdown: evidenziare LaTeX math `$formula$` / `$$formula$$`**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Il tokenizer Markdown non aveva mai gestito il dollaro — un colore
    "giallo" visto in una sessione precedente su `$$formula$$` non era
    riproducibile con un caso pulito, e non risultava da nessuna regola
    del tokenizer (probabile artefatto non diagnosticato di quel test
    specifico). Nessun codice per `$`/`$$` esisteva prima di questa
    voce.
  - **Deciso**: stesso colore per `$...$` (inline) e `$$...$$`
    (display) — riuso di `HL_STRING`, stessa semantica "testo
    letterale/non-prosa" già usata per i code span. Nessun nuovo
    setting colore dedicato.
  - **Implementazione**: nuovo ramo in `syntaxHighlightRowMarkdown()`
    (prima del case backtick): determina la larghezza del marcatore
    (1 o 2 `$`) guardando se il carattere successivo è anch'esso `$`,
    poi cerca una chiusura della STESSA larghezza — un `$$...$$` deve
    chiudere con `$$`, non con un singolo `$` che compare dentro la
    formula (es. un simbolo di valuta legittimo nella formula stessa).
    Solo a singola riga, stesso limite già dichiarato per l'enfasi
    (nessun tracking multi-riga oltre ai fenced code block).
  - Verificato via pty (`HOME` isolato): `Inline $a+b=c$ and display
    $$x^2+y^2=z^2$$ done.` — entrambe le espressioni colorate in verde
    (stesso colore delle stringhe/code span), delimitazione `$$`
    corretta (non si ferma al primo `$` interno), testo circostante
    (`Inline`, `and display`, `done.`) non colorato.

- [x] **Markdown: colore dedicato per LaTeX math, distinto dai code span**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Domanda esplorativa dell'utente dopo il fix precedente ($ colorato
    come HL_STRING, stesso colore dei code span `` `...` ``): se
    valesse la pena distinguerli. **Deciso**: sì, nuova classe
    `HL_MATH` e setting `color_syntax_math` (default ciano chiaro),
    stesso pattern già usato per `HL_EMPHASIS_STRONG`/
    `color_syntax_emphasis_strong`.
  - Verificato via pty: `` `x=1` `` (code span) in verde,
    `$a+b=c$` (math) in ciano — colori ora distinti nello stesso
    paragrafo.

- [x] **Linguaggi `.conf` custom: supporto per keyword con prefisso (LaTeX `\comando`) e math mode**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - L'utente ha chiesto un file `~/.tinyedit/syntax/latex.conf` di
    esempio; il primo tentativo (keyword `\begin`, `\section`, ecc.,
    tramite il meccanismo `.conf` esistente) non funzionava: il
    tokenizer generico C-like riconosce come possibile inizio di
    keyword solo lettere/underscore, mai `\`, quindi ogni comando
    LaTeX restava non colorato — solo `line_comment = %` funzionava.
  - **Deciso** (l'utente ha respinto l'alternativa "scrivi un
    tokenizer LaTeX dedicato in C", chiarendo che il senso dei file
    `.conf` è proprio evitare di scrivere C per ogni nuovo caso, e che
    il math mode già fatto per Markdown deve essere sfruttabile anche
    da un `.conf`): estesa `struct syntaxLang` con due nuovi campi
    invece di scrivere un tokenizer dedicato —
    - `keyword_prefix_chars` (nuova chiave `.conf` opzionale): caratteri
      extra, oltre a lettera/underscore, che possono iniziare/continuare
      una keyword — `\` per LaTeX. Il tokenizer generico ora controlla
      questo insieme oltre a `isalpha()`/`_` sia per decidere se provare
      un match di keyword sia per consumare il resto del token.
    - `math_mode = true` (nuova chiave `.conf` opzionale): riusa la
      STESSA funzione `syntaxTryHighlightMath()` già scritta per
      Markdown (estratta in una funzione condivisa apposta, non
      duplicata) per riconoscere `$...$`/`$$...$$` → `HL_MATH`, prima
      di qualunque altra regola del tokenizer generico.
    - Entrambi i campi sono `NULL`/`0` per default nei linguaggi
      built-in esistenti (inizializzazione posizionale C99, i campi
      aggiunti in coda allo struct restano a zero per gli initializer
      con meno elementi) — nessuna regressione per C/C++/Python/Shell/
      JS.
  - **`~/.tinyedit/syntax/latex.conf`** creato come esempio d'uso reale
    (non solo di test): `extensions = tex,latex,sty,cls`, keyword dei
    comandi LaTeX più comuni (`\begin`, `\section`, `\ref`, ecc.),
    `line_comment = %`, `keyword_prefix_chars = \`, `math_mode = true`.
  - Verificato via pty (`HOME` isolato, con lo stesso `latex.conf`
    installato in `~/.tinyedit/syntax/` dell'utente): `\begin`,
    `\section`, `\end` colorati come keyword, `%` come commento,
    `$a+b=c$` come math (stesso `HL_MATH`/colore usato in Markdown) —
    tutto tramite il file `.conf`, zero nuovo tokenizer dedicato per
    LaTeX.

- [x] **LaTeX math_mode: aggiunto supporto per `\[...\]`/`\(...\)`, incluso `\[...\]` multi-riga**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente: `\[...\]` è anch'esso display math in LaTeX,
    equivalente a `$$...$$` — non riconosciuto dal `math_mode` appena
    aggiunto (solo `$`/`$$`). Prima estensione (single-line) ha coperto
    anche `\(...\)` (inline, equivalente a `$...$`), ma `\[...\]` in
    pratica è quasi sempre scritto con i delimitatori su righe separate
    dal contenuto — il che richiedeva propagazione multi-riga, non
    ancora presente.
  - **Nuova funzione condivisa** `syntaxTryHighlightDelimited()`
    (delimitatori a larghezza fissa, usata per `\[`/`\]` e `\(`/`\)`,
    a differenza di `$`/`$$` che condividono lo stesso carattere per
    due larghezze diverse e restano nella loro logica dedicata).
  - **Multi-riga**: nuovo campo `erow.hl_open_math` (`tinyedit.h`),
    parallelo a `hl_open_comment` ma tenuto separato — un linguaggio
    potrebbe in teoria avere sia un commento a blocco aperto sia math
    aperto contemporaneamente, e va tracciato distintamente per non
    farli competere sullo stesso bit. `syntaxHighlightRow()` cambia
    firma da "ritorna lo stato commento" a "aggiorna
    row->hl_open_comment/hl_open_math direttamente" (più pulito,
    dato che `row` è già passato per puntatore) — accetta ora anche
    `prev_open_math` oltre a `prev_open_comment`. Propagato ovunque:
    tokenizer generico C-like (nuovo stato `in_math` accanto a
    `in_comment`), tokenizer Markdown (stessa estensione, dato che
    Markdown supporta LaTeX math allo stesso modo), dispatcher,
    `editorRehighlightFrom()`/`editorUpdateRow()` in `tinyedit.c`.
  - `~/.tinyedit/syntax/latex.conf` non ha richiesto modifiche (stesso
    `math_mode = true` di prima) — il multi-riga è comportamento del
    motore, non una nuova chiave di configurazione.
  - Verificato via pty (`HOME` isolato, con `latex.conf` reale
    dell'utente): `\[` / `x^2 + y^2 = z^2` / `\]` su tre righe separate
    colorate correttamente come math (stesso ciano di `$...$`), insieme
    a `\(a+b=c\)`, `$inline$`, `$$display$$` sulla stessa riga (tutti
    single-line, invariati) e `\begin`/`\end` come keyword.

- [x] **Bracketed paste: incollare testo lungo dal terminale era lento e produceva caratteri di chiusura spuri**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente (con screenshot che mostrava il sintomo):
    incollare testo lungo direttamente nel terminale (Cmd+V/tasto
    destro, non il `Ctrl-V` interno dell'editor che già usa la
    clipboard di sistema) impiegava "una vita" ed è emerso, sempre
    dallo screenshot, che lasciava anche caratteri di chiusura
    spuri accodati al testo (es. `'\`\`\`'}'}]')'` dopo del testo con
    parentesi/apici/backtick).
  - **Causa root di entrambi i sintomi, la stessa**: senza bracketed
    paste, il terminale manda il testo incollato sullo stdin del
    programma esattamente come se fosse stato digitato byte per byte —
    ogni carattere passa per `editorProcessKeypress()` individualmente,
    quindi: 1) un redraw completo + uno snapshot di undo per carattere
    (lento, O(lunghezza incollata) chiamate invece di una), e 2) ogni
    `(`, `'`, `` ` ``, `[` nel testo incollato attiva
    `editorInsertCharAutoClose()` come se l'utente lo avesse digitato
    apposta, inserendo una chiusura automatica che nel testo originale
    non c'era.
  - **Fix**: supporto al bracketed paste mode (`\x1b[?2004h`/`l`,
    estensione terminale ampiamente supportata, non uno standard
    POSIX/termios). Attivato con `enableBracketedPaste()` all'avvio
    (disattivato all'uscita via `atexit`, per non alterare il
    comportamento di incolla della shell/programma successivo). Da quel
    punto il terminale avvolge ogni testo incollato tra `ESC[200~` e
    `ESC[201~` invece di mandarlo come tasti singoli.
  - **`editorReadKey()`**: nuovo riconoscimento di `ESC[200~` →
    `PASTE_START_KEY` (nuovo valore in `enum editorKey`, `tinyedit.h`).
    **Bug intermedio scoperto e corretto durante l'implementazione**: il
    primo tentativo leggeva un byte di troppo poco per `"200~"`/`"201~"`
    (4 byte dopo `[`, non 3 come le altre sequenze CSI già gestite) —
    verificato che il testo salvato non corrispondeva esattamente
    all'input (caratteri residui tipo `~` e frammenti duplicati),
    corretto leggendo esplicitamente i due byte mancanti (terza cifra +
    `~`) prima di decidere.
  - **`editorReadPastedText()`** (nuova funzione): una volta ricevuto
    `PASTE_START_KEY`, legge byte grezzi direttamente da stdin
    (bypassando la logica di decodifica di `editorReadKey()`, che
    interpreterebbe erroneamente un ESC letterale dentro il testo
    incollato come inizio di un'altra sequenza) fino al marcatore di
    fine `ESC[201~`, con matching progressivo tollerante ai falsi
    positivi parziali (un `ESC[20` seguito da altro nel testo incollato
    non chiude prematuramente il paste).
  - **`editorProcessKeypress()`**: nuovo case `PASTE_START_KEY` che
    cancella la selezione attiva (se presente, stesso comportamento di
    `Ctrl-V`) e inserisce il testo con `editorInsertText()` — la STESSA
    funzione già usata da `Ctrl-V`/clipboard di sistema, che chiama
    `editorInsertChar()` direttamente (mai
    `editorInsertCharAutoClose()`), quindi zero auto-chiusura spuria e
    un solo giro di redraw/coalescenza-undo invece di uno per carattere.
  - Verificato via pty (`HOME` isolato): testo con `'\`\`\`'}'}])'')'
    text (a) [b] {c} 'd' \`e\`` incollato via marcatori bracketed-paste
    simulati → salvato **identico byte-per-byte** all'input, nessun
    carattere di chiusura spurio. Timing: 300 righe / 22KB incollate in
    ~31ms fino al messaggio di stato "pasted" (contro il vecchio
    percorso byte-per-byte, che avrebbe fatto 300+ redraw completi).

- [x] **Supporto mouse: click posiziona il cursore, trascinamento seleziona, rotella scrolla**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita dell'utente ("procediamo ora con il mouse"),
    dopo che il mouse era stato deliberatamente escluso dal piano nelle
    sessioni precedenti (vedi la nota tecnica ora rimossa più sotto):
    abilitare il mouse-reporting del terminale disattiva la selezione
    nativa del terminale (Cmd+C/Cmd+V di Ghostty) mentre è attivo.
  - **Deciso**: nuovo setting `mouse_enabled` (F2, default `false`,
    coerente con quanto già documentato — un opt-in esplicito, mai un
    default). Toggle live: cambiarlo da F2 e premere `Ctrl-S` attiva/
    disattiva il mouse-reporting immediatamente, non solo al prossimo
    avvio (stesso trattamento già riservato a `show_top_bar`/
    `tab_stop`).
  - **Protocollo**: SGR mouse reporting (`\x1b[?1002h` per click+drag,
    `\x1b[?1006h` per l'encoding SGR delle coordinate — a differenza
    del vecchio encoding X10, non ha limiti di risoluzione e distingue
    press/release senza ambiguità). Disattivato all'uscita via
    `atexit()`, stessa cautela già usata per il bracketed paste (non
    deve restare attivo per la shell/programma successivo nel
    terminale).
  - **`editorReadKey()`**: nuovo riconoscimento `ESC[<Cb;Cx;Cy(M|m)` →
    `MOUSE_EVENT_KEY` (nuovo valore `enum editorKey`), con parsing
    diretto (i tre campi hanno larghezza variabile, a differenza di
    ogni altra sequenza CSI già gestita che ha layout fisso) —
    popola variabili globali `mouseEventButton`/`Col`/`Row`/`Press`
    lette subito dal chiamante. Il ramo `seq[1] == '<'` esisteva già
    come drain generico (aggiunto durante l'hardening delle sequenze
    escape sconosciute, vedi voce precedente) — ora decodifica invece
    di scartare.
  - **`editorMouseToCursor()`** (nuova funzione): converte una
    coordinata schermo 1-based (quella riportata dal terminale) in una
    posizione file (riga, colonna), inversa esatta della matematica di
    posizionamento cursore già in `editorRefreshScreen()` — deve
    restare sincronizzata con quella, non reinventata a parte. Gestisce
    sia wrap attivo che disattivo, e cammina i caratteri con
    `utf8NextCharLen()` (mai assumendo 1 byte = 1 colonna) per centrare
    correttamente un click su un carattere
    multi-byte/a doppia larghezza.
  - **`editorProcessKeypress()`**, nuovo case `MOUSE_EVENT_KEY`: press
    del bottone sinistro (Cb=0) posiziona cursore e ancora una
    selezione collassata lì; drag (Cb=32, motion col bottone premuto)
    muove il cursore mantenendo l'ancora, estendendo la selezione;
    release smette di tracciare il drag; rotella (Cb=64/65) scorre
    3 righe senza toccare cursore/selezione. `MOUSE_EVENT_KEY` aggiunto
    alla whitelist di tasti che NON azzerano `E.sel_active` a inizio
    `editorProcessKeypress()` (altrimenti la selezione appena creata al
    press verrebbe cancellata prima di poter essere estesa dal drag
    successivo).
  - Verificato via pty (`HOME` isolato, `mouse_enabled = true`):
    riconoscimento attivazione mouse-reporting all'avvio; click
    posiziona correttamente il cursore (verificato tramite la
    posizione riportata in barra di stato); drag sulla stessa riga
    produce selezione con reverse-video visibile nell'output;
    rotellina scorre il testo (una riga sparisce dalla vista) senza
    spostare il cursore nel buffer.

- [x] **Bug: Freccia Su non funzionava nella ricerca regex (e Freccia Su era rotta anche in letterale in un caso limite)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente: con una regex, Freccia Giù trovava
    correttamente il match successivo ma Freccia Su non tornava
    indietro. Investigando è emerso che il bug era in realtà doppio, e
    il secondo (in modalità letterale) esisteva già prima ma non era
    stato notato perché richiede un match a colonna 0.
  - **Bug 1 (il principale, causa del sintomo segnalato)**: la ricerca
    in avanti ripartiva da `last_cx + 1` (un byte dopo l'INIZIO
    dell'ultimo match), non dalla sua fine. Per la ricerca letterale i
    due coincidono di fatto (ogni match ha sempre la stessa lunghezza
    del pattern), ma per una regex con quantificatore (`[0-9]+` che
    matcha `"111"`, 3 caratteri) la ricerca successiva ripartiva DENTRO
    il match appena trovato, trovando un sotto-match invece di
    avanzare alla riga successiva — sintomo osservato: la colonna
    avanzava di continuo (7→8→9) restando sulla stessa riga invece di
    saltare alla riga con la prossima occorrenza. **Fix**: nuovo stato
    `last_len` (lunghezza dell'ultimo match, non solo posizione),
    ripartenza da `last_cx + last_len` invece di `last_cx + 1`.
  - **Bug 2 correlato, in `editorRegexFindLastOnRow()`**: la stessa
    identica confusione inizio-vs-fine esisteva nell'avanzamento
    interno del loop che cerca "l'ultimo match sulla riga" per la
    ricerca all'indietro — avanzava di `mx + 1` (dentro il match appena
    trovato) invece di `mx + mlen` (dopo la sua fine), quindi
    `regexec()` alla iterazione successiva trovava un sotto-match più
    corto che SOVRASCRIVEVA il risultato corretto con uno parziale.
    **Fix**: stessa correzione, avanzamento a `mx + mlen` (minimo 1 per
    non bloccarsi su match vuoti tipo `a*`).
  - **Bug 3, scoperto verificando la modalità letterale dopo il fix
    del bug 1 (non era il sintomo originale, ma una regressione
    preesistente nello stesso codice)**: quando l'ultimo match trovato
    era già a colonna 0, `from_x = last_cx - 1` diventa `-1`, clampato
    a `0` — ma `0` è esattamente dove si trova il match corrente,
    quindi la ricerca all'indietro lo ritrovava di nuovo sulla STESSA
    riga invece di scendere a quella precedente (Freccia Su bloccata su
    un match a colonna 0). **Fix**: quando `last_cx == 0`, saltare
    esplicitamente all'ultima colonna della riga precedente invece di
    clampare a 0 e ricercare di nuovo sulla riga corrente.
  - Verificato via pty (`HOME` isolato) in entrambe le modalità:
    sequenza `giù, giù, su, su` su testo a 3 righe (una occorrenza per
    riga) torna esattamente alle stesse posizioni sia in regex
    (`[0-9]+`, match di lunghezza variabile) sia in letterale (incluso
    il caso limite del match a colonna 0 in entrambe le modalità).

- [x] **Lag del mouse, soprattutto con la rotellina**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente subito dopo l'implementazione del supporto
    mouse, con conferma esplicita che il sintomo si manifestava
    "specialmente con lo scroll".
  - **Causa root**: il main loop fa un redraw completo dello schermo
    per ogni singolo evento processato (`editorRefreshScreen()` dopo
    ogni `editorProcessKeypress()`). Un singolo gesto fisico di scroll
    (rotellina o gesture trackpad) genera tipicamente decine di report
    SGR in rapidissima successione, ognuno dei quali arrivava come
    evento separato — con un redraw completo per ciascuno, il display
    finiva sistematicamente "in coda" rispetto al gesto reale, perché
    mentre un redraw è in corso il terminale continua ad accodare altri
    eventi.
  - **Fix**: nuova `stdinHasDataReady()` (wrapper su `select()` non
    bloccante) e un ciclo di coalescing dentro il case
    `MOUSE_EVENT_KEY` di `editorProcessKeypress()` — dopo aver applicato
    un evento (wheel scroll o drag), controlla se un altro evento mouse
    è già in coda pronto da leggere senza attendere; se sì, lo legge e
    applica anch'esso, ripetendo finché la coda non si svuota, e solo a
    quel punto esce per il redraw. L'intera raffica produce un solo
    redraw invece di uno per evento. Un tasto non-mouse che dovesse
    trovarsi mescolato nella stessa raffica bufferizzata viene
    scartato silenziosamente (caso estremamente raro, non vale la
    complessità di un meccanismo di pushback per questo).
  - Verificato via pty (`HOME` isolato): 20 eventi wheel inviati in
    un'unica raffica producono **1 solo redraw** (misurato contando le
    occorrenze di `ESC[?25l`, emesso una volta per redraw), contro i 20
    redraw del comportamento precedente; un singolo evento isolato
    continua a produrre esattamente 1 redraw (nessun ritardo introdotto
    dal meccanismo di coalescing per lo scroll lento/normale).

- [x] **Bug: lo scroll con la rotellina si bloccava dopo circa due schermate sopra/sotto il cursore**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente: scrollando con la rotellina, la vista si
    fermava a una certa distanza dalla riga selezionata (il cursore) e
    non andava oltre, sia scrollando su che giù.
  - **Causa root**: `editorScroll()` (chiamata ad ogni redraw) forza
    sempre `E.rowoff` a rientrare nella finestra `[cursor_vy -
    screenrows + 1, cursor_vy]` per tenere il cursore visibile —
    corretto per il movimento normale del cursore (frecce, PageUp/Down,
    digitazione), ma la rotellina deliberatamente NON sposta il
    cursore (per design: scrollare la vista non deve spostare dove si
    sta scrivendo). Risultato: non appena la vista veniva scrollata
    abbastanza da uscire da quella finestra attorno al cursore fermo,
    `editorScroll()` la riportava indietro al redraw successivo —
    limitando di fatto lo scroll a circa un'altezza schermo (`
    screenrows`) sopra/sotto la posizione del cursore, in entrambe le
    direzioni.
  - **Fix**: quando il wheel sposta `E.rowoff`, se il cursore
    risulterebbe fuori dalla nuova vista, viene spostato al bordo
    visibile più vicino (riga in alto o in basso, a seconda della
    direzione) — stesso comportamento standard di ogni altro editor:
    la vista scrolla libera, il cursore la segue solo quanto basta per
    restare dentro lo schermo, invece di ancorare la vista al cursore
    fermo. Gestito sia per wrap attivo (via
    `editorFileRowAtVideoRow()`) sia disattivo.
  - Verificato via pty (`HOME` isolato, documento di 200 righe):
    scroll con tick singoli spaziati avanza progressivamente e
    correttamente (`4/200 → 7/200 → ... → 31/200`, 3 righe per tick);
    una raffica di 40 eventi wheel-down consecutivi (simulando un
    gesto rapido, ben oltre il vecchio limite di ~2 schermate) porta
    correttamente a `121/200` in un solo redraw (verifica congiunta col
    fix di coalescing precedente).

- [x] **Bug: scrollando dopo un click o una selezione, la selezione appariva/cresceva da sola**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente con screenshot: click su un punto qualsiasi
    (nessuna selezione visibile), poi scroll → improvvisamente appariva
    una selezione, che continuava a crescere man mano che si scrollava
    ulteriormente. Ripetuto in un secondo turno: anche selezionando
    davvero del testo con drag e poi scrollando, la selezione cambiava
    — l'utente ha chiarito esplicitamente il comportamento atteso: *"la
    selezione è indipendente da ciò che viene visualizzato"*.
  - **Causa root, in due parti**:
    1. Il fix precedente per "lo scroll si blocca dopo 2 schermate"
       (vedi voce sopra) spostava `E.cy` per tenerlo dentro la vista
       ogni volta che la rotellina scrollava. Ma il click semplice
       lasciava `E.sel_active = 1` con un'ancora ferma sul punto
       cliccato (pensato per essere invisibile: `editorGetSelection()`
       tratta ancora==cursore come "nessuna selezione") — peccato che
       quella funzione confronti l'ancora col cursore **corrente**, non
       uno snapshot del momento del click. Spostare `E.cy` per lo
       scroll, con l'ancora ferma, faceva quindi *apparire* una
       selezione mai richiesta dall'utente.
    2. Anche dopo aver escluso il caso "click semplice" (vedi sotto),
       una selezione VERA fatta con drag veniva comunque alterata: il
       tentativo intermedio di trascinare anche `E.sel_anchor_y` insieme
       a `E.cy` (per "far scorrere la selezione insieme alla vista")
       era concettualmente sbagliato — la posizione del testo
       selezionato nel file non ha alcuna relazione con cosa è
       visibile a schermo in un dato momento, va lasciata del tutto
       intoccata dallo scroll.
  - **Deciso** (dopo un tentativo intermedio scartato — vedi sopra):
    soluzione alla radice, non un altro caso speciale. Il vero problema
    era aver fatto muovere `E.cy` dalla rotellina fin dall'inizio.
    **Il mouse wheel ora non tocca mai `E.cy`/`E.cx` né la selezione**
    — sposta solo `E.rowoff` (la vista). Per evitare che
    `editorScroll()` (chiamata ad ogni redraw, che normalmente
    ri-centra sempre la vista attorno al cursore fermo) vanificasse
    subito lo scroll libero, nuovo flag one-shot `E.free_scroll`
    (`tinyedit.h`): impostato dal wheel, fa sì che `editorScroll()`
    salti la ri-centratura per un solo redraw e si auto-azzeri subito
    dopo — qualunque movimento reale del cursore successivo (frecce,
    click, digitazione) torna automaticamente al comportamento normale
    "la vista segue il cursore" al giro successivo.
  - Il fix per il click semplice (non armare più `E.sel_active` finché
    non c'è un vero drag, con l'ancora del click ricordata in variabili
    locali `press_anchor_x/y` fino a quel momento) resta comunque valido
    e utile di per sé, indipendentemente dal fix dello scroll — chiude
    anche il primo scenario riportato dall'utente alla radice.
  - Verificato via pty (`HOME` isolato, 200 righe): 1) scroll libero di
    40 tick (120 righe) porta la vista a "Line 120" mentre lo status
    bar conferma il cursore ancora fermo a `1/200: C 1`, nessun limite
    residuo; 2) drag-select su "Line 4", scroll di 30 righe (la
    selezione esce di vista, correttamente non visibile — non
    cresciuta né sparita dal buffer), poi scroll indietro alla stessa
    posizione → selezione **identica, byte per byte**, a quella
    subito dopo il drag originale.

- [x] **Schermata di aiuto (F1) non aggiornata: mancavano mouse/paste/regex, nessun indicatore di scroll, nessun riferimento ai file di configurazione**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente in due parti: 1) F1 era rimasta ferma a
    prima di tutte le feature aggiunte in questa sessione (mouse,
    bracketed paste, ricerca regex) e non aveva l'indicatore `^`/`v`
    già introdotto per il pannello F2 quando l'elenco supera lo
    schermo; 2) nessuna indicazione su dove trovare altre informazioni
    sul funzionamento (file `.conf` custom, `~/.tinyeditrc`, ecc.).
  - **Voci aggiunte a `helpEntries[]`**: click/drag/wheel del mouse
    (con nota "if enabled, see F2" dato che è disattivo di default),
    paste da terminale (bracketed paste, nessuna auto-chiusura sul
    testo incollato), `Ctrl-G` per il toggle regex dentro la ricerca,
    backtick aggiunto all'elenco dei caratteri di auto-chiusura (era
    rimasto solo `( { [ " ' $`).
  - **Indicatore di scroll**: stessa convenzione già usata dal
    pannello F2 (`editorSettingsDrawRow()`'s `scroll_indicator`) — `^`
    sulla prima voce visibile se ce ne sono altre sopra, `v`
    sull'ultima se ce ne sono altre sotto, disegnato come colonna a
    sinistra (gutter). Richiede calcolare in anticipo `last_visible`
    (quale sarà l'ultima voce effettivamente disegnata in questo
    frame), dato che dipende sia dall'altezza schermo sia da quante
    voci restano — non decidibile finché il limite del ciclo di
    disegno non è già noto.
  - **Nuova sezione "Configuration files"** in fondo all'elenco:
    `~/.tinyeditrc` (tutte le impostazioni di F2, hand-editable),
    `~/.tinyedit/syntax/*.conf` (linguaggi di syntax highlighting
    custom), `~/.tinyedit/backup/` (backup di crash-recovery) — con
    rimando a `README.md` per i dettagli completi, senza duplicare
    tutta la documentazione dentro l'help stesso.
  - Verificato via pty (`HOME` isolato, schermo di 20 righe per
    forzare lo scroll): prima schermata mostra le nuove voci
    mouse/paste/regex con `v` sull'ultima riga visibile; scrollando
    fino in fondo compare `^` sulla prima riga visibile e la nuova
    sezione "Configuration files" con tutti e tre i percorsi.

- [x] **Bug: schermata F1 con reverse-video su tutto lo schermo, titolo e prima riga fuse insieme**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Segnalato dall'utente con screenshot subito dopo l'aggiornamento
    dei contenuti di F1: l'intera schermata appariva in reverse-video
    grigio, con il testo del titolo e della prima voce dell'elenco
    fusi sulla stessa riga invece che andare a capo.
  - **Causa root**: la stringa del titolo veniva scritta con
    `abAppend(&ab, "...", 47)` — un conteggio byte hardcoded rimasto
    da prima che il testo del titolo cambiasse. La stringa reale è 61
    byte; `abAppend()` tronca al numero passato, quindi gli ultimi 14
    byte (`\x1b[m\x1b[K\r\n\x1b[K\r\n` — reset del reverse-video, clear
    di fine riga, ed entrambi gli a-capo) non venivano mai scritti.
    Risultato: il reverse-video restava attivo per tutto il resto del
    frame, e senza l'a-capo il contenuto successivo continuava sulla
    stessa riga del titolo.
  - **Fix**: sostituito il conteggio hardcoded con `strlen()` sulla
    stringa effettiva, così non può più disallinearsi se il testo del
    titolo cambia ancora in futuro.
  - Verificato via pty (`HOME` isolato): titolo ora si chiude
    correttamente (`\x1b[m\x1b[K` dopo "(any key to close)"), riga vuota
    e contenuto vanno a capo normalmente, nessun reverse-video residuo
    sul resto dello schermo.

- [x] **Versione incrementata, schermata info (Ctrl-I): identità del progetto + info sul file corrente**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - Richiesta esplicita: incrementare la release e aggiungere una
    schermata attivabile con Ctrl-I che mostri versione/autore/GitHub
    più informazioni sul file aperto.
  - **Versione**: `TE_VERSION` `0.1` → `0.2` — giustificato dalla mole
    di feature aggiunte in questa sessione (syntax highlighting
    multi-linguaggio, ricerca regex, mouse, bracketed paste, licenza).
    Non `1.0`: nessuna dichiarazione esplicita di stabilità di
    API/formato config finora.
  - **Bug scoperto durante l'implementazione — Ctrl-I collide col
    Tab**: `CTRL_KEY('i')` produce lo stesso codice ASCII (9) del
    tasto Tab per definizione (`CTRL_KEY()` è solo una mascheratura di
    bit, e `Tab`/`Ctrl-I` condividono da sempre lo stesso controllo
    ASCII) — build fallita con "duplicate case value" appena tentato.
    **Verificato con l'utente sul suo terminale reale** (Ghostty, non
    assunto): Ctrl-I lì arriva come sequenza CSI-u
    `ESC[5;5u`, distinta dal semplice byte 0x09 del Tab. Nota
    particolare: il campo "5" qui NON è il codepoint CSI-u standard
    (che per 'i' sarebbe 105) — Ghostty manda un valore diverso in
    questo caso, quindi riconosciuto come sequenza letterale esatta
    `5;5u`, non parsato come CSI-u generico (che romperebbe su altri
    terminali con codifiche diverse). Nuovo `CTRL_I_KEY` in `enum
    editorKey`; su un terminale che non manda questa sequenza
    specifica, Ctrl-I resta indistinguibile dal Tab (limite
    intrinseco, non risolvibile lato editor). **Confermato
    dall'utente**: su Terminal.app di macOS, Ctrl-I digita
    effettivamente un Tab invece di aprire la schermata info — stesso
    limite già noto per Shift+Arrow su quel terminale (con fallback
    `Ctrl-T` per la selezione), qui senza un
    fallback equivalente possibile dato che non c'è modo di
    distinguere le due combinazioni via tastiera su quel terminale.
    Menzionato sia nella voce di F1 sia sotto.
  - **Contenuto della schermata** (`editorInfoScreen()`, overlay
    statico come F1, qualunque tasto chiude): sezione "tinyedit"
    (versione, autore con email, licenza con nota sulla parte BSD
    ereditata da linenoise, URL GitHub) e sezione "Current file"
    (percorso e stato modificato, filetype rilevato, numero righe,
    conteggio caratteri UTF-8-aware, posizione cursore, encoding,
    stato del backup automatico, quanti step di undo sono in uso sul
    massimo configurato).
  - Verificato via pty (`HOME` isolato): Tab continua a indentare
    normalmente (nessuna regressione); `ESC[5;5u` apre correttamente
    la schermata info con tutti i campi popolati e coerenti con lo
    stato reale del file di prova.

- [x] **Apertura e chiusura del file senza riavviare tinyedit (`Ctrl-O` / `Ctrl-W`)**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - `Ctrl-W` non è più un alias di uscita: propone save/discard/cancel se
    il documento è modificato, scarica il file corrente e lascia tinyedit
    aperto su un buffer vuoto senza nome.
  - `Ctrl-O` applica lo stesso controllo al documento corrente, poi chiede
    manualmente il percorso. Un file esistente viene caricato nella stessa
    sessione; un percorso inesistente apre un buffer vuoto già associato a
    quel nome e crea il file al primo `Ctrl-S`, come Vim.
  - Chiusura, apertura e `Ctrl-Q` condividono un'unica routine di conferma,
    così un salvataggio fallito o annullato interrompe sempre l'operazione
    senza perdere il documento. Al cambio file vengono azzerati cursore,
    selezione, ricerca, scroll, cronologia undo/redo e stato del backup.
  - Aggiornati la tabella dei tasti nel README e l'help interno F1. Questa
    voce sostituisce il precedente comportamento storico `Ctrl-W = Ctrl-Q`.

- [x] **Freeze di Replace all quando la sostituzione contiene il pattern cercato**
  - _Segnalato: 2026-09-04 · Corretto: 2026-09-04_
  - Riproduzione: ricerca regex di una sequenza letterale `\\n`, passaggio
    a sostituzione con `Ctrl-R`, `\n` come testo sostitutivo e scelta `a`
    (all).
  - **Causa**: il replace riutilizzava la ricerca circolare dell'interfaccia;
    dopo l'ultima occorrenza tornava alla prima e una sostituzione che
    continuava a soddisfare il pattern rendeva il ciclo infinito.
  - **Fix**: Replace all percorre ora il documento una sola volta, dall'inizio
    alla fine. Nel testo sostitutivo regex `\n`, `\t`, `\r` e `\\` vengono
    decodificati; una sostituzione può quindi dividere realmente una riga.
    Anche i match regex a lunghezza zero avanzano sempre di un carattere
    originale, e l'intera operazione crea un solo step di undo.
  - Aggiunto test PTY che riproduce esattamente la conversione di due sequenze
    `\\n` in a-capo, salva il file e ne verifica i byte risultanti.

- [x] **Bug: Backspace/Canc cancellavano un solo carattere invece dell'intera selezione**
  - _Segnalato: 2026-09-04 · Corretto: 2026-09-04_
  - Segnalato dall'utente: con del testo selezionato, `Backspace` e `Canc`
    rimuovevano un singolo carattere lasciando intatto il resto della
    selezione — mentre l'incolla (`Ctrl-V` e il bracketed paste) sostituiva
    correttamente l'intera selezione, rendendo l'incoerenza evidente.
  - **Causa**: il case `BACKSPACE`/`DEL_KEY` di `editorProcessKeypress()`
    chiamava direttamente `editorDelChar()`, che per definizione agisce su
    un carattere solo, senza mai interrogare lo stato di selezione — a
    differenza del case `PASTE_START_KEY`, che chiamava `editorDeleteRange()`
    sulla selezione prima di inserire.
  - **Fix**: il case ora usa `had_sel`/`had_sel_*`, la copia della selezione
    già catturata a inizio funzione *prima* del blocco che azzera
    `E.sel_active` (nessuno dei due tasti è nella whitelist di quel blocco,
    quindi rileggere `editorGetSelection()` dentro il case avrebbe restituito
    "nessuna selezione"). Con una selezione attiva delega a
    `editorDeleteRange()`, che gestisce già il caso multi-riga, posiziona il
    cursore e crea un solo snapshot di undo; senza selezione il
    comportamento precedente resta invariato, incluso l'`ARROW_RIGHT`
    preliminare che distingue `Canc` da `Backspace`.
  - Verificato via pty (`HOME` isolato, `TIOCSWINSZ` esplicito perché senza
    dimensioni di finestra `getWindowSize()` fallisce e l'editor esce
    subito): selezione di 5 caratteri + `Backspace` e + `Canc` lasciano
    entrambe `" world"` da `"hello world"`; selezione multi-riga +
    `Backspace` fonde correttamente le righe; senza selezione `Backspace`
    continua a cancellare un solo carattere (`"hello"` → `"helo"`).

## Note tecniche aperte

- [x] **Schermata Info spostata da Ctrl-I a F3**
  - _Inserito: 2026-09-04 · Completato: 2026-09-04_
  - `Ctrl-I` non è un binding portabile: condivide il byte ASCII del Tab,
    Terminal.app lo tratta come Tab e Ghostty può lasciare visibile la
    coda `5;5u`. La schermata Info usa ora F3, riconoscendo sia la forma
    SS3 `ESC O R` sia la forma CSI `ESC[13~`, senza conflitti con Ctrl-T.

- Ctrl-C/X/V nell'editor non confliggono con le scorciatoie di sistema:
  su macOS il sistema usa Cmd+C/V (Ctrl-C è libero in raw mode), su
  Linux i terminali grafici tipicamente usano Ctrl-Shift-C/V per non
  rompere la convenzione storica di Ctrl-C come SIGINT.
