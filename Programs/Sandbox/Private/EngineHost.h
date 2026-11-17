#pragma once

#include <Core/Memory/Allocator.h>

namespace sandbox {

// 헤드리스 엔진 호스트 설정이다(Sandbox --engine).
struct EngineHostOptions {
    // 실행할 프레임 수다(EngineConfig::maxFrames).
    rocks::uint64 frames = 60;
};

// 헤드리스 엔진 실행이다(Docs/Design/LoggingSystem.ko.md 5.1절 — tick이 있는 엔진 호스트는 Async).
// Async LogSystem과 stderr sink를 설치하고, 게임 스레드 이름을 붙이고, World 하나로 frames만큼
// 돌린 뒤 정리한다. 엔진이 시작·Run 종료·종료를 로그로 남긴다. 성공 0, 실패 1이다.
int RunEngineHost(rocks::Allocator& allocator, const EngineHostOptions& options) noexcept;

// --self-test의 한 단계다. 이 실행 파일을 `--engine --frames 3`으로 다시 띄워 종료 코드 0과
// stderr의 엔진 시작·Run 종료·종료 줄을 확인한다(창·GPU가 필요 없다). 실패 수를 돌려준다.
rocks::uint32 RunEngineHostSelfTest() noexcept;

} // namespace sandbox
