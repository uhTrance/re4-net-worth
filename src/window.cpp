// RE4 Net Worth - the in-game window and the on-screen widget.
//
// Both are drawn into REFramework's own ImGui context, handed over every frame
// by on_imgui_frame. ImGui is compiled into this DLL from the same commit and
// with the same options REFramework uses (see imgui_config.hpp), so the
// context's structs line up. A REFramework update that moves ImGui on can
// break that; updating this plugin's ImGui to match fixes it.
//
// The window is the same Case File look as the RE4 Charm Predictor: dark ink
// on cream paper, or cream on dark card in dark mode, with the same sizing
// controls. The widget is the small glass panel from the stream overlay: the
// total in gold, rolling to each new figure, with the change shown under it.

#include "window.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include "imgui.h"

#include "re4_common.hpp"

namespace re4 {
namespace {

constexpr const char* VERSION = "1.0.0";

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------
std::mutex g_state_mutex;
std::shared_ptr<const OutputState> g_state;   // latest published snapshot
std::atomic<bool> g_paused{false};
std::atomic<bool> g_dark_flag{true};
std::atomic<bool> g_export_flag{true};

std::string g_data_dir;
std::string g_settings_path;
std::string g_page_url;   // file:/// link to the browser view
std::string g_font_path;

// ---------------------------------------------------------------------------
// Settings, saved as a small flat JSON file next to net_worth.json
// ---------------------------------------------------------------------------
struct Settings {
    bool show_window = true;
    bool show_widget = true;
    bool dark = true;
    bool browser_export = true;
    bool scale_auto = true;
    float scale = 1.0f;          // window size multiplier (the Size slider)
    float widget_scale = 1.0f;   // the widget's own size
    float win_w = 0.0f;          // window size as a share of the screen, 0 = unset
    float win_h = 0.0f;
    float widget_x = 0.5f;       // widget centre-top as a share of the screen
    float widget_y = 0.02f;
};
Settings g_cfg;

// Fixed-point, and the same in every locale (snprintf's %f would write a
// comma under a comma-decimal C locale and break the file).
std::string num(float v, int digits) {
    char buf[32];
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, digits);
    return std::string(buf, r.ptr);
}

void save_settings() {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string out = "{\n";
    out += std::string("  \"show_window\": ") + b(g_cfg.show_window) + ",\n";
    out += std::string("  \"show_widget\": ") + b(g_cfg.show_widget) + ",\n";
    out += std::string("  \"dark\": ") + b(g_cfg.dark) + ",\n";
    out += std::string("  \"browser_export\": ") + b(g_cfg.browser_export) + ",\n";
    out += std::string("  \"scale_auto\": ") + b(g_cfg.scale_auto) + ",\n";
    out += "  \"scale\": " + num(g_cfg.scale, 2) + ",\n";
    out += "  \"widget_scale\": " + num(g_cfg.widget_scale, 2) + ",\n";
    out += "  \"win_w\": " + num(g_cfg.win_w, 3) + ",\n";
    out += "  \"win_h\": " + num(g_cfg.win_h, 3) + ",\n";
    out += "  \"widget_x\": " + num(g_cfg.widget_x, 4) + ",\n";
    out += "  \"widget_y\": " + num(g_cfg.widget_y, 4) + "\n}\n";
    write_file_atomic(g_settings_path, out);
    g_dark_flag = g_cfg.dark;
    g_export_flag = g_cfg.browser_export;
}

// Just enough JSON for the file save_settings() writes: "key": value pairs.
bool find_value(const std::string& src, const char* key, std::string& out) {
    const auto k = std::string("\"") + key + "\"";
    auto p = src.find(k);
    if (p == std::string::npos) return false;
    p = src.find(':', p + k.size());
    if (p == std::string::npos) return false;
    ++p;
    while (p < src.size() && (src[p] == ' ' || src[p] == '\t')) ++p;
    auto e = p;
    while (e < src.size() && src[e] != ',' && src[e] != '\n' && src[e] != '}') ++e;
    out = src.substr(p, e - p);
    while (!out.empty() && (out.back() == ' ' || out.back() == '\r')) out.pop_back();
    return !out.empty();
}

void load_settings() {
    const auto src = read_file(g_settings_path);
    if (!src.empty()) {
        std::string v;
        const auto get_bool = [&](const char* k, bool& dst) { if (find_value(src, k, v)) dst = (v == "true"); };
        const auto get_float = [&](const char* k, float& dst, float lo, float hi) {
            if (!find_value(src, k, v)) return;
            float f = 0.0f;
            if (std::from_chars(v.data(), v.data() + v.size(), f).ec != std::errc{}) return;
            if (std::isfinite(f)) dst = std::clamp(f, lo, hi);
        };
        get_bool("show_window", g_cfg.show_window);
        get_bool("show_widget", g_cfg.show_widget);
        get_bool("dark", g_cfg.dark);
        get_bool("browser_export", g_cfg.browser_export);
        get_bool("scale_auto", g_cfg.scale_auto);
        get_float("scale", g_cfg.scale, 0.75f, 3.0f);
        get_float("widget_scale", g_cfg.widget_scale, 0.5f, 3.0f);
        get_float("win_w", g_cfg.win_w, 0.0f, 1.0f);
        get_float("win_h", g_cfg.win_h, 0.0f, 1.0f);
        get_float("widget_x", g_cfg.widget_x, 0.0f, 1.0f);
        get_float("widget_y", g_cfg.widget_y, 0.0f, 1.0f);
    }
    g_dark_flag = g_cfg.dark;
    g_export_flag = g_cfg.browser_export;
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// The game folder, from the executable's own path.
std::wstring game_dir_w() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d(exe);
    const auto slash = d.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : d.substr(0, slash);
}

// file:/// URL with / separators, spaces and anything non-ASCII percent-encoded.
std::string file_url(const std::string& utf8_path) {
    std::string out = "file:///";
    static const char* HEX = "0123456789ABCDEF";
    for (unsigned char c : utf8_path) {
        if (c == '\\') out += '/';
        else if (c == ' ' || c >= 0x80 || c == '#' || c == '%' || c == '?') {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 15];
        } else {
            out += (char)c;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------
struct Palette {
    uint32_t paper, paper_line, paper_lt, edge, tab, ink, ink2, dim, red, green, amber, hover;
};
// 0xRRGGBB, the same values as the browser page and the Charm Predictor.
const Palette LIGHT = {0xefe7d4, 0xe3d8bf, 0xfaf4e6, 0xc4b590, 0xd8ccb0, 0x28231d, 0x4a4238,
                       0x8a7d66, 0x962218, 0x3d6b2e, 0x9a6b14, 0xffffff};
const Palette DARK = {0x1f1c17, 0x29251f, 0x2a261f, 0x5c513e, 0x2e2a22, 0xece3cf, 0xcfc4ad,
                      0x9a8d74, 0xe2574a, 0x7cc06a, 0xd9a33f, 0x3a342a};

const Palette& pal() { return g_cfg.dark ? DARK : LIGHT; }

ImU32 col(uint32_t hex, int a = 255) {
    return IM_COL32((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF, a);
}
ImVec4 vec(uint32_t hex, float a = 1.0f) {
    return ImVec4(((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, a);
}

// Every size in the window goes through S(), so the Size slider scales it all.
float S(float x) { return x * g_cfg.scale; }

ImFont* g_font = nullptr;      // Courier Prime Bold, or null to use REFramework's
ImFont* g_sys_font = nullptr;  // REFramework's own font, for non-Latin names
bool g_font_tried = false;

// Automatic size by screen height: 1080p 1.15x, 1440p 1.25x, 2160p 2.0x,
// straight lines between those, proportional outside them.
float auto_scale(float h) {
    static const float P[3][2] = {{1080, 1.15f}, {1440, 1.25f}, {2160, 2.0f}};
    float s;
    if (h <= P[0][0]) s = P[0][1] * h / P[0][0];
    else if (h >= P[2][0]) s = P[2][1] * h / P[2][0];
    else {
        const int i = h <= P[1][0] ? 0 : 1;
        s = P[i][1] + (P[i + 1][1] - P[i][1]) * (h - P[i][0]) / (P[i + 1][0] - P[i][0]);
    }
    s = std::floor(s * 20.0f + 0.5f) / 20.0f;
    return std::clamp(s, 0.75f, 3.0f);
}

int g_pushed_colors = 0;
int g_pushed_vars = 0;

void push_theme() {
    const auto& p = pal();
    const std::pair<ImGuiCol, uint32_t> colors[] = {
        {ImGuiCol_Text, p.ink}, {ImGuiCol_TextDisabled, p.dim}, {ImGuiCol_WindowBg, p.paper},
        {ImGuiCol_PopupBg, p.paper_lt}, {ImGuiCol_Border, p.edge}, {ImGuiCol_FrameBg, p.paper_lt},
        {ImGuiCol_FrameBgHovered, p.hover}, {ImGuiCol_FrameBgActive, p.hover},
        {ImGuiCol_TitleBg, p.tab}, {ImGuiCol_TitleBgActive, p.edge}, {ImGuiCol_TitleBgCollapsed, p.tab},
        {ImGuiCol_ScrollbarBg, p.paper}, {ImGuiCol_ScrollbarGrab, p.edge},
        {ImGuiCol_ScrollbarGrabHovered, p.dim}, {ImGuiCol_ScrollbarGrabActive, p.ink2},
        {ImGuiCol_CheckMark, p.red}, {ImGuiCol_SliderGrab, p.ink}, {ImGuiCol_SliderGrabActive, p.red},
        {ImGuiCol_Button, p.paper_lt}, {ImGuiCol_ButtonHovered, p.hover}, {ImGuiCol_ButtonActive, p.edge},
        {ImGuiCol_Header, p.paper_lt}, {ImGuiCol_HeaderHovered, p.hover}, {ImGuiCol_HeaderActive, p.edge},
        {ImGuiCol_Separator, p.edge}, {ImGuiCol_TableHeaderBg, p.tab},
        {ImGuiCol_TableBorderStrong, p.edge}, {ImGuiCol_TableBorderLight, p.paper_line},
        {ImGuiCol_TableRowBg, p.paper_lt}, {ImGuiCol_TableRowBgAlt, p.paper},
    };
    for (const auto& [idx, hex] : colors) ImGui::PushStyleColor(idx, vec(hex));
    g_pushed_colors = (int)(sizeof(colors) / sizeof(colors[0]));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(18), S(14)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(3));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, S(1.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(4), S(3)));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(S(6), S(3)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(14));
    g_pushed_vars = 8;

    ImGui::PushFont(g_font, S(16));   // null font = keep the current one, at this size
}

void pop_theme() {
    ImGui::PopFont();
    ImGui::PopStyleVar(g_pushed_vars);
    ImGui::PopStyleColor(g_pushed_colors);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
std::string fmt(double v) {
    long long n = (long long)std::llround(v);
    const bool neg = n < 0;
    if (neg) n = -n;
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return neg ? "-" + s : s;
}

// Anything outside Latin-1 has a UTF-8 lead byte of 0xC4 or above; Courier
// Prime has no glyphs for those, REFramework's own font does.
bool needs_system_font(const std::string& s) {
    for (unsigned char c : s) if (c >= 0xC4 && c <= 0xF4) return true;
    return false;
}

void text(uint32_t hex, const std::string& s) {
    const bool sys = g_font != nullptr && g_sys_font != nullptr && needs_system_font(s);
    if (sys) ImGui::PushFont(g_sys_font, S(16));
    ImGui::PushStyleColor(ImGuiCol_Text, vec(hex));
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopStyleColor();
    if (sys) ImGui::PopFont();
}

void kicker(const char* s) {
    ImGui::Spacing();
    std::string up(s);
    for (auto& c : up) c = (char)std::toupper((unsigned char)c);
    text(pal().red, up);
}

// Right-aligned within the current table cell.
void num_cell(const std::string& s, uint32_t hex) {
    const float w = ImGui::CalcTextSize(s.c_str()).x;
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
    text(hex, s);
}

// "Mixed Herb (G+R)" -> "G+R", "Green Herb" -> "Green", for the plan's steps.
std::string short_herb(const std::string& name) {
    const std::string pre = "Mixed Herb (";
    if (name.rfind(pre, 0) == 0 && name.size() > pre.size() + 1 && name.back() == ')') {
        return name.substr(pre.size(), name.size() - pre.size() - 1);
    }
    const std::string suf = " Herb";
    if (name.size() > suf.size() && name.compare(name.size() - suf.size(), suf.size(), suf) == 0) {
        return name.substr(0, name.size() - suf.size());
    }
    return name;
}

// The game's own name for an English herb name from net_worth.json.
std::string herb_local(const OutputState& st, const std::string& english, bool short_form) {
    for (int i = 0; i < HERB_KINDS; ++i) {
        if (st.herb_english[i] == english) {
            const auto& local = st.herb_local[i];
            if (local.empty() || local == english) return short_form ? short_herb(english) : english;
            return local;
        }
    }
    return short_form ? short_herb(english) : english;
}

std::vector<std::string> split_plus(const std::string& s) {
    std::vector<std::string> out;
    size_t p = 0;
    for (;;) {
        const auto q = s.find(" + ", p);
        out.push_back(s.substr(p, q == std::string::npos ? std::string::npos : q - p));
        if (q == std::string::npos) break;
        p = q + 3;
    }
    return out;
}

// ["Ruby","Ruby","Sapphire"] -> "2x Ruby, Sapphire"
std::string collapse(const std::vector<std::string>& names) {
    std::vector<std::string> order;
    std::map<std::string, int> counts;
    for (const auto& n : names) {
        if (counts[n]++ == 0) order.push_back(n);
    }
    std::string out;
    for (const auto& n : order) {
        if (!out.empty()) out += ", ";
        if (counts[n] > 1) out += std::to_string(counts[n]) + "x ";
        out += n;
    }
    return out;
}

std::string treasure_label(const TreasureResult& t) {
    return t.duplicate_ordinal ? t.name + " #" + std::to_string(t.duplicate_ordinal) : t.name;
}

// What the optimiser wants in a treasure against what is socketed, as
// multisets. Gems come back out for free, so a mismatch is a trip to the
// merchant, never lost value.
struct GemStatus {
    bool done = false;
    std::vector<std::pair<std::string, bool>> parts; // gem, already in place
    std::vector<std::string> strays;                 // socketed but not wanted
};
GemStatus gem_status(const TreasureResult& t) {
    GemStatus g;
    std::map<std::string, int> pool;
    for (const auto& n : t.actual_gems) pool[n]++;
    bool missing = false;
    for (const auto& n : t.gems_used) {
        const bool ok = pool[n] > 0;
        if (ok) pool[n]--;
        else missing = true;
        g.parts.emplace_back(n, ok);
    }
    for (const auto& [n, c] : pool) for (int i = 0; i < c; ++i) g.strays.push_back(n);
    g.done = t.gems_used.empty() ? t.actual_gems.empty() : (!missing && g.strays.empty());
    return g;
}

// ---------------------------------------------------------------------------
// Drawing pieces
// ---------------------------------------------------------------------------
ImDrawList* dl() { return ImGui::GetWindowDrawList(); }

// Ruled paper behind everything, like the page's lines.
void draw_paper() {
    const auto pos = ImGui::GetWindowPos();
    const auto size = ImGui::GetWindowSize();
    const float top = ImGui::GetCursorScreenPos().y;
    for (float y = top + S(26); y < pos.y + size.y - 4; y += S(30)) {
        dl()->AddLine(ImVec2(pos.x + 2, y), ImVec2(pos.x + size.x - 2, y), col(pal().paper_line), 1.0f);
    }
}

// A small bordered box with a caption and a value.
void box(const char* id, const std::string& caption, const std::string& value, uint32_t value_hex, float w) {
    const auto p = ImGui::GetCursorScreenPos();
    const float h = S(46);
    dl()->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col(pal().paper_lt), S(4));
    dl()->AddRect(p, ImVec2(p.x + w, p.y + h), col(pal().edge), S(4), 0, S(2));
    dl()->AddText(ImVec2(p.x + S(9), p.y + S(6)), col(pal().dim), caption.c_str());
    dl()->AddText(ImVec2(p.x + S(9), p.y + S(24)), col(value_hex), value.c_str());
    ImGui::InvisibleButton(id, ImVec2(w, h));
}

void draw_status(const OutputState* st) {
    const auto p = ImGui::GetCursorScreenPos();
    const float th = ImGui::GetTextLineHeight();
    const bool paused = g_paused.load();
    const uint32_t dot = st == nullptr ? 0xe0a34a : (paused ? 0xe0a34a : 0x6fbf5a);
    dl()->AddCircleFilled(ImVec2(p.x + S(5), p.y + th / 2), S(4.5f), col(dot), 12);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(16), p.y));
    text(pal().dim, st == nullptr ? "Waiting for a loaded save"
                    : paused ? "Paused while the game loads or saves"
                             : "Live from the game");
}

// ---------------------------------------------------------------------------
// Window sizing (Size / Width / Height, the same rules as the Charm Predictor)
// ---------------------------------------------------------------------------
ImVec2 g_min_win{0, 0};        // smallest size that shows every control, measured each frame
ImVec2 g_last_win{0, 0};       // window size as of the last frame
float g_last_scale = 0.0f;
float g_drag_scale = 0.0f;     // scale when the Size slider drag started
bool g_pending_win = false;
ImVec2 g_pending_frac{0, 0};
bool g_saved_size_applied = false;
float g_row_w = 0.0f;
std::chrono::steady_clock::time_point g_copied_at{};

void mark_row() {
    g_row_w = std::max(g_row_w, ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x);
}

void draw_controls() {
    ImGui::Spacing();
    g_row_w = 0.0f;
    const auto& io = ImGui::GetIO();
    const auto ds = io.DisplaySize;

    // Size: live preview while dragging. The slider keeps the width it had
    // when the drag started, so it does not stretch away from the mouse.
    ImGui::SetNextItemWidth((g_drag_scale > 0 ? g_drag_scale : g_cfg.scale) * 150.0f);
    float v = g_cfg.scale;
    if (ImGui::SliderFloat("Size", &v, 0.75f, 3.0f, "%.2fx")) {
        if (g_drag_scale <= 0) g_drag_scale = g_cfg.scale;
        g_cfg.scale = std::floor(v * 20.0f + 0.5f) / 20.0f;
        g_cfg.scale_auto = false;
    }
    if (g_drag_scale > 0 && !ImGui::IsItemActive()) {
        g_drag_scale = 0;
        save_settings();
    }
    ImGui::SameLine();
    if (g_cfg.scale_auto) {
        text(pal().dim, "(auto)");
    } else if (ImGui::SmallButton("Auto")) {
        g_cfg.scale_auto = true;
        save_settings();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto sizes the window to your screen resolution.");

    // Width and height: 0% is the smallest size that still shows these
    // controls, 100% is the whole screen. They follow edge drags too.
    if (g_last_win.x > 0 && g_min_win.x > 0 && ds.x > 0 && ds.y > 0) {
        const float span_x = std::max(ds.x - g_min_win.x, 1.0f);
        const float span_y = std::max(ds.y - g_min_win.y, 1.0f);
        const auto pct = [](float val, float lo, float span) {
            return std::clamp((int)std::floor((val - lo) / span * 100.0f + 0.5f), 0, 100);
        };
        int wp = pct(g_last_win.x, g_min_win.x, span_x);
        int hp = pct(g_last_win.y, g_min_win.y, span_y);
        const char* tip = "0% is the smallest size that still shows these controls, 100% is the whole screen.";
        ImGui::SameLine(0, S(22));
        ImGui::SetNextItemWidth(S(150));
        const bool cw = ImGui::SliderInt("Width", &wp, 0, 100, "%d%%");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine(0, S(22));
        ImGui::SetNextItemWidth(S(150));
        const bool ch = ImGui::SliderInt("Height", &hp, 0, 100, "%d%%");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        if (cw || ch) {
            g_pending_win = true;
            g_pending_frac = ImVec2(cw ? (g_min_win.x + wp / 100.0f * span_x) / ds.x : g_last_win.x / ds.x,
                                    ch ? (g_min_win.y + hp / 100.0f * span_y) / ds.y : g_last_win.y / ds.y);
        }
    }
    mark_row();

    // Second row: widget, look and browser view.
    if (ImGui::Checkbox("Widget", &g_cfg.show_widget)) save_settings();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("A small net worth panel on top of the game.\n"
                          "Drag it while the REFramework menu is open.");
    }
    if (g_cfg.show_widget) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(110));
        if (ImGui::SliderFloat("Widget size", &g_cfg.widget_scale, 0.5f, 3.0f, "%.2fx")) {
            g_cfg.widget_scale = std::floor(g_cfg.widget_scale * 20.0f + 0.5f) / 20.0f;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) save_settings();
    }
    ImGui::SameLine(0, S(26));
    if (ImGui::Checkbox("Dark mode", &g_cfg.dark)) save_settings();
    ImGui::SameLine(0, S(26));
    if (ImGui::Checkbox("Browser view", &g_cfg.browser_export)) save_settings();
    if (g_cfg.browser_export) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy browser link")) {
            ImGui::SetClipboardText(g_page_url.c_str());
            g_copied_at = std::chrono::steady_clock::now();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Copies a link to the browser view. Paste it into your browser's address bar.\n%s",
                              g_page_url.c_str());
        }
    }
    mark_row();
    if (g_copied_at.time_since_epoch().count() != 0 &&
        std::chrono::steady_clock::now() - g_copied_at < std::chrono::seconds(2)) {
        ImGui::SameLine();
        text(pal().green, "Copied");
    }

    // Room for the right padding and a scrollbar, and the bottom padding.
    g_min_win = ImVec2(std::min(g_row_w + S(18) + S(22), ds.x > 0 ? ds.x : FLT_MAX),
                       std::min(ImGui::GetCursorPosY() + S(14), ds.y > 0 ? ds.y : FLT_MAX));
}

