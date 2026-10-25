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
#include <time.h>

#include <array/rbuf.h>
#include <compat/posix_string.h>
#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/config_file.h>
#include <file/file_path.h>
#include <libretro.h>
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <queues/task_queue.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "romm.h"
#include "romm_client.h"
#include "romm_launch.h"
#include "romm_library.h"
#include "romm_platform.h"
#include "romm_saves.h"
#include "romm_sidebar.h"
#include "../../command.h"
#include "../../configuration.h"
#include "../../core_info.h"
#include "../../defaults.h"
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
/* Unpair This Device is waiting on RomM; no sync starts meanwhile. */
static bool romm_unpairing = false;
/* Check Connection is waiting on RomM. */
static bool romm_checking = false;
/* The running sync was started at startup rather than by the user. */
static bool romm_syncing_quietly = false;

/* A sync of one system, from its page. */
typedef struct
{
   char fs_slug[64];
   enum romm_system_action action;
} romm_system_job_t;

/* The one the running sync is for (fs_slug empty for none), and the
 * ones waiting for it (an RBUF, oldest first). */
static romm_system_job_t romm_system_job;
static romm_system_job_t *romm_system_jobs = NULL;

static bool romm_can_sync(void);
static void romm_start_system_job(const romm_system_job_t *job);
static void romm_start_next_system_job(void);
#if defined(HAVE_MENU) && defined(HAVE_UPDATE_CORES)
static void romm_want_missing_cores(const romm_library_t *lib,
      const char *only, bool all);
#endif
/* RomM was asked this run which user the token acts for. */
static bool romm_username_asked = false;

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

static bool romm_overrides_active(void);

static void romm_note_username(const char *username)
{
   settings_t *settings = config_get_ptr();
   if (*username)
      strlcpy(settings->arrays.romm_username, username,
            sizeof(settings->arrays.romm_username));
}

static void romm_on_username(void *user_data, enum romm_status status,
      int http_status, const char *username)
{
   if (status == ROMM_OK)
      romm_note_username(username);
}

/* Asks RomM, once a run, which user the token acts for, when that
 * isn't known. */
static void romm_ask_username(void)
{
   settings_t *settings = config_get_ptr();
   if (     romm_username_asked || *settings->arrays.romm_username
         || string_is_empty(settings->arrays.romm_token))
      return;
   romm_username_asked = romm_client_whoami(
         settings->arrays.romm_server_url, settings->arrays.romm_token,
         romm_on_username, NULL);
}

/* What Check Connection has found so far */
typedef struct
{
   char version[64];
   char username[256];
} romm_check_t;

static void romm_check_scopes_cb(void *user_data, enum romm_status status,
      int http_status, bool found, const char *missing_scopes)
{
   settings_t *settings = config_get_ptr();
   romm_check_t *check  = (romm_check_t*)user_data;
   char msg[1024];

   romm_checking = false;
   /* RomM's list shows the token as it is now, which is what decides
    * whether saves sync. */
   if (     status == ROMM_OK && found
         && !string_is_equal(missing_scopes,
               settings->arrays.romm_missing_scopes))
   {
      RARCH_LOG("[RomM] Noted that RomM gave this device %s%s.\n",
            *missing_scopes ? "all but " : "every permission asked for",
            missing_scopes);
      strlcpy(settings->arrays.romm_missing_scopes, missing_scopes,
            sizeof(settings->arrays.romm_missing_scopes));
      if (!romm_overrides_active())
         command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   }
   if (status == ROMM_OK && found && *missing_scopes)
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_ROMM_CONNECTED_MISSING_SCOPES),
            check->version, check->username, missing_scopes);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_WARNING);
   }
   else
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CONNECTED_AS),
            check->version, check->username);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
   }
   free(check);
}

static void romm_check_token_cb(void *user_data,
      enum romm_status status, int http_status, const char *username)
{
   settings_t *settings = config_get_ptr();
   romm_check_t *check  = (romm_check_t*)user_data;
   char msg[512];

   if (status == ROMM_OK)
   {
      romm_note_username(username);
      /* And whether RomM gave this device every permission. */
      strlcpy(check->username, username, sizeof(check->username));
      if (romm_client_token_scopes(settings->arrays.romm_server_url,
            settings->arrays.romm_token, settings->arrays.romm_device_id,
            romm_check_scopes_cb, check))
         return;
   }
   romm_checking = false;
   if (status == ROMM_OK)
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CONNECTED_AS),
            check->version, username);
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
   free(check);
}

static void romm_check_connection_cb(void *user_data,
      enum romm_status status, int http_status, const char *version)
{
   settings_t *settings = config_get_ptr();
   romm_check_t *check;
   char msg[512];

   if (status != ROMM_OK)
   {
      romm_checking = false;
      romm_notify_failure(status, http_status);
      return;
   }

