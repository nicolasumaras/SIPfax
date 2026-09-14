#ifndef V90LINEECHO_H
#define V90LINEECHO_H
#include <stdint.h>
#define V90_LINE_ECHO_TAPS 65
#define V90_LINE_ECHO_RING 8192
typedef struct {
 double history[V90_LINE_ECHO_RING],coefficients[V90_LINE_ECHO_TAPS];
 uint64_t tx_samples,rx_samples;
 unsigned delay,enabled;
} V90LineEcho;
int v90_line_echo_init(V90LineEcho *,unsigned);
void v90_line_echo_tx(V90LineEcho *,int16_t);
int16_t v90_line_echo_rx(V90LineEcho *,int16_t);
#endif
