#include <solace/config.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>

using namespace solace::config;

namespace {
int checks = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

template<class F> void rejects(F action, const char* description) {
    bool failed = false;
    try { action(); } catch (const Error&) { failed = true; }
    check(failed, description);
}

std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Test input could not be opened");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Scratch {
    std::filesystem::path directory;
    std::vector<std::filesystem::path> outputs;
    Scratch() {
        directory = std::filesystem::temp_directory_path() /
            (L"solace-config-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Scratch directory already exists");
    }
    std::filesystem::path file(std::wstring_view name) {
        auto path = profile_path(directory, name);
        outputs.push_back(path);
        return path;
    }
    ~Scratch() {
        std::error_code ignored;
        for (const auto& path : outputs) std::filesystem::remove(path, ignored);
        std::filesystem::remove(directory, ignored);
    }
};

void test_keys() {
    check(key_string(make_key("playerlist", "priority")) == "40a5e9e6", "Recovered priority key differs");
    check(key_string(make_key("peek assistance", "quick peek color")) == "3428e79d", "Recovered color key differs");
    check(key_string(make_key(std::string(121, 'a'), std::string(121, 'b'))) == "186e0809", "120-byte truncation differs");
    check(key_string(make_key(std::string(119, 'a') + "\xC3\xA9", "x")) == "218ad8d9", "Truncation must count UTF-8 bytes");
    check(key_string(make_key(std::string_view("a\0b", 3), "x")) == "8601ec8c", "Hash must end at the first NUL");
    check(key_string(make_key("", "")) == "86018bb1", "Empty category/name key differs");
    check(key_string(1) == "00000001", "Key must retain leading zeroes");
}

void test_values() {
    const std::vector<FieldValue> values{
        Setting{true, Binding{65, 7, false}}, true, std::int32_t{-1234}, std::uint8_t{255},
        1.25f, Color{1, 2, 3, 4}, Vector3{1.0f, -2.0f, 0.5f}, BooleanArray{true, false}, std::string("text")
    };
    for (const auto& value : values)
        check(decode_value(encode_value(value), type_of(value)) == value, "Supported field did not round-trip");
    check(encode_value(Setting{true, Binding{0, 5, false}}).at("b").is_null(), "Zero binding key must serialize as null");
    auto old_binding = decode_value(nlohmann::json{{"v", false}, {"b", {{"k", 65}}}}, FieldType::setting);
    check(std::get<Setting>(old_binding).binding == Binding{65, 0, true}, "Recovered omitted binding members differ");
    check(!std::get<Setting>(decode_value(nlohmann::json{{"v", false}, {"b", nullptr}}, FieldType::setting)).binding,
          "Null binding should be empty");
    rejects([] { decode_value(256, FieldType::byte); }, "Byte overflow accepted");
    rejects([] { decode_value(-1, FieldType::byte); }, "Negative byte accepted");
    rejects([] { decode_value(2147483648ULL, FieldType::integer); }, "Int32 overflow accepted");
    rejects([] { decode_value(1.5, FieldType::integer); }, "Fractional integer accepted");
    rejects([] { decode_value(1, FieldType::boolean); }, "Numeric boolean accepted");
    rejects([] { decode_value(nlohmann::json::array({1, 2, 3}), FieldType::color); }, "Wrong color length accepted");
    rejects([] { decode_value(nlohmann::json::array({true}), FieldType::boolean_array, 2); }, "Wrong boolean array length accepted");
    rejects([] { decode_value(1, FieldType::custom); }, "Unrecovered custom serializer accepted");
    rejects([] { encode_value(std::numeric_limits<float>::infinity()); }, "Non-finite float accepted");
}

void test_document() {
    const auto original = nlohmann::json{
        {"version", 3}, {"producer", {"retained", 1}},
        {"fields", {{"40a5e9e6", 3}, {"deadbeef", {{"unrecovered", {1, 2, 3}}}}}}
    };
    auto document = ConfigDocument::from_json(original);
    check(ConfigDocument::parse(document.dump()).json() == original, "Unknown data lost in document round-trip");
    document.set(0x40a5e9e6, std::uint8_t{4});
    check(document.json().at("producer") == original.at("producer"), "Top-level metadata lost on edit");
    check(document.json().at("fields").at("deadbeef") == original.at("fields").at("deadbeef"), "Unbound field changed on edit");
    check(!document.get(1, FieldType::integer), "Missing field created a default");
    rejects([] { ConfigDocument::parse(R"({"version":2,"fields":{}})"); }, "Unrecovered version 2 accepted");
    rejects([] { ConfigDocument::parse(R"({"version":4,"fields":{}})"); }, "Unknown future version accepted");
    rejects([] { ConfigDocument::parse(R"({"version":3,"fields":{"ABCDEF00":0}})"); }, "Noncanonical key accepted");
    rejects([] { ConfigDocument::parse(R"({"version":3,"fields":{},"version":3})"); }, "Duplicate JSON member accepted");
    rejects([] { ConfigDocument::parse(""); }, "Empty document accepted");
    rejects([] { ConfigDocument::parse("{"); }, "Malformed JSON accepted");
}

