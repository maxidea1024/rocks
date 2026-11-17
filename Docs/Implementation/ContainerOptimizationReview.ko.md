# 컨테이너 최적화 검토

- 기준: `40bb47f`(origin/feature/engine-runtime, 좌표계 UE 전환 C1 포함)
- 브랜치: `feature/container-opt`(별도 worktree, push 안 함)
- 출발점: `af54a83` 기준 실험(`review/container-trivial-slack`, 56파일)의 패치를 옮긴 뒤 요구 1–7항과 대조해 보완했다.
  `af54a83..40bb47f`는 C1 커밋 하나뿐이고 실험이 고친 파일과 겹치지 않아 `git apply --3way`가 충돌 없이 적용됐다.

커밋은 셋이다.

| 커밋 | 내용 |
|---|---|
| (a) | `ConstructAt`·`DestroyAt` 제약, `InitializeAt` 분리, 호출부 이전(컨테이너의 K·V·T별 소멸 분기 포함) |
| (b) | Array 범위 경로(memcpy/memmove), 소멸 전용 순회·반환 직전 초기화 제거, Optional/Result/Variant trivial 소멸, UE 5.8 성장·축소 정책, 압축 해제기 정확 예약 |
| (c) | BitArray를 uint64 word 단위로 처리, 이 문서 |

`DestroyAt`이 trivial 타입을 거부하므로 컨테이너의 소멸 분기(`if constexpr`)는 (a)에 함께 들어가야 각 커밋이 컴파일된다.

## 1. ConstructAt·DestroyAt·InitializeAt

- `ConstructAt<T, Args...>`: `IsConstructibleV<T, Args&&...> && !IsTriviallyConstructibleV<T, Args&&...>`일 때만 호출할 수 있다.
  판정은 실제 생성 인자별이다(`IsTrivialV<T>` 하나로 가르지 않는다).
- `DestroyAt<T>`: `IsDestructibleV<T> && !IsTriviallyDestructibleV<T>`일 때만 호출할 수 있다.
- `ConstructAt<int>(p)`, `ConstructAt<int>(p, 7)`, `DestroyAt(int*)`는 컴파일 오류다. 함수 안에서 아무것도 하지 않는 방식이 아니다.
- `InitializeAt<T>(storage, args...)`: 값 초기화와 수명 시작의 generic 진입점이다.
  `InitializeAt<int>(p)`는 0, `InitializeAt<int>(p, v)`는 v를 저장한다. 인자 평가, 변환 생성자, 사용자 정의 이동 생성자의 부작용을 보존한다. non-trivial 생성만 `ConstructAt`으로 보낸다.
- 호출부: allocator bookkeeping(Arena·Binned·Budget·Debug·External·Pool·Stack·System), Name, Function, Delegate, SharedPtr, UniquePtr, Swap, 압축·암호·태스크·지역화.
  trivial 레코드의 `DestroyAt`은 `static_assert(IsTriviallyDestructibleV<...>)`로 바꿔 타입이 바뀌면 컴파일에서 드러나게 했다. 사용자 deleter와 자원 반환은 그대로 부른다.
- `SharedPtr::TryMake`는 T가 trivially destructible이면 `destroyObject`를 nullptr로 두고, `Function`은 `destroy_`를 nullptr로 둔다(호출 측이 nullptr을 건너뛴다). `TryAdopt`은 deleter를 불러야 하므로 늘 설정한다.

## 2. 소멸 호출과 순회 제거

