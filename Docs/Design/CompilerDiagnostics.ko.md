# Rocks 컴파일러 경고 정책

기준일: 2026-11-10. 이 문서는 전체 엔진 구현 계획과 Core 상세 구현 계획의 필수 빌드 규약이다.

## 필수 규칙

**컴파일러 경고는 허용하지 않는다. 활성화된 경고가 한 건이라도 발생하면 오류로 처리하여 빌드를 실패시킨다.** 개발용 로컬 빌드와 CI에 같은 기준을 적용한다. Debug·Release·ASan·Core-only·헤드리스와 향후 Editor·Shipping 구성에서 이 규칙을 끌 수 있는 선택 옵션을 제공하지 않는다.

엔진 소유 Runtime, 공개 헤더 단독 컴파일, 플랫폼 backend, 테스트, C++ 호스트 도구 및 생성 코드를 적용 대상으로 한다. 테스트라서 경고를 허용하거나 생성 파일이라는 이유로 경고 검사를 생략하지 않는다. 생성 코드의 결함은 생성기와 fixture를 함께 수정한다.

| 컴파일러 계열 | 필수 기본 설정 | 현재 적용 |
|---|---|---|
| MSVC 호환 드라이버 / clang-cl | `/W4 /WX` | Windows 전체 Foundation target |
| Clang / Apple Clang, 향후 지원할 GCC 계열 | `-Wall -Wextra -Wpedantic -Werror` | macOS 전체 Foundation target. GCC 실행은 미검증 |

여기서 경고 0건은 **고정한 컴파일러와 명시한 진단 집합에서 경고 없이 빌드됨**을 뜻한다. 모든 컴파일러의 모든 선택적 진단을 무조건 켜거나 경고가 없다는 이유로 UB·race·메모리 누수 부재를 증명한다는 뜻이 아니다. 버전 변경은 진단 변화와 실제 Windows/macOS 빌드를 함께 검토한다.

## 금지하는 우회

- `/WX-`, 경고 수준 축소, `/wd...`, `-w`, `-Wno-...`, `-Wno-error...` 등으로 엔진 소유 코드의 문제를 숨기지 않는다.
- `#pragma warning(disable: ...)`, `diagnostic ignored`, `diagnostic warning` 및 동등한 매크로로 경고를 끄거나 오류를 경고로 강등하지 않는다.
- `_CRT_SECURE_NO_WARNINGS` 같은 전역 경고 억제 정의를 사용하지 않는다. 플랫폼 API 차이는 private adapter와 올바른 호출로 해결한다.
- 외부 SDK 헤더를 엔진 공개 헤더나 공용 구현에 무분별하게 포함하지 않는다. 현재 시스템 SDK 자체의 진단 기본 처리는 공급자 도구 체계를 따르되, 엔진 소유 코드를 `SYSTEM` 헤더로 표시하여 숨기지 않는다.

외부 라이브러리를 추가할 때는 고정 버전·별도 target·adapter 경계를 기록한다. 외부 원본에서 빌드 경고가 발생하면 호환 버전 선택 또는 추적 가능한 수정으로 해결하고 엔진 전체에 억제 옵션을 전파하지 않는다. 현재 승인된 경고 억제 예외는 없다.

## 실제 강제와 수용 기준

1. `Rocks.Build`(`Programs/Rocks.Build`)가 모든 번역 단위에 공통 진단 설정을 붙인다. 모듈 규칙은 이 프로필을 바꾸지 않는다.
2. `PolicyChecks.CompilationDatabase`(`policy --compile-commands`)는 실제 `compile_commands.json`의 모든 번역 단위에서 필수 플래그와 금지된 억제·강등 옵션을 확인한다. `PolicyChecks.SourceWarnings`는 엔진·테스트·도구 소스의 직접적인 진단 억제 pragma/define도 검사한다.
3. 도구 harness(`Rocks.ToolTests native`)는 정상 fixture의 실제 컴파일 성공과, 같은 프로필의 미사용 지역 변수 fixture가 **warning-as-error 진단으로 실패함**을 함께 검증한다. 단순한 문법 오류·파일 누락으로 실패한 것은 통과로 인정하지 않는다. fixture는 기본 제품 빌드에서 제외하고 매 실행 재컴파일한다.
4. 필수 플래그 누락·경고 강등·소스 억제를 주입하는 음성 대조군으로 감사 자체도 검증한다. `dotnet msbuild Rocks.proj /t:Test`의 정상 검증 경로와 Windows/macOS CI가 이 검사를 실행한다. 빌드만 한 결과는 최종 수용 증거로 인정하지 않는다.
5. 빌드 성공, 경고 정책 검사 성공, 서식 검사 성공, 관련 단위·장애·수명 시험 성공을 모두 요구한다. warning-as-error 실패를 해결하려고 테스트를 삭제하거나 경고 수준을 낮추지 않는다.

소스 검사는 `.h`/`.cpp`/`.mm` 대상 lexical guard이며 전처리 확장 전체의 형식 검증은 아니다. 블록 주석에 쓴 pragma 예제도 탐지할 수 있으므로 긴 예시는 설계 문서에 둔다. `_Pragma` 같은 우회와 새 build script·새 compiler driver를 통한 설정 변경도 코드 리뷰에서 금지한다. 신규 도구 체계 지원 시 실제 compiler invocation에 대한 감사와 음성 fixture를 확장해야 한다. 컴파일 경고 규약은 로깅의 Warning 등급이나 에셋 검증 경고를 자동으로 금지하는 규칙과는 구분한다.

## 현재 검증 증거

[CI 실행 42771207691](https://github.com/maxidea1024/rocks/actions/runs/42771207691)의 Windows x64 Clang 20.1.8, macOS ARM64 Apple Clang 17.0.0 Debug/ASan에서 필수 프로필·정상 컴파일·의도적 경고의 오류 거부를 모두 확인했다. 로컬 Clang 21.1.6 Core-only/Debug/Release/ASan에서도 통과했다. 상세 수치와 source digest는 [Foundation 검증 기록](../Implementation/FoundationStatus.ko.md)을 따른다. 경고 정책을 문서 선언만으로 완료 처리하지 않는다.
