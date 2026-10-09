/* Harness for the RomM library (network/romm/romm_library.c): the
 * romm:// paths games get, playlist file names, and the playlists
 * written for a platform, read back as JSON.
 *
 * Includes the library's translation unit so its statics are tested
 * as shipped. Files are written to the current directory and removed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>

#include "../../../network/romm/romm_library.c"

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

static romm_rom_t rom(unsigned id, bool single_file, const char *fs_name,
      const char *name, const char *crc)
{
   romm_rom_t r;
   memset(&r, 0, sizeof(r));
   r.id          = id;
   r.single_file = single_file;
   strlcpy(r.fs_name, fs_name, sizeof(r.fs_name));
   strlcpy(r.name, name, sizeof(r.name));
   strlcpy(r.crc, crc, sizeof(r.crc));
   return r;
}

static void test_content_path(void)
{
   char s[64];
   romm_rom_t r = rom(3335, true, "Final Fight CD (USA).zip", "", "");

   CHECK(romm_library_content_path(s, sizeof(s), &r) == 36
         && !strcmp(s, "romm://3335/Final Fight CD (USA).zip"),
         "single file: %s", s);
   r = rom(4920, false, "AeroWings (1999)(Crave)", "", "");
   CHECK(romm_library_content_path(s, sizeof(s), &r)
         && !strcmp(s, "romm://4920/AeroWings (1999)(Crave).m3u"),
         "folder: %s", s);
   CHECK(!romm_library_content_path(s, 20, &r) && !*s,
         "truncated path returned: %s", s);
}

static void test_playlist_name(void)
{
   romm_platform_t p;
   char s[64];

   memset(&p, 0, sizeof(p));
   strlcpy(p.slug, "genesis", sizeof(p.slug));
   strlcpy(p.name, "Sega Mega Drive/Genesis", sizeof(p.name));
   strlcpy(p.libretro_slug, "Sega - Mega Drive - Genesis",
         sizeof(p.libretro_slug));
   CHECK(romm_library_playlist_name(s, sizeof(s), &p)
         && !strcmp(s, "Sega - Mega Drive - Genesis.lpl"),
         "libretro name: %s", s);
   p.libretro_slug[0] = '\0';
   CHECK(romm_library_playlist_name(s, sizeof(s), &p)
         && !strcmp(s, "RomM - Sega Mega Drive_Genesis.lpl"),
         "RomM name with a slash: %s", s);
   p.name[0] = '\0';
   CHECK(romm_library_playlist_name(s, sizeof(s), &p)
         && !strcmp(s, "RomM - genesis.lpl"), "slug only: %s", s);
   CHECK(!romm_library_playlist_name(s, 8, &p) && !*s,
         "truncated name returned: %s", s);
}

/* The string at @key of item @item ("" for the playlist itself). */
static bool lpl_get(const char *doc, size_t len, int item, const char *key,
      char *s, size_t s_len)
{
   rjson_t *json = rjson_open_buffer(doc, len);
   enum rjson_type type;
   int cur     = -1;
   bool at_key = false;
   bool found  = false;

   while (!found && (type = rjson_next(json)) != RJSON_DONE
         && type != RJSON_ERROR)
   {
      unsigned depth = rjson_get_context_depth(json);
      if (type == RJSON_OBJECT && depth == 3)
         cur++;
      if (at_key)
      {
         size_t _len;
         if (type == RJSON_STRING || type == RJSON_NUMBER)
         {
            strlcpy(s, rjson_get_string(json, &_len), s_len);
            found = true;
         }
         at_key = false;
      }
      else if (type == RJSON_STRING && (rjson_get_context_count(json) & 1)
            && ((item < 0 && depth == 1) || (depth == 3 && cur == item)))
      {
         size_t _len;
         at_key = string_is_equal(rjson_get_string(json, &_len), key);
      }
   }
   rjson_free(json);
   return found;
}

