# Rocks GC 대규모 처리 보강 설계

기준일: 2026-11-16, 개정 1(초안). 개정 16(2026-11-20, 사용자 지시 "남은작업 진행"): 18.11절 남은 연결과 Trim(AL7) — 게임 템플릿·Commandlet 연결, `Allocator`의 `TrimMemory`·`MarkThreadIdle`·`MarkThreadActive`, 쉬는 스레드 캐시를 `TrimMemory`가 비움, `WorkerScheduler`의 표시, 엔진의 호출 자리 둘과 `WorldGcSchedule::trimMemory`, 리뷰 후속 가운데 작은 것. 개정 15(2026-11-20, 사용자 지시 "엔진에 BinnedAllocator 연결해"): 18.10절 제품 연결(AL6) — 실행 진입점(Launch)이 `SystemAllocator` 위에 `BinnedAllocator`를 하나 두고 범용 할당자로 내주며 Sandbox 엔진 호스트가 그것을 엔진에 넘긴다, 11절 결정 8 확정, 0절·18절 머리 갱신, `ThreadExitSlot`의 OS 오류 값 보존(18.4절). 개정 14(2026-11-20, 마무리): 0절 "현재 상태"를 더함(단계별 상태, 8단계 결과, 남은 한계), 4절 gate 표에 현재 값 열, 6절 GC-S6·11절 결정 8(`BinnedAllocator`의 제품 연결 — 결정 대기), 18절 리뷰 반영(겹친 중복 반환의 slab 경로 한계, 수치 맞춤). 개정 13(2026-11-20, 메인 세션 판정): 18.4절 — 반환 표지를 비교 교환에서 비교 뒤 단순 대입으로 되돌리고(개정 9의 결정을 뒤집음 — 측정한 비용), 대신 블록을 내줄 때 표지가 서 있는지 검사해 Fatal로 잡음. 18.2·18.6·18.8절 — 할당자 비교의 플랫폼 기준을 바로잡고(AL0–AL4의 platform은 계수 장치가 섞인 `BenchAllocator`였다) gate A1을 새 기준으로 다시 정함, gate A2에서 "몫이 절반 이하"를 뺌. 18절·18.6절 — 사실 정정: 지금 제품 코드는 엔진에 `SystemAllocator`를 넘긴다(`BinnedAllocator` 연결은 결정 대기). 18.8절 — 쉬는 스레드의 캐시 상한. 개정 12(2026-11-20): 18.4절 — `ThreadExitSlot`의 `thread_local` 계약을 좁힘(macOS CI: callback 안에서 읽은 `thread_local`은 초기값 — 값이 남아 있다는 보장이 없다), 할당자는 스레드 종료 칸의 값을 기준으로 삼음(`thread_local` 포인터는 사본), 남는 한계와 플랫폼별 순서, fiber 한계, 종료 칸에 걸지 못한 스레드(메인 세션 검토 AL3). 개정 11(2026-11-20): 18.6절 — 원장의 지역성 hash를 버린 대안으로 옮김(메인 세션 판정: 군집이 표의 채움 비율에 따라 절벽으로 커진다), 18.8절 gate A1 확정(churn 회수 3.5배 이내). 개정 10(2026-11-20): 18.4절 주소 범위 — POSIX에서 slab마다 보호를 바꾸지 않는 접근을 열어 둔 예약(`VirtualMemoryAccess::Open`, UE 5.8 방식), commit·decommit 실패가 주소 범위를 닫지 않음(메인 세션 검토 AL2b). 18.6절 — `HashMap`의 0개 비우기 규칙 고침, 원장의 지역성 hash(AL1b). 개정 9(2026-11-20): 18절을 구현 전에 보강 — 예약·commit·decommit 실패의 처리, 표지 비밀값의 최상위 비트와 비교 교환(동시 중복 반환), 캐시 없는 구성(`threadCaches`), 큰 경로의 상위 주소 검증, `ThreadExitSlot`의 API 모양과 등록부 수명, `Trim`·`Usage` API. 개정 8(2026-11-20): 18절 보강(메인 세션 검토 F1–F7) — 반환 표지 불변식(떼어 묶음에 넣을 때 세움), 오용 검출의 동시성 한계와 완전한 대안, 스레드 종료 hook(`platform::ThreadExitSlot`), 예약 실패 때의 축소·큰 경로, ASan에서의 내부 접근, 주소 원장을 공용 내부 구현 하나로(Core `HashMap`), 이전 칸은 가득 묶음만. 개정 7(2026-11-20): GC-S6 공용 할당자 설계(18절 — `SystemAllocator` 원장을 주소 hash로, `BinnedAllocator`를 예약 주소 범위·크기 등급 slab·스레드 캐시 구조로 다시 만듦, UE 5.8 Binned2·3·4 대조, gate·시험 계획). 설계만이며 구현은 다른 세션의 빌드 작업 병합 뒤 시작한다. 개정 6(2026-11-19): GC-S5 수집 일정과 파괴 예산(17절 — 묶음 회수, 회수를 미루는 STW 수집과 예산 회수, World 수집 일정, 엔진이 소유하는 GC scheduler), 6절 GC-S5·11절 결정 3 갱신. 리뷰 반영(17절): 회수의 최소 진척과 묶음 안 시간 한도, 회수 대기 질의, 회수 없이 cycle 끝내기, 실시간 간격, 프로세스가 쓸 수 있는 프로세서 수, 요청의 스레드 계약, worker 상한. 개정 5(2026-11-19): 병렬 marker의 FIFO 미리 읽기 고리와 일반 그래프 표식 gate의 남은 차이(16절), 간선 제거의 장부 감소를 미루는 기록(13.4절 — 리뷰 반영: 기록 256칸(꼬리 지연), 적용 대상 검사를 모든 빌드 Fatal로, 세대에 더해 상태 검사), 5단계 판단: 지정 클러스터를 경계 비율 안전장치 밖에 두는 근거·해체된 패키지를 다시 묶지 않는 근거(13.5절), 장부 고리 칸 16비트 번호안 기각(13.9절). 개정 4(2026-11-18): Registry 규모 점검 반영(14절), 소스 파일 구성(15절), Slot hot·cold 분리(6절 GC-S3 "Slot hot·cold 분리", 4절 gate, 11절 결정 5). 개정 2(2026-11-17): 클러스터·영구 영역 상세 설계(13절), 열린 결정 4 확정, 열린 결정 6 해소, UE 클러스터 사실 정정. 개정 3(2026-11-18): 장부 배치·클러스터 표식 경로를 측정에 맞춰 바꾸고 패키지 클러스터의 경계 비율 안전장치를 더함(13.4·13.5·13.9절).

**이 문서의 작업은 반드시 해야 하는 작업이다.** 현재 `ObjectRegistry` GC는 AAA급 규모(살아 있는 객체 100만 개 이상)의 게임에서 쓸 수 없는 상태다(2026-11-16 사용자 판단). 이 문서의 GC-S 단계는 선택 최적화가 아니며 성능 미달을 "측정 결과로 관리"하는 대상도 아니다. 단계를 모두 마치고 4절의 성능 gate를 통과해야 GC를 대규모 콘텐츠에 쓸 수 있다고 본다.

착수 시점은 사용자가 정한다(2026-11-16 "작업은 나중에 할 것"). 이 문서는 고정 구현 순서(A0-B → A0-O → 리플렉션 B9 → Curve → A0-G → A1–A6 → 화면 마일스톤)를 바꾸지 않는다. 완료 시한은 열린 결정이다(11절).

상태 표기는 다음과 같다. **설계**는 이 문서가 정한 계약이고 코드는 없다. **제안**은 측정 전 수치이며 GC-S0 벤치로 확정한다. **결정 필요**는 사용자가 정할 항목이다. 이 표기는 문서를 처음 쓴 때(2026-11-16)의 것이다 — 그때는 문서 전체가 설계였다. 지금은 GC-S0–GC-S7을 구현하고 쟀으며 현재 상태는 0절에 모았다.

참조 기준은 Unreal Engine 5.8.4-release(commit `f45729c0b6f0`, 로컬 클론 `C:\github\UnrealEngine`)로 고정한다. 약칭은 다음과 같다. 경로는 `Engine/Source/Runtime/CoreUObject/` 기준이다.

- GC = `Private/UObject/GarbageCollection.cpp`
- FRC = `Public/UObject/FastReferenceCollector.h`
- UOA = `Public/UObject/UObjectArray.h`
- FLG = `Private/UObject/GarbageCollectionInternalFlags.h`

UE 소스는 설계 사실의 대조에만 쓰고 코드를 옮기지 않는다.

## 0. 현재 상태 (2026-11-20, 9단계 마무리)

이 문서의 단계를 Phase 0–4와 그 뒤의 보강 단계들로 구현하고 쟀다. 기록은 [GC 대규모 처리 실험 결과](../Implementation/GcScaleExperiment.ko.md)다.

| 단계 | 상태 | 어디에 |
|---|---|---|
| GC-S0 측정 기준·장기 smoke | 구현 | 실험 1–4절(`GcBench`, `GcSoak`) |
| GC-S1 잠금 없는 marker·배치 visitor·prefetch, GC-S2 시간 기준 예산 | 구현 | 실험 5절 |
| GC-S3 GC 밀집 자료·루트 목록·비트 sweep·상한, Slot hot·cold 분리, M2 | 구현 | 실험 6·17절, 이 문서 14절 |
| GC-S4 병렬 표식 | 구현 | 실험 7·20–21절, 이 문서 16절 |
| GC-S5 수집 일정과 파괴 예산 | 구현 | 이 문서 17절, 실험 24절 |
| GC-S6 공용 할당자 | 구현. Sandbox 엔진 호스트에 연결(개정 15 — 18.10절) | 이 문서 18절, 실험 25절 |
| GC-S7 클러스터·영구 영역, M3 | 구현 | 이 문서 13절, 실험 13–15·22–23절 |
| M1·M4–M7 | 하지 않음 | 6절 끝 |

4절 gate의 현재 값은 4절 표의 마지막 열이다. 한 줄로: Begin·sweep 판정·증분 cycle·기본 상한·회수 예산은 충족, 클러스터가 맞는 그래프는 표식 gate를 모두 충족, 일반 그래프의 ×8 병렬 표식은 목표의 1.1–1.15배, 증분 Step p99는 재측정이 필요하다. 서문의 기준("단계를 모두 마치고 4절의 gate를 통과")에 비추면 단계는 M1·M4–M7을 빼고 마쳤고, gate는 이 미달·재측정 항목과 3M 시나리오가 남아 있다 — 전부 통과한 상태는 아니다.

8단계(GC-S6 공용 할당자)의 결과 — 계수 없는 플랫폼 할당을 기준으로 다시 잰 값이다(실험 25.8절. 앞 절들의 "플랫폼"에는 벤치의 계수 장치가 섞여 있었다):

| gate | 목표 | 실측 | 판정 |
|---|---|---|---|
| A1 `SystemAllocator`(1M 생성·회수) | 끝난다. 플랫폼 할당 대비 생성 2배·종료 회수 3배·churn 회수 4.5배 이내 | 생성 1.37–1.45배, 종료 회수 2.1–2.8배, churn 회수 3.8–4.3배(1,954 · 383 · 344 ns/객체) | 충족 |
| A2 `BinnedAllocator`(같은 측정) | 객체당 시간이 플랫폼 할당 이하 | 생성 0.97배, churn 생성 0.96배, churn 회수 0.81배, 종료 회수 0.75배(1,341 · 1,289 · 66.5 · 139 ns/객체) | 충족 |
| A3 할당·반환 쌍 | 40 ns 이하 | 7.2–7.4 ns | 충족 |
| A4 다른 스레드가 반환 | 같은 스레드의 3배 이내 | 2.2–2.6배 | 충족 |
| A5 8 스레드 처리량 | 1 스레드의 4배 이상 | round별 3.6–6.9배, 중앙 4.0배(다섯 round 가운데 둘이 4배 미만 — 다른 세션의 부하가 30% 안팎인 기계) | 경계 |
| A6 메모리 | 요청 byte 대비 +20% 이하 | +6.8%(churn 뒤 +9.7%) | 충족 |
| A7 GC churn 회수 | 플랫폼 할당보다 줄어든다 | 다섯 round 모두 낮다 — 중앙값 151.5 대 170.0 ns/객체(0.89배, round별 0.85–0.97배) | 충족 |

- 한 일: `SystemAllocator`의 원장을 주소 hash로 바꿔 반환이 O(1) 상환이 됐고(전에는 선형 검색이라 100K 이상이 끝나지 않았다), `BinnedAllocator`를 예약 주소 범위·64KB slab·크기 등급 32개·스레드 캐시·이전 칸·스레드 종료 정리 구조로 다시 만들었다. `platform::ThreadExitSlot`과 접근을 열어 둔 예약(`VirtualMemoryAccess::Open`)을 PlatformBase에 더했다.
- 반환 표지는 비교 뒤 단순 대입이고 블록을 내줄 때 표지를 검사한다(개정 13 — 비교 교환이 GC churn 회수에서 객체당 6–12 ns였다).

남은 한계(빠짐없이):

- **제품 연결(개정 15·16 — 18.10·18.11절).** 실행 진입점(Launch)이 `SystemAllocator` 위에 `BinnedAllocator`를 하나 두고 범용 할당자(`ApplicationContext::DefaultAllocator()`)로 내준다. Sandbox 엔진 호스트, 게임 템플릿, Commandlet(설정 문맥·`trace-report`·`log` 명령)이 그것을 쓴다. 벤치 도구는 바꾸지 않는다(결정 — 측정 도구는 할당자를 일부러 고른다). 큰 블록(8 KB 초과)은 원장을 두 번 지난다(`BinnedAllocator`의 큰 할당 원장과 상위 `SystemAllocator`의 원장).
- **엔진이 Trim을 부르는 때(개정 16 — 18.11.6절).** 전체 수집의 회수가 끝난 프레임과 World를 파괴한 뒤, 둘뿐이다(`WorldGcSchedule::trimMemory`로 끈다). 기본 방식(증분)에서는 전체 수집을 요청하거나 World를 파괴하기 전까지 부르지 않는다 — 그 사이에는 빈 slab이 보관 한도(4 MiB)를 넘는 만큼만 그때그때 풀린다. 호스트가 `Allocator::TrimMemory()`를 직접 부를 수 있다. 메모리 압박 신호에 따른 Trim은 없다(신호를 내는 플랫폼 계층이 없다).
- **ASan은 로컬에서 돌리지 않았다.** `BinnedAllocator`의 수동 poison 경로는 Windows CI의 AddressSanitizer 단계(run 44656731971, dd157ca6 — Asan 구성 Foundation)에서 처음 실행돼 통과했다(메인 세션 확인: 2,659개 가운데 실패 0 — `CoreBinned` 27·`GcSoak` 9·`ThreadExitSlot` 4 포함. 그 단계 자체는 다른 세션의 크래시 시험 둘로 실패했다). macOS에서는 그 단계가 아직 돌지 못했다(앞 단계의 다른 세션 실패). 그 앞의 efd940ac에서는 같은 단계가 `TryMarkReturned`의 비교 교환을 use-after-poison으로 잡았다 — 중복 반환 경쟁 시험에서 한 스레드가 블록을 닫은 뒤 다른 스레드가 계측되는 원자 연산으로 읽었다. 그 코드와 시험은 개정 13에서 없어졌다. 개정 15(AL6)의 코드는 그 단계에서 아직 돌지 않았다.
- **쉬는 스레드의 캐시(개정 16 — 18.11.4절).** `TrimMemory`는 호출 스레드의 캐시와, 쉰다고 표시한(`MarkThreadIdle`) 스레드의 캐시를 비운다. 표시하는 것은 `WorkerScheduler`의 worker뿐이다(작업이 없어 막히는 대기 앞뒤). 표시하지 않고 쉬는 스레드(로그 출력·비동기 파일·trace 등)와 Trim 때 깨어 있던 스레드는 스레드 × 인스턴스당 최대 2,361,856 byte(`BinnedAllocator::MaxThreadCacheBytes`)를 쥔 채로 있고, 그 블록이 붙드는 commit은 가장 나쁜 경우 195 MiB다(18.8절). UE의 epoch(Trim 때 깨어 있던 스레드가 다음에 쉴 때 스스로 비움)는 넣지 않았다(후속). 래퍼 allocator를 끼워도 표시와 Trim은 상위로 넘어간다.
- **스레드 종료 순서의 남는 경우**(18.4절). 할당자의 종료 hook이 지나간 뒤 같은 스레드의 뒤 순번 종료 정리가 allocator를 쓰는 경우와, 스레드의 첫 allocator 사용이 자기 종료 정리 안인 경우 — 기록 하나가 새고 그 캐시는 인스턴스 파괴 때 회수된다(해제된 메모리를 만지지는 않는다). fiber에서는 `ThreadExitSlot`을 쓰지 않는다.
- **겹친 중복 반환**(18.4절). 같은 블록을 두 스레드가 같은 순간에 반환하면 둘 다 성공으로 돌아올 수 있다. 그 블록을 두 번째로 내줄 때 Fatal로 잡지만, 그 사이에 다시 반환되거나 slab으로 돌아가면 놓치고 slab 계수가 어긋날 수 있다. 오용 검출은 그 slab의 decommit·재사용과 겹치면 보장되지 않는다(한계 2).
- **GC churn 회수의 이득은 작다**(gate A7). 일반 그래프의 churn 회수는 `BinnedAllocator`에서 플랫폼 할당의 0.89배다(개정 13 구현 — 비교 교환 구현에서는 같은 수준이었다). `SystemAllocator`에 견주면 0.37배다(실험 25.10절). 할당자만 바꿔 잰 객체 비교에서는 churn 회수 0.81배·종료 회수 0.75배이고, 큰 이득은 뜨거운 쌍·여러 스레드 쪽이다.
- **측정의 한계.** 측정은 다른 세션의 빌드 부하가 드나드는 기계에서 했다(판정에는 round의 최솟값·중앙값을 썼고, 마지막 재측정 때의 부하는 30% 안팎이었다). gate A5(8 스레드 배수)는 경계다. 실제 엔진의 객체 크기 분포는 재지 않았다(메모리 +6.8%는 벤치의 객체 크기에서의 값이다). 3M 객체 시나리오는 재지 않았다.
- **플랫폼.** 이 기계는 Windows만 빌드한다. macOS는 CI로, Android는 NDK 구문 검사로만 봤다.
- **검증의 한계.** origin을 충돌 없이 병합한 뒤에는 다시 검증하지 않는다(작업 규칙). 양쪽이 같은 고정 크기 등록부에 더하는 변경은 충돌 없이도 깨진다 — 시험 실행기의 자식 probe 등록부(8칸)에서 AL5의 probe와 다른 세션의 probe가 병합으로 만나 아홉이 됐고, EngineTests의 시험 넷이 dd157ca6부터 실패했다. AL6의 전체 실행에서 찾아 고쳤다(16칸). 같은 실행에서 엔진 단계 크래시 시험의 자식 하나가 120초 시한에 걸린 것을 한 번 봤다 — 재현되지 않았고 멈춘 자리를 가르지 못했다(실험 25.10절).
- **4절 gate 가운데 남은 것.** 일반 그래프의 STW 병렬 표식(×8 16.7–17.2ms — 목표 15ms의 1.1–1.15배), 증분 Step p99(부하 아래 판정 불가 — 재측정 필요), Slot 정보 합계(hot 64B는 충족, 전체 192.3B — 11절 결정 5).
- **리뷰의 후속 목록**(실험 25.9·25.10·25.11절): 겹친 중복 반환에서 캐시 보유와 slab 보유의 표지 값 나누기, 종료 hook의 전역 잠금 구간, `System` 종류의 남은 할당 검사 자리, 주소 범위 예약을 첫 호출 때 만들기, `MaxThreadExitSlots` 초과 시험, 예약이 거부되는 환경에 기대는 단언(`smallInSystem=0`·`fallbackAllocations == 0`), UE식 epoch와 named thread broadcast, 표시하지 않는 엔진 스레드의 캐시, `TrimMemory`가 전역 등록부 잠금을 쥐는 시간(worker 수에 비례), World 여러 개를 한 번에 파괴할 때의 Trim 묶기, 쉬는 캐시의 스레드 종료·인스턴스 파괴·동시 `TrimMemory`의 시험 등.

## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 작업의 지위 | 필수. 선택 최적화가 아니다 | 서문, 3절 |
| 규모 책임 | 객체 규모는 엔진이 감당한다. 사용자에게 "객체를 적게 만들라"를 GC 문제의 1차 해법으로 요구하지 않는다 | 2절 |
| 성능 gate | 1M 객체 STW 병렬 표식 ≤ 15ms 등 4절의 수치(제안). GC-S0 벤치로 확정하고 이후 회귀 gate로 유지한다 | 4절 |
| 표식 동기화 | 표식에 Registry `gate_`를 쓰지 않는다. 표식은 atomic(바이트 교환 또는 bit `fetch_or`)으로 하고 이긴 쪽만 큐에 넣는다 | 3.2절, 5절, 계획 §6.2 2번 |
| GC 자료 배치 | GC가 읽는 값을 Slot에서 떼어 slot index로 접근하는 밀집 배열(SoA)로 둔다. Slot의 나머지는 그대로 둔다 | 3.2절 |
| 루트 | 루트 목록을 증분 관리한다. Begin은 O(루트 수)다 | UE `GRoots`, 3.2절 |
| sweep | live·mark bitmap의 word 연산으로 white 후보만 찾는다. Retire 순서는 슬롯 순서를 유지한다 | 3.2절, 7절 |
| 병렬 | worker별 큐와 512칸 블록, 블록 단위 훔치기. World가 scheduler를 넘긴다 | UE `FWorkBlock`, 3.3절 |
| 예산 | 증분 Step 예산의 기본을 시간으로 바꾼다. `maxObjects`는 상한 장치로만 남긴다 | 3.2절 7번 |
| 수집 일정 | STW 병렬 전체 수집을 정식 경로로 둔다. 증분은 선택지로 유지한다. 파괴·회수는 시간 예산으로 여러 프레임에 나눈다 | UE 기본값, 5절 |
| 할당자 | O(1) 소유권 판정, 스레드별 캐시, 크기별 slab을 갖춘 할당자를 Core Memory 공용으로 둔다. ObjectRuntime 전용 구현은 두지 않는다 | 3.2절 8번 |
| MetaCompiler 활용 | 생성 코드로 marker 전용 정적 특수화, 잎 타입 표시, 참조 위치 상수, 정적 schema를 낸다(M1–M7, 제안) | 6절 끝 "메타 컴파일러로 얻는 추가 여지" |
| 클러스터 | 패키지 정책 Registry의 패키지, World의 Level을 엔진이 자동으로 클러스터로 둔다. 모든 강한 간선 쓰기에서 장부를 정확히 갱신하므로 UE와 달리 런타임 변경으로 깨지지 않고 해체가 필요 없다 | 13절 |
| 영구 영역 | 특수 클러스터로 둔다. 영구 객체는 영구 객체만 가리킬 수 있다(결정 4 확정, 2026-11-17) | 13.7절 |
| 세대별 GC | 이 문서 범위 밖이다. 측정 전에는 보류한다. 클러스터·영구 영역과는 다른 기법이다 | UE도 쓰지 않는다 |

## 2. 원칙: 규모는 엔진의 책임

게임 엔진은 콘텐츠의 규모가 커져도 문제없이 구동되어야 한다. 객체 수를 줄여 GC를 피하는 것은 GC가 약한 엔진에서 사용자가 떠안는 회피책이다. 대표적인 예가 Unity다. Rocks는 이 부담을 사용자에게 넘기지 않는다.

- 객체 수 줄이기는 사용자의 선택적 최적화로만 다룬다. GC 비용을 이유로 사용자 설계를 강제하는 안내를 1차 처방으로 쓰지 않는다. 지금 [GC 안내](../Guides/GarbageCollectionGuide.ko.md) 7.2절의 "값 배열·덩어리 데이터로 둘 수 있는지 검토"는 GC-S 완료 뒤 고친다(12절).
- [계획](EngineImplementationPlan.ko.md) §5.1의 "매 정점·입자·physics body·render instance를 Object로 생성하지 않는다"는 이 원칙과 충돌하지 않는다. 그 항목은 원래 GC 대상이 될 이유가 없는 대량 데이터의 표현 규칙이다. Entity·Component·에셋 객체처럼 정체성과 수명이 필요한 객체는 많아도 된다.
- 정확성이 먼저다. 오수거를 허용해 시간을 맞추지 않는다(계획 §6.4). 기존 정지형 `Collect`를 결과 대조용 oracle로 유지한다.

## 3. 현재 상태와 진단

### 3.1 현재 구현의 경로

기준 코드는 feature/engine-runtime `b2e87d3`이다. 경로는 `Core/Source/Runtime/ObjectRuntime/` 기준이다.

표식 하나의 경로는 다음 순서로 진행된다.

1. 작업 큐에서 16B `ObjectHandle`을 꺼낸다.
2. `FindSlot`이 chunk 표를 거쳐 Slot을 찾고 세대를 검사한다.
3. `slot->type.Ops().trace`가 객체 본체를 읽는다.
4. 참조 배열 버퍼를 읽으며 간선마다 `TraceVisitor`의 함수 포인터를 호출한다.
5. `MarkEdge`가 `gate_`를 잡고 대상 Slot을 다시 찾아 `mark`를 확인하고 기록한다.

각 단계가 앞 단계 결과의 주소를 필요로 하므로 캐시 미스가 겹치지 않고 직렬로 일어난다.

### 3.2 원인 (영향이 큰 순서)

1. **간선마다 전역 잠금.** `MarkEdge`가 간선 하나마다 `gate_`(Windows SRWLOCK)를 잡는다(`Private/ObjectRegistryGc.cpp` 35행). `StepCollection`은 객체마다 `hasError()`와 큐 pop에서 잠금을 잡고 시계를 읽는다(295–341행). sweep은 Slot 하나마다 잠금을 잡는다(427–447행). 병렬 경로에서는 모든 worker가 이 잠금에서 직렬화된다.
2. **192B Slot에 GC 필드와 비GC 필드가 섞임.** `Slot`(`Private/ObjectRegistryInternal.h` 48–102행)은 세대·상태·object·type·roots·mark·outer와 함께 이름, 부모·자식 고리, 회수 목록 포인터, 각종 계수를 담는다. 크기는 192B(cache line 3개)이고 `mark`는 오프셋 104다. 1M Slot이면 183 MiB를 차지한다. UE는 같은 역할의 `FUObjectItem`을 약 24B의 밀집 배열로 둔다(UOA 41–60행).
3. **간선마다 간접 호출, 객체마다 비인라인 호출.** `TraceVisitor::Visit`는 간선마다 함수 포인터를 호출한다(`Public/Object/RuntimeTypeOps.h` 429–448행). `TypeLease::Ops()`(`Private/TypeLease.cpp` 284행)와 `FindSlot`(`Private/ObjectRegistry.cpp` 100–107행)은 다른 번역 단위에 있다.
4. **prefetch와 배치 없음.** Slot → 본체 → 참조 배열 → 대상 Slot으로 이어지는 미스 사슬을 앞당겨 읽지 않는다.
5. **루트 수집과 sweep의 전체 Slot 순회.** `BeginCollection`은 루트를 찾으려고 모든 Slot을 훑는다(140–150행). 이 순회는 Step 예산 밖이다. `Collect`의 루트 수집과 sweep도 전체 Slot을 훑는다(507–519, 541–549행).
6. **병렬 경로의 구조.** 객체 하나마다 `Function`을 할당·제출하고 64개마다 모두 기다린다(355–413행). `World::StepGarbageCollection`은 scheduler를 넘기지 않으므로 게임 경로는 언제나 직렬이다(`Engine/Source/Runtime/World/Private/World.cpp` 1651행).
7. **객체 수 기준 예산.** 기본 `GcBudget`은 Step당 작업 64개, soft 1ms다(`Public/Object/ObjectRegistryOptions.h` 26–29행). 1M 객체에서는 표식 약 15,600 Step과 sweep 약 16,400 Step이 필요하다. 60fps에서 cycle 하나가 약 9분 걸리고, 그동안 쓰레기가 회수되지 않는다.
8. **회수와 할당자.** `ReclaimEligible`은 객체마다 잠금을 두 번 잡는다(624–647행 — GC-S5에서 묶음으로, 17.1절). 샘플 프로그램의 `SystemAllocator`는 `Deallocate`에서 살아 있는 할당의 연결 목록을 잠금 아래에서 선형 탐색한다(`Core/Source/Runtime/Core/Private/Memory/SystemAllocator.cpp` 76–78행). `BinnedAllocator`도 전역 잠금과 원장 선형 검색을 쓰며 "성능 보장 없음"이라고 명시한다(`BinnedAllocator.h`).

### 3.3 계획과의 차이

[계획](EngineImplementationPlan.ko.md) §6.2는 이미 세 가지를 정했다: epoch bitmap의 test-and-set으로 중복 enqueue 억제, worker의 청크·회색 객체 블록 분담, 큰 참조 배열의 bounded batch(256개 등) 분할. 지금은 객체 하나 단위의 task 분담만 있다. 현재 구현은 정확성 기준 구현(계획 §1.1 "정확한 정지형 GC → … 병렬·점진화")에서 이 세 가지로 넘어가지 못한 상태다. 이 문서는 계획의 방향을 바꾸지 않는다. 계획이 정한 병렬·점진화 단계를 구체화하고 필수로 못박는다.

### 3.4 모델 측정 (2026-11-16, 리포 밖 독립 모델)

리포 코드를 고치지 않고 위 경로를 본뜬 독립 C++ 모델로 쟀다. 모델이 재현한 요소는 다음과 같다.

- Slot 192B와 필드 순서
- 16B handle 큐, 비인라인 `FindSlot`과 `Ops()`
- 간선마다 함수 포인터 호출과 SRWLOCK

측정 환경은 Windows 11, 논리 코어 20개, clang -O2다. 그래프는 무작위 간선 평균 4개, 256개 단위 Outer 묶음, 루트 약 4.9천 개다. 수치는 표식 시간의 중앙값(ms)이며 실행마다 ±20% 안팎으로 차이가 났다.

| 변형 | 1M · 본체 섞임 | 1M · 본체 순차 | 100K · 본체 순차 | 1M · 간선 8 |
|---|---:|---:|---:|---:|
| 현재 `Collect` | 721.5 | 645.9 | 57.3 | 939.6 |
| 현재 `StepCollection` 직렬(예산 무제한) | 772.2 | 747.7 | 61.5 | 1306.9 |
| 간선별 잠금만 제거 | 514.0 | 452.1 | 39.7 | 796.4 |
| Slot 유지 + 잠금 제거 + 배치 visitor + prefetch (GC-S1) | 199.2 | 173.2 | 15.7 | 347.6 |
| SoA + 배치 visitor + 3단 prefetch + bitmap (GC-S3) | 44.7 | 54.9 | 3.10 | 87.4 |
| 병렬 4스레드, 512칸 블록, atomic 표식 (GC-S4) | 22.8 | 21.1 | 1.74 | 35.0 |
| 병렬 16스레드 | 10.6 | 12.0 | 1.33 | 18.4 |

1M에서 현재 구조의 루트 수집과 sweep은 각각 7–10ms였다. 루트 목록과 bitmap을 쓰면 각각 0.05ms와 0.01–0.03ms였다.

512칸 블록 병렬 구조에서 간선마다 전역 잠금을 그대로 두면 1·4·16스레드가 각각 108·186·406ms로, **스레드를 늘릴수록 느려졌다**. 잠금을 atomic 표식으로 바꾸면 같은 실행에서 48.5(단일)·21.7·9.5ms였다. 이 비교는 위 표와 다른 실행이므로 16스레드 값(9.5와 10.6)이 다르다. task를 묶는 것만으로는 병렬 이득이 없고, 표식 동기화를 바꾸는 것이 전제다.

모델에는 한계가 있다. 합성 그래프이고 실제 리플렉션 Trace보다 객체당 일이 적으며, 회수와 장벽 비용은 재지 않았다. 따라서 비율과 순위를 근거로 쓰고, 절대값은 GC-S0의 실제 `ObjectRegistry` 벤치로 다시 잰다.

