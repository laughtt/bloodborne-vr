// SPDX-License-Identifier: GPL-2.0-or-later
// BloodborneVR.exe: the native dashboard (Dear ImGui + SDL3 renderer). All logic and files live in the
// Python side (launcher/bbvr_cli.py, bbvr_settings.py, bbvr_launcher.py), so this window and the web menu
// (BloodborneVR.exe --web) read and write the same settings. The look follows a dark dashboard style
// (own tokens: bg #0d0f14, panel #151922, border #262c3b, accent red #e3262f).
//
//   BloodborneVR.exe                        the dashboard
//   BloodborneVR.exe --web [args]           the web settings menu in a console window (old behaviour)
//   BloodborneVR.exe --check / --reset ...  any launcher argument also runs the console launcher
//   BloodborneVR.exe --screenshot FILE.png [--tab play|options|dlss|controls|online|about]   one frame, no START
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
    std::string tab = "play";
    json online = json::object();  // newest answer of `bbvr_cli.py online view` (UI thread owns it)
    json online_incoming;
    bool online_has_incoming = false, online_have = false;
    std::string online_msg;        // result of the last create / join / leave
    bool online_msg_bad = false;
    double online_poll_at = -100;
    char online_code[16] = "";
    bool alpha_ack = false;         // OK pressed on the early-alpha notice (the state catches up a moment later)
    bool lobby_edit = false;        // the small "Change" under Play online: show the lobby address field
    char online_addr[160] = "";     // lobby address typed by the player (never shipped; saved with `online server`)
    bool online_open = false;       // the Play tab shows the Play online step instead of the profiles
    bool online_start = false;      // a host / join worked: start the game with the room
    bool online_mode_seen = false;  // the remembered choice was applied once
    std::string demo_flow;          // screenshots only: open | room | running | noserver
    std::string group = "display";
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
    while (A.log.size() > 3000) A.log.pop_front();
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

// Online tab: `bbvr_cli.py online <args>` (create | join CODE | leave | view). Anything but `view` is followed by a
// `view`, whose answer fills the tab; the first answer's message (errors) is shown under the buttons.
static void start_game(bool solo);
static void online_async(std::vector<std::string> args, std::string then_mode = "")
{
    A.cli_busy++;
    std::thread([args, then_mode] {
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
        if (!then_mode.empty() && first.value("ok", false)) run({"mode", then_mode});  // remember Host / Join
        json view = args[0] == "view" ? first : run({"view"});
        std::lock_guard<std::mutex> l2(A.mu);
        if (args[0] != "view") {
            A.online_msg = first.value("ok", false) ? "" : (first.contains("message") && first["message"].is_string() ? first["message"].get<std::string>() : "That did not work.");
            A.online_msg_bad = !A.online_msg.empty();
            if (!then_mode.empty() && first.value("ok", false)) A.online_start = true;  // host / join worked: start the game
        }
        A.online_incoming = view;
        A.online_has_incoming = true;
        A.cli_busy--;
    }).detach();
}

static std::string default_save_body(bool values)
{
    json b = json::object();
    if (values && A.st.contains("values")) b["values"] = A.st["values"];
    b["game_running"] = A.running.load();
    return b.dump();
}

static void start_game(bool solo)
{
    if (A.running || !A.env.ok) return;
    {
        std::lock_guard<std::mutex> lock(A.mu);
        A.log.clear();
        A.partial.clear();
    }
    bool send_values = A.dirty.exchange(false);
    std::string body = default_save_body(send_values);
    A.crash_base = A.st.value("crash", json::object()).value("count", 0);
    A.crash_new.clear();
    A.running = true;
    if (A.job) CloseHandle(A.job);
    A.job = make_job();
    HANDLE job = A.job;
    std::string game = A.st.value("app", json::object()).value("game_dir", "");
    std::thread([body, job, game, solo] {
        {  // the launcher reads the saved settings: make sure the last edit is on disk first
            std::lock_guard<std::mutex> lock(A.cli_mu);
            std::string ignore;
            run_capture(A.env, {A.env.script_dir + L"\\bbvr_cli.py", L"save"}, body, [&](const std::string& c) { ignore += c; });
        }
        log_add("Starting. Put the headset on when the game says so.\n");
        std::vector<std::wstring> args = {A.env.script_dir + L"\\bbvr_launcher.py", L"--no-menu"};
        if (solo) args.push_back(L"--solo");  // Play solo: online off even when a room is saved
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
// theme

static ImU32 C(unsigned rgb, float a = 1.0f)
{
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(a * 255));
}
static ImVec4 CV(unsigned rgb, float a = 1.0f)
{
    return ImVec4(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
enum : unsigned {
    BG = 0x0d0f14, PANEL = 0x151922, PANEL2 = 0x1b2030, BORDER = 0x262c3b, TEXT = 0xe9ecf2, MUTED = 0x8b93a7,
    ACCENT = 0xe3262f, ACCENT_H = 0xf23a43, GREEN = 0x3ecf8e, AMBER = 0xf5a524, BLUE = 0x5b8def
};

static ImFont *F_REG, *F_SEMI, *F_MONO;

static void setup_theme()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = S(12);
    s.ChildRounding = S(12);
    s.FrameRounding = S(8);
    s.PopupRounding = S(10);
    s.GrabRounding = S(8);
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
    c[ImGuiCol_ChildBg] = CV(PANEL);
    c[ImGuiCol_PopupBg] = CV(PANEL);
    c[ImGuiCol_Border] = CV(BORDER);
    c[ImGuiCol_FrameBg] = CV(PANEL2);
    c[ImGuiCol_FrameBgHovered] = CV(0x232a3d);
    c[ImGuiCol_FrameBgActive] = CV(0x2a3248);
    c[ImGuiCol_SliderGrab] = CV(ACCENT);
    c[ImGuiCol_SliderGrabActive] = CV(ACCENT_H);
    c[ImGuiCol_CheckMark] = CV(ACCENT);
    c[ImGuiCol_Button] = CV(PANEL2);
    c[ImGuiCol_ButtonHovered] = CV(0x2a3248);
    c[ImGuiCol_ButtonActive] = CV(0x33405c);
    c[ImGuiCol_Header] = CV(0x2a3248);
    c[ImGuiCol_HeaderHovered] = CV(0x323b55);
    c[ImGuiCol_HeaderActive] = CV(0x3a4565);
    c[ImGuiCol_ScrollbarBg] = CV(PANEL, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = CV(0x2f364a);
    c[ImGuiCol_ScrollbarGrabHovered] = CV(0x3b4560);
    c[ImGuiCol_ScrollbarGrabActive] = CV(0x4a5575);
    c[ImGuiCol_Separator] = CV(BORDER);
    c[ImGuiCol_TableHeaderBg] = CV(PANEL2);
    c[ImGuiCol_TableBorderStrong] = CV(BORDER);
    c[ImGuiCol_TableBorderLight] = CV(BORDER, 0.6f);
    c[ImGuiCol_TableRowBgAlt] = CV(0xffffff, 0.02f);
    c[ImGuiCol_ModalWindowDimBg] = CV(0x05050a, 0.75f);
    c[ImGuiCol_TextSelectedBg] = CV(ACCENT, 0.35f);
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
    bool sys = F_REG != nullptr;
    if (!F_REG) F_REG = io.Fonts->AddFontDefault();
    if (!F_SEMI) F_SEMI = F_REG;
    if (!F_MONO) F_MONO = F_REG;
    return sys;
}

// ---------------------------------------------------------------------------------------------
// widgets

static bool BeginCard(const char* id, ImVec2 size, unsigned border = BORDER)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, CV(PANEL));
    ImGui::PushStyleColor(ImGuiCol_Border, CV(border));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, S(12));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(16), S(14)));
    return ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
}
static void EndCard()
{
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

static void Heading(const char* t)
{
    ImGui::PushFont(F_SEMI, S(13));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(MUTED));
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
    ImU32 bg = *v ? C(ACCENT, a) : C(hot ? 0x2a3248 : 0x232a3d, a);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), C(BORDER), h * 0.5f);
    float r = h * 0.5f - S(3);
    dl->AddCircleFilled(ImVec2(*v ? p.x + w - h * 0.5f : p.x + h * 0.5f, p.y + h * 0.5f), r, C(0xffffff, *v ? 1.0f : 0.8f));
    return pressed;
}

