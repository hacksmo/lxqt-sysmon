#include <QApplication>
#include <QDebug>
#include <QFileInfo>
#include <QPluginLoader>
#include <QTimer>

#include "sysmonplugin.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    // 用法：./testload [libsysmon.so 路径]（默认 build/libsysmon.so）
    const QString path = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                  : QStringLiteral("build/libsysmon.so");
    QPluginLoader loader(QFileInfo(path).absoluteFilePath());
    if (!loader.load()) {
        qWarning() << "load failed:" << loader.errorString();
        return 1;
    }
    qInfo() << "loaded:" << loader.metaData();

    auto *lib = qobject_cast<ILXQtPanelPluginLibrary *>(loader.instance());
    if (!lib) {
        qWarning() << "cast failed";
        return 2;
    }

    ILXQtPanelPluginStartupInfo info;
    info.lxqtPanel = nullptr;
    info.settings = nullptr;
    info.desktopFile = nullptr;

    ILXQtPanelPlugin *plugin = lib->instance(info);
    QWidget *w = plugin->widget();
    w->setWindowTitle(QStringLiteral("SysMon preview"));
    w->resize(w->sizeHint());
    w->show();
    qInfo() << "sizeHint:" << w->sizeHint();

    QTimer::singleShot(40000, &app, &QApplication::quit);
    return app.exec();
}
