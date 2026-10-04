# tinyedit — stato del codice e necessità della granularità

Data: 4 ottobre 2026. Revisione: working tree corrente, base Git `9a8127f`, versione dichiarata `0.3.5`.

> Aggiornamento successivo all’audit (2026-10-04): i cinque bug della sezione
> CRITICA-URGENTE sono stati corretti nel working tree. Anche il punto 7 è
> stato riclassificato critico e corretto con caricamento a candidato, commit
> differito del nome e gestione degli esiti di persistenza. Il punto 6 è
> completamente risolto: eventi conservati, gesture nello stato del documento
> e dispatcher separato da mouse, navigazione, comandi ed editing. Anche il
> punto 5 è risolto: unica tabella ASCII, API integrate nel core e mappa
> del modulo corretta. Il report conserva
> diagnosi e riferimenti della revisione analizzata; lo stato delle correzioni
> e i test di regressione sono riportati nel [TODO](../TODO.md).

## Giudizio complessivo

**La granularità è disomogenea: buona nelle primitive di buffer, history, UTF-8 e integrazione POSIX; insufficiente nel coordinamento dell'applicazione; eccessiva soltanto in alcuni adattatori senza semantica.** Non consiglio né di accorpare indiscriminatamente gli helper né di spezzare tutte le funzioni lunghe. Il beneficio maggiore viene da tre interventi: rendere unici gli algoritmi condivisi, separare le azioni utente dal decoding degli eventi, concentrare la gestione degli invarianti di testo/layout e delle transazioni di modifica.

Il codice è già più modulare di un editor kilo monolitico: `buffer`, `history` e `render` ricevono oggetti di stato espliciti; allocazione e terminale hanno confini riconoscibili; la tabella delle impostazioni e i tokenizer per famiglia di linguaggi sono scelte appropriate. Però `tinyedit.c` conserva editing, caricamento/salvataggio, ricerca, mouse, navigazione, schermate F1/F2/F3 e disegno. La separazione dei file non ha ancora eliminato la dipendenza dei comandi da `E` e `S` globali.

**Non serve un framework, un sistema di plugin o una funzione per ogni ramo.** Servono responsabilità verificabili, ownership chiara e un solo luogo per ciascuna regola che può cambiare.

## Perimetro e metodo

Analizzati README, TODO, IDEAS, Makefile, sorgenti e contratti degli header; controllati i punti di chiamata e i test pertinenti. L'inventario copre tutte le **398 definizioni di funzione in `src/`: 313 compilate nell'applicazione, 85 nel linenoise storico escluso dal Makefile**. Le dichiarazioni negli header sono contratti delle stesse funzioni, non ulteriori unità da contare. Le funzioni dei test non sono parte dell'inventario applicativo.

La valutazione è sulla versione presente nel working tree, che conteneva già numerose modifiche non committate: non rappresenta soltanto il commit indicato. Non sono stati modificati sorgenti, header o documentazione preesistente. `make test` ha rigenerato eseguibili di build/test; questo report è un nuovo artefatto.

Per ogni funzione distinguo:

- **A — necessità alta:** la funzione separata difende un contratto, una risorsa, una regola algoritmica o un'azione utente autonoma.
- **M — necessità media:** helper utile e proporzionato; l'inlining sarebbe possibile, ma non migliora necessariamente il codice.
- **B — necessità bassa:** indirection senza valore sufficiente, duplicazione o API senza utilizzo applicativo concreto.
- **S — separazione da migliorare:** mantenere l'operazione riconoscibile, ma dividerne responsabilità o estrarre un nucleo indipendente. Non significa eliminare la funzione.
- **H — storico:** nessuna necessità per il binario corrente; giudizio separato sull'organizzazione interna della versione conservata.

Il grado indica **la necessità del confine funzionale**, non l'importanza della feature e non una certificazione di correttezza. Una funzione piccola può essere essenziale e una funzione lunga può essere coesa. Ho usato come criteri: invarianti e unità delle coordinate, ownership, riuso reale, testabilità del percorso effettivo, dipendenze globali, duplicazione, atomicità undo e sviluppi descritti nei documenti. Le prospettive di IDEAS sono condizionali: non sono requisiti approvati e non giustificano da sole nuove astrazioni.

Le misure di dimensione sono righe non vuote del corpo dopo rimozione dei commenti, incluse parentesi graffe e direttive; **non sono complessità ciclomatica né LOC normalizzate da un compilatore**. Riferimenti e chiamanti derivano dall'ispezione del codice; le callback e le varianti condizionali richiedono attenzione. Nessuna conclusione sulle prestazioni si basa sul solo numero di chiamate o di funzioni.

## Evidenza quantitativa

| Modulo | Funzioni | Righe dei corpi senza commenti/vuoti | Giudizio |
|---|---:|---:|---|
| alloc.c | 4 | 21 | Confine indispensabile e minimo |
| backup.c | 8 | 142 | Buona separazione di identità, directory e I/O |
| buffer.c | 14 | 145 | Primitive ben delimitate; invarianti lasciati ai chiamanti |
| clipboard.c | 14 | 201 | Buona separazione processi/backend/fallback |
| command.c | 4 | 21 | Metadati utili; esecuzione ancora indiretta |
| editor_state.c | 3 | 30 | Nome/mappa fuorvianti e parte dell'API scollegata |
| history.c | 10 | 89 | Granularità corretta; costo snapshot da valutare |
| menu.c | 12 | 166 | UI compatta, sufficientemente autonoma |
| render.c | 11 | 127 | Layout riusabile; manca un invariante di avanzamento |
| settings.c | 20 | 278 | Descriptor appropriati; policy e rappresentazione colori condividono il modulo |
| syntax.c | 32 | 943 | Giusta divisione per grammatica; alcuni tokenizer troppo concentrati |
| terminal.c | 24 | 484 | Confine POSIX corretto; parser input e configurazione Ghostty da distinguere |
| tinyedit.c | 142 | 3124 | Principale concentrazione di responsabilità e wrapper |
| utf8.c | 15 | 247 | Separazione necessaria; euristiche grapheme/width documentate |
| linenoise.c, storico | 85 | 1489 | Non entra nel giudizio di manutenzione del binario |

`tinyedit.c` contiene circa il **45% delle funzioni attive** e circa il **51% delle righe dei corpi attivi**. Sono 95 le funzioni attive con al massimo cinque righe secondo questa misura: il dato identifica candidati da leggere, non 95 funzioni superflue.

Le funzioni più concentrate sono `editorProcessKeypress` (393 righe), `terminalReadKey` (217), `syntaxHighlightRowMarkdown` (180), `syntaxHighlightRowGeneric` (121), `editorFindFrom` (120), `syntaxParseLangFile` (100). Il conteggio ridotto rispetto alle righe fisiche dipende dalla documentazione e dai commenti: non va confuso con le dimensioni del file.

## Verifica dello stato attuale

`make test` completato con successo: suite syntax, settings/backup, editor_state, buffer, history, render, UTF-8 e test PTY. Le build dei test utilizzano anche warning severi (`-Wextra`, conversioni, shadow, format, prototipi); la build principale usa il profilo meno severo del Makefile. Non ho provato terminali reali diversi né nuove combinazioni di tasti.

Ho inoltre compilato un harness temporaneo in `/tmp` con AddressSanitizer e UndefinedBehaviorSanitizer, includendo il core corrente e collegando gli altri moduli. Il harness richiama direttamente funzioni interne, senza aprire una sessione interattiva né modificare configurazioni personali. Esito:

| Caso | Risultato osservato | Limite della prova |
|---|---|---|
| Prompt: append di 160 byte su allocazione di 128 | ASan: heap-buffer-overflow, scrittura di 160 byte | Prova diretta della funzione, non digitazione PTY |
| Top bar: nome di 300 byte, viewport di 250 colonne | ASan: stack-buffer-overflow, lettura di 250 byte | Nome sintetico assegnato allo stato; funzione reale di disegno |
| Riga `é\tX`, tab stop 4 | Larghezza render 4; coordinata cursore finale 5 | Misura algoritmica; nessuna dipendenza dal font del terminale |
| Wrap di `界` in una colonna | Timeout dopo 2 secondi; processo terminato dal harness | Conferma del mancato avanzamento; non misura prestazioni generali |
| PageDown con documento vuoto, viewport 80×20 | UBSan: accesso membro su puntatore NULL; ASan: SEGV | Evento passato tramite pending_key alla funzione reale di dispatch |

Gli esiti positivi della suite **non coprono questi confini**. Le prove aggiuntive non sono state trasformate in test del repository, perché la richiesta riguarda un'analisi e non una correzione.

## Criticità ordinate per priorità

### 1. Sicurezza del buffer dei prompt — priorità alta, confermata

