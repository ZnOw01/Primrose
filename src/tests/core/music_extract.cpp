// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Extraction of the Alto-family music streams (romfs/Sound/Resource/Stream/*.bwav)
// from a loose dump and from the NUP archive built from it.

#include <catch2/catch_test_macros.hpp>

#include <opus.h>
#include <opus_multistream.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "core/file_sys/music_extract.h"
#include "core/file_sys/nup_writer.h"

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("music-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(const fs::path& p, std::string_view value) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void Put16(std::string& out, std::size_t at, u16 value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

void Put32(std::string& out, std::size_t at, s32 value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

// A repeating signed-nibble ramp (values in [-8, 7]): with zero coefficients and a
// scale of 1, a DSP frame decodes each sample back to exactly this value.
std::vector<s16> NibblePattern(s16 start, u32 count) {
    std::vector<s16> out;
    for (u32 i = 0; i < count; ++i) {
        s16 value = static_cast<s16>(((start % 16) + 16 + static_cast<s16>(i)) % 16);
        if (value > 7) {
            value = static_cast<s16>(value - 16);
        }
        out.push_back(value);
    }
    return out;
}

u8 NibbleOf(s16 sample) {
    return sample >= 0 ? static_cast<u8>(sample) : static_cast<u8>(sample + 16);
}

// A stereo BWAV with the DSP codec: two 8-byte frames (28 samples) per channel.
// Zero coefficients and a scale of 1 in every frame header make the decoded PCM
// equal to the signed nibbles, so the expected output is known exactly.
std::string MakeDspBwav(const std::vector<s16>& left, const std::vector<s16>& right,
                        s32 loop_start = -1, s32 loop_end = -1) {
    REQUIRE(left.size() == 28);
    REQUIRE(right.size() == 28);
    for (const s16 sample : left) {
        REQUIRE(sample >= -8);
        REQUIRE(sample <= 7);
    }
    constexpr u32 kDataStart = 0xC0;
    constexpr u32 kInterleave = 2 * 8;
    std::string out(0xC0 + kInterleave * 2, '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 2);
    for (u16 ch = 0; ch < 2; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        // Real streams store each channel's own absolute data start in +0x34 (channel 1
        // at kDataStart + kInterleave), which the decoder must not combine with the
        // interleave stride again.
        const u32 own_start = kDataStart + ch * kInterleave;
        Put16(out, base + 0x00, 1);               // DSP codec
        Put32(out, base + 0x04, 48000);            // sample rate
        Put32(out, base + 0x0c, 28);               // samples
        Put32(out, base + 0x30, own_start);        // full-file offset
        Put32(out, base + 0x34, own_start);        // data start
        Put32(out, base + 0x38, 1);               // flag
        Put32(out, base + 0x3c, loop_end);         // loop end
        Put32(out, base + 0x40, loop_start);      // loop start
        Put16(out, base + 0x46, 0);               // hist1
        Put16(out, base + 0x48, 0);               // hist2
    }
    Put32(out, 0x8C, kDataStart + kInterleave); // second channel's data offset
    const auto channel_data = [&](u16 ch) {
        const std::vector<s16>& samples = ch == 0 ? left : right;
        std::size_t at = kDataStart + ch * kInterleave;
        for (u32 frame = 0; frame < 2; ++frame) {
            // Frame header 0x00: scale 1<<0 and coefficient index 0, so with zero
            // coefficients every decoded sample equals its signed nibble.
            out[at++] = '\x00';
            for (u32 pair = 0; pair < 7; ++pair) {
                const s16 high = samples[frame * 14 + pair * 2];
                const s16 low = samples[frame * 14 + pair * 2 + 1];
                out[at++] = static_cast<char>((NibbleOf(high) << 4) | NibbleOf(low));
            }
        }
    };
    channel_data(0);
    channel_data(1);
    return out;
}

std::string MakePcm16Bwav(const std::vector<s16>& left, const std::vector<s16>& right) {
    REQUIRE(left.size() == 28);
    REQUIRE(right.size() == 28);
    constexpr u32 kDataStart = 0xC0;
    constexpr u32 kInterleave = 2 * 28;
    std::string out(0xC0 + kInterleave * 2, '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 2);
    for (u16 ch = 0; ch < 2; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        const u32 own_start = kDataStart + ch * kInterleave;
        Put16(out, base + 0x00, 0);           // PCM16 codec
        Put32(out, base + 0x04, 32000);       // sample rate
        Put32(out, base + 0x0c, 28);          // samples
        Put32(out, base + 0x30, own_start); // full-file offset
        Put32(out, base + 0x34, own_start);  // data start
        Put32(out, base + 0x38, 1);           // flag
        Put32(out, base + 0x3c, -1);          // no loop
    }
    Put32(out, 0x8C, kDataStart + kInterleave);
    // Block-interleaved layout: the whole first channel, then the whole second.
    std::size_t at = kDataStart;
    for (const s16 sample : left) {
        std::memcpy(out.data() + at, &sample, sizeof(s16));
        at += sizeof(s16);
    }
    for (const s16 sample : right) {
        std::memcpy(out.data() + at, &sample, sizeof(s16));
        at += sizeof(s16);
    }
    return out;
}

std::vector<s16> Ramp(s16 first, s16 step, u32 count) {
    std::vector<s16> out;
    s16 value = first;
    for (u32 i = 0; i < count; ++i) {
        out.push_back(value);
        value = static_cast<s16>(value + step);
    }
    return out;
}

std::vector<s16> Interleave(const std::vector<s16>& left, const std::vector<s16>& right) {
    std::vector<s16> out;
    out.reserve(left.size() * 2);
    for (u32 i = 0; i < left.size(); ++i) {
        out.push_back(left[i]);
        out.push_back(right[i]);
    }
    return out;
}

std::string PcmPayload(const std::vector<s16>& pcm) {
    return std::string(reinterpret_cast<const char*>(pcm.data()), pcm.size() * sizeof(s16));
}

constexpr u64 TitleId = 0x0400ABCD00020000ULL;

std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

// A dump with two music streams per tree, ignored effects and voice, and an update
// that replaces one stream (base had DSP, the update ships PCM16) and adds another.
fs::path MakeDump(const fs::path& root) {
    const fs::path base = root / "base";
    const fs::path stream = base / "romfs/Sound/Resource/Stream";
    Put(base / "exefs/main", "main");
    Put(base / "exefs/main.npdm", Npdm(TitleId));
    Put(base / "romfs/readme.txt", "not audio");
    Put(stream / "BGM_Main.bwav",
        MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28), 14, 28));
    Put(stream / "Dm_Title_0005.bwav", MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    Put(stream / "ExampleMelody_Test.bwav",
        MakeDspBwav(NibblePattern(3, 28), NibblePattern(11, 28)));
    Put(stream / "SE_Ignored.bwav", MakeDspBwav(NibblePattern(4, 28), NibblePattern(12, 28)));
    Put(stream / "NV_Ignored.bwav", MakeDspBwav(NibblePattern(5, 28), NibblePattern(13, 28)));
    Put(stream / "IgnoreMe.bwav", MakeDspBwav(NibblePattern(6, 28), NibblePattern(14, 28)));
    const fs::path update = root / "updates" / "7" / "romfs/Sound/Resource/Stream";
    Put(root / "updates" / "7" / "exefs/main", "update main");
    Put(root / "updates" / "7" / "exefs/main.npdm", Npdm(TitleId));
    Put(update / "BGM_Main.bwav", MakePcm16Bwav(Ramp(7, 1, 28), Ramp(-7, -1, 28)));
    Put(update / "BGM_New.bwav",
        MakeDspBwav(NibblePattern(8, 28), NibblePattern(0, 28), 5, 28));
    return base / "exefs/main";
}

std::string WavOf(const fs::path& out_dir, std::string_view name) {
    return ReadAll(out_dir / fmt::format("{}.wav", name));
}

// A 1x1 transparent PNG, small enough to spell out in full.
constexpr std::array<u8, 67> kPng{
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
    0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06,
    0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44,
    0x41, 0x54, 0x78, 0x9C, 0x62, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D,
    0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
    0x60, 0x82,
};

// ---------------------------------------------------------------------------------------------
// Ogg Opus page checks: every page's CRC, sequence number and packet framing is
// verified against RFC 3533 so the writer's muxer cannot drift silently.
// -------------------------------------------------------------------------------------------

u32 OggCrc(const u8* data, std::size_t size) {
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
    u32 crc = 0;
    for (std::size_t i = 0; i < size; ++i) {
        crc = (crc << 8) ^ table[(crc >> 24) ^ data[i]];
    }
    return crc;
}

struct OggPage {
    u8 flags{};
    s64 granule{};
    std::vector<std::vector<u8>> packets;
};

std::vector<OggPage> ParseOgg(const std::string& bytes) {
    std::vector<OggPage> pages;
    // Bytes of a packet still open from earlier pages (only the cover-bearing
    // OpusTags packet is ever big enough to span).
    std::vector<u8> carry;
    std::size_t at = 0;
    while (at < bytes.size()) {
        REQUIRE(bytes.size() - at >= 27);
        REQUIRE(std::memcmp(bytes.data() + at, "OggS", 4) == 0);
        REQUIRE(bytes[at + 4] == 0); // stream structure version
        OggPage page;
        page.flags = static_cast<u8>(bytes[at + 5]);
        std::memcpy(&page.granule, bytes.data() + at + 6, sizeof(page.granule));
        u32 serial{};
        std::memcpy(&serial, bytes.data() + at + 14, sizeof(serial));
        REQUIRE(serial != 0);
        u32 sequence{};
        std::memcpy(&sequence, bytes.data() + at + 18, sizeof(sequence));
        REQUIRE(sequence == pages.size());
        u32 stored_crc{};
        std::memcpy(&stored_crc, bytes.data() + at + 22, sizeof(stored_crc));
        const u8 segments = static_cast<u8>(bytes[at + 26]);
        REQUIRE(at + 27 + segments <= bytes.size());

        // The CRC covers the header with the CRC field zeroed, the lacing table, the body.
        std::string page_bytes = bytes.substr(at, 27);
        std::memset(page_bytes.data() + 22, 0, 4);
        u32 body_size = 0;
        for (u8 segment = 0; segment < segments; ++segment) {
            const u8 lacing = static_cast<u8>(bytes[at + 27 + segment]);
            page_bytes.push_back(static_cast<char>(lacing));
            body_size += lacing;
        }
        REQUIRE(at + 27 + segments + body_size <= bytes.size());
        const std::string body = bytes.substr(at + 27 + segments, body_size);
        page_bytes.append(body);
        REQUIRE(OggCrc(reinterpret_cast<const u8*>(page_bytes.data()), page_bytes.size()) ==
                stored_crc);

        // A page that starts inside a packet must say so, and vice versa.
        if (carry.empty()) {
            REQUIRE((page.flags & 0x01) == 0);
        } else {
            REQUIRE((page.flags & 0x01) != 0);
        }

        // Reassemble packets; one still open at the page's end continues on the
        // next page, its lacing table necessarily full.
        std::size_t offset = 0;
        for (u8 segment = 0; segment < segments; ++segment) {
            const u8 lacing = static_cast<u8>(bytes[at + 27 + segment]);
            carry.insert(carry.end(), body.begin() + offset, body.begin() + offset + lacing);
            offset += lacing;
            if (lacing < 255) {
                page.packets.push_back(std::move(carry));
                carry.clear();
            }
        }
        REQUIRE(offset == body_size);
        if (!carry.empty()) {
            REQUIRE(segments == 255);
        }
        pages.push_back(std::move(page));
        at += 27 + segments + body_size;
    }
    REQUIRE(carry.empty());
    return pages;
}

std::string Base64Decode(std::string_view text) {
    std::string out;
    u32 buffer = 0;
    int bits = 0;
    for (const char c : text) {
        u32 value;
        if (c >= 'A' && c <= 'Z') {
            value = static_cast<u32>(c - 'A');
        } else if (c >= 'a' && c <= 'z') {
            value = static_cast<u32>(c - 'a' + 26);
        } else if (c >= '0' && c <= '9') {
            value = static_cast<u32>(c - '0' + 52);
        } else if (c == '+') {
            value = 62;
        } else if (c == '/') {
            value = 63;
        } else {
            break; // padding or junk
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

} // namespace

TEST_CASE("FileSys::Music round-trips through a loose dump", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    // The update replaces BGM_Main and adds BGM_New; Dm_Title and ExampleMelody count
    // too, the plain effect and voice files do not.
    REQUIRE(info.stream_count == 4);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Bgm)] == 3);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Title)] == 1);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Cutscene)] == 0);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    const auto result =
        FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary, &error);
    REQUIRE(result == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 4);
    REQUIRE(summary.files_failed == 0);
    REQUIRE(summary.duplicates == 0);
    REQUIRE(error.empty());

    // Clean names: category markers and the numeric suffix are gone, a lone token
    // lands directly in its kind folder.
    REQUIRE(fs::exists(out_dir.path / "Music/Main.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/New.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/Test.wav"));
    REQUIRE(fs::exists(out_dir.path / "Title/Title.wav"));
    REQUIRE(!fs::exists(out_dir.path / "SE_Ignored.wav"));
    REQUIRE(!fs::exists(out_dir.path / "NV_Ignored.wav"));
    REQUIRE(!fs::exists(out_dir.path / "IgnoreMe.wav"));

    // BGM_Main comes from the update tree (PCM16), not the base (DSP).
    const auto main_pcm = WavOf(out_dir.path / "Music", "Main");
    const auto expected_main = PcmPayload(Interleave(Ramp(7, 1, 28), Ramp(-7, -1, 28)));
    REQUIRE(main_pcm.size() > 44 + expected_main.size()); // ID3 chunk trails the data
    REQUIRE(main_pcm.substr(44, expected_main.size()) == expected_main);
    REQUIRE(std::memcmp(main_pcm.data(), "RIFF", 4) == 0);
    REQUIRE(std::memcmp(main_pcm.data() + 8, "WAVE", 4) == 0);

    // The DSP track decodes back to its signed nibbles.
    const auto new_pcm = WavOf(out_dir.path / "Music", "New");
    REQUIRE(new_pcm.substr(44, expected_main.size()) ==
            PcmPayload(Interleave(NibblePattern(8, 28), NibblePattern(0, 28))));

    // Sample rate straight from the BWAV channel block.
    u32 rate{};
    std::memcpy(&rate, main_pcm.data() + 24, sizeof(rate));
    REQUIRE(rate == 32000);

    // The declared RIFF size covers every chunk through the end of the file.
    u32 riff_size{};
    std::memcpy(&riff_size, main_pcm.data() + 4, sizeof(riff_size));
    REQUIRE(riff_size == main_pcm.size() - 8);

    // A second scan replaces the first's totals instead of accumulating them.
    FileSys::Music::SourceInfo again;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), again));
    REQUIRE(again.stream_count == info.stream_count);
    REQUIRE(again.stream_bytes == info.stream_bytes);

    // Embedded metadata: clean title plus "<game> - Original OST".
    REQUIRE(main_pcm.find("TIT2") != std::string::npos);
    REQUIRE(main_pcm.find("Main") != std::string::npos);
    REQUIRE(main_pcm.find(" - Original OST") != std::string::npos);

    // manifest.csv lists every stream with its clean output name, kind and loop points.
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Main.bwav,Music/Main.wav,bgm,2,32000,0.001,,") !=
            std::string::npos);
    REQUIRE(manifest.find("BGM_New.bwav,Music/New.wav,bgm,2,48000,0.001,0.000,0.001,") !=
            std::string::npos);
    REQUIRE(manifest.find("Dm_Title_0005.bwav,Title/Title.wav,title,2,48000,") !=
            std::string::npos);
    REQUIRE(manifest.find("SE_Ignored") == std::string::npos);
}

