// SPDX-License-Identifier: GPL-2.0-or-later
// BloodborneVR.exe: the native dashboard (Dear ImGui + SDL3 renderer). All logic and files live in the
// Python side (launcher/bbvr_cli.py, bbvr_settings.py, bbvr_launcher.py), so this window and the web menu
// (BloodborneVR.exe --web) read and write the same settings.
//
// Layout: a left sidebar of pages, a content area, and a bottom bar that is always there (status line, Stop, PLAY).
// Pages: Play (banner from the player's own game folder, ready check, VR / Co-op / Flat mode, VR quality presets),
// Graphics, Display & FPS, VR comfort, Combat & hands, Controls, Online co-op, Advanced, Log, About. The option
// pages are drawn from the "pages" and "options" the Python side sends, so a new option only needs the schema.
// The look is a dark Bloodborne palette: near-black browns, warm bone text, one gold accent, blood red for PLAY.
//
//   BloodborneVR.exe                        the dashboard
//   BloodborneVR.exe --web [args]           the web settings menu in a console window (old behaviour)
//   BloodborneVR.exe --check / --reset ...  any launcher argument also runs the console launcher
//   BloodborneVR.exe --screenshot FILE.png [--tab play|graphics|display|comfort|combat|controls|online|advanced|log|about]
//        one frame, no PLAY. --demo pretends the first-run steps are done; --mode vr|coop|coopflat|flat, --advanced, --size WxH, --flow room|running|noserver
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <SDL3/SDL.h>
#include <atomic>
#include <algorithm>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#ifndef BBVR_VERSION
#define BBVR_VERSION "dev"
#endif

using json = nlohmann::json;
static float g_scale = 1.0f;
static bool g_demo = false;
static std::string g_demo_mode;  // screenshots: vr | coop | coopflat | flat
static float S(float v) { return v * g_scale; }

// ---------------------------------------------------------------------------------------------
// strings / paths / processes

static std::wstring W(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static std::string U8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static bool file_exists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static bool dir_exists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Quotes one argument so CommandLineToArgvW / the C runtime read it back unchanged.
static void add_arg(std::wstring& line, const std::wstring& arg)
{
    if (!line.empty()) line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        line += arg;
        return;
    }
    line += L'"';
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') slashes++, i++;
        if (i == arg.size()) {
            line.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            line.append(slashes * 2 + 1, L'\\');
            line += L'"';
        } else {
            line.append(slashes, L'\\');
            line += arg[i];
        }
    }
    line += L'"';
}

struct Env {
    std::wstring exe_dir, python, py_prefix, script_dir;  // script_dir holds bbvr_cli.py / bbvr_launcher.py
    bool ok = false;
    std::string problem;
};

static std::wstring env_var(const wchar_t* name)
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, 32768);
    return n && n < 32768 ? std::wstring(buf, n) : L"";
}

static Env find_env()
{
    Env e;
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    e.exe_dir.assign(buf, n);
    e.exe_dir = e.exe_dir.substr(0, e.exe_dir.find_last_of(L'\\'));
    std::wstring o = env_var(L"BBVR_LAUNCHER_DIR");
    if (!o.empty() && file_exists(o + L"\\bbvr_cli.py")) e.script_dir = o;
    if (e.script_dir.empty() && file_exists(e.exe_dir + L"\\launcher\\bbvr_cli.py")) e.script_dir = e.exe_dir + L"\\launcher";
    if (e.script_dir.empty()) {  // source tree: walk up to tools\release\zip
        std::wstring d = e.exe_dir;
        for (int i = 0; i < 6 && !d.empty(); i++) {
            if (file_exists(d + L"\\tools\\release\\zip\\bbvr_cli.py")) {
                e.script_dir = d + L"\\tools\\release\\zip";
                break;
            }
            size_t p = d.find_last_of(L'\\');
            if (p == std::wstring::npos) break;
            d = d.substr(0, p);
        }
    }
    o = env_var(L"BBVR_LAUNCHER_PYTHON");
    if (!o.empty()) e.python = o;
    else if (file_exists(e.exe_dir + L"\\python\\python.exe")) e.python = e.exe_dir + L"\\python\\python.exe";
    else if (SearchPathW(nullptr, L"py.exe", nullptr, 32768, buf, nullptr)) e.python = buf, e.py_prefix = L"-3";
    else if (SearchPathW(nullptr, L"python.exe", nullptr, 32768, buf, nullptr)) e.python = buf;
    if (e.script_dir.empty()) e.problem = "The launcher scripts were not found next to BloodborneVR.exe. Unzip the whole download and start it from the unzipped folder.";
    else if (e.python.empty()) e.problem = "Python was not found. Use the full download (it has a python folder) or install Python 3.10 or newer.";
    e.ok = e.problem.empty();
    return e;
}

static HANDLE make_job()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info = {};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info));
    return job;
}

