# Rocks Core 상세 기능·구현 명세

기준일: 2026-11-11. 상태: 기능 인벤토리·계약·작업 순서. 실제 구현 완료 증거가 아니다. 관련 현황은 저장소의 `Docs/Implementation/FoundationStatus.ko.md`, `Docs/Design/UnrealCoreFeatureAudit.ko.md`, `Docs/Design/PlatformLayer.ko.md`와 해당 소스·테스트를 우선한다.

## 1. 기준과 범위

- 대조 기준은 **UE 5.6 Core**다. 기준 버전의 공개 헤더와 공식 문서를 고정 커밋 단위로 비교한다. Rocks는 이름이나 ABI를 복제하지 않지만 기능 단위로 대응 경로가 있어야 한다. Blueprint는 제외하며 자체 언어는 상위 계층에서 연결한다.
- 기존 감사표의 UE-C01~C33을 출발점으로 삼되 미기재 기능을 제외하지 않는다. 공개 헤더/심볼 전체를 기계적으로 추출·분류해 별도 인벤토리를 만들고, 중복·deprecated·플랫폼 전용·상위 모듈과 대응 불필요 사유를 검토한다. 이 명세 자체는 UE5.6 전수 목록이 아니며 최종 누락 없음의 증거가 아니다.
- 이 환경의 `work/ue5-reference`에 있는 고정 커밋 `cdda65cecacd9a1278020925c357bf4cc0b17e8c`의 제한된 5.6 참조 파일(예: `Array.h`, `BitArray.h`, `GenericPlatformFile.h`, `Paths.h`) 및 기존 감사표의 링크를 참고했다. Epic 원본은 접근 권한·라이선스 확인이 필요하다. 아래 기능 중 원본 확인이 안 된 항목은 **Rocks 목표 또는 검토 후보**이며 해당 기능을 UE5.6에서 확인했다고 주장하지 않는다. 공개 API 웹 페이지가 다른 버전을 표시하면 5.6 근거로 취급하지 않는다.
- 책임 범위: `Base`(표현·checked 산술) → `PlatformBase`(OS/compiler/CPU 경계) → `Core`(allocator·컨테이너·유틸·수학) → `Task`/`Localization`/`Serialization`/`MetaSchema`/`ObjectRuntime`. 물리 엔진과 충돌 질의 중 월드·동역학·broadphase는 상위 계층이고 Core는 순수 기하 프리미티브와 명시된 접촉/교차 함수부터 제공한다.
- 사용자 요구: C++23, 엔진 소유 Runtime과 공개 API의 STL 비사용, 예외·RTTI 없는 경로, 컴파일러 경고를 오류로 처리, 한국어 사용·주의 주석, clang-format 4-space, 직관적인 API, Windows/macOS 개발 환경, 향후 다른 플랫폼 확장. 안정성·정확성 우선, 확장성 다음, 성능은 계측을 근거로 개선한다. 메모리 누수 0은 검증 목표이며 모든 상황에 대한 선험적 보증이 아니다.

### 상태·우선순위

`부분`은 해당 파일·일부 API가 존재함을 뜻하며 동등성 또는 플랫폼 전체 통과가 아니다. `후속`은 목표이나 미구현 또는 미확인, `확인 필요`는 코드 인벤토리로 아직 판정하지 않은 상태다. `P0`은 기반 안전성·범용 사용 경로, `P1`은 다음 사용성·완성도, `P2`는 고급 운영/성능 확장이다. 우선순위가 범위 제외나 즉시 완료를 뜻하지 않는다.

## 2. 의존 방향과 배치

```text
Base → PlatformBase → Core → Task / Localization / Serialization / MetaSchema
                                 ↓           ↓              ↓
                           ObjectRuntime ← MetaSchema → ObjectSerialization
                                 ↓
                    Engine / Physics / Renderer / Editor / Tools
```

화살표는 소비자가 기반을 의존하는 관계를 개념적으로 나타낸다. 상위 모듈이 Core 구현으로 역의존하지 않는다. 실제 `ObjectSerialization`은 ObjectRuntime과 Serialization을 의존한다. 운영체제 헤더와 동적 라이브러리/CPU별 intrinsic은 각 private 플랫폼 구현에 둔다. 공용 generic 인터페이스·기본 구현을 먼저 정하고 플랫폼별 구현이 필요한 연산만 대체한다. 플랫폼마다 `#if`를 기능 코드에 흩뿌리지 않는다. 모듈·공개 헤더 단독 컴파일과 순환 의존 금지를 CI에서 확인한다.

## 3. 범주별 기능 인벤토리와 설계 계약

### 3.1 Base·메모리·수명

