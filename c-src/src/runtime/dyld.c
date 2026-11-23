#include "ios6/dyld.h"
#include "ios6/runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DYLD_MAX_IMAGES     32
#define DYLD_MAX_ADD_FRAMES 16

static symbol_resolver_fn s_resolver;
static void *s_resolver_user_data;
static const loaded_image_t *s_images[DYLD_MAX_IMAGES];
static size_t s_image_count;

/* One in-flight registration: the callback being run, its caller's LR, and the next image. */
typedef struct {
    uint32_t func;
    uint32_t saved_lr;
    size_t next_image;
} add_frame_t;
static add_frame_t s_add_frames[DYLD_MAX_ADD_FRAMES];
static size_t s_add_depth;

void dyld_init(symbol_resolver_fn resolver, void *user_data) {
    s_resolver = resolver;
    s_resolver_user_data = user_data;
}

void dyld_register_image(const loaded_image_t *image) {
    if (s_image_count == DYLD_MAX_IMAGES) {
        fprintf(stderr, "dyld: more than %d images\n", DYLD_MAX_IMAGES);
        exit(1);
    }
    s_images[s_image_count++] = image;
}

/* The registered image whose mapped segments contain `addr`, or NULL. */
static const loaded_image_t *image_containing(uint32_t addr) {
    for (size_t i = 0; i < s_image_count; i++) {
        const loaded_image_t *img = s_images[i];
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

/* Points NEXT_PC at the callback for the next image, or pops the frame and returns to the caller. */
static void add_image_step(uint32_t *gtc) {
    add_frame_t *f = &s_add_frames[s_add_depth - 1];
    if (f->next_image < s_image_count) {
        const loaded_image_t *img = s_images[f->next_image++];
        gtc[GTC_R0] = img->base_addr;
        gtc[GTC_R1] = img->slide;
        gtc[GTC_LR] = runtime_resolve_symbol("__ios6_dyld_add_image_continue", NULL);
        gtc[GTC_FIELD_NEXT_PC] = f->func;
        return;
    }
    gtc[GTC_LR] = f->saved_lr;
    gtc[GTC_FIELD_NEXT_PC] = f->saved_lr;
    s_add_depth--;
}

void dyld_register_add_image(uint32_t *gtc, uint32_t func) {
    if (func == 0) { // a NULL callback is ignored
        gtc[GTC_FIELD_NEXT_PC] = gtc[GTC_LR];
        return;
    }
    if (s_add_depth == DYLD_MAX_ADD_FRAMES) {
        fprintf(stderr, "dyld: add-image registrations nested deeper than %d\n", DYLD_MAX_ADD_FRAMES);
        exit(1);
    }
    s_add_frames[s_add_depth++] = (add_frame_t){ .func = func, .saved_lr = gtc[GTC_LR], .next_image = 0 };
    add_image_step(gtc);
}

void dyld_add_image_continue(uint32_t *gtc) {
    if (s_add_depth == 0) {
        fprintf(stderr, "dyld: add-image continuation without a pending registration\n");
        exit(1);
    }
    add_image_step(gtc);
}
