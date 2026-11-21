// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/music_extract.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <tuple>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/vfs/vfs_real.h"

#include <opus.h>
#include <opus_multistream.h>

namespace FileSys::Music {

namespace {

constexpr std::array MusicPrefixes{"BGM_", "Dm_", "ExampleMelody_"};

struct Entry {
    std::string name;
    VirtualFile file;
};

struct Collected {
    std::vector<Entry> entries;
    std::string game_title;
    // NUP/package cover art (PNG or JPEG bytes), embedded into every track.
    std::vector<u8> icon;
};

std::string Lower(std::string_view value) {
    std::string out{value};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool EndsWithCI(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           Lower(value.substr(value.size() - suffix.size())) == Lower(suffix);
}

bool IsNupPath(const std::string& path) {
    return EndsWithCI(path, ".nup");
}

bool IsMusicName(const std::string& name) {
    if (!EndsWithCI(name, ".bwav")) {
        return false;
    }
    return std::any_of(MusicPrefixes.begin(), MusicPrefixes.end(),
                       [&name](std::string_view prefix) {
                           return Lower(name).starts_with(Lower(prefix));
                       });
}

VirtualDir FindSubdirCI(const VirtualDir& dir, std::string_view name) {
    if (dir == nullptr) {
        return nullptr;
    }
    if (const auto exact = dir->GetSubdirectory(std::string{name}); exact != nullptr) {
        return exact;
    }
    const auto lower = Lower(name);
    for (const auto& sub : dir->GetSubdirectories()) {
        if (Lower(sub->GetName()) == lower) {
            return sub;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Loose folder dumps
// -------------------------------------------------------------------------------------

std::optional<std::filesystem::path> FindChildCI(const std::filesystem::path& parent,
                                                 std::string_view name) {
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        return std::nullopt;
    }
    // Walk with explicit error codes: the range-for's increment throws on an I/O
    // error, and nothing above catches filesystem_error.
    const auto lower = Lower(name);
    const std::filesystem::directory_iterator end{};
    for (auto it = std::filesystem::directory_iterator(parent, ec); !ec && it != end;
         it.increment(ec)) {
        std::error_code dir_ec;
        if (it->is_directory(dir_ec) && !dir_ec &&
            Lower(Common::FS::PathToUTF8String(it->path().filename())) == lower) {
            return it->path();
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> StreamDirOfRomfs(const std::filesystem::path& romfs) {
    const auto sound = FindChildCI(romfs, "Sound");
    if (!sound) {
        return std::nullopt;
    }
    const auto resource = FindChildCI(*sound, "Resource");
    if (!resource) {
        return std::nullopt;
    }
    return FindChildCI(*resource, "Stream");
}

void CollectFromStreamDir(const VirtualDir& stream_dir, std::map<std::string, Entry>& out) {
    if (stream_dir == nullptr) {
        return;
    }
    for (const auto& file : stream_dir->GetFiles()) {
        if (IsMusicName(file->GetName())) {
            out.insert_or_assign(file->GetName(), Entry{file->GetName(), file});
        }
    }
}

bool CollectLoose(const std::filesystem::path& dump_root, RealVfsFilesystem& vfs,
                  Collected& collected, std::string& error) {
    // base/romfs (or a bare romfs/), then every updates/<version>/romfs in ascending
    // order so newer trees override older ones by file name.
    std::vector<std::filesystem::path> romfs_dirs;
    if (const auto base = FindChildCI(dump_root, "base")) {
        if (const auto romfs = FindChildCI(*base, "romfs")) {
            romfs_dirs.push_back(*romfs);
        }
    } else if (const auto romfs = FindChildCI(dump_root, "romfs")) {
        romfs_dirs.push_back(*romfs);
    }
    std::vector<std::pair<u64, std::filesystem::path>> updates;
    if (const auto updates_dir = FindChildCI(dump_root, "updates")) {
        std::error_code ec;
        const std::filesystem::directory_iterator end{};
        for (auto it = std::filesystem::directory_iterator(*updates_dir, ec); !ec && it != end;
             it.increment(ec)) {
            u64 version{};
            const auto name = Common::FS::PathToUTF8String(it->path().filename());
            const auto [ptr, err] =
                std::from_chars(name.data(), name.data() + name.size(), version);
            std::error_code dir_ec;
            if (err != std::errc{} || ptr != name.data() + name.size() ||
                !it->is_directory(dir_ec) || dir_ec) {
                continue;
            }
            updates.emplace_back(version, it->path());
        }
        if (ec) {
            error = fmt::format("cannot list {}: {}", Common::FS::PathToUTF8String(*updates_dir),
                                ec.message());
            return false;
        }
    }
    std::sort(updates.begin(), updates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [version, dir] : updates) {
        if (const auto romfs = FindChildCI(dir, "romfs")) {
            romfs_dirs.push_back(*romfs);
        }
    }
    if (romfs_dirs.empty()) {
        error = "no romfs in the dump";
        return false;
    }

    std::map<std::string, Entry> merged;
    for (const auto& romfs : romfs_dirs) {
        if (const auto stream = StreamDirOfRomfs(romfs)) {
            CollectFromStreamDir(vfs.OpenDirectory(Common::FS::PathToUTF8String(*stream)),
                                 merged);
        }
    }
    // No music streams is a valid, recognized dump: the caller reports "no
    // music" rather than "unsupported source".
    collected.entries.reserve(merged.size());
    for (auto& [name, entry] : merged) {
        collected.entries.push_back(std::move(entry));
    }
    const auto probed = Nup::ProbeMetadata(dump_root);
    collected.game_title = !probed.title_name.empty()
                               ? probed.title_name
                               : Common::FS::PathToUTF8String(dump_root.filename());
    collected.icon = probed.icon;
    return true;
}

// ---------------------------------------------------------------------------------------------
// NUP archives
// -------------------------------------------------------------------------------------

bool CollectNup(const std::string& game_path, RealVfsFilesystem& vfs, Collected& collected,
                std::string& error) {
    const auto file = vfs.OpenFile(game_path);
    if (file == nullptr || !NupContainer::IsNupFile(file)) {
        error = "not a NUP archive";
        return false;
    }
    NupContainer container{file};
    if (container.GetStatus() != Loader::ResultStatus::Success) {
        error = fmt::format("NUP container error {} ({})",
                            static_cast<int>(container.GetStatus()),
                            Loader::GetResultStatusString(container.GetStatus()));
        return false;
    }
    if (container.GetPackages().empty()) {
        error = "NUP container has no packages";
        return false;
    }

    // The romfs the game would see: the base package overlaid with the newest update,
    // or the update alone when the container is a standalone update.
    VirtualDir merged;
    if (const auto base = container.FindBasePackage()) {
        merged = container.GetRomFSDir(*base);
    }
    if (const auto update = container.FindBestUpdatePackage()) {
        const auto update_romfs = container.GetRomFSDir(*update);
        if (merged != nullptr && update_romfs != nullptr) {
            merged = NupContainer::MergeShadowDirs(merged, update_romfs);
        } else if (update_romfs != nullptr) {
            merged = update_romfs;
        }
    }
    if (merged == nullptr) {
        error = "NUP container has no RomFS";
        return false;
    }

    const auto sound = FindSubdirCI(merged, "Sound");
    const auto resource = FindSubdirCI(sound, "Resource");
    const auto stream = FindSubdirCI(resource, "Stream");
    if (stream == nullptr) {
        // A valid archive; this game just stores no stream music.
        return true;
    }
    std::map<std::string, Entry> merged_files;
    CollectFromStreamDir(stream, merged_files);
    collected.entries.reserve(merged_files.size());
    for (auto& [name, entry] : merged_files) {
        collected.entries.push_back(std::move(entry));
    }
    for (const auto& package : container.GetPackages()) {
        if (!package.title_name.empty()) {
            collected.game_title = package.title_name;
            break;
        }
    }
    if (collected.game_title.empty()) {
        collected.game_title = std::string{Common::FS::GetFilename(game_path)};
    }
    if (const auto base = container.FindBasePackage()) {
        collected.icon = container.GetPackages()[*base].icon;
    }
    if (collected.icon.empty()) {
        for (const auto& package : container.GetPackages()) {
            if (!package.icon.empty()) {
                collected.icon = package.icon;
                break;
            }
        }
    }
    return true;
}

bool Collect(const std::string& game_path, RealVfsFilesystem& vfs, Collected& collected,
             std::string& error) {
    if (IsNupPath(game_path)) {
        return CollectNup(game_path, vfs, collected, error);
    }
    const auto root = Nup::DumpRootFromMain(std::filesystem::path{game_path});
    if (!root) {
        error = "not a NUP archive or a loose dump";
        return false;
    }
    if (!CollectLoose(*root, vfs, collected, error)) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// Track classification and clean names
// -------------------------------------------------------------------------------------

// A stream name split into its underscore tokens with the category markers
// (BGM, Dm, ExampleMelody) peeled off the front. Demo music can hide behind the
// song prefix - "BGM_Dm_*" belongs to the same cutscene family as the plain
// "Dm_*" - so the peel keeps going past BGM.
struct NameParts {
    // Owned copies: the source string lives only inside ParseName, so views into
    // it would dangle the moment the function returns.
    std::vector<std::string> tokens;
    bool demo{}; // a Dm marker was peeled off
};

NameParts ParseName(const std::string& entry_name) {
    const std::string stem =
        EndsWithCI(entry_name, ".bwav") ? entry_name.substr(0, entry_name.size() - 5) : entry_name;
    NameParts parts;
    std::size_t start = 0;
    bool real = false; // a non-marker token has been seen
    for (std::size_t i = 0; i <= stem.size(); ++i) {
        if (i != stem.size() && stem[i] != '_') {
            continue;
        }
        if (i > start) {
            const std::string_view token = std::string_view{stem}.substr(start, i - start);
            const std::string head = Lower(token);
            if (real || (head != "bgm" && head != "dm" && head != "examplemelody")) {
                real = true;
                parts.tokens.emplace_back(token);
            } else if (head == "dm") {
                parts.demo = true;
            }
        }
        start = i + 1;
    }
    return parts;
}

Kind KindOf(const std::string& name) {
    const std::string stem = EndsWithCI(name, ".bwav") ? name.substr(0, name.size() - 5) : name;
    const NameParts parts = ParseName(name);
    if (parts.demo && !parts.tokens.empty() && Lower(parts.tokens.front()) == "title") {
        return Kind::Title;
    }
    // Layers the game mixes live: close/far ambience, vocal or noise stems, final-lap variants.
    for (const std::string_view marker : {"_Close_", "_Far_", "_Noise", "_Pad", "_Vocal_", "_FL"}) {
        if (stem.find(marker) != std::string::npos) {
            return Kind::Stems;
        }
    }
    // Sequenced parts: intros, loops, jingles, outros and A/B/C sections.
    static constexpr std::array SegmentMarkers{
        "_Intro",  "_Loop", "_Jingle", "_Outro", "_OneShot", "_Prerace",
    };
    for (const std::string_view marker : SegmentMarkers) {
        if (stem.find(marker) != std::string::npos) {
            return Kind::Segments;
        }
    }
    if (stem.size() >= 2 && stem.back() >= 'A' && stem.back() <= 'C' &&
        stem[stem.size() - 2] == '_') {
        return Kind::Segments;
    }
    if (parts.demo) {
        return Kind::Cutscene;
    }
    return Kind::Bgm;
}

// Folder spelling of each kind, in Kind order; matches the checkboxes in the
// extraction dialog so the disk layout reads like the choices behind it.
constexpr std::array<std::string_view, KindCount> KindFolders{
    "Music", "Cutscenes", "Title", "Segments", "Stems",
};

bool SafeComponent(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos;
}

// The output folder is kind/area components joined with '/', the file name one
// component. Both trace back to the game's stream names, so both are checked
// before anything is written below the chosen output directory.
bool SafeOutputPath(const std::string& folder, const std::string& file_name) {
    if (!SafeComponent(file_name)) {
        return false;
    }
    std::size_t start = 0;
    for (std::size_t i = 0; i <= folder.size(); ++i) {
        if (i != folder.size() && folder[i] != '/') {
            continue;
        }
        if (i > start && !SafeComponent(std::string_view{folder}.substr(start, i - start))) {
            return false;
        }
        start = i + 1;
    }
    return true;
}

// Walks a folder's components below the output root once and remembers the
// folders proven clean; true only when a component turns out to be a symlink.
bool FolderHasSymlink(const std::filesystem::path& out_dir, const std::string& folder,
                      std::set<std::string>& clean_folders) {
    if (clean_folders.count(folder) != 0) {
        return false;
    }
    std::filesystem::path acc = out_dir;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= folder.size(); ++i) {
        if (i != folder.size() && folder[i] != '/') {
            continue;
        }
        if (i > start) {
            acc /= std::string{std::string_view{folder}.substr(start, i - start)};
            std::error_code ec;
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(acc, ec)) && !ec) {
                return true;
            }
        }
        start = i + 1;
    }
    clean_folders.insert(folder);
    return false;
}

// ---------------------------------------------------------------------------------------------
// BWAV (NintendoWare wave), laid out as vgmstream reads it:
//   0x00 "BWAV", 0x04 BOM, 0x0e channel count, then a 0x4C block per channel from 0x10.
// ---------------------------------------------------------------------------------------------

constexpr std::size_t ChannelBlockSize = 0x4C;
constexpr u16 CodecPcm16 = 0;
constexpr u16 CodecDsp = 1;
constexpr u16 CodecNxOpus = 2;

// Cap on one track's decoded PCM: far past anything a real game ships (the
// longest known track decodes to ~60 MB), small enough that a corrupt header
// claiming billions of samples cannot exhaust the machine before it is caught.
constexpr u64 kMaxPcmBytes = 1ull << 30;

struct BwavChannel {
    u16 codec{};
    s32 sample_rate{};
    s32 num_samples{};
    std::array<s16, 16> coefs{};
    u32 data_start{};
    s32 loop_end{-1};
    s32 loop_start{};
    s16 hist1{};
    s16 hist2{};
};

struct BwavInfo {
    u16 channels{};
    u32 interleave{};
    std::vector<BwavChannel> channel;
};

u16 Read16(std::span<const u8> data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        return 0;
    }
    u16 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

s32 ReadS32(std::span<const u8> data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    s32 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

u32 ReadU32(std::span<const u8> data, std::size_t offset) {
    return static_cast<u32>(ReadS32(data, offset));
}

bool ParseBwav(std::span<const u8> data, BwavInfo& info, std::string& note) {
    if (data.size() < 0x10) {
        note = "truncated header";
        return false;
    }
    if (std::memcmp(data.data(), "BWAV", 4) != 0) {
        note = "missing BWAV magic";
        return false;
    }
    info.channels = Read16(data, 0x0e);
    if (info.channels == 0 || info.channels > 8) {
        note = fmt::format("bad channel count {}", info.channels);
        return false;
    }
    info.channel.resize(info.channels);
    for (u16 ch = 0; ch < info.channels; ++ch) {
        const std::size_t base = 0x10 + ChannelBlockSize * ch;
        if (base + ChannelBlockSize > data.size()) {
            note = "truncated channel block";
            return false;
        }
        BwavChannel& channel = info.channel[ch];
        channel.codec = Read16(data, base + 0x00);
        channel.sample_rate = ReadS32(data, base + 0x04);
        channel.num_samples = ReadS32(data, base + 0x0c);
        channel.data_start = ReadU32(data, base + 0x34);
        channel.loop_end = ReadS32(data, base + 0x3c);
        channel.loop_start = ReadS32(data, base + 0x40);
        channel.hist1 = static_cast<s16>(Read16(data, base + 0x46));
        channel.hist2 = static_cast<s16>(Read16(data, base + 0x48));
        for (std::size_t i = 0; i < channel.coefs.size(); ++i) {
            channel.coefs[i] = static_cast<s16>(Read16(data, base + 0x10 + i * 2));
        }
        if (channel.codec != CodecPcm16 && channel.codec != CodecDsp &&
            channel.codec != CodecNxOpus) {
            note = fmt::format("unknown codec {}", channel.codec);
            return false;
        }
        if (channel.sample_rate <= 0 || channel.num_samples < 0) {
            note = "bad rate or sample count";
            return false;
        }
    }
    if (info.channels > 1) {
        // vgmstream: the second channel's data offset sits at 0x8C; the per-channel stride
        // is the difference to the first channel's data start.
        const u32 second_channel = ReadU32(data, 0x8C);
        info.interleave = second_channel > info.channel[0].data_start
                              ? second_channel - info.channel[0].data_start
                              : 0;
    }
    return true;
}

s16 Clamp16(s32 sample) {
    return static_cast<s16>(std::clamp(sample, -0x8000, 0x7FFF));
}

// Byte offset of a channel's data, in the block-interleaved layout vgmstream reads:
// blocks of `interleave` bytes, cycling through the channels. When one block holds the
// whole channel (the usual case in these dumps) it degenerates to a contiguous read.
std::size_t InterleavedOffset(u16 channels, u32 interleave, u16 channel,
                              std::size_t byte_in_channel) {
    if (interleave == 0 || channels <= 1) {
        return byte_in_channel;
    }
    const std::size_t block = byte_in_channel / interleave;
    const std::size_t within = byte_in_channel % interleave;
    return block * interleave * channels + static_cast<std::size_t>(channel) * interleave +
           within;
}

// One channel of Nintendo DSP 4-bit ADPCM. 8-byte frames: a header byte (scale in the
// low nibble, coefficient index in the high one) followed by 14 samples as signed
// nibbles, high nibble first, decoded exactly as vgmstream does.
// Channel positions follow vgmstream: every channel is relative to channel 0's data
// start (the per-channel +0x34 fields hold the same absolute offsets, so mixing them
// with the interleave stride would double-count it).
void DecodeDspChannel(std::span<const u8> data, const BwavChannel& channel, u32 base_start,
                      u16 channel_index, u16 channels, u32 interleave, std::span<s16> out) {
    s32 hist1 = channel.hist1;
    s32 hist2 = channel.hist2;
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t offset =
            base_start +
            InterleavedOffset(channels, interleave, channel_index, (done / 14) * 8);
        if (offset + 8 > data.size()) {
            break;
        }
        const u8 header = data[offset];
        const s32 scale = 1 << (header & 0xf);
        u32 coef_index = (header >> 4) & 0xf;
        if (coef_index > 7) {
            coef_index = 7;
        }
        const s32 coef1 = channel.coefs[coef_index * 2 + 0];
        const s32 coef2 = channel.coefs[coef_index * 2 + 1];
        for (std::size_t i = 0; i < 7 && done < out.size(); ++i) {
            const u8 nibbles = data[offset + 1 + i];
            for (const u8 nibble : {u8(nibbles >> 4), u8(nibbles & 0xf)}) {
                if (done >= out.size()) {
                    break;
                }
                const s32 code = nibble < 8 ? nibble : nibble - 16;
                const s64 prediction = (s64{code} * scale << 11) + 1024 +
                                       s64{coef1} * hist1 + s64{coef2} * hist2;
                const s16 sample = Clamp16(static_cast<s32>(prediction >> 11));
                out[done++] = sample;
                hist2 = hist1;
                hist1 = sample;
            }
        }
    }
    // Zero-fill anything the stored data does not cover.
    std::fill(out.begin() + done, out.end(), s16{});
}

void DecodePcm16Channel(std::span<const u8> data, const BwavChannel& channel, u32 base_start,
                        u16 channel_index, u16 channels, u32 interleave, std::span<s16> out) {
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t offset =
            base_start + InterleavedOffset(channels, interleave, channel_index, done * 2);
        if (offset + 2 > data.size()) {
            break;
        }
        u16 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        out[done++] = static_cast<s16>(value);
    }
    std::fill(out.begin() + done, out.end(), s16{});
}

// ---------------------------------------------------------------------------------------------
// NXOpus (Nintendo Switch Opus, BWAV codec 2), laid out as vgmstream reads it. Every
// BWAV channel block points at its own Opus stream: a 0x80000001 "basic info" chunk,
// then a 0x80000004 "data info" chunk whose region is a series of packets, each with a
// big-endian size in an 8-byte header.
// ---------------------------------------------------------------------------------------------

struct NxOpusStream {
    u16 channels{};
    u32 sample_rate{};
    u32 data_start{};
    u32 data_size{};
    u16 skip{};
};

u32 ReadU32BE(std::span<const u8> data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return (u32{data[offset]} << 24) | (u32{data[offset + 1]} << 16) |
           (u32{data[offset + 2]} << 8) | u32{data[offset + 3]};
}

bool ParseNxOpus(std::span<const u8> data, u32 stream_start, NxOpusStream& stream,
                 std::string& note) {
    // The offsets and sizes come straight from the file, so widen before adding:
    // a crafted chunk must not wrap a 32-bit sum past the bounds checks below.
    const std::size_t start = static_cast<std::size_t>(stream_start);
    if (start + 0x28 > data.size()) {
        note = "NXOpus header out of range";
        return false;
    }
    if (ReadU32(data, start + 0x00) != 0x80000001) {
        note = "NXOpus: missing basic info chunk";
        return false;
    }
    stream.channels = data[start + 0x09];
    stream.sample_rate = ReadU32(data, start + 0x0c);
    const u32 data_offset = ReadU32(data, start + 0x10);
    stream.skip = Read16(data, start + 0x1c);
    if (stream.channels == 0 || stream.channels > 8) {
        note = fmt::format("NXOpus: bad channel count {}", stream.channels);
        return false;
    }
    // Opus decodes at 48 kHz; games that stored another rate still play 48 kHz audio.
    if (stream.sample_rate != 48000) {
        stream.sample_rate = 48000;
    }
    const std::size_t data_chunk = start + static_cast<std::size_t>(data_offset);
    if (data_offset == 0 || data_chunk + 8 > data.size() ||
        ReadU32(data, data_chunk) != 0x80000004) {
        note = "NXOpus: missing data info chunk";
        return false;
    }
    stream.data_start = static_cast<u32>(data_chunk + 8);
    stream.data_size = ReadU32(data, data_chunk + 4);
    if (static_cast<std::size_t>(stream.data_start) + stream.data_size > data.size()) {
        note = "NXOpus: packet data out of range";
        return false;
    }
    return true;
}

bool DecodeNxOpus(std::span<const u8> data, const NxOpusStream& stream, std::span<s16> out,
                 std::string& note) {
    int opus_error = OPUS_OK;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
        opus_decoder_create(stream.sample_rate, stream.channels, &opus_error),
        &opus_decoder_destroy};
    if (!decoder || opus_error != OPUS_OK) {
        note = "NXOpus: decoder init failed";
        return false;
    }
    constexpr int kMaxPacketSamples = 5760;
    std::vector<s16> packet_pcm(static_cast<std::size_t>(kMaxPacketSamples) * stream.channels);
    std::size_t pos = stream.data_start;
    const std::size_t end = pos + stream.data_size;
    std::size_t produced = 0;
    std::size_t skipped = 0;
    while (pos + 8 <= end && produced < out.size()) {
        const u32 size = ReadU32BE(data, pos);
        pos += 8;
        if (pos + size > end) {
            note = "NXOpus: truncated packet";
            break;
        }
        const int decoded = opus_decode(decoder.get(), data.data() + pos,
                                        static_cast<opus_int32>(size), packet_pcm.data(),
                                        kMaxPacketSamples, 0);
        pos += size;
        if (decoded < 0) {
            note = fmt::format("NXOpus: decode error {}", decoded);
            break;
        }
        for (int i = 0; i < decoded * stream.channels && produced < out.size(); ++i) {
            if (skipped < stream.skip) {
                ++skipped;
                continue;
            }
            out[produced++] = packet_pcm[i];
        }
    }
    std::fill(out.begin() + produced, out.end(), s16{});
    return true;
}

struct DecodedTrack {
    std::vector<s16> pcm;
    u32 sample_rate{};
    u16 channels{};
    s64 loop_start{};
    s64 loop_end{-1};
};

bool DecodeBwav(std::span<const u8> data, DecodedTrack& out, std::string& note) {
    BwavInfo info;
    if (!ParseBwav(data, info, note)) {
        return false;
    }
    const s32 num_samples = info.channel[0].num_samples;
    if (num_samples == 0) {
        note = "no samples";
        return false;
    }
    // The header's sample count drives every allocation below, so bound it
    // before trusting it: PCM16 and DSP samples must physically fit in the
    // stream (which also rejects truncated tracks), and no track may ask for
    // more than a gigabyte of decoded audio.
    u64 needed = 0;
    for (const BwavChannel& channel : info.channel) {
        if (channel.codec == CodecPcm16) {
            needed += static_cast<u64>(num_samples) * 2;
        } else if (channel.codec == CodecDsp) {
            needed += (static_cast<u64>(num_samples) + 13) / 14 * 8;
        }
    }
    if (static_cast<u64>(num_samples) * info.channels * 2 > kMaxPcmBytes ||
        needed > data.size()) {
        note = "declared sample count exceeds the stream data";
        return false;
    }
    out.sample_rate = static_cast<u32>(info.channel[0].sample_rate);
    out.channels = info.channels;
    out.pcm.assign(static_cast<std::size_t>(num_samples) * info.channels, 0);
    // DSP and PCM channels are positioned from channel 0's data start (the per-channel
    // +0x34 fields hold the same absolute offsets, so mixing them with the interleave
    // stride would double-count it). NXOpus channels instead each own a full Opus stream
    // at their +0x34 offset, and one stream can carry several audio channels.
    const u32 base_start = info.channel[0].data_start;
    u16 track_channel = 0;
    for (u16 ch = 0; ch < info.channels; ++ch) {
        const BwavChannel& channel = info.channel[ch];
        if (track_channel >= info.channels) {
            note = "NXOpus layers exceed the channel count";
            return false;
        }
        std::vector<s16> channel_pcm;
        if (channel.codec == CodecNxOpus) {
            NxOpusStream stream;
            if (!ParseNxOpus(data, channel.data_start, stream, note)) {
                return false;
            }
            if (track_channel + stream.channels > info.channels) {
                note = "NXOpus layers exceed the channel count";
                return false;
            }
            channel_pcm.assign(static_cast<std::size_t>(num_samples) * stream.channels, 0);
            if (!DecodeNxOpus(data, stream, channel_pcm, note)) {
                return false;
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(num_samples); ++i) {
                for (u16 sub = 0; sub < stream.channels; ++sub) {
                    out.pcm[i * info.channels + track_channel + sub] =
                        channel_pcm[i * stream.channels + sub];
                }
            }
            track_channel += stream.channels;
            continue;
        }
        if (base_start >= data.size()) {
            note = fmt::format("channel {} data out of range", ch);
            return false;
        }
        channel_pcm.assign(static_cast<std::size_t>(num_samples), 0);
        if (channel.codec == CodecPcm16) {
            DecodePcm16Channel(data, channel, base_start, ch, info.channels, info.interleave,
                               channel_pcm);
        } else {
            DecodeDspChannel(data, channel, base_start, ch, info.channels, info.interleave,
                             channel_pcm);
        }
        for (std::size_t i = 0; i < channel_pcm.size(); ++i) {
            out.pcm[i * info.channels + track_channel] = channel_pcm[i];
        }
        ++track_channel;
    }
    out.loop_start = info.channel[0].loop_start;
    out.loop_end = info.channel[0].loop_end;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Clean output names and embedded metadata
// -------------------------------------------------------------------------------------

bool IsAllDigits(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c) != 0;
           });
}

// "BGM_XXX_YYY_0005" becomes folder "XXX" and title "YYY": the category markers
// are dropped by ParseName, a single trailing numeric token is peeled off as the
// cue id, the remaining tokens join with spaces, and a lone token lands directly
// in the kind folder.
struct CleanName {
    std::string folder;
    std::string title;
    // The cue id peeled off the title. It folds back in when another stream
    // cleans to the same name, so "Scene 0001" and "Scene 0002" keep their own
    // identities instead of becoming "Scene 1" and "Scene 2".
    std::string cue;
};

CleanName CleanNameFor(const std::string& entry_name) {
    std::vector<std::string> tokens = ParseName(entry_name).tokens;
    CleanName clean;
    if (tokens.size() >= 2 && IsAllDigits(tokens.back())) {
        clean.cue = tokens.back();
        tokens.pop_back();
    }
    if (tokens.empty()) {
        clean.title = EndsWithCI(entry_name, ".bwav")
                          ? entry_name.substr(0, entry_name.size() - 5)
                          : entry_name;
        return clean;
    }
    if (tokens.size() == 1) {
        clean.title = tokens[0];
        return clean;
    }
    clean.folder = tokens[0];
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (!clean.title.empty()) {
            clean.title += ' ';
        }
        clean.title += tokens[i];
    }
    return clean;
}

// What gets embedded into every written track.
struct TrackTags {
    std::string title;
    std::string artist;
    std::string album;
    std::span<const u8> cover{};
    std::string_view cover_mime{};
};

// PNG and JPEG headers carry the pixel size; the OpusTags picture block wants it.
void ImageInfo(std::span<const u8> data, std::string_view& mime, u32& width, u32& height) {
    mime = {};
    width = height = 0;
    if (data.size() >= 24 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G') {
        width = (u32{data[16]} << 24) | (u32{data[17]} << 16) | (u32{data[18]} << 8) |
                u32{data[19]};
        height = (u32{data[20]} << 24) | (u32{data[21]} << 16) | (u32{data[22]} << 8) |
                 u32{data[23]};
        mime = "image/png";
        return;
    }
    if (data.size() >= 4 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        std::size_t at = 2;
        while (at + 4 < data.size()) {
            if (data[at] != 0xFF) {
                ++at;
                continue;
            }
            const u8 marker = data[at + 1];
            const u32 length = (u32{data[at + 2]} << 8) | u32{data[at + 3]};
            const bool start_of_frame = (marker >= 0xC0 && marker <= 0xCF) &&
                                        marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (start_of_frame && at + 9 < data.size()) {
                height = (u32{data[at + 5]} << 8) | u32{data[at + 6]};
                width = (u32{data[at + 7]} << 8) | u32{data[at + 8]};
                mime = "image/jpeg";
                return;
            }
            at += 2 + length;
        }
    }
}

// ID3v2.4 with synch-safe sizes, written into the WAV "id3 " chunk.
void PutSynchsafe(std::string& out, u32 value) {
    out.push_back(static_cast<char>((value >> 21) & 0x7F));
    out.push_back(static_cast<char>((value >> 14) & 0x7F));
    out.push_back(static_cast<char>((value >> 7) & 0x7F));
    out.push_back(static_cast<char>(value & 0x7F));
}

void PutId3TextFrame(std::string& out, std::string_view id, std::string_view text) {
    std::string payload;
    payload.push_back('\x03'); // UTF-8
    payload.append(text);
    payload.push_back('\0');
    out.append(id);
    PutSynchsafe(out, static_cast<u32>(payload.size()));
    out.push_back('\0');
    out.push_back('\0');
    out.append(payload);
}

std::string MakeId3v2(const TrackTags& tags) {
    std::string frames;
    if (!tags.title.empty()) {
        PutId3TextFrame(frames, "TIT2", tags.title);
    }
    if (!tags.artist.empty()) {
        PutId3TextFrame(frames, "TPE1", tags.artist);
    }
    if (!tags.album.empty()) {
        PutId3TextFrame(frames, "TALB", tags.album);
    }
    if (!tags.cover.empty() && !tags.cover_mime.empty()) {
        std::string payload;
        payload.push_back('\x03'); // UTF-8
        payload.append(tags.cover_mime);
        payload.push_back('\0');
        payload.push_back('\x03'); // front cover
        payload.push_back('\0');  // empty description
        payload.append(reinterpret_cast<const char*>(tags.cover.data()), tags.cover.size());
        frames.append("APIC");
        PutSynchsafe(frames, static_cast<u32>(payload.size()));
        frames.push_back('\0');
        frames.push_back('\0');
        frames.append(payload);
    }
    std::string out = "ID3";
    out.push_back('\x04');
    out.push_back('\x00');
    out.push_back('\x00');
    PutSynchsafe(out, static_cast<u32>(frames.size()));
    out.append(frames);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Output writers
// -------------------------------------------------------------------------------------

bool WriteWav(const std::filesystem::path& path, const DecodedTrack& track,
              const TrackTags& tags) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::string id3 = MakeId3v2(tags);
    const u32 id3_chunk = static_cast<u32>(id3.size()) + (id3.size() % 2);
    const u32 data_size = static_cast<u32>(track.pcm.size() * sizeof(s16));
    // Everything after the RIFF header: "WAVE", the fmt chunk, the data chunk
    // and the id3 chunk.
    const u32 riff_size = 4 + (8 + 16) + (8 + data_size) + (8 + id3_chunk);
    const u32 byte_rate = track.sample_rate * track.channels * 2;
    const u16 block_align = track.channels * 2;
    constexpr u16 format = 1;
    constexpr u32 fmt_size = 16;
    constexpr u16 bits = 16;
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riff_size), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmt_size), 4);
    out.write(reinterpret_cast<const char*>(&format), 2);
    out.write(reinterpret_cast<const char*>(&track.channels), 2);
    out.write(reinterpret_cast<const char*>(&track.sample_rate), 4);
    out.write(reinterpret_cast<const char*>(&byte_rate), 4);
    out.write(reinterpret_cast<const char*>(&block_align), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_size), 4);
    out.write(reinterpret_cast<const char*>(track.pcm.data()),
              static_cast<std::streamsize>(data_size));
    if (!id3.empty()) {
        const u32 id3_size = static_cast<u32>(id3.size());
        out.write("id3 ", 4);
        out.write(reinterpret_cast<const char*>(&id3_size), 4);
        out.write(id3.data(), static_cast<std::streamsize>(id3.size()));
        if (id3_chunk > id3_size) {
            out.write("\0", 1);
        }
    }
    // The final flush can still fail (disk full); check it, not just the writes.
    out.close();
    return out.good();
}

