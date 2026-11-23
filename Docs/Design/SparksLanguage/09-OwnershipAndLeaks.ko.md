## 9. 소유·해제와 누수 방지

전체 계획 §29.6은 메모리 누수 0을 출시 Gate로 두고 모든 allocation·resource에 owner scope와 최종 회수 조건을 명시하도록 규정한다. Sparks는 소유 범위를 고정하고 범위 종료의 해제 경로를 3개 연산으로 한정한다.

### 9.1 소유 범위와 해제 연산

최상위 소유 범위는 **Session**(PlaySession 또는 process host), **Instance**(스크립트 수명 객체), **Execution**(handler 실행 1회와 그 `All`/`Race` 하위), **ModuleHost**(코드)의 4개다. 스크립트는 소유 범위를 새로 만들지 않는다. 범위가 끝날 때의 일괄 해제는 다음 연산으로만 일어나며, 이 연산은 사용자 코드를 실행하지 않고 할당하지 않고 멈추지 않으므로 실패할 수 없다. 범위 안의 개별 해제(대입·블록 종료·참조계수 0)는 §9.2를 따르며 같은 조건을 지킨다. admission은 새 실행·전달·로드 요청의 수락이다. 격리(§8.3)는 Detach의 앞 단계(실행 EndExec·유지 자원 해제·연결 key 폐기)를 먼저 수행하고 값 필드 초기화를 Destroy hook 뒤로 미룬 것이며 같은 실패 불가 조건을 지킨다.

| 연산 | 수행 내용 |
|---|---|
| `EndExec(frame)` | 하위 실행 종료 → 대기 key 폐기 → 지역 유지 자원을 역순 해제 → 값 슬롯 해제 → revision lease 반환 |
| `Detach(instance)` | admission 차단 → 모든 실행 EndExec → 필드 유지 자원을 선언 역순 해제 → 연결 key 폐기 → 자기 이벤트 delegate 비우기 → 값 필드 초기화 |
| `CloseSession` | admission 차단 → 모든 instance Detach → 큐·Changed 자리 폐기 → World 파괴 → 연결 표 해제(이 시점에 ledger 0). 정상 종료 hook은 그 전에 World 정상 종료에서 실행한다(§5.1) |

| 종료 경로 | 적용 |
|---|---|
| 정상 종료·fault·취소 | EndExec(fault는 Execution 트리 전체, 취소는 취소된 실행과 그 하위) |
| 비활성화 | Disable hook 후 실행 취소·유지 자원 해제·연결 비활성 |
| 파괴 | 스크립트 단계 끝 적용(PendingDestroy) → 프레임 끝 정리에서 묶음 단위 Disable(활성이면) → Destroy hook → Detach(§4.3) |
| 격리 | hook 없이 실행·유지 자원·연결 해제, 값 필드 보존. 그 instance가 파괴될 때 Destroy hook 후 Detach |
| GC가 도달 불가로 판정 | 회수 전 cleanup hook에서 Detach(hook 없음, 정확히 1회 보장, 전체 계획 §5.2 — 연동 요구 SE-O1) |
| 셀 일괄 unload | Disable·Destroy, 각 Destroy 반환 직후 Detach. 발행자별 compact는 셀 단위 1회 |
| travel | 이전 World 정상 종료(Disable·Destroy·Detach). Session은 유지 |
| Tryout Stop | World 정상 종료(Disable·Destroy) 후 CloseSession |
| reload | 변경된 `on` 선언과 필드만 해제. 대기 중인 이전 revision 실행은 §12.4를 따른다 |
| process 종료 | 모든 World 정상 종료 → 모든 CloseSession → ModuleHost → TypeRegistry → NamePool Shutdown. 정해진 시간 안에 Busy가 해소되지 않으면 강제 해제하지 않고 NL06 실패로 보고 |

### 9.2 대상별 직접 보유자와 해제 시점

