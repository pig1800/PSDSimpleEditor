#include "LayerTree.h"

#include <QApplication>
#include <QCursor>
#include <QDrag>
#include <QDropEvent>
#include <QHeaderView>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QStyle>
#include <QTreeView>

#include <algorithm>
#include <functional>
#include <vector>

namespace tree {

namespace {

constexpr int kIconColumnWidth = 22;

// Icons drawn at 2x so they stay crisp on high-DPI screens.
QPixmap canvas16() {
    QPixmap pm(32, 32);
    pm.fill(Qt::transparent);
    return pm;
}

QIcon eyeIcon() {
    QPixmap pm = canvas16();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    const QColor c = QApplication::palette().color(QPalette::Text);
    p.setPen(QPen(c, 1.2));
    QPainterPath eye;
    eye.moveTo(1.5, 8);
    eye.quadTo(8, 1.5, 14.5, 8);
    eye.quadTo(8, 14.5, 1.5, 8);
    p.drawPath(eye);
    p.setBrush(c);
    p.drawEllipse(QPointF(8, 8), 2.3, 2.3);
    p.end();
    pm.setDevicePixelRatio(2);
    return QIcon(pm);
}

// Photoshop's clipping-mask marker: a bent arrow pointing down to the base layer.
QIcon clipIcon() {
    QPixmap pm = canvas16();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    const QColor c = QApplication::palette().color(QPalette::Text);
    p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(3.5, 3.5), QPointF(9.5, 3.5));
    p.drawLine(QPointF(9.5, 3.5), QPointF(9.5, 12.5));
    p.setBrush(c);
    const QPointF head[3] = {QPointF(6.5, 10), QPointF(12.5, 10), QPointF(9.5, 14)};
    p.drawPolygon(head, 3);
    p.end();
    pm.setDevicePixelRatio(2);
    return QIcon(pm);
}

QIcon previewIcon() {
    QPixmap pm = canvas16();
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(2, 2);
    const QColor c = QApplication::palette().color(QPalette::Text);
    p.setPen(QPen(c, 1.1));
    p.drawRect(QRectF(1.5, 2.5, 13, 11));
    p.setBrush(c);
    const QPointF hills[3] = {QPointF(2.5, 12.5), QPointF(6.5, 7), QPointF(10, 12.5)};
    p.drawPolygon(hills, 3);
    const QPointF hill2[3] = {QPointF(7, 12.5), QPointF(10.5, 8), QPointF(13.5, 12.5)};
    p.drawPolygon(hill2, 3);
    p.drawEllipse(QPointF(11.5, 5.5), 1.3, 1.3);
    p.end();
    pm.setDevicePixelRatio(2);
    return QIcon(pm);
}

QList<QStandardItem*> makeRow(const QString& name, const QIcon& icon, int record, Kind kind, bool visible, bool clipped) {
    auto* nameItem = new QStandardItem(icon, name);
    nameItem->setData(record, RecordRole);
    nameItem->setData(int(kind), KindRole);
    nameItem->setToolTip(name);
    Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (kind != KindPreview) flags |= Qt::ItemIsDragEnabled;  // layers and groups can be moved...
    if (kind == KindGroup) flags |= Qt::ItemIsDropEnabled;     // ...into groups
    nameItem->setFlags(kind == KindPreview ? flags : flags | Qt::ItemIsEditable);

    auto* clipItem = new QStandardItem;
    auto* visItem = new QStandardItem;
    for (QStandardItem* it : {clipItem, visItem}) {
        it->setFlags(flags);
        it->setData(record, RecordRole);
        it->setData(int(kind), KindRole);
    }
    if (clipped) {
        clipItem->setIcon(clipIcon());
        clipItem->setToolTip(QObject::tr("Clipping mask"));
    }
    if (visible) {
        visItem->setIcon(eyeIcon());
        visItem->setToolTip(QObject::tr("Visible"));
    }
    return {nameItem, clipItem, visItem};
}

}  // namespace