   /* Paired: the token has to work too. */
   if (     !string_is_empty(settings->arrays.romm_token)
         && (check = (romm_check_t*)calloc(1, sizeof(*check))))
   {
      strlcpy(check->version, version, sizeof(check->version));
      if (romm_client_whoami(settings->arrays.romm_server_url,
            settings->arrays.romm_token, romm_check_token_cb, check))
         return;
      free(check);
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

/* Save sync reads and writes RomM's saves as this device: without any
 * of these it would only fail. */
static const char *const romm_saves_scopes[] = {
   "assets.read", "assets.write", "devices.read", "devices.write" };

/* Whether @missing, as pairing names missing scopes, holds one that
 * save sync needs. */
static bool romm_saves_scope_missing(const char *missing)
{
   size_t i;
   for (i = 0; i < ARRAY_SIZE(romm_saves_scopes); i++)
      if (romm_scope_listed(missing, romm_saves_scopes[i]))
         return true;
   return false;
}

/* Whether this device's pairing lets saves sync: a device paired
 * before saves synced has no device id. */
static bool romm_saves_allowed(void)
{
   settings_t *settings = config_get_ptr();
   return !string_is_empty(settings->arrays.romm_device_id)
      && !romm_saves_scope_missing(settings->arrays.romm_missing_scopes);
}

#ifdef HAVE_CLOUDSYNC
#define ROMM_CLOUD_SYNC_PATH "/api/sync/retroarch/"

enum romm_cloud_sync_setup
{
   ROMM_CLOUD_SYNC_SET_UP = 0,
   ROMM_CLOUD_SYNC_ELSEWHERE,
   ROMM_CLOUD_SYNC_FAILED
};

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
   strlcpy(settings->arrays.romm_missing_scopes, missing_scopes,
         sizeof(settings->arrays.romm_missing_scopes));
   /* The new token may act for another user. */
   *settings->arrays.romm_username = '\0';
   romm_username_asked             = false;
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
   romm_ask_username();
#ifdef HAVE_CLOUDSYNC
   if (     cloud_sync_now
         && settings->uints.cloud_sync_sync_mode == CLOUD_SYNC_MODE_AUTOMATIC)
      task_push_cloud_sync();
#endif
   if (*missing_scopes)
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(romm_saves_scope_missing(missing_scopes)
               ? MSG_ROMM_PAIRED_NO_SAVES
               : MSG_ROMM_PAIRED_MISSING_SCOPES), missing_scopes);
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

/* History and Favorites list RomM games the sync doesn't write: drops
 * those gone from RomM, deleted or hidden from the user, and follows
 * renames. */
static void romm_follow_games(playlist_t *playlist,
      const romm_rom_t *const *old, const romm_rom_t *const *lib)
{
   char path[PATH_MAX_LENGTH];
   char label[NAME_MAX_LENGTH];
   bool changed = false;
   size_t i     = playlist_size(playlist);

   while (i--)
   {
      const struct playlist_entry *entry = NULL;
      const char *was_label;

      playlist_get_index(playlist, i, &entry);
      if (     !entry || !entry->path
            || !string_starts_with(entry->path, ROMM_SCHEME))
         continue;
      was_label = entry->label ? entry->label : "";
      if (!romm_library_follow_game(old, lib, entry->path, was_label,
            path, sizeof(path), label, sizeof(label)))
      {
         RARCH_LOG("[RomM] %s is gone from RomM; removed from %s.\n",
               entry->path, path_basename(playlist_get_conf_path(playlist)));
         playlist_delete_index(playlist, i);
         changed = true;
      }
      else if (   !string_is_equal(path, entry->path)
               || !string_is_equal(label, was_label))
      {
         struct playlist_entry update = {0};
         RARCH_LOG("[RomM] %s is now %s in %s.\n", entry->path, path,
               path_basename(playlist_get_conf_path(playlist)));
         update.path  = path;
         update.label = label;
         playlist_update(playlist, i, &update);
         changed = true;
      }
   }
   if (changed)
   {
      playlist_write_file(playlist);
      romm_playlist_changed(playlist_get_conf_path(playlist));
   }
}

static int romm_cmp_rom_title_desc(const void *a, const void *b)
{
   const romm_rom_t *x = *(const romm_rom_t *const*)a;
   const romm_rom_t *y = *(const romm_rom_t *const*)b;
   return strcasecmp(y->name, x->name);
}

/* Favorites follow RomM's favorites collection: RomM games it doesn't
 * hold leave, and those it holds join, at the top in title order, as a
 * favorite added here would. Other content stays as it is. */
static void romm_follow_favorites(playlist_t *favorites,
      const romm_library_t *lib, const romm_rom_t *const *index)
{
   const romm_rom_t **add = NULL;
   unsigned *have         = NULL;
   bool changed           = false;
   size_t i               = playlist_size(favorites);
   size_t j;

   while (i--)
   {
      const struct playlist_entry *entry = NULL;
      unsigned id;

      playlist_get_index(favorites, i, &entry);
      if (!entry || !(id = romm_library_path_id(entry->path)))
         continue;
      for (j = 0; j < RBUF_LEN(lib->favorite_ids); j++)
         if (lib->favorite_ids[j] == id)
            break;
      if (j < RBUF_LEN(lib->favorite_ids))
      {
         RBUF_PUSH(have, id);
         continue;
      }
      RARCH_LOG("[RomM] %s is no longer a favorite in RomM.\n", entry->path);
      playlist_delete_index(favorites, i);
      changed = true;
   }

   for (i = 0; i < RBUF_LEN(lib->favorite_ids); i++)
   {
      const romm_rom_t *rom = romm_library_find(index, lib->favorite_ids[i]);
      for (j = 0; j < RBUF_LEN(have); j++)
         if (have[j] == lib->favorite_ids[i])
            break;
      if (rom && j == RBUF_LEN(have))
         RBUF_PUSH(add, rom);
   }
   if (RBUF_LEN(add))
      qsort(add, RBUF_LEN(add), sizeof(*add), romm_cmp_rom_title_desc);
   for (i = 0; i < RBUF_LEN(add); i++)
   {
      struct playlist_entry entry = {0};
      char path[PATH_MAX_LENGTH];

      if (!romm_library_content_path(path, sizeof(path), add[i]))
         continue;
      entry.path      = path;
      entry.label     = (char*)add[i]->name;
      entry.core_path = (char*)FILE_PATH_DETECT;
      entry.core_name = (char*)FILE_PATH_DETECT;
      entry.db_name   = (char*)romm_library_rom_playlist(lib, add[i]);
      if (playlist_push(favorites, &entry))
      {
         RARCH_LOG("[RomM] %s is a favorite in RomM.\n", path);
         changed = true;
      }
   }
   RBUF_FREE(add);
   RBUF_FREE(have);

   if (changed)
   {
      /* Sorted as CMD_EVENT_ADD_TO_FAVORITES sorts. */
      enum playlist_sort_mode mode = playlist_get_sort_mode(favorites);
      if (     (config_get_ptr()->bools.playlist_sort_alphabetical
               && mode == PLAYLIST_SORT_MODE_DEFAULT)
            || mode == PLAYLIST_SORT_MODE_ALPHABETICAL)
         playlist_qsort(favorites);
      playlist_write_file(favorites);
      romm_playlist_changed(playlist_get_conf_path(favorites));
   }
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

/* From the library manifest, read once and then kept by each sync: the
 * user's favorites collection, and the sidebar's playlists. */
static unsigned romm_favorites_collection        = 0;
#ifdef HAVE_MENU
static romm_sidebar_system_t *romm_sidebar_cache = NULL;
#endif
static bool romm_manifest_read                   = false;
/* Favorites changed here, counted, and that count when the running
 * library sync started: a sync that overlaps a change leaves Favorites
 * to the next. */
static unsigned romm_favorites_changes         = 0;
static unsigned romm_favorites_changes_at_sync = 0;
/* Favorites to send once the favorites collection is made. */
static unsigned *romm_favorites_waiting        = NULL;
static bool romm_favorites_creating            = false;

/* The library as the manifest, then each sync, has it: when it was
 * synced, its games and systems, and the systems that sync left for a
 * manual one, as "NES, PlayStation". */
static int64_t romm_library_synced_at = 0;
static unsigned romm_library_games    = 0;
static unsigned romm_library_systems  = 0;
static char romm_library_waiting[128];
/* The systems new in RomM, left off, that the Systems page hasn't
 * shown, as "Saturn, 3DO". */
static char romm_library_new[128];
/* What the last sync left to fetch, per system (an RBUF). */
static romm_library_waiting_t *romm_library_waiting_list = NULL;

/* @only: the systems the sync was for, whose waiting it replaces; NULL
 * for every system. */
static void romm_note_library(const romm_library_t *lib, const char *only)
{
   romm_library_waiting_t *waiting = NULL;
   size_t _len = 0;
   size_t i;

   romm_library_synced_at = lib->synced_at;
   romm_library_systems   = (unsigned)RBUF_LEN(lib->platforms);
   romm_library_games     = 0;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      romm_library_games += (unsigned)RBUF_LEN(lib->platforms[i].roms);
   for (i = 0; only && i < RBUF_LEN(romm_library_waiting_list); i++)
      if (!romm_library_listed(only,
            romm_library_waiting_list[i].platform.fs_slug))
         RBUF_PUSH(waiting, romm_library_waiting_list[i]);
   for (i = 0; i < RBUF_LEN(lib->waiting); i++)
      RBUF_PUSH(waiting, lib->waiting[i]);
   RBUF_FREE(romm_library_waiting_list);
   romm_library_waiting_list = waiting;
   *romm_library_waiting = '\0';
   for (i = 0; i < RBUF_LEN(waiting)
         && _len < sizeof(romm_library_waiting); i++)
      _len += snprintf(romm_library_waiting + _len,
            sizeof(romm_library_waiting) - _len, "%s%s",
            i ? ", " : "", waiting[i].platform.name);
   *romm_library_new = '\0';
   for (i = 0; i < RBUF_LEN(lib->unsynced); i++)
      if (romm_library_listed(config_get_ptr()->arrays.romm_sync_new,
            lib->unsynced[i].fs_slug))
      {
         if (*romm_library_new)
            strlcat(romm_library_new, ", ", sizeof(romm_library_new));
         strlcat(romm_library_new, lib->unsynced[i].name,
               sizeof(romm_library_new));
      }
}

/* Systems new in RomM start off: switched off, marked new for the
 * Systems page, and named in a message. */
static void romm_note_added(const romm_library_t *lib)
{
   settings_t *settings = config_get_ptr();
   size_t count         = RBUF_LEN(lib->added);
   size_t i;
   char names[256];
   char msg[384];

   *names = '\0';
   for (i = 0; i < count; i++)
   {
      const romm_platform_t *p = &lib->added[i];
      char one[192];
      if (!romm_library_list_add(settings->arrays.romm_sync_exclude,
               sizeof(settings->arrays.romm_sync_exclude), p->fs_slug)
            || !romm_library_list_add(settings->arrays.romm_sync_new,
               sizeof(settings->arrays.romm_sync_new), p->fs_slug))
         RARCH_WARN("[RomM] No room to switch %s off; it syncs next "
               "time.\n", p->name);
      snprintf(one, sizeof(one), msg_hash_to_str(MSG_ROMM_SYNC_NEW_SYSTEM),
            p->name, p->rom_count);
      if (i)
         strlcat(names, ", ", sizeof(names));
      strlcat(names, one, sizeof(names));
   }
   if (!romm_overrides_active())
      command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   snprintf(msg, sizeof(msg), msg_hash_to_str(count == 1
         ? MSG_ROMM_SYNC_NEW_ONE : MSG_ROMM_SYNC_NEW), names);
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_INFO);
}

static void romm_manifest_load(void)
{
   settings_t *settings = config_get_ptr();
   romm_library_t lib;
   char path[PATH_MAX_LENGTH];

   if (romm_manifest_read)
      return;
   romm_manifest_read = true;
   romm_library_manifest_path(path, sizeof(path),
         settings->paths.directory_playlist);
   if (romm_library_load(&lib, path))
   {
      romm_note_library(&lib, NULL);
      romm_favorites_collection = lib.favorites_id;
#ifdef HAVE_MENU
      romm_sidebar_systems(&romm_sidebar_cache, &lib);
#endif
   }
   romm_library_free(&lib);
}

static void romm_on_favorites_sent(void *user_data,
      enum romm_status status, int http_status)
{
   if (status == ROMM_ERR_UNAUTHORIZED || status == ROMM_ERR_FORBIDDEN)
      romm_notify_failure(status, http_status);
   else if (status != ROMM_OK)
      romm_notify(msg_hash_to_str(MSG_ROMM_FAVORITES_NOT_SENT),
            MESSAGE_QUEUE_CATEGORY_WARNING);
}

static void romm_send_favorites(const unsigned *ids, size_t count,
      bool remove)
{
   settings_t *settings = config_get_ptr();
   if (!romm_client_collection_roms(settings->arrays.romm_server_url,
         settings->arrays.romm_token, romm_favorites_collection, ids, count,
         remove, romm_on_favorites_sent, NULL))
      romm_on_favorites_sent(NULL, ROMM_ERR_UNREACHABLE, 0);
}

static void romm_on_favorites_created(void *user_data,
      enum romm_status status, int http_status, unsigned id)
{
   romm_favorites_creating = false;
   if (status == ROMM_OK)
   {
      romm_favorites_collection = id;
      if (RBUF_LEN(romm_favorites_waiting))
         romm_send_favorites(romm_favorites_waiting,
               RBUF_LEN(romm_favorites_waiting), false);
   }
   else
      romm_on_favorites_sent(NULL, status, http_status);
   RBUF_FREE(romm_favorites_waiting);
}

void romm_favorite_changed(const char *content_path, bool added)
{
   settings_t *settings = config_get_ptr();
   unsigned id          = romm_library_path_id(content_path);

   if (!id || !*settings->arrays.romm_token)
      return;
   romm_favorites_changes++;
   romm_manifest_load();
   if (romm_favorites_collection)
      romm_send_favorites(&id, 1, !added);
   /* RomM makes the collection with the first favorite. */
   else if (added)
   {
      RBUF_PUSH(romm_favorites_waiting, id);
      if (romm_favorites_creating)
         return;
      if (romm_client_create_favorites(settings->arrays.romm_server_url,
            settings->arrays.romm_token, romm_on_favorites_created, NULL))
         romm_favorites_creating = true;
      else
      {
         RBUF_FREE(romm_favorites_waiting);
         romm_on_favorites_sent(NULL, ROMM_ERR_UNREACHABLE, 0);
      }
   }
}

#ifdef HAVE_MENU
void romm_menu_sidebar_group(file_list_t *list)
{
   romm_sidebar_names_t names;

   romm_manifest_load();
   names.collections = msg_hash_to_str(MSG_ROMM_SIDEBAR_COLLECTIONS);
   names.systems    = msg_hash_to_str(MSG_ROMM_SIDEBAR_SYSTEMS);
   names.other      = msg_hash_to_str(MSG_ROMM_SIDEBAR_OTHER);
   names.group_type = MENU_ROMM_GROUP;
   romm_sidebar_group(list, romm_sidebar_cache,
         config_get_ptr()->arrays.romm_sidebar_open, &names);
}

bool romm_menu_sidebar_toggle(const struct item_file *row)
{
   settings_t *settings = config_get_ptr();
   char open[sizeof(settings->arrays.romm_sidebar_open)];

   if (!row || row->type != MENU_ROMM_GROUP || !row->label)
      return false;
   strlcpy(open, settings->arrays.romm_sidebar_open, sizeof(open));
   romm_sidebar_toggle(open, sizeof(open), row->label);
   configuration_set_string(settings, settings->arrays.romm_sidebar_open,
         open);
   return true;
}

bool romm_menu_owns_playlist(const char *playlist_file)
{
   size_t i;

   romm_manifest_load();
   for (i = 0; i < RBUF_LEN(romm_sidebar_cache); i++)
      if (string_is_equal(romm_sidebar_cache[i].playlist, playlist_file))
         return true;
   return false;
}

bool romm_menu_sidebar_is_open(const struct item_file *row)
{
   return row && row->type == MENU_ROMM_GROUP && row->label
      && romm_sidebar_is_open(config_get_ptr()->arrays.romm_sidebar_open,
            row->label);
}

unsigned romm_menu_sidebar_depth(const file_list_t *list, size_t i)
{
   /* A grouped list starts with a group. */
   if (!list->size || list->list[0].type != MENU_ROMM_GROUP)
      return 0;
   return (unsigned)list->list[i].entry_idx;
}

/* @at, in seconds since the epoch, as local "2026-10-20 14:54". */
static void romm_menu_time(char *s, size_t len, int64_t at)
{
   time_t t      = (time_t)at;
   struct tm *tm = localtime(&t);
   if (!tm || !strftime(s, len, "%Y-%m-%d %H:%M", tm))
      *s = '\0';
}

static bool romm_menu_status_line(file_list_t *list, const char *line)
{
   return menu_entries_append(list, line, "",
         MENU_ENUM_LABEL_ROMM_STATUS_ENTRY, MENU_SETTINGS_CORE_INFO_NONE,
         0, 0, NULL);
}

unsigned romm_menu_status_list(file_list_t *list)
{
   settings_t *settings = config_get_ptr();
   unsigned count       = 0;
   char line[512];

   romm_manifest_load();
   if (string_is_empty(settings->arrays.romm_token))
      strlcpy(line, msg_hash_to_str(MSG_ROMM_STATUS_NOT_PAIRED),
            sizeof(line));
   else if (*settings->arrays.romm_username)
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_STATUS_PAIRED_AS),
            settings->arrays.romm_username);
   else
      strlcpy(line, msg_hash_to_str(MSG_ROMM_STATUS_PAIRED), sizeof(line));
   if (romm_menu_status_line(list, line))
      count++;

   if (romm_syncing)
      strlcpy(line, msg_hash_to_str(MSG_ROMM_STATUS_SYNCING), sizeof(line));
   else if (romm_library_synced_at > 0)
   {
      char when[32];
      romm_menu_time(when, sizeof(when), romm_library_synced_at);
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_STATUS_LIBRARY),
            romm_library_games, romm_library_systems, when);
   }
   else
      strlcpy(line, msg_hash_to_str(MSG_ROMM_STATUS_NOT_SYNCED),
            sizeof(line));
   if (romm_menu_status_line(list, line))
      count++;

   if (*romm_library_waiting)
   {
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_STATUS_WAITING),
            romm_library_waiting);
      if (romm_menu_status_line(list, line))
         count++;
   }
   if (*romm_library_new)
   {
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_STATUS_NEW),
            romm_library_new);
      if (romm_menu_status_line(list, line))
         count++;
   }
   return count;
}

