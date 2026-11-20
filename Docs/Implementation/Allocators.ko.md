# Rocks Core 할당기 설계와 사용 규약

## 범위와 현재 상태

공통 `Allocator`는 바이트 저장소의 할당·반환 인터페이스다. `SystemAllocator`, `ArenaAllocator`, `StackAllocator`, `PoolAllocator`, `BinnedAllocator`, `BudgetAllocator`, `DebugAllocator`, `ExternalAllocator`와 플랫폼의 `VirtualMemoryRegion`이 구현되어 있다. 이 문서는 소스 기준의 사용·수명 계약을 기록하며, 전체 UE5 Core 기능 대응이나 플랫폼별 테스트 통과를 뜻하지 않는다. 테스트와 실제 실행 결과는 별도로 확인한다.

| 구현 | 주요 용도 | 수명·동시성 기준 |
|---|---|---|
| `SystemAllocator` | OS 정렬 할당을 이용하는 기준 구현 | 할당·반환·통계는 동기화한다(잠금 하나). 살아 있는 할당은 주소 hash 원장에 적어 반환 조회가 상환 O(1)이다. 미반환 할당이 있으면 파괴 시 Fatal이다. |
| `ArenaAllocator` | 수명이 비슷한 임시 작업의 청크 재사용 | 생성한 스레드 전용이다. 개별 반환은 논리적 반환이며 공간은 `Reset()` 후 재사용한다. |
| `StackAllocator` | 역순 반환이 확실한 단일 스레드 작업 | 생성 스레드 전용이며 표시는 일괄 해제가 아닌 반환 경계 확인에 사용한다. |
| `PoolAllocator` | 크기·정렬이 정해진 블록의 반복 재사용 | 일반 호출을 동기화한다. 상위 할당기의 스레드 제약은 그대로 적용된다. |
| `BinnedAllocator` | 범용 할당기: 작은 요청은 예약한 주소 범위의 64KB slab에서 크기 등급별 블록으로, 큰 요청은 상위 할당기로 | 모든 호출을 여러 스레드에서 동시에 부를 수 있다(스레드 캐시 + 등급별 잠금). 미반환 할당이 있으면 파괴 시 Fatal이다. |
| `BudgetAllocator` | 상위 할당기로부터 얻는 payload의 사용량 제한 | 일반 변경 호출을 직렬화한다. 통계는 게시 완료된 상태의 스냅샷이다. |
| `DebugAllocator` | 인접 guard와 제한된 quarantine을 이용한 메모리 손상 진단 | 일반 호출을 동기화한다. 검사는 호출 시점에 수행하며 payload 접근은 별도 동기화가 필요하다. |
| `ExternalAllocator` | 외부 backend callback을 공통 인터페이스에 연결 | callback을 잠금 밖에서 병렬 호출한다. 실제 공급자와 모듈 수명은 별도 연결이 필요하다. |
| `FrameArena` | 프레임·렌더 그래프 수명 객체(소멸자 등록·일괄 비움) | `Allocator`가 아니다. 여러 스레드가 동시에 할당하고(스레드별 `FrameArenaWriter`), `Reset`·`Trim`은 다른 호출과 겹치지 않아야 한다. |

가상 메모리와 Stack은 초기 구현이며, 자동 일괄 해제 stack, page guard 기반 진단과 실제 jemalloc·TCMalloc·mimalloc의 제품 공급자 통합은 아직 제공하지 않는다. `DebugAllocator`의 바이트 guard와 제한된 poison·quarantine은 진단의 일부다. 현재 구현의 성능·단편화·최대 지연과 UE5 대비 성능은 측정하지 않았다.

## 공통 계약

- 정렬은 0이 아닌 2의 거듭제곱이어야 한다. 유효한 정렬의 0바이트 요청은 `nullptr`로 성공한다. 무효 정렬은 0바이트여도 오류다. `Deallocate(nullptr)`은 정상 무효 연산이지만 Arena의 잘못된 스레드 호출 등 각 구현의 우선 검사는 적용된다.
- 할당 실패는 `Result`로 원인을 반환한다. 일반적으로 정렬 오류는 `InvalidArgument`, 크기 산술 초과는 `Overflow`, 메모리 부족은 `OutOfMemory`다. 구현별 `Busy`, `CapacityExceeded`, `Unsupported`, `WrongThread`, `ShuttingDown`, `NotFound`의 조건과 검사 순서는 아래 항목을 따른다.
- 각 할당기는 저장소를 제공할 뿐 C++ 객체를 자동 생성하거나 소멸하지 않는다. `Array` 등의 사용자는 객체를 먼저 소멸시키고 포인터의 모든 사용을 끝낸 뒤, 같은 할당기로 정확한 시작 주소를 반환해야 한다. 할당기와 상위 할당기는 모든 사용자보다 오래 살아야 한다.
- 외부·중복 포인터는 임의 주소를 역참조하지 않고 소유 기록에서 확인한다. 이미 반환된 주소가 새 할당에 재사용된 경우 원시 포인터만으로 이전 세대를 구별할 수 없다. 이 진단은 수명 위반이나 악의적 공급자를 격리하는 장치가 아니다.
- `AllocationStats`의 `liveAllocations`, `liveBytes`, `totalAllocations`는 게시된 사용자 요청 기준이다. 정렬 여유, 기록·청크·상위 할당기의 내부 메모리까지 포괄하지 않는다. 스냅샷이 0이어도 다른 스레드의 작업이 끝났다는 보장은 없다.
- `Allocator` 자체에는 `Reallocate`가 없다. 크기 변경은 별도 새 할당·데이터 이동·기존 반환과 실패 처리가 필요하다. 반환된 메모리의 동시 접근과 호출 중 할당기 파괴는 사용자가 동기화한다.

