#include "sysmonplugin.h"

#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QList>
#include <QPainter>
#include <QTimer>
#include <QTimerEvent>
#include <QtMath>

#include <algorithm>

namespace {

const int kPadLeft = 8;
const int kPadRight = 8;
const int kGap = 5;

bool readText(const QString &path, QByteArray *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    *out = f.readAll().trimmed();
    return true;
}

bool readU64(const QString &path, quint64 *out)
{
    QByteArray text;
    if (!readText(path, &text))
        return false;
    bool ok = false;
    quint64 value = text.toULongLong(&ok);
    if (!ok)
        return false;
    *out = value;
    return true;
}

QList<QByteArray> readLines(const QString &path)
{
    QList<QByteArray> lines;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return lines;
    // 注意：/proc 下的文件 size() 为 0，不能用 atEnd() 判断结尾
    QByteArray line = f.readLine();
    while (!line.isEmpty()) {
        lines.append(line);
        line = f.readLine();
    }
    return lines;
}

QString fmtRate(double bps)
{
    if (bps < 0.0)
        return QStringLiteral("--");
    if (bps >= 10.0 * 1048576.0)
        return QStringLiteral("%1 MB/s").arg(bps / 1048576.0, 0, 'f', 0);
    if (bps >= 1048576.0)
        return QStringLiteral("%1 MB/s").arg(bps / 1048576.0, 0, 'f', 1);
    if (bps >= 10240.0)
        return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 KB/s").arg(bps / 1024.0, 0, 'f', 1);
}


QString defaultRouteInterface()
{
    const QList<QByteArray> lines = readLines(QStringLiteral("/proc/net/route"));
    for (int i = 1; i < lines.size(); ++i) {
        const QList<QByteArray> fields = lines.at(i).simplified().split(' ');
        if (fields.size() < 4 || fields.at(1) != "00000000")
            continue;
        bool ok = false;
        const uint flags = fields.at(3).toUInt(&ok, 16);
        if (ok && (flags & 0x2))
            return QString::fromLatin1(fields.at(0));
    }
    return QString();
}

} // namespace

SysMonWidget::SysMonWidget(QWidget *parent)
    : QWidget(parent)
{
    mClock.start();
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    findZones();
    findHwmon();
    mTimerId = startTimer(1000);
    QTimer::singleShot(120, this, [this] {
        sample();
        update();
    });
}

QSize SysMonWidget::sizeHint() const
{
    const QFontMetrics fm(font());
    return QSize(contentWidth(fm), qMax(22, fm.height() + 8));
}

void SysMonWidget::findZones()
{
    const QDir dir(QStringLiteral("/sys/class/powercap"));
    const QStringList entries = dir.entryList(QStringList() << QStringLiteral("intel-rapl:*"),
                                              QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString base = dir.absoluteFilePath(entry);
        QByteArray name;
        if (!readText(base + QStringLiteral("/name"), &name))
            continue;
        RaplZone zone;
        zone.path = base;
        quint64 range = 0;
        if (readU64(base + QStringLiteral("/max_energy_range_uj"), &range))
            zone.maxRange = range;
        if (name == "psys" && !mPsys.valid) {
            zone.valid = true;
            mPsys = zone;
        } else if (name == "package-0" && !mPackage.valid) {
            zone.valid = true;
            mPackage = zone;
        }
    }
}

void SysMonWidget::findHwmon()
{
    const QDir dir(QStringLiteral("/sys/class/hwmon"));
    const QStringList entries = dir.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        const QString base = dir.absoluteFilePath(entry);
        QByteArray name;
        if (!readText(base + QStringLiteral("/name"), &name))
            continue;
        if (name == "coretemp" && mCoretempPath.isEmpty())
            mCoretempPath = base;
        else if (name == "acpitz" && mAcpitzPath.isEmpty())
            mAcpitzPath = base;
        else if (name == "nvme" && mNvmePath.isEmpty())
            mNvmePath = base;
        else if (name.startsWith("iwlwifi") && mWifiPath.isEmpty())
            mWifiPath = base;
    }
}

