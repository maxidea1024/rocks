## 4. 텍스처

### 4.1 추출

- `UTexture::Source`가 유효해야 한다(동적 텍스처·쿠킹 전용 텍스처는 `Source.IsValid() == false` — E/Classes/Engine/Texture.h:1269-1272). 아니면 `NoSourceData`.
- 픽셀은 `FTextureSource::GetMipData(nullptr)`로 mip 사슬 전체를 한 번 풀고(Texture.h:347, 549-567), (block, layer, mip)별 `FSharedBuffer`를 blob으로 쓴다. 호출마다 다시 푸는 `GetMipData(Out, B, L, M)`(Texture.h:329)는 쓰지 않는다.
- mip 안의 배치는 slice 순서, slice 안은 행 우선·패딩 없음이다(RT/ImageCore/Public/ImageCore.h:234-246). slice 의미: Volume은 depth(mip마다 줄어든다), Array는 층(mip에서 그대로), Cube는 큐브 하나에 6 slice(Cube Array는 6 × N)다(Texture.cpp:3162-3169, 2209-2230).
- **큐브 면 순서.** slice 0–5는 D3D 면 순서(+X·−X·+Y·−Y·+Z·−Z, D3D Y 위 기준)이고 면 안 u·v 방향도 D3D 규약이다(RT/RHI/Public/RHIDefinitions.h:506-515 `ECubeFace`, Dev/TextureCompressor/Private/TextureCompressorModule.cpp:2194-2213). Rocks `TextureSource` 큐브도 같은 순서·방향이므로(AssetSystem 12.3절) 재배치하지 않는다. 참고로 UE는 cubemap 샘플링 때 Y·Z를 맞바꾸므로 UE 월드 축 기준으로는 slice가 +X·−X·+Z·−Z·+Y·−Y에 대응한다. 이 대응은 렌더러가 cubemap을 샘플링할 때 맞춘다(Rocks 좌표도 UE와 같다).
- 위도·경도 큐브맵(`IsLongLatCubemap()`, Texture.h:425-429)은 `longLat: true`로 표시한다. Rocks의 equirect → Cube 변환은 후속이므로(AssetSystem 12.3절) 임포트는 `UnsupportedSourceLayout`이다.
- layer가 2개 이상(다층 가상 텍스처)이거나 block이 2개 이상(UDIM)이면 추출은 하되 Rocks 임포트는 `UnsupportedSourceLayout`이다. UDIM은 가상 텍스처와 함께 다룬다(AssetSystem 12.2절 사용자 결정).

### 4.2 형식 대응

| UE `ETextureSourceFormat`(TextureDefines.h:350-369) | Rocks `TextureSource` 형식 | 변환 |
|---|---|---|
| G8 | G8 | 없음 |
| BGRA8 | RGBA8 | 임포트가 B·R 교환 |
| BGRE8 | RGBA32F | UE `FColor::FromRGBE`와 같은 식으로 해제(지수 0이면 0, 아니면 (m + 0.5) · 2^(e − 136), 알파 1 — RT/Core/Private/Math/Color.cpp:353-369) |
| RGBA16 | RGBA16 | 없음 |
| RGBA16F | RGBA16F | 없음 |
| G16 | G16 | 없음 |
| RGBA32F | RGBA32F | 없음 |
| R16F | R16F | 없음 |
| R32F | R32F | 없음 |

layer의 감마 공간(`GetGammaSpace(Layer)` — Texture.h:400)은 `gammaSpace`(`Linear`·`sRGB`·`Pow22`)로 기록하고, 임포트가 Rocks `SRGB`·`bUseLegacyGamma` 설정 해석(AssetSystem 12.2절 색 해석 1번)과 대조한다. 둘이 다르면 `GammaMismatch` 경고를 내고 UE 설정값을 따른다.

### 4.3 손실 원본

`GetSourceCompression()`(Texture.h:395)이 `TSCF_JPEG`·`TSCF_UEJPEG`이면 원본 자체가 손실 데이터다. UE 5.8은 JPEG 임포트 때 `[TextureImporter] RetainJpegFormat`(기본 True — Engine/Config/BaseEditor.ini:284-288)이면 JPEG 바이트를 원본으로 보관한다. Interchange 경로(Engine/Plugins/Interchange/Runtime/Source/Import/Private/Texture/InterchangeTextureFactory.cpp:717-721)와 옛 팩토리 경로(Ed/Private/Factories/EditorFactories.cpp:2967-3003) 모두 이 설정을 따른다.