// -------------------------------------------------------------------------------------------
// Ogg Opus output (RFC 7845). The encoder always runs at 48 kHz - Opus is a 48 kHz
// codec - and the stream's original rate is kept in the OpusHead for reference.
// -------------------------------------------------------------------------------------

constexpr u32 OpusRate = 48000;
constexpr std::size_t OpusFrameSamples = 960; // 20 ms

void PutLE16(std::string& out, u16 value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>(value >> 8));
}

void PutLE32(std::string& out, u32 value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFF));
    }
}

// The Ogg page CRC: the non-reflected CRC-32 with polynomial 0x04c11db7, no final
// xor, computed over the header with the CRC field zeroed plus the page body.
u32 OggCrcUpdate(u32 crc, const u8* data, std::size_t size) {
    static const auto table = [] {
        std::array<u32, 256> out{};
        for (u32 i = 0; i < 256; ++i) {
            u32 value{i << 24};
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 0x80000000) != 0 ? (value << 1) ^ 0x04c11db7 : value << 1;
            }
            out[i] = value;
        }
        return out;
    }();
    for (std::size_t i = 0; i < size; ++i) {
        crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];
    }
    return crc;
}

// Pages packets out to a file (RFC 3533). A packet bigger than a page's lacing
// capacity - the tags packet when the cover art is large - is split across
// pages with the continued flag; audio packets (20 ms each) never span.
class OggWriter {
public:
    OggWriter(std::ofstream& out, u32 serial, u16 preskip)
        : out(out), serial(serial), preskip(preskip) {}

