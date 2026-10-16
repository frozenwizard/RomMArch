/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (vfs_implementation_romm.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include <libretro.h>
#ifdef HAVE_THREADS
#include <retro_atomic.h>
#include <rthreads/rthreads.h>
#endif

#include <vfs/vfs_implementation_romm.h>

typedef struct romm_vfs_file
{
   struct romm_vfs_file *next;
   char *path;
   uint8_t *data;
   uint64_t size;
   /* Open handles, plus one while the file is in the list */
   unsigned refs;
} romm_vfs_file_t;

static romm_vfs_file_t *romm_vfs_files = NULL;

/* Compares up to @n characters of two paths without regard to case or
 * to which slash separates their parts: cores on Windows join the
 * paths in disc lists and cue sheets with '\\'. */
static int romm_vfs_pathncmp(const char *a, const char *b, size_t n)
{
   for (; n; n--, a++, b++)
   {
      int ca = (*a == '\\') ? '/' : tolower((unsigned char)*a);
      int cb = (*b == '\\') ? '/' : tolower((unsigned char)*b);
      if (ca != cb)
         return ca - cb;
      if (!ca)
         break;
   }
   return 0;
}

#define romm_vfs_pathcmp(a, b) romm_vfs_pathncmp((a), (b), (size_t)-1)

#ifdef HAVE_THREADS
/* Made by whichever thread first needs it, as cores can open romm://
 * paths on threads of their own; kept for the life of the process. */
static retro_atomic_ptr_t romm_vfs_lock_ptr;   /* slock_t * */

static slock_t *romm_vfs_lock(void)
{
   slock_t *l = (slock_t*)retro_atomic_load_acquire_ptr(&romm_vfs_lock_ptr);
   if (!l)
   {
      slock_t *fresh = slock_new();
      if (!fresh)
         return NULL;
      if (retro_atomic_cas_ptr(&romm_vfs_lock_ptr, NULL, fresh))
         l = fresh;
      else
      {
         slock_free(fresh);             /* another thread published first */
         l = (slock_t*)retro_atomic_load_acquire_ptr(&romm_vfs_lock_ptr);
      }
   }
   return l;
}
#endif

/* NULL, and nothing locked, only when the lock couldn't be made. */
static void *romm_vfs_lock_acquire(void)
{
#ifdef HAVE_THREADS
   slock_t *l = romm_vfs_lock();
   if (l)
      slock_lock(l);
   return l;
#else
   return NULL;
#endif
}

static void romm_vfs_lock_release(void *l)
{
#ifdef HAVE_THREADS
   if (l)
      slock_unlock((slock_t*)l);
#endif
}

/* Called with the lock held. */
static void romm_vfs_unref(romm_vfs_file_t *file)
{
   if (--file->refs)
      return;
   free(file->data);
   free(file->path);
   free(file);
}

/* Takes @file out of the list; called with the lock held. */
static void romm_vfs_unlink(romm_vfs_file_t **link)
{
   romm_vfs_file_t *file = *link;
   *link                 = file->next;
   file->next            = NULL;
   romm_vfs_unref(file);
}

bool retro_vfs_romm_add(const char *path, void *data, uint64_t size)
{
   romm_vfs_file_t *file;
   romm_vfs_file_t **link;
   void *lock;

   if (!path || !*path || (!data && size))
      return false;
#ifdef HAVE_THREADS
   if (!romm_vfs_lock())
      return false;
#endif
   if (!(file = (romm_vfs_file_t*)calloc(1, sizeof(*file))))
      return false;
   if (!(file->path = strdup(path)))
   {
      free(file);
      return false;
   }
   file->data = (uint8_t*)data;
   file->size = size;
   file->refs = 1;

   lock = romm_vfs_lock_acquire();
   for (link = &romm_vfs_files; *link; link = &(*link)->next)
      if (!romm_vfs_pathcmp((*link)->path, path))
      {
         romm_vfs_unlink(link);
         break;
      }
   file->next     = romm_vfs_files;
   romm_vfs_files = file;
   romm_vfs_lock_release(lock);
   return true;
}

bool retro_vfs_romm_remove(const char *path)
{
   romm_vfs_file_t **link;
   void *lock;
   bool found = false;

   if (!path)
      return false;
   lock = romm_vfs_lock_acquire();
   for (link = &romm_vfs_files; *link; link = &(*link)->next)
      if (!romm_vfs_pathcmp((*link)->path, path))
      {
         romm_vfs_unlink(link);
         found = true;
         break;
      }
   romm_vfs_lock_release(lock);
   return found;
}

/* Removes the files under @prefix or, with @others, the rest. */
static void romm_vfs_remove_matching(const char *prefix, bool others)
{
   romm_vfs_file_t **link;
   void *lock;
   size_t len;

   if (!prefix)
      return;
   len  = strlen(prefix);
   lock = romm_vfs_lock_acquire();
   for (link = &romm_vfs_files; *link; )
   {
      if (!romm_vfs_pathncmp((*link)->path, prefix, len) != others)
         romm_vfs_unlink(link);
      else
         link = &(*link)->next;
   }
   romm_vfs_lock_release(lock);
}

void retro_vfs_romm_remove_prefix(const char *prefix)
{
   romm_vfs_remove_matching(prefix, false);
}

void retro_vfs_romm_keep_prefix(const char *prefix)
{
   romm_vfs_remove_matching(prefix, true);
}

bool retro_vfs_file_open_romm(libretro_vfs_implementation_file *stream,
      const char *path, unsigned mode)
{
   romm_vfs_file_t *file;
   void *lock;

   /* Downloaded games are never written to. */
   if (mode != RETRO_VFS_FILE_ACCESS_READ)
      return false;

   lock = romm_vfs_lock_acquire();
   for (file = romm_vfs_files; file; file = file->next)
      if (!romm_vfs_pathcmp(file->path, path))
      {
         file->refs++;
         break;
      }
   romm_vfs_lock_release(lock);

   if (!file)
      return false;
   stream->romm_file = file;
   stream->romm_pos  = 0;
   stream->size      = (int64_t)file->size;
   return true;
}

int64_t retro_vfs_file_read_romm(libretro_vfs_implementation_file *stream,
      void *s, uint64_t len)
{
   romm_vfs_file_t *file = (romm_vfs_file_t*)stream->romm_file;
   uint64_t left;

   if (!file || stream->romm_pos < 0)
      return -1;
   if ((uint64_t)stream->romm_pos >= file->size)
      return 0;
   left = file->size - (uint64_t)stream->romm_pos;
   if (len > left)
      len = left;
   memcpy(s, file->data + stream->romm_pos, (size_t)len);
   stream->romm_pos += (int64_t)len;
   return (int64_t)len;
}

int64_t retro_vfs_file_seek_romm(libretro_vfs_implementation_file *stream,
      int64_t offset, int whence)
{
   romm_vfs_file_t *file = (romm_vfs_file_t*)stream->romm_file;
   int64_t pos;

   if (!file)
      return -1;
   switch (whence)
   {
      case RETRO_VFS_SEEK_POSITION_START:
         pos = offset;
         break;
      case RETRO_VFS_SEEK_POSITION_CURRENT:
         pos = stream->romm_pos + offset;
         break;
      case RETRO_VFS_SEEK_POSITION_END:
         pos = (int64_t)file->size + offset;
         break;
      default:
         return -1;
   }
   /* Past the end is allowed, as with stdio; reads there return 0. */
   if (pos < 0)
      return -1;
   stream->romm_pos = pos;
   return pos;
}

int64_t retro_vfs_file_tell_romm(libretro_vfs_implementation_file *stream)
{
   return stream->romm_file ? stream->romm_pos : -1;
}

void retro_vfs_file_close_romm(libretro_vfs_implementation_file *stream)
{
   romm_vfs_file_t *file = (romm_vfs_file_t*)stream->romm_file;
   void *lock;

   if (!file)
      return;
   lock = romm_vfs_lock_acquire();
   romm_vfs_unref(file);
   romm_vfs_lock_release(lock);
   stream->romm_file = NULL;
}

int retro_vfs_stat_romm(const char *path, int64_t *size)
{
   romm_vfs_file_t *file;
   void *lock;
   size_t len;
   int flags = 0;

   if (!path || !*path)
      return 0;
   len = strlen(path);
   /* A directory named with or without its trailing slash */
   if (path[len - 1] == '/' || path[len - 1] == '\\')
      len--;

   lock = romm_vfs_lock_acquire();
   for (file = romm_vfs_files; file; file = file->next)
   {
      if (!romm_vfs_pathcmp(file->path, path))
      {
         flags = RETRO_VFS_STAT_IS_VALID | RETRO_VFS_STAT_IS_READONLY;
         if (size)
            *size = (int64_t)file->size;
         break;
      }
      if (     !romm_vfs_pathncmp(file->path, path, len)
            && file->path[len] == '/')
         flags = RETRO_VFS_STAT_IS_VALID | RETRO_VFS_STAT_IS_DIRECTORY;
   }
   romm_vfs_lock_release(lock);
   return flags;
}
