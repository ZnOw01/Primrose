// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QString>

#include "common/common_types.h"

/// User-chosen artwork per game, stored under <eden dir>/game_art/<program id>/.
namespace QtCommon::GameArt {

QString HeroPath(u64 program_id);
QString IconPath(u64 program_id);

bool HasIcon(u64 program_id);

bool SaveHero(u64 program_id, const QByteArray& image);
bool SaveIcon(u64 program_id, const QByteArray& image);

void RemoveHero(u64 program_id);
void RemoveIcon(u64 program_id);

} // namespace QtCommon::GameArt