## 4. 성능 gate (제안 — 현재 값은 마지막 열)

기준 환경은 8코어 데스크톱 Release 빌드다. 살아 있는 객체 1M, 강한 간선 평균 4–8개, Outer 묶음이 있는 그래프를 쓴다. GC-S0이 이 수치를 확정한다. 확정 뒤에는 회귀 gate로 계속 잰다. 마지막 열은 2026-11-20의 값이다(실험 0.2절 — 1M · 간선 4, 논리 20코어 기계, 부하가 있는 상태의 측정이 섞여 있다). 할당자의 gate(A1–A7)는 18.8절이다.

| 항목 | 목표 | 비고 | 현재(2026-11-20) |
|---|---|---|---|
| STW 병렬 전체 표식 | ≤ 15ms | 모델 16스레드 10.6ms, 8스레드 13.2ms | 일반 그래프 ×8 16.7–17.2ms(미달 — 1.1–1.15배. ×16은 10.9–11.1ms), 클러스터 그래프 ×8 3.2–6.5ms(충족) |
| 단일 스레드 전체 표식 | ≤ 60ms | 병렬을 쓸 수 없는 환경의 하한. 모델 45ms | 일반 그래프 57.6–64.4ms(경계 — 부하 아래 87–110ms), 클러스터 그래프 7–20ms(충족) |
| Begin(루트 수집) | ≤ 1ms | O(루트 수) | 0.04–0.40ms(충족) |
| sweep 후보 판정 | ≤ 1ms | bitmap. Retire 처리 비용은 white 수에 비례하며 별도로 잰다 | 0.01–0.19ms(충족) |
| 증분 Step 한 번 | ≤ 2ms(p99) | GC-S의 통과 조건(gate)이다. 계획 §6.3의 mark slice 0.5ms·safepoint 1.0ms는 그보다 엄격한 soft 목표로 따로 기록하며 gate가 아니다 | p95 2.1ms. p99는 부하 아래라 판정하지 못했다(재측정 필요) |
| 증분 cycle 완료 | 1M에서 60프레임 이내 | 시간 예산이 2ms일 때 | 직렬 49–51 Step, 병렬 ×8 14–28 Step, 클러스터 그래프 3–5 Step(충족) |
| 대량 파괴(레벨 언로드 등) | 프레임당 시간 예산 안에서 나눠 처리 | UE 기본 2ms/프레임 | 회수를 미루고 프레임 2ms 예산으로 나눈다 — churn 98K가 12–16프레임(충족, 17절) |
| 객체당 GC·관리 메모리 | Slot 정보 합계 ≤ 64B | 지금 192B. 개정 4: hot 레코드 64B(GC·자주 쓰는 경로가 읽는 칸)는 충족, 슬롯 전체는 192.3B(hot 64 + cold 128) — 합계 목표의 재정의는 11절 결정 5 | hot 64B는 충족, 슬롯 전체는 192.3B |
| 기본 상한 | `maxSlots` ≥ 2,097,152 | UE `MaxUObjects` 기본 2M(`Private/UObject/UObjectBase.cpp` 1363행) | 2,097,152(충족) |
| 3M 객체 | 위 수치의 선형 비례 이내 | 대규모 회귀 시나리오 | 재지 않았다 |

## 5. UE 5.8 대조

1M 단위를 감당하는 UE의 구성이다. 굵은 줄번호는 이 문서 작성 때 직접 다시 확인했다. 나머지는 조사 때 읽은 위치다.

| 항목 | UE 5.8 | Rocks에서의 대응 |
|---|---|---|
| 표식 저장 | `FUObjectItem::FlagsAndRefCount` 상위 32비트, chunk(64K) 배열(UOA 41–60, 711) | GC-S3 밀집 배열 |
| 표식 초기화 | Reachable·MaybeUnreachable flag 맞바꾸기, O(1)(FLG 116–123) | epoch 증가로 이미 동등 |
| 표식 동기화 | 일반 read 뒤 `InterlockedAnd`, 이긴 스레드만 enqueue(FLG 99–109) | GC-S1(단일)·GC-S4(병렬) |
| 참조 방문 | 클래스별 schema(멤버 16비트 압축)를 `switch`로 해석, ARO만 함수 포인터(FRC 683–740) | GC-S1 배치 visitor, GC-S7 schema |
| 배치·prefetch | `TReferenceBatcher` 3단 큐(GC 1478–1481, 1758–1837), 객체 반복자 2·6·16칸 앞 prefetch(FRC 85–119) | GC-S1 |
| 병렬 | 최대 16 worker(**`Private/UObject/ReachabilityAnalysisState.h` 27**), 512칸 `FWorkBlock`(**FRC 127–136**), CAS work-stealing(GC 2264–2435) | GC-S4 |
| 루트 | `GRoots`와 dirty 큐로 증분 관리(GC 630–697, 4382–4419) | GC-S3 루트 목록 |
| 영구 객체 | DisregardForGC(UOA 1215–1217). cooked 빌드 전용, 시작 때 열린 창에서 만든 객체(`Launch/Private/LaunchEngineLoop.cpp` 3909–3917) | GC-S7, 13.7절 |
| 클러스터 | 루트 하나 + 멤버, 멤버 내부 간선은 따라가지 않음(GC 4295–4380, 3130–3158). 생성 때의 스냅샷이고 런타임 쓰기 추적은 없다(검증 경고 `GarbageCollectionVerification.cpp` 267, 오류 누적 시 386에서 Fatal). **에셋·액터 클러스터는 기본 꺼짐**(`gc.AssetClustreringEnabled=False`·`gc.ActorClusteringEnabled=False`, `Engine/Config/BaseEngine.ini` 1752–1755) | GC-S7, 13절 |
| 증분 표식 | `gc.AllowIncrementalReachability=0`, 실험 기능, 켜면 5ms(**GC 302–316**). 증분 반복은 work-stealing을 쓰지 않음(FRC 1175) | GC-S5에서 STW 병렬을 정식 경로로 |
| 미도달 수집 | item 배열 전체를 `ParallelFor`로 선형 스캔(GC 5378–5410) | GC-S3 bitmap |
| 파괴 | BeginDestroy·FinishDestroy를 프레임에 나눔, `gc.IncrementalGCTimePerFrame` 2ms(**`Runtime/Engine/Private/UnrealEngine.cpp` 1826**) | GC-S5 |
| 실행 주기 | `gc.TimeBetweenPurgingPendingKillObjects=61.1`(**`Engine/Config/BaseEngine.ini` 1749**), 레벨 전환 시점 | GC-S5 일정 정책 |

UE가 1M을 감당하는 이유는 세 가지가 함께 작동하기 때문이다. 표식 비용을 낮추는 기법, 표식 대상 수를 줄이는 기법, 자주 돌리지 않는 운영 정책이다. 세대별 GC는 쓰지 않는다. 대상 수 줄이기 가운데 기본으로 켜진 것은 DisregardForGC이고, 에셋·액터 클러스터는 기본 설정에서 꺼져 있다(개정 2 정정). 따라서 UE 기본 경로의 1M 처리는 주로 표식 비용·병렬·운영 정책에서 나온다.

## 6. 단계

단계 번호는 의존 순서다. GC-S2는 작아서 GC-S1과 같은 묶음으로 진행할 수 있다. 단계마다 동작 불변 이동 커밋과 성능 커밋을 나눈다. 수천 줄 규모의 이동은 별도 커밋으로 둔다.

### GC-S0 · 측정 기준

- `Programs/GcBench`를 `CompressionBench`와 같은 형태로 둔다. 실제 `ObjectRegistry`와 생성 타입을 쓴다.
- 시나리오는 100K·1M·3M 객체, 간선 분포(평균 4·8, 큰 `ObjectRefArray` 하나), Outer 묶음, 생성·파괴 섞기, 증분 Step 분포(p50·p95·p99·최대), 회수와 대량 파괴, 할당자별 비교다.
- 출력은 phase별 시간(Begin·표식·sweep·회수), 메모리 피크, Step 횟수다.
- 완료 기준: 4절 수치의 기준값을 기록하고 gate 수치를 확정한다.

### GC-S1 · 잠금 제거, 배치 visitor, prefetch

- 단일 marker의 `MarkEdge`에서 `gate_`를 뺀다. 오류 상태는 marker 지역 변수로 둔다. 전제 확인은 7.1절에 따른다.
- 지금의 scheduler 경로(`ObjectRegistryGc.cpp` 355–413행)는 같은 `MarkEdge`를 여러 worker에서 동시에 부른다. 잠금만 빼면 `gcWork_` push와 오류 상태가 경합한다. 그래서 GC-S4 전까지 scheduler 경로는 잠금을 쥐는 별도 edge 함수를 유지한다. 잠금 없는 함수는 직렬 marker 전용이다.
- `mark` 접근은 `Atomic`(relaxed)으로 바꿔 잠금 밖 접근이 데이터 경합이 되지 않게 한다.
- `TraceVisitor::Visit`를 인라인으로 지역 버퍼(예: handle 256개)에 쌓고 가득 차면 flush 콜백을 부르는 형태로 바꾼다. 사용자·생성 Trace 코드는 그대로 둔다. 같은 visitor를 쓰는 `ValidateStagedEdge`·`TallyEdge`는 flush 콜백에서 순서대로 처리해 기존 방문 순서와 집계 결과를 보존한다.
- 작업 큐 기준 16·8·4칸 앞의 Slot, 본체, 참조 배열을 prefetch한다. 거리는 GC-S0으로 조정한다.
- `StepCollection` 루프에서 객체마다 잡던 잠금과 시계 호출을 없앤다. 시계는 N개(예: 256)마다 확인한다.
- 완료 기준: 결과 집합이 oracle(`Collect` 기준 구현)과 같다. GC-S0 1M 표식이 모델 기준 약 3.5배 이상 빨라진다.

### GC-S2 · 시간 기준 예산

- `GcBudget`의 기본을 시간 한도로 바꾼다. `maxObjects`의 기본은 상한 장치 수준으로 크게 둔다. 하위호환은 고려하지 않는다.
- sweep은 Slot 하나가 아니라 bitmap word 하나(GC-S3 뒤) 또는 chunk 하나를 작업 단위로 센다.
- 완료 기준: 1M 증분 cycle이 4절 목표 안에 끝난다(GC-S3 전에는 중간 수치를 기록).

### GC-S3 · GC 필드 분리(SoA), 루트 목록, bitmap sweep, 상한

- GC가 읽는 값만 slot index로 접근하는 밀집 배열로 옮긴다.
  - hot 레코드 16B: object 포인터, 타입 index(trace 함수표), outer slot index
  - 표식: epoch 바이트 또는 mark bitmap
  - 상태 바이트, live bitmap
- 밀집 배열은 chunk처럼 페이지 단위로 늘려 기존 포인터를 옮기지 않는다. 부모·자식 고리(`Slot*`)와 cold 필드는 Slot에 남긴다.
- 작업 큐는 u32 slot index로 바꾼다(1M 기준 16 MiB → 4 MiB). GC 중 세대 검사는 Debug 검증으로만 한다. "강한 간선은 언제나 살아 있다"는 불변식을 Debug에서 확인한다.
- `roots`가 0↔1로 바뀔 때 루트 목록을 갱신한다. 바뀌는 곳은 `AddRoot`(`Private/ObjectRegistry.cpp` 158행 부근), Commit의 root 증가(`Private/ObjectRegistryCreate.cpp` 554행 부근), `detail::ReleaseRoot`(`Private/ObjectRegistry.cpp` 228–238행)이며 모두 `gate_` 아래다.
- 패키지 정책 Registry에서는 `VisitIdentityEdges`가 자식 고리(`firstChild`·`nextSibling`)를 따라간다(`Private/ObjectRegistryGc.cpp` 22–26행). 이 고리를 cold로 남기면 marker가 cold Slot을 계속 읽게 된다. 방법은 둘 중 하나다. hot 쪽에 자식 index 목록을 두거나, cold 접근을 허용하고 그 비용을 GC-S0으로 잰다. 이 단계에서 둘 중 하나로 정한다.
- sweep은 `live & ~mark`를 word 단위로 계산해 white 후보만 `RetireLocked`로 보낸다. 순서는 슬롯 순서(새 chunk부터)를 유지한다.
- `maxSlots` 기본값을 4절 상한으로 올린다. Slot 정보 합계를 4절 목표(≤ 64B)로 줄이는 방안(cold 필드의 이름·고리 정보 분리 포함)을 이 단계에서 정한다.

#### Slot hot·cold 분리 (개정 4, 2026-11-18 구현)

`Slot`을 캐시 줄 하나의 hot 레코드와 같은 chunk 안에 나란히 둔 cold 레코드 `SlotCold`로 나눈다. chunk는 `Slot slots[64]`와 `SlotCold colds[64]`를 갖고, `ObjectRegistry::ColdOf(slot)`가 `chunks_[index / 64]->colds[index % 64]`를 돌려준다(chunk 표 조회 하나, 추가 할당 없음).

| hot `Slot` (64 B, `alignas(64)`, `static_assert`) | 크기 |
|---|---|
| `generation`·`index` | 8 |
| `state`·`queue`(회수 목록 종류)·`indexed`·`linked` | 4 |
| `clusterInCount`(13.4절 장부 inCount) | 4 |
| `object` | 8 |
| `type`(TypeLease) | 16 |
| `roots`·`borrows`·`encodePins`·`rootPosition` | 16 |
| `mutations`(변경 표지) | 8 |

| cold `SlotCold` (128 B) | 크기 |
|---|---|
| `nextInList`·`prevInList`(빈 슬롯·회수 대기·파괴 요청 목록) | 16 |
| `storage` | 8 |
| `stableType` | 16 |
| `transaction`(생성 트랜잭션) | 8 |
| `outer`(ObjectHandle) | 16 |
| `outermost`·`parent`·`firstChild`·`lastChild`·`nextSibling`·`prevSibling`(Slot*) | 48 |
| `name`(NameRecord*) | 8 |
| `backendHolds`·`pathBytes`·`discarded`·`destroyListed` | 8 |

접근 경로별로 읽는 쪽:

| 경로 | hot | cold |
|---|---|---|
| handle 조회(`FindSlot` — 세대·상태), Borrow 얻기·놓기, 루트 보유·반환 | 예 | 아니오 |
| 참조 쓰기·컨테이너 편집(소유자·대상 검증, 소유자 저장소 범위, 변경 표지, encode 고정, 클러스터 inCount) | 예 | 생성 트랜잭션(staged 쓰기의 Reserved 소유자·대상)과 관찰자 Registry의 패키지 쌍(`outer`·`outermost`)만 |
| GC 표식(직렬·병렬) | 아니오(GC 밀집 자료만) | 패키지 정책의 자식 고리(`firstChild`·`nextSibling`)만 |
| sweep·Retire | 상태·회수 목록 종류 | 회수 목록 고리, 파괴 요청 목록 |
| 생성(Reserve·Commit), 회수(FinishReclaim), 이름 조회·Discard·부분 트리 순회, backend 보유 | 예 | 예 |

소유자 저장소 범위 검사(컨테이너·참조 칸이 소유자 객체 안에 있는지 — `SetEdge`·`CheckContainerOwnerLocked`·batch)는 cold의 `storage`가 아니라 hot의 `object`에서 시작 주소를 유도한다: 저장소 시작 = `object` − 타입의 Object 기반 위치(`TypeLease::ObjectOffset`). 기반 위치는 C++ 타입마다 고정이지만(단일 비가상 상속) 0이라는 보장은 없다 — `Object`는 가상 함수가 없어 파생이 가상 함수를 가지면 MSVC ABI는 vfptr를 맨 앞에 두고 `Object` 기반을 그 뒤에 놓는다(World의 Actor·Component). 그래서 Registry가 생성마다 `object − storage`를 타입 레코드에 기록하고(첫 생성이 정하고 이후는 같아야 한다 — 다르면 Fatal) 범위 검사가 그 값을 읽는다. 타입 레코드는 같은 검사가 이미 읽는 `Ops().size` 옆이라 줄이 늘지 않는다. 처음 분리(a1916228)는 이 검사가 cold를 읽어 위 표의 "참조 쓰기는 hot만"과 어긋났다(리뷰 지적 — 측정은 실험 19절). 버린 대안: (1) `MakeRuntimeTypeOps`에서 기반 위치를 상수로 계산 — 생성되지 않은 저장소에서 파생→기반 포인터 변환이 필요해 정의되지 않은 동작이고 수동 함수표는 값을 줄 수 없다. (2) `object`를 시작으로 보고 범위를 `size`로 — 기반 위치만큼 저장소 끝 뒤를 받아 준다. (3) hot에 저장소 포인터를 더함 — hot이 72B가 된다.

불변식: 두 레코드는 같은 슬롯 번호로 짝지어지고 chunk와 함께만 생기고 사라진다(chunk는 Registry 소멸 전까지 옮기지 않는다). 회수(`FinishReclaim`)는 cold를 빈 상태로 되돌린다. 재사용되는 빈 슬롯의 cold가 빈 상태라는 것을 Debug에서 생성 때 검사한다(아래 시험 보강).

측정(1M · 간선 4, 같은 실행 번갈아 세 round — 실험 17절): Slot 메모리 200.3 → 192.3 B/slot, 생성·연결·직렬·병렬 표식은 흔들림 안에서 같고, churn 회수가 230–254 → 256–262 ns/object로 5–10% 느려졌다(회수가 두 줄을 고친다).

검토하고 버린 대안: (1) hot에 cold 포인터를 두는 방식 — hot이 72B가 되어 줄 하나를 넘는다. (2) 자식 고리를 hot에 두는 방식 — 패키지 정책 표식만 쓰는 48B 때문에 모든 Registry의 hot이 두 줄이 된다. (3) cold를 별도 할당 배열로 — chunk 안에 두면 할당·수명이 chunk와 같아 잠금 안 할당이 늘지 않는다.

64B(합계)를 못 채운 이유와 다음 방안: hot만 64B이고 cold가 128B다. 합계를 더 줄이려면 cold의 Slot* 고리 8개를 슬롯 번호(4B)로(−32B), Outer handle을 hot 레코드(GC `GcHot::outer`)의 Outer 번호와 그 슬롯의 세대로(−16B), stableType을 타입 표 번호로(−12B), 생성 트랜잭션을 생성 중에만 쓰는 곁 표로(−8B), 이름·자식 고리를 이름 있는/Outer 있는 객체만 갖는 identity 레코드로 옮겨야 한다. 이 모두를 해도 슬롯 정보 합계는 약 80–100B다(이름과 Outer 고리가 Registry 안에 있는 한). 그래서 4절 "Slot 정보 합계 ≤ 64B"는 "GC·자주 쓰는 경로가 읽는 hot 레코드 ≤ 64B"로 바꾸고, 합계는 별도 목표(위 방안 적용 뒤 재측정)로 두기를 제안한다(11절 결정 5).

시험 보강(음성 대조 H1–H3에서 드러남):

- H1(모든 cold 조회를 이웃 슬롯의 cold로 — 한 chunk 안의 일관된 순열)은 어떤 시험도 실패하지 않았다. 순열이 일관되면 동작이 같으므로 결함이 아니다(cold 대응은 같은 함수 하나로만 정한다 — `ColdOf`).
- H2(회수가 cold Outer를 비우지 않음)·H3(cold 생성 트랜잭션을 비우지 않음)도 실패하지 않았다. 재사용 생성이 두 칸을 덮어써 지금은 결과가 같지만, 회수의 "cold를 빈 상태로" 계약이 깨져도 아무 검사가 없다는 뜻이다. 보강: Debug에서 빈 슬롯을 재사용할 때 cold 칸(목록 고리·storage·stableType·트랜잭션·Outer·고리·이름·backend 보유·경로 길이·플래그)이 모두 빈 값인지 검사하고 아니면 Fatal("Reused slot kept cold state from its previous object")로 멈춘다.
- 다른 chunk의 cold를 읽는 결함(H4)과 회수가 이름 기록 포인터를 남기는 결함(H5)은 실험 17절에 결과를 남긴다.
- 완료 기준: Begin·sweep 후보 판정이 각 1ms 이하이고, 단일 스레드 표식이 4절 목표를 만족한다.

### GC-S4 · 병렬 표식

- worker별 지역 큐와 512칸 블록을 쓰고 블록 단위로 훔친다. 첫 구현은 블록 풀 하나를 잠금으로 보호하는 단순한 방식으로 시작한다(모델이 이 방식으로 10.6ms였다). 그 뒤 CAS 기반 큐로 옮길지는 GC-S0 측정으로 정한다.
- 표식은 atomic 교환(바이트) 또는 `fetch_or`(bit)로 하고 이긴 쪽만 enqueue한다.
- `parallelTraceSafe`가 아닌 수동 Trace 타입은 별도 큐에 모아 소유 스레드에서 처리한다. 생성 타입은 이미 opt-in한다(`Programs/Rocks.MetaCompiler/CppEmitter.cs` 338행).
- 삽입 장벽은 `gcWork_` 직접 push 대신 장벽 전용 큐에 넣고 marker가 다음 slice 시작이나 종결 확인 때 소비한다. UE의 `GReachableObjects`에 해당한다.
- `World`가 `WorkerScheduler`를 넘긴다. 1·2·N worker 모두에서 진행과 종료를 검증한다(계획 §6.4).
- 지역 큐에서 꺼낸 항목은 처리 전에 참여자의 FIFO 미리 읽기 고리(16칸)를 거친다(개정 5, 16절). 지역 큐는 LIFO라 "몇 칸 아래" 항목은 곧 꺼낼 항목이 아니다 — 처리 중에 넣은 자식이 위에 쌓인다. 고리에 들어올 때 hot 레코드, 고리 중간에서 객체 본체, 앞쪽에서 참조 배열 버퍼를 미리 읽어 미리 읽기 거리를 큐 순서와 무관하게 고정한다(각각 15·9·5 trace 앞). 참여자 상태(`MarkParticipant`)는 캐시 줄 경계에 맞춘다(이웃 참여자와 줄을 나누지 않는다).
- 완료 기준: STW 병렬 전체 표식이 4절 목표를 만족한다. 결과 집합이 oracle과 같다.

### GC-S5 · 수집 일정과 파괴 예산

- STW 병렬 전체 수집을 정식 경로로 둔다. 레벨 전환·로딩 화면, 메모리 압박, 주기 실행(간격은 설정)에서 쓴다. 이 단계를 마치면 [계획](EngineImplementationPlan.ko.md) §6.1의 "점진 marking을 기본 구현으로 채택"은 선택 경로로 바뀐다. 계획 6.6절이 이 변경을 예고한다.
- (개정 6, 17절) 구현에서 정한 것: STW 병렬 전체 수집과 증분 수집은 둘 다 정식 경로다. World의 기본 일정은 증분이고(병렬 slice로 1M에서 프레임당 약 1ms — 실험 20절), STW 병렬 전체 수집은 요청(레벨 전환·로딩·메모리 압박)과 주기 일정에서 쓴다. 전체 수집의 회수는 프레임마다 시간 예산으로 나눈다.
- 증분 표식은 선택지로 유지한다. 증분 slice는 직렬 또는 소수 worker로 시작한다. 증분 반복의 work-stealing은 UE도 지원하지 않으므로 필요성을 측정한 뒤 정한다.
- 회수·파괴(BeginDestroy 대응 hook, 소멸자, 해제)는 시간 예산으로 여러 프레임에 나눈다. 회수는 N개씩 한 번의 잠금으로 떼어 내고 소멸자와 해제는 잠금 밖에서 묶어 처리한다.
- 완료 기준: 대량 파괴 시나리오에서 프레임당 예산을 지킨다. World 수명 정책과 시험을 새 일정에 맞춘다.

### GC-S6 · 할당자

- 필요한 성질은 세 가지다.
  - 원장 탐색 대신 주소 정렬 마스크로 페이지 헤더를 찾는 O(1) 소유권 판정
  - 스레드별 캐시와, 다른 스레드가 해제한 블록을 받는 atomic 목록
  - 같은 크기 객체를 모으는 크기별 slab
- 위치는 Core Memory 공용 모듈이다. ObjectRuntime 전용 할당자는 두지 않는다. 계획 C28의 slab/thread cache를 이 단계의 필수 항목으로 끌어올린다.
- 먼저 기존 `ExternalAllocator`와 jemalloc·TCMalloc 바인딩(`VendorAllocatorBackend.h`)으로 할당자 몫을 GC-S0에서 분리해 잰다.
- 완료 기준: 1M 생성·회수 시간이 GC-S0 기준에서 할당자 원인 비중을 제거한 수준이 된다. 수치는 GC-S0이 정한다.
- (개정 7) 상세 설계와 gate는 18절이다. 정한 것: 새 클래스를 더하지 않고 `SystemAllocator`·`BinnedAllocator`를 다시 만든다(같은 역할의 범용 할당자를 둘 두지 않는다). 할당자 몫은 외부 라이브러리 대신 반환이 무동작인 bump 할당자를 바닥으로 삼아 잰다(18.2절).
- (개정 13) 구현하고 쟀다(18절, 실험 25절). 완료 기준에 비추면: 1M 생성·회수가 두 할당자 모두 끝나고(전에는 100K 이상이 끝나지 않았다), `BinnedAllocator`는 네 항목 모두 플랫폼 할당 이하다. 다만 엔진에 넘어가는 것은 아직 `SystemAllocator`라 제품 경로의 할당자 몫은 그 배수(churn 회수 3.8–4.3배)다 — `BinnedAllocator`를 엔진·응용 프로그램에 연결하는 일은 후속 작업(결정 대기, 11절 8번)이다.
- (개정 15) 연결했다(18.10절): Sandbox 엔진 호스트가 Launch의 범용 할당자(기본 `BinnedAllocator`)를 엔진에 넘긴다.

### GC-S7 · 표식 대상 줄이기와 큰 Trace 분할

- **영구 영역.** 시작 때 만들어 끝까지 사는 객체를 표식 대상에서 뺀다(UE DisregardForGC에 해당). 영구 객체는 영구 객체만 가리킬 수 있다(결정 4 확정). 상세는 13.7절이다.
- **클러스터.** 함께 살고 함께 죽는 객체 묶음을 노드 하나로 표식한다. 패키지 정책 Registry의 패키지([ObjectFoundations](ObjectFoundations.ko.md) 1.7절)와 World의 Level이 클러스터다. 엔진이 생성 Commit 때 자동으로 가입시킨다. 사용자가 묶는 장치로 두지 않는다. 상세는 13절이다.
- **schema 기반 Trace.** MetaCompiler가 참조 멤버의 offset·종류 표를 생성하고 marker가 그 표를 해석한다. 큰 `ObjectRefArray`·`ObjectRefMap`은 블록 단위 작업으로 쪼갠다(계획 §6.2 2번의 "큰 참조 배열 bounded batch 분할"). 수동 Trace는 기존 함수 경로로 남긴다.
- 완료 기준: 큰 배열 하나(예: 1M 참조)가 Step 예산을 넘기지 않는다. 영구·클러스터 객체가 표식 비용에서 빠진다.

### 메타 컴파일러로 얻는 추가 여지 (제안)

UE는 리플렉션으로 만든 schema를 실행 중에 `switch`로 해석한다(FRC 683–740). Rocks에는 MetaCompiler가 있으므로 같은 정보로 **빌드 때 타입별 특수화 코드와 상수를 생성**할 수 있다. 현재 생성 Trace는 `static void Trace(const T&, TraceVisitor&)` 고정 시그니처라 이 여지를 쓰지 않는다(`Programs/Rocks.MetaCompiler/CppEmitter.cs` 255–262·339행). 아래 항목은 모두 **제안**이며 GC-S0으로 효과를 잰 뒤 채택한다. 붙는 단계를 괄호에 적었다.

| 번호 | 내용 | 기대 효과 |
|---|---|---|
| M1 (GC-S1) | **marker 전용 정적 특수화.** 생성 Trace를 visitor 타입에 대한 템플릿으로 내고, `RuntimeTypeOps`에 GC marker 전용 진입점을 따로 둔다. marker는 객체당 간접 호출 1회만 하고 간선당 간접 호출은 0회다. 필드 수가 고정이면 컴파일러가 펼친다. 관찰자 집계·Commit 검증은 기존 `TraceVisitor` 경로를 그대로 쓴다. 수동 Trace 타입은 배치 visitor 경로로 간다 | 간선당 호출 비용 제거. 공개 `TraceVisitor` 계약은 바꾸지 않는다 |
| M2 (GC-S3) | **잎 타입 표시.** 생성기는 강한 참조가 없는 타입을 이미 안다(빈 Trace를 낸다, 255행). 이를 타입 정보의 상수로 내보낸다. marker는 잎 객체를 표식만 하고 큐에 넣지 않으며, outer 간선만 hot 레코드에서 바로 따라간다 | 잎 객체의 큐 왕복·Slot 조회·trace 호출 제거. 효과는 잎 객체 비율에 비례 |
| M3 (GC-S1·S3) | **참조 위치 상수.** 생성기가 강한 참조 필드가 놓인 byte 범위와 컨테이너 버퍼 포인터의 offset을 상수로 낸다. prefetch가 객체 전체가 아니라 필요한 cache line과 컨테이너 버퍼를 정확히 가져온다 | 미스 사슬 단축. 큰 객체일수록 크다 |
| M4 (GC-S7) | **정적 schema.** offset·종류 표를 constexpr로 생성한다. M1의 특수화 코드와 같은 원천에서 만든다. 큰 `ObjectRefArray`·`ObjectRefMap`의 블록 분할 지점과 Step 예산 안에서 Trace를 끊었다 이어 가는 데 쓴다 | 거대 Trace 분할의 기반 |
| M5 (GC-S4) | **병렬 안전의 정적 판정.** 생성 타입은 이미 `parallelTraceSafe`를 opt-in한다(338행). 수동 Trace만 소유 스레드 큐로 보낸다 | 이미 있는 정보의 활용 |
| M6 (GC-S7) | **소유 관계에서 자동 클러스터.** "배타적으로 소유한 하위 객체"를 리플렉션 메타데이터로 표시하면 클러스터 경계를 빌드·로드 때 정할 수 있다. 지금 리플렉션에는 이 표시가 없으므로 새 메타데이터가 필요하다 | **개정 2에서 불필요.** 클러스터 경계를 패키지 소속과 World 소유 사슬이 이미 정하므로 새 메타데이터가 없어도 엔진이 클러스터를 만든다(13.3절) |
| M7 (GC-S7) | **정적 대상 타입에 따른 장벽·표식 생략.** 필드의 대상 타입이 영구 영역 전용 타입으로 정적으로 정해지면 장벽 shade와 표식 enqueue를 생략하는 경로를 생성한다 | Registry 단위 분리가 이미 있으므로 효과가 제한적일 수 있다. 측정 뒤 결정 |

한계는 다음과 같다.

- 손으로 쓴 `Trace`(`NativeClass`)는 이 혜택을 받지 못하고 배치 visitor 경로에 남는다.
- 컨테이너 길이는 실행 시에만 안다.
- 템플릿 인스턴스화로 코드 크기가 늘어난다.
- 생성 Trace의 시그니처가 바뀌므로 MetaCompiler 출력과 `MetaTests` 기대값을 함께 고친다. 하위호환은 고려하지 않는다.
- 3.4절 모델의 B1(인라인 배치 visitor)이 M1의 효과를 이미 상당 부분 근사한다. 그래서 M1의 추가 이득은 크지 않을 수 있다. 추가 이득은 M2·M3 쪽이 클 것으로 본다(해석, 측정 전).

## 7. 정확성과 불변식

### 7.1 잠금 제거의 전제

GC slice는 `processing_` 아래에서 실행되며, 그동안 생성·간선 변경·Discard는 `Busy`다. 이것이 marker가 `gate_` 없이 그래프와 Slot 배치를 읽어도 된다는 근거다. GC-S1 착수 때 아래를 모든 진입점에서 감사한다.

