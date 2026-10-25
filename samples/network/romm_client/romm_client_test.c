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
 * from trimmed RomM replies, a platform's ordered game ids, a game's
 * files, a game file downloaded into memory, file downloads, a
 * platform's firmware and its download, and a game's saves and save
 * states, their downloads and uploads. Then the wait for approval,
 * with the stubs' clock standing in for time: the poll schedule,
 * slowing down when told, and how approval, denial, expiry and
 * cancellation end it.
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
   int http_status;
   char token[128];
   char device_id[64];
   char missing_scopes[256];
} pair_wait_result_t;

static void on_pair_wait(void *user_data, enum romm_status status,
      int http_status, const char *token, const char *device_id,
      const char *missing_scopes)
{
   pair_wait_result_t *r = (pair_wait_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   strlcpy(r->token, token, sizeof(r->token));
   strlcpy(r->device_id, device_id, sizeof(r->device_id));
   strlcpy(r->missing_scopes, missing_scopes, sizeof(r->missing_scopes));
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
   CHECK(romm_json_get("{\"a\": true, \"b\": false}", 24, "a", s, sizeof(s))
         && !strcmp(s, "true"), "true: %s", s);
   CHECK(romm_json_get("{\"a\": true, \"b\": false}", 24, "b", s, sizeof(s))
         && !strcmp(s, "false"), "false: %s", s);
   CHECK(!romm_json_get("{\"a\": null}", 11, "a", s, sizeof(s)),
         "null read as a value");
}

static void test_json_list(void)
{
   static const char *doc =
      "{\"metadatum\": {\"genres\": [\"Action\", \"Platform\"],"
      " \"ids\": [3, 7], \"mixed\": [\"a\", {\"x\": \"skip\"},"
      " [\"skip\"], \"b\"], \"none\": [], \"name\": \"n\"},"
      " \"genres\": [\"top\"]}";
   char s[64];

   CHECK(romm_json_get_list(doc, strlen(doc), "metadatum.genres", s,
         sizeof(s), ", ") && !strcmp(s, "Action, Platform"), "genres: %s", s);
   CHECK(romm_json_get_list(doc, strlen(doc), "metadatum.ids", s, sizeof(s),
         "+") && !strcmp(s, "3+7"), "numbers: %s", s);
   CHECK(romm_json_get_list(doc, strlen(doc), "metadatum.mixed", s,
         sizeof(s), ",") && !strcmp(s, "a,b"), "nested skipped: %s", s);
   CHECK(romm_json_get_list(doc, strlen(doc), "metadatum.none", s, sizeof(s),
         ",") && !*s, "empty: %s", s);
   CHECK(romm_json_get_list(doc, strlen(doc), "metadatum.name", s, sizeof(s),
         ",") && !strcmp(s, "n"), "a string: %s", s);
   CHECK(romm_json_get_list(doc, strlen(doc), "genres", s, sizeof(s), ",")
         && !strcmp(s, "top"), "top level: %s", s);
   CHECK(!romm_json_get(doc, strlen(doc), "metadatum.genres", s, sizeof(s)),
         "array read as a value without a separator");
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

   /* With arrays: the values of an array directly in an item, under
    * its key, and nothing deeper. */
   memset(&l, 0, sizeof(l));
   {
      const char *doc = "[{\"id\": 1, \"ids\": [3, \"x\", {\"a\": 1}, [4], true],"
            " \"m\": {\"y\": [5]}, \"n\": \"a\"}, {\"id\": 2, \"ids\": []}]";
      CHECK(romm_json_items_with_arrays(doc, strlen(doc), NULL,
               items_member, items_end, &l)
            && !strcmp(l.log, "id=1;ids=3;ids=x;n=a;|id=2;|"),
            "with arrays: %s", l.log);
   }
}

static void values_value(void *ctx, enum rjson_type type,
      const char *value, size_t len)
{
   items_log_t *l = (items_log_t*)ctx;
   strlcat(l->log, type == RJSON_NULL ? "null" : value, sizeof(l->log));
   strlcat(l->log, ";", sizeof(l->log));
}

static bool values(const char *doc, const char *key, items_log_t *l)
{
   memset(l, 0, sizeof(*l));
   return romm_json_values(doc, strlen(doc), key, values_value, l);
}

static void test_json_values(void)
{
   items_log_t l;

   CHECK(values("{\"items\": [{\"rom_id_index\": [9]}],"
            " \"x\": {\"rom_id_index\": [8]},"
            " \"rom_id_index\": [3, 1, [7], {\"a\": 6}, \"s\", null, 2],"
            " \"total\": 3}", "rom_id_index", &l)
         && !strcmp(l.log, "3;1;s;null;2;"), "values: %s", l.log);
   CHECK(values("{\"rom_id_index\": []}", "rom_id_index", &l) && !*l.log,
         "empty: %s", l.log);
   CHECK(!values("{\"rom_id_index\": 5}", "rom_id_index", &l),
         "not an array");
   CHECK(!values("{\"rom_id_index\": [1, 2", "rom_id_index", &l),
         "truncated array reported complete");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_list;
   size_t count;
   romm_collection_t collections[3];
   unsigned rom_ids[3][4];
   size_t rom_count[3];
} collections_result_t;

static void on_collections(void *user_data, enum romm_status status,
      int http_status, const romm_collection_t *collections, size_t count)
{
   collections_result_t *r = (collections_result_t*)user_data;
   size_t i, j;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_list    = collections != NULL;
   r->count       = count;
   for (i = 0; i < count && i < 3; i++)
   {
      r->collections[i]         = collections[i];
      r->collections[i].rom_ids = NULL;
      r->rom_count[i]           = RBUF_LEN(collections[i].rom_ids);
      for (j = 0; j < r->rom_count[i] && j < 4; j++)
         r->rom_ids[i][j] = collections[i].rom_ids[j];
   }
}

static void collections(collections_result_t *r, bool smart)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_collections("http://romm.local:8090/", "rmm_0123abcd",
         smart, on_collections, r), "collection list not started");
   CHECK(stub_req.count == 1 && !strcmp(stub_req.method, "GET"),
         "%u requests, method %s", stub_req.count, stub_req.method);
   CHECK(!strcmp(stub_req.url, smart
            ? "http://romm.local:8090/api/collections/smart"
            : "http://romm.local:8090/api/collections"),
         "URL %s", stub_req.url);
}

