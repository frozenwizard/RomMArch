/* Harness for preparing a RomM game for launch
 * (network/romm/romm_launch.c), with the test playing RomM through
 * the client harness's request stubs and the files landing in the
 * real romm:// VFS: a game that is one file, a disc folder whose list
 * is made from its cue sheet (one file over a 64 MB range), one with
 * RomM's own .m3u and discs in subfolders, and how a RAR, a deleted
 * game, a file renamed since the last sync, a folder with nothing to
 * launch, a bad CRC32 and a cancel end it. A folder holding a cartridge
 * launches it, chosen by the core's extensions. Zips holding disc images are unpacked (one disc, two discs, the
 * disc a history entry names, a bad CRC32) and stay ready to load as
 * what they held; zipped cartridges are not unpacked.
 * BIOS files the core lacks are fetched into the system folder first,
 * each written whole or not at all.
 * Also that the previous game is released, and that unloading
 * releases every game but the one loading next.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <encodings/crc32.h>
#include <encodings/deflate.h>
#include <streams/file_stream.h>

#include "../../../network/romm/romm_launch.c"

#include "../romm_client/stubs_retroarch.h"

static unsigned failures = 0;

#define TEST_SYSTEM_DIR "romm_launch_test_system"

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

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   char launch_path[256];
} launch_result_t;

static void on_launch(void *user_data, enum romm_status status,
      int http_status, const char *launch_path)
{
   launch_result_t *r = (launch_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   strlcpy(r->launch_path, launch_path ? launch_path : "",
         sizeof(r->launch_path));
}

static bool step(void)
{
   retro_task_t *task = stub_task;

   task->handler(task);
   if (!(task->flags & RETRO_TASK_FLG_FINISHED))
      return false;
   stub_task = NULL;
   task->callback(task, NULL, task->user_data, NULL);
   task->cleanup(task);
   free(task->title);
   free(task);
   return true;
}

/* Answers the request last started with @len bytes of @body (copied
 * to the heap, since a download hands it over). */
static void answer_pending(int status, const void *body, size_t len)
{
   http_transfer_data_t resp;

   memset(&resp, 0, sizeof(resp));
   resp.status = status;
   if (body)
   {
      resp.data = (char*)malloc(len ? len : 1);
      memcpy(resp.data, body, len);
      resp.len  = len;
   }
   stub_req.cb(NULL, &resp, stub_req.user_data,
         status < 0 ? "Connection refused" : NULL);
   free(resp.data);
}

/* Steps until a request starts, then answers it. */
static void answer(int status, const void *body, size_t len)
{
   unsigned before = stub_req.count;
   unsigned steps  = 0;

   while (stub_req.count == before && stub_task && steps++ < 10)
      step();
   CHECK(stub_req.count == before + 1, "no request to answer");
   if (stub_req.count == before + 1)
      answer_pending(status, body, len);
}

static void answer_str(int status, const char *body)
{
   answer(status, body, strlen(body));
}

static bool run_out(void)
{
   unsigned before = stub_req.count;
   unsigned steps  = 0;
   while (stub_task && steps++ < 20)
      if (step())
         return stub_req.count == before;
   return false;
}

/* The romm:// file at @path, as a string. */
static bool vfs_file_is(const char *path, const void *data, size_t len)
{
   void *buf   = NULL;
   int64_t got = 0;
   bool same   = filestream_read_file(path, &buf, &got)
      && got == (int64_t)len && (!len || !memcmp(buf, data, len));
   free(buf);
   return same;
}

static void start_core(launch_result_t *r, const char *content_path,
      const char *const *firmware, size_t firmware_count,
      const char *core_exts)
{
   romm_launch_params_t params;

   memset(&params, 0, sizeof(params));
   params.server         = "http://romm.local:8090";
   params.token          = "rmm_tok";
   params.content_path   = content_path;
   params.title          = "Downloading";
   params.unpack_title   = "Unpacking";
   params.system_dir     = TEST_SYSTEM_DIR;
   params.firmware       = firmware;
   params.firmware_count = firmware_count;
   params.core_exts      = core_exts;
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_launch_prepare(&params, on_launch, r) && stub_task,
         "prepare not started for %s", content_path);
}

static void start_with(launch_result_t *r, const char *content_path,
      const char *const *firmware, size_t firmware_count)
{
   start_core(r, content_path, firmware, firmware_count, NULL);
}

static void start(launch_result_t *r, const char *content_path)
{
   start_with(r, content_path, NULL, 0);
}

static void test_rom_id(void)
{
   CHECK(romm_launch_rom_id("romm://314/a.zip") == 314, "id");
   CHECK(!romm_launch_rom_id("romm://x/a.zip") && !romm_launch_rom_id("romm://314")
         && !romm_launch_rom_id("/games/a.zip") && !romm_launch_rom_id(NULL),
         "id from a path without one");
}

