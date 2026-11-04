/*
 * console_win.c
 *
 * Copyright (c) 2026 Scintillae Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * console_win.c - console visibility policy for tigryx on Windows.
 *
 * Windows has two PE subsystems that matter here:
 *
 *   CONSOLE  - Windows always creates a console window before main() runs.
 *              This is what `tigryx.exe` / `tigryxvm.exe` use so that typing
 *              `tigryx ...` at a cmd/PowerShell prompt behaves like any other
 *              CLI tool: the shell waits, shares its console, and $? works.
 *
 *   GUI      - Windows never creates a console. This is what `tigryxw.exe` /
 *              `tigryxvmw.exe` use (the "w" twins), so double-clicking a .tgx
 *              file or a bound executable never flashes a console window.
 *              The installer registers file associations against tigryxw.exe.
 *
 * `tigryx bind --no-console` copies tigryxvmw.exe as the stub so the produced
 * executable is GUI-subsystem and never gets a console from Windows.
 *
 * relic_console_init() - call at the very top of main(), before any I/O.
 *   On GUI-subsystem binaries, tries AttachConsole(ATTACH_PARENT_PROCESS) to
 *   grab the parent shell's console. If that succeeds (launched from
 *   cmd/PowerShell), stdout/stderr work normally. If it fails (Explorer
 *   double-click), there is no console at all and none is shown.
 *   On CONSOLE-subsystem binaries this is a no-op (the process already has
 *   a console; AttachConsole fails and stdio is untouched).
 *
 * relic_console_attach(no_console) - call once no_console is known. When
 *   true, sets a global flag that makes relic_console_ensure() a complete
 *   no-op: no console is EVER shown under any circumstance. When false,
 *   relic_console_ensure() works normally (show/create on demand).
 *
 * relic_console_ensure() - call right before writing anything a user should
 *   see (a fatal error, @console, --version, ...). If no_console was set,
 *   this is a no-op. Otherwise, un-hides or allocates a console as needed.
 *
 * All three are cheap to call repeatedly (idempotent) and safe on every
 * platform: elsewhere - and on the Win9x target, whose vintage MinGW headers
 * don't declare GetConsoleProcessList - they compile to no-ops.
 */
#include "../../include/tigryx.h"

#if defined(_WIN32) && !defined(RELIC_WIN95)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>

/* True when this process is the ONLY one attached to its console, which
 * means the console was created just for us - i.e. we were launched from
 * Explorer (a file association, shortcut, or a double-clicked exe) rather
 * than from a shell.  A shell's console always has at least the shell
 * attached too. */
static int relic_console_is_exclusively_ours(void) {
    DWORD pids[4];
    DWORD n = GetConsoleProcessList(pids, (DWORD)(sizeof pids / sizeof pids[0]));
    return n == 1;
}

/* Track whether we successfully attached to a parent console in init(). */
static int g_console_attached = 0;

/* True when the given std handle (STD_OUTPUT_HANDLE / STD_ERROR_HANDLE /
 * STD_INPUT_HANDLE) is already usable - i.e. the parent redirected it to a
 * file or pipe, or we were launched from a console that gave us one.
 *
 * This matters because freopen("CONOUT$") unconditionally would REPLACE an
 * inherited redirect: `myapp.exe > out.txt` on a GUI-subsystem build would
 * write to the console instead of out.txt, silently losing the output. So
 * only reopen the streams Windows did not give us. */
static int relic_std_handle_is_valid(DWORD which) {
    HANDLE h = GetStdHandle(which);
    return h != NULL && h != INVALID_HANDLE_VALUE;
}

/* When true, relic_console_ensure() is a complete no-op — no console is
 * ever shown under any circumstance. Set by relic_console_attach(1). */
static int g_no_console = 0;

void relic_console_init(void) {
    if (g_console_attached) return;
    /* GUI-subsystem binary: try to grab the parent's console so CLI use
     * (cmd/PowerShell) still works. If there is no parent console (Explorer
     * launch), this fails silently and we have no console at all. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        g_console_attached = 1;
        /* Reopen C stdio handles on the attached console. The CRT's pre-
         * opened stdin/stdout/stderr point at the (non-existent) GUI handles
         * we were born with; redirect them to the console we just attached.
         * Streams the parent already redirected (to a file or pipe) are left
         * alone so `myapp.exe > out.txt` keeps working. */
        if (!relic_std_handle_is_valid(STD_INPUT_HANDLE))  freopen("CONIN$",  "r", stdin);
        if (!relic_std_handle_is_valid(STD_OUTPUT_HANDLE)) freopen("CONOUT$", "w", stdout);
        if (!relic_std_handle_is_valid(STD_ERROR_HANDLE))  freopen("CONOUT$", "w", stderr);
    }
}

void relic_console_attach(int no_console) {
    if (!no_console) return;
    /* If a console exists and is exclusively ours (CONSOLE-subsystem child
     * that got its own console), hide it and set g_no_console so ensure()
     * never re-shows it. On GUI-subsystem builds there is nothing to hide. */
    HWND h = GetConsoleWindow();
    if (h) {
        if (relic_console_is_exclusively_ours()) {
            ShowWindow(h, SW_HIDE);
            g_no_console = 1;
        }
        /* else: shared with a shell — don't hide the user's terminal, and
         * don't set g_no_console so ensure() can still show errors there. */
    } else {
        /* No console at all (GUI-subsystem, Explorer launch, or
         * CREATE_NO_WINDOW child). Set g_no_console so ensure() doesn't
         * try to AllocConsole — that would pop up a new window. */
        g_no_console = 1;
    }
}

/* True when stderr is already redirected somewhere the user can see it
 * (a pipe or file) rather than a console buffer. A handle Windows gave us
 * for a console - including an invisible one created by a parent's
 * CREATE_NO_WINDOW - is FILE_TYPE_CHAR; a pipe (mintty, IDE terminals,
 * CI logs) or a `2>file` redirect is not. */
static int relic_stderr_is_redirected(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_CHAR;
}

void relic_console_ensure(void) {
    /* If --no-console was specified, NEVER show a console under any
     * circumstance. This is absolute — no AllocConsole, no ShowWindow,
     * nothing. The user explicitly asked for no console. */
    if (g_no_console) return;

    /* stderr already reaches the user through a pipe or file - showing or
     * allocating a console window would add nothing. This is also what
     * stops a hidden CREATE_NO_WINDOW console (mintty/IDE terminal case)
     * from popping up just to display an error the pipe already delivers. */
    if (relic_stderr_is_redirected()) return;

    HWND h = GetConsoleWindow();
    if (h) {
        if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
        return;
    }
    /* No console at all (GUI-subsystem, Explorer launch without
     * --no-console). Allocate one so the user can see error output,
     * @console text, --version, etc. */
    if (AllocConsole()) {
        g_console_attached = 1;
        /* As in init(): never clobber a redirect the parent handed us. */
        if (!relic_std_handle_is_valid(STD_INPUT_HANDLE))  freopen("CONIN$",  "r", stdin);
        if (!relic_std_handle_is_valid(STD_OUTPUT_HANDLE)) freopen("CONOUT$", "w", stdout);
        if (!relic_std_handle_is_valid(STD_ERROR_HANDLE))  freopen("CONOUT$", "w", stderr);
        SetConsoleTitleA("Tigryx");
    }
}

#else /* !_WIN32 || RELIC_WIN95 */

void relic_console_init(void) {}
void relic_console_attach(int no_console) { (void)no_console; }
void relic_console_ensure(void) {}

#endif
