## 12. 텍스처

### 12.1 타입 구성

- 계층은 UE 5.8과 같다: `Asset` → `StreamableRenderAsset`(추상) → `Texture`(공통 설정, 12.4.1절) → 차원별 `Texture2D`·`TextureCube`·`Texture2DArray`·`TextureCubeArray`·`VolumeTexture`(UE 5.8과 같은 다섯 클래스, 고유 설정은 12.4.2절).
- `StreamableRenderAsset`(UE `UStreamableRenderAsset`, Engine/Classes/Engine/StreamableRenderAsset.h:38): 텍스처와 뒤에 올 메시(UE `UStaticMesh`·`USkinnedAsset` — StaticMesh.h:602, SkinnedAsset.h:46)가 함께 쓰는 LOD·mip 스트리밍 기반이다. 메시가 나중에 같은 기반을 쓸 때 필드를 옮기지 않도록(필드 ID·직렬화가 바뀐다) 텍스처 단계에서 미리 나눈다(2026-11-17 사용자 결정).
  - 위치: AssetRuntime. 그래픽 의존 없는 데이터 기반이라 AssetRuntime의 의존 방향(3절)을 바꾸지 않는다.
  - 저장 설정: `NeverStream`, `bGlobalForceMipLevelsToBeResident`, `NumCinematicMipLevels`(UE StreamableRenderAsset.h:342–374, 상태는 12.4.1절 표). UE는 앞의 둘을 `uint8 : 1` bitfield로 두며, Rocks는 리플렉션 B9m(bitfield bool) 뒤라 같은 모양으로 둘 수 있다.
  - 실행 중 상태(UE는 모두 이 객체의 transient 멤버 — 상주·요청 LOD 수 `CachedSRRState`, `StreamingIndex`(duplicatetransient), 강제 상주 표시·시각, 시네마틱 mip 사용, 갱신 대기 표시, mip 변경 콜백, StreamableRenderAsset.h:300–402): 객체에는 게임 lane만 읽고 쓰는 transient **스트리밍 표 handle** 하나만 둔다(UE `StreamingIndex`). 나머지(상주 mip 수, 진행 중 전송·요청 세대, 갱신 대기, 강제 상주·시네마틱 mip 사용 판정, mip 변경 통지 구독)는 TextureStreaming 모듈의 표에 둔다([TextureStreaming](../TextureStreaming.ko.md) 9절, 개정 3). 렌더 lane이 바꾸는 값을 게임 lane 객체에 쓰지 않기 위해서다(RenderBase 5절 lane 소유). 게임 쪽이 상주 mip 수를 읽을 때는 sync 지점에서 복사한 snapshot을 읽는다. 저장 설정 세 개는 상주 계획이 메타로 읽는다. Graphics 타입은 이 클래스에 들이지 않는다.
- 에디터 데이터 `TextureSource`(`ROCKS_STRUCT`, `ROCKS_PROPERTY(EditorOnly)` 필드): 형식·크기·slice 수·mip 수·layer 수·블록 목록·`BulkDataRef`(Zstd, raw SHA-256). 리플렉션으로 저장하므로 네이티브 직렬화 경로가 없다(5.6절). **원본 저장 형식은 디코딩한 정규 픽셀**이다(원본 파일 바이트는 저장하지 않는다 — UE는 PNG·JPEG 바이트를 그대로 둘 수 있다, 12.4.4절). 근거: 재임포트 없이 재빌드, 디코더 버전과 무관한 동일 입력, 빌드 경로에 원본 포맷 코드가 없음. 원본 파일 SHA-256은 ImportRecord에 있다.
- 플랫폼 데이터 `TexturePlatformData`(`ROCKS_STRUCT`, 쿠킹 데이터): dimension·format·sRGB·**swizzle**(출력 R·G·B·A 각각이 형식의 R·G·B·A·0·1 중 무엇인지 — 렌더러가 view swizzle로 적용, A0-G)·크기·mip 수·slice 수·mip별 `{ 크기(X·Y·Z), 바이트 크기(패딩 제외), 위치 }`(위치는 스트리밍 mip이면 그 mip의 덩어리 `BulkDataRef`, 상주 꼬리 mip이면 꼬리 덩어리 안 오프셋 — 한 mip의 모든 slice·면·깊이를 함께 담는다, [렌더 기반](../RenderBase.ko.md) 6.5절 mip-major 배치)·상주 꼬리 덩어리 `BulkDataRef`·**상주 꼬리 mip 수**(쿠킹 때 계산, 12.5.1절 — 렌더는 다시 계산하지 않는다)·**샘플러**(텍스처·LOD 그룹에서 정한 Filter·MaxAniso·축별 주소·Border 색 — 렌더러가 그대로 쓴다).
- Texture2DArray·TextureCubeArray·VolumeTexture는 다른 에셋을 원본으로 묶을 수 있다(UE 5.8 `SourceTextures`·`Source2DTexture` — Build 의존, 16.2절). 원본 파일 여러 개로 직접 임포트하는 경로도 있다(12.3절).
- UE의 다른 텍스처 계열(렌더 타깃 에셋 `TextureRenderTarget*`, IES `TextureLightProfile`, `RuntimeVirtualTexture`, 미디어·동적 텍스처)은 후속이다(18절).

### 12.2 원본 형식

| 원본 | TextureSource |
|---|---|
| PNG 회색 1·2·4비트 | G8(비트 복제로 확장: v × 255 / (2^b − 1)) |
| PNG·PSD 회색 8비트, TGA 회색(형식 3·11) 8비트 | G8 |
| PNG·PSD 회색 16비트 | G16 |
| PNG·PSD 회색+알파 8비트, PNG 회색 1–8비트 + tRNS | GA8(tRNS: 지정 값과 같은 texel은 알파 0, 나머지 255) |
| PNG·PSD 회색+알파 16비트, PNG 회색 16비트 + tRNS | GA16 |
| PNG 팔레트 1·2·4·8비트(PLTE·tRNS), BMP 1·4·8비트 색표(BI_RGB·BI_RLE4·BI_RLE8), TGA 색표(형식 1·9), PSD Indexed | RGBA8(tRNS에 없는 항목은 알파 255) |
| PSD Bitmap(1비트) | G8(0 → 255 흰색, 1 → 0 검정 — PSD 규약) |
| PNG 색 8비트(RGB·RGBA·RGB+tRNS), PSD RGB 8비트, BMP 16·24·32비트, TGA 15·16·24·32비트(형식 2·10) | RGBA8(5·6비트 성분은 비트 복제로 확장) |
| PNG 색 16비트(RGB·RGBA·RGB+tRNS), PSD RGB 16비트 | RGBA16 |
| PSD 32비트 float RGB·RGBA | RGBA32F |
| PSD 32비트 float 회색, 회색+알파 | R32F, RGBA32F(회색을 r = g = b로) |
| Radiance HDR(RGBE) | RGBA32F(RGBE를 float로 — 정밀도 손실 없음) |
| 무손실 OpenEXR(압축 없음·RLE·ZIPS·ZIP·PIZ, half·float·uint 채널, scanline·tile) | 채널 수·정밀도에 따라 R16F·RGBA16F·R32F·RGBA32F |
| DDS 비압축 DXGI 형식(R8·R8G8·RGBA8·BGRA8·BGRX8·R16·RGBA16·R16F·RGBA16F·R32F·RGBA32F, sRGB 변형, 레거시 헤더의 같은 형식) | 같은 정밀도의 TextureSource 형식. 2D·Cube·2DArray·Volume·CubeArray와 원본 mip 체인을 그대로 받는다 |
| TIFF 무손실(압축 없음·LZW·Deflate·PackBits, 8·16비트 정수, 32비트 float, 회색·RGB·알파·팔레트, strip·tile) | G8·G16·GA8·GA16·RGBA8·RGBA16·R32F·RGBA32F(PNG·PSD와 같은 대응) |
| JPEG(JFIF·Adobe·EXIF 컨테이너, baseline·progressive, 8비트 정밀도, 회색 1성분·YCbCr/RGB 3성분) | 회색 G8, 색 RGBA8(알파 255, sRGB). 임포트 때 libjpeg-turbo로 풀어 정규 픽셀로 저장한다(12.1절 원칙). CMYK·YCCK(4성분)·12비트·lossless JPEG는 `UnsupportedSourceFormat`. EXIF 회전 정보는 적용하지 않는다(UE 5.8 ImageWrapper JPEG와 같다) |
| 손실 WebP, 손실 압축 EXR(B44·B44A·DWAA·DWAB, 32비트 float 채널의 PXR24), JPEG 압축 TIFF, 블록 압축 DDS(BC1–7 등) | `LossySource`로 거부(사용자 결정: JPEG 밖의 손실 원본은 받지 않는다 — 2026-11-23 개정). 블록 압축 DDS는 UE 5.8도 원본으로 받지 않는다 — DDS 파서는 BC를 인식하지만 원본 형식 대응표(`DXGIRawFormatMap`, `DXGIFormatGetClosestRawFormat`)에 없어 "DXGIFormat not supported"로 거부(ImageCore/Private/DDSFile.cpp 1289·1350, UnrealEd/Private/Factories/EditorFactories.cpp `UTextureFactory::ImportDDS` 3289·3320–3325) |

2026-11-13 사용자 결정: DDS(비압축)·TIFF(무손실)·Radiance HDR·무손실 OpenEXR 디코더를 A5 범위에 넣는다(HDR·EXR은 처음에 "후속"이었다). PCX는 UE가 받지만 쓰이지 않으므로 지원하지 않는다. IES(조명 프로파일)는 텍스처 원본이 아니라 별도 에셋 종류라 조명 단계, UDIM은 가상 텍스처(12.4절 계약)와 함께 다룬다. 블록 압축(BC·ASTC·ETC)은 원본이 아니라 쿠킹 결과 형식이며 텍스처 파이프라인의 압축 단계(화면 마일스톤 뒤, 사용자 결정 순서)에서 인코더와 함께 구현한다.

2026-11-23 사용자 결정(2026-11-20 결정을 대체): JPEG 원본을 UE처럼 받는다. UE 5.8은 JPEG를 임포트할 때 기본 설정(`[TextureImporter] RetainJpegFormat` = True — BaseEditor.ini)에서 JPEG 바이트를 원본으로 보관하고 빌드 때 푼다(EditorFactories.cpp 2960–3006). Rocks는 받는 범위를 UE와 같게 하되 저장은 12.1절 원칙대로 **임포트 때 풀어 정규 픽셀로** 둔다 — 근거: 디코더 버전과 무관한 동일 빌드 입력, 빌드·쿠킹 경로에 원본 형식 코드가 없음(결정성 — 10절). 대가는 원본 크기(JPEG보다 크다 — Zstd로 줄인다)이고, JPEG 바이트 보관(UE `RetainJpegFormat` 대응 — 빌드가 결정적 정수 IDCT로 푼다)은 크기가 문제가 될 때의 후속 선택이다. 디코더는 외부 라이브러리 libjpeg-turbo다(ThirdParty, 버전 고정 — UE ImageWrapper와 같은 구현. 사용자 결정 2026-11-23: 구현 난이도가 큰 것은 외부 라이브러리, 간단한 것은 자체 구현 — progressive·Huffman·IDCT·업샘플링·마커 변형을 직접 만들고 검증하는 비용이 크다). 정수 IDCT `JDCT_ISLOW`, SIMD 없이 C 경로로 빌드한다(NASM 빌드 의존을 두지 않는다 — 임포트 때 한 번 푸는 용도라 속도보다 빌드 단순성, 결과는 정수 산술이라 호스트와 무관하다). ImageCore가 libjpeg-turbo 호출을 감싸 다른 원본 디코더와 같은 결과 형식(G8·RGBA8)·진단(`InvalidSource`·`UnsupportedSourceFormat`)으로 낸다. 다른 원본 디코더(BMP·TGA·PNG·PSD·DDS·TIFF·HDR·EXR)는 자체 구현 그대로다(zlib 등 범용 압축은 Core Compression). [Unreal 교환 형식](../UnrealExchange.ko.md)(4.3절)으로 가져오는 JPEG·UEJPEG 원본 텍스처도 옵션 없이 받는다(해제한 픽셀 — `--allow-lossy-origin`·`LossyOriginAccepted`는 없앴다).

