#include "Document.h"

#include "LayerTree.h"
#include "gpu/LayerDecoder.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardItemModel>
#include <QTimer>
#include <QTreeView>
#include <QtConcurrent/QtConcurrentRun>

#include <functional>

namespace {

QString modeName(psd::ColorMode m) {
    switch (m) {
        case psd::ColorMode::Bitmap: return QStringLiteral("Bitmap");
        case psd::ColorMode::Grayscale: return QStringLiteral("Grayscale");
        case psd::ColorMode::Indexed: return QStringLiteral("Indexed");
        case psd::ColorMode::RGB: return QStringLiteral("RGB");
        case psd::ColorMode::CMYK: return QStringLiteral("CMYK");
        case psd::ColorMode::Multichannel: return QStringLiteral("Multichannel");
        case psd::ColorMode::Duotone: return QStringLiteral("Duotone");
        case psd::ColorMode::Lab: return QStringLiteral("Lab");
    }
    return QStringLiteral("mode %1").arg(int(m));
}

void forEachItem(QStandardItem* parent, const std::function<void(QStandardItem*)>& f) {
    for (int r = 0; r < parent->rowCount(); ++r) {
        QStandardItem* it = parent->child(r, tree::ColName);
        f(it);
        forEachItem(it, f);
    }
}

// Every item of every row (all columns), recursively.
void forEachRow(QStandardItem* parent, const std::function<void(QStandardItem*)>& f) {
    for (int r = 0; r < parent->rowCount(); ++r) {
        for (int c = 0; c < parent->columnCount(); ++c)
            if (QStandardItem* it = parent->child(r, c)) f(it);
        forEachRow(parent->child(r, tree::ColName), f);
    }
}

}  // namespace

Document::Document(QObject* parent) : QObject(parent) {
    model_ = new QStandardItemModel(this);
    view_ = new tree::LayerView;  // parentless until the window adds it
    tree::setupView(view_);
    view_->setModel(model_);
    connect(view_->selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& current, const QModelIndex&) { onCurrentChanged(current); });
    connect(model_, &QStandardItemModel::itemChanged, this, &Document::onItemChanged);
    connect(view_, &tree::LayerView::layersMoved, this, &Document::onLayersMoved);
    auto folder = [this](const QModelIndex& index, bool open) {
        QStandardItem* it = model_->itemFromIndex(index.siblingAtColumn(tree::ColName));
        if (it && it->data(tree::KindRole).toInt() == tree::KindGroup) it->setIcon(tree::folderIcon(open));
    };
    connect(view_, &QTreeView::expanded, this, [folder](const QModelIndex& i) { folder(i, true); });
    connect(view_, &QTreeView::collapsed, this, [folder](const QModelIndex& i) { folder(i, false); });
}

Document::~Document() {
    if (cancel_) cancel_->store(true);
    waitForJobs();
    display_.release();
    delete view_.data();
}

void Document::waitForJobs() {
    for (QFuture<void>& f : jobs_) f.waitForFinished();
    jobs_.clear();
}

bool Document::open(const QString& path, QString& error) {
    const QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    std::wstring e;
    if (!psd_.open(native.toStdWString(), e)) {
        error = QString::fromStdWString(e);
        return false;
    }
    path_ = native;
    const tree::Populated populated = tree::populate(model_, psd_);
    tree::setupColumns(view_);
    for (QStandardItem* it : populated.openGroups) view_->setExpanded(it->index(), true);
    view_->setReorderable(populated.reorderable);
    // Start on the file's own flattened image, with the list showing its top-most layers.
    view_->setCurrentIndex(model_->index(model_->rowCount() - 1, tree::ColName));
    if (!populated.reorderable) say(tr("The group structure of this file is incomplete: moving layers is disabled"));
    QTimer::singleShot(0, view_.data(), [v = view_.data()] { v->scrollToTop(); });
    return true;
}

void Document::say(const QString& text) {
    lastMessage_ = text;
    emit message(text);
}

QString Document::fileName() const { return QFileInfo(path_).fileName(); }

QString Document::infoText() const {
    int layers = 0;
    for (const psd::LayerRecord& L : psd_.layers) layers += L.kind != psd::LayerKind::GroupEnd;
    return tr("%1 × %2 · %3 %4-bit · %5 · %6 layers")
        .arg(psd_.width)
        .arg(psd_.height)
        .arg(modeName(psd_.mode))
        .arg(psd_.depth)
        .arg(psd_.psb ? QStringLiteral("PSB") : QStringLiteral("PSD"))
        .arg(layers);
}

QString Document::changeSummary() const {
    QStringList parts;
    if (renames_.size() == 1) parts << tr("1 layer renamed");
    else if (!renames_.empty()) parts << tr("%1 layers renamed").arg(renames_.size());
    if (reordered_) parts << tr("layers moved");
    return parts.join(QStringLiteral(", "));
}

QString Document::currentName(int record) const {
    const auto it = renames_.find(size_t(record));
    return tree::toQString(it != renames_.end() ? it->second : psd_.displayName(size_t(record)));
}

void Document::onCurrentChanged(const QModelIndex& index) {
    if (view_->isMoving()) return;  // rows passing through during a drag and drop move
    if (!index.isValid() || index.data(tree::KindRole).toInt() == tree::KindGroup) return;  // groups: nothing
    select(index.data(tree::RecordRole).toInt());
}