static void test_write_playlist(void)
{
   const char *path = "romm_library_test.lpl";
   romm_rom_t roms[3];
   char *doc       = NULL;
   int64_t len     = 0;
   char s[256];

   roms[0] = rom(3335, true, "Final Fight CD (USA).zip", "Final Fight CD",
         "0beca6fd");
   roms[1] = rom(2810, false, "Lunar \"Silver\" (USA)", "Lunar: Silver Star",
         "17c2c9b9");
   roms[2] = rom(7, true, "a.md", "A \\ B", "");

   CHECK(romm_library_write_playlist(path, "Sega - Mega-CD - Sega CD.lpl",
         "/cores/genesis_plus_gx_libretro.dylib", "Genesis Plus GX",
         roms, 3), "playlist not written");
   CHECK(filestream_read_file(path, (void**)&doc, &len) && doc,
         "playlist not read back");
   if (!doc)
      return;

   CHECK(lpl_get(doc, len, -1, "version", s, sizeof(s))
         && !strcmp(s, "1.5"), "version %s", s);
   CHECK(lpl_get(doc, len, -1, "default_core_path", s, sizeof(s))
         && !strcmp(s, "/cores/genesis_plus_gx_libretro.dylib"),
         "default core %s", s);
   CHECK(lpl_get(doc, len, -1, "thumbnail_match_mode", s, sizeof(s))
         && !strcmp(s, "1"), "thumbnail match mode %s", s);
   CHECK(lpl_get(doc, len, -1, "sort_mode", s, sizeof(s))
         && !strcmp(s, "2"), "sort mode %s", s);
   CHECK(lpl_get(doc, len, -1, "label_display_mode", s, sizeof(s))
         && !strcmp(s, "0"), "label display mode %s", s);

   CHECK(lpl_get(doc, len, 0, "path", s, sizeof(s))
         && !strcmp(s, "romm://3335/Final Fight CD (USA).zip"), "0 path %s", s);
   CHECK(lpl_get(doc, len, 0, "label", s, sizeof(s))
         && !strcmp(s, "Final Fight CD"), "0 label %s", s);
   CHECK(lpl_get(doc, len, 0, "core_path", s, sizeof(s))
         && !strcmp(s, "DETECT"), "0 core %s", s);
   CHECK(lpl_get(doc, len, 0, "crc32", s, sizeof(s))
         && !strcmp(s, "0BECA6FD|crc"), "0 crc %s", s);
   CHECK(lpl_get(doc, len, 0, "db_name", s, sizeof(s))
         && !strcmp(s, "Sega - Mega-CD - Sega CD.lpl"), "0 db %s", s);
   CHECK(lpl_get(doc, len, 1, "path", s, sizeof(s))
         && !strcmp(s, "romm://2810/Lunar \"Silver\" (USA).m3u"),
         "1 path %s", s);
   CHECK(lpl_get(doc, len, 1, "crc32", s, sizeof(s)) && !*s,
         "folder game has a CRC: %s", s);
   CHECK(lpl_get(doc, len, 2, "label", s, sizeof(s))
         && !strcmp(s, "A \\ B"), "2 label %s", s);
   CHECK(!lpl_get(doc, len, 3, "path", s, sizeof(s)), "extra item %s", s);
   free(doc);
   doc = NULL;

   /* Rewriting replaces; no games still makes a valid playlist. */
   CHECK(romm_library_write_playlist(path, "x.lpl", NULL, NULL, roms, 0),
         "empty playlist not written");
   CHECK(filestream_read_file(path, (void**)&doc, &len) && doc,
         "empty playlist not read back");
   if (doc)
   {
      CHECK(lpl_get(doc, len, -1, "default_core_path", s, sizeof(s))
            && !*s, "default core %s", s);
      CHECK(!lpl_get(doc, len, 0, "path", s, sizeof(s)), "item in empty");
      CHECK(strstr(doc, "\"items\": [\n\n  ]") != NULL, "items: %s", doc);
      free(doc);
   }
   filestream_delete(path);

   CHECK(!romm_library_write_playlist("no-such-dir/x.lpl", "x.lpl", NULL,
         NULL, roms, 1), "write into a missing folder reported done");
}

int main(void)
{
   test_content_path();
   test_playlist_name();
   test_write_playlist();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM library checks passed\n");
   return 0;
}
