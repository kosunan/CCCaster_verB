#include "shared_contracts/ConfigPath.hpp"
#include <fstream>
#include <chrono>
#include <iostream>
int main() {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / ("cccaster_config_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directory(dir);
    try {
        const auto path = cccaster::ConfigPath(dir);
        if (path.filename() != "cccaster.ini" || fs::exists(path)) throw std::runtime_error("fresh");
        std::ofstream(dir / "cccaster_v10.ini") << "legacy";
        if (!fs::exists(cccaster::ConfigPath(dir))) throw std::runtime_error("migration");
        std::string value;
        std::ifstream(path) >> value;
        if (value != "legacy") throw std::runtime_error("content");
        std::ofstream(path) << "current";
        std::ifstream(cccaster::ConfigPath(dir)) >> value;
        if (value != "current" || !fs::exists(dir / "cccaster_v10.ini")) throw std::runtime_error("preservation");
    } catch (const std::exception& e) { std::cerr << e.what(); fs::remove_all(dir); return 1; }
    fs::remove_all(dir);
}
