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

#include "util/copy.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fts.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "util/finally.h"
#include "util/fts.h"
#include "util/logging.h"
#include "util/path.h"
#include "util/string.h"

// WARNING: Everything operates on paths, so it's subject to race conditions
// Directory copy operations will not cross mountpoint boundaries

namespace mb
{
namespace util
{

bool copy_data_fd(int fd_source, int fd_target)
{
    char buf[10240];
    ssize_t nread;

    while ((nread = read(fd_source, buf, sizeof buf)) > 0) {
        char *out_ptr = buf;
        ssize_t nwritten;

        do {
            if ((nwritten = write(fd_target, out_ptr, nread)) < 0) {
                return false;
            }

            nread -= nwritten;
            out_ptr += nwritten;
        } while (nread > 0);
    }

    return nread == 0;
}

static bool copy_data(const char *source, const char *target)
{
    int fd_source = -1;
    int fd_target = -1;

    fd_source = open(source, O_RDONLY);
    if (fd_source < 0) {
        return false;
    }

    auto close_source_fd = finally([&] {
        close(fd_source);
    });

    fd_target = open(target, O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd_target < 0) {
        return false;
    }

    auto close_target_fd = finally([&] {
        close(fd_target);
    });

    if (!copy_data_fd(fd_source, fd_target)) {
        return false;
    }

    return true;
}

bool copy_xattrs(const char *source, const char *target)
{
    ssize_t size;
    std::vector<char> names;
    char *end_names;
    char *name;
    std::vector<char> value;

    // xattr names are in a NULL-separated list
    size = llistxattr(source, nullptr, 0);
    if (size < 0) {
        if (errno == ENOTSUP) {
            LOGV("%s: xattrs not supported on filesystem", source);
            return true;
        } else {
            LOGE("%s: Failed to list xattrs: %s",
                 source, strerror(errno));
            return false;
        }
    }

    names.resize(size + 1);

    size = llistxattr(source, names.data(), size);
    if (size < 0) {
        LOGE("%s: Failed to list xattrs on second try: %s",
             source, strerror(errno));
        return false;
    } else {
        names[size] = '\0';
        end_names = names.data() + size;
    }

    for (name = names.data(); name != end_names; name = strchr(name, '\0') + 1) {
        if (!*name) {
            continue;
        }

        size = lgetxattr(source, name, nullptr, 0);
        if (size < 0) {
            LOGW("%s: Failed to get attribute '%s': %s",
                 source, name, strerror(errno));
            continue;
        }

        value.resize(size);

        size = lgetxattr(source, name, value.data(), size);
        if (size < 0) {
            LOGW("%s: Failed to get attribute '%s' on second try: %s",
                 source, name, strerror(errno));
            continue;
        }

        if (lsetxattr(target, name, value.data(), size, 0) < 0) {
            if (errno == ENOTSUP) {
                LOGV("%s: xattrs not supported on filesystem", target);
                break;
            } else {
                LOGE("%s: Failed to set xattrs: %s",
                     target, strerror(errno));
                return false;
            }
        }
    }

    return true;
}

bool copy_stat(const char *source, const char *target)
{
    struct stat sb;

    if (lstat(source, &sb) < 0) {
        LOGE("%s: Failed to stat: %s", source, strerror(errno));
        return false;
    }

    if (lchown(target, sb.st_uid, sb.st_gid) < 0) {
        LOGE("%s: Failed to chown: %s", target, strerror(errno));
        return false;
    }

    if (!S_ISLNK(sb.st_mode)) {
        if (chmod(target, sb.st_mode & (S_ISUID | S_ISGID | S_ISVTX
                                      | S_IRWXU | S_IRWXG | S_IRWXO)) < 0) {
            LOGE("%s: Failed to chmod: %s", target, strerror(errno));
            return false;
        }
    }

    return true;
}

bool copy_contents(const char *source, const char *target)
{
    int fd_source = -1;
    int fd_target = -1;

    if ((fd_source = open(source, O_RDONLY)) < 0) {
        return false;
    }

    auto close_source_fd = finally([&] {
        close(fd_source);
    });

    if ((fd_target = open(target, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0) {
        return false;
    }

    auto close_target_fd = finally([&] {
        close(fd_target);
    });

    if (!copy_data_fd(fd_source, fd_target)) {
        return false;
    }

    return true;
}

bool copy_file(const char *source, const char *target, int flags)
{
    mode_t old_umask = umask(0);

    auto restore_umask = finally([&] {
        umask(old_umask);
    });

    if (unlink(target) < 0 && errno != ENOENT) {
        LOGE("%s: Failed to remove old file: %s",
             target, strerror(errno));
        return false;
    }

    struct stat sb;
    if (((flags & COPY_FOLLOW_SYMLINKS)
            ? stat : lstat)(source, &sb) < 0) {
        LOGE("%s: Failed to stat: %s",
             source, strerror(errno));
        return false;
    }

    switch (sb.st_mode & S_IFMT) {
    case S_IFBLK:
        if (mknod(target, S_IFBLK | S_IRWXU, sb.st_rdev) < 0) {
            LOGW("%s: Failed to create block device: %s",
                 target, strerror(errno));
            return false;
        }
        break;

    case S_IFCHR:
        if (mknod(target, S_IFCHR | S_IRWXU, sb.st_rdev) < 0) {
            LOGW("%s: Failed to create character device: %s",
                 target, strerror(errno));
            return false;
        }
        break;

    case S_IFIFO:
        if (mkfifo(target, S_IRWXU) < 0) {
            LOGW("%s: Failed to create FIFO pipe: %s",
                 target, strerror(errno));
            return false;
        }
        break;

    case S_IFLNK:
        if (!(flags & COPY_FOLLOW_SYMLINKS)) {
            std::string symlink_path;
            if (!read_link(source, &symlink_path)) {
                LOGW("%s: Failed to read symlink path: %s",
                     source, strerror(errno));
                return false;
            }

            if (symlink(symlink_path.c_str(), target) < 0) {
                LOGW("%s: Failed to create symlink: %s",
                     target, strerror(errno));
                return false;
            }

            break;
        }

        // Treat as file

    case S_IFREG:
        if (!copy_data(source, target)) {
            LOGE("%s: Failed to copy data: %s",
                 target, strerror(errno));
            return false;
        }
        break;

    case S_IFSOCK:
        LOGE("%s: Cannot copy socket", target);
        errno = EINVAL;
        return false;

    case S_IFDIR:
        LOGE("%s: Cannot copy directory", target);
        errno = EINVAL;
        return false;
    }

    if ((flags & COPY_ATTRIBUTES)
            && !copy_stat(source, target)) {
        LOGE("%s: Failed to copy attributes: %s",
             target, strerror(errno));
        return false;
    }
    if ((flags & COPY_XATTRS)
            && !copy_xattrs(source, target)) {
        LOGE("%s: Failed to copy xattrs: %s",
             target, strerror(errno));
        return false;
    }

    return true;
}


class RecursiveCopier : public FTSWrapper {
public:
    RecursiveCopier(std::string path, std::string target, int copyflags)
        : FTSWrapper(path, 0), _copyflags(copyflags), _target(target) {
    }

    virtual bool on_pre_execute() override
    {
        // This is almost *never* useful, so we won't allow it
        if (_copyflags & COPY_FOLLOW_SYMLINKS) {
            _error_msg = "COPY_FOLLOW_SYMLINKS not allowed for recursive copies";
            LOGE("%s", _error_msg.c_str());
            return false;
        }

        // Create the target directory if it doesn't exist
        if (mkdir(_target.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) < 0
                && errno != EEXIST) {
            _error_msg = format("%s: Failed to create directory: %s",
                                _target.c_str(), strerror(errno));
            LOGE("%s", _error_msg.c_str());
            return false;
        }

        // Ensure target is a directory

        if (stat(_target.c_str(), &sb_target) < 0) {
            _error_msg = format("%s: Failed to stat: %s",
                                _target.c_str(), strerror(errno));
            LOGE("%s", _error_msg.c_str());
            return false;
        }

        if (!S_ISDIR(sb_target.st_mode)) {
            _error_msg = format("%s: Target exists but is not a directory",
                                _target.c_str());
            LOGE("%s", _error_msg.c_str());
            return false;
        }

        return true;
    }

    virtual int on_changed_path() override
    {
        // Make sure we aren't copying the target on top of itself
        if (sb_target.st_dev == _curr->fts_statp->st_dev
                && sb_target.st_ino == _curr->fts_statp->st_ino) {
            _error_msg = format("%s: Cannot copy on top of itself",
                                _curr->fts_path);
            LOGE("%s", _error_msg.c_str());
            return Action::FTS_Fail | Action::FTS_Stop;
        }

        // According to fts_read()'s manpage, fts_path includes the path given
        // to fts_open() as a prefix, so 'curr->fts_path + strlen(source)' will
        // give us a relative path we can append to the target.
        _curtgtpath.clear();

        char *relpath = _curr->fts_path + _path.size();

        _curtgtpath += _target;
        if (!(_copyflags & COPY_EXCLUDE_TOP_LEVEL)) {
            if (_curtgtpath.back() != '/') {
                _curtgtpath += "/";
            }
            _curtgtpath += _root->fts_name;
        }
        if (_curtgtpath.back() != '/'
                && *relpath != '/' && *relpath != '\0') {
            _curtgtpath += "/";
        }
        _curtgtpath += relpath;

        return Action::FTS_OK;
    }

    virtual int on_reached_directory_pre() override
    {
        // Skip tree?
        bool skip = false;
        bool success = true;

        struct stat sb;

        // Create target directory if it doesn't exist
        if (mkdir(_curtgtpath.c_str(), S_IRWXU | S_IRWXG | S_IRWXO) < 0
                && errno != EEXIST) {
            _error_msg = format("%s: Failed to create directory: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            success = false;
            skip = true;
        }

        // Ensure target path is a directory
        if (!skip && stat(_curtgtpath.c_str(), &sb) == 0
                && !S_ISDIR(sb.st_mode)) {
            _error_msg = format("%s: Exists but is not a directory",
                                _curtgtpath.c_str());
            LOGW("%s", _error_msg.c_str());
            success = false;
            skip = true;
        }

        // If we're skipping, then we have to set the attributes now, since
        // on_reached_directory_post() won't be called
        if (skip) {
            if (!cp_attrs()) {
                success = false;
            }

            if (!cp_xattrs()) {
                success = false;
            }
        }

        return (skip ? Action::FTS_Skip : 0)
                | (success ? Action::FTS_OK : Action::FTS_Fail);
    }

    virtual int on_reached_directory_post() override
    {
        if (!cp_attrs()) {
            return Action::FTS_Fail;
        }

        if (!cp_xattrs()) {
            return Action::FTS_Fail;
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_file() override
    {
        if (!remove_existing_file()) {
            return Action::FTS_Fail;
        }

        // Copy file contents
        if (!copy_data(_curr->fts_accpath, _curtgtpath.c_str())) {
            _error_msg = format("%s: Failed to copy data: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return Action::FTS_Fail;
        }

        if (!cp_attrs()) {
            return Action::FTS_Fail;
        }

        if (!cp_xattrs()) {
            return Action::FTS_Fail;
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_symlink() override
    {
        if (!remove_existing_file()) {
            return Action::FTS_Fail;
        }

        // Find current symlink target
        std::string symlink_path;
        if (!read_link(_curr->fts_accpath, &symlink_path)) {
            _error_msg = format("%s: Failed to read symlink path: %s",
                                _curr->fts_accpath, strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return Action::FTS_Fail;
        }

        // Create new symlink
        if (symlink(symlink_path.c_str(), _curtgtpath.c_str()) < 0) {
            _error_msg = format("%s: Failed to create symlink: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return Action::FTS_Fail;
        }

        if (!cp_attrs()) {
            return Action::FTS_Fail;
        }

        if (!cp_xattrs()) {
            return Action::FTS_Fail;
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_block_device() override
    {
        if (!remove_existing_file()) {
            return Action::FTS_Fail;
        }

        if (mknod(_curtgtpath.c_str(), S_IFBLK | S_IRWXU,
                _curr->fts_statp->st_rdev) < 0) {
            _error_msg = format("%s: Failed to create block device: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return Action::FTS_Fail;
        }

        if (!cp_attrs()) {
            return Action::FTS_Fail;
        }

        if (!cp_xattrs()) {
            return Action::FTS_Fail;
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_character_device() override
    {
        if (!remove_existing_file()) {
            return Action::FTS_Fail;
        }

        if (mknod(_curtgtpath.c_str(), S_IFCHR | S_IRWXU,
                _curr->fts_statp->st_rdev) < 0) {
            _error_msg = format("%s: Failed to create character device: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return Action::FTS_Fail;
        }

        if (!cp_attrs()) {
            return Action::FTS_Fail;
        }

        if (!cp_xattrs()) {
            return Action::FTS_Fail;
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_fifo() override
    {
        if (!remove_existing_file()) {
            return Action::FTS_Fail;
        }

        if (mkfifo(_curtgtpath.c_str(), S_IRWXU) < 0) {
            _error_msg = format("%s: Failed to create FIFO pipe: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return Action::FTS_Fail;
        }

        if (!cp_attrs()) {
            return Action::FTS_Fail;
        }

        if (!cp_xattrs()) {
            return Action::FTS_Fail;
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_socket() override
    {
        LOGD("%s: Skipping socket", _curr->fts_accpath);
        return Action::FTS_Skip;
    }

private:
    int _copyflags;
    std::string _target;
    struct stat sb_target;
    std::string _curtgtpath;

    bool remove_existing_file()
    {
        // Remove existing file
        if (unlink(_curtgtpath.c_str()) < 0 && errno != ENOENT) {
            _error_msg = format("%s: Failed to remove old path: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return false;
        }
        return true;
    }

    bool cp_attrs()
    {
        if ((_copyflags & COPY_ATTRIBUTES)
                && !copy_stat(_curr->fts_accpath, _curtgtpath.c_str())) {
            _error_msg = format("%s: Failed to copy attributes: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return false;
        }
        return true;
    }

    bool cp_xattrs()
    {
        if ((_copyflags & COPY_XATTRS)
                && !copy_xattrs(_curr->fts_accpath, _curtgtpath.c_str())) {
            _error_msg = format("%s: Failed to copy xattrs: %s",
                                _curtgtpath.c_str(), strerror(errno));
            LOGW("%s", _error_msg.c_str());
            return false;
        }
        return true;
    }
};


// Copy as much as possible
bool copy_dir(const char *source, const char *target, int flags)
{
    mode_t old_umask = umask(0);

    RecursiveCopier copier(source, target, flags);
    bool ret = copier.run();

    umask(old_umask);

    return ret;
}

}
}
