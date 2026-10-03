/* vt.c - terminal emulation core. See vt.h. */
#define _GNU_SOURCE
#include "vt.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "lz.h"

#define MAX_PARAMS 24
#define OSC_MAX 4096
#define MAX_STYLES (1u << 20)
#define MAX_COMBS 2047

typedef enum { ST_GROUND, ST_ESC, ST_CSI, ST_OSC, ST_OSC_ESC, ST_STR, ST_STR_ESC, ST_CHARSET, ST_SKIP1 } State;
enum { CS_US = 0, CS_GRAPHICS = 1 };

/* A screen: rows*cols cells in fixed slots; order[y] says which slot currently shows row y. */
typedef struct {
    VtCell *cells;
    VtLineMeta *meta;
    int *order;
} Scr;

/* A line in the scrollback: trailing blanks are not stored. */
typedef struct {
    VtLineMeta meta;
    int len;
    VtCell cells[];
} HLine;

typedef struct {
    int x, y;
    uint32_t fg, bg;
    uint16_t attrs;
    uint8_t g[2], gl;
} Saved;

#define BLOCK_LINES 128
#define HOT_KEEP 1024
#define CACHE_ENTRIES 6
#define PENDING_MAX ((size_t)8 << 20)   /* raw bytes waiting for a worker */
#define DEFAULT_RAM_BUDGET ((size_t)32 << 20)
#define DEFAULT_DISK_BUDGET ((size_t)4 << 30)

typedef struct Job {
    atomic_int refs, done;
    uint8_t *raw; size_t raw_size;
    uint8_t *out; size_t out_size;
    int stored;                    /* incompressible: the raw bytes are kept as they are */
    int nlines;
} Job;

typedef struct {
    uint64_t id;
    int nlines;
    uint32_t raw_size, comp_size;
    uint8_t *data;                 /* compressed bytes in memory, or NULL */
    int64_t disk_off;              /* offset in the spill file, or -1 */
    int stored;
    int shuffled;                  /* compressed data is in the byte-plane layout */
    Job *job;                      /* compression still running (raw bytes live in the job) */
} Block;

typedef struct {                   /* a decoded block, kept so a scrolled-back view is cheap */
    uint64_t id;
    int nlines;
    VtCell *cells;
    int off[BLOCK_LINES + 1];
    VtLineMeta meta[BLOCK_LINES];
    uint64_t stamp;
} CacheEntry;

struct Vt {
    int cols, rows;
    Scr main, alt, *cur;
    /* history: a ring of recent lines kept as they are, then compressed blocks of older lines */
    int max_lines;                 /* -1 = unlimited */
    HLine **hist;
    int hcap, hhead, hcount;
    size_t hot_bytes;
    Block *blocks;
    int bcap, bhead, bcount;
    long block_lines;
    uint64_t next_block_id;
    size_t ram_budget, disk_budget, packed_bytes, pending_bytes, disk_bytes, raw_total;
    int pending_blocks;            /* blocks whose compression job has not been picked up yet */
    bool spill;
    int spill_fd;
    int64_t spill_end;
    uint64_t spill_next_id;        /* oldest block that may still be in memory */
    CacheEntry *cache[CACHE_ENTRIES];
    uint64_t cache_clock;

    int cx, cy; /* cx == cols means a wrap is pending */
    uint32_t pen_fg, pen_bg;
    uint16_t pen_attrs;
    uint32_t pen_style, erase_style;
    bool erase_valid;
    int top, bottom;
    uint32_t modes;
    uint8_t *tabs;
    Saved saved_main, saved_alt, saved_1049;
    uint8_t g[2], gl;
    int cursor_style;
    uint32_t last_cp;

    VtStyle *styles;
    uint32_t nstyles, cstyles;
    uint32_t *style_hash;
    uint32_t hash_mask;
    uint32_t combs[MAX_COMBS + 1];
    unsigned ncombs;

    State st;
    int utf_need;
    uint32_t utf_cp;
    int par[MAX_PARAMS];
    uint8_t sub[MAX_PARAMS]; /* param was introduced by ':' */
    int np;
    bool have_digit;
    char priv, inter;
    int charset_slot;
    char osc[OSC_MAX + 1];
    int osc_len;

    VtWriteFn write_fn;
    VtEventFn event_fn;
    VtCacheFree cache_free;
    void *user;
};

/* ---- width table ------------------------------------------------------------------- */

typedef struct { uint32_t lo, hi; } Range;

static const Range ZERO_WIDTH[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},
    {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4}, {0x0711, 0x0711},
    {0x0730, 0x074A}, {0x0900, 0x0902}, {0x093C, 0x093C}, {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0E31, 0x0E31},
    {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E}, {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x200B, 0x200F}, {0x2028, 0x202E},
    {0x2060, 0x2064}, {0x20D0, 0x20FF}, {0x302A, 0x302D}, {0x3099, 0x309A}, {0xFE00, 0xFE0F}, {0xFE20, 0xFE2F},
    {0xFEFF, 0xFEFF}, {0xE0100, 0xE01EF},
};
static const Range WIDE[] = {
    {0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F}, {0x2693, 0x2693}, {0x26A1, 0x26A1},
    {0x26AA, 0x26AB}, {0x26BD, 0x26BE}, {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD}, {0x2705, 0x2705}, {0x270A, 0x270B},
    {0x2728, 0x2728}, {0x274C, 0x274C}, {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2B1B, 0x2B1C}, {0x2B50, 0x2B50}, {0x2B55, 0x2B55}, {0x2E80, 0x303E},
    {0x3041, 0x33FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60}, {0xFFE0, 0xFFE6}, {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E},
    {0x1F191, 0x1F19A}, {0x1F200, 0x1F320}, {0x1F32D, 0x1F335}, {0x1F337, 0x1F37C}, {0x1F37E, 0x1F393},
    {0x1F3A0, 0x1F3CA}, {0x1F3CF, 0x1F3D3}, {0x1F3E0, 0x1F3F0}, {0x1F3F4, 0x1F3F4}, {0x1F3F8, 0x1F43E},
    {0x1F440, 0x1F440}, {0x1F442, 0x1F4FC}, {0x1F4FF, 0x1F53D}, {0x1F54B, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F57A, 0x1F57A}, {0x1F595, 0x1F596}, {0x1F5A4, 0x1F5A4}, {0x1F5FB, 0x1F64F}, {0x1F680, 0x1F6C5},
    {0x1F6CC, 0x1F6CC}, {0x1F6D0, 0x1F6D2}, {0x1F6D5, 0x1F6D7}, {0x1F6EB, 0x1F6EC}, {0x1F6F4, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F93A}, {0x1F93C, 0x1F945}, {0x1F947, 0x1F9FF}, {0x1FA70, 0x1FAFF},
    {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
};

static bool in_table(uint32_t cp, const Range *tab, size_t n) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (cp < tab[mid].lo) hi = mid;
        else if (cp > tab[mid].hi) lo = mid + 1;
        else return true;
    }
    return false;
}

int vt_wcwidth(uint32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return -1;
    if (cp < 0x300) return 1;
    if (in_table(cp, ZERO_WIDTH, sizeof ZERO_WIDTH / sizeof *ZERO_WIDTH)) return 0;
    if (cp >= 0x1100 && in_table(cp, WIDE, sizeof WIDE / sizeof *WIDE)) return 2;
    return 1;
}



/* ---- styles ------------------------------------------------------------------------------------- */

static inline uint32_t hash_style(uint32_t fg, uint32_t bg, uint16_t attrs) {
    uint32_t h = fg * 2654435761u;
    h ^= bg + 0x9e3779b9u + (h << 6) + (h >> 2);
    h ^= attrs * 40503u;
    h ^= h >> 15;
    return h;
}

static void style_table_init(Vt *t) {
    t->cstyles = 64;
    t->styles = calloc(t->cstyles, sizeof(VtStyle));
    t->nstyles = 1;   /* id 0 is the default style */
    t->hash_mask = 127;
    t->style_hash = calloc(t->hash_mask + 1, sizeof(uint32_t));
}

static uint32_t intern_style(Vt *t, uint32_t fg, uint32_t bg, uint16_t attrs) {
    if (!fg && !bg && !attrs) return 0;
    uint32_t i = hash_style(fg, bg, attrs) & t->hash_mask;
    while (t->style_hash[i]) {
        const VtStyle *s = &t->styles[t->style_hash[i]];
        if (s->fg == fg && s->bg == bg && s->attrs == attrs) return t->style_hash[i];
        i = (i + 1) & t->hash_mask;
    }
    if (t->nstyles >= MAX_STYLES) return 0;   /* pathological output: degrade to the default style */
    if (t->nstyles == t->cstyles) {
        t->cstyles *= 2;
        t->styles = realloc(t->styles, t->cstyles * sizeof(VtStyle));
    }
    uint32_t id = t->nstyles++;
    t->styles[id] = (VtStyle){fg, bg, attrs};
    if (t->nstyles * 2 > t->hash_mask) {   /* grow and rehash */
        uint32_t nm = (t->hash_mask + 1) * 2 - 1;
        uint32_t *nh = calloc(nm + 1, sizeof(uint32_t));
        for (uint32_t k = 1; k < t->nstyles; k++) {
            uint32_t j = hash_style(t->styles[k].fg, t->styles[k].bg, t->styles[k].attrs) & nm;
            while (nh[j]) j = (j + 1) & nm;
            nh[j] = k;
        }
        free(t->style_hash);
        t->style_hash = nh;
        t->hash_mask = nm;
    } else {
        t->style_hash[i] = id;
        return id;
    }
    return id;
}

