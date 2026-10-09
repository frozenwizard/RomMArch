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

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

#include <compat/strl.h>

#include "romm_platform.h"

bool romm_platform_open_url(const char *url)
{
   @autoreleasepool
   {
      NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
      return u && [[NSWorkspace sharedWorkspace] openURL:u];
   }
}

size_t romm_platform_device_name(char *s, size_t len)
{
   @autoreleasepool
   {
      NSString *name = [[NSHost currentHost] localizedName];
      if (!name.length)
         name = [[NSProcessInfo processInfo] hostName];
      return strlcpy(s, name.length ? name.UTF8String : "Mac", len);
   }
}

size_t romm_platform_new_identifier(char *s, size_t len)
{
   @autoreleasepool
   {
      return strlcpy(s, [[NSUUID UUID] UUIDString].lowercaseString.UTF8String,
            len);
   }
}