- 플러그인: 해제한 픽셀을 그대로 추출하고 `lossyOrigin: "jpeg"`를 기록한다. 원래 JPEG 바이트(`GetBulkDataPayload()`, Texture.h:443-446)는 넣지 않는다.
- Rocks 임포트: 옵션 없이 받는다(사용자 결정 2026-11-23 — 파일 원본 JPEG 허용과 같다, AssetSystem 12.2절). 처음 결정(2026-11-20)의 기본 거부·`--allow-lossy-origin`·`LossyOriginAccepted` 경고는 없앴다. `lossyOrigin`은 플러그인 출력의 정보 칸으로만 남는다.
- `TSCF_None`·`TSCF_PNG`·`TSCF_UEDELTA`는 무손실이다(TextureDefines.h:337-346. UEDELTA는 무손실 delta + Oodle — Texture.cpp:2612-2650).

### 4.4 설정

UTexture·차원별 클래스의 저장 프로퍼티를 이름 그대로 `settings`에 쓴다. Rocks 텍스처 설정은 UE 5.8 이름과 1:1 대조표가 이미 있다(AssetSystem 12.4절). 임포트는 그 표로 옮기고, 표에서 "후속"인 설정이 기본값이 아니면 `UnsupportedSetting` 경고를 낸다. `Texture2DArray`의 `SourceTextures`·`VolumeTexture`의 `Source2DTexture` 같은 에셋 참조는 의존으로 기록한다.

### 4.5 구현에서 정한 점(UX2a)

UX2a(UE 쪽 텍스처 추출, `FRocksTextureExtractor`) 중 4절이 정하지 않았거나 다르게 정한 점이다. 모두 권장안으로 확정했다.

1. **`data` 스키마.** `data`는 `{ "source": … }` 하나다. `source`는 다음 키를 이 순서로 갖는다.

   | 키 | 내용 |
   |---|---|
   | `format` | 층 0 원본 형식(4.2절 표 왼쪽 이름에서 `TSF_`를 뗀 것: `G8`·`BGRA8`·`BGRE8`·`RGBA16`·`RGBA16F`·`G16`·`RGBA32F`·`R16F`·`R32F`) |
   | `compression` | 원본 보관 형식(`None`·`PNG`·`JPEG`·`UEJPEG`·`UEDELTA` — `TSCF_`를 뗀 것). 정보용이다. 픽셀은 언제나 푼 값이다 |
   | `lossyOrigin` | `JPEG`·`UEJPEG`이면 `"jpeg"`, 아니면 `null`(4.3절) |
   | `longLat` | `IsLongLatCubemap()`(4.1절) |
   | `numLayers`·`numBlocks` | 층·블록 수(4.1절 — 둘 이상이면 Rocks 임포트가 `UnsupportedSourceLayout`) |
   | `layers` | 층마다 `{ "format", "gammaSpace" }`(`gammaSpace`는 `Linear`·`Pow22`·`sRGB` — 4.2절 `GetGammaSpace(Layer)`) |
   | `blocks` | 블록마다 `{ "blockX", "blockY", "sizeX", "sizeY", "numSlices", "numMips", "mips": [ { "sizeX", "sizeY", "numSlices", "pixels": [층마다 blob 참조] } ] }` |

   mip 크기는 `FTextureSource::FMipData::GetMipDataWithInfo`의 `FImageInfo`를 그대로 적는다(볼륨은 mip마다 깊이가 줄고 배열·큐브는 slice 수가 그대로다 — `GetMippedNumSlices`). 원본이 없으면(`Source.IsValid()` 거짓) `source`는 `null`이고 error 진단 `NoSourceData`다(4.1절). 층 형식이 위 9종이 아니면 `null`과 `UnsupportedSourceFormat`, mip 사슬을 풀지 못하면 `null`과 `SourceReadFailed`다(모두 error, 에셋 수준 — 내보내기는 계속한다).
