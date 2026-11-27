# 빌드 결과 재사용 설계(메타 단계 증분·공유 compile 결과 저장소)

기준일: 2026-11-28, 개정 1. 같은 기계에서 여러 세션이 하루 종일 빌드하는데 빌드가 매우 느리다는 사용자 보고의 원인을 기존 빌드 기록으로 재고, 입력이 같은 일을 다시 하지 않게 하는 두 가지를 정한다.

1. **메타 단계 증분(1순위)**: 리플렉션 메타 생성(MetaCompiler), 셰이더 파라미터 생성(MetaCompiler `shader-parameters`), 안정 ID 검사(`ids verify`·`ids verify-set`)를 입력이 같으면 건너뛴다. 지금은 입력이 바뀌지 않아도 빌드마다 모두 다시 돈다.
2. **공유 compile 결과 저장소 `SharedCompileStore`(2순위)**: 한 기계의 여러 작업 사본(worktree)과 여러 빌드 타깃이 native compile 결과(그리고 1의 메타 생성 결과)를 서로 다시 쓴다.

이름은 Rocks 이름이다. "빌드 캐시"·`BuildCache`·DDC는 쓰지 않는다(DDC 대응은 [에셋 너겟](../AssetSystem/09-AssetNuggets.ko.md)이다). 분산·원격 실행(Incredibuild 류)은 범위 밖이고 제안하지 않는다.

대상 코드는 `Programs/Rocks.Build.Cli`(Program·MetadataRunner·ShaderParameterRunner·MetadataOutputTransaction), `Programs/Rocks.MetaCompiler`(CompilerDriver·ShaderParameterGenerator), `Programs/Rocks.Build`(ActionGraph·Executor·IncrementalStore·BuildInputs·Dependencies·NativeBuildEngine)다. native 증분의 기반인 2단 key는 [유니티 빌드·PCH 설계](../UnityBuildPch.ko.md) 4.1–4.4절이다.

UE 근거는 UE 5.8.4(`release`, 로컬 클론 `C:\github\UnrealEngine`)다. UBT 경로는 `Engine/Source/Programs/UnrealBuildTool/`(약어 `UBT/`), UBA 경로는 `Engine/Source/Programs/UnrealBuildAccelerator/`(약어 `UBA/`) 기준이다. UE 이름은 아래 대응표와 근거에만 적는다.

상태: **설계 초안 개정 3**(MI0·MI1 shadow·MI2 구현)(결정 BR1–BR30, 모두 권장안 — 2026-11-28 교차 검증 반영). 구현 단계는 MI0–MI4(메타 단계)와 SCS1–SCS6(공유 저장소)이고, 순서는 MI0 → MI1(+MI2, shadow mode) → SCS1 → MI3·MI4 → SCS2–SCS6이다(5장). 착수는 사용자가 정한다. 설계 중에는 native 빌드를 돌리지 않았다. 측정은 끝난 빌드의 기록·로그(읽기만)와 작은 clang-cl 탐침(7장)으로 했다.

## 우선순위와 기대 효과

| 순위 | 항목 | 대상 빌드 | 지금 | 기대 | 줄어드는 시간 |
|---|---|---|---|---|---|
| 1 | 메타 단계 증분(MI1–MI2, shadow 확인 뒤 건너뛰기 켬) | 리플렉션 입력이 바뀌지 않은 증분 빌드(예: wt-ut5 Sandbox, `.cpp` 하나 수정) | 523초(native `executed=9`) | 약 1분(native·선행 타깃·검사) | **빌드마다 약 7–8분** |
| 1 | 같음 | 무변경 빌드(sparks·ma2b EngineTests 등) | 321–720초 | 1분 안팎 | 빌드마다 약 4–11분 |
| 1 | 메타 모듈 병렬(MI3) | 리플렉션 헤더가 많이 바뀐 빌드(병합 등) | 메타 단계 287–875초 | 의존이 없는 모듈을 동시에 | 모듈별 시간을 잰 뒤 정한다(MI0) — MaterialSchema 하나가 하한 |
| 2 | 공유 compile 저장소(SCS1–SCS4) | 새 작업 사본의 첫 빌드(Commandlet Debug) | native 단계 126초 | compile CPU의 34%(지금 include 규칙)–67%(정밀 include 검증) 적중 | 빌드마다 약 0.7–1.4분 |
| 2 | 같음 | 같은 작업 사본의 둘째 Game 종류 타깃(UIDemo 뒤 Sandbox) | native 94초 | compile CPU의 46% 적중 | 약 0.7분 |
| 2 | 메타 결과 공유(SCS5) | 새 작업 사본의 첫 빌드 | 메타 단계 512초 | 가까운 commit에서 입력이 같은 모듈 | 빌드마다 약 3–6분(추정, MI0·SCS6에서 잰다) |
| 2 | 정밀 include 검증(SCS1) | Core 공개 디렉터리에 헤더를 더한 origin 병합 뒤의 증분 빌드 | 거의 모든 TU 재컴파일 | 그 헤더를 연 TU만 | native 단계의 대부분 |

