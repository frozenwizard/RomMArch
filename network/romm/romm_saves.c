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
#include <file/file_path.h>
#include <formats/rjson.h>
#include <lrc_hash.h>
#include <retro_dirent.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <time/rtime.h>

#include "romm_json.h"
#include "romm_saves.h"
#include "../../verbosity.h"

#define ROMM_SAVES_VERSION 1
#define ROMM_SAVES_PFX "[RomM] "
/* RomM's slot for saves synced as they are played, as its web player
 * and Argosy use, and how many versions RomM keeps there. */
#define ROMM_SAVES_SLOT "autosave"
#define ROMM_SAVES_KEEP 10
/* Where a save not chosen in a conflict is kept; syncs pass it over. */
#define ROMM_SAVES_KEPT_SLOT "conflict"

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

static void romm_saves_md5(const void *data, size_t len, char *md5)
{
   static const char hex[] = "0123456789abcdef";
   unsigned char digest[16];
   MD5_CTX ctx;
   int i;

   MD5_Init(&ctx);
   MD5_Update(&ctx, data, (unsigned long)len);
   MD5_Final(digest, &ctx);
   for (i = 0; i < 16; i++)
   {
      md5[i * 2]     = hex[digest[i] >> 4];
      md5[i * 2 + 1] = hex[digest[i] & 15];
   }
   md5[32] = '\0';
}

