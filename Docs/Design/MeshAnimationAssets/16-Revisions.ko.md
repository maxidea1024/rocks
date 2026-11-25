## 16. 개정 이력

- 개정 1(2026-11-20): 초안. 사용자 지시(2026-11-19 "언리얼에서 텍스쳐·스테이틱메시·매터리얼·애니메이션을 임포트하는 기능 … 관련 자료구조 잡는것도 같이")에 따라 작성했다. UE 5.8.4 조사 세 갈래(텍스처·정적 메시 원본 / 스켈레톤·스킨 메시·애니메이션 / 머티리얼 그래프·플러그인 기반)를 근거로 했다.
- 개정 2(2026-11-20): 교차 검증(설계 반례 리뷰, UE 사실 확인) 반영. 17절.
- 개정 3(2026-11-20): 15절 요구 MR1–MR10을 대상 문서에 반영했다(RenderBase 29, Renderer 10, TextureStreaming 5, MaterialSystem 6, PhysicsJoltAdapter 1, AssetSystem 10). MR8은 확인 결과 `FrameRate`·`Box<double>` 미러만 없어 MA2·MA6 단계에 넣었다. 15절에 상태 열을 더했다.
- 개정 4(2026-11-20): MA2 전 결정을 권장안으로 확정해 반영했다(사용자 지시 "MA2 전 결정 사항들도 권장안대로"). index 형식은 GraphicsTypes `IndexFormat` 번호(자체 `MeshIndexWidth` 삭제, MA-D10 개정), skinned 쿠킹 데이터는 섹션별 palette 대신 LOD 통합 bone map과 256 기준 번호 폭(RenderBase R18), 메시 LOD 스트리밍은 TextureStreaming 4.9절(T9). 15절 MR2·MR3·MR7 상태를 고쳤다.
- 개정 5(2026-11-20): MA1 `MeshSource` 모듈 구현에서 정한 점을 3.7절에 적었다(위치 속성 `Position`, Load·Store 인자와 AssetRuntime `MemoryBulkSource`, 로드·검증 판정 분담, 엔진 이름 속성 판정, 스킨 가중치 중복·합 0, 값 정리 순서, 가변 길이 값의 메모리 형태, 진단 형식, 위상 조회). 3.2절 표에 `Position`을 더하고 13절 MA1 상태를 적었다.

- 개정 6(2026-11-21): 사용자 지시 "진행 가능한거부터"에 따라 MA2를 MA2a(빌드 핵심 — 순수 함수·MikkTSpace)와 MA2b(클래스·쿠킹·임포트)로, MA4를 MA4a(본 계층·스켈레톤 클래스·병합 판정)와 MA4b(임포트·병합 저장)로 나눴다. MA3·MA5·MA6의 선행을 b 단계로 고쳤다.

- 개정 7(2026-11-21): MA2a 구현에서 정한 점을 5.4절에 적었다(API·입출력 형태, 지금 모듈 의존, 원본 사본의 범위, UE 원본 임계값 확정과 removeDegenerates를 끈 때의 정확 일치 퇴화 제거, 정점 단위 법선 묶음과 대각선 soft, 비유한 중간값, buildScale 검사(`InvalidSetting`), MikkTSpace 저자 원본·패치·원본 버그 수정·참조 글루, Core `Acos`, 용접 색 비교, 섹션·슬롯, 경고 순서, 결정성, 진단 등록, 시험 도우미 이동). 5.2절 9단계 임계값 문장, 12.2절 대조표, 13절 MA2a 상태를 고쳤다.

- 개정 8(2026-11-21): MA4a 구현에서 정한 점을 6.4절에 적었다(모듈 의존, 계층 불변식의 순서와 경계, pose 정규화 함수, 스켈레톤 필드 규칙, 새 진단 코드 `InvalidSkeletonData`와 `SkeletonDiagnostic`, UE 기본 경로를 옮긴 병합 판정과 대소문자 이름 바꾸기, 결정성, 시험 환경). 11절 표에 `InvalidSkeletonData`를 더하고 12.3절 대조표, 13절 MA4a 상태를 고쳤다.