- **slice와 겹칠 수 있는 cross-thread 경로.** 다음 함수들은 접근 문맥 검사와 `processing_` 검사 없이 어느 스레드에서나 `gate_`를 잡는다. 이 경로들은 `mark`·`state`를 쓰지 않는다(`WakeDeferredLocked`는 `state`를 읽고 회수 목록 고리만 바꾼다 — `Private/ObjectRegistryGc.cpp` 173–178·264–269행). GC-S1에서 이를 시험으로 고정한다.
  - `ReleaseBackend`·`ReleaseEncodePin`(`Private/ObjectRegistry.cpp` 297–320행)
  - `detail::ReleaseRoot`(228–238행). `roots`와 통계를 바꾸므로 GC-S3 루트 목록과 직접 관련된다. slice 중 루트 해제를 루트 목록에 어떻게 반영할지(즉시 반영 또는 다음 Begin까지 지연)를 GC-S3에서 정한다.
  - `ReleaseBorrow`(285–295행)는 slice 시작 때 `totalBorrows_ == 0`을 요구하므로(`Private/ObjectRegistryGc.cpp` 300행) 실제로는 slice와 겹치지 않는다.
- **marker 밖에서 `mark`를 읽고 쓰는 곳.**
  - `ShadeLocked`(`Private/ObjectRegistryGc.cpp` 65–92행)
  - Commit의 shade(`Private/ObjectRegistryCreate.cpp` 548행 부근)
  - Begin·`Collect`의 epoch wrap 초기화
  - `IsCutoff`(59–63행)와 그것을 거치는 `CheckLive`. 이들은 `gcPhase_ == Sweeping`일 때만 `mark`를 읽는다.
  
  Sweeping 전환이 모든 marker 종료 뒤 `gate_` 아래에서 일어나는 한, 이 읽기는 marker의 쓰기와 겹치지 않는다. 장벽 shade와 marker는 둘 다 slice 밖과 안으로 나뉜다. 그래도 `mark`는 `Atomic`으로 두어 잠금 밖 접근을 정의된 동작으로 만든다. `CaptureDiagnostics`는 `mark`를 읽지 않는다.
- allocator 콜백 재진입은 `Busy`로 막혀 있다. 소멸자 콜백은 회수 단계에서만 실행되며 표식과 겹치지 않음을 확인한다.

### 7.2 결정성

- 표식 결과 집합은 순서와 무관하게 같다. 병렬 표식의 시험은 sleep이나 경쟁 운에 기대지 않고, 결과 집합·계수와 oracle 대조로 단언한다.
- Retire 순서는 sweep이 슬롯 순서(새 chunk부터)를 지켜 유지한다. 장벽 shading 순서나 `gcWork_` 순서에 기대는 기존 시험은 결과 집합 단언으로 바꾼다.
- 관찰자 집계(`TallyEdge`)는 배치 visitor의 flush가 방문 순서를 보존하므로 기존과 같은 결과를 낸다. GC-S1에서 시험으로 고정한다.

### 7.3 oracle

계획 §6.3에 따라 정지형 직렬 `Collect`를 oracle로 유지한다. 고정 그래프와 무작위 변경에서 live set 누락 0건, 보수적 잔존 객체가 무변경 full cycle 두 번 뒤 해제되는지를 매 단계 확인한다.

## 8. 현실적으로 어려운 부분

| 항목 | 내용 | 대응 |
|---|---|---|
| 잠금 제거의 전제 증명 | `gate_`가 표식 외의 cross-thread 경로도 보호한다. 감사 범위가 실제 비용이다 | 7.1절. GC-S1 첫 커밋을 감사와 Debug 검증으로 둔다 |
| 증분 장벽과 병렬 표식의 공존 | 장벽이 `gate_` 아래에서 `gcWork_`에 넣는다. UE도 증분 반복의 work-stealing을 지원하지 않는다 | 장벽 전용 큐(GC-S4). 증분은 직렬로 시작(GC-S5) |
| 중단할 수 없는 거대 Trace | 사용자 Trace 함수 하나가 1M 간선을 방문하면 쪼갤 수 없다 | schema 기반 Trace와 컨테이너 블록 분할(GC-S7). Trace 계약 변경이 따른다 |
| SoA 전환의 변경 면적 | Slot 필드 접근이 `ObjectRegistry*.cpp` 열 개 남짓에 퍼져 있다 | hot 필드만 분리하고 이동 커밋을 먼저 둔다 |
| 하드웨어 한계 | 표식은 메모리 지연에 묶인 작업이라 모델에서 8스레드 이후 확장이 둔화됐다. 3.4절 표의 실행에서 13.2 → 10.6ms, 잠금 비교 실행에서 13.4 → 9.5ms였다. 코어가 적은 기기에서는 병렬 효과가 작다 | 대상 수 줄이기(GC-S7)와 일정 정책(GC-S5)의 비중을 높인다 |
| handle 모델의 비용 | 간선이 16B handle이라 참조 배열이 포인터의 두 배다. 대신 본체를 읽지 않고 slot index를 얻는다 | GC 중 세대 검사 생략(GC-S3)과 Debug 불변식 검증 |
| 할당자 교체 | Core 공용 모듈 작업이라 다른 세션과 겹친다 | GC-S6을 Core Memory 작업으로 조율한다 |

## 9. 시험과 벤치

- 기존 GC 시험(`Tests/GcTests.cpp`, `IncrementalGcTests.cpp`, `IncrementalGcReviewTests.cpp`, `ObjectIdentityGcTests.cpp`)은 단계마다 전부 통과해야 한다. 순서 의존 단언은 7.2절 기준으로 바꾼다.
- 단계별 추가 시험은 다음과 같다.
  - oracle 대조(무작위 그래프 100K, seed 고정)
  - 잠금 밖 표식 중 cross-thread release 경합(명시 gate로 순서를 만들어 결정적으로 단언)
  - 1·2·N worker 종료
  - 장벽 큐 overflow
  - 큰 배열 분할
  - 영구·클러스터 경계
- 벤치는 GC-S0 시나리오를 단계마다 다시 돌려 4절 gate와 대조하고 결과를 구현 문서에 기록한다.
- Asan과 전체 체인은 단계 묶음 끝에 한 번 돌린다.

## 10. 범위 밖

- 세대별 GC. remembered set과 쓰기 장벽 비용이 따르고 UE도 쓰지 않는다. GC-S 완료 뒤 측정으로 필요성을 다시 본다.
- 완전 concurrent marker(mutator와 동시 표식). 계획 §6.1대로 P3 연구로 둔다.
- 이동·압축 GC. 계획의 주소 비이동 원칙을 유지한다.

## 11. 열린 결정

| 번호 | 항목 | 선택지 |
|---|---|---|
| 1 | 완료 시한 | 화면 마일스톤 뒤 대규모 콘텐츠 작업 전 / 특정 gate 전 / 사용자가 따로 정함. **결정 필요** |
| 2 | 표식 단위 | epoch 바이트(1M → 1 MiB, 단순) 또는 bit(128 KiB, `fetch_or`). GC-S0 측정으로 정한다 |
| 3 | STW 전체 수집 주기 | **정함(개정 6, 17.3절)**: World 기본은 증분 일정, STW 병렬 전체 수집은 요청(`World::RequestFullCollection`·`Engine::RequestFullCollection` — 레벨 전환·로딩·메모리 압박)과 주기 일정(`WorldGcMode::FullCollection`, 간격 기본 60초). 메모리 임계 신호는 플랫폼 계층이 생기면 엔진 요청에 잇는다 |
| 4 | 영구 객체가 일반 객체를 가리킬 때의 규칙 | **확정(2026-11-17): 금지.** 위반 쓰기·Commit은 `Unsupported`다. 일반 객체를 붙잡아야 하면 RootRef를 쓴다(13.7절) |
| 5 | Slot 정보 64B 목표의 달성 방법 | 개정 4: hot·cold 분리로 hot 64B 달성(6절 GC-S3). 합계(192B)는 cold 고리 번호화·Outer·identity 레코드 분리로 약 80–100B까지 가능 — 4절 gate를 "hot ≤ 64B"와 별도 합계 목표로 나누기를 제안 |
| 6 | 배타 소유 하위 객체의 표시 방식(M6) | **해소(개정 2).** 패키지·Level 소속으로 경계를 정하므로 표시가 필요 없다(13.3절) |
| 7 | slice 중 루트 해제의 루트 목록 반영 시점 | 즉시 반영 / 다음 Begin까지 지연. GC-S3에서 정한다 |
| 8 | `BinnedAllocator`를 엔진·응용 프로그램에 연결(개정 13) | **확정(개정 15, 2026-11-20 — 사용자 지시): 연결한다.** 소유는 실행 진입점(Launch)이고 Sandbox 엔진 호스트가 엔진에 넘긴다(18.10절). Commandlet·벤치 도구의 연결, `Trim` 시점, 다른 스레드 캐시의 `Trim`은 후속이다. **개정 16(18.11절)**: 게임 템플릿과 Commandlet을 연결했고 벤치 도구는 바꾸지 않기로 정했다. Trim 시점과 쉬는 스레드 캐시도 넣었다 |

## 12. 다른 문서 정정

- [EngineImplementationPlan](EngineImplementationPlan.ko.md) §6에 6.6절 "대규모 처리 필수 보강"을 두고 이 문서를 가리킨다. 1.1절 F1 행·1.1절의 C28 문장·4.1절의 "C28 이후 측정 기반 최적화안" 문장에 GC-S6 필수를 덧붙이고, §6.1에 6.6절 참조를 단다. 개정 1.10(33.15절)으로 이번에 반영한다.
- [Core·Object 입력 상세 계획](CoreObjectInputDetailedPlan.ko.md)의 C28은 "선택 R3"으로 남아 있다. 이 표는 `CoreObjectInputTasks.ko.csv`·`CoreObjectInputData.json`과 짝이므로 이번에는 고치지 않는다. GC-S6 착수 때 세 곳을 함께 필수로 바꾼다.
- [GC 안내](../Guides/GarbageCollectionGuide.ko.md) 7절과 8절은 GC-S 단계를 마칠 때마다 현재 동작과 수치로 고친다. 7.2절의 객체 수 줄이기 권고는 GC-S 완료 뒤 사용자 선택 최적화로 낮춘다.
- [증분 GC 구현 계약](../Implementation/IncrementalGarbageCollection.ko.md)은 GC-S1(잠금), GC-S2(예산), GC-S4(장벽 큐) 때 계약을 고친다.

## 13. 클러스터와 영구 영역 (GC-S7 상세, 개정 3)

상태: **설계**. 실험 브랜치(experiment/gc-scale) Phase 4에서 구현·측정한다([실험 결과](../Implementation/GcScaleExperiment.ko.md) 12절).

### 13.1 출발점과 사실 정정

- Rocks의 타입 정보는 Registry 객체가 아니다. `TypeRegistry`가 따로 소유하고 GC 대상이 아니다. 그래서 UE의 DisregardForGC에 들어가는 네이티브 클래스·CDO에 해당하는 몫이 Rocks에는 원래 없다. 지금 영구 영역에 들어갈 객체는 Engine Registry를 초기화할 때 만드는 객체(서브시스템)와 앞으로 생길 시작 콘텐츠다. 효과는 그 수에 비례하며 측정으로 기록한다.
- UE 5.8의 에셋·액터 클러스터는 기본 설정에서 꺼져 있고 cooked 빌드에서만 만들어진다(5절 표). UE 클러스터는 생성 때의 스냅샷이다. 실행 중 멤버가 클러스터 밖의 새 객체를 가리키게 되어도 장부를 고치지 않고, 검증 모드에서만 경고한다. 멤버나 참조 대상이 Garbage가 되면 클러스터를 해체한다.
- Rocks는 UE와 조건이 다르다. 세 가지를 쓸 수 있다.
  - 모든 강한 간선 쓰기가 Registry API(`SetEdge`·컨테이너 편집·Commit)를 지난다. 관찰자([ObjectFoundations](ObjectFoundations.ko.md) 3절)가 이미 이 지점에서 패키지 사이 간선을 센다.
  - 패키지 정책 Registry에서는 소속 객체가 Discard로만 죽고, 패키지가 살아 있으면 소속 객체가 모두 산다(1.7절).
  - World에서는 Level → Actor → Component 소유 사슬이 World API로만 바뀐다(`World.h` 소유·수명 규약).
- 그래서 Rocks 클러스터는 UE를 옮기지 않는다. 간선 쓰기마다 장부를 정확히 고쳐 런타임 변경에도 깨지지 않고, 해체 없이 개별 이탈을 처리하는 형태로 설계한다.

### 13.2 의미

- 클러스터는 루트 객체 하나와 소속 객체 집합이다. 루트도 소속이다.
- 소속은 **루트 → 소속 객체의 암묵 강한 간선**으로 정의한다. 패키지 고리 간선(1.7절)과 같은 뜻이다. 정지형 oracle `Collect`도 이 간선을 따라간다. 그러므로 STW 클러스터 표식의 결과 집합은 oracle과 정확히 같아야 한다(보수적 잔존을 허용하지 않는다). 증분 표식의 비교는 7.3절 기준(보수적 잔존은 무변경 full cycle 두 번 뒤 해제)을 따른다.
- 소속 간선을 따르는 oracle은 같은 소속 정보를 쓰므로, 소속이 실제 소유에서 어긋난 경우(고아 소속)는 잡지 못한다. 이것은 13.8절의 소속 불변식 검증이 잡는다.
- 표식은 소속 객체 사이의 간선을 따라가지 않는다. 루트가 살면 소속 전체가 살기 때문이다. 클러스터 하나의 표식 비용은 O(1 + 경계 소속 객체의 간선 수 + 이탈 대상 수)다.
- 가입은 생성 Commit 때만 한다. 이탈은 되돌릴 수 없다(재가입 없음). 해체는 루트의 RequestDestroy·Discard, Registry 종료, 그리고 sweep 직전 표식되지 않은 클러스터의 정리(13.5절, 버그 경로)에서만 일어난다.

### 13.3 누가 클러스터를 만드나

엔진이 자동으로 만든다. 사용자가 객체를 묶는 공개 장치는 두지 않는다.

| Registry | 클러스터 | 루트 | 소속 | 가입 | 이탈 |
|---|---|---|---|---|---|
| 패키지 정책(에셋) | 패키지 하나 | 패키지 객체 | outermost가 그 패키지인 객체 | 생성 Commit(자동) | Discard(부분 트리) |
| World | Level 하나 | Level | World API(`SpawnActor`·`AddComponent`)로 만든 Actor·Component | 그 생성 Commit(World가 지정) | `DestroyActor`·`DestroyComponent`의 RequestDestroy |
| 영구 영역을 연 Registry | 영구 영역(특수 클러스터) | 없음(언제나 생존) | 영구 창이 열린 동안 Commit한 객체 | Commit | 없음(Shutdown만) |

- 소속 조건은 `RuntimeTypeOps::completeFieldLayout`인 타입이다. 모든 강한 간선이 metadata에 있고 Registry API로 쓰인다는 선언이다. 이 조건을 못 맞추는 객체(수동 Trace, 리플렉션 base를 둔 native 파생)는 클러스터 밖 일반 객체로 남는다. 정확성은 그대로이고 최적화만 받지 못한다. 패키지 정책 Registry는 이미 이 조건을 강제한다.
- World는 Level을 만드는 트랜잭션에서 그 Level을 클러스터 루트로 지정하고, Spawn·AddComponent 트랜잭션에서 가입할 클러스터를 지정한다. 이 API는 World·AssetManager 같은 엔진 계층용이다(Discard와 같은 위치). 가입 지정은 루트가 Alive가 아니거나 이미 해체된 클러스터를 `StaleHandle`로 거부한다.
- World는 소속 객체가 실제 소유에서 빠지면 반드시 RequestDestroy해 이탈시킨다. 특히 Actor가 World API 밖에서 파괴(`Object::Destroy`)되어 DestroyRequested가 된 뒤에도 그 Actor의 Component를 빠짐없이 RequestDestroy해야 한다. 지금 `DestroyActorNow`는 Component 목록을 Actor Borrow로 읽어 DestroyRequested Actor에서 0개로 보므로(`World.cpp` 203–226·1536행), World 레코드가 Actor별 Component handle을 따로 보관하게 고친다. 고치지 않으면 그 Component가 Level이 끝날 때까지 남는 누수(고아 소속)가 된다.
- World는 RequestDestroy의 결과를 확인한다. 이탈은 실패할 수 있으므로(아래 이탈 절차) 실패를 버리면 고아 소속이 남는다.
- M6(배타 소유 표시 메타데이터)과 열린 결정 6은 필요 없어졌다.

### 13.4 장부

장부는 읽는 쪽에 맞춰 세 곳에 나눠 둔다(개정 3 — 배치별 측정은 [실험 결과](../Implementation/GcScaleExperiment.ko.md) 14절).

| 칸 | 자리 | 크기 | 뜻 | 읽는 쪽 |
|---|---|---|---|---|
| `clusterState` | 장부 칸(클러스터를 켠 Registry만, GC 페이지와 함께 할당) | u32 | 소속 클러스터, 또는 "클러스터 C에서 이탈한 대상(detached)", 또는 없음 | 이탈·해체, 소속이 아닌 대상의 detached 판정, marker(섞인 chunk만) |
| `inCount` | Slot(앞 칸의 채움 바이트 자리 — Slot 크기 그대로) | u32 | 소속이면 같은 클러스터 소속 객체에서 오는 간선 수. detached면 C 소속 객체에서 오는 간선 수. 공개 전 가입 지정이면 루트 slot | 참조 쓰기(추가는 대상 Slot을 검증으로 이미 읽었다) |
| `outCount` | 장부 칸 | u32 | 소속 객체의 간선 가운데 대상이 같은 클러스터의 소속도 detached도 아닌 간선 수 | 참조 쓰기 |
| 목록 고리 | 장부 칸 | u32 ×2 | 소속 목록 또는 detached 목록 | 이탈·해체 |
| 경계 고리 | 장부 칸 | u32 ×2 | `outCount > 0`인 소속 객체 목록 | 클러스터 처리 |
| chunk 클러스터 표 | `GcBits`(chunk 64칸의 비트 묶음, 64 B 한 캐시 줄) | chunk당 u64 ×2 + u32 ×2 | 이번 cycle에 클러스터 표식을 본 소속 비트(`clusterSeen`), chunk 안 클러스터 둘까지의 번호와 고름 비트. 셋 이상이면 넓은 표를 읽는다 | marker, 참조 쓰기의 간선 분류 |
| 넓은 chunk 클러스터 표 | `ClusterChunk`(클러스터를 켠 Registry만, chunk당 64 B) | 클러스터 번호 8개 + 소속마다 3비트 고름 | 작은 패키지(64칸 chunk에 클러스터 셋 이상). 8개를 넘으면 "섞임"(`clusterState`를 읽는다) | marker, 참조 쓰기의 간선 분류 |

장부 칸은 객체당 24B, chunk 표는 슬롯당 0.5B(비트 묶음이 32 → 64 B), 넓은 표는 클러스터 Registry에 슬롯당 1B다. 간선 분류는 소유자와 대상의 소속·클러스터 번호를 chunk 표(비트 묶음 배열은 1M 객체에 1MB라 캐시에 남는다)에서 얻고, 소속이 아닌 대상만 장부 칸의 `clusterState`로 detached인지 본다. 그래서 같은 클러스터 안 간선의 추가는 무작위 메모리 읽기가 없고, 제거는 기록에 12 B를 순차로 덧붙인다(대상 줄은 몰아서 적용할 때 미리 읽으며 고친다 — 아래). 클러스터 레코드는 루트 slot, 소속 수, 경계 목록 머리와 경계 소속 수, detached 목록 머리, 처리 epoch를 갖고, 표식 epoch는 클러스터 번호로 찾는 u32 배열에 따로 둔다(병렬 marker가 비교 교환한다).

**간선 제거의 장부 감소를 미룬다**(개정 5, 4단계 — 실험 22절). 클러스터를 켠 Registry에서 간선 제거가 끈 Registry보다 중앙값으로 약 110–140ns 느렸다(패키지 1M, 사이 간선 10·30%, 제거 352–389 ↔ 241–263ns). 추가는 대상 Slot을 검증이 이미 읽어 장부가 거의 공짜지만, 제거는 대상을 검증하지 않으므로 장부가 대상의 `inCount`·비트 묶음·장부 칸을 처음 읽기 때문이다. 그래서 제거의 장부 감소를 바로 하지 않고 Registry의 작은 기록에 (소유자 슬롯, 대상 슬롯, 대상 세대)를 덧붙이고, 계수가 정확해야 하는 때에 몰아서 적용한다.
- 기록: `LedgerEditLocked`의 지운 값(단일 간선·배열 원소의 바꾸기·제거, map 대입·지우기)과 `LedgerClearLocked`(비우기·원소 제거 — 배열 비우기, `StructRefArray` 원소 제거·비우기)가 덧붙인다(잠금 안, 순차 쓰기 12 B — 대상 줄을 건드리지 않는다). 256칸(`LedgerRemovalCapacity`)이 차면 먼저 적용한다(첫 클러스터 예약 때 3 KB를 받는다). 추가는 그대로 바로 센다 — 계수는 실제 간선 수 이상이 되어(미적용 감소만큼 크다) 아래 판단이 모두 보수 쪽이다. 바로 하는 제거는 이탈(`LeaveTrace`)의 몫 빼기와 map 전체 비우기(`ClearMapLedgerLocked` — 컨테이너 값 순회)뿐이다.
- 적용 시점(`ApplyLedgerRemovalsLocked`): 이탈 판정 전(`FinishLeaveLocked` — `inCount`로 detached 여부, `outCount`가 0인지 확인), 해체 전(`DissolveClusterLocked` — 미표식 해체·전체 해체 포함), 경계 비율 안전장치 판정 전(`FlattenBoundaryClustersLocked`), cycle 시작(`AdvanceGcEpochLocked` — Begin·Collect), 기록이 찼을 때. 적용은 기록 순서로 8칸 앞을 미리 읽으며 한다. Marking 중의 기록은 그 cycle의 cutoff(경계 비율 판정)에서, Idle·Sweeping·Reclaiming 중의 기록과 취소(`CancelCollection` — 적용하지 않는다) 뒤 남은 기록은 다음 cycle 시작에서 적용된다. 그래서 cycle 시작 적용은 정지형 Collect의 정확성(도달하지 않는 detached를 그 Collect에서 회수)에 필요하고, cutoff 적용은 Marking 중 지운 detached가 한 cycle만 더 사는 상한을 정한다.
- 분류가 기록 때와 같은 근거: 기록과 적용 사이에 소유자·대상의 클러스터 관계를 바꾸는 일은 이탈·해체뿐이고 둘 다 먼저 적용한다. 대상 상태 전이 가운데 detached → 없음은 계수가 0일 때만인데, 미적용 감소가 남은 대상은 계수가 0이 아니다. 기록한 대상은 적용 전에 회수될 수 없다: Marking 중의 기록은 cutoff(경계 비율 판정 전)에 적용되고, Sweeping 중의 기록이 가리키는 대상은 그 cycle에 표식됐다(지운 간선이 cutoff까지 있었다). 그래서 적용은 대상의 세대가 기록과 같고 상태가 Alive나 DestroyRequested인지 보고(장부를 고치는 쓰기는 live 쓰기뿐이라 대상은 기록 때 Alive였다), 아니면 그 불변식이 깨진 것으로 **모든 빌드에서 Fatal**이다. 상태도 보는 이유: 세대가 최대(`maxGeneration`)에 이른 슬롯은 회수가 세대를 올리지 않고 Exhausted로 두므로 세대 대조만으로는 회수를 놓친다. Release 대체 처리(회수된 대상을 클러스터 밖으로 보고 소유자 `outCount`만 줄이기)를 버린 이유: 불변식이 깨진 상태에서 대상이 실은 안쪽이었다면 소유자가 경계 고리에서 빠져 표식이 덜 되고 회수 후 사용으로 틀린다. 같은 장부의 다른 이상(계수 부족)도 Release에서 Fatal이다. 이 경로는 정상 API로 닿지 않아 시험이 아니라 검사로 막는다.
- 늦는 쪽이 안전한 근거: 미적용 감소는 계수를 크게 남길 뿐이라 detached가 목록에 더 남고(클러스터 처리가 표식해 한 cycle 더 산다), 경계 고리에 소유자가 더 남는다(클러스터 처리가 더 trace한다). 일찍 회수되는 일은 없다. 다음 cycle 시작에 반드시 적용되므로 영원히 남지 않는다. 삽입 장벽은 계수와 무관하다. 증분 Step 사이의 제거는 기록에 쌓였다가 기록이 차거나 다음 Begin에 적용된다(그 cycle의 부유 쓰레기가 조금 늘 수 있다).
- 기록 크기(꼬리 지연 — 실험 22.2절): 적용은 `gate_`를 쥔 채 칸마다 무작위 대상 줄을 고친다(칸당 약 35–100ns). 1M 무작위 대상에서 4096칸 적용은 140–350µs였다 — 기록을 채운 제거 하나가 그만큼 걸리고, 그동안 `gate_`를 기다리는 다른 스레드(루트 놓기·Borrow·쓰기)도 같다. 칸 수별 제거 최대 지연(패키지 1M 사이 30%, 같은 시점 번갈아 세 round, 쓰기 50K × 3): 256칸 13–25µs, 512칸 27–61µs, 1024칸 56–98µs, 4096칸 185–331µs. 묶음 중앙값 처리량은 흔들림 안에서 칸 수와 무관했다(칸 수가 바꾸는 것은 적용마다 미리 읽기 거리 8칸을 다시 채우는 비용뿐이다). 그래서 256칸으로 정했다. 제거 p99는 칸 수와 무관하게 0.7–1.2µs이고, p99.9는 256칸에서 적용이 들어 8.8–14.6µs(4096칸은 적용이 0.1%보다 드물어 1.0–1.8µs)다. 버린 대안: 적용 나눠 하기(기록이 차면 일부만 적용) — 이탈·해체·Begin은 기록을 모두 적용해야 하므로 꼬리는 결국 칸 수가 정한다. 칸 수를 줄이는 쪽이 단순하고 같은 효과다.
- 측정(실험 22절, 같은 시점 번갈아 네 round, 묶음 중앙값): 켬 제거 352–389 → **275–313ns**(적용 비용 포함 — 기록이 벤치 쓰기 묶음 50K보다 작아 묶음 안에서 적용된다), 끔 241–263ns. 같은 round 켬·끔 중앙 차이가 약 110–140ns에서 18–62ns(대개 20–30ns)로 줄었다. 부하가 큰 때의 확인 측정(커밋한 코드, 세 round)도 켬 제거 중앙 412–535 → 299–358ns로 순서가 같다. 추가는 바뀌지 않는다.
- 버린 대안: (1) 잠금 전 미리 읽기 — 제거 호출 안에서 미스와 겹칠 독립 작업이 수십 ns뿐이라 흔들림 안이었다. (2) `inCount`를 페이지별 밀집 배열(슬롯당 4 B)로 — 3–5%만 줄고 메모리가 는다. (3) 기록을 대상 순으로 정렬해 적용 — 넣지 않았다(측정하지 않은 판단: 4096칸이 1M 슬롯에 흩어져 같은 줄을 다시 쓰는 일이 드물고, 적용은 미리 읽기로 미스를 겹친다. 대상이 몰리는 그래프에서는 다시 볼 수 있다). (4) 추가도 미루기 — 추가는 대상 검증이 대상 Slot을 이미 읽어 미룰 이득이 없고, 계수가 실제보다 작아질 수 있어(감소가 먼저 적용되면) 안전 쪽 논증이 깨진다.

**간선 분류.** 소유자 X가 클러스터 C의 소속이고 대상이 t일 때 다음과 같이 센다. 소유자가 클러스터 밖이면 장부가 없다.

- t가 C의 소속이면 `inCount[t]`
- t가 C에서 이탈한 detached면 `inCount[t]`
- 그 밖이면 `outCount[X]`. 0 → 1이면 X를 경계 목록에 넣고, 1 → 0이면 뺀다.

outer 간선(생성 때 고정)도 같은 규칙으로 센다. 패키지 고리 간선과 소속 간선 자체는 세지 않는다.

**갱신 지점.** Commit(가입과 staged 간선 일괄), `SetEdge`·`ObjectRefArray`·`ObjectRefMap`·`StructRefArray`의 모든 편집(대입·추가·제거·비우기 각각), Discard·RequestDestroy(이탈), 루트의 Discard·RequestDestroy(해체), Shutdown이다. 위치는 관찰자 기록 지점과 겹치지만 **판정 기준은 관찰자와 따로 둔다.** 장부의 기준은 "소유자가 클러스터 소속인가"다. 관찰자는 Outer 있는 Alive 소유자와 패키지 사이 간선만 보고, 비우기 경로(`ClearArrayEdges`·`ClearMapEdges`·`RemoveStructElement`·`ClearStructElements`)는 관찰 대상이 아니면 내용을 보지 않고 떼어 낸다. 장부는 소속 소유자라면 이 경로에서도 지워지는 값을 모두 센다. 소속 소유자의 struct 원소를 지울 때는 원소 Trace(`RuntimeStructOps::trace`)가 반드시 있어야 하며, 얻지 못하면 `Unsupported`다.

- **이탈**(객체 B, 클러스터 C): 먼저 B의 Trace와 outer로 B가 C 장부에 남긴 몫을 뺀다(대상이 C 소속이거나 C의 detached면 `inCount[t]` 감소, 0이 된 detached는 목록에서 뺌, 그 밖이면 `outCount[B]`를 비우고 경계에서 뺌). 그다음 `inCount[B] > 0`이면 B를 C의 detached로 옮기고, 아니면 클러스터 없음으로 둔다. 비용은 O(B의 간선)이다. 부분 트리 Discard는 객체마다 차례로 이탈한다(순서와 무관하게 결과가 같다). 이탈 Trace는 `gate_` 밖에서 `processing_` 아래로 한다(Discard와 같은 방식). 수동 ops로 `completeFieldLayout`을 선언한 타입의 Trace를 잠금 안에서 부르지 않기 위해서다. 그래서 소속 객체의 RequestDestroy도 재진입 중에는 `Busy`가 될 수 있다.
- **칸 초기화**: 이탈해 클러스터 없음이 되거나 해체될 때 `inCount`·`outCount`·두 고리를 0으로 비운다. 슬롯이 재사용될 때 새 객체가 옛 값을 물려받지 않게 한다.
- **가입**: 새 객체만 가입한다. 공개 전 객체는 같은 트랜잭션의 staged 간선만 받으므로(Alive 소유자의 편집은 Reserved 대상을 거부한다 — `CheckTargetLocked`), 가입 때문에 분류가 바뀌는 기존 간선이 없다. 트랜잭션의 새 객체 소속을 모두 정한 뒤 staged 간선을 센다.
- **해체**(C): 소속과 detached의 `clusterState`와 장부 칸을 모두 비운다. 비용은 O(소속 + detached)다. 클러스터 레코드는 "해체됨"으로 표시하고, 큐에 남은 그 클러스터 항목은 꺼낼 때 이 표시를 보고 버린다. 클러스터 id는 루트 slot에서 얻으며, 같은 cycle 안에서는 루트 slot이 회수되지 않는다.
- **분류가 어긋나지 않는 근거**: 대상의 상태 전이는 네 가지뿐이다. 생성 → 소속, 소속 → detached(계수를 그대로 옮김), detached → 없음(계수가 0일 때만), 소속 → 없음(해체, 그 클러스터 장부는 버림). 어느 전이도 다른 클러스터 장부의 분류를 바꾸지 않는다. 다른 클러스터 D의 장부에서 C의 소속은 언제나 "그 밖"이기 때문이다.

### 13.5 표식과 sweep

