#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <Launch/Application.h>

namespace sandbox {

// 헤드리스 엔진 호스트 설정이다(Sandbox --engine).
struct EngineHostOptions {
    // 실행할 프레임 수다(EngineConfig::maxFrames).
    rocks::uint64 frames = 60;
    // Engine branch 설정 파일(JSON)이다. 엔진 층 파일 다음 Project 층으로 읽는다(log.*는 "Settings"
    // 섹션). null이면 읽지 않는다.
    const char* configPath = nullptr;
    // trace 파일 경로다(`--trace-file <path>`, trace.file 명령과 같다). null이면 열지 않는다.
    const char* traceFile = nullptr;
    // 명령줄 `--set=<target>=<value>`·`--trace=<목록>` 인자다(나온 순서). 실행 동안 빌린다.
    rocks::Span<const rocks::StringView> arguments{};
    // 시험용: 0이 아니면 이 번째 PumpEvents에서 launch::RequestTermination()을 부른다(Ctrl-C
    // 처리기가 남기는 것과 같은 표시). 종료 요청 다리가 그 표시를 보고 정상 종료로 잇는지 확인한다.
    rocks::uint64 requestTerminationAtFrame = 0;
};

// 헤드리스 엔진 실행이다(Docs/Design/LoggingSystem.ko.md 5.1절 — tick이 있는 엔진 호스트는 Async,
// Docs/Design/ConfigSystem.ko.md 5.3절 — 부트 순서). 설정 문맥은 이 호스트가 하나만 만들고 엔진이
// 빌려 쓴다. 순서:
//   1) 모듈 타입·설정 등록(log.*, trace.*·TracePresets) → 2) 엔진 층 파일(LoadConfigFiles —
//   실행 파일 위쪽에서 찾은 Engine 폴더) → 3) --config 파일(Project 층) → 4) --set(Session 층) →
//   Sync → 5) CompleteStartup → 6) LogHost 설치(기본 Async·stderr) → 7) trace 세션·콘솔 명령·
//   --trace → 8) 엔진 루프(프레임마다 sync 지점) → 엔진 정리 → trace 정지 → 설치 해제 →
//   LogHost::Stop → 설정 문맥 파괴. LogHost가 멈춘 출력 스레드를 포기했으면(WriterAbandoned)
//   LogHost와 설정 문맥을 파괴하지 않고 context.RequestExitWithoutStaticTeardown()으로 Launch가
//   정리(크래시 수집 해제) 뒤 _Exit로 끝내게 한다. allocator는 context.DefaultAllocator()다.
// 설치 전 설정 오류는 비상 경로(stderr)로만 나간다. 성공 0, 실패 1, 설정 오류 2다.
int RunEngineHost(rocks::launch::ApplicationContext& context,
                  const EngineHostOptions& options) noexcept;

// --self-test의 한 단계다. 이 실행 파일을 `--engine --frames 3`으로 다시 띄워 종료 코드 0과
// stderr의 엔진 시작·Run 종료·종료 줄을 확인하고, 설정 파일(Sync·파일 기록)로 다시 띄워 로그 파일에
// 종료 줄이 남는지 확인한다(창·GPU가 필요 없다). 실패 수를 돌려준다.
rocks::uint32 RunEngineHostSelfTest() noexcept;

} // namespace sandbox
