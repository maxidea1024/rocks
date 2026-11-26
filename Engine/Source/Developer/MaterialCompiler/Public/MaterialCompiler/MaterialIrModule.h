#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
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
    Preview = 7
};

// 진입점 수다(마지막 값 + 1).
inline constexpr uint32 IrEntryCount = 8;

// 진입점 결과 칸 하나다. slot은 출력 핀 키(정적 원문 — 예: "WorldPositionOffset")다. 한 모듈 안에서
// (entry, slot)은 유일하다.
struct IrResult {
    IrEntry entry = IrEntry::Surface;
    StringView slot;
    IrValueId value = NoIrValue;
};

class ROCKS_MATERIALCOMPILER_API IrModule {
public:
    explicit IrModule(Allocator& allocator) noexcept : values_(allocator), results_(allocator) {
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
        hash_ = Sha256Digest{};
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
    Sha256Digest hash_{};
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