- 객체 x에 닿으면: x가 클러스터 C의 소속이면 C를 atomic으로 표식하고 이긴 쪽이 C를 큐에 넣는다. detached이거나 클러스터 밖이면 지금처럼 개별 표식한다.
- 소속 판정과 클러스터 찾기는 mark 비트와 같은 캐시 줄에서 끝낸다(개정 3). `clustered` 비트가 켜졌으면 `clusterSeen` 비트를 본다. 켜졌으면(이번 cycle에 이 소속에서 이미 클러스터를 표식했다) 끝이다. 아니면 비트를 켜고 chunk 클러스터 표에서 번호를 얻어(섞인 chunk만 `clusterState`) 표식 epoch 배열을 비교한다. 소속당 cycle마다 한 번만 번호·epoch를 읽는다. 간선마다 장부 칸과 레코드를 읽던 개정 2 배치는 경계가 큰 클러스터에서 표식을 클러스터 없을 때보다 2–4배 느리게 했다(패키지 사이 간선 30–100%, 실험 14절).
- 클러스터 C 처리: 경계 목록의 소속 객체를 모두 **소속 trace 항목**(일반 표식 항목과 구분되는 큐 항목)으로 큐에 넣고, detached 목록의 객체를 개별 표식한다. 그다음 C를 "처리됨"(처리 epoch)으로 표시한다. 고리 안에 커서를 두고 slice를 넘기지 않는다. 고리는 slice 사이에 mutator가 바꾸기 때문이다. 넣는 비용은 경계 소속당 u32 하나다.
- 소속 trace 항목 처리: 그 객체의 Trace와 outer를 돌리되 같은 클러스터 소속 대상은 `clusterState` 비교로 건너뛴다. 꺼낼 때 그 객체가 더 이상 소속이 아니면(그 사이 이탈·해체) 일반 trace로 처리한다(보수적).
- 루트 목록에 소속 객체가 있으면 그 클러스터를 표식한다. 영구 영역은 Begin에서 표식된 상태로 시작하고 큐에 넣지 않는다(경계·detached가 없다).
- sweep 후보는 `live & ~mark & ~clustered`다(`clustered`는 소속 bitmap). 표식되지 않은 클러스터는 sweep 시작 전에 해체해 소속을 white 후보로 내보낸다. 패키지 RootRef를 Discard 없이 놓은 버그 경로이며 `retiredWithoutDiscard`는 지금처럼 센다.
- cutoff 판정(`IsCutoff`·`CheckLive`·`ValidateStagedEdge`)은 소속 객체를 클러스터 표식으로 판정한다.
- 병렬: 소속 trace 항목은 일반 블록처럼 나뉘어 worker 사이에서 훔쳐진다. `parallelTraceSafe`가 아닌 타입의 소속 trace 항목은 지금처럼 소유 스레드 큐로 보낸다.
- Sweeping 중 새로 만든 클러스터(루트 지정 Commit)는 표식된 상태로 시작한다. 아니면 그 소속이 cutoff로 판정된다.
- **경계 비율 안전장치**(개정 3): 패키지 클러스터의 경계 소속 수가 소속 수의 80%(`ClusterFlattenBoundaryPercent`)를 넘으면 cycle 끝에 해체한다. 증분은 cutoff에서 표식되지 않은 클러스터를 해체한 직후(남은 클러스터는 표식됐으므로 해체가 소속에 mark를 켠다), 정지형은 sweep 분류 뒤다. 그 패키지의 객체는 이후 일반 객체로 표식되고, 해체된 패키지(살아 있는 루트가 소속이 아니다)의 새 객체는 가입하지 않는다. 경계가 거의 전부인 클러스터는 소속을 거의 다 trace하면서 소속마다 클러스터 찾기를 더 치르므로 클러스터 없을 때보다 느리기 때문이다. 문턱은 측정으로 정했다(실험 14절).
  - 엔진이 지정한 클러스터(World Level)와 영구 영역은 대상이 아니다. 지정 클러스터를 빼는 근거는 측정이다(5단계, 실험 23절): World형 그래프(Level 10 × Actor 30K × (Actor + Component 2))에서 소속의 P%가 다른 Level의 Actor를 하나 더 가리키게 해 경계 비율을 P%로 올려도 켬이 끔보다 느리지 않았다. 일곱 round 직렬 표식이 P = 90%에서 켬 38.8–46.1 ↔ 끔 38.4–54.7ms(켬이 빠른 round 여섯), P = 100%에서 40.8–46.1 ↔ 38.2–53.8ms(다섯)였고, 병렬 ×8은 두 경우 모두 켬 10.9–14.5 ↔ 끔 13.1–15.8ms였다. 패키지형은 경계 88%에서 끔보다 6–8% 느렸다(실험 14.3절). 차이는 그래프 모양에서 온다고 본다(추정): World형 소속은 Level → Actor, Actor → Component 안쪽 간선을 늘 함께 가져, 경계 소속을 trace해도 그 안쪽 대상은 클러스터 표식 한 번으로 끝난다. 그래서 패키지 문턱(80%)을 지정 클러스터에 걸면 이득이 있는 Level을 해체한다. 해체하면 World의 다음 가입 지정이 `StaleHandle`이 되므로, 해체된 지정 루트로의 가입을 아무 일 없는 성공으로 바꾸는 장치(장부 칸 상태 비트)도 함께 필요했다 — 이득이 없어 넣지 않았다. 경계가 큰 Level을 만드는 그래프가 실제로 생기면 World가 `WorldDesc::levelClusters`로 끈다.
  - 해체된 패키지는 다시 묶지 않는다(5단계 판단, 실험 23절). 다시 묶으려면 패키지 객체를 모두 다시 가입시키고 간선을 다시 세어야 한다. 그 비용의 하한을 패키지 1M 생성에서 쟀다(클러스터 켬 − 끔, 두 round): 가입 +30–60ms, 장부 세기 +90ms(사이 10%)·+350–385ms(사이 100%)이고, 여기에 패키지 객체를 찾아 trace하는 몫이 더해진다. 1M당 수백 ms의 STW 일이라 Begin gate(1ms)를 넘고, 나누려면 증분 재가입이 따로 필요하다. 얻는 것은 경계가 실제로 줄어든 패키지의 표식 이득이다(사이 10%에서 1M당 cycle마다 약 19ms — 같은 측정의 켬 61.7–64.1 ↔ 끔 80.9–82.6ms). 그런데 해체된 패키지는 장부가 없어 경계가 줄었는지 알 수 없다. 알려면 해체된 패키지 객체의 간선 쓰기마다 패키지 사이 간선을 세야 하고, 이는 소유자의 outermost(cold Slot)를 읽는 비용을 쓰기마다 다시 더한다(4단계에서 줄인 것과 같은 종류의 무작위 읽기). 추측으로 주기적으로 다시 묶으면 실패할 때마다 재가입 비용을 버린다. 패키지를 다시 읽으면 새 패키지 객체가 새 클러스터를 만들므로(시험 `PackageBoundaryValve*`가 확인한다) 해체는 그 패키지의 이번 수명에만 적용된다.

### 13.6 증분 장벽

- 삽입 장벽(`ShadeLocked`)은 대상이 소속이면 그 클러스터를 shade한다.
- Marking 중 C가 이미 표식된 뒤 경계가 늘거나 detached 계수가 늘면, 새 간선의 대상은 삽입 장벽이 이미 shade했으므로 추가 조치가 없다.
- Marking 중 가입: 처리가 끝난 C에 새 소속 N이 들어오면 N을 shade해도 C에 닿아 아무 일도 하지 않는다. 그래서 N의 간선 대상이 모두 따로 shade되어야 한다. staged 간선의 Alive 대상은 쓰는 시점에 이미 shade된다(`ObjectRegistryEdges.cpp` 166행, 배열·map도 같다). **Marking 중 Create는 Alive outer도 shade한다**(지금은 Commit 뒤 trace의 outer 방문에 기대므로, 이 규칙을 새로 더한다). 같은 트랜잭션의 다른 새 객체는 Commit이 shade한다.
- Marking 중 이탈: C가 표식됐으면 B를 shade한다(보수적, 다음 cycle에 회수).
- Marking 중 해체: C가 표식됐으면 소속의 mark를 모두 켠다. **C가 아직 처리되지 않았으면**(장벽이나 slice 예산 때문에 큐에 남은 상태) 경계 소속을 소속 trace 항목으로 큐에 넣고 detached를 개별 shade한다. 해체 뒤 이 항목들은 일반 trace가 된다. 처리가 끝난 C는 밖으로 나가는 간선을 이미 따라갔으므로 mark만 켠다. 어느 경우든 안쪽 간선의 대상은 모두 mark가 켜진다.
- Sweeping 중 이탈·해체: 소속의 mark를 그 클러스터의 표식 상태로 맞춘다.

### 13.7 영구 영역

- 영구 영역은 특수 클러스터다. 언제나 표식된 상태이고 해체는 Shutdown 때만 한다.
- 들어가는 법: `RegistryOptions`의 영구 창 칸을 켠 Registry는 `ClosePermanentRegion(access)` 전까지 Commit되는 객체가 모두 영구다. 닫은 뒤에는 다시 열 수 없다. Engine은 Engine Registry 초기화(서브시스템 생성) 뒤 닫는다. World·에셋 Registry는 열지 않는다. 패키지 정책과 영구 창을 함께 켠 Registry 생성은 거부한다(Fatal, 정책과 같은 방식). 패키지 정책에서는 자식 고리가 간선이어서, 창을 닫은 뒤 만든 자식이 영구 → 일반 간선이 되기 때문이다. `ClosePermanentRegion`은 `processing_` 중에는 `Busy`다(Commit 검증과 공개 사이에 창 상태가 바뀌지 않게). 영구 창 안에서 만드는 객체는 `completeFieldLayout` 타입이어야 한다. 영구 객체는 trace하지 않으므로, Trace가 빠뜨린 필드가 있으면 쓰기 검사가 불완전해지고 회수 후 사용이 되기 때문이다.
- 규칙(결정 4 확정, 2026-11-17): 영구 객체의 강한 간선 대상(outer 포함)은 영구 객체뿐이다. 위반하는 쓰기·Commit은 Release에서도 `Unsupported`로 거부한다(비교 하나). 일반 객체를 붙잡아야 하면 RootRef를 쥔다. 영구 객체의 RequestDestroy·Discard도 `Unsupported`다.
- 표식과 sweep에서 비용이 0이다. 영구 객체를 가리키는 간선은 언제나 표식된 클러스터에 닿아 끝난다.

### 13.8 검증

- Debug 장부 검증(옵션, 비용 큼): Begin마다 모든 클러스터 장부를 처음부터 다시 계산해 비교한다. soak에서 켠다. 미룬 장부 감소(13.4절)는 적용하지 않고 기대 계수에 더해 대조한다 — 장부는 "실제 간선 수 + 미적용 감소"여야 한다. 기록도 적용 때와 같은 조건(대상 세대·상태, 소유자가 소속)을 본다. 검증이 적용하면 Debug에만 적용 시점이 하나 더 생겨 Release의 적용 경로(cycle 시작·cutoff)가 Debug 시험에서 가려진다(리뷰 반영 때 음성 대조 "cycle 시작에서 기록을 버림"이 이 때문에 잡히지 않았다).
- 소속 불변식(같은 검증에서): 명시 지정 클러스터(World Level)의 루트가 아닌 소속은 `inCount ≥ 1`이어야 한다. 실제 소유 간선(Level의 Actor 배열, Actor의 Component 배열)이 모두 같은 클러스터 안 간선이기 때문이다. 이 불변식이 고아 소속을 직접 잡는다. 패키지 클러스터는 고리가 소유이므로 이 검사를 하지 않는다.
- oracle(`Collect`, 소속 간선 포함)과 결과 집합이 같아야 한다. 무작위 그래프와 soak 모두에서 확인한다.
- 음성 대조: 이탈 때 `inCount` 이동을 빼거나, 이탈 shade를 빼거나, `outCount` 갱신 하나를 빼거나, 처리 전 해체에서 경계 항목 넣기를 빼거나, 비우기 경로의 장부 갱신을 빼면 검출되어야 한다.
- soak 시나리오: 패키지 생성·부분 Discard·전체 Discard, 패키지 사이 간선 편집, Level·Actor·Component 생성·파괴, 같은 Level 안 Actor 사이 참조, 증분 cycle 도중의 모든 변경. 특히 장벽으로 shade만 된 클러스터를 같은 slice 사이에 해체하는 경우, slice 예산 중간에 끊긴 클러스터, 처리된 클러스터에 가입하면서 밖의 outer를 가진 새 소속, World API 밖 Actor 파괴를 넣는다.
- (개정 4) 소속 소유자의 `StructRefArray` 원소 제거·비우기, 패키지 객체의 간선, 기존 패키지 아래 새 객체를 soak에 넣는다. 경계 비율 안전장치(13.5절)는 모델이 따로 예측한다: 모델은 장부와 같은 규칙(간선·outer·원소 참조, 대상이 같은 클러스터 소속이거나 그 클러스터의 detached면 안쪽)으로 패키지 클러스터마다 경계 소속 수를 세고, cycle 끝(cutoff 직후·정지형 수집 뒤)에 도달한 클러스터 가운데 문턱을 넘는 것을 해체 대상으로 고른다. 그 집합을 Registry 답(`InspectClusterMembership`)과, 그 수를 `GcReport::flattenedClusters`와 대조한다(어긋나면 실패). 처음 구현은 Registry 답을 그대로 따라갔다(리뷰 지적). 새 패키지의 소속 수는 2–10개로 고르게 둔다. 소속이 2–4개뿐이면 경계 비율이 50·67·75·100%처럼 띄엄띄엄이라 문턱을 80%에서 90%로 바꿔도 smoke가 구별하지 못했다(음성 대조 R3, 70%는 잡았다). 5/6 = 83%, 9/10 = 90% 같은 비율이 나와야 문턱 양쪽이 모두 시험된다. smoke의 seed는 섞은 뒤 xorshift 상태의 맨 아래 비트를 켠다(섞기 전에 `seed | 1`로 켜던 처음 구현은 짝수 seed와 다음 홀수 seed를 같은 실행으로 만들었다).
- (개정 4) 삽입 장벽 결정적 시험은 keeper를 먼저 trace한 뒤 keeper → 소속 간선을 건다(장벽을 끄면 실패해야 한다).

리뷰 기록: 개정 2 초안(b293feb)을 독립 리뷰 1회로 검토했다(2026-11-17). 차단 1건(처리 전 해체), 중요 4건(처리된 클러스터 가입의 outer, 관찰자와 다른 장부 기준, World 우회 파괴의 고아 소속, slice 중단·재개), 참고 2건을 이 절에 반영했다.

### 13.9 비용과 한계

- 간선 추가는 `gate_` 아래에서 계수 하나(같은 클러스터 안이면 대상 Slot의 `inCount`, 밖이면 소유자 장부 칸의 `outCount` — 경계 고리 출입이면 고리 이웃 칸까지)를 고친다(무할당). 간선 제거는 기록에 12 B를 순차로 덧붙이는 비용이다. 기록이 차거나, 기록이 쌓인 뒤 처음 이탈·해체·Begin(Collect 포함)·경계 비율 판정이 오면 최대 256칸을 `gate_` 아래에서 한꺼번에 적용한다(1M 무작위 대상에서 13–25µs — 13.4절).
- 클러스터 기능을 켠 Registry에서 객체당 25B(장부 칸 24B, 넓은 chunk 클러스터 표 1B)와 Registry마다 미룬 감소 기록 3 KB(256칸 × 12 B)가 늘어난다. chunk 클러스터 표(슬롯당 0.5B)는 모든 Registry에 있다.
- 경계 소속 비율이 높으면 효과가 줄어든다. 패키지 클러스터는 경계 80%를 넘으면 안전장치가 해체하고, 지정 클러스터는 그대로 둔다(World형은 경계 100%에서도 끔보다 느리지 않았다 — 13.5절).
- 장부 고리(목록·경계, u32 ×4 = 16B)를 페이지 안 16비트 번호로 줄이는 안은 버렸다(5단계 판단, 실험 23절). 클러스터 목록은 GC 페이지(4096칸)를 넘나든다 — World Level 하나가 소속 9만으로 페이지 22개 이상에 걸치고, 패키지도 빈 슬롯 재사용으로 흩어진다. 페이지 안 번호로는 페이지 사이를 잇지 못하므로 (클러스터, 페이지)마다 부분 고리와 클러스터별 페이지 목록이 필요하고, 가입·이탈·해체·경계 순회마다 그 목록을 찾는 일이 더해진다. 줄어드는 양은 슬롯당 8B(고리 16B → 8B)로, 측정한 1M 그래프의 GC 밀집 자료 46.4(패키지)·49.7(World) B/슬롯의 약 17%, 객체당 전체 메모리 442.9·400.4 B의 약 2%다. 영구 영역만 쓰는 Registry(Engine)는 칸이 거의 필요 없다(영구 소속은 경계·detached가 없고 목록은 Shutdown 해체에만 쓴다) — 칸 없는 영구 전용 배치는 Engine이 실제로 영구 객체를 만들 때(실험 13.11절: 지금 0개) 다시 본다.
- 소속 조건을 못 맞추는 타입은 혜택이 없다. World의 게임 Actor가 native 파생이면 클러스터 밖이다.
- 경계가 큰 클러스터는 처리할 때마다 경계 소속 수만큼 큐 항목을 넣는다.

### 13.10 UE 5.8과의 차이

| 항목 | UE 5.8 | Rocks |
|---|---|---|
| 기본 사용 | 에셋·액터 클러스터 꺼짐, cooked 전용 | 패키지 정책 Registry와 World에서 항상 |
| 경계 | 생성 때의 스냅샷과 MutableObjects. 런타임 변경은 검증 경고뿐 | 모든 강한 간선 쓰기에서 정확히 갱신 |
| 멤버 이탈 | 루트나 참조 대상이 Garbage가 되면 해체 | 개별 이탈 O(그 객체의 간선), 해체 불필요 |
| 생성 시점 | 로딩 PostLoad 뒤, Level Finalize | 생성 Commit |
| 영구 객체 → 일반 객체 | 따라가지 않고 검증 경고 | 쓰기 때 거부 |

## 14. Registry 규모 점검 반영 (개정 4, 2026-11-18)

연산·cycle마다 객체·Actor·타입 수에 비례하던 곳을 고친 구조다(측정과 그대로 둔 곳의 판단은 [실험 결과](../Implementation/GcScaleExperiment.ko.md) 16절, World·틱 쪽 계약은 [엔진 런타임](EngineRuntime.ko.md) 4.5·7.2·7.4·8절).

### 14.1 파괴 요청 목록 (`TakeDestroyRequests`)

- `RequestDestroy`·`Object::Destroy`·Discard가 객체를 DestroyRequested로 바꿀 때 그 슬롯을 Registry의 파괴 요청 목록(이중 연결 — cold의 `nextInList`·`prevInList`, `destroyListed`)에 넣는다. Retire가 목록에서 뺀다. 세 슬롯 목록(빈 슬롯·회수 대기·파괴 요청)은 상태로 배타적이라 같은 고리를 쓴다.
- `TakeDestroyRequests(access, Span<ObjectHandle> out)`가 지난 호출 뒤의 요청을 꺼낸다(할당 없음, 순서 정하지 않음, 0이 나올 때까지 되풀이). 소유 계층(World)이 자기 API 밖 파괴를 객체 수에 비례하는 훑기 없이 찾는다. 회수된 요청은 목록에서 빠지므로, 소유 계층은 회수가 일어날 수 있는 지점(GC step) 전마다 꺼내야 빠뜨리지 않는다(World는 매 프레임 step 전 — 엔진 런타임 7.4절).
- 버린 대안: World가 cycle마다 Actor를 모두 `InspectLifecycle`로 조회(Actor 수에 비례), 파괴 콜백(잠금 안 사용자 코드).
- 한계: 꺼내지 않으면 목록은 회수 때까지 남는다(메모리 증가 없음 — 슬롯 안 고리).

### 14.2 GC 타입 색인

- `AcquireGcType`(객체 생성마다)은 타입 키(`RuntimeTypeOps` 주소) → GC 타입 번호를 개방 주소 색인으로 찾는다(용량은 타입 표의 두 배 — 적재율 ≤ 1/2, 표가 커질 때 다시 만든다). 처음 구현은 타입 표를 처음부터 훑었다(생성마다 타입 수에 비례).

### 14.3 클러스터 소속 진단 조회 (`InspectClusterMembership`)

- 객체가 지금 클러스터 소속인지 돌려준다(진단·시험용, 접근 규칙은 `InspectLifecycle`과 같다). 클러스터 smoke 모델이 경계 비율 안전장치(13.5절)의 해체를 Registry 답과 대조하는 데 쓴다.

### 14.4 그대로 둔 선형 비용

- 클러스터 레코드 표 훑기(cutoff·정지형 sweep의 표식되지 않은 클러스터 해체와 안전장치): 100K 클러스터에서 약 0.1ms, cutoff slice 최댓값은 10K·100K 모두 1.04–1.10ms로 같았다. slice로 나누는 판(Sweeping의 레코드 커서와 클러스터 epoch로 cutoff 판정)을 만들어 쟀으나 차이가 없어 넣지 않았다.
- ProcessCluster·해체의 고리 순회(그 클러스터의 경계·소속 수에 비례 — 표식·해체할 양), `ObjectRefArray::IndexOf`·`RemoveReference(handle)`(API 의미), 이름 색인 rehash(분할 상환), 관찰자 `TakeChanges`(키 수), Debug 검증 재계산.
- 할당자(SystemAllocator·BinnedAllocator·PoolAllocator)의 살아 있는 할당 수에 비례하는 해제·할당은 GC-S6에서 다룬다.

## 15. 소스 파일 구성 (2026-11-18)

GC 구현(`Core/Source/Runtime/ObjectRuntime/Private/`)을 기능별 파일로 나눈다. 한 파일 약 2,000줄(`ObjectRegistryGc.cpp`)을 동작 불변 이동으로 나눴다. 3단계 일반 그래프 표식 개선이 표식 파일만 고치게 하려는 것이다.

| 파일 | 담당 | 새 기능을 넣는 곳 |
|---|---|---|
| `ObjectRegistryGc.cpp` | cycle 진행: `Collect`(정지형)·`BeginCollection`·`StepCollection`·`CancelCollection`의 단계 전환, cutoff 뒤 white 판정(`RetireWhiteLocked`), GC 보고 | GC 일정·단계·예산 정책(GC-S5) |
| `ObjectRegistryGcMark.cpp` | 표식의 공통·직렬 경로: 슬롯 표식(`MarkIndex`)·배치 간선(`MarkHandles`)·참조 위치 표(M3)·신원 간선·직렬 marker(`MarkSerial`)·삽입 장벽(`ShadeLocked`)·cutoff 판정 | 직렬 표식 개선(큐 구조·미리 읽기·M1) |
| `ObjectRegistryGcParallel.cpp` | 병렬 표식(`MarkParallel` — 참여자 지역 큐·공유 풀·소유 스레드 큐) | 병렬 표식 개선 |
| `ObjectRegistryGcData.cpp` | GC 밀집 자료: GC 페이지·비트 묶음·타입 표(`AcquireGcType`)·루트 목록과 성장(`PrepareGcGrowth`·`GrowGcLocked`) | 밀집 자료 배치·순서 |
| `ObjectRegistryGcReclaim.cpp` | sweep 뒤: `RetireLocked`, 회수 대상 목록(Pending·Deferred·Ready), 파괴 요청 목록(`TakeDestroyRequests`), 회수(`ReclaimEligible`·`DrainRetired`) | 회수 예산·잠금(GC-S5) |
| `ObjectRegistryGcInternal.h` | 위 파일들이 함께 쓰는 상수(시계 간격·미리 읽기 거리·할당 태그)와 작은 helper(표식 비트 읽기·쓰기, 로그) | GC 파일 사이 공용 helper |

클러스터 표식·해체(`ObjectRegistryClusters.cpp`)와 참조 쓰기·장벽 호출부(`ObjectRegistryEdges.cpp` 등)는 이미 따로 있다. 파일은 수백 줄 안쪽을 유지하고, 천 줄 가까이 커지면 기능 경계로 다시 나눈다(나눔은 동작 불변 이동 커밋).

## 16. 일반 그래프 표식 gate (3단계, 개정 5, 2026-11-19)

4절 gate 중 표식 둘(단일 스레드 ≤ 60ms, STW 병렬 ≤ 15ms)을 일반 그래프(1M · 간선 평균 4, 클러스터 없음 — GC-S0 시나리오)에서 다시 쟀다. 측정은 `GcBench --objects 1000000 --refs 4 --reps 9 --mark-only`(정지형 표식만, 최소·중앙)를 높은 우선순위로 설정마다 번갈아 다섯 round 돌렸다(후보 비교는 세 round — 실험 20·21절). 측정 동안 같은 기계에서 에뮬레이터·다른 세션 빌드가 CPU를 계속 써서 절대값은 부하 없는 때(실험 17절)보다 10–40% 높다. 판정은 같은 시점 번갈아 잰 상대값으로 한다. 측정 기계는 논리 코어 20개다. 4절 gate의 기준 환경은 8코어 데스크톱이라 **gate 판정은 ×8(참여자 9)로 한다.** ×16(참여자 17)은 이 기계에서만 의미 있는 참고값이다.

### 16.1 병렬 marker의 FIFO 미리 읽기 고리 (채택)

- 구조: 참여자(`MarkParticipant`)마다 16칸 고리를 둔다. `Pop`은 고리가 16칸이 될 때까지 지역 큐(블록 더미의 맨 위)에서 꺼내 고리 뒤에 넣고 그 항목의 hot 레코드를 미리 읽는다. 처리할 항목은 고리 앞에서 꺼낸다. 꺼낼 때 고리의 8번째 칸 항목의 객체 본체, 4번째 칸 항목의 참조 배열 버퍼를 미리 읽는다. 처리 순서로는 hot이 15, 본체가 9, 배열이 5 trace 앞이다(직렬 marker의 16·8·4칸과 거의 같은 단계). 고리 칸 수는 hot 미리 읽기 거리와 같게 묶는다(`static_assert`).
- 참여자 상태의 캐시 줄 정렬(개정 5 보강, 리뷰 지적): `MarkParticipant`(약 112 B)가 정렬 없이 배열에 놓여 참여자 i의 고리 머리·개수와 i+1의 지역 큐·계수가 한 줄에 들었다(참여자마다 다른 스레드가 매 객체 고친다 — false sharing). `alignas(CacheLineSize)`로 참여자마다 줄을 나눈다(128 B).
- 불변식: 고리의 항목은 그 참여자의 회색 객체다(표식 비트는 이미 켜졌다). 다른 참여자가 훔칠 수 없고(블록 단위 나눔은 지역 큐에서만 한다) 참여자는 고리와 지역 큐가 모두 빌 때만 idle이 된다 — 끝 판정(모두 idle이고 풀이 비면 끝)은 그대로 맞다. 예산·오류로 멈추면 고리의 항목을 지역 큐로 되돌린 뒤 남은 블록을 leftovers로 모은다(다음 slice가 이어 간다 — 회색 객체를 잃지 않는다).
- 측정(같은 시점 번갈아 다섯 round, 최소 ms — 실험 20.2·21절): 병렬 ×8 표식은 고리 전 34.5–36.8 → 고리(정렬 전) 20.4–21.0 → **고리 + 정렬 16.7–17.2**. ×16(참고값)은 19.8–20.9 → 13.1–13.9 → **10.9–11.1**. 직렬은 바뀌지 않는다. 병렬 증분 cycle(×8 기본 예산)은 고리로 44 → 28 Step, 42.7 → 26.9ms다(정렬 전 측정). 정렬한 상태에서 고리 8칸은 ×8 24.9–25.4로 덜 줄고, 24칸(17.4–18.3)은 16칸(17.6–18.0)과 같아 16칸을 유지한다.
- 버린 대안: (1) LIFO 위치 기준 미리 읽기(처음 구현 — 곧 꺼낼 항목을 겨누지 못해 효과가 작다). (2) 대상 비트 묶음 미리 읽기 단계(배열을 읽어 대상의 `GcBits`를 미리 읽음) — 직렬 표식이 5–25% 느려졌다(미리 읽기가 늘어 줄 채움 버퍼가 모자라는 것으로 본다). (3) 직렬 미리 읽기 거리 조정(hot·본체·배열 12–32·6–16·3–8칸) — 차이가 흔들림 안이다.

### 16.2 남은 차이와 원인

| gate | 목표 | 현재(부하 아래 최소) | 상태 |
|---|---|---|---|
| STW 병렬 표식(×8 판정) | ≤ 15ms | ×8 16.7–17.2ms(참고: ×16·참여자 17·논리 20코어 10.9–11.1ms) | 미달(약 1.1–1.15배) |
| 단일 스레드 표식 | ≤ 60ms | 부하가 줄어든 round 57.6–64.4ms, 부하 아래 87–110ms | 경계 — 부하 없을 때 목표 근처, 부하 아래 미달 |

- 단일 스레드 표식은 기계 부하에 가장 민감하다(같은 실행 파일이 round마다 57.9 ↔ 110.5ms). 부하가 줄어든 round의 최소는 목표 근처지만 여유가 없다. 원인(표본 프로파일, 직렬 표식 표본 기준): 참조 배열 원소 읽기 31%(`VisitStrongEdges` — 4칸 앞에서 미리 읽은 배열이 아직 오지 않음), 큐 앞쪽 미리 읽기 단계의 요구 읽기 29%(본체를 미리 읽으려면 hot을, 배열을 미리 읽으려면 본체를 읽어야 한다), 대상 비트 묶음 검사 23%(`MarkIndex`), 나머지 17%. 객체 하나에 무작위 주소 셋(hot 16 B·본체·배열 버퍼 — 1M에서 각각 16·46·81 MB)이 사슬로 걸려 있어, 미리 읽기 거리를 늘려도 줄 채움 버퍼 수가 동시 미스를 묶는다(16.1 (2)·(3)). 객체당 약 90ns로 DRAM 지연 하나와 비슷하다.
- 병렬 ×8의 원인: 같은 round의 직렬 최소 ÷ ×8 최소는 고리(정렬 전) 2.81–3.16배, 정렬 뒤 3.8–4.6배(직렬이 부하로 흔들린 한 round는 5.4배)다. 참여자 9로 선형의 약 1/3–1/2이다(앞 개정의 "4.4배"는 다른 조건의 직렬 값으로 나눈 잘못된 비였다). 확인된 원인 하나는 참여자 상태의 false sharing이었다(정렬로 ×8 약 18% 감소). 남은 원인은 객체당 무작위 주소 셋(직렬과 같다)과 표식 비트 묶음 줄(chunk 64칸마다 64 B)을 여러 참여자가 `fetch_or`로 고쳐 다른 참여자의 캐시가 무효화되는 것으로 본다(추정 — 이 기계에서는 성능 계수기를 쓸 수 없어 미스 종류를 직접 세지 못했다).
- 다음 방안(구현하지 않음, 측정 근거와 함께 남긴다): (1) 표식 비트를 읽기 전용 칸(live·leaf·clustered)과 분리해 쓰기 줄 경합을 줄인다 — 표식 비트 자체는 여전히 공유된다. (2) 그래프 배치: 생성 순서(=슬롯 순서)와 본체·배열 할당 주소를 가깝게(GC-S6 할당자 — size slab이 같은 묶음의 객체를 가까이 둔다). (3) 클러스터 그래프는 이미 두 gate를 충족한다(실험 14.4절 — 7–20ms·×8 3.2–6.5ms). 일반 그래프에서 gate를 지키려면 객체 그래프를 클러스터(Level·패키지)로 묶는 것이 효과가 크다.

## 17. 수집 일정과 파괴 예산 (GC-S5, 7단계, 개정 6, 2026-11-19)

6절 GC-S5의 구현 설계다. 측정은 [실험 결과](../Implementation/GcScaleExperiment.ko.md) 24절이다. 7단계 리뷰(ce760705..26524906)의 지적을 반영했다.

### 17.1 묶음 회수

