#include "SpikeRun.h"

#include "SpikeShaders.h"
#include "SpikeTools.h"

#include <Core/Diagnostics/Log.h>
#include <Core/Text/NumberText.h>
#include <Platform/Clock.h>
#include <locale.h>
#include <spirv-tools/libspirv.h>
#include <spirv_cross_c.h>
#include <stdio.h>
#include <string.h>

// SH0 스파이크 실행 순서(Docs/Design/ShaderSystem.ko.md 16절):
// 1. 도구 열기와 S8(열기·첫 컴파일 비용, SHA-256 처리량).
// 2. 내장 Renderer VS·PS → DXIL(서명 hash, 리플렉션), SPIR-V(spirv-val, 리플렉션),
//    DXBC(FXC), SPIRV-Cross MSL·ESSL 두 가지·HLSL 5.0(→ FXC).
// 3. ROCKS_IMAGE_FORMAT: [[vk::image_format]]이 SPIR-V 형식으로 나오는지.
// 4. 상수 초월 함수 fixture(S3 상수 접기)와 실수 출력 fixture(10절, C locale·쉼표 locale 비교).
// 5. S13(typed buffer의 ESSL·결합 sampler)·S14(VS layer·viewport 출력의 형식별 수용).
// 6. S2(D3D12 셰이더 모델)·S12(GL 드라이버의 #version 320 es).
// 산출물은 manifest.json에 이름·크기·SHA-256으로 남고(결정성 비교용), 측정값은 report.json에
// 남는다.
namespace rocks::spike {

namespace {

constexpr double NanosecondsPerMicrosecond = 1000.0;
constexpr double NanosecondsPerSecond = 1e9;
constexpr double BytesPerMebibyte = 1048576.0;
// SHA-256 처리량은 같은 입력을 여러 번 해시해 가장 빠른 값을 쓴다.
constexpr uint32 HashRounds = 5;
// 산출물 이름 버퍼 크기다(OutputDirectory 항목 이름 상한과 같다).
constexpr usize OutputNameBytes = 96;
// 쉼표 소수점 locale이다(SPIRV-Cross fixup_radix_point 확인용). Windows UCRT 이름이다.
constexpr char CommaLocale[] = "de-DE";
// 측정 S14: VS layer·viewport 출력의 DXIL 셰이더 모델(7.2절 7항, D3D12_SM6 형식)이다.
constexpr char LayerViewportShaderModel[] = "6_6";
// SpvCapabilityShaderViewportIndexLayerEXT(SPIR-V 사양 3.31, SPIR-V 1.5 전 확장 capability)다.
constexpr uint32 ShaderViewportIndexLayerCapability = 5254;
constexpr char ShaderViewportIndexLayerExtension[] = "SPV_EXT_shader_viewport_index_layer";

double MicrosecondsSince(uint64 start) noexcept {
    return static_cast<double>(platform::MonotonicNanoseconds() - start) /
           NanosecondsPerMicrosecond;
}

const char* CrossExtension(CrossTarget target) noexcept {
    switch (target) {
    case CrossTarget::EsslAndroid:
    case CrossTarget::EsslPc:
        return "essl";
    case CrossTarget::Msl:
        return "msl";
    case CrossTarget::Hlsl50:
        return "hlsl";
    }

    return "txt";
}

// text에서 수 토큰을 찾아 float로 읽었을 때 bits와 같은 첫 토큰을 spelling에 쓴다. 토큰은
// 영숫자·'_'·'.' 뒤가 아닌 곳에서 시작하는 [0-9.]와 지수부이며, 바로 앞 '-'를 부호로 붙인다.
bool FindFloatLiteral(StringView text, uint32 bits, StringBuilderBase& spelling) noexcept {
    const char* data = text.Data();
    const usize size = text.ByteCount();
    auto isDigit = [](char value) { return value >= '0' && value <= '9'; };
    auto isWord = [&](char value) {
        return isDigit(value) || value == '_' || value == '.' || (value >= 'a' && value <= 'z') ||
               (value >= 'A' && value <= 'Z');
    };
    for (usize start = 0; start < size; ++start) {
        if (!isDigit(data[start]) || (start > 0 && isWord(data[start - 1]))) {
            continue;
        }

        usize end = start;
        while (end < size) {
            const char value = data[end];
            const bool exponent =
                (value == 'e' || value == 'E') && end + 1 < size &&
                (isDigit(data[end + 1]) || data[end + 1] == '-' || data[end + 1] == '+');
            const bool sign = (value == '-' || value == '+') && end > start &&
                              (data[end - 1] == 'e' || data[end - 1] == 'E');
            if (!isDigit(value) && value != '.' && !exponent && !sign) {
                break;
            }

            ++end;
        }

        const usize first = start > 0 && data[start - 1] == '-' ? start - 1 : start;
        const StringView token(data + first, end - first);
        const Result<float> parsed = TryParseFloat(token);
        if (parsed) {
            uint32 parsedBits = 0;
            const float value = parsed.Value();
            memcpy(&parsedBits, &value, sizeof(parsedBits));
            if (parsedBits == bits) {
                spelling.Clear();
                spelling.Append(token);
                return true;
            }
        }

        start = end;
    }

    return false;
}

// text에서 needle이 든 첫 줄을 line에 쓴다.
void FindLine(StringView text, StringView needle, StringBuilderBase& line) noexcept {
    line.Clear();
    const usize at = text.Find(needle, StringComparison::CaseSensitive);
    if (at == StringView::NotFound) {
        return;
    }

    usize begin = at;
    while (begin > 0 && text.Data()[begin - 1] != '\n') {
        --begin;
    }

    usize end = at;
    while (end < text.ByteCount() && text.Data()[end] != '\n') {
        ++end;
    }

    line.Append(StringView(text.Data() + begin, end - begin));
}

// text에서 needle이 든 줄을 모두 report의 열린 객체에 key 배열로 쓴다.
void ReportLines(Report& report, StringView key, StringView text, StringView needle) noexcept {
    report.BeginArray(key);
    const char* data = text.Data();
    const usize size = text.ByteCount();
    usize begin = 0;
    while (begin < size) {
        usize end = begin;
        while (end < size && data[end] != '\n') {
            ++end;
        }

        const StringView line(data + begin, end - begin);
        if (line.Find(needle, StringComparison::CaseSensitive) != StringView::NotFound) {
            report.Element(line);
        }

        begin = end + 1;
    }

    report.EndArray();
}

void ReportRequires(Report& report, const ShaderRequires& shaderRequires) noexcept {
    report.UInt("requiresFlags", shaderRequires.flags);
    report.Bool("requiresViewportAndLayerFromAnyShader",
                shaderRequires.viewportAndLayerFromAnyShader);
}

class Spike final {
public:
    explicit Spike(const RunOptions& options) noexcept
        : options_(options), output_(options.outputDirectory), vertexEssl_(SpikeAllocator()),
          pixelEssl_(SpikeAllocator()) {
    }