    // A header packet (OpusHead, OpusTags); its pages carry granule 0.
    bool WriteHeader(std::string_view packet) {
        header_pages = true;
        return Place(reinterpret_cast<const u8*>(packet.data()), packet.size()) && FlushPage();
    }

    // A whole audio packet; a page closes once its body fills up, its granule
    // counting the samples of every packet completed so far.
    bool AddAudio(std::string_view packet, std::size_t samples) {
        header_pages = false;
        pending_samples += static_cast<s64>(samples);
        if (!Place(reinterpret_cast<const u8*>(packet.data()), packet.size())) {
            return false;
        }
        samples_done += pending_samples;
        pending_samples = 0;
        return body.size() < 32 * 1024 || FlushPage();
    }

    bool Finish() {
        return FlushPage(true);
    }

private:
    // Lacing values for the whole packet: runs of 255 closed by a remainder
    // (0 when the size is a whole multiple of 255). A packet larger than a
    // page's 255-entry lacing table spans pages, the continued flag set on
    // each page that starts inside it.
    bool Place(const u8* data, std::size_t size) {
        std::vector<u8> values;
        std::size_t left = size;
        while (left >= 255) {
            values.push_back(255);
            left -= 255;
        }
        values.push_back(static_cast<u8>(left));
        open_packet = true;
        std::size_t at = 0;
        for (const u8 value : values) {
            if (lacing.size() == 255 && !FlushPage()) {
                return false;
            }
            lacing.push_back(value);
            body.append(reinterpret_cast<const char*>(data + at), value);
            at += value;
        }
        open_packet = false;
        return out.good();
    }

