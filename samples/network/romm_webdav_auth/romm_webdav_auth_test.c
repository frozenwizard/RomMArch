/* Harness for WebDAV authorization against RomM.
 *
 * RomM's WebDAV (Cloud Sync) takes Basic auth only with an account's
 * real password, which a single-sign-on user may not have. It takes a
 * paired device's token as a Bearer token, so with HAVE_ROMM the
 * driver sends that instead, but only to the RomM server's own
 * addresses: the server address and the approval web address. Any
 * other WebDAV server gets the configured username and password as
 * before, never the token.
 *
 * RomM keeps no server-side backups (no COPY; its MOVE deletes), so
 * with destructive sync off an upload to it is a plain PUT and a delete
 * a plain DELETE, while other servers still get the backup first.
 *
 * Includes the driver's translation unit, built with HAVE_ROMM, and
 * the webdav_reauth harness's stubs, which record each request the
 * driver starts.
 */

#include <stdio.h>
#include <string.h>

#include <boolean.h>
#include <compat/strl.h>

#include "../../../network/cloud_sync/webdav.c"

#include "../webdav_reauth/stubs_retroarch.h"

static unsigned failures = 0;

static void check(const char *what, bool ok)
{
   printf("  %-62s %s\n", what, ok ? "ok" : "FAIL");
   if (!ok)
      failures++;
}

static void setup(const char *server, const char *web, const char *token,
      const char *user, const char *pass)
{
   settings_t *settings = config_get_ptr();
   strlcpy(settings->arrays.romm_server_url, server,
         sizeof(settings->arrays.romm_server_url));
   strlcpy(settings->arrays.romm_web_url, web,
         sizeof(settings->arrays.romm_web_url));
   strlcpy(settings->arrays.romm_token, token,
         sizeof(settings->arrays.romm_token));
   strlcpy(settings->arrays.webdav_username, user,
         sizeof(settings->arrays.webdav_username));
   strlcpy(settings->arrays.webdav_password, pass,
         sizeof(settings->arrays.webdav_password));
   free(webdav_driver_st.basic_auth_header);
   webdav_driver_st.basic_auth_header = NULL;
   webdav_driver_st.basic             = true;
   /* as a sync is pushed */
   webdav_sync_capture();
}

/* The header the driver sends to @url. */
static bool header_is(const char *url, const char *want)
{
   char *got;
   bool  ok;
   strlcpy(webdav_driver_st.url, url, sizeof(webdav_driver_st.url));
   got = webdav_get_auth_header("PUT", url);
   ok  = want ? (got && !strcmp(got, want)) : !got;
   if (!ok)
      printf("    got: %s\n", got ? got : "(none)");
   free(got);
   return ok;
}

static bool header_starts(const char *url, const char *prefix)
{
   char *got;
   bool  ok;
   strlcpy(webdav_driver_st.url, url, sizeof(webdav_driver_st.url));
   got = webdav_get_auth_header("PUT", url);
   ok  = got && !strncmp(got, prefix, strlen(prefix));
   free(got);
   return ok;
}

static void on_done(void *user_data, const char *path, bool success,
      RFILE *file)
{
   (void)user_data; (void)path; (void)success; (void)file;
}

/* Starts a sync against @url and reports whether its first request,
 * the OPTIONS probe, carried an Authorization header. */
static bool begin_has_auth(const char *url)
{
   settings_t *settings = config_get_ptr();
   bool        had_auth;
   strlcpy(settings->arrays.webdav_url, url,
         sizeof(settings->arrays.webdav_url));
   stub_reset();
   if (!webdav_sync_begin(on_done, NULL) || stub_req.count != 1)
      return false;
   had_auth = stub_req.had_auth;
   /* the probe's answer: none, which frees its state */
   stub_req.cb(NULL, NULL, stub_req.user_data, NULL);
   return had_auth;
}

/* The method of the first request an upload (or a delete) of a file
 * starts against @url, its answer left out so the request fails. */
static const char *first_request(const char *url, bool upload)
{
   static const char tmp[] = "romm_webdav_auth.tmp";
   const char *method = NULL;
   RFILE      *rfile  = NULL;
   bool        ok;

   strlcpy(webdav_driver_st.url, url, sizeof(webdav_driver_st.url));
   stub_reset();
   if (upload)
   {
      filestream_write_file(tmp, "data", 4);
      rfile = filestream_open(tmp, RETRO_VFS_FILE_ACCESS_READ,
            RETRO_VFS_FILE_ACCESS_HINT_NONE);
      ok    = rfile && webdav_update("a.opt", rfile, on_done, NULL);
   }
   else
      ok = webdav_delete("a.opt", on_done, NULL);
   if (ok && stub_req.count)
   {
      method = stub_req.method;
      stub_req.cb(NULL, NULL, stub_req.user_data, NULL);
   }
   if (rfile)
      filestream_close(rfile);
   filestream_delete(tmp);
   return method;
}

