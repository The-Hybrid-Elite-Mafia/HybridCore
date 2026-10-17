#include "hybrid/ui/Renderer.hpp"
#include "hybrid/ui/Gl.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace hc::ui {

using namespace hc::gl;

namespace {

constexpr float kModeTexture = 0, kModeText = 1, kModeTextBlur = 2, kModeArc = 3, kModeGlow = 4, kModeFill = 5, kModeFlat = 6;
constexpr float kTexChassis = 0, kTexParts = 1, kTexStatic = 2;
constexpr float kBloomStrength = 0.32f;

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

// Colours are linear and premultiplied; textures are sRGB (decoded to linear by the GPU).
constexpr const char* kFragmentSrc = R"(#version 330 core
in vec2 vUv; in vec2 vLocal; in vec4 vColor; in vec4 vP; in vec4 vQ;
uniform sampler2D uChassis;
uniform sampler2D uParts;
uniform sampler2D uStatic;
uniform sampler2D uFont;
out vec4 frag;
float sdRoundRect(vec2 p, vec2 hs, float r) {
    vec2 q = abs(p) - hs + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
void main() {
    int mode = int(vP.x + 0.5);
    int tex  = int(vP.y + 0.5);
    vec2 gx = dFdx(vUv), gy = dFdy(vUv);
    vec4 c = vColor;
    if (mode == 0) {
        // Chassis and parts are minified with a sharper footprint (0.6 x the pixel's): crisper chrome
        // when the window is smaller than 4K; at 4K the chassis is still sampled 1:1 from level 0.
        vec4 t = tex == 0 ? textureGrad(uChassis, vUv, gx * 0.6, gy * 0.6)
               : tex == 1 ? textureGrad(uParts, vUv, gx * 0.6, gy * 0.6)
                          : textureGrad(uStatic, vUv, gx, gy);
        c *= t;
    } else if (mode == 1) {
        c *= textureGrad(uFont, vUv, gx, gy).r;
    } else if (mode == 2) {
        c *= textureGrad(uFont, vUv, gx, gy).g;
    } else if (mode == 3) {                      // arc: vQ.x centre radius px, vQ.y width px, vP.zw angles
        float r = length(vLocal);
        float ang = atan(vLocal.x, -vLocal.y);
        float ring = clamp(vQ.y * 0.5 - abs(r - vQ.x) + 0.5, 0.0, 1.0);
        float a0 = min(vP.z, vP.w), a1 = max(vP.z, vP.w);
        float e = 0.75 / max(r, 1.0);
        float inArc = smoothstep(a0 - e, a0 + e, ang) * (1.0 - smoothstep(a1 - e, a1 + e, ang));
        c *= ring * inArc;
    } else if (mode == 4) {                      // glow: vQ.xy half size px, vQ.z radius px, vP.z spread px
        float d = max(sdRoundRect(vLocal, vQ.xy, vQ.z), 0.0);
        c *= exp(-(d * d) / max(vP.z * vP.z, 1.0));
    } else if (mode == 5) {                      // soft fill: vP.z edge softness px
        float d = sdRoundRect(vLocal, vQ.xy, vQ.z);
        c *= clamp(0.5 - d / max(vP.z, 1.0), 0.0, 1.0);
    }
    frag = c;
}
)";

constexpr const char* kPostVertexSrc = R"(#version 330 core
out vec2 vUv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    vUv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char* kPostFragmentSrc = R"(#version 330 core
in vec2 vUv;
uniform int uMode;
uniform vec2 uTexel;
uniform float uStrength;
uniform sampler2D uA;
uniform sampler2D uB;
uniform sampler2D uC;
out vec4 frag;
const float W[5] = float[](0.2270, 0.1945, 0.1216, 0.0540, 0.0162);
vec3 encode(vec3 l) {
    l = clamp(l, 0.0, 1.0);
    return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), l));
}
void main() {
    if (uMode == 0 || uMode == 1) {             // separable Gaussian
        vec2 dir = uMode == 0 ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
        vec4 s = texture(uA, vUv) * W[0];
        for (int i = 1; i < 5; ++i) {
            vec2 o = dir * float(i) * 1.5;
            s += (texture(uA, vUv + o) + texture(uA, vUv - o)) * W[i];
        }
        frag = s;
    } else if (uMode == 2) {                     // downsample
        frag = texture(uA, vUv);
    } else {                                     // composite + sRGB encode
        vec3 scene = texture(uA, vUv).rgb;
        vec3 bloom = texture(uB, vUv).rgb + 0.4 * texture(uC, vUv).rgb;
        frag = vec4(encode(scene + uStrength * bloom), 1.0);
    }
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

GLuint link(const char* vsSrc, const char* fsSrc, std::string* error) {
    const GLuint vs = compile(kVertexShader, vsSrc, error);
    if (!vs) return 0;
    const GLuint fs = compile(kFragmentShader, fsSrc, error);
    if (!fs) { glDeleteShader(vs); return 0; }
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, kLinkStatus, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(p, kInfoLogLength, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(p, len, nullptr, log.data());
        if (error) *error = "shader link failed: " + log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

void setTexParams(bool mips, GLint levels) {
    ::glTexParameteri(GL_TEXTURE_2D, kTextureBaseLevel, 0);
    ::glTexParameteri(GL_TEXTURE_2D, kTextureMaxLevel, mips ? levels - 1 : 0);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, kClampToEdge);
    ::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, kClampToEdge);
}

} // namespace

