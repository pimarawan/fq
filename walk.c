/*
 * Directories are read with GetFileInformationByHandleEx(FileFullDirectoryInfo):
 * one call fills a 64 KiB buffer with hundreds of entries. For reparse points
 * the EaSize field holds the reparse tag. FQ_ENUM=find switches to
 * FindFirstFileExW and FQ_BUF=<KiB> changes the buffer, for benchmarking.
 *
 * Work stealing (Blumofe and Leiserson, JACM 1999): a worker takes its own
 * newest directory, so alone it walks depth-first; an idle worker steals the
 * oldest directory of a random other worker, which is the biggest subtree.
 */

#include "fq.h"
#include <stdlib.h>
#include <string.h>

volatile LONG fq_stop;

/* FILE_FULL_DIR_INFO; our own copy so MinGW and MSVC agree */
typedef struct {
    ULONG NextEntryOffset;
    ULONG FileIndex;
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    LARGE_INTEGER EndOfFile;
    LARGE_INTEGER AllocationSize;
    ULONG FileAttributes;
    ULONG FileNameLength; /* bytes */
    ULONG EaSize;         /* the reparse tag, for reparse points */
    WCHAR FileName[1];
} FullDirInfo;

#define FILE_FULL_DIRECTORY_INFO ((FILE_INFO_BY_HANDLE_CLASS)14)

typedef struct {
    WCHAR *rel; /* path below the root, "" for the root itself */
    uint32_t len;
    uint32_t depth;
    uint64_t *anc; /* with -L: (volume, file index) of each folder above */
    uint32_t nanc;
} Job;

typedef struct {
    SRWLOCK lock;
    Job *v;
    size_t head, tail, cap; /* live jobs are v[head..tail) */
} Deque;

typedef struct {
    Deque dq;
    HANDLE thread;
    int id;
    uint32_t rng;
    BYTE *buf;
    WCHAR *path; /* the directory being read, \\?\ form */
    size_t pathcap;
    WCHAR *line; /* the entry being printed */
    size_t linecap;
    WCHAR *open; /* the entry, \\?\ form, for --preview */
    size_t opencap;
    uint64_t *anc; /* with -L: the chain for the folder being read */
    uint32_t nanc, capanc;
    OutBuf out;
    RxScratch *rx;
    uint64_t dirs, files, hits, unreadable, steals;
    char pad[64]; /* keep workers off each other's cache lines */
} Worker;

static struct {
    const Query *q;
    Worker *w;
    int n;
    volatile LONG64 pending; /* jobs pushed and not yet finished */
    volatile LONG64 queued;  /* jobs sitting in a deque */
    volatile LONG idle;
    volatile LONG64 hits;
    SRWLOCK idle_lock;
    CONDITION_VARIABLE idle_cv;
    HANDLE done;
    const WCHAR *open;
    size_t open_len;
    const WCHAR *show;
    size_t show_len;
    bool drive_root;
    bool use_find;
    bool preview;
    DWORD bufsize;
} W;

static LONG64 load(volatile LONG64 *p) { return InterlockedCompareExchange64(p, 0, 0); }

static void reserve(WCHAR **p, size_t *cap, size_t n)
{
    if (n > *cap) {
        *cap = n < 512 ? 512 : n * 2;
        *p = xrealloc(*p, *cap * sizeof **p);
    }
}

/* -L: a folder that is its own ancestor is a cycle and is not entered.
 * Two links to the same place are both followed, like find -L. */
static bool enter(Worker *w, const Job *j, HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION fi;
    uint64_t vol = 0, idx = 0;
    bool known = GetFileInformationByHandle(h, &fi) != 0;
    if (known) {
        vol = fi.dwVolumeSerialNumber;
        idx = (uint64_t)fi.nFileIndexHigh << 32 | fi.nFileIndexLow;
        for (uint32_t i = 0; i < j->nanc; i++)
            if (j->anc[2 * i] == vol && j->anc[2 * i + 1] == idx)
                return false;
    }
    if (j->nanc + 1 > w->capanc) {
        w->capanc = (j->nanc + 1) * 2;
        w->anc = xrealloc(w->anc, w->capanc * 2 * sizeof *w->anc);
    }
    if (j->nanc)
        memcpy(w->anc, j->anc, j->nanc * 2 * sizeof *w->anc);
    w->nanc = j->nanc;
    if (known) {
        w->anc[2 * w->nanc] = vol;
        w->anc[2 * w->nanc + 1] = idx;
        w->nanc++;
    }
    return true;
}

static void wake(bool all)
{
    AcquireSRWLockExclusive(&W.idle_lock);
    if (all)
        WakeAllConditionVariable(&W.idle_cv);
    else
        WakeConditionVariable(&W.idle_cv);
    ReleaseSRWLockExclusive(&W.idle_lock);
}

