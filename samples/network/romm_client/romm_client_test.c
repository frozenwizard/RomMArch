/* Harness for the RomM API client (network/romm/romm_client.c).
 *
 * The test plays the server: each request the client starts is
 * recorded by the stubs, checked, and answered with the response the
 * test chooses. Covers the server address joining, the heartbeat
 * request, and how its replies are read: RomM's version, another web
 * server's page, an HTTP error, and no answer at all. Then the start
 * of device pairing: the request RomM gets, and the code and approval
 * link read from its reply. The token check (whose user a token acts
 * for, and a token RomM turns down). The platform and game lists, read
 * from trimmed RomM replies. Then the wait for approval, with the
 * stubs' clock standing in for time: the poll schedule, slowing down
 * when told, and how approval, denial, expiry and cancellation end it.
 *
 * Includes the client's translation unit so its static JSON reader is
 * tested as shipped.
 */

#include <stdio.h>
#include <string.h>

#include <boolean.h>

#include "../../../network/romm/romm_client.c"

#include "stubs_retroarch.h"

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
   char version[64];
} heartbeat_result_t;

static void on_heartbeat(void *user_data, enum romm_status status,
      int http_status, const char *version)
{
   heartbeat_result_t *r = (heartbeat_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   strlcpy(r->version, version ? version : "(null)", sizeof(r->version));
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_pairing;
   romm_pairing_t pairing;
} pair_start_result_t;

static void on_pair_start(void *user_data, enum romm_status status,
      int http_status, const romm_pairing_t *pairing)
{
   pair_start_result_t *r = (pair_start_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_pairing = pairing != NULL;
   if (pairing)
      r->pairing = *pairing;
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   char username[64];
} whoami_result_t;

static void on_whoami(void *user_data, enum romm_status status,
      int http_status, const char *username)
{
   whoami_result_t *r = (whoami_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   strlcpy(r->username, username, sizeof(r->username));
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   char token[128];
   char device_id[64];
} pair_wait_result_t;

static void on_pair_wait(void *user_data, enum romm_status status,
      const char *token, const char *device_id)
{
   pair_wait_result_t *r = (pair_wait_result_t*)user_data;
   r->calls++;
   r->status = status;
   strlcpy(r->token, token, sizeof(r->token));
   strlcpy(r->device_id, device_id, sizeof(r->device_id));
}

/* Answers the recorded request with @status and @body. -1 is what the
 * HTTP task reports when it got no response; -2 answers with no
 * response data at all. */
static void answer(int status, const char *body)
{
   http_transfer_data_t resp;
   char buf[8192];

   memset(&resp, 0, sizeof(resp));
   resp.status = status;
   if (body)
   {
      strlcpy(buf, body, sizeof(buf));
      resp.data = buf;
      resp.len  = strlen(buf);
   }
   stub_req.cb(NULL, status == -2 ? NULL : &resp, stub_req.user_data,
         status < 0 ? "Connection refused" : NULL);
}

static void test_url(void)
{
   char s[64];

   CHECK(romm_client_url(s, sizeof(s), "http://h:8090", "/api/x")
         && !strcmp(s, "http://h:8090/api/x"), "plain: got %s", s);
   CHECK(romm_client_url(s, sizeof(s), "http://h:8090//", "/api/x")
         && !strcmp(s, "http://h:8090/api/x"), "trailing slashes: got %s", s);
   CHECK(!romm_client_url(s, sizeof(s), "", "/api/x"), "empty server accepted");
   CHECK(!romm_client_url(s, sizeof(s), NULL, "/api/x"), "NULL server accepted");
   CHECK(!romm_client_url(s, sizeof(s), "///", "/api/x"), "slashes-only server accepted");
   CHECK(!romm_client_url(s, 16, "http://h:8090", "/api/x"),
         "truncated URL accepted");
}

static void test_json(void)
{
   /* Decoys: a key that only starts with the wanted one, the wanted
    * keys under another object, and in an array. */
   static const char *doc =
      "{\"SYSTEMS\": {\"VERSION\": \"prefix\"},"
      " \"A\": {\"SYSTEM\": {\"VERSION\": \"nested\"}},"
      " \"SYSTEM\": {\"LIST\": [{\"VERSION\": \"array\"}],"
      "  \"INNER\": {\"X\": 1}, \"VERSION\": \"right\"},"
      " \"VERSION\": \"top\", \"N\": 42}";
   char s[32];

   CHECK(romm_json_get(doc, strlen(doc), "SYSTEM.VERSION", s, sizeof(s))
         && !strcmp(s, "right"), "SYSTEM.VERSION: got %s", s);
   CHECK(romm_json_get(doc, strlen(doc), "VERSION", s, sizeof(s))
         && !strcmp(s, "top"), "VERSION: got %s", s);
   CHECK(romm_json_get(doc, strlen(doc), "N", s, sizeof(s))
         && !strcmp(s, "42"), "number: got %s", s);
   CHECK(!romm_json_get(doc, strlen(doc), "SYSTEM.MISSING", s, sizeof(s)),
         "missing key found");
   CHECK(!romm_json_get(doc, strlen(doc), "SYSTEM", s, sizeof(s)),
         "object returned as a value");
   CHECK(!romm_json_get("{\"SYSTEM\": {\"X\": 1}, \"B\": {\"VERSION\": \"b\"}}",
            44, "SYSTEM.VERSION", s, sizeof(s)),
         "key found in an object after the wanted one closed: %s", s);
   CHECK(!romm_json_get("[{\"VERSION\": \"x\"}]", 18, "VERSION", s, sizeof(s)),
         "key inside a top-level array found");
   CHECK(!romm_json_get("<html>", 6, "VERSION", s, sizeof(s)),
         "HTML parsed as JSON");
}

/* Records what romm_json_items() reports as "key=value;" per member and
 * "|" per object. */
typedef struct
{
   char log[512];
} items_log_t;

static void items_member(void *ctx, const char *key, enum rjson_type type,
      const char *value, size_t len)
{
   items_log_t *l = (items_log_t*)ctx;
   strlcat(l->log, key, sizeof(l->log));
   strlcat(l->log, "=", sizeof(l->log));
   strlcat(l->log, type == RJSON_NULL ? "null"
         : type == RJSON_TRUE ? "true"
         : type == RJSON_FALSE ? "false" : value, sizeof(l->log));
   strlcat(l->log, ";", sizeof(l->log));
}

static void items_end(void *ctx)
{
   strlcat(((items_log_t*)ctx)->log, "|", sizeof(((items_log_t*)ctx)->log));
}

static bool items(const char *doc, const char *key, items_log_t *l)
{
   memset(l, 0, sizeof(*l));
   return romm_json_items(doc, strlen(doc), key, items_member, items_end, l);
}

static void test_json_items(void)
{
   items_log_t l;

   CHECK(items("[{\"id\": 1, \"m\": {\"id\": 9, \"x\": [1, 2]},"
            " \"n\": \"a\", \"l\": [{\"id\": 8}], \"t\": true,"
            " \"f\": false, \"z\": null}, {\"id\": 2}]", NULL, &l)
         && !strcmp(l.log, "id=1;n=a;t=true;f=false;z=null;|id=2;|"),
         "top-level array: %s", l.log);
   CHECK(items("[]", NULL, &l) && !*l.log, "empty array: %s", l.log);
   CHECK(items("[1, [2, 3], {\"id\": 4}, \"s\"]", NULL, &l)
         && !strcmp(l.log, "id=4;|"), "non-object items: %s", l.log);
   CHECK(!items("{\"items\": [{\"id\": 1}]}", NULL, &l),
         "object walked as an array");
   CHECK(items("{\"x\": {\"items\": [{\"id\": 7}]}, \"total\": 2,"
            " \"items\": [{\"id\": 1, \"items\": [{\"id\": 5}]}, {\"id\": 2}],"
            " \"after\": [{\"id\": 3}]}", "items", &l)
         && !strcmp(l.log, "id=1;|id=2;|"), "member array: %s", l.log);
   CHECK(!items("{\"items\": 3, \"other\": [{\"id\": 1}]}", "items", &l),
         "array found under another key: %s", l.log);
   CHECK(!items("[{\"id\": 1}, {\"id\": 2", NULL, &l),
         "truncated array reported complete");
   CHECK(!items("<html>", NULL, &l), "HTML walked as JSON");
}

static void heartbeat(heartbeat_result_t *r)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_heartbeat("http://romm.local:8090/", on_heartbeat, r),
         "heartbeat not started");
   CHECK(stub_req.count == 1, "%u requests started", stub_req.count);
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/heartbeat"),
         "URL %s", stub_req.url);
   CHECK(strstr(stub_req.headers, "Accept: application/json\r\n") != NULL,
         "headers %s", stub_req.headers);
}

