/**
 * @file Probes.cpp
 * @brief Implementation of the checks declared in Probes.h.
 */
#include "Probes.h"

// The Animal Crossing PC port's shaders, embedded at build time (third_party/acgc-shaders).
#include "AcgcFragmentShader.generated.h"
#include "AcgcVertexShader.generated.h"

#include <sys/statvfs.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace probe
{
namespace
{

/** Frame budget at 60 fps, in milliseconds. */
constexpr double kFrameBudget60Ms = 1000.0 / 60.0;
/** Frame budget at 30 fps (Animal Crossing's own frame rate on the GameCube). */
constexpr double kFrameBudget30Ms = 1000.0 / 30.0;
/** How long each benchmark scene is measured (after warm-up). */
constexpr Uint32 kBenchmarkMs = 1500;
/** Frames drawn before measuring, so shader compilation and caches settle. */
constexpr int kWarmupFrames = 5;
/** Measured frames wanted even when the GPU is slow, so the average means something. */
constexpr std::size_t kMinMeasuredFrames = 5;
/** Hard limit per scene, so a very slow GPU cannot keep the probe busy for minutes. */
constexpr Uint32 kSceneTimeoutMs = 8000;
/** Size of the generated test textures. */
constexpr int kTextureSize = 256;

std::string Format(const char *fmt, double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), fmt, value);
    return buffer;
}

std::string GlString(GLenum name)
{
    const GLubyte *value = gl().glGetString ? gl().glGetString(name) : nullptr;
    return value ? reinterpret_cast<const char *>(value) : "(none)";
}

GLint GlInt(GLenum name)
{
    GLint value = 0;
    gl().glGetIntegerv(name, &value);
    gl().glGetError(); // An unsupported query must not leave an error behind for later checks.
    return value;
}

/** Reads a small text file whole. @return Empty on failure. */
std::string ReadFile(const char *path)
{
    std::ifstream in(path);
    std::ostringstream content;
    content << in.rdbuf();
    return content.str();
}

/** Extracts "key": "value" from a flat JSON text without a JSON parser. */
std::string JsonString(const std::string &json, const std::string &key)
{
    const std::size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos)
        return {};
    const std::size_t open = json.find('"', json.find(':', at) + 1);
    const std::size_t close = open == std::string::npos ? open : json.find('"', open + 1);
    if (open == std::string::npos || close == std::string::npos)
        return {};
    return json.substr(open + 1, close - open - 1);
}

/**
 * Compiles one shader stage.
 *
 * @param log Receives the compiler log (may be non-empty on success: warnings).
 * @return The shader, or 0 on failure.
 */
GLuint CompileShader(GLenum type, const std::string &source, std::string *log)
{
    Gl &g             = gl();
    GLuint shader     = g.glCreateShader(type);
    const char *text  = source.c_str();
    g.glShaderSource(shader, 1, &text, nullptr);
    g.glCompileShader(shader);

    GLint length = 0;
    g.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    if (length > 1) {
        std::string buffer(static_cast<std::size_t>(length), '\0');
        g.glGetShaderInfoLog(shader, length, nullptr, &buffer[0]);
        *log = buffer.c_str();
    }

    GLint ok = GL_FALSE;
    g.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        g.glDeleteShader(shader);
        return 0;
    }
    return shader;
}

/** Links two compiled stages. @return The program, or 0 on failure (log filled). */
GLuint LinkProgram(GLuint vs, GLuint fs, std::string *log)
{
    Gl &g          = gl();
    GLuint program = g.glCreateProgram();
    g.glAttachShader(program, vs);
    g.glAttachShader(program, fs);
    g.glLinkProgram(program);

    GLint length = 0;
    g.glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    if (length > 1) {
        std::string buffer(static_cast<std::size_t>(length), '\0');
        g.glGetProgramInfoLog(program, length, nullptr, &buffer[0]);
        *log = buffer.c_str();
    }

    GLint ok = GL_FALSE;
    g.glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        g.glDeleteProgram(program);
        return 0;
    }
    return program;
}

/**
 * Rewrites desktop GLSL 3.30 to GLSL ES 3.00 the way the Android port does
 * (app/src/main/cpp/android/android_glue.c): replace the #version line and
 * add the precision qualifiers ES requires.
 */
std::string RewriteToEs(const std::string &source)
{
    const std::size_t newline = source.find('\n');
    const std::string body = (source.rfind("#version", 0) == 0 && newline != std::string::npos) ? source.substr(newline + 1)
                                                                                                 : source;
    return "#version 300 es\n"
           "precision highp float;\n"
           "precision highp int;\n"
           "precision highp sampler2D;\n" +
           body;
}

/** Creates a test texture: a coloured checkerboard with an alpha gradient. */
GLuint MakeTexture(int seed)
{
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kTextureSize * kTextureSize * 4));
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const std::size_t i = static_cast<std::size_t>((y * kTextureSize + x) * 4);
            const bool check    = ((x / 16) + (y / 16) + seed) % 2 == 0;
            pixels[i + 0]       = static_cast<std::uint8_t>(check ? 230 : 40 + seed * 60);
            pixels[i + 1]       = static_cast<std::uint8_t>(check ? 120 + seed * 40 : 90);
            pixels[i + 2]       = static_cast<std::uint8_t>(check ? 60 : 200);
            pixels[i + 3]       = static_cast<std::uint8_t>(x); // alpha ramp exercises the alpha test
        }
    }

    Gl &g          = gl();
    GLuint texture = 0;
    g.glGenTextures(1, &texture);
    g.glBindTexture(GL_TEXTURE_2D, texture);
    g.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, kTextureSize, kTextureSize, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    return texture;
}

