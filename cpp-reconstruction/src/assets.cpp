#include <solace/assets.hpp>
#include <solace/assets_generated.hpp>

#include <algorithm>

namespace solace::assets {

std::span<const Entry> all() noexcept {
    return generated::entries;
}

const Entry* find(std::string_view path) noexcept {
    const auto entries = all();
    const auto found = std::lower_bound(
        entries.begin(), entries.end(), path,
        [](const Entry& entry, std::string_view key) { return entry.path < key; });
    return found != entries.end() && found->path == path ? &*found : nullptr;
}

std::string_view source_path() noexcept {
    return generated::source_path;
}

std::string_view source_sha256() noexcept {
    return generated::source_sha256;
}

std::size_t total_bytes() noexcept {
    return generated::total_bytes;
}

} // namespace solace::assets