static void push(Worker *w, Job j)
{
    Deque *d = &w->dq;
    /* count it before anyone can steal and finish it, or pending could hit
     * zero while there is still work */
    InterlockedIncrement64(&W.pending);
    AcquireSRWLockExclusive(&d->lock);
    if (d->tail == d->cap) {
        if (d->head > 0) {
            memmove(d->v, d->v + d->head, (d->tail - d->head) * sizeof *d->v);
            d->tail -= d->head;
            d->head = 0;
        }
        if (d->tail == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 64;
            d->v = xrealloc(d->v, d->cap * sizeof *d->v);
        }
    }
    d->v[d->tail++] = j;
    ReleaseSRWLockExclusive(&d->lock);
    InterlockedIncrement64(&W.queued);
    if (InterlockedCompareExchange(&W.idle, 0, 0) > 0)
        wake(false);
}

static bool take(Deque *d, Job *j, bool newest)
{
    bool got = false;
    if (d->tail > d->head) {
        *j = newest ? d->v[--d->tail] : d->v[d->head++];
        if (d->head == d->tail)
            d->head = d->tail = 0;
        got = true;
    }
    return got;
}

static bool pop(Worker *w, Job *j)
{
    AcquireSRWLockExclusive(&w->dq.lock);
    bool got = take(&w->dq, j, true);
    ReleaseSRWLockExclusive(&w->dq.lock);
    if (got)
        InterlockedDecrement64(&W.queued);
    return got;
}

static bool steal(Worker *w, Job *j)
{
    if (W.n < 2)
        return false;
    for (int round = 0; round < 2; round++) {
        for (int k = 0; k < W.n; k++) {
            w->rng ^= w->rng << 13; /* xorshift32 */
            w->rng ^= w->rng >> 17;
            w->rng ^= w->rng << 5;
            Worker *v = &W.w[w->rng % W.n];
            if (v == w)
                continue;
            /* the first round never waits for a busy owner */
            if (round == 0) {
                if (!TryAcquireSRWLockExclusive(&v->dq.lock))
                    continue;
            } else {
                AcquireSRWLockExclusive(&v->dq.lock);
            }
            bool got = take(&v->dq, j, false);
            ReleaseSRWLockExclusive(&v->dq.lock);
            if (got) {
                InterlockedDecrement64(&W.queued);
                w->steals++;
                return true;
            }
        }
    }
    return false;
}

static void finish(void)
{
    if (InterlockedDecrement64(&W.pending) == 0) {
        wake(true);
        SetEvent(W.done);
    }
}

/* sleep until there is something to steal or nothing left at all */
static bool wait_for_work(void)
{
    AcquireSRWLockExclusive(&W.idle_lock);
    InterlockedIncrement(&W.idle);
    while (load(&W.queued) == 0 && load(&W.pending) > 0)
        SleepConditionVariableSRW(&W.idle_cv, &W.idle_lock, INFINITE, 0);
    InterlockedDecrement(&W.idle);
    bool more = load(&W.pending) > 0;
    ReleaseSRWLockExclusive(&W.idle_lock);
    return more;
}

static bool in(int64_t v, int64_t lo, int64_t hi) { return v >= lo && v <= hi; }

static void emit(Worker *w, const Job *j, size_t pathlen, const WCHAR *name, size_t n, bool dir,
                 uint64_t size, int64_t mtime, int64_t ctime)
{
    const Query *q = W.q;
    LONG64 k = 0;
    if (q->max_results) {
        k = InterlockedIncrement64(&W.hits);
        if ((uint64_t)k > q->max_results) {
            InterlockedExchange(&fq_stop, 1);
            return;
        }
    }

    /* printed path: <show><rel>\<name> */
    size_t len = W.show_len + j->len + 1 + n;
    reserve(&w->line, &w->linecap, len + 1);
    WCHAR *p = w->line;
    memcpy(p, W.show, W.show_len * sizeof *p);
    p += W.show_len;
    if (j->len) {
        memcpy(p, j->rel, j->len * sizeof *p);
        p += j->len;
        *p++ = '\\';
    }
    size_t name_at = (size_t)(p - w->line);
    memcpy(p, name, n * sizeof *p);
    p += n;
    *p = 0;

    Hit h = {w->line, (size_t)(p - w->line), name_at, NULL, dir, size, mtime, ctime};
    if (W.preview) {
        reserve(&w->open, &w->opencap, pathlen + n + 2);
        memcpy(w->open, w->path, pathlen * sizeof *w->open);
        size_t m = pathlen;
        if (w->open[m - 1] != '\\')
            w->open[m++] = '\\';
        memcpy(w->open + m, name, n * sizeof *w->open);
        w->open[m + n] = 0;
        h.open = w->open;
    }
    out_hit(&w->out, &h);
    w->hits++;
    if (q->max_results && (uint64_t)k == q->max_results)
        InterlockedExchange(&fq_stop, 1);
}

