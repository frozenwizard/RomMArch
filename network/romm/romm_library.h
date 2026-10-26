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
   /* That file's size and modification time as the sync left it; a
    * file that differs was written by something else since. */
   int64_t playlist_size;
   int64_t playlist_mtime;
   /* When a sync last fetched it, in seconds since the epoch; 0 when
    * the manifest is from before this was kept. */
   int64_t synced_at;
   /* Its games in RomM's order (an RBUF). */
   romm_rom_t *roms;
} romm_library_platform_t;

/* What the last sync wrote. Kept with the playlists so the next sync
 * knows which playlists are RomM's and what has changed. */
/* A RomM collection the sync wrote a playlist for. */
typedef struct
{
   unsigned id;
   bool smart;
   char name[128];
   /* Its playlist's file name in the playlist folder. */
   char playlist[256];
} romm_library_collection_t;

/* A platform a sync left as it was, having more to fetch than its
 * limit. */
typedef struct
{
   romm_platform_t platform;
   /* Games to fetch: at least this many; 0 when only covers wait. */
   size_t games;
   /* Covers to fetch, or 0. */
   size_t covers;
} romm_library_waiting_t;

typedef struct
{
   char server[256];
   /* When that sync finished, in seconds since the epoch. */
   int64_t synced_at;
   romm_library_platform_t *platforms; /* an RBUF */
   romm_library_collection_t *collections; /* an RBUF */
   /* The user's favorites collection, 0 when RomM has none. */
   unsigned favorites_id;
   /* Its games, an RBUF, when this sync listed collections: then
    * @favorites_listed is set. Neither is in the manifest. */
   unsigned *favorite_ids;
   bool favorites_listed;
   /* Platforms this sync left as they were (an RBUF); not in the
    * manifest. */
   romm_library_waiting_t *waiting;
   /* RomM's other platforms with games, which the sync didn't write:
    * switched off, run by no emulator, or left waiting (an RBUF). */
   romm_platform_t *unsynced;
   /* Of those, the ones new in RomM that the sync left off, with
    * params.new_off (an RBUF); not in the manifest. */
   romm_platform_t *added;
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
 * covers are looked up under thumbnails/<db_name without .lpl>. A core
 * the user set for a game in the playlist being replaced is kept, and
 * so is how it shows its games (labels, thumbnails, sorting). */
bool romm_library_write_playlist(const char *path, const char *db_name,
      const char *default_core_path, const char *default_core_name,
      const romm_rom_t *roms, size_t count);

/* Finds the first of @cores (core file names without "_libretro" and
 * extension) that is installed, filling its path and display name. */
typedef bool (*romm_library_find_core_t)(const char *const *cores,
      char *path, size_t path_len, char *name, size_t name_len);

/* The playlist at @path was just written or deleted. */
typedef void (*romm_library_playlist_cb_t)(const char *path);

/* Can the core at @core_path run system @libretro_slug? */
typedef bool (*romm_library_core_runs_t)(const char *core_path,
      const char *libretro_slug);

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
   /* Runs as each playlist is rewritten or deleted, so a copy of it in
    * memory can be read again, or dropped, before anything writes that
    * back; may be NULL. */
   romm_library_playlist_cb_t playlist_changed;
   /* Also writes a playlist per RomM collection, the user's, other
    * users' public ones and smart ones, but not the favorites. */
   bool collections;
   /* A playlist to fetch from RomM again in full: its games, without
    * the incremental shortcut, and their covers, then written anew.
    * NULL for none. */
   const char *refetch;
   /* A playlist's default core is kept only if this says it runs the
    * system; NULL keeps any. */
   romm_library_core_runs_t core_runs;
   /* A platform with more games new or changed than this is left as
    * the last sync wrote it, or not written yet; one with more covers
    * to fetch leaves them for a later sync. Either is listed in the
    * library's waiting. 0 fetches everything. */
   size_t limit;
   /* Platforms switched off, by fs_slug, separated by commas; NULL for
    * none. Their playlists and covers go, as for platforms gone from
    * RomM, and they're listed in the library's unsynced. */
   const char *exclude;
   /* A platform the last sync didn't list starts off: it isn't synced,
    * and is listed in the library's added for the caller to switch
    * off. Not on a first sync, which takes every platform. */
   bool new_off;
   /* Syncs only these platforms, by fs_slug as for @exclude; the others
    * keep their manifest entries, playlists and covers as the last sync
    * left them, and aren't counted as gone. NULL syncs every one. */
   const char *only;
   /* For the platforms in @only: every game fetched, without the
    * incremental shortcut, every cover again, and the playlist written
    * anew, as @refetch does for one playlist. */
   bool full;
   /* For the platforms in @only: the playlist and the covers are
    * deleted first, with what the user set in the playlist (cores,
    * sorting, labels), and the platform is synced as if new. */
   bool rebuild;
} romm_library_sync_params_t;

