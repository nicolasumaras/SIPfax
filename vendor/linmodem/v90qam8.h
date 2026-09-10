/* 7200/3200 mapping frames after symbol/trellis decoding. GPL-2.0. */
#ifndef V90QAM8_H
#define V90QAM8_H
#include "v90shell.h"
typedef struct { V90Shell shell; unsigned previous; } V90Qam8Frames;
void v90_qam8_frames_init(V90Qam8Frames *s,unsigned previous_quadrant);
/* Eight time-ordered quadrant|(ring<<2) labels -> 18 scrambled bits.
 * Caller establishes frame alignment and differential state. Rejected frames
 * leave output untouched; valid labels still advance differential history so
 * a bad shell does not corrupt the next frame. Invalid labels reset nothing. */
int v90_qam8_frame(V90Qam8Frames *s,const uint8_t labels[8],uint8_t bits[18]);
#endif
