/* assetmgr.h -- see assetmgr.c. MIT licensed, see LICENSE. */
#ifndef PB_ASSETMGR_H
#define PB_ASSETMGR_H
/* The asset API is reached only through the import table; nothing in the port
 * calls it directly. This header exists so android_stubs.h can declare the
 * entry points in one place and any signature drift is a compile error. */
#endif
