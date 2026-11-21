// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/music_extract_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStandardPaths>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

#include "common/fs/fs_util.h"

namespace {

QString Sanitized(std::string_view name) {
    QString out;
    for (const char c : name) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        out.append(QLatin1Char(plain ? c : '_'));
    }
    while (out.endsWith(u' ')) {
        out.chop(1);
    }
    return out;
}

std::vector<u8> ReadImageBytes(const QString& path) {
    // std::filesystem::path carries the Unicode form through; a narrow string
    // would go through the ANSI code page on Windows.
    std::ifstream in(std::filesystem::path{path.toStdU16String()}, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// One checkbox per stream kind, in FileSys::Music::Kind order.
struct KindChoice {
    const char* label;
    const char* tooltip;
};
constexpr std::array<KindChoice, FileSys::Music::KindCount> KindRows{{
    {"Music", "The BGM_ songs played during normal play."},
    {"Cutscenes", "Dm_ cutscene music, including the BGM_Dm_ variants."},
    {"Title", "The title screen performance (Dm_Title)."},
    {"Segments", "Intros, loops, jingles and other sequenced parts of a song."},
    {"Stems", "Close and far ambience, vocal and noise layers the game mixes live."},
}};

} // namespace

MusicExtractDialog::MusicExtractDialog(const FileSys::Music::SourceInfo& info, QWidget* parent)
    : QDialog(parent), nup_cover(info.cover) {
    setWindowTitle(tr("Audio Explorer"));
    setModal(true);
    setMinimumWidth(520);

    const QString title = QString::fromStdString(info.game_title).trimmed();
    auto* summary = new QLabel(
        tr("Only unencrypted formats are supported. NSP files and any format that requires "
           "decryption keys can't be used.\n\n"
           "Extracted audio is for your personal use only and remains the property of Nintendo. "
           "Eglantine does not support or allow its distribution.")
            .arg(info.stream_count)
            .arg(info.stream_bytes / (1024 * 1024))
            .arg(title.isEmpty() ? tr("this game") : title));
    summary->setWordWrap(true);

    auto* out_label = new QLabel(tr("Output folder:"));
    out_edit = new QLineEdit;
    const QString default_dir =
        QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + QDir::separator() +
        (title.isEmpty() ? tr("game music") : Sanitized(title.toStdString()));
    out_edit->setText(QDir::toNativeSeparators(default_dir));
    auto* browse = new QPushButton(tr("Browse..."));
    connect(browse, &QPushButton::clicked, this, &MusicExtractDialog::Browse);

    auto* out_row = new QHBoxLayout;
    out_row->setContentsMargins(0, 0, 0, 0);
    out_row->addWidget(out_label);
    out_row->addWidget(out_edit, 1);
    out_row->addWidget(browse);

    encoding_combo = new QComboBox;
    encoding_combo->addItem(tr("WAV (PCM 16-bit)"), int(FileSys::Music::Encoding::Wav));
    encoding_combo->addItem(tr("Opus (compact)"), int(FileSys::Music::Encoding::Opus));
    encoding_combo->setItemData(
        1, tr("Ogg Opus at 48 kHz; roughly a tenth of the WAV size for the same game audio."),
        Qt::ToolTipRole);

    // Which kinds of stream come out; each shows how many this game holds, and kinds
    // with nothing to write are disabled rather than hidden.
    auto* kinds_grid = new QGridLayout;
    kinds_grid->setContentsMargins(0, 0, 0, 0);
    kinds_grid->setHorizontalSpacing(12);
    kinds_grid->setVerticalSpacing(4);
    for (std::size_t index = 0; index < KindRows.size(); ++index) {
        const KindChoice& choice = KindRows[index];
        auto* box = new QCheckBox(tr("%1 (%2)").arg(tr(choice.label)).arg(info.kind_counts[index]));
        box->setToolTip(tr(choice.tooltip));
        box->setChecked(info.kind_counts[index] > 0);
        box->setEnabled(info.kind_counts[index] > 0);
        connect(box, &QCheckBox::toggled, this, &MusicExtractDialog::UpdateOkEnabled);
        kind_boxes[index] = box;
        kinds_grid->addWidget(box, static_cast<int>(index / 3), static_cast<int>(index % 3));
    }
    auto* kinds_holder = new QWidget;
    kinds_holder->setLayout(kinds_grid);

    layout_tree = new QRadioButton(
        tr("Sort into category folders (Music, Cutscenes, Title, Segments, Stems), "
           "one subfolder per area or scene inside each"));
    layout_tree->setChecked(true);
    layout_flat = new QRadioButton(tr("Flat - every track directly in the output folder"));

    auto* structure_box = new QVBoxLayout;
    structure_box->setContentsMargins(0, 0, 0, 0);
    structure_box->setSpacing(4);
    structure_box->addWidget(layout_tree);
    structure_box->addWidget(layout_flat);
    auto* structure_holder = new QWidget;
    structure_holder->setLayout(structure_box);

    // Cover art embedded into every track: the NUP icon when it has one, otherwise
    // an image of the user's choosing.
    cover_state = new QLabel;
    auto* cover_choose = new QPushButton(tr("Choose..."));
    cover_reset = new QPushButton(tr("Reset"));
    connect(cover_choose, &QPushButton::clicked, this, &MusicExtractDialog::ChooseCover);
    connect(cover_reset, &QPushButton::clicked, this, &MusicExtractDialog::ResetCover);
    auto* cover_row = new QHBoxLayout;
    cover_row->setContentsMargins(0, 0, 0, 0);
    cover_row->addWidget(cover_state, 1);
    cover_row->addWidget(cover_choose);
    cover_row->addWidget(cover_reset);
    auto* cover_holder = new QWidget;
    cover_holder->setLayout(cover_row);
    UpdateCoverLabel();

    auto* form = new QFormLayout;
    form->addRow(tr("Extract:"), kinds_holder);
    form->addRow(tr("Encoding:"), encoding_combo);
    form->addRow(tr("Structure:"), structure_holder);
    form->addRow(tr("Cover art:"), cover_holder);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    ok_button = buttons->button(QDialogButtonBox::Ok);
    ok_button->setText(tr("Extract"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(24, 24, 24, 20);
    column->setSpacing(14);
    column->addWidget(summary);
    column->addLayout(out_row);
    column->addLayout(form);
    column->addStretch();
    column->addWidget(buttons);

    connect(out_edit, &QLineEdit::textChanged, this, &MusicExtractDialog::UpdateOkEnabled);
    UpdateOkEnabled();
}

void MusicExtractDialog::Browse() {
    const QString current = out_edit->text();
    const QString dir =
        QFileDialog::getExistingDirectory(this, tr("Select Music Output Folder"),
                                          QDir(current).dirName().isEmpty() ? QString{} : current);
    if (!dir.isEmpty()) {
        out_edit->setText(QDir::toNativeSeparators(dir));
    }
}

void MusicExtractDialog::ChooseCover() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Select Cover Art"), QString{}, tr("Images (*.png *.jpg *.jpeg);;All Files (*)"));
    if (path.isEmpty()) {
        return;
    }
    std::vector<u8> bytes = ReadImageBytes(path);
    if (bytes.empty()) {
        return;
    }
    chosen_cover = std::move(bytes);
    chosen_name = QFileInfo(path).fileName();
    UpdateCoverLabel();
}

void MusicExtractDialog::ResetCover() {
    chosen_cover.clear();
    chosen_name.clear();
    UpdateCoverLabel();
}

void MusicExtractDialog::UpdateCoverLabel() {
    if (!chosen_name.isEmpty()) {
        cover_state->setText(tr("Cover: %1").arg(chosen_name));
    } else if (!nup_cover.empty()) {
        cover_state->setText(tr("Cover: the NUP thumbnail"));
    } else {
        cover_state->setText(tr("Cover: none (this NUP stores no thumbnail)"));
    }
    cover_reset->setEnabled(!chosen_name.isEmpty());
}

void MusicExtractDialog::UpdateOkEnabled() {
    const bool any_kind = std::any_of(kind_boxes.begin(), kind_boxes.end(),
                                      [](const QCheckBox* box) { return box->isChecked(); });
    ok_button->setEnabled(!out_edit->text().trimmed().isEmpty() && any_kind);
}

std::filesystem::path MusicExtractDialog::OutDir() const {
    return std::filesystem::path{out_edit->text().trimmed().toStdU16String()};
}

FileSys::Music::Layout MusicExtractDialog::Layout() const {
    return layout_flat->isChecked() ? FileSys::Music::Layout::Flat : FileSys::Music::Layout::ByKind;
}

FileSys::Music::Encoding MusicExtractDialog::Encoding() const {
    return static_cast<FileSys::Music::Encoding>(encoding_combo->currentData().toInt());
}

std::array<bool, FileSys::Music::KindCount> MusicExtractDialog::Kinds() const {
    std::array<bool, FileSys::Music::KindCount> out{};
    for (std::size_t index = 0; index < out.size(); ++index) {
        out[index] = kind_boxes[index]->isChecked();
    }
    return out;
}

const std::vector<u8>& MusicExtractDialog::CoverOverride() const {
    return chosen_cover;
}