QString toQString(const std::u16string& s) { return QString::fromUtf16(s.data(), qsizetype(s.size())); }

std::u16string fromQString(const QString& s) {
    return std::u16string(reinterpret_cast<const char16_t*>(s.utf16()), size_t(s.size()));
}

QIcon folderIcon(bool open) {
    return QApplication::style()->standardIcon(open ? QStyle::SP_DirOpenIcon : QStyle::SP_DirClosedIcon);
}

Populated populate(QStandardItemModel* model, const psd::PsdFile& file) {
    model->clear();
    model->setColumnCount(ColumnCount);
    model->invisibleRootItem()->setFlags(model->invisibleRootItem()->flags() | Qt::ItemIsDropEnabled);
    Populated result;
    const QIcon layerIcon = QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    // Records are stored bottom-most first; a group is [end marker] children [group record].
    // Walking top-down, a group record opens a folder and its end marker closes it.
    std::vector<QStandardItem*> parents{model->invisibleRootItem()};
    for (size_t i = file.layers.size(); i-- > 0;) {
        const psd::LayerRecord& L = file.layers[i];
        const QString name = toQString(file.displayName(i));
        switch (L.kind) {
            case psd::LayerKind::GroupOpen:
            case psd::LayerKind::GroupClosed: {
                const bool open = L.kind == psd::LayerKind::GroupOpen;
                QList<QStandardItem*> row = makeRow(name, folderIcon(open), int(i), KindGroup, !L.hidden(), L.clipped());
                parents.back()->appendRow(row);
                if (open) result.openGroups.append(row[0]);
                parents.push_back(row[0]);
                break;
            }
            case psd::LayerKind::GroupEnd:
                if (parents.size() > 1) {
                    parents.back()->setData(int(i), EndRecordRole);
                    parents.pop_back();
                } else {
                    result.reorderable = false;  // an end marker without its group
                }
                break;
            default:
                parents.back()->appendRow(makeRow(name, layerIcon, int(i), KindLayer, !L.hidden(), L.clipped()));
                break;
        }
    }
    QList<QStandardItem*> preview = makeRow(QObject::tr("preview"), previewIcon(), kPreviewRecord, KindPreview, false, false);
    QFont italic = preview[0]->font();
    italic.setItalic(true);
    preview[0]->setFont(italic);
    preview[0]->setToolTip(QObject::tr("The flattened image stored in the file"));
    model->invisibleRootItem()->appendRow(preview);
    if (parents.size() > 1) result.reorderable = false;  // a group without its end marker
    return result;
}

std::vector<size_t> recordOrder(const QStandardItemModel* model) {
    std::vector<size_t> topDown;
    std::function<void(const QStandardItem*)> walk = [&](const QStandardItem* parent) {
        for (int r = 0; r < parent->rowCount(); ++r) {
            const QStandardItem* it = parent->child(r, ColName);
            switch (it->data(KindRole).toInt()) {
                case KindLayer:
                    topDown.push_back(size_t(it->data(RecordRole).toInt()));
                    break;
                case KindGroup:  // a group is: its record, its children, its end marker
                    topDown.push_back(size_t(it->data(RecordRole).toInt()));
                    walk(it);
                    topDown.push_back(size_t(it->data(EndRecordRole).toInt()));
                    break;
                default:  // preview
                    break;
            }
        }
    };
    walk(model->invisibleRootItem());
    return std::vector<size_t>(topDown.rbegin(), topDown.rend());
}

void setupView(QTreeView* view) {
    view->setHeaderHidden(true);
    view->setUniformRowHeights(true);
    view->setSelectionBehavior(QAbstractItemView::SelectRows);
    view->setSelectionMode(QAbstractItemView::ExtendedSelection);  // Shift/Ctrl; the canvas shows the current row
    view->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    view->setExpandsOnDoubleClick(false);  // double-click renames; expand with the arrow or keys
    view->setItemDelegateForColumn(ColName, new NameDelegate(view));
    view->setAllColumnsShowFocus(true);
    view->setTextElideMode(Qt::ElideRight);
    view->setIndentation(12);  // the pane is narrow; deep groups must keep room for names
    view->setDragEnabled(true);
    view->setAcceptDrops(true);
    view->setDropIndicatorShown(true);
    view->setDragDropMode(QAbstractItemView::InternalMove);  // other drops (files) go to the window
    view->setDefaultDropAction(Qt::MoveAction);
    view->setAutoExpandDelay(700);  // hovering over a collapsed group while dragging opens it
}

