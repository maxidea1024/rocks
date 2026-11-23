#include <ShaderCompilerCommon/ShaderFormatDefinition.h>

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <stdio.h>
#include <string.h>

// 형식 정의·엔진 컴파일 프로필 읽기(Docs/Design/ShaderSystem.ko.md 12.1절)다. 모든 속성은 이름 표로
// 허용 목록을 두고, 표 밖 속성·어휘는 오류로 보고한다(첫 오류에서 멈춘다).
namespace rocks::shader {

namespace {

// 개수 상한 값의 범위다(오타로 들어온 큰 값을 거부한다).
constexpr uint32 MaxResourceLimit = 4096;
// GLES 에뮬레이션 vec4 상한 값의 범위다.
constexpr uint32 MaxUniformVectors = 65536;
// DXC가 받는 셰이더 모델 6.x의 minor 상한이다(6.0–6.9).
constexpr uint8 MaxShaderModel6Minor = 9;
// GLES 형식의 ESSL 버전(2절, `#version 320 es`)이다.
constexpr uint32 EsslVersion320 = 320;
// 형식 정의 파일 크기 상한(JSON 읽기 전 거부)이다.
constexpr uint64 MaxDefinitionFileBytes = 1u << 20;
// 경로 버퍼 크기다(디렉터리 + "/" + id + ".json").
constexpr usize MaxFilePathBytes = 1024;

template <typename T>
struct NameEntry {
    const char* name;
    T value;
};

constexpr NameEntry<ShaderBackend> BackendNames[] = {{"D3D12", ShaderBackend::D3D12},
                                                     {"D3D11", ShaderBackend::D3D11},
                                                     {"Vulkan", ShaderBackend::Vulkan},
                                                     {"Metal", ShaderBackend::Metal},
                                                     {"OpenGL", ShaderBackend::OpenGL}};

constexpr NameEntry<ShaderFormatState> StateNames[] = {
    {"supported", ShaderFormatState::Supported}, {"placeholder", ShaderFormatState::Placeholder}};

constexpr NameEntry<ShaderFormatModule> ModuleNames[] = {
    {"ShaderFormatD3D", ShaderFormatModule::D3D},
    {"ShaderFormatVulkan", ShaderFormatModule::Vulkan},
    {"ShaderFormatMetal", ShaderFormatModule::Metal},
    {"ShaderFormatOpenGL", ShaderFormatModule::OpenGL}};

// "path"에 올 수 있는 주 경로다. DxcSpirvHlsl50Fxc는 "fallbackPath"에만 온다.
constexpr NameEntry<ShaderCompilerPath> PathNames[] = {
    {"DxcDxil", ShaderCompilerPath::DxcDxil},
    {"Fxc", ShaderCompilerPath::Fxc},
    {"DxcSpirv", ShaderCompilerPath::DxcSpirv},
    {"DxcSpirvMsl", ShaderCompilerPath::DxcSpirvMsl},
    {"DxcSpirvEssl", ShaderCompilerPath::DxcSpirvEssl}};

constexpr NameEntry<ShaderCompilerPath> FallbackPathNames[] = {
    {"DxcSpirvHlsl50Fxc", ShaderCompilerPath::DxcSpirvHlsl50Fxc}};

constexpr NameEntry<ImageFormatExpansion> ImageFormatExpansionNames[] = {
    {"Empty", ImageFormatExpansion::Empty}, {"VkImageFormat", ImageFormatExpansion::VkImageFormat}};

constexpr NameEntry<SpirvTargetEnv> TargetEnvNames[] = {{"vulkan1.1", SpirvTargetEnv::Vulkan1_1}};

constexpr NameEntry<MslPlatform> MslPlatformNames[] = {{"macos", MslPlatform::MacOS}};

constexpr NameEntry<ShaderHost> HostNames[] = {{"windows-x64", ShaderHost::WindowsX64},
                                               {"macos-arm64", ShaderHost::MacosArm64}};

constexpr NameEntry<uint32> StageNames[] = {
    {"vertex", ShaderStageVertex}, {"pixel", ShaderStagePixel}, {"compute", ShaderStageCompute}};

constexpr NameEntry<uint32> FeatureNames[] = {
    {"compute", ShaderFeatureCompute},
    {"waveOperations", ShaderFeatureWaveOperations},
    {"vertexViewportLayerOutput", ShaderFeatureVertexViewportLayerOutput}};

constexpr NameEntry<uint32> DeviceRequirementNames[] = {
    {"D3D_FEATURE_LEVEL_11_0", DeviceRequiresD3DFeatureLevel11_0},
    {"D3D12_SHADER_MODEL_6_6", DeviceRequiresD3D12ShaderModel6_6},
    {"VULKAN_1_1", DeviceRequiresVulkan1_1},
    {"OPENGL_4_3", DeviceRequiresOpenGL4_3},
    {"OPENGL_ES_3_2", DeviceRequiresOpenGLES3_2},
    {"GL_ARB_ES3_2_compatibility", DeviceRequiresGlArbEs32Compatibility},
    {"GL_ARB_clip_control", DeviceRequiresGlArbClipControl}};

// DXC [[vk::image_format]] 철자(업스트림 DXC SPIR-V 문서의 지원 목록)다. 색인이 비트 번호다.
constexpr const char* ImageFormatNames[] = {
    "rgba32f",    "rgba16f",     "r32f",      "rgba8",    "rgba8snorm", "rg32f",     "rg16f",
    "r11g11b10f", "r16f",        "rgba16",    "rgb10a2",  "rg16",       "rg8",       "r16",
    "r8",         "rgba16snorm", "rg16snorm", "rg8snorm", "r16snorm",   "r8snorm",   "rgba32i",
    "rgba16i",    "rgba8i",      "r32i",      "rg32i",    "rg16i",      "rg8i",      "r16i",
    "r8i",        "rgba32ui",    "rgba16ui",  "rgba8ui",  "r32ui",      "rgb10a2ui", "rg32ui",
    "rg16ui",     "rg8ui",       "r16ui",     "r8ui"};

constexpr usize ImageFormatCount = sizeof(ImageFormatNames) / sizeof(ImageFormatNames[0]);
static_assert(ImageFormatCount <= 64, "unsupportedImageFormats is a 64-bit mask");

template <typename T, usize N>
constexpr Span<const NameEntry<T>> Names(const NameEntry<T> (&table)[N]) noexcept {
    return Span<const NameEntry<T>>(table, N);
}

template <typename T>
const NameEntry<T>* FindName(Span<const NameEntry<T>> table, StringView name) noexcept {
    for (const NameEntry<T>& entry : table) {
        if (StringView(entry.name) == name) {
            return &entry;
        }
    }

    return nullptr;
}

template <typename T>
const char* NameOf(Span<const NameEntry<T>> table, T value) noexcept {
    for (const NameEntry<T>& entry : table) {
        if (entry.value == value) {
            return entry.name;
        }
    }

    return "unknown";
}

bool IsIdCharacter(char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
}

// 첫 오류를 기억하며 JSON 값을 읽는다. 오류 뒤 호출은 아무 일도 하지 않고 false를 돌려준다.
class Reader final {
public:
    explicit Reader(ShaderFormatStatus& status) noexcept : status_(status) {
    }

