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

#ifndef __ROMM_LAUNCH_H
#define __ROMM_LAUNCH_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "romm_client.h"

RETRO_BEGIN_DECLS

typedef struct
{
   const char *server;
   const char *token;
   /* The game's playlist path, romm://<id>/<name> */
   const char *content_path;
   /* Shown while downloading, then while unpacking a zipped disc; no
    * @title mutes the task. */
   const char *title;
   const char *unpack_title;
   /* BIOS files the core wants that @system_dir lacks, as paths in it
    * such as "dc/dc_boot.bin": those RomM has for the game's platform
    * are fetched there first. */
   const char *system_dir;
   const char *const *firmware;
   size_t firmware_count;
} romm_launch_params_t;

/* @http_status is that of a failed request, or 0. On success
 * @launch_path is what to load: the playlist path, or for a zipped
 * disc image, the disc or disc list unpacked from it. */
typedef void (*romm_launch_cb_t)(void *user_data,
      enum romm_status status, int http_status, const char *launch_path);

/* The RomM game id in a romm://<id>/... path; 0 if it has none. */
unsigned romm_launch_rom_id(const char *content_path);

/* Has @content_path's game been fetched into memory already? If so,
 * @launch_path gets what to load, as the fetch gave it. */
bool romm_launch_is_ready(const char *content_path, char *launch_path,
      size_t len);

/* Frees the memory of every fetched game but @keep_path's; of them all
 * when @keep_path is NULL or not a RomM game. */
void romm_launch_release(const char *keep_path);

/* Fetches the game behind playlist path @content_path from RomM into
 * memory, as romm://<id>/<path inside the game> files, which the
 * loader and cores read through the VFS. A folder game also gets the
 * disc list @content_path names (romm://<id>/<folder>.m3u). A zip
 * holding a disc image is unpacked into its members. The game fetched
 * before is released first. Runs as a task showing its progress; @cb
 * runs on the main thread when it ends. */
bool romm_launch_prepare(const romm_launch_params_t *params,
      romm_launch_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