static void test_collections(void)
{
   collections_result_t r;

   /* Shaped like RomM's CollectionSchema; covers are paths, not ids. */
   collections(&r, false);
   answer(200, "[{\"name\": \"Couch Co-op\", \"description\": \"\","
         " \"rom_ids\": [12, 10, 11], \"path_covers_small\": [\"a/1.png\"],"
         " \"is_public\": false, \"is_favorite\": false, \"is_virtual\": false,"
         " \"is_smart\": false, \"id\": 3, \"url_cover\": null,"
         " \"user_id\": 1, \"owner_username\": \"alice\"},"
         " {\"name\": \"Favourites\", \"rom_ids\": [7], \"is_favorite\": true,"
         " \"id\": 4, \"owner_username\": \"alice\"},"
         " {\"name\": \"No id\", \"rom_ids\": [1, 2]},"
         " {\"name\": \"Empty\", \"rom_ids\": [], \"id\": 9,"
         " \"is_public\": true, \"owner_username\": \"alice\"}]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_list && r.count == 3,
         "list: calls %u status %d count %u", r.calls, r.status,
         (unsigned)r.count);
   CHECK(r.collections[0].id == 3
         && !strcmp(r.collections[0].name, "Couch Co-op")
         && !strcmp(r.collections[0].owner, "alice")
         && !r.collections[0].favorite && !r.collections[0].smart
         && !r.collections[0].is_public
         && r.rom_count[0] == 3 && r.rom_ids[0][0] == 12
         && r.rom_ids[0][1] == 10 && r.rom_ids[0][2] == 11,
         "first: %u %s %s %u games", r.collections[0].id,
         r.collections[0].name, r.collections[0].owner,
         (unsigned)r.rom_count[0]);
   CHECK(r.collections[1].favorite && r.rom_count[1] == 1,
         "favorites: %d %u", r.collections[1].favorite,
         (unsigned)r.rom_count[1]);
   CHECK(r.collections[2].id == 9 && r.rom_count[2] == 0
         && r.collections[2].is_public
         && !strcmp(r.collections[2].owner, "alice"),
         "empty: %u %u", r.collections[2].id, (unsigned)r.rom_count[2]);

   collections(&r, true);
   answer(200, "[{\"id\": 5, \"name\": \"Unplayed RPGs\","
         " \"filter_criteria\": {\"genres\": [\"RPG\"], \"ids\": [99]},"
         " \"rom_ids\": [21, 22], \"is_smart\": true}]");
   CHECK(r.status == ROMM_OK && r.count == 1 && r.collections[0].smart
         && r.rom_count[0] == 2 && r.rom_ids[0][1] == 22,
         "smart: status %d count %u smart %d %u games", r.status,
         (unsigned)r.count, r.collections[0].smart, (unsigned)r.rom_count[0]);

   collections(&r, false);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN && !r.has_list,
         "403: status %d", r.status);

   collections(&r, false);
   answer(200, "[{\"id\": 3, \"rom_ids\": [1, 2");
   CHECK(r.status == ROMM_ERR_RESPONSE && !r.has_list,
         "cut short: status %d", r.status);
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
   CHECK(r.status == ROMM_ERR_FORBIDDEN, "403: status %d", r.status);

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
         " \"family_name\": \"Sega\","
         " \"firmware\": [{\"id\": 2, \"platform_id\": 28,"
         " \"file_name\": \"boot.bin\"}], \"display_name\": \"Dreamcast\"},"
         " {\"id\": 26, \"slug\": \"switch\", \"rom_count\": 81,"
         " \"name\": \"Nintendo Switch\", \"libretro_slug\": null,"
         " \"family_name\": null,"
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
         && !strcmp(r.platforms[0].fs_slug, "dc")
         && !strcmp(r.platforms[0].name, "Dreamcast")
         && !strcmp(r.platforms[0].libretro_slug, "Sega - Dreamcast")
         && !strcmp(r.platforms[0].family_name, "Sega"),
         "dc: %u %u %s %s %s %s", r.platforms[0].id, r.platforms[0].rom_count,
         r.platforms[0].slug, r.platforms[0].name,
         r.platforms[0].libretro_slug, r.platforms[0].family_name);
   CHECK(r.platforms[1].id == 26 && !*r.platforms[1].libretro_slug
         && !*r.platforms[1].family_name && !*r.platforms[1].fs_slug
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
   CHECK(r.status == ROMM_ERR_FORBIDDEN && r.http_status == 403,
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
   bool more;
   size_t count;
   romm_rom_t roms[4];
} roms_result_t;

static void on_roms(void *user_data, enum romm_status status,
      int http_status, const romm_rom_t *roms, size_t count, bool more)
{
   roms_result_t *r = (roms_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_list    = roms != NULL;
   r->count       = count;
   r->more        = more;
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
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_list && r.count == 2
         && !r.more, "list: calls %u status %d count %u more %d", r.calls,
         r.status, (unsigned)r.count, r.more);
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

   /* A full page is not the last, though a game was left out of it. */
   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_roms("http://h", "t", 15, NULL, 0, 3, on_roms, &r),
         "page of 3 not started");
   answer(200, roms_reply);
   CHECK(r.status == ROMM_OK && r.count == 2 && r.more,
         "full page: status %d count %u more %d", r.status,
         (unsigned)r.count, r.more);

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

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   size_t len;
   unsigned char data[16];
} download_result_t;

static void on_download(void *user_data, enum romm_status status,
      int http_status, const void *data, size_t len)
{
   download_result_t *r = (download_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->len         = len;
   if (data)
      memcpy(r->data, data, len < 16 ? len : 16);
}

static void test_download(void)
{
   static const unsigned char png[] = { 0x89, 'P', 'N', 'G', 0, 0, 0, 13 };
   http_transfer_data_t resp;
   download_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_download("http://romm.local:8090", "rmm_0123abcd",
         "/assets/romm/resources/roms/15/3335/cover/big.png", on_download, &r),
         "download not started");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/assets/romm/"
         "resources/roms/15/3335/cover/big.png"), "URL %s", stub_req.url);
   CHECK(strstr(stub_req.headers, "Authorization: Bearer rmm_0123abcd\r\n")
         && !strstr(stub_req.headers, "application/json"),
         "headers %s", stub_req.headers);

   /* Binary, NULs included. */
   memset(&resp, 0, sizeof(resp));
   resp.status = 200;
   resp.data   = (char*)png;
   resp.len    = sizeof(png);
   stub_req.cb(NULL, &resp, stub_req.user_data, NULL);
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.len == sizeof(png)
         && !memcmp(r.data, png, sizeof(png)), "PNG: status %d len %u",
         r.status, (unsigned)r.len);

   memset(&r, 0, sizeof(r));
   romm_client_download("http://h", "t", "/x.png", on_download, &r);
   answer(404, "{\"detail\": \"Not Found\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 404 && !r.len,
         "404: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_download("http://h", "t", "/x.png", on_download, &r);
   answer(200, "");
   CHECK(r.status == ROMM_ERR_RESPONSE, "empty: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_download("http://h", "t", "/x.png", on_download, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE, "no response: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_download("http://h", "", "/x.png", on_download, &r)
         && stub_req.count == 0 && r.calls == 0,
         "download started without a token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_list;
   size_t count;
   romm_firmware_t firmware[4];
} firmware_result_t;

static void on_firmware(void *user_data, enum romm_status status,
      int http_status, const romm_firmware_t *firmware, size_t count)
{
   firmware_result_t *r = (firmware_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_list    = firmware != NULL;
   r->count       = count;
   for (i = 0; i < count && i < 4; i++)
      r->firmware[i] = firmware[i];
}

static void firmware(firmware_result_t *r, unsigned platform_id)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_firmware("http://romm.local:8090", "rmm_0123abcd",
         platform_id, on_firmware, r), "firmware list not started");
}

static void test_firmware(void)
{
   firmware_result_t r;
   download_result_t d;

   /* Trimmed from RomM nightly-bdaac69. */
   firmware(&r, 22);
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/firmware?platform_id=22")
         && strstr(stub_req.headers, "Bearer rmm_0123abcd"), "request %s",
         stub_req.url);
   answer(200, "[{\"id\": 101, \"platform_id\": 22,"
         " \"file_name\": \"openbios.bin\", \"file_path\": \"bios/psx\","
         " \"file_size_bytes\": 524288, \"is_verified\": false,"
         " \"crc_hash\": \"\", \"md5_hash\": \"\"},"
         " {\"id\": 115, \"platform_id\": 22, \"file_name\": \"scph5501.bin\","
         " \"file_size_bytes\": 524288, \"is_verified\": true,"
         " \"crc_hash\": \"8d8cb7e4\", \"missing_from_fs\": false},"
         " {\"platform_id\": 22, \"file_name\": \"no id.bin\"},"
         " {\"id\": 9, \"file_name\": \"\"}]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_list && r.count == 2,
         "list: status %d count %u", r.status, (unsigned)r.count);
   CHECK(r.firmware[0].id == 101 && !r.firmware[0].verified
         && r.firmware[0].size == 524288 && !*r.firmware[0].crc
         && !strcmp(r.firmware[0].name, "openbios.bin"), "unverified: %u %s",
         r.firmware[0].id, r.firmware[0].name);
   CHECK(r.firmware[1].id == 115 && r.firmware[1].verified
         && !strcmp(r.firmware[1].crc, "8d8cb7e4")
         && !strcmp(r.firmware[1].name, "scph5501.bin"), "verified: %u %s %s",
         r.firmware[1].id, r.firmware[1].name, r.firmware[1].crc);

   /* A token paired before firmware.read was asked for. */
   firmware(&r, 22);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN && !r.has_list, "403: status %d",
         r.status);

   firmware(&r, 22);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && r.http_status == 0,
         "no response: status %d", r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_firmware("http://h", "t", 0, on_firmware, &r)
         && stub_req.count == 0, "listed firmware for no platform");

   memset(&d, 0, sizeof(d));
   stub_reset();
   CHECK(romm_client_firmware_content("http://romm.local:8090",
         "rmm_0123abcd", 143, "scph9002(7502).bin", on_download, &d)
         && !strcmp(stub_req.url, "http://romm.local:8090/api/firmware/143/"
         "content/scph9002%287502%29.bin"), "firmware URL %s", stub_req.url);
   answer(200, "BIOS");
   CHECK(d.calls == 1 && d.status == ROMM_OK && d.len == 4
         && !memcmp(d.data, "BIOS", 4), "firmware: status %d len %u",
         d.status, (unsigned)d.len);
   stub_reset();
   CHECK(!romm_client_firmware_content("http://h", "t", 0, "a.bin",
         on_download, &d)
         && !romm_client_firmware_content("http://h", "t", 1, "",
         on_download, &d) && stub_req.count == 0,
         "firmware fetched without an id or name");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   bool has_list;
   size_t count;
   unsigned ids[8];
} rom_ids_result_t;

