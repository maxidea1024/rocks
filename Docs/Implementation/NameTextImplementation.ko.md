# Rocks Name·Text 구현 계약과 검증

기준일: 2026-11-10. C13 이름 intern과 문자열 상위의 지역화 Text를 구현하는 단계다. Core 전체 완료나 언어별 표시 규칙·UI 완성을 의미하지 않는다. 엔진 소유 Runtime과 공개 API는 STL을 사용하지 않는다.

## 1. 모듈과 타입의 역할

의존 방향은 `Localization → Core → PlatformBase → Base`다. `Name`은 Core에 있고, `Text`는 별도 `rocks_localization` target에 있다. Core와 ObjectRuntime이 Localization에 의존하지 않는다. CPU 전용 Foundation/CoreFoundation 테스트 호스트가 두 모듈을 함께 검증한다.

| 타입 | 역할 | 소유권 |
|---|---|---|
| String / StringView | UTF-8 소유 값 / 비소유 바이트 뷰 | 기존 Core 문자열 계약 유지 |
| Name | 세션 내 intern identity | domain+id 값, 문자열이나 pool을 소유하지 않음 |
| NamePool / NameLease | intern storage / 읽기 수명 보존 | pool이 저장소 소유, lease가 Shutdown 완료를 막음 |
| Text | 원문·번역 ID·format 인자의 불변 이력 | 명시 Clone으로 노드 공유, 입력 문자열과 인자의 수명을 독립 보존 |
| CatalogSnapshot | 특정 문화·revision의 불변 번역 데이터 | 입력을 깊게 복사, store보다 오래 보유 가능 |
| CatalogStore | 현재 snapshot 게시 | 교체·Close 이후에도 이미 획득한 reader는 유효 |
| ResolvedText | 표시할 결과 및 fallback 진단 | 입력 Text와 snapshot에서 독립한 String |

이름이나 타입에 U/F/A 등의 접두사를 붙이지 않는다. 번역된 결과를 Name·게임플레이 정체성·네트워크 비교 키로 사용하지 않는다.

## 2. Name 계약

파일은 `Core/Text/Name.h`, `Core/Text/NamePool.h`, `Core/Private/Text/Name.cpp`다.

### API와 상태

`NamePool(Allocator&)` 뒤 `Initialize(config)`로 시작한다. `TryIntern(StringView)`는 유효 UTF-8을 intern하고 `Result<Name>`을 반환한다. `Acquire(Name)`은 `Result<NameLease>`를 반환하며 lease의 `View()`로 원문을 읽는다. `TryClone()`은 명시적으로 reader 수를 늘린다. `Reset()`으로 반환한다.

기본 Name은 이름 없음이며 빈 문자열을 intern한 유효한 이름과 구분한다. UTF-8 바이트를 대소문자 구분하여 비교한다. 정규화·locale case folding은 암묵 적용하지 않는다. 내부 NUL은 길이 기반 데이터로 허용한다. FFI·OS 경계에서는 해당 경계의 입력 제한을 적용해야 한다.

각 초기화는 새로운 process-local domain을 받는다. pool 재생성·재초기화 이후의 이전 Name은 거부한다. domain과 entry id를 wrap하여 재사용하지 않는다. 영속 저장·네트워크에는 원문 또는 별도 stable Guid를 사용한다.

`Shutdown()`은 새 intern/acquire/clone을 닫는다. 진행 중 할당·cleanup 또는 lease가 있으면 Busy이며 닫힌 상태를 유지한다. caller 종료·lease 반환 후 재호출하면 저장소를 반환한다. pool 소멸 전에 모든 API caller를 join하고 lease를 해제해야 한다. allocator는 pool과 보관 중인 저장소보다 오래 살아야 한다.

### 할당·동시성

첫 구현은 고정 64개 bucket과 단일 gate를 사용한다. 샤드별 병렬 잠금은 계획에 남기되 현재 구현으로 표시하지 않는다. hash 충돌은 바이트 전체 비교로 해결한다. hash는 조회 보조이며 영속 digest나 공격 입력 방어 수단으로 주장하지 않는다. 테스트용 hash mask로 모든 이름의 충돌을 강제한다.