- 회수(`ReclaimEligible`)는 `ReclaimBatchSize`(64)개씩 한 번의 잠금으로 회수 대상 목록에서 떼어 내고(Reclaiming 상태), 소멸자·저장소 반환·이름 기록 반환을 잠금 밖에서 차례로 부른 뒤, 슬롯 되돌리기(밀집 자료 비우기·세대 올리기·빈 슬롯 목록)를 다시 한 번의 잠금으로 한다. 객체마다 잠금 두 번이던 것이 묶음마다 두 번이 된다.
- 시간 한도는 소멸자 사이마다 본다. 한도가 지나면 아직 소멸하지 않은 work는 두 번째 잠금에서 Retired(풀린 목록)로 되돌린다(타입 lease·이름 기록을 슬롯에 되돌린다). 그래서 넘침은 묶음이 아니라 소멸자 하나다 — 묶음 전과 같다. 처음 리뷰 전 구현은 묶음마다만 보아 넘침이 소멸자 64개까지였다(리뷰 지적).
- 최소 진척: 시간 한도는 첫 객체를 회수한 뒤부터 본다. 회수할 수 있는 객체가 있으면 한 번의 호출이 적어도 하나를 회수한다 — 호출 직전에 선점되어 한도가 이미 지났어도 0으로 끝나지 않는다(리뷰 지적: 0을 "남은 것 없음"으로 읽는 쪽이 회수를 멈췄다). 객체 수 한도는 묶음 크기를 줄여 정확히 지킨다.
- 묶음 안 순서는 전과 같다(소멸자는 떼어 낸 순서). 차이는 앞 객체의 슬롯이 묶음 끝까지 Reclaiming으로 남는 것뿐이다 — 뒤 객체의 소멸자가 앞 객체 handle을 조회해도 전처럼 살아 있지 않은 객체(StaleHandle)다. 묶음 사이에는 잠금을 풀므로, 소멸자(또는 다른 스레드)가 지연 슬롯의 보유 계수를 반환하면 그 슬롯이 풀린 목록으로 옮겨져 다음 묶음에 회수된다.
- 측정(1M · 간선 4, churn 약 98K, 같은 시점 번갈아 세 round): 묶음 64의 회수는 24.2–24.4ms(객체당 246.8–249.3ns)로 같은 round 묶음 전 25.9–27.0ms보다 6–10% 적다. 묶음 16·256도 차이가 없었다 — 경합 없는 잠금은 싸고, 남은 몫은 소멸자·반환·슬롯 되돌리기의 메모리 접근이다(GC-S6 할당자 몫 포함). 다른 스레드가 `gate_`를 함께 쓸 때(루트 놓기·쓰기) 잠금 횟수가 64분의 1로 주는 것이 더 크다.
- 버린 대안: 목록 전체를 한 번에 떼어 내기 — 묶음 배열이 객체 수에 비례하고 되돌릴 work가 많아진다. 소멸자를 잠금 안에서 부르기 — 사용자 코드를 잠금 아래서 부르지 않는다는 규칙(7.1절)을 깬다. 묶음마다만 시간 보기(처음 구현) — 넘침이 묶음 크기만큼 커진다.

### 17.2 회수를 미루는 STW 수집과 예산 회수

- `Collect(access, scheduler, GcReclaim::Deferred)`는 표식·sweep 분류까지 하고 회수하지 않는다. 분류된 객체(Retired)는 회수 대상 목록에 남는다(이미 도달 불가이고 조회·간선 대상이 될 수 없다). 반환한 보고서의 `reclaimed`는 0, `retired`는 분류 수다. 이후 `DrainRetired`가 회수한 수와 시간은 마지막 보고서(`LastGcReport`)의 `reclaimed`·`reclaimNanoseconds`에 더한다 — 회수가 끝나면 마지막 보고서가 그 수집의 회수까지 보여 준다(리뷰 지적: 0으로만 남았다). 더하는 규칙: 예산 없는 `DrainRetired`도 더한다. 증분 cycle이 진행 중일 때(slice 사이) 부른 `DrainRetired`는 진행 중인 cycle이 아니라 앞 cycle의 보고서에 더해진다(진행 중인 cycle은 끝날 때 자기 보고서로 바꾼다). `durationNanoseconds`(수집 자체의 시간)는 바꾸지 않는다.
- `DrainRetired(access, GcBudget)`이 예산(객체 수·시간)만큼 회수하고 회수한 수를 돌려준다(17.1절의 최소 진척·소멸자 하나 넘침). 예산 없는 `DrainRetired(access)`는 전과 같이 모두 회수한다. 남은 회수 대상이 있어도 다음 `Collect`·`BeginCollection`은 할 수 있다(회수 대상 목록은 cycle과 무관하게 남고, 증분 cycle의 Reclaiming 단계·정지형 Collect의 회수가 함께 처리한다).
- `HasPendingReclaim()`은 회수할 수 있을지 모르는 객체가 남았는지를 O(1)로 답한다(풀린 목록·분류 전 목록이 비지 않았다 — 보유 중이라 지연된 목록은 세지 않는다. 보유가 풀리면 풀린 목록으로 옮겨지므로 다시 참이 된다). 반환값 0 대신 이 질의로 "회수가 남았는가"를 판정한다.
- `FinishCollectionDeferringReclaim(access)`는 Sweeping·Reclaiming 단계의 증분 cycle을 회수 없이 끝낸다 — 남은 sweep 분류는 모두 하고(분류는 chunk 비트 판정이라 1M에서 약 11ms — 실험 24.1절 churn의 sweep), 분류된 객체는 회수 대상 목록에 남긴 채 Idle로 둔다. Marking이면 InvalidArgument다(취소하거나 진행한다). safepoint 조건은 `StepCollection`과 같다: 처리 중이거나 Borrow·활성 생성 트랜잭션이 있으면 Busy다(Sweeping의 남은 분류가 Borrow가 걸린 white를 Retire하지 않게 — "Begin/Step은 Borrow=0·활성 생성=0 safepoint에서만" 계약, 메인 세션 검증 지적 N2). 전체 수집 요청이 진행 중인 cycle의 회수를 예산 없이 한 프레임에 하지 않게 한다(리뷰 지적 R1).
- 목적: STW 병렬 전체 수집에서 표식·sweep은 한 프레임에 끝내고(1M ×8 약 17ms + sweep), 소멸자·반환은 프레임마다 시간 예산으로 나눈다(UE `gc.IncrementalGCTimePerFrame` 2ms에 대응 — 5절 표). 1M churn 98K의 회수는 약 24ms라 한 프레임에 넣으면 표식과 합쳐 40ms를 넘는다.
- 버린 대안: STW 수집을 Begin + 무제한 Step 하나로 흉내 내기 — 증분 경로의 장벽·slice 비용(회색 객체 이월 등)을 치르고, 표식과 회수의 예산을 나눌 수 없다. 반환값 0으로 회수 끝을 판정하기(처음 구현) — 선점·보유 때문에 회수가 남았는데 멈춘다.

### 17.3 World 수집 일정

- `WorldDesc::gcSchedule`(`WorldGcSchedule`)이 정한다.
  - `mode`: `WorldGcMode::Incremental`(기본) — 지금처럼 World 파괴가 있으면 프레임 끝마다 `gcBudget`만큼 증분 slice를 진행한다. `WorldGcMode::FullCollection` — 마지막 전체 수집 뒤 `fullCollectionIntervalSeconds`(기본 60초 — UE `gc.TimeBetweenPurgingPendingKillObjects` 61.1초에 대응)가 지나면 프레임 끝에 STW 병렬 전체 수집을 한다.
  - `FullCollection`은 간격만 본다(World 파괴가 있었는지 보지 않는다). UE도 시간만 본다(`Runtime/Engine/Private/UnrealEngine.cpp` 2263–2289: 플레이를 시작한 World가 있으면 `TimeSinceLastPendingKillPurge`를 더하고 간격을 넘으면 수집 — 파괴 여부 조건이 없다). World 파괴 없이 생긴 쓰레기(참조를 지운 일반 객체, 루트 없이 만든 객체)도 다음 주기에 회수된다. 처음 구현은 World 파괴가 있을 때만 주기 수집을 해 그런 쓰레기가 다음 World 파괴나 요청까지 남았다(메인 세션 검증 지적 N3). 간격이 0 이하면 주기 수집을 하지 않는다(요청만 — UE의 `TimeBetweenPurgingPendingKillObjects > 0` 조건과 같다).
  - 간격은 `Tick`에 넘긴 실제 경과 시간(일시정지·시간 배율과 무관한 unscaled delta)의 합으로 센다. UE도 주기 수집 간격을 `FApp::GetDeltaTime()`으로 센다(`Runtime/Engine/Private/UnrealEngine.cpp` 2265). 처음 구현은 World 고정 스텝의 게임 시간이라 일시정지 중에는 수집이 멈췄다(리뷰 지적 R3).
  - `reclaimBudget`: 회수 대기가 있을 때 프레임마다 회수에 쓰는 예산이다(기본 시간 2ms).
- `World::RequestFullCollection()`은 모드와 무관하게 STW 병렬 전체 수집을 요청한다(레벨 전환·로딩 화면·메모리 압박). 요청은 다음 GC 단계(Tick 끝)에서 소비된다 — Tick 밖에서 부르면 다음 Tick 끝, Tick 안(hook)에서 부르면 그 Tick 끝이다. 진행 중인 증분 cycle이 Marking이면 취소하고, Sweeping·Reclaiming이면 `FinishCollectionDeferringReclaim`으로 회수 없이 끝낸 뒤 수집한다. 수집이 Busy(외부 Borrow — 진행 중인 cycle을 끝내는 단계의 Busy 포함)면 요청을 남겨 다음 GC 단계에서 다시 한다. 회수가 남은 채 전체 수집을 다시 해도(메모리 압박으로 요청이 연달아 올 때) 남은 회수는 이어진다 — 회수 대기는 표지가 아니라 질의로 판정한다(처음 구현은 새로 분류된 객체가 0이면 표지를 내려 남은 회수가 멈췄다, 메인 세션 검증 지적 N1).
- 회수: GC 단계마다 Registry가 Idle이고 `HasPendingReclaim()`이면 다른 GC 진행보다 먼저 `DrainRetired(reclaimBudget)`을 부른다. 반환값이 아니라 질의로 판정하므로, 선점으로 짧게 끝난 프레임이나 보유(BackendLease 등) 때문에 늦어진 객체도 보유가 풀린 뒤의 프레임에 회수된다(리뷰 지적 D1·R2). 증분 cycle이 진행 중이면 그 Reclaiming 단계가 함께 회수하므로 따로 부르지 않는다.
- 프레임당 GC 비용(전체 수집 뒤): 회수 `reclaimBudget`(기본 2ms) + 증분 일정이면 그 프레임에 새 cycle의 slice 하나(`gcBudget`, 기본 1ms)까지다. 전체 수집을 하는 프레임은 STW 수집(1M ×8 약 17ms + sweep)이 든다.
- 스레드 계약: `World::RequestFullCollection`은 어느 스레드에서나 부를 수 있다(메모리 압박 신호 등 — 요청 표지는 atomic이고 게임 스레드의 Tick이 소비한다). 그 밖의 World API는 지금처럼 소유(게임) 스레드 전용이다.
- 기본을 증분으로 둔 근거(측정, 실험 20·21절): 1M에서 병렬 ×8 STW 표식은 약 17ms로 한 프레임(16.7ms)을 넘고, 증분 ×8 기본 예산은 slice 약 1ms × 21번이다. 그래서 프레임마다 도는 일정은 증분이 기본이고, 멈춤을 감출 수 있는 순간(전환·로딩)과 메모리 압박에는 STW 병렬 수집을 쓴다. 주기 일정(UE 방식)은 `FullCollection` 모드로 고른다.
- 종료(`Shutdown`)는 지금처럼 진행 중인 cycle을 끝내고 전체 수집으로 모두 회수한다(회수를 미루지 않는다). 끝내는 Step 상한과 예산은 전체 수집 요청과 같은 상수(`FullCollectionFinishSteps`·`UnboundedFinishBudget`)다.
- (개정 16) `WorldGcSchedule::trimMemory`(기본 켬): 전체 수집의 회수가 다 끝난 GC 단계에 World가 자기 allocator의 `TrimMemory`를 한 번 부른다(회수할 것이 없었으면 수집한 프레임에 바로). 증분 cycle이 끝날 때는 부르지 않는다. 근거와 UE 대조는 18.11.6절이다.

### 17.4 엔진이 소유하는 GC scheduler

- `Engine`이 GC 표식용 `WorkerScheduler`를 만든다(초기화 단계 `GcScheduler` — 엔진 Registry 뒤, 서브시스템 앞). worker 수는 `EngineConfig::gcWorkers`이고, 기본(`GcWorkersAuto`)은 `platform::LogicalProcessorCount() − 1`을 8(`MaxAutoGcWorkers`)로 자른 값이다(gate를 ×8로 판정 — 4절, 실험 21절에서 ×16의 추가 이득이 작다). 0이면 만들지 않는다(직렬 표식). `MaxGcWorkers`(64 — `WorkerScheduler`의 상한)를 넘으면 초기화가 InvalidArgument로 실패한다(설정 실수를 조용히 자르지 않는다).
- `CreateWorld`는 `WorldCreateDesc::gcScheduler`가 없으면 엔진 scheduler를 넘긴다. 여러 World가 한 scheduler를 빌린다 — World Tick은 한 게임 스레드에서 차례로 돌므로 표식 slice가 겹치지 않는다. 엔진은 World를 모두 파괴한 뒤 scheduler를 끝낸다(종료 순서).
- `Engine::RequestFullCollection()`은 어느 스레드에서나 부를 수 있다. 엔진의 atomic 요청 표지만 세우고, 게임 스레드의 프레임(World Tick 전)이 그것을 소비해 살아 있는 World마다 `RequestFullCollection`을 건다(다른 스레드에서 World 배열을 돌지 않는다 — 리뷰 지적 R6).
- `Engine`의 `Initialize`·`Shutdown`·소멸은 같은 스레드(게임 스레드)에서 부른다(scheduler·World 수명이 그 스레드의 순서에 기댄다).
- `platform::LogicalProcessorCount()`(Thread.h)는 이 프로세스가 쓸 수 있는 논리 프로세서 수다. Android(Linux 커널)는 `sched_getaffinity` + `CPU_COUNT`(UE `Runtime/Core/Private/Unix/UnixPlatformMisc.cpp` 729–735와 같은 방식 — cpuset·`taskset`의 제한을 따른다. POSIX 공통 코드는 OS hook `PosixLogicalProcessorCount`를 부르고 구현은 OS별 파일에 둔다), Apple은 `sysconf(_SC_NPROCESSORS_ONLN)`(affinity API가 없다), Windows는 `GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)`와 `GetProcessAffinityMask`의 비트 수 가운데 작은 값이다(64개를 넘는 프로세서 그룹 기계에서는 mask가 한 그룹만 보이므로 그룹 수 쪽으로 기울지 않게 작은 값). 버린 대안: 시스템 전체 수(`sysconf`·`GetActiveProcessorCount`만) — affinity로 제한된 프로세스가 쓸 수 없는 코어 수만큼 worker를 만들어 과다 구독한다(처음 구현, 리뷰 지적 R4).
- 버린 대안: World마다 scheduler — World 수만큼 worker가 늘어 과다 구독하고, Tick이 차례로 돌아 동시에 쓰지도 않는다. 엔진 범용 작업 scheduler 공유(UE는 GC 병렬 표식에 TaskGraph worker를 쓴다) — 지금 엔진에는 범용 작업 scheduler가 없고, 생기면 GC 표식 slice(수 ms)가 다른 작업을 막지 않도록 우선순위·lane을 정한 뒤 옮긴다. 전용 스레드는 그때까지의 선택이며, 다른 작업 스레드와 합쳐 코어 수를 넘는 과다 구독 위험이 남는다(17.5절).

### 17.5 남은 것과 결정

- 회수 객체당 약 250ns는 소멸자·저장소 반환·슬롯 되돌리기의 메모리 접근이다. 반환 몫은 GC-S6(할당자)에서 줄인다.
- 메모리 압박 신호를 내는 플랫폼 계층은 아직 없다. 엔진 요청 API(다른 스레드에서 불러도 된다)까지 두었다.
- 엔진 Registry(영구 영역·GameInstance 등)는 아직 GC 일정이 없다(엔진 Registry 객체가 생기면 같은 일정을 단다).
- `Incremental` 모드는 World 파괴가 있을 때만 cycle을 시작한다(기존 엔진 런타임 설계). World 파괴 없이 생긴 쓰레기는 다음 World 파괴나 전체 수집 요청까지 남는다 — `FullCollection` 모드는 간격으로 회수한다(17.3절). 후속: 파괴 없이 생긴 쓰레기의 증분 발동 조건(예: 쓰기·생성 수 임계, 최대 간격).
- 진행 중인 cycle의 sweep 나머지는 전체 수집 요청 프레임에 한꺼번에 한다(R1 결정 — 분류는 chunk 비트 판정이라 회수보다 훨씬 싸다. 회수만 예산으로 나눈다).
- 보유로 늦어진 회수(R2 결정)는 보유가 풀린 뒤 첫 GC 단계에서 회수한다. 보유가 풀리지 않으면 계속 남는다(보유자의 책임).
- 주기 간격의 실시간 기준(R3 결정)은 일시정지 중에도 수집을 멈추지 않는다. 일시정지 중 수집을 막아야 하는 앱은 `Incremental` 모드나 요청만 쓴다.
- GC 전용 scheduler와 다른 작업 스레드의 코어 공유(17.4절)는 엔진 범용 작업 scheduler가 생길 때 정한다.

## 18. 공용 할당자 (GC-S6, 8단계, 개정 7–16, 2026-11-20)

6절 GC-S6의 상세 설계다. 코드는 Core(`Core/Source/Runtime/Core` Memory)이고 ObjectRuntime 전용 할당자는 두지 않는다. 다른 세션이 할당자를 맡고 있지 않음을 확인했다(2026-11-19: 원격·로컬 브랜치와 worktree, Core/Memory의 최근 변경은 FrameArena와 개명뿐). 이 절의 설계는 AL0–AL5로 구현하고 쟀다(18.9절 단계, 실험 25절).

**지금 제품 코드가 쓰는 할당자(개정 13 — 사실 정정)**: 제품 코드는 엔진에 `SystemAllocator`를 넘긴다. `launch::ApplicationContext`가 `SystemAllocator`를 소유해 `SystemAllocator()`로 내주고(`Core/Source/Runtime/Launch/Private/Application.cpp`) Sandbox의 `EngineHost`가 그것을 엔진에 넘긴다(`Programs/Sandbox/Private/EngineHost.cpp`). `BinnedAllocator`를 만드는 곳은 자기 시험, `GcSoak` 시험, `GcBench`뿐이다. `BinnedAllocator`를 엔진·응용 프로그램에 연결하는 일은 이 단계 범위에 없었고 결정 대기다. 개정 11은 "1M 작은 객체의 제품 경로는 `BinnedAllocator`"라고 적었는데 사실이 아니었다(18.6절·18.8절 A1에서 그 문장을 뺐다). 그러므로 지금은 `SystemAllocator`의 배수(gate A1)가 1M 규모의 제품 경로에 그대로 걸린다. (개정 15: 연결했다 — 18.10절. 이 문단은 개정 13 때의 상태다.)

### 18.1 지금 상태와 문제

- `SystemAllocator`: 할당마다 OS 할당 둘(사용자 블록 + 원장 레코드)을 하고 살아 있는 할당을 연결 목록에 둔다. `Deallocate`는 그 목록을 주소로 선형 검색한다(O(살아 있는 할당 수)). 샘플·시험의 기본 할당자다.
- `BinnedAllocator`: 16–4096 byte 9개 구간의 `PoolAllocator`와 상위 allocator를 묶고, 모든 호출을 잠금 하나로 직렬화하며 소유권 원장을 선형 검색한다. `PoolAllocator`의 반환은 chunk 목록을 검색한다.
- 측정(실험 3.5절, 살아 있는 객체 20K): 회수가 플랫폼 할당(121ns/객체)의 30–35배(3.7–4.2µs)이고 Binned의 생성은 객체당 0.7ms다. 100K 이상은 끝나지 않아 1M을 잴 수 없었다(실험 0.2절 gate "1M 생성·회수" 행).
- 두 할당자의 계약은 지킬 가치가 있다: 미소유·중복·중간 주소의 `Deallocate`는 그 주소를 역참조하지 않고 `NotFound`이고, 남은 할당이 있으면 파괴가 Fatal이다. 새 구조도 이 계약을 지킨다(18.4절).

### 18.2 할당자 몫을 재는 법

- 6절은 jemalloc·TCMalloc 바인딩으로 할당자 몫을 떼어 재자고 했다. 그 라이브러리는 저장소에 없고 C++ 쪽 외부 편의 라이브러리를 들이지 않는다는 원칙이 있다. 대신 **bump 할당자**(반환이 무동작, 주소만 올리는 벤치 전용 할당자)를 바닥으로 쓴다: 같은 시나리오의 시간에서 bump의 시간을 뺀 것이 그 할당자의 몫이다.
- `GcBench --allocators`를 1M까지 늘리고 할당자를 고르게 한다(bump · 플랫폼(`BenchAllocator`) · `SystemAllocator` · `BinnedAllocator`). 생성+연결, churn 회수, 종료 회수를 객체당 ns로 낸다.
  - 기준 바로잡기(AL5): AL0–AL4의 "플랫폼"은 `BenchAllocator`였다 — 플랫폼 할당 앞에 머리 16 byte를 두고 할당·반환마다 범주별 계수를 atomic으로 올리고 내린다(GC 시나리오의 메모리 표를 위한 장치). 그 계수의 비용이 플랫폼의 몫에 섞여 있어, 다른 할당자를 "플랫폼의 몇 배·몇 %"로 볼 때 기준이 실제보다 비싸게 잡혔다(같은 계수를 `BinnedAllocator` 위에 얹어 돌린 gate A7 측정에서 드러났다). 할당자 비교의 `platform`은 계수·머리 없는 플랫폼 할당(`RawPlatformAllocator` — 작은 벤치가 처음부터 쓰던 것)으로 바꾸고, 옛 기준은 `platform-counted`(종류 4)로 남겨 같은 round에서 둘을 함께 잰다. gate A1·A2의 배수와 몫은 새 기준으로 다시 낸다(18.8절).
- 작은 벤치(`GcBench --allocator-micro`): 크기 16–8192의 할당·반환 쌍(단일 스레드), 한 스레드가 할당하고 다른 스레드가 반환하는 흐름(명시 handoff — 시간에 기대지 않는다), 8 스레드 동시 할당·반환 처리량.
- gate A6·A7의 측정 통로(AL5): `--allocators`가 `SystemAllocator`·`BinnedAllocator` 실행에서 생성 뒤와 churn 뒤의 메모리를 낸다 — `SystemAllocator`의 `liveBytes`는 요청 크기의 합이고 `BinnedAllocator`의 `liveBytes`는 작은 블록 크기의 합 + 큰 요청 크기이며(큰 요청은 backing인 `SystemAllocator`가 센다), 같은 그래프라 두 실행의 할당 수가 같으므로 작은 요청 바이트 = (System 실행의 요청 합) − (Binned 실행의 큰 요청 합)이다. `--allocator-memory`는 요청 크기 분포를 더 낸다(통과 allocator를 끼우므로 그 실행의 시간은 쓰지 않는다). 일반 그래프 시나리오(`--objects N`)는 `--allocator-kind`로 블록을 받는 할당자를 고른다(범주별 계수를 세는 `BenchAllocator`가 머리 16 byte를 더한 크기로 그 할당자에 요청한다 — 플랫폼 쪽도 같은 머리를 두므로 조건이 같다). bump는 한 스레드 전용이라 이 시나리오에 쓰지 않는다.

### 18.3 UE 5.8과의 대조

UE 5.8.4의 작은 블록 할당자(`Runtime/Core` — `Public/HAL/MallocBinned2.h`·`MallocBinned3.h`·`MallocBinned4.h`·`MallocBinnedCommon.h`와 `Private/HAL`의 구현)를 읽었다. Windows·Android·Mac의 기본은 Binned3이고(`UEBuildWindows.cs` 1492–1495, `UEBuildAndroid.cs` 231–234, `UEBuildMac.cs` 294–297), Linux·iOS는 Binned2다.

| 항목 | UE 5.8 | Rocks(이 설계) |
|---|---|---|
| 크기 등급 | Binned2: 51개(16–32752, 표 — `MallocBinned2.cpp` 50–94), Binned3: 52개(8–28672)와 그 위 4KB 단계(`MallocBinnedCommon.cpp` 30–102·142–166) | 32개(16–8192): 128까지 16 단계, 그 위는 2배 구간마다 넷(18.5절) |
| 크기 → 등급 | `(Size+7)>>3`으로 색인하는 uint8 표(`MallocBinnedCommon.h` 858–865) | `(bytes+15)>>4`로 색인하는 uint8 표(513칸) |
| 반환 때 소유 판정 | Binned2: 64KB 정렬로 페이지 머리를 읽음(`MallocBinned2.h` 242–254 — 남의 주소도 읽는다). Binned3: 등급마다 1GB 예약 주소 범위와 주소 산술(`MallocBinned3.h` 30–34·182–215). Binned4: 주소 2단 표(`MallocBinned4.h` 223–235) | 인스턴스마다 예약 주소 범위 하나 + slab 번호별 1 byte 표. 범위 밖 주소는 읽지 않는다(18.4절) |
| 작은 블록 단위 | Binned2·4: 64KB 페이지, 머리(`FPoolHeader`)가 페이지 앞에. Binned3: 등급별 블록(4KB의 배수)과 비트 트리 | 64KB slab, 머리 64 B가 slab 앞에 |
| 큰 할당 | OS 직접 + 페이지 캐시(`TCachedOSPageAllocator<64, 64MB>` — `MallocBinned2.h` 21–22·232–240) | 상위 allocator로 넘기고 주소 hash 원장으로 크기를 안다 |
| 스레드 캐시 | 등급마다 묶음 둘(부분·가득), 묶음 한도 64개 또는 64KB(`MallocBinnedCommon.h` 45–54·266–406) | 같다(등급마다 묶음 둘, 64개 또는 64KB) |
| 스레드 사이 이동 | 등급마다 8칸 lock-free 재활용기(CAS — `MallocBinnedCommonUtils.h` 47–95). 가득 차면 등급 잠금 아래 페이지로 돌려줌 | 같다(등급마다 8칸 atomic 포인터) |
| 다른 스레드가 반환 | 반환한 스레드의 캐시에 넣는다(소유 스레드·원격 목록 없음 — `MallocBinned2.cpp` 886–893) | 같다 |
| 캐시 없는 스레드 | TLS 캐시를 켠 스레드만 캐시(`SetupTLSCachesOnCurrentThread`), 나머지는 등급 잠금 | 모든 스레드가 처음 쓸 때 캐시를 받는다(인스턴스별) |
| 스레드 종료 | 자동 hook이 없다. 캐시를 켠 엔진 스레드가 실행 루프 끝에서 `FMemory::ClearAndDisableTLSCachesOnCurrentThread`를 직접 부른다(`Private/Async/Fundamental/Scheduler.cpp` 869, `Private/Async/TaskGraph.cpp` 636 — 캐시를 비우고 등록을 풀고 해제: `MallocBinnedCommon.h` 553–569·786–796) | 모든 스레드가 캐시를 받으므로 스레드 종료 hook이 필요하다(`platform::ThreadExitSlot` — 18.4절) |
| 주소 공간이 좁은 기기 | iOS는 Binned2(`UEBuildIOS.cs` 709–710: "Prefer MB2 as MB3 requires extended virtual address space entitlement") | 예약을 줄여 다시 시도하고, 끝내 못 하면 모두 큰 경로(18.4절) |
| 잠금 | 등급별 재귀 mutex + 전역 `ExternalAllocMutex`(`MallocBinned2.h` 216, `MallocBinnedCommon.h` 411) | 등급별 mutex + 큰 할당 원장 mutex + slab 범위 mutex |
| 빈 페이지 | Binned2: 즉시 페이지 캐시로(`MallocBinned2.cpp` 318–328), Binned3: 즉시 decommit(`MallocBinned3.cpp` 360–372) | 빈 slab은 한도까지 보관, 넘으면 decommit. `Trim`이 모두 decommit |
| 무결성 검사 | 머리 canary(반환마다 — `MallocBinned2.h` 51–60·329–340), 중복 반환 검출은 선택(기본 꺼짐 — `MallocBinned2.h` 41–46, 블록 안 표지) | 범위·slab 표·블록 경계 검사와 블록 안 반환 표지(모든 빌드 — 18.4절) |
| 인스턴스 | 프로세스 전역 하나 | `Allocator&`로 넘기는 인스턴스(여러 개 가능 — 시험마다 하나) |

이름은 Rocks 것을 쓴다(slab·묶음·이전 칸). UE 이름은 이 표에만 둔다.

### 18.4 `BinnedAllocator` 구조와 계약

같은 클래스 이름으로 다시 만든다(같은 역할의 범용 할당자를 둘 두지 않는다. 지금 쓰는 곳은 자기 시험과 GcBench뿐이다).

- **주소 범위.** 생성 때 `VirtualMemoryRegion`으로 `BinnedAllocatorConfig::reserveBytes`(64KB의 배수)를 예약한다(commit하지 않는다). slab은 그 범위의 64KB 조각이고 쓸 때 commit한다. slab 번호별 1 byte 표(등급 + 1, 0은 안 쓰는 slab — 16 GiB에 256 KB)를 따로 둔다.
  - 기본값: Windows·Mac 16 GiB, Android 4 GiB(39비트 주소 기기에서 인스턴스 여러 개를 감안). iOS는 지원 대상이 아니다 — 더할 때는 UE가 iOS에서 Binned2를 쓰는 이유(큰 주소 공간 예약에 extended virtual addressing entitlement가 필요 — 18.3절 표)를 따라 작은 기본값으로 시작한다.
  - 예약 실패: 생성은 실패하지 않는다. 예약이 실패하면 크기를 절반씩 줄여 가며 `MinReserveBytes`(64 MiB) 이상인 동안 다시 시도하고(기본값은 2의 거듭제곱이라 64 MiB까지 간다. 그렇지 않은 설정은 절반이 64 MiB 아래로 내려가는 데서 멈춘다 — 100 MiB면 한 번), 끝내 못 하면 예약 없이 연다 — 모든 요청이 큰 경로(상위 allocator)로 간다. 실제 예약 크기(`ReservedBytes()`)와 큰 경로로 간 작은 요청 수(`fallbackAllocations`)를 통계로 낸다. slab 표의 플랫폼 할당이 실패해도 같은 처리다. 설정한 `reserveBytes`가 `MinReserveBytes`보다 작으면 그 값 한 번만 시도한다(작게 쓰겠다는 요청을 더 줄이지 않는다 — 도구·시험은 1 MiB 같은 작은 범위를 쓴다).
  - slab 번호 관리(개정 9): 한 번도 쓰지 않은 slab은 bump 번호로 주고, 쓴 뒤 decommit한 slab은 비트맵(slab당 1비트 — 16 GiB에 32 KB)에 두어 낮은 번호부터 다시 쓴다(주소가 앞으로 모인다). 표와 비트맵은 생성 때 한 번 받으므로 그 뒤의 slab 관리에는 실패할 할당이 없다.
  - 예약 종류(개정 10, AL2b): `VirtualMemoryRegion`에 접근을 열어 둔 예약 `VirtualMemoryAccess::Open`을 더하고 `BinnedAllocator`가 그 종류로 예약한다. 기본 종류 `Guarded`(지금까지의 방식)는 POSIX에서 예약을 `PROT_NONE`으로 잡고 commit·decommit마다 `mprotect`로 보호를 바꾼다. 보호가 다른 구간마다 매핑(VMA)이 하나씩 생기므로 commit한 slab과 decommit한 slab이 섞이는 만큼 매핑이 늘고, Linux·Android의 프로세스당 한도(`vm.max_map_count`, 기본 65530)를 넘으면 `mprotect`가 ENOMEM으로 실패한다(그 프로세스의 다른 mmap도 같이 실패하기 시작한다).
    - `Open`은 UE 5.8과 같다: 예약 때 `mmap(PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE)` 한 번(`Runtime/Core/Private/Unix/UnixPlatformMemory.cpp` 635, `Android/AndroidPlatformMemory.cpp` 618, `Apple/ApplePlatformMemory.cpp` 753), commit은 Unix·Android에서 아무 일도 하지 않고(`UnixPlatformMemory.cpp` 671–675, `AndroidPlatformMemory.cpp` 646–650) Apple은 `madvise(MADV_FREE_REUSE)`(784), decommit은 `madvise(MADV_DONTNEED)`(Unix 683, Android 656)·Apple `MADV_FREE_REUSABLE`(791)이다. 보호를 바꾸지 않아 매핑이 쪼개지지 않는다. Windows는 두 종류가 같다(commit charge가 실제라 `VirtualAlloc(MEM_COMMIT)`·`VirtualFree(MEM_DECOMMIT)`이 접근을 열고 막는다).
    - 계약 차이: `Open`의 POSIX에서는 commit하지 않았거나 decommit한 범위의 접근이 fault라는 보장이 없다(읽으면 0이나 옛 내용, 쓰면 페이지가 다시 붙는다). 그래도 되는 근거: 반환의 소유 판정은 쓰지 않는 slab을 slab 표에서 걸러 읽지 않고(아래 2단계), 할당자는 commit하지 않은 slab의 블록을 내주지 않는다. 접근 차단에 기대는 코드(`SecretArena`의 guard 페이지)는 `Guarded`를 그대로 쓴다.
    - 메모리 초과 약정을 끈 시스템(Linux `vm.overcommit_memory=2`)에서는 예약 크기가 곧 약정이라 큰 예약이 거부될 수 있다. 위의 축소 경로(절반씩 64 MiB까지, 끝내 못 하면 큰 경로)가 받는다.
    - 버린 대안: `Guarded`를 그대로 쓰고 한계로 적기 — 1M 객체 규모(slab 수천–수만 개)에서 매핑 한도에 닿는 것은 기본 구성의 실패다. POSIX의 기본 종류를 바꾸기 — `SecretArena`가 `PROT_NONE` guard에 기댄다.
  - commit·decommit 실패(개정 10, AL2b — 개정 9의 처리를 바꾼다): `VirtualMemoryRegion`은 commit·decommit이 실패해도 닫히지 않는다. 전에는 한 번 실패하면 Poisoned가 되어 `Release`만 됐고, 그래서 commit이 한 번 실패한 뒤(Windows의 commit 한도 초과 — 일시적인 실패다) 새 slab을 영영 얻지 못하고 decommit이 거절되어 `Trim`이 메모리를 하나도 돌려주지 못했다 — 메모리가 모자랄 때 반환 수단이 꺼진다. commit은 두 플랫폼 모두 OS 호출 하나(`VirtualAlloc(MEM_COMMIT)`, `mprotect`)이고 실패가 어떤 상태를 남기든 같은 범위의 재시도와 decommit이 유효하므로 영역 전체를 닫을 이유가 없다. Poisoned는 `Release` 실패에만 남는다.
    - commit 실패: 그 요청만 큰 경로로 가고 다음 요청이 같은 slab 번호를 다시 시도한다. UE 5.8은 commit 실패를 치명 OOM으로 끝낸다(`Runtime/Core/Private/Windows/WindowsPlatformMemory.cpp` 596–598). Rocks는 큰 경로로 계속 간다(위 "예약 실패"와 같은 선택).
    - decommit 실패: `Open` 예약과 Windows에서는 그 범위가 그대로다(접근·내용 유지). 그 slab을 commit된 채 보관해 다시 쓰고(보관 한도를 넘을 수 있다) 다음 `Trim`이 다시 푼다. `Guarded`의 POSIX에서는 일부 페이지의 접근이 막혔을 수 있어 호출자가 commit되지 않은 것으로 다룬다(`VirtualMemory.h` 계약 — `BinnedAllocator`는 `Open`만 쓴다). 개정 9의 "상태를 알 수 없어 다시 쓰지 않는 slab"은 없어진다.
    - 실패 주입은 `VirtualMemoryRegion`의 시험 통로 하나로 한다(`PlatformBase/Private/Common/VirtualMemoryTestAccess.h` — 이 스레드의 n번째 Commit·Decommit을 OS를 부르지 않고 실패시킨다). `BinnedAllocator`의 자체 주입(commit 차례, decommit 결과)은 없앤다 — 실패가 실제 영역을 지나야 "닫히지 않음"을 시험이 본다.
  - 내부 상태(등급 32개의 잠금·slab 목록·이전 칸·원장)는 플랫폼 할당 하나에 두고 공개 헤더에는 포인터만 둔다(헤더가 `HashMap`·`VirtualMemory`를 끌어오지 않는다). 그 할당이 실패하면 Fatal이다(생성자는 실패를 돌려줄 길이 없고 상태 없이는 큰 경로도 못 연다).