unsigned romm_menu_sidebar_list(file_list_t *list, const char *key)
{
   size_t i;
   const char *dir        = config_get_ptr()->paths.directory_playlist;
   const char **playlists = NULL;
   unsigned count         = 0;

   romm_manifest_load();
   romm_sidebar_members(&playlists, romm_sidebar_cache, key);
   for (i = 0; i < RBUF_LEN(playlists); i++)
   {
      char path[PATH_MAX_LENGTH];
      fill_pathname_join_special(path, dir, playlists[i], sizeof(path));
      /* A platform without games has no playlist. */
      if (!path_is_valid(path))
         continue;
      if (menu_entries_append(list, path, "",
            MENU_ENUM_LABEL_PLAYLIST_COLLECTION_ENTRY,
            FILE_TYPE_PLAYLIST_COLLECTION, 0, count, NULL))
         count++;
   }
   RBUF_FREE(playlists);
   return count;
}

/* The systems as the Systems page lists them (an RBUF), kept for the
 * values its rows show each frame. */
static romm_system_row_t *romm_menu_rows = NULL;

/* The system whose Stop Syncing waits for a second OK, and whether the
 * page was just rebuilt to show that; the next rebuild disarms it. */
static char romm_menu_armed[64];
static bool romm_menu_arming = false;

static void romm_menu_refresh(void)
{
   menu_state_get_ptr()->flags |= MENU_ST_FLAG_ENTRIES_NEED_REFRESH;
}

/* Lists the library's systems in romm_menu_rows, from the manifest and
 * the systems switched off and new. */
static void romm_menu_load_rows(void)
{
   settings_t *settings = config_get_ptr();
   romm_library_t lib;
   char path[PATH_MAX_LENGTH];

   romm_library_manifest_path(path, sizeof(path),
         settings->paths.directory_playlist);
   romm_library_load(&lib, path);
   romm_sidebar_system_rows(&romm_menu_rows, &lib,
         settings->arrays.romm_sync_exclude, settings->arrays.romm_sync_new,
         romm_library_waiting_list, RBUF_LEN(romm_library_waiting_list));
   romm_library_free(&lib);
}

static romm_system_row_t *romm_menu_find_row(const char *fs_slug)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(romm_menu_rows); i++)
      if (string_is_equal(romm_menu_rows[i].platform.fs_slug, fs_slug))
         return &romm_menu_rows[i];
   return NULL;
}

unsigned romm_menu_systems_list(file_list_t *list)
{
   unsigned count = 0;
   size_t i;

   *romm_menu_armed = '\0';
   romm_menu_load_rows();
   if (!RBUF_LEN(romm_menu_rows))
      return romm_menu_status_line(list,
            msg_hash_to_str(MSG_ROMM_SYSTEMS_EMPTY)) ? 1 : 0;

   for (i = 0; i < RBUF_LEN(romm_menu_rows); i++)
   {
      const romm_system_row_t *row = &romm_menu_rows[i];
      const romm_system_row_t *prev = i ? &romm_menu_rows[i - 1] : NULL;
      bool no_emulator = row->state == ROMM_SYSTEM_NO_EMULATOR;
      const char *group = NULL;

      /* A heading for each maker, then one for the systems no emulator
       * runs. */
      if (no_emulator)
      {
         if (!prev || prev->state != ROMM_SYSTEM_NO_EMULATOR)
            group = msg_hash_to_str(MSG_ROMM_SYSTEMS_NO_EMULATOR);
      }
      else if (!prev || !string_is_equal_noncase(prev->maker, row->maker))
         group = *row->maker ? row->maker
               : msg_hash_to_str(MSG_ROMM_SIDEBAR_OTHER);
      if (group && menu_entries_append(list, group, "",
            MENU_ENUM_LABEL_ROMM_SYSTEM_GROUP, MENU_SETTINGS_CORE_INFO_NONE,
            0, 0, NULL))
         count++;

      if (menu_entries_append(list, row->platform.fs_slug, "",
            MENU_ENUM_LABEL_ROMM_SYSTEM_ENTRY,
            no_emulator ? MENU_ROMM_SYSTEM_NO_EMULATOR : MENU_ROMM_SYSTEM,
            0, 0, NULL))
      {
         file_list_set_alt_at_offset(list, list->size - 1,
               row->platform.name);
         count++;
      }
   }
   return count;
}

unsigned romm_menu_playlist_systems(file_list_t *list,
      const char *playlist_file)
{
   unsigned count = 0;
   size_t i;
   char text[256];

   romm_menu_load_rows();
   for (i = 0; i < RBUF_LEN(romm_menu_rows); i++)
   {
      const romm_system_row_t *row = &romm_menu_rows[i];
      if (!string_is_equal(row->playlist, playlist_file))
         continue;
      snprintf(text, sizeof(text),
            msg_hash_to_str(MSG_ROMM_SYSTEM_OPTIONS), row->platform.name);
      if (menu_entries_append(list, row->platform.fs_slug, "",
            MENU_ENUM_LABEL_ROMM_SYSTEM_OPTIONS, MENU_ROMM_SYSTEM,
            0, 0, NULL))
      {
         file_list_set_alt_at_offset(list, list->size - 1, text);
         count++;
      }
   }
   return count;
}

