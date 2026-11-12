// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Local test hook, never part of a release: LICHII_FAKE_GAMES=<count> appends <count> synthetic
// games to the library so the library styles can be tried with a large collection. Unset, it
// does nothing. Fake entries never boot and never touch files, settings or play time data.

#pragma once

#include <algorithm>
#include <string>

#include <QModelIndex>
#include <QString>
#include <QtGlobal>

#include "common/common_types.h"

namespace FakeGames {

/// Marks fake folder and game rows (set on column 0).
constexpr int FakeEntryRole = Qt::UserRole + 64;

/// Fake program ids: 01FA4E followed by the entry number.
constexpr u64 IdBase = 0x01FA4E0000000000ULL;
constexpr u64 IdMask = 0xFFFFFF0000000000ULL;

/// Program id of fake entry number `index` (0-based).
inline u64 IdFor(int index) {
    return IdBase | (static_cast<u64>(index + 1) << 16);
}

inline QString PathPrefix() {
    return QStringLiteral("fake://");
}

/// Number of fake games requested by LICHII_FAKE_GAMES, 0 when unset or invalid.
inline int Count() {
    static const int count = [] {
        bool ok = false;
        const int value = qEnvironmentVariableIntValue("LICHII_FAKE_GAMES", &ok);
        return ok && value > 0 ? std::min(value, 20000) : 0;
    }();
    return count;
}

inline bool Enabled() {
    return Count() > 0;
}

inline bool IsFakeId(u64 program_id) {
    return Enabled() && program_id != 0 && (program_id & IdMask) == IdBase;
}

inline bool IsFakePath(const QString& path) {
    return Enabled() && path.startsWith(PathPrefix());
}

inline bool IsFakePath(const std::string& path) {
    return IsFakePath(QString::fromStdString(path));
}

inline bool IsFake(const QModelIndex& index) {
    if (!Enabled() || !index.isValid()) {
        return false;
    }
    const QModelIndex first = index.sibling(index.row(), 0);
    return first.data(FakeEntryRole).toBool();
}

} // namespace FakeGames