/** One benchmark scene: how much of the TEV pipeline each full-screen layer uses. */
struct Scene {
    const char *name;
    int stages;       ///< TEV stages (1..3)
    int textures;     ///< Textures sampled (0..3)
    bool lights;      ///< Per-pixel lighting with all 8 lights
    bool indirect;    ///< Indirect texturing on stage 1
    int layers;       ///< Full-screen layers per frame (overdraw)
    double passMs;    ///< At or under: PASS
    double warnMs;    ///< At or under: WARN; above: FAIL
};

/**
 * "Typical" approximates a busy outdoor frame: two-stage textured materials,
 * fog and alpha-tested foliage, about three layers of overdraw. "Worst case"
 * turns every feature of the shader on at once, which no real frame does.
 */
constexpr std::array<Scene, 2> kScenes = { {
    { "Typical scene", 2, 1, false, false, 3, kFrameBudget60Ms * 0.6, kFrameBudget30Ms * 0.6 },
    { "Worst case", 3, 3, true, true, 4, kFrameBudget30Ms * 0.6, kFrameBudget30Ms * 1.5 },
} };

/**
 * The values of the "folded" uniforms for a scene: the ones the port turns
 * into constants when it builds a specialised shader variant (see
 * s_folded_uniforms and emit_const_block in the port's pc_gx_tev.c). Used both
 * to upload them as uniforms (uber shader) and to emit them as constants
 * (variant), so the two programs always draw the same thing.
 */
struct TevSetup {
    int numStages = 0;
    std::array<GLint, 12> colorIn{}, alphaIn{}, bsc{}, out{}, indCfg{};
    std::array<GLint, 9> indWrap{}, ksel{};
    std::array<GLint, 6> swap{};
    std::array<GLint, 3> colorOp{}, alphaOp{}, tcSrc{}, useTexture{};
    std::array<GLint, 4> lighting0{}, lighting1{};
    std::array<GLint, 16> swapTables = { 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 };
    std::array<GLint, 3> alphaCtrl = { 4, 0, 7 }; // GREATER than ref0, AND, ALWAYS: foliage cut-outs
    int numIndStages = 0;
    int fogEnable    = 1;
};

/** Builds the TEV configuration for @p scene. */
TevSetup MakeTevSetup(const Scene &scene)
{
    TevSetup t;
    t.numStages = scene.stages;
    // Per-stage TEV setup: d + mix(a, b, c) with a = ZERO, b = TEXC, c = RASC, d = CPREV (ZERO on stage 0).
    auto &colorIn = t.colorIn;
    auto &alphaIn = t.alphaIn;
    auto &out     = t.out;
    auto &indCfg  = t.indCfg;
    auto &tcSrc   = t.tcSrc;
    auto &useTexture = t.useTexture;
    auto &ksel    = t.ksel;
    for (int s = 0; s < 3; ++s) {
        const std::size_t i4 = static_cast<std::size_t>(s) * 4;
        colorIn[i4 + 0] = 15;
        colorIn[i4 + 1] = 8;
        colorIn[i4 + 2] = 10;
        colorIn[i4 + 3] = s == 0 ? 15 : 0;
        alphaIn[i4 + 0] = 7;
        alphaIn[i4 + 1] = 4;
        alphaIn[i4 + 2] = 5;
        alphaIn[i4 + 3] = s == 0 ? 7 : 0;
        out[i4 + 0] = 1; // clamp colour
        out[i4 + 1] = 1; // clamp alpha
        tcSrc[static_cast<std::size_t>(s)]      = s % 2;
        useTexture[static_cast<std::size_t>(s)] = s < scene.textures ? 1 : 0;
        ksel[static_cast<std::size_t>(s) * 3 + 0] = 12; // K0 RGB
        ksel[static_cast<std::size_t>(s) * 3 + 1] = 16; // K0 R
        if (scene.indirect && s == 1) {
            indCfg[i4 + 0] = 0; // indirect stage 0
            indCfg[i4 + 1] = 1; // matrix 0 (full 2D)
            indCfg[i4 + 2] = 3; // bias S and T
        }
    }
    // Colour channel: vertex colour as material; per-pixel lighting with all 8 lights in the worst case.
    t.lighting0    = { scene.lights ? 1 : 0, 1, 0, 1 };
    t.lighting1    = { 0, 1, scene.lights ? 0xFF : 0, 0 };
    t.numIndStages = scene.indirect ? 1 : 0;
    return t;
}

/** GLSL list of @p count ivecN values taken from @p values, e.g. "ivec4(1,2,3,4),ivec4(...)". */
template <std::size_t N>
std::string IvecList(const std::array<GLint, N> &values, int width, int count)
{
    std::string list;
    for (int i = 0; i < count; ++i) {
        list += (i ? "," : "") + std::string(width == 1 ? "" : "ivec" + std::to_string(width)) + (width == 1 ? "" : "(");
        for (int c = 0; c < width; ++c)
            list += (c ? "," : "") + std::to_string(values[static_cast<std::size_t>(i * width + c)]);
        list += width == 1 ? "" : ")";
    }
    return list;
}

/**
 * The constant block the port prepends to a specialised fragment shader
 * (same names, types and order as emit_const_block in pc_gx_tev.c).
 */
