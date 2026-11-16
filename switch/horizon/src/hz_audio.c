/* Horizon layer: the game's sound output, on libnx audout, in place of SDL's audio device.
 *
 * The engine's cellAudio mixes one 256-frame block at a time and hands it to SDL_QueueAudio; it
 * mixes the next block only while SDL_GetQueuedAudioSize says the device has room. The game's
 * own audio clock (the read index of its ports) advances with those blocks, and its cutscenes
 * and streams are timed by that clock.
 * On the console, through devkitPro's SDL2, nothing was heard and after about 24 s of audio the
 * device stopped taking data: the queue stayed full, the mixer waited for ever, the audio clock
 * froze and the opening cutscene stood still on one shot (console runs 5 and 6: the same frame,
 * [audio-ring] read index stuck at 4423).
 * So the five SDL audio calls cellAudio makes are answered here (linked with --wrap):
 *   - samples go to audout as 16-bit stereo 48 kHz, four mixer blocks (1024 frames) a buffer;
 *   - the queue size reported is what audout still holds;
 *   - and the device can never stall the game: if audout releases nothing for a while, the
 *     queued data is counted as played (dropped), so the mixer and the game's clock keep going.
 */
#include <switch.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    HZ_AUDIO_FRAMES = 1024,                 /* frames in one audout buffer (4 mixer blocks) */
    HZ_AUDIO_BYTES = HZ_AUDIO_FRAMES * 4,   /* s16 stereo: exactly one 4 KiB page */
    HZ_AUDIO_BUFFERS = 12,
    HZ_AUDIO_HIDDEN_FRAMES = 2 * HZ_AUDIO_FRAMES,   /* kept queued beyond what the mixer is told */
    HZ_AUDIO_DEVICE_ID = 2
};
static AudioOutBuffer s_buffers[HZ_AUDIO_BUFFERS];
static int s_buffer_free[HZ_AUDIO_BUFFERS];
static int16_t *s_fill;                     /* the buffer being filled */
static int s_fill_index = -1;
static uint32_t s_fill_frames;
static uint64_t s_submitted_frames, s_released_frames, s_dropped_frames;
static uint64_t s_last_release_tick;
static int s_started;
static Mutex s_lock;
uint64_t hz_audio_stats[4];                 /* buffers played, frames dropped, append failures, started */

static void reclaim(void)
{
    AudioOutBuffer *released = NULL;
    u32 count = 0;
    if (!s_started) return;
    while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)) && count && released) {
        for (int i = 0; i < HZ_AUDIO_BUFFERS; ++i)
            if (&s_buffers[i] == released) { s_buffer_free[i] = 1; break; }
        s_released_frames += HZ_AUDIO_FRAMES;
        ++hz_audio_stats[0];
        s_last_release_tick = armGetSystemTick();
        released = NULL; count = 0;
    }
}
/* frames the device still holds, as far as the game is concerned */
static uint64_t outstanding(void)
{
    const uint64_t done = s_released_frames + s_dropped_frames;
    return s_submitted_frames > done ? s_submitted_frames - done : 0;
}
/* The device has released nothing for 250 ms although it holds data: count that data as played. */
static void unstick(void)
{
    if (outstanding() && armTicksToNs(armGetSystemTick() - s_last_release_tick) > 250000000ull) {
        const uint64_t lost = outstanding();
        s_dropped_frames += lost;
        hz_audio_stats[1] += lost;
        s_last_release_tick = armGetSystemTick();
    }
}

