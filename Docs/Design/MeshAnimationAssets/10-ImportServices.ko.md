## 10. 임포트 서비스

임포터(교환 형식·glTF·FBX)는 파일 형식을 읽어 아래 명세를 만들고, 공용 서비스가 에셋을 만든다. 텍스처의 `TextureImportSpec`(AssetSystem 12.3절)과 같은 방식이다.

| 서비스 | 입력 | 결과 |
|---|---|---|
| `MeshTools::ImportStaticMesh(StaticMeshImportSpec)` | LOD별 `MeshSource`(원본·결과 메시), 설정, 슬롯 목록(이름 + 머티리얼 참조), 소켓, 단순 충돌 | `StaticMesh` 패키지 |
| `AnimationTools::ImportSkeleton(SkeletonImportSpec)` | `BoneHierarchy`, 소켓, 가상 본, 커브 정보, 리타깃 방식 | `Skeleton` 패키지(기존 스켈레톤이면 병합) |
| `MeshTools::ImportSkeletalMesh(SkeletalMeshImportSpec)` | 위 정적 메시 입력 + 메시 본 계층 + 스켈레톤 참조 | `SkeletalMesh` 패키지, 필요하면 스켈레톤 병합 |
| `AnimationTools::ImportClip(ClipImportSpec)` | `ClipSourceData`, 이벤트·마커·설정, 스켈레톤 참조 | `AnimationClip` 패키지 |

서비스는 3.1·3.3·3.5·6.1·7.2절 검증을 하고 진단(AssetSystem 4.6절 형식)을 돌려준다. 저장은 원자적이다. 실패하면 기존 에셋을 보존한다(AssetSystem 17절).