static void entry(Worker *w, const Job *j, size_t pathlen, const WCHAR *name, size_t n,
                  DWORD attr, DWORD tag, int64_t size, int64_t mtime, int64_t ctime)
{
    const Query *q = W.q;
    if (name[0] == '.' && (n == 1 || (n == 2 && name[1] == '.')))
        return;
    if (!q->hidden && ((attr & FILE_ATTRIBUTE_HIDDEN) || name[0] == '.'))
        return;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (q->dirs && in(mtime, q->mtime_lo, q->mtime_hi) && in(ctime, q->ctime_lo, q->ctime_hi) &&
            pattern_match(&q->pat, w->rx, name, n))
            emit(w, j, pathlen, name, n, true, 0, mtime, ctime);

        /* only symlinks and junctions; OneDrive placeholders etc. are folders */
        bool link = (attr & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    (tag == IO_REPARSE_TAG_SYMLINK || tag == IO_REPARSE_TAG_MOUNT_POINT);
        if (fq_stop || j->depth >= q->max_depth || (link && !q->follow) ||
            (q->skip && skip_dir(name, n, j->depth == 0 && W.drive_root)))
            return;

        Job c;
        c.len = (uint32_t)(j->len + (j->len ? 1 : 0) + n);
        c.rel = xmalloc((c.len + 1) * sizeof *c.rel);
        WCHAR *p = c.rel;
        if (j->len) {
            memcpy(p, j->rel, j->len * sizeof *p);
            p += j->len;
            *p++ = '\\';
        }
        memcpy(p, name, n * sizeof *p);
        c.rel[c.len] = 0;
        c.depth = j->depth + 1;
        c.anc = NULL;
        c.nanc = 0;
        if (q->follow && w->nanc) {
            c.nanc = w->nanc;
            c.anc = xmalloc(c.nanc * 2 * sizeof *c.anc);
            memcpy(c.anc, w->anc, c.nanc * 2 * sizeof *c.anc);
        }
        push(w, c);
        return;
    }

    w->files++;
    if (!q->files || (uint64_t)size < q->min_size || (uint64_t)size > q->max_size ||
        !in(mtime, q->mtime_lo, q->mtime_hi) || !in(ctime, q->ctime_lo, q->ctime_hi) ||
        !ext_matches(q, name, n) || (q->type != TYPE_ANY && type_of_name(name, n) != q->type) ||
        !pattern_match(&q->pat, w->rx, name, n))
        return;
    emit(w, j, pathlen, name, n, false, (uint64_t)size, mtime, ctime);
}

static void read_by_handle(Worker *w, const Job *j, size_t pathlen)
{
    HANDLE h = CreateFileW(w->path, FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        w->unreadable++;
        return;
    }
    if (W.q->follow && !enter(w, j, h)) {
        CloseHandle(h);
        return;
    }
    while (!fq_stop) {
        if (!GetFileInformationByHandleEx(h, FILE_FULL_DIRECTORY_INFO, w->buf, W.bufsize)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                w->unreadable++;
            break;
        }
        for (BYTE *p = w->buf;;) {
            const FullDirInfo *e = (const FullDirInfo *)p;
            entry(w, j, pathlen, e->FileName, e->FileNameLength / sizeof(WCHAR), e->FileAttributes,
                  e->EaSize, e->EndOfFile.QuadPart, e->LastWriteTime.QuadPart,
                  e->CreationTime.QuadPart);
            if (!e->NextEntryOffset || fq_stop)
                break;
            p += e->NextEntryOffset;
        }
    }
    CloseHandle(h);
}

