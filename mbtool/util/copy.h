/*
 * Copyright (C) 2014-2015  Andrew Gunnerson <andrewgunnerson@gmail.com>
 *
 * This file is part of MultiBootPatcher
 *
 * MultiBootPatcher is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiBootPatcher is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiBootPatcher.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

namespace mb
{
namespace util
{

enum CopyFlags : int
{
    COPY_ATTRIBUTES          = 0x1,
    COPY_XATTRS              = 0x2,
    COPY_EXCLUDE_TOP_LEVEL   = 0x4,
    COPY_FOLLOW_SYMLINKS     = 0x8
};

bool copy_data_fd(int fd_source, int fd_target);
bool copy_xattrs(const char *source, const char *target);
bool copy_stat(const char *source, const char *target);
bool copy_contents(const char *source, const char *target);
bool copy_file(const char *source, const char *target, int flags);
bool copy_dir(const char *source, const char *target, int flags);

}
}
