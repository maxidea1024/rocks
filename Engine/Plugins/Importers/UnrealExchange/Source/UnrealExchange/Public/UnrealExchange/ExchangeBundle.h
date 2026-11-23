#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <Platform/File.h>
#include <UnrealExchange/ExchangeDiagnostic.h>

// Unreal 교환 묶음 리더다(Docs/Design/UnrealExchange.ko.md 3절, 10.4절). UE 쪽 RocksExport가 쓴
// `<이름>.rxbundle/` 디렉터리의 bundle.json과 에셋 JSON을 읽어 공통 스키마(3.4·3.5절)·정렬·blob
// 참조(3.3절)를 검증하고, 에셋마다 종류별 단계(UX2–UX8)가 읽을 settings·data JSON을 넘긴다.
//
// 언제 쓰나: asset-import-unreal 명령(UX1b)이 묶음을 열 때, 시험이 손으로 만든 묶음을 검증할 때.
// 에셋 DB에는 쓰지 않는다. 경로 대응·임포트 순서는 ExchangePaths.h·ExchangeImportOrder.h다.
//
// 사용 예:
//   ExchangeBundle bundle(allocator);
//   ExchangeDiagnostics diagnostics(allocator);
//   if (auto read = ReadExchangeBundle(platform::PhysicalFileSystem(), "D:/Export/Props.rxbundle",
//                                      {}, bundle, diagnostics);
//       !read) {
//       return read; // 묶음 수준 오류 — diagnostics에 이유
//   }
//   for (const ExchangeAsset& asset : bundle.Assets()) {
//       if (asset.state == ExchangeAssetState::Ready) { /* asset.settings·asset.data */ }
//   }

namespace rocks {

// 교환 클래스다(2절 UE 클래스 이름에서 U를 뺀 것). 그 밖의 클래스는 Unsupported다.
enum class ExchangeClass : uint8 {
    Texture2D,
    TextureCube,
    Texture2DArray,
    TextureCubeArray,
    VolumeTexture,
    StaticMesh,
    Skeleton,
    SkeletalMesh,
    AnimSequence,
    Material,
    MaterialInstanceConstant,
    MaterialFunction,
    MaterialFunctionInstance,
    MaterialParameterCollection,
    Unsupported
};

// 교환 클래스 이름이다(예: StaticMesh → "StaticMesh"). Unsupported·모르는 값은 "Unsupported"다.
ROCKS_UNREALEXCHANGE_API const char* ExchangeClassName(ExchangeClass type) noexcept;

// 이름에 맞는 교환 클래스다(대소문자 구분). 없으면 Unsupported다.
ROCKS_UNREALEXCHANGE_API ExchangeClass FindExchangeClass(StringView name) noexcept;

// 텍스처 5종이면 참이다.
ROCKS_UNREALEXCHANGE_API bool IsExchangeTextureClass(ExchangeClass type) noexcept;

// 머티리얼 계열(Material·MaterialInstanceConstant·MaterialFunction·MaterialFunctionInstance)이면
// 참이다.
ROCKS_UNREALEXCHANGE_API bool IsExchangeMaterialFamily(ExchangeClass type) noexcept;

// 묶음·기존 에셋에 없는 Hard 대상을 엔진 기본값으로 비워 둘 수 있는 클래스(텍스처 5종·Material·
// MaterialInstanceConstant)면 참이다(9.3절 — 그 밖의 대상은 MissingDependency).
ROCKS_UNREALEXCHANGE_API bool IsExchangeReplaceableTarget(ExchangeClass type) noexcept;

// blob 배치다(3.3절). 앞 7개는 고정 크기 원소, 뒤 3개는 가변 길이 배치(MeshAnimationAssets
// 3.4.1절)다.
enum class ExchangeBlobLayout : uint8 {
    U8,
    U16,
    U32,
    I32,
    F16,
    F32,
    F64,
    Names,
    BoneInfluences,
    SparseVec3
};

// 배치 이름이다(예: F32 → "f32", BoneInfluences → "boneInfluences"). 정적 수명이다.
ROCKS_UNREALEXCHANGE_API const char* ExchangeBlobLayoutName(ExchangeBlobLayout layout) noexcept;

// 고정 크기 원소의 byte 수다. 가변 길이 배치는 0이다.
ROCKS_UNREALEXCHANGE_API uint32 ExchangeBlobElementBytes(ExchangeBlobLayout layout) noexcept;

// blob 참조 하나다(3.3절). 리더가 형식(hex·크기 곱·components)을 확인한 값이다.
struct ExchangeBlobRef {
    Sha256Digest digest{};
    uint64 bytes = 0;
    uint64 count = 0;
    uint32 components = 0;
    ExchangeBlobLayout layout = ExchangeBlobLayout::U8;
};

enum class ExchangeDependencyKind : uint8 {
    Hard,
    Soft
};

// 에셋 JSON의 의존 항목 하나다(3.5절). 문자열은 그 에셋의 JSON 문서를 가리킨다.
struct ExchangeDependency {
    StringView objectPath;
    // 대상 클래스 이름(원문)과 교환 클래스다.
    StringView className;
    ExchangeClass targetClass = ExchangeClass::Unsupported;
    ExchangeDependencyKind kind = ExchangeDependencyKind::Hard;
    // redirect를 푼 대상이 묶음에 있으면 그 에셋 번호, 없으면 ExchangeNoIndex다.
    uint32 target = ExchangeNoIndex;
};

enum class ExchangeAssetState : uint8 {
    Ready,      // 공통 스키마·blob 검증을 통과
    Failed,     // 에셋 수준 오류(진단에 이유)
    Unsupported // 범위 밖 클래스 — 에셋 JSON을 읽지 않음
};

// 묶음의 에셋 하나다. objectPath·className·file은 bundle.json 문서를, 나머지 문자열·JSON 값은
// 이 에셋의 문서를 가리킨다(묶음이 살아 있는 동안 유효).
struct ExchangeAsset {
    explicit ExchangeAsset(Allocator& allocator) noexcept
        : dependencies(allocator), blobs(allocator) {
    }

