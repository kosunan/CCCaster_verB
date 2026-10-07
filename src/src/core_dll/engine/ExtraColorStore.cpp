#include "core_dll/engine/ExtraColorStore.hpp"
#include "core_dll/common/DataPaths.hpp"
#include <fstream>
#include <windows.h>

namespace cccaster::training_palette {
std::filesystem::path ExtraPath(unsigned character,unsigned extra) {
    const auto root=core::paths::Resolve("extra_colors");
    return std::filesystem::path(std::u8string(root.begin(),root.end()))/
        ("character_"+std::to_string(character))/("extra_"+std::to_string(extra+1)+".cccolor");
}
bool ReadColorFile(const std::filesystem::path& path,std::vector<uint8_t>& bytes,std::string& error) {
    try {
        const auto size=std::filesystem::file_size(path);
        if(size>MaxExtraBytes){error="Color file is too large.";return false;}
        std::ifstream file(path,std::ios::binary);std::vector<uint8_t> next(size);
        if(!file.read(reinterpret_cast<char*>(next.data()),next.size())){error="Cannot read color file.";return false;}
        bytes=std::move(next);error.clear();return true;
    }catch(const std::exception&){error="Cannot open color file.";return false;}
}
bool WriteColorFile(const std::filesystem::path& path,std::span<const uint8_t> bytes,std::string& error) {
    std::filesystem::path temporary;
    try {
        if(bytes.empty() || bytes.size()>MaxExtraBytes){error="Invalid color file size.";return false;}
        auto absolute=std::filesystem::absolute(path);
        if(!std::filesystem::is_directory(absolute.parent_path())){error="Destination folder does not exist.";return false;}
        wchar_t name[MAX_PATH];
        if(!GetTempFileNameW(absolute.parent_path().c_str(),L"CCP",0,name)){error="Cannot create temporary color file.";return false;}
        temporary=name;
        {std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
         file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());file.flush();
         if(!file)throw std::runtime_error("write");}
        if(!MoveFileExW(temporary.c_str(),absolute.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("replace");
        error.clear();return true;
    }catch(const std::exception&) {
        if(!temporary.empty()){std::error_code ignored;std::filesystem::remove(temporary,ignored);}
        error="Cannot save color file; the previous file was kept.";return false;
    }
}
bool LoadExtra(unsigned character,unsigned extra,ExtraColor& value,std::string& error) {
    if(character>100 || extra>=ExtraCount){error="Invalid extra slot.";return false;}
    std::vector<uint8_t> bytes;
    if(!ReadColorFile(ExtraPath(character,extra),bytes,error))return false;
    ExtraColor next;
    if(!DecodeExtra(bytes,next) || next.character!=character){error="Extra color file is invalid or belongs to another character.";return false;}
    value=std::move(next);return true;
}
bool SaveExtra(unsigned character,unsigned extra,const ExtraColor& value,std::string& error) {
    if(character>100 || character!=value.character || extra>=ExtraCount){error="Invalid extra slot.";return false;}
    const auto bytes=EncodeExtra(value);
    if(bytes.empty()){error="Extra color data exceeds the supported size.";return false;}
    try{std::filesystem::create_directories(ExtraPath(character,extra).parent_path());}
    catch(const std::exception&){error="Cannot create extra color folder.";return false;}
    return WriteColorFile(ExtraPath(character,extra),bytes,error);
}
}