    bool Ok() const noexcept {
        return status_.code == ShaderFormatErrorCode::None;
    }

    // prefix.key(prefix가 비면 key)를 오류 위치로 남긴다. at이 유효하면 그 줄을 남긴다.
    bool Fail(ShaderFormatErrorCode code, StringView prefix, StringView key,
              JsonValue at = {}) noexcept {
        if (!Ok()) {
            return false;
        }

        status_.code = code;
        if (prefix.ByteCount() == 0) {
            snprintf(status_.path, sizeof(status_.path), "%.*s", static_cast<int>(key.ByteCount()),
                     key.Data());
        } else {
            snprintf(status_.path, sizeof(status_.path), "%.*s.%.*s",
                     static_cast<int>(prefix.ByteCount()), prefix.Data(),
                     static_cast<int>(key.ByteCount()), key.Data());
        }

        status_.line = at.IsValid() ? at.Position().line : 0;
        return false;
    }

    // object의 속성이 모두 allowed에 있는지 본다.
    bool CheckKeys(JsonValue object, Span<const char* const> allowed, StringView prefix) noexcept {
        if (!Ok()) {
            return false;
        }

        for (usize index = 0; index < object.Size(); ++index) {
            const StringView key = object.KeyAt(index);
            bool known = false;
            for (const char* name : allowed) {
                known = known || StringView(name) == key;
            }

            if (!known) {
                return Fail(ShaderFormatErrorCode::UnknownProperty, prefix, key,
                            object.ValueAt(index));
            }
        }

        return true;
    }

    // 필수 속성 값이다. 없으면 MissingProperty이고 무효 값을 돌려준다.
    JsonValue Require(JsonValue object, StringView prefix, const char* key) noexcept {
        if (!Ok()) {
            return {};
        }

        const JsonValue value = object.Find(key);
        if (!value.IsValid()) {
            Fail(ShaderFormatErrorCode::MissingProperty, prefix, key, object);
        }

        return value;
    }

    JsonValue RequireObject(JsonValue object, StringView prefix, const char* key) noexcept {
        const JsonValue value = Require(object, prefix, key);
        if (value.IsValid() && !value.IsObject()) {
            Fail(ShaderFormatErrorCode::WrongType, prefix, key, value);
            return {};
        }

        return value;
    }

    bool ReadUInt(JsonValue value, StringView prefix, const char* key, uint32 low, uint32 high,
                  uint32& out) noexcept {
        if (!Ok() || !value.IsValid()) {
            return false;
        }

        if (!value.IsNumber() || value.Type() == JsonType::Double) {
            return Fail(ShaderFormatErrorCode::WrongType, prefix, key, value);
        }

        const Result<uint64> number = value.TryGetUInt64();
        if (!number || number.Value() < low || number.Value() > high) {
            return Fail(ShaderFormatErrorCode::InvalidValue, prefix, key, value);
        }

        out = static_cast<uint32>(number.Value());
        return true;
    }

    bool ReadUInt(JsonValue object, StringView prefix, const char* key, uint32 low, uint32 high,
                  uint32& out, bool required) noexcept {
        if (!required && !object.Find(key).IsValid()) {
            return Ok();
        }

        return ReadUInt(Require(object, prefix, key), prefix, key, low, high, out);
    }

    bool ReadBool(JsonValue object, StringView prefix, const char* key, bool& out) noexcept {
        const JsonValue value = Require(object, prefix, key);
        if (!value.IsValid()) {
            return false;
        }

        if (!value.IsBool()) {
            return Fail(ShaderFormatErrorCode::WrongType, prefix, key, value);
        }

        out = value.GetBool();
        return true;
    }

    bool ReadString(JsonValue value, StringView prefix, StringView key, StringView& out) noexcept {
        if (!Ok() || !value.IsValid()) {
            return false;
        }

        if (!value.IsString()) {
            return Fail(ShaderFormatErrorCode::WrongType, prefix, key, value);
        }

        out = value.GetString();
        return true;
    }

