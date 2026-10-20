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
#include <stdint.h>
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
   /* RomM turned the token down: revoked or expired. */
   ROMM_ERR_UNAUTHORIZED,
   /* RomM took the token but refused the request: a scope the approver
    * left out. */
   ROMM_ERR_FORBIDDEN,
   /* Pairing: the user turned the device down. */
   ROMM_ERR_DENIED,
   /* Pairing: the code ran out before the user approved it. */
   ROMM_ERR_EXPIRED,
   ROMM_ERR_CANCELLED,
   /* A file couldn't be written, such as a playlist. */
   ROMM_ERR_WRITE,
   /* Launch: nothing in the game can be loaded, such as a RAR. */
   ROMM_ERR_UNSUPPORTED,
   /* Launch: a download doesn't match RomM's size or CRC32. */
   ROMM_ERR_CHECKSUM,
   /* Launch: no memory to hold the game. */
   ROMM_ERR_MEMORY,
   /* Save sync: RomM's copy changed since this device last synced it. */
   ROMM_ERR_CONFLICT,
   /* Launch: the entry names a file RomM has renamed since the last
    * library sync. */
   ROMM_ERR_RENAMED
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
   /* Its maker, such as "Nintendo"; empty when RomM knows none. */
   char family_name[64];
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

/* One of a game's files in RomM. */
typedef struct
{
   unsigned id;
   uint64_t size;
   /* CRC32 in hex; empty when RomM has none. */
   char crc[9];
   /* The name RomM serves it under. */
   char name[256];
   /* Where it sits in the game's folder, such as "Disc 1/track01.bin":
    * what cue sheets and disc lists inside the game refer to it by.
    * Just its name for a game that is one file. */
   char path[512];
} romm_file_t;

/* A BIOS or other firmware file RomM holds for a platform. */
typedef struct
{
   unsigned id;
   uint64_t size;
   /* RomM matched it to a known good dump. */
   bool verified;
   /* CRC32 in hex; empty when RomM has none. */
   char crc[9];
   char name[256];
} romm_firmware_t;

/* A save or save state the user has in RomM for a game. */
typedef struct
{
   unsigned id;
   unsigned rom_id;
   uint64_t size;
   /* The name RomM keeps it under. A save uploaded to a slot has the
    * upload time in it, such as "Game [2026-10-12_20-15-00].srm". */
   char file_name[456];
   /* The emulator its client said made it, such as "Snes9x"; empty
    * when none. */
   char emulator[64];
   /* Saves only: the slot it went to, empty when none. */
   char slot[256];
   /* Saves only: MD5 of its contents in hex, empty when unknown. */
   char content_hash[33];
   /* When RomM last changed it, such as "2026-10-12T20:15:00+00:00". */
   char updated_at[40];
} romm_save_t;

/* A save or save state to upload for game @rom_id. */
typedef struct
{
   const char *server;
   const char *token;
   unsigned rom_id;
   /* What RomM keeps it under, such as "Super Metroid (USA).srm". */
   const char *file_name;
   const void *data;
   size_t len;
   /* The emulator that made it, such as "Snes9x"; NULL for none. */
   const char *emulator;
   /* Saves only, each NULL or 0 to leave out. In @slot, each upload
    * is a new version, RomM keeping the latest @keep; with no slot it
    * replaces the game's save of that name. With @device_id, RomM's id
    * for this device, RomM refuses it when the slot or save changed
    * since this device last synced it, unless @overwrite.
    * @content_hash, the MD5 of @data in hex, is recorded as what the
    * device holds. */
   const char *slot;
   unsigned keep;
   const char *device_id;
   const char *content_hash;
   bool overwrite;
} romm_upload_t;

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

/* @token and @device_id are empty unless @status is ROMM_OK.
 * @http_status is that of the last poll RomM answered, 0 when none.
 * @missing_scopes names, comma separated, the scopes pairing asked
 * for that the approver left out; empty when there are none. */
typedef void (*romm_pair_wait_cb_t)(void *user_data,
      enum romm_status status, int http_status, const char *token,
      const char *device_id, const char *missing_scopes);

/* @platforms holds @count entries, valid only during the call; NULL
 * unless @status is ROMM_OK. */
typedef void (*romm_platforms_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_platform_t *platforms, size_t count);

/* A collection in RomM: the user's, another user's public one, or a
 * smart one. */
typedef struct
{
   unsigned id;
   char name[128];
   /* The RomM user who made it. */
   char owner[64];
   /* Its games' ids, an RBUF. */
   unsigned *rom_ids;
   bool smart;
   /* Someone's favorites. */
   bool favorite;
   bool is_public;
} romm_collection_t;

