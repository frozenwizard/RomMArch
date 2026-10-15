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
#include <file/config_file.h>
#include <file/file_path.h>
#include <queues/task_queue.h>
#include <string/stdstring.h>

#include "romm.h"
#include "romm_client.h"
#include "romm_launch.h"
#include "romm_library.h"
#include "romm_platform.h"
#include "romm_saves.h"
#include "../../command.h"
#include "../../configuration.h"
#include "../../core_info.h"
#include "../../file_path_special.h"
#include "../../msg_hash.h"
#include "../../paths.h"
#include "../../playlist.h"
#include "../../runloop.h"
#include "../../tasks/task_content.h"
#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"
#if defined(HAVE_MENU) && defined(HAVE_UPDATE_CORES)
#include "../../core_updater_list.h"
#include "../../tasks/tasks_internal.h"
#endif
#include "../../version.h"
#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

/* Set from the moment pairing starts until its wait ends, with the
 * addresses it began with and, once RomM gave a code, the wait.
 * Callbacks of a pairing since replaced carry an older generation. */
static bool romm_pairing = false;
static unsigned romm_pair_gen = 0;
static void *romm_pair_wait = NULL;
static char romm_pair_server[NAME_MAX_LENGTH];
static char romm_pair_web[NAME_MAX_LENGTH];
static bool romm_syncing = false;
/* Check Connection is waiting on RomM. */
static bool romm_checking = false;
/* The running sync was started at startup rather than by the user. */
static bool romm_syncing_quietly = false;

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
      case ROMM_ERR_FORBIDDEN:
         strlcpy(msg, msg_hash_to_str(MSG_ROMM_FORBIDDEN), sizeof(msg));
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

   romm_checking = false;
   if (status == ROMM_OK)
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CONNECTED_AS),
            version, username);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
   }
   else if (status == ROMM_ERR_FORBIDDEN)
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_FORBIDDEN_SCOPE),
            "me.read");
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
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
      romm_checking = false;
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

   romm_checking = false;
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CONNECTED), version);
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
}

void romm_check_connection(void)
{
   settings_t *settings = config_get_ptr();
   const char *server   = settings->arrays.romm_server_url;

   if (romm_checking)
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_CHECK_IN_PROGRESS),
            MESSAGE_QUEUE_CATEGORY_INFO);
      return;
   }
   if (string_is_empty(server))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NO_SERVER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }

   if (!romm_client_heartbeat(server, romm_check_connection_cb, NULL))
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
   else
      romm_checking = true;
}

#ifdef HAVE_CLOUDSYNC
#define ROMM_CLOUD_SYNC_PATH "/api/sync/retroarch/"

enum romm_cloud_sync_setup
{
   ROMM_CLOUD_SYNC_SET_UP = 0,
   ROMM_CLOUD_SYNC_ELSEWHERE,
   ROMM_CLOUD_SYNC_FAILED
};

/* Whether @scope is in @list, as pairing names missing scopes
 * ("a, b"). */
static bool romm_scope_listed(const char *list, const char *scope)
{
   size_t      _len = strlen(scope);
   const char *p    = list;
   while ((p = strstr(p, scope)))
   {
      if (     (p == list || p[-1] == ' ')
            && (p[_len] == ',' || p[_len] == '\0'))
         return true;
      p += _len;
   }
   return false;
}

/* Whether Cloud Sync is pointed at RomM's WebDAV, as pairing points it,
 * at this server's address or an earlier one. */
static bool romm_cloud_sync_is_romms(settings_t *settings)
{
   return string_is_equal(settings->arrays.cloud_sync_driver, "webdav")
      && string_ends_with(settings->arrays.webdav_url, ROMM_CLOUD_SYNC_PATH);
}

/* Whether to point Cloud Sync at RomM's WebDAV, whose address goes in
 * @url. Cloud Sync the user has on for another service is left as it
 * is. */
static enum romm_cloud_sync_setup romm_cloud_sync_check(
      settings_t *settings, const char *missing_scopes,
      char *url, size_t len)
{
   /* RomM's WebDAV wants both; without them every sync would fail. */
   if (     romm_scope_listed(missing_scopes, "assets.read")
         || romm_scope_listed(missing_scopes, "assets.write"))
   {
      RARCH_WARN("[RomM] Cloud Sync left as it was: it needs the assets permissions pairing left out.\n");
      return ROMM_CLOUD_SYNC_FAILED;
   }
   if (!romm_client_url(url, len,
         settings->arrays.romm_server_url, ROMM_CLOUD_SYNC_PATH))
   {
      RARCH_WARN("[RomM] The server address is too long for Cloud Sync's; Cloud Sync left as it was.\n");
      return ROMM_CLOUD_SYNC_FAILED;
   }
   if (     settings->bools.cloud_sync_enable
         && !romm_cloud_sync_is_romms(settings))
   {
      RARCH_LOG("[RomM] Cloud Sync is on with the %s driver at another address; left as it is.\n",
            settings->arrays.cloud_sync_driver);
      return ROMM_CLOUD_SYNC_ELSEWHERE;
   }
   return ROMM_CLOUD_SYNC_SET_UP;
}

/* Points Cloud Sync at RomM's WebDAV, @url, which takes the device
 * token, for RetroArch's configs only: saves and states sync per game
 * id instead. */
