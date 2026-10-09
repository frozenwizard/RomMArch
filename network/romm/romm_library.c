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

#include <array/rbuf.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/rjson.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "romm_json.h"
#include "romm_library.h"
#include "../../file_path_special.h"
#include "../../playlist.h"

#define ROMM_LIBRARY_VERSION 1

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

bool romm_library_write_playlist(const char *path, const char *db_name,
      const char *default_core_path, const char *default_core_name,
      const romm_rom_t *roms, size_t count)
{
   rjsonwriter_t *writer;
   const char *buf;
   int buf_len    = 0;
   bool ok        = false;
   size_t written = 0;
   size_t i;

   if (!(writer = rjsonwriter_open_memory()))
      return false;

   /* The format playlist_write_file() writes, version 1.5. Covers are
    * named after the file, and RomM's title order is kept. */
   rjsonwriter_raw(writer, "{\n", 2);
   romm_lib_json_member(writer, 2, "version", "1.5", false);
   romm_lib_json_member(writer, 2, "default_core_path",
         default_core_path ? default_core_path : "", false);
   romm_lib_json_member(writer, 2, "default_core_name",
         default_core_name ? default_core_name : "", false);
   romm_lib_json_member_int(writer, "label_display_mode",
         LABEL_DISPLAY_MODE_DEFAULT);
   romm_lib_json_member_int(writer, "right_thumbnail_mode",
         PLAYLIST_THUMBNAIL_MODE_DEFAULT);
   romm_lib_json_member_int(writer, "left_thumbnail_mode",
         PLAYLIST_THUMBNAIL_MODE_DEFAULT);
   romm_lib_json_member_int(writer, "thumbnail_match_mode",
         PLAYLIST_THUMBNAIL_MATCH_MODE_WITH_FILENAME);
   romm_lib_json_member_int(writer, "sort_mode", PLAYLIST_SORT_MODE_OFF);
   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "items");
   rjsonwriter_raw(writer, ": [\n", 4);

   for (i = 0; i < count; i++)
   {
      char content_path[PATH_MAX_LENGTH];
      char crc32[16];

      if (!romm_library_content_path(content_path, sizeof(content_path),
            &roms[i]))
         continue;
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
      romm_lib_json_member(writer, 6, "core_path", FILE_PATH_DETECT, false);
      romm_lib_json_member(writer, 6, "core_name", FILE_PATH_DETECT, false);
      romm_lib_json_member(writer, 6, "crc32", crc32, false);
      romm_lib_json_member(writer, 6, "db_name", db_name, true);
      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_raw(writer, "}", 1);
   }

   rjsonwriter_raw(writer, "\n  ]\n}\n", 7);

   if ((buf = rjsonwriter_get_memory_buffer(writer, &buf_len)))
      ok = filestream_write_file_atomic(path, buf, buf_len);
   rjsonwriter_free(writer);
   return ok;
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
   else if (string_is_equal(key, "slug"))
      strlcpy(p->platform.slug, value, sizeof(p->platform.slug));
   else if (string_is_equal(key, "name"))
      strlcpy(p->platform.name, value, sizeof(p->platform.name));
   else if (string_is_equal(key, "libretro_slug"))
      strlcpy(p->platform.libretro_slug, value,
            sizeof(p->platform.libretro_slug));
   else if (string_is_equal(key, "playlist"))
      strlcpy(p->playlist, value, sizeof(p->playlist));
}

static void romm_lib_load_platform_end(void *ctx)
{
   romm_lib_load_t *load = (romm_lib_load_t*)ctx;

   if (load->cur.platform.id && *load->cur.playlist)
      RBUF_PUSH(load->lib->platforms, load->cur);
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
   }

   free(load);
   free(doc);
   if (!ok)
      romm_library_free(lib);
   return ok;
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

   rjsonwriter_add_spaces(writer, 2);
   rjsonwriter_add_string(writer, "platforms");
   rjsonwriter_raw(writer, ": [\n", 4);
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
   {
      const romm_library_platform_t *p = &lib->platforms[i];
      if (i)
         rjsonwriter_raw(writer, ",\n", 2);
      rjsonwriter_add_spaces(writer, 4);
      rjsonwriter_rawf(writer, "{\"id\": %u, \"rom_count\": %u, ",
            p->platform.id, p->platform.rom_count);
      rjsonwriter_add_string(writer, "slug");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, p->platform.slug);
      rjsonwriter_raw(writer, ", ", 2);
      rjsonwriter_add_string(writer, "name");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, p->platform.name);
      rjsonwriter_raw(writer, ", ", 2);
      rjsonwriter_add_string(writer, "libretro_slug");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, p->platform.libretro_slug);
      rjsonwriter_raw(writer, ", ", 2);
      rjsonwriter_add_string(writer, "playlist");
      rjsonwriter_raw(writer, ": ", 2);
      rjsonwriter_add_string(writer, p->playlist);
      rjsonwriter_raw(writer, "}", 1);
   }
   rjsonwriter_raw(writer, "\n  ],\n", 6);

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
   memset(lib, 0, sizeof(*lib));
}