/* Content unloading frees every game but the one loading next. */
static void test_release(void)
{
   retro_vfs_romm_add("romm://5/a.gb", strdup("a"), 1);
   retro_vfs_romm_add("romm://50/b.cue", strdup("b"), 1);
   retro_vfs_romm_add("romm://50/b.bin", strdup("b"), 1);
   romm_launch_release("romm://50/b.m3u");
   CHECK(!path_is_valid("romm://5/a.gb") && path_is_valid("romm://50/b.cue")
         && path_is_valid("romm://50/b.bin"), "kept the wrong game");
   romm_launch_release("/games/c.gb");
   CHECK(!path_is_valid("romm://50/b.cue"), "a local game kept a RomM one");
   retro_vfs_romm_add("romm://5/a.gb", strdup("a"), 1);
   romm_launch_release(NULL);
   CHECK(!path_is_valid("romm://5/a.gb"), "closing kept a game");
}

static void test_single_file(void)
{
   launch_result_t r;

   retro_vfs_romm_add("romm://1/old game.gb", strdup("old"), 3);

   start(&r, "romm://314/Double Dragon (U) [!].zip");
   CHECK(!path_is_valid("romm://1/old game.gb"), "previous game kept");
   CHECK(!romm_launch_is_ready("romm://314/Double Dragon (U) [!].zip", NULL, 0),
         "ready before fetching");

   answer_str(200, "{\"id\": 314, \"platform_id\": 6,"
         " \"fs_name\": \"Double Dragon (U) [!].zip\", \"fs_path\": \"roms/gb/roms\","
         " \"has_simple_single_file\": true, \"files\": [{\"id\": 313,"
         " \"file_name\": \"Double Dragon (U) [!].zip\","
         " \"full_path\": \"roms/gb/roms/Double Dragon (U) [!].zip\","
         " \"file_size_bytes\": 10, \"crc_hash\": \"40a8bf12\"}]}");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/roms/314"),
         "game request %s", stub_req.url);

   /* A zip's CRC in RomM is the ROM inside, so only its size counts. */
   answer(206, "PK\3\4zipped", 10);
   CHECK(strstr(stub_req.url, "/api/roms/314/content/Double%20Dragon%20%28U%29"
         "%20%5B%21%5D.zip?file_ids=313&purpose=play&range_start=0")
         && strstr(stub_req.headers, "Range: bytes=0-9\r\n"),
         "download %s %s", stub_req.url, stub_req.headers);
   CHECK(run_out() && r.calls == 1 && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://314/Double Dragon (U) [!].zip"),
         "single: calls %u status %d path %s", r.calls, r.status,
         r.launch_path);
   CHECK(romm_launch_is_ready("romm://314/Double Dragon (U) [!].zip", NULL, 0)
         && vfs_file_is("romm://314/Double Dragon (U) [!].zip",
            "PK\3\4zipped", 10), "game not in memory");
}

/* PlayStation: a .cue and a .bin over one 64 MB range, CRC32 checked. */
static void test_cue_bin(void)
{
   const size_t bin_size = (size_t)ROMM_LAUNCH_RANGE + 16;
   const char *cue       = "FILE \"Alundra (USA) (v1.1).bin\" BINARY\n";
   uint8_t *bin          = (uint8_t*)malloc(bin_size);
   char reply[2048];
   launch_result_t r;
   size_t i;

   for (i = 0; i < bin_size; i++)
      bin[i] = (uint8_t)(i * 7);
   snprintf(reply, sizeof(reply), "{\"id\": 4320, \"platform_id\": 22,"
         " \"fs_name\": \"Alundra (USA) (v1.1)\", \"fs_path\": \"roms/psx/roms\","
         " \"has_simple_single_file\": false, \"files\": ["
         "{\"id\": 5054, \"file_name\": \"Alundra (USA) (v1.1).bin\","
         " \"full_path\": \"roms/psx/roms/Alundra (USA) (v1.1)/Alundra (USA) (v1.1).bin\","
         " \"file_size_bytes\": %u, \"crc_hash\": \"%08x\"},"
         "{\"id\": 5055, \"file_name\": \"Alundra (USA) (v1.1).cue\","
         " \"full_path\": \"roms/psx/roms/Alundra (USA) (v1.1)/Alundra (USA) (v1.1).cue\","
         " \"file_size_bytes\": %u, \"crc_hash\": \"%08x\"}]}",
         (unsigned)bin_size, encoding_crc32(0, bin, bin_size),
         (unsigned)strlen(cue),
         encoding_crc32(0, (const uint8_t*)cue, strlen(cue)));

   start(&r, "romm://4320/Alundra (USA) (v1.1).m3u");
   answer_str(200, reply);
   answer(206, bin, (size_t)ROMM_LAUNCH_RANGE);
   CHECK(strstr(stub_req.headers, "Range: bytes=0-67108863\r\n") != NULL,
         "first range %s", stub_req.headers);
   CHECK(stub_task && stub_task->progress == 0, "progress before any range");
   answer(206, bin + ROMM_LAUNCH_RANGE, 16);
   CHECK(strstr(stub_req.headers, "Range: bytes=67108864-67108879\r\n")
         != NULL, "second range %s", stub_req.headers);
   CHECK(stub_task && stub_task->progress == 99, "progress %d",
         stub_task ? (int)stub_task->progress : -1);
   answer_str(206, cue);
   CHECK(run_out() && r.status == ROMM_OK, "cue/bin: status %d", r.status);

   CHECK(vfs_file_is("romm://4320/Alundra (USA) (v1.1).bin", bin, bin_size),
         "bin");
   CHECK(vfs_file_is("romm://4320/Alundra (USA) (v1.1).cue", cue, strlen(cue)),
         "cue");
   CHECK(vfs_file_is("romm://4320/Alundra (USA) (v1.1).m3u",
         "Alundra (USA) (v1.1).cue\n", 25), "disc list");

   /* The same bytes with a different CRC32 are refused, and nothing of
    * the game is left behind. */
   snprintf(reply, sizeof(reply), "{\"id\": 4320, \"platform_id\": 22,"
         " \"fs_name\": \"Alundra (USA) (v1.1)\", \"fs_path\": \"roms/psx/roms\","
         " \"has_simple_single_file\": false, \"files\": ["
         "{\"id\": 5055, \"file_name\": \"Alundra (USA) (v1.1).cue\","
         " \"full_path\": \"roms/psx/roms/Alundra (USA) (v1.1)/Alundra (USA) (v1.1).cue\","
         " \"file_size_bytes\": %u, \"crc_hash\": \"%08x\"},"
         "{\"id\": 5054, \"file_name\": \"Alundra (USA) (v1.1).bin\","
         " \"full_path\": \"roms/psx/roms/Alundra (USA) (v1.1)/Alundra (USA) (v1.1).bin\","
         " \"file_size_bytes\": 16, \"crc_hash\": \"deadbeef\"}]}",
         (unsigned)strlen(cue),
         encoding_crc32(0, (const uint8_t*)cue, strlen(cue)));
   start(&r, "romm://4320/Alundra (USA) (v1.1).m3u");
   answer_str(200, reply);
   answer_str(206, cue);
   answer(206, bin, 16);
   CHECK(run_out() && r.status == ROMM_ERR_CHECKSUM, "bad CRC: status %d",
         r.status);
   CHECK(!path_is_valid("romm://4320/Alundra (USA) (v1.1).cue")
         && !romm_launch_is_ready("romm://4320/Alundra (USA) (v1.1).m3u", NULL, 0),
         "half-fetched game left in memory");
   free(bin);
}

