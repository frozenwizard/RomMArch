/* Harness for RomM save sync (network/romm/romm_saves.c): the record
 * of what is in sync, its round trip through JSON, RomM's times, which
 * files in a folder are a game's saves or save states, and their MD5.
 * Then what a sync does with each file, given the files here, RomM's
 * and the record: fetch, send, record a match or report a conflict.
 * Last, syncs run against RomM played through the client harness's
 * request stubs: fetching before a game starts, sending after it
 * closes, a conflict, a failed listing, a file in use, and syncs
 * queued behind one another; then conflicts resolved each way, the
 * copy not chosen kept in RomM.
 *
 * Includes the module's translation unit so its statics are tested as
 * shipped. Files go to a folder under the current directory, removed
 * at the end.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <file/file_path.h>

#include "../../../network/romm/romm_saves.c"

#include "../romm_client/stubs_retroarch.h"

#define DIR "romm_saves_test_dir"

static unsigned failures = 0;

#define CHECK(cond, ...) \
   do { \
      if (!(cond)) \
      { \
         printf("FAIL %s:%d: ", __FILE__, __LINE__); \
         printf(__VA_ARGS__); \
         printf("\n"); \
         failures++; \
      } \
   } while (0)

static void write_file(const char *name, const char *data)
{
   char path[256];
   fill_pathname_join(path, DIR, name, sizeof(path));
   filestream_write_file(path, data, (int64_t)strlen(data));
}

static void remove_file(const char *name)
{
   char path[256];
   fill_pathname_join(path, DIR, name, sizeof(path));
   filestream_delete(path);
}

static void test_time(void)
{
   CHECK(romm_json_time("2026-10-13T02:18:25+00:00") == 1791857905,
         "list time %lld",
         (long long)romm_json_time("2026-10-13T02:18:25+00:00"));
   /* An upload's reply carries microseconds; the same second. */
   CHECK(romm_json_time("2026-10-13T02:18:25.590502+00:00") == 1791857905,
         "fraction dropped");
   CHECK(romm_json_time("2026-10-13T02:18:25Z") == 1791857905, "Z");
   CHECK(romm_json_time("2026-10-13T04:18:25+02:00") == 1791857905
         && romm_json_time("2026-10-12T21:18:25-05:00") == 1791857905,
         "offsets");
   CHECK(romm_json_time("1970-01-01T00:00:00Z") == 0
         && romm_json_time("2024-02-29T12:00:00Z") == 1709208000
         && romm_json_time("2000-03-01T00:00:00Z") == 951868800,
         "calendar");
   CHECK(romm_json_time("2026-10-13") == -1
         && romm_json_time("2026-13-13T02:18:25Z") == -1
         && romm_json_time("2026-10-13T02:18:25 junk") == -1
         && romm_json_time("") == -1 && romm_json_time(NULL) == -1,
         "not times");
}

static void test_suffix(void)
{
   const char *base = "Super Metroid (USA)";
   const char *s;

   CHECK((s = romm_saves_suffix("Super Metroid (USA).srm", base))
         && !strcmp(s, ".srm"), "srm %s", s ? s : "NULL");
   CHECK((s = romm_saves_suffix("Super Metroid (USA).state.auto", base))
         && !strcmp(s, ".state.auto"), "auto state");
   CHECK((s = romm_saves_suffix("Super Metroid (USA).A1.bin", base))
         && !strcmp(s, ".A1.bin"), "memory card");
   CHECK((s = romm_saves_suffix("super metroid (usa).SRM", base))
         && !strcmp(s, ".SRM"), "case");
   CHECK(!romm_saves_suffix("Super Metroid (USA).state1.png", base)
         && !romm_saves_suffix("Super Metroid (USA).srm.tmp", base),
         "screenshot or temporary file taken");
   /* Another game whose name starts with this one's. */
   CHECK(!romm_saves_suffix("Super Metroid (USA). Redesign.srm", base)
         && !romm_saves_suffix("Super Metroid (USA) (Rev 1).srm", base),
         "another game's file taken");
   CHECK(!romm_saves_suffix("Super Metroid (USA)", base)
         && !romm_saves_suffix("Super Metroid (USA).", base)
         && !romm_saves_suffix("Super Metroid (USA).a.b.c.d", base)
         && !romm_saves_suffix("Other.srm", base)
         && !romm_saves_suffix("Super Metroid (USA).srm", ""),
         "not a suffix");
}

#define MD5_CACHED "c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0c0"

static bool has_suffix(const romm_saves_local_t *list, const char *suffix,
      const char *md5)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(list); i++)
      if (     !strcmp(list[i].suffix, suffix)
            && (!md5 || !strcmp(list[i].md5, md5)))
         return true;
   return false;
}

static void test_local(void)
{
   romm_saves_local_t *saves, *states;
   char md5[33];
   char path[256];

   path_mkdir(DIR);
   write_file("abc", "abc");
   fill_pathname_join(path, DIR, "abc", sizeof(path));
   CHECK(romm_saves_md5_file(path, md5)
         && !strcmp(md5, "900150983cd24fb0d6963f7d28e17f72"), "md5 %s", md5);
   CHECK(!romm_saves_md5_file(DIR "/missing", md5), "md5 of nothing");
   remove_file("abc");

   write_file("Game.srm", "abc");
   write_file("Game.rtc", "");
   write_file("Game.state", "s0");
   write_file("Game.state1", "s1");
   write_file("Game.state1.png", "png");
   write_file("Game.state.auto", "auto");
   write_file("Game. Bros.srm", "other game");
   write_file("Other.srm", "other");
   write_file("Game.srm.tmp", "tmp");
   fill_pathname_join(path, DIR, "Game.d", sizeof(path));
   path_mkdir(path);

   saves  = romm_saves_list_local(DIR, "Game", false, NULL);
   states = romm_saves_list_local(DIR, "Game", true, NULL);
   CHECK(RBUF_LEN(saves) == 2
         && has_suffix(saves, ".srm", "900150983cd24fb0d6963f7d28e17f72")
         && has_suffix(saves, ".rtc", "d41d8cd98f00b204e9800998ecf8427e"),
         "saves: %u", (unsigned)RBUF_LEN(saves));
   CHECK(RBUF_LEN(states) == 3 && has_suffix(states, ".state", NULL)
         && has_suffix(states, ".state1", NULL)
         && has_suffix(states, ".state.auto", NULL),
         "states: %u", (unsigned)RBUF_LEN(states));
   RBUF_FREE(saves);
   RBUF_FREE(states);

   /* A file as recorded isn't read: its MD5 is the record's. */
   {
      romm_saves_game_t game;
      romm_saves_file_t *f;
      int64_t mtime = 0;
      memset(&game, 0, sizeof(game));
      fill_pathname_join(path, DIR, "Game.srm", sizeof(path));
      path_get_mtime(path, &mtime);
      f = romm_saves_game_file(&game, ".srm", true);
      strlcpy(f->md5, MD5_CACHED, sizeof(f->md5));
      f->size  = 3;
      f->mtime = mtime;
      f = romm_saves_game_file(&game, ".rtc", true);
      strlcpy(f->md5, MD5_CACHED, sizeof(f->md5));
      f->size  = 0;
      f->mtime = mtime - 1;
      saves = romm_saves_list_local(DIR, "Game", false, &game);
      CHECK(RBUF_LEN(saves) == 2 && has_suffix(saves, ".srm", MD5_CACHED)
            && has_suffix(saves, ".rtc", "d41d8cd98f00b204e9800998ecf8427e"),
            "cached MD5 not used, or used for a file changed since");
      CHECK(saves && saves[0].size == (saves[0].suffix[1] == 's' ? 3 : 0)
            && saves[0].mtime == mtime, "size %lld mtime %lld",
            saves ? (long long)saves[0].size : -1LL,
            saves ? (long long)saves[0].mtime : -1LL);
      RBUF_FREE(saves);
      /* Same time, another size. */
      f = romm_saves_game_file(&game, ".srm", false);
      f->size = 4;
      saves = romm_saves_list_local(DIR, "Game", false, &game);
      CHECK(has_suffix(saves, ".srm", "900150983cd24fb0d6963f7d28e17f72"),
            "cached MD5 used for another size");
      RBUF_FREE(saves);
      RBUF_FREE(game.files);
   }
   CHECK(!romm_saves_list_local(DIR "/none", "Game", false, NULL)
         && !romm_saves_list_local(DIR, "", false, NULL), "listed nothing");

   remove_file("Game.srm");
   remove_file("Game.rtc");
   remove_file("Game.state");
   remove_file("Game.state1");
   remove_file("Game.state1.png");
   remove_file("Game.state.auto");
   remove_file("Game. Bros.srm");
   remove_file("Other.srm");
   remove_file("Game.srm.tmp");
   fill_pathname_join(path, DIR, "Game.d", sizeof(path));
   filestream_delete(path);
}