static inline void pen_commit(Vt *t) {
    t->pen_style = intern_style(t, t->pen_fg, t->pen_bg, t->pen_attrs);
    t->erase_valid = false;   /* erased cells take only the background; computed on first use */
}

static inline uint32_t erase_style(Vt *t) {
    if (!t->erase_valid) { t->erase_style = intern_style(t, 0, t->pen_bg, 0); t->erase_valid = true; }
    return t->erase_style;
}

const VtStyle *vt_style(const Vt *t, uint32_t id) { return id < t->nstyles ? &t->styles[id] : &t->styles[0]; }
uint32_t vt_comb_char(const Vt *t, unsigned idx) { return idx && idx <= t->ncombs ? t->combs[idx] : 0; }

static unsigned comb_index(Vt *t, uint32_t cp) {
    for (unsigned i = 1; i <= t->ncombs; i++) if (t->combs[i] == cp) return i;
    if (t->ncombs >= MAX_COMBS) return 0;
    t->combs[++t->ncombs] = cp;
    return t->ncombs;
}

/* ---- screens ------------------------------------------------------------------------------------ */

static void scr_alloc(Scr *s, int cols, int rows) {
    s->cells = calloc((size_t)cols * (size_t)rows, sizeof(VtCell));
    s->meta = calloc((size_t)rows, sizeof(VtLineMeta));
    s->order = malloc((size_t)rows * sizeof(int));
    for (int i = 0; i < rows; i++) { s->order[i] = i; s->meta[i].dirty = 1; }
}

static void scr_release_caches(Vt *t, Scr *s, int rows) {
    if (!s->meta) return;
    for (int i = 0; i < rows; i++) {
        if (s->meta[i].cache && t->cache_free) t->cache_free(s->meta[i].cache, t->user);
        s->meta[i].cache = NULL;
    }
}

static void scr_free(Vt *t, Scr *s, int rows) {
    scr_release_caches(t, s, rows);
    free(s->cells); free(s->meta); free(s->order);
    memset(s, 0, sizeof *s);
}

static inline VtCell *rowp(Vt *t, int y) { return t->cur->cells + (size_t)t->cur->order[y] * (size_t)t->cols; }
static inline VtLineMeta *rowm(Vt *t, int y) { return &t->cur->meta[t->cur->order[y]]; }
static inline void dirty(Vt *t, int y) { rowm(t, y)->dirty = 1; }

static inline VtCell fill_cell(Vt *t) { VtCell c = {0, erase_style(t) << 8}; return c; }

static void clear_cells(Vt *t, VtCell *p, int n) {
    VtCell f = fill_cell(t);
    for (int i = 0; i < n; i++) p[i] = f;
}

/* an edit that may touch any cell of the row: the high-water mark falls back to the full width */
static inline void touch_row(Vt *t, int y) {
    VtLineMeta *m = rowm(t, y);
    m->dirty = 1;
    m->hw = (uint16_t)t->cols;
}

static void clear_row(Vt *t, int y) {
    VtLineMeta *m = rowm(t, y);
    VtCell *r = rowp(t, y);
    if (erase_style(t) == 0) {   /* default blanks: only the cells that were written need clearing */
        memset(r, 0, (size_t)m->hw * sizeof(VtCell));
        m->hw = 0;
    } else {
        clear_cells(t, r, t->cols);
        m->hw = (uint16_t)t->cols;
    }
    m->dirty = 1;
}

/* ---- scrollback --------------------------------------------------------------------------------- */
/* Recent lines stay in `hist` exactly as written. When that ring fills, its oldest BLOCK_LINES lines are
 * packed into a block: byte-shuffled (so the code-point and style planes become runs of zeros) and
 * compressed on a worker thread. Blocks beyond the memory budget move to an unlinked temporary file;
 * beyond the disk budget (or the line limit) the oldest block is dropped. */

static long total_lines(const Vt *t) { return (long)t->hcount + t->block_lines; }

static void hist_release(Vt *t, HLine *h) {
    if (h->meta.cache && t->cache_free) t->cache_free(h->meta.cache, t->user);
    t->hot_bytes -= sizeof(HLine) + (size_t)h->len * sizeof(VtCell);
    free(h);
}

/* ---- compression workers: all cores, started on first use ---------------------------------------- */

typedef struct Task { Job *job; struct Task *next; } Task;
static struct { pthread_mutex_t m; pthread_cond_t c; Task *head, *tail; int started; } pool = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, NULL, NULL, 0};

static void job_unref(Job *j) {
    if (atomic_fetch_sub(&j->refs, 1) == 1) { free(j->raw); free(j->out); free(j); }
}

/* Byte-shuffle the cells (all low bytes, then all second bytes, ...): the code-point and style
 * planes turn into long runs, which the compressor squeezes much harder. Done here, off the
 * parsing thread. Layout in: lens (2n bytes) then cells; out: lens then 8 planes. */
static uint8_t *shuffle_block(const uint8_t *raw, size_t raw_size, int nlines) {
    uint8_t *sh = malloc(raw_size ? raw_size : 1);
    if (!sh) return NULL;
    size_t head = 2 * (size_t)nlines, ncells = (raw_size - head) / 8;
    memcpy(sh, raw, head);
    const uint8_t *cells = raw + head;
    uint8_t *planes = sh + head;
    for (size_t c = 0; c < ncells; c++) {
        const uint8_t *src = cells + 8 * c;
        for (int pl = 0; pl < 8; pl++) planes[(size_t)pl * ncells + c] = src[pl];
    }
    return sh;
}

static void job_run(Job *j) {
    uint8_t *sh = shuffle_block(j->raw, j->raw_size, j->nlines);
    uint8_t *out = sh ? malloc(sd_lz_bound(j->raw_size)) : NULL;
    size_t n = out ? sd_lz_compress(sh, j->raw_size, out, sd_lz_bound(j->raw_size)) : 0;
    free(sh);
    if (n == 0 || n >= j->raw_size) { free(out); j->out = NULL; j->stored = 1; j->out_size = j->raw_size; }
    else {
        uint8_t *shrunk = realloc(out, n);
        j->out = shrunk ? shrunk : out;
        j->out_size = n;
    }
    atomic_store_explicit(&j->done, 1, memory_order_release);
    job_unref(j);
}

static void *pool_worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&pool.m);
        while (!pool.head) pthread_cond_wait(&pool.c, &pool.m);
        Task *tk = pool.head;
        pool.head = tk->next;
        if (!pool.head) pool.tail = NULL;
        pthread_mutex_unlock(&pool.m);
        Job *j = tk->job;
        free(tk);
        job_run(j);
    }
    return NULL;
}

static void pool_submit(Job *j) {
    if (getenv("SD_NOPOOL")) { job_run(j); return; }
    Task *tk = malloc(sizeof *tk);
    if (!tk) { job_run(j); return; }   /* out of memory: compress right here */
    tk->job = j; tk->next = NULL;
    pthread_mutex_lock(&pool.m);
    if (!pool.started) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        if (n < 1) n = 1;
        if (n > 16) n = 16;
        for (long i = 0; i < n; i++) {
            pthread_t th;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            pthread_attr_setstacksize(&at, 256 * 1024);   /* the compressor needs ~40 KB of stack */
            if (pthread_create(&th, &at, pool_worker, NULL) == 0) pool.started++;
            pthread_attr_destroy(&at);
        }
        if (!pool.started) pool.started = -1;
    }
    int usable = pool.started > 0;
    if (usable) {
        if (pool.tail) pool.tail->next = tk; else pool.head = tk;
        pool.tail = tk;
        pthread_cond_signal(&pool.c);
    }
    pthread_mutex_unlock(&pool.m);
    if (!usable) { free(tk); job_run(j); }
}

/* ---- blocks ---------------------------------------------------------------------------------------- */

static inline Block *blk(const Vt *t, int i) { return &t->blocks[(t->bhead + i) % t->bcap]; }

/* pick up a finished compression job */
static void block_apply(Vt *t, Block *b) {
    Job *j = b->job;
    if (!j || !atomic_load_explicit(&j->done, memory_order_acquire)) return;
    t->pending_bytes -= b->raw_size;
    t->pending_blocks--;
    if (j->stored) { b->data = j->raw; j->raw = NULL; b->comp_size = b->raw_size; b->stored = 1; b->shuffled = 0; }
    else { b->data = j->out; j->out = NULL; b->comp_size = (uint32_t)j->out_size; b->stored = 0; b->shuffled = 1; }
    t->packed_bytes += b->comp_size;
    b->job = NULL;
    job_unref(j);   /* frees the raw bytes if they were not taken */
}