    StringView objectPath;
    StringView className;
    StringView file;
    ExchangeClass exchangeClass = ExchangeClass::Unsupported;
    ExchangeAssetState state = ExchangeAssetState::Failed;
    // Ready일 때만 채운다.
    StringView ueClass;
    JsonValue settings;
    JsonValue data;
    // 플러그인이 낸 진단 배열(3.5절 diagnostics — 항목 형식은 리더가 확인했다)이다.
    JsonValue exporterDiagnostics;
    Array<ExchangeDependency> dependencies;
    // settings·data에서 처음 나온 순서의 서로 다른 blob 참조다.
    Array<ExchangeBlobRef> blobs;
    UniquePtr<JsonDocument> document;
};

// redirect 하나다(3.4절, 문자열은 bundle.json 문서).
struct ExchangeRedirect {
    StringView from;
    StringView to;
};

// bundle.json의 exporter 항목이다.
struct ExchangeExporter {
    StringView plugin;
    StringView pluginVersion;
    StringView engineVersion;
    StringView engineCommit;
};

struct ExchangeReadOptions {
    // 참이면 읽는 동안 모든 blob 파일의 크기·SHA-256·가변 배치를 확인한다(10.4절 12번). 거짓이면
    // 참조 형식만 보고 파일은 LoadExchangeBlob 때 확인한다.
    bool verifyBlobs = true;
    // 묶음의 JSON 파일(bundle.json + 에셋 JSON) 크기 합 상한이다(10.4절 18번). 넘는 파일부터는 읽지
    // 않는다(bundle.json이면 묶음 수준, 에셋 JSON이면 그 에셋 LimitExceeded). 시험이 줄여 쓴다.
    uint64 maxBundleJsonBytes = UnrealExchangeMaxBundleJsonBytes;
};

// 읽은 묶음이다. ReadExchangeBundle이 채우고 그 뒤로 바뀌지 않으므로 여러 스레드가 const로 함께
// 읽어도 된다. 읽기에 쓴 FileSystem은 묶음보다 오래 살아야 한다(LoadExchangeBlob이 쓴다).
class ROCKS_UNREALEXCHANGE_API ExchangeBundle {
public:
    explicit ExchangeBundle(Allocator& allocator) noexcept;

    ExchangeBundle(const ExchangeBundle&) = delete;
    ExchangeBundle& operator=(const ExchangeBundle&) = delete;

