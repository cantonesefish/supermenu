#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <string_view>

namespace sm::json {

struct Value {
    enum class Type { Null, Bool, Number, String } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;

    bool IsNull() const { return type == Type::Null; }
    bool IsBool() const { return type == Type::Bool; }
    bool IsNumber() const { return type == Type::Number; }
    bool IsString() const { return type == Type::String; }
};

using Object = std::map<std::string, Value>;

// Parses a flat JSON object. Nested objects/arrays are validated for syntax but
// skipped (their key is not stored); malformed input returns false.
bool ParseObject(std::string_view text, Object& out);

// Serialises a UTF-8 string as a JSON string literal (with quotes).
std::string Quote(std::string_view utf8);

inline std::wstring Utf8ToWide(std::string_view s);
inline std::string WideToUtf8(std::wstring_view s);

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string u(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), u.data(), n, nullptr, nullptr);
    return u;
}

}  // namespace sm::json
