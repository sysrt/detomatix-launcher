#include "core.hpp"
#include "json.hpp"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <winhttp.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <cwctype>
#include <cstdint>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace fs = std::filesystem;

namespace core {

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

std::string narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

static std::wstring lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring exePath() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return buf;
}

std::wstring exeDir() {
    std::wstring p = exePath();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring moduleDir() { return exeDir(); }

bool fileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string readFile(const std::wstring& p) {
    std::ifstream f(fs::path(p), std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::wstring& p, const std::string& data) {
    std::ofstream f(fs::path(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    return (bool)f;
}

void openInExplorer(const std::wstring& path) {
    if (path.empty()) return;
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

static bool crackUrl(const std::wstring& url, bool& https, std::wstring& host,
                     INTERNET_PORT& port, std::wstring& path) {
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t hostBuf[256] = {0};
    wchar_t pathBuf[4096] = {0};
    uc.lpszHostName = hostBuf;
    uc.dwHostNameLength = (DWORD)(sizeof(hostBuf) / sizeof(wchar_t) - 1);
    uc.lpszUrlPath = pathBuf;
    uc.dwUrlPathLength = (DWORD)(sizeof(pathBuf) / sizeof(wchar_t) - 1);
    if (!WinHttpCrackUrl(url.c_str(), (DWORD)url.size(), 0, &uc)) return false;
    https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    host = hostBuf;
    path = pathBuf;
    if (path.empty()) path = L"/";
    port = uc.nPort;
    return !host.empty();
}

bool httpGet(const std::string& url, std::string& out, std::string& err) {
    std::wstring wurl = widen(url);
    bool https = false;
    std::wstring host, path;
    INTERNET_PORT port = 0;
    if (!crackUrl(wurl, https, host, port, path)) { err = "invalid url"; return false; }

    HINTERNET hS = WinHttpOpen(L"Detomatix/1.0",
                               WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                               WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hS) { err = "WinHttpOpen failed (" + std::to_string(GetLastError()) + ")"; return false; }
    WinHttpSetTimeouts(hS, 15000, 15000, 25000, 25000);

    HINTERNET hC = WinHttpConnect(hS, host.c_str(), port, 0);
    if (!hC) { err = "connect failed (" + std::to_string(GetLastError()) + ")"; WinHttpCloseHandle(hS); return false; }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hR = WinHttpOpenRequest(hC, L"GET", path.c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hR) { err = "OpenRequest failed"; WinHttpCloseHandle(hC); WinHttpCloseHandle(hS); return false; }

    const wchar_t* hdrs = L"Accept-Encoding: identity\r\n";
    BOOL ok = WinHttpSendRequest(hR, hdrs, -1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hR, nullptr);
    if (!ok) {
        err = "request failed (" + std::to_string(GetLastError()) + ")";
        WinHttpCloseHandle(hR); WinHttpCloseHandle(hC); WinHttpCloseHandle(hS);
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(hR, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    out.clear();
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hR, &avail)) break;
        if (avail == 0) break;
        size_t off = out.size();
        out.resize(off + avail);
        DWORD read = 0;
        if (!WinHttpReadData(hR, &out[off], avail, &read)) { out.resize(off); break; }
        out.resize(off + read);
        if (read == 0) break;
    }

    WinHttpCloseHandle(hR);
    WinHttpCloseHandle(hC);
    WinHttpCloseHandle(hS);

    if (status != 200) { err = "HTTP " + std::to_string(status); out.clear(); return false; }
    return true;
}

std::vector<DWORD> findPids(const std::wstring& exeNameLower) {
    std::vector<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (lower(pe.szExeFile) == exeNameLower) out.push_back(pe.th32ProcessID);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

DWORD findPid(const std::wstring& exeNameLower) {
    auto v = findPids(exeNameLower);
    return v.empty() ? 0 : v[0];
}

void findPids(const std::vector<std::wstring>& exeNamesLower,
              std::map<std::wstring, DWORD>& out) {
    out.clear();
    if (exeNamesLower.empty()) return;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    ZeroMemory(&pe, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring name = lower(pe.szExeFile);
            for (const auto& want : exeNamesLower)
                if (name == want && !out.count(want)) out[want] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    for (const auto& want : exeNamesLower) out.emplace(want, 0);
}

bool processAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

static bool rpm(HANDLE hp, uint32_t addr, void* buf, size_t sz) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(hp, (LPCVOID)(uintptr_t)addr, buf, sz, &got)) return false;
    return got == sz;
}

static uint32_t remoteModuleBase(HANDLE hp, const std::wstring& nameLower) {
    HMODULE mods[1024];
    DWORD needed = 0;
    if (!EnumProcessModulesEx(hp, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) return 0;
    DWORD count = needed / sizeof(HMODULE);
    if (count > 1024) count = 1024;
    const std::wstring want = lower(nameLower);
    for (DWORD k = 0; k < count; k++) {
        wchar_t base[MAX_PATH] = {0};
        if (!GetModuleBaseNameW(hp, mods[k], base, MAX_PATH)) continue;
        if (lower(base) == want) return (uint32_t)(uintptr_t)mods[k];
    }
    return 0;
}

static uint32_t remoteExport(HANDLE hp, uint32_t modBase, const char* funcName, int depth) {
    if (!modBase || depth > 4) return 0;

    IMAGE_DOS_HEADER dos;
    if (!rpm(hp, modBase, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    IMAGE_NT_HEADERS32 nt;
    if (!rpm(hp, modBase + (uint32_t)dos.e_lfanew, &nt, sizeof(nt))) return 0;
    if (nt.Signature != IMAGE_NT_SIGNATURE) return 0;

    const IMAGE_DATA_DIRECTORY& dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    DWORD expRva = dir.VirtualAddress, expSize = dir.Size;
    if (!expRva) return 0;

    IMAGE_EXPORT_DIRECTORY exp;
    if (!rpm(hp, modBase + expRva, &exp, sizeof(exp))) return 0;
    if (!exp.NumberOfNames) return 0;

    std::vector<DWORD> names(exp.NumberOfNames);
    if (!rpm(hp, modBase + exp.AddressOfNames, names.data(), names.size() * sizeof(DWORD))) return 0;
    std::vector<WORD> ordinals(exp.NumberOfNames);
    if (!rpm(hp, modBase + exp.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD))) return 0;

    for (DWORD k = 0; k < exp.NumberOfNames; k++) {
        char nameBuf[256] = {0};
        if (!rpm(hp, modBase + names[k], nameBuf, sizeof(nameBuf) - 1)) continue;
        if (std::strcmp(nameBuf, funcName) != 0) continue;

        DWORD funcRva = 0;
        if (!rpm(hp, modBase + exp.AddressOfFunctions + ordinals[k] * sizeof(DWORD), &funcRva, sizeof(DWORD))) return 0;

        if (funcRva >= expRva && funcRva < expRva + expSize) {
            char fwd[256] = {0};
            if (!rpm(hp, modBase + funcRva, fwd, sizeof(fwd) - 1)) return 0;
            std::string f(fwd);
            size_t dot = f.find('.');
            if (dot == std::string::npos) return 0;
            std::wstring fwdDll = widen(f.substr(0, dot) + ".dll");
            std::string  fwdFn  = f.substr(dot + 1);
            uint32_t fbase = remoteModuleBase(hp, fwdDll);
            return remoteExport(hp, fbase, fwdFn.c_str(), depth + 1);
        }
        return modBase + funcRva;
    }
    return 0;
}

std::set<std::wstring> loadedModuleNames(DWORD pid) {
    std::set<std::wstring> out;
    HANDLE hp = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) return out;
    HMODULE mods[1024];
    DWORD needed = 0;
    if (EnumProcessModulesEx(hp, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
        DWORD count = needed / sizeof(HMODULE);
        if (count > 1024) count = 1024;
        for (DWORD k = 0; k < count; k++) {
            wchar_t base[MAX_PATH] = {0};
            if (GetModuleBaseNameW(hp, mods[k], base, MAX_PATH)) out.insert(lower(base));
        }
    }
    CloseHandle(hp);
    return out;
}

bool processHasModule(DWORD pid, const std::wstring& moduleLower) {
    auto names = loadedModuleNames(pid);
    return names.count(lower(moduleLower)) != 0;
}

bool startProcess(const std::wstring& exe,
                  const std::wstring& args,
                  const std::wstring& cwd,
                  DWORD& outPid, std::string& err) {
    std::wstring cmdline = L"\"" + exe + L"\"";
    if (!args.empty()) cmdline += L" " + args;

    std::vector<wchar_t> buf(cmdline.begin(), cmdline.end());
    buf.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof(pi));

    const wchar_t* c = cwd.empty() ? nullptr : cwd.c_str();
    if (!CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, FALSE,
                        0, nullptr, c, &si, &pi)) {
        err = "CreateProcessW failed (err " + std::to_string(GetLastError()) + ")";
        return false;
    }
    outPid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool injectDll(DWORD pid, const std::wstring& dllPath, std::string& err) {
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, pid);
    if (!hp) { err = "OpenProcess failed (err " + std::to_string(GetLastError()) + ")"; return false; }

    uint32_t k32 = remoteModuleBase(hp, L"kernel32.dll");
    if (!k32) k32 = remoteModuleBase(hp, L"kernelbase.dll");
    uint32_t loadLib = remoteExport(hp, k32, "LoadLibraryW", 0);
    if (!loadLib) {
        CloseHandle(hp);
        err = "could not resolve LoadLibraryW in target (arch mismatch?)";
        return false;
    }

    SIZE_T bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(hp, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) {
        CloseHandle(hp);
        err = "VirtualAllocEx failed (err " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!WriteProcessMemory(hp, remote, dllPath.c_str(), bytes, nullptr)) {
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        CloseHandle(hp);
        err = "WriteProcessMemory failed (err " + std::to_string(GetLastError()) + ")";
        return false;
    }

    HANDLE th = CreateRemoteThread(hp, nullptr, 0,
                                   (LPTHREAD_START_ROUTINE)(uintptr_t)loadLib,
                                   remote, 0, nullptr);
    if (!th) {
        VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
        CloseHandle(hp);
        err = "CreateRemoteThread failed (err " + std::to_string(GetLastError()) + ")";
        return false;
    }

    WaitForSingleObject(th, 15000);
    DWORD code = 0;
    GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(hp, remote, 0, MEM_RELEASE);
    CloseHandle(hp);

    if (code == 0) { err = "target LoadLibraryW returned NULL (load failed)"; return false; }
    return true;
}

std::wstring setModPathsEnv(const std::vector<std::wstring>& dllPaths) {
    std::wstring joined;
    for (const auto& p : dllPaths) {
        if (p.empty()) continue;
        if (!joined.empty()) joined += L";";
        joined += p;
    }

    SetEnvironmentVariableW(L"BYOND_MOD_PATHS", joined.empty() ? nullptr : joined.c_str());

    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"SOFTWARE\\Detomatix", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &hk, nullptr) == ERROR_SUCCESS) {
        if (joined.empty())
            RegDeleteValueW(hk, L"BYOND_MOD_PATHS");
        else
            RegSetValueExW(hk, L"BYOND_MOD_PATHS", 0, REG_SZ,
                           (const BYTE*)joined.c_str(),
                           (DWORD)((joined.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(hk);
    }
    return joined;
}

std::wstring detectByondDir() {
    const wchar_t* keys[] = {
        L"SOFTWARE\\WOW6432Node\\Dantom\\BYOND",
        L"SOFTWARE\\Dantom\\BYOND"
    };
    for (auto k : keys) {
        HKEY hk = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
            wchar_t buf[512] = {0};
            DWORD sz = sizeof(buf), type = 0;
            if (RegQueryValueExW(hk, L"installpath", nullptr, &type, (LPBYTE)buf, &sz) == ERROR_SUCCESS && type == REG_SZ) {
                RegCloseKey(hk);
                std::wstring p = buf;
                while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
                if (!p.empty()) return p;
            }
            RegCloseKey(hk);
        }
    }
    const wchar_t* cands[] = { L"C:\\Program Files (x86)\\BYOND", L"C:\\Program Files\\BYOND" };
    for (auto c : cands) {
        if (fileExists(std::wstring(c) + L"\\bin\\byond.exe")) return c;
    }
    return L"";
}

std::wstring byondExe(const std::wstring& byondDir)      { return byondDir + L"\\bin\\byond.exe"; }
std::wstring dreamseekerExe(const std::wstring& byondDir){ return byondDir + L"\\bin\\dreamseeker.exe"; }

std::wstring modsDir() {
    std::error_code ec;
    std::wstring local = exeDir() + L"\\mods";
    if (fs::is_directory(local, ec)) return local;

    std::wstring parent = exeDir() + L"\\..\\mods";
    if (fs::is_directory(parent, ec)) {
        std::wstring canon = fs::weakly_canonical(parent, ec).wstring();
        return ec ? parent : canon;
    }

    fs::create_directories(local, ec);
    std::wstring readme = local + L"\\README.txt";
    if (!fileExists(readme)) {
        writeFile(readme,
            "Each mod is a folder containing one .dll and a mod.json, e.g.\r\n\r\n"
            "  mods\\MyMod\\MyMod.dll\r\n"
            "  mods\\MyMod\\mod.json\r\n\r\n"
            "mod.json:\r\n"
            "{\r\n"
            "  \"name\": \"My Mod\",\r\n"
            "  \"version\": \"1.0.0\",\r\n"
            "  \"author\": \"you\",\r\n"
            "  \"description\": \"What it does.\",\r\n"
            "  \"dll\": \"MyMod.dll\"\r\n"
            "}\r\n\r\n"
            "Loose files also work:  MyMod.dll  (+ optional  MyMod.json).\r\n");
    }
    return local;
}

static std::wstring extLower(const fs::path& p) {
    std::wstring e = p.extension().wstring();
    return lower(e);
}

static Mod parseModJson(const std::wstring& jsonPath, const std::wstring& fallbackName) {
    Mod m;
    std::string data = readFile(jsonPath);
    mj::Value v;
    if (!data.empty() && mj::parse(data, v) && v.type == mj::Value::Obj) {
        m.name        = v.getStr("name", "");
        m.version     = v.getStr("version", "");
        m.author      = v.getStr("author", "");
        m.description = v.getStr("description", "");
        std::string dllField = v.getStr("dll", "");
        if (!dllField.empty()) {
            fs::path jp(jsonPath);
            m.dllPath = (jp.parent_path() / widen(dllField)).wstring();
        }
    }
    if (m.name.empty()) m.name = narrow(fallbackName);
    return m;
}

std::vector<Mod> scanMods(const std::wstring& dir) {
    std::vector<Mod> out;
    std::error_code ec;
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) return out;

    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& de = *it;
        if (!de.is_directory(ec)) continue;

        std::wstring jsonPath, dllPath;
        std::error_code ec2;
        for (auto fit = fs::directory_iterator(de.path(), ec2); !ec2 && fit != fs::directory_iterator(); fit.increment(ec2)) {
            const fs::directory_entry& fe = *fit;
            if (!fe.is_regular_file(ec2)) continue;
            std::wstring e = extLower(fe.path());
            if (e == L".json") {
                if (jsonPath.empty() || lower(fe.path().filename().wstring()) == L"mod.json")
                    jsonPath = fe.path().wstring();
            } else if (e == L".dll") {
                if (dllPath.empty()) dllPath = fe.path().wstring();
            }
        }
        if (dllPath.empty()) continue;

        std::wstring folder = de.path().filename().wstring();
        Mod m = jsonPath.empty() ? Mod() : parseModJson(jsonPath, folder);
        if (jsonPath.empty()) m.name = narrow(folder);
        m.id = narrow(folder);
        if (m.dllPath.empty()) m.dllPath = dllPath;
        out.push_back(m);
    }

    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::directory_entry& de = *it;
        if (!de.is_regular_file(ec)) continue;
        if (extLower(de.path()) != L".dll") continue;

        std::wstring base = de.path().stem().wstring();
        fs::path jsonCand = de.path().parent_path() / (base + L".json");
        std::error_code ec3;
        Mod m = fs::exists(jsonCand, ec3) ? parseModJson(jsonCand.wstring(), base) : Mod();
        if (m.name.empty()) m.name = narrow(base);
        m.id = narrow(base);
        m.dllPath = de.path().wstring();
        out.push_back(m);
    }

    return out;
}

static const wchar_t* kConfigPath() {
    static std::wstring p;
    if (p.empty()) p = exeDir() + L"\\config.json";
    return p.c_str();
}

Config loadConfig() {
    Config cfg;
    std::string data = readFile(kConfigPath());
    mj::Value v;
    if (!data.empty() && mj::parse(data, v) && v.type == mj::Value::Obj) {
        cfg.byondDir   = widen(v.getStr("byond_dir", ""));
        cfg.startPager = v.getBool("start_pager", true);
        cfg.effects    = v.getBool("effects", true);
        const mj::Value* mods = v.find("mods");
        if (mods && mods->type == mj::Value::Obj) {
            for (auto& kv : mods->obj)
                if (kv.second.type == mj::Value::Bool) cfg.modEnabled[kv.first] = kv.second.b;
        }
    }
    if (cfg.byondDir.empty()) cfg.byondDir = detectByondDir();
    return cfg;
}

void saveConfig(const Config& cfg) {
    std::string out = "{\n";
    out += "  \"byond_dir\": " + mj::quote(narrow(cfg.byondDir)) + ",\n";
    out += "  \"start_pager\": " + std::string(cfg.startPager ? "true" : "false") + ",\n";
    out += "  \"effects\": " + std::string(cfg.effects ? "true" : "false") + ",\n";
    out += "  \"mods\": {\n";
    size_t i = 0;
    for (auto& kv : cfg.modEnabled) {
        out += "    " + mj::quote(kv.first) + ": " + std::string(kv.second ? "true" : "false");
        if (++i < cfg.modEnabled.size()) out += ",";
        out += "\n";
    }
    out += "  }\n}\n";
    writeFile(kConfigPath(), out);
}

}
