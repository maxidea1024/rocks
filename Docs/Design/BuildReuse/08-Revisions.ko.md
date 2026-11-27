## 8. 다른 문서 개정과 개정 이력

### 8.1 다른 문서 개정(구현 때 먼저 적는다)

- [리플렉션 기반](../ReflectionFoundations.ko.md) 118행 "탐색은 빌드마다 한다 … 필요해지면 캐시"를 2장(메타 단계 증분)으로 바꾼다. [리플렉션 생성 소스](../ReflectionGeneratedSources.ko.md) 6.3절(메타 생성 실행)에 모듈 판정·병렬·inventory 게시를 적는다(MI1–MI3).
- [셰이더 시스템](../ShaderSystem/README.ko.md) 16.2절(셰이더 파라미터 생성): `shader-parameters:<모듈>` 판정(MI2).
- [유니티 빌드·PCH](../UnityBuildPch.ko.md): 4.1절 선언 입력 key에서 헤더 이름 목록을 빼고 정밀 include 검증을 더한다(SCS1). 7.1절 작업 파일 집합의 병합 규칙(1.6절). 8.7절 도구 빌드 ID 정규화에 3.4.1절 인자. 그 문서는 이미 500줄을 넘으므로 개정 때 장별 폴더로 나눈다.
- [빌드 타깃](../BuildTargets/README.ko.md) 3.2절: 서드파티에 타깃 매크로를 넘기지 않는 안(1.7절, 선택 — 정책 검사 "서드파티 원본·`Rocks.patch`에서 `ROCKS_WITH_*` 사용 금지"와 함께).
- `Programs/Rocks.Build/README.ko.md`: 저장소 설정·보고 항목, 메타 단계 기록 파일.
- `.github/workflows/foundation.yml`(빌드를 부르는 모든 작업 흐름): `ROCKS_COMPILE_STORE=off`, `ROCKS_META_INCREMENTAL` 값 명시(3.8절).

### 8.2 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-28 | 첫 초안. 출력 폴더 301개·빌드 로그·coordinator의 wt-ut5 측정(Sandbox 증분 523초)으로 원인 넷을 찾았다(1장). 1순위 메타 단계 증분(BR1–BR8, MI0–MI4), 2순위 공유 compile 결과 저장소(BR9–BR24, SCS1–SCS6). 다른 세션 요구(물리·텍스처·메시·UI·머티리얼)를 4장에 대조했다 |
| 2 | 2026-11-28 | 교차 검증 반영. B1(다시 생성한 의존 모듈은 stage와 대조, T5-2 기대 정정), B2(입력 메모 하나·메타 단계 안정 검사·도구 디렉터리 포함), B3(저장소 게시는 빌드 끝 안정 검사 뒤 대기 목록에서, 게시 직전 재해시). I1 도구 ID(디렉터리 전체·런타임 판·SDK 경로·게시 위치·clang-format 전달), I2 clang wrapper·depfile 강제·제외 규칙(T6), I3 열거·존재 확인 기록, I4 이름 목록 방식 조건 확대·모든 파일 이름 inventory·대소문자·reparse·모든 후보, I5 PCH 탐색 목록(BR29), I6 inventory 정규화·도구 경로 독립(BR30), I7 시험 하네스·CI·TimeProvider(3.8절), I8 증분 정리·dedupe 시각 갱신, I9 MI0 내용·shadow mode(BR25)·단계 순서, I10 DAG 간선·실패 보고·메모리 상한. M1 UE 근거 정정(link cache·`validateCacheWritesOutput`·UHT 모듈별 기록), M2 `meta-ids`를 `meta`에 합침, `--check`·`OutputInvocationLock` 전제, stdout 정규화, 전체 재컴파일 한 번으로 묶음, up to date 줄의 형식 수, 의존 기록 위치, prefix map 우선순위(T2-3)·Coverage `-fcoverage-compilation-dir` |
| 3 | 2026-11-28 | MI0·MI1(shadow)·MI2 구현 반영: 기록 위치를 `meta-cache.json` 하나로, 인자의 stage 경로는 모두 `<STAGE>`, 실패한 clang 실행도 depfile 필수(실측), 셰이더 파라미터 key에서 clang-format 제외, 이번 단계가 나중에 쓰는 출력(셰이더 파라미터 루트)은 판정·안정 검사에서 뺌. MI0 측정(2.12절)과 shadow 확인(2.13절 — 불일치 0, 무관한 `.cpp` 수정에서 28/28 would-hit) |
