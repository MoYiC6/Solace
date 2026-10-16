#include <solace/api.h>
#include <solace/assets.hpp>
#include <solace/config.hpp>
#include <solace/script.hpp>
#include <algorithm>
#include <cstring>
#include <exception>
#include <string_view>

namespace {
void message(std::string_view text, char* target, size_t capacity) noexcept {
    if (!target || !capacity) return;
    const size_t size = std::min(text.size(), capacity - 1);
    std::memcpy(target, text.data(), size);
    target[size] = '\0';
}
bool bad_buffer(const void* pointer, size_t size) noexcept { return !pointer && size != 0; }
}

const char* solace_core_version(void) { return "0.2.0-partial-reconstruction"; }
size_t solace_asset_count(void) { return solace::assets::all().size(); }
int solace_get_asset(size_t index, solace_asset_view* output) {
    if (!output || output->structure_size != sizeof(solace_asset_view) || index >= solace_asset_count()) return 1;
    const auto& entry = solace::assets::all()[index];
    output->path = entry.path.data(); output->path_size = entry.path.size();
    output->data = reinterpret_cast<const unsigned char*>(entry.bytes.data()); output->data_size = entry.bytes.size();
    output->sha256 = entry.sha256.data(); output->sha256_size = entry.sha256.size();
    output->original_rva = entry.rva; output->is_template = entry.is_template ? 1 : 0;
    return 0;
}
int solace_field_key(const char* category, size_t category_size, const char* name, size_t name_size, uint32_t* output) {
    if (bad_buffer(category, category_size) || bad_buffer(name, name_size) || !output) return 1;
    *output = solace::config::make_key({category ? category : "", category_size}, {name ? name : "", name_size});
    return 0;
}
int solace_validate_config(const char* text, size_t size, size_t* field_count, char* error, size_t capacity) {
    message("", error, capacity);
    if (bad_buffer(text, size) || bad_buffer(error, capacity) || !field_count) {
        message("Invalid buffer or output argument", error, capacity); return 1;
    }
    try {
        const auto document = solace::config::ConfigDocument::parse({text ? text : "", size});
        *field_count = document.json().at("fields").size();
        message("", error, capacity);
        return 0;
    } catch (const std::exception& failure) { message(failure.what(), error, capacity); return 2; }
    catch (...) { message("Unknown C++ exception", error, capacity); return 2; }
}
int solace_run_lua(const char* text, size_t size, char* error, size_t capacity) {
    message("", error, capacity);
    if (bad_buffer(text, size) || bad_buffer(error, capacity)) {
        message("Invalid buffer argument", error, capacity); return 1;
    }
    try {
        solace::script::Runtime runtime;
        runtime.run({text ? text : "", size});
        message("", error, capacity);
        return 0;
    } catch (const std::exception& failure) { message(failure.what(), error, capacity); return 2; }
    catch (...) { message("Unknown C++ exception", error, capacity); return 2; }
}
