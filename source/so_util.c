/* so_util.c -- see so_util.h. MIT licensed, see LICENSE. */
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "so_util.h"

#define PT_LOAD_    1
#define PT_DYNAMIC_ 2
#define PF_X_       1

#define DT_NULL_            0
#define DT_HASH_            4
#define DT_STRTAB_          5
#define DT_SYMTAB_          6
#define DT_RELA_            7
#define DT_RELASZ_          8
#define DT_PLTRELSZ_        2
#define DT_JMPREL_          23
#define DT_INIT_ARRAY_      25
#define DT_INIT_ARRAYSZ_    27

#define R_AARCH64_ABS64_     257
#define R_AARCH64_GLOB_DAT_  1025
#define R_AARCH64_JUMP_SLOT_ 1026
#define R_AARCH64_RELATIVE_  1027

#define STB_LOCAL_  0
#define STB_WEAK_   2
#define STT_FUNC_   2

typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } so_rela;

static so_module *g_modules;

#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))

static inline uintptr_t base_of(const so_module *m)
{
    return (uintptr_t)(m->finalized ? m->load_virtbase : m->load_base);
}

static inline void *mod_ptr(const so_module *m, uint64_t off)
{
    return (void *)(base_of(m) + (uintptr_t)off);
}

so_module *so_module_list(void) { return g_modules; }

