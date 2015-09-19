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

#include "util/directory.h"

#include <vector>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include "util/path.h"

namespace mb
{
namespace util
{

bool mkdir_recursive(const char *dir, mode_t mode)
{
    char *p;
    char *save_ptr;
    std::vector<char> temp;
    std::vector<char> copy;

    if (!*dir) {
        errno = EINVAL;
        return false;
    }

    copy.assign(dir, dir + strlen(dir));
    copy.push_back('\0');
    temp.resize(strlen(dir) + 2);
    temp[0] = '\0';

    if (dir[0] == '/') {
        strcat(temp.data(), "/");
    }

    p = strtok_r(copy.data(), "/", &save_ptr);
    while (p != nullptr) {
        strcat(temp.data(), p);
        strcat(temp.data(), "/");

        if (mkdir(temp.data(), mode) < 0 && errno != EEXIST) {
            return false;
        }

        p = strtok_r(nullptr, "/", &save_ptr);
    }

    return true;
}

bool mkdir_parent(const char *path, mode_t perms)
{
    if (!*path) {
        errno = EINVAL;
        return false;
    }

    struct stat sb;
    std::string dir = dir_name(path);

    if (!mkdir_recursive(dir.c_str(), perms) && errno != EEXIST) {
        return false;
    }

    if (stat(dir.c_str(), &sb) < 0) {
        return false;
    }

    if (!S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return false;
    }

    return true;
}

}
}
