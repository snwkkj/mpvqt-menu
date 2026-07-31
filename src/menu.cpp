#include "menu.h"

#include <cstring>

static const mpv_node *mapValue(const mpv_node &node, const char *key)
{
    if (node.format != MPV_FORMAT_NODE_MAP || !node.u.list)
        return nullptr;
    for (int i = 0; i < node.u.list->num; ++i) {
        if (std::strcmp(node.u.list->keys[i], key) == 0)
            return &node.u.list->values[i];
    }
    return nullptr;
}

static QString stringValue(const mpv_node &node, const char *key)
{
    const mpv_node *value = mapValue(node, key);
    if (!value || value->format != MPV_FORMAT_STRING || !value->u.string)
        return {};
    return QString::fromUtf8(value->u.string);
}

QVector<MenuItem> parseMenu(const mpv_node *node)
{
    QVector<MenuItem> result;
    if (!node || node->format != MPV_FORMAT_NODE_ARRAY || !node->u.list)
        return result;

    for (int i = 0; i < node->u.list->num; ++i) {
        const mpv_node &source = node->u.list->values[i];
        if (source.format != MPV_FORMAT_NODE_MAP)
            continue;

        MenuItem item;
        item.type = stringValue(source, "type");
        item.title = stringValue(source, "title");
        item.command = stringValue(source, "cmd");
        item.shortcut = stringValue(source, "shortcut");

        if (const mpv_node *state = mapValue(source, "state");
            state && state->format == MPV_FORMAT_NODE_ARRAY && state->u.list) {
            for (int j = 0; j < state->u.list->num; ++j) {
                const mpv_node &value = state->u.list->values[j];
                if (value.format != MPV_FORMAT_STRING || !value.u.string)
                    continue;
                item.checked |= std::strcmp(value.u.string, "checked") == 0;
                item.disabled |= std::strcmp(value.u.string, "disabled") == 0;
                item.hidden |= std::strcmp(value.u.string, "hidden") == 0;
            }
        }

        if (const mpv_node *submenu = mapValue(source, "submenu"))
            item.submenu = parseMenu(submenu);

        if (!item.hidden)
            result.push_back(std::move(item));
    }
    return result;
}