/* Dreamcast's D2: RomM's own .m3u names discs in subfolders. */
static void test_romm_m3u(void)
{
   const char *m3u = "D2 Disc 1/D2 Disc 1.gdi\nD2 Disc 2/D2 Disc 2.gdi\n";
   launch_result_t r;

   start(&r, "romm://4940/D2 (2000)(Sega)[!].m3u");
   answer_str(200, "{\"id\": 4940, \"platform_id\": 28,"
         " \"fs_name\": \"D2 (2000)(Sega)[!]\", \"fs_path\": \"roms/dc/roms\","
         " \"has_simple_single_file\": false, \"files\": ["
         "{\"id\": 1, \"file_name\": \"D2 Disc 1.gdi\","
         " \"full_path\": \"roms/dc/roms/D2 (2000)(Sega)[!]/D2 Disc 1/D2 Disc 1.gdi\","
         " \"file_size_bytes\": 3},"
         "{\"id\": 2, \"file_name\": \"D2 Disc 2.gdi\","
         " \"full_path\": \"roms/dc/roms/D2 (2000)(Sega)[!]/D2 Disc 2/D2 Disc 2.gdi\","
         " \"file_size_bytes\": 3},"
         "{\"id\": 3, \"file_name\": \"D2 v1.000.m3u\","
         " \"full_path\": \"roms/dc/roms/D2 (2000)(Sega)[!]/D2 v1.000.m3u\","
         " \"file_size_bytes\": 48}]}");
   answer_str(206, "gd1");
   answer_str(206, "gd2");
   answer_str(206, m3u);
   CHECK(run_out() && r.status == ROMM_OK, "D2: status %d", r.status);
   CHECK(vfs_file_is("romm://4940/D2 Disc 2/D2 Disc 2.gdi", "gd2", 3),
         "disc in a subfolder");
   CHECK(vfs_file_is("romm://4940/D2 v1.000.m3u", m3u, strlen(m3u))
         && vfs_file_is("romm://4940/D2 (2000)(Sega)[!].m3u", m3u, strlen(m3u)),
         "RomM's disc list not used");
}

/* A folder holding a cartridge and its manual launches the cartridge,
 * fetched alone; a lone .bin is a cartridge too, not a disc. A folder
 * with nothing the core takes is refused before any download, and a
 * core that reads no disc list gets the first disc itself. */