static void test_heartbeat(void)
{
   heartbeat_result_t r;

   heartbeat(&r);
   answer(200, "{\"SYSTEM\": {\"VERSION\": \"nightly-bdaac69\","
         " \"GIT_BRANCH\": null, \"SHOW_SETUP_WIZARD\": false},"
         " \"OIDC\": {\"ENABLED\": true}}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.http_status == 200
         && !strcmp(r.version, "nightly-bdaac69"),
         "RomM: calls %u status %d version %s", r.calls, r.status, r.version);

   heartbeat(&r);
   answer(200, "<!DOCTYPE html><html>Welcome to nginx!</html>");
   CHECK(r.calls == 1 && r.status == ROMM_ERR_RESPONSE && !*r.version,
         "web page: status %d version %s", r.status, r.version);

   heartbeat(&r);
   answer(200, "{\"status\": \"ok\"}");
   CHECK(r.calls == 1 && r.status == ROMM_ERR_RESPONSE,
         "JSON without a version: status %d", r.status);

   heartbeat(&r);
   answer(404, "{\"detail\": \"Not Found\"}");
   CHECK(r.calls == 1 && r.status == ROMM_ERR_HTTP && r.http_status == 404,
         "404: status %d HTTP %d", r.status, r.http_status);

   heartbeat(&r);
   answer(-1, NULL);
   CHECK(r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE
         && r.http_status == 0,
         "no response: status %d HTTP %d", r.status, r.http_status);

   heartbeat(&r);
   answer(-2, NULL);
   CHECK(r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE
         && r.http_status == 0,
         "no response data: status %d HTTP %d", r.status, r.http_status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   stub_req.refuse = true;
   CHECK(!romm_client_heartbeat("http://romm.local", on_heartbeat, &r),
         "heartbeat reported started when the request wasn't");
   CHECK(!romm_client_heartbeat("", on_heartbeat, &r),
         "heartbeat started with no server");
   CHECK(r.calls == 0, "callback ran for a request never started");
}

static void whoami(whoami_result_t *r)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_whoami("http://romm.local:8090", "rmm_0123abcd",
         on_whoami, r), "token check not started");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/users/me"),
         "URL %s", stub_req.url);
   CHECK(strstr(stub_req.headers, "Authorization: Bearer rmm_0123abcd\r\n")
         && strstr(stub_req.headers, "Accept: application/json\r\n"),
         "headers %s", stub_req.headers);
}

