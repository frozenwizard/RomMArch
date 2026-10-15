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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array/rbuf.h>
#include <compat/strl.h>
#include <features/features_cpu.h>
#include <formats/rjson.h>
#include <queues/task_queue.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>

#include "romm_client.h"
#include "romm_json.h"
#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"

#define ROMM_PFX "[RomM] "
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
   "collections.write",
   "devices.read",
   "devices.write"
};

typedef struct
{
   romm_heartbeat_cb_t cb;
   void *user_data;
} romm_heartbeat_ctx_t;

typedef struct
{
   romm_whoami_cb_t cb;
   void *user_data;
} romm_whoami_ctx_t;

typedef struct
{
   romm_pair_start_cb_t cb;
   void *user_data;
   char web[1024];
} romm_pair_start_ctx_t;

typedef struct
{
   romm_pair_wait_cb_t cb;
   void *user_data;
   retro_task_t *task;
   retro_time_t deadline;
   unsigned interval;
   enum romm_status status;
   int http_status;
   bool finished;
   bool poll_pending;
   char url[1024];
   char body[256];
   char token[128];
   char device_id[64];
   char missing_scopes[256];
} romm_pair_wait_ctx_t;

typedef struct
{
   romm_platforms_cb_t cb;
   void *user_data;
} romm_platforms_ctx_t;

typedef struct
{
   romm_roms_cb_t cb;
   void *user_data;
   unsigned limit;
} romm_roms_ctx_t;

typedef struct
{
   romm_download_cb_t cb;
   void *user_data;
} romm_download_ctx_t;

typedef struct
{
   romm_firmware_cb_t cb;
   void *user_data;
} romm_firmware_ctx_t;

typedef struct
{
   romm_rom_ids_cb_t cb;
   void *user_data;
} romm_rom_ids_ctx_t;

typedef struct
{
   romm_rom_files_cb_t cb;
   void *user_data;
} romm_rom_files_ctx_t;

typedef struct
{
   romm_content_cb_t cb;
   void *user_data;
   uint64_t offset;
   uint64_t length;
} romm_content_ctx_t;

typedef struct
{
   romm_saves_cb_t cb;
   void *user_data;
   /* What is listed, for the log: "saves" or "save states" */
   const char *what;
} romm_saves_ctx_t;

typedef struct
{
   romm_save_cb_t cb;
   void *user_data;
   /* What is uploaded, for the log: "save" or "save state" */
   const char *what;
} romm_upload_ctx_t;

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

static unsigned romm_json_uint(const char *value)
{
   return (unsigned)strtoul(value, NULL, 10);
}