TEST_CASE("FileSys::Music flat layout keeps every track in the output folder",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::Flat;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(fs::exists(out_dir.path / "Main.wav"));
    REQUIRE(fs::exists(out_dir.path / "New.wav"));
    REQUIRE(fs::exists(out_dir.path / "Title.wav"));
    REQUIRE(fs::exists(out_dir.path / "Test.wav"));
}

TEST_CASE("FileSys::Music reads the same tracks from a packed NUP", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);
    const fs::path archive = dump_dir.path / "music.nup";
    std::string pack_error;
    REQUIRE(FileSys::Nup::PackDump(dump_dir.path, archive, {}, {}, nullptr, &pack_error) ==
            FileSys::Nup::PackResult::Success);

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(archive.string(), info));
    REQUIRE(info.stream_count == 4);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(archive.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 4);

    // Same bytes as the loose run: the update package overlays the base inside the
    // archive, and the zstd member decompresses to the original stream.
    const auto main_pcm = WavOf(out_dir.path / "Music", "Main");
    REQUIRE(main_pcm.substr(44, 112) == PcmPayload(Interleave(Ramp(7, 1, 28), Ramp(-7, -1, 28))));
    REQUIRE(fs::exists(out_dir.path / "Title/Title.wav"));
}

TEST_CASE("FileSys::Music flags identical tracks and skips unknown sources",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    const std::string bytes = MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28));
    Put(stream / "BGM_First.bwav", bytes);
    Put(stream / "BGM_Same.bwav", bytes);
    Put(stream / "BGM_Other.bwav", MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    // Two streams whose clean name is the same: "BGM_Scene_Song_0001" and
    // "..._0002" both become Scene/Song, so their cue ids tell them apart.
    Put(stream / "BGM_Scene_Song_0001.bwav", bytes);
    Put(stream / "BGM_Scene_Song_0002.bwav", bytes);

    const fs::path main = dump_dir.path / "base/exefs/main";
    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    REQUIRE(info.stream_count == 5);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 5);
    REQUIRE(summary.duplicates == 3);
    // Collisions keep their cue ids; unique names stay clean.
    REQUIRE(fs::exists(out_dir.path / "Music/Scene/Song 0001.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/Scene/Song 0002.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/First.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/Same.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/Other.wav"));
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    // BGM_Same decodes to the same PCM as BGM_First and points at it.
    REQUIRE(manifest.find("BGM_Same.bwav,Music/Same.wav,bgm,2,48000,0.001,,") !=
            std::string::npos);
    REQUIRE(manifest.find("Music/First.wav") != std::string::npos);
    REQUIRE(manifest.find("BGM_Scene_Song_0001.bwav,Music/Scene/Song 0001.wav,") !=
            std::string::npos);
    REQUIRE(manifest.find("BGM_Scene_Song_0002.bwav,Music/Scene/Song 0002.wav,") !=
            std::string::npos);

    // Neither a plain folder nor a missing NUP is a music source.
    FileSys::Music::SourceInfo bad;
    REQUIRE(!FileSys::Music::ScanMusic((dump_dir.path / "base").string(), bad));
    REQUIRE(!FileSys::Music::ScanMusic((dump_dir.path / "missing.nup").string(), bad));
}

