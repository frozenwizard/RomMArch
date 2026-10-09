/* Harness for the RomM library (network/romm/romm_library.c): the
 * romm:// paths games get, playlist file names, the playlists written
 * for a platform, read back as JSON, and the manifest's round trip.
 * Then a sync, with the test playing RomM through the client harness's
 * request stubs: the requests made, paging, the playlists and manifest
 * written, and how a failed request or a cancel ends it; playlists of
 * platforms gone from RomM removed, others' left alone, and the
 * manifest after a sync that stopped part way.
 *
 * Includes the library's translation unit so its statics are tested
 * as shipped. Files are written to the current directory and removed.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>

#include "../../../network/romm/romm_library.c"

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

#define TEST_DIR "romm_library_test_dir"

typedef struct
{
   unsigned calls;
   enum romm_status status;
   size_t platforms;
   size_t games;
} sync_result_t;

static void on_sync(void *user_data, enum romm_status status,
      const romm_library_t *lib)
{
   sync_result_t *r = (sync_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status    = status;
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
   task->callback(task, NULL, task->user_data, NULL);
   task->cleanup(task);
   free(task->title);
   free(task);
   stub_task = NULL;
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
   while (stub_task && steps++ < 20)
      if (sync_step())
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
   " \"name\": \"Genesis\", \"libretro_slug\": \"Sega - Mega Drive - Genesis\"},"
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
   CHECK(romm_library_sync("http://romm.local:8090", "rmm_tok", TEST_DIR,
         "Syncing", on_sync, &r) && stub_task, "sync not started");
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
         && r.games == 303, "result: calls %u status %d platforms %u"
         " games %u", r.calls, r.status, (unsigned)r.platforms,
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
            && !strcmp(lib.platforms[2].playlist,
               "Sega - Mega Drive - Genesis.lpl"),
            "manifest Genesis: %u games", (unsigned)RBUF_LEN(
               lib.platforms[1].roms));
   romm_library_free(&lib);

   /* No response part way: reported, the manifest left as it was. */
   memset(&r, 0, sizeof(r));
   CHECK(romm_library_sync("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, on_sync, &r), "second sync not started");
   CHECK(stub_task && (stub_task->flags & RETRO_TASK_FLG_MUTE),
         "untitled sync not muted");
   sync_answer(200, sync_platforms_reply);
   sync_answer(-1, NULL);
   CHECK(sync_run_out() && r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE,
         "unreachable: calls %u status %d", r.calls, r.status);
   CHECK(romm_library_load(&lib, manifest) && RBUF_LEN(lib.platforms) == 3,
         "manifest changed by a failed sync");
   romm_library_free(&lib);

   /* Cancelled while a request is out: it waits for the answer, then
    * stops without another. */
   memset(&r, 0, sizeof(r));
   CHECK(romm_library_sync("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, on_sync, &r), "third sync not started");
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
   CHECK(romm_library_sync("http://romm.local:8090", "rmm_tok", TEST_DIR,
         NULL, on_sync, &r), "fourth sync not started");
   CHECK(sync_run_out() && r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE,
         "refused: calls %u status %d", r.calls, r.status);
   stub_req.refuse = false;

   CHECK(!romm_library_sync("http://h", "", TEST_DIR, NULL, on_sync, &r)
         && !romm_library_sync("", "t", TEST_DIR, NULL, on_sync, &r)
         && !romm_library_sync("http://h", "t", "", NULL, on_sync, &r),
         "sync started without a server, token or folder");
   stub_refuse_task = true;
   CHECK(!romm_library_sync("http://h", "t", TEST_DIR, NULL, on_sync, &r),
         "sync reported started when its task wasn't");
   stub_refuse_task = false;

   filestream_delete(TEST_DIR "/Sega - Mega-CD - Sega CD.lpl");
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
   CHECK(romm_library_sync("http://romm.local:8090", "t", TEST_DIR, NULL,
         on_sync, &r), "sync not started");
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
   CHECK(romm_library_sync("http://romm.local:8090", "t", TEST_DIR, NULL,
         on_sync, &r), "second sync not started");
   sync_answer(200, "[{\"id\": 15, \"rom_count\": 3,"
         " \"libretro_slug\": \"Sega - Mega-CD - Sega CD\"},"
         " {\"id\": 16, \"rom_count\": 1, \"name\": \"SNES\","
         " \"libretro_slug\": \"Nintendo - SNES\"},"
         " {\"id\": 17, \"rom_count\": 1, \"name\": \"New\"}]");
   page = roms_page(15, 1, 3);
   sync_answer(200, page);
   free(page);
   sync_answer(500, "{\"detail\": \"Internal Server Error\"}");
   CHECK(sync_run_out() && r.status == ROMM_ERR_HTTP && r.platforms == 2
         && r.games == 4, "partial: status %d platforms %u games %u",
         r.status, (unsigned)r.platforms, (unsigned)r.games);
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

int main(void)
{
   test_content_path();
   test_playlist_name();
   test_write_playlist();
   test_manifest();
   test_sync();
   test_sync_reconcile();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM library checks passed\n");
   return 0;
}
