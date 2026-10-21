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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <array/rbuf.h>
#include <compat/posix_string.h>
#include <compat/strl.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <formats/rjson.h>
#include <queues/task_queue.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "romm_json.h"
#include "romm_library.h"
#include "../../file_path_special.h"
#include "../../playlist.h"
#include "../../verbosity.h"

#define ROMM_LIBRARY_VERSION 1
#define ROMM_LIBRARY_PAGE 250
#define ROMM_LIBRARY_COVERS_AT_ONCE 4
/* Games whose covers are looked for on disk per step of the sync */
#define ROMM_LIBRARY_COVERS_PER_STEP 256
/* RomM's timestamps have whole seconds, and a game can be saved with
 * one a moment older than the newest the last sync saw. */
#define ROMM_LIBRARY_CURSOR_MARGIN 60
#define ROMM_LIB_PFX "[RomM] "

/* A cover to fetch: its path on the server and where it goes. */
typedef struct
{
   char *url_path;
   char *dest;
} romm_lib_cover_t;

typedef struct
{
   retro_task_t *task;
   romm_lib_cover_t *jobs;
   size_t next;
   size_t done;
   size_t failed;
   unsigned in_flight;
   retro_time_t started;
   char *token;
   char server[NAME_MAX_LENGTH];
} romm_lib_covers_t;

enum romm_lib_sync_state
{
   ROMM_LIB_SYNC_PLATFORMS = 0,
   /* Starting the next platform RomM has games on */
   ROMM_LIB_SYNC_NEXT,
   /* For a platform the last sync wrote: its game ids, then the games
    * changed since, a page at a time, merged with the manifest's */
   ROMM_LIB_SYNC_IDS,
   ROMM_LIB_SYNC_CHANGES,
   ROMM_LIB_SYNC_MERGE,
   /* All its games, a page at a time */
   ROMM_LIB_SYNC_ROMS,
   /* Over the limit: writing it from the manifest's games instead */
   ROMM_LIB_SYNC_HOLD,
   ROMM_LIB_SYNC_WRITE,
   /* RomM's collections, its smart ones, then a playlist each */
   ROMM_LIB_SYNC_COLLECTIONS,
   ROMM_LIB_SYNC_SMART,
   ROMM_LIB_SYNC_WRITE_COLLECTIONS,
   /* The covers to fetch and to delete, a slice of games at a time */
   ROMM_LIB_SYNC_COVERS,
   ROMM_LIB_SYNC_SAVE
};

typedef struct
{
   romm_library_sync_cb_t cb;
   void *user_data;
   retro_task_t *task;
   /* The manifest as the last sync left it, and the one being built */
   romm_library_t old;
   romm_library_t lib;
   /* RomM's platforms, and the games of the one being fetched */
   romm_platform_t *remote;
   romm_rom_t *roms;
   size_t platform;
   unsigned offset;
   /* That platform in the last sync's manifest, its ids now, and when
    * its games were last seen changing */
   const romm_library_platform_t *old_platform;
   unsigned *ids;
   char cursor[32];
   /* More games new or changed than this leave a platform as it was */
   size_t limit;
   /* With a limit: that platform's games by id, and how many of the
    * changes fetched are new or changed since */
   const romm_rom_t **known;
   size_t changed;
   /* Per entry of @lib: was its playlist written by this sync? */
   bool *written;
   unsigned rewritten;
   retro_time_t started;
   enum romm_lib_sync_state state;
   enum romm_status status;
   int http_status;
   bool pending;
   bool finished;
   /* Covers the sync found missing or changed, for the covers task */
   romm_lib_cover_t *covers;
   /* Where cover planning is: its phase, the last sync's games by id,
    * the cover files this sync lists, and the next game. */
   unsigned cover_phase;
   const romm_rom_t **cover_old;
   char **cover_dests;
   size_t cover_platform;
   size_t cover_game;
   /* The platform whose covers are being planned, from that job on */
   size_t cover_limited;
   size_t cover_from;
   romm_library_find_core_t find_core;
   romm_library_playlist_cb_t playlist_changed;
   /* RomM's collections, when asked for, and which lists it gave */
   romm_collection_t *remote_collections;
   bool collections;
   bool collections_listed;
   bool smart_listed;
   /* The playlist to fetch in full and write anew, or empty */
   char refetch[256];
   /* Platforms switched off, as params.exclude; NULL for none */
   char *exclude;
   bool new_off;
   /* There was a manifest from a last sync */
   bool had_old;
   /* The platforms to sync, as params.only and as RomM's ids now; NULL
    * for every one */
   char *only;
   unsigned *only_ids;
   romm_library_core_runs_t core_runs;
   char *token;
   char *covers_title;
   char server[NAME_MAX_LENGTH];
   char playlist_dir[PATH_MAX_LENGTH];
   char thumbnails_dir[PATH_MAX_LENGTH];
   char manifest[PATH_MAX_LENGTH];
} romm_lib_sync_t;

/* The covers task running, which the next sync to plan covers stops.
 * Main thread. */
static retro_task_t *romm_lib_covers_running = NULL;

/* D10: emulators that read games through RetroArch's file layer. */
typedef struct
{
   const char *libretro_slug;
   const char *cores[3];
} romm_lib_system_cores_t;

static const romm_lib_system_cores_t romm_lib_system_cores[] = {
   { "Nintendo - Nintendo Entertainment System", { "mesen", "fceumm" } },
   { "Nintendo - Family Computer Disk System",   { "mesen", "fceumm" } },
   { "Nintendo - Super Nintendo Entertainment System", { "snes9x" } },
   { "Nintendo - Game Boy",                      { "gambatte", "sameboy" } },
   { "Nintendo - Game Boy Color",                { "gambatte", "sameboy" } },
   { "Nintendo - Game Boy Advance",              { "mgba" } },
   { "Nintendo - Nintendo 64",                   { "mupen64plus_next" } },
   { "Nintendo - Nintendo DS",                   { "melondsds" } },
   { "Nintendo - Virtual Boy",                   { "mednafen_vb" } },
   { "Sega - Mega Drive - Genesis",              { "genesis_plus_gx" } },
   { "Sega - Master System - Mark III",          { "genesis_plus_gx" } },
   { "Sega - Game Gear",                         { "genesis_plus_gx" } },
   { "Sega - Mega-CD - Sega CD",                 { "genesis_plus_gx" } },
   { "Sega - 32X",                               { "picodrive" } },
   { "Sega - Saturn",                            { "mednafen_saturn" } },
   { "Sega - Dreamcast",                         { "flycast" } },
   { "Sony - PlayStation",                       { "mednafen_psx_hw", "swanstation" } },
   { "NEC - PC Engine - TurboGrafx 16",          { "mednafen_pce_fast" } },
   { "NEC - PC Engine CD - TurboGrafx-CD",       { "mednafen_pce_fast" } },
   { "SNK - Neo Geo CD",                         { "neocd" } },
   { "SNK - Neo Geo Pocket",                     { "mednafen_ngp" } },
   { "SNK - Neo Geo Pocket Color",               { "mednafen_ngp" } },
   { "The 3DO Company - 3DO",                    { "opera" } },
   { "Bandai - WonderSwan",                      { "mednafen_wswan" } },
   { "Bandai - WonderSwan Color",                { "mednafen_wswan" } },
   { "Atari - Lynx",                             { "handy" } },
   { "Atari - 2600",                             { "stella" } },
   { "Atari - 7800",                             { "prosystem" } },
   { "Coleco - ColecoVision",                    { "gearcoleco" } },
   { "GCE - Vectrex",                            { "vecx" } }
};

const char *const *romm_library_preferred_cores(const char *libretro_slug)
{
   size_t i;
   if (!string_is_empty(libretro_slug))
      for (i = 0; i < ARRAY_SIZE(romm_lib_system_cores); i++)
         if (string_is_equal(romm_lib_system_cores[i].libretro_slug,
               libretro_slug))
            return romm_lib_system_cores[i].cores;
   return NULL;
}

/* Core options whose default saves one file for every game: per game,
 * each game's saves go up to RomM under that game (plan D11). */
static const char *const romm_lib_option_defaults[][2] = {
   /* Sega CD backup RAM: <game>.brm rather than scd_<region>.brm */
   { "genesis_plus_gx_system_bram", "per game" },
   /* Flycast's memory card A1: <game>.A1.bin rather than a shared one */
   { "reicast_per_content_vmus",    "VMU A1" }
};

const char *romm_library_core_option_default(const char *key)
{
   size_t i;
   if (!string_is_empty(key))
      for (i = 0; i < ARRAY_SIZE(romm_lib_option_defaults); i++)
         if (string_is_equal(romm_lib_option_defaults[i][0], key))
            return romm_lib_option_defaults[i][1];
   return NULL;
}

/* Characters a file name can't hold on some system RetroArch runs on. */
static void romm_lib_scrub_file_name(char *s)
{
   while ((s = strpbrk(s, "/\\:*?\"<>|")))
      *s++ = '_';
}

size_t romm_library_content_path(char *s, size_t len,
      const romm_rom_t *rom)
{
   int _len = snprintf(s, len, ROMM_SCHEME "%u/%s%s", rom->id,
         rom->fs_name, rom->single_file ? "" : ".m3u");
   if (_len < 0 || (size_t)_len >= len)
   {
      if (len)
         *s = '\0';
      return 0;
   }
   return (size_t)_len;
}

size_t romm_library_cover_path(char *s, size_t len,
      const char *thumbnails_dir, const char *playlist,
      const romm_rom_t *rom)
{
   char content_path[PATH_MAX_LENGTH];
   char system[NAME_MAX_LENGTH];
   char name[NAME_MAX_LENGTH];
   char dir[PATH_MAX_LENGTH];
   char *c;

   if (!romm_library_content_path(content_path, sizeof(content_path), rom))
      return 0;
   /* As gfx_thumbnail_set_content_playlist() and
    * gfx_thumbnail_fill_content_img() name it. */
   fill_pathname(name, path_basename(content_path), "", sizeof(name));
   for (c = name; (c = strpbrk(c, "&*/:`\"<>?\\|")); c++)
      *c = '_';
   strlcat(name, ".png", sizeof(name));
   fill_pathname(system, playlist, "", sizeof(system));
   fill_pathname_join(dir, thumbnails_dir, system, sizeof(dir));
   fill_pathname_join(dir, dir, "Named_Boxarts", sizeof(dir));
   return fill_pathname_join(s, dir, name, len);
}

size_t romm_library_playlist_name(char *s, size_t len,
      const romm_platform_t *platform)
{
   int _len;

   if (*platform->libretro_slug)
      _len = snprintf(s, len, "%s.lpl", platform->libretro_slug);
   else
      _len = snprintf(s, len, "RomM - %s.lpl",
            *platform->name ? platform->name : platform->slug);
   if (_len < 0 || (size_t)_len >= len)
   {
      if (len)
         *s = '\0';
      return 0;
   }
   romm_lib_scrub_file_name(s);
   return (size_t)_len;
}

static void romm_lib_json_member(rjsonwriter_t *writer, unsigned indent,
      const char *key, const char *value, bool last)
{
   rjsonwriter_add_spaces(writer, indent);
   rjsonwriter_add_string(writer, key);
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, value);
   if (!last)
      rjsonwriter_raw(writer, ",", 1);
   rjsonwriter_raw(writer, "\n", 1);
}

static void romm_lib_json_member_int(rjsonwriter_t *writer,
      const char *key, int value)
{
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, key);
   rjsonwriter_rawf(writer, ": %d,\n", value);
}

/* A core the user set for one game, by RomM game id. */
typedef struct
{
   unsigned id;
   char *core_path;
   char *core_name;
} romm_lib_game_core_t;

