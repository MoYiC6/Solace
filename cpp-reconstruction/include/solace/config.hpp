#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace solace::config {

inline constexpr int format_version = 3;
inline constexpr std::uint64_t maximum_file_size = 64ULL * 1024 * 1024;

class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Numeric values come from the recovered 24-byte field registration record.
enum class FieldType : std::uint8_t {
    setting = 0, boolean = 1, integer = 2, byte = 3, number = 4,
    color = 5, vector3 = 6, boolean_array = 7, string = 8, custom = 9
};

struct Binding {
    std::int32_t key{};
    std::int32_t mode{}; // Retained verbatim; the Solace mode enumeration is unresolved.
    bool l{true};       // Recovered member; absent l loads as true. UI semantics unresolved.
    bool operator==(const Binding&) const = default;
};

struct Setting {
    bool value{};
    std::optional<Binding> binding;
    bool operator==(const Setting&) const = default;
};

using Color = std::array<std::uint8_t, 4>;
using Vector3 = std::array<float, 3>;
using BooleanArray = std::vector<bool>;
using FieldValue = std::variant<Setting, bool, std::int32_t, std::uint8_t,
                                float, Color, Vector3, BooleanArray, std::string>;

std::uint32_t make_key(std::string_view category, std::string_view name) noexcept;
std::string key_string(std::uint32_t key);
FieldType type_of(const FieldValue& value) noexcept;
nlohmann::json encode_value(const FieldValue& value);
FieldValue decode_value(const nlohmann::json& value, FieldType type,
                        std::optional<std::size_t> boolean_count = std::nullopt);

// A document is independent of any application globals. Unknown field payloads
// and top-level members survive parse/edit/write semantically unchanged.
class ConfigDocument {
public:
    ConfigDocument();
    static ConfigDocument parse(std::string_view text);
    static ConfigDocument from_json(nlohmann::json root);
    static ConfigDocument load_file(const std::filesystem::path& path);

    const nlohmann::json& json() const noexcept { return root_; }
    std::string dump() const;
    void save_file(const std::filesystem::path& path) const;
    bool contains(std::uint32_t key) const;
    std::optional<FieldValue> get(std::uint32_t key, FieldType type,
                                  std::optional<std::size_t> boolean_count = std::nullopt) const;
    void set(std::uint32_t key, const FieldValue& value);

private:
    explicit ConfigDocument(nlohmann::json root) : root_(std::move(root)) {}
    nlohmann::json root_;
};

using BindingTarget = std::variant<
    std::reference_wrapper<Setting>, std::reference_wrapper<bool>,
    std::reference_wrapper<std::int32_t>, std::reference_wrapper<std::uint8_t>,
    std::reference_wrapper<float>, std::reference_wrapper<Color>,
    std::reference_wrapper<Vector3>, std::reference_wrapper<BooleanArray>,
    std::reference_wrapper<std::string>>;

struct ApplyReport {
    std::vector<std::string> applied;
    std::vector<std::string> missing;
    std::vector<std::string> unbound;
};

// The caller owns each bound value and keeps it alive until the registry dies.
// No field names, defaults, or hidden reset behavior are invented here.
// This explicit binding API does not reproduce the original loader's reset
// and version-dependent default initialization before applying a document.
class Registry {
public:
    void bind(std::string_view category, std::string_view name, BindingTarget target);
    ApplyReport apply(const ConfigDocument& document) const;
    void capture(ConfigDocument& document) const;
    std::size_t size() const noexcept { return fields_.size(); }

private:
    struct Field {
        std::uint32_t key;
        std::string category;
        std::string name;
        BindingTarget target;
    };
    std::vector<Field> fields_;
};

// Reading these paths does not create directories or change APPDATA files.
std::filesystem::path default_directory();
void create_directory(const std::filesystem::path& directory);
std::filesystem::path profile_path(const std::filesystem::path& directory,
                                   std::wstring_view profile_name);
std::vector<std::wstring> list_profiles(const std::filesystem::path& directory);

} // namespace solace::config
