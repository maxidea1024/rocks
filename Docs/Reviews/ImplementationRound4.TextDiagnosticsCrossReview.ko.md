# 구현 교차 검토 4차 — Text 수명·예산과 경고 정책

검토일: 2026-11-10. 검토 범위는 Localization의 Text/Catalog/Resolver와 엔진 소유 소스의 컴파일 경고 정책이다. 구현 담당자와 별도로 코드를 읽고 구체적인 실패 순서를 대입하였다. 검토 과정에서 production·테스트 소스를 직접 수정하지 않았으며, 이 기록 작성 시에도 해당 소스를 변경하지 않았다.

## 1. 판정과 증거 범위

**현재 검토 범위의 미해결 확정 구현 결함은 0건이다.** Text의 소유권·참조 계수·정리 순서에서는 새로운 확정 결함이 발견되지 않았다. 초기 실행에서 발견된 중복 인자 오류 코드 불일치는 담당자가 수정하였다. 경고 정책 감사의 검출 공백 3건은 수정본에 동일 입력을 재대입하여 해결을 확인하였다.

| 구분 | 확인 범위와 판정 |
|---|---|
| Text/Catalog/Resolver | 최신 소스와 테스트의 정적 검토. 이 검토자가 전체 테스트 실행을 재수행한 것은 아님 |
| 중복 Text 인자 오류 코드 | 최초 통합 실행에서 발견되었다는 담당자 보고와 수정 소스·기존 assertion의 일치 확인 |
| 깊이·양수 work 예산·store callback 회귀 | 테스트 소스의 반례와 기대 결과 확인. 최종 실행 결과는 별도 build evidence 기준 |
| 경고 강등 반례 | 저장소 외부 scratch에서 Windows clang-cl을 직접 실행하여 종료 코드와 진단 확인 |
| 경고 감사 수정 확인 | 동일 command/source 입력으로 수정 전후 감사 결과를 직접 비교 |
| Windows 최종 build matrix | 이 보고서에 실행 수·통과 수를 재기재하지 않음. 담당자의 최종 source digest 및 실행 보고와 구분 |
| macOS native·macOS ASan | 이 검토에서 NOT RUN |
| TSan | 이 검토에서 NOT RUN. 잠금 검토와 일반 native thread 테스트로 대체하여 통과를 선언하지 않음 |

검토 파일은 `Engine/Localization/Public/Localization/{Text.h,Catalog.h}`, `Engine/Localization/Private/{Text.cpp,Catalog.cpp,TextResolve.cpp,LocalizationInternal.h}`, 관련 `Tests/Localization` 소스, `CMakeLists.txt`, `Build/audit_warning_policy.py`, `Tests/Integration/test_warning_policy.py`다. 구현 계약 문서 `Docs/Implementation/NameTextImplementation.ko.md`도 함께 대조하였다.

## 2. Text·Catalog 소유권과 종료 경계

### 2.1 공유 DAG의 반환

`Text.cpp`의 `DropToQueue`와 `ReleaseText`는 노드별 gate에서 참조 계수를 감소시키고, 0으로 전환한 노드만 intrusive 정리 목록에 추가한다. parent가 보존한 pattern과 각 argument reference를 각각 한 번 반환한다. 같은 child를 pattern과 여러 argument에서 공유하는 경우에도 보존 횟수와 반환 횟수가 대응한다.

노드의 자식을 정리 목록에 추가한 뒤 현재 노드의 String/Array와 노드 저장소를 반환한다. 어느 node gate도 allocator의 Deallocate 호출까지 보유하지 않는다. 자식은 각 노드에 기록된 원래 allocator로 반환되므로 parent와 child의 allocator가 달라도 소유권 경로는 유지된다. 마지막 반환은 그래프 깊이에 따른 재귀 호출이나 신규 allocation을 요구하지 않는다.

`Text::Format`의 중간 실패도 확인하였다. pattern 보존 후 argument 배열·이름 복사·child 보존 중 일부가 실패하면, 지역 Text 소유자의 반환 경로에서 이미 보존한 부분만 정리한다. 아직 보존하지 않은 child 포인터는 null 상태이므로 추가 반환하지 않는다. 이 경로를 입력 Text가 변경되지 않는다는 계약과 대조하였다.

`TextCreationLimitsAndSharedDeepHistoryReleaseNeedNoAllocation`은 깊이 2,048의 공유 이력을 구성하고 해제 단계의 다음 할당을 실패하도록 설정한 뒤 allocation 시도가 없음을 검사한다. 이 테스트 작성 사실을 모든 가능한 DAG·allocator callback에 대한 보편적 무누수 증명으로 해석하지 않는다.

### 2.2 Catalog 게시·교체·Close

