# Analisi della decomposizione dello stato di TinyEdit

Data: 2026-09-09  
Oggetto: censimento funzione per funzione delle dipendenze da `E`, `S` e dagli
altri globali mutabili di `tinyedit.c`.

## Decisione proposta

Mantenere una sola radice editoriale `editorConfig E`, composta da strutture più fini.
Le funzioni ricevono la struttura più piccola che rappresenta davvero il loro
contratto; quando una funzione attraversa tre o più aree principali riceve
direttamente `editorConfig *`.

Per la fase corrente `S` resta deliberatamente globale, come deciso dopo
l'analisi. Potrà essere rivalutata separatamente senza bloccare la
decomposizione di `E`.

## Struttura risultante

```c
struct editorBuffer {
    erow *rows;
    int32_t row_count;
};

struct editorCursor {
    int32_t cx, cy;
    int32_t rx;
};

struct editorSelection {
    uint8_t active;
    int32_t anchor_x, anchor_y;
    uint8_t pinned;
};

struct editorHistory {
    undoSnapshot *undo_stack;
    int32_t undo_count;
    undoSnapshot *redo_stack;
    int32_t redo_count;
    enum undoEditType last_edit_type;
    time_t last_edit_time;
};

struct editorFileState {
    char *filename;
    uint8_t dirty;
    enum lineEndingMode detected_line_ending;
    uint8_t line_endings_mixed;
    time_t last_backup_time;
};

struct editorDocument {
    struct editorBuffer buffer;
    struct editorCursor cursor;
    struct editorSelection selection;
    struct editorHistory history;
    struct editorFileState file;
};

struct editorView {
    int32_t rowoff, coloff;
    int32_t screenrows, screencols;
    uint8_t free_scroll;
};

struct editorUi {
    char statusmsg[512];
    time_t statusmsg_time;
    uint8_t statusmsg_sticky;
    const char *session_slogan;
};

struct editorSearch {
    int32_t match_y, match_x, match_len;
    int32_t saved_cx, saved_cy;
    int32_t saved_rowoff, saved_coloff;
    int32_t direction;
    uint8_t regex_mode;
    uint8_t switch_to_replace;
};

struct editorInput {
    struct termios orig_termios;
    int32_t mouse_button;
    int32_t mouse_col, mouse_row;
    uint8_t mouse_press;
    int32_t pending_key;
};

struct editorConfig {
    struct editorDocument document;
    struct editorView view;
    struct editorUi ui;
    struct editorSearch search;
};
```

Le quattro aree della radice sono quindi `document`, `view`, `ui` e `search`.
Lo stato del terminale è incapsulato nel relativo modulo e `S` resta globale.
`document` è ulteriormente scomposto perché le sue primitive
sono quelle che beneficiano maggiormente di test C diretti.

`dirty` rimane in `editorFileState`, non nel buffer: indica che il documento
deve essere salvato, non che una particolare riga è mutabile. Le primitive che
effettuano una modifica completa ricevono quindi `editorDocument *`; le
primitive meccaniche che lavorano su una `erow` non devono impostare `dirty`.
Questo elimina l'effetto collaterale nascosto oggi presente in
`editorRowInsertChar()`, `editorRowAppendString()` e `editorRowDelChar()`.

## Regola per scegliere il parametro

- Una sola area: puntatore a quella struttura.
- Due aree realmente necessarie: due puntatori, oppure il padre naturale se
  vengono quasi sempre usate insieme.
- Tre o più aree principali: `editorConfig *`.
- Funzione pura o già parametrizzata completamente: nessun puntatore di stato.
- Configurazione in sola lettura: puntatore `const`.
- Le funzioni di orchestrazione possono ricevere sempre `editorConfig *`.

