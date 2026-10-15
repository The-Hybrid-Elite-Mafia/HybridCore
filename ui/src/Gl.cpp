#include "hybrid/ui/Gl.hpp"

#include <cstdint>
#include <cstring>

namespace hc::gl {

#define HC_GL_DEFINE(ret, name, args) PFN_##name name = nullptr;
HC_GL_FUNCS(HC_GL_DEFINE)
#undef HC_GL_DEFINE

namespace {
void* lookup(const char* name) noexcept {
    void* p = reinterpret_cast<void*>(::wglGetProcAddress(name));
    // Some drivers return small sentinel values instead of nullptr for unknown names.
    const auto v = reinterpret_cast<std::intptr_t>(p);
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == -1) {
        static HMODULE gl = ::GetModuleHandleW(L"opengl32.dll");
        p = gl ? reinterpret_cast<void*>(::GetProcAddress(gl, name)) : nullptr;
    }
    return p;
}
} // namespace

const char* loadFunctions() noexcept {
#define HC_GL_LOAD(ret, name, args)                                   \
    name = reinterpret_cast<PFN_##name>(lookup(#name));               \
    if (!name) return #name;
    HC_GL_FUNCS(HC_GL_LOAD)
#undef HC_GL_LOAD
    return nullptr;
}

bool hasExtension(const char* ext) noexcept {
    if (!glGetStringi) return false;
    GLint n = 0;
    ::glGetIntegerv(kNumExtensions, &n);
    for (GLint i = 0; i < n; ++i) {
        const auto* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (e && std::strcmp(e, ext) == 0) return true;
    }
    return false;
}

} // namespace hc::gl