## ArenaAllocator

```cpp
rocks::SystemAllocator system;
rocks::ArenaAllocator arena(system, 64 * 1024);
{
    rocks::Array<rocks::int32> values(arena);
    auto result = values.TryPushBack(7);
    if (!result) {
        return result.Error();
    }
} // values가 저장소를 반환한다.

auto reset = arena.Reset();
if (!reset) {
    return reset.Error();
}
```

위 코드는 `ErrorCode`를 반환할 수 있는 함수 내부의 예다. `Deallocate`는 살아 있는 개별 할당을 기록에서 제거하지만 공간을 즉시 회수하지 않는다. 미반환 할당이 있으면 `Reset()`과 `Trim()`은 `Busy`이며 기존 데이터·청크를 유지한다. 비어 있을 때 `Reset()`은 청크를 유지해 재사용하고 `Trim()`은 상위 할당기로 반환한다. 미반환 상태의 소멸과 상위 할당기의 소유 청크 반환 거부는 Fatal이다.

모든 호출과 파괴는 생성 스레드에서 실행한다. `Result` API는 잘못된 스레드에서 인자 검사보다 먼저 `WrongThread`다. `Stats()`나 파괴를 다른 스레드에서 실행하면 Fatal이다. 생성 스레드가 종료된 뒤 OS가 스레드 ID를 재사용하는 경우 동일 스레드로 오인할 수 있으므로 생성 스레드는 Arena보다 오래 살아야 한다. 상위 callback의 동일 Arena 재진입은 `Result` API에서 `Busy`, `Stats()`·파괴에서 Fatal이다. `tag`는 새 청크 확보 시 상위 할당기로 전달되며 개별 할당의 태그별 통계는 제공하지 않는다.

강제 frame reset은 없다(프레임 수명 객체를 한 번에 비우려면 아래 `FrameArena`를 쓴다). 프레임 경계에서 사용하려면 객체 소멸과 작업·GPU 등 백엔드의 사용 종료를 상위 시스템이 먼저 보장하고, 모든 할당을 반환한 뒤 `Reset()`해야 한다. 임의의 live 포인터나 소멸하지 않은 객체를 자동으로 무효화하지 않는다.

## FrameArena

`Core/Memory/FrameArena.h`는 한 수명(프레임·렌더 그래프 하나) 동안 객체를 만들고, 수명이 끝나면 `Reset`이 등록된 소멸자를 등록 역순으로 부른 뒤 메모리를 한 번에 비우는 다중 스레드 bump arena다(RenderBase CP3, UE `Core/Public/Misc/MemStack.h`·`RenderCore/Public/RenderGraphAllocator.h` 대응).

```cpp
rocks::FrameArena arena(system);                       // 표준 블록 64 KiB, 빈 블록 64개까지 남김
auto pass = arena.TryNew<RenderPass>(name);            // 소멸자가 있으면 Reset 때 부른다
auto draws = arena.TryNewArray<DrawCommand>(count);    // 값 초기화한 연속 배열(Span)
{
    rocks::FrameArenaWriter writer(arena);             // 이 스레드 전용 cursor
    auto node = writer.TryNew<GraphNode>(...);         // 자기 블록 안에서는 잠금 없음
}
auto reset = arena.Reset();                            // 소멸자(등록 역순) → 블록 비움
```

**기존 할당기와 겹치지 않는 이유.** `ArenaAllocator`·`StackAllocator`는 개별 할당의 반환을 기록하고 반환되지 않은 할당이 있으면 `Reset`을 `Busy`로 거부한다. 객체 수명을 모르는 할당기라 살아 있는 객체의 메모리를 비우지 않기 위해서다. `FrameArena`는 객체 수명을 스스로 가진다. 만든 객체의 수명은 정의상 `Reset`에서 끝나고 소멸자도 arena가 부르며 개별 반환이 없다. 그래서 `Reset`은 살아 있는 할당을 이유로 거부하지 않고, 아직 할당 중인 writer가 열려 있을 때만 `Busy`다. 개별 반환이 없으므로 `Allocator` 인터페이스(성장하는 `Array`의 이전 버퍼 반환 등)를 구현하지 않는다. 가변 길이 목록은 `TryNewArray`로 크기를 정해 만든다.

