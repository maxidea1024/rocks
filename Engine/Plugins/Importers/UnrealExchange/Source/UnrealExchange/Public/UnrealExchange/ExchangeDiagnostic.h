#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/Json.h>
#include <Core/Text/String.h>
#include <UnrealExchange/UnrealExchangeFormat.h>

// Unreal 교환 묶음 진단이다(Docs/Design/UnrealExchange 10.3·10.4절). 리더(ExchangeBundle.h)와
// 임포트 순서(ExchangeImportOrder.h)가 진단을 하나씩 쌓고, 호출자(명령·시험)는 끝난 뒤 목록을 본다.
//
// 사용 예:
//   ExchangeDiagnostics diagnostics(allocator);
//   auto read = ReadExchangeBundle(files, directory, {}, bundle, diagnostics);
//   for (const ExchangeDiagnostic& item : diagnostics.Items()) {
//       // AssetDiagnosticName(item.code), ExchangeIssueName(item.issue), item.text.View() ...
//   }

namespace rocks {

// 진단 수준이다. 코드마다 고정이다(10.3절 표 — ExchangeSeverityOf).
enum class ExchangeSeverity : uint8 {
    Error,
    Warning,
    Info
};

// 진단이 가리키는 파일 종류다.
enum class ExchangeFile : uint8 {
    None,       // 파일과 무관(경로 대응·순서)
    BundleJson, // bundle.json
    AssetJson,  // assets/<패키지 경로>.json
    Blob        // blobs/<2자>/<64자>.bin
};

// 진단의 세부 규칙이다(text가 무엇을 담는지 함께 적는다).
enum class ExchangeIssue : uint8 {
    None,
    ReadFailed,        // 파일을 열거나 읽지 못함(error = 파일 오류, text = 묶음 안 상대 경로)
    JsonSyntax,        // JSON 구문 오류(text = Core JSON 오류 설명, position = 위치)
    ByteOrderMark,     // UTF-8 BOM(3.2절은 BOM 없음)
    MissingKey,        // 필수 키 없음(text = 키)
    WrongType,         // 값 타입이 다름(text = 키)
    InvalidValue,      // 값 형식·범위 오류(text = 키)
    UnknownKey,        // 모르는 키(UnknownField, text = 키)
    NotSorted,         // 정렬·유일성 위반(text = 뒤 항목의 objectPath)
    ObjectPathSyntax,  // objectPath 문법 위반(text = 값)
    FileNameMismatch,  // 목록의 file이 objectPath에서 정한 이름과 다름(text = file)
    ManifestMismatch,  // 목록과 에셋 JSON의 objectPath·class, 의존 class와 대상 class가 다름
    RedirectConflict,  // redirect from이 에셋 경로와 겹치거나 to가 다른 from(text = from)
    BlobConflict,      // 같은 blob hex를 다른 bytes로 적음(text = hex)
    BlobSize,          // blob 파일 크기·참조 크기 불일치(text = hex)
    BlobHash,          // blob SHA-256 불일치(text = hex)
    BlobLayout,        // 가변 배치 파싱 실패(text = hex)
    LimitExceeded,     // 상한 초과(10.4절 16번, text = 키)
    Version,           // 모르는 formatVersion(UnsupportedBundleVersion)
    UnsupportedClass,  // 범위 밖 클래스(UnsupportedAsset, text = 클래스 이름)
    Renamed,           // 경로 정리로 이름이 바뀜(UnrealPathRenamed, text = 대응 경로)
    PathTooLong,       // 대응 경로가 상한 초과(UnrealPathTooLong, text = 대응 경로)
    PathCollision,     // 대응 경로가 다른 에셋과 겹침(UnrealPathCollision, related = 다른 에셋)
    InvalidRoot,       // --root가 논리 경로 규칙을 어김(InvalidPath, text = root)
    BuildCycle,        // Build 참조가 낀 순환(UnsupportedCycle)
    DepthLimit,        // 단위 깊이가 UnrealImportMaxDepth 초과(UnsupportedCycle)
    MissingTarget,     // 묶음·기존 에셋에 없는 Hard 대상(text = 대상 objectPath)
    FailedDependency,  // Hard 대상 에셋이 실패함(MissingDependency, related = 대상 에셋)
    FailedMember,      // 같은 저장 단위의 다른 에셋이 실패함(MissingDependency, related = 그 에셋)
    NotRegularFile,    // 일반 파일이 아님(디렉터리·장치·FIFO·link, text = 묶음 안 상대 경로)
    ExporterError,     // 플러그인이 error 진단을 냄(InvalidSource, text = 그 진단 code)
    AmbiguousExisting, // 기존 에셋 조회가 둘 이상(UnrealPathCollision, text = objectPath)
    HardCycle,         // Hard만의 순환 단위 — 다중 에셋 원자 저장 전까지(UnsupportedCycle)
    NoKindImporter,    // 교환 클래스의 임포터가 없음(UnsupportedAsset, text = 클래스 이름)
    PathTaken, // 새 에셋의 대응 경로를 다른 Rocks 에셋이 씀(UnrealPathCollision, text = 경로)
    // 종류별 임포터의 서비스 임포트가 실패함(10.6절 2번 — 코드·오류는 서비스 진단, text = 그 path)
    ServiceFailure
};

// 세부 규칙 이름이다(예: BlobHash → "BlobHash"). 모르는 값은 "Unknown"이다. 정적 수명이다.
ROCKS_UNREALEXCHANGE_API const char* ExchangeIssueName(ExchangeIssue issue) noexcept;

// 코드의 수준이다(10.3절 표). 이 모듈이 내지 않는 코드는 Error다.
ROCKS_UNREALEXCHANGE_API ExchangeSeverity ExchangeSeverityOf(AssetDiagnosticCode code) noexcept;

// 진단 하나다.
struct ExchangeDiagnostic {
    explicit ExchangeDiagnostic(Allocator& allocator) noexcept : text(allocator) {
    }