static void test_record(void)
{
   romm_saves_record_t rec, back;
   romm_saves_game_t *g;
   romm_saves_file_t *f;
   char path[256];

   romm_saves_record_path(path, sizeof(path), "/x/saves");
   CHECK(!strcmp(path, "/x/saves/rommarch-saves.json"), "path %s", path);

   memset(&rec, 0, sizeof(rec));
   CHECK(!romm_saves_record_game(&rec, 3335, "Genesis Plus GX", false),
         "found a game in an empty record");
   g = romm_saves_record_game(&rec, 3335, "Genesis Plus GX", true);
   g->pending = true;
   strlcpy(g->save_dir, "/x/saves/Genesis Plus GX", sizeof(g->save_dir));
   strlcpy(g->state_dir, "/x/states/Genesis Plus GX", sizeof(g->state_dir));
   strlcpy(g->base, "Final Fight CD (USA)", sizeof(g->base));
   f = romm_saves_game_file(g, ".brm", true);
   f->remote_id   = 2;
   f->remote_time = 1791857916;
   strlcpy(f->md5, "bc8177cd3eda0a4750c6e17b57eaa076", sizeof(f->md5));
   f->size        = 8192;
   f->mtime       = 1791857000;
   f = romm_saves_game_file(g, ".state1", true);
   f->remote_id   = 1;
   f->remote_time = 1791857928;
   CHECK(romm_saves_game_file(g, ".brm", false)->remote_id == 2
         && !romm_saves_game_file(g, ".srm", false), "file lookup");
   /* The same game with another emulator is another entry; a quote in
    * a name survives the round trip. */
   g = romm_saves_record_game(&rec, 3335, "PicoDrive", true);
   strlcpy(g->base, "Say \"Hi\"", sizeof(g->base));
   g = romm_saves_record_game(&rec, 12, "Snes9x", true);
   CHECK(RBUF_LEN(rec.games) == 3
         && romm_saves_record_game(&rec, 3335, "Genesis Plus GX", true)
            == &rec.games[0], "games: %u", (unsigned)RBUF_LEN(rec.games));

   path_mkdir(DIR);
   fill_pathname_join(path, DIR, "rommarch-saves.json", sizeof(path));
   CHECK(romm_saves_record_save(&rec, path), "record not written");
   CHECK(romm_saves_record_load(&back, path), "record not read");
   CHECK(RBUF_LEN(back.games) == 3, "games read: %u",
         (unsigned)RBUF_LEN(back.games));
   if (RBUF_LEN(back.games) == 3)
   {
      g = &back.games[0];
      CHECK(g->rom_id == 3335 && !strcmp(g->emulator, "Genesis Plus GX")
            && g->pending
            && !strcmp(g->save_dir, "/x/saves/Genesis Plus GX")
            && !strcmp(g->state_dir, "/x/states/Genesis Plus GX")
            && !strcmp(g->base, "Final Fight CD (USA)")
            && RBUF_LEN(g->files) == 2, "game: %u %s files %u", g->rom_id,
            g->emulator, (unsigned)RBUF_LEN(g->files));
      f = romm_saves_game_file(g, ".brm", false);
      CHECK(f && f->remote_id == 2 && f->remote_time == 1791857916
            && !strcmp(f->md5, "bc8177cd3eda0a4750c6e17b57eaa076")
            && f->size == 8192 && f->mtime == 1791857000,
            "brm read back");
      f = romm_saves_game_file(g, ".state1", false);
      CHECK(f && f->remote_id == 1 && !*f->md5, "state read back");
      CHECK(!back.games[1].pending && !RBUF_LEN(back.games[1].files)
            && !strcmp(back.games[1].emulator, "PicoDrive")
            && !strcmp(back.games[1].base, "Say \"Hi\"")
            && back.games[2].rom_id == 12, "other games");
   }
   romm_saves_record_free(&back);
   romm_saves_record_free(&rec);

   /* Unknown versions and broken files read as no record. */
   write_file("rommarch-saves.json", "{\"version\": 2, \"games\": [],"
         " \"files\": []}");
   CHECK(!romm_saves_record_load(&back, path) && !back.games,
         "read another version");
   write_file("rommarch-saves.json", "{\"version\": 1, \"games\": [");
   CHECK(!romm_saves_record_load(&back, path) && !back.games,
         "read a cut-off file");
   /* A file of a game not listed is dropped. */
   write_file("rommarch-saves.json", "{\"version\": 1, \"games\": ["
         "{\"rom_id\": 5, \"emulator\": \"mGBA\"}], \"files\": ["
         "{\"rom_id\": 6, \"emulator\": \"mGBA\", \"suffix\": \".srm\"},"
         "{\"rom_id\": 5, \"emulator\": \"mGBA\", \"suffix\": \".srm\"}]}");
   CHECK(romm_saves_record_load(&back, path) && RBUF_LEN(back.games) == 1
         && RBUF_LEN(back.games[0].files) == 1, "stray file kept");
   romm_saves_record_free(&back);
   filestream_delete(path);
   CHECK(!romm_saves_record_load(&back, path) && !back.games,
         "read a missing file");
}

static romm_save_t remote(unsigned id, const char *name,
      const char *emulator, const char *hash, const char *updated_at)
{
   romm_save_t r;
   memset(&r, 0, sizeof(r));
   r.id     = id;
   r.rom_id = 3335;
   strlcpy(r.file_name, name, sizeof(r.file_name));
   strlcpy(r.emulator, emulator, sizeof(r.emulator));
   strlcpy(r.content_hash, hash, sizeof(r.content_hash));
   strlcpy(r.updated_at, updated_at, sizeof(r.updated_at));
   return r;
}

static romm_saves_local_t local(const char *suffix, const char *md5)
{
   romm_saves_local_t l;
   memset(&l, 0, sizeof(l));
   strlcpy(l.suffix, suffix, sizeof(l.suffix));
   strlcpy(l.md5, md5, sizeof(l.md5));
   return l;
}

