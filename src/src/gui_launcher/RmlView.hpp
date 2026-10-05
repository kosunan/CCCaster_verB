#pragma once
#include "GuiProtocol.hpp"
#include <RmlUi/Core.h>
#include <functional>
#include <map>

namespace cccaster::gui {
// 画面は状態を表示し、既存の検証済みコマンド境界へ操作を渡す。
class RmlView final : public Rml::EventListener {
    Rml::Context& context_;
    Rml::ElementDocument* document_ = nullptr;
    std::function<void(Json)> send_;
    Json state_;
    std::string language_="ja", page_="matching", matchingTab_="public", selectedPerson_, peopleSignature_, requestSignature_;
    int listPage_=0;
    bool initialized_=false, updating_=false;
    std::vector<std::tuple<Rml::Element*,std::string,std::string>> translations_;
    void Translate(const std::string& language);
    void People();
    const Json* SelectedPerson() const;
    void Requests();
    void Navigate(const std::string& page);
    void MatchingTab(const std::string& tab);
    void Registration();
    void Disable(const char* id, bool disabled);
public:
    RmlView(Rml::Context& context,std::function<void(Json)> send);
    ~RmlView() override;
    Rml::Element* Find(const std::string& id) const;
    std::string Value(const char* id) const;
    void SetValue(const char* id,const std::string& value,bool force=false);
    void Text(const char* id,const std::string& value);
    void Show(const char* id,bool visible);
    void State(const Json& state);
    void ProcessEvent(Rml::Event& event) override;
    Json Inspect() const;
    Json Test(const Json& request);
    Json Save() const;
    void Restore(const Json& saved);
};
}