| ID/우선 | 기능·상태 | 완료 조건과 검증 |
|---|---|---|
| B01 P0 | 고정 폭 signed/unsigned, 크기·포인터 차이, 정렬, 엔디언, GUID; `Types.h`, `Checked.h`, `Guid.h` **부분** | checked add/sub/mul/align/변환, shift, 0/최대/음수·ABI 테스트. 네이티브 포인터 크기·padding을 영속 데이터에 직접 사용하지 않는다. |
| B02 P0 | compiler/OS/CPU 속성·capability, 진단/내장함수 경계 **부분** | 미지원 기능은 빌드·초기화 오류로 드러낸다. Clang Win64/macOS x64·ARM64 단독 헤더, 경고 음성 대조. |
| M01 P0 | allocator 요청 크기·alignment·0·overflow·소유권·해제·재할당 계약. System/Arena/Stack/Pool/Binned/Budget/Debug 및 외부 backend **부분** | 할당 실패 전 상태 보존, allocator 수명·교차 해제·중복 해제, 누수 계수, 조각화와 false sharing 측정. Realloc/TryRealloc의 보존·alignment·0 semantics를 정하고 모든 구현의 지원 여부를 명시한다. |
| M02 P0 | 가상 메모리 reserve/commit/decommit/protect/release, page size **부분** | OS 오류, 페이지 경계·부분 변경·cleanup 재시도, fault injection, 실제 Windows/macOS. 보호·page guard 등 미구현 세부 사항은 별도 판정한다. |
| M03 P1 | frame/scratch, stack mark/rewind, slab, per-thread cache/trim, 큰 할당 **후속** | reset 이후 포인터·destructor·스레드 소유권, nested mark, OOM·회수·통계, 엄격 LIFO allocator와 growing container의 비호환을 진단한다. |
| M04 P1 | canary/poison/quarantine, tag/stack trace, live allocation, leak/failure injection, 외부 allocator **부분** | 이중 반환·오소유·guard 훼손·정렬·동시성, 계측 메모리 예산, 오류 보고의 재귀·OOM, 실제 jemalloc/TCMalloc 등은 라이선스·배포·플랫폼 지원 후 통합. |
| M05 P1 | unique/shared/weak native 소유권과 deleter, scope guard — `Core/Memory/UniquePtr.h`(할당기 기반 단독 소유, 기반 타입 이동 시 원래 할당 주소 보존, 해제 실패 Fatal), `Core/Memory/SharedPtr.h`의 `SharedPtr`/`WeakPtr`(UE ThreadSafe TSharedPtr/TWeakPtr 대응: 객체·제어 블록 단일 할당, 원자 계수, 0이 아닐 때만 증가하는 Lock, 원래 타입 소멸 함수 보존), `SharedRef`(UE TSharedRef 대응, null 불가·이동도 복사)와 `EnableSharedFromThis`(UE TSharedFromThis 대응, weak 토큰만 보유), `Core/ScopeExit.h` **부분**. 사용자 deleter·aliasing 생성은 **후속** | 원자성/메모리 순서·deleter·제어블록 allocator 수명·weak lock 경합·cycle·동적 모듈 종료. GC root/weak object reference와 혼동하지 않는다. |

### 3.2 컨테이너·알고리즘·일반 utility

| ID/우선 | 기능·상태 | 완료 조건과 검증 |
|---|---|---|
| C01 P0 | `Array`, `Span`의 조회·용량·범위 삽입/삭제·검색·반복, 정적/inline 배열·small buffer; 동적 배열·span **부분**, 나머지 **후속** | alias 포함 insert/append, overflow/OOM rollback, move-only·생성/파괴 수명, view 무효화, allocator 수명·Stack/LIFO 제약. 명확한 빈 범위와 iterator 경계. |
| C02 P0 | `HashMap`/`HashSet`, policy·이종 조회·중복·iteration; **부분** | 충돌·tombstone·reserve/rehash·seeded random oracle, 잘못된 hash/equality 계약, OOM 보존, 삽입 입력 이동 및 key/value 교체 정책. 현재 Rocks의 중복 의미와 UE의 차이를 문서화한다. |
| C03 P0 | `BitArray` **부분**, `SparseArray`/stable slot 및 generation **후속** | word 경계·tail bit masking·self alias·remove swap 차이, slot free-list·stale handle·generation wrap·OOM, 메모리 사용량과 반복 순서. |
| C04 P1 | queue/deque/ring·heap/priority queue·multi-map/set·tree **후속** | 소유권·용량·순서·포인터 무효화·overflow·OOM. 단일 스레드 queue와 동시 queue는 별도 계약이며 SPSC/MPSC/MPMC, bounded/unbounded, blocking/close를 명확히 구분한다. |
| C05 P0 | 범용 sort/lower/upper/binary search와 `Array::Sort` **부분** | 비안정/안정 정렬 구분, comparator 엄격 약순서·noexcept·이종 키 양방향, 포인터 자동 역참조 정책, move-only 수명·대조 oracle·최악 복잡도. stable sort·projection·filter·transform·heap 등은 **후속**. |
| C06 P0 | traits/concepts, `Move`, `Result`, `Function`, `Optional`(`Core/Optional.h`, UE TOptional 대응: 빈 접근 Fatal, 자기 별칭 Emplace 안전), `Tuple`(`Core/Tuple.h`, UE TTuple 대응: 구조적 바인딩·Tie·Apply, std trait 특수화는 이 파일에만 허용), `Variant`(`Core/Variant.h`, UE TVariant 대응: 정확한 타입만 암묵 변환, 다른 대안 접근 Fatal, 자기 참조 안전 교체), `Delegate`/`MulticastDelegate`(`Core/Delegate.h`, UE TDelegate/TMulticastDelegate 대응: SharedPtr 약한 바인딩, 실행 중 자기 해제 지연, Broadcast 중 추가·제거·중첩 안전) **부분**; Pair, delegate payload·GC 객체 바인딩·스레드 안전 모드·event **후속** | 빈 상태, move-only, 비복사성·noexcept, compile-negative, in-flight callback의 해제/재진입/모듈 수명. `Result<bool>` 성공과 값의 혼동을 편의 API에 노출하지 않는다. |
| C07 P1 | range/view·iterator·hash·비트·enum flag·scope exit·version — `ScopeExit`/`ROCKS_ON_SCOPE_EXIT`(UE ON_SCOPE_EXIT 대응, noexcept 정리·Dismiss) 구현, 나머지 **부분/후속** | dangling view, 잘못된 비트/shift·부호·정렬·overflow, endian·hash golden, 범용 비암호 hash와 보안 hash의 구분, cleanup 실패 진단. |

### 3.3 문자열·국제화·경로

