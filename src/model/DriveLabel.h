#pragma once

#include <QString>

class KFilePlacesModel;
class QModelIndex;

// The system root is displayed as Local Disk (C:). Other volumes use their
// label and kernel device node. These names never change a mount or volume.
//
// Rename dialogs prefill the bare label, or committing the node back would make
// the suffix permanent and append a second one next time
namespace DriveLabel {

// The plain places model label when there is no device to bracket
QString forPlace(const KFilePlacesModel *places, const QModelIndex &index);

QString deviceNode(const KFilePlacesModel *places, const QModelIndex &index);

// The partition's filesystem, e.g. "NTFS" or "exFAT", or "RAW" when the
// volume carries none the kernel recognises
QString fileSystemType(const KFilePlacesModel *places, const QModelIndex &index);

} // namespace DriveLabel