static bool first_request_is(const char *url, bool upload, const char *want)
{
   const char *got = first_request(url, upload);
   if (!got || strcmp(got, want))
      printf("    got: %s\n", got ? got : "(none)");
   return got && !strcmp(got, want);
}

int main(void)
{
   static const char bearer[] = "Authorization: Bearer rmm_tok\r\n";

   printf("romm_webdav_auth_test\n");

   setup("http://romm.lan:8090", "https://romm.example.org", "rmm_tok",
         "", "");
   check("RomM's server address gets the device token",
         header_is("http://romm.lan:8090/api/sync/retroarch/", bearer));
   check("so does its approval web address",
         header_is("https://romm.example.org/api/sync/retroarch/", bearer));
   check("the server address itself matches",
         header_is("http://romm.lan:8090", bearer));
   check("a longer port sharing the prefix gets nothing",
         header_is("http://romm.lan:80901/api/sync/retroarch/", NULL));
   check("a host sharing the prefix gets nothing",
         header_is("http://romm.lan:8090.example/dav/", NULL));
   check("another server gets nothing",
         header_is("https://dav.example.net/remote.php/dav/", NULL));

   setup("http://romm.lan:8090/", "", "rmm_tok", "", "");
   check("a server address ending in / still matches",
         header_is("http://romm.lan:8090/api/sync/retroarch/", bearer));

   setup("http://romm.lan:8090", "", "rmm_tok", "user", "pass");
   check("another server gets its username and password, not the token",
         header_starts("https://dav.example.net/dav/",
            "Authorization: Basic "));
   check("RomM gets the token even with a username and password set",
         header_is("http://romm.lan:8090/api/sync/retroarch/", bearer));

   setup("http://romm.lan:8090", "", "rmm_tok", "", "");
   strlcpy(config_get_ptr()->arrays.romm_token, "rmm_new",
         sizeof(config_get_ptr()->arrays.romm_token));
   check("a token saved during a sync waits for the next one",
         header_is("http://romm.lan:8090/api/sync/retroarch/", bearer));

   setup("http://romm.lan:8090", "", "", "user", "pass");
   check("unpaired, RomM gets the username and password",
         header_starts("http://romm.lan:8090/api/sync/retroarch/",
            "Authorization: Basic "));

   setup("", "", "rmm_tok", "", "");
   check("no RomM address set: nothing matches",
         header_is("http://romm.lan:8090/api/sync/retroarch/", NULL));

   setup("https://", "http://", "rmm_tok", "", "");
   check("an address that is only a scheme matches no server",
         header_is("https://dav.example.net/dav/", NULL));
   check("nor does a web address that is only a scheme",
         header_is("http://dav.example.net/dav/", NULL));
   setup("romm.lan:8090", "", "rmm_tok", "", "");
   check("an address without a scheme matches nothing",
         header_is("romm.lan:8090/api/sync/retroarch/", NULL));

   setup("http://romm.lan:8090", "", "rmm_tok", "", "");
   check("a sync with RomM probes it with the token",
         begin_has_auth("http://romm.lan:8090/api/sync/retroarch"));
   check("a sync with another server probes it without",
         !begin_has_auth("http://dav.example.net/dav"));
   setup("http://romm.lan:8090", "", "", "", "");
   check("unpaired, the probe goes without credentials",
         !begin_has_auth("http://romm.lan:8090/api/sync/retroarch"));

   /* non-destructive: RomM's copies aren't backed up first */
   setup("http://romm.lan:8090", "", "rmm_tok", "", "");
   config_get_ptr()->bools.cloud_sync_destructive = false;
   check("an upload to RomM is a plain PUT",
         first_request_is("http://romm.lan:8090/api/sync/retroarch/",
            true, "PUT"));
   check("a delete on RomM is a plain DELETE",
         first_request_is("http://romm.lan:8090/api/sync/retroarch/",
            false, "DELETE"));
   setup("http://romm.lan:8090", "", "", "", "");
   check("so is an unpaired one",
         first_request_is("http://romm.lan:8090/api/sync/retroarch/",
            true, "PUT"));
   check("another server still gets the backup first (MKCOL deleted/)",
         first_request_is("http://dav.example.net/dav/", true, "MKCOL"));
   check("and its deletes too",
         first_request_is("http://dav.example.net/dav/", false, "MKCOL"));
   config_get_ptr()->bools.cloud_sync_destructive = true;
   check("destructive: another server gets a plain PUT",
         first_request_is("http://dav.example.net/dav/", true, "PUT"));
   check("and a plain DELETE",
         first_request_is("http://dav.example.net/dav/", false, "DELETE"));
   stub_reset();

   printf("romm_webdav_auth_test: %s\n", failures ? "FAIL" : "PASS");
   return failures ? 1 : 0;
}