`CatalogStore::Publish`는 새 snapshot 보존을 확보한 뒤 gate에서 이전 current를 지역 previous로 분리하고 새 current를 설치한다. 이전 snapshot의 마지막 반환은 gate 밖에서 일어난다. callback이 다른 snapshot을 게시하더라도 바깥 Publish가 복귀하면서 current를 다시 덮어쓰는 단계가 없다.

`Acquire`는 store gate에서 현재 snapshot의 계수를 보존한다. 이때 사용하는 catalog 계수 gate 안에서는 allocator 호출이 없다. `ReleaseCatalog`도 계수 gate를 해제한 후 실제 파괴를 수행하므로 allocator callback에서 store를 다시 조회하는 순서와 잠금 순환이 발생하지 않는다.

`Close`는 gate에서 closed 상태를 먼저 게시하고 current를 분리한다. 이미 획득한 snapshot은 독립 reference로 유지된다. callback에서 Close/Acquire/Publish를 다시 호출하는 순서를 정적으로 대입하였으며, Close 이후의 새 admission 차단과 기존 reader 보존 계약이 일치하였다. store 자체 파괴는 진행 중 API 호출이 종료된 뒤라는 호출자 전제를 유지한다.

추가된 `CatalogOldSnapshotCleanupCanReenterAcquireAndPublishOutsideStoreGate`는 이전 snapshot 정리 callback에서 revision 2를 Acquire한 뒤 revision 3을 Publish하고, 최종 current가 3인지 검사한다. 이 실행 fixture의 callback 동작은 Acquire/Publish이며, callback 내부 Close는 이번 검토의 정적 대입 범위다.

### 2.3 Resolve 실패 원자성과 제한

`TextResolve.cpp`의 `Spend`는 남은 work에서 차감하여 누적 덧셈 overflow를 피한다. `Visit`의 root 깊이는 1이고 pattern/argument 방문 시 증가한다. 설정 깊이 64 초과는 InvalidArgument이며, 실제 방문이 설정 한도를 초과하면 CapacityExceeded다.

출력 상한은 최종 결과뿐 아니라 각 중간 String의 payload에 적용된다. 임시 pattern이 최종 결과보다 길다는 이유로 한도를 우회하지 않는다. 잘못된 문법·없는 인자·예산 초과·OOM에서 지역 결과만 파괴하며 부분 결과를 외부에 게시하지 않는다. 전달된 Text와 snapshot의 이력은 수정하지 않는다.

동일 래퍼의 Reset/move와 읽기 경합은 호출자가 금지해야 한다. refcount의 잠금은 래퍼 자체를 자유롭게 동시 변경할 수 있다는 보장이 아니다. Resolve는 호출 동안 입력 이력과 allocator의 수명이 유지된다는 전제를 갖는다.

## 3. Text 계약 수정과 검증 보강

| 항목 | 성격·반례 | 조치와 확인 |
|---|---|---|
| TD01 중복 인자 오류 코드 | 실제 초기 실행에서 발견된 계약/테스트 불일치. 중복 이름에 최초 합의와 assertion은 InvalidArgument를 요구하나 초안은 AlreadyExists 반환 | 담당자가 `Text::Format`을 InvalidArgument로 수정. 현재 `Text.cpp`와 `TextFormatRejectsInvalidBindingsWithoutChangingInputs`의 기대값 일치 확인 |
| TD02 양수 work 예산 | P2 검증 공백. maxWork=0만 검사하면 잔량 차감을 누락하고 0만 특별 거부하는 회귀를 검출하지 못함 | `TextBudgetTests.cpp` 추가. Invariant("x")는 예산 2 실패/3 성공, matching catalog 후보 1개를 포함하면 3 실패/4 성공. Format은 부족한 양수 예산과 충분한 예산의 고정 출력 대조 |
| TD03 callback 재진입 | 확인된 구현 결함이 아닌 중요한 경계의 실행 증거 보강. 이전 snapshot 정리에서 재게시한 값을 바깥 호출이 덮어쓰거나 store gate를 다시 획득하여 교착할 수 있는 순서 | `CatalogReentryTests.cpp`에 Acquire/Publish callback 회귀 추가. callback 1회, 관찰 revision 2, 최종 revision 3, 마지막 allocator 잔여 0을 검사 |

깊이 64의 정확한 성공 경계와 한 단계 초과 실패는 기존 `TextResolveMaximumDepthBoundaryHasExactGolden`에서 확인하였다. TD02의 Format 테스트는 전체 검색 알고리즘의 정확한 work 수를 고정하는 테스트가 아니다. 구현 변경 여지를 유지하면서 양수 예산과 반복 인자 해석 경로를 검사한다.

TD02·TD03의 추가 테스트를 읽고 반례가 표현되었음을 확인하였다. 이 문서의 검토자는 해당 추가 테스트를 직접 다시 실행하지 않았으므로, 코드 확인과 최종 실행 통과를 구분한다.

## 4. 경고 정책 감사의 검출 공백

