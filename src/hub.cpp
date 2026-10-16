#include "hub.hpp"
#include "core.hpp"
#include "json.hpp"

#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace hub {

static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') a++;
    while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
    return s.substr(a, b - a);
}

static bool ieqAt(const std::string& s, size_t i, const char* lit) {
    size_t n = std::strlen(lit);
    if (i + n > s.size()) return false;
    for (size_t k = 0; k < n; k++) {
        char a = s[i + k], b = lit[k];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

static void putUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out.push_back((char)cp);
    else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static std::string decodeEntities(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '&') {
            size_t semi = s.find(';', i);
            if (semi != std::string::npos && semi - i <= 8) {
                std::string e = s.substr(i + 1, semi - i - 1);
                if (e == "amp")  { out.push_back('&'); i = semi + 1; continue; }
                if (e == "lt")   { out.push_back('<'); i = semi + 1; continue; }
                if (e == "gt")   { out.push_back('>'); i = semi + 1; continue; }
                if (e == "quot") { out.push_back('"'); i = semi + 1; continue; }
                if (e == "apos" || e == "#39") { out.push_back('\''); i = semi + 1; continue; }
                if (e == "nbsp") { out.push_back(' '); i = semi + 1; continue; }
                if (!e.empty() && e[0] == '#') {
                    unsigned cp = 0;
                    bool ok = false;
                    if (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
                        cp = (unsigned)std::strtoul(e.c_str() + 2, nullptr, 16), ok = e.size() > 2;
                    else
                        cp = (unsigned)std::strtoul(e.c_str() + 1, nullptr, 10), ok = e.size() > 1;
                    if (ok && cp > 0 && cp <= 0x10FFFF) { putUtf8(out, cp); i = semi + 1; continue; }
                }
            }
        }
        out.push_back(s[i++]);
    }
    return out;
}

static std::string htmlToText(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    for (size_t i = 0; i < html.size();) {
        if (html[i] == '<') {
            if (ieqAt(html, i, "<br>"))      { out.push_back('\n'); i += 4; continue; }
            if (ieqAt(html, i, "<br/>"))     { out.push_back('\n'); i += 5; continue; }
            if (ieqAt(html, i, "<br />"))    { out.push_back('\n'); i += 6; continue; }
            if (ieqAt(html, i, "<p>"))       { out.push_back('\n'); i += 3; continue; }
            if (ieqAt(html, i, "</p>"))      { out.push_back('\n'); i += 4; continue; }
            size_t close = html.find('>', i);
            if (close == std::string::npos) break;
            i = close + 1;
            continue;
        }
        out.push_back(html[i++]);
    }
    return decodeEntities(out);
}

static std::string collapse(const std::string& s, char sep = ' ') {
    std::string out;
    out.reserve(s.size());
    bool ws = false;
    for (char c : s) {
        if (c == '\n') {
            if (!out.empty() && out.back() != sep) out.push_back(sep);
            ws = true;
            continue;
        }
        if ((unsigned char)c <= ' ') { ws = true; continue; }
        if (ws && !out.empty() && out.back() != sep) out.push_back(' ');
        ws = false;
        out.push_back(c);
    }
    return trim(out);
}

static std::string firstLine(const std::string& s) {
    size_t n = s.find('\n');
    return collapse(n == std::string::npos ? s : s.substr(0, n));
}

static std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n - 1) + "\xE2\x80\xA6";
}

static std::string unescapeC(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[++i];
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(c); break;
            }
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static std::string unquote(const std::string& v) {
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
        return unescapeC(v.substr(1, v.size() - 2));
    return v;
}

static int countListItems(const std::string& v) {
    size_t a = v.find('(');
    size_t b = v.rfind(')');
    if (a == std::string::npos || b == std::string::npos || b < a) return 0;
    int count = 0;
    bool in = false, esc = false;
    for (size_t i = a + 1; i < b; i++) {
        char c = v[i];
        if (esc) { esc = false; continue; }
        if (c == '\\') { esc = true; continue; }
        if (c == '"') { if (in) in = false; else { in = true; count++; } }
    }
    return count;
}

static int reportedPlayers(const std::string& text) {
    size_t p = text.find("Players:");
    if (p == std::string::npos) return -1;
    size_t i = p + 8;
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) i++;
    int n = 0;
    bool any = false;
    while (i < text.size() && isdigit((unsigned char)text[i])) {
        n = n * 10 + (text[i] - '0');
        i++;
        any = true;
    }
    return any ? n : -1;
}

struct World {
    std::string url, host, status, title, desc;
    int version = 0, players = 0;
};

std::string servers(std::string& err) {
    std::string url = "https://www.byond.com/games/Exadv1/SpaceStation13?format=text";
    std::string text, e;
    if (!core::httpGet(url, text, e)) {
        err = e.empty() ? "request failed" : e;
        return "";
    }

    std::vector<World> ws;
    ws.reserve(128);
    World cur;
    bool has = false;

    size_t i = 0, n = text.size();
    while (i < n) {
        size_t eol = text.find('\n', i);
        std::string line = text.substr(i, (eol == std::string::npos ? n : eol) - i);
        i = (eol == std::string::npos ? n : eol + 1);
        while (!line.empty() && line.back() == '\r') line.pop_back();

        std::string t = trim(line);
        if (t.rfind("world/", 0) == 0) {
            if (has) ws.push_back(cur);
            cur = World();
            has = true;
            continue;
        }
        if (!has) continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trim(t.substr(0, eq));
        std::string val = trim(t.substr(eq + 1));
        if (val.empty()) continue;

        if (key == "url")                 cur.url = unquote(val);
        else if (key == "host")           cur.host = unquote(val);
        else if (key == "status")         cur.status = unquote(val);
        else if (key == "server_version") cur.version = std::atoi(val.c_str());
        else if (key == "players")        cur.players = countListItems(val);
    }
    if (has) ws.push_back(cur);

    for (auto& w : ws) {
        std::string text2 = htmlToText(w.status);
        w.title = firstLine(text2);
        if (w.title.empty()) w.title = "(unnamed server)";
        w.desc = truncate(collapse(text2, '|'), 220);
        int rep = reportedPlayers(text2);
        if (rep >= 0) w.players = rep;
    }
    std::stable_sort(ws.begin(), ws.end(),
                     [](const World& a, const World& b) { return a.players > b.players; });

    std::string out;
    out.reserve(text.size());
    out.push_back('[');
    for (size_t k = 0; k < ws.size(); k++) {
        const World& w = ws[k];
        if (k) out += ",";
        out += "{";
        out += "\"url\":"     + mj::quote(w.url);
        out += ",\"players\":" + std::to_string(w.players);
        out += ",\"version\":" + std::to_string(w.version);
        out += ",\"host\":"    + mj::quote(w.host);
        out += ",\"title\":"   + mj::quote(w.title);
        out += ",\"desc\":"    + mj::quote(w.desc);
        out += "}";
    }
    out += "]";
    err.clear();
    return out;
}

}
