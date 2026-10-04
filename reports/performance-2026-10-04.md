# Misure di scalabilità — 2026-10-04

Benchmark del core effettivo, eseguito su questa macchina con compilazione
`-O2` e `clock()` (tempo CPU); nessun output verso terminale durante le misure.
Riproduzione: `make benchmark`. Fixture: 1.000, 10.000 e 50.000 righe identiche
con ASCII, accenti, CJK e un emoji composto; viewport 80×40, soft wrap 72,
scroll a metà documento. Cache di wrap preparate prima della misura.
Il matching collega un'apertura sulla prima riga alla chiusura sull'ultima.

Ogni tempo di draw/conteggio/pair è la media di 20 chiamate consecutive.
Dopo il fix conteggio/pair includono **una chiamata senza cache e 19 riusi**:
il miglioramento non rappresenta 20 scansioni complete più veloci.
Gli snapshot sono cinque cicli di copia e rilascio. Le misure sono indicative,
non un limite di latenza, non RSS e non tempi di avvio/caricamento o I/O.

| Righe | Draw prima → dopo (ms) | Conteggio prima → dopo (ms) | Pair prima → dopo (ms) | Snapshot dopo (ms) | Payload per snapshot |
|---:|---:|---:|---:|---:|---:|
| 1.000 | 0,2528 → 0,0551 | 0,3976 → 0,00005 | 0,4617 → 0,0178 | 0,0244 | 94.002 B |
| 10.000 | 1,2942 → 0,0630 | 3,4330 → 0,00040 | 3,5499 → 0,1788 | 0,2558 | 940.002 B |
| 50.000 | 6,0927 → 0,0995 | 17,0998 → 0,00185 | 17,6656 → 0,8861 | 1,2220 | 4.700.002 B |

Dati grezzi: [prima](performance-2026-10-04-before.csv),
[dopo](performance-2026-10-04-after.csv).

## Cambiamenti giustificati dalle misure

- Disegno wrapped: una sola ricerca del primo segmento visibile, poi avanzamento
  sequenziale. Eliminati totale e conversione ripetuti per ogni riga schermo.
- Conteggio: grapheme per riga calcolati quando si ricostruisce il render;
  totale del documento calcolato una volta dopo invalidazione e poi riusato.
  Totali oltre il massimo rappresentabile sono saturati a `INT32_MAX`.
- Pair: risultato riusato finché testo e posizione del cursore sono invariati.
- Buffer di output: crescita geometrica, invece di una riallocazione per append.

## Limiti residui e decisioni

Il disegno wrapped richiede ancora una scansione del prefisso prima della
viewport. Scroll, navigazione e conversioni riga/video mantengono percorsi
lineari. Il primo matching di parentesi resta una scansione del testo;
muovere il cursore o modificare il documento richiede un nuovo calcolo.
Il primo totale dopo una modifica somma i conteggi di tutte le righe.

Undo continua a copiare l'intero documento. Nella fixture a 50.000 righe,
200 snapshot (profondità predefinita) hanno circa **940 MB di payload teorico**,
prima dell'overhead degli allocator e dello stato attivo. Nessuna riduzione
implicita della profondità: undo a delta/budget in byte è lavoro futuro distinto.
I vettori delle righe e dell'history continuano a crescere con riallocazioni:
il benchmark non misura carico strutturale, quindi non giustifica una riscrittura.

Verifiche di correttezza: frame wrapped confrontati byte per byte con il
percorso di riferimento su Unicode, tab, righe vuote, diverse larghezze e scroll
oltre EOF; invalidazione dei cache dopo edit/undo/reset; output incrementale.
Le allocazioni POSIX restano recuperabili: test fault injection per `getline`
(EIO/ENOMEM/EINTR) e `realpath` (ENOMEM/EACCES/ENOTDIR), con fallback solo ENOENT.