2. **blob 배치.** mip 하나·층 하나 = blob 하나다(4.1절 (block, layer, mip)). 바이트는 `GetMipData(nullptr)`로 한 번 푼 사슬의 그 mip view를 그대로 쓴다(slice 순서, 행 우선, 패딩 없음). 배치는 `G8` u8×1, `BGRA8`·`BGRE8` u8×4(UE 저장 순서 B·G·R·A/E 그대로 — 교환·해제는 Rocks 임포트, 4.2절), `RGBA16` u16×4, `RGBA16F` f16×4, `G16` u16×1, `RGBA32F` f32×4, `R16F` f16×1, `R32F` f32×1이고 `count`는 texel 수(sizeX × sizeY × numSlices)다.
3. **`settings`.** 텍스처 객체의 UPROPERTY 덤프 전체다(4.4절, 11.5·11.7절 13번 규칙). `Source` 프로퍼티(FTextureSource의 UPROPERTY — 크기·형식·블록 표·Id)는 `data`와 겹치므로 뺀다. 덤프가 모은 참조(`CompositeTexture`·`SourceTextures`·`Source2DTexture` 등)는 의존 목록에 합친다(11.7절 4번). enum 값은 UE C++ 값 이름 그대로다(`TA_Wrap`·`TC_Default`·`TEXTUREGROUP_World` — `GetNameStringByValue`). 접두를 떼는 것은 Rocks 임포트의 대조표(AssetSystem 12.4절 — Rocks 이름은 접두를 뗀다)가 한다.
4. **손실 원본.** 플러그인은 거부하지 않는다. 푼 픽셀을 쓰고 `lossyOrigin`과 info 진단 `LossyOrigin`을 남긴다. 받는 판단은 Rocks 임포트(UX2b)가 한다. UE 5.8 엔진 콘텐츠에도 있다(`/Engine/EngineMaterials/T_Grid_Crosses` G8 JPEG, `/Engine/EditorMaterials/Dataflow/T_CheckerBoard` BGRA8 JPEG).
5. **로드.** 범위 안 텍스처는 본문을 위해 로드한다(`FSoftObjectPath::TryLoad`). 로드하지 못하면 error 진단 `LoadFailed`이고 본문은 빈 객체다. blob 쓰기 실패(디스크·크기 불일치)는 내보내기 전체 실패다.
6. **본문 조립.** 의존·진단이 본문 추출 결과에 따라 바뀌므로 본문(`settings`·`data`)을 먼저 조각으로 만들고(`FRocksJsonWriter` 기준 깊이 1, `FinishFragment`) 에셋 JSON에 그대로 끼운다(`RawValue`). 그래서 bytes는 한 writer로 쓴 것과 같다.
7. **자기 시험.** `RocksExport.TextureExtraction`(commandlet `-SelfTest`에도 포함)이 엔진 텍스처 8개(BGRA8 PNG, G8 JPEG, RGBA16F, BGRE8 UE Delta 큐브, BGRA8 큐브·볼륨·2D 배열, R32F 볼륨)를 내보내고, blob마다 `FTextureSource::GetMipData(Out, Block, Layer, Mip)`(LockMip을 거치는 다른 경로)로 꺼낸 원본과 SHA-256·`bytes`·blob 파일 내용·`count`·`layout`을 대조한다. `settings`에 `Source`가 없고 `SRGB`·`CompressionSettings`가 있는지, `format`·`lossyOrigin`·`longLat`·블록·층 수도 본다. 결정성은 `ExportDeterminism`(정육면체 닫힘에 텍스처 2개가 든다)이 본다. 수동 검증(2026-11-22): 엔진 텍스처 15개와 시험 프로젝트에 임포트한 16비트 PNG 2개(G16·RGBA16 — 엔진 콘텐츠에 없다)를 닫힘과 함께 내보낸 묶음(에셋 20개, 원본 형식 G8·BGRA8·BGRE8·G16·RGBA16·RGBA16F·RGBA32F·R32F, 보관 형식 None·PNG·JPEG·UEDELTA, 2D·큐브(위도경도 포함)·2D 배열·볼륨)을 두 번 내보내 `createdUtc` 말고 bytes가 같고, Rocks 리더(`UnrealExchange_ExternalBundle`)가 blob 전부의 크기·SHA-256을 확인해 20개 모두 Ready였다. R16F 원본과 TextureCubeArray는 엔진 콘텐츠에 없어 확인하지 못했다.