| 대상 | 직접 보유자 | 해제 시점 |
|---|---|---|
| 값 버퍼(string·Array·Map) | 보관한 슬롯(참조계수), 회계는 instance | 참조계수 0 |
| 이벤트 연결 | Instance의 연결 집합 | Detach, 격리, 원천 필드 변경, 선언이 추가·삭제·변경되는 reload, CloseSession |
| 이벤트·시간·로드 대기 | Execution | 발생 시 또는 EndExec. 발행자 소멸 시 대기가 완료되지 않은 채 끝남(§6.1) |
| 실행 | handler 실행 자리 또는 부모 실행 | 반환·fault·취소·Detach |
| 발행 큐 항목 | Session | 전달, 대상 소멸 시 폐기, CloseSession |
| 유지 자원 | 담긴 필드 또는 지역 변수 | 대입·`null`·블록 종료·비활성화(Disable hook 반환 후)·격리·EndExec·Detach. backend 완료가 늦으면 유지 자원 slot의 generation을 확인하여 폐기된 slot에 도착한 완료는 버린다 |
| 일회성 효과 | 엔진 subsystem | 최대 지속 시간 후 스스로 종료. CloseSession(Tryout Stop 포함)이 시작되면 재생 중인 것을 즉시 중지하고 이후 호출은 효과 없이 반환 |
| 에셋 lease | 결과 값을 보관한 슬롯·생성된 Actor | 참조계수 0, Actor 파괴 |
| 생성된 Actor | World 또는 부착 부모 | 스크립트 단계 끝 파괴, 부모 파괴, CloseSession. 설계상 상한이 없는 유일한 증가이므로 검출 대상 |
| code revision | ModuleHost, instance·frame이 lease | 마지막 lease 반환. process 단위 ledger로 검사 |
| eval·외부 모듈 | EvalScope 또는 ModuleLease | 범위 종료, 비동기 unload(§12.7) |
| 진단 표 | session ring, 위치별 counter는 revision(hash key) | ring 덮어쓰기, revision 반환. 강한 참조를 보관하지 않음 |

### 9.3 예산과 Gate

- **예산(논리 단위, SD34):** 상한은 획득 시점에 거부하며(로드 요청은 거부하지 않고 admission을 지연, 실행은 건너뜀) 해제 경로에서는 적용하지 않는다. 기본값은 프로젝트 설정이며 쿡 시 고정된다. 서버·클라이언트·재시뮬레이션의 fault 결과가 같아야 하므로 실행 중에는 바꿀 수 없고, 엔진 쪽 표현은 `BootCVar` + `Replicated()`(`sparks.budget.*`, CVarSystem)다.

| 예산 | 기본값 | 단위 |
|---|---|---|
| 명령어 수: 실행 조각 | 1,000,000 | bytecode 명령어(AOT도 같은 계수, §12.5) |
| 명령어 수: Disable·Destroy(Teardown) | 100,000 | 같음 |
| 명령어 수: eval 1회 | 1,000,000 | 같음 |
| 호출 깊이 | 256 | frame |
| VM 메모리: instance / session | 16 MiB / 512 MiB | 논리 크기(원소 수 × 원소 크기, 공유 버퍼는 보유자마다) |
| 단일 버퍼 | 16,777,216 원소 또는 64 MiB | 먼저 닿는 쪽 |
| 유지 자원: 종류별 instance당 | 64 | 개 |
| 구독 연결: instance당 / session당 | 4,096 / 1,048,576 | 항목 |
| 동시 실행·대기: instance당 | 256 | 실행 |
| 진행 중 로드: instance당 | 32 | 요청 |
| Spawn: 실행 조각당 / 스크립트 단계당 | 1,024 / 8,192 | Actor |
| 전달 수 N: 스크립트 단계당 / 발행 큐: session당 | 65,536 / 262,144 | 항목(§5.4) |
| 적용 루프: 스크립트 단계당 | 64 | 회(§5.1) |
| eval scope | 16 | 개 |
| pause 중 큐: 종류별 | 4,096 | 항목(§9.4) |
| 종료 Busy deadline / TypeRegistry 재시도 | 5 s / 120 프레임 | (§9.1, §12.7) |