    int Run() noexcept;

private:
    bool OpenTools() noexcept;
    void ReportVersions() noexcept;
    void MeasureFirstCompiles() noexcept;
    void RunRenderer(const ShaderInput& input, StringBuilderBase& esslPc) noexcept;
    void RunImageFormats() noexcept;
    void RunTranscendental() noexcept;
    void RunFloatLiterals() noexcept;
    void RunTypedBuffers() noexcept;
    void RunLayerViewport() noexcept;

    // name.extension 이름으로 산출물을 쓴다. 실패는 필수 검사 실패다.
    void Write(const char* name, const char* extension, Span<const uint8> bytes) noexcept;
    void WriteText(const char* name, const char* extension, StringView text) noexcept;

    // DXIL을 만들고 서명 hash를 검사해 report의 열린 객체에 "dxil"로 쓴다. reflection이면 리플렉션
    // 덤프를 <name>.dxil.reflection.json으로 남긴다.
    bool BuildDxil(const ShaderInput& input, bool reflection) noexcept;
    // SPIR-V를 만들고 spirv-val로 검증해 "spirv"로 쓴다. 성공하면 code에 SPIR-V가 남는다.
    bool BuildSpirv(const ShaderInput& input, bool reflection, ByteArray& code) noexcept;
    // SPIRV-Cross 출력을 만들어 쓴다. 결과 원문은 text에 남는다.
    bool BuildCross(const char* name, Span<const uint8> spirv, CrossTarget target,
                    StringBuilderBase& text) noexcept;
    bool BuildFxc(const char* name, const char* extension, StringView source, const char* entry,
                  const char* profile, StringView key) noexcept;

    void Require(bool condition) noexcept {
        if (!condition) {
            failed_ = true;
        }
    }

