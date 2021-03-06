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

#include "daemon_v3.h"

#include <unordered_map>
#include <unordered_set>

#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "packages.h"
#include "reboot.h"
#include "roms.h"
#include "switcher.h"
#include "util/copy.h"
#include "util/finally.h"
#include "util/fts.h"
#include "util/logging.h"
#include "util/properties.h"
#include "util/selinux.h"
#include "util/socket.h"
#include "version.h"
#include "wipe.h"

// flatbuffers
#include "protocol/file_chmod_generated.h"
#include "protocol/file_close_generated.h"
#include "protocol/file_open_generated.h"
#include "protocol/file_read_generated.h"
#include "protocol/file_seek_generated.h"
#include "protocol/file_selinux_get_label_generated.h"
#include "protocol/file_selinux_set_label_generated.h"
#include "protocol/file_stat_generated.h"
#include "protocol/file_write_generated.h"
#include "protocol/path_chmod_generated.h"
#include "protocol/path_copy_generated.h"
#include "protocol/path_selinux_get_label_generated.h"
#include "protocol/path_selinux_set_label_generated.h"
#include "protocol/path_get_directory_size_generated.h"
#include "protocol/mb_get_booted_rom_id_generated.h"
#include "protocol/mb_get_installed_roms_generated.h"
#include "protocol/mb_get_version_generated.h"
#include "protocol/mb_set_kernel_generated.h"
#include "protocol/mb_switch_rom_generated.h"
#include "protocol/mb_wipe_rom_generated.h"
#include "protocol/mb_get_packages_count_generated.h"
#include "protocol/reboot_generated.h"
#include "protocol/request_generated.h"
#include "protocol/response_generated.h"