/* @collections holds @count entries, valid only during the call; NULL
 * unless @status is ROMM_OK. */
typedef void (*romm_collections_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_collection_t *collections, size_t count);

/* @roms holds @count entries, valid only during the call; NULL unless
 * @status is ROMM_OK. @more is set when RomM's page was full, counting
 * games left out of @roms, so another may follow. */
typedef void (*romm_roms_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_rom_t *roms, size_t count, bool more);

/* @ids holds @count game ids, valid only during the call; NULL unless
 * @status is ROMM_OK. */
typedef void (*romm_rom_ids_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const unsigned *ids, size_t count);

/* @rom and its @count @files are valid only during the call; NULL
 * unless @status is ROMM_OK. */
typedef void (*romm_rom_files_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_rom_t *rom, const romm_file_t *files, size_t count);

/* @data holds @len bytes, from malloc and now the callback's to free;
 * NULL unless @status is ROMM_OK, and possibly NULL for an empty
 * file. */
typedef void (*romm_content_cb_t)(void *user_data,
      enum romm_status status, int http_status, void *data, size_t len);

/* @firmware holds @count entries, valid only during the call; NULL
 * unless @status is ROMM_OK. */
typedef void (*romm_firmware_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_firmware_t *firmware, size_t count);

/* @saves holds @count entries, valid only during the call; NULL
 * unless @status is ROMM_OK. */
typedef void (*romm_saves_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_save_t *saves, size_t count);

/* @save is the upload as RomM keeps it, valid only during the call;
 * NULL unless @status is ROMM_OK. ROMM_ERR_CONFLICT when RomM refused
 * it for having changed since this device's last sync. */
typedef void (*romm_save_cb_t)(void *user_data,
      enum romm_status status, int http_status, const romm_save_t *save);

/* @data holds @len bytes, valid only during the call; NULL unless
 * @status is ROMM_OK. */
typedef void (*romm_download_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const void *data, size_t len);

/* @found is false when RomM lists no token for the device; else
 * @missing_scopes names, comma separated, the scopes pairing asks for
 * that the device's newest token lacks, empty when none. */
typedef void (*romm_token_scopes_cb_t)(void *user_data,
      enum romm_status status, int http_status, bool found,
      const char *missing_scopes);

/* A request that brings nothing back. */
typedef void (*romm_done_cb_t)(void *user_data,
      enum romm_status status, int http_status);

/* @id is the new collection's, 0 unless @status is ROMM_OK. */
typedef void (*romm_collection_created_cb_t)(void *user_data,
      enum romm_status status, int http_status, unsigned id);

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
 * NULL waits silently. Returns the wait, or NULL. */
void *romm_client_pair_wait(const char *server,
      const romm_pairing_t *pairing, const char *title,
      romm_pair_wait_cb_t cb, void *user_data);

/* Ends @wait, whose callback hasn't run yet, as soon as a poll still
 * out has answered; the callback gets ROMM_ERR_CANCELLED. Main thread
 * only. */
void romm_client_pair_cancel(void *wait);

/* Lists the platforms @token's user can see on @server, in RomM's
 * order; @cb runs on the main thread. */
bool romm_client_platforms(const char *server, const char *token,
      romm_platforms_cb_t cb, void *user_data);

/* Lists the collections @token's user can see on @server, their own
 * and other users' public ones, or with @smart their smart
 * collections; @cb runs on the main thread. */
bool romm_client_collections(const char *server, const char *token,
      bool smart, romm_collections_cb_t cb, void *user_data);

/* Fetches up to @limit of the games on platform @platform_id, from
 * @offset in RomM's title order, leaving out games whose files are
 * missing. With @updated_after (UTC, such as "2026-10-03T00:23:20Z"),
 * only games changed since then. @cb runs on the main thread. */
bool romm_client_roms(const char *server, const char *token,
      unsigned platform_id, const char *updated_after,
      unsigned offset, unsigned limit,
      romm_roms_cb_t cb, void *user_data);

/* Lists the ids of every game romm_client_roms() would return for
 * @platform_id, in the same order, in one request. @cb runs on the
 * main thread. */
bool romm_client_rom_ids(const char *server, const char *token,
      unsigned platform_id, romm_rom_ids_cb_t cb, void *user_data);

/* Fetches game @rom_id with its files; a game RomM deleted, or that
 * the user may no longer see, answers HTTP 404. @cb runs on the main
 * thread. */
