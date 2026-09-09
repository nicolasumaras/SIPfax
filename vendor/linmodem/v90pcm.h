#ifndef V90PCM_H
#define V90PCM_H
#include "v90cp.h"
typedef struct {unsigned pp,scrambler,odd,last_sign;int magnitude[6];} V90ShapeFrame;
typedef struct {
    unsigned k,s,sr,width,depth,alaw,m[6],map[6][128];
    unsigned scrambler,odd,last_sign,q,t,queued;
    double a1,a2,b1,b2,x,y,v;
    V90ShapeFrame queue[8];
    void *opaque;int (*get_bit)(void *);
} V90Pcm;
int v90_pcm_level(int alaw,unsigned ucode);
/* Configure from a CRC-validated CP, reset every coding/filter memory. */
int v90_pcm_init(V90Pcm *s,const V90Cp *cp,int (*get_bit)(void *),void *opaque);
/* Renegotiation uses CPt's constellation/K with preceding data shaping. */
int v90_pcm_renegotiate(V90Pcm *s,const V90Cp *data,const V90Cp *training,int (*get_bit)(void *),void *opaque);
/* At a six-sample boundary, discard unsent lookahead and return input bits
 * to rewind in the caller. Preserve the shaping state of transmitted samples. */
unsigned v90_pcm_discard_lookahead(V90Pcm *s);
void v90_pcm_frame(V90Pcm *s,int16_t out[6]);
#endif
