/* Interrupts held off while a lock is held (platform.h).
 *
 * A file of its own because sigaction and SA_RESTART need declarations that
 * -std=c11 alone does not give on glibc (SA_RESTART is X/Open, not bare
 * POSIX), and this macro is the only one of its kind lap needs: platform.c
 * builds without it.
 */
#if !defined(_WIN32) && !defined(_XOPEN_SOURCE)
#define _XOPEN_SOURCE 700
#endif

#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <signal.h>
#include <time.h>
#endif

static int hold_depth;

#ifdef _WIN32

#ifndef STATUS_CONTROL_C_EXIT
#define STATUS_CONTROL_C_EXIT 0xC000013AL
#endif

static volatile LONG pending;

static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        InterlockedExchange(&pending, 1);
        return TRUE;
    }
    return FALSE;
}

void plat_signals_hold(void) {
    if (hold_depth++ > 0)
        return;
    InterlockedExchange(&pending, 0);
    SetConsoleCtrlHandler(on_ctrl, TRUE);
}

void plat_signals_release(void) {
    if (hold_depth == 0 || --hold_depth > 0)
        return;
    SetConsoleCtrlHandler(on_ctrl, FALSE);
    if (InterlockedExchange(&pending, 0)) {
        fflush(NULL);
        ExitProcess((UINT)STATUS_CONTROL_C_EXIT);
    }
}

void plat_test_pause(const char *point) {
    const char *want = getenv("LAP_TEST_PAUSE");
    if (want && strcmp(want, point) == 0)
        Sleep(2000);
}

#else

static const int held[] = {SIGINT, SIGTERM, SIGHUP};
#define HELD_N ((int)(sizeof held / sizeof held[0]))

static volatile sig_atomic_t pending;
static struct sigaction saved[HELD_N];
static int installed[HELD_N];

static void on_signal(int sig) {
    if (pending == 0)
        pending = sig;
}

void plat_signals_hold(void) {
    if (hold_depth++ > 0)
        return;
    pending = 0;
    for (int i = 0; i < HELD_N; i++) {
        installed[i] = 0;
        struct sigaction now;
        if (sigaction(held[i], NULL, &now) != 0 || now.sa_handler == SIG_IGN)
            continue; /* started ignoring it (a background job's SIGINT) */
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
#ifdef SA_RESTART
        sa.sa_flags = SA_RESTART; /* a read or write it lands in carries on */
#endif
        installed[i] = sigaction(held[i], &sa, &saved[i]) == 0;
    }
}

void plat_signals_release(void) {
    if (hold_depth == 0 || --hold_depth > 0)
        return;
    for (int i = 0; i < HELD_N; i++)
        if (installed[i])
            sigaction(held[i], &saved[i], NULL);
    int sig = pending;
    pending = 0;
    if (sig != 0) {
        fflush(NULL); /* what was printed reaches its reader */
        raise(sig);
    }
}

void plat_test_pause(const char *point) {
    const char *want = getenv("LAP_TEST_PAUSE");
    if (!want || strcmp(want, point) != 0)
        return;
    /* nanosleep ends early when a signal arrives, which is the point */
    struct timespec ts = {2, 0};
    nanosleep(&ts, NULL);
}

#endif