| 항목 | 계약 |
|---|---|
| 정렬 | 0이 아닌 2의 거듭제곱(아니면 0 byte여도 `InvalidArgument`). 0 byte는 성공한 `nullptr`. 블록 payload는 64 byte 정렬이고 더 큰 정렬은 블록 안 여유로 맞춘다 |
| 대형 할당 | 요청 + (정렬 − 1)이 표준 payload의 절반을 넘으면 그 할당만의 대형 블록을 backing에서 받는다. 대형 블록은 `Reset`에서 바로 돌려준다 |
| 블록 재사용 | `Reset`은 표준 블록을 `maxRetainedBlocks`개까지 비워 다음 수명에 쓰고 나머지를 돌려준다. 수명마다 쓰는 양이 비슷하면 backing 호출이 없다. `Trim`은 남긴 블록을 모두 돌려주고, 파괴는 `Reset` 뒤 `Trim`이다 |
| 소멸자 | `TryNew`·`TryNewArray`로 만든 객체 가운데 자명하지 않은 소멸자만 [기록][객체] 한 덩어리로 등록한다. 등록은 원자적 머리 삽입이라 `Reset`은 등록 역순(여러 스레드면 게시 순서의 역순)으로 부르고, 배열은 원소 역순이다. 생성자·소멸자는 noexcept여야 한다(static_assert) |
| 다중 스레드 | `FrameArenaWriter`는 한 스레드의 cursor다. 자기 표준 블록 안 할당은 잠금이 없고 새 블록을 얻을 때만 arena 잠금을 쓴다. arena 자신의 `TryAllocate`·`TryNew`는 공유 cursor를 잠금으로 나눠 쓴다. `Stats`·writer 생성·파괴도 동시에 부를 수 있다 |
| Reset | 다른 어떤 호출과도 겹치지 않아야 한다(호출자가 수명 경계를 동기화). 열린 writer가 있거나 `Reset` 안(소멸자)에서 다시 부르면 `Busy`이고 아무것도 바꾸지 않는다. 소멸자 안의 할당도 `Busy`다 |
| 실패 | 할당 실패는 backing 오류, 크기 넘침은 `Overflow`. 실패해도 이미 받은 할당은 그대로다. writer가 열린 채 arena를 파괴하면 Fatal, backing이 자기 블록 반환을 거부하면 Fatal이다 |
| 통계 | `requestedBytes`는 `TryAllocate`에 넘긴 크기의 합이다. `TryNew`·`TryNewArray`의 소멸자 기록은 들어가고 정렬 여유·블록 머리는 빠진다 |
| 진단 | Asan 구성은 새 블록과 `Reset`으로 비운 블록의 payload를 poison하고 할당한 범위만 연다. 이전 수명의 포인터를 쓰면 ASan이 보고한다 |

backing은 arena보다 오래 살고 여러 스레드의 할당·해제를 지원해야 한다. backing 호출은 arena 잠금 안에서 하므로 backing 콜백이 같은 arena를 부르면 교착이다(금지). 시험(`Core/Tests/FrameArenaTests.cpp`)은 등록 역순 소멸(객체·배열 원소 역순·자명 소멸자 미등록·두 번째 Reset), 1–1024 정렬·256 정렬 타입·대형 블록 두 종류와 Reset 반환·0 byte·잘못된 정렬·`Overflow`, 블록 남김 수·다음 수명의 backing 무호출·`Trim`·`maxRetainedBlocks` 0·파괴 시 전부 반환, 네 writer의 동시 할당 8000개(내용으로 겹침 확인·소멸자 수), 열린 writer의 Reset 거부·소멸자 안 재진입 거부·backing 실패 보존을 확인한다.

## StackAllocator

`StackAllocator(backing, chunkBytes)`는 생성 스레드 전용 LIFO 저장소다. 마지막 할당부터 정확한 주소를 `Deallocate()`해야 한다. 순서를 어기면 `Busy`, 소유하지 않거나 이미 반환한 주소는 `NotFound`다. 잘못된 스레드의 `Result` 호출은 인자 검사보다 먼저 `WrongThread`이며, `Mark()`·`Stats()`·파괴에서 위반하면 Fatal이다. 생성 스레드와 backing은 stack보다 오래 살아야 한다.

```cpp
rocks::StackAllocator stack(system);
auto mark = stack.Mark();
auto block = stack.Allocate(64, 16);
if (!block) {
    return block.Error();
}

// 사용과 객체 소멸을 끝낸 뒤 할당의 역순으로 반환한다.
auto released = stack.Deallocate(block.Value());
if (!released) {
    return released.Error();
}
auto rewound = stack.Rewind(mark);
if (!rewound) {
    return rewound.Error();
}
```

`Rewind()`는 해당 표시 뒤 할당이 모두 반환되었는지 확인할 뿐 live allocation을 일괄 해제하거나 C++ 객체를 소멸하지 않는다. 살아 있는 후속 할당이 있으면 `Busy`다. 다른 allocator의 표시나 기본 표시는 `InvalidArgument`, 기준 할당 반환 또는 `Reset()`·`Trim()` 이후 무효인 표시는 `StaleHandle`이다. 오류를 복구해야 하는 경우 `Mark()` 대신 `TryMark()`를 쓴다. 모든 할당 반환 후 `Reset()`은 청크를 보유하고 `Trim()`은 반환한다. backing callback 재진입은 `Result` API에서 `Busy`, `Mark()`·`Stats()`·파괴에서는 Fatal이다.

성장하는 `Array`는 새 버퍼를 확보한 다음 이전 버퍼를 반환하므로 LIFO 규약에 맞지 않는다. 하나의 버퍼만 확보하고 역순 소멸이 보장되는 경우에 한해 사용한다. 생명주기·원시 포인터 재사용·backing 해제 실패의 제약은 Arena와 동일하게 주의한다.