원본별 규칙:

- BMP: 16비트 BI_RGB는 555(최상위 비트 무시), 32비트 BI_RGB는 4번째 byte가 예약값이므로 알파 255로 읽는다. BI_BITFIELDS·BI_ALPHABITFIELDS는 마스크를 따른다. 40 byte 헤더는 마스크(BITFIELDS 3개·ALPHABITFIELDS 4개)가 헤더 뒤에 온다(Adobe 변형 — UE BmpImageWrapper.cpp 325행도 받는다). 알파 마스크는 V4·V5 헤더(108·124)의 알파 마스크가 0이 아닐 때와 40 byte 헤더의 BI_ALPHABITFIELDS일 때만 쓰고, 56 byte(Adobe V3) 헤더의 알파 마스크는 무시한다(UE 391행과 같다). BI_RGB에서는 V5의 알파 마스크도 무시한다. 색표 범위 밖 index(biClrUsed 뒤)는 불투명 검정(0,0,0,255)이다(UE 196–222행과 같다 — 오류가 아니다). 마스크는 연속 비트여야 하고 서로 겹치지 않아야 하며(아니면 `InvalidSource`), 폭 w의 성분은 v × 255 / (2^w − 1)을 round-half-up해 8비트로 만든다(w > 8도 RGBA8로 줄인다). BI_JPEG·BI_PNG는 `UnsupportedSourceFormat`.
- TGA: image descriptor의 알파 비트 수(bit 0–3)가 0이면 4번째 성분·16비트의 attribute 비트를 무시한다(알파 255). TGA 2.0 확장 영역의 attributes type이 0–2(알파 없음·미정의·무시)면 무시, 3이면 일반 알파, 4(premultiplied)면 성분을 8비트로 확장한 뒤 straight 알파로 되돌린다(min(255, round-half-up(c × 255 / a)), 알파 0이면 색 0).
- PSD: 색 모드 RGB·Grayscale·Indexed·Bitmap을 받고 CMYK·Lab·Multichannel·Duotone은 `UnsupportedSourceFormat`. 병합(composite) 이미지 데이터를 쓴다 — image resource 0x0421(Version Info)의 hasRealMergedData가 false면(Maximize Compatibility를 끄고 저장) 레이어 합성은 지원하지 않으므로 `UnsupportedSourceFormat`. Indexed는 색표 크기를 resource 0x0416(색 수)으로, 투명 항목을 0x0417(투명 index)로 정한다(그 항목은 알파 0). 알파는 UE와 같다(EditorFactories.cpp PSD 읽기 — 2026-11-24 A5e 교차 검증, 애매하면 UE 원칙): Grayscale 2채널의 둘째, RGB 4채널의 넷째 채널을 늘 알파로 쓴다(Photoshop 병합 의미 — 레이어 수가 음수일 때만 첫 추가 채널이 투명도 — 와 다를 수 있지만 교환 결과를 UE와 같게 한다). 그보다 많은 채널(저장된 선택 영역·spot 채널)은 `IgnoredSourceChannels` 경고와 함께 버린다.
- float 원본(PSD 32비트 등)은 디코딩 직후 NaN을 0으로, ±Inf를 ±float 최대 유한값으로 바꾼다(이후 필터·mip에서 번지지 않게).
- PNG 범위는 15절.

색 해석과 출력 부호화(정수 원본의 색 채널에만 적용, 알파는 항상 선형 값 그대로):

1. 원본 디코딩: `SourceColorSettings.EncodingOverride`가 None이 아니면 그 부호화(구현: `Linear`·`sRGB`, 나머지는 계약)로, None이면 `SRGB = on`일 때 `bUseLegacyGamma ? x^2.2 : sRGB 구간 함수`, off일 때 선형으로 읽는다. float 원본은 EncodingOverride가 없으면 선형이다(SRGB는 출력에만 영향).
2. 모든 처리(크기 조정·mip·필터)는 선형 float에서 한다(12.5절).
3. 출력 부호화(`SRGB = on`이고 4번에서 무시되지 않을 때): 출력 형식이 8비트이면 sRGB 구간 함수로 부호화한다(GPU는 구간 함수로 디코딩하므로 `bUseLegacyGamma`는 원본 디코딩에만 쓰인다). 쿠킹 형식에 sRGB 변형이 있으면(RGBA8) 그것을 쓰고, 없으면(R8·RG8) 12.4.3절의 swizzle을 데이터에 먼저 적용한 뒤 RGBA8 sRGB로 확장하고 swizzle은 항등으로 기록한다(예: GA8 Default → (g, g, g, a), Grayscale → (g, g, g, 1)). 16비트 정수 출력(R16·RG16·RGBA16)은 sRGB 변형이 없으므로 선형화해 같은 채널 수의 16F(R16F·RG16F·RGBA16F)로 저장하고 sRGB 표시를 하지 않는다(16F는 1.0 근처 약 11비트 정밀도로 16비트 원본보다 낮다 — sRGB 16비트 텍스처의 대가로 문서화한다).
4. `SRGB` 값은 저장된 값만으로 해석한다. 다음 경우 빌드는 SRGB를 off로 보고 `SrgbIgnored` 경고를 낸다: CompressionSettings가 선형 데이터 종류(12.4.3절 SRGB 열이 "무시")일 때, float 원본이 float 출력으로 갈 때(부호화할 8비트 출력이 없다). 명령 편의: CompressionSettings를 선형 종류로 바꾸거나 float 원본을 임포트하는 명령은 같은 명령에서 SRGB를 주지 않았으면 SRGB를 off로 함께 저장한다(UE도 선형 종류에서 SRGB를 끈다) — 저장 값이 깨끗하게 유지된다.
5. 양자화: 정수 출력은 [0, 1]로 자른 뒤 round-half-up(unorm8 = floor(v × 255 + 0.5), unorm16은 65535). float 출력: NaN → 0, +Inf·−Inf → 그 형식의 최대·최소 유한값, 16F 범위(±65504)를 넘는 유한값은 ±65504로 자른다. 16F 변환은 round-to-nearest-even. 16비트 정수 → 8비트 출력도 이 규칙이다(선형 또는 디코딩된 float를 거친다).

### 12.3 원본 구성과 지정 문법

| 차원 | 원본 | 명령(13절 문법) |
|---|---|---|
| 2D | 파일 하나 | `asset-import /Game/T/A a.png` |
| Cube | 6면 파일, 또는 한 파일의 배치 | `asset-import /Game/T/Sky --cube=px:a_px.png,nx:a_nx.png,py:a_py.png,ny:a_ny.png,pz:a_pz.png,nz:a_nz.png` / `asset-import /Game/T/Sky cross.png --cube-layout=HorizontalCross` |
| 2DArray | 파일 여러 개(slice 순서), 또는 Texture2D 에셋 목록 | `asset-import /Game/T/Arr --array=a0.png,a1.png` / `asset-new Texture2DArray /Game/T/Arr SourceTextures=/Game/T/A,/Game/T/B` |
| CubeArray | Cube 구성 × n, 또는 TextureCube 에셋 목록 | `asset-import /Game/T/Skies "--cube-array=px:…,nz:…;px:…,nz:…"` / `asset-new TextureCubeArray /Game/T/Skies SourceTextures=/Game/T/S0,/Game/T/S1` |
| Volume | 2D 파일 + 타일 크기, 또는 slice 파일 여러 개, 또는 Texture2D 에셋 + 타일 크기 | `asset-import /Game/T/Vol atlas.png --volume-tile=16x16` / `asset-import /Game/T/Vol --volume=s0.png,s1.png` / `asset-new VolumeTexture /Game/T/Vol Source2DTexture=/Game/T/Atlas Source2DTileSizeX=16 Source2DTileSizeY=16` |

- Cube 면 순서는 D3D 규약(+X −X +Y −Y +Z −Z = `px nx py ny pz nz`)이고 면 이미지의 방향도 D3D 규약을 따른다. 배치: `HorizontalCross`(4×3 칸: (1,0)=+Y, (0,1)=−X, (1,1)=+Z, (2,1)=+X, (3,1)=−Z, (1,2)=−Y), `VerticalCross`(3×4 칸: (1,0)=+Y, (0,1)=−X, (1,1)=+Z, (2,1)=+X, (1,2)=−Y, (1,3)=−Z를 180° 회전해 담음), `Strip6`(가로 6칸, 면 순서대로). 칸 크기가 같지 않거나 비면 `InvalidImportOption`.
- `--cube-array`의 각 cube 지정은 `--cube` 값과 같은 형식이고 `;`로 나눈다(셸 명령 구분자이므로 따옴표로 감싼다). `--cube-layout`·`--volume-tile`은 위치 인자 원본 하나와 함께, `--cube`·`--array`·`--cube-array`·`--volume`은 위치 인자 원본 없이 쓴다.
- equirect(위도·경도) → Cube 변환은 후속(HDR 디코더 자체는 A5 — 12.2절). 서비스 API는 같은 구성을 `TextureImportSpec` 구조로 받는다.

### 12.4 설정(UE 5.8 대조)

대조 근거: UE 5.8(5.8.4, commit `f45729c0b6f0`) 원본의 헤더 — `Engine/Source/Runtime/Engine/Classes/Engine/`의 `Texture.h`(`UTexture`·`FTextureSource`·`FTextureSourceColorSettings`), `StreamableRenderAsset.h`(기반 `UStreamableRenderAsset`의 스트리밍 멤버), `Texture2D.h`·`TextureCube.h`·`Texture2DArray.h`·`TextureCubeArray.h`·`VolumeTexture.h`, `TextureDefines.h`(관련 enum), `TextureLODSettings.h`(`FTextureLODGroup`). 처음에는 UE 5.6 공식 API 문서(C++ `UTexture` 멤버 75개, `FTextureLODGroup` 멤버 29개, Python `unreal.Texture`·`Texture2D`·`TextureCube`·`Texture2DArray`·`TextureCubeArray`·`VolumeTexture` 편집 속성 — 2026-11-12 확인)로 작성했고, 2026-11-15에 5.8 원본으로 다시 확인했다. `UTexture`·`FTextureSource`·차원별 클래스의 데이터 멤버는 5.6과 같다(추가·삭제 없음). 5.8에서 바뀐 점은 해당 행에 "5.8에서 바뀜"으로 적는다: TextureGroup 82종, `ETextureSourceEncoding`의 `HLG` 추가, `FTextureLODGroup`의 품질 등급별 상한 4개 추가(33개). 이 절의 표는 UE C++ 이름으로 적는다. Rocks 필드 이름(명령행 이름도)은 Rocks 규칙대로 camelCase이고 bool에 `b` 접두를 붙이지 않는다(`LODGroup` → `lodGroup`, `bFlipGreenChannel` → `flipGreenChannel`, `SRGB` → `srgb` — 2026-11-24 사용자 결정, 처음엔 UE 이름을 `Name="…"`로 노출했다). UE 교환은 이 대응으로 옮긴다. enum 값 이름은 UE 접두사(`TC_`·`TMGS_`·`TEXTUREGROUP_`·`TF_`·`TA_` 등)를 뗀 나머지 그대로이고 접두가 붙은 이름은 받지 않는다(예: `TC_HDR_Compressed` → `HDR_Compressed` — 2026-11-24 사용자 결정, ToolPrefix 없음). **계약** 설정은 스키마에 있고 기본값이 아니면 `UnsupportedSetting`이며, 사용자가 정한 순서(기반 → 화면 → 압축·보정·스트리밍)의 뒤 단계에서 구현한다.

#### 12.4.1 공통 설정(UTexture·UStreamableRenderAsset)

"(UStreamableRenderAsset)"이 붙은 멤버는 Rocks에서도 `Texture`가 아니라 기반 `StreamableRenderAsset`(12.1절)의 필드다. 나머지는 `Texture` 필드다.