static void on_rom_ids(void *user_data, enum romm_status status,
      int http_status, const unsigned *ids, size_t count)
{
   rom_ids_result_t *r = (rom_ids_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status   = status;
   r->has_list = ids != NULL;
   r->count    = count;
   for (i = 0; i < count && i < 8; i++)
      r->ids[i] = ids[i];
}

static void test_rom_ids(void)
{
   rom_ids_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_rom_ids("http://romm.local:8090", "rmm_0123abcd", 15,
         on_rom_ids, &r), "id list not started");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/roms"
            "?platform_ids=15&missing=false&order_by=name&order_dir=asc"
            "&with_char_index=false&with_filter_values=false"
            "&with_rom_id_index=true&with_total=false&limit=1&offset=0"),
         "URL %s", stub_req.url);
   CHECK(strstr(stub_req.headers, "Bearer rmm_0123abcd") != NULL,
         "headers %s", stub_req.headers);
   /* The page's one game carries ids of its own. */
   answer(200, "{\"items\": [{\"id\": 3335, \"files\": [{\"id\": 3613}]}],"
         " \"total\": 3, \"limit\": 1, \"offset\": 0,"
         " \"char_index\": {}, \"rom_id_index\": [3335, 2810, 41],"
         " \"filter_values\": {}}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.count == 3
         && r.ids[0] == 3335 && r.ids[1] == 2810 && r.ids[2] == 41,
         "ids: status %d count %u", r.status, (unsigned)r.count);

   memset(&r, 0, sizeof(r));
   romm_client_rom_ids("http://h", "t", 15, on_rom_ids, &r);
   answer(200, "{\"items\": [], \"rom_id_index\": []}");
   CHECK(r.status == ROMM_OK && r.has_list == false && r.count == 0,
         "all hidden: status %d count %u", r.status, (unsigned)r.count);

   memset(&r, 0, sizeof(r));
   romm_client_rom_ids("http://h", "t", 15, on_rom_ids, &r);
   answer(200, "{\"items\": []}");
   CHECK(r.status == ROMM_ERR_RESPONSE, "no index: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_rom_ids("http://h", "t", 15, on_rom_ids, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE, "no response: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_rom_ids("http://h", "t", 0, on_rom_ids, &r)
         && !romm_client_rom_ids("http://h", "", 15, on_rom_ids, &r)
         && stub_req.count == 0 && r.calls == 0,
         "id list started without a platform or token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_rom;
   romm_rom_t rom;
   size_t count;
   romm_file_t files[4];
} rom_files_result_t;

static void on_rom_files(void *user_data, enum romm_status status,
      int http_status, const romm_rom_t *rom, const romm_file_t *files,
      size_t count)
{
   rom_files_result_t *r = (rom_files_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_rom     = rom != NULL;
   if (rom)
      r->rom = *rom;
   r->count = count;
   for (i = 0; i < count && i < 4; i++)
      r->files[i] = files[i];
}

static void rom_files(rom_files_result_t *r, unsigned id)
{
   memset(r, 0, sizeof(*r));
   stub_reset();
   CHECK(romm_client_rom_files("http://romm.local:8090", "rmm_0123abcd", id,
         on_rom_files, r), "game not requested");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   bool has;
   romm_rom_details_t d;
} rom_details_result_t;

static void on_rom_details(void *user_data, enum romm_status status,
      int http_status, const romm_rom_details_t *details)
{
   rom_details_result_t *r = (rom_details_result_t*)user_data;
   r->calls++;
   r->status = status;
   r->has    = details != NULL;
   if (details)
      r->d = *details;
}

static void test_rom_details(void)
{
   rom_details_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_rom_details("http://romm.local:8090", "rmm_0123abcd",
         46, on_rom_details, &r), "details not requested");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/roms/46"),
         "request %s", stub_req.url);
   answer(200, "{\"id\": 46, \"name\": \"Advance Wars\","
         " \"summary\": \"Lead the Orange Star army.\","
         " \"fs_size_bytes\": 8388608,"
         " \"metadatum\": {\"genres\": [\"Strategy\", \"Turn-based\"],"
         " \"companies\": [\"Intelligent Systems\", \"Nintendo\"],"
         " \"first_release_date\": 1000598400000,"
         " \"average_rating\": 87.5},"
         " \"regions\": [\"USA\"], \"languages\": [],"
         " \"files\": [{\"id\": 1}]}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has && r.d.id == 46
         && !strcmp(r.d.name, "Advance Wars")
         && !strcmp(r.d.summary, "Lead the Orange Star army.")
         && !strcmp(r.d.genres, "Strategy, Turn-based")
         && !strcmp(r.d.companies, "Intelligent Systems, Nintendo")
         && !strcmp(r.d.regions, "USA") && !*r.d.languages
         && r.d.release_date == 1000598400000LL
         && !strcmp(r.d.rating, "87.5") && r.d.size == 8388608ULL,
         "details: %d [%s] [%s] [%s] [%s] %lld [%s] %llu", r.status,
         r.d.name, r.d.genres, r.d.companies, r.d.regions,
         (long long)r.d.release_date, r.d.rating,
         (unsigned long long)r.d.size);

   /* No metadata at all, as for a game RomM couldn't match. */
   memset(&r, 0, sizeof(r));
   romm_client_rom_details("http://romm.local:8090", "t", 47,
         on_rom_details, &r);
   answer(200, "{\"id\": 47, \"name\": \"x\", \"summary\": null,"
         " \"metadatum\": {\"genres\": [], \"companies\": [],"
         " \"first_release_date\": null, \"average_rating\": null},"
         " \"regions\": [], \"languages\": [], \"fs_size_bytes\": 0}");
   CHECK(r.status == ROMM_OK && r.has && !*r.d.summary && !*r.d.genres
         && !r.d.release_date && !*r.d.rating, "unmatched: [%s] [%s] %lld",
         r.d.summary, r.d.genres, (long long)r.d.release_date);

   memset(&r, 0, sizeof(r));
   romm_client_rom_details("http://romm.local:8090", "t", 48,
         on_rom_details, &r);
   answer(404, "{\"detail\": \"Not found\"}");
   CHECK(r.calls == 1 && r.status != ROMM_OK && !r.has, "404: %d", r.status);
   CHECK(!romm_client_rom_details("http://h", "t", 0, on_rom_details, &r),
         "game 0 requested");
}