bool romm_client_rom_files(const char *server, const char *token,
      unsigned rom_id, romm_rom_files_cb_t cb, void *user_data);

/* Downloads @length bytes from @offset of file @file_id, named
 * @file_name, of game @rom_id into memory; RomM records it as played
 * rather than downloaded. A @length of 0 asks for the whole file.
 * RetroArch's HTTP client refuses replies over 256 MB, so larger files
 * are fetched a range at a time. Returns the transfer's task, whose
 * progress shows how far it has got and which can be cancelled, or
 * NULL. @cb runs on the main thread. */
void *romm_client_rom_content(const char *server, const char *token,
      unsigned rom_id, unsigned file_id, const char *file_name,
      uint64_t offset, uint64_t length,
      romm_content_cb_t cb, void *user_data);

/* Fetches the file at @path on @server, such as a cover, into memory.
 * @path goes into the URL as is. @cb runs on the main thread. */
bool romm_client_download(const char *server, const char *token,
      const char *path, romm_download_cb_t cb, void *user_data);

/* Lists the firmware RomM has for platform @platform_id. @cb runs on
 * the main thread. */
bool romm_client_firmware(const char *server, const char *token,
      unsigned platform_id, romm_firmware_cb_t cb, void *user_data);

/* Fetches firmware @firmware_id, named @file_name, into memory. @cb
 * runs on the main thread. */
bool romm_client_firmware_content(const char *server, const char *token,
      unsigned firmware_id, const char *file_name,
      romm_download_cb_t cb, void *user_data);

/* Looks up in RomM's list of the user's tokens (which needs me.read)
 * the newest one bound to @device_id, as pairing again leaves the
 * older ones there, and what it lacks of the scopes pairing asks for.
 * @cb runs on the main thread. */
bool romm_client_token_scopes(const char *server, const char *token,
      const char *device_id, romm_token_scopes_cb_t cb, void *user_data);

/* Lists the saves the user has in RomM for game @rom_id, from every
 * client and slot. @cb runs on the main thread. */
bool romm_client_saves(const char *server, const char *token,
      unsigned rom_id, romm_saves_cb_t cb, void *user_data);

/* Lists the save states the user has in RomM for game @rom_id. @cb
 * runs on the main thread. */
bool romm_client_states(const char *server, const char *token,
      unsigned rom_id, romm_saves_cb_t cb, void *user_data);

/* Fetches save @save_id into memory. With @device_id, RomM's id for
 * this device, RomM records that the device now has this version.
 * @cb runs on the main thread. */
bool romm_client_save_content(const char *server, const char *token,
      unsigned save_id, const char *device_id,
      romm_download_cb_t cb, void *user_data);

/* Tells RomM that this device, @device_id, holds save @save_id, whose
 * MD5 is @content_hash (or NULL), as if it had fetched it: RomM
 * refuses the device's next upload to the save's slot otherwise. @cb
 * runs on the main thread. */
bool romm_client_save_downloaded(const char *server, const char *token,
      unsigned save_id, const char *device_id, const char *content_hash,
      romm_save_cb_t cb, void *user_data);

/* Fetches save state @state_id into memory. @cb runs on the main
 * thread. */
bool romm_client_state_content(const char *server, const char *token,
      unsigned state_id, romm_download_cb_t cb, void *user_data);

/* Deletes the @count save states in @ids from RomM. @cb runs on the
 * main thread. */
bool romm_client_states_delete(const char *server, const char *token,
      const unsigned *ids, size_t count, romm_done_cb_t cb,
      void *user_data);

/* Adds the @count games in @ids to the user's collection
 * @collection_id, or with @remove takes them out. @cb runs on the main
 * thread. */
bool romm_client_collection_roms(const char *server, const char *token,
      unsigned collection_id, const unsigned *ids, size_t count,
      bool remove, romm_done_cb_t cb, void *user_data);

/* Creates the user's favorites collection, named as RomM's web UI
 * names it. @cb runs on the main thread. */
bool romm_client_create_favorites(const char *server, const char *token,
      romm_collection_created_cb_t cb, void *user_data);

/* Uploads @up as a save. @up's strings and data are copied. @cb runs
 * on the main thread. */
bool romm_client_save_upload(const romm_upload_t *up,
      romm_save_cb_t cb, void *user_data);

/* Uploads @up as a save state; RomM replaces the game's state of that
 * name. @cb runs on the main thread. */
bool romm_client_state_upload(const romm_upload_t *up,
      romm_save_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