| UE 5.8 멤버 | 상태 |
|---|---|
| `LODGroup`(TextureGroup 82종: World…Project48, 값 0–81. Project01–48은 프로젝트 설정에서 이름을 바꾼다. 5.8에서 바뀜: 5.6은 66종 World…Project32, 값 0–65 — Project33–48 16개가 더해졌다. 상태는 5.6의 66종 기준으로 정한 것이다) | 구현 |
| `SRGB`, `bUseLegacyGamma`(원본을 sRGB 대신 2.2 감마로 해석, 12.2절) | 구현 |
| `CompressionSettings`(편집 가능 15종, 대응은 12.4.3절). 숨김 값 `TC_LQ`(13)·`TC_EncodedReflectionCapture`(14, UE 주석: 더 이상 쓰지 않음) | 구현. 숨김 값은 `InvalidSetting` |
| `CompressionNoAlpha` | 구현. UE 설명대로 비압축 RGBA 출력에는 적용하지 않으므로 지금은 결과를 바꾸지 않는다(블록 압축 단계에서 의미가 생긴다) |
| `CompressionForceAlpha`, `CompressionNone`(UE 내부 설정 — 편집 속성 아님) | 구현(스키마·너겟 키 포함). 비압축 쿠킹은 알파를 자동으로 빼지 않고 모두 비압축이므로 지금은 결과를 바꾸지 않는다 |
| `CompressionCacheId`(Guid — 너겟 키에 넣어 바꾸면 재빌드) | 구현 |
| `MipGenSettings`: `FromTextureGroup`·`SimpleAverage`·`NoMipmaps`·`Unfiltered` | 구현(12.5절) |
| `Filter`(`Nearest`·`Bilinear`·`Trilinear`·`Default` = 그룹 값) | 구현 |
| `LODBias`, `MaxTextureSize`(0 = 형식 최대) | 구현(그룹 값과의 합성은 12.5절) |
| `bFlipGreenChannel`, `bPreserveBorder`, `bNormalizeNormals` | 구현 |
| `PowerOfTwoMode`(`None`·`PadToPowerOfTwo`·`PadToSquarePowerOfTwo`·`StretchToPowerOfTwo`·`StretchToSquarePowerOfTwo`·`ResizeToSpecificResolution`), `PaddingColor`, `bPadWithBorderColor`, `ResizeDuringBuildX`·`ResizeDuringBuildY`(`ResizeToSpecificResolution`일 때만 적용, 0 = 원래 크기) | 구현(12.5절) |
| `NeverStream`, `bGlobalForceMipLevelsToBeResident`(UStreamableRenderAsset), `MipLoadOptions` = `Default`·`AllMips` | 구현. 지금은 모든 mip이 상주하므로 이 값들은 모두 충족된다 |
| `Availability`(`GPU`·`CPU` — CPU는 Texture2D 전용, 픽셀은 CPU에서 읽고 GPU에는 작은 검정 placeholder만 올린다) | 구현(저장·쿠킹). 렌더러 반영은 화면 마일스톤 |
| `CookPlatformTilingSettings`(`FromTextureGroup`·`Tile`·`DoNotTile`), `bNoTiling`(UE 내부) | 구현. 타일링을 지원하는 플랫폼이 없어 모든 값이 비타일 결과다 |
| `SourceColorSettings`: `EncodingOverride` = `None`·`Linear`·`sRGB`, `ColorSpace` = `None`·`sRGB` | 구현(색 공간 변환 없음) |
| `AssetImportData` | ImportRecord로 구현(5.4절) |
| `MipGenSettings`: `Sharpen0`–`Sharpen10`·`Blur1`–`Blur5`·`Angular`(Cube 전용)·`LeaveExistingMips`(원본 mip — 12.4.4절) | 계약 |
| `bUseNewMipFilter`(5.6 공개 문서에는 의미가 "현대적 mip 필터, 끄면 옛 동작"으로만 있었다. 5.8 원본 주석: mip 필터만이 아니라 옛 텍스처 처리 경로와 개선 경로를 고르는 스위치이고, 표시 이름은 "Use Improved Image Processing", 멤버 기본값은 false지만 새 텍스처·재임포트 때 `SetModernSettingsForNewOrChangedTexture`가 켠다 — Texture.h 1376–1382, 5.6도 같다) | 기록(저장·교환만 — 빌드 결과에 영향 없음, 계약 판정 없음). UE가 새 텍스처에 켜므로(아래) 교환 텍스처를 거부하지 않는다. 의미는 보정 단계에서 UE 동작과 대조해 확정한다(2026-11-24 A5d 교차 검증 — 처음엔 "false만 받는다"였다) |
| 보정: `AdjustBrightness`·`AdjustBrightnessCurve`·`AdjustVibrance`·`AdjustSaturation`·`AdjustRGBCurve`·`AdjustHue`·`AdjustMinAlpha`·`AdjustMaxAlpha`, `bChromaKeyTexture`·`ChromaKeyThreshold`·`ChromaKeyColor`, `bDoScaleMipsForAlphaCoverage`·`AlphaCoverageThresholds`, `CompositeTexture`·`CompositeTextureMode`(`Disabled`·`NormalRoughnessToRed/Green/Blue/Alpha`)·`CompositePower`, `Downscale`(플랫폼별 — `OrderedMap<Name, float>`를 담은 struct, 리플렉션 B9f·B9c)·`DownscaleOptions`(`Default`·`Unfiltered`·`SimpleAverage`·`Sharpen0`–`Sharpen10`) | 계약 |
| 색 관리: `SourceColorSettings`의 그 밖 `EncodingOverride`(`ST2084`·`Gamma22`·`BT1886`·`Gamma26`·`Cineon`·`REDLog`·`REDLog3G10`·`SLog1`–`SLog3`·`AlexaV3LogC`·`CanonLog`·`ProTune`·`VLog`. 5.8에서 바뀜: `HLG`(17)가 더해졌다 — 이 표에는 아직 분류하지 않았다), `ColorSpace`(`Rec2020`·`ACESAP0`·`ACESAP1`·`P3DCI`·`P3D65`·`REDWideGamut`·`SonySGamut3`·`SonySGamut3Cine`·`AlexaWideGamut`·`CanonCinemaGamut`·`GoProProtuneNative`·`PanasonicVGamut`·`Custom`), 색도 좌표 4개, `ChromaticAdaptationMethod`(`None`·`Bradford`·`CAT02`) | 계약 |
| 압축: `CompressionYCoCg`, `LayerFormatSettings`(층별 형식 설정, UE 내부), `LossyCompressionAmount`(7종), `CompressionQuality`(ASTC 6종), `bOodlePreserveExtremes` | 계약 |
| `OodleTextureSdkVersion`(UE FName — Name 필드, 리플렉션 B9c. UE가 새 텍스처에 최신 버전을 쓴다) | 기록(Rocks는 Oodle을 쓰지 않는다 — 계약 판정 없음) |
| 스트리밍: `NumCinematicMipLevels`(UStreamableRenderAsset), `MipLoadOptions` = `OnlyFirstMip` | 계약(쿠킹 규칙은 12.5.1절 — 시네마틱 mip은 쿠킹 편향에 넣지 않고 런타임이 뺀다, OnlyFirstMip은 비스트리밍) |
| 가상 텍스처: `VirtualTextureStreaming`, `VirtualTextureStreamingPriority`, `bUseVirtualTextureStreamingPriority`, `VirtualTexturePrefetchMips` | 계약 |
| 설정이 아닌 멤버: `DeferCompression`·`CompressFinal`(에디터 세션 동작), `OverrideRunningPlatformName`(에디터 미리보기), `TextureEditorCustomEncoding`, `bIsImporting`·`bCustomPropertiesImported`·`bNotOfflineProcessed`(임포트 내부 표시), `LightingGuid`·`LevelIndex`(정적 조명·스트리밍 내부), `DidSerializeStreamingMipsForPlatform`·`LoadedMainStreamObjectVersion`(직렬화 내부), `TextureReference`·`ReleaseFence`·`PrivateResource`·`PrivateResourceRenderThread`·`bAsyncResourceReleaseHasBeenStarted`·`KnownCompositeTexture`(런타임 자원), 폐기 멤버 `bDitherMipMapAlpha_DEPRECATED`·`SourceFilePath_DEPRECATED`. `AssetUserData`(Python 편집 속성)는 일반 에셋 사용자 데이터 기능으로 후속(18절) | 대응하지 않음 |

#### 12.4.2 차원별 설정

| 클래스 | UE 5.8 고유 설정 | 상태 |
|---|---|---|
| Texture2D | `AddressX`·`AddressY`(`Wrap`·`Clamp`·`Mirror`) | 구현 |
| TextureCube | 없음(UE 5.8 TextureCube에는 주소 설정이 없다 — 샘플러는 Clamp, TextureCube.cpp 497–502) | 구현 |
| Texture2DArray | `AddressX`·`AddressY`·`AddressZ`, `SourceTextures`(Texture2D 목록) | 구현(A0-G에서 GraphicsTypes 샘플러에 W축 주소 추가) |
| TextureCubeArray | `SourceTextures`(TextureCube 목록) | 구현 |
| VolumeTexture | `AddressMode`(세 축 공통), `Source2DTexture`·`Source2DTileSizeX`·`Source2DTileSizeY` | 구현(같은 A0-G 보강) |
| Rocks 확장 | 주소 `Border` + `BorderColor`(`TransparentBlack`·`OpaqueBlack`·`OpaqueWhite`). UE TextureAddress에는 없다(Wrap·Clamp·Mirror). GraphicsTypes에 구현되어 있다 | 구현 |

#### 12.4.3 CompressionSettings → 쿠킹 형식(비압축)

아래 규칙은 UE 5.6 공개 문서만으로 정했다 — 문서로는 설정별 블록 압축 형식(BC·ASTC)과 단일 채널 설정이 원본의 어느 채널을 쓰는지가 확인되지 않았다. 이번 비압축 쿠킹은 채널 의미와 원본 정밀도를 보존하는 아래 규칙으로 정하고, 블록 압축 단계에서 UE 대응을 확정한다. "빨강"은 원본의 빨강 채널(회색 원본은 회색 값)이다 — UE 주석이 `Alpha`를 "단일 채널, displacement·gray에도 쓸 수 있다"고 하므로(TextureDefines.h 401, 5.8도 같다) 회색·빨강 데이터를 담는 단일 채널로 본다. UE 5.8 원본으로 확인한 UE 동작(5.6도 같다 — 설계 차이는 블록 압축 단계에서 다시 본다): 설정별 쿠킹 형식은 `GetDefaultTextureFormatName`(Texture.cpp 4201)이 정한다. 단일 채널 설정이 읽는 원본 채널은 `Grayscale`·`Alpha`가 R(`bReplicateRed`), `Displacementmap`·`DistanceFieldFont`가 A(`bReplicateAlpha`)다(TextureDerivedData.cpp 876–883, Texture.cpp 4308–4310, enum 표시 이름 "from source R/A" — TextureDefines.h 396–402). `Grayscale`·`Displacementmap`은 원본이 G16일 때만 G16이고 RGBA16 원본도 G8로 낸다(Texture.cpp 4312–4321). 따라서 아래 표의 `Displacementmap`·`DistanceFieldFont` "빨강"과 G16이 아닌 16비트·float 원본의 R16·R16F 출력은 UE와 다르다. 표의 형식은 SRGB off 기준이며 sRGB 출력은 12.2절 3번 규칙을 따른다.

| CompressionSettings(값) | 데이터 | 8비트 정수 원본 | 16비트 정수 원본 | float 원본 | swizzle | SRGB |
|---|---|---|---|---|---|---|
| `Default`(0) | 색·알파 | G8 → R8, GA8 → RG8, RGBA8 → RGBA8 | G16 → R16, GA16 → RG16, RGBA16 → RGBA16 | 같은 채널 수의 16F | R: rrr1, RG: rrrg, RGBA: 항등 | 설정값 |
| `Normalmap`(1) | 법선 xyz | RGBA8 | RGBA16 | RGBA16F | 항등 | 무시 |
| `Masks`(2) | 선형 채널 | RGBA8 | RGBA16 | RGBA16F | 항등 | 무시 |
| `Grayscale`(3) | 빨강 | R8 | R16 | R16F | rrr1 | 설정값 |
| `Displacementmap`(4) | 빨강 | R8 | R16 | R16F | 항등(r001) | 무시 |
| `VectorDisplacementmap`(5) | RGBA | RGBA8 | RGBA16 | RGBA16F | 항등 | 무시 |
| `HDR`(6) | RGBA | RGBA16F | RGBA16F | RGBA16F | 항등 | 무시 |
| `EditorIcon`(7, UI — 5.8에서 바뀜: 표시 이름이 5.6 "UserInterface2D (RGBA8)"에서 "Uncompressed (RGBA8)"로) | RGBA | RGBA8 | RGBA8 | RGBA8 | 항등 | 설정값 |
| `Alpha`(8) | 빨강 | R8 | R16 | R16F | 항등(r001) | 무시 |
| `DistanceFieldFont`(9) | 빨강 | R8 | R8 | R8 | 항등(r001) | 무시 |
| `HDR_Compressed`(10) | RGB | RGBA16F | RGBA16F | RGBA16F | 항등(데이터 알파 1) | 무시 |
| `BC7`(11) | RGBA | RGBA8 | RGBA8 | RGBA8 | 항등 | 설정값 |
| `HalfFloat`(12) | 빨강 | R16F | R16F | R16F | 항등(r001) | 무시 |
| `SingleFloat`(15) | 빨강 | R32F | R32F | R32F | 항등(r001) | 무시 |
| `HDR_F32`(16) | RGBA | RGBA32F | RGBA32F | RGBA32F | 항등 | 무시 |

