# Audit di refactoring di TinyEdit

Data: 2026-09-09  
Ambito: codice C compilato, test, documentazione tecnica e ultimi 10 commit.

## Esito sintetico

Il progetto è funzionalmente solido e la suite esistente copre bene le
regressioni terminali più delicate. Il rischio principale non è una singola
funzione, ma la concentrazione di troppe responsabilità in `tinyedit.c` e il
dispatch dei tasti basato su effetti collaterali globali e whitelist. Il bug
del paste è un esempio diretto di questa fragilità.

Il refactoring consigliato va eseguito per passi piccoli, mantenendo C99,
POSIX, zero dipendenze esterne, tipi espliciti e funzioni UTF-8 di `utf8.c`,
come richiesto da `CLAUDE.md`.

## Evidenze raccolte

- Build completa con i warning rigorosi già usati dai test.
- Suite `make test`: test syntax, settings/backup e PTY superati.
- `tinyedit.c`: circa 5.000 righe e circa 170 funzioni; gestisce terminale,
  input, buffer, undo, file I/O, rendering, ricerca, overlay e impostazioni.
- `syntax.c`: circa 1.500 righe; è già un modulo separato ma combina loading
  delle configurazioni, lookup dei linguaggi e più tokenizer.
- 51 allocazioni dinamiche nel codice applicativo esaminato; varie chiamate
  in percorsi centrali non controllano il fallimento.
- La build rigorosa segnala cinque `-Wformat-nonliteral`; quattro sono wrapper
  variadici intenzionali, uno è il prompt dinamico. Non sono errori osservati,
  ma impediscono una policy realmente warning-clean.
- `linenoise.c` contiene tipi nel `.c`, in contrasto con la convenzione
  attuale, ma è sorgente storico non compilato: non conviene rifattorizzarlo.

## Findings prioritizzati

### P0 — Nessun problema bloccante residuo rilevato

Il bug segnalato è stato corretto e coperto da test. Non sono emerse altre
regressioni che richiedano una correzione immediata prima dell'uso normale.

### P1 — Dispatch dei tasti e ciclo di vita della selezione troppo accoppiati

`editorProcessKeypress()` cattura la selezione, applica una whitelist che può
cancellarla e solo dopo esegue lo switch. Ogni nuovo tasto deve quindi essere
classificato correttamente in due luoghi. `PASTE_START_KEY` era stato aggiunto
allo switch ma non alla whitelist, causando la regressione.

Refactoring proposto: introdurre una classificazione esplicita dell'azione
(`MOVE`, `EXTEND_SELECTION`, `PRESERVE_SELECTION`, `CONSUME_SELECTION`,
`CLEAR_SELECTION`) o piccoli handler che ricevono uno snapshot immutabile
della selezione. La cancellazione dello stato va effettuata dall'azione che la
richiede, non da un filtro globale precedente allo switch.

Prima di farlo vanno fissate con test le semantiche oggi intenzionali:
Tab/Shift-Tab mantengono il blocco, apertura coppia avvolge, Backspace/Delete
cancellano, paste sostituisce, Save conserva. Resta da decidere esplicitamente
se un carattere normale o Invio debbano sostituire la selezione come negli
editor comuni: oggi la selezione viene solo cancellata e il carattere/Invio è
inserito alla posizione del cursore.

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

### P1 — Gestione uniforme degli errori di allocazione

Diversi `malloc`/`realloc` sono dereferenziati senza verifica. Il pattern
`ptr = realloc(ptr, size)` perde inoltre il vecchio puntatore se l'allocazione
fallisce. Per un editor, un OOM non dovrebbe trasformarsi in corruzione o
perdita non diagnosticata del documento.

Refactoring proposto: helper locali `xmalloc`/`xrealloc` con uscita controllata
che ripristini lo stato terminale, oppure API fallibili propagate fino al loop
principale. Per la filosofia semplice di TinyEdit, gli helper fail-fast sono
la scelta meno invasiva; prima dell'uscita va tentato un backup se il buffer è
dirty e ha un nome.

### P2 — Separare `tinyedit.c` per responsabilità

La convenzione “un modulo per responsabilità” è rispettata per clipboard,
UTF-8, settings, backup e syntax, ma non più per il core cresciuto. Una
separazione incrementale consigliata è:

1. `terminal.c/.h`: raw mode, resize, lettura/decodifica tasti, bracketed paste,
   mouse reporting e dimensioni terminale.
2. `buffer.c/.h`: righe, range, selezione, serializzazione e primitive raw.
3. `history.c/.h`: undo/redo e transazioni di modifica.
4. `render.c/.h`: wrap, mapping coordinate, barre e frame principale.
5. `tinyedit.c`: orchestrazione, file lifecycle e dispatch ad alto livello.

Non conviene estrarre subito gli overlay F1/F2/F3: dipendono ancora molto da
rendering e settings e produrrebbero API larghe. Vanno spostati solo dopo aver
stabilizzato `render.c`.

### P2 — Stato globale e dipendenze implicite

`E`, `S` e vari globali di ricerca/mouse rendono molte funzioni difficili da
testare senza PTY. Non serve introdurre dependency injection complessa:
passare `editorConfig *` alle primitive di buffer/history e lasciare globale
solo l'orchestrazione consentirebbe test C diretti e più rapidi.

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
