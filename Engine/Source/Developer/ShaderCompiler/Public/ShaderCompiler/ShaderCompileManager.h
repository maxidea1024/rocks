#pragma once

#include <AssetNugget/NuggetCache.h>
#include <Base/Types.h>
#include <Core/CancelToken.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <ShaderCompiler/ShaderCompileOptions.h>
#include <ShaderCompiler/ShaderJobKey.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCompiler/ShaderPreprocessor.h>
#include <ShaderCompiler/ShaderSourceTree.h>
#include <ShaderCompiler/ShaderWorkerChannel.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 셰이더 컴파일 관리자와 워커 풀이다(Docs/Design/ShaderSystem 10절, 11.1절 SC4·SC11·SC12, UE
// FShaderCompilingManager 대응). job마다 전처리 → (system 도구 형식이면) 도구 조회 → 키 → 너겟 일괄
// 조회 → 미적중을 묶음으로 나눠 워커 채널에 보냄 → 결과 게시 순서로 돈다.
//
// - 조정: Compile을 부른 스레드가 조정한다(너겟 요청을 시작하고 기다린다). 그 스레드는 너겟 요청의
//   scheduler worker가 아니어야 한다(NuggetRequest.h 계약). 결과는 job 순서 그대로다.
// - 워커 풀: 채널 workerCount개(기본 min(논리 코어 - 1, 8), 최소 1). 워커는 첫 묶음에서 띄운다(찬
//   캐시는 워커 0개). 묶음 상한은 job maxBatchJobs개·원문 합 maxBatchSourceBytes다.
// - 작업 디렉터리: `<workRoot>/<PID>/<n>/`. 시작할 때 자기 `<PID>`를 비우고, 다른 `<PID>` 가운데 그
//   프로세스가 살아 있지 않은 것을 지운다(PID 재사용으로 살아 보이는 것은 남긴다 — SC4).
// - 너겟: bucket shader, key = job 키. 값 0(Code)이 없는 적중은 손상으로 다시 컴파일한다.
//   Miss·Corrupt·DigestChanged·Dropped는 다시 컴파일해 게시, Cancelled는 job 취소, 캐시 실패는
//   required면 job 오류(CacheFailed)이고 아니면 진단만이다. Put Conflict는 결정성 위반으로 통계를
//   남긴다. 실패 job은 게시하지 않는다.
// - 진행(진행 보고 12절): 전체·완료·실패·취소·캐시 적중 job 수와 워커 job 줄의 잠정 완료 수를 원자
//   계수로 내놓는다(Progress). 묶음이 끝나면 잠정 값을 실제 결과로 바꾼다. Compile마다 control의
//   sink가 이 호출의 job 수(added 한 번)와 끝난 수(finished — 잠정 완료 포함, 높은 물 표시라 늘
//   때만)를 받는다. Compile이 돌아올 때 finished 합 = added다(실패·취소 포함).
// - 취소(11.1절 PG5 메모): control.cancel을 job 준비마다·너겟 조회 전·묶음을 맡길 때 본다. 진행 중
//   묶음은 채널이 워커를 강제 종료한다. 취소 뒤 남은 job은 Cancelled이고 이미 성공한 job은 그대로
//   게시한다. token으로 취소된 job이 있으면 Compile은 Cancelled다.
//
// 사용 예:
//   ShaderCompileManager manager(config);
//   if (auto started = manager.Start(); !started) { ... }
//   ShaderCompileJob job;
//   job.format = &snapshot;                  // LoadShaderFormatSnapshot
//   job.entryPath = "/Engine/Private/Sample.rsf";
//   job.entryPoint = "PSMain";
//   job.stage = ShaderStagePixel;
//   job.options = DefaultShaderCompileOptions(snapshot.definition);
//   Array<ShaderCompileJobResult> results(allocator);
//   manager.Compile(Span<const ShaderCompileJob>(&job, 1), results);
//   manager.Shutdown();
namespace rocks {

class ProgressCounter;
class WorkerScheduler;

} // namespace rocks