static void romm_cloud_sync_apply(settings_t *settings, const char *url)
{
   configuration_set_string(settings,
         settings->arrays.webdav_url, url);
   configuration_set_string(settings,
         settings->arrays.cloud_sync_driver, "webdav");
   configuration_set_bool(settings,
         settings->bools.cloud_sync_enable, true);
   configuration_set_bool(settings,
         settings->bools.cloud_sync_sync_configs, true);
   configuration_set_bool(settings,
         settings->bools.cloud_sync_sync_saves, false);
   configuration_set_bool(settings,
         settings->bools.cloud_sync_sync_thumbs, false);
   configuration_set_bool(settings,
         settings->bools.cloud_sync_sync_system, false);
   /* A new server: forget the last sync, as changing driver does. */
   task_push_cloud_sync_update_driver();
}

/* A sync still running would go on with the driver it began with and
 * write back the last-sync record the setup clears, so the setup waits
 * for it to end. Main thread, as the pairing callback. */
static void romm_cloud_sync_wait_handler(retro_task_t *task)
{
   settings_t *settings = config_get_ptr();
   char url[sizeof(settings->arrays.webdav_url)];

   if (task_cloud_sync_in_progress())
   {
      task->when = cpu_features_get_time_usec() + 250000;
      return;
   }
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
   if (romm_cloud_sync_check(settings, "", url, sizeof(url))
         != ROMM_CLOUD_SYNC_SET_UP)
      return;
   romm_cloud_sync_apply(settings, url);
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   if (settings->uints.cloud_sync_sync_mode == CLOUD_SYNC_MODE_AUTOMATIC)
      task_push_cloud_sync();
}

static bool romm_cloud_sync_wait(void)
{
   retro_task_t *task = task_init();

   if (!task)
      return false;
   task->handler = romm_cloud_sync_wait_handler;
   task->flags  |= RETRO_TASK_FLG_MAIN_THREAD | RETRO_TASK_FLG_MUTE;
   task->when    = cpu_features_get_time_usec() + 250000;
   if (!task_queue_push(task))
   {
      free(task);
      return false;
   }
   RARCH_LOG("[RomM] Cloud Sync is running; it will be pointed at RomM once it ends.\n");
   return true;
}
#endif

void romm_cloud_sync_follow_server(void)
{
#ifdef HAVE_CLOUDSYNC
   settings_t *settings = config_get_ptr();
   char url[sizeof(settings->arrays.webdav_url)];

   if (     !settings->bools.cloud_sync_enable
         || !romm_cloud_sync_is_romms(settings)
         || !romm_client_url(url, sizeof(url),
               settings->arrays.romm_server_url, ROMM_CLOUD_SYNC_PATH)
         || string_is_equal(url, settings->arrays.webdav_url))
      return;
   if (task_cloud_sync_in_progress())
   {
      RARCH_WARN("[RomM] Cloud Sync is running, so it stays at %s until the next start.\n",
            settings->arrays.webdav_url);
      return;
   }
   RARCH_LOG("[RomM] Cloud Sync follows RomM to %s.\n", url);
   configuration_set_string(settings,
         settings->arrays.webdav_url, url);
   /* A new address may be a new server: forget the last sync. */
   task_push_cloud_sync_update_driver();
#endif
}

/* RetroArch doesn't save its configuration while a game's overrides
 * are on, and unloading them reloads it from disk. */
static bool romm_overrides_active(void)
{
   return (runloop_state_get_ptr()->flags
         & RUNLOOP_FLAG_OVERRIDES_ACTIVE) != 0;
}

static void romm_on_pair_approved(void *user_data, enum romm_status status,
      int http_status, const char *token, const char *device_id,
      const char *missing_scopes)
{
   settings_t *settings = config_get_ptr();
#ifdef HAVE_CLOUDSYNC
   enum romm_cloud_sync_setup cloud_sync;
   bool cloud_sync_now = false;
   char url[sizeof(settings->arrays.webdav_url)];
#endif
   char msg[512];

   if ((uintptr_t)user_data != romm_pair_gen)
      return;
   romm_pairing   = false;
   romm_pair_wait = NULL;

   if (status == ROMM_ERR_CANCELLED)
      return;
   if (status != ROMM_OK)
   {
      romm_notify_failure(status, http_status);
      return;
   }
   /* The token is for the server that issued it. */
   if (!string_is_equal(romm_pair_server, settings->arrays.romm_server_url))
   {
      RARCH_WARN("[RomM] Pairing approved by %s, but the server address is now %s; not kept.\n",
            romm_pair_server, settings->arrays.romm_server_url);
      romm_notify(msg_hash_to_str(MSG_ROMM_PAIR_SERVER_CHANGED),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   /* A game with overrides started while the pairing waited. */
   if (romm_overrides_active())
   {
      RARCH_WARN("[RomM] Pairing approved while a game's overrides are on, which keep the configuration from being saved; not kept.\n");
      romm_notify(msg_hash_to_str(MSG_ROMM_PAIR_OVERRIDES),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }

   strlcpy(settings->arrays.romm_token, token,
         sizeof(settings->arrays.romm_token));
   strlcpy(settings->arrays.romm_device_id, device_id,
         sizeof(settings->arrays.romm_device_id));
#ifdef HAVE_CLOUDSYNC
   cloud_sync = romm_cloud_sync_check(settings, missing_scopes,
         url, sizeof(url));
   if (cloud_sync == ROMM_CLOUD_SYNC_SET_UP)
   {
      if (!task_cloud_sync_in_progress())
      {
         romm_cloud_sync_apply(settings, url);
         cloud_sync_now = true;
      }
      else if (!romm_cloud_sync_wait())
         cloud_sync = ROMM_CLOUD_SYNC_FAILED;
   }
#endif
   /* Keep the pairing even if RetroArch doesn't save on exit. */
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
#ifdef HAVE_CLOUDSYNC
   if (     cloud_sync_now
         && settings->uints.cloud_sync_sync_mode == CLOUD_SYNC_MODE_AUTOMATIC)
      task_push_cloud_sync();
#endif
   if (*missing_scopes)
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_ROMM_PAIRED_MISSING_SCOPES), missing_scopes);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_WARNING);
   }
