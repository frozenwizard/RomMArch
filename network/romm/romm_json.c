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

#include <compat/strl.h>
#include <string/stdstring.h>

#include "romm_json.h"

/* Walks the objects in the top-level array, or in the array at
 * @array_key of the top-level object, skipping nested values. False
 * unless that array is there and complete. */
bool romm_json_items(const char *data, size_t len,
      const char *array_key, romm_json_member_cb_t member,
      romm_json_item_end_cb_t item_end, void *ctx)
{
   /* Depth the array's own items are at; 0 until it starts. */
   unsigned item_depth = 0;
   bool key_matched    = false;
   bool complete       = false;
   rjson_t *json;
   enum rjson_type type;
   char key[64];

   if (!data || !len || !(json = rjson_open_buffer(data, len)))
      return false;

   key[0] = '\0';
   while ((type = rjson_next(json)) != RJSON_DONE && type != RJSON_ERROR)
   {
      /* rjson reports an opening bracket at its own depth and a
       * closing one at the depth around it. */
      unsigned depth = rjson_get_context_depth(json);
      bool is_key    = type == RJSON_STRING
            && rjson_get_context_type(json) == RJSON_OBJECT
            && (rjson_get_context_count(json) & 1);
      const char *str;
      size_t _len;

      if (!item_depth)
      {
         if (type == RJSON_ARRAY && depth == (array_key ? 2u : 1u)
               && (!array_key || key_matched))
            item_depth = depth + 1;
         else if (array_key && depth == 1)
         {
            key_matched = false;
            if (is_key)
            {
               str         = rjson_get_string(json, &_len);
               key_matched = string_is_equal(str, array_key);
            }
         }
         continue;
      }

      if (depth + 1 < item_depth)
      {
         complete = true;
         break;
      }
      if (type == RJSON_OBJECT_END && depth + 1 == item_depth)
      {
         if (item_end)
            item_end(ctx);
         continue;
      }
      if (     depth != item_depth
            || rjson_get_context_type(json) != RJSON_OBJECT)
         continue;

      switch (type)
      {
         case RJSON_STRING:
         case RJSON_NUMBER:
            str = rjson_get_string(json, &_len);
            if (is_key)
               strlcpy(key, str, sizeof(key));
            else
               member(ctx, key, type, str, _len);
            break;
         case RJSON_TRUE:
         case RJSON_FALSE:
         case RJSON_NULL:
            member(ctx, key, type, "", 0);
            break;
         default:
            break;
      }
   }

   rjson_free(json);
   return complete;
}