// Runs a hidden console process, feeds stdin_data, passes combined stdout/stderr to on_chunk. Returns the
// exit code, or -1 when the process could not be started. job: assigned so the game tree can be killed.
static int run_capture(const Env& env, const std::vector<std::wstring>& args, const std::string& stdin_data,
                       const std::function<void(const std::string&)>& on_chunk, HANDLE job = nullptr)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE out_r, out_w, in_r, in_w;
    if (!CreatePipe(&out_r, &out_w, &sa, 0) || !CreatePipe(&in_r, &in_w, &sa, 0)) return -1;
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    std::wstring line;
    add_arg(line, env.python);
    if (!env.py_prefix.empty()) add_arg(line, env.py_prefix);
    for (auto& a : args) add_arg(line, a);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(env.python.c_str(), &line[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, env.exe_dir.c_str(), &si, &pi);
    CloseHandle(out_w);
    CloseHandle(in_r);
    if (!ok) {
        CloseHandle(out_r);
        CloseHandle(in_w);
        return -1;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    std::thread feeder([&] {
        DWORD w;
        size_t off = 0;
        while (off < stdin_data.size() && WriteFile(in_w, stdin_data.data() + off, (DWORD)(stdin_data.size() - off), &w, nullptr) && w) off += w;
        CloseHandle(in_w);
    });
    char buf[8192];
    DWORD got;
    while (ReadFile(out_r, buf, sizeof(buf), &got, nullptr) && got) on_chunk(std::string(buf, got));
    feeder.join();
    CloseHandle(out_r);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// Old behaviour: the console launcher (web menu) in its own console window; waits for it.
static int run_console_launcher(const Env& env, const std::vector<std::wstring>& args)
{
    std::wstring line;
    add_arg(line, env.python);
    if (!env.py_prefix.empty()) add_arg(line, env.py_prefix);
    add_arg(line, env.script_dir + L"\\bbvr_launcher.py");
    for (auto& a : args) add_arg(line, a);
    SetEnvironmentVariableW(L"BBVR_EXE", L"1");
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(env.python.c_str(), &line[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        nullptr, env.exe_dir.c_str(), &si, &pi)) {
        MessageBoxW(nullptr, L"Could not start Python.", L"Bloodborne VR", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}

// ---------------------------------------------------------------------------------------------
// application state (UI thread owns `st`; workers hand over results through `incoming`)

struct App {
    Env env;
    json st = json::object();
    bool have_state = false;
    std::mutex mu;                 // guards incoming, log, status text
    json incoming;                 // newest state from a worker
    bool has_incoming = false;
    std::mutex cli_mu;             // serialises calls of bbvr_cli.py
    std::atomic<bool> dirty{false};
    double dirty_at = 0;
    std::atomic<int> cli_busy{0};
    std::string message;           // red/green one-liner (folder errors, ...)
    bool message_bad = false;
    std::deque<std::string> log;
    std::string partial;
    bool log_follow = true;
    std::atomic<bool> running{false};
    HANDLE job = nullptr;
    int crash_base = -1;           // crash reports saved when the game was started (-1: no game started yet)
    std::string crash_new;         // path of the report saved by the session that just ended ("" = none / dismissed)
    std::string tab = "play";      // the page shown (the id of the sidebar item)
    json changed = json::object(); // option edits not saved yet: {id: value}, sent as "set"
    SDL_Texture* banner = nullptr; // sce_sys/pic1.png of the player's own game folder (never bundled)
    std::string banner_path;
    float banner_w = 0, banner_h = 0;
    json online = json::object();  // newest answer of `bbvr_cli.py online view` (UI thread owns it)
    json online_incoming;
    bool online_has_incoming = false, online_have = false;
    std::string online_msg;        // result of the last create / join / leave
    bool online_msg_bad = false;
    double online_poll_at = -100;
    char online_code[16] = "";
    bool alpha_ack = false;         // OK pressed on the early-alpha notice (the state catches up a moment later)
    bool lobby_edit = false;        // the small "Change" under the room controls: show the lobby address field
    char online_addr[160] = "";     // lobby address typed by the player (never shipped; saved with `online server`)
    bool online_joined = false;     // a host / join worked: switch to the Co-op mode
    std::string demo_flow;          // screenshots only: room | running | noserver
    bool advanced = false;
    bool quit_confirm = false;
    SDL_Window* window = nullptr;
};
static App A;

static void log_add(const std::string& text)
{
    std::lock_guard<std::mutex> lock(A.mu);
    for (char c : text) {
        if (c == '\n' || c == '\r') {
            if (!A.partial.empty() || c == '\n') {
                if (c == '\n' || !A.partial.empty()) {
                    if (!(c == '\r' && A.partial.empty())) A.log.push_back(A.partial);
                }
                A.partial.clear();
            }
        } else
            A.partial += c;
    }
    while (A.log.size() > 6000) A.log.pop_front();  // the Log page keeps the last 6000 lines
}

static void cli_async(std::vector<std::string> args, std::string body = "")
{
    A.cli_busy++;
    std::thread([args, body] {
        std::string out;
        {
            std::lock_guard<std::mutex> lock(A.cli_mu);
            std::vector<std::wstring> wa = {A.env.script_dir + L"\\bbvr_cli.py"};
            for (auto& a : args) wa.push_back(W(a));
            run_capture(A.env, wa, body, [&](const std::string& c) { out += c; });
        }
        try {
            size_t brace = out.find('{');
            json j = json::parse(out.substr(brace == std::string::npos ? 0 : brace));
            std::lock_guard<std::mutex> lock(A.mu);
            A.incoming = j;
            A.has_incoming = true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(A.mu);
            A.message = "The settings helper did not answer: " + out.substr(0, 300);
            A.message_bad = true;
        }
        A.cli_busy--;
    }).detach();
}

// Room controls: `bbvr_cli.py online <args>` (create | join CODE | leave | view). Anything but `view` is followed by a
// `view`, whose answer fills the page; the first answer's message (errors) is shown under the buttons. A host / join
// that worked switches the mode to Co-op; the game is started with PLAY, never by the room buttons.
static void online_async(std::vector<std::string> args, bool joins = false)
{
    A.cli_busy++;
    std::thread([args, joins] {
        auto run = [](const std::vector<std::string>& a) {
            std::string out;
            std::vector<std::wstring> wa = {A.env.script_dir + L"\\bbvr_cli.py", L"online"};
            for (auto& x : a) wa.push_back(W(x));
            run_capture(A.env, wa, "", [&](const std::string& c) { out += c; });
            try {
                size_t brace = out.find('{');
                return json::parse(out.substr(brace == std::string::npos ? 0 : brace));
            } catch (...) {
                return json{{"ok", false}, {"message", "The settings helper did not answer: " + out.substr(0, 200)}};
            }
        };
        std::lock_guard<std::mutex> lock(A.cli_mu);
        json first = run(args);
        json view = args[0] == "view" ? first : run({"view"});
        std::lock_guard<std::mutex> l2(A.mu);
        if (args[0] != "view") {
            A.online_msg = first.value("ok", false) ? "" : (first.contains("message") && first["message"].is_string() ? first["message"].get<std::string>() : "That did not work.");
            A.online_msg_bad = !A.online_msg.empty();
            if (joins && first.value("ok", false)) A.online_joined = true;
        }
        A.online_incoming = view;
        A.online_has_incoming = true;
        A.cli_busy--;
    }).detach();
}

// What the next "save" sends: the option edits not saved yet ("set": only what the player changed, kept by the Python
// side as overrides over the quality preset) and whether the game runs (live files are then written at once).
static std::string default_save_body(bool with_changes)
{
    json b = json::object();
    if (with_changes && !A.changed.empty()) b["set"] = A.changed;
    if (with_changes) A.changed = json::object();
    b["game_running"] = A.running.load();
    return b.dump();
}

static void save_now(json body)  // a mode / quality / reset choice: sent at once, with the edits not saved yet
{
    if (!body.contains("quality") && !A.changed.empty()) body["set"] = A.changed;  // a preset drops the edits of what it sets anyway
    A.changed = json::object();
    body["game_running"] = A.running.load();
    A.dirty = false;
    cli_async({"save"}, body.dump());
}

static std::string cur_mode() { return A.st.value("mode", "vr"); }

static void start_game()
{
    if (A.running || !A.env.ok) return;
    {
        std::lock_guard<std::mutex> lock(A.mu);
        A.log.clear();
        A.partial.clear();
    }
    bool send_values = A.dirty.exchange(false);
    std::string body = default_save_body(send_values);
    std::string mode = cur_mode();
    bool coop_flat = mode == "coop" && A.st.value("coop_view", "vr") == "flat";
    A.crash_base = A.st.value("crash", json::object()).value("count", 0);
    A.crash_new.clear();
    A.running = true;
    if (A.job) CloseHandle(A.job);
    A.job = make_job();
    HANDLE job = A.job;
    std::string game = A.st.value("app", json::object()).value("game_dir", "");
    std::thread([body, job, game, mode, coop_flat] {
        {  // the launcher reads the saved settings: make sure the last edit is on disk first
            std::lock_guard<std::mutex> lock(A.cli_mu);
            std::string ignore;
            run_capture(A.env, {A.env.script_dir + L"\\bbvr_cli.py", L"save"}, body, [&](const std::string& c) { ignore += c; });
        }
        log_add(mode == "flat" ? "Starting (Flat).\n" : "Starting. Put the headset on when the game says so.\n");
        std::vector<std::wstring> args = {A.env.script_dir + L"\\bbvr_launcher.py", L"--no-menu", L"--mode", W(mode)};
        if (coop_flat) args.push_back(L"--flat");
        if (!game.empty()) {
            args.push_back(L"--game-dir");
            args.push_back(W(game));
        }
        SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
        SetEnvironmentVariableW(L"PYTHONUTF8", L"1");
        SetEnvironmentVariableW(L"BBVR_NATIVE", L"1");
        int code = run_capture(A.env, args, "", [](const std::string& c) { log_add(c); }, job);
        if (code == -1) log_add("Could not start Python.\n");
        else if (code) log_add("\nThe game ended with exit code " + std::to_string(code) + ". Please attach logs\\bbvr-latest.log to a bug report.\n");
        else log_add("\nThe game ended.\n");
        A.running = false;
        cli_async({"state"});
    }).detach();
}

static void stop_game()
{
    if (A.job) TerminateJobObject(A.job, 1);
}

// ---------------------------------------------------------------------------------------------
// theme: near-black browns, warm bone text, one gold accent, blood red for PLAY (muted text is kept light enough to
// read at small sizes: about 7:1 on the content background)

static ImU32 C(unsigned rgb, float a = 1.0f)
{
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(a * 255));
}
static ImVec4 CV(unsigned rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
enum : unsigned {
    BG = 0x0e0c0b,      // sidebar, bottom bar, window
    PANEL = 0x151210,   // content
    CARD = 0x1c1815,    // cards, fields
    CARD2 = 0x26211c,   // buttons, hover
    CARD3 = 0x322b25,   // pressed
    BORDER = 0x2e2722,
    TEXT = 0xe9e2d6,
    MUTED = 0xaa9f90,
    GOLD = 0xc8a96a,
    BLOOD = 0x7c1717,   // PLAY
    BLOOD_H = 0x9e2222,
    BLOOD_A = 0x5e1010,
    GREEN = 0x7fb88a,
    AMBER = 0xd9a441,
    ACCENT = 0xd36b5c   // errors
};

static ImFont *F_REG, *F_SEMI, *F_MONO, *F_SERIF, *F_SERIF_B;
static SDL_Renderer* g_renderer = nullptr;

static void setup_theme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = S(10);
    s.ChildRounding = S(10);
    s.FrameRounding = S(7);
    s.PopupRounding = S(8);
    s.GrabRounding = S(7);
    s.ScrollbarRounding = S(8);
    s.FrameBorderSize = 1;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 1;
    s.PopupBorderSize = 1;
    s.FramePadding = ImVec2(S(10), S(7));
    s.ItemSpacing = ImVec2(S(10), S(8));
    s.ScrollbarSize = S(10);
    s.GrabMinSize = S(14);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = CV(TEXT);
    c[ImGuiCol_TextDisabled] = CV(MUTED);
    c[ImGuiCol_WindowBg] = CV(BG);
    c[ImGuiCol_ChildBg] = CV(CARD);
    c[ImGuiCol_PopupBg] = CV(CARD);
    c[ImGuiCol_Border] = CV(BORDER);
    c[ImGuiCol_FrameBg] = CV(CARD2);
    c[ImGuiCol_FrameBgHovered] = CV(CARD3);
    c[ImGuiCol_FrameBgActive] = CV(0x3d342c);
    c[ImGuiCol_SliderGrab] = CV(GOLD);
    c[ImGuiCol_SliderGrabActive] = CV(0xdcc489);
    c[ImGuiCol_CheckMark] = CV(GOLD);
    c[ImGuiCol_Button] = CV(CARD2);
    c[ImGuiCol_ButtonHovered] = CV(CARD3);
    c[ImGuiCol_ButtonActive] = CV(0x3d342c);
    c[ImGuiCol_Header] = CV(CARD3);
    c[ImGuiCol_HeaderHovered] = CV(0x3d342c);
    c[ImGuiCol_HeaderActive] = CV(0x4a3f35);
    c[ImGuiCol_ScrollbarBg] = CV(PANEL, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = CV(0x3a322b);
    c[ImGuiCol_ScrollbarGrabHovered] = CV(0x4d4238);
    c[ImGuiCol_ScrollbarGrabActive] = CV(0x5e5144);
    c[ImGuiCol_Separator] = CV(BORDER);
    c[ImGuiCol_TableHeaderBg] = CV(CARD2);
    c[ImGuiCol_TableBorderStrong] = CV(BORDER);
    c[ImGuiCol_TableBorderLight] = CV(BORDER, 0.6f);
    c[ImGuiCol_TableRowBgAlt] = CV(0xffffff, 0.02f);
    c[ImGuiCol_ModalWindowDimBg] = CV(0x050403, 0.78f);
    c[ImGuiCol_TextSelectedBg] = CV(BLOOD_H, 0.45f);
}

static bool load_fonts(float px)
{
    ImGuiIO& io = ImGui::GetIO();
    wchar_t win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    std::string dir = U8(win) + "\\Fonts\\";
    auto load = [&](const char* name) -> ImFont* {
        std::string p = dir + name;
        if (!file_exists(W(p))) return nullptr;
        return io.Fonts->AddFontFromFileTTF(p.c_str(), px);
    };
    F_REG = load("segoeui.ttf");
    F_SEMI = load("seguisb.ttf");
    F_MONO = load("consola.ttf");
    F_SERIF = load("georgia.ttf");
    F_SERIF_B = load("georgiab.ttf");
    bool sys = F_REG != nullptr;
    if (!F_REG) F_REG = io.Fonts->AddFontDefault();
    if (!F_SEMI) F_SEMI = F_REG;
    if (!F_MONO) F_MONO = F_REG;
    if (!F_SERIF) F_SERIF = F_SEMI;
    if (!F_SERIF_B) F_SERIF_B = F_SERIF;
    return sys;
}

// ---------------------------------------------------------------------------------------------
// widgets

static bool BeginCard(const char* id, ImVec2 size, unsigned border = BORDER, bool auto_height = false)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, CV(CARD));
    ImGui::PushStyleColor(ImGuiCol_Border, CV(border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(10));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    ImGuiChildFlags f = ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding;
    if (auto_height) f |= ImGuiChildFlags_AutoResizeY;
    return ImGui::BeginChild(id, size, f, auto_height ? (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse) : 0);
}
static void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static void Heading(const char* t)  // small gold caps over a card's content
{
    ImGui::PushFont(F_SEMI, S(12.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(GOLD));
    std::string up;
    for (const char* p = t; *p; p++) up += (char)toupper((unsigned char)*p);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, S(2)));
}

static void Muted(const char* t, float size = 13)
{
    ImGui::PushFont(F_REG, S(size));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
static void MutedWrapped(const std::string& t, float size = 13, float wrap = 0.0f)
{
    ImGui::PushFont(F_REG, S(size));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
    ImGui::PushTextWrapPos(wrap > 0 ? ImGui::GetCursorPosX() + wrap : 0.0f);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
static void Colored(const std::string& t, unsigned color, float size = 13)
{
    ImGui::PushFont(F_REG, S(size));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(color));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(t.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}
static void SerifTitle(const char* t, float size, unsigned color = TEXT)
{
    ImGui::PushFont(F_SERIF_B, S(size));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(color));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static std::string Fit(const std::string& s, float w)
{
    if (ImGui::CalcTextSize(s.c_str()).x <= w) return s;
    size_t n = s.size();
    while (n > 4 && ImGui::CalcTextSize((s.substr(0, n) + "...").c_str()).x > w) n--;
    return s.substr(0, n) + "...";
}

static bool Toggle(const char* id, bool* v, bool faded = false)
{
    float h = S(22), w = S(40);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(w, h));
    bool hot = ImGui::IsItemHovered();
    bool pressed = ImGui::IsItemClicked();
    if (pressed) *v = !*v;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float a = faded ? 0.55f : 1.0f;
    ImU32 bg = *v ? C(BLOOD_H, a) : C(hot ? 0x3d342c : 0x2d2620, a);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), *v ? C(GOLD, 0.7f * a) : C(0x4a4036, a), h * 0.5f);
    float r = h * 0.5f - S(3);
    dl->AddCircleFilled(ImVec2(*v ? p.x + w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f), r, C(0xf4ece0, *v ? 1.0f : 0.75f));
    return pressed;
}

static bool Button(const char* label, ImVec2 size, bool primary = false, bool outline = false)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(7));
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, CV(BLOOD));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CV(BLOOD_H));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CV(BLOOD_A));
        ImGui::PushStyleColor(ImGuiCol_Border, CV(GOLD, 0.35f));
    } else if (outline) {
        ImGui::PushStyleColor(ImGuiCol_Button, CV(CARD));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CV(CARD2));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CV(CARD3));
        ImGui::PushStyleColor(ImGuiCol_Border, CV(0x4f443a));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, CV(CARD2));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CV(CARD3));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CV(0x3d342c));
        ImGui::PushStyleColor(ImGuiCol_Border, CV(BORDER));
    }
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return r;
}

// One choice of a row of choices (mode, quality, render mode): a framed cell with a title and an optional second line.
// The selected cell has a gold frame and a faint blood tint.
static bool Choice(const char* id, const char* title, const std::string& line1, const std::string& line2, ImVec2 size,
                   bool selected, float title_px = 17, bool serif = true, const std::string& line3 = "", bool disabled = false,
                   const std::string& tip = "")
{
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool hot = ImGui::IsItemHovered() && !disabled;
    bool clicked = ImGui::IsItemClicked() && !disabled;
    if (disabled && !tip.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip.c_str());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 end(pos.x + size.x, pos.y + size.y);
    dl->AddRectFilled(pos, end, selected ? C(BLOOD, 0.30f) : C(hot ? CARD3 : CARD2), S(9));
    dl->AddRect(pos, end, selected ? C(GOLD) : C(hot ? 0x5a4d40 : BORDER), S(9), 0, selected ? 2.0f : 1.0f);
    ImFont* f = serif ? F_SERIF_B : F_SEMI;
    float ty = pos.y + S(9);
    dl->AddText(f, S(title_px), ImVec2(pos.x + S(12), ty), selected ? C(0xf4ece0) : C(TEXT), title);
    ty += S(title_px) + S(5);
    if (!line1.empty()) {
        dl->AddText(F_REG, S(13), ImVec2(pos.x + S(12), ty), selected ? C(GOLD) : C(MUTED), Fit(line1, size.x - S(20)).c_str());
        ty += S(17);
    }
    if (!line2.empty()) {
        dl->AddText(F_REG, S(13), ImVec2(pos.x + S(12), ty), C(MUTED), Fit(line2, size.x - S(20)).c_str());
        ty += S(17);
    }
    if (!line3.empty()) dl->AddText(F_REG, S(12), ImVec2(pos.x + S(12), ty), C(selected ? GOLD : MUTED), line3.c_str(), nullptr, size.x - S(20));
    if (disabled) dl->AddRectFilled(pos, end, C(PANEL, 0.55f), S(9));  // greyed out
    return clicked;
}

static void Dot(unsigned color, float cy_offset = 0)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = S(5.5f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c(p.x + r, p.y + S(11) + cy_offset);
    dl->AddCircleFilled(c, r + S(3), C(color, 0.18f));
    dl->AddCircleFilled(c, r, C(color));
}

static void open_url(const std::string& url)
{
    SDL_OpenURL(url.c_str());
}

