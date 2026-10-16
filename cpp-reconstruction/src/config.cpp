#include <solace/config.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <type_traits>
#include <utility>

namespace solace::config {
namespace {

[[noreturn]] void invalid(std::string_view message) {
    throw Error(std::string(message));
}

[[noreturn]] void file_error(std::string_view operation, DWORD code = GetLastError()) {
    throw Error(std::string(operation) + ": Win32 error " + std::to_string(code));
}

struct FileHandle {
    HANDLE value;
    explicit FileHandle(HANDLE handle) : value(handle) {}
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
};

std::int64_t signed_integer(const nlohmann::json& value, std::int64_t low,
                            std::int64_t high) {
    if (!value.is_number_integer()) invalid("Expected an integer JSON value");
    if (value.is_number_unsigned()) {
        const auto v = value.get<std::uint64_t>();
        if (v > static_cast<std::uint64_t>(high)) invalid("Integer is out of range");
        return static_cast<std::int64_t>(v);
    }
    const auto v = value.get<std::int64_t>();
    if (v < low || v > high) invalid("Integer is out of range");
    return v;
}

std::int32_t int32_value(const nlohmann::json& value) {
    return static_cast<std::int32_t>(signed_integer(value,
        std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()));
}

std::uint8_t byte_value(const nlohmann::json& value) {
    return static_cast<std::uint8_t>(signed_integer(value, 0, 255));
}

bool bool_value(const nlohmann::json& value) {
    if (!value.is_boolean()) invalid("Expected a boolean JSON value");
    return value.get<bool>();
}

float float_value(const nlohmann::json& value) {
    if (!value.is_number()) invalid("Expected a numeric JSON value");
    const auto number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
        invalid("Number is outside the finite float range");
    return static_cast<float>(number);
}

void finite(float value) {
    if (!std::isfinite(value)) invalid("Non-finite float is not a configuration value");
}

void require_array(const nlohmann::json& value, std::size_t size) {
    if (!value.is_array() || value.size() != size) invalid("Unexpected array length");
}

const nlohmann::json& member(const nlohmann::json& value, const char* name) {
    if (!value.is_object() || !value.contains(name))
        invalid(std::string("Missing JSON member: ") + name);
    return value.at(name);
}

bool valid_key(std::string_view key) {
    return key.size() == 8 && std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

FieldValue snapshot(const BindingTarget& target) {
    return std::visit([](auto ref) -> FieldValue { return ref.get(); }, target);
}

void assign(const BindingTarget& target, const FieldValue& value) {
    std::visit([&value](auto ref) {
        using T = std::remove_reference_t<decltype(ref.get())>;
        ref.get() = std::get<T>(value);
    }, target);
}

} // namespace

std::uint32_t make_key(std::string_view category, std::string_view name) noexcept {
    // CT-decompile_1800271e0 lines 86-91 copy the fixed byte counts, then
    // write '.' and the final NUL. They do not use formatted string output.
    // sub_180005B80 obtains C-string length before sub_180027F80 hashes it;
    // thus a copied NUL inside category ends hashing before the later dot.
    std::uint64_t hash = 14695981039346656037ULL;
    const auto append = [&hash](std::string_view bytes) {
        for (const unsigned char byte : bytes) {
            if (byte == 0) return false; // The recovered hash consumes a C string.
            hash = (hash ^ byte) * 1099511628211ULL;
        }
        return true;
    };
    if (append(category.substr(0, 120)) && append(".")) append(name.substr(0, 120));
    return static_cast<std::uint32_t>(hash);
}

std::string key_string(std::uint32_t key) {
    std::ostringstream stream;
    stream << std::hex << std::nouppercase << std::setfill('0') << std::setw(8) << key;
    return stream.str();
}

FieldType type_of(const FieldValue& value) noexcept {
    return static_cast<FieldType>(value.index());
}

nlohmann::json encode_value(const FieldValue& value) {
    return std::visit([](const auto& data) -> nlohmann::json {
        using T = std::decay_t<decltype(data)>;
        if constexpr (std::is_same_v<T, Setting>) {
            nlohmann::json binding = nullptr;
            if (data.binding && data.binding->key != 0)
                binding = {{"k", data.binding->key}, {"m", data.binding->mode}, {"l", data.binding->l}};
            return {{"v", data.value}, {"b", std::move(binding)}};
        } else if constexpr (std::is_same_v<T, float>) {
            finite(data);
            return data;
        } else if constexpr (std::is_same_v<T, Vector3>) {
            for (float v : data) finite(v);
            return data;
        } else {
            return data;
        }
    }, value);
}

FieldValue decode_value(const nlohmann::json& value, FieldType type,
                        std::optional<std::size_t> boolean_count) {
    switch (type) {
    case FieldType::setting: {
        Setting result{bool_value(member(value, "v")), std::nullopt};
        const auto& binding = member(value, "b");
        if (!binding.is_null()) {
            if (!binding.is_object()) invalid("A binding must be an object or null");
            // CT-decompile_18024f270 supplies 0 for missing k/m and true for l.
            Binding recovered{binding.contains("k") ? int32_value(binding.at("k")) : 0,
                              binding.contains("m") ? int32_value(binding.at("m")) : 0,
                              binding.contains("l") ? bool_value(binding.at("l")) : true};
            if (recovered.key != 0) result.binding = recovered;
        }
        return result;
    }
    case FieldType::boolean: return bool_value(value);
    case FieldType::integer: return int32_value(value);
    case FieldType::byte: return byte_value(value);
    case FieldType::number: return float_value(value);
    case FieldType::color:
        require_array(value, 4);
        return Color{byte_value(value[0]), byte_value(value[1]), byte_value(value[2]), byte_value(value[3])};
    case FieldType::vector3:
        require_array(value, 3);
        return Vector3{float_value(value[0]), float_value(value[1]), float_value(value[2])};
    case FieldType::boolean_array: {
        if (!value.is_array()) invalid("Expected a boolean array");
        if (boolean_count && value.size() != *boolean_count) invalid("Boolean array length differs from its binding");
        BooleanArray result;
        result.reserve(value.size());
        for (const auto& entry : value) result.push_back(bool_value(entry));
        return result;
    }
    case FieldType::string:
        if (!value.is_string()) invalid("Expected a string JSON value");
        return value.get<std::string>();
    case FieldType::custom: invalid("Custom field type 9 needs a recovered serializer; no implementation is registered");
    }
    invalid("Unknown field type");
}

ConfigDocument::ConfigDocument() : root_{{"version", format_version}, {"fields", nlohmann::json::object()}} {}

ConfigDocument ConfigDocument::from_json(nlohmann::json root) {
    if (int32_value(member(root, "version")) != format_version)
        invalid("Only recovered configuration version 3 is supported; migration is unresolved");
    const auto& fields = member(root, "fields");
    if (!fields.is_object()) invalid("The fields member must be an object");
    for (const auto& item : fields.items())
        if (!valid_key(item.key())) invalid("Field keys must be eight lowercase hexadecimal digits");
    return ConfigDocument(std::move(root));
}

ConfigDocument ConfigDocument::parse(std::string_view text) {
    if (text.empty() || text.size() > maximum_file_size) invalid("Configuration size must be between 1 byte and 64 MiB");
    try {
        // Reject duplicate object members instead of silently losing one on edit.
        std::vector<std::set<std::string>> object_keys;
        auto callback = [&object_keys](int, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            if (event == nlohmann::json::parse_event_t::object_start) object_keys.emplace_back();
            else if (event == nlohmann::json::parse_event_t::key) {
                if (!object_keys.back().insert(parsed.get<std::string>()).second)
                    invalid("Duplicate JSON object member");
            } else if (event == nlohmann::json::parse_event_t::object_end) object_keys.pop_back();
            return true;
        };
        return from_json(nlohmann::json::parse(text.begin(), text.end(), callback));
    } catch (const nlohmann::json::exception& error) {
        throw Error(std::string("Invalid configuration JSON: ") + error.what());
    }
}

ConfigDocument ConfigDocument::load_file(const std::filesystem::path& path) {
    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) file_error("Open configuration");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) file_error("Read configuration size");
    if (size.QuadPart <= 0 || static_cast<std::uint64_t>(size.QuadPart) > maximum_file_size)
        invalid("Configuration size must be between 1 byte and 64 MiB");
    std::string text(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    if (!ReadFile(file.value, text.data(), static_cast<DWORD>(text.size()), &read, nullptr))
        file_error("Read configuration");
    if (read != text.size()) invalid("Configuration read was incomplete");
    return parse(text);
}

std::string ConfigDocument::dump() const { return root_.dump(2, ' '); }

void ConfigDocument::save_file(const std::filesystem::path& path) const {
    const auto text = dump();
    if (text.size() > maximum_file_size) invalid("Configuration exceeds the 64 MiB read limit");
    FileHandle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) file_error("Create configuration");
    DWORD written{};
    if (!WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr))
        file_error("Write configuration");
    if (written != text.size()) invalid("Configuration write was incomplete");
}