static void test_folder_cartridge(void)
{
   static const char gb[]  = "gb|gbc|dmg";
   static const char gpx[] = "mdx|md|smd|gen|bin|cue|iso|sms|bms|gg|sg|68k|sgd|chd|m3u";
   launch_result_t r;
   unsigned before;

   start_core(&r, "romm://70/Pokemon Red (USA).m3u", NULL, 0, gb);
   answer_str(200, "{\"id\": 70, \"fs_name\": \"Pokemon Red (USA)\","
         " \"fs_path\": \"roms/gb/roms\", \"has_simple_single_file\": false,"
         " \"files\": [{\"id\": 1, \"file_name\": \"manual.pdf\","
         " \"full_path\": \"roms/gb/roms/Pokemon Red (USA)/manual.pdf\","
         " \"file_size_bytes\": 9000000},"
         "{\"id\": 2, \"file_name\": \"Pokemon Red (USA).gb\","
         " \"full_path\": \"roms/gb/roms/Pokemon Red (USA)/Pokemon Red (USA).gb\","
         " \"file_size_bytes\": 4}]}");
   answer_str(206, "pkmn");
   CHECK(strstr(stub_req.url, "/content/Pokemon%20Red%20%28USA%29.gb?file_ids=2")
         != NULL, "cartridge request %s", stub_req.url);
   before = stub_req.count;
   CHECK(run_out() && r.status == ROMM_OK && stub_req.count == before
         && !strcmp(r.launch_path, "romm://70/Pokemon Red (USA).gb"),
         "cartridge folder: status %d path %s", r.status, r.launch_path);
   CHECK(vfs_file_is("romm://70/Pokemon Red (USA).gb", "pkmn", 4)
         && !path_is_valid("romm://70/manual.pdf")
         && !path_is_valid("romm://70/Pokemon Red (USA).m3u"),
         "cartridge folder: files in memory");

   start_core(&r, "romm://71/Sonic.m3u", NULL, 0, gpx);
   answer_str(200, "{\"id\": 71, \"fs_name\": \"Sonic\","
         " \"fs_path\": \"roms/genesis/roms\", \"has_simple_single_file\": false,"
         " \"files\": [{\"id\": 1, \"file_name\": \"Sonic.bin\","
         " \"full_path\": \"roms/genesis/roms/Sonic/Sonic.bin\","
         " \"file_size_bytes\": 4}, {\"id\": 2, \"file_name\": \"Sonic.txt\","
         " \"full_path\": \"roms/genesis/roms/Sonic/Sonic.txt\","
         " \"file_size_bytes\": 2}]}");
   answer_str(206, "SEGA");
   CHECK(run_out() && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://71/Sonic.bin")
         && !path_is_valid("romm://71/Sonic.m3u"),
         "lone .bin: status %d path %s", r.status, r.launch_path);

   start_core(&r, "romm://72/Docs.m3u", NULL, 0, gb);
   answer_str(200, "{\"id\": 72, \"fs_name\": \"Docs\","
         " \"fs_path\": \"roms/gb/roms\", \"has_simple_single_file\": false,"
         " \"files\": [{\"id\": 1, \"file_name\": \"manual.pdf\","
         " \"full_path\": \"roms/gb/roms/Docs/manual.pdf\", \"file_size_bytes\": 3},"
         "{\"id\": 2, \"file_name\": \"readme.txt\","
         " \"full_path\": \"roms/gb/roms/Docs/readme.txt\", \"file_size_bytes\": 3}]}");
   before = stub_req.count;
   CHECK(run_out() && r.status == ROMM_ERR_UNSUPPORTED && stub_req.count == before,
         "nothing the core takes: status %d", r.status);

   start_core(&r, "romm://73/Disc Game.m3u", NULL, 0, "cue|bin");
   answer_str(200, "{\"id\": 73, \"fs_name\": \"Disc Game\","
         " \"fs_path\": \"roms/x/roms\", \"has_simple_single_file\": false,"
         " \"files\": [{\"id\": 1, \"file_name\": \"Disc Game.cue\","
         " \"full_path\": \"roms/x/roms/Disc Game/Disc Game.cue\", \"file_size_bytes\": 3},"
         "{\"id\": 2, \"file_name\": \"Disc Game.bin\","
         " \"full_path\": \"roms/x/roms/Disc Game/Disc Game.bin\", \"file_size_bytes\": 3}]}");
   answer_str(206, "cue");
   answer_str(206, "bin");
   CHECK(run_out() && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://73/Disc Game.cue")
         && path_is_valid("romm://73/Disc Game.bin")
         && !path_is_valid("romm://73/Disc Game.m3u"),
         "no disc lists: status %d path %s", r.status, r.launch_path);
}

typedef struct
{
   const char *name;
   const uint8_t *data;
   size_t len;
   bool deflate;
} zip_member_t;

static void put16(uint8_t *p, unsigned v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
   put16(p, v & 0xffff);
   put16(p + 2, v >> 16);
}

/* Raw deflate, as a zip member holds it. */
static uint8_t *deflate_raw(const uint8_t *data, size_t len, size_t *out_len)
{
   size_t cap   = len + len / 8 + 1024;
   uint8_t *out = (uint8_t*)malloc(cap);
   void *z      = rdeflate_new(6, -15);

   *out_len = 0;
   rdeflate_set_in(z, data, len);
   rdeflate_set_out(z, out, cap);
   rdeflate_finish(z);
   for (;;)
   {
      size_t rd = 0, wr = 0;
      int st    = rdeflate_process(z, &rd, &wr);
      *out_len += wr;
      if (st == RDEFLATE_PROCESS_END)
         break;
      if (st == RDEFLATE_PROCESS_ERROR || (!rd && !wr))
      {
         *out_len = 0;
         break;
      }
   }
   rdeflate_free(z);
   return out;
}

/* A zip of @count members; with @bad_crc its directory gives the last
 * member a wrong CRC32. */
