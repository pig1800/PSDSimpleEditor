#include "MainWindow.h"
#include "gpu/Gpu.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char** argv) {
    // The canvas is a native child window; keep the rest of the widget tree non-native.
    QApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PSD Simple Editor"));

    std::wstring error;
    if (!gpu::Device::get().init(error))
        QMessageBox::warning(nullptr, QStringLiteral("PSD Simple Editor"),
                             QStringLiteral("%1\n\nImages can't be shown; renaming still works.").arg(QString::fromStdWString(error)));

    MainWindow window;
    window.show();
    window.openFiles(app.arguments().mid(1));
    return app.exec();
}
