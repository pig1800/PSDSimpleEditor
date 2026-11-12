// One open file (one tab): the parsed structure, pending renames, its layer tree, and the GPU
// image of its current layer. Selecting another layer releases the old image at once; switching
// tabs keeps it (each tab holds its own).
#pragma once

#include "LayerTree.h"
#include "gpu/Gpu.h"
#include "psd/PsdFile.h"
#include "psd/PsdWriter.h"

#include <QFuture>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <atomic>
#include <climits>
#include <memory>

class QStandardItem;
class QStandardItemModel;

namespace gpu {
struct DecodeResult;
}

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject* parent = nullptr);
    ~Document() override;

    bool open(const QString& path, QString& error);
    // Saves to `target` (the file itself, or Save As). Afterwards the document is that file.
    bool save(const QString& target, QString& error);

    QString path() const { return path_; }
    QString fileName() const;
    bool isDirty() const { return !renames_.empty() || reordered_; }
    QString infoText() const;
    QString lastMessage() const { return lastMessage_; }
    QString changeSummary() const;  // e.g. "2 layers renamed, layer order changed"
    void commitPendingEdit() {
        if (view_) view_->commitEdit();
    }
    tree::LayerView* view() const { return view_; }  // owned by whoever it is added to
    gpu::DisplayState* display() { return &display_; }

signals:
    void dirtyChanged();
    void imageChanged();  // the current image is ready (or there is none to show): redraw
    void message(const QString& text);

private:
    void onCurrentChanged(const QModelIndex& index);
    void onItemChanged(QStandardItem* item);
    void onLayersMoved();
    void select(int record);
    void onDecoded(uint64_t generation, const gpu::DecodeResult& result);
    void waitForJobs();
    QString currentName(int record) const;

    void say(const QString& text);

    QString path_;
    QString lastMessage_;
    psd::PsdFile psd_;
    psd::RenameMap renames_;
    std::vector<size_t> order_;  // record order the tree describes (file order)
    bool reordered_ = false;     // order_ differs from the file's
    bool ignoreItemChanges_ = false;
    QPointer<tree::LayerView> view_;
    QStandardItemModel* model_ = nullptr;
    gpu::DisplayState display_;
    int selected_ = INT_MIN;
    uint64_t generation_ = 0;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QList<QFuture<void>> jobs_;
};