typedef struct
{
   romm_lib_game_core_t *list;
   unsigned id;
   char *core_path;
   char *core_name;
} romm_lib_game_cores_t;

static void romm_lib_game_core_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_lib_game_cores_t *cores = (romm_lib_game_cores_t*)ctx;

   if (string_is_equal(key, "path")
         && string_starts_with(value, ROMM_SCHEME))
      cores->id = (unsigned)strtoul(value + STRLEN_CONST(ROMM_SCHEME),
            NULL, 10);
   else if (string_is_equal(key, "core_path") && !cores->core_path)
      cores->core_path = strdup(value);
   else if (string_is_equal(key, "core_name") && !cores->core_name)
      cores->core_name = strdup(value);
}

static void romm_lib_game_core_end(void *ctx)
{
   romm_lib_game_cores_t *cores = (romm_lib_game_cores_t*)ctx;

   if (     cores->id && cores->core_path && cores->core_name
         && *cores->core_path
         && !string_is_equal(cores->core_path, FILE_PATH_DETECT))
   {
      romm_lib_game_core_t core;
      core.id        = cores->id;
      core.core_path = cores->core_path;
      core.core_name = cores->core_name;
      RBUF_PUSH(cores->list, core);
   }
   else
   {
      free(cores->core_path);
      free(cores->core_name);
   }
   cores->id        = 0;
   cores->core_path = NULL;
   cores->core_name = NULL;
}

static int romm_lib_cmp_game_core(const void *a, const void *b)
{
   unsigned x = ((const romm_lib_game_core_t*)a)->id;
   unsigned y = ((const romm_lib_game_core_t*)b)->id;
   return x < y ? -1 : x > y;
}

/* How a playlist shows its games, which Playlist Management changes. */
typedef struct
{
   int label_display_mode;
   int right_thumbnail_mode;
   int left_thumbnail_mode;
   int sort_mode;
} romm_lib_display_t;

static void romm_lib_read_mode(const char *doc, size_t len,
      const char *key, int *mode)
{
   char num[16];
   if (romm_json_get(doc, len, key, num, sizeof(num)))
      *mode = (int)strtol(num, NULL, 10);
}

/* What the user set in the playlist at @path: how it shows its games,
 * into @display, and the cores of single games, returned. */
static romm_lib_game_core_t *romm_lib_read_playlist(const char *path,
      romm_lib_display_t *display)
{
   romm_lib_game_cores_t cores;
   char *doc   = NULL;
   int64_t len = 0;

   memset(&cores, 0, sizeof(cores));
   if (     !path_is_valid(path)
         || !filestream_read_file(path, (void**)&doc, &len) || !doc)
      return NULL;
   romm_lib_read_mode(doc, (size_t)len, "label_display_mode",
         &display->label_display_mode);
   romm_lib_read_mode(doc, (size_t)len, "right_thumbnail_mode",
         &display->right_thumbnail_mode);
   romm_lib_read_mode(doc, (size_t)len, "left_thumbnail_mode",
         &display->left_thumbnail_mode);
   romm_lib_read_mode(doc, (size_t)len, "sort_mode", &display->sort_mode);
   romm_json_items(doc, (size_t)len, "items", romm_lib_game_core_member,
         romm_lib_game_core_end, &cores);
   free(cores.core_path);
   free(cores.core_name);
   free(doc);
   if (cores.list)
      qsort(cores.list, RBUF_LEN(cores.list), sizeof(*cores.list),
            romm_lib_cmp_game_core);
   return cores.list;
}

static void romm_lib_free_game_cores(romm_lib_game_core_t *list)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(list); i++)
   {
      free(list[i].core_path);
      free(list[i].core_name);
   }
   RBUF_FREE(list);
}

/* As romm_library_write_playlist(); a game's db_name is @db_names[i]
 * when @db_names is given. With @changed, the file is written only when
 * it would come out different, and @changed says whether it was. */
static bool romm_lib_write_playlist(const char *path, const char *db_name,
      const char *const *db_names,
      const char *default_core_path, const char *default_core_name,
      const romm_rom_t *roms, size_t count, bool *changed)
{
   romm_lib_game_core_t *game_cores;
   romm_lib_display_t display;
   rjsonwriter_t *writer;
   const char *buf;
   int buf_len    = 0;
   bool ok        = false;
   size_t written = 0;
   size_t i;

   /* RomM's title order unless the user picked another. */
   display.label_display_mode   = LABEL_DISPLAY_MODE_DEFAULT;
   display.right_thumbnail_mode = PLAYLIST_THUMBNAIL_MODE_DEFAULT;
   display.left_thumbnail_mode  = PLAYLIST_THUMBNAIL_MODE_DEFAULT;
   display.sort_mode            = PLAYLIST_SORT_MODE_OFF;
   game_cores = romm_lib_read_playlist(path, &display);

   if (!(writer = rjsonwriter_open_memory()))
   {
      romm_lib_free_game_cores(game_cores);
      return false;
   }

   /* The format playlist_write_file() writes, version 1.5. Covers are
    * named after the file. */
   rjsonwriter_raw(writer, "{\n", 2);
   romm_lib_json_member(writer, 2, "version", "1.5", false);
   romm_lib_json_member(writer, 2, "default_core_path",
         default_core_path ? default_core_path : "", false);
   romm_lib_json_member(writer, 2, "default_core_name",
         default_core_name ? default_core_name : "", false);
   romm_lib_json_member_int(writer, "label_display_mode",
         display.label_display_mode);
   romm_lib_json_member_int(writer, "right_thumbnail_mode",
         display.right_thumbnail_mode);
   romm_lib_json_member_int(writer, "left_thumbnail_mode",
         display.left_thumbnail_mode);
   romm_lib_json_member_int(writer, "thumbnail_match_mode",
         PLAYLIST_THUMBNAIL_MATCH_MODE_WITH_FILENAME);
   romm_lib_json_member_int(writer, "sort_mode", display.sort_mode);
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "items");
   rjsonwriter_raw(writer, ": [\n", 4);

   for (i = 0; i < count; i++)
   {
      char content_path[PATH_MAX_LENGTH];
      char crc32[16];
      romm_lib_game_core_t key;
      const romm_lib_game_core_t *core = NULL;

      if (!romm_library_content_path(content_path, sizeof(content_path),
            &roms[i]))
         continue;
      key.id = roms[i].id;
      if (game_cores)
         core = (const romm_lib_game_core_t*)bsearch(&key, game_cores,
               RBUF_LEN(game_cores), sizeof(*game_cores),
               romm_lib_cmp_game_core);
      crc32[0] = '\0';
      if (roms[i].single_file && *roms[i].crc)
      {
         size_t _len = strlcpy(crc32, roms[i].crc, sizeof(crc32));
         string_to_upper(crc32);
         strlcpy(crc32 + _len, "|crc", sizeof(crc32) - _len);
      }

      if (written++)
         rjsonwriter_raw(writer, ",\n", 2);
      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_raw(writer, "{\n", 2);
      romm_lib_json_member(writer, 6, "path", content_path, false);
      romm_lib_json_member(writer, 6, "label", roms[i].name, false);
      romm_lib_json_member(writer, 6, "core_path",
            core ? core->core_path : FILE_PATH_DETECT, false);
      romm_lib_json_member(writer, 6, "core_name",
            core ? core->core_name : FILE_PATH_DETECT, false);
      romm_lib_json_member(writer, 6, "crc32", crc32, false);
      romm_lib_json_member(writer, 6, "db_name",
            db_names ? db_names[i] : db_name, true);
      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_raw(writer, "}", 1);
   }

   rjsonwriter_raw(writer, "\n  ]\n}\n", 7);

   if ((buf = rjsonwriter_get_memory_buffer(writer, &buf_len)))
   {
      void *old     = NULL;
      int64_t len   = 0;
      bool same     = changed
         && path_is_valid(path)
         && filestream_read_file(path, &old, &len)
         && len == buf_len
         && !memcmp(old, buf, (size_t)buf_len);
      free(old);
      ok = same || filestream_write_file_atomic(path, buf, buf_len);
      if (changed)
         *changed = ok && !same;
   }
   rjsonwriter_free(writer);
   romm_lib_free_game_cores(game_cores);
   return ok;
}

bool romm_library_write_playlist(const char *path, const char *db_name,
      const char *default_core_path, const char *default_core_name,
      const romm_rom_t *roms, size_t count)
{
   return romm_lib_write_playlist(path, db_name, NULL, default_core_path,
         default_core_name, roms, count, NULL);
}

bool romm_library_listed(const char *list, const char *name)
{
   size_t len;

   if (!list || string_is_empty(name))
      return false;
   len = strlen(name);
   while (*list)
   {
      const char *end;
      const char *last;
      while (*list == ',' || *list == ' ')
         list++;
      for (end = list; *end && *end != ','; end++) { }
      for (last = end; last > list && last[-1] == ' '; last--) { }
      if ((size_t)(last - list) == len && !strncmp(list, name, len))
         return true;
      list = end;
   }
   return false;
}

size_t romm_library_manifest_path(char *s, size_t len,
      const char *playlist_dir)
{
   char dir[PATH_MAX_LENGTH];
   fill_pathname_join(dir, playlist_dir, "romm", sizeof(dir));
   return fill_pathname_join(s, dir, "library.json", len);
}

typedef struct
{
   romm_library_t *lib;
   romm_library_platform_t cur;
   romm_rom_t rom;
   romm_library_collection_t collection;
   /* The platform the last game was added to. */
   size_t last;
} romm_lib_load_t;

static void romm_lib_load_platform_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_library_platform_t *p = &((romm_lib_load_t*)ctx)->cur;

   if (string_is_equal(key, "id"))
      p->platform.id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "rom_count"))
      p->platform.rom_count = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "playlist_size"))
      p->playlist_size = (int64_t)strtoll(value, NULL, 10);
   else if (string_is_equal(key, "playlist_mtime"))
      p->playlist_mtime = (int64_t)strtoll(value, NULL, 10);
   else if (string_is_equal(key, "slug"))
      strlcpy(p->platform.slug, value, sizeof(p->platform.slug));
   else if (string_is_equal(key, "fs_slug"))
      strlcpy(p->platform.fs_slug, value, sizeof(p->platform.fs_slug));
   else if (string_is_equal(key, "name"))
      strlcpy(p->platform.name, value, sizeof(p->platform.name));
   else if (string_is_equal(key, "libretro_slug"))
      strlcpy(p->platform.libretro_slug, value,
            sizeof(p->platform.libretro_slug));
   else if (string_is_equal(key, "family_name"))
      strlcpy(p->platform.family_name, value,
            sizeof(p->platform.family_name));
   else if (string_is_equal(key, "playlist"))
      strlcpy(p->playlist, value, sizeof(p->playlist));
}

static void romm_lib_load_platform_end(void *ctx)
{
   romm_lib_load_t *load = (romm_lib_load_t*)ctx;

   if (load->cur.platform.id && *load->cur.playlist)
      RBUF_PUSH(load->lib->platforms, load->cur);
   else if (load->cur.platform.id)
      RBUF_PUSH(load->lib->unsynced, load->cur.platform);
   memset(&load->cur, 0, sizeof(load->cur));
}

static void romm_lib_load_rom_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_rom_t *r = &((romm_lib_load_t*)ctx)->rom;

   if (string_is_equal(key, "id"))
      r->id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "platform_id"))
      r->platform_id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "single_file"))
      r->single_file = type == RJSON_TRUE;
   else if (string_is_equal(key, "crc"))
      strlcpy(r->crc, value, sizeof(r->crc));
   else if (string_is_equal(key, "fs_name"))
      strlcpy(r->fs_name, value, sizeof(r->fs_name));
   else if (string_is_equal(key, "fs_name_no_ext"))
      strlcpy(r->fs_name_no_ext, value, sizeof(r->fs_name_no_ext));
   else if (string_is_equal(key, "name"))
      strlcpy(r->name, value, sizeof(r->name));
   else if (string_is_equal(key, "cover"))
      strlcpy(r->cover, value, sizeof(r->cover));
   else if (string_is_equal(key, "updated_at"))
      strlcpy(r->updated_at, value, sizeof(r->updated_at));
}