static void test_rom_files(void)
{
   rom_files_result_t r;

   /* One file; what's inside the zip carries names and sizes too. */
   rom_files(&r, 3335);
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/roms/3335")
         && strstr(stub_req.headers, "Bearer rmm_0123abcd"), "request %s",
         stub_req.url);
   answer(200, "{\"id\": 3335, \"platform_id\": 15,"
         " \"fs_name\": \"Final Fight CD (USA).zip\","
         " \"fs_name_no_ext\": \"Final Fight CD (USA)\","
         " \"fs_path\": \"roms/segacd/roms\", \"name\": \"Final Fight CD\","
         " \"has_simple_single_file\": true, \"crc_hash\": \"0beca6fd\","
         " \"files\": [{\"id\": 3613, \"rom_id\": 3335,"
         " \"file_name\": \"Final Fight CD (USA).zip\","
         " \"file_path\": \"roms/segacd/roms\","
         " \"file_size_bytes\": 383484647,"
         " \"full_path\": \"roms/segacd/roms/Final Fight CD (USA).zip\","
         " \"crc_hash\": \"0beca6fd\", \"archive_members\": [{\"name\":"
         " \"Track 01.bin\", \"size\": 28209888}]}],"
         " \"rom_user\": {\"id\": 9}}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_rom && r.count == 1,
         "single: status %d count %u", r.status, (unsigned)r.count);
   CHECK(r.rom.id == 3335 && r.rom.platform_id == 15 && r.rom.single_file
         && !strcmp(r.rom.fs_name, "Final Fight CD (USA).zip")
         && !strcmp(r.rom.name, "Final Fight CD"),
         "game: %u %u %d [%s] [%s]", r.rom.id, r.rom.platform_id,
         r.rom.single_file, r.rom.fs_name, r.rom.name);
   CHECK(r.files[0].id == 3613 && r.files[0].size == 383484647ULL
         && !strcmp(r.files[0].crc, "0beca6fd")
         && !strcmp(r.files[0].name, "Final Fight CD (USA).zip")
         && !strcmp(r.files[0].path, "Final Fight CD (USA).zip"),
         "file: %u %llu %s [%s] [%s]", r.files[0].id,
         (unsigned long long)r.files[0].size, r.files[0].crc,
         r.files[0].name, r.files[0].path);

   /* A folder with discs in folders of their own, and a file over 4 GB. */
   rom_files(&r, 4930);
   answer(200, "{\"id\": 4930, \"platform_id\": 28,"
         " \"fs_name\": \"D2 (2000)(Sega)[!]\", \"fs_path\": \"roms/dc/roms\","
         " \"name\": \"D2\", \"has_simple_single_file\": false,"
         " \"files\": ["
         "{\"id\": 1, \"file_name\": \"D2 Disc 1.gdi\","
         " \"full_path\": \"roms/dc/roms/D2 (2000)(Sega)[!]/D2 Disc 1/D2 Disc 1.gdi\","
         " \"file_size_bytes\": 87},"
         "{\"id\": 2, \"file_name\": \"track03.bin\","
         " \"full_path\": \"roms/dc/roms/D2 (2000)(Sega)[!]/D2 Disc 1/track03.bin\","
         " \"file_size_bytes\": 4500000000},"
         "{\"id\": 3, \"file_name\": \"D2.m3u\","
         " \"full_path\": \"roms/dc/roms/D2 (2000)(Sega)[!]/D2.m3u\","
         " \"file_size_bytes\": 408}]}");
   CHECK(r.status == ROMM_OK && !r.rom.single_file && r.count == 3,
         "folder: status %d single %d count %u", r.status,
         r.rom.single_file, (unsigned)r.count);
   CHECK(!strcmp(r.files[0].path, "D2 Disc 1/D2 Disc 1.gdi")
         && !strcmp(r.files[2].path, "D2.m3u"), "paths [%s] [%s]",
         r.files[0].path, r.files[2].path);
   CHECK(r.files[1].size == 4500000000ULL, "large size %llu",
         (unsigned long long)r.files[1].size);

   rom_files(&r, 9);
   answer(404, "{\"detail\": \"Rom with ID 9 not found\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 404 && !r.has_rom,
         "deleted: status %d HTTP %d", r.status, r.http_status);

   rom_files(&r, 9);
   answer(200, "{\"id\": 9, \"fs_name\": \"a.zip\"}");
   CHECK(r.status == ROMM_ERR_RESPONSE, "no files: status %d", r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_rom_files("http://h", "t", 0, on_rom_files, &r)
         && !romm_client_rom_files("http://h", "", 5, on_rom_files, &r)
         && r.calls == 0 && stub_req.count == 0,
         "game requested without an id or token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   void *data;
   size_t len;
} content_result_t;

static void on_content(void *user_data, enum romm_status status,
      int http_status, void *data, size_t len)
{
   content_result_t *r = (content_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->data        = data;
   r->len         = len;
}

static void test_rom_content(void)
{
   http_transfer_data_t resp;
   content_result_t r;
   char *body;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_rom_content("http://romm.local:8090", "rmm_0123abcd",
         3335, 3613, "Final Fight CD (USA) [!] & Co+.zip", 0, 0, on_content, &r)
         != NULL, "download not started");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/roms/3335/"
         "content/Final%20Fight%20CD%20%28USA%29%20%5B%21%5D%20%26%20Co%2B.zip"
         "?file_ids=3613&purpose=play"), "URL %s", stub_req.url);
   CHECK(strstr(stub_req.headers, "Bearer rmm_0123abcd")
         && !strstr(stub_req.headers, "application/json")
         && !strstr(stub_req.headers, "Range:"),
         "headers %s", stub_req.headers);

   /* The body is handed over, not copied, and not freed by the
    * transfer afterwards. */
   body = (char*)malloc(6);
   memcpy(body, "ROM\0\1\2", 6);
   memset(&resp, 0, sizeof(resp));
   resp.status = 200;
   resp.data   = body;
   resp.len    = 6;
   stub_req.cb(NULL, &resp, stub_req.user_data, NULL);
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.data == body && r.len == 6
         && !resp.data && !resp.len, "ownership: status %d data %p len %u",
         r.status, r.data, (unsigned)r.len);
   free(r.data);

   memset(&r, 0, sizeof(r));
   romm_client_rom_content("http://h", "t", 4, 5, "Pok\xc3\xa9mon/x.gb",
         0, 0, on_content, &r);
   CHECK(strstr(stub_req.url, "/content/Pok%C3%A9mon%2Fx.gb?") != NULL,
         "UTF-8 and slash: %s", stub_req.url);
   answer(404, "{\"detail\": \"Not Found\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 404 && !r.data,
         "404: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_rom_content("http://h", "t", 4, 5, "x.gb", 0, 0, on_content, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && !r.data, "no response: %d",
         r.status);

   /* A range: asked for with Range, answered 206. */
   memset(&r, 0, sizeof(r));
   romm_client_rom_content("http://h", "t", 4, 5, "big.bin",
         67108864ULL, 67108864ULL, on_content, &r);
   CHECK(strstr(stub_req.headers, "Range: bytes=67108864-134217727\r\n")
         != NULL, "range header %s", stub_req.headers);
   /* Each range its own URL, or the HTTP task refuses the next one
    * while the last is still queued. */
   CHECK(!strcmp(stub_req.url, "http://h/api/roms/4/content/big.bin"
         "?file_ids=5&purpose=play&range_start=67108864"), "range URL %s",
         stub_req.url);
   body = (char*)malloc(4);
   memset(&resp, 0, sizeof(resp));
   resp.status = 206;
   resp.data   = body;
   resp.len    = 4;
   stub_req.cb(NULL, &resp, stub_req.user_data, NULL);
   CHECK(r.status == ROMM_OK && r.data == body && r.len == 4,
         "206: status %d", r.status);
   free(r.data);

   /* A 200 for a later range would be the whole file, not the part. */
   memset(&r, 0, sizeof(r));
   romm_client_rom_content("http://h", "t", 4, 5, "big.bin", 64, 64,
         on_content, &r);
   answer(200, "whole file");
   CHECK(r.status == ROMM_ERR_HTTP && !r.data, "200 for a later range: %d",
         r.status);

   /* A 200 for the first range of a small file is the file. */
   memset(&r, 0, sizeof(r));
   romm_client_rom_content("http://h", "t", 4, 5, "small.bin", 0, 64,
         on_content, &r);
   body = (char*)malloc(5);
   memcpy(body, "small", 5);
   memset(&resp, 0, sizeof(resp));
   resp.status = 200;
   resp.data   = body;
   resp.len    = 5;
   stub_req.cb(NULL, &resp, stub_req.user_data, NULL);
   CHECK(r.status == ROMM_OK && r.len == 5, "200 for a first range: %d",
         r.status);
   free(r.data);

   /* More than asked for is not the range. */
   memset(&r, 0, sizeof(r));
   romm_client_rom_content("http://h", "t", 4, 5, "big.bin", 0, 2,
         on_content, &r);
   answer(206, "too long");
   CHECK(r.status == ROMM_ERR_RESPONSE && !r.data, "too long: %d", r.status);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_rom_content("http://h", "t", 0, 5, "x", 0, 0, on_content, &r)
         && !romm_client_rom_content("http://h", "t", 4, 0, "x", 0, 0, on_content, &r)
         && !romm_client_rom_content("http://h", "t", 4, 5, "", 0, 0, on_content, &r)
         && !romm_client_rom_content("http://h", "", 4, 5, "x", 0, 0, on_content, &r),
         "download started without a game, file, name or token");
   stub_req.refuse = true;
   CHECK(!romm_client_rom_content("http://h", "t", 4, 5, "x", 0, 0, on_content, &r),
         "download reported started when the request wasn't");
   CHECK(r.calls == 0, "callback for a download never started");
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
#if defined(_WIN32)
   const char *os = "Windows";
#elif defined(__APPLE__)
   const char *os = "macOS";
#else
   const char *os = "Linux";