size_t romm_menu_system_value(const char *fs_slug, char *s, size_t len)
{
   const romm_system_row_t *row = romm_menu_find_row(fs_slug);

   *s = '\0';
   if (!row)
      return 0;
   switch (row->state)
   {
      case ROMM_SYSTEM_SYNCED:
         return snprintf(s, len,
               msg_hash_to_str(MSG_ROMM_SYSTEM_VALUE_GAMES), row->games);
      case ROMM_SYSTEM_WAITING:
         return snprintf(s, len,
               msg_hash_to_str(MSG_ROMM_SYSTEM_VALUE_WAITING),
               row->games_waiting ? row->games_waiting : row->covers_waiting);
      case ROMM_SYSTEM_NOT_SYNCED:
         return strlcpy(s,
               msg_hash_to_str(MSG_ROMM_SYSTEM_VALUE_NOT_SYNCED), len);
      case ROMM_SYSTEM_OFF:
         return strlcpy(s, msg_hash_to_str(MSG_ROMM_SYSTEM_VALUE_OFF), len);
      case ROMM_SYSTEM_NEW:
         return strlcpy(s, msg_hash_to_str(MSG_ROMM_SYSTEM_VALUE_NEW), len);
      case ROMM_SYSTEM_NO_EMULATOR:
         return strlcpy(s,
               msg_hash_to_str(MSG_ROMM_SYSTEM_VALUE_NO_EMULATOR), len);
   }
   return 0;
}

size_t romm_menu_system_title(const char *fs_slug, char *s, size_t len)
{
   const romm_system_row_t *row = romm_menu_find_row(fs_slug);
   if (!row)
   {
      romm_menu_load_rows();
      row = romm_menu_find_row(fs_slug);
   }
   return strlcpy(s, row ? row->platform.name : fs_slug, len);
}

/* An action of a system's page, labelled @text. */
static bool romm_menu_action_row(file_list_t *list, const char *fs_slug,
      enum msg_hash_enums label, const char *text)
{
   if (!menu_entries_append(list, fs_slug, "", label, MENU_ROMM_SYSTEM,
         0, 0, NULL))
      return false;
   file_list_set_alt_at_offset(list, list->size - 1, text);
   return true;
}

/* Covers on disk in @playlist's box art folder: how many and their
 * size. A folder two systems share counts both. */
static void romm_menu_covers(const char *playlist, unsigned *count,
      uint64_t *bytes)
{
   settings_t *settings = config_get_ptr();
   struct string_list *files;
   char name[NAME_MAX_LENGTH];
   char dir[PATH_MAX_LENGTH];
   char boxarts[PATH_MAX_LENGTH];
   size_t i;

   *count = 0;
   *bytes = 0;
   fill_pathname(name, playlist, "", sizeof(name));
   fill_pathname_join(dir, settings->paths.directory_thumbnails, name,
         sizeof(dir));
   fill_pathname_join(boxarts, dir, "Named_Boxarts", sizeof(boxarts));
   if (!(files = dir_list_new(boxarts, "png|jpg", false, false, false,
         false)))
      return;
   for (i = 0; i < files->size; i++)
   {
      int32_t size = path_get_size(files->elems[i].data);
      if (size > 0)
         *bytes += (uint64_t)size;
      (*count)++;
   }
   string_list_free(files);
}

unsigned romm_menu_system_page(file_list_t *list, const char *fs_slug)
{
   settings_t *settings = config_get_ptr();
   romm_system_row_t *row;
   enum romm_system_state state;
   unsigned count = 0;
   size_t i;
   char line[512];

   if (!romm_menu_arming)
      *romm_menu_armed = '\0';
   romm_menu_arming = false;
   romm_menu_load_rows();
   if (!(row = romm_menu_find_row(fs_slug)))
      return romm_menu_status_line(list,
            msg_hash_to_str(MSG_ROMM_SYSTEM_GONE)) ? 1 : 0;

   /* Shown now, so no longer marked new: this page still says it is. */
   state = row->state;
   if (state == ROMM_SYSTEM_NEW)
   {
      romm_library_list_remove(settings->arrays.romm_sync_new, fs_slug);
      if (!romm_overrides_active())
         command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
      row->state        = ROMM_SYSTEM_OFF;
      *romm_library_new = '\0';
      for (i = 0; i < RBUF_LEN(romm_menu_rows); i++)
         if (romm_menu_rows[i].state == ROMM_SYSTEM_NEW)
         {
            if (*romm_library_new)
               strlcat(romm_library_new, ", ", sizeof(romm_library_new));
            strlcat(romm_library_new, romm_menu_rows[i].platform.name,
                  sizeof(romm_library_new));
         }
   }

   if (romm_syncing && string_is_equal(romm_system_job.fs_slug, fs_slug))
      strlcpy(line, msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_SYNCING),
            sizeof(line));
   else switch (state)
   {
      case ROMM_SYSTEM_SYNCED:
         strlcpy(line, msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_SYNCED),
               sizeof(line));
         break;
      case ROMM_SYSTEM_WAITING:
         snprintf(line, sizeof(line),
               msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_WAITING),
               row->games_waiting, row->covers_waiting);
         break;
      case ROMM_SYSTEM_NOT_SYNCED:
         strlcpy(line, msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_NOT_SYNCED),
               sizeof(line));
         break;
      case ROMM_SYSTEM_OFF:
         strlcpy(line, msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_OFF),
               sizeof(line));
         break;
      case ROMM_SYSTEM_NEW:
         strlcpy(line, msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_NEW),
               sizeof(line));
         break;
      case ROMM_SYSTEM_NO_EMULATOR:
         strlcpy(line, msg_hash_to_str(MSG_ROMM_SYSTEM_STATUS_NO_EMULATOR),
               sizeof(line));
         break;
   }
   if (romm_menu_status_line(list, line))
      count++;

   snprintf(line, sizeof(line), msg_hash_to_str(*row->playlist
         ? MSG_ROMM_SYSTEM_GAMES : MSG_ROMM_SYSTEM_GAMES_IN_ROMM),
         row->games);
   if (romm_menu_status_line(list, line))
      count++;

   if (*row->playlist)
   {
      unsigned covers;
      uint64_t bytes;
      char when[32];
      if (row->synced_at > 0)
      {
         romm_menu_time(when, sizeof(when), row->synced_at);
         snprintf(line, sizeof(line),
               msg_hash_to_str(MSG_ROMM_SYSTEM_SYNCED_AT), when);
         if (romm_menu_status_line(list, line))
            count++;
      }
      romm_menu_covers(row->playlist, &covers, &bytes);
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_SYSTEM_COVERS),
            covers, (double)bytes / (1024.0 * 1024.0));
      if (romm_menu_status_line(list, line))
         count++;
   }

   snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_SYSTEM_FOLDER),
         row->platform.fs_slug);
   if (romm_menu_status_line(list, line))
      count++;

   if (state == ROMM_SYSTEM_NO_EMULATOR)
      return count;
   if (state == ROMM_SYSTEM_OFF || state == ROMM_SYSTEM_NEW)
   {
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_SYSTEM_DO_SYNC),
            row->platform.name);
      if (romm_menu_action_row(list, fs_slug,
            MENU_ENUM_LABEL_ROMM_SYSTEM_SYNC, line))
         count++;
      return count;
   }
   if (*row->playlist)
   {
      if (romm_menu_action_row(list, fs_slug,
            MENU_ENUM_LABEL_ROMM_SYSTEM_REFRESH,
            msg_hash_to_str(MSG_ROMM_SYSTEM_DO_REFRESH)))
         count++;
      if (romm_menu_action_row(list, fs_slug,
            MENU_ENUM_LABEL_ROMM_SYSTEM_REFETCH,
            msg_hash_to_str(MSG_ROMM_SYSTEM_DO_REFETCH)))
         count++;
      if (romm_menu_action_row(list, fs_slug,
            MENU_ENUM_LABEL_ROMM_SYSTEM_REBUILD,
            msg_hash_to_str(MSG_ROMM_SYSTEM_DO_REBUILD)))
         count++;
   }
   else if (romm_menu_action_row(list, fs_slug,
         MENU_ENUM_LABEL_ROMM_SYSTEM_REFRESH,
         msg_hash_to_str(MSG_ROMM_SYSTEM_DO_SYNC_NOW)))
      count++;
   snprintf(line, sizeof(line), msg_hash_to_str(
         string_is_equal(romm_menu_armed, fs_slug)
         ? MSG_ROMM_SYSTEM_DO_STOP_CONFIRM : MSG_ROMM_SYSTEM_DO_STOP),
         row->platform.name);
   if (romm_menu_action_row(list, fs_slug,
         MENU_ENUM_LABEL_ROMM_SYSTEM_STOP, line))
      count++;
   return count;
}

void romm_menu_system_action(const char *fs_slug,
      enum romm_system_action action)
{
   settings_t *settings = config_get_ptr();
   bool armed           = string_is_equal(romm_menu_armed, fs_slug);
   romm_system_job_t job;
   char msg[256];
   size_t i;

   *romm_menu_armed = '\0';
   if (action == ROMM_SYSTEM_ACTION_STOP && !armed)
   {
      strlcpy(romm_menu_armed, fs_slug, sizeof(romm_menu_armed));
      romm_menu_arming = true;
      romm_menu_refresh();
      return;
   }
   if (!romm_can_sync())
      return;

   if (action == ROMM_SYSTEM_ACTION_STOP || action == ROMM_SYSTEM_ACTION_SYNC)
   {
      if (action == ROMM_SYSTEM_ACTION_STOP)
         romm_library_list_add(settings->arrays.romm_sync_exclude,
               sizeof(settings->arrays.romm_sync_exclude), fs_slug);
      else
         romm_library_list_remove(settings->arrays.romm_sync_exclude,
               fs_slug);
      romm_library_list_remove(settings->arrays.romm_sync_new, fs_slug);
      if (!romm_overrides_active())
         command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   }

