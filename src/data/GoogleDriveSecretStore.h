#pragma once

#include "GoogleDriveAuth.h"

// Uses the standard Secret Service wallet through libsecret. Silent reads never
// request unlocking; explicit reads/writes may use the wallet's normal prompt.
namespace GoogleDriveSecretStore {
enum class Action { Read, Write, Remove };
void run(QObject *owner, const QString &key, Action action, bool silent,
         QByteArray record, GoogleDriveCredentialStore::Completion completion);
}