void setupColumns(QTreeView* view) {
    QHeaderView* h = view->header();
    h->setStretchLastSection(false);
    h->setMinimumSectionSize(kIconColumnWidth);
    h->setSectionResizeMode(ColName, QHeaderView::Stretch);
    h->setSectionResizeMode(ColClip, QHeaderView::Fixed);
    h->setSectionResizeMode(ColVisible, QHeaderView::Fixed);
    h->resizeSection(ColClip, kIconColumnWidth);
    h->resizeSection(ColVisible, kIconColumnWidth);
}

QWidget* NameDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const {
    QWidget* w = QStyledItemDelegate::createEditor(parent, option, index);
    if (auto* edit = qobject_cast<QLineEdit*>(w)) edit->setMaxLength(255);
    return w;
}

}  // namespace tree

namespace tree {

LayerView::LayerView(QWidget* parent) : QTreeView(parent) {}

void LayerView::setReorderable(bool on) {
    setDragDropMode(on ? QAbstractItemView::InternalMove : QAbstractItemView::NoDragDrop);
}

void LayerView::commitEdit() {
    if (state() != EditingState) return;
    for (const QModelIndex& i : {currentIndex(), currentIndex().siblingAtColumn(ColName)}) {
        if (QWidget* editor = indexWidget(i)) {
            commitData(editor);
            closeEditor(editor, QAbstractItemDelegate::NoHint);
            return;
        }
    }
}

QStandardItemModel* LayerView::standardModel() const { return static_cast<QStandardItemModel*>(model()); }

QStandardItem* LayerView::parentOf(QStandardItem* item) const {
    return item->parent() ? item->parent() : standardModel()->invisibleRootItem();
}

// The selected rows that move: drag-enabled, not inside another selected group (that moves them
// along), top-most first.
QList<QStandardItem*> LayerView::movedItems() const {
    QList<QStandardItem*> selected;
    for (const QModelIndex& i : selectionModel()->selectedRows(ColName)) {
        QStandardItem* it = standardModel()->itemFromIndex(i);
        if (it && (it->flags() & Qt::ItemIsDragEnabled)) selected.append(it);
    }
    QList<QStandardItem*> moved;
    for (QStandardItem* it : selected) {
        bool inner = false;
        for (QStandardItem* p = it->parent(); p && !inner; p = p->parent()) inner = selected.contains(p);
        if (!inner) moved.append(it);
    }
    auto path = [](QStandardItem* it) {
        QList<int> rows;
        for (; it; it = it->parent()) rows.prepend(it->row());
        return rows;
    };
    std::sort(moved.begin(), moved.end(), [&](QStandardItem* a, QStandardItem* b) { return path(a) < path(b); });
    return moved;
}

bool LayerView::target(const QPoint& pos, const QList<QStandardItem*>& moved, Target& t) const {
    QStandardItem* root = standardModel()->invisibleRootItem();
    QStandardItem* preview = root->child(root->rowCount() - 1, ColName);
    const QModelIndex index = indexAt(pos).siblingAtColumn(ColName);
    QStandardItem* item = index.isValid() ? standardModel()->itemFromIndex(index) : nullptr;
    const bool group = item && item->data(KindRole).toInt() == KindGroup;
    switch (dropIndicatorPosition()) {
        case QAbstractItemView::OnItem:  // onto a group: at its top
            if (!group) return false;
            t = {item, item->child(0, ColName)};
            break;
        case QAbstractItemView::AboveItem:
            if (!item) return false;
            t = {parentOf(item), item};
            break;
        case QAbstractItemView::BelowItem:
            if (!item || item == preview) return false;  // the preview stays last
            if (group && isExpanded(index) && item->rowCount() > 0)  // the line is above its first child
                t = {item, item->child(0, ColName)};
            else
                t = {parentOf(item), parentOf(item)->child(item->row() + 1, ColName)};
            break;
        case QAbstractItemView::OnViewport:  // below everything: the bottom of the list
            t = {root, preview};
            break;
    }
    // Not into a group that is itself moving.
    for (QStandardItem* p = t.parent; p && p != root; p = p->parent())
        if (moved.contains(p)) return false;
    // Insert before the first sibling that stays; at the root, never after the preview.
    while (t.anchor && moved.contains(t.anchor)) t.anchor = t.parent->child(t.anchor->row() + 1, ColName);
    if (t.parent == root && !t.anchor) t.anchor = preview;
    return true;
}

// Our own drag: QAbstractItemView's removes the dragged rows after a move, but dropEvent() has
// already moved the very same items.
void LayerView::startDrag(Qt::DropActions) {
    const QList<QStandardItem*> moved = movedItems();
    if (moved.isEmpty()) return;
    QModelIndexList indexes;
    for (QStandardItem* it : moved) indexes << it->index();
    auto* drag = new QDrag(this);
    drag->setMimeData(model()->mimeData(indexes));  // lets the base class accept the drag
    const QRect r = visualRect(moved.first()->index());
    const QRect row(0, r.top(), viewport()->width(), r.height());
    drag->setPixmap(viewport()->grab(row));
    drag->setHotSpot(viewport()->mapFromGlobal(QCursor::pos()) - row.topLeft());
    drag->exec(Qt::MoveAction, Qt::MoveAction);
}

void LayerView::dragMoveEvent(QDragMoveEvent* e) {
    QTreeView::dragMoveEvent(e);  // drop indicator, auto-scroll, auto-expand
    Target t;
    if (e->isAccepted() && !target(e->position().toPoint(), movedItems(), t)) e->ignore();
}

void LayerView::dropEvent(QDropEvent* e) {
    if (e->source() != this) {
        e->ignore();
        return;
    }
    const QList<QStandardItem*> moved = movedItems();
    Target t;
    const bool ok = !moved.isEmpty() && target(e->position().toPoint(), moved, t);
    stopAutoScroll();
    setState(NoState);
    viewport()->update();
    if (!ok) {
        e->ignore();
        return;
    }
    e->acceptProposedAction();
    moveItems(moved, t);  // the items themselves, not copies decoded from the mime data
}

void LayerView::moveItems(const QList<QStandardItem*>& moved, const Target& t) {
    moving_ = true;
    QStandardItem* current = standardModel()->itemFromIndex(currentIndex().siblingAtColumn(ColName));
    QList<QStandardItem*> expanded;  // groups in the moved rows that are open
    std::function<void(QStandardItem*)> collect = [&](QStandardItem* it) {
        if (it->data(KindRole).toInt() == KindGroup && isExpanded(it->index())) expanded.append(it);
        for (int r = 0; r < it->rowCount(); ++r) collect(it->child(r, ColName));
    };
    for (QStandardItem* it : moved) collect(it);

    QList<QList<QStandardItem*>> rows;
    for (QStandardItem* it : moved) rows.append(parentOf(it)->takeRow(it->row()));
    int at = t.anchor ? t.anchor->row() : t.parent->rowCount();
    for (const QList<QStandardItem*>& row : rows) t.parent->insertRow(at++, row);

    for (QStandardItem* g : expanded) setExpanded(g->index(), true);
    QItemSelection selection;
    for (const QList<QStandardItem*>& row : rows) selection.select(row.first()->index(), row.last()->index());
    selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    if (current) selectionModel()->setCurrentIndex(current->index(), QItemSelectionModel::NoUpdate);
    moving_ = false;
    emit layersMoved();
}

}  // namespace tree