   strlcpy(job.fs_slug, fs_slug, sizeof(job.fs_slug));
   job.action = action;
   if (!romm_syncing)
      romm_start_system_job(&job);
   else
   {
      const romm_system_row_t *row = romm_menu_find_row(fs_slug);
      for (i = 0; i < RBUF_LEN(romm_system_jobs); i++)
         if (string_is_equal(romm_system_jobs[i].fs_slug, fs_slug))
            break;
      if (i < RBUF_LEN(romm_system_jobs))
         romm_system_jobs[i].action = action;
      else
         RBUF_PUSH(romm_system_jobs, job);
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SYSTEM_QUEUED),
            row ? row->platform.name : fs_slug);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_INFO);
   }
   romm_menu_refresh();
}

/* The game the Information page last asked RomM about, and its
 * details; a failure is asked again a few seconds on. */
static unsigned romm_info_id          = 0;
static bool romm_info_loaded          = false;
static bool romm_info_loading         = false;
static retro_time_t romm_info_failed  = 0;
static romm_rom_details_t romm_info;

static void romm_on_game_details(void *user_data, enum romm_status status,
      int http_status, const romm_rom_details_t *details)
{
   if ((unsigned)(uintptr_t)user_data != romm_info_id)
      return;
   romm_info_loading = false;
   if (status == ROMM_OK && details)
   {
      romm_info        = *details;
      romm_info_loaded = true;
   }
   else
      romm_info_failed = cpu_features_get_time_usec();
   romm_menu_refresh();
}

/* "12.3 MB" */
static void romm_menu_size(char *s, size_t len, uint64_t bytes)
{
   if (bytes >= 1024ULL * 1024 * 1024)
      snprintf(s, len, "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
   else if (bytes >= 1024 * 1024)
      snprintf(s, len, "%.1f MB", (double)bytes / (1024.0 * 1024));
   else
      snprintf(s, len, "%u KB", (unsigned)((bytes + 1023) / 1024));
}

static unsigned romm_menu_info_line(file_list_t *list, enum msg_hash_enums fmt,
      const char *value)
{
   char line[512];
   if (!*value)
      return 0;
   snprintf(line, sizeof(line), msg_hash_to_str(fmt), value);
   return romm_menu_status_line(list, line) ? 1 : 0;
}

unsigned romm_menu_game_info(file_list_t *list, const char *content_path)
{
   settings_t *settings = config_get_ptr();
   unsigned id          = romm_library_path_id(content_path);
   unsigned count       = 0;
   char value[64];

   if (!id)
      return 0;
   if (id != romm_info_id)
   {
      romm_info_id      = id;
      romm_info_loaded  = false;
      romm_info_loading = false;
      romm_info_failed  = 0;
   }
   if (     !romm_info_loaded && !romm_info_loading
         && (   !romm_info_failed
             || cpu_features_get_time_usec() - romm_info_failed > 5000000)
         && romm_client_rom_details(settings->arrays.romm_server_url,
               settings->arrays.romm_token, id, romm_on_game_details,
               (void*)(uintptr_t)id))
      romm_info_loading = true;

   if (!romm_info_loaded)
      return romm_menu_status_line(list, msg_hash_to_str(romm_info_loading
            ? MSG_ROMM_GAME_LOADING : MSG_ROMM_GAME_UNAVAILABLE)) ? 1 : 0;

   if (romm_info.release_date > 0)
   {
      time_t t      = (time_t)(romm_info.release_date / 1000);
      struct tm *tm = gmtime(&t);
      if (!tm || !strftime(value, sizeof(value), "%Y-%m-%d", tm))
         *value = '\0';
      count += romm_menu_info_line(list, MSG_ROMM_GAME_RELEASED, value);
   }
   count += romm_menu_info_line(list, MSG_ROMM_GAME_GENRES, romm_info.genres);
   count += romm_menu_info_line(list, MSG_ROMM_GAME_COMPANIES,
         romm_info.companies);
   count += romm_menu_info_line(list, MSG_ROMM_GAME_REGIONS,
         romm_info.regions);
   count += romm_menu_info_line(list, MSG_ROMM_GAME_LANGUAGES,
         romm_info.languages);
   if (*romm_info.rating)
   {
      snprintf(value, sizeof(value), "%.0f",
            strtod(romm_info.rating, NULL));
      count += romm_menu_info_line(list, MSG_ROMM_GAME_RATING, value);
   }
   if (romm_info.size)
   {
      romm_menu_size(value, sizeof(value), romm_info.size);
      count += romm_menu_info_line(list, MSG_ROMM_GAME_SIZE, value);
   }
   /* One row, its start; OK shows it all. */
   if (     *romm_info.summary
         && menu_entries_append(list, content_path, "",
               MENU_ENUM_LABEL_ROMM_GAME_SUMMARY, MENU_ROMM_SYSTEM, 0, 0,
               NULL))
   {
      char start[40];
      char line[128];
      size_t _len = strlcpy(start, romm_info.summary, sizeof(start) - 4);
      /* Its first words, short enough for the row not to scroll */
      if (_len >= sizeof(start) - 4)
      {
         char *space = strrchr(start, ' ');
         if (space && space > start + 16)
            *space = '\0';
         strlcat(start, "...", sizeof(start));
      }
      snprintf(line, sizeof(line), msg_hash_to_str(MSG_ROMM_GAME_SUMMARY),
            start);
      file_list_set_alt_at_offset(list, list->size - 1, line);
      count++;
   }
   return count;
}

bool romm_menu_game_summary(char *s, size_t len)
{
   if (!romm_info_loaded || !*romm_info.summary)
      return false;
   strlcpy(s, romm_info.summary, len);
   return true;
}

unsigned romm_menu_game_open_row(file_list_t *list, const char *content_path)
{
   if (     !romm_library_path_id(content_path)
         || !menu_entries_append(list, content_path, "",
               MENU_ENUM_LABEL_ROMM_OPEN_GAME, MENU_ROMM_SYSTEM, 0, 0, NULL))
      return 0;
   file_list_set_alt_at_offset(list, list->size - 1,
         msg_hash_to_str(MSG_ROMM_GAME_OPEN));
   return 1;
}

void romm_menu_open_game(const char *content_path)
{
   settings_t *settings = config_get_ptr();
   const char *base     = *settings->arrays.romm_web_url
      ? settings->arrays.romm_web_url : settings->arrays.romm_server_url;
   unsigned id          = romm_library_path_id(content_path);
   char url[1024];
   size_t _len;

   if (!id || !*base)
      return;
   _len = strlcpy(url, base, sizeof(url));
   while (_len && url[_len - 1] == '/')
      url[--_len] = '\0';
   snprintf(url + _len, sizeof(url) - _len, "/rom/%u", id);
   if (!romm_platform_open_url(url))
      romm_notify(url, MESSAGE_QUEUE_CATEGORY_INFO);
}
#endif

/* The name of the system with RomM folder @fs_slug in @lib. */
static const char *romm_library_system_name(const romm_library_t *lib,
      const char *fs_slug, size_t *games)
{
   size_t i;
   *games = 0;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      if (string_is_equal(lib->platforms[i].platform.fs_slug, fs_slug))
      {
         *games = RBUF_LEN(lib->platforms[i].roms);
         return lib->platforms[i].platform.name;
      }
   for (i = 0; i < RBUF_LEN(lib->unsynced); i++)
      if (string_is_equal(lib->unsynced[i].fs_slug, fs_slug))
         return lib->unsynced[i].name;
   return fs_slug;
}

static void romm_library_synced(enum romm_status status, int http_status,
      const romm_library_t *old, const romm_library_t *lib);

static void romm_on_library_synced(void *user_data, enum romm_status status,
      int http_status, const romm_library_t *old, const romm_library_t *lib)
{
   romm_library_synced(status, http_status, old, lib);
   *romm_system_job.fs_slug = '\0';
   romm_start_next_system_job();
}

static void romm_library_synced(enum romm_status status, int http_status,
      const romm_library_t *old, const romm_library_t *lib)
{
   bool quiet   = romm_syncing_quietly;
   size_t games = 0;
   size_t i;
   char msg[256];

   romm_syncing = false;
   /* Even a sync that failed part way may have noted them. */
   if (RBUF_LEN(lib->added))
      romm_note_added(lib);
   if (status == ROMM_OK)
   {
      const romm_rom_t **old_index = romm_library_index(old);
      const romm_rom_t **index     = romm_library_index(lib);
      bool favorites_kept          = romm_favorites_changes
            == romm_favorites_changes_at_sync && !romm_favorites_creating;
      if (g_defaults.content_history)
         romm_follow_games(g_defaults.content_history, old_index, index);
      if (g_defaults.content_favorites)
      {
         romm_follow_games(g_defaults.content_favorites, old_index, index);
         if (lib->favorites_listed && favorites_kept)
            romm_follow_favorites(g_defaults.content_favorites, lib, index);
      }
      RBUF_FREE(old_index);
      RBUF_FREE(index);
      if (favorites_kept)
         romm_favorites_collection = lib->favorites_id;
      romm_note_library(lib, *romm_system_job.fs_slug
            ? romm_system_job.fs_slug : NULL);
#if defined(HAVE_MENU) && defined(HAVE_UPDATE_CORES)
      romm_want_missing_cores(lib, *romm_system_job.fs_slug
            ? romm_system_job.fs_slug : NULL,
            *romm_system_job.fs_slug
            && romm_system_job.action == ROMM_SYSTEM_ACTION_REFETCH);
#endif
#ifdef HAVE_MENU
      romm_sidebar_systems(&romm_sidebar_cache, lib);
#endif
      romm_manifest_read = true;
      romm_ask_username();
   }
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
   /* Said by the sync that left them, the one at start: the status
    * lines keep them listed after a sync of other systems. */
   if (RBUF_LEN(lib->waiting))
   {
      char names[128];
      *names = '\0';
      for (i = 0; i < RBUF_LEN(lib->waiting); i++)
      {
         if (i)
            strlcat(names, ", ", sizeof(names));
         strlcat(names, lib->waiting[i].platform.name, sizeof(names));
      }
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SYNC_WAITING),
            config_get_ptr()->uints.romm_sync_start_limit, names);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_INFO);
   }
   if (quiet)
      return;
   if (*romm_system_job.fs_slug)
   {
      const char *name = romm_library_system_name(lib,
            romm_system_job.fs_slug, &games);
      if (romm_system_job.action == ROMM_SYSTEM_ACTION_STOP)
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_SYSTEM_STOPPED), name);
      else
         snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SYSTEM_SYNCED),
               name, (unsigned)games);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
      romm_sync_owed_saves();
      return;
   }
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      games += RBUF_LEN(lib->platforms[i].roms);
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SYNCED),
         (unsigned)games, (unsigned)RBUF_LEN(lib->platforms));
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
   romm_sync_owed_saves();
}