namespace rocks::shader {

// 워커 묶음 상한(측정 S8 — 워커 열기 비용을 묶음으로 나눠 갚는다)이다.
inline constexpr uint32 DefaultShaderBatchJobs = 32;
inline constexpr usize DefaultShaderBatchSourceBytes = usize{4} * 1024 * 1024;
// 머티리얼 생성 원문 연결 경로다(Docs/Design/MaterialSystem 7.2·7.8절, MSR3 — job의 materialSource
// 텍스트로 잇는다).
inline constexpr char MaterialGeneratedIncludePath[] = "/Engine/Generated/Material.rsh";
// 기본 워커 수 상한이다(min(논리 코어 - 1, 이 값)).
inline constexpr uint32 MaxDefaultShaderWorkers = 8;
// 작업 디렉터리 이름(<root>/Intermediate/ShaderWorkers)이다.
inline constexpr char ShaderWorkerDirectoryName[] = "Intermediate/ShaderWorkers";
// Compile 한 번이 받는 진행 sink 수 상한이다(대기 작업·진행 작업·호스트 준비 진행 + 여유 하나).
inline constexpr usize MaxShaderCompileProgressSinks = 4;

// Compile 한 번의 진행을 받는 곳이다(진행 보고 12장, ShaderSystem 11.1절 PG5 메모). added는 시작에
// 한 번(이 호출의 job 수), finished는 끝난 job 수만큼 아무 스레드(조정 스레드·워커 채널 스레드)에서
// 불린다. 받는 곳은 잠금 없는 일만 한다.
struct ShaderCompileProgressSink {
    void (*added)(void* context, uint64 jobs) noexcept = nullptr;
    void (*finished)(void* context, uint64 jobs) noexcept = nullptr;
    void* context = nullptr;
};

// Compile 한 번의 취소와 진행이다.
struct ShaderCompileControl {
    // 취소다(기본은 취소될 수 없는 token).
    CancelToken cancel;
    // 진행 sink다(최대 MaxShaderCompileProgressSinks개 — 넘는 sink는 받지 않는다). Compile이 돌아올
    // 때까지 살아 있어야 한다.
    Span<const ShaderCompileProgressSink> sinks{};
};

// counter의 AddTotal·Add로 잇는 sink다(작업 수준 계수·구간 계수 모두). counter는 sink를 쓰는
// Compile이 끝날 때까지 산다.
//
// 사용 예:
//   auto counter = ProgressCounter::ForTask(task, 0);
//   const ShaderCompileProgressSink sinks[] = {MakeShaderCompileCounterSink(counter.Value())};
//   ShaderCompileControl control;
//   control.cancel = task.Token();
//   control.sinks = sinks;
//   (void)manager.Compile(jobs, results, control);
ROCKS_SHADERCOMPILER_API ShaderCompileProgressSink
MakeShaderCompileCounterSink(ProgressCounter& counter) noexcept;

// 형식 정의 snapshot이다(세션을 시작할 때 한 번 읽은 byte와 그 정의 — 키와 워커 메시지가 같은
// byte를 쓴다, 10절).
struct ShaderFormatSnapshot {
    explicit ShaderFormatSnapshot(Allocator& allocator) noexcept : bytes(allocator) {
    }

