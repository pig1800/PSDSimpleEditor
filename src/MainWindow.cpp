#include "MainWindow.h"

#include "Canvas.h"
#include "Document.h"
#include "gpu/Gpu.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QScrollBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabBar>
#include <QTreeView>
#include <QVBoxLayout>

namespace {
const char* const kFilter = "Photoshop files (*.psd *.psb);;All files (*)";
constexpr int kCanvasWidth = 400;
constexpr int kTreeWidth = 200;
}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(tr("PSD Simple Editor"));
    setAcceptDrops(true);

    auto* central = new QWidget;
    auto* column = new QVBoxLayout(central);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);

    tabs_ = new QTabBar;
    tabs_->setTabsClosable(true);
    tabs_->setMovable(true);
    tabs_->setDocumentMode(true);
    tabs_->setExpanding(false);
    tabs_->setElideMode(Qt::ElideMiddle);
    tabs_->setUsesScrollButtons(true);
    tabs_->hide();
    column->addWidget(tabs_);

    auto* left = new QWidget;
    auto* row = new QHBoxLayout(left);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    canvas_ = new Canvas;
    scroll_ = new QScrollBar(Qt::Vertical);
    scroll_->hide();
    row->addWidget(canvas_, 1);
    row->addWidget(scroll_);

    stack_ = new QStackedWidget;
    auto* hint = new QLabel(tr("Open (Ctrl+O) or drop PSD / PSB files here"));
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    hint->setEnabled(false);
    stack_->addWidget(hint);

    splitter_ = new QSplitter(Qt::Horizontal);
    splitter_->addWidget(left);
    splitter_->addWidget(stack_);
    splitter_->setStretchFactor(0, 1);  // window resizes go to the canvas
    splitter_->setStretchFactor(1, 0);
    splitter_->setCollapsible(0, false);
    splitter_->setCollapsible(1, false);
    splitter_->setSizes({kCanvasWidth, kTreeWidth});
    column->addWidget(splitter_, 1);
    setCentralWidget(central);

    info_ = new QLabel;
    statusBar()->addPermanentWidget(info_);

    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&Open..."), QKeySequence::Open, this, &MainWindow::openDialog);
    save_ = file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(current()); });
    saveAs_ = file->addAction(tr("Save &As..."), QKeySequence::SaveAs, this, [this] { saveAs(current()); });
    close_ = file->addAction(tr("&Close Tab"), QKeySequence(Qt::CTRL | Qt::Key_W), this, [this] { closeTab(tabs_->currentIndex()); });
    file->addSeparator();
    file->addAction(tr("E&xit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    auto* fitGroup = new QActionGroup(this);
    fitWhole_ = view->addAction(tr("Fit &Whole Image"), QKeySequence(Qt::CTRL | Qt::Key_1), this, [this] { setFit(false); });
    fitWidth_ = view->addAction(tr("Fit Wi&dth (scroll)"), QKeySequence(Qt::CTRL | Qt::Key_2), this, [this] { setFit(true); });
    for (QAction* a : {fitWhole_, fitWidth_}) {
        a->setCheckable(true);
        fitGroup->addAction(a);
    }
    fitWhole_->setChecked(true);
    view->addSeparator();
    view->addAction(tr("&Next Tab"), QKeySequence(Qt::CTRL | Qt::Key_Tab), this, [this] {
        if (tabs_->count() > 1) tabs_->setCurrentIndex((tabs_->currentIndex() + 1) % tabs_->count());
    });
    view->addAction(tr("&Previous Tab"), QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab), this, [this] {
        if (tabs_->count() > 1) tabs_->setCurrentIndex((tabs_->currentIndex() + tabs_->count() - 1) % tabs_->count());
    });

    connect(tabs_, &QTabBar::currentChanged, this, &MainWindow::onTabChanged);
    connect(tabs_, &QTabBar::tabCloseRequested, this, [this](int i) { closeTab(i); });
    connect(canvas_, &Canvas::scrollChanged, this, [this](int maximum, int page, int value) {
        const QSignalBlocker block(scroll_);
        scroll_->setRange(0, maximum);
        scroll_->setPageStep(page);
        scroll_->setSingleStep(std::max(page / 8, 16));
        scroll_->setValue(value);
        scroll_->setEnabled(maximum > 0);
    });
    connect(scroll_, &QScrollBar::valueChanged, canvas_, &Canvas::scrollTo);
    connect(canvas_, &Canvas::failed, this, [this](const QString& m) { statusBar()->showMessage(m); });

    if (!gpu::Device::get().ok()) statusBar()->showMessage(tr("No D3D12 device: images can't be shown (renaming works)"));
    resize(kCanvasWidth + kTreeWidth + splitter_->handleWidth(), 800);
    updateActions();
}

Document* MainWindow::current() const {
    const int i = tabs_->currentIndex();
    return i < 0 ? nullptr : static_cast<Document*>(tabs_->tabData(i).value<void*>());
}

int MainWindow::indexOf(const Document* doc) const {
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->tabData(i).value<void*>() == doc) return i;
    return -1;
}

void MainWindow::openDialog() {
    openFiles(QFileDialog::getOpenFileNames(this, tr("Open"), QString(), tr(kFilter)));
}

