#include "ComputerModel.h"
#include "ApplicationAppearance.h"
#include "ComputerView.h"
#include "GoogleDriveConnection.h"
#include "Locations.h"
#include "aero/icons.h"

#include <KFilePlacesModel>

#include <QImage>
#include <QItemSelectionModel>
#include <QDir>
#include <QListView>
#include <QSignalSpy>
#include <QScopeGuard>
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
    bool hasStorageQuota() const override { return m_connected && quotaKnown; }
    quint64 storageTotalBytes() const override { return quotaTotal; }
    quint64 storageAvailableBytes() const override { return quotaAvailable; }
    void refreshStorageQuota(bool force = false) override
    {
        quotaRequests.append(force);
        if (publishUnknownOnRequest)
            Q_EMIT storageQuotaChanged();
    }
    void connectDrive() override { ++authenticationRequests; }
    void signInAgain() override { ++authenticationRequests; }
    void setConnected(bool connected)
    {
        m_connected = connected;
        Q_EMIT mountStateChanged(connected);
    }
    int authenticationRequests = 0;
    QList<bool> quotaRequests;
    bool publishUnknownOnRequest = false;
    void setQuota(quint64 total, quint64 available, bool known = true)
    {
        quotaKnown = known;
        quotaTotal = total;
        quotaAvailable = available;
        Q_EMIT storageQuotaChanged();
    }
