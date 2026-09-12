#include "cli_launcher/ConfigManager.hpp"
#include <fstream>
#include <mutex>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace cccaster::main_app {

namespace {
Config &GlobalConfig() {
    static Config config;
    return config;
}
} // namespace

static inline std::string Trim(const std::string &s) {
    if (s.empty())
        return s;
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, (last - first + 1));
}

// --- Config Implementation ---

void Config::Load(const std::string &filePath) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    configData.clear();
    std::ifstream file(filePath);
    if (!file.is_open())
        return;

    std::string line;
    std::string currentSection = "";

    while (std::getline(file, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        if (line[0] == '[' && line[line.length() - 1] == ']') {
            currentSection = line.substr(1, line.length() - 2);
        } else {
            size_t delimPos = line.find('=');
            if (delimPos != std::string::npos) {
                std::string key = Trim(line.substr(0, delimPos));
                std::string value = Trim(line.substr(delimPos + 1));
                configData[currentSection][key] = value;
            }
        }
    }
}

void Config::Save(const std::string &filePath) {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::ofstream file(filePath);
    if (!file.is_open())
        return;

    for (const auto &sectionPair : configData) {
        if (!sectionPair.first.empty()) {
            file << "[" << sectionPair.first << "]\n";
        }
        for (const auto &keyValuePair : sectionPair.second) {
            file << keyValuePair.first << " = " << keyValuePair.second << "\n";
        }
        file << "\n";
    }
}

void Config::Clear() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    configData.clear();
}

bool Config::SaveChecked(const std::string &filePath) {
    const auto temporary = filePath + ".tmp";
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::ofstream file(temporary, std::ios::trunc);
        if (!file) return false;
        for (const auto &[section, values] : configData) {
            if (!section.empty()) file << '[' << section << "]\n";
            for (const auto &[key, value] : values) file << key << " = " << value << '\n';
            file << '\n';
        }
        file.flush();
        if (!file) return false;
        file.close();
        if (file.fail()) return false;
    }
    std::error_code error;
    if (std::filesystem::exists(filePath, error)) {
        std::filesystem::copy_file(filePath, filePath + ".bak",
                                  std::filesystem::copy_options::overwrite_existing, error);
        if (error) return false;
    } else if (error) return false;
#ifdef _WIN32
    return MoveFileExA(temporary.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    std::filesystem::rename(temporary, filePath, error);
    return !error;
#endif
}

std::string Config::GetString(const std::string &section, const std::string &key,
                              const std::string &defaultValue) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto secIt = configData.find(section);
    if (secIt != configData.end()) {
        auto keyIt = secIt->second.find(key);
        if (keyIt != secIt->second.end()) {
            return keyIt->second;
        }
    }
    return defaultValue;
}

int Config::GetInt(const std::string &section, const std::string &key, int defaultValue) const {
    std::string strVal = GetString(section, key, "");
    if (!strVal.empty()) {
        try {
            return std::stoi(strVal);
        } catch (...) {
            return defaultValue;
        }
    }
    return defaultValue;
}

void Config::SetString(const std::string &section, const std::string &key, const std::string &value) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    configData[section][key] = value;
}

void Config::SetInt(const std::string &section, const std::string &key, int value) {
    SetString(section, key, std::to_string(value));
}

// プロセス共通APIも同じパーサとロックを使う。
void ConfigManager::Load(const std::string &path) {
    GlobalConfig().Load(path);
}
void ConfigManager::Save(const std::string &path) {
    GlobalConfig().Save(path);
}
std::string ConfigManager::GetString(const std::string &section, const std::string &key,
                                     const std::string &fallback) {
    return GlobalConfig().GetString(section, key, fallback);
}
int ConfigManager::GetInt(const std::string &section, const std::string &key, int fallback) {
    return GlobalConfig().GetInt(section, key, fallback);
}
void ConfigManager::SetString(const std::string &section, const std::string &key, const std::string &value) {
    GlobalConfig().SetString(section, key, value);
}
void ConfigManager::SetInt(const std::string &section, const std::string &key, int value) {
    GlobalConfig().SetInt(section, key, value);
}
} // namespace cccaster::main_app
