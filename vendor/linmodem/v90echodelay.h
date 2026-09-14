#ifndef V90ECHODELAY_H
#define V90ECHODELAY_H
#include "v90lineecho.h"
#define V90_ECHO_WINDOW 1024
#define V90_ECHO_MAX_DELAY 7000
#define V90_ECHO_SEARCH_BUDGET 256
typedef struct {
 double received[V90_ECHO_WINDOW],window[V90_ECHO_WINDOW];
 double reference[V90_ECHO_MAX_DELAY+V90_ECHO_WINDOW],scores[V90_ECHO_MAX_DELAY+1];
 double energy;
 uint64_t samples,last_scan,lock_sample;
 unsigned searching,minimum,next,candidate,locked,delay,scans;
} V90EchoDelay;
void v90_echo_delay_init(V90EchoDelay *);
void v90_echo_delay_rx(V90EchoDelay *,int16_t);
/* Call once per received audio block, before appending its transmitted block.
 * At most V90_ECHO_SEARCH_BUDGET lags are evaluated per call. */
int v90_echo_delay_step(V90EchoDelay *,V90LineEcho *);
#endif
