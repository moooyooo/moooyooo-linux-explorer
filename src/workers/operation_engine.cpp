#include <gio/gio.h>
#include "operation_engine.h"
#include "domain/file_operation.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QHash>
#include <QSet>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace mlex {
namespace {
struct Failure { QString message; };
struct Cancelled {};
constexpr int UndoEntryLimit = 2048;
constexpr int TreeEntryLimit = 100000;

void checkCancel(OperationControl &control) { if (control.cancelled) throw Cancelled{}; }
void require(bool condition, const QString &message) { if (!condition) throw Failure{message}; }
QString systemFailure() {
    switch (errno) {
    case EEXIST: return "同名の項目があります。上書きしていません。";
    case EACCES: case EPERM: return "アクセス権がありません。";
    case ENOSPC: case EDQUOT: return "保存先の空き容量が不足しています。";
    case ENOENT: return "対象が見つかりません。";
    case ENOTEMPTY: return "フォルダに別の項目があるため削除できません。";
    case EROFS: return "読み取り専用の場所です。";
    case EXDEV: return "異なるファイルシステムへの名前変更はできません。";
    default: return "ファイル操作に失敗しました。対象の状態とアクセス権を確認してください。";
    }
}
void requireSystem(bool condition) { if (!condition) throw Failure{systemFailure()}; }

struct Descriptor {
    int fd = -1;
    explicit Descriptor(int value) : fd(value) {}
    ~Descriptor() { if (fd >= 0) ::close(fd); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};

QStringList children(const QString &path) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    requireSystem(fd >= 0);
    DIR *directory = ::fdopendir(fd);
    if (!directory) { const auto message = systemFailure(); ::close(fd); throw Failure{message}; }
    QStringList names;
    errno = 0;
    while (auto *entry = ::readdir(directory)) {
        const QByteArray bytes(entry->d_name);
        if (bytes == "." || bytes == "..") continue;
        const QString name = QFile::decodeName(bytes);
        if (QFile::encodeName(name) != bytes || !validFileName(name)) {
            ::closedir(directory);
            throw Failure{"UTF-8で正確に表せない名前が含まれるため操作を停止しました。"};
        }
        names.append(name);
        errno = 0;
    }
    const int error = errno;
    ::closedir(directory);
    if (error) { errno = error; throw Failure{systemFailure()}; }
    return names;
}

bool directory(const QJsonObject &stamp) { return S_ISDIR(stamp.value("mode").toInt()); }
bool regular(const QJsonObject &stamp) { return S_ISREG(stamp.value("mode").toInt()); }
bool link(const QJsonObject &stamp) { return S_ISLNK(stamp.value("mode").toInt()); }
void supported(const QJsonObject &stamp) {
    require(!stamp.isEmpty(), "対象が見つかりません。");
    require(directory(stamp) || regular(stamp) || link(stamp), "特殊ファイルはこの操作の対象にできません。");
}

void walk(const QString &root, const QString &relative, QJsonArray &entries, OperationControl &control, int limit) {
    checkCancel(control);
    require(entries.size() < limit, "対象が記録上限を超えています。");
    require(relative.count('/') < 128, "フォルダの階層が深すぎます。");
    const QString path = relative.isEmpty() ? root : QDir(root).filePath(relative);
    const auto stamp = fileStamp(path);
    supported(stamp);
    entries.append(QJsonObject{{"relative", relative}, {"stamp", stamp}});
    if (directory(stamp)) {
        for (const auto &name : children(path)) walk(root, relative.isEmpty() ? name : relative + '/' + name, entries, control, limit);
        require(matchesStamp(path, stamp), "確認中にフォルダの内容が変更されました。再試行してください。");
    }
}

QJsonArray manifest(const QString &root, OperationControl &control, int limit = TreeEntryLimit) {
    QJsonArray result;
    walk(root, {}, result, control, limit);
    return result;
}

bool sameManifest(const QString &root, const QJsonArray &expected, OperationControl &control) {
    if (expected.isEmpty()) return false;
    const auto actual = manifest(root, control);
    if (actual.size() != expected.size()) return false;
    QHash<QString, QJsonObject> saved;
    for (const auto &value : expected) {
        const auto item = value.toObject();
        saved.insert(item.value("relative").toString(), item.value("stamp").toObject());
    }
    for (const auto &value : actual) {
        const auto item = value.toObject();
        if (saved.value(item.value("relative").toString()) != item.value("stamp").toObject()) return false;
    }
    return true;
}

int renameExclusive(const QString &from, const QString &to) {
    return int(::syscall(SYS_renameat2, AT_FDCWD, QFile::encodeName(from).constData(),
                         AT_FDCWD, QFile::encodeName(to).constData(), RENAME_NOREPLACE));
}

void metadata(const QString &path, const struct stat &source, bool symlink) {
    if (!symlink) requireSystem(::chmod(QFile::encodeName(path).constData(), source.st_mode & 0777) == 0);
    const timespec times[]{source.st_atim, source.st_mtim};
    requireSystem(::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), times, AT_SYMLINK_NOFOLLOW) == 0);
}

