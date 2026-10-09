/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>

#include <compat/strl.h>
#include <formats/rjson.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>

#include "romm_client.h"
#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"

#define ROMM_PFX "[RomM] "
#define ROMM_JSON_MAX_KEYS 4
#define ROMM_HEADERS_JSON "Accept: application/json\r\n"
#define ROMM_CLIENT_NAME "Rommarch"

/* What pairing asks for; the user can narrow it when approving. */
static const char *const romm_scopes[] = {
   "me.read",
   "roms.read",
   "roms.user.read",
   "roms.user.write",
   "platforms.read",
   "assets.read",
   "assets.write",
   "firmware.read",
   "collections.read",
   "collections.write"
};

typedef struct
{
   romm_heartbeat_cb_t cb;
   void *user_data;
} romm_heartbeat_ctx_t;

typedef struct
{
   romm_pair_start_cb_t cb;
   void *user_data;
   char web[1024];
} romm_pair_start_ctx_t;

/* Copies the string or number at @path, object keys joined by '.'
 * such as "SYSTEM.VERSION". */
static bool romm_json_get(const char *data, size_t len, const char *path,
      char *s, size_t s_len)
{
   const char *keys[ROMM_JSON_MAX_KEYS];
   size_t key_lens[ROMM_JSON_MAX_KEYS];
   unsigned num_keys = 0;
   /* How many of @keys the object being parsed is nested under. */
   unsigned matched  = 0;
   bool at_key       = false;
   bool found        = false;
   const char *p     = path;
   rjson_t *json;
   enum rjson_type type;

   for (;;)
   {
      const char *dot = strchr(p, '.');
      if (num_keys == ROMM_JSON_MAX_KEYS)
         return false;
      keys[num_keys]     = p;
      key_lens[num_keys] = dot ? (size_t)(dot - p) : strlen(p);
      num_keys++;
      if (!dot)
         break;
      p = dot + 1;
   }

   if (!data || !len || !(json = rjson_open_buffer(data, len)))
      return false;

   while (!found
         && (type = rjson_next(json)) != RJSON_DONE
         && type != RJSON_ERROR)
   {
      unsigned depth = rjson_get_context_depth(json);

      if (at_key)
      {
         at_key = false;
         if (type == RJSON_OBJECT && matched + 1 < num_keys)
            matched++;
         else if ((type == RJSON_STRING || type == RJSON_NUMBER)
               && matched + 1 == num_keys)
         {
            size_t _len;
            const char *str = rjson_get_string(json, &_len);
            strlcpy(s, str, s_len);
            found = true;
         }
      }
      else if (type == RJSON_STRING
            && depth == matched + 1
            && rjson_get_context_type(json) == RJSON_OBJECT
            && (rjson_get_context_count(json) & 1))
      {
         size_t _len;
         const char *str = rjson_get_string(json, &_len);
         at_key = _len == key_lens[matched]
               && !memcmp(str, keys[matched], _len);
      }
      else if ((type == RJSON_OBJECT_END || type == RJSON_ARRAY_END)
            && depth < matched + 1)
         matched = depth ? depth - 1 : 0;
   }

   rjson_free(json);
   return found;
}

static unsigned romm_json_get_uint(const char *data, size_t len,
      const char *path, unsigned fallback)
{
   char num[24];
   if (romm_json_get(data, len, path, num, sizeof(num)))
   {
      unsigned long n = strtoul(num, NULL, 10);
      if (n)
         return (unsigned)n;
   }
   return fallback;
}

static enum romm_status romm_response_status(
      const http_transfer_data_t *data, int expected)
{
   if (!data || data->status <= 0)
      return ROMM_ERR_UNREACHABLE;
   if (data->status != expected)
      return ROMM_ERR_HTTP;
   return ROMM_OK;
}

bool romm_client_url(char *s, size_t len, const char *server,
      const char *path)
{
   size_t server_len;

   if (string_is_empty(server))
      return false;
   server_len = strlen(server);
   while (server_len && server[server_len - 1] == '/')
      server_len--;
   if (!server_len || server_len + strlen(path) >= len)
      return false;
   memcpy(s, server, server_len);
   strlcpy(s + server_len, path, len - server_len);
   return true;
}

