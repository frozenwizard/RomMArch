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

#ifndef __ROMM_SAVES_H
#define __ROMM_SAVES_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>

#include "romm_client.h"

RETRO_BEGIN_DECLS

/* What was last synced with RomM of one of a game's files. */
typedef struct
{
   /* Its name after the game's base name, such as ".srm" or ".state1";
    * those starting ".state" are save states. */
   char suffix[64];
   /* RomM's entry it last matched, and when RomM last changed that, in
    * seconds since 1970 UTC. */
   unsigned remote_id;
   int64_t remote_time;
   /* MD5 of the local file when it last matched RomM's, in hex. */
   char md5[33];
} romm_saves_file_t;

/* A game as played with one emulator. */
typedef struct
{
   unsigned rom_id;
   /* RetroArch's name for the core: the folder its saves sort into,
    * and what RomM records as the emulator, such as "Snes9x". */
   char emulator[64];
   /* Played since its files last went up to RomM. */
   bool pending;
   /* Where its files are, named <base><suffix>. */
   char save_dir[1024];
   char state_dir[1024];
   char base[256];
   romm_saves_file_t *files;
} romm_saves_game_t;

/* Which save and state files of which games are in sync with RomM. */
typedef struct
{
   romm_saves_game_t *games;
} romm_saves_record_t;

/* One of a game's save or state files on this computer. */
typedef struct
{
   char suffix[64];
   char md5[33];
} romm_saves_local_t;

enum romm_saves_step_type
{
   /* RomM's copy is newer, or the only one: fetch it. */
   ROMM_SAVES_DOWNLOAD = 0,
   /* The local file is newer, or the only one: send it. */
   ROMM_SAVES_UPLOAD,
   /* Both changed since the last sync. */
   ROMM_SAVES_CONFLICT,
   /* Both hold the same, never recorded: record it. */
   ROMM_SAVES_MATCH
};

/* One file to bring in line with RomM. */
typedef struct
{
   enum romm_saves_step_type type;
   bool is_state;
   char suffix[64];
   /* The local file's MD5; empty when there is none. */
   char md5[33];
   /* RomM's copy; id 0 when there is none. */
   romm_save_t remote;
} romm_saves_step_t;

/* What romm_saves_plan() compares. */
typedef struct
{
   /* What was last synced of the game with this emulator, or NULL. */
   const romm_saves_game_t *game;
   const char *emulator;
   /* The base name of the game's files here, and RomM's file name for
    * the game without extension (NULL if unknown), which other
    * clients name their saves after. */
   const char *base;
   const char *rom_base;
   const romm_saves_local_t *saves;
   size_t saves_count;
   const romm_saves_local_t *states;
   size_t states_count;
   const romm_save_t *remote_saves;
   size_t remote_saves_count;
   const romm_save_t *remote_states;
   size_t remote_states_count;
} romm_saves_plan_params_t;

/* What a sync before launch does with a file changed on both sides
 * since the last sync. Either way the copy not chosen is kept in
 * RomM, apart from what syncs use: a save as "<base> (conflict)
 * <suffix>" in the "conflict" slot, a state as "<base> (conflict
 * <time>)<suffix>". */
enum romm_saves_resolve
{
   /* Neither copy is touched; the file is counted as a conflict. */
   ROMM_SAVES_RESOLVE_NONE = 0,
   /* RomM's copy replaces this device's. */
   ROMM_SAVES_RESOLVE_REMOTE,
   /* This device's copy goes up over RomM's. */
   ROMM_SAVES_RESOLVE_LOCAL
};

/* A sync of one game's saves and states with RomM. */
typedef struct
{
   const char *server;
   const char *token;
   /* RomM's id for this device, or NULL: saves go up and down as it,
    * so RomM refuses an upload over a newer save from elsewhere. */
   const char *device_id;
   unsigned rom_id;
   const char *emulator;
   /* See romm_saves_plan_params_t. */
   const char *base;
   const char *rom_base;
   /* Where the game's saves and states are, or go. */
   const char *save_dir;
   const char *state_dir;
   /* The record, at romm_saves_record_path(). */
   const char *record_path;
   /* Send what changed here (after the game closed), rather than fetch
    * what RomM has newer (before it starts). */
   bool push;
   /* Before launch, what to do with a file changed on both sides. */
   enum romm_saves_resolve resolve;
   /* Whether a running core owns the file at @path, which a download
    * then mustn't replace; NULL if none can. */
   bool (*is_live)(const char *path);
} romm_saves_sync_params_t;

/* Files a sync moved or left alone. */
typedef struct
{
   unsigned downloaded;
   unsigned uploaded;
   /* Changed both here and in RomM; neither copy was touched. */
   unsigned conflicts;
   unsigned failed;
} romm_saves_result_t;

/* @status is that of listing RomM's saves and states; @result counts
 * the files. */
typedef void (*romm_saves_sync_cb_t)(void *user_data,
      enum romm_status status, int http_status,
      const romm_saves_result_t *result);

/* <@save_dir>/rommarch-saves.json */
size_t romm_saves_record_path(char *s, size_t len, const char *save_dir);

/* Reads the record at @path into @rec, which starts empty. False, with
 * @rec empty, when there is none or it can't be read. */
bool romm_saves_record_load(romm_saves_record_t *rec, const char *path);

/* Writes @rec to @path, replacing it. */
bool romm_saves_record_save(const romm_saves_record_t *rec,
      const char *path);

void romm_saves_record_free(romm_saves_record_t *rec);

/* The record of game @rom_id played with @emulator; with @create, a
 * new one when there is none, else NULL. Valid until @rec changes. */
romm_saves_game_t *romm_saves_record_game(romm_saves_record_t *rec,
      unsigned rom_id, const char *emulator, bool create);

/* @game's record of its file with @suffix; with @create, a new one
 * when there is none, else NULL. Valid until @game's files change. */
romm_saves_file_t *romm_saves_game_file(romm_saves_game_t *game,
      const char *suffix, bool create);

/* The rest of file name @name after @base, when @name is one of that
 * game's files: a suffix of up to three dot-led parts without spaces,
 * such as ".srm", ".A1.bin" or ".state.auto", that isn't a screenshot
 * or a temporary file. NULL otherwise. */
const char *romm_saves_suffix(const char *name, const char *base);

/* Writes the MD5 of the file at @path to @md5 in hex. */
bool romm_saves_md5_file(const char *path, char *md5);

/* @base's save files in @dir, or its save states with @states, each
 * with its MD5; an RBUF for the caller to free. */
romm_saves_local_t *romm_saves_list_local(const char *dir,
      const char *base, bool states);

/* What to do to bring a game's files here and in RomM in line, one
 * step per file that needs one; an RBUF for the caller to free. For
 * each kind of save (".srm", ".brm", ...), RomM's copy is the newest
 * of that kind from any client and slot but the "conflict" slot; for
 * each save state, the one of the same name the same emulator made. */
romm_saves_step_t *romm_saves_plan(const romm_saves_plan_params_t *p);

/* Syncs one game's files with RomM per @p: before it starts, fetches
 * what RomM has newer, and records what both already hold; after it
 * closes, sends what changed here. Files that changed on both sides
 * are counted, not touched. The record is updated after each file.
 * Syncs run one at a time, in the order asked for. @cb runs on the
 * main thread, never before this returns; false when the sync can't
 * start, and @cb then doesn't run. */
bool romm_saves_sync(const romm_saves_sync_params_t *p,
      romm_saves_sync_cb_t cb, void *user_data);

RETRO_END_DECLS

#endif
