#include "ios6/hooks.h"
#include "ios6/context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HEAP_BASE 0x60000000u
#define HEAP_SIZE 0x20000000u
#define FAKE_ZONE 0x21009800u

typedef void (*hook_fn)(guest_arena_t *arena, uint32_t *gtc);

typedef struct {
    uint32_t addr;
    hook_fn fn;
    const char *name;
} hook_entry_t;

#define MAX_HOOKS 256
static hook_entry_t s_hooks[MAX_HOOKS];
static size_t s_hook_count;
static uint32_t s_heap_next = HEAP_BASE;
static bool s_trace;

#define HP(a) ((void *)guest_arena_to_host(arena, (a)))
#define R(n) gtc[n]

/* ---- allocator: bump pointer with a 16-byte header holding the size; free() is a no-op ---- */
static uint32_t heap_alloc(guest_arena_t *arena, uint32_t size, uint32_t align) {
    if (align < 16) align = 16;
    uint32_t start = (s_heap_next + 16 + align - 1) & ~(align - 1);
    uint32_t end = start + ((size + 15) & ~15u);
    if (end < start || end > HEAP_BASE + HEAP_SIZE) return 0;
    uint32_t *hdr = HP(start - 16);
    hdr[0] = size;
    s_heap_next = end;
    return start;
}
static uint32_t heap_size(guest_arena_t *arena, uint32_t p) {
    if (p < HEAP_BASE + 16 || p >= s_heap_next) return 0;
    return *(uint32_t *)HP(p - 16);
}

static void h_malloc(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_alloc(arena, R(0), 16); }
static void h_zone_malloc(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_alloc(arena, R(1), 16); }
static void h_calloc(guest_arena_t *arena, uint32_t *gtc) {
    uint64_t total = (uint64_t)R(0) * R(1);
    uint32_t p = total > 0x10000000 ? 0 : heap_alloc(arena, (uint32_t)total, 16);
    if (p) memset(HP(p), 0, (uint32_t)total);
    R(0) = p;
}
static void h_zone_calloc(guest_arena_t *arena, uint32_t *gtc) {
    R(0) = R(1); R(1) = R(2); h_calloc(arena, gtc);
}
static uint32_t do_realloc(guest_arena_t *arena, uint32_t old, uint32_t size) {
    if (!old) return heap_alloc(arena, size, 16);
    uint32_t osz = heap_size(arena, old);
    uint32_t p = heap_alloc(arena, size, 16);
    if (p) memcpy(HP(p), HP(old), osz < size ? osz : size);
    return p;
}
static void h_realloc(guest_arena_t *arena, uint32_t *gtc) { R(0) = do_realloc(arena, R(0), R(1)); }
static void h_zone_realloc(guest_arena_t *arena, uint32_t *gtc) { R(0) = do_realloc(arena, R(1), R(2)); }
static void h_free(guest_arena_t *arena, uint32_t *gtc) { (void)arena; R(0) = 0; }
static void h_valloc(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_alloc(arena, R(0), 4096); }
static void h_zone_valloc(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_alloc(arena, R(1), 4096); }
static void h_zone_memalign(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_alloc(arena, R(2), R(1)); }
static void h_posix_memalign(guest_arena_t *arena, uint32_t *gtc) {
    uint32_t p = heap_alloc(arena, R(2), R(1));
    uint32_t *out = HP(R(0));
    if (p && out) { *out = p; R(0) = 0; } else R(0) = 12; /* ENOMEM */
}
static void h_malloc_size(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_size(arena, R(0)); }
static void h_zone_size(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_size(arena, R(1)); }
static void h_good_size(guest_arena_t *arena, uint32_t *gtc) { (void)arena; R(0) = (R(0) + 15) & ~15u; }
static void h_default_zone(guest_arena_t *arena, uint32_t *gtc) { (void)arena; R(0) = FAKE_ZONE; }
static void h_zone_from_ptr(guest_arena_t *arena, uint32_t *gtc) { R(0) = heap_size(arena, R(0)) ? FAKE_ZONE : 0; }
static void h_zone_free(guest_arena_t *arena, uint32_t *gtc) { (void)arena; R(0) = 0; }