    const RunOptions& options_;
    OutputDirectory output_;
    Report report_;
    shader::ShaderToolSet tools_;
    StringBuilder<4096> vertexEssl_;
    StringBuilder<4096> pixelEssl_;
    bool failed_ = false;
};

void Spike::Write(const char* name, const char* extension, Span<const uint8> bytes) noexcept {
    char file[OutputNameBytes];
    const int written = snprintf(file, sizeof(file), "%s.%s", name, extension);
    Require(written > 0 && static_cast<usize>(written) < sizeof(file) &&
            output_.Write(file, bytes));
}

void Spike::WriteText(const char* name, const char* extension, StringView text) noexcept {
    Write(name, extension, AsBytes(text));
}

bool Spike::OpenTools() noexcept {
    char root[shader::MaxShaderToolPathBytes];
    usize rootBytes = 0;
    const platform::FileStatus store =
        shader::GetShaderToolStoreRoot(root, sizeof(root), rootBytes);
    if (!store) {
        Log::Error(shader::LogShaderTool,
                   "Shader tool store root is unavailable on this host (OS error {OsError}); the "
                   "spike cannot open its required tools",
                   store.nativeError);
        return false;
    }

    report_.BeginObject("s8");
    const uint64 openStart = platform::MonotonicNanoseconds();
    const shader::ShaderToolLoadStatus opened = tools_.Open(root, rootBytes);
    const double openMicroseconds = MicrosecondsSince(openStart);
    if (!opened) {
        // 스파이크는 네 도구가 모두 필요하다(필수 도구 실패는 Error).
        Log::Error(shader::LogShaderTool,
                   "Required shader tool {Tool} could not be opened: {Code} (OS error {OsError}, "
                   "symbol {Symbol}). {Message}",
                   opened.tool != nullptr ? opened.tool : "unknown",
                   shader::ShaderToolLoadCodeName(opened.code), opened.file.nativeError,
                   opened.symbol != nullptr ? opened.symbol : "none",
                   shader::ShaderToolLoadMessage(opened.code));
        report_.EndObject();
        return false;
    }

    // 열기(ShaderToolSet::Open)는 runtime 파일의 크기·SHA-256 대조와 DynamicLibrary::Open을 함께
    // 한다.
    report_.Double("openToolsMicroseconds", openMicroseconds);

    // SHA-256 처리량: 네 도구의 runtime 파일을 메모리에 읽어 해시한다(파일 읽기는 빼고 잰다).
    uint64 totalBytes = 0;
    double bestHashSeconds = 0.0;
    const char* names[] = {shader::DxcToolName, shader::FxcToolName, shader::SpirvCrossToolName,
                           shader::SpirvToolsToolName};
    ByteArray bytes(SpikeAllocator());
    for (const char* name : names) {
        const shader::ShaderTool* tool = shader::FindShaderTool(StringView(name));
        if (tool == nullptr) {
            continue;
        }

        for (usize index = 0; index < tool->fileCount; ++index) {
            const shader::ShaderToolFile& file = tool->files[index];
            if (file.role != shader::ShaderToolFileRole::Runtime) {
                continue;
            }

            char path[shader::MaxShaderToolPathBytes];
            snprintf(path, sizeof(path), "%.*s/%s/%s", static_cast<int>(rootBytes), root,
                     tool->storeKey, file.path);
            if (!ReadWholeFile(path, bytes)) {
                continue;
            }

            double best = 0.0;
            for (uint32 round = 0; round < HashRounds; ++round) {
                const uint64 start = platform::MonotonicNanoseconds();
                const Sha256Digest digest = Sha256::Hash(bytes.View());
                const double seconds =
                    static_cast<double>(platform::MonotonicNanoseconds() - start) /
                    NanosecondsPerSecond;
                Require(digest == file.sha256);
                best = round == 0 || seconds < best ? seconds : best;
            }

            totalBytes += bytes.Size();
            bestHashSeconds += best;
        }
    }

    report_.UInt("hashedRuntimeBytes", totalBytes);
    report_.Double("sha256MebibytesPerSecond",
                   bestHashSeconds > 0.0
                       ? static_cast<double>(totalBytes) / BytesPerMebibyte / bestHashSeconds
                       : 0.0);
    report_.Double("sha256RuntimeFilesMicroseconds",
                   bestHashSeconds * NanosecondsPerSecond / NanosecondsPerMicrosecond);
    MeasureFirstCompiles();
    report_.EndObject();
    return true;
}

void Spike::MeasureFirstCompiles() noexcept {
    // 프로세스에서 처음 부르는 컴파일(도구 안 지연 초기화 포함)과 바로 다음 같은 컴파일의 시간이다.
    DxcOutput first;
    DxcOutput second;
    uint64 start = platform::MonotonicNanoseconds();
    Require(
        CompileWithDxc(tools_.Dxc(), RendererVertex, DxcTarget::Dxil, options_.shaderModel, first));
    report_.Double("firstDxilCompileMicroseconds", MicrosecondsSince(start));
    start = platform::MonotonicNanoseconds();
    Require(CompileWithDxc(tools_.Dxc(), RendererVertex, DxcTarget::Dxil, options_.shaderModel,
                           second));
    report_.Double("secondDxilCompileMicroseconds", MicrosecondsSince(start));

    DxcOutput spirv;
    DxcOutput spirvAgain;
    start = platform::MonotonicNanoseconds();
    Require(CompileWithDxc(tools_.Dxc(), RendererVertex, DxcTarget::Spirv, options_.shaderModel,
                           spirv));
    report_.Double("firstSpirvCompileMicroseconds", MicrosecondsSince(start));
    start = platform::MonotonicNanoseconds();
    Require(CompileWithDxc(tools_.Dxc(), RendererVertex, DxcTarget::Spirv, options_.shaderModel,
                           spirvAgain));
    report_.Double("secondSpirvCompileMicroseconds", MicrosecondsSince(start));

    StringBuilder<4096> text(SpikeAllocator());
    StringBuilder<256> diagnostics(SpikeAllocator());
    start = platform::MonotonicNanoseconds();
    Require(CrossCompile(tools_.SpirvCross(), spirv.code.View(), CrossTarget::EsslPc, text,
                         diagnostics));
    report_.Double("firstSpirvCrossMicroseconds", MicrosecondsSince(start));
    start = platform::MonotonicNanoseconds();
    Require(CrossCompile(tools_.SpirvCross(), spirv.code.View(), CrossTarget::EsslPc, text,
                         diagnostics));
    report_.Double("secondSpirvCrossMicroseconds", MicrosecondsSince(start));
    start = platform::MonotonicNanoseconds();
    StringBuilder<256> validation(SpikeAllocator());
    Require(ValidateSpirv(tools_.SpirvTools(), spirv.code.View(), validation));
    report_.Double("firstSpirvValMicroseconds", MicrosecondsSince(start));
}

void Spike::ReportVersions() noexcept {
    report_.BeginObject("tools");
    uint32 major = 0;
    uint32 minor = 0;
    const shader::ShaderToolLoadStatus versioned =
        shader::QueryDxcVersion(tools_.Dxc(), major, minor);
    if (versioned) {
        char version[32];
        snprintf(version, sizeof(version), "%u.%u", major, minor);
        report_.String("dxc", StringView(version));
    } else {
        // 버전 표시는 없어도 되는 선택 정보다(Debug).
        Log::Debug(shader::LogShaderTool,
                   "DXC version query is unavailable: {Code}; the report omits it",
                   shader::ShaderToolLoadCodeName(versioned.code));
    }

    const auto spirvToolsVersion = reinterpret_cast<decltype(&spvSoftwareVersionString)>(
        tools_.SpirvTools().address[shader::SpirvToolsFunctions::SoftwareVersionString]);
    report_.String("spirvTools", StringView(spirvToolsVersion()));
    const auto crossVersion = reinterpret_cast<decltype(&spvc_get_version)>(
        tools_.SpirvCross().address[shader::SpirvCrossFunctions::GetVersion]);
    unsigned crossMajor = 0;
    unsigned crossMinor = 0;
    unsigned crossPatch = 0;
    crossVersion(&crossMajor, &crossMinor, &crossPatch);
    char cross[32];
    snprintf(cross, sizeof(cross), "%u.%u.%u", crossMajor, crossMinor, crossPatch);
    report_.String("spirvCrossCApi", StringView(cross));
    report_.Bool("fxc", tools_.HasFxc());
    report_.String("dxilShaderModel", StringView(options_.shaderModel));
    report_.EndObject();
}

bool Spike::BuildDxil(const ShaderInput& input, bool reflection) noexcept {
    DxcOutput output;
    const bool compiled =
        CompileWithDxc(tools_.Dxc(), input, DxcTarget::Dxil, options_.shaderModel, output);
    report_.BeginObject("dxil");
    report_.Bool("ok", compiled);
    report_.String("diagnostics", output.diagnostics.View());
    bool ok = compiled;
    if (compiled) {
        Write(input.name, "dxil", output.code.View());
        // 10절 dxil 서명: digest가 0이거나 preview 우회 값이면 실패다.
        const ContainerDigest digest = ReadContainerDigest(output.code.View());
        char hex[ContainerDigest::Bytes * 2 + 1];
        ContainerDigestHex(digest, hex);
        report_.String("containerDigest", StringView(hex));
        report_.Bool("signed", digest.isSigned);
        ok = digest.isSigned;
        if (reflection) {
            Report dump;
            dump.BeginObject();
            const bool dumped = DumpDxilReflection(tools_.Dxc(), output.reflection.View(), dump);
            dump.EndObject();
            ok = ok && dumped && dump.Finish();
            report_.Bool("reflection", dumped);
            if (dumped) {
                WriteText(input.name, "dxil.reflection.json", dump.Text());
            }
        }
    }

    report_.EndObject();
    Require(ok);
    return ok;
}

bool Spike::BuildSpirv(const ShaderInput& input, bool reflection, ByteArray& code) noexcept {
    DxcOutput output;
    const bool compiled =
        CompileWithDxc(tools_.Dxc(), input, DxcTarget::Spirv, options_.shaderModel, output);
    report_.BeginObject("spirv");
    report_.Bool("ok", compiled);
    report_.String("diagnostics", output.diagnostics.View());
    bool ok = compiled;
    if (compiled) {
        Write(input.name, "spv", output.code.View());
        StringBuilder<256> validation(SpikeAllocator());
        const bool valid = ValidateSpirv(tools_.SpirvTools(), output.code.View(), validation);
        report_.Bool("spirvVal", valid);
        report_.String("spirvValMessage", validation.View());
        ok = valid;
        if (reflection) {
            Report dump;
            dump.BeginObject();
            const bool dumped = DumpSpirvReflection(output.code.View(), dump);
            dump.EndObject();
            ok = ok && dumped && dump.Finish();
            report_.Bool("reflection", dumped);
            if (dumped) {
                WriteText(input.name, "spv.reflection.json", dump.Text());
            }
        }

        ok = ok && code.TryAppend(output.code.View());
    }

    report_.EndObject();
    return ok;
}

bool Spike::BuildCross(const char* name, Span<const uint8> spirv, CrossTarget target,
                       StringBuilderBase& text) noexcept {
    StringBuilder<256> diagnostics(SpikeAllocator());
    const bool ok = CrossCompile(tools_.SpirvCross(), spirv, target, text, diagnostics);
    report_.BeginObject();
    report_.String("target", StringView(CrossTargetName(target)));
    report_.Bool("ok", ok);
    report_.String("diagnostics", diagnostics.View());
    report_.EndObject();
    if (ok) {
        char extension[48];
        snprintf(extension, sizeof(extension), "%s.%s", CrossTargetName(target),
                 CrossExtension(target));
        WriteText(name, extension, text.View());
    }

    return ok;
}

bool Spike::BuildFxc(const char* name, const char* extension, StringView source, const char* entry,
                     const char* profile, StringView key) noexcept {
    ByteArray code(SpikeAllocator());
    StringBuilder<256> diagnostics(SpikeAllocator());
    const bool ok = CompileWithFxc(tools_.Fxc(), source, name, entry, profile, code, diagnostics);
    report_.BeginObject(key);
    report_.Bool("ok", ok);
    report_.String("profile", StringView(profile));
    report_.String("diagnostics", diagnostics.View());
    if (ok) {
        Write(name, extension, code.View());
        const ContainerDigest digest = ReadContainerDigest(code.View());
        char hex[ContainerDigest::Bytes * 2 + 1];
        ContainerDigestHex(digest, hex);
        report_.String("containerDigest", StringView(hex));
    }

    report_.EndObject();
    return ok;
}

void Spike::RunRenderer(const ShaderInput& input, StringBuilderBase& esslPc) noexcept {
    report_.BeginObject();
    report_.String("name", StringView(input.name));
    BuildDxil(input, true);
    ByteArray spirv(SpikeAllocator());
    Require(BuildSpirv(input, true, spirv));

    // D3D11_SM5 기본 경로: 원문을 FXC SM 5.0으로.
    char profile[8];
    snprintf(profile, sizeof(profile), "%s_5_0", StageProfilePrefix(input.stage));
    Require(BuildFxc(input.name, "dxbc", StringView(input.source), input.entry, profile, "dxbc"));

    report_.BeginArray("spirvCross");
    StringBuilder<4096> text(SpikeAllocator());
    StringBuilder<4096> hlsl50(SpikeAllocator());
    if (spirv.Size() > 0) {
        Require(BuildCross(input.name, spirv.View(), CrossTarget::Msl, text));
        Require(BuildCross(input.name, spirv.View(), CrossTarget::EsslAndroid, text));
        Require(BuildCross(input.name, spirv.View(), CrossTarget::EsslPc, esslPc));
        Require(BuildCross(input.name, spirv.View(), CrossTarget::Hlsl50, hlsl50));
    }

    report_.EndArray();
    // D3D11 대체 경로 확인: SPIRV-Cross HLSL 5.0을 FXC로 다시 컴파일한다(진입점은 main).
    if (hlsl50.ByteCount() > 0) {
        Require(BuildFxc(input.name, "hlsl50.dxbc", hlsl50.View(), "main", profile, "hlsl50Fxc"));
    }

    report_.EndObject();
}

void Spike::RunImageFormats() noexcept {
    // 7.2절 1항 ROCKS_IMAGE_FORMAT: DXC가 [[vk::image_format]]을 SPIR-V OpTypeImage 형식으로
    // 내는지, DXIL에서는 무시되는지(경고 여부), ESSL 형식 한정자가 어떻게 나오는지 본다.
    report_.BeginArray("imageFormat");
    StringBuilder<1024> source(SpikeAllocator());
    StringBuilder<4096> essl(SpikeAllocator());
    StringBuilder<256> line(SpikeAllocator());
    for (usize index = 0; index < ImageFormatCaseCount; ++index) {
        const ImageFormatCase& test = ImageFormatCases[index];
        Require(BuildImageFormatSource(test, source));
        const ShaderInput input{test.name, source.CString(), "CSMain", ShaderStage::Compute};
        report_.BeginObject();
        report_.String("name", StringView(test.name));
        report_.String("attribute", StringView(test.attribute != nullptr ? test.attribute : ""));
        report_.UInt("expectedFormat", test.expectedFormat);
        ByteArray spirv(SpikeAllocator());
        const bool built = BuildSpirv(input, false, spirv);
        uint32 format = 0;
        const bool read = built && ReadStorageImageFormat(spirv.View(), format);
        report_.UInt("spirvFormat", format);
        report_.Bool("matches", read && format == test.expectedFormat);
        Require(read && format == test.expectedFormat);

        DxcOutput dxil;
        const bool dxilBuilt =
            CompileWithDxc(tools_.Dxc(), input, DxcTarget::Dxil, options_.shaderModel, dxil);
        report_.Bool("dxilOk", dxilBuilt);
        report_.String("dxilDiagnostics", dxil.diagnostics.View());

        StringBuilder<256> diagnostics(SpikeAllocator());
        const bool crossed = built && CrossCompile(tools_.SpirvCross(), spirv.View(),
                                                   CrossTarget::EsslPc, essl, diagnostics);
        report_.Bool("esslOk", crossed);
        report_.String("esslDiagnostics", diagnostics.View());
        if (crossed) {
            FindLine(essl.View(), "image2D", line);
            report_.String("esslImageLine", line.View());
            WriteText(test.name, "gles31_pc.essl", essl.View());
        }

        report_.EndObject();
    }

    report_.EndArray();
}

void Spike::RunTranscendental() noexcept {
    // 10절 상수 접기: 리터럴 인자 초월 함수가 접혔는지(SPIR-V OpExtInst가 남았는지)와 산출 byte를
    // manifest로 남겨 실행·빌드 사이에 비교한다.
    report_.BeginObject("transcendental");
    BuildDxil(TranscendentalPixel, false);
    ByteArray spirv(SpikeAllocator());
    Require(BuildSpirv(TranscendentalPixel, false, spirv));
    report_.UInt("spirvExtInst", CountExtInst(spirv.View()));
    ByteArray optimized(SpikeAllocator());
    StringBuilder<256> diagnostics(SpikeAllocator());
    const bool optimizedOk = spirv.Size() > 0 && OptimizeSpirv(tools_.SpirvTools(), spirv.View(),
                                                               optimized, diagnostics);
    report_.Bool("optimizedOk", optimizedOk);
    if (optimizedOk) {
        Write(TranscendentalPixel.name, "opt.spv", optimized.View());
        StringBuilder<256> validation(SpikeAllocator());
        Require(ValidateSpirv(tools_.SpirvTools(), optimized.View(), validation));
        report_.UInt("optimizedExtInst", CountExtInst(optimized.View()));
    }

    report_.BeginArray("spirvCross");
    StringBuilder<4096> text(SpikeAllocator());
    if (spirv.Size() > 0) {
        Require(BuildCross(TranscendentalPixel.name, spirv.View(), CrossTarget::EsslPc, text));
        Require(BuildCross(TranscendentalPixel.name, spirv.View(), CrossTarget::Msl, text));
    }

    report_.EndArray();
    report_.EndObject();
}

void Spike::RunFloatLiterals() noexcept {
    // 10절 SPIRV-Cross 실수 출력: 극값 상수의 ESSL·MSL 철자가 원래 bit로 다시 읽히는지(C locale),
    // 쉼표 소수점 locale에서도 같은 byte인지 본다. 기대 철자(%.32g의 정확한 반올림)는 report의
    // 철자를 독립 계산과 대조한다(결과는 문서 5.4절).
    report_.BeginObject("floatLiterals");
    BuildDxil(FloatLiteralPixel, false);
    ByteArray spirv(SpikeAllocator());
    Require(BuildSpirv(FloatLiteralPixel, false, spirv));
    const CrossTarget targets[] = {CrossTarget::EsslPc, CrossTarget::Msl};
    StringBuilder<4096> esslText(SpikeAllocator());
    StringBuilder<4096> mslText(SpikeAllocator());
    StringBuilderBase* cText[] = {&esslText, &mslText};
    report_.BeginArray("spirvCross");
    for (usize index = 0; index < 2 && spirv.Size() > 0; ++index) {
        Require(BuildCross(FloatLiteralPixel.name, spirv.View(), targets[index], *cText[index]));
    }

    report_.EndArray();
    report_.BeginArray("values");
    StringBuilder<128> spelling(SpikeAllocator());
    for (usize index = 0; index < FloatLiteralCaseCount; ++index) {
        const FloatLiteralCase& value = FloatLiteralCases[index];
        report_.BeginObject();
        report_.String("hlsl", StringView(value.spelling));
        report_.String("meaning", StringView(value.meaning));
        report_.UInt("bits", value.bits);
        for (usize target = 0; target < 2; ++target) {
            const bool found = FindFloatLiteral(cText[target]->View(), value.bits, spelling);
            Require(found);
            report_.String(target == 0 ? StringView("essl") : StringView("msl"),
                           found ? spelling.View() : StringView(""));
        }

        report_.EndObject();
    }

    report_.EndArray();

    // 같은 SPIR-V를 쉼표 소수점 locale에서 다시 바꿔 byte를 비교한다(SPIRV-Cross
    // fixup_radix_point).
    const char* switched = setlocale(LC_ALL, CommaLocale);
    report_.Bool("commaLocaleAvailable", switched != nullptr);
    if (switched != nullptr) {
        char probe[16];
        snprintf(probe, sizeof(probe), "%.1f", 1.5);
        report_.String("commaLocaleSample", StringView(probe));
        for (usize index = 0; index < 2 && spirv.Size() > 0; ++index) {
            StringBuilder<4096> again(SpikeAllocator());
            StringBuilder<256> diagnostics(SpikeAllocator());
            const bool ok =
                CrossCompile(tools_.SpirvCross(), spirv.View(), targets[index], again, diagnostics);
            const bool same = ok && again.View() == cText[index]->View();
            report_.Bool(index == 0 ? StringView("esslSameInCommaLocale")
                                    : StringView("mslSameInCommaLocale"),
                         same);
            Require(same);
        }
    }

    setlocale(LC_ALL, "C");
    report_.EndObject();
}

void Spike::RunTypedBuffers() noexcept {
    // 측정 S13(7.2절 4항 GLES typed buffer, Renderer RQ10): typed buffer SRV·UAV가 ESSL에서
    // samplerBuffer 계열·texelFetch, imageBuffer 계열·imageLoad·imageStore로 나오는지, SPIRV-Cross
    // 결합 sampler 목록에 typed buffer SRV가 들어가는지 본다. MSL 줄은 참고로 남긴다.
    report_.BeginArray("typedBuffer");
    const ShaderInput* inputs[] = {&TypedBufferPixel, &TypedBufferCompute};
    const CrossTarget targets[] = {CrossTarget::EsslPc, CrossTarget::EsslAndroid, CrossTarget::Msl};
    for (const ShaderInput* input : inputs) {
        report_.BeginObject();
        report_.String("name", StringView(input->name));
        BuildDxil(*input, true);
        ByteArray spirv(SpikeAllocator());
        Require(BuildSpirv(*input, true, spirv));
        report_.BeginArray("spirvCross");
        for (const CrossTarget target : targets) {
            if (spirv.Size() == 0) {
                break;
            }

            StringBuilder<4096> text(SpikeAllocator());
            StringBuilder<256> diagnostics(SpikeAllocator());
            const bool essl = target != CrossTarget::Msl;
            report_.BeginObject();
            report_.String("target", StringView(CrossTargetName(target)));
            const bool ok = CrossCompile(tools_.SpirvCross(), spirv.View(), target, text,
                                         diagnostics, essl ? &report_ : nullptr);
            report_.Bool("ok", ok);
            report_.String("diagnostics", diagnostics.View());
            if (ok) {
                char extension[48];
                snprintf(extension, sizeof(extension), "%s.%s", CrossTargetName(target),
                         CrossExtension(target));
                WriteText(input->name, extension, text.View());
                if (essl) {
                    ReportLines(report_, "samplerBufferLines", text.View(), "samplerBuffer");
                    ReportLines(report_, "imageBufferLines", text.View(), "imageBuffer");
                    ReportLines(report_, "texelFetchLines", text.View(), "texelFetch");
                    ReportLines(report_, "imageLoadLines", text.View(), "imageLoad");
                    ReportLines(report_, "imageStoreLines", text.View(), "imageStore");
                } else {
                    ReportLines(report_, "textureLines", text.View(), "texture");
                }
            }

            report_.EndObject();
        }

        report_.EndArray();
        report_.EndObject();
    }

    report_.EndArray();
}

void Spike::RunLayerViewport() noexcept {
    // 측정 S14(7.2절 7항 vertexViewportLayerOutput, Renderer RQ14): VS 출력
    // SV_RenderTargetArrayIndex·SV_ViewportArrayIndex를 형식마다 받는지 본다. DXIL은 SM 6.6과
    // 기본 셰이더 모델, SPIR-V는 vulkan1.1의 capability·확장, SPIRV-Cross는 MSL·ESSL·HLSL 5.0,
    // D3D11_SM5는 FXC vs_5_0(원문과 SPIRV-Cross HLSL 5.0 둘 다)이다. 받지 않는 형식은 측정 결과라
    // 필수 검사로 두지 않는다(DXIL SM 6.6·SPIR-V만 필수).
    const ShaderInput& input = LayerViewportVertex;
    report_.BeginObject("layerViewport");
    report_.BeginArray("dxil");
    const char* models[] = {LayerViewportShaderModel, options_.shaderModel};
    for (usize index = 0; index < 2; ++index) {
        const char* model = models[index];
        // --sm 6_6이면 같은 산출물을 두 번 쓰지 않는다.
        if (index > 0 && strcmp(model, models[0]) == 0) {
            continue;
        }

        DxcOutput output;
        const bool compiled = CompileWithDxc(tools_.Dxc(), input, DxcTarget::Dxil, model, output);
        report_.BeginObject();
        report_.String("shaderModel", StringView(model));
        report_.Bool("ok", compiled);
        report_.String("diagnostics", output.diagnostics.View());
        bool ok = compiled;
        if (compiled) {
            char extension[32];
            snprintf(extension, sizeof(extension), "sm%s.dxil", model);
            Write(input.name, extension, output.code.View());
            const ContainerDigest digest = ReadContainerDigest(output.code.View());
            report_.Bool("signed", digest.isSigned);
            ShaderRequires shaderRequires;
            const bool read =
                ReadDxilRequires(tools_.Dxc(), output.reflection.View(), shaderRequires);
            report_.Bool("requiresRead", read);
            ReportRequires(report_, shaderRequires);
            ok = digest.isSigned && read;
        }

        report_.EndObject();
        // 첫 항목(SM 6.6)만 필수다.
        if (index == 0) {
            Require(ok);
        }
    }

    report_.EndArray();
    ByteArray spirv(SpikeAllocator());
    Require(BuildSpirv(input, true, spirv));
    report_.BeginObject("spirvFeatures");
    SpirvFeatureMatch match;
    const bool scanned =
        spirv.Size() > 0 && ReportSpirvFeatures(spirv.View(), ShaderViewportIndexLayerCapability,
                                                ShaderViewportIndexLayerExtension, report_, match);
    report_.Bool("scanned", scanned);
    report_.Bool("shaderViewportIndexLayerCapability", match.capability);
    report_.Bool("shaderViewportIndexLayerExtension", match.extension);
    report_.EndObject();

    report_.BeginArray("spirvCross");
    const CrossTarget targets[] = {CrossTarget::Msl, CrossTarget::EsslPc, CrossTarget::EsslAndroid,
                                   CrossTarget::Hlsl50};
    StringBuilder<4096> hlsl50(SpikeAllocator());
    for (const CrossTarget target : targets) {
        if (spirv.Size() == 0) {
            break;
        }

        StringBuilder<4096> text(SpikeAllocator());
        StringBuilder<256> diagnostics(SpikeAllocator());
        report_.BeginObject();
        report_.String("target", StringView(CrossTargetName(target)));
        const bool ok = CrossCompile(tools_.SpirvCross(), spirv.View(), target, text, diagnostics);
        report_.Bool("ok", ok);
        report_.String("diagnostics", diagnostics.View());
        if (ok) {
            char extension[48];
            snprintf(extension, sizeof(extension), "%s.%s", CrossTargetName(target),
                     CrossExtension(target));
            WriteText(input.name, extension, text.View());
            switch (target) {
            case CrossTarget::Msl:
                ReportLines(report_, "layerLines", text.View(), "render_target_array_index");
                ReportLines(report_, "viewportLines", text.View(), "viewport_array_index");
                break;
            case CrossTarget::EsslPc:
            case CrossTarget::EsslAndroid:
                ReportLines(report_, "extensionLines", text.View(), "#extension");
                ReportLines(report_, "layerLines", text.View(), "gl_Layer");
                ReportLines(report_, "viewportLines", text.View(), "gl_ViewportIndex");
                break;
            case CrossTarget::Hlsl50:
                ReportLines(report_, "layerLines", text.View(), "SV_RenderTargetArrayIndex");
                ReportLines(report_, "viewportLines", text.View(), "SV_ViewportArrayIndex");
                hlsl50.Append(text.View());
                break;
            }
        }

        report_.EndObject();
    }

    report_.EndArray();

    // D3D11_SM5: 원문을 FXC vs_5_0으로, SPIRV-Cross HLSL 5.0 출력도 FXC로(D3D11 대체 경로).
    report_.BeginArray("fxc");
    const StringView sources[] = {StringView(input.source), hlsl50.View()};
    const char* entries[] = {input.entry, "main"};
    const char* extensions[] = {"dxbc", "hlsl50.dxbc"};
    for (usize index = 0; index < 2; ++index) {
        if (sources[index].ByteCount() == 0) {
            continue;
        }

        ByteArray code(SpikeAllocator());
        StringBuilder<256> diagnostics(SpikeAllocator());
        const bool ok = CompileWithFxc(tools_.Fxc(), sources[index], input.name, entries[index],
                                       "vs_5_0", code, diagnostics);
        report_.BeginObject();
        report_.String("source", index == 0 ? StringView("hlsl") : StringView("spirvCrossHlsl50"));
        report_.String("profile", StringView("vs_5_0"));
        report_.Bool("ok", ok);
        report_.String("diagnostics", diagnostics.View());
        if (ok) {
            Write(input.name, extensions[index], code.View());
            ShaderRequires shaderRequires;
            report_.Bool("requiresRead",
                         ReadDxbcRequires(tools_.Fxc(), code.View(), shaderRequires));
            ReportRequires(report_, shaderRequires);
        }

        report_.EndObject();
    }

    report_.EndArray();
    report_.EndObject();
}

int Spike::Run() noexcept {
    setlocale(LC_ALL, "C");
    if (!output_.Create()) {
        return 1;
    }

    report_.BeginObject();
    if (!OpenTools()) {
        return 2;
    }

    ReportVersions();
    report_.BeginArray("renderer");
    RunRenderer(RendererVertex, vertexEssl_);
    RunRenderer(RendererPixel, pixelEssl_);
    report_.EndArray();
    RunImageFormats();
    RunTranscendental();
    RunFloatLiterals();
    RunTypedBuffers();
    RunLayerViewport();
    report_.BeginObject("s2");
    ProbeD3D12ShaderModel(report_);
    report_.EndObject();
    report_.BeginObject("s12");
    ProbeOpenGl(vertexEssl_.View(), pixelEssl_.View(), report_);
    report_.EndObject();
    report_.Bool("requiredChecksPassed", !failed_);
    report_.EndObject();
    const bool reported = report_.Finish();
    const bool manifest = output_.WriteManifest();
    const bool written = reported && output_.WriteUnlisted("report.json", report_.Text());
    printf("%s\n", failed_ ? "ShaderToolSpike: FAILED" : "ShaderToolSpike: OK");
    return !failed_ && manifest && written ? 0 : 1;
}

} // namespace

int RunSpike(const RunOptions& options) noexcept {
    // Spike는 함수 표·버퍼가 커서 정적 저장소에 둔다(한 프로세스에 한 번 실행한다).
    static Spike spike(options);
    return spike.Run();
}

} // namespace rocks::spike
