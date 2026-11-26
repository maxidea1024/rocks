## 8. 모듈 배치

| 모듈 | 계층 | 책임 | 의존 |
|---|---|---|---|
| `MeshSource` | Engine Runtime | `MeshSourceData`·`MeshSource`, 속성 이름 상수, `ValidateMeshSource`, 위상 조회(다각형의 삼각형·코너 등) | AssetRuntime(`BulkDataRef`·`Package`)·Core·MetaSchema·Serialization |
| `Animation` | Engine Runtime | `BoneHierarchy`·`Skeleton`·`AnimationAsset`·`AnimationClip`·쿠킹 데이터, 클립 샘플링(원본·쿠킹) | AssetRuntime·Curves·Core |
| `Mesh` | Engine Runtime | `StaticMesh`·`SkinnedAsset`·`SkeletalMesh`, 슬롯·섹션·소켓·단순 충돌·빌드 설정 값 타입, 쿠킹 렌더 데이터 구조, 로드 검증 | AssetRuntime·MeshSource·Animation·Material·GraphicsTypes(MA2b1 — Animation은 MA5b) |
| `MeshTools` | Engine Developer | 빌드(5.2·6.3절), 법선·tangent·MikkTSpace, 용접, 쿠킹 처리기, 메시 임포트 서비스 | Mesh·MeshSource·AssetCooker·AssetDatabase·AssetNugget·ThirdParty MikkTSpace(패치) |
| `AnimationTools` | Engine Developer | 클립 빌드·재샘플링·쿠킹 처리기, 스켈레톤 병합, 임포트 서비스 | Animation·AssetCooker·AssetDatabase·AssetNugget |

- 텍스처 모듈 구성(`Texture` Runtime + `TextureTools` Developer, AssetSystem 3절)과 같은 모양이다. 렌더 층은 이 에셋 모듈을 모른다. 쿠킹 데이터 → 렌더 자원 연결은 렌더 쪽 연결 모듈이 한다(머티리얼의 `Material` → `MaterialRuntime` 분리와 같다).
- `Mesh`가 `Animation`에 의존하는 이유는 `SkeletalMesh`가 `Skeleton`·`BoneHierarchy`를 쓰기 때문이다. 반대 방향은 없다. 스켈레톤의 미리보기 메시는 에디터 UI 범위라 넣지 않는다.
- `MeshSource`가 AssetRuntime에 의존하는 것은 Runtime 모듈 사이 의존이 하나 느는 것뿐이고 방향 문제는 없다(결정 MA-D3). AssetSystem 3절 모듈 그래프에 이 다섯 모듈을 더한다(요구 MR10).
