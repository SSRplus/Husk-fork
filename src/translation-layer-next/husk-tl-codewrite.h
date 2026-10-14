/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Stores into code, done through the JIT region's writable view (husk-tl-codewrite.c). */
#ifndef HUSK_TL_CODEWRITE_H
#define HUSK_TL_CODEWRITE_H

#include <stdbool.h>
#include <stddef.h>

/* From now on, stores into the executable view are carried out through the writable one, and the guest may map executable
 * memory of its own (it gets a piece of the JIT region). For mod loaders that hook the game (Geode). */
void tl_codewrite_enable(void);
bool tl_codewrite_enabled(void);
/* Stores carried out so far, for the log. */
long tl_codewrite_count(void);

/*
 * A JIT's own code cache (Vita3K's dynarmic maps 128 MiB of read-write-execute memory and writes machine code into it for as long as the game runs): where the
 * process may run code of its own (a debugger attached, a jailbreak: not iOS 26 and later) the memory is real, and its pages are flipped between writable and
 * executable as the guest touches them -- a store into an executable page makes it writable, running code in a writable one makes it executable (and flushes the
 * instruction cache). A fault per page change instead of one per store, which is the difference between a game that starts and one that seems to hang.
 */
bool tl_codewrite_toggle_alloc(size_t len, void **out);
bool tl_codewrite_toggle_contains(const void *addr);
bool tl_codewrite_toggle_free(void *addr, size_t len);          /* true when the range was one of these (and is released when whole) */
long tl_codewrite_flips(void);

#endif
