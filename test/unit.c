/* unit tests for the matchers: build with the Makefile or build.bat test */
#include "fq.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void fq_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static int failed, run;

/* print a wide string with anything outside ASCII as \uXXXX */
static const char *show(const WCHAR *s)
{
    static char buf[4][256];
    static int k;
    char *b = buf[k++ & 3], *p = b;
    for (; *s && p < b + 240; s++)
        p += *s >= 0x20 && *s < 0x7f ? sprintf(p, "%c", (char)*s) : sprintf(p, "\\u%04x", *s);
    *p = 0;
    return b;
}

static void check(int kind, bool icase, const WCHAR *pat, const WCHAR *name, bool want)
{
    static const char *const kinds[] = {"all", "substr", "glob", "regex"};
    Pattern p;
    const char *err = NULL;
    run++;
    if (!pattern_compile(&p, pat, kind, icase, &err)) {
        printf("FAIL %s %s: does not compile (%s)\n", kinds[kind], show(pat), err);
        failed++;
        return;
    }
    RxScratch *rx = p.kind == PAT_REGEX ? regex_scratch(p.re) : NULL;
    bool got = pattern_match(&p, rx, name, wcslen(name));
    if (got != want) {
        printf("FAIL %s%s %s on %s: got %d, want %d\n", kinds[kind], icase ? " -i" : "", show(pat),
               show(name), got, want);
        failed++;
    }
}

static void bad(const WCHAR *pat)
{
    Pattern p;
    const char *err = NULL;
    run++;
    if (pattern_compile(&p, pat, PAT_REGEX, false, &err)) {
        printf("FAIL regex %s should not compile\n", show(pat));
        failed++;
    }
}

#define RE(pat, name, want) check(PAT_REGEX, false, pat, name, want)
#define REI(pat, name, want) check(PAT_REGEX, true, pat, name, want)
#define GL(pat, name, want) check(PAT_GLOB, false, pat, name, want)
#define GLI(pat, name, want) check(PAT_GLOB, true, pat, name, want)

int main(void)
{
    fold_init();

    /* the old fq lowercased the pattern for -i, which turned \D into \d */
    RE(L"^\\D+$", L"abc", true);
    RE(L"^\\D+$", L"123", false);
    REI(L"^\\D+$", L"abc", true);
    REI(L"^\\D+$", L"123", false);
    REI(L"^\\W", L"-x", true);
    REI(L"^\\S+$", L"a b", false);

    /* [^...] was broken in the old regex library */
    RE(L"^[^abc]+$", L"abc", false);
    RE(L"^[^abc]+$", L"xyz", true);
    RE(L"[^a]", L"aaa", false);

    RE(L"^\\d{4}-\\d{2}", L"2024-05-notes.md", true);
    RE(L"^\\d{4}-\\d{2}", L"24-05", false);
    RE(L"colou?r", L"color.txt", true);
    RE(L"colou?r", L"colour.txt", true);
    RE(L"colou?r", L"colouur.txt", false);
    RE(L"\\.txt$", L"a.txt", true);
    RE(L"\\.txt$", L"a.txt.bak", false);
    RE(L"^(ab)+c$", L"ababc", true);
    RE(L"^(ab)+c$", L"abac", false);
    RE(L"^(?:x|y)z", L"yz", true);
    RE(L"^a.c$", L"abc", true);
    RE(L"^a.c$", L"abbc", false);
    RE(L"^x{2,3}$", L"x", false);
    RE(L"^x{2,3}$", L"xx", true);
    RE(L"^x{2,3}$", L"xxx", true);
    RE(L"^x{2,3}$", L"xxxx", false);
    RE(L"^x{2,}$", L"xxxxxx", true);
    RE(L"a{,3}", L"a{,3}", true); /* not a count: literal brace */
    RE(L"$", L"", true);
    RE(L"$", L"abc", true);
    RE(L"^$", L"a", false);
    RE(L"a|", L"zzz", true); /* empty alternative matches anything */
    RE(L"README", L"readme.md", false);
    REI(L"README", L"readme.md", true);
    REI(L"café", L"CAFÉ.txt", true); /* folding goes past ASCII */
    REI(L"[a-z]+\\.TXT$", L"NOTES.txt", true);

    /* exponential for a backtracking engine, linear here */
    {
        WCHAR name[64];
        for (int i = 0; i < 40; i++)
            name[i] = 'a';
        name[40] = 'c';
        name[41] = 0;
        LARGE_INTEGER t0, t1, hz;
        QueryPerformanceCounter(&t0);
        RE(L"^(a*)*b$", name, false);
        RE(L"^(a|aa)+$", name, false);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&hz);
        double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)hz.QuadPart;
        run++;
        if (ms > 50) {
            printf("FAIL pathological regexes took %.1f ms\n", ms);
            failed++;
        }
    }

    bad(L"(");
    bad(L"a)");
    bad(L"[a");
    bad(L"*a");
    bad(L"a{3,1}");
    bad(L"\\q");
    bad(L"^*");

    /* globs */
    GL(L"*.c", L"main.c", true);
    GL(L"*.c", L"main.cpp", false);
    GL(L"*.{c,h}", L"fq.h", true);
    GL(L"*.{c,h}", L"fq.o", false);
    GL(L"{a,b{c,d}}*", L"bdx", true);
    GL(L"{a,b{c,d}}*", L"bex", false);
    GL(L"{abc}", L"{abc}", true); /* no comma: just braces */
    GL(L"[!a]*", L"abc", false);
    GL(L"[!a]*", L"bc", true);
    GL(L"[^a]*", L"bc", true);
    GL(L"[a-c]?", L"bz", true);
    GL(L"[]]", L"]", true);
    GL(L"[a-", L"[a-", true); /* no closing bracket: literal */
    GL(L"?.txt", L"a.txt", true);
    GL(L"?.txt", L"ab.txt", false);
    GL(L"a*b*c", L"aXbYbZc", true);
    GL(L"a*b*c", L"aXbY", false);
    GL(L"\\*", L"*", true);
    GL(L"\\*", L"a", false);
    GL(L"*", L"", true);
    GL(L"**.md", L"x.md", true);
    GL(L"*.JPG", L"x.jpg", false);
    GLI(L"*.JPG", L"x.jpg", true);
    GLI(L"[A-C]*", L"beta", true);

    /* substrings */
    check(PAT_SUBSTR, true, L"Report", L"my_report.pdf", true);
    check(PAT_SUBSTR, false, L"Report", L"my_report.pdf", false);
    check(PAT_SUBSTR, true, L"été", L"ÉTÉ 2024.jpg", true);
    check(PAT_SUBSTR, false, L"abc", L"ab", false);
    check(PAT_SUBSTR, false, L"", L"anything", true);
    check(PAT_SUBSTR, false, L"*", L"anything", true);

    printf("%d/%d unit tests passed\n", run - failed, run);
    return failed != 0;
}
