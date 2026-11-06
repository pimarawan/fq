/* Console: WriteConsoleW, so any name prints right. Pipes and files: UTF-8. */

#include "fq.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FLUSH_AT (16 * 1024) /* code units buffered per worker */

static struct {
    HANDLE h;
    bool own, console, color, json, broken;
    int preview;
    SRWLOCK lock;
    char *u8;
    size_t u8cap;
} O;

static void sink(const WCHAR *s, size_t n)
{
    if (O.broken)
        return;
    if (O.console) {
        while (n) {
            DWORD k = n > 8192 ? 8192 : (DWORD)n, done;
            if (k < n && IS_HIGH_SURROGATE(s[k - 1]))
                k--; /* never split a surrogate pair across writes */
            if (!WriteConsoleW(O.h, s, k, &done, NULL) || !done) {
                O.broken = true;
                return;
            }
            s += done;
            n -= done;
        }
        return;
    }
    while (n) {
        int k = n > (1 << 20) ? 1 << 20 : (int)n;
        if ((size_t)k < n && IS_HIGH_SURROGATE(s[k - 1]))
            k--;
        int m = WideCharToMultiByte(CP_UTF8, 0, s, k, NULL, 0, NULL, NULL);
        if ((size_t)m > O.u8cap) {
            O.u8cap = (size_t)m * 2;
            O.u8 = xrealloc(O.u8, O.u8cap);
        }
        WideCharToMultiByte(CP_UTF8, 0, s, k, O.u8, m, NULL, NULL);
        for (char *p = O.u8; m > 0;) {
            DWORD done;
            if (!WriteFile(O.h, p, (DWORD)m, &done, NULL) || !done) {
                O.broken = true; /* closed pipe: no point walking on */
                InterlockedExchange(&fq_stop, 1);
                return;
            }
            p += done;
            m -= (int)done;
        }
        s += k;
        n -= (size_t)k;
    }
}

static void put(WCHAR **buf, size_t *len, size_t *cap, const WCHAR *s, size_t n)
{
    if (*len + n > *cap) {
        *cap = (*len + n) * 2 + 256;
        *buf = xrealloc(*buf, *cap * sizeof **buf);
    }
    memcpy(*buf + *len, s, n * sizeof **buf);
    *len += n;
}

static void puta(WCHAR **buf, size_t *len, size_t *cap, const char *s)
{
    WCHAR tmp[128];
    size_t n = 0;
    while (*s) {
        tmp[n++] = (unsigned char)*s++;
        if (n == 128) {
            put(buf, len, cap, tmp, n);
            n = 0;
        }
    }
    put(buf, len, cap, tmp, n);
}

#define ESC_DIR L"\x1b[1;34m"
#define ESC_PARENT L"\x1b[34m"
#define ESC_RESET L"\x1b[0m"

static void line(WCHAR **buf, size_t *len, size_t *cap, const Hit *h)
{
    if (!O.color) {
        put(buf, len, cap, h->path, h->len);
    } else if (h->dir) {
        put(buf, len, cap, ESC_DIR, 7);
        put(buf, len, cap, h->path, h->len);
        put(buf, len, cap, ESC_RESET, 4);
    } else {
        if (h->name) { /* folders in blue, the file name as is */
            put(buf, len, cap, ESC_PARENT, 5);
            put(buf, len, cap, h->path, h->name);
            put(buf, len, cap, ESC_RESET, 4);
        }
        put(buf, len, cap, h->path + h->name, h->len - h->name);
    }
    put(buf, len, cap, L"\n", 1);
}

static void human_size(uint64_t n, char *s, size_t cap)
{
    if (n < 1024)
        snprintf(s, cap, "%llu bytes", (unsigned long long)n);
    else if (n < 1024 * 1024)
        snprintf(s, cap, "%.1f KB", n / 1024.0);
    else if (n < 1024ULL * 1024 * 1024)
        snprintf(s, cap, "%.1f MB", n / (1024.0 * 1024));
    else
        snprintf(s, cap, "%.1f GB", n / (1024.0 * 1024 * 1024));
}

/* no NULs and 95% printable; bytes >= 0x80 count, for UTF-8 */
static bool looks_like_text(const unsigned char *b, size_t n)
{
    size_t ok = 0, m = n < 512 ? n : 512;
    for (size_t i = 0; i < m; i++) {
        if (b[i] == 0)
            return false;
        ok += b[i] >= 0x20 || b[i] == '\t' || b[i] == '\n' || b[i] == '\r';
    }
    return m == 0 || ok * 100 >= m * 95;
}

