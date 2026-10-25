/**
 * @file main.cpp
 * @brief ES3 webOS probe: checks whether an LG webOS TV can run the Animal
 *        Crossing PC port's OpenGL ES 3 renderer, and shows the answer on screen.
 *
 * Steps:
 *   1. Create a full-screen window and ask for an OpenGL ES 3.0 context. If the
 *      TV refuses, fall back to ES 2.0 so the results can still be shown.
 *   2. Run the probes (system, context, shaders, performance).
 *   3. Save the report as text next to the app's data and show it until Back.
 *
 * Keys: Back/Exit/Escape quit, OK/Enter runs the benchmark again.
 *
 * For testing without a TV: PROBE_SCREENSHOT=<file.bmp> saves the results
 * screen to that file and exits.
 */
#include "GlFunctions.h"
#include "Probes.h"
#include "Report.h"
#include "TextRenderer.h"

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace
{

/** Remote control scancodes from the SDL-webOS backport (SDL_SCANCODE_WEBOS_*). */
constexpr int kScancodeWebOSBack = 482;
constexpr int kScancodeWebOSExit = 505;

constexpr probe::Rgba kBackground = { 0.04f, 0.06f, 0.15f, 1.0f };
constexpr probe::Rgba kTitle      = { 1.0f, 0.82f, 0.24f, 1.0f };
constexpr probe::Rgba kText       = { 0.92f, 0.92f, 0.96f, 1.0f };
constexpr probe::Rgba kDim        = { 0.6f, 0.63f, 0.75f, 1.0f };
constexpr probe::Rgba kSection    = { 0.35f, 0.67f, 1.0f, 1.0f };
constexpr probe::Rgba kPass       = { 0.35f, 0.8f, 0.47f, 1.0f };
constexpr probe::Rgba kWarn       = { 1.0f, 0.75f, 0.25f, 1.0f };
constexpr probe::Rgba kFail       = { 1.0f, 0.42f, 0.35f, 1.0f };

/** Where the text report is written. */
std::string ReportPath()
{
    const char *home = std::getenv("HOME");
    return std::string(home && *home ? home : "/tmp") + "/es3-probe-report.txt";
}

/** Window + context, created for one ES version. */
struct GlWindow {
    SDL_Window *window    = nullptr;
    SDL_GLContext context = nullptr;
};

/** Tries to create a full-screen window with an ES @p major.@p minor context and a @p depthBits depth buffer. */
GlWindow CreateGlWindowWithDepth(int major, int minor, int depthBits)
{
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depthBits);

    GlWindow result;
    result.window = SDL_CreateWindow("ES3 webOS probe", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1920, 1080,
                                     SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!result.window)
        return result;

    result.context = SDL_GL_CreateContext(result.window);
    if (!result.context) {
        SDL_DestroyWindow(result.window);
        result.window = nullptr;
    }
    return result;
}

/**
 * Tries to create an ES @p major.@p minor context, preferring the 24-bit depth
 * buffer the game uses but accepting less, so a missing EGL config does not
 * hide the result. @p depthBits receives the depth size that worked.
 */
GlWindow CreateGlWindow(int major, int minor, int *depthBits)
{
    for (const int depth : { 24, 16, 0 }) {
        GlWindow result = CreateGlWindowWithDepth(major, minor, depth);
        if (result.context) {
            *depthBits = depth;
            return result;
        }
    }
    return {};
}

/** Colour for a finding's status tag. */
probe::Rgba StatusColor(probe::Status status)
{
    switch (status) {
        case probe::Status::Pass: return kPass;
        case probe::Status::Warn: return kWarn;
        case probe::Status::Fail: return kFail;
        case probe::Status::Info: return kDim;
    }
    return kDim;
}

