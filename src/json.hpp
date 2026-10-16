#pragma once

#include <string>
#include <vector>
#include <utility>
#include <cstdio>
#include <cstdlib>

namespace mj {

struct Value {
    enum Type { Null, Bool, Num, Str, Arr, Obj };
    Type type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    Value() {}
    explicit Value(bool v) : type(Bool), b(v) {}
    explicit Value(double v) : type(Num), num(v) {}
    explicit Value(int v) : type(Num), num((double)v) {}
    explicit Value(const std::string& v) : type(Str), str(v) {}
    explicit Value(const char* v) : type(Str), str(v) {}

    static Value makeArr() { Value v; v.type = Arr; return v; }
    static Value makeObj() { Value v; v.type = Obj; return v; }

    const Value* find(const std::string& k) const {
        if (type != Obj) return nullptr;
        for (auto& kv : obj) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string getStr(const std::string& k, const std::string& def = "") const {
        const Value* v = find(k); return (v && v->type == Str) ? v->str : def;
    }
    bool getBool(const std::string& k, bool def = false) const {
        const Value* v = find(k); return (v && v->type == Bool) ? v->b : def;
    }
    double getNum(const std::string& k, double def = 0) const {
        const Value* v = find(k); return (v && v->type == Num) ? v->num : def;
    }
};

namespace detail {

struct Parser {
    const std::string& s;
    size_t i = 0;
    bool ok = true;
    explicit Parser(const std::string& src) : s(src) {}

    void ws() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++; }
    bool eof() { return i >= s.size(); }
    char peek() { return i < s.size() ? s[i] : '\0'; }
    void fail() { ok = false; }

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

    bool readHex4(unsigned& out) {
        out = 0;
        for (int k = 0; k < 4; k++) {
            if (i >= s.size()) return false;
            char h = s[i++];
            out <<= 4;
            if (h >= '0' && h <= '9') out |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') out |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') out |= (unsigned)(h - 'A' + 10);
            else return false;
        }
        return true;
    }

    std::string parseString() {
        std::string out;
        if (peek() != '"') { fail(); return out; }
        i++;
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return out;
            if (c == '\\') {
                if (i >= s.size()) { fail(); return out; }
                char e = s[i++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!readHex4(cp)) { fail(); return out; }
                        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i] == '\\' && s[i+1] == 'u') {
                            i += 2;
                            unsigned low = 0;
                            if (!readHex4(low)) { fail(); return out; }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        }
                        putUtf8(out, cp);
                        break;
                    }
                    default: fail(); return out;
                }
            } else {
                out.push_back(c);
            }
        }
        fail();
        return out;
    }

    Value parseValue() {
        ws();
        Value v;
        if (eof()) { fail(); return v; }
        char c = peek();
        if (c == '{') {
            v.type = Value::Obj;
            i++;
            ws();
            if (peek() == '}') { i++; return v; }
            while (true) {
                ws();
                std::string key = parseString();
                if (!ok) return v;
                ws();
                if (peek() != ':') { fail(); return v; }
                i++;
                Value val = parseValue();
                if (!ok) return v;
                v.obj.emplace_back(key, std::move(val));
                ws();
                if (peek() == ',') { i++; continue; }
                if (peek() == '}') { i++; break; }
                fail(); return v;
            }
        } else if (c == '[') {
            v.type = Value::Arr;
            i++;
            ws();
            if (peek() == ']') { i++; return v; }
            while (true) {
                Value val = parseValue();
                if (!ok) return v;
                v.arr.push_back(std::move(val));
                ws();
                if (peek() == ',') { i++; continue; }
                if (peek() == ']') { i++; break; }
                fail(); return v;
            }
        } else if (c == '"') {
            v.type = Value::Str;
            v.str = parseString();
        } else if (c == 't') {
            if (s.compare(i, 4, "true") == 0) { i += 4; v.type = Value::Bool; v.b = true; }
            else fail();
        } else if (c == 'f') {
            if (s.compare(i, 5, "false") == 0) { i += 5; v.type = Value::Bool; v.b = false; }
            else fail();
        } else if (c == 'n') {
            if (s.compare(i, 4, "null") == 0) { i += 4; v.type = Value::Null; }
            else fail();
        } else {
            size_t start = i;
            if (peek() == '-' || peek() == '+') i++;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i]=='.' || s[i]=='e' || s[i]=='E' || s[i]=='+' || s[i]=='-')) i++;
            if (i == start) { fail(); return v; }
            v.type = Value::Num;
            v.num = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        }
        return v;
    }
};

}

inline bool parse(const std::string& text, Value& out) {
    detail::Parser p(text);

    if (text.size() >= 3 &&
        (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        p.i = 3;
    }
    out = p.parseValue();
    p.ws();
    return p.ok;
}

inline std::string escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    size_t i = 0, n = in.size();
    while (i < n) {
        unsigned char c = (unsigned char)in[i];
        if (c < 0x80) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                default:
                    if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); out += b; }
                    else out.push_back((char)c);
            }
            i++;
            continue;
        }

        int len = (c >= 0xC2 && c <= 0xDF) ? 1
                : (c >= 0xE0 && c <= 0xEF) ? 2
                : (c >= 0xF0 && c <= 0xF4) ? 3 : -1;
        bool ok = (len > 0) && (i + (size_t)len < n);
        unsigned cp = 0;
        if (ok) {
            cp = c & (0x7F >> len);
            for (int k = 1; k <= len; k++) {
                unsigned char cc = (unsigned char)in[i + k];
                if ((cc & 0xC0) != 0x80) { ok = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (ok) {
                if ((len == 2 && cp < 0x800) || (len == 3 && cp < 0x10000) ||
                    (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
                    ok = false;
            }
        }
        if (ok) { out.append(in, i, (size_t)len + 1); i += (size_t)len + 1; }
        else    { out += "\xEF\xBF\xBD"; i++; }
    }
    return out;
}

inline std::string quote(const std::string& s) { return "\"" + escape(s) + "\""; }

inline std::string dump(const Value& v) {
    switch (v.type) {
        case Value::Null: return "null";
        case Value::Bool: return v.b ? "true" : "false";
        case Value::Num: {
            char buf[64];
            if (v.num == (double)(long long)v.num) std::snprintf(buf, sizeof(buf), "%lld", (long long)v.num);
            else std::snprintf(buf, sizeof(buf), "%.10g", v.num);
            return buf;
        }
        case Value::Str: return quote(v.str);
        case Value::Arr: {
            std::string out = "[";
            for (size_t k = 0; k < v.arr.size(); k++) { if (k) out += ","; out += dump(v.arr[k]); }
            return out + "]";
        }
        case Value::Obj: {
            std::string out = "{";
            for (size_t k = 0; k < v.obj.size(); k++) {
                if (k) out += ",";
                out += quote(v.obj[k].first) + ":" + dump(v.obj[k].second);
            }
            return out + "}";
        }
    }
    return "null";
}

}