static void preview(WCHAR **buf, size_t *len, size_t *cap, const Hit *h)
{
    if (h->dir) {
        puta(buf, len, cap, "  [Directory]\n\n");
        return;
    }
    HANDLE f = CreateFileW(h->open, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        puta(buf, len, cap, "  [cannot open]\n\n");
        return;
    }
    static unsigned char raw[64 * 1024];
    DWORD n = 0;
    ReadFile(f, raw, sizeof raw, &n, NULL);
    CloseHandle(f);

    int type = type_of_name(h->path + h->name, h->len - h->name);
    bool utf16 = n >= 2 && raw[0] == 0xFF && raw[1] == 0xFE;
    if (!utf16 && type != TYPE_TEXT && !looks_like_text(raw, n)) {
        char s[96], size[32];
        human_size(h->size, size, sizeof size);
        snprintf(s, sizeof s, "  Type: %s, Size: %s\n\n", type_label(type), size);
        puta(buf, len, cap, s);
        return;
    }

    /* decode to UTF-16 */
    static WCHAR text[64 * 1024];
    size_t tn;
    if (utf16) {
        tn = (n - 2) / 2;
        memcpy(text, raw + 2, tn * 2);
    } else {
        size_t skip = n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF ? 3 : 0;
        int k = MultiByteToWideChar(CP_UTF8, 0, (char *)raw + skip, (int)(n - skip), text, 64 * 1024);
        tn = k > 0 ? (size_t)k : 0;
    }

    int lines = 0;
    size_t i = 0;
    while (i < tn && lines < O.preview) {
        size_t start = i;
        while (i < tn && text[i] != '\n')
            i++;
        size_t end = i;
        if (end > start && text[end - 1] == '\r')
            end--;
        puta(buf, len, cap, "  ");
        if (end - start > 100) {
            put(buf, len, cap, text + start, 97);
            puta(buf, len, cap, "...");
        } else {
            put(buf, len, cap, text + start, end - start);
        }
        puta(buf, len, cap, "\n");
        lines++;
        if (i < tn)
            i++; /* the '\n' */
    }
    if (lines == 0)
        puta(buf, len, cap, "  [empty file]\n");
    else if (i < tn || h->size > n)
        puta(buf, len, cap, "  [...]\n");
    puta(buf, len, cap, "\n");
}

void out_flush(OutBuf *b)
{
    if (!b->len)
        return;
    AcquireSRWLockExclusive(&O.lock);
    sink(b->buf, b->len);
    ReleaseSRWLockExclusive(&O.lock);
    b->len = 0;
}

void out_hit(OutBuf *b, const Hit *h)
{
    if (O.json) {
        if (b->nrec == b->caprec) {
            b->caprec = b->caprec ? b->caprec * 2 : 256;
            b->recs = xrealloc(b->recs, b->caprec * sizeof *b->recs);
        }
        Rec r = {wdup(h->path, h->len), h->dir, h->size, h->mtime, h->ctime};
        b->recs[b->nrec++] = r;
        return;
    }
    if (O.preview) { /* a path and its preview have to stay together */
        static WCHAR *pbuf;
        static size_t plen, pcap;
        AcquireSRWLockExclusive(&O.lock);
        plen = 0;
        line(&pbuf, &plen, &pcap, h);
        preview(&pbuf, &plen, &pcap, h);
        sink(pbuf, plen);
        ReleaseSRWLockExclusive(&O.lock);
        return;
    }
    line(&b->buf, &b->len, &b->cap, h);
    if (b->len >= FLUSH_AT)
        out_flush(b);
}

static void json_str(WCHAR **buf, size_t *len, size_t *cap, const WCHAR *s)
{
    put(buf, len, cap, L"\"", 1);
    for (; *s; s++) {
        WCHAR c = *s;
        char esc[8];
        switch (c) {
        case '"': puta(buf, len, cap, "\\\""); break;
        case '\\': puta(buf, len, cap, "\\\\"); break;
        case '\b': puta(buf, len, cap, "\\b"); break;
        case '\f': puta(buf, len, cap, "\\f"); break;
        case '\n': puta(buf, len, cap, "\\n"); break;
        case '\r': puta(buf, len, cap, "\\r"); break;
        case '\t': puta(buf, len, cap, "\\t"); break;
        default:
            if (c < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", c);
                puta(buf, len, cap, esc);
            } else {
                put(buf, len, cap, &c, 1);
            }
        }
    }
    put(buf, len, cap, L"\"", 1);
}

