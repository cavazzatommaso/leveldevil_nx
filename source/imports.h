/* imports.h -- the symbol table the modules are linked against.
 *
 * Every undefined symbol in liblime.so and libApplicationMain.so has exactly
 * one entry in imports.c. tools/check_imports.py compares the table with the
 * ELF import lists; the loader also refuses to start if anything is left
 * unresolved and names the symbols in the error screen.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_IMPORTS_H
#define HS_IMPORTS_H

#include <stdint.h>
#include "so_util.h"

void      imports_init(void);
/* Import table lookup only. */
uintptr_t imports_lookup(const char *name);
/* so_resolver_fn: the import table first, then exports of other modules. */
uintptr_t imports_resolve(const char *name, so_module *importer);

#endif