#endif

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
   CHECK(romm_json_get(stub_req.body, strlen(stub_req.body), "platform", s,
            sizeof(s))
         && !strcmp(s, os), "platform %s", s);
   CHECK(romm_json_get(stub_req.body, strlen(stub_req.body),
            "client_version", s, sizeof(s))
         && !strcmp(s, "1.22.0"), "client version %s", s);
   CHECK(strstr(stub_req.body, "\"requested_scopes\":[\"me.read\",") != NULL
         && strstr(stub_req.body, "\"assets.write\"") != NULL
         && strstr(stub_req.body, "\"devices.read\",\"devices.write\"]")
               != NULL,
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
         " \"scopes\": [\"assets.read\", \"assets.write\","
         " \"collections.read\", \"collections.write\","
         " \"devices.read\", \"devices.write\", \"firmware.read\","
         " \"me.read\", \"platforms.read\", \"roms.read\","
         " \"roms.user.read\", \"roms.user.write\"],"
         " \"expires_at\": null}");
   CHECK(r.calls == 0, "callback before the task finished");
   CHECK(stub_task->when <= stub_now, "approval not handled right away");
   CHECK(run_task(), "approved but not finished");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.http_status == 200
         && !strcmp(r.token, "rmm_0123abcd")
         && !strcmp(r.device_id, "5d1e-device") && !*r.missing_scopes,
         "approved: calls %u status %d token %s device %s missing %s",
         r.calls, r.status, r.token, r.device_id, r.missing_scopes);

   /* The approver unticked two scopes; RomM sorts what it grants. */
   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(200, "{\"access_token\": \"rmm_0123abcd\","
         " \"device_id\": \"5d1e-device\","
         " \"scopes\": [\"assets.read\", \"collections.read\","
         " \"collections.write\", \"devices.read\", \"firmware.read\","
         " \"me.read\", \"platforms.read\", \"roms.read\","
         " \"roms.user.read\", \"roms.user.write\"],"
         " \"expires_at\": null}");
   CHECK(run_task() && r.status == ROMM_OK
         && !strcmp(r.missing_scopes, "assets.write, devices.write"),
         "unticked: status %d missing %s", r.status, r.missing_scopes);

   /* A reply that doesn't list scopes says nothing is missing. */
   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(200, "{\"access_token\": \"rmm_0123abcd\","
         " \"device_id\": \"5d1e-device\"}");
   CHECK(run_task() && r.status == ROMM_OK && !*r.missing_scopes,
         "no scope list: status %d missing %s", r.status, r.missing_scopes);

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
   answer(502, "Bad Gateway");
   CHECK(run_task() && r.status == ROMM_ERR_HTTP && r.http_status == 502,
         "502: status %d HTTP %d", r.status, r.http_status);

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

   /* Cancelled between polls: ends at once, without polling again. */
   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   answer(400, "{\"detail\": \"authorization_pending\"}");
   romm_client_pair_cancel(stub_task);
   CHECK(run_task() && stub_req.count == 1 && r.calls == 1
         && r.status == ROMM_ERR_CANCELLED && !*r.token,
         "cancelled between polls: status %d after %u polls",
         r.status, stub_req.count);

   /* Cancelled with a poll out: waits for its answer, then stops. */
   pair_wait(&r, 600);
   expect_poll(t0 + 5 * SEC);
   romm_client_pair_cancel(stub_task);
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

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_list;
   size_t count;
   romm_save_t saves[4];
} saves_result_t;

static void on_saves(void *user_data, enum romm_status status,
      int http_status, const romm_save_t *saves, size_t count)
{
   saves_result_t *r = (saves_result_t*)user_data;
   size_t i;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_list    = saves != NULL;
   r->count       = count;
   for (i = 0; i < count && i < 4; i++)
      r->saves[i] = saves[i];
}

static void test_saves(void)
{
   static const unsigned char sram[] = { 0, 'S', 0, 'M', 0xff };
   http_transfer_data_t resp;
   saves_result_t r;
   download_result_t d;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_saves("http://romm.local:8090", "rmm_0123abcd", 3335,
         on_saves, &r), "save list not started");
   CHECK(!strcmp(stub_req.url, "http://romm.local:8090/api/saves?rom_id=3335")
         && strstr(stub_req.headers, "Bearer rmm_0123abcd"), "request %s",
         stub_req.url);
   /* RomM's SaveSchema, trimmed: one save replaced in place, one in the
    * web player's autosave slot with its screenshot and device list,
    * whose members must not be read as the save's. */
   answer(200, "[{\"id\": 12, \"rom_id\": 3335, \"user_id\": 1,"
         " \"file_name\": \"Super Metroid (USA).srm\","
         " \"file_name_no_ext\": \"Super Metroid (USA)\","
         " \"file_extension\": \"srm\", \"file_size_bytes\": 8192,"
         " \"emulator\": \"Snes9x\", \"slot\": null,"
         " \"content_hash\": \"0f343b0931126a20f133d67c2b018a3b\","
         " \"updated_at\": \"2026-10-12T20:15:00+00:00\","
         " \"screenshot\": null, \"device_syncs\": []},"
         " {\"id\": 13, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-12_21-00-00].srm\","
         " \"file_size_bytes\": 8192, \"emulator\": \"snes9x\","
         " \"slot\": \"autosave\", \"content_hash\": null,"
         " \"updated_at\": \"2026-10-12T21:00:00+00:00\","
         " \"screenshot\": {\"id\": 7, \"rom_id\": 3335,"
         " \"file_name\": \"shot.png\", \"updated_at\": \"x\"},"
         " \"device_syncs\": [{\"device_id\": \"d1\","
         " \"last_synced_at\": \"2026-10-12T21:00:00+00:00\","
         " \"is_current\": true}]},"
         " {\"rom_id\": 3335, \"file_name\": \"no id.srm\"}]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_list && r.count == 2,
         "list: status %d count %u", r.status, (unsigned)r.count);
   CHECK(r.saves[0].id == 12 && r.saves[0].rom_id == 3335
         && r.saves[0].size == 8192
         && !strcmp(r.saves[0].file_name, "Super Metroid (USA).srm")
         && !strcmp(r.saves[0].emulator, "Snes9x") && !*r.saves[0].slot
         && !strcmp(r.saves[0].content_hash,
               "0f343b0931126a20f133d67c2b018a3b")
         && !strcmp(r.saves[0].updated_at, "2026-10-12T20:15:00+00:00"),
         "save: %u %s %s %s", r.saves[0].id, r.saves[0].file_name,
         r.saves[0].emulator, r.saves[0].updated_at);
   CHECK(r.saves[1].id == 13 && !strcmp(r.saves[1].slot, "autosave")
         && !*r.saves[1].content_hash
         && !strcmp(r.saves[1].file_name,
               "Super Metroid (USA) [2026-10-12_21-00-00].srm")
         && !strcmp(r.saves[1].updated_at, "2026-10-12T21:00:00+00:00"),
         "slotted save: %u %s slot %s at %s", r.saves[1].id,
         r.saves[1].file_name, r.saves[1].slot, r.saves[1].updated_at);

   memset(&r, 0, sizeof(r));
   stub_reset();
   romm_client_saves("http://h", "t", 3335, on_saves, &r);
   answer(200, "[]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.count == 0,
         "none: status %d count %u", r.status, (unsigned)r.count);

   /* A token paired before assets.read was asked for. */
   memset(&r, 0, sizeof(r));
   romm_client_saves("http://h", "t", 3335, on_saves, &r);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN && !r.has_list, "403: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   romm_client_saves("http://h", "t", 3335, on_saves, &r);
   answer(200, "<html>proxy</html>");
   CHECK(r.status == ROMM_ERR_RESPONSE && !r.has_list, "HTML: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   romm_client_saves("http://h", "t", 3335, on_saves, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && r.http_status == 0,
         "no response: status %d", r.status);

   /* States: same reply shape, without slot or hash. */
   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_states("http://romm.local:8090", "rmm_0123abcd", 3335,
         on_saves, &r), "state list not started");
   CHECK(!strcmp(stub_req.url,
         "http://romm.local:8090/api/states?rom_id=3335"), "request %s",
         stub_req.url);
   answer(200, "[{\"id\": 40, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"file_size_bytes\": 431020, \"emulator\": \"Snes9x\","
         " \"core\": null, \"updated_at\": \"2026-10-12T20:16:01+00:00\","
         " \"screenshot\": {\"id\": 8,"
         " \"file_name\": \"Super Metroid (USA).state1.png\"}}]");
   CHECK(r.status == ROMM_OK && r.count == 1 && r.saves[0].id == 40
         && r.saves[0].size == 431020 && !*r.saves[0].slot
         && !strcmp(r.saves[0].file_name, "Super Metroid (USA).state1")
         && !strcmp(r.saves[0].emulator, "Snes9x"),
         "state: status %d count %u %s", r.status, (unsigned)r.count,
         r.saves[0].file_name);

   /* Contents: a save fetched with this device's id is recorded as
    * synced by RomM; binary, NULs included. */
   memset(&d, 0, sizeof(d));
   stub_reset();
   CHECK(romm_client_save_content("http://romm.local:8090", "rmm_0123abcd",
         12, "4e4c54f7-8573-4246-95bf-4f11890b27e6", on_download, &d)
         && !strcmp(stub_req.url, "http://romm.local:8090/api/saves/12/"
         "content?device_id=4e4c54f7-8573-4246-95bf-4f11890b27e6")
         && strstr(stub_req.headers, "Bearer rmm_0123abcd"),
         "save URL %s", stub_req.url);
   memset(&resp, 0, sizeof(resp));
   resp.status = 200;
   resp.data   = (char*)sram;
   resp.len    = sizeof(sram);
   stub_req.cb(NULL, &resp, stub_req.user_data, NULL);
   CHECK(d.calls == 1 && d.status == ROMM_OK && d.len == sizeof(sram)
         && !memcmp(d.data, sram, sizeof(sram)), "save: status %d len %u",
         d.status, (unsigned)d.len);
   stub_reset();
   CHECK(romm_client_save_content("http://h", "t", 12, NULL, on_download, &d)
         && !strcmp(stub_req.url, "http://h/api/saves/12/content"),
         "save without a device %s", stub_req.url);
   memset(&d, 0, sizeof(d));
   answer(404, "{\"detail\": \"Save file not found on disk\"}");
   CHECK(d.status == ROMM_ERR_HTTP && d.http_status == 404, "404: status %d",
         d.status);
   stub_reset();
   CHECK(romm_client_state_content("http://h", "t", 40, on_download, &d)
         && !strcmp(stub_req.url, "http://h/api/states/40/content"),
         "state URL %s", stub_req.url);

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(!romm_client_saves("http://h", "t", 0, on_saves, &r)
         && !romm_client_save_content("http://h", "t", 0, NULL, on_download,
               &d)
         && !romm_client_state_content("http://h", "t", 0, on_download, &d)
         && !romm_client_states("http://h", "t", 0, on_saves, &r)
         && !romm_client_saves("http://h", "", 3335, on_saves, &r)
         && stub_req.count == 0 && r.calls == 0,
         "listed or fetched without an id or a token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   bool has_save;
   romm_save_t save;
} upload_result_t;

