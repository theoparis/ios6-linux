#include "ios6/dyld.h"
#include "ios6/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define DYLD_MAX_LOADED     32
#define DYLD_MAX_LISTED     MAX_CACHE_IMAGES
#define DYLD_MAX_FRAMES     16

/* Guest copy of the image list: dyld_image_info[] {mh, path, modDate}, then the path strings. */
#define DYLD_INFO_BASE      0x21040000u
#define DYLD_INFO_SIZE      0x40000u
#define DYLD_INFO_STRIDE    12u
#define DYLD_INFO_STRINGS   (DYLD_INFO_BASE + DYLD_INFO_STRIDE * DYLD_MAX_LISTED)

#define DYLD_STATE_BOUND    40u /* dyld_image_state_bound: the state every listed image is in */

#define LC_SEGMENT_CMD          0x1u
#define LC_LOAD_DYLIB_CMD       0xcu
#define LC_LOAD_WEAK_DYLIB_CMD  0x80000018u
#define LC_REEXPORT_DYLIB_CMD   0x8000001fu
#define LC_LOAD_UPWARD_DYLIB_CMD 0x80000023u

typedef struct {
    uint32_t header;
    uint32_t slide;
    const char *path;
    const loaded_image_t *loaded; /* NULL for shared cache images */
} listed_image_t;

static symbol_resolver_fn s_resolver;
static void *s_resolver_user_data;
static guest_arena_t *s_arena;
static const loaded_image_t *s_loaded[DYLD_MAX_LOADED];
static size_t s_loaded_count;
static listed_image_t s_list[DYLD_MAX_LISTED];
static size_t s_list_count;
static uint32_t s_strings_used;

typedef enum { FRAME_ADD_IMAGE, FRAME_STATE_SINGLE, FRAME_STATE_BATCH } frame_kind_t;

/* One in-flight registration: the callback being run, its caller's LR, and progress. */
typedef struct {
    frame_kind_t kind;
    uint32_t func;
    uint32_t state;
    uint32_t saved_lr;
    size_t next_image;
} frame_t;
static frame_t s_frames[DYLD_MAX_FRAMES];
static size_t s_depth;

void dyld_init(symbol_resolver_fn resolver, void *user_data) {
    s_resolver = resolver;
    s_resolver_user_data = user_data;
}

void dyld_register_image(const loaded_image_t *image) {
    if (s_loaded_count == DYLD_MAX_LOADED) {
        fprintf(stderr, "dyld: more than %d loaded images\n", DYLD_MAX_LOADED);
        exit(1);
    }
    s_loaded[s_loaded_count++] = image;
}

/* ---- image list -------------------------------------------------------------------- */

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static const uint8_t *guest_ptr(uint32_t addr) {
    return (const uint8_t *)guest_arena_to_host(s_arena, addr);
}

/* Calls fn(cmd, command bytes) for each load command of the image whose header is at `header`. */
static void for_each_command(uint32_t header, void (*fn)(uint32_t, const uint8_t *, void *), void *ctx) {
    const uint8_t *mh = guest_ptr(header);
    uint32_t ncmds;
    memcpy(&ncmds, mh + 16, 4);
    const uint8_t *p = mh + 28; /* sizeof(mach_header) */
    for (uint32_t i = 0; i < ncmds; i++) {
        uint32_t cmd, size;
        memcpy(&cmd, p, 4);
        memcpy(&size, p + 4, 4);
        fn(cmd, p, ctx);
        p += size;
    }
}

static size_t list_find_path(const char *path) {
    for (size_t i = 0; i < s_list_count; i++)
        if (strcmp(s_list[i].path, path) == 0) return i;
    return (size_t)-1;
}

static size_t list_add(const char *path, uint32_t header, uint32_t slide, const loaded_image_t *loaded) {
    if (s_list_count == DYLD_MAX_LISTED) {
        fprintf(stderr, "dyld: more than %d images\n", DYLD_MAX_LISTED);
        exit(1);
    }
    s_list[s_list_count] = (listed_image_t){ header, slide, path, loaded };
    return s_list_count++;
}

typedef struct {
    shared_cache_t *cache;
    const char *names[64];
    size_t count;
} deps_ctx_t;

static void collect_dependency(uint32_t cmd, const uint8_t *p, void *vctx) {
    if (cmd != LC_LOAD_DYLIB_CMD && cmd != LC_LOAD_WEAK_DYLIB_CMD &&
        cmd != LC_REEXPORT_DYLIB_CMD && cmd != LC_LOAD_UPWARD_DYLIB_CMD) return;
    deps_ctx_t *d = vctx;
    uint32_t name_off;
    memcpy(&name_off, p + 8, 4);
    if (d->count == 64) {
        fprintf(stderr, "dyld: image has more than 64 dependencies\n");
        exit(1);
    }
    d->names[d->count++] = (const char *)p + name_off;
}

/* Resolves `name` to a new list entry: a shared cache image, else a registered loaded image
 * with the same file name. */