## Censimento: terminale e input

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `die` | ripristino terminale prima dell'uscita | `editorInput *` |
| `disableRawMode` | `orig_termios` | `editorInput *` |
| `restoreTerminalVisualState` | nessuno stato C | nessuno |
| `enableRawMode` | `orig_termios` | `editorInput *` |
| `disableBracketedPaste` | nessuno | nessuno |
| `enableBracketedPaste` | nessuno | nessuno |
| `stdinHasDataReady` | nessuno; il riferimento al mouse è solo documentale | nessuno |
| `disableMouseReporting` | nessuno | nessuno |
| `enableMouseReporting` | nessuno | nessuno |
| `handleWinch` | flag di resize, se introdotto | `editorInput *` o nessuno oggi |
| `enableResizeHandling` | nessuno | nessuno |
| `editorDrainUnknownCsiSequence` | stdin | nessuno |
| `editorReadKey` | pending key, evento mouse, setting Cmd | `editorInput *`, `const editorSettings *` |
| `editorReadPastedText` | stdin | nessuno |
| `getCursorPosition` | terminale I/O | nessuno |
| `getWindowSize` | terminale I/O | nessuno |
| `editorReadMultiByteKey` | usa il decoder di input | `editorInput *`, `const editorSettings *` |

`editorReadKey()` e `editorReadMultiByteKey()` potrebbero ricevere due
puntatori, ma è più pulito far dipendere `editorInput` dalla sola opzione
`mac_command_keys` copiata/configurata all'avvio. Se si evita questa copia, due
puntatori sono ancora accettabili: sono solo due aree e il contratto resta
chiaro.

## Censimento: righe, buffer e rendering della riga

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorRowCxToRx` | riga + `tab_stop` | `erow *`, `const editorSettings *` |
| `editorRowRxToCx` | riga + `tab_stop` | `erow *`, `const editorSettings *` |
| `editorRehighlightFrom` | buffer, filename, syntax settings | `editorConfig *` |
| `editorUpdateRow` | buffer circostante, filename, syntax/render settings | `editorConfig *`, `erow *` |
| `editorUpdateAllRows` | buffer | `editorConfig *` perché richiama update contestuale |
| `editorInsertRow` | buffer | `editorBuffer *` |
| `editorFreeRow` | una riga | `erow *` già sufficiente |
| `editorDelRow` | buffer | `editorBuffer *` |
| `editorRowInsertChar` | una riga | `erow *`; nessun `dirty` implicito |
| `editorRowAppendString` | una riga | `erow *`; nessun `dirty` implicito |
| `editorRowDelChar` | una riga | `erow *`; nessun `dirty` implicito |
| `editorClearRows` | buffer | `editorBuffer *` |
| `editorLoadLines` | buffer + line-ending file state | `editorDocument *` |
| `editorRowsToString` | buffer + line-ending policy | `editorBuffer *`, `lineEndingMode` |

Il punto importante è separare la mutazione meccanica della riga dal commit
dell'azione utente. Il livello `editorDocument` imposta `dirty`, aggiorna
rendering/sintassi e registra history una sola volta.

## Censimento: history e operazioni di editing

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorMakeSnapshot` | buffer + cursor | `const editorDocument *` |
| `editorFreeSnapshot` | snapshot ricevuto | nessuno stato |
| `editorClearRedoStack` | history | `editorHistory *` |
| `editorPushUndo` | history + snapshot del documento + limite setting | `editorDocument *`, `int32_t max_depth` |
| `editorRestoreSnapshot` | buffer + cursor + dirty + rendering | `editorConfig *` |
| `editorUndo` | document + rendering | `editorConfig *` |
| `editorRedo` | document + rendering | `editorConfig *` |
| `editorInsertCharRaw` | buffer + cursor | `editorDocument *` |
| `editorInsertChar` | buffer + cursor + history + dirty | `editorDocument *` |
| `editorInsertNewlineRaw` | buffer + cursor | `editorDocument *` |
| `editorInsertNewlineAutoIndent` | document + settings | `editorDocument *`, `const editorSettings *` |
| `editorDelChar` | document/history | `editorDocument *` |
| `editorSelectionRange` | selection + cursor | due puntatori `const` (implementato e testato direttamente) |
| `editorSerializeRange` | buffer | `const editorBuffer *` |
| `editorDeleteRangeRaw` | buffer + cursor + dirty | `editorDocument *` |
| `editorDeleteRange` | document/history | `editorDocument *` |
| `editorInsertTextRaw` | buffer + cursor | `editorDocument *` |
| `editorReplaceSelectionWithText` | document/history/selection | `editorDocument *` |
| `editorRowOutdent` | riga + `tab_stop` | `erow *`, `int32_t tab_stop` |
| `editorIndentSelection` | document + indentation settings | `editorDocument *`, `const editorSettings *` |
| `editorAutoCloseFor` | pair settings | `const editorSettings *` |
| `editorTrySkipMultiByteClose` | document + pair settings | `editorDocument *`, `const editorSettings *` |
| `editorInsertCharAutoClose` | document + pair settings | `editorDocument *`, `const editorSettings *` |
| `editorFreeUndoRedo` | history | `editorHistory *` |