    template <typename T>
    bool ReadName(JsonValue object, StringView prefix, const char* key,
                  Span<const NameEntry<T>> table, T& out) noexcept {
        StringView text;
        const JsonValue value = Require(object, prefix, key);
        if (!ReadString(value, prefix, key, text)) {
            return false;
        }

        const NameEntry<T>* found = FindName(table, text);
        if (found == nullptr) {
            return Fail(ShaderFormatErrorCode::InvalidValue, prefix, key, value);
        }

        out = found->value;
        return true;
    }

    // 문자열 배열을 비트 집합으로 읽는다. 같은 이름이 두 번이면 DuplicateValue다.
    bool ReadBits(JsonValue object, StringView prefix, const char* key,
                  Span<const NameEntry<uint32>> table, uint32& out) noexcept {
        out = 0;
        const JsonValue array = Require(object, prefix, key);
        if (!array.IsValid()) {
            return false;
        }

        if (!array.IsArray()) {
            return Fail(ShaderFormatErrorCode::WrongType, prefix, key, array);
        }

        for (usize index = 0; index < array.Size(); ++index) {
            StringView text;
            const JsonValue element = array.At(index);
            if (!ReadString(element, prefix, key, text)) {
                return false;
            }

            const NameEntry<uint32>* found = FindName(table, text);
            if (found == nullptr) {
                return Fail(ShaderFormatErrorCode::InvalidValue, prefix, key, element);
            }

            if ((out & found->value) != 0) {
                return Fail(ShaderFormatErrorCode::DuplicateValue, prefix, key, element);
            }

            out |= found->value;
        }

        return true;
    }

    // "6_6"·"2.4"처럼 한 자리 major, separator, 한두 자리 minor다.
    bool ReadVersion(JsonValue object, StringView prefix, const char* key, char separator,
                     ShaderVersion& out) noexcept {
        StringView text;
        const JsonValue value = Require(object, prefix, key);
        if (!ReadString(value, prefix, key, text)) {
            return false;
        }

        const char* data = text.Data();
        const usize size = text.ByteCount();
        auto digit = [](char character) { return character >= '0' && character <= '9'; };
        const bool shaped = (size == 3 || size == 4) && digit(data[0]) && data[1] == separator &&
                            digit(data[2]) && (size == 3 || digit(data[3]));
        if (!shaped) {
            return Fail(ShaderFormatErrorCode::InvalidValue, prefix, key, value);
        }

        out.major = static_cast<uint8>(data[0] - '0');
        out.minor = static_cast<uint8>(data[2] - '0');
        if (size == 4) {
            out.minor = static_cast<uint8>(out.minor * 10 + (data[3] - '0'));
        }

        return true;
    }