static uint8_t *make_zip(const zip_member_t *m, size_t count, bool bad_crc,
      size_t *zip_len)
{
   uint8_t *packed[8];
   size_t plen[8];
   uint32_t crc[8], offset[8];
   size_t i, len = 22, pos = 0, dir;
   uint8_t *zip;

   for (i = 0; i < count; i++)
   {
      crc[i] = encoding_crc32(0, m[i].data, m[i].len);
      if (bad_crc && i == count - 1)
         crc[i] ^= 1;
      if (m[i].deflate)
         packed[i] = deflate_raw(m[i].data, m[i].len, &plen[i]);
      else
      {
         packed[i] = (uint8_t*)malloc(m[i].len + 1);
         memcpy(packed[i], m[i].data, m[i].len);
         plen[i]   = m[i].len;
      }
      len += 30 + 46 + 2 * strlen(m[i].name) + plen[i];
   }
   zip = (uint8_t*)calloc(1, len);
   for (i = 0; i < count; i++)
   {
      size_t nl = strlen(m[i].name);
      offset[i] = (uint32_t)pos;
      put32(zip + pos, 0x04034b50);
      put16(zip + pos + 4, 20);
      put16(zip + pos + 8, m[i].deflate ? 8 : 0);
      put32(zip + pos + 14, crc[i]);
      put32(zip + pos + 18, (uint32_t)plen[i]);
      put32(zip + pos + 22, (uint32_t)m[i].len);
      put16(zip + pos + 26, (unsigned)nl);
      memcpy(zip + pos + 30, m[i].name, nl);
      memcpy(zip + pos + 30 + nl, packed[i], plen[i]);
      pos += 30 + nl + plen[i];
      free(packed[i]);
   }
   dir = pos;
   for (i = 0; i < count; i++)
   {
      size_t nl = strlen(m[i].name);
      put32(zip + pos, 0x02014b50);
      put16(zip + pos + 4, 20);
      put16(zip + pos + 6, 20);
      put16(zip + pos + 10, m[i].deflate ? 8 : 0);
      put32(zip + pos + 16, crc[i]);
      put32(zip + pos + 20, (uint32_t)plen[i]);
      put32(zip + pos + 24, (uint32_t)m[i].len);
      put16(zip + pos + 28, (unsigned)nl);
      put32(zip + pos + 42, offset[i]);
      memcpy(zip + pos + 46, m[i].name, nl);
      pos += 46 + nl;
   }
   put32(zip + pos, 0x06054b50);
   put16(zip + pos + 8, (unsigned)count);
   put16(zip + pos + 10, (unsigned)count);
   put32(zip + pos + 12, (uint32_t)(pos - dir));
   put32(zip + pos + 16, (uint32_t)dir);
   *zip_len = pos + 22;
   return zip;
}

/* RomM's answer for game @id, one zip of @size bytes. */
static void answer_zip_game(unsigned id, const char *zip_name, size_t size)
{
   char reply[1024];
   snprintf(reply, sizeof(reply), "{\"id\": %u, \"platform_id\": 15,"
         " \"fs_name\": \"%s\", \"fs_path\": \"roms/segacd/roms\","
         " \"has_simple_single_file\": true, \"files\": [{\"id\": 7,"
         " \"file_name\": \"%s\", \"full_path\": \"roms/segacd/roms/%s\","
         " \"file_size_bytes\": %u, \"crc_hash\": \"0beca6fd\"}]}",
         id, zip_name, zip_name, zip_name, (unsigned)size);
   answer_str(200, reply);
}

/* Sega CD: a zip holding a .cue and its tracks, one of them over more
 * than one unpacking slice, is unpacked and the .cue launched. */
static void test_zipped_disc(void)
{
   const char *cue      = "FILE \"Game (Track 01).bin\" BINARY\n"
      "FILE \"Game (Track 02).bin\" BINARY\n";
   const size_t t1_size = ROMM_LAUNCH_UNZIP_SLICE + 1000;
   uint8_t *t1          = (uint8_t*)malloc(t1_size);
   zip_member_t m[3];
   launch_result_t r;
   uint8_t *zip;
   size_t zip_len, i;
   char path[256];

   for (i = 0; i < t1_size; i++)
      t1[i] = (uint8_t)((i * 13) ^ (i >> 9));
   m[0].name = "Game (Track 01).bin";
   m[0].data = t1;
   m[0].len  = t1_size;
   m[0].deflate = true;
   m[1].name = "Game (Track 02).bin";
   m[1].data = (const uint8_t*)"audio";
   m[1].len  = 5;
   m[1].deflate = false;
   m[2].name = "Game.cue";
   m[2].data = (const uint8_t*)cue;
   m[2].len  = strlen(cue);
   m[2].deflate = true;
   zip = make_zip(m, 3, false, &zip_len);

   start(&r, "romm://3335/Game.zip");
   answer_zip_game(3335, "Game.zip", zip_len);
   answer(206, zip, zip_len);
   step();
   CHECK(stub_task && stub_task->title
         && !strcmp(stub_task->title, "Unpacking"), "unpacking title");
   CHECK(run_out() && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://3335/Game.cue"),
         "zipped disc: status %d path %s", r.status, r.launch_path);
   CHECK(vfs_file_is("romm://3335/Game (Track 01).bin", t1, t1_size)
         && vfs_file_is("romm://3335/Game (Track 02).bin", "audio", 5)
         && vfs_file_is("romm://3335/Game.cue", cue, strlen(cue)),
         "members not unpacked");
   CHECK(!path_is_valid("romm://3335/Game.zip"), "zip kept");
   CHECK(romm_launch_is_ready("romm://3335/Game.zip", path, sizeof(path))
         && !strcmp(path, "romm://3335/Game.cue"),
         "unpacked zip not ready as its disc: %s", path);
   CHECK(!romm_launch_is_ready("romm://3336/Game.zip", path, sizeof(path)),
         "another game ready");

   /* History names the .cue it launched; the zip comes again. */
   start(&r, "romm://3335/Game.cue");
   answer_zip_game(3335, "Game.zip", zip_len);
   answer(206, zip, zip_len);
   CHECK(run_out() && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://3335/Game.cue")
         && vfs_file_is("romm://3335/Game.cue", cue, strlen(cue)),
         "from history: status %d path %s", r.status, r.launch_path);
   free(zip);

   /* A member that doesn't match its CRC32: nothing is left. */
   zip = make_zip(m, 3, true, &zip_len);
   start(&r, "romm://3335/Game.zip");
   answer_zip_game(3335, "Game.zip", zip_len);
   answer(206, zip, zip_len);
   CHECK(run_out() && r.status == ROMM_ERR_CHECKSUM
         && !path_is_valid("romm://3335/Game (Track 01).bin")
         && !path_is_valid("romm://3335/Game.zip"),
         "bad member: status %d", r.status);
   CHECK(!romm_launch_is_ready("romm://3335/Game.zip", path, sizeof(path)),
         "ready after a failed fetch");
   free(zip);
   free(t1);
}