static void romm_lib_load_rom_end(void *ctx)
{
   romm_lib_load_t *load = (romm_lib_load_t*)ctx;
   romm_library_platform_t *platforms = load->lib->platforms;
   size_t count = RBUF_LEN(platforms);
   size_t i;

   if (load->rom.id && *load->rom.fs_name)
   {
      /* Games come grouped by platform. */
      if (     load->last >= count
            || platforms[load->last].platform.id != load->rom.platform_id)
      {
         for (i = 0; i < count; i++)
            if (platforms[i].platform.id == load->rom.platform_id)
               break;
         load->last = i;
      }
      if (load->last < count)
         RBUF_PUSH(platforms[load->last].roms, load->rom);
   }
   memset(&load->rom, 0, sizeof(load->rom));
}

static void romm_lib_load_collection_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_library_collection_t *c = &((romm_lib_load_t*)ctx)->collection;

   if (string_is_equal(key, "smart"))
      c->smart = type == RJSON_TRUE;
   else if (string_is_equal(key, "id"))
      c->id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "name"))
      strlcpy(c->name, value, sizeof(c->name));
   else if (string_is_equal(key, "playlist"))
      strlcpy(c->playlist, value, sizeof(c->playlist));
}

static void romm_lib_load_collection_end(void *ctx)
{
   romm_lib_load_t *load = (romm_lib_load_t*)ctx;

   if (load->collection.id && *load->collection.playlist)
      RBUF_PUSH(load->lib->collections, load->collection);
   memset(&load->collection, 0, sizeof(load->collection));
}

bool romm_library_load(romm_library_t *lib, const char *path)
{
   romm_lib_load_t *load;
   char *doc   = NULL;
   int64_t len = 0;
   bool ok     = false;
   char num[32];

   memset(lib, 0, sizeof(*lib));
   if (     !path_is_valid(path)
         || !filestream_read_file(path, (void**)&doc, &len)
         || !doc)
      return false;
   if (!(load = (romm_lib_load_t*)calloc(1, sizeof(*load))))
   {
      free(doc);
      return false;
   }
   load->lib = lib;

   if (     romm_json_get(doc, (size_t)len, "version", num, sizeof(num))
         && strtoul(num, NULL, 10) == ROMM_LIBRARY_VERSION
         && romm_json_get(doc, (size_t)len, "server",
               lib->server, sizeof(lib->server))
         && romm_json_get(doc, (size_t)len, "synced_at", num, sizeof(num))
         && romm_json_items(doc, (size_t)len, "platforms",
               romm_lib_load_platform_member, romm_lib_load_platform_end,
               load)
         && romm_json_items(doc, (size_t)len, "roms",
               romm_lib_load_rom_member, romm_lib_load_rom_end, load))
   {
      lib->synced_at = (int64_t)strtoll(num, NULL, 10);
      ok             = true;
      /* A manifest from before collections has none. */
      romm_json_items(doc, (size_t)len, "collections",
            romm_lib_load_collection_member, romm_lib_load_collection_end,
            load);
      if (romm_json_get(doc, (size_t)len, "favorites_id", num, sizeof(num)))
         lib->favorites_id = (unsigned)strtoul(num, NULL, 10);
   }

   free(load);
   free(doc);
   if (!ok)
      romm_library_free(lib);
   return ok;
}

/* One entry of the manifest's platforms; a platform not synced has no
 * playlist. */
static void romm_lib_save_platform(rjsonwriter_t *writer, bool first,
      const romm_platform_t *platform, const char *playlist,
      int64_t playlist_size, int64_t playlist_mtime)
{
   if (!first)
      rjsonwriter_raw(writer, ",\n", 2);
   rjsonwriter_add_spaces(writer, 4);
   rjsonwriter_rawf(writer, "{\"id\": %u, \"rom_count\": %u, "
         "\"playlist_size\": %lld, \"playlist_mtime\": %lld, ",
         platform->id, platform->rom_count,
         (long long)playlist_size, (long long)playlist_mtime);
   rjsonwriter_add_string(writer, "slug");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, platform->slug);
   rjsonwriter_raw(writer, ", ", 2);
   rjsonwriter_add_string(writer, "fs_slug");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, platform->fs_slug);
   rjsonwriter_raw(writer, ", ", 2);
   rjsonwriter_add_string(writer, "name");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, platform->name);
   rjsonwriter_raw(writer, ", ", 2);
   rjsonwriter_add_string(writer, "libretro_slug");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, platform->libretro_slug);
   rjsonwriter_raw(writer, ", ", 2);
   rjsonwriter_add_string(writer, "family_name");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, platform->family_name);
   rjsonwriter_raw(writer, ", ", 2);
   rjsonwriter_add_string(writer, "playlist");
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, playlist);
   rjsonwriter_raw(writer, "}", 1);
}

bool romm_library_save(const romm_library_t *lib, const char *path)
{
   rjsonwriter_t *writer;
   const char *buf;
   char dir[PATH_MAX_LENGTH];
   int buf_len    = 0;
   bool ok        = false;
   size_t written = 0;
   size_t i, j;

   fill_pathname_basedir(dir, path, sizeof(dir));
   if (!path_is_directory(dir) && !path_mkdir(dir))
      return false;
   if (!(writer = rjsonwriter_open_memory()))
      return false;

   rjsonwriter_raw(writer, "{\n", 2);
   romm_lib_json_member_int(writer, "version", ROMM_LIBRARY_VERSION);
   romm_lib_json_member(writer, 2, "server", lib->server, false);
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "synced_at");
   rjsonwriter_rawf(writer, ": %lld,\n", (long long)lib->synced_at);
   romm_lib_json_member_int(writer, "favorites_id", (int)lib->favorites_id);

   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "platforms");
   rjsonwriter_raw(writer, ": [\n", 4);
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      romm_lib_save_platform(writer, !i, &lib->platforms[i].platform,
            lib->platforms[i].playlist, lib->platforms[i].playlist_size,
            lib->platforms[i].playlist_mtime);
   for (i = 0; i < RBUF_LEN(lib->unsynced); i++)
      romm_lib_save_platform(writer, !i && !RBUF_LEN(lib->platforms),
            &lib->unsynced[i], "", 0, 0);
   rjsonwriter_raw(writer, "\n  ],\n", 6);

   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "collections");
   rjsonwriter_raw(writer, ": [", 3);
   for (i = 0; i < RBUF_LEN(lib->collections); i++)
   {
      const romm_library_collection_t *c = &lib->collections[i];
      rjsonwriter_raw(writer, i ? ",\n" : "\n", i ? 2 : 1);
      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_rawf(writer, "{\"id\": %u, \"smart\": %s, ", c->id,
            c->smart ? "true" : "false");
      rjsonwriter_add_string(writer, "name");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, c->name);
      rjsonwriter_raw(writer, ", ", 2);
      rjsonwriter_add_string(writer, "playlist");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, c->playlist);
      rjsonwriter_raw(writer, "}", 1);
   }
   rjsonwriter_raw(writer, RBUF_LEN(lib->collections) ? "\n  ],\n" : "],\n",
         RBUF_LEN(lib->collections) ? 6 : 3);

   /* One line per game keeps the file readable at thousands of games. */
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "roms");
   rjsonwriter_raw(writer, ": [\n", 4);
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
   {
      const romm_library_platform_t *p = &lib->platforms[i];
      for (j = 0; j < RBUF_LEN(p->roms); j++)
      {
         const romm_rom_t *r = &p->roms[j];
         if (written++)
            rjsonwriter_raw(writer, ",\n", 2);
         rjsonwriter_add_spaces(writer, 4);
         rjsonwriter_rawf(writer, "{\"id\": %u, \"platform_id\": %u, "
               "\"single_file\": %s, ", r->id, p->platform.id,
               r->single_file ? "true" : "false");
         rjsonwriter_add_string(writer, "crc");
         rjsonwriter_raw(writer, ": ", 2);
         rjsonwriter_add_string(writer, r->crc);
         rjsonwriter_raw(writer, ", ", 2);
         rjsonwriter_add_string(writer, "fs_name");
         rjsonwriter_raw(writer, ": ", 2);
         rjsonwriter_add_string(writer, r->fs_name);
         rjsonwriter_raw(writer, ", ", 2);
         rjsonwriter_add_string(writer, "fs_name_no_ext");
         rjsonwriter_raw(writer, ": ", 2);
         rjsonwriter_add_string(writer, r->fs_name_no_ext);
         rjsonwriter_raw(writer, ", ", 2);
         rjsonwriter_add_string(writer, "name");
         rjsonwriter_raw(writer, ": ", 2);
         rjsonwriter_add_string(writer, r->name);
         rjsonwriter_raw(writer, ", ", 2);
         rjsonwriter_add_string(writer, "cover");
         rjsonwriter_raw(writer, ": ", 2);
         rjsonwriter_add_string(writer, r->cover);
         rjsonwriter_raw(writer, ", ", 2);
         rjsonwriter_add_string(writer, "updated_at");
         rjsonwriter_raw(writer, ": ", 2);
         rjsonwriter_add_string(writer, r->updated_at);
         rjsonwriter_raw(writer, "}", 1);
      }
   }
   rjsonwriter_raw(writer, "\n  ]\n}\n", 7);

   if ((buf = rjsonwriter_get_memory_buffer(writer, &buf_len)))
      ok = filestream_write_file_atomic(path, buf, buf_len);
   rjsonwriter_free(writer);
   return ok;
}

void romm_library_free(romm_library_t *lib)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      RBUF_FREE(lib->platforms[i].roms);
   RBUF_FREE(lib->platforms);
   RBUF_FREE(lib->collections);
   RBUF_FREE(lib->favorite_ids);
   RBUF_FREE(lib->waiting);
   RBUF_FREE(lib->unsynced);
   RBUF_FREE(lib->added);
   memset(lib, 0, sizeof(*lib));
}

/* Writes @name from every platform in @lib it belongs to: RomM can have
 * two platforms with one libretro name, such as Genesis and Mega
 * Drive. */
static bool romm_lib_write_named_playlist(const romm_library_t *lib,
      const char *playlist_dir, const char *name,
      const char *core_path, const char *core_name)
{
   char path[PATH_MAX_LENGTH];
   const romm_library_platform_t *only = NULL;
   romm_rom_t *merged = NULL;
   bool ok;
   size_t i, j;

   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
   {
      const romm_library_platform_t *p = &lib->platforms[i];
      if (!string_is_equal(p->playlist, name))
         continue;
      if (!only)
      {
         only = p;
         continue;
      }
      if (!merged)
         for (j = 0; j < RBUF_LEN(only->roms); j++)
            RBUF_PUSH(merged, only->roms[j]);
      for (j = 0; j < RBUF_LEN(p->roms); j++)
         RBUF_PUSH(merged, p->roms[j]);
   }
   if (!only)
      return false;

   fill_pathname_join(path, playlist_dir, name, sizeof(path));
   ok = merged
      ? romm_library_write_playlist(path, name, core_path, core_name,
            merged, RBUF_LEN(merged))
      : romm_library_write_playlist(path, name, core_path, core_name,
            only->roms, RBUF_LEN(only->roms));
   RBUF_FREE(merged);
   return ok;
}

static bool romm_lib_has_playlist(const romm_library_t *lib,
      const char *name)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      if (string_is_equal(lib->platforms[i].playlist, name))
         return true;
   return false;
}