std::string VariantConstants(const TevSetup &t)
{
    std::ostringstream c;
    c << "const int u_num_tev_stages = " << t.numStages << ";\n";
    c << "const ivec4 u_tev_color_in[3] = ivec4[3](" << IvecList(t.colorIn, 4, 3) << ");\n";
    c << "const ivec4 u_tev_alpha_in[3] = ivec4[3](" << IvecList(t.alphaIn, 4, 3) << ");\n";
    c << "const int u_tev_color_op[3] = int[3](" << IvecList(t.colorOp, 1, 3) << ");\n";
    c << "const int u_tev_alpha_op[3] = int[3](" << IvecList(t.alphaOp, 1, 3) << ");\n";
    c << "const int u_tev_tc_src[3] = int[3](" << IvecList(t.tcSrc, 1, 3) << ");\n";
    c << "const ivec4 u_tev_bsc[3] = ivec4[3](" << IvecList(t.bsc, 4, 3) << ");\n";
    c << "const ivec4 u_tev_out[3] = ivec4[3](" << IvecList(t.out, 4, 3) << ");\n";
    c << "const ivec2 u_tev_swap[3] = ivec2[3](" << IvecList(t.swap, 2, 3) << ");\n";
    c << "const ivec3 u_tev_ksel[3] = ivec3[3](" << IvecList(t.ksel, 3, 3) << ");\n";
    c << "const ivec4 u_tev_ind_cfg[3] = ivec4[3](" << IvecList(t.indCfg, 4, 3) << ");\n";
    c << "const ivec3 u_tev_ind_wrap[3] = ivec3[3](" << IvecList(t.indWrap, 3, 3) << ");\n";
    c << "const int u_use_texture[3] = int[3](" << IvecList(t.useTexture, 1, 3) << ");\n";
    c << "const ivec4 u_lighting_cfg0 = " << IvecList(t.lighting0, 4, 1) << ";\n";
    c << "const ivec4 u_lighting_cfg1 = " << IvecList(t.lighting1, 4, 1) << ";\n";
    c << "const ivec4 u_swap_table[4] = ivec4[4](" << IvecList(t.swapTables, 4, 4) << ");\n";
    c << "const ivec3 u_alpha_ctrl = " << IvecList(t.alphaCtrl, 3, 1) << ";\n";
    c << "const int u_num_ind_stages = " << t.numIndStages << ";\n";
    c << "const int u_fog_enable = " << t.fogEnable << ";\n";
    return c.str();
}