// ---------------------------------------------------------------------------
// Net worth content
// ---------------------------------------------------------------------------
double sum_items(const std::vector<ItemEntry>& v) {
    double s = 0;
    for (const auto& e : v) s += e.total_price;
    return s;
}

void draw_total(const OutputState& st) {
    kicker("Net worth");
    ImGui::PushFont(g_font, S(34));
    text(pal().ink, fmt(st.total));
    ImGui::PopFont();
    ImGui::SameLine();
    // Sit "ptas" on the big figure's baseline.
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(34) - S(16) - S(4));
    text(pal().dim, "ptas");

    double treasures = 0;
    for (const auto& t : st.treasures) treasures += t.final_price;
    struct Cell { const char* id; const char* caption; double value; bool plus; };
    std::vector<Cell> cells = {
        {"##b0", "Pesetas", (double)st.pesetas, false},
        {"##b1", "Items", sum_items(st.items), false},
        {"##b2", "Storage", sum_items(st.storage), false},
        {"##b3", "Key items", sum_items(st.key_items), false},
        {"##b4", "Treasures combined", treasures, false},
        {"##b5", "Treasures as-is", sum_items(st.plain_treasures), false},
        {"##b6", "Leftover gems", sum_items(st.leftover_gems), false},
    };
    if (st.herb_total > 0) cells.push_back({"##b7", "Herbs (combined)", st.herb_total, false});
    if (st.trade_shop_gain > 0) cells.push_back({"##b8", "Trade shop plan", st.trade_shop_gain, true});

    ImGui::Spacing();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float gap = S(8);
    const int cols = std::max(1, (int)((avail + gap) / (S(170) + gap)));
    const float w = (avail - (cols - 1) * gap) / cols;
    for (size_t i = 0; i < cells.size(); ++i) {
        if (i % cols != 0) ImGui::SameLine(0, gap);
        const auto& c = cells[i];
        box(c.id, c.caption, (c.plus ? "+" : "") + fmt(c.value), c.plus ? pal().green : pal().ink, w);
    }

    if (st.ammo_bonus_pct > 0 || st.heal_bonus_pct > 0) {
        std::string s = "Charm bonuses included:";
        if (st.ammo_bonus_pct > 0) s += " ammo +" + std::to_string(st.ammo_bonus_pct) + "%";
        if (st.heal_bonus_pct > 0) {
            if (st.ammo_bonus_pct > 0) s += ",";
            s += " recovery items +" + std::to_string(st.heal_bonus_pct) + "%";
        }
        text(pal().dim, s);
    }
}