#define MD5_A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define MD5_B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define MD5_C "cccccccccccccccccccccccccccccccc"
#define T1 "2026-10-13T02:00:00+00:00"
#define T2 "2026-10-13T03:00:00+00:00"
#define T3 "2026-10-13T04:00:00.250000+00:00"

/* Plans with the given files; @game may be NULL. */
static romm_saves_step_t *plan(const romm_saves_game_t *game,
      const romm_saves_local_t *saves, size_t saves_count,
      const romm_saves_local_t *states, size_t states_count,
      const romm_save_t *rsaves, size_t rsaves_count,
      const romm_save_t *rstates, size_t rstates_count)
{
   romm_saves_plan_params_t p;
   memset(&p, 0, sizeof(p));
   p.game                = game;
   p.emulator            = "Snes9x";
   p.base                = "Super Metroid (USA)";
   p.rom_base            = "Super Metroid (U)";
   p.saves               = saves;
   p.saves_count         = saves_count;
   p.states              = states;
   p.states_count        = states_count;
   p.remote_saves        = rsaves;
   p.remote_saves_count  = rsaves_count;
   p.remote_states       = rstates;
   p.remote_states_count = rstates_count;
   return romm_saves_plan(&p);
}

static bool step_is(const romm_saves_step_t *steps, size_t i,
      enum romm_saves_step_type type, const char *suffix,
      unsigned remote_id)
{
   return i < RBUF_LEN(steps) && steps[i].type == type
      && !strcmp(steps[i].suffix, suffix)
      && steps[i].remote.id == remote_id;
}