static void blocks_poll(Vt *t) {
    /* jobs can finish out of order, so look at every block that still has one */
    int left = t->pending_blocks;
    for (int i = t->bcount - 1; i >= 0 && left > 0; i--) {
        Block *b = blk(t, i);
        if (!b->job) continue;
        left--;
        block_apply(t, b);
    }
}

static void cache_drop_id(Vt *t, uint64_t id) {
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        CacheEntry *e = t->cache[i];
        if (!e || e->id != id) continue;
        for (int k = 0; k < e->nlines; k++)
            if (e->meta[k].cache && t->cache_free) t->cache_free(e->meta[k].cache, t->user);
        free(e->cells); free(e);
        t->cache[i] = NULL;
    }
}

static void spill_close(Vt *t) { if (t->spill_fd >= 0) close(t->spill_fd); t->spill_fd = -1; t->spill_end = 0; }

static void block_free_storage(Vt *t, Block *b) {
    if (b->job) { t->pending_bytes -= b->raw_size; t->pending_blocks--; job_unref(b->job); b->job = NULL; }
    if (b->data) { free(b->data); t->packed_bytes -= b->comp_size; b->data = NULL; }
    if (b->disk_off >= 0) {
#ifdef FALLOC_FL_PUNCH_HOLE
        if (t->spill_fd >= 0) (void)fallocate(t->spill_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, b->disk_off, b->comp_size);
#endif
        t->disk_bytes -= b->comp_size;
        b->disk_off = -1;
    }
}

static void evict_oldest_block(Vt *t) {
    if (!t->bcount) return;
    Block *b = blk(t, 0);
    cache_drop_id(t, b->id);
    block_free_storage(t, b);
    t->raw_total -= b->raw_size;
    t->block_lines -= b->nlines;
    t->bhead = (t->bhead + 1) % t->bcap;
    t->bcount--;
    if (t->bcount == 0) { spill_close(t); t->bhead = 0; }
}

static int spill_open(Vt *t) {
    if (t->spill_fd >= 0) return 0;
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = "/tmp";
    int fd = -1;
#ifdef O_TMPFILE
    fd = open(dir, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#endif
    if (fd < 0) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/lestrix-hist-XXXXXX", dir);
        fd = mkstemp(path);
        if (fd >= 0) { unlink(path); fcntl(fd, F_SETFD, FD_CLOEXEC); }
    }
    if (fd < 0) return -1;
    t->spill_fd = fd;
    t->spill_end = 0;
    return 0;
}

static int block_spill(Vt *t, Block *b) {
    if (b->job || !b->data || spill_open(t) != 0) return -1;
    const uint8_t *p = b->data;
    size_t left = b->comp_size;
    int64_t off = t->spill_end;
    while (left) {
        ssize_t w = pwrite(t->spill_fd, p, left, off + (int64_t)(p - b->data));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; left -= (size_t)w;
    }
    t->spill_end += b->comp_size;
    free(b->data);
    b->data = NULL;
    b->disk_off = off;
    t->packed_bytes -= b->comp_size;
    t->disk_bytes += b->comp_size;
    return 0;
}

static void enforce_limits(Vt *t) {
    if (t->max_lines > 0)
        while (total_lines(t) > t->max_lines) {
            if (t->bcount) evict_oldest_block(t);
            else if (t->hcount) {
                hist_release(t, t->hist[t->hhead]);
                t->hhead = (t->hhead + 1) % t->hcap;
                t->hcount--;
            } else break;
        }
    while (t->bcount && t->packed_bytes + t->pending_bytes > t->ram_budget) {
        if (t->spill && t->disk_bytes < t->disk_budget) {
            if (t->spill_next_id < blk(t, 0)->id) t->spill_next_id = blk(t, 0)->id;
            uint64_t idx = t->spill_next_id - blk(t, 0)->id;
            if (idx >= (uint64_t)t->bcount) break;
            Block *b = blk(t, (int)idx);
            if (b->job) block_apply(t, b);
            if (b->job || !b->data) break;          /* still compressing: try again after the next push */
            if (block_spill(t, b) != 0) { t->spill = false; continue; }
            t->spill_next_id++;
        } else {
            evict_oldest_block(t);
        }
    }
    while (t->bcount && t->disk_bytes > t->disk_budget) evict_oldest_block(t);
}

/* Pack the oldest BLOCK_LINES hot lines into a block. */
static void pack_oldest(Vt *t) {
    int n = BLOCK_LINES;
    if (t->hcount < n) return;
    blocks_poll(t);
    size_t ncells = 0;
    for (int i = 0; i < n; i++) ncells += (size_t)t->hist[(t->hhead + i) % t->hcap]->len;
    size_t raw_size = 2 * (size_t)n + 8 * ncells;
    uint8_t *raw = malloc(raw_size);
    Job *j = malloc(sizeof *j);
    if (!raw || !j || raw_size > UINT32_MAX) {    /* cannot pack: drop the oldest line so memory stays bounded */
        free(raw); free(j);
        hist_release(t, t->hist[t->hhead]);
        t->hhead = (t->hhead + 1) % t->hcap;
        t->hcount--;
        return;
    }
    uint8_t *lens = raw, *cells = raw + 2 * (size_t)n;
    for (int i = 0; i < n; i++) {
        HLine *h = t->hist[(t->hhead + i) % t->hcap];
        lens[2 * i] = (uint8_t)h->len;
        lens[2 * i + 1] = (uint8_t)((unsigned)h->len >> 8);
        memcpy(cells, h->cells, (size_t)h->len * sizeof(VtCell));
        cells += (size_t)h->len * sizeof(VtCell);
    }
    for (int i = 0; i < n; i++) hist_release(t, t->hist[(t->hhead + i) % t->hcap]);
    t->hhead = (t->hhead + n) % t->hcap;
    t->hcount -= n;

    if (t->bcount == t->bcap) {   /* grow the block ring */
        int ncap = t->bcap ? t->bcap * 2 : 16;
        Block *nb = malloc((size_t)ncap * sizeof(Block));
        if (!nb) { free(raw); free(j); return; }
        for (int i = 0; i < t->bcount; i++) nb[i] = *blk(t, i);
        free(t->blocks);
        t->blocks = nb; t->bcap = ncap; t->bhead = 0;
    }
    Block *b = &t->blocks[(t->bhead + t->bcount) % t->bcap];
    memset(b, 0, sizeof *b);
    b->id = t->next_block_id++;
    b->nlines = n;
    b->raw_size = (uint32_t)raw_size;
    b->disk_off = -1;
    atomic_init(&j->refs, 2);
    atomic_init(&j->done, 0);
    j->raw = raw; j->raw_size = raw_size; j->out = NULL; j->out_size = 0; j->stored = 0; j->nlines = n;
    b->job = j;
    t->bcount++;
    t->block_lines += n;
    t->raw_total += raw_size;
    t->pending_bytes += raw_size;
    t->pending_blocks++;
    if (t->pending_bytes > PENDING_MAX) {   /* the workers are behind: compress here so the backlog stays bounded */
        job_run(j);                          /* drops the worker's reference; ours is released by block_apply */
        block_apply(t, b);
        return;
    }
    pool_submit(j);
}

/* move the screen row `y` into the history (compact copy, trailing blanks trimmed) */
static void hist_push(Vt *t, int y) {
    if (t->max_lines == 0) return;
    VtCell *src = rowp(t, y);
    int len = rowm(t, y)->hw;
    if (len > t->cols) len = t->cols;
    while (len > 0 && src[len - 1].cp == 0 && src[len - 1].sf == 0) len--;
    HLine *h = malloc(sizeof(HLine) + (size_t)len * sizeof(VtCell));
    if (!h) return;   /* out of memory: skip this line rather than fail */
    h->len = len;
    memcpy(h->cells, src, (size_t)len * sizeof(VtCell));
    VtLineMeta *m = rowm(t, y);
    h->meta = *m;          /* the renderer's cached node travels with the line */
    h->meta.dirty = 0;
    m->cache = NULL;
    m->dirty = 1;
    if (t->hcount == t->hcap) {
        if (t->hcap >= HOT_KEEP + BLOCK_LINES) pack_oldest(t);
        else { hist_release(t, t->hist[t->hhead]); t->hhead = (t->hhead + 1) % t->hcap; t->hcount--; }
    }
    t->hist[(t->hhead + t->hcount) % t->hcap] = h;
    t->hcount++;
    t->hot_bytes += sizeof(HLine) + (size_t)len * sizeof(VtCell);
    enforce_limits(t);
}

void vt_clear_history(Vt *t) {
    for (int i = 0; i < t->hcount; i++) hist_release(t, t->hist[(t->hhead + i) % t->hcap]);
    t->hhead = t->hcount = 0;
    while (t->bcount) evict_oldest_block(t);
    for (int i = 0; i < CACHE_ENTRIES; i++) if (t->cache[i]) cache_drop_id(t, t->cache[i]->id);
    t->spill_next_id = t->next_block_id;
}

