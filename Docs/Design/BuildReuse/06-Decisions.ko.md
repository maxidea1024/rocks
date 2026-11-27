## 6. 결정

모두 권장안이다(사용자 확정 전).

| 번호 | 결정 | 대안과 버린 이유 |
|---|---|---|
| BR1 | 메타 단계를 모듈 단위 action(`meta:<모듈>`·`meta-ids:<모듈>`·`meta-ids-set`·`shader-parameters:<모듈>`)으로 나눠 따로 판정한다 | 타깃 전체 하나로 판정(UE UHT timestamp): 모듈 하나가 바뀌어도 모두 돈다 |
| BR2 | 메타 action key = 도구 ID(MetaCompiler 디렉터리 파일 내용) + Clang 툴체인 key + clang-format 실행 파일 내용 + 응답 파일 인자(stage 경로는 자리 표시, `--jobs` 제외) + 입력 헤더·정의 헤더·identity 명세·import descriptor 내용. 의존 기록은 MetaCompiler의 모든 clang 실행 depfile 합집합 | 입력 헤더 내용만: include한 헤더의 변화를 놓친다. 판 문자열만(도구 내용 대신): 같은 판 번호로 다시 빌드한 도구를 구별하지 못한다 |
| BR3 | 판정 코드는 native와 하나다(IncrementalStore·DependencyRecord·Fingerprints를 Rocks.Build 공개 API로 감싸 Cli가 쓴다). 기록 파일만 `<out>/Metadata/meta-cache.json`으로 나눈다 | Cli에 따로 구현: 같은 규칙이 두 벌이 된다(공용 모듈 규칙) |
| BR4 | identity inventory를 `<out>/Metadata/<모듈>.inventory.json`으로 게시한다 | stage에만 둠: 건너뛴 모듈의 `ids verify` 판정 입력이 없다 |
| BR5 | 건너뛴 모듈은 게시본을 의존 모듈의 include·import로 쓴다. 바뀐 모듈만 한 transaction으로 게시하고, 게시 뒤에만 기록을 저장한다 | 건너뛴 모듈도 stage에 복사: 복사 비용만 늘고 내용은 같다. 게시 전 기록 저장: 게시 실패 때 다음 빌드가 틀린 hit를 낸다 |
| BR6 | 출력 검증은 (크기, 시각, 파일 ID) 뒤 해시 재확인이고, 게시 디렉터리의 잉여 파일도 miss로 본다 | 존재만 확인: 손으로 고친 생성 파일을 놓친다 |
| BR7 | 의존이 없는 메타 모듈은 동시에 돌고, 동시 모듈 수 × 검증 병렬 ≤ `--jobs`, 출력 줄은 의존 순서 | 직렬 유지: 바뀐 모듈이 많을 때 임계 경로보다 오래 걸린다 |
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
| BR18 | archive·link·BuildInfo TU·`--timing`은 저장하지 않는다. PCH를 소비자 실행 때만 만드는 것은 후속 | link 저장: 작업 사본마다 `/pdbsourcepath`가 달라 공유되지 않는다. UE 기본도 link를 cache하지 않는다(`bCacheLinkActions`) |
| BR19 | 저장소 오류·경합은 모두 miss·게시 생략이고 기다리지 않는다. 정리는 try-lock | 대기 상한 뒤 포기: 그래도 기다린다(UI 세션 요구) |
| BR20 | 후보 상한 8, 크기 상한 32 GiB, 미사용 14일, 정리 간격 1시간 — 모두 이름 있는 상수 + 환경 변수 | 상한 없음: 디스크를 채운다 |
| BR21 | 같은 TU의 동시 컴파일을 막지 않는다 | "진행 중" 표지로 기다림: 다른 세션의 빌드 시간에 묶인다 |
| BR22 | 기본 켬(읽기·쓰기). `ROCKS_COMPILE_STORE`(`off`·`read`·`on`), `--compile-store`가 우선. CI 작업 흐름은 off | 기본 끔: 이득을 놓친다. CI 켬: CI가 처음부터 빌드 검증을 하지 않게 된다 |
| BR23 | 확인 모드 `ROCKS_COMPILE_STORE=verify`: hit여도 컴파일해 경로 독립 구성의 byte를 비교하고 다르면 진단(정기 점검·T2 보조) | 없음: 결정성이 깨져도 모른다. UE `validateCacheWritesOutput`(`UbaCacheClient.h:51`)과 같은 목적 |
| BR24 | 메타 생성 결과도 같은 저장소에 넣는다(SCS5, 3.7절). 메타 증분(MI1) 뒤에 한다 | 넣지 않음: 새 작업 사본의 첫 빌드가 메타 단계 전체(512초)를 돈다. MI1 전에 함께: 메타 action의 key·의존 기록이 아직 없다 |
