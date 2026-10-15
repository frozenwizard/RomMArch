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

#ifndef __ROMM_PLATFORM_H
#define __ROMM_PLATFORM_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The platform side of the RomM frontend: romm_apple.m on Apple,
 * romm_platform.c on Windows and Linux. */

/* Opens @url in the user's web browser. */
bool romm_platform_open_url(const char *url);

/* The computer's name as the user knows it, for RomM's device list. */
size_t romm_platform_device_name(char *s, size_t len);

/* A new random identifier for this install. */
size_t romm_platform_new_identifier(char *s, size_t len);

RETRO_END_DECLS

#endif
