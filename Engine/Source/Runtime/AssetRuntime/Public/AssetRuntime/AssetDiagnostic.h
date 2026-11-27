#pragma once

#include <Core/Result.h>
#include <Object/AssetId.h>

namespace rocks {

// 에셋 진단 코드다(Docs/Design/AssetSystem 4.6절). API는 Result의 ErrorCode로 실패를 알리고,
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
    // 메시(Docs/Design/MeshAnimationAssets 11절 — MA1 원본 메시와 MA2a 빌드가 내는 것)
    InvalidMeshData,
    NonFiniteAttribute,
    UvOutOfRange,
    NonNormalizedWeights,
    TooManyInfluences,
    MaterialSlotFallback,
    NegativeBuildScale,
    // 스킨 메시 빌드(Docs/Design/MeshAnimationAssets 6.5절 — MA5a): 정보 InfluencesTrimmed, 경고
    // MissingInfluences.
    InfluencesTrimmed,
    MissingInfluences,
    // 단순 충돌(Docs/Design/MeshAnimationAssets 4.8절 — MA3a): 오류 InvalidCollisionData, 임포터
    // 경고 UnsupportedCollisionShape.
    InvalidCollisionData,
    UnsupportedCollisionShape,
    // 임포트·설정(4.6절 — MA2a 메시 빌드가 처음 낸다)
    UnsupportedSetting,
    InvalidSetting,
    // 애니메이션(Docs/Design/MeshAnimationAssets 6.4·11절 — MA4a 스켈레톤이 내는 것)
    InvalidSkeletonData,
    SkeletonMismatch,
    // 클립(Docs/Design/MeshAnimationAssets 7.5절 — MA6a): 오류 InvalidClipData, 경고
    // UnknownBoneTrack, 임포터 경고 EventPropertiesDropped.
    InvalidClipData,
    UnknownBoneTrack,
    EventPropertiesDropped,
    // Unreal 교환 묶음(UnrealExchange 10.3·10.4절 — UX1a 리더·경로 대응·순서): 오류
    // CorruptBundle·UnsupportedBundleVersion·UnrealPathTooLong·UnrealPathCollision, 경고
    // UnknownField·UnsupportedAsset·MissingUnrealDependency, 정보 UnrealPathRenamed. 종류별
    // 임포트(10.6절·4.6절 — UX2b): 오류 UnsupportedSourceLayout(다층·UDIM·위도경도 큐브 원본), 경고
    // GammaMismatch(원본 층 감마와 SRGB·bUseLegacyGamma 설정이 다름 — 설정을 따른다). 메시
    // 임포트(5.5절 — UX3b): 경고 UnrealNameNormalized(메시 원본 속성 이름·Name 값을 NFC로 바꿨다 —
    // detail = 바뀐 수).
    CorruptBundle,
    UnsupportedBundleVersion,
    UnknownField,
    UnsupportedAsset,
    UnrealPathRenamed,
    UnrealPathTooLong,
    UnrealPathCollision,
    MissingUnrealDependency,
    UnsupportedSourceLayout,
    GammaMismatch,
    UnrealNameNormalized,
    // 감시 갱신(AssetSystem 7.1절 16 — A2d3): 감시 루트(Content)가 사라졌다(색인을 비우고
    // 다시 나타나면 전체 검색).
    RootLost,
    // 별칭(AssetSystem 4.3절, 7.1절 17 — A2e1): 경고 AliasResolved(옛 경로를 별칭으로
    // 풀었다)· AliasInUse(prune이 만료 항목의 텍스트 사용을 찾아 남겼다)·AliasSearchTruncated(prune
    // 검색이 상한에 닿아 아무것도 지우지 않았다), 오류 CorruptAliasTable(Config/AssetAliases.json
    // 형식 위반).
    AliasResolved,
    AliasInUse,
    AliasSearchTruncated,
    CorruptAliasTable,
    // 삭제(AssetSystem 7.1절 17 — A2e3): 다른 패키지가 대상을 참조한다(디스크 참조자 —
    // InvalidArgument, 열린 패키지의 메모리 참조 — Busy, related = 첫 참조자).
    ReferencedBy,
    // 고아 출력(7.1절 17 — A2e4, 경고): 참조 기록의 owner가 이 출력을 소유하지 않는다.
    OrphanedOutput,
    // 임포트 원본(AssetSystem 4.5·4.6절, 7.1절 18 — A2f): 원본 파일·루트가
    // 없음(MissingSource), 원본 내용이 기록과 다름(SourceChanged — 상태), 원본·원본 루트 표를
    // 읽거나 해석하지 못함 (InvalidSource), 원본 형식을 받는 임포터가
    // 없음(UnsupportedSourceFormat), 원본이 어느 루트 아래도 아니라 절대 경로로
    // 기록함(AbsoluteSourcePath — 경고).
    MissingSource,
    SourceChanged,
    InvalidSource,
    UnsupportedSourceFormat,
    AbsoluteSourcePath,
    // 임포트·설정(4.6절, 7.1절 18 — A2f): 등록 임포터가 기록보다 새 버전(ImporterOutdated — 상태),
    // 임포트 옵션 값·역할이 잘못됨(InvalidImportOption), 옵션도 설정도 아닌 이름(UnknownProperty).
    ImporterOutdated,
    InvalidImportOption,
    UnknownProperty,
    // 쿠킹 verify(AssetSystem 10.2절 결정 12 — A4e1): 처리기가 너겟 적중 없이 다시 만든
    // 결과가 적중 결과와 byte가 다르다(오류 — 키가 입력을 다 담지 않았거나 처리기가 비결정적이다).
    NuggetMismatch,
    // 클래스 기본값(RuntimeClassMetadata 'A5c 지문 연결'): 클래스의 기본값 레코드를 만들 수
    // 없다(타입 계약 위반 — 비결정 생성자·non-null 참조 기본값 등, related = 그 TypeId). 그
    // 클래스를 쓰는 대상의 cookKey 오류, 쓰는 대상이 없으면 쿠킹 보고서 경고다(다시 쿠킹으로 풀리지
    // 않는다 — UnsupportedVersion과 나눈다).
    InvalidClassDefaults,
    // 텍스처 원본(AssetSystem 12.2·12.9절 — A5e 디코더): 손실 원본 형식(LossySource — 오류, JPEG
    // 밖의 손실 형식), sRGB가 아닌 색 표시(NonSrgbSource — 경고, 변환하지 않는다), 버린 추가
    // 채널(IgnoredSourceChannels — 경고, detail = 버린 수).
    LossySource,
    NonSrgbSource,
    IgnoredSourceChannels,
    // 텍스처 원본(AssetSystem 12.10절 — A5f 디코더): 버린 원본 mip(IgnoredSourceMips — 경고,
    // detail = slice당 버린 mip 수 — mip 0만 쓴다), 버린 추가 이미지(IgnoredSourceImages — 경고,
    // detail = 버린 수 — TIFF 다음 IFD·EXR 다른 part).
    IgnoredSourceMips,
    IgnoredSourceImages,
    // 텍스처 빌드(AssetSystem 12.13절 — A5i): SRGB를 무시함(SrgbIgnored — 경고, 선형 데이터 종류나
    // float 출력의 SRGB on), Address Wrap인 PreserveBorder(PreserveBorderWithWrap — 경고).
    SrgbIgnored,
    PreserveBorderWithWrap,
    // 쿠킹 너겟(AssetSystem 12.14절 — A5j): 같은 키에 다른 값이 이미 있다(NuggetConflict — 경고,
    // 처리기 비결정성·키 누락 진단, 9절 Put 판정. 이번 결과를 쓰고 기존 항목은 둔다).
    NuggetConflict,
    // 텍스처 원본 묶음(AssetSystem 12.14절 A5j2): 원본 재구성이 원본을 비웠다(SourceCleared — 경고,
    // Volume 원본 2D·타일이 원본을 만들지 못함, UE와 같이 비우고 알린다).
    SourceCleared,
    // 사운드 원본(AudioSystem 3.36.2절 — AU4): 모르는 chunk를 건너뛰었다(IgnoredSourceChunks —
    // 경고, detail = 건너뛴 chunk 수).
    IgnoredSourceChunks,
    // 텍스처 압축 형식(AssetSystem 12.17절 TD11·TD12 — TC2): 고른 압축 형식의 인코더가 아직 없어
    // 대체했다(EncoderUnavailable — 경고, detail = 원래 PixelFormat 번호), 프로필 backend가 고른
    // 형식을 샘플링 보장하지 않아 대체했다(PlatformFormatFallback — 경고, detail = 바꾸기 전
    // PixelFormat 번호).
    EncoderUnavailable,
    PlatformFormatFallback,
    // 커브 에셋(Docs/Design/CurveAssets 5.1·5.3절 — CA3·CA4): 임포트가 원본의 행·항목 하나 또는
    // 행의 칸 일부를 건너뛰었다(CurveImportRowSkipped — 경고, detail = CSV 물리 줄 번호 또는 JSON
    // 배열 원소 번호, 1부터), 쿠킹이 커브 표의 모드가 쓰지 않는 맵을 비웠다(CurveTableUnusedRows —
    // 경고, 쿠킹 진단 setting = 버린 행 수).
    CurveImportRowSkipped,
    CurveTableUnusedRows
};

// 진단 이름이다(예: CorruptPackage → "CorruptPackage"). 모르는 값은 "Unknown"이다. 반환 문자열은
// 정적 수명이다. 명령행 진단 줄 `<진단 코드>: <path>: <설명>`(AS 13절)과 로그가 쓴다.
ROCKS_ASSETRUNTIME_API const char* AssetDiagnosticName(AssetDiagnosticCode code) noexcept;

// 경고·정보 수준 진단인지다(4.6절·MeshAnimationAssets 11절·UnrealExchange 10.3절 표의 경고·정보 —
// 실패가 아니라 결과에 남는다). 임포터 경고(AssetImportContext::Warn)는 이 코드만 받는다.
ROCKS_ASSETRUNTIME_API bool AssetDiagnosticIsWarning(AssetDiagnosticCode code) noexcept;

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
