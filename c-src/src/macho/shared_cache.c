#include "ios6/shared_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char magic[16];
    uint32_t mapping_offset;
    uint32_t mapping_count;
    uint32_t images_offset;
    uint32_t images_count;
} dsc_header_t;

typedef struct {
    uint64_t address;
    uint64_t size;
    uint64_t file_offset;
    uint32_t max_prot;
    uint32_t init_prot;
} dsc_mapping_t;

typedef struct {
    uint64_t address;
    uint64_t mod_time;
    uint64_t inode;
    uint32_t path_file_offset;
    uint32_t pad;
} dsc_image_t;

static int parse_image_exports(shared_cache_t *cache, size_t idx) {
    cache_image_t *img = &cache->images[idx];
    if (img->parsed) return 0;
    img->parsed = true;

    // Find file offset of image: img->address - mapping[0].address
    uint64_t base_addr = cache->mappings[0].address;
    uint32_t file_off = (uint32_t)(img->address - base_addr);

    FILE *f = cache->file;
    fseek(f, file_off + 16, SEEK_SET);
    uint32_t ncmds;
    if (fread(&ncmds, 4, 1, f) != 1) return -1;
    fseek(f, file_off + 28, SEEK_SET);

    uint32_t export_off = 0, export_sz = 0;

    for (uint32_t c = 0; c < ncmds; c++) {
        long pos = ftell(f);
        uint32_t cmd, cmdsize;
        if (fread(&cmd, 4, 1, f) != 1 || fread(&cmdsize, 4, 1, f) != 1) break;

        if (cmd == (0x22 | 0x80000000)) { // LC_DYLD_INFO_ONLY
            uint32_t info[10];
            if (fread(info, 4, 10, f) == 10) {
                export_off = info[8];
                export_sz = info[9];
            }
        } else if (cmd == (0x1f | 0x80000000)) { // LC_REEXPORT_DYLIB
            uint32_t stroff;
            if (fread(&stroff, 4, 1, f) == 1 && img->reexport_count < 32) {
                fseek(f, pos + stroff, SEEK_SET);
                char name[256];
                if (fgets(name, sizeof(name), f)) {
                    name[strcspn(name, "\r\n")] = '\0';
                    strncpy(img->reexports[img->reexport_count++], name, 255);
                }
            }
        }
        fseek(f, pos + cmdsize, SEEK_SET);
    }

    if (export_sz > 0) {
        fseek(f, export_off, SEEK_SET);
        uint8_t *data = (uint8_t *)malloc(export_sz);
        if (data) {
            fread(data, export_sz, 1, f);
            export_table_parse(data, export_sz, &img->exports);
            free(data);
        }
    }

    return 0;
}

int shared_cache_open(shared_cache_t *cache, const char *path) {
    memset(cache, 0, sizeof(*cache));
    strncpy(cache->cache_path, path, sizeof(cache->cache_path) - 1);

    cache->file = fopen(path, "rb");
    if (!cache->file) return -1;

    dsc_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, cache->file) != 1) {
        fclose(cache->file);
        return -1;
    }

    if (strncmp(hdr.magic, "dyld_v1", 7) != 0) {
        fclose(cache->file);
        return -1;
    }

    // Read mappings
    fseek(cache->file, hdr.mapping_offset, SEEK_SET);
    cache->mapping_count = hdr.mapping_count < MAX_CACHE_MAPPINGS ? hdr.mapping_count : MAX_CACHE_MAPPINGS;
    for (size_t i = 0; i < cache->mapping_count; i++) {
        dsc_mapping_t m;
        fread(&m, sizeof(m), 1, cache->file);
        cache->mappings[i].address = m.address;
        cache->mappings[i].size = m.size;
        cache->mappings[i].file_offset = m.file_offset;
        cache->mappings[i].max_prot = m.max_prot;
        cache->mappings[i].init_prot = m.init_prot;
    }

    // Read images
    fseek(cache->file, hdr.images_offset, SEEK_SET);
    cache->image_count = hdr.images_count < MAX_CACHE_IMAGES ? hdr.images_count : MAX_CACHE_IMAGES;
    for (size_t i = 0; i < cache->image_count; i++) {
        dsc_image_t img;
        fread(&img, sizeof(img), 1, cache->file);
        cache->images[i].address = img.address;

        long cur = ftell(cache->file);
        fseek(cache->file, img.path_file_offset, SEEK_SET);
        if (fgets(cache->images[i].path, sizeof(cache->images[i].path), cache->file)) {
            cache->images[i].path[strcspn(cache->images[i].path, "\r\n")] = '\0';
        }
        fseek(cache->file, cur, SEEK_SET);
    }

    return 0;
}

