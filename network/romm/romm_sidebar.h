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

/* A playlist the RomM library writes: a system's, with the maker it is
 * grouped under, or a collection's. */
typedef struct
{
   char playlist[256];
   /* Empty when neither RomM nor the libretro name tells. */
   char maker[64];
   bool collection;
} romm_sidebar_system_t;

typedef struct
{
   /* Names shown on group rows. */
   const char *collections;
   const char *systems;
   /* The group of systems without a maker. */
   const char *other;
   /* The item type group rows get. */
   unsigned group_type;
} romm_sidebar_names_t;

/* Fills @systems, an RBUF it empties first, with the library's system
 * playlists, each once, and their makers, then its collections'. */
void romm_sidebar_systems(romm_sidebar_system_t **systems,
      const romm_library_t *lib);

/* Rewrites @list, one row per playlist file (path the file name, alt
 * the name shown), into groups: Collections, then Systems with a group
 * per maker inside it, each system under its maker, and after them any
 * playlist that isn't RomM's. A group's rows are left out unless @open
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
 * in group @key: collections by file name, systems by maker, then by
 * file name. */
void romm_sidebar_members(const char ***playlists,
      const romm_sidebar_system_t *systems, const char *key);

/* The maker @p is grouped under: RomM's family_name, else the vendor in
 * its libretro name; empty when neither tells. */
void romm_sidebar_maker(char *s, size_t len, const romm_platform_t *p);

/* Where a system stands, as the Systems page shows it. */
enum romm_system_state
{
   ROMM_SYSTEM_SYNCED = 0,
   /* Synced, with more to fetch that the last sync left */
   ROMM_SYSTEM_WAITING,
   /* On, never synced: a sync at start left it for a manual one */
   ROMM_SYSTEM_NOT_SYNCED,
   ROMM_SYSTEM_OFF,
   /* Off, being new in RomM, and not shown on the page yet */
   ROMM_SYSTEM_NEW,
   ROMM_SYSTEM_NO_EMULATOR
};

/* A row of the Systems page. */
typedef struct
{
   romm_platform_t platform;
   char maker[64];
   /* Its playlist; empty when not synced. */
   char playlist[256];
   /* When a sync last fetched it; 0 when never or not known. */
   int64_t synced_at;
   enum romm_system_state state;
   /* Its games here when synced, else RomM's count. */
   unsigned games;
   /* What the last sync left to fetch. */
   unsigned games_waiting;
   unsigned covers_waiting;
} romm_system_row_t;

/* Fills @rows, an RBUF it empties first, with every system in @lib,
 * synced or not, in the state @exclude and @new_list give them (lists
 * as romm_library_listed() reads) and the @waiting_count systems a
 * sync left waiting, @waiting. By maker, as the sidebar has them, then
 * by name; the ones no emulator runs last. */
void romm_sidebar_system_rows(romm_system_row_t **rows,
      const romm_library_t *lib, const char *exclude, const char *new_list,
      const romm_library_waiting_t *waiting, size_t waiting_count);

RETRO_END_DECLS

#endif