- **반환의 소유 판정.** 1–3은 주소를 읽지 않고 끝낸다. 4–5는 표로 "쓰는 slab"임을 확인한 뒤 그 slab 안을 읽는다.
  1. `address − base`가 예약 크기 밖이면 큰 할당 원장을 본다(없으면 `NotFound`).
  2. slab 표가 0이면 `NotFound`(쓰지 않는 slab — commit되지 않았을 수 있어 읽지 않는다).
  3. slab 안 offset에서 머리 크기를 뺀 값이 그 등급 블록 크기의 배수가 아니면 `NotFound`(중간 주소). 나눗셈은 등급별 역수 상수로 한다.
  4. 블록 번호가 slab 머리의 "뗀 블록 수" 이상이면 `NotFound`(아직 뗀 적 없는 블록). slab 머리 한 줄을 읽는다(UE Binned2도 반환마다 머리 canary를 읽는다 — slab 수천 개의 머리는 L2 캐시에 든다).
  5. 블록의 반환 표지(둘째 word가 `address XOR 인스턴스 비밀값`)가 서 있으면 `NotFound`(중복 반환, 또는 떼었지만 내준 적 없는 블록).
- **반환 표지의 불변식: 표지가 없는 블록은 지금 사용자에게 나가 있는 블록뿐이다.**
  - 표지를 세우는 때: 사용자가 반환할 때, 그리고 **slab에서 떼어 묶음에 넣을 때**(bump로 새로 뗀 블록 포함 — 연결 word를 쓰는 같은 줄이라 추가 캐시 줄이 없다). 떼기만 하고 표지를 세우지 않으면, 새로 commit한 slab의 블록은 둘째 word가 0이라 4·5단계를 모두 통과해 한 번도 내준 적 없는 주소의 반환이 성공하고 같은 블록이 자유 목록에 두 번 들어간다(메인 세션 검토 F1).
  - 표지를 지우는 때: 사용자에게 내주는 순간뿐이다(스레드 캐시에서 꺼낼 때, 캐시 없는 경로에서 slab이 바로 내줄 때). 지우기 전에 표지가 서 있는지 본다(개정 13 — 아래 "겹친 중복 반환"). 스레드 캐시·이전 칸·slab 자유 목록 사이를 옮길 때는 표지가 그대로다.
  - 다시 쓰는 slab(보관소에서 꺼낸 slab)에는 옛 표지가 남아 있지만 "뗀 블록 수"가 0에서 다시 시작하므로 4단계가 막고, 떼는 순간 새 표지를 쓴다.
  - 표지는 UE의 선택 기능(`UE_MB2_DOUBLE_FREE_DETECTION`, 기본 꺼짐)과 같은 방식을 모든 빌드에서 켠 것이다. 캐시에 든 블록은 slab이 "쓰는 블록"으로 세므로 그 slab은 풀리지 않는다 — 올바른 반환이 읽는 주소는 언제나 commit돼 있다.
  - 한계 1(값 일치): 표지는 64비트 값 일치라 살아 있는 블록의 둘째 word에 사용자가 우연히 같은 값을 써 두면 정상 반환이 `NotFound`가 된다. 비밀값은 인스턴스마다 무작위이고 **최상위 비트를 세운다**(개정 9): 주소의 최상위 비트는 0이므로 표지는 언제나 최상위 비트가 선 값이다 — 0, 작은 정수, 사용자 공간 포인터처럼 흔한 값과는 같아질 수 없고, 최상위 비트가 선 값(음수, 부호가 선 실수) 가운데 2^-63으로만 겹친다. 슬롯별 할당 비트(추가 캐시 줄 하나)를 두지 않는 대신 받아들인다. OS 난수를 얻지 못하면 상태 주소를 섞은 값을 쓴다(표지는 오용 검출용이고 보안 경계가 아니다).
  - 겹친 중복 반환(개정 13 — 개정 9의 비교 교환을 되돌린다, 메인 세션 판정): 반환은 **비교 뒤 단순 대입**이다 — 표지가 서 있으면 `NotFound`, 아니면 표지를 쓴다. 등급 잠금 경로도 같다.
    - 되돌린 까닭(측정 — 실험 25.8절): 비교 교환(잠금 접두 명령)의 몫이 GC churn 회수에서 객체당 약 11 ns(77 → 66 ns), 뜨거운 할당·반환 쌍에서 1.5–1.9 ns(9.1 → 7.2 ns), 8 스레드 처리량의 15–24%였다(판정 때는 10–15%로 보고했고, 다시 잰 범위가 이것이다). UE 5.8의 중복 반환 검출도 비교 뒤 단순 대입이고(`Private/HAL/MallocBinned2.cpp` 118–131 `SetAndValidateDoubleFreeCanary`) 컴파일 스위치의 기본은 0이다(`Public/HAL/MallocBinned2.h` 44–46).
    - 내줄 때의 검사: 블록을 사용자에게 내주는 두 지점(스레드 캐시에서 꺼낼 때, 등급 잠금 경로에서 slab이 바로 내줄 때)에서 표지를 지우기 전에 표지가 서 있는지 본다. 서 있지 않으면 Fatal `BinnedAllocator free block lost its return marker`다. 표지 불변식상 나가 있지 않은 블록은 모두 표지가 서 있으므로, 서 있지 않다는 것은 같은 블록이 두 목록에 들어갔거나(겹친 중복 반환 — 한쪽 목록에서 먼저 나가며 표지가 지워졌다) 반환 뒤에 누가 그 word를 썼다는 뜻이다. 연결 word를 읽는 같은 줄이라 비용이 거의 없다(측정으로 확인 — 실험 25.8절).
    - 계약: 순서가 있는 중복 반환은 전처럼 `NotFound`다. 두 스레드가 겹쳐 반환한 중복(한쪽의 비교와 대입 사이에 다른 쪽이 비교하는 수 ns의 창)은 둘 다 성공으로 돌아올 수 있고, 그 블록을 두 번째로 내주려는 순간 Fatal로 잡는다. 반환한 뒤 그 블록의 둘째 word를 덮어쓴 use-after-free도 같은 Fatal로 잡힌다(첫째 word만 덮어쓴 경우는 연결이 깨져 잡지 못한다 — 표지는 둘째 word만 본다).
    - 한계: 검사는 블록을 내줄 때만 한다. (1) 그 블록을 먼저 받은 사용자가 다시 반환한 뒤에 다른 목록에서 꺼내면 표지가 다시 서 있어 놓친다(같은 블록이 계속 두 목록에 든다). (2) 두 번 든 블록이 다시 나가기 전에 slab으로 돌아가는 경로(스레드 종료의 캐시 반납, `Trim`, 묶음이 넘쳐 slab으로 돌려주는 경우)는 이 검사를 지나지 않는다 — slab의 "쓰는 블록 수"가 두 번 줄어, 살아 있는 블록이 남은 slab이 풀려 그 블록이 조용히 다시 나가거나, decommit된 slab을 읽는 접근 위반이나 파괴 때의 Fatal `slab accounting diverged`가 될 수 있다(리뷰 지적 — `ReturnBlock`은 표지를 보지 않는다). 두 목록에 든 블록의 연결 word는 나중에 넣은 쪽이 덮어쓰므로 그 뒤의 블록들이 두 목록에 함께 달릴 수 있다 — 그 블록들도 두 번째로 나가는 순간 같은 Fatal이다.
    - 올바른 프로그램에서는 표지 word를 두 스레드가 함께 만지지 않는다. 블록의 주인은 언제나 하나다: 사용자 → 반환한 스레드의 캐시(그 스레드만 만진다) → 이전 칸(원자 교환으로 넘긴다) → slab 자유 목록(등급 잠금).
    - 시험: 결과가 timing에 기대게 되는 동시 중복 반환 시험(`CoreBinnedConcurrentDuplicateReturnHasOneWinner`, `CoreBinnedCrossThreadAndConcurrentDuplicateReturn`의 동시 부분)은 없앤다. 대신 결정적인 Fatal 대조(자식 프로세스) 셋을 둔다 — 시험 통로로 같은 블록을 목록에 두 번 넣은 뒤 할당, 반환한 블록의 둘째 word를 덮어쓴 뒤 할당(스레드 캐시 경로, 등급 잠금 경로). 음성 대조는 내줄 때의 검사를 생략하는 것이다.
    - ASan: 표지의 비교·대입·검사는 모두 계측 제외 내부 함수의 직접 접근이다(비교 교환을 위해 그 word를 열던 처리는 없어진다).
    - 버린 대안: 비교 교환 유지(위 비용 — 수 ns 창의 오용을 막으려고 모든 반환이 낸다), 검사 없이 단순 대입만(UE 방식 그대로 — 겹친 중복이 같은 블록을 조용히 두 번 내준다), 내줄 때 대신 묶음을 옮길 때 검사(블록을 한 번 더 읽는다).
  - 뗀 블록 수의 게시 순서(개정 9): 묶음을 뗄 때 블록마다 표지를 먼저 쓰고 "뗀 블록 수"를 뒤에 게시한다(release 저장, 반환은 acquire로 읽는다). 4단계를 지난 주소의 블록에는 표지가 이미 서 있다.
  - 한계 2(동시성 — 메인 세션 검토 F2): 오용(미소유·옛·중복 주소) 검출은 그 주소가 든 slab이 **같은 시점에 다른 스레드에 의해 풀리거나 다시 쓰이는 중이 아닐 때** 성립한다. 잘못된 반환이 slab 표를 읽은 직후 다른 스레드가 그 slab을 decommit하면(보관소 넘침·`Trim`) 머리나 표지를 읽다가 fault가 나고, 그 slab이 다른 등급으로 다시 쓰이면 옛 등급의 경계로 새 내용을 읽어 미검출일 수 있다. 올바른 반환은 그 블록이 "쓰는 블록"이라 slab이 풀리지 않으므로 영향이 없다. 주소 재사용 뒤의 옛 포인터를 구분하지 못한다는 `Allocator.h`의 기존 한계와 같은 부류다. 오용 시험은 decommit·재사용과 경쟁하지 않게 만든다(단일 스레드, 또는 그 slab에 살아 있는 블록을 남겨 둔다).
- **slab.** 머리 64 B(등급, 쓰는 블록 수, 뗀 블록 수(bump), 돌려받은 블록의 자유 목록, 등급의 부분 slab 목록 연결)가 slab 앞에 있고 블록이 뒤를 채운다. 새 slab은 자유 목록을 미리 만들지 않고 bump로 떼어 준다. 쓰는 블록 수가 0이 되면 등급 목록에서 빠져 빈 slab 보관소(인스턴스 공용, 한도 64개 = 4 MiB)로 가고, 넘으면 표를 0으로 한 뒤 decommit한다.
- **스레드 캐시.** 스레드마다·인스턴스마다 등급별 묶음 둘(부분·가득)을 둔다. 묶음 한도는 64개 또는 64KB 가운데 작은 쪽이고 **등급으로 정해진다**(8192 등급은 8개).
  - 할당: 부분 묶음에서 꺼낸다. 비면 가득 묶음과 바꾸고, 그것도 비면 이전 칸에서 묶음 하나를 받고, 없으면 등급 잠금 아래 부분 slab(없으면 새 slab)에서 한 묶음을 채운다(표지를 세운다).
  - 반환: 부분 묶음에 넣는다. 차면 가득 묶음과 바꾸고, 가득 묶음도 차 있으면 그것을 이전 칸에 넣고, 칸이 없으면 등급 잠금 아래 블록마다 자기 slab으로 돌려준다.
  - 캐시 없는 경로(개정 9): 등급 잠금 아래 slab에서 블록 하나를 바로 주고받는 경로를 함께 둔다. 캐시를 얻지 못한 호출(스레드 기록·캐시의 플랫폼 할당 실패, OS의 스레드 종료 칸 소진), 종료 hook이 지나간 스레드, 그리고 `BinnedAllocatorConfig::threadCaches`를 끈 인스턴스가 쓴다. `threadCaches = false`는 반환한 블록이 곧바로 slab으로 돌아가 slab 수준 동작(보관·decommit·다시 쓰기)이 결정적이므로 시험과 작은 도구가 쓴다. 예약하지 못한 인스턴스는 작은 블록이 없으므로 캐시도 만들지 않는다. 기록을 스레드 종료 칸에 걸지 못한 스레드(칸 없음, OS 거부)는 "걸지 못한 스레드" 표지 기록을 가리키게 해 다시 시도하지 않는다 — 다시 해도 결과가 같고, 호출마다 기록을 할당했다가 버리지 않는다(개정 12, 메인 세션 검토 AL3). 기록·캐시의 플랫폼 할당 실패는 일시적일 수 있어 다음 호출이 다시 시도한다.
  - 이전 칸: 등급마다 atomic 포인터 8칸(한 캐시 줄)이다. 넣기는 빈 칸에 비교 교환, 꺼내기는 찬 칸을 null과 교환이다(묶음 통째로 주고받으므로 ABA가 없다). **이전 칸의 묶음은 언제나 가득 찬 묶음이고 개수는 등급의 한도다** — 개수를 블록에 적지 않는다(16 byte 블록은 word가 둘뿐이고 첫째는 연결, 둘째는 표지다). 부분 묶음은 이전 칸에 가지 않는다: 스레드 종료·`Trim`·인스턴스 파괴 때의 부분 묶음은 등급 잠금 아래 slab으로 바로 돌려준다.
  - 스레드 쪽 조회: 번역 단위 안의 `thread_local` 포인터(소멸자 없는 단순 값)가 그 스레드의 기록(인스턴스 번호 → 캐시의 작은 표)을 가리키고, "마지막에 쓴 인스턴스" 한 칸을 먼저 본다. 인스턴스 번호는 프로세스 전역에서 재사용하지 않는다(같은 주소에 다시 만든 인스턴스와 구분). 기록의 죽은 인스턴스 항목은 그 스레드가 다음 캐시를 만들 때 버린다(인스턴스를 수천 번 만들고 없애는 시험에서 기록이 자라지 않는다).
- **스레드 종료와 수명**(메인 세션 검토 F3). PlatformBase에는 스레드 종료를 알리는 기능이 없다(FLS·pthread key 소멸자를 쓰는 코드가 없음). `platform::Thread`로 만들지 않은 스레드(OS 스레드 풀, 외부 라이브러리)도 캐시를 받으므로 OS의 스레드별 소멸 hook이 필요하다.
  - PlatformBase에 `platform::ThreadExitSlot`을 더한다(`Platform/Thread.h`): 스레드별 포인터 칸 하나와, 값이 null이 아닌 스레드가 끝날 때 **그 스레드에서** 불리는 정리 함수다. Windows는 `FlsAlloc`의 callback, POSIX는 `pthread_key_create`의 소멸자다. API는 생성자(callback)·`IsValid()`·`Get()`·`Set(value)`이고 프로세스 수명 동안 유지한다(소멸자는 칸을 해제하지 않는다 — Windows `FlsFree`는 남은 값마다 callback을 부른다). OS가 칸을 주지 않으면 `IsValid()`가 거짓이고 그 스레드들은 캐시 없는 경로로 간다. `Thread::Join`은 이 정리가 끝난 뒤에 돌아온다(시험이 순서를 Join으로 만든다).
  - UE 5.8도 같은 장치를 쓴다(`Runtime/Core/Private/Windows/WindowsPlatformTLS.cpp` 104: `FlsAlloc(&OnThreadExit)`, 119–135: callback이 값을 먼저 지운다 — "clear the value otherwise this function will be called again"). Windows 구현(AL3): FLS callback은 값만 받으므로 칸마다 다른 trampoline을 건다(그래서 칸 수에 상한 `MaxThreadExitSlots` 8을 두고 POSIX도 같은 상한을 지킨다). trampoline은 그 스레드의 값을 먼저 지우고, 프로세스 종료 절차 중이면(`RtlDllShutdownInProgress`) 부르지 않고, 아니면 callback을 부른다. POSIX의 key 소멸자는 값을 지운 뒤에 불리고 `main` 반환·`exit`에서는 불리지 않으므로 두 플랫폼의 계약이 같다: callback 안에서 `Get()`은 null이고, 프로세스가 끝날 때는 불리지 않는다.
  - OS 오류 값 보존(개정 15 — 리뷰 S1): `Get`·`Set`은 호출 스레드의 OS 오류 값(Windows last-error, POSIX `errno`)을 바꾸지 않는다(성공하든 실패하든 — 두 구현 모두 호출 앞뒤로 저장·복원한다). Windows의 `FlsGetValue`는 성공해도 last-error를 0으로 지운다(측정 — Windows 11 26200: 값이 있든 없든 `FlsGetValue` 뒤 last-error가 0, `FlsSetValue`는 성공하면 그대로). 할당자의 느린 경로(스레드의 첫 호출)가 이 조회를 지나므로, 제품에 연결하면 실패한 Win32 호출과 그 뒤의 `GetLastError` 사이에 할당이 끼어 오류 값이 지워질 수 있다. 그래서 Windows 구현이 호출 앞뒤로 last-error를 보존한다(읽기 하나와 쓰기 하나 — 느린 경로에만 든다). POSIX의 `pthread_getspecific`·`pthread_setspecific`은 성공할 때 `errno`를 바꾸지 않지만 실패한 `pthread_setspecific`이 바꾸는 libc가 있어(리뷰 지적 — 확인하지는 못했다) POSIX 구현도 같은 모양으로 보존한다. 시험은 성공 경로와 유효하지 않은 칸을 본다(OS 호출이 실패하는 경로는 만들지 못한다).
  - 계약: callback은 짧은 정리(잠금, 메모리 반환)만 하고 스레드 생성·모듈 로드·다른 스레드 대기를 하지 않는다. 다른 TLS 소멸과의 순서는 정하지 않는다. 프로세스가 끝날 때는 불리지 않는다: POSIX는 `main` 반환·`exit`에서 key 소멸자를 부르지 않고, Windows는 종료를 부른 스레드에서 FLS callback을 부르지만 그때 다른 스레드는 이미 강제 종료돼 잠금을 쥔 채일 수 있어(정리 함수가 잠금을 기다리면 종료가 멈춘다) 구현이 걸러 낸다. 프로세스 수명과 같은 자원은 OS가 거둔다. `thread_local`(개정 12 — macOS CI에서 깨져 좁혔다): trivial한 `thread_local` 변수는 callback 안에서 읽고 쓸 수 있지만(접근은 된다) **그 스레드가 두었던 값이 남아 있다는 보장은 없다. callback은 인자로 받은 값만 믿는다.** 이 한 줄이 모든 플랫폼의 계약이고, 시험도 접근만 본다(쓰고 읽으면 쓴 값 — 두었던 값이 남았는지는 단언하지 않는다). 개정 11까지는 "읽고 쓸 수 있다"만 적고 시험이 값 유지를 단언했는데 Windows에서만 성립했다.
    - macOS(CI run 44636348775에서 확인 — callback이 읽은 값이 초기값. 원인은 소스로 확인: dyld `libdyld/ThreadLocalVariables.cpp`, libpthread `src/pthread_tsd.c`): 이미지의 `thread_local` 저장소가 pthread key의 값이고 그 소멸자가 `free`다. libpthread는 동적 key를 번호 오름차순으로 돌고 새 key는 가장 낮은 빈 번호를 받는다 — 저장소의 key가 우리 칸(처음 쓸 때 만든다)보다 먼저 만들어져 저장소가 먼저 해제된다(이 순서는 CI 결과와 맞는 추론이다). 그 뒤의 접근은 초기 내용으로 저장소를 다시 만들어 key에 다시 걸고(그래서 접근은 된다), 값이 남은 key는 `PTHREAD_DESTRUCTOR_ITERATIONS`회까지 다시 돌므로 다음 회차에 또 해제된다.
    - Windows(측정 — Windows 11 26200, clang-cl `/MDd`·`/MT` 탐침 각 3회): FLS callback이 non-trivial `thread_local` 소멸자보다 먼저 불리고, callback이 읽은 값은 스레드가 둔 값이며, callback이 쓴 값을 뒤에 도는 `thread_local` 소멸자가 그대로 읽는다 — 저장소는 둘이 모두 끝난 뒤까지 남는다. 문서로 보장된 순서는 아니다(OS 구현의 순서를 잰 것).
    - Android(소스로 확인, 기기에서 돌려 보지는 않았다): 이 저장소의 대상(API 26)은 emulated TLS다(NDK 29 clang의 출력이 `__emutls_get_address`를 부른다. API 29 이상은 ELF TLS). compiler-rt `lib/builtins/emutls.c`는 저장소를 pthread key의 값으로 두고 bionic에서는 해제를 한 회차 미룬다(`EMUTLS_SKIP_DESTRUCTOR_ROUNDS 1` — "pthread 소멸자 안에서 `thread_local`을 쓰는 앱" 때문이라고 적혀 있다). 첫 회차의 callback에서는 값이 남아 있고 둘째 회차부터는 macOS와 같다. ELF TLS면 bionic `pthread_exit`가 `thread_local` 소멸자(`__cxa_thread_finalize`) → key 소멸자(`pthread_key_clean_all`) → 동적 TLS 해제 순서라 값이 남는다.
  - fiber에서 쓰지 않는다(개정 12, 메인 세션 검토 AL3): Windows의 FLS 값은 fiber마다 따로이고(스레드를 fiber로 바꾸면 그 스레드의 값이 그 fiber의 것이 된다) 정리 함수는 fiber를 지우는 스레드에서 불린다. 값을 둔 스레드와 정리 함수가 도는 스레드가 달라지면 "그 스레드에서 불린다"가 깨진다 — 할당자는 캐시를 소유 스레드만 만진다고 가정한다. 저장소에 fiber를 쓰는 코드는 없어 구현은 그대로 두고 `ThreadExitSlot`의 계약에 적는다.
  - 등록부와 수명(개정 9): 살아 있는 인스턴스의 등록부(잠금 + 인스턴스 목록 + 다음 번호 + 종료 칸)는 처음 쓸 때 플랫폼 할당으로 만들고 **파괴하지 않는다** — Windows는 프로세스 종료를 부른 스레드의 종료 hook을 정적 소멸 뒤에 부를 수 있어, 정적 저장 기간 객체로 두면 hook이 파괴된 잠금을 잡는다. 인스턴스는 수가 적어 intrusive 목록으로 두고 번호로 찾는다(버린 대안: 번호 → 인스턴스 hash 표 — 생성 때 실패할 할당이 하나 더 생긴다). 종료 hook은 그 스레드의 `thread_local` 포인터를 "종료한 스레드" 표지 기록으로 바꿔, 뒤따르는 종료 정리(다른 thread_local 소멸자 등)가 allocator를 써도 해제한 기록을 보지 않고 캐시 없는 경로로 가게 한다(캐시를 다시 만들지 않는다).
  - 기준은 칸의 값이다(개정 12): 스레드의 기록은 **종료 칸의 값이 기준**이고 `thread_local` 포인터는 그 값을 빨리 읽기 위한 사본이다. 포인터가 비어 있으면(초기값) 느린 경로가 먼저 칸의 값을 보고, 값이 있으면 그 기록을 다시 가리킨다. 스레드가 끝나는 동안 `thread_local` 저장소가 해제된 뒤·우리 hook이 불리기 전에 같은 스레드의 다른 종료 정리가 allocator를 쓰는 경우다(macOS): 칸을 보지 않고 새 기록을 만들어 걸면 칸에 살아 있던 옛 기록이 덮여, 그 기록의 캐시들이 hook으로 돌아오지 못한다(인스턴스 파괴 때까지 블록을 쥐고 기록은 샌다). 빠른 경로는 그대로다 — 칸은 포인터가 비어 있을 때만 읽고, 그것은 스레드가 기록을 얻기 전의 호출뿐이다(보통 첫 호출 한 번). 시험은 `thread_local` 포인터만 지우는 시험 통로로 이 상태를 만든다(지운 뒤에도 기록이 하나이고 스레드가 끝나면 캐시 수가 0).
  - 남는 한계(개정 12) — hook이 지나간 뒤 같은 스레드의 **뒤 순번** 종료 정리가 allocator를 쓰는 경우: "종료한 스레드" 표지는 `thread_local` 포인터에만 있다(칸의 값은 hook이 불릴 때 이미 null이고 callback은 같은 칸에 다시 걸지 않는다). `thread_local` 저장소가 다시 해제되면 표지도 사라지고, 그 뒤의 호출은 새 기록과 캐시를 만들어 칸에 건다. POSIX는 값이 다시 걸린 key의 소멸자를 `PTHREAD_DESTRUCTOR_ITERATIONS`(POSIX 최소 4)회까지 다시 돌리므로 그 안이면 hook이 새 기록을 다시 정리한다. 마지막 회차에서 우리 key의 차례 뒤에 생긴 기록은 hook이 불리지 않는다: 기록(수십 byte와 항목 표)이 새고, 그 캐시의 블록은 인스턴스 파괴 때 회수된다(캐시는 인스턴스가 소유하므로 해제된 메모리를 만지는 일은 없다). 플랫폼별로 보면 Windows는 표지가 스레드 끝까지 남아 해당이 없고(위 측정), macOS는 hook이 표지를 쓰는 순간 저장소가 다시 생겨 그 회차에는 표지가 서 있고 다음 회차의 해제 뒤부터 해당하며, Android(API 26)는 저장소가 해제되는 둘째 회차 뒤부터 해당한다. 같은 부류가 하나 더 있다 — 그 스레드가 allocator를 **처음** 쓰는 것이 hook의 차례 뒤에 도는 종료 정리인 경우다(기록이 없었으므로 hook이 불리지 않았고 표지도 없다). Windows는 FLS callback을 칸 번호 오름차순으로 한 번씩만 부르고 `thread_local` 소멸자를 그 뒤에 부른다(측정 — 같은 탐침: 차례가 지난 칸에 값을 걸면 `FlsSetValue`는 성공하지만 callback은 불리지 않는다). 그래서 뒤 번호 칸의 callback이나 `thread_local` 소멸자에서 allocator를 처음 쓴 스레드의 기록은 새고 캐시는 인스턴스 파괴 때 회수된다. POSIX는 회차가 남아 있으면 hook이 불린다. 버린 대안: hook 안에서 같은 칸에 표지 값을 다시 걸기 — POSIX는 그 값으로 소멸자를 회차마다 다시 부르고 Windows FLS는 다시 부르지 않아 칸의 계약이 플랫폼마다 달라진다. 표지만 두는 OS TLS 칸(소멸자 없는 pthread key·`TlsAlloc`)을 따로 두면 한계가 없어지지만, 종료 정리 안에서 hook 뒤 회차에 allocator를 쓰는 코드가 저장소에 없어 플랫폼 기능을 하나 더 두지 않는다(생기면 이 대안으로 간다).
  - 할당자 쪽: 캐시는 인스턴스가 소유하고 등록 목록에 둔다. 스레드가 끝나면 hook이 그 스레드의 기록을 돌며, 프로세스 전역 인스턴스 등록부(mutex)에서 번호가 살아 있는 인스턴스에만 캐시를 돌려준다 — 블록을 slab으로(가득 묶음은 이전 칸에 자리가 있으면 그리로), 계수를 인스턴스 합계로, 캐시 레코드 반납. 인스턴스가 먼저 파괴된 경우: 인스턴스 파괴가 같은 등록부 잠금 아래 자기 번호를 지우고 남은 캐시를 모두 비워 해제했으므로, hook은 그 항목을 건너뛴다(죽은 캐시를 만지지 않는다). 프로세스 종료 때 주 스레드: 인스턴스 파괴가 주 스레드의 캐시까지 비우므로 hook이 불리지 않아도 누수가 없고, 뒤늦게 불리면 모든 항목이 죽은 번호라 기록만 반납한다.
- **큰 할당**(8192 byte 초과, 정렬 64 초과, 예약 범위가 다 찼거나 예약하지 못했을 때의 작은 요청): 상위 allocator로 넘기고 주소 → 크기 원장(18.6절의 공용 내부 구현)에 적는다. 예약 범위가 다 찬 것은 실패가 아니라 느린 경로다(통계에 센다). 큰 경로로 간 작은 요청도 정렬 16 이상을 지킨다.
  - 상위가 준 주소의 검증(개정 9, Fatal): 성공과 함께 null, 정렬이 틀린 주소, 주소 + 크기가 넘치는 주소, 예약 범위와 겹치는 주소, 원장에 이미 있는 주소(살아 있는 주소를 다시 줌). 부분 겹침은 hash 원장으로 잡지 못한다(다시 만들기 전에는 구간 목록의 선형 검색으로 잡았다 — 원장을 O(1)로 바꾼 대가이고, 그 책임은 상위 allocator에 있다).
  - 상위가 큰 블록의 반환을 거부하면 그 오류를 돌려주고 할당은 살아 있는 채로 남는다. 원장에서 먼저 지우고(동시 중복 반환은 한쪽만 지나간다) 상위를 잠금 밖에서 부른 뒤, 거부되면 기록을 되돌린다. 되돌리는 넣기까지 실패하면(표 늘리기 실패 — 이중 실패) Fatal이다. 원장에 적지 못한 새 블록은 상위에 돌려주고 `OutOfMemory`다(상위가 그 반환을 거부하면 Fatal).
