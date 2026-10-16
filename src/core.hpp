#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>
#include <windows.h>

namespace core {

std::wstring widen(const std::string& s);
std::string  narrow(const std::wstring& s);

std::wstring exePath();
std::wstring exeDir();
std::wstring moduleDir();

bool        fileExists(const std::wstring& p);
std::string readFile(const std::wstring& p);
bool        writeFile(const std::wstring& p, const std::string& data);

bool httpGet(const std::string& url, std::string& out, std::string& err);

void openInExplorer(const std::wstring& path);

std::vector<DWORD> findPids(const std::wstring& exeNameLower);
void               findPids(const std::vector<std::wstring>& exeNamesLower,
                             std::map<std::wstring, DWORD>& out);
DWORD              findPid(const std::wstring& exeNameLower);
bool               processAlive(DWORD pid);
bool               processHasModule(DWORD pid, const std::wstring& moduleLower);
std::set<std::wstring> loadedModuleNames(DWORD pid);
bool               startProcess(const std::wstring& exe,
                                const std::wstring& args,
                                const std::wstring& cwd,
                                DWORD& outPid, std::string& err);

bool injectDll(DWORD pid, const std::wstring& dllPath, std::string& err);

std::wstring setModPathsEnv(const std::vector<std::wstring>& dllPaths);

std::wstring detectByondDir();
std::wstring byondExe(const std::wstring& byondDir);
std::wstring dreamseekerExe(const std::wstring& byondDir);

struct Mod {
    std::string  id;
    std::string  name;
    std::string  version;
    std::string  author;
    std::string  description;
    std::wstring dllPath;
    bool         enabled = true;
};

std::wstring modsDir();
std::vector<Mod> scanMods(const std::wstring& dir);

struct Config {
    std::wstring              byondDir;
    std::map<std::string,bool> modEnabled;
    bool                       startPager = true;
    bool                       effects    = true;
};

Config loadConfig();
void   saveConfig(const Config& cfg);

}
