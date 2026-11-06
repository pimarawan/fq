/*
 * Thompson NFA run in lockstep, no backtracking: O(n*m) for a name of n code
 * units and a program of m instructions. See Russ Cox, "Regular Expression
 * Matching Can Be Simple And Fast" (2007).
 *
 * Supported: . ^ $ | () (?:) [] [^] * + ? {m} {m,} {m,n} \d \w \s \D \W \S
 */

#include "fq.h"
#include <stdlib.h>
#include <string.h>

enum { I_CHAR, I_ANY, I_CLASS, I_BOL, I_EOL, I_SPLIT, I_JMP, I_MATCH };

typedef struct {
    unsigned char op;
    WCHAR c;  /* I_CHAR, already folded when icase */
    int x, y; /* jump targets, or x = class index */
} Inst;

struct Regex {
    Inst *prog;
    int n;
    Class *cls;
    int ncls;
    bool icase;
    bool anchored; /* starts with ^: only try position 0 */
};

struct RxScratch {
    int *cur, *next, *stack;
    unsigned *mark;
    unsigned gen;
};

enum { N_EMPTY, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_CAT, N_ALT, N_REP };

typedef struct Node {
    int type;
    WCHAR c;
    int cls;
    int min, max; /* N_REP, max < 0 means unbounded */
    struct Node *a, *b;
} Node;

typedef struct {
    const WCHAR *s;
    size_t i, n;
    const char *err;
    Regex *re;
    Node *pool;
    int npool, cappool;
} Parser;

#define MAX_INST 20000
#define MAX_REP 1000

static Node *node(Parser *p, int type, Node *a, Node *b)
{
    if (p->npool == p->cappool) {
        /* grow by allocating a fresh block: old nodes must not move */
        p->cappool = 64;
        p->pool = xmalloc(p->cappool * sizeof *p->pool);
        p->npool = 0;
    }
    Node *n = &p->pool[p->npool++];
    memset(n, 0, sizeof *n);
    n->type = type;
    n->a = a;
    n->b = b;
    return n;
}

static int new_class(Parser *p)
{
    Regex *re = p->re;
    re->cls = xrealloc(re->cls, (re->ncls + 1) * sizeof *re->cls);
    memset(&re->cls[re->ncls], 0, sizeof re->cls[0]);
    return re->ncls++;
}

static unsigned builtin(WCHAR c)
{
    switch (c) {
    case 'd': return CL_DIGIT;
    case 'D': return CL_NDIGIT;
    case 'w': return CL_WORD;
    case 'W': return CL_NWORD;
    case 's': return CL_SPACE;
    case 'S': return CL_NSPACE;
    }
    return 0;
}

