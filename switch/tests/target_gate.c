#include <assert.h>
#include "../horizon/src/hz_target_gate.h"

int main(void)
{
    target_gate gate = {0};
    for (unsigned i = 1; i <= 100; ++i)
        assert(!target_is_steady(&gate, 6400, 4500, i*150000000ull));
    for (unsigned i = 101; i < 180; ++i)
        assert(!target_is_steady(&gate, 6400, i%2 ? 6290 : 6580, i*150000000ull));
    for (unsigned i = 180; i < 280; ++i)
        assert(!target_is_steady(&gate, 6400, 6487, i*150000000ull));
    assert(target_is_steady(&gate, 6400, 6490, 280*150000000ull));
    assert(!target_is_steady(&gate, 6400, 6490, 300*150000000ull));
    assert(!target_is_steady(&gate, 6400, 8000, 301*150000000ull));
    for (unsigned i=1; i<=20; ++i)
        assert(!target_is_steady(&gate, 6400, 6400, (50ull+i)*1000000000ull));
}