- HashMap·OrderedEntryTable·OrderedMap은 K와 V를 각각 `IsTriviallyDestructibleV`로 판정한다. raw storage wrapper(Slot·Entry)의 trivial 여부로 판단하지 않는다.
- 소멸만 하는 반복문은 반복문 전체를 `if constexpr` 안에 둔다: HashMap `Reset`, OrderedEntryTable `DestroyEntries`, SlotArray `Release`, SparseArray `Clear`·`Release`, Array 재할당·삭제.
- 메타데이터(점유 상태, free list, generation, 점유 bit, 삭제 칸 수)는 그대로 갱신한다. SlotArray `Clear`는 trivial T여도 `FreeSlot`으로 generation을 올린다.
- 저장소를 바로 반환하는 경로의 불필요한 초기화를 없앴다. HashMap `Reset`은 슬롯 상태를 Empty로 덮지 않고, `Rehash`는 옛 슬롯을 Empty로 바꾸지 않는다. SparseArray `Release`는 점유 bitmap을 0으로 덮지 않는다(nodes 다음 bitmap의 반환 순서는 유지).
- 소멸·allocator 콜백이 남은 원소를 볼 수 있는 경로(EraseIf, OrderedEntryTable `DestroyEntries`)는 논리 삭제를 먼저 게시한 뒤 소멸하는 순서를 유지했다.
- OrderedEntryTable은 곧 K·V·hash·alive를 쓸 Entry를 값 초기화(0 채우기)하지 않고 기본 초기화만 한다.
- Optional/Result/Variant는 보관 타입이 모두 trivially destructible이면 constrained defaulted 소멸자로 래퍼도 trivially destructible이다.

## 3. Array 범위 연산

- `CanBitwiseMove = IsTriviallyCopyableV<T> && IsTriviallyMoveConstructibleV<T> && !IsVolatileV<T>`. 복사 경로는 `IsTriviallyCopyConstructibleV`로 따로 판정한다.
- 적용: 재할당·relocation, 복사·이동 append, 중간 삽입(memmove), 순서 유지 삭제(memmove), swap 삭제(memcpy — 원본 `[Size()-moved, Size())`와 목적지가 겹치지 않음), Resize의 값 초기화(trivial 기본 생성·소멸 타입은 배열 값 초기화로 memset).
- 계약:
  - 자기 원소 삽입은 이동 전에 값을 확보하고, 재할당 경로는 옛 버퍼가 살아 있을 때 새 원소를 만든다. 자기 배열 append도 relocation 전에 tail을 복사한다.
  - 할당 실패는 원소·크기·용량·주소와 입력(Span·`Array&&` 원본)을 보존한다.
  - trivial 소멸만으로 사용자 정의 복사·이동 생성자를 생략하지 않는다(시험이 이동 횟수를 센다).
  - Resize는 값 초기화(int는 0), `TryAppendUninitialized`는 초기화 생략(trivial 타입 전용)으로 구분된다.
  - 빈 범위: `platform::MemoryCopy/MemoryMove`는 0 byte면 포인터를 쓰지 않고, 값 초기화도 count가 0이면 배열 new를 하지 않는다.
- 이번에 보완: 값 초기화의 배열 new를 trivial 소멸 타입으로 제한했다(소멸이 non-trivial이면 원소별 `InitializeAt`). 배열 new cookie가 생길 여지를 없앤다.

## 4. BitArray word 처리

`BitsPerWord = 64`, `WordShift = 6`, `WordMask = 63`. word index는 shift, word 안 위치는 AND다.

| 연산 | 처리 |
|---|---|
| SetRange, Resize | 첫·끝 부분 word는 mask, 가운데 word는 MemorySet |
| Find, Contains, FindAndSetFirstZeroBit | word 검사 + `CountTrailingZeros64`, 마지막 word padding mask |
| CountSetBits, TryCountSetBits | word `PopCount64` + 경계 mask |
| Insert, RemoveAt, RemoveAtSwap | 목적지 word 경계 단위로 최대 64bit 이동, 겹침 방향에 따라 뒤에서부터 |
| Clone | 이번에 보완: `TryReserve(Size())`(정확 크기) + 사용 word 복사. 이전에는 성장 slack을 잡고 0으로 채운 뒤 덮어썼다 |
| Reserve | 배열 수명 시작 + 사용 word 복사 + 새 word 0 |
| Combine | AND/OR/XOR별 word 연산, Maximum/Minimum/Maintain과 missingValue 유지, 자기 자신 입력 허용 |
| Invert, Clear | 사용 word만 처리, 크기 밖 bit는 0 불변식 유지 |