static void test_plan(void)
{
   romm_saves_record_t rec;
   romm_saves_game_t *g;
   romm_saves_file_t *f;
   romm_saves_step_t *st;
   romm_saves_local_t l[2];
   romm_save_t r[4];

   memset(&rec, 0, sizeof(rec));

   /* First sync: only here, only in RomM, the same, different. */
   l[0] = local(".srm", MD5_A);
   st   = plan(NULL, l, 1, NULL, 0, NULL, 0, NULL, 0);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_UPLOAD, ".srm", 0)
         && !strcmp(st[0].md5, MD5_A) && !st[0].is_state,
         "only here: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   /* The web player's save: named after RomM's file, slot time added. */
   r[0] = remote(21, "Super Metroid (U) [2026-10-12_21-00-00].srm",
         "snes9x", MD5_B, T1);
   st   = plan(NULL, NULL, 0, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_DOWNLOAD, ".srm", 21)
         && !*st[0].md5, "only in RomM: %u steps %s",
         (unsigned)RBUF_LEN(st), RBUF_LEN(st) ? st[0].suffix : "");
   RBUF_FREE(st);
   r[0] = remote(21, "Super Metroid (USA).srm", "Snes9x", MD5_A, T1);
   st   = plan(NULL, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_MATCH, ".srm", 21),
         "same: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   r[0] = remote(21, "Super Metroid (USA).srm", "Snes9x", MD5_B, T1);
   st   = plan(NULL, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1
         && step_is(st, 0, ROMM_SAVES_CONFLICT, ".srm", 21),
         "different: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);

   /* In sync: version 21 at T1 holding MD5_A. */
   g = romm_saves_record_game(&rec, 3335, "Snes9x", true);
   f = romm_saves_game_file(g, ".srm", true);
   f->remote_id   = 21;
   f->remote_time = romm_json_time(T1);
   strlcpy(f->md5, MD5_A, sizeof(f->md5));
   /* RomM's hash may be unknown; the record still says it's in sync. */
   r[0] = remote(21, "Super Metroid (USA) [2026-10-12_21-00-00].srm",
         "Snes9x", "", T1);
   st   = plan(g, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(!RBUF_LEN(st), "in sync: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   /* Played here since. */
   l[0] = local(".srm", MD5_C);
   st   = plan(g, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_UPLOAD, ".srm", 21),
         "played here: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   /* Played elsewhere since: a newer version in the slot, an older one
    * and a later one from a slot time earlier in the day. */
   l[0] = local(".srm", MD5_A);
   r[1] = remote(30, "Super Metroid (U) [2026-10-12_23-00-00].srm",
         "snes9x", MD5_B, T3);
   r[2] = remote(25, "Super Metroid (U) [2026-10-12_22-00-00].srm",
         "snes9x", MD5_C, T2);
   st   = plan(g, l, 1, NULL, 0, r, 3, NULL, 0);
   CHECK(RBUF_LEN(st) == 1
         && step_is(st, 0, ROMM_SAVES_DOWNLOAD, ".srm", 30),
         "played elsewhere: %u steps, remote %u", (unsigned)RBUF_LEN(st),
         RBUF_LEN(st) ? st[0].remote.id : 0);
   RBUF_FREE(st);
   /* The web player updates its save in place: same id, later time. */
   r[0] = remote(21, "Super Metroid (USA).srm", "snes9x", MD5_B, T2);
   st   = plan(g, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1
         && step_is(st, 0, ROMM_SAVES_DOWNLOAD, ".srm", 21),
         "updated in place: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   /* Both. */
   l[0] = local(".srm", MD5_C);
   st   = plan(g, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1
         && step_is(st, 0, ROMM_SAVES_CONFLICT, ".srm", 21),
         "both: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   /* Both ended up the same: just recorded. */
   r[0] = remote(31, "Super Metroid (USA).srm", "Snes9x", MD5_C, T2);
   st   = plan(g, l, 1, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_MATCH, ".srm", 31),
         "both the same: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   /* Deleted here: fetched again. Deleted in RomM: left alone, unless
    * played since. */
   r[0] = remote(21, "Super Metroid (USA).srm", "Snes9x", MD5_A, T1);
   st   = plan(g, NULL, 0, NULL, 0, r, 1, NULL, 0);
   CHECK(RBUF_LEN(st) == 1
         && step_is(st, 0, ROMM_SAVES_DOWNLOAD, ".srm", 21),
         "deleted here: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   l[0] = local(".srm", MD5_A);
   st   = plan(g, l, 1, NULL, 0, NULL, 0, NULL, 0);
   CHECK(!RBUF_LEN(st), "deleted in RomM: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   l[0] = local(".srm", MD5_C);
   st   = plan(g, l, 1, NULL, 0, NULL, 0, NULL, 0);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_UPLOAD, ".srm", 0),
         "deleted in RomM, played here: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);

   /* Kinds: .rtc beside .srm; RomM's name lost a quote, matched by the
    * kind known here; an unknown kind by its extension. */
   l[0] = local(".srm", MD5_A);
   l[1] = local(".A1.bin", MD5_A);
   r[0] = remote(21, "Super Metroid (USA).srm", "Snes9x", MD5_A, T1);
   r[1] = remote(40, "Super Metroid USA [2026-10-12_21-00-00].A1.bin",
         "Snes9x", MD5_B, T2);
   r[2] = remote(41, "Super Metroid (USA).rtc", "Snes9x", MD5_B, T2);
   r[3] = remote(42, "Something Else.sav", "mGBA", MD5_B, T2);
   st   = plan(g, l, 2, NULL, 0, r, 4, NULL, 0);
   CHECK(RBUF_LEN(st) == 3
         && step_is(st, 0, ROMM_SAVES_CONFLICT, ".A1.bin", 40)
         && step_is(st, 1, ROMM_SAVES_DOWNLOAD, ".rtc", 41)
         && step_is(st, 2, ROMM_SAVES_DOWNLOAD, ".sav", 42),
         "kinds: %u steps: %s %s %s", (unsigned)RBUF_LEN(st),
         RBUF_LEN(st) > 0 ? st[0].suffix : "",
         RBUF_LEN(st) > 1 ? st[1].suffix : "",
         RBUF_LEN(st) > 2 ? st[2].suffix : "");
   RBUF_FREE(st);

   /* States: only this emulator's, by name; no hash to compare. */
   l[0] = local(".state1", MD5_A);
   r[0] = remote(50, "Super Metroid (USA).state1", "bsnes", "", T3);
   r[1] = remote(51, "Super Metroid (USA).state.auto", "Snes9x", "", T2);
   r[2] = remote(52, "Super Metroid (USA).state1", "Snes9x", "", T1);
   st   = plan(g, NULL, 0, l, 1, NULL, 0, r, 3);
   CHECK(RBUF_LEN(st) == 2
         && step_is(st, 0, ROMM_SAVES_CONFLICT, ".state1", 52)
         && step_is(st, 1, ROMM_SAVES_DOWNLOAD, ".state.auto", 51)
         && st[0].is_state && st[1].is_state,
         "states: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);
   f = romm_saves_game_file(g, ".state1", true);
   f->remote_id   = 52;
   f->remote_time = romm_json_time(T1);
   strlcpy(f->md5, MD5_A, sizeof(f->md5));
   l[0] = local(".state1", MD5_B);
   st   = plan(g, NULL, 0, l, 1, NULL, 0, r, 3);
   CHECK(RBUF_LEN(st) == 2
         && step_is(st, 0, ROMM_SAVES_UPLOAD, ".state1", 52)
         && step_is(st, 1, ROMM_SAVES_DOWNLOAD, ".state.auto", 51),
         "state saved here: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);

   romm_saves_record_free(&rec);
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   romm_saves_result_t result;
} sync_result_t;

static void on_sync(void *user_data, enum romm_status status,
      int http_status, const romm_saves_result_t *result)
{
   sync_result_t *r = (sync_result_t*)user_data;
   r->calls++;
   r->status = status;
   r->result = *result;
}

static void answer(int status, const char *body)
{
   http_transfer_data_t resp;
   char buf[4096];

   memset(&resp, 0, sizeof(resp));
   resp.status = status;
   if (body)
   {
      strlcpy(buf, body, sizeof(buf));
      resp.data = buf;
      resp.len  = strlen(buf);
   }
   stub_req.cb(NULL, &resp, stub_req.user_data,
         status < 0 ? "Connection refused" : NULL);
}

static const char *live_path;
static bool is_live(const char *path)
{
   return live_path && !strcmp(path, live_path);
}

static void sync_params(romm_saves_sync_params_t *p, bool push)
{
   static char record[256];
   memset(p, 0, sizeof(*p));
   fill_pathname_join(record, DIR, "rommarch-saves.json", sizeof(record));
   p->server      = "http://romm.local:8090";
   p->token       = "rmm_0123abcd";
   p->device_id   = "e409";
   p->rom_id      = 3335;
   p->emulator    = "Snes9x";
   p->base        = "Super Metroid (USA)";
   p->rom_base    = "Super Metroid (U)";
   p->save_dir    = DIR "/saves/Snes9x";
   p->state_dir   = DIR "/states/Snes9x";
   p->record_path = record;
   p->push        = push;
   p->is_live     = is_live;
}

static bool read_is(const char *path, const char *want)
{
   void *buf   = NULL;
   int64_t len = 0;
   bool same   = filestream_read_file(path, &buf, &len)
      && (size_t)len == strlen(want) && !memcmp(buf, want, (size_t)len);
   free(buf);
   return same;
}

#define SRM DIR "/saves/Snes9x/Super Metroid (USA).srm"
#define STATE1 DIR "/states/Snes9x/Super Metroid (USA).state1"

static void test_sync(void)
{
   romm_saves_sync_params_t p;
   romm_saves_record_t rec;
   romm_saves_game_t *g;
   sync_result_t r, r2;
   char md5[33];
   char path[256];

   path_mkdir(DIR);

   /* Before the first launch: the web player's save is fetched, as
    * this device, and recorded. */
   memset(&r, 0, sizeof(r));
   sync_params(&p, false);
   stub_reset();
   CHECK(romm_saves_sync(&p, on_sync, &r), "pull not started");
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/saves?rom_id=3335"), "%s",
         stub_req.url);
   answer(200, "[{\"id\": 30, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-12_21-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"content_hash\": \"a2f3f7e1d0b1dc3f2f2a7b7f4c3a0e11\","
         " \"updated_at\": \"2026-10-13T02:00:00+00:00\"}]");
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/states?rom_id=3335"), "%s",
         stub_req.url);
   answer(200, "[{\"id\": 50, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"emulator\": \"bsnes\","
         " \"updated_at\": \"2026-10-13T02:00:00+00:00\"}]");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/saves/30/"
         "content?device_id=e409"), "%s", stub_req.url);
   CHECK(r.calls == 0, "finished before the download");
   answer(200, "SRAM");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.result.downloaded == 1
         && !r.result.uploaded && !r.result.conflicts && !r.result.failed,
         "pull: calls %u status %d down %u failed %u", r.calls, r.status,
         r.result.downloaded, r.result.failed);
   CHECK(read_is(SRM, "SRAM"), "save not written");
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && g->pending && !strcmp(g->base, "Super Metroid (USA)")
         && !strcmp(g->save_dir, DIR "/saves/Snes9x")
         && RBUF_LEN(g->files) == 1 && g->files[0].remote_id == 30
         && g->files[0].remote_time == romm_json_time(
            "2026-10-13T02:00:00Z"),
         "record after the pull");
   romm_saves_record_free(&rec);

   /* After playing: the save goes up to the autosave slot; a state of
    * this emulator goes up too. */
   filestream_write_file(SRM, "SRAM2", 5);
   path_mkdir(DIR "/states/Snes9x");
   filestream_write_file(STATE1, "STATE", 5);
   memset(&r, 0, sizeof(r));
   sync_params(&p, true);
   stub_reset();
   CHECK(romm_saves_sync(&p, on_sync, &r), "push not started");
   answer(200, "[{\"id\": 30, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-12_21-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T02:00:00+00:00\"}]");
   answer(200, "[]");
   romm_saves_md5_file(SRM, md5);
   snprintf(path, sizeof(path), "http://romm.local:8090/api/saves"
         "?rom_id=3335&emulator=Snes9x&slot=autosave&device_id=e409"
         "&content_hash=%s&autocleanup=true&autocleanup_limit=10", md5);
   CHECK(!strcmp(stub_req.method, "POST") && !strcmp(stub_req.url, path),
         "save upload %s", stub_req.url);
   CHECK(strstr(stub_req.body, "filename=\"Super Metroid (USA).srm\"")
         && strstr(stub_req.body, "\r\n\r\nSRAM2\r\n"), "body %s",
         stub_req.body);
   answer(200, "{\"id\": 31, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-12_22-00-00].srm\","
         " \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T03:00:00.500000+00:00\"}");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/states"
         "?rom_id=3335&emulator=Snes9x"), "state upload %s", stub_req.url);
   answer(200, "{\"id\": 51, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"updated_at\": \"2026-10-13T03:00:01+00:00\"}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.result.uploaded == 2
         && !r.result.downloaded && !r.result.conflicts,
         "push: calls %u up %u", r.calls, r.result.uploaded);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && !g->pending
         && romm_saves_game_file(g, ".srm", false)->remote_id == 31
         && romm_saves_game_file(g, ".srm", false)->remote_time
            == romm_json_time("2026-10-13T03:00:00Z")
         && !strcmp(romm_saves_game_file(g, ".srm", false)->md5, md5)
         && romm_saves_game_file(g, ".state1", false)->remote_id == 51,
         "record after the push");
   romm_saves_record_free(&rec);

   /* Played here and in the web player since: nothing moves. */
   filestream_write_file(SRM, "SRAM3", 5);
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[{\"id\": 32, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-12_23-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T04:00:00+00:00\"},"
         " {\"id\": 51, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"emulator\": \"Snes9x\","
         " \"updated_at\": \"2026-10-13T03:00:01+00:00\"}]");
   answer(200, "[{\"id\": 51, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"emulator\": \"Snes9x\","
         " \"updated_at\": \"2026-10-13T03:00:01+00:00\"}]");
   CHECK(r.calls == 1 && r.result.conflicts == 1 && !r.result.uploaded
         && stub_req.count == 2, "conflict: calls %u conflicts %u up %u "
         "requests %u", r.calls, r.result.conflicts, r.result.uploaded,
         stub_req.count);
   CHECK(read_is(SRM, "SRAM3"), "conflicting save touched");
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && g->pending, "a conflict left nothing owed");
   romm_saves_record_free(&rec);

   /* RomM unreachable: the record survives, the game still owed. */
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(-1, NULL);
   CHECK(r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE,
         "unreachable: status %d", r.status);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && g->pending && RBUF_LEN(g->files) == 2,
         "record lost after a failed listing");
   romm_saves_record_free(&rec);

   /* A file a running core owns isn't replaced. */
   filestream_delete(SRM);
   live_path = SRM;
   memset(&r, 0, sizeof(r));
   sync_params(&p, false);
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[{\"id\": 32, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).srm\","
         " \"emulator\": \"snes9x\","
         " \"updated_at\": \"2026-10-13T04:00:00+00:00\"}]");
   answer(200, "[]");
   answer(200, "SRAM4");
   CHECK(r.calls == 1 && r.result.failed == 1 && !r.result.downloaded
         && !path_is_valid(SRM), "live file: failed %u", r.result.failed);
   live_path = NULL;

   /* A second sync waits for the first. */
   memset(&r, 0, sizeof(r));
   memset(&r2, 0, sizeof(r2));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   p.rom_id = 12;
   CHECK(romm_saves_sync(&p, on_sync, &r2) && stub_req.count == 1,
         "second sync didn't wait: %u requests", stub_req.count);
   answer(200, "[]");
   answer(200, "[]");
   CHECK(r.calls == 1 && !r2.calls && stub_req.count == 3
         && !strcmp(stub_req.url,
            "http://romm.local:8090/api/saves?rom_id=12"),
         "queued sync: %u %u %s", r.calls, r2.calls, stub_req.url);
   answer(200, "[]");
   answer(200, "[]");
   CHECK(r2.calls == 1 && r2.status == ROMM_OK, "queued sync not run");

   /* A sync that can't start says so, and never calls back; the game
    * is still owed. */
   memset(&r, 0, sizeof(r));
   sync_params(&p, true);
   stub_reset();
   stub_req.refuse = true;
   CHECK(!romm_saves_sync(&p, on_sync, &r) && !r.calls
         && !RBUF_LEN(romm_saves_queue), "refused start: calls %u",
         r.calls);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && g->pending, "refused start left nothing owed");
   romm_saves_record_free(&rec);

   /* Queued behind one, it calls back as the one before ends. */
   memset(&r, 0, sizeof(r));
   memset(&r2, 0, sizeof(r2));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   CHECK(romm_saves_sync(&p, on_sync, &r2), "queued sync refused");
   answer(200, "[]");
   stub_req.refuse = true;
   answer(200, "[]");
   CHECK(r.calls == 1 && r2.calls == 1
         && r2.status == ROMM_ERR_UNREACHABLE && !RBUF_LEN(romm_saves_queue),
         "queued refused start: %u %u status %d", r.calls, r2.calls,
         r2.status);
   stub_req.refuse = false;

   sync_params(&p, false);
   p.token = "";
   CHECK(!romm_saves_sync(&p, on_sync, &r) && !romm_saves_sync(NULL,
         on_sync, &r), "sync without a token");

   filestream_delete(SRM);
   filestream_delete(STATE1);
   filestream_delete(DIR "/saves/Snes9x");
   filestream_delete(DIR "/saves");
   filestream_delete(DIR "/states/Snes9x");
   filestream_delete(DIR "/states");
   filestream_delete(p.record_path);
}

