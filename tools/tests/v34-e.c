#include <assert.h>
#include <stdio.h>
#include "v34e.h"
int main(void)
{
    int n = 0;
    for (int i = 0; i < 19; ++i) assert(!v34_e_bit(&n, 1, 1));
    assert(v34_e_bit(&n, 1, 1));
    assert(!v34_e_bit(&n, 0, 1));
    for (int i = 0; i < 100; ++i) assert(!v34_e_bit(&n, 1, 0));
    assert(n == 20);
    for (int interrupted = 0; interrupted < 20; ++interrupted) {
        n = 0;
        for (int i = 0; i < interrupted; ++i) assert(!v34_e_bit(&n, 1, 1));
        assert(!v34_e_bit(&n, 0, 1));
        for (int i = 0; i < 19; ++i) assert(!v34_e_bit(&n, 1, 1));
        assert(v34_e_bit(&n, 1, 1));
    }
    puts("PASS: E requires 20 consecutive ones and CRC-validated MP; zero resets the run");
}
