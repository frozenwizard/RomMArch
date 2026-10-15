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
#include <file/archive_file.h>
#include <file/file_path.h>
#include <libretro.h>
#include <lists/string_list.h>
#include <queues/task_queue.h>
#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <string/stdstring.h>
#include <vfs/vfs_implementation_romm.h>

#include "romm_launch.h"
#include "romm_library.h"
#include "../../verbosity.h"

/* RetroArch's HTTP client refuses replies over 256 MB. */
#define ROMM_LAUNCH_RANGE ((uint64_t)64 * 1024 * 1024)
/* Unpacked per task step, so the menu keeps drawing */
#define ROMM_LAUNCH_UNZIP_SLICE (4 * 1024 * 1024)
#define ROMM_LAUNCH_PFX "[RomM] "

enum romm_launch_state
{
   ROMM_LAUNCH_FILES = 0,
   /* Fetching the BIOS files the core lacks that RomM has */
   ROMM_LAUNCH_FIRMWARE,
   /* Fetching the files one after another, a range at a time */
   ROMM_LAUNCH_DOWNLOAD,
   /* Unpacking a zipped disc image, a slice at a time */
   ROMM_LAUNCH_UNZIP
};

/* A BIOS file to fetch: where it goes in the system folder and, once
 * matched, RomM's copy. */
typedef struct
{
   unsigned id;
   uint64_t size;
   char crc[9];
   char name[256];
   char path[256];
} romm_launch_bios_t;

typedef struct
{
   romm_launch_cb_t cb;
   void *user_data;
   retro_task_t *task;
   romm_rom_t rom;
   romm_file_t *files;
   romm_launch_bios_t *bios;
   /* The next of @bios to fetch, once RomM's list has matched them */
   size_t bios_next;
   bool bios_listed;
   /* The file being fetched or unpacked, its bytes so far and their
    * CRC32 */
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
   /* The one file of a folder game launched without a disc list,
    * fetched alone; or -1 */
   int alone;
   char *core_exts;
   retro_time_t started;
   enum romm_launch_state state;
   enum romm_status status;
   int http_status;
   bool pending;
   bool finished;
   char *token;
   char *unpack_title;
   file_archive_entry_source_t *unzip;
   char server[NAME_MAX_LENGTH];
   char system_dir[PATH_MAX_LENGTH];
   char content_path[PATH_MAX_LENGTH];
   /* What the loader is given: @content_path, or what a zip held */
   char launch_path[PATH_MAX_LENGTH];
   char zip_path[PATH_MAX_LENGTH];
   /* romm://<id>/, which every file of the game is under */
   char prefix[64];
} romm_launch_t;

/* The game last fetched, and what it loads as: a zip holding a disc is
 * gone once unpacked. */
static char romm_launch_held_content[PATH_MAX_LENGTH];
static char romm_launch_held_path[PATH_MAX_LENGTH];

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

