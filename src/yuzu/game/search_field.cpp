// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "game/game_list.h"
#include "game/home_side_panel.h"
#include "game/search_field.h"

#include <QKeyEvent>
#include <QPainter>
#include <QToolButton>

namespace {

constexpr int SideMargin = 16;
constexpr int ControlSpacing = 8;
/// Height of the row's controls, matching the details panel's buttons so the bar reads as one
/// control family.
constexpr int ControlHeight = 38;
constexpr int CloseButtonSize = 28;
constexpr int CloseIconSize = 20;

QString Rgba(QColor color) {
    return QStringLiteral("rgba(%1, %2, %3, %4)")
        .arg(color.red())
        .arg(color.green())
        .arg(color.blue())
        .arg(color.alpha());
}

QColor WithAlpha(QColor color, int alpha) {
    color.setAlpha(alpha);
    return color;
}

/// The row's line edit: outlined like the details panel's body buttons and derived from the
/// palette so it stays legible on light and dark themes alike. The height is stated in the style
/// sheet as well because QSS, not the widget, decides the size of a styled control.
QString LineEditStyle(const QPalette& pal) {
    const QColor text = pal.color(QPalette::WindowText);
    const QColor accent = pal.color(QPalette::Highlight);
    return QStringLiteral(
        "QLineEdit { border: 1px solid %1; border-radius: 8px; padding: 0 12px; "
        "min-height: %2px; max-height: %2px; background: %3; color: %4; "
        "selection-background-color: %5; } "
        "QLineEdit:hover { border-color: %6; } "
        "QLineEdit:focus { border-color: %7; } "
        "QLineEdit:disabled { border-color: %8; color: %9; }")
        // The 1 px border is part of the box, so the content height is the row height minus it.
        .arg(Rgba(WithAlpha(text, 60)), QString::number(ControlHeight - 2),
             Rgba(pal.color(QPalette::Base)), Rgba(text))
        .arg(Rgba(accent), Rgba(WithAlpha(text, 110)), Rgba(accent),
             Rgba(WithAlpha(text, 35)), Rgba(WithAlpha(text, 90)));
}

/// Icon-only close button, painted like the details panel's small tool buttons: a styled
/// QToolButton has its frame and icon laid out by the platform style, which draws the surface
/// inset and scales the glyph down, so the pill and the glyph are drawn here instead.
class FilterCloseButton : public QToolButton {
public:
    FilterCloseButton(int size, int icon_size, QWidget* parent) : QToolButton{parent} {
        setFixedSize(size, size);
        setIconSize(QSize(icon_size, icon_size));
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::TabFocus);
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QPalette& pal = palette();
        const QColor text = pal.color(QPalette::WindowText);
        const bool hovered = underMouse();
        const bool pressed = isDown();

        const QColor fill = pressed ? WithAlpha(text, 45)
                                    : (hovered ? WithAlpha(text, 25) : QColor(Qt::transparent));
        const QColor border = WithAlpha(text, 60);

        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(box, 8, 8);

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(hasFocus() ? pal.color(QPalette::Highlight) : border, 1));
        p.drawRoundedRect(box, 8, 8);

        const QSize glyph = iconSize();
        const QRect glyph_rect(
            QPoint((width() - glyph.width()) / 2, (height() - glyph.height()) / 2), glyph);
        const QIcon::Mode mode =
            !isEnabled() ? QIcon::Disabled : (hovered ? QIcon::Active : QIcon::Normal);
        icon().paint(&p, glyph_rect, Qt::AlignCenter, mode, QIcon::Off);
    }

    void enterEvent(QEnterEvent* event) override {
        update();
        QToolButton::enterEvent(event);
    }
    void leaveEvent(QEvent* event) override {
        update();
        QToolButton::leaveEvent(event);
    }
};

} // namespace

// TODO: Remove GameList dependence?
GameListSearchField::KeyReleaseEater::KeyReleaseEater(GameList* gamelist_, QObject* parent)
    : QObject(parent), gamelist{gamelist_} {}

