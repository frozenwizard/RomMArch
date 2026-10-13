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
#include <encodings/crc32.h>
#include <features/features_cpu.h>
#include <file/file_path.h>
#include <libretro.h>
#include <queues/task_queue.h>
#include <retro_miscellaneous.h>
#include <string/stdstring.h>
#include <vfs/vfs_implementation_romm.h>

#include "romm_launch.h"
#include "romm_library.h"
#include "../../verbosity.h"

/* RetroArch's HTTP client refuses replies over 256 MB. */
#define ROMM_LAUNCH_RANGE ((uint64_t)64 * 1024 * 1024)
#define ROMM_LAUNCH_PFX "[RomM] "

enum romm_launch_state
{
   ROMM_LAUNCH_FILES = 0,
   /* Fetching the files one after another, a range at a time */
   ROMM_LAUNCH_DOWNLOAD
};

typedef struct
{
   romm_launch_cb_t cb;
   void *user_data;
   retro_task_t *task;
   romm_rom_t rom;
   romm_file_t *files;
   /* The file being fetched, its bytes so far and their CRC32 */
   size_t file;
   uint8_t *buf;
   uint64_t have;
   uint32_t crc;
   bool check_crc;
   /* Bytes in all the files, and in those already fetched */
   uint64_t total;
   uint64_t done;
   /* A folder game's disc list: RomM's own .m3u, by index, or one
    * made from its disc images */
   int m3u_file;
   char *list;
   size_t list_len;
   retro_time_t started;
   enum romm_launch_state state;
   enum romm_status status;
   int http_status;
   bool pending;
   bool finished;
   char *token;
   char server[NAME_MAX_LENGTH];
   char content_path[PATH_MAX_LENGTH];
   /* romm://<id>/, which every file of the game is under */
   char prefix[64];
} romm_launch_t;

unsigned romm_launch_rom_id(const char *content_path)
{
   const char *p;
   char *end;
   unsigned long id;

   if (!content_path || !string_starts_with(content_path, ROMM_SCHEME))
      return 0;
   p  = content_path + STRLEN_CONST(ROMM_SCHEME);
   id = strtoul(p, &end, 10);
   if (end == p || *end != '/')
      return 0;
   return (unsigned)id;
}

bool romm_launch_is_ready(const char *content_path)
{
   return (retro_vfs_stat_romm(content_path, NULL)
         & RETRO_VFS_STAT_IS_VALID) != 0;
}

void romm_launch_release(const char *keep_path)
{
   char prefix[32];
   unsigned rom_id = romm_launch_rom_id(keep_path);

   if (!rom_id)
   {
      retro_vfs_romm_remove_prefix(ROMM_SCHEME);
      return;
   }
   snprintf(prefix, sizeof(prefix), ROMM_SCHEME "%u/", rom_id);
   retro_vfs_romm_keep_prefix(prefix);
}

static bool romm_launch_ext_is(const char *path, const char *const *exts)
{
   const char *ext = path_get_extension(path);
   size_t i;

   if (!ext)
      return false;
   for (i = 0; exts[i]; i++)
      if (!strcasecmp(ext, exts[i]))
         return true;
   return false;
}

static const char *const romm_launch_archive_exts[] = {
   "zip", "7z", "rar", NULL };
static const char *const romm_launch_rar_ext[] = { "rar", NULL };

static int romm_launch_cmp_path(const void *a, const void *b)
{
   return strcasecmp((*(const romm_file_t* const*)a)->path,
         (*(const romm_file_t* const*)b)->path);
}

/* Chooses what a folder game's disc list holds: RomM's own .m3u at the
 * top of the folder, else the disc descriptors (.cue, .gdi, ...) in
 * path order, else the disc images themselves. False when the folder
 * holds nothing that can be launched. */
static bool romm_launch_pick_discs(romm_launch_t *launch)
{
   static const char *const m3u_ext[] = { "m3u", NULL };
   static const char *const tiers[][6] = {
      { "cue", "gdi", "ccd", "mds", "toc", NULL },
      { "chd", "cdi", "iso", "pbp", "img", NULL },
      { "bin", NULL }
   };
   const romm_file_t **picked = NULL;
   size_t count               = RBUF_LEN(launch->files);
   int m3u                    = -1;
   size_t i, t, len;

   for (i = 0; i < count; i++)
   {
      const char *path = launch->files[i].path;
      if (!strchr(path, '/') && romm_launch_ext_is(path, m3u_ext))
      {
         if (m3u >= 0)
         {
            m3u = -2;
            break;
         }
         m3u = (int)i;
      }
   }
   if (m3u >= 0)
   {
      launch->m3u_file = m3u;
      return true;
   }

   for (t = 0; t < ARRAY_SIZE(tiers) && !picked; t++)
      for (i = 0; i < count; i++)
         if (romm_launch_ext_is(launch->files[i].path, tiers[t]))
            RBUF_PUSH(picked, &launch->files[i]);
   if (!picked)
      return false;

   qsort(picked, RBUF_LEN(picked), sizeof(*picked), romm_launch_cmp_path);
   for (i = 0, len = 0; i < RBUF_LEN(picked); i++)
      len += strlen(picked[i]->path) + 1;
   if ((launch->list = (char*)malloc(len + 1)))
   {
      launch->list[0] = '\0';
      for (i = 0; i < RBUF_LEN(picked); i++)
      {
         strlcat(launch->list, picked[i]->path, len + 1);
         strlcat(launch->list, "\n", len + 1);
      }
      launch->list_len = len;
   }
   RBUF_FREE(picked);
   return launch->list != NULL;
}

