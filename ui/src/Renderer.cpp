#include "hybrid/ui/Renderer.hpp"
#include "hybrid/ui/Gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hc::ui {

using namespace hc::gl;

namespace {

constexpr float kModeImage = 0, kModeSolid = 1, kModeCircle = 2, kModeRoundRect = 3, kModeGlow = 4, kModeText = 5, kModeArc = 6;

constexpr const char* kVertexSrc = R"(#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec2 aLocal;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec4 aP;
layout(location = 5) in vec4 aQ;
uniform vec2 uScale;
out vec2 vUv; out vec2 vLocal; out vec4 vColor; out vec4 vP; out vec4 vQ;
void main() {
    vUv = aUv; vLocal = aLocal; vColor = aColor; vP = aP; vQ = aQ;
    gl_Position = vec4(aPos.x * uScale.x - 1.0, 1.0 - aPos.y * uScale.y, 0.0, 1.0);
}
)";

constexpr const char* kFragmentSrc = R"(#version 330 core
in vec2 vUv; in vec2 vLocal; in vec4 vColor; in vec4 vP; in vec4 vQ;
uniform sampler2D uImage;
uniform sampler2D uFont;
out vec4 frag;
const float PI = 3.14159265;
void main() {
    int mode = int(vP.x + 0.5);
    vec4 c = vColor;
    if (mode == 0) {
        c *= texture(uImage, vUv);
    } else if (mode == 2) {                     // circle: vP.z = radius px, vP.y = shading
        float r = length(vLocal);
        c.a *= clamp(vP.z - r + 0.5, 0.0, 1.0);
        vec2 n = vLocal / max(vP.z, 1.0);
        float lightT = clamp(0.5 - 0.5 * (n.x * 0.45 + n.y * 0.9), 0.0, 1.0);
        c.rgb *= mix(1.0 - vP.y, 1.0 + vP.y, lightT);
        float rim = smoothstep(0.82, 1.0, length(n));
        c.rgb *= 1.0 - 0.35 * vP.y * rim;
    } else if (mode == 3) {                     // rounded rect: vP.y radius px, vQ.xy half size px
        vec2 q = abs(vLocal) - (vQ.xy - vP.y);
        float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - vP.y;
        c.a *= clamp(0.5 - d, 0.0, 1.0);
    } else if (mode == 4) {                     // glow: vQ.xy core half size px, vP.y spread px
        vec2 q = max(abs(vLocal) - vQ.xy, 0.0);
        float d = length(q);
        c.a *= exp(-(d * d) / max(vP.y * vP.y, 1.0)) ;
    } else if (mode == 5) {                     // text coverage
        c.a *= texture(uFont, vUv).r;
    } else if (mode == 6) {                     // arc: vP.y a0, vP.z a1 (rad, 0 = up, cw), vQ.x outer radius px, vQ.y thickness px
        float r = length(vLocal);
        float ang = atan(vLocal.x, -vLocal.y);
        float ring = clamp(vQ.x - r + 0.5, 0.0, 1.0) * clamp(r - (vQ.x - vQ.y) + 0.5, 0.0, 1.0);
        float a0 = min(vP.y, vP.z), a1 = max(vP.y, vP.z);
        float edge = 1.0 / max(r, 1.0);
        float inArc = smoothstep(a0 - edge, a0 + edge, ang) * (1.0 - smoothstep(a1 - edge, a1 + edge, ang));
        c.a *= ring * inArc;
    }
    if (c.a <= 0.001) discard;
    frag = c;
}
)";