| ID/우선 | 기능·상태 | 완료 조건과 검증 |
|---|---|---|
| T01 P0 | 소유 `String`/비소유 `StringView`, append/insert/replace/find/split·join/builder/format **부분** | UTF-8 byte 인덱스 계약, embedded NUL, malformed UTF-8, 빈 값·중첩 alias·OOM rollback, 경로 연산 시 `/` 지원 및 절대 경로 정책. 사용자가 인코딩 단위와 view 수명을 추측하지 않도록 한다. |
| T02 P0 | interned `Name`, None·번호·case 비교·표기 **부분** | pool/domain 수명·동시 종료·lease·대소문자·번호 경계·hash/equality·unicode 정책, 안정 영속 ID와 네트워크 표현을 런타임 token과 분리한다. |
| T03 P1 | Unicode validation·UTF-8/16/32 변환, normalization/case folding·locale **후속** | Unicode 버전과 외부 라이브러리/라이선스, invalid sequence 정책, surrogate·분할 스트림·확장 문자·멀티바이트 인덱스 golden/fuzz. ASCII 기능을 Unicode 전체 지원으로 표시하지 않는다. |
| T04 P1 | `Text` 지역화 catalog·culture snapshot, plural/gender·숫자·통화·날짜 **부분** | fallback·동시 culture 변경·번역 누락·형식 오류·시간대·대형/음수, 에디터 추출 및 쿠킹 연동. 표시용 Text와 내부 식별 Name을 구분한다. |
| T05 P0 | 공용 경로 lexical 처리·플랫폼 호스트 문법 선택 **부분** | Windows drive/UNC/root-relative·POSIX root, 상대 계산·정규화·확장자·path join·Unicode·embedded NUL·대소문자. 문자열 정규화만으로 symlink 포함 관계·보안 경계·동일 파일을 보증하지 않는다. |

### 3.4 수학·기본 충돌·SIMD

2026-11-11 기준 `PlatformBase/Public/Platform/Math`(Generic→Clang→SSE2/NEON 상속, 4-lane scalar/SSE2/NEON 레지스터)와 `Core/Public/Core/Math`(벡터·행렬·투영·quaternion·Rotator·Transform·Rect·색·난수·일괄 변환)를 구현했다. 상세 계약·UE 5.6 차이·검증 범위는 [수학 구현](../Implementation/Math.ko.md)을 따른다. 사용자 지시에 따라 UE처럼 CPU별 최적화 함수를 선택하는 자체 구현을 먼저 두고, 공개 구현(UE 5.6 고정 커밋)은 의미 대조에만 쓰며 코드를 복제하지 않는다. 외부 backend 후보 평가는 batch·고급 kernel의 측정 기반 교체 기준으로 유지한다. 아래 표의 상태는 구현 범위이며 macOS·ARM64 실행과 성능은 미검증이다.

| ID/우선 | 기능 | 계약·검증 |
|---|---|---|
| G01 P0 | scalar: clamp/min/max/lerp, degree/radian, 절대·상대 오차, 유한성, floor/ceil/round, remainder, safe divide/sqrt, 근삿값 — `MathUtility.h`·`PlatformMath` **부분**(보간·Wrap·GridSnap·정수 변환·Estimate 포함, safe divide·상대 오차 helper는 후속) | NaN/Inf/서명된 0·subnormal·오버플로·도메인 오류와 반올림은 함수별로 정한다. 잘못된 입력을 조용히 0으로 바꾸지 않는다. double precision 기준 및 golden vector. |
| G02 P0 | `Vector2/3/4`, 정수 벡터·점, 크기·내적·외적·거리·정규화·투영·보간 — `Vec2/3/4(d)`, `IntVec2/3` **부분**(IntVector4·unsigned 변형 후속) | float/double 타입과 정렬, 0 벡터 및 실패 의미, point/direction 구분, 점곱/외적·정규화 불변식, scalar/SIMD differential. |
| G03 P0 | quaternion, rotation, matrix3/4, affine `Transform`, inversion·composition·방향/위치 변환 — `Quat(d)`, `Rotator`, `Mat4(d)`, `MatrixFactory`, `Transform` **부분**(Matrix3·dual quaternion 후속, 열 우선 저장·(x,y,z,w) 순서 결정) | 기존 설계의 오른손·+Z 위·+X 전방·미터·라디안·초와 column vector·`world=parent×local`, reversed-Z [0,1]을 유지하고 예제로 고정한다. 행렬 메모리 배치와 회전 세부 순서는 별도로 결정한다. 퇴화·비가역·음수/비균일 scale·오차 누적을 검사한다. |
| G04 P1 | plane, ray, line/segment, sphere, AABB, oriented box, capsule, triangle, frustum, 2D rect·bounds — `Shapes.h`(frustum 제외 8종), `Rect.h`(`Box2`, `IntRect`), `Interval.h` **부분**, frustum·BoxSphereBounds **후속** | 유한값/음수 반경/역전 min-max 등 생성 검증, 빈·퇴화 shape, 경계 접촉 포함/제외, 축 및 좌표 공간 계약. |
| G05 P0 | 기본 교차·포함·거리·최근접점, ray/segment 대 plane/sphere/AABB/triangle, sphere-sphere·sphere-AABB·AABB-AABB — `Intersection.h` **구현**(Windows x64 실행 검증, 지원 조합은 [수학 구현](../Implementation/Math.ko.md) §2.1) | 순수 함수, 단위 방향 요구 여부·매개변수 범위·hit point/normal/distance 의미, 시작점 내부·접선·평행·0 길이·뒤쪽·퇴화·epsilon. 대칭성과 double reference·무작위 property·경계 golden 검증. 처음에는 모든 조합을 구현했다고 표시하지 않는다. |
| G06 P1 | OBB/capsule/frustum 교차, 삼각형·mesh 기초, swept shape·접촉 정보 — OBB–OBB·Capsule–Sphere/Capsule·광선 대 Capsule/OBB **부분**, frustum·swept·접촉 정보 **후속** | 지원 쌍·양방향·normal 방향·접촉 깊이·불변식, broadphase/physical response는 별도 물리 계층에 남긴다. 정확한 불변식과 비지원 조합 명시. |
| G07 P0 | 색, linear/sRGB 변환, packed 정규화, 랜덤/시드 — `Color`·`LinearColor`·`RandomStream`(PCG32) **부분**(Float16 색·이름 색 목록·noise 후속) | 채널 범위·alpha·전달 함수·rounding·NaN·정확한 golden. 결정적 PRNG 알고리즘/seed, 품질·상태 크기·스레드 정책과 보안용 금지를 문서화한다. |
| G08 P0/P1 | SIMD(x64 SSE/AVX, ARM64 NEON 가능성) 추상화·scalar fallback·기능 감지/dispatch — SSE2/NEON/scalar 4-lane 레지스터와 CPU별 PlatformMath **부분**(AVX2/FMA dispatch·double 레지스터·SoA batch 후속, NEON 실행 미검증) | 코드 경계 내 intrinsic, OS 상태까지 확인한 안전한 dispatch, 미지원 CPU fallback, 정렬/alias, SIMD vs scalar 허용 오차·특수값·실 CPU 매트릭스. 부동소수점 결과의 플랫폼 간 비트 단위 결정성은 보장 여부를 따로 정한다. |

