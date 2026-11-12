# Implementation Round 3 — String 독립 검토

검토일: 2026-11-10. 대상은 `Core/Text/String.h`, `StringView.h`, `Private/Text/String.cpp`, `Tests/Core/StringTests.cpp`의 13개 테스트와 `Docs/Implementation/StringAndText.ko.md`, `FoundationStatus.ko.md`다. 구현 담당과 별도로 전체 소스를 읽고 실패 순서·메모리 수명·문서 계약을 대조했다. 이 보고서의 소스 검토 판정과 실행 결과는 구분한다.

## 판정

현재 명시한 UTF-8 String/StringView 범위에서 수정이 필요한 확정 구현 결함은 발견하지 못했다. 문자열 코드는 읽기 전용으로 검토했으며 원본이나 테스트를 수정하지 않았다. Name·지역화 Text·정규화·shaping·플랫폼 문자열 변환은 구현하지 않은 후속 범위로 정확히 구분되어 있다.

## 구현과 반례 대조

| 경계 | 독립 검토한 반례와 실제 처리 | 판정 |
|---|---|---|
| UTF-8 유효성 | ASCII와 2/3/4-byte scalar를 구분한다. continuation을 읽기 전에 남은 길이를 검사하고 최소 scalar, surrogate, U+10FFFF 상한으로 overlong·잘림·범위 초과를 거부한다. NUL과 noncharacter는 유효 scalar로 취급한다. | 계약 일치 |
| Slice | `offset > bytes`와 `count > bytes - offset`을 먼저 검사해 합의 overflow를 피한다. 원본 전체 UTF-8 유효성과 양 끝의 scalar 경계를 요구하며 빈 slice도 문자 중간이면 거부한다. grapheme 보장은 하지 않는다. | 계약 일치 |
| 자기 참조와 겹침 | in-place Assign/Append는 memmove를 사용한다. 성장 경로는 원래 payload와 source를 모두 새 버퍼에 복사한 뒤 이전 저장소를 반환한다. 종단 NUL까지 포함하는 자기 뷰로 성장하는 Assign과 겹치는 Append도 테스트한다. | 계약 일치 |
| 실패 원자성 | UTF 오류와 overflow는 쓰기 전 반환한다. OOM은 새 저장소가 생기기 전 반환해 내용·capacity·기존 주소와 뷰를 보존한다. Reserve/Assign/Append/Clone의 실패와 lifecycle의 각 할당 실패를 별도로 검사한다. | 계약 일치 |
| 길이·종단·성장 | payload 길이와 종단 NUL 공간을 구분한다. AllocateBuffer의 checked addition과 Append의 checked 합으로 wrap을 막는다. GrowthCapacity는 최대 payload `MaxSize - 1`에 포화한다. 빈 문자열도 읽을 수 있는 종단 포인터가 있다. | 계약 일치 |
| 이동·복제·할당기 | move assignment는 이전 저장소를 이전 allocator로 반환하고 새 저장소의 allocator도 함께 이관한다. 원본은 기존 allocator를 가진 빈 값으로 재사용할 수 있다. TryClone은 지정한 allocator에 독립 복사한다. | 계약 일치 |
| 수명·스레드 | view는 메모리를 소유하지 않고 실제 readable 범위는 caller 전제다. 동일 String의 동시 변경과 allocator 콜백의 변경 재진입은 금지한다. 별도의 동기화나 GC 안전성을 암묵 보장하지 않는다. | 계약 일치 |
| STL 경계 | 대상 구현과 공개 API에 std:: 문자열·컨테이너·알고리즘은 없다. `<string.h>`의 memcpy/memmove는 바이트 복사 경로이며 STL 문자열 도입과 구분한다. | 계약 일치 |

13개 테스트는 고정 UTF-8 golden bytes, 잘못된 입력 분류, 결합 문자와 scalar 경계의 차이, 내부 NUL, 자기 전체/부분 뷰, 5단계 N-fail, allocator 간 move/clone, 1,000회 lifecycle을 포함한다. golden 값은 validator의 동일 분기를 다시 실행하는 oracle이 아니다. OOM 시험은 결과뿐 아니라 주소·capacity·원문·기존 뷰·live allocation/byte를 검사한다.

