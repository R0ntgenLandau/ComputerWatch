#pragma once

#include <windows.h>

#include <string_view>

namespace cw::ui {

enum class ButtonKind { Tab, Action, Primary, Sort };

namespace detail {

constexpr COLORREF Canvas = RGB(242, 245, 250);
constexpr COLORREF Card = RGB(255, 255, 255);
constexpr COLORREF Border = RGB(225, 231, 240);
constexpr COLORREF Ink = RGB(23, 38, 59);
constexpr COLORREF Muted = RGB(83, 100, 122);
constexpr COLORREF Blue = RGB(33, 86, 207);
constexpr COLORREF Selection = RGB(234, 240, 255);
constexpr COLORREF Pressed = RGB(219, 230, 253);
constexpr COLORREF StrongBlue = RGB(23, 67, 167);

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

inline Colors buttonColors(ButtonKind kind, bool selected, UINT state, bool contrast) {
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
    const auto backdrop = kind == ButtonKind::Tab ? Canvas : Card;
    Colors colors{backdrop, backdrop, backdrop, kind == ButtonKind::Sort ? Muted : Ink, Blue};
    if (disabled) {
        colors.text = Muted;
        if (primary || kind == ButtonKind::Action) {
            colors.fill = Canvas;
            colors.border = Border;
        }
        return colors;
    }
    if (primary) {
        colors.fill = pressed || hot ? StrongBlue : Blue;
        colors.border = colors.fill;
        colors.text = Card;
        colors.focus = Card;
    } else if (selected || pressed || hot) {
        colors.fill = pressed ? Pressed : Selection;
        colors.border = colors.fill;
        colors.text = Blue;
    } else if (kind == ButtonKind::Action) {
        colors.border = Border;
    }
    if ((state & ODS_DEFAULT) && !primary) {
        colors.border = Blue;
    }
    return colors;
}

} // namespace detail

inline bool drawButton(const DRAWITEMSTRUCT& item, int dpi, ButtonKind kind, bool selected,
    HFONT font, std::wstring_view caption) {
    if (item.CtlType != ODT_BUTTON || !item.hDC) {
        return false;
    }
    const int saved = SaveDC(item.hDC);
    if (!saved) {
        return false;
    }
    const bool contrast = detail::highContrast();
    const auto colors = detail::buttonColors(kind, selected, item.itemState, contrast);
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