static bool Button(const char* label, ImVec2 size, bool primary = false, bool outline = false)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, S(8));
    if (primary) {
        ImGui::PushStyleColor(ImGuiCol_Button, CV(ACCENT));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CV(ACCENT_H));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CV(0xb81d25));
        ImGui::PushStyleColor(ImGuiCol_Border, CV(ACCENT, 0));
    } else if (outline) {
        ImGui::PushStyleColor(ImGuiCol_Button, CV(PANEL));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CV(0x232a3d));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CV(0x2a3248));
        ImGui::PushStyleColor(ImGuiCol_Border, CV(0x3a4258));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, CV(PANEL2));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, CV(0x2a3248));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, CV(0x33405c));
        ImGui::PushStyleColor(ImGuiCol_Border, CV(BORDER));
    }
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar();
    return r;
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

// ---------------------------------------------------------------------------------------------
// This PC rows

static std::string jstr(const json& j, const char* k, const std::string& d = "")
{
    return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : d;
}

static void folder_cb(void*, const char* const* list, int)
{
    if (list && list[0]) cli_async({"game-dir", list[0]});
}

struct Row {
    unsigned color;
    std::string title, detail;
    const char* button = nullptr;
    bool primary = false;
    int action = 0;
};

static void draw_rows(const std::vector<Row>& rows, float width)
{
    int i = 0;
    for (const Row& r : rows) {
        ImGui::PushID(i++);
        ImVec2 start = ImGui::GetCursorPos();
        Dot(r.color);
        ImGui::SetCursorPos(ImVec2(start.x + S(24), start.y));
        ImGui::PushFont(F_SEMI, S(15));
        ImGui::TextUnformatted(r.title.c_str());
        ImGui::PopFont();
        float bw = r.button ? S(120) : 0;
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
                if (r.action == 2) cli_async({"state"});
                if (r.action == 3) open_folder(jstr(A.st.value("crash", json::object()), "dir"));
            }
        }
        ImGui::SetCursorPos(ImVec2(start.x, start.y + S(46)));
        ImGui::PopID();
    }
}

