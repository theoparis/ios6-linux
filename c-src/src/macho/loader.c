#include "ios6/macho.h"
#include "ios6/exports.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REBASE_TYPE_POINTER 1
#define REBASE_OPCODE_MASK 0xF0
#define REBASE_IMMEDIATE_MASK 0x0F
#define REBASE_OPCODE_DONE 0x00
#define REBASE_OPCODE_SET_TYPE_IMM 0x10
#define REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x20
#define REBASE_OPCODE_ADD_ADDR_ULEB 0x30
#define REBASE_OPCODE_ADD_ADDR_IMM_SCALED 0x40
#define REBASE_OPCODE_DO_REBASE_IMM_TIMES 0x50
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES 0x60
#define REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB 0x70
#define REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB 0x80

#define BIND_TYPE_POINTER 1
#define BIND_OPCODE_MASK 0xF0
#define BIND_IMMEDIATE_MASK 0x0F
#define BIND_OPCODE_DONE 0x00
#define BIND_OPCODE_SET_DYLIB_ORDINAL_IMM 0x10
#define BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB 0x20
#define BIND_OPCODE_SET_DYLIB_SPECIAL_IMM 0x30
#define BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM 0x40
#define BIND_OPCODE_SET_TYPE_IMM 0x50
#define BIND_OPCODE_SET_ADDEND_SLEB 0x60
#define BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB 0x70
#define BIND_OPCODE_ADD_ADDR_ULEB 0x80
#define BIND_OPCODE_DO_BIND 0x90
#define BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB 0xA0
#define BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED 0xB0
#define BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB 0xC0

static uint64_t read_uleb128(const uint8_t **p, const uint8_t *end) {
    uint64_t result = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t byte = *(*p)++;
        result |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    return result;
}

static int64_t read_sleb128(const uint8_t **p, const uint8_t *end) {
    int64_t result = 0;
    int shift = 0;
    uint8_t byte = 0;
    while (*p < end) {
        byte = *(*p)++;
        result |= ((int64_t)(byte & 0x7f)) << shift;
        shift += 7;
        if ((byte & 0x80) == 0) break;
    }
    if (shift < 64 && (byte & 0x40) != 0) {
        result |= (!0ULL) << shift;
    }
    return result;
}

static void apply_rebase(guest_arena_t *arena,
                         const segment_command_32_t *segs,
                         size_t seg_count,
                         uint32_t slide,
                         const uint8_t *opcodes,
                         size_t len) {
    if (slide == 0 || len == 0) return;
    const uint8_t *p = opcodes;
    const uint8_t *end = opcodes + len;

    size_t seg_index = 0;
    uint32_t seg_offset = 0;

    while (p < end) {
        uint8_t byte = *p++;
        uint8_t opcode = byte & REBASE_OPCODE_MASK;
        uint8_t imm = byte & REBASE_IMMEDIATE_MASK;

        switch (opcode) {
            case REBASE_OPCODE_DONE:
                return;
            case REBASE_OPCODE_SET_TYPE_IMM:
                break;
            case REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_index = imm;
                seg_offset = (uint32_t)read_uleb128(&p, end);
                break;
            case REBASE_OPCODE_ADD_ADDR_ULEB:
                seg_offset += (uint32_t)read_uleb128(&p, end);
                break;
            case REBASE_OPCODE_ADD_ADDR_IMM_SCALED:
                seg_offset += imm * 4;
                break;
            case REBASE_OPCODE_DO_REBASE_IMM_TIMES: {
                for (int i = 0; i < imm; i++) {
                    if (seg_index < seg_count) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target += slide;
                    }
                    seg_offset += 4;
                }
                break;
            }
            case REBASE_OPCODE_DO_REBASE_ULEB_TIMES: {
                uint64_t count = read_uleb128(&p, end);
                for (uint64_t i = 0; i < count; i++) {
                    if (seg_index < seg_count) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target += slide;
                    }
                    seg_offset += 4;
                }
                break;
            }
            case 0x80: { // REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB
                uint64_t count = read_uleb128(&p, end);
                uint32_t skip = (uint32_t)read_uleb128(&p, end);
                for (uint64_t i = 0; i < count; i++) {
                    if (seg_index < seg_count) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target += slide;
                    }
                    seg_offset += 4 + skip;
                }
                break;
            }
            case REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: {
                if (seg_index < seg_count) {
                    uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                    uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                    if (target) *target += slide;
                }
                seg_offset += 4 + (uint32_t)read_uleb128(&p, end);
                break;
            }
            default:
                break;
        }
    }
}

