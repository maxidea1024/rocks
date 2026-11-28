#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <MaterialCompiler/MaterialIrCollection.h>
#include <MaterialCompiler/MaterialIrCustomTable.h>
#include <MaterialCompiler/MaterialIrDefine.h>
#include <MaterialSchema/MaterialContextFeature.h>
#include <MaterialSchema/MaterialContextValue.h>

// 머티리얼 IR 모듈이다(Docs/Design/MaterialSystem 6.2·6.5·6.6·6.8절). (static 변형, 품질 수준)마다
// 하나이고, 진입점 결과(7.3절 함수의 출력 칸)에서 닿는 값만 담는다(DCE). 값 번호는 결과 순서대로
// 깊이 우선 후위 순(피연산자가 먼저)으로 다시 매긴다: 원문 생성 순서와 같고 노드 저장 순서·빌더의
// 생성 순서와 무관하다(6.6절).
//
// 사용 예:
//   const IrResult results[] = {{IrEntry::Opacity, "Opacity", opacity}};
//   IrModule module(allocator);
//   if (auto built = BuildIrModule(builder, results, module); !built) { ... }
//   const Sha256Digest hash = module.Hash();
namespace rocks::material {

// 진입점이다(7.3절 함수 표 순서).
enum class IrEntry : uint8 {
    Vertex = 0,
    VertexPrevious = 1,
    Surface = 2,
    Opacity = 3,
    OpacityMask = 4,
    PixelDepthOffset = 5,
    CustomOutput = 6,
    Preview = 7,
    // 대표 노멀(6.11절 MS2d1 — EvaluateMaterialNormal, 픽셀).
    Normal = 8,
    // 정점 빈도 custom 출력과 그 이전 프레임 값이다(6.11절 MS2d3 — FirstPersonOutput).
    CustomOutputVertex = 9,
    CustomOutputVertexPrevious = 10
};

// 진입점 수다(마지막 값 + 1).
inline constexpr uint32 IrEntryCount = 11;

// 진입점 결과 칸 하나다. slot은 출력 핀 키(정적 원문 — 예: "WorldPositionOffset")다. 한 모듈 안에서
// (entry, slot)은 유일하다.
struct IrResult {
    IrEntry entry = IrEntry::Surface;
    StringView slot;
    IrValueId value = NoIrValue;
};

class ROCKS_MATERIALCOMPILER_API IrModule {
public:
    explicit IrModule(Allocator& allocator) noexcept
        : values_(allocator), results_(allocator), parameters_(allocator), textures_(allocator),
          collections_(allocator), origins_(allocator), defines_(allocator), customs_(allocator) {
    }

    IrModule(const IrModule&) = delete;
    IrModule& operator=(const IrModule&) = delete;

    Span<const IrValue> Values() const noexcept {
        return values_.View();
    }

    // 결과다. 진입점 순서 → 넣은 순서다. value는 다시 매긴 번호다.
    Span<const IrResult> Results() const noexcept {
        return results_.View();
    }

    // 최적화를 마친 IR의 SHA-256이다(6.5절 IR hash — 값과 결과를 고정 형식으로 쓴 바이트).
    Sha256Digest Hash() const noexcept {
        return hash_;
    }

    // entry 결과에서 닿는 문맥 값(Extern)의 공급 기능 비트다(6.9절 — 생성 원문의 사용 정의).
    MaterialContextFeatureMask EntryFeatures(IrEntry entry) const noexcept {
        const uint32 index = static_cast<uint32>(entry);
        return index < IrEntryCount ? entryFeatures_[index] : NoMaterialContextFeatures;
    }

    // 진입점에서 닿는 수 파라미터다(ParameterId 순 — Parameter 즉시값 [3]이 이 번호다, 6.10절).
    Span<const IrParameter> Parameters() const noexcept {
        return parameters_.View();
    }

    // 진입점에서 닿는 텍스처 slot이다((출처, id) 순 — Texture 즉시값 [3]이 이 번호다, 생성 원문의
    // MaterialTexture<k>, 6.10절 MS2c3).
    Span<const IrTexture> Textures() const noexcept {
        return textures_.View();
    }

    // 닿는 수 파라미터·파라미터 텍스처의 출처다(정체 Guid 순 — 12.8절 MS3d1).
    Span<const IrParameterOrigin> ParameterOrigins() const noexcept {
        return origins_.View();
    }