void copyTree(const QString &from, const QString &to, OperationControl &control, int &count) {
    checkCancel(control);
    require(++count <= TreeEntryLimit, "コピー対象が上限を超えています。");
    const auto before = fileStamp(from);
    supported(before);
    struct stat info {};
    requireSystem(::lstat(QFile::encodeName(from).constData(), &info) == 0);
    if (directory(before)) {
        requireSystem(::mkdir(QFile::encodeName(to).constData(), 0700) == 0);
        for (const auto &name : children(from)) copyTree(QDir(from).filePath(name), QDir(to).filePath(name), control, count);
    } else if (link(before)) {
        QByteArray target(65536, '\0');
        const auto size = ::readlink(QFile::encodeName(from).constData(), target.data(), size_t(target.size()));
        require(size >= 0 && size < target.size(), "リンク先を読み取れません。");
        target.resize(size);
        requireSystem(::symlink(target.constData(), QFile::encodeName(to).constData()) == 0);
    } else {
        Descriptor input(::open(QFile::encodeName(from).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
        requireSystem(input.fd >= 0);
        struct stat opened {};
        require(::fstat(input.fd, &opened) == 0 && S_ISREG(opened.st_mode) && opened.st_dev == info.st_dev && opened.st_ino == info.st_ino,
                "コピー元が確認後に差し替えられました。");
        Descriptor output(::open(QFile::encodeName(to).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
        requireSystem(output.fd >= 0);
        char buffer[64 * 1024];
        qint64 copied = 0;
        while (true) {
            checkCancel(control);
            const auto size = ::read(input.fd, buffer, sizeof(buffer));
            if (size < 0 && errno == EINTR) continue;
            requireSystem(size >= 0);
            if (size == 0) break;
            ssize_t offset = 0;
            while (offset < size) {
                const auto written = ::write(output.fd, buffer + offset, size_t(size - offset));
                if (written < 0 && errno == EINTR) continue;
                requireSystem(written > 0);
                offset += written;
            }
            copied += size;
            if (control.progress) control.progress("コピー中", copied);
        }
        requireSystem(::fsync(output.fd) == 0);
    }
    require(matchesStamp(from, before), "コピー中にコピー元が変更されました。完成先へは確定していません。");
    metadata(to, info, link(before));
}

// Copied directories may have read-only permissions. Make only our private
// staging tree writable before QTemporaryDir removes an abandoned copy.
void writableStaging(const QString &path) {
    const auto stamp = fileStamp(path);
    if (!directory(stamp)) return;
    ::chmod(QFile::encodeName(path).constData(), 0700);
    for (const auto &name : children(path)) writableStaging(QDir(path).filePath(name));
}
struct Staging {
    QTemporaryDir directory;
    explicit Staging(const QString &pattern) : directory(pattern) {}
    ~Staging() { try { if (directory.isValid()) writableStaging(directory.path()); } catch (...) {} }
};

void removeVerified(const QString &root, const QJsonArray &entries, OperationControl &control, bool &changed) {
    require(sameManifest(root, entries, control), "対象が変更・差し替えされています。内容を消さずに停止しました。");
    for (auto it = entries.constEnd(); it != entries.constBegin();) {
        const auto item = (*--it).toObject();
        checkCancel(control);
        const QString relative = item.value("relative").toString();
        const QString path = relative.isEmpty() ? root : QDir(root).filePath(relative);
        const auto stamp = item.value("stamp").toObject();
        // Removing children changes directory timestamps; inode identity and rmdir
        // still prevent replacement or deletion of newly added contents.
        require(matchesStamp(path, stamp, directory(stamp)), "削除中に対象が変更されました。残りの操作を停止しました。");
        const int result = directory(stamp) ? ::rmdir(QFile::encodeName(path).constData()) : ::unlink(QFile::encodeName(path).constData());
        requireSystem(result == 0);
        changed = true;
        if (control.progress) control.progress("削除中", 0);
    }
}

QString otherName(const QString &path, int number, bool isDirectory) {
    const QFileInfo info(path);
    const QString extension = isDirectory || info.suffix().isEmpty() || info.fileName().startsWith('.') ? QString() : "." + info.suffix();
    const QString base = info.fileName().left(info.fileName().size() - extension.size());
    return info.dir().filePath(base + QString(" - コピー (%1)").arg(number) + extension);
}

QJsonObject single(const QString &kind, const QString &path, const QJsonObject &stamp, const QString &destination = {}, const QString &name = {}) {
    return {{"kind", kind}, {"items", QJsonArray{QJsonObject{{"path", path}, {"stamp", stamp}}}},
            {"destination", destination}, {"name", name}, {"policy", "skip"}};
}

QJsonObject run(const QJsonObject &request, OperationControl &control, int depth) {
    QJsonArray successes, failures, warnings, inverse, metadataChanges;
    bool changed = false, cancelled = false, allUndoable = true;
    const QString kind = request.value("kind").toString();
    auto result = [&] {
        QJsonObject undo;
        if (changed && allUndoable && failures.isEmpty() && !cancelled && !inverse.isEmpty())
            undo = {{"kind", "batch"}, {"actions", inverse}};
        if (QJsonDocument(undo).toJson(QJsonDocument::Compact).size() > MaxOperationMessage / 2) undo = {};
        QJsonObject result{{"successes", successes}, {"failures", failures}, {"cancelled", cancelled},
                           {"changed", changed}, {"undo", undo}, {"warnings", warnings}, {"metadata_changes", metadataChanges}};
        if (QJsonDocument(result).toJson(QJsonDocument::Compact).size() > MaxOperationMessage - 4096) {
            result["undo"] = QJsonObject{};
            result["metadata_changes"] = QJsonArray{};
        }
        return result;
    };
    try {
        require(depth < 4, "操作要求の階層が不正です。");
        if (kind == "batch") {
            const auto actions = request.value("actions").toArray();
            require(!actions.isEmpty() && actions.size() <= MaxOperationItems, "操作件数が不正です。");
            // Validate every recorded source before starting a multi-item undo.
            for (const auto &value : actions) {
                const auto action = value.toObject();
                require(action.value("kind") != "batch", "入れ子の操作要求は受け付けません。");
                for (const auto &entry : action.value("items").toArray()) {
                    const auto item = entry.toObject();
                    const QString path = item.value("path").toString();
                    require(matchesStamp(path, item.value("stamp").toObject()), "記録後に対象が変更されました。操作していません。");
                    if (action.contains("manifest")) require(sameManifest(path, action.value("manifest").toArray(), control), "記録後に内容が変更されました。操作していません。");
                }
            }
            for (const auto &value : actions) {
                checkCancel(control);
                const auto part = run(value.toObject(), control, depth + 1);
                for (const auto &v : part.value("successes").toArray()) successes.append(v);
                for (const auto &v : part.value("failures").toArray()) failures.append(v);
                for (const auto &v : part.value("warnings").toArray()) warnings.append(v);
                for (const auto &v : part.value("metadata_changes").toArray()) metadataChanges.append(v);
                changed |= part.value("changed").toBool();
                cancelled |= part.value("cancelled").toBool();
                if (!part.value("undo").toObject().isEmpty()) {
                    auto prefix = part.value("undo").toObject().value("actions").toArray();
                    for (const auto &action : inverse) prefix.append(action);
                    inverse = prefix;
                }
                else if (part.value("changed").toBool()) allUndoable = false;
                if (!failures.isEmpty() || cancelled) break;
            }
            return result();
        }
        require(QStringList{"rename", "mkdir", "copy", "move", "trash", "remove", "remove_created"}.contains(kind), "操作の種類が不正です。");
        auto items = request.value("items").toArray();
        if (kind == "mkdir") items = QJsonArray{QJsonObject{}};
        require(!items.isEmpty() && items.size() <= MaxOperationItems, "一度に操作できる項目は256件までです。");
        require(QJsonDocument(items).toJson(QJsonDocument::Compact).size() < MaxOperationMessage / 4, "操作元のパスが長すぎます。項目を分けてください。");
        if (kind == "rename") require(items.size() == 1, "名前変更は1件ずつ実行してください。");
        const QString destination = request.value("destination").toString();
        if (kind == "mkdir" || kind == "rename" || kind == "copy" || kind == "move")
            require((validOperationPath(destination) || destination == "/") && QFileInfo(destination).isDir(), "保存先のフォルダが見つかりません。");
        const QString name = request.value("name").toString();
        if (kind == "mkdir" || kind == "rename") require(validFileName(name), "名前が不正です。空文字、/、NUL、.、.. は使えません。");
        const QString targetName = request.value("target_name").toString();
        if (request.contains("target_name")) require(items.size() == 1 && validFileName(targetName), "保存先の名前が不正です。");
        // Overlapping selections could otherwise operate on an item twice.
        for (int i = 0; i < items.size(); ++i) for (int j = i + 1; j < items.size(); ++j) {
            const QString a = items[i].toObject().value("path").toString();
            const QString b = items[j].toObject().value("path").toString();
            require(a != b && !a.startsWith(b + '/') && !b.startsWith(a + '/'), "親子のフォルダや重複した項目は分けて操作してください。");
        }
        QSet<QString> seen;
        for (const auto &value : items) {
            const auto item = value.toObject();
            const QString source = item.value("path").toString();
            try {
                checkCancel(control);
                if (kind != "mkdir") {
                    require(validOperationPath(source) && !seen.contains(source), "操作元の指定が不正または重複しています。");
                    seen.insert(source);
                    require(matchesStamp(source, item.value("stamp").toObject()), "選択後に対象が変更・差し替えされました。再選択してください。");
                    supported(fileStamp(source));
                    if (request.contains("manifest")) require(sameManifest(source, request.value("manifest").toArray(), control), "記録後に内容が変更されました。操作していません。");
                }
                if (control.progress) control.progress(kind == "copy" ? "コピー中" : "操作中", 0);
                QHash<QString, QJsonObject> parents;
                QStringList parentPaths{destination};
                if (!source.isEmpty()) parentPaths.append(QFileInfo(source).absolutePath());
                for (const auto &path : parentPaths) {
                    const auto stamp = fileStamp(path);
                    if (!stamp.isEmpty()) parents.insert(path, stamp);
                }
                QString target;
                QJsonObject undo;
                if (kind == "mkdir") {
                    target = QDir(destination).filePath(name);
                    requireSystem(::mkdir(QFile::encodeName(target).constData(), 0755) == 0);
                    changed = true;
                    successes.append(QJsonObject{{"source", source}, {"target", target}, {"kind", kind}});
                    undo = single("remove_created", target, fileStamp(target));
                    undo["manifest"] = manifest(target, control);
                    auto redo = request;
                    redo["revive_stamp"] = fileStamp(target);
                    undo["redo"] = redo;
                    if (!request.value("revive_stamp").toObject().isEmpty())
                        metadataChanges.append(QJsonObject{{"before", request.value("revive_stamp")}, {"after", fileStamp(target)}});
                } else if (kind == "copy" || kind == "move" || kind == "rename") {
                    target = QDir(destination).filePath(kind == "rename" ? name : targetName.isEmpty() ? QFileInfo(source).fileName() : targetName);
                    const QString realSource = QFileInfo(source).canonicalFilePath();
                    const QString realDestination = QFileInfo(destination).canonicalFilePath();
                    const auto stamp = fileStamp(source);
                    if (directory(stamp)) require(realDestination != realSource && !realDestination.startsWith(realSource + '/'), "自分自身やその配下へはコピー・移動できません。");
                    const bool samePath = QFileInfo(source).canonicalPath() == realDestination && QFileInfo(source).fileName() == QFileInfo(target).fileName();
                    if (samePath && kind != "copy") {
                        successes.append(QJsonObject{{"source", source}, {"target", source}});
                        continue;
                    }
                    const bool keepBoth = request.value("policy") == "keep_both" || (samePath && kind == "copy" && targetName.isEmpty());
                    const QString proposed = target;
                    int suffix = 1;
                    if (keepBoth) while (!fileStamp(target).isEmpty()) {
                        require(suffix < 10000, "別名の候補が多すぎます。");
                        target = otherName(proposed, suffix++, directory(stamp));
                    }
                    require(fileStamp(target).isEmpty(), "同名の項目があるためスキップしました。上書きしていません。");
                    bool crossDevice = false;
                    if (kind != "copy") {
                        require(matchesStamp(source, stamp), "移動元が変更されました。");
                        if (renameExclusive(source, target) != 0) {
                            crossDevice = errno == EXDEV && kind == "move";
                            requireSystem(crossDevice);
                        } else {
                            changed = true;
                            successes.append(QJsonObject{{"source", source}, {"target", target}, {"kind", kind}});
                            metadataChanges.append(QJsonObject{{"before", stamp}, {"after", fileStamp(target)}});
                        }
                    }
                    if (kind == "copy" || crossDevice) {
                        const auto sourceManifest = manifest(source, control);
                        Staging staging(QDir(destination).filePath(".mlex-copy-XXXXXX"));
                        require(staging.directory.isValid(), "コピー用の一時領域を作成できません。");
                        const QString staged = staging.directory.filePath("item");
                        int copiedItems = 0;
                        copyTree(source, staged, control, copiedItems);
                        require(sameManifest(source, sourceManifest, control), "コピー元が操作中に変更されました。完成先へは確定していません。");
                        checkCancel(control);
                        while (renameExclusive(staged, target) != 0) {
                            requireSystem(keepBoth && errno == EEXIST && suffix < 10000);
                            target = otherName(proposed, suffix++, directory(stamp));
                        }
                        changed = true;
                        successes.append(QJsonObject{{"source", source}, {"target", target}, {"kind", "copy"}});
                        if (crossDevice) {
                            try { removeVerified(source, sourceManifest, control, changed); }
                            catch (...) {
                                warnings.append("コピー先は完成していますが、移動元の削除は完了していません。");
                                throw;
                            }
                            successes[successes.size() - 1] = QJsonObject{{"source", source}, {"target", target}, {"kind", "move"}};
                            warnings.append("別のファイルシステムへの移動は取り消し対象外です。");
                        } else if (sourceManifest.size() <= UndoEntryLimit) {
                            undo = single("remove_created", target, fileStamp(target));
                            const auto createdManifest = manifest(target, control, UndoEntryLimit);
                            undo["manifest"] = createdManifest;
                            auto redo = single("copy", source, fileStamp(source), destination);
                            redo["manifest"] = sourceManifest;
                            // Preserve the exact chosen name for redo, including duplicate copies.
                            redo["target_name"] = QFileInfo(target).fileName();
                            redo["revive_manifest"] = createdManifest;
                            undo["redo"] = redo;
                            // A successful redo recreates exactly the verified
                            // copy. Rebind later redo entries to its new inodes.
                            const auto previous = request.value("revive_manifest").toArray();
                            QHash<QString, QJsonObject> created;
                            for (const auto &entry : createdManifest) {
                                const auto item = entry.toObject();
                                created.insert(item.value("relative").toString(), item.value("stamp").toObject());
                            }
                            for (const auto &entry : previous) {
                                const auto item = entry.toObject();
                                const auto after = created.value(item.value("relative").toString());
                                if (!after.isEmpty()) metadataChanges.append(QJsonObject{{"before", item.value("stamp")}, {"after", after}});
                            }
                        }
                        warnings.append("コピーは権限と更新時刻を保持します。ACL・拡張属性・ハードリンク構造の完全な複製は保証しません。");
                    } else {
                        undo = single("rename", target, fileStamp(target), QFileInfo(source).absolutePath(), QFileInfo(source).fileName());
                        try { undo["manifest"] = manifest(target, control, UndoEntryLimit); }
                        catch (const Failure &) { undo = {}; }
                    }
                } else if (kind == "trash") {
                    GFile *file = g_file_new_for_path(QFile::encodeName(source).constData());
                    GError *error = nullptr;
                    const bool ok = g_file_trash(file, control.gioCancellation, &error);
                    g_object_unref(file);
                    const auto detail = error ? QString::fromUtf8(error->message) : QString();
                    if (error) g_error_free(error);
                    if (!ok && control.cancelled) throw Cancelled{};
                    require(ok, "ごみ箱へ移動できませんでした。完全削除への切り替えは行っていません。\n" + detail);
                    changed = true;
                    successes.append(QJsonObject{{"source", source}, {"target", target}, {"kind", kind}});
                    warnings.append("ごみ箱からの復元はOSのごみ箱で行ってください。");
                } else {
                    const auto entries = kind == "remove_created" ? request.value("manifest").toArray() : manifest(source, control);
                    removeVerified(source, entries, control, changed);
                    successes.append(QJsonObject{{"source", source}, {"target", target}, {"kind", kind}});
                    if (kind == "remove_created") undo = request.value("redo").toObject();
                }
                if (!undo.isEmpty()) inverse.prepend(undo);
                else allUndoable = false;
                for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
                    const auto after = fileStamp(it.key());
                    if (!after.isEmpty() && after != it.value()) metadataChanges.append(QJsonObject{{"before", it.value()}, {"after", after}});
                }
            } catch (const Failure &error) {
                failures.append(QJsonObject{{"source", source}, {"error", error.message}});
                allUndoable = false;
            }
        }
    } catch (const Cancelled &) { cancelled = true; }
      catch (const Failure &error) { failures.append(QJsonObject{{"error", error.message}}); }
    return result();
}
}

QJsonObject executeOperation(const QJsonObject &request, OperationControl &control) {
    if (QJsonDocument(request).toJson(QJsonDocument::Compact).size() > MaxOperationMessage * 3 / 4)
        return {{"failures", QJsonArray{QJsonObject{{"error", "操作要求が大きすぎます。項目を分けてください。"}}}}};
    return run(request, control, 0);
}
}
