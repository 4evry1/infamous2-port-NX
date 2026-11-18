#include "hz_fps_hud.h"
#include <assert.h>
int main(void)
{
    hz_fps_window w = {0};
    assert(hz_fps_update(&w, 100, 100, 200) && !w.valid);
    assert(!hz_fps_update(&w, 500000100, 104, 207));
    assert(hz_fps_update(&w, 1000000100, 108, 215));
    assert(w.valid && w.base_tenths == 80 && w.output_tenths == 150);
    assert(hz_fps_update(&w, 2250000100, 118, 225));
    assert(w.base_tenths == 80 && w.output_tenths == 80);
    assert(hz_fps_update(&w, 3250000100, 118, 226));
    assert(w.base_tenths == 0 && w.output_tenths == 10);
    assert(hz_fps_update(&w, 1, 0, 0) && !w.valid);
    return 0;
}