    bool FlushPage(bool eos = false) {
        u8 flags = 0;
        if (first_page) {
            flags |= 0x02;
        }
        if (page_starts_continued) {
            flags |= 0x01;
        }
        if (eos) {
            flags |= 0x04;
        }
        const s64 granule = header_pages ? 0 : preskip + samples_done;
        std::array<u8, 27> header{};
        std::memcpy(header.data(), "OggS", 4);
        header[5] = flags;
        std::memcpy(header.data() + 6, &granule, sizeof(granule));
        std::memcpy(header.data() + 14, &serial, sizeof(serial));
        std::memcpy(header.data() + 18, &page_sequence, sizeof(page_sequence));
        header[26] = static_cast<u8>(lacing.size());
        // The CRC covers the header with the CRC field zeroed, then the lacing table
        // and the body (RFC 3533 6.1).
        u32 crc = OggCrcUpdate(0, header.data(), header.size());
        crc = OggCrcUpdate(crc, lacing.data(), lacing.size());
        crc = OggCrcUpdate(crc, reinterpret_cast<const u8*>(body.data()), body.size());
        std::memcpy(header.data() + 22, &crc, sizeof(crc));
        out.write(reinterpret_cast<const char*>(header.data()),
                  static_cast<std::streamsize>(header.size()));
        out.write(reinterpret_cast<const char*>(lacing.data()),
                  static_cast<std::streamsize>(lacing.size()));
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        ++page_sequence;
        lacing.clear();
        body.clear();
        first_page = false;
        page_starts_continued = open_packet;
        return out.good();
    }

