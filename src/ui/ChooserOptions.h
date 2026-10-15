#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>

enum class ChooserMode { OpenFile, SaveFile, SaveFiles };

struct ChooserFilter {
    QString id;
    QString label;
    QStringList globs;
    QStringList mimeTypes;
};

struct ChooserChoiceOption {
    QString id;
    QString label;
};

struct ChooserChoice {
    QString id;
    QString label;
    QList<ChooserChoiceOption> options;
    QString selected;
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
    // Qt mnemonic text; adapters translate their protocol's mnemonic syntax.
    QString acceptLabel;
    Qt::CaseSensitivity filterCaseSensitivity = Qt::CaseInsensitive;
    QList<ChooserChoice> choices;
    // SaveFiles picks one folder and returns destinations in this order; no writes.
    QStringList saveNames;
};
