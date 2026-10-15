#include "ApplicationAppearance.h"
#include "Assets.h"
#include "aero/buttons.h"
#include "aero/icons.h"
#include <AeroQt/stylesheet.h>

#include <QApplication>
#include <QEvent>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

namespace {
// Native scroll bars rather than the theme's skinned ones
//
// They cannot be rescued one widget at a time, since while an application wide
// stylesheet is active Qt wraps even an explicitly restyled widget back into the
// stylesheet engine, so removing the rules is the only bypass
static QString withoutScrollBarRules(QString qss)
{
    // Comments first, so a brace inside one cannot derail the block scan
    static const QRegularExpression comment(
        QStringLiteral(R"(/\*.*?\*/)"),
        QRegularExpression::DotMatchesEverythingOption);
    qss.remove(comment);

    // A stylesheet has no nested braces, so walk the blocks and drop the
    // selectors naming a scroll bar
    QString out;
    out.reserve(qss.size());
    int pos = 0;
    while (pos < qss.size()) {
        const int open = qss.indexOf(QLatin1Char('{'), pos);
        int close = -1;
        if (open >= 0)
            close = qss.indexOf(QLatin1Char('}'), open);

        // No complete block left, so the rest is copied as it is
        if (close < 0) {
            out += QStringView(qss).mid(pos);
            break;
        }

        const QString selectorList = qss.mid(pos, open - pos);
        const QString body = qss.mid(open, close - open + 1);

        QStringList kept;
        const QStringList selectors = selectorList.split(QLatin1Char(','));
        for (const QString &selector : selectors) {
            if (!selector.contains(QLatin1String("QScrollBar")))
                kept << selector;
        }
        if (!kept.isEmpty())
            out += kept.join(QLatin1Char(',')) + body;

        pos = close + 1;
    }
    return out;
}

// The theme reapplies its sheet whenever the desktop changes, which arrives as
// a style change, and the strip is deferred since mutating the stylesheet
// during delivery would reenter the style engine
class ScrollBarUnstyler : public QObject {
public:
    explicit ScrollBarUnstyler(QApplication *app)
        : QObject(app)
        , m_app(app)
    {
        strip();
        app->installEventFilter(this);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const bool styleChanged = event->type() == QEvent::StyleChange;
        const bool hasScrollBarRules =
            m_app->styleSheet().contains(QLatin1String("QScrollBar"));

        if (styleChanged && !m_pending && hasScrollBarRules) {
            m_pending = true;
            QTimer::singleShot(0, this, [this]() {
                m_pending = false;
                strip();
            });
        }
        return QObject::eventFilter(watched, event);
    }

private:
    void strip()
    {
        const QString qss = m_app->styleSheet();
        const QString filtered = withoutScrollBarRules(qss);
        if (filtered != qss)
            m_app->setStyleSheet(filtered);
    }

    QApplication *m_app;
    bool m_pending = false;
};

} // namespace

void Explorer::configureApplicationAppearance(QApplication *app)
{
    Aero::registerStylesheet(app);
    new ScrollBarUnstyler(app);

    // The chrome has no opinion on what its icons degrade to.
    Aero::setIconFallbacks({QStringLiteral("system-file-manager"),
                            QStringLiteral("folder")});
    Aero::MenuButton::setPillArt(QLatin1String(Explorer::Art::CommandHover),
                                QLatin1String(Explorer::Art::CommandPress));
}