static void on_upload(void *user_data, enum romm_status status,
      int http_status, const romm_save_t *save)
{
   upload_result_t *r = (upload_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->has_save    = save != NULL;
   if (save)
      r->save = *save;
}

/* Checks the recorded request's body is one form part holding @len
 * bytes of @data as file @file_name in field @field. */
static bool form_holds(const char *field, const char *file_name,
      const void *data, size_t len)
{
   char boundary[96];
   char head[1024];
   char tail[128];
   const char *b = strstr(stub_req.content_type, "boundary=");
   size_t head_len, tail_len;

   if (     strncmp(stub_req.content_type, "multipart/form-data; ", 21)
         || !b)
      return false;
   strlcpy(boundary, b + 9, sizeof(boundary));
   head_len = (size_t)snprintf(head, sizeof(head),
         "--%s\r\nContent-Disposition: form-data; name=\"%s\";"
         " filename=\"%s\"\r\nContent-Type: application/octet-stream"
         "\r\n\r\n", boundary, field, file_name);
   tail_len = (size_t)snprintf(tail, sizeof(tail), "\r\n--%s--\r\n",
         boundary);
   return stub_req.body_len == head_len + len + tail_len
       && !memcmp(stub_req.body, head, head_len)
       && !memcmp(stub_req.body + head_len, data, len)
       && !memcmp(stub_req.body + head_len + len, tail, tail_len);
}

static void test_upload(void)
{
   static const unsigned char sram[] = { 0, 'S', 0, 'M', 0xff, '\r', '\n' };
   romm_upload_t up;
   upload_result_t r;

   memset(&up, 0, sizeof(up));
   up.server       = "http://romm.local:8090";
   up.token        = "rmm_0123abcd";
   up.rom_id       = 3335;
   up.file_name    = "Super Metroid (USA).srm";
   up.data         = sram;
   up.len          = sizeof(sram);
   up.emulator     = "Snes9x";
   up.slot         = "autosave";
   up.keep         = 10;
   up.device_id    = "e409ecd7-f8f1-458e-bf82-09c5f8604113";
   up.content_hash = "0f343b0931126a20f133d67c2b018a3b";

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_save_upload(&up, on_upload, &r), "upload not started");
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url, "http://romm.local:8090/api/saves"
            "?rom_id=3335&emulator=Snes9x&slot=autosave"
            "&device_id=e409ecd7-f8f1-458e-bf82-09c5f8604113"
            "&content_hash=0f343b0931126a20f133d67c2b018a3b"
            "&autocleanup=true&autocleanup_limit=10"),
         "request %s %s", stub_req.method, stub_req.url);
   CHECK(strstr(stub_req.headers, "Authorization: Bearer rmm_0123abcd\r\n")
         && strstr(stub_req.headers, "application/json"),
         "headers %s", stub_req.headers);
   CHECK(form_holds("saveFile", "Super Metroid (USA).srm", sram,
         sizeof(sram)), "body (%u bytes, %s) %s",
         (unsigned)stub_req.body_len, stub_req.content_type, stub_req.body);

   /* RomM's reply, trimmed; the screenshot's members aren't the save's. */
   answer(200, "{\"id\": 21, \"rom_id\": 3335, \"user_id\": 1,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-12_21-30-00].srm\","
         " \"file_size_bytes\": 7, \"emulator\": \"Snes9x\","
         " \"slot\": \"autosave\","
         " \"content_hash\": \"0f343b0931126a20f133d67c2b018a3b\","
         " \"updated_at\": \"2026-10-13T02:30:00+00:00\","
         " \"screenshot\": {\"id\": 7, \"file_name\": \"shot.png\"},"
         " \"device_syncs\": [{\"device_id\": \"e409\","
         " \"is_current\": true}]}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_save
         && r.save.id == 21 && r.save.rom_id == 3335 && r.save.size == 7
         && !strcmp(r.save.file_name,
               "Super Metroid (USA) [2026-10-12_21-30-00].srm")
         && !strcmp(r.save.slot, "autosave")
         && !strcmp(r.save.emulator, "Snes9x")
         && !strcmp(r.save.content_hash, "0f343b0931126a20f133d67c2b018a3b")
         && !strcmp(r.save.updated_at, "2026-10-13T02:30:00+00:00"),
         "uploaded: status %d id %u %s at %s", r.status, r.save.id,
         r.save.file_name, r.save.updated_at);

   /* Another client wrote to the slot since this device last synced. */
   memset(&r, 0, sizeof(r));
   romm_client_save_upload(&up, on_upload, &r);
   answer(409, "{\"detail\": \"Slot has a newer save since your last"
         " sync\"}");
   CHECK(r.status == ROMM_ERR_CONFLICT && r.http_status == 409
         && !r.has_save, "409: status %d", r.status);
   /* Sent over it anyway, the user having chosen this device's. */
   up.overwrite = true;
   stub_reset();
   CHECK(romm_client_save_upload(&up, on_upload, &r)
         && !strcmp(stub_req.url, "http://romm.local:8090/api/saves"
            "?rom_id=3335&emulator=Snes9x&slot=autosave"
            "&device_id=e409ecd7-f8f1-458e-bf82-09c5f8604113"
            "&content_hash=0f343b0931126a20f133d67c2b018a3b"
            "&autocleanup=true&autocleanup_limit=10&overwrite=true"),
         "overwrite %s", stub_req.url);
   answer(200, "{\"id\": 22, \"rom_id\": 3335, \"file_name\": \"x.srm\"}");
   up.overwrite = false;

   /* A token paired without assets.write or devices.write. */
   memset(&r, 0, sizeof(r));
   romm_client_save_upload(&up, on_upload, &r);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN, "403: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_save_upload(&up, on_upload, &r);
   answer(400, "{\"detail\": \"Invalid emulator name: x\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 400,
         "400: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_save_upload(&up, on_upload, &r);
   answer(200, "<html>proxy</html>");
   CHECK(r.status == ROMM_ERR_RESPONSE && !r.has_save, "HTML: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   romm_client_save_upload(&up, on_upload, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && r.http_status == 0,
         "no response: status %d", r.status);

   /* Plain replace: no slot, device or hash; names escaped. */
   up.slot         = NULL;
   up.device_id    = NULL;
   up.content_hash = NULL;
   up.emulator     = "Genesis Plus GX";
   up.file_name    = "Say \"Hi\" \\ Bye.brm";
   stub_reset();
   CHECK(romm_client_save_upload(&up, on_upload, &r)
         && !strcmp(stub_req.url, "http://romm.local:8090/api/saves"
            "?rom_id=3335&emulator=Genesis%20Plus%20GX"),
         "plain upload %s", stub_req.url);
   CHECK(form_holds("saveFile", "Say \\\"Hi\\\" \\\\ Bye.brm", sram,
         sizeof(sram)), "quoted name in %s", stub_req.body);

   /* A state: the save-only fields don't apply. */
   up.emulator     = "Snes9x";
   up.file_name    = "Super Metroid (USA).state1";
   up.slot         = "autosave";
   up.device_id    = "e409ecd7-f8f1-458e-bf82-09c5f8604113";
   up.content_hash = "0f343b0931126a20f133d67c2b018a3b";
   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_state_upload(&up, on_upload, &r)
         && !strcmp(stub_req.url, "http://romm.local:8090/api/states"
            "?rom_id=3335&emulator=Snes9x"), "state upload %s",
         stub_req.url);
   CHECK(form_holds("stateFile", "Super Metroid (USA).state1", sram,
         sizeof(sram)), "state body %s", stub_req.body);
   answer(200, "{\"id\": 40, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA).state1\","
         " \"file_size_bytes\": 7, \"emulator\": \"Snes9x\","
         " \"updated_at\": \"2026-10-13T02:31:00+00:00\"}");
   CHECK(r.status == ROMM_OK && r.save.id == 40 && !*r.save.slot,
         "state: status %d id %u", r.status, r.save.id);

   /* An empty file is still a file. */
   up.data = NULL;
   up.len  = 0;
   stub_reset();
   CHECK(romm_client_state_upload(&up, on_upload, &r)
         && form_holds("stateFile", "Super Metroid (USA).state1", "", 0),
         "empty file body %s", stub_req.body);

   memset(&r, 0, sizeof(r));
   stub_reset();
   up.data  = sram;
   up.len   = sizeof(sram);
   up.token = "";
   CHECK(!romm_client_save_upload(&up, on_upload, &r), "uploaded without"
         " a token");
   up.token  = "t";
   up.rom_id = 0;
   CHECK(!romm_client_save_upload(&up, on_upload, &r), "uploaded without"
         " a game");
   up.rom_id    = 3335;
   up.file_name = "";
   CHECK(!romm_client_state_upload(&up, on_upload, &r), "uploaded without"
         " a name");
   CHECK(!romm_client_save_upload(NULL, on_upload, &r)
         && stub_req.count == 0 && r.calls == 0, "upload started");
}