bool ConfigDocument::contains(std::uint32_t key) const { return root_.at("fields").contains(key_string(key)); }

std::optional<FieldValue> ConfigDocument::get(std::uint32_t key, FieldType type,
                                             std::optional<std::size_t> count) const {
    const auto& fields = root_.at("fields");
    const auto found = fields.find(key_string(key));
    if (found == fields.end()) return std::nullopt;
    return decode_value(*found, type, count);
}

void ConfigDocument::set(std::uint32_t key, const FieldValue& value) {
    root_["fields"][key_string(key)] = encode_value(value);
}

void Registry::bind(std::string_view category, std::string_view name, BindingTarget target) {
    const auto key = make_key(category, name);
    for (const auto& field : fields_)
        if (field.key == key) invalid("Duplicate configuration key or hash collision: " + key_string(key));
    fields_.push_back(Field{key, std::string(category), std::string(name), target});
}

ApplyReport Registry::apply(const ConfigDocument& document) const {
    ApplyReport report;
    std::vector<std::pair<const BindingTarget*, FieldValue>> pending;
    std::set<std::string> bound_keys;
    for (const auto& field : fields_) {
        const auto key = key_string(field.key);
        bound_keys.insert(key);
        const auto current = snapshot(field.target);
        const auto count = std::holds_alternative<BooleanArray>(current)
            ? std::optional<std::size_t>(std::get<BooleanArray>(current).size()) : std::nullopt;
        try {
            auto value = document.get(field.key, type_of(current), count);
            if (value) {
                report.applied.push_back(key);
                pending.emplace_back(&field.target, std::move(*value));
            } else report.missing.push_back(key);
        } catch (const Error& error) {
            throw Error("Field " + key + " (" + field.category + "." + field.name + "): " + error.what());
        }
    }
    for (const auto& entry : document.json().at("fields").items())
        if (!bound_keys.contains(entry.key())) report.unbound.push_back(entry.key());
    // Validate all payloads before modifying caller values. This is an explicit
    // recovery-project API policy, not a claim about original loader atomicity.
    for (const auto& [target, value] : pending) assign(*target, value);
    return report;
}