void test_registry() {
    std::uint8_t priority = 0;
    Color color{};
    bool missing = true;
    Registry registry;
    registry.bind("playerlist", "priority", std::ref(priority));
    registry.bind("peek assistance", "quick peek color", std::ref(color));
    registry.bind("not-present", "setting", std::ref(missing));
    auto document = ConfigDocument::parse(R"({"version":3,"fields":{"40a5e9e6":5,"3428e79d":[10,20,30,40],"deadbeef":{"custom":true}}})");
    const auto report = registry.apply(document);
    check(priority == 5 && color == Color{10, 20, 30, 40}, "Registry failed to apply known fields");
    check(missing && report.applied.size() == 2 && report.missing.size() == 1 && report.unbound.size() == 1,
          "Apply did not report exact missing/unbound fields");
    auto bad = ConfigDocument::parse(R"({"version":3,"fields":{"40a5e9e6":8,"3428e79d":[10]}})");
    rejects([&] { registry.apply(bad); }, "Invalid bound field accepted");
    check(priority == 5, "Invalid document partially applied a known field");
    priority = 9;
    registry.capture(document);
    check(std::get<std::uint8_t>(*document.get(0x40a5e9e6, FieldType::byte)) == 9, "Registry capture failed");
    check(document.json().at("fields").at("deadbeef").at("custom") == true, "Capture discarded unbound data");
    rejects([&] { registry.bind("playerlist", "priority", std::ref(priority)); }, "Duplicate binding accepted");
}

void test_files(Scratch& scratch) {
    const auto alpha = scratch.file(L"alpha");
    const auto beta = scratch.file(L"beta");
    ConfigDocument doc;
    doc.set(make_key("playerlist", "priority"), std::uint8_t{3});
    doc.save_file(beta);
    doc.save_file(alpha);
    check(ConfigDocument::load_file(alpha).json() == doc.json(), "File round-trip differs");
    check(bytes(alpha) == doc.dump(), "File writer changed JSON bytes");
    check(list_profiles(scratch.directory) == std::vector<std::wstring>{L"alpha", L"beta"}, "Profile enumeration/sorting differs");
    rejects([&] { profile_path(scratch.directory, L"../outside"); }, "Parent traversal accepted");
    rejects([&] { ConfigDocument::load_file(scratch.directory / L"missing.json"); }, "Missing file accepted");
    const auto empty = scratch.file(L"empty");
    { std::ofstream output(empty, std::ios::binary); }
    rejects([&] { ConfigDocument::load_file(empty); }, "Empty file accepted");
    const auto oversized = scratch.file(L"oversized");
    {
        std::ofstream output(oversized, std::ios::binary);
        output.seekp(static_cast<std::streamoff>(maximum_file_size));
        output.put('x');
    }
    rejects([&] { ConfigDocument::load_file(oversized); }, "File over 64 MiB accepted");
}

std::size_t test_originals(const std::filesystem::path& directory, Scratch& scratch) {
    const auto profiles = list_profiles(directory);
    if (profiles.empty()) throw std::runtime_error("Original configuration directory has no profiles");
    std::size_t count = 0;
    for (const auto& name : profiles) {
        const auto original_path = profile_path(directory, name);
        const auto before = bytes(original_path);
        const auto original = ConfigDocument::load_file(original_path);
        const auto output = scratch.file(L"original-copy-" + std::to_wstring(count));
        original.save_file(output);
        check(ConfigDocument::load_file(output).json() == original.json(), "Original configuration document lost data");
        check(bytes(original_path) == before, "Original configuration bytes changed");
        ++count;
    }
    return count;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 2) throw std::runtime_error("Usage: config_tests [original-config-directory]");
        Scratch scratch;
        test_keys();
        test_values();
        test_document();
        test_registry();
        test_files(scratch);
        const auto original_count = argc == 2 ? test_originals(argv[1], scratch) : 0;
        std::cout << "Passed " << checks << " configuration checks; original profiles round-tripped: "
                  << original_count << ". Original files were read only.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Configuration test failure: " << error.what() << '\n';
        return 1;
    }
}