uint32_t __wrap_SDL_OpenAudioDevice(const char *device, int capture, const void *want, void *have, int allowed)
{
    (void)device; (void)capture; (void)allowed;
    if (have && want) memcpy(have, want, 32);   /* SDL_AudioSpec: the engine gets what it asked for */
    mutexLock(&s_lock);
    if (!s_started) {
        Result rc = audoutInitialize();
        if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
        if (R_SUCCEEDED(rc)) {
            for (int i = 0; i < HZ_AUDIO_BUFFERS; ++i) {
                void *memory = memalign(0x1000, HZ_AUDIO_BYTES);
                if (!memory) { rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory); break; }
                memset(memory, 0, HZ_AUDIO_BYTES);
                s_buffers[i].next = NULL;
                s_buffers[i].buffer = memory;
                s_buffers[i].buffer_size = HZ_AUDIO_BYTES;
                s_buffers[i].data_size = HZ_AUDIO_BYTES;
                s_buffers[i].data_offset = 0;
                s_buffer_free[i] = 1;
            }
        }
        s_started = R_SUCCEEDED(rc);
        hz_audio_stats[3] = (uint64_t)s_started;
        s_last_release_tick = armGetSystemTick();
        fprintf(stderr, "[hz] audio: audout %s (result 0x%x), %u Hz, %d buffers of %d frames\n",
                s_started ? "started" : "FAILED: the game runs silent, its audio clock still advances",
                (unsigned)rc, (unsigned)audoutGetSampleRate(), HZ_AUDIO_BUFFERS, HZ_AUDIO_FRAMES);
    }
    mutexUnlock(&s_lock);
    return HZ_AUDIO_DEVICE_ID;   /* never 0: with no device the game's audio clock would not run */
}
void __wrap_SDL_PauseAudioDevice(uint32_t device, int pause) { (void)device; (void)pause; }
void __wrap_SDL_CloseAudioDevice(uint32_t device) { (void)device; }

/* stereo float frames in, as the engine queues them */
int __wrap_SDL_QueueAudio(uint32_t device, const void *data, uint32_t length)
{
    const float *in = data;
    uint32_t frames = length / (2 * sizeof(float));
    (void)device;
    mutexLock(&s_lock);
    reclaim();
    while (frames) {
        uint32_t take;
        if (s_fill_index < 0) {
            for (int i = 0; i < HZ_AUDIO_BUFFERS; ++i)
                if (s_buffer_free[i]) { s_fill_index = i; break; }
            if (s_fill_index < 0 || !s_started) {
                /* no device, or it holds every buffer: these frames are counted as played */
                s_submitted_frames += frames;
                s_dropped_frames += frames;
                hz_audio_stats[1] += frames;
                break;
            }
            s_buffer_free[s_fill_index] = 0;
            s_fill = s_buffers[s_fill_index].buffer;
            s_fill_frames = 0;
        }
        take = HZ_AUDIO_FRAMES - s_fill_frames;
        if (take > frames) take = frames;
        for (uint32_t i = 0; i < take * 2; ++i) {
            float v = in[i] * 32767.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            s_fill[s_fill_frames * 2 + i] = (int16_t)v;
        }
        in += take * 2;
        frames -= take;
        s_fill_frames += take;
        s_submitted_frames += take;
        if (s_fill_frames == HZ_AUDIO_FRAMES) {
            if (R_FAILED(audoutAppendAudioOutBuffer(&s_buffers[s_fill_index]))) {
                ++hz_audio_stats[2];
                s_buffer_free[s_fill_index] = 1;
                s_dropped_frames += HZ_AUDIO_FRAMES;
                hz_audio_stats[1] += HZ_AUDIO_FRAMES;
            }
            s_fill_index = -1;
        }
    }
    mutexUnlock(&s_lock);
    return 0;
}
/* hz-0.43: frames really queued on audout, the hidden ones included (cellAudio.c's late-block hold: the
 * SDL figure below is clamped at 0 and cannot tell a full hidden reserve from an empty device). */
uint32_t hz_audio_outstanding_frames(void)
{
    uint64_t frames;
    mutexLock(&s_lock);
    reclaim();
    frames = outstanding();
    mutexUnlock(&s_lock);
    return (uint32_t)frames;
}
/* bytes of queued float-stereo audio, as SDL would report: what the mixer paces itself by */
uint32_t __wrap_SDL_GetQueuedAudioSize(uint32_t device)
{
    uint64_t frames;
    (void)device;
    mutexLock(&s_lock);
    reclaim();
    unstick();
    frames = outstanding();
    mutexUnlock(&s_lock);
    frames = frames > HZ_AUDIO_HIDDEN_FRAMES ? frames - HZ_AUDIO_HIDDEN_FRAMES : 0;
    return (uint32_t)(frames * 2 * sizeof(float));
}
