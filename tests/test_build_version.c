#include "build_version.h"
#include <assert.h>

static void check_log(const char *game, const char *gpu, int mismatch) {
    FILE *out = tmpfile();
    assert(out);
    bb_print_versions(out, game, gpu, "test");
    rewind(out);
    char text[1024];
    size_t n = fread(text, 1, sizeof(text) - 1, out);
    text[n] = 0;
    fclose(out);
    assert(strstr(text, "Version: game "));
    assert(!!strstr(text, "VERSION MISMATCH:") == mismatch);
    if (mismatch) assert(strstr(text, "extract the release zip into a new, empty folder"));
}
static void check_identity(const char *data, size_t size, const char *expected) {
    FILE *f = tmpfile();
    assert(f);
    assert(fwrite(data, 1, size, f) == size);
    rewind(f);
    char version[256];
    bb_read_game_version(f, version, sizeof(version));
    fclose(f);
    assert(!strcmp(version, expected));
}
int main(void) {
    const char identity[] = "MZ..." BB_GAME_VERSION_MARKER "ufc3-v0.1.8-dirty";
    check_identity(identity, sizeof(identity), "ufc3-v0.1.8-dirty");
    check_identity(identity, sizeof(identity) - 1, "unknown");
    check_identity("MZ legacy executable", 20, "unknown");

    check_log("ufc3-v0.1.8-1-g7ded85e", "ufc3-v0.1.8-1-g7ded85e", 0);
    check_log("ufc3-v0.1.8", "ufc3-v0.1.7", 1);
    check_log("7ded85e-dirty", "7ded85e-dirty", 0);
    check_log("unknown", "unknown", 1);
    assert(!bb_versions_match("", ""));
    assert(!bb_versions_match(NULL, "ufc3-v0.1.8"));
    puts("Build version comparison and warning: OK");
    return 0;
}