static void test_save_downloaded(void)
{
   upload_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_save_downloaded("http://romm.local:8090/",
         "rmm_0123abcd", 21, "e409ecd7-f8f1-458e-bf82-09c5f8604113",
         "0f343b0931126a20f133d67c2b018a3b", on_upload, &r),
         "confirmation not started");
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url,
            "http://romm.local:8090/api/saves/21/downloaded")
         && !strcmp(stub_req.content_type, "application/json")
         && strstr(stub_req.headers,
            "Authorization: Bearer rmm_0123abcd\r\n"),
         "request %s %s (%s) %s", stub_req.method, stub_req.url,
         stub_req.content_type, stub_req.headers);
   CHECK(!strcmp(stub_req.body,
         "{\"device_id\":\"e409ecd7-f8f1-458e-bf82-09c5f8604113\","
         "\"content_hash\":\"0f343b0931126a20f133d67c2b018a3b\"}"),
         "body %s", stub_req.body);
   answer(200, "{\"id\": 21, \"rom_id\": 3335,"
         " \"file_name\": \"Super Metroid (USA) [2026-10-12_21-30-00].srm\","
         " \"slot\": \"autosave\","
         " \"updated_at\": \"2026-10-13T02:30:00+00:00\"}");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.has_save
         && r.save.id == 21
         && !strcmp(r.save.updated_at, "2026-10-13T02:30:00+00:00"),
         "confirmed: status %d id %u", r.status, r.save.id);

   /* No hash: RomM then records the version without one. */
   stub_reset();
   romm_client_save_downloaded("http://h", "t", 21, "e409", NULL,
         on_upload, &r);
   CHECK(!strcmp(stub_req.body, "{\"device_id\":\"e409\"}"), "body %s",
         stub_req.body);

   /* A token without devices.write; a device RomM doesn't know. */
   memset(&r, 0, sizeof(r));
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN && !r.has_save, "403: status %d",
         r.status);
   memset(&r, 0, sizeof(r));
   romm_client_save_downloaded("http://h", "t", 21, "e409", NULL,
         on_upload, &r);
   answer(404, "{\"detail\": \"Device with ID e409 not found\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 404,
         "404: status %d", r.status);

   stub_reset();
   CHECK(!romm_client_save_downloaded("http://h", "t", 0, "e409", NULL,
            on_upload, &r)
         && !romm_client_save_downloaded("http://h", "t", 21, "", NULL,
            on_upload, &r)
         && !romm_client_save_downloaded("http://h", "", 21, "e409", NULL,
            on_upload, &r)
         && stub_req.count == 0, "confirmation started without a save,"
         " device or token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
} done_result_t;

static void on_done(void *user_data, enum romm_status status,
      int http_status)
{
   done_result_t *r = (done_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
}

static void test_device_delete(void)
{
   done_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_device_delete("http://romm.local:8090/", "rmm_0123abcd",
         "6f1c-dev ice", on_done, &r), "delete not started");
   CHECK(!strcmp(stub_req.method, "DELETE")
         && !strcmp(stub_req.url,
            "http://romm.local:8090/api/devices/6f1c-dev%20ice")
         && strstr(stub_req.headers, "Authorization: Bearer rmm_0123abcd\r\n"),
         "request %s %s [%s]", stub_req.method, stub_req.url,
         stub_req.headers);
   answer(204, "");
   CHECK(r.calls == 1 && r.status == ROMM_OK, "204: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_device_delete("http://h", "t", "d", on_done, &r);
   answer(404, "{\"detail\": \"Device with ID d not found\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 404,
         "404: status %d", r.status);
   memset(&r, 0, sizeof(r));
   romm_client_device_delete("http://h", "t", "d", on_done, &r);
   answer(403, "{}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN, "403: status %d", r.status);
   memset(&r, 0, sizeof(r));
   romm_client_device_delete("http://h", "t", "d", on_done, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && r.http_status == 0,
         "no response: status %d", r.status);

   stub_reset();
   CHECK(!romm_client_device_delete("http://h", "t", "", on_done, &r)
         && !romm_client_device_delete("http://h", "", "d", on_done, &r)
         && !romm_client_device_delete("", "t", "d", on_done, &r)
         && stub_req.count == 0, "delete started without device or token");
}

static void test_states_delete(void)
{
   static const unsigned ids[] = { 101, 4294967295u, 7 };
   done_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_states_delete("http://romm.local:8090", "rmm_0123abcd",
         ids, 3, on_done, &r), "delete not started");
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url,
            "http://romm.local:8090/api/states/delete")
         && !strcmp(stub_req.content_type, "application/json")
         && strstr(stub_req.headers,
            "Authorization: Bearer rmm_0123abcd\r\n")
         && !strcmp(stub_req.body,
            "{\"states\": [101,4294967295,7]}"),
         "request %s %s %s", stub_req.method, stub_req.url, stub_req.body);
   answer(200, "[101, 4294967295, 7]");
   CHECK(r.calls == 1 && r.status == ROMM_OK, "deleted: status %d",
         r.status);

   memset(&r, 0, sizeof(r));
   romm_client_states_delete("http://h", "t", ids, 1, on_done, &r);
   CHECK(!strcmp(stub_req.body, "{\"states\": [101]}"), "one id %s",
         stub_req.body);
   answer(404, "{\"detail\": \"State with ID 101 not found\"}");
   CHECK(r.status == ROMM_ERR_HTTP && r.http_status == 404,
         "404: status %d", r.status);
   memset(&r, 0, sizeof(r));
   romm_client_states_delete("http://h", "t", ids, 1, on_done, &r);
   answer(-1, NULL);
   CHECK(r.status == ROMM_ERR_UNREACHABLE && r.http_status == 0,
         "no response: status %d", r.status);

   stub_reset();
   CHECK(!romm_client_states_delete("http://h", "t", ids, 0, on_done, &r)
         && !romm_client_states_delete("http://h", "t", NULL, 1, on_done,
            &r)
         && !romm_client_states_delete("http://h", "", ids, 1, on_done, &r)
         && stub_req.count == 0, "delete started without states or token");
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   int http_status;
   unsigned id;
} created_result_t;