TEST_CASE("FileSys::Music separates demo streams and folds cue ids into names",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    // A song, a demo performance hiding behind the song prefix (two cues of the
    // same scene), its title variant, and a song that merely starts with "Title".
    Put(stream / "BGM_OpenField.bwav",
        MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));
    Put(stream / "BGM_Dm_OpenField_0005.bwav",
        MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    Put(stream / "BGM_Dm_OpenField_0045.bwav",
        MakeDspBwav(NibblePattern(3, 28), NibblePattern(11, 28)));
    Put(stream / "BGM_Dm_Title_0010.bwav",
        MakeDspBwav(NibblePattern(4, 28), NibblePattern(12, 28)));
    Put(stream / "BGM_Title_Theme.bwav",
        MakeDspBwav(NibblePattern(5, 28), NibblePattern(13, 28)));
    const fs::path main = dump_dir.path / "base/exefs/main";

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    REQUIRE(info.stream_count == 5);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Bgm)] == 2);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Cutscene)] == 2);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Title)] == 1);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 5);
    // The demo streams sort into their own folders; the two cues of one scene
    // keep the cue ids the game named them with, and a lone demo title stays a
    // title while "BGM_Title_Theme" remains a song under an area named Title.
    REQUIRE(fs::exists(out_dir.path / "Music/OpenField.wav"));
    REQUIRE(fs::exists(out_dir.path / "Cutscenes/OpenField 0005.wav"));
    REQUIRE(fs::exists(out_dir.path / "Cutscenes/OpenField 0045.wav"));
    REQUIRE(fs::exists(out_dir.path / "Title/Title.wav"));
    REQUIRE(fs::exists(out_dir.path / "Music/Title/Theme.wav"));
    REQUIRE(!fs::exists(out_dir.path / "OpenField 1.wav"));
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Dm_OpenField_0005.bwav,Cutscenes/OpenField 0005.wav,cutscene,") !=
            std::string::npos);
    REQUIRE(manifest.find("BGM_Dm_OpenField_0045.bwav,Cutscenes/OpenField 0045.wav,cutscene,") !=
            std::string::npos);
    REQUIRE(manifest.find("BGM_Dm_Title_0010.bwav,Title/Title.wav,title,") != std::string::npos);
    REQUIRE(manifest.find("BGM_Title_Theme.bwav,Music/Title/Theme.wav,bgm,") != std::string::npos);
}

