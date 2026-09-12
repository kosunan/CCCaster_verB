#pragma once

#include <string>
#include <unordered_map>
#include <shared_mutex>

namespace cccaster::main_app {

class Config {
  public:
    void Load(const std::string &filePath);
    void Save(const std::string &filePath);
    // 設定UI用。失敗を通知し、置換前の内容は .bak に残す。
    bool SaveChecked(const std::string &filePath);
    void Clear();

    std::string GetString(const std::string &section, const std::string &key,
                          const std::string &defaultValue = "") const;
    int GetInt(const std::string &section, const std::string &key, int defaultValue = 0) const;
    void SetString(const std::string &section, const std::string &key, const std::string &value);
    void SetInt(const std::string &section, const std::string &key, int value);

  private:
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> configData;
    mutable std::shared_mutex mutex_;
};

class ConfigManager {
  public:
    static void Load(const std::string &filePath);
    static void Save(const std::string &filePath);

    static std::string GetString(const std::string &section, const std::string &key,
                                 const std::string &defaultValue = "");
    static int GetInt(const std::string &section, const std::string &key, int defaultValue = 0);
    static void SetString(const std::string &section, const std::string &key, const std::string &value);
    static void SetInt(const std::string &section, const std::string &key, int value);
};

} // namespace cccaster::main_app
