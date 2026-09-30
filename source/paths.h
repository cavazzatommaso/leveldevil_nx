/* paths.h -- find the game folder, wherever it is.
 *
 * The .nro's own folder is tried first (argv[0] from the Homebrew Menu), then
 * a few well-known names, then every folder in sdmc:/switch. A folder holds
 * the game when it has libLevelDevil.so (loose or in lib/) and the APK's
 * assets -- game.projectc, game.arcd, ... -- in assets/ or loose.
 *
 * save/, config.txt and leveldevil.log live in that folder. Paths handed to
 * the game have the "sdmc:" prefix stripped.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef PB_PATHS_H
#define PB_PATHS_H

#include <stddef.h>

#define PB_PACKAGE "com.unept.leveldevil"

/* Locate the game. Returns 1 on success; on failure fills err with the list of
 * places that were searched. Call before log_init(): nothing is logged yet. */
int paths_locate(int argc, char **argv, char *err, size_t errlen);

/* Create save/ and chdir into the assets folder. */
int paths_init(char *err, size_t errlen);

const char *paths_root(void);          /* sdmc:/switch/leveldevil_nx      */
const char *paths_assets(void);        /* <root>/assets, or <root>            */
const char *paths_assets_nodev(void);  /* /switch/leveldevil_nx/assets    */
const char *paths_save(void);          /* <root>/save                         */
const char *paths_save_nodev(void);    /* /switch/leveldevil_nx/save      */
const char *paths_lib(void);
const char *paths_config(void);
const char *paths_log(void);

/* Rewrite a path coming from the game: Android storage locations map into
 * save/, "file://" is stripped, and "//", "/./" and "dir/../" are collapsed.
 * Always NUL-terminates; returns out. */
const char *path_translate(const char *in, char *out, size_t outlen);

#endif
