#include <ShaderCompiler/ShaderPreprocessDefines.h>

#include "ShaderSourceStatusInternal.h"

// 전처리 정의 조립(Docs/Design/ShaderSystem 11.1절 SC10)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::shader {

namespace preprocess_defines_detail {

constexpr StringView ReservedPrefix("ROCKS_");
constexpr StringView CompilerReservedPrefix("__");
constexpr StringView FormatPrefix("ROCKS_FORMAT_");
// 셰이더 모델 숫자 값의 최대 자릿수(uint8)다.
constexpr usize MaxModelDigits = 3;

StringView BackendDefine(ShaderBackend backend) noexcept {
    switch (backend) {
    case ShaderBackend::D3D12:
        return "ROCKS_BACKEND_D3D12";
    case ShaderBackend::D3D11:
        return "ROCKS_BACKEND_D3D11";
    case ShaderBackend::Vulkan:
        return "ROCKS_BACKEND_VULKAN";
    case ShaderBackend::Metal:
        return "ROCKS_BACKEND_METAL";
    case ShaderBackend::OpenGL:
        return "ROCKS_BACKEND_GLES";
    }

    return StringView();
}

// 1차 경로의 컴파일러다(D3D11_SM5의 대체 경로도 FXC 정의로 전처리한다 — SC10).
StringView CompilerDefine(ShaderCompilerPath path) noexcept {
    return path == ShaderCompilerPath::Fxc ? StringView("ROCKS_COMPILER_FXC")
                                           : StringView("ROCKS_COMPILER_DXC");
}

StringView StageDefine(uint32 stage) noexcept {
    switch (stage) {
    case ShaderStageVertex:
        return "ROCKS_VERTEX_SHADER";
    case ShaderStagePixel:
        return "ROCKS_PIXEL_SHADER";
    case ShaderStageCompute:
        return "ROCKS_COMPUTE_SHADER";
    default:
        return StringView();
    }
}

bool IsIdentifier(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > MaxShaderDefineNameBytes) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char value = name.Data()[index];
        const bool letter =
            (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
        const bool digit = value >= '0' && value <= '9';
        if (!letter && !(digit && index != 0)) {
            return false;
        }
    }

    return true;
}

// 0–255 정수의 10진 표기다.
StringView Decimal(uint8 value, char (&storage)[MaxModelDigits]) noexcept {
    usize count = 0;
    char reversed[MaxModelDigits];
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value = static_cast<uint8>(value / 10);
    } while (value != 0);

    for (usize index = 0; index < count; ++index) {
        storage[index] = reversed[count - 1 - index];
    }

    return StringView(storage, count);
}

} // namespace preprocess_defines_detail

StringView ShaderDefineSet::Find(StringView name, bool& found) const noexcept {
    for (const ShaderDefine& define : defines_) {
        if (define.name == name) {
            found = true;
            return define.value;
        }
    }

    found = false;
    return StringView();
}

void ShaderDefineSet::Reset() noexcept {
    text_.Clear();
    entries_.Clear();
    defines_.Clear();
}

ShaderSourceStatus BuildShaderPreprocessDefines(const ShaderFormatDefinition& format, uint32 stage,
                                                bool hlsl2021, Span<const ShaderDefine> jobDefines,
                                                ShaderDefineSet& out) noexcept {
    using namespace preprocess_defines_detail;
    using source_detail::MakeSourceStatus;
    out.Reset();
    const StringView stageName = StageDefine(stage);
    if (format.state != ShaderFormatState::Supported || stageName.IsEmpty() ||
        (format.stages & stage) == 0) {
        return MakeSourceStatus(ShaderSourceError::UnsupportedStage, StringView(), 0,
                                "shader format does not compile this stage", format.Id());
    }

    for (usize index = 0; index < jobDefines.Size(); ++index) {
        const StringView name = jobDefines[index].name;
        if (name.StartsWith(ReservedPrefix, StringComparison::CaseSensitive) ||
            name.StartsWith(CompilerReservedPrefix, StringComparison::CaseSensitive)) {
            return MakeSourceStatus(ShaderSourceError::ReservedDefine, StringView(), 0,
                                    "job define names cannot start with ROCKS_ or __", name);
        }

        if (!IsIdentifier(name)) {
            return MakeSourceStatus(ShaderSourceError::InvalidDefine, StringView(), 0,
                                    "job define name must be an identifier", name);
        }

        for (const char value : jobDefines[index].value) {
            if (value == '\n' || value == '\r') {
                return MakeSourceStatus(ShaderSourceError::InvalidDefine, StringView(), 0,
                                        "job define value cannot contain a line break", name);
            }
        }

        for (usize other = 0; other < index; ++other) {
            if (jobDefines[other].name == name) {
                return MakeSourceStatus(ShaderSourceError::DuplicateDefine, StringView(), 0,
                                        "job define is given twice", name);
            }
        }
    }

    char major[MaxModelDigits];
    char minor[MaxModelDigits];
    bool memoryOk = true;
    const auto add = [&out, &memoryOk](StringView prefix, StringView name,
                                       StringView value) noexcept {
        ShaderDefineSet::Entry entry;
        entry.nameOffset = out.text_.ByteCount();
        entry.nameBytes = prefix.ByteCount() + name.ByteCount();
        entry.valueOffset = entry.nameOffset + entry.nameBytes;
        entry.valueBytes = value.ByteCount();
        memoryOk = memoryOk && static_cast<bool>(out.text_.TryAppend(prefix)) &&
                   static_cast<bool>(out.text_.TryAppend(name)) &&
                   static_cast<bool>(out.text_.TryAppend(value)) &&
                   static_cast<bool>(out.entries_.TryPushBack(entry));
    };
    add(FormatPrefix, format.Id(), "1");
    add(StringView(), BackendDefine(format.backend), "1");
    add(StringView(), CompilerDefine(format.compiler.path), "1");
    add(StringView(), "ROCKS_SHADER_MODEL_MAJOR",
        Decimal(format.compiler.shaderModel.major, major));
    add(StringView(), "ROCKS_SHADER_MODEL_MINOR",
        Decimal(format.compiler.shaderModel.minor, minor));
    add(StringView(), "ROCKS_HLSL2021", hlsl2021 ? StringView("1") : StringView("0"));
    add(StringView(), stageName, "1");
    for (const ShaderDefine& define : jobDefines) {
        add(StringView(), define.name, define.value);
    }

    // 문자열이 다 자란 뒤에 view를 만든다(자라는 동안 주소가 바뀐다).
    for (const ShaderDefineSet::Entry& entry : out.entries_) {
        ShaderDefine define;
        define.name = StringView(out.text_.Data() + entry.nameOffset, entry.nameBytes);
        define.value = StringView(out.text_.Data() + entry.valueOffset, entry.valueBytes);
        memoryOk = memoryOk && static_cast<bool>(out.defines_.TryPushBack(define));
    }

    if (!memoryOk) {
        out.Reset();
        return MakeSourceStatus(ShaderSourceError::OutOfMemory, StringView(), 0, "out of memory");
    }

    out.defines_.Sort([](const ShaderDefine& left, const ShaderDefine& right) noexcept {
        return StringView::CompareBytes(left.name, right.name) < 0;
    });
    return ShaderSourceStatus{};
}

} // namespace rocks::shader