static void apply_bind(guest_arena_t *arena,
                       const segment_command_32_t *segs,
                       size_t seg_count,
                       uint32_t slide,
                       const uint8_t *opcodes,
                       size_t len,
                       symbol_resolver_fn resolver,
                       void *user_data) {
    if (len == 0 || !resolver) return;
    const uint8_t *p = opcodes;
    const uint8_t *end = opcodes + len;

    size_t seg_index = 0;
    uint32_t seg_offset = 0;
    const char *symbol_name = "";
    int64_t addend = 0;

    while (p < end) {
        uint8_t byte = *p++;
        uint8_t opcode = byte & BIND_OPCODE_MASK;
        uint8_t imm = byte & BIND_IMMEDIATE_MASK;

        switch (opcode) {
            case BIND_OPCODE_DONE:
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_IMM:
            case BIND_OPCODE_SET_DYLIB_SPECIAL_IMM:
                break;
            case BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB:
                read_uleb128(&p, end);
                break;
            case BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: {
                symbol_name = (const char *)p;
                while (p < end && *p != '\0') p++;
                if (p < end) p++; // Skip null byte
                break;
            }
            case BIND_OPCODE_SET_TYPE_IMM:
                break;
            case BIND_OPCODE_SET_ADDEND_SLEB:
                addend = read_sleb128(&p, end);
                break;
            case BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB:
                seg_index = imm;
                seg_offset = (uint32_t)read_uleb128(&p, end);
                break;
            case BIND_OPCODE_ADD_ADDR_ULEB:
                seg_offset += (uint32_t)read_uleb128(&p, end);
                break;
            case BIND_OPCODE_DO_BIND: {
                if (seg_index < seg_count && strlen(symbol_name) > 0) {
                    uint32_t resolved = resolver(symbol_name, user_data);
                    if (resolved != 0) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target = resolved + (uint32_t)addend;
                    }
                }
                seg_offset += 4;
                break;
            }
            case BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: {
                if (seg_index < seg_count && strlen(symbol_name) > 0) {
                    uint32_t resolved = resolver(symbol_name, user_data);
                    if (resolved != 0) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target = resolved + (uint32_t)addend;
                    }
                }
                seg_offset += 4 + (uint32_t)read_uleb128(&p, end);
                break;
            }
            case BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: {
                if (seg_index < seg_count && strlen(symbol_name) > 0) {
                    uint32_t resolved = resolver(symbol_name, user_data);
                    if (resolved != 0) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target = resolved + (uint32_t)addend;
                    }
                }
                seg_offset += 4 + imm * 4;
                break;
            }
            case 0xC0: { // BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB
                uint64_t count = read_uleb128(&p, end);
                uint32_t skip = (uint32_t)read_uleb128(&p, end);
                uint32_t resolved = (seg_index < seg_count && strlen(symbol_name) > 0)
                                        ? resolver(symbol_name, user_data) : 0;
                for (uint64_t i = 0; i < count; i++) {
                    if (resolved != 0) {
                        uint32_t addr = segs[seg_index].vmaddr + slide + seg_offset;
                        uint32_t *target = (uint32_t *)guest_arena_to_host(arena, addr);
                        if (target) *target = resolved + (uint32_t)addend;
                    }
                    seg_offset += 4 + skip;
                }
                break;
            }
            default:
                break;
        }
    }
}