    // id 철자(ASCII 영숫자·'_'·'-', 1 byte 이상, 용량 안)를 out에 복사한다.
    bool ReadId(JsonValue value, StringView prefix, StringView key,
                char (&out)[MaxShaderFormatIdBytes]) noexcept {
        StringView text;
        if (!ReadString(value, prefix, key, text)) {
            return false;
        }

        bool valid = text.ByteCount() > 0 && text.ByteCount() < MaxShaderFormatIdBytes;
        for (usize index = 0; valid && index < text.ByteCount(); ++index) {
            valid = IsIdCharacter(text.Data()[index]);
        }

        if (!valid) {
            return Fail(ShaderFormatErrorCode::InvalidValue, prefix, key, value);
        }

        memcpy(out, text.Data(), text.ByteCount());
        out[text.ByteCount()] = '\0';
        return true;
    }

private:
    ShaderFormatStatus& status_;
};

// 도구 이름 배열을 도구 표 항목으로 읽는다.
bool ReadTools(Reader& reader, JsonValue object, StringView prefix, const char* key,
               Span<const ShaderTool> table, const ShaderTool* (&out)[MaxShaderFormatTools],
               usize& count) noexcept {
    count = 0;
    const JsonValue array = reader.Require(object, prefix, key);
    if (!array.IsValid()) {
        return false;
    }

    if (!array.IsArray()) {
        return reader.Fail(ShaderFormatErrorCode::WrongType, prefix, key, array);
    }

    for (usize index = 0; index < array.Size(); ++index) {
        StringView name;
        const JsonValue element = array.At(index);
        if (!reader.ReadString(element, prefix, key, name)) {
            return false;
        }

        const ShaderTool* found = nullptr;
        for (const ShaderTool& tool : table) {
            if (StringView(tool.name) == name) {
                found = &tool;
            }
        }

        if (found == nullptr) {
            return reader.Fail(ShaderFormatErrorCode::UnknownTool, prefix, key, element);
        }

        for (usize seen = 0; seen < count; ++seen) {
            if (out[seen] == found) {
                return reader.Fail(ShaderFormatErrorCode::DuplicateValue, prefix, key, element);
            }
        }

        if (count == MaxShaderFormatTools) {
            return reader.Fail(ShaderFormatErrorCode::TooMany, prefix, key, element);
        }

        out[count++] = found;
    }

    return true;
}

bool HasTool(const ShaderFormatDefinition& definition, const char* name) noexcept {
    for (usize index = 0; index < definition.toolCount; ++index) {
        if (platform::CStringEquals(definition.tools[index]->name, name)) {
            return true;
        }
    }

    return false;
}

bool ReadImageFormats(Reader& reader, JsonValue object, StringView prefix, const char* key,
                      uint64& out) noexcept {
    out = 0;
    const JsonValue array = object.Find(key);
    if (!array.IsValid()) {
        return reader.Ok();
    }

    if (!array.IsArray()) {
        return reader.Fail(ShaderFormatErrorCode::WrongType, prefix, key, array);
    }

    for (usize index = 0; index < array.Size(); ++index) {
        StringView name;
        const JsonValue element = array.At(index);
        if (!reader.ReadString(element, prefix, key, name)) {
            return false;
        }

        const usize bit = ImageFormatIndex(name);
        if (bit == IndexNone) {
            return reader.Fail(ShaderFormatErrorCode::InvalidValue, prefix, key, element);
        }

        const uint64 mask = uint64{1} << bit;
        if ((out & mask) != 0) {
            return reader.Fail(ShaderFormatErrorCode::DuplicateValue, prefix, key, element);
        }

        out |= mask;
    }

    return true;
}

// "compiler" 객체다. 경로마다 쓰는 속성만 받고 모두 필수다(unsupportedImageFormats만 선택).
bool ReadCompiler(Reader& reader, JsonValue compiler, ShaderFormatCompiler& out) noexcept {
    constexpr StringView Prefix = "compiler";
    if (!reader.ReadName(compiler, Prefix, "path", Names(PathNames), out.path)) {
        return false;
    }

    static const char* const DxilKeys[] = {"path", "shaderModel", "imageFormat"};
    static const char* const FxcKeys[] = {"path", "shaderModel", "imageFormat", "fallbackPath",
                                          "spirvTargetEnv"};
    static const char* const SpirvKeys[] = {"path", "shaderModel", "imageFormat", "spirvTargetEnv",
                                            "dxPositionW"};
    static const char* const MslKeys[] = {"path",           "shaderModel", "imageFormat",
                                          "spirvTargetEnv", "dxPositionW", "mslVersion",
                                          "mslPlatform"};
    static const char* const EsslKeys[] = {
        "path",           "shaderModel", "imageFormat", "unsupportedImageFormats",
        "spirvTargetEnv", "dxPositionW", "esslVersion", "fixupClipSpace",
        "flipVertexY"};
    Span<const char* const> allowed(DxilKeys);
    switch (out.path) {
    case ShaderCompilerPath::Fxc:
        allowed = Span<const char* const>(FxcKeys);
        break;
    case ShaderCompilerPath::DxcSpirv:
        allowed = Span<const char* const>(SpirvKeys);
        break;
    case ShaderCompilerPath::DxcSpirvMsl:
        allowed = Span<const char* const>(MslKeys);
        break;
    case ShaderCompilerPath::DxcSpirvEssl:
        allowed = Span<const char* const>(EsslKeys);
        break;
    default:
        break;
    }

    if (!reader.CheckKeys(compiler, allowed, Prefix) ||
        !reader.ReadVersion(compiler, Prefix, "shaderModel", '_', out.shaderModel) ||
        !reader.ReadName(compiler, Prefix, "imageFormat", Names(ImageFormatExpansionNames),
                         out.imageFormat)) {
        return false;
    }

    // D3D 경로는 SM 5.0(FXC) 또는 6.x(DXC), SPIR-V 경로는 DXC 6.x 프로필이다.
    const bool fxc = out.path == ShaderCompilerPath::Fxc;
    const ShaderVersion model = out.shaderModel;
    const bool modelValid = fxc ? model == ShaderVersion{5, 0}
                                : model.major == 6 && model.minor <= MaxShaderModel6Minor;
    if (!modelValid) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, Prefix, "shaderModel",
                           compiler.Find("shaderModel"));
    }

    // SH0: DXC는 [[vk::image_format]]을 SPIR-V에만 쓰고 DXIL에서는 무시하며 경고를 낸다.
    const bool d3dPath = fxc || out.path == ShaderCompilerPath::DxcDxil;
    const ImageFormatExpansion expected =
        d3dPath ? ImageFormatExpansion::Empty : ImageFormatExpansion::VkImageFormat;
    if (out.imageFormat != expected) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, Prefix, "imageFormat",
                           compiler.Find("imageFormat"));
    }

    if (out.path == ShaderCompilerPath::DxcDxil) {
        return true;
    }

    if (!reader.ReadName(compiler, Prefix, "spirvTargetEnv", Names(TargetEnvNames),
                         out.spirvTargetEnv)) {
        return false;
    }

    if (fxc) {
        return reader.ReadName(compiler, Prefix, "fallbackPath", Names(FallbackPathNames),
                               out.fallbackPath);
    }

    if (!reader.ReadBool(compiler, Prefix, "dxPositionW", out.dxPositionW)) {
        return false;
    }

    if (out.path == ShaderCompilerPath::DxcSpirvMsl) {
        if (!reader.ReadVersion(compiler, Prefix, "mslVersion", '.', out.mslVersion) ||
            !reader.ReadName(compiler, Prefix, "mslPlatform", Names(MslPlatformNames),
                             out.mslPlatform)) {
            return false;
        }

        if (out.mslVersion.major < 2) {
            return reader.Fail(ShaderFormatErrorCode::InvalidValue, Prefix, "mslVersion",
                               compiler.Find("mslVersion"));
        }
    }

    if (out.path == ShaderCompilerPath::DxcSpirvEssl) {
        return reader.ReadUInt(compiler, Prefix, "esslVersion", EsslVersion320, EsslVersion320,
                               out.esslVersion, true) &&
               reader.ReadBool(compiler, Prefix, "fixupClipSpace", out.fixupClipSpace) &&
               reader.ReadBool(compiler, Prefix, "flipVertexY", out.flipVertexY) &&
               ReadImageFormats(reader, compiler, Prefix, "unsupportedImageFormats",
                                out.unsupportedImageFormats);
    }

    return true;
}

