#include "project.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStringConverter>
#include <QUuid>
#include <cmath>

// Resource initialization must live outside a namespace for static linking.
static void initializeSchema() { Q_INIT_RESOURCE(project_schema); }

namespace mlex {
namespace {
bool fail(QString *error, const QString &message) {
    if (error) *error = message;
    return false;
}

const QJsonObject &schema() {
    static const QJsonObject root = [] {
        initializeSchema();
        QFile file(":/mlex/schemas/project.schema.json");
        if (!file.open(QIODevice::ReadOnly)) qFatal("Project schema unavailable");
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return root;
}

// Only the schema vocabulary used by the versioned, embedded project schema.
bool validate(const QJsonValue &value, const QJsonObject &rule, const QString &at, QString *error) {
    auto invalid = [&] { return fail(error, at + QStringLiteral(": 保存形式に適合しません。")); };
    if (rule.contains("$ref")) {
        const QString name = rule.value("$ref").toString().section('/', -1);
        return validate(value, schema().value("$defs").toObject().value(name).toObject(), at, error);
    }
    if (rule.contains("oneOf")) {
        int matches = 0;
        for (const auto &option : rule.value("oneOf").toArray())
            matches += validate(value, option.toObject(), at, nullptr) ? 1 : 0;
        if (matches != 1) return invalid();
    }
    if (rule.contains("const") && value != rule.value("const")) return invalid();
    if (rule.contains("enum") && !rule.value("enum").toArray().contains(value)) return invalid();
    const QString type = rule.value("type").toString();
    if ((!type.isEmpty()) && !(
        (type == "object" && value.isObject()) || (type == "array" && value.isArray()) ||
        (type == "string" && value.isString()) || (type == "boolean" && value.isBool()) ||
        (type == "null" && value.isNull()) ||
        (type == "number" && value.isDouble() && std::isfinite(value.toDouble())) ||
        (type == "integer" && value.isDouble() && std::isfinite(value.toDouble()) &&
         std::floor(value.toDouble()) == value.toDouble()))) return invalid();
    if (value.isObject()) {
        const auto object = value.toObject();
        const auto properties = rule.value("properties").toObject();
        for (const auto &key : rule.value("required").toArray())
            if (!object.contains(key.toString())) return invalid();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (!properties.contains(it.key()) && rule.value("additionalProperties") == false) return invalid();
            if (properties.contains(it.key()) && !validate(it.value(), properties.value(it.key()).toObject(), at + "." + it.key(), error)) return false;
        }
    } else if (value.isArray()) {
        const auto array = value.toArray();
        if (array.size() < rule.value("minItems").toInt(0) || array.size() > rule.value("maxItems").toInt(INT_MAX)) return invalid();
        QJsonArray seen;
        for (qsizetype i = 0; i < array.size(); ++i) {
            if (rule.value("uniqueItems").toBool() && seen.contains(array[i])) return invalid();
            seen.append(array[i]);
            if (rule.contains("items") && !validate(array[i], rule.value("items").toObject(), at + QStringLiteral("[%1]").arg(i), error)) return false;
        }
    } else if (value.isString()) {
        const QString string = value.toString();
        const auto length = string.toUcs4().size();
        if (length < rule.value("minLength").toInt(0) || length > rule.value("maxLength").toInt(INT_MAX)) return invalid();
        if (rule.contains("pattern") && !QRegularExpression(rule.value("pattern").toString()).match(string).hasMatch()) return invalid();
    } else if (value.isDouble()) {
        if (rule.contains("minimum") && value.toDouble() < rule.value("minimum").toDouble()) return invalid();
        if (rule.contains("maximum") && value.toDouble() > rule.value("maximum").toDouble()) return invalid();
    }
    return true;
}

// Qt's JSON parser handles syntax; this pass additionally rejects duplicate
// object keys (including escaped spellings) and excessive nesting.
bool unambiguousJson(const QByteArray &bytes, QString *error) {
    struct Frame { bool object; QSet<QString> keys; };
    QList<Frame> frames;
    for (qsizetype i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '{' || c == '[') {
            frames.append(Frame{c == '{', {}});
            if (frames.size() > 32) return fail(error, "JSONの階層が深すぎます。");
        } else if (c == '}' || c == ']') {
            if (!frames.isEmpty()) frames.removeLast();
        } else if (c == '"') {
            const qsizetype start = i++;
            while (i < bytes.size() && bytes[i] != '"') {
                if (bytes[i] == '\\') ++i;
                ++i;
            }
            qsizetype next = i + 1;
            while (next < bytes.size() && QByteArray(" \t\n\r").contains(bytes[next])) ++next;
            if (next < bytes.size() && bytes[next] == ':' && !frames.isEmpty() && frames.last().object) {
                const auto token = bytes.mid(start, i - start + 1);
                const QString key = QJsonDocument::fromJson("[" + token + "]").array().first().toString();
                if (frames.last().keys.contains(key)) return fail(error, "JSONに重複したキーがあります。");
                frames.last().keys.insert(key);
            }
        }
    }
    return true;
}
}

QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QJsonObject defaultView() {
    return {{"mode", "details"}, {"sort_by", "name"}, {"sort_order", "asc"},
            {"directories_first", true}, {"show_hidden", false},
            {"columns", QJsonArray{"name", "size", "type", "modified"}},
            {"icon_size", 32}, {"tree_visible", true}, {"tree_width", 180}};
}

QJsonObject encodeLocation(const QString &path) {
    const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString home = QDir::cleanPath(QDir::homePath());
    if (clean == home) return {{"kind", "home_relative"}, {"path", ""}};
    if (clean.startsWith(home + '/')) return {{"kind", "home_relative"}, {"path", clean.mid(home.size() + 1)}};
    return {{"kind", "absolute"}, {"path", clean}};
}

QString resolveLocation(const QJsonObject &location) {
    if (location.value("kind") == "home_relative") return QDir::cleanPath(QDir(QDir::homePath()).filePath(location.value("path").toString()));
    return location.value("path").toString();
}

QJsonObject makePane(const QString &path) {
    return {{"pane_id", newId()}, {"location", encodeLocation(path)}, {"view", defaultView()},
            {"rect", QJsonObject{{"x", 0.02}, {"y", 0.02}, {"width", 0.7}, {"height", 0.8}}},
            {"window_state", "normal"}};
}

QJsonObject makeProject(const QStringList &paths) {
    QJsonArray panes;
    for (const auto &path : paths) {
        auto pane = makePane(path);
        const double offset = 0.025 * (panes.size() % 8);
        pane["rect"] = QJsonObject{{"x", offset}, {"y", offset}, {"width", 0.72}, {"height", 0.78}};
        panes.append(pane);
    }
    return {{"format", "moooyooo-linux-explorer-project"}, {"schema_version", 1},
            {"project_id", newId()}, {"revision", 1}, {"name", "無題の業務"},
            {"window", QJsonObject{{"width", 1280}, {"height", 800}, {"maximized", false}}},
            {"layout", QJsonObject{{"mode", "mdi"}, {"arrangement", "free"}}},
            {"panes", panes}, {"active_pane_id", panes.isEmpty() ? QJsonValue(QJsonValue::Null) : panes.first().toObject().value("pane_id")}};
}

bool validateProject(const QJsonObject &project, QString *error) {
    if (!validate(project, schema(), "$", error)) return false;
    QSet<QString> ids;
    for (const auto &value : project.value("panes").toArray()) {
        const auto pane = value.toObject();
        const QString id = pane.value("pane_id").toString();
        if (ids.contains(id)) return fail(error, "子ウィンドウのIDが重複しています。");
        ids.insert(id);
        const auto location = pane.value("location").toObject();
        const QString path = location.value("path").toString();
        for (const QChar c : path)
            if (c.unicode() < 32 || c.unicode() == 127) return fail(error, "場所に制御文字は使えません。");
        for (const QString &part : path.split('/'))
            if (part == "." || part == "..") return fail(error, "場所に相対移動の要素は使えません。");
        const bool absolute = location.value("kind") == "absolute";
        if (absolute != path.startsWith('/') || (!absolute && path.contains("://"))) return fail(error, "場所の種類とパスが一致しません。");
        const auto rect = pane.value("rect").toObject();
        if (rect.value("x").toDouble() + rect.value("width").toDouble() > 1.000001 ||
            rect.value("y").toDouble() + rect.value("height").toDouble() > 1.000001) return fail(error, "子ウィンドウが保存範囲を超えています。");
        if (!pane.value("view").toObject().value("columns").toArray().contains("name")) return fail(error, "名前の列は必須です。");
    }
    const auto active = project.value("active_pane_id");
    if (ids.isEmpty() ? !active.isNull() : !ids.contains(active.toString())) return fail(error, "アクティブな子ウィンドウの参照が不正です。");
    return true;
}

bool decodeProject(const QByteArray &bytes, QJsonObject *project, QString *error) {
    if (bytes.size() > MaxProjectBytes) return fail(error, "プロジェクトの上限は2MiBです。");
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString decoded = decoder(bytes);
    Q_UNUSED(decoded);
    if (decoder.hasError()) return fail(error, "UTF-8形式のJSONが必要です。");
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) return fail(error, "JSONを読み取れません。");
    if (!unambiguousJson(bytes, error)) return false;
    if (!validateProject(document.object(), error)) return false;
    if (project) *project = document.object();
    return true;
}

QByteArray encodeProject(const QJsonObject &project) { return QJsonDocument(project).toJson(QJsonDocument::Indented); }
}
