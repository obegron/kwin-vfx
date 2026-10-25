// SPDX-License-Identifier: GPL-2.0-or-later
// Controls either an explicitly selected private preview or the desktop KWin bus.
#include <QApplication>
#include <QActionGroup>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QStandardPaths>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QCursor>
#include <QDebug>
#include <cmath>
#include <stdexcept>
#include <cstdio>
#include <array>
#include <utility>

class EnhancementTray : public QObject
{
public:
    EnhancementTray(QDBusConnection connection, QString configuration, bool desktop)
        : bus(connection), configPath(std::move(configuration)), desktop(desktop), tray(this), timer(this)
    {
        header = menu.addAction("Connecting to preview…"); header->setEnabled(false);
        menu.addSeparator();
        load = menu.addAction("Enable Window Enhancement", [this] {
            QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
            QDBusReply<bool> reply = effect.call("loadEffect", "kwin_vfx");
            if (!reply.isValid() || !reply.value())
                tray.showMessage("Could not load effect", reply.isValid() ? "KWin could not load Window Enhancement." : reply.error().message(), QSystemTrayIcon::Warning);
            refresh();
        });
        windowMenu = menu.addMenu("Windows — check to enhance");
        emptyWindows = windowMenu->addAction("No application windows"); emptyWindows->setEnabled(false);
        QObject::connect(windowMenu, &QMenu::aboutToShow, this, [this] { refresh(); });
        menu.addAction("Disable all windows", [this] {
            QDBusInterface control("org.kde.KWin", "/WindowEnhancement", "org.kde.KWin.WindowEnhancement", bus);
            control.call("disableAllWindows"); refresh();
        });
        enable = menu.addAction("Resume checked windows", [this] { setStrength(lastNonzero); });
        disable = menu.addAction("Pause all enhancements", [this] { setStrength(0); });
        hdr = menu.addAction("TrueHDR — SDR to HDR");
        hdr->setCheckable(true);
        QObject::connect(hdr, &QAction::triggered, this, [this](bool enabled) {
            QSettings config(configPath, QSettings::IniFormat);
            config.setValue("Effect-kwin_vfx/TrueHdr", enabled); config.sync();
            QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
            effect.call("reconfigureEffect", "kwin_vfx");
            QTimer::singleShot(100, this, [this] { refresh(); });
        });
        sdrHdr = menu.addAction("TrueHDR look — SDR tone mapping");
        sdrHdr->setCheckable(true);
        QObject::connect(sdrHdr, &QAction::triggered, this, [this](bool enabled) {
            QSettings config(configPath, QSettings::IniFormat);
            config.setValue("Effect-kwin_vfx/TrueHdrSdr", enabled); config.sync();
            QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
            effect.call("reconfigureEffect", "kwin_vfx");
            QTimer::singleShot(100, this, [this] { refresh(); });
        });
        auto *modeMenu = menu.addMenu("Enhancement mode");
        auto *modeGroup = new QActionGroup(this);
        for (const auto &[label, mode] : std::array<std::pair<const char *, int>, 3>{{
                 {"Video denoise — Low", 8}, {"Deblur — Low", 12}, {"CAS only — GPU shader", -1}}}) {
            auto *action = modeMenu->addAction(QString::fromUtf8(label));
            action->setCheckable(true); action->setData(mode); modeGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, mode] { setMode(mode); });
            modes.append(action);
        }
        auto *sharpenMenu = menu.addMenu("CAS sharpening");
        auto *sharpenGroup = new QActionGroup(this);
        for (int percent : {0, 25, 50, 75, 100}) {
            auto *action = sharpenMenu->addAction(percent == 0 ? "Off" : QString::number(percent) + "%");
            action->setCheckable(true); action->setData(percent); sharpenGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, percent] {
                QSettings config(configPath, QSettings::IniFormat);
                config.setValue("Effect-kwin_vfx/CasStrength", percent / 100.0); config.sync();
                QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
                effect.call("reconfigureEffect", "kwin_vfx");
                QTimer::singleShot(100, this, [this] { refresh(); });
            });
            sharpens.append(action);
        }
        auto *grainMenu = menu.addMenu("Film grain");
        auto *grainGroup = new QActionGroup(this);
        for (int percent : {0, 25, 50, 100}) {
            const QString label = percent == 0 ? "Off" : percent == 25 ? "Subtle (25%)" : percent == 50 ? "Medium (50%)" : "Strong (100%)";
            auto *action = grainMenu->addAction(label);
            action->setCheckable(true); action->setData(percent); grainGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, percent] {
                QSettings config(configPath, QSettings::IniFormat);
                config.setValue("Effect-kwin_vfx/FilmGrain", percent / 100.0); config.sync();
                QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
                effect.call("reconfigureEffect", "kwin_vfx");
                QTimer::singleShot(100, this, [this] { refresh(); });
            });
            grains.append(action);
        }
        auto *strength = menu.addMenu("Strength");
        auto *group = new QActionGroup(this);
        for (int percent = 0; percent <= 100; percent += 10) {
            auto *action = strength->addAction(QString::number(percent) + "%");
            action->setCheckable(true); action->setData(percent); group->addAction(action);
            QObject::connect(action, &QAction::triggered, this, [this, percent] { setStrength(percent); });
            strengths.append(action);
        }
        menu.addSeparator();
        auto *layout = menu.addAction("Shortcut: Windows + Ctrl + E (active layout)"); layout->setEnabled(false);
        menu.addAction("Unload Window Enhancement", [this] {
            QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
            effect.call("unloadEffect", "kwin_vfx"); refresh();
        });
        menu.addAction("Disable all and quit tray", [this] {
            QDBusInterface control("org.kde.KWin", "/WindowEnhancement", "org.kde.KWin.WindowEnhancement", bus);
            control.call("disableAllWindows"); qApp->quit();
        });
        tray.setContextMenu(&menu);
        QObject::connect(&tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) menu.popup(QCursor::pos());
        });
        QObject::connect(&timer, &QTimer::timeout, this, [this] { refresh(); });
        timer.start(250); refresh(); tray.show();
        tray.showMessage("Window enhancement controls", "Check windows in the Windows menu to enhance them. Mode, HDR, CAS, grain and strength apply to all checked windows.", QSystemTrayIcon::Information, 5000);
    }
