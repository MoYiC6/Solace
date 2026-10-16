#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace solace::assets {

// Recovered literal bytes, without the original DLL's trailing NUL. All views
// refer to immutable data with static lifetime; no host execution is implied.
struct Entry {
    std::string_view path;
    std::string_view evidence_id;
    std::string_view kind;
    std::string_view sha256;
    std::string_view section;
    std::uint32_t rva;
    std::uint64_t file_offset;
    bool is_template;
    std::span<const std::uint8_t> bytes;
};

// Entries are ordered by manifest-relative path. Lookup is exact and
// case-sensitive; it does not normalize or reinterpret the requested path.
[[nodiscard]] std::span<const Entry> all() noexcept;
[[nodiscard]] const Entry* find(std::string_view path) noexcept;
[[nodiscard]] std::string_view source_path() noexcept;
[[nodiscard]] std::string_view source_sha256() noexcept;
[[nodiscard]] std::size_t total_bytes() noexcept;

} // namespace solace::assets