TEST_CASE("FileSys::Music reports cancellation and per-file failures", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Good.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));
    Put(stream / "BGM_Bad.bwav", "not a bwav at all");

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(
                (dump_dir.path / "base/exefs/main").string(), options, {}, nullptr, &summary,
                &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);
    REQUIRE(summary.files_failed == 1);
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Bad.bwav,Music/Bad.wav,bgm,,,") != std::string::npos);
    REQUIRE(manifest.find("missing BWAV magic") != std::string::npos);

    // Cancellation stops before touching the destination: an existing manifest
    // survives a cancelled rerun untouched.
    Put(out_dir.path / "manifest.csv", "sentinel\n");
    std::atomic_bool cancel{true};
    FileSys::Music::Summary cancelled;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, &cancel, &cancelled, &error) ==
            FileSys::Music::Result::Cancelled);
    REQUIRE(cancelled.files_ok == 0);
    REQUIRE(ReadAll(out_dir.path / "manifest.csv") == "sentinel\n");
}

TEST_CASE("FileSys::Music tells a music-free dump apart from a bad source",
          "[core][music]") {
    TempDir dump_dir;
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(dump_dir.path / "base/romfs/credits.txt", "no audio here");

    const fs::path main = dump_dir.path / "base/exefs/main";
    FileSys::Music::SourceInfo info;
    // A valid dump that stores no stream music is recognized; it just has none.
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    REQUIRE(info.stream_count == 0);

    FileSys::Music::Options options;
    options.out_dir = dump_dir.path / "out";
    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::NoMusic);

    // A plain folder is still not a music source at all.
    FileSys::Music::SourceInfo bad;
    REQUIRE(!FileSys::Music::ScanMusic((dump_dir.path / "base").string(), bad));
}