static void open_folder(const std::string& utf8_path)
{
    if (utf8_path.empty()) return;
    std::wstring p = W(utf8_path);
    ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static std::string jstr(const json& j, const char* k, const std::string& d = "")
{
    return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : d;
}

static void folder_cb(void*, const char* const* list, int)
{
    if (list && list[0]) cli_async({"game-dir", list[0]});
}

// ---------------------------------------------------------------------------------------------
// small state helpers

static bool dlss_ok()
{
    return A.st.value("caps", json::object()).value("dlss", json::object()).value("ok", false);
}
static std::string cur_view()  // "vr" or "flat": what the game window is (flat mode, or co-op played flat)
{
    std::string m = cur_mode();
    if (m == "flat") return "flat";
    if (m == "coop" && A.st.value("coop_view", "vr") == "flat") return "flat";
    return "vr";
}
static const json* find_option(const std::string& id)
{
    if (!A.st.contains("options")) return nullptr;
    for (const json& o : A.st["options"])
        if (o.value("id", "") == id) return &o;
    return nullptr;
}
static std::string option_label(const std::string& id)
{
    const json* o = find_option(id);
    return o ? jstr(*o, "label", id) : id;
}
static bool is_edited(const std::string& id)
{
    if (!A.st.contains("overrides")) return false;
    for (const json& x : A.st["overrides"])
        if (x.is_string() && x.get<std::string>() == id) return true;
    return false;
}
static json value_of(const std::string& id)
{
    return A.st.contains("values") && A.st["values"].contains(id) ? A.st["values"][id] : json();
}
static void note_change(const std::string& id)  // the value in A.st["values"][id] was edited: save it soon
{
    A.changed[id] = A.st["values"][id];
    A.dirty = true;
    A.dirty_at = ImGui::GetTime();
}
static void set_option(const std::string& id, const json& v)
{
    A.st["values"][id] = v;
    note_change(id);
}

static void set_mode(const char* m)
{
    if (cur_mode() == m) return;
    A.st["mode"] = m;
    save_now({{"mode", m}});
}
static void set_coop_view(const char* v)
{
    if (A.st.value("coop_view", "vr") == v) return;
    A.st["coop_view"] = v;
    save_now({{"coop_view", v}});
}
static void choose_quality(const std::string& q)
{
    if (A.st.value("quality", "High") == q && !A.st.contains("quality_modified")) return;
    A.changed = json::object();  // edits of what a preset sets are dropped by the preset anyway
    A.st["quality"] = q;
    A.st["quality_modified"] = json::array();
    save_now({{"quality", q}});
}

static const json& presets()
{
    static const json none = json::array();
    if (A.st.contains("quality_table") && A.st["quality_table"].contains("presets")) return A.st["quality_table"]["presets"];
    return none;
}
static std::string preset_row(const json& p, size_t i)
{
    if (!p.contains("rows") || p["rows"].size() <= i || p["rows"][i].size() < 2) return "";
    return p["rows"][i][1].get<std::string>();
}
// "DLSS quality" / "FSR 1" and "2880x1620 per eye" for a preset card
static std::string preset_line1(const json& p)
{
    std::string up = preset_row(p, 0), mode = preset_row(p, 1);
    if (mode == "performance") mode = "perf.";
    if (mode == "from the eye size") mode = "auto";
    return up == "DLSS" ? "DLSS " + mode : up;
}
static std::string preset_line2(const json& p) { return preset_row(p, 2); }  // the eye image, e.g. 2880x1620

// ---------------------------------------------------------------------------------------------
// ready check

struct Row {
    unsigned color;
    std::string title, detail;
    const char* button = nullptr;
    bool primary = false;
    int action = 0;
    bool hollow = false;
};

static std::vector<Row> ready_rows()
{
    std::vector<Row> rows;
    if (!A.st.contains("ready")) return rows;
    for (const json& r : A.st["ready"]) {
        Row x;
        std::string status = jstr(r, "status"), id = jstr(r, "id");
        x.color = status == "ok" ? GREEN : status == "warn" ? AMBER : status == "bad" ? ACCENT : MUTED;
        x.hollow = status == "na" || status == "info";
        x.title = jstr(r, "title");
        x.detail = jstr(r, "detail");
        if (id == "game") {
            x.button = status == "bad" ? "Choose folder" : "Change...";
            x.primary = status == "bad";
            x.action = 1;
        }
        rows.push_back(x);
    }
    if (!A.env.ok) rows.insert(rows.begin(), Row{ACCENT, "Launcher scripts", A.env.problem});
    return rows;
}

static void draw_rows(const std::vector<Row>& rows, float width)
{
    int i = 0;
    for (const Row& r : rows) {
        ImGui::PushID(i++);
        ImVec2 start = ImGui::GetCursorPos();
        if (r.hollow) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircle(ImVec2(p.x + S(5.5f), p.y + S(11)), S(5), C(r.color, 0.8f), 0, 1.5f);
        } else
            Dot(r.color);
        ImGui::SetCursorPos(ImVec2(start.x + S(24), start.y));
        ImGui::PushFont(F_SEMI, S(15));
        ImGui::TextUnformatted(r.title.c_str());
        ImGui::PopFont();
        float bw = r.button ? S(116) : 0;
        ImGui::SetCursorPos(ImVec2(start.x + S(24), start.y + S(20)));
        ImGui::PushFont(F_REG, S(13));
        ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
        ImGui::TextUnformatted(Fit(r.detail, width - S(24) - bw - S(12)).c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        if (r.button) {
            ImGui::SetCursorPos(ImVec2(start.x + width - bw, start.y + S(2)));
            if (Button(r.button, ImVec2(bw, S(30)), r.primary)) {
                if (r.action == 1) SDL_ShowOpenFolderDialog(folder_cb, nullptr, A.window, nullptr, false);
            }
        }
        ImGui::SetCursorPos(ImVec2(start.x, start.y + S(44)));
        ImGui::PopID();
    }
}

// ---------------------------------------------------------------------------------------------
// option widgets (one row: label and help on the left, the control on the right)

static bool draw_option(const json& o, json& values, bool compact = false)
{
    std::string id = o["id"], kind = o["kind"];
    bool changed = false;
    json& v = values[id];
    std::string off;
    if (o.contains("only_if"))
        for (auto it = o["only_if"].begin(); it != o["only_if"].end(); ++it)
            if (!(values.contains(it.key()) && values[it.key()] == it.value())) {
                std::string want = it.value().is_boolean() ? (it.value().get<bool>() ? "on" : "off") : it.value().dump();
                if (it.value().is_string()) want = it.value().get<std::string>();
                off = "needs " + option_label(it.key()) + " = " + want;
            }
    if (id.rfind("dlssnr", 0) == 0 && !A.st.value("caps", json::object()).value("dlssnr", json::object()).value("ok", true)) off = "DLSS-NR is not available";
    bool disabled = !off.empty();

    float total = ImGui::GetContentRegionAvail().x;
    float ctrl_w = std::min(S(300), total * 0.42f);
    float lab_w = total - ctrl_w - S(16);
    ImVec2 top = ImGui::GetCursorPos();

    ImGui::PushID(id.c_str());
    ImGui::BeginGroup();
    ImGui::PushFont(F_SEMI, S(15));
    ImGui::TextUnformatted(jstr(o, "label").c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    bool live = o.value("live", false);
    ImGui::PushFont(F_SEMI, S(11));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(live ? GOLD : MUTED));
    ImGui::TextUnformatted(live ? "LIVE" : "RESTART");
    ImGui::PopStyleColor();
    if (is_edited(id)) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, CV(AMBER));
        ImGui::TextUnformatted("EDITED");
        ImGui::PopStyleColor();
    }
    ImGui::PopFont();
    std::string help = compact ? "" : jstr(o, "help");
    if (disabled) help = (help.empty() ? "" : help + " ") + "(" + off + ")";
    if (!help.empty()) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + lab_w);
        ImGui::PushFont(F_REG, S(13));
        ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
        ImGui::TextUnformatted(help.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        ImGui::PopTextWrapPos();
    }
    ImGui::EndGroup();
    float lab_h = ImGui::GetItemRectSize().y;

    ImGui::SetCursorPos(ImVec2(top.x + total - ctrl_w, top.y));
    ImGui::SetNextItemWidth(ctrl_w);
    if (disabled) ImGui::BeginDisabled();
    ImGui::PushFont(F_REG, S(15));
    if (kind == "bool") {
        bool b = v.is_boolean() && v.get<bool>();
        ImGui::SetCursorPosX(top.x + total - S(40));
        if (Toggle("##t", &b, disabled)) {
            v = b;
            changed = true;
        }
    } else if (kind == "choice" || kind == "runtime") {
        std::vector<std::pair<std::string, std::string>> items;
        const json caps = A.st.value("caps", json::object());
        std::string cur = v.is_string() ? v.get<std::string>() : "";
        if (kind == "runtime") {
            std::string act = jstr(caps, "active_runtime");
            items.push_back({"", "System active runtime" + (act.empty() ? "" : " (" + act + ")")});
            if (caps.contains("runtimes"))
                for (auto& r : caps["runtimes"]) items.push_back({jstr(r, "path"), jstr(r, "name") + " [" + jstr(r, "kind") + "]" + (r.value("active", false) ? " (active)" : "")});
        } else {
            if (v.is_null()) items.push_back({"", "(game default)"});
            for (auto& c : o["choices"]) {
                std::string cs = c.get<std::string>(), label = cs == "default" ? "default (driver)" : cs;
                if (o.contains("labels") && o["labels"].contains(cs)) label = o["labels"][cs].get<std::string>();
                if (id == "upscale" && cs == "dlss" && !dlss_ok()) label = "DLSS (not available)";
                if (o.contains("adv_choices") && !A.advanced && cs != cur)  // older / test-only choices stay out of sight
                    if (std::find(o["adv_choices"].begin(), o["adv_choices"].end(), cs) != o["adv_choices"].end()) continue;
                items.push_back({cs, label});
            }
        }
        std::string curlabel = cur;
        for (auto& it : items)
            if (it.first == cur) curlabel = it.second;
        if (ImGui::BeginCombo("##c", Fit(curlabel, ctrl_w - S(40)).c_str())) {
            for (auto& it : items) {
                bool blocked = id == "upscale" && it.first == "dlss" && !dlss_ok();
                if (ImGui::Selectable(it.second.c_str(), it.first == cur, blocked ? ImGuiSelectableFlags_Disabled : 0)) {
                    v = it.first;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
    } else if (kind == "int" || kind == "float") {
        bool isint = kind == "int";
        double mn = o.value("min", 0.0), mx = o.value("max", 1.0), step = o.value("step", isint ? 1.0 : 0.01);
        if (v.is_null()) {
            if (Button("game default (click to set)", ImVec2(ctrl_w, S(32)))) {
                v = o["default"].is_null() ? mn : o["default"].get<double>();
                if (isint) v = (long long)v.get<double>();
                changed = true;
            }
        } else {
            bool clearable = o["default"].is_null();
            float sw = ctrl_w - (clearable ? S(34) : 0);
            ImGui::SetNextItemWidth(sw);
            if (isint) {
                int x = (int)v.get<double>();
                if (ImGui::SliderInt("##i", &x, (int)mn, (int)mx)) {
                    v = x;
                    changed = true;
                }
            } else {
                float x = (float)v.get<double>();
                const char* fmt = step < 0.0015 ? "%.3f" : (step < 0.05 ? "%.2f" : "%.1f");
                if (ImGui::SliderFloat("##f", &x, (float)mn, (float)mx, fmt)) {
                    x = (float)(std::round(x / step) * step);
                    v = (double)x;
                    changed = true;
                }
            }
            if (clearable) {
                ImGui::SameLine();
                if (Button("x", ImVec2(S(28), 0))) {
                    v = nullptr;
                    changed = true;
                }
            }
        }
    } else if (kind == "multiline") {
        static std::string buf;
        buf = v.is_string() ? v.get<std::string>() : "";
        buf.resize(buf.size() + 512);
        ImGui::InputTextMultiline("##m", &buf[0], buf.size(), ImVec2(ctrl_w, S(84)));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            v = std::string(buf.c_str());
            changed = true;
        }
    } else {  // text, floats
        char buf[512];
        std::string cur = v.is_string() ? v.get<std::string>() : "";
        snprintf(buf, sizeof(buf), "%s", cur.c_str());
        ImGui::InputTextWithHint("##s", "default", buf, sizeof(buf));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            if (buf[0]) v = std::string(buf);
            else v = nullptr;
            changed = true;
        }
    }
    ImGui::PopFont();
    if (disabled) ImGui::EndDisabled();
    float ctrl_h = ImGui::GetItemRectSize().y;
    ImGui::SetCursorPos(ImVec2(top.x, top.y + std::max(lab_h, ctrl_h) + S(compact ? 4 : 8)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + total, p.y), C(BORDER, 0.8f));
    ImGui::Dummy(ImVec2(0, S(compact ? 4 : 8)));
    ImGui::PopID();
    if (changed) note_change(id);
    return changed;
}

static bool option_visible(const json& o)
{
    if (o.value("advanced", false) && !A.advanced) return false;
    return true;
}

static void render_mode_control(float w);  // Stereo | Mono (| Alternate-eye in advanced), with the hint

// The sections of one page ("pages" of the state): a gold heading, then a card of option rows.
static void draw_sections(const json& page, const std::string& skip_id = "")
{
    int si = 0;
    for (const json& sec : page.value("sections", json::array())) {
        std::vector<const json*> shown;
        for (const json& idj : sec.value("options", json::array())) {
            const json* o = find_option(idj.get<std::string>());
            if (o && option_visible(*o)) shown.push_back(o);
        }
        if (shown.empty()) continue;
        ImGui::PushID(si++);
        Heading(jstr(sec, "title").c_str());
        if (BeginCard("sec", ImVec2(-FLT_MIN, 0), BORDER, true)) {
            for (const json* o : shown) {
                if (jstr(*o, "id") == "mode") {
                    render_mode_control(ImGui::GetContentRegionAvail().x);
                    ImGui::Dummy(ImVec2(0, S(6)));
                } else
                    draw_option(*o, A.st["values"]);
            }
        }
        EndCard();
        ImGui::Dummy(ImVec2(0, S(8)));
        ImGui::PopID();
    }
    (void)skip_id;
}