int shared_cache_map_into_arena(shared_cache_t *cache, guest_arena_t *arena) {
    for (size_t i = 0; i < cache->mapping_count; i++) {
        cache_mapping_t *m = &cache->mappings[i];
        int prot = PROT_READ;
        if (m->init_prot & 2) prot |= PROT_WRITE;
        if (m->init_prot & 4) prot |= PROT_EXEC;

        guest_arena_mprotect(arena, (uint32_t)m->address, (size_t)m->size, PROT_READ | PROT_WRITE);

        fseek(cache->file, m->file_offset, SEEK_SET);
        void *dst = guest_arena_to_host(arena, (uint32_t)m->address);
        if (dst) {
            // Read in 4 MiB chunks to avoid large stack buffers
            size_t remaining = (size_t)m->size;
            uint8_t *cur = (uint8_t *)dst;
            while (remaining > 0) {
                size_t chunk = remaining < (4 * 1024 * 1024) ? remaining : (4 * 1024 * 1024);
                if (fread(cur, chunk, 1, cache->file) != 1) break;
                cur += chunk;
                remaining -= chunk;
            }
        }

        guest_arena_mprotect(arena, (uint32_t)m->address, (size_t)m->size, prot);
    }
    return 0;
}

static uint32_t resolve_depth(shared_cache_t *cache, const char *dylib_path, const char *symbol, int depth) {
    if (depth > 8) return 0;

    int idx = -1;
    for (size_t i = 0; i < cache->image_count; i++) {
        if (strcmp(cache->images[i].path, dylib_path) == 0) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) return 0;

    parse_image_exports(cache, (size_t)idx);
    cache_image_t *img = &cache->images[idx];

    uint32_t addr = export_table_lookup(&img->exports, symbol);
    if (addr != 0) {
        return (uint32_t)img->address + addr;
    }

    // Try reexports
    for (size_t r = 0; r < img->reexport_count; r++) {
        uint32_t r_addr = resolve_depth(cache, img->reexports[r], symbol, depth + 1);
        if (r_addr != 0) return r_addr;
    }

    return 0;
}

uint32_t shared_cache_resolve(shared_cache_t *cache, const char *dylib_path, const char *symbol) {
    return resolve_depth(cache, dylib_path, symbol, 0);
}

/* Translate a cache-virtual address to a file offset using the mapping table. */
static int cache_addr_to_file(const shared_cache_t *cache, uint64_t addr, uint64_t *off) {
    for (size_t i = 0; i < cache->mapping_count; i++) {
        const cache_mapping_t *m = &cache->mappings[i];
        if (addr >= m->address && addr < m->address + m->size) {
            *off = addr - m->address + m->file_offset;
            return 0;
        }
    }
    return -1;
}

int shared_cache_mod_init_funcs(shared_cache_t *cache, const char *path,
                                uint32_t *out, size_t max) {
    size_t idx = (size_t)-1;
    for (size_t i = 0; i < cache->image_count; i++) {
        if (strcmp(cache->images[i].path, path) == 0) { idx = i; break; }
    }
    if (idx == (size_t)-1) return -1;

    uint64_t base = cache->images[idx].address;
    uint64_t hoff;
    if (cache_addr_to_file(cache, base, &hoff) != 0) return -1;

    uint32_t ncmds = 0;
    fseek(cache->file, (long)hoff + 16, SEEK_SET);
    if (fread(&ncmds, 4, 1, cache->file) != 1) return -1;
    fseek(cache->file, (long)hoff + 28, SEEK_SET);

    size_t n = 0;
    for (uint32_t c = 0; c < ncmds; c++) {
        long pos = ftell(cache->file);
        uint32_t cmd, cmdsize;
        if (fread(&cmd, 4, 1, cache->file) != 1 || fread(&cmdsize, 4, 1, cache->file) != 1) break;
        if (cmd == 0x1) { /* LC_SEGMENT */
            uint32_t nsects;
            fseek(cache->file, pos + 48, SEEK_SET);
            if (fread(&nsects, 4, 1, cache->file) != 1) break;
            for (uint32_t s = 0; s < nsects; s++) {
                char sectname[16];
                uint32_t addr, size, flags;
                fseek(cache->file, pos + 56 + s * 68, SEEK_SET);
                if (fread(sectname, 16, 1, cache->file) != 1) break;
                fseek(cache->file, pos + 56 + s * 68 + 32, SEEK_SET);
                if (fread(&addr, 4, 1, cache->file) != 1) break;
                if (fread(&size, 4, 1, cache->file) != 1) break;
                fseek(cache->file, pos + 56 + s * 68 + 56, SEEK_SET);
                if (fread(&flags, 4, 1, cache->file) != 1) break;
                if ((flags & 0xff) != 0x9 /* S_MOD_INIT_FUNC_POINTERS */) continue;
                uint64_t foff;
                if (cache_addr_to_file(cache, addr, &foff) != 0) continue;
                fseek(cache->file, (long)foff, SEEK_SET);
                for (uint32_t k = 0; k < size / 4 && n < max; k++) {
                    uint32_t fn;
                    if (fread(&fn, 4, 1, cache->file) != 1) break;
                    if (fn) out[n++] = fn;
                }
            }
        }
        fseek(cache->file, pos + cmdsize, SEEK_SET);
    }
    return (int)n;
}

void shared_cache_close(shared_cache_t *cache) {
    if (cache->file) {
        fclose(cache->file);
        cache->file = NULL;
    }
    for (size_t i = 0; i < cache->image_count; i++) {
        export_table_free(&cache->images[i].exports);
    }
}
