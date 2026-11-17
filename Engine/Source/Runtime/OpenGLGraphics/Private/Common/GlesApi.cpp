#include "GlesApi.h"

#include <Platform/Memory.h>

namespace rocks::graphics::gles {

namespace {

template <class Function>
bool Bind(Function& function, const char* name, GlesProcLoader loader, void* context) noexcept {
    function = reinterpret_cast<Function>(loader(name, context));
    return function != nullptr;
}

} // namespace

bool LoadCoreFunctions(GlesFunctions& gl, GlesProcLoader loader, void* context,
                       const char** missing) noexcept {
#define ROCKS_GLES_LOAD_FUNCTION(ReturnType, Name, Parameters)                                     \
    if (!Bind(gl.Name, "gl" #Name, loader, context)) {                                             \
        if (missing != nullptr) {                                                                  \
            *missing = "gl" #Name;                                                                 \
        }                                                                                          \
        return false;                                                                              \
    }
    ROCKS_GLES_CORE_FUNCTIONS(ROCKS_GLES_LOAD_FUNCTION)
#undef ROCKS_GLES_LOAD_FUNCTION
    return true;
}

bool LoadTimerQueryFunctions(GlesTimerQueryFunctions& out, const GlesTimerQueryNames& names,
                             GlesProcLoader loader, void* context) noexcept {
    GlesTimerQueryFunctions loaded;
    if (!Bind(loaded.QueryCounter, names.queryCounter, loader, context) ||
        !Bind(loaded.GetQueryObjectui64v, names.getQueryObjectui64v, loader, context)) {
        return false;
    }

    out = loaded;
    return true;
}

bool HasGlExtension(const GlesFunctions& gl, const char* name) noexcept {
    GLint count = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint index = 0; index < count; ++index) {
        const auto* extension =
            reinterpret_cast<const char*>(gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(index)));
        if (extension != nullptr && platform::CStringEquals(extension, name)) {
            return true;
        }
    }

    return false;
}

bool HasExtensionToken(const char* list, const char* name) noexcept {
    if (list == nullptr) {
        return false;
    }

    const usize nameBytes = platform::CStringLength(name);
    for (const char* cursor = list; *cursor != '\0';) {
        while (*cursor == ' ') {
            ++cursor;
        }

        const char* end = cursor;
        while (*end != '\0' && *end != ' ') {
            ++end;
        }

        if (static_cast<usize>(end - cursor) == nameBytes &&
            platform::MemoryCompare(cursor, name, nameBytes) == 0) {
            return true;
        }

        cursor = end;
    }

    return false;
}

const char* GlErrorName(GLenum error) noexcept {
    switch (error) {
    case GL_NO_ERROR:
        return "GL_NO_ERROR";
    case GL_INVALID_ENUM:
        return "GL_INVALID_ENUM";
    case GL_INVALID_VALUE:
        return "GL_INVALID_VALUE";
    case GL_INVALID_OPERATION:
        return "GL_INVALID_OPERATION";
    case GL_OUT_OF_MEMORY:
        return "GL_OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION:
        return "GL_INVALID_FRAMEBUFFER_OPERATION";
    case GL_CONTEXT_LOST:
        return "GL_CONTEXT_LOST";
    default:
        return "GL error";
    }
}

} // namespace rocks::graphics::gles
