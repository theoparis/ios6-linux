#ifndef IOS6_DYLD_H
#define IOS6_DYLD_H

#include <stdint.h>
#include "ios6/guestmem.h"
#include "ios6/macho.h"

/*
 * dyld private entry points that guest libdyld reaches through _dyld_func_lookup.
 * Images are registered by pointer and must outlive the runtime.
 */
void dyld_init(symbol_resolver_fn resolver, void *user_data);
void dyld_register_image(const loaded_image_t *image);

/*
 * __dyld_fast_stub_entry(cell, lazy_offset): binds the lazy pointer described by the
 * lazy-bind entry at `lazy_offset` of the image containing `cell`, and returns its target.
 * Exits the process when the image or symbol is unknown, as dyld does.
 */
uint32_t dyld_fast_stub_entry(guest_arena_t *arena, uint32_t cell, uint32_t lazy_offset);

/*
 * __dyld_register_func_for_add_image(func): calls func(mach_header, slide) for every
 * registered image. Calls run as guest code and return through the
 * __ios6_dyld_add_image_continue trampoline, which sets NEXT_PC itself.
 */
void dyld_register_add_image(uint32_t *gtc, uint32_t func);
void dyld_add_image_continue(uint32_t *gtc);

#endif /* IOS6_DYLD_H */
