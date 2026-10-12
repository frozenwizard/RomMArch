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
#include <file/file_path.h>
#include <string/stdstring.h>

#include "romm.h"
#include "romm_client.h"
#include "romm_library.h"
#include "romm_platform.h"
#include "../../command.h"
#include "../../configuration.h"
#include "../../core_info.h"
#include "../../msg_hash.h"
#include "../../playlist.h"
#include "../../runloop.h"
#include "../../version.h"
#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

/* Set from the moment pairing starts until its wait ends. */
static bool romm_pairing = false;
static bool romm_syncing = false;

static void romm_notify(const char *msg,
      enum message_queue_category category)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, category);
}

/* Reports why a request to the configured server failed. */
static void romm_notify_failure(enum romm_status status, int http_status)
{
   settings_t *settings = config_get_ptr();
   const char *server   = settings->arrays.romm_server_url;
   char msg[512];

   switch (status)
   {
      case ROMM_ERR_UNREACHABLE:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_UNREACHABLE), server);
         break;
      case ROMM_ERR_HTTP:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_HTTP_ERROR), http_status);
         break;
      case ROMM_ERR_DENIED:
         strlcpy(msg, msg_hash_to_str(MSG_ROMM_PAIR_DENIED), sizeof(msg));
         break;
      case ROMM_ERR_EXPIRED:
         strlcpy(msg, msg_hash_to_str(MSG_ROMM_PAIR_EXPIRED), sizeof(msg));
         break;
      case ROMM_ERR_UNAUTHORIZED:
         strlcpy(msg, msg_hash_to_str(MSG_ROMM_UNAUTHORIZED), sizeof(msg));
         break;
      case ROMM_ERR_WRITE:
         strlcpy(msg, msg_hash_to_str(MSG_ROMM_WRITE_FAILED), sizeof(msg));
         break;
      default:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_NOT_ROMM), server);
         break;
   }

   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
}

static void romm_check_token_cb(void *user_data,
      enum romm_status status, int http_status, const char *username)
{
   char *version = (char*)user_data;
   char msg[512];

   if (status == ROMM_OK)
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CONNECTED_AS),
            version, username);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
   }
   else
      romm_notify_failure(status, http_status);
   free(version);
}

static void romm_check_connection_cb(void *user_data,
      enum romm_status status, int http_status, const char *version)
{
   settings_t *settings = config_get_ptr();
   char *version_copy;
   char msg[512];

   if (status != ROMM_OK)
   {
      romm_notify_failure(status, http_status);
      return;
   }

   /* Paired: the token has to work too. */
   if (     !string_is_empty(settings->arrays.romm_token)
         && (version_copy = strdup(version)))
   {
      if (romm_client_whoami(settings->arrays.romm_server_url,
            settings->arrays.romm_token, romm_check_token_cb, version_copy))
         return;
      free(version_copy);
   }

   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CONNECTED), version);
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
}

void romm_check_connection(void)
{
   settings_t *settings = config_get_ptr();
   const char *server   = settings->arrays.romm_server_url;

   if (string_is_empty(server))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NO_SERVER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }

   if (!romm_client_heartbeat(server, romm_check_connection_cb, NULL))
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
}

static void romm_on_pair_approved(void *user_data, enum romm_status status,
      const char *token, const char *device_id)
{
   settings_t *settings = config_get_ptr();

   romm_pairing = false;

   if (status == ROMM_ERR_CANCELLED)
      return;
   if (status != ROMM_OK)
   {
      romm_notify_failure(status, 0);
      return;
   }

   strlcpy(settings->arrays.romm_token, token,
         sizeof(settings->arrays.romm_token));
   /* Keep the pairing even if RetroArch doesn't save on exit. */
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   romm_notify(msg_hash_to_str(MSG_ROMM_PAIRED),
         MESSAGE_QUEUE_CATEGORY_SUCCESS);
}

static void romm_on_pair_started(void *user_data, enum romm_status status,
      int http_status, const romm_pairing_t *pairing)
{
   settings_t *settings = config_get_ptr();
   char title[256];

   if (status != ROMM_OK)
   {
      romm_pairing = false;
      romm_notify_failure(status, http_status);
      return;
   }

   snprintf(title, sizeof(title),
         msg_hash_to_str(MSG_ROMM_PAIR_APPROVE), pairing->user_code);
   if (!romm_client_pair_wait(settings->arrays.romm_server_url, pairing,
         title, romm_on_pair_approved, NULL))
   {
      romm_pairing = false;
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return;
   }
   romm_platform_open_url(pairing->approve_url);
}