#### 수학·충돌 설계의 선행 결정과 검증 순서

기존 전체 계획의 오른손 좌표계에서는 +X 전방과 +Z 위에 따라 +Y는 왼쪽이며 `Cross(+X,+Y)=+Z`다. 월드 위치 double과 카메라 상대 float의 변환은 double 원점 차감 후 수행한다. renderer·physics·asset 입출력의 좌표·단위 변환은 명시적으로 검사한다. 표현 타입과 수치 연산은 검증된 상용 공개 backend의 라이선스·버전·지원 플랫폼·STL/예외 의존성을 확인하고 adapter로 연결한다. backend가 지원하지 않는 기능은 다른 후보나 독립 구현을 별도 심사한다.

벡터는 차원·float/double별 저장과 정밀도를 명시한다. `TryNormalize` 등 checked 연산은 영벡터·비유한 입력·길이 제곱 overflow·근접 영값의 실패와 입력 보존을 정하고, fallback은 호출자가 명시한다. tolerance가 길이인지 제곱 길이인지 분명하게 구분한다. UE 5.6 Vector2와 Vector3의 정규화 실패 동작은 서로 다르므로 그대로 혼합하지 않고 Rocks에서 차원 공통 계약을 정해 검증한다. SIMD/scalar 차이와 정확도·근사 경로는 별도로 검사한다.

기본 shape은 빈 상태와 잘못된 상태, 유한 좌표와 음수 반지름, 정렬되지 않은 min/max를 정의한다. 접촉 포함 여부, ray의 시작점이 내부인 경우, 평행·0 방향·퇴화 삼각형·유효 `t` 구간과 epsilon을 각 쌍별로 명시한다. 정규화되지 않은 ray에서 `t`와 실제 거리를 혼동하지 않는다. bool 접촉 결과와 입력 오류를 구분한다. 서로 바뀐 shape의 대칭성, 고정밀 기준·seeded property·접선·극단 크기·signed zero·NaN/Inf를 테스트한다. 비균일 scale과 회전의 shear, 음수 determinant와 normal 변환은 Transform 단계에서 별도로 다룬다.

### 3.5 플랫폼·동시성·시간·파일

| ID/우선 | 기능·상태 | 완료 조건과 검증 |
|---|---|---|
| P01 P0 | generic/platform file interface, 물리 file open/read/write/seek/size/delete·wrapper 주입 **부분** | 소유권·close 오류·부분 I/O·EOF·Unicode·권한·symlink·atomic 저장, 실제 Windows/macOS 실행. 구현은 Generic+platform private 경계 유지. |
| P02 P0 | 디렉터리 생성/삭제/열거·stat·copy/move·timestamp·기본 경로 **후속/확인 필요** | 파일·디렉터리/symlink 의미, recursive 경계·실패 rollback·권한·race/TOCTOU, 정렬 없는 열거와 cancellation, 경로·자원 수명·실 OS. |
| P03 P1 | flush/truncate/read-at·async I/O·memory mapping·watch·wrapper chain·I/O cache — 비동기 offset 읽기(`Platform/AsyncFile.h`, Windows Native/WorkerPool 검증) **부분**, 나머지 **후속** | 부분 완료·취소·늦은 callback·종료·ordering·rescan/overflow·backpressure·예산과 소유권. 백엔드별 미지원은 명시적 결과. |
| P04 P0 | thread/Mutex/CV/event/semaphore·clock **부분** | start/join/timeout/자기 join·동시 wait·shutdown·timeout 음성대조, 재귀 여부, primitive 생성/소멸과 memory ordering. 프로세스 fatal 테스트의 실제 setup 성공을 독립적으로 검증하고 partial stderr 및 timeout 단계를 보존한다. |
| P05 P1 | atomic/TLS/RW lock·thread name/priority/affinity·CPU·stack 옵션·wait groups **후속/확인 필요** | supported capability, atomic order·ABA·lost wakeup·writer starvation·over-subscription·OS 실패. 동기화 없는 잘못된 사용은 암묵적 성공으로 감추지 않는다. |
| P06 P1 | process spawn/wait/exit·pipe/IPC·환경·CLI·동적 라이브러리 **후속/부분** | argument quoting, pipe concurrent drain과 bounded output, 제한시간·child cleanup·분리 자손/OS job 한계, 재시도·재진입·Unicode, Windows/macOS와 headless. |
| P07 P0/P1 | monotonic clock/duration·UTC/date/timecode·tick과 dilation **부분/후속** | timeout 단조시계, overflow·clock adjustment·윤년·timezone, fixed/variable tick의 pause·time dilation·누적치·결정성. engine tick은 상위 모듈과 통합한다. |