/* A core whose info file doesn't list the system can't run it, such as
 * Handy set on Game Boy; one without an info file is given the benefit
 * of the doubt. */
static bool romm_core_runs(const char *core_path, const char *libretro_slug)
{
   core_info_t *info = NULL;
   if (     !core_info_find(core_path, &info) || !info
         || !info->databases_list)
      return true;
   return string_list_find_elem(info->databases_list, libretro_slug) != 0;
}

/* @job: the one system to sync, and how; NULL for every system. */
static bool romm_start_library_sync(bool quiet, const char *refetch,
      unsigned limit, const romm_system_job_t *job)
{
   settings_t *settings = config_get_ptr();
   romm_library_sync_params_t params;

   memset(&params, 0, sizeof(params));
   params.server         = settings->arrays.romm_server_url;
   params.token          = settings->arrays.romm_token;
   params.playlist_dir   = settings->paths.directory_playlist;
   params.thumbnails_dir = settings->paths.directory_thumbnails;
   params.title          = msg_hash_to_str(MSG_ROMM_SYNCING);
   params.covers_title   = msg_hash_to_str(MSG_ROMM_FETCHING_COVERS);
   params.find_core      = romm_find_core;
   params.playlist_changed = romm_playlist_changed;
   params.collections    = true;
   params.refetch        = refetch;
   params.core_runs      = romm_core_runs;
   params.limit          = limit;
   params.exclude        = settings->arrays.romm_sync_exclude;
   params.new_off        = true;
   if (job)
   {
      params.only        = job->fs_slug;
      params.full        = job->action == ROMM_SYSTEM_ACTION_REFETCH;
      params.rebuild     = job->action == ROMM_SYSTEM_ACTION_REBUILD;
   }
   romm_favorites_changes_at_sync = romm_favorites_changes;
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
      romm_start_library_sync(true, NULL,
            settings->uints.romm_sync_on_start == ROMM_SYNC_START_PARTIAL
            ? settings->uints.romm_sync_start_limit : 0, NULL);
}

/* Says why there can't be a sync, if there can't. */
static bool romm_can_sync(void)
{
   settings_t *settings = config_get_ptr();

   if (romm_unpairing)
      return false;
   if (string_is_empty(settings->arrays.romm_server_url))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NO_SERVER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return false;
   }
   if (string_is_empty(settings->arrays.romm_token))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NOT_PAIRED),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return false;
   }
   return true;
}

static void romm_refresh(const char *refetch)
{
   if (romm_syncing)
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_SYNC_IN_PROGRESS),
            MESSAGE_QUEUE_CATEGORY_INFO);
      return;
   }
   if (!romm_can_sync())
      return;

   if (!romm_start_library_sync(false, refetch, 0, NULL))
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
}

static void romm_start_system_job(const romm_system_job_t *job)
{
   romm_system_job = *job;
   if (!romm_start_library_sync(false, NULL, 0, job))
   {
      *romm_system_job.fs_slug = '\0';
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
   }
}

/* The next system waiting for a sync, once none runs. */
static void romm_start_next_system_job(void)
{
   romm_system_job_t job;
   size_t i;

   if (romm_syncing || !RBUF_LEN(romm_system_jobs))
      return;
   job = romm_system_jobs[0];
   for (i = 1; i < RBUF_LEN(romm_system_jobs); i++)
      romm_system_jobs[i - 1] = romm_system_jobs[i];
   RBUF_RESIZE(romm_system_jobs, RBUF_LEN(romm_system_jobs) - 1);
   romm_start_system_job(&job);
}

void romm_refresh_library(void)
{
   romm_refresh(NULL);
}

void romm_refresh_playlist(void)
{
   playlist_t *playlist = playlist_get_cached();
   const char *path     = playlist ? playlist_get_conf_path(playlist) : NULL;

   if (path && *path)
      romm_refresh(path_basename(path));
}

/* The entry being fetched, loaded once it's in memory and its saves
 * are synced; and whether the fetch is what's running */
static bool romm_launching = false;
static bool romm_fetching  = false;
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

bool romm_content_dir_name(char *s, size_t len, const char *content_path)
{
   unsigned rom_id = romm_launch_rom_id(content_path);
   if (!rom_id)
      return false;
   snprintf(s, len, "%u", rom_id);
   return true;
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
      if (!romm_content_dir_name(name, sizeof(name), path))
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

/* The name RetroArch files @core_path's saves under: the one the core
 * gives itself, which can differ from @core_name, its info file's, in
 * case (vecx, VecX), two folders where names are case-sensitive. */
static bool romm_core_folder_name(char *s, size_t len,
      const char *core_path, const char *core_name)
{
   struct retro_system_info sysinfo;
   if (     libretro_get_system_info(core_path, &sysinfo, NULL)
         && !string_is_empty(sysinfo.library_name))
      return strlcpy(s, sysinfo.library_name, len) > 0;
   return strlcpy(s, core_name, len) > 0;
}

/* @path's file name without its extension, as RetroArch names saves. */
static void romm_saves_base(char *s, size_t len, const char *path)
{
   char *dot;
   strlcpy(s, path_basename(path), len);
   if ((dot = strrchr(s, '.')) && dot != s)
      *dot = '\0';
}

/* @name, a file name from RomM, as one this system can store. */
static void romm_name_for_disk(char *name, size_t len)
{
#ifdef _WIN32
   char *clean = sanitize_path_part(name, strlen(name));
   if (clean)
   {
      strlcpy(name, clean, len);
      free(clean);
   }
#endif
}

void romm_content_base_name(char *s, size_t len)
{
   char *name;
   if (!romm_launch_rom_id(s))
      return;
   name = (char*)path_basename(s);
   romm_name_for_disk(name, len - (size_t)(name - s));
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

/* The save RAM file of the content closing, as its save file list
 * names it; false when it names none. */
static bool romm_closing_savefile(char *s, size_t len)
{
   const struct string_list *list =
      (const struct string_list*)savefile_ptr_get();
   size_t i;

   if (!list)
      return false;
   for (i = 0; i < list->size; i++)
      if (list->elems[i].attr.i == RETRO_MEMORY_SAVE_RAM)
      {
         strlcpy(s, list->elems[i].data, len);
         return true;
      }
   return false;
}

/* Is @game the content closing, whose save RAM is @live? Only its name
 * is sure before it loads: overrides and the core's own name for
 * itself can put its folder elsewhere. */
static bool romm_played_is_closing(const romm_played_t *game,
      const char *live)
{
   char name[NAME_MAX_LENGTH];

   if (!game->rom_id)
      return false;
   strlcpy(name, game->base, sizeof(name));
   strlcat(name, FILE_PATH_SRM_EXTENSION, sizeof(name));
   return string_is_equal_noncase(path_basename(live), name);
}

static void romm_strip_trailing_slash(char *s)
{
   size_t _len = strlen(s);
   while (_len > 1 && (s[_len - 1] == '/' || s[_len - 1] == '\\'))
      s[--_len] = '\0';
}

/* Takes for @game the folders the closing content used, its save RAM
 * being @live. False when they're in memory beside the content. */
static bool romm_played_follow(romm_played_t *game, const char *live)
{
   const char *dir = dir_get_ptr(RARCH_DIR_CURRENT_SAVESTATE);
   char save_dir[PATH_MAX_LENGTH];
   char state_dir[PATH_MAX_LENGTH];

   fill_pathname_basedir(save_dir, live, sizeof(save_dir));
   romm_strip_trailing_slash(save_dir);
   strlcpy(state_dir, string_is_empty(dir) ? game->state_dir : dir,
         sizeof(state_dir));
   romm_strip_trailing_slash(state_dir);
   if (     string_starts_with(save_dir, ROMM_SCHEME)
         || string_starts_with(state_dir, ROMM_SCHEME))
   {
      RARCH_WARN("[RomM] Saves of game %u went beside the game, in "
            "memory; not sent.\n", game->rom_id);
      return false;
   }
   if (     !string_is_equal(save_dir, game->save_dir)
         || !string_is_equal(state_dir, game->state_dir))
      RARCH_WARN("[RomM] Game %u kept its saves in %s and states in %s, "
            "not %s and %s; they're sent from there.\n", game->rom_id,
            save_dir, state_dir, game->save_dir, game->state_dir);
   strlcpy(game->save_dir, save_dir, sizeof(game->save_dir));
   strlcpy(game->state_dir, state_dir, sizeof(game->state_dir));
   return true;
}

/* A game whose owed saves are on their way up */
typedef struct
{
   unsigned rom_id;
   char emulator[64];
} romm_owed_t;

/* Owed syncs still out, and how many games ended in a conflict */
static romm_owed_t **romm_owed      = NULL;
static unsigned romm_owed_conflicts = 0;

static bool romm_owed_in_flight(unsigned rom_id, const char *emulator)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(romm_owed); i++)
      if (     romm_owed[i]->rom_id == rom_id
            && string_is_equal(romm_owed[i]->emulator, emulator))
         return true;
   return false;
}

