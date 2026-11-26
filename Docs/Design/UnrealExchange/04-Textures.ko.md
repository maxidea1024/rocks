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
