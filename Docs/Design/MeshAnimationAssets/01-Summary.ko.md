## 1. 결정 요약

| 항목 | 결정 | 근거·대안 |
|---|---|---|
| 원본 데이터 모델 | 속성 기반 다각형 메시 `MeshSource`(UE `FMeshDescription` 대응). 정점·코너·변·삼각형·다각형·다각형 묶음의 여섯 요소와, 요소별로 이름 있는 속성 배열을 둔다 | UE 정적·스킨 메시가 모두 이것을 원본으로 쓴다(3절). 빌드 결과(렌더 버퍼)를 원본으로 두면 n-gon·hard edge·정밀도를 잃는다(3.6절) |
| 원본 저장 | 속성 배열 하나 = Bulk entry 하나. 요소 수·속성 목록은 리플렉션 구조체에 둔다 | 텍스처 원본(`TextureSource`)과 같은 방식이다. 네이티브 직렬화 경로를 만들지 않는다(AssetSystem 5.6절). 대안은 결정 MA-D2 |
| 정적 메시 | `StaticMesh : StreamableRenderAsset`. 원본 LOD 목록, 빌드·축소 설정, 머티리얼 슬롯, 섹션 정보, 소켓, 단순 충돌, 쿠킹 렌더 데이터 | UE `UStaticMesh`. 기반 클래스는 AssetSystem 12.1절이 이미 메시용으로 나눠 두었다 |
| 스켈레톤 | `Skeleton : Asset`. 본 계층(`BoneHierarchy` — 이름·부모·기준 pose), 본별 이동 리타깃 방식, 가상 본, 소켓, 커브 정보, 슬롯 묶음 | UE `USkeleton`·`FReferenceSkeleton` |
| 스킨 메시 | `SkinnedAsset : StreamableRenderAsset`(추상) → `SkeletalMesh`. 메시는 자기 본 계층을 갖고 Skeleton을 Hard로 참조한다. 스킨 가중치·morph target의 원본은 `MeshSource` 속성이다 | UE 5.8은 LOD별 MeshDescription이 원본이고 `UMorphTarget`은 빌드 결과다(6절) |
| 애니메이션 | `AnimationAsset`(추상, Skeleton Hard + Build 참조) → `AnimationClip`. 원본은 프레임 균일 간격의 본 트랙(이동·회전·크기, float), 커브(`TangentCurve`), 이벤트, 동기 마커, 설정이다 | UE 5.8 데이터 모델은 회전을 float Euler 채널로 저장한다. 원본 quaternion이 따로 없고 프레임별 평가값이 받을 수 있는 최선이다(7.2절) |
| 머티리얼 연결 | 메시의 머티리얼 슬롯이 `ObjectRef<MaterialBase>`를 Hard로 갖는다. 슬롯 배열은 `StructRefArray` | MaterialSystem 4.2·4.4절. 참조 struct의 배열은 `StructRefArray`만 허용된다(ReflectionExtensions 1.3절 표) |
| 정점 형식 | 쿠킹 정점 stream은 GraphicsTypes `VertexFormat` 번호로 기록한다. index 형식도 GraphicsTypes `IndexFormat` 번호로 기록한다 | RenderBase GX3a(10.4절)에서 정점 원소 형식 20종이 구현되었다. `IndexFormat`은 MA2 전에 Graphics에서 GraphicsTypes로 옮기고 고정 번호표를 붙인다(RenderBase 결정 R20) |
| tangent 계산 | MikkTSpace 참조 구현을 ThirdParty로 쓰되, CRT 초월 함수를 Core 결정적 수학으로 바꾸는 패치를 둔다 | UE와 같은 tangent를 내야 노멀맵 베이크 결과와 맞는다. 쿠킹 중 CRT 초월 함수는 금지다(AssetSystem 10절). 결정 MA-D5 |
| 축소 LOD·Nanite·lightmap UV 생성·거리장·Lumen 카드 | 설정은 보존한다. 빌드는 후속 단계에서 하고, 그때까지는 `UnsupportedSetting` 경고를 낸다 | 해당 기능의 소비자(축소기·GI·VirtualGeometry)가 아직 없다 |
| 애니메이션 압축 | 쿠킹 v1은 비압축(상수 트랙 접기만)이다. ACL은 후속이다 | 계획 15절 |

> **콘텐츠 해시 전환(예정, [ContentHash](../ContentHash.ko.md), 2026-11-28).** 이 문서가 콘텐츠 주소·동일성 판정으로 정한 SHA-256 값(메시 원본·중간 결과 해시(MeshSource Bulk 포함))은 `ContentHash`(BLAKE3-256, 32 byte)로 바뀐다. 하위 호환 없이 일괄 B(CH-B3b)에서 한 번에 바뀌고, 그때 본문과 형식 버전을 고친다. 외부 무결성·프로토콜 SHA-256(CH5)은 남는다.
