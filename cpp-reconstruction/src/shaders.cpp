#include <solace/shaders.hpp>
#include <solace/assets.hpp>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using nlohmann::json;
namespace solace::shaders {
namespace {
constexpr unsigned strict = D3DCOMPILE_ENABLE_STRICTNESS;
#define PAIR(path) Spec{path,"vs_main","vs_5_0",strict,"0x1802c4450",false}, Spec{path,"ps_main","ps_5_0",strict,"0x1802c4450",false}
constexpr Spec specs[] = {
    {"sources/hlsl/shader_0046AA80.hlsl", "main", "vs_5_0", 0, "0x18018bac0", true},
    {"sources/hlsl/motionblur_0046ABA0.hlsl", "main", "ps_5_0", 0, "0x18018bac0", true},
    {"sources/hlsl/storm_004BF560.hlsl", "main", "ps_5_0", strict, "0x180226590", false},
    {"sources/hlsl/sky_004C0840.hlsl", "main", "ps_5_0", strict, "0x180226590", false},
    {"sources/hlsl/sky_004C0FF0.hlsl", "main", "ps_5_0", strict, "0x180226590", false},
    {"sources/hlsl/sky_004C1A10.hlsl", "main", "ps_5_0", strict, "0x180226590", false},
    {"sources/hlsl/sky_004C22F0.hlsl", "main", "ps_5_0", strict, "0x180226590", false},
    PAIR("sources/hlsl/shader_004C5C20.hlsl"),
    PAIR("sources/hlsl/composite_005508F0.hlsl"),
    PAIR("sources/hlsl/scene_005554E0.hlsl"),
    PAIR("sources/hlsl/scene_00556E90.hlsl"),
    PAIR("sources/hlsl/scene_005574B0.hlsl")
};
#undef PAIR
void require(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed, HRESULT=" + std::to_string(result));
}
json reflection(ID3DBlob* code) {
    ComPtr<ID3D11ShaderReflection> reflector;
    require(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&reflector)), "D3DReflect");
    D3D11_SHADER_DESC description{};
    require(reflector->GetDesc(&description), "Get shader description");
    json bindings = json::array(), buffers = json::array();
    for (unsigned i = 0; i < description.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC resource{};
        require(reflector->GetResourceBindingDesc(i, &resource), "Get resource binding");
        bindings.push_back({{"name", resource.Name}, {"type", resource.Type}, {"slot", resource.BindPoint}, {"count", resource.BindCount}});
    }
    for (unsigned i = 0; i < description.ConstantBuffers; ++i) {
        auto* buffer = reflector->GetConstantBufferByIndex(i);
        D3D11_SHADER_BUFFER_DESC data{};
        require(buffer->GetDesc(&data), "Get constant buffer");
        json variables = json::array();
        for (unsigned j = 0; j < data.Variables; ++j) {
            auto* variable = buffer->GetVariableByIndex(j);
            D3D11_SHADER_VARIABLE_DESC field{};
            D3D11_SHADER_TYPE_DESC type{};
            require(variable->GetDesc(&field), "Get buffer variable");
            require(variable->GetType()->GetDesc(&type), "Get variable type");
            variables.push_back({{"name", field.Name}, {"offset", field.StartOffset}, {"size", field.Size},
                                 {"rows", type.Rows}, {"columns", type.Columns}, {"elements", type.Elements}});
        }
        buffers.push_back({{"name", data.Name}, {"size", data.Size}, {"variables", variables}});
    }
    return {{"resources", bindings}, {"constant_buffers", buffers},
            {"instruction_count", description.InstructionCount}, {"input_count", description.InputParameters},
            {"output_count", description.OutputParameters}, {"compiler", description.Creator}};
}
}
std::span<const Spec> specifications() noexcept { return specs; }
Compiled compile(const Spec& spec) {
    const auto* source = assets::find(spec.asset_path);
    if (!source || source->kind != "hlsl") throw std::runtime_error("Shader asset is missing or has wrong kind");
    std::vector<std::uint8_t> text(source->bytes.begin(), source->bytes.end());
    if (spec.include_terminator) text.push_back(0);
    ComPtr<ID3DBlob> bytecode, diagnostics;
    // pSourceName is diagnostic metadata; this recovery build uses the asset path.
    const auto result = D3DCompile(text.data(), text.size(), spec.asset_path, nullptr, nullptr,
        spec.entrypoint, spec.target, spec.flags, 0, &bytecode, &diagnostics);
    const std::string message = diagnostics
        ? std::string(static_cast<const char*>(diagnostics->GetBufferPointer()), diagnostics->GetBufferSize()) : "";
    if (FAILED(result)) throw std::runtime_error(std::string(spec.asset_path) + ": " + message);
    const auto* bytes = static_cast<const std::uint8_t*>(bytecode->GetBufferPointer());
    return {spec, {bytes, bytes + bytecode->GetBufferSize()}, reflection(bytecode.Get()), message};
}
json compile_all(const std::filesystem::path& output_directory) {
    // Compile everything before writing any candidate output; errors are not hidden.
    std::vector<Compiled> output;
    for (const auto& spec : specs) output.push_back(compile(spec));
    if (!output_directory.empty()) std::filesystem::create_directories(output_directory);
    json manifest = {{"source_sha256", assets::source_sha256()}, {"programs", json::array()},
        {"status", "shader-compilation-verified-rendering-not-verified"}};
    for (const auto& item : output) {
        auto name = std::filesystem::path(item.spec.asset_path).stem().string() + "." + item.spec.target + ".cso";
        if (!output_directory.empty()) {
            std::ofstream file(output_directory / name, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(item.bytecode.data()), static_cast<std::streamsize>(item.bytecode.size()));
            file.close();
            if (!file) throw std::runtime_error("Writing compiled shader failed");
        }
        manifest["programs"].push_back({{"source", item.spec.asset_path}, {"entrypoint", item.spec.entrypoint},
            {"target", item.spec.target}, {"flags", item.spec.flags}, {"bytecode_size", item.bytecode.size()},
            {"evidence_function", item.spec.evidence_function}, {"output", name},
            {"diagnostics", item.diagnostics}, {"reflection", item.reflection}});
    }
    if (!output_directory.empty()) {
        std::ofstream file(output_directory / "manifest.json"); file << manifest.dump(2) << '\n'; file.close();
        if (!file) throw std::runtime_error("Writing shader manifest failed");
    }
    return manifest;
}
}