static void romm_launch_finish(romm_launch_t *launch,
      enum romm_status status, int http_status)
{
   launch->status      = status;
   launch->http_status = http_status;
   launch->finished    = true;
   launch->task->when  = 0;
}

static void romm_launch_files_cb(void *user_data, enum romm_status status,
      int http_status, const romm_rom_t *rom, const romm_file_t *files,
      size_t count)
{
   romm_launch_t *launch = (romm_launch_t*)user_data;
   size_t i;

   launch->pending    = false;
   launch->task->when = 0;
   if (status != ROMM_OK)
   {
      romm_launch_finish(launch, status, http_status);
      return;
   }
   if (rom->id != romm_launch_rom_id(launch->content_path) || !count)
   {
      romm_launch_finish(launch, ROMM_ERR_RESPONSE, 0);
      return;
   }

   launch->rom = *rom;
   for (i = 0; i < count; i++)
   {
      RBUF_PUSH(launch->files, files[i]);
      launch->total += files[i].size;
   }

   /* Nothing in RetroArch reads RAR. */
   if (     rom->single_file
         && romm_launch_ext_is(files[0].name, romm_launch_rar_ext))
   {
      romm_launch_finish(launch, ROMM_ERR_UNSUPPORTED, 0);
      return;
   }
   if (!rom->single_file && !romm_launch_pick_discs(launch))
   {
      romm_launch_finish(launch, ROMM_ERR_UNSUPPORTED, 0);
      return;
   }
   RARCH_LOG(ROMM_LAUNCH_PFX "Fetching %s: %u files, %llu bytes.\n",
         rom->fs_name, (unsigned)count, (unsigned long long)launch->total);
   launch->state = ROMM_LAUNCH_DOWNLOAD;
}

/* Hands the file just fetched to the VFS, under the playlist path for
 * a game that is one file. */
static bool romm_launch_store_file(romm_launch_t *launch)
{
   const romm_file_t *f = &launch->files[launch->file];
   char path[PATH_MAX_LENGTH];

   if (launch->check_crc
         && strtoul(f->crc, NULL, 16) != (unsigned long)launch->crc)
   {
      RARCH_WARN(ROMM_LAUNCH_PFX "%s doesn't match RomM's CRC32.\n",
            f->path);
      romm_launch_finish(launch, ROMM_ERR_CHECKSUM, 0);
      return false;
   }
   if (launch->rom.single_file)
      strlcpy(path, launch->content_path, sizeof(path));
   else
      snprintf(path, sizeof(path), "%s%s", launch->prefix, f->path);

   if ((int)launch->file == launch->m3u_file)
   {
      if (!(launch->list = (char*)malloc(launch->have + 1)))
      {
         romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
         return false;
      }
      if (launch->have)
         memcpy(launch->list, launch->buf, (size_t)launch->have);
      launch->list_len = (size_t)launch->have;
   }

   if (!retro_vfs_romm_add(path, launch->buf, launch->have))
   {
      romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
      return false;
   }
   launch->buf   = NULL;
   launch->done += launch->have;
   launch->have  = 0;
   launch->file++;
   return true;
}

static void romm_launch_content_cb(void *user_data, enum romm_status status,
      int http_status, void *data, size_t len)
{
   romm_launch_t *launch = (romm_launch_t*)user_data;
   const romm_file_t *f  = &launch->files[launch->file];

   launch->pending    = false;
   launch->task->when = 0;
   if (status != ROMM_OK)
   {
      free(data);
      romm_launch_finish(launch, status, http_status);
      return;
   }
   if (!len || launch->have + len > f->size)
   {
      free(data);
      romm_launch_finish(launch, ROMM_ERR_CHECKSUM, 0);
      return;
   }
   memcpy(launch->buf + launch->have, data, len);
   if (launch->check_crc)
      launch->crc = encoding_crc32(launch->crc, (const uint8_t*)data, len);
   launch->have += len;
   free(data);

   if (launch->have == f->size)
      romm_launch_store_file(launch);
}

/* Starts fetching the next range of the current file, or finishes the
 * game once every file is in. */