### 플랫폼 비동기 파일 I/O 상세 계약과 단계

2026-11-11에 1차 구현을 추가했다: `Platform/AsyncFile.h`의 `AsyncIoContext`·`AsyncReadFile`·`AsyncReadRequest`(offset 지정 읽기, 호출자 소유 요청, 완료 통지 1회, 취소 경합 결과 보고, 소멸 시 취소 후 완료 대기), Windows Native(overlapped + IO completion port)와 모든 플랫폼 공용 WorkerPool(위치 지정 동기 읽기) backend, macOS WorkerPool(`pread`) hook. Windows x64에서 두 backend를 실제 OS 파일로 검증했다. macOS는 빌드·실행하지 않았고, 쓰기·scatter/gather·우선순위·deadline·dispatch I/O 평가·fake backend 결정적 interleaving 테스트는 아래 목표로 남는다. 아래 계약은 남은 범위를 포함한 목표다. OS별 기능 차이는 `GenericPlatform` 공통 계약과 private backend에서 처리하고 호출자가 운영체제 전처리 분기를 작성하지 않게 한다. 에셋 로더나 Task Graph와 결합하더라도 Core 레이어만으로 headless 테스트가 가능해야 한다.

1. **공통 API:** 파일 핸들에서 명시적 offset과 길이를 받는 비동기 read/write를 우선한다. 요청은 소유형 완료 핸들로 관리하며 상태, 실제 전송 바이트, OS 오류, 취소 요청과 최종 완료 결과를 구분한다. `Submit`의 즉시 실패, 즉시 완료, 보류 완료가 같은 계약으로 관찰되고 완료 통지는 정확히 한 번이어야 한다. 대기 외 callback/Task 연결은 실행 스레드·재진입·동기 완료 시점을 문서화한다. scatter/gather, flush, 우선순위와 deadline은 capability에 따라 후속 확장한다.
2. **소유권과 취소:** 읽기 대상·쓰기 원본 버퍼, 요청 레코드, 핸들 및 callback 데이터는 OS가 실제로 사용을 마친 최종 완료까지 유지한다. 빌린 버퍼와 소유형 버퍼의 책임을 타입/함수로 구분한다. `Cancel`은 취소 요청이며 즉시 완료나 버퍼 회수를 뜻하지 않는다. 완료와 경합하면 정상 완료·취소·다른 오류 중 하나를 결과로 보고, 부분 전송도 숨기지 않는다. 파일 닫기/dispatcher 종료는 신규 제출을 막고 진행 중 요청을 완료 또는 취소 후 회수한다. 취소 불가능하거나 정지한 장치에 무조건 짧은 종료 시간을 약속하지 않는다.
3. **Windows:** `FILE_FLAG_OVERLAPPED` 핸들과 `OVERLAPPED`, IO completion port 또는 동등한 Windows thread-pool I/O 경로를 backend 후보로 구현·측정한다. 호출 즉시 완료와 pending, completion packet 발생 정책·실패·중복 회수를 명확히 한다. 요청 단위 `CancelIoEx`는 취소를 보장하거나 완료까지 기다리지 않으므로 OS의 최종 완료 관찰 전 OVERLAPPED와 버퍼를 재사용하지 않는다. 동기 핸들 대체 경로 및 네트워크·특수 파일의 지원 여부는 별도로 검증한다. 근거: Microsoft [비동기 I/O](https://learn.microsoft.com/en-us/windows/win32/fileio/synchronous-and-asynchronous-i-o), [취소](https://learn.microsoft.com/windows/win32/api/ioapiset/nf-ioapiset-cancelioex), [완료 포트](https://learn.microsoft.com/en-us/windows/win32/fileio/i-o-completion-ports).
4. **macOS:** dispatch I/O, POSIX 비동기 I/O 및 제한된 worker 기반 positional I/O를 후보로 평가한다. 일반 파일에 대한 실제 가용성·offset·순서·취소·완료·성능을 실기기에서 확인하고 지원하지 못하는 기능은 capability와 결과로 드러낸다. 단순 `dispatch_read`는 제출 뒤 취소할 수 없으므로 요청별 OS 취소를 보장한다고 가정하지 않는다. worker fallback은 동기 시스템 호출을 worker에 위임한 것임을 명시하고 동시 실행 수, 큐 예산, backpressure 및 shutdown 경합을 검증한다. 근거: Apple [dispatch_read](https://developer.apple.com/documentation/dispatch/dispatch_read?changes=__5). 콘솔과 다른 OS의 native backend는 플랫폼 SDK 접근·라이선스 및 capability 감사 후 추가한다.
5. **필수 검증:** 빈/대형 파일, UTF-8 경로, offset/overflow/EOF·partial transfer, 즉시/지연 완료, 동시 겹침, cancel-before-submit·pending·completion 경합, 반복 취소, 파일 close·dispatcher shutdown·callback 재진입과 late completion을 결정적 fake backend와 실제 OS 양쪽에서 검사한다. submit/할당/OS 실패를 주입해 누수·중복 callback·버퍼 조기 해제가 없는지 추적한다. 1/다수 생산자·완료자 stress, 큐 포화·우선순위·시간 초과 및 ASan/race 도구를 포함한다. Windows x64, macOS x64/ARM64 실 실행을 각각 기록하며 미실행 플랫폼은 통과로 표시하지 않는다.

### 3.5.1 모바일(iOS·Android) 지원 — 2026-11-11 요구 반영

사용자 요구에 따라 Android와 iOS를 Core·PlatformBase의 정식 대상으로 둔다(콘솔은 후속). 현재 저장소에는 모바일 backend·빌드 대상이 없고 이 개발 환경에는 iOS SDK(Xcode)와 Android NDK가 없어 **미구현·미검증**이다. 이미 구현한 것 중 모바일에 직접 쓰이는 부분은 ARM64 NEON 수학 경로(ARM64 대상 컴파일만 확인)와 POSIX `pread` 기반 비동기 읽기 WorkerPool hook이다.

| ID/우선 | 항목 | 완료 조건과 검증 |
|---|---|---|
| MB01 P0 | 빌드 대상: iOS ARM64(device·simulator), Android ARM64·x86_64(API level 고정), 교차 컴파일 toolchain·SDK 버전 고정 | 현재 Rocks.Build의 교차 빌드 거부를 대상별 허용 목록으로 바꾸고 미지원 조합은 계속 거부. 대상별 경고 오류·단독 헤더 컴파일, CI에서 최소 빌드 |
| MB02 P0 | PlatformBase backend: `Private/Posix`(Mac·iOS·Android 공통 thread·sync·clock·file·pread), `Private/Apple`(Mac·iOS 공통), `Private/IOS`, `Private/Android` | 기능별로 정확히 하나의 구현 선택, Fatal·VirtualMemory(mmap/mprotect)·TLS·thread 이름, 실기기 정상·실패·종료 테스트 |
| MB03 P0 | 파일·경로: 앱 샌드박스(iOS Documents/Library/Caches, Android internal/external files), Android APK/AAB asset(AAssetManager)·asset pack, 읽기 전용 번들 | 경로 문법과 저장소 종류 분리, 비동기 읽기에서 압축 해제 없이 offset 읽기 가능한 asset만 직접 지원, 미지원은 Unsupported |
| MB04 P1 | 수명·자원: 백그라운드 전환 시 I/O·worker 정지/재개, 메모리 경고(iOS didReceiveMemoryWarning, Android onTrimMemory)와 allocator 예산 연동 | 진행 중 요청의 완료·취소 규약 유지, 앱 일시정지 중 callback 지연 허용, 강제 종료 시 부분 쓰기 정책 |
| MB05 P1 | 성능·SIMD: ARM64 NEON 실행 검증, FMA 축약 금지(`-ffp-contract=off`, 빌드 적용·codegen 확인 완료)의 실기기 결과 확인, thermal throttling 하의 측정 | `ScalarSimdBitExact` 판정을 실기기 결과로 갱신, 미실행 플랫폼은 통과로 표시하지 않음 |

### 3.6 로깅·프로파일링·설정·자원 변환

| ID/우선 | 기능·상태 | 완료 조건과 검증 |
|---|---|---|
| D01 P0 | logger → sink·level/category, assert/check/ensure, fatal, 재진입/OOM fallback **부분** | 여러 sink, 동기화·필터·순서·failure propagation, stdout/stderr 잠김·종료·경로 실패; Fatal 음성 대조가 실패 상황을 가짜 PASS로 만들지 않음. |
| D02 P1 | structured log, file rotation/async sink, trace·CPU/GPU profiler, counters, dropped event, memory/task/I/O/GC 측정 **부분/후속** | 비활성 비용·overflow/drop·종료/동시 capture·시간 상관·headless viewer 연계. GPU는 Renderer 계층. |
| D03 P1 | config 계층·platform/target override, 환경·commandline, reload/change listeners, runtime console variables **후속** | 우선순위·잘못된 구문/진단·원자 snapshot·동시 갱신·민감값 처리·headless. 기존 Commandlet은 console variable 관리 구현을 뜻하지 않는다. |
| D04 P1 | CRC/checksum·비암호 hash·Base64·compression 인터페이스·streaming — `Core/Encoding/Crc32.h`(zlib 호환 CRC-32, 분할 계산)와 `Base64.h`(RFC 4648 엄격 디코딩, 실패 시 출력 불변) **부분**, compression·streaming·URL-safe 변형 **후속** | 공식 golden, 빈 입력·overflow·partial chunk·악성/절단 데이터·출력 제한·OOM rollback, 안전하지 않은 형식의 오용 방지. 암호/보안은 검증된 라이브러리와 키·무결성 정책을 별도로 선택한다. |
| D05 P1 | parser·토큰화·wildcard·glob·버전·progress/cancellation **후속/확인 필요** | 입력 제한·정확한 위치·Unicode·escape·malformed·reentrancy·headless 취소·작업 연동. 정규식 엔진 등 고비용 의존성은 별도 검토. |

### 3.7 작업·모듈·객체 경계

| ID/우선 | 기능·상태 | 완료 조건과 검증 |
|---|---|---|
| R01 P0 | worker scheduler/inline executor, task dependencies·취소·결과·wait·shutdown **부분** | fan-in/out·nested task·capacity/OOM·취소/late completion·teardown·공정성·stress. thread lifecycle과 객체 문맥/GC write barrier를 상위 계약과 연결한다. |
| R02 P1 | module/plugin 등록·capability·dynamic load/unload·hot reload **부분/후속** | lease/in-flight callback·ABI/version 충돌·provider 교체·실패 rollback·동기화. 모듈을 unload하기 전에 호출 가능한 함수와 메모리 소유권을 회수한다. |
| R03 P1 | serialization·reflection·object 수명·GC와 Core 공용 타입 연결 **부분** | Core 기능 구현을 CoreUObject 전체 완료로 오인하지 않는다. 생성 메타/식별·schema·rename·GC root/weak/barrier·파괴 상태·데이터 오류와 allocator·thread 안전성을 별도 게이트로 검증한다. |
| R04 P0 | 빌드/테스트/명령행 도구, 경고 오류 처리·harness·headless **부분** | .NET/MSBuild·Clang, 자체 빌드의 증분 정확성, native unit + negative compile/fatal·단독 헤더·seed·fault injection·sanitizer·비교 oracle. 실패/skip은 성공 수에 포함해도 실행 증거로 취급하지 않는다. Windows/macOS 실기기·CI evidence와 source digest를 분리한다. |

## 4. 공통 API·수명·오류 규약

1. `Try*`는 복구 가능한 실패를 값으로 반환하고 실패 전 상태·자원 보존을 명시한다. 성공 여부와 업무용 bool 값이 혼동되는 `if (Result<bool>)`를 기본 상태 조회 API로 사용하지 않는다. 반환 오류의 원인·경로·플랫폼 코드는 보존하되 noexcept 하의 진단 메모리 한계를 명시한다.
2. 기본 사용 경로는 `values.Sort()`, `path / name`, `string.Contains(...)`처럼 의도를 직접 표현한다. 추가 동작은 명시적 비교기·allocator·context로 확장하고 내부 registry/scope를 반복적으로 직접 조립하지 않는다. API 예제는 실제 컴파일·실행 테스트로 유지한다.
3. 소유 객체, 빌린 view·pointer, allocator·module·native handle 수명은 타입과 문서에 기록한다. 재할당/삭제/정렬/동시 변경에 따른 참조 무효화·동일 객체 identity 보존 여부를 명시한다. 수명 문제가 보이면 unsafe fallback을 선택하지 않는다.
4. 동시 접근은 각 API별 생산/소비자 수, lock/atomic 메모리 순서, 재진입, callback 실행 스레드, 종료·취소와 작업자 고갈 정책을 정의한다. 데이터 경쟁을 허용하는지 암묵적으로 가정하지 않는다.
5. 파일의 lexical 경로와 OS 상 실제 파일 identity·symlink/junction 보안 검사는 분리한다. 문자열의 byte·code point·grapheme 단위와 지역화 표시 문자열을 구분한다.
6. 공통 math/simd는 좌표계·단위·정밀도·오류·alignment·padding·영속 표현을 사전에 확정한다. API의 정상 값과 degenerate·NaN/Inf의 실패/반환을 구별한다.

## 5. 단계별 작업 순서와 의존성

| 단계 | 선행 조건과 구현 범위 | 종료 게이트 |
|---|---|---|
| 0 · 실패 원인/시험 신뢰성 | ASan 스레드 종료 시간 초과의 단계별 증거 수집, 거짓 PASS 차단, setup 실패·timeout·stderr 수거 음성대조. **2026-11-11 사용자 지시로 누락 구현보다 후순위로 보류**(확보 증거는 §7-5) | 재현 방법과 원인/한계를 분리 기록. 정상·음성·반복 및 기존 harness 회귀. 단순 재시도 성공만으로 원인 해결 판정 금지. |
| 1 · 기준·기반 보호 | UE5.6 공개 헤더/공개 API 인벤토리와 Rocks 담당 모듈 자동 대조, uncommitted 변경 보존, Base·allocator·수명·플랫폼 계약 | 각 심볼/기능의 버전·출처·상태·예외·테스트를 기록. 허위 완료 0, 라이선스 검토, CI 경고 오류·header 단독 컴파일. |
| 2 · 범용 자료구조·공용 유틸 | Array/Span/Map/Set/BitArray 미완과 sparse/queue, traits·optional·scope guard, 문자열·Name/Unicode 및 경로. 운영체제 디렉터리·stat과 함께 사용 시나리오 우선 | API 사용 예, alias/move-only/OOM/property/negative harness, 플랫폼 접근 가능 범위 기록. 상호 의존 재진입·allocator/LIFO 실패 검증. |
| 3 · 수학·기본 충돌 | 좌표/정밀도/타입 계약 → CPU별 PlatformMath·4-lane 레지스터 → scalar·vector·matrix·투영·quaternion·Rotator·transform·색·난수 → 기본 shape 및 교차 → batch/추가 ISA. 외부 backend는 측정 기반 교체 후보로 유지. **진행 중**(구현 범위는 [수학 구현](../Implementation/Math.ko.md)) | double/golden oracle, 경계/NaN/degenerate/property, 실제 x64/ARM64와 scalar/SIMD 대조. 각 shape pair 지원 여부와 실패 계약을 열거. 전체 물리 엔진 완료로 표기하지 않는다. |
| 4 · 플랫폼·동시성·진단 | 파일: ① 디렉터리·stat·`ReadAt`/`WriteAt`·flush·truncate(동기 positional I/O) → ② **비동기 파일 I/O**: 결정적 fake backend로 공통 계약(제출·완료 1회·취소 경합·버퍼/핸들 수명·종료) 고정 → Windows overlapped+IOCP backend → macOS worker 기반 positional I/O와 dispatch I/O 평가 → ③ wrapper chain·mapping·watch. 이후 thread 옵션·atomic·task 연동, structured logging·tracing·config | Windows/macOS 각 실제 OS·권한·IO 오류·동시성·shutdown. ASan/가능한 race detector와 장시간 stress. 미지원 capability의 음성 테스트. |
| 5 · 완성·확장 | 수학 고급 shape, native smart pointer, shared cache·프로파일러, compression·통계·외부 allocator, 직렬화/GC/상위 Engine 연결 | 지표·예산·API 안정성·라이선스·플랫폼별 실행 증거를 만족한 항목만 완료로 변경. UE5.6 Core 차이·미지원 목록과 이유 유지. |

단계 간 가능한 병렬 작업은 허용하되 기반 수명·오류 계약과 기준 구현을 우회하지 않는다. 사용자가 Core 안정화 전 상위 기능 진행을 요청하지 않았으므로 Renderer·Editor·Gameplay는 이 명세의 구현 대상이 아니다.

## 6. 검증 매트릭스와 완료 판정

- 모든 공개 기능은 **정상·빈 입력·최소/최대·잘못된 입력·오류/실패 주입·중복/alias·이동 전용 및 자원 수명·재진입/동시성** 중 해당하는 항목을 제공하고 독립적인 oracle 또는 golden과 비교한다. 메모리 할당 기능은 라이브 할당/byte가 모든 정상·실패 경로에서 회수되는지 확인한다.
- 테스트 하네스는 실패를 성공으로 오인하지 않도록 의도적으로 성공해야 할 양성 대조, 의도적으로 실패해야 할 음성 대조, 컴파일 실패 원인 확인, subprocess timeout·정리 실패·SKIP을 별도로 기록한다. 테스트의 setup이 실패해 목적과 다른 fatal이 발생하는 경우 반드시 FAIL이어야 한다.
- 단일 CI 기록에서 source digest·compiler·OS/CPU·빌드 구성·seed·test name·skip reason·asan/race detector 상태·실행 시각·결과 파일을 남긴다. 경고는 오류, 실제 미실행을 PASS로 간주하지 않는다. 테스트 실행 중 source가 바뀌면 그 결과는 해당 이후 상태의 근거가 아니다.
- Windows/macOS의 디버그·최적화·ASan은 각각 증거를 보존한다. macOS 실행 환경을 사용할 수 없으면 해당 항목을 미검증으로 둔다. x64와 ARM64의 성능/정확성 결과도 구분한다.
- 정렬·컨테이너는 재현 가능한 무작위 workload와 느리고 단순한 oracle, 수학은 고정 golden과 고정밀 oracle, 병렬 경로는 순차 기준 결과 및 종료/취소 stress로 교차 검증한다. 단순 반복 성공만으로 불가능한 interleaving의 부재를 보증하지 않는다.
- 구현 후 독립 서브에이전트가 API·수명·보안/플랫폼 및 테스트 타당성을 교차 검토하고, 발견 사항을 수정한 뒤 수정분을 다시 검토한다. 외부 코드 참조/복제 여부 및 라이선스·범위를 기록한다.

## 7. 확인되지 않은 결정 사항

1. 수학: 자체 구현과 CPU별 PlatformMath(SSE2/NEON)를 채택했고 float/double 저장은 padding 없는 성분 순서, 행렬은 열 우선, quaternion은 (x,y,z,w)로 결정했다(2026-11-11). 컴파일러의 FMA 축약은 모든 native 대상에서 금지(`-ffp-contract=off`)하기로 결정했다(2026-11-11, 근거: 축약이 ARM64에서만 일어나 같은 소스의 행렬 역행렬 성공 여부까지 달라지는 것을 리뷰가 재현). 외부 backend 교체 여부, AVX2/FMA 등 선택 ISA의 runtime dispatch(명시적 FMA kernel 포함), 플랫폼 간 부동소수점 결정성 범위(libm 초월 함수 차이 포함)는 아직 결정하지 않았다.
2. 기존 [전체 구현 계획](EngineImplementationPlan.ko.md)의 기본 좌표 규약은 오른손·+Z 위·+X 전방·미터·라디안·초, column vector와 `world=parent×local`, reversed-Z 깊이 [0,1], 월드 위치 double/렌더 상대 float다. UE의 좌표·단위와는 가져오기/내보내기 경계에서 변환한다. matrix 메모리 배치(열 우선)·quaternion 순서((x,y,z,w))·Rocks view 공간(카메라 +X 전방)과 NDC 규약은 결정했다. 기본 collision 접촉 기준(닫힌 집합)과 함수별 epsilon은 도형 구현 문서에 함수별로 기록한다.
3. UE5.6 공개 Core 전체 원본/심볼을 아직 기계적으로 대조하지 않았다. 공개되지 않은 내부 기능과 상위 모듈을 Core와 혼동하지 않으며, 5.8 참고 사례는 5.6 증거로 취급하지 않는다.
4. 실제 macOS x64/ARM64, 다른 OS/콘솔, 실제 외부 allocator 제품 배포 및 성능 동등성은 실행 증거가 없다. 계측 가능한 목표와 대상 하드웨어를 별도로 정한다.
5. ASan thread-unjoined timeout은 최초 실행에서 관찰됐으나 기록된 timeout만으로 정확한 교착 원인은 확정되지 않았다. 2026-11-11 현재 소스(준비 마커 포함)의 ASan 빌드를 2초 정지 감지 하네스로 반복하자 22회 중 3회 정지했고, 두 사례는 65초 이상 지속된 **영구 정지**였다. 유일하게 남은 메인 스레드의 스택은 `Fatal → abort → ExitProcess → LdrShutdownProcess → ASan 런타임 DLL_PROCESS_DETACH의 onexit 표 해제 → ASan 할당기 → WaitOnAddress`였다. 테스트의 `_set_abort_behavior`가 `_CALL_REPORTFAULT`를 끄므로 abort가 `__fastfail` 대신 `_exit(3)`→`ExitProcess`로 진행하고, `ExitProcess`가 종료 중이던 worker를 강제 종료한 뒤 DLL 분리 코드가 고아가 된 ASan 내부 잠금을 기다린다는 해석이 증거와 일치한다(Microsoft ExitProcess 문서의 교착 경고와 같은 형태). 정확한 ASan 잠금 이름은 런타임 PDB가 없어 미확정이며, 원래 20초 실패가 같은 원인인지도 당시 증거가 없어 확정하지 않는다. 종료 코드가 이미 설정된 이 상태의 프로세스는 외부 TerminateProcess로 제거되지 않아 `ProcessRunner`의 timeout 정리도 끝나지 않는다. 제안 수정(Windows `Fatal`이 진단 출력 후 `TerminateProcess`로 종료, EXE TLS callback 기반 결정적 회귀, ProcessRunner의 부분 출력·종료 상태 보존)은 사용자 지시로 보류했고 아직 적용하지 않았다.
