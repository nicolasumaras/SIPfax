/* Passive PPP echo health observer; sample clock is 8 kHz. GPL-2.0. */
#ifndef V90ECHO_H
#define V90ECHO_H
#include <stdint.h>
typedef struct {
    int armed,fired,started,escape,overflow;
    unsigned length,crc,count;
    uint8_t prefix[16],pending[256];
    uint32_t magic[256];
    int64_t sent[256],first,last;
} V90Echo;
void v90_echo_init(V90Echo *s);
void v90_echo_pause(V90Echo *s);
void v90_echo_tx(V90Echo *s,unsigned byte,int64_t now);
void v90_echo_rx(V90Echo *s,const uint8_t *frame,unsigned length,int64_t now);
int v90_echo_due(V90Echo *s,int64_t now);
#endif