void MainWindow::openFiles(const QStringList& paths) {
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        if (!fi.isFile()) continue;
        bool already = false;
        for (Document* d : docs_) {
            if (QFileInfo(d->path()) == fi) {
                tabs_->setCurrentIndex(indexOf(d));
                already = true;
                break;
            }
        }
        if (already) continue;

        auto* doc = new Document(this);
        connect(doc, &Document::dirtyChanged, this, [this, doc] {
            updateTab(doc);
            updateActions();
        });
        connect(doc, &Document::imageChanged, this, [this, doc] {
            if (doc == current()) canvas_->redraw();
        });
        connect(doc, &Document::message, this, [this, doc](const QString& m) {
            if (doc == current()) statusBar()->showMessage(m);
        });
        QString error;
        if (!doc->open(fi.absoluteFilePath(), error)) {
            QMessageBox::warning(this, tr("Open"), tr("Cannot open %1:\n%2").arg(fi.fileName(), error));
            delete doc;
            continue;
        }
        docs_.append(doc);
        stack_->addWidget(doc->view());
        tabs_->show();
        const int i = tabs_->addTab(doc->fileName());
        tabs_->setTabData(i, QVariant::fromValue<void*>(doc));
        updateTab(doc);
        if (tabs_->currentIndex() == i) onTabChanged(i);  // the first tab: currentChanged came before the data
        else tabs_->setCurrentIndex(i);
    }
}

void MainWindow::onTabChanged(int) {
    Document* doc = current();
    if (doc) {
        stack_->setCurrentWidget(doc->view());
        canvas_->setDisplay(doc->display());
        info_->setText(doc->infoText());
        statusBar()->showMessage(doc->lastMessage());
        setWindowTitle(tr("%1 - PSD Simple Editor").arg(doc->fileName()));
        doc->view()->setFocus();
    } else {
        stack_->setCurrentIndex(0);
        canvas_->setDisplay(nullptr);
        info_->clear();
        statusBar()->clearMessage();
        setWindowTitle(tr("PSD Simple Editor"));
    }
    canvas_->redraw();
    updateActions();
}

void MainWindow::updateTab(Document* doc) {
    const int i = indexOf(doc);
    if (i < 0) return;
    tabs_->setTabText(i, doc->isDirty() ? doc->fileName() + QStringLiteral(" *") : doc->fileName());
    tabs_->setTabToolTip(i, doc->path());
}

void MainWindow::updateActions() {
    Document* doc = current();
    save_->setEnabled(doc != nullptr);  // also while a rename is still being typed (save commits it)
    saveAs_->setEnabled(doc != nullptr);
    close_->setEnabled(doc != nullptr);
}

void MainWindow::setFit(bool width) {
    scroll_->setVisible(width);
    canvas_->setFit(width ? Canvas::Fit::Width : Canvas::Fit::Whole);
    canvas_->redraw();  // the scroll bar changed the canvas width (or the mode)
}

bool MainWindow::save(Document* doc) {
    if (!doc) return false;
    doc->commitPendingEdit();
    if (!doc->isDirty()) {
        statusBar()->showMessage(tr("No changes to save"), 3000);
        return true;
    }
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = doc->save(doc->path(), error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::critical(this, tr("Save"), error);
        return false;
    }
    statusBar()->showMessage(tr("Saved %1").arg(doc->fileName()), 5000);
    return true;
}

bool MainWindow::saveAs(Document* doc) {
    if (!doc) return false;
    doc->commitPendingEdit();
    const QString target = QFileDialog::getSaveFileName(this, tr("Save As"), doc->path(), tr(kFilter));
    if (target.isEmpty()) return false;
    for (Document* d : docs_) {
        if (d != doc && QFileInfo(d->path()) == QFileInfo(target)) {
            QMessageBox::warning(this, tr("Save As"), tr("%1 is open in another tab.").arg(QFileInfo(target).fileName()));
            return false;
        }
    }
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = doc->save(target, error);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::critical(this, tr("Save As"), error);
        return false;
    }
    updateTab(doc);
    onTabChanged(tabs_->currentIndex());
    statusBar()->showMessage(tr("Saved %1").arg(doc->fileName()), 5000);
    return true;
}

// Before a tab or the app closes: false if the user cancels (or saving fails).
bool MainWindow::maybeSave(Document* doc) {
    if (!doc) return true;
    doc->commitPendingEdit();
    if (!doc->isDirty()) return true;
    tabs_->setCurrentIndex(indexOf(doc));
    QMessageBox box(QMessageBox::Warning, tr("Unsaved changes"),
                    tr("%1 has unsaved changes. Save them?").arg(doc->fileName()),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setInformativeText(doc->changeSummary() + QLatin1Char('.'));
    box.setDefaultButton(QMessageBox::Save);
    box.setEscapeButton(QMessageBox::Cancel);
    const int answer = box.exec();
    if (answer == QMessageBox::Save) return save(doc);
    return answer == QMessageBox::Discard;
}

bool MainWindow::closeTab(int index) {
    if (index < 0) return false;
    auto* doc = static_cast<Document*>(tabs_->tabData(index).value<void*>());
    if (!maybeSave(doc)) return false;
    docs_.removeOne(doc);
    canvas_->setDisplay(nullptr);
    tabs_->removeTab(index);  // shows the next tab (or none)
    if (tabs_->count() == 0) tabs_->hide();
    delete doc;  // releases its GPU image; its tree view leaves the stack
    onTabChanged(tabs_->currentIndex());
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    for (int i = 0; i < tabs_->count(); ++i) {
        if (!maybeSave(static_cast<Document*>(tabs_->tabData(i).value<void*>()))) {
            e->ignore();
            return;
        }
    }
    canvas_->setDisplay(nullptr);
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& url : e->mimeData()->urls())
        if (url.isLocalFile()) paths << url.toLocalFile();
    openFiles(paths);
    e->acceptProposedAction();
}
