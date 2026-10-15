/* Harness for the Windows and Linux side of the RomM frontend
 * (network/romm/romm_platform.c): each new install identifier is a
 * fresh random UUID, the device name is the computer's, and on Linux
 * the approval link goes to xdg-open as one argument, through no
 * shell.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#endif

#include <boolean.h>

#include "../../../network/romm/romm_platform.h"

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

static bool is_uuid4(const char *s)
{
   size_t i;

   if (strlen(s) != 36)
      return false;
   for (i = 0; i < 36; i++)
   {
      char c = s[i];
      if (i == 8 || i == 13 || i == 18 || i == 23)
      {
         if (c != '-')
            return false;
      }
      else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
         return false;
   }
   return s[14] == '4' && strchr("89ab", s[19]);
}

static void test_identifier(void)
{
   char a[64];
   char b[64];
   char cut[9];

   CHECK(romm_platform_new_identifier(a, sizeof(a)) == 36 && is_uuid4(a),
         "identifier %s", a);
   CHECK(romm_platform_new_identifier(b, sizeof(b)) == 36 && is_uuid4(b)
         && strcmp(a, b), "second identifier %s after %s", b, a);
   CHECK(romm_platform_new_identifier(cut, sizeof(cut)) == 36
         && strlen(cut) == 8, "cut short: %s", cut);
}

static void test_device_name(void)
{
   char name[128];

   CHECK(romm_platform_device_name(name, sizeof(name)) == strlen(name)
         && *name, "device name %s", name);
#ifndef _WIN32
   {
      char host[256];
      CHECK(!gethostname(host, sizeof(host)) && !strcmp(name, host),
            "device name %s, host name %s", name, host);
   }
#endif
   printf("device name: %s\n", name);
}

#ifndef _WIN32
#define BIN_DIR "romm_platform_test_bin"

/* An xdg-open first on the PATH writes down what it was given. */
static void test_open_url(void)
{
   const char *url = "http://romm.local:8090/pair?code=AB12&x='$(touch "
      BIN_DIR "/shell)'";
   char cwd[1024];
   char path[4096];
   char got[256];
   const char *old_path = getenv("PATH");
   FILE *f;
   size_t len = 0;
   unsigned i;

   mkdir(BIN_DIR, 0755);
   remove(BIN_DIR "/opened");
   if (!(f = fopen(BIN_DIR "/xdg-open", "w")))
   {
      CHECK(false, "can't write %s/xdg-open", BIN_DIR);
      return;
   }
   fputs("#!/bin/sh\n"
         "printf '%s' \"$1\" > " BIN_DIR "/opened.tmp\n"
         "mv " BIN_DIR "/opened.tmp " BIN_DIR "/opened\n", f);
   fclose(f);
   chmod(BIN_DIR "/xdg-open", 0755);
   if (!getcwd(cwd, sizeof(cwd)))
      return;
   snprintf(path, sizeof(path), "%s/" BIN_DIR ":%s", cwd,
         old_path ? old_path : "/usr/bin:/bin");
   setenv("PATH", path, 1);

   CHECK(romm_platform_open_url(url), "open refused");
   CHECK(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD,
         "a child left to reap");
   /* It runs detached; give it a few seconds. */
   for (i = 0; i < 500 && !(f = fopen(BIN_DIR "/opened", "r")); i++)
      usleep(10000);
   if (f)
   {
      len      = fread(got, 1, sizeof(got) - 1, f);
      got[len] = '\0';
      fclose(f);
   }
   CHECK(f && !strcmp(got, url), "xdg-open was given %s",
         f ? got : "nothing");
   CHECK(!(f = fopen(BIN_DIR "/shell", "r")), "the link went through a shell");
   if (f)
      fclose(f);

   /* No xdg-open at all: only the detached opener fails. */
   setenv("PATH", "/nonexistent", 1);
   romm_platform_open_url(url);
   CHECK(waitpid(-1, NULL, WNOHANG) < 0 && errno == ECHILD,
         "a child left to reap without xdg-open");
   if (old_path)
      setenv("PATH", old_path, 1);
}
#endif

int main(void)
{
   test_identifier();
   test_device_name();
#ifndef _WIN32
   test_open_url();
#endif

   if (failures)
   {
      printf("%u check(s) failed\n", failures);
      return 1;
   }
   printf("all RomM platform checks passed\n");
   return 0;
}
