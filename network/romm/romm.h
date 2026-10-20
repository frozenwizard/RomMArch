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

#include <stddef.h>
#include <boolean.h>
#include <lists/file_list.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Asks the configured RomM server for its version and reports the
 * outcome as a notification. */
void romm_check_connection(void);

/* Pairs this install with the configured RomM server: shows the code,
 * opens the approval page in the browser and, once the user approves,
 * keeps the device's token and saves the configuration. */
void romm_pair(void);

/* The server or sign-in address has changed: a pairing running starts
 * over with it. */
void romm_pair_follow_address(void);

/* Points Cloud Sync, when it is on and pointed at RomM's WebDAV, at the
 * server address as it now is. */
void romm_cloud_sync_follow_server(void);

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

/* Launches the romm:// entry @content_path, from the playlist named
 * @playlist_name, when it has no installed core: with its platform's
 * preferred core if one is installed, or else, once a second Run
 * confirms it, after downloading that core. False when there is no
 * preferred core or no core downloader, for the stock flow to go on. */
bool romm_launch_without_core(const char *content_path, const char *label,
      const char *playlist_name);

/* For a RomM game at @content_path, the name of the folder its saves
 * and states sort into by content: its RomM id, wherever in the game
 * the file is. False for other content. */
bool romm_content_dir_name(char *s, size_t len, const char *content_path);

/* For a RomM game at @s, with or without its extension: on Windows,
 * takes out of its file name the characters Windows can't store, which
 * RomM's names can hold. Saves, states and overrides are named after
 * it. Other content is left as it is. */
void romm_content_base_name(char *s, size_t len);

/* The core has unloaded and @next_content, if any, loads next: frees
 * the memory of every RomM game but that one. */
void romm_content_unloaded(const char *next_content);

/* Content is closing and its save RAM is on disk: if it is a RomM
 * game, sends what changed of its saves and states to RomM. */
void romm_content_closed(void);

/* Sends the saves of games that changed since their last sync but
 * didn't go up: played offline, left running at quit, or refused. */
void romm_sync_owed_saves(void);

/* Content at @content_path was added to Favorites or, without @added,
 * taken out: for a RomM game, the same goes to RomM's favorites. */
void romm_favorite_changed(const char *content_path, bool added);

/* Groups a sidebar's playlist rows, @list, as romm_sidebar_group()
 * does, with the groups the user left open. */
void romm_menu_sidebar_group(file_list_t *list);

/* Opens or closes the group sidebar row @row is; false if it isn't a
 * group. */
bool romm_menu_sidebar_toggle(const struct item_file *row);

/* Whether sidebar row @row is an open group. */
bool romm_menu_sidebar_is_open(const struct item_file *row);

/* How deep in the groups row @i of sidebar @list sits: 0 unless
 * romm_menu_sidebar_group() grouped it. */
unsigned romm_menu_sidebar_depth(const file_list_t *list, size_t i);

/* Lists in @list, as the Playlists menu does, the playlists in the
 * group with key @key. Returns how many. */
unsigned romm_menu_sidebar_list(file_list_t *list, const char *key);

RETRO_END_DECLS

#endif
