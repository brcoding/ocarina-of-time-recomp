// From metroid-prime-4-recomp runtime/frame_rate.c @ 586e91e1b6f38c9e512e39f2d47a046fdf273cf0 (MIT, Copyright (c) 2026 Jeff Aigner), copied with mp4 renamed to oot.
// Adapted from totk-pc-recomp runtime/frame_rate.c @ c944f9f94520874f2a669add30dd08d29ee9f7b5 (MIT, Copyright (c) 2026
// Valiantsin Dzerakh), which adapted MacNativeGames games/totk/runtime/game_patches.c @ ebe621d (MIT, Copyright (c)
// 2026 MacNativeGames contributors) runtime_frame_step_update: the step average, its cap and reset, and the frame
// limit; TotK's 60 fps controller, cloth clock, world clock from the player actor and speed probe are left out.
//
// Frame rate: the host half of the patch set realtime (recomp/patches-v131072.txt). The game steps its frame update by
// a fixed 1/60 s, its frame length on the Switch, and much of it (player movement and turning among it) reads that
// length from a {1/60, 1/30} pair in rodata; the patched sites read the step from the patch page (+0, and the pair at
// +8) instead, and every present writes the moving average of the last 5 display intervals there, at most 1/15 s (slower
// frames slow the game down, as on the console), restarted after a gap of a quarter second (loading). Without the step
// game time ran at the frame rate's share of 60 (about 0.7 at 41 fps), and subtitles fell seconds behind their voices.
// With the step on, the pacer ignores the game's present interval and holds a frame only to the frame limit
// (nvn_api_obj.c present_period, OOT_FPS_LIMIT). Three 32-bit stores per present: the guest reads them on its own threads.
//
// The world clock and the 60 Hz tick count (input scripts, profiler, A/B blocks) run on display time from the first
// present, as before the step: with the step game time follows it.
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"
#include "layout.h"

#include <windows.h>

#define STEP_SAMPLES 5
#define STEP_MAX_S (1.0 / 15.0)   // longest step by default; a longer frame slows the game down (OOT_STEP_MIN_FPS)
#define STEP_RESET_S 0.25         // a gap this long (loading, a debugger) restarts the average
#define STEP_FIRST_S (1.0 / 60.0)
#define LIMIT_MIN_HZ 30
#define LIMIT_MAX_HZ 360

static _Atomic uint32_t *step_word;   // patch page +0: the step in seconds (float bits); NULL with the step off
static uint64_t last_ns;
static double samples[STEP_SAMPLES];
static unsigned sample_count, sample_next;
static _Atomic uint64_t first_present_ns, last_present_ns;

// +0 the step the frame update reads; +8 and +12 the frame-time pair (recomp/patches-v131072.txt: the game's
// {1/60, 1/30} at main+0x13bf400, picked by its frame-rate mode), both the step.
// Constant mode (ocarina-of-time-recomp, no patch table): this game keeps its frame time as the float 1/60 at
// main+0x3e4066c, loaded by 252 sites (LDR S from the shared rodata word, found by scanning main for loads of 1/60 and
// 1/30, 2026-11-09), and 1/30 at main+0x3e40b64 (17 sites, its 30 Hz work). The lifted code reads them from memory
// (LD32), so writing the step into the words moves every site at once: at 60 fps they hold their own values. The page
// is made writable once; a mismatch of either word (another build of the game) leaves the mode off.
#define CONST60_OFF 0x3e4066cull
#define CONST30_OFF 0x3e40b64ull
static _Atomic uint32_t *const60, *const30;
// The step the game's update actually advances by: the float at main+0x4d41204 (in .data), 1/60 while it runs (the
// one exact 1/60 in main's writable image, OOT_F32_SCAN in gameplay, 2026-11-09; forcing it to 1/30 ran the opening
// cutscene ahead at the same wall time). Written while it holds a step (about 1/60 or 1/30, or what was written last).
#define STEPVAR_OFF 0x4d41204ull
static uint32_t stepvar_last;
static double step_max_s = STEP_MAX_S;

static void step_store(float seconds) {
    uint32_t w;
    memcpy(&w, &seconds, 4);
    if (const60) {
        _Atomic uint32_t *sv = (_Atomic uint32_t *)(uintptr_t)(MOD_MAIN_BASE + STEPVAR_OFF);
        uint32_t cur = atomic_load_explicit(sv, memory_order_relaxed);
        float fc;
        memcpy(&fc, &cur, 4);
        if (cur == stepvar_last || (fc > 0.0165f && fc < 0.0168f) || (fc > 0.0332f && fc < 0.0335f)) {
            atomic_store_explicit(sv, w, memory_order_relaxed);
            stepvar_last = w;
        }
        float twice = seconds * 2.0f;
        uint32_t w2;
        memcpy(&w2, &twice, 4);
        atomic_store_explicit(const30, w2, memory_order_relaxed);
        atomic_store_explicit(const60, w, memory_order_release);
        return;
    }
    atomic_store_explicit(&step_word[2], w, memory_order_relaxed);
    atomic_store_explicit(&step_word[3], w, memory_order_relaxed);
    atomic_store_explicit(step_word, w, memory_order_release);
}