/* Does @lib write playlist @name, for a platform or a collection? */
static bool romm_lib_playlist_taken(const romm_library_t *lib,
      const char *name)
{
   size_t i;
   if (romm_lib_has_playlist(lib, name))
      return true;
   for (i = 0; i < RBUF_LEN(lib->collections); i++)
      if (string_is_equal(lib->collections[i].playlist, name))
         return true;
   return false;
}

static void romm_lib_sync_remove(romm_lib_sync_t *sync, const char *name)
{
   char path[PATH_MAX_LENGTH];

   fill_pathname_join(path, sync->playlist_dir, name, sizeof(path));
   if (path_is_valid(path) && !filestream_delete(path))
   {
      RARCH_LOG(ROMM_LIB_PFX "Removed %s, no longer synced.\n", name);
      if (sync->playlist_changed)
         sync->playlist_changed(path);
   }
}

/* Deletes the playlists the last sync wrote that this one didn't. */
static void romm_lib_sync_remove_stale(romm_lib_sync_t *sync)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(sync->old.platforms); i++)
      if (!romm_lib_playlist_taken(&sync->lib,
            sync->old.platforms[i].playlist))
         romm_lib_sync_remove(sync, sync->old.platforms[i].playlist);
   for (i = 0; i < RBUF_LEN(sync->old.collections); i++)
      if (!romm_lib_playlist_taken(&sync->lib,
            sync->old.collections[i].playlist))
         romm_lib_sync_remove(sync, sync->old.collections[i].playlist);
}

/* @lib's entry for platform @id, if it wrote one. */
static const romm_library_platform_t *romm_lib_find_platform(
      const romm_library_t *lib, unsigned id)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      if (lib->platforms[i].platform.id == id)
         return &lib->platforms[i];
   return NULL;
}

/* Platform @id, if @lib lists it as not synced. */
static const romm_platform_t *romm_lib_find_unsynced(
      const romm_library_t *lib, unsigned id)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(lib->unsynced); i++)
      if (lib->unsynced[i].id == id)
         return &lib->unsynced[i];
   return NULL;
}

/* A sync that stopped part way has rewritten some playlists; the
 * manifest records those and keeps the rest from the last sync, so
 * the playlists stay accounted for. */
static void romm_lib_sync_save_partial(romm_lib_sync_t *sync)
{
   size_t i;

   if (!RBUF_LEN(sync->lib.platforms))
      return;
   for (i = 0; i < RBUF_LEN(sync->old.platforms); i++)
   {
      if (romm_lib_has_playlist(&sync->lib, sync->old.platforms[i].playlist))
         continue;
      RBUF_PUSH(sync->lib.platforms, sync->old.platforms[i]);
      sync->old.platforms[i].roms = NULL;
   }
   for (i = 0; i < RBUF_LEN(sync->old.collections); i++)
      if (!romm_lib_playlist_taken(&sync->lib,
            sync->old.collections[i].playlist))
         RBUF_PUSH(sync->lib.collections, sync->old.collections[i]);
   for (i = 0; i < RBUF_LEN(sync->old.unsynced); i++)
      if (     !romm_lib_find_platform(&sync->lib, sync->old.unsynced[i].id)
            && !romm_lib_find_unsynced(&sync->lib,
                  sync->old.unsynced[i].id))
         RBUF_PUSH(sync->lib.unsynced, sync->old.unsynced[i]);
   if (!sync->lib.favorites_listed)
      sync->lib.favorites_id = sync->old.favorites_id;
   strlcpy(sync->lib.server, sync->server, sizeof(sync->lib.server));
   sync->lib.synced_at = sync->old.synced_at;
   romm_library_save(&sync->lib, sync->manifest);
}

static void romm_lib_covers_free(romm_lib_cover_t *jobs)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(jobs); i++)
   {
      free(jobs[i].url_path);
      free(jobs[i].dest);
   }
   RBUF_FREE(jobs);
}

/* RetroArch picks an image decoder by extension, and RomM serves JPEG
 * covers under .png names. */
static const char *romm_lib_cover_ext(const void *data, size_t len)
{
   const unsigned char *b = (const unsigned char*)data;
   if (len >= 8 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8))
      return ".png";
   if (len >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
      return ".jpg";
   return NULL;
}

static const char *const romm_lib_cover_exts[] = { ".png", ".jpg" };

/* Is @dest, as .png or .jpg, on disk? */
static bool romm_lib_cover_exists(const char *dest)
{
   char path[PATH_MAX_LENGTH];
   size_t i;
   for (i = 0; i < ARRAY_SIZE(romm_lib_cover_exts); i++)
   {
      fill_pathname(path, dest, romm_lib_cover_exts[i], sizeof(path));
      if (path_is_valid(path))
         return true;
   }
   return false;
}

/* Deletes @dest as .png and .jpg, except as @keep_ext. */
static void romm_lib_cover_delete(const char *dest, const char *keep_ext)
{
   char path[PATH_MAX_LENGTH];
   size_t i;
   for (i = 0; i < ARRAY_SIZE(romm_lib_cover_exts); i++)
   {
      if (keep_ext && string_is_equal(keep_ext, romm_lib_cover_exts[i]))
         continue;
      fill_pathname(path, dest, romm_lib_cover_exts[i], sizeof(path));
      if (path_is_valid(path))
         filestream_delete(path);
   }
}

static int romm_lib_cmp_str(const void *a, const void *b)
{
   return strcmp(*(char* const*)a, *(char* const*)b);
}

static int romm_lib_cmp_rom_id(const void *a, const void *b)
{
   unsigned x = (*(const romm_rom_t* const*)a)->id;
   unsigned y = (*(const romm_rom_t* const*)b)->id;
   return x < y ? -1 : x > y;
}

static int romm_lib_cmp_cover_dest(const void *a, const void *b)
{
   const romm_lib_cover_t *x = *(const romm_lib_cover_t* const*)a;
   const romm_lib_cover_t *y = *(const romm_lib_cover_t* const*)b;
   int c                     = strcmp(x->dest, y->dest);
   return c ? c : (x < y ? -1 : x > y);
}

/* Keeps one job per cover file, the first: games can share one, such
 * as a PS1 game's folder and .rar copies, and two downloads to one file
 * race, the later deleting the earlier when their formats differ. */
static void romm_lib_covers_dedupe(romm_lib_cover_t **jobs)
{
   romm_lib_cover_t **sorted = NULL;
   const char *dest          = NULL;
   size_t count              = RBUF_LEN(*jobs);
   size_t i, j;

   if (count < 2)
      return;
   for (i = 0; i < count; i++)
      RBUF_PUSH(sorted, &(*jobs)[i]);
   if (RBUF_LEN(sorted) != count)
   {
      RBUF_FREE(sorted);
      return;
   }
   qsort(sorted, count, sizeof(*sorted), romm_lib_cmp_cover_dest);
   for (i = 0; i < count; i++)
   {
      if (dest && string_is_equal(sorted[i]->dest, dest))
      {
         free(sorted[i]->url_path);
         sorted[i]->url_path = NULL;
      }
      else
         dest = sorted[i]->dest;
   }
   RBUF_FREE(sorted);

   for (i = j = 0; i < count; i++)
   {
      if ((*jobs)[i].url_path)
         (*jobs)[j++] = (*jobs)[i];
      else
         free((*jobs)[i].dest);
   }
   RBUF_RESIZE(*jobs, j);
}

/* The next game of @lib from the cover planning's place in it, with
 * its platform; NULL after the last. */
static const romm_rom_t *romm_lib_next_game(romm_lib_sync_t *sync,
      const romm_library_t *lib, const romm_library_platform_t **platform)
{
   while (sync->cover_platform < RBUF_LEN(lib->platforms))
   {
      const romm_library_platform_t *p =
            &lib->platforms[sync->cover_platform];
      if (sync->cover_game < RBUF_LEN(p->roms))
      {
         *platform = p;
         return &p->roms[sync->cover_game++];
      }
      sync->cover_platform++;
      sync->cover_game = 0;
   }
   return NULL;
}

/* Is platform @id one this sync is for? */
static bool romm_lib_sync_only(const romm_lib_sync_t *sync, unsigned id)
{
   size_t i;
   if (!sync->only)
      return true;
   for (i = 0; i < RBUF_LEN(sync->only_ids); i++)
      if (sync->only_ids[i] == id)
         return true;
   return false;
}

/* Lists @rom's cover, in @p's folder, and plans fetching it unless it
 * is on disk and unchanged in RomM. */
static void romm_lib_plan_cover(romm_lib_sync_t *sync,
      const romm_library_platform_t *p, const romm_rom_t *rom)
{
   const romm_rom_t **old;
   romm_lib_cover_t job;
   char dest[PATH_MAX_LENGTH];
   char *query;
   char *dest_copy;

   if (     !*rom->cover
         || !romm_library_cover_path(dest, sizeof(dest),
               sync->thumbnails_dir, p->playlist, rom)
         || !(dest_copy = strdup(dest)))
      return;
   RBUF_PUSH(sync->cover_dests, dest_copy);
   if (!romm_lib_sync_only(sync, p->platform.id))
      return;

   old = sync->cover_old ? (const romm_rom_t**)bsearch(&rom,
         sync->cover_old, RBUF_LEN(sync->cover_old),
         sizeof(*sync->cover_old), romm_lib_cmp_rom_id) : NULL;
   if (     romm_lib_cover_exists(dest)
         && old && string_is_equal((*old)->cover, rom->cover)
         && !string_is_equal(p->playlist, sync->refetch))
      return;

   /* Only a cache buster, with a space in it. */
   job.url_path = strdup(rom->cover);
   job.dest     = strdup(dest);
   if (!job.url_path || !job.dest)
   {
      free(job.url_path);
      free(job.dest);
      return;
   }
   if ((query = strchr(job.url_path, '?')))
      *query = '\0';
   RBUF_PUSH(sync->covers, job);
}

/* Deletes the cover of @rom, which the last sync listed in @p, unless
 * this sync lists it. */
static void romm_lib_drop_cover(romm_lib_sync_t *sync,
      const romm_library_platform_t *p, const romm_rom_t *rom)
{
   char dest[PATH_MAX_LENGTH];
   char *key = dest;

   if (     !*rom->cover
         || !romm_library_cover_path(dest, sizeof(dest),
               sync->thumbnails_dir, p->playlist, rom))
      return;
   if (!sync->cover_dests || !bsearch(&key, sync->cover_dests,
         RBUF_LEN(sync->cover_dests), sizeof(*sync->cover_dests),
         romm_lib_cmp_str))
      romm_lib_cover_delete(dest, NULL);
}

/* Over the limit, drops the covers just planned for the platform being
 * planned. Its games keep the covers the last sync saw, or none, so
 * the next sync still finds them changed. */
static void romm_lib_limit_covers(romm_lib_sync_t *sync)
{
   size_t count = RBUF_LEN(sync->covers) - sync->cover_from;
   romm_library_platform_t *p;
   romm_library_waiting_t waiting;
   size_t i;

   if (     !sync->limit || count <= sync->limit
         || sync->cover_limited >= RBUF_LEN(sync->lib.platforms))
   {
      sync->cover_from = RBUF_LEN(sync->covers);
      return;
   }
   for (i = sync->cover_from; i < RBUF_LEN(sync->covers); i++)
   {
      free(sync->covers[i].url_path);
      free(sync->covers[i].dest);
   }
   RBUF_RESIZE(sync->covers, sync->cover_from);

   p = &sync->lib.platforms[sync->cover_limited];
   for (i = 0; i < RBUF_LEN(p->roms); i++)
   {
      const romm_rom_t *rom = &p->roms[i];
      const romm_rom_t **old = sync->cover_old
         ? (const romm_rom_t**)bsearch(&rom, sync->cover_old,
               RBUF_LEN(sync->cover_old), sizeof(*sync->cover_old),
               romm_lib_cmp_rom_id)
         : NULL;
      strlcpy(p->roms[i].cover, old ? (*old)->cover : "",
            sizeof(p->roms[i].cover));
   }

   RARCH_LOG(ROMM_LIB_PFX "%s: %u covers to fetch, more than %u; left for "
         "a manual sync.\n", p->platform.name, (unsigned)count,
         (unsigned)sync->limit);
   for (i = 0; i < RBUF_LEN(sync->lib.waiting); i++)
      if (sync->lib.waiting[i].platform.id == p->platform.id)
      {
         sync->lib.waiting[i].covers += count;
         return;
      }
   waiting.platform = p->platform;
   waiting.games    = 0;
   waiting.covers   = count;
   RBUF_PUSH(sync->lib.waiting, waiting);
}