// The single list of everything actionable, in the order it has to happen:
// unclaimed Spinels before spending them, buying before socketing.
// Crafting is left out on purpose - it is irreversible and costs usable ammo.
struct Step { std::string text; double gain = 0; std::string note; std::string blocked; };

void draw_plan(const OutputState& st) {
    std::vector<Step> steps;
    if (st.spinel_pending > 0) {
        steps.push_back({"Hand in " + std::to_string(st.pending_quest_count) + " request" +
                             (st.pending_quest_count == 1 ? "" : "s"),
                         0, "+" + std::to_string(st.spinel_pending) + " Spinels", ""});
    }
    for (const auto& q : st.sell_quests) {
        steps.push_back({"Sell " + q.requirement, 0, "+" + std::to_string(q.spinels) + " Spinels", ""});
    }
    if (!st.trade_shop_recommended.empty()) {
        int cost = 0;
        std::string names;
        for (const auto& t : st.trade_shop_recommended) {
            cost += t.spinel_cost;
            if (!names.empty()) names += ", ";
            names += (t.item_count > 1 ? std::to_string(t.item_count) + "x " : "") + t.name;
        }
        steps.push_back({"Buy " + names, 0, std::to_string(cost) + " Spinels",
                         st.spinel_pending > 0 && cost > st.spinel_count ? "after handing in" : ""});
    }
    for (const auto& t : st.trade_shop_combos) {
        if (!t.gems_used.empty()) steps.push_back({treasure_label(t) + " <- " + collapse(t.gems_used), t.final_price, "", ""});
    }
    for (const auto& t : st.treasures) {
        if (!t.gems_used.empty() && !gem_status(t).done) {
            steps.push_back({treasure_label(t) + " <- " + collapse(t.gems_used), t.final_price, "", ""});
        }
    }
    // The plan's END state: buying a Yellow Herb changes the right mix.
    const auto& herb_steps = st.trade_shop_herb_steps.empty() ? st.herb_steps : st.trade_shop_herb_steps;
    for (const auto& s : herb_steps) {
        std::string line = "Mix ";
        const auto parts = split_plus(s);
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i) line += " + ";
            line += herb_local(st, parts[i], true);
        }
        steps.push_back({line, 0, "", ""});
    }
    if (steps.empty()) return;

    ImGui::Spacing();
    kicker("The plan");
    if (st.trade_shop_gain > 0) {
        ImGui::SameLine();
        text(pal().green, "+" + fmt(st.trade_shop_gain) + " from the Trade shop");
    }
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& s = steps[i];
        text(pal().dim, std::to_string(i + 1) + ".");
        ImGui::SameLine();
        text(pal().ink, s.text);
        if (s.gain > 0) { ImGui::SameLine(0, S(14)); text(pal().green, fmt(s.gain)); }
        if (!s.note.empty()) { ImGui::SameLine(0, S(14)); text(pal().dim, s.note); }
        if (!s.blocked.empty()) { ImGui::SameLine(0, S(14)); text(pal().red, s.blocked); }
    }
}

