#include "fq.h"
#include <stdlib.h>
#include <string.h>

WCHAR fq_upper[65536];
WCHAR fq_lower[65536];

void fold_init(void)
{
    static bool done;
    if (done)
        return;
    done = true;
    static WCHAR all[65536];
    for (unsigned i = 0; i < 65536; i++)
        all[i] = fq_upper[i] = fq_lower[i] = (WCHAR)i;
    /* skip the surrogates; LCMapStringEx is in kernel32, so no user32.dll */
    static const struct { unsigned from, n; } runs[] = {{1, 0xD7FF}, {0xE000, 0x2000}};
    for (int r = 0; r < 2; r++) {
        int n = (int)runs[r].n;
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, all + runs[r].from, n,
                      fq_upper + runs[r].from, n, NULL, NULL, 0);
        LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, all + runs[r].from, n,
                      fq_lower + runs[r].from, n, NULL, NULL, 0);
    }
}

static inline WCHAR up(WCHAR c) { return fq_upper[c]; }

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fq_err("fq: out of memory\n");
        ExitProcess(2);
    }
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) {
        fq_err("fq: out of memory\n");
        ExitProcess(2);
    }
    return p;
}

WCHAR *wdup(const WCHAR *s, size_t n)
{
    WCHAR *d = xmalloc((n + 1) * sizeof *d);
    memcpy(d, s, n * sizeof *d);
    d[n] = 0;
    return d;
}

char *utf8(const WCHAR *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *d = xmalloc(n > 0 ? (size_t)n : 1);
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, d, n, NULL, NULL))
        d[0] = 0;
    return d;
}

bool class_add(Class *c, WCHAR lo, WCHAR hi)
{
    if (lo > hi)
        return false;
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 4;
        c->lo = xrealloc(c->lo, c->cap * sizeof *c->lo);
        c->hi = xrealloc(c->hi, c->cap * sizeof *c->hi);
    }
    c->lo[c->n] = lo;
    c->hi[c->n] = hi;
    c->n++;
    return true;
}

static bool is_digit(WCHAR c) { return c >= '0' && c <= '9'; }
static bool is_word(WCHAR c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static bool is_space(WCHAR c) { return c == ' ' || (c >= '\t' && c <= '\r'); }

static bool class_has1(const Class *c, WCHAR ch)
{
    for (int i = 0; i < c->n; i++)
        if (ch >= c->lo[i] && ch <= c->hi[i])
            return true;
    unsigned b = c->builtin;
    return ((b & CL_DIGIT) && is_digit(ch)) || ((b & CL_NDIGIT) && !is_digit(ch)) ||
           ((b & CL_WORD) && is_word(ch)) || ((b & CL_NWORD) && !is_word(ch)) ||
           ((b & CL_SPACE) && is_space(ch)) || ((b & CL_NSPACE) && !is_space(ch));
}

bool class_has(const Class *c, WCHAR ch, bool icase)
{
    bool in = class_has1(c, ch) ||
              (icase && (class_has1(c, fq_upper[ch]) || class_has1(c, fq_lower[ch])));
    return in != c->neg;
}

enum { G_LIT, G_ANY, G_STAR, G_CLASS };

typedef struct {
    unsigned char op;
    WCHAR c;     /* G_LIT, folded when icase */
    Class *cls;  /* G_CLASS */
} GTok;

typedef struct Glob {
    GTok *t;
    int n;
} Glob;

/* [...] at s[*i]; NULL if unclosed, and then the [ is literal */
static Class *glob_class(const WCHAR *s, size_t n, size_t *i)
{
    size_t j = *i + 1;
    Class *c = xmalloc(sizeof *c);
    memset(c, 0, sizeof *c);
    if (j < n && (s[j] == '!' || s[j] == '^')) {
        c->neg = true;
        j++;
    }
    bool first = true;
    while (j < n && (s[j] != ']' || first)) {
        first = false;
        WCHAR lo = s[j];
        if (lo == '\\' && j + 1 < n)
            lo = s[++j];
        j++;
        WCHAR hi = lo;
        if (j + 1 < n && s[j] == '-' && s[j + 1] != ']') {
            hi = s[j + 1];
            if (hi == '\\' && j + 2 < n)
                hi = s[++j + 1];
            j += 2;
        }
        if (!class_add(c, lo, hi))
            class_add(c, hi, lo); /* [z-a]: be forgiving */
    }
    if (j >= n) {
        free(c->lo);
        free(c->hi);
        free(c);
        return NULL;
    }
    *i = j + 1;
    return c;
}

static void glob_compile(Glob *g, const WCHAR *s, size_t n, bool icase)
{
    g->t = xmalloc((n + 1) * sizeof *g->t);
    g->n = 0;
    for (size_t i = 0; i < n;) {
        GTok k = {G_LIT, 0, NULL};
        if (s[i] == '*') {
            while (i < n && s[i] == '*')
                i++;
            k.op = G_STAR;
        } else if (s[i] == '?') {
            k.op = G_ANY;
            i++;
        } else if (s[i] == '[' && (k.cls = glob_class(s, n, &i)) != NULL) {
            k.op = G_CLASS;
        } else {
            if (s[i] == '\\' && i + 1 < n)
                i++;
            k.c = icase ? up(s[i]) : s[i];
            i++;
        }
        g->t[g->n++] = k;
    }
}

static bool glob_match(const Glob *g, const WCHAR *s, size_t n, bool icase)
{
    int p = 0, star = -1;
    size_t t = 0, mark = 0;
    while (t < n) {
        if (p < g->n) {
            const GTok *k = &g->t[p];
            if (k->op == G_STAR) {
                star = ++p;
                mark = t;
                continue;
            }
            bool ok;
            switch (k->op) {
            case G_ANY: ok = true; break;
            case G_LIT: ok = k->c == (icase ? up(s[t]) : s[t]); break;
            default: ok = class_has(k->cls, s[t], icase); break;
            }
            if (ok) {
                p++;
                t++;
                continue;
            }
        }
        if (star < 0)
            return false;
        p = star; /* let the last star eat one more character */
        t = ++mark;
    }
    while (p < g->n && g->t[p].op == G_STAR)
        p++;
    return p == g->n;
}

/* a{b,c{d,e}}f -> abf acdf acef, once at compile time */
typedef struct {
    WCHAR **v;
    int n, cap;
} StrList;

static void push_str(StrList *l, WCHAR *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n++] = s;
}

