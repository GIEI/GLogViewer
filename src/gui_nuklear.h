#ifndef LOGVIEW_GUI_NUKLEAR_H
#define LOGVIEW_GUI_NUKLEAR_H

#include <SDL3/SDL.h>

#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_COMMAND_USERDATA
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT

#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "nuklear.h"
#include "nuklear_sdl3_renderer.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#endif