overflow 실행 fixture는 `Reserve(MaxSize)`와 유효 입력의 slice 범위 초과를 검사하며 공통 checked 산술 회귀가 덧셈 경계를 검사한다. 실제로 읽을 수 없는 거대 뷰를 만들어 Append에 전달하지 않는다. 따라서 거대 payload의 실제 할당·Append 실행을 검증했다고 주장하지 않으며 그 경로는 checked 연산과 분기 순서의 소스 검토로 구분한다. `StringView(nullptr, nonzero)`는 Fatal 사전조건이지 Result로 복구하는 UTF 내용 오류가 아니다.

## 문서 보완 확인

- 지역화 의존 방향을 `UI/Studio → Localization → Core.Text`로 명시해 Core의 상위 모듈 의존으로 읽히지 않게 했다.
- 파괴 요청 객체의 보존 조건을 외부 RootRef로부터의 도달성으로 한정했다. 도달 불가능한 cycle은 수거하며 간선 존재만으로 보존하지 않는다.
- 실제 metadata fixture를 원본 변경 후 독립 복사본 읽기로 설명하고, GC oracle를 인접 배열/DFS로 표기했다.
- null+nonzero의 Fatal 계약, Name/Text 등 후속 범위, 소스 검토 당시의 최종 실행 대기 상태를 확인했다. 이전 Object ASan 52개 통과를 String 검증 결과로 합산하지 않는다.

## 최종 실행 증거 확인 — 2026-11-10

build 담당이 실행한 Windows x64 결과를 각 preset의 `foundation-report.json`, `build-evidence.json`, `Testing/Temporary/LastTest.log` 원문으로 독립 확인했다. 모든 preset에 String 테스트 13개가 포함되며 seed는 7351이다.

| 실행 | 테스트 | CTest suite | 결과 |
|---|---:|---:|---|
| windows-clang-core | 38 | 5 | failed_tests=0, failures=0 |
| windows-clang-debug | 65 | 5 | failed_tests=0, failures=0 |
| windows-clang-release | 65 | 5 | failed_tests=0, failures=0 |
| windows-clang-asan | 65 | 6 | failed_tests=0, failures=0 |

네 preset의 build evidence는 모두 PASS, `sourceStableDuringRun=true`, `runnerReportCurrent=true`이며 실행 전후 source digest가 같다. 공통 digest는 `62cfbadfab18902669f767aae452a0a7a901be8eea67554cd14a02aa55b7b3c4`이고 대상 파일은 80개다. Git dirty 작업 트리에서 실행한 결과이며 clean commit 실행으로 바꾸어 표현하지 않는다.

ASan의 여섯 번째 suite `asan_negative_control`은 별도 자식 프로세스에 의도한 use-after-free를 만들고 실제 AddressSanitizer 진단을 확인해 통과했다. 정상 테스트의 메모리 오류를 성공 처리한 것이 아니다. Windows ASan 결과를 LeakSanitizer 또는 데이터 경쟁 검증으로 해석하지 않는다.

Release의 `repeat-report.json`, `repeat-evidence.json`, `repeat-output.log`도 확인했다. 동일 seed 7351로 65개 테스트를 각각 100회 실행한 총 6,500회에서 failed_tests=0/failures=0이다. String 13개도 반복 대상에 포함된다. 반복 evidence의 source digest 역시 위 값과 같고 실행 전후 안정 상태를 기록한다.

이로써 현 String/StringView 구현의 Windows 실행 증거 확인을 마쳤다. 이 확인 중에는 문서만 갱신했으며 source freeze를 유지했다. macOS native CI와 commit/push 확인은 root 담당의 별도 후속이며, 지역화 Text·Name·UI 통합이나 전체 Foundation Gate 완료로 확대하지 않는다.
