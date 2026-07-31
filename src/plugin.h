#pragma once

#include "menu.h"
#include <mpv/client.h>

#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <thread>

class PluginContext {
public:
    explicit PluginContext(mpv_handle *handle);
    ~PluginContext();
    void setMenu(QVector<MenuItem> items);
    void showMenu();
    void closeMenu();
    void shutdown();
    void openFiles(const QString &target, bool multiple);
    void openFolder(const QString &target);
    void saveFile(const QString &target);
    void getClipboard(const QString &target);
    void setClipboard(const QString &text);

private:
    QByteArray runHelper(const QString &mode, const QByteArray &input = {});
    QByteArray dialogRequest() const;
    void sendReply(const QString &target, const char *message,
                   const QStringList &values);

    mpv_handle *mpv_;
    QVector<MenuItem> menuItems_;
    std::atomic_bool stopping_{false};
    std::atomic_bool menuActive_{false};
    std::atomic_bool menuCloseRequested_{false};
    std::thread menuThread_;
};