[`editorPromptAppend`](../src/tinyedit.c#L749) raddoppia la capacità dichiarata ma non chiama `teRealloc` prima della copia. L'allocazione iniziale è 128 byte; 128 byte di testo richiedono già 129 byte con NUL. È un errore di memoria concreto, già indicato come aperto nel TODO e confermato da ASan.

**Implicazione sulla granularità:** l'helper va conservato, perché raccoglie proprio l'invariante capacità/lunghezza/NUL. Il confine è giusto; il contratto non è implementato. Accorparlo al prompt disperderebbe ulteriormente una regola che serve per digitazione e paste. Dopo il fix servono prove oltre soglia per entrambi i percorsi.

### 2. Top bar e lunghezza restituita da snprintf — priorità alta, confermata

[`editorDrawTopBar`](../src/tinyedit.c#L2255) usa un buffer locale di 160 byte e la lunghezza restituita da `snprintf`, limitandola alle colonne ma non alla capacità del buffer. `snprintf` restituisce la lunghezza richiesta, anche quando tronca. Su schermi abbastanza larghi e nomi lunghi `abAppend` legge oltre lo stack.

**Implicazione:** una piccola primitive per append formattato con lunghezza effettivamente disponibile potrebbe essere giustificata dai vari punti di formattazione, oppure basta applicare uniformemente il clamp già presente in `editorInfoAppendLine` e nell'help. Non serve un motore di template. La gestione delle lunghezze deve essere unica o sistematicamente verificabile.

### 3. Wrap senza avanzamento — priorità alta, confermata

[`renderRowSegments`](../src/render.c#L58) interrompe la scansione se il primo grapheme supera `wrapcols`. Con `界` e larghezza 1, `pos` resta uguale a `line_start`; il ciclo esterno aggiunge segmenti senza consumare testo. `renderSoftWrapCols` può restituire 1 su viewport strettissime o con limite impostato a 1. Il caso porta a consumo crescente di memoria e infine OOM, o a un editor che appare bloccato.

**Implicazione:** la segmentazione deve garantire avanzamento anche quando un glyph è più largo della viewport. La funzione è già il posto giusto per il contratto; spezzarla ulteriormente non corregge l'algoritmo. Il test utile è sull'invariante «ogni iterazione consuma almeno un grapheme o termina».

### 4. Due modelli diversi per tab e UTF-8 — priorità alta, confermata

[`editorUpdateRow`](../src/tinyedit.c#L368) espande i tab usando `idx`, un offset di byte nel render, mentre `bufferRowCxToRx` usa colonne visive. Per `é\tX` il primo calcola il padding a partire da due byte, il secondo da una colonna: render e cursore divergono. Anche la mappatura inversa in `editorDrawRowSegment` usa `render_byte % tab_stop` e avanza `source_byte++`: conserva i byte multibyte nel testo ordinario, ma riproduce la stessa confusione nella gestione dei tab.

**Implicazione:** serve un'unica camminata del testo che tenga esplicitamente offset sorgente, offset render e colonna visiva. L'estrazione utile è una primitive di layout, non una serie di wrapper rinominati. È anche il punto in cui la regola di progetto sul cammino UTF-8 non è rispettata nel core di preparazione delle righe.

### 5. API di auto-close testata ma scollegata — priorità media, verificata staticamente

**Aggiornamento successivo alla diagnosi (2026-10-04): completamente risolto.**
Il core usa `editorAutoClosePairFor()` per opener e wrapping della selezione
e `editorIsAsymmetricAutoClose()` per riconoscere i closer. Entrambe consultano
l’unica tabella ASCII in `editor_state.c`; tabella, wrapper e ciclo paralleli
nel core sono stati eliminati. Module map, README e riferimenti delle
impostazioni descrivono la responsabilità effettiva. Corretto anche il caso
in cui un opener disabilitato non sostituiva la selezione. Unit test, test
del core reale, PTY e ASan/UBSan passati. La diagnosi seguente è storica.

`editorAutoClosePairFor` e `editorIsAsymmetricAutoClose` in `editor_state.c` non hanno chiamanti nel codice applicativo. I test unitari esercitano quegli helper; `tinyedit.c` mantiene un'altra tabella e `editorAutoCloseFor`, più un ciclo autonomo per i closer asimmetrici. I test PTY coprono alcune azioni effettive, ma i test unitari del modulo non verificano la fonte di verità usata in digitazione.

**Implicazione:** integrare davvero gli helper condivisi e togliere la copia, oppure eliminare l'API scollegata e testare il nucleo reale. Tenerle entrambe aumenta il costo e permette divergenze. La Module map attribuisce a `editor_state.c` inizializzazione e cleanup: in realtà il file contiene selezione e policy auto-close, mentre inizializzazione/reset restano nel core.

### 6. Dispatcher: crash su documento vuoto e consumo degli eventi — priorità alta per il crash

**Aggiornamento successivo alla diagnosi (2026-10-04): completamente risolto.**
Oltre alla guardia sul documento vuoto già introdotta, `editorProcessKeypress()`
esegue una sola volta il primo evento non mouse letto nel burst e lascia
in coda gli eventi oltre il limite di 64 report. `editorHandleMouseEvent()`
instrada ogni report anche verso i menu, senza leggere input. Drag e ancore
sono in `editorDocument.mouse`, azzerati al reset e al recupero; release e
input da tastiera interrompono la gesture. `editorDispatchKey()` delega a
handler distinti per navigazione/selezione, comandi e modifiche al testo.
Regressioni con input realmente accodato, PTY e ASan/UBSan passate; dettagli
nel TODO. Il testo seguente conserva la diagnosi della revisione originale.

[`editorProcessKeypress`](../src/tinyedit.c#L4834) combina lettura, menu, selezione, editing, navigazione, clipboard, schermate e drenaggio del mouse. Nel drenaggio, se legge un evento successivo diverso da `MOUSE_EVENT_KEY`, non lo rimette in `pending_key` e non lo redispatcha: il tasto già consumato viene perso. È un rischio direttamente leggibile nel flusso; non ho eseguito una riproduzione PTY specifica del timing.

Lo stato di drag e delle ancore è `static` dentro il ramo mouse: non è ripristinato da `editorResetDocument`, e contrasta con la convenzione di globali/static di progetto. Il ramo PageUp/PageDown con wrap accede inoltre a `rows[0]` anche se il documento è vuoto; `editorSoftWrapCols` restituisce sempre almeno 1, quindi non protegge il caso. Quest'ultimo percorso è stato confermato anche dal harness con UBSan/ASan: PageDown su documento vuoto provoca accesso NULL e crash. Richiede una guardia prima dell'accesso e una regressione dedicata; il possibile tasto perso nel drenaggio rimane invece un rilievo statico di priorità media.

**Implicazione:** estrarre gestione evento mouse, movimenti con selezione e invocazione dei comandi. Il dispatcher deve decidere la destinazione, non implementare ciascuna destinazione. Uno stato esplicito di input deve conservare ogni evento non gestito.

### 7. Caricamento e salvataggio non completamente transazionali — priorità media, evidenza statica

**Aggiornamento successivo alla diagnosi:** riclassificato CRITICO-URGENTE
e corretto il 2026-10-04. La priorità media e l’evidenza statica nel titolo
descrivono l’audit originale; ora esistono regressioni con errori I/O
iniettati e verifiche del core/PTY. `fileio.c` separa parsing e persistenza
dallo stato globale: il documento corrente e il nome vengono aggiornati
solo ai rispettivi punti di commit. Gli errori dopo rename sono distinti
dagli errori precedenti alla sostituzione. Dettagli e limiti nel TODO.


`editorOpenFile` verifica il file con un'apertura preliminare, poi resetta il documento e `editorOpen` lo riapre. Un errore tra le due aperture non lascia disponibile il vecchio documento; gli errori di `getline` non vengono distinti da EOF con `ferror`, quindi un caricamento parziale può apparire riuscito. `editorSaveInternal` sostituisce il nome corrente prima del successo del Save as: un errore conserva il testo, ma cambia l'identità del documento; il backup precedente non viene gestito contestualmente. `editorOpen` rimuove tutti i CR/LF finali della riga: può perdere un CR che appartiene al contenuto, oltre al terminatore vero.

**Implicazione:** un loader che costruisce un documento temporaneo e restituisce esito, e un commit del nome dopo il successo, sono separazioni giustificate già oggi. Non occorre anticipare il multi-buffer per ottenere questi benefici. `editorAtomicSave`, `backupWrite` e `settingsSave` usano temp+rename, ma non sincronizzano la directory dopo rename: atomicità della sostituzione e durabilità in caso di perdita di alimentazione sono proprietà diverse.

### 8. Confini delle modifiche e aggiornamenti troppo frequenti — priorità media

Le coppie `Raw`/comando sono utili perché distinguono la mutazione dall'unico snapshot dell'azione utente. Però «Raw» significa senza snapshot, **non** senza rendering/highlight: una cancellazione di grapheme in `editorDelChar` passa per una cancellazione/ricostruzione per ogni byte. Auto-indent e auto-close inseriscono più byte con ripetuti aggiornamenti; auto-close richiama più volte `editorInsertChar`, lasciando l'unità dell'undo alla coalescenza temporale anziché a un confine esplicito. Anche indentare più righe propaga ripetutamente lo stato syntax.

**Implicazione:** mantenere primitive raw e comandi atomici, ma concentrare «inizio azione → mutazioni → ricostruzione dal primo punto toccato → fine azione». Può bastare un protocollo semplice; non propongo un sistema generico di transazioni con rollback. Il batching serve a correttezza e costo, non soltanto all'estetica dei nomi.

**Aggiornamento 2026-10-04 — corretto:** scope annidabili delle modifiche nel
core, un solo snapshot per azione e cache aggiornati alla chiusura esterna.
Le righe modificate vengono ricostruite una volta; il tokenizer percorre
le righe coinvolte, poi propaga lo stato fino alla convergenza. Backspace
usa un range grapheme; indentazione usa span; auto-close ha undo esplicito.
Regressioni sul core applicativo verificano frequenza degli aggiornamenti,
propagazione Markdown e atomicità undo/redo.

### 9. Ricerca: algoritmo, sessione e vista mescolati — priorità media

`editorFindFrom` compila la regex, sceglie il percorso riga/documento, cerca e modifica cursore/highlight. `editorFindCallback` gestisce sessione, tasti, direzione e copie di match; parte dello stato (`last_*`) è fuori da `editorSearch`. I percorsi regex inversi su riga e testo duplicano l'algoritmo con tipi diversi. La ricompilazione ad ogni ricerca/occorrenza e la serializzazione per regex multilinea hanno costi evidenti staticamente, ma non sono stati profilati.

**Implicazione:** restituire un risultato di ricerca senza muovere il cursore; la sessione decide come applicarlo. Unificare il nucleo della ricerca inversa conservando conversioni esplicite. Per estensioni come opzioni di ricerca, selezione del perimetro o navigazione fra risultati questo confine diventa più importante. Non serve aggiungere subito tali feature.

**Aggiornamento 2026-10-04 — corretto:** estratto `search.c` con API che
restituisce un match e non modifica stato UI. Query compilata e navigazione
appartengono a `editorSearch`; riuso della compilazione e del testo multilinea
con invalidazione sulle mutazioni e sui cambi documento/history. Unificato
il nucleo regex inverso. Regressioni nel motore e nel core verificano cache,
coordinate e cancellazione; corretti gli edge case di `^` dopo newline e
match vuoti oltre EOF nelle ricerche multilinea senza wrap.

### 10. Scalabilità e documentazione — priorità ordinaria

`editorDrawRows` ricalcola il totale delle righe video per ogni riga dello schermo e converte ogni posizione ripercorrendo il buffer: costo O(righe schermo × righe documento), anche con cache di segmentazione valida. `editorCountChars` percorre tutto il testo a ogni status bar; il matching parentesi può attraversare il documento a ogni redraw. Snapshot undo completi moltiplicano la memoria per gli step conservati, e i vettori vengono riallocati di frequente. Sono opportunità di misurazione, non prove di lentezza percepibile per ogni file.

README attribuisce ancora raw mode e buffer al core e non elenca tutti i nuovi moduli; IDEAS descrive il buffer come statico, mentre è dinamico, e tratta la preservazione line ending come futura pur essendo già presente nello stato e nella serializzazione. Questi disallineamenti non devono guidare nuovi refactoring. Ci sono anche controlli `if (!ptr)` dopo gli allocator fatali: rami irraggiungibili che confondono la policy OOM, specialmente in backup/clipboard/save. Le allocazioni implicite di POSIX (`realpath(..., NULL)`, `getline`) meritano una policy d'errore esplicita distinta.

**Aggiornamento 2026-10-04 — affrontato con misure:** benchmark del core,
dati prima/dopo e limiti in `performance-2026-10-04.md`. Corretti i redraw
ripetitivi: iterazione wrapped sequenziale, cache dei conteggi e del matching,
crescita geometrica del frame. Rimossi check irraggiungibili degli allocator;
backup distingue gli errori `realpath` da ENOENT. README/IDEAS aggiornati.
Snapshot completi, crescita dei vettori e conversioni lineari restano scelte
misurate/documentate; non sono dichiarati risolti da un cambio di algoritmo.

## Quale granularità conservare e quale cambiare

### Separazioni necessarie anche con un solo chiamante

Allocator controllati; attivazione/ripristino di ogni modalità terminale; gestione del processo figlio; ownership delle righe e degli snapshot; decoding UTF-8 bounded; conversioni byte/colonne/righe video; normalizzazione della selezione; serializzazione per disco versus intervallo clipboard. Questi confini permettono di ragionare su precondizioni ed effetti e di testare gli algoritmi senza TTY. Il riuso non è l'unica giustificazione.

Le funzioni `editorInsertRow`, `editorDelRow`, `editorRowInsertString` e `editorDeleteRange` non sono semplici duplicati delle primitive buffer: aggiungono cache, dirty, cursore o undo. Eliminare quel livello senza sostituirne il contratto obbligherebbe ogni chiamante a ricordarsi gli stessi effetti.

### Separazioni ridondanti o deboli

`editorRowSegments`, `editorSegVisibleEnd`, `editorRxToSegment` inoltrano gli stessi argomenti senza aggiungere stato o policy. Sono candidati concreti a chiamata diretta. Gli adattatori `editorRowCxToRx`, `editorTextCols`, `editorVideoRowOf`, `editorRowsToString` invece vincolano impostazioni/documento corrente o policy line-ending: oggi riducono rumore, anche se in futuro andranno parametrizzati o rimossi quando gli oggetti diventano espliciti.

`clipboardFree` esegue soltanto `free`: mantenibile come convenzione di ownership dell'API, ma non essenziale nel progetto attuale. `clearRedo`, `popSnapshot`, `editorFindModeIndicator`, `editorInfoAppendBlank` sono piccoli e legittimi: nominano un'operazione o una policy e mantengono leggibile il chiamante. Il risparmio di poche righe non giustifica un refactoring prioritario.

`clipboardBackendName`, `settingColorName`, `utf8StrWidth` non sono referenziate dai corpi delle funzioni applicative attive: l'API pubblica/test può comunque giustificarle. `utf8StrWidth` in particolare è una primitive pertinente al dominio e può essere utile per correggere il layout; non la eliminerei insieme a un getter di sola presentazione non usato. «Non usato oggi» non equivale a «inutile», ma l'uso futuro deve essere concreto.

### Funzioni da dividere per responsabilità

- `editorProcessKeypress`: dispatcher, comandi, mouse e movimento con selezione.
- `terminalReadKey`: acquisizione/sequenza e decodifica CSI/SS3/CSI-u/mouse, preservando le sequenze già verificate e la gestione dell'input parziale.
- `editorFindFrom`: compilazione/policy, ricerca pura, applicazione del match alla vista.
- `editorFindCallback`: stato sessione e risposta ai tasti; evitare frammentazione in getter senza logica.
- `editorInsertCharAutoClose`: riconoscimento/policy di coppia, wrapping della selezione e modifica atomica; trasporto UTF-8 non deve dipendere dall'auto-close.
- `editorSettingsDrawRow`: formattazione del valore e campione colore; metadati di preview solo se sostituiscono realmente i confronti per chiave.
- `syntaxHighlightRowMarkdown`: stato di blocco e scansione inline riutilizzabile. Il ramo che chiude math multilinea ha una seconda scansione inline ridotta, e quello che chiude emphasis ritorna senza analizzare la coda: un nucleo unico evita differenze accidentali.
- `syntaxParseLangFile`: parsing dei campi, validazione e distruzione completa delle liste. Le assegnazioni ripetute di liste perdono le precedenti e i percorsi invalidi liberano il vettore ma non ogni stringa.

`syntaxHighlightRowGeneric` e `syntaxHighlightRowXml` sono lunghe ma relativamente coese: estrarre sottoscanner quando isolano un contratto riusabile (stringa/commento/identificatore), non una funzione per ogni `if`. CSS è già compatto: non richiede un nuovo modulo dedicato.

## Prospettiva di nuove implementazioni

| Sviluppo | Cosa è già adeguato | Confine da rafforzare prima o insieme allo sviluppo | Cosa non anticipare |
|---|---|---|---|
| Correzioni prompt | append e loop distinti | capacità effettiva/NUL; cancellazione grapheme-aware | Libreria universale di widget |
| Multi-buffer | `editorDocument` contiene buffer/cursore/selezione/history/file | comandi con documento esplicito; stato ricerca `last_*`; vista da associare al buffer; lifecycle sicuro | Manager di buffer prima della decisione UI |
| Completamento percorsi | prompt condiviso per Open/Save as | modello del testo separato dal render/callback ricerca; risposta al Tab locale al prompt | Reintegrare linenoise intero |
| Navigazione file laterale | comando Open e layout già riconoscibili | apertura transazionale; rettangolo viewport indipendente da barre/gutter | Albero virtuale o backend astratto senza requisiti |
| Più grammatiche/Python triple quote | tabelle C-like e dispatcher dedicati | stato lessicale esplicito, sottoscanner bounded, propagazione coerente | Parser completo o syntax annidata non approvata |
| Temi e status bar personalizzata | descriptor, palette e funzioni barre | testo con clamp/width; statistiche aggiornate sulle modifiche | Sistema di stili generalizzato |
| Build vanilla | menu/syntax sono moduli già individuabili | core non deve richiedere chiamate/stato syntax quando escluso; layout/editor commands separati dalle feature opzionali | Moltiplicare `#ifdef` in tutte le primitive |
| File grandi | buffer dinamico e cache wrap per riga | batching, conteggi/cache invalidabili, scansione progressiva viewport; profiling undo | Rope/piece table prima di misure e obiettivi |
| Preservazione CRLF/LF | metadati e serializzazione già presenti | loader unico che consuma esattamente il terminatore; recovery coerente | Trattarla ancora come feature totalmente assente |

## Ordine consigliato degli interventi

1. Correggere overflow prompt/top bar, avanzamento wrap, accesso Page su buffer vuoto e coerenza tab/UTF-8; aggiungere regressioni che colpiscano i confini reali.
2. Eliminare la duplicazione auto-close e collegare i test al percorso usato dall'applicazione. Allineare Module map e documentazione alle responsabilità effettive.
3. Estrarre gestione mouse/eventi e comandi dal dispatcher; preservare gli eventi letti in anticipo. Un solo punto di applicazione delle regole di selezione per i movimenti.
4. Rendere esplicito il confine dell'azione utente e differire ricostruzioni/highlight nelle modifiche multiple. Rendere loader e Save as transazionali.
5. Separare ricerca pura/sessione e pannello impostazioni dal flusso principale, quando si interviene in quelle aree. Spostare codice solo dopo aver chiarito gli argomenti e gli effetti: un file nuovo con accesso totale a `E` non risolve il problema.
6. Misurare redraw e memoria undo su carichi rappresentativi; soltanto dopo scegliere cache aggiuntive o strutture diverse. Eliminare i wrapper trasparenti nel normale lavoro nelle aree coinvolte.

Non propongo una soglia obbligatoria di righe per funzione né un numero target di moduli. Un refactoring è riuscito se una regola ha un solo proprietario, il test esercita quel proprietario e un comando può cambiare senza richiedere interventi coordinati in rami non pertinenti.

## Inventario funzione per funzione

Le tabelle seguenti riportano ogni definizione, il grado attuale, la motivazione/azione e la prospettiva specifica. I riferimenti di riga sono quelli della revisione analizzata. L'inventario CSV allegato aggiunge dimensione del corpo e firma, per filtrare e ricontrollare la copertura; non sostituisce il giudizio manuale.

### src/alloc.c — 4 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`teOutOfMemory`](../src/alloc.c#L16) | A | Un solo percorso fatale garantisce exit e cleanup atexit; conservare. | Stessa policy per ogni nuova allocazione applicativa. |
| [`teMalloc`](../src/alloc.c#L30) | A | Normalizza dimensione zero e gestisce OOM; API obbligatoria. | Mantenere centrale anche nelle nuove strutture. |
| [`teRealloc`](../src/alloc.c#L43) | A | Non restituisce NULL al chiamante; separazione necessaria. | Controllare anche overflow delle dimensioni nei chiamanti. |
| [`teStrdup`](../src/alloc.c#L56) | A | Stringhe possedute con policy OOM uniforme. | Riutilizzare per nomi, config e documenti futuri. |

### src/backup.c — 8 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`fnv1a64`](../src/backup.c#L32) | M | Hash di identità isolato dal filesystem; piccolo e coeso. | Resta dettaglio privato del naming, non utilità globale. |
| [`absolutePathOf`](../src/backup.c#L48) | A | Risoluzione del percorso anche per file nuovo; contratto distinto. | Policy esplicita per realpath/OOM e identità dei nuovi buffer. |
| [`backupPathFor`](../src/backup.c#L85) | A | Centralizza percorso e hash usati da tutte le operazioni. | Riutilizzabile per documenti multipli senza legarlo a E. |
| [`ensureBackupDir`](../src/backup.c#L104) | A | Gestisce creazione e permessi delle directory. | Conservare come responsabilità locale del backup. |
| [`backupExists`](../src/backup.c#L127) | M | Query leggibile sul recovery senza esporre naming. | Utile per un eventuale elenco documenti recuperabili. |
| [`backupRead`](../src/backup.c#L141) | A | Parsing header, lettura e ownership del contenuto; coeso. | Esplicitare errori/EINTR e relazione con loader comune. |
| [`backupWrite`](../src/backup.c#L190) | A | Temp, flush, fsync e rename formano una singola operazione. | Non generalizzare finché non si allineano policy di mode/durabilità. |
| [`backupRemove`](../src/backup.c#L248) | M | Incapsula il naming per la rimozione. | Coordinare identità vecchia/nuova nel Save as. |

### src/buffer.c — 14 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`bufferRowCxToRx`](../src/buffer.c#L15) | A | Byte sorgente → colonne con grapheme e tab; contratto essenziale. | Deve essere coerente con la primitive unica di layout. |
| [`bufferRowRxToCx`](../src/buffer.c#L38) | A | Colonne → confine sorgente; semantica del glyph contenente il target. | Conservare e rendere esplicito il rounding per click/navigazione. |
| [`bufferInsertRow`](../src/buffer.c#L66) | A | Possiede allocazione e spostamento righe; niente UI/undo. | Base adatta al multi-buffer; verificare limiti dimensioni/alias. |
| [`bufferFreeRow`](../src/buffer.c#L87) | A | Un solo proprietario della distruzione di testo e cache. | Conservare e aggiornare quando cambia il contenuto di erow. |
| [`bufferDeleteRow`](../src/buffer.c#L101) | A | Rimozione strutturale distinta da cancellazione caratteri. | Riutilizzabile per comandi e documenti multipli. |
| [`bufferClear`](../src/buffer.c#L115) | A | Distruzione dell'intero buffer e reset del suo stato. | Elemento del lifecycle del documento. |
| [`bufferRowInsertByte`](../src/buffer.c#L128) | M | Convenienza byte sopra insert span; nessun grapheme implicito. | Diminuirne l'uso per operazioni multibyte; non aggiungere policy Unicode qui. |
| [`bufferRowInsert`](../src/buffer.c#L140) | A | Primitive span con NUL e memmove; confine opportuno. | Usarla per batching, con limiti e contratto alias espliciti. |
| [`bufferRowAppend`](../src/buffer.c#L155) | M | Append specializzato evita spostamento del suffisso. | Mantenere se utile a join; non duplicare policy di aggiornamento. |
| [`bufferRowDeleteByte`](../src/buffer.c#L168) | M | Convenienza sulla primitive di range; documentata come byte. | Preferire cancellazione range unico per grapheme completi. |
| [`bufferRowDeleteRange`](../src/buffer.c#L178) | A | Cancellazione di span mantiene size/NUL in un punto. | Nucleo comune per delete, replace e outdent. |
| [`bufferRowOutdent`](../src/buffer.c#L193) | A | Regola indipendente su tab o spazi esistenti; testabile. | Riutilizzare per outdent di riga/blocco, distinta da reindentazione futura. |
| [`bufferSerialize`](../src/buffer.c#L216) | A | Policy disco con terminatori e newline finale; ownership chiara. | Conservare per save, confronto disco e recovery. |
| [`bufferSerializeRange`](../src/buffer.c#L248) | A | Intervallo clipboard LF e NUL differisce dal formato disco. | Riutilizzabile per comandi di riga intera senza nuova serializzazione. |

### src/clipboard.c — 14 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`internalCopy`](../src/clipboard.c#L28) | M | Aggiorna buffer fallback e lunghezza in un punto. | Rimuovere controlli NULL irraggiungibili dopo allocator fatali. |
| [`internalPaste`](../src/clipboard.c#L42) | M | Restituisce una copia posseduta del fallback. | Contratto stabile anche con più documenti. |
| [`commandExists`](../src/clipboard.c#L62) | A | Ricerca PATH senza shell e gestione segmenti vuoti. | Tenere privata finché non esiste un secondo uso concreto. |
| [`detectBackend`](../src/clipboard.c#L93) | A | Ordine delle preferenze distinto dall'esecuzione processi. | Estendere tabella/backend senza cambiare editing. |
| [`clipboardBackend`](../src/clipboard.c#L128) | M | Lazy detection centralizzata; getter con semantica reale. | Valutare invalidazione solo se si aggiunge cambio backend runtime. |
| [`clipboardBackendName`](../src/clipboard.c#L139) | B | API descrittiva senza chiamante applicativo corrente. | Mantenere soltanto per un uso UI concreto; status personalizzata è condizionale. |
| [`execCopyCommand`](../src/clipboard.c#L158) | A | argv e uscita del figlio distinti dalla pipe del genitore. | Nuovi backend restano confinati qui o in una tabella semplice. |
| [`execPasteCommand`](../src/clipboard.c#L186) | A | argv di lettura specifici; non sovrapporre alle scritture. | Conservare separazione copy/paste senza helper generico forzato. |
| [`waitForChild`](../src/clipboard.c#L215) | A | Gestisce EINTR e stato figlio per entrambe le direzioni. | Riutilizzo locale corretto; non introdurre process framework. |
| [`runCopyCommand`](../src/clipboard.c#L230) | A | Pipe, fork, SIGPIPE, write e wait compongono un contratto. | Aggiungere timeout se requisito reale; preservare restituzione dell'esito. |
| [`runPasteCommand`](../src/clipboard.c#L263) | A | Lettura dinamica e verifica figlio con ownership esplicita. | Uniformare policy OOM/EINTR; mantenere autonomia dal documento. |
| [`clipboardCopy`](../src/clipboard.c#L311) | A | Facade backend/fallback impedisce dettagli POSIX nel comando. | Restare API comune a prompt e testo del documento. |
| [`clipboardPaste`](../src/clipboard.c#L334) | A | Facade restituisce dati e fallback con ownership uniforme. | Base corretta per comandi futuri di paste. |
| [`clipboardFree`](../src/clipboard.c#L349) | B | Solo free; valore limitato alla convenzione di ownership dell'API. | Conservarlo se si vuole mantenere l'API di rilascio, senza priorità di refactoring. |

### src/command.c — 4 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`commandGetDescriptor`](../src/command.c#L35) | A | Fonte unica di etichetta, shortcut e legame setting. | Aggiungere metadati soltanto quando sostituiscono duplicazioni reali. |
| [`commandIsSetting`](../src/command.c#L49) | M | Predicato comune evita conoscenza della tabella ai menu. | Conservare; separare in futuro classe comando e binding tastiera. |
| [`commandIsChecked`](../src/command.c#L60) | A | Adatta comando a setting letto da copia live/draft. | Resta necessario con temi/menu opzionali. |
| [`commandToggleSetting`](../src/command.c#L74) | A | Mutazione della copia settings separata dall'applicazione/persistenza. | Utile per un executor comandi che non passi da tasti sintetici. |

### src/editor_state.c — 3 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`editorSelectionRange`](../src/editor_state.c#L15) | A | Normalizza ancora/cursore in range ordinato senza globali. | Base condivisa per editing, render e multi-buffer. |
| [`editorAutoClosePairFor`](../src/editor_state.c#L46) | B | Policy appropriata, ma oggi è scollegata dal percorso reale duplicato. | Integrarla come fonte unica e conservarla; altrimenti eliminare API/test orfani. |
| [`editorIsAsymmetricAutoClose`](../src/editor_state.c#L61) | B | Predicato testato ma non usato dall'applicazione. | Collegarlo alla policy unica di auto-close, senza aggiungere un secondo livello inutile. |

### src/history.c — 10 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`historyMakeSnapshot`](../src/history.c#L16) | A | Copia posseduta di testo/cursore, indipendente dal render. | Contratto utile anche se gli snapshot saranno sostituiti da delta. |
| [`historyFreeSnapshot`](../src/history.c#L37) | A | Distruzione completa di una risorsa complessa. | Conservare con qualsiasi rappresentazione della history. |
| [`clearStack`](../src/history.c#L50) | M | Un solo ciclo di distruzione per undo e redo. | Privato e proporzionato; nessuna nuova astrazione necessaria. |
| [`clearRedo`](../src/history.c#L59) | M | Nomina l'invalidazione al nuovo edit e azzera il conteggio. | Conservare come policy locale della history. |
| [`historyRecordEdit`](../src/history.c#L70) | A | Grouping, invalidazione redo e limite profondità sono policy coese. | Separare unità dell'azione da tempo; profilare memoria snapshot. |
| [`popSnapshot`](../src/history.c#L97) | M | Trasferimento ownership e decremento count centralizzati. | Evitare shrink ad ogni pop se le misure motivano capacità persistente. |
| [`historyBeginUndo`](../src/history.c#L109) | A | Trasferisce stato attuale al redo e restituisce snapshot. | Conservare rispetto al restore per consentire ricostruzione cache esterna. |
| [`historyBeginRedo`](../src/history.c#L126) | A | Operazione simmetrica con direzione esplicita. | La piccola duplicazione non giustifica un enum direzione generico oggi. |
| [`historyRestoreSnapshot`](../src/history.c#L143) | A | Ripristina sorgente/cursore senza accoppiare syntax/UI. | Chiarire metadata/selection inclusi quando si definisce undo futuro. |
| [`historyClear`](../src/history.c#L161) | A | Lifecycle completo di undo/redo e grouping. | Riutilizzabile nella distruzione dei singoli documenti. |

### src/linenoise.c — 85 funzioni

Tutte le funzioni seguenti sono escluse dal binario. H descrive la necessità applicativa nulla; la motivazione valuta il confine interno e la prospettiva chiarisce eventuali principi riutilizzabili. Non propongo refactoring di questo archivio come priorità.

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`utf8ByteLen`](../src/linenoise.c#L160) | H | Confine interno pertinente: Stima lead byte distinta dalla validazione del decoder. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8DecodeChar`](../src/linenoise.c#L176) | H | Confine interno pertinente: Contratto rigoroso available/consumed/valid; essenziale per tutto il testo. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`isVariationSelector`](../src/linenoise.c#L211) | H | Confine interno pertinente: Proprietà Unicode nominata e riusata. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`isSkinToneModifier`](../src/linenoise.c#L220) | H | Confine interno pertinente: Proprietà emoji distinta da combining marks. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`isZWJ`](../src/linenoise.c#L229) | H | Confine interno pertinente: Predicato piccolo ma esprime regola condivisa di composizione. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`isRegionalIndicator`](../src/linenoise.c#L238) | H | Confine interno pertinente: Regola bandiere distinta, condivisa avanti/indietro. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`isCombiningMark`](../src/linenoise.c#L248) | H | Confine interno pertinente: Range supportati centralizzati per grouping/width. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`isGraphemeExtend`](../src/linenoise.c#L261) | H | Confine interno pertinente: Compone proprietà usate dai due versi di navigazione. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8DecodePrev`](../src/linenoise.c#L273) | H | Confine interno pertinente: Ricerca suffix bounded distinta dal forward decoder. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8PrevCharLen`](../src/linenoise.c#L300) | H | Confine interno pertinente: Passo grapheme all'indietro, indipendente da UI. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8NextCharLen`](../src/linenoise.c#L361) | H | Confine interno pertinente: Passo grapheme bounded avanti con recupero byte singolo. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8CharWidth`](../src/linenoise.c#L423) | H | Confine interno pertinente: Larghezza codepoint distinta da sequenza/grapheme. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`ansiEscapeLen`](../src/linenoise.c#L485) | H | Confine interno pertinente: Misura escape bounded per stringhe decorate. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8StrWidth`](../src/linenoise.c#L507) | H | Confine interno pertinente: Primitive utile, oggi usata nei test ma non nel core attivo. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`utf8SingleCharWidth`](../src/linenoise.c#L552) | H | Confine interno pertinente: Larghezza di un'unità con fallback invalid coerente. Implementazione storica distinta. | Nessuna necessità nel binario; usare utf8.c attuale, con decoding bounded e recovery, per ogni nuova feature. |
| [`linenoiseMaskModeEnable`](../src/linenoise.c#L620) | H | Setter pubblico nominato; separazione interna proporzionata alla policy mask. | Non introdurre password prompt nell'editor senza requisito. |
| [`linenoiseMaskModeDisable`](../src/linenoise.c#L629) | H | Operazione inversa pubblica leggibile; non accorpare solo per una riga. | Nessuna necessità per il core attuale. |
| [`linenoiseSetMultiLine`](../src/linenoise.c#L639) | H | API di modalità propria del vecchio line editor. | Non equivale al soft-wrap del documento attuale. |
| [`isUnsupportedTerm`](../src/linenoise.c#L649) | H | Detection capability separata dal fallback input. | Riutilizzare il concetto, non aggiungere dipendenza dal modulo storico. |
| [`enableRawMode`](../src/linenoise.c#L666) | H | Setup POSIX autonomo con workaround specifico del prototipo. | Il core ha terminalEnableRawMode; evitare duplicazione. |
| [`disableRawMode`](../src/linenoise.c#L716) | H | Cleanup distintamente richiamabile da uscita e stop. | Usare lifecycle terminal.c attuale. |
| [`getCursorPosition`](../src/linenoise.c#L736) | H | Query terminale separata dalle dimensioni e con file descriptor espliciti. | Il fallback esiste già in terminal.c; nessun secondo algoritmo necessario. |
| [`getColumns`](../src/linenoise.c#L764) | H | Facade ioctl/query/fallback a 80, coesa nel vecchio editor. | Usare terminalGetWindowSize e geometry corrente. |
| [`linenoiseClearScreen`](../src/linenoise.c#L807) | H | API di emissione ANSI piccola e autonoma. | Nessuna esigenza di reintegrarla; refresh corrente possiede il frame. |
| [`linenoiseBeep`](../src/linenoise.c#L819) | H | Feedback minimo nominato, interno e coeso. | Eventuale beep nuovo resta policy editor/terminale. |
| [`freeCompletions`](../src/linenoise.c#L832) | H | Distruzione di tutte le stringhe e vettore; separazione necessaria internamente. | Concetto utile al completion percorsi; nuovo owner esplicito nel prompt attuale. |
| [`refreshLineWithCompletion`](../src/linenoise.c#L854) | H | Disegna proposta salvando/ripristinando stato reale; operazione autonoma. | Per completion futuro preferire modello proposta senza mutare temporaneamente lo stato. |
| [`completeLine`](../src/linenoise.c#L902) | H | Controller Tab/Esc/commit mescola navigazione e render. | Ispirazione per stati, non copia diretta del controller nel core. |
| [`linenoiseSetCompletionCallback`](../src/linenoise.c#L960) | H | API callback piccola, legittima per libreria standalone. | Completion futuro può usare callback prompt limitata, non includere tutta la libreria. |
| [`linenoiseSetHintsCallback`](../src/linenoise.c#L970) | H | API hint indipendente dal completamento. | Nessuna necessità attuale; aggiungere solo con feature hint richiesta. |
| [`linenoiseSetFreeHintsCallback`](../src/linenoise.c#L980) | H | Esplicita ownership dei dati prodotti dalla callback. | Con nuove callback definire ownership insieme al contratto. |
| [`linenoiseAddCompletion`](../src/linenoise.c#L995) | H | Copia/append con recovery OOM locale; confine appropriato. | Implementazione nuova deve usare policy alloc del progetto e destructor completo. |
| [`abInit`](../src/linenoise.c#L1027) | H | Setup buffer output storico; helper minimo. | Core usa ABUF_INIT; non creare seconda famiglia di buffer. |
| [`abAppend`](../src/linenoise.c#L1038) | H | Append fallibile che tronca output silenziosamente su OOM. | Non riusare: core ha allocator controllati e deve mantenere quella policy. |
| [`abFree`](../src/linenoise.c#L1053) | H | Rilascio semplice con nome risorsa. | Nessuna necessità distinta dal buffer frame corrente. |
| [`foldCountLines`](../src/linenoise.c#L1078) | H | Conteggio newline per placeholder fold isolato. | Fold non pianificati: nessuna utility nuova giustificata oggi. |
| [`shouldFoldText`](../src/linenoise.c#L1093) | H | Policy soglia/multilinea nominata e riusata. | Non introdurre fold dell'editor per riutilizzare questo predicato. |
| [`foldSetRenderedText`](../src/linenoise.c#L1103) | H | Costruisce etichetta del fold a partire da contenuto. | Mantenere solo storico; eventuale fold richiede modello documento diverso. |
| [`linenoiseBuildHistoryFold`](../src/linenoise.c#L1127) | H | Calcola intervallo con contesto per history; algoritmo specifico. | Non confondere history di righe con undo dell'editor. |
| [`linenoiseGetRenderFolds`](../src/linenoise.c#L1181) | H | Raccoglie fold validi per render/edit; confine interno utile. | Nuovi fold richiederebbero policy esplicita e nuovi test. |
| [`linenoiseRenderBuffer`](../src/linenoise.c#L1214) | H | Proiezione sorgente/fold e mapping cursore; separazione algoritmica forte. | Ispirazione per separare testo/render, non implementazione da trasferire. |
| [`linenoiseEditNextLen`](../src/linenoise.c#L1286) | H | Passo avanti aggrega fold e grapheme, diverso da semplice UTF-8. | Core senza fold deve usare utf8NextCharLen attuale. |
| [`linenoiseEditPrevLen`](../src/linenoise.c#L1305) | H | Passo inverso con policy fold autonoma. | Core usa utf8PrevCharLen attuale. |
| [`linenoiseFoldAdd`](../src/linenoise.c#L1324) | H | Inserimento ordinato con limite fold; primitive appropriata. | Nessuna necessità per nuove feature attualmente pianificate. |
| [`linenoiseFoldClear`](../src/linenoise.c#L1345) | H | Reset esplicito piccolo ma significativo. | Conservare storico, non importare stato fold nel core. |
| [`linenoiseFoldRemove`](../src/linenoise.c#L1355) | H | Compatta due array coordinati e count. | Per una nuova feature evitare array paralleli se non necessari. |
| [`linenoiseRangeOverlapsFold`](../src/linenoise.c#L1369) | H | Predicato range strutturale autonomo. | Non generalizzare range framework a partire da questo solo caso. |
| [`linenoiseAdjustFoldsAfterInsert`](../src/linenoise.c#L1386) | H | Trasla/invalida intervalli dopo insert; policy distinta. | Esempio di invalidazione derivata, non codice attualmente richiesto. |
| [`linenoiseAdjustFoldsAfterDelete`](../src/linenoise.c#L1408) | H | Policy di invalidazione dopo delete con semantica differente. | Eventuali decorazioni future richiedono propria semantica di range. |
| [`refreshShowHints`](../src/linenoise.c#L1432) | H | Disegno hint bounded e ownership callback separati. | Piccolo renderer dedicato soltanto se la feature hint viene approvata. |
| [`refreshSingleLine`](../src/linenoise.c#L1488) | H | Layout, clipping, mask, hints e output concentrati. | Core ha renderer fullscreen; trasferire invarianti width, non il ramo storico. |
| [`refreshMultiLine`](../src/linenoise.c#L1583) | H | Pulizia vecchie righe, geometry e output molto concentrati. | Non usarlo per il wrapping documento corrente. |
| [`refreshLineWithFlags`](../src/linenoise.c#L1697) | H | Dispatcher tra due modalità, helper appropriato internamente. | Nessun dispatcher aggiuntivo richiesto nel core. |
| [`refreshLine`](../src/linenoise.c#L1710) | H | Facade flags predefiniti, convenienza media. | Core non usa API linenoise; evitare wrapper equivalenti per inerzia. |
| [`linenoiseHide`](../src/linenoise.c#L1720) | H | Operazione di pulizia display per API asincrona. | Nessuna nuova astrazione hide/show senza UI asincrona richiesta. |
| [`linenoiseShow`](../src/linenoise.c#L1732) | H | Ridisegno consapevole di completion; non semplice alias. | La presentazione del frame corrente deve restare unificata. |
| [`linenoiseEditGrow`](../src/linenoise.c#L1747) | H | Capacità bounded e OOM recuperabile; confine corretto. | Principio utile al prompt, con teRealloc e policy del progetto. |
| [`linenoiseEditInsertNoRefresh`](../src/linenoise.c#L1788) | H | Separa mutazione e redraw; scelta interna giustificata. | Principio da applicare al batching attuale, senza riportare fold/history. |
| [`linenoiseEditInsert`](../src/linenoise.c#L1820) | H | Comando con fast-path output e fallback redraw. | Per core attuale privilegiare batching coerente prima di fast-path. |
| [`linenoiseEditMoveLeft`](../src/linenoise.c#L1855) | H | Comando movimento più refresh, piccolo e autonomo. | Core ha editorMoveCursor; nessuna duplice API da mantenere. |
| [`linenoiseEditMoveRight`](../src/linenoise.c#L1868) | H | Azione simmetrica con passo grapheme/fold. | Usare navigazione fullscreen corrente. |
| [`linenoiseEditMoveHome`](../src/linenoise.c#L1880) | H | Comando nominato con guardia no-op. | Home visual/logico corrente ha policy diversa. |
| [`linenoiseEditMoveEnd`](../src/linenoise.c#L1892) | H | Comando nominato, aggiornamento e refresh coerenti. | Core gestisce fine segmento/documento con layout dedicato. |
| [`linenoiseEditHistoryNext`](../src/linenoise.c#L1910) | H | History, resize, fold e redraw troppo accoppiati. | Non usarlo per multi-buffer: quello conserva documenti, non righe di history. |
| [`linenoiseEditDelete`](../src/linenoise.c#L1956) | H | Delete unità e invalidazione fold raccolti. | Core deve cancellare un range grapheme una volta con cache coerenti. |
| [`linenoiseEditBackspace`](../src/linenoise.c#L1973) | H | Delete inverso distinto dal forward delete. | Nessuna necessità di importazione; principi di ownership utili. |
| [`linenoiseEditDeletePrevWord`](../src/linenoise.c#L1991) | H | Azione parola con regola spazi semplice. | Se introdotta nel core, usare navigation policy corrente e undo atomico. |
| [`linenoiseEditStart`](../src/linenoise.c#L2016) | H | Session setup, raw mode, buffer e history insieme. | Startup attuale deve distinguere app/documento/input. |
| [`pasteBufferReserve`](../src/linenoise.c#L2064) | H | Grow bounded separato da append, gestione overflow/OOM. | Confine capacità utile per correggere prompt/paste, non dipendenza da linenoise. |
| [`pasteBufferAppend`](../src/linenoise.c#L2100) | H | Controlla limiti paste/editor prima della copia. | Nuovi limiti sono decisione di policy; preservare size_t ai confini byte. |
| [`linenoiseEditPaste`](../src/linenoise.c#L2122) | H | Protocollo, overflow, newline e fold concentrati. | Core ha terminalReadPastedText e replace-selection: mantenere quei confini. |
| [`linenoiseEditFeed`](../src/linenoise.c#L2210) | H | Dispatcher storico esteso con parsing, completion e mutazioni. | Non reintegrarlo; stessa ragione per dividere dispatcher attuale. |
| [`linenoiseEditStop`](../src/linenoise.c#L2426) | H | Cleanup della sessione interattiva separato dal feed. | Il core usa cleanup terminale atexit e lifecycle documento. |
| [`linenoiseBlockingEdit`](../src/linenoise.c#L2445) | H | Adatta API feed a loop sincrono con ownership buffer. | Non reintegrare per prompt: significherebbe due stack input/termios. |
| [`linenoisePrintKeyCodes`](../src/linenoise.c#L2475) | H | Strumento diagnostico autonomo e utile per vedere byte reali. | Concetto utile per verifiche con utente, preferire logger mirato separato. |
| [`linenoiseReadLine`](../src/linenoise.c#L2508) | H | Lettura dinamica standalone con error propagation. | Nuovo loader deve distinguere EOF/errori e usare policy alloc attuale. |
| [`linenoiseNoTTY`](../src/linenoise.c#L2560) | H | Facade della lettura semplice; granularità interna di convenienza. | Non amplia automaticamente tinyedit a editing non-TTY. |
| [`linenoise`](../src/linenoise.c#L2577) | H | Entry point con routing TTY/fallback, coeso per libreria. | Nessuna necessità nel binario fullscreen. |
| [`linenoiseFree`](../src/linenoise.c#L2614) | H | Rilascio con riconoscimento sentinella; non è semplice free. | Nuove API devono evitare confusione dati/sentinel se non necessaria. |
| [`freeHistory`](../src/linenoise.c#L2627) | H | Distruttore history globale, confine risorsa legittimo. | Undo corrente ha historyClear per documento. |
| [`linenoiseAtExit`](../src/linenoise.c#L2643) | H | Coordina raw cleanup e history, hook legittimo. | Core ha propri handler; non sovrapporre lifecycle. |
| [`linenoiseHistoryAdd`](../src/linenoise.c#L2662) | H | Inserimento/dedup/limite della history delle righe. | Non sostituisce historyRecordEdit; modelli differenti. |
| [`linenoiseHistorySetMaxLen`](../src/linenoise.c#L2702) | H | Resize e rilascio elementi eccedenti sono policy coese. | Nessuna necessità per undo depth corrente senza revisione della rappresentazione. |
| [`linenoiseHistorySave`](../src/linenoise.c#L2737) | H | Persistenza history e permessi autonomi. | Non unificare con salvataggio documento; formato/policy differenti. |
| [`linenoiseHistoryLoad`](../src/linenoise.c#L2767) | H | Lettura history e decodifica newline autonome. | Nessun requisito corrente di history prompt persistente. |

### src/menu.c — 12 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`menuFirstItem`](../src/menu.c#L47) | M | Salta i separatori e definisce il focus iniziale. | Conservare finché le definizioni restano statiche e non vuote. |
| [`menuNextItem`](../src/menu.c#L60) | A | Navigazione circolare che evita separatori. | Validare menu senza elementi se si introducono voci dinamiche. |
| [`menuStartColumn`](../src/menu.c#L74) | M | Posizione comune a draw e hit testing. | Se layout diventa adattivo, usare geometria condivisa per entrambe. |
| [`menuWidth`](../src/menu.c#L87) | A | Misura unica di label/shortcut/checkmark. | Con nuovi testi usare larghezza Unicode e clipping viewport. |
| [`menuAppendAt`](../src/menu.c#L107) | M | Incapsula posizione ANSI e callback di output. | Confine utile senza imporre un framework di rendering. |
| [`menuAppendRule`](../src/menu.c#L122) | M | Bordi ripetuti e posizionamento raccolti. | Mantenere privato; eventuale tema modifica un punto. |
| [`menuInit`](../src/menu.c#L135) | M | Inizializzazione semanticamente distinta da apertura. | Piccola duplicazione con menuOpen accettabile. |
| [`menuOpen`](../src/menu.c#L147) | M | Azione di apertura con reset focus. | Conservare per input da tastiera e mouse. |
| [`menuHandleKey`](../src/menu.c#L160) | A | Traduce navigazione menu in comando, senza eseguirlo. | Buon modello per altre UI: risultato esplicito invece di tasto sintetico. |
| [`menuHandleMouse`](../src/menu.c#L197) | A | Hit testing e selezione menu autonomi. | Con clipping serve un rettangolo comune al disegno. |
| [`menuDrawBar`](../src/menu.c#L237) | A | Barra separata dal popup; callback limita l'accoppiamento. | Adatta alla build vanilla come feature esclusa. |
| [`menuDrawPopup`](../src/menu.c#L264) | A | Disegno popup coeso; non va spezzato per ogni elemento. | Bounding/clipping se voci o viewport diventano dinamiche. |

### src/render.c — 11 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`renderGutterWidth`](../src/render.c#L15) | A | Regola di larghezza unica per buffer/view. | Conservare per navigazione, mouse e disegno. |
| [`renderTextCols`](../src/render.c#L32) | A | Sottrae il gutter e protegge la larghezza disponibile. | Viewport esplicita favorisce sidebar e split futuri. |
| [`renderSoftWrapCols`](../src/render.c#L44) | A | Centralizza cap/margine e minimo; contratto distinto. | Non confondere minimo positivo con capacità di contenere glyph larghi. |
| [`renderRowSegments`](../src/render.c#L58) | A | Cache di segmenti e doppia unità byte/colonna essenziali; correggere avanzamento. | Unico algoritmo di wrap per ogni vista; cache per larghezza da rivedere con viste multiple. |
| [`renderSegmentVisibleEnd`](../src/render.c#L123) | A | Fine byte senza spazi, utilizzata per slicing. | Conservare distinta dalla fine in colonne. |
| [`renderSegmentVisibleEndRx`](../src/render.c#L136) | A | Fine visiva distinta dai byte; policy trailing spaces. | Allineare al layout Unicode/tab unico. |
| [`renderRxToSegment`](../src/render.c#L155) | A | Traduzione colonna → segmento e offset locale. | Confine utile per cursore e hit testing. |
| [`renderRowVideoHeight`](../src/render.c#L171) | M | Nomina la misura logica → video con policy no-wrap. | Conservare come punto d'accesso alla cache. |
| [`renderVideoRowOf`](../src/render.c#L182) | A | Trasforma posizione logica in coordinata video. | Se profiling lo richiede, aggiungere indice delle altezze qui. |
| [`renderFileRowAtVideoRow`](../src/render.c#L196) | A | Conversione inversa con gestione EOF/empty. | Definire esplicitamente fallback dell'ultimo segmento; test su viewport vuote. |
| [`renderTotalVideoRows`](../src/render.c#L219) | A | Conteggio condiviso che può popolare cache. | Calcolare una volta per frame; invalidazione con modifiche/larghezza. |

### src/settings.c — 20 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`settingSlot`](../src/settings.c#L176) | A | Concentra offset e requisito int32_t dei campi. | Conservare vincolo dimensioni; accessor pubblico solo se evita duplicazione reale. |
| [`settingSlotConst`](../src/settings.c#L186) | A | Mantiene const-correctness dell'accesso generico. | Non fondere con cast che eliminino const. |
| [`settingsFind`](../src/settings.c#L196) | A | Ricerca descriptor utilizzata dai comandi. | Riutilizzarla nel parser invece di mantenere ricerca equivalente. |
| [`settingsGetBool`](../src/settings.c#L210) | A | Verifica tipo e restituisce booleano del setting. | Contratto stabile per menu e altre UI. |
| [`settingsToggleBool`](../src/settings.c#L222) | A | Verifica tipo e modifica solo lo slot pertinente. | Conservare separata da persist/apply. |
| [`settingsDefaults`](../src/settings.c#L235) | A | Unico punto di reset/default, semplice anche se lungo. | Allineare nuovi campi/descriptor con test di copertura; non un helper per assegnazione. |
| [`configPath`](../src/settings.c#L282) | M | Nome config e troncamento uniformi per read/write. | Tenere privata salvo esigenza concreta di path condiviso. |
| [`enumIndexOf`](../src/settings.c#L296) | M | Ricerca nome → enum senza duplicare nel parser. | Conservare per nuove opzioni enum. |
| [`settingColorFromLegacyName`](../src/settings.c#L326) | A | Compatibilità storica isolata dalla palette corrente. | Rimuovere solo con scelta esplicita di fine compatibilità. |
| [`trim`](../src/settings.c#L339) | M | Normalizzazione parser locale, piccola e riusata. | Non forzare utility condivisa con syntax: ownership differente. |
| [`addFiletypeOverride`](../src/settings.c#L354) | A | Replace/append e ownership del registry centralizzati. | Valutare registry esplicito per reload, non per multi-buffer da solo. |
| [`settingsSetFiletype`](../src/settings.c#L379) | M | API validata per chiamanti esterni; non semplice alias. | Preservare separazione modifica registry/persistenza. |
| [`freeFiletypeOverrides`](../src/settings.c#L390) | A | Distruzione del registry prima del reload. | Conservare se si introduce caricamento esplicito di configurazioni. |
| [`settingsLoad`](../src/settings.c#L407) | A | Parsing piccolo con default/compatibilità/override; divisione ulteriore non urgente. | Accessor/parse valore condivisi se aumenta la varietà delle opzioni. |
| [`settingsSave`](../src/settings.c#L487) | A | Serializer e commit atomico della config; confine necessario. | Policy di durabilità e dati sconosciuti da decidere se si amplia il formato. |
| [`filetypeForExtension`](../src/settings.c#L543) | A | Ordine override/builtin in un punto. | Normalizzare confronto estensioni se si allinea alla syntax case-insensitive. |
| [`ansiColorCode`](../src/settings.c#L569) | A | Mappa enum → sequenza foreground, riusata. | Conservare o sostituire con tabella, senza motore temi anticipato. |
| [`ansiBgColorCode`](../src/settings.c#L631) | A | Background ha semantica diversa da foreground/dim. | Conservare distinta e testare palette se estesa. |
| [`settingColorIsDim`](../src/settings.c#L671) | M | Predicato policy palette usato dalla UI. | Resta utile per filtrare scelte background. |
| [`settingColorName`](../src/settings.c#L681) | B | Getter pubblico senza uso applicativo corrente. | Conservare solo per presentazione concreta/test dell'API; bassa priorità. |

### src/syntax.c — 32 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`syntaxExtensionEquals`](../src/syntax.c#L145) | A | Confronto ASCII case-insensitive unico nel modulo. | Candidato condiviso reale con detection editor, evitando dipendenza circolare. |
| [`syntaxIsAsciiLetter`](../src/syntax.c#L169) | M | Policy ASCII esplicita evita dipendenza dalla locale. | Conservare come predicato lessicale, non sostituire con isalpha senza criterio. |
| [`syntaxLangForFilename`](../src/syntax.c#L182) | A | Precedenza user/builtin e estrazione estensione coese. | Risolvere una volta per documento se prestazioni/reload lo richiedono. |
| [`syntaxUserLangForExtension`](../src/syntax.c#L222) | A | Lookup user condiviso da capability e label. | Conservare come nucleo unico dei due getter. |
| [`syntaxUserLangHasExtension`](../src/syntax.c#L241) | M | Facade booleana impedisce esposizione della struct lingua. | Utile alla segnalazione config mancante. |
| [`syntaxUserFiletypeForExtension`](../src/syntax.c#L251) | M | Facade label con ownership borrowed chiara. | Resta pertinente al registry filetype. |
| [`syntaxDupTrimmed`](../src/syntax.c#L292) | A | Normalizzazione con nuova ownership, diversa da trim in-place. | Riutilizzare nel parser; evitare copia intermedia se si accetta uno span. |
| [`syntaxSplitList`](../src/syntax.c#L314) | A | Parsing liste e ownership meritano confine dedicato. | Aggiungere distruttore completo per sostituzioni/errore/reload. |
| [`syntaxParseLangFile`](../src/syntax.c#L353) | S | Troppi campi, validation e trasferimenti ownership nel medesimo flusso. | Separare draft parse/validate/destroy; utile a nuove grammatiche e reload. |
| [`syntaxLoadUserLangs`](../src/syntax.c#L486) | A | Scansione directory distinta dal parsing di un file. | Reload e lifetime espliciti soltanto se richiesti. |
| [`isWordBoundary`](../src/syntax.c#L530) | A | Una policy di keyword boundary per il tokenizer. | Conservare con test sui byte UTF-8 e identificatori. |
| [`syntaxCharLenAt`](../src/syntax.c#L541) | A | Passo bounded condiviso per scanner di testo. | Usarlo uniformemente quando si attraversano caratteri, distinguendo fill di hl byte-wise. |
| [`syntaxIsIdentifierByte`](../src/syntax.c#L556) | A | Regola lessicale con prefissi e posizione iniziale. | Nuovi linguaggi estendono la policy senza toccare disegno. |
| [`syntaxHighlightQuoted`](../src/syntax.c#L572) | A | Scanner quote condiviso fra generic e CSS. | Base utile per tokenizer Python dedicato; triple quote richiedono stato distinto. |
| [`matchKeyword`](../src/syntax.c#L601) | A | Matching e boundary condivisi fra lingue tabellari. | Ottimizzare soltanto con misure su liste grandi. |
| [`syntaxHighlightRowGeneric`](../src/syntax.c#L618) | S | Ordine lessicale coeso, ma comment/math/number/identifier concentrati. | Estrarre scanner con risultato/progresso chiaro se si amplia; mantenere precedenze. |
| [`isMdExtension`](../src/syntax.c#L806) | M | Nomina famiglia Markdown in detection/dispatcher. | Conservare piccolo predicato; no registry universale necessario. |
| [`syntaxTryHighlightDelimited`](../src/syntax.c#L825) | A | Algoritmo delimitatori condiviso per math. | Mantenere privato finché altre classi non condividono esattamente il contratto. |
| [`syntaxTryHighlightMath`](../src/syntax.c#L859) | A | Unifica formati math per generic e Markdown. | Conservare separato dalla gestione stato multilinea. |
| [`syntaxIsFrontMatterFence`](../src/syntax.c#L901) | A | Riconoscimento strutturale della fence autonomo. | Utile a tokenizer Markdown, senza parser YAML completo. |
| [`syntaxHighlightFrontMatterLine`](../src/syntax.c#L924) | A | Grammatica metadata distinta dagli inline Markdown. | Conservare come scanner limitato e documentato. |
| [`syntaxTryHighlightMarkdownTag`](../src/syntax.c#L973) | A | Subscanner tag evita crescita dell'inline principale. | Condividere soltanto regole XML realmente identiche. |
| [`syntaxTryHighlightMarkdownLink`](../src/syntax.c#L1045) | A | Link annidati e avanzamento sono responsabilità autonoma. | Aggiungere limiti/prove nesting se input arbitrari diventano requisito robustezza. |
| [`syntaxHighlightRowMarkdown`](../src/syntax.c#L1104) | S | Stati di blocco e più scansioni inline disallineate nello stesso corpo. | Separare block-state e scanner inline unico, utile anche a fence future. |
| [`isXmlExtension`](../src/syntax.c#L1355) | M | Predicato famiglia markup usato da più decisioni. | Allineare detection auto-close senza duplicare estensioni. |
| [`syntaxTryHighlightTemplateBlock`](../src/syntax.c#L1368) | A | Configurazione delimitatori e classe token isolati dall'XML. | Punto naturale per nuove template definitions. |
| [`syntaxHighlightRowXml`](../src/syntax.c#L1413) | S | Tag/attribute/quote/comment coesi ma più stati interni e scanner parziali. | Separare attributi/stringhe quando si introducono multilinea o annidamenti reali. |
| [`isCssExtension`](../src/syntax.c#L1528) | M | Piccola policy di dispatcher/capability. | Nessun nuovo modulo giustificato per questo solo predicato. |
| [`syntaxHighlightRowCss`](../src/syntax.c#L1538) | A | Scanner compatto per grammatica CSS, riusa quoted. | Mantenere locale; estrarre solo subscanner con nuovo riuso. |
| [`syntaxHighlightRow`](../src/syntax.c#L1613) | A | Facade che seleziona grammatica e gestisce stato/cache. | Sostituire molti flag con stato lessicale esplicito se cresce; supportare esclusione vanilla. |
| [`syntaxColorFor`](../src/syntax.c#L1688) | A | Classi lessicali indipendenti dalla palette configurata. | Confine utile per temi; escludibile nella build senza syntax. |
| [`syntaxHasBuiltinExtension`](../src/syntax.c#L1720) | A | Capability check coerente con dispatcher dedicati/tabellari. | Tenere allineato al registry quando si aggiungono grammatiche. |

### src/terminal.c — 24 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`terminalDie`](../src/terminal.c#L65) | A | Errore fatale con exit che lascia eseguire cleanup. | Conservare ma distinguere errori recuperabili di documenti. |
| [`terminalDisableRawMode`](../src/terminal.c#L76) | A | Ripristino termios separato e registrabile atexit. | Cleanup dovrebbe gestire errore senza rientrare in exit ricorsivamente. |
| [`terminalRestoreVisualState`](../src/terminal.c#L87) | A | Ripristino screen/attributi/cursore con guardia di stato. | Conservare unico punto al termine di ogni sessione. |
| [`terminalEnterAlternateScreen`](../src/terminal.c#L100) | A | Attivazione e registrazione cleanup autonome. | Stessa coppia anche con schermate nuove. |
| [`terminalEnableRawMode`](../src/terminal.c#L113) | A | Configura POSIX e salva originale senza accoppiarsi all'editing. | Base di portabilità; nessun backend Windows anticipato. |
| [`terminalDisableBracketedPaste`](../src/terminal.c#L150) | A | Hook cleanup dedicato, pur essendo una singola write. | Conservare perché atexit richiede un'operazione distinta. |
| [`terminalEnableBracketedPaste`](../src/terminal.c#L161) | A | Attiva protocollo e ne registra il cleanup. | Mantenere distinto dal consumo del payload. |
| [`terminalDisableKittyKeyboard`](../src/terminal.c#L178) | A | Pop condizionale del protocollo, ownership dello stato. | Necessario per toggling runtime e uscita. |
| [`terminalEnableKittyKeyboard`](../src/terminal.c#L190) | A | Push idempotente con cleanup registrato una volta. | Conservare se si espande la tastiera estesa. |
| [`terminalConfigureGhosttyCommandBindings`](../src/terminal.c#L212) | S | Ricerca config, rewrite file e reload UI mescolati. | Separare locator/rewrite/reload in area specifica Ghostty, mantenendo policy attuale. |
| [`terminalReloadGhosttyConfiguration`](../src/terminal.c#L311) | A | Processo osascript distinto dalla modifica file. | Resta integrazione specifica della piattaforma; non generalizzare process API ora. |
| [`terminalRemoveGhosttyCommandBindings`](../src/terminal.c#L337) | M | Adattatore atexit senza argomenti; semanticamente necessario. | Conservare o sostituire con cleanup sessione equivalente. |
| [`terminalInputReady`](../src/terminal.c#L358) | A | Polling POSIX isolato dalla policy di drenaggio mouse. | Non consumare eventi qui; introdurre queue/pending nel livello input. |
| [`terminalDisableMouseReporting`](../src/terminal.c#L387) | A | Ripristina modalità mouse e override shift. | Necessario come hook runtime/atexit, anche se piccolo. |
| [`terminalEnableMouseReporting`](../src/terminal.c#L397) | A | Attivazione distinta dal reporting hover del menu. | Evitare registrazioni cleanup ripetute se molte attivazioni runtime. |
| [`terminalSetMenuMouseMotion`](../src/terminal.c#L408) | A | Switch protocollo specifico, separato dalla decisione menu. | Conservare con input mouse e menu opzionale. |
| [`handleWinch`](../src/terminal.c#L420) | A | Handler minimo signal-safe: modifica solo sig_atomic_t. | Conservare senza inserire rendering o allocazioni. |
| [`terminalEnableResizeHandling`](../src/terminal.c#L431) | A | Installazione handler separata dal consumo del flag. | Verificare esito sigaction se si rafforza avvio/diagnostica. |
| [`editorDrainUnknownCsiSequence`](../src/terminal.c#L465) | A | Recovery bounded del parser impedisce residui interpretati come testo. | Con parser unificato diventa stato/operazione equivalente. |
| [`terminalCsiUSuperShortcut`](../src/terminal.c#L480) | A | Policy Command/Super confinata e testabile. | Mapparla a comandi quando input/eventi saranno separati. |
| [`terminalReadKey`](../src/terminal.c#L509) | S | Acquisizione, pending, timeout, CSI/SS3/CSI-u e mouse nello stesso corpo. | Separare decoding su byte bounded da I/O; preservare sequenze verificate reali. |
| [`terminalReadPastedText`](../src/terminal.c#L850) | A | Payload paste ha protocollo/ownership distinti dai tasti. | Definire gestione letture parziali/timeouts prima di riusarlo per input più complesso. |
| [`getCursorPosition`](../src/terminal.c#L911) | A | Fallback protocollo query distinto da ioctl. | Conservare privato; validare risposta e letture brevi. |
| [`terminalGetWindowSize`](../src/terminal.c#L932) | A | Facade dimensioni con fallback; non dipende dal layout editor. | Riutilizzabile per sidebar/nuove schermate. |

### src/tinyedit.c — 142 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`editorPrimaryModifier`](../src/tinyedit.c#L188) | M | Policy UI Ctrl/Cmd usata in più messaggi. | Conservare come helper presentazione; separarla dai binding effettivi. |
| [`editorShortcutText`](../src/tinyedit.c#L201) | A | Trasformazione bounded condivisa fra splash/help/info/settings. | Se shortcut diventano dati strutturati, generare testo da quei dati. |
| [`editorReadKey`](../src/tinyedit.c#L226) | M | Adattatore che lega impostazione macOS e firma condizionale. | Con eventi espliciti spostare policy nella sessione terminale. |
| [`editorRowCxToRx`](../src/tinyedit.c#L241) | M | Adatta tab_stop corrente alla conversione condivisa. | Con document/view espliciti passare contesto, senza duplicare algoritmo. |
| [`editorRowRxToCx`](../src/tinyedit.c#L251) | M | Adatta tab_stop corrente al glyph contenente la colonna. | Mantenere contratto rounding e parametrizzare con nuove viste. |
| [`editorMatchingPairAtCursor`](../src/tinyedit.c#L268) | A | Scansione bilanciamento autonoma; più lunga ma coesa. | Estrarre dal globale per test puri; cache solo dopo profiling. |
| [`editorRehighlightFrom`](../src/tinyedit.c#L341) | A | Propaga stato lessicale fino a stabilizzazione. | Riutilizzare come commit degli aggiornamenti batch. |
| [`editorUpdateRow`](../src/tinyedit.c#L368) | S | Espansione tab, invalidazione wrap, tokenizer e propagazione insieme; unità incoerenti. | Separare layout coerente e syntax, con aggiornamento batch e build vanilla. |
| [`editorUpdateAllRows`](../src/tinyedit.c#L427) | M | Nomina ricostruzione globale per impostazioni/undo. | Evitare propagazioni replicate; rebuild layout e passata syntax ordinate. |
| [`editorInsertRow`](../src/tinyedit.c#L437) | A | Buffer insert + cache + dirty: aggiunge contratto reale. | Conservare livello documento, rendendo esplicito il proprietario. |
| [`editorDelRow`](../src/tinyedit.c#L450) | A | Delete strutturale + propagazione syntax + dirty. | Riutilizzarlo o equivalente nel protocollo batch. |
| [`editorRowInsertChar`](../src/tinyedit.c#L463) | A | Primitive byte più aggiornamento stato; non puro alias. | Ridurne l'uso per span multibyte; un solo refresh per operazione. |
| [`editorRowInsertString`](../src/tinyedit.c#L475) | A | Inserimento span più cache/dirty e guardia no-op. | Base preferibile a loop di insert char per future feature. |
| [`editorRowAppendString`](../src/tinyedit.c#L488) | A | Join/append con effetti derivati centralizzati. | Conservare semantica o integrarla nel layer batch. |
| [`editorRowDelChar`](../src/tinyedit.c#L500) | M | Wrapper delete byte + rebuild; granularità troppo fine per grapheme. | Sostituire nel percorso Unicode con delete range unico. |
| [`editorPushUndo`](../src/tinyedit.c#L517) | M | Adatta profondità e tempo live al modulo history. | Con azioni esplicite evitare che grouping temporale definisca atomicità. |
| [`editorUndo`](../src/tinyedit.c#L527) | A | Comando: history, ripristino cache e feedback UI. | Mantenere come azione utente con documento esplicito. |
| [`editorRedo`](../src/tinyedit.c#L545) | A | Comando autonomo simmetrico, leggibile. | Condividere solo ricostruzione comune se ne cresce il corpo. |
| [`editorInsertCharRaw`](../src/tinyedit.c#L565) | A | Mutazione senza snapshot e aggiornamento cursore. | Convertire in inserimento span per evitare transienti UTF-8/rebuild ripetuti. |
| [`editorInsertChar`](../src/tinyedit.c#L577) | A | Confine undo della digitazione semplice. | Non riusarlo per ogni byte di un'azione composta. |
| [`editorInsertNewlineRaw`](../src/tinyedit.c#L588) | A | Split/inserimento di riga senza snapshot intermedio. | Riutilizzabile nel batch text/replace con cache differite. |
| [`editorInsertNewlineAutoIndent`](../src/tinyedit.c#L617) | A | Azione Enter e indent ereditato con un solo snapshot. | Condividere calcolo/applicazione indent con Enter su selezione. |
| [`editorDelChar`](../src/tinyedit.c#L647) | S | Grapheme/join corretti come responsabilità, ma delete/rebuild per byte. | Usare range unico e mantenere comando Backspace distinto dal buffer. |
| [`editorEffectiveLineEnding`](../src/tinyedit.c#L680) | A | Policy setting explicit versus detection auto. | Conservare per save/statistiche/recovery. |
| [`editorRowsToString`](../src/tinyedit.c#L692) | M | Adattatore che impone policy disco e newline finale. | Legarlo al documento esplicito nel lifecycle futuro. |
| [`editorPromptDisplayText`](../src/tinyedit.c#L720) | A | Rappresentazione dei controlli/spazi distinta dal testo reale. | Con modello prompt separato conserva render non distruttivo. |
| [`editorPromptAppend`](../src/tinyedit.c#L749) | A | Confine capacità/lunghezza/NUL opportuno ma oggi errato. | Correggere teRealloc e overflow prima del completion dei percorsi. |
| [`editorPromptCB`](../src/tinyedit.c#L774) | S | Loop input/render/clipboard/callback e stato ricerca globale mescolati. | Separare modello testo e risposta eventi; Backspace per grapheme, completion locale. |
| [`editorPrompt`](../src/tinyedit.c#L865) | M | Facade semplice nasconde callback opzionali a Open/Save. | Mantenere fino a API prompt con opzioni esplicite realmente necessarie. |
| [`editorClearRows`](../src/tinyedit.c#L880) | M | Clear del buffer con reset cursore, distinto da reset documento. | Lifecycle esplicito evitando reset duplicati. |
| [`editorLoadLines`](../src/tinyedit.c#L892) | A | Caricamento bytes recovery distinto da filesystem. | Condividere splitter/terminatori con loader file e ricostruire metadata. |
| [`editorResolveFiletype`](../src/tinyedit.c#L934) | A | Coordina nome syntax e registry, con persistenza intenzionale. | Ridurre effetti impliciti se si introduce reload/config temporanea. |
| [`editorWarnMissingHighlightConfig`](../src/tinyedit.c#L962) | A | Diagnostica distinta dal rilevamento e dall'apertura. | Conservare come policy UI opzionale. |
| [`editorOpen`](../src/tinyedit.c#L981) | S | Ownership nome, I/O, ending detection, righe e UI nello stesso flusso. | Loader transazionale con esito; distinguere EOF/errore e terminatore esatto. |
| [`editorMaybeBackup`](../src/tinyedit.c#L1044) | A | Policy dirty/intervallo separata dal backup I/O. | Timer/event loop se si vuole periodicità anche senza tasti; esito fallimento esplicito. |
| [`editorRecoveryScreenLine`](../src/tinyedit.c#L1065) | M | Riga centrata/stilizzata riusata dalla schermata recovery. | Clamp Unicode se si mostrano nomi arbitrari; no widget framework. |
| [`editorRecoveryScreen`](../src/tinyedit.c#L1083) | A | Disegno dell'avviso distinto dalla decisione di recovery. | Render con geometria comune alle altre schermate. |
| [`editorOfferBackupRecovery`](../src/tinyedit.c#L1121) | A | Coordina domanda, lettura e sostituzione documento. | Commit recovery transazionale e metadata coerenti. |
| [`editorWriteAll`](../src/tinyedit.c#L1151) | A | Write/EINTR/partial handling isolati. | Riutilizzabile per output frame solo dopo decisione della policy errori. |
| [`editorAtomicSave`](../src/tinyedit.c#L1168) | A | I/O temp/mode/fsync/rename coeso, già separato dalla UI. | Modulo I/O se loader viene estratto; preservare symlink/mode e durabilità. |
| [`editorSaveInternal`](../src/tinyedit.c#L1225) | S | Prompt, cambio identità, syntax, I/O e cleanup backup insieme. | Nome e metadata committati solo al successo; risultato esplicito al chiamante. |
| [`editorSave`](../src/tinyedit.c#L1270) | M | Nome comando leggibile con policy normale. | Mantenere come entry point per shortcut/menu. |
| [`editorSaveAs`](../src/tinyedit.c#L1280) | M | Nome comando distinto con richiesta obbligatoria del path. | Mantenere nel command executor, anche se chiama il medesimo nucleo. |
| [`editorDiffersFromDisk`](../src/tinyedit.c#L1304) | A | Confronto contenuto per evitare richiesta save dopo undo. | Se si introduce revision salvata, preservare semantica sulle modifiche esterne. |
| [`editorConfirmDocumentChange`](../src/tinyedit.c#L1340) | A | Policy save/discard/cancel condivisa da open/close/quit. | Riutilizzabile per singolo documento con esito save esplicito. |
| [`editorResetDocument`](../src/tinyedit.c#L1374) | S | Cleanup documento, view, search e backup concentrati e duplicati con init. | Separare lifecycle documento e reset sessione/vista; base multi-buffer. |
| [`editorCloseFile`](../src/tinyedit.c#L1411) | A | Azione utente con conferma/reset e feedback. | Applicarla al documento selezionato in futuro. |
| [`editorOpenFile`](../src/tinyedit.c#L1430) | S | Prompt, probe, distruzione vecchio e riapertura introducono race. | Caricare temporaneo e sostituire solo al successo; base sidebar/completion. |
| [`editorQuit`](../src/tinyedit.c#L1476) | A | Conferma e uscita sono azione autonoma. | Con multi-buffer iterare conferme con policy esplicita, senza anticiparla ora. |
| [`abAppend`](../src/tinyedit.c#L1494) | A | Unico append frame; proprietà del buffer centralizzata. | Aggiungere capacità/overflow guard se profiling o robustezza lo richiedono. |
| [`editorMenuAppend`](../src/tinyedit.c#L1508) | A | Adattatore di firma callback: collega menu senza esporre implementazione. | Conservare finché API output menu usa callback/context. |
| [`abFree`](../src/tinyedit.c#L1518) | M | Rilascio leggibile accanto alla risorsa frame. | Non prioritario accorparlo; reset se si riutilizza abuf. |
| [`abAppendReset`](../src/tinyedit.c#L1536) | A | Reset attributi con ripristino background, diversa da ESC[m isolato. | Conservare per evitare leak visivi con nuove decorazioni. |
| [`editorGutterWidth`](../src/tinyedit.c#L1555) | M | Adatta buffer e flag settings a layout. | Parametrizzare quando viewport/documento non sono globali. |
| [`editorTextCols`](../src/tinyedit.c#L1565) | M | Adatta view/buffer e flag del gutter. | Un contesto layout sostituirà wrapper globali se necessario. |
| [`editorSoftWrapCols`](../src/tinyedit.c#L1583) | M | Adatta settings/view al contratto condiviso. | Usare risultato unico per frame, non reinterpretare soft_wrap come bool. |
| [`editorRowSegments`](../src/tinyedit.c#L1595) | B | Inoltra identicamente argomenti e risultato a renderRowSegments. | Chiamata diretta al modulo; nessuna policy futura dimostrata. |
| [`editorSegVisibleEnd`](../src/tinyedit.c#L1605) | B | Inoltra identicamente tutti gli argomenti. | Chiamata diretta, conservando distinzione byte/colonna. |
| [`editorSegVisibleEndRx`](../src/tinyedit.c#L1615) | M | Wrapper aggiunge tab_stop corrente; valore di adattatore reale. | Passare contesto layout se cambia la struttura delle viste. |
| [`editorRxToSegment`](../src/tinyedit.c#L1631) | B | Wrapper trasparente senza policy o stato aggiunto. | Eliminabile durante modifica dell'area. |
| [`editorVideoRowOf`](../src/tinyedit.c#L1641) | M | Adatta buffer globale alla conversione condivisa. | Parametrizzare per documento/view espliciti. |
| [`editorFileRowAtVideoRow`](../src/tinyedit.c#L1651) | M | Adatta buffer globale e conserva output multipli. | Conservare contratto; diretto quando buffer diventa argomento. |
| [`editorTotalVideoRows`](../src/tinyedit.c#L1660) | M | Adatta buffer corrente al conteggio layout. | Cache per frame/documento invece di nuove chiamate ripetute. |
| [`editorMouseToCursor`](../src/tinyedit.c#L1682) | A | Conversione completa screen → documento, distinta da parser mouse. | Viewport rettangolare per sidebar e test click su wide/tab. |
| [`editorScroll`](../src/tinyedit.c#L1734) | A | Policy vista segue cursore/free scroll e margine. | Rendere esplicite coordinate e stato view per buffer switching. |
| [`editorDrawRowSegment`](../src/tinyedit.c#L1813) | S | Mappatura sorgente/render, priorità highlight, invisibili e emissione insieme. | Unificare layout/tab; contesto highlight invece di quattordici parametri. |
| [`editorRowTerminatorHighlighted`](../src/tinyedit.c#L1934) | A | Policy newline selezionato/match distinta da glyph normali. | Conservare per range multilinea e nuove decorazioni. |
| [`editorDrawHighlightedTerminator`](../src/tinyedit.c#L1947) | M | Emissione e reset del glyph EOL condivisi. | Riutilizzare finché selection e search condividono stile. |
| [`editorDrawGutter`](../src/tinyedit.c#L1961) | A | Disegno numero/continuazione distinto dal testo. | Conservare con geometry esplicita nelle nuove viste. |
| [`editorChooseSlogan`](../src/tinyedit.c#L1989) | M | Policy scelta e non ripetizione autonoma; coesa. | Non trasformarla in modulo random generale senza altri requisiti. |
| [`editorDrawSplashRow`](../src/tinyedit.c#L2037) | A | Splash per righe visibili separato dal documento. | Precalcolare larghezza per frame; non serve framework di schermate. |
| [`editorDrawRows`](../src/tinyedit.c#L2093) | S | Iterazione viewport e conversioni ripetute mescolate al disegno. | Scansione progressiva dei segmenti visibili; totale calcolato una volta. |
| [`editorCountChars`](../src/tinyedit.c#L2202) | A | Conteggio grapheme riusato da status/info; algoritmo chiaro. | Spostare nel buffer/cache con invalidazione sulle modifiche se misurato utile. |
| [`editorFiletypeLabel`](../src/tinyedit.c#L2230) | M | Getter nome tipo dal file corrente; logica estensione ripetuta altrove. | Unificare detection filename, non inventare registry globale aggiuntivo. |
| [`editorDrawTopBar`](../src/tinyedit.c#L2255) | A | Elemento UI autonomo; correggere clamp snprintf. | Primitive bounded comune per testi arbitrari/temi. |
| [`editorDrawStatusBar`](../src/tinyedit.c#L2285) | A | Elemento UI coeso, ma statistiche e byte-width hanno limiti. | Separare raccolta dati e layout se si personalizza la barra. |
| [`editorDrawMessageBar`](../src/tinyedit.c#L2337) | A | Scadenza, tail-scroll e hint menu hanno policy locale. | Scroll/clipping per grapheme e spazio riservato al menu. |
| [`editorRefreshScreen`](../src/tinyedit.c#L2375) | S | Resize, scroll, frame, overlay, geometria cursore e write nello stesso corpo. | Preparazione geometry → draw → present; niente accesso I/O nei calcoli puri. |
| [`editorSetStatusMessage`](../src/tinyedit.c#L2451) | A | Messaggio temporaneo aggiorna testo, tempo e policy. | Condividere nucleo va_list con sticky solo se riduce manutenzione. |
| [`editorSetStatusMessageSticky`](../src/tinyedit.c#L2472) | A | Policy persistente distinta e usata nei messaggi iniziali. | Conservare entry point nominata; evitare bool opaco ai chiamanti. |
| [`editorSegColToCx`](../src/tinyedit.c#L2496) | A | Conversione navigazione che arrotonda dopo glyph, diversa dal click. | Non fonderla ciecamente con RxToCx; esplicitare rounding in layout comune. |
| [`editorMoveCursorWrapped`](../src/tinyedit.c#L2527) | A | Movimento verticale video distinto da logical rows. | Conservare come algoritmo testabile con view esplicita. |
| [`editorMoveCursor`](../src/tinyedit.c#L2570) | A | Movimento base con confini EOF e grapheme; coeso. | Separare dalla selezione; centralizzare validazione del cursore. |
| [`editorMoveCursorWord`](../src/tinyedit.c#L2634) | A | Regola salto parole distinta da singolo grapheme. | Non introdurre Unicode word segmentation completa senza requisito. |
| [`editorSerializeRange`](../src/tinyedit.c#L2686) | M | Adatta documento corrente alla serializzazione clipboard. | Chiamata diretta quando il documento diventa argomento dei comandi. |
| [`editorDeleteRangeRaw`](../src/tinyedit.c#L2696) | A | Delete multilinea e riposizionamento senza snapshot; contratto utile. | Nucleo documento batch con propagazione differita. |
| [`editorDeleteRange`](../src/tinyedit.c#L2726) | A | Wrapper aggiunge confine undo; non trasparente. | Conservare azione autonoma o equivalente command transaction. |
| [`editorReplaceSelectionWithNewline`](../src/tinyedit.c#L2737) | A | Delete+Enter con indent sono una singola azione. | Condividere indent con Enter normale senza duplicare snapshot. |
| [`editorInsertTextRaw`](../src/tinyedit.c#L2767) | A | Split LF e inserimento multilinea senza snapshot intermedi. | Batch layout/syntax; policy CRLF paste distinta da loader se necessaria. |
| [`editorReplaceSelectionWithText`](../src/tinyedit.c#L2791) | A | Unifica Ctrl-V e bracketed paste, garantendo uno snapshot. | Conservare come nucleo anche per replacement futuri. |
| [`editorRowOutdent`](../src/tinyedit.c#L2818) | A | Adatta primitive a cache/dirty e restituisce delta. | Mantenere nel layer documento, non nel dispatcher. |
| [`editorIndentSelection`](../src/tinyedit.c#L2840) | A | Azione blocco con riaggiustamento range e un undo. | Batch span/righe; un helper per endpoints solo se riusato. |
| [`editorDecodeRegexReplacement`](../src/tinyedit.c#L2930) | A | Sintassi replacement diversa dalla query; ownership autonoma. | Conservare separata; gruppi catturati richiedono nuova decisione. |
| [`editorDecodeRegexPattern`](../src/tinyedit.c#L2962) | A | Normalizzazione escapes query prima della compilazione. | Policy esplicita per CR→LF; non fonderla con replacement genericamente. |
| [`editorSearchText`](../src/tinyedit.c#L3001) | M | Serializza tutto per regex multilinea; duplica parte delle altre serializzazioni. | Un solo serializer LF con NUL, mantenendo il contratto distinto dal disco. |
| [`editorSearchOffsetForPosition`](../src/tinyedit.c#L3025) | A | Mappa coordinate logiche a offset nella stringa LF. | Base motore ricerca; indice cumulativo se profiling lo richiede. |
| [`editorSearchPositionForOffset`](../src/tinyedit.c#L3038) | A | Conversione inversa necessaria per match multilinea. | Gestire empty esplicitamente; restituzione risultato con posizione valida. |
| [`editorRegexFindLastInText`](../src/tinyedit.c#L3059) | A | Ricerca inversa tramite scansione avanti, algoritmo autonomo. | Unificare con versione riga attraverso span e size_t. |
| [`editorRegexFindLastOnRow`](../src/tinyedit.c#L3086) | B | Duplica ricerca inversa sul testo con int32_t e erow. | Delegare al nucleo span unico con conversioni controllate. |
| [`editorFindFrom`](../src/tinyedit.c#L3151) | S | Compilazione, algoritmo, wrap e mutazione cursore/highlight mescolati. | Query compilata/sessione separata e match restituito senza UI. |
| [`editorFindCallback`](../src/tinyedit.c#L3293) | S | Gestione input/sessione e last_* esterni alla struct search. | Raccogliere stato e separare move/apply; test sulla sessione reale. |
| [`editorFindModeIndicator`](../src/tinyedit.c#L3422) | M | Callback leggibile che nomina modo corrente. | Conservare come presentazione, senza far conoscere E.search al prompt generico. |
| [`editorFind`](../src/tinyedit.c#L3432) | A | Azione avvio sessione, prompt e passaggio replace. | Sessione esplicita e documento selezionato per multi-buffer. |
| [`editorFindAndReplace`](../src/tinyedit.c#L3471) | S | Prompt, conferme, ricerca, loop progresso e mutazione nello stesso flusso. | Separare piano di avanzamento dalla UI; mantenere undo unico e no-wrap. |
| [`settingsScreenSlot`](../src/tinyedit.c#L3581) | B | Replica accessor offsetof del modulo settings. | Usare un accesso condiviso appropriato o confinare UI con accessor; mantenere int32_t. |
| [`editorSettingsIsSyntaxColor`](../src/tinyedit.c#L3591) | M | Predicato di visibilità/categoria riusato. | Metadata categoria nel descriptor se sostituisce dipendenza dal nome chiave. |
| [`editorSettingsSyntaxColorSample`](../src/tinyedit.c#L3601) | M | Campioni locali raccolti senza allungare draw row. | Metadati preview quando aumentano le categorie; restare UI, non parser. |
| [`editorSettingsPlainColorSample`](../src/tinyedit.c#L3620) | M | Campioni semplici con contratto diverso dai syntax. | Unificare dati preview solo se semplifica i branch esistenti. |
| [`editorSettingsIsColor`](../src/tinyedit.c#L3636) | M | Classificazione palette leggibile per render. | Descriptor esplicito se crescono opzioni non palette con prefisso color_. |
| [`editorSettingsVisibleCount`](../src/tinyedit.c#L3647) | A | Filtro di visibilità distinto dalla navigazione. | Conservare policy condivisa con mapping visible→descriptor. |
| [`editorSettingsDescriptorAt`](../src/tinyedit.c#L3662) | A | Indice visibile distinto dall'indice dati; confine necessario. | Lista di indici precalcolata solo se serve o riduce duplicazioni. |
| [`editorSettingsLabelWidth`](../src/tinyedit.c#L3677) | A | Misura allineamento rispettando visibilità/indent. | Larghezza Unicode se label diventano arbitrarie/localizzate. |
| [`editorSettingsCycleEnum`](../src/tinyedit.c#L3705) | A | Ciclo e salto dim su background sono policy reale. | Spostare policy su descriptor soltanto con più vincoli analoghi. |
| [`editorSettingsDrawRow`](../src/tinyedit.c#L3732) | S | Formattazione value/label e molte preview speciali insieme. | Separare formatter e preview; nuovi temi modificano un solo livello. |
| [`editorHelpScreen`](../src/tinyedit.c#L3857) | S | Loop scroll e disegno help nello stesso corpo. | Separare render/evento se cresce; geometry/resize coerenti con altre schermate. |
| [`editorInfoAppendLine`](../src/tinyedit.c#L3951) | A | Append formattato bounded con contatore righe; utile e riusato. | Modello per rendere sicuri altri append formattati. |
| [`editorInfoAppendSection`](../src/tinyedit.c#L3970) | M | Stile e contatore sezione centralizzati. | Tenere privata, non introdurre API universale di sezioni. |
| [`editorInfoAppendBlank`](../src/tinyedit.c#L3984) | M | Riga vuota e contatore come operazione semantica. | Accorpabile ma nessun beneficio prioritario. |
| [`editorInfoScreen`](../src/tinyedit.c#L4003) | A | Schermata dati coesa grazie agli helper; non dividere campo per campo. | Clipping/scroll condivisi se aumentano informazioni o viewport molto piccole. |
| [`editorSettingsVisibleRows`](../src/tinyedit.c#L4084) | M | Unica policy altezza pannello con minimo. | Allineare alla geometry totale quando menu/top bar cambiano. |
| [`editorSettingsRender`](../src/tinyedit.c#L4095) | A | Disegno riusato da loop e mini-prompt int: estrazione giustificata. | Conservare distinto dal controller del pannello. |
| [`editorSettingsEditInt`](../src/tinyedit.c#L4169) | A | Editor numerico locale che ridisegna la stessa schermata. | strtol validato al posto di atoi; non riusare prompt documento che cambia la vista. |
| [`editorSettingsSave`](../src/tinyedit.c#L4210) | S | Applica runtime, aggiorna cache, protocolli e persist con feedback. | Separare applySettings da persist/UI; rollback solo se policy richiesta. |
| [`editorSettingsScreen`](../src/tinyedit.c#L4262) | S | Controller navigazione/edit/draft/conferma abbastanza esteso. | Estrarre dall'editor core mantenendo render dedicato e stato esplicito. |
| [`editorAutoCloseFor`](../src/tinyedit.c#L4391) | B | Seconda tabella/lookup che replica editorAutoClosePairFor. | Delegare alla fonte unica; wrapper solo per adattare single_quote live. |
| [`editorFilenameHasExtension`](../src/tinyedit.c#L4409) | M | Lookup case-insensitive ma duplica policy del modulo syntax. | Unificare in utilità di filename piccola se realmente condivisa. |
| [`editorIsXmlTagFile`](../src/tinyedit.c#L4428) | M | Policy estensioni auto-close distinta dal tokenizer. | Conservare predicato; base_tokenizer xml non implica autorizzare auto-close a tutti i template. |
| [`editorIsHtmlFile`](../src/tinyedit.c#L4439) | M | Distingue policy void HTML dall'XML. | Conservare perché cambia comportamento, non solo etichetta. |
| [`editorIsXmlNameStart`](../src/tinyedit.c#L4450) | A | Regola lessicale iniziale separata dal resto del nome. | Documentare limiti ASCII/locale e nuove regole solo se richieste. |
| [`editorIsXmlNameByte`](../src/tinyedit.c#L4460) | A | Regola nome continuazione differente dall'inizio. | Conservare, condividere con syntax solo se grammatiche equivalenti. |
| [`editorIsHtmlVoidTag`](../src/tinyedit.c#L4470) | A | Eccezioni HTML centralizzate in tabella. | Resta privata dell'assistenza editing. |
| [`editorTryAutoCloseXmlTag`](../src/tinyedit.c#L4490) | A | Riconoscimento e inserimento closing tag, funzione autonoma. | Separare riconoscimento puro se si amplia; non implementare parser HTML completo. |
| [`editorReadMultiByteKey`](../src/tinyedit.c#L4549) | A | Raccoglie sequenza e preserva il tasto non-continuation. | Spostare nel layer input affinché UTF-8 funzioni indipendentemente da auto-close. |
| [`editorTrySkipMultiByteClose`](../src/tinyedit.c#L4587) | A | Policy closer Unicode distinta da trasporto input. | Conservare nel nucleo auto-close unico con snapshot solo se muta testo. |
| [`editorInsertCharAutoClose`](../src/tinyedit.c#L4614) | S | ASCII/Unicode, selezione, skip, dollari/tag e inserimento nello stesso corpo. | Riconoscimento puro + azione atomica; mantenere eccezioni Markdown/quote deliberate. |
| [`editorMenuCommandKey`](../src/tinyedit.c#L4781) | M | Bridge pratico menu→shortcut ma introduce dipendenza inversa dai tasti. | Executor comandi comune a input/menu; eliminarlo allora, non aggiungere altri bridge. |
| [`editorToggleMenuSetting`](../src/tinyedit.c#L4808) | A | Copy/toggle/apply e chiusura menu hanno effetti reali. | Restare comando che usa applySettings separato. |
| [`editorSyncMenuMouseMotion`](../src/tinyedit.c#L4821) | A | Transizione idempotente di protocollo secondo stato menu. | Conservare come collegamento UI→terminale, con cleanup coerente. |
| [`editorProcessKeypress`](../src/tinyedit.c#L4834) | S | Dispatcher contiene quasi tutte le policy applicative e mouse. | Estrarre handler comandi/mouse/movimento+selezione; conservare eventi non mouse già letti. |
| [`editorFreeUndoRedo`](../src/tinyedit.c#L5415) | M | Adattatore atexit senza argomenti sopra historyClear. | Sostituire con lifecycle sessione/documenti quando necessario. |
| [`initEditor`](../src/tinyedit.c#L5425) | S | Init documento/UI/settings/menu/geometry nello stesso flusso. | Condividere inizializzazione documento con reset, distinto da app startup. |
| [`main`](../src/tinyedit.c#L5476) | A | Orchestrazione avvio/main loop appropriata, non troppo granulare. | Integrare eventi/timer e sessione esplicita quando richiesti, senza frammentare ogni chiamata. |

### src/utf8.c — 15 funzioni

| Funzione | Grado | Valutazione attuale | Prospettiva / decisione |
|---|:---:|---|---|
| [`utf8ByteLen`](../src/utf8.c#L22) | A | Stima lead byte distinta dalla validazione del decoder. | Conservare per acquisizione bounded; non trattarla come validazione completa. |
| [`utf8DecodeChar`](../src/utf8.c#L39) | A | Contratto rigoroso available/consumed/valid; essenziale per tutto il testo. | Fonte unica anche per nuovi tokenizer e primitive layout. |
| [`isVariationSelector`](../src/utf8.c#L88) | A | Proprietà Unicode nominata e riusata. | Conservare range/policy documentati. |
| [`isSkinToneModifier`](../src/utf8.c#L98) | A | Proprietà emoji distinta da combining marks. | Conservare per grouping/width coerenti. |
| [`isZWJ`](../src/utf8.c#L108) | A | Predicato piccolo ma esprime regola condivisa di composizione. | Mantenere anche con miglioramento delle euristiche grapheme. |
| [`isRegionalIndicator`](../src/utf8.c#L118) | A | Regola bandiere distinta, condivisa avanti/indietro. | Prove su sequenze lunghe per coerenza del pairing. |
| [`isCombiningMark`](../src/utf8.c#L129) | A | Range supportati centralizzati per grouping/width. | Aggiornare policy qui; non dichiarare conformità Unicode completa. |
| [`isGraphemeExtend`](../src/utf8.c#L143) | A | Compone proprietà usate dai due versi di navigazione. | Conservare come policy, senza duplicare classificazione nei chiamanti. |
| [`utf8DecodePrev`](../src/utf8.c#L155) | A | Ricerca suffix bounded distinta dal forward decoder. | Conservare per recupero non distruttivo su byte malformati. |
| [`utf8PrevCharLen`](../src/utf8.c#L174) | A | Passo grapheme all'indietro, indipendente da UI. | Base per delete/cursore/prompt; ampliare test simmetria senza fondere i due versi. |
| [`utf8NextCharLen`](../src/utf8.c#L243) | A | Passo grapheme bounded avanti con recupero byte singolo. | Fonte unica per layout/split non ASCII. |
| [`utf8CharWidth`](../src/utf8.c#L302) | A | Larghezza codepoint distinta da sequenza/grapheme. | Aggiornare euristiche secondo requisiti terminali reali. |
| [`ansiEscapeLen`](../src/utf8.c#L364) | A | Misura escape bounded per stringhe decorate. | Tenere privata a utf8StrWidth finché non c'è parsing ANSI condiviso reale. |
| [`utf8StrWidth`](../src/utf8.c#L381) | A | Primitive utile, oggi usata nei test ma non nel core attivo. | Riutilizzarla dove si corregge layout UI; distingue width da strlen. |
| [`utf8SingleCharWidth`](../src/utf8.c#L430) | A | Larghezza di un'unità con fallback invalid coerente. | Conservare e usarla nel layout unificato. |

## Controllo di copertura e limiti

Inventario verificato: 398 righe individuali, 313 attive e 85 storiche; nessuna definizione applicativa omessa. Distribuzione attiva: A: 205, M: 71, B: 11, S: 26. Le categorie sono giudizi progettuali, non soglie automatiche.

Questa è una revisione architetturale e mirata di robustezza, non una prova esaustiva dell’assenza di bug, una misura di copertura test o un benchmark. I casi confermati sono distinti dalle osservazioni statiche; le proposte future dipendono dalla scelta delle feature. Nessuna libreria esterna, nuovo protocollo tasti o implementazione di feature è stata introdotta.