static std::vector<Row> pc_rows()
{
    std::vector<Row> rows;
    const json app = A.st.value("app", json::object());
    const json caps = A.st.value("caps", json::object());
    bool eboot = app.value("eboot", false);
    std::string game = jstr(app, "game_dir");
    Row g;
    g.title = "Game dump";
    g.color = eboot ? GREEN : ACCENT;
    g.detail = eboot ? game + "  (eboot.bin found)" : (game.empty() ? "Not set. Choose the folder of YOUR dump that contains eboot.bin." : "No eboot.bin in " + game);
    g.button = eboot ? "Change..." : "Choose folder";
    g.primary = !eboot;
    g.action = 1;
    rows.push_back(g);

    Row v;
    v.title = "VR runtime (OpenXR)";
    std::string active = jstr(caps, "active_runtime");
    size_t nrt = caps.contains("runtimes") ? caps["runtimes"].size() : 0;
    v.color = active.empty() ? (nrt ? AMBER : ACCENT) : GREEN;
    v.detail = !active.empty() ? "Active: " + active : (nrt ? "Installed but none is the active runtime. Set your headset software as the OpenXR runtime."
                                                            : "None found. Install your headset software and set it as the OpenXR runtime.");
    rows.push_back(v);

    Row gpu;
    gpu.title = "Graphics card";
    std::string names;
    bool rtx = false;
    if (caps.contains("gpus"))
        for (auto& x : caps["gpus"]) {
            names += (names.empty() ? "" : ", ") + jstr(x, "name");
            rtx = rtx || x.value("rtx", false);
        }
    gpu.color = names.empty() ? AMBER : GREEN;
    gpu.detail = names.empty() ? "Not detected" : names + (rtx ? "  (NVIDIA RTX: DLSS possible)" : "");
    rows.push_back(gpu);

    Row d;
    d.title = "DLSS files";
    const json files = app.value("dlss_files", json::object());
    bool a = files.value("nvngx_dlss.dll", false), b = files.value("bbport_dlss.dll", false);
    d.color = (a && b) ? GREEN : AMBER;
    std::string missing;
    if (!a) missing += "nvngx_dlss.dll";
    if (!b) missing += std::string(missing.empty() ? "" : ", ") + "bbport_dlss.dll";
    d.detail = (a && b) ? "bin\\nvngx_dlss.dll and bin\\bbport_dlss.dll found" : "Missing: " + missing + " (they ship in bin/ with the release; reinstall or unzip again). Without them the game uses FSR 1.";
    rows.push_back(d);

    Row c;
    c.title = "Crash reports";
    const json cr = A.st.value("crash", json::object());
    int ncrash = cr.value("count", 0);
    c.color = !cr.value("available", false) ? MUTED : (ncrash ? AMBER : GREEN);
    c.detail = !cr.value("available", false) ? "Not part of this build"
               : std::to_string(ncrash) + " saved in " + jstr(cr, "dir") + ". Never sent automatically.";
    if (cr.value("available", false)) {
        c.button = "Open folder";
        c.action = 3;
    }
    rows.push_back(c);
    return rows;
}

// ---------------------------------------------------------------------------------------------
// option widgets

static bool dlss_ok()
{
    return A.st.value("caps", json::object()).value("dlss", json::object()).value("ok", false);
}

static bool draw_option(const json& o, json& values)
{
    std::string id = o["id"], kind = o["kind"];
    bool changed = false;
    json& v = values[id];
    std::string off;
    if (o.contains("only_if"))
        for (auto it = o["only_if"].begin(); it != o["only_if"].end(); ++it)
            if (!(values.contains(it.key()) && values[it.key()] == it.value())) off = "only with " + it.key() + " = " + it.value().dump();
    bool disabled = !off.empty();

    float total = ImGui::GetContentRegionAvail().x;
    float ctrl_w = std::min(S(300), total * 0.42f);
    float lab_w = total - ctrl_w - S(16);
    ImVec2 top = ImGui::GetCursorPos();

    ImGui::PushID(id.c_str());
    // label column
    ImGui::BeginGroup();
    ImGui::PushFont(F_SEMI, S(15));
    ImGui::TextUnformatted(jstr(o, "label").c_str());
    ImGui::PopFont();
    ImGui::SameLine();
    bool live = o.value("live", false);
    ImGui::PushFont(F_SEMI, S(11));
    ImGui::PushStyleColor(ImGuiCol_Text, CV(live ? BLUE : MUTED));
    ImGui::TextUnformatted(live ? "LIVE" : "RESTART");
    ImGui::PopStyleColor();
    ImGui::PopFont();
    std::string help = jstr(o, "help");
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

    // control column
    ImGui::SetCursorPos(ImVec2(top.x + total - ctrl_w, top.y));
    ImGui::SetNextItemWidth(ctrl_w);
    if (disabled) ImGui::BeginDisabled();
    ImGui::PushFont(F_REG, S(15));
    if (kind == "bool") {
        bool b = v.is_boolean() && v.get<bool>();
        if (Toggle("##t", &b, disabled)) {
            v = b;
            changed = true;
        }
        if (v.is_null()) {
            ImGui::SameLine();
            Muted("game default");
        }
    } else if (kind == "choice" || kind == "runtime") {
        std::vector<std::pair<std::string, std::string>> items;
        const json caps = A.st.value("caps", json::object());
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
                items.push_back({cs, label});
            }
        }
        std::string cur = v.is_string() ? v.get<std::string>() : "";
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
    ImGui::SetCursorPos(ImVec2(top.x, top.y + std::max(lab_h, ctrl_h) + S(8)));
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + total, p.y), C(BORDER, 0.7f));
    ImGui::Dummy(ImVec2(0, S(8)));
    ImGui::PopID();
    return changed;
}

static void draw_options_list(const std::string& group, bool dlss_tab)
{
    if (!A.st.contains("options") || !A.st.contains("values")) return;
    bool any = false;
    bool changed = false;
    for (const json& o : A.st["options"]) {
        if (jstr(o, "tab") != group) continue;
        if (o.value("advanced", false) && !A.advanced) continue;
        any = true;
        changed |= draw_option(o, A.st["values"]);
    }
    if (!any) Muted("Nothing here. Switch on \"Show advanced options\" for more.");
    if (changed) {
        A.dirty = true;
        A.dirty_at = ImGui::GetTime();
    }
    (void)dlss_tab;
}

// ---------------------------------------------------------------------------------------------
// tabs

// ---- Play online / Play solo (the start flow) ------------------------------------------------------------------
// Play solo starts the launcher with --solo. Play online opens a step: Host a game (creates a room, shows the code,
// starts the game as the host), Join a friend (code, joins, starts as a guest), or - already in a room - Start / Leave.
// The code stays on the Play tab, big, with a Copy button, for as long as the room lasts.

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

