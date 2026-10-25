/**
 * @file TextRenderer.h
 * @brief Draws the results screen with OpenGL ES: text and filled rectangles.
 *
 * Uses GLSL ES 1.00 and no vertex array objects, so it works in an ES 2
 * context too. That way the probe can still show its findings when the TV
 * refuses an ES 3 context.
 */
#ifndef ES3PROBE_TEXT_RENDERER_H
#define ES3PROBE_TEXT_RENDERER_H

#include "GlFunctions.h"

#include <string>
#include <vector>

namespace probe
{

/** An RGBA colour with components in 0..1. */
struct Rgba {
    float r, g, b, a;
};

class TextRenderer
{
public:
    TextRenderer() = default;
    /** Does not touch GL: the context may already be gone. Call Release() first. */
    ~TextRenderer() = default;

    TextRenderer(const TextRenderer &)            = delete;
    TextRenderer &operator=(const TextRenderer &) = delete;

    /** Builds the glyph atlas and shader. @return False on a GL failure. */
    bool Init();

    /** Frees the GL objects. Must be called while the GL context is still current. */
    void Release();

    /** Starts a frame on a @p width x @p height drawable (pixel coordinates, origin top-left). */
    void Begin(int width, int height);

    /** Draws @p text at (@p x, @p y) with glyphs @p scale times 8 pixels tall. */
    void Text(int x, int y, const std::string &text, int scale, Rgba color);

    /** Fills a rectangle. */
    void Rect(int x, int y, int w, int h, Rgba color);

    /** Sends everything queued since Begin() to the GPU. */
    void End();

    /** Pixel width of @p text at @p scale. */
    static int Measure(const std::string &text, int scale);

private:
    /** One textured or solid vertex: position, atlas coordinate, colour. */
    struct Vertex {
        float x, y, u, v, r, g, b, a;
    };

    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Rgba color);

    GLuint program_ = 0;
    GLuint atlas_   = 0;
    GLuint buffer_  = 0;
    GLint uScreen_  = -1;
    GLint uAtlas_   = -1;
    int width_      = 0;
    int height_     = 0;
    std::vector<Vertex> vertices_;
};

} // namespace probe

#endif // ES3PROBE_TEXT_RENDERER_H