Qui il padre naturale è `editorDocument`: passare separatamente buffer,
cursor, selection e history produrrebbe esattamente il problema indicato
dall'utente. Le primitive puramente meccaniche possono scendere a
`editorBuffer *` o `erow *`; le azioni dell'editor ricevono il documento.

## Censimento: file e ciclo di vita del documento

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorEffectiveLineEnding` | file state + setting | `const editorFileState *`, `const editorSettings *` |
| `editorResolveFiletype` | filename + settings persistenti | `editorConfig *` |
| `editorWarnMissingHighlightConfig` | filename + UI status | `editorConfig *` |
| `editorOpen` | intero documento, settings, UI | `editorConfig *` |
| `editorMaybeBackup` | document/file + UI + setting | `editorConfig *` |
| `editorOfferBackupRecovery` | document/file + UI | `editorConfig *` |
| `editorWriteAll` | fd e byte buffer | nessuno stato |
| `editorAtomicSave` | argomenti completi; usa filename solo per metadata | rendere esplicito l'argomento, nessuno stato |
| `editorSaveInternal` | document/file + UI + settings | `editorConfig *` |
| `editorSave` | orchestrazione | `editorConfig *` |
| `editorSaveAs` | orchestrazione | `editorConfig *` |
| `editorDiffersFromDisk` | buffer + file state + line ending | `const editorConfig *` oppure helper con argomenti espliciti |
| `editorConfirmDocumentChange` | document + UI + input | `editorConfig *` |
| `editorResetDocument` | document, view, search, UI parziale | `editorConfig *` |
| `editorCloseFile` | orchestrazione | `editorConfig *` |
| `editorOpenFile` | orchestrazione | `editorConfig *` |
| `editorQuit` | orchestrazione/terminale/file | `editorConfig *` |

Queste funzioni non vanno artificialmente ristrette: il lifecycle del file è
per natura trasversale e ricevere `editorConfig *` rende il contratto più
onesto di quattro puntatori separati.

## Censimento: viewport, coordinate e rendering

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorGutterWidth` | buffer + setting | `const editorBuffer *`, `const editorSettings *` |
| `editorTextCols` | view + setting | `const editorView *`, `const editorSettings *` |
| `editorSoftWrapCols` | view + setting | `const editorView *`, `const editorSettings *` |
| `editorRowSegments` | riga/cache wrap | `erow *`, `int32_t wrapcols` già sufficiente |
| `editorSegVisibleEnd` | riga e segmenti | argomenti già sufficienti |
| `editorSegVisibleEndRx` | riga e segmenti | argomenti già sufficienti |
| `editorRxToSegment` | riga e coordinate | argomenti già sufficienti |
| `editorRowVideoHeight` | buffer + wrap | `editorBuffer *`, `int32_t wrapcols` |
| `editorVideoRowOf` | buffer + wrap | `editorBuffer *`, `int32_t wrapcols` |
| `editorFileRowAtVideoRow` | buffer + wrap | `editorBuffer *`, `int32_t wrapcols` |
| `editorTotalVideoRows` | buffer + wrap | `editorBuffer *`, `int32_t wrapcols` |
| `editorMouseToCursor` | document + view + settings | `editorConfig *` |
| `editorScroll` | document + view + settings | `editorConfig *` |
| `editorSegColToCx` | riga + tab setting | `erow *`, `int32_t tab_stop` |
| `editorMoveCursorWrapped` | document + view/settings | `editorConfig *` |
| `editorMoveCursor` | document + view/settings | `editorConfig *` |
| `editorMoveCursorWord` | document | `editorDocument *` |
| `abAppend` | append buffer ricevuto | nessuno stato |
| `abFree` | append buffer ricevuto | nessuno stato |
| `abAppendReset` | background setting | `abuf *`, `const editorSettings *` |
| `editorDrawRowSegment` | document, selection, search, settings | `const editorConfig *` |
| `editorDrawGutter` | buffer + settings | `const editorBuffer *`, `const editorSettings *` |
| `editorChooseSlogan` | UI | `editorUi *` |
| `editorDrawSplashRow` | view + UI/settings | `const editorConfig *` |
| `editorDrawRows` | document, view, search, settings | `const editorConfig *` |
| `editorCountChars` | buffer | `const editorBuffer *` |
| `editorFiletypeLabel` | file state | `const editorFileState *` |
| `editorDrawTopBar` | document/file, view, settings | `const editorConfig *` |
| `editorDrawStatusBar` | document/file, view, settings | `const editorConfig *` |
| `editorDrawMessageBar` | UI + view | `const editorUi *`, `const editorView *` |
| `editorRefreshScreen` | quasi tutte le aree | `editorConfig *` |
| `editorSetStatusMessage` | UI | `editorUi *` |
| `editorSetStatusMessageSticky` | UI | `editorUi *` |

