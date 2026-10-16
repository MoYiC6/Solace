#include <solace/shaders.hpp>
#include <iostream>
#include <stdexcept>

int main() {
    try {
        const auto report = solace::shaders::compile_all({});
        if (report.at("programs").size() != 17) throw std::runtime_error("Unexpected shader program count");
        for (const auto& program : report.at("programs")) {
            if (program.at("bytecode_size").get<unsigned>() == 0) throw std::runtime_error("Empty shader bytecode");
            if (program.at("source") == "sources/hlsl/motionblur_0046ABA0.hlsl") {
                if (program.at("reflection").at("resources").size() != 4)
                    throw std::runtime_error("Motion blur resources changed");
            }
        }
        auto invalid = solace::shaders::specifications().front(); invalid.entrypoint = "missing_entrypoint";
        bool rejected = false;
        try { solace::shaders::compile(invalid); } catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) throw std::runtime_error("Compiler failure was suppressed");
        std::cout << "12 recovered HLSL sources / 17 shader stages compiled and reflected\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