/* RomM's states for the prune test: the one syncs use, changed at
 * @t60, one kept with another emulator and ten kept with this one,
 * listed newest first; 101, the oldest, is listed last. */
static void kept_states(char *list, size_t len, const char *t60)
{
   size_t n = (size_t)snprintf(list, len, "[{\"id\": 60,"
         " \"rom_id\": 3335, \"file_name\": \"Super Metroid (USA)"
         ".state1\", \"emulator\": \"Snes9x\","
         " \"updated_at\": \"%s\"},"
         " {\"id\": 120, \"rom_id\": 3335, \"file_name\": \"Super"
         " Metroid (USA) (conflict 2026-10-01 00-00-00).state1\","
         " \"emulator\": \"bsnes\","
         " \"updated_at\": \"2026-10-01T00:00:00+00:00\"}", t60);
   unsigned i;

   for (i = 10; i >= 1; i--)
      n += (size_t)snprintf(list + n, len - n, ", {\"id\": %u,"
            " \"rom_id\": 3335, \"file_name\": \"Super Metroid (USA)"
            " (conflict 2026-10-%02u 00-00-00).state%u\","
            " \"emulator\": \"Snes9x\","
            " \"updated_at\": \"2026-10-%02uT00:00:00+00:00\"}",
            100 + i, i, i % 3 + 1, i);
   strlcpy(list + n, "]", len - n);
}

