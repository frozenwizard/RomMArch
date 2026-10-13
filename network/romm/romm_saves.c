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
#include <compat/posix_string.h>
#include <compat/strl.h>
#include <file/file_path.h>
#include <formats/rjson.h>
#include <lrc_hash.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>

#include "romm_json.h"
#include "romm_saves.h"

#define ROMM_SAVES_VERSION 1

size_t romm_saves_record_path(char *s, size_t len, const char *save_dir)
{
   return fill_pathname_join(s, save_dir, "rommarch-saves.json", len);
}

void romm_saves_record_free(romm_saves_record_t *rec)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(rec->games); i++)
      RBUF_FREE(rec->games[i].files);
   RBUF_FREE(rec->games);
   memset(rec, 0, sizeof(*rec));
}

romm_saves_game_t *romm_saves_record_game(romm_saves_record_t *rec,
      unsigned rom_id, const char *emulator, bool create)
{
   romm_saves_game_t game;
   size_t i;

   for (i = 0; i < RBUF_LEN(rec->games); i++)
      if (     rec->games[i].rom_id == rom_id
            && string_is_equal(rec->games[i].emulator, emulator))
         return &rec->games[i];
   if (!create)
      return NULL;
   memset(&game, 0, sizeof(game));
   game.rom_id = rom_id;
   strlcpy(game.emulator, emulator, sizeof(game.emulator));
   RBUF_PUSH(rec->games, game);
   return &rec->games[RBUF_LEN(rec->games) - 1];
}

romm_saves_file_t *romm_saves_game_file(romm_saves_game_t *game,
      const char *suffix, bool create)
{
   romm_saves_file_t file;
   size_t i;

   for (i = 0; i < RBUF_LEN(game->files); i++)
      if (string_is_equal(game->files[i].suffix, suffix))
         return &game->files[i];
   if (!create)
      return NULL;
   memset(&file, 0, sizeof(file));
   strlcpy(file.suffix, suffix, sizeof(file.suffix));
   RBUF_PUSH(game->files, file);
   return &game->files[RBUF_LEN(game->files) - 1];
}

typedef struct
{
   romm_saves_record_t *rec;
   romm_saves_game_t game;
   romm_saves_file_t file;
   unsigned file_rom_id;
   char file_emulator[64];
} romm_saves_load_t;

static void romm_saves_load_game_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_saves_game_t *g = &((romm_saves_load_t*)ctx)->game;

   if (string_is_equal(key, "rom_id"))
      g->rom_id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "emulator"))
      strlcpy(g->emulator, value, sizeof(g->emulator));
   else if (string_is_equal(key, "pending"))
      g->pending = type == RJSON_TRUE;
   else if (string_is_equal(key, "save_dir"))
      strlcpy(g->save_dir, value, sizeof(g->save_dir));
   else if (string_is_equal(key, "state_dir"))
      strlcpy(g->state_dir, value, sizeof(g->state_dir));
   else if (string_is_equal(key, "base"))
      strlcpy(g->base, value, sizeof(g->base));
}

static void romm_saves_load_game_end(void *ctx)
{
   romm_saves_load_t *load = (romm_saves_load_t*)ctx;

   if (     load->game.rom_id && *load->game.emulator
         && !romm_saves_record_game(load->rec, load->game.rom_id,
               load->game.emulator, false))
      RBUF_PUSH(load->rec->games, load->game);
   memset(&load->game, 0, sizeof(load->game));
}

static void romm_saves_load_file_member(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len)
{
   romm_saves_load_t *load = (romm_saves_load_t*)ctx;
   romm_saves_file_t *f    = &load->file;

   if (string_is_equal(key, "rom_id"))
      load->file_rom_id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "emulator"))
      strlcpy(load->file_emulator, value, sizeof(load->file_emulator));
   else if (string_is_equal(key, "suffix"))
      strlcpy(f->suffix, value, sizeof(f->suffix));
   else if (string_is_equal(key, "remote_id"))
      f->remote_id = (unsigned)strtoul(value, NULL, 10);
   else if (string_is_equal(key, "remote_time"))
      f->remote_time = (int64_t)strtoll(value, NULL, 10);
   else if (string_is_equal(key, "md5"))
      strlcpy(f->md5, value, sizeof(f->md5));
}

