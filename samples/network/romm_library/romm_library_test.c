/* Harness for the RomM library (network/romm/romm_library.c): the
 * romm:// paths games get, playlist file names, the playlists written
 * for a platform, read back as JSON, and the manifest's round trip.
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

static romm_library_platform_t lib_platform(unsigned id, const char *slug,
      const char *libretro_slug, const char *playlist)
{
   romm_library_platform_t p;
   memset(&p, 0, sizeof(p));
   p.platform.id        = id;
   p.platform.rom_count = 2;
   strlcpy(p.platform.slug, slug, sizeof(p.platform.slug));
   strlcpy(p.platform.name, "Name of \"it\"", sizeof(p.platform.name));
   strlcpy(p.platform.libretro_slug, libretro_slug,
         sizeof(p.platform.libretro_slug));
   strlcpy(p.playlist, playlist, sizeof(p.playlist));
   return p;
}

static void test_manifest(void)
{
   const char *dir  = "romm_library_test_dir";
   char path[256];
   romm_library_t lib, back;
   romm_library_platform_t p;
   romm_rom_t r;

   CHECK(romm_library_manifest_path(path, sizeof(path), dir)
         && !strcmp(path, "romm_library_test_dir/romm/library.json"),
         "manifest path %s", path);
   CHECK(!romm_library_load(&back, path) && !back.platforms,
         "loaded a manifest that isn't there");

   memset(&lib, 0, sizeof(lib));
   strlcpy(lib.server, "http://192.168.68.46:8090", sizeof(lib.server));
   lib.synced_at = 1791500000LL;
   p = lib_platform(15, "segacd", "Sega - Mega-CD - Sega CD",
         "Sega - Mega-CD - Sega CD.lpl");
   r = rom(3335, true, "Final Fight CD (USA).zip", "Final Fight CD",
         "0beca6fd");
   r.platform_id = 15;
   strlcpy(r.fs_name_no_ext, "Final Fight CD (USA)", sizeof(r.fs_name_no_ext));
   strlcpy(r.cover, "/assets/romm/resources/roms/15/3335/cover/big.png"
         "?ts=2026-10-03 00:23:20", sizeof(r.cover));
   strlcpy(r.updated_at, "2026-10-03T00:23:20+00:00", sizeof(r.updated_at));
   RBUF_PUSH(p.roms, r);
   r = rom(2810, false, "Lunar \"Silver\" (USA)", "Lunar: Silver Star", "");
   RBUF_PUSH(p.roms, r);
   RBUF_PUSH(lib.platforms, p);
   p = lib_platform(26, "switch", "", "RomM - Nintendo Switch.lpl");
   RBUF_PUSH(lib.platforms, p);
   p = lib_platform(13, "nes", "Nintendo - Nintendo Entertainment System",
         "Nintendo - Nintendo Entertainment System.lpl");
   r = rom(2621, true, "Addams Family, The.zip",
         "\xe8\xb1\xaa\xe8\xa1\x80 \\ tab\there", "1234abcd");
   RBUF_PUSH(p.roms, r);
   RBUF_PUSH(lib.platforms, p);

   CHECK(romm_library_save(&lib, path), "manifest not saved");
   CHECK(romm_library_load(&back, path), "manifest not loaded");
   CHECK(!strcmp(back.server, lib.server) && back.synced_at == lib.synced_at,
         "header: %s %lld", back.server, (long long)back.synced_at);
   CHECK(RBUF_LEN(back.platforms) == 3, "%u platforms",
         (unsigned)RBUF_LEN(back.platforms));
   if (RBUF_LEN(back.platforms) == 3)
   {
      size_t i, j;
      for (i = 0; i < 3; i++)
      {
         const romm_library_platform_t *a = &lib.platforms[i];
         const romm_library_platform_t *b = &back.platforms[i];
         CHECK(a->platform.id == b->platform.id
               && a->platform.rom_count == b->platform.rom_count
               && !strcmp(a->platform.slug, b->platform.slug)
               && !strcmp(a->platform.name, b->platform.name)
               && !strcmp(a->platform.libretro_slug, b->platform.libretro_slug)
               && !strcmp(a->playlist, b->playlist),
               "platform %u: %u %s [%s] [%s] %s", (unsigned)i,
               b->platform.id, b->platform.slug, b->platform.name,
               b->platform.libretro_slug, b->playlist);
         CHECK(RBUF_LEN(a->roms) == RBUF_LEN(b->roms), "platform %u: %u games",
               (unsigned)i, (unsigned)RBUF_LEN(b->roms));
         for (j = 0; j < RBUF_LEN(a->roms) && j < RBUF_LEN(b->roms); j++)
         {
            const romm_rom_t *x = &a->roms[j];
            const romm_rom_t *y = &b->roms[j];
            CHECK(x->id == y->id && y->platform_id == a->platform.id
                  && x->single_file == y->single_file
                  && !strcmp(x->crc, y->crc)
                  && !strcmp(x->fs_name, y->fs_name)
                  && !strcmp(x->fs_name_no_ext, y->fs_name_no_ext)
                  && !strcmp(x->name, y->name)
                  && !strcmp(x->cover, y->cover)
                  && !strcmp(x->updated_at, y->updated_at),
                  "game %u: %u %u %d %s [%s] [%s] [%s] [%s] %s", y->id,
                  x->id, y->platform_id, y->single_file, y->crc, y->fs_name,
                  y->fs_name_no_ext, y->name, y->cover, y->updated_at);
         }
      }
   }
   romm_library_free(&back);
   CHECK(!back.platforms && !*back.server, "freed library not empty");

   /* A newer format, or a damaged file, reads as no library. */
   {
      char *doc   = NULL;
      int64_t len = 0;
      if (filestream_read_file(path, (void**)&doc, &len) && doc)
      {
         char *v = strstr(doc, "\"version\": 1");
         if (v)
         {
            v[11] = '2';
            filestream_write_file(path, doc, len);
            CHECK(!romm_library_load(&back, path) && !back.platforms,
                  "loaded a newer manifest format");
            v[11] = '1';
         }
         filestream_write_file(path, doc, len / 2);
         CHECK(!romm_library_load(&back, path) && !back.platforms,
               "loaded a truncated manifest");
         free(doc);
      }
   }

   filestream_delete(path);
   filestream_delete("romm_library_test_dir/romm");
   filestream_delete(dir);
   romm_library_free(&lib);
}

int main(void)
{
   test_content_path();
   test_playlist_name();
   test_write_playlist();
   test_manifest();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM library checks passed\n");
   return 0;
}
