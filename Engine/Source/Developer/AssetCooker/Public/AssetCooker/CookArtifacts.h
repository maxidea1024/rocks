#pragma once

#include <AssetCooker/CookConfig.h>
#include <AssetCooker/CookTargets.h>
#include <AssetNugget/NuggetKey.h>
#include <AssetRuntime/CookedManifest.h>
#include <Core/Text/StringBuilder.h>

// 비에셋 산출물 단계다(Docs/Design/AssetSystem 10.3절 3·5·6항, 10.3.1절 A4f3). 셰이더
// 라이브러리처럼 에셋이 아닌 쿠킹 산출물을 만드는 단계를 구현 모듈(ShaderCompiler 등)이 모듈 시작
// hook에서 표에 등록하고, CookProject가 에셋 쿠킹 뒤·게시 전에 부른다. 단계는 sink가 준 staging
// 경로에 파일을 쓰고 Produce로 넘긴다 — 크기·SHA-256·게시 이름은 AssetCooker가 그 파일에서 직접
// 계산한다.
//
// 사용 예(구현 모듈의 시작·종료 hook):
//   class ShaderLibraryStage final : public CookArtifactStage { ... };
//   ShaderLibraryStage stage;
//   Result<void> Startup() { return CookArtifactStages().Register(stage); }
//   void Shutdown() { (void)CookArtifactStages().Unregister(stage); }
namespace rocks {

class NuggetCache;

// 단계 실행 요청이다(모두 빌림 — Run 동안 살아 있다).
struct CookArtifactRequest {
    Allocator* allocator = nullptr;
    platform::FileSystem* files = nullptr;
    // 병합한 대상 프로필(셰이더 칸 — graphicsBackends·shaderFormats)과 쿠킹 규칙이다.
    const TargetProfile* profile = nullptr;
    const CookSettings* settings = nullptr;
    StringView cookedRoot{};
    // `<쿠킹 루트>/.staging`이다(파일 경로는 CookArtifactSink::NewStagingPath로 받는다).
    StringView stagingDirectory{};
    // 너겟 캐시다. null이면 단계가 정한다 — engineOnly면 단계가 엔진 너겟 root(셰이더 쪽
    // ResolveShaderNuggetRoot)로 연 캐시를 쓰고 Run 안에서 닫는다, 아니면 너겟 없이 만든다(10.3.1절
    // A4f4 3번).
    NuggetCache* nuggets = nullptr;
    // 엔진 전용 쿠킹이면 `Engine` artifact만 만든다(A4f4 `asset-cook --engine`).
    bool engineOnly = false;
    // 참이면 너겟 적중도 다시 만들어 대조한다(CookContext::verifyNuggets와 같은 뜻).
    bool verifyNuggets = false;
    // 이전 매니페스트의 같은 종류 artifact 중 게시 파일 크기·SHA-256이 맞는 것이다(10.3절 6항).
    // 같은 (이름, 형식)·같은 cookKey면 Reuse로 그대로 쓴다.
    Span<const CookedManifestArtifact> reusable{};
};

// 새로 만든 artifact 하나다.
struct CookArtifactOutput {
    // CookedArtifactEngineName 또는 CookedArtifactProjectName이다.
    StringView name{};
    StringView formatId{};
    // 단계가 정의하는 재사용 키다(셰이더 결과를 바꾸는 입력 전부 — CookSettings 칸 포함, 10.3.1절
    // A4f3 1번).
    Sha256Digest cookKey{};
    // NewStagingPath가 준 경로에 다 쓰고 닫은 파일이다.
    StringView stagingFile{};
};

// 단계의 출력 sink다(CookProject가 구현한다). 계약 위반은 InvalidArgument다 — 단계는 그 오류를 Run
// 결과로 그대로 돌려준다(CookProject가 멈추고 CookProjectResult::stageFailure에 원인을 적는다).
class CookArtifactSink {
public:
    virtual ~CookArtifactSink() = default;

    // 이번 실행에서 유일한 staging 파일 경로(`<staging>/artifact-<번호>.tmp`)를 out 뒤에 붙인다.
    // 오류: 할당 실패.
    virtual Result<void> NewStagingPath(StringBuilderBase& out) noexcept = 0;

    // 새 artifact를 게시 위치(`Shaders/<이름>-<형식>-<hash>.rshl`)로 옮긴다. 오류(InvalidArgument):
    // 이름·형식 문법, 프로필 backend 표에 없는 형식, engineOnly인데 `Engine`이 아님, 이번 실행의
    // 같은 (이름, 형식) 두 번, NewStagingPath가 주지 않았거나 이미 쓴 경로, 파일 0 byte·
    // CookedArtifactMaxBytes 초과. 그 밖에 파일 IO 오류, 할당 실패.
    virtual Result<void> Produce(const CookArtifactOutput& output) noexcept = 0;