#ifdef HAVE_CLOUDSYNC
   else if (cloud_sync == ROMM_CLOUD_SYNC_ELSEWHERE)
      romm_notify(msg_hash_to_str(MSG_ROMM_PAIRED_CLOUD_SYNC_ELSEWHERE),
            MESSAGE_QUEUE_CATEGORY_SUCCESS);
#endif
   else
      romm_notify(msg_hash_to_str(MSG_ROMM_PAIRED),
            MESSAGE_QUEUE_CATEGORY_SUCCESS);
}

static void romm_on_pair_started(void *user_data, enum romm_status status,
      int http_status, const romm_pairing_t *pairing)
{
   char title[256];

   if ((uintptr_t)user_data != romm_pair_gen)
      return;
   if (status != ROMM_OK)
   {
      romm_pairing = false;
      romm_notify_failure(status, http_status);
      return;
   }

   snprintf(title, sizeof(title),
         msg_hash_to_str(MSG_ROMM_PAIR_APPROVE), pairing->user_code);
   if (!(romm_pair_wait = romm_client_pair_wait(romm_pair_server, pairing,
         title, romm_on_pair_approved, user_data)))
   {
      romm_pairing = false;
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return;
   }
   romm_platform_open_url(pairing->approve_url);
}

/* Ends the pairing running, if any; its callbacks are then ignored. */
static void romm_pair_stop(void)
{
   if (!romm_pairing)
      return;
   RARCH_LOG("[RomM] The pairing running ends.\n");
   romm_pairing = false;
   romm_pair_gen++;
   if (romm_pair_wait)
      romm_client_pair_cancel(romm_pair_wait);
   romm_pair_wait = NULL;
}

void romm_pair(void)
{
   settings_t *settings = config_get_ptr();
   char *identifier     = settings->arrays.romm_device_identifier;
   char device_name[128];

   if (romm_overrides_active())
   {
      RARCH_WARN("[RomM] Not pairing while a game's overrides are on, which keep the configuration from being saved.\n");
      romm_notify(msg_hash_to_str(MSG_ROMM_PAIR_OVERRIDES),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   /* Pressed again, say after fixing an address: start over. */
   romm_pair_stop();
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
         PACKAGE_VERSION, romm_on_pair_started,
         (void*)(uintptr_t)romm_pair_gen))
   {
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return;
   }
   romm_pairing = true;
   strlcpy(romm_pair_server, settings->arrays.romm_server_url,
         sizeof(romm_pair_server));
   strlcpy(romm_pair_web, settings->arrays.romm_web_url,
         sizeof(romm_pair_web));
}

void romm_pair_follow_address(void)
{
   settings_t *settings = config_get_ptr();

   /* The menu also calls this as a field opens, unchanged. */
   if (     romm_pairing
         && (  !string_is_equal(romm_pair_server,
                  settings->arrays.romm_server_url)
            || !string_is_equal(romm_pair_web,
                  settings->arrays.romm_web_url)))
      romm_pair();
}

/* The menu shows a playlist from a copy in memory, and writes that
 * copy back whole when a game's core is set, so a copy older than the
 * file the sync just wrote, or deleted, would undo the sync. Main
 * thread, as the sync's handler. */
static void romm_playlist_changed(const char *path)
{
   playlist_t *cached = playlist_get_cached();
   bool refresh       = false;

   /* A read under way may have the file as it was. */
   if (playlist_init_cached_pending())
   {
      playlist_init_cached_defer_abort();
      refresh = true;
   }
   if (cached && string_is_equal(path, playlist_get_conf_path(cached)))
   {
      playlist_config_t config;
      if (!path_is_valid(path))
         playlist_free_cached();
      else if (playlist_config_copy(playlist_get_config(cached), &config))
      {
         playlist_free_cached();
         playlist_init_cached(&config);
      }
      refresh = true;
   }
#ifdef HAVE_MENU
   if (refresh)
      menu_state_get_ptr()->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
#endif
}

/* The menu lists the playlists in its sidebar. */
static void romm_reload_playlists(void)
{
#ifdef HAVE_MENU
   struct menu_state *menu_st = menu_state_get_ptr();

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
      int http_status, const romm_library_t *old, const romm_library_t *lib)
{
   bool quiet   = romm_syncing_quietly;
   size_t games = 0;
   size_t i;
   char msg[256];

   romm_syncing = false;
   romm_reload_playlists();

   if (status == ROMM_ERR_CANCELLED)
      return;
   if (status != ROMM_OK)
   {
      if (     !quiet
            || status == ROMM_ERR_UNAUTHORIZED
            || status == ROMM_ERR_FORBIDDEN || status == ROMM_ERR_WRITE)
         romm_notify_failure(status, http_status);
      return;
   }
   if (quiet)
      return;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      games += RBUF_LEN(lib->platforms[i].roms);
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SYNCED),
         (unsigned)games, (unsigned)RBUF_LEN(lib->platforms));
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
   romm_sync_owed_saves();
}