Renderer::~Renderer() { shutdown(); }

bool Renderer::init(int wantSamples, std::string* error) {
    const auto* ver = reinterpret_cast<const char*>(::glGetString(GL_VERSION));
    const auto* ren = reinterpret_cast<const char*>(::glGetString(GL_RENDERER));
    version_      = ver ? ver : "?";
    rendererName_ = ren ? ren : "?";
    GLint ms = 1;
    ::glGetIntegerv(kMaxSamples, &ms);
    maxSamples_ = std::max(1, static_cast<int>(ms));
    samples_    = std::clamp(wantSamples, 1, maxSamples_);

    program_ = link(kVertexSrc, kFragmentSrc, error);
    if (!program_) return false;
    post_ = link(kPostVertexSrc, kPostFragmentSrc, error);
    if (!post_) return false;

    glUseProgram(program_);
    uScale_ = glGetUniformLocation(program_, "uScale");
    glUniform1i(glGetUniformLocation(program_, "uChassis"), 0);
    glUniform1i(glGetUniformLocation(program_, "uParts"), 1);
    glUniform1i(glGetUniformLocation(program_, "uStatic"), 2);
    glUniform1i(glGetUniformLocation(program_, "uFont"), 3);
    glUseProgram(post_);
    uPostMode_     = glGetUniformLocation(post_, "uMode");
    uPostTexel_    = glGetUniformLocation(post_, "uTexel");
    uPostStrength_ = glGetUniformLocation(post_, "uStrength");
    glUniform1i(glGetUniformLocation(post_, "uA"), 4);
    glUniform1i(glGetUniformLocation(post_, "uB"), 5);
    glUniform1i(glGetUniformLocation(post_, "uC"), 6);

    glGenVertexArrays(1, &emptyVao_);
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &vbo_);
    glBindBuffer(kArrayBuffer, vbo_);
    const auto stride = static_cast<GLsizei>(sizeof(Vertex));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, x)));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, u)));
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, lx)));
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, r)));
    glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, mode)));
    glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<const void*>(offsetof(Vertex, q0)));
    for (GLuint i = 0; i < 6; ++i) glEnableVertexAttribArray(i);
    verts_.reserve(6 * 8192);
    bloomVerts_.reserve(6 * 2048);
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) { if (error) *error = "renderer init GL error " + std::to_string(err); return false; }
    return true;
}

void Renderer::shutdown() noexcept {
    if (!glDeleteProgram) return;   // GL never loaded
    for (Target* t : {&msaa_, &static_, &scene_, &bloomA_, &bloomB_, &wideA_, &wideB_, &capture_}) freeTarget(*t);
    targetBytes_ = 0;
    for (unsigned int* t : {&chassisTex_, &partsTex_, &fontTex_})
        if (*t) { ::glDeleteTextures(1, t); *t = 0; }
    budget_.release(chassisBytes_ + partsBytes_ + fontBytes_);
    chassisBytes_ = partsBytes_ = fontBytes_ = 0;
    if (vbo_) { glDeleteBuffers(1, &vbo_); vbo_ = 0; }
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (emptyVao_) { glDeleteVertexArrays(1, &emptyVao_); emptyVao_ = 0; }
    if (program_) { glDeleteProgram(program_); program_ = 0; }
    if (post_) { glDeleteProgram(post_); post_ = 0; }
    fbW_ = fbH_ = 0;
}

