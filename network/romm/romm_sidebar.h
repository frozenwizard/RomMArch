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

#ifndef __ROMM_SIDEBAR_H
#define __ROMM_SIDEBAR_H

#include <stddef.h>

#include <boolean.h>
#include <lists/file_list.h>
#include <retro_common_api.h>

#include "romm_library.h"

RETRO_BEGIN_DECLS

/* Group keys, as a sidebar's open groups name them. A maker's group is
 * ROMM_SIDEBAR_SYSTEMS "/" and its name. */
#define ROMM_SIDEBAR_COLLECTIONS "collections"
#define ROMM_SIDEBAR_SYSTEMS     "systems"
#define ROMM_SIDEBAR_UNSUPPORTED "unsupported"

/* A playlist the RomM library writes: a system's, with the maker it is
 * grouped under, or a collection's. */
typedef struct
{
   char playlist[256];
   /* Empty when neither RomM nor the libretro name tells. */
   char maker[64];
   bool collection;
   /* RetroArch has no system for it: RomM gave it no libretro name. */
   bool unsupported;
} romm_sidebar_system_t;

typedef struct
{
   /* Names shown on group rows. */
   const char *collections;
   const char *systems;
   /* The group of systems without a maker. */
   const char *other;
   /* The group of unsupported systems, last in Systems. */
   const char *unsupported;
   /* The item type group rows get. */
   unsigned group_type;
} romm_sidebar_names_t;

/* Fills @systems, an RBUF it empties first, with the library's system
 * playlists, each once, and their makers, then its collections'. */
void romm_sidebar_systems(romm_sidebar_system_t **systems,
      const romm_library_t *lib);

/* Rewrites @list, one row per playlist file (path the file name, alt
 * the name shown), into groups: Collections, then Systems with a group
 * per maker inside it, each system under its maker, and an Unsupported
 * group last, and after them any playlist that isn't RomM's. A group's rows are left out unless @open
 * names its key; keys are separated by '|'. Group rows have no path,
 * their key as label and their name as alt. Each row's entry_idx is its
 * depth. */
void romm_sidebar_group(file_list_t *list,
      const romm_sidebar_system_t *systems, const char *open,
      const romm_sidebar_names_t *names);

/* Whether @open names @key. */
bool romm_sidebar_is_open(const char *open, const char *key);

/* Adds @key to @open, or takes it out if it's there. */
void romm_sidebar_toggle(char *open, size_t len, const char *key);

/* Fills @playlists, an RBUF it empties first, with the playlist files
 * in group @key: collections and unsupported systems by file name,
 * other systems by maker, then by file name. */
void romm_sidebar_members(const char ***playlists,
      const romm_sidebar_system_t *systems, const char *key);

RETRO_END_DECLS

#endif
