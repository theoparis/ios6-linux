#ifndef IOS6_DYLD_H
#define IOS6_DYLD_H

#include <stdint.h>
#include "ios6/guestmem.h"
#include "ios6/macho.h"
#include "ios6/shared_cache.h"

/*
 * dyld private entry points that guest libdyld reaches through _dyld_func_lookup.
 * Loaded images are registered by pointer and must outlive the runtime.
 */
void dyld_init(symbol_resolver_fn resolver, void *user_data);
void dyld_register_image(const loaded_image_t *image);

/*
 * Builds dyld's image list for `exe` in dyld's load order: the executable, then each image's
 * dependents (LC_LOAD_DYLIB and friends) as they are first reached. Dependents come from the
 * shared cache, or from registered loaded images matched by file name. An unresolvable
 * dependency exits the process, as dyld does. The list is mirrored in guest memory as a
 * dyld_image_info array. Every image counts as bound; no initializer beyond libSystem's runs.
 */
void dyld_build_image_list(guest_arena_t *arena, shared_cache_t *cache, const loaded_image_t *exe);

/*
 * __dyld_fast_stub_entry(cell, lazy_offset): binds the lazy pointer described by the
 * lazy-bind entry at `lazy_offset` of the image containing `cell`, and returns its target.
 * Exits the process when the image or symbol is unknown, as dyld does.
 */
uint32_t dyld_fast_stub_entry(guest_arena_t *arena, uint32_t cell, uint32_t lazy_offset);

/* Image list queries behind __dyld_image_count and friends. Out-of-range indices yield 0. */
uint32_t dyld_image_count(void);
uint32_t dyld_image_header(uint32_t index);
uint32_t dyld_image_name(uint32_t index);   /* guest address of the path string */
uint32_t dyld_image_slide(uint32_t index);
uint32_t dyld_image_slide_for_header(uint32_t header);
uint32_t dyld_image_path_containing(uint32_t addr);

/*
 * __dyld_dlopen(path, mode): handle (the image's mach_header address) for an image already in
 * the list, 0 for an absent image under RTLD_NOLOAD. Loading a new image is not supported and
 * exits the process.
 */
uint32_t dyld_dlopen(const char *path, uint32_t mode);
/*
 * Registrations that call back into guest code for the images in the list. The callbacks run
 * as guest code and return through the __ios6_dyld_callback_continue trampoline; both
 * registration entries and dyld_callback_continue set NEXT_PC themselves.
 *
 *  __dyld_register_func_for_add_image(func):       func(mach_header, slide) per image
 *  __dyld_dyld_register_image_state_change_handler(state, batch, handler):
 *      batch:  handler(state, count, dyld_image_info[]) once
 *      single: handler(state, const dyld_image_info *) per image
 *      A non-NULL return (an error string) is fatal.
 */
void dyld_register_add_image(uint32_t *gtc, uint32_t func);
void dyld_register_state_handler(uint32_t *gtc, uint32_t state, uint32_t batch, uint32_t handler);
void dyld_callback_continue(uint32_t *gtc);

#endif /* IOS6_DYLD_H */
