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
#include <string/stdstring.h>

#include "romm_client.h"
#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"

#define ROMM_PFX "[RomM] "
#define ROMM_JSON_MAX_KEYS 4
#define ROMM_HEADERS_JSON "Accept: application/json\r\n"

typedef struct
{
   romm_heartbeat_cb_t cb;
   void *user_data;
} romm_heartbeat_ctx_t;

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
