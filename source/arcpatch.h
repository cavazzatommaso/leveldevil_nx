/* arcpatch.h -- see arcpatch.c. MIT licensed, see LICENSE. */
#ifndef LD_ARCPATCH_H
#define LD_ARCPATCH_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *arci, *arcd;          /* malloc'd; owned by the caller */
    size_t   arci_len, arcd_len;
    int      entries, constants;   /* scenes patched, arrow images hidden */
} ArcPatch;

/* 1 and a filled *out on success; 0 and a reason in why otherwise (the
 * caller then serves the archive unchanged). */
int arcpatch_hide_touch_buttons(const uint8_t *arci, size_t arci_len,
                                const uint8_t *arcd, size_t arcd_len, ArcPatch *out,
                                char *why, size_t whylen);

#endif