- 채널 수가 늘면 회색은 r = g = b, 없는 알파는 1이다. 16비트 → 8비트, float → 8비트는 12.2절 5번 양자화를 따른다(float 원본의 8비트 출력에서 `SRGB = on`이면 sRGB로 부호화해 어두운 영역 정밀도를 지킨다).
- 블록 압축 형식이 필요한 설정은 프로파일이 압축을 요구하면 `UnsupportedSetting`이다(12.6절).

#### 12.4.4 TextureSource(UE FTextureSource 대조)

| UE 5.8 FTextureSource | Rocks TextureSource | 상태 |
|---|---|---|
| `Format`(ETextureSourceFormat: G8·BGRA8·BGRE8·RGBA16·RGBA16F·G16·RGBA32F·R16F·R32F) | G8·GA8·G16·GA16·RGBA8·RGBA16·RGBA16F·RGBA32F·R16F·R32F. GA8·GA16은 Rocks 추가(UE는 BGRA8·RGBA16으로 확장), 색 순서는 RGBA, BGRE8(RGBE)은 디코딩 때 float로 | 구현 |
| `SizeX`·`SizeY`·`NumSlices`(Cube 6, CubeArray 6 × n, Volume 깊이, 2DArray 층 수, long-lat Cube 1) | 같음(long-lat 제외) | 구현 |
| `NumMips`(원본 mip — `LeaveExistingMips`) | numMips = 1만 | 계약 |
| `NumLayers`·`LayerFormat`·`LayerColorInfo_LockProtected`(다층 원본) | numLayers = 1만 | 계약 |
| `Blocks`·`BaseBlockX`·`BaseBlockY`·`BlockDataOffsets`(UDIM 다중 블록) | 블록 1개만 | 계약(가상 텍스처와 함께) |
| `bLongLatCubemap` | 없음 | 후속(equirect → Cube 변환 — HDR 디코더는 A5) |
| `CompressionFormat`(None·PNG·JPEG·UEJPEG·UEDELTA) | 정규 픽셀 + Zstd(원본 압축 바이트를 저장하지 않음 — 12.1절 근거. JPEG도 받되 풀어서 저장한다 — UE 기본 `RetainJpegFormat` 보관은 후속 선택, 12.2절 2026-11-23) | 구현(설계 차이) |
| `Id`·`bGuidIsHash` | 콘텐츠 SHA-256(`BulkDataRef`) | 구현 |
| `BulkData`(FEditorBulkData) | `BulkDataRef` → Bulk 섹션 | 구현 |
| `Owner`, `BulkDataLock`·`LockState`·`LockedMipData`·`NumLockedMips`, `TornOffGammaSpace`·`TornOffOwnerName`·`TornOffTextureClass` | 런타임 소유·잠금·분리 상태(데이터 아님) | 대응하지 않음 |

#### 12.4.5 LOD 그룹(TargetProfile 그룹 표, UE 5.8 FTextureLODGroup 대조)

| FTextureLODGroup 멤버 | 상태 |
|---|---|
| `Group`, `MinLODSize`, `MaxLODSize`, `LODBias`, `MaxLODMipCount`, `MipGenSettings`, `MaxAniso`, `HighPriorityLoad`(로드 우선순위 High), `MipLoadOptions` = `Default`·`AllMips`, `CookPlatformTilingDisabled`(타일링 플랫폼 없음) | 구현(합성은 12.5절) |
| `MinMagFilter`·`MipFilter`(FName: min/mag `point`·`linear`·`aniso`, mip `point`·`linear`)와 `Filter`(ETextureSamplerFilter) | 구현. 프로파일은 UE 설정처럼 MinMagFilter·MipFilter로 적고 Filter는 그 조합으로 정한다: `point`(mip 무관) → `Point`, `linear`+`point` → `Bilinear`, `linear`+`linear` → `Trilinear`, `aniso`+`point` → `AnisotropicPoint`, `aniso`+`linear` → `AnisotropicLinear`. Filter를 함께 적었는데 조합과 다르면 `InvalidSetting`, Filter만 적으면 MinMagFilter·MipFilter는 그 역대응으로 정한다(`Point` → point·point). AnisotropicPoint는 A0-G에서 GraphicsTypes 필터에 추가한다. 프로파일 검증: MinLODSize > MaxLODSize면 `InvalidSetting` |
| `LODBias_Smaller`·`LODBias_Smallest`·`MaxLODSize_Smaller`·`MaxLODSize_Smallest`(장치 메모리 등급별) | 계약 |
| `NumStreamedMips`, `OptionalLODBias`·`OptionalMaxLODSize`·`DuplicateNonOptionalMips`(선택 mip 패키지) | 계약(스트리밍) |
| `LODBias_VT`·`MaxLODSize_VT`·`VirtualTextureTileCountBias`·`VirtualTextureTileSizeBias`·`VirtualTextureStreamingPriority` | 계약(가상 텍스처) |
| `Downscale`·`DownscaleOptions`, `LossyCompressionAmount` | 계약(보정·압축) |

5.8에서 바뀜: `FTextureLODGroup`에 `MaxLODSize_Low`·`MaxLODSize_Medium`·`MaxLODSize_High`·`MaxLODSize_Epic`(`r.TextureGroup.QualityLevel`·`r.TextureGroup.MaxQualityLevel` 품질 등급별 상한, 가상 텍스처에는 쓰지 않음)이 더해져 멤버가 33개가 되었고(TextureLODSettings.h 76–90), `UTextureLODSettings`에 쿠킹 때 `OptionalMaxLODSize` 대신 품질 등급 상한을 쓰게 하는 `TextureGroupOptionalQualityLevel`이 더해졌다(263–265). 위 표는 5.6의 29개 기준이며 새 멤버는 아직 분류하지 않았다.

**메시 LOD 그룹**(메시 에셋 스펙 MR9, 2026-11-20). 메시도 텍스처처럼 그룹 이름(`lodGroup`)으로 TargetProfile의 기본값을 받는다([MeshAnimationAssets](../MeshAnimationAssets.ko.md) 4.7절). TargetProfile에 텍스처 LOD 그룹 표와 나란히 메시 LOD 그룹 표를 둔다. 항목은 UE 5.8 `FStaticMeshLODGroup`(Engine/Public/StaticMeshResources.h:58)의 멤버에 대응한다: 기본 LOD 수(`DefaultNumLODs`), 스트리밍·선택 LOD 수 상한(`DefaultMaxNumStreamedLODs`·`DefaultMaxNumOptionalLODs`), LOD 스트리밍 지원(`bSupportLODStreaming`), 기본 라이트맵 해상도(`DefaultLightMapResolution`), LOD별 기본 축소 설정(`DefaultSettings[]`)과 축소 설정 보정(`SettingsBias`·`BasePercentTrianglesMult`·`BasePercentVerticesMult`). 화면 크기·최소 LOD는 그룹이 아니라 메시 에셋의 플랫폼별 값이다(텍스처와 같은 덮어쓰기 표). 축소 관련 항목은 축소기가 생길 때까지 보존 상태다(MeshAnimationAssets 4.2절).

### 12.5 빌드 처리와 mip 규칙

적용 순서(한 텍스처의 각 slice·면에 같은 순서):

1. 원본 → 선형 float 디코딩(12.2절 1번). 알파는 그대로.
2. `bFlipGreenChannel`(g → 1 − g).
3. `PowerOfTwoMode`(X·Y 축에만 적용 — Volume은 각 slice의 X·Y, Z는 그대로. Square 변형은 X = Y = 둘 중 큰 값): Pad는 각 축을 n 이상인 가장 작은 2의 거듭제곱으로 늘리고, 늘어난 영역은 `bPadWithBorderColor`면 가장 가까운 테두리 texel, 아니면 `PaddingColor`(원본과 같은 색 해석으로 디코딩한 값)로 채운다. Stretch는 각 축을 가장 가까운 2의 거듭제곱(차이가 같으면 큰 쪽)으로, ResizeToSpecificResolution은 `ResizeDuringBuildX`·`ResizeDuringBuildY`(0이면 원래 크기)로 Lanczos3 재표본한다. 차원 규칙: Cube·CubeArray 면은 정사각형이어야 하고(원본이 아니면 `InvalidSource`) Resize가 정사각형을 깨면 `InvalidSetting`이다. 2DArray·CubeArray·Volume의 slice는 모두 같은 크기·형식이어야 한다(파일 구성이면 `InvalidSource`, 에셋 원본 묶음이면 `InvalidSetting`).
4. 크기 제한과 LOD 합성(쿠킹 때, 그룹 = 프로파일 LOD 그룹, `FromTextureGroup`·`Default`도 그룹 값으로 바뀐다). N = 1×1(×1)까지의 전체 mip 사슬 길이, d = 버릴 상위 mip 수:
   - 하드 상한 d_hard = max(상위 mip의 긴 축(X·Y, Volume은 Z 포함)이 min(MaxTextureSize(0이면 무한), 그룹 MaxLODSize(0이면 무한), 차원 최대(2D·Cube·Array 16384, Volume 2048 — 프로파일 형식 표)) 이하가 되는 최소 수, N − 그룹 MaxLODMipCount(0이면 제한 없음)).
   - 편향 d_bias = 텍스처 LODBias + 그룹 LODBias를 남는 상위 mip의 긴 축(Volume은 Z 포함)이 그룹 MinLODSize 이상이 되는 최대 수로 줄인 값(원본이 MinLODSize보다 작으면 0).
   - d = min(max(d_bias, d_hard), N − 1). 상한이 MinLODSize보다 우선한다.
   - d_bias에는 `NumCinematicMipLevels`를 넣지 않는다(UE와 같다, 12.5.1절 4번). 이 절의 LODBias는 쿠킹 편향이며 런타임 bias·표본 mip bias와 다른 양이다(12.5.1절 5번).
   - `NoMipmaps`면 mip 사슬을 저장하지 않는다. LODBias·MaxLODMipCount는 적용하지 않고 d_hard의 크기 상한만 2배씩 SimpleAverage 축소로 맞춘다.
5. mip 생성(선형 float, 아래 규칙): mip k + 1은 처리를 마친 mip k에서 만들고, 만든 직후 `bPreserveBorder`(Address가 Wrap이면 `PreserveBorderWithWrap` 경고)와 `bNormalizeNormals`를 그 mip에 적용한 결과가 다음 mip의 입력이 된다(mip 0에는 둘 다 적용하지 않는다). d개의 상위 mip은 만든 뒤 버린다.
6. 출력 부호화·양자화(12.2절 3·5번), swizzle 기록(12.4.3절).

