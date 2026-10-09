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

#ifndef __ROMM_CLIENT_H
#define __ROMM_CLIENT_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

enum romm_status
{
   ROMM_OK = 0,
   /* No HTTP response: wrong address, server down or no network. */
   ROMM_ERR_UNREACHABLE,
   /* An HTTP status the request doesn't expect. */
   ROMM_ERR_HTTP,
   /* A body RomM wouldn't send, such as another web server's page. */
   ROMM_ERR_RESPONSE,
   /* RomM turned the token down: revoked, expired or missing scopes. */
   ROMM_ERR_UNAUTHORIZED,
   /* Pairing: the user turned the device down. */
   ROMM_ERR_DENIED,
   /* Pairing: the code ran out before the user approved it. */
   ROMM_ERR_EXPIRED,
   ROMM_ERR_CANCELLED,
   /* A file couldn't be written, such as a playlist. */
   ROMM_ERR_WRITE
};

/* A pairing RomM has started: the user approves it at @approve_url
 * (or enters @user_code there) while the device polls with
 * @device_code. Times are in seconds. */
typedef struct
{
   char device_code[160];
   char user_code[32];
   char approve_url[1024];
   unsigned interval;
   unsigned expires_in;
} romm_pairing_t;

/* A platform in the user's RomM library. */
typedef struct
{
   unsigned id;
   /* Games RomM has on the platform, before the user's filters. */
   unsigned rom_count;
   /* RomM's short name, such as "snes". */
   char slug[64];
   /* What RomM shows, the user's custom name if they set one. */
   char name[128];
   /* The libretro database name, such as "Nintendo - Super Nintendo
    * Entertainment System"; empty when RomM knows none. */
   char libretro_slug[128];
} romm_platform_t;

/* A game in the user's RomM library. */
typedef struct
{
   unsigned id;
   unsigned platform_id;
   /* The game is one file rather than a folder. */
   bool single_file;
   /* That file's CRC32 in hex; empty when RomM has none. */
   char crc[9];
   /* The file or folder name in RomM's library, and that name without
    * its extension, which RomM matches synced saves against. */
   char fs_name[256];
   char fs_name_no_ext[256];
   /* RomM's title, or fs_name_no_ext when it has none. */
   char name[256];
   /* Path of the large cover on the server; empty when there is none. */
   char cover[192];
   char updated_at[32];
} romm_rom_t;

/* @version is RomM's version string, empty unless @status is ROMM_OK.
 * @http_status is 0 when there was no response. */
typedef void (*romm_heartbeat_cb_t)(void *user_data,
      enum romm_status status, int http_status, const char *version);

/* @pairing is NULL unless @status is ROMM_OK. */
typedef void (*romm_pair_start_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_pairing_t *pairing);

/* @username is empty unless @status is ROMM_OK. */
typedef void (*romm_whoami_cb_t)(void *user_data,
      enum romm_status status, int http_status, const char *username);

/* @token and @device_id are empty unless @status is ROMM_OK. */
typedef void (*romm_pair_wait_cb_t)(void *user_data,
      enum romm_status status, const char *token, const char *device_id);

/* @platforms holds @count entries, valid only during the call; NULL
 * unless @status is ROMM_OK. */
typedef void (*romm_platforms_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_platform_t *platforms, size_t count);

/* @roms holds @count entries, valid only during the call; NULL unless
 * @status is ROMM_OK. */
typedef void (*romm_roms_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_rom_t *roms, size_t count);

/* Joins @server (with or without a trailing slash) and @path. */
bool romm_client_url(char *s, size_t len, const char *server,
      const char *path);

/* Checks @server is a reachable RomM; @cb runs on the main thread. */
bool romm_client_heartbeat(const char *server,
      romm_heartbeat_cb_t cb, void *user_data);

/* Asks @server which user @token acts for; @cb runs on the main
 * thread. */
bool romm_client_whoami(const char *server, const char *token,
      romm_whoami_cb_t cb, void *user_data);

/* Asks @server to pair this device. @device_id must stay the same
 * across pairings so RomM updates its device entry instead of adding
 * one; @device_name is what RomM lists it as. The approval link uses
 * @web, the address the user signs in to RomM at, or @server when
 * @web is empty. @cb runs on the main thread. */
bool romm_client_pair_start(const char *server, const char *web,
      const char *device_id, const char *device_name,
      const char *client_version,
      romm_pair_start_cb_t cb, void *user_data);

/* Polls @server until the user approves or denies @pairing, or it
 * expires; @cb runs on the main thread with the device's token. A
 * @title is shown in the task notification for as long as it waits;
 * NULL waits silently. */
bool romm_client_pair_wait(const char *server,
      const romm_pairing_t *pairing, const char *title,
      romm_pair_wait_cb_t cb, void *user_data);

/* Lists the platforms @token's user can see on @server, in RomM's
 * order; @cb runs on the main thread. */
bool romm_client_platforms(const char *server, const char *token,
      romm_platforms_cb_t cb, void *user_data);

/* Fetches up to @limit of the games on platform @platform_id, from
 * @offset in RomM's title order, leaving out games whose files are
 * missing. With @updated_after (UTC, such as "2026-10-03T00:23:20Z"),
 * only games changed since then. Fewer than @limit means the last
 * page. @cb runs on the main thread. */
bool romm_client_roms(const char *server, const char *token,
      unsigned platform_id, const char *updated_after,
      unsigned offset, unsigned limit,
      romm_roms_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