int so_load(so_module *mod, const char *path, const char *shortname)
{
    FILE *fp;
    long fsize;
    unsigned char *img;
    uint64_t phoff;
    uint16_t phentsize, phnum;
    int i;

    memset(mod, 0, sizeof(*mod));
    snprintf(mod->name, sizeof(mod->name), "%s", shortname);

    fp = fopen(path, "rb");
    if (!fp) {
        LOGE("so_load: cannot open %s", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize < 0x100) {
        fclose(fp);
        return -2;
    }
    img = malloc((size_t)fsize);
    if (!img) {
        fclose(fp);
        return -3;
    }
    if (fread(img, 1, (size_t)fsize, fp) != (size_t)fsize) {
        LOGE("so_load: short read on %s", path);
        fclose(fp);
        free(img);
        return -2;
    }
    fclose(fp);

    if (memcmp(img, "\x7f" "ELF", 4) != 0 || img[4] != 2 || img[5] != 1) {
        LOGE("so_load: %s is not a 64-bit little-endian ELF", path);
        goto bad;
    }
    if ((img[16] | (img[17] << 8)) != 3 || (img[18] | (img[19] << 8)) != 183) {
        LOGE("so_load: %s is not an AArch64 shared object (wrong APK ABI folder?)", path);
        goto bad;
    }
    memcpy(&phoff, img + 32, 8);
    memcpy(&phentsize, img + 54, 2);
    memcpy(&phnum, img + 56, 2);
    if (phentsize != sizeof(so_phdr) || phnum > SO_MAX_PHDRS ||
        phoff + (uint64_t)phnum * phentsize > (uint64_t)fsize)
        goto bad;
    memcpy(mod->phdr, img + phoff, (size_t)phnum * sizeof(so_phdr));
    mod->phnum = phnum;

    for (i = 0; i < phnum; i++) {
        const so_phdr *p = &mod->phdr[i];
        if (p->p_type == PT_LOAD_ && p->p_vaddr + p->p_memsz > mod->load_size)
            mod->load_size = (size_t)(p->p_vaddr + p->p_memsz);
    }
    mod->load_size = ALIGN_UP(mod->load_size, 0x1000);
    if (!mod->load_size)
        goto bad;

    mod->load_base = memalign(0x1000, mod->load_size);
    if (!mod->load_base) {
        LOGE("so_load: out of memory for %s (%zu KB)", shortname, mod->load_size / 1024);
        free(img);
        return -3;
    }
    memset(mod->load_base, 0, mod->load_size);

    for (i = 0; i < phnum; i++) {
        const so_phdr *p = &mod->phdr[i];
        if (p->p_type != PT_LOAD_)
            continue;
        if (p->p_offset + p->p_filesz > (uint64_t)fsize) {
            free(mod->load_base);
            mod->load_base = NULL;
            goto bad;
        }
        memcpy((char *)mod->load_base + p->p_vaddr, img + p->p_offset, (size_t)p->p_filesz);
    }
    free(img);
    img = NULL;

    for (i = 0; i < phnum; i++) {
        const so_phdr *p = &mod->phdr[i];
        const uint64_t *dyn;
        if (p->p_type != PT_DYNAMIC_)
            continue;
        for (dyn = mod_ptr(mod, p->p_vaddr); dyn[0] != DT_NULL_; dyn += 2) {
            switch (dyn[0]) {
            case DT_HASH_:         mod->dt_hash = dyn[1]; break;
            case DT_STRTAB_:       mod->dt_strtab = dyn[1]; break;
            case DT_SYMTAB_:       mod->dt_symtab = dyn[1]; break;
            case DT_RELA_:         mod->dt_rela = dyn[1]; break;
            case DT_RELASZ_:       mod->dt_relasz = dyn[1]; break;
            case DT_JMPREL_:       mod->dt_jmprel = dyn[1]; break;
            case DT_PLTRELSZ_:     mod->dt_pltrelsz = dyn[1]; break;
            case DT_INIT_ARRAY_:   mod->dt_init_array = dyn[1]; break;
            case DT_INIT_ARRAYSZ_: mod->dt_init_arraysz = dyn[1]; break;
            default: break;
            }
        }
    }
    if (!mod->dt_symtab || !mod->dt_strtab || !mod->dt_hash) {
        LOGE("so_load: %s has no DT_SYMTAB/DT_STRTAB/DT_HASH", shortname);
        free(mod->load_base);
        mod->load_base = NULL;
        return -2;
    }
    mod->nsyms = ((const uint32_t *)mod_ptr(mod, mod->dt_hash))[1];

    virtmemLock();
    mod->load_virtbase = virtmemFindCodeMemory(mod->load_size, 0x1000);
    if (mod->load_virtbase)
        mod->reservation = virtmemAddReservation(mod->load_virtbase, mod->load_size);
    virtmemUnlock();
    if (!mod->load_virtbase || !mod->reservation) {
        LOGE("so_load: no code address space for %s", shortname);
        free(mod->load_base);
        mod->load_base = NULL;
        return -4;
    }

    mod->next = NULL;
    if (!g_modules) {
        g_modules = mod;
    } else {
        so_module *m = g_modules;
        while (m->next)
            m = m->next;
        m->next = mod;
    }
    LOGI("so_load: %s staged, %zu KB, will run at %p, %u symbols",
         shortname, mod->load_size / 1024, mod->load_virtbase, mod->nsyms);
    return 0;

bad:
    free(img);
    return -2;
}

static so_sym *symtab(const so_module *m) { return (so_sym *)mod_ptr(m, m->dt_symtab); }
static const char *strtab(const so_module *m) { return (const char *)mod_ptr(m, m->dt_strtab); }

static int reloc_range(so_module *m, uint64_t off, uint64_t size, int resolve,
                       so_resolver_fn resolver, int *missing, char *missing_buf, size_t missing_len)
{
    so_rela *r = (so_rela *)mod_ptr(m, off);
    size_t n = (size_t)(size / sizeof(so_rela)), i;
    so_sym *syms = symtab(m);
    const char *strs = strtab(m);

    for (i = 0; i < n; i++) {
        uint32_t type = (uint32_t)(r[i].r_info & 0xFFFFFFFFu);
        uint32_t symi = (uint32_t)(r[i].r_info >> 32);
        uint64_t *where;
        so_sym *sym = symi ? &syms[symi] : NULL;

        if (r[i].r_offset + 8 > m->load_size) {
            LOGE("%s: relocation outside the image at 0x%llx", m->name,
                 (unsigned long long)r[i].r_offset);
            return -1;
        }
        where = (uint64_t *)((char *)m->load_base + r[i].r_offset);

        switch (type) {
        case R_AARCH64_RELATIVE_:
            if (!resolve)
                *where = (uint64_t)(uintptr_t)m->load_virtbase + (uint64_t)r[i].r_addend;
            break;
        case R_AARCH64_ABS64_:
        case R_AARCH64_GLOB_DAT_:
        case R_AARCH64_JUMP_SLOT_:
            if (!sym) {
                if (!resolve)
                    *where = (uint64_t)r[i].r_addend;
                break;
            }
            if (sym->st_shndx != 0) {
                if (!resolve)
                    *where = (uint64_t)(uintptr_t)m->load_virtbase + sym->st_value + (uint64_t)r[i].r_addend;
            } else if (resolve) {
                const char *name = strs + sym->st_name;
                uintptr_t addr = resolver(name, m);
                if (addr) {
                    *where = (uint64_t)addr + (uint64_t)r[i].r_addend;
                } else if ((sym->st_info >> 4) == STB_WEAK_) {
                    *where = 0;
                } else {
                    (*missing)++;
                    if (missing_buf && !strstr(missing_buf, name)) {
                        size_t len = strlen(missing_buf);
                        snprintf(missing_buf + len, missing_len - len, "%s%s", len ? ", " : "", name);
                    }
                }
            }
            break;
        default:
            if (!resolve) {
                LOGE("%s: unsupported relocation type %u", m->name, type);
                return -1;
            }
            break;
        }
    }
    return 0;
}

int so_relocate(so_module *mod)
{
    if (mod->dt_rela && reloc_range(mod, mod->dt_rela, mod->dt_relasz, 0, NULL, NULL, NULL, 0))
        return -1;
    if (mod->dt_jmprel && reloc_range(mod, mod->dt_jmprel, mod->dt_pltrelsz, 0, NULL, NULL, NULL, 0))
        return -1;
    return 0;
}

int so_resolve(so_module *mod, so_resolver_fn resolver, char *missing, size_t missing_len)
{
    int n = 0;
    if (missing && missing_len)
        missing[0] = '\0';
    if (mod->dt_rela)
        reloc_range(mod, mod->dt_rela, mod->dt_relasz, 1, resolver, &n, missing, missing_len);
    if (mod->dt_jmprel)
        reloc_range(mod, mod->dt_jmprel, mod->dt_pltrelsz, 1, resolver, &n, missing, missing_len);
    return n;
}

int so_finalize(so_module *mod)
{
    const size_t npages = mod->load_size / 0x1000;
    unsigned char *is_x;
    Result rc;
    int want_x, i;

    armDCacheFlush(mod->load_base, mod->load_size);
    rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)(uintptr_t)mod->load_virtbase,
                                 (u64)(uintptr_t)mod->load_base, mod->load_size);
    if (R_FAILED(rc)) {
        LOGE("%s: svcMapProcessCodeMemory failed 0x%x", mod->name, rc);
        return -1;
    }
    /* From here the staging buffer belongs to the kernel. */
    mod->finalized = 1;

    is_x = calloc(npages, 1);
    if (!is_x)
        return -1;
    for (i = 0; i < mod->phnum; i++) {
        const so_phdr *p = &mod->phdr[i];
        size_t first, last, pg;
        if (p->p_type != PT_LOAD_ || !(p->p_flags & PF_X_))
            continue;
        first = (size_t)(p->p_vaddr / 0x1000);
        last = (size_t)(ALIGN_UP(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000);
        for (pg = first; pg < last && pg < npages; pg++)
            is_x[pg] = 1;
    }

    /* A page covered by both an executable and a writable segment cannot get
     * both permissions. Android's 64 KB segment alignment makes this
     * impossible for the shipped libraries, but say so rather than fault
     * mysteriously if a future build differs. */
    for (i = 0; i < mod->phnum; i++) {
        const so_phdr *p = &mod->phdr[i];
        size_t pg;
        if (p->p_type != PT_LOAD_ || (p->p_flags & PF_X_))
            continue;
        for (pg = (size_t)(p->p_vaddr / 0x1000);
             pg < (size_t)(ALIGN_UP(p->p_vaddr + p->p_memsz, 0x1000) / 0x1000) && pg < npages; pg++)
            if (is_x[pg])
                LOG_ONCE("%s: page %zu is in both an executable and a writable segment; "
                         "it will be mapped read-execute", mod->name, pg);
    }

    /* Code memory arrives with no access. The kernel allows exactly one
     * transition per page, so executable runs go straight to RX and
     * everything else (data and inter-segment gaps) straight to RW. */
    for (want_x = 1; want_x >= 0; want_x--) {
        size_t pg = 0;
        while (pg < npages) {
            size_t end;
            if (is_x[pg] != want_x) {
                pg++;
                continue;
            }
            end = pg;
            while (end < npages && is_x[end] == want_x)
                end++;
            rc = svcSetProcessMemoryPermission(envGetOwnProcessHandle(),
                                               (u64)(uintptr_t)mod->load_virtbase + pg * 0x1000,
                                               (end - pg) * 0x1000, want_x ? Perm_Rx : Perm_Rw);
            if (R_FAILED(rc)) {
                LOGE("%s: permission %s on pages %zu..%zu failed 0x%x",
                     mod->name, want_x ? "RX" : "RW", pg, end, rc);
                free(is_x);
                return -1;
            }
            pg = end;
        }
    }
    free(is_x);
    armICacheInvalidate(mod->load_virtbase, mod->load_size);
    LOGI("so_finalize: %s mapped at %p-%p", mod->name, mod->load_virtbase,
         (void *)((uintptr_t)mod->load_virtbase + mod->load_size));
    return 0;
}