크기 조정 필터 Lanczos3(결정적 계산 — 기준값 표로 고정):
- 표본 중심: 대상 i의 원본 좌표 = (i + 0.5) × (원본 길이 / 대상 길이) − 0.5.
- 커널: L(x) = sinc(x)·sinc(x / 3)(|x| < 3, 그 밖 0), sinc(x) = sin(πx) / (πx), sinc(0) = 1. Core 결정적 수학의 `SinPi`로 계산한다(A0-C).
- 축소(비율 s = 대상 / 원본 < 1)이면 커널을 1/s배 넓힌다(L(x·s), 반경 3/s 원본 texel). 확대이면 반경 3.
- 가중치는 탭 합으로 정규화한다. 축별 분리(X → Y, Volume Resize는 X·Y만).
- 경계는 텍스처 Address를 따른다: Wrap → 감싸기, Mirror → GPU MIRROR(주기 2n, 끝 texel 포함 반사 — n = 1이면 항상 그 texel), Clamp·Border → 끝 texel 반복.
- 정수 출력 형식으로 갈 값은 결과를 [0, 1]로 자른다(음수 lobe의 넘침 제거). float 형식은 자르지 않고 12.2절 5번만 적용한다.

mip 규칙:

- 크기: 각 축 길이 n의 다음 mip 길이는 m = max(1, floor(n / 2))이며 1×1(×1)까지 만든다.
- SimpleAverage(축별 가중치의 곱 — 분리 가능, 2D 최대 3×3, Volume 최대 3×3×3 표본):
  - n = 1: 그 축은 복사(가중치 1).
  - n 짝수: 대상 i는 원본 2i·2i+1을 ½·½.
  - n 홀수(n ≥ 3): 대상 i는 원본 2i·2i+1·2i+2를 (m − i)/n·m/n·(i + 1)/n(다상 3탭 상자 필터 — 모든 원본 texel의 기여가 m/n으로 같아 평균이 보존된다).
- 색은 선형 float에서 평균하고(1번에서 이미 선형화), 알파 가중하지 않는다(UE SimpleAverage). Normalmap(`bNormalizeNormals`)은 평균 후 [−1, 1]로 펴서 재정규화하고(길이 0이면 (0, 0, 1)) 다시 [0, 1]로 접는다.
- Unfiltered: 원본 [2i, 2j(, 2k)] 표본(UE: 2×2(×2) 묶음의 첫 texel). n = 1인 축은 0.
- Cube: 면별 독립 mip(면 경계 연속성은 Angular와 함께 후속).
- PreserveBorder(mip k ≥ 1, 각 slice·면): 바깥 한 줄(첫·끝 행과 열)의 texel을 mip 0 테두리에서 최근접으로 가져온 값으로 덮는다. 길이 m인 변의 위치 j는 mip 0의 같은 변(길이 n)의 위치 clamp(floor((j + 0.5) × n / m), 0, n − 1)를 쓰고, 모서리는 mip 0의 같은 모서리다. 한 축 길이가 1이면 그 축은 덮을 안쪽이 없으므로 mip 0 테두리의 해당 위치 값 하나다. 바뀐 값은 다음 mip의 입력이 된다(5번).
- 정수 반올림은 출력 양자화에서만 한다(12.2절 5번).
- 시험: 4×4·5×3·1×7·3×1·Volume 4×4×4·3×3×3 원본의 mip 픽셀 표, PowerOfTwoMode 6종·Lanczos3 기준값 표, LOD 합성(LODBias·MaxTextureSize·그룹 MaxLODSize·MinLODSize·MaxLODMipCount·NoMipmaps) 표.

#### 12.5.1 스트리밍 메타와 상주 꼬리(텍스처 스트리밍 T1·T5·T8)

2026-11-20 렌더 세션 요구([텍스처 스트리밍](../TextureStreaming.ko.md) 4.3·4.4·11절, [렌더 기반](../RenderBase.ko.md) 6.5절)를 쿠킹 규칙으로 정한다. 렌더는 상주 mip 수를 다시 계산하지 않고 아래 메타를 쓴다. N = 쿠킹 뒤 mip 수(12.5절 4번에서 d개를 버린 뒤), X·Y = 쿠킹 뒤 상위 mip의 크기다.

1. **덩어리와 hash.**
   - 스트리밍 mip은 mip 하나가 bulk 덩어리 하나다(그 mip의 모든 slice·면·깊이).
   - 상주 꼬리 mip은 모두 덩어리 하나에 큰 mip부터 이어 둔다. 메타의 mip 항목은 그 덩어리 안 오프셋과 바이트 크기를 가진다.
   - 덩어리 hash는 그 덩어리 `BulkDataRef`의 raw SHA-256이다(5절 Bulk 섹션 다이제스트). hash 칸을 따로 두지 않는다. 스트리밍 전송은 읽고 푼 뒤 대조하고, 다르면 손상 오류를 낸다(TextureStreaming 5.1절). 상주 꼬리 덩어리는 패키지 로드의 bulk 검증을 따른다.
   - UE 대응: mip마다 bulk 하나(`FTexture2DMipMap`, TextureResource.h 45–57), inline mip은 에셋과 함께 상주.
2. **상주 꼬리 mip 수 R**(T8, 쿠킹 때 계산해 메타에 쓴다).
   - 비스트리밍(3번)이거나 N ≤ 1이면 R = N.
   - 그 밖에는 R = min(N, max(1, 형식 mip tail 수, `TextureMinResidentMipCount` = 7)). 형식 mip tail 수는 플랫폼이 작은 mip을 한 할당에 묶는 수다(타일링 플랫폼이 없어 지금은 0).
   - 블록 압축 형식(블록 4×4)은 R = max(R, N − floor(log2(X / 4)), N − floor(log2(Y / 4)))다. 스트리밍 mip은 각 축이 블록의 배수가 되고, 상주 꼬리의 가장 큰 mip은 각 축이 4 이상이 된다. 스트리밍 텍스처는 2의 거듭제곱이므로(3번) 식이 정확하다. X 또는 Y가 4보다 작으면 N ≤ 2라 첫 규칙에서 이미 R = N이다.
   - UE 5.8 `GetNumNonStreamingMipsDirect`(Developer/TextureFormat/Public/Interfaces/ITextureFormat.h 157–195, `NUM_INLINE_DERIVED_MIPS` = 7)와 같다. UE는 LOD 편향으로 자르기 전의 크기로 계산한 뒤 남은 mip 수로 자르는데, N − floor(log2(X / 4))는 자르기 전후가 같으므로 결과가 같다.
   - 7은 이름 있는 상수다. 바꾸면 쿠킹 결과가 바뀌므로 쿠커 버전을 올린다.
3. **비스트리밍**(T5). 다음이면 쿠킹이 R = N으로 써서 모든 mip을 상주 꼬리 덩어리 하나에 둔다. 스트리밍 덩어리를 만들지 않고 별도 표시 칸도 두지 않는다 — R = N이 곧 비스트리밍이다(UE와 같다: 비스트리밍이면 모든 mip을 inline으로 쓴다, TextureDerivedData.cpp 3001–3004).
   - `NeverStream`, UI 그룹, N = 1, `NoMipmaps`.
   - Cube·CubeArray(UE `GetStreamingDisabledForNonVirtualTextureProperties`, ITextureFormat.h 49–64). Volume·2DArray는 스트리밍한다(UE 기본값과 같다).
   - X 또는 Y가 2의 거듭제곱이 아닌 크기(1차, 후속에서 다시 본다).
   - `MipLoadOptions` = `OnlyFirstMip`(12.4.1절과 같다).
   - `Availability` = CPU(GPU에는 placeholder만 있다).
   - TextureStreaming 4.3절의 "전체 상주 전용" 표시는 셰이더 사용에서 정하는 런타임 판정이라 쿠킹 대상이 아니다.
4. **시네마틱 mip.** 쿠킹 편향 d(12.5절 4번)에는 `NumCinematicMipLevels`를 넣지 않고, 시네마틱용으로 버릴 mip을 따로 남기지도 않는다. 시네마틱 mip은 쿠킹 결과의 상위 mip이다. 런타임은 시네마틱 pin이 아닐 때 `NumCinematicMipLevels`만큼 bias를 더해(상주 꼬리 아래로는 줄이지 않는다) 그 mip을 쓰지 않고, 시네마틱 pin일 때는 그 bias를 뺀다. `NumCinematicMipLevels`는 `StreamableRenderAsset`의 저장 설정 그대로 메타가 된다(12.1절).
   - UE 5.8 확인: 쿠킹은 `CalculateLODBias`를 `NumCinematicMipLevels` = 0으로 부른다(TextureDerivedData.cpp 2969). 쿠킹된 플랫폼에서 런타임 `CalculateLODBias`는 텍스처·그룹 LODBias를 다시 더하지 않고 `NumCinematicMipLevels`만 더한다(TextureLODSettings.cpp 262–275). 스트리밍은 강제 상주 + 시네마틱 mip 사용일 때 그만큼 뺀다(StreamingTexture.cpp 187–193, Texture.cpp 2112 주석 "ResourceLODBias almost always == NumCinematicMipLevels").
   - 예: 원본 1024, LODBias 2, `NumCinematicMipLevels` 1이면 쿠킹 상위 256, 평소 128, 시네마틱 256이다. 렌더 세션 요구안("min(NumCinematicMipLevels, 쿠킹 LOD bias)개를 남긴다")이면 쿠킹 상위 512, 평소 256, 시네마틱 512가 되어 UE와 다르다. 그래서 UE 방식으로 정했고, TextureStreaming 4.3절 문구 개정을 렌더 세션에 요청했다.
5. **bias 용어**(T1). 서로 더하지 않는 세 양이다.
   - 쿠킹 편향: 텍스처·그룹 `LODBias`와 크기 상한으로 버리는 상위 mip 수(12.5절 4번). 쿠킹 결과에 이미 반영되어 런타임이 다시 더하지 않는다.
   - 런타임 bias: 전역 bias + 예산 bias + 비시네마틱 감산(4번). TextureStreaming 4.3절 `maxMips`가 쓴다.
   - 표본 mip bias: view의 텍스처 mip bias(temporal upscaler의 음수 bias 등). 상주 계획의 표본 해상도 = 렌더 폭 × 2^(−표본 mip bias)다(TextureStreaming 4.1절). 에셋 데이터와 무관하며 쿠킹에 들어가지 않는다.
6. **시험(A5).**
   - R 표: 비압축·블록 압축 형식 × 1×1·4×4·8×8·256×256·2048×2048·2048×64.
   - 비스트리밍 조건마다 R = N이고 스트리밍 덩어리가 없음.
   - 쿠킹 편향과 `NumCinematicMipLevels` 조합의 쿠킹 상위 mip 표(4번 예 포함).
   - 덩어리 SHA-256이 쿠킹 데이터 바이트와 같음, 메타 오프셋·바이트 크기 합이 꼬리 덩어리 크기와 같음.

### 12.6 GPU 압축 조사

BC·ASTC·ETC 인코더 없음, 렌더러는 비압축만. 쿠킹은 비압축으로 완성하고 프로파일의 압축 지원은 "없음". 압축 인코더는 텍스처 파이프라인 후속 단계(사용자 결정 순서).

### 12.7 A5 구현 단계(2026-11-23)

순서 근거:
- 클래스 기본값 레코드·차이 쓰기(RuntimeClassMetadata '클래스 기본값 결정' 시점 항목)는 모든 패키지 저장 byte를 바꾼다. 그래서 텍스처 fixture보다 먼저 한다(뒤에 하면 텍스처 fixture를 다시 만든다 — 하위호환 없음). 텍스처의 계약 설정 판정(`UnsupportedSetting` = 레코드 기본값과 다름)도 레코드를 쓴다.
- 텍스처는 타입 → 원본 디코더 → 임포터 → 빌드 → 쿠킹 순이다. 각 단계는 앞 단계 산출물만 쓴다.
- 빌드·시험 실행은 화면 마일스톤 뒤에 한 번 한다(사용자 결정 2026-11-23). 단계마다 시험·대조표를 쓰고 리뷰어 판정을 받고 로컬 커밋한다. coverage와 실행 결과는 빌드 때 채운다.
- 단계마다 구현 전에 그 단계의 세부 설계를 이 문서(또는 해당 문서 — 레코드는 RuntimeClassMetadata·ObjectFoundations)에 먼저 적는다.