## PoolAllocator

`PoolAllocator(backing, blockBytes, blockAlignment, blocksPerChunk)`는 고정 블록을 청크 단위로 확보하고 반환된 슬롯을 재사용한다. `blockBytes`와 `blocksPerChunk`는 0보다 커야 하고 `blockAlignment`는 2의 거듭제곱이어야 한다. 잘못된 설정은 생성 시 Fatal이며 표현할 수 없는 청크 크기는 할당 시 `Overflow`다. 요청 크기나 정렬이 설정을 초과하면 `Unsupported`다. `Capacity()`는 전체 슬롯 수, `ChunkCount()`는 청크 수다.

사용 중인 블록이 있으면 `Trim()`과 `Shutdown()`은 `Busy`이고 기존 상태를 유지한다. 모든 블록 반환 후 `Trim()`은 청크를 반환하되 계속 사용할 수 있다. `Shutdown()`은 반환과 함께 새 비영 할당을 닫으며 재호출은 성공한다. `Allocate`는 종료 상태보다 정렬·크기 검사와 0바이트 처리를 먼저 한다. 따라서 종료 후에도 무효 정렬은 `InvalidArgument`, 지원 범위를 넘는 요청은 `Unsupported`, 유효한 0바이트는 성공이다. 유효하고 지원되는 비영 요청은 `ShuttingDown`이다.

Pool 자체의 호출은 동기화되지만, 상위 `Allocate`·`Deallocate`는 호출한 스레드에서 실행된다. 상위가 Arena처럼 스레드 전용이면 다른 스레드의 청크 확장은 `WrongThread`가 될 수 있다. 반환이 거부되는 스레드에서 `Trim`·`Shutdown`·파괴를 실행하면 소유 청크 반환 실패로 Fatal이다. 동일 Pool에 대한 상위 callback의 재진입은 `Busy` 또는 Fatal로 진단한다. 상위 callback이 다른 스레드의 동일 Pool 작업을 기다리거나 여러 Pool 잠금을 역순으로 획득하는 구성은 교착 위험이 있어 금지한다. 개별 tag를 보관하지만 태그별 조회·통계 API는 없다. 청크 검색 비용과 경합 비용은 별도 측정이 필요하다.

## BinnedAllocator

`BinnedAllocator(backing, config)`는 범용 할당기다(설계와 근거는 `Docs/Design/GarbageCollectionScale.ko.md` 18절, UE `FMallocBinned3` 대응). 작은 요청(8192바이트 이하, 정렬 64 이하)은 인스턴스가 예약한 주소 범위 안의 64KB slab에서 크기 등급별 블록으로 내주고, 그보다 큰 요청은 backing으로 넘긴다.

```cpp
rocks::SystemAllocator system;
rocks::BinnedAllocator memory(system);
auto block = memory.Allocate(48, 16);
if (block) {
    (void)memory.Deallocate(block.Value());
}
```