void so_run_init_array(so_module *mod)
{
    size_t i, n;
    uintptr_t *arr;
    if (!mod->dt_init_array || !mod->dt_init_arraysz)
        return;
    arr = (uintptr_t *)mod_ptr(mod, mod->dt_init_array);
    n = (size_t)(mod->dt_init_arraysz / sizeof(uintptr_t));
    LOGI("%s: running %zu constructors", mod->name, n);
    for (i = 0; i < n; i++) {
        if (arr[i] != 0 && arr[i] != (uintptr_t)-1)
            ((void (*)(void))arr[i])();
    }
}

static unsigned long elf_hash(const char *name)
{
    unsigned long h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000ul;
        if (g)
            h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

uintptr_t so_symbol(so_module *mod, const char *name)
{
    const uint32_t *hash;
    uint32_t nbucket, i;
    const so_sym *syms;
    const char *strs;

    if (!mod || !name || !mod->dt_hash)
        return 0;
    hash = (const uint32_t *)mod_ptr(mod, mod->dt_hash);
    nbucket = hash[0];
    if (!nbucket)
        return 0;
    syms = symtab(mod);
    strs = strtab(mod);
    for (i = hash[2 + elf_hash(name) % nbucket]; i != 0; i = hash[2 + nbucket + i]) {
        const so_sym *s = &syms[i];
        if (i >= mod->nsyms)
            break;
        if (s->st_shndx == 0 || (s->st_info >> 4) == STB_LOCAL_)
            continue;
        if (strcmp(strs + s->st_name, name) == 0)
            return (uintptr_t)mod->load_virtbase + (uintptr_t)s->st_value;
    }
    return 0;
}

void so_foreach_symbol(so_module *mod, so_sym_cb cb, void *ud)
{
    const so_sym *syms = symtab(mod);
    const char *strs = strtab(mod);
    uint32_t i;
    for (i = 1; i < mod->nsyms; i++) {
        const so_sym *s = &syms[i];
        if (s->st_shndx == 0 || !s->st_value)
            continue;
        if (cb(strs + s->st_name, (uintptr_t)mod->load_virtbase + (uintptr_t)s->st_value,
               s->st_size, s->st_info & 0xF, ud))
            break;
    }
}

so_module *so_module_by_addr(uintptr_t addr)
{
    so_module *m;
    for (m = g_modules; m; m = m->next) {
        uintptr_t b = (uintptr_t)m->load_virtbase;
        if (addr >= b && addr < b + m->load_size)
            return m;
    }
    return NULL;
}

so_module *so_module_by_name(const char *shortname)
{
    so_module *m;
    for (m = g_modules; m; m = m->next)
        if (!strcmp(m->name, shortname))
            return m;
    return NULL;
}

int so_symbolize(uintptr_t addr, const char **modname, const char **symname, uintptr_t *offset_in_sym)
{
    so_module *m = so_module_by_addr(addr);
    const so_sym *syms, *best = NULL;
    const char *strs;
    uint64_t off;
    uint32_t i;

    if (!m || !m->finalized)
        return 0;
    *modname = m->name;
    *symname = NULL;
    *offset_in_sym = addr - (uintptr_t)m->load_virtbase;
    syms = symtab(m);
    strs = strtab(m);
    off = addr - (uintptr_t)m->load_virtbase;
    for (i = 1; i < m->nsyms; i++) {
        const so_sym *s = &syms[i];
        if (s->st_shndx == 0 || (s->st_info & 0xF) != STT_FUNC_ || s->st_value > off)
            continue;
        if (!best || s->st_value > best->st_value)
            best = s;
    }
    if (best && off - best->st_value < 0x100000) {
        *symname = strs + best->st_name;
        *offset_in_sym = (uintptr_t)(off - best->st_value);
    }
    return 1;
}

/* bionic's struct dl_phdr_info, including the adds/subs counters libgcc's
 * unwinder uses to validate its FDE cache. Modules never unload, so the
 * counters are constant and the cache stays valid. */
struct bionic_dl_phdr_info {
    uint64_t        dlpi_addr;
    const char     *dlpi_name;
    const so_phdr  *dlpi_phdr;
    uint16_t        dlpi_phnum;
    uint64_t        dlpi_adds;
    uint64_t        dlpi_subs;
    size_t          dlpi_tls_modid;
    void           *dlpi_tls_data;
};

int so_dl_iterate_phdr(int (*cb)(void *info, size_t size, void *data), void *data)
{
    so_module *m;
    int ret = 0;
    for (m = g_modules; m; m = m->next) {
        struct bionic_dl_phdr_info info;
        if (!m->finalized)
            continue;
        memset(&info, 0, sizeof(info));
        info.dlpi_addr = (uint64_t)(uintptr_t)m->load_virtbase;
        info.dlpi_name = m->name;
        info.dlpi_phdr = m->phdr;
        info.dlpi_phnum = (uint16_t)m->phnum;
        info.dlpi_adds = 2;
        info.dlpi_subs = 0;
        ret = cb(&info, sizeof(info), data);
        if (ret)
            break;
    }
    return ret;
}
