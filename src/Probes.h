/**
 * @file Probes.h
 * @brief The individual checks the probe runs.
 *
 * Together they answer one question: can this TV run the Animal Crossing PC
 * port's renderer, which needs OpenGL ES 3.0 and a large GameCube "TEV"
 * fragment shader, fast enough at the TV's drawing resolution?
 */
#ifndef ES3PROBE_PROBES_H
#define ES3PROBE_PROBES_H

#include "GlFunctions.h"
#include "Report.h"

#include <SDL.h>

#include <string>
#include <vector>

namespace probe
{

/** What the app asked for and what it got when creating the GL context. */
struct ContextResult {
    int requestedMajor = 0;
    int requestedMinor = 0;
    int gotMajor       = 0; ///< From SDL_GL_GetAttribute after creation.
    int gotMinor       = 0;
    int depthBits      = 0;      ///< Depth buffer size the window got (the game wants 24).
    bool isEs3         = false;  ///< GL_VERSION reports OpenGL ES 3.x or later.
    std::string es3Error;        ///< SDL error if the ES 3 attempt failed.
    std::vector<std::string> missingFunctions;
};

/** Device, OS and display facts (no identifiers such as serial numbers). */
void ProbeSystem(Report &report, SDL_Window *window);

/** GL context, driver strings, limits and extensions. */
void ProbeContext(Report &report, const ContextResult &context);

/** Compiles a small shader that uses the ES 3 features the port relies on. */
void ProbeEs3Features(Report &report);

/**
 * Compiles and links the Animal Crossing port's own shaders (default.vert and
 * default.frag), rewritten to GLSL ES 3.00 exactly as the Android port does.
 *
 * @return The linked program, or 0 if it failed (the reason is in the report).
 */
GLuint ProbeAcgcShaders(Report &report);

/**
 * Measures how long the GPU takes to draw full-screen layers with the port's
 * shader, for a typical scene and a worst case. Uses the specialised variants
 * the port really draws with (uber shader as reference), renders at 1080p,
 * 720p and 480p and scales up to the window. Frames are shown while measuring.
 */
void ProbePerformance(Report &report, GLuint uberProgram, SDL_Window *window);

/**
 * Checks whether compiled programs can be saved and reloaded
 * (glGetProgramBinary / glProgramBinary), so the port could cache its shader
 * variants on disk and compile each one only once.
 */
void ProbeProgramCache(Report &report, GLuint program);

} // namespace probe

#endif // ES3PROBE_PROBES_H
