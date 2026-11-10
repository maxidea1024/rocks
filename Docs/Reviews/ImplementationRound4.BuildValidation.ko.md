# Rocks Foundation 4차 빌드·실행 검증

검증일: 2026-11-10. Windows x64, clang-cl 21.1.6, C++23, 예외·RTTI 비활성 구성을 사용하였다. Name, Text/Catalog, 공용 동시성 API 및 경고 정책을 포함한 최종 소스에서 아래 검증을 완료하였다.

## 실행 결과

| 구성 | 개별 테스트 | CTest 묶음 | 결과 |
|---|---:|---:|---|
| `windows-clang-core` | 83 | 8 | 전부 PASS |
| `windows-clang-debug` | 110 | 8 | 전부 PASS |
| `windows-clang-release` | 110 | 8 | 전부 PASS |
| `windows-clang-asan` | 110 | 9 | 전부 PASS |
| Release, seed 7351, 100회 반복 | 110 × 100 = 11,000회 | 기본 보고서와 별도 실행 | 실패 0 |

`Build/format.py --check`도 통과하였다. 네 구성의 `build-evidence.json`에서 `status=PASS`, `sourceStableDuringRun=true`, `runnerReportCurrent=true`를 확인하고, 기록된 보고서 SHA-256을 실제 `foundation-report.json`과 대조하였다. 각 `Testing/Temporary/LastTest.log`의 통과 묶음 수는 위 표와 일치한다.

최종 공통 소스 집합은 116개 파일이다. 네 구성과 반복 실행 모두 다음 digest를 사용하였다.

```text
e4b9e381004fda7b8c5252c241783496fe5a14a000a8d6d631ab7e27f57d02ed
```

알고리즘은 `sha256(path-NUL-sha256(file))`이다. 실행 당시 Git HEAD는 `fce27b519e944db4229adb68f4fb46a7b5f031bc`, 작업 트리는 `dirty=true`였다. 따라서 위 결과는 해당 HEAD만의 결과가 아니라 digest로 식별한 미커밋 구현을 포함한 작업 트리의 결과이다. 문서 파일은 이 소스 digest 대상에서 제외한다.

## 독립 검증 범위

Text/Catalog 독립 테스트 22개는 공개 헤더 계약과 고정 기대 문자열을 기준으로 작성하였다. 구현 내부를 테스트 기대값 산출에 복제하지 않았다.

| 대상 | 확인 내용 |
|---|---|
| 문자열 소유권 | 원본 scratch 변경·파괴 이후 UTF-8 및 내부 NUL 보존, 결과 String의 입력 독립성 |
| 번역 선택 | 누락·source revision 불일치 진단 구분, 빈 번역 보존, 명시 snapshot 기준 해석 |
| Format | 한글·emoji, brace escape, 반복 이름, 삽입 인자 brace 재해석 금지, 중복·잘못된 인자 거부, unused 인자 허용 |
| 수명 | 인자 이름 복사, 원래 pattern/인자 해제 후 이력 보존, clone/move, store 소멸 이후 snapshot 유지 |
| 예산 | output·depth·work 제한, 깊이 64 성공/65 거부, 양수 work의 정확한 literal·catalog 경계 |
| 실패 처리 | 전체 생성·게시·해석 수명의 모든 관측 할당 지점에 N번째 실패 주입, Resolve/Format 실패 후 재시도 및 보존 상태 검사 |
| 회수 | 2,048단계 공유 이력의 무할당 해제 및 allocator 잔여 할당 0 |
| 동시성·재진입 | 세 reader와 publisher의 언어 교체·Close 경합, 이전 snapshot 일관성, 이전 snapshot 해제 콜백에서 Acquire/Publish 재진입 |

공용 동시성 API 직접 테스트 9개는 운영체제 헤더·직접 호출·조건부 컴파일 없이 작성하였다. Thread 생성·완료 대기 timeout·Join·재시작·self Join 거부, 조건 변수 predicate와 NotifyOne/All, 수동 reset event의 유지·Reset·다중 대기자, semaphore 전체량 초과 거부와 순서 보존 producer/consumer를 검증하였다. `thread_local` 소멸자를 명시적으로 대기시켜 entry 반환 확인과 native 종료·TLS 정리를 기다리는 Join의 의미도 구분하였다.

다른 검토자가 작성한 동시 Join 및 Join/완료 대기 경합 테스트 2개, Name/Text 통합 테스트 2개도 최종 구성 및 100회 반복에 포함되었다. 기존 Object, String, 메모리·컨테이너·Task·진단 테스트 역시 같은 전체 실행에 포함되었다.

## 실패 검출 장치

