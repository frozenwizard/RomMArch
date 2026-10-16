/* Harness for the romm:// in-memory files
 * (libretro-common/vfs/vfs_implementation_romm.c), used the way
 * RetroArch and cores use them: through filestream_* and path_*, into
 * the VFS. Covers reading, seeking and sizes, case-insensitive paths,
 * directories, read-only access, files removed (one game's, or all
 * but one game's) or replaced while open, and readers on other
 * threads, including ones that start before the first file is added.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <file/file_path.h>
#include <rthreads/rthreads.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation_romm.h>

static unsigned failures = 0;

#define CHECK(cond, ...) \
   do { \
      if (!(cond)) \
      { \
         printf("FAIL %s:%d: ", __FILE__, __LINE__); \
         printf(__VA_ARGS__); \
         printf("\n"); \
         failures++; \
      } \
   } while (0)

static void *bytes(const char *s)
{
   size_t len = strlen(s);
   char *p    = (char*)malloc(len);
   memcpy(p, s, len);
   return p;
}

static void test_read_seek(void)
{
   RFILE *f;
   char buf[32];

   CHECK(retro_vfs_romm_add("romm://12/Game (USA).bin",
         bytes("0123456789abcdef"), 16), "add");

   CHECK((f = filestream_open("romm://12/Game (USA).bin",
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE)) != NULL,
         "open");
   if (!f)
      return;
   CHECK(filestream_get_size(f) == 16, "size %lld",
         (long long)filestream_get_size(f));
   CHECK(filestream_read(f, buf, 4) == 4 && !memcmp(buf, "0123", 4), "read");
   CHECK(filestream_tell(f) == 4, "tell %lld", (long long)filestream_tell(f));
   CHECK(filestream_seek(f, 6, RETRO_VFS_SEEK_POSITION_CURRENT) == 10
         && filestream_read(f, buf, 2) == 2 && !memcmp(buf, "ab", 2),
         "seek current");
   CHECK(filestream_seek(f, -3, RETRO_VFS_SEEK_POSITION_END) == 13
         && filestream_read(f, buf, 32) == 3 && !memcmp(buf, "def", 3),
         "seek end, short read");
   CHECK(filestream_read(f, buf, 4) == 0, "read at end");
   CHECK(filestream_seek(f, 100, RETRO_VFS_SEEK_POSITION_START) == 100
         && filestream_read(f, buf, 4) == 0, "past the end");
   CHECK(filestream_seek(f, -1, RETRO_VFS_SEEK_POSITION_START) < 0,
         "seek before the start");
   CHECK(filestream_write(f, "x", 1) < 0, "write to a romm:// file");
   filestream_close(f);

   /* Cores asking for mapped or bulk access still get the memory file. */
   CHECK((f = filestream_open("romm://12/Game (USA).bin",
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_FREQUENT_ACCESS)) != NULL
         && filestream_read(f, buf, 3) == 3 && !memcmp(buf, "012", 3),
         "frequent-access hint");
   filestream_close(f);
   CHECK((f = filestream_open("romm://12/Game (USA).bin",
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_SEQUENTIAL_BULK)) != NULL
         && filestream_read(f, buf, 3) == 3, "sequential-bulk hint");
   filestream_close(f);

   CHECK(!filestream_open("romm://12/Game (USA).bin",
         RETRO_VFS_FILE_ACCESS_WRITE, RETRO_VFS_FILE_ACCESS_HINT_NONE),
         "opened for writing");
   CHECK(!filestream_open("romm://12/Other.bin",
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE),
         "opened a file that isn't there");
}

static void test_paths(void)
{
   void *data   = NULL;
   int64_t len  = 0;

   /* Cue sheets name tracks in whatever case they like. */
   CHECK(path_is_valid("romm://12/GAME (usa).BIN"), "case-insensitive stat");
   CHECK(path_get_size("romm://12/Game (USA).bin") == 16, "path size");
   CHECK(path_is_directory("romm://12") && path_is_directory("romm://12/"),
         "the game's folder");
   /* Cores on Windows join a disc list's or cue sheet's folder and
    * file names with a backslash. */
   CHECK(path_is_valid("romm://12\\Game (USA).bin")
         && path_get_size("romm://12\\game (usa).bin") == 16
         && path_is_directory("romm://12\\"), "backslash separator");
   CHECK(!path_is_directory("romm://1") && !path_is_valid("romm://12/x"),
         "folder or file that isn't there");
   CHECK(!path_is_directory("romm://12/Game (USA).bin"),
         "a file reported as a folder");
   CHECK(filestream_read_file("romm://12/game (usa).bin", &data, &len)
         && len == 16 && !memcmp(data, "0123456789abcdef", 16),
         "whole-file read");
   free(data);
}