| 단계 | 내용 | 모듈 |
|---|---|---|
| A5a | 클래스 기본값 레코드: Registry 밖 인스턴스 인코딩 경로(그래프 저장과 같은 인코딩 본문), TypeRegistry 등록 레코드에 붙는 지연 생성·한 번 게시(TypeLease로 접근), 두 번 생성해 인코딩 대조(결정성), 참조 기본값 null만, 할당 실패가 아닌 실패는 등록 세대 동안 기억, 레코드가 없는 타입(추상·불완전 레이아웃·생성 함수표 없음), 레코드 경로 조회(PropertyPath), 지문(레코드 byte SHA-256 + 구성(editor·cooked)·codec wire 버전) | ObjectRuntime·ObjectSerialization |
| A5b | (A5b1 차이 쓰기·override 집합 → A5b2 되돌리기 — RuntimeClassMetadata 'A5b' 명세) 차이 쓰기와 override 집합: 차이 형식은 호출자 옵션(패키지 저장만 — undo·스냅샷·복제·세이브는 전부 저장 + 집합 기록), 저장 값 = override 집합 ∪ 기준과 인코딩이 다른 값, struct 멤버 단위·Atomic 통째·배열 통째, 차이 형식 로드가 파일의 값 경로로 집합을 채움, 경로 쓰기(`asset-set`)가 기록, 되돌리기(단일 값·원소·멤버, Atomic 통째), 같음 = wire byte(참조는 대상 정체성) | ObjectSerialization·AssetRuntime·AssetDatabase |
| A5c | 지문 연결과 이주: 패키지 Summary 타입 표 클래스 행 지문 열(A1 형식 개정), cookKey 지역 입력의 export 구체 클래스별 (TypeId, 지문) 목록(A4 개정), 쿠킹 패키지 로드의 지문 불일치 `UnsupportedVersion`·에디터 패키지 진단, `TickSettings` 리플렉션 이동, fixture·바이트 헤더 재생성 | AssetRuntime·AssetCooker·Engine |
| A5d | 텍스처 타입: `StreamableRenderAsset`(AssetRuntime), `Texture`와 차원 클래스 다섯, 설정 enum(12.4절 — TextureGroup 82종 등), `TextureSource`·`TexturePlatformData`, 계약 설정 `UnsupportedSetting`·숨김 값 `InvalidSetting` 판정 | Texture(새 Runtime 모듈)·AssetRuntime |
| A5e | 원본 디코더 1: 결과 형식(TextureSource 형식 10종)·진단 공통부, BMP·TGA·PNG(15절)·PSD(12.2절 규칙) | ImageCore(새 Developer 모듈) |
| A5f | 원본 디코더 2: DDS 비압축·TIFF 무손실·Radiance HDR·무손실 OpenEXR | ImageCore |
| A5g | JPEG: libjpeg-turbo를 ThirdParty로(`Engine/Source/ThirdParty/LibJpegTurbo` — 버전 고정 `ThirdParty.json`, C 경로·`JDCT_ISLOW`·NASM 없음, CMake가 만드는 설정 헤더는 손으로 쓴다), ImageCore가 감싸 G8·RGBA8과 같은 진단으로 낸다(CMYK·YCCK·12비트·lossless `UnsupportedSourceFormat`) | ThirdParty·ImageCore |
| A5h | 임포터: TextureTools 임포터(A2f 프레임워크 — 차원 구성마다 역할·접두 역할, 12.3절 원본 구성과 Cube 배치 3종·Volume 타일), `TextureImportSpec`, 명령 문법(`--cube`·`--cube-layout`·`--array`·`--cube-array`·`--volume`·`--volume-tile` — 13절 예약 이름), 저장 설정 편의(12.2절 4번), 원본 정규 픽셀 bulk(Zstd) | TextureTools(새 Developer 모듈) |
| A5i | 빌드: 12.5절 전부(선형화·`bFlipGreenChannel`·PowerOfTwoMode·Lanczos3·mip 규칙·PreserveBorder·NormalizeNormals·출력 부호화·양자화·swizzle·LOD 합성)와 기준값 표 시험 | TextureTools |
| A5j | 쿠킹: TargetProfile 텍스처 칸(형식 지원 표·LOD 그룹 Map `MergeByKey`, 엔진 `Windows`·`Android` 기본값 — 10.2절 2번), 텍스처 `CookProcessor`(너겟 키 = 처리기가 읽은 유효 설정·원본 SHA-256·그룹 값), `TexturePlatformData`(mip 덩어리·상주 꼬리 R·샘플러 — 12.5.1절), 에셋 원본 묶음(`SourceTextures`·`Source2DTexture` — Build 의존), 비스트리밍 조건 | TextureTools·AssetCooker |
| A5k | 14절 텍스처 시나리오(임포트 → 설정 편집 → 쿠킹 → 런타임 로드 데이터 대조), 클래스별 PostLoad dispatch 필요 여부 판정(11절 9번 — 텍스처가 쓰지 않으면 후속으로 둔다), 렌더 세션에 쿠킹 데이터 형태 알림 | 전체 |

- 임포터 구성(차원마다 임포터를 나눌지, 고정 역할·접두 역할 배치)은 A5h 세부 설계에서 정한다. A2f 규칙상 고정 역할은 정확히 한 번씩 와야 하므로(7.1절 19 A2f3b), 원본 하나 구성(2D·배치 Cube·타일 Volume)과 파일 여러 개 구성(6면 Cube·Array·slice Volume)은 역할 집합이 다르다.
- A5a–A5c의 표현 방식(override 집합 자료구조, Registry 밖 인코딩 경로, 레코드 경로 해석기)은 RuntimeClassMetadata가 "A5 구현 명세에서 정한다"고 남긴 항목이다. A5a 착수 때 RuntimeClassMetadata에 그 명세를 먼저 적는다(ObjectFoundations는 1000줄이 넘어 나눔 대상이라 결정 원문이 있는 RuntimeClassMetadata에 둔다 — A5a 명세 2026-11-23).

### 12.8 A5d 구현 명세 — 텍스처 타입(2026-11-24, 착수 전 — 교차 검증 1차 반영: 색도 좌표 기본값, UE 새 텍스처 설정과 영향 조건, 판정 전용 API, ObjectRefArray)

12.1·12.4절의 타입·설정을 리플렉션 타입으로 만들고 계약·숨김 값 판정을 둔다(12.7절 A5d). 원본 디코더·임포터·빌드·쿠킹은 A5e–A5j다. 근거: UE 5.8 `Texture.h`(UTexture 멤버 1268–1675행과 `WITH_EDITORONLY_DATA` 구간, 생성자 기본값 Texture.cpp 148행), `TextureDefines.h`(enum 값), `StreamableRenderAsset.h` 338–395행, 차원별 헤더, `VirtualTextureEnum.h`(EVTProducerPriority).

**모듈과 등록**:
- `StreamableRenderAsset`은 AssetRuntime(`AssetRuntime/StreamableRenderAsset.h`, `ROCKS_CLASS(Abstract)` : Asset)이다.
- 새 Runtime 모듈 `Texture`(Engine/Source/Runtime/Texture — 공개 의존 AssetRuntime·CoreTypeBindings·GraphicsTypes, 그래픽 장치 의존 없음): `Texture/TextureEnums.h`(enum), `Texture/TextureSource.h`(TextureSource·TexturePlatformData·보조 struct), `Texture/Texture.h`(Texture와 차원 클래스 다섯), `Texture/TextureSettingsCheck.h`(판정). rocksids는 `Texture.rocksids.json`.
- 에셋 명령의 타입 모듈은 목록이 된다: `AssetTypeModules = {"Animation", "Texture"}`(AssetCommandTypes가 차례로 시작 — 각 의존 폐포의 타입이 등록된다). 하나의 집계 모듈을 두는 대안은 모듈 경계 밖 묶음이라 버린다.

**enum**(값 = UE 값, 이름은 UE 접두사를 뗀 그대로 — 12.4절 원칙, 저장 형식이라 바꾸지 않는다, 바탕 형식 uint8): `TextureGroup`(82종 World=0…Project48=81), `TextureMipGenSettings`(22종 FromTextureGroup=0…Angular=21), `TexturePowerOfTwoSetting`(6), `TextureSamplerFilter`(Point·Bilinear·Trilinear·AnisotropicPoint·AnisotropicLinear), `TextureMipLoadOptions`(Default·AllMips·OnlyFirstMip), `TextureAvailability`(GPU·CPU), `TextureDownscaleOptions`(14), `TextureLossyCompressionAmount`(7), `TextureCompositeMode`(Disabled…NormalRoughnessToAlpha 5종), `TextureCompressionSettings`(17종 — `LQ`(13)·`EncodedReflectionCapture`(14)는 `Hidden` 열거자: 텍스트·편집에 보이지 않지만 저장 값은 받고 판정이 `InvalidSetting`), `TextureSourceEncoding`(18종 None…HLG), `TextureColorSpace`(15종 — Custom = 99), `TextureCookPlatformTilingSettings`(3), `TextureChromaticAdaptationMethod`(3), `TextureFilter`(Nearest·Bilinear·Trilinear·Default), `TextureAddress`(Wrap·Clamp·Mirror + Rocks `Border` = 3), `TextureBorderColor`(TransparentBlack·OpaqueBlack·OpaqueWhite — Rocks), `TextureCompressionQuality`(6), `VirtualTextureProducerPriority`(Lowest…Highest 8종, `Count` Hidden), `TextureSourceFormat`(Rocks: Invalid=0·G8·GA8·G16·GA16·RGBA8·RGBA16·RGBA16F·RGBA32F·R16F·R32F — 12.4.4절), `TextureDimension`(Texture2D·TextureCube·Texture2DArray·TextureCubeArray·Volume), `TextureSwizzle`(R·G·B·A·Zero·One). UE의 `_MAX`·`Count` 표지는 두지 않는다(VirtualTextureProducerPriority만 UE처럼 Hidden).

**StreamableRenderAsset 필드**: `NeverStream`·`bGlobalForceMipLevelsToBeResident`(uint8 bitfield), `NumCinematicMipLevels`(int32 — 계약). 스트리밍 표 handle은 `ROCKS_NATIVE` 칸 하나(12.1절 — 저장하지 않음).

**Texture 필드**(아래는 UE 이름 — Rocks 필드·도구 이름은 그 camelCase·b 접두 없는 이름, 12.4절. 기본값 = UE 생성자 값·그 밖 0. EditorOnly는 UE `WITH_EDITORONLY_DATA` 구간과 같다):
- EditorOnly: `Source`(TextureSource), `AdjustBrightness`(1)·`AdjustBrightnessCurve`(1)·`AdjustVibrance`(0)·`AdjustSaturation`(1)·`AdjustRGBCurve`(1)·`AdjustHue`(0)·`AdjustMinAlpha`(0)·`AdjustMaxAlpha`(1), `CompressionNoAlpha`·`CompressionForceAlpha`·`CompressionNone`(bitfield), `LossyCompressionAmount`(Default), `OodleTextureSdkVersion`(Name, None), `MaxTextureSize`(0), `CompressionQuality`(Default), `CompressionCacheId`(Guid 0), `bDoScaleMipsForAlphaCoverage`(false), `AlphaCoverageThresholds`(Vec4d 0 — UE FVector4는 double), `bUseNewMipFilter`(false), `bPreserveBorder`·`bFlipGreenChannel`(bitfield), `PowerOfTwoMode`(None), `PaddingColor`(Color 0,0,0,255 — Rocks Color 기본 생성자는 a = 0이라 명시), `bPadWithBorderColor`(false), `ResizeDuringBuildX`·`ResizeDuringBuildY`(0), `bChromaKeyTexture`(false), `ChromaKeyThreshold`(1/255), `ChromaKeyColor`(Color 마젠타 255,0,255,255), `MipGenSettings`(FromTextureGroup), `CompositeTexture`(ObjectRef<Texture> — Hard), `CompositeTextureMode`(NormalRoughnessToAlpha), `CompositePower`(1), `LayerFormatSettings`(Array<TextureFormatSettings>), `bNormalizeNormals`·`bUseLegacyGamma`(bitfield), `SourceColorSettings`(TextureSourceColorSettings).
- 저장(런타임): `LODBias`(0), `CompressionSettings`(Default), `Filter`(Default), `MipLoadOptions`(Default), `VirtualTextureStreamingPriority`(Normal), `VirtualTexturePrefetchMips`(uint8 0), `CookPlatformTilingSettings`(FromTextureGroup), `bOodlePreserveExtremes`(false), `LODGroup`(World), `Downscale`(TextureDownscale — 기본 0, 플랫폼별 `OrderedMap<Name, float>`), `DownscaleOptions`(Default), `Availability`(GPU), `SRGB`(bitfield 1), `bNoTiling`·`VirtualTextureStreaming`·`bUseVirtualTextureStreamingPriority`·`CompressionYCoCg`(bitfield 0), `PlatformData`(TexturePlatformData — 쿠킹이 채운다. 에디터 패키지에서는 기본값이라 차이 저장이 생략한다).
- 두지 않는다(12.4.1절 "대응하지 않음"): LightingGuid·LevelIndex·CompressFinal·DeferCompression·bNotOfflineProcessed·AssetUserData·폐기 멤버·런타임 자원.
- 보조 struct: `TextureSourceColorSettings`(EncodingOverride·ColorSpace·Red/Green/Blue/WhiteChromaticityCoordinate(Vec2d (0, 0) — UE `FTextureSourceColorSettings()`가 0으로 두고 ColorSpace에 따라 `UpdateColorSpaceChromaticities`가 채운다, Texture.cpp 262–270)·ChromaticAdaptationMethod(Bradford — UE 기본)), `TextureFormatSettings`(CompressionSettings·CompressionNoAlpha·CompressionForceAlpha·CompressionNone·CompressionYCoCg·SRGB — UE FTextureFormatSettings), `TextureDownscale`(Default float·PerPlatform OrderedMap<Name, float> — UE FPerPlatformFloat).