static bool romm_start_library_sync(bool quiet)
{
   settings_t *settings = config_get_ptr();
   romm_library_sync_params_t params;

   params.server         = settings->arrays.romm_server_url;
   params.token          = settings->arrays.romm_token;
   params.playlist_dir   = settings->paths.directory_playlist;
   params.thumbnails_dir = settings->paths.directory_thumbnails;
   params.title          = msg_hash_to_str(MSG_ROMM_SYNCING);
   params.covers_title   = msg_hash_to_str(MSG_ROMM_FETCHING_COVERS);
   params.find_core      = romm_find_core;
   params.playlist_changed = romm_playlist_changed;
   if (!romm_library_sync(&params, romm_on_library_synced, NULL))
      return false;
   romm_syncing         = true;
   romm_syncing_quietly = quiet;
   return true;
}

void romm_refresh_library_at_start(void)
{
   settings_t *settings = config_get_ptr();

   if (     !romm_syncing
         && !string_is_empty(settings->arrays.romm_server_url)
         && !string_is_empty(settings->arrays.romm_token))
      romm_start_library_sync(true);
}

void romm_refresh_library(void)
{
   settings_t *settings = config_get_ptr();

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

   if (!romm_start_library_sync(false))
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
}

/* The entry being fetched, loaded once it's in memory */
static bool romm_launching = false;
static char romm_launch_core[PATH_MAX_LENGTH];
static char romm_launch_content[PATH_MAX_LENGTH];
static char romm_launch_label[NAME_MAX_LENGTH];
/* What loads: the entry's path, or a disc unpacked from it */
static char romm_launch_path[PATH_MAX_LENGTH];

/* The game whose saves were synced for this launch */
typedef struct
{
   unsigned rom_id;
   char emulator[64];
   char base[NAME_MAX_LENGTH];
   char rom_base[NAME_MAX_LENGTH];
   char save_dir[PATH_MAX_LENGTH];
   char state_dir[PATH_MAX_LENGTH];
} romm_played_t;

/* The one launching, the one running, and the one a launch replaces,
 * whose saves go up when it has closed */
static romm_played_t romm_played;
static romm_played_t romm_running;
static romm_played_t romm_closing;

/* The BIOS files the core at @core_path wants that the system folder
 * lacks, as the core's info names them; with @required_only, those
 * it can't do without. None when cores look beside the content, as
 * that is in memory. */
static const char **romm_missing_firmware(const char *core_path,
      bool required_only)
{
   settings_t *settings   = config_get_ptr();
   const char *system_dir = settings->paths.directory_system;
   core_info_t *info      = NULL;
   const char **missing   = NULL;
   size_t i;

   if (     string_is_empty(system_dir)
         || settings->bools.systemfiles_in_content_dir
         || !core_info_find(core_path, &info))
      return NULL;
   for (i = 0; i < info->firmware_count; i++)
   {
      const core_info_firmware_t *fw = &info->firmware[i];
      char path[PATH_MAX_LENGTH];

      if (string_is_empty(fw->path) || (required_only && fw->optional))
         continue;
      fill_pathname_join_special(path, system_dir, fw->path, sizeof(path));
      if (!path_is_valid(path))
         RBUF_PUSH(missing, fw->path);
   }
   return missing;
}

static int romm_load_content(const char *core_path,
      const char *content_path, const char *label)
{
   content_ctx_info_t content_info;

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;
   return task_push_load_content_from_playlist_from_menu(core_path,
         content_path, *label ? label : NULL, &content_info, NULL, NULL)
      ? 0 : -1;
}

/* Where RetroArch puts the saves, or with @states the save states, of
 * content @path played with core @core_name, the way
 * runloop_path_set_redirect() works it out. False when they'd go
 * beside the content, which is in memory. */
static bool romm_saves_dir(char *s, size_t len, const char *path,
      const char *core_name, bool states)
{
   settings_t *settings = config_get_ptr();
   const char *dir      = dir_get_ptr(states
         ? RARCH_DIR_SAVESTATE : RARCH_DIR_SAVEFILE);
   char tmp[PATH_MAX_LENGTH];

   if (     string_is_empty(dir)
         || (states ? settings->bools.savestates_in_content_dir
                    : settings->bools.savefiles_in_content_dir))
      return false;
   strlcpy(s, dir, len);
   if (states ? settings->bools.sort_savestates_by_content_enable
              : settings->bools.sort_savefiles_by_content_enable)
   {
      char name[NAME_MAX_LENGTH];
      fill_pathname_parent_dir_name(name, path, sizeof(name));
      fill_pathname_join_special(tmp, s, name, sizeof(tmp));
      strlcpy(s, tmp, len);
   }
   if (     (states ? settings->bools.sort_savestates_enable
                    : settings->bools.sort_savefiles_enable)
         && !string_is_empty(core_name))
   {
      fill_pathname_join(tmp, s, core_name, sizeof(tmp));
      strlcpy(s, tmp, len);
   }
   return true;
}

/* @path's file name without its extension, as RetroArch names saves. */
static void romm_saves_base(char *s, size_t len, const char *path)
{
   char *dot;
   strlcpy(s, path_basename(path), len);
   if ((dot = strrchr(s, '.')) && dot != s)
      *dot = '\0';
}

/* How long a second Run has to choose RomM's save in a conflict */
#define ROMM_CONFLICT_ASK_USEC 10000000

/* A launch waiting on the choice of save in a conflict; a new
 * generation cancels the timer of the one before. */
static bool romm_conflict_pending = false;
static unsigned romm_conflict_gen = 0;
static char romm_conflict_content[PATH_MAX_LENGTH];