경계: `LowMask(64)`는 shift를 평가하지 않고, 다음 word는 필요한 bit가 거기 있을 때만 읽는다. `RemoveAtSwap`의 원본 시작점은 기존 계약대로 `Size() - count`다.
`/64`·`%64` 표기 변경만으로 성능을 주장하지 않는다. 개선은 per-bit 반복을 per-word 반복으로 줄인 데서 온다.

## 5. 성장·축소 정책

UE 5.8.4 `Engine/Source/Runtime/Core/Public/Containers/ContainerAllocationPolicies.h`(로컬 클론, 읽기만)를 다시 확인했다.
`CONTAINER_INITIAL_ALLOC_ZERO_SLACK = 1`, `AGGRESSIVE_MEMORY_SAVING = 0`에서 `DefaultCalculateSlackGrow`는 첫 할당 `max(4, NewMax)`, 이후 `NewMax + 3 * NewMax / 8 + 16`이고 `NewMax > Retval`이면 최대값으로 포화한다.
`DefaultCalculateSlackShrink`는 `(slack byte >= 16384 || 3 * NewMax < 2 * CurrentMax) && (slack 원소 > 64 || NewMax == 0)`일 때 줄인다. Rocks에는 allocator quantization이 없어 쓰지 않는다.

- `detail::GrowthCapacityWithin(capacity, required, maximum)`: capacity는 첫 할당 여부만 가리고 크기는 required로 정한다.
  `floor(required * 3 / 8)`를 `(required / 8) * 3 + (required % 8) * 3 / 8`로 곱셈 overflow 없이 구하고, 덧셈 전에 남은 폭과 비교해 maximum에서 포화한다.
- Array(`ArrayGrowthCapacity`·`TryEnsureCapacity` 포함), SparseArray, SlotArray가 같은 함수를 쓴다. BitArray는 같은 함수를 uint64 word 수에 적용한다.
  ccf0354의 `TryEnsureCapacity`와 `GrowthCapacityWithin`은 이 정책 하나로 맞춰졌다. 시험 `ArrayEnsureCapacityGrowsGeometricallyUnlikeExactReserve`의 기대값은 성장 함수로 다시 계산하지 않고 손으로 구한 용량 표(4, 22, 47, 82, 130 — 할당 5번)로 바꿨다.
- 명시적 Reserve는 요청 크기(BitArray는 word 경계, 맵은 power-of-two 슬롯)만 쓴다.
- `detail::ShrinkCapacity(capacity, required, elementSize)`: byte 조건은 `slack > 16383 / elementSize`, 원소 조건은 `required / 2 < slack`(= `3r < 2c`)로 곱셈 없이 같은 경계를 판정한다.
- 축소 API: Array·SparseArray `TryShrinkSlack()`, HashMap·HashSet·OrderedMap·OrderedSet `ShrinkSlack()`.
  삭제·Resize의 용량 유지와 exact `TryShrink`·`Compact`는 그대로다. 맵은 open addressing의 power-of-two 슬롯과 최대 부하율 1/2를 유지하고 축소 판정만 UE 식을 쓴다(UE의 chaining 버킷 정책은 옮기지 않았다).
- SlotArray는 generation 이력을 버리면 stale handle이 되살아날 수 있어 축소 API를 두지 않았다.

## 6. 압축 해제기

해제기는 다음 크기와 출력 상한을 스스로 정한다. 여기에 Array slack을 겹쳐 얹으면 피크 메모리가 해제 예산을 넘는다(기존 압축 폭탄 시험이 검출).
`AppendDecodeStorage`는 overflow를 검사한 뒤 `Size() + bytes`를 정확히 예약하고 `TryAppendUninitialized`로 늘린다. 실패하면 길이·내용·용량을 보존한다.
적용: LZ4 frame·Brotli(`DecodeOutput::Grow`), zlib(`InflateOutput`), zstd, 사용자 등록 형식, 그리고 이번에 보완한 LZ4 raw(`Lz4Codec.cpp`, 2배 재시도 루프).
고정 64KiB씩 늘리는 스트림 해제(`CompressionStream`)는 해제기가 기하 성장을 정하지 않으므로 Array 성장 그대로 둔다. 메모리 상한 시험 기준은 완화하지 않았다.