**차원 클래스**: `Texture2D`(AddressX·AddressY = Wrap, BorderColor), `TextureCube`(없음), `Texture2DArray`(AddressX·AddressY·AddressZ = Wrap, BorderColor, EditorOnly `SourceTextures` ObjectRefArray<Texture2D> — RMC2304), `TextureCubeArray`(EditorOnly `SourceTextures` ObjectRefArray<TextureCube>), `VolumeTexture`(AddressMode = Wrap, BorderColor, EditorOnly `Source2DTexture` ObjectRef<Texture2D>·`Source2DTileSizeX`·`Source2DTileSizeY`). `BorderColor`(TextureBorderColor, 기본 TransparentBlack)는 Rocks 확장이다(12.4.2절). UE Texture2D의 `ImportedSize`·`FirstResourceMemMip`·`bTemporarilyDisableStreaming`·`bHasBeenPaintedInEditor`는 두지 않는다(설정 아님).

**TextureSource**(EditorOnly 필드만): `Format`(TextureSourceFormat), `SizeX`·`SizeY`·`NumSlices`(uint32), `NumMips`·`NumLayers`(uint32, 1 — 다른 값은 계약), `Bulk`(BulkDataRef — Zstd, raw SHA-256 = 원본 픽셀 정체성). 검증 hook(`Validate`): Format이 Invalid가 아니면 크기 > 0, NumSlices ≥ 1. 크기 규칙(차원별 slice 수 — Cube 6, CubeArray 6의 배수)은 빌드 판정(A5i)이다.

**TexturePlatformData**(쿠킹 데이터): `Dimension`, `PixelFormat`(uint8 — GraphicsTypes `PixelFormat` 번호, 검증 hook이 `PixelFormatFromNumber`로 알려진 번호인지와 비압축 색 형식인지 본다. 번호가 쿠킹 데이터의 저장 형식이 된다 — 기존 번호를 바꾸거나 다시 쓰지 않고, 부득이 뜻을 바꾸면 TexturePlatformData 타입 버전을 올려 다시 쿠킹되게 한다. PixelFormat 문서(렌더 세션 소유)에 같은 계약을 적도록 A5k 알림에 넣는다 — 리플렉션 enum이 아닌 그래픽 형식 표를 저장 칸에 그대로 쓰는 것은 UE FTexturePlatformData::PixelFormat과 같다), `SRGB`(bool), `Swizzle`(TextureSwizzle 넷), `SizeX`·`SizeY`·`SizeZ`·`NumMips`·`NumSlices`(uint32), `Mips`(Array<TextureMipData{SizeX·SizeY·SizeZ, ByteSize u64, Bulk BulkDataRef(스트리밍 mip), TailOffset u64(상주 꼬리 mip)}>), `TailBulk`(BulkDataRef), `ResidentTailMips`(uint32), `Sampler`(TextureSamplerSettings{Filter TextureSamplerFilter, MaxAniso uint32, AddressU·V·W TextureAddress, BorderColor}). 값의 일관성(mip 크기 사슬·꼬리 범위)은 쿠킹 처리기(A5j)와 렌더 업로드가 본다 — 여기서는 칸만 정한다.

**판정**(`CheckTextureSettings(ObjectRegistry&, access, ObjectHandle texture, Allocator&, Array<TextureSettingIssue>& out) → Result<void>`, 이슈 = {AssetDiagnosticCode UnsupportedSetting·InvalidSetting, PropertyPathBuffer 경로}):
- 계약 필드(아래 목록)의 값이 클래스 기본값과 다르면 `UnsupportedSetting`이다. 같음은 차이 저장과 같은 판정이다(A5b — wire byte 같음, NaN payload까지). 판정 전용 API ObjectSerialization `FindChangedFields(ObjectRegistry&, access, ObjectHandle, Allocator&, Array<PropertyPathBuffer>& out) → Result<void>`가 그 객체의 필드를 그래프 저장과 같은 필드 루프·기준 비교(`OmitCurrent`)로 인코딩하되 노드 Record를 만들지 않고 기준과 다른 위치를 모은다: 최상위 필드, 비Atomic struct는 다른 잎 멤버 경로(A5b override 단위와 같은 펼침 — Atomic struct·배열·Map은 그 필드 경로). 객체 참조는 기준이 늘 null이므로 non-null이면 언제나 다름이다(그래프 밖 대상도 — 그래프 저장의 외부 참조 정책을 거치지 않는다). 레코드 없는 타입은 InvalidArgument가 아니라 Unsupported(호출자가 값 판정만 한다), 레코드 실패는 그 오류다.
- 계약 필드와 영향 조건(조건이 거짓이면 그 필드는 결과를 바꾸지 않으므로 판정하지 않는다 — UE가 새 텍스처에 쓰는 `SetModernSettingsForNewOrChangedTexture` 값(Texture.cpp 211–235: CompositeTexture 없으면 CompositeTextureMode = Disabled, AlphaCoverageThresholds = (0,0,0,0.75), bNormalizeNormals·bUseNewMipFilter = true, OodleTextureSdkVersion 최신)을 가진 교환 텍스처가 걸리지 않게): Adjust* 8개(늘), bChromaKeyTexture(늘)·ChromaKeyThreshold·ChromaKeyColor(bChromaKeyTexture일 때), bDoScaleMipsForAlphaCoverage(늘)·AlphaCoverageThresholds(bDoScaleMipsForAlphaCoverage일 때), CompositeTexture(늘)·CompositeTextureMode·CompositePower(CompositeTexture가 non-null일 때), Downscale·DownscaleOptions(늘), SourceColorSettings의 색도 좌표 넷·ChromaticAdaptationMethod(ColorSpace가 Custom일 때 — 그 밖의 ColorSpace는 값 판정이 먼저 거부하거나(None·sRGB 밖) 좌표가 쓰이지 않는다), CompressionYCoCg·LayerFormatSettings·LossyCompressionAmount·CompressionQuality·bOodlePreserveExtremes(늘), NumCinematicMipLevels(늘), VirtualTextureStreaming(늘)·VirtualTextureStreamingPriority·bUseVirtualTextureStreamingPriority·VirtualTexturePrefetchMips(VirtualTextureStreaming일 때), Source.NumMips·Source.NumLayers(늘). 기록만 하는 필드(판정 없음): OodleTextureSdkVersion, bUseNewMipFilter(12.4.1절), bNormalizeNormals(구현 — 12.4.1절).
- 값 판정(부분 구현 enum — 기본값과 같은지가 아니라 받는 값인지): MipGenSettings가 Sharpen0–10·Blur1–5·Angular·LeaveExistingMips, SourceColorSettings.EncodingOverride가 None·Linear·sRGB 밖, ColorSpace가 None·sRGB 밖, MipLoadOptions = OnlyFirstMip이면 `UnsupportedSetting`. CompressionSettings가 LQ·EncodedReflectionCapture면 `InvalidSetting`.
- 이슈는 경로 순(스키마 필드 순 → 멤버 순)이고 결정적이다. 레코드 없는 타입(손 함수표 파생 등)은 차이 판정을 하지 않고 값 판정만 한다(`FindChangedFields`의 Unsupported), 레코드 실패는 그 오류다.
- 텍스트 입력의 숨김 값(asset-set·임포트 설정의 `CompressionSettings=LQ`)은 ReflectionText `HiddenEnumerator` 진단으로 거부되고, 에셋 명령 출력(AssetCommandlets `AssetTextDiagnosticName` — asset-new·asset-set·asset-import·asset-cook의 대입 진단 줄)이 그 진단을 `InvalidSetting` 진단 이름으로 사상한다(숨김 열거자는 텍스처 밖에서도 저장 값으로만 유효한 값이므로 모든 필드에 같다. 구현 때 정함 — 처음엔 AssetService가 텍스처 필드만 사상한다고 적었다)(ReflectionExtensions 748행 "A5가 InvalidSetting으로 바꾼다"의 지점 — 명령 출력 `asset-set: InvalidSetting: CompressionSettings: …`). binary로 들어온 숨김 값은 판정이 `InvalidSetting`이다.
- 부르는 곳: 텍스처 빌드(A5i)가 빌드 전에, 쿠킹이 그 결과로 실패한다. 임포터·asset-set은 저장을 막지 않는다(UE도 계약 값 저장은 막지 않는다 — 빌드에서 거부).

**구현에서 정한 점(2026-11-24)**: enum은 UE 접두를 받지 않는다(ToolPrefix 없음 — 사용자 결정). 필드 이름은 camelCase·b 접두 없음(사용자 결정 — 12.4절). 숫자로 시작하는 TextureGroup 이름은 C++ `EightBitData`·`SixteenBitData`에 도구 이름 `8BitData`·`16BitData`다. TextureMipGenSettings는 22종(0–21)이다(처음 25종으로 잘못 적었다). 판정 이슈는 같은 위치를 한 번만 낸다(Downscale처럼 멤버 여럿이 달라도 필드 이슈 하나). 텍스처 모듈 ID는 `Texture.rocksids.json`(35 타입·120 필드).

**버린 대안**: 계약 판정을 노드 Record를 만드는 그래프 저장으로(외부 참조 정책 — Reject면 non-null 참조가 오류, Admit의 Null 대체는 기준과 같은 0이 되어 같음으로 잘못 판정, Include는 대상까지 노드로 끌어온다 — 교차 검증 필수 3), Rocks 생성자 기본값을 UE의 새 텍스처 값으로(bUseNewMipFilter = true가 보정 전 의미 미확정과 부딪치고 UE 생성자 값과 다른 레코드가 된다 — 영향 조건 판정으로 푼다), 계약 필드마다 기본값 상수를 코드에 두고 비교(레코드와 두 곳에서 어긋난다 — 차이 저장 판정 하나를 쓴다), TexturePlatformData를 Transient로 두고 쿠킹 데이터를 별도 섹션에(쿠킹 패키지도 리플렉션 그래프 하나로 — 5.6절), PixelFormat을 텍스처 전용 enum으로(그래픽 형식 표와 두 벌이 된다), 집계 타입 모듈.

**시험(A5d)**: 등록 형태(필드 수·이름 노출·EditorOnly 플래그·기본값 = UE 생성자 값 — 레코드 경로 조회로, 색도 좌표 (0,0)·PaddingColor 알파 255), enum 값(UE 값과 표 대조·Hidden 열거자의 텍스트 거부), 차원 클래스 상속·주소 기본값, TextureSource·TexturePlatformData 검증 hook 거부, 그래프 왕복(에디터·쿠킹 정책 — 쿠킹은 EditorOnly 없음), 판정(계약 필드 하나씩 바꿈 → UnsupportedSetting 경로, 기본값 그대로 → 이슈 없음, NaN payload·-0.0 차이, 값 판정 각 값, 숨김 값 InvalidSetting(binary·텍스트 사상), 레코드 없는 파생은 값 판정만, non-null CompositeTexture(그래프 밖 대상) → 다름, UE 새 텍스처 설정 값(CompositeTextureMode Disabled·AlphaCoverageThresholds 0.75·bNormalizeNormals·bUseNewMipFilter·OodleTextureSdkVersion)만 바꾼 텍스처 → 이슈 없음, 영향 조건이 참이면 같은 필드가 이슈), FindChangedFields(최상위·비Atomic 잎·Atomic 통째·배열 통째·참조), ObjectRefArray SourceTextures 왕복, 에셋 명령 타입 모듈 목록(asset-new Texture2D·asset-info), 할당 실패.