static size_t list_add_dependency(shared_cache_t *cache, const char *name) {
    if (cache) {
        for (size_t i = 0; i < cache->image_count; i++) {
            if (strcmp(cache->images[i].path, name) == 0)
                return list_add(cache->images[i].path, (uint32_t)cache->images[i].address, 0, NULL);
        }
    }
    for (size_t i = 0; i < s_loaded_count; i++) {
        if (strcmp(base_name(s_loaded[i]->path), base_name(name)) == 0)
            return list_add(s_loaded[i]->path, s_loaded[i]->base_addr, s_loaded[i]->slide, s_loaded[i]);
    }
    fprintf(stderr, "dyld: Library not loaded: %s\n", name);
    exit(1);
}

/* dyld's recursiveLoadLibraries: append all of an image's new dependents, then recurse. */
static void load_dependents(shared_cache_t *cache, size_t idx) {
    deps_ctx_t d = { .cache = cache };
    for_each_command(s_list[idx].header, collect_dependency, &d);
    size_t added[64];
    size_t nadded = 0;
    for (size_t i = 0; i < d.count; i++) {
        // Names live in guest memory, which stays mapped; match by name before adding.
        size_t found = list_find_path(d.names[i]);
        if (found == (size_t)-1) added[nadded++] = list_add_dependency(cache, d.names[i]);
    }
    for (size_t i = 0; i < nadded; i++) load_dependents(cache, added[i]);
}

void dyld_build_image_list(guest_arena_t *arena, shared_cache_t *cache, const loaded_image_t *exe) {
    s_arena = arena;
    s_list_count = 0;
    size_t root = list_add(exe->path, exe->base_addr, exe->slide, exe);
    load_dependents(cache, root);

    guest_arena_mprotect(arena, DYLD_INFO_BASE, DYLD_INFO_SIZE, PROT_READ | PROT_WRITE);
    uint32_t *infos = (uint32_t *)guest_arena_to_host(arena, DYLD_INFO_BASE);
    s_strings_used = 0;
    for (size_t i = 0; i < s_list_count; i++) {
        size_t len = strlen(s_list[i].path) + 1;
        if (DYLD_INFO_STRINGS - DYLD_INFO_BASE + s_strings_used + len > DYLD_INFO_SIZE) {
            fprintf(stderr, "dyld: image path table overflow\n");
            exit(1);
        }
        uint32_t str = DYLD_INFO_STRINGS + s_strings_used;
        memcpy(guest_arena_to_host(arena, str), s_list[i].path, len);
        s_strings_used += (uint32_t)len;
        infos[i * 3 + 0] = s_list[i].header;
        infos[i * 3 + 1] = str;
        infos[i * 3 + 2] = 0; /* imageFileModDate */
    }
    if (getenv("IOS6_TRACE"))
        fprintf(stderr, "[dyld] image list: %zu images\n", s_list_count);
}

uint32_t dyld_image_count(void) { return (uint32_t)s_list_count; }
uint32_t dyld_image_header(uint32_t i) { return i < s_list_count ? s_list[i].header : 0; }
uint32_t dyld_image_slide(uint32_t i) { return i < s_list_count ? s_list[i].slide : 0; }

uint32_t dyld_image_name(uint32_t i) {
    if (i >= s_list_count) return 0;
    uint32_t str;
    memcpy(&str, guest_ptr(DYLD_INFO_BASE + i * DYLD_INFO_STRIDE + 4), 4);
    return str;
}

uint32_t dyld_image_slide_for_header(uint32_t header) {
    for (size_t i = 0; i < s_list_count; i++)
        if (s_list[i].header == header) return s_list[i].slide;
    return 0;
}

uint32_t dyld_dlopen(const char *path, uint32_t mode) {
    if (!path) return s_list_count ? s_list[0].header : 0; // dlopen(NULL): the main program
    size_t i = list_find_path(path);
    if (i != (size_t)-1) return s_list[i].header;
    if (mode & 0x10 /* RTLD_NOLOAD */) return 0;
    fprintf(stderr, "dyld: dlopen(%s): loading new images is not supported\n", path);
    exit(1);
}

typedef struct { uint32_t slide, addr; int hit; } contain_ctx_t;

static void test_segment(uint32_t cmd, const uint8_t *p, void *vctx) {
    if (cmd != LC_SEGMENT_CMD) return;
    contain_ctx_t *c = vctx;
    uint32_t vmaddr, vmsize;
    memcpy(&vmaddr, p + 24, 4);
    memcpy(&vmsize, p + 28, 4);
    if (vmsize && c->addr - (vmaddr + c->slide) < vmsize && c->addr >= vmaddr + c->slide) c->hit = 1;
}

uint32_t dyld_image_path_containing(uint32_t addr) {
    for (size_t i = 0; i < s_list_count; i++) {
        contain_ctx_t c = { s_list[i].slide, addr, 0 };
        for_each_command(s_list[i].header, test_segment, &c);
        if (c.hit) return dyld_image_name((uint32_t)i);
    }
    return 0;
}

