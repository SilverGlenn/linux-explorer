#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

enum class ChooserMode { OpenFile, SaveFile };

struct ChooserFilter {
    QString id;
    QString label;
    QStringList globs;
    QStringList mimeTypes;
};

struct ChooserOptions {
    ChooserMode mode = ChooserMode::OpenFile;
    QString title;
    QUrl startingLocation;
    QString suggestedName;
    QList<ChooserFilter> filters;
    QString selectedFilter;
    QString defaultExtension;
    bool localOnly = true;
    bool multiple = false;
    bool directory = false;
};
