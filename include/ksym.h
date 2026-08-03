/* ksym.h — kernel symbol export for runtime symbol resolution.
 *
 * PURPOSE
 *   Lets selected kernel functions register their name and address in a
 *   dedicated linker section (.ksymtab) so that patch_tools.c can locate
 *   them at run time without a live ELF symbol table (which the linker
 *   strips from kernel.elf and objcopy strips from kernel.bin).
 *
 *   Only functions explicitly annotated with EXPORT_SYMBOL appear in the
 *   table. Nothing is patchable by accident.
 *
 * RESPONSIBILITIES
 *   - Define ksym_entry_t, the table entry type.
 *   - Provide EXPORT_SYMBOL(fn) to register a function from any .c file.
 *   - Provide ksym_lookup(name) and ksym_count() as inline helpers.
 *
 * PUBLIC API
 *   EXPORT_SYMBOL(fn)     Place at file scope in a .c file, after the
 *                         function definition. Registers fn's address
 *                         under the string "fn" in .ksymtab.
 *
 *   ksym_lookup(name)     Linear scan over .ksymtab. Returns the address
 *                         of the first matching entry, or 0 if not found.
 *
 *   ksym_count()          Number of exported symbols. 0 on a kernel built
 *                         without any EXPORT_SYMBOL annotations.
 *
 * LINKER CONTRACT
 *   linker.ld must contain a .ksymtab output section that:
 *     1. Sets __start_ksymtab to the section start.
 *     2. Collects KEEP(*(ksymtab)) from every object.
 *     3. Sets __stop_ksymtab to the section end.
 *   The linker provides __start_ksymtab/__stop_ksymtab automatically for
 *   sections named without a leading dot in the *input* section attribute
 *   (section("ksymtab"), not section(".ksymtab")).
 *
 * DEPENDENCIES
 *   None. Freestanding-safe; usable in any kernel .c file.
 *
 * FUTURE EXTENSION POINTS
 *   ksym_entry_t could carry a size field once the linker is taught to
 *   emit one (e.g. via __attribute__((section("ksymtab.sizes")))), so
 *   patch_apply can refuse a patch that extends beyond a function's body.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

/* One exported symbol: a name and its run-time address. */
typedef struct {
    const char *name;
    uintptr_t   addr;
} ksym_entry_t;

/* Linker-section bounds.  Defined by the .ksymtab output section in
 * linker.ld; declared here as arrays because the address of an array is
 * the linker symbol itself (no extra dereference). */
extern const ksym_entry_t __start_ksymtab[];
extern const ksym_entry_t __stop_ksymtab[];

/* Export fn's address under the string "fn" into .ksymtab.
 * Place at file scope, after the function definition.
 * __attribute__((used)) keeps the entry alive even if nothing references
 * the variable by name.
 *
 * On host builds (FABLEOS_HOSTTEST) the linker section mechanism does not
 * work the same way (Mach-O vs ELF, no automatic __start_/__stop_ symbols),
 * so we emit a no-op. Host test suites that exercise patch_tools.c should
 * supply a synthetic table through tests/host/kshim.c instead. */
#ifdef FABLEOS_HOSTTEST
#define EXPORT_SYMBOL(fn)   /* no-op on host */
#else
#define EXPORT_SYMBOL(fn) \
    static const ksym_entry_t _ksym_##fn \
        __attribute__((used, section("ksymtab"))) = \
        { #fn, (uintptr_t)(fn) }
#endif

/* Linear scan over the table. Returns the address of the first entry
 * whose name matches exactly, or 0 if not found. */
static inline uintptr_t ksym_lookup(const char *name) {
    for (const ksym_entry_t *e = __start_ksymtab; e < __stop_ksymtab; e++) {
        const char *a = e->name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == '\0' && *b == '\0') return e->addr;
    }
    return 0;
}

/* Number of exported symbols in .ksymtab. */
static inline size_t ksym_count(void) {
    return (size_t)(__stop_ksymtab - __start_ksymtab);
}
