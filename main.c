#include "fq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char usage[] =
    "fq " FQ_VERSION ": find files and folders on Windows\n"
    "\n"
    "usage: fq [pattern] [path] [options]\n"
    "\n"
    "  pattern   part of the name to look for; empty or * matches everything\n"
    "  path      where to look (default: the current folder)\n"
    "\n"
    "matching\n"
    "  -g, --glob              pattern is a glob: * ? [a-z] [!a-z] {jpg,png}\n"
    "  -r, --regex             pattern is a regular expression\n"
    "  -c, --case              match case (by default case is ignored)\n"
    "\n"
    "what to list\n"
    "      --folders           list folders as well as files\n"
    "      --folders-only      list only folders\n"
    "      --files-only        list only files (the default)\n"
    "  -H, --include-hidden    include hidden files and names starting with .\n"
    "  -L, --follow-symlinks   go into symlinked and junctioned folders\n"
    "      --no-skip           also go into node_modules, .git, build, dist, bin,\n"
    "                          obj, Debug, Release ... and, at the top of a drive,\n"
    "                          Windows, Program Files, ProgramData ...\n"
    "                          (skipped folders are still listed if they match)\n"
    "  -d, --max-depth <n>     how deep to go; 0 = only the folder itself\n"
    "      --max-results <n>   stop after n results\n"
    "\n"
    "filters (sizes and types apply to files)\n"
    "  -e, --ext <list>        extensions, comma-separated: pdf,docx\n"
    "  -t, --type <type>       text, image, video, audio or archive\n"
    "      --min <size>        at least this big: 10K, 5M, 1G\n"
    "      --max <size>        at most this big\n"
    "      --size <size>       exactly this big; +size bigger, -size smaller\n"
    "      --after <date>      modified on or after YYYY-MM-DD (UTC)\n"
    "      --before <date>     modified before YYYY-MM-DD\n"
    "      --created-after <date>\n"
    "      --created-before <date>\n"
    "\n"
    "output\n"
    "      --json              one JSON document with every result\n"
    "      --preview [n]       show the first n lines of text files (default 10)\n"
    "      --out <file>        write the results to a file, as UTF-8\n"
    "      --color <when>      auto, always or never\n"
    "      --stats             print counts and timings to stderr at the end\n"
    "  -q, --quiet             no warnings\n"
    "\n"
    "speed\n"
    "  -j, --threads <n>       worker threads (default: one per CPU, at most 8)\n"
    "      --timeout <ms>      stop after this many milliseconds\n"
    "\n"
    "  -h, --help              this\n"
    "  -V, --version           the version\n"
    "\n"
    "examples\n"
    "  fq report                       names containing \"report\"\n"
    "  fq \"*.{jpg,png}\" D:\\ --glob     pictures on D:\n"
    "  fq \"^\\d{4}-\" --regex            names starting with a year and a dash\n"
    "  fq \"\" C:\\ --min 1G              files over 1 GB anywhere on C:\n"
    "  fq build --folders-only         folders called build\n";

static void say(const char *s)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD n = (DWORD)strlen(s), done, mode;
    if (GetFileType(h) == FILE_TYPE_CHAR && GetConsoleMode(h, &mode)) {
        WCHAR *w = xmalloc((n + 1) * sizeof *w);
        int k = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, (int)n + 1);
        WriteConsoleW(h, w, (DWORD)k, &done, NULL);
        free(w);
    } else {
        WriteFile(h, s, n, &done, NULL);
    }
}

static int fail(const char *what, const WCHAR *arg)
{
    char *a = arg ? utf8(arg) : NULL;
    if (a)
        fq_err("fq: %s '%s'\n", what, a);
    else
        fq_err("fq: %s\n", what);
    fq_err("try 'fq --help'\n");
    free(a);
    return 1;
}

static bool parse_u64(const WCHAR **s, uint64_t *v)
{
    const WCHAR *p = *s;
    uint64_t n = 0;
    if (*p < '0' || *p > '9')
        return false;
    for (; *p >= '0' && *p <= '9'; p++) {
        if (n > (UINT64_MAX - 9) / 10)
            return false;
        n = n * 10 + (uint64_t)(*p - '0');
    }
    *s = p;
    *v = n;
    return true;
}

static bool parse_number(const WCHAR *s, uint64_t *v)
{
    return parse_u64(&s, v) && *s == 0;
}

