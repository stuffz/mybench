#include "ui/handcursor.h"

#include <QAbstractItemView>
#include <QEvent>
#include <QMouseEvent>
#include <QTabBar>

namespace
{

// Function-local static: a file-scope QString with a dynamic initialiser could
// throw before main (cert-err58).
const QString &filterName()
{
    static const QString name = QStringLiteral("handCursorFilter");
    return name;
}

// One filter per target, owned by it. Both cases are the same shape: hit-test
// the pointer, hand or arrow, and drop back to the arrow on the way out.
class HandOverHits : public QObject
{
public:
    explicit HandOverHits(QWidget *target) : QObject(target), m_target(target)
    {
        setObjectName(filterName());
    }

protected:
    virtual bool hits(const QPoint &pos) const = 0;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::MouseMove)
        {
            const auto *move = static_cast<QMouseEvent *>(event);
            m_target->setCursor(
                hits(move->position().toPoint()) ? Qt::PointingHandCursor : Qt::ArrowCursor
            );
        }
        else if (event->type() == QEvent::Leave)
        {
            m_target->unsetCursor();
        }
        return QObject::eventFilter(watched, event);
    }

    QWidget *m_target;
};

class HandOverRows : public HandOverHits
{
public:
    HandOverRows(QAbstractItemView *view, QWidget *viewport) : HandOverHits(viewport), m_view(view)
    {}

protected:
    bool hits(const QPoint &pos) const override { return m_view->indexAt(pos).isValid(); }

private:
    QAbstractItemView *m_view;
};

class HandOverTabs : public HandOverHits
{
public:
    explicit HandOverTabs(QTabBar *bar) : HandOverHits(bar), m_bar(bar) {}

protected:
    bool hits(const QPoint &pos) const override { return m_bar->tabAt(pos) >= 0; }

private:
    QTabBar *m_bar;
};

// AppStyle::polish() runs over every widget again each time the theme sheet is
// reapplied, so without this a theme change would stack a second filter on the
// same target, and the next one a third.
bool alreadyWatched(QWidget *target)
{
    return target->findChild<QObject *>(filterName(), Qt::FindDirectChildrenOnly) != nullptr;
}

} // namespace

void handCursorOnRows(QAbstractItemView *view)
{
    // The viewport is what the pointer is actually over; the view itself is the
    // frame and the scrollbars around it.
    QWidget *viewport = view->viewport();
    if (alreadyWatched(viewport))
    {
        return;
    }
    viewport->setMouseTracking(true);
    viewport->installEventFilter(new HandOverRows(view, viewport));
}

void handCursorOnTabs(QTabBar *bar)
{
    if (alreadyWatched(bar))
    {
        return;
    }
    bar->setMouseTracking(true);
    bar->installEventFilter(new HandOverTabs(bar));
}