- **정렬.** 블록은 16 byte 정렬이다. 16 초과 64 이하 정렬은 블록 크기가 그 정렬의 배수인 첫 등급으로 올린다(머리 64 B와 블록 크기가 모두 배수라 블록 주소가 정렬된다 — UE `PromoteToLargerBin`과 같다).
- **통계.** `Stats()`(할당 수·byte)와 `Usage()`(실제 예약 크기, commit한 slab byte, 보관 중인 빈 slab 수, 살아 있는 큰 할당 수, 큰 경로로 간 작은 요청의 누적 수)로 나눠 낸다. 다른 스레드가 쓰는 중에는 한 블록의 반환 계수를 할당 계수보다 먼저 읽을 수 있으므로 등급별로 음수가 되지 않게 자른다. `liveAllocations`·`totalAllocations`는 스레드 캐시의 계수(소유 스레드만 쓰는 relaxed atomic)와 인스턴스 합계를 더한 값이다. `liveBytes`는 요청 크기가 아니라 **블록 크기의 합**이다(반환 때 요청 크기를 알 수 없다 — `Allocator.h`의 "요청 크기"와 다른 점을 클래스 주석에 적는다). 다른 스레드가 쓰는 중의 `Stats`는 그 순간의 근사다.
- **ASan**(메인 세션 검토 F5). ASan 구성에서는 반환한 블록 전체를 `AsanPoisonMemory`로 닫고 할당 때 요청 크기만 연다(기존 `AddressSanitizer.h` 규칙). 할당자 자신은 닫힌 블록의 연결 word·표지를 읽고 쓰므로, 그 접근은 계측에서 뺀 내부 함수 둘(word 읽기·쓰기)로만 한다 — `Base/Compiler.h`에 `ROCKS_NO_SANITIZE_ADDRESS`(clang `no_sanitize("address")`)를 더한다. slab 머리는 닫지 않는다. 구현(AL4)에서 정한 점: slab에서 뗀 블록도 그 자리에서 닫는다(불변식 — 사용자에게 나가 있지 않은 블록은 모두 닫혀 있고, 나가 있는 블록은 요청 크기만 열려 있다). 계측에서 빠지는 것은 그 함수 본문의 직접 접근뿐이라(그 함수가 부른 함수의 접근은 검사된다) 연결·표지의 단순 읽기·쓰기만 내부 함수로 하고, 표지를 세우는 비교 교환은 계측되는 원자 함수(`Platform/Atomics.h` — 원자 builtin을 직접 쓰지 않는 정책)를 지나므로 그 word(8 byte)만 열고 한다: 성공하면 호출자가 블록 전체를 닫고, 실패하면(이미 닫힌 블록) 그 word를 다시 닫는다(개정 13: 표지를 단순 대입으로 되돌리면서 이 처리는 없어졌다 — 표지 접근은 모두 계측 제외 함수의 직접 접근이다). 주소 범위를 풀 때는 쓴 적 있는 slab을 모두 연다(같은 주소를 OS가 다른 용도로 다시 내줄 수 있다). ASan이 아닌 구성에서는 이 호출들이 컴파일에서 빠진다. 버린 대안: 앞 16 byte를 열어 두기(가장 작은 등급은 검사가 없어지고 모든 블록의 첫 두 word가 빠진다), 내부 접근마다 열고 닫기(느리고, 열린 사이의 사용자 오용을 놓친다).
- **계약.** 0 byte는 성공한 nullptr, 잘못된 정렬은 `InvalidArgument`, 미소유·중복·중간 주소 반환은 `NotFound`(범위 밖·안 쓰는 slab·중간 주소는 역참조 없이, 그 밖은 위 한계 2 안에서), 남은 할당이 있으면 파괴 Fatal, `Trim`은 호출 스레드 캐시·이전 칸·빈 slab 보관소를 비워 decommit한다(다른 스레드의 캐시는 건드리지 않는다 — 개정 16: 이름이 `TrimMemory`가 됐고 쉰다고 표시한 스레드의 캐시는 비운다, 18.11.4절). 지금의 "같은 스레드 재진입은 Busy" 규칙은 없앤다(작은 경로는 상위 allocator를 부르지 않고, 큰 경로는 잠금 밖에서 부른다). `Trim`은 값을 돌려주지 않는다(살아 있는 할당이 있어도 되고 실패할 것이 없다 — 전의 `Busy`가 없어진다). `Shutdown`은 없앤다(닫힌 상태를 따로 두지 않는다 — 수명은 객체 파괴로 끝난다).

### 18.5 크기 등급

- 16, 32, …, 128(16 단계 8개), 그 위는 2배 구간 (128, 256], …, (4096, 8192]마다 넷(160·192·224·256, 320·384·448·512, …, 5120·6144·7168·8192)으로 32개다. 모두 16의 배수다. 128을 넘는 요청의 등급 올림 낭비는 최대 20%(2배 구간의 첫 단계 — 129 → 160)이고 구간 끝으로 갈수록 준다(225 → 256은 12%). 128 이하는 16 byte 단위 올림이다(최소 블록 16).
- 상한 8192의 근거: slab이 64KB 고정이면 8192 블록이 slab당 7개(꼬리 낭비 12.4%)다. 16384는 3개(25%)라 고정 slab으로는 낭비가 크다. 그 위는 큰 할당이다. 엔진의 객체 저장소·작은 배열은 이 안이고, GC 페이지(약 68KB)·Slot chunk(약 12KB)는 큰 할당이다(드물다).
- 버린 대안: UE Binned2 표(64KB의 약수에 맞춘 51개 — 32752까지) — 큰 등급은 slab당 1–2개라 slab 머리·잠금 비용이 블록마다 든다. 등급별 여러 조각 slab(큰 등급은 128–256KB) — slab 번호 → slab 시작을 찾는 표가 더 필요하다. 실제 엔진의 크기 분포를 잰 뒤 상한을 올릴 때 다시 본다(18.8절).

### 18.6 주소 원장과 `SystemAllocator`

- **공용 내부 구현 하나**(메인 세션 검토 F6): 주소 → 크기 원장을 `Core/Private/Memory`의 내부 클래스 하나(`detail::AddressLedger`)로 두고 `SystemAllocator`와 `BinnedAllocator` 큰 경로가 함께 쓴다(각자 구현하지 않는다). 구현은 Core `HashMap<uptr, usize>`다 — 같은 모듈(Core)이라 층 의존 문제가 없고, 할당 재귀는 원장 전용 내부 allocator(플랫폼 할당을 그대로 부르는 추적 없는 `Allocator` 구현)로 끊는다(지금 `SystemAllocator`의 원장 레코드가 플랫폼 할당을 쓰는 것과 같다). `HashMap`은 지운 칸(tombstone)을 rehash로 정리하며 상환 O(1)이다.
- `SystemAllocator`: 살아 있는 할당의 연결 목록을 이 원장으로 바꾼다. 할당마다의 원장 레코드 OS 할당이 없어진다(OS 할당 둘 → 하나). `Deallocate`는 O(1) 기대(상환)다. 계약은 그대로다(잠금 하나, 미소유·중복은 역참조 없이 `NotFound`, 파괴 때 남은 할당 Fatal). 원장 넣기가 실패하면(표 늘리기 실패) 그 `Allocate`가 `OutOfMemory`다(사용자 블록을 먼저 반환한다).
- 버린 대안: 원장 전용 열린 주소 표를 새로 만들기(칸 16 B, 묘비 없는 삭제) — `HashMap`(칸 32 B)보다 작지만 같은 기능의 중복 구현이다. gate A1 측정에서 원장이 병목이면 다시 본다. **AL1에서 다시 봤다(실험 25.2절)**: 표가 작을 때 원장과 잠금의 몫은 연산당 19 ns이고, 1M에서의 차이(churn 회수가 플랫폼의 3.2배 — 옛 기준(platform-counted). 계수 없는 플랫폼 기준으로는 3.8–4.3배: 18.8절)는 반환마다 큰 표를 무작위로 찌르는 캐시·TLB 미스 하나다. 칸을 16 B로 줄여도 표가 64 → 32 MB가 될 뿐 미스는 남으므로 바꾸지 않는다.
- 버린 대안 — 원장의 지역성 hash(AL1b에서 넣었다가 개정 11에서 되돌렸다, 메인 세션 판정): 문제는 칸 크기가 아니라 무작위성이므로, 주소의 4KB 페이지 번호만 섞고 페이지 안 위치(16 byte 단위)를 더하는 hash를 `HashMap`의 `Hasher`로 원장에만 넣어 봤다. 같은 페이지의 블록이 표의 이웃 256칸에 모여, 주소가 가까운 블록을 잇달아 넣고 지우는 흐름이 표의 같은 영역을 다시 쓴다.
  - 이득(1M, 같은 round에서 번갈아 3회의 최솟값 — 실험 25.2절): churn 회수 330 → 214 ns/객체, 종료 회수 379 → 277, churn 생성 1,805 → 1,596, 생성 1,856 → 1,731. 플랫폼 대비(옛 기준(platform-counted)) churn 회수가 3.0배에서 1.95배가 됐다. 창을 512·1,024칸으로 넓히면 더 느렸다(1M churn 회수 331 · 346 · 433 ns, 기본 hash 600 ns — 기계에 부하가 걸린 round).
  - 버린 까닭은 군집이다. 페이지마다의 256칸 창이 무작위 자리에 놓여 창이 겹치는 곳의 부하가 고르지 않고, 이어지는 주소에서는 표의 채움 비율(산 원소 + 삭제 칸 — `HashMap`은 3/4까지 찬다)에 따라 절벽으로 커진다. 선형 탐사를 흉내 낸 값이다(칸 수 2^20, 이어지는 주소, 실제 `MixHash` — 넣은 원소가 제자리에서 밀린 칸 수의 평균 / 그 채움에 닿기 직전의 삽입 4,096번이 본 칸 수의 평균. 작업 임시 폴더 `al1b/simulate_fill.py`):

    | 블록 간격 | 채움 0.5 | 0.625 | 0.74 | 기본 hash 0.74 |
    |---|---|---|---|---|
    | 16 B | 130 / 308 | 202 / 543 | 342 / 1,546 | 1.4 / 7.6 |
    | 32 B | 39 / 186 | 72 / 367 | 140 / 619 | 1.4 / 7.5 |
    | 64 B | 11 / 60 | 26 / 169 | 54 / 201 | 1.4 / 8.0 |
    | 112 B | 3.0 / 21 | 9.8 / 41 | 27 / 174 | 1.4 / 7.7 |
    | 208 B | 0.8 / 4.6 | 2.9 / 14 | 9.4 / 95 | 1.4 / 7.9 |

    최장 탐사는 채움 0.74에서 16 B 간격 13,469칸, 64 B 간격 4,371칸이다(기본 hash는 120–200칸). 실제 `HashMap`으로 잰 부하율 1/2의 최장 탐사(65,536개)도 이어지는 주소에서 16 B 1,765 · 32 B 1,416 · 64 B 1,080 · 208 B 217칸이었다(기본 hash는 24–45칸).
  - 잰 벤치(1M, 채움 0.48, 객체 간격 100 B대)는 이 표의 가장 좋은 구석이다. 같은 객체로 살아 있는 수가 1.5M만 돼도 채움이 0.72가 된다. 빨라지는 지점은 벤치 하나의 모양이고, 느려지는 쪽은 블록 간격과 채움에 따라 한계가 없다.
  - (개정 13: 개정 11은 여기에 "1M 작은 객체의 제품 경로는 `BinnedAllocator`라 이득이 걸리는 제품 경로가 없다"를 근거로 더했다. 사실이 아니어서 뺐다 — 그때 제품 코드는 엔진에 `SystemAllocator`를 넘겼다(18절 머리. 개정 15에서 연결했다 — 18.10절). 버린 근거는 위의 군집 절벽 하나다.)
  - 그래서 예측할 수 있는 상수 배(churn 회수 3.0–3.2배 — 옛 기준(platform-counted). 계수 없는 플랫폼 기준으로는 3.8–4.3배)를 받고, 채움과 주소 모양에 기대는 절벽을 기본 할당자 밑에 두지 않는다. 원장은 주소를 통째로 섞는 기본 hash를 쓰고, 시험 `CoreAddressLedgerProbeLengthByAddressPattern`·`CoreAddressLedgerProbeLengthWhileTombstonesAccumulate`가 "주소 모양이 군집을 만들지 않는다"를 지킨다(주소 모양 8종과, 창을 밀며 삭제 칸을 쌓는 주기 내내 최장 탐사 64칸 이하 — 지역성 hash를 넣으면 잡힌다).
  - 군집 없이 지역성을 얻는 길은 페이지 디렉터리와 페이지별 표(2단 구조)다. `HashMap`을 버리고 전용 구조를 만드는 일이라 넣지 않았다. 지금 제품 코드가 엔진에 넘기는 것이 `SystemAllocator`이므로(18절 머리) 이 배수는 1M 규모의 제품 경로에 걸린다 — `BinnedAllocator` 연결 결정과 함께 다시 본다(연결하면 작은 블록은 원장을 지나지 않는다). (개정 15: 연결했다 — 18.10절. Sandbox 엔진 호스트에서는 작은 블록이 원장을 지나지 않는다. 아직 `SystemAllocator`를 쓰는 곳 — 게임 템플릿, Commandlet, 벤치 도구 — 에는 이 배수가 그대로 걸린다.)
  - `AddressLedger::Remove`는 `Find` 뒤 `Erase`로 두 번 찾는다. 둘째 조회는 방금 지난 칸이라 캐시에 있다. `HashMap`에 한 번에 꺼내는 API가 없고 따로 잰 이득이 없어 더하지 않았다.
- 고침(AL1b — AL1에서 발견): `HashMap::Erase`는 원소가 0개가 되면 묘비(삭제 칸)가 하나뿐이어도 모든 칸을 비웠다(`ClearSlots` — 용량에 비례). 살아 있는 수가 큰 고점 뒤에 0과 1 사이를 오가면 쌍마다 표 전체를 다시 써서 상환 O(1)이 깨진다(1M 고점이면 쌍마다 64 MB). 기본 할당자 밑에 이 절벽을 두지 않도록 원장을 감싸지 않고 컨테이너를 고친다.
  - 규칙: `Erase`로 0개가 됐을 때 삭제 칸이 칸 수의 1/4(`HashMap::EmptyClearDivisor`) 이상일 때만 비운다. 그 밖에는 삭제 칸을 그대로 두고 삽입의 정리(`SettleDeletedSlots`)에 맡긴다. `EraseIf`·`Clear`는 어차피 모든 칸을 보므로 전처럼 비운다. `HashSet`은 `HashMap` 위에 있어 같이 바뀐다.
  - 상환 근거: 삭제 칸은 `Erase` 한 번에 하나씩만 생기고(삭제 칸을 재사용하는 삽입은 줄인다) 비우기·재해시가 0으로 되돌린다. 그래서 비우기 앞에는 지난 비우기·재해시 뒤로 칸 수/4번 이상의 `Erase`가 있고, 비우기 한 번의 O(칸 수)는 그 `Erase`들에 나뉜다(`Erase`당 칸 넷). 불변식 (점유 + 삭제) ≤ 칸 수 × 3/4는 그대로다(`Erase`는 합을 바꾸지 않는다).
  - 바뀌는 관찰: 원소가 0개인 `HashMap`·`HashSet`에도 삭제 칸이 남을 수 있다(`DeletedSlotCount()` — 칸 수의 1/4 미만). 빈 Map의 `Compact`·`ShrinkSlack`은 전처럼 저장소를 돌려준다.
  - 비율: 분모를 키우면(1/8) 빈 Map에 남는 삭제 칸이 줄지만 비우기가 잦아지고, 줄이면(1/2) 비우기는 드물지만 빈 Map의 없는 키 조회가 삭제 칸을 더 지난다. 1/4이면 남는 삭제 칸의 부하가 1/4 미만이라 탐사가 짧다.
  - 버린 대안: 0개가 돼도 비우지 않기(빈 Map에 삭제 칸이 3/4 가까이 남아 다음 정리까지 없는 키 조회가 길다), 원장에 지워지지 않는 항목을 하나 두어 0개가 되지 않게 하기(우회 — 다른 `HashMap` 사용자는 절벽을 그대로 가진다).
  - 진단: `HashMap::LongestProbe()`(가장 멀리 밀려난 원소를 찾으며 보는 칸 수)와 `HashSet::SlotCount()`·`DeletedSlotCount()`를 더한다(시험과 원장 hash의 군집 측정에 쓴다).

### 18.7 버린 대안 (구조)

- 새 클래스(`SlabAllocator` 등)를 더하고 `BinnedAllocator`를 남기기: 같은 역할이 둘이 된다. 초기 단계라 하위호환을 두지 않는다.
- UE Binned3처럼 등급마다 주소 범위 예약(1GB × 등급 수): 인스턴스가 여러 개일 수 있어(시험·도구) 인스턴스당 수십 GB 예약은 39비트 주소 기기(Android)에서 부담이고, 한 등급이 범위를 다 쓰면 다른 등급으로 넘기는 처리가 따로 든다. 범위 하나를 slab 단위로 나눠 쓰면 등급 사이에 주소가 자유롭게 간다.
- UE Binned2처럼 64KB 정렬로 머리 읽기: 미소유 주소를 읽는다(계약 위반).
- UE Binned4처럼 주소 2단 표: 예약 범위가 없어도 되지만 표 두 줄을 읽는다. 인스턴스가 범위를 예약할 수 있으면 범위 비교 하나가 더 싸다.
- slab마다 소유 스레드와 원격 반환 목록(mimalloc 방식): 다른 스레드의 반환이 많을 때 유리하지만 slab 머리에 atomic 목록과 소유 이전 규칙이 든다. GC 회수는 소유 스레드가 반환하고, 스레드 사이 흐름은 이전 칸으로 충분한지 먼저 잰다(18.8절 gate A4).
- 오용 검출을 동시성 아래에서도 완전하게(18.4절 한계 2의 대안): (1) 반환마다 잠금·참조 계수·"반환 중" 표시(저장 뒤 읽기 순서를 보장하는 fence) — 빠른 경로에 잠금 접두 명령 하나(x64에서 20 cycle 안팎으로 추정했다. AL5에서 잰 값은 뜨거운 쌍에서 1.5–1.9 ns, 식은 회수 경로에서 반환당 2–4 ns다 — 실험 25.8절)가 든다. (2) slab을 풀 때 decommit 대신 접근 권한을 둔 채 내용만 버리고(Windows `DiscardVirtualMemory`, POSIX `madvise`), slab 표 칸에 세대 번호를 더해 머리·표지를 읽은 뒤 칸을 다시 대조한다(낙관적 읽기) — 빠른 경로 비용은 같은 줄의 비교 하나라 싸지만, Windows에서 commit charge가 최고 사용량에 머물고(물리 메모리만 돌아간다), slab 표가 slab당 4 byte가 되며, PlatformBase에 내용 버리기 연산이 더 필요하다. 오용 반환과 slab 풀기가 같은 순간에 겹치는 것은 이미 결함인 프로그램의 드문 경쟁이고 fault는 그 자리에서 드러나므로, 지금은 한계로 적고 넣지 않는다. commit charge가 문제되지 않는 것이 확인되거나 오용 검출을 동시성 아래에서도 보장해야 하면 (2)를 넣는다.
- 이전 칸 대신 등급 잠금만: 묶음 단위라 잠금 빈도는 낮지만(64번에 한 번) 병렬 표식의 큐 블록처럼 여러 스레드가 같은 등급을 주고받을 때 잠금이 겹친다. 8칸 비교 교환은 단순하고 UE가 쓰는 구조다.

### 18.8 gate (확정 — 개정 13, 실험 25.8절)

부하가 큰 기계라 같은 round의 상대값으로 본다(높은 우선순위, 번갈아).

| 번호 | 항목 | 목표 |
|---|---|---|
| A1 | 1M · 간선 4 생성+연결, churn 회수, 종료 회수(`SystemAllocator`) | 끝난다. 계수 없는 플랫폼 할당 대비 생성+연결 2배 이내, 종료 회수 3배 이내, churn 회수 4.5배 이내. **확정(개정 13, 실험 25.8절)**: 1.37–1.45배, 2.1–2.8배, 3.8–4.3배(세 실행의 범위). 할당자는 그대로이고 잣대가 바뀌어 다시 정했다 — 개정 11은 계수 장치가 섞인 옛 기준(platform-counted)으로 "2배·3.5배 이내"(1.30배·1.77배·3.2배)를 확정했었다. 상환 O(1)의 근거: 살아 있는 객체가 20K에서 1M으로 50배가 되는 동안 객체당 churn 회수는 160 → 346 ns(2.2배), 종료 회수는 214 → 397 ns(1.9배)다(실험 25.2절 — 선형 검색이던 전에는 20K에서 3,443 ns였고 1M은 끝나지 않았다). 늘어난 몫은 반환마다 원장 표(1M에서 64 MB)를 찌르는 캐시 미스 하나다. 지역성 hash로 줄여 봤으나 군집 때문에 버렸다(18.6절). 지금 제품 코드는 엔진에 `SystemAllocator`를 넘기므로 이 배수가 제품 경로에 걸린다(`BinnedAllocator` 연결은 결정 대기 — 18절 머리) |
| A2 | 같은 측정(`BinnedAllocator`) | 객체당 시간이 계수 없는 플랫폼 할당 이하(개정 13: "할당자 몫이 플랫폼의 절반 이하"는 계수 장치가 섞인 기준 위에서 정한 제안값이라 gate에서 뺐다. 몫은 측정 사실로 실험 25.8절에 적는다). **확정**: 생성 0.97배, churn 생성 0.96배, churn 회수 0.81배, 종료 회수 0.75배(몫은 churn 회수가 플랫폼의 64%, 종료 회수 44%) |
| A3 | 단일 스레드 할당·반환 쌍(16–256 B) | 중앙 40ns 이하. **확정(개정 15의 재측정 — 실험 25.10절)**: 7.2–7.4 ns |
| A4 | 한 스레드가 할당하고 다른 스레드가 반환(64 B) | 쌍당 시간이 단일 스레드의 3배 이내(잠금 붕괴 없음). **확정(개정 15의 재측정)**: 2.2–2.6배 |
| A5 | 8 스레드 동시 할당·반환 처리량 | 1 스레드의 4배 이상. **경계(개정 15의 재측정)**: round별 3.6–6.9배, 중앙 4.0배 — 다섯 round 가운데 둘이 4배 미만이다(다른 세션의 부하가 30% 안팎인 기계). 비교 교환을 뺀 뒤 1 스레드 값이 104 → 120 M쌍/s로 오른 만큼 배수는 내려갔고, 8 스레드의 처리량 자체는 416–637 M쌍/s로 비교 교환 때(359–510)보다 높다. 플랫폼 할당은 같은 round에서 4.0–4.7배였다 |
| A6 | 1M 객체 저장소의 메모리 | 요청 byte 대비 추가 20% 이하(등급 올림 + slab 머리·꼬리 — 벤치 객체 크기에서의 값을 함께 적는다). **확정**: +6.8%(churn 뒤 +9.7%) — 벤치의 요청은 128·64·48 B가 바이트의 75%다 |
| A7 | GC churn 회수(실험 24절 — 객체당 약 235–250ns) | `BinnedAllocator`에서 플랫폼 할당보다 줄어든다. **충족(개정 15의 재측정 — 실험 25.10절)**: 다섯 round 모두 낮다(중앙값 151.5 대 170.0 ns/객체 — 0.89배, round별 0.85–0.97배). 개정 13의 "같은 수준"은 비교 교환 구현으로 잰 값이었다(163.3–175.3 대 168.9–197.4) |

한계로 남길 것: 실제 엔진의 크기 분포 미측정(상한 8192·등급 표), `PoolAllocator` 반환의 chunk 검색(그대로 — 이 할당자는 더는 쓰지 않는다), 다른 스레드 캐시의 Trim(개정 16에서 쉰다고 표시한 스레드의 캐시는 비우게 했다 — 18.11.4절. 표시하지 않는 스레드가 쥐는 상한은 스레드 × 인스턴스당 2,361,856 byte: 아래 "쉬는 스레드의 캐시"), 겹친 중복 반환(둘 다 성공으로 돌아올 수 있고 두 번째로 내줄 때 Fatal — 그 사이에 다시 반환되거나 slab으로 돌아가면 놓치고, slab 계수가 어긋날 수 있다: 18.4절), 오용 검출의 동시성 한계(18.4절 한계 2 — 그 slab의 decommit·재사용과 겹치면 fault 또는 미검출), 반환 표지의 값 일치(2^-63 — 18.4절 한계 1), 예약하지 못한 기기에서는 모든 요청이 큰 경로(느리지만 맞다).

쉬는 스레드의 캐시(AL5 — 위 한계를 판단할 수치. 개정 16: `Trim`은 `TrimMemory`가 됐고 쉰다고 표시한 스레드의 캐시도 비운다 — 아래는 표시하지 않는 스레드에 그대로 맞다): `Trim`은 호출 스레드의 캐시만 비우므로, 한 번 크게 쓰고 쉬는 스레드는 캐시를 쥔 채로 있다(그 스레드가 끝나거나 그 스레드가 `Trim`을 부를 때까지).

- 상한을 공개 상수 `BinnedAllocator::MaxThreadCacheBytes`로 둔다: 등급마다 한도만큼 찬 묶음 둘(부분·가득)의 합 = 2,361,856 byte(약 2.25 MiB), 블록 3,120개다(1024 이하 등급 20개는 묶음 64개, 그 위 12개는 64KB를 넘지 않는 개수). 스레드 하나 × 인스턴스 하나의 값이다. 상수는 등급 표에서 계산한 값과 `static_assert`로 묶고(표가 바뀌면 컴파일이 멈춘다), 시험이 모든 등급을 한도까지 채운 쉬는 스레드에서 이 값을 본다.
- 붙들리는 commit은 이보다 클 수 있다: 캐시의 블록이 든 slab은 "쓰는 블록"이 있어 풀리지 않는다. 가장 나쁜 경우는 캐시의 블록마다 다른 slab의 마지막 쓰는 블록인 때로 3,120 × 64 KiB = 195 MiB다(반환이 slab마다 하나씩 흩어진 경우 — 회수는 보통 주소가 모여 있어 여기서 멀다).
- UE 5.8의 처리(`Public/HAL/MallocBinnedCommonUtils.h` 143–189, `MallocBinnedCommon.h` 809–821): `Trim`이 trim epoch를 올리고, 스레드는 잠들 때(`MarkTLSCachesAsUnusedOnCurrentThread`) epoch가 바뀌었으면 자기 캐시를 비운다. 데스크톱이 아닌 플랫폼은 `Trim`을 부른 스레드가 등록된 캐시를 `TryLock`으로 직접 비우고, 데스크톱은 named 스레드에만 비우기를 방송하고 task 스레드를 건너뛴다("too slow and they don't have much memory").
- 같은 것을 넣으려면 캐시마다 잠금(쓰는 구간 표시)과 scheduler의 "잠듦·깸" 알림이 allocator까지 내려와야 한다(Task → Core 할당자 통지 통로). 지금은 넣지 않는다 — 상한이 스레드당 2.25 MiB로 정해져 있고 worker 스레드 수는 수십이다. 메모리 압박 대응(Trim을 실제로 부르는 쪽)이 생길 때 함께 넣는다.

### 18.9 단계와 시험 계획

| 단계 | 내용 |
|---|---|
| AL0 | `GcBench --allocators`를 1M·할당자 선택·bump 바닥으로 넓히고 지금 상태를 잰다(System·Binned는 끝나는 규모까지) |
| AL1 | 공용 주소 원장 `detail::AddressLedger`(Core `HashMap` + 추적 없는 내부 allocator)와 `SystemAllocator`의 원장 교체 |
| AL2 | `BinnedAllocator` 다시 만들기 1: 예약 범위(실패 때 축소·예약 없음)·slab 표·등급·소유 판정 다섯 단계와 표지 불변식·큰 할당(스레드 캐시 없이 등급 잠금) |
| AL3 | `platform::ThreadExitSlot`(Windows FLS callback, POSIX pthread key 소멸자)과 그 시험, 스레드 캐시·이전 칸(가득 묶음만)·스레드 종료 hook·인스턴스 파괴 |
| AL4 | `Trim`·통계(`ReservedBytes`·`fallbackAllocations`)·ASan(`ROCKS_NO_SANITIZE_ADDRESS` 내부 접근)·범위 소진 경로 |
| AL5 | 측정(18.8절)·GC churn 재측정·문서(실험 25절, 0절 갱신) |
| AL6 | 제품 연결(18.10절 — 개정 15): Launch의 범용 할당자, Sandbox 엔진 호스트, `ThreadExitSlot`의 OS 오류 값 보존 |
| AL7 | 남은 연결과 Trim(18.11절 — 개정 16): 게임 템플릿·Commandlet, `Allocator::TrimMemory`·쉬는 표시, 쉬는 스레드 캐시 비우기, `WorkerScheduler`, 엔진의 호출 자리, 리뷰 후속 |

시험(단계마다 음성 대조와 함께):

- 계약: 0 byte, 잘못된 정렬, 등급 경계 크기(16·128·129·8192·8193), 정렬 32·64의 등급 올림과 128의 큰 경로, 미소유(다른 인스턴스·스택·범위 안의 안 쓰는 slab)·중간 주소·중복 반환이 `NotFound`이고 상태가 그대로인 것, 남은 할당 파괴 Fatal(음성 대조 프로그램 — 지금의 `allocator-binned-invalid-N`을 새 계약으로).
- 표지 불변식(F1): 새 인스턴스에서 한 블록을 할당하고 `주소 + 블록 크기`(떼어 캐시에 든, 내준 적 없는 블록)를 반환하면 `NotFound`이고, 이어지는 할당들이 서로 다른 주소를 받는다. 뗀 적 없는 블록(bump 뒤)의 반환도 `NotFound`다. 보관소에서 다시 꺼낸 slab(옛 표지가 남은 slab)에서도 같다.
- 오용 시험은 그 slab의 decommit·재사용과 경쟁하지 않게 만든다(F2 — 단일 스레드, 또는 그 slab에 살아 있는 블록을 남겨 둔다).
- 예약 실패(F4): 예약 크기를 주입해(시험용 설정) 축소 재시도와 "예약 없음" 상태를 만든다 — 모든 요청이 큰 경로로 가고 통계가 맞는다.
- `platform::ThreadExitSlot`(F3): 값을 둔 스레드가 끝나면 그 스레드에서 정리 함수가 한 번 불린다(명시 gate로 순서를 만든다), 값이 null인 스레드는 불리지 않는다, `platform::Thread`가 아닌 OS 스레드에서도 불린다(Windows `CreateThread`·POSIX `pthread_create`로 만든 스레드).
- 이전 칸(F7): 이전 칸에서 받은 묶음의 블록 수가 등급 한도와 같다, 부분 묶음은 스레드 종료·`Trim` 때 이전 칸이 아니라 slab으로 간다(이전 칸 8칸이 빈 채 남는다).
- 모델: 무작위 할당·반환을 그림자 표(주소 → 크기·내용 무늬)와 대조한다 — 겹침 없음, 내용 보존, 통계 일치, 끝에 `Trim` 뒤 commit된 slab 0.
- 스레드: 한 스레드가 할당하고 다른 스레드가 반환(명시 gate로 순서를 만든다), 스레드 끝에 캐시가 비워지는 것(끝난 뒤 통계·`Trim`), 인스턴스 파괴 뒤에도 살아 있는 스레드가 다른 인스턴스를 쓰는 것, 이전 칸이 찬 경로·빈 경로를 결정적으로 지나게 하는 크기.
- 실패 주입: slab commit 실패(범위를 작게 잡아 소진 → 큰 경로), 상위 allocator 실패(`NFailAllocator`), hash 원장 늘리기 실패.
- 장기: `GcSoak`·클러스터 smoke를 `BinnedAllocator` 위에서 돌린다(지금은 `SystemAllocator`·벤치 할당자).
- 음성 대조 후보: refill에서 뗀 블록에 표지를 세우지 않음(F1), 반환 표지를 할당 때 지우지 않음, 뗀 블록 수 검사 생략, 부분 묶음을 이전 칸에 넣음(F7), 스레드 종료 hook이 죽은 인스턴스의 캐시를 건너뛰지 않음, 예약 실패 때 축소하지 않음, slab 표를 0으로 돌리지 않음, 이전 칸 넣기의 비교 교환을 단순 대입으로, 스레드 끝 비우기 생략, 등급 올림에서 정렬 배수 검사 생략. 원장(`HashMap`이라 "뒤 칸 당기기"는 없다 — 개정 9): 반환 때 원장에서 지우지 않음, 원장 넣기 실패를 무시하고 게시, 중복 주소 넣기를 덮어씀, 원장 넣기 실패 때 블록을 돌려주지 않음.

