#include "DetailsPane.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QTemporaryDir>
#include <QtTest>
#include <KFileItem>

class DetailsPaneTest : public QObject {
    Q_OBJECT
private slots:
    void selectedDirectoryUsesFlexibleReadableFields()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.path() + QStringLiteral("/A directory name long enough to need room");
        QVERIFY(QDir().mkpath(path));

        DetailsPane pane;
        pane.resize(700, 54);
        pane.show();
        pane.showSelection({KFileItem(QUrl::fromLocalFile(path))});
        QCoreApplication::processEvents();

        auto *name = pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"));
        auto *type = pane.findChild<QLabel *>(QStringLiteral("detailsSecondary"));
        auto *date = pane.findChild<QLabel *>(QStringLiteral("detailsTopMetadata"));
        QVERIFY(name && type && date);
        QCOMPARE(name->toolTip(), QStringLiteral("A directory name long enough to need room"));
        QCOMPARE(name->accessibleName(), name->toolTip());
        QCOMPARE(type->toolTip(), QStringLiteral("File folder"));
        QVERIFY(date->toolTip().startsWith(QStringLiteral("Date modified:")));
        QVERIFY(name->width() > 100);
        QCOMPARE(name->geometry().center().y(), date->geometry().center().y());

        pane.resize(260, 54);
        QCoreApplication::processEvents();
        QCOMPARE(name->toolTip(), QStringLiteral("A directory name long enough to need room"));
        QCOMPARE(type->toolTip(), QStringLiteral("File folder"));
    }

    void multipleSelectionAndComputerContextsRemainInformative()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString filePath = temporary.path() + QStringLiteral("/note.txt");
        QFile file(filePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("note"), qint64(4));
        file.close();
        const QString directoryPath = temporary.path() + QStringLiteral("/folder");
        QVERIFY(QDir().mkpath(directoryPath));

        DetailsPane pane;
        pane.showSelection({KFileItem(QUrl::fromLocalFile(filePath)),
                            KFileItem(QUrl::fromLocalFile(directoryPath))});
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("2 items selected"));
        QVERIFY(pane.findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"))->toolTip().contains(QStringLiteral("files only")));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsTopMetadata"))->toolTip(), QString());

        pane.showComputerSummary();
        QVERIFY(!pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip().isEmpty());
    }

    void summariesClearSelectionMetadata()
    {
        DetailsPane pane;
        pane.showSelection({KFileItem(QUrl::fromLocalFile(QDir::homePath()))});
        pane.showFolderSummary(3);
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("3 items"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsTopMetadata"))->toolTip(), QString());
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsBottomMetadata"))->toolTip(), QString());

        pane.showLibrary(QIcon(), QStringLiteral("Documents"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("Documents"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsSecondary"))->toolTip(), QStringLiteral("Library"));
        pane.showMessage(QStringLiteral("Unavailable"));
        QCOMPARE(pane.findChild<QLabel *>(QStringLiteral("detailsPrimary"))->toolTip(), QStringLiteral("Unavailable"));
        QCOMPARE(pane.height(), 54);
    }
};

QTEST_MAIN(DetailsPaneTest)
#include "DetailsPaneTest.moc"
