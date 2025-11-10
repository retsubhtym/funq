/*
Copyright: SCLE SFE
Contributor: Julien Pagès <j.parkouss@gmail.com>

This software is a computer program whose purpose is to test graphical
applications written with the QT framework (http://qt.digia.com/).

This software is governed by the CeCILL v2.1 license under French law and
abiding by the rules of distribution of free software.  You can  use,
modify and/ or redistribute the software under the terms of the CeCILL
license as circulated by CEA, CNRS and INRIA at the following URL
"http://www.cecill.info".

As a counterpart to the access to the source code and  rights to copy,
modify and redistribute granted by the license, users are provided only
with a limited warranty  and the software's author,  the holder of the
economic rights,  and the successive licensors  have only  limited
liability.

In this respect, the user's attention is drawn to the risks associated
with loading,  using,  modifying and/or developing or reproducing the
software by the user in light of its specific status of free software,
that may mean  that it is complicated to manipulate,  and  that  also
therefore means  that it is reserved for developers  and  experienced
professionals having in-depth computer knowledge. Users are therefore
encouraged to load and test the software's suitability as regards their
requirements in conditions enabling the security of their systems and/or
data to be ensured and,  more generally, to use and operate it in the
same conditions as regards security.

The fact that you are presently reading this means that you have had
knowledge of the CeCILL v2.1 license and that you accept its terms.
*/

#include "pick.h"

#include "objectpath.h"

#include <QApplication>
#include <QBackingStore>
#include <QColor>
#include <QCoreApplication>
#include <QDebug>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QList>
#include <QMetaProperty>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRegion>
#include <QResizeEvent>
#include <QRubberBand>
#include <QScopedPointer>
#include <QTextStream>
#include <QVariant>
#include <QVariantMap>
#include <QWidget>
#include <algorithm>
// #include <QQuickWindow>

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
#include <QBackingStore>
#include <QExposeEvent>
#include <QGuiApplication>
#include <QResizeEvent>
#include <QWindow>
#endif

#ifdef QT_QUICK_LIB
#include <QQuickItem>
#include <QQuickWindow>
#endif