/* Lists the covers to fetch: those not on disk, and those RomM changed
 * since the last sync. Deletes the covers of games no longer listed,
 * or listed under another name. Each game is looked for on disk, so
 * this goes a slice of games per call; true once done. */
static bool romm_lib_sync_plan_covers(romm_lib_sync_t *sync)
{
   const romm_library_platform_t *p;
   const romm_rom_t *rom;
   unsigned budget = ROMM_LIBRARY_COVERS_PER_STEP;
   size_t i, j;

   if (!sync->cover_phase)
   {
      /* Covers an earlier sync is still fetching aren't on disk, so
       * this one plans them again. */
      if (romm_lib_covers_running)
      {
         RARCH_LOG(ROMM_LIB_PFX "Stopping the covers still being "
               "fetched; this sync fetches them.\n");
         task_queue_cancel_task(romm_lib_covers_running);
      }
      for (i = 0; i < RBUF_LEN(sync->old.platforms); i++)
         for (j = 0; j < RBUF_LEN(sync->old.platforms[i].roms); j++)
            RBUF_PUSH(sync->cover_old, &sync->old.platforms[i].roms[j]);
      if (sync->cover_old)
         qsort(sync->cover_old, RBUF_LEN(sync->cover_old),
               sizeof(*sync->cover_old), romm_lib_cmp_rom_id);
      sync->cover_phase = 1;
   }

   if (sync->cover_phase == 1)
   {
      while ((rom = romm_lib_next_game(sync, &sync->lib, &p)))
      {
         if (sync->cover_platform != sync->cover_limited)
         {
            romm_lib_limit_covers(sync);
            sync->cover_limited = sync->cover_platform;
         }
         romm_lib_plan_cover(sync, p, rom);
         if (!--budget)
            return false;
      }
      romm_lib_limit_covers(sync);
      romm_lib_covers_dedupe(&sync->covers);
      if (sync->cover_dests)
         qsort(sync->cover_dests, RBUF_LEN(sync->cover_dests),
               sizeof(*sync->cover_dests), romm_lib_cmp_str);
      sync->cover_platform = 0;
      sync->cover_game     = 0;
      sync->cover_phase    = 2;
   }

   if (sync->cover_phase == 2)
   {
      while ((rom = romm_lib_next_game(sync, &sync->old, &p)))
      {
         romm_lib_drop_cover(sync, p, rom);
         if (!--budget)
            return false;
      }
      sync->cover_phase = 3;
   }
   return true;
}

/* One download in flight. */
typedef struct
{
   romm_lib_covers_t *covers;
   size_t job;
} romm_lib_cover_req_t;

static void romm_lib_covers_download_cb(void *user_data,
      enum romm_status status, int http_status,
      const void *data, size_t len)
{
   romm_lib_cover_req_t *req = (romm_lib_cover_req_t*)user_data;
   romm_lib_covers_t *covers = req->covers;
   const char *dest          = covers->jobs[req->job].dest;
   const char *ext           = status == ROMM_OK
         ? romm_lib_cover_ext(data, len) : NULL;
   char path[PATH_MAX_LENGTH];
   char dir[PATH_MAX_LENGTH];
   bool ok                   = false;

   if (ext)
   {
      fill_pathname(path, dest, ext, sizeof(path));
      fill_pathname_basedir(dir, path, sizeof(dir));
      if (     path_mkdir(dir)
            && filestream_write_file_atomic(path, data, (int64_t)len))
      {
         /* RomM may have changed the cover's format. */
         romm_lib_cover_delete(dest, ext);
         ok = true;
      }
   }
   if (!ok)
   {
      RARCH_WARN(ROMM_LIB_PFX "No cover for %s (status %d, HTTP %d).\n",
            dest, status, http_status);
      covers->failed++;
   }
   covers->in_flight--;
   covers->done++;
   covers->task->when = 0;
   free(req);
}

/* Runs on the main thread, like the download callbacks. */
static void romm_lib_covers_handler(retro_task_t *task)
{
   romm_lib_covers_t *covers = (romm_lib_covers_t*)task->state;
   size_t count              = RBUF_LEN(covers->jobs);
   bool cancelled            = (task_get_flags(task)
         & RETRO_TASK_FLG_CANCELLED) != 0;

   while (     !cancelled
         && covers->in_flight < ROMM_LIBRARY_COVERS_AT_ONCE
         && covers->next < count)
   {
      romm_lib_cover_req_t *req = (romm_lib_cover_req_t*)
            malloc(sizeof(*req));
      if (req)
      {
         req->covers = covers;
         req->job    = covers->next;
      }
      covers->next++;
      if (req && romm_client_download(covers->server, covers->token,
            covers->jobs[req->job].url_path,
            romm_lib_covers_download_cb, req))
         covers->in_flight++;
      else
      {
         free(req);
         covers->failed++;
         covers->done++;
      }
   }

   task_set_progress(task, (int8_t)(count ? covers->done * 100 / count : 100));
   /* Downloads still out hold @covers; finish once they answer. */
   if (!covers->in_flight && (cancelled || covers->done >= count))
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
   else
      task->when = cpu_features_get_time_usec() + 250000;
}

static void romm_lib_covers_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_lib_covers_t *covers = (romm_lib_covers_t*)user_data;
   RARCH_LOG(ROMM_LIB_PFX "Fetched %u of %u covers in %u ms.\n",
         (unsigned)(covers->done - covers->failed),
         (unsigned)RBUF_LEN(covers->jobs),
         (unsigned)((cpu_features_get_time_usec() - covers->started)
            / 1000));
}

static void romm_lib_covers_cleanup(retro_task_t *task)
{
   romm_lib_covers_t *covers = (romm_lib_covers_t*)task->state;

   if (romm_lib_covers_running == task)
      romm_lib_covers_running = NULL;
   if (covers)
   {
      romm_lib_covers_free(covers->jobs);
      if (covers->token)
      {
         memset(covers->token, 0, strlen(covers->token));
         free(covers->token);
      }
      free(covers);
   }
   task->state = NULL;
}

/* Starts fetching @jobs, which the task then owns. */
static bool romm_lib_fetch_covers(const char *server, const char *token,
      romm_lib_cover_t *jobs, const char *title)
{
   romm_lib_covers_t *covers;
   retro_task_t *task;

   if (!(covers = (romm_lib_covers_t*)calloc(1, sizeof(*covers))))
      return false;
   if (!(covers->token = strdup(token)) || !(task = task_init()))
   {
      free(covers->token);
      free(covers);
      return false;
   }
   covers->task    = task;
   covers->jobs    = jobs;
   covers->started = cpu_features_get_time_usec();
   strlcpy(covers->server, server, sizeof(covers->server));

   task->handler   = romm_lib_covers_handler;
   task->callback  = romm_lib_covers_cb;
   task->cleanup   = romm_lib_covers_cleanup;
   task->state     = covers;
   task->user_data = covers;
   task->flags    |= RETRO_TASK_FLG_MAIN_THREAD;
   if (title)
   {
      task->title    = strdup(title);
      task->progress = 0;
   }
   else
      task->flags   |= RETRO_TASK_FLG_MUTE;

   if (!task_queue_push(task))
   {
      free(task->title);
      free(task);
      free(covers->token);
      free(covers);
      return false;
   }
   romm_lib_covers_running = task;
   RARCH_LOG(ROMM_LIB_PFX "Covers to fetch: %u.\n", (unsigned)RBUF_LEN(jobs));
   return true;
}
const romm_rom_t **romm_library_index(const romm_library_t *lib)
{
   const romm_rom_t **index = NULL;
   size_t i, j;

   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      for (j = 0; j < RBUF_LEN(lib->platforms[i].roms); j++)
         RBUF_PUSH(index, &lib->platforms[i].roms[j]);
   if (index)
      qsort(index, RBUF_LEN(index), sizeof(*index), romm_lib_cmp_rom_id);
   return index;
}

static const romm_rom_t *romm_lib_index_find(const romm_rom_t *const *index,
      unsigned id)
{
   romm_rom_t key;
   const romm_rom_t *k = &key;
   const romm_rom_t *const *found;

   if (!index)
      return NULL;
   key.id = id;
   found  = (const romm_rom_t *const*)bsearch(&k, index, RBUF_LEN(index),
         sizeof(*index), romm_lib_cmp_rom_id);
   return found ? *found : NULL;
}

const romm_rom_t *romm_library_find(const romm_rom_t *const *index,
      unsigned id)
{
   return romm_lib_index_find(index, id);
}

unsigned romm_library_path_id(const char *path)
{
   char *end;
   unsigned long n;

   if (     !path || !string_starts_with(path, ROMM_SCHEME)
         || !ISDIGIT((unsigned char)path[STRLEN_CONST(ROMM_SCHEME)]))
      return 0;
   n = strtoul(path + STRLEN_CONST(ROMM_SCHEME), &end, 10);
   return *end == '/' ? (unsigned)n : 0;
}

bool romm_library_follow_game(const romm_rom_t *const *old,
      const romm_rom_t *const *lib, const char *path, const char *label,
      char *new_path, size_t path_len, char *new_label, size_t label_len)
{
   char was[PATH_MAX_LENGTH];
   const romm_rom_t *now, *before;
   char *end;
   unsigned long n;

   strlcpy(new_path, path, path_len);
   strlcpy(new_label, label, label_len);
   if (     !string_starts_with(path, ROMM_SCHEME)
         || !ISDIGIT((unsigned char)path[STRLEN_CONST(ROMM_SCHEME)]))
      return true;
   n = strtoul(path + STRLEN_CONST(ROMM_SCHEME), &end, 10);
   if (*end != '/')
      return true;
   if (!(now = romm_lib_index_find(lib, (unsigned)n)))
      return false;

   /* A path within the game, such as a disc unpacked from its zip,
    * is left as it is. */
   if (     (before = romm_lib_index_find(old, (unsigned)n))
         && romm_library_content_path(was, sizeof(was), before)
         && string_is_equal(was, path))
      romm_library_content_path(new_path, path_len, now);
   if (before && string_is_equal(label, before->name))
      strlcpy(new_label, now->name, label_len);
   return true;
}

/* The proleptic Gregorian date @z days after 1970-01-01. */
static void romm_lib_civil_from_days(long z, int *y, int *m, int *d)
{
   long era, doe, yoe, doy, mp;
   z  += 719468;
   era = (z >= 0 ? z : z - 146096) / 146097;
   doe = z - era * 146097;
   yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
   doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
   mp  = (5 * doy + 2) / 153;
   *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
   *m  = (int)(mp < 10 ? mp + 3 : mp - 9);
   *y  = (int)(yoe + era * 400 + (*m <= 2));
}

/* RomM's ISO 8601 time @at, less @margin seconds, as UTC for a URL:
 * "2026-10-03T00:22:20Z". */
