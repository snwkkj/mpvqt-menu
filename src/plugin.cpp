#include "plugin.h"

#include <mpv/client.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryFile>

#include <cstdio>
#include <sys/stat.h>

#define MENU_DATA_PROP "user-data/menu/items"
#define WINDOW_MINIMIZED_PROP "window-minimized"
#define DIALOG_FILTER_PROP "user-data/menu/dialog/filters"
#define DIALOG_DEF_PATH_PROP "user-data/menu/dialog/default-path"
#define DIALOG_DEF_NAME_PROP "user-data/menu/dialog/default-name"

extern "C" {
MPV_EXPORT int mpv_open_cplugin(mpv_handle *handle);
extern const unsigned char _binary_menu_helper_start[];
extern const unsigned char _binary_menu_helper_end[];
}

static QJsonArray menuJson(const QVector<MenuItem> &items)
{
    QJsonArray array;
    for (const MenuItem &item : items) {
        QJsonObject object{{"type", item.type}, {"title", item.title},
                           {"command", item.command}, {"shortcut", item.shortcut},
                           {"checked", item.checked}, {"disabled", item.disabled}};
        object["submenu"] = menuJson(item.submenu);
        array.append(object);
    }
    return array;
}

static QString propertyString(mpv_handle *mpv, const char *name)
{
    char *value = mpv_get_property_string(mpv, name);
    if (!value) return {};
    const QString result = QString::fromUtf8(value);
    mpv_free(value);
    return result;
}

PluginContext::PluginContext(mpv_handle *handle) : mpv_(handle) {}

PluginContext::~PluginContext() { shutdown(); }

void PluginContext::shutdown()
{
    stopping_ = true;
    menuCloseRequested_ = true;
    if (menuThread_.joinable()) menuThread_.join();
}

void PluginContext::closeMenu()
{
    if (menuActive_) menuCloseRequested_ = true;
}

void PluginContext::setMenu(QVector<MenuItem> items) { menuItems_ = std::move(items); }

QByteArray PluginContext::runHelper(const QString &mode, const QByteArray &input)
{
    QString helperPath;
    {
        QTemporaryFile helper(QDir::tempPath() + "/mpv-menu-helper-XXXXXX");
        if (!helper.open()) return {};

        const auto helperSize = _binary_menu_helper_end - _binary_menu_helper_start;
        if (helper.write(reinterpret_cast<const char *>(_binary_menu_helper_start),
                         helperSize) != helperSize)
            return {};
        if (::fchmod(helper.handle(), S_IRUSR | S_IWUSR | S_IXUSR) != 0) {
            std::fprintf(stderr, "[menu] failed to make embedded helper executable\n");
            return {};
        }
        helper.flush();
        helperPath = helper.fileName();
        helper.setAutoRemove(false);
        helper.close();
    }

    QProcess process;
    if (mode == "menu") {
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        // A real X11 popup receives the NETWM popup-menu type and is not shown
        // as an independent application by Wayland task managers via XWayland.
        environment.insert("QT_QPA_PLATFORM", "xcb");
        process.setProcessEnvironment(environment);
    }
    process.start(helperPath, {mode});
    if (!process.waitForStarted(3000)) {
        std::fprintf(stderr, "[menu] failed to start embedded helper: %s\n",
                     process.errorString().toUtf8().constData());
        QFile::remove(helperPath);
        return {};
    }
    if (!input.isEmpty()) process.write(input);
    process.closeWriteChannel();
    while (!process.waitForFinished(50)) {
        const bool closeMenu = mode == "menu" && menuCloseRequested_;
        if (!stopping_ && !closeMenu) continue;
        process.terminate();
        if (!process.waitForFinished(500)) {
            process.kill();
            process.waitForFinished(500);
        }
        break;
    }
    QFile::remove(helperPath);
    if (stopping_) return {};
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::fprintf(stderr, "[menu] embedded helper failed: %s\n",
                     process.readAllStandardError().constData());
        return {};
    }
    return process.readAllStandardOutput();
}

void PluginContext::showMenu()
{
    if (menuActive_.exchange(true)) return;
    if (menuThread_.joinable()) menuThread_.join();
    menuCloseRequested_ = false;

    const char *open[] = {"script-message", "menu-open", nullptr};
    mpv_command(mpv_, open);
    const QByteArray input = QJsonDocument(menuJson(menuItems_)).toJson(QJsonDocument::Compact);
    menuThread_ = std::thread([this, input] {
        const QString command = QString::fromUtf8(runHelper("menu", input)).trimmed();
        if (!stopping_ && !command.isEmpty() && !command.startsWith('#') &&
            command != "ignore")
            mpv_command_string(mpv_, command.toUtf8().constData());
        if (!stopping_) {
            const char *close[] = {"script-message", "menu-close", nullptr};
            mpv_command(mpv_, close);
        }
        menuActive_ = false;
    });
}

