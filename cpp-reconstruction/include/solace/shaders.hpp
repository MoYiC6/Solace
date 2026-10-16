#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace solace::shaders {
struct Spec {
    const char* asset_path;
    const char* entrypoint;
    const char* target;
    unsigned flags;
    const char* evidence_function;
    bool include_terminator;
};
struct Compiled {
    Spec spec;
    std::vector<std::uint8_t> bytecode;
    nlohmann::json reflection;
    std::string diagnostics;
};
std::span<const Spec> specifications() noexcept;
Compiled compile(const Spec& spec);
nlohmann::json compile_all(const std::filesystem::path& output_directory);
}