static enum romm_status romm_response_status(
      const http_transfer_data_t *data, int expected)
{
   if (!data || data->status <= 0)
      return ROMM_ERR_UNREACHABLE;
   if (data->status == 401)
      return ROMM_ERR_UNAUTHORIZED;
   if (data->status == 403)
      return ROMM_ERR_FORBIDDEN;
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

/* Returns the transfer's task, or NULL. */
static void *romm_get_with_headers(const char *server, const char *token,
      const char *path, const char *extra_headers,
      retro_task_callback_t cb, void *ctx)
{
   char url[2048];
   char headers[512];

   if (     string_is_empty(token)
         || !romm_client_url(url, sizeof(url), server, path))
      return NULL;
   if ((size_t)snprintf(headers, sizeof(headers),
         "Authorization: Bearer %s\r\n%s", token, extra_headers)
         >= sizeof(headers))
      return NULL;
   return task_push_http_transfer_with_headers(url, true, NULL,
         headers, cb, ctx);
}

/* Percent-encodes @src for one segment of a URL path. */
static bool romm_url_escape(char *s, size_t len, const char *src)
{
   static const char hex[] = "0123456789ABCDEF";
   size_t pos              = 0;

   for (; *src; src++)
   {
      unsigned char c = (unsigned char)*src;
      if (ISALNUM(c) || c == '-' || c == '.' || c == '_' || c == '~')
      {
         if (pos + 1 >= len)
            return false;
         s[pos++] = (char)c;
      }
      else
      {
         if (pos + 3 >= len)
            return false;
         s[pos++] = '%';
         s[pos++] = hex[c >> 4];
         s[pos++] = hex[c & 15];
      }
   }
   if (pos >= len)
      return false;
   s[pos] = '\0';
   return true;
}

/* Starts GET @path on @server with @token's Bearer authorization,
 * asking for JSON. */
static bool romm_get(const char *server, const char *token,
      const char *path, retro_task_callback_t cb, void *ctx)
{
   return romm_get_with_headers(server, token, path, ROMM_HEADERS_JSON,
         cb, ctx) != NULL;
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

static void romm_whoami_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_whoami_ctx_t *ctx     = (romm_whoami_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   char username[256];

   username[0] = '\0';
   if (     status == ROMM_OK
         && !romm_json_get(data->data, data->len, "username",
               username, sizeof(username)))
      status = ROMM_ERR_RESPONSE;

   if (status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "Token acts for user %s.\n", username);
   else
      RARCH_WARN(ROMM_PFX "Token check failed (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status, username);
   free(ctx);
}

bool romm_client_whoami(const char *server, const char *token,
      romm_whoami_cb_t cb, void *user_data)
{
   romm_whoami_ctx_t *ctx;

   if (!(ctx = (romm_whoami_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!romm_get(server, token, "/api/users/me", romm_whoami_cb, ctx))
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

static void romm_pair_scope(void *ctx, enum rjson_type type,
      const char *value, size_t len)
{
   uint32_t *granted = (uint32_t*)ctx;
   size_t i;

   if (type != RJSON_STRING)
      return;
   for (i = 0; i < ARRAY_SIZE(romm_scopes); i++)
      if (string_is_equal(value, romm_scopes[i]))
         *granted |= (uint32_t)1 << i;
}

/* Lists in @s the scopes asked for that the token reply @data leaves
 * out; none when it doesn't list scopes at all. */
static void romm_pair_missing_scopes(const char *data, size_t len,
      char *s, size_t s_len)
{
   uint32_t granted = 0;
   size_t i;

   *s = '\0';
   if (!romm_json_values(data, len, "scopes", romm_pair_scope, &granted))
      return;
   for (i = 0; i < ARRAY_SIZE(romm_scopes); i++)
   {
      if (granted & ((uint32_t)1 << i))
         continue;
      if (*s)
         strlcat(s, ", ", s_len);
      strlcat(s, romm_scopes[i], s_len);
   }
}

static void romm_pair_wait_finish(romm_pair_wait_ctx_t *ctx,
      enum romm_status status)
{
   ctx->status    = status;
   ctx->finished  = true;
   ctx->task->when = 0;
}

static void romm_pair_poll_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_pair_wait_ctx_t *ctx  = (romm_pair_wait_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   char detail[64];

   ctx->poll_pending = false;
   if (status != ROMM_ERR_UNREACHABLE)
      ctx->http_status = data->status;

   if (status == ROMM_OK)
   {
      if (     romm_json_get(data->data, data->len, "access_token",
                  ctx->token, sizeof(ctx->token))
            && romm_json_get(data->data, data->len, "device_id",
                  ctx->device_id, sizeof(ctx->device_id)))
      {
         romm_pair_missing_scopes(data->data, data->len,
               ctx->missing_scopes, sizeof(ctx->missing_scopes));
         romm_pair_wait_finish(ctx, ROMM_OK);
      }
      else
         romm_pair_wait_finish(ctx, ROMM_ERR_RESPONSE);
      return;
   }
   /* Keep polling through a dropped connection; the deadline ends it. */
   if (status == ROMM_ERR_UNREACHABLE)
   {
      RARCH_DBG(ROMM_PFX "Pairing poll got no response.\n");
      return;
   }
   if (data->status == 400 && romm_json_get(data->data, data->len,
            "detail", detail, sizeof(detail)))
   {
      if (string_is_equal(detail, "authorization_pending"))
      {
         RARCH_DBG(ROMM_PFX "Pairing not approved yet.\n");
         return;
      }
      if (string_is_equal(detail, "slow_down"))
      {
         RARCH_DBG(ROMM_PFX "Pairing polls slowed down.\n");
         ctx->interval   += 5;
         ctx->task->when += 5000000;
         return;
      }
      if (string_is_equal(detail, "access_denied"))
      {
         romm_pair_wait_finish(ctx, ROMM_ERR_DENIED);
         return;
      }
      if (string_is_equal(detail, "expired_token"))
      {
         romm_pair_wait_finish(ctx, ROMM_ERR_EXPIRED);
         return;
      }
   }
   /* Rate limited */
   if (data->status == 429)
   {
      RARCH_DBG(ROMM_PFX "Pairing polls rate limited.\n");
      ctx->interval   += 5;
      ctx->task->when += 5000000;
      return;
   }
   RARCH_WARN(ROMM_PFX "Pairing poll failed (HTTP %d).\n", data->status);
   romm_pair_wait_finish(ctx, ROMM_ERR_HTTP);
}

/* Runs on the main thread (RETRO_TASK_FLG_MAIN_THREAD), like the poll
 * callbacks, so the two share @ctx without locking. */
static void romm_pair_wait_handler(retro_task_t *task)
{
   romm_pair_wait_ctx_t *ctx = (romm_pair_wait_ctx_t*)task->state;
   retro_time_t now          = cpu_features_get_time_usec();

   /* A poll still out holds @ctx; finish only once it has answered. */
   if (ctx->poll_pending)
   {
      task->when = now + 250000;
      return;
   }
   if (!ctx->finished && (task_get_flags(task) & RETRO_TASK_FLG_CANCELLED))
   {
      ctx->status   = ROMM_ERR_CANCELLED;
      ctx->finished = true;
   }
   if (!ctx->finished && now >= ctx->deadline)
   {
      ctx->status   = ROMM_ERR_EXPIRED;
      ctx->finished = true;
   }
   if (ctx->finished)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   if (task_push_http_transfer_with_content(ctx->url, "POST",
         ctx->body, strlen(ctx->body), "application/json", true, false,
         ROMM_HEADERS_JSON, romm_pair_poll_cb, ctx))
      ctx->poll_pending = true;
   task->when = now + (retro_time_t)ctx->interval * 1000000;
}

static void romm_pair_wait_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_pair_wait_ctx_t *ctx = (romm_pair_wait_ctx_t*)user_data;

   if (ctx->status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "Paired as device %s.\n", ctx->device_id);
   else
      RARCH_WARN(ROMM_PFX "Pairing ended (status %d).\n", ctx->status);
   if (*ctx->missing_scopes)
      RARCH_WARN(ROMM_PFX "Pairing left out %s.\n", ctx->missing_scopes);

   ctx->cb(ctx->user_data, ctx->status, ctx->http_status,
         ctx->status == ROMM_OK ? ctx->token     : "",
         ctx->status == ROMM_OK ? ctx->device_id : "",
         ctx->missing_scopes);
}

static void romm_pair_wait_cleanup(retro_task_t *task)
{
   romm_pair_wait_ctx_t *ctx = (romm_pair_wait_ctx_t*)task->state;
   if (ctx)
      memset(ctx->token, 0, sizeof(ctx->token));
   free(ctx);
   task->state = NULL;
}

void *romm_client_pair_wait(const char *server,
      const romm_pairing_t *pairing, const char *title,
      romm_pair_wait_cb_t cb, void *user_data)
{
   romm_pair_wait_ctx_t *ctx;
   retro_task_t *task;
   retro_time_t now;
   rjsonwriter_t *writer;
   const char *body;
   int body_len = 0;

   if (!pairing || string_is_empty(pairing->device_code))
      return NULL;
   if (!(ctx = (romm_pair_wait_ctx_t*)calloc(1, sizeof(*ctx))))
      return NULL;
   if (!romm_client_url(ctx->url, sizeof(ctx->url), server,
         "/api/auth/device/token"))
   {
      free(ctx);
      return NULL;
   }

   if (!(writer = rjsonwriter_open_memory()))
   {
      free(ctx);
      return NULL;
   }
   rjsonwriter_raw(writer, "{", 1);
   rjsonwriter_add_string(writer, "device_code");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, pairing->device_code);
   rjsonwriter_raw(writer, "}", 1);
   body = rjsonwriter_get_memory_buffer(writer, &body_len);
   if (!body || (size_t)body_len >= sizeof(ctx->body))
   {
      rjsonwriter_free(writer);
      free(ctx);
      return NULL;
   }
   memcpy(ctx->body, body, (size_t)body_len);
   rjsonwriter_free(writer);

   if (!(task = task_init()))
   {
      free(ctx);
      return NULL;
   }

   now            = cpu_features_get_time_usec();
   ctx->cb        = cb;
   ctx->user_data = user_data;
   ctx->task      = task;
   ctx->interval  = pairing->interval ? pairing->interval : 5;
   ctx->deadline  = now + (retro_time_t)pairing->expires_in * 1000000;

   task->handler   = romm_pair_wait_handler;
   task->callback  = romm_pair_wait_cb;
   task->cleanup   = romm_pair_wait_cleanup;
   task->state     = ctx;
   task->user_data = ctx;
   task->when      = now + (retro_time_t)ctx->interval * 1000000;
   task->flags    |= RETRO_TASK_FLG_MAIN_THREAD;
   if (title)
   {
      task->title    = strdup(title);
      task->progress = -1;
   }
   else
      task->flags   |= RETRO_TASK_FLG_MUTE;

   if (!task_queue_push(task))
   {
      free(task->title);
      free(task);
      free(ctx);
      return NULL;
   }
   RARCH_LOG(ROMM_PFX "Waiting for the pairing to be approved.\n");
   return task;
}

void romm_client_pair_cancel(void *wait)
{
   retro_task_t *task = (retro_task_t*)wait;
   task_queue_cancel_task(task);
   /* Now rather than at the next poll */
   task->when = 0;
}

typedef struct
{
   romm_platform_t *list;
   romm_platform_t cur;
} romm_platforms_parse_t;

static void romm_platforms_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_platform_t *p = &((romm_platforms_parse_t*)ctx)->cur;

   if (type != RJSON_STRING && type != RJSON_NUMBER)
      return;
   if (string_is_equal(key, "id"))
      p->id = romm_json_uint(value);
   else if (string_is_equal(key, "rom_count"))
      p->rom_count = romm_json_uint(value);
   else if (string_is_equal(key, "slug"))
      strlcpy(p->slug, value, sizeof(p->slug));
   else if (string_is_equal(key, "libretro_slug"))
      strlcpy(p->libretro_slug, value, sizeof(p->libretro_slug));
   /* display_name includes the user's custom name. */
   else if (string_is_equal(key, "display_name") && *value)
      strlcpy(p->name, value, sizeof(p->name));
   else if (string_is_equal(key, "name") && !*p->name)
      strlcpy(p->name, value, sizeof(p->name));
}

static void romm_platforms_item_end(void *ctx)
{
   romm_platforms_parse_t *parse = (romm_platforms_parse_t*)ctx;

   if (parse->cur.id)
      RBUF_PUSH(parse->list, parse->cur);
   memset(&parse->cur, 0, sizeof(parse->cur));
}

static void romm_platforms_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_platforms_ctx_t *ctx  = (romm_platforms_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   romm_platforms_parse_t parse;

   memset(&parse, 0, sizeof(parse));
   if (     status == ROMM_OK
         && !romm_json_items(data->data, data->len, NULL,
               romm_platforms_member, romm_platforms_item_end, &parse))
      status = ROMM_ERR_RESPONSE;

   if (status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "RomM lists %u platforms.\n",
            (unsigned)RBUF_LEN(parse.list));
   else
      RARCH_WARN(ROMM_PFX "Couldn't list platforms (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? parse.list : NULL,
         status == ROMM_OK ? RBUF_LEN(parse.list) : 0);
   RBUF_FREE(parse.list);
   free(ctx);
}

bool romm_client_platforms(const char *server, const char *token,
      romm_platforms_cb_t cb, void *user_data)
{
   romm_platforms_ctx_t *ctx;

   if (!(ctx = (romm_platforms_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!romm_get(server, token, "/api/platforms", romm_platforms_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

typedef struct
{
   romm_rom_t *list;
   romm_rom_t cur;
   /* Items RomM sent, those left out of @list included */
   size_t listed;
} romm_roms_parse_t;

static void romm_roms_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_rom_t *r = &((romm_roms_parse_t*)ctx)->cur;

   if (string_is_equal(key, "has_simple_single_file"))
      r->single_file = type == RJSON_TRUE;
   else if (type != RJSON_STRING && type != RJSON_NUMBER)
      return;
   else if (string_is_equal(key, "id"))
      r->id = romm_json_uint(value);
   else if (string_is_equal(key, "platform_id"))
      r->platform_id = romm_json_uint(value);
   else if (string_is_equal(key, "crc_hash"))
      strlcpy(r->crc, value, sizeof(r->crc));
   else if (string_is_equal(key, "fs_name"))
      strlcpy(r->fs_name, value, sizeof(r->fs_name));
   else if (string_is_equal(key, "fs_name_no_ext"))
      strlcpy(r->fs_name_no_ext, value, sizeof(r->fs_name_no_ext));
   else if (string_is_equal(key, "name"))
      strlcpy(r->name, value, sizeof(r->name));
   else if (string_is_equal(key, "path_cover_large"))
      strlcpy(r->cover, value, sizeof(r->cover));
   else if (string_is_equal(key, "updated_at"))
      strlcpy(r->updated_at, value, sizeof(r->updated_at));
}

static void romm_roms_item_end(void *ctx)
{
   romm_roms_parse_t *parse = (romm_roms_parse_t*)ctx;
   romm_rom_t *r            = &parse->cur;

   parse->listed++;
   if (r->id && *r->fs_name)
   {
      if (!*r->name)
         strlcpy(r->name, *r->fs_name_no_ext ? r->fs_name_no_ext
               : r->fs_name, sizeof(r->name));
      RBUF_PUSH(parse->list, *r);
   }
   memset(r, 0, sizeof(*r));
}

static void romm_roms_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_roms_ctx_t *ctx       = (romm_roms_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   romm_roms_parse_t *parse;

   /* A romm_rom_t is about 1 KB, too big to parse into on the stack. */
   if (!(parse = (romm_roms_parse_t*)calloc(1, sizeof(*parse))))
      status = ROMM_ERR_RESPONSE;
   else if (status == ROMM_OK
         && !romm_json_items(data->data, data->len, "items",
               romm_roms_member, romm_roms_item_end, parse))
      status = ROMM_ERR_RESPONSE;

   if (status == ROMM_OK)
      RARCH_DBG(ROMM_PFX "Got %u games.\n", (unsigned)RBUF_LEN(parse->list));
   else
      RARCH_WARN(ROMM_PFX "Couldn't list games (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? parse->list : NULL,
         status == ROMM_OK ? RBUF_LEN(parse->list) : 0,
         status == ROMM_OK && parse->listed >= ctx->limit);
   if (parse)
   {
      RBUF_FREE(parse->list);
      free(parse);
   }
   free(ctx);
}

bool romm_client_roms(const char *server, const char *token,
      unsigned platform_id, const char *updated_after,
      unsigned offset, unsigned limit,
      romm_roms_cb_t cb, void *user_data)
{
   char path[384];
   size_t _len;
   romm_roms_ctx_t *ctx;

   if (!platform_id || !limit)
      return false;
   _len = (size_t)snprintf(path, sizeof(path),
         "/api/roms?platform_ids=%u&missing=false&order_by=name"
         "&order_dir=asc&with_char_index=false&with_filter_values=false"
         "&with_rom_id_index=false&with_total=false&limit=%u&offset=%u",
         platform_id, limit, offset);
   if (_len >= sizeof(path))
      return false;
   if (!string_is_empty(updated_after))
   {
      const char *c;
      /* Goes into the URL as is. */
      for (c = updated_after; *c; c++)
         if (!ISALNUM((unsigned char)*c) && *c != ':' && *c != '-'
               && *c != '.')
            return false;
      if ((size_t)snprintf(path + _len, sizeof(path) - _len,
            "&updated_after=%s", updated_after) >= sizeof(path) - _len)
         return false;
   }

   if (!(ctx = (romm_roms_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;
   ctx->limit     = limit;

   if (!romm_get(server, token, path, romm_roms_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

static void romm_download_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_download_ctx_t *ctx   = (romm_download_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;

   if (status == ROMM_OK && (!data->data || !data->len))
      status = ROMM_ERR_RESPONSE;
   if (status != ROMM_OK)
      RARCH_DBG(ROMM_PFX "Download failed (status %d, HTTP %d).\n",
            status, http_status);
   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? data->data : NULL,
         status == ROMM_OK ? data->len : 0);
   free(ctx);
}

bool romm_client_download(const char *server, const char *token,
      const char *path, romm_download_cb_t cb, void *user_data)
{
   romm_download_ctx_t *ctx;

   if (!(ctx = (romm_download_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!romm_get_with_headers(server, token, path, "", romm_download_cb,
         ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

static void romm_rom_ids_value(void *ctx, enum rjson_type type,
      const char *value, size_t len)
{
   unsigned **ids = (unsigned**)ctx;
   unsigned id;

   if (type == RJSON_NUMBER && (id = romm_json_uint(value)))
      RBUF_PUSH(*ids, id);
}

static void romm_rom_ids_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_rom_ids_ctx_t *ctx    = (romm_rom_ids_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   unsigned *ids              = NULL;

   if (     status == ROMM_OK
         && !romm_json_values(data->data, data->len, "rom_id_index",
               romm_rom_ids_value, &ids))
      status = ROMM_ERR_RESPONSE;

   if (status != ROMM_OK)
      RARCH_WARN(ROMM_PFX "Couldn't list game ids (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? ids : NULL,
         status == ROMM_OK ? RBUF_LEN(ids) : 0);
   RBUF_FREE(ids);
   free(ctx);
}

bool romm_client_rom_ids(const char *server, const char *token,
      unsigned platform_id, romm_rom_ids_cb_t cb, void *user_data)
{
   char path[384];
   romm_rom_ids_ctx_t *ctx;

   /* The index comes with one page of games, so ask for the smallest;
    * filters and order match romm_client_roms(). */
   if (     !platform_id
         || (size_t)snprintf(path, sizeof(path),
            "/api/roms?platform_ids=%u&missing=false&order_by=name"
            "&order_dir=asc&with_char_index=false&with_filter_values=false"
            "&with_rom_id_index=true&with_total=false&limit=1&offset=0",
            platform_id) >= sizeof(path))
      return false;
   if (!(ctx = (romm_rom_ids_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!romm_get(server, token, path, romm_rom_ids_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

typedef struct
{
   romm_file_t *list;
   romm_file_t cur;
   char full_path[1024];
   /* The game's folder in RomM's library, with a trailing slash */
   char base[1024];
} romm_rom_files_parse_t;

static void romm_rom_files_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_rom_files_parse_t *parse = (romm_rom_files_parse_t*)ctx;
   romm_file_t *f                = &parse->cur;

   if (type != RJSON_STRING && type != RJSON_NUMBER)
      return;
   if (string_is_equal(key, "id"))
      f->id = romm_json_uint(value);
   else if (string_is_equal(key, "file_size_bytes"))
      f->size = (uint64_t)strtoull(value, NULL, 10);
   else if (string_is_equal(key, "crc_hash"))
      strlcpy(f->crc, value, sizeof(f->crc));
   else if (string_is_equal(key, "file_name"))
      strlcpy(f->name, value, sizeof(f->name));
   else if (string_is_equal(key, "full_path"))
      strlcpy(parse->full_path, value, sizeof(parse->full_path));
}

static void romm_rom_files_item_end(void *ctx)
{
   romm_rom_files_parse_t *parse = (romm_rom_files_parse_t*)ctx;
   romm_file_t *f                = &parse->cur;

   if (f->id && *f->name)
   {
      if (     *parse->base
            && string_starts_with(parse->full_path, parse->base)
            && parse->full_path[strlen(parse->base)])
         strlcpy(f->path, parse->full_path + strlen(parse->base),
               sizeof(f->path));
      else
         strlcpy(f->path, f->name, sizeof(f->path));
      RBUF_PUSH(parse->list, *f);
   }
   memset(f, 0, sizeof(*f));
   parse->full_path[0] = '\0';
}

static void romm_rom_files_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_rom_files_ctx_t *ctx  = (romm_rom_files_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   romm_rom_files_parse_t *parse;
   romm_rom_t rom;
   char value[32];
   char fs_path[512];

   memset(&rom, 0, sizeof(rom));
   fs_path[0] = '\0';
   if (!(parse = (romm_rom_files_parse_t*)calloc(1, sizeof(*parse))))
      status = ROMM_ERR_RESPONSE;
   else if (status == ROMM_OK)
   {
      if (romm_json_get(data->data, data->len, "id", value, sizeof(value)))
         rom.id = romm_json_uint(value);
      if (romm_json_get(data->data, data->len, "platform_id",
            value, sizeof(value)))
         rom.platform_id = romm_json_uint(value);
      if (romm_json_get(data->data, data->len, "has_simple_single_file",
            value, sizeof(value)))
         rom.single_file = string_is_equal(value, "true");
      romm_json_get(data->data, data->len, "fs_name",
            rom.fs_name, sizeof(rom.fs_name));
      romm_json_get(data->data, data->len, "fs_name_no_ext",
            rom.fs_name_no_ext, sizeof(rom.fs_name_no_ext));
      romm_json_get(data->data, data->len, "name", rom.name,
            sizeof(rom.name));
      romm_json_get(data->data, data->len, "crc_hash", rom.crc,
            sizeof(rom.crc));
      romm_json_get(data->data, data->len, "updated_at", rom.updated_at,
            sizeof(rom.updated_at));
      romm_json_get(data->data, data->len, "fs_path", fs_path,
            sizeof(fs_path));
      if (!*rom.name)
         strlcpy(rom.name, rom.fs_name_no_ext, sizeof(rom.name));
      if (*fs_path && *rom.fs_name)
         snprintf(parse->base, sizeof(parse->base), "%s/%s/", fs_path,
               rom.fs_name);

      if (     !rom.id || !*rom.fs_name
            || !romm_json_items(data->data, data->len, "files",
                  romm_rom_files_member, romm_rom_files_item_end, parse))
         status = ROMM_ERR_RESPONSE;
   }

   if (status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "Game %u has %u files.\n", rom.id,
            (unsigned)RBUF_LEN(parse->list));
   else
      RARCH_WARN(ROMM_PFX "Couldn't get the game (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? &rom : NULL,
         status == ROMM_OK ? parse->list : NULL,
         status == ROMM_OK ? RBUF_LEN(parse->list) : 0);
   if (parse)
   {
      RBUF_FREE(parse->list);
      free(parse);
   }
   free(ctx);
}

bool romm_client_rom_files(const char *server, const char *token,
      unsigned rom_id, romm_rom_files_cb_t cb, void *user_data)
{
   char path[64];
   romm_rom_files_ctx_t *ctx;

   if (!rom_id)
      return false;
   snprintf(path, sizeof(path), "/api/roms/%u", rom_id);
   if (!(ctx = (romm_rom_files_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!romm_get(server, token, path, romm_rom_files_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}


static void romm_content_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_content_ctx_t *ctx    = (romm_content_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data,
         ctx->length ? 206 : 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   void *body                 = NULL;
   size_t len                 = 0;

   /* A range from the start may come back whole from a server that
    * ignores ranges; fine as long as it fits. */
   if (     status == ROMM_ERR_HTTP && data->status == 200
         && !ctx->offset && data->len <= ctx->length)
      status = ROMM_OK;
   if (status == ROMM_OK && ctx->length && data->len > ctx->length)
      status = ROMM_ERR_RESPONSE;

   /* Taken over rather than copied: a disc image runs to hundreds of
    * MB. The transfer's cleanup frees only what is left in @data. */
   if (status == ROMM_OK)
   {
      body       = data->data;
      len        = data->len;
      data->data = NULL;
      data->len  = 0;
   }
   else
      RARCH_WARN(ROMM_PFX "Couldn't download a game file (status %d, "
            "HTTP %d).\n", status, http_status);

   ctx->cb(ctx->user_data, status, http_status, body, len);
   free(ctx);
}

void *romm_client_rom_content(const char *server, const char *token,
      unsigned rom_id, unsigned file_id, const char *file_name,
      uint64_t offset, uint64_t length,
      romm_content_cb_t cb, void *user_data)
{
   char name[768];
   char path[1024];
   char range[64];
   size_t _len;
   romm_content_ctx_t *ctx;
   void *transfer;

   /* The HTTP task refuses a GET to a URL already in the queue, and the
    * range just fetched is still there while its callback starts the
    * next; RomM ignores the extra parameter. */
   if (     !rom_id || !file_id || string_is_empty(file_name)
         || !romm_url_escape(name, sizeof(name), file_name))
      return NULL;
   _len = (size_t)snprintf(path, sizeof(path),
         "/api/roms/%u/content/%s?file_ids=%u&purpose=play",
         rom_id, name, file_id);
   if (length && _len < sizeof(path))
      _len += (size_t)snprintf(path + _len, sizeof(path) - _len,
            "&range_start=%llu", (unsigned long long)offset);
   if (_len >= sizeof(path))
      return NULL;
   range[0] = '\0';
   if (length)
      snprintf(range, sizeof(range), "Range: bytes=%llu-%llu\r\n",
            (unsigned long long)offset,
            (unsigned long long)(offset + length - 1));
   if (!(ctx = (romm_content_ctx_t*)malloc(sizeof(*ctx))))
      return NULL;
   ctx->cb        = cb;
   ctx->user_data = user_data;
   ctx->offset    = offset;
   ctx->length    = length;

   if (!(transfer = romm_get_with_headers(server, token, path, range,
         romm_content_cb, ctx)))
      free(ctx);
   return transfer;
}

typedef struct
{
   romm_firmware_t *list;
   romm_firmware_t cur;
} romm_firmware_parse_t;

static void romm_firmware_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_firmware_t *f = &((romm_firmware_parse_t*)ctx)->cur;

   if (string_is_equal(key, "is_verified"))
      f->verified = type == RJSON_TRUE;
   else if (type != RJSON_STRING && type != RJSON_NUMBER)
      return;
   else if (string_is_equal(key, "id"))
      f->id = romm_json_uint(value);
   else if (string_is_equal(key, "file_size_bytes"))
      f->size = (uint64_t)strtoull(value, NULL, 10);
   else if (string_is_equal(key, "crc_hash"))
      strlcpy(f->crc, value, sizeof(f->crc));
   else if (string_is_equal(key, "file_name"))
      strlcpy(f->name, value, sizeof(f->name));
}

static void romm_firmware_item_end(void *ctx)
{
   romm_firmware_parse_t *parse = (romm_firmware_parse_t*)ctx;

   if (parse->cur.id && *parse->cur.name)
      RBUF_PUSH(parse->list, parse->cur);
   memset(&parse->cur, 0, sizeof(parse->cur));
}

static void romm_firmware_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_firmware_ctx_t *ctx   = (romm_firmware_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   romm_firmware_parse_t parse;

   memset(&parse, 0, sizeof(parse));
   if (     status == ROMM_OK
         && !romm_json_items(data->data, data->len, NULL,
               romm_firmware_member, romm_firmware_item_end, &parse))
      status = ROMM_ERR_RESPONSE;

   if (status != ROMM_OK)
      RARCH_WARN(ROMM_PFX "Couldn't list firmware (status %d, HTTP %d).\n",
            status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? parse.list : NULL,
         status == ROMM_OK ? RBUF_LEN(parse.list) : 0);
   RBUF_FREE(parse.list);
   free(ctx);
}

bool romm_client_firmware(const char *server, const char *token,
      unsigned platform_id, romm_firmware_cb_t cb, void *user_data)
{
   char path[64];
   romm_firmware_ctx_t *ctx;

   if (!platform_id)
      return false;
   snprintf(path, sizeof(path), "/api/firmware?platform_id=%u",
         platform_id);
   if (!(ctx = (romm_firmware_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;

   if (!romm_get(server, token, path, romm_firmware_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

bool romm_client_firmware_content(const char *server, const char *token,
      unsigned firmware_id, const char *file_name,
      romm_download_cb_t cb, void *user_data)
{
   char name[768];
   char path[1024];

   if (     !firmware_id || string_is_empty(file_name)
         || !romm_url_escape(name, sizeof(name), file_name))
      return false;
   snprintf(path, sizeof(path), "/api/firmware/%u/content/%s",
         firmware_id, name);
   return romm_client_download(server, token, path, cb, user_data);
}

typedef struct
{
   romm_save_t *list;
   romm_save_t cur;
} romm_saves_parse_t;

static void romm_saves_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_save_t *s = &((romm_saves_parse_t*)ctx)->cur;

   if (type != RJSON_STRING && type != RJSON_NUMBER)
      return;
   if (string_is_equal(key, "id"))
      s->id = romm_json_uint(value);
   else if (string_is_equal(key, "rom_id"))
      s->rom_id = romm_json_uint(value);
   else if (string_is_equal(key, "file_size_bytes"))
      s->size = (uint64_t)strtoull(value, NULL, 10);
   else if (string_is_equal(key, "file_name"))
      strlcpy(s->file_name, value, sizeof(s->file_name));
   else if (string_is_equal(key, "emulator"))
      strlcpy(s->emulator, value, sizeof(s->emulator));
   else if (string_is_equal(key, "slot"))
      strlcpy(s->slot, value, sizeof(s->slot));
   else if (string_is_equal(key, "content_hash"))
      strlcpy(s->content_hash, value, sizeof(s->content_hash));
   else if (string_is_equal(key, "updated_at"))
      strlcpy(s->updated_at, value, sizeof(s->updated_at));
}

static void romm_saves_item_end(void *ctx)
{
   romm_saves_parse_t *parse = (romm_saves_parse_t*)ctx;

   if (parse->cur.id && *parse->cur.file_name)
      RBUF_PUSH(parse->list, parse->cur);
   memset(&parse->cur, 0, sizeof(parse->cur));
}

static void romm_saves_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_saves_ctx_t *ctx      = (romm_saves_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;
   romm_saves_parse_t *parse;

   if (!(parse = (romm_saves_parse_t*)calloc(1, sizeof(*parse))))
      status = ROMM_ERR_RESPONSE;
   else if (status == ROMM_OK
         && !romm_json_items(data->data, data->len, NULL,
               romm_saves_member, romm_saves_item_end, parse))
      status = ROMM_ERR_RESPONSE;

   if (status == ROMM_OK)
      RARCH_DBG(ROMM_PFX "RomM has %u %s for the game.\n",
            (unsigned)RBUF_LEN(parse->list), ctx->what);
   else
      RARCH_WARN(ROMM_PFX "Couldn't list %s (status %d, HTTP %d).\n",
            ctx->what, status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? parse->list : NULL,
         status == ROMM_OK ? RBUF_LEN(parse->list) : 0);
   if (parse)
   {
      RBUF_FREE(parse->list);
      free(parse);
   }
   free(ctx);
}

static bool romm_list_saves(const char *server, const char *token,
      const char *path_fmt, const char *what, unsigned rom_id,
      romm_saves_cb_t cb, void *user_data)
{
   char path[64];
   romm_saves_ctx_t *ctx;

   if (!rom_id)
      return false;
   snprintf(path, sizeof(path), path_fmt, rom_id);
   if (!(ctx = (romm_saves_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;
   ctx->what      = what;

   if (!romm_get(server, token, path, romm_saves_cb, ctx))
   {
      free(ctx);
      return false;
   }
   return true;
}

bool romm_client_saves(const char *server, const char *token,
      unsigned rom_id, romm_saves_cb_t cb, void *user_data)
{
   return romm_list_saves(server, token, "/api/saves?rom_id=%u", "saves",
         rom_id, cb, user_data);
}

bool romm_client_states(const char *server, const char *token,
      unsigned rom_id, romm_saves_cb_t cb, void *user_data)
{
   return romm_list_saves(server, token, "/api/states?rom_id=%u",
         "save states", rom_id, cb, user_data);
}

bool romm_client_save_content(const char *server, const char *token,
      unsigned save_id, const char *device_id,
      romm_download_cb_t cb, void *user_data)
{
   char id[192];
   char path[256];

   if (!save_id)
      return false;
   if (string_is_empty(device_id))
      snprintf(path, sizeof(path), "/api/saves/%u/content", save_id);
   else
   {
      if (!romm_url_escape(id, sizeof(id), device_id))
         return false;
      snprintf(path, sizeof(path), "/api/saves/%u/content?device_id=%s",
            save_id, id);
   }
   return romm_client_download(server, token, path, cb, user_data);
}

bool romm_client_state_content(const char *server, const char *token,
      unsigned state_id, romm_download_cb_t cb, void *user_data)
{
   char path[64];

   if (!state_id)
      return false;
   snprintf(path, sizeof(path), "/api/states/%u/content", state_id);
   return romm_client_download(server, token, path, cb, user_data);
}

/* RomM's reply to a request answered with a save, into @save. */
static enum romm_status romm_save_reply(const http_transfer_data_t *data,
      romm_save_t *save)
{
   enum romm_status status = romm_response_status(data, 200);
   char value[32];

   memset(save, 0, sizeof(*save));
   if (status == ROMM_ERR_HTTP && data->status == 409)
      return ROMM_ERR_CONFLICT;
   if (status != ROMM_OK)
      return status;
   if (romm_json_get(data->data, data->len, "id", value, sizeof(value)))
      save->id = romm_json_uint(value);
   if (romm_json_get(data->data, data->len, "rom_id",
         value, sizeof(value)))
      save->rom_id = romm_json_uint(value);
   if (romm_json_get(data->data, data->len, "file_size_bytes",
         value, sizeof(value)))
      save->size = (uint64_t)strtoull(value, NULL, 10);
   romm_json_get(data->data, data->len, "file_name", save->file_name,
         sizeof(save->file_name));
   romm_json_get(data->data, data->len, "emulator", save->emulator,
         sizeof(save->emulator));
   romm_json_get(data->data, data->len, "slot", save->slot,
         sizeof(save->slot));
   romm_json_get(data->data, data->len, "content_hash",
         save->content_hash, sizeof(save->content_hash));
   romm_json_get(data->data, data->len, "updated_at", save->updated_at,
         sizeof(save->updated_at));
   return save->id && *save->file_name ? ROMM_OK : ROMM_ERR_RESPONSE;
}

typedef struct
{
   romm_done_cb_t cb;
   void *user_data;
} romm_done_ctx_t;

static void romm_states_delete_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_done_ctx_t *ctx       = (romm_done_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   enum romm_status status    = romm_response_status(data, 200);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;

   if (status != ROMM_OK)
      RARCH_WARN(ROMM_PFX "Couldn't delete save states (status %d, HTTP "
            "%d).\n", status, http_status);
   ctx->cb(ctx->user_data, status, http_status);
   free(ctx);
}

bool romm_client_states_delete(const char *server, const char *token,
      const unsigned *ids, size_t count, romm_done_cb_t cb,
      void *user_data)
{
   char url[2048];
   char headers[512];
   char *body;
   size_t _len, cap, i;
   romm_done_ctx_t *ctx;
   bool pushed = false;

   if (     !ids || !count || string_is_empty(token)
         || !romm_client_url(url, sizeof(url), server,
               "/api/states/delete")
         || (size_t)snprintf(headers, sizeof(headers),
               "Authorization: Bearer %s\r\n" ROMM_HEADERS_JSON, token)
            >= sizeof(headers))
      return false;
   /* {"states": [ids]}: at most 11 bytes an id. */
   cap = 16 + count * 12;
   if (!(body = (char*)malloc(cap)))
      return false;
   _len = strlcpy(body, "{\"states\": [", cap);
   for (i = 0; i < count; i++)
      _len += (size_t)snprintf(body + _len, cap - _len, "%s%u",
            i ? "," : "", ids[i]);
   strlcpy(body + _len, "]}", cap - _len);

   if ((ctx = (romm_done_ctx_t*)malloc(sizeof(*ctx))))
   {
      ctx->cb        = cb;
      ctx->user_data = user_data;
      if (!(pushed = task_push_http_transfer_with_content(url, "POST",
            body, strlen(body), "application/json", true, false, headers,
            romm_states_delete_cb, ctx) != NULL))
         free(ctx);
   }
   free(body);
   return pushed;
}

static void romm_upload_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_upload_ctx_t *ctx     = (romm_upload_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   romm_save_t save;
   enum romm_status status    = romm_save_reply(data, &save);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;

   if (status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "Uploaded %s %u as %s.\n", ctx->what, save.id,
            save.file_name);
   else
      RARCH_WARN(ROMM_PFX "Couldn't upload a %s (status %d, HTTP %d).\n",
            ctx->what, status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? &save : NULL);
   free(ctx);
}

/* Appends "&@key=@value", @value percent-encoded, at @s + *@_len. */
static bool romm_query_add(char *s, size_t len, size_t *_len,
      const char *key, const char *value)
{
   char esc[1024];
   size_t n;

   if (!romm_url_escape(esc, sizeof(esc), value))
      return false;
   n = (size_t)snprintf(s + *_len, len - *_len, "&%s=%s", key, esc);
   if (n >= len - *_len)
      return false;
   *_len += n;
   return true;
}

/* A multipart/form-data body holding @data as file @file_name in form
 * field @field; NULL when out of memory. */
static char *romm_form_body(const char *boundary, const char *field,
      const char *file_name, const void *data, size_t len,
      size_t *body_len)
{
   char head[1536];
   char tail[96];
   char name[1024];
   size_t head_len, tail_len, pos = 0;
   const char *c;
   char *body;

   /* A quoted string: backslash before '"' and '\'. */
   for (c = file_name; *c && pos + 2 < sizeof(name); c++)
   {
      if (*c == '"' || *c == '\\')
         name[pos++] = '\\';
      name[pos++] = (*c == '\r' || *c == '\n') ? ' ' : *c;
   }
   name[pos] = '\0';

   head_len = (size_t)snprintf(head, sizeof(head),
         "--%s\r\nContent-Disposition: form-data; name=\"%s\";"
         " filename=\"%s\"\r\nContent-Type: application/octet-stream"
         "\r\n\r\n", boundary, field, name);
   tail_len = (size_t)snprintf(tail, sizeof(tail), "\r\n--%s--\r\n",
         boundary);
   if (head_len >= sizeof(head) || tail_len >= sizeof(tail))
      return NULL;
   if (!(body = (char*)malloc(head_len + len + tail_len)))
      return NULL;
   memcpy(body, head, head_len);
   if (len)
      memcpy(body + head_len, data, len);
   memcpy(body + head_len + len, tail, tail_len);
   *body_len = head_len + len + tail_len;
   return body;
}

static bool romm_upload(const romm_upload_t *up, bool is_state,
      romm_save_cb_t cb, void *user_data)
{
   static unsigned counter;
   char path[3072];
   char url[4096];
   char headers[512];
   char boundary[64];
   char content_type[96];
   size_t _len, body_len = 0;
   romm_upload_ctx_t *ctx;
   char *body;
   bool pushed = false;

   if (     !up || !up->rom_id || string_is_empty(up->token)
         || string_is_empty(up->file_name) || (!up->data && up->len))
      return false;

   _len = (size_t)snprintf(path, sizeof(path), "/api/%s?rom_id=%u",
         is_state ? "states" : "saves", up->rom_id);
   if (     (!string_is_empty(up->emulator)
            && !romm_query_add(path, sizeof(path), &_len, "emulator",
                  up->emulator))
         || (!is_state && !string_is_empty(up->slot)
            && !romm_query_add(path, sizeof(path), &_len, "slot",
                  up->slot))
         || (!is_state && !string_is_empty(up->device_id)
            && !romm_query_add(path, sizeof(path), &_len, "device_id",
                  up->device_id))
         || (!is_state && !string_is_empty(up->content_hash)
            && !romm_query_add(path, sizeof(path), &_len, "content_hash",
                  up->content_hash)))
      return false;
   if (!is_state && !string_is_empty(up->slot) && up->keep)
   {
      size_t n = (size_t)snprintf(path + _len, sizeof(path) - _len,
            "&autocleanup=true&autocleanup_limit=%u", up->keep);
      if (n >= sizeof(path) - _len)
         return false;
      _len += n;
   }
   if (!is_state && up->overwrite)
   {
      if (_len + STRLEN_CONST("&overwrite=true") >= sizeof(path))
         return false;
      _len += strlcpy(path + _len, "&overwrite=true", sizeof(path) - _len);
   }
   if (     !romm_client_url(url, sizeof(url), up->server, path)
         || (size_t)snprintf(headers, sizeof(headers),
               "Authorization: Bearer %s\r\n" ROMM_HEADERS_JSON, up->token)
            >= sizeof(headers))
      return false;

   snprintf(boundary, sizeof(boundary), "RommarchForm%08x%08x",
         (unsigned)cpu_features_get_time_usec(), ++counter);
   snprintf(content_type, sizeof(content_type),
         "multipart/form-data; boundary=%s", boundary);
   if (!(body = romm_form_body(boundary,
         is_state ? "stateFile" : "saveFile", up->file_name, up->data,
         up->len, &body_len)))
      return false;

   if ((ctx = (romm_upload_ctx_t*)malloc(sizeof(*ctx))))
   {
      ctx->cb        = cb;
      ctx->user_data = user_data;
      ctx->what      = is_state ? "save state" : "save";
      /* The body is copied into the request. */
      if (!(pushed = task_push_http_transfer_with_content(url, "POST",
            body, body_len, content_type, true, false, headers,
            romm_upload_cb, ctx) != NULL))
         free(ctx);
   }
   free(body);
   return pushed;
}

bool romm_client_save_upload(const romm_upload_t *up,
      romm_save_cb_t cb, void *user_data)
{
   return romm_upload(up, false, cb, user_data);
}

bool romm_client_state_upload(const romm_upload_t *up,
      romm_save_cb_t cb, void *user_data)
{
   return romm_upload(up, true, cb, user_data);
}

static void romm_save_downloaded_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_upload_ctx_t *ctx     = (romm_upload_ctx_t*)user_data;
   http_transfer_data_t *data = (http_transfer_data_t*)task_data;
   romm_save_t save;
   enum romm_status status    = romm_save_reply(data, &save);
   int http_status            = status == ROMM_ERR_UNREACHABLE
         ? 0 : data->status;

   if (status == ROMM_OK)
      RARCH_LOG(ROMM_PFX "Told RomM this device has save %u.\n", save.id);
   else
      RARCH_WARN(ROMM_PFX "Couldn't tell RomM this device has a save "
            "(status %d, HTTP %d).\n", status, http_status);

   ctx->cb(ctx->user_data, status, http_status,
         status == ROMM_OK ? &save : NULL);
   free(ctx);
}

bool romm_client_save_downloaded(const char *server, const char *token,
      unsigned save_id, const char *device_id, const char *content_hash,
      romm_save_cb_t cb, void *user_data)
{
   char path[64];
   char url[2048];
   char headers[512];
   rjsonwriter_t *writer;
   romm_upload_ctx_t *ctx;
   const char *body;
   int body_len = 0;
   bool pushed  = false;

   if (     !save_id || string_is_empty(token)
         || string_is_empty(device_id))
      return false;
   snprintf(path, sizeof(path), "/api/saves/%u/downloaded", save_id);
   if (     !romm_client_url(url, sizeof(url), server, path)
         || (size_t)snprintf(headers, sizeof(headers),
               "Authorization: Bearer %s\r\n" ROMM_HEADERS_JSON, token)
            >= sizeof(headers))
      return false;
   if (!(ctx = (romm_upload_ctx_t*)malloc(sizeof(*ctx))))
      return false;
   ctx->cb        = cb;
   ctx->user_data = user_data;
   ctx->what      = "save";

   if (!(writer = rjsonwriter_open_memory()))
   {
      free(ctx);
      return false;
   }
   rjsonwriter_raw(writer, "{", 1);
   rjsonwriter_add_string(writer, "device_id");
   rjsonwriter_raw(writer, ":", 1);
   rjsonwriter_add_string(writer, device_id);
   if (!string_is_empty(content_hash))
   {
      rjsonwriter_raw(writer, ",", 1);
      rjsonwriter_add_string(writer, "content_hash");
      rjsonwriter_raw(writer, ":", 1);
      rjsonwriter_add_string(writer, content_hash);
   }
   rjsonwriter_raw(writer, "}", 1);

   if ((body = rjsonwriter_get_memory_buffer(writer, &body_len)))
      pushed = task_push_http_transfer_with_content(url, "POST",
            body, (size_t)body_len, "application/json", true, false,
            headers, romm_save_downloaded_cb, ctx) != NULL;
   rjsonwriter_free(writer);

   if (!pushed)
      free(ctx);
   return pushed;
}
