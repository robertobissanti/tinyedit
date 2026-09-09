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

- **Tipi a dimensione esplicita, uniformi per concetto** (obbligatorio,
  vale per ogni nuovo codice C in questo progetto, non solo per il
  refactoring che l'ha introdotto):
  - Ogni indice/lunghezza/dimensione della famiglia "posizione nel
    file o nello schermo" (cursore, numero di righe, larghezza/altezza
    schermo, offset di scroll, lunghezze di `erow`, colonne di wrap
    ecc.) è **`int32_t`**, con segno — mai `int` semplice. Il segno è
    necessario perché il codice usa sentinelle `-1` (es.
    `search_match_y`) e confronta/sottrae indici e size direttamente
    senza cast; un unico tipo evita cast signed/unsigned sparsi.
  - Ogni flag booleano (0/1) è **`uint8_t`**, mai `int`.
  - Byte grezzi letti da terminale o buffer UTF-8 (`editorReadKey` e
    affini) sono **`uint8_t`**; i codepoint Unicode decodificati sono
    **`uint32_t`** (vedi `utf8.h`).
  - Lunghezze/offset di byte passati a `malloc`/`memcpy`/`strlen` e
    funzioni POSIX restano **`size_t`** (non forzare a `int32_t`: sono
    per contratto non negative e servono a interagire con l'API di
    libreria, non con gli indici dell'editor).
  - **Eccezione documentata**: `struct editorSettings` (`settings.h`)
    ha *tutti* i campi a `int32_t`, inclusi i booleani come
    `show_line_numbers`/`show_top_bar` — non `uint8_t` come il resto
    del progetto. È un vincolo tecnico: `settingSlot()`/
    `settingsScreenSlot()` accedono ai campi genericamente via
    `offsetof` castato a `int32_t*`, e questo richiede che ogni campo
    occupi la stessa dimensione. Non "correggere" questi campi a
    `uint8_t` senza riscrivere anche l'accessor generico.
  - Le firme imposte da API esterne (`int main(int argc, char **argv)`,
    `void handleWinch(int sig)` per `sigaction`, file descriptor `int`
    da `open()`) restano al tipo richiesto dal linguaggio/POSIX, non
    vanno convertite.
  - **Priorità sulle regole in conflitto**: questa regola non deve MAI
    produrre più cast di quanti ce ne fossero senza di essa. Se rendere
    due valori dello stesso tipo "family" (es. un indice e la size con
    cui si confronta) richiederebbe introdurre un cast che prima non
    serviva, la coerenza di tipo perde e si tiene il tipo che elimina
    il cast — l'obiettivo di fondo è ridurre cast e uso di
    memoria/cache/registri, non applicare l'etichetta "int32_t" ovunque
    a prescindere. In pratica: prima verificare se un cast sopravvive
    al cambio di tipo o ne nasce uno nuovo, e se sì, riconsiderare il
    tipo scelto per quella variabile.
- **Manipolazione di caratteri/testo: sempre tramite le funzioni
  portate da linenoise in `utf8.c`/`utf8.h`** (`utf8ByteLen`,
  `utf8DecodeChar`, `utf8PrevCharLen`, `utf8NextCharLen`,
  `utf8CharWidth`, `utf8StrWidth`, `utf8SingleCharWidth`), mai indicizzando
  `row->chars`/`row->render` byte per byte a mano o assumendo che un
  byte corrisponda a un carattere/colonna. **Motivo**: `render` e
  `chars` contengono UTF-8 raw — un carattere può occupare 1-4 byte ma
  1-2 colonne di schermo (vedi il bug del wrap corretto in
  `TODO.md`: `editorRowSegments()` calcolava `seg_start[]` come offset
  byte ma veniva confrontato con `rx`, una colonna, e i due divergono
  su qualunque riga con caratteri non-ASCII prima di un punto di
  wrap). Quando serve sia l'offset byte (per indicizzare/copiare
  `render`/`chars`) sia la colonna equivalente (per confronti con
  `E.rx`/`E.cx` "visivi"), calcolarli **entrambi** esplicitamente
  camminando col passo (`clen`) restituito da `utf8NextCharLen`/
  `utf8PrevCharLen`, mai assumendo che coincidano.
- **Zero dipendenze esterne** oltre alla libreria standard POSIX/C99.
  Non introdurre librerie di terze parti (niente ncurses, niente
  framework TUI) senza discuterne esplicitamente prima — è una scelta
  di design deliberata del progetto, non una svista.
- Un modulo per responsabilità chiara (es. `clipboard.c`/`utf8.c` sono
  separati dall'editor core) ma senza over-engineering: se una feature è
  piccola e specifica di `tinyedit.c`, non serve un file a parte.
- **Ogni allocazione dinamica deve gestire il fallimento** (obbligatorio e
  critico): nel codice applicativo usare esclusivamente `teMalloc()`,
  `teRealloc()` e `teStrdup()` da `alloc.c`/`alloc.h`, mai chiamare
  direttamente `malloc()`/`realloc()`/`strdup()`. Gli helper terminano in
  modo controllato e lasciano eseguire gli handler `atexit()` che ripristinano
  raw mode, mouse, bracketed paste e stato visivo del terminale. Non assegnare
  mai il risultato di un `realloc()` fallibile direttamente all'unico
  puntatore valido: si perderebbe il buffer originale in caso di errore. Se
  un'API deve invece poter recuperare da OOM, deve documentarlo e usare un
  puntatore temporaneo con propagazione esplicita dell'errore.
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