// EventFilter in order to process systemkeys while editing the searchfield
bool GameListSearchField::KeyReleaseEater::eventFilter(QObject* obj, QEvent* event) {
    // If it isn't a KeyRelease event then continue with standard event processing
    if (event->type() != QEvent::KeyRelease)
        return QObject::eventFilter(obj, event);

    QKeyEvent* keyEvent = static_cast<QKeyEvent*>(event);
    QString edit_filter_text = gamelist->search_field->edit_filter->text().toLower();

    // If the searchfield's text hasn't changed special function keys get checked
    // If no function key changes the searchfield's text the filter doesn't need to get reloaded
    if (edit_filter_text == edit_filter_text_old) {
        switch (keyEvent->key()) {
        // Escape: Resets the searchfield
        case Qt::Key_Escape: {
            if (edit_filter_text_old.isEmpty()) {
                return QObject::eventFilter(obj, event);
            } else {
                gamelist->search_field->edit_filter->clear();
                edit_filter_text.clear();
            }
            break;
        }
        // Return and Enter
        // If the enter key gets pressed first checks how many and which entry is visible
        // If there is only one result launch this game
        case Qt::Key_Return:
        case Qt::Key_Enter: {
            if (gamelist->search_field->visible == 1) {
                const QString file_path = gamelist->GetLastFilterResultItem();

                // To avoid loading error dialog loops while confirming them using enter
                // Also users usually want to run a different game after closing one
                gamelist->search_field->edit_filter->clear();
                edit_filter_text.clear();
                emit gamelist->GameChosen(file_path);
            } else {
                return QObject::eventFilter(obj, event);
            }
            break;
        }
        default:
            return QObject::eventFilter(obj, event);
        }
    }
    edit_filter_text_old = edit_filter_text;
    return QObject::eventFilter(obj, event);
}

void GameListSearchField::setFilterResult(int visible_, int total_) {
    visible = visible_;
    total = total_;

    label_filter_result->setText(tr("%1 of %n result(s)", "", total).arg(visible));
}

QString GameListSearchField::filterText() const {
    return edit_filter->text();
}

void GameListSearchField::clear() {
    edit_filter->clear();
}

void GameListSearchField::setFocus() {
    if (edit_filter->isVisible()) {
        edit_filter->setFocus();
    }
}

GameListSearchField::GameListSearchField(GameList* parent) : QWidget{parent} {
    auto* const key_release_eater = new KeyReleaseEater(parent, this);
    layout_filter = new QHBoxLayout;
    layout_filter->setContentsMargins(SideMargin, 8, SideMargin, 8);
    layout_filter->setSpacing(ControlSpacing);
    label_filter = new QLabel;
    edit_filter = new QLineEdit;
    edit_filter->clear();
    edit_filter->installEventFilter(key_release_eater);
    edit_filter->setClearButtonEnabled(true);
    connect(edit_filter, &QLineEdit::textChanged, parent, &GameList::OnTextChanged);
    label_filter_result = new QLabel;
    button_filter_close = new FilterCloseButton(CloseButtonSize, CloseIconSize, this);
    connect(button_filter_close, &QToolButton::clicked, parent, &GameList::OnFilterCloseClicked);
    layout_filter->addWidget(label_filter);
    layout_filter->addWidget(edit_filter);
    layout_filter->addWidget(label_filter_result);
    layout_filter->addWidget(button_filter_close);
    setLayout(layout_filter);
    RefreshResultPalette();
    RefreshStyles();
    RetranslateUI();
}

void GameListSearchField::RefreshResultPalette() {
    // Muted like the details panel's secondary text.
    QPalette pal = label_filter_result->palette();
    pal.setColor(QPalette::WindowText,
                 WithAlpha(palette().color(QPalette::WindowText), 160));
    label_filter_result->setPalette(pal);
}

void GameListSearchField::RefreshStyles() {
    const QPalette& pal = palette();
    edit_filter->setStyleSheet(LineEditStyle(pal));
    // The close glyph is palette-derived, so it is re-derived along with the styles.
    button_filter_close->setIcon(
        HomeGlyphs::Close(pal.color(QPalette::WindowText), devicePixelRatioF(), CloseIconSize));
    button_filter_close->update();
}

void GameListSearchField::changeEvent(QEvent* event) {
    if (event->type() == QEvent::LanguageChange) {
        RetranslateUI();
    }
    if (event->type() == QEvent::PaletteChange) {
        RefreshResultPalette();
        // Setting child style sheets while Qt is still applying the change crashes it, so the
        // styles follow on the next event loop turn.
        if (!style_refresh_queued) {
            style_refresh_queued = true;
            QMetaObject::invokeMethod(
                this,
                [this] {
                    style_refresh_queued = false;
                    RefreshStyles();
                },
                Qt::QueuedConnection);
        }
    }

    QWidget::changeEvent(event);
}

void GameListSearchField::RetranslateUI() {
    label_filter->setText(tr("Filter:"));
    edit_filter->setPlaceholderText(tr("Enter pattern to filter"));
    button_filter_close->setToolTip(tr("Hide Filter Bar"));
}
