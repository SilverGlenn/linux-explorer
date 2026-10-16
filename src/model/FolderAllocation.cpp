#include "FolderAllocation.h"

#include <QFile>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace FolderAllocation {
namespace {
using Inode = std::pair<dev_t, ino_t>;
struct CloseDirectory {
    void operator()(DIR *directory) const { ::closedir(directory); }
};
using Directory = std::unique_ptr<DIR, CloseDirectory>;
struct Frame {
    Directory directory;
    struct stat initial;
};

bool changed(const struct stat &before, const struct stat &after)
{
    return before.st_dev != after.st_dev || before.st_ino != after.st_ino
        || before.st_mtim.tv_sec != after.st_mtim.tv_sec
        || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec
        || before.st_ctim.tv_sec != after.st_ctim.tv_sec
        || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec;
}
} // namespace

Result calculate(const QStringList &paths, const std::shared_ptr<std::atomic_bool> &cancel)
{
    Result result;
    std::set<Inode> visited;
    const auto cancelled = [&] {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.incomplete = true;
        }
        return result.cancelled;
    };
    const auto count = [&](const struct stat &status) {
        if (!visited.emplace(status.st_dev, status.st_ino).second)
            return false;
        ++result.entries;
        // st_blocks is always in 512-byte units, independent of st_blksize.
        if (status.st_blocks < 0
            || quint64(status.st_blocks) > (std::numeric_limits<quint64>::max() - result.bytes) / 512) {
            result.incomplete = true;
        } else {
            result.bytes += quint64(status.st_blocks) * 512;
        }
        return true;
    };
    std::vector<Frame> stack;
    const auto enter = [&](int parent, const char *name, const struct stat &status) {
        const int descriptor = ::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0) {
            result.incomplete = true;
            return;
        }
        struct stat opened {};
        if (::fstat(descriptor, &opened) != 0 || changed(status, opened)) {
            ::close(descriptor);
            result.incomplete = true;
            return;
        }
        Directory directory(::fdopendir(descriptor));
        if (!directory) {
            ::close(descriptor);
            result.incomplete = true;
            return;
        }
        stack.push_back({std::move(directory), opened});
    };

    for (const QString &path : paths) {
        if (cancelled())
            break;
        QByteArray encoded = QFile::encodeName(path);
        // A trailing slash would make lstat follow a symlink to a directory.
        while (encoded.size() > 1 && encoded.endsWith('/'))
            encoded.chop(1);
        if (encoded.isEmpty() || encoded.contains('\0')) {
            result.incomplete = true;
            continue;
        }
        struct stat status {};
        if (::lstat(encoded.constData(), &status) != 0) {
            result.incomplete = true;
            continue;
        }
        if (!count(status) || !S_ISDIR(status.st_mode))
            continue;
        enter(AT_FDCWD, encoded.constData(), status);
        while (!stack.empty() && !cancelled()) {
            Frame &frame = stack.back();
            const int parent = ::dirfd(frame.directory.get());
            errno = 0;
            const auto *entry = ::readdir(frame.directory.get());
            if (!entry) {
                if (errno != 0)
                    result.incomplete = true;
                struct stat after {};
                if (::fstat(parent, &after) != 0 || changed(frame.initial, after))
                    result.incomplete = true;
                stack.pop_back();
                continue;
            }
            if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
                continue;
            struct stat child {};
            if (::fstatat(parent, entry->d_name, &child, AT_SYMLINK_NOFOLLOW) != 0) {
                result.incomplete = true;
                continue;
            }
            if (count(child) && S_ISDIR(child.st_mode))
                enter(parent, entry->d_name, child);
        }
    }
    return result;
}
} // namespace FolderAllocation