    uint32 formatVersion = 0;
    ExchangeExporter exporter;
    bool substrate = false;
    bool retainJpegFormat = false;
    // 정보용(3.4절)이다. 없으면 빈 view·무효 JsonValue다.
    StringView createdUtc;
    JsonValue environment;

    // objectPath 순(UTF-8 byte)의 에셋이다.
    Span<const ExchangeAsset> Assets() const noexcept {
        return assets_.View();
    }

    // from 순의 redirect다.
    Span<const ExchangeRedirect> Redirects() const noexcept {
        return redirects_.View();
    }

    // 묶음 디렉터리(읽을 때 준 값)다.
    StringView Directory() const noexcept {
        return directory_.View();
    }

    // objectPath가 같은 에셋 번호다(redirect를 풀지 않는다). 없으면 ExchangeNoIndex다.
    uint32 FindAsset(StringView objectPath) const noexcept;

    // objectPath가 redirect from이면 to, 아니면 objectPath 그대로다.
    StringView ResolveRedirect(StringView objectPath) const noexcept;

    // 읽기 전 상태로 되돌린다.
    void Clear() noexcept;

private:
    friend struct ExchangeBundleAccess;

    Allocator* allocator_;
    platform::FileSystem* files_ = nullptr;
    String directory_;
    UniquePtr<JsonDocument> manifest_;
    Array<ExchangeAsset> assets_;
    Array<ExchangeRedirect> redirects_;
};

// directory의 묶음을 읽어 out에 채운다. out은 먼저 비운다.
//
// 인자:
// - files: 파일 접근(시험은 메모리 파일 시스템). 묶음보다 오래 살아야 한다.
// - directory: 묶음 디렉터리(`.../Props.rxbundle`). 그 아래 상대 경로를 `/`로 붙여 연다.
// - options: blob 파일 검증 여부.
// - out: 결과. 할당은 out의 allocator로 한다.
// - diagnostics: 진단을 더한다(비우지 않는다).
//
// 반환: 묶음 수준 오류(bundle.json 읽기 실패·손상·버전·정렬·상한, 10.4절 2번)는 실패이고 out은 빈
// 상태다(CorruptBundle은 InvalidArgument·CapacityExceeded 또는 파일 오류, 버전은 Unsupported).
// 에셋 수준 오류는 성공이고 그 에셋이 Failed다. 할당 실패는 진단 없이 그 오류다.
ROCKS_UNREALEXCHANGE_API Result<void> ReadExchangeBundle(platform::FileSystem& files,
                                                         StringView directory,
                                                         const ExchangeReadOptions& options,
                                                         ExchangeBundle& out,
                                                         ExchangeDiagnostics& diagnostics) noexcept;

// asset 번호 에셋의 blob 참조 하나를 out에 읽는다(out은 먼저 비운다). 크기·SHA-256·가변 배치를 다시
// 확인한다(읽기와 임포트 사이에 파일이 바뀌어도 잡는다). 실패하면 diagnostics에 CorruptBundle(asset
// 번호, 파일 Blob)을 더하고 InvalidArgument 또는 파일 오류를 돌려주며 out은 빈 상태다. 할당 실패는
// 진단 없이 그 오류다. const 묶음을 여러 스레드가 함께 써도 된다(diagnostics·out은 호출마다 따로).
ROCKS_UNREALEXCHANGE_API Result<void> LoadExchangeBlob(const ExchangeBundle& bundle, uint32 asset,
                                                       const ExchangeBlobRef& blob,
                                                       Array<uint8>& out,
                                                       ExchangeDiagnostics& diagnostics) noexcept;

// 교환 JSON의 실수 값을 읽는다(3.2절): JSON 숫자이거나 문자열 "nan"·"inf"·"-inf"다. 그 밖은
// InvalidArgument다. 종류별 단계가 settings·data를 읽을 때 쓴다.
//
// 사용 예:
//   double roughness = 0.0;
//   if (auto read = ReadExchangeNumber(settings.Find("Roughness"), roughness); !read) { ... }
ROCKS_UNREALEXCHANGE_API Result<void> ReadExchangeNumber(JsonValue value, double& out) noexcept;

} // namespace rocks
