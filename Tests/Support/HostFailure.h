#pragma once

#include <stdio.h>

namespace rocks::test {

// CRT stream lock 재진입 검사용 호스트 경계다. 엔진의 일반 로그/Thread API가 아니다.
// lock과 unlock은 같은 테스트 스레드에서 호출한다.
// 쓰기 전용 binary 보고서 파일을 연다. 실패는 nullptr이며 caller가 fclose한다.
FILE* OpenReportFile(const char* path) noexcept;

// 모든 테스트 프로세스 시작 시 호출한다. 실패·assert·abort가 사람 입력을 기다리는 OS·CRT 대화상자를
// 띄우지 않고 stderr 진단과 종료 코드로 끝나게 한다(자동 실행이 멈추지 않게).
void PrepareAutomatedTestProcess() noexcept;
void PrepareFatalTestProcess() noexcept;
void LockStandardErrorForTest() noexcept;
void UnlockStandardErrorForTest() noexcept;

} // namespace rocks::test