/** Uploads the uniforms for @p scene to the port's shader (uniforms a variant folded away are simply absent). */
void ConfigureScene(GLuint program, const Scene &scene)
{
    Gl &g          = gl();
    const auto loc = [&](const char *name) { return g.glGetUniformLocation(program, name); };

    const std::array<float, 16> identity4 = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    const std::array<float, 9> identity3  = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    g.glUniformMatrix4fv(loc("u_projection"), 1, GL_FALSE, identity4.data());
    g.glUniformMatrix4fv(loc("u_modelview"), 1, GL_FALSE, identity4.data());
    g.glUniformMatrix3fv(loc("u_normal_mtx"), 1, GL_FALSE, identity3.data());
    const std::array<GLint, 2> zero2i     = { 0, 0 };
    const std::array<GLint, 2> texgenTex0 = { 4, 4 }; // GX_TG_TEX0 for both coordinates
    g.glUniform1iv(loc("u_texmtx_enable"), 2, zero2i.data());
    g.glUniform1iv(loc("u_texgen_src"), 2, texgenTex0.data());

    const TevSetup t = MakeTevSetup(scene);
    const auto &colorIn = t.colorIn, &alphaIn = t.alphaIn, &bsc = t.bsc, &out = t.out, &indCfg = t.indCfg;
    const auto &indWrap = t.indWrap, &ksel = t.ksel;
    const auto &swap    = t.swap;
    const auto &colorOp = t.colorOp, &alphaOp = t.alphaOp, &tcSrc = t.tcSrc, &useTexture = t.useTexture;
    g.glUniform1i(loc("u_num_tev_stages"), scene.stages);
    g.glUniform4iv(loc("u_tev_color_in"), 3, colorIn.data());
    g.glUniform4iv(loc("u_tev_alpha_in"), 3, alphaIn.data());
    g.glUniform1iv(loc("u_tev_color_op"), 3, colorOp.data());
    g.glUniform1iv(loc("u_tev_alpha_op"), 3, alphaOp.data());
    g.glUniform1iv(loc("u_tev_tc_src"), 3, tcSrc.data());
    g.glUniform4iv(loc("u_tev_bsc"), 3, bsc.data());
    g.glUniform4iv(loc("u_tev_out"), 3, out.data());
    g.glUniform2iv(loc("u_tev_swap"), 3, swap.data());
    g.glUniform4iv(loc("u_tev_ind_cfg"), 3, indCfg.data());
    g.glUniform3iv(loc("u_tev_ind_wrap"), 3, indWrap.data());
    g.glUniform3iv(loc("u_tev_ksel"), 3, ksel.data());
    g.glUniform1iv(loc("u_use_texture"), 3, useTexture.data());

    g.glUniform4iv(loc("u_swap_table"), 4, t.swapTables.data());

    const std::array<float, 4> half = { 0.5f, 0.5f, 0.5f, 1.0f };
    g.glUniform4fv(loc("u_tev_prev"), 1, half.data());
    g.glUniform4fv(loc("u_tev_reg0"), 1, half.data());
    g.glUniform4fv(loc("u_tev_reg1"), 1, half.data());
    g.glUniform4fv(loc("u_tev_reg2"), 1, half.data());
    const std::array<float, 16> kcolors = { 1, 0.8f, 0.6f, 1, 0.5f, 0.5f, 0.5f, 1, 0.2f, 0.4f, 0.8f, 1, 1, 1, 1, 1 };
    g.glUniform4fv(loc("u_kcolor"), 4, kcolors.data());

    g.glUniform4iv(loc("u_lighting_cfg0"), 1, t.lighting0.data());
    g.glUniform4iv(loc("u_lighting_cfg1"), 1, t.lighting1.data());
    const std::array<float, 8> chan = { 1, 1, 1, 1, 0.3f, 0.3f, 0.3f, 1 };
    g.glUniform4fv(loc("u_chan_color"), 2, chan.data());
    std::array<float, 24> lightPos{};
    std::array<float, 32> lightColor{};
    for (std::size_t i = 0; i < 8; ++i) {
        lightPos[i * 3 + 0] = std::cos(static_cast<float>(i));
        lightPos[i * 3 + 1] = std::sin(static_cast<float>(i));
        lightPos[i * 3 + 2] = 1.0f;
        lightColor[i * 4 + 0] = lightColor[i * 4 + 1] = lightColor[i * 4 + 2] = 0.1f;
        lightColor[i * 4 + 3] = 1.0f;
    }
    g.glUniform3fv(loc("u_light_pos"), 8, lightPos.data());
    g.glUniform4fv(loc("u_light_color"), 8, lightColor.data());

    // Alpha test (GREATER than 8/255, as foliage cut-outs do) and fog: both on in every scene.

    const std::array<GLint, 2> alphaRefs = { 8, 0 };
    g.glUniform3iv(loc("u_alpha_ctrl"), 1, t.alphaCtrl.data());
    g.glUniform2iv(loc("u_alpha_refs"), 1, alphaRefs.data());
    g.glUniform1i(loc("u_fog_enable"), t.fogEnable);
    const std::array<float, 4> fogParams = { 0.0f, -1.0f, 1.0f, 0.0f };
    const std::array<float, 4> fogColor  = { 0.6f, 0.75f, 1.0f, 1.0f };
    g.glUniform4fv(loc("u_fog_params"), 1, fogParams.data());
    g.glUniform4fv(loc("u_fog_color"), 1, fogColor.data());

    // Indirect texturing.
    g.glUniform1i(loc("u_num_ind_stages"), t.numIndStages);
    const std::array<float, 8> indScale = { 1, 1, 1, 1, 1, 1, 1, 1 };
    const std::array<float, 9> indRow0  = { 0.05f, 0, 0, 0.05f, 0, 0, 0.05f, 0, 0 };
    const std::array<float, 9> indRow1  = { 0, 0.05f, 0, 0, 0.05f, 0, 0, 0.05f, 0 };
    g.glUniform2fv(loc("u_ind_scale"), 4, indScale.data());
    g.glUniform3fv(loc("u_ind_mtx_r0"), 3, indRow0.data());
    g.glUniform3fv(loc("u_ind_mtx_r1"), 3, indRow1.data());

    // Texture units: 0-2 regular, 3-4 indirect.
    g.glUniform1i(loc("u_texture0"), 0);
    g.glUniform1i(loc("u_texture1"), 1);
    g.glUniform1i(loc("u_texture2"), 2);
    g.glUniform1i(loc("u_ind_tex0"), 3);
    g.glUniform1i(loc("u_ind_tex1"), 4);
}

/** Average and 95th percentile of @p samples. */
void Summarise(std::vector<double> samples, double *average, double *p95)
{
    if (samples.empty()) {
        *average = *p95 = 0.0;
        return;
    }
    double sum = 0.0;
    for (const double s : samples)
        sum += s;
    *average = sum / static_cast<double>(samples.size());
    std::sort(samples.begin(), samples.end());
    *p95 = samples[std::min(samples.size() - 1, samples.size() * 95 / 100)];
}

} // namespace

void ProbeSystem(Report &report, SDL_Window *window)
{
    report.Section("System");

    const std::string osInfo = ReadFile("/var/run/nyx/os_info.json");
    const std::string webos  = JsonString(osInfo, "webos_release");
    report.Add(Status::Info, "webOS", webos.empty() ? "unknown (not a TV?)" : webos);

    SDL_version linked{};
    SDL_GetVersion(&linked);
    const char *driver = SDL_GetCurrentVideoDriver();
    report.Add(Status::Info, "SDL", std::to_string(linked.major) + "." + std::to_string(linked.minor) + "." +
                                        std::to_string(linked.patch) + " (" + SDL_GetRevision() + "), video " +
                                        (driver ? driver : "?"));

    report.Add(Status::Info, "CPU", std::to_string(SDL_GetCPUCount()) + " cores" + (SDL_HasNEON() ? ", NEON" : ""));
    report.Add(Status::Info, "RAM", std::to_string(SDL_GetSystemRAM()) + " MB");

    int drawW = 0, drawH = 0;
    SDL_GL_GetDrawableSize(window, &drawW, &drawH);
    std::string display = "drawable " + std::to_string(drawW) + "x" + std::to_string(drawH);
#if defined(__WEBOS__)
    int panelW = 0, panelH = 0, refresh = 0;
    if (SDL_webOSGetPanelResolution(&panelW, &panelH))
        display += ", panel " + std::to_string(panelW) + "x" + std::to_string(panelH);
    if (SDL_webOSGetRefreshRate(&refresh))
        display += ", " + std::to_string(refresh) + " Hz";
#endif
    report.Add(Status::Info, "Display", display);

    if (const char *home = std::getenv("HOME")) {
        struct statvfs fs {};
        if (statvfs(home, &fs) == 0) {
            const double freeMb = static_cast<double>(fs.f_bavail) * static_cast<double>(fs.f_frsize) / (1024.0 * 1024.0);
            // A full Animal Crossing disc image is about 1.4 GB.
            const Status status = freeMb >= 1600.0 ? Status::Pass : Status::Warn;
            report.Add(status, "Free storage", Format("%.0f MB", freeMb) + " (a full disc image needs ~1.4 GB)");
        }
    }
}