/* ---- decoding blocks for viewing ---------------------------------------------------------------------- */

static CacheEntry *decode_entry(Vt *t, Block *b) {
    CacheEntry *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->id = b->id;
    e->nlines = b->nlines;
    const uint8_t *raw = NULL;
    uint8_t *tmp = NULL, *comp = NULL;
    int shuffled = 0;
    if (b->job) raw = b->job->raw;                       /* still compressing: the raw bytes are alive in the job */
    else if (b->data || b->disk_off >= 0) {
        const uint8_t *src = b->data;
        if (!src) {
            comp = malloc(b->comp_size);
            ssize_t got = 0;
            if (comp && t->spill_fd >= 0) got = pread(t->spill_fd, comp, b->comp_size, b->disk_off);
            if (got == (ssize_t)b->comp_size) src = comp;
        }
        if (src) {
            if (b->stored) raw = src;
            else {
                tmp = malloc(b->raw_size);
                if (tmp && sd_lz_decompress(src, b->comp_size, tmp, b->raw_size)) { raw = tmp; shuffled = b->shuffled; }
            }
        }
    }
    int n = b->nlines;
    size_t ncells = 0;
    int ok = raw && b->raw_size >= 2 * (size_t)n;
    int lens[BLOCK_LINES];
    for (int i = 0; ok && i < n; i++) { lens[i] = raw[2 * i] | (raw[2 * i + 1] << 8); ncells += (size_t)lens[i]; }
    if (ok && b->raw_size != 2 * (size_t)n + 8 * ncells) ok = 0;
    if (ok) {
        e->cells = malloc((ncells ? ncells : 1) * sizeof(VtCell));
        ok = e->cells != NULL;
    }
    if (ok) {
        const uint8_t *body = raw + 2 * (size_t)n;
        if (shuffled) {
            for (size_t c = 0; c < ncells; c++) {
                uint8_t *dst = (uint8_t *)&e->cells[c];
                for (int pl = 0; pl < 8; pl++) dst[pl] = body[(size_t)pl * ncells + c];
            }
        } else {
            memcpy(e->cells, body, ncells * sizeof(VtCell));
        }
        int off = 0;
        for (int i = 0; i < n; i++) { e->off[i] = off; off += lens[i]; }
        e->off[n] = off;
    } else {                                              /* damaged or unreadable: show empty lines */
        free(e->cells);
        e->cells = NULL;
        memset(e->off, 0, sizeof e->off);
    }
    for (int i = 0; i < n; i++) e->meta[i].dirty = 1;
    free(tmp); free(comp);
    return e;
}

static CacheEntry *cache_get(Vt *t, Block *b) {
    block_apply(t, b);
    CacheEntry *victim = NULL;
    int vi = 0;
    for (int i = 0; i < CACHE_ENTRIES; i++) {
        CacheEntry *e = t->cache[i];
        if (e && e->id == b->id) { e->stamp = ++t->cache_clock; return e; }
        if (!e) { victim = NULL; vi = i; goto fill; }
        if (!victim || e->stamp < victim->stamp) { victim = e; vi = i; }
    }
    cache_drop_id(t, victim->id);
fill:;
    CacheEntry *e = decode_entry(t, b);
    if (e) { e->stamp = ++t->cache_clock; t->cache[vi] = e; }
    return e;
}

/* idx -1 = newest. Fills whichever outputs are non-NULL. */
static int hist_lookup(Vt *t, int idx, VtCell **cells, int *len, VtLineMeta **meta) {
    long k = -(long)idx - 1;
    if (k < 0 || k >= total_lines(t)) return 0;
    if (k < t->hcount) {
        HLine *h = t->hist[(t->hhead + t->hcount - 1 - k) % t->hcap];
        if (cells) *cells = h->cells;
        if (len) *len = h->len < t->cols ? h->len : t->cols;
        if (meta) *meta = &h->meta;
        return 1;
    }
    long k2 = k - t->hcount;
    int bi = t->bcount - 1 - (int)(k2 / BLOCK_LINES), li = BLOCK_LINES - 1 - (int)(k2 % BLOCK_LINES);
    if (bi < 0) return 0;
    CacheEntry *e = cache_get(t, blk(t, bi));
    if (!e) return 0;
    int l = e->off[li + 1] - e->off[li];
    if (cells) *cells = e->cells ? e->cells + e->off[li] : NULL;
    if (len) *len = l < t->cols ? l : t->cols;
    if (meta) *meta = &e->meta[li];
    return 1;
}

void vt_set_history(Vt *t, int max_lines, size_t ram_budget, size_t disk_budget, bool spill) {
    t->max_lines = max_lines;
    if (ram_budget) t->ram_budget = ram_budget;
    if (disk_budget) t->disk_budget = disk_budget;
    t->spill = spill;
    if (max_lines == 0) { vt_clear_history(t); return; }
    int want = (max_lines > 0 && max_lines <= HOT_KEEP + BLOCK_LINES) ? max_lines : HOT_KEEP + BLOCK_LINES;
    if (want != t->hcap) {
        while (t->hcount > want) {   /* shrink: pack what can be packed, drop the rest */
            if (t->hcap >= HOT_KEEP + BLOCK_LINES && t->hcount >= BLOCK_LINES && max_lines != 0 && want >= HOT_KEEP + BLOCK_LINES) pack_oldest(t);
            else { hist_release(t, t->hist[t->hhead]); t->hhead = (t->hhead + 1) % t->hcap; t->hcount--; }
        }
        HLine **nh = calloc((size_t)want, sizeof(HLine *));
        if (nh) {
            for (int i = 0; i < t->hcount; i++) nh[i] = t->hist[(t->hhead + i) % t->hcap];
            free(t->hist);
            t->hist = nh; t->hcap = want; t->hhead = 0;
        }
    }
    enforce_limits(t);
}

void vt_history_stats(const Vt *t, VtHistoryStats *o) {
    memset(o, 0, sizeof *o);
    o->lines = total_lines(t);
    o->hot_lines = t->hcount;
    o->block_count = t->bcount;
    o->hot_bytes = t->hot_bytes;
    o->packed_bytes = t->packed_bytes + t->pending_bytes;
    o->disk_bytes = t->disk_bytes;
    o->raw_bytes = t->raw_total;
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (t->cache[i]) o->cache_bytes += sizeof(CacheEntry) + (size_t)t->cache[i]->off[t->cache[i]->nlines] * sizeof(VtCell);
}

size_t vt_compact(Vt *t) {
    VtHistoryStats before, after;
    vt_history_stats(t, &before);
    size_t b4 = before.hot_bytes + before.packed_bytes + before.cache_bytes;
    if (t->hcap >= HOT_KEEP + BLOCK_LINES)
        while (t->hcount >= 2 * BLOCK_LINES) pack_oldest(t);   /* keep the newest screenful of lines as they are */
    for (int i = 0; i < CACHE_ENTRIES; i++) if (t->cache[i]) cache_drop_id(t, t->cache[i]->id);
    blocks_poll(t);
    enforce_limits(t);
    vt_history_stats(t, &after);
    size_t af = after.hot_bytes + after.packed_bytes + after.cache_bytes;
    return b4 > af ? b4 - af : 0;
}

/* ---- scrolling ---------------------------------------------------------------------------------- */

/* Rotate the slots of rows [top, bottom] up by n and blank the rows that wrapped around. */
static void region_up(Vt *t, int top, int bottom, int n) {
    int span = bottom - top + 1;
    if (n > span) n = span;
    if (n <= 0) return;
    int *ord = t->cur->order;
    int tmp[n];
    memcpy(tmp, ord + top, (size_t)n * sizeof(int));
    memmove(ord + top, ord + top + n, (size_t)(span - n) * sizeof(int));
    memcpy(ord + bottom - n + 1, tmp, (size_t)n * sizeof(int));
    for (int y = bottom - n + 1; y <= bottom; y++) clear_row(t, y);
}

static void region_down(Vt *t, int top, int bottom, int n) {
    int span = bottom - top + 1;
    if (n > span) n = span;
    if (n <= 0) return;
    int *ord = t->cur->order;
    int tmp[n];
    memcpy(tmp, ord + bottom - n + 1, (size_t)n * sizeof(int));
    memmove(ord + top + n, ord + top, (size_t)(span - n) * sizeof(int));
    memcpy(ord + top, tmp, (size_t)n * sizeof(int));
    for (int y = top; y < top + n; y++) clear_row(t, y);
}

static void scroll_up(Vt *t, int n) {
    if (t->cur == &t->main && t->top == 0 && t->bottom == t->rows - 1) {
        if (n > t->rows) n = t->rows;
        for (int k = 0; k < n; k++) hist_push(t, k);   /* rows 0..n-1 are the ones leaving the top */
    }
    region_up(t, t->top, t->bottom, n);
}

static void scroll_down(Vt *t, int n) { region_down(t, t->top, t->bottom, n); }

/* ---- cursor ------------------------------------------------------------------------------------- */

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void vbounds(Vt *t) {
    int lo = 0, hi = t->rows - 1;
    if ((t->modes & VT_M_ORIGIN) && !(t->top == 0 && t->bottom == t->rows - 1)) { lo = t->top; hi = t->bottom; }
    t->cy = clampi(t->cy, lo, hi);
}