private:
    bool m_connected;
    bool quotaKnown = false;
    quint64 quotaTotal = 0;
    quint64 quotaAvailable = 0;
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

    void suppliedWindowsDriveArtworkSurvivesOtherSystemThemes()
    {
        const QString originalTheme = QIcon::themeName();
        const auto restore = qScopeGuard([&] { QIcon::setThemeName(originalTheme); });
        const QImage base = QPixmap(QStringLiteral(":/win7/google-drive-base/drive-48.png")).toImage();
        QVERIFY(!base.isNull());
        QIcon::setThemeName(QStringLiteral("breeze"));
        const QImage rendered = Aero::googleDriveIcon().pixmap(QSize(48, 48), 1.0).toImage();
        QIcon::setThemeName(QStringLiteral("hicolor"));
        QCOMPARE(Aero::googleDriveIcon().pixmap(QSize(48, 48), 1.0).toImage(), rendered);
        int badgePixels = 0;
        for (int y = 0; y < 48; ++y) {
            for (int x = 0; x < 48; ++x) {
                if (x >= 16 || y < 16 || y >= 32)
                    QCOMPARE(rendered.pixel(x, y), base.pixel(x, y));
                else if (rendered.pixel(x, y) != base.pixel(x, y))
                    ++badgePixels;
            }
        }
        QVERIFY(badgePixels > 10);
    }

    void cachedQuotaSuppliesCapacityFiguresImmediately()
    {
        FixturePlaces places;
        FixtureConnection connection(true);
        connection.setQuota(15000, 12000);
        RecordingComputerModel model(&places, &connection);
        const QModelIndex drive = driveIndex(&model);
        QVERIFY(drive.data(ComputerModel::SizeKnownRole).toBool());
        QCOMPARE(drive.data(ComputerModel::TotalSizeRole).toULongLong(), 15000ULL);
        QCOMPARE(drive.data(ComputerModel::AvailableSizeRole).toULongLong(), 12000ULL);
        QVERIFY(!drive.siblingAtColumn(ComputerModel::TotalSize).data().toString().isEmpty());
        QVERIFY(!QModelIndex(drive).siblingAtColumn(ComputerModel::FreeSpace).data().toString().isEmpty());
        QCOMPARE(connection.quotaRequests, QList<bool>{false});
        QCOMPARE(connection.authenticationRequests, 0);
    }

    void quotaUpdatesRepaintTheTileWithoutLosingSelection()
    {
        FixturePlaces places;
        FixtureConnection connection(true);
        RecordingComputerModel model(&places, &connection);
        QItemSelectionModel selection(&model);
        const QPersistentModelIndex drive = driveIndex(&model);
        selection.select(drive, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        connection.setQuota(1000, 600);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(changes.size(), 1);
        QVERIFY(drive.isValid());
        QVERIFY(selection.isSelected(drive));
        QCOMPARE(changes.first().at(0).value<QModelIndex>().column(), int(ComputerModel::Name));
        QCOMPARE(changes.first().at(1).value<QModelIndex>().column(), int(ComputerModel::FreeSpace));
        const QList<int> roles = changes.first().at(2).value<QList<int>>();
        QVERIFY(roles.contains(ComputerModel::SizeKnownRole));
        QVERIFY(roles.contains(ComputerModel::TotalSizeRole));
        QVERIFY(roles.contains(ComputerModel::AvailableSizeRole));
        QCOMPARE(drive.data(ComputerModel::AvailableSizeRole).toULongLong(), 600ULL);
        connection.setQuota(1000, 0); // a full account still has a known quota
        QCOMPARE(resets.size(), 0);
        QVERIFY(drive.data(ComputerModel::SizeKnownRole).toBool());
        QCOMPARE(drive.data(ComputerModel::AvailableSizeRole).toULongLong(), 0ULL);
        QVERIFY(!QModelIndex(drive).siblingAtColumn(ComputerModel::FreeSpace).data().toString().isEmpty());
        connection.setQuota(1000, 0, false);
        QVERIFY(!drive.data(ComputerModel::SizeKnownRole).toBool());
        QCOMPARE(drive.data(ComputerModel::TotalSizeRole).toULongLong(), 0ULL);
        QCOMPARE(drive.data(ComputerModel::AvailableSizeRole).toULongLong(), 0ULL);
        QVERIFY(selection.isSelected(drive));
        QCOMPARE(resets.size(), 0);
        QCOMPARE(connection.quotaRequests, QList<bool>{false});
    }

    void invalidQuotaRemainsUnknown_data()
    {
        QTest::addColumn<quint64>("total");
        QTest::addColumn<quint64>("available");
        QTest::newRow("unknown-total") << quint64(0) << quint64(0);
        QTest::newRow("available-exceeds-total") << quint64(100) << quint64(101);
    }

    void invalidQuotaRemainsUnknown()
    {
        QFETCH(quint64, total);
        QFETCH(quint64, available);
        FixturePlaces places;
        FixtureConnection connection(true);
        RecordingComputerModel model(&places, &connection);
        connection.setQuota(total, available);
        const QModelIndex drive = driveIndex(&model);
        QVERIFY(!drive.data(ComputerModel::SizeKnownRole).toBool());
        QCOMPARE(drive.data(ComputerModel::TotalSizeRole).toULongLong(), 0ULL);
        QCOMPARE(drive.data(ComputerModel::AvailableSizeRole).toULongLong(), 0ULL);
    }

    void unknownQuotaDoesNotLoopOnPlacesChangesButRefreshRetries()
    {
        FixturePlaces places;
        FixtureConnection connection(true);
        connection.publishUnknownOnRequest = true;
        RecordingComputerModel model(&places, &connection);
        QCOMPARE(connection.quotaRequests, QList<bool>{false});
        connection.setQuota(0, 0, false);
        places.resetPlaces();
        places.resetPlaces();
        QCOMPARE(connection.quotaRequests, QList<bool>{false});
        model.refresh();
        QCOMPARE(connection.quotaRequests, (QList<bool>{false, true}));
        connection.setConnected(false);
        connection.setConnected(true);
        QCOMPARE(connection.quotaRequests, (QList<bool>{false, true, false}));
        // A same-boolean notification can represent a new kernel mount ID.
        connection.setConnected(true);
        QCOMPARE(connection.quotaRequests, (QList<bool>{false, true, false, false}));
        for (const QUrl &url : std::as_const(model.capacityRequests))
            QVERIFY(!Locations::isGoogleDriveMount(url));
        QCOMPARE(connection.authenticationRequests, 0);
    }

    void actualRootDiskStaysFirstUnderEverySort()
    {
        FixturePlaces places;
        FixtureConnection connection;
        RecordingComputerModel model(&places, &connection);
        // KFilePlacesModel's device classification is not virtual. These pure
        // rows exercise the actual comparator without host-device simulation.
        ComputerModel::Device root;
        root.url = QUrl::fromLocalFile(QStringLiteral("/"));
        root.name = QStringLiteral("ZZZ renamed root");
        root.type = QStringLiteral("ZZZ");
        // Unknown root capacity must still precede all known drives.
        ComputerModel::Device impostor;
        impostor.url = QUrl::fromLocalFile(QStringLiteral("/tmp/impostor"));
        impostor.name = QStringLiteral("Local Disk (C:)");
        impostor.type = QStringLiteral("AAA");
        impostor.sizeKnown = true;
        impostor.total = 2;
        impostor.available = 1;
        ComputerModel::Device cloud;
        cloud.url = Locations::googleDriveMount();
        cloud.name = QStringLiteral("Google Drive (G:)");
        cloud.type = QStringLiteral("Google Drive");
        cloud.sizeKnown = true;
        cloud.total = 1000;
        cloud.available = 500;
        const QList<ComputerModel::Device> rows{impostor, cloud, root};
        for (int key = ComputerModel::SortByName; key <= ComputerModel::SortByType; ++key) {
            for (Qt::SortOrder order : {Qt::AscendingOrder, Qt::DescendingOrder}) {
                model.setSortKey(ComputerModel::SortKey(key), order == Qt::AscendingOrder
                                 ? Qt::DescendingOrder : Qt::AscendingOrder);
                model.m_devices = rows;
                model.setSortKey(ComputerModel::SortKey(key), order);
                QCOMPARE(model.urlForIndex(model.index(0, 0)), root.url);
                const bool cloudBefore = key == ComputerModel::SortByName
                    ? order == Qt::AscendingOrder : order == Qt::DescendingOrder;
                QCOMPARE(model.urlForIndex(model.index(1, 0)),
                         cloudBefore ? cloud.url : impostor.url);
            }
        }
    }

    void quotaResetsOnlyWhenSizeOrderingActuallyChanges()
    {
        FixturePlaces places;
        FixtureConnection connection(true);
        RecordingComputerModel model(&places, &connection);
        ComputerModel::Device root;
        root.url = QUrl::fromLocalFile(QStringLiteral("/"));
        root.name = QStringLiteral("Local Disk (C:)");
        ComputerModel::Device other;
        other.url = QUrl::fromLocalFile(QStringLiteral("/tmp/other-drive"));
        other.name = QStringLiteral("Other drive");
        other.sizeKnown = true;
        other.total = 500;
        other.available = 400;
        const ComputerModel::Device cloud = model.m_devices.at(driveIndex(&model).row());
        model.m_devices = {other, cloud, root};
        model.setSortKey(ComputerModel::SortBySize);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changes(&model, &QAbstractItemModel::dataChanged);
        connection.setQuota(200, 100);
        QCOMPARE(resets.size(), 1);
        QCOMPARE(model.urlForIndex(model.index(0, 0)), root.url);
        QCOMPARE(model.urlForIndex(model.index(1, 0)), cloud.url);
        connection.setQuota(250, 120);
        QCOMPARE(resets.size(), 1);
        QCOMPARE(changes.size(), 1);
        connection.setQuota(800, 600);
        QCOMPARE(resets.size(), 2);
        QCOMPARE(model.urlForIndex(model.index(0, 0)), root.url);
        QCOMPARE(model.urlForIndex(model.index(2, 0)), cloud.url);
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
        connection.setQuota(15ULL * 1024 * 1024 * 1024, 12ULL * 1024 * 1024 * 1024);
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
