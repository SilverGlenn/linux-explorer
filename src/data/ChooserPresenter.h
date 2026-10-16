#pragma once

#include "ChooserOptions.h"
#include "ChooserWindow.h"

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <functional>

class QProcess;
class QTimer;

struct ChooserSelection {
    QList<QUrl> urls;
    QString filter;
    QMap<QString, QString> choices;
};

// One private presenter process for a request whose foreign parent uses the
// other native window system. The portal process keeps every D-Bus authority,
// request handle, cancellation decision and final result validation.
class ChooserPresenter : public QObject {
public:
    using Completion = std::function<void(uint, const ChooserSelection &)>;

    explicit ChooserPresenter(QObject *parent = nullptr);
    ~ChooserPresenter() override;

    static QString requiredPlatform(const Explorer::ChooserWindowOptions &options);
    static int runChild();

    void start(const ChooserOptions &chooser, const Explorer::ChooserWindowOptions &window,
               Completion completion);
    void cancel();

private:
    void readOutput();
    void complete(uint response, const ChooserSelection &selection = {});
    void releaseProcess(bool force);

    QPointer<QProcess> m_process;
    QTimer *m_startupTimer;
    Completion m_completion;
    QByteArray m_output;
    ChooserSelection m_selection;
    uint m_response = 2;
    bool m_ready = false;
    bool m_received = false;
    bool m_done = false;
};