후보 메모리 예산을 예약한 뒤 allocator 호출과 문자열 복사는 gate 밖에서 수행한다. 게시 직전에 중복을 다시 확인하여 경쟁 intern의 성공 결과는 하나의 identity를 공유한다. 패자 후보와 실패 cleanup까지 active operation으로 유지하여 Shutdown이 먼저 완료되지 않도록 한다. allocator callback의 Stats/Shutdown 재진입을 허용하되 pool 자체 파괴는 금지한다.

기본 한도는 entry 65,536개와 저장소 16 MiB다. byte 예산에는 entry metadata, 종단 NUL, 게시 전 후보 예약을 포함한다. 다른 요청의 예약 때문에 일시적으로 CapacityExceeded일 수 있다. 실패가 기존 게시 entry를 변경하지 않도록 한다.

## 3. Text 계약

파일은 `Localization/Text.h`, `Localization/Catalog.h`와 해당 Private 구현이다. 문자열 처리는 자체 String/StringView를 사용한다. 안정적인 TextId는 Guid이며 Name의 세션 ID와 별개다.

### 생성과 이력

- `Text::Invariant`: 언어와 무관한 UTF-8 원문을 소유한다.
- `Text::Localized`: TextId, 0이 아닌 sourceRevision, 원문 fallback을 보존한다.
- `Text::Integer`: locale에 의존하지 않는 signed 64-bit ASCII 10진수 문구를 만든다. 언어별 숫자 서식이나 plural 선택 기능이 아니다.
- `Text::Format`: 패턴 Text와 이름 있는 Text 인자를 보존한다. 인자 이름은 `[A-Za-z_][A-Za-z0-9_]*`, 최대 64 byte이며 중복을 거부한다. 인자 값과 이름을 임시 입력에 의존하지 않도록 보존한다.

Text는 move-only이며 `Clone()`으로 명시 공유한다. 생성 이후 기존 노드만 참조하는 불변 DAG여서 생성 API로 cycle을 만들 수 없다. 마지막 반환은 추가 할당 없이 반복형 처리로 수행하여 깊은 이력의 파괴가 재귀 스택에 의존하지 않도록 한다. 다른 allocator에서 생성된 자식도 각 원래 allocator로 반환한다. 모든 관련 allocator가 마지막 노드보다 오래 살아야 한다.

### 번역 선택과 서식

`ResolveText(allocator, text, snapshot, limits)`는 소유 결과 String과 진단 카운터를 반환한다. 전역 locale이나 현재 store를 암묵 조회하지 않으며 호출에 전달한 snapshot만 사용한다. 빈 snapshot은 번역 없음이다. 번역 ID 누락은 원문 fallback+missingTranslations, sourceRevision 불일치는 원문 fallback+staleTranslations로 기록한다. 유효한 빈 번역과 번역 누락은 다르게 처리한다.

Format 문법은 `{name}`, `{{`, `}}`다. 이름·중복은 Format 생성 시, 실제 패턴 문법과 필요한 인자 유무는 번역을 선택한 Resolve 단계에서 검사한다. 잘못된 문법·없는 인자는 InvalidArgument다. 사용하지 않는 인자는 허용한다. 삽입된 인자 결과의 brace는 다시 파싱하지 않는다. literal은 UTF-8 경계를 지키는 구간으로 추가한다.

기본 resolve 한도는 출력 1 MiB, 깊이 64, work 8 Mi 단위다. source 16 MiB·인자 64개 등의 생성 hard ceiling도 적용한다. work에는 node 방문, 패턴/원문 byte 검사, 출력 byte, catalog 후보 및 인자 이름 비교 비용을 포함한다. 한도 초과는 CapacityExceeded, 잘못된 limit 설정은 InvalidArgument, 할당 실패는 OutOfMemory다. 실패 시 부분 결과를 외부에 게시하지 않는다.

### Catalog와 언어 변경

CatalogSnapshot 생성은 culture·revision과 모든 entry/번역을 깊게 복사한다. 빈/잘못된 culture, 0 revision/ID, 중복 TextId를 거부한다. culture는 자동 정규화하지 않는 명시 식별자다. 기본 entry 한도는 4,096개, 입력 byte 예산은 4 MiB이며 hard ceiling과 구분한다.