static int const_mode_init(void) {
    // Opt-in (OOT_REALTIME=1): the words drive the game's animation and physics timing but not its movement, which
    // counts frames, so at a low frame rate Link walked slowly while jumps and rolls ran on real time (2026-11-09).
    const char *e = cfg_get("OOT_REALTIME");
    if (!e || *e != '1') {
        rt_log("frame rate: the game's fixed 1/60 s steps (OOT_REALTIME=1: real-time step, experimental)");
        return 0;
    }
    uint32_t *a = (uint32_t *)(uintptr_t)(MOD_MAIN_BASE + CONST60_OFF), *b = (uint32_t *)(uintptr_t)(MOD_MAIN_BASE + CONST30_OFF);
    if (*a != 0x3c888889u || *b != 0x3d088889u) {
        rt_log("frame rate: the frame-time constants are not where this build of the game keeps them (%08x %08x): fixed steps",
               *a, *b);
        return 0;
    }
    DWORD old;
    uintptr_t page = (uintptr_t)a & ~(uintptr_t)0xfff;
    size_t len = ((uintptr_t)b | 0xfff) + 1 - page;
    if (!VirtualProtect((void *)page, len, PAGE_READWRITE, &old)) {
        rt_log("frame rate: cannot make the frame-time constants writable (error %lu): fixed steps", GetLastError());
        return 0;
    }
    const60 = (_Atomic uint32_t *)a, const30 = (_Atomic uint32_t *)b;
    const char *mf = cfg_get("OOT_STEP_MIN_FPS");   // the lowest frame rate the step keeps up with (default 15)
    int fps = mf && *mf ? atoi(mf) : 15;
    if (fps >= 5 && fps <= 60) step_max_s = 1.0 / fps;
    rt_log("frame rate: real-time step through the game's frame-time constants (main+%#llx, main+%#llx; OOT_REALTIME=0 turns it off)",
           (unsigned long long)CONST60_OFF, (unsigned long long)CONST30_OFF);
    return 1;
}

void frame_step_init(void) {
    int realtime = rt_patch_set("realtime");
    if (realtime < 0) {
        if (const_mode_init()) {
            static uint32_t dummy[4];
            step_word = (_Atomic uint32_t *)dummy;   // the step logic runs; step_store writes the constants
            step_store((float)STEP_FIRST_S);
            return;
        }
        rt_log("frame rate: no patch table, the game's fixed 1/60 s steps");
        return;
    }
    if (rt_patch_enabled(realtime)) {
        step_word = (_Atomic uint32_t *)rt_patch_page();
        step_store((float)STEP_FIRST_S);
    }
    rt_log("frame rate: %s", step_word ? "real-time step (5-frame average of display intervals, at most 1/15 s)"
                                       : "the game's fixed 1/60 s steps (OOT_REALTIME=0)");
}

int frame_step_realtime(void) { return step_word != NULL; }

// The frame limit (OOT_FPS_LIMIT, live): any whole rate from 30 to 360, or display (the window's monitor). Without the
// real-time step the pacer follows the game's interval and the limit does not apply.
static int limit_hz = -1;   // 0: display; -1 before the first read
static unsigned limit_seen;

static void limit_live(void) {
    unsigned v = cfg_version();
    if (limit_hz >= 0 && v == limit_seen) return;
    limit_seen = v;
    const char *e = cfg_get("OOT_FPS_LIMIT");
    int hz = 60;
    if (e && !strcmp(e, "display")) hz = 0;
    else if (e && *e) {
        char *end;
        long n = strtol(e, &end, 10);
        if (!*end && n >= LIMIT_MIN_HZ && n <= LIMIT_MAX_HZ) hz = (int)n;
        else rt_log("frame rate: OOT_FPS_LIMIT=%s is not 30..360 or display; 60", e);
    }
    if (hz != limit_hz) {
        limit_hz = hz;
        if (hz) rt_log("frame rate: limit %d fps", hz);
        else rt_log("frame rate: limit at the monitor's refresh rate");
    }
}

uint64_t frame_step_cap_ns(void) {
    limit_live();
    double hz = limit_hz ? limit_hz : gfx_window_refresh_hz();
    if (hz < LIMIT_MIN_HZ) hz = 60;   // display rate unknown (no window yet)
    if (hz > LIMIT_MAX_HZ) hz = LIMIT_MAX_HZ;
    return (uint64_t)(1e9 / hz + 0.5);
}

// The frame rate the limit gives, for the performance overlay; 60 without the real-time step, 0 before a frame read it.
double frame_step_limit_hz(void) {
    if (limit_hz < 0) return 0;
    if (!step_word) return 60;
    double r = limit_hz ? limit_hz : gfx_window_refresh_hz();
    return r < LIMIT_MIN_HZ ? 60 : r > LIMIT_MAX_HZ ? LIMIT_MAX_HZ : r;
}

void frame_step_present(uint64_t display_ns, int interval) {
    (void)interval;
    limit_live();
    if (step_word && last_ns) {
        double dt = (double)(display_ns - last_ns) * 1e-9;
        if (dt > STEP_RESET_S || dt < 0.001) {
            sample_count = sample_next = 0;
        } else {
            if (dt > step_max_s) dt = step_max_s;
            samples[sample_next++ % STEP_SAMPLES] = dt;
            if (sample_count < STEP_SAMPLES) sample_count++;
            double sum = 0;
            for (unsigned i = 0; i < sample_count; i++) sum += samples[i];
            step_store((float)(sum / sample_count));
        }
    }
    last_ns = display_ns;
    uint64_t zero = 0;
    atomic_compare_exchange_strong(&first_present_ns, &zero, display_ns ? display_ns : 1);
    atomic_store(&last_present_ns, display_ns);
}

int64_t frame_step_world_us(void) {
    uint64_t first = atomic_load(&first_present_ns), last = atomic_load(&last_present_ns);
    return first && last >= first ? (int64_t)((last - first) / 1000) : -1;
}

uint64_t frame_step_ticks(void) {
    int64_t us = frame_step_world_us();
    return us < 0 ? 0 : (uint64_t)us * 60 / 1000000;
}
