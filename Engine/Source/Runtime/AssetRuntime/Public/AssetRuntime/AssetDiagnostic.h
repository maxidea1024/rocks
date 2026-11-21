#pragma once

#include <Core/Result.h>
#include <Object/AssetId.h>

namespace rocks {

// 에셋 진단 코드다(Docs/Design/AssetSystem.ko.md 4.6절). API는 Result의 ErrorCode로 실패를 알리고,
// 그 실패가 에셋 쪽 어떤 분류인지는 이 코드로 따로 준다(ErrorCode 하나로는 "손상"과 "더 새 버전"을
// 가를 수 없다). 원본 오류 코드는 AssetDiagnostic::error에 그대로 남긴다.
//
// 여기 값은 진단 이름과 1:1이며 저장 포맷이 아니다(순서를 바꿔도 된다). A1이 내는 것은 패키지·
// 매니페스트·로드 쪽(CorruptPackage·UnsupportedVersion·NewerVersion·MissingDependency·TypeMismatch·
// IncompatibleCook·UnpackagedReference·UnsupportedCycle)이고, 나머지는 A2–A5가 같은 이름표를 쓴다.
// 메시 코드는 MeshSource 모듈(MA1)과 MeshTools 빌드(MA2a·MA5a)가 MeshDiagnostic::code로, 스켈레톤
// 코드는 Animation 모듈(MA4a)이 SkeletonDiagnostic::code로, 클립 코드는 Animation 모듈(MA6a)이
// ClipDiagnostic::code로 낸다. Unreal 교환 코드는 엔진 플러그인 UnrealExchange(UX1a)가
// ExchangeDiagnostic::code로 낸다.
enum class AssetDiagnosticCode : uint16 {
    None,
    // 식별·경로
    DuplicateAssetId,
    DuplicatePath,
    CaseCollision,
    InvalidPath,
    MissingFile,
    UnregisteredFile,
    // 참조
    TypeMismatch,
    MissingDependency,
    UnsupportedCycle,
    UnpackagedReference,
    // 패키지·저장
    CorruptPackage,
    UnsupportedVersion,
    NewerVersion,
    ExternalModification,
    NotDurable,
    // 에셋 너겟·쿠킹
    CorruptEntry,
    IncompatibleCook,
    NeverCookReferenced,
    // 콘텐츠 서명(ContentSigning.ko.md 7절 — A1f): 서명 봉투를 신뢰할 수 없다(모르는 키·낮은
    // contentVersion·다른 scope·서명 불일치 — AssetDiagnostic::section이 ContentEnvelopeField)
    ContentNotTrusted,
    // 메시(MeshAnimationAssets.ko.md 11절 — MA1 원본 메시와 MA2a 빌드가 내는 것)
    InvalidMeshData,
    NonFiniteAttribute,
    UvOutOfRange,
    NonNormalizedWeights,
    TooManyInfluences,
    MaterialSlotFallback,
    NegativeBuildScale,
    // 스킨 메시 빌드(MeshAnimationAssets.ko.md 6.5절 — MA5a): 정보 InfluencesTrimmed, 경고
    // MissingInfluences.
    InfluencesTrimmed,
    MissingInfluences,
    // 단순 충돌(MeshAnimationAssets.ko.md 4.8절 — MA3a): 오류 InvalidCollisionData, 임포터 경고
    // UnsupportedCollisionShape.
    InvalidCollisionData,
    UnsupportedCollisionShape,
    // 임포트·설정(4.6절 — MA2a 메시 빌드가 처음 낸다)
    UnsupportedSetting,
    InvalidSetting,
    // 애니메이션(MeshAnimationAssets.ko.md 6.4·11절 — MA4a 스켈레톤이 내는 것)
    InvalidSkeletonData,
    SkeletonMismatch,
    // 클립(MeshAnimationAssets.ko.md 7.5절 — MA6a): 오류 InvalidClipData, 경고 UnknownBoneTrack,
    // 임포터 경고 EventPropertiesDropped.
    InvalidClipData,
    UnknownBoneTrack,
    EventPropertiesDropped,
    // Unreal 교환 묶음(UnrealExchange.ko.md 10.3·10.4절 — UX1a 리더·경로 대응·순서): 오류
    // CorruptBundle·UnsupportedBundleVersion·UnrealPathTooLong·UnrealPathCollision, 경고
    // UnknownField·UnsupportedAsset·MissingUnrealDependency, 정보 UnrealPathRenamed.
    CorruptBundle,
    UnsupportedBundleVersion,
    UnknownField,
    UnsupportedAsset,
    UnrealPathRenamed,
    UnrealPathTooLong,
    UnrealPathCollision,
    MissingUnrealDependency,
    // 감시 갱신(AssetSystem.ko.md 7.1절 16 — A2d3): 감시 루트(Content)가 사라졌다(색인을 비우고
    // 다시 나타나면 전체 검색).
    RootLost
};

// 진단 이름이다(예: CorruptPackage → "CorruptPackage"). 모르는 값은 "Unknown"이다. 반환 문자열은
// 정적 수명이다. 명령행 진단 줄 `<진단 코드>: <path>: <설명>`(AS 13절)과 로그가 쓴다.
const char* AssetDiagnosticName(AssetDiagnosticCode code) noexcept;

// 실패 하나의 분류와 위치다. 실패한 호출이 out 인자로 채운다(성공하면 건드리지 않는다).
struct AssetDiagnostic {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    // 원본 오류 코드다(호출이 돌려준 ErrorCode와 같다 — 예: 손상 판정의 InvalidArgument, 그래프
    // 검증이 낸 TypeMismatch, 버전 판정의 Unsupported).
    ErrorCode error = ErrorCode::None;
    // 진단 대상 에셋(패키지)이다. 모르면 null이다.
    AssetId asset{};
    // 다른 에셋이 원인이면 그 에셋이다(MissingDependency·TypeMismatch의 의존 대상). 없으면
    // null이다.
    AssetId related{};
    // 실패한 섹션 종류(PackageSectionKind 값, 0 = 헤더·섹션 표·섹션 밖)다.
    uint32 section = 0;
    // 섹션 안 위치(항목 index·byte offset 등 섹션마다 다르다 — 진단용)다.
    uint64 detail = 0;
};

// out이 있으면 진단을 채우고 error로 실패를 돌려준다. 패키지·매니페스트 reader의 실패 지점이 쓴다.
//
// 사용 예:
//   if (magicMismatch) {
//       return FailAsset<void>(diagnostic, AssetDiagnosticCode::CorruptPackage,
//                              ErrorCode::InvalidArgument, asset);
//   }
template <class T>
Result<T> FailAsset(AssetDiagnostic* out, AssetDiagnosticCode code, ErrorCode error,
                    AssetId asset = {}, uint32 section = 0, uint64 detail = 0,
                    AssetId related = {}) noexcept {
    if (out != nullptr) {
        out->code = code;
        out->error = error;
        out->asset = asset;
        out->related = related;
        out->section = section;
        out->detail = detail;
    }

    return Result<T>::Failure(error);
}

} // namespace rocks