CatalogStore는 생성된 snapshot을 Publish하고 reader가 Acquire한다. 기존 snapshot 정리는 gate 밖에서 한다. Close 뒤 새 Publish/Acquire는 ShuttingDown이며 이미 획득한 reader는 계속 사용할 수 있다. store가 파괴되어도 snapshot은 allocator가 살아 있으면 유효하다. store 파괴는 store API 호출이 모두 끝난 후 수행한다.

Publish는 호출자가 정한 게시 순서를 따른다. 비동기 로더의 오래된 요청을 자동으로 판별하지 않는다. 로더·PIE 계층에서 request generation을 검사한 뒤 Publish해야 한다. culture 전환과 같은 culture의 콘텐츠 교체는 동일한 snapshot 게시 경로를 사용한다. UI는 문화와 revision 정책에 따라 layout을 갱신하는 후속 소비자다.

서로 다른 Text/snapshot 래퍼의 Clone·읽기·Reset을 병행할 수 있지만 동일 래퍼의 이동·Reset·읽기 경합은 호출자가 막는다. 할당기가 실제 호출 스레드를 지원해야 한다. 공유 계수 잠금이 사용자 래퍼 전체의 무제한 동시 변경을 허용하는 것은 아니다.

## 4. 검증 기준

| 영역 | 독립 검증 기준 |
|---|---|
| Name identity | 별도 pool·재초기화 stale 거부, 빈 값/한글/NUL/대소문자, 강제 hash 충돌에서도 byte 동등 기준과 일치 |
| Name 수명·경합 | 동일/상이 입력 native thread 경쟁, lease 유지 중 Busy, 차단된 allocator와 Shutdown 경합, callback 재진입, 예약 예산 반환 |
| Text 이력 | 입력 원문·인자 scratch 해제 이후 읽기, Clone/이동/다른 allocator 소유권, 깊은 DAG의 비재귀 해제 |
| 번역 | exact sourceRevision, missing/stale fallback 카운터, 빈 번역, 여러 문화 snapshot, store/원래 wrapper 해제 후 reader 유지 |
| 서식 | 한글·emoji·내부 NUL golden, brace escape, 삽입값 재파싱 금지, 이름/문법 실패, 정수 최솟값·최댓값, 출력/깊이/work 경계 |
| 장애 | 각 실제 allocation에 대한 N-fail sweep, 실패 후 기존 게시값·입력 유지, 모든 임시 owner 반환과 live allocation/byte 0 |
| 통합 | Name identity가 언어 전환과 독립, 이전 프레임 snapshot 고정, 반복 headless session 종료 후 잔여 owner 0 |

동시성 fixture는 thread-safe SystemAllocator를 사용하고 NFailAllocator의 비동기 계수 경합을 만들지 않는다. sleep 시간 추측보다 명시 gate/이벤트로 필요한 순서를 만든다. 데드락이 CI를 무한 점유하지 않도록 CTest 시간 한도를 둔다.

Name 10개·Localization 24개와 플랫폼 확장을 포함한 전체 110개가 Windows Debug/Release/ASan에서 통과했다. Core-only는 83개이며 CTest는 일반 8개/ASan 9개다. 실제 source digest·반복 실행·native CI·교차 검토 결과는 [Foundation 최신 검증 기록](FoundationStatus.ko.md)을 따른다. 테스트 작성·소스 검토·실제 실행의 범위를 구분한다.

스레드 생성과 동기화는 [공통 Platform API](PlatformConcurrency.ko.md)를 사용한다. 공용 구현·테스트의 OS 분기와 네이티브 호출을 private backend로 옮겼다. 모든 C++ 구성은 [컴파일러 경고 무허용 정책](../Design/CompilerDiagnostics.ko.md)을 따른다.

## 5. 후속 범위

Name pool의 샤드 잠금/검색 최적화, Guid 생성·parse·format, 일반 hash/Path, catalog 추출·쿠킹·DDC·비동기 로더·request generation, locale별 숫자/통화/날짜·plural/select adapter, BiDi·shaping·폰트·IME/UI는 아직 후속이다. Text를 String의 별명으로 만들지 않고 이력과 snapshot 경계를 먼저 확보한 상태에서 확장한다.

언어별 규칙은 검증된 라이브러리 adapter로 도입한다. 이번 invariant integer·named substitution을 완전한 국제화 엔진으로 표기하지 않는다. Windows/macOS Foundation 통과를 실제 에디터·UI 통합 검증으로 합산하지 않는다.