    // AssetSystem 4.6절 진단 이름(CorruptBundle 등)이다.
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ExchangeSeverity severity = ExchangeSeverity::Error;
    // 원본 오류 코드다(손상 CorruptData, 상한 CapacityExceeded, 버전 Unsupported, 파일 오류는
    // 그대로). 경고·정보는 None.
    ErrorCode error = ErrorCode::None;
    ExchangeIssue issue = ExchangeIssue::None;
    ExchangeFile file = ExchangeFile::None;
    // 묶음의 에셋 번호(ExchangeBundle::Assets)다. bundle.json 자체의 진단이면 ExchangeNoIndex다.
    uint32 asset = ExchangeNoIndex;
    // 관련 번호(다른 에셋 번호 등 — ExchangeIssue 설명)다. 없으면 ExchangeNoIndex다.
    uint32 related = ExchangeNoIndex;
    // JSON 안 위치다. JSON과 무관하면 byteOffset 0, 줄·열 1이다.
    JsonPosition position;
    // 키·objectPath·blob hex 같은 짧은 설명이다(ExchangeIssue 설명).
    String text;
};

// 진단 입력이다(ExchangeDiagnostics::TryAdd).
struct ExchangeDiagnosticInput {
    AssetDiagnosticCode code = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
    ExchangeIssue issue = ExchangeIssue::None;
    ExchangeFile file = ExchangeFile::None;
    uint32 asset = ExchangeNoIndex;
    uint32 related = ExchangeNoIndex;
    JsonPosition position;
    StringView text;
};

// 묶음 하나의 진단 목록이다. 상한 UnrealExchangeMaxDiagnostics를 넘는 진단은 담지 않고 개수만 센다.
// 한 스레드에서 쓴다.
class ROCKS_UNREALEXCHANGE_API ExchangeDiagnostics {
public:
    explicit ExchangeDiagnostics(Allocator& allocator) noexcept;

    ExchangeDiagnostics(const ExchangeDiagnostics&) = delete;
    ExchangeDiagnostics& operator=(const ExchangeDiagnostics&) = delete;

    // 진단을 더한다. 수준은 code로 정한다. text는 복사한다. 상한을 넘으면 담지 않고 Dropped를
    // 늘리며 성공한다. 실패는 할당 오류뿐이다(목록은 호출 전 그대로).
    Result<void> TryAdd(const ExchangeDiagnosticInput& input) noexcept;

    Span<const ExchangeDiagnostic> Items() const noexcept {
        return items_.View();
    }

    // 상한 때문에 담지 못한 진단 수다.
    uint64 Dropped() const noexcept {
        return dropped_;
    }

    // 담은 진단 가운데 수준이 severity인 것의 수다(Dropped 제외).
    usize Count(ExchangeSeverity severity) const noexcept;

    // code가 같은 진단의 수다(Dropped 제외).
    usize Count(AssetDiagnosticCode code) const noexcept;

    // 담은 진단 가운데 첫 code 진단이다. 없으면 null이다.
    const ExchangeDiagnostic* Find(AssetDiagnosticCode code) const noexcept;

    void Clear() noexcept;

private:
    Allocator* allocator_;
    Array<ExchangeDiagnostic> items_;
    uint64 dropped_ = 0;
};

} // namespace rocks