static void test_whoami(void)
{
   whoami_result_t r;

   whoami(&r);
   answer(200, "{\"id\": 1, \"username\": \"alice\", \"role\": \"admin\","
         " \"oauth_scopes\": [\"me.read\"]}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && !strcmp(r.username, "alice"),
         "user: status %d username %s", r.status, r.username);

   whoami(&r);
   answer(401, "{\"detail\": \"Invalid token\"}");
   CHECK(r.status == ROMM_ERR_UNAUTHORIZED && r.http_status == 401
         && !*r.username, "401: status %d", r.status);

   whoami(&r);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_UNAUTHORIZED, "403: status %d", r.status);

   whoami(&r);
   answer(200, "{\"id\": 1}");
   CHECK(r.status == ROMM_ERR_RESPONSE, "no username: status %d", r.status);

   whoami(&r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE, "no response: status %d", r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_whoami("http://romm.local", "", on_whoami, &r),
         "token check started without a token");
   CHECK(stub_req.count == 0 && r.calls == 0, "request or callback without a token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_list;
   size_t count;
   romm_platform_t platforms[4];
} platforms_result_t;

static void on_platforms(void *user_data, enum romm_status status,
      int http_status, const romm_platform_t *platforms, size_t count)
{
   platforms_result_t *r = (platforms_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_list    = platforms != NULL;
   r->count       = count;
   for (i = 0; i < count && i < 4; i++)
      r->platforms[i] = platforms[i];
}

static void platforms(platforms_result_t *r)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_platforms("http://romm.local:8090/", "rmm_0123abcd",
         on_platforms, r), "platform list not started");
   CHECK(stub_req.count == 1 && !strcmp(stub_req.method, "GET"),
         "%u requests, method %s", stub_req.count, stub_req.method);
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/platforms"),
         "URL %s", stub_req.url);
   CHECK(strstr(stub_req.headers, "Authorization: Bearer rmm_0123abcd\r\n")
         != NULL, "headers %s", stub_req.headers);
}

