#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Platform/Process.h>
#include <ShaderCompilerCommon/ShaderWorkerProtocol.h>

// 워커 채널이다(Docs/Design/ShaderSystem 11.1절 SC4·SC7). 워커 하나를 전용 채널 스레드가
// 소유한다: P1 Process는 한 객체를 한 스레드만 다루고 다른 스레드에서 막힌 Read를 깨우는 취소가
// 없으므로, 그 Process의 시작·쓰기·읽기·종료를 모두 채널 스레드가 한다. 다른 스레드는 Run으로
// 묶음을 맡기고 결과를 기다린다(요청 큐).
//
// 묶음 하나: 요청 byte를 `input.<seq>.tmp`에 다 쓰고 no-replace 이동으로 `input.<seq>`에 둔 뒤 입력
// pipe에 `<seq>\n`을 쓴다. 출력 pipe에서 `done <seq>`를 받은 뒤에만 `output.<seq>`를 읽고 지운다
// (파일 존재 polling이 없다). `held <seq>`(시험 hook)는 hook에 묻고(풀기·끝내기), `child <pid>`는
// 결과에 모은다. 워커가 끝나면(EOF) 종료 종류·코드와 진단으로 그 묶음을 실패로 보고하고 다음
// 묶음에서 워커를 다시 띄운다. 진행 중 묶음은 취소하지 않는다(워커 강제 종료는 채널 스레드가
// hook으로만 한다).
//
// 워커는 `--work-dir <dir> --parent-pid <이 프로세스> --worker <n>`으로 낮은 우선순위로 띄운다.
// 표준 오류는 Null이다(읽지 않는 pipe가 차서 멈추는 교착을 막는다 — 워커 로그는 worker.log).
//
// 사용 예:
//   ShaderWorkerChannel channel(allocator);
//   ShaderWorkerLaunch launch;
//   launch.executable = workerPath;            // 호출하는 실행 파일 옆의 ShaderCompileWorker
//   launch.workDirectory = "<root>/Intermediate/ShaderWorkers/<pid>/0";
//   if (auto started = channel.Start(launch); !started) { ... }
//   ShaderWorkerBatchResult result(allocator);
//   channel.Run(requestBytes, result);         // result.outcome·response·exit·diagnostics
//   channel.Shutdown();                         // 입력 pipe를 닫고 워커 종료를 기다린다
namespace rocks::shader {

struct ShaderWorkerLaunch {
    // 워커 실행 파일(UTF-8 절대 경로)이다.
    StringView executable;
    // 작업 디렉터리(없으면 만든다)다.
    StringView workDirectory;
    uint32 worker = 0;
    // 워커가 입력 파일을 지우지 않는다(디버그 — UE -KeepInput).
    bool keepInput = false;
    // 시험 hook job을 받는 워커다(SC7).
    bool testHooks = false;
};

enum class ShaderWorkerBatchOutcome : uint8 {
    // 워커가 응답을 냈다(job별 상태는 response).
    Completed,
    // 워커가 응답 없이 끝났다(버전 불일치 코드·충돌·강제 종료 — exit·diagnostics).
    WorkerExited,
    // 채널 쪽 실패(작업 디렉터리·파일·pipe·손상된 출력 — diagnostics).
    ChannelFailed,
    // 채널이 닫혔다.
    ShutDown
};

ROCKS_SHADERCOMPILER_API const char*
ShaderWorkerBatchOutcomeName(ShaderWorkerBatchOutcome outcome) noexcept;

struct ShaderWorkerBatchResult {
    explicit ShaderWorkerBatchResult(Allocator& allocator) noexcept
        : response(allocator), diagnostics(allocator), children(allocator) {
    }

    ShaderWorkerBatchOutcome outcome = ShaderWorkerBatchOutcome::ChannelFailed;
    ShaderWorkerResponse response;
    // WorkerExited의 종료다.
    platform::ProcessExit exit;
    // 영어 진단이다(WorkerExited는 ShaderWorkerExitDiagnostic).
    String diagnostics;
    // 이 묶음을 처리한 워커의 PID다.
    uint32 workerProcessId = 0;
    // 워커가 알린 자식 PID(`child <pid>`, 시험 hook)다.
    Array<uint32> children;
};

// 워커가 `held <seq>`를 썼을 때 고르는 동작이다(SC7 — 명시 gate).
enum class ShaderWorkerHeldAction : uint8 {
    // 입력 pipe에 한 줄을 써서 풀어 준다.
    Release,
    // 채널 스레드가 워커를 강제 종료한다.
    Terminate
};

// 채널 스레드가 부르는 시험 hook이다(제품 코드는 걸지 않는다).
struct ShaderWorkerChannelHooks {
    ShaderWorkerHeldAction (*held)(void* context, uint32 sequence, uint32 workerProcessId,
                                   Span<const uint32> children) noexcept = nullptr;
    void* context = nullptr;
};

class ROCKS_SHADERCOMPILER_API ShaderWorkerChannel final {
public:
    // allocator는 채널보다 오래 살고 여러 스레드의 동시 할당을 받아야 한다.
    explicit ShaderWorkerChannel(Allocator& allocator) noexcept;
    // Shutdown을 부른다.
    ~ShaderWorkerChannel() noexcept;
    ShaderWorkerChannel(const ShaderWorkerChannel&) = delete;
    ShaderWorkerChannel& operator=(const ShaderWorkerChannel&) = delete;

    // 채널 스레드를 시작한다(워커는 첫 묶음에서 띄운다). 문자열은 복사한다. 오류: 이미 시작함
    // AlreadyExists, 빈 경로 InvalidArgument, 스레드 시작 실패 DependencyFailed, 할당 실패.
    Result<void> Start(const ShaderWorkerLaunch& launch,
                       const ShaderWorkerChannelHooks& hooks = {}) noexcept;

    // 요청 파일 byte(EncodeShaderWorkerRequest 결과)를 워커에 맡기고 결과를 기다린다. 여러 스레드가
    // 불러도 들어온 순서로 하나씩 처리한다. 시작 전이거나 닫혔으면 out.outcome이 ShutDown이다.
    void Run(Span<const uint8> request, ShaderWorkerBatchResult& out) noexcept;

    // 새 묶음을 막고 남은 묶음을 끝낸 뒤 입력 pipe를 닫아 워커 종료를 기다리고 스레드를 회수한다.
    // 여러 번 불러도 된다.
    void Shutdown() noexcept;

    struct State;

private:
    Allocator& allocator_;
    State* state_ = nullptr;
};

} // namespace rocks::shader