/* Two discs in one zip launch through a disc list; a zipped cartridge
 * is left zipped. */
static void test_zipped_discs(void)
{
   zip_member_t m[4];
   launch_result_t r;
   uint8_t *zip;
   size_t zip_len;
   char path[256];

   m[0].name = "Disc 1.cue";
   m[1].name = "Disc 1.bin";
   m[2].name = "Disc 2/Disc 2.cue";
   m[3].name = "Disc 2/Disc 2.bin";
   m[0].data = m[1].data = m[2].data = m[3].data = (const uint8_t*)"x";
   m[0].len  = m[1].len  = m[2].len  = m[3].len  = 1;
   m[0].deflate = m[1].deflate = m[2].deflate = m[3].deflate = false;
   zip = make_zip(m, 4, false, &zip_len);
   start(&r, "romm://3336/Two Discs.zip");
   answer_zip_game(3336, "Two Discs.zip", zip_len);
   answer(206, zip, zip_len);
   CHECK(run_out() && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://3336/Two Discs.m3u")
         && vfs_file_is("romm://3336/Two Discs.m3u",
            "Disc 1.cue\nDisc 2/Disc 2.cue\n", 29)
         && path_is_valid("romm://3336/Disc 2/Disc 2.bin"),
         "two discs: status %d path %s", r.status, r.launch_path);
   CHECK(romm_launch_is_ready("romm://3336/Two Discs.zip", path, sizeof(path))
         && !strcmp(path, "romm://3336/Two Discs.m3u"),
         "unpacked discs not ready as their list: %s", path);
   free(zip);

   m[0].name = "Game.gb";
   zip = make_zip(m, 1, false, &zip_len);
   start(&r, "romm://20/Game.zip");
   answer_zip_game(20, "Game.zip", zip_len);
   answer(206, zip, zip_len);
   CHECK(run_out() && r.status == ROMM_OK
         && !strcmp(r.launch_path, "romm://20/Game.zip")
         && vfs_file_is("romm://20/Game.zip", zip, zip_len)
         && !path_is_valid("romm://20/Game.gb"),
         "cartridge: status %d path %s", r.status, r.launch_path);
   CHECK(romm_launch_is_ready("romm://20/Game.zip", path, sizeof(path))
         && !strcmp(path, "romm://20/Game.zip"), "cartridge ready as %s", path);
   free(zip);
}

static void remove_test_system(void)
{
   filestream_delete(TEST_SYSTEM_DIR "/scph5501.bin");
   filestream_delete(TEST_SYSTEM_DIR "/PS1_ROM.BIN");
   filestream_delete(TEST_SYSTEM_DIR "/dc/dc_boot.bin");
   filestream_delete(TEST_SYSTEM_DIR "/dc");
   filestream_delete(TEST_SYSTEM_DIR);
}

static void answer_psx_game(void)
{
   answer_str(200, "{\"id\": 5, \"platform_id\": 22, \"fs_name\": \"Game.bin\","
         " \"fs_path\": \"roms/psx/roms\", \"has_simple_single_file\": true,"
         " \"files\": [{\"id\": 1, \"file_name\": \"Game.bin\","
         " \"full_path\": \"roms/psx/roms/Game.bin\","
         " \"file_size_bytes\": 4}]}");
}

/* The BIOS files the core lacks that RomM has for the platform go to
 * the system folder first, under the core's names, a verified copy
 * preferred. One RomM lacks, one with the wrong CRC32, and a list
 * RomM refuses don't stop the game. */
