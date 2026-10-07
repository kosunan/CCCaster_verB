#pragma once
#include "core_dll/engine/ExtraColorNetwork.hpp"
#include "core_dll/mbaa_mem/GameInput.hpp"

namespace cccaster::training_palette::selection {
bool Install();
void Configure(unsigned mode,bool host);
void Tick();
bool Editable(unsigned side);
bool Visible(unsigned side);
unsigned Character(unsigned side);
int Choice(unsigned side);
bool Available(unsigned side,unsigned extra);
bool Choose(unsigned side,int extra);
void Filter(game_interface::GameInput& p1,game_interface::GameInput& p2);
void Publish(uint32_t epoch,uint32_t revision);
bool Ready(uint32_t epoch,uint32_t localCharacter,uint32_t peerCharacter,uint32_t localRevision,uint32_t peerRevision);
std::shared_ptr<const ExtraColor> Resolve(unsigned side,unsigned character);
const char* Notice();
unsigned MenuColor(unsigned side);
bool NativePage(unsigned side);
int NativeInput(unsigned side,unsigned direction,unsigned buttons);
const ExtraColor* Saved(unsigned side,unsigned extra);
const uint32_t* PreviewPalette(uint32_t* cg,const uint32_t* original);
}
