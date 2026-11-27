## 8. 다른 문서 개정과 개정 이력

### 8.1 다른 문서 개정(구현 때 먼저 적는다)

- [리플렉션 기반](../ReflectionFoundations.ko.md) 118행 "탐색은 빌드마다 한다 … 필요해지면 캐시"를 2장(메타 단계 증분)으로 바꾼다. [리플렉션 생성 소스](../ReflectionGeneratedSources.ko.md) 6.3절(메타 생성 실행)에 모듈 판정·병렬·inventory 게시를 적는다(MI1–MI3).
- [셰이더 시스템](../ShaderSystem/README.ko.md) 16.2절(셰이더 파라미터 생성): `shader-parameters:<모듈>` 판정(MI2).
- [유니티 빌드·PCH](../UnityBuildPch.ko.md): 4.1절 선언 입력 key에서 헤더 이름 목록을 빼고 정밀 include 검증을 더한다(SCS1). 7.1절 작업 파일 집합의 병합 규칙(1.6절). 8.7절 도구 빌드 ID 정규화에 3.4.1절 인자. 그 문서는 이미 500줄을 넘으므로 개정 때 장별 폴더로 나눈다.
- [빌드 타깃](../BuildTargets/README.ko.md) 3.2절: 서드파티에 타깃 매크로를 넘기지 않는 안(1.7절, 선택 — 정책 검사 "서드파티 원본·`Rocks.patch`에서 `ROCKS_WITH_*` 사용 금지"와 함께).
- `Programs/Rocks.Build/README.ko.md`: 저장소 설정·보고 항목, 메타 단계 기록 파일.

### 8.2 개정 이력

| 개정 | 날짜 | 내용 |
|---|---|---|
| 1 | 2026-11-28 | 첫 초안. 출력 폴더 301개·빌드 로그·coordinator의 wt-ut5 측정(Sandbox 증분 523초)으로 원인 넷을 찾았다(1장). 1순위 메타 단계 증분(BR1–BR8, MI0–MI4), 2순위 공유 compile 결과 저장소(BR9–BR24, SCS1–SCS6). 다른 세션 요구(물리·텍스처·메시·UI·머티리얼)를 4장에 대조했다 |