static void online_start_card(ImVec2 size, bool ready)
{
    const bool busy = A.cli_busy > 0;
    const json& v = A.online;
    const std::string step = jstr(A.st.value("app", json::object()), "online_step", "host");
    if (BeginCard("onlinestart", size)) {
        Heading("Play online");
        if (online_in_room()) {
            std::string code = jstr(v, "code");
            big_code(code, 40);
            MutedWrapped(jstr(v, "role") == "host" ? "You host this room. Give the code to up to two friends." : "You are a guest in this room.", 13);
            ImGui::Dummy(ImVec2(0, S(6)));
            copy_code_button(code, S(120));
            ImGui::SameLine();
            if (busy) ImGui::BeginDisabled();
            if (Button(jstr(v, "role") == "host" ? "Close room" : "Leave", ImVec2(S(110), S(34)), false, true)) online_async({"leave"});
            if (busy) ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, S(8)));
            if (!ready) ImGui::BeginDisabled();
            ImGui::PushFont(F_SEMI, S(18));
            if (Button("Start with this room", ImVec2(-FLT_MIN, S(46)), true) && ready) start_game(false);
            ImGui::PopFont();
            if (!ready) ImGui::EndDisabled();
        } else if (online_no_server()) {
            lobby_address_field(busy);
        } else {
            if (!ready) ImGui::BeginDisabled();
            if (busy) ImGui::BeginDisabled();
            if (Button("Host a game", ImVec2(-FLT_MIN, S(44)), step != "join")) online_async({"create"}, "host");
            if (busy) ImGui::EndDisabled();
            if (!ready) ImGui::EndDisabled();
            MutedWrapped("Opens a room and shows its code. Friends join with it.", 12);
            ImGui::Dummy(ImVec2(0, S(8)));
            Heading("Join a friend");
            ImGui::SetNextItemWidth(S(150));
            ImGui::PushFont(F_MONO, S(20));
            ImGui::InputTextWithHint("##startcode", "K7Q-M4X", A.online_code, sizeof(A.online_code), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
            ImGui::PopFont();
            ImGui::SameLine();
            bool off = busy || !ready || !A.online_code[0];
            if (off) ImGui::BeginDisabled();
            if (Button("Join", ImVec2(S(80), S(34)), step == "join")) online_async({"join", A.online_code}, "join");
            if (off) ImGui::EndDisabled();
            lobby_line(busy);
        }
        if (busy) Muted("Working...");
        online_messages();
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - S(48));
        if (Button("Back", ImVec2(S(80), S(32)), false, true)) A.online_open = false;
    }
    EndCard();
}

// While a room exists the code stays on the Play tab (also while the game runs).
static void room_code_card(ImVec2 size)
{
    if (BeginCard("roomcode", size)) {
        Heading(jstr(A.online, "role") == "host" ? "Your room (host)" : "Your room (guest)");
        std::string code = jstr(A.online, "code");
        big_code(code, 34);
        ImGui::SameLine(0, S(14));
        copy_code_button(code, S(110));
        int n = (int)A.online.value("members", json::array()).size();
        if (n > 0) Muted((std::to_string(n) + " of " + std::to_string(A.online.value("max", 3)) + " in the room").c_str());
    }
    EndCard();
}

// Play tab, under the profile cards: "Low fps? Try Performance" on a PC without an RTX GPU, and the 3D / Flat screen
// (mono) choice. The mode is the Stereo mode option; it is saved through the same bbvr_cli save path as every option
// (so the profile becomes Custom, which keeps all the other values).
static void play_view_row(const std::string& cur)
{
    const json caps = A.st.value("caps", json::object());
    bool any_rtx = false;
    for (const json& g : caps.value("gpus", json::array())) any_rtx |= g.value("rtx", false);
    if (!any_rtx && cur != "Performance") {
        ImGui::PushStyleColor(ImGuiCol_Text, CV(AMBER));
        ImGui::PushFont(F_REG, S(13));
        ImGui::TextUnformatted("Low fps? Try Performance.");
        ImGui::PopFont();
        ImGui::PopStyleColor();
    }
    if (!A.st.contains("values")) return;
    const std::string mode = jstr(A.st["values"], "mode", "alt");
    ImGui::PushFont(F_SEMI, S(15));
    ImGui::TextUnformatted("View");
    ImGui::PopFont();
    const struct { const char* label; const char* value; } choices[] = {{"3D", "alt"}, {"Flat screen (mono)", "mono"}};
    float w = (ImGui::GetContentRegionAvail().x - S(8)) / 2;
    for (int i = 0; i < 2; i++) {
        if (i) ImGui::SameLine(0, S(8));
        if (Button(choices[i].label, ImVec2(w, S(32)), mode == choices[i].value) && mode != choices[i].value) {
            json vals = A.st["values"];
            vals["mode"] = choices[i].value;
            json b = {{"values", vals}, {"game_running", A.running.load()}};
            A.dirty = false;
            cli_async({"save"}, b.dump());
            A.st["values"]["mode"] = choices[i].value;
            A.st["profile"] = "Custom";
        }
    }
}