### 12.9 A5e 구현 명세 — 원본 디코더 1(2026-11-24, 착수 전 — 교차 검증 1차 반영: 진단 등록, UE 대조와 결과 차이)

12.2절 표·원본별 규칙과 15절(PNG)을 디코더로 만든다(12.7절 A5e — BMP·TGA·PNG·PSD, 결과 형식·진단 공통부). DDS·TIFF·HDR·EXR은 A5f, JPEG는 A5g다. 시제품 브랜치 `prototype/texture-importer`의 AssetTools 디코더(RGBA8 전용)는 참고만 한다.

**모듈**: 새 Developer 모듈 `ImageCore`(Engine/Source/Developer/ImageCore — 공개 의존 Texture(`TextureSourceFormat`)·AssetRuntime(`AssetDiagnosticCode`), 비공개 의존 Compression(zlib)). 에디터 도구 전용이다(쿠킹 산출물·런타임에 원본 형식 코드가 없다 — 12.1절).

**API**(`ImageCore/ImageDecode.h`, 이름공간 `rocks::image`):
- `ImageFileFormat` = Unknown·Bmp·Tga·Png·Psd·Dds·Tiff·Hdr·Exr·Jpeg·WebP. `DetectImageFileFormat(Span<const uint8> file, StringView pathHint) → ImageFileFormat` — 서명으로 판별하고 서명이 없는 TGA만 TGA 2.0 footer 또는 확장자(`.tga`·`.icb`·`.vda`·`.vst`, 대소문자 무시)로 판별한다.
- `DecodedImage{TextureSourceFormat format, uint32 width, height, numSlices = 1, Array<uint8> pixels}` — 행 순서는 위에서 아래, 행 사이 패딩 없음, 채널은 형식 순서(G·GA·RGBA), 16비트 정수와 float는 little-endian, slice는 차례로 이어 붙인다(A5f DDS의 배열·Cube·Volume — mip은 원본에서 버리고 0번만 둔다, 12.4.4절 NumMips = 1). A5e 형식은 늘 1이다.
- `ImageDecodeLimits{maxDimension = 16384, maxPixelBytes = 4 GiB}` — 한 변이 넘으면 `InvalidSource`·`CapacityExceeded`, 결과 byte가 넘으면 같다(할당 전에 판정). 16384² RGBA32F가 정확히 4 GiB다. 임포트 원본 파일 합 한도(`MaxImportTotalSourceBytes` 2 GiB — A2f)는 입력 파일 byte이고 이 한도는 디코딩한 픽셀이라 따로 본다.
- 진단 등록: `LossySource`·`NonSrgbSource`·`IgnoredSourceChannels`를 `AssetDiagnosticCode` enum 끝에 더한다(시험·명령 인자 `warn=<번호>`가 숫자를 쓴다 — 사이에 끼우지 않는다). 이름표 `AssetDiagnosticName`에 더하고, `AssetDiagnosticIsWarning`은 NonSrgbSource·IgnoredSourceChannels를 경고(임포트 문맥 `Warn`이 받는다), LossySource를 오류로 둔다.
- `ImageDecodeResult`: 성공이면 이미지와 경고 목록(`AssetDiagnosticCode` NonSrgbSource·IgnoredSourceChannels + detail u32 — 경고는 결과를 바꾸지 않는다), 실패면 `ImageDecodeFailure{AssetDiagnosticCode code, ErrorCode error, uint64 offset}`: 구조·길이·CRC·체크섬 위반 `InvalidSource`/InvalidArgument, 받지 않는 변형 `UnsupportedSourceFormat`/Unsupported, 손실 형식 `LossySource`/Unsupported, 한도 `InvalidSource`/CapacityExceeded, 할당 실패 None/OutOfMemory. offset은 판정한 byte 위치(진단용)다.
- `DecodeImage(Allocator&, Span<const uint8> file, ImageFileFormat, const ImageDecodeLimits&, ImageDecodeResult& out) → Result<void>` — format이 Unknown이면 판별한다. A5e 밖 형식(Dds·Tiff·Hdr·Exr·Jpeg)은 그 단계까지 `UnsupportedSourceFormat`, WebP는 손실(VP8)이면 `LossySource`, 무손실(VP8L)이면 `UnsupportedSourceFormat`(12.2절은 손실 WebP만 정했다 — 무손실 WebP는 UE도 받지만 후속), 확장(VP8X)은 뒤따르는 이미지 chunk로 가른다(VP8L 있으면 무손실, VP8 있으면 손실, 애니메이션 ANMF는 그 첫 프레임의 chunk), 판별 실패는 `UnsupportedSourceFormat`.
- 입력은 신뢰하지 않는다: 모든 길이·오프셋·곱셈을 넘침 검사하고, 출력 크기는 헤더로 계산해 한 번 할당한다. 다른 원본 디코더(A5f·A5g)도 같은 결과·진단으로 낸다.

**형식별 규칙**(12.2절 표가 결과 형식을 정한다 — 여기는 구현 경계):
- BMP: BITMAPINFOHEADER(40)·V2(52)·V3(56)·V4(108)·V5(124) 헤더(UE `GetHeaderVersion`과 같다), OS/2 BITMAPCOREHEADER(12)는 `UnsupportedSourceFormat`. 1·4·8비트 색표(BI_RGB·BI_RLE4·BI_RLE8 — RLE의 delta·끝 표지, 그리지 않은 texel은 색표 0번), 16·24·32비트 BI_RGB·BI_BITFIELDS·BI_ALPHABITFIELDS(12.2절 마스크 규칙 — 40 byte 헤더 뒤 마스크, 56 byte 알파 무시), 아래→위·위→아래(음수 높이 — RLE는 아래→위만), 행 4 byte 정렬. 색표 항목 수는 biClrUsed(0이면 2^bitCount), 범위 밖 index는 불투명 검정(UE와 같다).
- TGA: 형식 1·2·3·9·10·11(형식 1의 색표는 적용한다 — UE는 index를 회색으로 읽지만(TgaImageWrapper.cpp 182행) 그것은 단순화라 색표를 쓰는 쪽을 택한다, 결과가 UE와 다르다), 색표 항목 15·16·24·32비트, truecolor 15·16·24·32비트, 회색 8비트(그 밖의 회색 깊이 `UnsupportedSourceFormat`), RLE 패킷이 행을 넘어도 받는다(TGA 2.0 권고 위반이지만 흔하다), image descriptor 원점 비트(위·오른쪽), 12.2절 알파 규칙(알파 비트 수 0이면 무시, 확장 영역 attributes type 3·4 — 4는 straight로 되돌림).
- PNG: 15절. IHDR 검증(압축·필터·interlace 방법 0·1만), 필수 chunk 순서(IHDR 처음·PLTE는 IDAT 앞·IEND 끝), 모르는 필수 chunk(첫 글자 대문자) `UnsupportedSourceFormat`, 모르는 보조 chunk는 건너뜀, CRC32 검사, IDAT 이어 붙인 zlib 해제(Compression `TryDecompress(Zlib)` — Adler-32 확인, 해제 크기 = IHDR 계산값과 정확히 같아야 함), 필터 0–4, Adam7 7패스, 비트 확장(1·2·4비트 회색 v × 255 / (2^b − 1)), tRNS(회색·RGB 지정 값과 같으면 알파 0 — 16비트는 16비트 값으로 비교, 팔레트는 항목별 알파), 결과는 12.2절 표. `NonSrgbSource` 경고(변환 안 함) 판정: sRGB chunk가 있으면 경고 없음(PNG 규격 우선순위 — gAMA·cHRM을 보지 않는다), 없으면 iCCP가 있으면 경고(프로필 내용·이름을 해석하지 않는다), 없으면 gAMA가 45455 ± 500 밖이거나(1/2.2 근처 — 2.2 감마 ±약 1%) cHRM의 8개 값 중 하나라도 sRGB 값(백색 31270·32900, R 64000·33000, G 30000·60000, B 15000·6000)과 1000(0.01) 넘게 다르면 경고, 셋 다 없으면 sRGB로 보고 경고 없음. APNG(acTL)는 기본 이미지만, IDAT가 기본 이미지가 아니면(fcTL이 IDAT 앞에 없음) 그대로 기본 이미지다.
- PSD: 버전 1(PSD)·2(PSB — 길이 칸 8 byte), 색 모드 Bitmap·Grayscale·Indexed·RGB(12.2절 — CMYK·Lab·Multichannel·Duotone `UnsupportedSourceFormat`), 깊이 1(Bitmap)·8·16·32(32는 Grayscale·RGB float), 병합 이미지 압축 0(raw)·1(RLE PackBits — 행 길이 표 PSD 2 byte·PSB 4 byte, 16비트 RLE도 받는다), ZIP(2·3)은 `UnsupportedSourceFormat`, image resource 0x0421 hasRealMergedData false면 `UnsupportedSourceFormat`, 0x0416·0x0417(Indexed 색 수·투명 index), 알파는 12.2절(UE와 같음 — Grayscale 둘째·RGB 넷째 채널, 그보다 많은 채널은 `IgnoredSourceChannels` 경고 — detail = 버린 수), float는 NaN → 0, ±Inf → ±최대 유한값. UE의 PSD 디코더는 ImageWrapper가 아니라 `Editor/UnrealEd/Private/Factories/EditorFactories.cpp`(2483–2640·3100행)이고 버전 1·채널 1·3·4·깊이 8·16·Grayscale·RGB·raw·RLE(16비트 RLE 거부)만 받으며 hasRealMergedData를 보지 않는다.
- UE 대비 상위집합(UE가 받지 않지만 Rocks가 받는 변형 — 같은 파일의 결과는 위 대조대로 같다): BMP 1·4비트·RLE4·RLE8(UE는 BI_RGB·BITFIELDS·ALPHABITFIELDS와 8·16·24·32비트), TGA 9·11·15비트(UE는 1(8비트)·2·3(8비트)·10), PSD PSB·32비트·Bitmap·Indexed·16비트 RLE.
- 결과 형식 대응은 12.2절 표 그대로다(GA8·GA16은 PNG·PSD, BMP·TGA는 회색이 G8·색이 RGBA8).

**버린 대안**: 결과를 늘 RGBA8로(시제품 — 16비트·float 정밀도를 잃는다, 12.4.4절), 디코더를 Texture 모듈에(런타임 모듈에 원본 형식 코드 — 12.1절), 형식마다 다른 오류 표현(임포터가 진단을 일관되게 낼 수 없다), libpng 등 외부 라이브러리(형식이 단순하고 zlib은 Core Compression이 있다 — 사용자 결정 2026-11-23: 어려운 것만 외부, JPEG만 libjpeg-turbo).

**시험(A5e)**: 형식마다 생성기(시험 안에서 바이트를 만든다 — 결정적)로 모든 변형(BMP 헤더 5종·비트 깊이·압축·마스크·방향, TGA 형식 6종·깊이·RLE 경계·원점·알파 규칙·premultiplied, PNG 색 형식 5·비트 깊이 전부·Adam7·필터 5·tRNS·팔레트·APNG, PSD 모드 4·깊이·압축·PSB·병합 표시·Indexed 자원·알파·추가 채널), 손상(길이 절단 모든 위치 — 실패하고 넘침 없음, CRC·Adler·RLE 넘침·색표 범위), 한도(한 변·byte), 판별(서명·footer·확장자·WebP 손실/무손실), 경고, 할당 실패. 외부 시험 묶음 PngSuite(15절)는 빌드 재개 때 받아 넣는다(자유 사용 라이선스 — 결과 대조). 빌드 재개 때 UE와 같은 파일의 픽셀 byte 대조를 몇 장 더한다(PSD 알파·BMP 범위 밖 index·TGA 형식 1).