void ProbeContext(Report &report, const ContextResult &context)
{
    report.Section("OpenGL ES context");

    if (!context.es3Error.empty())
        report.Add(Status::Fail, "ES 3.0 context", "refused: " + context.es3Error);
    report.Add(context.isEs3 ? Status::Pass : Status::Fail, "Context",
               "asked " + std::to_string(context.requestedMajor) + "." + std::to_string(context.requestedMinor) + ", got " +
                   std::to_string(context.gotMajor) + "." + std::to_string(context.gotMinor));
    report.Add(context.depthBits >= 24 ? Status::Pass : Status::Warn, "Depth buffer",
               std::to_string(context.depthBits) + "-bit (the game uses 24)");
    report.Add(Status::Info, "GL_VENDOR", GlString(GL_VENDOR));
    report.Add(Status::Info, "GL_RENDERER", GlString(GL_RENDERER));
    report.Add(context.isEs3 ? Status::Pass : Status::Fail, "GL_VERSION", GlString(GL_VERSION));
    report.Add(Status::Info, "GLSL", GlString(GL_SHADING_LANGUAGE_VERSION));

    if (!context.missingFunctions.empty()) {
        std::string names;
        for (std::size_t i = 0; i < context.missingFunctions.size() && i < 6; ++i)
            names += (i ? ", " : "") + context.missingFunctions[i];
        report.Add(Status::Fail, "Missing GL functions", std::to_string(context.missingFunctions.size()) + ": " + names);
    }

    // Limits the port's shader depends on: it declares about 120 fragment uniform vectors.
    const GLint fragUniforms = GlInt(GL_MAX_FRAGMENT_UNIFORM_VECTORS);
    report.Add(fragUniforms >= 128 ? Status::Pass : Status::Warn, "Fragment uniform vectors", std::to_string(fragUniforms));
    report.Add(Status::Info, "Vertex uniform vectors", std::to_string(GlInt(GL_MAX_VERTEX_UNIFORM_VECTORS)));
    const GLint textureUnits = GlInt(GL_MAX_TEXTURE_IMAGE_UNITS);
    report.Add(textureUnits >= 5 ? Status::Pass : Status::Fail, "Texture units", std::to_string(textureUnits) + " (needs 5)");
    report.Add(Status::Info, "Max texture size", std::to_string(GlInt(GL_MAX_TEXTURE_SIZE)));

    GLint range[2] = { 0, 0 };
    GLint precision = 0;
    if (gl().glGetShaderPrecisionFormat) {
        gl().glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
        report.Add(precision > 0 ? Status::Pass : Status::Fail, "Fragment highp float",
                   precision > 0 ? std::to_string(precision) + "-bit precision" : "not supported");
    }

    // Extensions: count, plus the texture formats relevant to an HD texture pack.
    GLint count = 0;
    std::string all;
    if (context.isEs3 && gl().glGetStringi) {
        count = GlInt(GL_NUM_EXTENSIONS);
        for (GLint i = 0; i < count; ++i) {
            const GLubyte *name = gl().glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            if (name)
                all += std::string(reinterpret_cast<const char *>(name)) + " ";
        }
    } else {
        all = GlString(GL_EXTENSIONS);
        count = static_cast<GLint>(std::count(all.begin(), all.end(), ' '));
    }
    const auto has = [&all](const char *name) { return all.find(name) != std::string::npos; };
    report.Add(Status::Info, "Extensions", std::to_string(count) + " (ASTC " + (has("texture_compression_astc_ldr") ? "yes" : "no") +
                                               ", S3TC " + (has("texture_compression_s3tc") ? "yes" : "no") + ")");
}

void ProbeEs3Features(Report &report)
{
    report.Section("Shaders");

    // Integer vector uniform arrays, switch, bit operations, texture() and out variables:
    // the GLSL ES 3.00 features the port's TEV shader is built from.
    static const char *kVertex = R"(#version 300 es
layout(location = 0) in vec3 a_position;
out vec2 v_uv;
void main() { v_uv = a_position.xy * 0.5 + 0.5; gl_Position = vec4(a_position, 1.0); })";
    static const char *kFragment = R"(#version 300 es