static void romm_heartbeat_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_heartbeat_ctx_t *ctx  = (romm_heartbeat_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   char version[64];

   version[0] = '\0';
   if (     status == ROMM_OK
         && !romm_json_get(data->data, data->len, "SYSTEM.VERSION",
               version, sizeof(version)))
      status = ROMM_ERR_RESPONSE;

   if (status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "Server runs RomM %s.\n", version);
   else
      RARCH_WARN(ROMM_PFX "Heartbeat failed (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status, version);
   free(ctx);
}

bool romm_client_heartbeat(const char *server,
      romm_heartbeat_cb_t cb, void *user_data)
{
   char url[1024];
   romm_heartbeat_ctx_t *ctx;

   if (!romm_client_url(url, sizeof(url), server, "/api/heartbeat"))
      return false;
   if (!(ctx = (romm_heartbeat_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!task_push_http_transfer_with_headers(url, true, NULL,
         ROMM_HEADERS_JSON, romm_heartbeat_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

static void romm_pair_start_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_pair_start_ctx_t *ctx = (romm_pair_start_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 201);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   romm_pairing_t pairing;
   char path[256];

   memset(&pairing, 0, sizeof(pairing));
   if (     status == ROMM_OK
         && (  !romm_json_get(data->data, data->len, "device_code",
                  pairing.device_code, sizeof(pairing.device_code))
            || !romm_json_get(data->data, data->len, "user_code",
                  pairing.user_code, sizeof(pairing.user_code))
            || !romm_json_get(data->data, data->len,
                  "verification_path_complete", path, sizeof(path))
            || !romm_client_url(pairing.approve_url,
                  sizeof(pairing.approve_url), ctx->web, path)))
      status = ROMM_ERR_RESPONSE;

   if (status == ROMM_OK)
   {
      pairing.interval   = romm_json_get_uint(data->data, data->len,
            "interval", 5);
      pairing.expires_in = romm_json_get_uint(data->data, data->len,
            "expires_in", 600);
      RARCH_LOG(ROMM_PFX "Pairing started; approve at %s\n",
            pairing.approve_url);
   }
   else
      RARCH_WARN(ROMM_PFX "Couldn't start pairing (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? &pairing : NULL);
   free(ctx);
}

bool romm_client_pair_start(const char *server, const char *web,
      const char *device_id, const char *device_name,
      const char *client_version,
      romm_pair_start_cb_t cb, void *user_data)
{
   char url[1024];
   rjsonwriter_t *writer;
   romm_pair_start_ctx_t *ctx;
   const char *body;
   int body_len   = 0;
   bool pushed    = false;
   size_t i;

   if (     string_is_empty(device_id)
         || string_is_empty(device_name)
         || !romm_client_url(url, sizeof(url), server,
               "/api/auth/device/init"))
      return false;
   if (!(ctx = (romm_pair_start_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;
   strlcpy(ctx->web, string_is_empty(web) ? server : web,
         sizeof(ctx->web));

   if (!(writer = rjsonwriter_open_memory()))
   {
      free(ctx);
      return false;
   }
   rjsonwriter_raw(writer, "{", 1);
   rjsonwriter_add_string(writer, "client_device_identifier");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, device_id);
   rjsonwriter_raw(writer, ",", 1);
   rjsonwriter_add_string(writer, "name");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, device_name);
   rjsonwriter_raw(writer, ",", 1);
   rjsonwriter_add_string(writer, "client");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, ROMM_CLIENT_NAME);
   rjsonwriter_raw(writer, ",", 1);
   rjsonwriter_add_string(writer, "platform");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, "macOS");
   rjsonwriter_raw(writer, ",", 1);
   rjsonwriter_add_string(writer, "client_version");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, client_version ? client_version : "");
   rjsonwriter_raw(writer, ",", 1);
   rjsonwriter_add_string(writer, "requested_scopes");
   rjsonwriter_raw(writer, ":[", 2);
   for (i = 0; i < ARRAY_SIZE(romm_scopes); i++)
   {
      if (i)
         rjsonwriter_raw(writer, ",", 1);
      rjsonwriter_add_string(writer, romm_scopes[i]);
   }
   rjsonwriter_raw(writer, "]}", 2);

   if ((body = rjsonwriter_get_memory_buffer(writer, &body_len)))
      pushed = task_push_http_transfer_with_content(url, "POST",
            body, (size_t)body_len, "application/json", true, false,
            ROMM_HEADERS_JSON, romm_pair_start_cb, ctx) != NULL;
   rjsonwriter_free(writer);

   if (!pushed)
      free(ctx);
   return pushed;
}