bool ReadLimits(Reader& reader, JsonValue limits, bool emulatedUniforms,
                ShaderFormatLimits& out) noexcept {
    constexpr StringView Prefix = "limits";
    static const char* const Keys[] = {"constantBuffers",   "sampledTextures", "storageBuffers",
                                       "storageImages",     "samplers",        "uavTotal",
                                       "uavStages",         "renderTargets",   "interStageVectors",
                                       "rootConstantBytes", "uniformVectors"};
    if (!reader.CheckKeys(limits, Span<const char* const>(Keys), Prefix) ||
        !reader.ReadUInt(limits, Prefix, "constantBuffers", 0, MaxResourceLimit,
                         out.constantBuffers, true) ||
        !reader.ReadUInt(limits, Prefix, "sampledTextures", 0, MaxResourceLimit,
                         out.sampledTextures, true) ||
        !reader.ReadUInt(limits, Prefix, "storageBuffers", 0, MaxResourceLimit, out.storageBuffers,
                         true) ||
        !reader.ReadUInt(limits, Prefix, "storageImages", 0, MaxResourceLimit, out.storageImages,
                         true) ||
        !reader.ReadUInt(limits, Prefix, "samplers", 0, MaxResourceLimit, out.samplers, true) ||
        !reader.ReadUInt(limits, Prefix, "uavTotal", 1, MaxResourceLimit, out.uavTotal, false) ||
        !reader.ReadBits(limits, Prefix, "uavStages", Names(StageNames), out.uavStages) ||
        !reader.ReadUInt(limits, Prefix, "renderTargets", 1, MaxRenderTargets, out.renderTargets,
                         true) ||
        !reader.ReadUInt(limits, Prefix, "interStageVectors", 1, MaxInterStageVectors,
                         out.interStageVectors, true) ||
        !reader.ReadUInt(limits, Prefix, "rootConstantBytes", 0, MaxRootConstantBytes,
                         out.rootConstantBytes, true)) {
        return false;
    }

    // 루트 상수는 32-bit 상수 단위다(D3D12 root constants, Vulkan push constant 4 byte 정렬).
    if (out.rootConstantBytes % 4 != 0) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, Prefix, "rootConstantBytes",
                           limits.Find("rootConstantBytes"));
    }

    // uniformVectors는 GLES 상수 버퍼 에뮬레이션(OpenGL backend)에만 있고 거기서는 필수다.
    const JsonValue uniforms = limits.Find("uniformVectors");
    if (!emulatedUniforms) {
        if (uniforms.IsValid()) {
            return reader.Fail(ShaderFormatErrorCode::UnknownProperty, Prefix, "uniformVectors",
                               uniforms);
        }

        return true;
    }

    const JsonValue object = reader.RequireObject(limits, Prefix, "uniformVectors");
    if (!object.IsValid()) {
        return false;
    }

    constexpr StringView UniformPrefix = "limits.uniformVectors";
    static const char* const StageKeys[] = {"vertex", "pixel", "compute"};
    if (!reader.CheckKeys(object, Span<const char* const>(StageKeys), UniformPrefix)) {
        return false;
    }

    for (usize stage = 0; stage < 3; ++stage) {
        if (!reader.ReadUInt(object, UniformPrefix, StageKeys[stage], 1, MaxUniformVectors,
                             out.uniformVectors[stage], true)) {
            return false;
        }
    }

    return true;
}

bool ReadProducers(Reader& reader, JsonValue root, Span<const ShaderTool> table,
                   ShaderFormatDefinition& out) noexcept {
    constexpr StringView Prefix = "producers";
    const JsonValue array = reader.Require(root, "", "producers");
    if (!array.IsValid()) {
        return false;
    }

    if (!array.IsArray() || array.Size() == 0) {
        return reader.Fail(array.IsArray() ? ShaderFormatErrorCode::InvalidValue
                                           : ShaderFormatErrorCode::WrongType,
                           "", "producers", array);
    }

    for (usize index = 0; index < array.Size(); ++index) {
        const JsonValue producer = array.At(index);
        if (!producer.IsObject()) {
            return reader.Fail(ShaderFormatErrorCode::WrongType, "", "producers", producer);
        }

        if (out.producerCount == MaxShaderFormatProducers) {
            return reader.Fail(ShaderFormatErrorCode::TooMany, "", "producers", producer);
        }

        static const char* const Keys[] = {"host", "systemTools"};
        ShaderFormatProducer& entry = out.producers[out.producerCount];
        if (!reader.CheckKeys(producer, Span<const char* const>(Keys), Prefix) ||
            !reader.ReadName(producer, Prefix, "host", Names(HostNames), entry.host)) {
            return false;
        }

        for (usize seen = 0; seen < out.producerCount; ++seen) {
            if (out.producers[seen].host == entry.host) {
                return reader.Fail(ShaderFormatErrorCode::DuplicateValue, Prefix, "host",
                                   producer.Find("host"));
            }
        }

        if (producer.Find("systemTools").IsValid()) {
            if (!ReadTools(reader, producer, Prefix, "systemTools", table, entry.systemTools,
                           entry.systemToolCount)) {
                return false;
            }

            // system 도구는 공유 라이브러리로 열지 않는 도구(Apple metal)이며 형식 도구 목록에도
            // 있어야 한다(job 키 3항).
            for (usize tool = 0; tool < entry.systemToolCount; ++tool) {
                const ShaderTool* systemTool = entry.systemTools[tool];
                if (systemTool->origin != ShaderToolOrigin::System ||
                    !HasTool(out, systemTool->name)) {
                    return reader.Fail(ShaderFormatErrorCode::InvalidValue, Prefix, "systemTools",
                                       producer.Find("systemTools"));
                }
            }
        }

        ++out.producerCount;
    }

    return true;
}