8. **mip 크기 불일치(개정 15).** 블록·mip·층마다 `GetMipDataWithInfo` 결과가 있고 크기가 그 mip 정보(`GetImageSizeBytes`)와 같은지 blob을 쓰기 전에 모두 본다. 하나라도 어긋나면 그 에셋의 `data.source`는 `null`이고 error `SourceReadFailed`다(전에는 내보내기 전체가 실패했다). 묶음의 다른 에셋은 영향을 받지 않는다.

### 4.6 Rocks 임포트(UX2b)

설계일 2026-11-26. 공통 틀은 10.6절이다. UX2b1은 10.6절 틀과 4.6절 2·3번 순수 변환(원본·설정)과 그 시험, UX2b2는 A2f 임포터·종류별 임포터·제품 연결과 그 시험이다. 모두 권장안으로 정했다.

1. **임포터.** 교환 클래스마다 A2f 임포터 하나다(주 출력 타입이 고정이라 — A2f 등록 규칙). 확장자 기본은 없고(이름으로만 고른다), 고정 역할 `asset`, 가변 역할 접두 `blob:`, 옵션 `UnrealExchangeImportOptions`, 버전 1이다.

   | 교환 클래스 | 이름 | 출력 | id |
   |---|---|---|---|
   | Texture2D | `unreal_texture_2d` | `Texture2D` | `337d13db-1df4-4802-ba47-55da35a5b39d` |
   | TextureCube | `unreal_texture_cube` | `TextureCube` | `93f7e0e1-1176-4305-a63e-6497472c9260` |
   | Texture2DArray | `unreal_texture_2d_array` | `Texture2DArray` | `12ee1fb9-5e52-41a5-9405-dfe8d9406ca1` |
   | TextureCubeArray | `unreal_texture_cube_array` | `TextureCubeArray` | `a49f013e-97bc-4fe4-b46d-fb6b0f126a37` |
   | VolumeTexture | `unreal_volume_texture` | `VolumeTexture` | `babb45b2-7cbe-49c3-ac8a-9bad95195d23` |

   종류별 임포터 하나(`ExchangeTextureKind`)가 다섯 클래스에 등록되고 클래스로 임포터 이름을 고른다.
2. **원본 변환(`ConvertExchangeTextureSource`).** `data.source`(4.5절 1번)에서 Rocks `TextureSource` 값과 픽셀 byte를 만든다. 검사 순서와 실패(코드 / 오류):
   1. `source`가 null(플러그인 오류 진단이 있어 문서 읽기가 이미 거부한다)·형식 위반 → `CorruptBundle` / `InvalidArgument`.
   2. `numLayers` ≠ 1, `numBlocks` ≠ 1, `longLat` 참 → `UnsupportedSourceLayout` / `Unsupported`(4.1절 — 다층·UDIM·위도경도 큐브).
   3. 층 0의 `format`이 `source.format`과 같고 4.2절 9종 가운데 하나다 → 아니면 `CorruptBundle`.
   4. 블록 0의 `blockX`·`blockY`가 0이고 `mips`가 `numMips`개다. 첫 mip만 쓴다(Rocks 원본 mip은 1 — AssetSystem 12.4.4절 계약. 원본 mip을 쓰는 `LeaveExistingMips`는 계약 설정이라 설정 판정이 알린다). 첫 mip의 크기가 블록 크기와 같다 → 아니면 `CorruptBundle`.
   5. slice 수가 클래스와 맞다: Texture2D 1, TextureCube 6, Texture2DArray 1 이상, TextureCubeArray 6의 배수(6 이상), VolumeTexture 1 이상 → 아니면 `InvalidSource` / `InvalidArgument`. 크기 상한은 파일 임포트와 같다(`image::ImageDecodeLimits` 기본 — 넘으면 `InvalidSource` / `CapacityExceeded`).
   6. 첫 mip `pixels[0]` blob의 배치가 형식과 맞다(4.5절 2번 — `count` = sizeX × sizeY × numSlices) → 아니면 `CorruptBundle`.
   7. 형식 대응(4.2절 표): G8·RGBA16·RGBA16F·G16·RGBA32F·R16F·R32F는 byte 그대로, BGRA8 → RGBA8(texel마다 B·R 교환), BGRE8 → RGBA32F(UE `FColor::FromRGBE` — 지수 0이면 (0, 0, 0, 1), 아니면 성분마다 (값 + 0.5) · 2^(e − 136), 알파 1, float로 계산).