static void test_platforms(void)
{
   platforms_result_t r;

   /* Trimmed from RomM nightly-bdaac69: firmware entries carry their
    * own id and file names, which mustn't leak into the platform. */
   platforms(&r);
   answer(200, "[{\"id\": 28, \"slug\": \"dc\", \"fs_slug\": \"dc\","
         " \"rom_count\": 29, \"name\": \"Dreamcast\", \"igdb_slug\": \"dc\","
         " \"libretro_slug\": \"Sega - Dreamcast\", \"custom_name\": \"\","
         " \"firmware\": [{\"id\": 2, \"platform_id\": 28,"
         " \"file_name\": \"boot.bin\"}], \"display_name\": \"Dreamcast\"},"
         " {\"id\": 26, \"slug\": \"switch\", \"rom_count\": 81,"
         " \"name\": \"Nintendo Switch\", \"libretro_slug\": null,"
         " \"custom_name\": \"Switch\", \"firmware\": [],"
         " \"display_name\": \"Switch\"},"
         " {\"slug\": \"broken\", \"name\": \"No id\"},"
         " {\"display_name\": \"Mega Drive\", \"id\": 8,"
         " \"name\": \"Sega Mega Drive/Genesis\", \"slug\": \"genesis\","
         " \"libretro_slug\": \"Sega - Mega Drive - Genesis\"}]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_list && r.count == 3,
         "list: calls %u status %d count %u", r.calls, r.status,
         (unsigned)r.count);
   CHECK(r.platforms[0].id == 28 && r.platforms[0].rom_count == 29
         && !strcmp(r.platforms[0].slug, "dc")
         && !strcmp(r.platforms[0].name, "Dreamcast")
         && !strcmp(r.platforms[0].libretro_slug, "Sega - Dreamcast"),
         "dc: %u %u %s %s %s", r.platforms[0].id, r.platforms[0].rom_count,
         r.platforms[0].slug, r.platforms[0].name,
         r.platforms[0].libretro_slug);
   CHECK(r.platforms[1].id == 26 && !*r.platforms[1].libretro_slug
         && !strcmp(r.platforms[1].name, "Switch"),
         "custom name, no libretro name: %s [%s]", r.platforms[1].name,
         r.platforms[1].libretro_slug);
   CHECK(r.platforms[2].id == 8
         && !strcmp(r.platforms[2].name, "Mega Drive"),
         "display name before name: %s", r.platforms[2].name);

   platforms(&r);
   answer(200, "[]");
   CHECK(r.status == ROMM_OK && r.count == 0, "empty: status %d count %u",
         r.status, (unsigned)r.count);

   platforms(&r);
   answer(200, "{\"detail\": \"Not a list\"}");
   CHECK(r.calls == 1 && r.status == ROMM_ERR_RESPONSE && !r.has_list,
         "object: status %d", r.status);

   platforms(&r);
   answer(200, "[{\"id\": 28, \"slug\": \"dc\"}, {\"id\": 6, \"sl");
   CHECK(r.status == ROMM_ERR_RESPONSE && !r.has_list && r.count == 0,
         "truncated: status %d count %u", r.status, (unsigned)r.count);

   platforms(&r);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_UNAUTHORIZED && r.http_status == 403,
         "403: status %d", r.status);

   platforms(&r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && r.http_status == 0,
         "no response: status %d", r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_platforms("http://romm.local", "", on_platforms, &r),
         "platform list started without a token");
   stub_req.refuse = true;
   CHECK(!romm_client_platforms("http://romm.local", "t", on_platforms, &r),
         "platform list reported started when the request wasn't");
   CHECK(r.calls == 0, "callback ran for a request never started");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_list;
   size_t count;
   romm_rom_t roms[4];
} roms_result_t;

static void on_roms(void *user_data, enum romm_status status,
      int http_status, const romm_rom_t *roms, size_t count)
{
   roms_result_t *r = (roms_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_list    = roms != NULL;
   r->count       = count;
   for (i = 0; i < count && i < 4; i++)
      r->roms[i] = roms[i];
}

static void roms(roms_result_t *r, const char *updated_after)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_roms("http://romm.local:8090", "rmm_0123abcd", 15,
         updated_after, 500, 250, on_roms, r), "game list not started");
   CHECK(stub_req.count == 1 && !strcmp(stub_req.method, "GET")
         && strstr(stub_req.headers, "Authorization: Bearer rmm_0123abcd\r\n"),
         "%u requests, method %s, headers %s", stub_req.count,
         stub_req.method, stub_req.headers);
}

/* Trimmed from RomM nightly-bdaac69. The nested objects carry their
 * own ids, names and dates, which mustn't leak into the game. */
