#ifndef SIPFAX_V34_E_H
#define SIPFAX_V34_E_H
/* V.34 10.1.3.11: E consists of 20 descrambled binary ones. Parameters
   must already have passed their frame CRC before E can start B1 reception. */
static inline int v34_e_bit(int *ones, int bit, int valid_mp)
{
    if (!bit) *ones = 0;
    else if (*ones < 20) ++*ones;
    return valid_mp && *ones == 20;
}
#endif