- 크기 등급은 32개다: 16–128은 16 단계, 그 위는 2배 구간마다 넷(160·192·224·256, …, 8192)이다. 블록은 16바이트 이상으로 정렬되고, 정렬 32·64는 블록 크기가 그 배수인 등급으로 올린다. 128을 넘는 요청의 등급 올림 낭비는 블록의 20% 이하다.
- 주소 범위: 생성 때 `config.reserveBytes`(0이면 Windows·Mac 16 GiB, Android 4 GiB)를 접근을 열어 둔 예약(`VirtualMemoryAccess::Open`)으로 잡고 slab을 쓸 때 commit한다. OS가 예약을 거부해도 생성은 실패하지 않는다. 절반씩 줄여 64 MiB까지 다시 시도하고(설정이 그보다 작으면 그 값 한 번만), 끝내 못 하면 모든 요청이 backing으로 간다. 실제 크기는 `ReservedBytes()`다. 범위가 다 찼거나 slab commit이 실패한 작은 요청도 backing으로 간다(`Usage().fallbackAllocations` — 0이 아니면 예약 크기를 늘릴지 본다). commit 실패는 일시적인 것으로 다뤄 다음 요청이 다시 시도한다.
- 빈 slab은 `config.retainedEmptySlabs`개(기본 64 = 4 MiB)까지 commit한 채 보관하고 넘으면 decommit한다. `Trim()`은 보관분을 모두 decommit한다. 살아 있는 할당이 있어도 부를 수 있고 값을 돌려주지 않는다. decommit이 실패한 slab은 commit된 채 보관해 다시 쓰고 다음 `Trim()`이 다시 푼다.
- 반환의 소유 판정: 미소유·중복·중간 주소의 `Deallocate`는 `NotFound`이고 상태를 바꾸지 않는다. 예약 범위 밖 주소, 쓰지 않는 slab의 주소, 블록 경계가 아닌 주소는 그 주소를 읽지 않고 판정한다. 쓰는 slab의 블록 경계는 slab 머리의 "뗀 블록 수"와 블록 안 반환 표지(둘째 word = 주소 XOR 인스턴스 비밀값)를 읽어 판정한다. 한계: 살아 있는 블록의 offset 8에 사용자가 그 블록의 표지 값(최상위 비트가 선 64비트 값)을 써 두면 정상 반환이 `NotFound`가 된다(2^-63). 잘못된 반환이 그 slab의 decommit·재사용과 같은 순간에 겹치면 검출이 보장되지 않는다(설계 18.4절 한계 2).
- 통계: `Stats()`의 `liveBytes`는 요청 크기가 아니라 내준 블록 크기의 합이다(반환 때 요청 크기를 알 수 없다 — 큰 할당은 요청 크기 그대로다). `Usage()`는 예약 크기, commit한 slab 바이트, 보관 중인 빈 slab 수, 살아 있는 큰 할당 수, backing으로 간 작은 요청의 누적 수를 낸다.
- backing: 이 객체와 모든 할당보다 오래 살고 어느 스레드에서나 부를 수 있어야 한다. backing 호출은 잠금 밖에서 한다(backing이 같은 allocator를 다시 불러도 된다 — 전의 재진입 `Busy` 규칙은 없다). 성공과 함께 null·정렬이 틀린 주소·주소 넘침·예약 범위와 겹치는 주소·이미 내준 주소를 돌려주면 Fatal이다. 큰 블록끼리의 부분 겹침은 검사하지 않는다(backing의 책임이다). backing이 큰 블록의 반환을 거부하면 그 오류를 돌려주고 할당은 살아 있는 채로 남는다.
- 스레드 캐시: 스레드마다·인스턴스마다 등급별 묶음 둘(부분·가득 — 묶음 한도는 64개 또는 64KB 가운데 작은 쪽)을 두어 대부분의 할당·반환이 잠금 없이 끝난다. 가득 찬 묶음은 등급마다 8칸인 이전 칸으로 스레드 사이를 오가고, 모자라거나 넘치면 등급 잠금 아래 slab과 묶음 단위로 주고받는다. 다른 스레드가 반환한 블록은 반환한 스레드의 캐시로 간다. 캐시는 그 스레드가 처음 쓸 때 생기고 스레드가 끝날 때(`platform::ThreadExitSlot` — 엔진 밖에서 만든 스레드 포함) 인스턴스로 돌아간다. 캐시에 든 블록은 slab이 "쓰는 블록"으로 세므로 그 slab은 풀리지 않는다. 스레드 하나가 인스턴스 하나의 캐시에 쥘 수 있는 블록은 `BinnedAllocator::MaxThreadCacheBytes`(2,361,856 byte)까지다. 다른 스레드 종료 정리(다른 `ThreadExitSlot`의 정리 함수 등) 안에서 써도 된다. 한계: 할당자의 종료 정리가 지나간 뒤의 종료 정리에서 쓴 블록은 드물게 그 스레드의 새 캐시에 남아 인스턴스 파괴 때 회수된다(설계 18.4절 "남는 한계").
- `Trim()`은 호출 스레드의 캐시와 이전 칸을 slab으로 돌려준 뒤에 빈 slab을 decommit한다. 다른 스레드의 캐시는 건드리지 않는다.
- `config.threadCaches = false`면 모든 작은 요청이 등급 잠금 경로로 간다. 느리지만 반환한 블록이 곧바로 slab으로 돌아가 slab 수준 동작이 결정적이다(시험·작은 도구). 캐시를 얻지 못한 호출(플랫폼 할당 실패), 기록을 스레드 종료 칸에 걸지 못한 스레드(칸 소진·OS 거부 — 그 스레드는 다시 시도하지 않는다), 종료 정리가 지나간 스레드도 이 경로를 쓴다.
- 스레드: 모든 메서드를 여러 스레드에서 동시에 부를 수 있다. 같은 주소를 두 스레드가 같은 순간에 반환하면(오용) 둘 다 성공으로 돌아올 수 있다 — 반환 표지는 비교 뒤 단순 대입이다. 그 블록을 두 번째로 내주려는 순간 Fatal `BinnedAllocator free block lost its return marker`로 끝난다(반환한 블록의 둘째 word를 덮어쓴 경우도 같다). 순서가 있는 중복 반환은 `NotFound`다. 파괴는 모든 호출이 끝난 뒤에 하고(다른 스레드가 살아 있어도 된다 — 파괴가 그 스레드의 캐시까지 해제한다), 반환하지 않은 할당이 남아 있으면 Fatal이다. `Shutdown()`은 없다(수명은 객체 파괴로 끝난다). 다른 스레드가 쓰는 중의 `Stats()`는 그 순간의 근사다.
- ASan 구성: 사용자에게 나가 있지 않은 블록(slab에서 뗀 블록, 캐시·이전 칸·slab 자유 목록의 블록)은 전체를 닫고(`AsanPoisonMemory`), 내줄 때 요청 크기만 연다. 반환 뒤 접근과 요청 크기를 넘는 접근이 ASan 보고로 드러난다. 할당기 자신의 자유 블록 word 접근은 계측에서 뺀 내부 함수(`ROCKS_NO_SANITIZE_ADDRESS`)로만 한다.
- 큰 할당의 주소 원장과 내부 상태는 플랫폼 할당을 쓰므로 backing의 예산에 들지 않는다. 원장 늘리기 실패(`OutOfMemory`)와 예약 실패는 시험 접근(`Core/Private/Memory/BinnedAllocatorTestAccess.h`)으로, slab commit·decommit 실패는 `VirtualMemoryRegion`의 시험 통로로 주입한다.

