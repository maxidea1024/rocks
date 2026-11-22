#include <ShaderCompiler/ShaderCompileOptions.generated.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompiler/ShaderCompiler.module.generated.h>

#include <Platform/Memory.h>

// 셰이더 컴파일 옵션(Docs/Design/ShaderSystem.ko.md 11.1절 SC9)의 이름=값 파서다.
namespace rocks::shader {

namespace compile_options_detail {

constexpr StringView Hlsl2021Name("hlsl2021");
constexpr StringView OptimizationName("optimization");
constexpr StringView DebugSymbolsName("debugSymbols");
constexpr StringView MatrixLayoutName("matrixLayout");
constexpr StringView TrueText("true");
constexpr StringView FalseText("false");

// 옵션 종류다(중복 검사의 비트 번호).
enum class OptionKind : uint8 {
    Hlsl2021,
    Optimization,
    DebugSymbols,
    MatrixLayout,
    Unknown
};

struct OptimizationEntry {
    StringView name;
    ShaderOptimization value;
};

constexpr OptimizationEntry OptimizationNames[] = {{"O0", ShaderOptimization::O0},
                                                   {"O1", ShaderOptimization::O1},
                                                   {"O2", ShaderOptimization::O2},
                                                   {"O3", ShaderOptimization::O3}};

constexpr StringView RowLayoutName("Row");

OptionKind KindOf(StringView name) noexcept {
    if (name == Hlsl2021Name) {
        return OptionKind::Hlsl2021;
    }

    if (name == OptimizationName) {
        return OptionKind::Optimization;
    }

    if (name == DebugSymbolsName) {
        return OptionKind::DebugSymbols;
    }

    if (name == MatrixLayoutName) {
        return OptionKind::MatrixLayout;
    }

    return OptionKind::Unknown;
}

bool ParseBool(StringView text, bool& value) noexcept {
    if (text == TrueText) {
        value = true;
        return true;
    }

    if (text == FalseText) {
        value = false;
        return true;
    }

    return false;
}

ShaderOptionStatus Failure(ShaderOptionError code, usize index, StringView name) noexcept {
    ShaderOptionStatus status;
    status.code = code;
    status.index = index;
    const usize bytes =
        name.ByteCount() < sizeof(status.name) - 1 ? name.ByteCount() : sizeof(status.name) - 1;
    if (bytes != 0) {
        platform::MemoryCopy(status.name, name.Data(), bytes);
    }

    status.name[bytes] = '\0';
    return status;
}

} // namespace compile_options_detail

const char* ShaderOptionErrorName(ShaderOptionError code) noexcept {
    switch (code) {
    case ShaderOptionError::None:
        return "None";
    case ShaderOptionError::InvalidSyntax:
        return "InvalidSyntax";
    case ShaderOptionError::UnknownName:
        return "UnknownName";
    case ShaderOptionError::InvalidValue:
        return "InvalidValue";
    case ShaderOptionError::Duplicate:
        return "Duplicate";
    case ShaderOptionError::Unsupported:
        return "Unsupported";
    }

    return "unknown";
}

ShaderCompileOptions DefaultShaderCompileOptions(const ShaderFormatDefinition& format) noexcept {
    ShaderCompileOptions options;
    options.hlsl2021 = format.hlsl2021;
    return options;
}

ShaderOptionStatus ParseShaderCompileOptions(Span<const StringView> assignments,
                                             ShaderCompileOptions& options) noexcept {
    using namespace compile_options_detail;
    ShaderCompileOptions parsed = options;
    uint32 seen = 0;
    for (usize index = 0; index < assignments.Size(); ++index) {
        const StringView assignment = assignments[index];
        StringView name;
        StringView value;
        if (!assignment.Split("=", name, value, StringComparison::CaseSensitive) ||
            name.IsEmpty()) {
            return Failure(ShaderOptionError::InvalidSyntax, index, assignment);
        }

        const OptionKind kind = KindOf(name);
        if (kind == OptionKind::Unknown) {
            return Failure(ShaderOptionError::UnknownName, index, name);
        }

        const uint32 bit = 1u << static_cast<uint32>(kind);
        if ((seen & bit) != 0) {
            return Failure(ShaderOptionError::Duplicate, index, name);
        }

        seen |= bit;
        bool valid = false;
        switch (kind) {
        case OptionKind::Hlsl2021:
            valid = ParseBool(value, parsed.hlsl2021);
            break;
        case OptionKind::DebugSymbols:
            valid = ParseBool(value, parsed.debugSymbols);
            break;
        case OptionKind::Optimization:
            for (const OptimizationEntry& entry : OptimizationNames) {
                if (value == entry.name) {
                    parsed.optimization = entry.value;
                    valid = true;
                }
            }

            break;
        case OptionKind::MatrixLayout:
            valid = value == RowLayoutName;
            parsed.matrixLayout = ShaderMatrixLayout::Row;
            break;
        case OptionKind::Unknown:
            break;
        }

        if (!valid) {
            return Failure(ShaderOptionError::InvalidValue, index, name);
        }

        if (kind == OptionKind::DebugSymbols && parsed.debugSymbols) {
            return Failure(ShaderOptionError::Unsupported, index, name);
        }
    }

    options = parsed;
    return ShaderOptionStatus{};
}

ShaderOptionStatus ValidateShaderCompileOptions(const ShaderCompileOptions& options) noexcept {
    if (options.debugSymbols) {
        return compile_options_detail::Failure(ShaderOptionError::Unsupported, 0,
                                               compile_options_detail::DebugSymbolsName);
    }

    if (static_cast<uint8>(options.optimization) > static_cast<uint8>(ShaderOptimization::O3) ||
        options.matrixLayout != ShaderMatrixLayout::Row) {
        return compile_options_detail::Failure(ShaderOptionError::InvalidValue, 0, StringView());
    }

    return ShaderOptionStatus{};
}

Result<void> RegisterShaderCompilerTypes(ModuleRegistration& registration) noexcept {
    return generated::ShaderCompiler::RegisterModuleTypes(registration);
}

} // namespace rocks::shader
