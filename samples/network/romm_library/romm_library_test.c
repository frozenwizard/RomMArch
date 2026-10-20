/* Harness for the RomM library (network/romm/romm_library.c): the
 * romm:// paths games get, playlist file names, the playlists written
 * for a platform, read back as JSON, and the manifest's round trip.
 * Then a sync, with the test playing RomM through the client harness's
 * request stubs: the requests made, paging, the playlists and manifest
 * written, and how a failed request or a cancel ends it; playlists of
 * platforms gone from RomM removed, others' left alone, and the
 * manifest after a sync that stopped part way; incremental syncs and
 * the default core each playlist gets. Covers: where they go,
 * which a sync fetches (new or changed in RomM, not already on disk),
 * and which it deletes.
 *
 * Includes the library's translation unit so its statics are tested
 * as shipped. Files are written to the current directory and removed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>

#include "../../../network/romm/romm_library.c"
#include "../../../network/romm/romm_sidebar.c"

#include "../romm_client/stubs_retroarch.h"

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

/* Paths are joined with the platform's separator; compare them in '/'. */
static bool path_is(const char *path, const char *want)
{
   char s[PATH_MAX_LENGTH];
   strlcpy(s, path, sizeof(s));
   pathname_make_slashes_portable(s);
   return !strcmp(s, want);
}

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

/* Replaces @old with @new in the file at @path. */
static void file_replace(const char *path, const char *old, const char *new_s)
{
   char *doc   = NULL;
   int64_t len = 0;
   char *at;

   if (filestream_read_file(path, (void**)&doc, &len) && doc
         && (at = strstr(doc, old)))
   {
      char edited[8192];
      snprintf(edited, sizeof(edited), "%.*s%s%s", (int)(at - doc), doc,
            new_s, at + strlen(old));
      filestream_write_file(path, edited, (int64_t)strlen(edited));
   }
   free(doc);
}

/* A core the user sets for one game survives the playlist being
 * rewritten, a rename in RomM included, and so do the playlist's
 * display settings, but not a thumbnail match the covers don't fit. */
static void test_game_core_kept(void)
{
   const char *path = "romm_library_test_cores.lpl";
   romm_rom_t roms[3];
   char *doc   = NULL;
   int64_t len = 0;
   char s[256];

   roms[0] = rom(3335, true, "Final Fight CD (USA).zip", "Final Fight CD", "");
   roms[1] = rom(2810, false, "Lunar (USA)", "Lunar", "");
   roms[2] = rom(7, true, "a.md", "A", "");
   CHECK(romm_library_write_playlist(path, "x.lpl", NULL, NULL, roms, 3),
         "playlist not written");

   /* Set Core Association on Lunar. */
   if (filestream_read_file(path, (void**)&doc, &len) && doc)
   {
      char *lunar = strstr(doc, "romm://2810/");
      char *core  = lunar ? strstr(lunar, "\"core_path\": \"DETECT\",") : NULL;
      if (core)
      {
         char edited[4096];
         snprintf(edited, sizeof(edited), "%.*s\"core_path\": \"/cores/x.dylib\","
               " \"core_name\": \"X\",%s", (int)(core - doc), doc,
               strstr(core, "\"core_name\": \"DETECT\",") + 24);
         filestream_write_file(path, edited, (int64_t)strlen(edited));
      }
      free(doc);
      doc = NULL;
   }

   file_replace(path, "\"sort_mode\": 2", "\"sort_mode\": 1");
   file_replace(path, "\"right_thumbnail_mode\": 0",
         "\"right_thumbnail_mode\": 3");
   file_replace(path, "\"label_display_mode\": 0",
         "\"label_display_mode\": 2");
   file_replace(path, "\"thumbnail_match_mode\": 1",
         "\"thumbnail_match_mode\": 0");

   strlcpy(roms[1].fs_name, "Lunar - The Silver Star (USA)",
         sizeof(roms[1].fs_name));
   CHECK(romm_library_write_playlist(path, "x.lpl", NULL, NULL, roms, 2),
         "playlist not rewritten");
   if (filestream_read_file(path, (void**)&doc, &len) && doc)
   {
      CHECK(lpl_get(doc, len, 1, "path", s, sizeof(s))
            && !strcmp(s, "romm://2810/Lunar - The Silver Star (USA).m3u"),
            "renamed: %s", s);
      CHECK(lpl_get(doc, len, 1, "core_path", s, sizeof(s))
            && !strcmp(s, "/cores/x.dylib"), "game's core: %s", s);
      CHECK(lpl_get(doc, len, 1, "core_name", s, sizeof(s))
            && !strcmp(s, "X"), "game's core name: %s", s);
      CHECK(lpl_get(doc, len, 0, "core_path", s, sizeof(s))
            && !strcmp(s, "DETECT"), "other game's core: %s", s);
      CHECK(lpl_get(doc, len, -1, "sort_mode", s, sizeof(s))
            && !strcmp(s, "1"), "sort mode %s", s);
      CHECK(lpl_get(doc, len, -1, "right_thumbnail_mode", s, sizeof(s))
            && !strcmp(s, "3"), "right thumbnail mode %s", s);
      CHECK(lpl_get(doc, len, -1, "label_display_mode", s, sizeof(s))
            && !strcmp(s, "2"), "label display mode %s", s);
      CHECK(lpl_get(doc, len, -1, "left_thumbnail_mode", s, sizeof(s))
            && !strcmp(s, "0"), "left thumbnail mode %s", s);
      CHECK(lpl_get(doc, len, -1, "thumbnail_match_mode", s, sizeof(s))
            && !strcmp(s, "1"), "thumbnail match mode %s", s);
      free(doc);
   }
   filestream_delete(path);
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
   strlcpy(p.platform.family_name, *libretro_slug ? "Maker \"A\"" : "",
         sizeof(p.platform.family_name));
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
         && path_is(path, "romm_library_test_dir/romm/library.json"),
         "manifest path %s", path);
   CHECK(!romm_library_load(&back, path) && !back.platforms,
         "loaded a manifest that isn't there");

   memset(&lib, 0, sizeof(lib));
   strlcpy(lib.server, "http://192.168.68.46:8090", sizeof(lib.server));
   lib.synced_at = 1791500000LL;
   p = lib_platform(15, "segacd", "Sega - Mega-CD - Sega CD",
         "Sega - Mega-CD - Sega CD.lpl");
   p.playlist_size  = 77551;
   p.playlist_mtime = 1791500001LL;
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
               && !strcmp(a->platform.family_name, b->platform.family_name)
               && !strcmp(a->playlist, b->playlist)
               && a->playlist_size == b->playlist_size
               && a->playlist_mtime == b->playlist_mtime,
               "platform %u: %u %s [%s] [%s] [%s] %s %lld %lld", (unsigned)i,
               b->platform.id, b->platform.slug, b->platform.name,
               b->platform.libretro_slug, b->platform.family_name, b->playlist,
               (long long)b->playlist_size, (long long)b->playlist_mtime);
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

#define TEST_DIR "romm_library_test_dir"

/* What sync_start() passes as the installed-core finder. */
static romm_library_find_core_t test_find_core;

/* The playlists a sync said it wrote or deleted, and the last one. */
static unsigned written_count;
static char written_path[256];

static void on_playlist_changed(const char *path)
{
   written_count++;
   strlcpy(written_path, path, sizeof(written_path));
}

static bool sync_start(const char *server, const char *token,
      const char *playlist_dir, const char *thumbnails_dir,
      const char *title, const char *covers_title,
      romm_library_sync_cb_t cb, void *user_data)
{
   romm_library_sync_params_t params;
   params.server         = server;
   params.token          = token;
   params.playlist_dir   = playlist_dir;
   params.thumbnails_dir = thumbnails_dir;
   params.title          = title;
   params.covers_title   = covers_title;
   params.find_core      = test_find_core;
   params.playlist_changed = on_playlist_changed;
   return romm_library_sync(&params, cb, user_data);
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   size_t platforms;
   size_t games;
   bool had_old;
} sync_result_t;

