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
#include <string.h>

#include <compat/strl.h>
#include <formats/rjson.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "romm_library.h"
#include "../../file_path_special.h"
#include "../../playlist.h"

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