/* ---- lazy binding ------------------------------------------------------------------ */

/* The registered image whose mapped segments contain `addr`, or NULL. */
static const loaded_image_t *image_containing(uint32_t addr) {
    for (size_t i = 0; i < s_loaded_count; i++) {
        const loaded_image_t *img = s_loaded[i];
        for (size_t j = 0; j < img->segment_count; j++) {
            const segment_command_32_t *seg = &img->segments[j];
            if (seg->vmsize == 0 || strcmp(seg->segname, "__PAGEZERO") == 0) continue;
            uint32_t start = seg->vmaddr + img->slide;
            if (addr >= start && addr - start < seg->vmsize) return img;
        }
    }
    return NULL;
}

uint32_t dyld_fast_stub_entry(guest_arena_t *arena, uint32_t cell, uint32_t lazy_offset) {
    const loaded_image_t *img = image_containing(cell);
    if (!img) {
        fprintf(stderr, "dyld: lazy bind requested from 0x%08x, outside every loaded image\n", cell);
        exit(1);
    }
    const char *name = "";
    uint32_t target = macho_lazy_bind(img, arena, s_resolver, s_resolver_user_data,
                                      lazy_offset, &name);
    if (target == 0) {
        fprintf(stderr, "dyld: Symbol not found: %s (lazy bind offset 0x%x)\n", name, lazy_offset);
        exit(1);
    }
    if (getenv("IOS6_TRACE")) fprintf(stderr, "[dyld] lazy bind %s -> 0x%08x\n", name, target);
    return target;
}

/* ---- guest callbacks --------------------------------------------------------------- */

static void call_guest(uint32_t *gtc, uint32_t func) {
    gtc[GTC_LR] = runtime_resolve_symbol("__ios6_dyld_callback_continue", NULL);
    gtc[GTC_FIELD_NEXT_PC] = func;
}

/* Starts the frame's next callback, or pops the frame and returns to the registrant. */
static void frame_step(uint32_t *gtc) {
    frame_t *f = &s_frames[s_depth - 1];
    switch (f->kind) {
    case FRAME_ADD_IMAGE:
        if (f->next_image < s_list_count) {
            const listed_image_t *img = &s_list[f->next_image++];
            gtc[GTC_R0] = img->header;
            gtc[GTC_R1] = img->slide;
            call_guest(gtc, f->func);
            return;
        }
        break;
    case FRAME_STATE_SINGLE:
        if (f->state <= DYLD_STATE_BOUND && f->next_image < s_list_count) {
            gtc[GTC_R0] = f->state;
            gtc[GTC_R1] = DYLD_INFO_BASE + (uint32_t)f->next_image++ * DYLD_INFO_STRIDE;
            call_guest(gtc, f->func);
            return;
        }
        break;
    case FRAME_STATE_BATCH:
        if (f->state <= DYLD_STATE_BOUND && f->next_image == 0 && s_list_count) {
            f->next_image = s_list_count;
            gtc[GTC_R0] = f->state;
            gtc[GTC_R1] = (uint32_t)s_list_count;
            gtc[GTC_R2] = DYLD_INFO_BASE;
            call_guest(gtc, f->func);
            return;
        }
        break;
    }
    gtc[GTC_LR] = f->saved_lr;
    gtc[GTC_FIELD_NEXT_PC] = f->saved_lr;
    s_depth--;
}

static void push_frame(uint32_t *gtc, frame_kind_t kind, uint32_t func, uint32_t state) {
    if (func == 0) { // a NULL callback is ignored
        gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_LR];
        return;
    }
    if (s_depth == DYLD_MAX_FRAMES) {
        fprintf(stderr, "dyld: callback registrations nested deeper than %d\n", DYLD_MAX_FRAMES);
        exit(1);
    }
    s_frames[s_depth++] = (frame_t){ .kind = kind, .func = func, .state = state,
                                     .saved_lr = gtc[GTC_LR], .next_image = 0 };
    frame_step(gtc);
}

void dyld_register_add_image(uint32_t *gtc, uint32_t func) {
    push_frame(gtc, FRAME_ADD_IMAGE, func, 0);
}

void dyld_register_state_handler(uint32_t *gtc, uint32_t state, uint32_t batch, uint32_t handler) {
    push_frame(gtc, batch == 1 ? FRAME_STATE_BATCH : FRAME_STATE_SINGLE, handler, state);
}

void dyld_callback_continue(uint32_t *gtc) {
    if (s_depth == 0) {
        fprintf(stderr, "dyld: callback continuation without a pending registration\n");
        exit(1);
    }
    const frame_t *f = &s_frames[s_depth - 1];
    if (f->kind != FRAME_ADD_IMAGE && gtc[GTC_R0] != 0) {
        const char *msg = (const char *)guest_arena_to_host(s_arena, gtc[GTC_R0]);
        fprintf(stderr, "dyld: image state change handler 0x%08x failed: %s\n",
                f->func, msg ? msg : "(bad pointer)");
        exit(1);
    }
    frame_step(gtc);
}
