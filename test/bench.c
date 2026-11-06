/*
 * bench <runs> <warmup> <command> [args...]
 * Prints min, median and max wall time in ms; output goes to NUL.
 * Compare the minimums: noise only ever adds time.
 */

#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* append one argument to a command line, quoted the way CommandLineToArgvW
 * and the C runtime read it back */
static void quote(wchar_t *dst, const wchar_t *arg)
{
    size_t n = wcslen(dst);
    bool q = !*arg || wcspbrk(arg, L" \t\"");
    if (n)
        dst[n++] = ' ';
    if (q)
        dst[n++] = '"';
    for (const wchar_t *p = arg;; p++) {
        size_t bs = 0;
        while (*p == '\\')
            p++, bs++;
        if (!*p) {
            for (size_t i = 0; i < (q ? bs * 2 : bs); i++)
                dst[n++] = '\\';
            break;
        }
        for (size_t i = 0; i < (*p == '"' ? bs * 2 + 1 : bs); i++)
            dst[n++] = '\\';
        dst[n++] = *p;
    }
    if (q)
        dst[n++] = '"';
    dst[n] = 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) {
        fwprintf(stderr, L"usage: bench <runs> <warmup> <command> [args...]\n");
        return 2;
    }
    int runs = _wtoi(argv[1]), warm = _wtoi(argv[2]);
    static wchar_t cmd[32768];
    for (int i = 3; i < argc; i++)
        quote(cmd, argv[i]);

    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    double *t = malloc(sizeof *t * (runs > 0 ? runs : 1));
    LARGE_INTEGER hz;
    QueryPerformanceFrequency(&hz);

    for (int i = -warm; i < runs; i++) {
        STARTUPINFOW si = {sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = nul;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        PROCESS_INFORMATION pi;
        static wchar_t line[32768];
        wcscpy(line, cmd);
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        if (!CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            fwprintf(stderr, L"bench: cannot run %ls\n", cmd);
            return 1;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        QueryPerformanceCounter(&t1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (i >= 0)
            t[i] = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart;
    }
    qsort(t, runs, sizeof *t, cmp);
    printf("%.1f %.1f %.1f\n", t[0], t[runs / 2], t[runs - 1]);
    return 0;
}
