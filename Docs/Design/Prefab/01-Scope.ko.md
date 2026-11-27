# prefab 설계 — 목적·범위·비범위

[목차](README.ko.md)

## 1. 목적·범위·비범위

### 1.1 목적

편집기에서 만든 Actor·Component 묶음을 **원형(archetype)** 으로 저장하고, 원형에서 인스턴스를 여러 번 만들며, 인스턴스마다 원형과 다른 부분만 **override** 로 기록한다. 원형이 바뀌면 override하지 않은 값은 새 원형을 따르고 override한 값은 유지된다. 원형은 다른 원형을 품을 수 있고(중첩), 다른 원형을 기준으로 차이만 저장할 수 있다(variant).

이 기능은 여러 문서가 "prefab 단계"로 미뤄 둔 일을 한곳에서 정한다.

- default subobject와 SubobjectGuid, 인스턴스별 clone·remap(EIP 5.3절, RCM 43·73행, OF 0절, SL 4.5절).
- 차이 저장 기준의 일반화 — "prefab이 생기면 기준이 archetype 사슬이 된다"(RCM 72행, EIP 5.3절).
- Map·Set의 key 단위 차이(RX 20절 열린 문제), 배열 원소 안정 ID(RF 1044행).
- Instanced 하위 객체(RX 0절, OF 0절).
- 부착 계층 영속(ER 4.6절 "레벨·prefab 저장 전에 영속한다").
- 원형의 Transient 클래스 예외(RX 9.2절 694행·20절).

### 1.2 범위

| 항목 | 이 문서가 정하는 것 |
|---|---|
| 클래스 원형 | 선언형 default subobject(MetaCompiler subobject 표), SubobjectGuid, subobject 기본값 레코드, 파생 클래스의 상속 subobject 기본값 덮기 |
| prefab 에셋 | 노드 표·NodeGuid·원형 데이터 형식, 패키지(A1) 안 위치, 두 가지 revision, 해석된 원형 캐시 |
| 인스턴스 생성 | 그래프 복원·내부 참조 재연결·그래프 사이 참조·제외 플래그·이름·소유 목록·Transform 합성·World 등록·hook 순서 |
| override | 필드 경로 기반 값 override, Map·Set key 추가·제거, 노드 추가·삭제, 명시 override 집합, 통째 단위 값의 적용 |
| 저장 | 원형 대비 차이, 레벨 배치 인스턴스 기록, 펼친 객체의 저장 제외 |
| 전파 | revision 변경 시 3-way 재기준(rebase), 충돌 분류, override 마이그레이션 |
| 중첩·variant | 원형 사슬, 순환 금지, 깊이·개수 한도 |
| 실행 비용 | spawn 경로 비용, 원형 캐시, 인스턴스 장부, GC·클러스터 관계 |
| 진단·스레드·수명 | 오류 코드, 단계별 스레드, lease 수명 |

### 1.3 비범위

- 편집기 UI(prefab 편집 모드·override 표시·Apply/Revert 버튼의 화면) — UI 툴킷 CU 단계와 편집기 단계가 정한다. 이 문서는 그 명령이 부르는 API와 결과 규칙만 정한다.
- Undo·transaction 기록 형식(CU4), setter·통지 hook(CU5) — 선행 조건으로만 적는다.
- 네트워크 복제 spawn(EIP 20.3절 마지막 문단) — 원형 ID·revision·spawn 인자 검증 규칙만 3.13절에 자리로 둔다.
- 세이브게임 형식(EIP 951행) — 파생 ID 규칙(3.4절)만 제공한다.
- 렌더 instancing으로 굽기(UE Packed Level Actor 대응) — 렌더 instancing 기반이 생긴 뒤의 후속이다(4절 D1, 5.2절 PF9).
- 별도 Level로 로드하는 묶음(UE Level Instance의 LevelStreaming 동작)과 WorldPartition 셀 편입 — 레벨 패키지·스트리밍 단계와 함께 정한다(4절 D1·D9, EIP 2064행 P2·M7).
- 레벨 스크립트 — `LevelScript`(레벨마다 숨은 Actor)는 prefab 본문과 Level Instance 안에 둘 수 없다. prefab 저장은 LevelScript 파생 노드를 `PrefabUnsaveableNode`로 거부한다(3.11절). Level Instance(D1 (c))를 도입할 때 다시 본다([레벨 스크립트](../LevelScript.ko.md) LD11 — UE는 Level Instance에 LSA를 허용하고 break만 막으므로 Rocks가 더 엄격하다).