static bool romm_lib_cursor(char *s, size_t len, const char *at,
      unsigned margin)
{
   int y, mo, d;
   int64_t t = romm_json_time(at);
   long days, secs;

   if (t < 0)
      return false;
   t   -= margin;
   days = (long)(t / 86400);
   secs = (long)(t % 86400);
   if (secs < 0)
   {
      secs += 86400;
      days--;
   }
   romm_lib_civil_from_days(days, &y, &mo, &d);
   return (size_t)snprintf(s, len, "%04d-%02d-%02dT%02ld:%02ld:%02ldZ",
         y, mo, d, secs / 3600, secs / 60 % 60, secs % 60) < len;
}

/* Lists the platform being synced as left for a later sync, with
 * @games to fetch. */
static void romm_lib_sync_wait(romm_lib_sync_t *sync, size_t games)
{
   romm_library_waiting_t waiting;

   waiting.platform = sync->remote[sync->platform];
   waiting.games    = games;
   waiting.covers   = 0;
   RBUF_PUSH(sync->lib.waiting, waiting);
   RARCH_LOG(ROMM_LIB_PFX "%s: %u games to fetch, more than %u; left for "
         "a manual sync.\n", waiting.platform.name, (unsigned)games,
         (unsigned)sync->limit);
}

/* Builds the platform's list in RomM's order from its ids: the games
 * that changed, fetched into @sync->roms, and the manifest's for the
 * rest. False when an id is in neither, such as a game the user can
 * see again, which needs a full fetch. */
static bool romm_lib_sync_merge(romm_lib_sync_t *sync)
{
   const romm_library_platform_t *old = sync->old_platform;
   const romm_rom_t **changed         = NULL;
   const romm_rom_t **known           = NULL;
   romm_rom_t *merged                 = NULL;
   bool ok                            = true;
   size_t i;

   for (i = 0; i < RBUF_LEN(sync->roms); i++)
      RBUF_PUSH(changed, &sync->roms[i]);
   for (i = 0; i < RBUF_LEN(old->roms); i++)
      RBUF_PUSH(known, &old->roms[i]);
   if (changed)
      qsort(changed, RBUF_LEN(changed), sizeof(*changed),
            romm_lib_cmp_rom_id);
   if (known)
      qsort(known, RBUF_LEN(known), sizeof(*known), romm_lib_cmp_rom_id);

   for (i = 0; ok && i < RBUF_LEN(sync->ids); i++)
   {
      romm_rom_t key;
      const romm_rom_t *k = &key;
      const romm_rom_t **found = NULL;

      key.id = sync->ids[i];
      if (changed)
         found = (const romm_rom_t**)bsearch(&k, changed,
               RBUF_LEN(changed), sizeof(*changed), romm_lib_cmp_rom_id);
      if (!found && known)
         found = (const romm_rom_t**)bsearch(&k, known,
               RBUF_LEN(known), sizeof(*known), romm_lib_cmp_rom_id);
      if (found)
         RBUF_PUSH(merged, **found);
      else
         ok = false;
   }

   RBUF_FREE(changed);
   RBUF_FREE(known);
   if (!ok)
   {
      RBUF_FREE(merged);
      return false;
   }
   RBUF_FREE(sync->roms);
   sync->roms = merged;
   return true;
}

/* Would @a and @b make the same playlist entries? */
static bool romm_lib_same_roms(const romm_rom_t *a, const romm_rom_t *b)
{
   size_t i;
   if (RBUF_LEN(a) != RBUF_LEN(b))
      return false;
   for (i = 0; i < RBUF_LEN(a); i++)
      if (     a[i].id != b[i].id
            || a[i].single_file != b[i].single_file
            || !string_is_equal(a[i].crc, b[i].crc)
            || !string_is_equal(a[i].fs_name, b[i].fs_name)
            || !string_is_equal(a[i].name, b[i].name))
         return false;
   return true;
}

static void romm_lib_sync_finish(romm_lib_sync_t *sync,
      enum romm_status status)
{
   sync->status    = status;
   sync->finished  = true;
   sync->task->when = 0;
}

/* A sync for some platforms: notes their ids, and starts the library
 * with the last sync's entries for the others, written first so a
 * playlist one shares with them keeps their games. */
static void romm_lib_sync_keep_others(romm_lib_sync_t *sync)
{
   size_t i;

   for (i = 0; i < RBUF_LEN(sync->remote); i++)
      if (romm_library_listed(sync->only, sync->remote[i].fs_slug))
         RBUF_PUSH(sync->only_ids, sync->remote[i].id);
   for (i = 0; i < RBUF_LEN(sync->old.platforms); i++)
   {
      romm_library_platform_t entry = sync->old.platforms[i];
      size_t j;
      if (romm_lib_sync_only(sync, entry.platform.id))
         continue;
      entry.roms = NULL;
      for (j = 0; j < RBUF_LEN(sync->old.platforms[i].roms); j++)
         RBUF_PUSH(entry.roms, sync->old.platforms[i].roms[j]);
      RBUF_PUSH(sync->lib.platforms, entry);
      RBUF_PUSH(sync->written, false);
   }
   for (i = 0; i < RBUF_LEN(sync->old.unsynced); i++)
      if (!romm_lib_sync_only(sync, sync->old.unsynced[i].id))
         RBUF_PUSH(sync->lib.unsynced, sync->old.unsynced[i]);
}

static void romm_lib_sync_platforms_cb(void *user_data,
      enum romm_status status, int http_status,
      const romm_platform_t *platforms, size_t count)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)user_data;
   size_t i;

   sync->pending    = false;
   sync->task->when = 0;
   if (status != ROMM_OK)
   {
      sync->http_status = http_status;
      romm_lib_sync_finish(sync, status);
      return;
   }
   for (i = 0; i < count; i++)
      RBUF_PUSH(sync->remote, platforms[i]);
   if (sync->only)
      romm_lib_sync_keep_others(sync);
   sync->state = ROMM_LIB_SYNC_NEXT;
}

static void romm_lib_sync_ids_cb(void *user_data,
      enum romm_status status, int http_status,
      const unsigned *ids, size_t count)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)user_data;
   size_t i;

   sync->pending    = false;
   sync->task->when = 0;
   if (status != ROMM_OK)
   {
      sync->http_status = http_status;
      romm_lib_sync_finish(sync, status);
      return;
   }
   for (i = 0; i < count; i++)
      RBUF_PUSH(sync->ids, ids[i]);
   if (sync->limit)
   {
      for (i = 0; i < RBUF_LEN(sync->old_platform->roms); i++)
         RBUF_PUSH(sync->known, &sync->old_platform->roms[i]);
      if (sync->known)
         qsort(sync->known, RBUF_LEN(sync->known), sizeof(*sync->known),
               romm_lib_cmp_rom_id);
   }
   sync->state = ROMM_LIB_SYNC_CHANGES;
}

static void romm_lib_sync_roms_cb(void *user_data,
      enum romm_status status, int http_status,
      const romm_rom_t *roms, size_t count, bool more)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)user_data;
   size_t i;

   sync->pending    = false;
   sync->task->when = 0;
   if (status != ROMM_OK)
   {
      sync->http_status = http_status;
      romm_lib_sync_finish(sync, status);
      return;
   }
   for (i = 0; i < count; i++)
      RBUF_PUSH(sync->roms, roms[i]);
   if (sync->limit && sync->state == ROMM_LIB_SYNC_CHANGES)
   {
      /* Not those the cursor's margin fetches again unchanged */
      for (i = 0; i < count; i++)
      {
         const romm_rom_t *rom    = &roms[i];
         const romm_rom_t **found = sync->known
            ? (const romm_rom_t**)bsearch(&rom, sync->known,
                  RBUF_LEN(sync->known), sizeof(*sync->known),
                  romm_lib_cmp_rom_id)
            : NULL;
         if (!found || !string_is_equal((*found)->updated_at,
               rom->updated_at))
            sync->changed++;
      }
      if (sync->changed > sync->limit)
      {
         sync->state = ROMM_LIB_SYNC_HOLD;
         return;
      }
   }
   if (more)
      sync->offset += ROMM_LIBRARY_PAGE;
   else if (sync->state == ROMM_LIB_SYNC_CHANGES)
      sync->state = ROMM_LIB_SYNC_MERGE;
   else
      sync->state = ROMM_LIB_SYNC_WRITE;
}

/* The default core the playlist at @path should have: the one it has,
 * which the user may have picked, unless it can't run the system, or
 * else the first installed of the system's preferred cores. @changed
 * is set when that differs from the file's. */
static void romm_lib_default_core(romm_lib_sync_t *sync, const char *path,
      const romm_platform_t *platform, char *core_path, size_t path_len,
      char *core_name, size_t name_len, bool *changed)
{
   const char *const *cores = romm_library_preferred_cores(
         platform->libretro_slug);
   char file_path[PATH_MAX_LENGTH];
   char *doc   = NULL;
   int64_t len = 0;

   file_path[0] = '\0';
   core_path[0] = '\0';
   core_name[0] = '\0';
   if (     path_is_valid(path)
         && filestream_read_file(path, (void**)&doc, &len) && doc)
   {
      romm_json_get(doc, (size_t)len, "default_core_path",
            file_path, sizeof(file_path));
      if (     *file_path && !string_is_equal(file_path, FILE_PATH_DETECT)
            && (   !sync->core_runs || !*platform->libretro_slug
                || sync->core_runs(file_path, platform->libretro_slug)))
      {
         strlcpy(core_path, file_path, path_len);
         romm_json_get(doc, (size_t)len, "default_core_name",
               core_name, name_len);
      }
   }
   free(doc);

   if (!*core_path && cores && sync->find_core
         && !sync->find_core(cores, core_path, path_len,
               core_name, name_len))
   {
      core_path[0] = '\0';
      core_name[0] = '\0';
   }
   *changed = !string_is_equal(core_path, file_path)
      && !(!*core_path && string_is_equal(file_path, FILE_PATH_DETECT));
}

/* The size of the file at @path, -1 when there is none, and its
 * modification time where the file system gives one. */
static void romm_lib_file_stamp(const char *path, int64_t *size,
      int64_t *mtime)
{
   *mtime = 0;
   if ((*size = path_get_size(path)) >= 0)
      path_get_mtime(path, mtime);
}

/* Records the playlist file @name, at @path, as this sync leaves it. */
static void romm_lib_stamp_playlist(romm_library_t *lib, const char *name,
      const char *path)
{
   int64_t size, mtime;
   size_t i;

   romm_lib_file_stamp(path, &size, &mtime);
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      if (string_is_equal(lib->platforms[i].playlist, name))
      {
         lib->platforms[i].playlist_size  = size;
         lib->platforms[i].playlist_mtime = mtime;
      }
}

/* Moves the fetched games into the library and, unless it would come
 * out as it is, writes their playlist; a platform with none the user
 * can see gets no playlist. */
