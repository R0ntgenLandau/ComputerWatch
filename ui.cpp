#include "ui.h"
#include "display_info.h"
#include "ui_controls.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace cw {
namespace {

constexpr wchar_t WindowClass[] = L"ComputerWatchDashboard";
constexpr WORD ApplicationIcon = 1;
constexpr WORD ReadingFontResource = 102;
constexpr wchar_t ReadingFontFamily[] = L"Anonymous Pro";
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

constexpr int PageInset = 24;
constexpr int McpCardBottom = 502;
constexpr int CardInset = 16;
constexpr int ModuleGap = 14;
constexpr int CardHeight = 236;
constexpr int CardTools = 128;
constexpr int MemoryModelRowHeight = 19;
constexpr int HardwareModelRowHeight = 32;
constexpr int RankHeight = 314;
constexpr int RankInset = 14;
constexpr int RankCpuWidth = 72;
constexpr int RankMemoryWidth = 92;
constexpr int RankHeaderOffset = 38;
constexpr int RankHeaderHeight = 26;
constexpr int DisplayListTop = 132;
constexpr int DisplayListHeight = 112;
constexpr int DisplayGamutTop = 260;
constexpr int DisplayPageHeight = 390;
constexpr int DisplayPlotPageHeight = 665;
constexpr int MemorySectionsTop = 326;
constexpr int MemorySectionsBody = 364;
constexpr int MemoryPlotHeight = 182;
constexpr int MemoryTimingHeight = 108;
constexpr int MemoryDeviceHeight = 220;
constexpr int EmptyMemorySlotHeight = 58;
constexpr int NetworkCardsTop = 252;
constexpr int NetworkRowHeight = 22;

constexpr std::pair<const wchar_t*, int> CoreColumns[] = {
    {L"处理器组", 130}, {L"逻辑处理器", 130}, {L"占用率", 160}, {L"频率（估算）", 160}
};
constexpr std::pair<const wchar_t*, int> DiskColumns[] = {
    {L"磁盘", 54}, {L"型号", 195}, {L"总线", 64}, {L"健康状态", 80},
    {L"温度", 70}, {L"损耗", 70}, {L"通电时长", 100}, {L"通电次数", 84},
    {L"不可纠正 / 介质错误", 154}
};

constexpr std::pair<const wchar_t*, int> DisplayColumns[] = {
    {L"序号", 36}, {L"名称", 124}, {L"信号分辨率", 94}, {L"刷新率", 76}, {L"连接类型", 106}
};
constexpr std::pair<const wchar_t*, int> MemoryTimingColumns[] = {
    {L"字段", 150}, {L"读数", 70}, {L"单位", 65}, {L"状态", 110}, {L"作用范围", 210},
    {L"采集时间", 155}, {L"来源", 270}, {L"原因 / 验证情况", 370}
};
constexpr std::pair<const wchar_t*, int> DiskSmartColumns[] = {
    {L"项目", 190}, {L"读数", 310}, {L"原始值 / 说明", 270}
};
constexpr int DiskSummaryTop = 157;
constexpr int DiskSummaryHeight = 112;
constexpr int DiskSmartTop = 347;
constexpr int DiskPageHeight = 535;
constexpr int GpuSpecificationsHeight = 186;
constexpr int GpuPageHeight = 583 + GpuSpecificationsHeight;

const wchar_t* readingStatus(ReadingStatus status) {
    switch (status) {
    case ReadingStatus::Ok: return L"ok";
    case ReadingStatus::NotApplicable: return L"not_applicable";
    case ReadingStatus::BackendMissing: return L"backend_missing";
    case ReadingStatus::ReadFailed: return L"read_failed";
    case ReadingStatus::Unverified: return L"unverified";
    }
    return L"read_failed";
}

enum class Tab { Live, History, Displays, Board, Settings, CpuDetails, GpuDetails, DiskDetails, MemoryDetails, NetworkDetails };
enum class RankField { Cpu, Memory };

struct RankSort {
    RankField field;
    bool ascending = false;
};

enum ControlId {
    LiveButton = 100, HistoryButton, SettingsButton, CpuCombo, GpuCombo,
    HistoryPicker, RangeCombo, QueryButton, RetentionEdit, SaveButton,
    CpuDetailButton, GpuDetailButton, DiskDetailButton, DetailBackButton, CoreList, DiskList,
    LeftSortCpu, LeftSortMemory, RightSortCpu, RightSortMemory, LightThemeButton, DarkThemeButton, CopyMcpButton,
    DisplaysButton, DisplayList, RefreshDisplaysButton, MemoryDetailButton,
    MemoryPlotButton, MemoryTimingButton, ImportMemoryTimingButton, MemoryTimingList, DiskSmartList, BoardButton,
    NetworkDetailButton
};

const wchar_t* buttonCaption(unsigned id) {
    switch (id) {
    case LiveButton: return L"实时监控";
    case HistoryButton: return L"历史记录";
    case DisplaysButton: return L"显示器";
    case BoardButton: return L"主板";
    case RefreshDisplaysButton: return L"刷新";
    case SettingsButton: return L"设置";
    case CpuDetailButton:
    case MemoryDetailButton:
    case NetworkDetailButton:
    case GpuDetailButton: return L"详情";
    case DiskDetailButton: return L"SMART";
    case DetailBackButton: return L"返回实时监控";
    case MemoryPlotButton: return L"展开占用率曲线";
    case MemoryTimingButton: return L"收起硬件时序";
    case ImportMemoryTimingButton: return L"导入报告";
    case QueryButton: return L"查看";
    case SaveButton: return L"保存";
    case LightThemeButton: return L"浅色";
    case DarkThemeButton: return L"深色";
    case CopyMcpButton: return L"复制客户端配置";
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

std::optional<std::array<unsigned, 4>> readMemoryTimingReport(const std::filesystem::path& path) {
    std::ifstream report(path, std::ios::binary | std::ios::ate);
    const auto length = report.tellg();
    if (length <= 0 || length > 16 * 1024 * 1024) return std::nullopt;
    std::string html(static_cast<size_t>(length), '\0');
    report.seekg(0);
    if (!report.read(html.data(), length)) return std::nullopt;
    const auto heading = html.find("id=\"Memory\"");
    if (heading == std::string::npos) return std::nullopt;
    const auto sectionStart = html.find("<HR>", heading);
    const auto sectionEnd = html.find("</TABLE>", sectionStart);
    const auto marker = html.find("(tCAS-tRCD-tRP-tRAS):", sectionStart);
    if (sectionEnd == std::string::npos || marker >= sectionEnd) return std::nullopt;
    const auto cell = html.find("<TD>", marker);
    if (cell == std::string::npos || cell >= html.find_first_of("\r\n", marker)) return std::nullopt;
    std::array<unsigned, 4> timing{};
    int consumed = 0;
    const char* reading = html.c_str() + cell + 4;
    if (std::sscanf(reading, "%3u-%3u-%3u-%3u%n", &timing[0], &timing[1], &timing[2], &timing[3],
        &consumed) != 4 || std::find(timing.begin(), timing.end(), 0) != timing.end()) return std::nullopt;
    if (std::string_view("\r\n< \t").find(reading[consumed]) == std::string_view::npos) return std::nullopt;
    return timing;
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

void drawLabel(HDC dc, HFONT font, std::wstring_view text, RECT area, COLORREF color, UINT align) {
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
        for (auto font : {normalFont_, smallFont_, brandFont_, titleFont_, valueFont_, compactValueFont_, boldFont_}) {
            if (font) {
                DeleteObject(font);
            }
        }
        if (readingFont_) RemoveFontMemResourceEx(readingFont_);
        if (surfaceBrush_) DeleteObject(surfaceBrush_);
    }

    bool create(bool startVisible) {
        const auto resource = FindResourceW(instance_, MAKEINTRESOURCEW(ReadingFontResource), RT_RCDATA);
        if (resource) {
            if (auto* bytes = LockResource(LoadResource(instance_, resource))) {
                DWORD count = 0;
                readingFont_ = AddFontMemResourceEx(bytes, SizeofResource(instance_, resource), nullptr, &count);
            }
        }
        darkMode_ = callbacks_.darkMode && callbacks_.darkMode();
        theme_ = &ui::palette(darkMode_);
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = windowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(ApplicationIcon));
        windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(ApplicationIcon),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        appIcon_ = windowClass.hIcon;
        trayIcon_ = windowClass.hIconSm ? windowClass.hIconSm : appIcon_;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = WindowClass;
        if (!RegisterClassExW(&windowClass)) {
            return false;
        }
        dpi_ = static_cast<int>(GetDpiForSystem());
        window_ = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"Computer Watch · 电脑监控",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT,
            px(1150), px(855), nullptr, nullptr, instance_, this);
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
    void label(HDC dc, HFONT font, std::wstring_view text, RECT area,
        COLORREF color = CLR_INVALID, UINT align = DT_LEFT) const {
        drawLabel(dc, font, text, area, color == CLR_INVALID ? theme_->ink : color, align);
    }

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

    static LRESULT CALLBACK styledControl(HWND child, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR context) {
        auto& dashboard = *reinterpret_cast<Dashboard*>(context);
        switch (message) {
        case WM_MOUSEMOVE: {
            if (!GetPropW(child, L"ComputerWatch.Hot")) {
                SetPropW(child, L"ComputerWatch.Hot", reinterpret_cast<HANDLE>(1));
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, child, 0};
                TrackMouseEvent(&track);
                InvalidateRect(child, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            RemovePropW(child, L"ComputerWatch.Hot");
            InvalidateRect(child, nullptr, FALSE);
            break;
        case WM_SETFOCUS: {
            if (GetDlgCtrlID(child) == HistoryPicker) InvalidateRect(child, nullptr, FALSE);
            if (IsWindowVisible(child) && dashboard.scrollMax_ > 0) {
                RECT bounds{}, client{};
                GetWindowRect(child, &bounds);
                MapWindowPoints(nullptr, dashboard.window_, reinterpret_cast<POINT*>(&bounds), 2);
                GetClientRect(dashboard.window_, &client);
                if (bounds.top < 0) dashboard.scrollTo(dashboard.scroll_ + bounds.top - dashboard.px(12));
                else if (bounds.bottom > client.bottom) {
                    dashboard.scrollTo(dashboard.scroll_ + bounds.bottom - client.bottom + dashboard.px(12));
                }
            }
            break;
        }
        case WM_NCDESTROY:
            RemovePropW(child, L"ComputerWatch.Hot");
            break;
        case WM_NOTIFY: {
            const auto notification = reinterpret_cast<const NMHDR*>(lParam);
            const int id = GetDlgCtrlID(child);
            if ((id == CoreList || id == DiskList || id == DiskSmartList || id == DisplayList ||
                 id == MemoryTimingList) && notification->code == NM_CUSTOMDRAW &&
                notification->hwndFrom == ListView_GetHeader(child)) {
                return dashboard.drawDetailHeader(*reinterpret_cast<NMCUSTOMDRAW*>(lParam));
            }
            break;
        }
        case WM_KILLFOCUS:
            if (GetDlgCtrlID(child) == HistoryPicker) InvalidateRect(child, nullptr, FALSE);
            break;
        case WM_PAINT:
            if (GetDlgCtrlID(child) == HistoryPicker && dashboard.darkMode_ &&
                GetFocus() != child && !ui::detail::highContrast()) {
                PAINTSTRUCT paint{};
                const auto dc = BeginPaint(child, &paint);
                dashboard.paintPicker(child, dc);
                EndPaint(child, &paint);
                return 0;
            }
            if (GetDlgCtrlID(child) == CpuCombo || GetDlgCtrlID(child) == GpuCombo || GetDlgCtrlID(child) == RangeCombo) {
                PAINTSTRUCT paint{};
                const auto dc = BeginPaint(child, &paint);
                RECT bounds{};
                GetClientRect(child, &bounds);
                dashboard.paintCombo(child, dc, bounds);
                EndPaint(child, &paint);
                return 0;
            }
            break;
        }
        return DefSubclassProc(child, message, wParam, lParam);
    }

    LRESULT drawDetailHeader(const NMCUSTOMDRAW& draw) const {
        if (ui::detail::highContrast()) return CDRF_DODEFAULT;
        if (draw.dwDrawStage == CDDS_PREPAINT) {
            fill(draw.hdc, draw.rc, theme_->background);
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
        const bool hot = (draw.uItemState & CDIS_HOT) != 0;
        fill(draw.hdc, draw.rc, hot ? theme_->selectionFill : theme_->background);
        line(draw.hdc, draw.rc.left, draw.rc.bottom - 1, draw.rc.right, draw.rc.bottom - 1, theme_->border);
        wchar_t caption[96]{};
        HDITEMW item{};
        item.mask = HDI_TEXT;
        item.pszText = caption;
        item.cchTextMax = static_cast<int>(std::size(caption));
        SendMessageW(draw.hdr.hwndFrom, HDM_GETITEMW, draw.dwItemSpec, reinterpret_cast<LPARAM>(&item));
        auto textBounds = draw.rc;
        InflateRect(&textBounds, -px(5), 0);
        label(draw.hdc, smallFont_, caption, textBounds, theme_->secondary);
        return CDRF_SKIPDEFAULT;
    }

    void paintPicker(HWND child, HDC dc) const {
        RECT bounds{};
        GetClientRect(child, &bounds);
        fill(dc, bounds, theme_->surface);
        rounded(dc, bounds, px(8), theme_->surface, theme_->border);
        DATETIMEPICKERINFO info{};
        info.cbSize = sizeof(info);
        SendMessageW(child, DTM_GETDATETIMEPICKERINFO, 0, reinterpret_cast<LPARAM>(&info));
        SYSTEMTIME date{};
        const bool valid = SendMessageW(child, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&date)) == GDT_VALID;
        const auto timestamp = valid ? fromLocalTime(date) : std::nullopt;
        bounds.left += px(8);
        bounds.right = info.rcButton.left > bounds.left ? info.rcButton.left : bounds.right - px(23);
        label(dc, smallFont_, timestamp ? timeText(*timestamp, true) : L"—", bounds, theme_->ink);
        const int x = (bounds.right + info.rcButton.right) / 2;
        const int y = bounds.bottom / 2;
        line(dc, x - px(4), y - px(2), x, y + px(2), theme_->secondary);
        line(dc, x, y + px(2), x + px(5), y - px(3), theme_->secondary);
    }

    void paintCombo(HWND child, HDC dc, RECT bounds) const {
        const auto& theme = *theme_;
        const bool contrast = ui::detail::highContrast();
        const bool open = SendMessageW(child, CB_GETDROPPEDSTATE, 0, 0);
        const auto background = contrast ? GetSysColor(COLOR_WINDOW) : theme.surface;
        const auto ink = contrast ? GetSysColor(COLOR_WINDOWTEXT) :
            IsWindowEnabled(child) ? theme.ink : theme.muted;
        const auto face = contrast ? background : open ? theme.pressedFill :
            GetPropW(child, L"ComputerWatch.Hot") ? theme.selectionFill : theme.surface;
        fill(dc, bounds, background);
        rounded(dc, bounds, px(8), face, contrast ? ink : theme.border);
        const int arrow = bounds.right - px(13);
        const int middle = bounds.bottom / 2;
        line(dc, arrow - px(4), middle - px(2), arrow, middle + px(2), ink);
        line(dc, arrow, middle + px(2), arrow + px(5), middle - px(3), ink);
        bounds.left += px(8);
        bounds.right -= px(25);
        label(dc, smallFont_, comboText(child, static_cast<int>(SendMessageW(child, CB_GETCURSEL, 0, 0))), bounds, ink);
        if (GetFocus() == child) {
            bounds.left -= px(8);
            bounds.right += px(25);
            ui::detail::focus(dc, bounds, dpi_, contrast ? ink : theme.blue, contrast);
        }
    }

    std::wstring comboText(HWND child, int index) const {
        if (index < 0) return {};
        const auto length = SendMessageW(child, CB_GETLBTEXTLEN, index, 0);
        if (length == CB_ERR) return {};
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        SendMessageW(child, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>(length));
        return text;
    }

    void sizeCombo(HWND child) const {
        SendMessageW(child, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), px(24));
        SendMessageW(child, CB_SETITEMHEIGHT, 0, px(26));
    }

    void styleControl(HWND child) {
        SetWindowSubclass(child, styledControl, 1, reinterpret_cast<DWORD_PTR>(this));
    }

    HWND combo(int id, std::initializer_list<const wchar_t*> options) {
        const auto child = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, id);
        for (auto option : options) {
            SendMessageW(child, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option));
        }
        SendMessageW(child, CB_SETCURSEL, 0, 0);
        sizeCombo(child);
        styleControl(child);
        return child;
    }

    HWND button(ControlId id) {
        const auto child = control(WC_BUTTONW, buttonCaption(id), BS_OWNERDRAW, id);
        styleControl(child);
        return child;
    }

    template<size_t N>
    HWND detailList(int id, const std::pair<const wchar_t*, int> (&columns)[N]) {
        const auto child = control(WC_LISTVIEWW, L"",
            LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS, id);
        styleControl(child);
        SendMessageW(child, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
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
        const auto oldFonts = std::array{normalFont_, smallFont_, brandFont_, titleFont_, valueFont_,
            compactValueFont_, boldFont_};
        normalFont_ = font(13);
        smallFont_ = font(12);
        brandFont_ = readingFont_ ? font(18, FW_BOLD, ReadingFontFamily) : font(18, FW_SEMIBOLD, L"Segoe UI");
        titleFont_ = readingFont_ ? font(20, FW_BOLD, ReadingFontFamily) : font(20, FW_SEMIBOLD, L"Segoe UI");
        valueFont_ = readingFont_ ? font(34, FW_BOLD, ReadingFontFamily) : font(34, FW_SEMIBOLD, L"Consolas");
        compactValueFont_ = readingFont_ ? font(15, FW_BOLD, ReadingFontFamily) : nullptr;
        boldFont_ = font(15, FW_SEMIBOLD);
        for (const auto child : {liveButton_, historyButton_, displaysButton_, boardButton_, settingsButton_, cpuCombo_, gpuCombo_, rangeCombo_,
            cpuDetailButton_, memoryDetailButton_, gpuDetailButton_, diskDetailButton_, networkDetailButton_,
            detailBackButton_, picker_, queryButton_,
            memoryPlotButton_, memoryTimingButton_, importMemoryTimingButton_,
            retentionEdit_, saveButton_, lightThemeButton_, darkThemeButton_, copyMcpButton_, refreshDisplaysButton_,
            coreList_, diskList_, diskSmartList_, displayList_, memoryTimingList_}) {
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
        displaysButton_ = button(DisplaysButton);
        boardButton_ = button(BoardButton);
        refreshDisplaysButton_ = button(RefreshDisplaysButton);
        settingsButton_ = button(SettingsButton);
        cpuCombo_ = combo(CpuCombo, {L"占用率 %", L"封装温度 °C", L"封装功耗 W"});
        gpuCombo_ = combo(GpuCombo, {L"占用率 %", L"温度 °C", L"板卡功耗 W"});
        cpuDetailButton_ = button(CpuDetailButton);
        memoryDetailButton_ = button(MemoryDetailButton);
        memoryPlotButton_ = button(MemoryPlotButton);
        memoryTimingButton_ = button(MemoryTimingButton);
        importMemoryTimingButton_ = button(ImportMemoryTimingButton);
        gpuDetailButton_ = button(GpuDetailButton);
        diskDetailButton_ = button(DiskDetailButton);
        networkDetailButton_ = button(NetworkDetailButton);
        detailBackButton_ = button(DetailBackButton);
        picker_ = control(DATETIMEPICK_CLASSW, L"", DTS_SHORTDATEFORMAT, HistoryPicker);
        styleControl(picker_);
        SendMessageW(picker_, DTM_SETFORMATW, 0, reinterpret_cast<LPARAM>(L"yyyy-MM-dd HH:mm:ss"));
        rangeCombo_ = combo(RangeCombo, {L"60 秒", L"1 小时", L"24 小时", L"7 天"});
        queryButton_ = button(QueryButton);
        retentionEdit_ = control(WC_EDITW, L"7", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, RetentionEdit);
        SendMessageW(retentionEdit_, EM_SETLIMITTEXT, 4, 0);
        saveButton_ = button(SaveButton);
        lightThemeButton_ = button(LightThemeButton);
        darkThemeButton_ = button(DarkThemeButton);
        copyMcpButton_ = button(CopyMcpButton);
        for (int i = 0; i < static_cast<int>(sortButtons_.size()); ++i) {
            sortButtons_[i] = control(WC_BUTTONW, L"", BS_OWNERDRAW, LeftSortCpu + i);
            styleControl(sortButtons_[i]);
            SendMessageW(sortButtons_[i], WM_SETFONT, reinterpret_cast<WPARAM>(smallFont_), FALSE);
        }
        updateSortHeaders();
        rankTooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            0, 0, 0, 0, window_, nullptr, instance_, nullptr);
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = 1;
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(rankTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        coreList_ = detailList(CoreList, CoreColumns);
        diskList_ = detailList(DiskList, DiskColumns);
        diskSmartList_ = detailList(DiskSmartList, DiskSmartColumns);
        displayList_ = detailList(DisplayList, DisplayColumns);
        memoryTimingList_ = detailList(MemoryTimingList, MemoryTimingColumns);
    }

    void applyTheme() {
        theme_ = &ui::palette(darkMode_);
        if (surfaceBrush_) DeleteObject(surfaceBrush_);
        surfaceBrush_ = CreateSolidBrush(theme_->surface);

        constexpr DWORD darkTitleBar = 20;
        constexpr DWORD legacyDarkTitleBar = 19;
        const BOOL dark = darkMode_;
        if (FAILED(DwmSetWindowAttribute(window_, darkTitleBar, &dark, sizeof(dark)))) {
            DwmSetWindowAttribute(window_, legacyDarkTitleBar, &dark, sizeof(dark));
        }

        // The Windows theme class is best effort. The canvas and text colors are owned by the app.
        const auto nativeTheme = darkMode_ ? L"DarkMode_Explorer" : nullptr;
        SetWindowTheme(window_, nativeTheme, nullptr);
        SetWindowTheme(rankTooltip_, nativeTheme, nullptr);
        for (const auto child : {cpuCombo_, gpuCombo_, rangeCombo_, picker_, retentionEdit_, coreList_,
             diskList_, diskSmartList_, displayList_, memoryTimingList_}) {
            SetWindowTheme(child, nativeTheme, nullptr);
        }
        for (const auto list : {coreList_, diskList_, diskSmartList_, displayList_, memoryTimingList_}) {
            SetWindowTheme(ListView_GetHeader(list), darkMode_ ? L"DarkMode_ItemsView" : nullptr, nullptr);
            SendMessageW(list, LVM_SETTEXTCOLOR, 0, theme_->ink);
            SendMessageW(list, LVM_SETBKCOLOR, 0, theme_->surface);
            SendMessageW(list, LVM_SETTEXTBKCOLOR, 0, theme_->surface);
        }
        SendMessageW(picker_, DTM_SETMCCOLOR, MCSC_BACKGROUND, theme_->surface);
        SendMessageW(picker_, DTM_SETMCCOLOR, MCSC_MONTHBK, theme_->surface);
        SendMessageW(picker_, DTM_SETMCCOLOR, MCSC_TEXT, theme_->ink);
        SendMessageW(picker_, DTM_SETMCCOLOR, MCSC_TITLEBK, theme_->primaryFill);
        SendMessageW(picker_, DTM_SETMCCOLOR, MCSC_TITLETEXT, theme_->brandGlyph);
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_FRAME);
    }

    void chooseTheme(bool dark) {
        if (darkMode_ == dark) return;
        if (callbacks_.setDarkMode && !callbacks_.setDarkMode(dark)) {
            appearanceMessage_ = L"无法保存外观设置。";
            invalidate();
            return;
        }
        darkMode_ = dark;
        appearanceMessage_.clear();
        applyTheme();
    }

    DetailPane detailPane() const {
        if (tab_ == Tab::MemoryDetails) {
            return DetailPane::Memory;
        }
        if (tab_ == Tab::CpuDetails) {
            return DetailPane::Cpu;
        }
        if (tab_ == Tab::GpuDetails) {
            return DetailPane::Gpu;
        }
        if (tab_ == Tab::DiskDetails) {
            return DetailPane::Disk;
        }
        if (tab_ == Tab::NetworkDetails) {
            return DetailPane::Network;
        }
        return DetailPane::None;
    }

    int networkCardHeight(const NetworkConnection& connection) const {
        int rows = 3; // Model, link speed and MAC address.
        if (details_.network->addressesAvailable) {
            for (const auto* addresses : {&connection.ipv4, &connection.ipv6, &connection.gateways, &connection.dnsServers})
                rows += static_cast<int>(std::max<size_t>(1, addresses->size()));
        }
        return 52 + rows * NetworkRowHeight;
    }

    int networkPageHeight() const {
        int bottom = NetworkCardsTop;
        if (details_.network) {
            for (const auto& connection : details_.network->connections)
                bottom += networkCardHeight(connection) + ModuleGap;
        }
        bottom += 300 + static_cast<int>(details_.traffic.session.size()+details_.traffic.today.size()+details_.traffic.week.size())*28;
        return std::max(390, bottom + PageInset - ModuleGap);
    }

    RECT rankPanel(RECT area, size_t index) const {
        const int top = cards_.back().bottom + px(30);
        if (area.right < px(780)) {
            const int y = top + static_cast<int>(index) * px(RankHeight + ModuleGap);
            return {px(PageInset), y, area.right - px(PageInset), y + px(RankHeight)};
        }
        return index == 0
            ? RECT{px(PageInset), top, area.right / 2 - px(ModuleGap / 2), top + px(RankHeight)}
            : RECT{area.right / 2 + px(ModuleGap / 2), top, area.right - px(PageInset), top + px(RankHeight)};
    }

    int statusOffset(RECT area) const {
        const int inlineWidth = detailPane() == DetailPane::None ? 852 : 755;
        return area.right < px(inlineWidth) ? px(22) : 0;
    }

    const DisplayInfo* selectedDisplay() const {
        if (!displays_) return nullptr;
        const auto index = static_cast<size_t>(SendMessageW(displayList_, LVM_GETNEXTITEM,
            static_cast<WPARAM>(-1), LVNI_SELECTED));
        return index < displays_->size() ? &(*displays_)[index] : nullptr;
    }

    int displayPageHeight() const {
        const auto display = selectedDisplay();
        return display && display->gamut ? DisplayPlotPageHeight : DisplayPageHeight;
    }

    int boardPageHeight() const {
        int bottom = 124;
        if (!inventory_ || inventory_->board.empty()) return bottom + 82;
        for (const auto& section : inventory_->board) {
            bottom += 48 + 23 * static_cast<int>(std::max<size_t>(1, section.fields.size()));
        }
        return bottom + PageInset;
    }

    int memoryTimingTop() const {
        return MemorySectionsBody + (memoryPlotExpanded_ ? MemoryPlotHeight : 0);
    }

    int memoryDevicesTop() const {
        return memoryTimingTop() + (memoryTimingExpanded_ ? currentMemoryTimingHeight() + MemoryTimingHeight : 0);
    }

    int currentMemoryTimingHeight() const {
        return details_.memoryTimings && !details_.memoryTimings->fields.empty() ? 310 : 144;
    }

    int memoryPageHeight() const {
        int height = memoryDevicesTop() + PageInset;
        if (!inventory_ || inventory_->memoryDevices.empty()) return height + 34;
        for (const auto& device : inventory_->memoryDevices) {
            height += device.empty ? EmptyMemorySlotHeight : MemoryDeviceHeight;
        }
        return height;
    }

    const std::vector<std::wstring>& modelList(size_t card) const {
        const auto& inventory = *inventory_;
        const std::array lists{&inventory.processors, &inventory.memory, &inventory.disks, &inventory.graphics};
        return *lists[card];
    }

    bool showGpuScope() const {
        return inventory_ && !inventory_->sampledGpu.empty() &&
            (inventory_->graphics.size() != 1 || inventory_->graphics.front() != inventory_->sampledGpu);
    }

    int modelHeight() const {
        if (tab_ != Tab::Live) return 0;
        const auto height = inventory_ ? std::max({size_t{HardwareModelRowHeight},
            inventory_->processors.size() * HardwareModelRowHeight,
            inventory_->memory.size() * MemoryModelRowHeight, inventory_->disks.size() * HardwareModelRowHeight,
            inventory_->graphics.size() * HardwareModelRowHeight}) : HardwareModelRowHeight;
        return 26 + static_cast<int>(height);
    }

    void layout(UINT moveFlags = 0) {
        RECT area{};
        GetClientRect(window_, &area);
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.hwnd = window_;
        tool.uId = 1;
        tool.rect = area;
        SendMessageW(rankTooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
        const int pad = px(PageInset);
        const int gap = px(ModuleGap);
        const int columns = std::clamp((static_cast<int>(area.right) - 2 * pad + gap) /
            px(194 + ModuleGap), 2, 5);
        const int contentWidth = static_cast<int>(area.right) - 2 * pad - (columns - 1) * gap;
        const int extra = statusOffset(area);
        const int cardY = px(tab_ == Tab::History ? 126 : 76) + extra;
        const int cardHeight = CardHeight + modelHeight();
        for (size_t i = 0; i < cards_.size(); ++i) {
            const int column = static_cast<int>(i) % columns;
            const int y = cardY + static_cast<int>(i) / columns * px(cardHeight + ModuleGap);
            const int x = pad + column * gap + contentWidth * column / columns;
            const int right = pad + column * gap + contentWidth * (column + 1) / columns;
            cards_[i] = {x, y, right, y + px(cardHeight)};
        }
        const auto detail = detailPane();
        const bool inDetail = detail != DetailPane::None;
        const bool live = tab_ == Tab::Live;
        const bool history = tab_ == Tab::History;
        const bool settings = tab_ == Tab::Settings;
        const bool overview = live || history;
        const int contentBottom = overview ? rankPanel(area, 1).bottom + pad :
            px(settings ? McpCardBottom + PageInset : tab_ == Tab::Displays ? displayPageHeight() :
                tab_ == Tab::Board ? boardPageHeight() :
                detail == DetailPane::Memory ? memoryPageHeight() : detail == DetailPane::Disk ? DiskPageHeight :
                detail == DetailPane::Gpu ? GpuPageHeight :
                detail == DetailPane::Network ? networkPageHeight() : 390);
        scrollMax_ = std::max(0, contentBottom - static_cast<int>(area.bottom));
        scroll_ = std::clamp(scroll_, 0, scrollMax_);
        const SCROLLINFO scrollInfo{sizeof(SCROLLINFO), SIF_RANGE | SIF_PAGE | SIF_POS,
            0, std::max(contentBottom, static_cast<int>(area.bottom)) - 1,
            static_cast<UINT>(area.bottom), scroll_, 0};
        SetScrollInfo(window_, SB_VERT, &scrollInfo, TRUE);
        RECT afterScroll{};
        GetClientRect(window_, &afterScroll);
        if (afterScroll.right != area.right) {
            layout(moveFlags);
            return;
        }

        const auto move = [&](HWND control, int x, int y, int width, int height, bool showControl) {
            const bool wasVisible = (GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0;
            const UINT visibility = showControl == wasVisible ? 0 : showControl ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
            SetWindowPos(control, nullptr, x, y - scroll_, std::max(width, 0), std::max(height, 0),
                SWP_NOZORDER | SWP_NOACTIVATE | visibility | moveFlags);
        };

        const int nav = pad + px(209);
        const std::array tabs{liveButton_, historyButton_, displaysButton_, boardButton_, settingsButton_};
        const std::array captions{LiveButton, HistoryButton, DisplaysButton, BoardButton, SettingsButton};
        const int tabGap = px(3);
        std::array<int, tabs.size()> widths{};
        int minimumWidth = tabGap * static_cast<int>(tabs.size() - 1);
        const auto dc = GetDC(window_);
        for (size_t i = 0; i < tabs.size(); ++i) {
            const std::wstring_view caption = buttonCaption(captions[i]);
            const int captionWidth = dc ? textWidth(dc, normalFont_, caption) : px(13 * static_cast<int>(caption.size()));
            widths[i] = captionWidth + 2 * px(ui::TabTextInset) + px(2);
            minimumWidth += widths[i];
        }
        if (dc) ReleaseDC(window_, dc);
        const int extraPerTab = std::clamp(
            (static_cast<int>(area.right) - pad - nav - minimumWidth) / static_cast<int>(tabs.size()), 0, px(12));
        int x = nav;
        for (size_t i = 0; i < tabs.size(); ++i) {
            const int width = widths[i] + extraPerTab;
            move(tabs[i], x, px(17), width, px(32), !inDetail);
            x += width + tabGap;
        }
        move(refreshDisplaysButton_, area.right - pad - px(62), px(87), px(62), px(28), tab_ == Tab::Displays);
        move(detailBackButton_, nav, px(17), px(118), px(32), inDetail);
        move(cpuCombo_, cards_[0].left + px(CardInset), cards_[0].top + px(CardTools), px(125), px(125), overview);
        move(gpuCombo_, cards_[3].left + px(CardInset), cards_[3].top + px(CardTools), px(125), px(125), overview);
        move(cpuDetailButton_, cards_[0].right - px(CardInset + 46), cards_[0].top + px(12), px(46), px(24), live);
        move(memoryDetailButton_, cards_[1].right - px(CardInset + 46), cards_[1].top + px(12), px(46), px(24), live);
        move(memoryPlotButton_, pad, px(MemorySectionsTop), px(130), px(28), detail == DetailPane::Memory);
        move(memoryTimingButton_, pad + px(138), px(MemorySectionsTop), px(118), px(28), detail == DetailPane::Memory);
        move(importMemoryTimingButton_, area.right - pad - px(86),
            px(memoryTimingTop() + currentMemoryTimingHeight() + 10), px(74), px(26),
            detail == DetailPane::Memory && memoryTimingExpanded_);
        move(memoryTimingList_, pad + px(12), px(memoryTimingTop() + 132),
            static_cast<int>(area.right) - 2 * pad - px(24), px(160),
            detail == DetailPane::Memory && memoryTimingExpanded_ &&
            details_.memoryTimings && !details_.memoryTimings->fields.empty());
        move(gpuDetailButton_, cards_[3].right - px(CardInset + 46), cards_[3].top + px(12), px(46), px(24), live);
        move(diskDetailButton_, cards_[2].right - px(CardInset + 60), cards_[2].top + px(12), px(60), px(24), live);
        move(networkDetailButton_, cards_[4].right - px(CardInset + 46), cards_[4].top + px(12), px(46), px(24), live);
        move(picker_, pad + px(70), px(78) + extra, px(202), px(27), history);
        move(rangeCombo_, pad + px(281), px(78) + extra, px(92), px(120), history);
        move(queryButton_, pad + px(383), px(78) + extra, px(68), px(27), history);
        move(retentionEdit_, pad + px(116), px(144), px(68), px(29), settings);
        move(saveButton_, pad + px(224), px(144), px(74), px(29), settings);
        move(lightThemeButton_, pad + px(116), px(308), px(76), px(30), settings);
        move(darkThemeButton_, pad + px(200), px(308), px(76), px(30), settings);
        move(copyMcpButton_, pad + px(14), px(451), px(132), px(30), settings);
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
        const int detailBottom = std::max(static_cast<int>(area.bottom), contentBottom);
        move(coreList_, pad + px(12), px(253), static_cast<int>(area.right) - 2 * pad - px(24),
            std::max(0, detailBottom - px(274)), detail == DetailPane::Cpu && !details_.cores.empty());
        const int diskWidth = static_cast<int>(area.right) - 2 * pad - px(24);
        move(diskList_, pad + px(12), px(DiskSummaryTop), diskWidth,
            px(DiskSummaryHeight), detail == DetailPane::Disk && !details_.disks.empty());
        move(diskSmartList_, pad + px(12), px(DiskSmartTop), diskWidth, detailBottom - px(DiskSmartTop + 22),
            detail == DetailPane::Disk && diskSmartRowCount_ > 0);
        move(displayList_, pad + px(12), px(DisplayListTop), static_cast<int>(area.right) - 2 * pad - px(24),
            px(DisplayListHeight), tab_ == Tab::Displays && displays_ && !displays_->empty());
    }

    void scrollTo(int position) {
        position = std::clamp(position, 0, scrollMax_);
        if (position == scroll_) return;
        scroll_ = position;
        cursor_ = {-1, -1};
        SendMessageW(rankTooltip_, TTM_POP, 0, 0);
        // Repaint the canvas and children together after all controls reach their new positions.
        layout(SWP_NOREDRAW | SWP_NOCOPYBITS);
        RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void invalidate() const { InvalidateRect(window_, nullptr, FALSE); }

    void addTrayIcon() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = window_;
        tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = TrayMessage;
        tray_.hIcon = trayIcon_;
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
            } else if (tab_ == Tab::Displays) {
                refreshDisplays();
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
        const auto* selectedDisk = selectedSmartDisk();
        const auto diskNumber = selectedDisk ? std::optional<unsigned>(selectedDisk->number) : std::nullopt;
        const size_t previousTimings = details_.memoryTimings ? details_.memoryTimings->fields.size() : 0;
        const bool networkActive = detailPane() == DetailPane::Network;
        const int previousNetworkHeight = networkActive ? networkPageHeight() : 0;
        details_ = std::move(details);
        updateDetailList(coreList_, previousCores, details_.cores.size(), detailPane() == DetailPane::Cpu);
        updateDetailList(diskList_, previousDisks, details_.disks.size(), detailPane() == DetailPane::Disk);
        if (detailPane() == DetailPane::Disk && !details_.disks.empty()) {
            const auto disk = std::find_if(details_.disks.begin(), details_.disks.end(),
                [&](const DiskDetail& entry) { return entry.number == diskNumber; });
            const int index = disk == details_.disks.end() ? 0 : static_cast<int>(disk - details_.disks.begin());
            if (selectedDiskIndex() != index) {
                ListView_SetItemState(diskList_, index, LVIS_SELECTED, LVIS_SELECTED);
            }
        }
        refreshSmartRows();
        updateDetailList(memoryTimingList_, previousTimings,
            details_.memoryTimings ? details_.memoryTimings->fields.size() : 0,
            detailPane() == DetailPane::Memory && memoryTimingExpanded_);
        if (detailPane() == DetailPane::Memory && previousTimings !=
            (details_.memoryTimings ? details_.memoryTimings->fields.size() : 0)) layout();
        if (networkActive && previousNetworkHeight != networkPageHeight()) layout();
    }

    int selectedDiskIndex() const {
        return ListView_GetNextItem(diskList_, -1, LVNI_SELECTED);
    }

    const DiskDetail* selectedSmartDisk() const {
        if (details_.disks.empty()) return nullptr;
        const auto index = static_cast<size_t>(selectedDiskIndex());
        return &details_.disks[index < details_.disks.size() ? index : 0];
    }

    void refreshSmartRows() {
        const auto* disk = selectedSmartDisk();
        const auto count = disk ? disk->smartRows.size() : 0;
        updateDetailList(diskSmartList_, diskSmartRowCount_, count, detailPane() == DetailPane::Disk);
        diskSmartRowCount_ = count;
    }

    void refreshDisplays() {
        const auto previous = displays_ ? displays_->size() : 0;
        displays_ = activeDisplays();
        updateDetailList(displayList_, previous, displays_ ? displays_->size() : 0, true);
        layout();
        invalidate();
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
        if (!inventory_ && callbacks_.inventory) {
            inventory_ = callbacks_.inventory();
            if (inventory_ && (tab_ == Tab::Live || tab_ == Tab::MemoryDetails || tab_ == Tab::Board)) layout();
        }
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
        if (list == diskSmartList_) {
            const auto* disk = selectedSmartDisk();
            if (disk && row < disk->smartRows.size() && column >= 0 && column < 3)
                return disk->smartRows[row][column];
            return {};
        }
        if (list == memoryTimingList_ && details_.memoryTimings && row < details_.memoryTimings->fields.size()) {
            const auto& field = details_.memoryTimings->fields[row];
            switch (column) {
            case 0: return field.name;
            case 1: return decimal(field.value.value_or(Missing), field.unit == L"ratio" ? 3 : 0, L"");
            case 2: return field.unit;
            case 3: return readingStatus(field.status);
            case 4: return field.scope;
            case 5: return field.sampledAtMs ? timeText(field.sampledAtMs, true) : L"—";
            case 6: return field.source;
            case 7: return field.reason;
            }
        }
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
        } else if (list == displayList_ && displays_ && row < displays_->size()) {
            const auto& display = (*displays_)[row];
            switch (column) {
            case 0: return std::to_wstring(row + 1);
            case 1: return display.name.empty() ? L"—" : display.name;
            case 2: return display.signalWidth && display.signalHeight ?
                std::to_wstring(display.signalWidth) + L" × " + std::to_wstring(display.signalHeight) : L"—";
            case 3: return display.refreshHz ? decimal(*display.refreshHz, 2, L" Hz") : L"—";
            case 4: return display.connection.empty() ? L"—" : display.connection;
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
        return {card.left + px(CardInset), card.top + px(151), card.right - px(CardInset), card.top + px(CardHeight - CardInset)};
    }

    RECT chartPlot(RECT area) const {
        InflateRect(&area, -px(5), -px(3));
        return area;
    }

    void clickChart(int x, int y) {
        const POINT point{x, y};
        if (tab_ == Tab::Live) {
            const auto chart = chartRect(cards_[1]);
            if (PtInRect(&chart, point)) {
                memoryPlotExpanded_ = true;
                updateMemorySections();
                activate(Tab::MemoryDetails);
            }
            return;
        }
        if (tab_ != Tab::History || history_.empty()) {
            return;
        }
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

    void updateMemorySections() {
        SetWindowTextW(memoryPlotButton_, memoryPlotExpanded_ ? L"收起占用率曲线" : L"展开占用率曲线");
        SetWindowTextW(memoryTimingButton_, memoryTimingExpanded_ ? L"收起硬件时序" : L"展开硬件时序");
        layout();
        invalidate();
    }

    void importMemoryTiming() {
        wchar_t path[32768]{};
        OPENFILENAMEW picker{};
        picker.lStructSize = sizeof(picker);
        picker.hwndOwner = window_;
        picker.lpstrFilter = L"HWiNFO HTML 报告\0*.htm;*.html\0\0";
        picker.lpstrFile = path;
        picker.nMaxFile = std::size(path);
        picker.lpstrTitle = L"选择本机 HWiNFO HTML 报告";
        picker.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&picker)) return;
        try {
            memoryTiming_ = readMemoryTimingReport(path);
        } catch (const std::exception&) {
            memoryTiming_.reset();
        }
        memoryTimingSource_ = memoryTiming_ ? L"HWiNFO 报告" : L"报告中没有可读取的主时序。";
        WIN32_FILE_ATTRIBUTE_DATA attributes{};
        if (memoryTiming_ && GetFileAttributesExW(path, GetFileExInfoStandard, &attributes)) {
            const auto& time = attributes.ftLastWriteTime;
            const ULARGE_INTEGER stamp{{time.dwLowDateTime, time.dwHighDateTime}};
            const auto ms = static_cast<int64_t>(stamp.QuadPart / 10000) - EpochOffset / 10000;
            memoryTimingSource_ += L" · 文件时间 " + timeText(ms, true);
        }
        invalidate();
    }

    void activate(Tab target) {
        tab_ = target;
        scroll_ = 0;
        for (const auto child : {liveButton_, historyButton_, displaysButton_, boardButton_, settingsButton_}) {
            InvalidateRect(child, nullptr, FALSE);
        }
        layout();
        syncDetailRequest();
        if (target == Tab::Live || detailPane() != DetailPane::None) {
            if (visible_.value_or(false)) {
                refreshLive();
                SetFocus(detailPane() == DetailPane::None ? liveButton_ : detailBackButton_);
            }
        } else if (target == Tab::Displays) {
            if (visible_.value_or(false)) refreshDisplays();
        } else if (target == Tab::History && !historyEnd_) {
            setPicker(nowMs());
            queryHistory();
        } else if (target == Tab::Settings) {
            settingsMessage_.clear();
            mcpMessage_.clear();
            appearanceMessage_.clear();
            try {
                const auto days = callbacks_.retention ? callbacks_.retention() : 7;
                SetWindowTextW(retentionEdit_, std::to_wstring(days).c_str());
            } catch (...) {
                settingsMessage_ = L"读取设置失败。";
            }
        }
        invalidate();
    }

    void copyMcpConfig() {
        const auto script = std::filesystem::path(callbacks_.executable).parent_path() / L"mcp_server.py";
        std::error_code error;
        if (callbacks_.executable.empty() || !std::filesystem::is_regular_file(script, error)) {
            mcpMessage_ = L"找不到 MCP 服务脚本，请重新构建或安装。";
        } else {
            const std::wstring config = L"{\n  \"mcpServers\": {\n    \"computer-watch\": {\n"
                L"      \"command\": \"uv\",\n      \"args\": [\"run\", \"--script\", \"" +
                script.generic_wstring() + L"\"]\n    }\n  }\n}";
            const auto bytes = (config.size() + 1) * sizeof(wchar_t);
            HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, bytes);
            bool copied = false;
            if (data) {
                if (auto* buffer = static_cast<wchar_t*>(GlobalLock(data))) {
                    std::wmemcpy(buffer, config.c_str(), config.size() + 1);
                    GlobalUnlock(data);
                    if (OpenClipboard(window_)) {
                        copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, data);
                        CloseClipboard();
                    }
                }
                if (!copied) GlobalFree(data);
            }
            mcpMessage_ = copied ? L"已复制，请粘贴到 MCP 客户端。" : L"无法复制，请重试。";
        }
        InvalidateRect(window_, nullptr, FALSE);
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

    bool drawControl(DRAWITEMSTRUCT item) const {
        if (item.CtlType == ODT_COMBOBOX) {
            if (item.itemState & ODS_COMBOBOXEDIT) {
                paintCombo(item.hwndItem, item.hDC, item.rcItem);
                return true;
            }
            const bool selected = (item.itemState & ODS_SELECTED) != 0;
            const bool contrast = ui::detail::highContrast();
            const auto background = contrast ? GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_WINDOW)
                : selected ? theme_->selectionFill : theme_->surface;
            const auto ink = contrast ? GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT)
                : selected ? theme_->blue : theme_->ink;
            fill(item.hDC, item.rcItem, background);
            auto bounds = item.rcItem;
            bounds.left += px(8);
            bounds.right -= px(6);
            label(item.hDC, smallFont_, comboText(item.hwndItem, static_cast<int>(item.itemID)), bounds, ink);
            return true;
        }
        if (GetPropW(item.hwndItem, L"ComputerWatch.Hot")) item.itemState |= ODS_HOTLIGHT;
        using ui::ButtonKind;
        const auto id = item.CtlID;
        if (id >= LeftSortCpu && id <= RightSortMemory) {
            const auto index = id - LeftSortCpu;
            const auto field = index % 2 == 0 ? RankField::Cpu : RankField::Memory;
            return ui::drawButton(item, dpi_, ButtonKind::Sort,
                rankSorts_[index / 2].field == field, smallFont_, sortCaption(index / 2, field), *theme_);
        }
        const auto caption = buttonCaption(id);
        if (!caption) return false;
        auto kind = ButtonKind::Action;
        if (id == LiveButton || id == HistoryButton || id == DisplaysButton || id == BoardButton || id == SettingsButton ||
            id == DetailBackButton) {
            kind = ButtonKind::Tab;
        } else if (id == QueryButton || id == SaveButton) {
            kind = ButtonKind::Primary;
        }
        const bool selected = (id == LiveButton && tab_ == Tab::Live) ||
            (id == HistoryButton && tab_ == Tab::History) || (id == DisplaysButton && tab_ == Tab::Displays) ||
            (id == BoardButton && tab_ == Tab::Board) || (id == SettingsButton && tab_ == Tab::Settings) ||
            (id == LightThemeButton && !darkMode_) || (id == DarkThemeButton && darkMode_);
        return ui::drawButton(item, dpi_, kind, selected, normalFont_, caption, *theme_);
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
            applyTheme();
            layout();
            addTrayIcon();
            return 0;
        case WM_GETMINMAXINFO: {
            const auto limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = {px(550), px(380)};
            return 0;
        }
        case WM_DPICHANGED: {
            const int previousDpi = dpi_;
            dpi_ = LOWORD(wParam);
            scroll_ = MulDiv(scroll_, dpi_, previousDpi);
            refreshFonts();
            for (const auto child : {cpuCombo_, gpuCombo_, rangeCombo_}) sizeCombo(child);
            scaleColumns(coreList_, CoreColumns);
            scaleColumns(diskList_, DiskColumns);
            scaleColumns(diskSmartList_, DiskSmartColumns);
            scaleColumns(displayList_, DisplayColumns);
            scaleColumns(memoryTimingList_, MemoryTimingColumns);
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
        case WM_DISPLAYCHANGE:
        case WM_DEVICECHANGE:
            if (tab_ == Tab::Displays && visible_.value_or(false) &&
                (message == WM_DISPLAYCHANGE || wParam == DBT_DEVNODES_CHANGED)) {
                refreshDisplays();
            }
            break;
        case WM_TIMER:
            if (wParam == RefreshTimer && (tab_ == Tab::Live || detailPane() != DetailPane::None ||
                (tab_ == Tab::Board && !inventory_))
                && visible_.value_or(false)) {
                refreshLive();
            }
            return 0;
        case WM_MOUSEWHEEL:
            wheelDelta_ += GET_WHEEL_DELTA_WPARAM(wParam);
            scrollTo(scroll_ - wheelDelta_ / WHEEL_DELTA * px(72));
            wheelDelta_ %= WHEEL_DELTA;
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO info{};
            info.cbSize = sizeof(info);
            info.fMask = SIF_TRACKPOS | SIF_PAGE;
            GetScrollInfo(window_, SB_VERT, &info);
            switch (LOWORD(wParam)) {
            case SB_LINEUP: scrollTo(scroll_ - px(32)); break;
            case SB_LINEDOWN: scrollTo(scroll_ + px(32)); break;
            case SB_PAGEUP: scrollTo(scroll_ - static_cast<int>(info.nPage)); break;
            case SB_PAGEDOWN: scrollTo(scroll_ + static_cast<int>(info.nPage)); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: scrollTo(info.nTrackPos); break;
            case SB_TOP: scrollTo(0); break;
            case SB_BOTTOM: scrollTo(scrollMax_); break;
            }
            return 0;
        }
        case WM_DRAWITEM:
            if (lParam && drawControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) return TRUE;
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (surfaceBrush_) {
                const auto dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, theme_->ink);
                SetBkColor(dc, theme_->surface);
                return reinterpret_cast<LRESULT>(surfaceBrush_);
            }
            break;
        case ButtonMessage:
            switch (wParam) {
            case LiveButton: activate(Tab::Live); break;
            case HistoryButton: activate(Tab::History); break;
            case DisplaysButton: activate(Tab::Displays); break;
            case BoardButton: activate(Tab::Board); break;
            case RefreshDisplaysButton: refreshDisplays(); break;
            case SettingsButton: activate(Tab::Settings); break;
            case QueryButton: queryHistory(); break;
            case SaveButton: saveSettings(); break;
            case CpuDetailButton: activate(Tab::CpuDetails); break;
            case MemoryDetailButton: activate(Tab::MemoryDetails); break;
            case MemoryPlotButton:
                memoryPlotExpanded_ = !memoryPlotExpanded_;
                updateMemorySections();
                break;
            case MemoryTimingButton:
                memoryTimingExpanded_ = !memoryTimingExpanded_;
                updateMemorySections();
                break;
            case ImportMemoryTimingButton: importMemoryTiming(); break;
            case GpuDetailButton: activate(Tab::GpuDetails); break;
            case DiskDetailButton: activate(Tab::DiskDetails); break;
            case NetworkDetailButton: activate(Tab::NetworkDetails); break;
            case DetailBackButton: activate(Tab::Live); break;
            case LeftSortCpu: sortBy(0, RankField::Cpu); break;
            case LeftSortMemory: sortBy(0, RankField::Memory); break;
            case RightSortCpu: sortBy(1, RankField::Cpu); break;
            case RightSortMemory: sortBy(1, RankField::Memory); break;
            case LightThemeButton: chooseTheme(false); break;
            case DarkThemeButton: chooseTheme(true); break;
            case CopyMcpButton: copyMcpConfig(); break;
            }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED && lParam) {
                // Leave the native control's notification before changing focus or layout.
                PostMessageW(window_, ButtonMessage, LOWORD(wParam), 0);
                return 0;
            }
            if (HIWORD(wParam) == CBN_DROPDOWN || HIWORD(wParam) == CBN_CLOSEUP ||
                HIWORD(wParam) == CBN_SELCHANGE) {
                const auto id = LOWORD(wParam);
                if (id == CpuCombo || id == GpuCombo || id == RangeCombo) {
                    InvalidateRect(reinterpret_cast<HWND>(lParam), nullptr, FALSE);
                    if (HIWORD(wParam) != CBN_SELCHANGE) return 0;
                }
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
            if (notification->hwndFrom == picker_ && notification->code == DTN_DROPDOWN && darkMode_) {
                const auto calendar = DateTime_GetMonthCal(picker_);
                if (calendar) SetWindowTheme(calendar, L"", nullptr);
            }
            if (notification->hwndFrom == rankTooltip_ && notification->code == TTN_GETDISPINFOW) {
                const auto tip = std::find_if(rankTips_.begin(), rankTips_.end(),
                    [&](const RankTip& entry) { return PtInRect(&entry.bounds, cursor_); });
                const auto model = std::find_if(modelTips_.begin(), modelTips_.end(),
                    [&](const ModelTip& entry) { return PtInRect(&entry.bounds, cursor_); });
                tooltipText_ = model != modelTips_.end() ? *model->text :
                    tip == rankTips_.end() ? L"" : tip->text;
                reinterpret_cast<NMTTDISPINFOW*>(lParam)->lpszText = tooltipText_.data();
                return 0;
            }
            if (notification->hwndFrom != coreList_ && notification->hwndFrom != diskList_ &&
                notification->hwndFrom != diskSmartList_ && notification->hwndFrom != displayList_ &&
                notification->hwndFrom != memoryTimingList_) {
                break;
            }
            if (notification->code == LVN_GETDISPINFOW) {
                auto& item = reinterpret_cast<NMLVDISPINFOW*>(lParam)->item;
                if ((item.mask & LVIF_TEXT) && item.pszText && item.cchTextMax > 0) {
                    const auto text = detailListValue(notification->hwndFrom, item.iItem, item.iSubItem);
                    lstrcpynW(item.pszText, text.c_str(), item.cchTextMax);
                }
            } else if ((notification->hwndFrom == diskList_ || notification->hwndFrom == displayList_) &&
                (notification->code == LVN_ITEMCHANGED || notification->code == LVN_ODSTATECHANGED)) {
                if (notification->hwndFrom == displayList_ && tab_ == Tab::Displays) layout();
                if (notification->hwndFrom == diskList_) refreshSmartRows();
                invalidate();
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            const POINT position{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) + scroll_};
            for (const auto& tip : rankTips_) {
                if (PtInRect(&tip.bounds, position) != PtInRect(&tip.bounds, cursor_)) {
                    SendMessageW(rankTooltip_, TTM_POP, 0, 0);
                    invalidate();
                    break;
                }
            }
            for (const auto& tip : modelTips_) {
                if (PtInRect(&tip.bounds, position) != PtInRect(&tip.bounds, cursor_)) {
                    SendMessageW(rankTooltip_, TTM_POP, 0, 0);
                    break;
                }
            }
            cursor_ = position;
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0};
            TrackMouseEvent(&track);
            break;
        }
        case WM_MOUSELEAVE:
            cursor_ = {-1, -1};
            invalidate();
            break;
        case WM_LBUTTONUP:
            clickChart(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) + scroll_);
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
        COLORREF color, int64_t start, int64_t end, double maximum, bool shade = false) const {
        const auto pen = CreatePen(PS_SOLID, px(2), color);
        if (!pen) {
            return;
        }
        const auto old = SelectObject(dc, pen);
        const auto oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
        const auto tint = [](int base, int accent) { return (base * 9 + accent) / 10; };
        const auto surface = theme_->surface;
        const auto shadeColor = RGB(tint(GetRValue(surface), GetRValue(color)),
            tint(GetGValue(surface), GetGValue(color)), tint(GetBValue(surface), GetBValue(color)));
        POINT previousPoint{};
        bool previous = false;
        int64_t previousTime = 0;
        const auto maxGap = tab_ != Tab::History ? 7'500 : std::max<int64_t>(
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
                if (shade) {
                    const POINT areaFill[] = {previousPoint, {x, y},
                        {x, area.bottom - 1}, {previousPoint.x, area.bottom - 1}};
                    const auto stroke = SelectObject(dc, GetStockObject(NULL_PEN));
                    SetDCBrushColor(dc, shadeColor);
                    Polygon(dc, areaFill, static_cast<int>(std::size(areaFill)));
                    SelectObject(dc, stroke);
                    MoveToEx(dc, previousPoint.x, previousPoint.y, nullptr);
                }
                LineTo(dc, x, y);
            } else {
                fill(dc, {x, y, x + px(2), y + px(2)}, color);
                MoveToEx(dc, x, y, nullptr);
            }
            previous = true;
            previousTime = sample.timestampMs;
            previousPoint = {x, y};
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    void paintChart(HDC dc, const RECT& area, Chart chart, const std::vector<Snapshot>& samples,
        int64_t start, int64_t end, COLORREF accent) const {
        const auto plot = chartPlot(area);
        for (int i = 1; i <= 3; ++i) {
            const int x = plot.left + (plot.right - plot.left) * i / 4;
            line(dc, x, plot.top, x, plot.bottom, theme_->grid);
        }
        line(dc, plot.left, (plot.top + plot.bottom) / 2, plot.right, (plot.top + plot.bottom) / 2, theme_->grid);
        line(dc, plot.left, plot.bottom - 1, plot.right, plot.bottom - 1, theme_->grid);
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
            label(dc, smallFont_, L"暂无曲线", area, theme_->muted, DT_CENTER);
            return;
        }
        chartLine(dc, plot, samples, chart.primary, accent, start, end, maximum, !chart.secondary);
        if (chart.secondary) {
            chartLine(dc, plot, samples, *chart.secondary, theme_->teal, start, end, maximum);
        }
        if (tab_ == Tab::History && historyTarget_ >= start && historyTarget_ <= end) {
            const int x = plot.left + static_cast<int>((historyTarget_ - start) *
                (plot.right - plot.left - 1) / (end - start));
            line(dc, x, plot.top, x, plot.bottom, theme_->muted);
        }
    }

    void cardValue(HDC dc, std::wstring_view text, RECT area) const {
        const auto separator = text.find_first_of(L" %");
        const auto number = text.substr(0, separator);
        const auto valueFace = textWidth(dc, valueFont_, text) > area.right - area.left && area.right - area.left < px(110)
            ? (compactValueFont_ ? compactValueFont_ : boldFont_) : valueFont_;
        label(dc, valueFace, number, area);
        if (separator != std::wstring_view::npos) {
            const auto unit = text.substr(separator + (text[separator] == L' ' ? 1 : 0));
            area.left += textWidth(dc, valueFace, number) + px(5);
            if (valueFace == valueFont_) area.top += px(11);
            if (area.left < area.right) label(dc, normalFont_, unit, area, theme_->secondary);
        }
    }

    void paintCard(HDC dc, size_t index, const Snapshot* snapshot, const std::vector<Snapshot>& samples,
        int64_t start, int64_t end) const {
        static constexpr std::array names{L"CPU", L"内存", L"磁盘", L"显卡", L"网络"};
        const auto card = cards_[index];
        const auto accent = theme_->cardColors[index];
        const int left = card.left + px(CardInset);
        const int right = card.right - px(CardInset);
        rounded(dc, card, px(22), theme_->surface, theme_->border);
        static constexpr std::array metricNames{L"处理器占用率", L"内存占用率", L"已用容量", L"显卡占用率", L"下载速率"};
        label(dc, boldFont_, names[index], {left, card.top + px(12), right, card.top + px(36)});
        const auto metricName = index == 3 && tab_ == Tab::Live && showGpuScope()
            ? L"占用率 · NVML 0" : metricNames[index];
        label(dc, smallFont_, metricName, {left, card.top + px(39), right, card.top + px(57)}, theme_->muted);
        std::wstring primary;
        std::wstring detail;
        std::wstring firstLegend;
        std::wstring secondLegend;
        switch (index) {
        case 0:
            primary = decimal(value(snapshot, Metric::CpuPercent), 1, L"%");
            detail = L"封装 " + decimal(value(snapshot, Metric::CpuTemp), 0, L" °C") + L"  ·  "
                + decimal(value(snapshot, Metric::CpuWatts), 1, L" W");

            break;
        case 1:
            primary = decimal(value(snapshot, Metric::MemoryPercent), 1, L"%");
            detail = bytes(value(snapshot, Metric::MemoryUsed)) + L" / " + bytes(value(snapshot, Metric::MemoryTotal));
            firstLegend = tab_ == Tab::Live ? L"占用率 % · 展开" : L"占用率 %";
            break;
        case 2:
            primary = bytes(value(snapshot, Metric::DiskUsed));
            detail = L"总容量 " + bytes(value(snapshot, Metric::DiskTotal));
            label(dc, smallFont_, L"读 " + bytes(value(snapshot, Metric::DiskRead), true) +
                L"  /  写 " + bytes(value(snapshot, Metric::DiskWrite), true),
                {left, card.top + px(107), right, card.top + px(125)}, theme_->secondary);
            firstLegend = L"读取";
            secondLegend = L"写入";
            break;
        case 3:
            primary = decimal(value(snapshot, Metric::GpuPercent), 1, L"%");
            detail = L"核心 " + decimal(value(snapshot, Metric::GpuTemp), 0, L" °C") + L"  ·  "
                + decimal(value(snapshot, Metric::GpuWatts), 1, L" W");
            label(dc, smallFont_, L"显存 " + bytes(value(snapshot, Metric::GpuMemoryUsed)) + L" / "
                + bytes(value(snapshot, Metric::GpuMemoryTotal)),
                {left, card.top + px(107), right, card.top + px(125)}, theme_->secondary);
            break;
        case 4:
            primary = bytes(value(snapshot, Metric::NetworkDown), true);
            detail = L"上传 " + bytes(value(snapshot, Metric::NetworkUp), true);
            firstLegend = L"下载";
            secondLegend = L"上传";
            break;
        }
        cardValue(dc, primary, {left, card.top + px(54), right, card.top + px(91)});
        label(dc, smallFont_, detail, {left, card.top + px(89), right, card.top + px(107)}, theme_->secondary);
        const int legendMid = (left + right) / 2;
        const int toolsY = card.top + px(CardTools);
        const auto legend = [&](int x, int end, std::wstring_view text, COLORREF color) {
            fill(dc, {x, toolsY + px(12), x + px(10), toolsY + px(14)}, color);
            label(dc, smallFont_, text, {x + px(15), toolsY, end, toolsY + px(26)}, theme_->secondary);
        };
        if (!secondLegend.empty()) {
            legend(left, legendMid, firstLegend, accent);
            legend(legendMid, right, secondLegend, theme_->teal);
        } else if (!firstLegend.empty()) {
            legend(left, right, firstLegend, accent);
        }
        paintChart(dc, chartRect(card), chartFor(index), samples, start, end, accent);
        if (tab_ == Tab::Live && index < 4) paintModels(dc, index, card);
    }

    void paintModels(HDC dc, size_t index, RECT card) const {
        const int left = card.left + px(CardInset);
        const int right = card.right - px(CardInset);
        const int top = card.top + px(CardHeight - 4);
        line(dc, left, top, right, top, theme_->border);
        label(dc, smallFont_, L"硬件型号", {left, top + px(5), right, top + px(24)}, theme_->muted);
        const int height = index == 1 ? MemoryModelRowHeight : HardwareModelRowHeight;
        RECT row{left, top + px(25), right, top + px(25 + height)};
        if (!inventory_ || modelList(index).empty()) {
            label(dc, smallFont_, inventory_ ? L"型号未提供" : L"正在读取…", row, theme_->secondary);
            return;
        }
        int rowIndex = 0;
        for (const auto& model : modelList(index)) {
            row.top = top + px(25 + rowIndex * height);
            row.bottom = top + px(25 + (++rowIndex) * height);
            std::wstring_view display = model;
            if (index == 1 && display.starts_with(L"DIMM_")) display.remove_prefix(5);
            const bool wraps = textWidth(dc, smallFont_, display) > right - left;
            if (wraps && index != 1) paragraph(dc, smallFont_, display, row, theme_->ink);
            else {
                auto caption = row;
                caption.bottom = caption.top + px(MemoryModelRowHeight);
                label(dc, smallFont_, display, caption, theme_->ink);
            }
            if (wraps || display.size() != model.size()) modelTips_.push_back({row, &model});
        }
        if (index == 3 && showGpuScope()) {
            const RECT scope{left, card.top + px(39), right, card.top + px(57)};
            modelTips_.push_back({scope, &inventory_->sampledGpu});
        }
    }

    void paintRanks(HDC dc, RECT panel, const wchar_t* title,
        const std::array<ProcessEntry, TopCount>* ranks, RankSort sort, size_t table) const {
        rounded(dc, panel, px(22), theme_->surface, theme_->border);
        const int left = panel.left + px(RankInset);
        const int right = panel.right - px(RankInset);
        const int pidWidth = px(62);
        const int cpuWidth = px(RankCpuWidth);
        const int memWidth = px(RankMemoryWidth);
        label(dc, boldFont_, title, {left, panel.top + px(7), right, panel.top + px(34)});
        const int headerY = panel.top + px(RankHeaderOffset);
        auto drawRow = [&](int y, int height, std::wstring_view name, std::wstring_view pid,
            std::wstring_view cpu, std::wstring_view memory, COLORREF color) {
            label(dc, smallFont_, name, {left, y, right - pidWidth - cpuWidth - memWidth, y + height}, color);
            label(dc, smallFont_, pid, {right - pidWidth - cpuWidth - memWidth, y, right - cpuWidth - memWidth - px(7), y + height}, theme_->muted, DT_RIGHT);
            label(dc, smallFont_, cpu, {right - cpuWidth - memWidth, y, right - memWidth - px(7), y + height}, color, DT_RIGHT);
            label(dc, smallFont_, memory, {right - memWidth, y, right - px(7), y + height}, color, DT_RIGHT);
        };
        drawRow(headerY, px(RankHeaderHeight), L"进程", L"PID", L"", L"", theme_->muted);
        const int rowsY = headerY + px(RankHeaderHeight + 4);
        line(dc, left, rowsY, right, rowsY, theme_->grid);
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
            label(dc, normalFont_, L"暂无进程记录", {left, rowsY + px(14), right, rowsY + px(42)}, theme_->muted);
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
            const RECT row{left, y, right, y + rowHeight};
            if (PtInRect(&row, cursor_)) fill(dc, row, theme_->selectionFill);
            if (i + 1 < count) line(dc, left, row.bottom, right, row.bottom, theme_->border);
            const auto end = std::find(std::begin(process.name), std::end(process.name), L'\0');
            const std::wstring_view name(process.name, static_cast<size_t>(end - std::begin(process.name)));
            auto& tip = rankTips_[table * TopCount + i];
            tip.bounds = row;
            if (textWidth(dc, smallFont_, name) > right - left - pidWidth - cpuWidth - memWidth) tip.text = name;
            drawRow(y, rowHeight, name, std::to_wstring(process.pid),
                decimal(hasCpu(&process) ? process.cpu : Missing, 1, L"%"), bytes(static_cast<double>(process.memory)), theme_->ink);
        }
    }

    void paintSettings(HDC dc, RECT area) const {
        const int pad = px(24);
        rounded(dc, {pad, px(80), area.right - pad, px(229)}, px(22), theme_->surface, theme_->border);
        label(dc, boldFont_, L"历史保留", {pad + px(14), px(89), area.right - pad, px(117)});
        label(dc, smallFont_, L"默认 7 天。超过保留时长的记录会自动清理。",
            {pad + px(14), px(115), area.right - pad, px(139)}, theme_->secondary);
        label(dc, normalFont_, L"保留时长", {pad + px(14), px(144), pad + px(107), px(173)}, theme_->secondary);
        label(dc, normalFont_, L"天", {pad + px(191), px(144), pad + px(218), px(173)}, theme_->secondary);
        label(dc, smallFont_, settingsMessage_, {pad + px(14), px(185), area.right - pad, px(212)}, theme_->blue);

        rounded(dc, {pad, px(245), area.right - pad, px(368)}, px(22), theme_->surface, theme_->border);
        label(dc, boldFont_, L"外观主题", {pad + px(14), px(255), area.right - pad, px(284)});
        label(dc, smallFont_, L"切换后立即生效，下次打开沿用所选主题。",
            {pad + px(14), px(283), area.right - pad, px(304)}, theme_->secondary);
        label(dc, normalFont_, L"显示模式", {pad + px(14), px(308), pad + px(107), px(338)}, theme_->secondary);
        label(dc, smallFont_, appearanceMessage_,
            {pad + px(290), px(308), area.right - pad - px(14), px(338)}, theme_->amber);

        rounded(dc, {pad, px(384), area.right - pad, px(McpCardBottom)}, px(22), theme_->surface, theme_->border);
        label(dc, boldFont_, L"MCP 客户端接入", {pad + px(14), px(393), area.right - pad, px(421)});
        label(dc, smallFont_, L"通过本机 stdio 查询采样、硬件详情与流量，支持按需刷新。需要 uv。",
            {pad + px(14), px(420), area.right - pad - px(14), px(444)}, theme_->secondary);
        label(dc, smallFont_, mcpMessage_,
            {pad + px(158), px(451), area.right - pad - px(14), px(481)}, theme_->blue);
    }

    void paintDetailSummary(HDC dc, RECT area,
        std::initializer_list<std::pair<const wchar_t*, std::wstring>> values) const {
        const int gap = px(10);
        const int count = static_cast<int>(values.size());
        const int width = (area.right - area.left - gap * (count - 1)) / count;
        int x = area.left;
        for (const auto& [title, reading] : values) {
            const int right = x + width;
            rounded(dc, {x, area.top, right, area.bottom}, px(14), theme_->surface, theme_->border);
            label(dc, normalFont_, title, {x + px(12), area.top + px(9), right - px(12), area.top + px(32)}, theme_->secondary);
            if (reading.find(L" / ") != std::wstring::npos) {
                label(dc, normalFont_, reading, {x + px(12), area.top + px(34), right - px(12), area.bottom - px(8)});
            } else {
                cardValue(dc, reading, {x + px(12), area.top + px(30), right - px(12), area.bottom - px(5)});
            }
            x = right + gap;
        }
    }

    void paintNetworkDetails(HDC dc, RECT area, const Snapshot* snapshot) const {
        const int left = px(PageInset);
        const int right = area.right - left;
        const auto& network = details_.network;
        label(dc, titleFont_, L"网络实时详情", {left, px(83), right, px(120)});
        paintDetailSummary(dc, {left, px(130), right, px(205)}, {
            {L"物理网卡", network ? std::to_wstring(network->physicalAdapters) : L"—"},
            {L"虚拟网卡", network ? std::to_wstring(network->virtualAdapters) : L"—"},
            {L"当前下载", bytes(value(snapshot, Metric::NetworkDown), true)},
            {L"当前上传", bytes(value(snapshot, Metric::NetworkUp), true)}
        });
        label(dc, boldFont_, L"当前网络连接", {left, px(212), right, px(244)});
        if (!network || !network->addressesAvailable || network->connections.empty()) {
            const auto message = !network ? (details_.pane == DetailPane::Network ?
                L"无法读取网卡信息" : L"正在读取网卡信息…") :
                !network->addressesAvailable ? L"无法读取 IP 配置" : L"暂无活动网络连接";
            label(dc, normalFont_, message, {left + px(125), px(212), right, px(244)}, theme_->muted);
        }
        if (!network) return;

        const auto speed = [](double bits) {
            return bits >= 1e9 ? decimal(bits / 1e9, 2, L" Gbps") : decimal(bits / 1e6, 1, L" Mbps");
        };
        int top = NetworkCardsTop;
        for (const auto& connection : network->connections) {
            const int height = networkCardHeight(connection);
            rounded(dc, {left, px(top), right, px(top + height)}, px(14), theme_->surface, theme_->border);
            const auto title = (connection.name.empty() ? L"网络连接" : connection.name) +
                (connection.physical ? L"  ·  物理网卡" : L"  ·  虚拟网卡");
            label(dc, boldFont_, title, {left + px(14), px(top + 6), right - px(14), px(top + 37)});
            int row = top + 39;
            const auto field = [&](std::wstring_view name, std::wstring_view reading) {
                label(dc, smallFont_, name, {left + px(14), px(row), left + px(144), px(row + NetworkRowHeight)},
                    theme_->secondary);
                label(dc, normalFont_, reading.empty() ? L"—" : reading,
                    {left + px(146), px(row), right - px(14), px(row + NetworkRowHeight)});
                row += NetworkRowHeight;
            };
            const auto addresses = [&](std::wstring_view name, const std::vector<std::wstring>& entries) {
                if (entries.empty()) field(name, L"—");
                for (size_t index = 0; index < entries.size(); ++index)
                    field(index == 0 ? name : L"", entries[index]);
            };
            field(L"网卡型号", connection.description);
            field(L"链路速率（收 / 发）",
                speed(connection.receiveBitsPerSecond) + L" / " + speed(connection.transmitBitsPerSecond));
            field(L"MAC 地址", connection.macAddress);
            if (network->addressesAvailable) {
                addresses(L"IPv4 地址", connection.ipv4);
                addresses(L"IPv6 地址", connection.ipv6);
                addresses(L"默认网关", connection.gateways);
                addresses(L"DNS 服务器", connection.dnsServers);
            }
            top += height + ModuleGap;
        }
        const auto& traffic=details_.traffic;
        label(dc,boldFont_,L"出站流量 · 按程序汇总（公网发送量降序）",{left,px(top),right,px(top+28)}); top+=30;
        std::wstring status=traffic.status ? L"采集未就绪，错误码："+std::to_wstring(traffic.status) : L"采集中 · 首次进入本页后持续记录，退出程序停止";
        if(traffic.started) status+=L" · 开始于 "+timeText(traffic.started,true);
        if(traffic.lost) status+=L" · 存在丢失事件，统计不完整";
        label(dc,smallFont_,status,{left,px(top),right,px(top+24)},theme_->muted); top+=24;
        label(dc,smallFont_,L"TCP/UDP 发送事件字节；局域网按私网/链路本地地址判定；其他含回环/组播/保留地址",{left,px(top),right,px(top+24)},theme_->muted); top+=24;
        const auto table=[&](const wchar_t* title,const std::vector<TrafficTotal>& rows,bool live){
            label(dc,boldFont_,title,{left,px(top),right,px(top+28)});
            const int width=right-left, nameRight=left+width*40/100;
            const int col=(right-nameRight)/4;
            const wchar_t* titles[]={L"公网发送",L"局域网发送",L"其他发送",L"上传速度"};
            for(int i=0;i<4;++i) label(dc,smallFont_,titles[i],{nameRight+i*col,px(top),nameRight+(i+1)*col,px(top+28)},theme_->secondary,DT_RIGHT);
            top+=28;
            if(rows.empty()){label(dc,smallFont_,L"暂无已记录流量",{left,px(top),right,px(top+28)},theme_->muted);top+=28;}
            for(const auto& r:rows){
                auto name=r.path.substr(r.path.find_last_of(L"\\")+1);
                label(dc,normalFont_,name,{left,px(top),nameRight-px(12),px(top+28)});
                const std::wstring cells[]={bytes(static_cast<double>(r.internet),false),bytes(static_cast<double>(r.lan),false),bytes(static_cast<double>(r.other),false),live?bytes(r.rate,true):L"—"};
                for(int i=0;i<4;++i) label(dc,normalFont_,cells[i],{nameRight+i*col,px(top),nameRight+(i+1)*col,px(top+28)},theme_->ink,DT_RIGHT);
                top+=28;
            }
        };
        table(L"本次采集",traffic.session,true);
        table(L"今天",traffic.today,false);
        table(L"最近 7 天",traffic.week,false);
    }

    void paintCpuDetails(HDC dc, RECT area, const Snapshot* snapshot) const {
        const int left = px(24);
        const int right = area.right - left;
        label(dc, titleFont_, L"CPU 实时详情", {left, px(83), right, px(120)});
        paintDetailSummary(dc, {left, px(130), right, px(205)}, {
            {L"整机 CPU 占用率", decimal(value(snapshot, Metric::CpuPercent), 1, L"%")},
            {L"CPU 封装温度", decimal(value(snapshot, Metric::CpuTemp), 0, L" °C")},
            {L"CPU 封装功耗 · RAPL", decimal(value(snapshot, Metric::CpuWatts), 1, L" W")}
        });
        rounded(dc, {left, px(217), right, area.bottom - px(10)}, px(22), theme_->surface, theme_->border);
        label(dc, boldFont_, L"逻辑处理器", {left + px(12), px(221), right - px(12), px(249)});
        label(dc, smallFont_, L"功耗取自 CPU 能量计数，读数受平台遥测影响",
            {left + px(122), px(221), right - px(14), px(249)}, theme_->muted, DT_RIGHT);
        if (details_.cores.empty()) {
            label(dc, normalFont_, L"暂无逻辑处理器明细。", {left + px(12), px(256), right - px(12), px(286)}, theme_->muted);
        }
    }

    void paintDetailFields(HDC dc, RECT area,
        std::initializer_list<std::pair<const wchar_t*, std::wstring>> fields) const {
        const int middle = area.left + (area.right - area.left) / 2;
        const int rowHeight = px(22);
        int index = 0;
        for (const auto& [title, reading] : fields) {
            const int left = index % 2 == 0 ? area.left : middle;
            const int right = (index % 2 == 0 ? middle : area.right) - px(10);
            const int top = area.top + index / 2 * rowHeight;
            const int valueLeft = left + px(96);
            label(dc, smallFont_, title, {left, top, valueLeft - px(5), top + rowHeight}, theme_->secondary);
            label(dc, smallFont_, reading.empty() ? L"—" : reading,
                {valueLeft, top, right, top + rowHeight});
            ++index;
        }
    }

    void paintMemoryDetails(HDC dc, RECT area, const Snapshot* snapshot) const {
        const int left = px(PageInset);
        const int right = area.right - left;
        const auto total = value(snapshot, Metric::MemoryTotal);
        const auto used = value(snapshot, Metric::MemoryUsed);
        const auto installed = inventory_ ? inventory_->installedMemory : Missing;
        label(dc, titleFont_, L"内存实时详情", {left, px(83), right, px(120)});
        paintDetailSummary(dc, {left, px(125), right, px(196)}, {
            {L"内存占用率", decimal(value(snapshot, Metric::MemoryPercent), 1, L"%")},
            {L"已用 / 系统可见", bytes(used) + L" / " + bytes(total)},
            {L"可用内存", bytes(total - used)}
        });
        const auto& memory = details_.memory;
        rounded(dc, {left, px(206), right, px(318)}, px(14), theme_->surface, theme_->border);
        paintDetailFields(dc, {left + px(12), px(207), right - px(4), px(318)}, {
            {L"已安装容量", bytes(installed)}, {L"硬件保留（估算）", bytes(installed - total)},
            {L"已提交", bytes(memory.committed)}, {L"提交上限", bytes(memory.commitLimit)},
            {L"提交峰值", bytes(memory.commitPeak)}, {L"系统缓存", bytes(memory.cache)},
            {L"分页池", bytes(memory.pagedPool)}, {L"非分页池", bytes(memory.nonpagedPool)},
            {L"页面大小", bytes(memory.pageSize)}
        });

        if (memoryPlotExpanded_) {
            const int top = MemorySectionsBody;
            const auto end = nowMs();
            const auto start = end - MinuteMs;
            rounded(dc, {left, px(top), right, px(top + MemoryPlotHeight - 8)}, px(14), theme_->surface, theme_->border);
            label(dc, boldFont_, L"最近 60 秒 · 内存占用率", {left + px(12), px(top + 4), right - px(12), px(top + 28)});
            const RECT graph{left + px(55), px(top + 31), right - px(12), px(top + 148)};
            paintChart(dc, graph, chartFor(1), live_, start, end, theme_->cardColors[1]);
            const auto plot = chartPlot(graph);
            for (const auto& [caption, y] : {std::pair{L"100%", plot.top},
                {L"50%", (plot.top + plot.bottom) / 2}, {L"0%", plot.bottom}}) {
                label(dc, smallFont_, caption, {left + px(8), y - px(10), plot.left - px(5), y + px(10)},
                    theme_->secondary, DT_RIGHT);
            }
            label(dc, smallFont_, timeText(start), {plot.left, plot.bottom, plot.right, plot.bottom + px(22)}, theme_->secondary);
            label(dc, smallFont_, timeText(end), {plot.left, plot.bottom, plot.right, plot.bottom + px(22)}, theme_->secondary, DT_RIGHT);
        }
        if (memoryTimingExpanded_) {
            const int currentTop = memoryTimingTop();
            rounded(dc, {left, px(currentTop), right, px(currentTop + currentMemoryTimingHeight() - 8)},
                px(14), theme_->surface, theme_->border);
            const auto report = details_.memoryTimings;
            const auto title = std::wstring(L"自动采集 · 当前运行配置") +
                (report ? L" · " + std::wstring(readingStatus(report->status)) : L"");
            label(dc, boldFont_, title, {left + px(12), px(currentTop + 5), right - px(12), px(currentTop + 29)});
            label(dc, smallFont_, report ? report->platform : L"正在请求硬件采集助手…",
                {left + px(12), px(currentTop + 31), right - px(12), px(currentTop + 53)}, theme_->secondary);
            label(dc, smallFont_, report ? report->backend : L"",
                {left + px(12), px(currentTop + 54), right - px(12), px(currentTop + 76)}, theme_->secondary);
            paragraph(dc, smallFont_, report ? report->reason : L"底层采集在独立进程执行，超时 5 秒。",
                {left + px(12), px(currentTop + 78), right - px(12), px(currentTop + 128)}, theme_->secondary);
            const int top = currentTop + currentMemoryTimingHeight();
            rounded(dc, {left, px(top), right, px(top + MemoryTimingHeight - 8)}, px(14), theme_->surface, theme_->border);
            label(dc, boldFont_, L"历史报告 · 备用对照", {left + px(12), px(top + 6), right - px(100), px(top + 30)});
            const auto timing = [&](size_t index) { return memoryTiming_ ? std::to_wstring((*memoryTiming_)[index]) : L"—"; };
            paintDetailFields(dc, {left + px(12), px(top + 31), right - px(4), px(top + 75)}, {
                {L"CL / tCAS", timing(0)}, {L"tRCD", timing(1)}, {L"tRP", timing(2)}, {L"tRAS", timing(3)}
            });
            label(dc, smallFont_, memoryTimingSource_,
                {left + px(12), px(top + 75), right - px(12), px(top + 95)}, theme_->secondary);
        }

        int top = memoryDevicesTop();
        if (!inventory_ || inventory_->memoryDevices.empty()) {
            label(dc, normalFont_, inventory_ ? L"固件未提供内存设备信息。" : L"正在读取内存设备…",
                {left, px(top), right, px(top + 30)}, theme_->secondary);
            return;
        }
        for (const auto& device : inventory_->memoryDevices) {
            const int height = device.empty ? EmptyMemorySlotHeight : MemoryDeviceHeight;
            rounded(dc, {left, px(top), right, px(top + height - 8)}, px(14), theme_->surface, theme_->border);
            const auto title = device.slot + (device.empty ? L" · 空插槽" : L"");
            label(dc, boldFont_, title, {left + px(12), px(top + 4), right - px(12), px(top + 29)});
            const auto speedUnit = device.legacySpeed ? L"（固件原值）" : L" MT/s";
            if (device.empty) {
                paintDetailFields(dc, {left + px(12), px(top + 24), right - px(4), px(top + height)},
                    {{L"存储体", device.bank}});
            } else {
                paintDetailFields(dc, {left + px(12), px(top + 30), right - px(4), px(top + height)}, {
                    {L"容量", bytes(device.capacity)}, {L"存储体", device.bank},
                    {L"制造商", device.manufacturer}, {L"部件型号", device.partNumber},
                    {L"序列号", device.serialNumber}, {L"资产标签", device.assetTag},
                    {L"内存类型", device.type}, {L"封装形态", device.formFactor},
                    {L"标称速率", decimal(device.ratedSpeed, 0, speedUnit)},
                    {L"配置速率", decimal(device.configuredSpeed, 0, speedUnit)},
                    {L"数据位宽", decimal(device.dataWidth, 0, L" bit")},
                    {L"总位宽", decimal(device.totalWidth, 0, L" bit")},
                    {L"Rank 数量", decimal(device.ranks, 0, L"")},
                    {L"配置电压", decimal(device.configuredVolts, 3, L" V")},
                    {L"最低电压", decimal(device.minVolts, 3, L" V")},
                    {L"最高电压", decimal(device.maxVolts, 3, L" V")}
                });
            }
            top += height;
        }
    }

    void paintGpuDetails(HDC dc, RECT area, const Snapshot* snapshot) const {
        const int left = px(PageInset);
        const int right = area.right - left;
        label(dc, titleFont_, L"显卡实时详情", {left, px(83), right, px(113)});
        if (inventory_ && !inventory_->sampledGpu.empty()) {
            label(dc, smallFont_, inventory_->sampledGpu, {left, px(113), right, px(135)}, theme_->secondary);
        }
        paintDetailSummary(dc, {left, px(138), right, px(213)}, {
            {L"GPU 占用率", decimal(value(snapshot, Metric::GpuPercent), 1, L"%")},
            {L"GPU 温度", decimal(value(snapshot, Metric::GpuTemp), 0, L" °C")},
            {L"板卡功耗", decimal(value(snapshot, Metric::GpuWatts), 1, L" W")}
        });
        const auto section = [&](int top, int bottom, const wchar_t* title,
            std::initializer_list<std::pair<const wchar_t*, std::wstring>> fields) {
            rounded(dc, {left, px(top), right, px(bottom)}, px(14), theme_->surface, theme_->border);
            label(dc, boldFont_, title, {left + px(12), px(top + 4), right - px(12), px(top + 30)});
            paintDetailFields(dc, {left + px(12), px(top + 31), right - px(4), px(bottom)}, fields);
        };
        const GpuSpecifications empty;
        const auto& specs = inventory_ ? inventory_->gpu : empty;
        const auto generation = [](double value) -> std::wstring {
            return std::isfinite(value) ? L"Gen " + decimal(value, 0, L"") : L"—";
        };
        section(225, 225 + GpuSpecificationsHeight - 12, L"硬件规格与板卡信息", {
            {L"CUDA 核心数", decimal(specs.cores, 0, L"")},
            {L"GPU 架构", specs.architecture},
            {L"显存容量", bytes(specs.memoryCapacity)},
            {L"显存位宽", decimal(specs.memoryBusWidth, 0, L" bit")},
            {L"CUDA 计算能力", specs.cudaCapability},
            {L"默认功耗上限", decimal(specs.defaultPowerLimitWatts, 1, L" W")},
            {L"显卡 PCIe 上限", generation(specs.maxPcieGeneration)},
            {L"本机 PCIe 上限", generation(specs.hostMaxPcieGeneration)},
            {L"本机最大通道数", decimal(specs.hostMaxPcieWidth, 0, L"")},
            {L"VBIOS 版本", specs.vbios},
            {L"板卡料号", specs.boardPartNumber},
            {L"板卡序列号", specs.serial}
        });
        section(225 + GpuSpecificationsHeight, 328 + GpuSpecificationsHeight, L"显存与视频", {
            {L"显存已用 / 总量", bytes(value(snapshot, Metric::GpuMemoryUsed)) + L" / " + bytes(value(snapshot, Metric::GpuMemoryTotal))},
            {L"显存读写活跃度", decimal(details_.gpuMemoryActivity, 0, L"%")},
            {L"视频编码占用率", decimal(details_.gpuEncoderPercent, 0, L"%")},
            {L"视频解码占用率", decimal(details_.gpuDecoderPercent, 0, L"%")}
        });
        section(340 + GpuSpecificationsHeight, 443 + GpuSpecificationsHeight, L"频率与散热", {
            {L"核心频率", decimal(details_.gpuCoreMhz, 0, L" MHz")},
            {L"显存频率", decimal(details_.gpuMemoryMhz, 0, L" MHz")},
            {L"风扇设定转速", decimal(details_.gpuFanRpm, 0, L" RPM")},
            {L"风扇设定比例", decimal(details_.gpuFanPercent, 0, L"%")},
            {L"热点温度", decimal(details_.gpuHotspotC, 0, L" °C")},
            {L"温度限制", details_.gpuLimitsAvailable ? (details_.gpuThermalLimited ? L"已触发" : L"未触发") : L"—"}
        });
        section(455 + GpuSpecificationsHeight, GpuPageHeight - PageInset, L"功耗与当前 PCIe 链路", {
            {L"生效功耗上限", decimal(details_.gpuPowerLimitWatts, 1, L" W")},
            {L"功耗限制", details_.gpuLimitsAvailable ? (details_.gpuPowerLimited ? L"已触发" : L"未触发") : L"—"},
            {L"性能状态", std::isfinite(details_.gpuPerformanceState)
                ? L"P" + decimal(details_.gpuPerformanceState, 0, L"") : L"—"},
            {L"PCIe 代际", decimal(details_.gpuPcieGeneration, 0, L"")},
            {L"PCIe 通道数", decimal(details_.gpuPcieWidth, 0, L"")}
        });
    }

    void paintDiskDetails(HDC dc, RECT area) const {
        area.bottom = std::max(area.bottom, static_cast<LONG>(px(DiskPageHeight)));
        const int left = px(24);
        const int right = area.right - left;
        label(dc, titleFont_, L"磁盘 SMART", {left, px(83), right, px(120)});
        label(dc, smallFont_, L"每行对应一块物理磁盘。损耗为设备报告的寿命消耗比例，横线表示暂无读数。",
            {left, px(120), right, px(149)}, theme_->secondary);
        rounded(dc, {left, px(149), right, area.bottom - px(10)}, px(22), theme_->surface, theme_->border);
        if (details_.disks.empty()) {
            const auto text = details_.pane == DetailPane::Disk ? L"暂无可用磁盘数据。" : L"正在读取磁盘可靠性数据…";
            label(dc, normalFont_, text, {left + px(12), px(160), right - px(12), px(190)}, theme_->muted);
            if (callbacks_.startCpu) {
                label(dc, smallFont_, L"如果一直没有读数，可从托盘菜单启动硬件传感器。",
                    {left + px(12), px(191), right - px(12), px(216)}, theme_->secondary);
            }
            return;
        }
        const auto& disk = *selectedSmartDisk();
        const int noteTop = px(DiskSummaryTop + DiskSummaryHeight + 10);
        label(dc, boldFont_, L"磁盘 " + std::to_wstring(disk.number) + L" · " + disk.name,
            {left + px(12), noteTop, right - px(12), noteTop + px(24)});
        const auto note = disk.smartLog.format == SmartLogFormat::Ata
            ? (disk.smartLog.hasThresholds ? L"读数依次为当前值 / 最差值 / 阈值。原始值按十六进制显示。"
                                          : L"读数依次为当前值 / 最差值 / 阈值。设备或驱动未返回阈值。")
            : disk.smartLog.format == SmartLogFormat::Nvme
                ? L"NVMe 健康日志。主机累计读写按每数据单位 512,000 字节估算。"
                : L"暂无详细 SMART 读数。";
        label(dc, smallFont_, note, {left + px(12), noteTop + px(24), right - px(12), noteTop + px(46)}, theme_->secondary);
        label(dc, smallFont_, disk.note, {left + px(12), noteTop + px(44), right - px(12), noteTop + px(66)}, theme_->muted);
    }

    void paintGamutPlot(HDC dc, int left, int top, const DisplayGamut& gamut) const {
        const int axis = px(238);
        const int plotLeft = left + px(32);
        const int plotBottom = top + px(252);
        const auto toPoint = [&](Chromaticity xy) -> POINT {
            return {plotLeft + std::lround(xy.x * axis), plotBottom - std::lround(xy.y * axis)};
        };
        for (int tick = 0; tick <= 5; ++tick) {
            const double value = tick / 5.0;
            const auto point = toPoint({value, value});
            line(dc, point.x, plotBottom - axis, point.x, plotBottom, theme_->grid);
            line(dc, plotLeft, point.y, plotLeft + axis, point.y, theme_->grid);
            const auto text = decimal(value, 1, L"");
            label(dc, smallFont_, text, {point.x - px(13), plotBottom + px(3), point.x + px(13),
                plotBottom + px(23)}, theme_->secondary, DT_CENTER);
            if (tick > 0) {
                label(dc, smallFont_, text, {left, point.y - px(10), plotLeft - px(6), point.y + px(10)},
                    theme_->secondary, DT_RIGHT);
            }
        }
        line(dc, plotLeft, plotBottom, plotLeft + axis, plotBottom, theme_->secondary);
        line(dc, plotLeft, plotBottom - axis, plotLeft, plotBottom, theme_->secondary);
        label(dc, smallFont_, L"x", {plotLeft + axis + px(16), plotBottom, plotLeft + axis + px(32),
            plotBottom + px(24)}, theme_->secondary);
        label(dc, smallFont_, L"y", {plotLeft - px(4), top - px(4), plotLeft + px(14), top + px(14)}, theme_->secondary);

        const int legend = plotLeft + axis + px(24);
        const auto drawTriangle = [&](const auto& primaries, int style, int width, COLORREF color, int legendY) {
            const auto pen = CreatePen(style, width, color);
            if (!pen) return;
            const auto previous = SelectObject(dc, pen);
            const auto previousMode = SetBkMode(dc, TRANSPARENT);
            const std::array points{toPoint(primaries[0]), toPoint(primaries[1]),
                toPoint(primaries[2]), toPoint(primaries[0])};
            Polyline(dc, points.data(), static_cast<int>(points.size()));
            MoveToEx(dc, legend, legendY, nullptr);
            LineTo(dc, legend + px(23), legendY);
            SetBkMode(dc, previousMode);
            SelectObject(dc, previous);
            DeleteObject(pen);
        };
        drawTriangle(gamut.primaries, PS_SOLID, px(2), theme_->blue, top + px(32));
        drawTriangle(SrgbPrimaries, PS_DASH, 1, theme_->secondary, top + px(56));
        label(dc, smallFont_, L"显示器", {legend + px(30), top + px(22), legend + px(90), top + px(42)});
        label(dc, smallFont_, L"sRGB", {legend + px(30), top + px(46), legend + px(90), top + px(66)});
        constexpr const wchar_t* names[]{L"R", L"G", L"B"};
        for (size_t i = 0; i < gamut.primaries.size(); ++i) {
            const auto point = toPoint(gamut.primaries[i]);
            rounded(dc, {point.x - px(3), point.y - px(3), point.x + px(4), point.y + px(4)},
                px(7), theme_->blue, theme_->surface);
            label(dc, smallFont_, names[i], {point.x + px(6), point.y - px(17), point.x + px(24), point.y + px(3)});
        }
    }

    void paintBoard(HDC dc, RECT area) const {
        const int pad = px(PageInset);
        const int right = area.right - pad;
        label(dc, titleFont_, L"主板信息", {pad, px(83), right, px(120)});
        if (!inventory_ || inventory_->board.empty()) {
            label(dc, normalFont_, inventory_ ? L"无法读取 SMBIOS 主板清单。" : L"等待读取硬件清单。",
                {pad, px(124), right, px(156)}, theme_->muted);
            return;
        }
        int top = 124;
        for (const auto& section : inventory_->board) {
            const int bottom = top + 48 + 23 * static_cast<int>(std::max<size_t>(1, section.fields.size()));
            const int left = pad + px(14);
            const int valueLeft = left + (right - left) * 44 / 100;
            rounded(dc, {pad, px(top), right, px(bottom - 8)}, px(14), theme_->surface, theme_->border);
            label(dc, boldFont_, section.title, {left, px(top + 7), right - px(14), px(top + 34)});
            int y = top + 34;
            if (section.fields.empty()) {
                label(dc, smallFont_, L"固件未提供记录", {left, px(y), right - px(14), px(y + 23)}, theme_->muted);
            }
            for (const auto& [name, reading] : section.fields) {
                label(dc, smallFont_, name, {left, px(y), valueLeft - px(8), px(y + 23)}, theme_->secondary);
                label(dc, normalFont_, reading.empty() ? L"—" : reading,
                    {valueLeft, px(y), right - px(14), px(y + 23)});
                y += 23;
            }
            top = bottom;
        }
    }

    void paintDisplays(HDC dc, RECT area) const {
        const int pad = px(PageInset);
        const int right = area.right - pad;
        const int bottom = px(displayPageHeight());
        label(dc, titleFont_, L"当前启用的显示器", {pad, px(83), right, px(120)});
        if (!displays_ || displays_->empty()) {
            const auto text = displays_ ? L"当前没有启用的显示器。" : L"无法读取当前显示配置。";
            rounded(dc, {pad, px(124), right, bottom - pad}, px(22), theme_->surface, theme_->border);
            label(dc, normalFont_, text, {pad + px(12), px(132), right - px(12), px(162)}, theme_->muted);
            return;
        }

        const int top = px(DisplayGamutTop);
        rounded(dc, {pad, px(124), right, top - px(6)}, px(22), theme_->surface, theme_->border);
        rounded(dc, {pad, top, right, bottom - pad}, px(22), theme_->surface, theme_->border);
        const int textLeft = pad + px(14);
        const int textRight = right - px(14);
        const auto display = selectedDisplay();
        if (!display) {
            label(dc, boldFont_, L"色域", {textLeft, top + px(9), textRight, top + px(31)});
            label(dc, normalFont_, L"选择一台显示器查看色域。", {textLeft, top + px(37), textRight, top + px(65)},
                theme_->secondary);
            return;
        }

        const auto name = display->name.empty() ? L"所选显示器" : display->name;
        label(dc, boldFont_, name + L" · 色域", {textLeft, top + px(9), textRight, top + px(31)});
        if (!display->gamut) {
            label(dc, normalFont_, L"暂无可用的 EDID 色域数据。", {textLeft, top + px(37), textRight, top + px(65)},
                theme_->secondary);
            return;
        }
        const auto& gamut = *display->gamut;
        const auto percentages = L"sRGB " + decimal(gamut.srgb, 1, L"%") +
            L"     Display P3 " + decimal(gamut.displayP3, 1, L"%") +
            L"     Adobe RGB " + decimal(gamut.adobeRgb, 1, L"%");
        label(dc, normalFont_, percentages, {textLeft, top + px(32), textRight, top + px(56)});
        const auto xy = [](Chromaticity point) {
            return L"(" + decimal(point.x, 3, L"") + L", " + decimal(point.y, 3, L"") + L")";
        };
        const auto coordinates = L"R " + xy(gamut.primaries[0]) + L"     G " + xy(gamut.primaries[1]) +
            L"     B " + xy(gamut.primaries[2]);
        label(dc, smallFont_, coordinates, {textLeft, top + px(55), textRight, top + px(76)}, theme_->secondary);
        label(dc, smallFont_, L"基于 EDID 的 CIE 1931 xy 覆盖率估算",
            {textLeft, top + px(76), textRight, top + px(98)}, theme_->secondary);
        paintGamutPlot(dc, textLeft, top + px(98), gamut);
    }

    void render(HDC dc, RECT area) const {
        for (auto& tip : rankTips_) tip = {};
        modelTips_.clear();
        fill(dc, {0, scroll_, area.right, area.bottom + scroll_}, theme_->background);
        const int pad = px(PageInset);
        line(dc, 0, px(60), area.right, px(60), theme_->border);
        DrawIconEx(dc, pad, px(18), appIcon_, px(28), px(28), 0, nullptr, DI_NORMAL);
        label(dc, brandFont_, L"Computer Watch", {pad + px(34), px(14), pad + px(204), px(50)});
        if (tab_ == Tab::Settings) {
            paintSettings(dc, area);
            return;
        }
        if (tab_ == Tab::Displays) {
            paintDisplays(dc, area);
            return;
        }
        if (tab_ == Tab::Board) {
            paintBoard(dc, area);
            return;
        }

        const bool history = tab_ == Tab::History;
        const auto& samples = history ? history_ : live_;
        const auto now = nowMs();
        const auto start = history ? historyStart_ : now - MinuteMs;
        const auto end = history ? historyEnd_ : now;
        const auto snapshot = history ? (selected_ ? &*selected_ : nullptr) : (live_.empty() ? nullptr : &live_.back());
        std::wstring status;
        COLORREF statusColor = history ? theme_->blue : theme_->green;
        const int extra = statusOffset(area);
        if (history) {
            rounded(dc, {pad, px(73) + extra, area.right - pad, px(110) + extra}, px(12), theme_->surface, theme_->border);
            label(dc, smallFont_, L"查询时间", {pad + px(12), px(78) + extra, pad + px(64), px(105) + extra}, theme_->secondary);
            label(dc, smallFont_, L"点击任意曲线定位采样时刻",
                {pad + px(465), px(78) + extra, area.right - pad - px(12), px(105) + extra}, theme_->secondary);
            status = historyFailed_ ? L"读取历史失败" :
                snapshot ? L"历史样本  " + timeText(snapshot->timestampMs, true) : L"该时间附近暂无记录";
        } else if (liveFailed_) {
            status = L"读取采集数据失败";
            statusColor = theme_->amber;
        } else if (!snapshot) {
            status = L"等待采集器";
            statusColor = theme_->amber;
        } else {
            status = L"最新样本  " + timeText(snapshot->timestampMs);
            if (now - snapshot->timestampMs > 12'000) {
                status += L"  ·  采集已暂停";
                statusColor = theme_->amber;
            }
        }
        const int badgeRight = area.right - pad;
        const int badgeLeft = extra ? pad : badgeRight - textWidth(dc, smallFont_, status) - px(15);
        const int statusY = px(extra ? 75 : 32);
        rounded(dc, {badgeLeft, statusY - px(3), badgeLeft + px(6), statusY + px(3)},
            px(6), statusColor, statusColor);
        label(dc, smallFont_, status, {badgeLeft + px(14), statusY - px(13), badgeRight,
            statusY + px(13)}, theme_->secondary);

        area.bottom = std::max(static_cast<int>(area.bottom), px(390));
        switch (detailPane()) {
        case DetailPane::Cpu: paintCpuDetails(dc, area, snapshot); return;
        case DetailPane::Memory: paintMemoryDetails(dc, area, snapshot); return;
        case DetailPane::Gpu: paintGpuDetails(dc, area, snapshot); return;
        case DetailPane::Disk: paintDiskDetails(dc, area); return;
        case DetailPane::Network: paintNetworkDetails(dc, area, snapshot); return;
        case DetailPane::None: break;
        }

        for (size_t index = 0; index < cards_.size(); ++index) {
            paintCard(dc, index, snapshot, samples, start, end);
        }
        const int y = cards_.back().bottom + px(30);
        label(dc, smallFont_, history ? timeText(start, true) : L"最近 60 秒",
            {pad, cards_.back().bottom + 1, area.right / 2, y - 1}, theme_->muted);
        label(dc, smallFont_, history ? timeText(end, true) : L"当前",
            {area.right / 2, cards_.back().bottom + 1, area.right - pad, y - 1}, theme_->muted, DT_RIGHT);
        paintRanks(dc, rankPanel(area, 0), L"CPU 占用前 10", snapshot ? &snapshot->byCpu : nullptr, rankSorts_[0], 0);
        paintRanks(dc, rankPanel(area, 1), L"内存占用前 10", snapshot ? &snapshot->byMemory : nullptr, rankSorts_[1], 1);
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
                SetViewportOrgEx(buffer, 0, -scroll_, nullptr);
                render(buffer, area);
                SetViewportOrgEx(buffer, 0, 0, nullptr);
                BitBlt(dc, 0, 0, area.right, area.bottom, buffer, 0, 0, SRCCOPY);
                SelectObject(buffer, old);
            } else {
                const int saved = SaveDC(dc);
                SetViewportOrgEx(dc, 0, -scroll_, nullptr);
                render(dc, area);
                RestoreDC(dc, saved);
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
    HICON appIcon_{}, trayIcon_{};
    UiCallbacks callbacks_;
    HWND window_{}, liveButton_{}, historyButton_{}, settingsButton_{}, cpuCombo_{}, gpuCombo_{};
    HWND displaysButton_{}, boardButton_{}, displayList_{}, refreshDisplaysButton_{};
    HWND picker_{}, rangeCombo_{}, queryButton_{}, retentionEdit_{}, saveButton_{};
    HWND lightThemeButton_{}, darkThemeButton_{};
    HWND copyMcpButton_{};
    HWND cpuDetailButton_{}, memoryDetailButton_{}, gpuDetailButton_{}, diskDetailButton_{}, detailBackButton_{};
    HWND networkDetailButton_{};
    HWND memoryPlotButton_{}, memoryTimingButton_{}, importMemoryTimingButton_{};
    HWND memoryTimingList_{};
    bool memoryPlotExpanded_{}, memoryTimingExpanded_ = true;
    std::optional<std::array<unsigned, 4>> memoryTiming_;
    std::wstring memoryTimingSource_ = L"导入本机 HWiNFO HTML 报告以查看时序。";
    HWND coreList_{}, diskList_{}, diskSmartList_{}, rankTooltip_{};
    size_t diskSmartRowCount_{};
    struct RankTip { RECT bounds{}; std::wstring text; };
    mutable std::array<RankTip, 2 * TopCount> rankTips_{};
    struct ModelTip { RECT bounds; const std::wstring* text; };
    mutable std::vector<ModelTip> modelTips_;
    std::optional<HardwareInventory> inventory_;
    std::optional<std::vector<DisplayInfo>> displays_;
    POINT cursor_{-1, -1};
    std::wstring tooltipText_;
    std::array<HWND, 4> sortButtons_{};
    HFONT normalFont_{}, smallFont_{}, brandFont_{}, titleFont_{}, valueFont_{}, compactValueFont_{}, boldFont_{};
    HANDLE readingFont_{};
    HBRUSH surfaceBrush_{};
    bool darkMode_{};
    const ui::Palette* theme_ = &ui::LightPalette;
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_{};
    bool trayAdded_{}, trayV4_{};
    int dpi_ = 96;
    int scroll_{}, scrollMax_{}, wheelDelta_{};
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
    std::wstring mcpMessage_;
    std::wstring appearanceMessage_;
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
