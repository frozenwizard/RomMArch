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
#include <string/stdstring.h>

#include "romm_sidebar.h"

#define ROMM_SIDEBAR_GROUP_ROW ((size_t)-1)

/* A row of the rewritten sidebar. */
typedef struct
{
   /* The list row it was, or ROMM_SIDEBAR_GROUP_ROW. */
   size_t src;
   char key[sizeof(ROMM_SIDEBAR_SYSTEMS) + 64];
   const char *name;
   unsigned depth;
} romm_sidebar_row_t;

/* A system's row, sorted into its maker's group, or a collection's. */
typedef struct
{
   size_t src;
   const char *maker;
   const char *name;
   bool collection;
} romm_sidebar_sys_row_t;

/* Systems without a maker come last. */
static int romm_sidebar_maker_cmp(const char *a, const char *b)
{
   if (!*a || !*b)
      return (!*a) - (!*b);
   return strcasecmp(a, b);
}

/* Collections first, then systems by maker; each by name. */
static int romm_sidebar_sys_row_cmp(const void *a, const void *b)
{
   const romm_sidebar_sys_row_t *x = (const romm_sidebar_sys_row_t*)a;
   const romm_sidebar_sys_row_t *y = (const romm_sidebar_sys_row_t*)b;
   int c = (int)y->collection - (int)x->collection;
   if (!c)
      c = romm_sidebar_maker_cmp(x->maker, y->maker);
   if (!c)
      c = strcasecmp(x->name, y->name);
   if (!c)
      c = (x->src > y->src) - (x->src < y->src);
   return c;
}

static int romm_sidebar_member_cmp(const void *a, const void *b)
{
   const romm_sidebar_system_t *x = *(const romm_sidebar_system_t *const*)a;
   const romm_sidebar_system_t *y = *(const romm_sidebar_system_t *const*)b;
   int c = romm_sidebar_maker_cmp(x->maker, y->maker);
   return c ? c : strcasecmp(x->playlist, y->playlist);
}

static void romm_sidebar_push_group(romm_sidebar_row_t **order,
      const char *key, const char *name, unsigned depth)
{
   romm_sidebar_row_t row;
   row.src   = ROMM_SIDEBAR_GROUP_ROW;
   row.name  = name;
   row.depth = depth;
   strlcpy(row.key, key, sizeof(row.key));
   RBUF_PUSH(*order, row);
}

static void romm_sidebar_push_row(romm_sidebar_row_t **order,
      size_t src, unsigned depth)
{
   romm_sidebar_row_t row;
   row.src    = src;
   row.name   = NULL;
   row.depth  = depth;
   row.key[0] = '\0';
   RBUF_PUSH(*order, row);
}

void romm_sidebar_systems(romm_sidebar_system_t **systems,
      const romm_library_t *lib)
{
   size_t i, j;

   RBUF_CLEAR(*systems);
   for (i = 0; i < RBUF_LEN(lib->platforms); i++)
   {
      const romm_library_platform_t *p = &lib->platforms[i];
      romm_sidebar_system_t sys;
      char *bar;

      if (!*p->playlist)
         continue;

      strlcpy(sys.playlist, p->playlist, sizeof(sys.playlist));
      sys.maker[0]   = '\0';
      sys.collection = false;
      if (*p->platform.family_name)
         strlcpy(sys.maker, p->platform.family_name, sizeof(sys.maker));
      else
      {
         /* "Nintendo" in "Nintendo - Game Boy". */
         const char *sep = strstr(p->platform.libretro_slug, " - ");
         if (sep && (size_t)(sep - p->platform.libretro_slug) < sizeof(sys.maker))
            strlcpy(sys.maker, p->platform.libretro_slug,
                  (size_t)(sep - p->platform.libretro_slug) + 1);
      }
      /* '|' separates the keys of open groups. */
      while ((bar = strchr(sys.maker, '|')))
         *bar = ' ';

      /* Platforms sharing a playlist: the first maker found. */
      for (j = 0; j < RBUF_LEN(*systems); j++)
         if (string_is_equal((*systems)[j].playlist, sys.playlist))
            break;
      if (j < RBUF_LEN(*systems))
      {
         if (!*(*systems)[j].maker)
            strlcpy((*systems)[j].maker, sys.maker,
                  sizeof((*systems)[j].maker));
      }
      else
         RBUF_PUSH(*systems, sys);
   }

   for (i = 0; i < RBUF_LEN(lib->collections); i++)
   {
      romm_sidebar_system_t sys;
      strlcpy(sys.playlist, lib->collections[i].playlist,
            sizeof(sys.playlist));
      sys.maker[0]   = '\0';
      sys.collection = true;
      RBUF_PUSH(*systems, sys);
   }
}

