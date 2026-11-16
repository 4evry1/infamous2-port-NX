/* Horizon layer: the renderer's window is the Switch screen.
 *
 * The engine's Vulkan backend gets its window, its surface and its window events from SDL2
 * (SDL_CreateWindow with SDL_WINDOW_VULKAN, SDL_Vulkan_CreateSurface, ...). devkitPro's SDL2 for
 * Switch draws through EGL and has no Vulkan side. The calls the backend makes are answered here
 * instead (linked with --wrap=<each>), so the engine source stays as it is:
 *   - the "window" is libnx's default native window (the whole screen, 1280x720);
 *   - the surface is Mesa NVK's VK_NN_vi_surface on that window (the same call the Captain
 *     America port's presenter makes);
 *   - there are no window events, no mouse, no fullscreen switch: those calls do nothing.
 * SDL itself still serves the controller and the audio device; only SDL's video subsystem is
 * never started.
 * The screen has one owner at a time: opening the window takes it from the text console
 * (hz_entry.c), dropping the window gives it back. */
#define VK_USE_PLATFORM_VI_NN 1
#include <switch.h>
#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void hz_screen_handover(int request);
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name);

enum { HZ_SCREEN_W = 1280, HZ_SCREEN_H = 720 };
enum { HZ_SDL_INIT_VIDEO = 0x00000020u };
enum { HZ_SDL_WINDOW_SHOWN = 0x00000004u, HZ_SDL_WINDOW_INPUT_FOCUS = 0x00000200u, HZ_SDL_WINDOW_VULKAN = 0x10000000u };
typedef struct { uint32_t format; int w, h, refresh_rate; void *driverdata; } hz_display_mode;   /* SDL_DisplayMode */
static char s_window_token;   /* the SDL_Window* the backend holds; never dereferenced by it */
static char s_error[128] = "";

/* ---- SDL's video subsystem is not used -------------------------------------------------------- */
int __real_SDL_InitSubSystem(uint32_t flags);
void __real_SDL_QuitSubSystem(uint32_t flags);
/* ...nor its audio subsystem: the sound output is audout directly (hz_audio.c) */
enum { HZ_SDL_INIT_AUDIO = 0x00000010u };
int __wrap_SDL_InitSubSystem(uint32_t flags)
{
    flags &= ~(uint32_t)(HZ_SDL_INIT_VIDEO | HZ_SDL_INIT_AUDIO);
    return flags ? __real_SDL_InitSubSystem(flags) : 0;
}
void __wrap_SDL_QuitSubSystem(uint32_t flags)
{
    flags &= ~(uint32_t)(HZ_SDL_INIT_VIDEO | HZ_SDL_INIT_AUDIO);
    if (flags) __real_SDL_QuitSubSystem(flags);
}
const char *__wrap_SDL_GetCurrentVideoDriver(void) { return "horizon-vi (Switch screen, no SDL video)"; }

/* ---- the window --------------------------------------------------------------------------------- */
void *__wrap_SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags)
{
    (void)title; (void)x; (void)y; (void)w; (void)h; (void)flags;
    hz_screen_handover(1);   /* the text console lets go of the screen */
    nwindowSetDimensions(nwindowGetDefault(), HZ_SCREEN_W, HZ_SCREEN_H);
    fprintf(stderr, "[hz] window: the Switch screen, %dx%d; the text console has let go of it\n", HZ_SCREEN_W, HZ_SCREEN_H);
    return &s_window_token;
}
void __wrap_SDL_DestroyWindow(void *window)
{
    (void)window;
    fprintf(stderr, "[hz] window dropped: the text console takes the screen back\n");
    hz_screen_handover(2);
}
uint32_t __wrap_SDL_GetWindowFlags(void *window)
{
    (void)window;
    return HZ_SDL_WINDOW_SHOWN | HZ_SDL_WINDOW_INPUT_FOCUS | HZ_SDL_WINDOW_VULKAN;   /* never minimized */
}
void __wrap_SDL_GetWindowSize(void *window, int *w, int *h) { (void)window; if (w) *w = HZ_SCREEN_W; if (h) *h = HZ_SCREEN_H; }
void __wrap_SDL_Vulkan_GetDrawableSize(void *window, int *w, int *h) { (void)window; if (w) *w = HZ_SCREEN_W; if (h) *h = HZ_SCREEN_H; }
int __wrap_SDL_SetWindowFullscreen(void *window, uint32_t flags) { (void)window; (void)flags; return 0; }
int __wrap_SDL_GetWindowDisplayIndex(void *window) { (void)window; return 0; }
static int display_mode(hz_display_mode *mode)
{
    if (!mode) return -1;
    memset(mode, 0, sizeof *mode);
    mode->w = HZ_SCREEN_W; mode->h = HZ_SCREEN_H; mode->refresh_rate = 60;
    return 0;
}
int __wrap_SDL_GetDesktopDisplayMode(int display, hz_display_mode *mode) { (void)display; return display_mode(mode); }
int __wrap_SDL_GetCurrentDisplayMode(int display, hz_display_mode *mode) { (void)display; return display_mode(mode); }

/* ---- the Vulkan surface -------------------------------------------------------------------------- */
int __wrap_SDL_Vulkan_GetInstanceExtensions(void *window, unsigned *count, const char **names)
{
    static const char *const wanted[] = {"VK_KHR_surface", "VK_NN_vi_surface"};
    (void)window;
    if (!count) return 0;
    if (names) {
        if (*count < 2) return 0;
        names[0] = wanted[0]; names[1] = wanted[1];
    }
    *count = 2;
    return 1;
}
int __wrap_SDL_Vulkan_CreateSurface(void *window, VkInstance instance, VkSurfaceKHR *surface)
{
    const PFN_vkCreateViSurfaceNN create = (PFN_vkCreateViSurfaceNN)vkGetInstanceProcAddr(instance, "vkCreateViSurfaceNN");
    VkViSurfaceCreateInfoNN info;
    VkResult result;
    (void)window;
    if (!create) {
        snprintf(s_error, sizeof s_error, "the driver has no vkCreateViSurfaceNN");
        fprintf(stderr, "[hz] surface: %s\n", s_error);
        return 0;
    }
    memset(&info, 0, sizeof info);
    info.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN;
    info.window = nwindowGetDefault();
    result = create(instance, &info, NULL, surface);
    if (result != VK_SUCCESS) {
        snprintf(s_error, sizeof s_error, "vkCreateViSurfaceNN failed (%d)", (int)result);
        fprintf(stderr, "[hz] surface: %s\n", s_error);
        return 0;
    }
    fprintf(stderr, "[hz] surface: VK_NN_vi_surface on the default window\n");
    return 1;
}

/* ---- no window events, no mouse ------------------------------------------------------------------- */
int __wrap_SDL_PollEvent(void *event) { (void)event; return 0; }
int __wrap_SDL_SetRelativeMouseMode(int enabled) { (void)enabled; return 0; }
int __wrap_SDL_GetRelativeMouseMode(void) { return 0; }
void __wrap_SDL_SetWindowGrab(void *window, int grabbed) { (void)window; (void)grabbed; }
int __wrap_SDL_ShowCursor(int toggle) { (void)toggle; return 0; }
void __wrap_SDL_WarpMouseInWindow(void *window, int x, int y) { (void)window; (void)x; (void)y; }
int __wrap_SDL_CaptureMouse(int enabled) { (void)enabled; return 0; }
