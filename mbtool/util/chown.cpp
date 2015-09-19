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

#include "util/chown.h"

#include <cerrno>
#include <grp.h>
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/fts.h"
#include "util/logging.h"
#include "util/string.h"

namespace mb
{
namespace util
{

static bool chown_internal(const char *path,
                           uid_t uid,
                           gid_t gid,
                           bool follow_symlinks)
{
    if (follow_symlinks) {
        return ::chown(path, uid, gid) == 0;
    } else {
        return ::lchown(path, uid, gid) == 0;
    }
}

class RecursiveChown : public FTSWrapper {
public:
    RecursiveChown(std::string path, uid_t uid, gid_t gid,
                   bool follow_symlinks)
        : FTSWrapper(path, FTS_GroupSpecialFiles),
        _uid(uid),
        _gid(gid),
        _follow_symlinks(follow_symlinks)
    {
    }

    virtual int on_reached_directory_post() override
    {
        return chown_path() ? Action::FTS_OK : Action::FTS_Fail;
    }

    virtual int on_reached_file() override
    {
        return chown_path() ? Action::FTS_OK : Action::FTS_Fail;
    }

    virtual int on_reached_symlink() override
    {
        return chown_path() ? Action::FTS_OK : Action::FTS_Fail;
    }

    virtual int on_reached_special_file() override
    {
        return chown_path() ? Action::FTS_OK : Action::FTS_Fail;
    }

private:
    uid_t _uid;
    gid_t _gid;
    bool _follow_symlinks;

    bool chown_path()
    {
        if (!chown_internal(_curr->fts_accpath, _uid, _gid, _follow_symlinks)) {
            _error_msg = format("%s: Failed to chown: %s",
                                _curr->fts_path, strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return false;
        }
        return true;
    }
};

// WARNING: Not thread safe! Android doesn't have getpwnam_r() or getgrnam_r()
bool chown(const char *path,
           const char *user,
           const char *group,
           int flags)
{
    uid_t uid;
    gid_t gid;

    errno = 0;
    struct passwd *pw = getpwnam(user);
    if (!pw) {
        if (!errno) {
            errno = EINVAL; // User does not exist
        }
        return false;
    } else {
        uid = pw->pw_uid;
    }

    errno = 0;
    struct group *gr = getgrnam(group);
    if (!gr) {
        if (!errno) {
            errno = EINVAL; // Group does not exist
        }
        return false;
    } else {
        gid = gr->gr_gid;
    }

    return chown(path, uid, gid, flags);
}

bool chown(const char *path,
           uid_t uid,
           gid_t gid,
           int flags)
{
    if (flags & CHOWN_RECURSIVE) {
        RecursiveChown fts(path, uid, gid, flags & CHOWN_FOLLOW_SYMLINKS);
        return fts.run();
    } else {
        return chown_internal(path, uid, gid, flags & CHOWN_FOLLOW_SYMLINKS);
    }
}

}
}