double SysMonWidget::sampleRapl(RaplZone &zone)
{
    if (!zone.valid)
        return -1.0;
    quint64 energy = 0;
    if (!readU64(zone.path + QStringLiteral("/energy_uj"), &energy))
        return zone.watts;
    const qint64 now = mClock.nsecsElapsed();
    if (zone.lastNs > 0 && now > zone.lastNs) {
        qint64 delta = static_cast<qint64>(energy) - static_cast<qint64>(zone.lastEnergy);
        if (zone.maxRange > 0 && delta < 0)
            delta += static_cast<qint64>(zone.maxRange);
        if (delta >= 0)
            zone.watts = static_cast<double>(delta) / 1e6 / (static_cast<double>(now - zone.lastNs) / 1e9);
    }
    zone.lastEnergy = energy;
    zone.lastNs = now;
    return zone.watts;
}

void SysMonWidget::readCpu()
{
    QFile f(QStringLiteral("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QList<QByteArray> parts = f.readLine().simplified().split(' ');
    if (parts.size() < 5 || parts.first() != "cpu")
        return;
    quint64 values[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    for (int i = 1; i < parts.size() && i <= 8; ++i)
        values[i - 1] = parts.at(i).toULongLong();
    quint64 total = 0;
    for (quint64 v : values)
        total += v;
    const quint64 idle = values[3] + values[4];
    if (mPrevTotal > 0 && total > mPrevTotal) {
        const quint64 totalDelta = total - mPrevTotal;
        const quint64 idleDelta = idle >= mPrevIdle ? idle - mPrevIdle : 0;
        mCpuPct = qBound(0.0, 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta)), 100.0);
    }
    mPrevTotal = total;
    mPrevIdle = idle;
}

void SysMonWidget::readMem()
{
    const QList<QByteArray> lines = readLines(QStringLiteral("/proc/meminfo"));
    quint64 totalKB = 0;
    quint64 availKB = 0;
    for (const QByteArray &line : lines) {
        if (line.startsWith("MemTotal:"))
            totalKB = line.mid(9).simplified().split(' ').first().toULongLong();
        else if (line.startsWith("MemAvailable:"))
            availKB = line.mid(13).simplified().split(' ').first().toULongLong();
    }
    if (totalKB == 0)
        return;
    const quint64 usedKB = totalKB > availKB ? totalKB - availKB : 0;
    mMemTotalGB = totalKB / 1048576.0;
    mMemUsedGB = usedKB / 1048576.0;
    mMemPct = 100.0 * static_cast<double>(usedKB) / static_cast<double>(totalKB);
}

void SysMonWidget::readNet()
{
    mIface = defaultRouteInterface();
    if (mIface.isEmpty()) {
        mRx = mTx = -1.0;
        mPrevNetOk = false;
        return;
    }

    const QList<QByteArray> lines = readLines(QStringLiteral("/proc/net/dev"));
    quint64 rx = 0;
    quint64 tx = 0;
    bool found = false;
    for (const QByteArray &line : lines) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        if (QString::fromLatin1(line.left(colon)).trimmed() != mIface)
            continue;
        const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
        if (fields.size() < 9)
            continue;
        rx = fields.at(0).toULongLong();
        tx = fields.at(8).toULongLong();
        found = true;
        break;
    }
    if (!found) {
        mRx = mTx = -1.0;
        mPrevNetOk = false;
        return;
    }

    const qint64 now = mClock.nsecsElapsed();
    if (mPrevNetOk && now > mPrevNetNs && rx >= mPrevRx && tx >= mPrevTx) {
        const double dt = static_cast<double>(now - mPrevNetNs) / 1e9;
        mRx = static_cast<double>(rx - mPrevRx) / dt;
        mTx = static_cast<double>(tx - mPrevTx) / dt;
    }
    mPrevRx = rx;
    mPrevTx = tx;
    mPrevNetNs = now;
    mPrevNetOk = true;
}

void SysMonWidget::readPower()
{
    mCpuWatts = sampleRapl(mPackage);
    const double psys = sampleRapl(mPsys);
    mWatts = psys >= 0.0 ? psys : mCpuWatts;
}

