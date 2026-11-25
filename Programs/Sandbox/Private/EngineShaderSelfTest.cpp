#include "EngineShaderSelfTest.h"
#include "EngineShaderProbe.h"
#include "SandboxShaders.h"

#include <Base/PlacementNew.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Memory.h>
#include <RenderBase/EngineShaderTypes.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/ShaderSet.h>
#include <ShaderCore/ShaderParameterTypes.h>
#include <stdio.h>
#include <string.h>

// 엔진 셰이더 SL0 GPU probe 장치 연결이다(Docs/Design/EngineShaders 7.6절). 영역 대조는
// EngineShaderReference*.cpp다.
namespace sandbox {

namespace {

using namespace rocks;
using namespace rocks::graphics;
using namespace sandbox::engine_shaders;

// ProbeCommon.rsh ProbeConstants의 C++ 판이다(레이아웃 표는 SandboxShaders.h ProbeConstantLayouts).
struct alignas(16) ProbeConstants {
    shader::ShaderUint4 control;
    shader::ShaderFloat4 parameters;
    shader::ShaderFloat4x4 values;
};

static_assert(sizeof(ProbeConstants) == shaders::ProbeConstantBytes,
              "ProbeConstants must match ProbeCommon.rsh");

// 배치 폭 상한(Docs/Design/EngineShaders 7.6절 — 장치 한도가 더 작으면 그 폭으로 나눈다)과 타일
// 원점 표현 상한(16 bit)이다.
constexpr uint32 MaxProbeExtent = 16384;
constexpr uint32 TileOriginLimit = 0xffff;
constexpr uint32 FloatProbeBytesPerCase = 16;
constexpr uint32 UintProbeBytesPerCase = 4;
// 장치 사이 대조에 남기는 실수 사례 수(probe마다 앞부분)다.
constexpr usize CrossDeviceFloatCases = 4096;
// 기록 label 길이다.
constexpr usize RecordLabelBytes = 96;

SystemAllocator& RecordAllocator() noexcept {
    static SystemAllocator allocator;
    return allocator;
}

// probe 하나의 장치 사이 기록이다.
struct ProbeRecord {
    explicit ProbeRecord(Allocator& allocator) noexcept : values(allocator) {
    }

    char label[RecordLabelBytes] = {};
    char firstDevice[RecordLabelBytes] = {};
    bool isUint = false;
    usize size = 0;
    uint64 fingerprint = 0;
    Array<float> values;
    double maxDifference = 0.0;
    char worstDevice[RecordLabelBytes] = {};
    uint32 uintMismatches = 0;
};

Array<ProbeRecord*>& Records() noexcept {
    static Array<ProbeRecord*> records(RecordAllocator());
    return records;
}

const char* currentDevice = "";

ProbeRecord* FindRecord(const char* label) noexcept {
    for (ProbeRecord* record : Records()) {
        if (strcmp(record->label, label) == 0) {
            return record;
        }
    }

    return nullptr;
}

ProbeRecord* AddRecord(const char* label, bool isUint) noexcept {
    auto memory = RecordAllocator().Allocate(sizeof(ProbeRecord), alignof(ProbeRecord));
    if (!memory) {
        return nullptr;
    }

    ProbeRecord* record = new (memory.Value()) ProbeRecord(RecordAllocator());
    snprintf(record->label, sizeof(record->label), "%s", label);
    snprintf(record->firstDevice, sizeof(record->firstDevice), "%s", currentDevice);
    record->isUint = isUint;
    if (!Records().TryPushBack(record)) {
        record->~ProbeRecord();
        (void)RecordAllocator().Deallocate(record);
        return nullptr;
    }

    return record;
}

class CrossDeviceRecorder final : public ProbeRecorder {
public:
    void RecordFloat(const char* label, Span<const float> values) noexcept override {
        ProbeRecord* record = FindRecord(label);
        const usize kept =
            values.Size() < CrossDeviceFloatCases * 4u ? values.Size() : CrossDeviceFloatCases * 4u;
        if (record == nullptr) {
            record = AddRecord(label, false);
            if (record != nullptr && record->values.TryResize(kept)) {
                platform::MemoryCopy(record->values.Data(), values.Data(), kept * sizeof(float));
                record->size = values.Size();
            }

            return;
        }

        if (record->values.Size() != kept) {
            return;
        }

        for (usize index = 0; index < kept; ++index) {
            const double a = record->values[index];
            const double b = values[index];
            if (isnan(a) || isnan(b) || isinf(a) || isinf(b)) {
                continue;
            }

            // 큰 값(지수 근사 2^127 등)은 상대 차이로 본다.
            const double difference = fabs(a - b) / fmax(1.0, fabs(a));
            if (difference > record->maxDifference) {
                record->maxDifference = difference;
                snprintf(record->worstDevice, sizeof(record->worstDevice), "%s", currentDevice);
            }
        }
    }