static const json* find_page(const std::string& id)
{
    if (!A.st.contains("pages")) return nullptr;
    for (const json& p : A.st["pages"])
        if (p.value("id", "") == id) return &p;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// banner: the key art of the player's OWN game folder (sce_sys/pic1.png), read when the window opens

static void ensure_banner()
{
    if (!g_renderer || !A.have_state) return;
    std::string path = jstr(A.st.value("app", json::object()).value("game", json::object()), "banner");
    if (path == A.banner_path) return;
    A.banner_path = path;
    if (A.banner) {
        SDL_DestroyTexture(A.banner);
        A.banner = nullptr;
    }
    if (path.empty()) return;
    SDL_Surface* surf = SDL_LoadPNG(path.c_str());
    if (!surf) return;
    A.banner = SDL_CreateTextureFromSurface(g_renderer, surf);
    A.banner_w = (float)surf->w;
    A.banner_h = (float)surf->h;
    SDL_DestroySurface(surf);
}

static void draw_banner(float w, float h)
{
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 e(p.x + w, p.y + h);
    const json game = A.st.value("app", json::object()).value("game", json::object());
    if (A.banner && A.banner_w > 0 && A.banner_h > 0) {
        float want = w / h, have = A.banner_w / A.banner_h;  // cover the box, crop the rest (a little above the middle)
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (have > want) {
            float k = want / have;
            uv0.x = (1 - k) * 0.5f;
            uv1.x = uv0.x + k;
        } else {
            float k = have / want;
            uv0.y = (1 - k) * 0.35f;
            uv1.y = uv0.y + k;
        }
        dl->AddImageRounded(ImTextureRef((ImTextureID)(intptr_t)A.banner), p, e, uv0, uv1, IM_COL32_WHITE, S(10));
    } else {
        dl->AddRectFilledMultiColor(p, e, C(0x2a0f0d), C(0x120d0b), C(0x0b0908), C(0x1d0c0a));
        dl->AddRect(p, e, C(BORDER), S(10));
    }
    // darken the lower part so the title reads on any picture
    dl->AddRectFilledMultiColor(ImVec2(p.x, p.y + h * 0.35f), e, C(0x0e0c0b, 0.0f), C(0x0e0c0b, 0.0f), C(0x0e0c0b, 0.92f), C(0x0e0c0b, 0.92f));
    dl->AddRect(p, e, C(BORDER), S(10));
    std::string sub;
    if (!jstr(game, "title_id").empty()) sub = jstr(game, "title_id") + "  -  game version " + jstr(game, "version");
    else sub = "Choose your game folder in the ready check";
    if (!A.banner) {  // the picture carries the game's own title; without it the window says it
        dl->AddText(F_SERIF_B, S(34), ImVec2(p.x + S(22), e.y - S(70)), C(0xf2ead9), "Bloodborne");
        dl->AddText(F_SERIF_B, S(34), ImVec2(p.x + S(22) + F_SERIF_B->CalcTextSizeA(S(34), FLT_MAX, 0, "Bloodborne ").x, e.y - S(70)), C(GOLD), "VR");
    }
    dl->AddText(F_REG, S(14), ImVec2(p.x + S(24), e.y - S(26)), C(GOLD), sub.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

// ---------------------------------------------------------------------------------------------
// the Play page pieces

static void mode_selector(float w)
{
    static const struct { const char* id; const char* label; const char* tag; } modes[] = {
        {"vr", "VR", "Solo, headset"}, {"coop", "Co-op", "Online"}, {"flat", "Flat", "Solo, monitor"}};
    float gap = S(8), cw = (w - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, gap);
        if (Choice((std::string("mode") + modes[i].id).c_str(), modes[i].label, modes[i].tag, "", ImVec2(cw, S(68)), cur_mode() == modes[i].id, 21)) set_mode(modes[i].id);
    }
    std::string note;
    for (const json& m : A.st.value("modes", json::array()))
        if (jstr(m, "id") == cur_mode()) note = jstr(m, "note");
    ImGui::Dummy(ImVec2(0, S(2)));
    MutedWrapped(note, 13, w);
}

static void coop_view_selector(float w)
{
    ImGui::PushFont(F_SEMI, S(14));
    ImGui::TextUnformatted("Play co-op in");
    ImGui::PopFont();
    float cw = std::min(S(130), (w - S(8)) / 2);
    std::string cv = A.st.value("coop_view", "vr");
    if (Choice("cvvr", "VR headset", "", "", ImVec2(cw, S(38)), cv == "vr", 15, false)) set_coop_view("vr");
    ImGui::SameLine(0, S(8));
    if (Choice("cvfl", "Flat screen", "", "", ImVec2(cw, S(38)), cv == "flat", 15, false)) set_coop_view("flat");
}

// DLSS-NR model file: NVIDIA's, not included, no download anywhere. "Find file..." copies the player's own copy next to the
// game exe (bbvr_cli.py nr-dll PATH); "What is this?" opens the local help page (bbvr_cli.py nr-help).
static void nr_file_cb(void*, const char* const* list, int)
{
    if (list && list[0]) cli_async({"nr-dll", list[0]});
}

static void nr_controls(float w)
{
    MutedWrapped("Super High needs the model file nvngx_dlssnr.dll. It is NVIDIA's and is NOT included with this mod; there is no download here. If you already have a copy on this PC, press Find file... and it is copied next to the game. Without it the game runs High.", 13, w);
    if (Button("Find file...", ImVec2(S(130), S(30)))) {
        static const SDL_DialogFileFilter filters[] = {{"DLSS-NR model (nvngx_dlssnr.dll)", "dll"}};
        SDL_ShowOpenFileDialog(nr_file_cb, nullptr, A.window, filters, 1, nullptr, false);
    }
    ImGui::SameLine();
    if (Button("What is this?", ImVec2(S(130), S(30)), false, true)) cli_async({"nr-help"});
    const json r = A.st.value("nr_dll", json::object());
    if (r.contains("message")) Colored(jstr(r, "message"), r.value("ok", false) && r.value("available", true) ? GREEN : AMBER, 12.5f);
    std::string notice = jstr(A.st, "quality_notice");
    if (!notice.empty() && !r.contains("message")) Colored(notice, AMBER, 12.5f);
}

static void preset_cards(float w)
{
    const json& ps = presets();
    if (ps.empty()) return;
    std::string cur = A.st.value("quality", "High");
    bool modified = A.st.contains("quality_modified") && !A.st["quality_modified"].empty();
    float gap = S(8), cw = (w - (ps.size() - 1) * gap) / (float)ps.size();
    int i = 0;
    for (const json& p : ps) {
        std::string name = jstr(p, "name");
        if (i++) ImGui::SameLine(0, gap);
        bool sel = name == cur, avail = p.value("available", true);
        // five cards: the name, the eye image and the measured fps; the table on the Graphics page has the rest
        std::string l3 = !avail ? "needs your own nvngx_dlssnr.dll (not included)" : jstr(p, "fps");
        if (Choice(("q" + name).c_str(), name.c_str(), sel && modified ? "custom (edited)" : preset_line2(p), "", ImVec2(cw, S(112)), sel, 14, true, l3, !avail, jstr(p, "reason")))
            choose_quality(name);
    }
}

static void render_mode_control(float w)
{
    ImGui::PushFont(F_SEMI, S(15));
    ImGui::TextUnformatted("VR render mode");
    ImGui::PopFont();
    std::string cur = jstr(A.st.value("values", json::object()), "mode", "stereo");
    struct Item { const char* id; const char* label; };
    std::vector<Item> items = {{"stereo", "Stereo"}, {"mono", "Mono"}};
    if (A.advanced || cur == "alt") items.push_back({"alt", "Alternate-eye"});
    float gap = S(8), cw = std::min(S(150), (w - gap * (items.size() - 1)) / items.size());
    for (size_t i = 0; i < items.size(); i++) {
        if (i) ImGui::SameLine(0, gap);
        if (Choice((std::string("rm") + items[i].id).c_str(), items[i].label, "", "", ImVec2(cw, S(38)), cur == items[i].id, 15, false) && cur != items[i].id)
            set_option("mode", items[i].id);
    }
    std::string hint = cur == "stereo" ? "True per-eye 3D (both eyes rendered). Switch to Mono if you get eye strain or performance problems."
                     : cur == "mono"   ? "One flat screen in the headset: use if you get eye strain or performance problems."
                                       : "The older alternate-eye method, kept for testing.";
    MutedWrapped(hint, 13, w);
}

static bool online_in_room() { return A.online.value("in_room", false) && !A.online.value("closed", false); }
static bool online_no_server() { return A.online_have && A.online.value("error", "") == "no_server"; }

static void poll_online()
{
    if (A.cli_busy == 0 && ImGui::GetTime() - A.online_poll_at > 3.0) {
        A.online_poll_at = ImGui::GetTime();
        online_async({"view"});
    }
}

static void copy_code_button(const std::string& code, float w)
{
    if (Button("Copy code", ImVec2(w, S(34)), true)) {
        ImGui::SetClipboardText(code.c_str());
        std::lock_guard<std::mutex> lock(A.mu);
        A.online_msg = "Code copied.";
        A.online_msg_bad = false;
    }
}

static void big_code(const std::string& code, float size)
{
    ImGui::PushFont(F_MONO, S(size));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(TEXT));
    ImGui::TextUnformatted(code.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

// The lobby address is a setting, never shipped: ask for it where the room is created / joined.
static void lobby_address_field(bool busy)
{
    MutedWrapped("Lobby address of your own server (http://address:port).", 13);
    ImGui::SetNextItemWidth(S(230));
    ImGui::InputTextWithHint("##lobby", "http://address:port", A.online_addr, sizeof(A.online_addr), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    if (busy || !A.online_addr[0]) ImGui::BeginDisabled();
    if (Button("Save", ImVec2(S(80), S(32)), true)) online_async({"server", A.online_addr});
    if (busy || !A.online_addr[0]) ImGui::EndDisabled();
}

// The release comes with a default lobby (defaults/release.json): nothing to type. A small line shows which lobby is
// used, with a way to enter another one (own server) and to go back to the default.
static void lobby_line(bool busy)
{
    if (!A.online_have || !A.online.contains("server") || jstr(A.online, "server").empty()) return;
    const bool is_default = A.online.value("is_default", false);
    ImGui::Dummy(ImVec2(0, S(6)));
    Muted(is_default ? "Lobby: default" : "Lobby: your own");
    ImGui::SameLine();
    if (Button(A.lobby_edit ? "Hide" : "Change", ImVec2(S(80), S(24)), false, true)) A.lobby_edit = !A.lobby_edit;
    if (!A.lobby_edit) return;
    ImGui::SetNextItemWidth(S(230));
    ImGui::InputTextWithHint("##lobbyown", "http://address:port", A.online_addr, sizeof(A.online_addr), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    if (busy || !A.online_addr[0]) ImGui::BeginDisabled();
    if (Button("Use", ImVec2(S(60), S(32)), true)) online_async({"server", A.online_addr});
    if (busy || !A.online_addr[0]) ImGui::EndDisabled();
    if (!is_default) {
        if (busy) ImGui::BeginDisabled();
        if (Button("Use the default lobby", ImVec2(S(190), S(30)), false, true)) {
            A.online_addr[0] = 0;
            online_async({"server", "default"});
        }
        if (busy) ImGui::EndDisabled();
    }
}

static void online_messages()
{
    std::lock_guard<std::mutex> lock(A.mu);
    if (!A.online_msg.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, CV(A.online_msg_bad ? ACCENT : GREEN));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(A.online_msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
}

// Host a game / Join with a code, or the room that exists (code, Copy, Leave). Used on the Play page (Co-op) and on the
// Online co-op page. The room belongs to the lobby; the game is started with PLAY.
static void room_panel(float w)
{
    const bool busy = A.cli_busy > 0;
    const json& v = A.online;
    if (!A.online_have) {
        Muted("Reading the room state...");
    } else if (online_in_room()) {
        std::string code = jstr(v, "code");
        big_code(code, 38);
        MutedWrapped(jstr(v, "role") == "host" ? "You host this room. Give the code to up to two friends, then press PLAY." : "You are a guest in this room. Press PLAY when the host is ready.", 13, w);
        int n = (int)v.value("members", json::array()).size();
        if (n > 0) Muted((std::to_string(n) + " of " + std::to_string(v.value("max", 3)) + " in the room").c_str());
        ImGui::Dummy(ImVec2(0, S(4)));
        copy_code_button(code, S(120));
        ImGui::SameLine();
        if (busy) ImGui::BeginDisabled();
        if (Button(jstr(v, "role") == "host" ? "Close room" : "Leave", ImVec2(S(110), S(34)), false, true)) online_async({"leave"});
        if (busy) ImGui::EndDisabled();
    } else if (online_no_server()) {
        lobby_address_field(busy);
    } else {
        if (busy) ImGui::BeginDisabled();
        if (Button("Host a game", ImVec2(S(170), S(38)), true)) online_async({"create"}, true);
        if (busy) ImGui::EndDisabled();
        ImGui::SameLine();
        Muted("Opens a room and shows its code.");
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PushFont(F_SEMI, S(14));
        ImGui::TextUnformatted("or join a friend");
        ImGui::PopFont();
        ImGui::SetNextItemWidth(S(150));
        ImGui::PushFont(F_MONO, S(20));
        ImGui::InputTextWithHint("##startcode", "K7Q-M4X", A.online_code, sizeof(A.online_code), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
        ImGui::PopFont();
        ImGui::SameLine();
        bool off = busy || !A.online_code[0];
        if (off) ImGui::BeginDisabled();
        if (Button("Join", ImVec2(S(80), S(34)), false, true)) online_async({"join", A.online_code}, true);
        if (off) ImGui::EndDisabled();
        lobby_line(busy);
    }
    if (busy) Muted("Working...");
    online_messages();
    (void)w;
}

static void play_display_row();

static void play_left(float lw, bool vr)  // ready check, Display size and quick settings
{
    if (BeginCard("ready", ImVec2(lw, 0), BORDER, true)) {
        Heading("Ready check");
        draw_rows(ready_rows(), lw - S(34));
    }
    EndCard();
    if (vr && A.have_state && A.st.contains("options")) {
        if (BeginCard("display", ImVec2(lw, 0), BORDER, true)) {
            Heading("Display size");
            play_display_row();
        }
        EndCard();
    }
    if (A.have_state && A.st.contains("options")) {
        if (BeginCard("quick", ImVec2(lw, 0), BORDER, true)) {
            Heading("Quick settings");
            if (vr) {
                for (const char* id : {"fps", "fps_counter", "hud_center"}) {
                    const json* o = find_option(id);
                    if (o) draw_option(*o, A.st["values"], true);
                }
            } else {
                const json* o = find_option("flat_fps");
                if (o) draw_option(*o, A.st["values"], true);
            }
        }
        EndCard();
    }
}

static void play_right(float rw, const std::string& mode, bool vr)  // mode, room, quality, render mode
{
    if (BeginCard("mode", ImVec2(rw, 0), BORDER, true)) {
        Heading("Mode");
        mode_selector(rw - S(34));
        if (mode == "coop") {
            ImGui::Dummy(ImVec2(0, S(8)));
            coop_view_selector(rw - S(34));
        }
    }
    EndCard();
    if (mode == "coop") {
        if (BeginCard("room", ImVec2(rw, 0), online_in_room() ? GOLD : BORDER, true)) {
            Heading(online_in_room() ? (jstr(A.online, "role") == "host" ? "Your room (host)" : "Your room (guest)") : "Online room");
            room_panel(rw - S(34));
        }
        EndCard();
    }
    if (vr) {
        if (BeginCard("quality", ImVec2(rw, 0), BORDER, true)) {
            ImVec2 head = ImGui::GetCursorPos();
            Heading("VR quality");
            ImGui::SetCursorPos(ImVec2(head.x + rw - S(34) - S(200), head.y - S(5)));
            if (Button("What each preset sets", ImVec2(S(200), S(24)), false, true)) A.tab = "graphics";
            ImGui::SetCursorPos(ImVec2(head.x, head.y + S(26)));
            preset_cards(rw - S(34));
            ImGui::Dummy(ImVec2(0, S(2)));
            std::string cur = A.st.value("quality", "High");
            std::string note;
            for (const json& p : presets())
                if (jstr(p, "name") == cur) note = jstr(p, "note");
            MutedWrapped(note, 13, rw - S(34));
            for (const json& p : presets())
                if (!p.value("available", true)) Colored(jstr(p, "name") + " is greyed out: " + jstr(p, "reason"), AMBER, 12.5f);
            ImGui::Dummy(ImVec2(0, S(4)));
            nr_controls(rw - S(34));
            ImGui::Dummy(ImVec2(0, S(8)));
            render_mode_control(rw - S(34));
        }
        EndCard();
    } else {
        if (BeginCard("flatnote", ImVec2(rw, 0), BORDER, true)) {
            Heading("Flat screen");
            MutedWrapped("The game opens in its own window on this PC. No headset or VR runtime is needed. Graphics are set in the game's own options menu, not here; the quality presets are for VR.", 13, rw - S(34));
        }
        EndCard();
    }
}

// Play tab, left column under "This PC": Display size (docs/vr/findings/41-display-size.md). A headset combo (Auto / Game
// default / presets / Custom WxH) and resolution steps; the line under them shows what the game gets (render size ->
// eye image). The headset is a hardware choice kept over every profile and preset (bbvr_settings STICKY: settings["hw"]);
// a resolution step is a normal option, kept as an override of the quality preset (Low / Medium start at 70 / 85 %).
static void play_save_value(const char* key, const json& value, bool /*sticky*/)
{
    json b = json::object();
    b["set"] = A.changed.is_object() ? A.changed : json::object();
    b["set"][key] = value;
    b["game_running"] = A.running.load();
    A.changed = json::object();
    A.dirty = false;
    cli_async({"save"}, b.dump());
    A.st["values"][key] = value;
}

static std::string size_text(const json& s)
{
    return s.is_array() && s.size() == 2 ? std::to_string(s[0].get<int>()) + "x" + std::to_string(s[1].get<int>()) : "";
}

static void play_display_row()
{
    if (!A.st.contains("values") || !A.st.contains("options")) return;
    const json* opt = nullptr;
    for (const json& o : A.st["options"])
        if (jstr(o, "id") == "display_size") opt = &o;
    if (!opt) return;  // an older bbvr_cli without Display size
    const json values = A.st["values"];
    const std::string cur = jstr(values, "display_size", "auto");
    const json disp = A.st.value("display", json::object());
    const json det = disp.value("detected", json::object());
    const json sizes = disp.value("sizes", json());

    // headset combo
    std::string auto_label = "Auto";
    if (!jstr(det, "name").empty())
        auto_label += ": " + jstr(det, "name") + (jstr(det, "source") == "log" ? " (last session)" : " (typical for " + jstr(det, "runtime_label") + ")");
    else
        auto_label += " (headset not known yet: 1080p frame)";
    std::vector<std::pair<std::string, std::string>> items;
    for (auto& c : (*opt)["choices"]) {
        std::string cs = c.get<std::string>(), label = cs;
        if (opt->contains("labels") && (*opt)["labels"].contains(cs)) label = (*opt)["labels"][cs].get<std::string>();
        if (cs == "auto") label = auto_label;
        items.push_back({cs, label});
    }
    std::string curlabel = cur;
    for (auto& it : items)
        if (it.first == cur) curlabel = it.second;
    const float full = ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(full);
    ImGui::PushFont(F_REG, S(15));
    if (ImGui::BeginCombo("##dispsize", Fit(curlabel, full - S(40)).c_str())) {
        for (auto& it : items)
            if (ImGui::Selectable(it.second.c_str(), it.first == cur) && it.first != cur) play_save_value("display_size", it.first, true);
        ImGui::EndCombo();
    }
    ImGui::PopFont();
    if (cur == "custom") {  // the headset's per-eye size, WxH
        static char buf[32];
        static bool editing = false;
        if (!editing) snprintf(buf, sizeof(buf), "%s", jstr(values, "display_custom").c_str());
        ImGui::SetNextItemWidth(full);
        ImGui::PushFont(F_REG, S(15));
        ImGui::InputTextWithHint("##dispcustom", "per-eye size, e.g. 2064x2208", buf, sizeof(buf));
        editing = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) play_save_value("display_custom", buf[0] ? json(std::string(buf)) : json(), true);
        ImGui::PopFont();
    }
    // resolution steps (all six are in Options > Display / VR)
    if (cur != "game") {
        static const char* steps[] = {"70", "85", "100", "125"};
        const std::string scale = jstr(values, "display_scale", "100");
        const float w = (full - 3 * S(6)) / 4;
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine(0, S(6));
            const std::string label = std::string(steps[i]) + "%";
            if (Button(label.c_str(), ImVec2(w, S(28)), scale == steps[i]) && scale != steps[i]) play_save_value("display_scale", steps[i], false);
        }
    }
    std::string line;
    if (sizes.is_object() && sizes.contains("render")) {
        line = "Render " + size_text(sizes["render"]) + (sizes.value("native", false) ? " (game's own)" : "") + " -> eye " +
               size_text(sizes["eye"]) + " (" + jstr(sizes, "upscale") + "). Lower % = more fps.";
    } else if (cur == "game") {
        line = "1080p frame; the eye image is the preset's (or Eye resolution on the Graphics page).";
    } else if (cur == "custom") {
        line = "Type the per-eye size your headset software reports (WxH).";
    } else {
        line = "Start the game once and Auto knows your headset; or pick it here.";
    }
    MutedWrapped(line, 13, full);
    MutedWrapped("The quality preset picks the upscaler, DLSS-NR and a starting resolution step; Display size and the % set the eye image for your headset and win over the preset's own size.", 12.5f, full);
}

static void tab_play(ImVec2 avail)
{
    ImGui::BeginChild("playscroll", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    float w = ImGui::GetContentRegionAvail().x;
    float bh = std::clamp(avail.y * 0.24f, S(120), S(180));
    draw_banner(w, bh);
    ImGui::Dummy(ImVec2(0, S(4)));
    const std::string mode = cur_mode(), view = cur_view();
    const bool vr = view == "vr";
    if (w < S(780)) {  // a narrow window: one column, the mode and the quality first
        play_right(w, mode, vr);
        play_left(w, vr);
    } else {
        float gap = S(16), lw = std::floor((w - gap) * 0.43f), rw = w - gap - lw;
        ImGui::BeginGroup();
        play_left(lw, vr);
        ImGui::EndGroup();
        ImGui::SameLine(0, gap);
        ImGui::BeginGroup();
        play_right(rw, mode, vr);
        ImGui::EndGroup();
    }
    ImGui::Dummy(ImVec2(0, S(10)));
    ImGui::EndChild();
}

// ---------------------------------------------------------------------------------------------
// option pages

static void quality_table_view()
{
    const json& ps = presets();
    if (ps.empty()) return;
    std::string cur = A.st.value("quality", "High");
    if (ImGui::BeginTable("qt", 1 + (int)ps.size(), ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_WidthStretch, 1.3f);
        for (const json& p : ps) ImGui::TableSetupColumn(jstr(p, "name").c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        size_t rows = ps[0].contains("rows") ? ps[0]["rows"].size() : 0;
        for (size_t r = 0; r < rows; r++) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushFont(F_REG, S(14));
            ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
            ImGui::TextUnformatted(ps[0]["rows"][r][0].get<std::string>().c_str());
            ImGui::PopStyleColor();
            for (const json& p : ps) {
                ImGui::TableNextColumn();
                bool sel = jstr(p, "name") == cur;
                ImGui::PushStyleColor(ImGuiCol_Text, CV(sel ? GOLD : TEXT));
                ImGui::TextUnformatted(preset_row(p, r).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopFont();
        }
        ImGui::EndTable();
    }
}

static void page_header_graphics()
{
    bool modified = A.st.contains("quality_modified") && !A.st["quality_modified"].empty();
    Heading("VR quality");
    if (BeginCard("gq", ImVec2(-FLT_MIN, 0), BORDER, true)) {
        float w = ImGui::GetContentRegionAvail().x;
        preset_cards(w);
        ImGui::Dummy(ImVec2(0, S(8)));
        quality_table_view();
        ImGui::Dummy(ImVec2(0, S(6)));
        nr_controls(w);
        ImGui::Dummy(ImVec2(0, S(6)));
        std::string tail = A.st.contains("quality_table") && !A.st["quality_table"].value("dlss", false)
                               ? "DLSS is not available on this PC, so the presets use FSR 1. "
                               : "";
        MutedWrapped(tail + "The scene is always rendered at 1920x1080; the upscaler makes the eye image, so a bigger eye image looks sharper and costs more GPU time. The frame limit is not part of a preset (Display & FPS). DLSS-NR is off except in Super High.", 13, w);
        if (modified) {
            ImGui::Dummy(ImVec2(0, S(6)));
            std::string list;
            for (const json& k : A.st["quality_modified"]) list += (list.empty() ? "" : ", ") + option_label(k.get<std::string>());
            Colored("Edited below: " + list + ".", AMBER);
            std::string reset_label = "Reset to " + A.st.value("quality", "High") + " preset";
            if (Button(reset_label.c_str(), ImVec2(S(220), S(30)), false, true)) {
                json ids = A.st["quality_modified"];
                A.st["quality_modified"] = json::array();
                save_now({{"reset", ids}});
            }
        }
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, S(8)));
}

static void page_options(const std::string& id, bool graphics = false)
{
    const json* page = find_page(id);
    if (!page || !A.st.contains("values")) return;
    if (graphics) {
        if (!dlss_ok()) {
            const json dl = A.st.value("caps", json::object()).value("dlss", json::object());
            Colored("DLSS is not available: " + jstr(dl, "reason", "not available") + " FSR 1 is used instead.", AMBER);
            ImGui::Dummy(ImVec2(0, S(4)));
        }
        page_header_graphics();
    }
    draw_sections(*page);
    if (!A.advanced) Muted("Switch on \"Show advanced options\" in the sidebar for more.");
}

static void update_switch();

// One settings row: title and detail on the left, a control of ctrl_w on the right.
template <class F>
static void setting_row(const char* id, const char* title, const std::string& detail, float ctrl_w, F control)
{
    ImGui::PushID(id);
    ImVec2 top = ImGui::GetCursorPos();
    float total = ImGui::GetContentRegionAvail().x;
    float lab_w = total - ctrl_w - S(16);
    ImGui::BeginGroup();
    ImGui::PushFont(F_SEMI, S(15));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    if (!detail.empty()) MutedWrapped(detail, 13, lab_w);
    ImGui::EndGroup();
    float lab_h = ImGui::GetItemRectSize().y;
    ImGui::SetCursorPos(ImVec2(top.x + total - ctrl_w, top.y));
    ImGui::SetNextItemWidth(ctrl_w);
    control();
    float ctrl_h = ImGui::GetItemRectSize().y;
    ImGui::SetCursorPos(ImVec2(top.x, top.y + std::max(lab_h, ctrl_h) + S(16)));
    ImGui::PopID();
}

static void tab_advanced()
{
    const json* page = find_page("advanced");
    if (page) draw_sections(*page);
    Heading("Application");
    if (BeginCard("app", ImVec2(-FLT_MIN, 0), BORDER, true)) {
        const json app = A.st.value("app", json::object());
        setting_row("gamedir", "Game folder", jstr(app, "game_dir").empty() ? "Not set." : jstr(app, "game_dir"), S(130), [] {
            if (Button("Change...", ImVec2(S(130), S(32)))) SDL_ShowOpenFolderDialog(folder_cb, nullptr, A.window, nullptr, false);
        });
        setting_row("profile", "Older profile",
                    "The profiles of earlier versions. Comfort sets snap turning and a steadier camera; the quality presets on the Play page replace Performance and Quality (DLSS).",
                    S(260), [] {
                        std::string cur = jstr(A.st, "profile", "Default");
                        if (ImGui::BeginCombo("##profile", cur.c_str())) {
                            for (const json& pj : A.st.value("profiles", json::array())) {
                                std::string p = pj.get<std::string>();
                                if (p == "Custom" && cur != "Custom") continue;
                                if (ImGui::Selectable(p.c_str(), p == cur) && p != cur) {
                                    A.st["profile"] = p;
                                    save_now({{"profile", p}});
                                }
                            }
                            ImGui::EndCombo();
                        }
                    });
        update_switch();
        setting_row("folders", "Folders", "Where the logs and the crash reports are kept (both stay on this PC).", S(270), [] {
            const json app = A.st.value("app", json::object());
            std::string logs = jstr(app, "log_dir"), crash = jstr(A.st.value("crash", json::object()), "dir");
            if (Button("Logs", ImVec2(S(124), S(32)))) open_folder(logs);
            ImGui::SameLine();
            if (Button("Crash reports", ImVec2(S(136), S(32)))) open_folder(crash);
        });
    }
    EndCard();
}

// ---------------------------------------------------------------------------------------------
// Log

static void tab_log(ImVec2 avail)
{
    int lines;
    {
        std::lock_guard<std::mutex> lock(A.mu);
        lines = (int)A.log.size();
    }
    if (Button("Copy", ImVec2(S(90), S(32)))) {
        std::string all;
        std::lock_guard<std::mutex> lock(A.mu);
        for (auto& l : A.log) all += l + "\n";
        if (!A.partial.empty()) all += A.partial + "\n";
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    if (Button("Clear", ImVec2(S(90), S(32)))) {
        std::lock_guard<std::mutex> lock(A.mu);
        A.log.clear();
        A.partial.clear();
    }
    ImGui::SameLine();
    Muted((std::to_string(lines) + " lines (the last 6000 are kept)").c_str());
    if (BeginCard("logcard", ImVec2(avail.x, avail.y - S(46)))) {
        ImGui::PushFont(F_MONO, S(13));
        ImGui::PushStyleColor(ImGuiCol_Text, CV(0xcfc6b8));
        ImGui::BeginChild("logscroll", ImVec2(0, 0), ImGuiChildFlags_None);
        {
            std::lock_guard<std::mutex> lock(A.mu);
            if (A.log.empty() && A.partial.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
                ImGui::TextUnformatted("The output of the game launcher appears here.");
                ImGui::PopStyleColor();
            }
            ImGui::PushTextWrapPos(0.0f);
            for (auto& l : A.log) ImGui::TextUnformatted(l.c_str());
            if (!A.partial.empty()) ImGui::TextUnformatted(A.partial.c_str());
            ImGui::PopTextWrapPos();
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) A.log_follow = true;
        else if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0) A.log_follow = false;
        if (A.log_follow) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
    EndCard();
}

// ---------------------------------------------------------------------------------------------
// Update switch (About and Advanced)

static void switch_row(const char* id, const char* title, const char* detail, bool* v, bool& changed)
{
    ImGui::PushID(id);
    ImVec2 top = ImGui::GetCursorPos();
    float total = ImGui::GetContentRegionAvail().x;
    ImGui::PushFont(F_SEMI, S(15));
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    MutedWrapped(detail, 13, total - S(70));
    float h = ImGui::GetCursorPosY() - top.y;
    ImGui::SetCursorPos(ImVec2(top.x + total - S(44), top.y + h * 0.5f - S(11)));
    if (Toggle("##sw", v)) changed = true;
    ImGui::SetCursorPos(ImVec2(top.x, top.y + h + S(8)));
    ImGui::PopID();
}

static void update_switch()
{
    const json up = A.st.value("update", json::object());
    bool on = up.value("enabled", true);
    bool changed = false;
    switch_row("updsw", "Check for updates at start", "Asks GitHub (the release list; nothing about you is sent) whether a newer version exists and says so here. Switch it off to never connect.", &on, changed);
    if (changed) cli_async({"update-opt", on ? "on" : "off"});
}

// ---------------------------------------------------------------------------------------------
// Online co-op page

static unsigned online_state_color(const std::string& st)
{
    if (st == "in world") return GREEN;
    if (st == "sign up") return AMBER;
    if (st == "host") return GOLD;
    return MUTED;
}

static std::string online_bell_text(const json& b, bool in_room)
{
    if (!in_room) return "";
    if (!b.is_object() || b.empty()) return "Not running. It starts with the game, which rings your bell by itself.";
    std::string state = jstr(b, "state");
    static const struct { const char* id; const char* text; } names[] = {
        {"waiting_for_world", "waiting until your character is in the world"}, {"bell_missing", "the bell is not in your inventory"},
        {"ready", "ready to ring"}, {"ringing", "ringing now"}, {"room_full", "room is full, resting"},
        {"room_closed", "the room closed"}, {"sign_up", "your sign is up, waiting for the host"},
        {"summoned", "you are in the host's world"}, {"off", "off"}};
    for (auto& n : names)
        if (state == n.id) state = n.text;
    std::string t = "Auto-bell: " + state + ".  Rings so far: " + std::to_string(b.value("rings", 0));
    double next = b.value("next_ring_in_s", 0.0);
    if (next > 1 && jstr(b, "state") == "ready") t += ".  Next ring in " + std::to_string((int)next) + " s";
    return t;
}

static void tab_online(ImVec2 avail)
{
    const json& v = A.online;
    const bool in_room = v.value("in_room", false);
    float gap = S(16), lw = std::floor((avail.x - gap) * 0.5f);
    ImGui::BeginChild("onlinescroll", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    ImGui::BeginChild("onl", ImVec2(lw, 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
    Heading("Room");
    if (BeginCard("room", ImVec2(-FLT_MIN, 0), in_room ? GOLD : BORDER, true)) {
        if (A.online_have && !v.value("ok", true) && !online_no_server() && !(v.contains("in_room"))) {
            MutedWrapped(jstr(v, "message", "The room server is not reachable."), 14);
        } else
            room_panel(lw - S(34));
        if (v.value("closed", false)) {
            ImGui::Dummy(ImVec2(0, S(6)));
            MutedWrapped("Your last room closed" + (jstr(v, "reason").empty() ? std::string(".") : " (" + jstr(v, "reason") + ")."), 13);
        }
        ImGui::Dummy(ImVec2(0, S(6)));
        MutedWrapped("Co-op is for up to three players. A room is opened on the lobby this release comes with; nothing to type. Pick Co-op on the Play page, then press PLAY.", 13, lw - S(34));
    }
    EndCard();
    ImGui::Dummy(ImVec2(0, S(8)));
    if (const json* page = find_page("online")) draw_sections(*page);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    ImGui::BeginChild("onr", ImVec2(avail.x - lw - gap - S(16), 0), ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoBackground);
    Heading("Members");
    if (BeginCard("members", ImVec2(-FLT_MIN, 0), BORDER, true)) {
        if (!in_room) {
            MutedWrapped("Nobody yet. Host a game or join one to see who is in the room.", 14);
        } else {
            int i = 0;
            for (const json& m : v.value("members", json::array())) {
                ImGui::PushID(i++);
                std::string st = jstr(m, "state", "waiting");
                ImVec2 start = ImGui::GetCursorPos();
                Dot(online_state_color(st));
                ImGui::SetCursorPos(ImVec2(start.x + S(24), start.y));
                ImGui::PushFont(F_SEMI, S(15));
                std::string name = jstr(m, "name", "?") + (m.value("you", false) ? " (you)" : "");
                ImGui::TextUnformatted(name.c_str());
                ImGui::PopFont();
                ImGui::SetCursorPos(ImVec2(start.x + S(24), start.y + S(22)));
                std::string line = st == "host" ? "host" : st == "in world" ? "in the host's world" : st == "sign up" ? "sign is up, waiting to be summoned" : "waiting (no sign yet)";
                MutedWrapped(line, 13);
                ImGui::Dummy(ImVec2(0, S(8)));
                ImGui::PopID();
            }
            ImGui::Dummy(ImVec2(0, S(6)));
            Heading("Auto-bell");
            MutedWrapped(online_bell_text(v.value("bell", json::object()), true), 14);
            std::string err = v.contains("bell") && v["bell"].is_object() ? jstr(v["bell"], "last_error") : "";
            if (!err.empty()) MutedWrapped("Last problem: " + err, 13);
        }
    }
    EndCard();
    ImGui::EndChild();
    ImGui::EndChild();
}

static void tab_controls(ImVec2 avail)
{
    static const char* rows[][2] = {
        {"Left stick", "Move"},
        {"Right stick", "Camera / turn (smooth or snap, see VR comfort)"},
        {"Right-hand swing", "Light attack (R1)"},
        {"Swing with the right grip held, or a very hard swing", "Heavy attack (R2); keep the grip held after the swing to charge"},
        {"Quick right-grip tap (under 0.35 s, no swing)", "Swap the right-hand weapon"},
        {"Right trigger", "Nothing in the world"},
        {"Left trigger (hold)", "Hold the gun (L2)"},
        {"Left-hand thrust forward (gun held)", "Shoot"},
        {"Left grip (squeeze)", "L1 (transform)"},
        {"A (right)", "Swap the right-hand weapon"},
        {"B (right)", "Roll / dodge, hold to sprint"},
        {"X (left)", "Swap the left-hand weapon"},
        {"Y (left)", "Interact (doors, pickups, ladders)"},
        {"Heal", "From the quick wheel, or bring a held Blood Vial to your mouth"},
        {"Right stick click (tap)", "Open the item wheel: pick with your right hand (touch or point) or the stick"},
        {"Left stick click while standing", "Open the VR dashboard (equipment, items, options)"},
        {"Left stick click while moving", "Jump"},
        {"Left stick click, held 1 s", "Recenter"},
        {"Left menu button", "Options (pause)"},
        {"Swing at a staggered enemy", "Visceral attack (stagger one with a gun parry, or a charged heavy from behind)"},
    };
    if (BeginCard("controls", avail)) {
        if (ImGui::BeginTable("ctl", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY, ImVec2(0, avail.y - S(100)))) {
            ImGui::TableSetupColumn("Controller input", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            ImGui::TableSetupColumn("In game", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();
            for (auto& r : rows) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushFont(F_SEMI, S(14));
                ImGui::TextWrapped("%s", r[0]);
                ImGui::PopFont();
                ImGui::TableNextColumn();
                ImGui::PushFont(F_REG, S(14));
                ImGui::TextWrapped("%s", r[1]);
                ImGui::PopFont();
            }
            ImGui::EndTable();
        }
        ImGui::Dummy(ImVec2(0, S(4)));
        MutedWrapped("Lock-on is off by default: a right stick click opens the item wheel instead. In the game's own menus (Options, pause): either stick moves the cursor (hold to repeat), A or either trigger confirms, B goes back, and the menu button closes the menu. Alpha: controls may change.");
    }
    EndCard();
}

static void tab_about(ImVec2 avail)
{
    float gap = S(16), lw = std::floor((avail.x - gap) * 0.5f);
    ImGui::BeginGroup();
    if (BeginCard("about1", ImVec2(lw, avail.y))) {
        Heading("Bloodborne VR");
        std::string ver = jstr(A.st.value("app", json::object()), "version");
        if (ver.empty()) ver = BBVR_VERSION;
        ImGui::PushFont(F_SEMI, S(22));
        ImGui::Text("Version %s", ver.c_str());
        ImGui::PopFont();
        MutedWrapped("An unofficial fan project that runs YOUR copy of Bloodborne (PS4, CUSA03173, version 1.09) natively on Windows with a Vulkan renderer and an OpenXR bridge for VR headsets. Early alpha, with co-op for up to three players.");
        ImGui::Dummy(ImVec2(0, S(10)));
        Heading("Licence");
        MutedWrapped("Free software under the GPL (see LICENSE and THIRD_PARTY_NOTICES.md next to this program). No game files are included and none are ever uploaded: you must own Bloodborne and use your own legally obtained dump. Not affiliated with, endorsed or sponsored by Sony Interactive Entertainment, FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are trademarks of their owners.");
        ImGui::Dummy(ImVec2(0, S(10)));
        Heading("Credits");
        MutedWrapped("This program uses NVIDIA DLSS (nvngx_dlss.dll, NVIDIA RTX SDKs LICENSE: see licenses/NVIDIA-DLSS-LICENSE.txt). Not sponsored or endorsed by NVIDIA.");
        ImGui::Dummy(ImVec2(0, S(10)));
        Heading("Links");
        if (Button("GitHub repository", ImVec2(S(170), S(34)))) open_url("https://github.com/laughtt/bloodborne-vr");
        ImGui::SameLine();
        if (Button("Open the logs folder", ImVec2(S(180), S(34)))) {
            std::string dd = jstr(A.st.value("app", json::object()), "data_dir");
            std::wstring base = dd.empty() ? A.env.exe_dir : W(dd);
            std::wstring logs = base + L"\\logs";
            if (!dir_exists(logs)) logs = base;
            ShellExecuteW(nullptr, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    EndCard();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    if (BeginCard("about2", ImVec2(avail.x - lw - gap, avail.y))) {
        Heading("Updates");
        {
            const json up = A.st.value("update", json::object());
            std::string stt = jstr(up, "status");
            std::string line = !up.value("enabled", true) ? "Update check is off."
                : stt == "newer" ? "New version " + jstr(up, "latest") + " is available."
                : stt == "current" ? "You have the newest version."
                : stt == "unavailable" ? "Update check unavailable right now."
                : "Not checked yet.";
            ImGui::PushStyleColor(ImGuiCol_Text, CV(stt == "newer" ? GREEN : MUTED));
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
            if (up.value("enabled", true) && Button("Check for updates", ImVec2(S(170), S(34)))) cli_async({"update-check"});
            if (stt == "newer") {
                ImGui::SameLine();
                if (Button("Open download page", ImVec2(S(190), S(34)), true)) open_url(jstr(up, "url"));
            }
            ImGui::Dummy(ImVec2(0, S(6)));
            update_switch();
        }
        Heading("Crash reports");
        const json cr = A.st.value("crash", json::object());
        if (cr.value("available", false)) {
            MutedWrapped("If the game crashes, a report is saved on this PC: a small crash dump, the fault address, the build id and the last lines of the log, with your user name, folders, IP addresses and PSN ids removed. Nothing is sent automatically. If you want to help, send the report file to the developer.");
            MutedWrapped(std::to_string(cr.value("count", 0)) + " saved in " + jstr(cr, "dir"));
            ImGui::Dummy(ImVec2(0, S(4)));
            if (Button("Open crash reports folder", ImVec2(S(250), S(34)))) open_folder(jstr(cr, "dir"));
        } else
            MutedWrapped("Crash reports are not part of this build.");
        ImGui::Dummy(ImVec2(0, S(10)));
        Heading("Settings");
        MutedWrapped("The dashboard and the web settings page edit the same files (user\\bbvr_settings.json, out\\fp.txt, stereo.txt, iq.txt), so you can use either. Changed control files keep a .bak of the original.");
        ImGui::Dummy(ImVec2(0, S(6)));
        if (Button("Open the web settings (fallback)", ImVec2(S(260), S(34)))) {
            std::wstring self;
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            self = buf;
            ShellExecuteW(nullptr, L"open", self.c_str(), L"--web", A.env.exe_dir.c_str(), SW_SHOWNORMAL);
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        Heading("Reporting bugs");
        MutedWrapped("Open an issue on the project page and attach logs\\bbvr-latest.log. bin\\bb-gpu-capabilities.exe prints your GPU's Vulkan features: include its output. Never post saves or game files.");
    }
    EndCard();
}

// ---------------------------------------------------------------------------------------------
// first-run prompts

static const char* LEGAL =
    "Bloodborne VR is an unofficial fan project. It contains no game files: you must own Bloodborne and use your own legally obtained dump. It is not affiliated with, endorsed or sponsored by Sony Interactive Entertainment, FromSoftware or any trademark holder. PlayStation, PS4 and Bloodborne are trademarks of their owners.";

static void centered_popup(const char* name, ImVec2 size)
{
    ImVec2 c = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(c, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(20)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, S(14));
}

static void prompts()
{
    if (!A.have_state) return;
    const json app = A.st.value("app", json::object());
    bool need_legal = !app.value("accepted", false);
    bool need_alpha = !need_legal && app.value("alpha_notice", false) && !A.alpha_ack;
    bool quit = A.quit_confirm;
    const char* which = need_legal ? "Welcome" : (need_alpha ? "Early alpha" : (quit ? "Quit" : nullptr));
    if (!which) return;
    ImGui::OpenPopup(which);
    centered_popup(which, ImVec2(S(560), 0));
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(560), S(150)), ImVec2(S(560), S(520)));
    if (ImGui::BeginPopupModal(which, nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::PushFont(F_SEMI, S(22));
        ImGui::TextUnformatted(which);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PushTextWrapPos(S(512));
        if (need_legal) {
            ImGui::TextUnformatted(LEGAL);
            ImGui::Dummy(ImVec2(0, S(6)));
            ImGui::TextUnformatted("Early alpha with co-op (up to 3 players). Free software under the GPL. If the game crashes, a report is saved on this PC (the About tab shows where); nothing is ever sent automatically.");
            ImGui::Dummy(ImVec2(0, S(10)));
            if (Button("I own a legal copy of Bloodborne - continue", ImVec2(-1, S(44)), true)) cli_async({"accept"});
        } else if (need_alpha) {
            ImGui::TextUnformatted("Early alpha: many bugs, crashes are possible, please be patient.");
            ImGui::Dummy(ImVec2(0, S(6)));
            ImGui::TextUnformatted("If the game crashes, the report stays on this PC (the About tab shows where); it is never uploaded. To report a bug, see README.txt. This note shows once per version.");
            ImGui::Dummy(ImVec2(0, S(10)));
            if (Button("OK", ImVec2(-1, S(44)), true)) {
                A.alpha_ack = true;
                cli_async({"alpha-ack"});
            }
        } else {
            ImGui::TextUnformatted("The game is running. Quitting the dashboard stops it. Quit anyway?");
            ImGui::Dummy(ImVec2(0, S(10)));
            if (Button("Stop the game and quit", ImVec2(S(250), S(44)), true)) {
                stop_game();
                SDL_Event e = {};
                e.type = SDL_EVENT_USER;
                A.quit_confirm = false;
                SDL_Event q = {};
                q.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&q);
                A.running = false;
            }
            ImGui::SameLine();
            if (Button("Keep playing", ImVec2(S(250), S(44)))) A.quit_confirm = false;
        }
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
}

// ---------------------------------------------------------------------------------------------
// frame: sidebar, content, bottom bar

static const float SIDEBAR_W = 220, BAR_H = 76;

struct StatusLine {
    std::string text;
    unsigned color = MUTED;
    bool can_play = false;
    std::string go;  // the page that fixes what blocks PLAY
};

static std::string render_name(const std::string& m)
{
    return m == "mono" ? "Mono" : m == "alt" ? "Alternate-eye" : "Stereo";
}

static StatusLine compute_status()
{
    StatusLine s;
    if (!A.have_state) {
        s.text = A.env.ok ? "Reading your settings..." : A.env.problem;
        s.color = A.env.ok ? MUTED : ACCENT;
        return s;
    }
    const json app = A.st.value("app", json::object());
    if (A.running) {
        s.text = cur_mode() == "flat" ? "The game is running." : "The game is running. Put the headset on.";
        s.color = GOLD;
        return s;
    }
    if (!A.env.ok) {
        s.text = A.env.problem;
        s.color = ACCENT;
        return s;
    }
    if (!app.value("eboot", false)) {
        s.text = "Choose your game folder first.";
        s.color = ACCENT;
        s.go = "play";
        return s;
    }
    if (!app.value("accepted", false)) {
        s.text = "Accept the note to continue.";
        s.color = AMBER;
        return s;
    }
    for (const json& r : A.st.value("ready", json::array()))
        if (jstr(r, "id") == "game" && jstr(r, "status") == "bad") {
            s.text = jstr(r, "detail");
            s.color = ACCENT;
            s.go = "play";
            return s;
        }
    if (cur_mode() == "coop" && !online_in_room()) {
        s.text = online_no_server() ? "Co-op needs a lobby address: enter it on the Online co-op page." : "Co-op: host a room or join one with a code first.";
        s.color = AMBER;
        s.go = online_no_server() ? "online" : "play";
        return s;
    }
    s.can_play = true;
    std::string mode = cur_mode(), view = cur_view();
    std::string t = mode == "coop" ? std::string("Co-op (") + (view == "flat" ? "flat" : "VR") + ") in room " + jstr(A.online, "code") : mode == "flat" ? "Flat" : "VR";
    if (view == "vr") {
        json vals = A.st.value("values", json::object());
        bool dlss = jstr(vals, "upscale") == "dlss" && dlss_ok();
        t += "  |  " + A.st.value("quality", "High") + " (" + (dlss ? "DLSS" : "FSR 1") + ")  |  " + render_name(jstr(vals, "mode", "stereo"));
    }
    s.color = MUTED;
    if (A.st.contains("warnings") && !A.st["warnings"].empty() && A.st["warnings"][0].is_string()) {
        t += "  |  " + A.st["warnings"][0].get<std::string>();
        s.color = AMBER;
    }
    s.text = "Ready: " + t;
    return s;
}

static void draw_sidebar(ImDrawList* dl, float h)
{
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(S(SIDEBAR_W), h - S(BAR_H)), C(BG));
    dl->AddLine(ImVec2(S(SIDEBAR_W), 0), ImVec2(S(SIDEBAR_W), h - S(BAR_H)), C(BORDER));
    float x = S(22);
    dl->AddText(F_SERIF_B, S(21), ImVec2(x, S(18)), C(GOLD), "BLOODBORNE");
    float tw = F_SERIF_B->CalcTextSizeA(S(21), FLT_MAX, 0, "BLOODBORNE").x;
    (void)tw;
    dl->AddRectFilled(ImVec2(x, S(48)), ImVec2(x + S(34), S(68)), C(BLOOD), S(5));
    ImVec2 vsz = F_SEMI->CalcTextSizeA(S(13), FLT_MAX, 0, "VR");
    dl->AddText(F_SEMI, S(13), ImVec2(x + (S(34) - vsz.x) * 0.5f, S(58) - vsz.y * 0.5f), C(0xf4ece0), "VR");
    std::string ver = jstr(A.st.value("app", json::object()), "version");
    if (ver.empty()) ver = BBVR_VERSION;
    dl->AddText(F_REG, S(13), ImVec2(x + S(44), S(51)), C(MUTED), ver.c_str());

    float y = S(88);
    const bool flat = cur_view() == "flat";
    if (A.have_state && A.st.contains("pages")) {
        int count = 0;
        for (const json& p : A.st["pages"])
            if (!(p.value("vr", false) && flat)) count++;
        // the items shrink a little in a short window so they never reach the switch at the bottom
        float step = std::min(S(42), (h - S(BAR_H) - S(88) - S(110)) / std::max(count, 1));
        float ih = step - S(2);
        for (const json& p : A.st["pages"]) {
            std::string id = p.value("id", ""), label = p.value("label", "");
            if (p.value("vr", false) && flat) continue;
            ImGui::SetCursorScreenPos(ImVec2(0, y));
            ImGui::PushID(id.c_str());
            ImGui::InvisibleButton("##nav", ImVec2(S(SIDEBAR_W), ih));
            bool sel = A.tab == id, hot = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked()) A.tab = id;
            ImGui::PopID();
            if (sel) {
                dl->AddRectFilled(ImVec2(0, y), ImVec2(S(SIDEBAR_W), y + ih), C(PANEL));
                dl->AddRectFilled(ImVec2(0, y), ImVec2(S(3), y + ih), C(BLOOD_H));
            } else if (hot)
                dl->AddRectFilled(ImVec2(0, y), ImVec2(S(SIDEBAR_W), y + ih), C(0x17130f));
            ImVec2 sz = F_SEMI->CalcTextSizeA(S(16), FLT_MAX, 0, label.c_str());
            dl->AddText(sel ? F_SEMI : F_REG, S(16), ImVec2(S(24), y + ih * 0.5f - sz.y * 0.5f), sel ? C(GOLD) : C(hot ? 0xd8cfc0 : 0xb9ae9d), label.c_str());
            y += step;
        }
    }
    // bottom of the sidebar: advanced switch and the update notice
    float by = h - S(BAR_H) - S(46);
    ImGui::SetCursorScreenPos(ImVec2(S(22), by));
    ImGui::PushFont(F_REG, S(14));
    bool adv = A.advanced;
    if (Toggle("##adv", &adv)) A.advanced = adv;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, CV(0xb9ae9d));
    ImGui::TextUnformatted("Show advanced options");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    const json up = A.st.value("update", json::object());
    if (up.value("enabled", true) && jstr(up, "status") == "newer") {
        float uy = by - S(78);
        dl->AddRectFilled(ImVec2(S(14), uy), ImVec2(S(SIDEBAR_W) - S(14), uy + S(68)), C(0x2a1d12), S(8));
        dl->AddRect(ImVec2(S(14), uy), ImVec2(S(SIDEBAR_W) - S(14), uy + S(68)), C(AMBER, 0.6f), S(8));
        dl->AddText(F_SEMI, S(13), ImVec2(S(24), uy + S(8)), C(0xe3b25a), ("Update " + jstr(up, "latest") + " available").c_str());
        ImGui::SetCursorScreenPos(ImVec2(S(24), uy + S(30)));
        if (Button("Download", ImVec2(S(100), S(28)), true)) open_url(jstr(up, "url"));
    }
}

static void draw_bottom_bar(ImDrawList* dl, float w, float h)
{
    float y0 = h - S(BAR_H);
    dl->AddRectFilled(ImVec2(0, y0), ImVec2(w, h), C(BG));
    dl->AddLine(ImVec2(0, y0), ImVec2(w, y0), C(BORDER));
    StatusLine st = compute_status();
    float play_w = S(230), stop_w = S(110), pad = S(24);
    float tx = pad;
    ImVec2 c(tx + S(5), y0 + S(BAR_H) * 0.5f);
    dl->AddCircleFilled(c, S(8), C(st.color, 0.18f));
    dl->AddCircleFilled(c, S(4.5f), C(st.color));
    float max_w = w - play_w - stop_w - pad * 2 - S(60);
    std::string text = Fit(st.text, max_w);
    ImVec2 tsz = F_REG->CalcTextSizeA(S(14.5f), FLT_MAX, 0, text.c_str());
    dl->AddText(F_REG, S(14.5f), ImVec2(tx + S(20), y0 + S(BAR_H) * 0.5f - tsz.y * 0.5f), C(st.can_play || A.running ? 0xd2c9ba : st.color), text.c_str());

    float bx = w - pad - play_w;
    ImGui::SetCursorScreenPos(ImVec2(bx, y0 + (S(BAR_H) - S(52)) * 0.5f));
    ImGui::PushFont(F_SERIF_B, S(24));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, st.can_play ? 1.0f : 0.55f);
    bool play = Button(A.running ? "RUNNING" : "PLAY", ImVec2(play_w, S(52)), true);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (play && !A.running) {
        if (st.can_play) start_game();
        else if (!st.go.empty()) A.tab = st.go;
    }
    ImGui::SetCursorScreenPos(ImVec2(bx - S(12) - stop_w, y0 + (S(BAR_H) - S(40)) * 0.5f));
    if (!A.running) ImGui::BeginDisabled();
    ImGui::PushFont(F_SEMI, S(15));
    if (Button("Stop", ImVec2(stop_w, S(40)), false, true)) stop_game();
    ImGui::PopFont();
    if (!A.running) ImGui::EndDisabled();
}

static const char* page_subtitle(const std::string& tab)
{
    if (tab == "graphics") return "VR quality presets and the upscaler";
    if (tab == "display") return "Frame rate, counter and render mode";
    if (tab == "comfort") return "Turning, camera, dodging and the HUD";
    if (tab == "combat") return "Hit shapes, attacks, hands and body";
    if (tab == "controls") return "VR controller map";
    if (tab == "online") return "Rooms, members and bells";
    if (tab == "advanced") return "Runtime, logs and the application";
    if (tab == "log") return "What the launcher and the game print";
    if (tab == "about") return "Version, licence, updates and crash reports";
    return "";
}

static void draw_ui(float w, float h)
{
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // content background
    dl->AddRectFilled(ImVec2(S(SIDEBAR_W), 0), ImVec2(w, h - S(BAR_H)), C(PANEL));

    // a VR-only page while the view is Flat: back to Play
    const bool show_online = A.have_state && A.st.value("online_play", true);
    if (A.have_state && cur_view() == "flat")
        if (const json* pg = find_page(A.tab))
            if (pg->value("vr", false)) A.tab = "play";
    if (A.tab == "online" && !show_online && A.have_state) A.tab = "play";

    float mx = S(28), cx = S(SIDEBAR_W) + mx, cw = w - S(SIDEBAR_W) - 2 * mx;
    // page title
    std::string title = "Play";
    if (const json* pg = find_page(A.tab)) title = jstr(*pg, "label", title);
    dl->AddText(F_SERIF_B, S(27), ImVec2(cx, S(16)), C(0xf2ead9), title.c_str());
    {
        std::string sub = A.tab == "play" ? "" : page_subtitle(A.tab);
        if (!sub.empty()) dl->AddText(F_REG, S(13.5f), ImVec2(cx + F_SERIF_B->CalcTextSizeA(S(27), FLT_MAX, 0, title.c_str()).x + S(16), S(27)), C(MUTED), sub.c_str());
    }

    // content
    float top = S(62);
    ImVec2 avail(cw, h - S(BAR_H) - top - S(10));
    ImGui::SetCursorScreenPos(ImVec2(cx, top));
    if (!A.crash_new.empty()) {  // the game crashed: say so plainly (the report is already saved)
        float bh = S(64);
        dl->AddRectFilled(ImVec2(cx, top), ImVec2(cx + avail.x, top + bh), C(0x2a1114), S(10));
        dl->AddRect(ImVec2(cx, top), ImVec2(cx + avail.x, top + bh), C(ACCENT), S(10));
        std::string msg = "The game crashed. A crash report was saved to " + A.crash_new + ". If you want to help, send that file to the developer.";
        ImGui::SetCursorScreenPos(ImVec2(cx + S(14), top + S(8)));
        ImGui::PushTextWrapPos(cx + avail.x - S(280));
        ImGui::TextUnformatted(msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(cx + avail.x - S(260), top + S(15)));
        if (Button("Open folder", ImVec2(S(120), S(34)), true)) open_folder(jstr(A.st.value("crash", json::object()), "dir"));
        ImGui::SameLine();
        if (Button("Dismiss", ImVec2(S(110), S(34)))) A.crash_new.clear();
        avail.y -= bh + S(10);
        top += bh + S(10);
        ImGui::SetCursorScreenPos(ImVec2(cx, top));
    }
    ImGui::BeginChild("content", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (!A.have_state) {
        ImGui::Dummy(ImVec2(0, S(20)));
        Muted(A.env.ok ? "Reading your settings..." : A.env.problem.c_str(), 16);
        std::lock_guard<std::mutex> lock(A.mu);
        if (!A.message.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, CV(ACCENT));
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(A.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    } else {
        // the room state: while Co-op, on the Online page, and once at start to know whether a room exists
        if (A.tab == "online" || cur_mode() == "coop" || !A.online_have) poll_online();
        if (A.tab == "play") tab_play(avail);
        else if (A.tab == "controls") tab_controls(avail);
        else if (A.tab == "online") tab_online(avail);
        else if (A.tab == "log") tab_log(avail);
        else if (A.tab == "about") tab_about(avail);
        else {
            ImGui::BeginChild("pagescroll", avail, ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            if (A.tab == "advanced") tab_advanced();
            else page_options(A.tab, A.tab == "graphics");
            ImGui::Dummy(ImVec2(0, S(14)));
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();

    draw_sidebar(dl, h);
    draw_bottom_bar(dl, w, h);

    {  // toast for folder errors etc.
        std::lock_guard<std::mutex> lock(A.mu);
        std::string err = jstr(A.st, "error");
        if (!err.empty() && A.have_state) {
            float tw = S(520);
            ImVec2 p(w * 0.5f - tw * 0.5f + S(SIDEBAR_W) * 0.5f, h - S(BAR_H) - S(56));
            dl->AddRectFilled(p, ImVec2(p.x + tw, p.y + S(40)), C(0x2a1114), S(10));
            dl->AddRect(p, ImVec2(p.x + tw, p.y + S(40)), C(ACCENT), S(10));
            dl->AddText(F_REG, S(14), ImVec2(p.x + S(14), p.y + S(11)), C(TEXT), Fit(err, tw - S(28)).c_str());
        }
    }
    prompts();
    ImGui::End();
}

static void pump_state()
{
    std::lock_guard<std::mutex> lock(A.mu);
    if (A.online_has_incoming) {
        A.online = A.online_incoming;
        A.online_has_incoming = false;
        A.online_have = true;
        if (g_demo && A.demo_flow == "noserver") A.online = json{{"ok", false}, {"error", "no_server"}};  // screenshots only
        if (g_demo && (A.demo_flow == "room" || A.demo_flow == "running")) {  // screenshots only
            A.online = json::parse(R"({"ok":true,"in_room":true,"code":"K7Q-M4X","role":"host","closed":false,"max":3,"members":[
                {"name":"Hunter","role":"host","state":"host","you":true},{"name":"Friend A","role":"guest","state":"in world"},
                {"name":"Friend B","role":"guest","state":"sign up"}],"bell":{"state":"ready","rings":2,"next_ring_in_s":48}})");
        }
    }
    if (A.has_incoming && (!A.dirty || !A.have_state)) {
        A.st = A.incoming;
        A.has_incoming = false;
        A.have_state = true;
        if (g_demo) {  // screenshots only: pretend the first-run steps are done (nothing is written)
            A.st["app"]["accepted"] = true;
            A.st["app"]["alpha_notice"] = false;
            A.st["app"]["eboot"] = true;
            if (A.st["app"]["game_dir"].get<std::string>().empty()) A.st["app"]["game_dir"] = R"(C:\Dumps\Bloodborne\CUSA03173)";
            if (!g_demo_mode.empty()) {
                std::string m = g_demo_mode == "coopflat" ? "coop" : g_demo_mode;
                A.st["mode"] = m;
                A.st["coop_view"] = g_demo_mode == "coopflat" ? "flat" : "vr";
            }
            if (!A.demo_flow.empty() && g_demo_mode.empty()) A.st["mode"] = "coop";
            if (A.demo_flow == "running") A.running = true;
        }
        const json cr = A.st.value("crash", json::object());
        if (!A.running && A.crash_base >= 0 && cr.value("count", 0) > A.crash_base) {  // the session that just ended crashed
            A.crash_new = jstr(cr, "latest");
            A.crash_base = -1;
        }
    }
}

static bool save_png(SDL_Renderer* r, const char* path)
{
    SDL_Surface* s = SDL_RenderReadPixels(r, nullptr);
    if (!s) return false;
    bool ok = SDL_SavePNG(s, path);
    SDL_DestroySurface(s);
    return ok;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::wstring> args;
    for (int i = 1; argv && i < argc; i++) args.push_back(argv[i]);
    if (argv) LocalFree(argv);

    A.env = find_env();
    std::string shot, force_tab;
    int win_w = 1120, win_h = 740;
    bool web = false;
    std::vector<std::wstring> passthrough;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == L"--web") web = true;
        else if (args[i] == L"--screenshot" && i + 1 < args.size()) shot = U8(args[++i]);
        else if (args[i] == L"--tab" && i + 1 < args.size()) force_tab = U8(args[++i]);
        else if (args[i] == L"--advanced") A.advanced = true;
        else if (args[i] == L"--demo") g_demo = true;
        else if (args[i] == L"--mode" && i + 1 < args.size() && g_demo) g_demo_mode = U8(args[++i]);  // screenshots: vr | coop | coopflat | flat
        else if (args[i] == L"--size" && i + 1 < args.size()) sscanf(U8(args[++i]).c_str(), "%dx%d", &win_w, &win_h);
        else if (args[i] == L"--flow" && i + 1 < args.size()) A.demo_flow = U8(args[++i]);  // screenshots: room | running | noserver
        else passthrough.push_back(args[i]);
    }
    if (web || !passthrough.empty()) {  // any launcher argument: the console launcher with the web menu
        if (!A.env.ok) {
            MessageBoxW(nullptr, W(A.env.problem).c_str(), L"Bloodborne VR", MB_OK | MB_ICONERROR);
            return 1;
        }
        return run_console_launcher(A.env, passthrough);
    }
    if (!force_tab.empty()) {
        if (force_tab == "options") force_tab = "comfort";  // names of the earlier dashboard
        if (force_tab == "dlss") force_tab = "graphics";
        A.tab = force_tab;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxA(nullptr, SDL_GetError(), "Bloodborne VR", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g_scale < 1.0f || g_scale > 3.0f) g_scale = 1.0f;
    if (!shot.empty()) g_scale = 1.0f;
    SDL_Window* window = SDL_CreateWindow("Bloodborne VR", (int)S((float)win_w), (int)S((float)win_h), SDL_WINDOW_RESIZABLE | (shot.empty() ? 0 : SDL_WINDOW_HIDDEN));
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        MessageBoxA(nullptr, SDL_GetError(), "Bloodborne VR", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_renderer = renderer;
    A.window = window;
    SDL_SetWindowMinimumSize(window, (int)S(880), (int)S(600));
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    load_fonts(S(16));
    setup_theme();

    if (A.env.ok) cli_async({"state"});
    if (A.env.ok && shot.empty()) cli_async({"update-check"});  // quiet and non-blocking; the helper honours the opt-out
    bool done = false;
    int frames = 0, settle = 0;
    while (!done) {
        SDL_Event e;
        bool any = SDL_WaitEventTimeout(&e, shot.empty() ? 33 : 5);
        while (any) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            if (e.type == SDL_EVENT_QUIT || (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && e.window.windowID == SDL_GetWindowID(window))) {
                if (A.running && !A.quit_confirm) A.quit_confirm = true;
                else if (!A.running) done = true;
                else done = false;
            }
            any = SDL_PollEvent(&e);
        }
        pump_state();
        ensure_banner();
        if (A.online_joined) {  // a host / join worked: the Co-op mode
            A.online_joined = false;
            set_mode("coop");
        }
        if (A.dirty && ImGui::GetTime() - A.dirty_at > 0.4) {  // debounced save; while the game runs the helper also writes the live files
            A.dirty = false;
            cli_async({"save"}, default_save_body(true));
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        int pw, ph;
        SDL_GetWindowSize(window, &pw, &ph);
        draw_ui((float)pw, (float)ph);
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 14, 12, 11, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        frames++;
        if (!shot.empty() && A.have_state && A.cli_busy == 0) settle++;
        if (!shot.empty() && (settle > 8 || frames > 2000)) {
            bool ok = save_png(renderer, shot.c_str());
            if (!ok) SDL_Log("screenshot failed: %s", SDL_GetError());
            done = true;
            break;
        }
        SDL_RenderPresent(renderer);
    }
    if (A.running) stop_game();
    if (A.banner) SDL_DestroyTexture(A.banner);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