bool romm_saves_md5_file(const char *path, char *md5)
{
   void *buf   = NULL;
   int64_t len = 0;

   if (!filestream_read_file(path, &buf, &len))
      return false;
   romm_saves_md5(buf, (size_t)len, md5);
   free(buf);
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

/* Where in @name @pattern is, its 'd's standing for any digit; NULL
 * if it's not there. */
static const char *romm_saves_find(const char *name, const char *pattern)
{
   const char *c;

   for (c = name; *c; c++)
   {
      size_t i;
      for (i = 0; pattern[i] && c[i]; i++)
         if (pattern[i] == 'd' ? !ISDIGIT((unsigned char)c[i])
               : c[i] != pattern[i])
            break;
      if (!pattern[i])
         return c;
   }
   return NULL;
}

/* Copies @name to @s without the time RomM adds to a save uploaded to
 * a slot, " [2026-10-12_21-18-25]". */
static void romm_saves_strip_tag(char *s, size_t len, const char *name)
{
   static const char pattern[] = " [dddd-dd-dd_dd-dd-dd]";
   const char *c;

   strlcpy(s, name, len);
   if ((c = romm_saves_find(name, pattern)))
   {
      size_t at = (size_t)(c - name);
      if (at < len)
         strlcpy(s + at, c + sizeof(pattern) - 1, len - at);
   }
}

/* The kind of file RomM's @name is: its suffix after @base or
 * @rom_base, else (unless @by_name) the longest of @known it ends
 * with, else its extension. */
static void romm_saves_remote_suffix(char *s, size_t len,
      const char *name, const char *base, const char *rom_base,
      const char *const *known, bool by_name)
{
   char plain[512];
   const char *suffix;
   size_t plain_len, best = 0, i;

   romm_saves_strip_tag(plain, sizeof(plain), name);
   if (     (suffix = romm_saves_suffix(plain, base))
         || (rom_base && (suffix = romm_saves_suffix(plain, rom_base))))
   {
      strlcpy(s, suffix, len);
      return;
   }
   *s = '\0';
   if (by_name)
      return;
   plain_len = strlen(plain);
   for (i = 0; i < RBUF_LEN(known); i++)
   {
      size_t k = strlen(known[i]);
      if (     k > best && k < plain_len
            && !strcasecmp(plain + plain_len - k, known[i]))
         best = k;
   }
   if (best)
      strlcpy(s, plain + plain_len - best, len);
   else if ((suffix = strrchr(plain, '.')))
      strlcpy(s, suffix, len);
   else
      *s = '\0';
}

static bool romm_saves_newer(const romm_save_t *a, const romm_save_t *b)
{
   int64_t ta = romm_json_time(a->updated_at);
   int64_t tb = romm_json_time(b->updated_at);
   return ta != tb ? ta > tb : a->id > b->id;
}

/* Decides one file's step; @local or @remote may be NULL. */
static void romm_saves_plan_file(romm_saves_step_t **steps,
      const romm_saves_game_t *game, bool is_state, const char *suffix,
      const romm_saves_local_t *local, const romm_save_t *remote)
{
   const romm_saves_file_t *rec = game
      ? romm_saves_game_file((romm_saves_game_t*)game, suffix, false)
      : NULL;
   bool same           = local && remote && *remote->content_hash
      && string_is_equal_noncase(local->md5, remote->content_hash);
   bool local_changed  = local
      && (!rec || !string_is_equal(local->md5, rec->md5));
   bool remote_changed = remote
      && (     !rec || remote->id != rec->remote_id
            || romm_json_time(remote->updated_at) > rec->remote_time);
   romm_saves_step_t step;

   memset(&step, 0, sizeof(step));
   if (same)
   {
      if (!rec || rec->remote_id != remote->id
            || !string_is_equal(rec->md5, local->md5))
         step.type = ROMM_SAVES_MATCH;
      else
         return;
   }
   else if (remote && !local)
      step.type = ROMM_SAVES_DOWNLOAD;
   else if (local_changed && remote_changed)
      step.type = ROMM_SAVES_CONFLICT;
   else if (remote_changed)
      step.type = ROMM_SAVES_DOWNLOAD;
   else if (local_changed)
      step.type = ROMM_SAVES_UPLOAD;
   else
      return;

   step.is_state = is_state;
   strlcpy(step.suffix, suffix, sizeof(step.suffix));
   if (local)
      strlcpy(step.md5, local->md5, sizeof(step.md5));
   if (remote)
      step.remote = *remote;
   RBUF_PUSH(*steps, step);
}

/* Plans the saves (or the states, with @is_state). */
static void romm_saves_plan_kind(romm_saves_step_t **steps,
      const romm_saves_plan_params_t *p, bool is_state)
{
   const romm_saves_local_t *local = is_state ? p->states : p->saves;
   size_t local_count              = is_state ? p->states_count
      : p->saves_count;
   const romm_save_t *remote       = is_state ? p->remote_states
      : p->remote_saves;
   size_t remote_count             = is_state ? p->remote_states_count
      : p->remote_saves_count;
   const char **known              = NULL;
   /* Per remote entry, its kind; the newest of each kind is chosen. */
   char (*kinds)[64]               = NULL;
   bool *chosen                    = NULL;
   size_t i, j;

   for (i = 0; i < local_count; i++)
      RBUF_PUSH(known, local[i].suffix);
   if (p->game)
      for (i = 0; i < RBUF_LEN(p->game->files); i++)
         if (string_starts_with(p->game->files[i].suffix, ".state")
               == is_state)
            RBUF_PUSH(known, p->game->files[i].suffix);

   if (remote_count)
   {
      kinds  = (char(*)[64])calloc(remote_count, sizeof(*kinds));
      chosen = (bool*)calloc(remote_count, sizeof(*chosen));
   }
   if (remote_count && (!kinds || !chosen))
      remote_count = 0;
   for (i = 0; i < remote_count; i++)
   {
      if (     is_state
            ? !string_is_equal(remote[i].emulator, p->emulator)
            : string_is_equal(remote[i].slot, ROMM_SAVES_KEPT_SLOT))
         continue;
      romm_saves_remote_suffix(kinds[i], sizeof(kinds[i]),
            remote[i].file_name, p->base, p->rom_base, known, is_state);
      if (     !*kinds[i]
            || string_starts_with(kinds[i], ".state") != is_state)
         continue;
      chosen[i] = true;
      for (j = 0; j < i; j++)
         if (chosen[j] && string_is_equal_noncase(kinds[j], kinds[i]))
         {
            if (romm_saves_newer(&remote[i], &remote[j]))
               chosen[j] = false;
            else
               chosen[i] = false;
            break;
         }
   }

   /* Local files, with RomM's copy of the same kind if any. */
   for (i = 0; i < local_count; i++)
   {
      const romm_save_t *match = NULL;
      for (j = 0; j < remote_count; j++)
         if (chosen[j] && string_is_equal_noncase(kinds[j],
               local[i].suffix))
         {
            match     = &remote[j];
            chosen[j] = false;
            break;
         }
      romm_saves_plan_file(steps, p->game, is_state, local[i].suffix,
            &local[i], match);
   }
   /* RomM's copies of kinds not here. */
   for (j = 0; j < remote_count; j++)
      if (chosen[j])
         romm_saves_plan_file(steps, p->game, is_state, kinds[j], NULL,
               &remote[j]);

   free(kinds);
   free(chosen);
   RBUF_FREE(known);
}

romm_saves_step_t *romm_saves_plan(const romm_saves_plan_params_t *p)
{
   romm_saves_step_t *steps = NULL;
   romm_saves_plan_kind(&steps, p, false);
   romm_saves_plan_kind(&steps, p, true);
   return steps;
}

typedef struct romm_saves_sync romm_saves_sync_t;

struct romm_saves_sync
{
   romm_saves_sync_cb_t cb;
   void *user_data;
   bool push;
   enum romm_saves_resolve resolve;
   bool (*is_live)(const char *path);
   unsigned rom_id;
   enum romm_status status;
   int http_status;
   romm_saves_result_t result;
   romm_save_t *remote_saves;
   romm_save_t *remote_states;
   romm_saves_record_t rec;
   romm_saves_step_t *steps;
   size_t next;
   /* The upload out was sent over newer saves. */
   bool overwriting;
   /* A state was kept from a conflict: older kept ones may go. */
   bool kept_state;
   char server[1024];
   char token[128];
   char device_id[64];
   char emulator[64];
   char base[256];
   char rom_base[256];
   char save_dir[1024];
   char state_dir[1024];
   char record_path[1024];
};

/* The sync running, then those waiting, in order. */
static romm_saves_sync_t **romm_saves_queue = NULL;

static void romm_saves_sync_step(romm_saves_sync_t *sync);
static bool romm_saves_sync_begin(romm_saves_sync_t *sync);

static void romm_saves_sync_free(romm_saves_sync_t *sync)
{
   RBUF_FREE(sync->remote_saves);
   RBUF_FREE(sync->remote_states);
   RBUF_FREE(sync->steps);
   romm_saves_record_free(&sync->rec);
   memset(sync->token, 0, sizeof(sync->token));
   free(sync);
}

static romm_saves_game_t *romm_saves_sync_game(romm_saves_sync_t *sync)
{
   return romm_saves_record_game(&sync->rec, sync->rom_id, sync->emulator,
         true);
}

static void romm_saves_sync_end(romm_saves_sync_t *sync)
{
   romm_saves_game_t *game = romm_saves_sync_game(sync);
   size_t i;

   strlcpy(game->save_dir, sync->save_dir, sizeof(game->save_dir));
   strlcpy(game->state_dir, sync->state_dir, sizeof(game->state_dir));
   strlcpy(game->base, sync->base, sizeof(game->base));
   /* Until a push gets everything up, the game is owed one. */
   game->pending = !sync->push || sync->status != ROMM_OK
      || sync->result.conflicts || sync->result.failed;
   if (!romm_saves_record_save(&sync->rec, sync->record_path))
      RARCH_WARN(ROMM_SAVES_PFX "Couldn't write %s.\n", sync->record_path);

   if (sync->status == ROMM_OK)
      RARCH_LOG(ROMM_SAVES_PFX "Synced saves of game %u (%s) %s: %u "
            "down, %u up, %u in conflict, %u failed.\n", sync->rom_id,
            sync->emulator, sync->push ? "after play" : "before play",
            sync->result.downloaded, sync->result.uploaded,
            sync->result.conflicts, sync->result.failed);
   else
      RARCH_WARN(ROMM_SAVES_PFX "Couldn't sync saves of game %u "
            "(status %d, HTTP %d).\n", sync->rom_id, sync->status,
            sync->http_status);
   if (sync->cb)
      sync->cb(sync->user_data, sync->status, sync->http_status,
            &sync->result);

   /* Next in line. */
   for (i = 1; i < RBUF_LEN(romm_saves_queue); i++)
      romm_saves_queue[i - 1] = romm_saves_queue[i];
   RBUF_RESIZE(romm_saves_queue, RBUF_LEN(romm_saves_queue) - 1);
   romm_saves_sync_free(sync);
   if (RBUF_LEN(romm_saves_queue))
   {
      if (!romm_saves_sync_begin(romm_saves_queue[0]))
         romm_saves_sync_end(romm_saves_queue[0]);
   }
   else
      RBUF_FREE(romm_saves_queue);
}

/* The record of @step's file now matching RomM's @remote, holding
 * @md5. */
static void romm_saves_sync_record(romm_saves_sync_t *sync,
      const romm_saves_step_t *step, const romm_save_t *remote,
      const char *md5)
{
   romm_saves_file_t *f = romm_saves_game_file(romm_saves_sync_game(sync),
         step->suffix, true);
   f->remote_id   = remote->id;
   f->remote_time = romm_json_time(remote->updated_at);
   strlcpy(f->md5, md5, sizeof(f->md5));
   romm_saves_record_save(&sync->rec, sync->record_path);
}

static size_t romm_saves_sync_path(const romm_saves_sync_t *sync,
      const romm_saves_step_t *step, char *s, size_t len)
{
   char name[384];
   snprintf(name, sizeof(name), "%s%s", sync->base, step->suffix);
   return fill_pathname_join(s,
         step->is_state ? sync->state_dir : sync->save_dir, name, len);
}

static void romm_saves_sync_downloaded(void *user_data,
      enum romm_status status, int http_status, const void *data,
      size_t len)
{
   romm_saves_sync_t *sync       = (romm_saves_sync_t*)user_data;
   const romm_saves_step_t *step = &sync->steps[sync->next - 1];
   char path[2048];
   char md5[33];

   romm_saves_sync_path(sync, step, path, sizeof(path));
   if (status != ROMM_OK)
      sync->result.failed++;
   else if (sync->is_live && sync->is_live(path))
   {
      RARCH_WARN(ROMM_SAVES_PFX "%s is in use; not replaced.\n", path);
      sync->result.failed++;
   }
   else
   {
      char dir[2048];
      fill_pathname_basedir(dir, path, sizeof(dir));
      if (     (path_is_directory(dir) || path_mkdir(dir))
            && filestream_write_file_atomic(path, data, (int64_t)len))
      {
         RARCH_LOG(ROMM_SAVES_PFX "Fetched %s from RomM.\n", path);
         romm_saves_md5(data, len, md5);
         romm_saves_sync_record(sync, step, &step->remote, md5);
         sync->result.downloaded++;
      }
      else
      {
         RARCH_WARN(ROMM_SAVES_PFX "Couldn't write %s.\n", path);
         sync->result.failed++;
      }
   }
   romm_saves_sync_step(sync);
}

static bool romm_saves_sync_upload(romm_saves_sync_t *sync,
      romm_saves_step_t *step, bool overwrite);

/* Whether RomM listed save @id when this sync began. */
static bool romm_saves_sync_listed(const romm_saves_sync_t *sync,
      unsigned id)
{
   size_t i;
   for (i = 0; i < RBUF_LEN(sync->remote_saves); i++)
      if (sync->remote_saves[i].id == id)
         return true;
   return false;
}

static void romm_saves_sync_uploaded(void *user_data,
      enum romm_status status, int http_status, const romm_save_t *save)
{
   romm_saves_sync_t *sync = (romm_saves_sync_t*)user_data;
   romm_saves_step_t *step = &sync->steps[sync->next - 1];

   /* A save equal to an older version in the slot comes back as that
    * version, which the next sync would take for RomM's copy having
    * changed. Sent again over it, it's the newest. */
   if (     status == ROMM_OK && !step->is_state && !sync->overwriting
         && save->id != step->remote.id
         && romm_saves_sync_listed(sync, save->id))
   {
      RARCH_LOG(ROMM_SAVES_PFX "RomM had %s%s as an older version (%u); "
            "sending it again as the newest.\n", sync->base, step->suffix,
            save->id);
      if (romm_saves_sync_upload(sync, step, true))
         return;
      status = ROMM_ERR_UNREACHABLE;
   }
   if (status == ROMM_OK)
   {
      romm_saves_sync_record(sync, step, save, step->md5);
      sync->result.uploaded++;
   }
   else if (status == ROMM_ERR_CONFLICT)
      sync->result.conflicts++;
   else
      sync->result.failed++;
   romm_saves_sync_step(sync);
}

/* RomM knows this device holds the save it matched. */
static void romm_saves_sync_confirmed(void *user_data,
      enum romm_status status, int http_status, const romm_save_t *save)
{
   romm_saves_sync_t *sync       = (romm_saves_sync_t*)user_data;
   const romm_saves_step_t *step = &sync->steps[sync->next - 1];

   if (status == ROMM_OK)
      romm_saves_sync_record(sync, step, &step->remote, step->md5);
   else
      sync->result.failed++;
   romm_saves_sync_step(sync);
}

/* Sends @step's file; @overwrite sends a save over a newer one. */
static bool romm_saves_sync_upload(romm_saves_sync_t *sync,
      romm_saves_step_t *step, bool overwrite)
{
   romm_upload_t up;
   void *buf   = NULL;
   int64_t len = 0;
   char path[2048];
   char md5[33];
   bool started;

   romm_saves_sync_path(sync, step, path, sizeof(path));
   if (!filestream_read_file(path, &buf, &len))
      return false;
   /* Written since it was listed: send what is there now. */
   romm_saves_md5(buf, (size_t)len, md5);
   strlcpy(step->md5, md5, sizeof(step->md5));

   memset(&up, 0, sizeof(up));
   up.server    = sync->server;
   up.token     = sync->token;
   up.rom_id    = sync->rom_id;
   up.file_name = path_basename(path);
   up.data      = buf;
   up.len       = (size_t)len;
   up.emulator  = sync->emulator;
   sync->overwriting = overwrite;
   if (step->is_state)
      started = romm_client_state_upload(&up, romm_saves_sync_uploaded,
            sync);
   else
   {
      up.slot         = ROMM_SAVES_SLOT;
      up.keep         = ROMM_SAVES_KEEP;
      up.device_id    = sync->device_id;
      up.content_hash = md5;
      up.overwrite    = overwrite;
      started = romm_client_save_upload(&up, romm_saves_sync_uploaded,
            sync);
   }
   free(buf);
   return started;
}

static bool romm_saves_sync_fetch(romm_saves_sync_t *sync,
      const romm_saves_step_t *step)
{
   return step->is_state
      ? romm_client_state_content(sync->server, sync->token,
            step->remote.id, romm_saves_sync_downloaded, sync)
      : romm_client_save_content(sync->server, sync->token,
            step->remote.id, sync->device_id, romm_saves_sync_downloaded,
            sync);
}

/* The copy not chosen is in RomM, kept apart: now the chosen one. */
static void romm_saves_sync_kept(void *user_data, enum romm_status status,
      int http_status, const romm_save_t *save)
{
   romm_saves_sync_t *sync = (romm_saves_sync_t*)user_data;
   romm_saves_step_t *step = &sync->steps[sync->next - 1];

   if (status != ROMM_OK)
      sync->result.failed++;
   else
   {
      RARCH_LOG(ROMM_SAVES_PFX "Kept the other copy of %s%s in RomM as "
            "%s.\n", sync->base, step->suffix, save->file_name);
      if (step->is_state)
      {
         romm_save_t kept = *save;
         strlcpy(kept.emulator, sync->emulator, sizeof(kept.emulator));
         RBUF_PUSH(sync->remote_states, kept);
         sync->kept_state = true;
      }
      if (sync->resolve == ROMM_SAVES_RESOLVE_REMOTE
            ? romm_saves_sync_fetch(sync, step)
            : romm_saves_sync_upload(sync, step, true))
         return;
      sync->result.failed++;
   }
   romm_saves_sync_step(sync);
}

/* Sends @len bytes of @data, the copy of @step's file not chosen, to
 * RomM to be kept apart from what syncs use. */
static bool romm_saves_sync_keep(romm_saves_sync_t *sync,
      const romm_saves_step_t *step, const void *data, size_t len)
{
   romm_upload_t up;
   char name[512];

   memset(&up, 0, sizeof(up));
   up.server    = sync->server;
   up.token     = sync->token;
   up.rom_id    = sync->rom_id;
   up.data      = data;
   up.len       = len;
   up.emulator  = sync->emulator;
   up.file_name = name;
   if (step->is_state)
   {
      char stamp[32];
      struct tm tm_;
      time_t now = time(NULL);
      rtime_localtime(&now, &tm_);
      strftime(stamp, sizeof(stamp), "%Y-%m-%d %H-%M-%S", &tm_);
      snprintf(name, sizeof(name), "%s (conflict %s)%s", sync->base,
            stamp, step->suffix);
      return romm_client_state_upload(&up, romm_saves_sync_kept, sync);
   }
   /* Named apart too: RomM files a slotted upload under its name and
    * second, whatever the slot, so an upload to the autosave slot in
    * the same second would take the place of a kept copy of the same
    * name. */
   snprintf(name, sizeof(name), "%s (conflict)%s", sync->base,
         step->suffix);
   up.slot = ROMM_SAVES_KEPT_SLOT;
   up.keep = ROMM_SAVES_KEEP;
   return romm_client_save_upload(&up, romm_saves_sync_kept, sync);
}

/* RomM's copy, to keep apart before this device's goes up over it. */
static void romm_saves_sync_loser_fetched(void *user_data,
      enum romm_status status, int http_status, const void *data,
      size_t len)
{
   romm_saves_sync_t *sync       = (romm_saves_sync_t*)user_data;
   const romm_saves_step_t *step = &sync->steps[sync->next - 1];

   if (status == ROMM_OK && romm_saves_sync_keep(sync, step, data, len))
      return;
   sync->result.failed++;
   romm_saves_sync_step(sync);
}

/* Starts resolving @step's conflict as asked. */
static bool romm_saves_sync_resolve(romm_saves_sync_t *sync,
      const romm_saves_step_t *step)
{
   void *buf   = NULL;
   int64_t len = 0;
   char path[2048];
   bool started;

   RARCH_LOG(ROMM_SAVES_PFX "%s%s changed both here and in RomM; using "
         "%s.\n", sync->base, step->suffix,
         sync->resolve == ROMM_SAVES_RESOLVE_REMOTE ? "RomM's"
         : "this device's");
   if (sync->resolve == ROMM_SAVES_RESOLVE_LOCAL)
      return step->is_state
         ? romm_client_state_content(sync->server, sync->token,
               step->remote.id, romm_saves_sync_loser_fetched, sync)
         : romm_client_save_content(sync->server, sync->token,
               step->remote.id, NULL, romm_saves_sync_loser_fetched, sync);
   romm_saves_sync_path(sync, step, path, sizeof(path));
   if (!filestream_read_file(path, &buf, &len))
      return false;
   started = romm_saves_sync_keep(sync, step, buf, (size_t)len);
   free(buf);
   return started;
}

/* Whether @state is one kept from a conflict with this emulator. */
static bool romm_saves_sync_is_kept(const romm_saves_sync_t *sync,
      const romm_save_t *state)
{
   return string_is_equal(state->emulator, sync->emulator)
      && romm_saves_find(state->file_name,
            " (conflict dddd-dd-dd dd-dd-dd).state");
}

static int romm_saves_cmp_newest(const void *a, const void *b)
{
   const romm_save_t *sa = *(const romm_save_t *const *)a;
   const romm_save_t *sb = *(const romm_save_t *const *)b;
   if (romm_saves_newer(sa, sb))
      return -1;
   return romm_saves_newer(sb, sa) ? 1 : 0;
}

static void romm_saves_sync_pruned(void *user_data,
      enum romm_status status, int http_status)
{
   romm_saves_sync_t *sync = (romm_saves_sync_t*)user_data;
   if (status == ROMM_OK)
      RARCH_LOG(ROMM_SAVES_PFX "Deleted the oldest kept states of %s.\n",
            sync->base);
   romm_saves_sync_step(sync);
}

/* Deletes the states kept from conflicts with this emulator past the
 * newest ROMM_SAVES_KEEP. False when there are none to delete. */
static bool romm_saves_sync_prune(romm_saves_sync_t *sync)
{
   const romm_save_t **kept = NULL;
   unsigned *ids            = NULL;
   bool started             = false;
   size_t i;

   for (i = 0; i < RBUF_LEN(sync->remote_states); i++)
      if (romm_saves_sync_is_kept(sync, &sync->remote_states[i]))
         RBUF_PUSH(kept, &sync->remote_states[i]);
   if (RBUF_LEN(kept) > ROMM_SAVES_KEEP)
   {
      qsort(kept, RBUF_LEN(kept), sizeof(*kept), romm_saves_cmp_newest);
      for (i = ROMM_SAVES_KEEP; i < RBUF_LEN(kept); i++)
         RBUF_PUSH(ids, kept[i]->id);
      started = romm_client_states_delete(sync->server, sync->token, ids,
            RBUF_LEN(ids), romm_saves_sync_pruned, sync);
   }
   RBUF_FREE(kept);
   RBUF_FREE(ids);
   return started;
}

/* Runs the steps for this direction, one at a time. */
static void romm_saves_sync_step(romm_saves_sync_t *sync)
{
   while (sync->next < RBUF_LEN(sync->steps))
   {
      romm_saves_step_t *step = &sync->steps[sync->next++];

      switch (step->type)
      {
         case ROMM_SAVES_MATCH:
            /* Unless RomM knows this device holds it, it refuses the
             * device's next upload to its slot. */
            if (!step->is_state && *sync->device_id)
            {
               if (romm_client_save_downloaded(sync->server, sync->token,
                     step->remote.id, sync->device_id, step->md5,
                     romm_saves_sync_confirmed, sync))
                  return;
               sync->result.failed++;
               break;
            }
            romm_saves_sync_record(sync, step, &step->remote, step->md5);
            break;
         case ROMM_SAVES_CONFLICT:
            if (sync->resolve != ROMM_SAVES_RESOLVE_NONE)
            {
               if (romm_saves_sync_resolve(sync, step))
                  return;
               sync->result.failed++;
               break;
            }
            RARCH_WARN(ROMM_SAVES_PFX "%s%s changed both here and in "
                  "RomM.\n", sync->base, step->suffix);
            sync->result.conflicts++;
            break;
         case ROMM_SAVES_DOWNLOAD:
            if (sync->push)
               break;
            if (romm_saves_sync_fetch(sync, step))
               return;
            sync->result.failed++;
            break;
         case ROMM_SAVES_UPLOAD:
            if (!sync->push)
               break;
            if (romm_saves_sync_upload(sync, step, false))
               return;
            sync->result.failed++;
            break;
      }
   }
   if (sync->kept_state)
   {
      sync->kept_state = false;
      if (romm_saves_sync_prune(sync))
         return;
   }
   romm_saves_sync_end(sync);
}

static void romm_saves_sync_states_cb(void *user_data,
      enum romm_status status, int http_status, const romm_save_t *saves,
      size_t count)
{
   romm_saves_sync_t *sync = (romm_saves_sync_t*)user_data;
   romm_saves_plan_params_t p;
   romm_saves_local_t *local_saves, *local_states;
   size_t i;

   if (status != ROMM_OK)
   {
      sync->status      = status;
      sync->http_status = http_status;
      romm_saves_sync_end(sync);
      return;
   }
   for (i = 0; i < count; i++)
      RBUF_PUSH(sync->remote_states, saves[i]);

   local_saves  = romm_saves_list_local(sync->save_dir, sync->base, false);
   local_states = romm_saves_list_local(sync->state_dir, sync->base, true);

   memset(&p, 0, sizeof(p));
   p.game                = romm_saves_record_game(&sync->rec,
         sync->rom_id, sync->emulator, false);
   p.emulator            = sync->emulator;
   p.base                = sync->base;
   p.rom_base            = *sync->rom_base ? sync->rom_base : NULL;
   p.saves               = local_saves;
   p.saves_count         = RBUF_LEN(local_saves);
   p.states              = local_states;
   p.states_count        = RBUF_LEN(local_states);
   p.remote_saves        = sync->remote_saves;
   p.remote_saves_count  = RBUF_LEN(sync->remote_saves);
   p.remote_states       = sync->remote_states;
   p.remote_states_count = RBUF_LEN(sync->remote_states);
   sync->steps           = romm_saves_plan(&p);
   RBUF_FREE(local_saves);
   RBUF_FREE(local_states);

   romm_saves_sync_step(sync);
}

static void romm_saves_sync_saves_cb(void *user_data,
      enum romm_status status, int http_status, const romm_save_t *saves,
      size_t count)
{
   romm_saves_sync_t *sync = (romm_saves_sync_t*)user_data;
   size_t i;

   if (status == ROMM_OK)
   {
      for (i = 0; i < count; i++)
         RBUF_PUSH(sync->remote_saves, saves[i]);
      if (romm_client_states(sync->server, sync->token, sync->rom_id,
            romm_saves_sync_states_cb, sync))
         return;
      status = ROMM_ERR_UNREACHABLE;
   }
   sync->status      = status;
   sync->http_status = http_status;
   romm_saves_sync_end(sync);
}

/* False when the first request can't start; the sync is then still
 * to be ended. */
static bool romm_saves_sync_begin(romm_saves_sync_t *sync)
{
   /* Read now: a sync that fails still writes the record back. */
   romm_saves_record_load(&sync->rec, sync->record_path);
   if (romm_client_saves(sync->server, sync->token, sync->rom_id,
         romm_saves_sync_saves_cb, sync))
      return true;
   sync->status = ROMM_ERR_UNREACHABLE;
   return false;
}

bool romm_saves_sync(const romm_saves_sync_params_t *p,
      romm_saves_sync_cb_t cb, void *user_data)
{
   romm_saves_sync_t *sync;

   if (     !p || !p->rom_id || string_is_empty(p->server)
         || string_is_empty(p->token) || string_is_empty(p->emulator)
         || string_is_empty(p->base) || string_is_empty(p->save_dir)
         || string_is_empty(p->state_dir)
         || string_is_empty(p->record_path))
      return false;
   if (!(sync = (romm_saves_sync_t*)calloc(1, sizeof(*sync))))
      return false;
   sync->cb        = cb;
   sync->user_data = user_data;
   sync->push      = p->push;
   sync->resolve   = p->push ? ROMM_SAVES_RESOLVE_NONE : p->resolve;
   sync->is_live   = p->is_live;
   sync->rom_id    = p->rom_id;
   strlcpy(sync->server, p->server, sizeof(sync->server));
   strlcpy(sync->token, p->token, sizeof(sync->token));
   if (p->device_id)
      strlcpy(sync->device_id, p->device_id, sizeof(sync->device_id));
   strlcpy(sync->emulator, p->emulator, sizeof(sync->emulator));
   strlcpy(sync->base, p->base, sizeof(sync->base));
   if (p->rom_base)
      strlcpy(sync->rom_base, p->rom_base, sizeof(sync->rom_base));
   strlcpy(sync->save_dir, p->save_dir, sizeof(sync->save_dir));
   strlcpy(sync->state_dir, p->state_dir, sizeof(sync->state_dir));
   strlcpy(sync->record_path, p->record_path, sizeof(sync->record_path));

   RBUF_PUSH(romm_saves_queue, sync);
   if (RBUF_LEN(romm_saves_queue) > 1 || romm_saves_sync_begin(sync))
      return true;
   /* Callers expect @cb after this returns, never before. */
   sync->cb = NULL;
   romm_saves_sync_end(sync);
   return false;
}
