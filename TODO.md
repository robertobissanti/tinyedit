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
