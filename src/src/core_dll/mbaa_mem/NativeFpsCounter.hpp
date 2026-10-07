#pragma once

namespace cccaster::game_interface::native_fps_counter {
// Exclude repeated simulation from the stock FPS count, retaining elapsed time.
bool Install(bool (*isReplay)());
}
