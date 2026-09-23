# Audit di refactoring di TinyEdit

Data: 2026-09-09  
Ambito: codice C compilato, test, documentazione tecnica e ultimi 10 commit.

## Esito sintetico

Il primo ciclo di refactoring approvato è completato sul branch
`codex/refactor-editor-architecture`. Il terminale è ora un modulo autonomo,
le allocazioni applicative sono controllate uniformemente, il dispatch non usa
più una whitelist globale per invalidare la selezione e `E` è composto da
stati più fini. La prima logica estratta in `editor_state.c` è verificata da
test C diretti, senza PTY. `S` resta intenzionalmente globale in questa fase.

Il refactoring consigliato va eseguito per passi piccoli, mantenendo C99,
POSIX, zero dipendenze esterne, tipi espliciti e funzioni UTF-8 di `utf8.c`,
come richiesto da `CLAUDE.md`.

## Evidenze raccolte

- Build completa con i warning rigorosi già usati dai test.
- Suite `make test`: test syntax, settings/backup e PTY superati.
- `terminal.c/.h`: raw mode, resize, input, mouse e bracketed paste estratti
  da `tinyedit.c`.
- `editor_state.c/.h`: normalizzazione della selezione pura e testabile senza
  terminale.
- `tinyedit.c` resta ancora ampio e continua a contenere buffer, history, file
  I/O, rendering, ricerca, overlay e orchestrazione.
- `syntax.c`: circa 1.500 righe; è già un modulo separato ma combina loading
  delle configurazioni, lookup dei linguaggi e più tokenizer.
- Le allocazioni applicative passano da `teMalloc`, `teRealloc` e `teStrdup`;
  il fallimento è sempre fatale e il cleanup registrato ripristina il terminale.
- La build rigorosa segnala cinque `-Wformat-nonliteral`; quattro sono wrapper
  variadici intenzionali, uno è il prompt dinamico. Non sono errori osservati,
  ma impediscono una policy realmente warning-clean.
- `linenoise.c` contiene tipi nel `.c`, in contrasto con la convenzione
  attuale, ma è sorgente storico non compilato: non conviene rifattorizzarlo.

## Findings prioritizzati

### P0 — Nessun problema bloccante residuo rilevato

Il bug segnalato è stato corretto e coperto da test. Non sono emerse altre
regressioni che richiedano una correzione immediata prima dell'uso normale.

### Risolto — Dispatch e ciclo di vita della selezione

La whitelist precedente allo switch è stata eliminata. Ogni handler ora
preserva, estende, consuma o cancella esplicitamente la selezione. Sono coperti
paste, redraw/Ctrl-L, Tab, Shift-Tab e selezione pinned a larghezza zero.

La semantica residua da decidere come scelta di prodotto è se caratteri normali
e Invio debbano sostituire una selezione come negli editor comuni.

### P1 — Undo basato su snapshot completi

Ogni snapshot copia tutte le righe e tutto il testo. Con
`undo_max_depth = 2000`, file grandi e digitazione non perfettamente
coalescente, tempo e memoria crescono come `dimensione_file × step_undo`.
Inoltre ogni `realloc` della pila può spostare l'intero array di snapshot.

Refactoring proposto: mantenere l'interfaccia utente invariata ma rappresentare
gli step come comandi/delta (`insert range`, `delete range`, `replace range`),
memorizzando solo testo e coordinate necessari all'inversione. È il lavoro a
maggiore rendimento prestazionale, ma anche quello con più rischio: va fatto
dopo aver ampliato i test di undo/redo su UTF-8, multilinea, indentazione,
replace-all e cambio file.

### P1 — Mutazioni di range con complessità evitabile

La cancellazione monoriga chiama `editorRowDelChar()` per ogni byte selezionato;
ciascuna chiamata esegue `memmove`, ridimensionamento e aggiornamento della
riga. Anche il bulk insert procede byte per byte. Su selezioni o paste grandi
questo moltiplica reallocazioni e ricalcoli.

Refactoring proposto: primitive di range che eseguano un solo `memmove` e un
solo `realloc` per riga, più un solo `editorUpdateRow()` per riga modificata.
Il livello pubblico continua a creare un unico step di undo; le primitive
`Raw` introdotte dalla correzione costituiscono già il confine giusto.