검토 시 CMake의 실제 공통 옵션은 Windows `/W4 /WX`, 다른 지원 compiler의 `-Wall -Wextra -Wpedantic -Werror`였다. 글로벌 CRT 경고 억제 매크로는 제거된 상태였다. 문제는 이 설정 자체보다 감사와 음성 대조가 미래 변경을 충분히 검출하지 못하는 데 있었다.

| 항목 | 수정 전 반례 | 위험·수정 확인 |
|---|---|---|
| WP01 필수 옵션 누락 | `clang++ -Wall -Werror -c probe.cpp`와 `-Wall -Wextra -Werror`를 command 감사가 승인 | P2 검증 공백. `-Wextra`/`-Wpedantic` 누락이 통과함. 수정 후 profile별 필수 옵션 각각을 검사하고 같은 입력을 거부함 |
| WP02 source의 오류 강등 | `#pragma clang diagnostic warning "-Wunused-variable"`을 source 감사가 승인 | P2 정책 검출 공백. `/WX`가 있어도 특정 진단을 warning으로 강등할 수 있음. 수정 후 clang/GCC diagnostic의 ignored뿐 아니라 warning도 거부함 |
| WP03 clang-cl 전달 옵션 | `clang-cl /W4 /WX /clang:-Wno-error=unused-variable /c probe.cpp`를 command 감사가 승인 | P2 검증 공백. compiler 전달 접두사 뒤의 억제를 검사하지 않음. 수정 후 `/clang:` 경로도 검사하고 동일 입력을 거부함 |

`Tests/Integration/test_warning_policy.py`에는 위 누락·우회 입력의 음성 대조가 추가되었다. WP01·WP03은 감사 함수 입력으로 직접 확인하였다. WP03 전달 옵션 자체의 별도 native 컴파일 실행까지 수행한 것은 아니다.

### 4.1 실제 compiler 반례

저장소를 변경하지 않는 별도 scratch에서 다음 입력을 Windows clang-cl로 `/W4 /WX /c` 옵션과 함께 컴파일하였다.

```cpp
#pragma clang diagnostic warning "-Wunused-variable"
int probe() {
    int unusedWarningProbe = 7;
    return 0;
}
```

관찰 결과는 종료 코드 0과 `warning: unused variable 'unusedWarningProbe' [-Wunused-variable]` 진단 1개였다. 수정 전 source 감사도 이 파일을 승인하였다. 동일 unused 변수를 pragma 없이 컴파일한 대조 입력은 `[-Werror,-Wunused-variable]` 오류와 종료 코드 1을 반환하였다. 따라서 compiler가 사용되지 않았거나 모든 컴파일이 실패하는 환경 문제와 구분하였다.

수정 후 같은 scratch를 다시 감사한 결과 WP02 파일이 `owned source suppresses warnings`로 거부되었다. compiler의 pragma 동작 자체를 바꾼 것이 아니라, 정책을 약화하는 source를 별도 감사로 검출하도록 보완한 것이다. WP01의 누락 옵션과 WP03 전달 옵션도 같은 입력으로 재검사하여 거부를 확인하였다.

## 5. 자동 감사의 한계와 후속 실행 구분

현재 source 감사는 `.h`, `.cpp`, `.mm`에 대한 행 단위 정규식 검사다. 정식 C/C++ 전처리기가 아니므로 `_Pragma`/`__pragma`, 매크로 확장, 여러 줄 조합, 미대상 확장자 등의 모든 우회 경로를 자동으로 증명하지 않는다. command 검사도 정식 compiler 옵션 parser나 모든 response file의 재귀 해석기가 아니다. 실제 규칙상 금지되는 경고 억제 범위와 현재 자동 탐지 범위를 구분해야 한다.

block comment 안의 `#pragma clang diagnostic ignored ...` 문장도 source 감사가 거부하는 오탐을 확인하였다. 해당 scratch의 실제 컴파일은 clean 종료 코드 0이었다. 이 보수적인 lexical 검사 특성을 문서화하고, 정상 코드를 허용하기 위한 변경이 필요할 때 별도 회귀로 확인해야 한다.

경고 정책은 선택한 baseline에서 발생하는 compiler warning을 오류로 처리한다는 의미다. compiler가 제공하는 모든 비활성 진단을 활성화했거나 외부 SDK의 모든 내부 경고까지 검사하였다는 의미로 확대하지 않는다. 의도적인 unused-warning target은 정상 전체 빌드와 분리하고, 깨끗한 대조 target의 성공 및 의도 경고 target의 실패를 함께 검사한다.

이 검토의 실제 실행 증거는 Windows의 격리 compiler 반례와 감사 함수 수정 전후 결과다. macOS와 TSan은 미실행이며, 최종 Debug/Release/ASan/Core-only matrix 및 그 source digest는 별도 최종 빌드 기록으로 확인한다. 테스트 추가·정적 검토·특정 반례 통과를 전체 엔진의 무누수 또는 무경합 증명으로 합산하지 않는다.