void SysMonWidget::readBattery()
{
    mBatteryPercent = -1.0;
    mBatteryCharging = false;

    QFile cap(QStringLiteral("/sys/class/power_supply/CMB1/capacity"));
    if (cap.open(QIODevice::ReadOnly)) {
        bool ok = false;
        const int value = QString::fromLatin1(cap.readAll()).trimmed().toInt(&ok);
        if (ok)
            mBatteryPercent = value;
    }

    QFile st(QStringLiteral("/sys/class/power_supply/CMB1/status"));
    if (st.open(QIODevice::ReadOnly))
        mBatteryCharging = QString::fromLatin1(st.readAll()).trimmed() == QStringLiteral("Charging");
}

void SysMonWidget::readTemps()
{
    mTemp = -1.0;

    if (!mCoretempPath.isEmpty()) {
        const QDir dir(mCoretempPath);
        const QStringList labels = dir.entryList(QStringList() << QStringLiteral("temp*_label"), QDir::Files);
        for (const QString &labelFile : labels) {
            QByteArray label;
            if (!readText(dir.absoluteFilePath(labelFile), &label))
                continue;
            if (!label.startsWith("Package"))
                continue;
            quint64 value = 0;
            QString input = labelFile;
            input.replace(QStringLiteral("_label"), QStringLiteral("_input"));
            if (readU64(dir.absoluteFilePath(input), &value)) {
                mTemp = value / 1000.0;
                break;
            }
        }
        if (mTemp < 0.0) {
            quint64 value = 0;
            if (readU64(mCoretempPath + QStringLiteral("/temp1_input"), &value))
                mTemp = value / 1000.0;
        }
    }
    if (mTemp < 0.0 && !mAcpitzPath.isEmpty()) {
        quint64 value = 0;
        if (readU64(mAcpitzPath + QStringLiteral("/temp1_input"), &value))
            mTemp = value / 1000.0;
    }

    mNvmeTemp = -1.0;
    if (!mNvmePath.isEmpty()) {
        quint64 value = 0;
        if (readU64(mNvmePath + QStringLiteral("/temp1_input"), &value))
            mNvmeTemp = value / 1000.0;
    }
    mWifiTemp = -1.0;
    if (!mWifiPath.isEmpty()) {
        quint64 value = 0;
        if (readU64(mWifiPath + QStringLiteral("/temp1_input"), &value))
            mWifiTemp = value / 1000.0;
    }
}

void SysMonWidget::sample()
{
    readCpu();
    readMem();
    readNet();
    readPower();
    readBattery();
    readTemps();
    updateTooltip();
}

QColor SysMonWidget::textColor() const
{
    QColor color = palette().color(QPalette::WindowText);
    if (!color.isValid() || color.alpha() == 0)
        color = palette().color(QPalette::Text);
    if (!color.isValid() || color.alpha() == 0) {
        const QColor bg = palette().color(QPalette::Window);
        color = bg.lightness() > 128 ? QColor(30, 30, 30) : QColor(235, 235, 235);
    }
    return color;
}

