#include "ios6/exports.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static uint64_t read_uleb128(const uint8_t *data, size_t size, size_t *pos) {
    uint64_t result = 0;
    int shift = 0;
    while (*pos < size) {
        uint8_t byte = data[(*pos)++];
        result |= ((uint64_t)(byte & 0x7f)) << shift;
        if ((byte & 0x80) == 0) break;
        shift += 7;
    }
    return result;
}

static const char *read_cstr(const uint8_t *data, size_t size, size_t *pos) {
    size_t start = *pos;
    while (*pos < size && data[*pos] != '\0') (*pos)++;
    if (*pos < size) (*pos)++; // Skip NUL
    return (const char *)&data[start];
}

typedef struct {
    size_t node_offset;
    char prefix[128];
} trie_stack_frame_t;

int export_table_parse(const uint8_t *data, size_t size, export_table_t *out_table) {
    out_table->count = 0;
    out_table->capacity = 256;
    out_table->symbols = (export_symbol_t *)malloc(out_table->capacity * sizeof(export_symbol_t));
    if (!out_table->symbols) return -1;

    trie_stack_frame_t *stack = (trie_stack_frame_t *)malloc(1024 * sizeof(trie_stack_frame_t));
    int stack_top = 0;

    stack[stack_top].node_offset = 0;
    stack[stack_top].prefix[0] = '\0';
    stack_top++;

    while (stack_top > 0) {
        stack_top--;
        size_t node_offset = stack[stack_top].node_offset;
        char prefix[128];
        strncpy(prefix, stack[stack_top].prefix, sizeof(prefix) - 1);
        prefix[sizeof(prefix) - 1] = '\0';

        if (node_offset >= size) continue;

        size_t pos = node_offset;
        uint64_t terminal_size = read_uleb128(data, size, &pos);
        size_t terminal_end = pos + terminal_size;

        if (terminal_size != 0 && strlen(prefix) > 0) {
            size_t tp = pos;
            uint64_t flags = read_uleb128(data, size, &tp);
            if ((flags & EXPORT_SYMBOL_FLAGS_REEXPORT) == 0) {
                uint64_t address = read_uleb128(data, size, &tp);

                if (out_table->count >= out_table->capacity) {
                    out_table->capacity *= 2;
                    out_table->symbols = (export_symbol_t *)realloc(
                        out_table->symbols, out_table->capacity * sizeof(export_symbol_t));
                }
                export_symbol_t *sym = &out_table->symbols[out_table->count++];
                strncpy(sym->name, prefix, sizeof(sym->name) - 1);
                sym->name[sizeof(sym->name) - 1] = '\0';
                sym->address = (uint32_t)address;
                sym->flags = (uint32_t)flags;
            }
        }

        size_t child_pos = terminal_end;
        if (child_pos >= size) continue;
        uint8_t child_count = data[child_pos++];

        for (uint8_t i = 0; i < child_count && stack_top < 1024; i++) {
            const char *label = read_cstr(data, size, &child_pos);
            uint64_t child_offset = read_uleb128(data, size, &child_pos);

            stack[stack_top].node_offset = (size_t)child_offset;
            snprintf(stack[stack_top].prefix, sizeof(stack[stack_top].prefix), "%s%s", prefix, label);
            stack_top++;
        }
    }

    free(stack);
    return 0;
}

uint32_t export_table_lookup(const export_table_t *table, const char *name) {
    if (!table) return 0;
    for (size_t i = 0; i < table->count; i++) {
        if (strcmp(table->symbols[i].name, name) == 0) {
            return table->symbols[i].address;
        }
    }
    return 0;
}

void export_table_free(export_table_t *table) {
    if (table && table->symbols) {
        free(table->symbols);
        table->symbols = NULL;
        table->count = 0;
        table->capacity = 0;
    }
}