static const char *roms_reply =
   "{\"items\": ["
   "{\"id\": 3335, \"igdb_id\": 1664, \"platform_id\": 15,"
   " \"platform_slug\": \"segacd\","
   " \"fs_name\": \"Final Fight CD (USA).zip\","
   " \"fs_name_no_tags\": \"Final Fight CD\","
   " \"fs_name_no_ext\": \"Final Fight CD (USA)\", \"fs_extension\": \"zip\","
   " \"name\": \"Final Fight CD\", \"name_sort_key\": \"final fight cd\","
   " \"metadatum\": {\"rom_id\": 3335, \"genres\": [\"Beat 'em up\"]},"
   " \"igdb_metadata\": {\"platforms\": [{\"igdb_id\": 78, \"name\": \"Sega CD\"}]},"
   " \"path_cover_small\": \"/assets/romm/resources/roms/15/3335/cover/small.png?ts=2026-10-03 00:23:20\","
   " \"path_cover_large\": \"/assets/romm/resources/roms/15/3335/cover/big.png?ts=2026-10-03 00:23:20\","
   " \"crc_hash\": \"0beca6fd\", \"has_simple_single_file\": true,"
   " \"has_nested_single_file\": false, \"has_multiple_files\": false,"
   " \"created_at\": \"2026-10-02T04:37:22+00:00\","
   " \"updated_at\": \"2026-10-03T00:23:20+00:00\", \"missing_from_fs\": false,"
   " \"rom_user\": {\"id\": -1, \"rom_id\": -1,"
   " \"updated_at\": \"2026-10-09T04:02:58+00:00\", \"hidden\": false},"
   " \"files\": [{\"id\": 3613, \"rom_id\": 3335,"
   " \"file_name\": \"Final Fight CD (USA).zip\", \"crc_hash\": \"ffffffff\"}]},"
   "{\"has_simple_single_file\": false, \"id\": 2810, \"platform_id\": 15,"
   " \"fs_name\": \"Lunar - The Silver Star (USA)\","
   " \"fs_name_no_ext\": \"Lunar - The Silver Star (USA)\","
   " \"name\": null, \"crc_hash\": \"\", \"path_cover_large\": null,"
   " \"updated_at\": \"2026-10-02T04:37:22+00:00\"},"
   "{\"id\": 9, \"name\": \"No file name\"}],"
   " \"total\": null, \"limit\": 250, \"offset\": 500,"
   " \"char_index\": {}, \"rom_id_index\": [], \"filter_values\": {}}";

static void test_roms(void)
{
   roms_result_t r;
   const romm_rom_t *g;

   roms(&r, NULL);
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/roms"
            "?platform_ids=15&missing=false&order_by=name&order_dir=asc"
            "&with_char_index=false&with_filter_values=false"
            "&with_rom_id_index=false&with_total=false&limit=250&offset=500"),
         "URL %s", stub_req.url);
   answer(200, roms_reply);
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_list && r.count == 2,
         "list: calls %u status %d count %u", r.calls, r.status,
         (unsigned)r.count);
   g = &r.roms[0];
   CHECK(g->id == 3335 && g->platform_id == 15 && g->single_file
         && !strcmp(g->crc, "0beca6fd")
         && !strcmp(g->fs_name, "Final Fight CD (USA).zip")
         && !strcmp(g->fs_name_no_ext, "Final Fight CD (USA)")
         && !strcmp(g->name, "Final Fight CD")
         && !strcmp(g->cover, "/assets/romm/resources/roms/15/3335/cover/big.png?ts=2026-10-03 00:23:20")
         && !strcmp(g->updated_at, "2026-10-03T00:23:20+00:00"),
         "single file: %u %u %d %s [%s] [%s] [%s] [%s] %s", g->id,
         g->platform_id, g->single_file, g->crc, g->fs_name,
         g->fs_name_no_ext, g->name, g->cover, g->updated_at);
   g = &r.roms[1];
   CHECK(g->id == 2810 && !g->single_file && !*g->crc && !*g->cover
         && !strcmp(g->name, "Lunar - The Silver Star (USA)"),
         "folder: %u %d [%s] [%s] [%s]", g->id, g->single_file, g->crc,
         g->cover, g->name);

   roms(&r, "2026-10-03T00:22:20Z");
   CHECK(strstr(stub_req.url, "&offset=500&updated_after=2026-10-03T00:22:20Z")
         != NULL, "URL %s", stub_req.url);
   answer(200, "{\"items\": [], \"total\": null}");
   CHECK(r.status == ROMM_OK && r.count == 0, "nothing changed: status %d"
         " count %u", r.status, (unsigned)r.count);

   roms(&r, NULL);
   answer(200, "[{\"id\": 1, \"fs_name\": \"a.nes\"}]");
   CHECK(r.status == ROMM_ERR_RESPONSE && !r.has_list,
         "bare array: status %d", r.status);

   roms(&r, NULL);
   answer(401, "{\"detail\": \"Invalid token\"}");
   CHECK(r.status == ROMM_ERR_UNAUTHORIZED, "401: status %d", r.status);

   roms(&r, NULL);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE, "no response: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_roms("http://h", "t", 15,
         "2026-10-03T00:23:20+00:00", 0, 50, on_roms, &r),
         "started with a '+' going into the URL");
   CHECK(!romm_client_roms("http://h", "t", 15, "x&y=1", 0, 50, on_roms, &r),
         "started with an '&' going into the URL");
   CHECK(!romm_client_roms("http://h", "t", 0, NULL, 0, 50, on_roms, &r),
         "started without a platform");
   CHECK(!romm_client_roms("http://h", "", 15, NULL, 0, 50, on_roms, &r),
         "started without a token");
   CHECK(stub_req.count == 0 && r.calls == 0,
         "request or callback for a list never started");
}

