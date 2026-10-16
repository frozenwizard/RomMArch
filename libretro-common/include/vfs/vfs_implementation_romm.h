/* Copyright  (C) 2010-2026 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (vfs_implementation_romm.h).
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
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef __LIBRETRO_SDK_VFS_IMPLEMENTATION_ROMM_H
#define __LIBRETRO_SDK_VFS_IMPLEMENTATION_ROMM_H

#include <stdint.h>
#include <boolean.h>
#include <retro_common_api.h>
#include <vfs/vfs.h>

RETRO_BEGIN_DECLS

/* Read-only files held in memory under romm:// paths. The frontend
 * adds a game's files after downloading them; anything that opens
 * those paths through the VFS, cores included, reads them from RAM.
 * Paths match without regard to case or to which slash separates their
 * parts, like the file systems the cue sheets and disc lists inside
 * them were written against. */

/* Adds @data (@size bytes, from malloc) as @path, taking ownership.
 * Replaces a file already at @path. */
bool retro_vfs_romm_add(const char *path, void *data, uint64_t size);

/* Removes @path. Handles still open keep reading it; its memory goes
 * when the last one closes. */
bool retro_vfs_romm_remove(const char *path);

/* Removes every file whose path starts with @prefix. */
void retro_vfs_romm_remove_prefix(const char *prefix);

/* Removes every file whose path doesn't start with @prefix. */
void retro_vfs_romm_keep_prefix(const char *prefix);

bool    retro_vfs_file_open_romm(libretro_vfs_implementation_file *stream,
        const char *path, unsigned mode);
int64_t retro_vfs_file_read_romm(libretro_vfs_implementation_file *stream,
        void *s, uint64_t len);
int64_t retro_vfs_file_seek_romm(libretro_vfs_implementation_file *stream,
        int64_t offset, int whence);
int64_t retro_vfs_file_tell_romm(libretro_vfs_implementation_file *stream);
void    retro_vfs_file_close_romm(libretro_vfs_implementation_file *stream);

/* RETRO_VFS_STAT_* flags for @path: a file, or a directory that holds
 * one; 0 when neither. */
int     retro_vfs_stat_romm(const char *path, int64_t *size);

RETRO_END_DECLS

#endif