    void RecordUint(const char* label, Span<const uint32> values) noexcept override {
        ProbeRecord* record = FindRecord(label);
        const uint64 fingerprint = Fingerprint(values);
        if (record == nullptr) {
            record = AddRecord(label, true);
            if (record != nullptr) {
                record->fingerprint = fingerprint;
                record->size = values.Size();
            }

            return;
        }

        if (record->fingerprint != fingerprint || record->size != values.Size()) {
            ++record->uintMismatches;
            snprintf(record->worstDevice, sizeof(record->worstDevice), "%s", currentDevice);
        }
    }
};

// 장치 하나의 probe 실행기다.
class DeviceProbeRunner final : public ProbeDevice {
public:
    DeviceProbeRunner(Device& device, Allocator& allocator) noexcept
        : device_(device), allocator_(allocator) {
        const uint32 limit = device.Caps().limits.textureSize2D;
        maxExtent_ = limit != 0 && limit < MaxProbeExtent ? limit : MaxProbeExtent;
    }

    ~DeviceProbeRunner() noexcept {
        for (Pipeline& pipeline : pipelines_) {
            if (pipeline.created) {
                device_.Retire(pipeline.handle);
            }
        }
    }

    DeviceProbeRunner(const DeviceProbeRunner&) = delete;
    DeviceProbeRunner& operator=(const DeviceProbeRunner&) = delete;

    bool RunFloat(const ProbeRequest& request, Array<float>& out) noexcept override {
        if (!out.TryResize(static_cast<usize>(CaseCount(request)) * 4u)) {
            return false;
        }

        return Run(request, false, reinterpret_cast<uint8*>(out.Data()), FloatProbeBytesPerCase);
    }

    bool RunUint(const ProbeRequest& request, Array<uint32>& out) noexcept override {
        if (!out.TryResize(CaseCount(request))) {
            return false;
        }

        return Run(request, true, reinterpret_cast<uint8*>(out.Data()), UintProbeBytesPerCase);
    }

private:
    enum PipelineKind : uint32 {
        MathFloat,
        MathUint,
        ColorFloat,
        ShadingFloat,
        PipelineKindCount
    };

    struct Pipeline {
        PipelineHandle handle;
        bool created = false;
        bool failed = false;
    };

    static PipelineKind KindOf(uint32 function) noexcept {
        if (function < ProbeFiniteBits) {
            return MathFloat;
        }

        if (function < ProbeColorMatrix) {
            return MathUint;
        }

        return function < ProbeDistributions ? ColorFloat : ShadingFloat;
    }

    Result<PipelineHandle> PipelineFor(PipelineKind kind) noexcept {
        Pipeline& pipeline = pipelines_[kind];
        if (pipeline.created) {
            return Result<PipelineHandle>::Success(pipeline.handle);
        }

        if (pipeline.failed) {
            return Result<PipelineHandle>::Failure(ErrorCode::DependencyFailed);
        }

        pipeline.failed = true;
        render::ShaderSet* set = shaders::ActiveSet();
        if (set == nullptr) {
            return Result<PipelineHandle>::Failure(ErrorCode::NotReady);
        }

        auto vertexEntry = set->FindEntry(render::FullscreenTriangleVertexType.hash);
        if (!vertexEntry) {
            return Result<PipelineHandle>::Failure(vertexEntry.Error());
        }

        auto vertex = set->DeviceShader(vertexEntry.Value());
        const shaders::ShaderTypeDesc* const pixels[PipelineKindCount] = {
            &shaders::MathFloatProbe, &shaders::MathUintProbe, &shaders::ColorFloatProbe,
            &shaders::ShadingFloatProbe};
        auto pixel = shaders::Get(*pixels[kind]);
        if (!vertex || !pixel) {
            printf("    probe shader unavailable: %s\n", pixels[kind]->name.Data());
            return Result<PipelineHandle>::Failure(!vertex ? vertex.Error() : pixel.Error());
        }

        PipelineDesc desc;
        desc.vertexShader = vertex.Value();
        desc.pixelShader = pixel.Value();
        desc.colorFormats[0] = kind == MathUint ? PixelFormat::R32Uint : PixelFormat::RGBA32Float;
        desc.drawConstantBytes = sizeof(ProbeConstants);
        auto created = device_.CreatePipeline(desc);
        if (!created) {
            printf("    probe pipeline failed: %.*s\n",
                   static_cast<int>(device_.LastError().ByteCount()), device_.LastError().Data());
            return created;
        }

        pipeline.handle = created.Value();
        pipeline.created = true;
        pipeline.failed = false;
        return created;
    }

