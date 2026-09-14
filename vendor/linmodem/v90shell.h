/* V.34 shell mapping shared by V.90 upstream rates. GPL-2.0. */
#ifndef V90SHELL_H
#define V90SHELL_H
#include <stdint.h>
#define V90_SHELL_MAX_M 18
#define V90_SHELL_SUM (8 * (V90_SHELL_MAX_M - 1))
typedef struct {
    unsigned m, k;
    uint64_t count[4][V90_SHELL_SUM + 1];
    uint64_t prefix[V90_SHELL_SUM + 2];
} V90Shell;
/* Initialize before use. Functions return 1 on success; encode/decode leave
 * output unchanged on rejection. Ring order is the eight transmitted symbols. */
int v90_shell_init(V90Shell *s, unsigned m, unsigned k);
int v90_shell_encode(const V90Shell *s, uint32_t index, uint8_t rings[8]);
int v90_shell_decode(const V90Shell *s, const uint8_t rings[8], uint32_t *index);
#endif
