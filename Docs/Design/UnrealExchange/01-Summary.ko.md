## 1. 결정 요약

| 항목 | 결정 | 근거·대안 |
|---|---|---|
| 방식 | UE 에디터 플러그인이 에디터 메모리의 객체에서 원본 데이터를 꺼낸다 | `.uasset` 해석은 UE 직렬화(버전·커스텀 버전·bulk·압축·tagged property)를 Rocks에 다시 만드는 일이다. glTF·FBX는 텍스처 설정·머티리얼 그래프·n-gon·애니메이션 이벤트를 담지 못하고, glTF는 좌표 변환(Y 위, 오른손, m)까지 들어간다(결정 UX-D1) |
| 형식 | 디렉터리 하나: `bundle.json`(목록) + 에셋별 JSON + 내용 주소 blob(`blobs/<sha256>.bin`) | 사람이 읽고 diff할 수 있는 메타데이터와, 크고 중복되는 배열의 분리. 같은 blob은 한 번만 쓴다 |
| 데이터 수준 | "UE 원본": 텍스처 `FTextureSource` 픽셀, 메시 `FMeshDescription`, 애니메이션 데이터 모델, 머티리얼 그래프 | Rocks가 자체 빌드·쿠킹을 하므로 UE 빌드 결과(렌더 버퍼·압축 텍스처·압축 애니메이션)는 원본으로 쓰지 않는다. 축소로 만든 LOD처럼 원본이 없는 것만 보조로 받는다 |
| 손실 원본 텍스처 | UE에 JPEG·UEJPEG로 저장된 원본도 옵션 없이 받는다(해제한 픽셀) | 사용자 결정(2026-11-23 — 2026-11-20의 기본 거부·`--allow-lossy-origin`을 대체, AssetSystem 12.2절 JPEG 허용과 같다). UE 5.8은 기본 설정에서 JPEG 바이트를 원본으로 보관한다(4.3절) |
| 메시 법선·tangent | 정적 메시는 기본으로 "UE 준비 원본"(UE 빌드 전처리를 적용한 사본)을 Rocks 원본으로 쓰고 재계산 플래그를 끈다. 옵션으로 순수 원본 + UE 플래그. 스킨 메시는 순수 원본만 | UE와 같은 셰이딩을 시험하려면 UE의 법선·tangent 계산 결과가 필요하다(5.3절). 스킨 메시는 UE에 사본용 공개 함수가 없다(6.2절) |
| 머티리얼 | 그래프 전체를 노드 단위로 추출하고 Rocks 노드로 번역한다. 번역하지 못하면 플러그인이 구운 텍스처로 "구운 표면" 대체를, 그것도 없으면 엔진 기본 머티리얼 인스턴스로 대체한다 | 그래프만으로는 번역 표가 덜 찬 동안 쓸 수 없고, 굽기만으로는 파라미터·인스턴스 구조가 사라진다. Epic GLTFExporter도 "단순하면 직접, 아니면 굽기" 순서다(8.4절) |
| 애니메이션 | 프레임별 평가 Transform(float) + 커브 키 + 이벤트 | UE 5.8 데이터 모델이 회전을 Euler 채널로 저장한다(MeshAnimationAssets 7.2절) |
| 식별 | Rocks AssetId는 새로 발급(무작위 v4). 재임포트는 ImportRecord·태그의 UE 객체 경로(옛 경로 포함)로 기존 에셋을 찾는다 | AssetSystem 4.1절 규칙을 바꾸지 않는다(결정 UX-D4) |
| 해시 | blob은 SHA-256. 플러그인이 자체 SHA-256 구현을 갖는다 | Rocks `BulkDataRef`가 SHA-256이다. UE에는 공개 SHA-256 API가 Experimental 플러그인에만 있다(11.3절) |
| 플러그인 위치 | UE 플러그인은 저장소 최상위 `Extras/UnrealExport/RocksExport/`, Rocks 임포터는 Rocks 엔진 플러그인 `Engine/Plugins/Importers/UnrealExchange/` | UE 플러그인의 `*.Build.cs`는 UE `ModuleRules`라 Rocks 규칙 glob·정책 검사 루트 밖이어야 한다(11.1절). Rocks 임포터는 선택 기능이라 엔진 핵심 계층이 아니라 플러그인에 둔다(10.1절) |