static bool expand_braces(const WCHAR *s, StrList *out)
{
    size_t n = wcslen(s), open = 0;
    int depth = 0;
    bool found = false;
    for (size_t i = 0; i < n && !found; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            continue;
        }
        if (s[i] == '{' && depth++ == 0)
            open = i;
        else if (s[i] == '}' && depth > 0 && --depth == 0) {
            /* split s[open+1 .. i) on top-level commas */
            size_t start = open + 1;
            int d = 0;
            bool comma = false;
            for (size_t j = open + 1; j <= i; j++) {
                if (s[j] == '\\' && j + 1 < i) {
                    j++;
                    continue;
                }
                if (s[j] == '{')
                    d++;
                else if (s[j] == '}' && j < i)
                    d--;
                else if ((s[j] == ',' && d == 0) || j == i) {
                    if (j == i && !comma)
                        break; /* {abc} has no alternatives: literal */
                    comma = true;
                    size_t plen = open, alen = j - start, slen = n - i - 1;
                    WCHAR *t = xmalloc((plen + alen + slen + 1) * sizeof *t);
                    memcpy(t, s, plen * sizeof *t);
                    memcpy(t + plen, s + start, alen * sizeof *t);
                    memcpy(t + plen + alen, s + i + 1, slen * sizeof *t);
                    t[plen + alen + slen] = 0;
                    bool ok = expand_braces(t, out);
                    free(t);
                    if (!ok)
                        return false;
                    start = j + 1;
                }
            }
            found = comma;
        }
    }
    if (!found)
        push_str(out, wdup(s, n));
    return out->n <= 1024;
}

bool pattern_compile(Pattern *p, const WCHAR *pat, int kind, bool icase, const char **err)
{
    memset(p, 0, sizeof *p);
    p->icase = icase;
    size_t n = wcslen(pat);
    if (icase)
        fold_init();

    if (n == 0 || (n == 1 && pat[0] == '*' && kind != PAT_REGEX)) {
        p->kind = PAT_ALL;
        return true;
    }
    p->kind = kind;
    switch (kind) {
    case PAT_SUBSTR:
        p->text = wdup(pat, n);
        p->len = n;
        if (icase)
            for (size_t i = 0; i < n; i++)
                p->text[i] = up(p->text[i]);
        return true;
    case PAT_GLOB: {
        StrList alts = {0};
        if (!expand_braces(pat, &alts)) {
            *err = "glob has too many {,} alternatives";
            return false;
        }
        p->globs = xmalloc(alts.n * sizeof *p->globs);
        p->nglobs = alts.n;
        for (int i = 0; i < alts.n; i++) {
            glob_compile(&p->globs[i], alts.v[i], wcslen(alts.v[i]), icase);
            free(alts.v[i]);
        }
        free(alts.v);
        return true;
    }
    default:
        p->re = regex_compile(pat, icase, err);
        return p->re != NULL;
    }
}

static bool substr_match(const Pattern *p, const WCHAR *s, size_t n)
{
    const WCHAR *k = p->text;
    size_t m = p->len;
    if (m > n)
        return false;
    for (size_t i = 0; i + m <= n; i++) {
        size_t j = 0;
        if (p->icase)
            while (j < m && up(s[i + j]) == k[j])
                j++;
        else
            while (j < m && s[i + j] == k[j])
                j++;
        if (j == m)
            return true;
    }
    return false;
}

