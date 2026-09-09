# Analisi della decomposizione dello stato di TinyEdit

Data: 2026-09-09  
Oggetto: censimento funzione per funzione delle dipendenze da `E`, `S` e dagli
altri globali mutabili di `tinyedit.c`.

## Decisione proposta

Mantenere una sola radice `editorState E`, composta da strutture più fini.
Le funzioni ricevono la struttura più piccola che rappresenta davvero il loro
contratto; quando una funzione attraversa tre o più aree principali riceve
direttamente `editorState *`.

Non conviene mantenere `S` come seconda globale indipendente: diventa
`E.settings`. In questo modo l'applicazione ha una sola radice di stato, ma una
funzione che usa soltanto le impostazioni può comunque ricevere
`const struct editorSettings *`.

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

struct editorState {
    struct editorDocument document;
    struct editorView view;
    struct editorUi ui;
    struct editorSearch search;
    struct editorInput input;
    struct editorSettings settings;
};
```

Le sei aree principali sono quindi `document`, `view`, `ui`, `search`, `input`
e `settings`. `document` è ulteriormente scomposto perché le sue primitive
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
- Tre o più aree principali: `editorState *`.
- Funzione pura o già parametrizzata completamente: nessun puntatore di stato.
- Configurazione in sola lettura: puntatore `const`.
- Le funzioni di orchestrazione possono ricevere sempre `editorState *`.

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
| `editorRehighlightFrom` | buffer, filename, syntax settings | `editorState *` |
| `editorUpdateRow` | buffer circostante, filename, syntax/render settings | `editorState *`, `erow *` |
| `editorUpdateAllRows` | buffer | `editorState *` perché richiama update contestuale |
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
| `editorRestoreSnapshot` | buffer + cursor + dirty + rendering | `editorState *` |
| `editorUndo` | document + rendering | `editorState *` |
| `editorRedo` | document + rendering | `editorState *` |
| `editorInsertCharRaw` | buffer + cursor | `editorDocument *` |
| `editorInsertChar` | buffer + cursor + history + dirty | `editorDocument *` |
| `editorInsertNewlineRaw` | buffer + cursor | `editorDocument *` |
| `editorInsertNewlineAutoIndent` | document + settings | `editorDocument *`, `const editorSettings *` |
| `editorDelChar` | document/history | `editorDocument *` |
| `editorGetSelection` | selection + cursor | `const editorDocument *` |
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
| `editorResolveFiletype` | filename + settings persistenti | `editorState *` |
| `editorWarnMissingHighlightConfig` | filename + UI status | `editorState *` |
| `editorOpen` | intero documento, settings, UI | `editorState *` |
| `editorMaybeBackup` | document/file + UI + setting | `editorState *` |
| `editorOfferBackupRecovery` | document/file + UI | `editorState *` |
| `editorWriteAll` | fd e byte buffer | nessuno stato |
| `editorAtomicSave` | argomenti completi; usa filename solo per metadata | rendere esplicito l'argomento, nessuno stato |
| `editorSaveInternal` | document/file + UI + settings | `editorState *` |
| `editorSave` | orchestrazione | `editorState *` |
| `editorSaveAs` | orchestrazione | `editorState *` |
| `editorDiffersFromDisk` | buffer + file state + line ending | `const editorState *` oppure helper con argomenti espliciti |
| `editorConfirmDocumentChange` | document + UI + input | `editorState *` |
| `editorResetDocument` | document, view, search, UI parziale | `editorState *` |
| `editorCloseFile` | orchestrazione | `editorState *` |
| `editorOpenFile` | orchestrazione | `editorState *` |
| `editorQuit` | orchestrazione/terminale/file | `editorState *` |

Queste funzioni non vanno artificialmente ristrette: il lifecycle del file è
per natura trasversale e ricevere `editorState *` rende il contratto più
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
| `editorMouseToCursor` | document + view + settings | `editorState *` |
| `editorScroll` | document + view + settings | `editorState *` |
| `editorSegColToCx` | riga + tab setting | `erow *`, `int32_t tab_stop` |
| `editorMoveCursorWrapped` | document + view/settings | `editorState *` |
| `editorMoveCursor` | document + view/settings | `editorState *` |
| `editorMoveCursorWord` | document | `editorDocument *` |
| `abAppend` | append buffer ricevuto | nessuno stato |
| `abFree` | append buffer ricevuto | nessuno stato |
| `abAppendReset` | background setting | `abuf *`, `const editorSettings *` |
| `editorDrawRowSegment` | document, selection, search, settings | `const editorState *` |
| `editorDrawGutter` | buffer + settings | `const editorBuffer *`, `const editorSettings *` |
| `editorChooseSlogan` | UI | `editorUi *` |
| `editorDrawSplashRow` | view + UI/settings | `const editorState *` |
| `editorDrawRows` | document, view, search, settings | `const editorState *` |
| `editorCountChars` | buffer | `const editorBuffer *` |
| `editorFiletypeLabel` | file state | `const editorFileState *` |
| `editorDrawTopBar` | document/file, view, settings | `const editorState *` |
| `editorDrawStatusBar` | document/file, view, settings | `const editorState *` |
| `editorDrawMessageBar` | UI + view | `const editorUi *`, `const editorView *` |
| `editorRefreshScreen` | quasi tutte le aree | `editorState *` |
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
| `editorPromptCB` | UI, view, input, clipboard | `editorState *` |
| `editorPrompt` | prompt + file state | `editorState *` |
| `editorDecodeRegexReplacement` | argomento | nessuno stato |
| `editorDecodeRegexPattern` | argomento | nessuno stato |
| `editorRegexFindLastOnRow` | riga e limite | eliminare dipendenze residue; argomenti espliciti |
| `editorFindFrom` | document + search | `editorDocument *`, `editorSearch *` |
| `editorFindCallback` | document + view + search | `editorState *` |
| `editorFindModeIndicator` | search | `const editorSearch *` |
| `editorFind` | document + view + search + UI/input | `editorState *` |
| `editorFindAndReplace` | document + search + UI/input/history | `editorState *` |

Gli attuali globali `search_*` devono entrare tutti in `editorSearch`; non ci
sono motivi di ciclo di vita per lasciarli separati da `E`.

## Censimento: help, info e impostazioni

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorPrimaryModifier` | settings | `const editorSettings *` |
| `editorShortcutText` | settings | `const editorSettings *` |
| `editorRecoveryScreenLine` | view | `const editorView *` |
| `editorRecoveryScreen` | document/file + view + input | `editorState *` |
| `settingsScreenSlot` | settings ricevute | nessun `E` |
| `editorSettingsIsSyntaxColor` | descrittore | nessuno stato |
| `editorSettingsIsColor` | descrittore | nessuno stato |
| `editorSettingsVisibleCount` | settings ricevute | nessun `E` |
| `editorSettingsDescriptorAt` | settings ricevute | nessun `E` |
| `editorSettingsCycleEnum` | argomenti completi | nessuno stato |
| `editorSettingsDrawRow` | argomenti completi | nessuno stato |
| `editorHelpScreen` | view + settings + input | `editorState *` |
| `editorInfoAppendLine` | append buffer | nessuno stato |
| `editorInfoAppendSection` | append buffer | nessuno stato |
| `editorInfoAppendBlank` | append buffer | nessuno stato |
| `editorInfoScreen` | document, view, settings, input | `editorState *` |
| `editorSettingsVisibleRows` | view | `const editorView *` |
| `editorSettingsRender` | view + settings temporanee | `const editorView *`, settings ricevute |
| `editorSettingsEditInt` | input/UI + settings temporanee | `editorState *`, settings temporanee |
| `editorSettingsSave` | settings vecchie/nuove + document/render/input | `editorState *`, settings temporanee |
| `editorSettingsScreen` | settings, UI, view, input | `editorState *` |