static void test_resolve(void)
{
   romm_saves_sync_params_t p;
   romm_saves_record_t rec;
   romm_saves_game_t *g;
   romm_saves_step_t *st;
   romm_saves_local_t l[1];
   romm_save_t rs[3];
   sync_result_t r;
   char md5[33];
   char url[512];

   /* Kept copies are never RomM's current copy. */
   l[0]  = local(".srm", MD5_A);
   rs[0] = remote(21, "Super Metroid (USA).srm", "Snes9x", MD5_A, T1);
   rs[1] = remote(22, "Super Metroid (USA) [2026-10-12_23-00-00].srm",
         "Snes9x", MD5_B, T3);
   strlcpy(rs[1].slot, "conflict", sizeof(rs[1].slot));
   rs[2] = remote(52, "Super Metroid (USA) (conflict 2026-10-13 01-02-03)"
         ".state1", "Snes9x", "", T3);
   st = plan(NULL, l, 1, NULL, 0, rs, 2, rs + 2, 1);
   CHECK(RBUF_LEN(st) == 1 && step_is(st, 0, ROMM_SAVES_MATCH, ".srm", 21),
         "kept copies: %u steps", (unsigned)RBUF_LEN(st));
   RBUF_FREE(st);

   path_mkdir(DIR);
   path_mkdir(DIR "/saves");
   path_mkdir(DIR "/saves/Snes9x");
   path_mkdir(DIR "/states");
   path_mkdir(DIR "/states/Snes9x");

   /* RomM's copy chosen: this device's goes to the conflict slot
    * first, then RomM's is fetched as this device. */
   filestream_write_file(SRM, "MINE", 4);
   memset(&r, 0, sizeof(r));
   sync_params(&p, false);
   p.resolve = ROMM_SAVES_RESOLVE_REMOTE;
   stub_reset();
   CHECK(romm_saves_sync(&p, on_sync, &r), "resolve not started");
   answer(200, "[{\"id\": 32, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-12_23-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T04:00:00+00:00\"}]");
   answer(200, "[]");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/saves"
         "?rom_id=3335&emulator=Snes9x&slot=conflict"
         "&autocleanup=true&autocleanup_limit=10")
         && strstr(stub_req.body,
            "filename=\"Super Metroid (USA) (conflict).srm\"")
         && strstr(stub_req.body, "\r\n\r\nMINE\r\n"),
         "kept upload %s", stub_req.url);
   answer(200, "{\"id\": 40, \"rom_id\": 3335, \"slot\": \"conflict\","
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_01-00-00].srm\"}");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/saves/32/"
         "content?device_id=e409"), "fetch %s", stub_req.url);
   answer(200, "THEIRS");
   CHECK(r.calls == 1 && r.result.downloaded == 1 && !r.result.conflicts
         && !r.result.failed && read_is(SRM, "THEIRS"),
         "RomM's chosen: down %u conflicts %u failed %u",
         r.result.downloaded, r.result.conflicts, r.result.failed);

   /* This device's chosen: RomM's goes to the conflict slot, then
    * this device's over it. */
   filestream_write_file(SRM, "MINE2", 5);
   memset(&r, 0, sizeof(r));
   p.resolve = ROMM_SAVES_RESOLVE_LOCAL;
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[{\"id\": 33, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-13_00-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T05:00:00+00:00\"}]");
   answer(200, "[]");
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/saves/33/content"), "fetch %s",
         stub_req.url);
   answer(200, "THEIRS2");
   CHECK(strstr(stub_req.url, "slot=conflict")
         && strstr(stub_req.body, "(conflict).srm\"")
         && strstr(stub_req.body, "\r\n\r\nTHEIRS2\r\n"), "kept %s",
         stub_req.url);
   answer(200, "{\"id\": 41, \"rom_id\": 3335, \"slot\": \"conflict\","
         " \"file_name\": \"x.srm\"}");
   romm_saves_md5_file(SRM, md5);
   snprintf(url, sizeof(url), "http://romm.local:8090/api/saves"
         "?rom_id=3335&emulator=Snes9x&slot=autosave&device_id=e409"
         "&content_hash=%s&autocleanup=true&autocleanup_limit=10"
         "&overwrite=true", md5);
   CHECK(!strcmp(stub_req.url, url)
         && strstr(stub_req.body, "\r\n\r\nMINE2\r\n"), "over %s",
         stub_req.url);
   answer(200, "{\"id\": 42, \"rom_id\": 3335, \"slot\": \"autosave\","
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_01-00-01].srm\","
         " \"updated_at\": \"2026-10-13T06:00:01+00:00\"}");
   CHECK(r.calls == 1 && r.result.uploaded == 1 && !r.result.conflicts
         && read_is(SRM, "MINE2"), "this device's chosen: up %u",
         r.result.uploaded);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && romm_saves_game_file(g, ".srm", false)->remote_id == 42,
         "record after keeping this device's");
   romm_saves_record_free(&rec);

   /* A state: RomM's is kept under a name of its own. */
   filestream_write_file(STATE1, "MYSTATE", 7);
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[]");
   answer(200, "[{\"id\": 60, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"emulator\": \"Snes9x\","
         " \"updated_at\": \"2026-10-13T05:00:00+00:00\"}]");
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/states/60/content"), "fetch %s",
         stub_req.url);
   answer(200, "STATE60");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/states"
         "?rom_id=3335&emulator=Snes9x")
         && strstr(stub_req.body,
            "filename=\"Super Metroid (USA) (conflict ")
         && strstr(stub_req.body, ").state1\"")
         && strstr(stub_req.body, "\r\n\r\nSTATE60\r\n"), "kept state %s",
         stub_req.body);
   answer(200, "{\"id\": 61, \"rom_id\": 3335, \"file_name\": \"k\"}");
   CHECK(strstr(stub_req.body, "filename=\"Super Metroid (USA).state1\"")
         && strstr(stub_req.body, "\r\n\r\nMYSTATE\r\n"), "state over %s",
         stub_req.body);
   answer(200, "{\"id\": 60, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"updated_at\": \"2026-10-13T06:00:02+00:00\"}");
   CHECK(r.calls == 1 && r.result.uploaded == 1, "state: up %u",
         r.result.uploaded);

   /* Ten states kept already: the oldest of this emulator's goes
    * once the eleventh is in, before the sync ends. Another
    * emulator's and the state syncs use aren't counted. */
   {
      char list[4096];

      kept_states(list, sizeof(list), "2026-10-13T06:30:00+00:00");
      filestream_write_file(STATE1, "MYSTATE2", 8);
      memset(&r, 0, sizeof(r));
      stub_reset();
      romm_saves_sync(&p, on_sync, &r);
      answer(200, "[]");
      answer(200, list);
      answer(200, "STATE60B");
      answer(200, "{\"id\": 111, \"rom_id\": 3335, \"file_name\":"
            " \"Super Metroid (USA) (conflict 2026-10-13 07-00-00)"
            ".state1\", \"updated_at\": \"2026-10-13T07:00:00+00:00\"}");
      answer(200, "{\"id\": 60, \"rom_id\": 3335,"
            " \"file_name\": \"Super Metroid (USA).state1\","
            " \"updated_at\": \"2026-10-13T07:00:01+00:00\"}");
      CHECK(!r.calls && !strcmp(stub_req.method, "POST")
            && !strcmp(stub_req.url,
               "http://romm.local:8090/api/states/delete")
            && !strcmp(stub_req.body, "{\"states\": [101]}"),
            "prune: calls %u %s %s", r.calls, stub_req.url, stub_req.body);
      answer(200, "[101]");
      CHECK(r.calls == 1 && r.result.uploaded == 1 && !r.result.failed,
            "after the prune: calls %u up %u", r.calls, r.result.uploaded);

      /* A failed delete doesn't fail the sync; the next keep retries. */
      kept_states(list, sizeof(list), "2026-10-13T07:30:00+00:00");
      filestream_write_file(STATE1, "MYSTATE3", 8);
      memset(&r, 0, sizeof(r));
      stub_reset();
      romm_saves_sync(&p, on_sync, &r);
      answer(200, "[]");
      answer(200, list);
      answer(200, "STATE60C");
      answer(200, "{\"id\": 112, \"rom_id\": 3335, \"file_name\":"
            " \"Super Metroid (USA) (conflict 2026-10-13 08-00-00)"
            ".state1\", \"updated_at\": \"2026-10-13T08:00:00+00:00\"}");
      answer(200, "{\"id\": 60, \"rom_id\": 3335,"
            " \"file_name\": \"Super Metroid (USA).state1\","
            " \"updated_at\": \"2026-10-13T08:00:01+00:00\"}");
      CHECK(!strcmp(stub_req.body, "{\"states\": [101]}"), "prune %s",
            stub_req.body);
      answer(500, "Internal Server Error");
      CHECK(r.calls == 1 && r.result.uploaded == 1 && !r.result.failed,
            "failed prune: calls %u failed %u", r.calls, r.result.failed);
   }

   /* Keeping fails: nothing else moves. */
   filestream_write_file(SRM, "MINE3", 5);
   memset(&r, 0, sizeof(r));
   p.resolve = ROMM_SAVES_RESOLVE_REMOTE;
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[{\"id\": 43, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-13_02-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T07:00:00+00:00\"}]");
   answer(200, "[{\"id\": 60, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"emulator\": \"Snes9x\","
         " \"updated_at\": \"2026-10-13T06:00:02+00:00\"}]");
   answer(500, "Internal Server Error");
   CHECK(r.calls == 1 && r.result.failed == 1 && !r.result.downloaded
         && read_is(SRM, "MINE3"), "failed keep: failed %u down %u",
         r.result.failed, r.result.downloaded);

   /* After play, nothing is resolved. */
   sync_params(&p, true);
   p.resolve = ROMM_SAVES_RESOLVE_REMOTE;
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[{\"id\": 43, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-13_02-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T07:00:00+00:00\"}]");
   answer(200, "[]");
   CHECK(r.calls == 1 && r.result.conflicts == 1 && stub_req.count == 2,
         "push resolved: conflicts %u requests %u", r.result.conflicts,
         stub_req.count);

   filestream_delete(SRM);
   filestream_delete(STATE1);
   filestream_delete(DIR "/saves/Snes9x");
   filestream_delete(DIR "/saves");
   filestream_delete(DIR "/states/Snes9x");
   filestream_delete(DIR "/states");
   filestream_delete(p.record_path);
}

/* A file here equal to RomM's newest, never recorded. */
static void test_match(void)
{
   romm_saves_sync_params_t p;
   romm_saves_record_t rec;
   romm_saves_game_t *g;
   sync_result_t r;
   char md5[33];
   char state_md5[33];
   char saves[512];
   char states[512];
   char body[256];

   path_mkdir(DIR);
   path_mkdir(DIR "/saves");
   path_mkdir(DIR "/saves/Snes9x");
   path_mkdir(DIR "/states");
   path_mkdir(DIR "/states/Snes9x");
   filestream_write_file(SRM, "SAME", 4);
   filestream_write_file(STATE1, "SAMESTATE", 9);
   romm_saves_md5_file(SRM, md5);
   romm_saves_md5_file(STATE1, state_md5);
   snprintf(saves, sizeof(saves), "[{\"id\": 70, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (U) [2026-10-13_08-00-00].srm\","
         " \"emulator\": \"snes9x\", \"slot\": \"autosave\","
         " \"content_hash\": \"%s\","
         " \"updated_at\": \"2026-10-13T08:00:00+00:00\"}]", md5);
   snprintf(states, sizeof(states), "[{\"id\": 80, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"emulator\": \"Snes9x\", \"content_hash\": \"%s\","
         " \"updated_at\": \"2026-10-13T08:00:00+00:00\"}]", state_md5);

   /* RomM is told this device holds the save, as a download would,
    * so its next upload to the slot isn't refused; then it's
    * recorded. States have no such record in RomM. */
   memset(&r, 0, sizeof(r));
   sync_params(&p, false);
   stub_reset();
   CHECK(romm_saves_sync(&p, on_sync, &r), "match sync not started");
   answer(200, saves);
   answer(200, states);
   snprintf(body, sizeof(body),
         "{\"device_id\":\"e409\",\"content_hash\":\"%s\"}", md5);
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url,
            "http://romm.local:8090/api/saves/70/downloaded")
         && !strcmp(stub_req.body, body), "confirmation %s %s %s",
         stub_req.method, stub_req.url, stub_req.body);
   CHECK(!r.calls, "finished before RomM was told");
   answer(200, "{\"id\": 70, \"rom_id\": 3335, \"file_name\": \"x.srm\","
         " \"updated_at\": \"2026-10-13T08:00:00+00:00\"}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && !r.result.failed
         && !r.result.downloaded && stub_req.count == 3,
         "match: calls %u failed %u requests %u", r.calls, r.result.failed,
         stub_req.count);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && romm_saves_game_file(g, ".srm", false)
         && romm_saves_game_file(g, ".srm", false)->remote_id == 70
         && !strcmp(romm_saves_game_file(g, ".srm", false)->md5, md5)
         && romm_saves_game_file(g, ".state1", false)
         && romm_saves_game_file(g, ".state1", false)->remote_id == 80,
         "record after the match");
   romm_saves_record_free(&rec);

   /* RomM can't be told: not recorded, so the next sync tries again. */
   filestream_delete(p.record_path);
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, saves);
   answer(200, "[]");
   answer(500, "Internal Server Error");
   CHECK(r.calls == 1 && r.result.failed == 1, "failed confirmation:"
         " failed %u", r.result.failed);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && !romm_saves_game_file(g, ".srm", false),
         "a match RomM wasn't told of was recorded");
   romm_saves_record_free(&rec);

   /* Without a device id there is nothing to tell. */
   p.device_id = NULL;
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, saves);
   answer(200, "[]");
   CHECK(r.calls == 1 && !r.result.failed && stub_req.count == 2,
         "match without a device: failed %u requests %u", r.result.failed,
         stub_req.count);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && romm_saves_game_file(g, ".srm", false)
         && romm_saves_game_file(g, ".srm", false)->remote_id == 70,
         "record after a match without a device");
   romm_saves_record_free(&rec);

   filestream_delete(SRM);
   filestream_delete(STATE1);
   filestream_delete(DIR "/saves/Snes9x");
   filestream_delete(DIR "/saves");
   filestream_delete(DIR "/states/Snes9x");
   filestream_delete(DIR "/states");
   filestream_delete(p.record_path);
}

