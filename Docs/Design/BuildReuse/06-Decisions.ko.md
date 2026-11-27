## 6. 결정

모두 권장안이다(사용자 확정 전).

| 번호 | 결정 | 대안과 버린 이유 |
|---|---|---|
| BR1 | 메타 단계를 모듈 단위 action(`meta:<모듈>` — 모듈의 `ids verify` 포함, `meta-ids-set`, `shader-parameters:<모듈>`)으로 나눠 따로 판정한다 | 타깃 전체 하나로 판정(UE UHT): 모듈 하나가 바뀌어도 모두 돈다. 모듈 `ids verify`를 따로 판정: 입력이 `meta:<모듈>`의 key·출력과 같아 판정이 늘 같다 |
| BR2 | 메타 action key = 도구 ID(넘겨받은 도구 디렉터리 전체 내용 + `dotnet --list-runtimes`) + Clang 툴체인 key + Cli가 푼 clang-format 실행 파일 내용 + 응답 파일 인자(stage 경로는 자리 표시, `--jobs`·계측 인자 제외) + 게시 위치 목록 + 입력 헤더·정의 헤더·identity 명세·import descriptor 내용. 의존 기록 = MetaCompiler의 모든 clang 실행 depfile 합집합 + clang 밖 열거·존재 확인 | 입력 헤더 내용만: include한 헤더의 변화를 놓친다. 판 문자열만(도구 내용 대신): 같은 판 번호로 다시 빌드한 도구를 구별하지 못한다. 도구 어셈블리 하나만: `runtimes/`·위성 어셈블리·런타임 판 변화를 놓친다 |
| BR3 | 판정 코드는 native와 하나다: Rocks.Build가 공개 `InputMemo`(입력 메모 — native `BuildInputs`의 씨앗)와 `StepRecordStore`(key·출력·의존 판정 — `FileIdentity`·`Fingerprints`·`AtomicFiles`를 쓴다)를 내고 Cli가 쓴다. 기록 파일만 `<out>/Metadata/meta-cache.json`으로 나눈다 | Cli에 따로 구현: 같은 규칙이 두 벌이 된다(공용 모듈 규칙) |
| BR4 | identity inventory를 `<out>/Metadata/<모듈>.inventory.json`으로 게시한다 | stage에만 둠: 건너뛴 모듈의 `ids verify` 판정 입력이 없다 |
| BR5 | 건너뛴 모듈은 게시본을, 이번에 다시 생성한 모듈은 stage를 소비 모듈의 판정 대상으로 본다(같은 plan 산출물은 새 값으로 보는 native 규칙). 바뀐 모듈만 한 transaction으로 게시하고, 게시와 안정 검사 뒤에만 기록을 저장한다 | 다시 생성한 의존 모듈도 게시본으로 판정: 게시 전이라 낡은 byte를 보고 틀린 hit를 낸다. 게시 전 기록 저장: 게시 실패 때 다음 빌드가 틀린 hit를 낸다 |
| BR6 | 출력 검증은 (크기, 시각, 파일 ID) 뒤 해시 재확인이고, 게시 디렉터리의 잉여 파일도 miss로 본다 | 존재만 확인: 손으로 고친 생성 파일을 놓친다 |
| BR7 | 의존이 없는 메타 모듈은 동시에 돈다. 간선 = `--import` ∪ `--include-dir` 생성 루트의 소유 모듈, 동시 수 = min(`--jobs`, MI0 메모리로 정한 상한), 출력 줄·실패 보고는 의존 순서 | 직렬 유지: 바뀐 모듈이 많을 때 임계 경로보다 오래 걸린다. 완료 순서 보고: 같은 입력에서 출력이 달라진다 |
| BR8 | MetaCompiler 내부 고정 비용(clang-format 프로세스 수·판 확인)은 MI0 측정 뒤 정한다 | 측정 전 수정: 효과를 모르고 바꾼다 |
| BR9 | 이름은 공유 compile 결과 저장소 `SharedCompileStore`, 디렉터리 `CompileStore` | "빌드 캐시"·DDC: 금지 용어 |
| BR10 | 기계 단위·사용자 단위 저장소(`%LOCALAPPDATA%\Rocks\out\CompileStore`) | 원격 cache server(UBA): 범위 밖. 작업 사본마다 따로: 지금 상태와 같다 |
| BR11 | 지역 증분과 저장소가 같은 판정(1차 key + 의존 내용 + 정밀 include 검증)을 쓰고, 지역 key에서 헤더 이름 목록을 뺀다 | 이름 목록 유지: 실측 적중률이 절반이 된다(1.7절), 병합마다 과한 재컴파일. 의존 기록만 보고 include 해석을 보지 않음(ccache direct 기본): shadow header를 놓친다 |
| BR12 | 2단 조회: 1차 key 디렉터리 아래 후보 항목, 후보마다 의존 내용 해시 검증 | 1차 key 하나에 결과 하나(입력 해시 미리 계산): 헤더 전체를 미리 알 수 없다. 전처리 결과 해시(ccache preprocessor mode): TU마다 전처리 비용 |
| BR13 | 후보 파일은 불변이고 이름이 내용으로 정해진다. 게시는 tmp + no-replace 이동, 잠금 없음 | 1차 key마다 고칠 수 있는 manifest 파일 + 잠금: 쓰는 쪽끼리 기다린다. 데이터베이스(SQLite): 프로세스 사이 잠금·손상 복구가 필요하다 |
| BR14 | 복원은 복사하며 해시를 잰다 | NTFS 하드 링크: 출력 폴더에서 제자리 수정이 일어나면 저장소 blob이 바뀐다, 여러 작업 사본이 같은 파일 ID를 가져 IncrementalStore의 (크기, 시각, 파일 ID) 판단이 서로 영향을 받는다, 볼륨이 다르면 쓸 수 없다. 복사 비용은 491 MiB/빌드로 compile보다 훨씬 작다. Dev Drive(ReFS)의 블록 복제는 OS가 복사에 쓰면 덤으로 얻는다(확인하지 않았다) |
| BR15 | 경로 독립이 시험(T2)으로 확인된 구성만 루트 없는 key를 쓰고, 나머지는 루트를 key에 넣는다(경로 결속) | 모두 공유: 경로가 박힌 object가 섞인다. 모두 결속: 작업 사본 사이 이득이 없다. UE도 PCH 사용 action을 `AbsolutePath`로 묶는다(`UBT/Platform/Windows/VCToolChain.cs:2353–2358`, `UBT/Executors/UnrealBuildAccelerator/UBAExecutor.cs:1356–1364`) |
| BR16 | object를 경로 독립으로 만든다(3.4.1절) | UBA처럼 출력 byte 안 경로를 바꿔 쓰기(`UbaCacheClient.cpp:1095–1140`): UE도 `.d`·`.json`·`.rsp` 같은 텍스트에만 한다(`ShouldNormalize`, 1836행). object 안 CodeView 문자열 길이가 바뀌어 쓸 수 없다 |
| BR17 | PCH는 저장하지 않고 소비 TU key에 PCH 입력 key를 넣는다 | PCH 저장: 헤더 절대 경로·강제 include 경로가 박혀 있고 수십 MB다(UE도 VFS 없이는 결속). |
| BR18 | archive·link·BuildInfo TU·`--timing`은 저장하지 않는다. PCH를 소비자 실행 때만 만드는 것은 후속 | link 저장: 작업 사본마다 `/pdbsourcepath`가 달라 공유되지 않는다. UE 5.8 UBA는 link도 기본으로 cache한다(`bCacheLinkActions = true`, `UBT/Executors/UnrealBuildAccelerator/UnrealBuildAcceleratorConfig.cs:371`) — UE는 기계마다 같은 경로 배치나 VFS에서 그 이득을 얻지만, Rocks의 link 출력은 링크한 작업 사본 경로를 담아(`/pdbsourcepath`) 작업 사본 사이에 같지 않다 |
| BR19 | 저장소 오류·경합은 모두 miss·게시 생략이고 기다리지 않는다. 정리는 try-lock | 대기 상한 뒤 포기: 그래도 기다린다(UI 세션 요구) |
| BR20 | 후보 상한 8, 크기 상한 32 GiB, 미사용 14일, 증분 정리(빌드 끝마다 shard 하나) — 모두 이름 있는 상수 + 환경 변수. 시각은 주입한 `TimeProvider` | 상한 없음: 디스크를 채운다. 빌드 끝 전체 훑기: 저장소가 커질수록 빌드 끝이 느려진다 |
| BR21 | 같은 TU의 동시 컴파일을 막지 않는다 | "진행 중" 표지로 기다림: 다른 세션의 빌드 시간에 묶인다 |
| BR22 | 기본 켬(읽기·쓰기). `ROCKS_COMPILE_STORE`(`off`·`read`·`on`), `--compile-store`가 우선. CI 작업 흐름은 off | 기본 끔: 이득을 놓친다. CI 켬: CI가 처음부터 빌드 검증을 하지 않게 된다 |
| BR23 | 확인 모드 `ROCKS_COMPILE_STORE=verify`: hit여도 컴파일해 경로 독립 구성의 byte를 비교하고 다르면 진단(정기 점검·T2 보조) | 없음: 결정성이 깨져도 모른다. UE `validateCacheWritesOutput`(`UBA/Common/Public/UbaCacheClient.h:51`, `UbaCacheClient.cpp:478`)은 cache에 쓰기 전에 출력 파일의 CAS를 다시 재는 것이라 목적이 다르다(전송 손상 방지) — Rocks의 그 몫은 blob 해시 재확인(3.6.1절)이 한다 |
| BR24 | 메타 생성 결과도 같은 저장소에 넣는다(SCS5, 3.7절). 메타 증분(MI1) 뒤에 한다 | 넣지 않음: 새 작업 사본의 첫 빌드가 메타 단계 전체(512초)를 돈다. MI1 전에 함께: 메타 action의 key·의존 기록이 아직 없다 |
| BR25 | 메타 건너뛰기는 shadow mode(판정만 하고 늘 생성, would-hit 불일치 보고)로 먼저 켜고, 실제 빌드에서 불일치 0건을 확인한 뒤 `on`으로 바꾼다. `ROCKS_META_INCREMENTAL`(`off`·`shadow`·`on`), 다른 값은 오류 | 바로 켬: 의존 기록의 빈틈이 있으면 낡은 생성물을 쓴다 |
| BR26 | 빌드 호출 시작 때 입력 메모 하나를 만들어 메타와 native가 함께 쓰고, 메타 단계 끝에 안정 검사(입력 헤더·이름 목록·의존 파일·열거·존재 확인·도구 디렉터리·clang-format·툴체인 key)를 통과해야 기록을 저장한다 | 안정 검사 없음: 실행 중 바뀐 헤더의 새 해시를 옛 생성물과 묶어 기록한다. 도구 디렉터리 제외: 같은 작업 사본에서 도구를 다시 빌드하는 호출과 겹치면 틀린 도구 ID로 기록한다 |
| BR27 | 공유 저장소 게시는 빌드 끝 안정 검사를 통과한 빌드에서 대기 목록을 꺼내 하고, 게시 직전에 의존 파일을 다시 해시한다 | action 끝마다 바로 게시: 빌드 중 바뀐 입력으로 만든 출력이 다른 작업 사본으로 퍼진다 |
| BR28 | 정밀 include 검증의 inventory는 확장자와 무관한 모든 파일 이름이고, 대소문자는 파일 시스템 규칙대로, reparse point는 받은 이름으로 본다. 후보 탐색 디렉터리는 모두 본다. 설명할 수 없는 경로·`__has_include_next`·`#include_next`·`#embed`·`__has_embed`·매크로 `__has_include`는 이름 목록 방식으로 떨어진다 | header 확장자만: `.def`·확장자 없는 이름·include한 `.cpp`의 shadow를 놓친다(지금 native에도 있는 빈틈). 링크 해석: 출력 폴더 판정과 도구 빌드 ID가 받은 경로 모양에 기댄다(UnityBuildPch 4.4절) |
| BR29 | PCH의 탐색 목록을 소비 key에 넣고, 소비 TU와 PCH의 탐색 목록이 다르면 그 TU는 저장소를 쓰지 않는다 | 무시: PCH 안 헤더가 소비 TU와 다른 순서로 풀렸을 때 정밀 include 검증이 틀린 목록으로 본다 |
| BR30 | 메타 결과 공유 전에 inventory를 `<R>`로 정규화하고, 도구를 경로 독립으로 빌드한다(`ContinuousIntegrationBuild`·`PathMap`) | 그대로 공유: inventory에 다른 작업 사본의 절대 경로가 남고, 도구 ID가 작업 사본마다 달라 공유되지 않는다 |
