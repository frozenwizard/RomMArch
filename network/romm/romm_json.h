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

#ifndef __ROMM_JSON_H
#define __ROMM_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>
#include <formats/rjson.h>

RETRO_BEGIN_DECLS

/* Copies the string, number or boolean ("true" or "false") at @path,
 * object keys joined by '.' such as "SYSTEM.VERSION". */
bool romm_json_get(const char *data, size_t len, const char *path,
      char *s, size_t s_len);

/* As romm_json_get(), or for an array at @path its strings and numbers
 * joined with @sep, such as "Action, Platform". */
bool romm_json_get_list(const char *data, size_t len, const char *path,
      char *s, size_t s_len, const char *sep);

/* A scalar member of an object in the array being walked; @value is ""
 * for true, false and null. */
typedef void (*romm_json_member_cb_t)(void *ctx, const char *key,
      enum rjson_type type, const char *value, size_t len);
/* Called after the last member of each object. */
typedef void (*romm_json_item_end_cb_t)(void *ctx);

/* Walks the objects in the top-level array, or in the array at
 * @array_key of the top-level object, skipping nested values. False
 * unless that array is there and complete. */
bool romm_json_items(const char *data, size_t len,
      const char *array_key, romm_json_member_cb_t member,
      romm_json_item_end_cb_t item_end, void *ctx);

/* As romm_json_items(), also passing @member the strings and numbers
 * of an array directly in an object, each under that array's key. */
bool romm_json_items_with_arrays(const char *data, size_t len,
      const char *array_key, romm_json_member_cb_t member,
      romm_json_item_end_cb_t item_end, void *ctx);

/* A scalar item in the array being walked. */
typedef void (*romm_json_value_cb_t)(void *ctx, enum rjson_type type,
      const char *value, size_t len);

/* Walks the strings, numbers, true, false and null directly in the
 * array at @array_key of the top-level object. False unless that
 * array is there and complete. */
bool romm_json_values(const char *data, size_t len,
      const char *array_key, romm_json_value_cb_t value, void *ctx);

/* Seconds since 1970 UTC of an ISO 8601 time such as RomM's
 * "2026-10-13T02:18:25.590502+00:00", fractions dropped; -1 if @iso
 * isn't one. */
int64_t romm_json_time(const char *iso);

RETRO_END_DECLS

#endif