- 개정 9(2026-11-21): MA3·MA5·MA6도 같은 방식으로 a·b로 나눴다(MA3a 충돌 값·검증, MA5a 스킨 빌드 핵심, MA6a 클립 핵심 — 에셋 DB·쿠커 없이 되는 부분). UX1a는 엔진 플러그인 빌드 지원(BuildTargets BS4 엔진 쪽)이 없어 보류했다.

- 개정 10(2026-11-21): MA5a 구현에서 정한 점을 6.5절에 적었다(API·결과 형태, 설정 struct 상속과 필드 번호, 스킨 빌드의 buildScale 보존, 설정 검사와 영향 상한 기본값, 가중치를 담은 원본 사본, 스킨 임계값과 번호가 겹친 삼각형 제외, 영향 줄이기와 루트 본 대체(`MissingInfluences` — 새 경고), 용접의 스킨 항목(영향·프로필·morph), 가중치 프로필 빌드, 쿠킹 칸 수와 8비트 잔차, bone map과 필요 본, inverse-bind와 `SingularBindPose`, morph target 짝·법선 재계산·담는 조건, 경고 순서, 정적 빌드와의 단계 공유, 진단 등록, 결정성). 11절 표에 `MissingInfluences`를 더하고 12.4절 대조표, 13절 MA5a 상태를 고쳤다.

- 개정 11(2026-11-21): MA6a 구현에서 정한 점을 7.5절에 적었다(파일·의존, `AnimationAsset`과 Build 의존 시점, CoreTypeBindings `FrameRate` 미러와 검증 hook, 플랫폼별 목표 비율 `PlatformFrameRate`, 커브 표시 bool과 변환 커브 채널 배열, 원본 bulk 배치·정렬·상수 접기, 원본 검증, UE `Evaluate`와 같은 샘플링 규칙(7.4절의 slerp를 UE nlerp로 바꿈), 쿠킹 변환의 정수 표본 수·트랙 대응·변환 커브 제외, 쿠킹 bulk 배치, 클립 검증·정렬, 진단 등록, 결정성). 13절 MA6a 상태, 15절 MR8 상태, 12.5절 대조표를 고쳤다.

- 개정 12(2026-11-21): MA3a 구현에서 정한 점을 4.8절에 적었다(형상 struct와 공통 필드·UE 기본값·열거 번호, 크기 규칙(반경·크기 양수, 캡슐 길이 0 허용, 실린더 길이 양수, 유한값), 볼록 형상 bulk 배치와 점 수·index·삼각형 수 상한·transform 규칙, 형상 수 상한, 검증 함수와 새 진단 `InvalidCollisionData`, 임포터의 `UnsupportedCollisionShape` 형식, 결정성). 11절 표에 `InvalidCollisionData`를 더하고 12.6절 대조표, 13절 MA3a 상태를 고쳤다.

- 개정 13(2026-11-21): MA 묶음 리뷰를 반영했다. 원본 로드의 Name 값 bulk 최소 길이 판정과 다각형 묶음 수 상한 `MeshMaxPolygonGroups`(3.1절, 3.7절 12), 겹치는 코너의 UE 가중합 정렬 키(5.4절 16), MikkTSpace 느린 경로의 자기 index 방어와 오브젝트 검사를 미룬 사유(5.4절 17), 본 없는 메시 병합 거부(6.4절 6), 쿠킹 데이터 로드의 규칙별 진단과 신뢰 범위(7.5절 10), 12절 머리 목록(스레드·palette·재쿠킹 항목 정리), 13절 b 단계 행의 로드 경로 검증 항목.
- 개정 14(2026-11-23): MA2b의 머티리얼 선행을 MaterialSystem MS3(에셋 모듈 전체)에서 MS3a(`MaterialBase`·`Material` 골격)로 좁혔다. 슬롯 참조 타입만 있으면 되기 때문이다(사용자 결정 2026-11-23 "Material 에셋 클래스만 먼저 만들어").
