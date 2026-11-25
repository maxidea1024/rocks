## 9. 저장 세부

### 9.1 원본 메시 위상 blob 배치

| 순서 | 배열 | 원소 | 길이 |
|---|---|---|---|
| 1 | 코너 → 정점 | uint32 | cornerCount |
| 2 | 변 정점 쌍 | uint32 × 2 | edgeCount |
| 3 | 다각형 둘레 시작 | uint32 | polygonCount + 1(첫 값 0, 단조 증가, 마지막은 전체 길이) |
| 4 | 다각형 둘레 코너 | uint32 | 3번 마지막 값 |
| 5 | 다각형 → 묶음 | uint32 | polygonCount |
| 6 | 삼각형 코너 | uint32 × 3 | triangleCount |
| 7 | 삼각형 → 다각형 | uint32 | triangleCount |

모두 little-endian이고 정렬 패딩이 없다. 정점·코너 번호의 순서는 임포트 결과 그대로이며 저장이 다시 매기지 않는다(재임포트 diff가 안정되도록).

### 9.2 결정적 정렬

속성 목록은 (element, name의 NFC UTF-8 byte 순)으로, 본 트랙은 본 이름 순으로, 커브는 이름 순으로 저장한다. 슬롯·섹션·소켓·본 계층은 의미 있는 순서라 사용자가 정한 순서를 유지한다.

### 9.3 너겟 키

AssetSystem 9절 공통 항목(원본 bytes 해시·처리기 버전·정규 설정·의존 키·프로필·스키마 버전)에 더해 다음을 넣는다.

| 처리기 | 추가 키 입력 |
|---|---|
| 정적 메시 빌드 | LOD별 원본 해시(위상·속성 bulk SHA-256), 빌드 설정 정규형, 쿠킹 플랫폼의 screenSize·minLod, 색 형식 프로필, MikkTSpace 패치 버전, meshoptimizer 어댑터 버전, 임계값 상수 집합 버전 |
| 스킨 메시 빌드 | 위 + 스켈레톤 build key(메시 본 ↔ 스켈레톤 대응), 영향 상한·정밀도 |
| 클립 쿠킹 | 원본 해시, 쿠킹 플랫폼 targetFrameRate, 스켈레톤 build key(본 번호), 보간 규칙 버전 |
