# tinyedit — istruzioni di progetto

Editor di testo da terminale in C puro, stile kilo. Prima di lavorare su
questo progetto, leggi:

- [`README.md`](README.md) — cos'è, come si builda, struttura del codice
- [`TODO.md`](TODO.md) — lavoro pianificato, con le decisioni di design
  già prese in conversazioni precedenti e quelle ancora aperte
  (marcate "DA DECIDERE")
- [`IDEAS.md`](IDEAS.md) — feature future non prioritarie, per contesto
  su dove il progetto potrebbe andare senza impegnarsi ora

## Stile del codice

- **Zero dipendenze esterne** oltre alla libreria standard POSIX/C99.
  Non introdurre librerie di terze parti (niente ncurses, niente
  framework TUI) senza discuterne esplicitamente prima — è una scelta
  di design deliberata del progetto, non una svista.
- Un modulo per responsabilità chiara (es. `clipboard.c`/`utf8.c` sono
  separati dall'editor core) ma senza over-engineering: se una feature è
  piccola e specifica di `tinyedit.c`, non serve un file a parte.
- Commenti solo dove il *perché* non è ovvio dal codice (vedi stile già
  usato in `tinyedit.c`/`clipboard.c`): niente commenti che ripetono cosa
  fa una riga.

### Ordine di un file: include → define → tipi → globali → funzioni

Ogni modulo (`.c` + `.h`) segue questa sequenza fissa, dall'alto in
basso, senza eccezioni:

1. **`#include`** — prima le feature-test macro (`_DEFAULT_SOURCE` ecc.,
   devono restare prima di qualsiasi `#include` per contratto POSIX),
   poi gli header locali del progetto (il proprio `.h` per primo — così
   si verifica che sia self-contained — seguito dagli altri moduli da
   cui dipende), poi gli header di sistema. Vedi `tinyedit.c`,
   `clipboard.c`, `utf8.c` per l'esempio di riferimento: stesso ordine
   in tutti e tre.
2. **`#define`** — macro e costanti. Se una macro descrive uno stato
   condiviso col resto del progetto (es. dimensioni di buffer, tasti),
   va nell'header; se è puramente un dettaglio interno del `.c` (usata
   in una sola funzione), può restare nel `.c`.
3. **Tipi** (`enum`, `struct`, `typedef`) — **vanno nell'header**, non
   nel `.c`, anche per un singolo translation unit come `tinyedit.c`.
   Separare i tipi dalla logica che li usa rende il file più semplice da
   orientarsi (vedi `tinyedit.h` per l'esempio di riferimento: contiene
   solo macro/enum/struct, zero funzioni).
4. **Variabili globali/`static`** — dichiarate subito dopo gli include,
   prima di qualsiasi funzione. Raggruppate per area se sono più di
   una-due (vedi `E`, poi lo stato di ricerca in `tinyedit.c`).
5. **Funzioni** — nel `.c`, organizzate in sezioni commentate per area
   funzionale (`/* ---- terminal ---- */`, `/* ---- row operations ---- */`
   ecc., pattern già in uso).

Quando aggiungi un nuovo modulo o una nuova feature a un file esistente,
riporta la struttura a questo ordine invece di accodare in fondo — non
serve un refactoring dedicato ogni volta, ma la prossima modifica in
quell'area è il momento naturale per sistemarlo.

## Testare le feature da terminale (nota importante)

Le sequenze di tasti con modificatori (Ctrl, Alt, frecce con Shift)
**vanno verificate prima con l'utente reale**, non assunte: terminali
diversi (Ghostty, iTerm2, Terminal.app, terminali Linux) mandano byte
diversi per la stessa combinazione. Nel dubbio, chiedi all'utente di
lanciare un piccolo logger di tasti in raw mode (pattern già usato in
conversazioni precedenti) invece di indovinare la sequenza.

Nota anche una limitazione nota: i pty emulati via `pty.openpty()` di
Python su macOS scartano silenziosamente alcuni byte di controllo
(es. 0x11/0x13, XON/XOFF) prima che arrivino al processo — non è un bug
del codice se un test automatizzato con quell'harness non riesce a
inviare Ctrl-Q/Ctrl-S. Verificare in un terminale reale in quel caso.

## Uso di sub-agent e workflow multi-agente

Il `TODO.md` elenca diverse feature relativamente indipendenti (undo,
selezione, ricerca, config, ecc.). Se l'utente chiede di implementarne
più di una insieme, o di "completare il progetto" in un colpo solo,
valuta se proporre un'orchestrazione multi-agente (tool `Workflow`) per
lavorare in parallelo su feature che non si toccano a vicenda — ma
**non lanciarla di tua iniziativa**: descrivi il piano (quali feature in
parallelo, quali in sequenza per via delle dipendenze già annotate nel
TODO, stima approssimativa di quanti agenti) e aspetta conferma esplicita
dell'utente prima di invocare `Workflow`. Questo vale anche in sessioni
future — l'istruzione non abilita l'auto-lancio, serve comunque una
richiesta esplicita dell'utente in quella sessione.

Quando proponi la suddivisione, tieni conto delle dipendenze reali già
mappate in `TODO.md`: ad esempio selezione testo blocca Ctrl-C/X/V e
Ctrl-A, il file di config blocca la TUI delle impostazioni. Non proporre
feature dipendenti in agenti paralleli che si scriverebbero addosso sullo
stesso file contemporaneamente.
