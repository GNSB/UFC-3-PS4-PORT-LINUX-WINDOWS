/* UFC-82: local trophies (runtime_trophy.c); runtime_services.c holds the sceNpTrophy entries. */
#ifndef RUNTIME_TROPHY_H
#define RUNTIME_TROPHY_H
#include <stddef.h>
#include <stdint.h>

#define TRP_MAGIC 0xDCA24D00u
#define TROPHY_MAX 128 /* ORBIS_NP_TROPHY_NUM_MAX */

typedef struct {
    char name[33];
    uint64_t offset, length;
    uint32_t flag; /* 0 PNG, 3 encrypted ESFM */
} TrpEntry;

/* Entries of a .trp held in memory (at most max); -1 when it is not one or is truncated. */
int runtime_trp_parse(const uint8_t *data, size_t size, TrpEntry *entries, int max);
const TrpEntry *runtime_trp_find(const TrpEntry *entries, int count, const char *name);

/* trophies.json: unlock times in unix seconds (0 = locked) by id. load returns the count. */
int runtime_trophy_load_json(const char *path, int64_t *times, int max);
int runtime_trophy_save_json(const char *path, const int64_t *times, int max);
/* Trophies and names from a decrypted TROP.SFM; the highest id + 1. */
int runtime_trophy_parse_sfm(const char *xml);

/* Reads the .trp, the key (ini trophy_key=, else keys.json) and the saved unlocks. Any path may be
 * missing: unlocks are still saved. */
void runtime_trophy_configure(const char *trp_path, const char *npbind_path, const char *json_path,
                              const char *ini_path, const char *keys_json_path);
/* 1 newly unlocked (saved, toast shown), 0 already unlocked, -1 bad id. *platinum: the platinum
 * unlocked with it, else -1. */
int runtime_trophy_unlock(int id, int *platinum);
int runtime_trophy_is_unlocked(int id);
/* Fill the game's OrbisNpTrophyGameDetails/GameData and TrophyDetails/TrophyData (may be NULL). */
void runtime_trophy_game_info(void *details, void *data);
int runtime_trophy_info(int id, void *details, void *data);
#endif