Le funzioni di mapping geometrico diventano ottimi candidati per test unitari
quando ricevono buffer/view esplicitamente. Il frame completo resta invece
correttamente una funzione sullo stato intero.

## Censimento: prompt, ricerca e replace

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorPromptDisplayText` | solo buffer del prompt | nessuno stato |
| `editorPromptAppend` | solo buffer del prompt | nessuno stato |
| `editorPromptCB` | UI, view, input, clipboard | `editorConfig *` |
| `editorPrompt` | prompt + file state | `editorConfig *` |
| `editorDecodeRegexReplacement` | argomento | nessuno stato |
| `editorDecodeRegexPattern` | argomento | nessuno stato |
| `editorRegexFindLastOnRow` | riga e limite | eliminare dipendenze residue; argomenti espliciti |
| `editorFindFrom` | document + search | `editorDocument *`, `editorSearch *` |
| `editorFindCallback` | document + view + search | `editorConfig *` |
| `editorFindModeIndicator` | search | `const editorSearch *` |
| `editorFind` | document + view + search + UI/input | `editorConfig *` |
| `editorFindAndReplace` | document + search + UI/input/history | `editorConfig *` |

Gli attuali globali `search_*` devono entrare tutti in `editorSearch`; non ci
sono motivi di ciclo di vita per lasciarli separati da `E`.

## Censimento: help, info e impostazioni

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorPrimaryModifier` | settings | `const editorSettings *` |
| `editorShortcutText` | settings | `const editorSettings *` |
| `editorRecoveryScreenLine` | view | `const editorView *` |
| `editorRecoveryScreen` | document/file + view + input | `editorConfig *` |
| `settingsScreenSlot` | settings ricevute | nessun `E` |
| `editorSettingsIsSyntaxColor` | descrittore | nessuno stato |
| `editorSettingsIsColor` | descrittore | nessuno stato |
| `editorSettingsVisibleCount` | settings ricevute | nessun `E` |
| `editorSettingsDescriptorAt` | settings ricevute | nessun `E` |
| `editorSettingsCycleEnum` | argomenti completi | nessuno stato |
| `editorSettingsDrawRow` | argomenti completi | nessuno stato |
| `editorHelpScreen` | view + settings + input | `editorConfig *` |
| `editorInfoAppendLine` | append buffer | nessuno stato |
| `editorInfoAppendSection` | append buffer | nessuno stato |
| `editorInfoAppendBlank` | append buffer | nessuno stato |
| `editorInfoScreen` | document, view, settings, input | `editorConfig *` |
| `editorSettingsVisibleRows` | view | `const editorView *` |
| `editorSettingsRender` | view + settings temporanee | `const editorView *`, settings ricevute |
| `editorSettingsEditInt` | input/UI + settings temporanee | `editorConfig *`, settings temporanee |
| `editorSettingsSave` | settings vecchie/nuove + document/render/input | `editorConfig *`, settings temporanee |
| `editorSettingsScreen` | settings, UI, view, input | `editorConfig *` |

