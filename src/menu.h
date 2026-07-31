#pragma once

#include <mpv/client.h>

#include <QString>
#include <QVector>

struct MenuItem {
    QString type;
    QString title;
    QString command;
    QString shortcut;
    bool checked = false;
    bool disabled = false;
    bool hidden = false;
    QVector<MenuItem> submenu;
};

QVector<MenuItem> parseMenu(const mpv_node *node);

