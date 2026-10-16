#include "webview/webview.h"
#include "core.hpp"
#include "hub.hpp"
#include "json.hpp"

#include <windows.h>
#include "WebView2.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <thread>
#include <fstream>
#include <filesystem>
#include <cwctype>

namespace fs = std::filesystem;

struct AppState {
    std::mutex               mx;
    std::vector<std::string> log;
    core::Config             cfg;
    std::wstring             byondDir, byondExe, dreamseekerExe, modsDir;

    std::string hubServersJson, hubServersErr;
    bool        hubServersLoading = false;
};

static AppState          g;
static std::atomic<bool> g_busy{false};

static void logLine(const std::string& s) {
    std::lock_guard<std::mutex> lk(g.mx);
    g.log.push_back(s);
    if (g.log.size() > 400)
        g.log.erase(g.log.begin(), g.log.begin() + (long)(g.log.size() - 400));
    static std::ofstream f(fs::path(core::exeDir() + L"\\detomatix.log"),
                           std::ios::app | std::ios::binary);
    if (f) { f << s << "\n"; f.flush(); }
}

struct Paths { std::wstring byondDir, byondExe, dreamseekerExe, modsDir; };

static void refreshPathsLocked() {
    if (g.cfg.byondDir.empty() || !core::fileExists(core::byondExe(g.cfg.byondDir))) {
        std::wstring det = core::detectByondDir();
        if (!det.empty()) g.cfg.byondDir = det;
    }
    g.byondDir       = g.cfg.byondDir;
    g.byondExe       = g.byondDir.empty() ? L"" : core::byondExe(g.byondDir);
    g.dreamseekerExe = g.byondDir.empty() ? L"" : core::dreamseekerExe(g.byondDir);
    g.modsDir        = core::modsDir();
}

static Paths refreshPaths() {
    std::lock_guard<std::mutex> lk(g.mx);
    refreshPathsLocked();
    return Paths{g.byondDir, g.byondExe, g.dreamseekerExe, g.modsDir};
}

static std::vector<core::Mod> loadMods() {
    std::wstring dir;
    std::map<std::string,bool> en;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        refreshPathsLocked();
        dir = g.modsDir;
        en  = g.cfg.modEnabled;
    }
    auto mods = core::scanMods(dir);
    for (auto& m : mods) {
        auto it = en.find(m.id);
        if (it != en.end()) m.enabled = it->second;
    }
    return mods;
}

struct Procs { DWORD byond = 0; DWORD ds = 0; };

static Procs procs() {
    std::map<std::wstring, DWORD> m;
    core::findPids({L"byond.exe", L"dreamseeker.exe"}, m);
    return Procs{m[L"byond.exe"], m[L"dreamseeker.exe"]};
}

static DWORD dsPid() { return procs().ds; }