GLuint compile(GLenum type, const char* src, std::string* error) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, kCompileStatus, &ok);
    if (!ok) {
        GLint len = 0;
        glGetShaderiv(s, kInfoLogLength, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(s, len, nullptr, log.data());
        if (error) *error = "shader compile failed: " + log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

std::uint8_t toByte(float v) { return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

} // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(std::string* error) {
    const auto* ver = reinterpret_cast<const char*>(::glGetString(GL_VERSION));
    const auto* ren = reinterpret_cast<const char*>(::glGetString(GL_RENDERER));
    version_      = ver ? ver : "?";
    rendererName_ = ren ? ren : "?";
    s3tc_ = hasExtension("GL_EXT_texture_compression_s3tc");

    const GLuint vs = compile(kVertexShader, kVertexSrc, error);
    if (!vs) return false;
    const GLuint fs = compile(kFragmentShader, kFragmentSrc, error);
    if (!fs) { glDeleteShader(vs); return false; }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program_, kLinkStatus, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(program_, kInfoLogLength, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(program_, len, nullptr, log.data());
        if (error) *error = "shader link failed: " + log;
        return false;
    }
    glUseProgram(program_);
    uScale_ = glGetUniformLocation(program_, "uScale");
    glUniform1i(glGetUniformLocation(program_, "uImage"), 0);
    glUniform1i(glGetUniformLocation(program_, "uFont"), 1);

    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(kArrayBuffer, vbo_);
    const auto stride = static_cast<GLsizei>(sizeof(Vertex));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, u)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, lx)));
    glVertexAttribPointer(3, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride, reinterpret_cast<const void*>(offsetof(Vertex, r)));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, p0)));
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, q0)));
    for (GLuint i = 0; i < 6; ++i) glEnableVertexAttribArray(i);
    verts_.reserve(6 * 4096);
    return ::glGetError() == GL_NO_ERROR;
}

void Renderer::shutdown() noexcept {
    if (!glDeleteProgram) return;   // GL never loaded
    if (faceTex_) { ::glDeleteTextures(1, &faceTex_); faceTex_ = 0; budget_.release(faceBytes_); }
    if (fontTex_) { ::glDeleteTextures(1, &fontTex_); fontTex_ = 0; budget_.release(fontBytes_); }
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
}

bool Renderer::setFaceplate(const CompressedTexture& tex, std::string* error) {
    if (tex.levels.empty() || tex.width > kMaxTextureSide || tex.height > kMaxTextureSide) {
        if (error) *error = "faceplate rejected: empty or larger than the 4096 px cap";
        return false;
    }
    std::uint64_t bytes = tex.bytes();
    if (!s3tc_) bytes *= 4;   // uncompressed fallback costs 4 bytes/pixel
    if (faceTex_) { ::glDeleteTextures(1, &faceTex_); faceTex_ = 0; budget_.release(faceBytes_); faceBytes_ = 0; }
    if (!budget_.reserve(bytes)) {
        if (error) *error = "faceplate rejected: would exceed the 64 MB UI VRAM budget";
        return false;
    }
    faceBytes_ = bytes;
    ::glGenTextures(1, &faceTex_);
    glActiveTexture(kTexture0);
    ::glBindTexture(GL_TEXTURE_2D, faceTex_);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::uint32_t w = tex.width, h = tex.height;
    for (std::size_t l = 0; l < tex.levels.size(); ++l) {
        if (s3tc_) {
            glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), kCompressedRgbaDxt5, static_cast<GLsizei>(w),
                                   static_cast<GLsizei>(h), 0, static_cast<GLsizei>(tex.levels[l].size()), tex.levels[l].data());
        } else {
            const Image img = decompressDxt5(tex.levels[l].data(), w, h);
            ::glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), GL_RGBA8, static_cast<GLsizei>(w), static_cast<GLsizei>(h), 0,
                           GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
        }
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
    }
    ::glTexParameteri(GL_TEXTURE_2D, kTextureBaseLevel, 0);
    ::glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, static_cast<GLint>(tex.levels.size() - 1));
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) {
        if (error) *error = "faceplate upload failed (GL error " + std::to_string(err) + ")";
        return false;
    }
    return true;
}

bool Renderer::setFont(const FontAtlas& font, std::string* error) {
    if (!font.valid()) { if (error) *error = "font atlas not baked"; return false; }
    if (fontTex_) { ::glDeleteTextures(1, &fontTex_); fontTex_ = 0; budget_.release(fontBytes_); fontBytes_ = 0; }
    const std::uint64_t bytes = static_cast<std::uint64_t>(font.atlasSize()) * font.atlasSize();
    if (!budget_.reserve(bytes)) { if (error) *error = "font atlas exceeds VRAM budget"; return false; }
    fontBytes_ = bytes;
    ::glGenTextures(1, &fontTex_);
    glActiveTexture(kTexture0 + 1);
    ::glBindTexture(GL_TEXTURE_2D, fontTex_);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ::glTexImage2D(GL_TEXTURE_2D, 0, kR8, static_cast<GLsizei>(font.atlasSize()), static_cast<GLsizei>(font.atlasSize()), 0,
                   GL_RED, GL_UNSIGNED_BYTE, font.pixels().data());
    ::glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, 0);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
    glActiveTexture(kTexture0);
    font_ = &font;
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) { if (error) *error = "font upload failed (GL error " + std::to_string(err) + ")"; return false; }
    return true;
}