static bool is_alnum(WCHAR c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* \t \n \r and punctuation; letters and digits with no meaning are errors */
static bool escape_char(Parser *p, WCHAR c, WCHAR *out)
{
    switch (c) {
    case 't': *out = '\t'; return true;
    case 'n': *out = '\n'; return true;
    case 'r': *out = '\r'; return true;
    }
    if (is_alnum(c)) {
        p->err = "unsupported escape in regex";
        return false;
    }
    *out = c;
    return true;
}

static Node *parse_class(Parser *p)
{
    int ci = new_class(p);
    Class *c = &p->re->cls[ci];
    p->i++; /* '[' */
    if (p->i < p->n && p->s[p->i] == '^') {
        c->neg = true;
        p->i++;
    }
    bool first = true;
    while (p->i < p->n && (p->s[p->i] != ']' || first)) {
        first = false;
        WCHAR lo = p->s[p->i++];
        if (lo == '\\') {
            if (p->i >= p->n)
                break;
            WCHAR e = p->s[p->i++];
            unsigned b = builtin(e);
            if (b) {
                c->builtin |= b;
                continue;
            }
            if (!escape_char(p, e, &lo))
                return NULL;
        }
        WCHAR hi = lo;
        if (p->i + 1 < p->n && p->s[p->i] == '-' && p->s[p->i + 1] != ']') {
            p->i++;
            hi = p->s[p->i++];
            if (hi == '\\') {
                if (p->i >= p->n)
                    break;
                if (!escape_char(p, p->s[p->i++], &hi))
                    return NULL;
            }
        }
        if (!class_add(c, lo, hi)) {
            p->err = "bad range in regex class";
            return NULL;
        }
    }
    if (p->i >= p->n) {
        p->err = "missing ] in regex";
        return NULL;
    }
    p->i++; /* ']' */
    Node *n = node(p, N_CLASS, NULL, NULL);
    n->cls = ci;
    return n;
}

static Node *parse_alt(Parser *p);

static Node *parse_atom(Parser *p)
{
    WCHAR c = p->s[p->i];
    Node *n;
    switch (c) {
    case '(':
        p->i++;
        if (p->i + 1 < p->n && p->s[p->i] == '?') {
            if (p->s[p->i + 1] != ':') {
                p->err = "only (?:...) groups are supported";
                return NULL;
            }
            p->i += 2;
        }
        n = parse_alt(p);
        if (!n)
            return NULL;
        if (p->i >= p->n || p->s[p->i] != ')') {
            p->err = "missing ) in regex";
            return NULL;
        }
        p->i++;
        return n;
    case '[': return parse_class(p);
    case '.': p->i++; return node(p, N_ANY, NULL, NULL);
    case '^': p->i++; return node(p, N_BOL, NULL, NULL);
    case '$': p->i++; return node(p, N_EOL, NULL, NULL);
    case '*': case '+': case '?':
        p->err = "nothing to repeat in regex";
        return NULL;
    case '\\': {
        if (++p->i >= p->n) {
            p->err = "trailing \\ in regex";
            return NULL;
        }
        WCHAR e = p->s[p->i++];
        unsigned b = builtin(e);
        if (b) {
            n = node(p, N_CLASS, NULL, NULL);
            n->cls = new_class(p);
            p->re->cls[n->cls].builtin = b;
            return n;
        }
        if (!escape_char(p, e, &c))
            return NULL;
        n = node(p, N_CHAR, NULL, NULL);
        n->c = c;
        return n;
    }
    }
    p->i++;
    n = node(p, N_CHAR, NULL, NULL);
    n->c = c;
    return n;
}

/* {m}, {m,}, {m,n}. Anything else is a literal '{'. */
static bool parse_count(Parser *p, int *min, int *max)
{
    size_t i = p->i + 1;
    int a = 0, b, digits = 0;
    while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9' && a <= MAX_REP)
        a = a * 10 + (p->s[i++] - '0'), digits++;
    if (!digits)
        return false;
    b = a;
    if (i < p->n && p->s[i] == ',') {
        i++;
        if (i < p->n && p->s[i] == '}')
            b = -1;
        else {
            b = digits = 0;
            while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9' && b <= MAX_REP)
                b = b * 10 + (p->s[i++] - '0'), digits++;
            if (!digits)
                return false;
        }
    }
    if (i >= p->n || p->s[i] != '}')
        return false;
    p->i = i + 1;
    *min = a;
    *max = b;
    return true;
}

static Node *parse_repeat(Parser *p)
{
    Node *n = parse_atom(p);
    while (n && p->i < p->n) {
        WCHAR c = p->s[p->i];
        int min, max;
        if (c == '*')
            min = 0, max = -1, p->i++;
        else if (c == '+')
            min = 1, max = -1, p->i++;
        else if (c == '?')
            min = 0, max = 1, p->i++;
        else if (c == '{' && parse_count(p, &min, &max)) {
            if (min > MAX_REP || max > MAX_REP || (max >= 0 && max < min)) {
                p->err = "bad {m,n} in regex";
                return NULL;
            }
        } else
            break;
        if (p->i < p->n && p->s[p->i] == '?')
            p->i++; /* lazy: same answer for a yes/no match */
        if (n->type == N_BOL || n->type == N_EOL) {
            p->err = "cannot repeat ^ or $";
            return NULL;
        }
        Node *r = node(p, N_REP, n, NULL);
        r->min = min;
        r->max = max;
        n = r;
    }
    return n;
}

static Node *parse_cat(Parser *p)
{
    Node *left = NULL;
    while (p->i < p->n && p->s[p->i] != '|' && p->s[p->i] != ')') {
        Node *n = parse_repeat(p);
        if (!n)
            return NULL;
        left = left ? node(p, N_CAT, left, n) : n;
    }
    return left ? left : node(p, N_EMPTY, NULL, NULL);
}

static Node *parse_alt(Parser *p)
{
    Node *left = parse_cat(p);
    while (left && p->i < p->n && p->s[p->i] == '|') {
        p->i++;
        Node *right = parse_cat(p);
        if (!right)
            return NULL;
        left = node(p, N_ALT, left, right);
    }
    return left;
}

typedef struct {
    Inst *v;
    int n, cap;
    bool overflow;
} Code;

static int emit(Code *c, int op)
{
    if (c->n >= MAX_INST) {
        c->overflow = true;
        return 0;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->v = xrealloc(c->v, c->cap * sizeof *c->v);
    }
    memset(&c->v[c->n], 0, sizeof c->v[0]);
    c->v[c->n].op = (unsigned char)op;
    return c->n++;
}

static void gen(Code *c, const Node *n, bool icase)
{
    int a, b;
    if (c->overflow)
        return;
    switch (n->type) {
    case N_EMPTY: break;
    case N_CHAR:
        a = emit(c, I_CHAR);
        c->v[a].c = icase ? fq_upper[n->c] : n->c;
        break;
    case N_ANY: emit(c, I_ANY); break;
    case N_CLASS:
        a = emit(c, I_CLASS);
        c->v[a].x = n->cls;
        break;
    case N_BOL: emit(c, I_BOL); break;
    case N_EOL: emit(c, I_EOL); break;
    case N_CAT:
        gen(c, n->a, icase);
        gen(c, n->b, icase);
        break;
    case N_ALT: /* split L1, L2; L1: a; jmp L3; L2: b; L3: */
        a = emit(c, I_SPLIT);
        c->v[a].x = c->n;
        gen(c, n->a, icase);
        b = emit(c, I_JMP);
        c->v[a].y = c->n;
        gen(c, n->b, icase);
        c->v[b].x = c->n;
        break;
    case N_REP: {
        for (int i = 0; i < n->min; i++)
            gen(c, n->a, icase);
        if (n->max < 0) { /* L1: split L2, L3; L2: a; jmp L1; L3: */
            a = emit(c, I_SPLIT);
            c->v[a].x = c->n;
            gen(c, n->a, icase);
            b = emit(c, I_JMP);
            c->v[b].x = a;
            c->v[a].y = c->n;
        } else { /* (max - min) nested optional copies */
            int first = c->n, k = n->max - n->min;
            for (int i = 0; i < k; i++) {
                a = emit(c, I_SPLIT);
                c->v[a].x = c->n;
                gen(c, n->a, icase);
            }
            for (int i = first; i < c->n && !c->overflow; i++)
                if (c->v[i].op == I_SPLIT && c->v[i].y == 0 && c->v[i].x == i + 1)
                    c->v[i].y = c->n;
        }
        break;
    }
    }
}

Regex *regex_compile(const WCHAR *pat, bool icase, const char **err)
{
    Regex *re = xmalloc(sizeof *re);
    memset(re, 0, sizeof *re);
    re->icase = icase;
    Parser p = {pat, 0, wcslen(pat), NULL, re, NULL, 0, 0};

    Node *tree = parse_alt(&p);
    if (tree && p.i < p.n) /* a ')' with no '(' */
        p.err = "unmatched ) in regex";
    if (!tree || p.err) {
        *err = p.err ? p.err : "bad regex";
        return NULL; /* the parse tree is leaked: we are about to exit */
    }

    Code c = {0};
    gen(&c, tree, icase);
    emit(&c, I_MATCH);
    if (c.overflow) {
        *err = "regex is too large";
        return NULL;
    }
    re->prog = c.v;
    re->n = c.n;
    re->anchored = re->n > 0 && re->prog[0].op == I_BOL;
    return re;
}

RxScratch *regex_scratch(const Regex *re)
{
    RxScratch *s = xmalloc(sizeof *s);
    size_t n = re ? (size_t)re->n : 1;
    s->cur = xmalloc(n * sizeof *s->cur);
    s->next = xmalloc(n * sizeof *s->next);
    s->stack = xmalloc(2 * n * sizeof *s->stack);
    s->mark = xmalloc(n * sizeof *s->mark);
    memset(s->mark, 0, n * sizeof *s->mark);
    s->gen = 0;
    return s;
}

/* add pc to list, following jumps; mark[] lets each pc in once per step */
static bool add(const Regex *re, RxScratch *s, int *list, int *len, int pc, size_t pos, size_t n)
{
    int sp = 0;
    s->stack[sp++] = pc;
    while (sp > 0) {
        pc = s->stack[--sp];
        if (s->mark[pc] == s->gen)
            continue;
        s->mark[pc] = s->gen;
        const Inst *in = &re->prog[pc];
        switch (in->op) {
        case I_JMP: s->stack[sp++] = in->x; break;
        case I_SPLIT:
            s->stack[sp++] = in->y;
            s->stack[sp++] = in->x;
            break;
        case I_BOL:
            if (pos == 0)
                s->stack[sp++] = pc + 1;
            break;
        case I_EOL:
            if (pos == n)
                s->stack[sp++] = pc + 1;
            break;
        case I_MATCH: return true;
        default: list[(*len)++] = pc; break;
        }
    }
    return false;
}

static void next_gen(RxScratch *s, int n)
{
    if (++s->gen == 0) { /* wrapped: clear the marks once every 4 billion steps */
        memset(s->mark, 0, n * sizeof *s->mark);
        s->gen = 1;
    }
}

bool regex_match(const Regex *re, RxScratch *s, const WCHAR *t, size_t n)
{
    int ncur = 0, nnext;
    next_gen(s, re->n);
    for (size_t i = 0;; i++) {
        /* unanchored: a new thread starts at every position */
        if ((i == 0 || !re->anchored) && add(re, s, s->cur, &ncur, 0, i, n))
            return true;
        if (i == n || ncur == 0) {
            if (i == n || re->anchored)
                return false;
            next_gen(s, re->n); /* fresh marks for the next start thread */
            continue;
        }
        WCHAR c = t[i], f = re->icase ? fq_upper[c] : c;
        next_gen(s, re->n);
        nnext = 0;
        for (int k = 0; k < ncur; k++) {
            const Inst *in = &re->prog[s->cur[k]];
            bool ok;
            switch (in->op) {
            case I_CHAR: ok = in->c == f; break;
            case I_ANY: ok = true; break;
            default: ok = class_has(&re->cls[in->x], c, re->icase); break;
            }
            if (ok && add(re, s, s->next, &nnext, s->cur[k] + 1, i + 1, n))
                return true;
        }
        int *tmp = s->cur;
        s->cur = s->next;
        s->next = tmp;
        ncur = nnext;
    }
}