static std::wstring lowerW(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static std::vector<std::wstring> enabledModDlls() {
    std::vector<std::wstring> out;
    for (auto& m : loadMods())
        if (m.enabled && !m.dllPath.empty() && core::fileExists(m.dllPath))
            out.push_back(m.dllPath);
    return out;
}

static std::string ensureBackend() {
    Procs pr = procs();
    if (pr.byond) return "byond.exe already running (pid " + std::to_string(pr.byond) + ")";
    Paths p = refreshPaths();
    if (p.byondExe.empty() || !core::fileExists(p.byondExe))
        return "byond.exe not found (set the BYOND directory in Settings)";
    std::wstring cwd = p.byondDir + L"\\bin";
    DWORD pid = 0;
    std::string err;
    if (!core::startProcess(p.byondExe, L"-minimize", cwd, pid, err))
        return "failed to start byond.exe: " + err;
    return "started byond.exe -minimize (pid " + std::to_string(pid) + ")";
}

static void injectInto(DWORD pid) {
    auto mods = loadMods();
    {
        std::vector<std::wstring> dlls;
        for (auto& m : mods)
            if (m.enabled && !m.dllPath.empty() && core::fileExists(m.dllPath))
                dlls.push_back(m.dllPath);
        std::wstring joined = core::setModPathsEnv(dlls);
        logLine("[mods] BYOND_MOD_PATHS = " + core::narrow(joined));
    }
    int injected = 0, attempted = 0;
    for (auto& m : mods) {
        if (!m.enabled) { logLine("[mods]  - skip (disabled): " + m.name); continue; }
        if (m.dllPath.empty() || !core::fileExists(m.dllPath)) {
            logLine("[inject] FAIL " + m.name + ": dll file missing");
            continue;
        }
        attempted++;
        std::string err;
        if (core::injectDll(pid, m.dllPath, err)) {
            logLine("[inject] OK   " + m.name + "  ->  " + core::narrow(m.dllPath));
            injected++;
        } else {
            logLine("[inject] FAIL " + m.name + ": " + err);
        }
    }
    logLine("[inject] finished: " + std::to_string(injected) + "/" + std::to_string(attempted) + " injected");
}

static void launchWorker(std::wstring addr) {
    g_busy = true;
    logLine("[launch] request -> " + core::narrow(addr));

    std::string msg = ensureBackend();
    logLine("[backend] " + msg);

    Paths p = refreshPaths();
    if (p.dreamseekerExe.empty() || !core::fileExists(p.dreamseekerExe)) {
        logLine("[launch] dreamseeker.exe not found: " + core::narrow(p.dreamseekerExe));
        g_busy = false;
        return;
    }

    std::wstring cwd = p.byondDir + L"\\bin";
    {
        std::wstring joined = core::setModPathsEnv(enabledModDlls());
        logLine("[launch] BYOND_MOD_PATHS = " + core::narrow(joined));
    }
    DWORD pid = 0;
    std::string err;
    if (!core::startProcess(p.dreamseekerExe, addr, cwd, pid, err)) {
        logLine("[launch] failed to start dreamseeker.exe: " + err);
        g_busy = false;
        return;
    }
    logLine("[launch] dreamseeker.exe started (pid " + std::to_string(pid) + ")");

    bool coreHit = false, winHit = false;
    for (int k = 0; k < 60; k++) {
        if (!core::processAlive(pid)) { logLine("[launch] dreamseeker.exe exited early"); g_busy = false; return; }
        coreHit = core::processHasModule(pid, L"byondcore.dll");
        winHit  = core::processHasModule(pid, L"byondwin.dll");
        if (coreHit && winHit) break;
        Sleep(500);
    }
    if (coreHit && winHit) logLine("[launch] byondcore.dll + byondwin.dll mapped; injecting mods");
    else logLine(std::string("[launch] warning: runtime not fully mapped (core=") +
                 (coreHit ? "yes" : "no") + " win=" + (winHit ? "yes" : "no") + "); injecting anyway");
    Sleep(600);

    injectInto(pid);
    g_busy = false;
}

static void hubServersWorker() {
    logLine("[hub] fetching SS13 servers from byond.com …");
    std::string err, js = hub::servers(err);
    {
        std::lock_guard<std::mutex> lk(g.mx);
        g.hubServersLoading = false;
        if (js.empty()) g.hubServersErr = err;
        else { g.hubServersJson = js; g.hubServersErr.clear(); }
    }
    if (js.empty()) logLine("[hub] SS13 servers failed: " + err);
    else            logLine("[hub] SS13 servers loaded (" + std::to_string(js.size()) + " bytes)");
}

static bool argFirst(const std::string& req, mj::Value& out) {
    if (req.empty()) return false;
    mj::Value v;
    if (!mj::parse(req, v)) return false;
    if (v.type == mj::Value::Arr) {
        if (v.arr.empty()) return false;
        out = v.arr[0];
        return true;
    }
    out = v;
    return true;
}

static std::string argStr(const std::string& req) {
    mj::Value v;
    if (!argFirst(req, v)) return "";
    return v.type == mj::Value::Str ? v.str : mj::dump(v);
}

static bool argObj(const std::string& req, mj::Value& out) {
    mj::Value v;
    if (!argFirst(req, v)) return false;
    if (v.type == mj::Value::Obj) { out = v; return true; }
    if (v.type == mj::Value::Str) {
        mj::Value p;
        if (mj::parse(v.str, p) && p.type == mj::Value::Obj) { out = p; return true; }
    }
    return false;
}

static std::string stateJson() {
    Paths p = refreshPaths();
    Procs pr = procs();
    bool startPager, effects;
    { std::lock_guard<std::mutex> lk(g.mx); startPager = g.cfg.startPager; effects = g.cfg.effects; }

    std::string out = "{";
    out += "\"byond_dir\":"       + mj::quote(core::narrow(p.byondDir)) + ",";
    out += "\"byond_exe\":"       + mj::quote(core::narrow(p.byondExe)) + ",";
    out += "\"dreamseeker_exe\":" + mj::quote(core::narrow(p.dreamseekerExe)) + ",";
    out += "\"mods_dir\":"        + mj::quote(core::narrow(p.modsDir)) + ",";
    out += "\"backend_running\":" + std::string(pr.byond ? "true" : "false") + ",";
    out += "\"backend_pid\":"     + std::to_string(pr.byond) + ",";
    out += "\"ds_running\":"      + std::string(pr.ds ? "true" : "false") + ",";
    out += "\"ds_pid\":"          + std::to_string(pr.ds) + ",";
    out += "\"start_pager\":"     + std::string(startPager ? "true" : "false") + ",";
    out += "\"effects\":"         + std::string(effects ? "true" : "false") + ",";
    out += "\"busy\":"            + std::string(g_busy.load() ? "true" : "false");
    out += "}";
    return out;
}

static std::string pollJson() {
    std::vector<std::string> log;
    std::string hubServers, hubServersErr;
    bool hubServersLoading;
    {
        std::lock_guard<std::mutex> lk(g.mx);
        log = g.log;
        hubServers = g.hubServersJson; hubServersErr = g.hubServersErr;
        hubServersLoading = g.hubServersLoading;
    }

    std::string out = "{";
    out += "\"log\":[";
    for (size_t i = 0; i < log.size(); i++) { if (i) out += ","; out += mj::quote(log[i]); }
    out += "],";
    out += "\"hub_servers\":"         + mj::quote(hubServers) + ",";
    out += "\"hub_servers_loading\":" + std::string(hubServersLoading ? "true" : "false") + ",";
    out += "\"hub_servers_err\":"     + mj::quote(hubServersErr) + ",";

    std::string st = stateJson();
    out += st.substr(1, st.size() - 2);
    out += "}";
    return out;
}

// Borderless chrome. The window keeps the native frame bits so Windows/DWM still
// treat it as a regular top-level window (smooth minimize/restore, taskbar
// minimize, drop shadow); the visible frame itself is removed here.
static WNDPROC g_prevWndProc = nullptr;

static LRESULT CALLBACK frameWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    // Client area covers the whole window -> none of the native frame shows.
    case WM_NCCALCSIZE:
        if (wp) return 0;
        break;
    // Keep the invisible resize edges out of hit-testing: the window is fixed.
    case WM_NCHITTEST: {
        LRESULT hit = CallWindowProcW(g_prevWndProc, h, msg, wp, lp);
        if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT) return HTCLIENT;
        return hit;
    }
    // Paint the app background instead of erasing to the default white.
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(10, 10, 15));
        FillRect(reinterpret_cast<HDC>(wp), &rc, br);
        DeleteObject(br);
        return 1;
    }
    }
    return CallWindowProcW(g_prevWndProc, h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    if (HMODULE u32 = GetModuleHandleW(L"user32.dll")) {
        typedef BOOL (WINAPI *SetCtxFn)(HANDLE);
        if (SetCtxFn setCtx = (SetCtxFn)GetProcAddress(u32, "SetProcessDpiAwarenessContext"))
            setCtx((HANDLE)-4);
    }

    g.cfg = core::loadConfig();
    { std::ofstream f(fs::path(core::exeDir() + L"\\detomatix.log"), std::ios::trunc | std::ios::binary); }
    refreshPaths();

    
    webview::webview w(false, nullptr);

    w.set_title("Detomatix");
    w.set_size(1280, 820, WEBVIEW_HINT_FIXED);

    HWND hwnd = static_cast<HWND>(w.window().value());

    g_prevWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(frameWndProc)));

    HICON hSmall = (HICON)LoadImage(hInst, MAKEINTRESOURCE(101), IMAGE_ICON, 16, 16, 0);
    HICON hBig   = (HICON)LoadImage(hInst, MAKEINTRESOURCE(101), IMAGE_ICON, 32, 32, 0);

    SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hSmall);
    SendMessage(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hBig);

    LONG_PTR style = GetWindowLongPtr(hwnd, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_POPUP | WS_MAXIMIZEBOX);
    style |= WS_THICKFRAME | WS_MINIMIZEBOX | WS_SYSMENU | WS_VISIBLE;
    SetWindowLongPtr(hwnd, GWL_STYLE, style);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    // WebView2's default surface is white; match it to the UI so restoring the
    // window never flashes a white frame before the first composited frame.
    auto ctrl = w.browser_controller();
    if (ctrl.ok()) {
        auto* base = static_cast<ICoreWebView2Controller*>(ctrl.value());
        ICoreWebView2Controller2* c2 = nullptr;
        if (base && SUCCEEDED(base->QueryInterface(
                        IID_ICoreWebView2Controller2,
                        reinterpret_cast<void**>(&c2))) && c2) {
            COREWEBVIEW2_COLOR bg{};
            bg.A = 255; bg.R = 10; bg.G = 10; bg.B = 15;
            c2->put_DefaultBackgroundColor(bg);
            c2->Release();
        }
    }

    w.bind("cpp_drag", [hwnd](const std::string&) -> std::string {
        ReleaseCapture();
        SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return "null";
    });
    w.bind("cpp_minimize", [hwnd](const std::string&) -> std::string {
        ShowWindow(hwnd, SW_MINIMIZE);
        return "null";
    });
    w.bind("cpp_close", [&w](const std::string&) -> std::string {
        w.terminate();
        return "null";
    });

    w.bind("cpp_init", [](const std::string&) -> std::string { return stateJson(); });

    w.bind("cpp_listMods", [](const std::string&) -> std::string {
        auto mods = loadMods();
        DWORD pid = dsPid();
        std::set<std::wstring> loaded;
        if (pid) loaded = core::loadedModuleNames(pid);
        std::string out = "[";
        for (size_t i = 0; i < mods.size(); i++) {
            const auto& m = mods[i];
            bool active = false;
            if (pid && !m.dllPath.empty())
                active = loaded.count(lowerW(fs::path(m.dllPath).filename().wstring())) != 0;
            if (i) out += ",";
            out += "{";
            out += "\"id\":"          + mj::quote(m.id);
            out += ",\"name\":"       + mj::quote(m.name);
            out += ",\"version\":"    + mj::quote(m.version);
            out += ",\"author\":"     + mj::quote(m.author);
            out += ",\"description\":"+ mj::quote(m.description);
            out += ",\"dll\":"        + mj::quote(core::narrow(m.dllPath));
            out += ",\"enabled\":"    + std::string(m.enabled ? "true" : "false");
            out += ",\"active\":"     + std::string(active ? "true" : "false");
            out += "}";
        }
        out += "]";
        return out;
    });

    w.bind("cpp_setMod", [](const std::string& req) -> std::string {
        mj::Value v;
        if (!argObj(req, v)) return "{\"ok\":false}";
        std::string id = v.getStr("id", "");
        bool on = v.getBool("on", true);
        if (id.empty()) return "{\"ok\":false}";
        { std::lock_guard<std::mutex> lk(g.mx); g.cfg.modEnabled[id] = on; core::saveConfig(g.cfg); }
        logLine(std::string("[mods] ") + (on ? "enabled " : "disabled ") + id);
        return "{\"ok\":true}";
    });

    w.bind("cpp_setByondDir", [](const std::string& dir) -> std::string {
        std::wstring d = core::widen(argStr(dir));
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        { std::lock_guard<std::mutex> lk(g.mx); g.cfg.byondDir = d; core::saveConfig(g.cfg); }
        logLine("[config] BYOND dir = " + core::narrow(d));
        return stateJson();
    });

    w.bind("cpp_setOption", [](const std::string& req) -> std::string {
        mj::Value v;
        if (!argObj(req, v)) return "{\"ok\":false}";
        std::string key = v.getStr("key", "");
        bool on = v.getBool("on", false);
        {
            std::lock_guard<std::mutex> lk(g.mx);
            if (key == "start_pager")      g.cfg.startPager = on;
            else if (key == "effects")     g.cfg.effects = on;
            else return "{\"ok\":false}";
            core::saveConfig(g.cfg);
        }
        logLine("[config] " + key + " = " + (on ? "on" : "off"));
        return stateJson();
    });

    w.bind("cpp_ensureBackend", [](const std::string&) -> std::string {
        std::string msg = ensureBackend();
        logLine("[backend] " + msg);
        return "{\"ok\":true,\"msg\":" + mj::quote(msg) + "}";
    });

    w.bind("cpp_openMods", [](const std::string&) -> std::string {
        Paths p = refreshPaths();
        core::openInExplorer(p.modsDir);
        return "{\"ok\":true}";
    });

    w.bind("cpp_openByond", [](const std::string&) -> std::string {
        Paths p = refreshPaths();
        if (!p.byondDir.empty()) core::openInExplorer(p.byondDir);
        return "{\"ok\":true}";
    });

    w.bind("cpp_launch", [](const std::string& addr) -> std::string {
        std::wstring wa = core::widen(argStr(addr));
        std::thread(launchWorker, wa).detach();
        return "{\"ok\":true}";
    });

    w.bind("cpp_hubServers", [](const std::string&) -> std::string {
        {
            std::lock_guard<std::mutex> lk(g.mx);
            if (g.hubServersLoading) return "{\"ok\":true}";
            g.hubServersLoading = true;
            g.hubServersJson.clear();
            g.hubServersErr.clear();
        }
        std::thread(hubServersWorker).detach();
        return "{\"ok\":true}";
    });

    w.bind("cpp_poll", [](const std::string&) -> std::string { return pollJson(); });

    w.bind("cpp_clearLog", [](const std::string&) -> std::string {
        std::lock_guard<std::mutex> lk(g.mx);
        g.log.clear();
        return "{\"ok\":true}";
    });

    {
        const char* ao = std::getenv("DETOMATIX_AUTOOPEN");
        if (ao && *ao) w.init(std::string("window.__AUTOOPEN__=") + mj::quote(ao) + ";");

        std::wstring htmlPath = core::exeDir() + L"\\assets\\index.html";
        if (!core::fileExists(htmlPath)) {
            MessageBoxW(0, (L"assets\\index.html not found at:\n" + htmlPath).c_str(), L"Detomatix", MB_ICONERROR);
            return 1;
        }
        std::wstring url = htmlPath;
        for (auto& c : url) if (c == L'\\') c = L'/';
        w.navigate("file:///" + core::narrow(url));
    }

    bool startPager;
    { std::lock_guard<std::mutex> lk(g.mx); startPager = g.cfg.startPager; }
    if (startPager) {
        std::thread([] {
            Sleep(300);
            logLine("[startup] " + ensureBackend());
        }).detach();
    }

    w.run();
    return 0;
}