static void hbounds(Vt *t) { t->cx = clampi(t->cx, 0, t->cols - 1); }

static void do_index(Vt *t) {
    if (t->cy == t->bottom) scroll_up(t, 1);
    else t->cy = t->cy + 1 < t->rows ? t->cy + 1 : t->cy;
}

static void do_reverse_index(Vt *t) {
    if (t->cy == t->top) scroll_down(t, 1);
    else if (t->cy > 0) t->cy--;
}

static void do_linefeed(Vt *t) {
    do_index(t);
    if (t->modes & VT_M_NEWLINE) t->cx = 0;
}

static void do_tab(Vt *t) {
    int x = t->cx;
    if (x >= t->cols) x = t->cols - 1;
    for (int i = x + 1; i < t->cols; i++)
        if (t->tabs[i]) { t->cx = i; return; }
    t->cx = t->cols - 1;
}

static void cursor_home(Vt *t) {
    t->cx = 0;
    t->cy = (t->modes & VT_M_ORIGIN) ? t->top : 0;
}

/* ---- drawing ------------------------------------------------------------------------------------ */

static const uint16_t GRAPHICS[32] = {
    0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0, 0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C,
    0x2514, 0x253C, 0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534, 0x252C, 0x2502, 0x2264, 0x2265,
    0x03C0, 0x2260, 0x00A3, 0x00B7,
};


static void insert_cells(Vt *t, int n) {
    VtCell *r = rowp(t, t->cy);
    int x = t->cx >= t->cols ? t->cols - 1 : t->cx;
    if (n > t->cols - x) n = t->cols - x;
    memmove(r + x + n, r + x, (size_t)(t->cols - x - n) * sizeof(VtCell));
    clear_cells(t, r + x, n);
    touch_row(t, t->cy);
}

static void wrap_if_needed(Vt *t, int w) {
    if (t->cx == t->cols) {
        if (t->modes & VT_M_AUTOWRAP) {
            dirty(t, t->cy);
            t->cx = 0;
            do_linefeed(t);
        } else if (w > 0) {
            t->cx -= w;
        }
    }
}

static void put_cp(Vt *t, uint32_t cp) {
    if (t->g[t->gl] == CS_GRAPHICS && cp >= 0x5f && cp <= 0x7e) cp = GRAPHICS[cp - 0x5f];
    int w = vt_wcwidth(cp);
    if (w < 0) return;
    if (w == 0) { /* combining mark: attach to the previous cell */
        int x = (t->cx >= t->cols ? t->cols : t->cx) - 1;
        if (x >= 0) {
            VtCell *r = rowp(t, t->cy);
            if (VT_CELL_COMB(r[x]) == 0) {
                unsigned ci = comb_index(t, cp);
                if (ci) r[x].cp |= ci << 21;
            }
            dirty(t, t->cy);
        }
        return;
    }
    wrap_if_needed(t, w);
    if ((t->modes & VT_M_INSERT)) insert_cells(t, w);
    VtCell *r = rowp(t, t->cy);
    uint32_t sf = t->pen_style << 8;
    if (w == 2) {
        r[t->cx].cp = cp;
        r[t->cx].sf = sf | VT_F_WIDE;
        if (t->cx + 1 < t->cols) { r[t->cx + 1].cp = 0; r[t->cx + 1].sf = sf | VT_F_TAIL; }
    } else {
        r[t->cx].cp = cp;
        r[t->cx].sf = sf;
    }
    VtLineMeta *lm = rowm(t, t->cy);
    lm->dirty = 1;
    if (t->cx + w > lm->hw) lm->hw = (uint16_t)(t->cx + w);
    t->cx = t->cx + w < t->cols ? t->cx + w : t->cols;
    t->last_cp = cp;
}

/* length of the leading run of printable ASCII (0x20..0x7e): eight bytes per step */
static inline size_t ascii_run(const uint8_t *p, size_t n) {
    size_t i = 0;
    const uint64_t ones = 0x0101010101010101ull, highs = 0x8080808080808080ull;
    while (i + 8 <= n) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        uint64_t below = (w - ones * 0x20) & ~w & highs;      /* a byte < 0x20 */
        uint64_t del = ((w ^ (ones * 0x7f)) - ones) & ~(w ^ (ones * 0x7f)) & highs;   /* a byte == 0x7f */
        if ((w & highs) | below | del) break;
        i += 8;
    }
    while (i < n && p[i] >= 0x20 && p[i] < 0x7f) i++;
    return i;
}

/* a run of printable ASCII: the hot path for ordinary output */
static void put_ascii(Vt *t, const uint8_t *p, size_t n) {
    if (t->g[t->gl] == CS_GRAPHICS || (t->modes & VT_M_INSERT) || !(t->modes & VT_M_AUTOWRAP)) {
        for (size_t i = 0; i < n; i++) put_cp(t, p[i]);
        return;
    }
    const uint32_t sf = t->pen_style << 8;
    while (n) {
        if (t->cx == t->cols) {
            dirty(t, t->cy);
            t->cx = 0;
            do_linefeed(t);
        }
        VtCell *r = rowp(t, t->cy);
        size_t room = (size_t)(t->cols - t->cx);
        size_t k = n < room ? n : room;
        VtCell *dst = r + t->cx;
        for (size_t i = 0; i < k; i++) { dst[i].cp = p[i]; dst[i].sf = sf; }
        t->cx += (int)k;
        t->last_cp = p[k - 1];
        p += k;
        n -= k;
        VtLineMeta *lm = rowm(t, t->cy);
        lm->dirty = 1;
        if (t->cx > lm->hw) lm->hw = (uint16_t)t->cx;
    }
}

/* ---- SGR ---------------------------------------------------------------------------------------- */

static void sgr(Vt *t) {
    if (t->np == 0) { t->pen_fg = t->pen_bg = 0; t->pen_attrs = 0; pen_commit(t); return; }
    for (int i = 0; i < t->np; i++) {
        int a = t->par[i];
        switch (a) {
        case 0: t->pen_fg = t->pen_bg = 0; t->pen_attrs = 0; break;
        case 1: t->pen_attrs |= VT_BOLD; break;
        case 2: t->pen_attrs |= VT_DIM; break;
        case 3: t->pen_attrs |= VT_ITALIC; break;
        case 4: t->pen_attrs |= VT_UNDERLINE; break;
        case 5: case 6: t->pen_attrs |= VT_BLINK; break;
        case 7: t->pen_attrs |= VT_REVERSE; break;
        case 8: t->pen_attrs |= VT_INVIS; break;
        case 9: t->pen_attrs |= VT_STRIKE; break;
        case 22: t->pen_attrs &= (uint16_t)~(VT_BOLD | VT_DIM); break;
        case 23: t->pen_attrs &= (uint16_t)~VT_ITALIC; break;
        case 24: t->pen_attrs &= (uint16_t)~VT_UNDERLINE; break;
        case 25: t->pen_attrs &= (uint16_t)~VT_BLINK; break;
        case 27: t->pen_attrs &= (uint16_t)~VT_REVERSE; break;
        case 28: t->pen_attrs &= (uint16_t)~VT_INVIS; break;
        case 29: t->pen_attrs &= (uint16_t)~VT_STRIKE; break;
        case 39: t->pen_fg = 0; break;
        case 49: t->pen_bg = 0; break;
        case 38: case 48: {
            uint32_t *dst = a == 38 ? &t->pen_fg : &t->pen_bg;
            if (i + 1 >= t->np) goto done;
            int kind = t->par[i + 1];
            if (kind == 5 && i + 2 < t->np) {
                *dst = VT_COLOR_IDX(clampi(t->par[i + 2], 0, 255));
                i += 2;
            } else if (kind == 2) {
                /* 38;2;r;g;b  or  38:2:cs:r:g:b  (colour-space id present when 4+ sub-params follow) */
                int j = i + 2, avail = t->np - j;
                int sub_count = 0;
                while (j + sub_count < t->np && t->sub[j + sub_count]) sub_count++;
                if (t->sub[i + 1] && sub_count >= 4) j++;
                if (avail >= 3 || (t->sub[i + 1] && sub_count >= 3)) {
                    *dst = VT_COLOR_RGB(clampi(t->par[j], 0, 255), clampi(t->par[j + 1], 0, 255), clampi(t->par[j + 2], 0, 255));
                    i = j + 2;
                } else {
                    goto done;
                }
            } else {
                goto done;
            }
            break;
        }
        default:
            if (a >= 30 && a <= 37) t->pen_fg = VT_COLOR_IDX(a - 30);
            else if (a >= 40 && a <= 47) t->pen_bg = VT_COLOR_IDX(a - 40);
            else if (a >= 90 && a <= 97) t->pen_fg = VT_COLOR_IDX(a - 90 + 8);
            else if (a >= 100 && a <= 107) t->pen_bg = VT_COLOR_IDX(a - 100 + 8);
            break;
        }
    }
done:
    pen_commit(t);
}

