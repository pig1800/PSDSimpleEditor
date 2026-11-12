// Tabs on top; below, a splitter with the D3D12 canvas (left, 400 px) and the current tab's
// layer tree (right, 200 px; keeps its width when the window is resized).
#pragma once

#include <QMainWindow>

class Canvas;
class Document;
class QAction;
class QLabel;
class QScrollBar;
class QSplitter;
class QStackedWidget;
class QTabBar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    void openFiles(const QStringList& paths);

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    Document* current() const;
    int indexOf(const Document* doc) const;
    void openDialog();
    bool save(Document* doc);
    bool saveAs(Document* doc);
    bool maybeSave(Document* doc);  // false: the user cancelled
    bool closeTab(int index);
    void onTabChanged(int index);
    void updateTab(Document* doc);
    void updateActions();
    void setFit(bool width);

    QTabBar* tabs_ = nullptr;
    QSplitter* splitter_ = nullptr;
    Canvas* canvas_ = nullptr;
    QScrollBar* scroll_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QLabel* info_ = nullptr;
    QAction* save_ = nullptr;
    QAction* saveAs_ = nullptr;
    QAction* close_ = nullptr;
    QAction* fitWhole_ = nullptr;
    QAction* fitWidth_ = nullptr;
    QList<Document*> docs_;  // tab order
};
