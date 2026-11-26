## 2. 범위

| 받는 것 | UE 클래스 |
|---|---|
| 텍스처 | `UTexture2D`·`UTextureCube`·`UTexture2DArray`·`UTextureCubeArray`·`UVolumeTexture` |
| 정적 메시 | `UStaticMesh` |
| 스켈레톤·스킨 메시 | `USkeleton`·`USkeletalMesh` |
| 애니메이션 | `UAnimSequence` |
| 머티리얼 | `UMaterial`·`UMaterialInstanceConstant`·`UMaterialFunction`·`UMaterialFunctionInstance`·`UMaterialParameterCollection` |

범위 밖(추출 시 `UnsupportedAsset` 진단): 레벨·액터·블루프린트, montage·composite·blend space·pose asset, physics asset, 렌더 타깃·미디어·IES·runtime virtual texture, Niagara, landscape, 사운드. Rocks에서 UE로 되돌리는 방향은 없다.
