/**
 * @file TextRenderer.cpp
 * @brief Implementation of TextRenderer (see TextRenderer.h).
 *
 * The atlas holds the 95 glyphs side by side plus one solid white cell at the
 * end; rectangles are drawn as quads sampling that solid cell, so text and
 * boxes share one shader and one draw call per frame.
 */
#include "TextRenderer.h"

#include "FontData.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace probe
{
namespace
{

constexpr int kAtlasCells  = kGlyphCount + 1; // glyphs + solid cell
constexpr int kAtlasWidth  = kAtlasCells * kGlyphSize;
constexpr int kAtlasHeight = kGlyphSize;
constexpr int kSolidCell   = kGlyphCount;

constexpr const char *kVertexShader = R"(#version 100
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute vec4 a_color;
uniform vec2 u_screen;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    // Pixel coordinates (origin top-left) to clip space.
    vec2 ndc = vec2(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0);
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_uv = a_uv;
    v_color = a_color;
})";

constexpr const char *kFragmentShader = R"(#version 100
precision mediump float;
uniform sampler2D u_atlas;
varying vec2 v_uv;
varying vec4 v_color;
void main() {
    // Premultiplied alpha: colour times its own alpha, times the glyph coverage.
    vec4 c = vec4(v_color.rgb * v_color.a, v_color.a);
    gl_FragColor = c * texture2D(u_atlas, v_uv).a;
})";

/** Compiles one stage. @return 0 on failure. */
GLuint Compile(GLenum type, const char *source)
{
    Gl &g         = gl();
    GLuint shader = g.glCreateShader(type);
    g.glShaderSource(shader, 1, &source, nullptr);
    g.glCompileShader(shader);
    GLint ok = GL_FALSE;
    g.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        g.glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

void TextRenderer::Release()
{
    Gl &g = gl();
    if (program_ && g.glDeleteProgram)
        g.glDeleteProgram(program_);
    if (atlas_ && g.glDeleteTextures)
        g.glDeleteTextures(1, &atlas_);
    if (buffer_ && g.glDeleteBuffers)
        g.glDeleteBuffers(1, &buffer_);
    program_ = atlas_ = buffer_ = 0;
}

bool TextRenderer::Init()
{
    Gl &g = gl();

    const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = Compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs)
        return false;

    program_ = g.glCreateProgram();
    g.glAttachShader(program_, vs);
    g.glAttachShader(program_, fs);
    g.glBindAttribLocation(program_, 0, "a_pos");
    g.glBindAttribLocation(program_, 1, "a_uv");
    g.glBindAttribLocation(program_, 2, "a_color");
    g.glLinkProgram(program_);
    g.glDeleteShader(vs);
    g.glDeleteShader(fs);
    GLint linked = GL_FALSE;
    g.glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE)
        return false;
    uScreen_ = g.glGetUniformLocation(program_, "u_screen");
    uAtlas_  = g.glGetUniformLocation(program_, "u_atlas");

    // White glyph pixels with alpha coverage; colour comes from the vertices.
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kAtlasWidth * kAtlasHeight * 4), 0);
    for (int cell = 0; cell < kAtlasCells; ++cell) {
        for (int row = 0; row < kGlyphSize; ++row) {
            const std::uint8_t bits =
                cell == kSolidCell ? 0xFF : kGlyphs[static_cast<std::size_t>(cell)][static_cast<std::size_t>(row)];
            for (int col = 0; col < kGlyphSize; ++col) {
                if (bits & (1u << col)) {
                    const std::size_t at = static_cast<std::size_t>((row * kAtlasWidth + cell * kGlyphSize + col) * 4);
                    pixels[at] = pixels[at + 1] = pixels[at + 2] = pixels[at + 3] = 0xFF;
                }
            }
        }
    }

    g.glGenTextures(1, &atlas_);
    g.glBindTexture(GL_TEXTURE_2D, atlas_);
    g.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    g.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kAtlasWidth, kAtlasHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    // Nearest filtering and edge clamping: crisp pixels, and valid for a non-power-of-two texture in ES 2.
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    g.glGenBuffers(1, &buffer_);
    return g.glGetError() == GL_NO_ERROR;
}

void TextRenderer::Begin(int width, int height)
{
    width_  = width;
    height_ = height;
    vertices_.clear();
}

void TextRenderer::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Rgba c)
{
    const Vertex tl{ x0, y0, u0, v0, c.r, c.g, c.b, c.a };
    const Vertex tr{ x1, y0, u1, v0, c.r, c.g, c.b, c.a };
    const Vertex bl{ x0, y1, u0, v1, c.r, c.g, c.b, c.a };
    const Vertex br{ x1, y1, u1, v1, c.r, c.g, c.b, c.a };
    vertices_.insert(vertices_.end(), { tl, tr, bl, tr, br, bl });
}

void TextRenderer::Text(int x, int y, const std::string &text, int scale, Rgba color)
{
    const float size = static_cast<float>(kGlyphSize * scale);
    float penX       = static_cast<float>(x);
    for (const char raw : text) {
        int index = static_cast<unsigned char>(raw) - static_cast<unsigned char>(kFirstGlyph);
        if (index < 0 || index >= kGlyphCount)
            index = '?' - kFirstGlyph;
        const float u0 = static_cast<float>(index * kGlyphSize) / kAtlasWidth;
        const float u1 = static_cast<float>((index + 1) * kGlyphSize) / kAtlasWidth;
        Quad(penX, static_cast<float>(y), penX + size, static_cast<float>(y) + size, u0, 0.0f, u1, 1.0f, color);
        penX += size;
    }
}

void TextRenderer::Rect(int x, int y, int w, int h, Rgba color)
{
    // Sample the middle of the solid cell so filtering never reaches a glyph.
    const float u = (static_cast<float>(kSolidCell * kGlyphSize) + 4.0f) / kAtlasWidth;
    Quad(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + w), static_cast<float>(y + h), u, 0.5f, u, 0.5f,
         color);
}

void TextRenderer::End()
{
    if (vertices_.empty() || !program_)
        return;

    Gl &g = gl();
    g.glUseProgram(program_);
    g.glUniform2fv(uScreen_, 1, std::array<float, 2>{ static_cast<float>(width_), static_cast<float>(height_) }.data());
    g.glUniform1i(uAtlas_, 0);
    g.glActiveTexture(GL_TEXTURE0);
    g.glBindTexture(GL_TEXTURE_2D, atlas_);

    g.glBindBuffer(GL_ARRAY_BUFFER, buffer_);
    g.glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices_.size() * sizeof(Vertex)), vertices_.data(), GL_STREAM_DRAW);
    const GLsizei stride = sizeof(Vertex);
    g.glEnableVertexAttribArray(0);
    g.glEnableVertexAttribArray(1);
    g.glEnableVertexAttribArray(2);
    g.glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(offsetof(Vertex, x)));
    g.glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(offsetof(Vertex, u)));
    g.glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(offsetof(Vertex, r)));

    g.glEnable(GL_BLEND);
    g.glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // the shader outputs premultiplied alpha
    g.glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size()));
    g.glDisable(GL_BLEND);

    g.glDisableVertexAttribArray(0);
    g.glDisableVertexAttribArray(1);
    g.glDisableVertexAttribArray(2);
    g.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

int TextRenderer::Measure(const std::string &text, int scale) { return static_cast<int>(text.size()) * kGlyphSize * scale; }

} // namespace probe
