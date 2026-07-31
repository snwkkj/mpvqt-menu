#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QCursor>
#include <QFileDialog>
#include <QFile>
#include <QEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QTimer>
#include <QWidget>

#include <xcb/xcb.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>

class MenuDismissFilter : public QObject {
public:
    explicit MenuDismissFilter(QApplication *app) : QObject(app), app_(app) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const bool applicationInactive =
            event->type() == QEvent::ApplicationStateChange &&
            app_->applicationState() != Qt::ApplicationActive;
        if (armed_ && (applicationInactive ||
                       event->type() == QEvent::WindowDeactivate)) {
            QTimer::singleShot(0, app_, &QCoreApplication::quit);
        }
        return QObject::eventFilter(watched, event);
    }

public:
    void arm() { armed_ = true; }

private:
    QApplication *app_;
    bool armed_ = false;
};

class X11OutsideClickWatcher : public QObject {
public:
    explicit X11OutsideClickWatcher(QApplication *app) : QObject(app), app_(app)
    {
        connection_ = xcb_connect(nullptr, nullptr);
        if (xcb_connection_has_error(connection_)) {
            xcb_disconnect(connection_);
            connection_ = nullptr;
            return;
        }

        const xcb_setup_t *setup = xcb_get_setup(connection_);
        xcb_screen_iterator_t screen = xcb_setup_roots_iterator(setup);
        if (!screen.data) {
            xcb_disconnect(connection_);
            connection_ = nullptr;
            return;
        }
        root_ = screen.data->root;
        activeWindowAtom_ = internAtom("_NET_ACTIVE_WINDOW");

        timer_.setInterval(5);
        connect(&timer_, &QTimer::timeout, this, [this] { poll(); });
    }

    ~X11OutsideClickWatcher() override
    {
        if (connection_) xcb_disconnect(connection_);
    }

    void arm()
    {
        if (!connection_) return;
        previousButtons_ = queryButtons();
        previousActiveWindow_ = queryActiveWindow();
        timer_.start();
    }

private:
    static constexpr uint16_t buttonMask_ =
        XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3 |
        XCB_BUTTON_MASK_4 | XCB_BUTTON_MASK_5;

    uint16_t queryButtons() const
    {
        xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(
            connection_, xcb_query_pointer(connection_, root_), nullptr);
        if (!reply) return previousButtons_;
        const uint16_t buttons = reply->mask & buttonMask_;
        std::free(reply);
        return buttons;
    }

    xcb_atom_t internAtom(const char *name) const
    {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(
            connection_,
            xcb_intern_atom(connection_, false, std::strlen(name), name),
            nullptr);
        if (!reply) return XCB_ATOM_NONE;
        const xcb_atom_t atom = reply->atom;
        std::free(reply);
        return atom;
    }

    xcb_window_t queryActiveWindow() const
    {
        if (activeWindowAtom_ == XCB_ATOM_NONE) return previousActiveWindow_;
        xcb_get_property_reply_t *reply = xcb_get_property_reply(
            connection_,
            xcb_get_property(connection_, false, root_, activeWindowAtom_,
                             XCB_ATOM_WINDOW, 0, 1),
            nullptr);
        if (!reply) return previousActiveWindow_;

        xcb_window_t window = XCB_WINDOW_NONE;
        if (reply->format == 32 && xcb_get_property_value_length(reply) >= 4)
            window = *static_cast<xcb_window_t *>(
                xcb_get_property_value(reply));
        std::free(reply);
        return window;
    }

    bool pointerInsideVisibleMenu() const
    {
        const QPoint pointer = QCursor::pos();
        for (QWidget *widget : app_->topLevelWidgets()) {
            if (qobject_cast<QMenu *>(widget) && widget->isVisible() &&
                widget->frameGeometry().contains(pointer))
                return true;
        }
        return false;
    }

    void poll()
    {
        const uint16_t buttons = queryButtons();
        const uint16_t pressed = buttons & ~previousButtons_;
        previousButtons_ = buttons;
        if (pressed && !pointerInsideVisibleMenu()) app_->quit();

        const xcb_window_t activeWindow = queryActiveWindow();
        if (activeWindow != previousActiveWindow_) app_->quit();
    }

    QApplication *app_;
    QTimer timer_;
    xcb_connection_t *connection_ = nullptr;
    xcb_window_t root_ = XCB_WINDOW_NONE;
    xcb_atom_t activeWindowAtom_ = XCB_ATOM_NONE;
    xcb_window_t previousActiveWindow_ = XCB_WINDOW_NONE;
    uint16_t previousButtons_ = 0;
};

static QByteArray stdinData()
{
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return {};
    return input.readAll();
}

