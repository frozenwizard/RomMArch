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

#ifndef __ROMM_H
#define __ROMM_H

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Asks the configured RomM server for its version and reports the
 * outcome as a notification. */
void romm_check_connection(void);

/* Pairs this install with the configured RomM server: shows the code,
 * opens the approval page in the browser and, once the user approves,
 * keeps the device's token and saves the configuration. */
void romm_pair(void);

/* Syncs the game library from the configured RomM server into the
 * playlist folder, reporting the outcome as a notification. */
void romm_refresh_library(void);

/* The same at startup, when paired and the user wants it: only a
 * failure the user has to act on is reported. */
void romm_refresh_library_at_start(void);

/* Launches the romm:// playlist entry @content_path with @core_path:
 * fetches the game from RomM into memory first, unless it's there
 * already, then loads it the way any playlist entry loads. Returns
 * 0, or -1 when it couldn't start. */
int romm_launch_from_menu(const char *core_path, const char *content_path,
      const char *label);

RETRO_END_DECLS

#endif