static void tab_play(ImVec2 avail)
{
    float gap = S(16);
    float left_w = std::floor((avail.x - gap) * 0.56f);
    float right_w = avail.x - gap - left_w;
    float pc_h = S(34) + 5 * S(46) + S(26);

    ImGui::BeginGroup();
    if (BeginCard("pc", ImVec2(left_w, pc_h))) {
        Heading("This PC");
        draw_rows(pc_rows(), left_w - S(34));
    }
    EndCard();
    if (BeginCard("log", ImVec2(left_w, avail.y - pc_h - S(12)))) {
        Heading("Session log");
        ImGui::PushFont(F_MONO, S(13));
        ImGui::PushStyleColor(ImGuiCol_Text, CV(0xb8bfd0));
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
    ImGui::EndGroup();

    ImGui::SameLine(0, gap);
    ImGui::BeginGroup();
    float btn_h = S(58);
    const bool play_online = A.have_state && A.st.value("app", json::object()).value("online_play", true);
    if (A.online_start && !A.running) {  // Host a game / Join a friend worked
        A.online_start = false;
        start_game(false);
    }
    if (g_demo && A.demo_flow == "noserver" && !A.online_have) {
        A.online = json{{"ok", false}, {"error", "no_server"}};
        A.online_have = true;
    }
    if (play_online && !A.online_mode_seen) {  // the last choice is remembered: online players land on the online step
        A.online_mode_seen = true;
        A.online_open = A.st.value("app", json::object()).value("play_mode", "solo") == "online";
    }
    if (!A.online_have) poll_online();
    const bool room_up = play_online && online_in_room();
    if (play_online && (A.online_open || room_up)) poll_online();
    const float room_h = (room_up && (!A.online_open || A.running)) ? S(120) : 0;
    const float below = A.running ? btn_h + S(52) : (play_online ? 2 * btn_h + S(64) : btn_h + S(52));
    if (A.online_open && !A.running) {
        const json app0 = A.st.value("app", json::object());
        online_start_card(ImVec2(right_w, avail.y), app0.value("eboot", false) && app0.value("accepted", false) && A.env.ok);
    } else {
    if (BeginCard("profiles", ImVec2(right_w, avail.y - below - room_h - (room_h > 0 ? S(8) : 0)))) {
        Heading("Profile");
        std::string cur = jstr(A.st, "profile", "Default");
        const json notes = A.st.value("profile_notes", json::object());
        if (A.st.contains("profiles")) {
            int n = 0;
            for (auto& pj : A.st["profiles"]) {
                std::string p = pj.get<std::string>();
                if (p == "Custom" && cur != "Custom") continue;
                n++;
            }
            const float view_h = S(84);  // the hint and the 3D / Flat screen row under the cards
            float ch = std::min(S(88), (ImGui::GetContentRegionAvail().y - view_h - (n - 1) * ImGui::GetStyle().ItemSpacing.y - S(6)) / std::max(n, 1));
            for (auto& pj : A.st["profiles"]) {
                std::string p = pj.get<std::string>();
                if (p == "Custom" && cur != "Custom") continue;
                bool sel = p == cur;
                ImGui::PushID(p.c_str());
                ImVec2 pos = ImGui::GetCursorScreenPos();
                float cw = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton("##card", ImVec2(cw, ch));
                bool hot = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked() && !sel) {
                    json b = {{"profile", p}, {"game_running", A.running.load()}};
                    A.dirty = false;
                    cli_async({"save"}, b.dump());
                    A.st["profile"] = p;
                }
                ImDrawList* dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(pos, ImVec2(pos.x + cw, pos.y + ch), sel ? C(ACCENT, 0.10f) : C(hot ? 0x1d2334 : PANEL2), S(10));
                dl->AddRect(pos, ImVec2(pos.x + cw, pos.y + ch), sel ? C(ACCENT) : C(BORDER), S(10), 0, sel ? 2.0f : 1.0f);
                dl->AddCircle(ImVec2(pos.x + cw - S(20), pos.y + S(20)), S(8), sel ? C(ACCENT) : C(0x3a4258), 0, 2.0f);
                if (sel) dl->AddCircleFilled(ImVec2(pos.x + cw - S(20), pos.y + S(20)), S(4.5f), C(ACCENT));
                dl->AddText(F_SEMI, S(17), ImVec2(pos.x + S(14), pos.y + S(9)), C(TEXT), p.c_str());
                std::string note = jstr(notes, p.c_str());
                dl->AddText(F_REG, S(13), ImVec2(pos.x + S(14), pos.y + S(32)), C(MUTED), note.c_str(), nullptr, cw - S(48));
                ImGui::PopID();
            }
            play_view_row(cur);
        }
    }
    EndCard();
    if (room_h > 0) {
        ImGui::Dummy(ImVec2(0, S(2)));
        room_code_card(ImVec2(right_w, room_h));
    }
    }
    const json app = A.st.value("app", json::object());
    bool ready = app.value("eboot", false) && app.value("accepted", false) && A.env.ok;
    const bool last_online = app.value("play_mode", "solo") == "online";
    if (!(A.online_open && !A.running)) {
        ImGui::Dummy(ImVec2(0, S(2)));
        if (A.running) {
            if (Button("Stop the game", ImVec2(right_w, btn_h), false, true)) stop_game();
        } else if (!play_online) {
            if (!ready) ImGui::BeginDisabled();
            ImGui::PushFont(F_SEMI, S(21));
            if (Button("START VR", ImVec2(right_w, btn_h), true) && ready) start_game(false);
            ImGui::PopFont();
            if (!ready) ImGui::EndDisabled();
        } else {
            if (!ready) ImGui::BeginDisabled();
            ImGui::PushFont(F_SEMI, S(21));
            if (Button("Play solo", ImVec2(right_w, btn_h), !last_online) && ready) {
                A.st["app"]["play_mode"] = "solo";
                online_async({"mode", "solo"});
                start_game(true);
            }
            if (Button("Play online", ImVec2(right_w, btn_h), last_online)) A.online_open = true;
            ImGui::PopFont();
            if (!ready) ImGui::EndDisabled();
        }
    }
    if (A.running) Muted("The game is running. Put the headset on.");
    else if (!A.env.ok) Muted(A.env.problem.substr(0, 80).c_str());
    else if (!app.value("eboot", false)) Muted("Choose your game folder first.");
    else if (!app.value("accepted", false)) Muted("Accept the note to continue.");
    else {
        std::string w;
        if (A.st.contains("warnings") && !A.st["warnings"].empty()) w = A.st["warnings"][0].get<std::string>();
        if (!w.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, CV(AMBER));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + right_w);
            ImGui::PushFont(F_REG, S(13));
            ImGui::TextUnformatted(w.c_str());
            ImGui::PopFont();
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else
            Muted("Put the headset on after you press START.");
    }
    ImGui::EndGroup();
}

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

// Update switch shared by the About tab and the Options page (Advanced).
static void update_switch()
{
    const json up = A.st.value("update", json::object());
    bool on = up.value("enabled", true);
    bool changed = false;
    switch_row("updsw", "Check for updates at start", "Asks GitHub (the release list; nothing about you is sent) whether a newer version exists and says so here. Switch it off to never connect.", &on, changed);
    if (changed) cli_async({"update-opt", on ? "on" : "off"});
}

