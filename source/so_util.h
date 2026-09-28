/* so_util.h -- load Android ARM64 shared objects into a Switch process.
 *
 * Lineage: the so_util.c of the vitaGL/fgsfds Android wrapper ports (MIT,
 * Andy Nguyen and fgsfds), by way of the Switch ports it was carried into.
 * This version reads everything from PT_DYNAMIC rather than section headers,
 * resolves exports through DT_HASH, and keeps a module list for dlopen,
 * dl_iterate_phdr and crash symbolization.
 *
 * Lifecycle of a module:
 *
 *   so_load        read the file, copy PT_LOADs into a heap staging buffer,
 *                  reserve a code-memory address range for the final mapping
 *   so_relocate    apply RELATIVE and module-internal relocations
 *   so_resolve     fill imports through a resolver callback (BIND_NOW: an
 *                  import nobody provides is reported, never left dangling)
 *   so_finalize    svcMapProcessCodeMemory the staging buffer onto the
 *                  reserved range, set RX on executable pages, RW elsewhere
 *   so_run_init_array
 *
 * After so_finalize the staging buffer is donated to the kernel and faults if
 * touched; every pointer into the module is rebased to the mapped range.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_SO_UTIL_H
#define HS_SO_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define SO_MAX_PHDRS 16

typedef struct {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} so_phdr;

typedef struct {
    uint32_t st_name;
    uint8_t  st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} so_sym;

typedef struct so_module {
    struct so_module *next;
    char name[64];

    void     *load_base;      /* heap staging buffer (donated at finalize) */
    void     *load_virtbase;  /* where the module lives and runs */
    size_t    load_size;
    void     *reservation;    /* VirtmemReservation* */
    int       finalized;

    so_phdr   phdr[SO_MAX_PHDRS]; /* pristine program headers, link-time vaddrs */
    int       phnum;

    /* Dynamic section, as offsets from the module base. */
    uint64_t  dt_symtab, dt_strtab, dt_hash;
    uint64_t  dt_rela, dt_relasz, dt_jmprel, dt_pltrelsz;
    uint64_t  dt_init_array, dt_init_arraysz;
    uint32_t  nsyms;
} so_module;

/* Returns the resolved address for an import, or 0. */
typedef uintptr_t (*so_resolver_fn)(const char *name, so_module *importer);

int  so_load(so_module *mod, const char *path, const char *shortname);
int  so_relocate(so_module *mod);
/* Returns the number of imports that could not be resolved. Their names are
 * appended to `missing` (comma separated, truncated to missing_len). */
int  so_resolve(so_module *mod, so_resolver_fn resolver, char *missing, size_t missing_len);
int  so_finalize(so_module *mod);
void so_run_init_array(so_module *mod);

/* Runtime address of a defined, non-local symbol, or 0. */
uintptr_t so_symbol(so_module *mod, const char *name);

/* Iterate every defined symbol (for the SDL stub scan). Return nonzero from
 * the callback to stop. */
typedef int (*so_sym_cb)(const char *name, uintptr_t addr, uint64_t size, int type, void *ud);
void so_foreach_symbol(so_module *mod, so_sym_cb cb, void *ud);

so_module *so_module_list(void);
so_module *so_module_by_addr(uintptr_t addr);
so_module *so_module_by_name(const char *shortname);

/* Nearest defined function symbol at or below addr. Returns 1 if found. */
int so_symbolize(uintptr_t addr, const char **modname, const char **symname, uintptr_t *offset_in_sym);

/* dl_iterate_phdr over loaded modules, bionic dl_phdr_info layout. */
int so_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data);

#endif
