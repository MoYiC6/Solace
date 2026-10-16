#include <solace/api.h>
#include <cstring>
#include <iostream>

int main() {
    if (solace_asset_count() != 177) return 1;
    solace_asset_view entry{};
    if (solace_get_asset(0, &entry) != 1) return 2;
    entry.structure_size = sizeof(entry);
    if (solace_get_asset(0, &entry) || !entry.data || !entry.path || entry.sha256_size != 64) return 3;
    if (solace_get_asset(solace_asset_count(), &entry) != 1) return 4;
    uint32_t key{};
    if (solace_field_key("playerlist", 10, "priority", 8, &key) || key != 0x40a5e9e6) return 5;
    char error[256]{};
    size_t fields{};
    const char config[] = "{\"version\":3,\"fields\":{\"40a5e9e6\":0}}";
    if (solace_validate_config(config, std::strlen(config), &fields, error, sizeof(error)) || fields != 1) return 6;
    if (solace_validate_config("{}", 2, &fields, error, sizeof(error)) != 2 || !error[0]) return 7;
    if (solace_validate_config(nullptr, 2, &fields, error, sizeof(error)) != 1) return 8;
    char invalid_error[2] = {'X', 'X'};
    if (solace_run_lua(nullptr, 1, invalid_error, sizeof(invalid_error)) != 1 || invalid_error[1] != '\0') return 12;
    const char lua[] = "assert(math.Lerp(0, 8, 0.5) == 4)";
    if (solace_run_lua(lua, std::strlen(lua), error, sizeof(error))) return 9;
    if (solace_run_lua("gui.Checkbox()", 14, error, sizeof(error)) != 2) return 10;
    char tiny[1] = {'x'};
    if (solace_run_lua("?", 1, tiny, 1) != 2 || tiny[0]) return 11;
    std::cout << "Recovered-core DLL ABI checks passed\n";
    return 0;
}
