#include <QApplication>
#include <QDebug>
#include <QPluginLoader>
#include <QTimer>

#include "sysmonplugin.h"

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QPluginLoader loader(QStringLiteral("/home/hackmo/.local/src/lxqt-sysmon/build/libsysmon.so"));
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