static void pair_start(pair_start_result_t *r, const char *web)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_pair_start("http://192.168.1.2:8090", web,
         "6f1c-device", "Alice's \"Mac\"", "1.22.0",
         on_pair_start, r), "pairing not started");
   CHECK(stub_req.count == 1, "%u requests started", stub_req.count);
}

static const char *pair_init_reply =
   "{\"device_code\": \"9c2e6b\", \"user_code\": \"QERK5C2H\","
   " \"verification_path\": \"/pair/device\","
   " \"verification_path_complete\": \"/pair/device?user_code=QERK5C2H\","
   " \"expires_in\": 600, \"interval\": 7}";

static void test_pair_start(void)
{
   pair_start_result_t r;
   char s[256];

   pair_start(&r, "https://romm.example.org/");
   CHECK(!strcmp(stub_req.method, "POST"), "method %s", stub_req.method);
   CHECK(!strcmp(stub_req.url, "http://192.168.1.2:8090/api/auth/device/init"),
         "URL %s", stub_req.url);
   CHECK(!strcmp(stub_req.content_type, "application/json"),
         "content type %s", stub_req.content_type);
   CHECK(romm_json_get(stub_req.body, strlen(stub_req.body),
            "client_device_identifier", s, sizeof(s))
         && !strcmp(s, "6f1c-device"), "device id %s", s);
   CHECK(romm_json_get(stub_req.body, strlen(stub_req.body), "name", s,
            sizeof(s))
         && !strcmp(s, "Alice's \"Mac\""), "name %s in %s", s, stub_req.body);
   CHECK(romm_json_get(stub_req.body, strlen(stub_req.body), "client", s,
            sizeof(s))
         && !strcmp(s, "Rommarch"), "client %s", s);
   CHECK(romm_json_get(stub_req.body, strlen(stub_req.body),
            "client_version", s, sizeof(s))
         && !strcmp(s, "1.22.0"), "client version %s", s);
   CHECK(strstr(stub_req.body, "\"requested_scopes\":[\"me.read\",") != NULL
         && strstr(stub_req.body, "\"assets.write\"") != NULL
         && strstr(stub_req.body, "\"collections.write\"]") != NULL,
         "scopes in %s", stub_req.body);

   answer(201, pair_init_reply);
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_pairing,
         "201: calls %u status %d", r.calls, r.status);
   CHECK(!strcmp(r.pairing.device_code, "9c2e6b"),
         "device code %s", r.pairing.device_code);
   CHECK(!strcmp(r.pairing.user_code, "QERK5C2H"),
         "user code %s", r.pairing.user_code);
   CHECK(!strcmp(r.pairing.approve_url,
            "https://romm.example.org/pair/device?user_code=QERK5C2H"),
         "approve URL %s", r.pairing.approve_url);
   CHECK(r.pairing.interval == 7 && r.pairing.expires_in == 600,
         "interval %u expires in %u", r.pairing.interval,
         r.pairing.expires_in);

   pair_start(&r, "");
   answer(201, "{\"device_code\": \"9c2e6b\", \"user_code\": \"QERK5C2H\","
         " \"verification_path_complete\": \"/pair/device?user_code=QERK5C2H\"}");
   CHECK(r.status == ROMM_OK && !strcmp(r.pairing.approve_url,
            "http://192.168.1.2:8090/pair/device?user_code=QERK5C2H"),
         "no web address: approve URL %s", r.pairing.approve_url);
   CHECK(r.pairing.interval == 5 && r.pairing.expires_in == 600,
         "defaults: interval %u expires in %u", r.pairing.interval,
         r.pairing.expires_in);

   pair_start(&r, NULL);
   answer(201, "{\"device_code\": \"9c2e6b\","
         " \"verification_path_complete\": \"/pair/device\"}");
   CHECK(r.calls == 1 && r.status == ROMM_ERR_RESPONSE && !r.has_pairing,
         "no user code: status %d", r.status);

   pair_start(&r, NULL);
   answer(422, "{\"detail\": [{\"msg\": \"Unknown scopes\"}]}");
   CHECK(r.calls == 1 && r.status == ROMM_ERR_HTTP && r.http_status == 422
         && !r.has_pairing, "422: status %d HTTP %d", r.status, r.http_status);

   pair_start(&r, NULL);
   answer(-1, NULL);
   CHECK(r.calls == 1 && r.status == ROMM_ERR_UNREACHABLE
         && r.http_status == 0, "no response: status %d", r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_pair_start("http://h", NULL, "", "Mac", "1",
         on_pair_start, &r), "started without a device id");
   stub_req.refuse = true;
   CHECK(!romm_client_pair_start("http://h", NULL, "id", "Mac", "1",
         on_pair_start, &r), "reported started when the request wasn't");
   CHECK(r.calls == 0, "callback ran for a request never started");
}