void Registry::capture(ConfigDocument& document) const {
    auto updated = document;
    for (const auto& field : fields_) updated.set(field.key, snapshot(field.target));
    document = std::move(updated);
}

std::filesystem::path default_directory() {
    const DWORD required = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (required == 0) file_error("Read APPDATA");
    std::wstring appdata(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(L"APPDATA", appdata.data(), required);
    if (copied == 0) file_error("Read APPDATA");
    if (copied >= required) invalid("APPDATA changed while being read");
    appdata.resize(copied);
    return std::filesystem::path(appdata) / L"solace_int" / L"configs";
}

void create_directory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) throw Error("Create configuration directory: " + error.message());
}

std::filesystem::path profile_path(const std::filesystem::path& directory, std::wstring_view name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ')
        invalid("Invalid configuration profile name");
    for (wchar_t c : name)
        if (c < 32 || std::wstring_view(L"\\/:*?\"<>|").find(c) != std::wstring_view::npos)
            invalid("A profile name must be a filename, without path separators");
    return directory / (std::wstring(name) + L".json");
}

std::vector<std::wstring> list_profiles(const std::filesystem::path& directory) {
    WIN32_FIND_DATAW data{};
    const auto pattern = directory / L"*.json";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) return {};
        file_error("List configurations", code);
    }
    std::vector<std::wstring> names;
    try {
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                names.push_back(std::filesystem::path(data.cFileName).stem().wstring());
        } while (FindNextFileW(find, &data));
        const auto code = GetLastError();
        if (code != ERROR_NO_MORE_FILES) file_error("Enumerate configurations", code);
    } catch (...) { FindClose(find); throw; }
    FindClose(find);
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace solace::config