constexpr ImGuiTableFlags TABLE_FLAGS = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                        ImGuiTableFlags_BordersOuterH | ImGuiTableFlags_PadOuterX |
                                        ImGuiTableFlags_SizingStretchProp;

float num_w() { return ImGui::CalcTextSize("0,000,000").x; }

bool section(const char* label, const char* id, const std::string& total) {
    ImGui::Spacing();
    std::string l = std::string(label);
    if (!total.empty()) l += "   " + total;
    l += "###";
    l += id;
    return ImGui::CollapsingHeader(l.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
}

void items_table(const char* id, const std::vector<ItemEntry>& items,
                 const char* name_col = "Name", const char* total_col = "Total") {
    auto sorted = items;
    std::sort(sorted.begin(), sorted.end(),
              [](const ItemEntry& a, const ItemEntry& b) { return a.total_price > b.total_price; });
    if (!ImGui::BeginTable(id, 4, TABLE_FLAGS)) return;
    ImGui::TableSetupColumn(name_col, ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Count").x);
    ImGui::TableSetupColumn("Unit", ImGuiTableColumnFlags_WidthFixed, num_w());
    ImGui::TableSetupColumn(total_col, ImGuiTableColumnFlags_WidthFixed, num_w());
    ImGui::TableHeadersRow();
    for (const auto& e : sorted) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); text(pal().ink, e.name.empty() ? "(unknown)" : e.name);
        ImGui::TableNextColumn(); num_cell(std::to_string(e.count), pal().ink2);
        ImGui::TableNextColumn(); num_cell(fmt(e.unit_price), pal().ink2);
        ImGui::TableNextColumn(); num_cell(fmt(e.total_price), pal().ink);
    }
    ImGui::EndTable();
}

