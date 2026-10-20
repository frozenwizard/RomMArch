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
#include <string.h>

#include <compat/strl.h>
#include <string/stdstring.h>

#include "romm_json.h"

#define ROMM_JSON_MAX_KEYS 4

/* Copies the string or number at @path, object keys joined by '.'
 * such as "SYSTEM.VERSION". */
bool romm_json_get(const char *data, size_t len, const char *path,
      char *s, size_t s_len)
{
   const char *keys[ROMM_JSON_MAX_KEYS];
   size_t key_lens[ROMM_JSON_MAX_KEYS];
   unsigned num_keys = 0;
   /* How many of @keys the object being parsed is nested under. */
   unsigned matched  = 0;
   bool at_key       = false;
   bool found        = false;
   const char *p     = path;
   rjson_t *json;
   enum rjson_type type;

   for (;;)
   {
      const char *dot = strchr(p, '.');
      if (num_keys == ROMM_JSON_MAX_KEYS)
         return false;
      keys[num_keys]     = p;
      key_lens[num_keys] = dot ? (size_t)(dot - p) : strlen(p);
      num_keys++;
      if (!dot)
         break;
      p = dot + 1;
   }

   if (!data || !len || !(json = rjson_open_buffer(data, len)))
      return false;

   while (!found
         && (type = rjson_next(json)) != RJSON_DONE
         && type != RJSON_ERROR)
   {
      unsigned depth = rjson_get_context_depth(json);

      if (at_key)
      {
         at_key = false;
         if (type == RJSON_OBJECT && matched + 1 < num_keys)
            matched++;
         else if ((type == RJSON_STRING || type == RJSON_NUMBER)
               && matched + 1 == num_keys)
         {
            size_t _len;
            const char *str = rjson_get_string(json, &_len);
            strlcpy(s, str, s_len);
            found = true;
         }
         else if ((type == RJSON_TRUE || type == RJSON_FALSE)
               && matched + 1 == num_keys)
         {
            strlcpy(s, type == RJSON_TRUE ? "true" : "false", s_len);
            found = true;
         }
      }
      else if (type == RJSON_STRING
            && depth == matched + 1
            && rjson_get_context_type(json) == RJSON_OBJECT
            && (rjson_get_context_count(json) & 1))
      {
         size_t _len;
         const char *str = rjson_get_string(json, &_len);
         at_key = _len == key_lens[matched]
               && !memcmp(str, keys[matched], _len);
      }
      else if ((type == RJSON_OBJECT_END || type == RJSON_ARRAY_END)
            && depth < matched + 1)
         matched = depth ? depth - 1 : 0;
   }

   rjson_free(json);
   return found;
}

/* Walks the objects in the top-level array, or in the array at
 * @array_key of the top-level object, skipping nested values. False
 * unless that array is there and complete. */
static bool romm_json_walk_items(const char *data, size_t len,
      const char *array_key, romm_json_member_cb_t member,
      romm_json_item_end_cb_t item_end, void *ctx, bool arrays)
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
      /* A value of an array in the item: @key is the array's. */
      if (     arrays
            && depth == item_depth + 1
            && rjson_get_context_type(json) == RJSON_ARRAY
            && (type == RJSON_STRING || type == RJSON_NUMBER))
      {
         str = rjson_get_string(json, &_len);
         member(ctx, key, type, str, _len);
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

bool romm_json_items(const char *data, size_t len,
      const char *array_key, romm_json_member_cb_t member,
      romm_json_item_end_cb_t item_end, void *ctx)
{
   return romm_json_walk_items(data, len, array_key, member, item_end,
         ctx, false);
}

bool romm_json_items_with_arrays(const char *data, size_t len,
      const char *array_key, romm_json_member_cb_t member,
      romm_json_item_end_cb_t item_end, void *ctx)
{
   return romm_json_walk_items(data, len, array_key, member, item_end,
         ctx, true);
}

bool romm_json_values(const char *data, size_t len,
      const char *array_key, romm_json_value_cb_t value, void *ctx)
{
   bool in_array    = false;
   bool key_matched = false;
   bool complete    = false;
   rjson_t *json;
   enum rjson_type type;

   if (!data || !len || !(json = rjson_open_buffer(data, len)))
      return false;

   while ((type = rjson_next(json)) != RJSON_DONE && type != RJSON_ERROR)
   {
      unsigned depth = rjson_get_context_depth(json);
      const char *str;
      size_t _len;

      if (!in_array)
      {
         if (type == RJSON_ARRAY && depth == 2 && key_matched)
            in_array = true;
         else if (depth == 1)
         {
            key_matched = false;
            if (     type == RJSON_STRING
                  && (rjson_get_context_count(json) & 1))
            {
               str         = rjson_get_string(json, &_len);
               key_matched = string_is_equal(str, array_key);
            }
         }
         continue;
      }

      if (depth < 2)
      {
         complete = true;
         break;
      }
      if (depth > 2)
         continue;
      switch (type)
      {
         case RJSON_STRING:
         case RJSON_NUMBER:
            str = rjson_get_string(json, &_len);
            value(ctx, type, str, _len);
            break;
         case RJSON_TRUE:
         case RJSON_FALSE:
         case RJSON_NULL:
            value(ctx, type, "", 0);
            break;
         default:
            break;
      }
   }

   rjson_free(json);
   return complete;
}

/* Days from 1970-01-01 to @y-@m-@d in the proleptic Gregorian
 * calendar. */
static int64_t romm_json_days(int y, int m, int d)
{
   int era, yoe, doy, doe;

   y  -= m <= 2;
   era = (y >= 0 ? y : y - 399) / 400;
   yoe = y - era * 400;
   doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
   doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
   return (int64_t)era * 146097 + doe - 719468;
}

int64_t romm_json_time(const char *iso)
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
   t = romm_json_days(y, mo, d) * 86400 + h * 3600 + mi * 60 + s;
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
