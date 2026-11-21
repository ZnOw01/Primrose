// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QDir>
#include <QFile>
#include <QSaveFile>

#include "common/fs/path_util.h"
#include "qt_common/util/game_art.h"

namespace QtCommon::GameArt {

namespace {

QString ArtDir(u64 program_id) {
    const auto base = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "game_art";
    return QString::fromStdString(Common::FS::PathToUTF8String(base)) + QLatin1Char('/') +
           QStringLiteral("%1").arg(program_id, 16, 16, QLatin1Char('0')).toUpper();
}

bool Save(const QString& path, const QByteArray& image) {
    if (image.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(image);
    return file.commit();
}

} // namespace

QString HeroPath(u64 program_id) {
    return ArtDir(program_id) + QStringLiteral("/hero.img");
}

QString IconPath(u64 program_id) {
    return ArtDir(program_id) + QStringLiteral("/icon.img");
}

bool HasIcon(u64 program_id) {
    return program_id != 0 && QFile::exists(IconPath(program_id));
}

bool SaveHero(u64 program_id, const QByteArray& image) {
    return program_id != 0 && Save(HeroPath(program_id), image);
}

bool SaveIcon(u64 program_id, const QByteArray& image) {
    return program_id != 0 && Save(IconPath(program_id), image);
}

void RemoveHero(u64 program_id) {
    QFile::remove(HeroPath(program_id));
}

void RemoveIcon(u64 program_id) {
    QFile::remove(IconPath(program_id));
}

} // namespace QtCommon::GameArt