/* 1500, 10K, 5M, 2G, 1T; an optional B or iB after the unit; powers of 1024 */
static bool parse_size(const WCHAR *s, uint64_t *v)
{
    uint64_t n, mul = 1;
    if (!parse_u64(&s, &n))
        return false;
    switch (*s) {
    case 'k': case 'K': mul = 1ULL << 10; break;
    case 'm': case 'M': mul = 1ULL << 20; break;
    case 'g': case 'G': mul = 1ULL << 30; break;
    case 't': case 'T': mul = 1ULL << 40; break;
    }
    if (mul > 1) {
        s++;
        if (*s == 'i')
            s++;
    }
    if (*s == 'b' || *s == 'B')
        s++;
    if (*s || n > UINT64_MAX / mul)
        return false;
    *v = n * mul;
    return true;
}

/* YYYY-MM-DD, midnight UTC, as FILETIME ticks */
static bool parse_date(const WCHAR *s, int64_t *t)
{
    uint64_t y, m, d;
    if (!parse_u64(&s, &y) || *s++ != '-' || !parse_u64(&s, &m) || *s++ != '-' ||
        !parse_u64(&s, &d) || *s)
        return false;
    if (y < 1601 || y > 30827 || m < 1 || m > 12 || d < 1 || d > 31)
        return false;
    SYSTEMTIME st = {(WORD)y, (WORD)m, 0, (WORD)d, 0, 0, 0, 0};
    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft)) /* also rejects 2025-02-30 */
        return false;
    *t = (int64_t)((uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime);
    return true;
}

static void parse_exts(Query *q, const WCHAR *list)
{
    for (const WCHAR *p = list; *p;) {
        while (*p == ' ' || *p == ',')
            p++;
        if (*p == '.')
            p++;
        const WCHAR *e = p;
        while (*p && *p != ',')
            p++;
        const WCHAR *end = p;
        while (end > e && end[-1] == ' ')
            end--;
        if (end == e)
            continue;
        WCHAR *x = wdup(e, (size_t)(end - e));
        for (WCHAR *c = x; *c; c++)
            *c = fq_upper[*c];
        q->exts = xrealloc(q->exts, (q->nexts + 1) * sizeof *q->exts);
        q->exts[q->nexts++] = x;
    }
}

