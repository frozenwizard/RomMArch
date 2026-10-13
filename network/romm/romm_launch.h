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

/* @http_status is that of a failed request, or 0. */
typedef void (*romm_launch_cb_t)(void *user_data,
      enum romm_status status, int http_status);

/* The RomM game id in a romm://<id>/... path; 0 if it has none. */
unsigned romm_launch_rom_id(const char *content_path);

/* Has @content_path's game been fetched into memory already? */
bool romm_launch_is_ready(const char *content_path);

/* Frees the memory of every fetched game but @keep_path's; of them all
 * when @keep_path is NULL or not a RomM game. */
void romm_launch_release(const char *keep_path);

/* Fetches the game behind playlist path @content_path from RomM into
 * memory, as romm://<id>/<path inside the game> files, which the
 * loader and cores read through the VFS. A folder game also gets the
 * disc list @content_path names (romm://<id>/<folder>.m3u). The game
 * fetched before is released first. Runs as a task showing @title and
 * its progress; @cb runs on the main thread when it ends. */
bool romm_launch_prepare(const char *server, const char *token,
      const char *content_path, const char *title,
      romm_launch_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
