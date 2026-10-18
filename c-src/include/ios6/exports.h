#ifndef IOS6_EXPORTS_H
#define IOS6_EXPORTS_H

#include <stdint.h>
#include <stddef.h>

#define EXPORT_SYMBOL_FLAGS_KIND_MASK 0x03
#define EXPORT_SYMBOL_FLAGS_KIND_REGULAR 0x00
#define EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL 0x01
#define EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION 0x04
#define EXPORT_SYMBOL_FLAGS_REEXPORT 0x08
#define EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER 0x10

typedef struct {
    char name[128];
    uint32_t address;
    uint32_t flags;
} export_symbol_t;

typedef struct {
    export_symbol_t *symbols;
    size_t count;
    size_t capacity;
} export_table_t;

int export_table_parse(const uint8_t *data, size_t size, export_table_t *out_table);
uint32_t export_table_lookup(const export_table_t *table, const char *name);
void export_table_free(export_table_t *table);

#endif /* IOS6_EXPORTS_H */