static bool romm_lib_sync_write(romm_lib_sync_t *sync)
{
   const romm_library_platform_t *old = sync->old_platform;
   romm_library_platform_t entry;
   char path[PATH_MAX_LENGTH];
   char core_path[PATH_MAX_LENGTH];
   char core_name[NAME_MAX_LENGTH];
   int64_t size, mtime;
   bool core_changed;
   bool write;
   size_t i;

   if (!RBUF_LEN(sync->roms))
      return true;
   memset(&entry, 0, sizeof(entry));
   entry.platform = sync->remote[sync->platform];
   if (!romm_library_playlist_name(entry.playlist, sizeof(entry.playlist),
         &entry.platform))
   {
      RBUF_FREE(sync->roms);
      return true;
   }
   fill_pathname_join(path, sync->playlist_dir, entry.playlist,
         sizeof(path));
   romm_lib_default_core(sync, path, &entry.platform,
         core_path, sizeof(core_path), core_name, sizeof(core_name),
         &core_changed);
   /* A file changed since, such as one the menu wrote back from a stale
    * copy, may not list what the manifest does. */
   romm_lib_file_stamp(path, &size, &mtime);
   write = !old
      || string_is_equal(entry.playlist, sync->refetch)
      || core_changed
      || !string_is_equal(old->playlist, entry.playlist)
      || !romm_lib_same_roms(old->roms, sync->roms)
      || size < 0
      || size != old->playlist_size
      || mtime != old->playlist_mtime;
   /* A playlist shared with a platform written earlier in this sync
    * was written without this one's games. */
   for (i = 0; !write && i < RBUF_LEN(sync->lib.platforms); i++)
      write = sync->written[i] && string_is_equal(
            sync->lib.platforms[i].playlist, entry.playlist);

   entry.roms = sync->roms;
   sync->roms = NULL;
   RBUF_PUSH(sync->lib.platforms, entry);
   RBUF_PUSH(sync->written, write);
   if (write)
   {
      sync->rewritten++;
      if (!romm_lib_write_named_playlist(&sync->lib, sync->playlist_dir,
            entry.playlist, core_path, core_name))
         return false;
      if (sync->playlist_changed)
         sync->playlist_changed(path);
   }
   romm_lib_stamp_playlist(&sync->lib, entry.playlist, path);
   return true;
}

/* Does @lib list platform @id in playlist @name? */
static bool romm_lib_lists_platform(const romm_library_t *lib,
      unsigned id, const char *name)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      if (     lib->platforms[i].platform.id == id
            && string_is_equal(lib->platforms[i].playlist, name))
         return true;
   return false;
}

/* Rewrites each playlist this sync left alone that the last one also
 * wrote for a platform it no longer holds, such as Mega Drive with its
 * games all deleted while Genesis keeps the playlist: the file still
 * lists that platform's games. */
static bool romm_lib_sync_drop_gone(romm_lib_sync_t *sync)
{
   size_t i, j;

   for (i = 0; i < RBUF_LEN(sync->lib.platforms); i++)
   {
      const romm_library_platform_t *p = &sync->lib.platforms[i];
      char path[PATH_MAX_LENGTH];
      char core_path[PATH_MAX_LENGTH];
      char core_name[NAME_MAX_LENGTH];
      bool core_changed;
      bool gone = false;

      if (sync->written[i])
         continue;
      for (j = 0; !gone && j < RBUF_LEN(sync->old.platforms); j++)
         gone = string_is_equal(sync->old.platforms[j].playlist, p->playlist)
            && !romm_lib_lists_platform(&sync->lib,
                  sync->old.platforms[j].platform.id, p->playlist);
      if (!gone)
         continue;

      fill_pathname_join(path, sync->playlist_dir, p->playlist,
            sizeof(path));
      romm_lib_default_core(sync, path, &p->platform, core_path,
            sizeof(core_path), core_name, sizeof(core_name), &core_changed);
      sync->rewritten++;
      if (!romm_lib_write_named_playlist(&sync->lib, sync->playlist_dir,
            p->playlist, core_path, core_name))
         return false;
      for (j = 0; j < RBUF_LEN(sync->lib.platforms); j++)
         if (string_is_equal(sync->lib.platforms[j].playlist, p->playlist))
            sync->written[j] = true;
      if (sync->playlist_changed)
         sync->playlist_changed(path);
      romm_lib_stamp_playlist(&sync->lib, p->playlist, path);
   }
   return true;
}

static void romm_lib_sync_collections_cb(void *user_data,
      enum romm_status status, int http_status,
      const romm_collection_t *collections, size_t count)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)user_data;
   bool smart            = sync->state == ROMM_LIB_SYNC_SMART;
   size_t i;

   sync->pending    = false;
   sync->task->when = 0;
   /* Without one list, the last sync's playlists for it stay. */
   if (status == ROMM_OK)
   {
      for (i = 0; i < count; i++)
      {
         romm_collection_t c = collections[i];
         c.rom_ids           = NULL;
         if (RBUF_TRYFIT(c.rom_ids, RBUF_LEN(collections[i].rom_ids)))
         {
            RBUF_RESIZE(c.rom_ids, RBUF_LEN(collections[i].rom_ids));
            memcpy(c.rom_ids, collections[i].rom_ids,
                  RBUF_SIZEOF(collections[i].rom_ids));
         }
         RBUF_PUSH(sync->remote_collections, c);
      }
      if (smart)
         sync->smart_listed       = true;
      else
         sync->collections_listed = true;
   }
   sync->state = smart
      ? ROMM_LIB_SYNC_WRITE_COLLECTIONS : ROMM_LIB_SYNC_SMART;
}

static int romm_lib_cmp_rom_name(const void *a, const void *b)
{
   const romm_rom_t *x = (const romm_rom_t*)a;
   const romm_rom_t *y = (const romm_rom_t*)b;
   int c               = strcasecmp(x->name, y->name);
   return c ? c : (x->id > y->id) - (x->id < y->id);
}

const char *romm_library_rom_playlist(const romm_library_t *lib,
      const romm_rom_t *rom)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
      if (lib->platforms[i].platform.id == rom->platform_id)
         return lib->platforms[i].playlist;
   return "";
}

/* "Collection - <name>.lpl", with the owner's name and then the id
 * added when another playlist has that name. */
static bool romm_lib_collection_playlist(char *s, size_t len,
      const romm_library_t *lib, const romm_collection_t *c)
{
   int i;

   for (i = 0; i < 3; i++)
   {
      int _len;
      if (i == 0)
         _len = snprintf(s, len, "Collection - %s.lpl", c->name);
      else if (i == 1 && *c->owner)
         _len = snprintf(s, len, "Collection - %s (%s).lpl", c->name,
               c->owner);
      else
         _len = snprintf(s, len, "Collection - %s (%u).lpl", c->name,
               c->id);
      if (_len < 0 || (size_t)_len >= len)
         continue;
      romm_lib_scrub_file_name(s);
      if (!romm_lib_playlist_taken(lib, s))
         return true;
   }
   return false;
}

/* Writes a playlist for each collection RomM listed that holds games
 * in the library, in title order, each game's db_name its platform's
 * playlist. Collections of a list RomM didn't give keep theirs. */
static bool romm_lib_sync_write_collections(romm_lib_sync_t *sync)
{
   const romm_rom_t **index = romm_library_index(&sync->lib);
   bool ok                  = true;
   size_t i, j;

   for (i = 0; ok && i < RBUF_LEN(sync->remote_collections); i++)
   {
      const romm_collection_t *c = &sync->remote_collections[i];
      romm_library_collection_t entry;
      romm_rom_t *roms      = NULL;
      const char **db_names = NULL;
      char path[PATH_MAX_LENGTH];
      bool changed          = false;

      /* The user's favorites go to Favorites, not a playlist of their
       * own; another user's public favorites are left out. */
      if (c->favorite)
      {
         if (!c->is_public && !sync->lib.favorites_id)
         {
            sync->lib.favorites_id = c->id;
            for (j = 0; j < RBUF_LEN(c->rom_ids); j++)
               RBUF_PUSH(sync->lib.favorite_ids, c->rom_ids[j]);
         }
         continue;
      }
      for (j = 0; j < RBUF_LEN(c->rom_ids); j++)
      {
         const romm_rom_t *rom = romm_lib_index_find(index, c->rom_ids[j]);
         if (rom)
            RBUF_PUSH(roms, *rom);
      }
      memset(&entry, 0, sizeof(entry));
      entry.id    = c->id;
      entry.smart = c->smart;
      strlcpy(entry.name, c->name, sizeof(entry.name));
      /* Like a platform, one without games the user can see gets no
       * playlist. */
      if (     !RBUF_LEN(roms)
            || !romm_lib_collection_playlist(entry.playlist,
                  sizeof(entry.playlist), &sync->lib, c))
      {
         RBUF_FREE(roms);
         continue;
      }

      qsort(roms, RBUF_LEN(roms), sizeof(*roms), romm_lib_cmp_rom_name);
      for (j = 0; j < RBUF_LEN(roms); j++)
         RBUF_PUSH(db_names, romm_library_rom_playlist(&sync->lib, &roms[j]));
      fill_pathname_join(path, sync->playlist_dir, entry.playlist,
            sizeof(path));
      if (string_is_equal(entry.playlist, sync->refetch))
         changed = true;
      if ((ok = romm_lib_write_playlist(path, NULL, db_names, NULL, NULL,
            roms, RBUF_LEN(roms), changed ? NULL : &changed)))
      {
         RBUF_PUSH(sync->lib.collections, entry);
         if (changed)
         {
            sync->rewritten++;
            if (sync->playlist_changed)
               sync->playlist_changed(path);
         }
      }
      RBUF_FREE(db_names);
      RBUF_FREE(roms);
   }
   RBUF_FREE(index);

   sync->lib.favorites_listed = sync->collections_listed;

   for (i = 0; ok && i < RBUF_LEN(sync->old.collections); i++)
   {
      const romm_library_collection_t *c = &sync->old.collections[i];
      if (     !(c->smart ? sync->smart_listed : sync->collections_listed)
            && !romm_lib_playlist_taken(&sync->lib, c->playlist))
         RBUF_PUSH(sync->lib.collections, *c);
   }
   return ok;
}

/* Is @p, which an emulator runs and isn't switched off, new since the
 * last sync, to start off? A platform keeps its folder name when
 * RomM's database is rebuilt and the ids change. */
static bool romm_lib_sync_new(const romm_lib_sync_t *sync,
      const romm_platform_t *p)
{
   size_t i;
   if (     !sync->new_off || !sync->had_old || sync->only
         || romm_lib_find_platform(&sync->old, p->id)
         || romm_lib_find_unsynced(&sync->old, p->id))
      return false;
   if (*p->fs_slug)
   {
      for (i = 0; i < RBUF_LEN(sync->old.platforms); i++)
         if (string_is_equal(sync->old.platforms[i].platform.fs_slug,
               p->fs_slug))
            return false;
      for (i = 0; i < RBUF_LEN(sync->old.unsynced); i++)
         if (string_is_equal(sync->old.unsynced[i].fs_slug, p->fs_slug))
            return false;
   }
   return true;
}

/* Is @p left out of this sync? No emulator runs a platform without a
 * libretro name. */
static bool romm_lib_sync_off(const romm_lib_sync_t *sync,
      const romm_platform_t *p)
{
   return !*p->libretro_slug
      || romm_library_listed(sync->exclude, p->fs_slug)
      || romm_lib_sync_new(sync, p);
}

/* Runs on the main thread (RETRO_TASK_FLG_MAIN_THREAD), like the
 * request callbacks, so they share @sync without locking. */