## 플랫폼 가상 메모리

`platform::QueryVirtualMemoryInfo()`로 page 크기와 예약 정렬 단위를 조회하고, `VirtualMemoryRegion`의 `Reserve(bytes)` → `Commit(offset, bytes)` → `Decommit(offset, bytes)` → `Release()`로 단일 예약을 관리한다. 예약 크기와 변경 범위는 page 크기의 양수 배수여야 한다. 결과의 `code`와 OS 실패 시 `nativeError`를 확인한다. 일반 `Allocator`처럼 개별 객체를 관리하지 않는다.

메서드 간 호출은 직렬화되지만 `Address()`로 얻은 주소의 접근은 보호하지 않는다. 반환된 주소의 모든 사용과 다른 호출을 끝낸 뒤 `Decommit()`·`Release()` 및 파괴를 수행한다. 데이터는 실행 권한이 없다. macOS의 commit은 Windows commit charge와 같은 보장을 제공하지 않는다. decommit 후 즉시 물리 메모리가 반환되거나 재commit 시 내용이 0으로 초기화된다고 가정하지 않는다.

예약 종류는 둘이다(`Reserve(bytes, access)`). 기본 `VirtualMemoryAccess::Guarded`는 commit하지 않은 범위와 decommit한 범위의 접근이 fault다(guard 페이지로 쓸 수 있다 — `SecretArena`). `VirtualMemoryAccess::Open`은 접근을 열어 둔 예약으로, 큰 주소 범위를 작은 조각으로 commit·decommit하는 할당자(`BinnedAllocator`)가 쓴다: POSIX에서 예약이 읽기·쓰기 매핑 하나이고 commit은 보호를 바꾸지 않으며 decommit은 페이지만 OS에 돌려주므로 매핑(VMA)이 조각마다 쪼개지지 않는다(Linux·Android의 프로세스당 매핑 수 한도 `vm.max_map_count`를 피한다 — UE 5.8 `FPlatformVirtualMemoryBlock`과 같다). 대신 그 종류의 POSIX에서는 commit하지 않았거나 decommit한 범위의 접근이 fault라는 보장이 없다. Windows는 두 종류가 같다.

reserve 실패는 예약을 남기지 않는다. commit·decommit 실패는 영역을 닫지 않는다: commit이 실패한 범위는 commit되지 않은 것으로 다루고 다시 commit하거나 decommit할 수 있다. decommit 실패는 `Open` 예약과 Windows에서 그 범위를 그대로 두고(접근·내용 유지), POSIX의 `Guarded`에서는 일부 페이지의 접근이 막혔을 수 있어 commit되지 않은 것으로 다룬다. release 실패만 소유권을 보존한 채 poisoned 상태를 만들고(주소 조회는 null, `Release()`만 다시 시도) 소멸 시 release 실패는 Fatal이다. 시험은 `PlatformBase/Private/Common/VirtualMemoryTestAccess.h`로 이 스레드의 n번째 commit·decommit을 OS를 부르지 않고 실패시킨다. 페이지 보호 fault probe와 일반 경계·동시 호출 시험이 있고 Windows Core Debug에서 통과한다. macOS는 CI에서 돈다.

## BudgetAllocator

`BudgetAllocator(backing, limitBytes)`는 payload 요청 바이트 합계에 상한을 적용한다. 초과 요청은 `CapacityExceeded`이며 기존 할당과 상한은 유지된다. `SetLimit()`은 현재 `liveBytes`보다 작은 값으로 줄일 수 없다. `PeakBytes()`는 해당 인스턴스의 최대 게시 사용량이다. 기록용 할당과 상위 할당기의 패딩은 한도와 사용자 통계에 포함하지 않는다.

소유 기록을 먼저 확보하고 payload 할당이 실패하면 기록을 반환한다. 상위 `Deallocate`가 실패하면 같은 payload와 소유 기록·사용량을 보존하고 오류를 전파한다. 재시도를 위해 상위 할당기도 실패 시 원래 메모리와 소유권을 보존해야 한다. 소유 기록 자체의 반환이 거부되면 Fatal이다.

일반 변경 호출은 직렬화되며 실제 상위 호출은 각 호출 스레드에서 실행된다. 상위 할당기도 해당 스레드의 접근을 지원해야 한다. 동일 스레드 상위 callback의 상태 변경 재진입은 `Busy`다. 유효한 0바이트·null 반환과 인자 오류는 진입 검사보다 먼저 처리되고, 통계 조회는 게시된 스냅샷을 허용한다. 상위 callback이 다른 스레드의 동일 Budget 작업을 기다리는 것은 교착 위험으로 금지한다. 파괴 전에는 조기 반환·통계 조회를 포함한 **모든** 호출이 끝나야 한다. 추적 중인 작업·대기자 또는 미반환 할당이 남은 파괴는 Fatal이다.

## DebugAllocator