/* ---- string / memory ---- */
static void h_memcpy(guest_arena_t *arena, uint32_t *gtc) { memmove(HP(R(0)), HP(R(1)), R(2)); }
static void h_bcopy(guest_arena_t *arena, uint32_t *gtc) { memmove(HP(R(1)), HP(R(0)), R(2)); R(0) = R(1); }
static void h_memset(guest_arena_t *arena, uint32_t *gtc) { memset(HP(R(0)), (int)R(1), R(2)); }
static void h_bzero(guest_arena_t *arena, uint32_t *gtc) { memset(HP(R(0)), 0, R(1)); }
static void h_memcmp(guest_arena_t *arena, uint32_t *gtc) { R(0) = (uint32_t)memcmp(HP(R(0)), HP(R(1)), R(2)); }
static void h_memchr(guest_arena_t *arena, uint32_t *gtc) {
    void *p = memchr(HP(R(0)), (int)R(1), R(2));
    R(0) = p ? R(0) + (uint32_t)((char *)p - (char *)HP(R(0))) : 0;
}
static void h_strlen(guest_arena_t *arena, uint32_t *gtc) { R(0) = (uint32_t)strlen(HP(R(0))); }
static void h_strcmp(guest_arena_t *arena, uint32_t *gtc) { R(0) = (uint32_t)strcmp(HP(R(0)), HP(R(1))); }
static void h_strncmp(guest_arena_t *arena, uint32_t *gtc) { R(0) = (uint32_t)strncmp(HP(R(0)), HP(R(1)), R(2)); }
static void h_strcpy(guest_arena_t *arena, uint32_t *gtc) { strcpy(HP(R(0)), HP(R(1))); }
static void h_strncpy(guest_arena_t *arena, uint32_t *gtc) { strncpy(HP(R(0)), HP(R(1)), R(2)); }
static void h_strlcpy(guest_arena_t *arena, uint32_t *gtc) {
    const char *src = HP(R(1));
    size_t sl = strlen(src), n = R(2);
    if (n) { size_t c = sl >= n ? n - 1 : sl; memcpy(HP(R(0)), src, c); ((char *)HP(R(0)))[c] = 0; }
    R(0) = (uint32_t)sl;
}
static void h_strlcat(guest_arena_t *arena, uint32_t *gtc) {
    char *dst = HP(R(0)); const char *src = HP(R(1));
    size_t n = R(2), dl = strnlen(dst, n), sl = strlen(src);
    if (dl == n) { R(0) = (uint32_t)(n + sl); return; }
    size_t c = sl >= n - dl ? n - dl - 1 : sl;
    memcpy(dst + dl, src, c); dst[dl + c] = 0;
    R(0) = (uint32_t)(dl + sl);
}
static void h_strchr(guest_arena_t *arena, uint32_t *gtc) {
    char *s = HP(R(0)); char *p = strchr(s, (int)R(1));
    R(0) = p ? R(0) + (uint32_t)(p - s) : 0;
}
static void h_strrchr(guest_arena_t *arena, uint32_t *gtc) {
    char *s = HP(R(0)); char *p = strrchr(s, (int)R(1));
    R(0) = p ? R(0) + (uint32_t)(p - s) : 0;
}
static void h_strdup(guest_arena_t *arena, uint32_t *gtc) {
    size_t n = strlen(HP(R(0))) + 1;
    uint32_t p = heap_alloc(arena, (uint32_t)n, 16);
    if (p) memcpy(HP(p), HP(R(0)), n);
    R(0) = p;
}

static const struct { const char *name; hook_fn fn; } s_table[] = {
    {"_malloc", h_malloc}, {"_calloc", h_calloc}, {"_realloc", h_realloc}, {"_free", h_free},
    {"_valloc", h_valloc}, {"_posix_memalign", h_posix_memalign},
    {"_malloc_size", h_malloc_size}, {"_malloc_good_size", h_good_size},
    {"_malloc_default_zone", h_default_zone}, {"_malloc_zone_from_ptr", h_zone_from_ptr},
    {"_malloc_zone_malloc", h_zone_malloc}, {"_malloc_zone_calloc", h_zone_calloc},
    {"_malloc_zone_realloc", h_zone_realloc}, {"_malloc_zone_free", h_zone_free},
    {"_malloc_zone_valloc", h_zone_valloc}, {"_malloc_zone_memalign", h_zone_memalign},
    {"_malloc_zone_size", h_zone_size},
    {"_memcpy", h_memcpy}, {"_memmove", h_memcpy}, {"_bcopy", h_bcopy}, {"_memset", h_memset},
    {"_bzero", h_bzero}, {"_memcmp", h_memcmp}, {"_memchr", h_memchr}, {"_bcmp", h_memcmp},
    {"_strlen", h_strlen}, {"_strcmp", h_strcmp}, {"_strncmp", h_strncmp}, {"_strcpy", h_strcpy},
    {"_strncpy", h_strncpy}, {"_strlcpy", h_strlcpy}, {"_strlcat", h_strlcat},
    {"_strchr", h_strchr}, {"_strrchr", h_strrchr}, {"_strdup", h_strdup},
};

void hooks_init(guest_arena_t *arena) {
    guest_arena_mprotect(arena, HEAP_BASE, HEAP_SIZE, PROT_READ | PROT_WRITE);
    s_trace = getenv("IOS6_TRACE_HOOKS") != NULL;
}

int hooks_install_from_cache(shared_cache_t *cache) {
    for (size_t i = 0; i < sizeof(s_table) / sizeof(s_table[0]) && s_hook_count < MAX_HOOKS; i++) {
        uint32_t a = shared_cache_resolve(cache, "/usr/lib/libSystem.B.dylib", s_table[i].name);
        if (!a) continue;
        s_hooks[s_hook_count].addr = a & ~1u;
        s_hooks[s_hook_count].fn = s_table[i].fn;
        s_hooks[s_hook_count].name = s_table[i].name;
        s_hook_count++;
    }
    return (int)s_hook_count;
}

int hooks_dispatch(guest_arena_t *arena, uint32_t *gtc, uint32_t pc) {
    for (size_t i = 0; i < s_hook_count; i++) {
        if (s_hooks[i].addr != pc) continue;
        if (s_trace) fprintf(stderr, "[hook] %s(0x%x, 0x%x, 0x%x)", s_hooks[i].name, gtc[0], gtc[1], gtc[2]);
        if (s_trace && !strncmp(s_hooks[i].name, "_str", 4)) {
            const char *a = HP(gtc[0]), *b = HP(gtc[1]);
            fprintf(stderr, " a=\"%.24s\"", a ? a : "");
            if (!strcmp(s_hooks[i].name, "_strncmp") || !strcmp(s_hooks[i].name, "_strcmp")) fprintf(stderr, " b=\"%.24s\"", b ? b : "");
        }
        s_hooks[i].fn(arena, gtc);
        if (s_trace) fprintf(stderr, " = 0x%x\n", gtc[0]);
        gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_LR];
        return 1;
    }
    return 0;
}
