#include "ui.h"
#include "ui_controls.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cwchar>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace cw {
namespace {

constexpr wchar_t WindowClass[] = L"ComputerWatchDashboard";
constexpr UINT ShowMessage = WM_APP + 1;
constexpr UINT TrayMessage = WM_APP + 2;
constexpr UINT ButtonMessage = WM_APP + 3;
constexpr UINT_PTR RefreshTimer = 1;
constexpr int OpenTray = 1;
constexpr int StartCpuTray = 2;
constexpr int ExitTray = 3;
constexpr int64_t MinuteMs = 60'000;
constexpr int64_t HourMs = 60 * MinuteMs;
constexpr int64_t DayMs = 24 * HourMs;
constexpr int64_t EpochOffset = 116'444'736'000'000'000;
constexpr std::array<int64_t, 4> Ranges{MinuteMs, HourMs, DayMs, 7 * DayMs};

constexpr COLORREF Background = RGB(242, 245, 250);
constexpr COLORREF Surface = RGB(255, 255, 255);
constexpr COLORREF Ink = RGB(23, 38, 59);
constexpr COLORREF Secondary = RGB(83, 100, 122);
constexpr COLORREF Muted = RGB(96, 112, 135);
constexpr COLORREF Grid = RGB(225, 231, 240);
constexpr COLORREF ChartBg = RGB(246, 249, 253);
constexpr COLORREF Blue = RGB(33, 86, 207);
constexpr COLORREF Teal = RGB(0, 125, 112);
constexpr COLORREF Amber = RGB(143, 85, 21);
constexpr COLORREF Green = RGB(19, 113, 72);
constexpr COLORREF BlueTint = RGB(234, 240, 255);
constexpr COLORREF GreenTint = RGB(234, 246, 239);
constexpr COLORREF AmberTint = RGB(255, 246, 228);
constexpr std::array CardColors{Blue, Teal, RGB(46, 107, 166), RGB(105, 68, 179), RGB(38, 99, 184)};

constexpr int PageInset = 24;
constexpr int CardInset = 14;
constexpr int RankInset = 14;
constexpr int RankCpuWidth = 72;
constexpr int RankMemoryWidth = 92;
constexpr int RankHeaderOffset = 38;
constexpr int RankHeaderHeight = 26;

constexpr std::pair<const wchar_t*, int> CoreColumns[] = {
    {L"处理器组", 130}, {L"逻辑处理器", 130}, {L"占用率", 160}, {L"频率（估算）", 160}
};
constexpr std::pair<const wchar_t*, int> DiskColumns[] = {
    {L"磁盘", 54}, {L"型号", 195}, {L"总线", 64}, {L"健康状态", 80},
    {L"温度", 70}, {L"损耗", 70}, {L"通电时长", 100}, {L"通电次数", 84},
    {L"不可纠正 / 介质错误", 154}
};

enum class Tab { Live, History, Settings, CpuDetails, GpuDetails, DiskDetails };
enum class RankField { Cpu, Memory };

struct RankSort {
    RankField field;
    bool ascending = false;
};

enum ControlId {
    LiveButton = 100, HistoryButton, SettingsButton, CpuCombo, GpuCombo,
    HistoryPicker, RangeCombo, QueryButton, RetentionEdit, SaveButton,
    CpuDetailButton, GpuDetailButton, DiskDetailButton, DetailBackButton, CoreList, DiskList,
    LeftSortCpu, LeftSortMemory, RightSortCpu, RightSortMemory
};

const wchar_t* buttonCaption(unsigned id) {
    switch (id) {
    case LiveButton: return L"实时监控";
    case HistoryButton: return L"历史记录";
    case SettingsButton: return L"设置";
    case CpuDetailButton:
    case GpuDetailButton: return L"详情";
    case DiskDetailButton: return L"SMART";
    case DetailBackButton: return L"返回实时监控";
    case QueryButton: return L"查看";
    case SaveButton: return L"保存";
    default: return nullptr;
    }
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring decimal(double value, int places, const wchar_t* unit) {
    if (!std::isfinite(value)) {
        return L"—";
    }
    wchar_t buffer[80]{};
    std::swprintf(buffer, std::size(buffer), L"%.*f%ls", places, value, unit);
    return buffer;
}

std::wstring bytes(double value, bool rate = false) {
    if (!std::isfinite(value) || value < 0) {
        return L"—";
    }
    constexpr const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    size_t index = 0;
    while (value >= 1024 && index + 1 < std::size(units)) {
        value /= 1024;
        ++index;
    }
    auto text = decimal(value, index == 0 ? 0 : 1, L" ");
    text += units[index];
    if (rate) {
        text += L"/s";
    }
    return text;
}

SYSTEMTIME localTime(int64_t timestamp) {
    ULARGE_INTEGER ticks{};
    ticks.QuadPart = static_cast<uint64_t>(timestamp * 10'000 + EpochOffset);
    const FILETIME fileTime{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc{}, local{};
    FileTimeToSystemTime(&fileTime, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}

std::optional<int64_t> fromLocalTime(SYSTEMTIME local) {
    SYSTEMTIME utc{};
    FILETIME fileTime{};
    if (!TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) ||
        !SystemTimeToFileTime(&utc, &fileTime)) {
        return std::nullopt;
    }
    ULARGE_INTEGER ticks{};
    ticks.LowPart = fileTime.dwLowDateTime;
    ticks.HighPart = fileTime.dwHighDateTime;
    return (static_cast<int64_t>(ticks.QuadPart) - EpochOffset) / 10'000;
}

std::wstring timeText(int64_t timestamp, bool withDate = false) {
    const auto time = localTime(timestamp);
    wchar_t buffer[32]{};
    if (withDate) {
        std::swprintf(buffer, std::size(buffer), L"%04u-%02u-%02u %02u:%02u:%02u",
            time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    } else {
        std::swprintf(buffer, std::size(buffer), L"%02u:%02u:%02u",
            time.wHour, time.wMinute, time.wSecond);
    }
    return buffer;
}

void fill(HDC dc, const RECT& area, COLORREF color) {
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void rounded(HDC dc, RECT area, int diameter, COLORREF background, COLORREF border) {
    const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCBrushColor(dc, background);
    SetDCPenColor(dc, border);
    RoundRect(dc, area.left, area.top, area.right, area.bottom, diameter, diameter);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
}

int textWidth(HDC dc, HFONT font, std::wstring_view text) {
    SIZE extent{};
    const auto old = SelectObject(dc, font);
    GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &extent);
    SelectObject(dc, old);
    return extent.cx;
}

void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    const auto old = SelectObject(dc, GetStockObject(DC_PEN));
    SetDCPenColor(dc, color);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, old);
}

void label(HDC dc, HFONT font, std::wstring_view text, RECT area, COLORREF color = Ink, UINT align = DT_LEFT) {
    if (text.empty()) {
        return;
    }
    const auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, const_cast<wchar_t*>(text.data()), static_cast<int>(text.size()), &area,
        align | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void paragraph(HDC dc, HFONT font, std::wstring_view text, RECT area, COLORREF color) {
    const auto old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, const_cast<wchar_t*>(text.data()), static_cast<int>(text.size()), &area,
        DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

struct Chart {
    Metric primary;
    std::optional<Metric> secondary;
    bool percent;
};

class Dashboard {
public:
    Dashboard(HINSTANCE instance, UiCallbacks callbacks)
        : instance_(instance), callbacks_(std::move(callbacks)) {}

    ~Dashboard() {
        for (auto font : {normalFont_, smallFont_, titleFont_, valueFont_, boldFont_}) {
            if (font) {
                DeleteObject(font);
            }
        }
    }

    bool create(bool startVisible) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = WindowClass;
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        dpi_ = static_cast<int>(GetDpiForSystem());
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"Computer Watch · 电脑监控",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            px(1130), px(750), nullptr, nullptr, instance_, this);
        if (!window_) {
            return false;
        }
        if (startVisible || !trayAdded_) {
            show();
        } else {
            updateVisibility(false);
        }
        return true;
    }

    HWND window() const { return window_; }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            const auto self = static_cast<Dashboard*>(create->lpCreateParams);
            self->window_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        const auto self = reinterpret_cast<Dashboard*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        return self ? self->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
    }

    int px(int logical) const { return MulDiv(logical, dpi_, 96); }

    HFONT font(int size, int weight = FW_NORMAL, const wchar_t* family = L"Microsoft YaHei UI") const {
        return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
    }

    HWND control(const wchar_t* className, const wchar_t* text, DWORD style, int id) {
        const auto child = CreateWindowExW(0, className, text, WS_CHILD | WS_TABSTOP | style,
            0, 0, 0, 0, window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont_), FALSE);
        return child;
    }

    HWND combo(int id, std::initializer_list<const wchar_t*> options) {
        const auto child = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, id);
        for (auto option : options) {
            SendMessageW(child, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
        }
        SendMessageW(child, CB_SETCURSEL, 0, 0);
        return child;
    }

    HWND button(ControlId id) {
        return control(WC_BUTTONW, buttonCaption(id), BS_OWNERDRAW, id);
    }

    template<size_t N>
    HWND detailList(int id, const std::pair<const wchar_t*, int> (&columns)[N]) {
        const auto child = control(WC_LISTVIEWW, L"",
            LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS, id);
        SendMessageW(child, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessageW(child, LVM_SETTEXTCOLOR, 0, Ink);
        SendMessageW(child, LVM_SETBKCOLOR, 0, Surface);
        SendMessageW(child, LVM_SETTEXTBKCOLOR, 0, Surface);
        int index = 0;
        for (const auto& [name, width] : columns) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(name);
            column.cx = px(width);
            SendMessageW(child, LVM_INSERTCOLUMNW, index++, reinterpret_cast<LPARAM>(&column));
        }
        return child;
    }

    template<size_t N>
    void scaleColumns(HWND list, const std::pair<const wchar_t*, int> (&columns)[N]) {
        for (size_t i = 0; i < N; ++i) {
            SendMessageW(list, LVM_SETCOLUMNWIDTH, i, px(columns[i].second));
        }
    }

    void refreshFonts() {
        const auto oldFonts = std::array{normalFont_, smallFont_, titleFont_, valueFont_, boldFont_};
        normalFont_ = font(13);
        smallFont_ = font(12);
        titleFont_ = font(26, FW_SEMIBOLD, L"Segoe UI");
        valueFont_ = font(29, FW_SEMIBOLD, L"Segoe UI");
        boldFont_ = font(15, FW_SEMIBOLD);
        for (const auto child : {liveButton_, historyButton_, settingsButton_, cpuCombo_, gpuCombo_, rangeCombo_,
            cpuDetailButton_, gpuDetailButton_, diskDetailButton_, detailBackButton_, picker_, queryButton_,
            retentionEdit_, saveButton_, coreList_, diskList_}) {
            if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(normalFont_), TRUE);
        }
        for (const auto child : sortButtons_) {
            if (child) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), TRUE);
        }
        for (const auto old : oldFonts) {
            if (old) DeleteObject(old);
        }
    }

    void initControls() {
        refreshFonts();
        liveButton_ = button(LiveButton);
        historyButton_ = button(HistoryButton);
        settingsButton_ = button(SettingsButton);
        cpuCombo_ = combo(CpuCombo, {L"占用率曲线", L"封装温度曲线", L"封装功耗曲线"});
        gpuCombo_ = combo(GpuCombo, {L"占用率曲线", L"温度曲线", L"板卡功耗曲线"});
        cpuDetailButton_ = button(CpuDetailButton);
        gpuDetailButton_ = button(GpuDetailButton);
        diskDetailButton_ = button(DiskDetailButton);
        detailBackButton_ = button(DetailBackButton);
        picker_ = control(DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT, HistoryPicker);
        SendMessageW(picker_, DTM_SETFORMATW, 0, reinterpret_cast<LPARAM>(L"yyyy-MM-dd HH:mm:ss"));
        rangeCombo_ = combo(RangeCombo, {L"60 秒", L"1 小时", L"24 小时", L"7 天"});
        queryButton_ = button(QueryButton);
        retentionEdit_ = control(WC_EDITW, L"7", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, RetentionEdit);
        SendMessageW(retentionEdit_, EM_SETLIMITTEXT, 4, 0);
        saveButton_ = button(SaveButton);
        for (int i = 0; i < static_cast<int>(sortButtons_.size()); ++i) {
            sortButtons_[i] = control(WC_BUTTONW, L"", BS_OWNERDRAW, LeftSortCpu + i);
            SendMessageW(sortButtons_[i], WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), FALSE);
        }
        updateSortHeaders();
        coreList_ = detailList(CoreList, CoreColumns);
        diskList_ = detailList(DiskList, DiskColumns);
    }

    void move(HWND control, int x, int y, int width, int height, bool showControl = true) {
        const bool wasVisible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
        const UINT visibility = showControl == wasVisible ? 0 : showControl ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
        SetWindowPos(control, nullptr, x, y, std::max(width, 0), std::max(height, 0),
            SWP_NOZORDER | SWP_NOACTIVATE | visibility);
    }

    DetailPane detailPane() const {
        if (tab_ == Tab::CpuDetails) {
            return DetailPane::Cpu;
        }
        if (tab_ == Tab::GpuDetails) {
            return DetailPane::Gpu;
        }
        if (tab_ == Tab::DiskDetails) {
            return DetailPane::Disk;
        }
        return DetailPane::None;
    }

    RECT rankPanel(RECT area, size_t index) const {
        const int top = cards_[0].bottom + px(27);
        const int bottom = std::min(static_cast<int>(area.bottom) - px(16), top + px(350));
        return index == 0
            ? RECT{px(PageInset), top, area.right / 2 - px(6), bottom}
            : RECT{area.right / 2 + px(6), top, area.right - px(PageInset), bottom};
    }

    void layout() {
        RECT area{};
        GetClientRect(window_, &area);
        const int pad = px(PageInset);
        const int gap = px(12);
        const int contentWidth = std::max(5, static_cast<int>(area.right) - 2 * pad - 4 * gap);
        const int cardY = px(tab_ == Tab::History ? 153 : 116);
        for (size_t i = 0; i < cards_.size(); ++i) {
            const int index = static_cast<int>(i);
            const int x = pad + index * gap + contentWidth * index / 5;
            const int right = pad + index * gap + contentWidth * (index + 1) / 5;
            cards_[i] = {x, cardY, right, cardY + px(190)};
        }
        const auto detail = detailPane();
        const bool inDetail = detail != DetailPane::None;
        const bool live = tab_ == Tab::Live;
        const bool history = tab_ == Tab::History;
        const bool settings = tab_ == Tab::Settings;
        const bool overview = live || history;
        move(liveButton_, pad, px(70), px(90), px(31), !inDetail);
        move(historyButton_, pad + px(96), px(70), px(90), px(31), !inDetail);
        move(settingsButton_, pad + px(192), px(70), px(64), px(31), !inDetail);
        move(detailBackButton_, pad, px(70), px(122), px(30), inDetail);
        move(cpuCombo_, cards_[0].left + px(CardInset), cardY + px(106), px(120), px(130), overview);
        move(gpuCombo_, cards_[3].left + px(CardInset), cardY + px(106), px(120), px(130), overview);
        move(cpuDetailButton_, cards_[0].right - px(CardInset + 49), cardY + px(10), px(49), px(24), live);
        move(gpuDetailButton_, cards_[3].right - px(CardInset + 49), cardY + px(10), px(49), px(24), live);
        move(diskDetailButton_, cards_[2].right - px(CardInset + 64), cardY + px(10), px(64), px(24), live);
        move(picker_, pad + px(70), px(110), px(202), px(27), history);
        move(rangeCombo_, pad + px(281), px(110), px(92), px(120), history);
        move(queryButton_, pad + px(383), px(110), px(68), px(27), history);
        move(retentionEdit_, pad + px(116), px(180), px(68), px(29), settings);
        move(saveButton_, pad + px(224), px(180), px(74), px(29), settings);
        for (size_t index = 0; index < rankSorts_.size(); ++index) {
            const auto panel = rankPanel(area, index);
            const int right = panel.right - px(RankInset);
            const int headerY = panel.top + px(RankHeaderOffset);
            const int cpuWidth = px(RankCpuWidth);
            const int memoryWidth = px(RankMemoryWidth);
            move(sortButtons_[2 * index], right - memoryWidth - cpuWidth, headerY,
                cpuWidth, px(RankHeaderHeight), overview);
            move(sortButtons_[2 * index + 1], right - memoryWidth, headerY,
                memoryWidth, px(RankHeaderHeight), overview);
        }
        move(coreList_, pad + px(12), px(281), static_cast<int>(area.right) - 2 * pad - px(24),
            std::max(0, static_cast<int>(area.bottom) - px(302)), detail == DetailPane::Cpu && !details_.cores.empty());
        move(diskList_, pad + px(12), px(185), static_cast<int>(area.right) - 2 * pad - px(24),
            std::max(0, static_cast<int>(area.bottom) - px(275)), detail == DetailPane::Disk && !details_.disks.empty());
    }

    void invalidate() const { InvalidateRect(window_, nullptr, FALSE); }

    void addTrayIcon() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = TrayMessage;
        tray_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        lstrcpynW(tray_.szTip, L"Computer Watch · 电脑监控", static_cast<int>(std::size(tray_.szTip)));
        trayAdded_ = Shell_NotifyIconW(NIM_ADD, &tray_) != FALSE;
        trayV4_ = false;
        if (trayAdded_) {
            tray_.uVersion = NOTIFYICON_VERSION_4;
            trayV4_ = Shell_NotifyIconW(NIM_SETVERSION, &tray_) != FALSE;
        }
    }

    void show() {
        ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(window_);
        updateVisibility(true);
    }

    void trayEvent(WPARAM wParam, LPARAM lParam) {
        const auto event = trayV4_ ? LOWORD(lParam) : static_cast<UINT>(lParam);
        if (event == NIN_SELECT || event == NIN_KEYSELECT || event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
            show();
        } else if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            if (!menu) {
                return;
            }
            AppendMenuW(menu, MF_STRING, OpenTray, L"打开监控面板");
            if (callbacks_.startCpu) {
                AppendMenuW(menu, MF_STRING, StartCpuTray, L"启动硬件传感器");
            }
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, ExitTray, L"退出并停止采集");
            POINT point{};
            if (trayV4_ && event == WM_CONTEXTMENU) {
                point = {GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
            } else {
                GetCursorPos(&point);
            }
            SetForegroundWindow(window_);
            const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                point.x, point.y, 0, window_, nullptr);
            DestroyMenu(menu);
            PostMessageW(window_, WM_NULL, 0, 0);
            switch (choice) {
            case OpenTray:
                show();
                break;
            case StartCpuTray:
                try {
                    callbacks_.startCpu();
                } catch (...) {
                    MessageBoxW(window_, L"无法启动硬件传感器。", L"Computer Watch", MB_OK | MB_ICONWARNING);
                }
                break;
            case ExitTray:
                try {
                    if (callbacks_.stop) {
                        callbacks_.stop();
                    }
                } catch (...) {
                    MessageBoxW(window_, L"无法停止采集。", L"Computer Watch", MB_OK | MB_ICONWARNING);
                    return;
                }
                DestroyWindow(window_);
                break;
            }
        }
    }

    void updateVisibility(bool visible) {
        if (visible_ && visible_.value() == visible) {
            return;
        }
        visible_ = visible;
        if (callbacks_.visibility) {
            try {
                callbacks_.visibility(visible);
            } catch (...) {
                OutputDebugStringW(L"Computer Watch: visibility callback failed.");
            }
        }
        syncDetailRequest();
        if (visible) {
            SetTimer(window_, RefreshTimer, 1000, nullptr);
            if (tab_ == Tab::Live || detailPane() != DetailPane::None) {
                refreshLive();
            }
        } else {
            KillTimer(window_, RefreshTimer);
        }
    }

    void updateDetailList(HWND list, size_t previous, size_t count, bool active) {
        if (previous != count) {
            SendMessageW(list, LVM_SETITEMCOUNT, count, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
        }
        const bool visible = active && count;
        if (((GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0) != visible) {
            ShowWindow(list, visible ? SW_SHOWNA : SW_HIDE);
        }
        if (visible) {
            InvalidateRect(list, nullptr, FALSE);
        }
    }

    void replaceDetails(HardwareDetails details) {
        const size_t previousCores = details_.cores.size();
        const size_t previousDisks = details_.disks.size();
        details_ = std::move(details);
        updateDetailList(coreList_, previousCores, details_.cores.size(), detailPane() == DetailPane::Cpu);
        updateDetailList(diskList_, previousDisks, details_.disks.size(), detailPane() == DetailPane::Disk);
    }

    void syncDetailRequest() {
        const auto requested = visible_.value_or(false) ? detailPane() : DetailPane::None;
        if (requested == requestedDetail_) {
            return;
        }
        replaceDetails({});
        try {
            if (callbacks_.selectDetail) {
                callbacks_.selectDetail(requested);
            }
            requestedDetail_ = requested;
        } catch (...) {
            OutputDebugStringW(L"Computer Watch: selecting hardware detail failed.");
        }
    }

    void refreshDetail() {
        const auto pane = detailPane();
        if (pane == DetailPane::None || !visible_.value_or(false) || !callbacks_.details) {
            return;
        }
        syncDetailRequest();
        try {
            auto current = callbacks_.details();
            if (current.pane == pane) {
                replaceDetails(std::move(current));
            }
        } catch (...) {
            replaceDetails({});
        }
    }

    void refreshLive() {
        try {
            live_ = callbacks_.live ? callbacks_.live() : std::vector<Snapshot>{};
            liveFailed_ = false;
        } catch (...) {
            liveFailed_ = true;
        }
        refreshDetail();
        invalidate();
    }

    const wchar_t* sortCaption(size_t index, RankField field) const {
        const auto sort = rankSorts_[index];
        if (field == RankField::Cpu) {
            return sort.field != field ? L"CPU" : sort.ascending ? L"CPU ↑" : L"CPU ↓";
        }
        return sort.field != field ? L"内存" : sort.ascending ? L"内存 ↑" : L"内存 ↓";
    }

    void updateSortHeaders() {
        for (size_t index = 0; index < rankSorts_.size(); ++index) {
            SetWindowTextW(sortButtons_[2 * index], sortCaption(index, RankField::Cpu));
            SetWindowTextW(sortButtons_[2 * index + 1], sortCaption(index, RankField::Memory));
        }
    }

    void sortBy(size_t table, RankField field) {
        auto& sort = rankSorts_[table];
        if (sort.field == field) {
            sort.ascending = !sort.ascending;
        } else {
            sort = {field, false};
        }
        updateSortHeaders();
        invalidate();
    }

    std::wstring detailListValue(HWND list, size_t row, int column) const {
        if (list == coreList_ && row < details_.cores.size()) {
            const auto& core = details_.cores[row];
            switch (column) {
            case 0: return std::to_wstring(core.group);
            case 1: return std::to_wstring(core.number);
            case 2: return decimal(core.loadPercent, 1, L"%");
            case 3: return decimal(core.frequencyMhz / 1000.0, 2, L" GHz");
            }
        } else if (list == diskList_ && row < details_.disks.size()) {
            const auto& disk = details_.disks[row];
            switch (column) {
            case 0: return std::to_wstring(disk.number);
            case 1: return disk.name.empty() ? L"—" : disk.name;
            case 2: return disk.bus.empty() ? L"—" : disk.bus;
            case 3: return disk.healthy ? (*disk.healthy ? L"健康" : L"异常") : L"—";
            case 4: return decimal(disk.temperatureC, 0, L" °C");
            case 5: return decimal(disk.wearPercent, 0, L"%");
            case 6: return decimal(disk.powerOnHours, 0, L" 小时");
            case 7: return decimal(disk.powerCycles, 0, L" 次");
            case 8: return decimal(disk.errors, 0, L"");
            }
        }
        return {};
    }

    void setPicker(int64_t target) const {
        auto local = localTime(target);
        SendMessageW(picker_, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&local));
    }

    void loadSelection(int64_t target) {
        selected_.reset();
        historyTarget_ = target;
        if (callbacks_.at) {
            auto snapshot = callbacks_.at(target);
            if (snapshot && std::abs(snapshot->timestampMs - target) <= 5'500) {
                selected_ = std::move(snapshot);
            }
        }
    }

    void queryHistory() {
        SYSTEMTIME local{};
        if (SendMessageW(picker_, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&local)) != GDT_VALID) {
            return;
        }
        const auto timestamp = fromLocalTime(local);
        if (!timestamp) {
            return;
        }
        const auto selectedRange = SendMessageW(rangeCombo_, CB_GETCURSEL, 0, 0);
        const auto range = selectedRange >= 0 && static_cast<size_t>(selectedRange) < Ranges.size()
            ? Ranges[static_cast<size_t>(selectedRange)] : Ranges[0];
        historyEnd_ = timestamp.value();
        historyStart_ = historyEnd_ - range;
        historyFailed_ = false;
        try {
            history_ = callbacks_.series
                ? callbacks_.series(historyStart_, historyEnd_, 360)
                : std::vector<Snapshot>{};
            loadSelection(historyEnd_);
        } catch (...) {
            history_.clear();
            selected_.reset();
            historyFailed_ = true;
        }
        invalidate();
    }

    RECT chartRect(const RECT& card) const {
        return {card.left + px(CardInset), card.top + px(140), card.right - px(CardInset), card.bottom - px(CardInset)};
    }

    RECT chartPlot(RECT area) const {
        InflateRect(&area, -px(5), -px(3));
        return area;
    }

    void clickChart(int x, int y) {
        if (tab_ != Tab::History || history_.empty()) {
            return;
        }
        const POINT point{x, y};
        for (const auto& card : cards_) {
            const auto chart = chartRect(card);
            if (!PtInRect(&chart, point)) {
                continue;
            }
            const auto plot = chartPlot(chart);
            const auto fraction = std::clamp(
                static_cast<double>(x - plot.left) / std::max(1L, plot.right - plot.left - 1), 0.0, 1.0);
            const auto target = historyStart_ + static_cast<int64_t>(std::llround(fraction * (historyEnd_ - historyStart_)));
            historyFailed_ = false;
            try {
                loadSelection(target);
                setPicker(target);
            } catch (...) {
                historyFailed_ = true;
            }
            invalidate();
            break;
        }
    }

    void activate(Tab target) {
        tab_ = target;
        for (const auto child : {liveButton_, historyButton_, settingsButton_}) {
            InvalidateRect(child, nullptr, FALSE);
        }
        layout();
        syncDetailRequest();
        if (target == Tab::Live || detailPane() != DetailPane::None) {
            if (visible_.value_or(false)) {
                refreshLive();
                SetFocus(detailPane() == DetailPane::None ? liveButton_ : detailBackButton_);
            }
        } else if (target == Tab::History && !historyEnd_) {
            setPicker(nowMs());
            queryHistory();
        } else if (target == Tab::Settings) {
            settingsMessage_.clear();
            try {
                const auto days = callbacks_.retention ? callbacks_.retention() : 7;
                SetWindowTextW(retentionEdit_, std::to_wstring(days).c_str());
            } catch (...) {
                settingsMessage_ = L"读取设置失败。";
            }
        }
        invalidate();
    }

    void saveSettings() {
        BOOL valid = FALSE;
        const UINT days = GetDlgItemInt(window_, RetentionEdit, &valid, FALSE);
        if (!valid || days < 1 || days > 3650) {
            settingsMessage_ = L"请输入 1 到 3650 之间的天数。";
        } else {
            try {
                settingsMessage_ = callbacks_.setRetention && callbacks_.setRetention(static_cast<int>(days))
                    ? L"已保存历史保留时长。" : L"无法保存设置。";
            } catch (...) {
                settingsMessage_ = L"无法保存设置。";
            }
        }
        invalidate();
    }

    bool drawControl(const DRAWITEMSTRUCT& item) const {
        using ui::ButtonKind;
        const auto id = item.CtlID;
        if (id >= LeftSortCpu && id <= RightSortMemory) {
            const auto index = id - LeftSortCpu;
            const auto field = index % 2 == 0 ? RankField::Cpu : RankField::Memory;
            return ui::drawButton(item, dpi_, ButtonKind::Sort,
                rankSorts_[index / 2].field == field, smallFont_, sortCaption(index / 2, field));
        }
        const auto caption = buttonCaption(id);
        if (!caption) return false;
        auto kind = ButtonKind::Action;
        if (id == LiveButton || id == HistoryButton || id == SettingsButton || id == DetailBackButton) {
            kind = ButtonKind::Tab;
        } else if (id == QueryButton || id == SaveButton) {
            kind = ButtonKind::Primary;
        }
        const bool selected = (id == LiveButton && tab_ == Tab::Live) ||
            (id == HistoryButton && tab_ == Tab::History) || (id == SettingsButton && tab_ == Tab::Settings);
        return ui::drawButton(item, dpi_, kind, selected, normalFont_, caption);
    }

    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam) {
        if (taskbarCreated_ && message == taskbarCreated_) {
            addTrayIcon();
            if (!trayAdded_) {
                show();
            }
            return 0;
        }
        switch (message) {
        case WM_CREATE:
            dpi_ = static_cast<int>(GetDpiForWindow(window_));
            taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
            initControls();
            layout();
            addTrayIcon();
            return 0;
        case WM_GETMINMAXINFO: {
            const auto limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {px(1040), px(686)};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_ = LOWORD(wParam);
            refreshFonts();
            scaleColumns(coreList_, CoreColumns);
            scaleColumns(diskList_, DiskColumns);
            const auto& bounds = *reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window_, nullptr, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            invalidate();
            return 0;
        }
        case WM_SIZE:
            layout();
            updateVisibility(wParam != SIZE_MINIMIZED && IsWindowVisible(window_));
            invalidate();
            return 0;
        case WM_SHOWWINDOW:
            updateVisibility(wParam != FALSE && !IsIconic(window_));
            return 0;
        case ShowMessage:
            show();
            return 0;
        case TrayMessage:
            trayEvent(wParam, lParam);
            return 0;
        case WM_TIMER:
            if (wParam == RefreshTimer && (tab_ == Tab::Live || detailPane() != DetailPane::None)
                && visible_.value_or(false)) {
                refreshLive();
            }
            return 0;
        case WM_DRAWITEM:
            if (lParam && drawControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case ButtonMessage:
            switch (wParam) {
            case LiveButton: activate(Tab::Live); break;
            case HistoryButton: activate(Tab::History); break;
            case SettingsButton: activate(Tab::Settings); break;
            case QueryButton: queryHistory(); break;
            case SaveButton: saveSettings(); break;
            case CpuDetailButton: activate(Tab::CpuDetails); break;
            case GpuDetailButton: activate(Tab::GpuDetails); break;
            case DiskDetailButton: activate(Tab::DiskDetails); break;
            case DetailBackButton: activate(Tab::Live); break;
            case LeftSortCpu: sortBy(0, RankField::Cpu); break;
            case LeftSortMemory: sortBy(0, RankField::Memory); break;
            case RightSortCpu: sortBy(1, RankField::Cpu); break;
            case RightSortMemory: sortBy(1, RankField::Memory); break;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED && lParam) {
                // Leave the native control's notification before changing focus or layout.
                PostMessageW(window_, ButtonMessage, LOWORD(wParam), 0);
                return 0;
            }
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (LOWORD(wParam) == CpuCombo || LOWORD(wParam) == GpuCombo) {
                    invalidate();
                } else if (LOWORD(wParam) == RangeCombo) {
                    queryHistory();
                }
                return 0;
            }
            break;
        case WM_NOTIFY: {
            const auto notification = reinterpret_cast<NMHDR*>(lParam);
            if (notification->hwndFrom != coreList_ && notification->hwndFrom != diskList_) {
                break;
            }
            if (notification->code == LVN_GETDISPINFOW) {
                auto& item = reinterpret_cast<NMLVDISPINFOW*>(lParam)->item;
                if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0) {
                    const auto text = detailListValue(notification->hwndFrom, item.iItem, item.iSubItem);
                    lstrcpynW(item.pszText, text.c_str(), item.cchTextMax);
                }
            } else if (notification->hwndFrom == diskList_ &&
                (notification->code == LVN_ITEMCHANGED || notification->code == LVN_ODSTATECHANGED)) {
                invalidate();
            }
            return 0;
        }
        case WM_LBUTTONUP:
            clickChart(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        case WM_CLOSE:
            if (trayAdded_) {
                ShowWindow(window_, SW_HIDE);
                updateVisibility(false);
                if (detailPane() != DetailPane::None) {
                    activate(Tab::Live);
                }
                return 0;
            }
            if (callbacks_.stop) {
                try {
                    callbacks_.stop();
                } catch (...) {
                    MessageBoxW(window_, L"无法停止采集。", L"Computer Watch", MB_OK | MB_ICONWARNING);
                    return 0;
                }
            }
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            updateVisibility(false);
            if (trayAdded_) {
                Shell_NotifyIconW(NIM_DELETE, &tray_);
            }
            PostQuitMessage(0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            paint();
            return 0;
        }
        return DefWindowProcW(window_, message, wParam, lParam);
    }

    double value(const Snapshot* snapshot, Metric metric) const {
        return snapshot ? (*snapshot)[metric] : NAN;
    }

    Chart chartFor(size_t card) const {
        switch (card) {
        case 0:
            switch (SendMessageW(cpuCombo_, CB_GETCURSEL, 0, 0)) {
            case 1: return {Metric::CpuTemp, {}, false};
            case 2: return {Metric::CpuWatts, {}, false};
            default: return {Metric::CpuPercent, {}, true};
            }
        case 1: return {Metric::MemoryPercent, {}, true};
        case 2: return {Metric::DiskRead, Metric::DiskWrite, false};
        case 3:
            switch (SendMessageW(gpuCombo_, CB_GETCURSEL, 0, 0)) {
            case 1: return {Metric::GpuTemp, {}, false};
            case 2: return {Metric::GpuWatts, {}, false};
            default: return {Metric::GpuPercent, {}, true};
            }
        default: return {Metric::NetworkDown, Metric::NetworkUp, false};
        }
    }

    void chartLine(HDC dc, const RECT& area, const std::vector<Snapshot>& samples, Metric metric,
        COLORREF color, int64_t start, int64_t end, double maximum) const {
        const auto pen = CreatePen(PS_SOLID, px(2), color);
        if (!pen) {
            return;
        }
        const auto old = SelectObject(dc, pen);
        bool previous = false;
        int64_t previousTime = 0;
        const auto maxGap = tab_ == Tab::Live ? 7'500 : std::max<int64_t>(
            7'500, 4 * (end - start) / static_cast<int64_t>(std::max<size_t>(samples.size(), 1)));
        for (const auto& sample : samples) {
            if (sample.timestampMs < start || sample.timestampMs > end) {
                continue;
            }
            const double reading = sample[metric];
            if (!std::isfinite(reading)) {
                previous = false;
                continue;
            }
            const auto fraction = static_cast<double>(sample.timestampMs - start) / (end - start);
            const auto yFraction = std::clamp(reading / maximum, 0.0, 1.0);
            const int x = area.left + static_cast<int>(std::lround(fraction * (area.right - area.left - 2)));
            const int y = area.bottom - 2 - static_cast<int>(std::lround(yFraction * (area.bottom - area.top - 3)));
            if (previous && sample.timestampMs - previousTime <= maxGap) {
                LineTo(dc, x, y);
            } else {
                fill(dc, {x, y, x + px(2), y + px(2)}, color);
                MoveToEx(dc, x, y, nullptr);
            }
            previous = true;
            previousTime = sample.timestampMs;
        }
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void paintChart(HDC dc, const RECT& area, Chart chart, const std::vector<Snapshot>& samples,
        int64_t start, int64_t end, COLORREF accent) const {
        rounded(dc, area, px(10), ChartBg, ChartBg);
        const auto plot = chartPlot(area);
        for (int i = 1; i <= 3; ++i) {
            const int x = plot.left + (plot.right - plot.left) * i / 4;
            line(dc, x, plot.top, x, plot.bottom, Grid);
        }
        line(dc, plot.left, (plot.top + plot.bottom) / 2, plot.right, (plot.top + plot.bottom) / 2, Grid);
        line(dc, plot.left, plot.bottom - 1, plot.right, plot.bottom - 1, Grid);
        double maximum = chart.percent ? 100.0 : 1.0;
        bool hasData = false;
        for (const auto& sample : samples) {
            if (sample.timestampMs < start || sample.timestampMs > end) {
                continue;
            }
            const double first = sample[chart.primary];
            const double second = chart.secondary ? sample[*chart.secondary] : NAN;
            for (const double reading : {first, second}) {
                if (std::isfinite(reading)) {
                    hasData = true;
                    if (!chart.percent) {
                        maximum = std::max(maximum, reading * 1.08);
                    }
                }
            }
        }
        if (!hasData || end <= start) {
            label(dc, smallFont_, L"暂无曲线", area, Muted, DT_CENTER);
            return;
        }
        chartLine(dc, plot, samples, chart.primary, accent, start, end, maximum);
        if (chart.secondary) {
            chartLine(dc, plot, samples, *chart.secondary, Teal, start, end, maximum);
        }
        if (tab_ == Tab::History && historyTarget_ >= start && historyTarget_ <= end) {
            const int x = plot.left + static_cast<int>((historyTarget_ - start) *
                (plot.right - plot.left - 1) / (end - start));
            line(dc, x, plot.top, x, plot.bottom, Muted);
        }
    }

    void cardValue(HDC dc, std::wstring_view text, RECT area) const {
        const auto separator = text.find_first_of(L" %");
        const auto number = text.substr(0, separator);
        label(dc, valueFont_, number, area);
        if (separator != std::wstring_view::npos) {
            const auto unit = text.substr(separator + (text[separator] == L' ' ? 1 : 0));
            area.left += textWidth(dc, valueFont_, number) + px(5);
            area.top += px(11);
            if (area.left < area.right) label(dc, normalFont_, unit, area, Secondary);
        }
    }

    void paintCard(HDC dc, size_t index, const Snapshot* snapshot, const std::vector<Snapshot>& samples,
        int64_t start, int64_t end) const {
        static constexpr std::array names{L"CPU", L"内存", L"磁盘", L"显卡", L"网络"};
        const auto card = cards_[index];
        const auto accent = CardColors[index];
        const int left = card.left + px(CardInset);
        const int right = card.right - px(CardInset);
        rounded(dc, card, px(18), Surface, Grid);
        rounded(dc, {left, card.top + px(18), left + px(7), card.top + px(25)}, px(7), accent, accent);
        label(dc, boldFont_, names[index], {left + px(14), card.top + px(9), right, card.top + px(34)});
        std::wstring primary;
        std::wstring detail;
        std::wstring firstLegend;
        std::wstring secondLegend;
        switch (index) {
        case 0:
            primary = decimal(value(snapshot, Metric::CpuPercent), 1, L"%");
            detail = decimal(value(snapshot, Metric::CpuTemp), 0, L" °C") + L"  ·  "
                + decimal(value(snapshot, Metric::CpuWatts), 1, L" W");
            if (std::isfinite(value(snapshot, Metric::CpuWatts))) detail += L" RAPL";
            break;
        case 1:
            primary = decimal(value(snapshot, Metric::MemoryPercent), 1, L"%");
            detail = bytes(value(snapshot, Metric::MemoryUsed)) + L" / " + bytes(value(snapshot, Metric::MemoryTotal));
            firstLegend = L"占用率曲线";
            break;
        case 2:
            primary = bytes(value(snapshot, Metric::DiskUsed));
            detail = L"总容量 " + bytes(value(snapshot, Metric::DiskTotal));
            firstLegend = L"读 " + bytes(value(snapshot, Metric::DiskRead), true);
            secondLegend = L"写 " + bytes(value(snapshot, Metric::DiskWrite), true);
            break;
        case 3:
            primary = decimal(value(snapshot, Metric::GpuPercent), 1, L"%");
            detail = decimal(value(snapshot, Metric::GpuTemp), 0, L" °C") + L"  ·  "
                + decimal(value(snapshot, Metric::GpuWatts), 1, L" W");
            label(dc, smallFont_, L"显存 " + bytes(value(snapshot, Metric::GpuMemoryUsed)) + L" / "
                + bytes(value(snapshot, Metric::GpuMemoryTotal)),
                {left, card.top + px(85), right, card.top + px(104)}, Secondary);
            break;
        case 4:
            primary = bytes(value(snapshot, Metric::NetworkDown), true);
            detail = L"上传 " + bytes(value(snapshot, Metric::NetworkUp), true);
            firstLegend = L"下载";
            secondLegend = L"上传";
            break;
        }
        cardValue(dc, primary, {left, card.top + px(33), right, card.top + px(67)});
        label(dc, smallFont_, detail, {left, card.top + px(66), right, card.top + px(86)}, Secondary);
        const int legendMid = (left + right) / 2;
        if (!secondLegend.empty()) {
            label(dc, smallFont_, firstLegend, {left, card.top + px(106), legendMid, card.top + px(133)}, accent);
            label(dc, smallFont_, secondLegend, {legendMid, card.top + px(106), right, card.top + px(133)}, Teal, DT_RIGHT);
        } else if (!firstLegend.empty()) {
            label(dc, smallFont_, firstLegend, {left, card.top + px(106), right, card.top + px(133)}, accent);
        }
        paintChart(dc, chartRect(card), chartFor(index), samples, start, end, accent);
    }

    void paintRanks(HDC dc, RECT panel, const wchar_t* title,
        const std::array<ProcessEntry, TopCount>* ranks, RankSort sort, COLORREF accent) const {
        rounded(dc, panel, px(18), Surface, Grid);
        const int left = panel.left + px(RankInset);
        const int right = panel.right - px(RankInset);
        const int pidWidth = px(70);
        const int cpuWidth = px(RankCpuWidth);
        const int memWidth = px(RankMemoryWidth);
        rounded(dc, {left, panel.top + px(15), left + px(4), panel.top + px(27)}, px(4), accent, accent);
        label(dc, boldFont_, title, {left + px(12), panel.top + px(7), right, panel.top + px(34)});
        const int headerY = panel.top + px(RankHeaderOffset);
        auto drawRow = [&](int y, int height, std::wstring_view name, std::wstring_view pid,
            std::wstring_view cpu, std::wstring_view memory, COLORREF color) {
            label(dc, smallFont_, name, {left, y, right - pidWidth - cpuWidth - memWidth, y + height}, color);
            label(dc, smallFont_, pid, {right - pidWidth - cpuWidth - memWidth, y, right - cpuWidth - memWidth - px(7), y + height}, color, DT_RIGHT);
            label(dc, smallFont_, cpu, {right - cpuWidth - memWidth, y, right - memWidth - px(7), y + height}, color, DT_RIGHT);
            label(dc, smallFont_, memory, {right - memWidth, y, right - px(7), y + height}, color, DT_RIGHT);
        };
        drawRow(headerY, px(RankHeaderHeight), L"进程", L"PID", L"", L"", Muted);
        const int rowsY = headerY + px(RankHeaderHeight + 4);
        line(dc, left, rowsY, right, rowsY, Grid);
        std::array<const ProcessEntry*, TopCount> rows{};
        size_t count = 0;
        if (ranks) {
            for (const auto& entry : *ranks) {
                if (entry.pid) {
                    rows[count++] = &entry;
                }
            }
        }
        if (!count) {
            label(dc, normalFont_, L"暂无进程记录", {left, rowsY + px(14), right, rowsY + px(42)}, Muted);
            return;
        }
        const auto hasCpu = [](const ProcessEntry* entry) { return std::isfinite(entry->cpu) && entry->cpu >= 0; };
        std::sort(rows.begin(), rows.begin() + count, [&](const ProcessEntry* left, const ProcessEntry* right) {
            if (sort.field == RankField::Cpu) {
                const bool leftAvailable = hasCpu(left);
                const bool rightAvailable = hasCpu(right);
                if (leftAvailable != rightAvailable) {
                    return leftAvailable;
                }
                if (leftAvailable && left->cpu != right->cpu) {
                    return sort.ascending ? left->cpu < right->cpu : left->cpu > right->cpu;
                }
            } else if (left->memory != right->memory) {
                return sort.ascending ? left->memory < right->memory : left->memory > right->memory;
            }
            return left->pid < right->pid;
        });
        const int availableHeight = static_cast<int>(panel.bottom) - rowsY - px(10);
        const int rowHeight = std::clamp(availableHeight / static_cast<int>(TopCount), px(16), px(27));
        for (size_t i = 0; i < count; ++i) {
            const auto& process = *rows[i];
            const int y = rowsY + static_cast<int>(i) * rowHeight;
            if (i % 2 == 0) {
                fill(dc, {panel.left + px(7), y + 1, panel.right - px(7), y + rowHeight}, ChartBg);
            }
            const auto end = std::find(std::begin(process.name), std::end(process.name), L'\0');
            const std::wstring_view name(process.name, static_cast<size_t>(end - std::begin(process.name)));
            drawRow(y, rowHeight, name, std::to_wstring(process.pid),
                decimal(hasCpu(&process) ? process.cpu : Missing, 1, L"%"), bytes(static_cast<double>(process.memory)), Ink);
        }
    }

    void paintSettings(HDC dc, RECT area) const {
        const int pad = px(24);
        rounded(dc, {pad, px(116), area.right - pad, px(265)}, px(18), Surface, Grid);
        label(dc, boldFont_, L"历史保留", {pad + px(14), px(125), area.right - pad, px(153)});
        label(dc, smallFont_, L"默认 7 天。超过保留时长的记录会自动清理。",
            {pad + px(14), px(151), area.right - pad, px(175)}, Secondary);
        label(dc, normalFont_, L"保留时长", {pad + px(14), px(180), pad + px(107), px(209)}, Secondary);
        label(dc, normalFont_, L"天", {pad + px(191), px(180), pad + px(218), px(209)}, Secondary);
        label(dc, smallFont_, settingsMessage_, {pad + px(14), px(221), area.right - pad, px(248)}, Blue);
    }

    void paintDetailSummary(HDC dc, RECT area,
        std::initializer_list<std::pair<const wchar_t*, std::wstring>> values) const {
        const int gap = px(10);
        const int count = static_cast<int>(values.size());
        const int width = (area.right - area.left - gap * (count - 1)) / count;
        int x = area.left;
        for (const auto& [title, reading] : values) {
            const int right = x + width;
            rounded(dc, {x, area.top, right, area.bottom}, px(14), Surface, Grid);
            label(dc, normalFont_, title, {x + px(12), area.top + px(9), right - px(12), area.top + px(32)}, Secondary);
            cardValue(dc, reading, {x + px(12), area.top + px(34), right - px(12), area.bottom - px(8)});
            x = right + gap;
        }
    }

    void paintCpuDetails(HDC dc, RECT area, const Snapshot* snapshot) const {
        const int left = px(24);
        const int right = area.right - left;
        label(dc, titleFont_, L"CPU 实时详情", {left, px(111), right, px(148)});
        paintDetailSummary(dc, {left, px(158), right, px(233)}, {
            {L"整机 CPU 占用率", decimal(value(snapshot, Metric::CpuPercent), 1, L"%")},
            {L"CPU 封装温度", decimal(value(snapshot, Metric::CpuTemp), 0, L" °C")},
            {L"CPU 封装功耗 · RAPL", decimal(value(snapshot, Metric::CpuWatts), 1, L" W")}
        });
        rounded(dc, {left, px(245), right, area.bottom - px(10)}, px(18), Surface, Grid);
        label(dc, boldFont_, L"逻辑处理器", {left + px(12), px(249), right - px(12), px(277)});
        label(dc, smallFont_, L"功耗取自 CPU 能量计数，读数受平台遥测影响",
            {left + px(150), px(249), right - px(14), px(277)}, Muted, DT_RIGHT);
        if (details_.cores.empty()) {
            label(dc, normalFont_, L"暂无逻辑处理器明细。", {left + px(12), px(284), right - px(12), px(314)}, Muted);
        }
    }

    void paintGpuDetails(HDC dc, RECT area, const Snapshot* snapshot) const {
        const int left = px(24);
        const int right = area.right - left;
        label(dc, titleFont_, L"显卡实时详情", {left, px(111), right, px(148)});
        paintDetailSummary(dc, {left, px(158), right, px(233)}, {
            {L"GPU 占用率", decimal(value(snapshot, Metric::GpuPercent), 1, L"%")},
            {L"GPU 温度", decimal(value(snapshot, Metric::GpuTemp), 0, L" °C")},
            {L"板卡功耗", decimal(value(snapshot, Metric::GpuWatts), 1, L" W")},
            {L"显存已用 / 总量", bytes(value(snapshot, Metric::GpuMemoryUsed)) + L" / " + bytes(value(snapshot, Metric::GpuMemoryTotal))}
        });
        rounded(dc, {left, px(245), right, px(450)}, px(18), Surface, Grid);
        label(dc, boldFont_, L"时钟与散热", {left + px(12), px(249), right - px(12), px(277)});
        const std::pair<const wchar_t*, std::wstring> fields[] = {
            {L"核心频率", decimal(details_.gpuCoreMhz, 0, L" MHz")},
            {L"显存频率", decimal(details_.gpuMemoryMhz, 0, L" MHz")},
            {L"风扇设定转速", decimal(details_.gpuFanRpm, 0, L" RPM")},
            {L"风扇设定比例", decimal(details_.gpuFanPercent, 0, L"%")},
            {L"热点温度", decimal(details_.gpuHotspotC, 0, L" °C")},
            {L"温度限制", details_.gpuLimitsAvailable ? (details_.gpuThermalLimited ? L"已触发" : L"未触发") : L"—"},
            {L"功耗限制", details_.gpuLimitsAvailable ? (details_.gpuPowerLimited ? L"已触发" : L"未触发") : L"—"}
        };
        const int width = (right - left - px(24)) / 2;
        for (int i = 0; i < static_cast<int>(std::size(fields)); ++i) {
            const int x = left + px(12) + (i % 2) * width;
            const int y = px(284 + 37 * (i / 2));
            const auto& [title, reading] = fields[i];
            rounded(dc, {x, y, x + width - px(10), y + px(30)}, px(8), ChartBg, ChartBg);
            label(dc, normalFont_, title, {x + px(8), y, x + width / 2, y + px(30)}, Secondary);
            label(dc, normalFont_, reading, {x + width / 2, y, x + width - px(18), y + px(30)}, Ink, DT_RIGHT);
        }
    }

    void paintDiskDetails(HDC dc, RECT area) const {
        const int left = px(24);
        const int right = area.right - left;
        label(dc, titleFont_, L"磁盘 SMART", {left, px(111), right, px(148)});
        label(dc, smallFont_, L"每行对应一块物理磁盘。损耗为设备报告的寿命消耗比例，横线表示暂无读数。",
            {left, px(148), right, px(177)}, Secondary);
        rounded(dc, {left, px(177), right, area.bottom - px(10)}, px(18), Surface, Grid);
        if (details_.disks.empty()) {
            const auto text = details_.pane == DetailPane::Disk ? L"暂无可用磁盘数据。" : L"正在读取磁盘可靠性数据…";
            label(dc, normalFont_, text, {left + px(12), px(188), right - px(12), px(218)}, Muted);
            if (callbacks_.startCpu) {
                label(dc, smallFont_, L"如果一直没有读数，可从托盘菜单启动硬件传感器。",
                    {left + px(12), px(219), right - px(12), px(244)}, Secondary);
            }
            return;
        }
        const int selected = static_cast<int>(SendMessageW(diskList_, LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
        const size_t index = selected >= 0 && static_cast<size_t>(selected) < details_.disks.size() ? selected : 0;
        const auto& disk = details_.disks[index];
        const int noteTop = area.bottom - px(80);
        label(dc, boldFont_, L"磁盘 " + std::to_wstring(disk.number) + L" 读数说明",
            {left + px(12), noteTop, right - px(12), noteTop + px(24)});
        const std::wstring_view note = disk.note.empty() ? L"选择表格中的磁盘，可查看相应的读数说明。" : std::wstring_view(disk.note);
        paragraph(dc, smallFont_, note, {left + px(12), noteTop + px(26), right - px(12), area.bottom - px(13)}, Secondary);
    }

    void render(HDC dc, RECT area) const {
        fill(dc, area, Background);
        const int pad = px(PageInset);
        rounded(dc, {pad, px(21), pad + px(35), px(56)}, px(12), Blue, Blue);
        const POINT pulse[] = {{pad + px(7), px(39)}, {pad + px(12), px(39)},
            {pad + px(16), px(32)}, {pad + px(21), px(45)}, {pad + px(25), px(38)}, {pad + px(28), px(38)}};
        if (const auto pen = CreatePen(PS_SOLID, px(2), Surface)) {
            const auto old = SelectObject(dc, pen);
            Polyline(dc, pulse, static_cast<int>(std::size(pulse)));
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        const int brand = pad + px(46);
        label(dc, titleFont_, L"Computer Watch", {brand, px(13), area.right - pad, px(47)});
        label(dc, smallFont_, L"本机性能与硬件传感器", {brand, px(44), area.right - pad, px(64)}, Secondary);
        if (tab_ == Tab::Settings) {
            paintSettings(dc, area);
            return;
        }

        const bool history = tab_ == Tab::History;
        const auto& samples = history ? history_ : live_;
        const auto now = nowMs();
        const auto start = history ? historyStart_ : now - MinuteMs;
        const auto end = history ? historyEnd_ : now;
        const auto snapshot = history ? (selected_ ? &*selected_ : nullptr) : (live_.empty() ? nullptr : &live_.back());
        std::wstring status;
        COLORREF statusColor = history ? Blue : Green;
        COLORREF statusFill = history ? BlueTint : GreenTint;
        if (history) {
            rounded(dc, {pad, px(106), area.right - pad, px(143)}, px(12), Surface, Grid);
            label(dc, smallFont_, L"查询时间", {pad, px(110), pad + px(64), px(137)}, Secondary);
            label(dc, smallFont_, L"点击任意曲线定位采样时刻",
                {pad + px(465), px(110), area.right - pad, px(137)}, Secondary);
            status = historyFailed_ ? L"读取历史失败" :
                snapshot ? L"历史样本  " + timeText(snapshot->timestampMs, true) : L"该时间附近暂无记录";
        } else if (liveFailed_) {
            status = L"读取采集数据失败";
            statusColor = Amber;
            statusFill = AmberTint;
        } else if (!snapshot) {
            status = L"等待采集器";
            statusColor = Amber;
            statusFill = AmberTint;
        } else {
            status = L"最新样本  " + timeText(snapshot->timestampMs);
            if (now - snapshot->timestampMs > 12'000) {
                status += L"  ·  采集已暂停";
                statusColor = Amber;
                statusFill = AmberTint;
            }
        }
        const int badgeRight = area.right - pad;
        const int badgeLeft = badgeRight - textWidth(dc, smallFont_, status) - px(35);
        rounded(dc, {badgeLeft, px(26), badgeRight, px(51)}, px(22), statusFill, statusFill);
        rounded(dc, {badgeLeft + px(10), px(35), badgeLeft + px(16), px(41)}, px(6), statusColor, statusColor);
        label(dc, smallFont_, status, {badgeLeft + px(22), px(26), badgeRight - px(9), px(51)}, statusColor);

        switch (detailPane()) {
        case DetailPane::Cpu: paintCpuDetails(dc, area, snapshot); return;
        case DetailPane::Gpu: paintGpuDetails(dc, area, snapshot); return;
        case DetailPane::Disk: paintDiskDetails(dc, area); return;
        case DetailPane::None: break;
        }

        for (size_t index = 0; index < cards_.size(); ++index) {
            paintCard(dc, index, snapshot, samples, start, end);
        }
        const int y = cards_[0].bottom + px(27);
        label(dc, smallFont_, history ? timeText(start, true) : L"最近 60 秒",
            {pad, cards_[0].bottom + 1, area.right / 2, y - 1}, Muted);
        label(dc, smallFont_, history ? timeText(end, true) : L"当前",
            {area.right / 2, cards_[0].bottom + 1, area.right - pad, y - 1}, Muted, DT_RIGHT);
        paintRanks(dc, rankPanel(area, 0), L"CPU 占用前 10", snapshot ? &snapshot->byCpu : nullptr, rankSorts_[0], Blue);
        paintRanks(dc, rankPanel(area, 1), L"内存占用前 10", snapshot ? &snapshot->byMemory : nullptr, rankSorts_[1], Teal);
    }

    void paint() const {
        PAINTSTRUCT ps{};
        const auto dc = BeginPaint(window_, &ps);
        RECT area{};
        GetClientRect(window_, &area);
        if (area.right > 0 && area.bottom > 0) {
            const auto buffer = CreateCompatibleDC(dc);
            const auto bitmap = CreateCompatibleBitmap(dc, area.right, area.bottom);
            if (buffer && bitmap) {
                const auto old = SelectObject(buffer, bitmap);
                render(buffer, area);
                BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old);
            } else {
                render(dc, area);
            }
            if (bitmap) {
                DeleteObject(bitmap);
            }
            if (buffer) {
                DeleteDC(buffer);
            }
        }
        EndPaint(window_, &ps);
    }

    HINSTANCE instance_;
    UiCallbacks callbacks_;
    HWND window_{}, liveButton_{}, historyButton_{}, settingsButton_{}, cpuCombo_{}, gpuCombo_{};
    HWND picker_{}, rangeCombo_{}, queryButton_{}, retentionEdit_{}, saveButton_{};
    HWND cpuDetailButton_{}, gpuDetailButton_{}, diskDetailButton_{}, detailBackButton_{};
    HWND coreList_{}, diskList_{};
    std::array<HWND, 4> sortButtons_{};
    HFONT normalFont_{}, smallFont_{}, titleFont_{}, valueFont_{}, boldFont_{};
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_{};
    bool trayAdded_{}, trayV4_{};
    int dpi_ = 96;
    Tab tab_ = Tab::Live;
    DetailPane requestedDetail_ = DetailPane::None;
    HardwareDetails details_;
    std::array<RankSort, 2> rankSorts_{{{RankField::Cpu}, {RankField::Memory}}};
    std::optional<bool> visible_;
    std::array<RECT, 5> cards_{};
    std::vector<Snapshot> live_, history_;
    std::optional<Snapshot> selected_;
    int64_t historyStart_{}, historyEnd_{}, historyTarget_{};
    bool liveFailed_{}, historyFailed_{};
    std::wstring settingsMessage_;
};

} // namespace

int runUi(HINSTANCE instance, bool startVisible, UiCallbacks callbacks) {
    const INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_DATE_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);
    Dashboard dashboard(instance, std::move(callbacks));
    if (!dashboard.create(startVisible)) {
        return 1;
    }
    MSG message{};
    int status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        if (!IsDialogMessageW(dashboard.window(), &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    return status == -1 ? 1 : static_cast<int>(message.wParam);
}

} // namespace cw
