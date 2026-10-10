/* Host check for arcpatch.c against a real Level Devil 1.4.1 assets folder.
 *   cc -I../source arcpatch_test.c ../source/arcpatch.c -o t && ./t <assets> <outdir>
 * Writes the patched game.arci/game.arcd to <outdir> for a second decoder. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "arcpatch.h"

static uint8_t *slurp(const char *dir, const char *name, size_t *len)
{
    char p[1024];
    FILE *f;
    uint8_t *b;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc(*len);
    assert(b && fread(b, 1, *len, f) == *len);
    fclose(f);
    return b;
}

static void spit(const char *dir, const char *name, const uint8_t *b, size_t len)
{
    char p[1024];
    FILE *f;
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "wb");
    assert(f && fwrite(b, 1, len, f) == len);
    fclose(f);
}

int main(int argc, char **argv)
{
    size_t il, dl;
    uint8_t *arci, *arcd;
    ArcPatch p;
    char why[128] = "";

    if (argc < 3 || !(arci = slurp(argv[1], "game.arci", &il)) || !(arcd = slurp(argv[1], "game.arcd", &dl))) {
        puts("arcpatch: skipped (no game files)");
        return 0;
    }
    assert(arcpatch_hide_touch_buttons(arci, il, arcd, dl, &p, why, sizeof(why)) || !puts(why));
    assert(p.entries == 2 && p.constants == 9);       /* 3 + 6 arrow images */
    assert(p.arci_len == il && p.arcd_len > dl);
    assert(!memcmp(p.arcd, arcd, dl));                 /* original data untouched */

    {   /* garbage in: refuses, changes nothing */
        ArcPatch q;
        uint8_t bad[64] = { 0 };
        assert(!arcpatch_hide_touch_buttons(bad, sizeof(bad), arcd, dl, &q, why, sizeof(why)));
    }
    spit(argv[2], "game.arci", p.arci, p.arci_len);
    spit(argv[2], "game.arcd", p.arcd, p.arcd_len);
    printf("arcpatch: ok (%d scenes, %d arrows hidden, +%zu bytes)\n", p.entries, p.constants, p.arcd_len - dl);
    return 0;
}
