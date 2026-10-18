#pragma once

#include <QString>

class QWidget;

// The two dialogs Windows puts up when it will not let you in, the flat refusal
// and the elevation prompt
//
// Only the wording and the shape are Windows', so nothing here performs or
// retries an operation and what Continue means is the caller's to decide
namespace AccessDialogs {

// Whether an error code means the user is not allowed, so every call site
// agrees on which failures get Windows' wording
bool isPermissionError(int error);

// The secondary line is the reason under the primary one
void showFailure(QWidget *parent, const QString &title, const QString &primary,
                 const QString &secondary = {});

// The same box, titled for a location that cannot be reached
void showLocationUnavailable(QWidget *parent, const QString &path,
                             const QString &reason = {});

// Whether elevating could reasonably be offered for this local path, which it
// cannot for removable media and remote or foreign filesystems
bool canOfferElevation(const QString &localPath);

// The elevation prompt, true for Continue and false for Cancel or Escape.
// The full path is shown as plain text under the folder name.
bool askForAdminAccess(QWidget *parent, const QString &folderName,
                       const QString &path = {});

// What the administrator strip says when clicked, informational so OK only
void showAdministratorWarning(QWidget *parent);

} // namespace AccessDialogs
