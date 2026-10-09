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
#include <boolean.h>
#include <retro_common_api.h>
#include <formats/rjson.h>

RETRO_BEGIN_DECLS

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

RETRO_END_DECLS

#endif
