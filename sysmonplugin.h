#ifndef SYSMONPLUGIN_H
#define SYSMONPLUGIN_H

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QFontMetrics>
#include <QString>
#include <QVector>
#include <QWidget>

#include <ilxqtpanelplugin.h>

class SysMonWidget : public QWidget
{
    Q_OBJECT

public:
    explicit SysMonWidget(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void timerEvent(QTimerEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct RaplZone {
        QString path;
        quint64 maxRange = 0;
        quint64 lastEnergy = 0;
        qint64 lastNs = 0;
        double watts = -1.0;
        bool valid = false;
    };

    struct Piece {
        QString text;
        QColor color;
        QString templ;      // 用于定宽的模板串（如 "100%"），保证面板宽度稳定
        bool right = false; // 数值右对齐到模板宽度
    };

    struct Field {
        QVector<Piece> pieces;
        QString templ;  // 整段定宽模板（如 "CPU 100%"、"15 W"），空则按各片段宽度累加
    };

    void findZones();
    void findHwmon();
    void sample();
    double sampleRapl(RaplZone &zone);
    void readCpu();
    void readMem();
    void readNet();
    void readPower();
    void readBattery();
    void readTemps();
    QVector<Field> buildFields() const;
    int contentWidth(const QFontMetrics &fm) const;
    QColor textColor() const;
    void updateTooltip();

    int mTimerId = 0;
    QElapsedTimer mClock;

    // CPU
    quint64 mPrevTotal = 0;
    quint64 mPrevIdle = 0;
    double mCpuPct = -1.0;

    // 内存
    double mMemPct = -1.0;
    double mMemUsedGB = 0.0;
    double mMemTotalGB = 0.0;

    // 网络
    QString mIface;
    bool mPrevNetOk = false;
    quint64 mPrevRx = 0;
    quint64 mPrevTx = 0;
    qint64 mPrevNetNs = 0;
    double mRx = -1.0;
    double mTx = -1.0;

    // 功耗 / 温度
    RaplZone mPsys;
    RaplZone mPackage;
    double mWatts = -1.0;
    double mCpuWatts = -1.0;
    QString mCoretempPath;
    QString mAcpitzPath;
    QString mNvmePath;
    QString mWifiPath;
    double mTemp = -1.0;
    double mNvmeTemp = -1.0;
    double mWifiTemp = -1.0;

    // 电池
    double mBatteryPercent = -1.0;
    bool mBatteryCharging = false;
};

class SysMonPlugin : public QObject, public ILXQtPanelPlugin
{
    Q_OBJECT

public:
    explicit SysMonPlugin(const ILXQtPanelPluginStartupInfo &startupInfo);
    ~SysMonPlugin() override;

    QWidget *widget() override { return mWidget; }
    QString themeId() const override { return QStringLiteral("sysmon"); }
    Flags flags() const override { return PreferRightAlignment; }
    bool isExpandable() const override { return false; }

private:
    SysMonWidget *mWidget = nullptr;
};

class SysMonPluginLibrary : public QObject, public ILXQtPanelPluginLibrary
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "lxqt.org/Panel/PluginInterface/3.0")
    Q_INTERFACES(ILXQtPanelPluginLibrary)

public:
    ILXQtPanelPlugin *instance(const ILXQtPanelPluginStartupInfo &startupInfo) const override
    {
        return new SysMonPlugin(startupInfo);
    }
};

#endif // SYSMONPLUGIN_H