/* ---- modes -------------------------------------------------------------------------------------- */

static void save_cursor(Vt *t, Saved *s) {
    s->x = t->cx; s->y = t->cy; s->fg = t->pen_fg; s->bg = t->pen_bg; s->attrs = t->pen_attrs;
    s->g[0] = t->g[0]; s->g[1] = t->g[1]; s->gl = t->gl;
}

static void restore_cursor(Vt *t, const Saved *s) {
    t->cx = clampi(s->x, 0, t->cols);
    t->cy = clampi(s->y, 0, t->rows - 1);
    t->pen_fg = s->fg; t->pen_bg = s->bg; t->pen_attrs = s->attrs;
    t->g[0] = s->g[0]; t->g[1] = s->g[1]; t->gl = s->gl;
    pen_commit(t);
}

static void switch_screen(Vt *t, bool alt, bool save) {
    if (alt == (t->cur == &t->alt)) return;
    if (alt) {
        if (save) save_cursor(t, &t->saved_1049);
        t->cur = &t->alt;
        memset(t->alt.cells, 0, (size_t)t->cols * (size_t)t->rows * sizeof(VtCell));
        for (int i = 0; i < t->rows; i++) { t->alt.order[i] = i; t->alt.meta[i].dirty = 1; t->alt.meta[i].hw = 0; }
        t->modes |= VT_M_ALT_SCREEN;
    } else {
        t->cur = &t->main;
        t->modes &= ~VT_M_ALT_SCREEN;
        for (int i = 0; i < t->rows; i++) t->main.meta[i].dirty = 1;
        if (save) restore_cursor(t, &t->saved_1049);
    }
}

static void set_mode(Vt *t, int mode, bool on, bool priv) {
    if (!priv) {
        if (mode == 4) t->modes = on ? t->modes | VT_M_INSERT : t->modes & ~VT_M_INSERT;
        else if (mode == 20) t->modes = on ? t->modes | VT_M_NEWLINE : t->modes & ~VT_M_NEWLINE;
        return;
    }
#define FLAG(bit) (t->modes = on ? (t->modes | (bit)) : (t->modes & ~(bit)))
    switch (mode) {
    case 1: FLAG(VT_M_APP_CURSOR); break;
    case 5: FLAG(VT_M_REVERSE_VIDEO); vt_mark_all_dirty(t); break;
    case 6: FLAG(VT_M_ORIGIN); cursor_home(t); break;
    case 7: FLAG(VT_M_AUTOWRAP); break;
    case 25: FLAG(VT_M_CURSOR_VISIBLE); break;
    case 47: case 1047: switch_screen(t, on, false); break;
    case 1048: if (on) save_cursor(t, &t->saved_1049); else restore_cursor(t, &t->saved_1049); break;
    case 1049: switch_screen(t, on, true); break;
    case 1000: t->modes &= ~(VT_M_MOUSE_BTN | VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY); if (on) t->modes |= VT_M_MOUSE_BTN; break;
    case 1002: t->modes &= ~(VT_M_MOUSE_BTN | VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY); if (on) t->modes |= VT_M_MOUSE_DRAG; break;
    case 1003: t->modes &= ~(VT_M_MOUSE_BTN | VT_M_MOUSE_DRAG | VT_M_MOUSE_ANY); if (on) t->modes |= VT_M_MOUSE_ANY; break;
    case 1004: FLAG(VT_M_FOCUS_EVENTS); break;
    case 1006: FLAG(VT_M_MOUSE_SGR); break;
    case 2004: FLAG(VT_M_BRACKETED_PASTE); break;
    default: break;
    }
#undef FLAG
}

/* ---- CSI ---------------------------------------------------------------------------------------- */

static void reply(Vt *t, const char *s) {
    if (t->write_fn) t->write_fn((const uint8_t *)s, strlen(s), t->user);
}

static inline int arg(const Vt *t, int i, int def) { return (i < t->np && t->par[i] > 0) ? t->par[i] : def; }

static void erase_line(Vt *t, int how) {
    VtCell *r = rowp(t, t->cy);
    int x = t->cx >= t->cols ? t->cols - 1 : t->cx;
    if (how == 0) clear_cells(t, r + x, t->cols - x);
    else if (how == 1) clear_cells(t, r, x + 1);
    else clear_cells(t, r, t->cols);
    touch_row(t, t->cy);
}

static void erase_display(Vt *t, int how) {
    if (how == 0) {
        erase_line(t, 0);
        for (int y = t->cy + 1; y < t->rows; y++) clear_row(t, y);
    } else if (how == 1) {
        for (int y = 0; y < t->cy; y++) clear_row(t, y);
        erase_line(t, 1);
    } else {
        for (int y = 0; y < t->rows; y++) clear_row(t, y);
        if (how == 3) vt_clear_history(t);
    }
}

static void csi_dispatch(Vt *t, uint8_t f) {
    int n;
    bool priv = t->priv == '?';
    if (t->inter == ' ' && f == 'q') { t->cursor_style = t->np ? t->par[0] : 0; return; }
    if (t->inter) return;
    if (t->priv && !priv) { /* '>' '=' '<' sequences: only answer the device-attribute queries */
        if (f == 'c' && t->priv == '>') reply(t, "\x1b[>1;10;0c");
        return;
    }
    switch (f) {
    case '@': insert_cells(t, arg(t, 0, 1)); break;
    case 'A': t->cy = t->cy - arg(t, 0, 1) < t->top ? t->top : t->cy - arg(t, 0, 1); break;
    case 'B': case 'e': t->cy = t->cy + arg(t, 0, 1) > t->bottom ? t->bottom : t->cy + arg(t, 0, 1); break;
    case 'C': case 'a': t->cx += arg(t, 0, 1); hbounds(t); break;
    case 'D':
        if (t->cx == t->cols) t->cx--;
        t->cx -= arg(t, 0, 1);
        hbounds(t);
        break;
    case 'E': t->cy = t->cy + arg(t, 0, 1) > t->bottom ? t->bottom : t->cy + arg(t, 0, 1); t->cx = 0; break;
    case 'F': t->cy = t->cy - arg(t, 0, 1) < t->top ? t->top : t->cy - arg(t, 0, 1); t->cx = 0; break;
    case 'G': case '`': t->cx = arg(t, 0, 1) - 1; hbounds(t); break;
    case 'H': case 'f': {
        int row = arg(t, 0, 1) - 1, col = arg(t, 1, 1) - 1;
        if (t->modes & VT_M_ORIGIN) row += t->top;
        t->cy = row; t->cx = col;
        vbounds(t); hbounds(t);
        break;
    }
    case 'I': for (n = arg(t, 0, 1); n > 0; n--) do_tab(t); break;
    case 'J': erase_display(t, t->np ? t->par[0] : 0); break;
    case 'K': erase_line(t, t->np ? t->par[0] : 0); break;
    case 'L': if (t->cy >= t->top && t->cy <= t->bottom) { region_down(t, t->cy, t->bottom, arg(t, 0, 1)); t->cx = 0; } break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bottom) { region_up(t, t->cy, t->bottom, arg(t, 0, 1)); t->cx = 0; } break;
    case 'P': {
        VtCell *r = rowp(t, t->cy);
        int x = t->cx >= t->cols ? t->cols - 1 : t->cx;
        n = arg(t, 0, 1);
        if (n > t->cols - x) n = t->cols - x;
        memmove(r + x, r + x + n, (size_t)(t->cols - x - n) * sizeof(VtCell));
        clear_cells(t, r + t->cols - n, n);
        touch_row(t, t->cy);
        break;
    }
    case 'S': scroll_up(t, arg(t, 0, 1)); break;
    case 'T': scroll_down(t, arg(t, 0, 1)); break;
    case 'X': {
        int x = t->cx >= t->cols ? t->cols - 1 : t->cx;
        n = arg(t, 0, 1);
        if (n > t->cols - x) n = t->cols - x;
        clear_cells(t, rowp(t, t->cy) + x, n);
        touch_row(t, t->cy);
        break;
    }
    case 'Z': {
        for (n = arg(t, 0, 1); n > 0; n--) {
            int x = t->cx >= t->cols ? t->cols - 1 : t->cx, found = 0;
            for (int i = x - 1; i >= 0; i--) if (t->tabs[i]) { t->cx = i; found = 1; break; }
            if (!found) t->cx = 0;
        }
        break;
    }
    case 'b': for (n = arg(t, 0, 1); n > 0 && t->last_cp; n--) put_cp(t, t->last_cp); break;
    case 'c':
        if (!priv && (t->np == 0 || t->par[0] == 0)) reply(t, "\x1b[?62;22c");
        break;
    case 'd': {
        int row = arg(t, 0, 1) - 1;
        if (t->modes & VT_M_ORIGIN) row += t->top;
        t->cy = row; vbounds(t);
        break;
    }
    case 'g':
        if (!t->np || t->par[0] == 0) { if (t->cx < t->cols) t->tabs[t->cx] = 0; }
        else if (t->par[0] == 3) memset(t->tabs, 0, (size_t)t->cols);
        break;
    case 'h': case 'l':
        for (int i = 0; i < (t->np ? t->np : 1); i++) set_mode(t, t->np ? t->par[i] : 0, f == 'h', priv);
        break;
    case 'm': if (!priv) sgr(t); break;
    case 'n': {
        int what = t->np ? t->par[0] : 0;
        if (what == 5) reply(t, "\x1b[0n");
        else if (what == 6) {
            char buf[32];
            int row = t->cy + 1 - ((t->modes & VT_M_ORIGIN) ? t->top : 0);
            snprintf(buf, sizeof buf, "\x1b[%d;%dR", row, (t->cx >= t->cols ? t->cols : t->cx + 1));
            reply(t, buf);
        }
        break;
    }
    case 'r':
        if (priv) break;
        if (t->np == 0) { t->top = 0; t->bottom = t->rows - 1; }
        else {
            int top = clampi(arg(t, 0, 1) - 1, 0, t->rows - 1);
            int bot = clampi(arg(t, 1, t->rows) - 1, 0, t->rows - 1);
            if (bot - top >= 1) { t->top = top; t->bottom = bot; }
            else break;
        }
        cursor_home(t);
        break;
    case 's': if (!priv) save_cursor(t, t->cur == &t->alt ? &t->saved_alt : &t->saved_main); break;
    case 'u': if (!priv) restore_cursor(t, t->cur == &t->alt ? &t->saved_alt : &t->saved_main); break;
    default: break;
    }
}