- 기계 전체(1.3절): 최근 4일 빌드 805회(하루 약 200회) 중 107회가 처음부터 빌드다. 나머지 약 700회 대부분이 1순위의 대상이다. 그중 리플렉션 입력이 바뀌지 않은 빌드가 절반만 되어도 하루 약 70회 × 7분 ≈ 8시간의 세션 대기가 없어진다(추정).
- MI1 shadow 확인(2.13절): 같은 조작(무관한 `.cpp` 하나)의 빌드가 655초이고 그중 메타 단계가 599초였으며, 28개 메타 action이 모두 would-hit·불일치 0이었다. 건너뛰기를 켜면 남는 것은 native 56초와 판정 비용이다.
- 2순위만으로는 대부분의 빌드가 빨라지지 않는다. native 단계는 이미 증분이 잘 되고(무변경 빌드 200회가 `executed≤5`), 빌드 시간의 75–95%가 메타 단계다(1.2절).

## 목차

- [1. 측정과 원인](01-MeasurementAndCauses.ko.md)
- [2. 메타 단계 증분(1순위)](02-MetadataIncremental.ko.md)
- [3. 공유 compile 결과 저장소(2순위)](03-SharedCompileStore.ko.md)
- [4. 다른 세션 요구](04-SessionRequirements.ko.md)
- [5. 시험·단계·측정](05-TestsStagesMeasurement.ko.md)
- [6. 결정](06-Decisions.ko.md)
- [7. 근거 세부와 탐침](07-Evidence.ko.md)
- [8. 다른 문서 개정과 개정 이력](08-Revisions.ko.md)

## 용어 대응

| Rocks | UE 5.8 대응 | 뜻 |
|---|---|---|
| 메타 단계 | UHT 실행(`UBT/System/UHTExecution.cs`) | native compile 전에 리플렉션 생성물·셰이더 파라미터 헤더를 만들고 안정 ID를 검사하는 단계 |
| 메타 action | (UHT는 타깃 전체를 한 번에 판정) | 메타 모듈 하나의 MetaCompiler 실행. 선언 입력 key와 의존 기록으로 건너뛴다 |
| 메타 기록 | UHT timestamp 파일 | 메타 action의 key·의존 기록·출력 해시 |
| 공유 compile 결과 저장소 `SharedCompileStore` | UBA cache(`UBA/Common/Public/UbaCacheClient.h`, cache server) | 기계 한 대의 사용자 단위 내용 주소 저장소 |
| 1차 key | cmd key(`UbaCacheClient.cpp:1742` `GetCmdKey`) + bucket | 정규화한 선언 입력의 해시 |
| 후보 항목(entry) | cache entry(`UBA/Common/Public/UbaCacheEntry.h`) | 1차 key 아래의 의존 목록 하나와 그 출력 blob |
| blob | CAS 항목(`CasKey`) | 출력 파일 byte. SHA-256 이름 |
| 루트 정규화 | root paths(`UBT/System/CppRootPaths.cs`, `UbaRootPaths.h`) | 작업 사본 루트·출력 폴더를 자리 표시로 바꾼다 |
| 경로 결속 action | `ArtifactMode.AbsolutePath`(`UBT/Actions/Action.cs:61`) | 출력이 절대 경로에 묶여 루트를 key에 넣는 action |
| 입력 전파 | `ArtifactMode.PropagateInputs`(`UBT/Actions/Action.cs:69`) | PCH를 저장하지 않고 소비 TU key에 PCH의 입력 key를 넣는다 |
| 정밀 include 검증 | (없음) | 헤더 이름 목록 대신, 연 헤더마다 앞선 탐색 위치에 같은 이름이 없는지 본다 |
| 경로 독립 object | VFS 모드(`bUseVFS`, `UBT/Platform/Windows/VCToolChain.cs:557–565`) | 절대 경로·시각이 없는 object |