static void on_sync(void *user_data, enum romm_status status,
      int http_status, const romm_library_t *old, const romm_library_t *lib)
{
   sync_result_t *r = (sync_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->had_old     = old != NULL;
   r->platforms = RBUF_LEN(lib->platforms);
   r->games     = 0;
   for (i = 0; i < r->platforms; i++)
      r->games += RBUF_LEN(lib->platforms[i].roms);
}

/* Runs the sync task's handler once, then its callback and cleanup if
 * that finished it. Returns whether it finished. */
static bool sync_step(void)
{
   retro_task_t *task = stub_task;

   task->handler(task);
   if (!(task->flags & RETRO_TASK_FLG_FINISHED))
      return false;
   /* The callback may push the covers task in its place. */
   stub_task = NULL;
   task->callback(task, NULL, task->user_data, NULL);
   task->cleanup(task);
   free(task->title);
   free(task);
   return true;
}

/* Runs the handler until it starts a request, then answers it. */
static void sync_answer(int status, const char *body)
{
   http_transfer_data_t resp;
   unsigned before = stub_req.count;
   unsigned steps  = 0;

   while (stub_req.count == before && stub_task && steps++ < 10)
      sync_step();
   CHECK(stub_req.count == before + 1, "no request to answer");
   if (stub_req.count != before + 1)
      return;
   memset(&resp, 0, sizeof(resp));
   resp.status = status;
   resp.data   = (char*)body;
   resp.len    = body ? strlen(body) : 0;
   stub_req.cb(NULL, &resp, stub_req.user_data,
         status < 0 ? "Connection refused" : NULL);
}

/* Steps the task until it finishes; false if it starts a request. */
static bool sync_run_out(void)
{
   unsigned before = stub_req.count;
   unsigned steps  = 0;
   retro_task_t *task;
   while ((task = stub_task) && steps++ < 20)
      if (sync_step() || stub_task != task)
         return stub_req.count == before;
   return false;
}

/* A page of @count games on @platform_id, ids from @first_id. */
static char *roms_page(unsigned platform_id, unsigned first_id,
      unsigned count)
{
   size_t cap = 64 + (size_t)count * 320;
   char *s    = (char*)malloc(cap);
   size_t len = strlcpy(s, "{\"items\": [", cap);
   unsigned i;

   for (i = 0; i < count; i++)
      len += snprintf(s + len, cap - len, "%s{\"id\": %u,"
            " \"platform_id\": %u, \"fs_name\": \"Game %u (USA).md\","
            " \"fs_name_no_ext\": \"Game %u (USA)\", \"name\": \"Game %u\","
            " \"has_simple_single_file\": true, \"crc_hash\": \"%08x\","
            " \"path_cover_large\": \"/assets/%u.png\","
            " \"updated_at\": \"2026-10-02T04:00:%02u+00:00\"}",
            i ? ", " : "", first_id + i, platform_id, first_id + i,
            first_id + i, first_id + i, first_id + i, first_id + i,
            i % 60);
   strlcpy(s + len, "], \"total\": null}", cap - len);
   return s;
}

static unsigned count_entries(const char *path)
{
   char *doc     = NULL;
   int64_t len   = 0;
   unsigned n    = 0;
   const char *p;
   if (!filestream_read_file(path, (void**)&doc, &len) || !doc)
      return 0;
   for (p = doc; (p = strstr(p, "\"path\": \"romm://")); p++)
      n++;
   free(doc);
   return n;
}

static const char *sync_platforms_reply =
   "[{\"id\": 15, \"slug\": \"segacd\", \"rom_count\": 2,"
   " \"name\": \"Sega CD\", \"libretro_slug\": \"Sega - Mega-CD - Sega CD\"},"
   " {\"id\": 26, \"slug\": \"switch\", \"rom_count\": 0,"
   " \"name\": \"Nintendo Switch\", \"libretro_slug\": null},"
   " {\"id\": 8, \"slug\": \"genesis\", \"rom_count\": 300,"
   " \"name\": \"Genesis\", \"libretro_slug\": \"Sega - Mega Drive - Genesis\","
   " \"family_name\": \"Sega\"},"
   " {\"id\": 30, \"slug\": \"megadrive\", \"rom_count\": 1,"
   " \"name\": \"Mega Drive\", \"libretro_slug\": \"Sega - Mega Drive - Genesis\"},"
   " {\"id\": 40, \"slug\": \"ps3\", \"rom_count\": 5,"
   " \"name\": \"PlayStation 3\", \"libretro_slug\": null}]";

static void test_sync(void)
{
   sync_result_t r;
   romm_library_t lib;
   char manifest[256];
   char *page;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);

   memset(&r, 0, sizeof(r));
   stub_reset();
   stub_task = NULL;
   CHECK(sync_start("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, "Syncing", NULL, on_sync, &r) && stub_task, "sync not started");
   CHECK(stub_task && (stub_task->flags & RETRO_TASK_FLG_MAIN_THREAD)
         && !strcmp(stub_task->title, "Syncing"), "task flags or title");

   sync_answer(200, sync_platforms_reply);
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/platforms")
         && strstr(stub_req.headers, "Bearer rmm_tok"), "platforms request"
         " %s", stub_req.url);

   /* The platform with no games is never asked for. */
   page = roms_page(15, 1, 2);
   sync_answer(200, page);
   free(page);
   CHECK(strstr(stub_req.url, "platform_ids=15&") && strstr(stub_req.url,
         "&limit=250&offset=0"), "Sega CD request %s", stub_req.url);

   page = roms_page(8, 100, 250);
   sync_answer(200, page);
   free(page);
   CHECK(strstr(stub_req.url, "platform_ids=8&")
         && strstr(stub_req.url, "&offset=0"), "Genesis page 1 %s",
         stub_req.url);
   CHECK(stub_task && stub_task->progress == 40, "progress %d",
         stub_task ? (int)stub_task->progress : -1);
   page = roms_page(8, 350, 50);
   sync_answer(200, page);
   free(page);
   CHECK(strstr(stub_req.url, "platform_ids=8&")
         && strstr(stub_req.url, "&offset=250"), "Genesis page 2 %s",
         stub_req.url);

   page = roms_page(30, 900, 1);
   sync_answer(200, page);
   free(page);
   CHECK(strstr(stub_req.url, "platform_ids=30&"), "Mega Drive %s",
         stub_req.url);

   /* Everything hidden from this user: no playlist. */
   sync_answer(200, "{\"items\": []}");
   CHECK(strstr(stub_req.url, "platform_ids=40&"), "PS3 %s", stub_req.url);

   CHECK(sync_run_out(), "sync didn't finish without more requests");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.platforms == 3
         && r.games == 303 && r.had_old, "result: calls %u status %d"
         " platforms %u games %u", r.calls, r.status, (unsigned)r.platforms,
         (unsigned)r.games);

   CHECK(count_entries(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl") == 2,
         "Sega CD playlist");
   CHECK(count_entries(TEST_DIR "/Sega - Mega Drive - Genesis.lpl") == 301,
         "Genesis and Mega Drive share one playlist: %u entries",
         count_entries(TEST_DIR "/Sega - Mega Drive - Genesis.lpl"));
   CHECK(!path_is_valid(TEST_DIR "/RomM - PlayStation 3.lpl")
         && !path_is_valid(TEST_DIR "/RomM - Nintendo Switch.lpl"),
         "playlist for a platform without games");

   CHECK(romm_library_load(&lib, manifest), "manifest not written");
   CHECK(!strcmp(lib.server, "http://romm.local:8090") && lib.synced_at > 0
         && RBUF_LEN(lib.platforms) == 3, "manifest: %s %lld %u",
         lib.server, (long long)lib.synced_at,
         (unsigned)RBUF_LEN(lib.platforms));
   if (RBUF_LEN(lib.platforms) == 3)
      CHECK(lib.platforms[1].platform.id == 8
            && RBUF_LEN(lib.platforms[1].roms) == 300
            && lib.platforms[1].roms[299].id == 399
            && !strcmp(lib.platforms[1].platform.family_name, "Sega")
            && !*lib.platforms[0].platform.family_name
            && !strcmp(lib.platforms[2].playlist,
               "Sega - Mega Drive - Genesis.lpl"),
            "manifest Genesis: %u games, maker [%s]", (unsigned)RBUF_LEN(
               lib.platforms[1].roms), lib.platforms[1].platform.family_name);
   romm_library_free(&lib);

   /* No response part way: reported, the manifest left as it was. */
   memset(&r, 0, sizeof(r));
   CHECK(sync_start("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, NULL, NULL, on_sync, &r), "second sync not started");
   CHECK(stub_task && (stub_task->flags & RETRO_TASK_FLG_MUTE),
         "untitled sync not muted");
   sync_answer(200, sync_platforms_reply);
   sync_answer(-1, NULL);
   CHECK(sync_run_out() && r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE
         && !r.had_old, "unreachable: calls %u status %d", r.calls,
         r.status);
   CHECK(romm_library_load(&lib, manifest) && RBUF_LEN(lib.platforms) == 3,
         "manifest changed by a failed sync");
   romm_library_free(&lib);

   /* Cancelled while a request is out: it waits for the answer, then
    * stops without another. */
   memset(&r, 0, sizeof(r));
   CHECK(sync_start("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, NULL, NULL, on_sync, &r), "third sync not started");
   sync_step();
   CHECK(stub_req.count == 9, "%u requests", stub_req.count);
   stub_task->flags |= RETRO_TASK_FLG_CANCELLED;
   sync_step();
   CHECK(stub_task && r.calls == 0, "finished with a request out");
   {
      http_transfer_data_t resp;
      memset(&resp, 0, sizeof(resp));
      resp.status = 200;
      resp.data   = (char*)sync_platforms_reply;
      resp.len    = strlen(sync_platforms_reply);
      stub_req.cb(NULL, &resp, stub_req.user_data, NULL);
   }
   CHECK(sync_run_out() && r.calls == 1 && r.status == ROMM_ERR_CANCELLED,
         "cancel: calls %u status %d", r.calls, r.status);

   /* A request that can't start ends the sync. */
   memset(&r, 0, sizeof(r));
   stub_req.refuse = true;
   CHECK(sync_start("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, NULL, NULL, on_sync, &r), "fourth sync not started");
   CHECK(sync_run_out() && r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE,
         "refused: calls %u status %d", r.calls, r.status);
   stub_req.refuse = false;

   CHECK(!sync_start("http://h", "", TEST_DIR, NULL, NULL, NULL,
            on_sync, &r)
         && !sync_start("", "t", TEST_DIR, NULL, NULL, NULL,
            on_sync, &r)
         && !sync_start("http://h", "t", "", NULL, NULL, NULL,
            on_sync, &r),
         "sync started without a server, token or folder");
   stub_refuse_task = true;
   CHECK(!sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r),
         "sync reported started when its task wasn't");
   stub_refuse_task = false;

   filestream_delete(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl");
   filestream_delete(TEST_DIR "/Sega - Mega Drive - Genesis.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

/* A full page RomM sent is followed by another, though the client
 * left out a game in it that has no file name. */
static void test_sync_short_page(void)
{
   sync_result_t r;
   char manifest[256];
   char *page;
   char *fs_name;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   memset(&r, 0, sizeof(r));
   stub_reset();
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL, on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 253,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   page = roms_page(8, 1, 250);
   if ((fs_name = strstr(page, "\"fs_name\": \"Game 5 (USA)")))
      fs_name[6] = 'X';
   sync_answer(200, page);
   free(page);
   page = roms_page(8, 251, 3);
   sync_answer(200, page);
   free(page);
   CHECK(strstr(stub_req.url, "&offset=250") != NULL, "page 2 %s",
         stub_req.url);
   CHECK(sync_run_out() && r.status == ROMM_OK && r.games == 252,
         "short page: status %d games %u", r.status, (unsigned)r.games);

   filestream_delete(TEST_DIR "/Sega - Mega Drive - Genesis.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

static void test_sync_reconcile(void)
{
   sync_result_t r;
   romm_library_t lib;
   romm_library_platform_t p;
   char manifest[256];
   char *page;
   int64_t synced_at;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);

   /* The last sync wrote Sega CD and a platform since gone; the user
    * has a playlist of their own. */
   memset(&lib, 0, sizeof(lib));
   strlcpy(lib.server, "http://romm.local:8090", sizeof(lib.server));
   lib.synced_at = 1000;
   p = lib_platform(15, "segacd", "Sega - Mega-CD - Sega CD",
         "Sega - Mega-CD - Sega CD.lpl");
   RBUF_PUSH(lib.platforms, p);
   p = lib_platform(99, "gone", "", "RomM - Gone.lpl");
   RBUF_PUSH(lib.platforms, p);
   CHECK(romm_library_save(&lib, manifest), "seed manifest not saved");
   romm_library_free(&lib);
   filestream_write_file(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl", "{}", 2);
   filestream_write_file(TEST_DIR "/RomM - Gone.lpl", "{}", 2);
   filestream_write_file(TEST_DIR "/Mine.lpl", "{}", 2);

   memset(&r, 0, sizeof(r));
   written_count = 0;
   CHECK(sync_start("http://romm.local:8090", "t", TEST_DIR, NULL,
         NULL, NULL, on_sync, &r), "sync not started");
   sync_answer(200, "[{\"id\": 15, \"rom_count\": 2,"
         " \"libretro_slug\": \"Sega - Mega-CD - Sega CD\"},"
         " {\"id\": 16, \"rom_count\": 1, \"name\": \"SNES\","
         " \"libretro_slug\": \"Nintendo - SNES\"}]");
   page = roms_page(15, 1, 2);
   sync_answer(200, page);
   free(page);
   page = roms_page(16, 5, 1);
   sync_answer(200, page);
   free(page);
   CHECK(sync_run_out() && r.status == ROMM_OK && r.platforms == 2,
         "sync: status %d platforms %u", r.status, (unsigned)r.platforms);
   CHECK(!path_is_valid(TEST_DIR "/RomM - Gone.lpl"),
         "playlist of a platform gone from RomM kept");
   CHECK(written_count == 3
         && path_is(written_path, TEST_DIR "/RomM - Gone.lpl"),
         "removal reported: %u changes, last %s", written_count,
         written_path);
   CHECK(path_is_valid(TEST_DIR "/Mine.lpl"), "user's playlist removed");
   CHECK(count_entries(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl") == 2
         && count_entries(TEST_DIR "/Nintendo - SNES.lpl") == 1,
         "playlists not written");
   CHECK(romm_library_load(&lib, manifest) && RBUF_LEN(lib.platforms) == 2
         && lib.platforms[1].platform.id == 16, "manifest after removal");
   synced_at = lib.synced_at;
   romm_library_free(&lib);

   /* Stopped after rewriting Sega CD: SNES stays as the last sync left
    * it, and the sync time doesn't move, so a refresh is still due. */
   memset(&r, 0, sizeof(r));
   CHECK(sync_start("http://romm.local:8090", "t", TEST_DIR, NULL,
         NULL, NULL, on_sync, &r), "second sync not started");
   sync_answer(200, "[{\"id\": 15, \"rom_count\": 3,"
         " \"libretro_slug\": \"Sega - Mega-CD - Sega CD\"},"
         " {\"id\": 16, \"rom_count\": 1, \"name\": \"SNES\","
         " \"libretro_slug\": \"Nintendo - SNES\"},"
         " {\"id\": 17, \"rom_count\": 1, \"name\": \"New\"}]");
   /* Sega CD: its ids, then the one game new since the last sync. */
   sync_answer(200, "{\"rom_id_index\": [1, 2, 3]}");
   page = roms_page(15, 3, 1);
   sync_answer(200, page);
   free(page);
   sync_answer(500, "{\"detail\": \"Internal Server Error\"}");
   CHECK(sync_run_out() && r.status == ROMM_ERR_HTTP
         && r.http_status == 500 && r.platforms == 2 && r.games == 4,
         "partial: status %d HTTP %d platforms %u games %u", r.status,
         r.http_status, (unsigned)r.platforms, (unsigned)r.games);
   CHECK(count_entries(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl") == 3
         && count_entries(TEST_DIR "/Nintendo - SNES.lpl") == 1,
         "playlists after a partial sync");
   CHECK(romm_library_load(&lib, manifest) && RBUF_LEN(lib.platforms) == 2
         && lib.synced_at == synced_at, "partial manifest: %u platforms"
         " synced at %lld", (unsigned)RBUF_LEN(lib.platforms),
         (long long)lib.synced_at);
   if (RBUF_LEN(lib.platforms) == 2)
      CHECK(RBUF_LEN(lib.platforms[0].roms) == 3
            && lib.platforms[1].platform.id == 16
            && RBUF_LEN(lib.platforms[1].roms) == 1,
            "partial manifest games: %u %u",
            (unsigned)RBUF_LEN(lib.platforms[0].roms),
            (unsigned)RBUF_LEN(lib.platforms[1].roms));
   romm_library_free(&lib);

   filestream_delete(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl");
   filestream_delete(TEST_DIR "/Nintendo - SNES.lpl");
   filestream_delete(TEST_DIR "/Mine.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

static void test_cover_path(void)
{
   char s[512];
   romm_rom_t r = rom(3335, true, "Tom & Jerry: \"Fun\" (USA).zip",
         "Tom and Jerry", "");

   CHECK(romm_library_cover_path(s, sizeof(s), "/thumbs",
         "Sega - Mega-CD - Sega CD.lpl", &r)
         && !strcmp(s, "/thumbs/Sega - Mega-CD - Sega CD/Named_Boxarts/"
            "Tom _ Jerry_ _Fun_ (USA).png"), "scrubbed: %s", s);
   r = rom(4920, false, "Christmas Nights Into Dreams... (EUR)", "", "");
   CHECK(romm_library_cover_path(s, sizeof(s), "/thumbs", "RomM - X 2.0.lpl",
         &r) && !strcmp(s, "/thumbs/RomM - X 2.0/Named_Boxarts/"
            "Christmas Nights Into Dreams... (EUR).png"), "folder: %s", s);
}

/* A page of games on platform 15 with the covers given; NULL for none. */
static char *covers_page(const char *cover1, const char *cover2,
      const char *cover3)
{
   const char *covers[3];
   char *s    = (char*)malloc(4096);
   size_t len = strlcpy(s, "{\"items\": [", 4096);
   unsigned i;

   covers[0] = cover1;
   covers[1] = cover2;
   covers[2] = cover3;
   for (i = 0; i < 3; i++)
   {
      if (!covers[i] && i == 2)
         break;
      len += snprintf(s + len, 4096 - len, "%s{\"id\": %u,"
            " \"platform_id\": 15, \"fs_name\": \"Game %u & Co (USA).bin\","
            " \"has_simple_single_file\": true,"
            " \"updated_at\": \"2026-10-02T04:00:00+00:00\","
            " \"path_cover_large\": %s%s%s}",
            i ? ", " : "", i + 1, i + 1, covers[i] ? "\"" : "",
            covers[i] ? covers[i] : "null", covers[i] ? "\"" : "");
   }
   strlcpy(s + len, "]}", 4096 - len);
   return s;
}

#define COVER_DIR TEST_DIR "/thumbs/Sega - Mega-CD - Sega CD/Named_Boxarts/"

/* Syncs one platform through to its end: @page answers the full fetch
 * or, after a first sync, the changed games, with @ids its index. */
static void covers_sync(sync_result_t *r, const char *ids, const char *page)
{
   memset(r, 0, sizeof(*r));
   CHECK(sync_start("http://romm.local:8090", "t", TEST_DIR,
         TEST_DIR "/thumbs", NULL, "Covers", on_sync, r), "sync not started");
   sync_answer(200, "[{\"id\": 15, \"rom_count\": 3,"
         " \"libretro_slug\": \"Sega - Mega-CD - Sega CD\"}]");
   if (ids)
      sync_answer(200, ids);
   sync_answer(200, page);
   CHECK(sync_run_out() && r->status == ROMM_OK, "sync: status %d",
         r->status);
}

/* Answers download @n (counting from 1) with @status and @body. */
static void answer_download(unsigned n, int status, const char *body)
{
   http_transfer_data_t resp;
   stub_logged_t *l = &stub_log[(n - 1) % STUB_LOG_MAX];

   memset(&resp, 0, sizeof(resp));
   resp.status = status;
   resp.data   = (char*)body;
   resp.len    = body ? strlen(body) : 0;
   l->cb(NULL, &resp, l->user_data, NULL);
}

#define PNG_DATA "\x89PNG\r\n\x1a\n-png"
#define JPG_DATA "\xff\xd8\xff\xe0-jpg"

static bool file_is(const char *path, const char *data)
{
   char *doc   = NULL;
   int64_t len = 0;
   bool same   = filestream_read_file(path, (void**)&doc, &len) && doc
      && len == (int64_t)strlen(data) && !memcmp(doc, data, (size_t)len);
   free(doc);
   return same;
}

static void test_covers(void)
{
   sync_result_t r;
   char manifest[256];
   char *page;
   unsigned first;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   stub_reset();

   /* First sync: both covers fetched, four at a time at most, without
    * the cache buster; the game without one gets none. RomM's JPEG
    * covers are saved as .jpg, whatever their name on the server. */
   page = covers_page("/assets/romm/resources/roms/15/1/cover/big.png?ts=2026-10-03 00:23:20",
         "/assets/romm/resources/roms/15/2/cover/big.png?ts=2026-10-03 00:23:21",
         NULL);
   covers_sync(&r, NULL, page);
   free(page);
   CHECK(stub_task && !strcmp(stub_task->title, "Covers"), "no covers task");
   if (!stub_task)
      return;
   first = stub_log_count + 1;
   sync_step();
   CHECK(stub_log_count == first + 1, "%u downloads started",
         stub_log_count - first + 1);
   CHECK(!strcmp(stub_log[(first - 1) % STUB_LOG_MAX].url,
         "http://romm.local:8090/assets/romm/resources/roms/15/1/cover/big.png")
         && !strcmp(stub_log[first % STUB_LOG_MAX].url,
         "http://romm.local:8090/assets/romm/resources/roms/15/2/cover/big.png"),
         "cover URLs %s %s", stub_log[(first - 1) % STUB_LOG_MAX].url,
         stub_log[first % STUB_LOG_MAX].url);
   CHECK(!sync_step(), "covers finished with downloads out");
   answer_download(first + 1, 200, JPG_DATA);
   answer_download(first, 200, PNG_DATA);
   CHECK(sync_step() && !stub_task, "covers task didn't finish");
   CHECK(file_is(COVER_DIR "Game 1 _ Co (USA).png", PNG_DATA),
         "PNG cover not saved as .png");
   CHECK(file_is(COVER_DIR "Game 2 _ Co (USA).jpg", JPG_DATA)
         && !path_is_valid(COVER_DIR "Game 2 _ Co (USA).png"),
         "JPEG cover not saved as .jpg");

   /* Nothing changed: nothing to fetch, .jpg counting as there. */
   page = covers_page("/assets/romm/resources/roms/15/1/cover/big.png?ts=2026-10-03 00:23:20",
         "/assets/romm/resources/roms/15/2/cover/big.png?ts=2026-10-03 00:23:21",
         NULL);
   covers_sync(&r, "{\"rom_id_index\": [1, 2]}", page);
   free(page);
   CHECK(!stub_task, "covers task with nothing to fetch");

   /* Game 1's cover changed in RomM, now a JPEG, and game 2 is gone:
    * 1 is fetched again and its PNG removed, 2's cover deleted. */
   page = covers_page("/assets/romm/resources/roms/15/1/cover/big.png?ts=2026-10-09 10:00:00",
         NULL, NULL);
   covers_sync(&r, "{\"rom_id_index\": [1, 2]}", page);
   free(page);
   CHECK(!path_is_valid(COVER_DIR "Game 2 _ Co (USA).jpg"),
         "cover of a game without one kept");
   CHECK(stub_task != NULL, "changed cover not fetched");
   if (stub_task)
   {
      first = stub_log_count + 1;
      sync_step();
      CHECK(stub_log_count == first, "%u downloads", stub_log_count - first + 1);
      answer_download(first, 200, JPG_DATA);
      CHECK(sync_step() && !stub_task, "covers task didn't finish");
   }
   CHECK(file_is(COVER_DIR "Game 1 _ Co (USA).jpg", JPG_DATA)
         && !path_is_valid(COVER_DIR "Game 1 _ Co (USA).png"),
         "cover that changed format");

   /* Deleted by hand: fetched again. A failed fetch, or one that isn't
    * an image, saves nothing. */
   filestream_delete(COVER_DIR "Game 1 _ Co (USA).jpg");
   page = covers_page("/assets/romm/resources/roms/15/1/cover/big.png?ts=2026-10-09 10:00:00",
         NULL, NULL);
   covers_sync(&r, "{\"rom_id_index\": [1, 2]}", page);
   free(page);
   CHECK(stub_task != NULL, "missing cover not fetched");
   if (stub_task)
   {
      first = stub_log_count + 1;
      sync_step();
      answer_download(first, 200, "<html>Not found</html>");
      CHECK(sync_step() && !stub_task, "covers task didn't finish");
   }
   CHECK(!path_is_valid(COVER_DIR "Game 1 _ Co (USA).png")
         && !path_is_valid(COVER_DIR "Game 1 _ Co (USA).jpg"),
         "saved something that isn't an image");

   page = covers_page("/assets/romm/resources/roms/15/1/cover/big.png?ts=2026-10-09 10:00:00",
         NULL, NULL);
   covers_sync(&r, "{\"rom_id_index\": [1, 2]}", page);
   free(page);
   if (stub_task)
   {
      /* Cancelled with a download out: waits for it, starts no more. */
      first = stub_log_count + 1;
      sync_step();
      stub_task->flags |= RETRO_TASK_FLG_CANCELLED;
      CHECK(!sync_step(), "cancelled covers finished with a download out");
      answer_download(first, 404, "{}");
      CHECK(sync_step() && !stub_task && stub_log_count == first,
            "cancelled covers task");
   }

   filestream_delete(COVER_DIR);
   filestream_delete(TEST_DIR "/thumbs/Sega - Mega-CD - Sega CD");
   filestream_delete(TEST_DIR "/thumbs");
   filestream_delete(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

/* Runs a task stepped outside stub_task to its end. */
static void task_run_out(retro_task_t *task)
{
   unsigned steps = 0;
   while (!(task->flags & RETRO_TASK_FLG_FINISHED) && steps++ < 20)
      task->handler(task);
   task->callback(task, NULL, task->user_data, NULL);
   task->cleanup(task);
   free(task->title);
   free(task);
}

/* A sync while the last one's covers are still being fetched stops
 * that task and fetches the covers itself, once. */
static void test_covers_superseded(void)
{
   const char *ids = "{\"rom_id_index\": [1, 2]}";
   sync_result_t r;
   retro_task_t *first;
   char manifest[256];
   char *page;
   unsigned n, m;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   stub_reset();
   page = covers_page("/assets/1/big.png?ts=1", "/assets/2/big.png?ts=1",
         NULL);

   covers_sync(&r, NULL, page);
   first = stub_task;
   CHECK(first && !strcmp(first->title, "Covers"), "no covers task");
   if (!first)
   {
      free(page);
      return;
   }
   n = stub_log_count + 1;
   sync_step();
   CHECK(stub_log_count == n + 1, "%u downloads", stub_log_count - n + 1);

   covers_sync(&r, ids, page);
   CHECK(first->flags & RETRO_TASK_FLG_CANCELLED, "first covers task left"
         " running");
   CHECK(stub_task && stub_task != first, "second sync fetches no covers");

   /* The first task's downloads land; it starts no more. */
   m = stub_log_count;
   answer_download(n, 200, PNG_DATA);
   answer_download(n + 1, 200, PNG_DATA);
   task_run_out(first);
   CHECK(stub_log_count == m, "cancelled task downloaded more");

   if (stub_task)
   {
      sync_step();
      CHECK(stub_log_count == m + 2, "second task: %u downloads",
            stub_log_count - m);
      answer_download(m + 1, 200, PNG_DATA);
      answer_download(m + 2, 200, PNG_DATA);
      CHECK(sync_step() && !stub_task, "second covers task didn't finish");
   }
   CHECK(!romm_lib_covers_running, "covers task still recorded");

   /* Covers on disk: a third sync has none to fetch, nothing to stop. */
   covers_sync(&r, ids, page);
   CHECK(!stub_task, "covers task with all on disk");
   free(page);

   filestream_delete(COVER_DIR "Game 1 _ Co (USA).png");
   filestream_delete(COVER_DIR "Game 2 _ Co (USA).png");
   filestream_delete(COVER_DIR);
   filestream_delete(TEST_DIR "/thumbs/Sega - Mega-CD - Sega CD");
   filestream_delete(TEST_DIR "/thumbs");
   filestream_delete(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

/* 300 games' covers are planned over steps of the sync, 256 games at
 * most per step, and all of them fetched. */
static void test_covers_in_steps(void)
{
   romm_lib_sync_t *sync;
   sync_result_t r;
   char manifest[256];
   char *page;
   unsigned steps = 0;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   stub_reset();
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, TEST_DIR "/thumbs", NULL, NULL,
         on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 300,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   page = roms_page(8, 1, 250);
   sync_answer(200, page);
   free(page);
   page = roms_page(8, 251, 50);
   sync_answer(200, page);
   free(page);

   sync = (romm_lib_sync_t*)stub_task->state;
   while (sync->state != ROMM_LIB_SYNC_COVERS && steps++ < 5)
      sync_step();
   sync_step();
   CHECK(sync->state == ROMM_LIB_SYNC_COVERS && sync->cover_phase == 1
         && sync->cover_game == 256 && RBUF_LEN(sync->covers) == 256,
         "first step: state %d phase %u game %u, %u covers", sync->state,
         sync->cover_phase, (unsigned)sync->cover_game,
         (unsigned)RBUF_LEN(sync->covers));
   CHECK(sync_run_out() && r.status == ROMM_OK, "sync: status %d",
         r.status);
   CHECK(stub_task && RBUF_LEN(((romm_lib_covers_t*)stub_task->state)->jobs)
         == 300, "covers task: %u jobs", stub_task ? (unsigned)RBUF_LEN(
            ((romm_lib_covers_t*)stub_task->state)->jobs) : 0);
   if (stub_task)
   {
      stub_task->flags |= RETRO_TASK_FLG_CANCELLED;
      CHECK(sync_step() && !stub_task, "covers task didn't stop");
   }

   filestream_delete(TEST_DIR "/Sega - Mega Drive - Genesis.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

/* A game's folder and .rar copies have one cover file between them:
 * one download, the first's. */
static void test_covers_shared_name(void)
{
   const char *page =
      "{\"items\": [{\"id\": 1, \"platform_id\": 15,"
      " \"fs_name\": \"Game 1 & Co (USA).rar\","
      " \"has_simple_single_file\": true,"
      " \"updated_at\": \"2026-10-02T04:00:00+00:00\","
      " \"path_cover_large\": \"/assets/1/big.png?ts=1\"},"
      " {\"id\": 2, \"platform_id\": 15,"
      " \"fs_name\": \"Game 1 & Co (USA)\","
      " \"has_simple_single_file\": false,"
      " \"updated_at\": \"2026-10-02T04:00:00+00:00\","
      " \"path_cover_large\": \"/assets/2/big.png?ts=1\"},"
      " {\"id\": 3, \"platform_id\": 15,"
      " \"fs_name\": \"Game 3 (USA).bin\","
      " \"has_simple_single_file\": true,"
      " \"updated_at\": \"2026-10-02T04:00:00+00:00\","
      " \"path_cover_large\": \"/assets/3/big.png?ts=1\"}]}";
   sync_result_t r;
   char manifest[256];
   unsigned n;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   stub_reset();

   covers_sync(&r, NULL, page);
   CHECK(stub_task != NULL, "no covers task");
   if (stub_task)
   {
      n = stub_log_count + 1;
      sync_step();
      CHECK(stub_log_count == n + 1, "%u downloads", stub_log_count - n + 1);
      CHECK(!strcmp(stub_log[(n - 1) % STUB_LOG_MAX].url,
            "http://romm.local:8090/assets/1/big.png")
            && !strcmp(stub_log[n % STUB_LOG_MAX].url,
            "http://romm.local:8090/assets/3/big.png"), "downloads %s %s",
            stub_log[(n - 1) % STUB_LOG_MAX].url,
            stub_log[n % STUB_LOG_MAX].url);
      answer_download(n, 200, PNG_DATA);
      answer_download(n + 1, 200, JPG_DATA);
      CHECK(sync_step() && !stub_task, "covers task didn't finish");
   }

   filestream_delete(COVER_DIR "Game 1 _ Co (USA).png");
   filestream_delete(COVER_DIR "Game 3 (USA).jpg");
   filestream_delete(COVER_DIR);
   filestream_delete(TEST_DIR "/thumbs/Sega - Mega-CD - Sega CD");
   filestream_delete(TEST_DIR "/thumbs");
   filestream_delete(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl");
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

static void test_cursor(void)
{
   char s[32];

   CHECK(romm_lib_cursor(s, sizeof(s), "2026-10-03T00:23:20+00:00", 60)
         && !strcmp(s, "2026-10-03T00:22:20Z"), "plain: %s", s);
   CHECK(romm_lib_cursor(s, sizeof(s), "2026-10-03T00:00:30+00:00", 60)
         && !strcmp(s, "2026-10-02T23:59:30Z"), "previous day: %s", s);
   CHECK(romm_lib_cursor(s, sizeof(s), "2024-03-01T00:00:10+00:00", 60)
         && !strcmp(s, "2024-02-29T23:59:10Z"), "leap day: %s", s);
   CHECK(romm_lib_cursor(s, sizeof(s), "2026-01-01T01:00:00+02:00", 60)
         && !strcmp(s, "2025-12-31T22:59:00Z"), "zone ahead: %s", s);
   CHECK(romm_lib_cursor(s, sizeof(s), "2026-10-09T04:02:58.718378-05:30",
            0) && !strcmp(s, "2026-10-09T09:32:58Z"), "fraction, zone"
         " behind: %s", s);
   CHECK(romm_lib_cursor(s, sizeof(s), "2026-10-09T04:02:58Z", 0)
         && !strcmp(s, "2026-10-09T04:02:58Z"), "Z: %s", s);
   CHECK(!romm_lib_cursor(s, sizeof(s), "", 60)
         && !romm_lib_cursor(s, sizeof(s), "yesterday", 60),
         "cursor from no time");
}

/* A game for the incremental test's pages. */
static void game_json(char *s, size_t len, unsigned id, const char *name,
      const char *updated_at)
{
   size_t _len = strlen(s);
   snprintf(s + _len, len - _len, "%s{\"id\": %u, \"platform_id\": 8,"
         " \"fs_name\": \"g%u.md\", \"name\": \"%s\","
         " \"has_simple_single_file\": true, \"updated_at\": \"%s\"}",
         _len > 12 ? ", " : "", id, id, name, updated_at);
}

static void test_sync_incremental(void)
{
   const char *pl = TEST_DIR "/Sega - Mega Drive - Genesis.lpl";
   sync_result_t r;
   char manifest[256];
   char page[2048];
   char s[256];
   char *doc       = NULL;
   int64_t len     = 0;
   unsigned before;
   unsigned i;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   stub_reset();

   /* First sync: everything. */
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 3,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 100, "A", "2026-10-02T04:00:00+00:00");
   game_json(page, sizeof(page), 101, "B", "2026-10-03T00:23:20+00:00");
   game_json(page, sizeof(page), 102, "C", "2026-10-01T10:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   CHECK(sync_run_out() && r.status == ROMM_OK && r.games == 3,
         "first: status %d games %u", r.status, (unsigned)r.games);

   /* Nothing changed: the ids and an empty change list, asked for
    * since a minute before the newest game, and no rewrite. */
   written_count = 0;
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 3,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   sync_answer(200, "{\"rom_id_index\": [100, 101, 102]}");
   CHECK(strstr(stub_req.url, "with_rom_id_index=true&") != NULL,
         "ids request %s", stub_req.url);
   sync_answer(200, "{\"items\": []}");
   CHECK(strstr(stub_req.url, "&updated_after=2026-10-03T00:22:20Z")
         && strstr(stub_req.url, "with_rom_id_index=false"),
         "changes request %s", stub_req.url);
   CHECK(sync_run_out() && r.status == ROMM_OK && r.games == 3,
         "unchanged: status %d games %u", r.status, (unsigned)r.games);
   CHECK(!written_count, "unchanged playlist rewritten");

   /* Written over since, as the menu does from a stale copy, or written
    * again as it was, a moment later: rewritten, though RomM didn't
    * change. */
   for (i = 0; i < 2; i++)
   {
      if (!i)
         filestream_write_file(pl, "{\"items\": []}", 13);
      else if (filestream_read_file(pl, (void**)&doc, &len) && doc)
      {
         int64_t mtime = 0;
         filestream_write_file(pl, doc, len);
         if (path_get_mtime(pl, &mtime))
            path_set_mtime(pl, mtime - 10);
         free(doc);
         doc = NULL;
      }
      written_count = 0;
      memset(&r, 0, sizeof(r));
      sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
            on_sync, &r);
      sync_answer(200, "[{\"id\": 8, \"rom_count\": 3,"
            " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
      sync_answer(200, "{\"rom_id_index\": [100, 101, 102]}");
      sync_answer(200, "{\"items\": []}");
      CHECK(sync_run_out() && r.status == ROMM_OK && written_count == 1
            && count_entries(pl) == 3, "%s: %u rewrites, %u entries",
            i ? "touched" : "overwritten", written_count, count_entries(pl));
   }

   /* A game added first in RomM's order, one renamed, one removed. */
   written_count = 0;
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 3,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   sync_answer(200, "{\"rom_id_index\": [103, 100, 102]}");
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 103, "New", "2026-10-09T10:00:00+00:00");
   game_json(page, sizeof(page), 100, "A Renamed", "2026-10-09T10:00:01+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   CHECK(sync_run_out() && r.status == ROMM_OK && r.games == 3,
         "changed: status %d games %u", r.status, (unsigned)r.games);
   CHECK(count_entries(pl) == 3, "%u entries", count_entries(pl));
   CHECK(written_count == 1 && path_is(written_path, pl),
         "rewrite reported %u times, last %s", written_count, written_path);
   if (filestream_read_file(pl, (void**)&doc, &len) && doc)
   {
      CHECK(lpl_get(doc, len, 0, "path", s, sizeof(s))
            && !strcmp(s, "romm://103/g103.md"), "0: %s", s);
      CHECK(lpl_get(doc, len, 1, "label", s, sizeof(s))
            && !strcmp(s, "A Renamed"), "1: %s", s);
      CHECK(lpl_get(doc, len, 2, "path", s, sizeof(s))
            && !strcmp(s, "romm://102/g102.md"), "2: %s", s);
      free(doc);
      doc = NULL;
   }

   /* A game the last sync didn't have and that hasn't changed, such as
    * one shown again: the platform is fetched in full. */
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 4,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   sync_answer(200, "{\"rom_id_index\": [99, 103, 100, 102]}");
   sync_answer(200, "{\"items\": []}");
   CHECK(strstr(stub_req.url, "updated_after=2026-10-09T09:59:01Z") != NULL,
         "changes request %s", stub_req.url);
   before = stub_req.count;
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 99, "Shown", "2026-01-01T00:00:00+00:00");
   game_json(page, sizeof(page), 103, "New", "2026-10-09T10:00:00+00:00");
   game_json(page, sizeof(page), 100, "A Renamed", "2026-10-09T10:00:01+00:00");
   game_json(page, sizeof(page), 102, "C", "2026-10-01T10:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   CHECK(stub_req.count == before + 1 && !strstr(stub_req.url,
         "updated_after") && strstr(stub_req.url, "&offset=0"),
         "full fetch %s", stub_req.url);
   CHECK(sync_run_out() && r.status == ROMM_OK && r.games == 4
         && count_entries(pl) == 4, "full: status %d games %u entries %u",
         r.status, (unsigned)r.games, count_entries(pl));

   /* The playlist deleted by hand is written again. */
   filestream_delete(pl);
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 4,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   sync_answer(200, "{\"rom_id_index\": [99, 103, 100, 102]}");
   sync_answer(200, "{\"items\": []}");
   CHECK(sync_run_out() && count_entries(pl) == 4, "deleted playlist");

   filestream_delete(pl);
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

/* Genesis and Mega Drive share a playlist; when only one changes, the
 * playlist still lists both. */
static void test_sync_shared_playlist(void)
{
   const char *pl = TEST_DIR "/Sega - Mega Drive - Genesis.lpl";
   const char *platforms =
      "[{\"id\": 8, \"rom_count\": 1,"
      " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"},"
      " {\"id\": 30, \"rom_count\": 1,"
      " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]";
   sync_result_t r;
   char manifest[256];
   char page[1024];

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);

   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, platforms);
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 1, "Genesis game", "2026-10-02T04:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 2, "Mega Drive game", "2026-10-02T04:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   CHECK(sync_run_out() && count_entries(pl) == 2, "first: %u entries",
         count_entries(pl));

   /* Genesis gains a game; Mega Drive is unchanged. */
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL,
         on_sync, &r);
   sync_answer(200, platforms);
   sync_answer(200, "{\"rom_id_index\": [1, 3]}");
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 3, "Genesis game 2", "2026-10-09T04:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   sync_answer(200, "{\"rom_id_index\": [2]}");
   sync_answer(200, "{\"items\": []}");
   CHECK(sync_run_out() && r.status == ROMM_OK && count_entries(pl) == 3,
         "shared: status %d, %u entries", r.status, count_entries(pl));

   /* Mega Drive's games all deleted while Genesis is unchanged: its
    * game leaves the playlist. */
   written_count = 0;
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL, on_sync, &r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 2,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"},"
         " {\"id\": 30, \"rom_count\": 0,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   sync_answer(200, "{\"rom_id_index\": [1, 3]}");
   sync_answer(200, "{\"items\": []}");
   CHECK(sync_run_out() && r.status == ROMM_OK && written_count == 1
         && count_entries(pl) == 2, "Mega Drive empty: status %d,"
         " %u rewrites, %u entries", r.status, written_count,
         count_entries(pl));

   /* It has its game again; then Genesis is gone from RomM. */
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL, on_sync, &r);
   sync_answer(200, platforms);
   sync_answer(200, "{\"rom_id_index\": [1, 3]}");
   sync_answer(200, "{\"items\": []}");
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 2, "Mega Drive game", "2026-10-02T04:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   sync_answer(200, page);
   CHECK(sync_run_out() && count_entries(pl) == 3, "back: %u entries",
         count_entries(pl));
   written_count = 0;
   memset(&r, 0, sizeof(r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL, on_sync, &r);
   sync_answer(200, "[{\"id\": 30, \"rom_count\": 1,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   sync_answer(200, "{\"rom_id_index\": [2]}");
   sync_answer(200, "{\"items\": []}");
   CHECK(sync_run_out() && r.status == ROMM_OK && written_count == 1
         && count_entries(pl) == 1, "Genesis gone: status %d,"
         " %u rewrites, %u entries", r.status, written_count,
         count_entries(pl));

   filestream_delete(pl);
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

/* History and Favorites follow a sync: games gone are dropped, a
 * renamed game's playlist path and RomM title follow it, and anything
 * else is left. */
static void test_follow_game(void)
{
   romm_library_t old, lib;
   romm_library_platform_t p;
   const romm_rom_t **old_index, **index;
   char path[256], label[256];

   memset(&old, 0, sizeof(old));
   memset(&lib, 0, sizeof(lib));
   p = lib_platform(15, "gba", "", "x.lpl");
   RBUF_PUSH(p.roms, rom(46, true, "Advance Wars.gba", "Advance Wars", ""));
   RBUF_PUSH(p.roms, rom(3335, true, "Final Fight CD (USA).zip",
         "Final Fight CD", ""));
   RBUF_PUSH(p.roms, rom(4320, false, "Alundra (USA) (v1.1)", "Alundra",
         ""));
   RBUF_PUSH(p.roms, rom(7, true, "gone.md", "Gone", ""));
   RBUF_PUSH(old.platforms, p);
   p = lib_platform(15, "gba", "", "x.lpl");
   RBUF_PUSH(p.roms, rom(4320, false, "Alundra (USA) (v1.1)",
         "Alundra (v1.1)", ""));
   RBUF_PUSH(p.roms, rom(46, true, "Advance Wars (USA).gba",
         "Advance Wars (USA)", ""));
   RBUF_PUSH(p.roms, rom(3335, true, "Final Fight CD (USA).zip",
         "Final Fight CD", ""));
   RBUF_PUSH(p.roms, rom(99, true, "x (USA).md", "X", ""));
   RBUF_PUSH(lib.platforms, p);
   old_index = romm_library_index(&old);
   index     = romm_library_index(&lib);
   CHECK(RBUF_LEN(index) == 4 && index[0]->id == 46 && index[3]->id == 4320,
         "index: %u games", (unsigned)RBUF_LEN(index));

   CHECK(romm_library_follow_game(old_index, index,
         "romm://46/Advance Wars.gba", "Advance Wars", path, sizeof(path),
         label, sizeof(label))
         && !strcmp(path, "romm://46/Advance Wars (USA).gba")
         && !strcmp(label, "Advance Wars (USA)"), "renamed: %s [%s]", path,
         label);
   CHECK(romm_library_follow_game(old_index, index,
         "romm://46/Advance Wars.gba", "My AW", path, sizeof(path),
         label, sizeof(label))
         && !strcmp(path, "romm://46/Advance Wars (USA).gba")
         && !strcmp(label, "My AW"), "user's title: %s [%s]", path, label);
   CHECK(romm_library_follow_game(old_index, index,
         "romm://3335/Final Fight CD (USA).cue", "Final Fight CD", path,
         sizeof(path), label, sizeof(label))
         && !strcmp(path, "romm://3335/Final Fight CD (USA).cue")
         && !strcmp(label, "Final Fight CD"), "disc: %s [%s]", path, label);
   CHECK(romm_library_follow_game(old_index, index,
         "romm://4320/Alundra (USA) (v1.1).m3u", "Alundra", path,
         sizeof(path), label, sizeof(label))
         && !strcmp(path, "romm://4320/Alundra (USA) (v1.1).m3u")
         && !strcmp(label, "Alundra (v1.1)"), "title only: %s [%s]", path,
         label);
   CHECK(!romm_library_follow_game(old_index, index, "romm://7/gone.md",
         "Gone", path, sizeof(path), label, sizeof(label)), "gone kept");
   CHECK(romm_library_follow_game(old_index, index, "romm://99/old.md",
         "Old", path, sizeof(path), label, sizeof(label))
         && !strcmp(path, "romm://99/old.md") && !strcmp(label, "Old"),
         "unknown before: %s [%s]", path, label);
   CHECK(romm_library_follow_game(old_index, index, "/games/x.zip", "X",
         path, sizeof(path), label, sizeof(label))
         && !strcmp(path, "/games/x.zip")
         && romm_library_follow_game(old_index, index, "romm://x/y", "Y",
         path, sizeof(path), label, sizeof(label))
         && !strcmp(path, "romm://x/y"), "not a RomM game: %s", path);
   CHECK(!romm_library_follow_game(NULL, NULL, "romm://46/a.gba", "A",
         path, sizeof(path), label, sizeof(label)), "empty library kept a"
         " game");

   RBUF_FREE(old_index);
   RBUF_FREE(index);
   romm_library_free(&old);
   romm_library_free(&lib);
}

static void test_preferred_cores(void)
{
   const char *const *c = romm_library_preferred_cores("Sony - PlayStation");
   CHECK(c && !strcmp(c[0], "mednafen_psx_hw") && !strcmp(c[1], "swanstation")
         && !c[2], "PlayStation cores");
   c = romm_library_preferred_cores("Sega - Mega-CD - Sega CD");
   CHECK(c && !strcmp(c[0], "genesis_plus_gx") && !c[1], "Sega CD cores");
   CHECK(!romm_library_preferred_cores("Sony - PlayStation 2")
         && !romm_library_preferred_cores("")
         && !romm_library_preferred_cores(NULL), "cores for a system without");
}

static bool genesis_installed;

static bool find_core(const char *const *cores, char *path,
      size_t path_len, char *name, size_t name_len)
{
   size_t i;
   for (i = 0; cores[i]; i++)
      if (genesis_installed && !strcmp(cores[i], "genesis_plus_gx"))
      {
         strlcpy(path, "/cores/genesis_plus_gx_libretro.dylib", path_len);
         strlcpy(name, "Sega - MS/GG/MD/CD (Genesis Plus GX)", name_len);
         return true;
      }
   return false;
}

/* Syncs Genesis: a full fetch of @page the first time, else its ids
 * and @page as the changes. */
static void genesis_sync(sync_result_t *r, const char *ids, const char *page)
{
   memset(r, 0, sizeof(*r));
   sync_start("http://h", "t", TEST_DIR, NULL, NULL, NULL, on_sync, r);
   sync_answer(200, "[{\"id\": 8, \"rom_count\": 2,"
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   if (ids)
      sync_answer(200, ids);
   sync_answer(200, page);
   CHECK(sync_run_out() && r->status == ROMM_OK, "sync: status %d",
         r->status);
}

static void test_sync_default_core(void)
{
   const char *pl = TEST_DIR "/Sega - Mega Drive - Genesis.lpl";
   sync_result_t r;
   char manifest[256];
   char page[1024];
   char s[256];
   char *doc   = NULL;
   int64_t len = 0;

   path_mkdir(TEST_DIR);
   romm_library_manifest_path(manifest, sizeof(manifest), TEST_DIR);
   test_find_core    = find_core;
   genesis_installed = false;

   /* No preferred core installed: the playlist leaves it to RetroArch. */
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 1, "A", "2026-10-02T04:00:00+00:00");
   game_json(page, sizeof(page), 2, "B", "2026-10-02T04:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   genesis_sync(&r, NULL, page);
   CHECK(filestream_read_file(pl, (void**)&doc, &len) && doc
         && lpl_get(doc, len, -1, "default_core_path", s, sizeof(s)) && !*s,
         "default core without one installed: %s", s);
   free(doc);
   doc = NULL;

   /* Genesis Plus GX installed: the next sync sets it, though no game
    * changed. */
   genesis_installed = true;
   genesis_sync(&r, "{\"rom_id_index\": [1, 2]}", "{\"items\": []}");
   if (filestream_read_file(pl, (void**)&doc, &len) && doc)
   {
      CHECK(lpl_get(doc, len, -1, "default_core_path", s, sizeof(s))
            && !strcmp(s, "/cores/genesis_plus_gx_libretro.dylib"),
            "default core path %s", s);
      CHECK(lpl_get(doc, len, -1, "default_core_name", s, sizeof(s))
            && !strcmp(s, "Sega - MS/GG/MD/CD (Genesis Plus GX)"),
            "default core name %s", s);
      CHECK(count_entries(pl) == 2, "entries lost");
      free(doc);
      doc = NULL;
   }

   /* The user picks PicoDrive for the playlist; a game changing in
    * RomM rewrites it with their choice kept. */
   if (filestream_read_file(pl, (void**)&doc, &len) && doc)
   {
      char *p = strstr(doc, "genesis_plus_gx_libretro.dylib");
      char *n = strstr(doc, "Sega - MS/GG/MD/CD (Genesis Plus GX)");
      if (p && n)
      {
         char edited[4096];
         snprintf(edited, sizeof(edited), "%.*spicodrive_libretro.dylib%.*s"
               "PicoDrive%s", (int)(p - doc), doc,
               (int)(n - (p + 30)), p + 30, n + 36);
         filestream_write_file(pl, edited, (int64_t)strlen(edited));
      }
      free(doc);
      doc = NULL;
   }
   strlcpy(page, "{\"items\": [", sizeof(page));
   game_json(page, sizeof(page), 2, "B Renamed", "2026-10-09T04:00:00+00:00");
   strlcat(page, "]}", sizeof(page));
   genesis_sync(&r, "{\"rom_id_index\": [1, 2]}", page);
   if (filestream_read_file(pl, (void**)&doc, &len) && doc)
   {
      CHECK(lpl_get(doc, len, -1, "default_core_path", s, sizeof(s))
            && !strcmp(s, "/cores/picodrive_libretro.dylib"),
            "user's core replaced: %s", s);
      CHECK(lpl_get(doc, len, -1, "default_core_name", s, sizeof(s))
            && !strcmp(s, "PicoDrive"), "user's core name %s", s);
      CHECK(lpl_get(doc, len, 1, "label", s, sizeof(s))
            && !strcmp(s, "B Renamed"), "not rewritten: %s", s);
      free(doc);
      doc = NULL;
   }

   test_find_core = NULL;
   filestream_delete(pl);
   filestream_delete(manifest);
   filestream_delete(TEST_DIR "/romm");
   filestream_delete(TEST_DIR);
}

static void test_core_option_defaults(void)
{
   const char *v = romm_library_core_option_default(
         "genesis_plus_gx_system_bram");
   CHECK(v && !strcmp(v, "per game"), "GPGX BRAM: %s", v ? v : "NULL");
   v = romm_library_core_option_default("reicast_per_content_vmus");
   CHECK(v && !strcmp(v, "VMU A1"), "Flycast VMU: %s", v ? v : "NULL");
   CHECK(!romm_library_core_option_default("genesis_plus_gx_cart_bram")
         && !romm_library_core_option_default("")
         && !romm_library_core_option_default(NULL), "other options");
}

static void sidebar_platform(romm_library_t *lib, const char *playlist,
      const char *family_name, const char *libretro_slug)
{
   romm_library_platform_t p;
   memset(&p, 0, sizeof(p));
   strlcpy(p.playlist, playlist, sizeof(p.playlist));
   strlcpy(p.platform.family_name, family_name,
         sizeof(p.platform.family_name));
   strlcpy(p.platform.libretro_slug, libretro_slug,
         sizeof(p.platform.libretro_slug));
   RBUF_PUSH(lib->platforms, p);
}

static romm_sidebar_system_t *sidebar_systems(void)
{
   romm_library_t lib;
   romm_sidebar_system_t *systems = NULL;

   memset(&lib, 0, sizeof(lib));
   sidebar_platform(&lib, "Sega - Mega Drive - Genesis.lpl", "",
         "Sega - Mega Drive - Genesis");
   sidebar_platform(&lib, "Sega - Mega Drive - Genesis.lpl", "Sega",
         "Sega - Mega Drive - Genesis");
   sidebar_platform(&lib, "Nintendo - Super Nintendo Entertainment System.lpl",
         "", "Nintendo - Super Nintendo Entertainment System");
   sidebar_platform(&lib, "RomM - Nintendo Switch.lpl", "nintendo", "");
   sidebar_platform(&lib, "Nintendo - Game Boy.lpl", "Nintendo",
         "Nintendo - Game Boy");
   sidebar_platform(&lib, "RomM - PlayStation 3.lpl", "", "");
   sidebar_platform(&lib, "RomM - Pipes.lpl", "A|B", "");
   sidebar_platform(&lib, "", "Sega", "Sega - Saturn");
   romm_sidebar_systems(&systems, &lib);
   RBUF_FREE(lib.platforms);
   return systems;
}

static void test_sidebar_systems(void)
{
   romm_sidebar_system_t *systems = sidebar_systems();

   CHECK(RBUF_LEN(systems) == 6, "%u systems", (unsigned)RBUF_LEN(systems));
   if (RBUF_LEN(systems) == 6)
   {
      CHECK(!strcmp(systems[0].playlist, "Sega - Mega Drive - Genesis.lpl")
            && !strcmp(systems[0].maker, "Sega"),
            "shared playlist, maker from the libretro name: %s [%s]",
            systems[0].playlist, systems[0].maker);
      CHECK(!strcmp(systems[1].maker, "Nintendo"),
            "maker from the libretro name: [%s]", systems[1].maker);
      CHECK(!strcmp(systems[2].maker, "nintendo"),
            "RomM's maker as RomM spells it: [%s]", systems[2].maker);
      CHECK(!*systems[4].maker, "no maker: [%s]", systems[4].maker);
      CHECK(!strcmp(systems[5].maker, "A B"), "no '|': [%s]",
            systems[5].maker);
   }
   RBUF_FREE(systems);
}

static void test_sidebar_open(void)
{
   char open[24];

   CHECK(romm_sidebar_is_open("collections|systems", "systems")
         && romm_sidebar_is_open("systems|x", "systems")
         && !romm_sidebar_is_open("systems/Sega", "systems")
         && !romm_sidebar_is_open("systems", "system")
         && !romm_sidebar_is_open("", "systems"),
         "is_open");

   open[0] = '\0';
   romm_sidebar_toggle(open, sizeof(open), "systems");
   CHECK(!strcmp(open, "systems"), "open one: [%s]", open);
   romm_sidebar_toggle(open, sizeof(open), "systems/Sega");
   CHECK(!strcmp(open, "systems|systems/Sega"), "open two: [%s]", open);
   romm_sidebar_toggle(open, sizeof(open), "systems");
   CHECK(!strcmp(open, "systems/Sega"), "close the first: [%s]", open);
   romm_sidebar_toggle(open, sizeof(open), "systems/Nintendo");
   CHECK(!strcmp(open, "systems/Sega"), "no room: [%s]", open);
   strlcpy(open, "a||b|", sizeof(open));
   romm_sidebar_toggle(open, sizeof(open), "b");
   CHECK(!strcmp(open, "a"), "empty keys dropped: [%s]", open);
}

#define SIDEBAR_ROW_TYPE  5
#define SIDEBAR_GROUP_TYPE 77

static file_list_t *sidebar_rows(void)
{
   static const char *rows[][2] = {
      { "Nintendo - Game Boy.lpl", "Game Boy" },
      { "Sega - Mega Drive - Genesis.lpl", "Mega Drive - Genesis" },
      { "RomM - PlayStation 3.lpl", "PlayStation 3" },
      { "Nintendo - Super Nintendo Entertainment System.lpl",
        "Super Nintendo Entertainment System" },
      { "My Own.lpl", "My Own" },
      { "RomM - Nintendo Switch.lpl", "Nintendo Switch" },
   };
   size_t i;
   file_list_t *list = (file_list_t*)calloc(1, sizeof(*list));

   for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++)
   {
      file_list_append(list, rows[i][0], "", SIDEBAR_ROW_TYPE, 0, 99);
      file_list_set_alt_at_offset(list, i, rows[i][1]);
      /* What a menu attaches must move with its row. */
      list->list[i].userdata = strdup(rows[i][1]);
   }
   return list;
}

static void sidebar_check_row(const file_list_t *list, size_t i,
      const char *path, const char *key, const char *name, size_t depth)
{
   const struct item_file *it = &list->list[i];
   if (i >= list->size)
   {
      CHECK(false, "row %u missing", (unsigned)i);
      return;
   }
   if (key)
      CHECK(it->type == SIDEBAR_GROUP_TYPE && !it->path && it->label
            && !strcmp(it->label, key) && it->alt && !strcmp(it->alt, name)
            && it->entry_idx == depth,
            "row %u: group %s [%s] depth %u", (unsigned)i,
            it->label ? it->label : "-", it->alt ? it->alt : "-",
            (unsigned)it->entry_idx);
   else
      CHECK(it->path && !strcmp(it->path, path) && it->type == SIDEBAR_ROW_TYPE
            && it->userdata && !strcmp((char*)it->userdata, it->alt)
            && it->entry_idx == depth,
            "row %u: %s depth %u", (unsigned)i, it->path ? it->path : "-",
            (unsigned)it->entry_idx);
}

static void test_sidebar_group(void)
{
   romm_sidebar_system_t *systems = sidebar_systems();
   romm_sidebar_names_t names;
   file_list_t *list;
   const char **members = NULL;

   names.systems    = "Systems";
   names.other      = "Other";
   names.group_type = SIDEBAR_GROUP_TYPE;

   list = sidebar_rows();
   romm_sidebar_group(list, systems, "collections|systems|systems/nintendo"
         "|systems/Nintendo|systems/", &names);
   CHECK(list->size == 9, "open: %u rows", (unsigned)list->size);
   sidebar_check_row(list, 0, NULL, "systems", "Systems", 0);
   sidebar_check_row(list, 1, NULL, "systems/Nintendo", "Nintendo", 1);
   sidebar_check_row(list, 2, "Nintendo - Game Boy.lpl", NULL, NULL, 2);
   sidebar_check_row(list, 3, "RomM - Nintendo Switch.lpl", NULL, NULL, 2);
   sidebar_check_row(list, 4,
         "Nintendo - Super Nintendo Entertainment System.lpl", NULL, NULL, 2);
   sidebar_check_row(list, 5, NULL, "systems/Sega", "Sega", 1);
   sidebar_check_row(list, 6, NULL, "systems/", "Other", 1);
   sidebar_check_row(list, 7, "RomM - PlayStation 3.lpl", NULL, NULL, 2);
   /* A row RomM didn't write follows, ungrouped. */
   sidebar_check_row(list, 8, "My Own.lpl", NULL, NULL, 0);
   file_list_free(list);

   /* Closed groups drop their rows, which free what was attached. */
   list = sidebar_rows();
   romm_sidebar_group(list, systems, "", &names);
   CHECK(list->size == 2, "closed: %u rows", (unsigned)list->size);
   sidebar_check_row(list, 0, NULL, "systems", "Systems", 0);
   sidebar_check_row(list, 1, "My Own.lpl", NULL, NULL, 0);
   file_list_free(list);

   list = sidebar_rows();
   romm_sidebar_group(list, systems, "systems", &names);
   CHECK(list->size == 5, "makers closed: %u rows", (unsigned)list->size);
   sidebar_check_row(list, 1, NULL, "systems/Nintendo", "Nintendo", 1);
   sidebar_check_row(list, 2, NULL, "systems/Sega", "Sega", 1);
   sidebar_check_row(list, 3, NULL, "systems/", "Other", 1);
   sidebar_check_row(list, 4, "My Own.lpl", NULL, NULL, 0);
   file_list_free(list);

   /* Without a library, the list stays as it was. */
   list = sidebar_rows();
   romm_sidebar_group(list, NULL, "systems", &names);
   CHECK(list->size == 6 && !strcmp(list->list[4].path, "My Own.lpl")
         && list->list[4].entry_idx == 99, "no library: %u rows",
         (unsigned)list->size);
   file_list_free(list);

   romm_sidebar_members(&members, systems, "systems");
   CHECK(RBUF_LEN(members) == 6
         && !strcmp(members[0], "RomM - Pipes.lpl")
         && !strcmp(members[1], "Nintendo - Game Boy.lpl")
         && !strcmp(members[3], "RomM - Nintendo Switch.lpl")
         && !strcmp(members[5], "RomM - PlayStation 3.lpl"),
         "all systems: %u", (unsigned)RBUF_LEN(members));
   romm_sidebar_members(&members, systems, "systems/Sega");
   CHECK(RBUF_LEN(members) == 1
         && !strcmp(members[0], "Sega - Mega Drive - Genesis.lpl"),
         "Sega: %u", (unsigned)RBUF_LEN(members));
   romm_sidebar_members(&members, systems, "systems/");
   CHECK(RBUF_LEN(members) == 1
         && !strcmp(members[0], "RomM - PlayStation 3.lpl"),
         "Other: %u", (unsigned)RBUF_LEN(members));
   romm_sidebar_members(&members, systems, "systemsx");
   CHECK(RBUF_LEN(members) == 0, "not a group: %u",
         (unsigned)RBUF_LEN(members));
   RBUF_FREE(members);
   RBUF_FREE(systems);
}

int main(void)
{
   test_content_path();
   test_core_option_defaults();
   test_playlist_name();
   test_write_playlist();
   test_game_core_kept();
   test_manifest();
   test_sync();
   test_sync_short_page();
   test_sync_reconcile();
   test_cursor();
   test_sync_incremental();
   test_sync_shared_playlist();
   test_follow_game();
   test_preferred_cores();
   test_sync_default_core();
   test_cover_path();
   test_covers();
   test_covers_superseded();
   test_covers_shared_name();
   test_covers_in_steps();
   test_sidebar_systems();
   test_sidebar_open();
   test_sidebar_group();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM library checks passed\n");
   return 0;
}