static void romm_load_played(void)
{
   /* The game running closes during the load, now or a frame later. */
   if (romm_running.rom_id)
      romm_closing = romm_running;
   if (!romm_load_content(romm_launch_core, romm_launch_path,
         romm_launch_label))
      romm_running = romm_played;
}

static bool romm_sync_played(const romm_played_t *game, bool push,
      enum romm_saves_resolve resolve, romm_saves_sync_cb_t cb,
      void *user_data);

static void romm_on_conflict_settled(void *user_data,
      enum romm_status status, int http_status,
      const romm_saves_result_t *result)
{
   bool remote    = user_data != NULL;
   romm_launching = false;
   if (status != ROMM_OK || result->failed || result->conflicts)
      romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_NOT_FETCHED),
            MESSAGE_QUEUE_CATEGORY_WARNING);
   else
      romm_notify(msg_hash_to_str(remote ? MSG_ROMM_SAVES_USED_REMOTE
            : MSG_ROMM_SAVES_USED_LOCAL), MESSAGE_QUEUE_CATEGORY_INFO);
   romm_load_played();
}

/* Settles the waiting launch's conflict as chosen, then loads it. */
static void romm_settle_conflict(enum romm_saves_resolve resolve)
{
   romm_conflict_pending = false;
   romm_conflict_gen++;
   romm_launching        = true;
   /* The callback tells which way by its user data. */
   if (!romm_sync_played(&romm_played, false, resolve,
         romm_on_conflict_settled,
         resolve == ROMM_SAVES_RESOLVE_REMOTE ? (void*)&romm_played : NULL))
   {
      romm_launching = false;
      romm_load_played();
   }
}

/* Runs on the main thread: no second Run in time keeps this device's
 * save. */
static void romm_conflict_timer_handler(retro_task_t *task)
{
   if (     (uintptr_t)task->user_data != romm_conflict_gen
         || !romm_conflict_pending)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }
   if (cpu_features_get_time_usec() < task->when)
      return;
   task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
   RARCH_LOG("[RomM] No second Run: using this device's save.\n");
   romm_settle_conflict(ROMM_SAVES_RESOLVE_LOCAL);
}

static void romm_on_saves_pulled(void *user_data, enum romm_status status,
      int http_status, const romm_saves_result_t *result)
{
   retro_task_t *task;

   romm_launching = false;
   if (status == ROMM_OK && !result->failed && result->conflicts)
   {
      romm_conflict_gen++;
      if ((task = task_init()))
      {
         task->handler   = romm_conflict_timer_handler;
         task->user_data = (void*)(uintptr_t)romm_conflict_gen;
         task->when      = cpu_features_get_time_usec()
            + ROMM_CONFLICT_ASK_USEC;
         task->flags    |= RETRO_TASK_FLG_MAIN_THREAD
                         | RETRO_TASK_FLG_MUTE;
         if (task_queue_push(task))
         {
            romm_conflict_pending = true;
            strlcpy(romm_conflict_content, romm_launch_content,
                  sizeof(romm_conflict_content));
            RARCH_LOG("[RomM] Save conflict in game %u: waiting for a "
                  "second Run.\n", romm_played.rom_id);
            romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_CONFLICT),
                  MESSAGE_QUEUE_CATEGORY_WARNING);
            return;
         }
         free(task);
      }
      romm_settle_conflict(ROMM_SAVES_RESOLVE_LOCAL);
      return;
   }
   if (status != ROMM_OK || result->failed)
      romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_NOT_FETCHED),
            MESSAGE_QUEUE_CATEGORY_WARNING);
   romm_load_played();
}

/* @push syncs @game after play rather than before. */
/* @push syncs @game after play; before, @resolve settles conflicts.
 * @cb gets @user_data. */
static bool romm_sync_played(const romm_played_t *game, bool push,
      enum romm_saves_resolve resolve, romm_saves_sync_cb_t cb,
      void *user_data)
{
   settings_t *settings = config_get_ptr();
   romm_saves_sync_params_t p;
   char record[PATH_MAX_LENGTH];

   romm_saves_record_path(record, sizeof(record),
         dir_get_ptr(RARCH_DIR_SAVEFILE));
   memset(&p, 0, sizeof(p));
   p.server      = settings->arrays.romm_server_url;
   p.token       = settings->arrays.romm_token;
   p.device_id   = settings->arrays.romm_device_id;
   p.rom_id      = game->rom_id;
   p.emulator    = game->emulator;
   p.base        = game->base;
   p.rom_base    = game->rom_base;
   p.save_dir    = game->save_dir;
   p.state_dir   = game->state_dir;
   p.record_path = record;
   p.push        = push;
   p.resolve     = resolve;
   p.is_live     = content_savefile_is_live;
   return romm_saves_sync(&p, cb, user_data);
}

static void romm_on_saves_pushed(void *user_data, enum romm_status status,
      int http_status, const romm_saves_result_t *result)
{
   if (status != ROMM_OK || result->failed)
      romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_NOT_SENT),
            MESSAGE_QUEUE_CATEGORY_WARNING);
   else if (result->conflicts)
      romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_CONFLICT_AFTER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
}

/* Is @game's save file one the content closing has? */
static bool romm_played_is_live(const romm_played_t *game)
{
   char name[NAME_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];

   if (!game->rom_id)
      return false;
   strlcpy(name, game->base, sizeof(name));
   strlcat(name, FILE_PATH_SRM_EXTENSION, sizeof(name));
   fill_pathname_join(path, game->save_dir, name, sizeof(path));
   return content_savefile_is_live(path);
}

