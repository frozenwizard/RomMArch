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
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

#include <compat/strl.h>
#include <crypto/kdf.h>
#include <encodings/utf.h>

#include "romm_platform.h"

#ifdef _WIN32
bool romm_platform_open_url(const char *url)
{
   wchar_t *wide = utf8_to_utf16_string_alloc(url);
   bool opened;

   if (!wide)
      return false;
   opened = (INT_PTR)ShellExecuteW(NULL, L"open", wide, NULL, NULL,
         SW_SHOWNORMAL) > 32;
   free(wide);
   return opened;
}

size_t romm_platform_device_name(char *s, size_t len)
{
   wchar_t name[256];
   DWORD size = sizeof(name) / sizeof(name[0]);
   char *utf8;

   /* The DNS host name keeps the case it was given; the NetBIOS name
    * is in capitals and cut at 15 characters. */
   if (     GetComputerNameExW(ComputerNameDnsHostname, name, &size)
         && size
         && (utf8 = utf16_to_utf8_string_alloc(name)))
   {
      size_t _len = strlcpy(s, utf8, len);
      free(utf8);
      return _len;
   }
   return strlcpy(s, "Windows PC", len);
}
#else
bool romm_platform_open_url(const char *url)
{
   int status;
   pid_t pid = fork();

   if (pid == 0)
   {
      /* A grandchild opens it, so none of ours is left to reap. */
      pid_t opener = fork();
      if (opener == 0)
      {
         execlp("xdg-open", "xdg-open", url, (char*)NULL);
         _exit(127);
      }
      _exit(opener < 0);
   }
   if (pid < 0)
      return false;
   while (waitpid(pid, &status, 0) < 0)
      if (errno != EINTR)
         return false;
   return WIFEXITED(status) && !WEXITSTATUS(status);
}

size_t romm_platform_device_name(char *s, size_t len)
{
   char name[256];

   if (!gethostname(name, sizeof(name)) && *name)
   {
      name[sizeof(name) - 1] = '\0';
      return strlcpy(s, name, len);
   }
   return strlcpy(s, "Linux", len);
}
#endif

size_t romm_platform_new_identifier(char *s, size_t len)
{
   uint8_t b[16];

   if (crypto_random_bytes(b, sizeof(b)) != 0)
   {
      if (len)
         *s = '\0';
      return 0;
   }
   /* A version 4 (random) UUID, like the Mac's NSUUID. */
   b[6] = (b[6] & 0x0f) | 0x40;
   b[8] = (b[8] & 0x3f) | 0x80;
   return (size_t)snprintf(s, len,
         "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
         "%02x%02x%02x%02x%02x%02x",
         b[0], b[1], b[2],  b[3],  b[4],  b[5],  b[6],  b[7],
         b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}