    std::ofstream& out;
    u32 serial;
    u16 preskip;
    bool header_pages{true};
    bool open_packet{false};
    bool page_starts_continued{false};
    s64 samples_done{};
    s64 pending_samples{};
    u32 page_sequence{};
    bool first_page{true};
    std::vector<u8> lacing;
    std::string body;
};

// Linear interpolation on the way up, box averaging on the way down: enough for
// feeding a lossy encoder whose sources are DSP- or Opus-compressed already.
std::vector<s16> ResampleTo48k(const std::vector<s16>& pcm, u16 channels, u32 from_rate) {
    if (from_rate == OpusRate || channels == 0 || pcm.empty()) {
        return pcm;
    }
    const std::size_t frames = pcm.size() / channels;
    if (frames == 0) {
        return pcm;
    }
    const double ratio = static_cast<double>(OpusRate) / from_rate;
    const std::size_t out_frames =
        static_cast<std::size_t>(std::llround(static_cast<double>(frames) * ratio));
    std::vector<s16> out(out_frames * channels);
    if (ratio > 1.0) {
        const double step = 1.0 / ratio;
        for (std::size_t frame = 0; frame < out_frames; ++frame) {
            const double pos = frame * step;
            const std::size_t left =
                std::min<std::size_t>(static_cast<std::size_t>(pos), frames - 1);
            const std::size_t right = std::min(left + 1, frames - 1);
            const double frac = std::min(pos - left, 1.0);
            for (u16 ch = 0; ch < channels; ++ch) {
                const double a = pcm[left * channels + ch];
                const double b = pcm[right * channels + ch];
                out[frame * channels + ch] =
                    static_cast<s16>(std::lround(a + (b - a) * frac));
            }
        }
    } else {
        const double step = 1.0 / ratio;
        for (std::size_t frame = 0; frame < out_frames; ++frame) {
            const std::size_t begin =
                std::min<std::size_t>(static_cast<std::size_t>(frame * step), frames - 1);
            const std::size_t end =
                std::min<std::size_t>(static_cast<std::size_t>((frame + 1) * step) + 1, frames);
            for (u16 ch = 0; ch < channels; ++ch) {
                s64 sum = 0;
                for (std::size_t i = begin; i < end; ++i) {
                    sum += pcm[i * channels + ch];
                }
                out[frame * channels + ch] =
                    static_cast<s16>(std::lround(static_cast<double>(sum) / (end - begin)));
            }
        }
    }
    return out;
}