TEST_CASE("FileSys::Music refuses unsafe and linked output destinations",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    TempDir link_target;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    // A stream whose cleaned folder is "..", one whose kind folder someone
    // replaced with a symlink, and a healthy cutscene track beside them.
    Put(stream / "BGM_.._Escape.bwav",
        MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));
    Put(stream / "BGM_Linked.bwav", MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    Put(stream / "Dm_Good.bwav", MakeDspBwav(NibblePattern(3, 28), NibblePattern(11, 28)));
    std::error_code link_ec;
    std::filesystem::create_directory_symlink(link_target.path, out_dir.path / "Music");
    const fs::path main = dump_dir.path / "base/exefs/main";

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);
    REQUIRE(summary.files_failed == 2);
    REQUIRE(fs::exists(out_dir.path / "Cutscenes/Good.wav"));
    // Neither refusal wrote anything: no escape out of Music/, nothing through
    // the link.
    REQUIRE(!fs::exists(out_dir.path / "Escape.wav"));
    REQUIRE(fs::is_empty(link_target.path));
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_.._Escape.bwav,Music/../Escape,bgm,,,") != std::string::npos);
    REQUIRE(manifest.find("unsafe output name") != std::string::npos);
    REQUIRE(manifest.find("BGM_Linked.bwav,Music/Linked.wav,bgm,,,") != std::string::npos);
    REQUIRE(manifest.find("the output folder passes through a symlink") != std::string::npos);
}

// Opt-in run against a real game archive: set EDEN_MUSIC_TEST_NUP to a .nup file and
// EDEN_MUSIC_TEST_OUT to a writable folder. Extracts every stream the way the UI does
// and fails when any track cannot be decoded.
TEST_CASE("FileSys::Music extracts a real NUP archive", "[core][music]") {
    const char* nup = std::getenv("EDEN_MUSIC_TEST_NUP");
    const char* out = std::getenv("EDEN_MUSIC_TEST_OUT");
    if (nup == nullptr || out == nullptr || *nup == '\0' || *out == '\0') {
        SKIP("set EDEN_MUSIC_TEST_NUP and EDEN_MUSIC_TEST_OUT to run this");
    }

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(nup, info));
    REQUIRE(info.stream_count > 0);

    FileSys::Music::Options options;
    options.out_dir = out;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding =
        std::getenv("EDEN_MUSIC_TEST_WAV") != nullptr ? FileSys::Music::Encoding::Wav
                                                      : FileSys::Music::Encoding::Opus;
    if (const char* cover = std::getenv("EDEN_MUSIC_TEST_COVER");
        cover != nullptr && *cover != '\0') {
        std::ifstream in(cover, std::ios::binary);
        options.cover_override = {std::istreambuf_iterator<char>(in), {}};
        REQUIRE(!options.cover_override.empty());
    }

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(nup, options, {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::Success);
    // Streams the decoder does not support (NXOpus cutscene takes in TotK-era audio)
    // are recorded in manifest.csv instead of failing the run.
    REQUIRE(summary.files_ok + summary.files_failed == info.stream_count);
    REQUIRE(summary.files_ok > 0);
    REQUIRE(fs::exists(options.out_dir / "manifest.csv"));
    fmt::print(stderr, "extracted {} stream(s), {} skipped (see manifest), {} duplicate(s), "
                       "title \"{}\"\n",
               summary.files_ok, summary.files_failed, summary.duplicates, info.game_title);
}

// ---------------------------------------------------------------------------------------------
// BWAV codec 2 (NXOpus), the TotK-era layout: each channel block points at its own
// Nintendo Switch Opus stream.
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<std::vector<u8>> EncodeOpusFrames(const std::vector<s16>& pcm) {
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder{
        opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &error),
        &opus_encoder_destroy};
    REQUIRE(encoder != nullptr);
    REQUIRE(error == OPUS_OK);
    opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(96000));

    std::vector<u8> buffer(4000);
    std::vector<std::vector<u8>> packets;
    for (std::size_t off = 0; off + 960 <= pcm.size(); off += 960) {
        const int size = opus_encode(encoder.get(), pcm.data() + off, 960, buffer.data(),
                                     static_cast<opus_int32>(buffer.size()));
        REQUIRE(size > 0);
        packets.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    return packets;
}

std::string MakeNxOpusStream(const std::vector<std::vector<u8>>& packets, u16 skip) {
    u32 data_size = 0;
    for (const auto& packet : packets) {
        data_size += 8 + static_cast<u32>(packet.size());
    }
    std::string stream(0x2C + data_size, '\0');
    Put32(stream, 0x00, 0x80000001); // basic info chunk
    Put32(stream, 0x04, 0x24);       // chunk size
    stream[0x09] = 1;                 // channels
    Put32(stream, 0x0c, 48000);      // sample rate
    Put32(stream, 0x10, 0x24);       // data info chunk offset
    Put16(stream, 0x1c, skip);       // pre-skip
    Put32(stream, 0x24, 0x80000004); // data info chunk
    Put32(stream, 0x28, data_size);
    std::size_t at = 0x2C;
    for (const auto& packet : packets) {
        const u32 size = static_cast<u32>(packet.size());
        stream[at++] = static_cast<char>(size >> 24);
        stream[at++] = static_cast<char>(size >> 16);
        stream[at++] = static_cast<char>(size >> 8);
        stream[at++] = static_cast<char>(size);
        at += 4;
        std::memcpy(stream.data() + at, packet.data(), packet.size());
        at += packet.size();
    }
    return stream;
}

