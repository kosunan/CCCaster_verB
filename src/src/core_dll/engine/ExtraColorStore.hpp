#pragma once
#include "core_dll/engine/ExtraColorFile.hpp"
#include <filesystem>

namespace cccaster::training_palette {
std::filesystem::path ExtraPath(unsigned character,unsigned extra);
bool ReadColorFile(const std::filesystem::path& path,std::vector<uint8_t>& bytes,std::string& error);
bool WriteColorFile(const std::filesystem::path& path,std::span<const uint8_t> bytes,std::string& error);
bool LoadExtra(unsigned character,unsigned extra,ExtraColor& value,std::string& error);
bool SaveExtra(unsigned character,unsigned extra,const ExtraColor& value,std::string& error);
}
