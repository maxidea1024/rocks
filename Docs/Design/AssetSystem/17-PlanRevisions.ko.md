## 17. 계획 문서 개정 사항

계획 문서에 함께 반영한다.

- 2026-11-22 Rockpile 17절의 AssetSystem 개정 반영(A3 착수, 사용자 결정 — Rockpile 레코드 계약): 1절 A3 행, 2절 프로세스 행, 3절 에셋 너겟 상태, 8절 동시 기록 문장, 9절(세대 Guid 둘·결과 압축과 저장 압축·Record·읽기 검증 보장 범위·쓰기 판정·인터페이스), 9.1절 A3 세부, 13절(`--verify-nuggets-source`·`nugget-gc` 위임), 14절 시나리오 9·13, 18절 계약 목록. 같은 날 CoreDataFoundations 132행·Rockpile 12.4절(프로젝트 세대 예외)도 고쳤다. Rockpile 17절의 나머지(계획 문서·Compression·PlatformLayer 등)는 Rockpile 서버 착수 때 반영한다.

- 하드 참조 = 다른 패키지 객체로의 `ObjectRef`(에셋 객체 Registry 안), 소프트 = `SoftObjectRef<T>`(AssetId + 객체 경로), World 객체 → 에셋은 `SoftObjectRef` + `AssetLease`(6.3절). §7.1의 `ObjectRef<MaterialAsset>` 예시 수정.
- 패키지당 대표 에셋 ID = 패키지 ID. 쿠킹 패키지 파일 이름은 `<AssetId>-<패키지 SHA-256 앞 16자>`(내용 주소, 4.2절).
- §9.1: "AssetRegistry"는 AssetIndex(에디터 패키지 인덱스, 재검색으로 복원되는 캐시)이고, 별칭 표는 인덱스 안이 아니라 `Config/AssetAliases.json`의 버전 관리 사용자 데이터다(재검색으로 복원되지 않는다, 4.3절).
- 로딩 API 추가: `AssetRequest.expectedType`·`scope`, `LoadBlocking`(도구·초기화 전용), `OnReady(LoadTicket&)` 참조 인자. `OnReady` lane·`ReadyMask`의 이번 구현 범위는 개정이 아니라 11.1절의 `계약` 상태다.
- §9.2 수명: 상주 단위 = 하드 참조 SCC, Closing 대기 중 revive, Pump 안의 Closing → Discard(11.4절). 계획의 발급·Closing 선형화 규칙과 같다.
- 임포트 worker process(§9.1): 장시간 실행 호스트(에디터)를 임포터 실패로부터 보호하는 장치이므로 에디터와 함께 구현한다. 이번 범위의 임포트 호스트는 명령행 프로세스 자체이며(실패해도 원자적 저장으로 에셋 보존), `Importer` 인터페이스는 입력(원본 바이트·옵션)·출력(객체 그래프·bulk)만 주고받아 프로세스 경계로 옮길 수 있게 한다.
- Virtual Asset(§9.4): 원본 bulk의 분리 저장소는 후속. 이번에는 에디터 패키지 안 bulk가 권위 저장소다(에셋 너겟에만 있는 원본은 허용하지 않는다는 원칙은 지킨다).
- 런타임 VFS(APK)는 Android 그래픽·패키징과 함께 후속(쿠킹 매니페스트는 쿠킹 루트 기준 상대 경로만 사용).
- 용어: UE의 DDC(Derived Data Cache)에 해당하는 것을 **에셋 너겟**(AssetNugget)이라 부른다(사용자 결정). 저장되는 가공 결과 하나가 너겟이고, 모듈은 `AssetNugget`, 모듈 안 타입은 짧은 `Nugget` 접두(`NuggetCache`·`NuggetBackend`·너겟 키)를 쓴다. 2026-11-13에 "빌드 캐시(BuildCache)"에서 바꿨다 — 코드 빌드의 캐시와 헷갈렸기 때문이다. 코드 산출물 캐시는 계획 문서의 `BuildArtifactCache`다. 계획 문서 전체에 반영.
- 반영한 계획 문서 위치: §3.1 모듈 표(AssetRuntime은 에셋 너겟 reader가 아니라 쿠킹 매니페스트 reader), §7.1 예시(`SoftObjectRef<MaterialAsset>`), §9.1 AssetIndex·별칭·worker process, §9.2 `AssetRequest`·수명, §9.4 Virtual Asset 후속·쿠킹 패키지 이름(내용 주소), VFS 후속, A21 위험 항목.