/* Owed syncs still out, and how many games ended in a conflict */
static unsigned romm_owed_pending   = 0;
static unsigned romm_owed_conflicts = 0;

static void romm_on_owed_pushed(void *user_data, enum romm_status status,
      int http_status, const romm_saves_result_t *result)
{
   char msg[256];

   if (status == ROMM_OK && result->conflicts)
      romm_owed_conflicts++;
   if (--romm_owed_pending || !romm_owed_conflicts)
      return;
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SAVES_OWED_CONFLICTS),
         romm_owed_conflicts);
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_WARNING);
}

void romm_sync_owed_saves(void)
{
   settings_t *settings = config_get_ptr();
   romm_saves_record_t rec;
   char record[PATH_MAX_LENGTH];
   size_t i;

   if (     romm_owed_pending
         || string_is_empty(settings->arrays.romm_token)
         || string_is_empty(dir_get_ptr(RARCH_DIR_SAVEFILE)))
      return;
   romm_saves_record_path(record, sizeof(record),
         dir_get_ptr(RARCH_DIR_SAVEFILE));
   if (!romm_saves_record_load(&rec, record))
      return;
   romm_owed_conflicts = 0;
   for (i = 0; i < RBUF_LEN(rec.games); i++)
   {
      const romm_saves_game_t *g = &rec.games[i];
      romm_played_t game;

      if (!g->pending || !*g->save_dir || !*g->state_dir || !*g->base)
         continue;
      /* Still being played: it goes up when it closes. */
      if (     (romm_running.rom_id == g->rom_id
               && string_is_equal(romm_running.emulator, g->emulator))
            || (romm_played.rom_id == g->rom_id && romm_launching))
         continue;
      memset(&game, 0, sizeof(game));
      game.rom_id = g->rom_id;
      strlcpy(game.emulator, g->emulator, sizeof(game.emulator));
      strlcpy(game.base, g->base, sizeof(game.base));
      strlcpy(game.save_dir, g->save_dir, sizeof(game.save_dir));
      strlcpy(game.state_dir, g->state_dir, sizeof(game.state_dir));
      RARCH_LOG("[RomM] Sending saves owed for game %u (%s).\n",
            game.rom_id, game.emulator);
      if (romm_sync_played(&game, true, ROMM_SAVES_RESOLVE_NONE,
            romm_on_owed_pushed, NULL))
         romm_owed_pending++;
   }
   romm_saves_record_free(&rec);
}

void romm_content_closed(void)
{
   romm_played_t *game = romm_played_is_live(&romm_closing)
      ? &romm_closing
      : romm_played_is_live(&romm_running) ? &romm_running : NULL;

   if (!game)
      return;
   if (!romm_sync_played(game, true, ROMM_SAVES_RESOLVE_NONE,
         romm_on_saves_pushed, NULL))
      romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_NOT_SENT),
            MESSAGE_QUEUE_CATEGORY_WARNING);
   memset(game, 0, sizeof(*game));
}

/* Fetches what RomM has newer of the saves of @launch_path, the game
 * of playlist entry @content_path, played with @core_path, then loads
 * it. */
static int romm_load_with_saves(const char *core_path,
      const char *content_path, const char *launch_path, const char *label)
{
   core_info_t *info    = NULL;

   memset(&romm_played, 0, sizeof(romm_played));
   if (     !(romm_played.rom_id = romm_launch_rom_id(content_path))
         || !core_info_find(core_path, &info)
         || string_is_empty(info->core_name)
         || !romm_saves_dir(romm_played.save_dir,
               sizeof(romm_played.save_dir), launch_path, info->core_name,
               false)
         || !romm_saves_dir(romm_played.state_dir,
               sizeof(romm_played.state_dir), launch_path, info->core_name,
               true))
   {
      RARCH_LOG("[RomM] Saves of %s aren't synced: no core name or save "
            "folder.\n", launch_path);
      memset(&romm_played, 0, sizeof(romm_played));
      return romm_load_content(core_path, launch_path, label);
   }
   strlcpy(romm_played.emulator, info->core_name,
         sizeof(romm_played.emulator));
   romm_saves_base(romm_played.base, sizeof(romm_played.base),
         launch_path);
   romm_saves_base(romm_played.rom_base, sizeof(romm_played.rom_base),
         content_path);
   RARCH_LOG("[RomM] Saves of game %u go to %s/%s.*, states to %s.\n",
         romm_played.rom_id, romm_played.save_dir, romm_played.base,
         romm_played.state_dir);

   if (core_path != romm_launch_core)
      strlcpy(romm_launch_core, core_path, sizeof(romm_launch_core));
   strlcpy(romm_launch_path, launch_path, sizeof(romm_launch_path));
   if (label != romm_launch_label)
      strlcpy(romm_launch_label, label, sizeof(romm_launch_label));

   romm_launching = true;
   if (!romm_sync_played(&romm_played, false, ROMM_SAVES_RESOLVE_NONE,
         romm_on_saves_pulled, NULL))
   {
      romm_launching = false;
      return romm_load_content(core_path, launch_path, label);
   }
   return 0;
}

static void romm_on_launch_prepared(void *user_data,
      enum romm_status status, int http_status, const char *launch_path)
{
   enum msg_hash_enums msg = MSG_UNKNOWN;

