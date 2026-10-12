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

#ifndef __ROMM_LIBRARY_H
#define __ROMM_LIBRARY_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "romm_client.h"

RETRO_BEGIN_DECLS

#define ROMM_SCHEME "romm://"

/* A platform as the last sync left it. */
typedef struct
{
   romm_platform_t platform;
   /* Its playlist's file name in the playlist folder. */
   char playlist[256];
   /* Its games in RomM's order (an RBUF). */
   romm_rom_t *roms;
} romm_library_platform_t;

/* What the last sync wrote. Kept with the playlists so the next sync
 * knows which playlists are RomM's and what has changed. */
typedef struct
{
   char server[256];
   /* When that sync finished, in seconds since the epoch. */
   int64_t synced_at;
   romm_library_platform_t *platforms; /* an RBUF */
} romm_library_t;

/* The playlist path of @rom: romm://<id>/<RomM file name>. A folder
 * game launches through a generated playlist of its discs, so its path
 * is romm://<id>/<folder name>.m3u. Either way the last part is what
 * RetroArch names saves after, which is what RomM matches them by. */
size_t romm_library_content_path(char *s, size_t len,
      const romm_rom_t *rom);

/* The playlist file name for @platform: its libretro database name, so
 * RetroArch's system icons apply, or "RomM - <name>" without one. */
size_t romm_library_playlist_name(char *s, size_t len,
      const romm_platform_t *platform);

/* Writes @roms, in order, as the playlist at @path, replacing it. Each
 * entry names @db_name (the playlist's file name) as its database, so
 * covers are looked up under thumbnails/<db_name without .lpl>. */
bool romm_library_write_playlist(const char *path, const char *db_name,
      const char *default_core_path, const char *default_core_name,
      const romm_rom_t *roms, size_t count);

/* Finds the first of @cores (core file names without "_libretro" and
 * extension) that is installed, filling its path and display name. */
typedef bool (*romm_library_find_core_t)(const char *const *cores,
      char *path, size_t path_len, char *name, size_t name_len);

typedef struct
{
   const char *server;
   const char *token;
   const char *playlist_dir;
   /* NULL leaves covers alone. */
   const char *thumbnails_dir;
   /* Shown by the sync and covers tasks; NULL runs them silently. */
   const char *title;
   const char *covers_title;
   /* Gives a playlist without a default core one; NULL leaves it to
    * RetroArch. */
   romm_library_find_core_t find_core;
} romm_library_sync_params_t;

/* @lib is the library as the sync left it, valid only during the
 * call. @http_status is that of a failed request, or 0. */
typedef void (*romm_library_sync_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_library_t *lib);

/* Where RetroArch looks for @rom's box art when it is listed in
 * @playlist: thumbnails/<playlist name>/Named_Boxarts/<file name>.png,
 * named after the playlist path's file name as the menu names it. A
 * JPEG cover is saved as .jpg in its place, which the menu finds when
 * it allows image types other than PNG. */
size_t romm_library_cover_path(char *s, size_t len,
      const char *thumbnails_dir, const char *playlist,
      const romm_rom_t *rom);

/* The emulators that load @libretro_slug's games from memory, best
 * first, as core file names without "_libretro" and extension; NULL
 * when there are none for the system. */
const char *const *romm_library_preferred_cores(const char *libretro_slug);

/* <@playlist_dir>/romm/library.json */
size_t romm_library_manifest_path(char *s, size_t len,
      const char *playlist_dir);

/* Reads the manifest at @path into @lib, which starts empty. False,
 * with @lib empty, when there is none or it can't be read. */
bool romm_library_load(romm_library_t *lib, const char *path);

/* Writes @lib to @path, replacing it, creating its folder if needed. */
bool romm_library_save(const romm_library_t *lib, const char *path);

void romm_library_free(romm_library_t *lib);

/* Brings the playlists in the playlist folder in line with the server:
 * one per RomM platform with games the token's user can see, and the
 * manifest recording them. Runs as a task showing its progress; @cb
 * runs on the main thread when it ends. Then, given a thumbnails
 * folder, a second task fetches the box art that is new or changed;
 * covers of games gone are deleted. */
bool romm_library_sync(const romm_library_sync_params_t *params,
      romm_library_sync_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