private:
    QDBusConnection bus;
    QString configPath;
    bool desktop = false;
    QMenu *windowMenu = nullptr;
    QAction *emptyWindows = nullptr, *load = nullptr;
    QMap<QString, QAction *> windows;
    QSystemTrayIcon tray;
    QMenu menu;
    QTimer timer;
    QAction *header = nullptr, *enable = nullptr, *disable = nullptr;
    QAction *hdr = nullptr;
    QAction *sdrHdr = nullptr;
    QList<QAction *> strengths;
    QList<QAction *> modes;
    QList<QAction *> sharpens;
    QList<QAction *> grains;
    int lastNonzero = 60;
    bool wasEnabled = false, seenOutput = false, closing = false;
    double baselineDraws = 0;
    int previousMode = -1;
    QString previousStatus;

    QJsonObject state()
    {
        QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
        effect.setTimeout(1500);
        QDBusReply<QString> reply = effect.call("debug", "kwin_vfx", "");
        if (!reply.isValid()) throw std::runtime_error(reply.error().message().toStdString());
        const auto doc = QJsonDocument::fromJson(reply.value().toUtf8());
        if (!doc.isObject()) throw std::runtime_error("Invalid preview status");
        return doc.object();
    }
    void display(const QString &status, const QColor &color, const QString &detail)
    {
        header->setText(status);
        tray.setToolTip((desktop ? "Window enhancement\n" : "Window enhancement preview\n") + status + "\n" + detail);
        if (previousStatus == status) return;
        previousStatus = status;
        QPixmap image(64, 64); image.fill(Qt::transparent);
        QPainter painter(&image); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen); painter.setBrush(color); painter.drawRoundedRect(2, 2, 60, 60, 13, 13);
        painter.setPen(Qt::white); painter.setFont(QFont("sans-serif", 24, QFont::Bold));
        painter.drawText(image.rect(), Qt::AlignCenter, "FX"); painter.end();
        tray.setIcon(QIcon(image));
    }
    void refresh()
    {
        try {
            const auto current = state();
            load->setEnabled(false);
            refreshWindows();
            const int percent = qRound(current["strength"].toDouble() * 100);
            const int selected = current["selected"].toInt();
            const int mode = current["nvidia_mode"].toInt(8);
            const bool trueHdr = current["true_hdr"].toBool();
            const bool hdrAvailable = current["hdr_available"].toBool();
            hdr->setChecked(current["true_hdr_output"].toBool());
            hdr->setEnabled(hdrAvailable && current["nvidia_available"].toBool());
            hdr->setText(hdrAvailable ? "TrueHDR — SDR to HDR" : current["hdr_capable"].toBool()
                ? "TrueHDR — enable HDR in Display Configuration" : "TrueHDR — unavailable on this screen");
            sdrHdr->setChecked(current["true_hdr_sdr"].toBool());
            sdrHdr->setEnabled(current["nvidia_available"].toBool());
            for (auto *action : grains) {
                action->setChecked(action->data().toInt() == qRound(current["film_grain"].toDouble() * 100));
                action->setEnabled(current.contains("film_grain"));
            }
            for (auto *action : sharpens) {
                action->setChecked(action->data().toInt() == qRound(current["cas_strength"].toDouble() * 100));
                action->setEnabled(current.contains("cas_strength"));
            }
            const bool active = selected > 0 && current["processing"].toBool();
            const int pipeline = mode + (trueHdr ? 100 : 0) + (current["cas_only"].toBool() ? 1000 : 0);
            if (pipeline != previousMode) { seenOutput = false; wasEnabled = false; previousMode = pipeline; }
            for (auto *action : modes) {
                action->setChecked(action->data().toInt() == (current["cas_only"].toBool() ? -1 : mode));
                action->setEnabled(action->data().toInt() == -1 ? current.contains("cas_only") : current["nvidia_available"].toBool());
            }
            if (percent > 0) lastNonzero = percent;
            for (auto *action : strengths) action->setChecked(action->data().toInt() == percent);
            if (!active) { seenOutput = false; wasEnabled = false; }
            else {
                if (!wasEnabled) { baselineDraws = current["nvidia_draws"].toDouble(); wasEnabled = true; }
                if (current["nvidia_draws"].toDouble() > baselineDraws) seenOutput = true;
                for (const auto &job : current["nvidia_jobs"].toArray())
                    if (job.toObject()["state"].toInt() == 4 && job.toObject()["mode"].toInt(-1) == mode
                        && job.toObject()["true_hdr"].toBool() == trueHdr) seenOutput = true;
            }
            const QString error = current["nvidia_error"].toString();
            const bool nvidia = current["backend"].toString() == "nvidia-experimental";
            const bool cas = current["cas_only"].toBool() && !trueHdr;
            const QString base = current["cas_only"].toBool() ? "CAS" : mode >= 12 ? "Deblur" : "Video denoise";
            const bool hdrOutput = current["true_hdr_output"].toBool();
            const QString family = trueHdr ? base + (hdrOutput ? " + TrueHDR" : " + TrueHDR look (SDR)") : cas ? "CAS shader" : base;
            const QString text = !active ? "Enhancement OFF" : nvidia && !seenOutput ? "AI STARTING… — " + family : (cas ? "CAS ON — " : "AI ON — ") + family + " — " + QString::number(percent) + "%";
            QString detail = !error.isEmpty() && !active ? error
                : QString::number(selected) + (desktop ? " checked window(s) on your desktop. Settings apply to all checked windows." : " checked window(s) in the private preview.");
            if (current["cas_strength"].toDouble() > 0)
                detail += "\nCAS sharpening: " + QString::number(qRound(current["cas_strength"].toDouble() * 100)) + "%";
            if (current["film_grain"].toDouble() > 0)
                detail += "\nFilm grain: " + QString::number(qRound(current["film_grain"].toDouble() * 100)) + "%";
            if (trueHdr) {
                detail += "\nTrueHDR target: " + QString::number(current["hdr_peak_nits"].toInt()) + " nits";
                if (!hdrOutput) detail += "\nSDR tone mapping — HDR output is not enabled";
            }
            display(text, !active ? QColor("#a94141") : nvidia && !seenOutput ? QColor("#bb8120") : QColor("#23834b"), detail);
            enable->setEnabled(selected > 0 && !active); disable->setEnabled(active);
        } catch (const std::exception &error) {
            display(desktop ? "Effect unloaded / unavailable" : "Preview closed / unavailable", QColor("#666666"), QString::fromUtf8(error.what()));
            load->setEnabled(true); windowMenu->setEnabled(false);
            enable->setEnabled(false); disable->setEnabled(false); hdr->setEnabled(false); sdrHdr->setEnabled(false);
            if (!desktop && !closing) { closing = true; QTimer::singleShot(2500, qApp, &QApplication::quit); }
        }
    }
    void setStrength(int percent)
    {
        if (percent < 0 || percent > 100) return;
        QSettings config(configPath, QSettings::IniFormat);
        config.setValue("Effect-kwin_vfx/Strength", percent / 100.0); config.sync();
        QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
        effect.call("reconfigureEffect", "kwin_vfx");
        QTimer::singleShot(100, this, [this] { refresh(); });
    }
    void setMode(int mode)
    {
        QSettings config(configPath, QSettings::IniFormat);
        config.setValue("Effect-kwin_vfx/CasOnly", mode == -1);
        if (mode == -1) {
            if (config.value("Effect-kwin_vfx/CasStrength", 0.0).toDouble() <= 0)
                config.setValue("Effect-kwin_vfx/CasStrength", 0.5);
        } else config.setValue("Effect-kwin_vfx/NvidiaMode", mode);
        config.sync();
        QDBusInterface effect("org.kde.KWin", "/Effects", "org.kde.kwin.Effects", bus);
        effect.call("reconfigureEffect", "kwin_vfx");
        QTimer::singleShot(100, this, [this] { refresh(); });
    }
    void refreshWindows()
    {
        QDBusInterface control("org.kde.KWin", "/WindowEnhancement", "org.kde.KWin.WindowEnhancement", bus);
        control.setTimeout(1500);
        QDBusReply<QString> reply = control.call("windowList");
        if (!reply.isValid()) throw std::runtime_error(reply.error().message().toStdString());
        const auto document = QJsonDocument::fromJson(reply.value().toUtf8());
        if (!document.isArray()) throw std::runtime_error("Invalid window list");
        windowMenu->setEnabled(true);
        QSet<QString> seen;
        for (const auto &value : document.array()) {
            const auto window = value.toObject();
            const auto id = window["id"].toString();
            if (id.isEmpty()) continue;
            seen.insert(id);
            QAction *action = windows.value(id);
            if (!action) {
                action = windowMenu->addAction(QString()); action->setCheckable(true); windows.insert(id, action);
                QObject::connect(action, &QAction::triggered, this, [this, id](bool enabled) {
                    try {
                    QDBusInterface control("org.kde.KWin", "/WindowEnhancement", "org.kde.KWin.WindowEnhancement", bus);
                    QDBusReply<bool> reply = control.call("setWindowEnabled", id, enabled);
                    if (!reply.isValid() || !reply.value())
                        tray.showMessage("Could not select window", "The window closed or cannot be enhanced.", QSystemTrayIcon::Warning);
                    else if (enabled && state()["strength"].toDouble() <= 0) setStrength(lastNonzero);
                    refresh();
                    } catch (const std::exception &error) {
                        tray.showMessage("Could not select window", QString::fromUtf8(error.what()), QSystemTrayIcon::Warning);
                    }
                });
            }
            const auto title = window["title"].toString();
            const auto application = window["application"].toString();
            QString label = title.isEmpty() ? application : title + " — " + application;
            action->setText(label.replace('&', "&&"));
            action->setToolTip(application);
            action->setChecked(window["enabled"].toBool());
        }
        for (auto it = windows.begin(); it != windows.end();) {
            if (!seen.contains(it.key())) { delete it.value(); it = windows.erase(it); }
            else ++it;
        }
        emptyWindows->setVisible(windows.isEmpty());
    }

};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("KWin VFX Preview");
    app.setDesktopFileName("kwin-vfx-tray");
    app.setQuitOnLastWindowClosed(false);
    QCommandLineParser parser; parser.addHelpOption();
    parser.addOption({"preview-session", "Private preview session.json", "path"});
    parser.addOption({"desktop", "Control Window Enhancement in the desktop compositor"});
    parser.addOption({"application", "Legacy preview option (all application windows are now listed)", "name"});
    parser.process(app);
    try {
        const bool desktop = parser.isSet("desktop");
        if (desktop == parser.isSet("preview-session")) throw std::runtime_error("Choose exactly one of --desktop or --preview-session");
        if (!QSystemTrayIcon::isSystemTrayAvailable()) throw std::runtime_error("The desktop system tray is unavailable");
        QDBusConnection bus = QDBusConnection::sessionBus();
        QString configuration;
        if (desktop) {
            app.setApplicationName("KWin VFX");
            if (!bus.registerService("org.kde.WindowEnhancementTray")) throw std::runtime_error("Window Enhancement tray is already running");
            configuration = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/kwinrc";
        } else {
            const QFileInfo fileInfo(parser.value("preview-session"));
            QFile file(fileInfo.absoluteFilePath());
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read preview session record");
            const auto config = QJsonDocument::fromJson(file.readAll()).object();
            const QString runDir = config["KWIN_VFX_RUN_DIR"].toString();
            const QString address = config["DBUS_SESSION_BUS_ADDRESS"].toString();
            if (!QFileInfo(runDir).fileName().startsWith("kwin-vfx-nested.") || QFileInfo(runDir).canonicalFilePath() != fileInfo.canonicalPath()
                || !config["WAYLAND_DISPLAY"].toString().startsWith("kwin-vfx-kwin-vfx-nested.")
                || address.isEmpty() || address == qEnvironmentVariable("DBUS_SESSION_BUS_ADDRESS"))
                throw std::runtime_error("Refusing a non-private compositor connection");
            bus = QDBusConnection::connectToBus(address, "kwin-vfx-private-control");
            if (!bus.isConnected()) throw std::runtime_error("Cannot connect to private preview bus");
            configuration = runDir + "/config/kwinrc";
        }
        EnhancementTray controls(bus, configuration, desktop);
        return app.exec();
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