    // 재사용 후보(request.reusable) 하나를 그대로 쓴다. 오류: 후보가 아님·같은 키 두 번
    // InvalidArgument, 할당 실패.
    virtual Result<void> Reuse(const CookedManifestArtifact& artifact) noexcept = 0;

    // job 실패·경고를 보고서에 남긴다(오류가 하나라도 있으면 게시하지 않는다). 오류: 할당 실패.
    virtual Result<void> Report(const CookDiagnostic& diagnostic) noexcept = 0;

    // 보고서 절(`stageStats`)에 값 하나를 적는다. 오류: 할당 실패.
    virtual Result<void> ReportStat(StringView name, uint64 value) noexcept = 0;
};

// 비에셋 산출물 단계다.
class CookArtifactStage {
public:
    virtual ~CookArtifactStage() = default;

    virtual CookArtifactKind Kind() const noexcept = 0;
    // 단계 정체성이다(보고서·실패 줄에 적는다, 너겟 키의 처리기 id로도 쓴다).
    virtual NuggetProcessorId Identity() const noexcept = 0;
    // 산출물을 만든다. job 실패는 sink.Report로 남기고 성공으로 끝낸다. 오류: sink 오류(그대로),
    // 파일 IO 오류, 할당 실패.
    virtual Result<void> Run(const CookArtifactRequest& request,
                             CookArtifactSink& sink) noexcept = 0;
};

// 종류별 단계 표다. 등록·해제는 모듈 시작·종료 hook에서만 하고(ModuleLoader가 한 스레드에서 부른다)
// 쿠킹은 읽기만 해서 잠금이 없다. 단계가 도는 동안(사용 중 계수) 등록·해제는 Busy다.
class ROCKS_ASSETCOOKER_API CookArtifactStageTable final {
public:
    CookArtifactStageTable() noexcept = default;
    CookArtifactStageTable(const CookArtifactStageTable&) = delete;
    CookArtifactStageTable& operator=(const CookArtifactStageTable&) = delete;

    // 오류: 모르는 종류 InvalidArgument, 같은 종류가 이미 있음 AlreadyExists, 사용 중 Busy.
    Result<void> Register(CookArtifactStage& stage) noexcept;

    // 오류: 그 종류에 이 객체가 없음 NotFound, 사용 중 Busy.
    Result<void> Unregister(CookArtifactStage& stage) noexcept;

    // kind의 단계다(없으면 null).
    CookArtifactStage* Find(CookArtifactKind kind) const noexcept;

    // 사용 중 계수다(CookProject가 단계 Run 동안 CookArtifactStageUse로 올린다).
    void BeginUse() noexcept {
        ++uses_;
    }

    void EndUse() noexcept {
        --uses_;
    }

    bool InUse() const noexcept {
        return uses_ != 0;
    }

private:
    // 칸 번호 = 종류 값이다(0은 쓰지 않는다).
    static constexpr usize KindSlots = static_cast<usize>(CookArtifactKind::ShaderLibrary) + 1;

    CookArtifactStage* stages_[KindSlots] = {};
    uint32 uses_ = 0;
};

// 표 사용 구간이다(생성에서 BeginUse, 소멸에서 EndUse).
class CookArtifactStageUse final {
public:
    explicit CookArtifactStageUse(CookArtifactStageTable& table) noexcept : table_(table) {
        table_.BeginUse();
    }

    ~CookArtifactStageUse() {
        table_.EndUse();
    }

    CookArtifactStageUse(const CookArtifactStageUse&) = delete;
    CookArtifactStageUse& operator=(const CookArtifactStageUse&) = delete;

private:
    CookArtifactStageTable& table_;
};

// 프로세스 단계 표다(AssetCooker 모듈 소유 — asset-cook 명령이 CookProjectRequest::stages로
// 넘긴다).
ROCKS_ASSETCOOKER_API CookArtifactStageTable& CookArtifactStages() noexcept;

// 계약 위반·단계 오류의 원인 호출이다.
enum class CookArtifactCall : uint8 {
    None,
    Run,
    NewStagingPath,
    Produce,
    Reuse,
    Report,
    ReportStat
};

// 호출 이름("Produce" 등 — 명령 실패 줄)이다.
ROCKS_ASSETCOOKER_API StringView CookArtifactCallName(CookArtifactCall call) noexcept;

// 단계가 CookProject를 멈춘 원인이다(call None이면 없음).
struct CookArtifactFailure {
    explicit CookArtifactFailure(Allocator& allocator) noexcept : formatId(allocator) {
    }

    CookArtifactCall call = CookArtifactCall::None;
    // 단계 정체성의 processor Guid다.
    Guid stage{};
    // Produce·Reuse의 형식 id다(그 밖은 빔).
    String formatId;
    ErrorCode error = ErrorCode::None;
};

} // namespace rocks