precision highp float;
precision highp int;
uniform ivec4 u_cfg[3];
uniform sampler2D u_tex;
in vec2 v_uv;
out vec4 fragColor;
void main() {
    vec4 c = texture(u_tex, v_uv);
    for (int s = 0; s < 3; s++) {
        switch (u_cfg[s].x & 3) {
            case 1: c.rgb *= 2.0; break;
            case 2: c.rgb += vec3(float(u_cfg[s].y >> 1) / 8.0); break;
            default: break;
        }
    }
    fragColor = c;
})";

    std::string vsLog, fsLog, linkLog;
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVertex, &vsLog);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFragment, &fsLog);
    GLuint program  = (vs && fs) ? LinkProgram(vs, fs, &linkLog) : 0;
    report.Add(program ? Status::Pass : Status::Fail, "GLSL ES 3.00 features", program ? "compiled and linked" : "failed");
    if (!program)
        report.Detail(fsLog + vsLog + linkLog);

    Gl &g = gl();
    if (vs)
        g.glDeleteShader(vs);
    if (fs)
        g.glDeleteShader(fs);
    if (program)
        g.glDeleteProgram(program);
}

GLuint ProbeAcgcShaders(Report &report)
{
    const std::string vertex   = RewriteToEs(kAcgcVertexShader);
    const std::string fragment = RewriteToEs(kAcgcFragmentShader);

    const Uint64 start = SDL_GetPerformanceCounter();
    std::string vsLog, fsLog, linkLog;
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex, &vsLog);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment, &fsLog);
    GLuint program  = (vs && fs) ? LinkProgram(vs, fs, &linkLog) : 0;
    const double ms = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 /
                      static_cast<double>(SDL_GetPerformanceFrequency());

    Gl &g = gl();
    if (vs)
        g.glDeleteShader(vs);
    if (fs)
        g.glDeleteShader(fs);

    if (!vs)
        report.Add(Status::Fail, "Port vertex shader", "does not compile");
    if (!fs)
        report.Add(Status::Fail, "Port TEV fragment shader", "does not compile");
    if (vs && fs && !program)
        report.Add(Status::Fail, "Port shaders", "compile but do not link");
    if (!program) {
        report.Detail(fsLog + vsLog + linkLog);
        return 0;
    }

    // The port compiles specialised variants at run time; slow compiles show up as hitches.
    const Status compileStatus = ms <= 250.0 ? Status::Pass : Status::Warn;
    report.Add(compileStatus, "Port TEV shader (uber)", "compiled and linked in " + Format("%.0f ms", ms));
    return program;
}

namespace
{

/** Internal render resolutions tried, each scaled up to the window like a TV upscales. */
struct RenderSize {
    const char *label;
    int width;
    int height;
};
constexpr std::array<RenderSize, 3> kRenderSizes = { {
    { "1080p", 1920, 1080 },
    { "720p", 1280, 720 },
    { "480p", 854, 480 }, // the GameCube's 480 lines, widened to 16:9 as the port's hor+ widescreen does
} };

/** Uniforms the port folds into constants for a variant (s_folded_uniforms in pc_gx_tev.c). */
constexpr std::array<const char *, 19> kFoldedUniforms = {
    "u_num_tev_stages", "u_tev_color_in", "u_tev_alpha_in", "u_tev_color_op", "u_tev_alpha_op",
    "u_tev_tc_src",     "u_tev_bsc",      "u_tev_out",      "u_tev_swap",     "u_tev_ksel",
    "u_tev_ind_cfg",    "u_tev_ind_wrap", "u_use_texture",  "u_lighting_cfg0", "u_lighting_cfg1",
    "u_swap_table",     "u_alpha_ctrl",   "u_num_ind_stages", "u_fog_enable",
};

/** True if @p line declares one of the folded uniforms (whole-identifier match). */
bool DeclaresFoldedUniform(const std::string &line)
{
    const std::size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos || line.compare(start, 7, "uniform") != 0)
        return false;
    const auto isIdent = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (const char *name : kFoldedUniforms) {
        const std::size_t at = line.find(name);
        const std::size_t end = at + std::char_traits<char>::length(name);
        if (at != std::string::npos && !isIdent(line[at - 1]) && (end >= line.size() || !isIdent(line[end])))
            return true;
    }
    return false;
}

/**
 * Builds the specialised fragment shader for @p scene the way the port does:
 * drop the folded uniform declarations and declare them as constants instead,
 * so the compiler can remove the run-time loops, switches and indexing.
 */
std::string SpecialisedFragment(const Scene &scene)
{
    const std::string es = RewriteToEs(kAcgcFragmentShader);
    std::istringstream lines(es);
    std::ostringstream out;
    std::string line;
    bool constantsWritten = false;
    while (std::getline(lines, line)) {
        if (DeclaresFoldedUniform(line))
            continue;
        out << line << '\n';
        // Right after the precision header, before anything can use the names.
        if (!constantsWritten && line.rfind("precision highp sampler2D", 0) == 0) {
            out << VariantConstants(MakeTevSetup(scene));
            constantsWritten = true;
        }
    }
    return out.str();
}

/** Compiles and links one program. @return 0 on failure; @p ms receives the compile+link time. */
GLuint BuildProgram(const std::string &vertex, const std::string &fragment, double *ms, std::string *log)
{
    Gl &g              = gl();
    const Uint64 start = SDL_GetPerformanceCounter();
    std::string vsLog, fsLog, linkLog;
    const GLuint vs = CompileShader(GL_VERTEX_SHADER, vertex, &vsLog);
    const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, fragment, &fsLog);
    const GLuint program = (vs && fs) ? LinkProgram(vs, fs, &linkLog) : 0;
    *ms = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
    if (vs)
        g.glDeleteShader(vs);
    if (fs)
        g.glDeleteShader(fs);
    *log = fsLog + vsLog + linkLog;
    return program;
}