static void romm_on_owed_pushed(void *user_data, enum romm_status status,
      int http_status, const romm_saves_result_t *result)
{
   romm_owed_t *owed = (romm_owed_t*)user_data;
   size_t i;
   char msg[256];

   for (i = 0; i < RBUF_LEN(romm_owed); i++)
      if (romm_owed[i] == owed)
      {
         for (; i + 1 < RBUF_LEN(romm_owed); i++)
            romm_owed[i] = romm_owed[i + 1];
         RBUF_RESIZE(romm_owed, RBUF_LEN(romm_owed) - 1);
         break;
      }
   free(owed);
   if (status == ROMM_OK && result->conflicts)
      romm_owed_conflicts++;
   if (RBUF_LEN(romm_owed))
      return;
   RBUF_FREE(romm_owed);
   if (!romm_owed_conflicts)
      return;
   snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_SAVES_OWED_CONFLICTS),
         romm_owed_conflicts);
   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_WARNING);
   romm_owed_conflicts = 0;
}

void romm_sync_owed_saves(void)
{
   settings_t *settings = config_get_ptr();
   romm_saves_record_t rec;
   char record[PATH_MAX_LENGTH];
   size_t i;

   if (     string_is_empty(settings->arrays.romm_token)
         || string_is_empty(dir_get_ptr(RARCH_DIR_SAVEFILE))
         || !romm_saves_allowed())
      return;
   romm_saves_record_path(record, sizeof(record),
         dir_get_ptr(RARCH_DIR_SAVEFILE));
   if (!romm_saves_record_load(&rec, record))
      return;
   for (i = 0; i < RBUF_LEN(rec.games); i++)
   {
      const romm_saves_game_t *g = &rec.games[i];
      romm_played_t game;
      romm_owed_t *owed;

      if (     !g->pending || !*g->save_dir || !*g->state_dir || !*g->base
            || romm_owed_in_flight(g->rom_id, g->emulator))
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
      if (!(owed = (romm_owed_t*)malloc(sizeof(*owed))))
         break;
      owed->rom_id = game.rom_id;
      strlcpy(owed->emulator, game.emulator, sizeof(owed->emulator));
      RARCH_LOG("[RomM] Sending saves owed for game %u (%s).\n",
            game.rom_id, game.emulator);
      if (romm_sync_played(&game, true, ROMM_SAVES_RESOLVE_NONE,
            romm_on_owed_pushed, owed))
         RBUF_PUSH(romm_owed, owed);
      else
         free(owed);
   }
   romm_saves_record_free(&rec);
}

void romm_content_closed(void)
{
   romm_played_t *game = NULL;
   char live[PATH_MAX_LENGTH];

   if (!romm_saves_allowed() || !romm_closing_savefile(live, sizeof(live)))
      return;
   if (romm_played_is_closing(&romm_closing, live))
      game = &romm_closing;
   else if (romm_played_is_closing(&romm_running, live))
      game = &romm_running;
   if (!game)
      return;
   if (     romm_played_follow(game, live)
         && !romm_sync_played(game, true, ROMM_SAVES_RESOLVE_NONE,
               romm_on_saves_pushed, NULL))
      romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_NOT_SENT),
            MESSAGE_QUEUE_CATEGORY_WARNING);
   memset(game, 0, sizeof(*game));
}

/* Said once a run that saves don't sync until pairing is done again */
static bool romm_saves_need_pairing_told = false;

/* Fetches what RomM has newer of the saves of @launch_path, the game
 * of playlist entry @content_path, played with @core_path, then loads
 * it. */
static int romm_load_with_saves(const char *core_path,
      const char *content_path, const char *launch_path, const char *label)
{
   core_info_t *info    = NULL;
   char core_folder[NAME_MAX_LENGTH];

   memset(&romm_played, 0, sizeof(romm_played));
   if (!romm_saves_allowed())
   {
      if (!romm_saves_need_pairing_told)
         romm_notify(msg_hash_to_str(MSG_ROMM_SAVES_NEED_PAIRING),
               MESSAGE_QUEUE_CATEGORY_WARNING);
      romm_saves_need_pairing_told = true;
      RARCH_WARN("[RomM] Saves of %s aren't synced: the pairing lacks "
            "the device id or the permissions saves need.\n",
            launch_path);
      return romm_load_content(core_path, launch_path, label);
   }
   if (     !(romm_played.rom_id = romm_launch_rom_id(content_path))
         || !core_info_find(core_path, &info)
         || string_is_empty(info->core_name)
         || !romm_core_folder_name(core_folder, sizeof(core_folder),
               core_path, info->core_name)
         || !romm_saves_dir(romm_played.save_dir,
               sizeof(romm_played.save_dir), launch_path, core_folder,
               false)
         || !romm_saves_dir(romm_played.state_dir,
               sizeof(romm_played.state_dir), launch_path, core_folder,
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
   romm_name_for_disk(romm_played.base, sizeof(romm_played.base));
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
   romm_fetching  = false;
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
      case ROMM_ERR_RENAMED:
         msg = MSG_ROMM_GAME_RENAMED;
         break;
      case ROMM_ERR_UNSUPPORTED:
         if (string_is_equal_noncase(
               path_get_extension(romm_launch_content), "rar"))
            msg = MSG_ROMM_RAR_UNSUPPORTED;
         else if (string_is_equal_noncase(
               path_get_extension(romm_launch_content), "7z"))
            msg = MSG_ROMM_7Z_DISC_UNSUPPORTED;
         else
            msg = MSG_ROMM_CANT_LAUNCH;
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
   core_info_t *info    = NULL;
   bool started;
   char title[NAME_MAX_LENGTH];
   char unpack_title[NAME_MAX_LENGTH];
   char launch_path[PATH_MAX_LENGTH];

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
   if (romm_launch_is_ready(content_path, launch_path, sizeof(launch_path)))
   {
      strlcpy(romm_launch_content, content_path,
            sizeof(romm_launch_content));
      return romm_load_with_saves(core_path, content_path, launch_path,
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
   if (core_info_find(core_path, &info))
      params.core_exts   = info->supported_extensions;
   started = romm_launch_prepare(&params, romm_on_launch_prepared, NULL);
   RBUF_FREE(firmware);
   if (!started)
   {
      romm_notify_failure(ROMM_ERR_UNREACHABLE, 0);
      return -1;
   }
   romm_launching = true;
   romm_fetching  = true;
   return 0;
}

#if defined(HAVE_MENU) && defined(HAVE_UPDATE_CORES)
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

/* A core to install, and the game to run once it's in ("" for none). */
typedef struct
{
   char core[64];
   char name[NAME_MAX_LENGTH];
   char system[NAME_MAX_LENGTH];
   char content[PATH_MAX_LENGTH];
   char label[NAME_MAX_LENGTH];
} romm_core_wanted_t;

/* The install running, if any, and the cores waiting for it (an RBUF,
 * oldest first). */
static romm_core_install_t *romm_core_installing = NULL;
static romm_core_wanted_t *romm_cores_wanted     = NULL;

static void romm_install_next_core(void);

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

   romm_core_installing = NULL;
   if (!ins->installed)
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_ROMM_CORE_INSTALL_FAILED), ins->name);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
      romm_install_next_core();
      return;
   }
   /* The downloader rescans the info files once its task is retired,
    * which can be after this. */
   if (!core_info_find(ins->core_path, &info))
   {
      bool refresh = true;
      command_event(CMD_EVENT_CORE_INFO_INIT, &refresh);
   }
   if (*ins->content)
      romm_launch_from_menu(ins->core_path, ins->content,
            *ins->label ? ins->label : NULL);
   else
   {
      snprintf(msg, sizeof(msg), msg_hash_to_str(MSG_ROMM_CORE_INSTALLED),
            ins->name);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_SUCCESS);
   }
   romm_install_next_core();
}

static void romm_core_install_cleanup(retro_task_t *task)
{
   free(task->state);
   task->state = NULL;
}

/* Installs @core, called @name, then runs @content_path, if any. */
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
   strlcpy(ins->content, content_path ? content_path : "",
         sizeof(ins->content));
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
   romm_core_installing = ins;
   RARCH_LOG("[RomM] Installing %s%s%s.\n", core,
         *ins->content ? " for " : "", ins->content);
   return true;
}

static void romm_install_next_core(void)
{
   char msg[NAME_MAX_LENGTH];
   size_t i;

   while (!romm_core_installing && RBUF_LEN(romm_cores_wanted))
   {
      romm_core_wanted_t want = romm_cores_wanted[0];
      for (i = 1; i < RBUF_LEN(romm_cores_wanted); i++)
         romm_cores_wanted[i - 1] = romm_cores_wanted[i];
      RBUF_RESIZE(romm_cores_wanted, RBUF_LEN(romm_cores_wanted) - 1);
      if (romm_install_core(want.core, want.name, want.content,
            want.label))
      {
         if (*want.system)
         {
            snprintf(msg, sizeof(msg),
                  msg_hash_to_str(MSG_ROMM_CORE_DOWNLOADING_FOR),
                  want.name, want.system);
            romm_notify(msg, MESSAGE_QUEUE_CATEGORY_INFO);
         }
      }
      else
      {
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_CORE_INSTALL_FAILED), want.name);
         romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
      }
   }
}

/* Asks for @core, called @name, for @system or to run @content_path:
 * installed next if nothing is, or after what's queued; a game waiting
 * goes first. Returns whether it is installing already. */