3. **설정 표(10.6절 8번 — `ExchangeTextureSettingRows`).** 행 번호는 표 순서(0부터)이고 `UnsupportedSetting` detail이다. UE 기본값은 UE 5.8 생성자 값이다(Texture.cpp 148행 — Rocks 기본값과 같다, AssetSystem 12.8절).

   | 행 | UE 키 | 처리 | 비고 |
   |---|---|---|---|
   | 0–7 | `AdjustBrightness`·`AdjustBrightnessCurve`·`AdjustVibrance`·`AdjustSaturation`·`AdjustRGBCurve`·`AdjustHue`·`AdjustMinAlpha`·`AdjustMaxAlpha` | Same | 계약 설정 — 판정은 AssetSystem `CheckTextureSettings` |
   | 8–10 | `CompressionNoAlpha`·`CompressionForceAlpha`·`CompressionNone` | Same | |
   | 11 | `LossyCompressionAmount` | Enum | |
   | 12 | `OodleTextureSdkVersion` | Same | Name |
   | 13 | `MaxTextureSize` | Same | |
   | 14 | `CompressionQuality` | Enum | |
   | 15 | `CompressionCacheId` | Guid | |
   | 16 | `bDoScaleMipsForAlphaCoverage` | Same | |
   | 17 | `AlphaCoverageThresholds` | Same | `{X,Y,Z,W}` |
   | 18–20 | `bUseNewMipFilter`·`bPreserveBorder`·`bFlipGreenChannel` | Same | |
   | 21 | `PowerOfTwoMode` | Enum | |
   | 22 | `PaddingColor` | Same | `{B,G,R,A}` |
   | 23–25 | `bPadWithBorderColor`·`ResizeDuringBuildX`·`ResizeDuringBuildY` | Same | |
   | 26–28 | `bChromaKeyTexture`·`ChromaKeyThreshold`·`ChromaKeyColor` | Same | |
   | 29 | `MipGenSettings` | Enum | |
   | 30 | `CompositeTexture` | Reference | |
   | 31 | `CompositeTextureMode` | Enum | |
   | 32 | `CompositePower` | Same | |
   | 33 | `LayerFormatSettings` | Struct 배열(멤버 `CompressionSettings` Enum, 나머지 Same) | 계약 |
   | 34 | `LODBias` | Same | |
   | 35 | `CompressionSettings` | Enum | |
   | 36 | `Filter` | Enum | |
   | 37 | `MipLoadOptions` | Enum | |
   | 38 | `VirtualTextureStreamingPriority` | Enum | |
   | 39 | `VirtualTexturePrefetchMips` | Same | |
   | 40 | `CookPlatformTilingSettings` | Enum | |
   | 41 | `bOodlePreserveExtremes` | Same | |
   | 42 | `LODGroup` | Enum | |
   | 43 | `Downscale` | PerPlatformFloat | |
   | 44 | `DownscaleOptions` | Enum | |
   | 45 | `Availability` | Enum | |
   | 46–48 | `SRGB`·`bNormalizeNormals`·`bUseLegacyGamma` | Same | |
   | 49 | `SourceColorSettings` | Struct(`EncodingOverride`·`ColorSpace`·`ChromaticAdaptationMethod` Enum, 좌표 Same) | |
   | 50–53 | `bNoTiling`·`VirtualTextureStreaming`·`bUseVirtualTextureStreamingPriority`·`CompressionYCoCg` | Same | |
   | 54–56 | `NumCinematicMipLevels`·`NeverStream`·`bGlobalForceMipLevelsToBeResident` | Same | UStreamableRenderAsset |
   | 57–59 | `AddressX`·`AddressY`·`AddressZ` | Enum | 차원 클래스(없는 클래스에서는 `ImportJson`이 모르는 키로 거부하므로 그 클래스 출력이 아니면 버린다 — 행에 클래스 집합) |
   | 60 | `AddressMode` | Enum | VolumeTexture |
   | 61 | `SourceTextures` | ReferenceArray | Texture2DArray·TextureCubeArray |
   | 62 | `Source2DTexture` | Reference | VolumeTexture |
   | 63–64 | `Source2DTileSizeX`·`Source2DTileSizeY` | Same | VolumeTexture |
   | 65 | `AssetUserData` | Unsupported(기본 `[]`) | 사용자 데이터 객체 — 후속 |
   | 66 | `NoRefStreamingLODBias` | Unsupported(기본 `{"Default": -1, "PerQuality": []}`) | UE 품질 단계별 스트리밍 편향 — 후속 |
   | 67 | `CPUCopyTexture` | Unsupported(기본 null) | UE 5.8 CPU 사본 텍스처 — Rocks는 `Availability`로 한다 |
   | 68–72 | `LightingGuid`·`AssetImportData`·`ImportedSize`·`FirstResourceMemMip`·`bHasBeenPaintedInEditor` | Ignore | 설정 아님(조명 식별·임포트 기록·표시용·런타임·에디터 상태) |

   Rocks 확장 `BorderColor`(UE에 없다)는 기본값으로 둔다. 표는 UE 5.8 엔진 콘텐츠를 내보낸 묶음(4.5절 7번 수동 검증의 20개)이 내는 키 전부다.
