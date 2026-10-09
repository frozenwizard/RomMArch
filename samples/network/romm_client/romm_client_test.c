/* Harness for the RomM API client (network/romm/romm_client.c).
 *
 * The test plays the server: each request the client starts is
 * recorded by the stubs, checked, and answered with the response the
 * test chooses. Covers the server address joining, the heartbeat
 * request, and how its replies are read: RomM's version, another web
 * server's page, an HTTP error, and no answer at all.
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

/* Answers the recorded request with @status and @body. -1 is what the
 * HTTP task reports when it got no response; -2 answers with no
 * response data at all. */
static void answer(int status, const char *body)
{
   http_transfer_data_t resp;
   char buf[2048];

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

int main(void)
{
   test_url();
   test_json();
   test_heartbeat();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall RomM client checks passed\n");
   return 0;
}