void Document::select(int record) {
    if (record == selected_) return;
    selected_ = record;
    if (cancel_) cancel_->store(true);
    display_.release();  // the previous image goes right away
    ++generation_;

    gpu::DecodeSource src;
    std::wstring problem;
    switch (gpu::makeSource(psd_, record, src, problem)) {
        case gpu::SourceStatus::Empty:
            say(tr("Empty layer (no pixels)"));
            emit imageChanged();
            return;
        case gpu::SourceStatus::Unsupported:
            say(QString::fromStdWString(problem));
            emit imageChanged();
            return;
        case gpu::SourceStatus::Ok:
            break;
    }
    QString info;
    if (record == tree::kPreviewRecord) {
        info = tr("preview: %1 × %2").arg(psd_.width).arg(psd_.height);
    } else {
        const psd::LayerRecord& L = psd_.layers[size_t(record)];
        info = tr("%1 × %2 at (%3, %4)").arg(L.width()).arg(L.height()).arg(L.left).arg(L.top);
    }
    say(info);

    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    const uint64_t generation = generation_;
    jobs_.removeIf([](const QFuture<void>& f) { return f.isFinished(); });
    jobs_.append(QtConcurrent::run([this, src = std::move(src), cancel, generation] {
        gpu::DecodeResult result;
        gpu::decode(src, *cancel, result);
        QMetaObject::invokeMethod(this, [this, generation, result] { onDecoded(generation, result); }, Qt::QueuedConnection);
    }));
}

void Document::onDecoded(uint64_t generation, const gpu::DecodeResult& result) {
    if (generation != generation_ || result.canceled) return;  // superseded: its image dies with `result`
    if (!result.error.empty()) say(QString::fromStdWString(result.error));
    else if (!result.warning.empty()) say(QString::fromStdWString(result.warning));
    display_.release();
    display_.image = result.image;
    emit imageChanged();
}

void Document::onLayersMoved() {
    order_ = tree::recordOrder(model_);
    reordered_ = false;
    for (size_t j = 0; j < order_.size() && !reordered_; ++j) reordered_ = order_[j] != j;
    emit dirtyChanged();
}

void Document::onItemChanged(QStandardItem* item) {
    if (ignoreItemChanges_) return;
    if (item->column() != tree::ColName || item->data(tree::KindRole).toInt() == tree::KindPreview) return;
    const int record = item->data(tree::RecordRole).toInt();
    const QString text = item->text();
    const QString current = currentName(record);
    if (text == current) return;  // icon/font change, or no actual rename
    if (text.isEmpty()) {         // an empty name is refused, as in Photoshop
        item->setText(current);
        return;
    }
    const std::u16string name = tree::fromQString(text);
    const bool renamed = name != psd_.displayName(size_t(record));
    if (renamed) renames_[size_t(record)] = name;
    else renames_.erase(size_t(record));
    QFont font = item->font();
    font.setBold(renamed);
    item->setFont(font);
    item->setToolTip(text);
    emit dirtyChanged();
}

bool Document::save(const QString& target, QString& error) {
    const QString native = QDir::toNativeSeparators(QFileInfo(target).absoluteFilePath());
    const bool inPlace = QFileInfo(native) == QFileInfo(path_);
    if (inPlace && !isDirty()) return true;
    waitForJobs();  // nothing reads the file while it is replaced
    std::wstring e;
    psd::Edits edits;
    edits.renames = renames_;
    if (reordered_) edits.order = order_;
    if (!psd::saveEdited(psd_, edits, native.toStdWString(), e)) {
        error = QString::fromStdWString(e);
        return false;
    }
    psd::PsdFile reparsed;
    if (!reparsed.open(native.toStdWString(), e)) {
        error = tr("Saved, but reopening the file failed: %1").arg(QString::fromStdWString(e));
        return false;
    }
    psd_ = std::move(reparsed);
    path_ = native;
    renames_.clear();
    ignoreItemChanges_ = true;
    if (reordered_) {  // the records now sit where the tree put them: renumber the items
        std::vector<size_t> newIndex(order_.size());
        for (size_t j = 0; j < order_.size(); ++j) newIndex[order_[j]] = j;
        forEachRow(model_->invisibleRootItem(), [&](QStandardItem* it) {
            const int record = it->data(tree::RecordRole).toInt();
            if (record >= 0) it->setData(int(newIndex[size_t(record)]), tree::RecordRole);
            if (it->column() == tree::ColName && it->data(tree::KindRole).toInt() == tree::KindGroup)
                it->setData(int(newIndex[size_t(it->data(tree::EndRecordRole).toInt())]), tree::EndRecordRole);
        });
        if (selected_ >= 0) selected_ = int(newIndex[size_t(selected_)]);
        reordered_ = false;
        order_.clear();
    }
    forEachItem(model_->invisibleRootItem(), [](QStandardItem* it) {
        if (it->font().bold()) {
            QFont f = it->font();
            f.setBold(false);
            it->setFont(f);
        }
    });
    ignoreItemChanges_ = false;
    emit dirtyChanged();
    return true;
}
