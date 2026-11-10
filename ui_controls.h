#pragma once

#include <windows.h>

#include <array>
#include <string_view>

namespace cw::ui {

enum class ButtonKind { Tab, Action, Primary, Sort };

struct Palette {
    COLORREF background;
    COLORREF surface;
    COLORREF border;
    COLORREF ink;
    COLORREF secondary;
    COLORREF muted;
    COLORREF grid;
    COLORREF chartBg;
    COLORREF blue;
    COLORREF teal;
    COLORREF amber;
    COLORREF green;
    COLORREF blueTint;
    COLORREF greenTint;
    COLORREF amberTint;
    std::array<COLORREF, 5> cardColors;
    COLORREF brandGlyph;
    COLORREF primaryFill;
    COLORREF primaryPressed;
    COLORREF selectionFill;
    COLORREF pressedFill;
};

inline constexpr Palette LightPalette{
    .background = RGB(242, 245, 250),
    .surface = RGB(255, 255, 255),
    .border = RGB(225, 231, 240),
    .ink = RGB(23, 38, 59),
    .secondary = RGB(83, 100, 122),
    .muted = RGB(96, 112, 135),
    .grid = RGB(225, 231, 240),
    .chartBg = RGB(246, 249, 253),
    .blue = RGB(33, 86, 207),
    .teal = RGB(0, 125, 112),
    .amber = RGB(143, 85, 21),
    .green = RGB(19, 113, 72),
    .blueTint = RGB(234, 240, 255),
    .greenTint = RGB(234, 246, 239),
    .amberTint = RGB(255, 246, 228),
    .cardColors = {RGB(33, 86, 207), RGB(0, 125, 112), RGB(46, 107, 166), RGB(105, 68, 179), RGB(38, 99, 184)},
    .brandGlyph = RGB(255, 255, 255),
    .primaryFill = RGB(33, 86, 207),
    .primaryPressed = RGB(23, 67, 167),
    .selectionFill = RGB(234, 240, 255),
    .pressedFill = RGB(219, 230, 253),
};

inline constexpr Palette DarkPalette{
    .background = RGB(14, 23, 40),
    .surface = RGB(23, 36, 58),
    .border = RGB(90, 111, 141),
    .ink = RGB(236, 242, 251),
    .secondary = RGB(182, 196, 217),
    .muted = RGB(154, 173, 200),
    .grid = RGB(50, 69, 97),
    .chartBg = RGB(17, 29, 49),
    .blue = RGB(143, 177, 255),
    .teal = RGB(113, 224, 204),
    .amber = RGB(255, 207, 128),
    .green = RGB(136, 220, 173),
    .blueTint = RGB(30, 54, 95),
    .greenTint = RGB(23, 61, 52),
    .amberTint = RGB(72, 51, 33),
    .cardColors = {RGB(143, 177, 255), RGB(113, 224, 204), RGB(131, 201, 248), RGB(202, 164, 255), RGB(118, 203, 214)},
    .brandGlyph = RGB(255, 255, 255),
    .primaryFill = RGB(44, 85, 184),
    .primaryPressed = RGB(37, 72, 157),
    .selectionFill = RGB(32, 55, 98),
    .pressedFill = RGB(38, 67, 112),
};

inline constexpr const Palette& palette(bool dark) {
    return dark ? DarkPalette : LightPalette;
}

namespace detail {

struct Colors {
    COLORREF backdrop;
    COLORREF fill;
    COLORREF border;
    COLORREF text;
    COLORREF focus;
};

inline int px(int logical, int dpi) {
    return MulDiv(logical, dpi > 0 ? dpi : 96, 96);
}

inline bool highContrast() {
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

inline bool keyboardFocus(UINT state) {
    return (state & ODS_FOCUS) && !(state & ODS_NOFOCUSRECT) && !(state & ODS_DISABLED);
}

inline void fill(HDC dc, const RECT& bounds, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void rounded(HDC dc, RECT bounds, int radius, COLORREF fillColor, COLORREF borderColor) {
    SelectObject(dc, GetStockObject(DC_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, fillColor);
    SetDCPenColor(dc, borderColor);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
}

inline void text(HDC dc, HFONT font, std::wstring_view value, RECT bounds,
    COLORREF color, UINT alignment, UINT state) {
    SelectObject(dc, font ? static_cast<HGDIOBJ>(font) : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    const UINT cue = (state & ODS_NOACCEL) ? DT_HIDEPREFIX : 0;
    DrawTextW(dc, value.data(), static_cast<int>(value.size()), &bounds,
        alignment | cue | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

inline void focus(HDC dc, RECT bounds, int dpi, COLORREF color, bool contrast) {
    InflateRect(&bounds, -px(3, dpi), -px(3, dpi));
    if (contrast) {
        DrawFocusRect(dc, &bounds);
        return;
    }
    SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    const int radius = px(6, dpi);
    RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
}

inline Colors buttonColors(ButtonKind kind, bool selected, UINT state, bool contrast, const Palette& theme) {
    const bool disabled = (state & ODS_DISABLED) != 0;
    const bool pressed = (state & ODS_SELECTED) != 0;
    const bool hot = (state & ODS_HOTLIGHT) != 0;
    const bool primary = kind == ButtonKind::Primary;
    if (contrast) {
        const auto backdrop = GetSysColor(kind == ButtonKind::Tab ? COLOR_BTNFACE : COLOR_WINDOW);
        const bool highlighted = !disabled && (primary || selected || pressed);
        const auto text = GetSysColor(disabled ? COLOR_GRAYTEXT : highlighted ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
        return {backdrop, highlighted ? GetSysColor(COLOR_HIGHLIGHT) : backdrop, text, text, text};
    }
    const auto backdrop = kind == ButtonKind::Tab ? theme.background : theme.surface;
    Colors colors{backdrop, backdrop, backdrop, kind == ButtonKind::Sort ? theme.secondary : theme.ink, theme.blue};
    if (disabled) {
        colors.text = theme.secondary;
        if (primary || kind == ButtonKind::Action) {
            colors.fill = theme.background;
            colors.border = theme.border;
        }
        return colors;
    }
    if (primary) {
        colors.fill = pressed || hot ? theme.primaryPressed : theme.primaryFill;
        colors.border = colors.fill;
        colors.text = theme.brandGlyph;
        colors.focus = theme.brandGlyph;
    } else if (selected || pressed || hot) {
        colors.fill = pressed ? theme.pressedFill : theme.selectionFill;
        colors.border = colors.fill;
        colors.text = theme.blue;
    } else if (kind == ButtonKind::Action) {
        colors.border = theme.border;
    }
    if ((state & ODS_DEFAULT) && !primary) {
        colors.border = theme.blue;
    }
    return colors;
}

} // namespace detail

inline bool drawButton(const DRAWITEMSTRUCT& item, int dpi, ButtonKind kind, bool selected,
    HFONT font, std::wstring_view caption, const Palette& theme = LightPalette) {
    if (item.CtlType != ODT_BUTTON || !item.hDC) {
        return false;
    }
    const int saved = SaveDC(item.hDC);
    if (!saved) {
        return false;
    }
    const bool contrast = detail::highContrast();
    const auto colors = detail::buttonColors(kind, selected, item.itemState, contrast, theme);
    detail::fill(item.hDC, item.rcItem, colors.backdrop);
    if (colors.fill != colors.backdrop || colors.border != colors.backdrop) {
        detail::rounded(item.hDC, item.rcItem, detail::px(12, dpi), colors.fill, colors.border);
    }

    RECT bounds = item.rcItem;
    InflateRect(&bounds, -detail::px(7, dpi), 0);
    if (item.itemState & ODS_SELECTED) {
        OffsetRect(&bounds, 0, detail::px(1, dpi));
    }
    detail::text(item.hDC, font, caption, bounds, colors.text,
        kind == ButtonKind::Sort ? DT_RIGHT : DT_CENTER, item.itemState);
    if (detail::keyboardFocus(item.itemState)) {
        detail::focus(item.hDC, item.rcItem, dpi, colors.focus, contrast);
    }
    RestoreDC(item.hDC, saved);
    return true;
}

} // namespace cw::ui
