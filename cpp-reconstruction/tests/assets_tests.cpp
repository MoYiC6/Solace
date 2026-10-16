#include <solace/assets.hpp>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string_view>

namespace {
void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "assets test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}
}

int main() {
    using namespace solace::assets;
    const auto entries = all();
    require(entries.size() == 177, "146 literals and 31 binary assets are present");
    require(total_bytes() == 3081129, "total recovered byte count");
    require(source_sha256() == "8e60e1071acd82b08dfd2bbc41b1155e10cfd113075f0ff40ed6372c2f5f232b",
            "original DLL provenance");
    std::size_t sum = 0;
    std::size_t templates = 0;
    std::map<std::string_view, std::size_t> counts;
    std::string_view previous;
    for (const auto& entry : entries) {
        require(previous.empty() || previous < entry.path, "strict path ordering and uniqueness");
        require(find(entry.path) == &entry, "every stable path resolves to its original entry");
        require(entry.sha256.size() == 64, "per-entry SHA-256 retained");
        require(!entry.bytes.empty(), "original literal payload retained");
        previous = entry.path;
        sum += entry.bytes.size();
        templates += entry.is_template ? 1 : 0;
        ++counts[entry.kind];
    }
    require(sum == total_bytes(), "enumeration byte accounting");
    require(templates == 6, "five JavaScript templates and one material template");
    const std::map<std::string_view, std::size_t> expected{
        {"hlsl", 12}, {"hud_javascript", 29}, {"material", 30},
        {"javascript_template", 5}, {"javascript", 4}, {"svg", 65}, {"material_template", 1},
        {"ttf", 5}, {"png", 26}};
    require(counts == expected, "all nine resource families retained");
    const auto* shader = find("sources/hlsl/shader_0046AA80.hlsl");
    require(shader != nullptr, "known shader path");
    require(shader->rva == 0x46B880 && shader->file_offset == 0x46AA80,
            "known shader location metadata");
    require(shader->bytes.size() == 284 && !shader->is_template, "known shader metadata");
    require(shader->sha256 == "473e7c903b62c6d8d65c09ddfc52675ecdab7444c69b5b345b95a22267e9d5b9",
            "known shader digest");
    require(find("sources/hud_javascript/runtime.js") != nullptr, "runtime script available as data");
    require(find("") == nullptr && find("sources/absent.js") == nullptr,
            "absent paths report absence");
    require(find("SOURCES/hud_javascript/runtime.js") == nullptr,
            "lookup preserves exact path identity");
    std::cout << "assets: 177 entries, 3081129 bytes, metadata and exact lookup verified\n";
}