std::string Base64(const std::string& data) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    u32 buffer = 0;
    int bits = 0;
    for (const char c : data) {
        buffer = (buffer << 8) | static_cast<u8>(c);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out.push_back(alphabet[(buffer >> bits) & 0x3F]);
        }
    }
    if (bits > 0) {
        out.push_back(alphabet[(buffer << (6 - bits)) & 0x3F]);
    }
    while (out.size() % 4 != 0) {
        out.push_back('=');
    }
    return out;
}

// The OpusTags cover picture uses the FLAC picture block layout inside base64.
std::string MakePictureBlock(std::span<const u8> cover, std::string_view mime, u32 width,
                             u32 height) {
    std::string out;
    const auto put_be32 = [&out](u32 value) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            out.push_back(static_cast<char>((value >> shift) & 0xFF));
        }
    };
    put_be32(3); // front cover
    put_be32(static_cast<u32>(mime.size()));
    out.append(mime);
    put_be32(0); // empty description
    put_be32(width);
    put_be32(height);
    put_be32(24); // depth
    put_be32(0);  // colors
    put_be32(static_cast<u32>(cover.size()));
    out.append(reinterpret_cast<const char*>(cover.data()), cover.size());
    return out;
}

std::string MakeOpusTags(const TrackTags& tags) {
    std::string out = "OpusTags";
    const std::string_view vendor = "Eglantine music extraction";
    PutLE32(out, static_cast<u32>(vendor.size()));
    out.append(vendor);
    std::vector<std::string> comments;
    const auto add = [&comments](std::string_view name, const std::string& value) {
        if (!value.empty()) {
            comments.push_back(fmt::format("{}={}", name, value));
        }
    };
    add("TITLE", tags.title);
    add("ARTIST", tags.artist);
    add("ALBUM", tags.album);
    if (!tags.cover.empty()) {
        std::string_view mime;
        u32 width{};
        u32 height{};
        ImageInfo(tags.cover, mime, width, height);
        if (!mime.empty()) {
            comments.push_back("METADATA_BLOCK_PICTURE=" +
                              Base64(MakePictureBlock(tags.cover, mime, width, height)));
        }
    }
    PutLE32(out, static_cast<u32>(comments.size()));
    for (const std::string& comment : comments) {
        PutLE32(out, static_cast<u32>(comment.size()));
        out.append(comment);
    }
    return out;
}

bool WriteOpus(const std::filesystem::path& path, const DecodedTrack& track,
               const TrackTags& tags) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::vector<s16> pcm = ResampleTo48k(track.pcm, track.channels, track.sample_rate);
    const std::size_t frames = track.channels == 0 ? 0 : pcm.size() / track.channels;
    if (frames == 0) {
        return false;
    }

    // Mono and stereo through the plain encoder; wider layouts as uncoupled
    // multistream so every channel survives.
    int opus_error = OPUS_OK;
    using EncoderPtr = std::unique_ptr<OpusEncoder, void (*)(OpusEncoder*)>;
    using MsEncoderPtr = std::unique_ptr<OpusMSEncoder, void (*)(OpusMSEncoder*)>;
    EncoderPtr encoder{nullptr, &opus_encoder_destroy};
    MsEncoderPtr ms_encoder{nullptr, &opus_multistream_encoder_destroy};
    std::vector<unsigned char> mapping;
    if (track.channels <= 2) {
        encoder.reset(opus_encoder_create(OpusRate, track.channels, OPUS_APPLICATION_AUDIO,
                                           &opus_error));
    } else {
        mapping.resize(track.channels);
        for (u16 ch = 0; ch < track.channels; ++ch) {
            mapping[ch] = static_cast<unsigned char>(ch);
        }
        ms_encoder.reset(opus_multistream_encoder_create(OpusRate, track.channels,
                                                         track.channels, 0, mapping.data(),
                                                         OPUS_APPLICATION_AUDIO, &opus_error));
    }
    if (opus_error != OPUS_OK || (!encoder && !ms_encoder)) {
        return false;
    }
    // Game audio through a lossy encoder: 96 kbps stereo keeps the mix transparent
    // at a fraction of the WAV size.
    const opus_int32 bitrate = track.channels == 1   ? 64000
                               : track.channels == 2 ? 96000
                                                    : 48000 * track.channels;
    opus_int32 lookahead = 0;
    if (encoder) {
        opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(bitrate));
        opus_encoder_ctl(encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    } else {
        opus_multistream_encoder_ctl(ms_encoder.get(), OPUS_SET_BITRATE(bitrate));
        opus_multistream_encoder_ctl(ms_encoder.get(), OPUS_GET_LOOKAHEAD(&lookahead));
    }
    const u16 preskip = static_cast<u16>(std::clamp<opus_int32>(lookahead, 0, 0xFFFF));

    std::string head = "OpusHead";
    head.push_back('\x01'); // version
    head.push_back(static_cast<char>(track.channels));
    PutLE16(head, preskip);
    PutLE32(head, track.sample_rate); // informational
    PutLE16(head, 0);                 // output gain
    if (mapping.empty()) {
        head.push_back('\x00');
    } else {
        // Family 1: uncoupled streams, one per channel, identity mapping.
        head.push_back('\x01');
        head.push_back(static_cast<char>(mapping.size()));
        head.push_back('\x00');
        head.append(reinterpret_cast<const char*>(mapping.data()), mapping.size());
    }

    // Players trim through the granule position, so one stream's serial is enough.
    const u32 serial =
        static_cast<u32>(Common::CityHash64(tags.title.data(), tags.title.size()) ^ pcm.size()) |
        1;
    OggWriter writer(out, serial, preskip);
    if (!writer.WriteHeader(head) || !writer.WriteHeader(MakeOpusTags(tags))) {
        return false;
    }

    std::vector<u8> packet(1275 * track.channels + 16);
    std::vector<s16> frame(OpusFrameSamples * track.channels);
    for (std::size_t start = 0; start < frames; start += OpusFrameSamples) {
        const std::size_t count = std::min(OpusFrameSamples, frames - start);
        // The tail is zero-padded into a whole 20 ms frame; the granule position
        // keeps the true sample count so the padding never plays.
        std::fill(frame.begin(), frame.end(), s16{});
        std::memcpy(frame.data(), pcm.data() + start * track.channels,
                    count * track.channels * sizeof(s16));
        const opus_int32 size =
            encoder != nullptr
                ? opus_encode(encoder.get(), frame.data(),
                              static_cast<opus_int32>(OpusFrameSamples), packet.data(),
                              static_cast<opus_int32>(packet.size()))
                : opus_multistream_encode(ms_encoder.get(), frame.data(),
                                          static_cast<opus_int32>(OpusFrameSamples),
                                          packet.data(), static_cast<opus_int32>(packet.size()));
        if (size <= 0) {
            return false;
        }
        if (!writer.AddAudio(std::string_view{reinterpret_cast<const char*>(packet.data()),
                                               static_cast<std::size_t>(size)},
                             count)) {
            return false;
        }
    }
    const bool ok = writer.Finish();
    // The final flush can still fail (disk full); check it, not just the writes.
    out.close();
    return ok && out.good();
}