bool romm_launch_is_ready(const char *content_path, char *launch_path,
      size_t len)
{
   const char *path = content_path;

   if (     content_path
         && !strcasecmp(content_path, romm_launch_held_content))
      path = romm_launch_held_path;
   if (!(retro_vfs_stat_romm(path, NULL) & RETRO_VFS_STAT_IS_VALID))
      return false;
   if (launch_path)
      strlcpy(launch_path, path, len);
   return true;
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

/* Disc descriptors, then disc images */
static const char *const romm_launch_disc_tiers[][6] = {
   { "cue", "gdi", "ccd", "mds", "toc", NULL },
   { "chd", "cdi", "iso", "pbp", "img", NULL }
};

/* Does the core load @path, going by its extension? Any file does
 * when the core's extensions aren't known. */
static bool romm_launch_core_takes(const romm_launch_t *launch,
      const char *path)
{
   const char *ext = path_get_extension(path);
   const char *p   = launch->core_exts;
   size_t len;

   if (!p || !*p)
      return true;
   if (!ext || !*ext)
      return false;
   len = strlen(ext);
   for (;;)
   {
      const char *bar = strchr(p, '|');
      size_t n        = bar ? (size_t)(bar - p) : strlen(p);
      if (n == len && !strncasecmp(p, ext, len))
         return true;
      if (!bar)
         return false;
      p = bar + 1;
   }
}

static bool romm_launch_is_disc(const char *path)
{
   return romm_launch_ext_is(path, romm_launch_disc_tiers[0])
       || romm_launch_ext_is(path, romm_launch_disc_tiers[1]);
}

static int romm_launch_cmp_path(const void *a, const void *b)
{
   return strcasecmp((*(const romm_file_t* const*)a)->path,
         (*(const romm_file_t* const*)b)->path);
}

/* Chooses what a folder game, or what a zip held, launches as: RomM's
 * own .m3u at the top of the folder, else a disc list of the disc
 * descriptors (.cue, .gdi, ...) in path order, else of the disc images,
 * else the first file the core takes, alone. Only what the core takes
 * counts, and a core that reads no disc list gets the first disc. False
 * when nothing can be launched. */
static bool romm_launch_pick_discs(romm_launch_t *launch)
{
   static const char *const m3u_ext[] = { "m3u", NULL };
   const romm_file_t **picked = NULL;
   size_t count               = RBUF_LEN(launch->files);
   bool takes_m3u             = romm_launch_core_takes(launch, "a.m3u");
   bool alone                 = false;
   int m3u                    = -1;
   size_t i, t, len;

   for (i = 0; i < count && takes_m3u; i++)
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

   for (t = 0; t < ARRAY_SIZE(romm_launch_disc_tiers) && !picked; t++)
      for (i = 0; i < count; i++)
         if (     romm_launch_ext_is(launch->files[i].path,
                     romm_launch_disc_tiers[t])
               && romm_launch_core_takes(launch, launch->files[i].path))
            RBUF_PUSH(picked, &launch->files[i]);
   /* A cartridge in a folder, beside a manual or a readme */
   if (!picked && launch->core_exts && *launch->core_exts)
   {
      for (i = 0; i < count; i++)
         if (romm_launch_core_takes(launch, launch->files[i].path))
            RBUF_PUSH(picked, &launch->files[i]);
      alone = true;
   }
   if (!picked)
      return false;

   qsort(picked, RBUF_LEN(picked), sizeof(*picked), romm_launch_cmp_path);
   if (alone || !takes_m3u)
   {
      snprintf(launch->launch_path, sizeof(launch->launch_path), "%s%s",
            launch->prefix, picked[0]->path);
      if (alone)
         launch->alone = (int)(picked[0] - launch->files);
      RBUF_FREE(picked);
      return true;
   }
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

/* Does the entry name a file other than the single-file game's own?
 * Its name is RomM's from the last sync; a disc unpacked from a zip,
 * as History holds it, has a name of its own. */
static bool romm_launch_renamed(const romm_launch_t *launch,
      const romm_file_t *file)
{
   static const char *const zip_ext[] = { "zip", NULL };
   const char *name = launch->content_path + strlen(launch->prefix);

   if (!strcasecmp(name, file->path))
      return false;
   return !romm_launch_ext_is(file->path, zip_ext)
       || romm_launch_ext_is(name, zip_ext);
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
   /* The file would be fetched under its new name and the old one
    * fail to load. */
   if (rom->single_file && romm_launch_renamed(launch, &files[0]))
   {
      RARCH_LOG(ROMM_LAUNCH_PFX "%s is %s in RomM now.\n",
            launch->content_path, files[0].path);
      romm_launch_finish(launch, ROMM_ERR_RENAMED, 0);
      return;
   }
   if (!rom->single_file && !romm_launch_pick_discs(launch))
   {
      romm_launch_finish(launch, ROMM_ERR_UNSUPPORTED, 0);
      return;
   }
   if (launch->alone >= 0)
   {
      romm_file_t f = launch->files[launch->alone];
      RBUF_RESIZE(launch->files, 1);
      launch->files[0] = f;
      launch->alone    = 0;
      launch->total    = f.size;
   }
   RARCH_LOG(ROMM_LAUNCH_PFX "Fetching %s: %u files, %llu bytes.\n",
         rom->fs_name, (unsigned)RBUF_LEN(launch->files),
         (unsigned long long)launch->total);
   launch->state = RBUF_LEN(launch->bios)
      ? ROMM_LAUNCH_FIRMWARE : ROMM_LAUNCH_DOWNLOAD;
}

/* Matches each BIOS file wanted to RomM's copy by name, a verified one
 * if there is one; those RomM lacks are dropped. */
static void romm_launch_firmware_list_cb(void *user_data,
      enum romm_status status, int http_status,
      const romm_firmware_t *firmware, size_t count)
{
   romm_launch_t *launch = (romm_launch_t*)user_data;
   size_t i, j, kept = 0;

   launch->pending     = false;
   launch->task->when  = 0;
   launch->bios_listed = true;
   if (status != ROMM_OK)
   {
      RBUF_CLEAR(launch->bios);
      return;
   }
   for (i = 0; i < RBUF_LEN(launch->bios); i++)
   {
      romm_launch_bios_t *b    = &launch->bios[i];
      const romm_firmware_t *f = NULL;

      for (j = 0; j < count; j++)
         if (     !strcasecmp(firmware[j].name, path_basename(b->path))
               && (!f || (firmware[j].verified && !f->verified)))
            f = &firmware[j];
      if (!f)
      {
         RARCH_LOG(ROMM_LAUNCH_PFX "RomM has no %s.\n", b->path);
         continue;
      }
      b->id   = f->id;
      b->size = f->size;
      strlcpy(b->crc, f->crc, sizeof(b->crc));
      strlcpy(b->name, f->name, sizeof(b->name));
      launch->bios[kept++] = *b;
   }
   RBUF_RESIZE(launch->bios, kept);
}

/* A BIOS file that doesn't match RomM's size or CRC32, or can't be
 * written, is left out; the core says what it misses. */
static void romm_launch_firmware_cb(void *user_data, enum romm_status status,
      int http_status, const void *data, size_t len)
{
   romm_launch_t *launch      = (romm_launch_t*)user_data;
   const romm_launch_bios_t *b = &launch->bios[launch->bios_next++];
   char path[PATH_MAX_LENGTH];
   char dir[PATH_MAX_LENGTH];

   launch->pending    = false;
   launch->task->when = 0;
   if (     status != ROMM_OK
         || (b->size && len != b->size)
         || (*b->crc && strtoul(b->crc, NULL, 16)
               != (unsigned long)encoding_crc32(0, (const uint8_t*)data, len)))
   {
      RARCH_WARN(ROMM_LAUNCH_PFX "Couldn't fetch BIOS %s (status %d).\n",
            b->path, status);
      return;
   }
   fill_pathname_join_special(path, launch->system_dir, b->path,
         sizeof(path));
   fill_pathname_basedir(dir, path, sizeof(dir));
   if (     (!path_is_directory(dir) && !path_mkdir(dir))
         || !filestream_write_file_atomic(path, data, (int64_t)len))
   {
      RARCH_WARN(ROMM_LAUNCH_PFX "Couldn't write %s.\n", path);
      return;
   }
   RARCH_LOG(ROMM_LAUNCH_PFX "Fetched BIOS %s.\n", path);
}

static void romm_launch_firmware_next(romm_launch_t *launch)
{
   bool started;

   if (!launch->bios_listed)
      started = romm_client_firmware(launch->server, launch->token,
            launch->rom.platform_id, romm_launch_firmware_list_cb, launch);
   else if (launch->bios_next < RBUF_LEN(launch->bios))
      started = romm_client_firmware_content(launch->server, launch->token,
            launch->bios[launch->bios_next].id,
            launch->bios[launch->bios_next].name,
            romm_launch_firmware_cb, launch);
   else
   {
      launch->state = ROMM_LAUNCH_DOWNLOAD;
      return;
   }
   if (started)
      launch->pending = true;
   else
      romm_launch_finish(launch, ROMM_ERR_UNREACHABLE, 0);
}

/* Hands the file just fetched or unpacked to the VFS. */
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

/* Given a zip, RetroArch launches its first member, never the .cue
 * inside; so a zip holding a disc is unpacked, and becomes the game's
 * files. True when it does. */
static bool romm_launch_unzip_begin(romm_launch_t *launch)
{
   static const char *const zip_ext[] = { "zip", NULL };
   struct string_list *members;
   romm_file_t *files = NULL;
   bool disc          = false;
   uint64_t total     = 0;
   size_t i;

   if (!romm_launch_ext_is(launch->files[0].path, zip_ext))
      return false;
   snprintf(launch->zip_path, sizeof(launch->zip_path), "%s%s",
         launch->prefix, launch->files[0].path);
   if (!(members = file_archive_get_file_list(launch->zip_path, NULL)))
      return false;

   for (i = 0; i < members->size; i++)
   {
      const char *name = members->elems[i].data;
      size_t len       = strlen(name);
      char path[PATH_MAX_LENGTH];
      romm_file_t f;

      if (!len || name[len - 1] == '/' || len >= sizeof(f.path))
         continue;
      memset(&f, 0, sizeof(f));
      strlcpy(f.name, path_basename(name), sizeof(f.name));
      strlcpy(f.path, name, sizeof(f.path));
      snprintf(path, sizeof(path), "%s#%s", launch->zip_path, name);
      snprintf(f.crc, sizeof(f.crc), "%08x", (unsigned)
            file_archive_get_file_crc32_and_size(path, &f.size));
      disc = disc || romm_launch_is_disc(name);
      total += f.size;
      RBUF_PUSH(files, f);
   }
   string_list_free(members);
   if (!disc)
   {
      RBUF_FREE(files);
      return false;
   }

   RBUF_FREE(launch->files);
   launch->files = files;
   launch->file  = 0;
   launch->done  = 0;
   launch->total = total;
   launch->state = ROMM_LAUNCH_UNZIP;
   if (launch->unpack_title)
   {
      task_free_title(launch->task);
      task_set_title(launch->task, launch->unpack_title);
      launch->unpack_title = NULL;
   }
   RARCH_LOG(ROMM_LAUNCH_PFX "Unpacking %s: %u files, %llu bytes.\n",
         path_basename(launch->zip_path), (unsigned)RBUF_LEN(files),
         (unsigned long long)total);
   return true;
}

/* A 7z holding a disc would launch its first member too, as a zip
 * would, but can't be unpacked a slice at a time. True when the game
 * is one. */
static bool romm_launch_7z_disc(const romm_launch_t *launch)
{
   static const char *const sevenzip_ext[] = { "7z", NULL };
   struct string_list *members;
   char path[PATH_MAX_LENGTH];
   bool disc = false;
   size_t i;

   if (!romm_launch_ext_is(launch->files[0].path, sevenzip_ext))
      return false;
   snprintf(path, sizeof(path), "%s%s", launch->prefix,
         launch->files[0].path);
   if (!(members = file_archive_get_file_list(path, NULL)))
      return false;
   for (i = 0; i < members->size && !disc; i++)
      disc = romm_launch_is_disc(members->elems[i].data);
   string_list_free(members);
   return disc;
}

/* The zip goes; the loader gets the game's .m3u, its one disc, or a
 * disc list made for it. */
static void romm_launch_unzip_end(romm_launch_t *launch)
{
   char *nl;

   retro_vfs_romm_remove(launch->zip_path);
   if (!romm_launch_pick_discs(launch))
   {
      romm_launch_finish(launch, ROMM_ERR_UNSUPPORTED, 0);
      return;
   }
   if (launch->m3u_file >= 0)
      snprintf(launch->launch_path, sizeof(launch->launch_path), "%s%s",
            launch->prefix, launch->files[launch->m3u_file].path);
   else if (!launch->list)
      ; /* launch_path is the one file picked */
   else if ((nl = strchr(launch->list, '\n')) && !nl[1])
   {
      *nl = '\0';
      snprintf(launch->launch_path, sizeof(launch->launch_path), "%s%s",
            launch->prefix, launch->list);
   }
   else
   {
      strlcpy(launch->launch_path, launch->zip_path,
            sizeof(launch->launch_path));
      path_remove_extension(launch->launch_path);
      strlcat(launch->launch_path, ".m3u", sizeof(launch->launch_path));
      if (!retro_vfs_romm_add(launch->launch_path, launch->list,
            launch->list_len))
      {
         romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
         return;
      }
      launch->list = NULL;
   }
   romm_launch_finish(launch, ROMM_OK, 0);
}

/* Unpacks the next slice of the current member. */
static void romm_launch_unzip_next(romm_launch_t *launch)
{
   const romm_file_t *f;
   int64_t want, got;

   if (launch->file >= RBUF_LEN(launch->files))
   {
      romm_launch_unzip_end(launch);
      return;
   }

   f = &launch->files[launch->file];
   if (!launch->unzip)
   {
      char path[PATH_MAX_LENGTH];
      int64_t size = -1;

      snprintf(path, sizeof(path), "%s#%s", launch->zip_path, f->path);
      launch->unzip = file_archive_entry_source_open(path, &size);
      if (!launch->unzip || (uint64_t)size != f->size)
      {
         romm_launch_finish(launch, ROMM_ERR_CHECKSUM, 0);
         return;
      }
      launch->have      = 0;
      launch->crc       = 0;
      launch->check_crc = true;
      if (f->size && !(launch->buf = (uint8_t*)malloc((size_t)f->size)))
      {
         romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
         return;
      }
   }

   want = (int64_t)(f->size - launch->have);
   if (want > ROMM_LAUNCH_UNZIP_SLICE)
      want = ROMM_LAUNCH_UNZIP_SLICE;
   if (want)
   {
      got = file_archive_entry_source_read(launch->unzip,
            launch->buf + launch->have, want);
      if (got <= 0)
      {
         romm_launch_finish(launch, ROMM_ERR_CHECKSUM, 0);
         return;
      }
      launch->crc   = encoding_crc32(launch->crc,
            launch->buf + launch->have, (size_t)got);
      launch->have += (uint64_t)got;
   }
   if (launch->have == f->size)
   {
      file_archive_entry_source_close(launch->unzip);
      launch->unzip = NULL;
      romm_launch_store_file(launch);
   }
}

/* Starts fetching the next range of the current file, or finishes the
 * game once every file is in. */
static void romm_launch_next(romm_launch_t *launch)
{
   const romm_file_t *f;
   uint64_t want;

   if (launch->file >= RBUF_LEN(launch->files))
   {
      /* A folder game of discs launches through its disc list. */
      if (!launch->rom.single_file && launch->list)
      {
         if (!retro_vfs_romm_add(launch->content_path, launch->list,
               launch->list_len))
         {
            romm_launch_finish(launch, ROMM_ERR_MEMORY, 0);
            return;
         }
         launch->list = NULL;
      }
      else if (romm_launch_unzip_begin(launch))
         return;
      else if (romm_launch_7z_disc(launch))
      {
         romm_launch_finish(launch, ROMM_ERR_UNSUPPORTED, 0);
         return;
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
      case ROMM_LAUNCH_FIRMWARE:
         romm_launch_firmware_next(launch);
         break;
      case ROMM_LAUNCH_DOWNLOAD:
         if (launch->total)
            task_set_progress(task, (int8_t)((launch->done + launch->have)
                  * 100 / launch->total));
         romm_launch_next(launch);
         break;
      case ROMM_LAUNCH_UNZIP:
         if (launch->total)
            task_set_progress(task, (int8_t)((launch->done + launch->have)
                  * 100 / launch->total));
         romm_launch_unzip_next(launch);
         break;
   }
}

static void romm_launch_cb(retro_task_t *task, void *task_data,
      void *user_data, const char *err)
{
   romm_launch_t *launch = (romm_launch_t*)user_data;

   if (launch->status == ROMM_OK)
   {
      strlcpy(romm_launch_held_content, launch->content_path,
            sizeof(romm_launch_held_content));
      strlcpy(romm_launch_held_path, launch->launch_path,
            sizeof(romm_launch_held_path));
      RARCH_LOG(ROMM_LAUNCH_PFX "Fetched %s into memory in %u ms.\n",
            launch->rom.fs_name, (unsigned)((cpu_features_get_time_usec()
               - launch->started) / 1000));
   }
   else
   {
      /* No half-fetched game is left behind. */
      retro_vfs_romm_remove_prefix(launch->prefix);
      RARCH_WARN(ROMM_LAUNCH_PFX "Couldn't fetch %s (status %d).\n",
            launch->content_path, launch->status);
   }
   if (launch->cb)
      launch->cb(launch->user_data, launch->status, launch->http_status,
            launch->status == ROMM_OK ? launch->launch_path : NULL);
}

static void romm_launch_cleanup(retro_task_t *task)
{
   romm_launch_t *launch = (romm_launch_t*)task->state;

   if (launch)
   {
      file_archive_entry_source_close(launch->unzip);
      RBUF_FREE(launch->files);
      RBUF_FREE(launch->bios);
      free(launch->buf);
      free(launch->list);
      free(launch->unpack_title);
      free(launch->core_exts);
      if (launch->token)
      {
         memset(launch->token, 0, strlen(launch->token));
         free(launch->token);
      }
      free(launch);
   }
   task->state = NULL;
}

bool romm_launch_prepare(const romm_launch_params_t *params,
      romm_launch_cb_t cb, void *user_data)
{
   romm_launch_t *launch;
   retro_task_t *task;
   const char *content_path = params->content_path;
   unsigned rom_id          = romm_launch_rom_id(content_path);
   size_t i;

   if (     !rom_id || string_is_empty(params->server)
         || string_is_empty(params->token))
      return false;
   if (!(launch = (romm_launch_t*)calloc(1, sizeof(*launch))))
      return false;
   if (     !(launch->token = strdup(params->token))
         || (params->unpack_title
            && !(launch->unpack_title = strdup(params->unpack_title)))
         || (params->core_exts
            && !(launch->core_exts = strdup(params->core_exts)))
         || !(task = task_init()))
   {
      free(launch->token);
      free(launch->unpack_title);
      free(launch->core_exts);
      free(launch);
      return false;
   }

   launch->cb        = cb;
   launch->user_data = user_data;
   launch->task      = task;
   launch->m3u_file  = -1;
   launch->alone     = -1;
   launch->started   = cpu_features_get_time_usec();
   strlcpy(launch->server, params->server, sizeof(launch->server));
   strlcpy(launch->content_path, content_path,
         sizeof(launch->content_path));
   strlcpy(launch->launch_path, content_path,
         sizeof(launch->launch_path));
   snprintf(launch->prefix, sizeof(launch->prefix), ROMM_SCHEME "%u/",
         rom_id);
   if (!string_is_empty(params->system_dir))
   {
      strlcpy(launch->system_dir, params->system_dir,
            sizeof(launch->system_dir));
      for (i = 0; i < params->firmware_count; i++)
      {
         romm_launch_bios_t b;
         memset(&b, 0, sizeof(b));
         strlcpy(b.path, params->firmware[i], sizeof(b.path));
         RBUF_PUSH(launch->bios, b);
      }
   }

   task->handler   = romm_launch_handler;
   task->callback  = romm_launch_cb;
   task->cleanup   = romm_launch_cleanup;
   task->state     = launch;
   task->user_data = launch;
   task->flags    |= RETRO_TASK_FLG_MAIN_THREAD;
   if (params->title)
   {
      task->title    = strdup(params->title);
      task->progress = 0;
   }
   else
      task->flags   |= RETRO_TASK_FLG_MUTE;

   if (!task_queue_push(task))
   {
      free(task->title);
      free(task);
      RBUF_FREE(launch->bios);
      free(launch->token);
      free(launch->unpack_title);
      free(launch->core_exts);
      free(launch);
      return false;
   }

   /* One game in memory at a time. A core still reading the last one
    * keeps its open files until it closes them. */
   retro_vfs_romm_remove_prefix(ROMM_SCHEME);
   RARCH_LOG(ROMM_LAUNCH_PFX "Preparing %s.\n", content_path);
   return true;
}