static void on_created(void *user_data, enum romm_status status,
      int http_status, unsigned id)
{
   created_result_t *r = (created_result_t*)user_data;
   r->calls++;
   r->status      = status;
   r->http_status = http_status;
   r->id          = id;
}

static void test_collection_roms(void)
{
   static const unsigned ids[] = { 10, 11 };
   done_result_t r;
   created_result_t c;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_collection_roms("http://romm.local:8090/",
         "rmm_0123abcd", 7, ids, 2, false, on_done, &r), "add not started");
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url,
            "http://romm.local:8090/api/collections/7/roms")
         && !strcmp(stub_req.content_type, "application/json")
         && strstr(stub_req.headers,
            "Authorization: Bearer rmm_0123abcd\r\n")
         && !strcmp(stub_req.body, "{\"rom_ids\": [10,11]}"),
         "add: %s %s %s", stub_req.method, stub_req.url, stub_req.body);
   answer(200, "{\"id\": 7, \"name\": \"Favorites\", \"rom_ids\": [10, 11]}");
   CHECK(r.calls == 1 && r.status == ROMM_OK, "added: status %d", r.status);

   memset(&r, 0, sizeof(r));
   romm_client_collection_roms("http://h", "t", 7, ids, 1, true, on_done, &r);
   CHECK(!strcmp(stub_req.method, "DELETE")
         && !strcmp(stub_req.url, "http://h/api/collections/7/roms")
         && !strcmp(stub_req.body, "{\"rom_ids\": [10]}"),
         "remove: %s %s %s", stub_req.method, stub_req.url, stub_req.body);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN, "403: status %d", r.status);

   stub_reset();
   CHECK(!romm_client_collection_roms("http://h", "t", 7, ids, 0, false,
            on_done, &r) && stub_req.count == 0, "started without games");

   memset(&c, 0, sizeof(c));
   CHECK(romm_client_create_favorites("http://romm.local:8090",
         "rmm_0123abcd", on_created, &c), "create not started");
   CHECK(!strcmp(stub_req.method, "POST")
         && !strcmp(stub_req.url, "http://romm.local:8090/api/collections"
            "?is_public=false&is_favorite=true")
         && !strcmp(stub_req.content_type,
            "application/x-www-form-urlencoded")
         && !strcmp(stub_req.body, "name=Favorites"),
         "create: %s %s %s %s", stub_req.method, stub_req.url,
         stub_req.content_type, stub_req.body);
   answer(200, "{\"name\": \"Favorites\", \"rom_ids\": [], \"is_favorite\":"
         " true, \"id\": 12, \"user_id\": 1}");
   CHECK(c.calls == 1 && c.status == ROMM_OK && c.id == 12,
         "created: status %d id %u", c.status, c.id);

   memset(&c, 0, sizeof(c));
   romm_client_create_favorites("http://h", "t", on_created, &c);
   answer(200, "{\"name\": \"Favorites\"}");
   CHECK(c.status == ROMM_ERR_RESPONSE && !c.id, "no id: status %d",
         c.status);
   memset(&c, 0, sizeof(c));
   romm_client_create_favorites("http://h", "t", on_created, &c);
   answer(400, "{\"detail\": \"Collection Favorites already exists\"}");
   CHECK(c.status == ROMM_ERR_HTTP && c.http_status == 400,
         "exists: status %d", c.status);
}

typedef struct
{
   unsigned calls;
   enum romm_status status;
   bool found;
   char missing[512];
} scopes_result_t;

static void on_token_scopes(void *user_data, enum romm_status status,
      int http_status, bool found, const char *missing_scopes)
{
   scopes_result_t *r = (scopes_result_t*)user_data;
   r->calls++;
   r->status = status;
   r->found  = found;
   strlcpy(r->missing, missing_scopes, sizeof(r->missing));
}

static void test_token_scopes(void)
{
   scopes_result_t r;

   memset(&r, 0, sizeof(r));
   stub_reset();
   CHECK(romm_client_token_scopes("http://romm.local:8090", "rmm_0123abcd",
         "e409", on_token_scopes, &r), "lookup not started");
   CHECK(!strcmp(stub_req.method, "GET")
         && !strcmp(stub_req.url, "http://romm.local:8090/api/client-tokens")
         && strstr(stub_req.headers,
            "Authorization: Bearer rmm_0123abcd\r\n"),
         "request %s %s", stub_req.method, stub_req.url);
   /* Another device's token, this device's first pairing (every scope)
    * and its second, newest, without the devices scopes; ids out of
    * order, and a nested value to step over. */
   answer(200, "[{\"id\": 7, \"name\": \"Living room\","
         " \"scopes\": [\"me.read\"], \"device_id\": \"aaaa\"},"
         " {\"id\": 9, \"name\": \"Mac\", \"scopes\": [\"me.read\","
         " \"roms.read\", \"roms.user.read\", \"roms.user.write\","
         " \"platforms.read\", \"assets.read\", \"assets.write\","
         " \"firmware.read\", \"collections.read\","
         " \"collections.write\"], \"extra\": {\"device_id\": \"e409\"},"
         " \"device_id\": \"e409\", \"expires_at\": null},"
         " {\"id\": 3, \"name\": \"Mac\", \"scopes\": [\"me.read\","
         " \"roms.read\", \"roms.user.read\", \"roms.user.write\","
         " \"platforms.read\", \"assets.read\", \"assets.write\","
         " \"firmware.read\", \"collections.read\","
         " \"collections.write\", \"devices.read\", \"devices.write\"],"
         " \"device_id\": \"e409\"}]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && r.found
         && !strcmp(r.missing, "devices.read, devices.write"),
         "newest token: found %d missing '%s'", r.found, r.missing);

   /* Every scope. */
   memset(&r, 0, sizeof(r));
   romm_client_token_scopes("http://h", "t", "e409", on_token_scopes, &r);
   answer(200, "[{\"id\": 3, \"scopes\": [\"me.read\", \"roms.read\","
         " \"roms.user.read\", \"roms.user.write\", \"platforms.read\","
         " \"assets.read\", \"assets.write\", \"firmware.read\","
         " \"collections.read\", \"collections.write\", \"devices.read\","
         " \"devices.write\", \"users.read\"], \"device_id\": \"e409\"}]");
   CHECK(r.found && !*r.missing, "full token: found %d missing '%s'",
         r.found, r.missing);

   /* No token for this device, a cut-off list, a refusal. */
   memset(&r, 0, sizeof(r));
   romm_client_token_scopes("http://h", "t", "e409", on_token_scopes, &r);
   answer(200, "[{\"id\": 7, \"scopes\": [], \"device_id\": \"aaaa\"},"
         " {\"id\": 8, \"scopes\": [], \"device_id\": null}]");
   CHECK(r.calls == 1 && r.status == ROMM_OK && !r.found,
         "other devices' tokens: found %d", r.found);
   memset(&r, 0, sizeof(r));
   romm_client_token_scopes("http://h", "t", "e409", on_token_scopes, &r);
   answer(200, "[{\"id\": 3, \"scopes\": [\"me.read\"], \"device_id\":"
         " \"e409\"}, {\"id\"");
   CHECK(r.status == ROMM_OK && !r.found, "cut-off list: found %d",
         r.found);
   memset(&r, 0, sizeof(r));
   romm_client_token_scopes("http://h", "t", "e409", on_token_scopes, &r);
   answer(403, "{\"detail\": \"Forbidden\"}");
   CHECK(r.status == ROMM_ERR_FORBIDDEN && !r.found, "403: status %d",
         r.status);

   stub_reset();
   CHECK(!romm_client_token_scopes("http://h", "t", "", on_token_scopes, &r)
         && !romm_client_token_scopes("http://h", "", "e409",
            on_token_scopes, &r)
         && stub_req.count == 0, "lookup started without a device or token");
}

int main(void)
{
   test_url();
   test_json();
   test_json_list();
   test_json_items();
   test_json_values();
   test_heartbeat();
   test_whoami();
   test_platforms();
   test_collections();
   test_roms();
   test_rom_ids();
   test_rom_files();
   test_rom_details();
   test_rom_content();
   test_download();
   test_firmware();
   test_saves();
   test_upload();
   test_save_downloaded();
   test_states_delete();
   test_device_delete();
   test_collection_roms();
   test_token_scopes();
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