static bool romm_want_core(const char *core, const char *name,
      const char *system, const char *content_path, const char *label)
{
   romm_core_wanted_t want;
   size_t i;

   if (romm_core_installing && string_is_equal(romm_core_installing->core,
         core))
   {
      if (content_path && !*romm_core_installing->content)
      {
         strlcpy(romm_core_installing->content, content_path,
               sizeof(romm_core_installing->content));
         strlcpy(romm_core_installing->label, label ? label : "",
               sizeof(romm_core_installing->label));
      }
      return true;
   }
   for (i = 0; i < RBUF_LEN(romm_cores_wanted); i++)
      if (string_is_equal(romm_cores_wanted[i].core, core))
         break;
   if (i < RBUF_LEN(romm_cores_wanted))
   {
      if (!content_path)
         return false;
      want = romm_cores_wanted[i];
      for (; i > 0; i--)
         romm_cores_wanted[i] = romm_cores_wanted[i - 1];
   }
   else
   {
      memset(&want, 0, sizeof(want));
      strlcpy(want.core, core, sizeof(want.core));
      strlcpy(want.name, name, sizeof(want.name));
      RBUF_PUSH(romm_cores_wanted, want);
      if (content_path)
         for (i = RBUF_LEN(romm_cores_wanted) - 1; i > 0; i--)
            romm_cores_wanted[i] = romm_cores_wanted[i - 1];
      else
         i = RBUF_LEN(romm_cores_wanted) - 1;
   }
   if (system)
      strlcpy(want.system, system, sizeof(want.system));
   if (content_path)
   {
      strlcpy(want.content, content_path, sizeof(want.content));
      strlcpy(want.label, label ? label : "", sizeof(want.label));
   }
   romm_cores_wanted[i] = want;
   romm_install_next_core();
   return false;
}

/* @all: every emulator of each system that isn't installed, as Refetch
 * asks (2026-10-22), rather than the first when none is. */
static void romm_want_missing_cores(const romm_library_t *lib,
      const char *only, bool all)
{
   settings_t *settings = config_get_ptr();
   size_t i;

   if (     !settings->bools.menu_show_core_updater
         || settings->bools.kiosk_mode_enable)
      return;
   /* Each system it wrote with none of its emulators installed gets
    * its first (decided 2026-10-21). */
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
   {
      const romm_platform_t *p = &lib->platforms[i].platform;
      const char *const *cores =
            romm_library_preferred_cores(p->libretro_slug);
      char path[PATH_MAX_LENGTH];
      char name[NAME_MAX_LENGTH];
      size_t k;

      if (!cores || (only && !romm_library_listed(only, p->fs_slug)))
         continue;
      for (k = 0; cores[k]; k++)
      {
         const char *one[2];
         one[0] = cores[k];
         one[1] = NULL;
         if (romm_find_core(all ? one : cores, path, sizeof(path), name,
               sizeof(name)))
         {
            if (all)
               continue;
            break;
         }
         romm_core_name(cores[k], name, sizeof(name));
         romm_want_core(cores[k], name, p->name, NULL, NULL);
         if (!all)
            break;
      }
   }
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
      char msg[NAME_MAX_LENGTH];

      if (     !settings->bools.menu_show_core_updater
            || settings->bools.kiosk_mode_enable)
         return false;
      romm_core_name(cores[0], name, sizeof(name));
      /* The platform's first emulator is fetched, and the game runs
       * once it's in. */
      snprintf(msg, sizeof(msg), msg_hash_to_str(
            romm_want_core(cores[0], name, NULL, content_path, label)
            ? MSG_ROMM_CORE_INSTALLING : MSG_ROMM_CORE_DOWNLOADING), name);
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
   if (romm_fetching)
      return;
   /* Content loaded or closed meanwhile drops a launch waiting on the
    * choice of save; its next Run asks again. */
   if (romm_conflict_pending)
   {
      RARCH_LOG("[RomM] Other content loaded or closed: %s no longer "
            "waits on the choice of save.\n", romm_conflict_content);
      romm_conflict_pending = false;
      romm_conflict_gen++;
   }
   /* A game in memory whose saves are syncing before it loads stays. */
   if (romm_launching)
   {
      RARCH_LOG("[RomM] Content closed while %s's saves sync; only it "
            "stays in memory.\n", romm_launch_content);
      romm_launch_release(romm_launch_content);
      return;
   }
   romm_launch_release(next_content);
}

#ifdef HAVE_MENU
/* Unpair This Device asks for a second OK: the first arms it, which the
 * rebuild of Settings > RomM it causes keeps. */
static bool romm_unpair_armed  = false;
static bool romm_unpair_arming = false;

unsigned romm_menu_unpair_row(file_list_t *list)
{
   settings_t *settings = config_get_ptr();

   if (!romm_unpair_arming)
      romm_unpair_armed = false;
   romm_unpair_arming = false;
   if (     string_is_empty(settings->arrays.romm_token)
         || !menu_entries_append(list, "", "", MENU_ENUM_LABEL_ROMM_UNPAIR,
               MENU_ROMM_SYSTEM, 0, 0, NULL))
      return 0;
   file_list_set_alt_at_offset(list, list->size - 1, msg_hash_to_str(
         romm_unpair_armed ? MSG_ROMM_UNPAIR_CONFIRM : MSG_ROMM_UNPAIR));
   return 1;
}

/* Whatever uses the pairing, or would write the library or the
 * configuration after it is gone. */
static bool romm_unpair_busy(void)
{
   return romm_syncing || romm_checking || romm_launching || romm_fetching
      || romm_running.rom_id || romm_closing.rom_id || romm_conflict_pending
      || RBUF_LEN(romm_owed) || romm_favorites_creating
      || romm_overrides_active()
#ifdef HAVE_CLOUDSYNC
      || task_cloud_sync_in_progress()
#endif
      ;
}

/* RomM's games leave @playlist, History or Favorites. */
static void romm_drop_games(playlist_t *playlist)
{
   bool changed = false;
   size_t i     = playlist_size(playlist);

   while (i--)
   {
      const struct playlist_entry *entry = NULL;
      playlist_get_index(playlist, i, &entry);
      if (entry && entry->path && string_starts_with(entry->path, ROMM_SCHEME))
      {
         playlist_delete_index(playlist, i);
         changed = true;
      }
   }
   if (changed)
   {
      playlist_write_file(playlist);
      romm_playlist_changed(playlist_get_conf_path(playlist));
   }
}

/* Leaves this device as a fresh install would be: not paired, Cloud
 * Sync off if it pointed at RomM, and no RomM library. Save and state
 * files, BIOS and cores stay. */
static void romm_unpair_here(void)
{
   settings_t *settings = config_get_ptr();
   char record[PATH_MAX_LENGTH];

   *settings->arrays.romm_token             = '\0';
   *settings->arrays.romm_device_id         = '\0';
   *settings->arrays.romm_device_identifier = '\0';
   *settings->arrays.romm_missing_scopes    = '\0';
   *settings->arrays.romm_username          = '\0';
   *settings->arrays.romm_sync_exclude      = '\0';
   *settings->arrays.romm_sync_new          = '\0';
   romm_username_asked = false;
#ifdef HAVE_CLOUDSYNC
   if (romm_cloud_sync_is_romms(settings))
      configuration_set_bool(settings,
            settings->bools.cloud_sync_enable, false);
#endif

   romm_saves_record_path(record, sizeof(record),
         dir_get_ptr(RARCH_DIR_SAVEFILE));
   if (path_is_valid(record))
      filestream_delete(record);
   romm_library_remove_all(settings->paths.directory_playlist,
         settings->paths.directory_thumbnails, romm_playlist_changed);
   if (g_defaults.content_history)
      romm_drop_games(g_defaults.content_history);
   if (g_defaults.content_favorites)
      romm_drop_games(g_defaults.content_favorites);

   romm_library_synced_at  = 0;
   romm_library_games      = 0;
   romm_library_systems    = 0;
   *romm_library_waiting   = '\0';
   *romm_library_new       = '\0';
   RBUF_FREE(romm_library_waiting_list);
   RBUF_FREE(romm_favorites_waiting);
   romm_saves_need_pairing_told = false;

   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
   romm_reload_playlists();
   RARCH_LOG("[RomM] Unpaired: the pairing and RomM's library are gone "
         "from this device.\n");
}

static void romm_on_device_deleted(void *user_data, enum romm_status status,
      int http_status)
{
   char msg[256];

   romm_unpairing = false;
   if (status == ROMM_ERR_UNREACHABLE)
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_UNPAIR_UNREACHABLE),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   romm_unpair_here();
   /* 404: RomM no longer had the device. */
   if (status == ROMM_OK || http_status == 404)
      romm_notify(msg_hash_to_str(MSG_ROMM_UNPAIRED),
            MESSAGE_QUEUE_CATEGORY_SUCCESS);
   else
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_ROMM_UNPAIRED_DEVICE_KEPT), http_status);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_WARNING);
   }
   romm_menu_refresh();
}

void romm_menu_unpair(void)
{
   settings_t *settings = config_get_ptr();

   if (romm_unpairing || string_is_empty(settings->arrays.romm_token))
      return;
   if (!romm_unpair_armed)
   {
      romm_unpair_armed  = true;
      romm_unpair_arming = true;
      romm_menu_refresh();
      return;
   }
   romm_menu_refresh();
   if (romm_unpair_busy())
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_UNPAIR_BUSY),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   romm_pair_stop();
   /* Paired before RomM's device id was kept: none to delete. */
   if (string_is_empty(settings->arrays.romm_device_id))
   {
      romm_on_device_deleted(NULL, ROMM_OK, 204);
      return;
   }
   if (!romm_client_device_delete(settings->arrays.romm_server_url,
         settings->arrays.romm_token, settings->arrays.romm_device_id,
         romm_on_device_deleted, NULL))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_UNPAIR_UNREACHABLE),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }
   romm_unpairing = true;
}
#endif