bool WriteEncoded(const std::filesystem::path& path, const DecodedTrack& track,
                  Encoding encoding, const TrackTags& tags) {
    if (encoding == Encoding::Opus) {
        return WriteOpus(path, track, tags);
    }
    return WriteWav(path, track, tags);
}

std::string_view ExtensionOf(Encoding encoding) {
    return encoding == Encoding::Opus ? ".opus" : ".wav";
}

std::string CsvEscape(std::string_view value) {
    if (value.find(',') == std::string_view::npos &&
        value.find('"') == std::string_view::npos && value.find('\n') == std::string_view::npos) {
        return std::string{value};
    }
    std::string out;
    out.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string Seconds(u64 samples, u32 sample_rate) {
    if (sample_rate == 0) {
        return "";
    }
    return fmt::format("{:.3f}", double(samples) / sample_rate);
}

u64 TrackDigest(const std::vector<s16>& pcm) {
    return Common::CityHash64(reinterpret_cast<const char*>(pcm.data()),
                              pcm.size() * sizeof(s16));
}

// Duplicate detection keys on the PCM plus the playback shape: the same bytes
// at a different sample rate or channel count are not the same track.
std::tuple<u64, u32, u16> TrackKey(const DecodedTrack& track) {
    return {TrackDigest(track.pcm), track.sample_rate, track.channels};
}

} // namespace

std::string_view KindName(Kind kind) {
    switch (kind) {
    case Kind::Bgm:
        return "bgm";
    case Kind::Cutscene:
        return "cutscene";
    case Kind::Title:
        return "title";
    case Kind::Segments:
        return "segments";
    case Kind::Stems:
        return "stems";
    default:
        return "";
    }
}

bool ScanMusic(const std::string& game_path, SourceInfo& info) {
    // The UI calls this on its own thread; no filesystem error may escape it.
    try {
        RealVfsFilesystem vfs;
        Collected collected;
        std::string error;
        if (!Collect(game_path, vfs, collected, error)) {
            LOG_WARNING(Loader, "Music scan of {} failed: {}", game_path, error);
            return false;
        }
        info.game_title = collected.game_title;
        info.stream_count = collected.entries.size();
        info.cover = collected.icon;
        // Replace, not accumulate: the caller may reuse one SourceInfo.
        info.kind_counts.fill(0);
        info.stream_bytes = 0;
        for (const auto& entry : collected.entries) {
            info.kind_counts[static_cast<std::size_t>(KindOf(entry.name))]++;
            info.stream_bytes += entry.file != nullptr ? entry.file->GetSize() : 0;
        }
        return true;
    } catch (const std::exception& ex) {
        LOG_WARNING(Loader, "Music scan of {} failed: {}", game_path, ex.what());
        return false;
    }
}

