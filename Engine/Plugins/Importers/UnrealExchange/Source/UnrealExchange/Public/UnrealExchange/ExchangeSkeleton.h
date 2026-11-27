#pragma once

#include <AnimationTools/SkeletonImport.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Object/ObjectRegistry.h>
#include <UnrealExchange/ExchangeMeshSource.h>
#include <UnrealExchange/ExchangeSettings.h>

// 교환 스켈레톤 → 스켈레톤 임포트 명세 변환이다(Docs/Design/UnrealExchange 6.4절 1번 — UX7b1). A2f
// 임포터 unreal_skeleton(UX7b2)이 문서를 검사한 뒤 이 변환으로 SkeletonImportSpec을 만들어
// ImportSkeleton에 넘긴다.
//
// 사용 예(A2f 임포터 안):
//   ExchangeSkeletonInput input{&asset, &options->references, &context.Registry(),
//                               context.Access()};
//   ExchangeSkeleton skeleton(allocator);
//   ExchangeMeshFailure failure;
//   if (auto built = skeleton.Build(input, failure); !built) { ... }
//   return ImportSkeleton(context, output, skeleton.Spec());
namespace rocks {

struct ExchangeSkeletonInput {
    // 스켈레톤 교환 에셋(Ready — 문서를 읽은 것)이다.
    const ExchangeAsset* asset = nullptr;
    // 참조로 걸 대상이다(스켈레톤 명세에는 참조가 없다 — 번역 규칙의 일관성으로 받는다).
    const OrderedMap<String, AssetId>* references = nullptr;
    // 번역·이름 문맥(타입 표·Name pool — 출력이 쓸 Registry)이다.
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
};

// 변환 결과다. Spec()은 이 객체와 input.asset이 살아 있는 동안 유효하다. 복사·이동하지 않는다.
class ROCKS_UNREALEXCHANGE_API ExchangeSkeleton {
public:
    explicit ExchangeSkeleton(Allocator& allocator) noexcept;
    ExchangeSkeleton(const ExchangeSkeleton&) = delete;
    ExchangeSkeleton& operator=(const ExchangeSkeleton&) = delete;

    // input으로 명세를 만든다(6.4절 1번): 설정 번역(ExchangeSkeletonSettings.h 표) → 본·리타깃
    // 방식·커브(data) → 가상 본·소켓(settings) → AssetUserData 점검. replaceOnReimport는 참이다. 한
    // 객체에 한 번만 부른다.
    //
    // 경고: 번역의 UnsupportedSetting(detail = 표 행 번호, 모르는 키 ExchangeUnknownSettingDetail),
    // 커브 메타데이터가 아닌 AssetUserData(detail = AssetUserData 행).
    //
    // 오류(failure.code·detail): 호출 계약(asset·registry 없음, 스켈레톤이 아님, 두 번 부름)
    // InvalidSource / InvalidArgument, 덤프 형식(본·pose·리타깃 방식 이름·커브·가상 본·소켓 —
    // detail = 그 배열 번호, 설정 행은 행 번호) CorruptBundle / CorruptData, 번역 값 종류 위반
    // InvalidSource / CorruptData, 코드 결함·할당 오류는 그 오류(코드 None).
    Result<void> Build(const ExchangeSkeletonInput& input, ExchangeMeshFailure& failure) noexcept;

    const SkeletonImportSpec& Spec() const noexcept {
        return spec_;
    }

    Span<const ExchangeSettingWarning> Warnings() const noexcept {
        return warnings_.View();
    }

private:
    // 커브 하나의 연결 본 범위다(linkedBones_ 안).
    struct CurveBones {
        usize start = 0;
        usize count = 0;
    };

    friend struct ExchangeSkeletonBuilder;

    Allocator* allocator_;
    bool started_ = false;
    JsonDocument translated_;
    Array<SkeletonImportBone> bones_;
    Array<BoneTranslationRetarget> retargetModes_;
    Array<SkeletonImportVirtualBone> virtualBones_;
    Array<SkeletonImportSocket> sockets_;
    Array<SkeletonImportCurve> curves_;
    Array<CurveBones> curveBones_;
    Array<StringView> linkedBones_;
    Array<ExchangeSettingWarning> warnings_;
    SkeletonImportSpec spec_;
};

} // namespace rocks