/** An off-screen colour target (texture + framebuffer) of a given size. */
struct RenderTarget {
    GLuint texture     = 0;
    GLuint framebuffer = 0;
    int width          = 0;
    int height         = 0;
};

/** Creates an RGBA8 render target. @return A target with framebuffer 0 on failure. */
RenderTarget MakeRenderTarget(int width, int height)
{
    Gl &g = gl();
    RenderTarget target;
    target.width  = width;
    target.height = height;
    g.glGenTextures(1, &target.texture);
    g.glBindTexture(GL_TEXTURE_2D, target.texture);
    g.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    g.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    g.glGenFramebuffers(1, &target.framebuffer);
    g.glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    g.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target.texture, 0);
    if (g.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        g.glDeleteFramebuffers(1, &target.framebuffer);
        target.framebuffer = 0;
    }
    g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return target;
}

void FreeRenderTarget(RenderTarget &target)
{
    Gl &g = gl();
    if (target.framebuffer)
        g.glDeleteFramebuffers(1, &target.framebuffer);
    if (target.texture)
        g.glDeleteTextures(1, &target.texture);
    target = RenderTarget{};
}

/**
 * Draws @p scene into @p target, scales it up to the window and measures the
 * GPU time per frame (draw + upscale, waited for with glFinish).
 *
 * @return Average milliseconds per frame, or a negative value if nothing could be measured.
 */
double MeasureScene(GLuint program, const Scene &scene, const RenderTarget &target, SDL_Window *window, int winW, int winH,
                    double *p95, std::size_t *frames, bool *timedOut)
{
    Gl &g = gl();
    g.glUseProgram(program);
    ConfigureScene(program, scene);

    std::vector<double> frameMs;
    const Uint32 sceneStart = SDL_GetTicks();
    Uint32 measureStart     = 0;
    *timedOut               = false;
    for (int frame = 0;; ++frame) {
        const Uint32 now = SDL_GetTicks();
        if (frame == kWarmupFrames)
            measureStart = now;
        if (frame > kWarmupFrames && frameMs.size() >= kMinMeasuredFrames && SDL_TICKS_PASSED(now, measureStart + kBenchmarkMs))
            break;
        if (SDL_TICKS_PASSED(now, sceneStart + kSceneTimeoutMs)) {
            *timedOut = true;
            break;
        }

        const Uint64 t0 = SDL_GetPerformanceCounter();
        g.glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
        g.glViewport(0, 0, target.width, target.height);
        g.glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        g.glClear(GL_COLOR_BUFFER_BIT);
        g.glEnable(GL_BLEND);
        for (int layer = 0; layer < scene.layers; ++layer)
            g.glDrawArrays(GL_TRIANGLES, 0, 6);
        g.glDisable(GL_BLEND);
        // Scale up to the window, as the TV would show it.
        g.glBindFramebuffer(GL_READ_FRAMEBUFFER, target.framebuffer);
        g.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        g.glBlitFramebuffer(0, 0, target.width, target.height, 0, 0, winW, winH, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        g.glFinish(); // Wait for the GPU so the time measured is the GPU's, not just command submission.
        const Uint64 t1 = SDL_GetPerformanceCounter();
        g.glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (frame >= kWarmupFrames)
            frameMs.push_back(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency()));
        SDL_GL_SwapWindow(window);
        SDL_PumpEvents(); // keep the compositor happy while measuring
    }

    *frames = frameMs.size();
    if (frameMs.empty())
        return -1.0;
    double average = 0.0;
    Summarise(frameMs, &average, p95);
    return average;
}

/** "12.3 ms avg, 14.0 ms p95" plus a note when the scene hit the time limit. */
std::string Timing(double average, double p95, std::size_t frames, bool timedOut)
{
    if (average < 0.0)
        return "too slow to measure";
    return Format("%.1f ms", average) + " avg, " + Format("%.1f ms", p95) + " p95" +
           (timedOut ? " (only " + std::to_string(frames) + " frames)" : "");
}

} // namespace