// backend와 처리기·경로의 짝(2절 형식 표)과 경로가 쓰는 도구가 목록에 있는지 본다.
bool CheckConsistency(Reader& reader, JsonValue root,
                      const ShaderFormatDefinition& definition) noexcept {
    ShaderFormatModule module = ShaderFormatModule::D3D;
    ShaderCompilerPath path = ShaderCompilerPath::DxcDxil;
    switch (definition.backend) {
    case ShaderBackend::D3D12:
        break;
    case ShaderBackend::D3D11:
        path = ShaderCompilerPath::Fxc;
        break;
    case ShaderBackend::Vulkan:
        module = ShaderFormatModule::Vulkan;
        path = ShaderCompilerPath::DxcSpirv;
        break;
    case ShaderBackend::Metal:
        module = ShaderFormatModule::Metal;
        path = ShaderCompilerPath::DxcSpirvMsl;
        break;
    case ShaderBackend::OpenGL:
        module = ShaderFormatModule::OpenGL;
        path = ShaderCompilerPath::DxcSpirvEssl;
        break;
    }

    if (definition.module != module) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, "processor", "module",
                           root.Find("processor"));
    }

    if (definition.compiler.path != path) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, "compiler", "path",
                           root.Find("compiler"));
    }

    bool needsSpirvTools = true;
    bool needsSpirvCross = true;
    bool needsFxc = false;
    bool needsMetal = false;
    switch (path) {
    case ShaderCompilerPath::DxcDxil:
        needsSpirvTools = false;
        needsSpirvCross = false;
        break;
    case ShaderCompilerPath::Fxc:
        needsFxc = true;
        break;
    case ShaderCompilerPath::DxcSpirv:
        needsSpirvCross = false;
        break;
    case ShaderCompilerPath::DxcSpirvMsl:
        needsMetal = true;
        break;
    default:
        break;
    }

    const bool complete = HasTool(definition, DxcToolName) &&
                          (!needsSpirvTools || HasTool(definition, SpirvToolsToolName)) &&
                          (!needsSpirvCross || HasTool(definition, SpirvCrossToolName)) &&
                          (!needsFxc || HasTool(definition, FxcToolName)) &&
                          (!needsMetal || HasTool(definition, MetalToolName));
    if (!complete) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, "", "tools", root.Find("tools"));
    }

    // UAV 단계는 이 형식의 단계 안이어야 한다.
    if ((definition.limits.uavStages & ~definition.stages) != 0) {
        return reader.Fail(ShaderFormatErrorCode::InvalidValue, "limits", "uavStages",
                           root.Find("limits"));
    }

    return true;
}

bool ReadFile(const char* path, Array<uint8>& bytes) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    if (!file.Size(size) || size > MaxDefinitionFileBytes ||
        !bytes.TryResize(static_cast<usize>(size))) {
        (void)file.Close();
        return false;
    }

    const platform::FileTransfer read = file.ReadExact(bytes.Data(), bytes.Size());
    const platform::FileStatus closed = file.Close();
    return read.status && read.bytes == bytes.Size() && closed;
}

// directory/<id>.json을 읽는다. id는 IsIdCharacter만 받아 경로 구분자가 들어가지 않는다.
bool ReadConfigFile(const char* directory, StringView id, Array<uint8>& bytes,
                    ShaderFormatStatus& status) noexcept {
    bool valid = id.ByteCount() > 0 && id.ByteCount() < MaxShaderFormatIdBytes;
    for (usize index = 0; valid && index < id.ByteCount(); ++index) {
        valid = IsIdCharacter(id.Data()[index]);
    }

    char path[MaxFilePathBytes];
    const int written = snprintf(path, sizeof(path), "%s/%.*s.json", directory,
                                 static_cast<int>(id.ByteCount()), id.Data());
    if (!valid || written <= 0 || static_cast<usize>(written) >= sizeof(path)) {
        status.code = ShaderFormatErrorCode::InvalidValue;
        snprintf(status.path, sizeof(status.path), "%.*s", static_cast<int>(id.ByteCount()),
                 id.Data());
        return false;
    }

    if (!ReadFile(path, bytes)) {
        status.code = ShaderFormatErrorCode::ReadFailed;
        snprintf(status.path, sizeof(status.path), "%s", path);
        return false;
    }

    return true;
}

bool ParseDocument(Span<const uint8> bytes, JsonDocument& document, Reader& reader,
                   ShaderFormatStatus& status) noexcept {
    const JsonError parsed =
        document.Parse(StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size()));
    if (!parsed.Succeeded()) {
        status.code = ShaderFormatErrorCode::InvalidJson;
        snprintf(status.path, sizeof(status.path), "%s", JsonErrorMessage(parsed.code));
        status.line = parsed.position.line;
        return false;
    }

    if (!document.Root().IsObject()) {
        return reader.Fail(ShaderFormatErrorCode::WrongType, "", "(root)", document.Root());
    }

    return true;
}

bool ReadHeader(Reader& reader, JsonValue root, StringView expectedId,
                char (&id)[MaxShaderFormatIdBytes]) noexcept {
    uint32 schema = 0;
    const JsonValue schemaValue = reader.Require(root, "", "schema");
    if (!reader.ReadUInt(schemaValue, "", "schema", 0, NumericLimits<uint32>::Max(), schema)) {
        return false;
    }

    if (schema != ShaderFormatSchemaVersion) {
        return reader.Fail(ShaderFormatErrorCode::UnsupportedSchema, "", "schema", schemaValue);
    }

    const JsonValue idValue = reader.Require(root, "", "id");
    if (!reader.ReadId(idValue, "", "id", id)) {
        return false;
    }

    if (expectedId.ByteCount() > 0 && StringView(id) != expectedId) {
        return reader.Fail(ShaderFormatErrorCode::IdMismatch, "", "id", idValue);
    }

    return true;
}

} // namespace