namespace {

bool readRealProperty(QObject * object, const char * name, qreal & target) {
    QVariant value = object->property(name);
    if (!value.isValid()) {
        return false;
    }
    bool ok = false;
    qreal asDouble = value.toDouble(&ok);
    if (!ok) {
        asDouble = static_cast<qreal>(value.toInt(&ok));
    }
    if (!ok) {
        return false;
    }
    target = asDouble;
    return true;
}

#ifdef QT_QUICK_LIB
bool isPickableItem(QQuickItem * item, const QPointF & scenePos,
                    QPointF * localPoint = nullptr) {
    if (!item) {
        return false;
    }
    if (!item->isVisible() || !item->isEnabled() || item->opacity() <= 0.0) {
        if (item) {
        }
        return false;
    }
    QPointF local = item->mapFromScene(scenePos);
    if (!item->contains(local)) {
        return false;
    }
    if (localPoint) {
        *localPoint = local;
    }
    return true;
}


static inline bool isButtonLike(const QQuickItem *item) {
    if (!item) return false;

    static const char *kButtonTypes[] = {
        "QQuickAbstractButton",
        "QQuickButton",
        "QQuickToolButton",
        "QQuickCheckBox",
        "QQuickRadioButton",
        "QQuickSwitch",
        "QQuickMenuItem",
        nullptr
    };
    for (const char **t = kButtonTypes; *t; ++t) {
        if (item->inherits(*t)) return true;
    }
    const QMetaObject *mo = item->metaObject();
    if (!mo) return false;
    return QString::fromLatin1(mo->className()).contains("Button", Qt::CaseInsensitive);
}

static inline QQuickItem *deepestItemUnderPoint(QQuickItem *root, const QPointF &scenePos) {
    if (!root) return nullptr;

    QPointF rootLocal = root->mapFromScene(scenePos);
    if (!root->isVisible() || !root->isEnabled() || root->opacity() <= 0.0 || !root->contains(rootLocal))
        return nullptr;

    QQuickItem *current = root;
    for (;;) {
        QPointF local = current->mapFromScene(scenePos);
        QQuickItem *next = current->childAt(local.x(), local.y());
        if (!next) break;
        if (!next->isVisible() || !next->isEnabled() || next->opacity() <= 0.0)
            break;
        current = next;
    }
    return current;
}

static inline QQuickItem *promoteToButtonAncestor(QQuickItem *leaf) {
    for (QQuickItem *it = leaf; it; it = it->parentItem()) {
        if (isButtonLike(it))
            return it;
    }
    return leaf;
}

QQuickItem * findQuickItemAt(QQuickItem * item, const QPointF & scenePos, bool preferButtons) {
    if (!item)
        return nullptr;

    QList<QQuickItem *> children = item->childItems();
    std::sort(children.begin(), children.end(),
              [](QQuickItem * left, QQuickItem * right) {
                  if (qFuzzyCompare(left->z(), right->z()))
                      return left < right;
                  return left->z() < right->z();
              });

    QList<QQuickItem*> found;
    for (int i = children.size() - 1; i >= 0; --i) {
        QQuickItem *child = children.at(i);
        if (!child) continue;
        if (QQuickItem *hit = findQuickItemAt(child, scenePos, preferButtons))
            found.push_back(hit);
    }

    const char* env = getenv("FUNQ_MODE_PICK_LARGEST");
    const bool preferLargestArea = env && strcmp(env, "1") == 0;

    if (!found.isEmpty()) {
        const qreal kDistEps       = 0.75;
        const qreal kAreaEps       = 1.0;
        const qreal kButtonBoostPx = 4.0;

        QQuickItem *bestItem = nullptr;
        qreal bestDist = std::numeric_limits<qreal>::max();
        qreal bestArea = preferLargestArea
                         ? -std::numeric_limits<qreal>::max()
                         :  std::numeric_limits<qreal>::max();

        for (QQuickItem *candidate : found) {
            const QRectF rect = candidate->mapRectToScene(candidate->boundingRect());
            if (rect.isEmpty()) continue;

            const qreal area = rect.width() * rect.height();
            const qreal dist = QLineF(rect.center(), scenePos).length();

            bool better = false;
            const bool candIsBtn = preferButtons && isButtonLike(candidate);
            const bool bestIsBtn = preferButtons && bestItem && isButtonLike(bestItem);

            if (preferButtons && bestItem) {
                if (candIsBtn && !bestIsBtn) {
                    if (dist <= bestDist + kButtonBoostPx)
                        better = true;
                } else if (!candIsBtn && bestIsBtn) {
                    if (dist + kButtonBoostPx >= bestDist)
                        better = false;
                }
            }

            if (!better) {
                if (dist + kDistEps < bestDist)
                    better = true;
                else if (qAbs(dist - bestDist) <= kDistEps) {
                    if (preferLargestArea) {
                        if (area > bestArea + kAreaEps)
                            better = true;
                    } else {
                        if (area + kAreaEps < bestArea)
                            better = true;
                    }
                    if (!better && preferButtons) {
                        if (candIsBtn && !bestIsBtn)
                            better = true;
                    }
                }
            }

            if (better) {
                bestItem = candidate;
                bestDist = dist;
                bestArea = area;
            }
        }

        if (bestItem) {
            // ---- NEW refinement block ----
            if (preferButtons) {
                // Use precise hit-testing to refine to actual visual leaf.
                if (QQuickItem *leaf = deepestItemUnderPoint(bestItem, scenePos)) {
                    // Prefer promoting to a button ancestor if found.
                    QQuickItem *button = promoteToButtonAncestor(leaf);
                    if (button && button != bestItem)
                        bestItem = button;
                }
            }
            // ---- END refinement block ----

            return bestItem;
        }
    }

    // Fallback: current item if it’s pickable.
    QPointF localPoint;
    if (!isPickableItem(item, scenePos, &localPoint))
        return nullptr;
    return item;
}

#endif

class HighlightOverlay {
public:
    virtual ~HighlightOverlay() {}
    virtual void showRect(const QRect & globalRect) = 0;
    virtual void hide() = 0;
};

}  // namespace