static void romm_saves_load_file_end(void *ctx)
{
   romm_saves_load_t *load = (romm_saves_load_t*)ctx;
   romm_saves_game_t *game = romm_saves_record_game(load->rec,
         load->file_rom_id, load->file_emulator, false);

   if (game && *load->file.suffix)
      RBUF_PUSH(game->files, load->file);
   memset(&load->file, 0, sizeof(load->file));
   load->file_rom_id      = 0;
   load->file_emulator[0] = '\0';
}

bool romm_saves_record_load(romm_saves_record_t *rec, const char *path)
{
   romm_saves_load_t *load;
   char *doc   = NULL;
   int64_t len = 0;
   bool ok     = false;
   char num[32];

   memset(rec, 0, sizeof(*rec));
   if (     !path_is_valid(path)
         || !filestream_read_file(path, (void**)&doc, &len)
         || !doc)
      return false;
   if (!(load = (romm_saves_load_t*)calloc(1, sizeof(*load))))
   {
      free(doc);
      return false;
   }
   load->rec = rec;

   ok =     romm_json_get(doc, (size_t)len, "version", num, sizeof(num))
         && strtoul(num, NULL, 10) == ROMM_SAVES_VERSION
         && romm_json_items(doc, (size_t)len, "games",
               romm_saves_load_game_member, romm_saves_load_game_end, load)
         && romm_json_items(doc, (size_t)len, "files",
               romm_saves_load_file_member, romm_saves_load_file_end, load);

   free(load);
   free(doc);
   if (!ok)
      romm_saves_record_free(rec);
   return ok;
}

static void romm_saves_json_string(rjsonwriter_t *writer, const char *key,
      const char *value)
{
   rjsonwriter_raw(writer, ", ", 2);
   rjsonwriter_add_string(writer, key);
   rjsonwriter_raw(writer, ": ", 2);
   rjsonwriter_add_string(writer, value);
}

bool romm_saves_record_save(const romm_saves_record_t *rec,
      const char *path)
{
   rjsonwriter_t *writer;
   const char *buf;
   int buf_len    = 0;
   bool ok        = false;
   size_t written = 0;
   size_t i, j;

   if (!(writer = rjsonwriter_open_memory()))
      return false;

   rjsonwriter_rawf(writer, "{\n  \"version\": %d,\n  \"games\": [\n",
         ROMM_SAVES_VERSION);
   for (i = 0; i < RBUF_LEN(rec->games); i++)
   {
      const romm_saves_game_t *g = &rec->games[i];
      if (i)
         rjsonwriter_raw(writer, ",\n", 2);
      rjsonwriter_rawf(writer, "    {\"rom_id\": %u", g->rom_id);
      romm_saves_json_string(writer, "emulator", g->emulator);
      rjsonwriter_rawf(writer, ", \"pending\": %s",
            g->pending ? "true" : "false");
      romm_saves_json_string(writer, "save_dir", g->save_dir);
      romm_saves_json_string(writer, "state_dir", g->state_dir);
      romm_saves_json_string(writer, "base", g->base);
      rjsonwriter_raw(writer, "}", 1);
   }
   rjsonwriter_raw(writer, "\n  ],\n  \"files\": [\n", 19);
   for (i = 0; i < RBUF_LEN(rec->games); i++)
   {
      const romm_saves_game_t *g = &rec->games[i];
      for (j = 0; j < RBUF_LEN(g->files); j++)
      {
         const romm_saves_file_t *f = &g->files[j];
         if (written++)
            rjsonwriter_raw(writer, ",\n", 2);
         rjsonwriter_rawf(writer, "    {\"rom_id\": %u", g->rom_id);
         romm_saves_json_string(writer, "emulator", g->emulator);
         romm_saves_json_string(writer, "suffix", f->suffix);
         rjsonwriter_rawf(writer, ", \"remote_id\": %u, "
               "\"remote_time\": %lld", f->remote_id,
               (long long)f->remote_time);
         romm_saves_json_string(writer, "md5", f->md5);
         rjsonwriter_raw(writer, "}", 1);
      }
   }
   rjsonwriter_raw(writer, "\n  ]\n}\n", 7);

   if ((buf = rjsonwriter_get_memory_buffer(writer, &buf_len)))
      ok = filestream_write_file_atomic(path, buf, buf_len);
   rjsonwriter_free(writer);
   return ok;
}