   romm_launching = false;
   switch (status)
   {
      case ROMM_OK:
         {
            const char **missing = romm_missing_firmware(romm_launch_core,
                  true);
            if (RBUF_LEN(missing))
            {
               char text[NAME_MAX_LENGTH];
               snprintf(text, sizeof(text),
                     msg_hash_to_str(MSG_ROMM_FIRMWARE_MISSING), missing[0]);
               romm_notify(text, MESSAGE_QUEUE_CATEGORY_WARNING);
            }
            RBUF_FREE(missing);
         }
         romm_load_with_saves(romm_launch_core, romm_launch_content,
               launch_path, romm_launch_label);
         return;
      case ROMM_ERR_CANCELLED:
         return;
      case ROMM_ERR_HTTP:
         if (http_status == 404)
            msg = MSG_ROMM_GAME_GONE;
         break;
      case ROMM_ERR_UNSUPPORTED:
         msg = string_is_equal_noncase(
               path_get_extension(romm_launch_content), "rar")
            ? MSG_ROMM_RAR_UNSUPPORTED : MSG_ROMM_CANT_LAUNCH;
         break;
      case ROMM_ERR_CHECKSUM:
         msg = MSG_ROMM_DOWNLOAD_BAD;
         break;
      case ROMM_ERR_MEMORY:
         msg = MSG_ROMM_NO_MEMORY;
         break;
      default:
         break;
   }
   if (msg != MSG_UNKNOWN)
      romm_notify(msg_hash_to_str(msg), MESSAGE_QUEUE_CATEGORY_ERROR);
   else
      romm_notify_failure(status, http_status);
}

int romm_launch_from_menu(const char *core_path, const char *content_path,
      const char *label)
{
   settings_t *settings = config_get_ptr();
   romm_launch_params_t params;
   const char **firmware;
   const char *name;
   bool started;
   char title[NAME_MAX_LENGTH];
   char unpack_title[NAME_MAX_LENGTH];

   /* Run again on a game waiting on a save conflict: RomM's save.
    * Any other launch drops the waiting one. */
   if (romm_conflict_pending)
   {
      bool same = string_is_equal(romm_conflict_content, content_path);
      romm_conflict_pending = false;
      romm_conflict_gen++;
      if (same)
      {
         RARCH_LOG("[RomM] Second Run: using RomM's save.\n");
         romm_settle_conflict(ROMM_SAVES_RESOLVE_REMOTE);
         return 0;
      }
   }
   if (romm_launching)
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_LAUNCH_IN_PROGRESS),
            MESSAGE_QUEUE_CATEGORY_INFO);
      return 0;
   }
   if (string_is_empty(settings->arrays.romm_token))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NOT_PAIRED),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return -1;
   }
   if (romm_launch_is_ready(content_path))
   {
      strlcpy(romm_launch_content, content_path,
            sizeof(romm_launch_content));
      return romm_load_with_saves(core_path, content_path, content_path,
            label ? label : "");
   }

   strlcpy(romm_launch_core, core_path, sizeof(romm_launch_core));
   strlcpy(romm_launch_content, content_path, sizeof(romm_launch_content));
   strlcpy(romm_launch_label, label ? label : "", sizeof(romm_launch_label));
   name = *romm_launch_label ? romm_launch_label
      : path_basename(content_path);
   snprintf(title, sizeof(title), msg_hash_to_str(MSG_ROMM_DOWNLOADING),
         name);
   snprintf(unpack_title, sizeof(unpack_title),
         msg_hash_to_str(MSG_ROMM_UNPACKING), name);
   firmware = romm_missing_firmware(core_path, false);
   memset(&params, 0, sizeof(params));
   params.server         = settings->arrays.romm_server_url;
   params.token          = settings->arrays.romm_token;
   params.content_path   = content_path;
   params.title          = title;
   params.unpack_title   = unpack_title;
   params.system_dir     = settings->paths.directory_system;
   params.firmware       = firmware;
   params.firmware_count = RBUF_LEN(firmware);
   started = romm_launch_prepare(&params, romm_on_launch_prepared, NULL);
   RBUF_FREE(firmware);
   if (!started)
   {
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return -1;
   }
   romm_launching = true;
   return 0;
}

#if defined(HAVE_MENU) && defined(HAVE_UPDATE_CORES)
/* How soon a second Run must follow to confirm a core download */
#define ROMM_CORE_CONFIRM_USEC 10000000

/* A core download, and the game to launch once it's in */
typedef struct
{
   bool listing;
   bool downloading;
   bool installed;
   char core[64];
   char name[NAME_MAX_LENGTH];
   char filename[NAME_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char content[PATH_MAX_LENGTH];
   char label[NAME_MAX_LENGTH];
} romm_core_install_t;

static bool romm_installing_core = false;
static char romm_offered_content[PATH_MAX_LENGTH];
static retro_time_t romm_offered_at;

/* The core's own name in its info file, such as "Genesis Plus GX";
 * the info files cover cores not yet installed. */
static void romm_core_name(const char *core, char *s, size_t len)
{
   settings_t *settings = config_get_ptr();
   config_file_t *conf;
   char path[PATH_MAX_LENGTH];

   strlcpy(s, core, len);
   fill_pathname_join_special(path, settings->paths.path_libretro_info,
         core, sizeof(path));
   strlcat(path, "_libretro.info", sizeof(path));
   if ((conf = config_file_new(path)))
   {
      config_get_array(conf, "corename", s, len);
      config_file_free(conf);
   }
}

/* Runs on the main thread, polling the core downloader's tasks: the
 * core list first, if it hasn't been fetched, then the core. */
static void romm_core_install_handler(retro_task_t *task)
{
   romm_core_install_t *ins  = (romm_core_install_t*)task->state;
   core_updater_list_t *list = core_updater_list_get_cached();
   settings_t *settings      = config_get_ptr();
   size_t i, len;

   task->when = cpu_features_get_time_usec() + 250000;
   if (ins->downloading)
   {
      if (task_core_updater_download_pending(ins->filename))
         return;
      ins->installed = path_is_valid(ins->core_path);
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   if (!list)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }
   if (task_core_updater_list_pending(list))
      return;
   if (!core_updater_list_size(list))
   {
      if (     ins->listing
            || !task_push_get_core_updater_list(list, false, false))
         task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      ins->listing = true;
      return;
   }

   len = strlen(ins->core);
   for (i = 0; i < core_updater_list_size(list); i++)
   {
      const core_updater_list_entry_t *entry = NULL;
      if (     core_updater_list_get_index(list, i, &entry)
            && !strncmp(entry->remote_filename, ins->core, len)
            && string_starts_with(entry->remote_filename + len, "_libretro."))
      {
         strlcpy(ins->filename, entry->remote_filename,
               sizeof(ins->filename));
         strlcpy(ins->core_path, entry->local_core_path,
               sizeof(ins->core_path));
         break;
      }
   }
   if (     !*ins->filename
         || !task_push_core_updater_download(list, ins->filename, 0, false,
               settings->bools.core_updater_auto_backup,
               (size_t)settings->uints.core_updater_auto_backup_history_size,
               settings->paths.directory_libretro,
               settings->paths.directory_core_assets))
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }
   ins->downloading = true;
}

