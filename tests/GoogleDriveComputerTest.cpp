#include "ComputerModel.h"
#include "ApplicationAppearance.h"
#include "ComputerView.h"
#include "GoogleDriveConnection.h"
#include "Locations.h"
#include "aero/icons.h"

#include <KFilePlacesModel>

#include <QImage>
#include <QDir>
#include <QListView>
#include <QSignalSpy>
#include <QTest>
#include <QToolButton>

#include <utility>

namespace {
// Only the mount-state interface is exercised. No account, keychain, helper,
// browser, or real mount is required to populate Computer.
class FixtureConnection final : public GoogleDriveConnection {
public:
    explicit FixtureConnection(bool connected = false) : m_connected(connected) {}
    bool isConnected() const override { return m_connected; }
    void connectDrive() override { ++authenticationRequests; }
    void signInAgain() override { ++authenticationRequests; }
    void setConnected(bool connected)
    {
        m_connected = connected;
        Q_EMIT mountStateChanged(connected);
    }
    int authenticationRequests = 0;
private:
    bool m_connected;
};

class FixturePlaces final : public KFilePlacesModel {
public:
    void resetPlaces()
    {
        beginResetModel();
        endResetModel();
    }
};

class RecordingComputerModel final : public ComputerModel {
public:
    RecordingComputerModel(KFilePlacesModel *places, GoogleDriveConnection *connection)
        : ComputerModel(places, connection, nullptr) {}
    QList<QUrl> capacityRequests;
protected:
    void queryFreeSpace(const QUrl &url) override { capacityRequests.append(url); }
};

QModelIndex driveIndex(const QAbstractItemModel *model)
{
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, ComputerModel::Name);
        if (index.data(ComputerModel::UrlRole).toUrl() == Locations::googleDriveMount())
            return index;
    }
    return {};
}

int driveCount(const QAbstractItemModel *model)
{
    int count = 0;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->index(row, ComputerModel::Name).data(ComputerModel::UrlRole).toUrl()
            == Locations::googleDriveMount()) {
            ++count;
        }
    }
    return count;
}
}

class GoogleDriveComputerTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        if (qEnvironmentVariableIsSet("EXPLORER_ICON_THEME"))
            QIcon::setThemeName(qEnvironmentVariable("EXPLORER_ICON_THEME"));
        Explorer::configureApplicationAppearance(qApp);
    }

    void connectedMountIsOneFixedDrive()
    {
        FixturePlaces places;
        FixtureConnection connection(true);
        RecordingComputerModel model(&places, &connection);

        QCOMPARE(driveCount(&model), 1);
        const QModelIndex index = driveIndex(&model);
        QCOMPARE(index.data().toString(), QStringLiteral("Google Drive (G:)"));
        QCOMPARE(model.urlForIndex(index), Locations::googleDriveMount());
        QCOMPARE(index.data(ComputerModel::RemovableRole).toBool(), false);
        QCOMPARE(index.siblingAtColumn(ComputerModel::Type).data().toString(),
                 QStringLiteral("Google Drive"));
        QCOMPARE(index.data(ComputerModel::FileSystemTypeRole).toString(),
                 QStringLiteral("Google Drive"));
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        QVERIFY(!icon.isNull());
        QCOMPARE(icon.pixmap(64, 64).toImage(), Aero::googleDriveIcon().pixmap(64, 64).toImage());

        QVERIFY(!index.data(ComputerModel::SizeKnownRole).toBool());
        QCOMPARE(index.data(ComputerModel::TotalSizeRole).toULongLong(), 0ULL);
        QCOMPARE(index.data(ComputerModel::AvailableSizeRole).toULongLong(), 0ULL);
        QVERIFY(index.siblingAtColumn(ComputerModel::TotalSize).data().toString().isEmpty());
        QVERIFY(index.siblingAtColumn(ComputerModel::FreeSpace).data().toString().isEmpty());
        QVERIFY(!model.placeIndexFor(Locations::googleDriveMount()).isValid());
        QCOMPARE(connection.authenticationRequests, 0);
    }

    void mountChangesAndRefreshNeverQueryRemoteCapacity()
    {
        FixturePlaces places;
        FixtureConnection connection;
        RecordingComputerModel model(&places, &connection);
        QCOMPARE(driveCount(&model), 0);
        connection.setConnected(true);
        QCOMPARE(driveCount(&model), 1);
        model.refresh();
        model.refresh();
        connection.setConnected(true);
        QCOMPARE(driveCount(&model), 1);
        for (const QUrl &url : std::as_const(model.capacityRequests))
            QVERIFY2(!Locations::isGoogleDriveMount(url), qPrintable(url.toString()));
        QVERIFY(!driveIndex(&model).data(ComputerModel::SizeKnownRole).toBool());

        connection.setConnected(false);
        QCOMPARE(driveCount(&model), 0);
        connection.setConnected(true);
        QCOMPARE(driveCount(&model), 1);
        QCOMPARE(connection.authenticationRequests, 0);
    }

    void placesResetsAndBookmarksNeverDuplicateMount()
    {
        FixturePlaces places;
        FixtureConnection connection(true);
        RecordingComputerModel model(&places, &connection);
        const QUrl mount = Locations::googleDriveMount();
        places.addPlace(QStringLiteral("Cloud bookmark"), mount);
        places.addPlace(QStringLiteral("Duplicate cloud bookmark"),
                        QUrl::fromLocalFile(mount.toLocalFile() + QLatin1Char('/')));
        places.addPlace(QStringLiteral("Cloud subfolder bookmark"),
                        QUrl::fromLocalFile(mount.toLocalFile() + QStringLiteral("/Documents")));
        places.resetPlaces();
        QCOMPARE(driveCount(&model), 1);
        QVERIFY(!model.placeIndexFor(mount).isValid());
        for (int row = 0; row < places.rowCount(); ++row) {
            const QModelIndex place = places.index(row, 0);
            if (Locations::isGoogleDriveMount(places.url(place)))
                places.setPlaceHidden(place, true);
        }
        places.resetPlaces();
        QCOMPARE(driveCount(&model), 1);
        QCOMPARE(driveIndex(&model).data().toString(), QStringLiteral("Google Drive (G:)"));
        connection.setConnected(false);
        places.resetPlaces();
        QCOMPARE(driveCount(&model), 0);
        connection.setConnected(true);
        places.resetPlaces();
        QCOMPARE(driveCount(&model), 1);
        QCOMPARE(connection.authenticationRequests, 0);
    }

    void computerViewGroupsGoogleDriveWithHardDisks()
    {
        FixturePlaces places;
        FixtureConnection connection;
        RecordingComputerModel model(&places, &connection);
        ComputerView view(&model);
        view.resize(700, 420);
        view.show();
        auto *fixed = view.findChild<QListView *>(QStringLiteral("fixedDriveList"));
        auto *removable = view.findChild<QListView *>(QStringLiteral("removableDriveList"));
        auto *heading = view.findChild<QToolButton *>(QStringLiteral("fixedDriveHeading"));
        QVERIFY(fixed && removable && heading);
        connection.setConnected(true);
        QCOMPARE(driveCount(fixed->model()), 1);
        QCOMPARE(driveCount(removable->model()), 0);
        QTRY_VERIFY(fixed->isVisible());
        QVERIFY(heading->text().startsWith(QStringLiteral("Hard Disk Drives (")));
        QSignalSpy activated(&view, &ComputerView::urlActivated);
        Q_EMIT fixed->activated(driveIndex(fixed->model()));
        QCOMPARE(activated.size(), 1);
        QCOMPARE(activated.first().first().toUrl(), Locations::googleDriveMount());
        const QString snapshots = qEnvironmentVariable("EXPLORER_SNAPSHOTS");
        if (!snapshots.isEmpty()) {
            QVERIFY(QDir().mkpath(snapshots));
            QVERIFY(view.grab().save(snapshots + QStringLiteral("/google-drive-computer.png")));
            QVERIFY(Aero::googleDriveIcon().pixmap(96, 96).save(
                snapshots + QStringLiteral("/google-drive-icon.png")));
        }
        connection.setConnected(false);
        QCOMPARE(driveCount(fixed->model()), 0);
    }
};

QTEST_MAIN(GoogleDriveComputerTest)
#include "GoogleDriveComputerTest.moc"
