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
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>

#include "romm.h"
#include "romm_client.h"
#include "romm_platform.h"
#include "../../command.h"
#include "../../configuration.h"
#include "../../msg_hash.h"
#include "../../runloop.h"
#include "../../version.h"

/* Set from the moment pairing starts until its wait ends. */
static bool romm_pairing = false;

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
      default:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_NOT_ROMM), server);
         break;
   }

   romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
}

static void romm_check_connection_cb(void *user_data,
      enum romm_status status, int http_status, const char *version)
{
   char msg[512];

   if (status != ROMM_OK)
   {
      romm_notify_failure(status, http_status);
      return;
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