static void addItems(QMenu *menu, const QJsonArray &items)
{
    for (const QJsonValue &value : items) {
        const QJsonObject item = value.toObject();
        if (item["type"] == "separator") { menu->addSeparator(); continue; }
        const QString title = item["title"].toString();
        if (title.isEmpty()) continue;
        QAction *action;
        if (item["type"] == "submenu") {
            QMenu *child = menu->addMenu(title);
            addItems(child, item["submenu"].toArray());
            action = child->menuAction();
            if (child->isEmpty()) action->setEnabled(false);
        } else {
            action = menu->addAction(title);
            action->setData(item["command"].toString());
        }
        if (!item["shortcut"].toString().isEmpty()) action->setText(title + "\t" + item["shortcut"].toString());
        action->setCheckable(item["checked"].toBool());
        action->setChecked(item["checked"].toBool());
        action->setEnabled(action->isEnabled() && !item["disabled"].toBool());
    }
}

static QStringList filters(const QJsonObject &request)
{
    QStringList result;
    for (const QJsonValue &value : request["filters"].toArray()) {
        const QJsonObject f = value.toObject();
        QString spec = f["spec"].toString();
        spec.replace(';', ' ');
        if (!f["name"].toString().isEmpty() && !spec.isEmpty()) result << f["name"].toString() + " (" + spec + ")";
    }
    return result;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc < 2) return 2;
    const QString mode = QString::fromLocal8Bit(argv[1]);
    const QByteArray input = stdinData();
    if (mode == "menu") {
        const bool x11Popup = QApplication::platformName() == "xcb";
        MenuDismissFilter dismissFilter(&app);
        X11OutsideClickWatcher outsideClickWatcher(&app);
        app.installEventFilter(&dismissFilter);
        QMenu menu;
        if (x11Popup) {
            menu.setAttribute(Qt::WA_X11NetWmWindowTypePopupMenu);
            menu.setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        } else {
            menu.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint |
                                Qt::WindowStaysOnTopHint);
        }
        addItems(&menu, QJsonDocument::fromJson(input).array());
        QObject::connect(&menu, &QMenu::triggered, &app, [&](QAction *selected) {
            const QByteArray value = selected->data().toString().toUtf8();
            std::fwrite(value.constData(), 1, value.size(), stdout);
            std::fflush(stdout);
            app.quit();
        });
        QObject::connect(&menu, &QMenu::aboutToHide, &app,
                         &QCoreApplication::quit);
        // Keep the pointer just above the first item when the popup opens.
        // Mutter places an XWayland popup exactly at QCursor::pos(), which
        // otherwise immediately highlights (and may open) the first submenu.
        const QPoint popupPosition = QCursor::pos() + QPoint(0, 6);
        if (x11Popup) {
            menu.popup(popupPosition);
        } else {
            menu.move(popupPosition);
            menu.show();
            menu.raise();
            menu.activateWindow();
        }
        QTimer::singleShot(150, &dismissFilter,
                           [&dismissFilter, &outsideClickWatcher] {
            dismissFilter.arm();
            outsideClickWatcher.arm();
        });
        return app.exec();
    }
    if (mode == "clipboard-get") {
        const QByteArray value = QApplication::clipboard()->text().toUtf8().toBase64();
        std::fwrite(value.constData(), 1, value.size(), stdout); return 0;
    }
    if (mode == "clipboard-set") { QApplication::clipboard()->setText(QString::fromUtf8(QByteArray::fromBase64(input))); return 0; }
    const QJsonObject request = QJsonDocument::fromJson(input).object();
    QString initial = request["path"].toString();
    if (!request["name"].toString().isEmpty()) initial += (initial.endsWith('/') ? "" : "/") + request["name"].toString();
    const QString filter = filters(request).join(";;");
    if (mode == "open" || mode == "open-multi") {
        QFileDialog dialog(nullptr, QObject::tr("Open"), initial, filter);
        dialog.setFileMode(mode == "open-multi" ? QFileDialog::ExistingFiles : QFileDialog::ExistingFile);
        if (dialog.exec() == QDialog::Accepted) {
            QJsonArray result; for (const QString &file : dialog.selectedFiles()) result.append(file);
            const QByteArray out = QJsonDocument(result).toJson(QJsonDocument::Compact); std::fwrite(out.constData(), 1, out.size(), stdout);
        }
    } else if (mode == "open-folder") {
        const QByteArray out = QFileDialog::getExistingDirectory(nullptr, QObject::tr("Open Folder"), initial).toUtf8(); std::fwrite(out.constData(), 1, out.size(), stdout);
    } else if (mode == "save") {
        const QByteArray out = QFileDialog::getSaveFileName(nullptr, QObject::tr("Save"), initial, filter).toUtf8(); std::fwrite(out.constData(), 1, out.size(), stdout);
    }
    return 0;
}