    // 배치를 장치 한도 안 타일로 나눠 그리고 읽어 out(사례 순서)에 모은다.
    bool Run(const ProbeRequest& request, bool isUint, uint8* out, uint32 bytesPerCase) noexcept {
        const PipelineKind kind = KindOf(request.function);
        if ((kind == MathUint) != isUint) {
            return false;
        }

        auto pipeline = PipelineFor(kind);
        if (!pipeline) {
            return false;
        }

        ProbeConstants constants;
        constants.control.x = request.function | (request.selector << 16);
        constants.control.y = request.width;
        constants.control.w = request.seed;
        constants.parameters = {request.parameters[0], request.parameters[1], request.parameters[2],
                                request.parameters[3]};
        platform::MemoryCopy(constants.values.m, request.values, sizeof(request.values));
        Array<uint8> tile(allocator_);
        for (uint32 originY = 0; originY < request.height; originY += maxExtent_) {
            for (uint32 originX = 0; originX < request.width; originX += maxExtent_) {
                if (originX > TileOriginLimit || originY > TileOriginLimit) {
                    return false;
                }

                const uint32 tileWidth =
                    request.width - originX < maxExtent_ ? request.width - originX : maxExtent_;
                const uint32 tileHeight =
                    request.height - originY < maxExtent_ ? request.height - originY : maxExtent_;
                constants.control.z = originX | (originY << 16);
                TextureDesc desc;
                desc.width = tileWidth;
                desc.height = tileHeight;
                desc.format = isUint ? PixelFormat::R32Uint : PixelFormat::RGBA32Float;
                desc.usage = TextureUsage::RenderTarget;
                auto target = device_.CreateTexture(desc, {});
                if (!target) {
                    return false;
                }

                const usize tileBytes = static_cast<usize>(tileWidth) * tileHeight * bytesPerCase;
                bool ok = tile.TryResize(tileBytes).IsOk();
                auto frame = ok ? device_.BeginOffscreen(target.Value(), {})
                                : Result<CommandContext*>::Failure(ErrorCode::OutOfMemory);
                if (frame) {
                    CommandContext& commands = *frame.Value();
                    commands.SetPipeline(pipeline.Value());
                    ok = commands
                             .SetDrawConstants(Span<const uint8>(
                                 reinterpret_cast<const uint8*>(&constants), sizeof(constants)))
                             .IsOk();
                    render::DrawFullscreenTriangle(commands);
                    ok = device_.EndFrame().IsOk() && ok &&
                         device_.ReadTexture(target.Value(), 0, 0, tile.View()).IsOk();
                } else {
                    ok = false;
                }

                device_.Retire(target.Value());
                if (!ok) {
                    printf("    probe %u draw failed: %.*s\n", request.function,
                           static_cast<int>(device_.LastError().ByteCount()),
                           device_.LastError().Data());
                    return false;
                }

                for (uint32 row = 0; row < tileHeight; ++row) {
                    const usize destination =
                        (static_cast<usize>(originY + row) * request.width + originX) *
                        bytesPerCase;
                    platform::MemoryCopy(out + destination,
                                         tile.Data() +
                                             static_cast<usize>(row) * tileWidth * bytesPerCase,
                                         static_cast<usize>(tileWidth) * bytesPerCase);
                }
            }
        }

        return true;
    }