/* ---- ESC / OSC ---------------------------------------------------------------------------------- */

static void esc_dispatch(Vt *t, uint8_t c) {
    Saved *s = t->cur == &t->alt ? &t->saved_alt : &t->saved_main;
    switch (c) {
    case '7': save_cursor(t, s); break;
    case '8': restore_cursor(t, s); break;
    case 'D': do_index(t); break;
    case 'E': do_index(t); t->cx = 0; break;
    case 'M': do_reverse_index(t); break;
    case 'H': if (t->cx < t->cols) t->tabs[t->cx] = 1; break;
    case 'c': vt_reset(t); break;
    case '=': t->modes |= VT_M_APP_KEYPAD; break;
    case '>': t->modes &= ~VT_M_APP_KEYPAD; break;
    default: break;
    }
}

static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void osc_dispatch(Vt *t) {
    t->osc[t->osc_len] = 0;
    char *semi = strchr(t->osc, ';');
    if (!semi) return;
    int code = atoi(t->osc);
    const char *text = semi + 1;
    if ((code == 0 || code == 2) && t->event_fn) t->event_fn(VT_EV_TITLE, text, t->user);
    else if (code == 7 && t->event_fn) {
        /* file://host/path -> /path, percent-decoded */
        const char *p = strstr(text, "://");
        p = p ? strchr(p + 3, '/') : NULL;
        if (!p) return;
        char out[OSC_MAX + 1];
        int o = 0;
        for (; *p && o < OSC_MAX; p++) {
            if (*p == '%' && hexv(p[1]) >= 0 && hexv(p[2]) >= 0) { out[o++] = (char)(hexv(p[1]) * 16 + hexv(p[2])); p += 2; }
            else out[o++] = *p;
        }
        out[o] = 0;
        t->event_fn(VT_EV_CWD, out, t->user);
    }
}


static void control(Vt *t, uint8_t c) {
    switch (c) {
    case 0x07: if (t->event_fn) t->event_fn(VT_EV_BELL, NULL, t->user); break;
    case 0x08: if (t->cx == t->cols) t->cx--; t->cx -= 1; hbounds(t); break;
    case 0x09: do_tab(t); break;
    case 0x0a: case 0x0b: case 0x0c: do_linefeed(t); break;
    case 0x0d: t->cx = 0; break;
    case 0x0e: t->gl = 1; break;
    case 0x0f: t->gl = 0; break;
    default: break;
    }
}

static inline void csi_start(Vt *t) {
    t->st = ST_CSI;
    t->np = 0;
    t->have_digit = false;
    t->priv = 0;
    t->inter = 0;
}

/* Parse a complete plain CSI sequence (ESC [ params final) in one go. Returns the number of bytes
 * used, or 0 when it is incomplete or unusual (private marker, intermediates): the caller then
 * hands the bytes to the state machine, which also copes with sequences split across reads. */
static inline size_t csi_fast(Vt *t, const uint8_t *p, size_t n) {
    size_t i = 2;
    int np = 0;
    int cur = 0;
    bool any = false, sub = false;
    uint8_t subs[MAX_PARAMS];
    int vals[MAX_PARAMS];
    for (; i < n; i++) {
        uint8_t c = p[i];
        if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); if (cur > 65535) cur = 65535; any = true; }
        else if (c == ';' || c == ':') {
            if (np >= MAX_PARAMS - 1) return 0;
            vals[np] = cur; subs[np] = sub; np++;
            cur = 0; any = true; sub = (c == ':');
        } else if (c >= 0x40 && c <= 0x7e) {
            if (any) { vals[np] = cur; subs[np] = sub; np++; }
            memcpy(t->par, vals, (size_t)np * sizeof(int));
            memcpy(t->sub, subs, (size_t)np);
            t->np = np;
            t->priv = 0;
            t->inter = 0;
            t->have_digit = any;
            csi_dispatch(t, c);
            return i + 1;
        } else {
            return 0;   /* private marker, intermediate, control character: not the common case */
        }
    }
    return 0;
}

void vt_feed(Vt *t, const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n) {
        uint8_t c = p[i];
        switch (t->st) {
        case ST_GROUND:
            if (c == 0x1b && t->utf_need == 0 && i + 2 < n && p[i + 1] == '[') {
                size_t used = csi_fast(t, p + i, n - i);
                if (used) { i += used; continue; }
            }
            if (t->utf_need == 0 && c >= 0x20 && c < 0x7f) {
                size_t run = ascii_run(p + i, n - i);
                put_ascii(t, p + i, run);
                i += run;
                continue;
            }
            i++;
            if (t->utf_need) {
                if ((c & 0xC0) == 0x80) {
                    t->utf_cp = (t->utf_cp << 6) | (c & 0x3f);
                    if (--t->utf_need == 0) put_cp(t, t->utf_cp);
                    continue;
                }
                t->utf_need = 0;
                put_cp(t, 0xFFFD);
                i--; /* reprocess this byte */
                continue;
            }
            if (c < 0x20 || c == 0x7f) {
                if (c == 0x1b) t->st = ST_ESC;
                else control(t, c);
            } else if (c >= 0xC2 && c <= 0xDF) { t->utf_need = 1; t->utf_cp = c & 0x1f; }
            else if (c >= 0xE0 && c <= 0xEF) { t->utf_need = 2; t->utf_cp = c & 0x0f; }
            else if (c >= 0xF0 && c <= 0xF4) { t->utf_need = 3; t->utf_cp = c & 0x07; }
            else put_cp(t, 0xFFFD);
            continue;

        case ST_ESC:
            i++;
            if (c == 0x1b) continue;
            if (c < 0x20) { if (c != 0x18 && c != 0x1a) control(t, c); else t->st = ST_GROUND; continue; }
            switch (c) {
            case '[': csi_start(t); break;
            case ']': t->st = ST_OSC; t->osc_len = 0; break;
            case 'P': case 'X': case '^': case '_': t->st = ST_STR; break;
            case '(': case ')': case '*': case '+': t->st = ST_CHARSET; t->charset_slot = c == '(' ? 0 : c == ')' ? 1 : 2; break;
            case '#': case '%': case ' ': t->st = ST_SKIP1; if (c == '#') t->inter = '#'; else t->inter = 0; break;
            default: esc_dispatch(t, c); t->st = ST_GROUND; break;
            }
            continue;

        case ST_SKIP1: /* ESC # 8 (screen alignment test) and friends: one more byte */
            i++;
            if (t->inter == '#' && c == '8') {
                VtCell e = {'E', 0};
                for (int y = 0; y < t->rows; y++) { VtCell *r = rowp(t, y); for (int x = 0; x < t->cols; x++) r[x] = e; touch_row(t, y); }
            }
            t->inter = 0;
            t->st = ST_GROUND;
            continue;

        case ST_CHARSET:
            i++;
            if (t->charset_slot < 2) t->g[t->charset_slot] = c == '0' ? CS_GRAPHICS : CS_US;
            t->st = ST_GROUND;
            continue;

        case ST_CSI:
            i++;
            if (c == 0x1b) { t->st = ST_ESC; continue; }
            if (c == 0x18 || c == 0x1a) { t->st = ST_GROUND; continue; }
            if (c < 0x20) { control(t, c); continue; }
            if (c >= '0' && c <= '9') {
                if (t->np == 0) { t->np = 1; t->par[0] = 0; t->sub[0] = 0; }
                if (t->np <= MAX_PARAMS) {
                    int *v = &t->par[t->np - 1];
                    *v = *v * 10 + (c - '0');
                    if (*v > 65535) *v = 65535;
                }
                t->have_digit = true;
            } else if (c == ';' || c == ':') {
                if (t->np == 0) { t->np = 1; t->par[0] = 0; t->sub[0] = 0; }
                if (t->np < MAX_PARAMS) { t->np++; t->par[t->np - 1] = 0; t->sub[t->np - 1] = (c == ':'); }
            } else if (c >= 0x3c && c <= 0x3f) {
                if (t->np == 0 && !t->have_digit) t->priv = (char)c;
            } else if (c >= 0x20 && c <= 0x2f) {
                t->inter = (char)c;
            } else if (c >= 0x40 && c <= 0x7e) {
                csi_dispatch(t, c);
                t->st = ST_GROUND;
            }
            continue;

        case ST_OSC:
            i++;
            if (c == 0x07) { osc_dispatch(t); t->st = ST_GROUND; }
            else if (c == 0x1b) t->st = ST_OSC_ESC;
            else if (c == 0x18 || c == 0x1a) t->st = ST_GROUND;
            else if (t->osc_len < OSC_MAX) t->osc[t->osc_len++] = (char)c;
            continue;
        case ST_OSC_ESC:
            i++;
            if (c == '\\') { osc_dispatch(t); t->st = ST_GROUND; }
            else { t->st = ST_ESC; i--; }
            continue;

        case ST_STR:
            i++;
            if (c == 0x1b) t->st = ST_STR_ESC;
            else if (c == 0x07 || c == 0x18 || c == 0x1a) t->st = ST_GROUND;
            continue;
        case ST_STR_ESC:
            i++;
            if (c == '\\') t->st = ST_GROUND;
            else { t->st = ST_ESC; i--; }
            continue;
        }
    }
}