할당 실패 시험(`CoreTests` Array 수명 시험)은 성공 경로의 실제 할당 횟수를 먼저 세고 모든 실패 지점을 순회한다. BitArray 시험은 명시적 Reserve로 재할당 경계를 만든다.

## 7. 시험

- `ContainerOptimizationTests.cpp`: 제약 concept·타입 특성 static_assert, 성장·축소 경계의 독립 상수(포화 포함), InitializeAt 부작용, 사용자 이동 생략 금지(삽입·삭제·swap·재할당 이동 횟수), 자기 별칭·중첩·빈 범위·할당 실패 보존, HashMap·OrderedMap의 (int|Owned) × (int|Owned) 수명, 축소 hysteresis·맵·SparseArray 축소.
- `BitArrayWordTests.cpp`: 독립 bool 모델 비교(이동·검색·집계·Combine·Clone·Reserve·축소 후 재확장), 0·1·63·64·65·127·128·129 등 경계, word slack 성장. BitArray.h와 SystemAllocator만 써서 sanitizer 하네스가 같은 소스를 그대로 쓴다.
- `ArrayGrowthReviewTests.cpp`: `TryEnsureCapacity` 용량 표와 할당 5번.

## 검증

Windows x64, clang 21(Swift 6.3.3 toolchain), 새 worktree에서 실행했다. 검증 축소 방침(2026-11-17)에 따라 Asan·Asan EngineTests·Release·Android는 돌리지 않았다(Asan 단계는 시작했다가 중단).

| 단계 | 대상 | 결과 |
|---|---|---|
| (a) 상태 Debug Foundation | 70c5e55 | 1,852 시험 통과, 도구 시험 통과 |
| (b) 상태 Debug Foundation | 94d829f | 1,858 시험 통과. 첫 실행에서 Meta 도구 시험 2개(`EnumDescriptorIsReadStrictly`: 임시 include 폴더 없음, `FixtureDeterminismTimestampAndStaleCheck`: 타임스탬프 재작성)가 실패했고 재실행에서 204/204 통과 — 같은 기계의 다른 세션 빌드와 겹친 환경 요인으로 보며 컨테이너 변경과 관계없다 |
| 최종 Debug Foundation | (c) | 1,863 시험 통과(ContainerOptimization 7개·BitArrayWord 6개 포함) |
| Commandlet 빌드·시험, format, meta, Build.Tests, Build.IntegrationTests | (c) | 통과 |
| EngineTests Debug | (c) | 1,949 시험 중 1,948 통과. `Process_RunTimeoutTerminatesChild`(300 ms 안에 자식이 1,000 byte를 넘게 써야 함)가 다른 세션 빌드로 부하가 클 때 실패했다. 부하가 내려간 뒤 이 바이너리와 컨테이너 변경이 없는 메인 트리 바이너리 모두 8/8 통과해 기존 시험의 시간 의존으로 본다(이번 변경과 무관, 시험 자체는 고치지 않음) |
| policy(`Rocks.ToolTests policy`) | (c) | 통과 |
| `git diff --check` | 각 커밋 | 통과 |
| 빈 줄 규칙(추가된 `}` 뒤) | 각 커밋 | 스크립트로 확인, 위반 0 |

요구 7항 개별 확인(probe는 worktree 밖 scratchpad에서 clang으로 실행):