void treasure_table(const char* id, const std::vector<TreasureResult>& list, bool show_status) {
    if (!ImGui::BeginTable(id, 4, TABLE_FLAGS)) return;
    ImGui::TableSetupColumn("Treasure", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Base", ImGuiTableColumnFlags_WidthFixed, num_w());
    ImGui::TableSetupColumn("Gems", ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn("Final", ImGuiTableColumnFlags_WidthFixed, num_w());
    ImGui::TableHeadersRow();
    const auto& p = pal();
    for (const auto& t : list) {
        ImGui::TableNextRow();
        GemStatus g;
        if (show_status) {
            g = gem_status(t);
            if (g.done) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, col(p.green, 0x22));
        }
        ImGui::TableNextColumn(); text(p.ink, treasure_label(t));
        ImGui::TableNextColumn(); num_cell(fmt(t.base_price), p.dim);
        ImGui::TableNextColumn();
        if (t.gems_used.empty()) {
            text(p.dim, "(no gems combined)");
        } else if (!show_status) {
            text(p.ink2, collapse(t.gems_used));
        } else {
            // Green = already in place, red = still to socket.
            for (size_t i = 0; i < g.parts.size(); ++i) {
                if (i) { ImGui::SameLine(0, 0); text(p.dim, ", "); ImGui::SameLine(0, 0); }
                text(g.parts[i].second ? p.green : p.red, g.parts[i].first);
            }
        }
        if (show_status && !g.strays.empty()) {
            std::string s;
            for (const auto& n : g.strays) { if (!s.empty()) s += ", "; s += n; }
            text(p.amber, "take out " + s + " first");
        }
        ImGui::TableNextColumn(); num_cell(fmt(t.final_price), p.ink);
    }
    ImGui::EndTable();
}

void draw_sections(const OutputState& st) {
    const auto& p = pal();

    double treasures = 0;
    for (const auto& t : st.treasures) treasures += t.final_price;
    if (!st.treasures.empty() && section("Combined treasures", "tre", fmt(treasures) + " ptas")) {
        text(p.dim, "Green gems are already in place, red ones still need socketing.");
        treasure_table("##tre", st.treasures, true);
    }
    if (!st.plain_treasures.empty() &&
        section("Treasures to sell as they are", "plain", fmt(sum_items(st.plain_treasures)) + " ptas")) {
        items_table("##plain", st.plain_treasures);
    }
    if (!st.leftover_gems.empty() &&
        section("Leftover gems", "left", fmt(sum_items(st.leftover_gems)) + " ptas")) {
        text(p.dim, "Gems no combination could use profitably.");
        items_table("##left", st.leftover_gems);
    }
    if (!st.items.empty() && section("Inventory", "inv", fmt(sum_items(st.items)) + " ptas")) {
        items_table("##inv", st.items);
    }
    if (!st.storage.empty() && section("Storage", "sto", fmt(sum_items(st.storage)) + " ptas")) {
        items_table("##sto", st.storage);
    }
    if (!st.key_items.empty() && section("Key items you can sell", "key", fmt(sum_items(st.key_items)) + " ptas")) {
        items_table("##key", st.key_items);
    }

    if (!st.herbs.empty() && section("Herbs, valued combined", "herb", fmt(st.herb_total) + " ptas")) {
        auto herbs = st.herbs;
        for (auto& h : herbs) h.name = herb_local(st, h.name, false);
        items_table("##herb", herbs, "Holding", "As-is");
        if (!st.herb_steps.empty()) {
            text(p.dim, "Combine these (+" + fmt(st.herb_total - st.herb_raw_total) + "):");
            for (const auto& s : st.herb_steps) {
                std::string line;
                const auto parts = split_plus(s);
                for (size_t i = 0; i < parts.size(); ++i) {
                    if (i) line += " + ";
                    line += herb_local(st, parts[i], false);
                }
                text(p.ink2, "  " + line);
            }
        }
    }

    if (!st.craft_plan.empty() &&
        section("Ammo crafting (not counted)", "craft", "+" + fmt(st.craft_gain) + " ptas if crafted")) {
        text(p.dim, "Crafting cannot be undone and you probably want the ammo,\n"
                    "so this is shown for you to judge and is not in the total.");
        if (ImGui::BeginTable("##craft", 4, TABLE_FLAGS)) {
            ImGui::TableSetupColumn("Craft", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Times", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Times").x);
            ImGui::TableSetupColumn("Rounds", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Rounds").x);
            ImGui::TableSetupColumn("Worth", ImGuiTableColumnFlags_WidthFixed, num_w());
            ImGui::TableHeadersRow();
            for (const auto& c : st.craft_plan) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn(); text(p.ink, c.local_name.empty() ? c.name : c.local_name);
                ImGui::TableNextColumn(); num_cell(std::to_string(c.times), p.ink2);
                ImGui::TableNextColumn(); num_cell(std::to_string(c.rounds), p.ink2);
                ImGui::TableNextColumn(); num_cell(fmt(c.value), p.ink);
            }
            ImGui::EndTable();
        }
    }

    // Separate Ways only. The combine plan IS counted (buying these and
    // combining them is a real, reversible gain); the catalog resale is not,
    // since Spinels only turn into pesetas by spending them.
    if (!st.trade_items.empty()) {
        std::string budget = "Spinels: " + std::to_string(st.spinel_count);
        if (st.spinel_pending > 0) {
            budget += " +" + std::to_string(st.spinel_pending) + " from " + std::to_string(st.pending_quest_count) +
                      " unclaimed request" + (st.pending_quest_count == 1 ? "" : "s");
        }
        if (st.spinel_from_sales > 0) budget += " +" + std::to_string(st.spinel_from_sales) + " from sellable request items";
        if (section("Trade shop", "trade", budget)) {
            if (!st.trade_shop_recommended.empty()) {
                text(p.green, "Combine plan, counted in net worth: +" + fmt(st.trade_shop_gain) + " ptas");
                if (ImGui::BeginTable("##tplan", 3, TABLE_FLAGS)) {
                    ImGui::TableSetupColumn("Shopping list", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableSetupColumn("Spinels", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Spinels").x);
                    ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Count").x);
                    ImGui::TableHeadersRow();
                    for (const auto& t : st.trade_shop_recommended) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn(); text(p.ink, t.name);
                        ImGui::TableNextColumn(); num_cell(std::to_string(t.spinel_cost), p.ink2);
                        ImGui::TableNextColumn(); num_cell(std::to_string(t.item_count), p.ink2);
                    }
                    ImGui::EndTable();
                }
                if (!st.trade_shop_combos.empty()) treasure_table("##tcombo", st.trade_shop_combos, false);
            }
            std::vector<TradeItemEntry> sellable;
            for (const auto& t : st.trade_items) if (t.total_sell_value > 0) sellable.push_back(t);
            std::sort(sellable.begin(), sellable.end(),
                      [](const TradeItemEntry& a, const TradeItemEntry& b) { return a.spinel_cost < b.spinel_cost; });
            double resale = 0;
            for (const auto& t : sellable) resale += t.total_sell_value;
            const auto hidden = st.trade_items.size() - sellable.size();
            text(p.dim, "Full catalog, resale value (not counted): " + fmt(resale) + " ptas" +
                            (hidden ? " (" + std::to_string(hidden) + " unsellable hidden)" : ""));
            if (!sellable.empty() && ImGui::BeginTable("##tcat", 4, TABLE_FLAGS)) {
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Spinels", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Spinels").x);
                ImGui::TableSetupColumn("Count", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Count").x);
                ImGui::TableSetupColumn("Resale", ImGuiTableColumnFlags_WidthFixed, num_w());
                ImGui::TableHeadersRow();
                for (const auto& t : sellable) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn(); text(p.ink, t.name);
                    ImGui::TableNextColumn(); num_cell(std::to_string(t.spinel_cost), p.ink2);
                    ImGui::TableNextColumn(); num_cell(std::to_string(t.item_count), p.ink2);
                    ImGui::TableNextColumn(); num_cell(fmt(t.total_sell_value), p.ink);
                }
                ImGui::EndTable();
            }
        }
    }
}