    // identity의 출처다(없으면 null).
    const IrParameterOrigin* FindParameterOrigin(Guid identity) const noexcept {
        for (const IrParameterOrigin& origin : origins_) {
            if (origin.identity == identity) {
                return &origin;
            }
        }

        return nullptr;
    }

    // 진입점에서 닿는 파라미터 컬렉션이다(asset id 순 — 생성 원문의 MaterialCollection<k>).
    Span<const IrCollection> Collections() const noexcept {
        return collections_.View();
    }

    // 내보낸 shading model 집합 비트다(6.11절 MS2d1 — 생성 원문 MATERIAL_SHADING_MODELS). 0이면
    // 표면이 없다(DefaultLit로 본다).
    uint32 ShadingModels() const noexcept {
        return shadingModels_;
    }

    void SetShadingModels(uint32 mask) noexcept {
        shadingModels_ = mask;
    }

    // 진입점에서 닿는 Custom HLSL 함수다(6.13절 MS2f5 — 처음 닿은 값 순서, CustomCall 즉시값 [0]이
    // 이 번호다).
    const IrCustomTable& CustomFunctions() const noexcept {
        return customs_;
    }

    // 닿는 Custom HLSL 함수가 픽셀을 버릴 수 있는가(UE bUsesCustomHLSLPixelDiscard — 픽셀 문맥
    // 항목만).
    bool UsesCustomPixelDiscard() const noexcept {
        for (const IrCustomFunction& function : customs_.Functions()) {
            if (function.pixelDiscard && function.context == IrCustomContext::Pixel) {
                return true;
            }
        }

        return false;
    }

    // 노드 속성이 정하는 생성 원문 정의다(6.11절 MS2d3 — 넣은 순서).
    Span<const IrDefine> Defines() const noexcept {
        return defines_.View();
    }

    // 정의를 더하고 IR hash에 섞는다(BuildIrModule 뒤에 부른다). 오류: 할당 실패.
    Result<void> AddDefines(Span<const IrDefine> defines) noexcept;

    // 보간 값(Interpolate) 성분 수의 합이다(6.9절 MS2b4a — 보간 배정).
    uint32 InterpolantComponents() const noexcept {
        return interpolantComponents_;
    }

    // 결과에서 닿는 문맥 값의 최소 기능 등급 중 가장 높은 것이다(6.9절 — 형식 거르기가 쓴다).
    MaterialCapabilityTier RequiredTier() const noexcept {
        return requiredTier_;
    }

    // 값·결과를 비운다(번역 실패 결과 — 6.8절).
    void Clear() noexcept {
        values_.Clear();
        results_.Clear();
        parameters_.Clear();
        textures_.Clear();
        collections_.Clear();
        origins_.Clear();
        defines_.Clear();
        customs_.Clear();
        hash_ = Sha256Digest{};
        shadingModels_ = 0;
        for (MaterialContextFeatureMask& features : entryFeatures_) {
            features = NoMaterialContextFeatures;
        }

        requiredTier_ = MaterialCapabilityTier::Mobile;
        interpolantComponents_ = 0;
    }

private:
    friend ROCKS_MATERIALCOMPILER_API Result<void>
    BuildIrModule(const IrBuilder& builder, Span<const IrResult> results, IrModule& out) noexcept;

    Array<IrValue> values_;
    Array<IrResult> results_;
    Array<IrParameter> parameters_;
    Array<IrTexture> textures_;
    Array<IrCollection> collections_;
    Array<IrParameterOrigin> origins_;
    Array<IrDefine> defines_;
    IrCustomTable customs_;
    Sha256Digest hash_{};
    uint32 shadingModels_ = 0;
    MaterialContextFeatureMask entryFeatures_[IrEntryCount] = {};
    MaterialCapabilityTier requiredTier_ = MaterialCapabilityTier::Mobile;
    uint32 interpolantComponents_ = 0;
};

// builder의 값에서 results가 닿는 값만 골라 out을 채운다. 결과는 진입점 순서로 안정 정렬한다.
// 진입점별 사용 기능·필요 기능 등급도 계산한다. 오류: 결과 값 번호가 빌더 밖이거나 (entry, slot)이
// 겹치거나 Extern의 표 번호가 표 밖이면 InvalidArgument, 할당 실패. 실패하면 out은 비어 있다.
ROCKS_MATERIALCOMPILER_API Result<void>
BuildIrModule(const IrBuilder& builder, Span<const IrResult> results, IrModule& out) noexcept;

} // namespace rocks::material