QByteArray PluginContext::dialogRequest() const
{
    QJsonObject request{{"path", propertyString(mpv_, DIALOG_DEF_PATH_PROP)},
                        {"name", propertyString(mpv_, DIALOG_DEF_NAME_PROP)}};
    QJsonArray filters;
    mpv_node node{};
    if (mpv_get_property(mpv_, DIALOG_FILTER_PROP, MPV_FORMAT_NODE, &node) >= 0) {
        if (node.format == MPV_FORMAT_NODE_ARRAY && node.u.list) {
            for (int i = 0; i < node.u.list->num; ++i) {
                const mpv_node &entry = node.u.list->values[i];
                if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list) continue;
                QJsonObject filter;
                for (int j = 0; j < entry.u.list->num; ++j) {
                    const mpv_node &value = entry.u.list->values[j];
                    if (value.format == MPV_FORMAT_STRING)
                        filter[entry.u.list->keys[j]] = QString::fromUtf8(value.u.string);
                }
                filters.append(filter);
            }
        }
        mpv_free_node_contents(&node);
    }
    request["filters"] = filters;
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

void PluginContext::sendReply(const QString &target, const char *message,
                              const QStringList &values)
{
    QList<QByteArray> storage = {"script-message-to", target.toUtf8(), message};
    for (const QString &value : values) storage.push_back(value.toUtf8());
    QVector<const char *> args;
    for (const QByteArray &value : storage) args.push_back(value.constData());
    args.push_back(nullptr);
    mpv_command(mpv_, args.data());
}

void PluginContext::openFiles(const QString &target, bool multiple)
{
    const QJsonDocument reply = QJsonDocument::fromJson(runHelper(multiple ? "open-multi" : "open", dialogRequest()));
    QStringList files;
    for (const QJsonValue &value : reply.array()) files.push_back(value.toString());
    if (!files.isEmpty()) sendReply(target, multiple ? "dialog-open-multi-reply" : "dialog-open-reply", files);
}

void PluginContext::openFolder(const QString &target)
{
    const QString path = QString::fromUtf8(runHelper("open-folder", dialogRequest())).trimmed();
    if (!path.isEmpty()) sendReply(target, "dialog-open-folder-reply", {path});
}

void PluginContext::saveFile(const QString &target)
{
    const QString path = QString::fromUtf8(runHelper("save", dialogRequest())).trimmed();
    if (!path.isEmpty()) sendReply(target, "dialog-save-reply", {path});
}

void PluginContext::getClipboard(const QString &target)
{
    const QByteArray encoded = runHelper("clipboard-get").trimmed();
    if (!encoded.isEmpty()) sendReply(target, "clipboard-get-reply", {QString::fromUtf8(QByteArray::fromBase64(encoded))});
}

void PluginContext::setClipboard(const QString &text)
{
    runHelper("clipboard-set", text.toUtf8().toBase64());
}

static void handleMessage(PluginContext &ctx, const mpv_event_client_message *msg)
{
    if (!msg || msg->num_args < 1) return;
    const QString command = QString::fromUtf8(msg->args[0]);
    const QString argument = msg->num_args > 1 ? QString::fromUtf8(msg->args[1]) : QString{};
    if (command == "show" || command == "native-menu/show") ctx.showMenu();
    else if (command == "clipboard/get" && !argument.isEmpty()) ctx.getClipboard(argument);
    else if (command == "clipboard/set") ctx.setClipboard(argument);
    else if (command == "dialog/open" && !argument.isEmpty()) ctx.openFiles(argument, false);
    else if (command == "dialog/open-multi" && !argument.isEmpty()) ctx.openFiles(argument, true);
    else if (command == "dialog/open-folder" && !argument.isEmpty()) ctx.openFolder(argument);
    else if (command == "dialog/save" && !argument.isEmpty()) ctx.saveFile(argument);
}

extern "C" MPV_EXPORT int mpv_open_cplugin(mpv_handle *handle)
{
    PluginContext ctx(handle);
    mpv_node node{};
    if (mpv_get_property(handle, MENU_DATA_PROP, MPV_FORMAT_NODE, &node) >= 0) {
        ctx.setMenu(parseMenu(&node));
        mpv_free_node_contents(&node);
    }
    mpv_observe_property(handle, 1, MENU_DATA_PROP, MPV_FORMAT_NODE);
    mpv_observe_property(handle, 2, WINDOW_MINIMIZED_PROP, MPV_FORMAT_FLAG);
    const char *init[] = {"script-message", "menu-init", mpv_client_name(handle), nullptr};
    mpv_command(handle, init);
    for (;;) {
        mpv_event *event = mpv_wait_event(handle, -1);
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            ctx.shutdown();
            break;
        }
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            auto *property = static_cast<mpv_event_property *>(event->data);
            if (property && property->data) {
                if (qstrcmp(property->name, MENU_DATA_PROP) == 0) {
                    ctx.setMenu(parseMenu(static_cast<mpv_node *>(property->data)));
                } else if (qstrcmp(property->name, WINDOW_MINIMIZED_PROP) == 0 &&
                           *static_cast<int *>(property->data)) {
                    ctx.closeMenu();
                }
            }
        } else if (event->event_id == MPV_EVENT_CLIENT_MESSAGE) {
            handleMessage(ctx, static_cast<mpv_event_client_message *>(event->data));
        }
    }
    mpv_unobserve_property(handle, 1);
    mpv_unobserve_property(handle, 2);
    return 0;
}
