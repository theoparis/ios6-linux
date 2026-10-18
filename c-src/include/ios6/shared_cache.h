#ifndef IOS6_SHARED_CACHE_H
#define IOS6_SHARED_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "ios6/guestmem.h"
#include "ios6/exports.h"

#define MAX_CACHE_MAPPINGS 16
#define MAX_CACHE_IMAGES 1024

typedef struct {
    uint64_t address;
    uint64_t size;
    uint64_t file_offset;
    uint32_t max_prot;
    uint32_t init_prot;
} cache_mapping_t;

typedef struct {
    char path[256];
    uint64_t address;
    export_table_t exports;
    char reexports[32][256];
    size_t reexport_count;
    bool parsed;
} cache_image_t;

typedef struct {
    char cache_path[512];
    FILE *file;
    size_t mapping_count;
    cache_mapping_t mappings[MAX_CACHE_MAPPINGS];
    size_t image_count;
    cache_image_t images[MAX_CACHE_IMAGES];
} shared_cache_t;

int shared_cache_open(shared_cache_t *cache, const char *path);
int shared_cache_map_into_arena(shared_cache_t *cache, guest_arena_t *arena);
uint32_t shared_cache_resolve(shared_cache_t *cache, const char *dylib_path, const char *symbol);
/* Fills `out` with the image's __mod_init_func pointers; returns count or -1. */
int shared_cache_mod_init_funcs(shared_cache_t *cache, const char *path, uint32_t *out, size_t max);
void shared_cache_close(shared_cache_t *cache);

#endif /* IOS6_SHARED_CACHE_H */