4. **감마 대조(4.2절).** 층 0 `gammaSpace`의 기대 설정: `Linear` → `SRGB` 거짓, `sRGB` → `SRGB` 참·`bUseLegacyGamma` 거짓, `Pow22` → `SRGB` 참·`bUseLegacyGamma` 참. 번역한 설정과 다르면 `GammaMismatch` 경고(detail = 층 `gammaSpace`의 UE `EGammaSpace` 번호 — `Linear` 0, `Pow22` 1, `sRGB` 2)를 내고 설정을 따른다(UE `GetGammaSpace`도 설정에서 계산한다 — 다르게 나오는 것은 16비트·float 원본에서 `SRGB`가 켜진 경우처럼 UE가 감마를 무시하는 때다).
5. **쓰기.** 출력(빈 key, 임포터 클래스)을 만들고 픽셀을 `AddBulk`, `source`(format·크기·slice·`numMips` 1·`numLayers` 1·bulk)를 쓴 뒤 설정을 `ImportJson`·게시한다. 파일 임포트의 새 임포트 기본값(UE `ApplyDefaultsForNewlyImportedTextures`)은 적용하지 않는다(UE 설정을 그대로 가져온다). 설정 정규화 hook(AssetSystem 12.12절)은 서비스가 다른 임포트와 같이 부른다. 손실 원본(4.3절)은 받는다(경고 없음).
6. **시험(UX2b1).** 원본 변환: 9종 형식 각각(BGRA 교환·BGRE 해제의 UE 식 대조, 지수 0), 층·블록·위도경도 거부, slice 수 규칙, 배치 불일치. 설정 번역: 행 처리 종류마다 양성·음성(enum 접두, Guid 순서, PerPlatform 비지 않음, Unsupported 기본값·비기본값, 모르는 키, 클래스 밖 주소 키 버림, 참조 null·배열 null 원소), 엔진 콘텐츠 묶음 설정 20개(`Tex1` — fixture로 복사)가 경고 없이(기본값) 번역되고 `ImportJson`을 통과. 공통: `ReadExchangeAssetDocument`가 묶음 리더와 같은 진단(기존 시험 그대로 통과 + byte 입력 시험), `VerifyExchangeBlob`, `ExternalTarget`.
7. **시험(UX2b2).** 서비스 위 임포트: 클래스 다섯의 새 임포트(원본·설정·태그·참조), 재임포트(id 유지·blob 집합 바뀜·FormerPaths 누적), 일반 `asset-reimport`가 같은 결과, 검사 순서 6번의 실패 하나하나, 감마 경고, 묶음 밖 기존 에셋 참조(`ExternalTarget`). 명령: `asset-import-unreal`이 fixture 묶음의 텍스처를 만들고 다시 실행하면 재임포트. 제품 실행 파일 C# 확인(`Rocks.Commandlet.Tests`).