    ShaderFormatDefinition definition;
    Array<uint8> bytes;
};

// directory/<id>.json을 읽어 snapshot을 만든다. 오류는 ShaderFormatStatus다.
ROCKS_SHADERCOMPILER_API ShaderFormatStatus
LoadShaderFormatSnapshot(const char* directory, StringView id, ShaderFormatSnapshot& out) noexcept;

struct ShaderCompileManagerConfig {
    // 결과·메시지·워커 채널 할당자다(여러 스레드의 동시 할당을 받아야 한다).
    Allocator* allocator = nullptr;
    const ShaderSourceTree* tree = nullptr;
    // 옵션 리플렉션 기록 문맥(RegisterShaderCompilerTypes를 등록한 TypeRegistry)이다.
    ShaderKeyReflection reflection{};
    // 너겟 캐시(열린 상태)다. null이면 캐시 없이 늘 컴파일한다.
    NuggetCache* cache = nullptr;
    // 너겟 일괄 요청의 scheduler다. null이면 동기 Get·Put이다.
    WorkerScheduler* scheduler = nullptr;
    // 워커 실행 파일과 작업 디렉터리 뿌리(<root>/Intermediate/ShaderWorkers)다.
    StringView workerExecutable{};
    StringView workRoot{};
    // 0이면 기본값이다.
    uint32 workerCount = 0;
    uint32 maxBatchJobs = DefaultShaderBatchJobs;
    usize maxBatchSourceBytes = DefaultShaderBatchSourceBytes;
    bool keepInput = false;
    // 시험 hook job(ShaderCompileJob::testHook)을 받는 워커다(SC7).
    bool testHooks = false;
    // 모든 채널에 거는 시험 hook이다(SC7 held — 제품 코드는 걸지 않는다).
    ShaderWorkerChannelHooks channelHooks{};
};

struct ShaderCompileJob {
    const ShaderFormatSnapshot* format = nullptr;
    // 엔트리 원문(.rsf 가상 경로)과 엔트리 함수 이름이다.
    StringView entryPath{};
    StringView entryPoint{};
    // ShaderStageBits 한 비트다.
    uint32 stage = 0;
    // job 정의(퍼뮤테이션)다. ROCKS_·__ 접두는 job 생성 오류다(SC10).
    Span<const ShaderDefine> defines{};
    ShaderCompileOptions options{};
    // 손으로 쓴 파라미터 레이아웃 표 목록이다(7.2절 5항, SSD31 — 정적 수명). 관리자가 정규 byte를
    // 워커에 보내고 그 SHA-256(ShaderParameterLayoutSignature)이 키 8행이다. 비면 8행은 none이다.
    Span<const ShaderParameterLayout> parameterLayouts{};
    // 셰이더 타입 요구 기능(ShaderFeatureBits — 워커의 기능 판정과 키 4행, 16.1절 SSD35)이다.
    uint32 requiredFeatures = 0;
    // 개발 전용 퍼뮤테이션인가(PS면 SC10이 ROCKS_DEBUG_BUFFER를 넣는다 — 16.1절 SSD34).
    bool developmentOnly = false;
    // 정점 입력 축 job의 정점 입력 타입 원문(.rsh 가상 경로)이다. 비지 않으면 전처리가
    // VertexInputGeneratedIncludePath include를 이 파일로 잇는다(SSD6, RenderBase 10.6절 2항 —
    // include 폐포에 들어가 키 5행이 정점 입력 타입마다 다르다).
    StringView vertexInputSource{};
    // 머티리얼 pass job의 생성 원문 텍스트다(Docs/Design/MaterialSystem 7.2·7.8절, MSR3). 비지
    // 않으면 전처리가 MaterialGeneratedIncludePath include를 이 텍스트로 잇는다(include 폐포에
    // 들어가 키가 머티리얼마다 다르다). 비면 잇지 않는다(머티리얼 없는 셰이더가 그 경로를
    // include하면 전처리 오류다). 컴파일이 끝날 때까지 살아 있어야 한다.
    StringView materialSource{};
    // 시험 hook(config.testHooks일 때만 — Compile 밖 값이면 그 job은 hook으로 워커에 간다).
    ShaderWorkerJobKind testHook = ShaderWorkerJobKind::Compile;
};

enum class ShaderCompileStatus : uint8 {
    Success,
    // 정의 조립·옵션·키 입력이 잘못되었다(diagnostics).
    InvalidJob,
    // 전처리 오류(diagnostics에 위치).
    PreprocessError,
    CompileError,
    // 도구를 열거나 부르지 못했다.
    ToolError,
    // 이 호스트·워커가 그 형식을 만들지 못한다.
    UnsupportedCapability,
    // 워커가 응답 없이 끝났다(종료 종류·코드는 diagnostics).
    WorkerFailed,
    // required 캐시의 실패다.
    CacheFailed,
    // 취소되었다(Compile의 취소 token 또는 닫힌 너겟 캐시).
    Cancelled
};

ROCKS_SHADERCOMPILER_API const char* ShaderCompileStatusName(ShaderCompileStatus status) noexcept;

struct ShaderCompileJobResult {
    explicit ShaderCompileJobResult(Allocator& allocator) noexcept
        : values(allocator), diagnostics(allocator), preprocessed(allocator) {
    }