### 18.10 제품 연결 (AL6, 개정 15)

사용자 지시(2026-11-20 "엔진에 BinnedAllocator 연결해")로 11절 결정 8을 확정하고 한 묶음으로 연결한다. 새 탐색·보강은 넣지 않는다.

- **소유는 실행 진입점(Launch)이다.** UE가 프로세스의 기본 할당자를 Binned로 두는 것과 같은 자리다. `ApplicationRunner`가 지금의 `SystemAllocator` 위에 `BinnedAllocator`를 하나 두고, `ApplicationContext::DefaultAllocator()`가 그것을 프로세스의 범용 할당자로 내준다. 이름이 `Allocator()`가 아닌 까닭은 멤버 함수 이름이 타입 `Allocator`를 가리지 않게 하려는 것이다. `SystemAllocator()`는 그대로 둔다(상위 할당자, 도구 로그가 계속 쓴다).
- **고를 수 있다.** `ApplicationSettings::defaultAllocator`(`DefaultAllocatorKind::Binned`가 기본, `System`)를 `configure`가 바꿀 수 있다. `System`이면 `DefaultAllocator()`가 `SystemAllocator()`와 같은 객체를 돌려준다 — 문제를 가를 때 쓰는 비교 수단이다(UE의 할당자 선택 스위치 대응). 명령줄 옵션은 이번에 넣지 않는다.
- **상위는 그 `SystemAllocator`다.** 새 공개 클래스를 만들지 않는다. 한계: 큰 블록(8 KB 초과)은 원장을 두 번 지난다(`BinnedAllocator`의 큰 할당 원장과 `SystemAllocator`의 원장 — 큰 블록마다 hash 넣기·지우기가 한 번씩 더 든다). 큰 블록은 드물다(1M 그래프에서 살아 있는 할당 290만 가운데 1.6만 — 실험 25.8절).
- **수명 순서.** 설정 적용(`configure` 뒤) → 범용 할당자 생성 → 도구 로그 시작 → 본문 → 도구 로그 정지 → `BinnedAllocator` 파괴 → (실행 상태 소멸 때) `SystemAllocator` 파괴. 남은 할당 계약은 지금과 같다: 본문이 범용 할당자의 할당을 남긴 채 돌아오면 `BinnedAllocator` 파괴가 Fatal `BinnedAllocator destroyed with live allocations`다. 계약은 `SystemAllocator`를 쓸 때와 같고 자리는 다르다(리뷰 지적): `BinnedAllocator`의 Fatal은 보호 프레임 안의 정리 단계에서 나므로 크래시 수집이 걸려 있으면 보고가 남고, `SystemAllocator`의 Fatal은 크래시 수집을 뗀 뒤 실행 상태가 소멸할 때 나서 stderr 한 줄만 남는다. 본문이 만든 스레드는 본문이 돌아오기 전에 끝나 있어야 한다는 것도 같다(살아 있더라도 파괴가 그 스레드의 캐시까지 회수하지만, 그 스레드가 그 뒤에 할당자를 부르면 안 된다).
- **`RequestExitWithoutStaticTeardown` 경로.** 정리(도구 로그, 종료 요청 처리기, 크래시 수집 해제) 뒤 `ExitProcessWithoutTeardown`으로 끝나는 경로에서는 `BinnedAllocator`를 파괴하지 않는다(`SystemAllocator`를 파괴하지 않는 것과 같다 — 이 경로는 멈춘 스레드가 할당을 쥔 채일 수 있어 남은 할당 Fatal을 내면 안 된다). 내장 진입은 이 요청을 무시하므로 언제나 파괴한다.
- **내장 진입.** `RunEmbeddedApplication` 실행마다 인스턴스 하나다(실행 상태와 수명이 같다).
- **비용.** 인스턴스 하나의 예약(주소 범위 16 GiB — Android 4 GiB, commit 아님)과 slab 표·비트맵(약 290 KB의 플랫폼 할당)이 Launch를 쓰는 모든 실행 파일에 든다(본문이 범용 할당자를 쓰지 않아도). 스레드 캐시는 그 할당자를 쓴 스레드마다 최대 2,361,856 byte(약 2.25 MiB)다(18.8절 "쉬는 스레드의 캐시").
- **Sandbox.** 엔진 호스트(`EngineHost.cpp`의 실행 경로 둘)와 self-test가 `context.DefaultAllocator()`를 엔진·모듈 로더·LogHost에 넘긴다. 같은 파일의 지역 `SystemAllocator`는 무엇에 쓰이는지 보고 엔진·모듈 로더로 가는 것만 바꾼다(구현에서 확인해 실험 문서에 적는다).
- **넣지 않는 것(후속).** `Trim`을 부르는 시점(레벨을 내린 뒤, 메모리 압박), 다른 스레드 캐시의 `Trim`, Commandlet·벤치 도구와 게임 템플릿의 연결, 명령줄 옵션.
- **시험.** Launch probe(자식 프로세스): 기본이 `BinnedAllocator`인지(범용 할당자가 시스템 할당자와 다른 객체이고, 작은 요청은 시스템 할당자의 살아 있는 할당 수를 늘리지 않으며 큰 요청은 하나 늘린다), `System` 설정이면 같은 객체인지, 본문이 할당을 남기면 Fatal인지, `RequestExitWithoutStaticTeardown`이면 남겨도 Fatal 없이 끝나는지. EngineTests: 엔진을 `BinnedAllocator` 위에서 초기화·프레임·GC·종료(끝에 남은 할당 0, `Trim` 뒤 commit 0). `ThreadExitSlot`: `Get`·`Set`이 OS 오류 값을 바꾸지 않는다. 음성 대조: Launch가 설정과 무관하게 시스템 할당자를 내줌, 설정을 무시하고 언제나 `BinnedAllocator`를 만듦.
- **측정(실험 25.10절).** `GcBench`의 일반 그래프 시나리오(1M · 간선 4)를 `SystemAllocator`와 `BinnedAllocator` 위에서 같은 round에 번갈아 쟀다(5 round의 중앙값): 생성 824 → 441 ms(0.54배), 연결 1,106 → 818 ms(0.74배), churn 생성+연결 175 → 127 ms(0.72배), churn 회수 414 → 152 ns/객체(0.37배), churn Collect 전체 107.5 → 81.5 ms(0.76배), 회수를 미뤘을 때 2 ms 예산의 프레임 수 24 → 10. 종료 회수는 이 시나리오가 재지 않아 할당자 비교의 값을 쓴다(383 → 139 ns/객체 — 25.8절).
- **구현에서 정한 것.** Sandbox `EngineHost.cpp`의 지역 `SystemAllocator` 둘은 그대로 뒀다 — 하나는 self-test 단계가 trace 파일을 읽어 들이는 버퍼이고 다른 하나는 self-test의 기기 특성 확인(`CheckDeviceTraitsFromCaps`)이라 엔진·모듈 로더로 가지 않는다. 시험 지원에 OS 오류 값을 쓰고 읽는 `test::SetSystemError`·`SystemError`를 더했다. 그 소스를 더하자 FoundationTests의 유니티 묶음 경계가 옮겨져 `WindowsComRefTests.cpp`(Windows COM 헤더의 `small` 같은 매크로)가 Compression 시험과 한 묶음의 앞에 놓여 컴파일이 깨졌다 — 그 시험 소스를 유니티 단독 소스로 뺐다(`FoundationTests.Build.cs`).

### 18.11 남은 연결과 Trim (AL7, 개정 16)

사용자 지시(2026-11-20 "남은작업 진행")로 18.10절이 후속으로 남긴 것을 한 묶음으로 넣는다. 범위는 이 절로 고정하고 여기 없는 탐색·보강은 하지 않는다.

#### 18.11.1 나머지 연결

- **게임 템플릿.** `Templates/Game/Foundation/Source/Main.cpp`가 `context.DefaultAllocator()`를 쓴다. 엔진 호스트 종류의 본보기라서(Launch 설계 16절 "새 실행 프로그램은 템플릿을 따른다") 범용 할당자를 쓰는 모습이어야 한다.
- **Commandlet(`Programs/Commandlet`).** 진입점이 `context.DefaultAllocator()`를 `commandlet_host::Run`에 넘기고, 명령은 등록할 때 건 문맥 포인터로 그 할당자를 받는다. 지역 `SystemAllocator` 자리별 결정:

  | 자리 | 먹이는 것 | 결정 |
  |---|---|---|
  | `Run`의 설정 할당자(`HostSettings`) | 설정 문맥 — 엔진 호스트와 같은 부트 | 범용 할당자. 엔진 호스트와 같은 할당자 위에서 같은 부트를 돈다 |
  | `trace-report` | trace 파일 전체, `TraceModel`, JSON 출력 | 범용 할당자. 명령의 실제 일이고 입력 크기에 비례한다 |
  | `log decode`·`seal`·`pack`·`view` | 키·파일·복호·압축 풀기 버퍼, `JsonDocument` | 범용 할당자. 같은 까닭이다 |
  | `core-check` | `SystemAllocator`의 할당·반환 수명 자체 | 그대로 둔다. 그 명령이 점검하는 대상이 `SystemAllocator`다 |

  `log keygen`은 할당자를 쓰지 않는다. 명령이 할당을 남긴 채 돌아오면 Launch의 정리에서 Fatal이다(지역 `SystemAllocator`의 파괴 Fatal과 계약은 같고 자리가 다르다 — 18.10절).
- **벤치 도구는 바꾸지 않는다(결정).** CompressionBench·CryptoBench·LogBench·GcBench는 지역 할당자를 그대로 쓴다. 측정 도구는 무엇을 재는지에 맞춰 할당자를 일부러 고른다 — GcBench는 `--allocator-kind`로 고르고, 나머지는 할당자 몫을 재지 않는 측정이다.

#### 18.11.2 UE 5.8 확인 (Trim)

로컬 클론(5.8.4)에서 읽은 것이다. 경로는 `Engine/Source/Runtime` 아래다.

- **부르는 시점.** `FMemory::Trim(bool bTrimThreadCaches = true)`(`Core/Public/HAL/UnrealMemory.h:229`, `Core/Private/HAL/UnrealMemory.cpp:559`)은 trim delegate를 알린 뒤 `GMalloc->Trim`을 부른다. `FMalloc::Trim`의 기본 구현은 무동작이다(`Core/Public/HAL/MemoryBase.h:166`). 엔진이 부르는 곳은 GC의 purge가 끝나는 자리 셋뿐이다(`CoreUObject/Private/UObject/GarbageCollection.cpp`).
  - `:5933` `PostCollectGarbageImpl` — 전체 purge(`bPerformFullPurge`) 직후.
  - `:4822` `IncrementalPurgeGarbage` — 증분 purge가 끝난 다음 호출("if the purge was completed last tick, perform the trim to finalize this incremental purge").
  - `:5772` `PreCollectGarbageImpl` — 새 GC가 남은 증분 purge를 마저 끝낸 뒤(전체 purge가 아닐 때).
  - 맵 로드는 `UEngine::LoadMap`(`Engine/Private/UnrealEngine.cpp:16078`)이 `UEngine::TrimMemory`(`:16716`)를 부르고, 그것이 전체 purge GC(`CollectGarbage(..., true)` — `:16729`)를 불러 `:5933`으로 간다. World 파괴에 직접 붙은 Trim은 없다.
- **쉬는 스레드의 캐시.** 스레드 캐시(`FPerThreadFreeBlockLists` — `Core/Public/HAL/MallocBinnedCommon.h:500`)마다 mutex가 있고 소유 스레드가 쓰는 동안 쥔다(`bLockedByOwnerThread` — `:642`).
  - worker는 막히는 대기 앞뒤에서 표시한다: `FMemory::MarkTLSCachesAsUnusedOnCurrentThread()` → 대기 → `MarkTLSCachesAsUsedOnCurrentThread()`(`Core/Private/Async/Fundamental/WaitingQueue.cpp:119–125`). 앞의 것이 mutex를 풀고(`UnlockTLS` — `MallocBinnedCommon.h:535`) 뒤의 것이 다시 잠근다(`LockTLS` — `:544`, 비우는 쪽이 쥐고 있으면 기다린다).
  - 빠른 경로의 `Get()`(`:504`)은 `bLockedByOwnerThread`가 거짓이면 null을 돌려준다 — 쉰다고 표시한 사이의 할당(대기 구현이 할당할 때)은 캐시 없이 간다.
  - `Trim`(`Core/Public/HAL/MallocBinnedCommonUtils.h:143`)은 epoch(`MemoryTrimEpoch`)를 올린다. 비데스크톱에서는 등록된 캐시를 돌며 `TryLock`에 성공한 것(쉬는 스레드의 것)을 그 자리에서 비운다(`:152–170`). 데스크톱에서는 그 순회를 건너뛴다("we may have too many threads and this could cause some hitches"). 이어서 named thread(게임·렌더·RHI 등)에 broadcast를 보내 각자 자기 캐시를 비우게 한다(`:172–188` — 비데스크톱은 task thread에도 보낸다). 그래서 데스크톱의 task worker만 Trim 때 비워지지 않고, 깨어 있던 worker가 다음에 쉴 때 epoch가 올랐으면 스스로 비운다(`MarkTLSCachesAsUnusedOnCurrentThread` → `FlushCurrentThreadCacheInternal(bNewEpochOnly)` — `MallocBinnedCommon.h:809`). `FMemory::Trim`은 주 스레드에서만 부른다(`UnrealMemory.h:226`).

#### 18.11.3 `Allocator` 인터페이스

`Allocator`만 가진 코드(엔진, `WorkerScheduler`)가 부를 수 있게 기본 인터페이스에 가상 함수 셋을 더한다(UE `FMalloc::Trim`·`MarkTLSCachesAs…`와 같은 구조). 기본 구현은 모두 무동작이다.

- `void TrimMemory() noexcept` — 쥐고만 있고 쓰지 않는 메모리를 OS·상위 allocator에 돌려준다. 살아 있는 할당은 건드리지 않는다. 어느 스레드에서나 부른다.
- `void MarkThreadIdle() noexcept` — 호출 스레드가 지금부터 이 allocator를 쓰지 않고 쉰다고 알린다. 막히지 않고 호출자 쪽으로 재진입하지 않는다(호출자가 자기 잠금을 쥔 채 부를 수 있다).
- `void MarkThreadActive() noexcept` — 다시 쓴다고 알린다. 그 스레드의 캐시를 다른 스레드의 `TrimMemory`가 비우는 중이면 끝날 때까지 막힐 수 있다.
- 표시는 세지 않는 상태다: 쉰다고 두 번 알려도 한 번 깨우면 쓰는 중이다. 표시하지 않은 스레드는 언제나 쓰는 중이다.
- **이름.** 가상 함수는 `Trim`이 아니라 `TrimMemory`다. `ArenaAllocator`·`PoolAllocator`·`StackAllocator`에 이미 `Result<void> Trim()`(살아 있는 할당이 없을 때만 chunk를 돌려주고 아니면 `Busy`, 단일 스레드 계약)이 있어서 같은 이름의 가상 함수를 둘 수 없고, 그 셋의 고유 계약(표시 무효화·`Busy`)을 `Allocator&`로 새게 하지 않는다. `BinnedAllocator::Trim()`은 재정의 `TrimMemory()`로 개명한다(이름 둘을 두지 않는다 — 이 문서와 실험 문서의 앞 절들은 그때의 이름 `Trim`으로 적혀 있다).
- **래퍼.** 상위 allocator 하나에 `Allocate`·`Deallocate`를 그대로 넘기는 래퍼는 이 셋도 그대로 넘긴다: Core `BudgetAllocator`·`DebugAllocator`, TestSupport `NFailAllocator`, 모듈 안의 adapter(ObjectSerialization `BudgetAllocator`·`ObservationAllocator`, SpatialIndex `CountingAllocator`). 래퍼를 끼우는 순간 Trim과 쉬는 표시가 조용히 사라지지 않게 한다. 상위가 없거나 넘기는 것이 뜻에 맞지 않는 allocator(`ArenaAllocator`·`PoolAllocator`·`StackAllocator`·`ExternalAllocator`·`SystemAllocator`)는 기본 무동작 그대로다.

#### 18.11.4 `BinnedAllocator`

- **캐시 상태.** 스레드 캐시마다 상태 하나를 둔다(원자 word, 뜨거운 등급 칸과 다른 캐시 줄): 쓰는 중 · 쉬는 중 · 비우는 중. 쓰는 중 ↔ 쉬는 중은 소유 스레드만, 쉬는 중 ↔ 비우는 중은 등록부 잠금을 쥔 스레드만 바꾼다. 잠금 없이 다투는 전이(쓰는 중 → 쉬는 중, 쉬는 중 → 쓰는 중, 쉬는 중 → 비우는 중)는 비교 교환이고, 등록부 잠금을 쥔 채 하는 되돌림(비우는 중 → 쉬는 중)과 비우기를 기다린 소유 스레드의 "쓰는 중" 쓰기는 단순 대입이다(그 잠금을 쥔 동안 다른 쓰는 쪽이 없다).
- **`MarkThreadIdle`.** 호출 스레드의 이 인스턴스 캐시(없으면 아무것도 안 한다)를 쓰는 중 → 쉬는 중으로 바꾸고, 스레드 기록의 "마지막에 쓴 인스턴스" 지름길이 이 인스턴스면 지운다. 잠금을 잡지 않는다.
- **`TrimMemory`.** 순서: 호출 스레드의 캐시가 쓰는 중이면 비운다 → 등록부 잠금 아래에서 이 인스턴스의 캐시 목록을 돌며 쉬는 중 → 비우는 중으로 바꾼 캐시의 묶음을 slab으로 돌려주고 쉬는 중으로 되돌린다 → 이전 칸을 비운다 → 보관한 빈 slab을 decommit한다. 쓰는 중인 다른 스레드의 캐시는 건드리지 않는다. 계수(내준·돌려받은 누적 수)는 캐시에 그대로 둔다 — 블록만 옮긴다.
- **`MarkThreadActive`.** 쉬는 중 → 쓰는 중 비교 교환이 되면 끝이다(잠금 없음). 비우는 중이면 등록부 잠금을 얻어(비우는 쪽이 그 잠금을 쥐고 있으므로 끝날 때까지 기다린다) 쓰는 중으로 둔다.
- **쉬는 중의 할당·반환은 안전하다(빠른 경로 비용 0).** 빠른 경로는 "마지막에 쓴 인스턴스" 지름길만 본다. 쉰다고 표시할 때 그 지름길을 지우므로 그 뒤의 호출은 느린 경로(`FindOrCreateCache`)로 가고, 느린 경로는 캐시가 쓰는 중이 아니면 캐시 없이 등급 잠금 경로로 보낸다(지름길도 다시 세우지 않는다). 지름길이 애초에 다른 인스턴스를 가리키고 있었어도 같다 — 이 인스턴스의 지름길은 느린 경로만 세운다. 그래서 대기 구현이 할당해도 비우는 쪽과 경쟁하지 않는다(UE `Get()`의 `bLockedByOwnerThread` 검사와 같은 결과다. 검사가 느린 경로에만 있다).
- **잠금 순서**는 그대로다(등록부 → 등급 → 범위). `MarkThreadActive`가 등록부 잠금을 잡을 수 있으므로, 그것을 부르는 쪽이 쥔 잠금은 등록부 잠금의 앞 순서가 된다 — 등록부 잠금을 쥔 채 allocator 밖으로 나가는 경로는 없다.
- **비용.** 빠른 경로(할당·반환)는 바뀌지 않는다(쌍 벤치로 확인한다). 깨어난 뒤의 첫 호출만 느린 경로 한 번이다. `TrimMemory`는 등록부 잠금을 쥔 채 쉬는 캐시를 모두 비우므로 그동안 스레드의 캐시 만들기·종료 hook·`Stats`·깨어나는 스레드가 기다린다(캐시 하나에 블록 최대 3,120개).
- **한계.**
  - 표시하지 않고 쉬는 스레드(엔진의 다른 스레드 — 로그 출력, 비동기 파일, trace 등)와 Trim 때 깨어 있던 스레드의 캐시는 그대로 남는다(18.8절의 상한 그대로). UE의 epoch(Trim 때 깨어 있던 스레드가 다음에 쉴 때 스스로 비움)는 넣지 않았다(후속).
  - 한 스레드가 여러 인스턴스의 캐시를 가지면 표시한 인스턴스의 캐시만 쉰다.
  - UE는 Trim 때 named thread(게임·렌더·RHI 등)에 broadcast를 보내 각자 캐시를 비우게 한다. 여기서는 호출 스레드와 쉰다고 표시한 스레드만 비운다(리뷰 지적).
  - UE 데스크톱과 다르다: UE는 데스크톱에서 쉬는 task worker의 캐시를 Trim이 직접 비우지 않는다(스레드 수가 많아 hitch를 우려). 여기서는 플랫폼을 가르지 않고 직접 비운다 — 표시하는 스레드가 `WorkerScheduler`의 worker뿐이고 캐시 상한이 2.25 MiB라 비용이 작다(측정은 실험 25.11절).

#### 18.11.5 `WorkerScheduler`

- worker는 작업이 없어 막히는 대기(`changed.Wait(gate)`) 바로 앞에서 `MarkThreadIdle`을, 깨어난 바로 뒤에 `MarkThreadActive`를 scheduler가 받은 allocator에 부른다.
- 앞의 표시는 gate를 쥔 채 부른다(풀었다 잡으면 그 사이에 들어온 작업의 신호를 놓친다 — `MarkThreadIdle`이 막히지 않는 계약인 까닭). 뒤의 표시는 gate를 풀고 부른 뒤 다시 잡는다: 비우는 중이라 막혀도 그동안 다른 스레드의 `Submit`을 붙잡지 않고, 깨어난 worker는 어차피 gate 아래에서 작업을 다시 훑는다.
- 한계: scheduler가 받은 allocator에만 표시한다. worker가 작업 본문에서 쓴 다른 인스턴스의 캐시는 표시되지 않는다. 작업 본문 안에서 막히는 대기(표식 worker의 gate 등)도 표시하지 않는다.

#### 18.11.6 엔진의 호출 자리

- **자리는 둘이다.**
  1. World: 전체 수집의 회수가 끝난 프레임. `World::RunFullCollection`은 회수를 미루므로(17.2절) "끝난 프레임"은 미룬 회수가 다 빠진 GC 단계다. 회수할 것이 없었으면 그 수집 직후다. UE `GarbageCollection.cpp:4822`·`:5933`에 대응한다.
  2. Engine: World를 파괴한 뒤(`Engine::DestroyWorldNow`가 World 객체까지 해제한 다음). UE의 맵 로드 Trim(`UEngine::TrimMemory`)에 대응한다.
- **증분 cycle이 끝날 때는 부르지 않는다.** UE는 purge가 끝날 때마다 부르지만 UE의 GC는 주기(기본 약 60초)마다 한 번이고, Rocks의 증분 cycle은 World 파괴가 있으면 프레임마다 이어질 수 있어 그보다 잦다. Trim은 호출 스레드의 캐시를 비우고 빈 slab을 decommit하므로 잦으면 다시 commit하는 비용이 든다.
- **끄는 설정은 하나다**: `WorldGcSchedule::trimMemory`(기본 켬). World의 자리는 자기 일정을, Engine의 자리는 파괴하는 World의 일정을 본다.
- 한계: 기본 방식(`WorldGcMode::Incremental`)에서는 전체 수집을 요청하거나 World를 파괴하기 전까지 엔진이 Trim을 부르지 않는다. 호스트가 `Allocator::TrimMemory()`를 직접 부를 수 있다. lease 같은 보유 때문에 회수가 지연된 객체는 1번 자리의 Trim을 미루지 않는다 — 첫 회수가 그 객체를 지연 목록으로 옮기고 `HasPendingReclaim`은 그 목록을 세지 않는다. 그 객체가 나중에 회수될 때는 다시 부르지 않는다(리뷰 지적 — 처음에는 반대로 적었다). 메모리 압박 신호에 따른 Trim은 넣지 않는다(신호를 내는 플랫폼 계층이 없다 — 17.3절).

#### 18.11.7 리뷰 후속 가운데 넣는 것

- 실험 25.9절: I1 — slab으로 블록을 돌려줄 때 그 slab의 "쓰는 블록 수"가 이미 0이면 Fatal `BinnedAllocator slab block count underflow`(겹친 중복 반환이 slab 경로로 간 경우의 일부를 잡는다. 표지 값을 나누는 쪽은 넣지 않는다). I2 — 등급 캐시와 이전 칸의 정렬을 `CacheLineSize`로. I4 — `TrimMemory` 공개 주석. I6 가운데 스레드 기록의 항목 표 늘리기 시험(`MaxThreadExitSlots` 초과 시험은 칸을 영구히 쓰므로 넣지 않는다). I7 — 예약 크기가 줄어드는 환경에서 실패하는 단언 고치기. I8 — `_Exit` 표기 통일.
- 실험 25.10절: 시험만 — I2 내장 진입의 파괴 경로, I3 할당자 수준의 OS 오류 값(새 스레드의 첫 `Allocate`·`Deallocate`), I8 Fatal 종료 코드 비교.
- 남기는 것(후속): 25.9절 I3(종료 hook의 등록부 잠금 범위), 25.10절 I5(`System` 종류의 남은 할당 검사 자리)·I6(예약을 첫 호출 때 만들기), UE식 epoch, 표시하지 않는 엔진 스레드의 캐시, 메모리 압박 신호.

#### 18.11.8 시험과 측정

- 할당자: 쉰다고 표시한 스레드의 캐시가 다른 스레드의 `TrimMemory`로 비는 것(명시 gate — 캐시 byte와 commit), 쓰는 중인 스레드의 캐시는 그대로인 것, 비우는 도중에 깨어난 소유 스레드가 기다렸다가 정상 동작하는 것(시험 통로로 비우는 쪽을 멈춰 세운다), 쉬는 중의 할당·반환이 캐시 없이 가는 것(지름길이 다른 인스턴스를 가리킬 때 포함), 래퍼 하나가 셋을 넘기는 것.
- `WorkerScheduler`: worker가 대기 앞뒤에서 표시하는 것(세는 allocator), 작업 본문은 쓰는 중 상태에서 도는 것.
- 엔진: World의 전체 수집 뒤 회수가 끝나면 한 번, World 파괴 뒤 한 번, 설정을 끄면 0번, 증분 cycle로는 0번.
- 음성 대조 셋: 쉬는 표시를 무시하고 쓰는 중 캐시까지 비움, 깨어날 때 기다리지 않음, 엔진이 Trim을 부르지 않음.
- 측정 둘: 쌍 벤치(빠른 경로에 변화 없음), 쉬는 worker가 있는 상태에서 `TrimMemory` 전후의 commit byte.

#### 18.11.9 측정과 구현에서 정한 것

- **빠른 경로는 그대로다.** 쌍 벤치를 AL7 앞의 실행 파일과 같은 round에서 번갈아 쟀다(5 round, 실험 25.11절): 할당·반환 쌍의 최솟값은 둘 다 7.3–7.4 ns, 묶음은 둘 다 5.4 ns/연산, 1 스레드 처리량 101–128 대 122–128 M쌍/s, 8 스레드 420–569 대 392–589 M쌍/s — 잡음 안에서 차이가 없다.
- **쉬는 worker의 캐시.** worker 8개가 각자 캐시를 채우고 대기에 들어간 상태에서 commit 12.2–13.4 MiB가 `TrimMemory` 뒤 256 KiB가 됐다(남은 것은 scheduler 자신의 살아 있는 할당이 든 slab 넷). 걸린 시간은 0.9–1.3 ms다. 표시를 넘기지 않은 대조에서는 7.2–7.5 MiB가 남는다(worker 캐시가 붙든 slab).
- **`MarkThreadIdle`은 비교 교환이다.** 쓰는 중일 때만 쉬는 중으로 바꾼다. 단순 대입이면 두 번 알린 경우에 비우는 중을 덮어써서, 비우는 쪽과 깨어난 소유 스레드가 같은 묶음을 만질 수 있다.
- **쉬는 스레드가 스스로 `TrimMemory`를 부르면** 자기 캐시를 직접 비우지 않고 쉬는 캐시를 도는 단계(등록부 잠금)에 맡긴다 — 같은 캐시를 다른 스레드의 `TrimMemory`와 겹쳐 만지지 않는다.
- **깨어난 뒤의 표시는 gate 밖이다.** `WorkerScheduler`의 worker는 대기에서 돌아오면 gate를 풀고 `MarkThreadActive`를 부른 뒤 다시 잡는다(18.11.5절). gate를 쥔 채 부르면 비우기가 끝날 때까지 다른 스레드의 `Submit`이 그 gate에서 기다린다.
- **시험 통로.** 비우는 쪽을 캐시를 가져간 직후에 세우는 hook(`TestHooks::SetTrimHook` — 지점 셋)과 조회(`IdleThreadCacheCount`·`CurrentThreadCacheBlockBytes`·`IsCurrentThreadShortcut`)를 더했다. slab 블록 수 underflow의 Fatal은 `ReturnToSlabAgain`으로 만든다(자식 프로세스).
- **Commandlet의 설정 할당자도 범용 할당자다.** 진입점에서 할당자를 넘기게 되면서 따로 둘 까닭이 없어졌다(엔진 호스트와 같은 부트를 같은 할당자 위에서 돈다).
- **다른 세션 파일.** 래퍼 전달을 넣느라 ObjectSerialization(`GraphInternal.h`·`GraphLoad.cpp`)과 SpatialIndex(`LooseOctreeInternal.h`)의 모듈 안 adapter에 세 함수를 더했다(동작 변화 없음 — 상위로 넘기기만 한다).
- **Apple의 commit 힌트는 `errno`를 남기지 않는다(리뷰가 짚은 가능성).** slab commit이 부르는 `PosixOpenCommit`(Apple — `madvise(MADV_FREE_REUSE)`, 실패를 무시한다)이 `errno`를 저장·복원한다. 무시하는 실패가 호출 스레드의 `errno`를 바꾸면, 할당자의 느린 경로가 실패한 OS 호출과 그 `errno`를 읽는 코드 사이에 끼었을 때 값이 바뀐다(`ThreadExitSlot`의 OS 오류 값 보존과 같은 까닭 — 18.4절).
- **`WorkerScheduler`의 표시 비용.** 깨어날 때마다 gate를 한 번 더 얻고 가상 호출 둘이 들며 깬 뒤 첫 할당이 느린 경로다. 병렬 표식에 보이는지 AL7 앞의 실행 파일과 같은 round에서 번갈아 쟀다(1M · 간선 4, 4 round — 다른 세션의 부하가 걸린 때라 절대값은 gate 측정 때보다 크다): STW 병렬 표식 ×8이 23.4–24.0 대 23.0–24.4 ms(중앙값 23.5 대 23.7), ×16이 14.0–14.7 대 14.3–14.6 ms, 증분 ×8의 표식 합이 27.8–29.1 대 26.9–30.4 ms, 증분 slice p99가 1,077–1,106 대 1,057–1,090 µs다. 잡음 안에서 차이가 없다.