/* one argument: a path if it is an existing folder or looks like one */
static bool looks_like_path(const WCHAR *a)
{
    if (a[0] == '\\' || a[0] == '/' || (a[0] && a[1] == ':'))
        return true;
    if (!wcscmp(a, L".") || !wcscmp(a, L"..") || !wcsncmp(a, L".\\", 2) || !wcsncmp(a, L"./", 2) ||
        !wcsncmp(a, L"..\\", 3) || !wcsncmp(a, L"../", 3))
        return true;
    DWORD attr = GetFileAttributesW(a);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool is_digits(const WCHAR *s)
{
    if (!*s)
        return false;
    for (; *s; s++)
        if (*s < '0' || *s > '9')
            return false;
    return true;
}

/* \\?\ form of the root, so paths longer than MAX_PATH work everywhere */
static WCHAR *open_path(const WCHAR *full)
{
    size_t n = wcslen(full);
    WCHAR *o;
    if (!wcsncmp(full, L"\\\\?\\", 4) || !wcsncmp(full, L"\\\\.\\", 4)) {
        o = wdup(full, n);
    } else if (!wcsncmp(full, L"\\\\", 2)) { /* \\server\share -> \\?\UNC\server\share */
        o = xmalloc((n + 8) * sizeof *o);
        wcscpy(o, L"\\\\?\\UNC");
        wcscat(o, full + 1);
    } else {
        o = xmalloc((n + 5) * sizeof *o);
        wcscpy(o, L"\\\\?\\");
        wcscat(o, full);
    }
    /* drop trailing separators, except the one in \\?\C:\ */
    size_t m = wcslen(o);
    while (m > 7 && o[m - 1] == '\\')
        o[--m] = 0;
    if (m == 6 && o[5] == ':') { /* \\?\C: -> \\?\C:\ */
        o = xrealloc(o, 8 * sizeof *o);
        o[6] = '\\';
        o[7] = 0;
    }
    return o;
}

static WCHAR *show_prefix(const WCHAR *typed)
{
    size_t n = wcslen(typed);
    WCHAR *s = xmalloc((n + 2) * sizeof *s);
    wcscpy(s, typed);
    for (WCHAR *c = s; *c; c++)
        if (*c == '/')
            *c = '\\';
    if (n && s[n - 1] != '\\' && !(n == 2 && s[1] == ':')) {
        s[n] = '\\';
        s[n + 1] = 0;
    }
    return s;
}

static bool opt(const WCHAR *a, const WCHAR *lng, const WCHAR *shrt)
{
    return !wcscmp(a, lng) || (shrt && !wcscmp(a, shrt));
}

int wmain(int argc, WCHAR **argv)
{
    fold_init();

    Query q;
    memset(&q, 0, sizeof q);
    q.files = true;
    q.skip = true;
    q.max_depth = UINT32_MAX;
    q.max_size = UINT64_MAX;
    q.mtime_lo = q.ctime_lo = INT64_MIN;
    q.mtime_hi = q.ctime_hi = INT64_MAX;
    q.type = TYPE_ANY;

    OutOptions oo = {NULL, COLOR_AUTO, false, 0};
    WalkOptions wo = {NULL, NULL, 0, 0, false};
    bool glob = false, regex = false, icase = true, stats = false, quiet = false;
    const WCHAR *pos[2] = {NULL, NULL};
    int npos = 0;
    bool more_opts = true;

    for (int i = 1; i < argc; i++) {
        WCHAR *a = argv[i];
        if (!more_opts || a[0] != '-' || a[1] == 0) {
            if (npos == 2)
                return fail("too many arguments:", a);
            pos[npos++] = a;
            continue;
        }
        if (!wcscmp(a, L"--")) {
            more_opts = false;
            continue;
        }

        /* --name=value */
        const WCHAR *val = NULL;
        WCHAR *eq = a[1] == '-' ? wcschr(a, '=') : NULL;
        if (eq) {
            *eq = 0;
            val = eq + 1;
        }
#define VALUE()                                                                                    \
    do {                                                                                           \
        if (!val) {                                                                                \
            if (i + 1 >= argc)                                                                     \
                return fail("missing value for", a);                                               \
            val = argv[++i];                                                                       \
        }                                                                                          \
    } while (0)

        if (opt(a, L"--help", L"-h")) {
            say(usage);
            return 0;
        } else if (opt(a, L"--version", L"-V")) {
            say("fq " FQ_VERSION "\n");
            return 0;
        } else if (opt(a, L"--case", L"-c")) {
            icase = false;
        } else if (opt(a, L"--glob", L"-g")) {
            glob = true;
        } else if (opt(a, L"--regex", L"-r")) {
            regex = true;
        } else if (opt(a, L"--include-hidden", L"-H")) {
            q.hidden = true;
        } else if (opt(a, L"--follow-symlinks", L"-L")) {
            q.follow = true;
        } else if (opt(a, L"--no-skip", NULL)) {
            q.skip = false;
        } else if (opt(a, L"--folders", NULL) || opt(a, L"--dirs", NULL)) {
            q.dirs = true;
        } else if (opt(a, L"--folders-only", NULL) || opt(a, L"--dirs-only", NULL)) {
            q.dirs = true;
            q.files = false;
        } else if (opt(a, L"--files-only", NULL)) {
            q.files = true;
            q.dirs = false;
        } else if (opt(a, L"--quiet", L"-q")) {
            quiet = true;
        } else if (opt(a, L"--json", NULL)) {
            oo.json = true;
        } else if (opt(a, L"--stats", NULL)) {
            stats = true;
        } else if (opt(a, L"--preview", NULL)) {
            uint64_t n = 10;
            if (val || (i + 1 < argc && is_digits(argv[i + 1]))) {
                if (!val)
                    val = argv[++i];
                if (!parse_number(val, &n) || n < 1 || n > 1000)
                    return fail("--preview wants 1 to 1000 lines, not", val);
            }
            oo.preview = (int)n;
        } else if (opt(a, L"--ext", L"-e")) {
            VALUE();
            parse_exts(&q, val);
        } else if (opt(a, L"--type", L"-t")) {
            VALUE();
            if ((q.type = type_parse(val)) < 0)
                return fail("--type is text, image, video, audio or archive, not", val);
        } else if (opt(a, L"--min", NULL)) {
            VALUE();
            if (!parse_size(val, &q.min_size))
                return fail("bad size", val);
        } else if (opt(a, L"--max", NULL)) {
            VALUE();
            if (!parse_size(val, &q.max_size))
                return fail("bad size", val);
        } else if (opt(a, L"--size", NULL)) {
            VALUE();
            uint64_t n;
            bool ok = parse_size(val + (val[0] == '+' || val[0] == '-'), &n);
            if (!ok)
                return fail("bad size", val);
            if (val[0] == '+') {
                if (n == UINT64_MAX)
                    return fail("nothing is bigger than", val);
                q.min_size = n + 1;
            } else if (val[0] == '-') {
                if (n == 0)
                    return fail("nothing is smaller than", val);
                q.max_size = n - 1;
            } else {
                q.min_size = q.max_size = n;
            }
        } else if (opt(a, L"--after", NULL) || opt(a, L"--before", NULL) ||
                   opt(a, L"--created-after", NULL) || opt(a, L"--created-before", NULL)) {
            VALUE();
            int64_t t;
            if (!parse_date(val, &t))
                return fail("dates look like 2025-01-31, not", val);
            if (!wcscmp(a, L"--after"))
                q.mtime_lo = t;
            else if (!wcscmp(a, L"--before"))
                q.mtime_hi = t;
            else if (!wcscmp(a, L"--created-after"))
                q.ctime_lo = t;
            else
                q.ctime_hi = t;
        } else if (opt(a, L"--max-depth", L"-d")) {
            VALUE();
            uint64_t n;
            if (!parse_number(val, &n))
                return fail("bad depth", val);
            q.max_depth = n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
        } else if (opt(a, L"--max-results", NULL)) {
            VALUE();
            if (!parse_number(val, &q.max_results))
                return fail("bad number", val);
        } else if (opt(a, L"--threads", L"-j")) {
            VALUE();
            uint64_t n;
            if (!parse_number(val, &n) || n > 256)
                return fail("--threads wants 0 to 256, not", val);
            wo.threads = (int)n;
        } else if (opt(a, L"--timeout", NULL)) {
            VALUE();
            uint64_t n;
            if (!parse_number(val, &n) || n >= INFINITE)
                return fail("bad timeout", val);
            wo.timeout_ms = (DWORD)n;
        } else if (opt(a, L"--out", NULL)) {
            VALUE();
            oo.out_file = val;
        } else if (opt(a, L"--color", NULL)) {
            VALUE();
            if (!_wcsicmp(val, L"auto"))
                oo.color = COLOR_AUTO;
            else if (!_wcsicmp(val, L"always"))
                oo.color = COLOR_ALWAYS;
            else if (!_wcsicmp(val, L"never"))
                oo.color = COLOR_NEVER;
            else
                return fail("--color is auto, always or never, not", val);
        } else {
            return fail("unknown option", a);
        }
#undef VALUE
    }

    /* fq | fq pattern | fq path | fq pattern path */
    const WCHAR *pattern = L"", *path = NULL;
    if (npos == 1 && looks_like_path(pos[0]))
        path = pos[0];
    else if (npos >= 1)
        pattern = pos[0];
    if (npos == 2)
        path = pos[1];

    if (glob && regex)
        return fail("pick one of --glob and --regex", NULL);
    if (q.min_size > q.max_size)
        return fail("no size passes both --min and --max", NULL);
    if (q.mtime_lo > q.mtime_hi || q.ctime_lo > q.ctime_hi)
        return fail("no date passes both the after and before filters", NULL);

    const char *err = NULL;
    if (!pattern_compile(&q.pat, pattern, regex ? PAT_REGEX : glob ? PAT_GLOB : PAT_SUBSTR, icase,
                         &err))
        return fail(err, pattern);

    /* the root: opened as \\?\C:\full\path, printed as typed */
    const WCHAR *typed = path ? path : L".";
    DWORD n = GetFullPathNameW(typed, 0, NULL, NULL);
    WCHAR *full = xmalloc((n + 1) * sizeof *full);
    if (!n || !GetFullPathNameW(typed, n + 1, full, NULL))
        return fail("bad path", typed);
    wo.open = open_path(full);
    DWORD attr = GetFileAttributesW(wo.open);
    if (attr == INVALID_FILE_ATTRIBUTES)
        return fail("no such folder:", typed);
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
        return fail("not a folder:", typed);
    wo.show = path ? show_prefix(path) : L"";
    wo.preview = oo.preview > 0;

    if (!out_open(&oo))
        return 1;

    LARGE_INTEGER t0, t1, hz;
    QueryPerformanceCounter(&t0);
    WalkStats st;
    memset(&st, 0, sizeof st);
    bool ok = walk(&q, &wo, &st);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&hz);
    out_close();
    if (!ok)
        return 1;

    if (st.timed_out && !quiet)
        fq_err("fq: stopped after %lu ms (--timeout), results are incomplete\n",
               (unsigned long)wo.timeout_ms);
    if (stats)
        fq_err("fq: %llu folders, %llu files, %llu matches in %.3f s "
               "(%d threads, %llu steals, %llu folders unreadable)\n",
               (unsigned long long)st.dirs, (unsigned long long)st.files,
               (unsigned long long)st.hits, (double)(t1.QuadPart - t0.QuadPart) / hz.QuadPart,
               st.threads, (unsigned long long)st.steals, (unsigned long long)st.unreadable);
    return 0;
}
