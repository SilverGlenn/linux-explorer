#pragma once

#include <QStringList>
#include <QtGlobal>

#include <atomic>
#include <memory>

// Read-only local allocation scan. Symlinks contribute their own blocks;
// directory and file inodes are counted once across the entire selection.
namespace FolderAllocation {
struct Result {
    quint64 bytes = 0;
    quint64 entries = 0;
    bool incomplete = false;
    bool cancelled = false;
};

// This may perform substantial filesystem I/O: call it on a worker thread.
Result calculate(const QStringList &paths,
                 const std::shared_ptr<std::atomic_bool> &cancel = {});
} // namespace FolderAllocation