/** Draws the report sized to the drawable: one full-width column, or two when it does not fit. */
void DrawReport(probe::TextRenderer &text, const probe::Report &report, int width, int height, const std::string &footer)
{
    text.Begin(width, height);

    const int scale   = std::max(1, height / 540); // 2 at 1080p: 16 px glyphs
    const int glyph   = 8 * scale;
    const int line    = glyph + 4 * scale;
    const int margin  = 12 * scale;

    // Height the report needs in one column (headings take 1.5 lines).
    int needed = 0;
    for (const probe::Finding &f : report.Findings())
        needed += f.label.empty() ? line * 3 / 2 : line;
    const int top     = margin + line * 3;
    const int bottom  = height - margin - line * 2;
    const bool twoColumns = needed > bottom - top;
    const int columnW  = twoColumns ? (width - margin * 3) / 2 : width - margin * 2;
    const int maxChars = columnW / glyph;

    text.Rect(0, 0, width, height, kBackground);
    text.Text(margin, margin, "ES3 webOS probe - can this TV run the Animal Crossing port's renderer?", scale, kTitle);

    const bool passed  = report.AllPassed();
    const std::string verdict = passed ? "VERDICT: looks good - no check failed" : "VERDICT: at least one check failed";
    text.Text(margin, margin + line, verdict, scale, passed ? kPass : kFail);

    int column = 0;
    int y      = top;
    for (const probe::Finding &f : report.Findings()) {
        // Continue in the second column once the first is full.
        if (twoColumns && y + line > bottom && column == 0) {
            column = 1;
            y      = top;
        }
        const int x = margin + column * (columnW + margin);

        if (f.label.empty()) {
            y += line / 2;
            text.Text(x, y, f.value, scale, kSection);
            y += line;
            continue;
        }

        std::string body = f.label + ": " + f.value;
        const std::size_t room = static_cast<std::size_t>(std::max(8, maxChars - 5));
        if (body.size() > room)
            body = body.substr(0, room - 1) + "~";
        if (f.status != probe::Status::Info)
            text.Text(x, y, probe::StatusTag(f.status), scale, StatusColor(f.status));
        text.Text(x + glyph * 5, y, body, scale, f.label == "  |" ? kDim : kText);
        y += line;
    }

    text.Text(margin, height - margin - glyph, footer, scale, kDim);
    text.End();
}

/** Saves the current back buffer as a BMP (for PROBE_SCREENSHOT). @return False on failure. */
bool SaveScreenshot(const char *path, int width, int height)
{
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ABGR8888);
    if (!surface)
        return false;
    probe::gl().glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, surface->pixels);

    // GL rows start at the bottom; BMP rows at the top.
    std::vector<std::uint8_t> row(static_cast<std::size_t>(surface->pitch));
    auto *pixels = static_cast<std::uint8_t *>(surface->pixels);
    for (int y = 0; y < height / 2; ++y) {
        std::uint8_t *top    = pixels + y * surface->pitch;
        std::uint8_t *bottom = pixels + (height - 1 - y) * surface->pitch;
        std::copy(top, top + surface->pitch, row.begin());
        std::copy(bottom, bottom + surface->pitch, top);
        std::copy(row.begin(), row.end(), bottom);
    }
    const bool saved = SDL_SaveBMP(surface, path) == 0;
    SDL_FreeSurface(surface);
    return saved;
}

/** Runs every probe in order. */
probe::Report RunProbes(SDL_Window *window, const probe::ContextResult &context)
{
    probe::Report report;
    probe::ProbeSystem(report, window);
    probe::ProbeContext(report, context);
    if (context.isEs3) {
        probe::ProbeEs3Features(report);
        const GLuint program = probe::ProbeAcgcShaders(report);
        probe::ProbePerformance(report, program, window);
        if (program)
            probe::gl().glDeleteProgram(program);
    }
    else {
        report.Section("Shaders");
        report.Add(probe::Status::Fail, "ES 3 shaders", "skipped: no ES 3 context");
    }
    return report;
}