static void romm_launch_next(romm_launch_t *launch)
{
   const romm_file_t *f;
   uint64_t want;

   if (launch->file >= RBUF_LEN(launch->files))
   {
      /* A folder game launches through its disc list. */
      if (!launch->rom.single_file)
      {
         if (!retro_vfs_romm_add(launch->content_path, launch->list,
               launch->list_len))
         {
            romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
            return;
         }
         launch->list = NULL;
      }
      romm_launch_finish(launch, ROMM_OK, 0);
      return;
   }

   f = &launch->files[launch->file];
   if (!launch->buf)
   {
      launch->have      = 0;
      launch->crc       = 0;
      launch->check_crc = *f->crc
            && !romm_launch_ext_is(f->name, romm_launch_archive_exts);
      if (!f->size)
      {
         romm_launch_store_file(launch);
         return;
      }
      if (!(launch->buf = (uint8_t*)malloc((size_t)f->size)))
      {
         romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
         return;
      }
   }

   want = f->size - launch->have;
   if (want > ROMM_LAUNCH_RANGE)
      want = ROMM_LAUNCH_RANGE;
   if (romm_client_rom_content(launch->server, launch->token,
         launch->rom.id, f->id, f->name, launch->have, want,
         romm_launch_content_cb, launch))
      launch->pending = true;
   else
      romm_launch_finish(launch, ROMM_ERR_UNREACHABLE, 0);
}

/* Runs on the main thread (RETRO_TASK_FLG_MAIN_THREAD), like the
 * request callbacks, so they share @launch without locking. */
static void romm_launch_handler(retro_task_t *task)
{
   romm_launch_t *launch = (romm_launch_t*)task->state;

   /* A request still out holds @launch; its callback wakes the task. */
   if (launch->pending)
   {
      task->when = cpu_features_get_time_usec() + 250000;
      return;
   }
   if (!launch->finished && (task_get_flags(task) & RETRO_TASK_FLG_CANCELLED))
      romm_launch_finish(launch, ROMM_ERR_CANCELLED, 0);
   if (launch->finished)
   {
      task_set_flags(task, RETRO_TASK_FLG_FINISHED, true);
      return;
   }

   task->when = 0;
   switch (launch->state)
   {
      case ROMM_LAUNCH_FILES:
         if (romm_client_rom_files(launch->server, launch->token,
               romm_launch_rom_id(launch->content_path),
               romm_launch_files_cb, launch))
            launch->pending = true;
         else
            romm_launch_finish(launch, ROMM_ERR_UNREACHABLE, 0);
         break;
      case ROMM_LAUNCH_DOWNLOAD:
         if (launch->total)
            task_set_progress(task, (int8_t)((launch->done + launch->have)
                  * 100 / launch->total));
         romm_launch_next(launch);
         break;
   }
}

static void romm_launch_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_launch_t *launch = (romm_launch_t*)user_data;

   if (launch->status == ROMM_OK)
      RARCH_LOG(ROMM_LAUNCH_PFX "Fetched %s into memory in %u ms.\n",
            launch->rom.fs_name, (unsigned)((cpu_features_get_time_usec()
               - launch->started) / 1000));
   else
   {
      /* No half-fetched game is left behind. */
      retro_vfs_romm_remove_prefix(launch->prefix);
      RARCH_WARN(ROMM_LAUNCH_PFX "Couldn't fetch %s (status %d).\n",
            launch->content_path, launch->status);
   }
   if (launch->cb)
      launch->cb(launch->user_data, launch->status, launch->http_status);
}

static void romm_launch_cleanup(retro_task_t *task)
{
   romm_launch_t *launch = (romm_launch_t*)task->state;

   if (launch)
   {
      RBUF_FREE(launch->files);
      free(launch->buf);
      free(launch->list);
      if (launch->token)
      {
         memset(launch->token, 0, strlen(launch->token));
         free(launch->token);
      }
      free(launch);
   }
   task->state = NULL;
}

bool romm_launch_prepare(const char *server, const char *token,
      const char *content_path, const char *title,
      romm_launch_cb_t cb, void *user_data)
{
   romm_launch_t *launch;
   retro_task_t *task;
   unsigned rom_id = romm_launch_rom_id(content_path);

   if (!rom_id || string_is_empty(server) || string_is_empty(token))
      return false;
   if (!(launch = (romm_launch_t*)calloc(1, sizeof(*launch))))
      return false;
   if (!(launch->token = strdup(token)) || !(task = task_init()))
   {
      free(launch->token);
      free(launch);
      return false;
   }

   launch->cb        = cb;
   launch->user_data = user_data;
   launch->task      = task;
   launch->m3u_file  = -1;
   launch->started   = cpu_features_get_time_usec();
   strlcpy(launch->server, server, sizeof(launch->server));
   strlcpy(launch->content_path, content_path,
         sizeof(launch->content_path));
   snprintf(launch->prefix, sizeof(launch->prefix), ROMM_SCHEME "%u/",
         rom_id);

   task->handler   = romm_launch_handler;
   task->callback  = romm_launch_cb;
   task->cleanup   = romm_launch_cleanup;
   task->state     = launch;
   task->user_data = launch;
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
      free(launch->token);
      free(launch);
      return false;
   }

   /* One game in memory at a time. A core still reading the last one
    * keeps its open files until it closes them. */
   retro_vfs_romm_remove_prefix(ROMM_SCHEME);
   RARCH_LOG(ROMM_LAUNCH_PFX "Preparing %s.\n", content_path);
   return true;
}