- `ConstructAt<int>(p)`, `ConstructAt<int>(p, 7)`, `DestroyAt(int*)`: 모두 "constraints not satisfied"로 컴파일 실패.
- `Optional<int>`·`Result<int>`·`Variant<int, float>`는 trivially destructible, non-trivial 보관 타입이면 아님(static_assert probe와 시험).
- O0: 대표 trivial 컨테이너 번역 단위에서 `DestroyAt` 참조 0개(변경 전 42개), `ConstructAt`은 HashMap Slot(사용자 생성자) 하나(변경 전 38개).
- O2: `Array<int>` 삽입은 memmove(+ 재할당 memcpy 2회), 순서 삭제는 memmove, swap 삭제는 memcpy, append·relocation은 memcpy, Resize 값 초기화는 memset으로 내려간다.
- BitArray 경계: `BitArrayWordTests.cpp`를 그대로 하네스로 묶어 clang `-fsanitize=address,undefined -fno-sanitize-recover=undefined`로 실행해 5개 시험 0 실패.
  같은 설정에서 64bit shift(UBSan)와 heap overflow(ASan) 음성 대조가 실제로 검출되는 것도 확인했다. 하네스 할당기는 ASan이 확실히 가로채는 malloc을 쓴다.
- 기존 실험에서 실패한 `Process_ParentHandlesAreNotInherited`·`Process_UnicodeExecutableAndWorkingDirectory`는 새 worktree의 (a)·(b)·최종 Debug 실행 모두에서 통과했다. 현재 HEAD(`40bb47f`) 단독 빌드로는 다시 돌리지 않았다(검증 범위 축소). 실험 당시 실패는 실행 위치·환경 요인으로 보인다.

## API·타입 특성·ABI 영향

- 소스 호환성: `ConstructAt<trivial>`·`DestroyAt(trivial*)`는 컴파일 오류다. generic 생성은 `InitializeAt`, 소멸은 `if constexpr` 분기가 필요하다.
- 새 공개 API: `InitializeAt`, Array·SparseArray `TryShrinkSlack`, HashMap·HashSet·OrderedMap·OrderedSet `ShrinkSlack`. 제거·이름 변경된 API는 없다.
- 타입 특성: `Optional<T>`·`Result<T>`·`Variant<Ts...>`는 보관 타입이 trivially destructible이면 trivially destructible이 된다. 복사·이동 생성은 사용자 정의 그대로라 trivially copyable은 아니다.
  by-value 전달 규약은 복사·이동 생성자의 trivial 여부에도 달려 있어 이번 변경으로 바뀌지 않을 것으로 보지만, 소멸자 특성이 바뀐 만큼 의존 모듈은 함께 다시 빌드해야 한다.
- 레이아웃: 컨테이너·래퍼의 크기와 멤버 배치는 바뀌지 않았다. `SharedControlBlock::destroyObject`·`Function::destroy_`는 nullptr일 수 있다(내부 계약).
- 동작: 자동 성장 용량 패턴이 바뀐다(첫 4, 이후 `r + 3r/8 + 16`). 성장률이 1.5배에서 약 1.375배로 낮아져 slack 메모리는 줄고 큰 배열의 반복 추가는 재할당이 조금 늘어난다.

## 벤치마크(측정용, 시험 아님)

같은 `bench.cpp`를 변경 전(`40bb47f`) 헤더와 변경 후 헤더로 각각 clang 21 `-O2`로 빌드했다(int 원소, CRT 정렬 할당기).
같은 기계에서 다른 세션의 빌드가 돌고 있어 잡음이 크다. 잡음은 시간을 늘리는 쪽이므로 18개 프로세스 × 각 7회 반복 중 최솟값을 주 지표로, 중앙값을 참고로 둔다.

| 연산 | 변경 전 최소 | 변경 후 최소 | 배율 | 중앙값(전 → 후) |
|---|---:|---:|---:|---:|
| Array 1M개 PushBack | 3.80 ms | 4.47 ms | 0.85 | 4.90 → 6.09 ms |
| Array 8개 PushBack × 100k | 8.94 ms | 6.10 ms | 1.47 | 12.51 → 9.20 ms |
| Array 가운데 삽입 × 20k | 17.67 ms | 2.72 ms | 6.50 | 25.50 → 3.97 ms |
| Array 가운데 삭제 × 20k | 20.21 ms | 8.76 ms | 2.31 | 32.69 → 12.31 ms |
| BitArray Find 1Mbit(4099칸마다 1) × 20 | 8.29 ms | 0.077 ms | 108 | 9.15 → 0.146 ms |
| BitArray CountSetBits 1Mbit × 50 | 12.99 ms | 1.55 ms | 8.4 | 19.06 → 1.90 ms |
| HashMap<int32,int32> 200k 삽입 + 삭제 | 14.89 ms | 14.73 ms | 1.01 | 21.84 → 24.23 ms |