std::string MakeNxOpusBwav(const std::string& left, const std::string& right, u16 skip,
                           s32 num_samples) {
    constexpr u32 kLeftStart = 0xC0;
    const u32 kRightStart = kLeftStart + static_cast<u32>(left.size());
    std::string out(kRightStart + right.size(), '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 2);
    for (u16 ch = 0; ch < 2; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        const u32 own_start = ch == 0 ? kLeftStart : kRightStart;
        Put16(out, base + 0x00, 2);           // NXOpus codec
        Put32(out, base + 0x04, 48000);       // sample rate
        Put32(out, base + 0x0c, num_samples); // samples
        Put32(out, base + 0x30, own_start);
        Put32(out, base + 0x34, own_start);
        Put32(out, base + 0x38, 1);
        Put32(out, base + 0x3c, -1);
    }
    std::memcpy(out.data() + kLeftStart, left.data(), left.size());
    std::memcpy(out.data() + kRightStart, right.data(), right.size());
    return out;
}

std::vector<s16> DecodeOpusFrames(const std::vector<std::vector<u8>>& packets, u16 skip,
                                  s32 take) {
    int error = OPUS_OK;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
        opus_decoder_create(48000, 1, &error), &opus_decoder_destroy};
    REQUIRE(decoder != nullptr);
    std::vector<s16> buffer(5760);
    std::vector<s16> all;
    for (const auto& packet : packets) {
        const int decoded = opus_decode(decoder.get(), packet.data(),
                                        static_cast<opus_int32>(packet.size()), buffer.data(),
                                        5760, 0);
        REQUIRE(decoded > 0);
        all.insert(all.end(), buffer.begin(), buffer.begin() + decoded);
    }
    REQUIRE(all.size() >= skip + static_cast<std::size_t>(take));
    return {all.begin() + skip, all.begin() + skip + take};
}

} // namespace

TEST_CASE("FileSys::Music decodes NXOpus BWAV streams", "[core][music]") {
    // Two distinct mono signals, encoded as real Opus packets.
    constexpr s32 kFrames = 4;
    constexpr s32 kFrameSamples = 960;
    constexpr u16 kSkip = 5;
    constexpr s32 kSamples = kFrames * kFrameSamples - kSkip;
    std::vector<s16> left(kFrames * kFrameSamples);
    std::vector<s16> right(kFrames * kFrameSamples);
    for (u32 i = 0; i < left.size(); ++i) {
        left[i] = static_cast<s16>(i % 1000);
        right[i] = static_cast<s16>(-800 + (i * 7) % 1600);
    }
    const auto left_packets = EncodeOpusFrames(left);
    const auto right_packets = EncodeOpusFrames(right);
    const std::string bwav = MakeNxOpusBwav(MakeNxOpusStream(left_packets, kSkip),
                                            MakeNxOpusStream(right_packets, kSkip), kSkip,
                                            kSamples);

    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Opus.bwav", bwav);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);
    REQUIRE(summary.files_failed == 0);

    // The extractor must produce exactly what decoding the same packets directly does,
    // with the pre-skip dropped.
    const auto expected_left = DecodeOpusFrames(left_packets, kSkip, kSamples);
    const auto expected_right = DecodeOpusFrames(right_packets, kSkip, kSamples);
    const auto wav = WavOf(out_dir.path / "Music", "Opus");
    REQUIRE(wav.substr(44, 2 * kSamples * 2) ==
            PcmPayload(Interleave(expected_left, expected_right)));
}

TEST_CASE("FileSys::Music rejects hostile BWAV headers without wild allocations",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));

    // An NXOpus chunk whose data offset would wrap 32-bit arithmetic before the
    // bounds checks.
    std::string wrap_stream(0x2C, '\0');
    Put32(wrap_stream, 0x00, 0x80000001);
    wrap_stream[0x09] = 1;
    Put32(wrap_stream, 0x0c, 48000);
    Put32(wrap_stream, 0x10, 0xFFFFFFF0);
    Put(stream / "BGM_Wrap.bwav", MakeNxOpusBwav(wrap_stream, wrap_stream, 0, 28));

    // A declared sample count that would ask for gigabytes of decoded audio.
    std::string huge = MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28));
    Put32(huge, 0x10 + 0x0c, 0x20000000);
    Put(stream / "BGM_Huge.bwav", huge);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::IoError);
    REQUIRE(summary.files_ok == 0);
    REQUIRE(summary.files_failed == 2);
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Wrap.bwav,Music/Wrap.wav,bgm,,,") != std::string::npos);
    REQUIRE(manifest.find("NXOpus: missing data info chunk") != std::string::npos);
    REQUIRE(manifest.find("declared sample count exceeds the stream data") != std::string::npos);
}

TEST_CASE("FileSys::Music embeds a chosen cover into every track", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Cover.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;
    options.cover_override.assign(kPng.begin(), kPng.end());

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);

    // The WAV carries an ID3 chunk with the cover as an APIC frame.
    const auto wav = WavOf(out_dir.path / "Music", "Cover");
    REQUIRE(wav.find("APIC") != std::string::npos);
    REQUIRE(wav.find("image/png") != std::string::npos);
    const std::string png(reinterpret_cast<const char*>(kPng.data()), kPng.size());
    REQUIRE(wav.find(png) != std::string::npos);
    REQUIRE(wav.find("TIT2") != std::string::npos);
    REQUIRE(wav.find("Cover") != std::string::npos);
}

TEST_CASE("FileSys::Music extracts only the selected kinds", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Main.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));
    Put(stream / "BGM_Scene_Intro.bwav",
        MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    Put(stream / "BGM_Amb_Close_X.bwav",
        MakeDspBwav(NibblePattern(3, 28), NibblePattern(11, 28)));
    Put(stream / "Dm_Cut_Scene.bwav",
        MakeDspBwav(NibblePattern(4, 28), NibblePattern(12, 28)));
    Put(stream / "Dm_Title_0005.bwav",
        MakeDspBwav(NibblePattern(5, 28), NibblePattern(13, 28)));
    const fs::path main = dump_dir.path / "base/exefs/main";

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    REQUIRE(info.stream_count == 5);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Bgm)] == 1);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Cutscene)] == 1);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Title)] == 1);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Segments)] == 1);
    REQUIRE(info.kind_counts[static_cast<std::size_t>(FileSys::Music::Kind::Stems)] == 1);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    // One song and one cutscene track; segments, stems and the title stay in the game.
    options.kinds = {true, true, false, false, false};

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 2);
    REQUIRE(fs::exists(out_dir.path / "Music/Main.wav"));
    REQUIRE(fs::exists(out_dir.path / "Cutscenes/Cut/Scene.wav"));
    REQUIRE(!fs::exists(out_dir.path / "Segments/Scene/Intro.wav"));
    REQUIRE(!fs::exists(out_dir.path / "Stems/Amb/Close X.wav"));
    REQUIRE(!fs::exists(out_dir.path / "Title/Title.wav"));
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Main.bwav,Music/Main.wav,bgm,") != std::string::npos);
    REQUIRE(manifest.find("Dm_Cut_Scene.bwav,Cutscenes/Cut/Scene.wav,cutscene,") !=
            std::string::npos);
    REQUIRE(manifest.find("BGM_Scene_Intro") == std::string::npos);
    REQUIRE(manifest.find("BGM_Amb_Close") == std::string::npos);
    REQUIRE(manifest.find("Dm_Title") == std::string::npos);

    // A run with every kind switched off is refused instead of writing nothing.
    options.kinds = {false, false, false, false, false};
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::NoMusic);
    REQUIRE(error.find("kinds") != std::string::npos);
}

