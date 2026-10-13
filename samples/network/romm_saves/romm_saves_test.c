/* Harness for RomM save sync (network/romm/romm_saves.c): the record
 * of what is in sync, its round trip through JSON, RomM's times, which
 * files in a folder are a game's saves or save states, and their MD5.
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
   CHECK(romm_saves_time("2026-10-13T02:18:25+00:00") == 1791857905,
         "list time %lld",
         (long long)romm_saves_time("2026-10-13T02:18:25+00:00"));
   /* An upload's reply carries microseconds; the same second. */
   CHECK(romm_saves_time("2026-10-13T02:18:25.590502+00:00") == 1791857905,
         "fraction dropped");
   CHECK(romm_saves_time("2026-10-13T02:18:25Z") == 1791857905, "Z");
   CHECK(romm_saves_time("2026-10-13T04:18:25+02:00") == 1791857905
         && romm_saves_time("2026-10-12T21:18:25-05:00") == 1791857905,
         "offsets");
   CHECK(romm_saves_time("1970-01-01T00:00:00Z") == 0
         && romm_saves_time("2024-02-29T12:00:00Z") == 1709208000
         && romm_saves_time("2000-03-01T00:00:00Z") == 951868800,
         "calendar");
   CHECK(romm_saves_time("2026-10-13") == -1
         && romm_saves_time("2026-13-13T02:18:25Z") == -1
         && romm_saves_time("2026-10-13T02:18:25 junk") == -1
         && romm_saves_time("") == -1 && romm_saves_time(NULL) == -1,
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

   saves  = romm_saves_list_local(DIR, "Game", false);
   states = romm_saves_list_local(DIR, "Game", true);
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
   CHECK(!romm_saves_list_local(DIR "/none", "Game", false)
         && !romm_saves_list_local(DIR, "", false), "listed nothing");

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
            && !strcmp(f->md5, "bc8177cd3eda0a4750c6e17b57eaa076"),
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

int main(void)
{
   test_time();
   test_suffix();
   test_local();
   test_record();
   filestream_delete(DIR);

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM save sync checks passed\n");
   return 0;
}