bool romm_sidebar_is_open(const char *open, const char *key)
{
   size_t len = strlen(key);

   while (open && *open)
   {
      const char *end = strchr(open, '|');
      size_t _len     = end ? (size_t)(end - open) : strlen(open);
      if (_len == len && !strncmp(open, key, len))
         return true;
      open = end ? end + 1 : NULL;
   }
   return false;
}

void romm_sidebar_toggle(char *open, size_t len, const char *key)
{
   char *out;
   const char *s = open;
   size_t _len   = 0;

   if (!*key || !(out = (char*)malloc(len)))
      return;
   *out = '\0';

   /* Copy every key but @key. */
   while (*s)
   {
      const char *end = strchr(s, '|');
      size_t n        = end ? (size_t)(end - s) : strlen(s);
      if (     n
            && !(n == strlen(key) && !strncmp(s, key, n))
            && _len + (_len ? 1 : 0) + n < len)
      {
         if (_len)
            out[_len++] = '|';
         memcpy(out + _len, s, n);
         _len     += n;
         out[_len] = '\0';
      }
      s = end ? end + 1 : s + n;
   }

   if (!romm_sidebar_is_open(open, key)
         && _len + (_len ? 1 : 0) + strlen(key) < len)
   {
      if (_len)
         out[_len++] = '|';
      strlcpy(out + _len, key, len - _len);
   }

   strlcpy(open, out, len);
   free(out);
}

/* Moves @list's rows into @order's, drops those it leaves out and
 * inserts the group rows. Rows move whole, so what the menu attached
 * to them stays theirs. */
static void romm_sidebar_apply(file_list_t *list,
      const romm_sidebar_row_t *order, unsigned group_type)
{
   size_t i, kept;
   size_t n               = list->size;
   size_t j               = 0;
   struct item_file *tmp  = (struct item_file*)malloc(
         (n ? n : 1) * sizeof(*tmp));
   bool *used             = (bool*)calloc(n ? n : 1, sizeof(*used));

   if (!tmp || !used)
   {
      free(tmp);
      free(used);
      return;
   }

   for (i = 0; i < RBUF_LEN(order); i++)
   {
      if (order[i].src == ROMM_SIDEBAR_GROUP_ROW)
         continue;
      tmp[j++]             = list->list[order[i].src];
      used[order[i].src]   = true;
   }
   kept = j;
   for (i = 0; i < n; i++)
      if (!used[i])
         tmp[j++] = list->list[i];
   memcpy(list->list, tmp, n * sizeof(*tmp));
   free(tmp);
   free(used);

   while (list->size > kept)
      file_list_pop(list, NULL);

   for (i = 0; i < RBUF_LEN(order); i++)
   {
      if (order[i].src != ROMM_SIDEBAR_GROUP_ROW)
         list->list[i].entry_idx = order[i].depth;
      else if (file_list_insert(list, NULL, order[i].key, group_type, 0,
            order[i].depth, i))
         file_list_set_alt_at_offset(list, i, order[i].name);
      else
         break;
   }
}

