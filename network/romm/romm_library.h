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
#include <boolean.h>
#include <retro_common_api.h>

#include "romm_client.h"

RETRO_BEGIN_DECLS

#define ROMM_SCHEME "romm://"

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

RETRO_END_DECLS

#endif
