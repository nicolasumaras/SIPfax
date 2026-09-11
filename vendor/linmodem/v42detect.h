/* V.42 detection-phase helpers; no LAPM implementation. GPL-2.0. */
#ifndef V42DETECT_H
#define V42DETECT_H
#include <stdint.h>
typedef struct {
 unsigned uart_bits,byte,marks,gap,previous,run,seen;
} V42Detect;
/* Feed descrambled synchronous bits. Returns 1 only on the first four
 * alternating-parity DC1 characters, separated by 8..16 mark bits.
 * Zero initialization resets the detector. The caller owns startup gating. */
int v42_detect_bit(V42Detect *,unsigned);
/* Ten E/NUL ADPs with eight marks between characters (V.42 Table 3).
 * n=0..359 returns the unscrambled bit; n>=360 returns -1. This declines
 * modem error correction; it does not advertise LAPM capability. */
int v42_decline_bit(unsigned n);
#endif