/* ---- lifecycle ---------------------------------------------------------------------------------- */

static void reset_tabs(Vt *t) {
    free(t->tabs);
    t->tabs = calloc((size_t)t->cols + 1, 1);
    for (int i = 8; i < t->cols; i += 8) t->tabs[i] = 1;
}

void vt_reset(Vt *t) {
    t->pen_fg = t->pen_bg = 0;
    t->pen_attrs = 0;
    pen_commit(t);
    t->modes = VT_M_AUTOWRAP | VT_M_CURSOR_VISIBLE;
    t->cur = &t->main;
    t->top = 0;
    t->bottom = t->rows - 1;
    t->g[0] = t->g[1] = CS_US;
    t->gl = 0;
    t->cursor_style = 0;
    t->st = ST_GROUND;
    t->utf_need = 0;
    memset(&t->saved_main, 0, sizeof t->saved_main);
    memset(&t->saved_alt, 0, sizeof t->saved_alt);
    memset(&t->saved_1049, 0, sizeof t->saved_1049);
    for (int y = 0; y < t->rows; y++) clear_row(t, y);
    t->cx = t->cy = 0;
    reset_tabs(t);
}

Vt *vt_new(int cols, int rows, int scrollback) {
    Vt *t = calloc(1, sizeof *t);
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    t->cols = cols; t->rows = rows;
    t->spill_fd = -1;
    t->ram_budget = DEFAULT_RAM_BUDGET;
    t->disk_budget = DEFAULT_DISK_BUDGET;
    t->spill = true;
    t->max_lines = scrollback < 0 ? -1 : scrollback;
    t->hcap = (scrollback < 0 || scrollback > HOT_KEEP + BLOCK_LINES) ? HOT_KEEP + BLOCK_LINES : (scrollback > 0 ? scrollback : 1);
    t->hist = calloc((size_t)t->hcap, sizeof(HLine *));
    style_table_init(t);
    scr_alloc(&t->main, cols, rows);
    scr_alloc(&t->alt, cols, rows);
    t->cur = &t->main;
    vt_reset(t);
    return t;
}

void vt_free(Vt *t) {
    if (!t) return;
    vt_clear_history(t);
    free(t->hist);
    free(t->blocks);
    spill_close(t);
    scr_free(t, &t->main, t->rows);
    scr_free(t, &t->alt, t->rows);
    free(t->tabs); free(t->styles); free(t->style_hash);
    free(t);
}

void vt_set_callbacks(Vt *t, VtWriteFn w, VtEventFn e, VtCacheFree cf, void *user) {
    t->write_fn = w; t->event_fn = e; t->cache_free = cf; t->user = user;
}

void vt_resize(Vt *t, int cols, int rows) {
    if (cols < 2) cols = 2;
    if (rows < 2) rows = 2;
    if (cols == t->cols && rows == t->rows) return;
    bool was_alt = t->cur == &t->alt;
    int old_cols = t->cols, old_rows = t->rows;

    /* main screen: if the cursor would fall off the bottom, the top rows move into scrollback */
    Scr *m = &t->main;
    int shift = (!was_alt && t->cy >= rows) ? t->cy - rows + 1 : 0;
    Scr *saved_cur = t->cur;
    t->cur = m;
    for (int k = 0; k < shift; k++) hist_push(t, k);
    t->cur = saved_cur;

    Scr nm, na;
    scr_alloc(&nm, cols, rows);
    scr_alloc(&na, cols, rows);
    int w = cols < old_cols ? cols : old_cols;
    for (int y = 0; y < rows; y++) {
        int sy = y + shift;
        if (sy < old_rows) {
            int slot = m->order[sy];
            memcpy(nm.cells + (size_t)y * (size_t)cols, m->cells + (size_t)slot * (size_t)old_cols, (size_t)w * sizeof(VtCell));
            nm.meta[y].hw = (uint16_t)w;
            VtCell *last = nm.cells + (size_t)y * (size_t)cols + w - 1;
            if (w < old_cols && (last->sf & VT_F_WIDE)) { last->cp = 0; last->sf &= ~(uint32_t)VT_F_WIDE; }
        }
        if (y < old_rows) {
            int aslot = t->alt.order[y];
            memcpy(na.cells + (size_t)y * (size_t)cols, t->alt.cells + (size_t)aslot * (size_t)old_cols, (size_t)w * sizeof(VtCell));
            na.meta[y].hw = (uint16_t)w;
        }
    }
    scr_free(t, &t->main, old_rows);
    scr_free(t, &t->alt, old_rows);
    t->main = nm;
    t->alt = na;
    t->cur = was_alt ? &t->alt : &t->main;
    t->cols = cols;
    t->rows = rows;
    t->cy = clampi(t->cy - shift, 0, rows - 1);
    t->cx = clampi(t->cx, 0, cols - 1);
    t->top = 0;
    t->bottom = rows - 1;
    reset_tabs(t);
    vt_mark_all_dirty(t);
}

void vt_mark_all_dirty(Vt *t) {
    for (int i = 0; i < t->rows; i++) { t->main.meta[i].dirty = 1; t->alt.meta[i].dirty = 1; }
    for (int i = 0; i < t->hcount; i++) t->hist[(t->hhead + i) % t->hcap]->meta.dirty = 1;
    for (int i = 0; i < CACHE_ENTRIES; i++)
        if (t->cache[i]) for (int k = 0; k < t->cache[i]->nlines; k++) t->cache[i]->meta[k].dirty = 1;
}

int vt_cols(const Vt *t) { return t->cols; }
int vt_rows(const Vt *t) { return t->rows; }
uint32_t vt_modes(const Vt *t) { return t->modes; }
int vt_cursor_x(const Vt *t) { return t->cx; }
int vt_cursor_y(const Vt *t) { return t->cy; }
int vt_cursor_style(const Vt *t) { return t->cursor_style; }
int vt_history_count(const Vt *t) { long n = total_lines(t); return t->cur == &t->alt ? 0 : (n > INT_MAX ? INT_MAX : (int)n); }

VtCell *vt_line(Vt *t, int idx, int *len) {
    if (idx >= 0) {
        if (idx >= t->rows) return NULL;
        if (len) *len = t->cols;
        return rowp(t, idx);
    }
    if (t->cur == &t->alt) return NULL;
    VtCell *cells = NULL;
    int l = 0;
    if (!hist_lookup(t, idx, &cells, &l, NULL)) return NULL;
    if (len) *len = l;
    static VtCell empty[1];
    return cells ? cells : empty;
}

VtLineMeta *vt_line_meta(Vt *t, int idx) {
    if (idx >= 0) return idx < t->rows ? rowm(t, idx) : NULL;
    if (t->cur == &t->alt) return NULL;
    VtLineMeta *m = NULL;
    return hist_lookup(t, idx, NULL, NULL, &m) ? m : NULL;
}

size_t vt_memory_used(const Vt *t) {
    size_t n = sizeof *t + (size_t)t->cstyles * sizeof(VtStyle) + (size_t)(t->hash_mask + 1) * sizeof(uint32_t);
    n += 2 * ((size_t)t->cols * (size_t)t->rows * sizeof(VtCell) + (size_t)t->rows * (sizeof(VtLineMeta) + sizeof(int)));
    VtHistoryStats h;
    vt_history_stats(t, &h);
    return n + (size_t)t->hcap * sizeof(HLine *) + h.hot_bytes + h.packed_bytes + h.cache_bytes + (size_t)t->bcap * sizeof(Block);
}