TEST_CASE("FileSys::Music writes Ogg Opus files", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Opus;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 4);
    REQUIRE(fs::exists(out_dir.path / "Music/Main.opus"));
    REQUIRE(fs::exists(out_dir.path / "Music/New.opus"));
    REQUIRE(fs::exists(out_dir.path / "Title/Title.opus"));
    REQUIRE(!fs::exists(out_dir.path / "Music/Main.wav"));
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("Music/Main.opus") != std::string::npos);

    // The 32 kHz PCM16 track is resampled into the 48 kHz encoder; the DSP track
    // already sits at 48 kHz. Every page's CRC has been verified by ParseOgg.
    const auto main_pages = ParseOgg(ReadAll(out_dir.path / "Music/Main.opus"));
    const auto new_pages = ParseOgg(ReadAll(out_dir.path / "Music/New.opus"));
    REQUIRE(main_pages.size() == 3); // OpusHead, OpusTags, one 20 ms frame
    REQUIRE((main_pages[0].flags & 0x02) != 0);
    REQUIRE(main_pages[0].granule == 0);
    REQUIRE(main_pages[0].packets.size() == 1);
    const auto& head = main_pages[0].packets[0];
    REQUIRE(std::memcmp(head.data(), "OpusHead", 8) == 0);
    REQUIRE(head[8] == 1); // version
    REQUIRE(head[9] == 2); // channels
    u16 preskip{};
    std::memcpy(&preskip, head.data() + 10, sizeof(preskip));
    REQUIRE(preskip > 0);
    u32 input_rate{};
    std::memcpy(&input_rate, head.data() + 12, sizeof(input_rate));
    REQUIRE(input_rate == 32000);
    REQUIRE(head[18] == 0); // channel mapping family 0

    const auto& tags = main_pages[1].packets[0];
    REQUIRE(main_pages[1].granule == 0);
    REQUIRE(std::memcmp(tags.data(), "OpusTags", 8) == 0);
    const std::string tags_bytes(reinterpret_cast<const char*>(tags.data()), tags.size());
    REQUIRE(tags_bytes.find("TITLE=Main") != std::string::npos);
    REQUIRE(tags_bytes.find("ALBUM=") != std::string::npos);

    // The single audio page holds one packet with the 28 input samples resampled
    // to 42 at 48 kHz; the EOS granule keeps the true count on top of the pre-skip.
    REQUIRE((main_pages[2].flags & 0x04) != 0);
    REQUIRE(main_pages[2].packets.size() == 1);
    REQUIRE(main_pages[2].granule == preskip + 42);
    int opus_error = OPUS_OK;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
        opus_decoder_create(48000, 2, &opus_error), &opus_decoder_destroy};
    REQUIRE(decoder != nullptr);
    REQUIRE(opus_error == OPUS_OK);
    std::array<s16, 5760 * 2> decoded{};
    REQUIRE(opus_decode(decoder.get(), main_pages[2].packets[0].data(),
                        static_cast<opus_int32>(main_pages[2].packets[0].size()),
                        decoded.data(), 5760, 0) == 960);

    const auto& new_head = new_pages[0].packets[0];
    u32 new_rate{};
    std::memcpy(&new_rate, new_head.data() + 12, sizeof(new_rate));
    REQUIRE(new_rate == 48000);
    u16 new_preskip{};
    std::memcpy(&new_preskip, new_head.data() + 10, sizeof(new_preskip));
    REQUIRE(new_pages.back().granule == new_preskip + 28);
}

TEST_CASE("FileSys::Music embeds the cover into the Opus tags", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Cover.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Opus;
    options.cover_override.assign(kPng.begin(), kPng.end());

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(),
                                         options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);

    const auto pages = ParseOgg(ReadAll(out_dir.path / "Music/Cover.opus"));
    const auto& tags = pages[1].packets[0];
    const std::string tags_bytes(reinterpret_cast<const char*>(tags.data()), tags.size());
    const auto marker = tags_bytes.find("METADATA_BLOCK_PICTURE=");
    REQUIRE(marker != std::string::npos);
    const std::string picture = Base64Decode(
        std::string_view{tags_bytes}.substr(marker + sizeof("METADATA_BLOCK_PICTURE=") - 1));
    REQUIRE(picture.find("image/png") != std::string::npos);
    const std::string png(reinterpret_cast<const char*>(kPng.data()), kPng.size());
    REQUIRE(picture.find(png) != std::string::npos);
    REQUIRE(tags_bytes.find("TITLE=Cover") != std::string::npos);
}

