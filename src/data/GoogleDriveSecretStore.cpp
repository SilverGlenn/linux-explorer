#include "GoogleDriveSecretStore.h"

#include <QFutureWatcher>
#include <QLibrary>
#include <QTimer>
#include <QtConcurrentRun>
#include <memory>
#include <utility>

namespace {
using Result = GoogleDriveCredentialStore::Result;
struct List { void *data; List *next; List *previous; };
struct Error { unsigned domain; int code; char *message; };
struct Schema;
struct Api {
    QLibrary secret{QStringLiteral("secret-1"), 0};
    QLibrary glib{QStringLiteral("glib-2.0"), 0};
    QLibrary object{QStringLiteral("gobject-2.0"), 0};
    QLibrary gio{QStringLiteral("gio-2.0"), 0};
    void *(*serviceGet)(int, void *, Error **) = nullptr;
    List *(*search)(void *, const Schema *, void *, int, void *, Error **) = nullptr;
    int (*locked)(void *) = nullptr;
    int (*load)(void *, void *, Error **) = nullptr;
    void *(*getSecret)(void *) = nullptr;
    const char *(*valueGet)(void *, size_t *) = nullptr;
    void (*valueUnref)(void *) = nullptr;
    int (*store)(const Schema *, const char *, const char *, const char *, void *, Error **, ...) = nullptr;
    int (*clear)(const Schema *, void *, Error **, ...) = nullptr;
    void *(*hashNew)(void *, void *) = nullptr;
    int (*hashInsert)(void *, void *, void *) = nullptr;
    void (*hashUnref)(void *) = nullptr;
    void (*listFree)(List *) = nullptr;
    void (*errorFree)(Error *) = nullptr;
    void (*unref)(void *) = nullptr;
    void *(*cancelNew)() = nullptr;
    void (*cancel)(void *) = nullptr;
    bool available = false;
    Api()
    {
#define RESOLVE(library, member, symbol) member = reinterpret_cast<decltype(member)>(library.resolve(symbol))
        RESOLVE(secret, serviceGet, "secret_service_get_sync");
        RESOLVE(secret, search, "secret_service_search_sync");
        RESOLVE(secret, locked, "secret_item_get_locked");
        RESOLVE(secret, load, "secret_item_load_secret_sync");
        RESOLVE(secret, getSecret, "secret_item_get_secret");
        RESOLVE(secret, valueGet, "secret_value_get");
        RESOLVE(secret, valueUnref, "secret_value_unref");
        RESOLVE(secret, store, "secret_password_store_sync");
        RESOLVE(secret, clear, "secret_password_clear_sync");
        RESOLVE(glib, hashNew, "g_hash_table_new");
        RESOLVE(glib, hashInsert, "g_hash_table_insert");
        RESOLVE(glib, hashUnref, "g_hash_table_unref");
        RESOLVE(glib, listFree, "g_list_free");
        RESOLVE(glib, errorFree, "g_error_free");
        RESOLVE(object, unref, "g_object_unref");
        RESOLVE(gio, cancelNew, "g_cancellable_new");
        RESOLVE(gio, cancel, "g_cancellable_cancel");
#undef RESOLVE
        available = serviceGet && search && locked && load && getSecret && valueGet && valueUnref
            && store && clear && hashNew && hashInsert && hashUnref && listFree && errorFree
            && unref && cancelNew && cancel;
    }
};
Api &api() { static Api instance; return instance; }

// The public libsecret ABI has a fixed, reserved tail for future extensions.
struct Attribute { const char *name; int type; };
struct SecretSchema {
    const char *name;
    int flags;
    Attribute attributes[32];
    int reserved;
    void *reservedPointers[7];
};
// Values from libsecret's public secret-schema.h and secret-types.h. Bit zero
// is reserved; retaining the names here avoids assuming consecutive flag bits.
constexpr int schemaDontMatchName = 1 << 1;
constexpr int searchAll = 1 << 1;
constexpr int searchUnlock = 1 << 2;
constexpr int searchLoadSecrets = 1 << 3;
const SecretSchema schema{"org.qt.keychain", schemaDontMatchName,
    {{"user", 0}, {"server", 0}, {"type", 0}}, 0, {}};
constexpr auto serviceName = "explorer-win7-google-drive";
struct Answer { Result result = Result::Failed; QByteArray record; };

Answer perform(const QString &key, GoogleDriveSecretStore::Action action, bool silent,
               QByteArray record, const std::shared_ptr<void> &cancellable)
{
    auto &a = api();
    const QByteArray encodedKey = key.toUtf8();
    Error *error = nullptr;
    void *service = a.serviceGet(0, cancellable.get(), &error);
    if (!service) {
        if (error) a.errorFree(error);
        return {Result::Unavailable, {}};
    }
    a.unref(service);
    if (action == GoogleDriveSecretStore::Action::Write) {
        const QByteArray encoded = record.toBase64();
        record.fill('\0');
        const bool stored = a.store(reinterpret_cast<const Schema *>(&schema), "default",
            "Explorer Google Drive", encoded.constData(), cancellable.get(), &error,
            "user", encodedKey.constData(), "server", serviceName, "type", "base64", nullptr);
        if (error) a.errorFree(error);
        return {stored ? Result::Success : Result::Failed, {}};
    }
    if (action == GoogleDriveSecretStore::Action::Remove) {
        a.clear(reinterpret_cast<const Schema *>(&schema), cancellable.get(), &error,
            "user", encodedKey.constData(), "server", serviceName, nullptr);
        const bool successful = !error;
        if (error) a.errorFree(error);
        return {successful ? Result::Success : Result::Failed, {}};
    }
    void *attributes = a.hashNew(reinterpret_cast<void *>(a.glib.resolve("g_str_hash")),
                                reinterpret_cast<void *>(a.glib.resolve("g_str_equal")));
    a.hashInsert(attributes, const_cast<char *>("user"), const_cast<char *>(encodedKey.constData()));
    a.hashInsert(attributes, const_cast<char *>("server"), const_cast<char *>(serviceName));
    a.hashInsert(attributes, const_cast<char *>("type"), const_cast<char *>("base64"));
    // Automatic reads deliberately omit both unlock and eager load. Loading an unlocked item's
    // secret below does not unlock it if it locks during this operation.
    List *items = a.search(nullptr, reinterpret_cast<const Schema *>(&schema), attributes,
                           silent ? searchAll : searchAll | searchUnlock | searchLoadSecrets,
                           cancellable.get(), &error);
    a.hashUnref(attributes);
    Answer answer{error ? Result::Failed : Result::Missing, {}};
    if (error) a.errorFree(error);
    if (items && items->next) {
        // Ambiguous records must never choose an arbitrary saved account.
        answer.result = Result::Failed;
    } else if (items) {
        if (a.locked(items->data)) {
            answer.result = Result::InteractionRequired;
        } else {
            error = nullptr;
            if (!a.load(items->data, cancellable.get(), &error)) {
                answer.result = Result::InteractionRequired;
            } else if (void *value = a.getSecret(items->data)) {
                size_t size = 0;
                const char *data = a.valueGet(value, &size);
                if (data && size <= 90000) {
                    const auto decoded = QByteArray::fromBase64Encoding(QByteArray(data, qsizetype(size)),
                        QByteArray::AbortOnBase64DecodingErrors);
                    if (decoded && decoded.decoded.size() <= 65536) {
                        answer = {Result::Success, decoded.decoded};
                    }
                }
                a.valueUnref(value);
            }
            if (error) a.errorFree(error);
        }
    }
    for (List *item = items; item; item = item->next) a.unref(item->data);
    if (items) a.listFree(items);
    return answer;
}
}