`DebugAllocator(backing, quarantineBytes)`는 payload 양쪽에 바이트 guard를 두고 새 payload를 `0xCD`로 채운다. 기본 quarantine 상한은 1 MiB의 payload이며, 기록·guard·정렬 여유는 이 상한과 payload 통계에 포함하지 않는다. 객체 생성·소멸과 포인터 사용 종료는 호출자가 책임진다.

```cpp
rocks::DebugAllocator debug(system);
auto block = debug.Allocate(32, 16, 7);
if (!block) {
    return block.Error();
}

// block.Value()는 32바이트다. 사용 종료 후 같은 할당기로 반환한다.
auto released = debug.Deallocate(block.Value());
if (!released) {
    return released.Error();
}

auto report = debug.Validate();
if (!report) {
    return report.Error();
}
if (report.Value().issue != rocks::DebugMemoryIssue::None) {
    // report.Value().address, offset, tag로 원인을 조사한다.
}
```

`Validate()`의 `Result`는 호출 성공 여부이며, 손상 여부는 성공 후 `DebugMemoryReport::issue`에서 별도로 확인한다. live와 quarantine 블록의 앞·뒤 guard 및 quarantine payload의 poison을 검사하여 처음 발견한 손상을 보고한다. 보고된 주소는 식별값이며 접근권을 제공하지 않는다. 손상 여부를 모든 메모리 쓰기 순간에 감시하지 않는다.

`Deallocate()`는 live 블록의 guard를 검사하고, quarantine에 보관할 때 payload를 `0xDD`로 채운다. guard 손상은 Fatal이다. 제한을 넘는 블록 또는 상한이 0인 경우 즉시 상위 할당기로 반환하며 그 경로에서는 payload를 덮지 않는다. quarantine에서 다른 블록을 축출하거나 `FlushQuarantine()`으로 반환할 때 guard와 poison 손상은 Fatal이다. 상위 반환 실패 시 해당 블록과 그 이후 블록은 보존하고 오류를 반환한다. 이미 반환된 이전 블록은 복원하지 않는다. 현재 `Deallocate()`의 블록은 실패 시 live 상태로 남는다.

quarantine 검사는 해제 후 쓰기의 일부만 검출한다. `0xDD`와 같은 값을 다시 쓰면 바이트 변화가 없어 탐지되지 않으며, 상위 할당기로 이미 반환된 주소의 접근·범위 밖 UAF·다른 메모리 파괴·경합 및 주소 재사용 뒤 stale pointer는 보장하지 않는다. payload 읽기·쓰기와 `Validate()`/`Deallocate()`가 겹치지 않도록 호출자가 동기화해야 한다. backing은 호출 스레드의 사용을 허용해야 하며, 미반환 live allocation이나 파괴 중 격리 저장소의 반환 거부는 Fatal이다.

## 외부 할당기 연결

`ExternalAllocatorBackend`에 공급자의 `allocate`·`deallocate` callback, context와 필요한 `ModuleLease`를 지정하고 이를 `ExternalAllocator`로 옮긴다. `JemallocBackend`, `TcmallocBackend`, `MimallocBackend`는 호출자가 제공한 C ABI 함수 쌍을 이 callback 형태로 바인딩한다. 실제 공급자 바이너리·라이브러리 로딩·링크는 별도로 마련해야 하며 전역 `malloc`을 자동 교체하지 않는다. 두 callback 중 하나라도 없으면 `IsAvailable()`은 거짓이고, 유효한 비영 할당과 비null 반환은 `Unsupported`다. `IsAvailable()`은 callback의 존재만 나타내며 공급자의 신뢰성·정상 링크·플랫폼 지원을 증명하지 않는다.

backend는 요청한 크기와 정렬을 만족하는 서로 겹치지 않는 저장소를 반환해야 하며, 반환 실패 시 주소와 내용·소유권을 유지해야 한다. callback은 `noexcept`이고 병렬 호출을 지원해야 한다. 같은 할당기에 대한 동일 스레드 callback 재진입은 `Busy`다. 같은 주소를 한 스레드가 반환하는 동안 다른 반환은 `Busy`다. payload가 살아 있는 다른 payload 또는 소유 기록과 겹치거나 주소 범위가 표현 범위를 넘는 것이 발견되면 Fatal로 진단한다. 악의적인 공급자의 모든 오동작을 격리하지는 않는다.

소유 기록은 기본적으로 플랫폼 할당 경계에서 확보한다. 선택적 metadata `Allocator*`를 전달하면 기록 할당 실패를 주입하거나 정책을 선택할 수 있다. 그 할당기는 ExternalAllocator·모든 호출보다 오래 살아야 하고 병렬 호출을 지원해야 하며, backend payload 및 다른 살아 있는 기록과 겹치지 않는 저장소를 반환해야 한다. 순환 의존은 금지한다. 기록 확보 실패 시 backend는 호출되지 않으며, 소유 기록 반환 실패는 Fatal이다. 외부 라이브러리가 동적 모듈이라면 함수 코드와 context는 마지막 callback까지 유효해야 하고 적절한 `ModuleLease`로 보호한다. 파괴 전에는 모든 호출을 종료하고 할당을 반환해야 한다.

### 벤더 함수 바인딩

