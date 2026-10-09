#include "file_clipboard.h"
#include "domain/file_operation.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QMimeData>
#include <QSet>
#include <QUrl>
#include <QUuid>

namespace mlex {
namespace {
constexpr auto OwnFormat = "application/x-mlex-file-transfer";
constexpr auto GnomeFormat = "x-special/gnome-copied-files";
constexpr auto KdeFormat = "application/x-kde-cutselection";
}
QJsonArray captureItems(const QStringList &paths) {
    QJsonArray items;
    for (const auto &path : paths) items.append(QJsonObject{{"path", path}, {"stamp", fileStamp(path)}});
    return items;
}

std::unique_ptr<QMimeData> makeFileTransfer(const QJsonArray &items, bool cut) {
    auto mime = std::make_unique<QMimeData>();
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const auto &value : items) {
        const auto url = QUrl::fromLocalFile(value.toObject().value("path").toString());
        urls.append(url);
        gnome += '\n' + url.toEncoded();
    }
    mime->setUrls(urls);
    mime->setData(GnomeFormat, gnome);
    mime->setData(KdeFormat, cut ? "1" : "0");
    mime->setData(OwnFormat, QJsonDocument(QJsonObject{{"version", 1}, {"cut", cut}, {"items", items},
        {"token", QUuid::createUuid().toString(QUuid::WithoutBraces)}}).toJson(QJsonDocument::Compact));
    return mime;
}

FileTransfer readFileTransfer(const QMimeData *mime) {
    FileTransfer result;
    auto fail = [&](const QString &message) { result.items = {}; result.error = message; return result; };
    if (!mime) return fail("コピーしたファイルがありません。");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto *format : {OwnFormat, GnomeFormat, KdeFormat, "text/uri-list"}) {
        const auto bytes = mime->data(format);
        if (bytes.size() > MaxOperationMessage) return fail("クリップボードの項目が多すぎます。");
        hash.addData(format); hash.addData(bytes);
    }
    result.fingerprint = hash.result();
    if (mime->hasFormat(OwnFormat)) {
        const auto doc = QJsonDocument::fromJson(mime->data(OwnFormat));
        const auto data = doc.object();
        if (!doc.isObject() || data.value("version").toInt() != 1 || !data.value("cut").isBool()) return fail("ファイル転送の形式が不正です。");
        result.items = data.value("items").toArray();
        result.cut = data.value("cut").toBool();
    } else {
        QList<QUrl> urls;
        if (mime->hasFormat(GnomeFormat)) {
            auto lines = mime->data(GnomeFormat).split('\n');
            const auto mode = lines.takeFirst();
            if (mode != "cut" && mode != "copy") return fail("認識できない切り取り形式です。");
            result.cut = mode == "cut";
            for (const auto &line : lines) if (!line.isEmpty()) {
                if (urls.size() >= MaxOperationItems) return fail("一度に操作できる項目は1〜256件です。");
                urls.append(QUrl::fromEncoded(line, QUrl::StrictMode));
            }
        } else if (mime->hasUrls()) {
            urls = mime->urls();
            if (mime->hasFormat(KdeFormat)) {
                const auto mode = mime->data(KdeFormat);
                if (mode != "0" && mode != "1") return fail("認識できない切り取り形式です。");
                result.cut = mode == "1";
            }
        } else return fail("コピーしたローカルファイルがありません。");
        if (urls.isEmpty() || urls.size() > MaxOperationItems) return fail("一度に操作できる項目は1〜256件です。");
        QStringList paths;
        for (const auto &url : urls) {
            if (!url.isValid() || !url.isLocalFile() || (!url.host().isEmpty() && url.host() != "localhost") ||
                !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return fail("ローカルファイルのURIだけを貼り付けできます。");
            paths.append(url.host() == "localhost" ? url.path(QUrl::FullyDecoded) : url.toLocalFile());
        }
        result.items = captureItems(paths);
    }
    if (result.items.isEmpty() || result.items.size() > MaxOperationItems) return fail("一度に操作できる項目は1〜256件です。");
    QSet<QString> seen;
    for (const auto &value : result.items) {
        const auto item = value.toObject();
        const QString path = item.value("path").toString();
        if (!validOperationPath(path) || seen.contains(path) || item.value("stamp").toObject().isEmpty()) return fail("対象が存在しないか、ファイルの指定が不正です。");
        seen.insert(path);
    }
    return result;
}
}