void draw_window_body(const OutputState* st) {
    draw_paper();
    draw_status(st);
    if (st != nullptr) draw_total(*st);
    draw_controls();
    ImGui::Separator();
    if (st == nullptr) {
        text(pal().dim, "The breakdown appears here once a save is loaded.");
        return;
    }
    draw_plan(*st);
    draw_sections(*st);
}

void draw_window(const OutputState* st) {
    auto& io = ImGui::GetIO();
    const auto ds = io.DisplaySize;
    if (g_cfg.scale_auto && ds.y > 0) {
        const float a = auto_scale(ds.y);
        if (a != g_cfg.scale) { g_cfg.scale = a; save_settings(); }
    }

    push_theme();
    if (g_min_win.x > 0) ImGui::SetNextWindowSizeConstraints(g_min_win, ImVec2(FLT_MAX, FLT_MAX));
    if (g_pending_win && ds.x > 0) {
        ImGui::SetNextWindowSize(ImVec2(ds.x * g_pending_frac.x, ds.y * g_pending_frac.y), ImGuiCond_Always);
        g_pending_win = false;
    } else if (g_last_scale > 0 && g_last_win.x > 0 && g_last_scale != g_cfg.scale) {
        // Size changed: grow or shrink the window with its contents.
        const float k = g_cfg.scale / g_last_scale;
        g_min_win = ImVec2(g_min_win.x * k, g_min_win.y * k);
        if (g_min_win.x > 0) ImGui::SetNextWindowSizeConstraints(g_min_win, ImVec2(FLT_MAX, FLT_MAX));
        // ...but never past the screen, or the resize grip ends up off it.
        ImGui::SetNextWindowSize(ImVec2(std::min(g_last_win.x * k, std::max(ds.x, g_min_win.x)),
                                        std::min(g_last_win.y * k, std::max(ds.y, g_min_win.y))),
                                 ImGuiCond_Always);
    } else if (!g_saved_size_applied && g_cfg.win_w > 0 && g_cfg.win_h > 0 && ds.x > 0) {
        ImGui::SetNextWindowSize(ImVec2(ds.x * std::min(g_cfg.win_w, 1.0f), ds.y * std::min(g_cfg.win_h, 1.0f)),
                                 ImGuiCond_Always);
    } else {
        ImGui::SetNextWindowSize(ImVec2(S(820), std::min(S(900), ds.y * 0.85f)), ImGuiCond_FirstUseEver);
    }
    g_saved_size_applied = true;
    g_last_scale = g_cfg.scale;

    bool open = true;
    if (ImGui::Begin("RE4 Net Worth", &open)) {
        if (!ImGui::IsWindowCollapsed()) {
            g_last_win = ImGui::GetWindowSize();
            // Remember the size once the mouse is let go (slider or edge drag).
            if (ds.x > 0 && ds.y > 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const float w = std::floor(g_last_win.x / ds.x * 1000.0f + 0.5f) / 1000.0f;
                const float h = std::floor(g_last_win.y / ds.y * 1000.0f + 0.5f) / 1000.0f;
                if (w != g_cfg.win_w || h != g_cfg.win_h) { g_cfg.win_w = w; g_cfg.win_h = h; save_settings(); }
            }
        }
        draw_window_body(st);
    }
    ImGui::End();
    pop_theme();
    if (!open) { g_cfg.show_window = false; save_settings(); }
}