```cpp
// 공급자 헤더와 라이브러리는 프로젝트가 별도로 준비한다.
rocks::MimallocBackend functions(mi_malloc_aligned, mi_free);
auto bound = functions.Bind(moduleLease); // 정적 링크면 빈 ModuleLease도 가능하다.
if (!bound) {
    return bound.Error();
}
rocks::ExternalAllocator allocator(rocks::Move(bound.Value()));
```

예제에서 `functions`는 `allocator`와 모든 호출보다 오래 살아야 한다. `Bind()`는 함수 주소가 없으면 `InvalidArgument`를 반환하며, 임시 바인딩 객체의 호출은 금지한다. 동적 라이브러리 함수는 적절한 `ModuleLease`와 유효한 context·호출 규약을 제공해야 한다. `Bind()`는 주소의 실제 공급자·동일 빌드 여부를 인증하지 않는다.

`JemallocBackend`는 동일 jemalloc 빌드에서 얻은 `aligned_alloc` 계열 함수와 `free`, `TcmallocBackend`는 실제 TCMalloc에 바인딩된 `aligned_alloc`/`free` 쌍을 받아 정렬을 첫 인자로 전달한다. 두 경로는 C11 규약에 맞게 요청 크기를 정렬의 배수로 올린다. `MimallocBackend`는 `mi_malloc_aligned(size, alignment)`와 `mi_free`의 인자 순서를 사용한다. 세 바인딩 모두 Rocks의 `tag`를 공급자에게 전달하지 않는다. 심볼 이름·접두사·calling convention·정적/동적 링크와 두 함수의 동일 공급자 여부는 실제 사용 환경에서 확인해야 한다. 이 바인딩은 바이너리를 포함하거나 가져오지 않는다.

## UE5와 외부 공급자 확장

UE5의 [FMemStackBase](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FMemStackBase)는 일괄 범위 해제를, [FMallocBinned2](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Core/FMallocBinned2)는 크기별 일반 할당기 기능을, [FMallocPoisonProxy](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Core/FMallocPoisonProxy)는 메모리 진단 기능을 제공한다. Rocks의 Arena·Stack·Pool·Binned·Budget·Debug는 일부 사용 사례의 기반이며 이들 기능 전체 또는 성능 동등성을 의미하지 않는다.

[Google TCMalloc의 플랫폼 안내](https://github.com/google/tcmalloc/blob/master/docs/platforms.md)는 Linux x86-64/AArch64를 지원 대상으로 열거한다. Windows·macOS의 지원을 이 표에서 확인할 수 없으므로 해당 OS에서 동작한다고 가정하지 않는다. [Meta는 jemalloc 개발 참여를 발표](https://engineering.fb.com/2026/03/02/data-infrastructure/investing-in-infrastructure-metas-renewed-commitment-to-jemalloc/)하였다. [mimalloc 공식 저장소](https://github.com/microsoft/mimalloc)는 Windows·macOS·Linux 등을 지원 대상으로 제시한다. Rocks는 위 함수 쌍을 받는 벤더별 바인딩을 제공하지만, 어느 공급자의 바이너리나 소스도 포함·로드하지 않는다. 별도의 Windows scratch 환경에서는 mimalloc v3.6.1 함수 연결 probe가 통과하였다. 이는 제품 빌드·배포 통합이나 다른 OS의 검증을 의미하지 않는다. jemalloc·TCMalloc의 실제 연결과 각 공급자의 제품 통합·플랫폼별 실행 검증은 미완료다.

공급자별 도입 전에는 다음 항목을 각각 완료한다.

1. 정확한 upstream 버전·commit·출처와 라이선스 및 배포 고지를 고정한다. Google TCMalloc의 [라이선스](https://github.com/google/tcmalloc/blob/master/LICENSE), jemalloc의 [라이선스](https://github.com/jemalloc/jemalloc/blob/dev/COPYING), mimalloc의 [라이선스](https://github.com/microsoft/mimalloc/blob/dev/LICENSE)를 해당 버전에서 다시 확인한다.
2. 정렬 할당과 반환을 동일 공급자에 연결하고 ABI·CRT 경계, 모듈 unload, 재진입, 초기화·종료와 실패 정책을 검증한다. 시스템 allocator와 외부 공급자 간 포인터를 섞지 않는다.
3. Rocks의 빌드 규칙과 설치 SDK에 플랫폼별 dependency·버전·라이선스·라이브러리 패키징을 추가한다. 미지원 플랫폼이나 빠진 라이브러리 선택은 명시적으로 실패해야 한다.
4. 해당 Windows·macOS·Linux 환경에서 실제 공급자를 사용해 정렬·대용량·다중 스레드·교차 스레드 반환·할당 실패·종료 및 장시간 누수 테스트를 실행한다. 가짜 callback 테스트는 adapter 계약만 검증한다.
5. 대표 게임 작업의 처리량·지연 분포·메모리 증가·단편화·스레드 경합을 측정한다. ThreadSanitizer·AddressSanitizer의 실제 지원 범위와 호환성을 별도 확인한다. 정책의 기본값은 안정성·측정 결과를 바탕으로 정하며 특정 공급자가 항상 빠르다고 가정하지 않는다.

개별 소스와 테스트·실행 증거는 [Foundation 구현 상태](FoundationStatus.ko.md)와 [UE5 Core 기능 감사표](../Design/UnrealCoreFeatureAudit.ko.md)에서 함께 추적한다.