int macho_load_file(guest_arena_t *arena,
                    const char *path,
                    symbol_resolver_fn resolver,
                    void *user_data,
                    loaded_image_t *out_image) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    memset(out_image, 0, sizeof(*out_image));

    if (fread(&out_image->header, sizeof(mach_header_32_t), 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (out_image->header.magic != MH_MAGIC || out_image->header.cputype != CPU_TYPE_ARM) {
        fclose(f);
        return -1;
    }

    uint32_t rebase_off = 0, rebase_size = 0;
    uint32_t bind_off = 0, bind_size = 0;
    uint32_t lazy_bind_off = 0, lazy_bind_size = 0;
    uint32_t export_off = 0, export_size = 0;
    uint32_t unix_entry = 0;
    uint64_t main_entryoff = 0;
    bool has_main = false;

    for (uint32_t i = 0; i < out_image->header.ncmds; i++) {
        long cmd_start = ftell(f);
        uint32_t cmd = 0, cmdsize = 0;
        if (fread(&cmd, 4, 1, f) != 1 || fread(&cmdsize, 4, 1, f) != 1) break;

        if (cmd == LC_SEGMENT && out_image->segment_count < MAX_SEGMENTS) {
            segment_command_32_t *seg = &out_image->segments[out_image->segment_count++];
            if (fread(seg->segname, 16, 1, f) != 1) break;
            if (fread(&seg->vmaddr, 4, 8, f) != 8) break; // vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, flags
        } else if (cmd == LC_DYLD_INFO_ONLY) {
            uint32_t info[10];
            if (fread(info, 4, 10, f) == 10) {
                rebase_off = info[0]; rebase_size = info[1];
                bind_off = info[2]; bind_size = info[3];
                lazy_bind_off = info[6]; lazy_bind_size = info[7];
                export_off = info[8]; export_size = info[9];
            }
        } else if (cmd == LC_MAIN) {
            uint64_t entryoff = 0, stacksize = 0;
            if (fread(&entryoff, 8, 1, f) == 1 && fread(&stacksize, 8, 1, f) == 1) {
                main_entryoff = entryoff;
                has_main = true;
            }
        } else if (cmd == LC_UNIXTHREAD) {
            fseek(f, cmd_start + 40 + 60, SEEK_SET); // thread state pc offset
            fread(&unix_entry, 4, 1, f);
        }

        fseek(f, cmd_start + cmdsize, SEEK_SET);
    }

    // Determine preferred base and slide
    uint32_t preferred_base = 0;
    for (size_t i = 0; i < out_image->segment_count; i++) {
        if (strcmp(out_image->segments[i].segname, "__TEXT") == 0) {
            preferred_base = out_image->segments[i].vmaddr;
            break;
        }
    }

    uint32_t slide = 0;
    if (out_image->header.filetype == 6 /* MH_DYLIB */) {
        // Dylibs go to their own region so they never collide with the main executable.
        static uint32_t next_dylib_base = 0x40000000u;
        uint32_t extent = 0;
        for (size_t i = 0; i < out_image->segment_count; i++) {
            segment_command_32_t *sg = &out_image->segments[i];
            if (strcmp(sg->segname, "__PAGEZERO") == 0 || sg->vmsize == 0) continue;
            uint32_t end = sg->vmaddr - preferred_base + sg->vmsize;
            if (end > extent) extent = end;
        }
        slide = next_dylib_base - preferred_base;
        next_dylib_base += (extent + 0xffffu) & ~0xffffu;
    } else if (preferred_base < MMAP_MIN_ADDR) {
        slide = (uint32_t)MMAP_MIN_ADDR - preferred_base;
    }
    out_image->slide = slide;
    out_image->base_addr = preferred_base + slide;

    if (has_main) {
        out_image->entry_point = out_image->base_addr + (uint32_t)main_entryoff;
    } else {
        out_image->entry_point = unix_entry + slide;
    }

    // Map segments
    for (size_t i = 0; i < out_image->segment_count; i++) {
        segment_command_32_t *seg = &out_image->segments[i];
        if (seg->vmsize == 0 || strcmp(seg->segname, "__PAGEZERO") == 0) continue;

        uint32_t start = seg->vmaddr + slide;
        guest_arena_mprotect(arena, start, seg->vmsize, PROT_READ | PROT_WRITE);

        if (seg->filesize > 0) {
            fseek(f, seg->fileoff, SEEK_SET);
            void *dst = guest_arena_to_host(arena, start);
            fread(dst, seg->filesize, 1, f);
        }

        if (seg->vmsize > seg->filesize) {
            void *dst_bss = guest_arena_to_host(arena, start + seg->filesize);
            memset(dst_bss, 0, seg->vmsize - seg->filesize);
        }

        int prot = PROT_READ;
        if (seg->initprot & 2) prot |= PROT_WRITE;
        if (seg->initprot & 4) prot |= PROT_EXEC;
        guest_arena_mprotect(arena, start, seg->vmsize, prot);
    }

    // Handle rebase
    if (rebase_size > 0 && slide > 0) {
        uint8_t *rebase_buf = malloc(rebase_size);
        fseek(f, rebase_off, SEEK_SET);
        fread(rebase_buf, rebase_size, 1, f);

        for (size_t i = 0; i < out_image->segment_count; i++) {
            segment_command_32_t *seg = &out_image->segments[i];
            if (seg->vmsize > 0 && strcmp(seg->segname, "__PAGEZERO") != 0) {
                guest_arena_mprotect(arena, seg->vmaddr + slide, seg->vmsize, PROT_READ | PROT_WRITE);
            }
        }

        apply_rebase(arena, out_image->segments, out_image->segment_count, slide, rebase_buf, rebase_size);
        free(rebase_buf);
    }

    // Handle bind
    if (bind_size > 0) {
        uint8_t *bind_buf = malloc(bind_size);
        fseek(f, bind_off, SEEK_SET);
        fread(bind_buf, bind_size, 1, f);
        apply_bind(arena, out_image->segments, out_image->segment_count, slide, bind_buf, bind_size, resolver, user_data);
        free(bind_buf);
    }

    if (lazy_bind_size > 0) {
        uint8_t *lazy_buf = malloc(lazy_bind_size);
        fseek(f, lazy_bind_off, SEEK_SET);
        fread(lazy_buf, lazy_bind_size, 1, f);
        apply_bind(arena, out_image->segments, out_image->segment_count, slide, lazy_buf, lazy_bind_size, resolver, user_data);
        free(lazy_buf);
    }

    // Parse export trie if present
    if (export_size > 0) {
        uint8_t *exp_buf = malloc(export_size);
        fseek(f, export_off, SEEK_SET);
        fread(exp_buf, export_size, 1, f);
        export_table_parse(exp_buf, export_size, &out_image->exports);
        free(exp_buf);
    }

    // Restore segment protections
    for (size_t i = 0; i < out_image->segment_count; i++) {
        segment_command_32_t *seg = &out_image->segments[i];
        if (seg->vmsize > 0 && strcmp(seg->segname, "__PAGEZERO") != 0) {
            int prot = PROT_READ;
            if (seg->initprot & 2) prot |= PROT_WRITE;
            if (seg->initprot & 4) prot |= PROT_EXEC;
            guest_arena_mprotect(arena, seg->vmaddr + slide, seg->vmsize, prot);
        }
    }

    fclose(f);
    return 0;
}
