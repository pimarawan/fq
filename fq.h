/* fq: find files on Windows. Names stay UTF-16 until they are printed. */

#ifndef FQ_H
#define FQ_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601 /* Windows 7 */
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING /* older MinGW headers */
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

#define FQ_VERSION "0.9.0"

extern WCHAR fq_upper[65536];
extern WCHAR fq_lower[65536];
void fold_init(void);

enum {
    CL_DIGIT = 1, CL_NDIGIT = 2, CL_WORD = 4, CL_NWORD = 8, CL_SPACE = 16, CL_NSPACE = 32
};
typedef struct {
    WCHAR *lo, *hi; /* inclusive ranges */
    int n, cap;
    unsigned builtin; /* CL_* sets mixed into the class */
    bool neg;
} Class;

bool class_add(Class *c, WCHAR lo, WCHAR hi);
bool class_has(const Class *c, WCHAR ch, bool icase);

typedef struct Regex Regex;
typedef struct RxScratch RxScratch;

enum { PAT_ALL, PAT_SUBSTR, PAT_GLOB, PAT_REGEX };

typedef struct {
    int kind;
    bool icase;
    WCHAR *text; /* substring, folded to upper case when icase */
    size_t len;
    struct Glob *globs; /* one per brace alternative: *.{c,h} -> *.c, *.h */
    int nglobs;
    Regex *re;
} Pattern;

bool pattern_compile(Pattern *p, const WCHAR *pat, int kind, bool icase, const char **err);
bool pattern_match(const Pattern *p, RxScratch *rx, const WCHAR *name, size_t n);

enum { TYPE_ANY, TYPE_TEXT, TYPE_IMAGE, TYPE_VIDEO, TYPE_AUDIO, TYPE_ARCHIVE };
int type_of_name(const WCHAR *name, size_t n); /* by extension; TYPE_ANY if unknown */
int type_parse(const WCHAR *s);                /* "image" -> TYPE_IMAGE, -1 if bad */
const char *type_label(int type);

typedef struct {
    Pattern pat;
    WCHAR **exts; /* folded, without the dot */
    size_t nexts;
    int type;
    uint64_t min_size, max_size; /* inclusive */
    int64_t mtime_lo, mtime_hi;  /* FILETIME ticks, inclusive */
    int64_t ctime_lo, ctime_hi;
    bool files, dirs;    /* what to print */
    bool hidden;         /* include hidden entries */
    bool follow;         /* descend into symlinked and junctioned directories */
    bool skip;           /* skip node_modules, .git, C:\Windows ... */
    uint32_t max_depth;  /* 0 = the root directory only */
    uint64_t max_results; /* 0 = unlimited */
} Query;

bool ext_matches(const Query *q, const WCHAR *name, size_t n);
bool skip_dir(const WCHAR *name, size_t n, bool at_drive_root);

Regex *regex_compile(const WCHAR *pat, bool icase, const char **err);
RxScratch *regex_scratch(const Regex *re);
bool regex_match(const Regex *re, RxScratch *s, const WCHAR *text, size_t n);

enum { COLOR_AUTO, COLOR_ALWAYS, COLOR_NEVER };

typedef struct {
    const WCHAR *out_file; /* NULL: stdout */
    int color;
    bool json;
    int preview; /* lines per text file, 0 = no preview */
} OutOptions;

typedef struct {
    const WCHAR *path; /* what gets printed */
    size_t len;
    size_t name;       /* offset of the last component */
    const WCHAR *open; /* \\?\ path, only set when previewing */
    bool dir;
    uint64_t size;
    int64_t mtime, ctime;
} Hit;

typedef struct {
    WCHAR *path;
    bool dir;
    uint64_t size;
    int64_t mtime, ctime;
} Rec;

/* per worker, so formatting needs no lock */
typedef struct {
    WCHAR *buf;
    size_t len, cap;
    Rec *recs; /* JSON mode keeps everything until the end */
    size_t nrec, caprec;
} OutBuf;

bool out_open(const OutOptions *o);
void out_hit(OutBuf *b, const Hit *h);
void out_flush(OutBuf *b);
void out_finish(OutBuf *bufs, int n); /* flush everything; JSON prints here */
void out_close(void);

void fq_err(const char *fmt, ...);

typedef struct {
    uint64_t dirs, files, hits, unreadable, steals;
    int threads;
    bool timed_out;
} WalkStats;

typedef struct {
    const WCHAR *open; /* \\?\C:\full\path of the root */
    const WCHAR *show; /* prefix printed before every path, may be empty */
    int threads;       /* 0 = pick */
    DWORD timeout_ms;  /* 0 = none */
    bool preview;      /* hits need their \\?\ path too */
} WalkOptions;

/* set to stop the walk early: max results, timeout, broken pipe */
extern volatile LONG fq_stop;

bool walk(const Query *q, const WalkOptions *o, WalkStats *st);

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
WCHAR *wdup(const WCHAR *s, size_t n);
char *utf8(const WCHAR *s); /* malloc'd, for messages */

#endif
