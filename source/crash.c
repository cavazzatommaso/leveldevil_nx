/* crash.c -- turn a fault into a readable report at the end of leveldevil.log.
 *
 * libnx enables user exception handling when the program defines
 * __libnx_exception_handler and an exception stack. When the handler
 * returns, libnx calls svcBreak and the system shows its error screen, so
 * this only has to write the report.
 *
 * Every pointer is checked with svcQueryMemory before it is read: a second
 * fault inside the handler would lose the report. Addresses inside the loaded
 * modules print as "liblime.so!SDL_PollEvent+0x1c (+0x4bb2d8)" -- the part in
 * parentheses is the file offset to look up in a disassembler. Addresses in
 * the port itself print as "host+0x...", which addr2line resolves against
 * leveldevil.elf.
 *
 * MIT licensed, see LICENSE.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "so_util.h"

u8  __nx_exception_stack[0x8000] __attribute__((aligned(16)));
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

extern char _start[];   /* the host image is linked at 0, so this is its base */

static int readable(uintptr_t addr, size_t len)
{
    uintptr_t a = addr, end = addr + len;
    if (addr < 0x1000 || end < addr)
        return 0;
    while (a < end) {
        MemoryInfo mi;
        u32 pi;
        if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || mi.type == MemType_Unmapped ||
            !(mi.perm & Perm_R) || mi.addr + mi.size <= a)
            return 0;
        a = (uintptr_t)(mi.addr + mi.size);
    }
    return 1;
}

static int is_code(uintptr_t addr)
{
    MemoryInfo mi;
    u32 pi;
    if (addr < 0x1000 || R_FAILED(svcQueryMemory(&mi, &pi, addr)))
        return 0;
    return mi.type != MemType_Unmapped && (mi.perm & Perm_X);
}

static uintptr_t host_end(void)
{
    static uintptr_t end;
    if (!end) {
        uintptr_t a = (uintptr_t)_start;
        int i;
        for (i = 0; i < 8; i++) {
            MemoryInfo mi;
            u32 pi;
            if (R_FAILED(svcQueryMemory(&mi, &pi, a)) || mi.type == MemType_Unmapped ||
                mi.addr + mi.size <= a)
                break;
            a = (uintptr_t)(mi.addr + mi.size);
        }
        end = a;
    }
    return end;
}

static const char *where(uintptr_t addr, char *buf, size_t n)
{
    const char *mod, *sym;
    uintptr_t off;
    if (so_symbolize(addr, &mod, &sym, &off)) {
        so_module *m = so_module_by_addr(addr);
        uintptr_t rel = m ? addr - (uintptr_t)m->load_virtbase : 0;
        if (sym)
            snprintf(buf, n, "%s!%s+0x%lx (+0x%lx)", mod, sym, (unsigned long)off, (unsigned long)rel);
        else
            snprintf(buf, n, "%s+0x%lx", mod, (unsigned long)rel);
    } else if (addr >= (uintptr_t)_start && addr < host_end()) {
        snprintf(buf, n, "host+0x%lx", (unsigned long)(addr - (uintptr_t)_start));
    } else {
        snprintf(buf, n, "0x%016lx", (unsigned long)addr);
    }
    return buf;
}

static const char *error_name(u32 code)
{
    switch (code) {
    case 0x100: return "instruction abort (jumped to non-executable memory)";
    case 0x101: return "data abort (bad memory access)";
    case 0x102: return "PC alignment fault";
    case 0x103: return "SP alignment fault";
    case 0x104: return "breakpoint / trap";
    case 0x106: return "SError";
    case 0x301: return "invalid supervisor call";
    default:    return "unknown exception";
    }
}

static void out(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    log_emergency(line);
}

void __libnx_exception_handler(ThreadExceptionDump *ctx);

void __libnx_exception_handler(ThreadExceptionDump *ctx)
{
    char a[200], b[200];
    uintptr_t fp, sp;
    int i;

    out("\n================================================================\n");
    out("CRASH: %s (0x%x), thread handle 0x%x\n", error_name(ctx->error_desc), ctx->error_desc,
        threadGetCurHandle());
    out("  PC  %s\n", where((uintptr_t)ctx->pc.x, a, sizeof(a)));
    out("  LR  %s\n", where((uintptr_t)ctx->lr.x, a, sizeof(a)));
    if (ctx->error_desc == 0x100 || ctx->error_desc == 0x101) {
        u32 ec = (ctx->esr >> 26) & 0x3F;
        out("  FAR 0x%016lx (%s%s)  ESR 0x%08x\n", (unsigned long)ctx->far.x,
            (ec == 0x24 || ec == 0x25) ? ((ctx->esr >> 6) & 1 ? "write" : "read") : "execute",
            ctx->far.x < 0x10000 ? ", near NULL" : "", ctx->esr);
    }
    out("  SP  0x%016lx  FP 0x%016lx\n", (unsigned long)ctx->sp.x, (unsigned long)ctx->fp.x);
    for (i = 0; i < 29; i += 4) {
        int j;
        char row[256];
        size_t len = 0;
        for (j = i; j < i + 4 && j < 29; j++)
            len += (size_t)snprintf(row + len, sizeof(row) - len, " x%-2d %016lx", j,
                                    (unsigned long)ctx->cpu_gprs[j].x);
        out(" %s\n", row);
    }

    out("backtrace (frame pointers):\n");
    fp = (uintptr_t)ctx->fp.x;
    for (i = 0; i < 32 && fp && !(fp & 7) && readable(fp, 16); i++) {
        uintptr_t next = *(uintptr_t *)fp, lr = *(uintptr_t *)(fp + 8);
        if (!lr)
            break;
        out("  #%-2d %s\n", i, where(lr, a, sizeof(a)));
        if (next <= fp)
            break;
        fp = next;
    }

    /* Code built without frame pointers leaves gaps in the chain; return
     * addresses sitting on the stack fill them in. */
    out("code addresses on the stack:\n");
    sp = (uintptr_t)ctx->sp.x & ~(uintptr_t)7;
    for (i = 0; i < 256 && readable(sp + (uintptr_t)i * 8, 8); i++) {
        uintptr_t v = *(uintptr_t *)(sp + (uintptr_t)i * 8);
        if ((so_module_by_addr(v) || (v >= (uintptr_t)_start && v < host_end())) && is_code(v))
            out("  sp+0x%03x %s\n", i * 8, where(v, b, sizeof(b)));
    }
    out("================================================================\n");
}