- 1M개 PushBack이 느려진 것은 성장률이 1.5배에서 약 1.375배(UE)로 낮아져 재할당이 31번 → 33번, 누적 복사량이 늘었기 때문이다. 대신 남는 slack 메모리는 줄어든다(정책 선택의 대가이며 범위 경로 회귀가 아니다).
- 작은 배열은 첫 4 다음 바로 22로 커져 재할당이 줄었다.
- HashMap은 이번 변경이 int 키·값의 탐사·재해시 경로를 바꾸지 않아 차이가 잡음 범위다.
- O0 호출 제거 확인(같은 probe 번역 단위): 변경 전 `DestroyAt` 참조 42개·`ConstructAt` 38개 → 변경 후 `DestroyAt` 0개, `ConstructAt`은 사용자 정의 생성자가 있는 HashMap Slot 하나뿐이다.

## 미검증 범위

- 전체 Asan·Release·Android 빌드(방침에 따라 생략). BitArray 경계만 별도 ASan·UBSan 하네스로 확인했다.
- Linux·macOS·Android 실기 실행.
- 현재 HEAD(`40bb47f`) 단독 빌드에서 Process 시험 2개 재확인.
- MSVC(cl.exe) 컴파일과 codegen: clang만 확인했다.
- volatile 원소의 실행 시험: `Array<volatile int>`는 원소 주소가 `void*`로 변환되지 않아 변경 전(`ConstructAt`)과 후(`InitializeAt`) 모두 인스턴스화되지 않는다(probe로 확인). `volatile int`는 trivially copyable이므로 byte 경로의 `!IsVolatileV<T>` 조건은 방어로 남겼다.
- 큰 원소·정렬이 큰 타입(alignas 64 이상)의 범위 경로 벤치마크, 실제 엔진 장면 단위 메모리·처리량 측정.
- 압축 해제기의 정확 예약이 아주 큰 출력에서 재할당 횟수를 늘리는지: 해제기 스스로 2배 성장을 정하므로 영향은 작다고 보지만 측정하지 않았다.
- 축소 API는 아직 엔진 호출부가 없다. 언제 부를지는 쓰는 쪽 결정이다.

## 남은 최적화 후보

- SparseArray·SlotArray의 union wrapper(Node·Slot) 생성과 점유·generation 메타데이터 작업, 리플렉션 runtime destroy callback 계약은 이번 범위에서 재설계하지 않았다.
- SparseArray 빈 칸 찾기(`allocated_.Find(false)`)는 이제 word 단위지만 free list와 bitmap을 함께 유지한다. bitmap만으로 free list를 대신하면 메모리와 갱신을 줄일 수 있다.
- HashMap `Rehash`는 새 슬롯 전체를 생성자로 초기화한다. 상태 배열을 분리하거나(SoA) memset 가능한 표현으로 바꾸면 줄일 수 있다.
- trivially relocatable(이동 + 소멸을 byte 복사로 대신할 수 있는 타입, 예: UniquePtr·Array 자신) 표지를 두면 non-trivial 원소의 재할당도 memcpy로 할 수 있다(UE `TIsBitwiseConstructible`·`TCanBulkSerialize` 계열과 같은 방향).
- `TryEmplaceAt` 재할당 경로는 앞·뒤 두 번 memcpy한다. 한 번의 relocation 뒤 memmove로 바꾸는 것과 비교해 볼 만하다.
- BitArray `TryClone`은 빈 사본에 `TryReserve`를 부르므로 새 word를 0으로 채운 뒤 사용 word를 다시 복사한다. 사본 전용 할당 경로를 두면 한 번만 쓴다.
- 락프리 컨테이너는 추가하지 않았다.