static void tab_options(ImVec2 avail)
{
    static const struct { const char* id; const char* label; } groups[] = {
        {"display", "Display / VR"}, {"comfort", "Comfort"}, {"combat", "Combat"}, {"body", "Body / arms"},
        {"ui", "UI"}, {"patches", "Patches"}, {"advanced", "Advanced"}};
    float side = S(200);
    ImGui::BeginGroup();
    if (BeginCard("groups", ImVec2(side, avail.y))) {
        for (auto& g : groups) {
            bool sel = A.group == g.id;
            ImGui::PushStyleColor(ImGuiCol_Button, sel ? CV(ACCENT, 0.16f) : CV(PANEL));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? CV(ACCENT, 0.22f) : CV(PANEL2));
            ImGui::PushStyleColor(ImGuiCol_Text, sel ? CV(TEXT) : CV(MUTED));
            ImGui::PushStyleColor(ImGuiCol_Border, CV(ACCENT, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5f));
            ImGui::PushFont(sel ? F_SEMI : F_REG, S(15));
            if (ImGui::Button(g.label, ImVec2(-1, S(36)))) A.group = g.id;
            ImGui::PopFont();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
        }
        ImGui::Dummy(ImVec2(0, S(10)));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PushFont(F_REG, S(14));
        bool adv = A.advanced;
        if (Toggle("##adv", &adv)) A.advanced = adv;
        ImGui::SameLine();
        ImGui::TextUnformatted("Advanced");
        ImGui::PopFont();
    }
    EndCard();
    ImGui::EndGroup();
    ImGui::SameLine(0, S(16));
    if (BeginCard("opts", ImVec2(avail.x - side - S(16), avail.y))) {
        draw_options_list(A.group, false);
        if (A.group == "advanced") {
            ImGui::Dummy(ImVec2(0, S(8)));
            update_switch();
        }
        if (jstr(A.st, "profile") == "Custom") {
            ImGui::Dummy(ImVec2(0, S(4)));
            Muted("Profile: Custom (your edits). Pick another profile on the Play tab to go back.");
        }
    }
    EndCard();
}

static void tab_dlss(ImVec2 avail)
{
    if (BeginCard("dlss", avail)) {
        const json dl = A.st.value("caps", json::object()).value("dlss", json::object());
        bool ok = dlss_ok();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        std::string msg = ok ? std::string("DLSS is available.") + (A.st.value("dlss_native", false) ? "" : " (using the DLSS-capable build next to the game until the main executable is rebuilt)")
                             : "DLSS is greyed out: " + jstr(dl, "reason", "not available");
        ImGui::PushFont(F_REG, S(14));
        float th = ImGui::CalcTextSize(msg.c_str(), nullptr, false, w - S(28)).y;
        ImDrawList* d = ImGui::GetWindowDrawList();
        d->AddRectFilled(p, ImVec2(p.x + w, p.y + th + S(20)), C(ok ? GREEN : AMBER, 0.10f), S(8));
        d->AddRect(p, ImVec2(p.x + w, p.y + th + S(20)), C(ok ? GREEN : AMBER, 0.5f), S(8));
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(14), p.y + S(10)));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + w - S(28));
        ImGui::TextUnformatted(msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + th + S(30)));
        MutedWrapped(jstr(A.st, "dlss_note"));
        ImGui::Dummy(ImVec2(0, S(8)));
        draw_options_list("dlss", true);
    }
    EndCard();
}