## Censimento: orchestrazione

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorProcessKeypress` | document, view, selection, history, input, settings, UI | `editorConfig *` |
| `initEditor` | inizializza ogni area | `editorConfig *` |
| `main` | possiede il ciclo di vita | crea `editorConfig E`; può restare locale a `main` |

`editorProcessKeypress()` è esattamente il caso in cui passare `E` è giusto:
smontarlo in molti puntatori renderebbe la firma peggiore senza ridurre
l'accoppiamento. Il disaccoppiamento del dispatch avverrà estraendo handler
più piccoli, ai quali passare `editorDocument *`, `editorSearch *` o lo stato
minimo appropriato.

## Globali assorbite e scelte deliberate

Assorbite in `E`:

- tutti i `search_*` → `E.search`;

Incapsulate nel modulo terminale:

- `mouseEventButton/Col/Row/Press`, `pending_key` e `orig_termios`.

Lasciate globali in questa fase:

- `S`, per decisione esplicita;
- `sessionSlogan`, finché non viene separato il rendering.

Da lasciare globali `static const` perché sono dati immutabili, non stato:

- slogan disponibili;
- tabelle help;
- tabelle delle coppie auto-close;
- descrittori e tabelle di configurazione/sintassi.

I flag di segnale devono restare `volatile sig_atomic_t` globali se il signal
handler deve scriverli: è un vincolo POSIX, non accoppiamento evitabile.

## Moduli suggeriti dopo il censimento

### `buffer.c/.h`

Contiene `editorBuffer`, gestione `erow`, serializzazione, insert/delete raw e
funzioni geometriche che dipendono soltanto dalla riga. Non conosce terminale,
UI, ricerca o settings globali.

### `history.c/.h`

Contiene `editorHistory` e le operazioni sugli step. Nella prima estrazione può
ancora usare snapshot completi; il passaggio ai delta è un refactoring
successivo e separato.

### `terminal.c/.h`

Completato: contiene il proprio stato interno, raw mode, decoder dei tasti,
bracketed paste, mouse e dimensioni terminale. Le sequenze restano testate via
PTY.

### `render.c/.h`

Contiene `editorView`, mapping visuale e rendering. Le funzioni di frame
ricevono `editorConfig *`; le funzioni geometriche più fini ricevono riga,
buffer o view.

### `tinyedit.c`

Possiede `editorConfig`, lifecycle del documento, ricerca, overlay e dispatch.
Ricerca e overlay potranno essere estratti solo dopo che buffer, terminale e
rendering hanno API stabili.

## Stato della sequenza operativa

1. Strutture annidate in `tinyedit.h`: completato.
2. Globali di ricerca assorbite; `S` mantenuta globale: completato.
3. Accessi aggiornati e suite verde: completato.
4. Prime primitive parametrizzate per selection/document/history: completato.
5. `editorSelectionRange` estratta e testata senza PTY: completato.
6. Estrazione `buffer.c/.h` e `history.c/.h`: lavoro successivo.
7. Estrazione `render.c/.h`: dopo test diretti sul mapping UTF-8.

Ogni passaggio deve essere un commit autonomo e prevalentemente meccanico. Non
va combinato nello stesso commit con il nuovo modello di undo o con cambi di
semantica dell'editor.

## Conclusione

Il censimento non giustifica il passaggio indiscriminato di `&E`. La
decomposizione produce tre categorie nette:

- primitive pure: nessuno stato globale;
- operazioni sul documento: `editorDocument *` o una sua sottostruttura;
- rendering, lifecycle, ricerca interattiva e dispatch: `editorConfig *`.

Questa gerarchia mantiene la comodità di una sola `E`, evita firme con molti
puntatori e rende testabili direttamente proprio le funzioni che oggi
richiedono inutilmente un PTY.
