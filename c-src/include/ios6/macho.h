#ifndef IOS6_MACHO_H
#define IOS6_MACHO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "ios6/guestmem.h"
#include "ios6/exports.h"

#define MH_MAGIC 0xfeedface

#define CPU_TYPE_ARM 12
#define CPU_SUBTYPE_ARM_V7 9
#define CPU_SUBTYPE_ARM_V7S 11

#define MH_EXECUTE 2
#define MH_DYLIB 6
#define MH_PIE 0x200000

#define LC_SEGMENT 0x1
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_LOAD_DYLIB 0xc
#define LC_UNIXTHREAD 0x5
#define LC_DYLD_INFO_ONLY (0x22 | 0x80000000)
#define LC_MAIN (0x28 | 0x80000000)

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
} mach_header_32_t;

typedef struct {
    char segname[16];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} segment_command_32_t;

#define MAX_SEGMENTS 32

typedef struct {
    mach_header_32_t header;
    uint32_t slide;
    guest_addr_t base_addr;
    guest_addr_t entry_point;
    size_t segment_count;
    segment_command_32_t segments[MAX_SEGMENTS];
    export_table_t exports;
} loaded_image_t;

typedef uint32_t (*symbol_resolver_fn)(const char *name, void *user_data);

int macho_load_file(guest_arena_t *arena,
                    const char *path,
                    symbol_resolver_fn resolver,
                    void *user_data,
                    loaded_image_t *out_image);

#endif /* IOS6_MACHO_H */
