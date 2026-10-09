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

/* @lib is the library as the sync left it, valid only during the
 * call. */
typedef void (*romm_library_sync_cb_t)(void *user_data,
      enum romm_status status, const romm_library_t *lib);

/* <@playlist_dir>/romm/library.json */
size_t romm_library_manifest_path(char *s, size_t len,
      const char *playlist_dir);

/* Reads the manifest at @path into @lib, which starts empty. False,
 * with @lib empty, when there is none or it can't be read. */
bool romm_library_load(romm_library_t *lib, const char *path);

/* Writes @lib to @path, replacing it, creating its folder if needed. */
bool romm_library_save(const romm_library_t *lib, const char *path);

void romm_library_free(romm_library_t *lib);

/* Brings the playlists in @playlist_dir in line with @server: one per
 * RomM platform with games the token's user can see, and the manifest
 * recording them. Runs as a task showing @title and its progress; @cb
 * runs on the main thread when it ends. */
bool romm_library_sync(const char *server, const char *token,
      const char *playlist_dir, const char *title,
      romm_library_sync_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