// The body behind the public entrypoint; static so it stays internal to this
// translation unit.
static Result ExtractMusicChecked(const std::string& game_path, const Options& options,
                                   const std::function<void(const Progress&)>& progress,
                                   const std::atomic_bool* cancel, Summary* summary_out,
                                   std::string* error_out) {
    RealVfsFilesystem vfs;
    Collected collected;
    std::string error;
    if (!Collect(game_path, vfs, collected, error)) {
        if (error_out != nullptr) {
            *error_out = error;
        }
        return Result::InvalidSource;
    }
    if (collected.entries.empty()) {
        if (error_out != nullptr) {
            *error_out = "no music streams found (looked for BGM_/Dm_/ExampleMelody_ .bwav "
                         "under Sound/Resource/Stream)";
        }
        return Result::NoMusic;
    }
    if (std::none_of(options.kinds.begin(), options.kinds.end(), [](bool on) { return on; })) {
        if (error_out != nullptr) {
            *error_out = "no track kinds selected";
        }
        return Result::NoMusic;
    }
    // Drop the streams of unselected kinds so the totals, the output names and the
    // manifest describe exactly what gets written.
    for (std::size_t index = collected.entries.size(); index-- > 0;) {
        if (!options.kinds[static_cast<std::size_t>(KindOf(collected.entries[index].name))]) {
            collected.entries.erase(collected.entries.begin() +
                                    static_cast<std::ptrdiff_t>(index));
        }
    }
    if (collected.entries.empty()) {
        if (error_out != nullptr) {
            *error_out = "no streams of the selected kinds (music, cutscene, title, "
                         "segments, stems)";
        }
        return Result::NoMusic;
    }

    // Bail before touching the destination: an already-cancelled run must not
    // truncate an existing manifest or overwrite earlier tracks.
    if (cancel != nullptr && cancel->load()) {
        return Result::Cancelled;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.out_dir, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = fmt::format("cannot create {}: {}",
                                     Common::FS::PathToUTF8String(options.out_dir), ec.message());
        }
        return Result::IoError;
    }

    Progress state;
    state.files_total = collected.entries.size();
    for (const auto& entry : collected.entries) {
        state.bytes_total += entry.file != nullptr ? entry.file->GetSize() : 0;
    }

    // Output naming: a kind folder first (Music, Cutscenes, Title, Segments,
    // Stems), an area subfolder from the first remaining token, and the stream's
    // cue id in place of a counter when several streams clean to the same name.
    struct OutputPlan {
        std::string folder;
        std::string title;
        std::string file_name;
    };
    struct CleanPlan {
        std::string folder;
        std::string title;
        std::string cue;
    };
    const std::string_view extension = ExtensionOf(options.encoding);
    std::map<std::pair<std::string, std::string>, u32> name_groups;
    std::vector<CleanPlan> cleans;
    cleans.reserve(collected.entries.size());
    for (const auto& entry : collected.entries) {
        const CleanName clean = CleanNameFor(entry.name);
        CleanPlan out;
        if (options.layout == Layout::ByKind) {
            out.folder = std::string{KindFolders[static_cast<std::size_t>(KindOf(entry.name))]};
            if (!clean.folder.empty()) {
                out.folder += '/';
                out.folder += clean.folder;
            }
        }
        out.title = std::move(clean.title);
        out.cue = std::move(clean.cue);
        // Case-folded keys: on a case-insensitive volume two spellings of one
        // name are the same file, so the collision logic must see them as one.
        ++name_groups[{Lower(out.folder), Lower(out.title)}];
        cleans.push_back(std::move(out));
    }
    std::map<std::pair<std::string, std::string>, u32> name_seen;
    std::set<std::pair<std::string, std::string>> used_names;
    std::vector<OutputPlan> plans;
    plans.reserve(cleans.size());
    for (const auto& [folder, title, cue] : cleans) {
        OutputPlan plan;
        plan.folder = folder;
        const auto key = std::make_pair(Lower(folder), Lower(title));
        std::string display = title;
        if (name_groups[key] > 1) {
            // Collisions fold the stream's own cue id back in when it has one;
            // a plain counter only when nothing distinguishes the names.
            display = cue.empty() ? fmt::format("{} {}", title, ++name_seen[key])
                                  : fmt::format("{} {}", title, cue);
        }
        // Differently-marked streams can still clean to the same display name in
        // Flat mode; number whatever is left.
        const std::string base = display;
        u32 extra = 1;
        while (!used_names.emplace(std::make_pair(Lower(folder), Lower(display))).second) {
            display = fmt::format("{} {}", base, ++extra);
        }
        plan.title = display;
        if (SafeOutputPath(plan.folder, display)) {
            plan.file_name = fmt::format("{}{}", display, extension);
        } else {
            // The dump controls its stream names; refuse anything that could
            // escape the output folder instead of writing it.
            plan.file_name.clear();
        }
        plans.push_back(std::move(plan));
    }

    // The same tags go into every track; only the title changes. The cover comes
    // from the chosen file, or from the NUP/package icon when none was chosen.
    TrackTags base_tags;
    base_tags.artist = collected.game_title.empty()
                           ? "Original OST"
                           : fmt::format("{} - Original OST", collected.game_title);
    base_tags.album = base_tags.artist;
    base_tags.cover = options.cover_override.empty() ? collected.icon : options.cover_override;
    u32 cover_width{};
    u32 cover_height{};
    ImageInfo(base_tags.cover, base_tags.cover_mime, cover_width, cover_height);

    Summary summary;
    // manifest.csv is a fresh regular file or the run fails: an unopenable or
    // linked path must not silently drop the extraction record.
    std::string manifest_error;
    std::error_code manifest_link_ec;
    const auto manifest_path = options.out_dir / "manifest.csv";
    if (std::filesystem::is_symlink(
            std::filesystem::symlink_status(manifest_path, manifest_link_ec)) &&
        !manifest_link_ec) {
        manifest_error = "manifest.csv exists as a symlink; refusing to overwrite it";
    }
    std::ofstream manifest;
    if (manifest_error.empty()) {
        manifest.open(manifest_path, std::ios::binary | std::ios::trunc);
    }
    const bool have_manifest = manifest_error.empty() && manifest.good();
    if (!have_manifest && manifest_error.empty()) {
        manifest_error =
            fmt::format("cannot create {}", Common::FS::PathToUTF8String(manifest_path));
    }
    if (have_manifest) {
        manifest << "file,output,kind,channels,sample_rate,duration_seconds,loop_start_seconds,"
                    "loop_end_seconds,digest,duplicate_of,note\n";
    }
    std::map<std::tuple<u64, u32, u16>, std::string> seen_digests;
    std::set<std::string> verified_folders;

    for (std::size_t index = 0; index < collected.entries.size(); ++index) {
        const Entry& entry = collected.entries[index];
        const OutputPlan& plan = plans[index];
        if (cancel != nullptr && cancel->load()) {
            break;
        }
        state.current = entry.name;
        if (progress) {
            progress(state);
        }

        DecodedTrack track;
        std::string note;
        u64 digest{};
        bool decoded = false;
        if (plan.file_name.empty()) {
            note = "unsafe output name (the stream name is not a valid file name)";
        } else if (entry.file == nullptr) {
            note = "file could not be opened";
        } else {
            const auto data = entry.file->ReadAllBytes();
            if (data.empty()) {
                note = "empty stream";
            } else if (DecodeBwav(data, track, note)) {
                decoded = true;
                digest = TrackDigest(track.pcm);
            }
        }

        const std::string_view kind = KindName(KindOf(entry.name));
        const std::string output_rel =
            plan.folder.empty()
                ? plan.file_name
                : plan.folder + "/" + (plan.file_name.empty() ? plan.title : plan.file_name);

        std::string duplicate_of;
        if (decoded) {
            ec.clear();
            const auto sub = plan.folder.empty()
                                 ? options.out_dir
                                 : options.out_dir / std::filesystem::path{plan.folder};
            if (!plan.folder.empty() && FolderHasSymlink(options.out_dir, plan.folder,
                                                         verified_folders)) {
                // A link inside the output tree could send the writes somewhere
                // else entirely; refuse instead of following it.
                decoded = false;
                note = "the output folder passes through a symlink";
            } else {
                std::error_code link_ec;
                if (!plan.folder.empty()) {
                    std::filesystem::create_directories(sub, ec);
                }
                if (ec) {
                    decoded = false;
                    note = fmt::format("cannot create {}: {}",
                                       Common::FS::PathToUTF8String(sub), ec.message());
                } else if (std::filesystem::is_symlink(
                               std::filesystem::symlink_status(sub / plan.file_name, link_ec)) &&
                           !link_ec) {
                    decoded = false;
                    note = "an existing symlink sits at the output path";
                } else {
                    TrackTags tags = base_tags;
                    tags.title = plan.title;
                    if (!WriteEncoded(sub / plan.file_name, track, options.encoding, tags)) {
                        decoded = false;
                        note = "write failed (is the disk full?)";
                    }
                }
            }
            if (decoded) {
                const auto seen = seen_digests.find(TrackKey(track));
                if (seen != seen_digests.end()) {
                    ++summary.duplicates;
                    duplicate_of = seen->second;
                } else {
                    seen_digests.emplace(TrackKey(track), output_rel);
                }
            }
        }

        if (decoded) {
            ++summary.files_ok;
        } else {
            ++summary.files_failed;
        }
        if (have_manifest) {
            manifest << CsvEscape(entry.name) << ',' << CsvEscape(output_rel) << ','
                     << CsvEscape(kind) << ','
                     << (decoded ? std::to_string(track.channels) : "") << ','
                     << (decoded ? std::to_string(track.sample_rate) : "") << ','
                     << (decoded ? Seconds(track.pcm.size() / track.channels, track.sample_rate)
                                 : "")
                     << ','
                     << (decoded && track.loop_end >= 0
                             ? Seconds(static_cast<u64>(track.loop_start), track.sample_rate)
                             : "")
                     << ','
                     << (decoded && track.loop_end >= 0
                             ? Seconds(static_cast<u64>(track.loop_end), track.sample_rate)
                             : "")
                     << ',' << fmt::format("{:016x}", digest) << ','
                     << CsvEscape(duplicate_of) << ',' << CsvEscape(note) << '\n';
            if (!manifest.good() && manifest_error.empty()) {
                manifest_error = "failed writing manifest.csv";
            }
        }

        state.files_done++;
        state.bytes_done += entry.file != nullptr ? entry.file->GetSize() : 0;
        if (progress) {
            progress(state);
        }
    }

    if (have_manifest) {
        manifest.close();
        if (!manifest.good() && manifest_error.empty()) {
            manifest_error = "failed writing manifest.csv";
        }
    }
    if (summary_out != nullptr) {
        *summary_out = summary;
    }
    if (cancel != nullptr && cancel->load()) {
        return Result::Cancelled;
    }
    if (summary.files_ok == 0) {
        if (error_out != nullptr) {
            *error_out = manifest_error.empty()
                             ? "every stream failed to decode (see manifest.csv)"
                             : manifest_error;
        }
        return Result::IoError;
    }
    if (!manifest_error.empty()) {
        if (error_out != nullptr) {
            *error_out = manifest_error;
        }
        return Result::IoError;
    }
    return Result::Success;
}

Result ExtractMusic(const std::string& game_path, const Options& options,
                    const std::function<void(const Progress&)>& progress,
                    const std::atomic_bool* cancel, Summary* summary_out,
                    std::string* error_out) {
    // The UI runs this inside a thread pool, where an uncaught exception
    // terminates the app; the checked body may still see one from allocation
    // or anything the error-code conversions above missed.
    try {
        return ExtractMusicChecked(game_path, options, progress, cancel, summary_out,
                                   error_out);
    } catch (const std::exception& ex) {
        if (error_out != nullptr) {
            *error_out = ex.what();
        }
        return Result::IoError;
    }
}

} // namespace FileSys::Music
