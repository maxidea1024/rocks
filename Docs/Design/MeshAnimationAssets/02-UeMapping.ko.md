## 2. UE 5.8 대응

| Rocks | UE 5.8 | 근거 |
|---|---|---|
| `MeshSource`(원본 메시) | `FMeshDescription` | RT/MeshDescription/Public/MeshDescription.h:131-149 |
| 정점 `Vertex` / 코너 `Corner` / 변 `Edge` / 삼각형 `Triangle` / 다각형 `Polygon` / 다각형 묶음 `PolygonGroup` | Vertex / VertexInstance / Edge / Triangle / Polygon / PolygonGroup | 같은 곳 |
| `StaticMeshSourceLod` | `FStaticMeshSourceModel` | E/Classes/Engine/StaticMeshSourceData.h |
| `MeshBuildSettings` | `FMeshBuildSettings` | E/Classes/Engine/EngineTypes.h:2942 |
| `MeshReductionSettings` | `FMeshReductionSettings` | E/Public/MeshReductionSettings.h:34 |
| `MeshMaterialSlot` | `FStaticMaterial`·`FSkeletalMaterial` | E/Classes/Engine/StaticMesh.h:486, E/Classes/Engine/SkinnedAssetCommon.h:370 |
| `MeshSectionInfo` | `FMeshSectionInfo` | StaticMesh.h:350-370 |
| `MeshSocket` | `UStaticMeshSocket` | E/Classes/Engine/StaticMeshSocket.h |
| `SimpleCollision`·`CollisionComplexity` | `UBodySetup`·`FKAggregateGeom`·`ECollisionTraceFlag` | E/Classes/PhysicsEngine/BodySetup.h, AggregateGeom.h:23-52, RT/PhysicsCore/Public/BodySetupEnums.h:10-22 |
| `StaticMeshRenderData`·`MeshRenderLod`·`MeshRenderSection` | `FStaticMeshRenderData`·`FStaticMeshLODResources`·`FStaticMeshSection` | E/Public/StaticMeshResources.h |
| `BoneHierarchy`·`BoneInfo` | `FReferenceSkeleton`·`FMeshBoneInfo` | E/Public/ReferenceSkeleton.h:13-23, 107-116 |
| `VirtualBone` | `FVirtualBone` | E/Classes/Animation/Skeleton.h:254-266 |
| `BoneSocket` | `USkeletalMeshSocket` | E/Classes/Engine/SkeletalMeshSocket.h:25-41 |
| `SkeletonCurveInfo` | `FCurveMetaData` | E/Public/Animation/AnimCurveMetadata.h:20-52 |
| `BoneTranslationRetarget` | `EBoneTranslationRetargetingMode` | Skeleton.h:71-85 |
| `SkinnedAsset`·`SkeletalMesh` | `USkinnedAsset`·`USkeletalMesh` | E/Classes/Engine/SkinnedAsset.h:46, SkeletalMesh.h |
| `SkeletalMeshSourceLod` | `FSkeletalMeshSourceModel`(`FSkeletalMeshLODInfo` 상속) | E/Classes/Engine/SkeletalMeshSourceModel.h:28-29 |
| `BoneInfluence` | `FBoneWeight` | RT/AnimationCore/Public/BoneWeights.h:206-210 |
| `AnimationAsset`·`AnimationClip` | `UAnimationAsset`·`UAnimSequence` | E/Classes/Animation/AnimationAsset.h:987, AnimSequence.h:202 |
| `ClipEvent`·`ClipSyncMarker` | `FAnimNotifyEvent`·`FAnimSyncMarker` | E/Public/Animation/AnimTypes.h:276-356, 479-506 |
| `ClipCurve`·`ClipTransformCurve` | `FFloatCurve`·`FTransformCurve` | E/Public/Animation/AnimCurveTypes.h:217-223, 303-319 |

이름 원칙: "정점·변·삼각형·다각형·morph target·소켓·가상 본" 같은 업계 공용어는 그대로 쓴다. UE의 vertex instance는 Rocks에서 **코너**라 부른다. 위치를 공유하는 정점과 구분하기 위해서다.