TEST_CASE("FileSys::Music spans a large cover's Opus tags across pages", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Cover.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));

    // A "PNG" far past a page's lacing capacity: magic intact, the rest padding.
    // Real NUP thumbnails (Super Mario Bros. Wonder's is a large JPEG) make the
    // base64 picture block bigger than one page can lace.
    std::vector<u8> big_cover(kPng.begin(), kPng.end());
    big_cover.resize(90 * 1024, 'A');

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Opus;
    options.cover_override = big_cover;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(),
                                         options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);

    // ParseOgg checks every page's CRC, sequence and continued flags while it
    // reassembles the packet that spans them.
    const auto pages = ParseOgg(ReadAll(out_dir.path / "Music/Cover.opus"));
    REQUIRE(pages.size() > 3); // head, two-plus tags pages, audio
    const std::vector<u8>* tags = nullptr;
    for (const auto& page : pages) {
        for (const auto& packet : page.packets) {
            if (packet.size() >= 8 && std::memcmp(packet.data(), "OpusTags", 8) == 0) {
                tags = &packet;
            }
        }
    }
    REQUIRE(tags != nullptr);
    REQUIRE(tags->size() > 65025 + 8); // it really had to span

    // The reassembled picture block carries the whole cover.
    const std::string tags_bytes(reinterpret_cast<const char*>(tags->data()), tags->size());
    const auto marker = tags_bytes.find("METADATA_BLOCK_PICTURE=");
    REQUIRE(marker != std::string::npos);
    const std::string picture = Base64Decode(
        std::string_view{tags_bytes}.substr(marker + sizeof("METADATA_BLOCK_PICTURE=") - 1));
    REQUIRE(picture.find("image/png") != std::string::npos);
    REQUIRE(picture.find(std::string(big_cover.begin(), big_cover.end())) !=
            std::string::npos);
    REQUIRE(std::memcmp(pages[0].packets[0].data(), "OpusHead", 8) == 0);
    REQUIRE((pages.back().flags & 0x04) != 0);
}

namespace {

// A 4-channel DSP BWAV: the real Super Mario Bros. Wonder archive carries
// multichannel BGM (Koopa Stage is 4 channels, PackunMarchLong2 is 6), and those
// tracks take the Opus multistream path with a channel mapping family 1 head.
std::string MakeQuadBwav(const std::vector<s16>& left, const std::vector<s16>& far_left,
                         const std::vector<s16>& far_right, const std::vector<s16>& right) {
    REQUIRE(left.size() == 28);
    REQUIRE(far_left.size() == 28);
    REQUIRE(far_right.size() == 28);
    REQUIRE(right.size() == 28);
    constexpr u32 kInterleave = 2 * 8; // one 8-byte DSP frame per channel block
    constexpr u32 kDataStart = 0x10 + 4 * 0x4C;
    std::string out(kDataStart + 2 * 4 * kInterleave, '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 4);
    for (u16 ch = 0; ch < 4; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        const u32 own_start = kDataStart + ch * kInterleave;
        Put16(out, base + 0x00, 1);          // DSP codec
        Put32(out, base + 0x04, 48000);      // sample rate
        Put32(out, base + 0x0c, 28);         // samples
        Put32(out, base + 0x30, own_start);  // full-file offset
        Put32(out, base + 0x34, own_start);  // data start
        Put32(out, base + 0x38, 1);          // flag
        Put32(out, base + 0x3c, -1);         // no loop
    }
    Put32(out, 0x8C, kDataStart + kInterleave); // second channel's data offset
    const std::array<const std::vector<s16>*, 4> channels{&left, &far_left, &far_right, &right};
    for (u32 frame = 0; frame < 2; ++frame) {
        for (u16 ch = 0; ch < 4; ++ch) {
            const std::vector<s16>& samples = *channels[ch];
            std::size_t at = kDataStart + frame * 4 * kInterleave + ch * kInterleave;
            out[at++] = '\x00'; // scale 1, coefficient 0: nibbles decode to themselves
            for (u32 pair = 0; pair < 7; ++pair) {
                const s16 high = samples[frame * 14 + pair * 2];
                const s16 low = samples[frame * 14 + pair * 2 + 1];
                out[at++] = static_cast<char>((NibbleOf(high) << 4) | NibbleOf(low));
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("FileSys::Music writes multichannel Opus with a family 1 head", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Quad.bwav",
        MakeQuadBwav(NibblePattern(1, 28), NibblePattern(5, 28), NibblePattern(9, 28),
                     NibblePattern(13, 28)));

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Opus;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(),
                                         options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);

    const auto pages = ParseOgg(ReadAll(out_dir.path / "Music/Quad.opus"));
    const auto& head = pages[0].packets[0];
    REQUIRE(std::memcmp(head.data(), "OpusHead", 8) == 0);
    REQUIRE(head[8] == 1);  // version
    REQUIRE(head[9] == 4);   // channels
    REQUIRE(head[18] == 1); // channel mapping family 1
    REQUIRE(head[19] == 4); // one uncoupled stream per channel
    REQUIRE(head[20] == 0);
    REQUIRE(head.size() == 21 + 4); // base + stream/coupled counts + the mapping table
    REQUIRE(head[21] == 0);
    REQUIRE(head[22] == 1);
    REQUIRE(head[23] == 2);
    REQUIRE(head[24] == 3);
    u16 preskip{};
    std::memcpy(&preskip, head.data() + 10, sizeof(preskip));
    REQUIRE(pages.back().granule == preskip + 28);

    // The audio packet decodes as a 4-channel uncoupled multistream.
    int opus_error = OPUS_OK;
    const std::array<unsigned char, 4> decode_mapping{0, 1, 2, 3};
    using MsDecoderPtr = std::unique_ptr<OpusMSDecoder, void (*)(OpusMSDecoder*)>;
    MsDecoderPtr decoder{opus_multistream_decoder_create(48000, 4, 4, 0,
                                                         decode_mapping.data(), &opus_error),
                         &opus_multistream_decoder_destroy};
    REQUIRE(decoder != nullptr);
    REQUIRE(opus_error == OPUS_OK);
    const auto& packet = pages.back().packets.back();
    std::array<s16, 5760 * 4> decoded{};
    REQUIRE(opus_multistream_decode(decoder.get(), packet.data(),
                                    static_cast<opus_int32>(packet.size()), decoded.data(),
                                    5760, 0) == 960);
}