static void iso_time(int64_t t, char *s, size_t cap)
{
    FILETIME ft = {(DWORD)t, (DWORD)((uint64_t)t >> 32)};
    SYSTEMTIME st;
    if (!FileTimeToSystemTime(&ft, &st))
        snprintf(s, cap, "invalid-date");
    else
        snprintf(s, cap, "%04d-%02d-%02dT%02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour,
                 st.wMinute, st.wSecond);
}

void out_finish(OutBuf *bufs, int n)
{
    for (int i = 0; i < n; i++)
        out_flush(&bufs[i]);
    if (!O.json)
        return;

    size_t count = 0;
    for (int i = 0; i < n; i++)
        count += bufs[i].nrec;

    WCHAR *buf = NULL;
    size_t len = 0, cap = 0;
    char s[512]; /* one record's fixed part is about 150 bytes */
    puta(&buf, &len, &cap, "{\n  \"type\": \"search\",\n  \"version\": \"" FQ_VERSION "\",\n");
    snprintf(s, sizeof s, "  \"count\": %llu,\n  \"results\": [\n", (unsigned long long)count);
    puta(&buf, &len, &cap, s);

    size_t k = 0;
    AcquireSRWLockExclusive(&O.lock);
    for (int i = 0; i < n; i++) {
        for (size_t j = 0; j < bufs[i].nrec; j++) {
            const Rec *r = &bufs[i].recs[j];
            char mod[48], cre[48];
            iso_time(r->mtime, mod, sizeof mod);
            iso_time(r->ctime, cre, sizeof cre);
            puta(&buf, &len, &cap, "    {\n      \"path\": ");
            json_str(&buf, &len, &cap, r->path);
            snprintf(s, sizeof s,
                     ",\n      \"directory\": %s,\n      \"size\": %llu,\n"
                     "      \"modified\": \"%s\",\n      \"created\": \"%s\"\n    }%s\n",
                     r->dir ? "true" : "false", (unsigned long long)r->size, mod, cre,
                     ++k < count ? "," : "");
            puta(&buf, &len, &cap, s);
            if (len > 64 * 1024) {
                sink(buf, len);
                len = 0;
            }
        }
    }
    puta(&buf, &len, &cap, "  ]\n}\n");
    sink(buf, len);
    ReleaseSRWLockExclusive(&O.lock);
    free(buf);
}

static bool is_console(HANDLE h)
{
    DWORD mode;
    return h && h != INVALID_HANDLE_VALUE && GetFileType(h) == FILE_TYPE_CHAR &&
           GetConsoleMode(h, &mode);
}

bool out_open(const OutOptions *o)
{
    InitializeSRWLock(&O.lock);
    O.json = o->json;
    O.preview = o->preview;
    if (o->out_file) {
        O.h = CreateFileW(o->out_file, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
        if (O.h == INVALID_HANDLE_VALUE) {
            char *name = utf8(o->out_file);
            fq_err("fq: cannot write '%s'\n", name);
            free(name);
            return false;
        }
        O.own = true;
    } else {
        O.h = GetStdHandle(STD_OUTPUT_HANDLE);
        O.console = is_console(O.h);
    }

    WCHAR env[4];
    bool no_color = GetEnvironmentVariableW(L"NO_COLOR", env, 4) > 0; /* no-color.org */
    if (O.json || o->out_file || o->color == COLOR_NEVER)
        O.color = false;
    else if (o->color == COLOR_ALWAYS)
        O.color = true;
    else
        O.color = O.console && !no_color;

    if (O.color && O.console) {
        DWORD mode;
        if (!GetConsoleMode(O.h, &mode) ||
            !SetConsoleMode(O.h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
            O.color = false;
    }
    return true;
}

void out_close(void)
{
    if (O.own)
        CloseHandle(O.h);
}

void fq_err(const char *fmt, ...)
{
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof msg)
        n = sizeof msg - 1;
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    DWORD done;
    if (is_console(h)) {
        WCHAR w[2048];
        int k = MultiByteToWideChar(CP_UTF8, 0, msg, n, w, 2048);
        WriteConsoleW(h, w, (DWORD)k, &done, NULL);
    } else {
        WriteFile(h, msg, (DWORD)n, &done, NULL);
    }
}