static unsigned online_state_color(const std::string& st)
{
    if (st == "in world") return GREEN;
    if (st == "sign up") return AMBER;
    if (st == "host") return ACCENT;
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
    const bool busy = A.cli_busy > 0;
    const json& v = A.online;
    const bool in_room = v.value("in_room", false);
    float gap = S(16), lw = std::floor((avail.x - gap) * 0.5f);
    ImGui::BeginGroup();
    if (BeginCard("room", ImVec2(lw, avail.y))) {
        if (!A.online_have) {
            Muted("Reading the room state...");
        } else if (!v.value("ok", true) || (v.contains("error") && !v.contains("in_room"))) {
            Heading("Room");
            MutedWrapped(online_no_server() ? "No lobby set. Enter the address of your lobby." : jstr(v, "message", "The room server is not reachable."), 14);
            if (online_no_server()) lobby_address_field(busy);
        } else if (in_room) {
            Heading("Room code");
            std::string code = jstr(v, "code");
            ImGui::PushFont(F_MONO, S(46));
            ImGui::PushStyleColor(ImGuiCol_Text, CV(TEXT));
            ImGui::TextUnformatted(code.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            MutedWrapped(jstr(v, "role") == "host" ? "You are the host. Give this code to your friends." : "You are a guest in this room.", 14);
            ImGui::Dummy(ImVec2(0, S(6)));
            if (Button("Copy code", ImVec2(S(130), S(36)), true)) {
                ImGui::SetClipboardText(code.c_str());
                std::lock_guard<std::mutex> lock(A.mu);
                A.online_msg = "Code copied.";
                A.online_msg_bad = false;
            }
            ImGui::SameLine();
            if (busy) ImGui::BeginDisabled();
            if (Button(jstr(v, "role") == "host" ? "Close room" : "Leave room", ImVec2(S(150), S(36)), false, true)) online_async({"leave"});
            if (busy) ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, S(10)));
            if (v.value("closed", false)) MutedWrapped("The room has closed.", 14);
        } else {
            Heading("Join a room");
            MutedWrapped("Type the code your host gave you, like K7Q-M4X.", 14);
            ImGui::Dummy(ImVec2(0, S(4)));
            ImGui::SetNextItemWidth(S(190));
            ImGui::PushFont(F_MONO, S(22));
            ImGui::InputTextWithHint("##code", "K7Q-M4X", A.online_code, sizeof(A.online_code), ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CharsNoBlank);
            ImGui::PopFont();
            ImGui::SameLine();
            if (busy || !A.online_code[0]) ImGui::BeginDisabled();
            if (Button("Join", ImVec2(S(100), S(36)), true)) online_async({"join", A.online_code});
            if (busy || !A.online_code[0]) ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, S(14)));
            Heading("Or host one");
            MutedWrapped("Open a room and give its code to up to two friends.", 14);
            ImGui::Dummy(ImVec2(0, S(4)));
            if (busy) ImGui::BeginDisabled();
            if (Button("Create room", ImVec2(S(150), S(36)), false, true)) online_async({"create"});
            if (busy) ImGui::EndDisabled();
            if (v.value("closed", false)) {
                ImGui::Dummy(ImVec2(0, S(8)));
                MutedWrapped("Your last room closed" + (jstr(v, "reason").empty() ? std::string(".") : " (" + jstr(v, "reason") + ")."), 14);
            }
        }
        if (A.online_have && !in_room) lobby_line(busy);
        if (busy) {
            ImGui::Dummy(ImVec2(0, S(6)));
            Muted("Working...");
        }
        {
            std::lock_guard<std::mutex> lock(A.mu);
            if (!A.online_msg.empty()) {
                ImGui::Dummy(ImVec2(0, S(6)));
                ImGui::PushStyleColor(ImGuiCol_Text, CV(A.online_msg_bad ? ACCENT : GREEN));
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted(A.online_msg.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }
    }
    EndCard();
    ImGui::EndGroup();
    ImGui::SameLine(0, gap);
    if (BeginCard("members", ImVec2(avail.x - lw - gap, avail.y))) {
        Heading("Members");
        if (!in_room) {
            MutedWrapped("Nobody yet. Create or join a room to see who is in it.", 14);
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
}

static void tab_controls(ImVec2 avail)
{
    static const char* rows[][2] = {
        {"Left stick", "Move"},
        {"Right stick", "Camera / turn (smooth or snap, see Options > Comfort)"},
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
        MutedWrapped("An unofficial fan project that runs YOUR copy of Bloodborne (PS4, CUSA03173, version 1.09) natively on Windows with a Vulkan renderer and an OpenXR bridge for VR headsets. Alpha, single-player.");
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
        MutedWrapped("The dashboard and the web settings page edit the same files (user\\bbvr_settings.json, out\\fp.txt, stereo.txt), so you can use either. Changed control files keep a .bak of the original.");
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
// frame

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
    float mx = S(24);

    // header
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(w, S(64)), C(0x10131a));
    dl->AddLine(ImVec2(0, S(64)), ImVec2(w, S(64)), C(BORDER));
    ImVec2 tsz = F_SEMI->CalcTextSizeA(S(26), FLT_MAX, 0, "BLOODBORNE");
    dl->AddText(F_SEMI, S(26), ImVec2(mx, S(15)), C(TEXT), "BLOODBORNE");
    float tx = mx + tsz.x + S(10);
    dl->AddRectFilled(ImVec2(tx, S(19)), ImVec2(tx + S(34), S(45)), C(ACCENT), S(7));
    ImVec2 vsz = F_SEMI->CalcTextSizeA(S(16), FLT_MAX, 0, "VR");
    dl->AddText(F_SEMI, S(16), ImVec2(tx + (S(34) - vsz.x) * 0.5f, S(32) - vsz.y * 0.5f), C(0xffffff), "VR");
    std::string ver = jstr(A.st.value("app", json::object()), "version");
    if (ver.empty()) ver = BBVR_VERSION;
    std::string status = A.running ? "Game running" : (A.have_state ? "Ready" : "Loading...");
    unsigned sc = A.running ? AMBER : (A.have_state ? GREEN : MUTED);
    ImVec2 ssz = F_REG->CalcTextSizeA(S(14), FLT_MAX, 0, status.c_str());
    ImVec2 vz = F_REG->CalcTextSizeA(S(14), FLT_MAX, 0, ver.c_str());
    float pw = ssz.x + S(40);
    float px = w - mx - pw;
    dl->AddRectFilled(ImVec2(px, S(18)), ImVec2(px + pw, S(46)), C(sc, 0.12f), S(14));
    dl->AddCircleFilled(ImVec2(px + S(16), S(32)), S(4.5f), C(sc));
    dl->AddText(F_REG, S(14), ImVec2(px + S(28), S(32) - ssz.y * 0.5f), C(sc), status.c_str());
    dl->AddText(F_REG, S(14), ImVec2(px - S(16) - vz.x, S(32) - vz.y * 0.5f), C(MUTED), ver.c_str());

    // tabs
    static const struct { const char* id; const char* label; } tabs[] = {{"play", "Play"}, {"options", "Options"}, {"dlss", "DLSS"}, {"controls", "Controls"}, {"online", "Online"}, {"about", "About"}};
    const bool show_online = A.have_state && (!A.st.value("app", json::object()).value("public", false) || A.st.value("app", json::object()).value("online_play", true));
    float x = mx;
    ImGui::SetCursorPos(ImVec2(0, 0));
    for (auto& t : tabs) {
        if (std::string(t.id) == "online" && !show_online) continue;
        ImVec2 sz = F_SEMI->CalcTextSizeA(S(16), FLT_MAX, 0, t.label);
        float bw = sz.x + S(32);
        ImGui::SetCursorScreenPos(ImVec2(x, S(70)));
        ImGui::PushID(t.id);
        ImGui::InvisibleButton("##tab", ImVec2(bw, S(40)));
        bool sel = A.tab == t.id, hot = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) A.tab = t.id;
        dl->AddText(sel ? F_SEMI : F_REG, S(16), ImVec2(x + S(16), S(90) - sz.y * 0.5f), sel ? C(TEXT) : C(hot ? 0xc5cbdb : MUTED), t.label);
        if (sel) dl->AddRectFilled(ImVec2(x + S(8), S(107)), ImVec2(x + bw - S(8), S(110)), C(ACCENT), S(2));
        ImGui::PopID();
        x += bw + S(4);
    }
    dl->AddLine(ImVec2(0, S(110)), ImVec2(w, S(110)), C(BORDER, 0.6f));

    // content
    ImVec2 avail(w - 2 * mx, h - S(124) - S(12));
    ImGui::SetCursorScreenPos(ImVec2(mx, S(122)));
    if (!A.crash_new.empty()) {  // the game crashed: say so plainly (the report is already saved)
        float bh = S(64);
        dl->AddRectFilled(ImVec2(mx, S(122)), ImVec2(mx + avail.x, S(122) + bh), C(0x2a1114), S(10));
        dl->AddRect(ImVec2(mx, S(122)), ImVec2(mx + avail.x, S(122) + bh), C(ACCENT), S(10));
        std::string msg = "The game crashed. A crash report was saved to " + A.crash_new + ". If you want to help, send that file to the developer.";
        ImGui::SetCursorScreenPos(ImVec2(mx + S(14), S(122) + S(8)));
        ImGui::PushTextWrapPos(mx + avail.x - S(280));
        ImGui::TextUnformatted(msg.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(mx + avail.x - S(260), S(122) + S(15)));
        if (Button("Open folder", ImVec2(S(120), S(34)), true)) open_folder(jstr(A.st.value("crash", json::object()), "dir"));
        ImGui::SameLine();
        if (Button("Dismiss", ImVec2(S(110), S(34)))) A.crash_new.clear();
        avail.y -= bh + S(10);
        ImGui::SetCursorScreenPos(ImVec2(mx, S(122) + bh + S(10)));
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
    } else if (A.tab == "play") tab_play(avail);
    else if (A.tab == "options") tab_options(avail);
    else if (A.tab == "dlss") tab_dlss(avail);
    else if (A.tab == "controls") tab_controls(avail);
    else if (A.tab == "online" && show_online) {
        if (ImGui::GetTime() - A.online_poll_at > 3.0 && A.cli_busy == 0) {  // the room changes under us: look again
            A.online_poll_at = ImGui::GetTime();
            online_async({"view"});
        }
        tab_online(avail);
    } else tab_about(avail);
    ImGui::EndChild();

    {  // toast for folder errors etc.
        std::lock_guard<std::mutex> lock(A.mu);
        std::string err = jstr(A.st, "error");
        if (!err.empty() && A.have_state) {
            float tw = S(520);
            ImVec2 p(w * 0.5f - tw * 0.5f, h - S(60));
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
        if (g_demo && (A.tab == "online" || A.demo_flow == "room" || A.demo_flow == "running")) {  // screenshots only
            A.online = json::parse(R"({"ok":true,"in_room":true,"code":"K7Q-M4X","role":"host","closed":false,"members":[
                {"name":"Hunter","role":"host","state":"host","you":true},{"name":"Friend A","role":"guest","state":"in world"},
                {"name":"Friend B","role":"guest","state":"sign up"}],"bell":{"state":"ready","rings":2,"next_ring_in_s":48}})");
        }
    }
    if (A.has_incoming && !A.dirty) {
        A.st = A.incoming;
        A.has_incoming = false;
        A.have_state = true;
        if (g_demo) {  // screenshots only: pretend the first-run steps are done (nothing is written)
            A.st["app"]["accepted"] = true;
            A.st["app"]["alpha_notice"] = false;
            A.st["app"]["eboot"] = true;
            A.st["app"]["game_dir"] = R"(C:\Dumps\Bloodborne\CUSA03173)";
            if (!A.demo_flow.empty()) A.st["app"]["play_mode"] = "online";
            if (A.demo_flow == "running") A.running = true;
        }
        const json cr = A.st.value("crash", json::object());
        if (!A.running && A.crash_base >= 0 && cr.value("count", 0) > A.crash_base) {  // the session that just ended crashed
            A.crash_new = jstr(cr, "latest");
            A.crash_base = -1;
        }
    } else if (A.has_incoming && !A.have_state) {
        A.st = A.incoming;
        A.has_incoming = false;
        A.have_state = true;
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
    bool web = false;
    std::vector<std::wstring> passthrough;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == L"--web") web = true;
        else if (args[i] == L"--screenshot" && i + 1 < args.size()) shot = U8(args[++i]);
        else if (args[i] == L"--tab" && i + 1 < args.size()) force_tab = U8(args[++i]);
        else if (args[i] == L"--group" && i + 1 < args.size()) A.group = U8(args[++i]);
        else if (args[i] == L"--advanced") A.advanced = true;
        else if (args[i] == L"--demo") g_demo = true;
        else if (args[i] == L"--flow" && i + 1 < args.size()) A.demo_flow = U8(args[++i]);  // screenshots: open | room | running | noserver
        else passthrough.push_back(args[i]);
    }
    if (web || !passthrough.empty()) {  // any launcher argument: the console launcher with the web menu
        if (!A.env.ok) {
            MessageBoxW(nullptr, W(A.env.problem).c_str(), L"Bloodborne VR", MB_OK | MB_ICONERROR);
            return 1;
        }
        return run_console_launcher(A.env, passthrough);
    }
    if (!force_tab.empty()) A.tab = force_tab;

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        MessageBoxA(nullptr, SDL_GetError(), "Bloodborne VR", MB_OK | MB_ICONERROR);
        return 1;
    }
    g_scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (g_scale < 1.0f || g_scale > 3.0f) g_scale = 1.0f;
    if (!shot.empty()) g_scale = 1.0f;
    SDL_Window* window = SDL_CreateWindow("Bloodborne VR", (int)S(1000), (int)S(680), SDL_WINDOW_RESIZABLE | (shot.empty() ? 0 : SDL_WINDOW_HIDDEN));
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        MessageBoxA(nullptr, SDL_GetError(), "Bloodborne VR", MB_OK | MB_ICONERROR);
        return 1;
    }
    A.window = window;
    SDL_SetWindowMinimumSize(window, (int)S(900), (int)S(620));
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
        SDL_SetRenderDrawColor(renderer, 13, 15, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        frames++;
        if (!shot.empty() && A.have_state && A.cli_busy == 0) settle++;
        if (!shot.empty() && (settle > 6 || frames > 2000)) {
            bool ok = save_png(renderer, shot.c_str());
            if (!ok) SDL_Log("screenshot failed: %s", SDL_GetError());
            done = true;
            break;
        }
        SDL_RenderPresent(renderer);
    }
    if (A.running) stop_game();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