static void romm_core_install_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_core_install_t *ins = (romm_core_install_t*)task->state;
   core_info_t *info        = NULL;
   char msg[NAME_MAX_LENGTH];

   romm_installing_core = false;
   if (!ins->installed)
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_ROMM_CORE_INSTALL_FAILED), ins->name);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
      return;
   }
   /* The downloader rescans the info files once its task is retired,
    * which can be after this. */
   if (!core_info_find(ins->core_path, &info))
   {
      bool refresh = true;
      command_event(CMD_EVENT_CORE_INFO_INIT, &refresh);
   }
   romm_launch_from_menu(ins->core_path, ins->content,
         *ins->label ? ins->label : NULL);
}

static void romm_core_install_cleanup(retro_task_t *task)
{
   free(task->state);
   task->state = NULL;
}

static bool romm_install_core(const char *core, const char *name,
      const char *content_path, const char *label)
{
   romm_core_install_t *ins;
   retro_task_t *task;

   if (!core_updater_list_get_cached())
      core_updater_list_init_cached();
   if (!(ins = (romm_core_install_t*)calloc(1, sizeof(*ins))))
      return false;
   if (!(task = task_init()))
   {
      free(ins);
      return false;
   }
   strlcpy(ins->core, core, sizeof(ins->core));
   strlcpy(ins->name, name, sizeof(ins->name));
   strlcpy(ins->content, content_path, sizeof(ins->content));
   strlcpy(ins->label, label ? label : "", sizeof(ins->label));
   task->handler  = romm_core_install_handler;
   task->callback = romm_core_install_cb;
   task->cleanup  = romm_core_install_cleanup;
   task->state    = ins;
   task->flags   |= RETRO_TASK_FLG_MAIN_THREAD | RETRO_TASK_FLG_MUTE;
   if (!task_queue_push(task))
   {
      free(ins);
      free(task);
      return false;
   }
   romm_installing_core = true;
   RARCH_LOG("[RomM] Installing %s for %s.\n", core, content_path);
   return true;
}
#endif

bool romm_launch_without_core(const char *content_path, const char *label,
      const char *playlist_name)
{
   const char *const *cores;
   char slug[NAME_MAX_LENGTH];
   char path[PATH_MAX_LENGTH];
   char name[NAME_MAX_LENGTH];

   if (     !string_starts_with(content_path, ROMM_SCHEME)
         || string_is_empty(playlist_name))
      return false;
   fill_pathname(slug, path_basename(playlist_name), "", sizeof(slug));
   if (!(cores = romm_library_preferred_cores(slug)))
      return false;
   if (romm_find_core(cores, path, sizeof(path), name, sizeof(name)))
   {
      romm_launch_from_menu(path, content_path, label);
      return true;
   }

#if defined(HAVE_MENU) && defined(HAVE_UPDATE_CORES)
   {
      settings_t *settings = config_get_ptr();
      retro_time_t now     = cpu_features_get_time_usec();
      char msg[NAME_MAX_LENGTH];

      if (     !settings->bools.menu_show_core_updater
            || settings->bools.kiosk_mode_enable)
         return false;
      romm_core_name(cores[0], name, sizeof(name));
      if (romm_installing_core)
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_CORE_INSTALLING), name);
      /* Asked already: this Run is the go-ahead. */
      else if (   string_is_equal(romm_offered_content, content_path)
               && now - romm_offered_at < ROMM_CORE_CONFIRM_USEC)
      {
         romm_offered_content[0] = '\0';
         if (romm_install_core(cores[0], name, content_path, label))
            return true;
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_CORE_INSTALL_FAILED), name);
      }
      else
      {
         strlcpy(romm_offered_content, content_path,
               sizeof(romm_offered_content));
         romm_offered_at = now;
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_CORE_MISSING), name);
         RARCH_LOG("[RomM] %s isn't installed; offered to download it.\n",
               cores[0]);
      }
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_INFO);
      return true;
   }
#else
   return false;
#endif
}

void romm_content_unloaded(const char *next_content)
{
   /* A fetch freed the rest when it began; what it holds is its own. */
   if (!romm_launching)
      romm_launch_release(next_content);
}