// ---------------------------------------------------------------------------
// Widget: the stream overlay's glass panel, drawn on top of the game.
// ---------------------------------------------------------------------------
double g_shown = 0;               // the figure currently drawn
double g_roll_from = 0, g_roll_to = 0;
double g_last_target = -1;
std::chrono::steady_clock::time_point g_roll_start{};
double g_delta_accum = 0;
std::chrono::steady_clock::time_point g_delta_at{};

ImVec2 g_grab{0, 0};              // where in the panel the drag started

constexpr double ROLL_MS = 700.0;
constexpr double DELTA_SETTLE_MS = 2000.0;
constexpr double DELTA_FADE_MS = 500.0;

void draw_widget(const OutputState* st) {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    const auto ms_since = [&](clock::time_point t) {
        return std::chrono::duration<double, std::milli>(now - t).count();
    };

    if (st != nullptr && st->total != g_last_target) {
        if (g_last_target < 0) {
            g_shown = st->total;   // first figure: no roll, no delta
        } else {
            g_delta_accum = (ms_since(g_delta_at) < DELTA_SETTLE_MS + DELTA_FADE_MS ? g_delta_accum : 0.0) +
                            (st->total - g_last_target);
            g_delta_at = now;
        }
        g_roll_from = g_shown;
        g_roll_to = st->total;
        g_roll_start = now;
        g_last_target = st->total;
    }
    if (g_last_target >= 0) {
        const double t = std::min(1.0, ms_since(g_roll_start) / ROLL_MS);
        const double e = 1.0 - std::pow(1.0 - t, 3.0);   // ease-out
        g_shown = g_roll_from + (g_roll_to - g_roll_from) * e;
    }

    // At 1.0x on a 2160p screen this is the stream overlay's own size
    // (64px figure, 20px title, 28px change); it scales with the screen.
    const float k = g_cfg.widget_scale * std::max(0.5f, ImGui::GetIO().DisplaySize.y / 1080.0f);
    const float value_size = 32.0f * k, title_size = 10.0f * k, delta_size = 14.0f * k;
    ImFont* f = g_sys_font;   // REFramework's sans font, like the overlay's Segoe UI

    // Measured with the draw list's own sizing, so text and panel agree even
    // if REFramework scales its style fonts.
    const auto measure = [&](float sz, const char* s) { return f->CalcTextSizeA(sz, FLT_MAX, 0.0f, s).x; };
    const ImVec2 widest(measure(value_size, "10,000,000"), value_size);
    const float pad_x = 20.0f * k, pad_y = 13.0f * k;
    const ImVec2 size(widest.x + pad_x * 2, value_size + title_size + pad_y * 2 + 4.0f * k);

    const auto ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * g_cfg.widget_x, ds.y * g_cfg.widget_y), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(size.x, size.y + delta_size + 8.0f * k), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                   ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##re4_net_worth_widget", nullptr, flags)) {
        const auto p = ImGui::GetWindowPos();
        // The foreground list draws over every window, REFramework's menu
        // included, so the panel is never hidden behind the net worth window.
        // The invisible window around it only provides the drag handle.
        auto* d = ImGui::GetForegroundDrawList();
        const float r = 15.0f * k;
        const ImVec2 b(p.x + size.x, p.y + size.y);
        // Dark glass with a sheen at the top and a bright rim.
        d->AddRectFilled(p, b, IM_COL32(12, 12, 16, 150), r);
        d->AddRectFilled(p, ImVec2(b.x, p.y + size.y * 0.45f), IM_COL32(255, 255, 255, 18), r,
                         ImDrawFlags_RoundCornersTop);
        d->AddRect(p, b, IM_COL32(255, 255, 255, 60), r, 0, std::max(1.0f, 1.2f * k));

        const auto shadowed = [&](float sz, ImVec2 at, ImU32 c, const char* s) {
            d->AddText(f, sz, ImVec2(at.x + 1.5f * k, at.y + 2.0f * k), IM_COL32(0, 0, 0, 150), s);
            d->AddText(f, sz, at, c, s);
        };

        const std::string value = st == nullptr && g_last_target < 0 ? "--" : fmt(g_shown);
        const float vw = measure(value_size, value.c_str());
        shadowed(value_size, ImVec2(p.x + (size.x - vw) / 2, p.y + pad_y), IM_COL32(0xf4, 0xd5, 0x8d, 255), value.c_str());

        const char* title = st == nullptr ? "WAITING FOR GAME" : "NET WORTH";
        const float tw = measure(title_size, title);
        shadowed(title_size, ImVec2(p.x + (size.x - tw) / 2, p.y + pad_y + value_size + 4.0f * k),
                 IM_COL32(0xc9, 0xc9, 0xd2, 255), title);

        // The change, accumulated over a burst, shown under the panel.
        const double since = ms_since(g_delta_at);
        if (g_delta_accum != 0 && g_delta_at.time_since_epoch().count() != 0 && since < DELTA_SETTLE_MS + DELTA_FADE_MS) {
            const double a = since < DELTA_SETTLE_MS ? 1.0 : 1.0 - (since - DELTA_SETTLE_MS) / DELTA_FADE_MS;
            const std::string ds_text = (g_delta_accum > 0 ? "+" : "-") + fmt(std::abs(g_delta_accum));
            const float dw = measure(delta_size, ds_text.c_str());
            const ImU32 c = g_delta_accum > 0 ? IM_COL32(0x7f, 0xd1, 0x8b, (int)(255 * a))
                                              : IM_COL32(0xe8, 0x61, 0x5f, (int)(255 * a));
            d->AddText(f, delta_size, ImVec2(p.x + (size.x - dw) / 2 + 1.5f * k, b.y + 4.0f * k + 2.0f * k),
                       IM_COL32(0, 0, 0, (int)(120 * a)), ds_text.c_str());
            d->AddText(f, delta_size, ImVec2(p.x + (size.x - dw) / 2, b.y + 4.0f * k), c, ds_text.c_str());
        }

        // Movable while the REFramework menu is open (that is when ImGui gets
        // the mouse). Saved as the panel's top-centre, as a share of the screen.
        ImGui::SetCursorScreenPos(p);
        ImGui::InvisibleButton("##drag", size);
        const auto mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsItemActivated()) g_grab = ImVec2(mouse.x - p.x, mouse.y - p.y);
        if (ImGui::IsItemActive()) ImGui::SetWindowPos(ImVec2(mouse.x - g_grab.x, mouse.y - g_grab.y));
        if (ImGui::IsItemDeactivated() && ds.x > 0 && ds.y > 0) {
            const auto np = ImGui::GetWindowPos();
            g_cfg.widget_x = std::clamp((np.x + size.x / 2) / ds.x, 0.0f, 1.0f);
            g_cfg.widget_y = std::clamp(np.y / ds.y, 0.0f, 1.0f);
            save_settings();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------
// REFramework callbacks
// ---------------------------------------------------------------------------
void bind_context(REFImGuiFrameCbData* data) {
    ImGui::SetCurrentContext((ImGuiContext*)data->context);
    ImGui::SetAllocatorFunctions((ImGuiMemAllocFunc)data->malloc_fn, (ImGuiMemFreeFunc)data->free_fn, data->user_data);
}

ImGuiContext* g_ctx = nullptr;

void on_imgui_frame(REFImGuiFrameCbData* data) {
    if (data == nullptr || data->context == nullptr) return;
    bind_context(data);

    // A new context means a new font atlas: our font has to be added again.
    if ((ImGuiContext*)data->context != g_ctx) {
        g_ctx = (ImGuiContext*)data->context;
        g_font = nullptr;
        g_font_tried = false;
    }

    // REFramework's current font, captured before ours is pushed: it covers
    // the languages Courier Prime does not, and is the widget's font.
    g_sys_font = ImGui::GetFont();
    auto* atlas = ImGui::GetIO().Fonts;
    // If REFramework ever rebuilds its atlas, our font goes with it: drop the
    // pointer rather than draw with a freed font, and add it again.
    if (g_font != nullptr && !atlas->Fonts.contains(g_font)) {
        g_font = nullptr;
        g_font_tried = false;
    }
    // Only while the atlas accepts new fonts (asserts are compiled out, so
    // nothing else would stop a locked atlas being changed).
    if (!g_font_tried && !atlas->Locked) {
        g_font_tried = true;
        if (GetFileAttributesA(g_font_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_font = atlas->AddFontFromFileTTF(g_font_path.c_str(), 16.0f);
        }
        if (g_font == nullptr) log_info("window font not found at %s - using REFramework's", g_font_path.c_str());
    }

    std::shared_ptr<const OutputState> st;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        st = g_state;
    }
    if (g_cfg.show_widget) draw_widget(st.get());
    if (g_cfg.show_window) draw_window(st.get());
}

void on_imgui_draw_ui(REFImGuiFrameCbData* data) {
    if (data == nullptr || data->context == nullptr) return;
    bind_context(data);
    if (ImGui::TreeNode((std::string("RE4 Net Worth v") + VERSION).c_str())) {
        if (ImGui::Checkbox("Show window", &g_cfg.show_window)) save_settings();
        if (ImGui::Checkbox("Show widget", &g_cfg.show_widget)) save_settings();
        ImGui::TreePop();
    }
}

} // namespace

bool ui_initialize(const REFrameworkPluginFunctions* fns, const std::string& data_dir) {
    g_data_dir = data_dir;
    g_settings_path = data_dir + "re4_net_worth_window.json";
    CreateDirectoryA((data_dir + "re4_net_worth").c_str(), nullptr);
    load_settings();

    const auto game = game_dir_w();
    g_page_url = file_url(wide_to_utf8(game + L"\\reframework\\data\\re4_net_worth\\NetWorth.html"));
    g_font_path = data_dir + "..\\fonts\\re4_net_worth\\CourierPrime-Bold.ttf";

    if (fns == nullptr || fns->on_imgui_frame == nullptr || fns->on_imgui_draw_ui == nullptr) return false;
    const bool a = fns->on_imgui_frame(on_imgui_frame);
    const bool b = fns->on_imgui_draw_ui(on_imgui_draw_ui);
    return a && b;
}

void ui_publish(const OutputState& st) {
    auto copy = std::make_shared<OutputState>(st);
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_state = std::move(copy);
}

void ui_set_paused(bool paused) { g_paused = paused; }
bool ui_browser_export() { return g_export_flag.load(); }
bool ui_dark() { return g_dark_flag.load(); }

} // namespace re4