#ifndef QT_NO_WIDGETS
WidgetHighlightOverlay::WidgetHighlightOverlay()
    : m_band(new QRubberBand(QRubberBand::Rectangle)) {
    m_band->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint |
                           Qt::WindowStaysOnTopHint);
    m_band->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_band->setPalette(QColor(Qt::red));
    m_band->setStyleSheet(
        "border: 2px solid #ff0000; background: rgba(255, 0, 0, 127);");
}

WidgetHighlightOverlay::~WidgetHighlightOverlay() { delete m_band; }

void WidgetHighlightOverlay::showRect(const QRect & globalRect) {
    m_band->setGeometry(globalRect);
    if (!m_band->isVisible()) {
        m_band->show();
    } else {
        m_band->raise();
    }
}

void WidgetHighlightOverlay::hide() {
    if (m_band->isVisible()) {
        m_band->hide();
    }
}
#endif  // QT_NO_WIDGETS

#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
WindowHighlightOverlay::WindowHighlightOverlay()
    : QWindow(), m_backingStore(new QBackingStore(this)) {
    setFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    // QWindow::setColor(Qt::transparent);
}

WindowHighlightOverlay::~WindowHighlightOverlay() {}

void WindowHighlightOverlay::showRect(const QRect & globalRect) {
    setGeometry(globalRect);
    if (!isVisible()) {
        show();
    } else {
        requestUpdate();
    }
    renderOverlay();
}

void WindowHighlightOverlay::hide() {
    if (isVisible()) {
        QWindow::hide();
    }
}

void WindowHighlightOverlay::exposeEvent(QExposeEvent * event) {
    QWindow::exposeEvent(event);
    renderOverlay();
}

void WindowHighlightOverlay::resizeEvent(QResizeEvent * event) {
    m_backingStore->resize(event->size());
    QWindow::resizeEvent(event);
}

void WindowHighlightOverlay::renderOverlay() {
    if (!isExposed()) {
        return;
    }
    QRect rect(QPoint(0, 0), size());
    m_backingStore->beginPaint(rect);
    QPaintDevice * device = m_backingStore->paintDevice();
    QPainter painter(device);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect, QColor(255, 0, 0, 127));
    QPen pen(QColor(255, 0, 0));
    pen.setWidth(2);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect.adjusted(1, 1, -2, -2));
    painter.end();
    m_backingStore->endPaint();
    m_backingStore->flush(rect);
}
#endif  // QT_VERSION >= 5.0.0

Pick::Pick(PickHandler * handler, QObject * parent)
    : QObject(parent),
      m_handler(handler),
      m_highlightOverlay(0),
#ifndef QT_NO_WIDGETS
      m_hasWidgetStack(false),
#endif
      m_highlightTarget(0),
      m_highlightPos(0, 0) {
#ifndef QT_NO_WIDGETS
    if (qobject_cast<QApplication *>(QCoreApplication::instance())) {
        m_hasWidgetStack = true;
        m_highlightOverlay = new WidgetHighlightOverlay();
    }
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    if (!m_highlightOverlay &&
        qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
        m_highlightOverlay = new WindowHighlightOverlay();
    }
#endif
}

Pick::~Pick() {
    if (m_handler) {
        delete m_handler;
    }
    delete m_highlightOverlay;
    m_highlightOverlay = 0;
}