/* A save sent up equal to an older version RomM still keeps. */
static void test_old_version(void)
{
   romm_saves_sync_params_t p;
   romm_saves_record_t rec;
   romm_saves_game_t *g;
   romm_saves_file_t *f;
   sync_result_t r;
   char md5[33];
   char list[1024];
   char url[512];

   path_mkdir(DIR);
   path_mkdir(DIR "/saves");
   path_mkdir(DIR "/saves/Snes9x");
   path_mkdir(DIR "/states");
   path_mkdir(DIR "/states/Snes9x");
   /* The save erased in the game, as blank as the first one sent. */
   filestream_write_file(SRM, "BLANK", 5);
   romm_saves_md5_file(SRM, md5);
   sync_params(&p, true);
   memset(&rec, 0, sizeof(rec));
   g = romm_saves_record_game(&rec, 3335, "Snes9x", true);
   g->pending = true;
   f = romm_saves_game_file(g, ".srm", true);
   f->remote_id   = 91;
   f->remote_time = romm_json_time("2026-10-13T09:00:00Z");
   strlcpy(f->md5, "0123456789abcdef0123456789abcdef", sizeof(f->md5));
   romm_saves_record_save(&rec, p.record_path);
   romm_saves_record_free(&rec);
   snprintf(list, sizeof(list), "[{\"id\": 90, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_08-00-00].srm\","
         " \"emulator\": \"Snes9x\", \"slot\": \"autosave\","
         " \"content_hash\": \"%s\","
         " \"updated_at\": \"2026-10-13T08:00:00+00:00\"},"
         " {\"id\": 91, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_09-00-00].srm\","
         " \"emulator\": \"Snes9x\", \"slot\": \"autosave\","
         " \"content_hash\": \"0123456789abcdef0123456789abcdef\","
         " \"updated_at\": \"2026-10-13T09:00:00+00:00\"}]", md5);

   /* RomM answers with version 90; it's sent again over it, and the
    * new version is what's recorded. */
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, list);
   answer(200, "[]");
   CHECK(strstr(stub_req.url, "slot=autosave")
         && !strstr(stub_req.url, "overwrite"), "first upload %s",
         stub_req.url);
   answer(200, "{\"id\": 90, \"rom_id\": 3335, \"slot\": \"autosave\","
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_08-00-00].srm\","
         " \"updated_at\": \"2026-10-13T08:00:00+00:00\"}");
   snprintf(url, sizeof(url), "http://romm.local:8090/api/saves"
         "?rom_id=3335&emulator=Snes9x&slot=autosave&device_id=e409"
         "&content_hash=%s&autocleanup=true&autocleanup_limit=10"
         "&overwrite=true", md5);
   CHECK(!r.calls && stub_req.count == 4 && !strcmp(stub_req.url, url)
         && strstr(stub_req.body, "\r\n\r\nBLANK\r\n"),
         "sent again: %u requests, %s", stub_req.count, stub_req.url);
   answer(200, "{\"id\": 92, \"rom_id\": 3335, \"slot\": \"autosave\","
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_10-00-00].srm\","
         " \"updated_at\": \"2026-10-13T10:00:00+00:00\"}");
   CHECK(r.calls == 1 && r.result.uploaded == 1 && !r.result.failed,
         "old version: up %u failed %u", r.result.uploaded, r.result.failed);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && !g->pending
         && (f = romm_saves_game_file(g, ".srm", false))
         && f->remote_id == 92 && !strcmp(f->md5, md5),
         "record after sending again");
   romm_saves_record_free(&rec);

   /* Answered with the newest it listed, as when the file changed to
    * it since: recorded, nothing sent again. */
   filestream_write_file(SRM, "BLANK2", 6);
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, "[{\"id\": 92, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_10-00-00].srm\","
         " \"emulator\": \"Snes9x\", \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T10:00:00+00:00\"}]");
   answer(200, "[]");
   answer(200, "{\"id\": 92, \"rom_id\": 3335, \"slot\": \"autosave\","
         " \"file_name\": \"x.srm\","
         " \"updated_at\": \"2026-10-13T10:00:00+00:00\"}");
   CHECK(r.calls == 1 && r.result.uploaded == 1 && stub_req.count == 3,
         "newest answered: up %u requests %u", r.result.uploaded,
         stub_req.count);

   filestream_delete(SRM);
   filestream_delete(DIR "/saves/Snes9x");
   filestream_delete(DIR "/saves");
   filestream_delete(DIR "/states/Snes9x");
   filestream_delete(DIR "/states");
   filestream_delete(p.record_path);
}

