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

/* Seconds since 1970 UTC of an ISO 8601 time such as RomM's
 * "2026-10-13T02:18:25.590502+00:00", fractions dropped; -1 if @iso
 * isn't one. */
int64_t romm_saves_time(const char *iso);

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

RETRO_END_DECLS

#endif
