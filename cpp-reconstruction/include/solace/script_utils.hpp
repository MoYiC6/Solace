#pragma once
#include <filesystem>

struct lua_State;

namespace solace::script {
// storage_root replaces the original APPDATA/solace_int/lua directory explicitly.
// Registration creates no files or directories. An empty root disables file/DB
// operations with an error, while the host-independent utilities remain usable.
void register_utils(lua_State* state, const std::filesystem::path& storage_root);
}