void GoogleDriveSecretStore::run(QObject *owner, const QString &key, Action action, bool silent,
                                QByteArray record, GoogleDriveCredentialStore::Completion completion)
{
    auto &a = api();
    if (!a.available) {
        QTimer::singleShot(0, owner, [completion] { completion(Result::Unavailable, {}); });
        return;
    }
    auto cancellable = std::shared_ptr<void>(a.cancelNew(), [](void *value) { api().unref(value); });
    auto *watcher = new QFutureWatcher<Answer>(owner);
    auto *deadline = new QTimer(watcher);
    deadline->setSingleShot(true);
    auto delivered = std::make_shared<bool>(false);
    QObject::connect(deadline, &QTimer::timeout, owner, [cancellable, delivered, completion, silent] {
        api().cancel(cancellable.get());
        // Explicit writes/removal retain their operation lock until the worker
        // actually settles; cancellation can race a committed wallet change.
        if (silent && !std::exchange(*delivered, true)) completion(Result::Unavailable, {});
    });
    QObject::connect(watcher, &QFutureWatcher<Answer>::finished, owner,
        [watcher, deadline, delivered, completion] {
            deadline->stop();
            const Answer answer = watcher->result();
            if (!std::exchange(*delivered, true)) completion(answer.result, answer.record);
            watcher->deleteLater();
        });
    QObject::connect(owner, &QObject::destroyed, watcher, [cancellable] { api().cancel(cancellable.get()); });
    deadline->start(silent ? 8000 : 180000);
    watcher->setFuture(QtConcurrent::run([key, action, silent, record = std::move(record), cancellable]() mutable {
        return perform(key, action, silent, std::move(record), cancellable);
    }));
}