/** Writes the report to disk and stdout. @return Footer line for the screen. */
std::string SaveReport(const probe::Report &report)
{
    const std::string text = report.ToText();
    std::fputs(text.c_str(), stdout);
    std::fflush(stdout);

    const std::string path = ReportPath();
    std::ofstream out(path, std::ios::trunc);
    out << "ES3 webOS probe report\n" << text;
    const std::string saved = out ? "Report saved to " + path : "Could not save the report";
    return saved + "   |   OK: run again   BACK: quit";
}

} // namespace

int main(int argc, char *argv[])
{
    // webOS passes a JSON launch parameter in argv[1]; the probe has no use for it.
    (void)argc;
    (void)argv;

#ifdef SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_BACK, "true");
#endif
#ifdef SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT
    SDL_SetHint(SDL_HINT_WEBOS_ACCESS_POLICY_KEYS_EXIT, "true");
#endif
#ifdef SDL_HINT_WEBOS_CURSOR_SLEEP_TIME
    SDL_SetHint(SDL_HINT_WEBOS_CURSOR_SLEEP_TIME, "1000");
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    probe::ContextResult context;
    context.requestedMajor = 3;
    context.requestedMinor = 0;
    GlWindow gw            = CreateGlWindow(3, 0, &context.depthBits);
    if (!gw.context) {
        context.es3Error = SDL_GetError();
        gw               = CreateGlWindow(2, 0, &context.depthBits);
    }
    if (!gw.context) {
        std::fprintf(stderr, "No OpenGL ES context at all: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &context.gotMajor);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &context.gotMinor);
    SDL_GL_SetSwapInterval(1);
    SDL_ShowCursor(SDL_DISABLE);

    context.missingFunctions = probe::gl().Load();
    const GLubyte *version   = probe::gl().glGetString ? probe::gl().glGetString(GL_VERSION) : nullptr;
    // "OpenGL ES 3.2 ..." - a driver may also report a higher version than requested.
    const std::string versionText = version ? reinterpret_cast<const char *>(version) : "";
    context.isEs3 = versionText.rfind("OpenGL ES ", 0) == 0 && versionText.size() > 10 && versionText[10] >= '3';

    probe::TextRenderer text;
    if (!text.Init())
        std::fprintf(stderr, "Text renderer failed; results go to stdout and the report file only\n");

    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(gw.window, &width, &height);

    // Show something while the probes run (the benchmark takes a few seconds).
    probe::Report running;
    running.Section("Running checks and a short GPU benchmark...");
    probe::gl().glViewport(0, 0, width, height);
    DrawReport(text, running, width, height, "Please wait");
    SDL_GL_SwapWindow(gw.window);

    probe::Report report = RunProbes(gw.window, context);
    std::string footer   = SaveReport(report);

    if (const char *shot = std::getenv("PROBE_SCREENSHOT")) {
        SDL_GL_GetDrawableSize(gw.window, &width, &height);
        probe::gl().glViewport(0, 0, width, height);
        DrawReport(text, report, width, height, footer);
        const bool saved = SaveScreenshot(shot, width, height);
        text.Release();
        SDL_GL_DeleteContext(gw.context);
        SDL_DestroyWindow(gw.window);
        SDL_Quit();
        return saved ? 0 : 1;
    }

    bool keepRunning = true;
    while (keepRunning) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT)
                keepRunning = false;
            if (event.type == SDL_KEYDOWN && !event.key.repeat) {
                const int sc = static_cast<int>(event.key.keysym.scancode);
                if (sc == kScancodeWebOSBack || sc == kScancodeWebOSExit || sc == SDL_SCANCODE_ESCAPE)
                    keepRunning = false;
                else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                    report = RunProbes(gw.window, context);
                    footer = SaveReport(report);
                }
            }
        }

        SDL_GL_GetDrawableSize(gw.window, &width, &height);
        probe::gl().glViewport(0, 0, width, height);
        DrawReport(text, report, width, height, footer);
        SDL_GL_SwapWindow(gw.window);
    }

    text.Release();
    SDL_GL_DeleteContext(gw.context);
    SDL_DestroyWindow(gw.window);
    SDL_Quit();
    return report.AllPassed() ? 0 : 2;
}
