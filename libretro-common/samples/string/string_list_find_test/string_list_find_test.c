/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (string_list_find_test.c).
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
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Regression test for string_list_find_elem in
 * libretro-common/lists/string_list.c.
 *
 * It matches whole elements, ignoring the case of letters. Folding
 * case with "| 32" also made '\0' equal ' ', so an element matched
 * any longer string going on with a space: a core info file's
 * "Nintendo - Game Boy" matched "Nintendo - Game Boy Advance". The
 * same fold made '@' equal '`' and '[' equal '{'. */

#include <stdio.h>

#include <lists/string_list.h>

static int failures = 0;

#define EXPECT_EQ_INT(got, want, msg) do { \
   long _g = (long)(got), _w = (long)(want); \
   if (_g != _w) { \
      printf("[ERROR] %s:%d  %s  got=%ld want=%ld\n", \
            __func__, __LINE__, (msg), _g, _w); \
      failures++; \
   } \
} while (0)

int main(void)
{
   struct string_list *list = string_split(
         "Nintendo - Game Boy|Nintendo - Game Boy Color|a@b", "|");

   if (!list)
   {
      printf("[ERROR] string_split failed\n");
      return 1;
   }

   EXPECT_EQ_INT(string_list_find_elem(list, "Nintendo - Game Boy"), 1,
         "exact element");
   EXPECT_EQ_INT(string_list_find_elem(list, "nintendo - game boy color"), 2,
         "letters in another case");
   EXPECT_EQ_INT(string_list_find_elem(list, "Nintendo - Game Boy Advance"), 0,
         "element followed by a space and more");
   EXPECT_EQ_INT(string_list_find_elem(list, "Nintendo - Game Boy "), 0,
         "element followed by a space");
   EXPECT_EQ_INT(string_list_find_elem(list, "Nintendo - Game"), 0,
         "part of an element");
   EXPECT_EQ_INT(string_list_find_elem(list, "a`b"), 0,
         "punctuation one bit apart");
   EXPECT_EQ_INT(string_list_find_elem(list, "A@B"), 3,
         "punctuation, letters in another case");
   EXPECT_EQ_INT(string_list_find_elem(list, NULL), 0, "no element");
   EXPECT_EQ_INT(string_list_find_elem(NULL, "a@b"), 0, "no list");

   string_list_free(list);

   if (failures)
   {
      printf("\n%d check(s) failed\n", failures);
      return 1;
   }
   printf("all string_list_find_elem checks passed\n");
   return 0;
}