/* Files left as the last sync found them aren't read again. */
static void test_stamps(void)
{
   romm_saves_sync_params_t p;
   romm_saves_record_t rec;
   romm_saves_game_t *g;
   romm_saves_file_t *f;
   sync_result_t r;
   int64_t old_time = (int64_t)time(NULL) - 100;
   char md5[33];
   char list[512];

   path_mkdir(DIR);
   path_mkdir(DIR "/saves");
   path_mkdir(DIR "/saves/Snes9x");
   path_mkdir(DIR "/states");
   path_mkdir(DIR "/states/Snes9x");
   filestream_write_file(SRM, "KEPT", 4);
   path_set_mtime(SRM, old_time);
   romm_saves_md5_file(SRM, md5);
   sync_params(&p, true);
   memset(&rec, 0, sizeof(rec));
   g = romm_saves_record_game(&rec, 3335, "Snes9x", true);
   f = romm_saves_game_file(g, ".srm", true);
   f->remote_id   = 95;
   f->remote_time = romm_json_time("2026-10-13T09:00:00Z");
   strlcpy(f->md5, md5, sizeof(f->md5));
   romm_saves_record_save(&rec, p.record_path);
   romm_saves_record_free(&rec);
   snprintf(list, sizeof(list), "[{\"id\": 95, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-13_09-00-00].srm\","
         " \"emulator\": \"Snes9x\", \"slot\": \"autosave\","
         " \"content_hash\": \"%s\","
         " \"updated_at\": \"2026-10-13T09:00:00+00:00\"}]", md5);

   /* In sync: the file's size and time are noted with its MD5. */
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, list);
   answer(200, "[]");
   CHECK(r.calls == 1 && stub_req.count == 2, "in sync: %u requests",
         stub_req.count);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && (f = romm_saves_game_file(g, ".srm", false))
         && f->size == 4 && f->mtime == old_time, "stamp %lld %lld",
         f ? (long long)f->size : -1LL, f ? (long long)f->mtime : -1LL);

   /* The next sync takes the record's MD5 without reading the file: a
    * record saying otherwise is believed, so nothing goes up. */
   strlcpy(f->md5, MD5_CACHED, sizeof(f->md5));
   romm_saves_record_save(&rec, p.record_path);
   romm_saves_record_free(&rec);
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, list);
   answer(200, "[]");
   CHECK(r.calls == 1 && !r.result.uploaded && stub_req.count == 2,
         "file read again: up %u requests %u", r.result.uploaded,
         stub_req.count);

   /* Written since, in the second it's listed (a later time, so the
    * check can't cross a second): read, sent, and its time not noted,
    * as it could change again unseen. */
   filestream_write_file(SRM, "NEWER", 5);
   path_set_mtime(SRM, (int64_t)time(NULL) + 5);
   romm_saves_md5_file(SRM, md5);
   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_saves_sync(&p, on_sync, &r);
   answer(200, list);
   answer(200, "[]");
   CHECK(strstr(stub_req.url, "slot=autosave"), "changed file not sent");
   answer(200, "{\"id\": 96, \"rom_id\": 3335, \"slot\": \"autosave\","
         " \"file_name\": \"x.srm\","
         " \"updated_at\": \"2026-10-13T10:00:00+00:00\"}");
   CHECK(r.calls == 1 && r.result.uploaded == 1, "changed: up %u",
         r.result.uploaded);
   CHECK(romm_saves_record_load(&rec, p.record_path)
         && (g = romm_saves_record_game(&rec, 3335, "Snes9x", false))
         && (f = romm_saves_game_file(g, ".srm", false))
         && !strcmp(f->md5, md5) && !f->mtime, "racy stamp kept: %lld",
         f ? (long long)f->mtime : -1LL);
   romm_saves_record_free(&rec);

   filestream_delete(SRM);
   filestream_delete(DIR "/saves/Snes9x");
   filestream_delete(DIR "/saves");
   filestream_delete(DIR "/states/Snes9x");
   filestream_delete(DIR "/states");
   filestream_delete(p.record_path);
}

int main(void)
{
   test_time();
   test_suffix();
   test_local();
   test_record();
   test_plan();
   test_sync();
   test_resolve();
   test_match();
   test_old_version();
   test_stamps();
   filestream_delete(DIR);

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM save sync checks passed\n");
   return 0;
}