    ShaderCompileStatus status = ShaderCompileStatus::InvalidJob;
    // 키(계산했으면)다.
    Sha256Digest key{};
    bool hasKey = false;
    // 너겟에서 온 결과다.
    bool cacheHit = false;
    ShaderJobValues values;
    String diagnostics;
    // 전처리 결과(키 5행 입력)다.
    PreprocessedShader preprocessed;
};

// 진행·통계 계수다(원자 — 다른 스레드가 읽어도 된다, 진행 보고 12절).
struct ShaderCompileProgress {
    uint64 total = 0;
    uint64 completed = 0;
    uint64 failed = 0;
    // Cancelled 상태로 끝난 job 수다(failed에 넣지 않는다).
    uint64 cancelled = 0;
    uint64 cacheHits = 0;
    // 워커 job 줄로 받은 아직 정산하지 않은 완료 수다.
    uint64 provisional = 0;
    uint64 workersStarted = 0;
    uint64 batches = 0;
    // 결정성 위반(Put Conflict)·손상 적중(값 0 없음·Corrupt) 수다.
    uint64 conflicts = 0;
    uint64 corruptHits = 0;
};

// 너겟 조회 결과에 대한 관리자 동작이다(10절 결과 처리 — 시험이 직접 대조한다).
enum class ShaderNuggetAction : uint8 {
    // 적중 값을 쓴다.
    UseHit,
    // 다시 컴파일해 게시한다.
    Recompile,
    // job 취소다.
    Cancel,
    // job 오류다(required 캐시 실패).
    Fail
};

// result의 동작이다: Hit이고 값 0이 있으면 UseHit(값 0이 없으면 손상 — Recompile), Cancelled는
// Cancel, failed이고 required면 Fail, 그 밖(Miss·Corrupt·DigestChanged·Dropped·Partial 등)은
// Recompile이다.
ROCKS_SHADERCOMPILER_API ShaderNuggetAction
ClassifyShaderNuggetGet(const NuggetCacheGetResult& result, bool required) noexcept;

class ROCKS_SHADERCOMPILER_API ShaderCompileManager final {
public:
    explicit ShaderCompileManager(const ShaderCompileManagerConfig& config) noexcept;
    ~ShaderCompileManager() noexcept;
    ShaderCompileManager(const ShaderCompileManager&) = delete;
    ShaderCompileManager& operator=(const ShaderCompileManager&) = delete;

    // 작업 디렉터리를 정리하고 워커 채널을 만든다(워커는 첫 묶음에서 띄운다). 오류: 설정 누락
    // InvalidArgument, 작업 디렉터리 실패는 그 파일 오류, 채널 시작 실패.
    Result<void> Start() noexcept;

    // jobs를 컴파일해 out(job 순서)에 둔다(out은 먼저 비운다). 실행 실패(할당)와 취소만 Result
    // 실패이고 job 실패는 결과 상태다. control.cancel로 취소된 job이 있으면 Cancelled를
    // 돌려준다(out은 그래도 job 순서로 채운다 — 성공한 job 결과는 쓸 수 있다). control의 sink는
    // 헤더 주석대로다.
    Result<void> Compile(Span<const ShaderCompileJob> jobs, Array<ShaderCompileJobResult>& out,
                         const ShaderCompileControl& control = {}) noexcept;

    // 한 건을 동기 Get·Put으로 컴파일한다(SC12 — Development 필요 시 컴파일).
    Result<void> CompileNow(const ShaderCompileJob& job, ShaderCompileJobResult& out) noexcept;

    // 지금 계수다.
    ShaderCompileProgress Progress() const noexcept;

    // 워커 채널을 닫는다(입력 pipe를 닫아 워커 종료를 기다린다). 여러 번 불러도 된다.
    void Shutdown() noexcept;

    struct State;

private:
    State* state_ = nullptr;
    ShaderCompileManagerConfig config_;
};

} // namespace rocks::shader
