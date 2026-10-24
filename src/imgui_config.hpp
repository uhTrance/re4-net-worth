// Dear ImGui compile options. These must match REFramework's own build
// (src/re2-imgui/re2_imconfig.hpp), because the window draws into
// REFramework's ImGui context and the structs have to line up exactly.
//
// REFramework also defines IMGUI_ENABLE_FREETYPE. That only selects the font
// loader compiled into imgui_draw.cpp and does not change any struct layout;
// the atlas we add our font to already carries REFramework's FreeType loader.
#pragma once

#define IMGUI_IMPL_WIN32_DISABLE_GAMEPAD
#define IM_ASSERT(_EXPR) ((void)(_EXPR))
#define IMGUI_USE_WCHAR32
#define IMGUI_DISABLE_DEMO_WINDOWS
