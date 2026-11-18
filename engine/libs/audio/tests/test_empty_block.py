"""Exercise the production ring selection and sample check with host PCM buffers."""
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / "cellAudio.c").read_text()
start = source.index("static int audio_next_block_empty(void)\n{")
function = source[start:source.index("\n}", start) + 2]
helper = source.index("static int audio_block_has_signal(")
function = source[helper:source.index("\n}", helper) + 2] + "\n" + function
harness = """
#include <assert.h>
#include <pthread.h>
typedef unsigned u32;
#define CELL_AUDIO_BLOCK_SAMPLES 256
#define CELL_AUDIO_PORT_MAX 2
#define mutex_lock pthread_mutex_lock
#define mutex_unlock pthread_mutex_unlock
static pthread_mutex_t s_audio_mutex = PTHREAD_MUTEX_INITIALIZER;
typedef struct { int in_use, running; struct { u32 nChannel, nBlock; } param;
                 float *buffer; u32 read_index; } AudioPortSlot;
static AudioPortSlot s_ports[2];
""" + function + """
int main(void) {
    float pcm[2 * 256 * 8] = {0};
    assert(!audio_next_block_empty());
    for (unsigned channels = 2; channels <= 8; channels *= 2) {
        unsigned n = channels * 256;
        s_ports[1] = (AudioPortSlot){1, 1, {channels, 2}, pcm, 3};
        pcm[0] = 1;
        assert(audio_next_block_empty());
        for (unsigned i = 0; i < n; ++i) {
            pcm[n+i] = -0.25f;
            assert(!audio_next_block_empty());
            pcm[n+i] = -0.0f;
        }
        assert(audio_next_block_empty());
    }
    s_ports[1].running = 0;
    assert(!audio_next_block_empty());
}
"""
with tempfile.TemporaryDirectory() as tmp:
    binary = Path(tmp) / "probe"
    subprocess.run(["cc", "-std=gnu17", "-O2", "-fsanitize=address,undefined",
                    "-pthread", "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([binary], check=True)
