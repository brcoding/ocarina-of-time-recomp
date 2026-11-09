// Written for ocarina-of-time-recomp (MIT, Copyright (c) 2026 Jeff Aigner).
// OOT_WATCH_WRITE=<addr>[:<len>[:rw]][,...] (diagnostic, at most 4): a hardware write watchpoint on each
// address, in every thread of the process, logging every write with the host instruction that made it, the value
// after it, the frame and the exe return addresses on the stack (tools/resolve_map.py names them; lifted functions
// are f_<guest address>). For a guest address that the GPU reads and nothing on the GPU writes (docs/level.md, the
// level's indirect dispatch records): guest addresses of imported pools are host addresses, so the game's lifted
// stores and the runtime's own copies both hit it. <len> is 1, 2, 4 or 8 (default 8), the address aligned to it;
// ":rw" traps reads too (the CPU reading back what the GPU wrote).
// OOT_WATCH_FROM=<frame> arms it from that frame on (default 0). At most 256 writes are logged; later ones run on.
// OOT_WATCH_REGS=1 adds the host's general registers at each trap: the lifted code keeps guest registers in them, so
// the pointer a stored value was loaded from (the source of a record field) shows up there.
//
// OOT_WATCH_POLL=<addr>[,<addr>...] (at most 4, from OOT_WATCH_FROM): the watch thread, which arms no trap on itself,
// reads each u64 every millisecond and logs every change it sees, at most 256: for values the GPU writes, which
// no trap catches (with OOT_MEMORY=import they land in the memory the CPU reads).
//
// OOT_WATCH_DUMP=<addr>:<len>@<frame>: at that frame the watch thread writes <len> bytes of guest memory at <addr> to
// build/logs/watchdump-<addr>.bin (relative to the working directory, the repository under tools/run.py), e.g. what
// OOT_PROBE's buffer mode stored (with OOT_MEMORY=import, where GPU writes land in guest memory).
//
// The debug registers are per thread and survive fiber switches, so a helper thread arms every thread of the
// process (suspend, set DR0-3 and DR7, resume) and checks for new ones every 200 ms. The trap comes after the write,
// as EXCEPTION_SINGLE_STEP with the address's bit set in DR6; the handler logs, clears DR6 and continues.
#include <windows.h>
#include <tlhelp32.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "gfx.h"
#include "host.h"

