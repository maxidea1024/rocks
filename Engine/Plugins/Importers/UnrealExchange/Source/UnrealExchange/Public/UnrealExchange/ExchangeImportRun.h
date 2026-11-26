#pragma once

#include <AssetDatabase/AssetDatabaseLock.h>
#include <AssetDatabase/AssetService.h>
#include <UnrealExchange/ExchangeImportOrder.h>

// Unreal 교환 임포트 실행이다(Docs/Design/UnrealExchange 10.5절 7·8번): 계획의 단위 순서대로
// 교환 클래스마다 등록한 종류별 임포터를 부르고, 실행 중 실패를 Hard 의존자에게 전파한다. 에셋을
// 실제로 만드는 종류별 임포터는 UX2b 이후 단계가 등록한다.
//
// 사용 예:
//   ExchangeKindTable kinds;
//   auto registered = kinds.Register(ExchangeClass::Texture2D, textureImporter);
//   ExchangeRunInput input;
//   input.bundle = &bundle;
//   input.plan = &plan;
//   input.kinds = &kinds;
//   ExchangeImportResult result(allocator);
//   auto ran = RunExchangeImport(input, false, result, diagnostics);
//   for (uint32 asset : result.Order()) { /* result.Outcome(asset) */ }

namespace rocks {

// --mesh-normals 값이다(5.3절 — 정적 메시가 UE 준비 원본을 쓰는지).
enum class ExchangeMeshNormals : uint8 {
    Unreal,
    Rocks
};

// 명령 옵션 가운데 종류별 임포터가 쓰는 것이다(10.2절).
struct ExchangeImportOptions {
    ExchangeMeshNormals meshNormals = ExchangeMeshNormals::Unreal;
};

// 종류별 임포터 하나의 요청이다(호출 동안만 유효하다).
struct ExchangeKindRequest {
    const ExchangeBundle* bundle = nullptr;
    const ExchangeImportPlan* plan = nullptr;
    // 임포트할 에셋 번호와 논리 경로(기존 에셋이면 그 경로)다.
    uint32 asset = ExchangeNoIndex;
    StringView path;
    // 다시 임포트할 기존 에셋이다(없으면 null id — 새 에셋).
    AssetId existing{};
    const ExchangeImportOptions* options = nullptr;
    // 에셋 번호마다 참조로 걸 AssetId다: 이번 실행에서 임포트했으면 그 id, 아니면 기존 에셋 id,
    // 둘 다 없으면 null id(9.3절 — 비우거나 실패).
    Span<const AssetId> resolved{};
    // 에셋 서비스와 잠금(쓰기)이다. 서비스의 경고는 warnings에 모인다.
    AssetService* service = nullptr;
    const AssetDatabaseLock* lock = nullptr;
    Array<EditorWarning>* warnings = nullptr;
    // 진단은 묶음 진단에 더한다(asset = 이 에셋 번호).
    ExchangeDiagnostics* diagnostics = nullptr;
};

// 교환 클래스 하나의 임포터다. 에셋을 만들거나 다시 임포트하고 그 AssetId를 돌려준다. 실패는 진단을
// 더하고 오류를 돌려준다(그 에셋만 실패 — 할당 실패는 실행 전체를 멈춘다).
class ROCKS_UNREALEXCHANGE_API ExchangeKindImporter {
public:
    virtual ~ExchangeKindImporter() = default;

    virtual Result<AssetId> Import(const ExchangeKindRequest& request) noexcept = 0;
};

// 교환 클래스마다 임포터 하나를 둔 표다(명령 하나 동안 — 임포터는 표보다 오래 산다).
class ROCKS_UNREALEXCHANGE_API ExchangeKindTable {
public:
    // 오류: Unsupported 클래스 InvalidArgument, 이미 등록한 클래스 AlreadyExists.
    Result<void> Register(ExchangeClass type, ExchangeKindImporter& importer) noexcept;

    // 클래스의 임포터다(없으면 null).
    ExchangeKindImporter* Find(ExchangeClass type) const noexcept;

private:
    static constexpr usize ClassCount = static_cast<usize>(ExchangeClass::Unsupported);

    ExchangeKindImporter* importers_[ClassCount] = {};
};

// 에셋 하나의 실행 결과다.
enum class ExchangeOutcomeState : uint8 {
    NotSelected,     // 고르지 않음(보고하지 않는다)
    Unsupported,     // 범위 밖 클래스(skipped unsupported)
    Skipped,         // 클래스의 임포터 없음(skipped no-importer)
    Failed,          // 계획·실행 실패(failure에 진단 코드)
    PlannedCreate,   // --dry-run: 새로 만들 것
    PlannedReimport, // --dry-run: 기존 에셋에 다시 임포트할 것
    Created,
    Reimported
};

struct ExchangeAssetOutcome {
    ExchangeOutcomeState state = ExchangeOutcomeState::NotSelected;
    // 만든·다시 임포트한(계획이면 기존) 에셋이다.
    AssetId id{};
    // 실패의 진단 코드와 원본 오류다(Failed만).
    AssetDiagnosticCode failure = AssetDiagnosticCode::None;
    ErrorCode error = ErrorCode::None;
};

// 실행 결과다: 에셋마다 결과와 보고 순서(계획의 단위·구성원 순, 그 뒤 고른 범위 밖 에셋을 묶음
// 순으로).
class ROCKS_UNREALEXCHANGE_API ExchangeImportResult {
public:
    explicit ExchangeImportResult(Allocator& allocator) noexcept
        : outcomes_(allocator), order_(allocator) {
    }

    ExchangeImportResult(const ExchangeImportResult&) = delete;
    ExchangeImportResult& operator=(const ExchangeImportResult&) = delete;

    const ExchangeAssetOutcome& Outcome(uint32 asset) const noexcept {
        return outcomes_[asset];
    }

    Span<const uint32> Order() const noexcept {
        return order_.View();
    }

    // state인 에셋 수다.
    usize Count(ExchangeOutcomeState state) const noexcept;

    void Clear() noexcept {
        outcomes_.Clear();
        order_.Clear();
    }

private:
    friend struct ExchangeRunAccess;

    Array<ExchangeAssetOutcome> outcomes_;
    Array<uint32> order_;
};

// 실행 입력이다(모두 실행 동안 빌린다). dry run은 kinds·service·lock을 쓰지 않는다(null 가능).
struct ExchangeRunInput {
    const ExchangeBundle* bundle = nullptr;
    const ExchangeImportPlan* plan = nullptr;
    const ExchangeKindTable* kinds = nullptr;
    const ExchangeImportOptions* options = nullptr;
    AssetService* service = nullptr;
    const AssetDatabaseLock* lock = nullptr;
    Array<EditorWarning>* warnings = nullptr;
};

// 계획을 실행한다(10.5절 8번). dryRun이면 임포터를 부르지 않고 계획 결과(PlannedCreate·
// PlannedReimport·Failed·Unsupported)만 채운다. 에셋 실패는 결과와 진단으로 알리고 성공한다.
//
// 반환: 할당 실패(진단 없이, out은 정해지지 않음). 실행 안 쓰기 오류는 그 에셋의 실패다.
ROCKS_UNREALEXCHANGE_API Result<void> RunExchangeImport(const ExchangeRunInput& input, bool dryRun,
                                                        ExchangeImportResult& out,
                                                        ExchangeDiagnostics& diagnostics) noexcept;

} // namespace rocks