static void test_firmware(void)
{
   static const char *const wanted[] = {
      "scph5501.bin", "dc/dc_boot.bin", "PS1_ROM.BIN", "missing.bin" };
   launch_result_t r;
   char reply[1024];

   remove_test_system();
   start_with(&r, "romm://5/Game.bin", wanted, ARRAY_SIZE(wanted));
   answer_psx_game();
   snprintf(reply, sizeof(reply), "["
         "{\"id\": 1, \"file_name\": \"scph5501.bin\", \"is_verified\": false,"
         " \"file_size_bytes\": 4, \"crc_hash\": \"%08x\"},"
         "{\"id\": 2, \"file_name\": \"SCPH5501.BIN\", \"is_verified\": true,"
         " \"file_size_bytes\": 4, \"crc_hash\": \"%08x\"},"
         "{\"id\": 3, \"file_name\": \"dc_boot.bin\", \"is_verified\": true,"
         " \"file_size_bytes\": 4, \"crc_hash\": \"%08x\"},"
         "{\"id\": 4, \"file_name\": \"ps1_rom.bin\", \"is_verified\": false,"
         " \"file_size_bytes\": 4, \"crc_hash\": \"deadbeef\"}]",
         encoding_crc32(0, (const uint8_t*)"5500", 4),
         encoding_crc32(0, (const uint8_t*)"5501", 4),
         encoding_crc32(0, (const uint8_t*)"dcbt", 4));
   answer_str(200, reply);
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/firmware?platform_id=22"),
         "firmware list %s", stub_req.url);
   answer_str(200, "5501");
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/firmware/2/content/SCPH5501.BIN"),
         "verified copy not preferred: %s", stub_req.url);
   answer_str(200, "dcbt");
   CHECK(strstr(stub_req.url, "/api/firmware/3/content/dc_boot.bin") != NULL,
         "BIOS in a folder %s", stub_req.url);
   answer_str(200, "ps1r");
   answer_str(206, "game");
   CHECK(strstr(stub_req.url, "/api/roms/5/content/Game.bin") != NULL,
         "game after BIOS %s", stub_req.url);
   CHECK(run_out() && r.status == ROMM_OK, "BIOS: status %d", r.status);
   CHECK(vfs_file_is(TEST_SYSTEM_DIR "/scph5501.bin", "5501", 4)
         && vfs_file_is(TEST_SYSTEM_DIR "/dc/dc_boot.bin", "dcbt", 4),
         "BIOS not written");
   CHECK(!path_is_valid(TEST_SYSTEM_DIR "/PS1_ROM.BIN"),
         "BIOS with the wrong CRC32 written");
   remove_test_system();

   /* A token paired without firmware.read. */
   start_with(&r, "romm://5/Game.bin", wanted, 1);
   answer_psx_game();
   answer_str(403, "{\"detail\": \"Forbidden\"}");
   answer_str(206, "game");
   CHECK(run_out() && r.status == ROMM_OK
         && !path_is_valid(TEST_SYSTEM_DIR "/scph5501.bin"),
         "BIOS list refused: status %d", r.status);

   /* A write that can't finish leaves no BIOS file to be taken as
    * installed: here its temporary file can't be made. */
   path_mkdir(TEST_SYSTEM_DIR "/scph5501.bin.tmp");
   start_with(&r, "romm://5/Game.bin", wanted, 1);
   answer_psx_game();
   snprintf(reply, sizeof(reply), "[{\"id\": 2, \"file_name\": \"scph5501.bin\","
         " \"is_verified\": true, \"file_size_bytes\": 4, \"crc_hash\": \"%08x\"}]",
         encoding_crc32(0, (const uint8_t*)"5501", 4));
   answer_str(200, reply);
   answer_str(200, "5501");
   answer_str(206, "game");
   CHECK(run_out() && r.status == ROMM_OK
         && !path_is_valid(TEST_SYSTEM_DIR "/scph5501.bin"),
         "unfinished BIOS write left a file: status %d", r.status);
   filestream_delete(TEST_SYSTEM_DIR "/scph5501.bin.tmp");
   remove_test_system();

   /* Nothing missing: no list asked for. */
   start_with(&r, "romm://5/Game.bin", NULL, 0);
   answer_psx_game();
   answer_str(206, "game");
   CHECK(strstr(stub_req.url, "/api/roms/5/content/") && run_out()
         && r.status == ROMM_OK, "no BIOS wanted: %s", stub_req.url);
}

