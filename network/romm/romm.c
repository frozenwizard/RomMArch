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

#include <string/stdstring.h>

#include "romm.h"
#include "romm_client.h"
#include "../../configuration.h"
#include "../../msg_hash.h"
#include "../../runloop.h"

static void romm_notify(const char *msg,
      enum message_queue_category category)
{
   runloop_msg_queue_push(msg, strlen(msg), 1, 180, true, NULL,
         MESSAGE_QUEUE_ICON_DEFAULT, category);
}

static void romm_check_connection_cb(void *user_data,
      enum romm_status status, int http_status, const char *version)
{
   settings_t *settings = config_get_ptr();
   const char *server   = settings->arrays.romm_server_url;
   char msg[512];

   switch (status)
   {
      case ROMM_OK:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_CONNECTED), version);
         break;
      case ROMM_ERR_UNREACHABLE:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_UNREACHABLE), server);
         break;
      case ROMM_ERR_HTTP:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_HTTP_ERROR), http_status);
         break;
      default:
         snprintf(msg, sizeof(msg),
               msg_hash_to_str(MSG_ROMM_NOT_ROMM), server);
         break;
   }

   romm_notify(msg, status == ROMM_OK
         ? MESSAGE_QUEUE_CATEGORY_SUCCESS
         : MESSAGE_QUEUE_CATEGORY_ERROR);
}

void romm_check_connection(void)
{
   settings_t *settings = config_get_ptr();
   const char *server   = settings->arrays.romm_server_url;
   char msg[512];

   if (string_is_empty(server))
   {
      romm_notify(msg_hash_to_str(MSG_ROMM_NO_SERVER),
            MESSAGE_QUEUE_CATEGORY_WARNING);
      return;
   }

   if (!romm_client_heartbeat(server, romm_check_connection_cb, NULL))
   {
      snprintf(msg, sizeof(msg),
            msg_hash_to_str(MSG_ROMM_UNREACHABLE), server);
      romm_notify(msg, MESSAGE_QUEUE_CATEGORY_ERROR);
   }
}