namespace {
bool uploadLevels(unsigned int& tex, std::uint64_t& bytesSlot, TextureBudget& budget, const TextureLevels& t, int unit,
                  const char* what, std::string* error) {
    if (t.levels.empty() || t.width > kMaxTextureSide || t.height > kMaxTextureSide) {
        if (error) *error = std::string(what) + " rejected: empty or larger than the 4096 px cap";
        return false;
    }
    if (tex) { ::glDeleteTextures(1, &tex); tex = 0; budget.release(bytesSlot); bytesSlot = 0; }
    if (!budget.reserve(t.bytes())) {
        if (error) *error = std::string(what) + " rejected: would exceed the 64 MB UI texture budget";
        return false;
    }
    bytesSlot = t.bytes();
    ::glGenTextures(1, &tex);
    glActiveTexture(kTexture0 + static_cast<GLenum>(unit));
    ::glBindTexture(GL_TEXTURE_2D, tex);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::uint32_t w = t.width, h = t.height;
    for (std::size_t l = 0; l < t.levels.size(); ++l) {
        ::glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(l), static_cast<GLint>(kSrgb8Alpha8), static_cast<GLsizei>(w),
                       static_cast<GLsizei>(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, t.levels[l].data());
        w = std::max<std::uint32_t>(1, w / 2);
        h = std::max<std::uint32_t>(1, h / 2);
    }
    setTexParams(true, static_cast<GLint>(t.levels.size()));
    glActiveTexture(kTexture0);
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) { if (error) *error = std::string(what) + " upload failed (GL error " + std::to_string(err) + ")"; return false; }
    return true;
}
} // namespace

bool Renderer::setChassis(const TextureLevels& t, std::string* error) {
    chassisW_ = static_cast<float>(t.width);
    chassisH_ = static_cast<float>(t.height);
    return uploadLevels(chassisTex_, chassisBytes_, budget_, t, 0, "faceplate", error);
}

bool Renderer::setParts(const PartsAtlas& atlas, std::string* error) {
    parts_ = atlas;
    const bool ok = uploadLevels(partsTex_, partsBytes_, budget_, atlas.texture, 1, "parts atlas", error);
    parts_.texture.levels.clear();   // CPU copy no longer needed; keep the sprite table
    return ok;
}

bool Renderer::setFont(const FontAtlas& font, std::string* error) {
    if (!font.valid()) { if (error) *error = "font atlas not baked"; return false; }
    if (fontTex_) { ::glDeleteTextures(1, &fontTex_); fontTex_ = 0; budget_.release(fontBytes_); fontBytes_ = 0; }
    const std::uint64_t bytes = static_cast<std::uint64_t>(font.atlasSize()) * font.atlasSize() * 2;
    if (!budget_.reserve(bytes)) { if (error) *error = "font atlas exceeds the UI texture budget"; return false; }
    fontBytes_ = bytes;
    ::glGenTextures(1, &fontTex_);
    glActiveTexture(kTexture0 + 3);
    ::glBindTexture(GL_TEXTURE_2D, fontTex_);
    ::glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    ::glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(kRg8), static_cast<GLsizei>(font.atlasSize()), static_cast<GLsizei>(font.atlasSize()), 0,
                   kRg, GL_UNSIGNED_BYTE, font.pixels().data());
    setTexParams(false, 1);
    glActiveTexture(kTexture0);
    font_ = &font;
    const GLenum err = ::glGetError();
    if (err != GL_NO_ERROR) { if (error) *error = "font upload failed (GL error " + std::to_string(err) + ")"; return false; }
    return true;
}

// ---- render targets ---------------------------------------------------------------------
bool Renderer::makeTarget(Target& t, int w, int h, unsigned int internalFormat, unsigned int format, unsigned int type, bool texture) {
    freeTarget(t);
    t.w = std::max(1, w);
    t.h = std::max(1, h);
    glGenFramebuffers(1, &t.fbo);
    glBindFramebuffer(kFramebuffer, t.fbo);
    int bpp = (internalFormat == kRgba16f) ? 8 : 4;
    if (texture) {
        ::glGenTextures(1, &t.tex);
        ::glBindTexture(GL_TEXTURE_2D, t.tex);
        ::glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat), t.w, t.h, 0, format, type, nullptr);
        setTexParams(false, 1);
        glFramebufferTexture2D(kFramebuffer, kColorAttachment0, GL_TEXTURE_2D, t.tex, 0);
    } else {
        glGenRenderbuffers(1, &t.rb);
        glBindRenderbuffer(kRenderbuffer, t.rb);
        if (samples_ > 1) glRenderbufferStorageMultisample(kRenderbuffer, samples_, internalFormat, t.w, t.h);
        else              glRenderbufferStorage(kRenderbuffer, internalFormat, t.w, t.h);
        glFramebufferRenderbuffer(kFramebuffer, kColorAttachment0, kRenderbuffer, t.rb);
        bpp *= samples_;
    }
    targetBytes_ += static_cast<std::uint64_t>(t.w) * t.h * bpp;
    return glCheckFramebufferStatus(kFramebuffer) == kFramebufferComplete;
}