const char* ShaderFormatErrorName(ShaderFormatErrorCode code) noexcept {
    switch (code) {
    case ShaderFormatErrorCode::None:
        return "None";
    case ShaderFormatErrorCode::ReadFailed:
        return "ReadFailed";
    case ShaderFormatErrorCode::InvalidJson:
        return "InvalidJson";
    case ShaderFormatErrorCode::UnsupportedSchema:
        return "UnsupportedSchema";
    case ShaderFormatErrorCode::UnknownProperty:
        return "UnknownProperty";
    case ShaderFormatErrorCode::MissingProperty:
        return "MissingProperty";
    case ShaderFormatErrorCode::WrongType:
        return "WrongType";
    case ShaderFormatErrorCode::InvalidValue:
        return "InvalidValue";
    case ShaderFormatErrorCode::DuplicateValue:
        return "DuplicateValue";
    case ShaderFormatErrorCode::UnknownTool:
        return "UnknownTool";
    case ShaderFormatErrorCode::IdMismatch:
        return "IdMismatch";
    case ShaderFormatErrorCode::UnknownFormat:
        return "UnknownFormat";
    case ShaderFormatErrorCode::PlaceholderFormat:
        return "PlaceholderFormat";
    case ShaderFormatErrorCode::HostCannotProduce:
        return "HostCannotProduce";
    case ShaderFormatErrorCode::TooMany:
        return "TooMany";
    }

    return "Unknown";
}

const char* ShaderBackendName(ShaderBackend backend) noexcept {
    return NameOf(Names(BackendNames), backend);
}

const char* ShaderHostName(ShaderHost host) noexcept {
    return NameOf(Names(HostNames), host);
}

usize ImageFormatIndex(StringView name) noexcept {
    for (usize index = 0; index < ImageFormatCount; ++index) {
        if (StringView(ImageFormatNames[index]) == name) {
            return index;
        }
    }

    return IndexNone;
}

bool ShaderFormatDefinition::CanProduceOn(ShaderHost host) const noexcept {
    return Producer(host) != nullptr;
}

const ShaderFormatProducer* ShaderFormatDefinition::Producer(ShaderHost host) const noexcept {
    for (usize index = 0; index < producerCount; ++index) {
        if (producers[index].host == host) {
            return &producers[index];
        }
    }

    return nullptr;
}

ShaderFormatStatus ParseShaderFormatDefinition(Span<const uint8> bytes, StringView expectedId,
                                               ShaderFormatDefinition& out,
                                               Span<const ShaderTool> tools) noexcept {
    ShaderFormatStatus status;
    out = ShaderFormatDefinition{};
    SystemAllocator allocator;
    JsonDocument document(allocator);
    Reader reader(status);
    if (!ParseDocument(bytes, document, reader, status)) {
        return status;
    }

    const JsonValue root = document.Root();
    if (!ReadHeader(reader, root, expectedId, out.id) ||
        !reader.ReadName(root, "", "status", Names(StateNames), out.state) ||
        !reader.ReadName(root, "", "backend", Names(BackendNames), out.backend)) {
        return status;
    }

    if (out.state == ShaderFormatState::Placeholder) {
        static const char* const PlaceholderKeys[] = {"schema", "id", "status", "backend"};
        (void)reader.CheckKeys(root, Span<const char* const>(PlaceholderKeys), "");
        out.sha256 = Sha256::Hash(bytes);
        return status;
    }

    static const char* const Keys[] = {"schema",
                                       "id",
                                       "status",
                                       "backend",
                                       "processor",
                                       "bindingConvention",
                                       "tools",
                                       "language",
                                       "compiler",
                                       "stages",
                                       "limits",
                                       "features",
                                       "deviceRequirements",
                                       "producers"};
    if (!reader.CheckKeys(root, Span<const char* const>(Keys), "")) {
        return status;
    }

    const JsonValue processor = reader.RequireObject(root, "", "processor");
    static const char* const ProcessorKeys[] = {"module", "version"};
    if (!processor.IsValid() ||
        !reader.CheckKeys(processor, Span<const char* const>(ProcessorKeys), "processor") ||
        !reader.ReadName(processor, "processor", "module", Names(ModuleNames), out.module) ||
        !reader.ReadUInt(processor, "processor", "version", 1, NumericLimits<uint32>::Max(),
                         out.moduleVersion, true) ||
        !reader.ReadUInt(root, "", "bindingConvention", 1, NumericLimits<uint32>::Max(),
                         out.bindingConvention, true) ||
        !ReadTools(reader, root, "", "tools", tools, out.tools, out.toolCount)) {
        return status;
    }

    const JsonValue language = reader.RequireObject(root, "", "language");
    static const char* const LanguageKeys[] = {"hlsl2021"};
    if (!language.IsValid() ||
        !reader.CheckKeys(language, Span<const char* const>(LanguageKeys), "language") ||
        !reader.ReadBool(language, "language", "hlsl2021", out.hlsl2021)) {
        return status;
    }

    const JsonValue compiler = reader.RequireObject(root, "", "compiler");
    if (!compiler.IsValid() || !ReadCompiler(reader, compiler, out.compiler) ||
        !reader.ReadBits(root, "", "stages", Names(StageNames), out.stages)) {
        return status;
    }

    if (out.stages == 0) {
        reader.Fail(ShaderFormatErrorCode::InvalidValue, "", "stages", root.Find("stages"));
        return status;
    }

    const JsonValue limits = reader.RequireObject(root, "", "limits");
    if (!limits.IsValid() ||
        !ReadLimits(reader, limits, out.backend == ShaderBackend::OpenGL, out.limits) ||
        !reader.ReadBits(root, "", "features", Names(FeatureNames), out.features) ||
        !reader.ReadBits(root, "", "deviceRequirements", Names(DeviceRequirementNames),
                         out.deviceRequirements) ||
        !ReadProducers(reader, root, tools, out) || !CheckConsistency(reader, root, out)) {
        return status;
    }

    out.sha256 = Sha256::Hash(bytes);
    return status;
}

