#include <ShaderCore/ShaderVirtualPath.h>

// 셰이더 가상 경로 문법(Docs/Design/ShaderSystem 11.1절 SC2)이다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace virtual_path_detail {

constexpr StringView EngineRootName("Engine");
constexpr StringView ProjectRootName("Project");
constexpr StringView PluginRootName("Plugin");
constexpr StringView EntryExtension("rsf");
constexpr StringView IncludeExtension("rsh");
constexpr StringView SharedExtension("h");

// 가상 경로 성분 문자다(ASCII [A-Za-z0-9_.-]).
bool IsVirtualPathCharacter(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '.' || value == '-';
}

// 플러그인 이름이다(가상 디렉터리에 '.'이 없어야 하므로 '.'를 뺀다).
bool IsPluginName(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > MaxShaderPluginNameBytes) {
        return false;
    }

    for (const char value : name) {
        if (value == '.' || !IsVirtualPathCharacter(value)) {
            return false;
        }
    }

    return true;
}

// '/' 뒤의 다음 성분을 잘라 낸다. cursor는 성분 시작 위치이며 다음 성분 시작으로 옮긴다.
StringView NextComponent(StringView path, usize& cursor) noexcept {
    const usize start = cursor;
    while (cursor < path.ByteCount() && path.Data()[cursor] != '/') {
        ++cursor;
    }

    const StringView component(path.Data() + start, cursor - start);
    if (cursor < path.ByteCount()) {
        ++cursor;
    }

    return component;
}

// 확장자(마지막 '.' 뒤)다. '.'이 없으면 빈 값이다.
StringView ExtensionOf(StringView fileName) noexcept {
    for (usize index = fileName.ByteCount(); index > 0; --index) {
        if (fileName.Data()[index - 1] == '.') {
            return StringView(fileName.Data() + index, fileName.ByteCount() - index);
        }
    }

    return StringView();
}

ShaderVirtualPathCheck Fail(ShaderVirtualPathError error, const char* reason) noexcept {
    ShaderVirtualPathCheck check;
    check.error = error;
    check.reason = reason;
    return check;
}

} // namespace virtual_path_detail

ShaderVirtualPathCheck CheckShaderVirtualPath(StringView path,
                                              ShaderVirtualPathInfo& info) noexcept {
    using namespace virtual_path_detail;
    info = ShaderVirtualPathInfo{};
    if (path.IsEmpty() || path.Data()[0] != '/') {
        return Fail(ShaderVirtualPathError::InvalidPath, "virtual shader path must start with '/'");
    }

    if (path.ByteCount() >= MaxShaderVirtualPathBytes) {
        ShaderVirtualPathCheck check =
            Fail(ShaderVirtualPathError::InvalidPath, "virtual shader path is too long");
        check.pathTooLong = true;
        return check;
    }

    // 성분을 차례로 본다. 루트 판정에는 앞 두 성분, 확장자에는 마지막 성분, Shared 판정에는 루트
    // 바로 뒤 성분이 필요하다.
    StringView first;
    StringView second;
    StringView third;
    StringView last;
    usize count = 0;
    usize cursor = 1;
    while (true) {
        const StringView component = NextComponent(path, cursor);
        if (component.IsEmpty() || component == StringView(".") || component == StringView("..")) {
            return Fail(ShaderVirtualPathError::InvalidPath,
                        "virtual shader path has an empty, '.' or '..' component");
        }

        for (const char value : component) {
            if (!IsVirtualPathCharacter(value)) {
                return Fail(ShaderVirtualPathError::InvalidPath,
                            "virtual shader path characters are limited to [A-Za-z0-9_.-] and '/'");
            }
        }

        first = count == 0 ? component : first;
        second = count == 1 ? component : second;
        third = count == 2 ? component : third;
        last = component;
        ++count;
        if (cursor >= path.ByteCount()) {
            // 끝 '/'는 빈 성분이다.
            if (path.Data()[path.ByteCount() - 1] == '/') {
                return Fail(ShaderVirtualPathError::InvalidPath,
                            "virtual shader path must name a file");
            }

            break;
        }
    }

    usize rootCount = 0;
    if (first == EngineRootName || first == ProjectRootName) {
        rootCount = 1;
        info.root =
            first == EngineRootName ? ShaderVirtualRoot::Engine : ShaderVirtualRoot::Project;
    } else if (first == PluginRootName && count > 1 && IsPluginName(second)) {
        rootCount = 2;
        info.root = ShaderVirtualRoot::Plugin;
        info.pluginName = second;
    } else {
        return Fail(ShaderVirtualPathError::InvalidPath,
                    "virtual shader path must be under /Engine, /Project or /Plugin/<name>");
    }

    if (count <= rootCount) {
        return Fail(ShaderVirtualPathError::InvalidPath,
                    "virtual shader path must name a file under its root");
    }

    const StringView afterRoot = rootCount == 1 ? second : third;
    info.shared = count > rootCount + 1 && afterRoot == StringView(SharedShaderDirectoryName);
    info.extension = ExtensionOf(last);
    if (info.shared) {
        if (info.extension != SharedExtension) {
            return Fail(ShaderVirtualPathError::InvalidExtension,
                        "only .h files are allowed under a Shared shader directory");
        }
    } else if (info.extension != EntryExtension && info.extension != IncludeExtension) {
        return Fail(ShaderVirtualPathError::InvalidExtension,
                    "shader source files must end with .rsf or .rsh");
    }

    return ShaderVirtualPathCheck{};
}

ShaderVirtualPathCheck CheckShaderVirtualDirectory(StringView directory) noexcept {
    using namespace virtual_path_detail;
    if (directory == StringView(EngineShaderVirtualRoot) ||
        directory == StringView(ProjectShaderVirtualRoot)) {
        return ShaderVirtualPathCheck{};
    }

    const StringView prefix(PluginShaderVirtualPrefix);
    if (directory.StartsWith(prefix, StringComparison::CaseSensitive) &&
        IsPluginName(StringView(directory.Data() + prefix.ByteCount(),
                                directory.ByteCount() - prefix.ByteCount()))) {
        return ShaderVirtualPathCheck{};
    }

    return Fail(ShaderVirtualPathError::InvalidDirectory,
                "virtual shader directory must be /Engine, /Project or /Plugin/<name> with "
                "[A-Za-z0-9_-] names and no trailing '/'");
}

} // namespace rocks::shader
