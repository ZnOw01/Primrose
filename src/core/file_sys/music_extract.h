// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

// Pulls the music streams out of a game the emulator can open from a NUP archive
// or a loose folder dump. Works on the Alto-family audio layout used by modern
// Nintendo titles: romfs/Sound/Resource/Stream/BGM_*.bwav are the songs, Dm_*
// cutscene music (which can also hide behind the song prefix as BGM_Dm_*) and
// ExampleMelody_* reference performances, while other prefixes in the same
// folder are voice, effects and ambience. Only the selected kinds
// (Options::kinds) are decoded and written.
namespace FileSys::Music {

enum class Layout {
    // One folder per kind (Music, Cutscenes, Title, Segments, Stems), each
    // holding a subfolder per area or scene taken from the stream's clean name.
    ByKind,
    // Every track directly in the output folder.
    Flat,
};

enum class Encoding {
    // PCM 16-bit; playable everywhere, roughly ten times the Opus size.
    Wav,
    // Ogg Opus at 48 kHz. The streams are DSP- or Opus-compressed to begin with,
    // so a lossless container buys no precision, only size.
    Opus,
};

// What a stream is, picked from its name; drives the kind filter and the manifest.
enum class Kind : u32 {
    Bgm,      // BGM_*: the songs.
    Cutscene, // Dm_* (also behind the song prefix as BGM_Dm_*): scene and demo
              // music short of the title performance.
    Title,    // Dm_Title* / BGM_Dm_Title*: the title screen performance.
    Segments, // Intros, loops, jingles, outros and A/B/C sections of a song.
    Stems,    // Close/far ambience, vocal or noise layers the game mixes live.
    Count,
};
constexpr std::size_t KindCount = static_cast<std::size_t>(Kind::Count);

// Manifest spelling of a kind ("bgm", "cutscene", ...).
std::string_view KindName(Kind kind);

struct Options {
    std::filesystem::path out_dir;
    Layout layout{Layout::ByKind};
    Encoding encoding{Encoding::Wav};
    // Which kinds to write out; all on by default.
    std::array<bool, KindCount> kinds{true, true, true, true, true};
    // Cover art override; empty means whatever the NUP carries, if anything.
    std::vector<u8> cover_override;
};

struct Progress {
    u64 files_done{};
    u64 files_total{};
    u64 bytes_done{};
    u64 bytes_total{};
    std::string current;
};

enum class Result { Success, Cancelled, InvalidSource, NoMusic, IoError };

struct Summary {
    u64 files_ok{};
    u64 files_failed{};
    u64 duplicates{};
};

struct SourceInfo {
    std::string game_title;
    u64 stream_count{};
    u64 stream_bytes{};
    // How many streams of each kind a run would see; drives the dialog's checkboxes.
    std::array<u64, KindCount> kind_counts{};
    // The NUP/package icon when one is stored; used as the cover art by default.
    std::vector<u8> cover;
};

// A quick look at what ExtractMusic would pull from the game. Returns false when the
// path is neither a NUP archive nor a loose dump; stream_count can still be 0.
bool ScanMusic(const std::string& game_path, SourceInfo& info);

// Decodes every selected music stream to the output folder (plus manifest.csv) and
// never touches the source. Tracks that fail to decode are recorded in the manifest
// and counted in the summary rather than aborting the run.
Result ExtractMusic(const std::string& game_path, const Options& options,
                    const std::function<void(const Progress&)>& progress = {},
                    const std::atomic_bool* cancel = nullptr, Summary* summary_out = nullptr,
                    std::string* error_out = nullptr);

} // namespace FileSys::Music