/* Days from 1970-01-01 to @y-@m-@d in the proleptic Gregorian
 * calendar. */
static int64_t romm_saves_days(int y, int m, int d)
{
   int era, yoe, doy, doe;

   y  -= m <= 2;
   era = (y >= 0 ? y : y - 399) / 400;
   yoe = y - era * 400;
   doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
   doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   return (int64_t)era * 146097 + doe - 719468;
}

int64_t romm_saves_time(const char *iso)
{
   int y, mo, d, h, mi, s, n = 0;
   int64_t t;
   const char *p;

   if (     !iso
         || sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d%n",
               &y, &mo, &d, &h, &mi, &s, &n) != 6
         || n != 19
         || mo < 1 || mo > 12 || d < 1 || d > 31
         || h > 23 || mi > 59 || s > 60)
      return -1;
   t = romm_saves_days(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
   p = iso + n;
   if (*p == '.')
      for (p++; *p >= '0' && *p <= '9'; p++);
   if ((*p == '+' || *p == '-') && strlen(p) == 6 && p[3] == ':')
   {
      int oh = (p[1] - '0') * 10 + (p[2] - '0');
      int om = (p[4] - '0') * 10 + (p[5] - '0');
      t     -= (*p == '+' ? 1 : -1) * (oh * 3600 + om * 60);
   }
   else if (!string_is_equal(p, "Z") && *p)
      return -1;
   return t;
}

const char *romm_saves_suffix(const char *name, const char *base)
{
   size_t base_len = strlen(base);
   const char *suffix, *c;
   unsigned dots   = 0;

   if (!base_len || strncasecmp(name, base, base_len))
      return NULL;
   suffix = name + base_len;
   if (*suffix != '.' || strlen(suffix) >= 64)
      return NULL;
   for (c = suffix; *c; c++)
   {
      if (*c == ' ')
         return NULL;
      if (*c == '.' && ++dots > 3)
         return NULL;
   }
   if (     string_ends_with_size(suffix, ".png", strlen(suffix), 4)
         || string_ends_with_size(suffix, ".tmp", strlen(suffix), 4)
         || c[-1] == '.')
      return NULL;
   return suffix;
}

bool romm_saves_md5_file(const char *path, char *md5)
{
   static const char hex[] = "0123456789abcdef";
   unsigned char digest[16];
   MD5_CTX ctx;
   void *buf   = NULL;
   int64_t len = 0;
   int i;

   if (!filestream_read_file(path, &buf, &len))
      return false;
   MD5_Init(&ctx);
   MD5_Update(&ctx, buf, (unsigned long)len);
   MD5_Final(digest, &ctx);
   free(buf);
   for (i = 0; i < 16; i++)
   {
      md5[i * 2]     = hex[digest[i] >> 4];
      md5[i * 2 + 1] = hex[digest[i] & 15];
   }
   md5[32] = '\0';
   return true;
}

romm_saves_local_t *romm_saves_list_local(const char *dir,
      const char *base, bool states)
{
   romm_saves_local_t *list = NULL;
   struct RDIR *rdir;

   if (     string_is_empty(dir) || string_is_empty(base)
         || !(rdir = retro_opendir(dir)))
      return NULL;
   while (retro_readdir(rdir))
   {
      const char *name = retro_dirent_get_name(rdir);
      const char *suffix;
      romm_saves_local_t local;
      char path[2048];

      if (     retro_dirent_is_dir(rdir, NULL)
            || !(suffix = romm_saves_suffix(name, base))
            || string_starts_with(suffix, ".state") != states)
         continue;
      fill_pathname_join(path, dir, name, sizeof(path));
      strlcpy(local.suffix, suffix, sizeof(local.suffix));
      if (romm_saves_md5_file(path, local.md5))
         RBUF_PUSH(list, local);
   }
   retro_closedir(rdir);
   return list;
}