생성 위치별 생성 Actor 수와 모듈별 revision 수는 상한 없이 ledger에 표시한다(revision은 §12.4의 살아 있는 revision 상한 8이 별도로 적용된다).
- **매 스크립트 단계 디버그 불변식:** 연결 표의 살아 있는 key 수와 항목 수 일치, 발행자별 죽은 항목 ≤ 직전 compact 시점의 살아 있는 항목 + 8(C2), instance별 유지 자원 ≤ 필드 수 + 활성 frame의 지역 수.
- **G1 · Tryout Stop:** CloseSession 직전 연결 표 참조계수 1, CloseSession 후 모든 스크립트 counter 0, revision lease와 이름 pool이 기준선 복귀(전체 계획 §29.6 NL09).
- **G2 · 2시간 soak:** 생성·파괴 반복, 배열 재작성, 5분마다 reload, 10초마다 eval. 30초 구간별 최솟값을 기록하고, 연속 10구간 중 직전 구간보다 큰 구간이 8개 이상이면서 그 10구간의 최소제곱 기울기가 예산의 분당 1%(예산이 없는 counter는 시작 값의 분당 1%)를 넘는 counter가 없어야 한다. 위반 시 생성 위치별 증가량 상위 항목을 보고한다.
- **G3 · 음성 대조:** key 폐기 생략, C2 비활성, 취소 시 지역 자원 누락, 이름 pool 삽입 등 의도한 누수가 Gate를 실패시키는지 확인한다.
- **G4 · 실패 행렬:** 모든 획득·`TryAdd`·재연결에서 N번째 실패, 모든 `await`에서 fault·취소를 주입하고 Detach 후 모든 counter 0을 확인한다.
- **G5 · reload 행렬:** §12.4의 변경 종류마다(본문·필드·`on`·component 구성·클래스 계층·`@net`·`@rpc`·const·사전) 대기 중인 실행·유지 자원·구독 연결이 있는 상태에서 reload하고, 롤백 경로(계획 실패·instance 재생성 실패)를 주입해 counter·필드 값·연결 수가 기대와 같은지 확인한다.
- **G6 · backend 차등:** conformance corpus를 VM과 AOT로 실행해 결과·fault 종류와 위치 ID·이벤트·Changed 순서·ledger counter·예산 초과 지점을 비교한다. AOT를 고를 수 있는 모든 target에서 돈다(§12.5).

### 9.4 lane과 일시 정지

- VM 버퍼는 Game lane에서만 변경한다. 비동기 저장·복제 직렬화·디버거 watch·telemetry는 고정된 버퍼를 읽고 해제를 Game lane으로 되돌리며, ledger 검사 전에 이를 모두 처리한다. 되돌림은 실패할 수 없어야 하므로 버퍼 머리의 노드를 쓰는 `IntrusiveMpscQueue`(Synchronization 12.2절, AssetSystem lease 해제와 같은 방식)로 하며 용량 제한 큐를 쓰지 않는다.
- Sparks handler는 Game lane에서 발행되는 이벤트에만 연결한다. 다른 lane의 이벤트는 엔진이 POD로 전달하고 Game lane에서 값으로 생성한다.
- breakpoint·pause 중 쌓이는 큐(lane 간 전달, UI clock 대기, Changed)는 상한이 있고 같은 종류를 합치며 넘친 수를 집계한다(전체 계획 §23.4).

### 9.5 이름 pool

Core `NamePool`은 항목을 Shutdown에서만 해제하며 기본 상한이 65,536개다. 런타임 문자열을 이름으로 바꾸는 경로는 검색 전용 `TryFind`(Core 변경 C5)를 사용해 새 항목을 만들지 않는다. 런타임 문자열이 native `Name` 인자로 흘러가는 바인딩은 binding schema 오류다. source eval은 이름을 새로 등록하지 않는다.
