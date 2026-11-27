## 4. 다른 세션 요구

설계 중 coordinator를 거쳐 받은 다른 세션의 요구와 이 설계의 대응이다. 오디오(rocks-87)·커브(rocks-dc)·물리(rocks-06)·텍스처(rocks-2c)·메시(rocks-10)·UI(rocks-03)·머티리얼(rocks-d5) 세션은 `Rocks.Build`·`Rocks.Build.Cli`·`Rocks.Tooling.Common`을 고치지 않고 계획도 없다고 했다(구현 겹침 없음).

| 세션 | 요구 | 대응 | 시험 |
|---|---|---|---|
| 물리(Jolt) | 결정성: FP 인자(`-ffp-contract=off` 등)가 key에 반드시 들고, 다른 FP 인자로 만든 object를 섞지 않는다 | 인자 전체가 지역 key와 1차 key에 든다(3.3.1절). 빼는 것은 실행 자리 표시뿐이다. 메타 action도 `--clang-arg` 전체가 key다(2.3절) | T1-2: `-ffp-contract=off` 하나 제거 → miss |
| 물리(Jolt) | Jolt 정의(`JPH_*`, Debug의 assert 켬)가 key에 든다 | `JPH_*`는 정의 헤더(`<out>/Definitions/Jolt.Definitions.h` — 실측 `JPH_CROSS_PLATFORM_DETERMINISTIC 1`·`JPH_ENABLE_ASSERTS 1`·`JPH_OBJECT_LAYER_BITS 32`)에 있다. 정의 헤더는 compile action의 입력(내용이 key)이고 의존 기록에도 든다 | T1-2: 정의 헤더 한 줄 변경 → miss |
| 물리 | MetaCompiler 생성 소스(CppEmitter가 낸 include 등)가 바뀌면 key에 반영된다 | 리플렉션 생성 소스(`<out>/Generated/<모듈>/Source/*.cpp`)는 compile action의 `Inputs`라 내용이 key에 든다. 생성 헤더는 컴파일러가 열므로 의존 기록이 내용 해시로 검증한다. 경로는 `<O>` 아래라 정규화되고, 생성물에 절대 경로가 없다(실측 — 루트 기준 include 철자) | T1-3: 생성 소스·생성 헤더 byte 하나 변경 → miss(두 경로 각각) |
| 물리·텍스처 | ThirdParty manifest `platformDefines`(AstcEncoder `ASTCENC_SSE=20` 등)와 `Rocks.patch`가 같은 key 규칙에 든다 | 서드파티 정의는 모듈 정의(정의 헤더)나 `/D`로 들어가 key에 든다. `Rocks.patch`는 저장소 안 원본에 이미 적용된 파일이라(ThirdParty manifest의 수정 목록) 원본 내용 해시로 든다 | T1-2: `ASTCENC_SSE=20` ↔ `41` → miss |
| 텍스처(ISPC) | 출력 byte는 ISPC 판·타깃·고정 인자(`ROCKS_ISPC_IDENTITY`)로 정해지므로 이 신원과 실행 파일 판이 key에 든다 | Ispc action의 identity가 `["ispc", 판, digest]`(실행 파일 내용 digest)이고 고정 인자 전체가 key다(`ActionGraph.cs` Ispc action). 소비 C++ 모듈은 `ROCKS_ISPC_IDENTITY` 정의로 받으므로 그쪽 key에도 든다 | T1-2: ISPC 판·digest·고정 인자 각각 변경 → miss |
| 텍스처(ISPC) | Windows Ispc action은 stage 작업 폴더에서 상대 경로로 돈다(비 ASCII 절대 경로를 못 연다) | key는 실행 철자(stage 기준 상대 경로)가 아니라 계획의 절대 경로를 정규화한 것이다. 실행 방식은 바꾸지 않는다 | T1-1 |
| 텍스처(ISPC) | 경고 인자(`--woff`·`--werror`)는 출력과 무관하다. ISPC object를 저장해도 된다 | 경고 인자도 key에 남긴다(보수적 — 엔진·서드파티 모듈 사이는 어차피 공유하지 않아 손해가 없다). ISPC object는 timestamp 0·절대 경로 없음이라(1.8절) 경로 독립으로 저장한다. 생성 헤더도 함께 저장한다 | T2(ISPC 포함) |
| 메시 | 빌드 중 저장소 파일이 바뀌면 build-evidence가 종료 1을 내는 규칙을 공유 저장소에서도 지킨다. 저장소에서 꺼낸 결과도 그 검사를 거친다 | 검사는 그대로다(3.5절). 저장소는 원본 트리에 쓰지 않는다. 받은 출력은 지역 의존 검증(지금 원본 내용 해시)을 통과한 것만 쓴다. 메타 단계 건너뛰기도 원본 digest 검사 범위를 바꾸지 않는다 | T4: 원본 안정 검사 유지 |
| UI | 공유 저장소가 잠금·다른 프로세스를 기다리느라 "Process/pipe deadline expired"에 걸리면 안 된다. 기다림이 생길 곳은 막지 않는 방식으로, 아니면 상한 뒤 저장소를 건너뛰고 컴파일(실패가 아니라 miss) | 게시는 no-replace 이동이라 잠금이 없다. 정리는 try-lock이라 기다리지 않는다. 같은 TU 동시 컴파일을 막는 표지를 두지 않는다. IO 오류·공유 위반은 모두 miss다(3.6절, BR19·BR21) | T3: 잠금을 다른 프로세스가 쥠 → 기다리지 않음 |
| 머티리얼 | 오래 쓰는 작업 사본에서도 빌드가 늘 첫 빌드처럼 오래 걸린다 | 원인은 메타 단계(1.4절)와 병합 때의 이름 목록·작업 파일 분리(1.5·1.6절)다. native 증분 판정 결함은 아니다(1.9절) | 5.3절 S0·S5 |
| coordinator | 메타 단계(리플렉션·`shader-parameters`·ID 검사) 증분을 1순위로. 입력(입력 헤더와 그 include 내용, 응답 파일 인자, MetaCompiler·clang-format·Clang 도구 판, descriptor schema 판)이 같으면 프로세스를 띄우지 않고, 생성물이 지워졌거나 바뀌었으면 다시 생성 | 2장 | T5 |

- 일부 세션이 메타 단계의 3분 제한 때문에 `-p:Jobs=4`로 돌렸다. 지금은 제한이 20분이라(`MetadataRunner.cs:19`) 필요 없다. 메타 증분 뒤에는 대부분의 빌드에서 MetaCompiler가 돌지 않는다.