    Device& device_;
    Allocator& allocator_;
    uint32 maxExtent_ = MaxProbeExtent;
    Pipeline pipelines_[PipelineKindCount];
};

// 시험하지 않은 공개 함수와 사유다(7.7절 5항). 목록은 probe 원문에서 부르지 않는 라이브러리 함수를
// 모은 것이다(SL0 coverage 사유 — compile 행렬은 include 폐포로 모두 컴파일한다).
struct CoverageReason {
    const char* function;
    const char* reason;
};

constexpr CoverageReason UntestedFunctions[] = {
    {"ROCKS_SHADER_ASSERT",
     "empty macro in SL0 (GPU messages are SL2); compiled by IncludeClosure"},
    {"ROCKS_PIXEL_OUTPUT_1..8(_DEPTH)",
     "declaration macros; IncludeClosurePixel compiles the 2-target form"},
    {"ROCKS_SCALAR_ARRAY_ELEMENT, ScalarArrayPackedLength",
     "constant buffer array access needs a cbuffer; SL2 parameter structs (SS P7) test it"},
    {"ROCKS_SHADER_SHARED_BEGIN/END, ShaderTypeAliases C++ side",
     "C++ inclusion of Engine/Shaders/Shared is SL2 (RenderData static_assert)"},
    {"ROCKS_SHADER_STATIC_ASSERT C++ side", "SL2 together with ShaderTypeAliases"},
    {"helpers checked through their public callers",
     "ChromaticAdaptationMatrix, ConvolveCosineLobeSH2, DaylightLocusXy, ExactPowerOfTwo, "
     "ExtendedSubtract, "
     "ExtendedToFloat3, FloorLog2Exponent, GridToOctahedral, HanningWindowWeight, "
     "IsShadingModelSupported, "
     "LaineKarrasPermutation, MakeExtendedFloat3, Morton spread/compact, NestedUniformScramble, "
     "OctahedralWrap, "
     "PcgOutput, Rocks filmic stages, SignedCubeRoot, SolveQuadratic, SplitFloat, "
     "WatertightEdgeFunction"},
};

} // namespace

uint32 RunEngineShaderChecks(const char* name, Device& device, Allocator& allocator) noexcept {
    printf("  %s: engine shader probes (Docs/Design/EngineShaders 7.6)\n", name);
    currentDevice = name;
    DeviceProbeRunner runner(device, allocator);
    CrossDeviceRecorder recorder;
    ProbeContext context{runner, recorder, allocator, name};
    RunScalarVectorProbes(context);
    RunMatrixQuaternionProbes(context);
    RunPackingProbes(context);
    RunRandomSamplingProbes(context);
    RunColorProbes(context);
    RunShadingProbes(context);
    printf("  [%s] %s engine shader probes: %u checks, %u failure%s\n",
           context.failures == 0 ? "PASS" : "FAIL", name, context.probes, context.failures,
           context.failures == 1 ? "" : "s");
    return context.failures;
}

void ResetEngineShaderRecords() noexcept {
    for (ProbeRecord* record : Records()) {
        record->~ProbeRecord();
        (void)RecordAllocator().Deallocate(record);
    }

    Records().Clear();
}

uint32 ReportEngineShaderCrossDevice() noexcept {
    uint32 failures = 0;
    double worst = 0.0;
    const char* worstLabel = "";
    for (const ProbeRecord* record : Records()) {
        if (record->isUint) {
            if (record->uintMismatches != 0) {
                ++failures;
                printf("  [FAIL] engine shader %s: integer results differ between %s and %s\n",
                       record->label, record->firstDevice, record->worstDevice);
            }

            continue;
        }

        if (record->maxDifference > 0.0) {
            printf("    engine shader %s: max difference between devices %.3g (relative above 1; "
                   "%s vs %s)\n",
                   record->label, record->maxDifference, record->firstDevice, record->worstDevice);
        }

        if (record->maxDifference > worst) {
            worst = record->maxDifference;
            worstLabel = record->label;
        }
    }

    printf("  [%s] engine shader cross-device comparison: %zu probes, integer probes %s, largest "
           "float difference %.3g (%s)\n",
           failures == 0 ? "PASS" : "FAIL", Records().Size(),
           failures == 0 ? "bit exact" : "differ", worst, worstLabel);
    printf("  engine shader coverage reasons (Docs/Design/EngineShaders 7.7 item 5):\n");
    for (const CoverageReason& reason : UntestedFunctions) {
        printf("    %s: %s\n", reason.function, reason.reason);
    }

    // 기록은 프로세스 정적 할당자에 있으므로 정적 소멸 전에 돌려준다.
    ResetEngineShaderRecords();
    return failures;
}

} // namespace sandbox