bool Pick::handleEvent(QObject * receiver, QEvent * event) {
    if (!m_handler) {
        return false;
    }
    if (event->type() == QEvent::MouseMove ||
        event->type() == QEvent::MouseButtonPress ||
        event->type() == QEvent::MouseButtonRelease) {
        QMouseEvent * evt = static_cast<QMouseEvent *>(event);
        Qt::KeyboardModifiers mods = evt->modifiers();
        bool ctrlShift = (mods & Qt::ShiftModifier) &&
                         (mods & Qt::ControlModifier);
        bool highlightMode = ctrlShift;
        bool buttonsOnly = mods & Qt::AltModifier;

        QRect candidateRect;
        QObject * candidateTarget = nullptr;
        QPoint candidatePos;
        bool hasCandidate = computeHighlightTarget(evt->globalPos(), candidateRect,
                                   candidateTarget, candidatePos, buttonsOnly);

        if (highlightMode && hasCandidate) {
            showHighlight(candidateRect);
            m_highlightTarget = candidateTarget;
            m_highlightPos = candidatePos;
        } else if (highlightMode) {
            hideHighlight();
        } else {
            hideHighlight();
        }

        if (event->type() == QEvent::MouseButtonPress && ctrlShift) {
            QObject * target =
                (highlightMode && m_highlightTarget)
                    ? m_highlightTarget
                    : (hasCandidate ? candidateTarget : receiver);
            QPoint pos = (highlightMode && m_highlightTarget)
                             ? m_highlightPos
                             : (hasCandidate ? candidatePos : evt->pos());
            if (!target) {
                target = receiver;
                pos = evt->pos();
            }
            m_handler->handle(target, pos);
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && ctrlShift) {
            return true;
        }
        return false;
    }
    return false;
}

PickFormatter::PickFormatter()
    : m_stream(stdout, QIODevice::WriteOnly),
      m_showProperties(true),
      m_showGeometry(true) {
}

void print_object_props(QObject * object, QTextStream & stream) {
    for (int i = 0; i < object->metaObject()->propertyCount(); ++i) {
        QMetaProperty property = object->metaObject()->property(i);
        QString strValue = property.read(object).toString();
        if (!strValue.isEmpty()) {
            stream << "\t" << property.name() << ": " << strValue << '\n';
        }
    }
}

void PickFormatter::handle(QObject * object, const QPoint & pos) {
    m_stream << "------------------------------------------------------------------"
             << '\n';
    QString path = QString("WIDGET: `%1` (pos: %2, %3)")
                       .arg(ObjectPath::objectPath(object))
                       .arg(pos.x())
                       .arg(pos.y());
    m_stream << path << '\n';
    if (m_showProperties) {
        m_stream << "\tObject type: " << object->metaObject()->className()
                 << '\n';
        print_object_props(object, m_stream);
    }
    if (m_showGeometry) {
        QVariant geomVar = object->property("geometry");
        QRectF rect;
        if (geomVar.isValid() && geomVar.canConvert<QRect>()) {
            rect = geomVar.toRect();
        } else if (geomVar.isValid() && geomVar.canConvert<QVariantMap>()) {
            QVariantMap geomMap = geomVar.toMap();
            if (geomMap.contains("x") && geomMap.contains("y") &&
                geomMap.contains("width") && geomMap.contains("height")) {
                rect = QRectF(geomMap.value("x").toDouble(),
                              geomMap.value("y").toDouble(),
                              geomMap.value("width").toDouble(),
                              geomMap.value("height").toDouble());
            }
        } else {
            qreal x = 0;
            qreal y = 0;
            qreal width = 0;
            qreal height = 0;
            bool hasPos = readRealProperty(object, "x", x) &&
                          readRealProperty(object, "y", y);
            bool hasSize = readRealProperty(object, "width", width) &&
                           readRealProperty(object, "height", height);
            if (!hasSize) {
                hasSize = readRealProperty(object, "implicitWidth", width) &&
                          readRealProperty(object, "implicitHeight", height);
            }
            if (hasPos && hasSize && width > 0 && height > 0) {
                rect = QRectF(x, y, width, height);
            }
        }
        if (!rect.isNull()) {
            m_stream << "\tGeometry: (" << rect.x() << ", " << rect.y()
                     << ", " << rect.width() << "x" << rect.height() << ")\n";
        }
    }

    QGraphicsView * view = dynamic_cast<QGraphicsView *>(object->parent());
    if (view) {
        QGraphicsItem * item = view->itemAt(pos);
        QObject * qitem = dynamic_cast<QObject *>(item);
        if (item) {
            m_stream << "GITEM: `" << ObjectPath::graphicsItemId(item)
                     << "` (QObject: " << (qitem != 0) << ")" << '\n';
            if (m_showGeometry) {
                QRectF grect = item->sceneBoundingRect();
                m_stream << "\tScene geometry: (" << grect.x() << ", "
                         << grect.y() << ", " << grect.width() << "x"
                         << grect.height() << ")\n";
            }
            if (qitem) {
                m_stream << "\tQObject type: "
                         << qitem->metaObject()->className() << '\n';
                if (m_showProperties) {
                    print_object_props(qitem, m_stream);
                }
            }
        }
    }

    m_stream.flush();
}