ShaderFormatStatus LoadShaderFormatDefinition(const char* directory, StringView id,
                                              ShaderFormatDefinition& out) noexcept {
    ShaderFormatStatus status;
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    if (!ReadConfigFile(directory, id, bytes, status)) {
        return status;
    }

    return ParseShaderFormatDefinition(bytes.View(), id, out);
}

ShaderFormatStatus ParseEngineCompileProfile(Span<const uint8> bytes, StringView expectedId,
                                             EngineCompileProfileDesc& out) noexcept {
    ShaderFormatStatus status;
    out = EngineCompileProfileDesc{};
    SystemAllocator allocator;
    JsonDocument document(allocator);
    Reader reader(status);
    if (!ParseDocument(bytes, document, reader, status)) {
        return status;
    }

    const JsonValue root = document.Root();
    static const char* const Keys[] = {"schema", "id", "host", "formats"};
    if (!reader.CheckKeys(root, Span<const char* const>(Keys), "") ||
        !ReadHeader(reader, root, expectedId, out.id) ||
        !reader.ReadName(root, "", "host", Names(HostNames), out.host)) {
        return status;
    }

    const JsonValue formats = reader.Require(root, "", "formats");
    if (!formats.IsValid()) {
        return status;
    }

    if (!formats.IsArray() || formats.Size() == 0) {
        reader.Fail(formats.IsArray() ? ShaderFormatErrorCode::InvalidValue
                                      : ShaderFormatErrorCode::WrongType,
                    "", "formats", formats);
        return status;
    }

    for (usize index = 0; index < formats.Size(); ++index) {
        const JsonValue element = formats.At(index);
        if (out.formatCount == MaxEngineProfileFormats) {
            reader.Fail(ShaderFormatErrorCode::TooMany, "", "formats", element);
            return status;
        }

        char (&id)[MaxShaderFormatIdBytes] = out.formats[out.formatCount];
        if (!reader.ReadId(element, "", "formats", id)) {
            return status;
        }

        for (usize seen = 0; seen < out.formatCount; ++seen) {
            if (StringView(out.formats[seen]) == StringView(id)) {
                reader.Fail(ShaderFormatErrorCode::DuplicateValue, "", "formats", element);
                return status;
            }
        }

        ++out.formatCount;
    }

    out.sha256 = Sha256::Hash(bytes);
    return status;
}

ShaderFormatStatus ResolveEngineCompileProfile(const EngineCompileProfileDesc& desc,
                                               Span<const ShaderFormatDefinition> definitions,
                                               EngineCompileProfile& out) noexcept {
    ShaderFormatStatus status;
    out = EngineCompileProfile{};
    memcpy(out.id, desc.id, sizeof(out.id));
    out.host = desc.host;
    out.sha256 = desc.sha256;
    for (usize index = 0; index < desc.formatCount; ++index) {
        const StringView id(desc.formats[index]);
        const ShaderFormatDefinition* found = nullptr;
        for (const ShaderFormatDefinition& definition : definitions) {
            if (definition.Id() == id) {
                found = &definition;
            }
        }

        ShaderFormatErrorCode code = ShaderFormatErrorCode::None;
        if (found == nullptr) {
            code = ShaderFormatErrorCode::UnknownFormat;
        } else if (found->state == ShaderFormatState::Placeholder) {
            code = ShaderFormatErrorCode::PlaceholderFormat;
        } else if (!found->CanProduceOn(desc.host)) {
            code = ShaderFormatErrorCode::HostCannotProduce;
        }

        if (code != ShaderFormatErrorCode::None) {
            status.code = code;
            snprintf(status.path, sizeof(status.path), "formats[%zu] %s",
                     static_cast<size_t>(index), desc.formats[index]);
            return status;
        }

        out.formats[out.formatCount++] = *found;
    }

    return status;
}

ShaderFormatStatus LoadEngineCompileProfile(const char* directory, StringView profileId,
                                            EngineCompileProfile& out) noexcept {
    ShaderFormatStatus status;
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    if (!ReadConfigFile(directory, profileId, bytes, status)) {
        return status;
    }

    EngineCompileProfileDesc desc;
    status = ParseEngineCompileProfile(bytes.View(), profileId, desc);
    if (!status) {
        return status;
    }

    // 형식 정의는 커서 힙에 둔다(MaxEngineProfileFormats개).
    Array<ShaderFormatDefinition> definitions(allocator);
    if (!definitions.TryResize(desc.formatCount)) {
        status.code = ShaderFormatErrorCode::TooMany;
        snprintf(status.path, sizeof(status.path), "formats");
        return status;
    }

    for (usize index = 0; index < desc.formatCount; ++index) {
        status = LoadShaderFormatDefinition(directory, StringView(desc.formats[index]),
                                            definitions[index]);
        if (status.code == ShaderFormatErrorCode::ReadFailed) {
            // 프로필이 가리킨 형식의 정의 파일이 없다.
            status.code = ShaderFormatErrorCode::UnknownFormat;
            snprintf(status.path, sizeof(status.path), "formats[%zu] %s",
                     static_cast<size_t>(index), desc.formats[index]);
        }

        if (!status) {
            return status;
        }
    }

    return ResolveEngineCompileProfile(desc, definitions.View(), out);
}

} // namespace rocks::shader
