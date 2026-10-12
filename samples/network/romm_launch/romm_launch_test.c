/* Harness for preparing a RomM game for launch
 * (network/romm/romm_launch.c), with the test playing RomM through
 * the client harness's request stubs and the files landing in the
 * real romm:// VFS: a game that is one file, a disc folder whose list
 * is made from its cue sheet (one file over a 64 MB range), one with
 * RomM's own .m3u and discs in subfolders, and how a RAR, a deleted
 * game, a folder with nothing to launch, a bad CRC32 and a cancel end
 * it. Also that the previous game is released.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <encodings/crc32.h>
#include <streams/file_stream.h>

#include "../../../network/romm/romm_launch.c"

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

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
} launch_result_t;

static void on_launch(void *user_data, enum romm_status status,
      int http_status)
{
   launch_result_t *r = (launch_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
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

static void start(launch_result_t *r, const char *content_path)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_launch_prepare("http://romm.local:8090", "rmm_tok",
         content_path, "Downloading", on_launch, r) && stub_task,
         "prepare not started for %s", content_path);
}

static void test_rom_id(void)
{
   CHECK(romm_launch_rom_id("romm://314/a.zip") == 314, "id");
   CHECK(!romm_launch_rom_id("romm://x/a.zip") && !romm_launch_rom_id("romm://314")
         && !romm_launch_rom_id("/games/a.zip") && !romm_launch_rom_id(NULL),
         "id from a path without one");
}

static void test_single_file(void)
{
   launch_result_t r;

   retro_vfs_romm_add("romm://1/old game.gb", strdup("old"), 3);

   start(&r, "romm://314/Double Dragon (U) [!].zip");
   CHECK(!path_is_valid("romm://1/old game.gb"), "previous game kept");
   CHECK(!romm_launch_is_ready("romm://314/Double Dragon (U) [!].zip"),
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
   CHECK(run_out() && r.calls == 1 && r.status == ROMM_OK,
         "single: calls %u status %d", r.calls, r.status);
   CHECK(romm_launch_is_ready("romm://314/Double Dragon (U) [!].zip")
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
         && !romm_launch_is_ready("romm://4320/Alundra (USA) (v1.1).m3u"),
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

static void test_refused(void)
{
   launch_result_t r;
   unsigned before;

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
   CHECK(!romm_launch_prepare("http://h", "t", "/games/a.gb", NULL,
            on_launch, &r)
         && !romm_launch_prepare("http://h", "", "romm://1/a.gb", NULL,
            on_launch, &r)
         && !romm_launch_prepare("", "t", "romm://1/a.gb", NULL,
            on_launch, &r), "prepared without a game, token or server");
}

int main(void)
{
   test_rom_id();
   test_single_file();
   test_cue_bin();
   test_romm_m3u();
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