QVector<SysMonWidget::Field> SysMonWidget::buildFields() const
{
    const QColor normal = textColor();
    QColor dim = normal;
    dim.setAlpha(150);

    // 深色面板用亮色，浅色面板用深色，保证对比度
    const bool lightText = normal.lightness() > 128;
    const QColor warn = lightText ? QColor(255, 179, 71) : QColor(176, 100, 0);
    const QColor hot = lightText ? QColor(255, 95, 86) : QColor(192, 40, 40);
    const QColor net = lightText ? QColor(74, 163, 255) : QColor(20, 100, 190);
    const QColor good = lightText ? QColor(126, 217, 141) : QColor(30, 130, 60);

    auto piece = [](const QString &text, const QColor &color,
                    const QString &templ = QString(), bool right = false) {
        Piece p;
        p.text = text;
        p.color = color;
        p.templ = templ;
        p.right = right;
        return p;
    };

    QVector<Field> fields;

    // CPU：整段定宽，标签与数值之间只留一个空格
    {
        Field f;
        f.templ = QStringLiteral("CPU 100%");
        f.pieces << piece(QStringLiteral("CPU "), dim);
        if (mCpuPct >= 0.0)
            f.pieces << piece(QStringLiteral("%1%").arg(qRound(mCpuPct)),
                              mCpuPct < 70 ? normal : (mCpuPct < 90 ? warn : hot));
        else
            f.pieces << piece(QStringLiteral("--%"), dim);
        fields << f;
    }

    // 内存：同上
    {
        Field f;
        f.templ = QStringLiteral("内存 100%");
        f.pieces << piece(QStringLiteral("内存 "), dim);
        if (mMemPct >= 0.0)
            f.pieces << piece(QStringLiteral("%1%").arg(qRound(mMemPct)),
                              mMemPct < 80 ? normal : (mMemPct < 92 ? warn : hot));
        else
            f.pieces << piece(QStringLiteral("--%"), dim);
        fields << f;
    }

    // 网速
    {
        Field f;
        f.pieces << piece(QStringLiteral("↓ "), net);
        f.pieces << piece(fmtRate(mRx), net, QStringLiteral("999 KB/s"), true);
        f.pieces << piece(QStringLiteral("  ↑ "), net);
        f.pieces << piece(fmtRate(mTx), net, QStringLiteral("999 KB/s"), true);
        fields << f;
    }

    // 功耗：只按 "15 W" 预留宽度
    {
        Field f;
        f.templ = QStringLiteral("15 W");
        if (mWatts >= 0.0)
            f.pieces << piece(QStringLiteral("%1 W").arg(qMin(99, qRound(mWatts))),
                              mWatts < 20 ? good : (mWatts < 35 ? warn : hot),
                              QStringLiteral("15 W"), true);
        else
            f.pieces << piece(QStringLiteral("-- W"), dim, QStringLiteral("15 W"), true);
        fields << f;
    }

    // 温度
    {
        Field f;
        if (mTemp >= 0.0)
            f.pieces << piece(QStringLiteral("%1°C").arg(qRound(mTemp)),
                              mTemp < 60 ? normal : (mTemp < 75 ? warn : hot),
                              QStringLiteral("99°C"), true);
        else
            f.pieces << piece(QStringLiteral("--°C"), dim, QStringLiteral("99°C"), true);
        fields << f;
    }

    // 电池百分比
    {
        Field f;
        if (mBatteryPercent >= 0.0) {
            const QColor color = (mBatteryPercent <= 20.0 && !mBatteryCharging) ? hot : good;
            f.pieces << piece(QStringLiteral("%1%").arg(qRound(mBatteryPercent)),
                              color, QStringLiteral("100%"), true);
        } else {
            f.pieces << piece(QStringLiteral("--%"), dim, QStringLiteral("100%"), true);
        }
        fields << f;
    }

    return fields;
}

int SysMonWidget::contentWidth(const QFontMetrics &fm) const
{
    int width = kPadLeft + kPadRight;
    const int sep = fm.horizontalAdvance(QStringLiteral("│"));
    bool first = true;
    for (const Field &field : buildFields()) {
        if (!first)
            width += sep + 2 * kGap;
        first = false;
        if (!field.templ.isEmpty()) {
            width += fm.horizontalAdvance(field.templ);
            continue;
        }
        for (const Piece &piece : field.pieces)
            width += fm.horizontalAdvance(piece.templ.isEmpty() ? piece.text : piece.templ);
    }
    return width;
}

void SysMonWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::TextAntialiasing);

    const QFontMetrics fm(font());
    p.setFont(font());

    QColor dim = textColor();
    dim.setAlpha(150);

    const int y = (height() + fm.ascent() - fm.descent()) / 2;
    const int sepAdvance = fm.horizontalAdvance(QStringLiteral("│"));

    const QVector<Field> fields = buildFields();

    // 实际内容宽度：每个字段按当前文本算（不再用最坏情况），整行靠右对齐。
    // 这样数字后面不会为 "100%" 预留空格，面板宽度又由 sizeHint 固定，不会跳动。
    int actual = 0;
    bool firstField = true;
    for (const Field &field : fields) {
        if (!firstField)
            actual += sepAdvance + 2 * kGap;   // 别漏掉分隔符，否则内容会被推出右边界
        firstField = false;
        for (const Piece &piece : field.pieces)
            actual += fm.horizontalAdvance(piece.templ.isEmpty() ? piece.text : piece.templ);
    }

    int x = qMax(kPadLeft, width() - kPadRight - actual);
    bool first = true;
    for (const Field &field : fields) {
        if (!first) {
            p.setPen(dim);
            x += kGap;
            p.drawText(x, y, QStringLiteral("│"));
            x += sepAdvance + kGap;
        }
        first = false;
        for (const Piece &piece : field.pieces) {
            const int slot = fm.horizontalAdvance(piece.templ.isEmpty() ? piece.text : piece.templ);
            p.setPen(piece.color);
            const int offset = piece.right ? slot - fm.horizontalAdvance(piece.text) : 0;
            p.drawText(x + offset, y, piece.text);
            x += slot;
        }
    }
}

void SysMonWidget::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != mTimerId) {
        QWidget::timerEvent(event);
        return;
    }
    sample();
    update();
}

void SysMonWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::FontChange || event->type() == QEvent::PaletteChange
        || event->type() == QEvent::StyleChange) {
        updateGeometry();
        update();
    }
    QWidget::changeEvent(event);
}

void SysMonWidget::updateTooltip()
{
    QStringList lines;

    if (mCpuPct >= 0.0)
        lines << QStringLiteral("<b>CPU 使用率</b> %1%").arg(mCpuPct, 0, 'f', 1);
    else
        lines << QStringLiteral("<b>CPU 使用率</b> --");

    if (mMemPct >= 0.0) {
        lines << QStringLiteral("<b>内存</b> %1 / %2 GB（%3%）")
                     .arg(mMemUsedGB, 0, 'f', 1)
                     .arg(mMemTotalGB, 0, 'f', 1)
                     .arg(mMemPct, 0, 'f', 1);
    } else {
        lines << QStringLiteral("<b>内存</b> --");
    }

    if (mRx >= 0.0) {
        lines << QStringLiteral("<b>网络（%1）</b> ↓ %2　↑ %3")
                     .arg(mIface, fmtRate(mRx), fmtRate(mTx));
    } else {
        lines << QStringLiteral("<b>网络</b> --");
    }

    if (mWatts >= 0.0) {
        if (mCpuWatts >= 0.0)
            lines << QStringLiteral("<b>整机功耗</b> %1 W（CPU %2 W）").arg(mWatts, 0, 'f', 1).arg(mCpuWatts, 0, 'f', 1);
        else
            lines << QStringLiteral("<b>整机功耗</b> %1 W").arg(mWatts, 0, 'f', 1);
    } else {
        lines << QStringLiteral("<b>整机功耗</b> --");
    }

    if (mTemp >= 0.0)
        lines << QStringLiteral("<b>CPU 温度</b> %1 °C").arg(mTemp, 0, 'f', 1);
    else
        lines << QStringLiteral("<b>CPU 温度</b> --");

    QString extra;
    if (mNvmeTemp >= 0.0)
        extra += QStringLiteral("　NVMe %1 °C").arg(mNvmeTemp, 0, 'f', 0);
    if (mWifiTemp >= 0.0)
        extra += QStringLiteral("　WiFi %1 °C").arg(mWifiTemp, 0, 'f', 0);
    if (!extra.isEmpty())
        lines << extra.trimmed();

    setToolTip(lines.join(QStringLiteral("<br>")));
}

SysMonPlugin::SysMonPlugin(const ILXQtPanelPluginStartupInfo &startupInfo)
    : QObject()
    , ILXQtPanelPlugin(startupInfo)
{
    mWidget = new SysMonWidget;
    mWidget->setObjectName(QStringLiteral("SysMon"));
}

SysMonPlugin::~SysMonPlugin()
{
    delete mWidget;
}