static int64_t ft(FILETIME t) { return (int64_t)((uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime); }

static void read_by_find(Worker *w, const Job *j, size_t pathlen)
{
    if (W.q->follow) {
        HANDLE h = CreateFileW(w->path, FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            w->unreadable++;
            return;
        }
        bool first = enter(w, j, h);
        CloseHandle(h);
        if (!first)
            return;
    }
    size_t m = pathlen;
    if (w->path[m - 1] != '\\')
        w->path[m++] = '\\';
    w->path[m++] = '*';
    w->path[m] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(w->path, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL,
                                FIND_FIRST_EX_LARGE_FETCH);
    w->path[pathlen] = 0;
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            w->unreadable++;
        return;
    }
    do {
        int64_t size = (int64_t)((uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow);
        entry(w, j, pathlen, fd.cFileName, wcslen(fd.cFileName), fd.dwFileAttributes,
              fd.dwReserved0, size, ft(fd.ftLastWriteTime), ft(fd.ftCreationTime));
    } while (!fq_stop && FindNextFileW(f, &fd));
    FindClose(f);
}

static void run(Worker *w, const Job *j)
{
    if (fq_stop)
        return;
    size_t len = W.open_len;
    reserve(&w->path, &w->pathcap, len + 1 + j->len + 3);
    memcpy(w->path, W.open, len * sizeof *w->path);
    if (j->len) {
        if (w->path[len - 1] != '\\')
            w->path[len++] = '\\';
        memcpy(w->path + len, j->rel, j->len * sizeof *w->path);
        len += j->len;
    }
    w->path[len] = 0;
    w->dirs++;
    if (W.use_find)
        read_by_find(w, j, len);
    else
        read_by_handle(w, j, len);
}

static DWORD WINAPI worker(void *arg)
{
    Worker *w = arg;
    Job j;
    for (;;) {
        if (pop(w, &j) || steal(w, &j)) {
            run(w, &j);
            free(j.rel);
            free(j.anc);
            finish();
            continue;
        }
        out_flush(&w->out); /* about to go idle: let finished lines out */
        if (!wait_for_work())
            break;
    }
    out_flush(&w->out);
    return 0;
}

static int default_threads(void)
{
    /* 8 beat 12 on a 6-core/12-thread CPU, see README */
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return cpus < 1 ? 1 : cpus > 8 ? 8 : (int)cpus;
}

bool walk(const Query *q, const WalkOptions *o, WalkStats *st)
{
    memset(&W, 0, sizeof W);
    W.q = q;
    W.open = o->open;
    W.open_len = wcslen(o->open);
    W.show = o->show;
    W.show_len = wcslen(o->show);
    W.drive_root = W.open_len == 7 && W.open[5] == ':' && W.open[6] == '\\';
    W.preview = o->preview;
    InitializeSRWLock(&W.idle_lock);
    InitializeConditionVariable(&W.idle_cv);

    WCHAR env[32];
    W.use_find = GetEnvironmentVariableW(L"FQ_ENUM", env, 32) && _wcsicmp(env, L"find") == 0;
    W.bufsize = 64 * 1024;
    if (GetEnvironmentVariableW(L"FQ_BUF", env, 32)) {
        long kb = wcstol(env, NULL, 10);
        if (kb >= 1 && kb <= 1024)
            W.bufsize = (DWORD)kb * 1024;
    }

    W.n = o->threads > 0 ? (o->threads > 256 ? 256 : o->threads) : default_threads();
    W.w = xmalloc(W.n * sizeof *W.w);
    memset(W.w, 0, W.n * sizeof *W.w);
    for (int i = 0; i < W.n; i++) {
        Worker *w = &W.w[i];
        w->id = i;
        w->rng = 2463534242u ^ (uint32_t)(i * 2654435761u);
        if (!w->rng)
            w->rng = 1;
        InitializeSRWLock(&w->dq.lock);
        w->buf = xmalloc(W.bufsize);
        w->rx = q->pat.kind == PAT_REGEX ? regex_scratch(q->pat.re) : NULL;
    }

    W.done = CreateEventW(NULL, TRUE, FALSE, NULL);
    Job root = {wdup(L"", 0), 0, 0, NULL, 0};
    push(&W.w[0], root);

    for (int i = 0; i < W.n; i++) {
        W.w[i].thread = CreateThread(NULL, 0, worker, &W.w[i], 0, NULL);
        if (!W.w[i].thread) {
            fq_err("fq: cannot start threads\n");
            return false;
        }
    }

    if (WaitForSingleObject(W.done, o->timeout_ms ? o->timeout_ms : INFINITE) == WAIT_TIMEOUT) {
        st->timed_out = true;
        InterlockedExchange(&fq_stop, 1);
        WaitForSingleObject(W.done, INFINITE);
    }
    for (int i = 0; i < W.n; i++) {
        WaitForSingleObject(W.w[i].thread, INFINITE);
        CloseHandle(W.w[i].thread);
    }

    OutBuf *bufs = xmalloc(W.n * sizeof *bufs);
    for (int i = 0; i < W.n; i++) {
        Worker *w = &W.w[i];
        st->dirs += w->dirs;
        st->files += w->files;
        st->hits += w->hits;
        st->unreadable += w->unreadable;
        st->steals += w->steals;
        bufs[i] = w->out;
    }
    st->threads = W.n;
    out_finish(bufs, W.n);
    free(bufs);
    return true;
}
