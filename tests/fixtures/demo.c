#include <stdint.h>

/* Commento multilinea:
 * naïve café 日本語 */
int32_t main(void) {
    const char *message = "quote escaped: \"ok\"";
    int32_t valore = 42;
    return message[0] == '\0' ? valore : 0;
}
