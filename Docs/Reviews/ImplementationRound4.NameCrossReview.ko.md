# Implementation Round 4 — Name 독립 교차 검토

2026-11-10 기준 `Name.h`, `NamePool.h`, `Name.cpp`와 `NameTests.cpp` 10개 테스트를 읽기 전용으로 검토했다. 구현·테스트 담당과 다른 관점에서 초기화/종료, 후보 예약, 중복 게시와 lease 수명을 대조했다. 수정이 필요한 확정 결함은 발견하지 못했다.

| 복합 반례 | 실제 계약과 검토 결과 |
|---|---|
| Initialize의 domain 발급 중 Shutdown | Initializing과 activeOperations를 먼저 게시한다. Shutdown이 Closing으로 바꾸면 domain 발급 뒤 Initialize가 ShuttingDown으로 반환하며 admission을 다시 열지 않는다. 후속 Shutdown으로 Stopped에 도달해야 재초기화할 수 있다. |
| TryIntern의 UTF/hash 처리 중 종료·재초기화 | Running 확인과 activeOperations 증가를 같은 gate에서 수행한다. config를 gate 밖에서 읽는 동안 Shutdown은 Busy이고 Initialize는 Stopped 조건을 만족하지 못한다. config 갱신 경합을 차단한다. |
| 게시 예산이 가득 찬 상태의 중복 이름 | 기존 byte 조회가 예산 검사보다 앞선다. 이미 존재하는 이름은 추가 후보나 메모리 없이 동일 Name을 반환한다. 신규 후보는 posted+pending entry/byte 합을 넘지 못한다. |
| 여러 worker의 동일 이름 후보 | 할당 전 예약과 할당 후 중복 재검사를 분리한다. 승자는 pending을 posted로 옮기고, 패자는 후보 해제가 끝날 때까지 activeOperations와 예약을 유지한다. cleanup보다 Shutdown 성공이 앞서지 않는다. |
| Acquire와 Shutdown 경합 | gate 안에서 현재 domain/id를 검증하고 activeLeases를 증가한다. 성공한 lease가 있으면 Shutdown은 Busy이며 이전 immutable view가 유지된다. 반환은 다른 스레드에서도 가능하다. |
| 재생성/외부 pool/빈 이름 | process-local domain과 게시 id를 모두 확인한다. 재초기화는 새 domain을 사용하고 슬롯 id가 같아도 이전 token을 거부한다. 빈 문자열은 유효 entry이고 기본 Name은 비어 있는 식별자로 구분한다. |
| allocator 콜백 재진입 | Allocate/Deallocate는 gate 밖이다. 후보 cleanup에는 operation 예약이, 전체 entry 회수에는 Reclaiming 상태가 남는다. Stats/Shutdown 재진입은 교착하거나 조기 완료하지 않는다. |

예산 산술은 posted와 pending 합이 설정 상한 이하라는 불변식 아래 남은 한도를 빼서 비교한다. 후보 크기는 metadata+payload+종단 NUL을 checked addition으로 검사한다. id는 serial과 bucket bits를 조합하되 serial 최대치를 확인하여 wrap을 허용하지 않는다. hash collision은 전체 길이·byte 동등 비교로 해결하며 UTF-8 검증과 무정규화 정책을 분리한다.

테스트는 8개 worker를 실제 후보 할당 단계에 모은 뒤 게시 경합을 실행하고, 할당과 취소 후보 Deallocate 각각에 barrier를 두어 Shutdown Busy·pending 계수를 확인한다. forced collision, cross-thread lease 반환, allocator Stats/Shutdown 재진입, N-fail·1,000회 생성/종료도 포함한다. 테스트가 단순히 동일 구현의 hash 결과를 기대값으로 복사하지 않는다.

이 검토는 소스와 fixture의 독립 대조다. Initialize의 전역 domain 발급 중간에 Shutdown을 강제 삽입하는 전용 fixture는 없으므로 그 경계는 소스 검토 근거와 실제 강제 실행을 구분한다. native 실행 성공 여부는 최종 build evidence를 따른다. 원본/테스트는 수정하지 않았으며 TSan·임의 스케줄 전체 탐색이나 제품용 성능을 검증한 것으로 확대하지 않는다.
