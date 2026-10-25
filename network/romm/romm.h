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

/* Fetches the playlist the menu has cached from RomM again, in full,
 * and writes it anew, in a library sync. */
void romm_refresh_playlist(void);

/* Is @playlist_file, a file name in the playlist folder, one the RomM
 * library writes? */
bool romm_menu_owns_playlist(const char *playlist_file);

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

/* Lists in @list the status lines of Settings > RomM: whether the
 * device is paired and as whom, and the library as the last sync left
 * it. Read from what's stored, without asking RomM. Returns how many. */
unsigned romm_menu_status_list(file_list_t *list);

/* Lists in @list the Connect to RomM page: the server address and,
 * until the device is paired, whether RomM answers there and, once it
 * does, Pair This Device. Asks RomM the first time it lists an address.
 * Returns how many rows. */
unsigned romm_menu_connect_list(file_list_t *list);

/* OK on Connect to RomM's address: opens the keyboard on the address,
 * or "https://" when none is set. */
void romm_menu_connect_address(void);

/* Lists in @list the pairing page: the server, then either Pair or,
 * once pressed, RomM's code, where to approve it, what it waits for and
 * Cancel; paired, it says so. Returns how many rows. */
unsigned romm_menu_pairing_list(file_list_t *list);

/* Lists in @list Choose Systems, the setup wizard's checklist of RomM's
 * systems by maker, asking RomM for them the first time: Start Sync,
 * then a row per system, path its folder name. Returns how many rows. */
unsigned romm_menu_wizard_list(file_list_t *list);

/* Choose Systems is opening: after a failed request, it asks again. */
void romm_menu_wizard_open(void);

/* What Choose Systems shows beside the system @fs_slug: on, off, or
 * that no emulator runs it. */
size_t romm_menu_wizard_value(const char *fs_slug, char *s, size_t len);

/* OK on a system of Choose Systems: ticks or unticks it. */
void romm_menu_wizard_toggle(const char *fs_slug);

/* The setup wizard's Sync All Systems, and Choose Systems' Start Sync,
 * which leaves the systems not ticked off: each starts the first sync,
 * with no Partial limit, and returns to the top of the menu. */
void romm_menu_wizard_sync_all(void);
void romm_menu_wizard_start(void);

/* Cancel on the pairing page, or leaving it: ends the pairing running. */
void romm_menu_pairing_cancel(void);

/* Lists in @list Unpair This Device, when the device is paired, as its
 * second OK would read once the first armed it. Returns how many rows. */
unsigned romm_menu_unpair_row(file_list_t *list);

/* OK on Unpair This Device: the first arms it; the second, unless a
 * sync, game or Cloud Sync is under way, deletes the device in RomM,
 * then clears the pairing, Cloud Sync's use of RomM and RomM's library
 * here. While RomM can't be reached it leaves the pairing as it is. */
void romm_menu_unpair(void);

/* Lists in @list the Systems page: every system in the library, under
 * its maker, each row's path its RomM folder name. Returns how many
 * rows. */
unsigned romm_menu_systems_list(file_list_t *list);

/* What the Systems page shows beside the system with RomM folder
 * @fs_slug: "60 games", "Off", "New" and so on. */
size_t romm_menu_system_value(const char *fs_slug, char *s, size_t len);

/* The name of the system with RomM folder @fs_slug. */
size_t romm_menu_system_title(const char *fs_slug, char *s, size_t len);

/* Lists in @list the page of the system with RomM folder @fs_slug, and
 * takes it off the systems marked new. Returns how many rows. */
unsigned romm_menu_system_page(file_list_t *list, const char *fs_slug);

/* Lists in @list, for the Manage page of playlist @playlist_file, a row
 * opening the page of each system it holds. Returns how many: 0 for a
 * collection's. */
unsigned romm_menu_playlist_systems(file_list_t *list,
      const char *playlist_file);

/* Lists in @list the details RomM has for the game at @content_path,
 * asking RomM for them the first time; while they come, and when RomM
 * doesn't answer, a line says so. Returns how many rows; 0 for a game
 * not from RomM. */
unsigned romm_menu_game_info(file_list_t *list, const char *content_path);

/* The summary of the game the Information page shows, for its pop-up;
 * false when there is none. */
bool romm_menu_game_summary(char *s, size_t len);

/* Lists in @list an Open in RomM row for the game at @content_path.
 * Returns how many rows. */
unsigned romm_menu_game_open_row(file_list_t *list,
      const char *content_path);

/* Opens the RomM page of the game at @content_path in the browser, at
 * the sign-in address, or else the server address. */
void romm_menu_open_game(const char *content_path);

/* What a system's page can do. */
enum romm_system_action
{
   ROMM_SYSTEM_ACTION_REFRESH = 0,
   ROMM_SYSTEM_ACTION_REFETCH,
   ROMM_SYSTEM_ACTION_REBUILD,
   ROMM_SYSTEM_ACTION_STOP,
   ROMM_SYSTEM_ACTION_SYNC
};

/* OK on @action on the page of the system with RomM folder @fs_slug:
 * starts that system's sync, or queues it behind the one running. Stop
 * Syncing asks for a second OK first. */
void romm_menu_system_action(const char *fs_slug,
      enum romm_system_action action);

RETRO_END_DECLS

#endif