namespace mb
{

namespace v3 = mbtool::daemon::v3;
namespace fb = flatbuffers;

static std::unordered_map<int, int> fd_map;
static int fd_count = 0;

static bool v3_send_response(int fd, const fb::FlatBufferBuilder &builder)
{
    return util::socket_write_bytes(
            fd, builder.GetBufferPointer(), builder.GetSize());
}

static bool v3_send_response_invalid(int fd)
{
    fb::FlatBufferBuilder builder;
    auto response = v3::CreateResponse(builder, v3::ResponseType_Invalid,
                                       v3::CreateInvalid(builder).Union());
    builder.Finish(response);
    return v3_send_response(fd, builder);
}

static bool v3_send_response_unsupported(int fd)
{
    fb::FlatBufferBuilder builder;
    auto response = v3::CreateResponse(builder, v3::ResponseType_Unsupported,
                                       v3::CreateUnsupported(builder).Union());
    builder.Finish(response);
    return v3_send_response(fd, builder);
}

static bool v3_file_chmod(int fd, const v3::Request *msg)
{
    auto request = (v3::FileChmodRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];

    // Don't allow setting setuid or setgid permissions
    uint32_t mode = request->mode();
    uint32_t masked = mode & (S_IRWXU | S_IRWXG | S_IRWXO);
    if (masked != mode) {
        return v3_send_response_invalid(fd);
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileChmodResponse> response;

    if (fchmod(ffd, mode) < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileChmodResponse(builder, false, error);
    } else {
        response = v3::CreateFileChmodResponse(builder, true);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileChmodResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_close(int fd, const v3::Request *msg)
{
    auto request = (v3::FileCloseRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end()) {
        return v3_send_response_invalid(fd);
    }

    // Remove ID from map
    int ffd = fd_map[request->id()];
    fd_map.erase(request->id());

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileCloseResponse> response;

    if (close(ffd) < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileCloseResponse(builder, false, error);
    } else {
        response = v3::CreateFileCloseResponse(builder, true);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileCloseResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_open(int fd, const v3::Request *msg)
{
    auto request = (v3::FileOpenRequest *) msg->request();
    if (!request->path()) {
        return v3_send_response_invalid(fd);
    }

    int flags = O_CLOEXEC;

    if (request->flags()) {
        for (short openflag : *request->flags()) {
            if (openflag == v3::FileOpenFlag_APPEND) {
                flags |= O_APPEND;
            } else if (openflag == v3::FileOpenFlag_CREAT) {
                flags |= O_CREAT;
            } else if (openflag == v3::FileOpenFlag_EXCL) {
                flags |= O_EXCL;
            } else if (openflag == v3::FileOpenFlag_RDONLY) {
                flags |= O_RDONLY;
            } else if (openflag == v3::FileOpenFlag_RDWR) {
                flags |= O_RDWR;
            } else if (openflag == v3::FileOpenFlag_TRUNC) {
                flags |= O_TRUNC;
            } else if (openflag == v3::FileOpenFlag_WRONLY) {
                flags |= O_WRONLY;
            }
        }
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileOpenResponse> response;

    int ffd = open(request->path()->c_str(), flags, request->perms());
    if (ffd < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileOpenResponse(builder, false, error);
    } else {
        // Assign a new ID
        int id = fd_count++;
        response = v3::CreateFileOpenResponse(builder, true, 0, id);
        fd_map[id] = ffd;
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileOpenResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_read(int fd, const v3::Request *msg)
{
    auto request = (v3::FileReadRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];

    std::vector<unsigned char> buf(request->count());

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileReadResponse> response;

    auto ret = read(ffd, buf.data(), buf.size());
    if (ret < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileReadResponse(builder, false, error);
    } else {
        auto data = builder.CreateVector(buf.data(), ret);
        response = v3::CreateFileReadResponse(builder, true, 0, ret, data);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileReadResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_seek(int fd, const v3::Request *msg)
{
    auto request = (v3::FileSeekRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];
    int64_t offset = request->offset();
    int whence;

    if (request->whence() == v3::FileSeekWhence_SEEK_SET) {
        whence = SEEK_SET;
    } else if (request->whence() == v3::FileSeekWhence_SEEK_CUR) {
        whence = SEEK_CUR;
    } else if (request->whence() == v3::FileSeekWhence_SEEK_END) {
        whence = SEEK_END;
    } else {
        return v3_send_response_invalid(fd);
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileSeekResponse> response;

    // Ahh, posix...
    errno = 0;
    off_t ret = lseek(ffd, offset, whence);
    if (ret < 0 || errno) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileSeekResponse(builder, false, error);
    } else {
        response = v3::CreateFileSeekResponse(builder, true, 0, ret);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileSeekResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_selinux_get_label(int fd, const v3::Request *msg)
{
    auto request = (v3::FileSELinuxGetLabelRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileSELinuxGetLabelResponse> response;

    std::string label;

    if (!util::selinux_fget_context(ffd, &label)) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileSELinuxGetLabelResponse(
                builder, false, error, 0);
    } else {
        auto fb_label = builder.CreateString(label);
        response = v3::CreateFileSELinuxGetLabelResponse(
                builder, true, 0, fb_label);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_PathSELinuxGetLabelResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_selinux_set_label(int fd, const v3::Request *msg)
{
    auto request = (v3::FileSELinuxSetLabelRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end() || !request->label()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileSELinuxSetLabelResponse> response;

    if (!util::selinux_fset_context(ffd, request->label()->c_str())) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileSELinuxSetLabelResponse(builder, false, error);
    } else {
        response = v3::CreateFileSELinuxSetLabelResponse(builder, true);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileSELinuxSetLabelResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_stat(int fd, const v3::Request *msg)
{
    auto request = (v3::FileStatRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileStatResponse> response;

    struct stat sb;

    if (fstat(ffd, &sb) < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileStatResponse(builder, false, error);
    } else {
        v3::StructStatBuilder ssb(builder);
        ssb.add_st_dev(sb.st_dev);
        ssb.add_st_ino(sb.st_ino);
        ssb.add_st_mode(sb.st_mode);
        ssb.add_st_nlink(sb.st_nlink);
        ssb.add_st_uid(sb.st_uid);
        ssb.add_st_gid(sb.st_gid);
        ssb.add_st_rdev(sb.st_rdev);
        ssb.add_st_size(sb.st_size);
        ssb.add_st_blksize(sb.st_blksize);
        ssb.add_st_blocks(sb.st_blocks);
        ssb.add_st_atime(sb.st_atime);
        ssb.add_st_mtime(sb.st_mtime);
        ssb.add_st_ctime(sb.st_ctime);
        auto fb_ssb = ssb.Finish();

        response = v3::CreateFileStatResponse(builder, true, 0, fb_ssb);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileStatResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_file_write(int fd, const v3::Request *msg)
{
    auto request = (v3::FileWriteRequest *) msg->request();
    if (fd_map.find(request->id()) == fd_map.end() || !request->data()) {
        return v3_send_response_invalid(fd);
    }

    int ffd = fd_map[request->id()];

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::FileWriteResponse> response;

    auto ret = write(ffd, request->data()->Data(), request->data()->size());
    if (ret < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreateFileWriteResponse(builder, false, error);
    } else {
        response = v3::CreateFileWriteResponse(builder, true, 0, ret);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_FileWriteResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_path_chmod(int fd, const v3::Request *msg)
{
    auto request = (v3::PathChmodRequest *) msg->request();
    if (!request->path()) {
        return v3_send_response_invalid(fd);
    }

    // Don't allow setting setuid or setgid permissions
    uint32_t mode = request->mode();
    uint32_t masked = mode & (S_IRWXU | S_IRWXG | S_IRWXO);
    if (masked != mode) {
        return v3_send_response_invalid(fd);
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::PathChmodResponse> response;

    if (chmod(request->path()->c_str(), mode) < 0) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreatePathChmodResponse(builder, false, error);
    } else {
        response = v3::CreatePathChmodResponse(builder, true);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_PathChmodResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_path_copy(int fd, const v3::Request *msg)
{
    auto request = (v3::PathCopyRequest *) msg->request();
    if (!request->source() || !request->target()) {
        return v3_send_response_invalid(fd);
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::PathCopyResponse> response;

    if (util::copy_contents(request->source()->c_str(),
                            request->target()->c_str())) {
        response = v3::CreatePathCopyResponse(builder, true);
    } else {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreatePathCopyResponse(builder, false, error);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_PathCopyResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_path_selinux_get_label(int fd, const v3::Request *msg)
{
    auto request = (v3::PathSELinuxGetLabelRequest *) msg->request();
    if (!request->path()) {
        return v3_send_response_invalid(fd);
    }

    std::string label;
    bool ret;
    if (request->follow_symlinks()) {
        ret = util::selinux_get_context(request->path()->c_str(), &label);
    } else {
        ret = util::selinux_lget_context(request->path()->c_str(), &label);
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::PathSELinuxGetLabelResponse> response;

    if (!ret) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreatePathSELinuxGetLabelResponse(
                builder, false, error, 0);
    } else {
        auto fb_label = builder.CreateString(label);
        response = v3::CreatePathSELinuxGetLabelResponse(
                builder, true, 0, fb_label);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_PathSELinuxGetLabelResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_path_selinux_set_label(int fd, const v3::Request *msg)
{
    auto request = (v3::PathSELinuxSetLabelRequest *) msg->request();
    if (!request->path()) {
        return v3_send_response_invalid(fd);
    }

    bool ret;
    if (request->follow_symlinks()) {
        ret = util::selinux_set_context(request->path()->c_str(),
                                        request->label()->c_str());
    } else {
        ret = util::selinux_lset_context(request->path()->c_str(),
                                         request->label()->c_str());
    }

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::PathSELinuxSetLabelResponse> response;

    if (!ret) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreatePathSELinuxSetLabelResponse(builder, false, error);
    } else {
        response = v3::CreatePathSELinuxSetLabelResponse(builder, true);
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_PathSELinuxSetLabelResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

class DirectorySizeGetter : public util::FTSWrapper {
public:
    DirectorySizeGetter(std::string path, std::vector<std::string> exclusions)
        : FTSWrapper(path, FTS_GroupSpecialFiles),
        _exclusions(std::move(exclusions)),
        _total(0)
    {
    }

    virtual int on_changed_path() override
    {
        // Exclude first-level directories
        if (_curr->fts_level == 1) {
            if (std::find(_exclusions.begin(), _exclusions.end(), _curr->fts_name)
                    != _exclusions.end()) {
                return Action::FTS_Skip;
            }
        }

        return Action::FTS_OK;
    }

    virtual int on_reached_file() override
    {
        dev_t dev = _curr->fts_statp->st_dev;
        ino_t ino = _curr->fts_statp->st_ino;

        // If this file has been visited before (hard link), then skip it
        if (_links.find(dev) != _links.end()
                && _links[dev].find(ino) != _links[dev].end()) {
            return Action::FTS_OK;
        }

        _total += _curr->fts_statp->st_size;
        _links[dev].emplace(dev);

        return Action::FTS_OK;
    }

    uint64_t total() const {
        return _total;
    }

private:
    std::vector<std::string> _exclusions;
    std::unordered_map<dev_t, std::unordered_set<ino_t>> _links;
    uint64_t _total;
};

static bool v3_path_get_directory_size(int fd, const v3::Request *msg)
{
    auto request = (v3::PathGetDirectorySizeRequest *) msg->request();
    if (!request->path()) {
        return v3_send_response_invalid(fd);
    }

    std::vector<std::string> exclusions;
    if (request->exclusions()) {
        for (auto const &exclusion : *request->exclusions()) {
            exclusions.push_back(exclusion->c_str());
        }
    }

    DirectorySizeGetter dsg(request->path()->c_str(), std::move(exclusions));
    bool ret = dsg.run();

    fb::FlatBufferBuilder builder;
    fb::Offset<v3::PathGetDirectorySizeResponse> response;

    if (!ret) {
        auto error = builder.CreateString(strerror(errno));
        response = v3::CreatePathGetDirectorySizeResponse(
                builder, false, error);
    } else {
        response = v3::CreatePathGetDirectorySizeResponse(
                builder, true, 0, dsg.total());
    }

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_PathGetDirectorySizeResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_get_booted_rom_id(int fd, const v3::Request *msg)
{
    (void) msg;

    fb::FlatBufferBuilder builder;
    fb::Offset<fb::String> id;
    auto rom = Roms::get_current_rom();
    if (rom) {
        id = builder.CreateString(rom->id);
    }

    // Create response
    auto response = v3::CreateMbGetBootedRomIdResponse(builder, id);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbGetBootedRomIdResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_get_installed_roms(int fd, const v3::Request *msg)
{
    (void) msg;

    fb::FlatBufferBuilder builder;

    Roms roms;
    roms.add_installed();

    std::vector<fb::Offset<v3::MbRom>> fb_roms;

    for (auto r : roms.roms) {
        std::string system_path = r->full_system_path();
        std::string cache_path = r->full_cache_path();
        std::string data_path = r->full_data_path();

        auto fb_id = builder.CreateString(r->id);
        auto fb_system_path = builder.CreateString(system_path);
        auto fb_cache_path = builder.CreateString(cache_path);
        auto fb_data_path = builder.CreateString(data_path);
        fb::Offset<fb::String> fb_version;
        fb::Offset<fb::String> fb_build;

        std::string build_prop;
        if (r->system_is_image) {
            build_prop += "/raw/images/";
            build_prop += r->id;
        } else {
            build_prop += system_path;
        }
        build_prop += "/build.prop";

        std::unordered_map<std::string, std::string> properties;
        util::file_get_all_properties(build_prop, &properties);

        if (properties.find("ro.build.version.release") != properties.end()) {
            const std::string &version = properties["ro.build.version.release"];
            fb_version = builder.CreateString(version);
        }
        if (properties.find("ro.build.display.id") != properties.end()) {
            const std::string &build = properties["ro.build.display.id"];
            fb_build = builder.CreateString(build);
        }

        v3::MbRomBuilder mrb(builder);
        mrb.add_id(fb_id);
        mrb.add_system_path(fb_system_path);
        mrb.add_cache_path(fb_cache_path);
        mrb.add_data_path(fb_data_path);
        mrb.add_version(fb_version);
        mrb.add_build(fb_build);
        auto fb_rom = mrb.Finish();

        fb_roms.push_back(fb_rom);
    }

    // Create response
    auto fb_roms_vec = builder.CreateVector(fb_roms);
    auto response = v3::CreateMbGetInstalledRomsResponse(builder, fb_roms_vec);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbGetInstalledRomsResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_get_version(int fd, const v3::Request *msg)
{
    (void) msg;

    fb::FlatBufferBuilder builder;

    // Get version
    auto version = builder.CreateString(get_mbtool_version());
    auto response = v3::CreateMbGetVersionResponse(builder, version);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbGetVersionResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_set_kernel(int fd, const v3::Request *msg)
{
    auto request = (v3::MbSetKernelRequest *) msg->request();
    if (!request->rom_id() || !request->boot_blockdev()) {
        return v3_send_response_invalid(fd);
    }

    fb::FlatBufferBuilder builder;

    bool success = set_kernel(request->rom_id()->c_str(),
                              request->boot_blockdev()->c_str());

    // Create response
    auto response = v3::CreateMbSetKernelResponse(builder, success);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbSetKernelResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_switch_rom(int fd, const v3::Request *msg)
{
    auto request = (v3::MbSwitchRomRequest *) msg->request();
    if (!request->rom_id() || !request->boot_blockdev()) {
        return v3_send_response_invalid(fd);
    }

    std::vector<std::string> block_dev_dirs;

    if (request->blockdev_base_dirs()) {
        for (auto const &base_dir : *request->blockdev_base_dirs()) {
            block_dev_dirs.push_back(base_dir->c_str());
        }
    }

    bool force_update_checksums = request->force_update_checksums();

    fb::FlatBufferBuilder builder;

    SwitchRomResult ret = switch_rom(request->rom_id()->c_str(),
                                     request->boot_blockdev()->c_str(),
                                     block_dev_dirs,
                                     force_update_checksums);

    bool success = ret == SwitchRomResult::SUCCEEDED;
    v3::MbSwitchRomResult fb_ret = v3::MbSwitchRomResult_FAILED;
    switch (ret) {
    case SwitchRomResult::SUCCEEDED:
        fb_ret = v3::MbSwitchRomResult_SUCCEEDED;
        break;
    case SwitchRomResult::FAILED:
        fb_ret = v3::MbSwitchRomResult_FAILED;
        break;
    case SwitchRomResult::CHECKSUM_NOT_FOUND:
        fb_ret = v3::MbSwitchRomResult_CHECKSUM_NOT_FOUND;
        break;
    case SwitchRomResult::CHECKSUM_INVALID:
        fb_ret = v3::MbSwitchRomResult_CHECKSUM_INVALID;
        break;
    }

    // Create response
    auto response = v3::CreateMbSwitchRomResponse(builder, success, fb_ret);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbSwitchRomResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_wipe_rom(int fd, const v3::Request *msg)
{
    auto request = (v3::MbWipeRomRequest *) msg->request();
    if (!request->rom_id()) {
        return v3_send_response_invalid(fd);
    }

    // Find and verify ROM is installed
    Roms roms;
    roms.add_installed();

    auto rom = roms.find_by_id(request->rom_id()->c_str());
    if (!rom) {
        LOGE("Tried to wipe non-installed or invalid ROM ID: %s",
             request->rom_id()->c_str());
        return v3_send_response_invalid(fd);
    }

    // The GUI should check this, but we'll enforce it here
    auto current_rom = Roms::get_current_rom();
    if (current_rom && current_rom->id == rom->id) {
        LOGE("Cannot wipe currently booted ROM: %s", rom->id.c_str());
        return v3_send_response_invalid(fd);
    }

    // Wipe the selected targets
    std::vector<int16_t> succeeded;
    std::vector<int16_t> failed;

    if (request->targets()) {
        std::string raw_system = get_raw_path("/system");
        if (mount("", raw_system.c_str(), "", MS_REMOUNT, "") < 0) {
            LOGW("Failed to mount %s as writable: %s",
                 raw_system.c_str(), strerror(errno));
        }

        for (short target : *request->targets()) {
            bool success = false;

            if (target == v3::MbWipeTarget_SYSTEM) {
                success = wipe_system(rom);
            } else if (target == v3::MbWipeTarget_CACHE) {
                success = wipe_cache(rom);
            } else if (target == v3::MbWipeTarget_DATA) {
                success = wipe_data(rom);
            } else if (target == v3::MbWipeTarget_DALVIK_CACHE) {
                success = wipe_dalvik_cache(rom);
            } else if (target == v3::MbWipeTarget_MULTIBOOT) {
                success = wipe_multiboot(rom);
            } else {
                LOGE("Unknown wipe target %d", target);
            }

            if (success) {
                succeeded.push_back(target);
            } else {
                failed.push_back(target);
            }
        }
    }

    fb::FlatBufferBuilder builder;

    // Create response
    auto fb_succeeded = builder.CreateVector(succeeded);
    auto fb_failed = builder.CreateVector(failed);
    auto response = v3::CreateMbWipeRomResponse(
            builder, fb_succeeded, fb_failed);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbWipeRomResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_mb_get_packages_count(int fd, const v3::Request *msg)
{
    auto request = (v3::MbGetPackagesCountRequest *) msg->request();
    if (!request->rom_id()) {
        return v3_send_response_invalid(fd);
    }

    // Find and verify ROM is installed
    Roms roms;
    roms.add_installed();

    auto rom = roms.find_by_id(request->rom_id()->c_str());
    if (!rom) {
        return v3_send_response_invalid(fd);
    }

    std::string packages_xml(rom->full_data_path());
    packages_xml += "/system/packages.xml";

    fb::FlatBufferBuilder builder;
    v3::MbGetPackagesCountResponseBuilder response_builder(builder);

    Packages pkgs;
    if (pkgs.load_xml(packages_xml)) {
        unsigned int system_pkgs = 0;
        unsigned int update_pkgs = 0;
        unsigned int other_pkgs = 0;

        for (std::shared_ptr<Package> pkg : pkgs.pkgs) {
            bool is_system = (pkg->pkg_flags & Package::FLAG_SYSTEM)
                    || (pkg->pkg_public_flags & Package::PUBLIC_FLAG_SYSTEM);
            bool is_update = (pkg->pkg_flags & Package::FLAG_UPDATED_SYSTEM_APP)
                    || (pkg->pkg_public_flags & Package::PUBLIC_FLAG_UPDATED_SYSTEM_APP);

            if (is_update) {
                ++update_pkgs;
            } else if (is_system) {
                ++system_pkgs;
            } else {
                ++other_pkgs;
            }
        }

        response_builder.add_success(true);
        response_builder.add_system_packages(system_pkgs);
        response_builder.add_system_update_packages(update_pkgs);
        response_builder.add_non_system_packages(other_pkgs);
    } else {
        response_builder.add_success(false);
    }

    auto response = response_builder.Finish();

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_MbGetPackagesCountResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

static bool v3_reboot(int fd, const v3::Request *msg)
{
    auto request = (v3::RebootRequest *) msg->request();

    fb::FlatBufferBuilder builder;

    std::string reboot_arg;
    if (request->arg()) {
        reboot_arg = request->arg()->c_str();
    }

    // The client probably won't get the chance to see the success message, but
    // we'll still send it for the sake of symmetry
    bool success = reboot_via_init(reboot_arg);

    // Create response
    auto response = v3::CreateRebootResponse(builder, success);

    // Wrap response
    v3::ResponseBuilder rb(builder);
    rb.add_response_type(v3::ResponseType_RebootResponse);
    rb.add_response(response.Union());
    builder.Finish(rb.Finish());

    return v3_send_response(fd, builder);
}

bool connection_version_3(int fd)
{
    std::string command;

    auto close_all_fds = util::finally([&]{
        // Ensure opened fd's are closed if the connection is lost
        for (auto &p : fd_map) {
            close(p.second);
        }
        fd_map.clear();
    });

    while (1) {
        std::vector<uint8_t> data;
        if (!util::socket_read_bytes(fd, &data)) {
            return false;
        }

        auto verifier = fb::Verifier(data.data(), data.size());
        if (!v3::VerifyRequestBuffer(verifier)) {
            LOGE("Received invalid buffer");
            return false;
        }

        const v3::Request *request = v3::GetRequest(data.data());
        v3::RequestType type = request->request_type();

        // NOTE: A false return value indicates a connection error, not a
        //       command failure!
        bool ret = true;

        if (type == v3::RequestType_FileChmodRequest) {
            ret = v3_file_chmod(fd, request);
        } else if (type == v3::RequestType_FileCloseRequest) {
            ret = v3_file_close(fd, request);
        } else if (type == v3::RequestType_FileOpenRequest) {
            ret = v3_file_open(fd, request);
        } else if (type == v3::RequestType_FileReadRequest) {
            ret = v3_file_read(fd, request);
        } else if (type == v3::RequestType_FileSeekRequest) {
            ret = v3_file_seek(fd, request);
        } else if (type == v3::RequestType_FileSELinuxGetLabelRequest) {
            ret = v3_file_selinux_get_label(fd, request);
        } else if (type == v3::RequestType_FileSELinuxSetLabelRequest) {
            ret = v3_file_selinux_set_label(fd, request);
        } else if (type == v3::RequestType_FileStatRequest) {
            ret = v3_file_stat(fd, request);
        } else if (type == v3::RequestType_FileWriteRequest) {
            ret = v3_file_write(fd, request);
        } else if (type == v3::RequestType_PathChmodRequest) {
            ret = v3_path_chmod(fd, request);
        } else if (type == v3::RequestType_PathCopyRequest) {
            ret = v3_path_copy(fd, request);
        } else if (type == v3::RequestType_PathSELinuxGetLabelRequest) {
            ret = v3_path_selinux_get_label(fd, request);
        } else if (type == v3::RequestType_PathSELinuxSetLabelRequest) {
            ret = v3_path_selinux_set_label(fd, request);
        } else if (type == v3::RequestType_PathGetDirectorySizeRequest) {
            ret = v3_path_get_directory_size(fd, request);
        } else if (type == v3::RequestType_MbGetBootedRomIdRequest) {
            ret = v3_mb_get_booted_rom_id(fd, request);
        } else if (type == v3::RequestType_MbGetInstalledRomsRequest) {
            ret = v3_mb_get_installed_roms(fd, request);
        } else if (type == v3::RequestType_MbGetVersionRequest) {
            ret = v3_mb_get_version(fd, request);
        } else if (type == v3::RequestType_MbSetKernelRequest) {
            ret = v3_mb_set_kernel(fd, request);
        } else if (type == v3::RequestType_MbSwitchRomRequest) {
            ret = v3_mb_switch_rom(fd, request);
        } else if (type == v3::RequestType_MbWipeRomRequest) {
            ret = v3_mb_wipe_rom(fd, request);
        } else if (type == v3::RequestType_MbGetPackagesCountRequest) {
            ret = v3_mb_get_packages_count(fd, request);
        } else if (type == v3::RequestType_RebootRequest) {
            ret = v3_reboot(fd, request);
        } else {
            // Invalid command; allow further commands
            ret = v3_send_response_unsupported(fd);
        }

        if (!ret) {
            return false;
        }
    }

    return true;
}

}