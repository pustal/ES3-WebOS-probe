/**
 * @file GlFunctions.cpp
 * @brief Loads the GL entry points declared in GlFunctions.h.
 */
#include "GlFunctions.h"

#include <SDL.h>

namespace probe
{

std::vector<std::string> Gl::Load()
{
    std::vector<std::string> missing;
#define PROBE_GL_LOAD(type, name)                                                       \
    name = reinterpret_cast<type>(SDL_GL_GetProcAddress(#name));                        \
    if (!name)                                                                          \
        missing.emplace_back(#name);
    PROBE_GL_FUNCTIONS(PROBE_GL_LOAD)
#undef PROBE_GL_LOAD
    return missing;
}

Gl &gl()
{
    static Gl instance;
    return instance;
}

} // namespace probe
