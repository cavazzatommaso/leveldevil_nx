/* paths.h -- find the game folder, wherever it is.
 *
 * The Homebrew Menu passes the .nro's own path as argv[0], so the folder the
 * .nro was launched from is where the game is looked for first. Any folder on
 * the SD card works, under any name.
 *
 * Both file layouts are accepted:
 *
 *   <folder>/leveldevil.nro           <folder>/leveldevil.nro
 *   <folder>/liblime.so              <folder>/lib/liblime.so
 *   <folder>/libApplicationMain.so   <folder>/lib/libApplicationMain.so
 *   <folder>/assets/...              <folder>/assets/...
 *
 * and the assets may sit either in <folder>/assets (the APK's assets folder
 * copied in whole) or directly in <folder> (its contents copied in).
 *
 * If the .nro was started some other way, or the game lives elsewhere, a few
 * well-known names are tried and then every folder in sdmc:/switch is checked.
 *
 * save/, config.txt and leveldevil.log are created inside whichever folder is
 * found. The process chdir()s into the assets folder, which is what makes the
 * game's Android-style relative paths ("assets/data/game.mbs") resolve
 * unchanged.
 *
 * Paths handed back to the game have the "sdmc:" device prefix stripped, since
 * Haxe's path helpers split on ':'; newlib resolves such a path against the
 * current device anyway.
 *
 * MIT licensed, see LICENSE.
 */
#ifndef HS_PATHS_H
#define HS_PATHS_H

#include <stddef.h>

#define HS_PACKAGE "com.unept.leveldevil"

/* Locate the game. Returns 1 on success; on failure fills err with the list of
 * places that were searched. Call before log_init(): nothing is logged yet. */
int paths_locate(int argc, char **argv, char *err, size_t errlen);

/* Create save/ and chdir into the assets folder. */
int paths_init(char *err, size_t errlen);

const char *paths_root(void);          /* sdmc:/switch/leveldevil_nx           */
const char *paths_assets(void);        /* <root>/assets, or <root>            */
const char *paths_assets_nodev(void);  /* /switch/leveldevil_nx/assets         */
const char *paths_save(void);          /* <root>/save                         */
const char *paths_save_nodev(void);    /* /switch/leveldevil_nx/save           */
const char *paths_lib_lime(void);
const char *paths_lib_app(void);
const char *paths_config(void);
const char *paths_log(void);
const char *paths_prefs(void);

/* Rewrite a path coming from the game: Android storage locations map into
 * save/, "file://" is stripped, and "//", "/./" and "dir/../" are collapsed.
 * Always NUL-terminates; returns out. */
const char *path_translate(const char *in, char *out, size_t outlen);

#endif