void romm_sidebar_group(file_list_t *list,
      const romm_sidebar_system_t *systems, const char *open,
      const romm_sidebar_names_t *names)
{
   size_t i, j;
   size_t first_system;
   romm_sidebar_sys_row_t *sys_rows = NULL;
   romm_sidebar_row_t *order        = NULL;

   for (i = 0; i < list->size; i++)
   {
      const char *path = list->list[i].path;
      if (!path || !*path)
         continue;
      for (j = 0; j < RBUF_LEN(systems); j++)
      {
         if (string_is_equal(systems[j].playlist, path))
         {
            romm_sidebar_sys_row_t s;
            s.src        = i;
            s.maker      = systems[j].maker;
            s.name       = list->list[i].alt ? list->list[i].alt : path;
            s.collection = systems[j].collection;
            RBUF_PUSH(sys_rows, s);
            break;
         }
      }
   }

   /* Nothing of RomM's to group. */
   if (!RBUF_LEN(sys_rows))
      return;

   qsort(sys_rows, RBUF_LEN(sys_rows), sizeof(*sys_rows),
         romm_sidebar_sys_row_cmp);

   /* Collections sort first. */
   for (first_system = 0; first_system < RBUF_LEN(sys_rows)
         && sys_rows[first_system].collection; first_system++) { }

   if (first_system)
   {
      romm_sidebar_push_group(&order, ROMM_SIDEBAR_COLLECTIONS,
            names->collections, 0);
      if (romm_sidebar_is_open(open, ROMM_SIDEBAR_COLLECTIONS))
         for (i = 0; i < first_system; i++)
            romm_sidebar_push_row(&order, sys_rows[i].src, 1);
   }

   if (first_system < RBUF_LEN(sys_rows))
   {
      romm_sidebar_push_group(&order, ROMM_SIDEBAR_SYSTEMS,
            names->systems, 0);
      if (romm_sidebar_is_open(open, ROMM_SIDEBAR_SYSTEMS))
      {
         for (i = first_system; i < RBUF_LEN(sys_rows); i = j)
         {
            char key[sizeof(ROMM_SIDEBAR_SYSTEMS) + 64];
            const char *maker = sys_rows[i].maker;
            bool maker_open;

            snprintf(key, sizeof(key), "%s/%s", ROMM_SIDEBAR_SYSTEMS, maker);
            romm_sidebar_push_group(&order, key,
                  *maker ? maker : names->other, 1);
            maker_open = romm_sidebar_is_open(open, key);

            for (j = i; j < RBUF_LEN(sys_rows)
                  && !romm_sidebar_maker_cmp(sys_rows[j].maker, maker); j++)
               if (maker_open)
                  romm_sidebar_push_row(&order, sys_rows[j].src, 2);
         }
      }
   }

   /* Playlists that aren't RomM's follow, as they were. */
   for (i = 0; i < list->size; i++)
   {
      for (j = 0; j < RBUF_LEN(sys_rows); j++)
         if (sys_rows[j].src == i)
            break;
      if (j == RBUF_LEN(sys_rows))
         romm_sidebar_push_row(&order, i, 0);
   }

   romm_sidebar_apply(list, order, names->group_type);
   RBUF_FREE(order);
   RBUF_FREE(sys_rows);
}

void romm_sidebar_members(const char ***playlists,
      const romm_sidebar_system_t *systems, const char *key)
{
   size_t i;
   const romm_sidebar_system_t **members = NULL;
   size_t prefix_len                     = strlen(ROMM_SIDEBAR_SYSTEMS);
   const char *maker                     = NULL;

   bool collections                      = string_is_equal(key,
         ROMM_SIDEBAR_COLLECTIONS);

   RBUF_CLEAR(*playlists);
   if (!collections)
   {
      if (strncmp(key, ROMM_SIDEBAR_SYSTEMS, prefix_len))
         return;
      if (key[prefix_len] == '/')
         maker = key + prefix_len + 1;
      else if (key[prefix_len])
         return;
   }

   for (i = 0; i < RBUF_LEN(systems); i++)
      if (     systems[i].collection == collections
            && (!maker || !romm_sidebar_maker_cmp(systems[i].maker, maker)))
         RBUF_PUSH(members, &systems[i]);

   if (RBUF_LEN(members))
      qsort(members, RBUF_LEN(members), sizeof(*members),
            romm_sidebar_member_cmp);
   for (i = 0; i < RBUF_LEN(members); i++)
      RBUF_PUSH(*playlists, members[i]->playlist);
   RBUF_FREE(members);
}