#define SEC 1000000

/* What the queue does with the client's task once the clock reaches
 * its 'when': runs the handler, then the callback and cleanup when it
 * has finished. Returns whether it finished. */
static bool run_task(void)
{
   retro_task_t *task = stub_task;

   if (task->when > stub_now)
      return false;
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

static void pair_wait(pair_wait_result_t *r, unsigned expires_in)
{
   romm_pairing_t pairing;

   memset(&pairing, 0, sizeof(pairing));
   strlcpy(pairing.device_code, "9c2e6b", sizeof(pairing.device_code));
   pairing.interval   = 5;
   pairing.expires_in = expires_in;

   memset(r, 0, sizeof(*r));
   stub_reset();
   stub_task = NULL;
   stub_now  = 1000 * SEC;
   CHECK(romm_client_pair_wait("http://192.168.1.2:8090/", &pairing,
         NULL, on_pair_wait, r), "wait not started");
   CHECK(stub_task != NULL, "no task pushed");
   CHECK(stub_task->flags & RETRO_TASK_FLG_MAIN_THREAD,
         "task doesn't run on the main thread");
   CHECK((stub_task->flags & RETRO_TASK_FLG_MUTE) && !stub_task->title,
         "untitled wait isn't silent");
   CHECK(stub_req.count == 0, "polled before the first interval");
}

/* Moves the clock to the task's next run and expects a poll. */
static void expect_poll(retro_time_t at)
{
   unsigned before = stub_req.count;
   CHECK(stub_task->when == at, "next poll at %lld, expected %lld",
         (long long)(stub_task->when - 1000 * SEC),
         (long long)(at - 1000 * SEC));
   stub_now = stub_task->when;
   CHECK(!run_task(), "finished instead of polling");
   CHECK(stub_req.count == before + 1, "no poll at %lld",
         (long long)(stub_now - 1000 * SEC));
}

static void test_pair_wait(void)
{
   pair_wait_result_t r;
   retro_time_t t0 = 1000 * SEC;

   /* Pending, slowed down, then approved. */
   pair_wait(&r, 600);
   stub_now = t0 + 4 * SEC;
   CHECK(!run_task() && stub_req.count == 0, "polled early");
   expect_poll(t0 + 5 * SEC);
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url,
               "http://192.168.1.2:8090/api/auth/device/token")
         && !strcmp(stub_req.content_type, "application/json")
         && !strcmp(stub_req.body, "{\"device_code\":\"9c2e6b\"}"),
         "poll %s %s %s", stub_req.method, stub_req.url, stub_req.body);
   answer(400, "{\"detail\": \"authorization_pending\"}");
   expect_poll(t0 + 10 * SEC);
   answer(400, "{\"detail\": \"slow_down\"}");
   expect_poll(t0 + 20 * SEC);
   answer(429, "{\"detail\": \"Too many polling attempts.\"}");
   expect_poll(t0 + 35 * SEC);
   answer(200, "{\"access_token\": \"rmm_0123abcd\","
         " \"device_id\": \"5d1e-device\","
         " \"scopes\": [\"me.read\"], \"expires_at\": null}");
   CHECK(r.calls == 0, "callback before the task finished");
   CHECK(stub_task->when <= stub_now, "approval not handled right away");
   CHECK(run_task(), "approved but not finished");
   CHECK(r.calls == 1 && r.status == ROMM_OK
         && !strcmp(r.token, "rmm_0123abcd")
         && !strcmp(r.device_id, "5d1e-device"),
         "approved: calls %u status %d token %s device %s",
         r.calls, r.status, r.token, r.device_id);

   /* A dropped connection is retried at the next interval. */
   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(-1, NULL);
   expect_poll(t0 + 10 * SEC);
   answer(400, "{\"detail\": \"access_denied\"}");
   CHECK(run_task() && r.calls == 1 && r.status == ROMM_ERR_DENIED
         && !*r.token, "denied: status %d token %s", r.status, r.token);

   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(400, "{\"detail\": \"expired_token\"}");
   CHECK(run_task() && r.status == ROMM_ERR_EXPIRED,
         "expired code: status %d", r.status);

   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(500, "Internal Server Error");
   CHECK(run_task() && r.status == ROMM_ERR_HTTP, "500: status %d", r.status);

   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(200, "{\"device_id\": \"5d1e-device\"}");
   CHECK(run_task() && r.status == ROMM_ERR_RESPONSE,
         "approval without a token: status %d", r.status);

   /* Never approved: ends at the deadline without polling again. */
   pair_wait(&r, 12);
   expect_poll(t0 + 5 * SEC);
   answer(400, "{\"detail\": \"authorization_pending\"}");
   expect_poll(t0 + 10 * SEC);
   answer(400, "{\"detail\": \"authorization_pending\"}");
   stub_now = stub_task->when;
   CHECK(run_task() && stub_req.count == 2 && r.status == ROMM_ERR_EXPIRED,
         "deadline: finished with status %d after %u polls",
         r.status, stub_req.count);

   /* Cancelled with a poll out: waits for its answer, then stops. */
   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   stub_task->flags |= RETRO_TASK_FLG_CANCELLED;
   stub_now = stub_task->when;
   CHECK(!run_task(), "finished while its poll was still out");
   answer(400, "{\"detail\": \"authorization_pending\"}");
   stub_now = stub_task->when;
   CHECK(run_task() && stub_req.count == 1 && r.status == ROMM_ERR_CANCELLED,
         "cancelled: status %d after %u polls", r.status, stub_req.count);

   /* A title stays in the task notification while it waits. */
   {
      romm_pairing_t pairing;
      memset(&pairing, 0, sizeof(pairing));
      strlcpy(pairing.device_code, "9c2e6b", sizeof(pairing.device_code));
      pairing.expires_in = 5;
      memset(&r, 0, sizeof(r));
      stub_reset();
      stub_now = t0;
      CHECK(romm_client_pair_wait("http://h", &pairing,
            "Approve this device in RomM with code QERK5C2H",
            on_pair_wait, &r), "titled wait not started");
      CHECK(stub_task->title && !strcmp(stub_task->title,
               "Approve this device in RomM with code QERK5C2H")
            && !(stub_task->flags & RETRO_TASK_FLG_MUTE)
            && stub_task->progress == -1,
            "titled wait: title %s flags %d progress %d",
            stub_task->title ? stub_task->title : "(none)",
            (int)stub_task->flags, (int)stub_task->progress);
      stub_now = stub_task->when;
      CHECK(run_task() && r.status == ROMM_ERR_EXPIRED,
            "titled wait: status %d", r.status);
   }

   memset(&r, 0, sizeof(r));
   stub_refuse_task = true;
   {
      romm_pairing_t pairing;
      memset(&pairing, 0, sizeof(pairing));
      strlcpy(pairing.device_code, "9c2e6b", sizeof(pairing.device_code));
      pairing.expires_in = 600;
      CHECK(!romm_client_pair_wait("http://h", &pairing, NULL, on_pair_wait, &r),
            "reported waiting when the task wasn't pushed");
      pairing.device_code[0] = '\0';
      stub_refuse_task = false;
      CHECK(!romm_client_pair_wait("http://h", &pairing, NULL, on_pair_wait, &r),
            "waiting without a device code");
   }
   CHECK(r.calls == 0, "callback ran for a wait never started");
}

int main(void)
{
   test_url();
   test_json();
   test_json_items();
   test_heartbeat();
   test_whoami();
   test_platforms();
   test_roms();
   test_pair_start();
   test_pair_wait();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM client checks passed\n");
   return 0;
}