/* @lib is the library as the sync left it and @old as it was before,
 * NULL unless @status is ROMM_OK; both valid only during the call.
 * @http_status is that of a failed request, or 0. */
typedef void (*romm_library_sync_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_library_t *old, const romm_library_t *lib);

/* Where RetroArch looks for @rom's box art when it is listed in
 * @playlist: thumbnails/<playlist name>/Named_Boxarts/<file name>.png,
 * named after the playlist path's file name as the menu names it. A
 * JPEG cover is saved as .jpg in its place, which the menu finds when
 * it allows image types other than PNG. */
size_t romm_library_cover_path(char *s, size_t len,
      const char *thumbnails_dir, const char *playlist,
      const romm_rom_t *rom);

/* The emulators that run @libretro_slug's games from RomM, best first,
 * as core file names without "_libretro" and extension; NULL when there
 * are none for the system. They load games from memory, but for a
 * system whose emulators all need a file. */
const char *const *romm_library_preferred_cores(const char *libretro_slug);

/* Rommarch's default for core option @key, where it differs from the
 * core's so each game's saves are a file of its own; NULL for the
 * core's own. */
const char *romm_library_core_option_default(const char *key);

/* @lib's games sorted by id, for romm_library_follow_game(): an RBUF
 * for the caller to free, valid while @lib is. */
const romm_rom_t **romm_library_index(const romm_library_t *lib);

/* The game @id in @index, as romm_library_index() made it; NULL when
 * there is none. */
const romm_rom_t *romm_library_find(const romm_rom_t *const *index,
      unsigned id);

/* The RomM game id in @path, 0 unless it is a romm:// path. */
unsigned romm_library_path_id(const char *path);

/* The playlist of the platform @rom is on in @lib, "" if none. */
const char *romm_library_rom_playlist(const romm_library_t *lib,
      const romm_rom_t *rom);

/* After a sync from @old to @lib (as indexed by romm_library_index()),
 * what becomes of an entry for @path, labelled @label, in a list of
 * games the sync doesn't write, such as History. False when its game
 * is gone: deleted, or hidden from the user. Otherwise @new_path is
 * its path now, renamed with the game when it was the game's playlist
 * path, and @new_label RomM's title now when @label was its title
 * before. */
bool romm_library_follow_game(const romm_rom_t *const *old,
      const romm_rom_t *const *lib, const char *path, const char *label,
      char *new_path, size_t path_len, char *new_label, size_t label_len);

/* Is @name one of the names in @list, which commas separate? */
bool romm_library_listed(const char *list, const char *name);

/* Adds @name to @list, a buffer of @len, unless it's there; false when
 * it doesn't fit. */
bool romm_library_list_add(char *list, size_t len, const char *name);

/* Takes @name out of @list wherever it is. */
void romm_library_list_remove(char *list, const char *name);

/* <@playlist_dir>/romm/library.json */
size_t romm_library_manifest_path(char *s, size_t len,
      const char *playlist_dir);

/* Reads the manifest at @path into @lib, which starts empty. False,
 * with @lib empty, when there is none or it can't be read. */
bool romm_library_load(romm_library_t *lib, const char *path);

/* Writes @lib to @path, replacing it, creating its folder if needed. */
bool romm_library_save(const romm_library_t *lib, const char *path);

void romm_library_free(romm_library_t *lib);

/* Games of one platform with the same name are a group, which a
 * playlist lists as one entry: the first in its order. Fills @groups,
 * an RBUF it empties first, with @lib's games that are in a group, in
 * their platforms' order. */
void romm_library_groups(romm_rom_t **groups, const romm_library_t *lib);

/* Deletes what the syncs wrote in @playlist_dir: each playlist its
 * manifest records, their games' covers in @thumbnails_dir (when not
 * empty), and the manifest, stopping covers still being fetched. @playlist_changed, when given, runs for
 * each playlist deleted. */
void romm_library_remove_all(const char *playlist_dir,
      const char *thumbnails_dir,
      romm_library_playlist_cb_t playlist_changed);

/* Brings the playlists in the playlist folder in line with the server:
 * one per RomM platform with games the token's user can see that an
 * emulator runs, and the manifest recording them. Runs as a task showing its progress; @cb
 * runs on the main thread when it ends. Then, given a thumbnails
 * folder, a second task fetches the box art that is new or changed;
 * covers of games gone are deleted. */
bool romm_library_sync(const romm_library_sync_params_t *params,
      romm_library_sync_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
