/*
 * Copyright (C) 2014  Andrew Gunnerson <andrewgunnerson@gmail.com>
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

#include <string>
#include <vector>

#define MF_WAIT             0x1
#define MF_CHECK            0x2
#define MF_CRYPT            0x4
#define MF_NONREMOVABLE     0x8
#define MF_VOLDMANAGED      0x10
#define MF_LENGTH           0x20
#define MF_RECOVERYONLY     0x40
#define MF_SWAPPRIO         0x80
#define MF_ZRAMSIZE         0x100
#define MF_VERIFY           0x200
#define MF_FORCECRYPT       0x400
#define MF_NOEMULATEDSD     0x800
#define MF_NOTRIM           0x1000
#define MF_FILEENCRYPTION   0x2000
#define MF_FORMATTABLE      0x4000
#define MF_SLOTSELECT       0x8000

namespace mb
{
namespace util
{

struct fstab_rec
{
    std::string blk_device;
    std::string mount_point;
    std::string fs_type;
    unsigned long flags;
    std::string fs_options;
    unsigned long fs_mgr_flags;
    std::string vold_args;
    std::string orig_line;
};

std::vector<fstab_rec> read_fstab(const std::string &path);

}
}