static struct {
    int n;
    uint64_t addr[4];
    uint32_t len[4], rw[4];
    uint64_t from;
    volatile LONG logged;
    uintptr_t exe_lo, exe_hi;
    int np;
    uint64_t poll[4], last[4];
    uint64_t dump_addr, dump_len, dump_frame;
    int regs;
} W;

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *cx = ep->ContextRecord;
    DWORD64 hit = cx->Dr6 & 0xf;
    if (!hit) return EXCEPTION_CONTINUE_SEARCH;
    cx->Dr6 = 0;
    if (InterlockedIncrement(&W.logged) <= 256) {
        for (int i = 0; i < W.n; i++) {
            if (!(hit >> i & 1)) continue;
            uint64_t v = 0;   // not for an rw watch: reading the address here would trap again
            if (!W.rw[i]) memcpy(&v, (const void *)(uintptr_t)W.addr[i], W.len[i]);
            char ret[160] = "";
            size_t k = 0;
            const uintptr_t *sp = (const uintptr_t *)(uintptr_t)cx->Rsp;
            for (int j = 0, found = 0; j < 64 && found < 4; j++)   // the first exe addresses on the stack: callers
                if (sp[j] - W.exe_lo < W.exe_hi - W.exe_lo) {
                    k += (size_t)snprintf(ret + k, sizeof ret - k, " exe+%#llx", (unsigned long long)(sp[j] - W.exe_lo));
                    found++;
                }
            uintptr_t rip = (uintptr_t)cx->Rip;
            int in_exe = rip - W.exe_lo < W.exe_hi - W.exe_lo;
            rt_log("watch: frame %llu, %s %#llx (now %#llx) by %s%#llx, thread %lu; stack%s",
                   (unsigned long long)gfx_stats().frames, W.rw[i] ? "access to" : "write to", (unsigned long long)W.addr[i],
                   (unsigned long long)v,
                   in_exe ? "exe+" : "host ", (unsigned long long)(in_exe ? rip - W.exe_lo : rip), GetCurrentThreadId(),
                   k ? ret : " (no exe address)");
            if (W.regs)
                rt_log("watch:   rax %llx rbx %llx rcx %llx rdx %llx rsi %llx rdi %llx rbp %llx r8 %llx r9 %llx r10 %llx "
                       "r11 %llx r12 %llx r13 %llx r14 %llx r15 %llx",
                       (unsigned long long)cx->Rax, (unsigned long long)cx->Rbx, (unsigned long long)cx->Rcx,
                       (unsigned long long)cx->Rdx, (unsigned long long)cx->Rsi, (unsigned long long)cx->Rdi,
                       (unsigned long long)cx->Rbp, (unsigned long long)cx->R8, (unsigned long long)cx->R9,
                       (unsigned long long)cx->R10, (unsigned long long)cx->R11, (unsigned long long)cx->R12,
                       (unsigned long long)cx->R13, (unsigned long long)cx->R14, (unsigned long long)cx->R15);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void arm(DWORD tid) {
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, tid);
    if (!h) return;
    if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT cx = {.ContextFlags = CONTEXT_DEBUG_REGISTERS};
        if (GetThreadContext(h, &cx)) {
            DWORD64 *dr[4] = {&cx.Dr0, &cx.Dr1, &cx.Dr2, &cx.Dr3};
            DWORD64 d7 = 0;
            for (int i = 0; i < W.n; i++) {
                uint32_t lenbits = W.len[i] == 8 ? 2 : W.len[i] == 4 ? 3 : W.len[i] == 2 ? 1 : 0;
                *dr[i] = W.addr[i];
                d7 |= 1ull << (2 * i) | (DWORD64)(W.rw[i] ? 3 : 1) << (16 + 4 * i) | (DWORD64)lenbits << (18 + 4 * i);   // local, write or rw
            }
            cx.Dr7 = d7;
            cx.Dr6 = 0;
            SetThreadContext(h, &cx);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

static DWORD WINAPI watch_thread(void *arg) {
    (void)arg;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    DWORD *armed = NULL;
    size_t narmed = 0, cap = 0;
    while (gfx_stats().frames < W.from) Sleep(50);
    rt_log("watch: armed from frame %llu on %d address(es), polling %d", (unsigned long long)gfx_stats().frames, W.n, W.np);
    for (int i = 0; i < W.np; i++) W.last[i] = *(volatile const uint64_t *)(uintptr_t)W.poll[i];
    for (DWORD tick = 0;; tick++) {
        int polled = 0, logged = 0;
        if (W.dump_len && gfx_stats().frames >= W.dump_frame) {
            char path[64];
            snprintf(path, sizeof path, "build/logs/watchdump-%llx.bin", (unsigned long long)W.dump_addr);
            FILE *f = fopen(path, "wb");
            size_t put = f ? fwrite((const void *)(uintptr_t)W.dump_addr, 1, (size_t)W.dump_len, f) : 0;
            if (f) fclose(f);
            rt_log("watch: frame %llu, %llu bytes at %#llx written to %s", (unsigned long long)gfx_stats().frames,
                   (unsigned long long)put, (unsigned long long)W.dump_addr, f ? path : "(cannot open)");
            W.dump_len = 0;
        }
        for (int ms = 0; W.np && ms < 200; ms++, Sleep(1))
            for (int i = 0; i < W.np; i++) {
                uint64_t v = *(volatile const uint64_t *)(uintptr_t)W.poll[i];
                polled = 1;
                if (v != W.last[i] && logged++ < 256 && InterlockedIncrement(&W.logged) <= 512)
                    rt_log("watch: frame %llu, poll %#llx changed %#llx -> %#llx", (unsigned long long)gfx_stats().frames,
                           (unsigned long long)W.poll[i], (unsigned long long)W.last[i], (unsigned long long)v);
                W.last[i] = v;
            }
        (void)tick;
        if (!W.n) { if (!polled) Sleep(200); continue; }
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = {.dwSize = sizeof te};
        if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
                int seen = 0;
                for (size_t i = 0; i < narmed && !seen; i++) seen = armed[i] == te.th32ThreadID;
                if (seen) continue;
                arm(te.th32ThreadID);
                if (narmed == cap) {
                    cap = cap ? 2 * cap : 256;
                    DWORD *a = realloc(armed, cap * sizeof *a);
                    if (!a) break;
                    armed = a;
                }
                armed[narmed++] = te.th32ThreadID;
            } while (Thread32Next(snap, &te));
        }
        if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
        if (!W.np) Sleep(200);
    }
    return 0;
}

void watch_start(void) {
    const char *e = cfg_get("OOT_WATCH_WRITE"), *pe = cfg_get("OOT_WATCH_POLL");
    for (const char *p = pe; p && *p && W.np < 4;) {
        char *q;
        uint64_t a = strtoull(p, &q, 0);
        if (q == p) break;
        W.poll[W.np++] = a & ~7ull;
        while (*q == ',' || *q == ' ') q++;
        p = q;
    }
    for (const char *p = e; p && *p && W.n < 4;) {
        char *q;
        uint64_t a = strtoull(p, &q, 0), l = 8;
        if (q == p) break;
        if (*q == ':') l = strtoull(q + 1, &q, 0);
        int rw = 0;
        if (q[0] == ':' && q[1] == 'r' && q[2] == 'w') rw = 1, q += 3;
        if (l != 1 && l != 2 && l != 4 && l != 8) l = 8;
        W.addr[W.n] = a & ~(l - 1), W.len[W.n] = (uint32_t)l, W.rw[W.n] = (uint32_t)rw, W.n++;
        while (*q == ',' || *q == ' ') q++;
        p = q;
    }
    const char *de = cfg_get("OOT_WATCH_DUMP");
    if (de && *de) {
        char *q;
        W.dump_addr = strtoull(de, &q, 0);
        W.dump_len = *q == ':' ? strtoull(q + 1, &q, 0) : 0;
        W.dump_frame = *q == '@' ? strtoull(q + 1, NULL, 10) : 0;
    }
    if (!W.n && !W.np && !W.dump_len) return;
    const char *f = cfg_get("OOT_WATCH_FROM");
    W.from = f && *f ? strtoull(f, NULL, 10) : 0;
    W.regs = cfg_get("OOT_WATCH_REGS") && *cfg_get("OOT_WATCH_REGS") == '1';
    HMODULE self = GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((uint8_t *)self + ((IMAGE_DOS_HEADER *)self)->e_lfanew);
    W.exe_lo = (uintptr_t)self, W.exe_hi = (uintptr_t)self + nt->OptionalHeader.SizeOfImage;
    AddVectoredExceptionHandler(1, watch_handler);
    HANDLE t = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
    rt_log("watch: OOT_WATCH_WRITE on %d address(es), from frame %llu", W.n, (unsigned long long)W.from);
}
