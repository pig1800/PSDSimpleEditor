// The layer list: a plain expandable tree (top-most layer first, groups as folders), with icon
// columns on the right for "visible" and "clipping mask", and a "preview" pseudo layer at the
// bottom that shows the file's own flattened image.
#pragma once

#include "psd/PsdFile.h"

#include <QStyledItemDelegate>
#include <QTreeView>

#include <vector>

class QStandardItem;
class QStandardItemModel;

namespace tree {

enum Role { RecordRole = Qt::UserRole + 1, KindRole, EndRecordRole /* groups: their end-marker record */ };
enum Kind { KindLayer = 0, KindGroup = 1, KindPreview = 2 };
enum Column { ColName = 0, ColClip = 1, ColVisible = 2, ColumnCount = 3 };
constexpr int kPreviewRecord = -1;

struct Populated {
    QList<QStandardItem*> openGroups;  // name items of groups saved as open
    bool reorderable = true;           // every group has its end marker (else moving is disabled)
};
// Fills `model` from the file's records.
Populated populate(QStandardItemModel* model, const psd::PsdFile& file);
// The record order (file order, bottom-most first) the tree describes.
std::vector<size_t> recordOrder(const QStandardItemModel* model);
void setupView(QTreeView* view);
void setupColumns(QTreeView* view);  // after the model has its columns
QIcon folderIcon(bool open);

QString toQString(const std::u16string& s);
std::u16string fromQString(const QString& s);

// Name editor: Photoshop's 255-character limit.
class NameDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
};

// The layer list view: extended selection (the canvas shows the current, i.e. last clicked, row)
// and drag and drop of layers and groups: onto a group puts them at its top, between rows inserts
// them there. The items themselves are moved (children, expansion and selection survive).
class LayerView : public QTreeView {
    Q_OBJECT
public:
    explicit LayerView(QWidget* parent = nullptr);
    bool isMoving() const { return moving_; }  // current-row changes during a move aren't selections
    void setReorderable(bool on);
    void commitEdit();  // a rename still being typed goes into the model now

signals:
    void layersMoved();

protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void dragMoveEvent(QDragMoveEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    struct Target {
        QStandardItem* parent = nullptr;
        QStandardItem* anchor = nullptr;  // insert before it (null: at the end of `parent`)
    };
    QStandardItemModel* standardModel() const;
    QStandardItem* parentOf(QStandardItem* item) const;
    QList<QStandardItem*> movedItems() const;
    bool target(const QPoint& pos, const QList<QStandardItem*>& moved, Target& t) const;
    void moveItems(const QList<QStandardItem*>& moved, const Target& t);

    bool moving_ = false;
};

}  // namespace tree