static void test_remove_replace(void)
{
   RFILE *f;
   char buf[8];
   void *x;

   f = filestream_open("romm://12/Game (USA).bin",
         RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
   CHECK(retro_vfs_romm_add("romm://12/game (usa).bin", bytes("NEW"), 3),
         "replace");
   CHECK(path_get_size("romm://12/Game (USA).bin") == 3, "replaced size");
   CHECK(f && filestream_read(f, buf, 4) == 4 && !memcmp(buf, "0123", 4),
         "open handle lost the old bytes");
   CHECK(retro_vfs_romm_remove("romm://12/Game (USA).bin")
         && !path_is_valid("romm://12/Game (USA).bin"), "remove");
   CHECK(!retro_vfs_romm_remove("romm://12/Game (USA).bin"), "removed twice");
   CHECK(f && filestream_read(f, buf, 2) == 2 && !memcmp(buf, "45", 2),
         "open handle lost a removed file");
   filestream_close(f);

   retro_vfs_romm_add("romm://12/a.cue", bytes("a"), 1);
   retro_vfs_romm_add("romm://12/b.bin", bytes("b"), 1);
   retro_vfs_romm_add("romm://120/c.bin", bytes("c"), 1);
   retro_vfs_romm_remove_prefix("romm://12/");
   CHECK(!path_is_valid("romm://12/a.cue") && !path_is_valid("romm://12/b.bin")
         && path_is_valid("romm://120/c.bin"), "remove a game's files");
   retro_vfs_romm_add("romm://12/a.cue", bytes("a"), 1);
   retro_vfs_romm_keep_prefix("romm://12/");
   CHECK(path_is_valid("romm://12/a.cue") && !path_is_valid("romm://120/c.bin"),
         "keep a game's files");
   retro_vfs_romm_keep_prefix("romm://12/");
   CHECK(path_is_valid("romm://12/a.cue"), "keep twice");
   retro_vfs_romm_remove_prefix("romm://");

   /* A refused add leaves the data with the caller. */
   x = bytes("x");
   CHECK(!retro_vfs_romm_add("", x, 1), "empty path");
   free(x);
   CHECK(retro_vfs_romm_add("romm://13/empty.bin", NULL, 0)
         && path_is_valid("romm://13/empty.bin")
         && path_get_size("romm://13/empty.bin") == 0, "empty file");
   retro_vfs_romm_remove("romm://13/empty.bin");
}

#define THREAD_READS 2000

static void early_reader(void *arg)
{
   unsigned *seen = (unsigned*)arg;
   unsigned i;
   for (i = 0; i < THREAD_READS && !*seen; i++)
   {
      RFILE *f = filestream_open("romm://15/first.bin",
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (!f)
         continue;
      if (path_is_valid("romm://15/first.bin"))
         (*seen)++;
      filestream_close(f);
   }
}

/* Cores can look for romm:// files on their own threads before the
 * frontend has added any; run first, before anything else adds one. */
static void test_threads_before_first_add(void)
{
   unsigned seen[2] = { 0, 0 };
   sthread_t *t[2];

   t[0] = sthread_create(early_reader, &seen[0]);
   t[1] = sthread_create(early_reader, &seen[1]);
   retro_vfs_romm_add("romm://15/first.bin", bytes("1"), 1);
   sthread_join(t[0]);
   sthread_join(t[1]);
   retro_vfs_romm_remove_prefix("romm://");
}

static void reader(void *arg)
{
   unsigned *bad = (unsigned*)arg;
   unsigned i;
   for (i = 0; i < THREAD_READS; i++)
   {
      char buf[4];
      RFILE *f = filestream_open("romm://14/disc.bin",
            RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
      if (!f)
         continue;
      if (filestream_read(f, buf, 4) != 4 || memcmp(buf, "disc", 4))
         (*bad)++;
      filestream_close(f);
   }
}

/* Cores read discs on threads of their own while the frontend adds and
 * removes files. */
static void test_threads(void)
{
   unsigned bad[2] = { 0, 0 };
   sthread_t *t[2];
   unsigned i;

   retro_vfs_romm_add("romm://14/disc.bin", bytes("disc"), 4);
   t[0] = sthread_create(reader, &bad[0]);
   t[1] = sthread_create(reader, &bad[1]);
   for (i = 0; i < THREAD_READS; i++)
   {
      retro_vfs_romm_add("romm://14/other.bin", bytes("o"), 1);
      retro_vfs_romm_remove("romm://14/other.bin");
   }
   sthread_join(t[0]);
   sthread_join(t[1]);
   CHECK(!bad[0] && !bad[1], "bad reads %u %u", bad[0], bad[1]);
   retro_vfs_romm_remove_prefix("romm://");
}

int main(void)
{
   test_threads_before_first_add();
   test_read_seek();
   test_paths();
   test_remove_replace();
   test_threads();

   if (failures)
   {
      printf("\n%u check(s) failed\n", failures);
      return 1;
   }
   printf("\nall romm:// VFS checks passed\n");
   return 0;
}