static void test_refused(void)
{
   romm_launch_params_t params;
   launch_result_t r;
   unsigned before;
   bool no_game, no_token;

   /* RAR: nothing is downloaded. */
   start(&r, "romm://4318/Alundra (USA) (v1.1).rar");
   answer_str(200, "{\"id\": 4318, \"fs_name\": \"Alundra (USA) (v1.1).rar\","
         " \"fs_path\": \"roms/psx/roms\", \"has_simple_single_file\": true,"
         " \"files\": [{\"id\": 9, \"file_name\": \"Alundra (USA) (v1.1).rar\","
         " \"full_path\": \"roms/psx/roms/Alundra (USA) (v1.1).rar\","
         " \"file_size_bytes\": 100}]}");
   before = stub_req.count;
   CHECK(run_out() && r.status == ROMM_ERR_UNSUPPORTED
         && stub_req.count == before, "RAR: status %d", r.status);

   /* Deleted from RomM, or hidden from this user. */
   start(&r, "romm://77/Gone.gb");
   answer_str(404, "{\"detail\": \"Rom with ID 77 not found\"}");
   CHECK(run_out() && r.status == ROMM_ERR_HTTP && r.http_status == 404,
         "gone: status %d HTTP %d", r.status, r.http_status);

   /* A folder with nothing a core could start from. */
   start(&r, "romm://25/Ace Combat 6 [4E4D07D1].m3u");
   answer_str(200, "{\"id\": 25, \"fs_name\": \"Ace Combat 6 [4E4D07D1]\","
         " \"fs_path\": \"roms/xbox360/console\", \"has_simple_single_file\": false,"
         " \"files\": [{\"id\": 1, \"file_name\": \"002796B1\","
         " \"full_path\": \"roms/xbox360/console/Ace Combat 6 [4E4D07D1]/00004000/002796B1\","
         " \"file_size_bytes\": 45056}]}");
   CHECK(run_out() && r.status == ROMM_ERR_UNSUPPORTED, "nothing to launch: %d",
         r.status);

   /* Renamed in RomM since the last sync: nothing is downloaded. */
   start(&r, "romm://46/Advance Wars.gba");
   answer_str(200, "{\"id\": 46, \"fs_name\": \"Advance Wars (USA).gba\","
         " \"fs_path\": \"roms/gba/roms\", \"has_simple_single_file\": true,"
         " \"files\": [{\"id\": 50, \"file_name\": \"Advance Wars (USA).gba\","
         " \"full_path\": \"roms/gba/roms/Advance Wars (USA).gba\","
         " \"file_size_bytes\": 4}]}");
   before = stub_req.count;
   CHECK(run_out() && r.status == ROMM_ERR_RENAMED && stub_req.count == before,
         "renamed: status %d", r.status);

   /* The same for a zip, whatever it holds. */
   start(&r, "romm://3335/Old Name.zip");
   answer_zip_game(3335, "Game.zip", 10);
   before = stub_req.count;
   CHECK(run_out() && r.status == ROMM_ERR_RENAMED && stub_req.count == before,
         "renamed zip: status %d", r.status);

   /* A name that differs only in case is the same file. */
   start(&r, "romm://46/advance wars (usa).GBA");
   answer_str(200, "{\"id\": 46, \"fs_name\": \"Advance Wars (USA).gba\","
         " \"fs_path\": \"roms/gba/roms\", \"has_simple_single_file\": true,"
         " \"files\": [{\"id\": 50, \"file_name\": \"Advance Wars (USA).gba\","
         " \"full_path\": \"roms/gba/roms/Advance Wars (USA).gba\","
         " \"file_size_bytes\": 4}]}");
   answer_str(206, "awar");
   CHECK(run_out() && r.status == ROMM_OK
         && vfs_file_is("romm://46/advance wars (usa).GBA", "awar", 4),
         "case only: status %d", r.status);

   /* RomM answering for another game. */
   start(&r, "romm://5/a.gb");
   answer_str(200, "{\"id\": 6, \"fs_name\": \"a.gb\", \"has_simple_single_file\": true,"
         " \"files\": [{\"id\": 1, \"file_name\": \"a.gb\", \"file_size_bytes\": 1}]}");
   CHECK(run_out() && r.status == ROMM_ERR_RESPONSE, "other game: %d", r.status);

   /* Cancelled with a download out: waits for it, then stops. */
   start(&r, "romm://314/Double Dragon (U) [!].zip");
   answer_str(200, "{\"id\": 314, \"fs_name\": \"Double Dragon (U) [!].zip\","
         " \"fs_path\": \"roms/gb/roms\", \"has_simple_single_file\": true,"
         " \"files\": [{\"id\": 313, \"file_name\": \"Double Dragon (U) [!].zip\","
         " \"full_path\": \"roms/gb/roms/Double Dragon (U) [!].zip\","
         " \"file_size_bytes\": 4}]}");
   step();
   before = stub_req.count;
   stub_task->flags |= RETRO_TASK_FLG_CANCELLED;
   CHECK(!step(), "finished with a download out");
   answer_pending(206, "abcd", 4);
   CHECK(run_out() && r.status == ROMM_ERR_CANCELLED
         && stub_req.count == before && !path_is_valid(
            "romm://314/Double Dragon (U) [!].zip"),
         "cancel: status %d", r.status);

   memset(&r, 0, sizeof(r));
   memset(&params, 0, sizeof(params));
   params.server       = "http://h";
   params.token        = "t";
   params.content_path = "/games/a.gb";
   no_game             = romm_launch_prepare(&params, on_launch, &r);
   params.content_path = "romm://1/a.gb";
   params.token        = "";
   no_token            = romm_launch_prepare(&params, on_launch, &r);
   params.token        = "t";
   params.server       = "";
   CHECK(!no_game && !no_token && !romm_launch_prepare(&params, on_launch, &r),
         "prepared without a game, token or server");
}

int main(void)
{
   test_rom_id();
   test_release();
   test_single_file();
   test_cue_bin();
   test_romm_m3u();
   test_folder_cartridge();
   test_zipped_disc();
   test_zipped_discs();
   test_firmware();
   test_refused();
   retro_vfs_romm_remove_prefix(ROMM_SCHEME);

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM launch checks passed\n");
   return 0;
}