## Censimento: orchestrazione

| Funzione | Stato effettivo | Parametro consigliato |
|---|---|---|
| `editorProcessKeypress` | document, view, selection, history, input, settings, UI | `editorState *` |
| `initEditor` | inizializza ogni area | `editorState *` |
| `main` | possiede il ciclo di vita | crea `editorState E`; può restare locale a `main` |

`editorProcessKeypress()` è esattamente il caso in cui passare `E` è giusto:
smontarlo in molti puntatori renderebbe la firma peggiore senza ridurre
l'accoppiamento. Il disaccoppiamento del dispatch avverrà estraendo handler
più piccoli, ai quali passare `editorDocument *`, `editorSearch *` o lo stato
minimo appropriato.

## Globali da assorbire o lasciare costanti

Da assorbire in `E`:

- tutti i `search_*` → `E.search`;
- `mouseEventButton/Col/Row/Press` e `pending_key` → `E.input`;
- `sessionSlogan` → `E.ui`;
- `S` → `E.settings`;
- `orig_termios`, già dentro l'attuale `E`, → `E.input`.

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

Contiene `editorInput`, raw mode, decoder dei tasti, bracketed paste, mouse e
dimensioni terminale. Le sequenze terminali restano testate via PTY.

### `render.c/.h`

Contiene `editorView`, mapping visuale e rendering. Le funzioni di frame
ricevono `editorState *`; le funzioni geometriche più fini ricevono riga,
buffer o view.

### `tinyedit.c`

Possiede `editorState`, lifecycle del documento, ricerca, overlay e dispatch.
Ricerca e overlay potranno essere estratti solo dopo che buffer, terminale e
rendering hanno API stabili.

## Ordine operativo raccomandato

1. Definire le strutture annidate in `tinyedit.h` senza spostare funzioni.
2. Convertire `S` e gli altri globali mutabili in membri di `editorState`.
3. Aggiornare gli accessi meccanicamente, mantenendo `make test` verde.
4. Parametrizzare prima le primitive pure di riga/buffer.
5. Estrarre `buffer.c/.h`, poi `history.c/.h`.
6. Estrarre `terminal.c/.h` preservando i test PTY.
7. Estrarre `render.c/.h` dopo aver aggiunto test C diretti sul mapping UTF-8.
8. Solo a confini stabilizzati, rifattorizzare il dispatch della selezione.

Ogni passaggio deve essere un commit autonomo e prevalentemente meccanico. Non
va combinato nello stesso commit con il nuovo modello di undo o con cambi di
semantica dell'editor.

## Conclusione

Il censimento non giustifica il passaggio indiscriminato di `&E`. La
decomposizione produce tre categorie nette:

- primitive pure: nessuno stato globale;
- operazioni sul documento: `editorDocument *` o una sua sottostruttura;
- rendering, lifecycle, ricerca interattiva e dispatch: `editorState *`.

Questa gerarchia mantiene la comodità di una sola `E`, evita firme con molti
puntatori e rende testabili direttamente proprio le funzioni che oggi
richiedono inutilmente un PTY.