void Renderer::begin(int w, int h, const Viewport& vp, Rgba clear) {
    fbW_ = std::max(1, w);
    fbH_ = std::max(1, h);
    vp_  = vp;
    stats_ = {};
    verts_.clear();
    blend_ = Blend::Alpha;
    ::glViewport(0, 0, fbW_, fbH_);
    ::glDisable(GL_SCISSOR_TEST);
    ::glClearColor(clear.r, clear.g, clear.b, 1.0f);
    ::glClear(GL_COLOR_BUFFER_BIT);
    ::glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);
    glUniform2f(uScale_, 2.0f / static_cast<float>(fbW_), 2.0f / static_cast<float>(fbH_));
    glBindVertexArray(vao_);
    glBindBuffer(kArrayBuffer, vbo_);
    glActiveTexture(kTexture0);
    ::glBindTexture(GL_TEXTURE_2D, faceTex_);
    glActiveTexture(kTexture0 + 1);
    ::glBindTexture(GL_TEXTURE_2D, fontTex_);
    glActiveTexture(kTexture0);
}

void Renderer::end() { flush(); }

void Renderer::flush() {
    if (verts_.empty()) return;
    if (blend_ == Blend::Additive) glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ZERO, GL_ONE);
    else                           glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glBufferData(kArrayBuffer, static_cast<GLsizeiptr>(verts_.size() * sizeof(Vertex)), verts_.data(), kStreamDraw);
    ::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts_.size()));
    ++stats_.drawCalls;
    verts_.clear();
}

void Renderer::quad(const float xs[4], const float ys[4], const float us[4], const float vs[4], const float lxs[4],
                    const float lys[4], Rgba c, float mode, const float p[3], const float q[4], Blend blend) {
    if (blend != blend_) { flush(); blend_ = blend; }
    const std::uint8_t r = toByte(c.r), g = toByte(c.g), b = toByte(c.b), a = toByte(c.a);
    auto v = [&](int i) {
        return Vertex{xs[i], ys[i], us[i], vs[i], lxs[i], lys[i], r, g, b, a, mode, p[0], p[1], p[2], q[0], q[1], q[2], q[3]};
    };
    // corners: 0 TL, 1 TR, 2 BR, 3 BL
    verts_.push_back(v(0)); verts_.push_back(v(1)); verts_.push_back(v(2));
    verts_.push_back(v(0)); verts_.push_back(v(2)); verts_.push_back(v(3));
    ++stats_.quads;
}

void Renderer::axisQuad(Rect px, float u0, float v0, float u1, float v1, Rgba c, float mode, const float p[3],
                        const float q[4], Blend blend, float hw, float hh) {
    const float xs[4]  = {px.x, px.x + px.w, px.x + px.w, px.x};
    const float ys[4]  = {px.y, px.y, px.y + px.h, px.y + px.h};
    const float us[4]  = {u0, u1, u1, u0};
    const float vs[4]  = {v0, v0, v1, v1};
    const float lxs[4] = {-hw, hw, hw, -hw};
    const float lys[4] = {-hh, -hh, hh, hh};
    quad(xs, ys, us, vs, lxs, lys, c, mode, p, q, blend);
}

void Renderer::faceplate(Rect r, Rgba tint) {
    const float p[3] = {0, 0, 0}, q[4] = {0, 0, 0, 0};
    axisQuad({sx(r.x), sy(r.y), r.w * vp_.scale, r.h * vp_.scale}, 0, 0, 1, 1, tint, kModeImage, p, q, Blend::Alpha, 0, 0);
}