bool pattern_match(const Pattern *p, RxScratch *rx, const WCHAR *name, size_t n)
{
    switch (p->kind) {
    case PAT_ALL: return true;
    case PAT_SUBSTR: return substr_match(p, name, n);
    case PAT_GLOB:
        for (int i = 0; i < p->nglobs; i++)
            if (glob_match(&p->globs[i], name, n, p->icase))
                return true;
        return false;
    default: return regex_match(p->re, rx, name, n);
    }
}

/* the extension of a name, without the dot; NULL if there is none */
static const WCHAR *ext_of(const WCHAR *name, size_t n, size_t *len)
{
    for (size_t i = n; i-- > 0;) {
        if (name[i] == '.') {
            if (i + 1 == n)
                return NULL;
            *len = n - i - 1;
            return name + i + 1;
        }
    }
    return NULL;
}

static bool ext_eq(const WCHAR *e, size_t n, const WCHAR *folded)
{
    size_t i = 0;
    for (; i < n && folded[i]; i++)
        if (up(e[i]) != folded[i])
            return false;
    return i == n && folded[i] == 0;
}

bool ext_matches(const Query *q, const WCHAR *name, size_t n)
{
    if (q->nexts == 0)
        return true;
    size_t len;
    const WCHAR *e = ext_of(name, n, &len);
    if (!e)
        return false;
    for (size_t i = 0; i < q->nexts; i++)
        if (ext_eq(e, len, q->exts[i]))
            return true;
    return false;
}

static const WCHAR *const type_exts[][40] = {
    [TYPE_TEXT] = {L"TXT", L"MD", L"C", L"CPP", L"H", L"HPP", L"CS", L"JAVA", L"PY", L"JS",
                   L"TS", L"HTML", L"HTM", L"CSS", L"XML", L"JSON", L"YAML", L"YML", L"INI",
                   L"CFG", L"CONF", L"LOG", L"SQL", L"SH", L"BAT", L"PS1", L"PHP", L"RB",
                   L"GO", L"RS", L"KT", L"SCALA", L"PL", L"R", L"TEX", L"RTF", L"TOML", NULL},
    [TYPE_IMAGE] = {L"JPG", L"JPEG", L"PNG", L"GIF", L"BMP", L"TIFF", L"TIF", L"SVG", L"WEBP",
                    L"ICO", L"PSD", L"AI", L"HEIC", L"AVIF", NULL},
    [TYPE_VIDEO] = {L"MP4", L"AVI", L"MKV", L"MOV", L"WMV", L"FLV", L"WEBM", L"M4V", L"3GP",
                    L"MPG", L"MPEG", NULL},
    [TYPE_AUDIO] = {L"MP3", L"WAV", L"FLAC", L"AAC", L"OGG", L"WMA", L"M4A", L"OPUS", L"AIFF",
                    NULL},
    [TYPE_ARCHIVE] = {L"ZIP", L"RAR", L"7Z", L"TAR", L"GZ", L"BZ2", L"XZ", L"ZST", L"CAB", L"MSI",
                      L"DEB", L"RPM", NULL},
};

int type_of_name(const WCHAR *name, size_t n)
{
    size_t len;
    const WCHAR *e = ext_of(name, n, &len);
    if (!e)
        return TYPE_ANY;
    fold_init();
    for (int t = TYPE_TEXT; t <= TYPE_ARCHIVE; t++)
        for (int i = 0; type_exts[t][i]; i++)
            if (ext_eq(e, len, type_exts[t][i]))
                return t;
    return TYPE_ANY;
}

int type_parse(const WCHAR *s)
{
    static const WCHAR *const names[] = {NULL, L"text", L"image", L"video", L"audio", L"archive"};
    for (int t = TYPE_TEXT; t <= TYPE_ARCHIVE; t++)
        if (_wcsicmp(s, names[t]) == 0)
            return t;
    return -1;
}

const char *type_label(int type)
{
    static const char *const labels[] = {"Unknown", "Text", "Image", "Video", "Audio", "Archive"};
    return labels[type];
}

/* build output and caches, anywhere in the tree */
static const WCHAR *const skip_any[] = {
    L"node_modules", L".git", L".svn", L".hg", L"__pycache__", L".vs", L"obj", L"bin",
    L"Debug", L"Release", L"packages", L"bower_components", L"dist", L"build", NULL,
};

/* system folders, only where Windows puts them: directly under a drive */
static const WCHAR *const skip_root[] = {
    L"$RECYCLE.BIN", L"System Volume Information", L"Windows", L"Program Files",
    L"Program Files (x86)", L"ProgramData", L"Recovery", L"Intel", L"AMD", L"NVIDIA", NULL,
};

static bool name_in(const WCHAR *name, size_t n, const WCHAR *const *list)
{
    for (int i = 0; list[i]; i++)
        if (wcslen(list[i]) == n && _wcsnicmp(name, list[i], n) == 0)
            return true;
    return false;
}

bool skip_dir(const WCHAR *name, size_t n, bool at_drive_root)
{
    return name_in(name, n, skip_any) || (at_drive_root && name_in(name, n, skip_root));
}