### Risolto — Gestione uniforme degli errori di allocazione

`alloc.c/.h` centralizza la policy fail-fast. `CLAUDE.md` rende obbligatorio
usare gli helper controllati e vieta le allocazioni applicative dirette.

### Parzialmente risolto — Separare `tinyedit.c` per responsabilità

La convenzione “un modulo per responsabilità” è rispettata per clipboard,
UTF-8, settings, backup e syntax, ma non più per il core cresciuto. Una
separazione incrementale consigliata è:

1. `terminal.c/.h`: completato.
2. `editor_state.c/.h`: avviato con la logica pura della selezione.
3. `buffer.c/.h`: residuo.
4. `history.c/.h`: residuo.
4. `render.c/.h`: wrap, mapping coordinate, barre e frame principale.
5. `tinyedit.c`: orchestrazione, file lifecycle e dispatch ad alto livello.

Non conviene estrarre subito gli overlay F1/F2/F3: dipendono ancora molto da
rendering e settings e produrrebbero API larghe. Vanno spostati solo dopo aver
stabilizzato `render.c`.

### Parzialmente risolto — Stato globale e dipendenze implicite

`E` contiene ora `document`, `view`, `ui` e `search`; le vecchie globali di
ricerca sono state assorbite in `E.search`. Le primitive migrate ricevono
`editorSelection` + `editorCursor`, `editorHistory` o `editorDocument` secondo
il loro uso effettivo. `S` resta globale per decisione esplicita. Il lavoro
residuo è estendere questo schema alle primitive buffer/history ancora interne
a `tinyedit.c`.

### P2 — Warning rigorosi non completamente puliti

I wrapper di formattazione sono intenzionali, ma il compilatore non può
validarne il contratto. Aggiungere attributi `format(printf, ...)` protetti da
macro portabile per GCC/Clang renderebbe verificabili le chiamate ai wrapper.
Per `editorPromptCB`, evitare un format arbitrario e passare dati strutturati
al renderer del prompt eliminerebbe il warning senza soppressioni.

### P3 — Documentazione molto dettagliata ma duplicata

README, TODO e commenti nel sorgente ripetono spesso le stesse decisioni e
alcuni commenti storici sono già diventati obsoleti durante questa correzione.
Tenere nel TODO il diario e nel codice solo invarianti/non-ovvietà; nel README
solo comportamento utente. Un controllo leggero in review sui nomi di
funzione citati nei documenti ridurrebbe il drift.

## Piano consigliato

### Fase 1 — Consolidamento senza cambio architetturale

- Aggiungere test di matrice per tutte le azioni su selezione e per undo/redo.
- Rendere warning-clean la build completa.
- Centralizzare allocazione e gestione OOM.
- Convertire insert/delete di range da loop per byte a operazioni bulk.

### Fase 2 — Confini del core

- Estrarre `buffer.c/.h`, preservando i tipi in header e le API a `int32_t` per
  coordinate e `size_t` per byte count.
- Estrarre `history.c/.h` e introdurre una transazione per azione utente.
- Sostituire la whitelist del dispatch con una policy esplicita per azione.

### Fase 3 — Terminale e rendering

- Estrarre decoder/input terminale e aggiungere test tabellari sulle sequenze.
- Estrarre wrap/rendering solo dopo aver fissato test UTF-8 su mapping byte,
  colonne e segmenti visuali.
- Valutare infine la divisione interna di `syntax.c` tra config e tokenizer.

## Criteri di accettazione del refactoring

- `make test` sempre verde a ogni commit.
- Build applicativa con gli stessi warning rigorosi dei test e zero warning.
- Nessun nuovo cast per forzare i tipi; coordinate `int32_t`, byte count
  `size_t`, flag `uint8_t`.
- Nessuna manipolazione di caratteri che aggiri `utf8.c`.
- Un solo snapshot/transazione per azione utente.
- Memoria dell'undo proporzionale alle modifiche, non al file completo.
- Nessuna dipendenza esterna e comportamento terminale verificato su terminale
  reale quando si introducono nuove sequenze.