void Renderer::freeTarget(Target& t) noexcept {
    if (t.fbo) glDeleteFramebuffers(1, &t.fbo);
    if (t.tex) ::glDeleteTextures(1, &t.tex);
    if (t.rb) glDeleteRenderbuffers(1, &t.rb);
    t = {};
}

bool Renderer::setTargetSize(int w, int h, std::string* error) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == fbW_ && h == fbH_) return true;
    targetBytes_ = 0;
    bool ok = makeTarget(msaa_, w, h, kSrgb8Alpha8, GL_RGBA, GL_UNSIGNED_BYTE, false);
    ok = ok && makeTarget(static_, w, h, kSrgb8Alpha8, GL_RGBA, GL_UNSIGNED_BYTE, true);
    ok = ok && makeTarget(scene_, w, h, kSrgb8Alpha8, GL_RGBA, GL_UNSIGNED_BYTE, true);
    ok = ok && makeTarget(bloomA_, (w + 1) / 2, (h + 1) / 2, kRgba16f, GL_RGBA, kHalfFloat, true);
    ok = ok && makeTarget(bloomB_, (w + 1) / 2, (h + 1) / 2, kRgba16f, GL_RGBA, kHalfFloat, true);
    ok = ok && makeTarget(wideA_, (w + 7) / 8, (h + 7) / 8, kRgba16f, GL_RGBA, kHalfFloat, true);
    ok = ok && makeTarget(wideB_, (w + 7) / 8, (h + 7) / 8, kRgba16f, GL_RGBA, kHalfFloat, true);
    ok = ok && makeTarget(capture_, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, true);
    glBindFramebuffer(kFramebuffer, 0);
    if (!ok) { if (error) *error = "could not create render targets"; fbW_ = fbH_ = 0; return false; }
    fbW_ = w;
    fbH_ = h;
    return true;
}

// ---- passes --------------------------------------------------------------------------------
void Renderer::beginPass(const Canvas& canvas) {
    canvas_ = canvas;
    verts_.clear();
    bloomVerts_.clear();
    glBindFramebuffer(kFramebuffer, msaa_.fbo);
    ::glViewport(0, 0, fbW_, fbH_);
    ::glDisable(GL_SCISSOR_TEST);
    ::glDisable(GL_DEPTH_TEST);
    ::glEnable(kFramebufferSrgb);
    ::glClearColor(0, 0, 0, 1);
    ::glClear(GL_COLOR_BUFFER_BIT);
    ::glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program_);
    glUniform2f(uScale_, 2.0f / static_cast<float>(fbW_), 2.0f / static_cast<float>(fbH_));
    glBindVertexArray(vao_);
    glBindBuffer(kArrayBuffer, vbo_);
    const unsigned int texs[4] = {chassisTex_, partsTex_, static_.tex, fontTex_};
    for (int i = 0; i < 4; ++i) { glActiveTexture(kTexture0 + static_cast<GLenum>(i)); ::glBindTexture(GL_TEXTURE_2D, texs[i]); }
    glActiveTexture(kTexture0);
}

void Renderer::flushTo(std::vector<Vertex>& v) {
    if (v.empty()) return;
    glBufferData(kArrayBuffer, static_cast<GLsizeiptr>(v.size() * sizeof(Vertex)), v.data(), kStreamDraw);
    ::glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
    ++stats_.drawCalls;
}

void Renderer::beginStatic(const Canvas& canvas) {
    stats_ = {};
    inStatic_ = true;
    // The static texture itself must not be bound while rendering into the layer that produces it.
    beginPass(canvas);
    glActiveTexture(kTexture0 + 2);
    ::glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(kTexture0);
}