void ProbePerformance(Report &report, GLuint uberProgram, SDL_Window *window)
{
    report.Section("Performance (port's TEV shader, upscaled to the TV)");
    if (!uberProgram) {
        report.Add(Status::Fail, "Benchmark", "skipped: the port's shader did not build");
        return;
    }

    Gl &g = gl();
    if (!g.glGenFramebuffers || !g.glBlitFramebuffer) {
        report.Add(Status::Fail, "Benchmark", "skipped: framebuffer functions missing");
        return;
    }
    int winW = 0, winH = 0;
    SDL_GL_GetDrawableSize(window, &winW, &winH);

    // Full-screen quad: position (x, y, z) and texture coordinate (u, v).
    const std::array<float, 30> quad = {
        -1, -1, 0, 0, 0, 1, -1, 0, 2, 0, -1, 1, 0, 0, 2, //
        1,  -1, 0, 2, 0, 1, 1,  0, 2, 2, -1, 1, 0, 0, 2,
    };
    GLuint buffer = 0;
    g.glGenBuffers(1, &buffer);
    g.glBindBuffer(GL_ARRAY_BUFFER, buffer);
    g.glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad.data(), GL_STATIC_DRAW);
    const GLsizei stride = 5 * sizeof(float);
    g.glEnableVertexAttribArray(0);
    g.glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, nullptr);
    g.glEnableVertexAttribArray(3);
    g.glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void *>(3 * sizeof(float)));
    g.glVertexAttrib3f(1, 0.0f, 0.0f, 1.0f);       // a_normal (constant)
    g.glVertexAttrib4f(2, 1.0f, 0.9f, 0.8f, 1.0f); // a_color0 (constant)

    std::array<GLuint, 5> textures{};
    for (std::size_t i = 0; i < textures.size(); ++i) {
        textures[i] = MakeTexture(static_cast<int>(i % 3));
        g.glActiveTexture(static_cast<GLenum>(GL_TEXTURE0 + i));
        g.glBindTexture(GL_TEXTURE_2D, textures[i]);
    }
    g.glActiveTexture(GL_TEXTURE0);
    g.glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::array<RenderTarget, kRenderSizes.size()> targets{};
    for (std::size_t r = 0; r < kRenderSizes.size(); ++r)
        targets[r] = MakeRenderTarget(kRenderSizes[r].width, kRenderSizes[r].height);

    const std::string vertex = RewriteToEs(kAcgcVertexShader);
    for (const Scene &scene : kScenes) {
        // The uber shader is the reference from probe 0.1; at 1080p only, since it is slow.
        if (&scene == &kScenes[0] && targets[0].framebuffer) {
            double p95 = 0.0;
            std::size_t frames = 0;
            bool timedOut = false;
            const double avg = MeasureScene(uberProgram, scene, targets[0], window, winW, winH, &p95, &frames, &timedOut);
            report.Add(Status::Info, std::string(scene.name) + ", uber, 1080p", Timing(avg, p95, frames, timedOut));
        }

        // The specialised variant the port would actually use for this material.
        double compileMs = 0.0;
        std::string log;
        const GLuint variant = BuildProgram(vertex, SpecialisedFragment(scene), &compileMs, &log);
        if (!variant) {
            report.Add(Status::Fail, std::string(scene.name) + ", variant", "does not compile");
            report.Detail(log);
            continue;
        }
        report.Add(compileMs <= 100.0 ? Status::Pass : Status::Warn, std::string(scene.name) + ", variant compile",
                   Format("%.0f ms", compileMs));

        for (std::size_t r = 0; r < kRenderSizes.size(); ++r) {
            const std::string label = std::string(scene.name) + ", variant, " + kRenderSizes[r].label;
            if (!targets[r].framebuffer) {
                report.Add(Status::Fail, label, "could not create the render target");
                continue;
            }
            double p95 = 0.0;
            std::size_t frames = 0;
            bool timedOut = false;
            const double avg = MeasureScene(variant, scene, targets[r], window, winW, winH, &p95, &frames, &timedOut);
            const Status status = avg < 0.0              ? Status::Fail
                                  : avg <= scene.passMs ? Status::Pass
                                  : avg <= scene.warnMs ? Status::Warn
                                                        : Status::Fail;
            report.Add(status, label, Timing(avg, p95, frames, timedOut));
        }
        g.glDeleteProgram(variant);
    }

    report.Add(Status::Info, "Scenes", "typical: 3 layers, 2 stages; worst: 4 layers, 3 stages, 8 lights, indirect");
    report.Add(Status::Info, "Budget", Format("%.1f ms", kFrameBudget30Ms) + " per frame at 30 fps (the game's rate), " +
                                           Format("%.1f ms", kFrameBudget60Ms) + " at 60 fps");

    for (RenderTarget &target : targets)
        FreeRenderTarget(target);
    g.glDisableVertexAttribArray(0);
    g.glDisableVertexAttribArray(3);
    g.glBindBuffer(GL_ARRAY_BUFFER, 0);
    g.glDeleteBuffers(1, &buffer);
    g.glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
    g.glUseProgram(0);
    g.glViewport(0, 0, winW, winH);
}

void ProbeProgramCache(Report &report, GLuint program)
{
    Gl &g = gl();
    if (!program || !g.glGetProgramBinary || !g.glProgramBinary) {
        report.Add(Status::Warn, "Shader cache", "program binaries unavailable");
        return;
    }

    const GLint formats = GlInt(GL_NUM_PROGRAM_BINARY_FORMATS);
    GLint length = 0;
    g.glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
    if (formats <= 0 || length <= 0) {
        report.Add(Status::Warn, "Shader cache", std::to_string(formats) + " binary formats: compiled shaders cannot be saved");
        return;
    }

    // Save the compiled program and load it into a new one, as a disk cache would on the next launch.
    std::vector<std::uint8_t> binary(static_cast<std::size_t>(length));
    GLenum format = 0;
    g.glGetProgramBinary(program, length, nullptr, &format, binary.data());

    const Uint64 start   = SDL_GetPerformanceCounter();
    const GLuint reload  = g.glCreateProgram();
    g.glProgramBinary(reload, format, binary.data(), length);
    GLint linked = GL_FALSE;
    g.glGetProgramiv(reload, GL_LINK_STATUS, &linked);
    const double ms = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 /
                      static_cast<double>(SDL_GetPerformanceFrequency());
    g.glDeleteProgram(reload);

    if (linked == GL_TRUE)
        report.Add(Status::Pass, "Shader cache", "binaries work: " + std::to_string(length / 1024) + " KB, reloaded in " +
                                                     Format("%.1f ms", ms) + " (vs a full compile)");
    else
        report.Add(Status::Warn, "Shader cache", "saving works but reloading failed");
}

} // namespace probe
