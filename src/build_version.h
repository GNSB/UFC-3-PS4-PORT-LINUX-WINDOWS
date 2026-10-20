/* Build identity shared by the game, GPU shim and launcher. */
#ifndef BB_BUILD_VERSION_H
#define BB_BUILD_VERSION_H
#include <stdio.h>
#include <string.h>
#ifndef BB_BUILD_VERSION
#define BB_BUILD_VERSION "unknown"
#endif
#define BB_GAME_VERSION_MARKER "BB_GAME_BUILD_VERSION="
#define BB_VERSION_WARNING "VERSION MISMATCH: components differ or are unknown — extract the release zip into a new, empty folder"
/* Read a NUL-terminated identity from the exe without executing legacy binaries. */
static inline void bb_read_game_version(FILE *f, char *version, size_t n) {
    if (!n) return;
    snprintf(version, n, "unknown");
    const char marker[] = BB_GAME_VERSION_MARKER;
    size_t matched = 0;
    int c;
    while ((c = fgetc(f)) != EOF) {
        if (c == marker[matched]) {
            if (++matched != sizeof(marker) - 1) continue;
            size_t used = 0;
            while ((c = fgetc(f)) != EOF && c && used + 1 < n) {
                if (c < 33 || c > 126) break;
                version[used++] = (char)c;
            }
            if (c == 0 && used) version[used] = 0;
            else snprintf(version, n, "unknown");
            break;
        }
        matched = c == marker[0] ? 1 : 0;
    }
}
static inline int bb_versions_match(const char *a, const char *b) {
    return a && b && a[0] && b[0] && strcmp(a, "unknown") && !strcmp(a, b);
}
static inline void bb_print_versions(FILE *out, const char *game, const char *gpu, const char *platform) {
    fprintf(out, "Version: game %s, GPU library %s (%s)\n", game, gpu, platform);
    if (!bb_versions_match(game, gpu)) fprintf(out, "%s\n", BB_VERSION_WARNING);
}
#endif
