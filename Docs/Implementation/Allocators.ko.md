# Rocks Core 할당기 설계와 사용 규약

## 범위와 현재 상태

공통 `Allocator`는 바이트 저장소의 할당·반환 인터페이스다. `SystemAllocator`, `ArenaAllocator`, `StackAllocator`, `PoolAllocator`, `BinnedAllocator`, `BudgetAllocator`, `DebugAllocator`, `ExternalAllocator`와 플랫폼의 `VirtualMemoryRegion`이 구현되어 있다. 이 문서는 소스 기준의 사용·수명 계약을 기록하며, 전체 UE5 Core 기능 대응이나 플랫폼별 테스트 통과를 뜻하지 않는다. 테스트와 실제 실행 결과는 별도로 확인한다.

| 구현 | 주요 용도 | 수명·동시성 기준 |
|---|---|---|
| `SystemAllocator` | OS 정렬 할당을 이용하는 기준 구현 | 할당·반환·통계는 동기화한다. 미반환 할당이 있으면 파괴 시 Fatal이다. |
| `ArenaAllocator` | 수명이 비슷한 임시 작업의 청크 재사용 | 생성한 스레드 전용이다. 개별 반환은 논리적 반환이며 공간은 `Reset()` 후 재사용한다. |
| `StackAllocator` | 역순 반환이 확실한 단일 스레드 작업 | 생성 스레드 전용이며 표시는 일괄 해제가 아닌 반환 경계 확인에 사용한다. |
| `PoolAllocator` | 크기·정렬이 정해진 블록의 반복 재사용 | 일반 호출을 동기화한다. 상위 할당기의 스레드 제약은 그대로 적용된다. |
| `BinnedAllocator` | 크기별 pool과 상위 할당기를 조합하는 범용 저장소 | 변경 호출을 직렬화한다. 원장 검색과 잠금 비용을 측정해야 한다. |
| `BudgetAllocator` | 상위 할당기로부터 얻는 payload의 사용량 제한 | 일반 변경 호출을 직렬화한다. 통계는 게시 완료된 상태의 스냅샷이다. |
| `DebugAllocator` | 인접 guard와 제한된 quarantine을 이용한 메모리 손상 진단 | 일반 호출을 동기화한다. 검사는 호출 시점에 수행하며 payload 접근은 별도 동기화가 필요하다. |
| `ExternalAllocator` | 외부 backend callback을 공통 인터페이스에 연결 | callback을 잠금 밖에서 병렬 호출한다. 실제 공급자와 모듈 수명은 별도 연결이 필요하다. |

가상 메모리와 Stack·Binned는 초기 구현이며, frame 수명 관리자, 자동 일괄 해제 stack, 스레드별 cache, page guard 기반 진단과 실제 jemalloc·TCMalloc·mimalloc의 제품 공급자 통합은 아직 제공하지 않는다. `DebugAllocator`의 바이트 guard와 제한된 poison·quarantine은 진단의 일부다. 현재 구현의 성능·단편화·최대 지연과 UE5 대비 성능은 측정하지 않았다.

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

강제 frame reset은 없다. 프레임 경계에서 사용하려면 객체 소멸과 작업·GPU 등 백엔드의 사용 종료를 상위 시스템이 먼저 보장하고, 모든 할당을 반환한 뒤 `Reset()`해야 한다. 임의의 live 포인터나 소멸하지 않은 객체를 자동으로 무효화하지 않는다.

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

`BinnedAllocator(backing)`는 정렬 16 이하인 요청을 16·32·64·128·256·512·1024·2048·4096바이트의 고정 pool 중 작은 적합 구간으로 보낸다. 더 큰 크기 또는 정렬은 backing에 직접 요청한다. 정확한 반환 주소와 객체 수명 관리는 호출자의 책임이다. `Trim()`·`Shutdown()`은 미반환 할당이 있으면 `Busy`이며, 성공한 `Shutdown()`은 재호출해도 성공한다. 종료 후 비영 할당·비null 반환·`Trim()`은 `ShuttingDown`이다. 인자 검증과 0바이트·null 처리는 종료 검사보다 먼저 이뤄진다.

변경 호출은 직렬화되며 backing callback은 호출 스레드의 잠금 안에서 실행된다. backing은 그 스레드에서 사용할 수 있어야 하고 재진입·다른 스레드의 동일 할당기 대기·역순 잠금으로 교착을 만들면 안 된다. 같은 스레드 재진입은 `Result`에서 `Busy`, `Stats()`·파괴에서 Fatal이다. backing의 일반 반환 실패 시 기존 소유 기록과 할당을 보존한다. 미반환 할당을 남기고 파괴하거나 pool 소유 저장소 반환이 거부되면 Fatal이다.

사용자 할당과 상위 청크·직접 할당 저장소를 각각 원장으로 관리한다. 두 원장은 별도 플랫폼 할당을 사용하므로 backing 예산에 기록 비용이 포함되지 않으며, 원장 할당 실패를 독립적으로 주입하는 테스트 경로는 아직 없다. 반환된 범위와 이 allocator가 보유한 저장소·원장 사이의 중첩 등 잘못된 상위 결과는 Fatal로 진단한다. 다른 고객의 저장소와 중첩되지 않도록 보장하는 것은 backing의 책임이며, 모든 외부 저장소에 대한 전역 중첩 검사는 아니다. pool과 원장의 잠금 및 선형 검색, 크기 구간의 내부 낭비를 고려해야 하며 처리량·단편화는 아직 측정하지 않았다. thread cache나 UE5 binned allocator와의 동등성은 제공한다고 주장하지 않는다.

## 플랫폼 가상 메모리

`platform::QueryVirtualMemoryInfo()`로 page 크기와 예약 정렬 단위를 조회하고, `VirtualMemoryRegion`의 `Reserve(bytes)` → `Commit(offset, bytes)` → `Decommit(offset, bytes)` → `Release()`로 단일 예약을 관리한다. 예약 크기와 변경 범위는 page 크기의 양수 배수여야 한다. 결과의 `code`와 OS 실패 시 `nativeError`를 확인한다. 일반 `Allocator`처럼 개별 객체를 관리하지 않는다.

메서드 간 호출은 직렬화되지만 `Address()`로 얻은 주소의 접근은 보호하지 않는다. 반환된 주소의 모든 사용과 다른 호출을 끝낸 뒤 `Decommit()`·`Release()` 및 파괴를 수행한다. 데이터는 실행 권한이 없다. macOS의 commit은 Windows commit charge와 같은 보장을 제공하지 않는다. decommit 후 즉시 물리 메모리가 반환되거나 재commit 시 내용이 0으로 초기화된다고 가정하지 않는다.

reserve 실패는 예약을 남기지 않는다. commit/decommit OS 실패 시 부분 변경 가능성이 있어 영역이 poisoned 상태가 되고 주소 조회는 null을 반환하며 `Release()`만 허용된다. release 실패도 소유권을 보존하고 poisoned 상태가 되므로 재시도해야 한다. 소멸 시 release 실패는 Fatal이다. 페이지 보호 fault probe와 일반 경계·동시 호출 테스트 코드는 존재하지만, OS 오류 및 부분 변경에 대한 fault injection은 아직 부족하며 Windows Core Debug에서는 일반 테스트와 보호 fault probe가 통과하였다. macOS 빌드·실행은 아직 검증되지 않았다.

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