static void romm_lib_sync_handler(retro_task_t *task)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)task->state;
   size_t count          = RBUF_LEN(sync->remote);
   size_t i;

   /* A request still out holds @sync; its callback wakes the task. */
   if (sync->pending)
   {
      task->when = cpu_features_get_time_usec() + 250000;
      return;
   }
   if (!sync->finished && (task_get_flags(task) & RETRO_TASK_FLG_CANCELLED))
      romm_lib_sync_finish(sync, ROMM_ERR_CANCELLED);
   if (sync->finished)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   task->when = 0;
   switch (sync->state)
   {
      case ROMM_LIB_SYNC_PLATFORMS:
         sync->had_old = romm_library_load(&sync->old, sync->manifest);
         if (romm_client_platforms(sync->server, sync->token,
               romm_lib_sync_platforms_cb, sync))
            sync->pending = true;
         else
            romm_lib_sync_finish(sync, ROMM_ERR_UNREACHABLE);
         break;
      case ROMM_LIB_SYNC_NEXT:
         for (; sync->platform < count; sync->platform++)
         {
            const romm_platform_t *p = &sync->remote[sync->platform];
            /* The others are in the library as the last sync left them. */
            if (!p->rom_count || !romm_lib_sync_only(sync, p->id))
               continue;
            if (!romm_lib_sync_off(sync, p))
               break;
            RBUF_PUSH(sync->lib.unsynced, *p);
            if (     *p->libretro_slug
                  && !romm_library_listed(sync->exclude, p->fs_slug)
                  && romm_lib_sync_new(sync, p))
            {
               RARCH_LOG(ROMM_LIB_PFX "%s is new in RomM; left off until "
                     "switched on.\n", p->name);
               RBUF_PUSH(sync->lib.added, *p);
            }
         }
         if (sync->platform >= count)
         {
            sync->state = sync->collections
               ? ROMM_LIB_SYNC_COLLECTIONS : ROMM_LIB_SYNC_COVERS;
            break;
         }
         task_set_progress(task, (int8_t)(sync->platform * 100 / count));
         RBUF_CLEAR(sync->ids);
         RBUF_CLEAR(sync->known);
         RBUF_FREE(sync->roms);
         sync->changed      = 0;
         sync->offset       = 0;
         sync->state        = ROMM_LIB_SYNC_ROMS;
         sync->old_platform = romm_lib_find_platform(&sync->old,
               sync->remote[sync->platform].id);
         if (*sync->refetch)
         {
            char playlist[256];
            if (     romm_library_playlist_name(playlist, sizeof(playlist),
                        &sync->remote[sync->platform])
                  && string_is_equal(playlist, sync->refetch))
               break;
         }
         if (sync->old_platform)
         {
            const romm_library_platform_t *old = sync->old_platform;
            const char *newest = "";
            for (i = 0; i < RBUF_LEN(old->roms); i++)
               if (strcmp(old->roms[i].updated_at, newest) > 0)
                  newest = old->roms[i].updated_at;
            if (romm_lib_cursor(sync->cursor, sizeof(sync->cursor),
                  newest, ROMM_LIBRARY_CURSOR_MARGIN))
               sync->state = ROMM_LIB_SYNC_IDS;
         }
         else if (   sync->limit
                  && sync->remote[sync->platform].rom_count > sync->limit)
         {
            romm_lib_sync_wait(sync,
                  sync->remote[sync->platform].rom_count);
            RBUF_PUSH(sync->lib.unsynced, sync->remote[sync->platform]);
            sync->platform++;
            sync->state = ROMM_LIB_SYNC_NEXT;
         }
         break;
      case ROMM_LIB_SYNC_IDS:
         if (romm_client_rom_ids(sync->server, sync->token,
               sync->remote[sync->platform].id, romm_lib_sync_ids_cb, sync))
            sync->pending = true;
         else
            romm_lib_sync_finish(sync, ROMM_ERR_UNREACHABLE);
         break;
      case ROMM_LIB_SYNC_MERGE:
         if (romm_lib_sync_merge(sync))
            sync->state = ROMM_LIB_SYNC_WRITE;
         else
         {
            RBUF_FREE(sync->roms);
            sync->offset = 0;
            sync->state  = ROMM_LIB_SYNC_ROMS;
         }
         break;
      case ROMM_LIB_SYNC_CHANGES:
      case ROMM_LIB_SYNC_ROMS:
         if (romm_client_roms(sync->server, sync->token,
               sync->remote[sync->platform].id,
               sync->state == ROMM_LIB_SYNC_CHANGES ? sync->cursor : NULL,
               sync->offset, ROMM_LIBRARY_PAGE, romm_lib_sync_roms_cb, sync))
            sync->pending = true;
         else
            romm_lib_sync_finish(sync, ROMM_ERR_UNREACHABLE);
         break;
      case ROMM_LIB_SYNC_HOLD:
         romm_lib_sync_wait(sync, sync->changed);
         RBUF_FREE(sync->roms);
         for (i = 0; i < RBUF_LEN(sync->old_platform->roms); i++)
            RBUF_PUSH(sync->roms, sync->old_platform->roms[i]);
         sync->state = ROMM_LIB_SYNC_WRITE;
         break;
      case ROMM_LIB_SYNC_WRITE:
         if (!romm_lib_sync_write(sync))
         {
            RARCH_WARN(ROMM_LIB_PFX "Couldn't write the playlist for %s.\n",
                  sync->remote[sync->platform].name);
            romm_lib_sync_finish(sync, ROMM_ERR_WRITE);
            break;
         }
         sync->platform++;
         sync->state = ROMM_LIB_SYNC_NEXT;
         break;
      case ROMM_LIB_SYNC_COLLECTIONS:
      case ROMM_LIB_SYNC_SMART:
         if (romm_client_collections(sync->server, sync->token,
               sync->state == ROMM_LIB_SYNC_SMART,
               romm_lib_sync_collections_cb, sync))
            sync->pending = true;
         else
            sync->state = sync->state == ROMM_LIB_SYNC_SMART
               ? ROMM_LIB_SYNC_WRITE_COLLECTIONS : ROMM_LIB_SYNC_SMART;
         break;
      case ROMM_LIB_SYNC_WRITE_COLLECTIONS:
         if (!romm_lib_sync_write_collections(sync))
         {
            RARCH_WARN(ROMM_LIB_PFX "Couldn't write a collection's "
                  "playlist.\n");
            romm_lib_sync_finish(sync, ROMM_ERR_WRITE);
            break;
         }
         sync->state = ROMM_LIB_SYNC_COVERS;
         break;
      case ROMM_LIB_SYNC_COVERS:
         if (!*sync->thumbnails_dir || romm_lib_sync_plan_covers(sync))
            sync->state = ROMM_LIB_SYNC_SAVE;
         break;
      case ROMM_LIB_SYNC_SAVE:
         if (!romm_lib_sync_drop_gone(sync))
         {
            RARCH_WARN(ROMM_LIB_PFX "Couldn't rewrite a playlist shared "
                  "with a platform gone from it.\n");
            romm_lib_sync_finish(sync, ROMM_ERR_WRITE);
            break;
         }
         romm_lib_sync_remove_stale(sync);
         if (!sync->lib.favorites_listed)
            sync->lib.favorites_id = sync->old.favorites_id;
         strlcpy(sync->lib.server, sync->server, sizeof(sync->lib.server));
         sync->lib.synced_at = (int64_t)time(NULL);
         romm_lib_sync_finish(sync,
               romm_library_save(&sync->lib, sync->manifest)
               ? ROMM_OK : ROMM_ERR_WRITE);
         break;
   }
}

static void romm_lib_sync_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)user_data;
   size_t games          = 0;
   size_t i;

   if (sync->status != ROMM_OK)
      romm_lib_sync_save_partial(sync);
   else if (RBUF_LEN(sync->covers)
         && romm_lib_fetch_covers(sync->server, sync->token, sync->covers,
               sync->covers_title))
      sync->covers = NULL;
   for (i = 0; i < RBUF_LEN(sync->lib.platforms); i++)
      games += RBUF_LEN(sync->lib.platforms[i].roms);
   if (sync->status == ROMM_OK)
      RARCH_LOG(ROMM_LIB_PFX "Library synced: %u platforms, %u games, "
            "%u collections, %u playlists rewritten, in %u ms.\n",
            (unsigned)RBUF_LEN(sync->lib.platforms), (unsigned)games,
            (unsigned)RBUF_LEN(sync->lib.collections),
            sync->rewritten, (unsigned)((cpu_features_get_time_usec()
               - sync->started) / 1000));
   else
      RARCH_WARN(ROMM_LIB_PFX "Library sync ended (status %d).\n",
            sync->status);

   if (sync->cb)
      sync->cb(sync->user_data, sync->status, sync->http_status,
            sync->status == ROMM_OK ? &sync->old : NULL, &sync->lib);
}

static void romm_lib_sync_cleanup(retro_task_t *task)
{
   romm_lib_sync_t *sync = (romm_lib_sync_t*)task->state;

   if (sync)
   {
      size_t i;
      for (i = 0; i < RBUF_LEN(sync->cover_dests); i++)
         free(sync->cover_dests[i]);
      RBUF_FREE(sync->cover_dests);
      RBUF_FREE(sync->cover_old);
      RBUF_FREE(sync->known);
      romm_library_free(&sync->old);
      romm_library_free(&sync->lib);
      romm_lib_covers_free(sync->covers);
      for (i = 0; i < RBUF_LEN(sync->remote_collections); i++)
         RBUF_FREE(sync->remote_collections[i].rom_ids);
      RBUF_FREE(sync->remote_collections);
      RBUF_FREE(sync->remote);
      RBUF_FREE(sync->roms);
      RBUF_FREE(sync->ids);
      RBUF_FREE(sync->written);
      free(sync->covers_title);
      free(sync->exclude);
      free(sync->only);
      RBUF_FREE(sync->only_ids);
      if (sync->token)
      {
         memset(sync->token, 0, strlen(sync->token));
         free(sync->token);
      }
      free(sync);
   }
   task->state = NULL;
}

bool romm_library_sync(const romm_library_sync_params_t *params,
      romm_library_sync_cb_t cb, void *user_data)
{
   const char *server         = params->server;
   const char *token          = params->token;
   const char *playlist_dir   = params->playlist_dir;
   const char *thumbnails_dir = params->thumbnails_dir;
   const char *title          = params->title;
   const char *covers_title   = params->covers_title;
   romm_lib_sync_t *sync;
   retro_task_t *task;

   if (     string_is_empty(server) || string_is_empty(token)
         || string_is_empty(playlist_dir))
      return false;
   if (!(sync = (romm_lib_sync_t*)calloc(1, sizeof(*sync))))
      return false;
   if (!(sync->token = strdup(token)) || !(task = task_init()))
   {
      free(sync->token);
      free(sync);
      return false;
   }

   sync->cb        = cb;
   sync->user_data = user_data;
   sync->task      = task;
   sync->find_core = params->find_core;
   sync->playlist_changed = params->playlist_changed;
   sync->collections = params->collections;
   if (params->refetch)
      strlcpy(sync->refetch, params->refetch, sizeof(sync->refetch));
   if (!string_is_empty(params->exclude))
      sync->exclude = strdup(params->exclude);
   sync->new_off     = params->new_off;
   if (!string_is_empty(params->only))
      sync->only = strdup(params->only);
   sync->core_runs   = params->core_runs;
   sync->limit       = params->limit;
   sync->started   = cpu_features_get_time_usec();
   strlcpy(sync->server, server, sizeof(sync->server));
   strlcpy(sync->playlist_dir, playlist_dir, sizeof(sync->playlist_dir));
   if (thumbnails_dir)
      strlcpy(sync->thumbnails_dir, thumbnails_dir,
            sizeof(sync->thumbnails_dir));
   if (covers_title)
      sync->covers_title = strdup(covers_title);
   romm_library_manifest_path(sync->manifest, sizeof(sync->manifest),
         playlist_dir);

   task->handler   = romm_lib_sync_handler;
   task->callback  = romm_lib_sync_cb;
   task->cleanup   = romm_lib_sync_cleanup;
   task->state     = sync;
   task->user_data = sync;
   task->flags    |= RETRO_TASK_FLG_MAIN_THREAD;
   if (title)
   {
      task->title    = strdup(title);
      task->progress = 0;
   }
   else
      task->flags   |= RETRO_TASK_FLG_MUTE;

   if (!task_queue_push(task))
   {
      free(task->title);
      free(task);
      free(sync->covers_title);
      free(sync->exclude);
      free(sync->only);
      free(sync->token);
      free(sync);
      return false;
   }
   RARCH_LOG(ROMM_LIB_PFX "Syncing the library from %s.\n", server);
   return true;
}