void Renderer::endStatic() {
    flushTo(verts_);
    verts_.clear();
    bloomVerts_.clear();   // the static layer's emissive parts are re-submitted each frame by the view
    glBindFramebuffer(kReadFramebuffer, msaa_.fbo);
    glBindFramebuffer(kDrawFramebuffer, static_.fbo);
    glBlitFramebuffer(0, 0, fbW_, fbH_, 0, 0, fbW_, fbH_, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(kFramebuffer, 0);
    inStatic_ = false;
}

void Renderer::beginFrame(const Canvas& canvas) {
    stats_ = {};
    beginPass(canvas);
    // Static layer as the background (FBO textures are bottom-up).
    const float color[4] = {1, 1, 1, 1}, p[2] = {0, 0}, q[4] = {0, 0, 0, 0};
    rectQuad({0, 0, static_cast<float>(fbW_), static_cast<float>(fbH_)}, 0, 1, 1, 0, color, kModeTexture, kTexStatic, p, q, false);
}

void Renderer::drawFullscreen(unsigned int program, int mode) {
    glUseProgram(program);
    glUniform1i(uPostMode_, mode);
    glBindVertexArray(emptyVao_);
    ::glDrawArrays(GL_TRIANGLES, 0, 3);
    ++stats_.drawCalls;
}

void Renderer::endFrame(unsigned int targetFbo) {
    flushTo(verts_);
    // Resolve MSAA.
    glBindFramebuffer(kReadFramebuffer, msaa_.fbo);
    glBindFramebuffer(kDrawFramebuffer, scene_.fbo);
    glBlitFramebuffer(0, 0, fbW_, fbH_, 0, 0, fbW_, fbH_, GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Bloom source: emissive quads at half resolution.
    glBindFramebuffer(kFramebuffer, bloomA_.fbo);
    ::glViewport(0, 0, bloomA_.w, bloomA_.h);
    ::glClearColor(0, 0, 0, 0);
    ::glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(program_);
    glBindVertexArray(vao_);
    glBindBuffer(kArrayBuffer, vbo_);
    stats_.bloomQuads = static_cast<std::uint32_t>(bloomVerts_.size() / 6);
    flushTo(bloomVerts_);

    ::glDisable(GL_BLEND);
    glUseProgram(post_);
    glUniform1f(uPostStrength_, kBloomStrength);
    auto pass = [&](Target& src, Target& dst, int mode) {
        glBindFramebuffer(kFramebuffer, dst.fbo);
        ::glViewport(0, 0, dst.w, dst.h);
        glActiveTexture(kTexture0 + 4);
        ::glBindTexture(GL_TEXTURE_2D, src.tex);
        glUniform2f(uPostTexel_, 1.0f / static_cast<float>(src.w), 1.0f / static_cast<float>(src.h));
        drawFullscreen(post_, mode);
    };
    pass(bloomA_, bloomB_, 0);
    pass(bloomB_, bloomA_, 1);
    pass(bloomA_, wideA_, 2);
    pass(wideA_, wideB_, 0);
    pass(wideB_, wideA_, 1);

    // Composite + sRGB encode (the window framebuffer is not assumed to be sRGB-capable).
    ::glDisable(kFramebufferSrgb);
    glBindFramebuffer(kFramebuffer, targetFbo);
    ::glViewport(0, 0, fbW_, fbH_);
    glActiveTexture(kTexture0 + 4); ::glBindTexture(GL_TEXTURE_2D, scene_.tex);
    glActiveTexture(kTexture0 + 5); ::glBindTexture(GL_TEXTURE_2D, bloomA_.tex);
    glActiveTexture(kTexture0 + 6); ::glBindTexture(GL_TEXTURE_2D, wideA_.tex);
    drawFullscreen(post_, 3);
    glActiveTexture(kTexture0);
    ::glEnable(GL_BLEND);
    verts_.clear();
    bloomVerts_.clear();
}

void Renderer::endFrameToCapture() { endFrame(capture_.fbo); }

Image Renderer::capture() const {
    Image img;
    img.width  = static_cast<std::uint32_t>(fbW_);
    img.height = static_cast<std::uint32_t>(fbH_);
    img.rgba.resize(static_cast<std::size_t>(fbW_) * fbH_ * 4);
    glBindFramebuffer(kFramebuffer, capture_.fbo);
    ::glPixelStorei(GL_PACK_ALIGNMENT, 1);
    ::glReadPixels(0, 0, fbW_, fbH_, GL_RGBA, GL_UNSIGNED_BYTE, img.rgba.data());
    glBindFramebuffer(kFramebuffer, 0);
    const std::size_t row = static_cast<std::size_t>(fbW_) * 4;
    for (int y = 0; y < fbH_ / 2; ++y)
        std::swap_ranges(img.rgba.begin() + static_cast<std::ptrdiff_t>(y * row), img.rgba.begin() + static_cast<std::ptrdiff_t>((y + 1) * row),
                         img.rgba.begin() + static_cast<std::ptrdiff_t>((fbH_ - 1 - y) * row));
    return img;
}

// ---- quads -----------------------------------------------------------------------------------
void Renderer::premul(Rgba c, Blend b, float out[4]) const {
    const float a = std::clamp(c.a, 0.0f, 1.0f);
    out[0] = srgbToLinear(c.r) * a;
    out[1] = srgbToLinear(c.g) * a;
    out[2] = srgbToLinear(c.b) * a;
    out[3] = b == Blend::Add ? 0.0f : a;
}

void Renderer::quad(const float xs[4], const float ys[4], const float us[4], const float vs[4], const float lx[4], const float ly[4],
                    const float c[4], float mode, float tex, const float p[2], const float q[4], bool emissive) {
    auto v = [&](int i) {
        return Vertex{xs[i], ys[i], us[i], vs[i], lx[i], ly[i], c[0], c[1], c[2], c[3], mode, tex, p[0], p[1], q[0], q[1], q[2], q[3]};
    };
    const Vertex a = v(0), b = v(1), cc = v(2), d = v(3);   // TL TR BR BL
    if (!bloomOnly_) {
        for (const Vertex* vx : {&a, &b, &cc, &a, &cc, &d}) verts_.push_back(*vx);
        ++stats_.quads;
    }
    if (emissive) for (const Vertex* vx : {&a, &b, &cc, &a, &cc, &d}) bloomVerts_.push_back(*vx);
}

void Renderer::rectQuad(Rect r, float u0, float v0, float u1, float v1, const float c[4], float mode, float tex,
                        const float p[2], const float q[4], bool emissive, float hw, float hh) {
    const float xs[4] = {r.x, r.x + r.w, r.x + r.w, r.x};
    const float ys[4] = {r.y, r.y, r.y + r.h, r.y + r.h};
    const float us[4] = {u0, u1, u1, u0};
    const float vs[4] = {v0, v0, v1, v1};
    const float lx[4] = {-hw, hw, hw, -hw};
    const float ly[4] = {-hh, -hh, hh, hh};
    quad(xs, ys, us, vs, lx, ly, c, mode, tex, p, q, emissive);
}

void Renderer::spriteUv(Sprite s, float fx0, float fy0, float fx1, float fy1, float& u0, float& v0, float& u1, float& v1) const {
    const SpriteInfo& i = parts_[s];
    u0 = i.u0 + (i.u1 - i.u0) * fx0;
    u1 = i.u0 + (i.u1 - i.u0) * fx1;
    v0 = i.v0 + (i.v1 - i.v0) * fy0;
    v1 = i.v0 + (i.v1 - i.v0) * fy1;
}

void Renderer::chassis(Rect src, Rect dst) {
    const float c[4] = {1, 1, 1, 1}, p[2] = {0, 0}, q[4] = {0, 0, 0, 0};
    rectQuad({px(dst.x), py(dst.y), dst.w * canvas_.scale, dst.h * canvas_.scale}, src.x / plate::kWidth, src.y / plate::kHeight,
             src.right() / plate::kWidth, src.bottom() / plate::kHeight, c, kModeTexture, kTexChassis, p, q, false);
}

void Renderer::sprite(Sprite s, Rect dst, Rgba tint, Blend blend, bool emissive) {
    float c[4];
    premul(tint, blend, c);
    float u0, v0, u1, v1;
    spriteUv(s, 0, 0, 1, 1, u0, v0, u1, v1);
    const float p[2] = {0, 0}, q[4] = {0, 0, 0, 0};
    rectQuad({px(dst.x), py(dst.y), dst.w * canvas_.scale, dst.h * canvas_.scale}, u0, v0, u1, v1, c, kModeTexture, kTexParts, p, q, emissive);
}

void Renderer::spriteRotated(Sprite s, float cx, float cy, float w, float h, float ang, Rgba tint, Blend blend, bool emissive) {
    float c[4];
    premul(tint, blend, c);
    float u0, v0, u1, v1;
    spriteUv(s, 0, 0, 1, 1, u0, v0, u1, v1);
    const float hw = w * canvas_.scale * 0.5f, hh = h * canvas_.scale * 0.5f;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const float lx[4] = {-hw, hw, hw, -hw}, ly[4] = {-hh, -hh, hh, hh};
    float xs[4], ys[4];
    for (int i = 0; i < 4; ++i) {
        xs[i] = px(cx) + lx[i] * ca - ly[i] * sa;
        ys[i] = py(cy) + lx[i] * sa + ly[i] * ca;
    }
    const float us[4] = {u0, u1, u1, u0}, vs[4] = {v0, v0, v1, v1};
    const float p[2] = {0, 0}, q[4] = {0, 0, 0, 0};
    quad(xs, ys, us, vs, lx, ly, c, kModeTexture, kTexParts, p, q, emissive);
}

void Renderer::nineSlice(Sprite s, Rect dst, float cornerScale, Rgba tint, Blend blend, bool emissive) {
    const SpriteInfo& i = parts_[s];
    float c[4];
    premul(tint, blend, c);
    float l = i.slice[0] * cornerScale, t = i.slice[1] * cornerScale, r = i.slice[2] * cornerScale, b = i.slice[3] * cornerScale;
    const float fx = std::min(1.0f, dst.w / std::max(l + r, 1e-3f)), fy = std::min(1.0f, dst.h / std::max(t + b, 1e-3f));
    l *= fx; r *= fx; t *= fy; b *= fy;
    const float xs[4] = {dst.x, dst.x + l, dst.right() - r, dst.right()};
    const float ys[4] = {dst.y, dst.y + t, dst.bottom() - b, dst.bottom()};
    const float fxs[4] = {0, i.slice[0] / i.width, 1 - i.slice[2] / i.width, 1};
    const float fys[4] = {0, i.slice[1] / i.height, 1 - i.slice[3] / i.height, 1};
    const float p[2] = {0, 0}, q[4] = {0, 0, 0, 0};
    for (int yy = 0; yy < 3; ++yy)
        for (int xx = 0; xx < 3; ++xx) {
            if (xs[xx + 1] <= xs[xx] || ys[yy + 1] <= ys[yy]) continue;
            float u0, v0, u1, v1;
            spriteUv(s, fxs[xx], fys[yy], fxs[xx + 1], fys[yy + 1], u0, v0, u1, v1);
            rectQuad({px(xs[xx]), py(ys[yy]), (xs[xx + 1] - xs[xx]) * canvas_.scale, (ys[yy + 1] - ys[yy]) * canvas_.scale}, u0, v0, u1, v1,
                     c, kModeTexture, kTexParts, p, q, emissive);
        }
}

void Renderer::arc(float cx, float cy, float radius, float width, float a0, float a1, Rgba col, bool emissive) {
    float c[4];
    premul(col, Blend::Over, c);
    const float R = radius * canvas_.scale, W = width * canvas_.scale, h = R + W * 0.5f + 2.0f;
    const float p[2] = {a0, a1}, q[4] = {R, W, 0, 0};
    rectQuad({px(cx) - h, py(cy) - h, h * 2, h * 2}, 0, 0, 0, 0, c, kModeArc, kTexParts, p, q, emissive, h, h);
}

void Renderer::glow(Rect core, float radius, float spread, Rgba col, bool emissive) {
    float c[4];
    premul(col, Blend::Add, c);
    const float s = canvas_.scale, S = spread * s;
    const float hw = core.w * s * 0.5f + 2.5f * S, hh = core.h * s * 0.5f + 2.5f * S;
    const float p[2] = {S, 0}, q[4] = {core.w * s * 0.5f, core.h * s * 0.5f, std::min(radius * s, std::min(core.w, core.h) * s * 0.5f), 0};
    rectQuad({px(core.cx()) - hw, py(core.cy()) - hh, hw * 2, hh * 2}, 0, 0, 0, 0, c, kModeGlow, kTexParts, p, q, emissive, hw, hh);
}

void Renderer::fillRound(Rect r, float radius, Rgba col, Blend blend, bool emissive) {
    float c[4];
    premul(col, blend, c);
    const float s = canvas_.scale, hw = r.w * s * 0.5f + 1, hh = r.h * s * 0.5f + 1;
    const float p[2] = {1.0f, 0}, q[4] = {r.w * s * 0.5f, r.h * s * 0.5f, std::min(radius * s, std::min(r.w, r.h) * s * 0.5f), 0};
    rectQuad({px(r.cx()) - hw, py(r.cy()) - hh, hw * 2, hh * 2}, 0, 0, 0, 0, c, kModeFill, kTexParts, p, q, emissive, hw, hh);
}

void Renderer::colorQuad(const float xs[4], const float ys[4], const Rgba cols[4], Blend blend, bool emissive) {
    Vertex v[4];
    for (int i = 0; i < 4; ++i) {
        float c[4];
        premul(cols[i], blend, c);
        v[i] = Vertex{px(xs[i]), py(ys[i]), 0, 0, 0, 0, c[0], c[1], c[2], c[3], kModeFlat, kTexParts, 0, 0, 0, 0, 0, 0};
    }
    if (!bloomOnly_) {
        for (int i : {0, 1, 2, 0, 2, 3}) verts_.push_back(v[i]);
        ++stats_.quads;
    }
    if (emissive) for (int i : {0, 1, 2, 0, 2, 3}) bloomVerts_.push_back(v[i]);
}

void Renderer::polyline(const float* xs, const float* ys, int n, float width, Rgba col, Blend blend, bool emissive) {
    const float h = 0.5f * width;
    const Rgba cs[4] = {col, col, col, col};
    for (int i = 0; i + 1 < n; ++i) {
        const float dx = xs[i + 1] - xs[i], dy = ys[i + 1] - ys[i];
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0e-4f) continue;
        const float nx = -dy / len * h, ny = dx / len * h;
        const float ex = dx / len * h * 0.5f, ey = dy / len * h * 0.5f;   // a little overlap closes the joints
        const float qx[4] = {xs[i] + nx - ex, xs[i + 1] + nx + ex, xs[i + 1] - nx + ex, xs[i] - nx - ex};
        const float qy[4] = {ys[i] + ny - ey, ys[i + 1] + ny + ey, ys[i + 1] - ny + ey, ys[i] - ny - ey};
        colorQuad(qx, qy, cs, blend, emissive);
    }
}

float Renderer::textWidth(TextStyle style, std::string_view s) const { return font_ ? font_->measure(style, s) : 0.0f; }
float Renderer::capHeight(TextStyle style) const { return font_ ? font_->capHeight(style) : 0.0f; }

void Renderer::text(TextStyle style, std::string_view s, float x, float baseline, Rgba col, Align align, TextFx fx) {
    if (!font_ || !font_->valid() || s.empty()) return;
    if (align != Align::Left) {
        const float w = font_->measure(style, s);
        x -= (align == Align::Center) ? w * 0.5f : w;
    }
    const float onePx = 1.0f / canvas_.scale;
    const float pad   = static_cast<float>(FontAtlas::kBlurRadius) * onePx;
    const float p[2] = {0, 0}, q[4] = {0, 0, 0, 0};
    auto pass = [&](float dx, float dy, Rgba c, Blend b, float mode, float padding, bool emissive) {
        float lc[4];
        premul(c, b, lc);
        font_->layout(style, s, x + dx, baseline + dy, padding, [&](const GlyphQuad& g) {
            rectQuad({px(g.x0), py(g.y0), (g.x1 - g.x0) * canvas_.scale, (g.y1 - g.y0) * canvas_.scale}, g.s0, g.t0, g.s1, g.t1, lc, mode,
                     kTexParts, p, q, emissive);
        });
    };
    switch (fx) {
    case TextFx::Plain:
        pass(0, 0, col, Blend::Over, kModeText, 0, false);
        break;
    case TextFx::Engraved:
        pass(0, onePx, {1, 1, 1, 0.30f * col.a}, Blend::Over, kModeText, 0, false);
        pass(0, 0, col, Blend::Over, kModeText, 0, false);
        break;
    case TextFx::Shadowed:
        pass(1.5f * onePx, 2.0f * onePx, {0, 0, 0, 0.95f * col.a}, Blend::Over, kModeTextBlur, pad, false);
        pass(0, 0, col, Blend::Over, kModeText, 0, false);
        break;
    case TextFx::Neon:
        pass(0, 0, col.withAlpha(0.85f * col.a), Blend::Add, kModeTextBlur, pad, true);
        pass(0, 0, col, Blend::Over, kModeText, 0, true);
        break;
    }
}

} // namespace hc::ui