void Renderer::rect(Rect r, Rgba c) {
    const float p[3] = {0, 0, 0}, q[4] = {0, 0, 0, 0};
    axisQuad({sx(r.x), sy(r.y), r.w * vp_.scale, r.h * vp_.scale}, 0, 0, 0, 0, c, kModeSolid, p, q, Blend::Alpha, 0, 0);
}

void Renderer::roundRect(Rect r, float radius, Rgba c) {
    const float s = vp_.scale, hw = r.w * s * 0.5f + 1, hh = r.h * s * 0.5f + 1;
    const float p[3] = {std::min(radius * s, std::min(r.w, r.h) * s * 0.5f), 0, 0};
    const float q[4] = {r.w * s * 0.5f, r.h * s * 0.5f, 0, 0};
    axisQuad({sx(r.cx()) - hw, sy(r.cy()) - hh, hw * 2, hh * 2}, 0, 0, 0, 0, c, kModeRoundRect, p, q, Blend::Alpha, hw, hh);
}

void Renderer::roundRectRotated(float cx, float cy, float w, float h, float radius, float ang, Rgba c) {
    const float s = vp_.scale, hw = w * s * 0.5f + 1, hh = h * s * 0.5f + 1;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const float lx[4] = {-hw, hw, hw, -hw}, ly[4] = {-hh, -hh, hh, hh};
    float xs[4], ys[4];
    for (int i = 0; i < 4; ++i) {
        xs[i] = sx(cx) + lx[i] * ca - ly[i] * sa;
        ys[i] = sy(cy) + lx[i] * sa + ly[i] * ca;
    }
    const float zero[4] = {0, 0, 0, 0};
    const float p[3] = {std::min(radius * s, std::min(w, h) * s * 0.5f), 0, 0};
    const float q[4] = {w * s * 0.5f, h * s * 0.5f, 0, 0};
    quad(xs, ys, zero, zero, lx, ly, c, kModeRoundRect, p, q, Blend::Alpha);
}

void Renderer::circle(float cx, float cy, float radius, Rgba c, float shading) {
    const float R = radius * vp_.scale, h = R + 1.5f;
    const float p[3] = {shading, R, 0}, q[4] = {0, 0, 0, 0};
    axisQuad({sx(cx) - h, sy(cy) - h, h * 2, h * 2}, 0, 0, 0, 0, c, kModeCircle, p, q, Blend::Alpha, h, h);
}

void Renderer::arc(float cx, float cy, float radius, float thickness, float a0, float a1, Rgba c) {
    const float R = radius * vp_.scale, h = R + 1.5f;
    const float p[3] = {a0, a1, 0}, q[4] = {R, thickness * vp_.scale, 0, 0};
    axisQuad({sx(cx) - h, sy(cy) - h, h * 2, h * 2}, 0, 0, 0, 0, c, kModeArc, p, q, Blend::Alpha, h, h);
}

void Renderer::glow(Rect core, float spread, Rgba c) {
    const float s = vp_.scale, S = spread * s;
    const float hw = core.w * s * 0.5f + 2.5f * S, hh = core.h * s * 0.5f + 2.5f * S;
    const float p[3] = {S, 0, 0}, q[4] = {core.w * s * 0.5f, core.h * s * 0.5f, 0, 0};
    axisQuad({sx(core.cx()) - hw, sy(core.cy()) - hh, hw * 2, hh * 2}, 0, 0, 0, 0, c, kModeGlow, p, q, Blend::Additive, hw, hh);
}

float Renderer::textWidth(TextSize size, std::string_view s) const { return font_ ? font_->measure(size, s) : 0.0f; }

void Renderer::text(TextSize size, std::string_view s, float x, float baseline, Rgba c, Align align) {
    if (!font_ || !font_->valid()) return;
    if (align != Align::Left) {
        const float w = font_->measure(size, s);
        x -= (align == Align::Center) ? w * 0.5f : w;
    }
    const float p[3] = {0, 0, 0}, q[4] = {0, 0, 0, 0};
    font_->layout(size, s, x, baseline, [&](const GlyphQuad& g) {
        axisQuad({sx(g.x0), sy(g.y0), (g.x1 - g.x0) * vp_.scale, (g.y1 - g.y0) * vp_.scale}, g.s0, g.t0, g.s1, g.t1, c,
                 kModeText, p, q, Blend::Alpha, 0, 0);
    });
}

} // namespace hc::ui
