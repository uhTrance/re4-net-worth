// The in-game window, drawn with REFramework's own ImGui context.
//
// The poller (plugin.cpp) hands every published snapshot to ui_publish(); the
// window draws the latest one each frame. Nothing here calls into the game:
// all game reads stay in the poller, so the window cannot add a game-thread
// hazard of its own.
#pragma once

#include <string>

#include "reframework/API.h"
#include "state.hpp"

namespace re4 {

// Registers the ImGui callbacks and loads the window settings.
bool ui_initialize(const REFrameworkPluginFunctions* fns, const std::string& data_dir);

// A new consistent snapshot to show.
void ui_publish(const OutputState& st);

// True while the poller is holding off (loading, saving, death screen).
void ui_set_paused(bool paused);

// Window settings the writer thread needs for the browser view.
bool ui_browser_export();
bool ui_dark();

} // namespace re4