void romm_pair(void)
{
   settings_t *settings = config_get_ptr();
   char *identifier     = settings->arrays.romm_device_identifier;
   char device_name[128];

   if (romm_pairing)
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_PAIR_IN_PROGRESS),
            MESSAGE_QUEUE_CATEGORY_INFO);
      return;
   }
   if (string_is_empty(settings->arrays.romm_server_url))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NO_SERVER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }

   if (string_is_empty(identifier))
      romm_platform_new_identifier(identifier,
            sizeof(settings->arrays.romm_device_identifier));
   romm_platform_device_name(device_name, sizeof(device_name));

   if (!romm_client_pair_start(settings->arrays.romm_server_url,
         settings->arrays.romm_web_url, identifier, device_name,
         PACKAGE_VERSION, romm_on_pair_started, NULL))
   {
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return;
   }
   romm_pairing = true;
}

/* The menu shows playlists from a cached copy and lists them in its
 * sidebar; both have to be reloaded after a sync rewrote the files. */
static void romm_reload_playlists(const romm_library_t *lib)
{
   settings_t *settings     = config_get_ptr();
   playlist_t *cached       = playlist_get_cached();
#ifdef HAVE_MENU
   struct menu_state *menu_st = menu_state_get_ptr();
#endif
   size_t i;

   if (cached)
   {
      for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      {
         char path[PATH_MAX_LENGTH];
         fill_pathname_join(path, settings->paths.directory_playlist,
               lib->platforms[i].playlist, sizeof(path));
         if (string_is_equal(path, playlist_get_conf_path(cached)))
         {
            playlist_config_t config;
            if (playlist_config_copy(playlist_get_config(cached), &config))
            {
               playlist_free_cached();
               playlist_init_cached(&config);
            }
            break;
         }
      }
   }

#ifdef HAVE_MENU
   if (menu_st->driver_ctx && menu_st->driver_ctx->environ_cb)
      menu_st->driver_ctx->environ_cb(MENU_ENVIRON_RESET_HORIZONTAL_LIST,
            NULL, menu_st->userdata);
   menu_st->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
#endif
}

static bool romm_find_core(const char *const *cores, char *path,
      size_t path_len, char *name, size_t name_len)
{
   core_info_list_t *list = NULL;
   size_t i, j;

   if (!core_info_get_list(&list) || !list)
      return false;
   for (i = 0; cores[i]; i++)
   {
      char file_name[NAME_MAX_LENGTH];
      size_t _len = strlcpy(file_name, cores[i], sizeof(file_name));
      strlcpy(file_name + _len, "_libretro.", sizeof(file_name) - _len);
      for (j = 0; j < list->count; j++)
      {
         const core_info_t *info = &list->list[j];
         if (     info->path
               && string_starts_with(path_basename(info->path), file_name))
         {
            strlcpy(path, info->path, path_len);
            strlcpy(name, info->display_name ? info->display_name
                  : cores[i], name_len);
            return true;
         }
      }
   }
   return false;
}

static void romm_on_library_synced(void *user_data, enum romm_status status,
      int http_status, const romm_library_t *lib)
{
   size_t games = 0;
   size_t clashes;
   size_t i;
   char msg[256];

   romm_syncing = false;
   romm_reload_playlists(lib);

   if (status == ROMM_ERR_CANCELLED)
      return;
   if (status != ROMM_OK)
   {
      romm_notify_failure(status, http_status);
      return;
   }
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      games += RBUF_LEN(lib->platforms[i].roms);
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SYNCED),
         (unsigned)games, (unsigned)RBUF_LEN(lib->platforms));
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);

   if ((clashes = romm_library_save_name_clashes(lib)))
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SAVE_NAME_CLASHES),
            (unsigned)clashes);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_WARNING);
   }
}

void romm_refresh_library(void)
{
   settings_t *settings = config_get_ptr();
   romm_library_sync_params_t params;

   if (romm_syncing)
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_SYNC_IN_PROGRESS),
            MESSAGE_QUEUE_CATEGORY_INFO);
      return;
   }
   if (string_is_empty(settings->arrays.romm_server_url))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NO_SERVER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   if (string_is_empty(settings->arrays.romm_token))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NOT_PAIRED),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }

   params.server         = settings->arrays.romm_server_url;
   params.token          = settings->arrays.romm_token;
   params.playlist_dir   = settings->paths.directory_playlist;
   params.thumbnails_dir = settings->paths.directory_thumbnails;
   params.title          = msg_hash_to_str(MSG_ROMM_SYNCING);
   params.covers_title   = msg_hash_to_str(MSG_ROMM_FETCHING_COVERS);
   params.find_core      = romm_find_core;
   if (!romm_library_sync(&params, romm_on_library_synced, NULL))
   {
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return;
   }
   romm_syncing = true;
}