void Pick::showHighlight(const QRect & globalRect) {
    if (m_highlightOverlay) {
        m_highlightOverlay->showRect(globalRect);
    }
}

void Pick::hideHighlight() {
    if (m_highlightOverlay) {
        m_highlightOverlay->hide();
    }
    m_highlightTarget = 0;
}

bool Pick::computeHighlightTarget(const QPoint & globalPos, QRect & outRect,
                                  QObject *& target, QPoint & localPos,
                                  bool buttonsOnly) const {
#ifndef QT_NO_WIDGETS
    QWidget * widget =
        m_hasWidgetStack ? QApplication::widgetAt(globalPos) : 0;
    if (widget) {
        QRect rect = widget->rect();
        QPoint topLeft = widget->mapToGlobal(rect.topLeft());
        outRect = QRect(topLeft, rect.size());
        target = widget;
        localPos = widget->mapFromGlobal(globalPos);
        return true;
    }
#endif
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    if (!QGuiApplication::instance()) {
        target = 0;
        return false;
    }
    const QList<QWindow *> windows = QGuiApplication::topLevelWindows();
    for (QWindow * window : windows) {
        if (!window || !window->isVisible()) {
            continue;
        }
        QRect rect(window->mapToGlobal(QPoint(0, 0)), window->size());
        if (!rect.contains(globalPos)) {
            continue;
        }
#ifdef QT_QUICK_LIB
        if (auto quickWindow = qobject_cast<QQuickWindow *>(window)) {
            auto windowPos = window->mapFromGlobal(globalPos);
            auto scenePos = QPointF(windowPos);
            if (auto content = quickWindow->contentItem()) {
                QQuickItem * quickItem = findQuickItemAt(content, scenePos, buttonsOnly);
                if (quickItem && quickItem != content) {
                    QPointF sceneTopLeft = quickItem->mapToScene(QPointF(0, 0));
                    QPoint globalTopLeft = window->mapToGlobal(QPoint(
                        qRound(sceneTopLeft.x()), qRound(sceneTopLeft.y())));
                    QSize size(qMax(1, qRound(quickItem->width())),
                               qMax(1, qRound(quickItem->height())));
                    outRect = QRect(globalTopLeft, size);
                    target = quickItem;
                    QPointF scenePoint = scenePos;
                    QPointF localPoint = quickItem->mapFromScene(scenePoint);
                    localPos = QPoint(static_cast<int>(localPoint.x()), static_cast<int>(localPoint.y()));
                    return true;
                }
            }
        }
#endif
        outRect = rect;
        target = window;
        localPos = globalPos - rect.topLeft();
        return true;
    }
#endif
    target = 0;
    return false;
}