일반 구성의 8개 CTest 묶음은 foundation, harness 음성 대조군, STL 경계 검사, scaffold transaction, 실행 증거 계약, 플랫폼 경계 검사, 플랫폼 경계 음성 대조군, warning policy로 구성한다. ASan 구성에는 의도적인 use-after-free 검출 대조군을 추가하였다.

warning policy는 실제 컴파일 명령의 `/W4 /WX` 적용과 경고 억제 부재를 확인하고, 정상 probe 컴파일 성공 및 unused-variable 경고 probe의 컴파일 실패를 검증하였다. `LastTest.log` 안의 해당 probe 컴파일 오류는 의도한 실패 검출 근거이며 제품 소스의 빌드 오류가 아니다. Windows 보고서 파일 열기는 `fopen_s`로 격리하여 글로벌 `_CRT_SECURE_NO_WARNINGS` 정의 없이 빌드하였다.

첫 통합 중 Windows SDK의 `Yield` 매크로와 공용 함수명이 충돌한 컴파일 오류를 확인하였다. 담당 구현자가 API를 `YieldThread`로 변경하였고, 이후 최종 네 구성에서 재검증하였다. 초기 Format 중복 인자 오류 코드 불일치도 최초 합의인 `InvalidArgument`로 수정된 소스에서 검증하였다.

## 증거 위치와 한계

각 구성의 `out/<preset>/foundation-report.json`, `build-evidence.json`, `Testing/Temporary/LastTest.log`를 근거로 사용하였다. 반복 실행은 `out/windows-clang-release/repeat-report.json`, `repeat-evidence.json`, `repeat-output.log`에 별도 저장하였다. 반복 증거에는 실행 명령, 11,000회·실패 0, 실행 전후 소스 digest, 실행 바이너리와 보고서 해시를 기록하였다. 기본 foundation 보고서는 덮어쓰지 않았다.

이번 검증은 Windows의 CPU Foundation 구현 범위이다. 이번 변경의 macOS native 컴파일·실행은 이 보고서 작성 시점에 미실행이며 후속 CI 결과로 별도 판정해야 한다. ASan 통과와 자체 allocator 잔여 할당 0을 프로세스 전체의 모든 native/CRT 자원 또는 모든 경합 상태에 대한 완전 증명으로 확대하지 않는다. 렌더러·Studio 등 후속 엔진 기능의 완료를 의미하지 않는다.


## 후속 네이티브 CI 독립 대조

로컬 검증 이후 커밋 `b38c9437b32dcd41837b5df7b8ab2a1dbc622272`의 CI 실행 `42771207691` 원본 artifact를 독립적으로 확인하였다. 앞 절의 macOS 미실행 상태는 아래 실행 결과로 갱신한다.

| 네이티브 구성 | 컴파일러 | 테스트 / CTest | 결과 |
|---|---|---:|---|
| Windows x64 Debug | Clang 20.1.8 | 110 / 8 | 전부 PASS |
| macOS ARM64 Debug | Apple Clang 17.0.0 | 110 / 8 | 전부 PASS |
| macOS ARM64 ASan | Apple Clang 17.0.0 | 110 / 9 | 전부 PASS |

세 artifact의 `build-evidence.json`, `foundation-report.json`, `Testing/Temporary/LastTest.log`를 각각 읽고 대조하였다. 실행 전후 HEAD는 모두 위 커밋과 일치하고, 작업 트리는 모두 `dirty=false`이다. 소스 116개 파일의 digest는 로컬 최종 검증과 같은 `e4b9e381004fda7b8c5252c241783496fe5a14a000a8d6d631ab7e27f57d02ed`이며, 모든 구성에서 `sourceStableDuringRun=true`를 확인하였다. 보고서 파일 해시와 `runnerReportCurrent`도 일치한다. 세 구성의 테스트 이름 집합은 로컬 전체 110개 집합과 동일하다.

macOS ASan 로그에서 TLS 정리와 entry 완료의 구분, 동시 Join·완료 대기, 번역 snapshot 재진입·언어 전환, 양수 작업량 경계 사례의 개별 PASS를 확인하였다. ASan 음성 대조군은 의도적인 child-process use-after-free 검출을 확인하였고, 경고 probe는 실제 `-Wall -Wextra -Wpedantic -Werror` 명령으로 unused-variable 오류를 발생시켰다. 세 구성 모두 플랫폼 경계 검사·음성 대조군 및 warning policy 통과 로그가 존재한다.

원본 증거 위치는 `out/ci-42771207691/foundation-Windows/windows-clang-debug/`, `out/ci-42771207691/foundation-macOS/macos-clang-debug/`, `out/ci-42771207691/foundation-macOS/macos-clang-asan/`이다. 이 후속 검토에서 실행 소스는 변경하지 않았으며, macOS Release 및 macOS 100회 반복 결과로 범위를 확대하지 않는다.
