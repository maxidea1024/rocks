# 에셋 시스템 설계

기준일: 2026-11-20, 개정 12([텍스처 스트리밍](TextureStreaming.ko.md) T1·T5·T8 반영 — 12.1절 플랫폼 데이터 메타, 12.5.1절 덩어리 hash·상주 꼬리 수·비스트리밍·시네마틱 mip·bias 용어. 시네마틱 mip은 UE 5.8 원본 확인 결과 쿠킹이 따로 남기지 않는다). 개정 11([레벨 스크립트](LevelScript.ko.md) 저장·쿡·로드 진단 — 4.6절). 개정 10(메시·애니메이션 스펙 [MeshAnimationAssets](MeshAnimationAssets.ko.md) MR9·MR10과 [Unreal 교환 형식](UnrealExchange.ko.md) UXR1–UXR3·UXR6 반영 — 3절 모듈, 4.6절 진단, 7절 임포터 태그, 12.2절 손실 원본 예외, 12.4.5절 메시 LOD 그룹, 14.1절 결정적 ID 발급기, 18절). 개정 9(A0-G 구현 — 1절 A0-G 행에 범위·스펙 차이, 2절 그래픽 행). 개정 8(텍스처 기반 클래스 `StreamableRenderAsset` — 12.1절, 사용자 결정). 개정 7(개정 6 최종 검증 반영: 저장 쪽 값 치환·SCC 키 튜플·Commit 간선 보고·Windows 디렉터리 동기화·PreserveBorder 정의). 원본 파일 임포트부터 쿠킹된 런타임 데이터 로드까지 에셋의 전체 수명주기와 첫 수직 단면(PNG → Texture Asset → 설정 수정·재임포트 → 플랫폼별 쿠킹 → 런타임 로드)을 정한다. [엔진 구현 계획](EngineImplementationPlan.ko.md)의 결정을 따른다: §5.2(Outer·객체 경로), §5.3(instancing), §6.1(참조 종류), §7(직렬화·스키마 변경), §9(에셋 파이프라인·로딩·패키지·재임포트·텍스처 스트리밍), §24.2(PrimaryAsset·bundle). 계획을 바꾸는 곳은 17절에 모으고 계획 문서도 같이 고친다. 에디터 UI 연동은 범위 밖이며(사용자 결정) 서비스 API와 명령행 도구로 같은 기능을 제공한다.

원칙: 기반이 부족하면 우회하지 않고 기반을 보강한다(사용자 지시). 동작하지 않는 기능을 완성처럼 표시하지 않는다. 상태 표기: **구현**(이번 범위에서 동작·시험), **계약**(API·스키마·포맷이 있고 호출하거나 기본값이 아닌 값을 주면 명시 오류 — `Unsupported`·`UnsupportedSetting`·`UnsupportedCapability` — 를 반환), **후속**(이번 범위에 API도 없음).

## 1. 단계와 완료 기준

| 단계 | 내용 | 완료 기준 |
|---|---|---|
| A0-P | PlatformBase: `DirectoryWatcher`, `File::Flush`, `FileSystem::Replace`, `FileLock`, `SetModifiedTime`, Windows 긴 경로(구현됨), `FileSystem::SyncDirectory`(쿠킹 게시용 디렉터리 동기화 — POSIX fsync(macOS F_FULLFSYNC), Windows는 항목 추가 권한(FILE_ADD_FILE, 관리자 불필요)으로 연 디렉터리 FlushFileBuffers, 동기화 실패는 NotDurable, wrapper 기본 Unsupported, 구현됨. `Replace`도 Windows에서 부모 디렉터리를 같은 방식으로 flush한다), `RunWithDeterministicFloatingPoint`(쿠킹 결정성용 부동소수점 환경 확인·실행·복원, 구현됨 — CoreDataFoundations 2.1절) | 기능별 시험(Windows 실행·Android 빌드, macOS 미검증 명시) |
| A0-C | Core: SHA-256(구현됨), Unicode(착수 시 최신 버전 — 2026-11 기준 18.0.0. NFC·NFD·simple case folding·General_Category, UCD 생성기), Default_Ignorable, 결정적 수학(기본 연산만 쓰는 exp2·log2·pow·SinPi·sin, 정확한 8비트 sRGB 표, 부동소수점 환경 guard), CRT 없는 숫자 변환(정확한 파서·최단 표기), JSON 읽기·쓰기(RFC 8259), `RecordList`(`WireType::RecordList`), 데이터 전용 ThirdParty 종류(C1) — 상세와 단계(C1–C6)는 [CoreDataFoundations](CoreDataFoundations.ko.md) | NIST·NormalizationTest·CaseFolding·JSONTestSuite·숫자 변환 대조·결정적 수학 기준값·RecordList 시험 |
| A0-B | 빌드·MetaCompiler(5.6절, 상세 [ReflectionFoundations](ReflectionFoundations.ko.md) B1–B8): 모듈 소유 메타데이터·모듈별 identity 명세·단위 간 import, `ROCKS_ENUM`, `ROCKS_STRUCT`(값 구조체), 배열 필드(`Array<T>`·`ObjectRefArray<T>`), `SoftObjectRef<T>` 필드, `EditorOnly`·`Required`·`Version=N`·`Name="…"` 지정자, `Sha256Digest` 값 종류, `ROCKS_CLASS(WorldOnly)`(그 밖 클래스의 `AssetLease` 멤버 거부), `AssetId` 값 종류, `ROCKS_NATIVE` 비영속 멤버, 필드 codec thunk·문맥, 툴체인 지문·도구 빌드 ID 주입, World 타입 리플렉션 이전 | 기존 시험 + 새 fixture(단계별 완료 기준은 상세 설계 11절) |
| A0-O | ObjectRuntime·ObjectSerialization(상세 [ObjectFoundations](ObjectFoundations.ko.md)): Outer·Name(생성 때 정하는 불변 값, 이름 색인·자식 고리·경로 조회), 부분 트리 `Discard`(논리 파괴 + 이름 색인·자식 고리에서 즉시 제거, 메모리는 GC), 패키지 사이 간선 관찰자 `ReferenceObserver`(순 다중도 합산 표), Registry 정책(패키지 없는 객체·이름 있는 패키지·WorldOnly 타입 거부, 패키지 정책 Registry는 outer → inner도 GC 강한 간선이라 소속 객체는 Discard로만 죽는다), 다중 root `Commit`, graph v2(호출자가 준 저장 집합·순서 = localId, 집합 밖 참조는 외부 참조 허락 hook(Import·Null·Reject) 또는 거부, 참조 값 표기(5.1절), 노드 타입은 export 표 기준, EditorOnly 제외, 누락 = 기준 형식 규칙(빠진 필드·멤버는 정책이 아니라 형식 규칙으로 허용되며 기준 값을 뜻한다)과 기준 적용 뒤 실효 값의 Required 진단([RuntimeClassMetadata](RuntimeClassMetadata.ko.md) '클래스 기본값 결정'), 단계형 복원(호출자 Commit)과 `prepare`(로드), 저장 쪽 값 치환은 B4가 구현한 `SubstituteStruct`(bulkId 재매핑 등), Commit 때 패키지 사이 간선 다중도 보고, 외부 참조 목록·번호 재배정), 한도(객체 4096 = export 상한, 외부 참조 65536, 필드 누계 1,048,576, 배열 원소 누계 4,194,304, 객체 경로·Soft 경로 4 KiB, 필드 값·그래프 바이트 64 MiB, 작업량 8 × (필드 + 원소 + 바이트)) | 그래프 왕복·외부 참조·마이그레이션·Discard·관찰자·소속 보존 시험(ObjectFoundations 9절) |
| 리플렉션 B9 | MetaSchema·ObjectRuntime·ObjectSerialization·MetaCompiler·Core(상세 [리플렉션 B9](ReflectionExtensions.ko.md)): 정수 폭·enum underlying 8종, struct `Atomic`·Core `Transform3f`·`Transform3d`, `Name`, `WeakObjectRef` PROPERTY·`ClassRef<Base>`·타입 도구 이름, `Optional`, Core `OrderedMap`·`OrderedSet`·`ObjectRefMap`·경로 key 단계, 정적 배열, 필드 플래그(DuplicateTransient·NonTryoutDuplicateTransient·SaveGame·TextExportTransient)·클래스 Transient·Deprecated·Within, 텍스트·JSON codec `ReflectionText`(대입 묶음 Prepare → Publish), `InstancedStruct`, struct 상속, `Text`, bitfield bool | 하위 단계별 시험(ReflectionExtensions 17절) |
| Curve | Core 수학·리플렉션(설계는 착수 때 — 2026-11-15 사용자 결정 "B9 직후, A0-G 앞"): UE 5.8 `FInterpCurve<T>`·`FInterpCurvePoint`(Core/Public/Math — 키별 보간 모드·접선 자동 계산·값·미분 평가·루프·최근접 탐색, 스플라인 기반) 대응 Core 수학 층(**구현됨** 2026-11-16 — `InterpCurve<T>`, [키 커브](../Implementation/InterpCurve.ko.md))과 `FRichCurve`·`FSimpleCurve`·`FRealCurve`·`FIndexedCurve`·`FKeyHandle`·`FIntegralCurve`·`FNameCurve`·`FStringCurve`·`FCompressedRichCurve`(Engine/Classes/Curves — 가중 접선·외삽·압축) 대응 값 타입과 리플렉션 미러 — **Curve 단계 CV1–CV10 완료**(2026-11-18, [커브 데이터](CurveData.ko.md) 17절): Core `KeyCurveMath`·`FrameRate`, `Curves` 모듈(`TangentCurve`·`PlainCurve`·계단 커브 셋·`CurveKeyHandleTable`·`PackedTangentCurve`), 리플렉션 struct 검증 hook·`Array<uint8>` Bytes wire·Core enum Binds, CoreTypeBindings `InterpCurve` 미러 다섯 형식·`TryCalcBounds`. 에셋 층(`UCurveFloat`·`UCurveVector`·`UCurveLinearColor`·`UCurveTable`·`FRuntimeFloatCurve`·`FCurveTableRowHandle`)은 A1 에셋 시스템 뒤 A 단계 에셋 타입이고 경계는 커브 데이터 14절이다 | 수학 층: `MathInterpCurveTests`·`MathInterpCurveGoldenTests`·`MathInterpCurveBoundsTests`·`MathKeyCurve*Tests`·`FrameRateTests`. 값 타입: `Curves/Tests`(UE 5.8 식 참조 golden·독립 고정 벡터·편집·핸들 표·압축 byte 대조·리플렉션 왕복·hook 거부 — 커브 데이터 16절) |
| A0-G | GraphicsTypes·Graphics: 차원(2D·Cube·3D·2DArray·CubeArray)·형식(R8·RG8·RGBA8·R16·RG16·RGBA16·R16F·RG16F·RGBA16F·R32F·RG32F·RGBA32F·BGRA8, sRGB 변형)·view swizzle(D3D12 `Shader4ComponentMapping`, OpenGL 채널별 `TEXTURE_SWIZZLE_R/G/B/A` — `TEXTURE_SWIZZLE_RGBA`는 OpenGL ES에 없다)·샘플러 W축 주소·필터 5종(Point·Bilinear·Trilinear·AnisotropicPoint·AnisotropicLinear — 기존 `Nearest`·`Anisotropic`의 이름 변경 포함)·**깊이 비교 샘플러**(비교 함수, 그림자용 — [렌더 기반](RenderBase.ko.md) SX13), 쿠킹 데이터 샘플러 D3D12·OpenGL. 착수 전 반영할 셰이더·렌더 요구: [셰이더 시스템](ShaderSystem.ko.md) 5.2절 G1(backend 중립 정의, Vulkan·Metal·GLES로 옮길 수 있을 것, 지원 표와 `UnsupportedCapability`)·G2(OpenGL 구현은 OpenGL ES 3.2 공통 API, 데스크톱 전용 DSA를 늘리지 않음, ES 확장 의존 형식·필터 조건)·G3(VulkanGraphics가 A0-G 범위를 받음, SH2), 텍스처 desc의 샘플러는 기본값이고 바인딩 때 다른 샘플러를 지정할 수 있게 둔다(렌더 기반 결정 R12, 바인딩 쪽은 GX2). **구현**(2026-11-18, 렌더 세션과 범위 확정 — 형식 목록은 [픽셀 형식](PixelFormat.ko.md) PF1–PF3에서 이미 끝났고, MSAA 텍스처·resolve, mip·slice 개별 view와 attachment, 바인딩 샘플러(GX6·SH2)는 렌더 세션 몫): GraphicsTypes `TextureDimension`(UE `ETextureDimension` 순서)·`CubeFace`(+X·−X·+Y·−Y·+Z·−Z, UE `ECubeFace`·D3D 면 순서와 방향)·`SwizzleSource`·`ChannelSwizzle`(`ComposeSwizzle`)·`CompareFunction`(Graphics `PipelineState.h`에서 옮겨 깊이 상태와 샘플러가 함께 쓴다)·`SamplerDesc`(`Filter` 5종 UE `ESamplerFilter` 순서, `addressW`, `comparison`·`compare`). Graphics `TextureDesc{dimension, depth, arraySize, swizzle}`·`TextureViewDesc::swizzle`·`ValidateTextureDesc`·`CheckTextureLimits`·`TextureSliceCount`·`TextureMipBytes`·`TextureDataBytes`, subresource = mip × slice(`SubresourceRange` slice 범위, `SubresourceStates`, 분리 전이 목록 지문은 slice 범위를 포함하고, begin·end 사이 접근 검사는 기록 장치(RenderBase 시험용 `RecordingDevice`)만 slice까지 본다 — 실제 backend는 이 검사를 하지 않고 D3D12 end·Vulkan begin 때 before 상태를 대조한다), 네 backend(D3D12·D3D11·Vulkan·OpenGL ES)의 차원별 생성·업로드·SRV·`ReadTexture`·전이, RenderGraph 풀 키(차원·깊이·층·swizzle)와 slice 단위 전이 기록. 지원 표는 [렌더 기반](RenderBase.ko.md) 4.4절 GX2 "구현(A0-G)"이다. 스펙과 다르게 정한 것: (1) 지원하지 않는 조합의 오류는 `UnsupportedCapability`가 아니라 Graphics의 기존 장치 능력 오류 `Unsupported`다(`UnsupportedCapability`는 A1에서 Core에 더하며, 그때 Graphics 계약을 함께 바꿀지 정한다). (2) 초기 픽셀·`UpdateTexture`·`ReadTexture`의 배치는 mip 우선(mip 하나 = 그 mip의 모든 slice·깊이 면)이다(mip별 스트리밍과 같은 단위). (3) view swizzle은 원 텍스처의 `TextureDesc::swizzle`을 이어받지 않는 view 형식 기준 대응이다. (4) D3D11은 view swizzle이 없어 항등이 아니면 `Unsupported`(셰이더 대체 없음). `AnisotropicPoint`는 하드웨어 대응이 없는 backend(D3D11, OPTIONS19 `AnisoFilterWithPointMipSupported`가 없는 D3D12)에서 UE처럼 `AnisotropicLinear`로 만든다(2026-11-18 사용자 결정 — 바꾼 사실은 `DeviceFeatures::anisotropicPointFilter` 거짓과 처음 한 번의 Debug 로그로 드러나고, 쿠킹 프로필은 UE 기본값(World 그룹 aniso + point)을 그대로 쓴다). 비교 샘플러는 깊이 형식(Depth·DepthStencil)에만 붙고(아니면 `InvalidArgument`), 그 텍스처의 view는 깊이 plane만 읽는다. (5) 비교 샘플러는 UE의 `SCF_Never`·`SCF_Less`가 아니라 `comparison` 표시 + `CompareFunction` 전체다. (6) 2D가 아닌 RenderTarget·DepthStencil과 다중 plane 형식(D24S8·D32S8·비디오)의 2D 아닌 차원은 `Unsupported`, OpenGL ES의 2D 아닌 깊이 텍스처 `ReadTexture`는 `Unsupported`다. (7) "쿠킹 데이터 샘플러 D3D12·OpenGL"은 AssetRuntime(A1)이 아직 없어 장치 계약 쪽만 했다: `TextureDesc::sampler`의 모든 값(필터·이방성·U·V·W 주소·border 색·비교)을 네 backend가 그대로 쓴다. `TexturePlatformData` → `TextureDesc` 연결은 A1·A5다 | self-test 확장(Sandbox `--self-test` RunTextureChecks: 차원별 왕복·셰이더 읽기·cube 면 방향·slice 전이, staging 업로드(배열·cube·3D, offset 0·512), BC1 배열·cube·3D 왕복, swizzle·A8 합성·view 독립, swizzle은 attachment·ReadTexture에 적용되지 않음, W축 주소, 비교 샘플러(점·선형 PCF), 필터 5종) |
| A1 | AssetRuntime: 패키지 포맷 v1, `Package`·`Asset`, 에셋 객체 Registry, 쿠킹 매니페스트(`CookedManifest.rman`), 로더(worker IO·압축 해제, Game 스레드 객체 생성, SCC), `AssetManager`(scope·PrimaryAsset·bundle), `CancelToken`·`TaskLane`·`LoadPriority`, Core ErrorCode 추가(`UnsupportedCapability`·`AlreadyRegistered`·`ScopeClosed`), EngineAssets 모듈 | 포맷·SCC·수명·scope 시험 |
| A2 | AssetDatabase(7.1절, 하위 단계 A2a–A2f): `ProjectLayout`, 경로 정책, 인덱스(검색·의존·별칭·진단·캐시·감시), 저장·dirty, 이동·복제·삭제, 임포트 프레임워크, 각 단계의 13절 명령 | 인덱스·이동·동시 쓰기 시험(하위 단계마다 A1과 같은 완료 조건 넷) |
| A3 | AssetNugget(9.1절, 하위 단계 A3a–A3c — Rockpile 레코드 계약): 키(공통 머리·세대·SCC)·Record(`.rnug` v2)·`NuggetFileSystemBackend`·`NuggetCache`(Memory 계층·세션 고정·소비 입력 대조)·일괄·비동기 요청, `NuggetRockpileBackend`는 계약 | 적중·무효화·손상·동시 기록·GC 시험 |
| A4 | AssetCooker(10.2절 — 하위 단계 A4a–A4e) | 원본 없이 쿠킹·재현성·부분 쿠킹 시험 |
| A5 | 텍스처: `Texture` 계열, `TextureSource`, ImageCore(BMP·TGA·PNG·JPEG·PSD·DDS·TIFF·Radiance HDR·무손실 OpenEXR — 12.2절), TextureTools(임포터·빌드·쿠킹 처리기), 클래스 기본값 레코드·차이 쓰기·override 집합·지문(cookKey·Summary 지문 열 — A1 Summary·A4 cookKey 산출물을 이때 고친다, RuntimeClassMetadata '클래스 기본값 결정') | 14절 텍스처 시나리오 |
| A6 | 남은 명령행(쿠킹·너겟 — A2 명령은 각 하위 단계에서 더한다)·문서·검증 보고 | 14절 결과 기록 |

## 2. 사전 조사(2026-11-12)

| 영역 | 있는 것 | 부족한 것 → 보강 |
|---|---|---|
| 객체 | `ObjectRegistry`(ContextId 문맥)·`RootRef`·`ObjectRef`(같은 Registry 안에서만)·`WeakObjectRef`·`Borrow`(다른 문맥 Registry는 WrongContext), `ConstructionContext`(스테이징한 객체를 한 번에 공개, RootRef는 첫 객체에만), `TypeId = Guid` | Outer·Name, 다중 root Commit(root마다 RootRef) → A0-O([ObjectFoundations](ObjectFoundations.ko.md)) |
| 리플렉션 | MetaCompiler: 스칼라·Guid·String·`ObjectRef`, 모든 비정적 필드 리플렉션 필수, 상속 base·참조 대상은 같은 생성 단위, 스키마 버전 1 고정, 타깃 소유 메타데이터·명세 | → A0-B([ReflectionFoundations](ReflectionFoundations.ko.md)) |
| 직렬화 | Archive·Record(TLV, 반복 필드 없음), `SaveObjectGraph/LoadObjectGraph`(단일 root, 내부 Commit, 마이그레이션 콜백, maxObjects 1024) | → A0-O graph v2(호출자 저장 집합, 단계형 복원·호출자 Commit, 외부 참조 — ObjectFoundations 5절), `RecordList` → A0-C |
| 비동기 | `WorkerScheduler`(DAG·취소), `AsyncReadFile/AsyncWriteFile`(IOCP, `Flush`) | 로더가 사용(11절) |
| 파일 | `FileSystem`, `CreateNew`, no-replace Move, `Path`, Win64는 `FILE_SHARE_DELETE` 열기 | → A0-P, VFS(APK) → 후속 |
| 해시·텍스트 | CRC32, `NameKey`, UTF-8 검증·`CodePointRange` | SHA-256·Unicode·JSON → A0-C |
| 프로세스 | 없음 | 임포트 worker process는 17절. PlatformBase `Process`·`LocalChannel`은 Rockpile R0(별도 작업 — A3는 쓰지 않는다) |
| 그래픽 | `PixelFormat` 전체(PF1–PF3), 차원 5종·view swizzle·샘플러 W축·필터 5종·비교 샘플러(A0-G 구현) | 바인딩 샘플러 → GX6 |
| 도구 | `CommandletRegistry`, `Programs/Commandlet` | `asset-*` → A6 |
| 기존 에셋 코드(미커밋) | AssetRuntime(단일 `.rasset`), AssetTools(BMP·TGA·PSD·PNG 디코더 — PNG는 zlib 대기, 설정·mip 빌드) | 대체. 디코더 → ImageCore, mip → TextureTools |
| 외부 라이브러리 | Core/ThirdParty(zlib·LZ4·Zstd), Core `Compression` 모듈 | PNG zlib·bulk Zstd에 사용. 이미지 디코더는 자체 구현 |

## 3. 개념과 모듈

| 개념 | 모듈·타입 | 상태 |
|---|---|---|
| Source File | 원본 루트(4.5절) | 구현 |
| Asset | AssetRuntime `Asset : Object`(Outer = `Package`), A5 `Texture` 계열 | 구현 |
| Asset Package | AssetRuntime `Package : Object`, `PackageWriter`·`PackageReader` | 구현 |
| Importer | AssetDatabase `Importer`, TextureTools `TextureImporter` | 구현(텍스처) |
| Import Metadata | 에디터 패키지 `ImportRecord` 섹션 | 구현 |
| Asset Registry(인덱스) | AssetDatabase `AssetIndex` | 구현 |
| 에셋 너겟 | AssetNugget | 구현(로컬 — `NuggetFileSystemBackend`), `NuggetRockpileBackend`는 계약(Rockpile 서버 R0–R2는 별도 — [Rockpile](Rockpile.ko.md)), C++ 직접 원격 backend 없음 |
| Cooker | AssetCooker | 구현(비압축 형식), 블록 압축 계약 |
| Runtime Asset Loader | AssetRuntime `AssetLoader` | 구현(CPU ready), `Render` ready 계약 |
| Asset Manager | AssetRuntime `AssetManager` | 구현(scope·PrimaryAsset·bundle·즉시 해제), 예산 LRU 해제 후속 |
| 메시·스켈레톤·애니메이션 에셋 | `MeshSource`·`Mesh`·`Animation`(Runtime), `MeshTools`·`AnimationTools`(Developer) | `MeshSource` 구현(MA1 — 원본 메시 저장·검증·위상 조회, bulk 쓰기는 `MemoryBulkSource`), `Mesh`(빌드 설정 값 타입 `MeshBuildSettings`·`SkinnedMeshBuildSettings`와 단순 충돌 값 타입 `SimpleCollision`·검증 — MA3a)·`MeshTools`(정적 메시 LOD 빌드 순수 함수 `BuildStaticMeshLod`, ThirdParty MikkTSpace 패치) 구현(MA2a — 지금 의존은 Mesh → ObjectRuntime·CoreTypeBindings·AssetRuntime, MeshTools → MeshSource·Mesh·Animation·DeterministicMath·MikkTSpace, 아래 그래프의 나머지는 MA2b 이후), 스킨 메시 LOD 빌드 순수 함수 `BuildSkinnedMeshLod`(영향 줄이기·bone map·inverse-bind·morph 변화량) 구현(MA5a), `Animation`(본 계층 `BoneHierarchy`·`Skeleton` 에셋 클래스·검증·병합 판정 `MergeBoneHierarchy`) 구현(MA4a), 클립 `AnimationAsset`·`AnimationClip`·원본·쿠킹 데이터 v1·쿠킹 변환 `BuildClipPlayback` 구현(MA6a — 지금 의존은 Animation → AssetRuntime·CoreTypeBindings·Curves), 나머지 설계([MeshAnimationAssets](MeshAnimationAssets.ko.md) 8절·3.7·4.8·5.4·6.4·6.5·7.5절, MA2b·MA4b·MA5b·MA6b·MA7) |
| 외부 엔진 교환 임포터 | 엔진 플러그인 `UnrealExchange`(Developer) | UX1a 리더·경로 대응·임포트 순서 구현, 나머지 설계([Unreal 교환 형식](UnrealExchange.ko.md) 10절, UX1–UX8) |

이름 구분: 에셋 객체가 사는 `ObjectRegistry`는 **에셋 객체 Registry**, 에디터 패키지 인덱스는 **AssetIndex**(UE AssetRegistry 대응), 쿠킹 결과의 런타임 목록은 **쿠킹 매니페스트**(`CookedManifest.rman`, 10절)라 부른다.

의존 방향(→는 의존):

```
Programs/Commandlet → TextureTools → { AssetCooker, AssetDatabase, AssetNugget, ImageCore, Texture }
AssetCooker → { AssetDatabase, AssetNugget }        AssetDatabase(Developer) → { AssetRuntime, HostConfig }
Texture(Runtime) → { AssetRuntime, GraphicsTypes }       ImageCore(Developer) → { Core, Compression }
AssetRuntime → { ObjectSerialization, ObjectRuntime, Serialization, Task, Compression }
EngineAssets(Runtime, 신규) → { Engine, AssetRuntime }   — AssetManager를 EngineSubsystem으로 붙이는 연결 모듈
MeshSource(Runtime) → { AssetRuntime }                  Animation(Runtime) → { AssetRuntime, Curves }      (설계, MeshAnimationAssets 8절)
Mesh(Runtime) → { AssetRuntime, MeshSource, Animation, Material, GraphicsTypes }
MeshTools(Developer) → { Mesh, MeshSource, AssetCooker, AssetDatabase, AssetNugget }   AnimationTools(Developer) → { Animation, AssetCooker, AssetDatabase, AssetNugget }
UnrealExchange(플러그인, Developer) → { AssetDatabase, TextureTools, MeshTools, AnimationTools, MaterialCompiler, Material }   (설계, UnrealExchange 10.1절)
```

AssetRuntime은 Engine을 모른다. 도구는 Engine을 링크하지 않고 `AssetManager`를 직접 소유한다. 게임은 `EngineAssets`가 AssetManager를 `EngineSubsystem`으로 등록하고 프레임 단계(로드 결과 전달·에셋 객체 Registry GC 한 조각)를 연결한다. World는 AssetRuntime에 의존하지 않는다(6.3절).

## 4. 식별과 경로

### 4.1 Asset ID와 객체 이름

- `AssetId`(128비트 Guid, 무작위 v4)는 대표 에셋과 그 패키지의 영속 ID다(패키지당 대표 에셋 하나). 생성 때 발급, 저장·이동·이름 변경·재임포트 후 유지, 복제 시 새로 발급. `AssetId`·`SoftObjectRef<T>` 값 타입은 Core(ObjectRuntime)에 둔다(리플렉션이 필드 타입으로 인식).
- 객체 이름: 대표 에셋은 빈 이름, 하위 객체는 같은 Outer 안에서 유일한 이름(계획 §5.2)이며 규칙은 경로 구성 요소(4.2절)와 같다(`.`·`/` 불가). 패키지 안 객체 경로는 Outer 체인 이름을 `.`로 이은 것이다(예: 대표 에셋 아래 `Body` 아래 `Socket` = `Body.Socket`). 하위 객체는 `<AssetId, 객체 경로>`로 식별한다. 하위 객체 이름 변경 안정성은 SubobjectGuid(계획 §5.3, 후속).
- 중복 ID는 덮어쓰지 않고 `DuplicateAssetId`로 보고하며 그 ID 조회는 오류를 반환한다. 해결은 `asset-fix-duplicate-id <path>`(지정한 쪽에 새 ID)로만 한다.

### 4.2 논리 경로

- 형식 `/<Mount>/<Dir>/.../<Name>`. `/Game` = 프로젝트 `Content/`, `/Engine` = 엔진 콘텐츠(후속). 하위 객체 경로는 `<PackagePath>.<객체 경로>`(예: `/Game/Hero.Body.Socket`).
- **진실의 원천은 파일 위치 하나다.** 에디터 패키지는 경로를 저장하지 않고 대표 에셋 이름은 파일 이름에서 유도한다. 쿠킹 매니페스트(10절)는 경로 힌트를 담으며 런타임의 경로 조회(`FindByPath`)는 이 힌트로 AssetId를 찾는 편의 기능이다(영속 참조는 항상 ID).
- 정규화: 구분자 `/`만(입력의 `\`는 거부), 앞 `/` 필수, 끝 `/`·빈 구성 요소·`.`·`..` 금지. 문자열은 NFC로 정규화하며 비교 키는 `TryMakeCaselessKey` = NFC(simple case folding(NFD(x)))다(Unicode §3.13 정준 caseless 일치의 simple folding 판, 버전은 A0-C). 파일 열거 이름도 NFC로 바꿔 비교한다(macOS NFD 대응). NTFS는 자체 대문자 표($UpCase)를 쓰므로 키가 다른데 NTFS가 같게 보는 드문 이름 쌍은 저장 시 `CaseCollision` 진단으로 잡는다.
- 구성 요소: 1–64 byte UTF-8(NFC). 허용: General_Category L*·N*·M*(단, 첫 문자는 L*·N*), `_`, `-`. 거부: Default_Ignorable_Code_Point(보이지 않는 문자 — L*·M*이어도), 그 밖의 문자(공백·제어·구두점·기호 포함 — 따라서 `.`·`,`·`;`·`\`도 없다), Windows 예약 이름(대소문자 무시 `CON PRN AUX NUL COM0-9 LPT0-9`, `COM¹²³ LPT¹²³`).
- 길이: 논리 경로 ≤ 256 byte. `ProjectLayout`이 OS 경로(임시 접미사 포함) 길이를 검사한다. Windows 긴 경로는 PlatformBase가 지원한다(A0-P).
- 정렬: 이름·경로의 결정적 정렬은 NFC UTF-8 byte 순, Guid는 (high, low) 부호 없는 수치 순이다(5·10절의 모든 정렬).
- 파일: 에디터 `<Project>/Content/<path>.rasset`, 쿠킹 `<쿠킹 루트>/Packages/<a>/<A>-<h>.rpkg`. A = AssetId의 36자 소문자 표기(Core Guid `{}` 서식, RFC 9562), a = A의 앞 2자, h = 패키지 SHA-256의 앞 16자(소문자 hex). 내용 주소 이름이라 같은 이름은 같은 내용이다(10절 게시).

### 4.3 경로 별칭

- 사람이 쓴 텍스트 경로는 별칭 표 "옛 경로 → AssetId"로 풀린다(계획 §9.1). 이동·이름 변경 때 추가하고 풀릴 때마다 `AliasResolved` 경고. 위치: `Config/AssetAliases.json`(버전 관리 대상, 팀 공유, 원자적 기록, 항목 정렬로 병합 충돌 최소화). 패키지 재검색으로는 복구되지 않는 사용자 데이터이므로 버전 관리가 백업이다(계획 문서의 "재검색으로 복원되는 캐시" 표현은 17절에서 고친다).
- 만료: 항목은 생성 UTC와 만료 일수(기본 90, 설정)를 가진다. `asset-alias-prune`이 만료 항목을 지우며, 지우기 전 텍스트 참조 검색(`Config/`·스크립트)에서 쓰인 곳을 보고한다. `asset-alias-list`·`asset-alias-remove` 제공.

### 4.4 작업

| 작업 | 방법 | 상태 |
|---|---|---|
| 이동·이름 변경(디렉터리 포함) | 잠금 → 대상 없음 확인 → `Move(no-replace)`(디렉터리면 디렉터리 한 번, 대소문자만 바꾸는 이름 변경 허용) → 별칭 추가(디렉터리면 안의 모든 패키지) → 인덱스 갱신. ID·참조 무변경 | 구현 |
| 복제 | 새 경로에 새 ID로 기록(하위 객체·내부 참조 재매핑, 계획 §5.3) | 구현 |
| 삭제 | 참조자가 있으면 거부, `--force`면 삭제 후 참조자에 `MissingDependency` | 구현 |
| 중복 ID 해결 | `asset-fix-duplicate-id` | 구현 |

읽기 전용으로 연 패키지(5.3절)는 저장·쿠킹·복제·ID 수정을 모두 `NewerVersion`으로 거부한다(모르는 내용을 잃는다).

### 4.5 원본 경로

원본은 이름 있는 원본 루트 기준 상대 경로(`{Project}/SourceArt/Checker.png`)로 기록한다. 루트 목록은 `Config/AssetSources.json`(이름 → 경로, 기본 `Project`). 프로젝트 밖 원본은 절대 경로로 기록하고 `AbsoluteSourcePath` 경고. 재임포트는 기록한 경로를 현재 루트로 풀고, 없으면 `MissingSource`로 실패·기존 보존. 재지정은 `--source=`(원본 하나) 또는 `--source.<역할>=`(원본 여러 개, 역할은 ImportRecord `sources[].role`)로 한다(13절).

### 4.6 진단

| 진단 | 뜻 |
|---|---|
| `DuplicateAssetId`, `DuplicatePath`, `CaseCollision`, `InvalidPath`, `MissingFile`, `UnregisteredFile` | 식별·경로 |
| `TypeMismatch`, `MissingDependency`(Hard·Soft), `UnsupportedCycle`, `UnpackagedReference`(패키지에 속하지 않은 객체를 저장 그래프가 참조) | 참조 |
| `CorruptPackage`, `UnsupportedVersion`, `NewerVersion`, `ExternalModification`, `NotDurable`(경고 — 교체는 됐고 디렉터리 동기화만 실패, 5.5절) | 패키지·저장 |
| `MissingSource`, `SourceChanged`, `InvalidSource`(원본 해석 실패), `UnsupportedSourceFormat`(지원하지 않는 원본 변형, 예: PSD CMYK), `LossySource`, `NonSrgbSource`, `IgnoredSourceChannels`(경고), `IgnoredSourceChunks`(경고 — 오디오 임포터, [오디오 시스템](AudioSystem.ko.md) 3.10.2절), `AbsoluteSourcePath` | 원본 |
| `ImporterOutdated`, `InvalidImportOption`, `OrphanedOutput`, `UnknownProperty`(모르는 속성 이름), `UnsupportedSetting`(계약 설정의 기본값이 아닌 값 — MA2a 메시 빌드가 보존 설정에 처음 내며 `AssetDiagnosticCode`에 등록), `InvalidSetting`(모순된 조합·숨김 값·쓸 수 없는 값 — 같이 등록, 메시 `buildScale`의 0·비유한 성분, 스킨 빌드 임계값의 음수·비유한 값과 음수 `boneInfluenceLimit`), `SrgbIgnored`(경고 — 선형 설정·float 출력의 SRGB), `PreserveBorderWithWrap`(경고) | 임포트·설정 |
| `CorruptEntry`(에셋 너겟 항목·게시 패키지 파일 손상), `IncompatibleCook`(부분 쿠킹 병합 불가·런타임 프로파일 불일치), `NeverCookReferenced` | 에셋 너겟·쿠킹 |
| `AliasResolved`, `RootLost`(감시 루트 소실) | 그 밖 |
| `InvalidMeshData`, `NonFiniteAttribute`, `UvOutOfRange`, `NonNormalizedWeights`, `TooManyInfluences`, `InfluencesTrimmed`, `MissingInfluences`(경고), `MaterialSlotFallback`, `NegativeBuildScale`, `UnsupportedCollisionShape`, `InvalidCollisionData`, `InvalidSkeletonData`, `SkeletonMismatch`, `UnknownBoneTrack`, `InvalidClipData`, `EventPropertiesDropped` | 메시·애니메이션([MeshAnimationAssets](MeshAnimationAssets.ko.md) 11절 — 앞 다섯 코드는 MA1이, `MaterialSlotFallback`·`NegativeBuildScale`은 MA2a가, `InvalidSkeletonData`·`SkeletonMismatch`는 MA4a가, `InfluencesTrimmed`(정보)·`MissingInfluences`는 MA5a가, `InvalidCollisionData`·`UnsupportedCollisionShape`(임포터 경고)는 MA3a가, `InvalidClipData`·`UnknownBoneTrack`·`EventPropertiesDropped`는 MA6a가 `AssetDiagnosticCode`에 등록, 나머지는 설계) |
| `DuplicateLevelScript`(`levelScript` 말고 또 있는 LevelScript 파생 Actor), `LevelScriptNotInLevel`(`levelScript`가 `actors` 원소가 아님), `CrossLevelReference`(레벨에 저장하는 참조가 다른 Level, 또는 영속 Entity 참조 전 다른 셀의 객체를 가리킴), `NotPlaceableClass`(편집기 배치 명령이 `NotPlaceable` 클래스를 받음), `LevelScriptCollider`(LevelScript의 Collider component) | 레벨(설계 — [레벨 스크립트](LevelScript.ko.md) 4.9절. 저장·쿡은 InvalidArgument, cooked 로드는 CorruptPackage) |
| `CorruptBundle`, `UnsupportedBundleVersion`, `UnknownField`, `UnsupportedAsset`, `UnsupportedSourceLayout`, `GammaMismatch`, `GeneratedLodBaked`, `BakedColorQuantized`, `UnrealPathRenamed`, `UnrealPathTooLong`, `UnrealPathCollision`, `MissingUnrealDependency`, `UntranslatedMaterialNode`, `MaterialBakedFallback`, `ParameterDefaultConflict`, `ParameterResolveMismatch` | Unreal 교환 임포트([Unreal 교환 형식](UnrealExchange.ko.md) 10.3절). `CorruptBundle`·`UnsupportedBundleVersion`·`UnknownField`·`UnsupportedAsset`·`UnrealPathRenamed`·`UnrealPathTooLong`·`UnrealPathCollision`·`MissingUnrealDependency`는 UX1a 리더가 내며 `AssetDiagnosticCode`에 등록(2026-11-21, 10.4절), 나머지는 설계 |

## 5. 패키지 포맷 v1

### 5.1 헤더·섹션 표·물리 배치

모든 정수 little-endian, Guid는 high·low u64(Archive 규칙). 첫 64 byte 가운데 magic·formatVersion·headerBytes·flags(offset 0–11)와 offset 60의 headerCrc 위치는 모든 포맷 버전에서 고정이다(버전 판정 전에 headerCrc와 flags를 검사하므로).

```
offset size 필드
0      4    magic "RPKG"
4      2    formatVersion (1)
6      2    headerBytes (64)
8      4    flags: Editor(1) | Cooked(2) 중 정확히 하나
12     4    sectionCount (1–32)
16     8    sectionTableOffset (8 정렬)
24     8    fileBytes = sectionTableOffset + 32 × sectionCount + 4  (Stat 크기와 같아야 한다)
32     16   assetId
48     4    bulkAlignment (2의 거듭제곱, 8–65536; 에디터 64, 쿠킹 = 프로파일 값, 기본 4096)
52     8    reserved (0)
60     4    headerCrc = CRC32(byte 0–59)
섹션 표(파일 끝): sectionCount × { u32 kind, u32 flags, u64 offset, u64 bytes, u32 crc32, u32 schemaVersion } + u32 tableCrc
```

- 섹션 flags: `Required`(1) — 이 섹션을 모르는 reader는 패키지를 해석할 수 없다. `PerEntryChecksum`(2) — 섹션 표의 crc32 칸은 0이고 entry별 CRC로 검증(Bulk). 정의되지 않은 비트는 `CorruptPackage`. 알려진 kind의 flags는 5.2절 표로 고정이다.
- 물리 순서는 kind 오름차순으로 고정한다: Summary(1) → Exports(2) → ObjectImports(3) → Dependencies(4) → Objects(5) → ImportRecord(6) → Tags(7) → Bulk(8) → BulkIndex(9) → 섹션 표. BulkIndex를 Bulk 뒤에 두어 스트리밍 기록 중 확정되는 압축 크기·CRC를 마지막에 쓴다. 빈 섹션 규칙은 5.2절 표(같은 내용 → 같은 바이트).
- 검증 순서와 진단:
  1. 크기 ≥ 64, magic, headerCrc, flags(Editor·Cooked 중 정확히 하나, 정의되지 않은 비트 없음 — 버전 판정이 이 값에 달렸다). 실패는 `CorruptPackage`(버전 필드를 믿을 수 없다).
  2. `formatVersion`: 5.3절 규칙(`NewerVersion`·`UnsupportedVersion`).
  3. 나머지 헤더·섹션 표: `headerBytes` = 64, bulkAlignment 범위, 필수 섹션(5.2절 "항상 있음")의 존재와 생략 규칙이 있는 섹션의 비어 있지 않음, `fileBytes` 식과 Stat 크기 일치, tableCrc, kind 중복 금지, 물리 순서 = kind 순서, 각 섹션 `offset ≥ 64`·`offset ≤ sectionTableOffset`·`bytes ≤ sectionTableOffset − offset`, offset은 8(Bulk는 bulkAlignment) 정렬, 겹침 금지, 패딩은 정렬 공간뿐이며 0. 위반은 `CorruptPackage`.
  4. 섹션별: 모르는 kind는 Required면 에디터 `NewerVersion`·쿠킹 `UnsupportedVersion`이다. Required가 아니면 에디터는 그 섹션을 건너뛰고 읽기 전용으로 열고 쿠킹은 `UnsupportedVersion`이다. 아는 kind의 flags가 표와 다르면 `CorruptPackage`, `schemaVersion`은 5.3절 규칙. 섹션 crc32(PerEntryChecksum 제외)·내용 검증 위반은 `CorruptPackage`.
- 상한: 파일 64 GiB, 비-Bulk 섹션 64 MiB, export 4096, object import 65536, 문자열 4 KiB. 위반은 `CorruptPackage`.
- export와 참조 표기:
  - export 집합은 Outer 체인이 이 패키지에 닿는 모든 객체다(Outer 표에서 정한다 — 참조 순회로 정하지 않는다). 저장 불가 객체(클래스가 Transient·Deprecated인 객체와 그 부분 트리)는 `IsUnsaveableObject`로 걸러 빼고, 그 객체를 가리키는 참조는 null + 진단으로 저장한다(graph 저장 정책 Exclude — [리플렉션 B9](ReflectionExtensions.ko.md) 9.2절). 패키지 정책 Registry의 자식 고리가 소속 객체를 붙잡으므로 이 집합은 GC 시점과 무관하다([ObjectFoundations](ObjectFoundations.ko.md) 1.7절). export index는 1부터(0 = 패키지 자신). 저장 순서는 대표 에셋, 이후 Outer 깊이 우선이며 같은 Outer 안은 이름 순(4.2절 정렬)이다. writer가 이 순서를 정해(`CollectInners(DepthFirst)`) graph v2에 넘기고 graph v2의 localId k = export k다. graph v2 노드 표의 outer 0은 Exports outerIndex 0과 같이 패키지(기준 Outer)다(A0-O — ObjectFoundations 5.1절).
  - export 표의 유일성: export 1이 대표 에셋이고 그것만 Primary 표시·빈 이름·outerIndex 0을 가진다. 나머지 export는 outerIndex ≥ 1(자기 index보다 작다)이고 같은 Outer 안에서 이름이 유일하다. 위반은 `CorruptPackage`.
  - object import 표의 유일성: outerImportIndex가 0이 아니면 자기 index보다 작고 같은 assetId의 이름 있는 항목을 가리켜야 하며, 5.2절 순서(같은 outer 아래 이름 엄격 오름차순)가 (assetId, 객체 경로)의 중복을 막는다. 위반은 `CorruptPackage`.
  - WeakObjectRef 필드는 같은 패키지(그래프) 안 대상만 보존하고 밖 대상은 null + 진단으로 저장한다(weak import 없음 — ReflectionExtensions 5.1절).
  - 패키지 밖 객체로의 참조는 object import다. 다른 패키지에 속하면 import, 어떤 패키지에도 속하지 않으면 저장을 `UnpackagedReference`로 거부한다.
  - Objects 섹션의 참조 값(u32): 0 = null, 1–0x7FFFFFFF = export index, 0x80000000 | i = object import index i(1부터). export 수·import 수를 넘는 값과 0x80000000(i = 0)은 `CorruptPackage`. Exports의 outerIndex도 export index다(0 = 패키지).
  - 객체 타입의 원천은 Exports.typeId 하나다. graph v2 노드는 타입을 따로 쓰지 않고 export를 가리키며, 모든 export의 typeId는 Summary의 타입 버전 표에 있어야 한다(없으면 `CorruptPackage`).
- CRC32는 우발적 손상 검출용이며, 콘텐츠 동일성은 SHA-256, 변조 방지는 범위 밖이다.

### 5.2 섹션 내용

| kind | 내용 | flags | 빈 경우 | Editor | Cooked |
|---|---|---|---|---|---|
| Summary(1) | Record: 타입 스키마 버전 표(TypeId → 버전, TypeId 정렬 — 클래스 행에는 저장 당시 클래스 기본값 레코드 지문 열, A5, RuntimeClassMetadata '클래스 기본값 결정'), 커스텀 버전(Guid → u32), Editor면 savedByEngineVersion(주·부·패치)·compatibleWithEngineVersion(주·부·0, 5.3절 — 쿠킹 패키지에는 없다), Cooked면 프로파일 id·프로파일 출력 설정 SHA-256·쿠커 버전 | Required | 항상 있음 | ○ | ○ |
| Exports(2) | `{ name(대표는 빈 문자열), typeId, outerIndex, flags(Primary·EditorOnly) }` | Required | 항상 있음 | ○ | ○(EditorOnly 제외) |
| ObjectImports(3) | `{ assetId, outerImportIndex(0 = 그 패키지의 대표 에셋을 Outer로 하는 경로의 시작), name(빈 이름 = 대표 에셋), typeId }`. 깊은 하위 객체는 outer import를 거쳐 객체 경로를 이룬다(UE import 표와 같은 방식). 순서: assetId 오름차순, 같은 에셋 안은 깊이 우선(outer는 바로 앞 항목이거나 그 outer 사슬의 항목, 또는 0)이고 같은 outer 아래 이름은 byte 엄격 오름차순(중복 금지 — 대표 에셋 항목은 빈 이름이라 그 에셋의 첫 항목). 이름은 객체 이름 규칙(4.2절 구성 요소)을 따르고 객체 경로는 4 KiB 이하(5.7절 13) | Required | 생략 | ○ | ○ |
| Dependencies(4) | `{ assetId, kind(Hard·Soft·EditorOnly·Build·ImportOwner), expectedTypeId, required }` (assetId, kind) 정렬·중복 제거. Hard·Soft·EditorOnly는 저장 결과의 외부 참조 목록(A0-O)에서, Build는 저장 전에 호출한 `DependencyDeclarer`(16.2절)가 준 목록에서, ImportOwner는 ImportRecord `ownerAssetId`(자기 자신이 아니면)에서 만든다. 로드 때 Hard·Soft·EditorOnly는 Objects 디코딩 결과와, ImportOwner는 ImportRecord와 대조하며(A2 — 5.7절 17, Build는 대조하지 않음) 불일치는 `CorruptPackage` | Required | 생략 | ○ | ○(EditorOnly·Build·ImportOwner 제외) |
| Objects(5) | ObjectSerialization graph v2 바이트 | Required | 항상 있음 | ○ | ○ |
| ImportRecord(6) | Import Metadata(5.4절) | — | 생략(임포트하지 않은 에셋) | ○ | × |
| Tags(7) | 인덱스 검색 태그 Record(타입이 정한 키→값, 키 정렬) | — | 생략 | ○ | × |
| Bulk(8) | entry 본문(bulkAlignment 정렬, PerEntryChecksum) | Required·PerEntryChecksum | 생략(entry 없음) | ○ | ○ |
| BulkIndex(9) | `{ bulkId(1부터 연속), flags(EditorOnly), offset(Bulk 섹션 시작 기준), storedBytes, rawBytes, codec(None=0·Zstd=1 — Zstd 저장 byte는 블록 컨테이너, 5.7절 6), crc32(저장 바이트), sha256(raw) }`. 검증: bulkId 순이며 offset도 증가, entry는 Bulk 섹션 안에서 겹치지 않고 entry 사이에는 bulkAlignment 정렬 패딩(0)만 있다, codec None이면 storedBytes = rawBytes, 쿠킹 패키지의 EditorOnly 표시는 0, Objects의 모든 `BulkDataRef`가 가리키는 bulkId가 있고 rawBytes·SHA-256이 같다 | Required | Bulk와 함께 생략 | ○ | ○ |

### 5.3 버전 축

| 축 | 위치 | 에디터 패키지 | 쿠킹 패키지 |
|---|---|---|---|
| 컨테이너 | `formatVersion` | 현재보다 낮으면 읽으며 마이그레이션, 높으면 `NewerVersion`(해석할 수 없는 컨테이너라 열지 않음) | 정확히 일치, 아니면 `UnsupportedVersion`(다시 쿠킹) |
| 섹션 | `schemaVersion` | 낮으면 마이그레이션, 높으면 그 섹션이 Required면 `NewerVersion`, 아니면 건너뛰고 읽기 전용 | 정확히 일치 |
| 타입 데이터 | Summary 타입·커스텀 버전·클래스 기본값 지문(A5), 마이그레이션 콜백 | 낮으면 마이그레이션, 높거나 모르는 필드가 있으면 읽기 전용으로 열고 저장은 `NewerVersion` 거부. 이 모드에서는 Dependencies 대조를 부분집합 검사로 바꾼다(모르는 필드 안의 참조는 디코딩 결과에 없으므로 "디코딩 결과 ⊆ Dependencies"만 본다). 클래스 기본값 지문이 다르면 "기본값이 바뀐 타입·영향받은 객체"를 진단하고 계속 연다(RuntimeClassMetadata '클래스 기본값 결정') | 정확히 일치(클래스 기본값 지문이 다르면 `UnsupportedVersion`, 다시 쿠킹 — 쿠킹 패키지도 기준 차이 형식이다) |
| 엔진 | Summary savedByEngineVersion·compatibleWithEngineVersion(에디터만, [ReflectionFoundations](ReflectionFoundations.ko.md) 8.2절) | compatibleWith의 주·부가 현재 엔진보다 새면 `NewerVersion`(`EngineVersion::CanLoadPackageCompatibleWith`가 거짓 — 더 새 엔진이 저장한 패키지). 같거나 오래되면 엔진 버전으로는 거부하지 않고 위 세 축이 판정한다. 패치는 보지 않는다. savedBy는 진단용이다 | 기록하지 않는다(엔진 버전으로 다시 쿠킹을 요구하지 않는다 — 위 세 축과 쿠커 버전이 판정한다) |

버전을 올릴 때 옛 fixture 3개 이상을 읽고 다시 저장하는 시험을 둔다(계획 §7).

타입 축 판정은 등록 저장소 조회 결과로 한다. 미등록(NotFound)과 클래스가 아닌 타입(TypeMismatch)은 "현재 버전 없음"(쿠킹 `UnsupportedVersion`, 에디터 읽기 전용 — `AssetPackage_NonClassTypeHasNoCurrentVersion`)이고, 그 밖의 조회 실패(종료된 저장소의 ShuttingDown 등)는 파일 판정이 아니므로 그 오류를 그대로 돌려준다(진단 코드 None — `AssetPackage_ClosedTypeRegistryIsNotVersionVerdict`). 처음 구현은 OutOfMemory만 전달하고 나머지를 미등록으로 보았는데, 조회는 할당하지 않아 그 분기는 닿지 않았고 ShuttingDown이 버전 판정으로 바뀌었다.

엔진 버전 축(A1): writer는 엔진 버전을 인자로 받는다(호출자는 `build::CurrentEngineVersion()`을 넘기고, 시험은 고정 값을 넘겨 golden이 버전 올림에 흔들리지 않게 한다). savedByEngineVersion은 그 값, compatibleWithEngineVersion은 `CompatibleBaseline()`(주·부·0 — 패치는 데이터 축과 공개 API를 바꾸지 않는다)이다. 쿠킹 패키지 Summary에는 둘 다 기록하지 않는다(UE도 쿠킹·procedural 저장에서 SavedBy·CompatibleWith를 비운다 — 쿠킹 byte·내용 주소 이름의 결정성, 도구 빌드 ID를 뺀 이유와 같다).

### 5.4 Record 목록과 Import Metadata

목록은 Record의 `WireType::RecordList` 필드다(payload = `u32 count + count × (u64 length + 중첩 Record)`, 빈 목록도 count 0으로 기록 — A0-C, Serialization 모듈).

```
ImportRecord (스키마 Guid, 버전 1)
  importerId: Guid, importerName: string, importerVersion: u32
  sources: RecordList[ { role, root, relativePath, sha256(32B), bytes } ]
  options: Record(임포터별 스키마·버전 — 디코딩·원본 변환 옵션만)
  outputs: RecordList[ { outputKey, assetId, typeId } ]
  ownerAssetId: Guid   — 다중 출력의 Import 기록 소유 패키지. 자기 자신이면 전체 기록, 아니면 sources·options·outputs는 비고 소유자를 가리킨다(Dependencies에 ImportOwner로 남아 소유자 삭제를 막는다)
```

원본 역할(`role`): 2D `source`, Cube 면 `px`·`nx`·`py`·`ny`·`pz`·`nz`, 2DArray·Volume slice `0`·`1`…, CubeArray `<i>.<면>`. 옵션 경계: 디코딩·원본 변환 옵션 = ImportRecord.options(바뀌면 재임포트), 빌드·쿠킹 설정 = 에셋 저장 속성(바뀌면 재빌드, 너겟 키 변경).

### 5.5 저장

1. 잠금(8절). 대상이 있으면 로드 때 기록한 지문(헤더 + 섹션 표 SHA-256)과 비교, 다르면 `ExternalModification`.
2. 같은 디렉터리에 `<name>.rasset.<guid 36자>.tmp`를 `CreateNew`.
3. Objects를 먼저 메모리에 직렬화한다(그 결과의 외부 참조 목록이 있어야 ObjectImports·Dependencies를 만든다). 그다음 섹션을 물리 순서대로 스트리밍 기록하며 CRC를 증분 계산한다. bulk는 블록 단위로 압축·CRC·SHA-256을 계산하며, 원본은 패키지 bulk 저장소(6.1절 — 메모리 raw 데이터이거나 아직 읽지 않은 entry면 이전 패키지 파일을 경로로 다시 열어 읽는다)다. 이전 파일에서 옮기는 entry는 저장 crc32와 raw SHA-256을 검증하며 다르면 저장을 `CorruptPackage`로 실패시킨다(손상이 새 CRC로 세탁되지 않게). bulkId는 저장마다 객체·필드 순서로 1부터 다시 매기고 writer가 (옛 id → 새 id) 재매핑 표를 만든다. BulkIndex·섹션 표를 쓰고 offset 0으로 돌아가 헤더를 쓴다. 메모리 피크(객체와 이미 상주한 raw 데이터 제외)는 Objects 바이트 + 다른 비-Bulk 섹션 하나 + bulk 블록 하나다(비-Bulk 섹션은 각각 64 MiB 상한).
4. `File::Flush` → Close.
5. 고정 크기 버퍼로 다시 읽어 헤더·표·섹션·entry CRC를 검증한다(writer 버그·짧은 쓰기 검출. 페이지 캐시를 읽으므로 매체 손상 검출이 목적은 아니다).
6. `FileSystem::Replace(temp, target)`(구현, `Platform/File.h` 계약). Windows: `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)`(다른 쪽이 `FILE_SHARE_DELETE`로 연 대상도 교체 — 이전 파일의 열린 핸들도 계속 유효하다). 이 정보 클래스가 없는 볼륨(FAT·일부 네트워크 FS)은 `MoveFileExW(REPLACE_EXISTING)`로 대신하며, 그 볼륨에서 열린 대상은 `Busy`·`AccessDenied`로 끝나고 대상은 바뀌지 않는다. `ReplaceFileW`는 쓰지 않는다(이전 대상의 속성·스트림을 합치고, `ERROR_UNABLE_TO_MOVE_REPLACEMENT`에서 대상을 지운 채 남긴다). Windows의 이름 변경 매체 기록은 NTFS 메타데이터 로그에 맡긴다(반환 직후 전원 차단 시 이전 상태일 수 있다). POSIX: 부모 디렉터리를 먼저 열고 `rename` 후 그 디렉터리를 `fsync`(macOS `F_FULLFSYNC`). 오류: 원본이 일반 파일이 아니거나 대상이 디렉터리·link면 `WrongType`, 대상이 읽기 전용이거나 권한이 없으면 `AccessDenied`, 공유 위반이면 `Busy`.
   교체 직전에 bulk 저장소가 캐시한 이전 파일 핸들이 있으면 닫는다(POSIX 의미 rename이 없는 볼륨의 `MoveFileExW`는 자기 핸들이 열려 있으면 실패하므로 모든 경로에서 같은 순서로 한다). 교체가 실패하면 이후 저장은 1단계의 지문 비교로 대상이 로드 때의 것인지 다시 본다(다르면 `ExternalModification` — 교체 직후 따로 확인하지 않는다, 7.1절 15 A2c2b).
7. 실패하면 임시 파일을 지우고 보고(기존 보존). `NotDurable`(POSIX 디렉터리 동기화만 실패)은 교체가 이미 보이는 상태이므로 임시 파일 정리 없이 저장 성공으로 처리하고 `NotDurable` 경고를 남긴다. 성공(경고 포함) 후: 저장 세대와 무관하게 패키지 bulk 저장소의 "메모리 id → entry" 표를 재매핑 표(옛 id → 새 id)로 새 파일 entry에 다시 잇는다(아직 읽지 않은 entry도 새 파일에 있다 — 객체의 `BulkDataRef` 필드는 바꾸지 않는다, 7.1절 15). 직렬화를 시작한 세대 = 현재 세대일 때만 dirty를 지운다(저장 중 편집이 있었으면 dirty 유지 — 편집으로 새로 생긴 raw 데이터는 메모리에 있으므로 다음 저장이 쓴다). 마지막으로 인덱스를 갱신. 파일에 쓰는 새 id는 저장 직렬화의 값 치환 hook(B4가 구현한 `SubstituteStruct` — graph v2가 쓴다)이 직렬화 바이트에 쓰고, 같은 옛 id를 가리키는 여러 참조는 같은 새 id가 된다.
8. 잠금 아래에서 발견한 `<name>.rasset.<guid 36자>.tmp` 형태의 파일만 고아로 지운다. 재검색·감시는 `.tmp`를 무시. 교체는 이름만 바꾸므로 새 파일은 이전 대상의 ACL·속성·대체 스트림을 승계하지 않는다(두 플랫폼, 두 경로 모두).

### 5.6 리플렉션·빌드 보강(A0-B 상세)

상세 설계와 단계(B1–B8)는 [리플렉션·빌드 보강 설계](ReflectionFoundations.ko.md)다. 요약:

- 메타데이터 소유를 타깃에서 모듈로(`NativeModuleInput.Metadata`·`ModuleDirectory`, 모듈별 명세 `<ModuleDir>/<Module>.rocksids.json`, 원본 철자를 따르는 생성 include를 모듈의 public·private include로 전파). 모듈 의존 순서로 모듈마다 한 번 실행하고, descriptor(`<Module>.rocksmeta.json`)를 `--import`해 다른 모듈의 타입을 쓴다. 명세 사이 ID 중복 검사, ID 기준으로 합친 이력·병합 검사(`ids move`), 다중 명세 원자 게시. `TargetRules.Metadata`·`IdentityManifest` 제거, fixture는 `ReflectionTestFixtures` 모듈.
- 값 종류: `ROCKS_ENUM`(열거자 `Name`·`DisplayName`·`Hidden`, 플래그 enum, 미선언 값 거부·로드 정책), `ROCKS_STRUCT`(중첩 Record, 참조 struct는 배열 원소 불가, struct 버전·마이그레이션, Core 수학 타입은 `Binds`), `Array<T>`(단일 값 원소는 `ValueArray` wire, struct 원소는 `RecordList`)·`ObjectRefArray<T>`, `AssetId`, `SoftObjectRef<T>`(AssetId + 객체 경로), `Sha256Digest`.
- 지정자: `Version=N`·`WorldOnly`(상속)·`EditorOnly`·`Required`(저장·쿠킹에서 거부)·`Name="…"`(도구 이름, 텍스처 설정은 UE C++ 이름, 12.4절)·`MaxElements`. `ROCKS_NATIVE`(비영속 멤버, 강한 참조를 재귀적으로 금지, `AssetLease`처럼 WorldOnly 전용 형질을 선언한 타입은 WorldOnly 클래스에만 — 에셋 객체 Registry는 WorldOnly 타입 생성을 거부한다, 11.4절). 영속 데이터는 모두 리플렉션 필드다 — `BulkDataRef`·`TextureSource`·`TexturePlatformData`도 `ROCKS_STRUCT`이므로 네이티브 직렬화 hook을 두지 않는다. `completeFieldLayout`은 "모든 영속 필드와 강한 간선을 기술"이다.
- 필드 codec: 필드별 생성 encode·두 단계 decode thunk와 문맥(참조 wire 매개변수, Soft 참조 수집기, 유효 플래그, struct 관찰·치환 hook, 진단, decode 정책) — graph v2(A0-O)와 패키지(A1)가 같은 thunk를 쓴다.
- 부동소수점: 빌드 전체가 이미 `-ffp-contract=off`이고 fast-math를 쓰지 않으므로 모듈별 모델은 두지 않는다(clang-cl의 `/fp:precise`는 contract=on을 뜻하고 `/WX`에서 옵션 충돌 경고가 오류가 되므로 쓰지 않는다). 결정적 수학 모듈의 fmadd·`long double`·외부 심볼(`llvm-nm`) 검사는 C3에서 모든 구성에 구현되었다([CoreDataFoundations](CoreDataFoundations.ko.md) 2절). `-fno-math-errno`는 clang-cl이 그 형태를 받지 않아 쓰지 않는다.
- 빌드가 주입하는 상수: 툴체인 지문(너겟 키, 구성·호스트 경로 제외)과 도구 빌드 ID(진단·캐시 무효화용, 입력 내용 해시 기반 — 패키지 Summary에는 넣지 않고 쿠킹 보고서·AssetIndex 캐시 header에 기록)를 Rocks.Build가 실행 파일마다 생성 번역 단위로 넣는다([ReflectionFoundations](ReflectionFoundations.ko.md) 8.1절·결정 17).
- 엔진 World 타입(Level·Actor·Component)은 `ROCKS_CLASS(WorldOnly)`로 옮긴다(B8).
### 5.7 구현에서 정한 점(A1a — 패키지 포맷 v1)

A1a 구현(`Engine/Source/Runtime/AssetRuntime` — `PackageFormat.h`·`PackageWriter.h`·`PackageReader.h`·`PackageLoad.h`)이 위 절에서 비어 있던 것을 다음과 같이 정했다.

1. **표 섹션의 바이트 배치**: Exports·ObjectImports·Dependencies·BulkIndex는 Record가 아니라 고정 배치다(`u32 count` + 항목, 정수 little-endian, Guid high·low, 문자열은 `u64 길이 + UTF-8`). Exports 항목 = `{name, typeId, outerIndex u32, flags u32}`, ObjectImports = `{assetId, outerImportIndex u32, name, typeId}`, Dependencies = `{assetId, kind u32, expectedTypeId, flags u32(bit 0 required)}`, BulkIndex = `{bulkId u32, flags u32, offset u64, storedBytes u64, rawBytes u64, codec u32, crc32 u32, sha256 32B}`. Summary만 Record다(스키마·필드 Guid는 `Private/PackageInternal.h`, 버전 표는 RecordList `{id, version}`, 엔진 버전은 중첩 Record `{주, 부, 패치}`, 프로파일 id는 String·설정 SHA-256은 Bytes 32). 섹션 schemaVersion은 모두 1이다.
2. **패딩·빈 섹션**: 섹션 offset은 정확히 앞 섹션 끝을 정렬한 값이어야 하고(사이 byte는 0), 섹션 표 offset도 마지막 섹션 끝을 8 정렬한 값이다. bytes 0인 섹션은 거부한다 — Bulk만 예외(entry가 모두 0 byte일 때). 존재하는 표 섹션의 count 0도 거부한다.
3. **Exports 순서 검증**: reader가 깊이 우선 순서(outer는 바로 앞 export 또는 그 조상)와 같은 Outer 안 이름 byte 오름차순(같은 byte 이름 중복 포함)을 검사한다. 대소문자·정규화만 다른 중복은 그래프 1단계(형제 유일성)가 잡는다.
4. **객체 단위 EditorOnly**: Exports flags의 EditorOnly(2)는 v1에서 예약이다. 객체를 EditorOnly로 표시하는 클래스 지정자가 없으므로 writer는 0을 쓰고 reader는 켜진 비트를 `CorruptPackage`로 거부한다(지정자가 생기면 schemaVersion을 올린다).
5. **Dependencies 합성 규칙**(writer와 로드 대조가 같은 함수 `MergeDependencyUses`): 에셋마다 hard 계열(외부 참조 대상 객체)은 EditorOnly가 아닌 쓰임이 하나라도 있으면 Hard, EditorOnly 쓰임만 있으면 EditorOnly, soft 계열도 같다(Soft·EditorOnly). 두 계열이 모두 EditorOnly면 EditorOnly 항목 하나다. required는 그 종류를 정한 쓰임들의 OR다. expectedTypeId는 Hard만 저장 때 대상 패키지 대표 에셋의 타입이고 나머지는 0이다(Soft 대상의 타입은 `RequestAsset`의 expectedType이 판정한다). 로드 대조는 (assetId, kind, required)의 정확한 일치(에디터 읽기 전용은 (assetId, kind) 부분집합)이며 모든 import의 에셋은 Hard 또는 EditorOnly 의존이어야 한다.
6. **bulk**: writer는 저장 그래프의 struct 치환 hook(`SubstituteStruct`)으로 `BulkDataRef`를 첫 출현 순 1부터 다시 매기고 (옛 id → 새 id) 표를 돌려준다. raw byte는 `PackageBulkSource`에서 얻어 크기·SHA-256을 대조한다(다르면 `CorruptPackage`). entry의 EditorOnly 표시는 그 entry의 모든 참조 위치가 EditorOnly일 때다. Zstd entry의 저장 byte는 Compression 모듈의 블록 컨테이너(RCBK, `PackageBulkBlockSizeLog2` = 18 → 256 KiB 블록)다 — writer가 블록 하나씩 압축해 sink에 흘려 쓰고 비워 둔 인덱스 자리를 끝에 `WriteAt`으로 채우므로 압축 메모리 피크는 블록 하나다(5.5절 3, `AssetPackage_BulkBlockCompressionPeak`). 블록마다 압축이 줄이지 못하면 그 블록은 raw로 담긴다(컨테이너 규칙). entry 저장 CRC는 인덱스 CRC와 payload CRC를 이어 붙여(`Crc32Combine` — Core에 더함) 만든다. 0 byte entry는 언제나 None이다. 처음 구현은 entry 전체를 한 번에 압축해 그 크기만큼 메모리를 잡았고, 압축이 줄이지 못하면 entry를 None으로 저장했다(끝까지 압축해 봐야 정해져 흘려 쓸 수 없다). 쿠킹 패키지 v1은 로더가 패키지 전체를 읽으므로(SHA-256 확인) 압축을 푼 raw를 `Package`가 소유하고 `Package::BulkBytes(ref)`로 준다(mip 부분 읽기는 TextureStreaming 단계).
7. **Package**: 하드 의존 패키지 목록(`ObjectRefArray<Package>`)은 두지 않는다 — AssetManager의 상주 단위 표가 의존과 간선 다중도를 가진다. `Package`의 id(Transient·ReadOnly)·종류·bulk는 로드가 Commit 전에 설정하고, 대표 `Asset`의 `assetId`(Transient·ReadOnly)도 같이 설정한다. 로드가 아닌 생성은 `NewPackage(registry, access, id, kind)`다.
8. **진단 모델**: API는 `Result`의 `ErrorCode`, 분류는 `AssetDiagnostic{code, error, asset, related, section, detail}` out 인자다(4.6절 이름표 `AssetDiagnosticCode`). 손상은 `InvalidArgument` + `CorruptPackage`, 버전 축은 `Unsupported` + `UnsupportedVersion`·`NewerVersion`, 할당 실패는 진단 코드 None + 원래 오류다. 그래프 단계 오류는 할당·스레드·종료 오류를 그대로, 미등록 타입(NotFound)·버전(Unsupported)은 `UnsupportedVersion`, 나머지는 `CorruptPackage`로 분류하고 원래 오류를 `error`에 남긴다.
9. **종류 불일치·버전 판정 보강**: 쿠킹을 기대하는 reader가 에디터 패키지를 받으면 `UnsupportedVersion`(다시 쿠킹), 에디터를 기대하는 reader가 쿠킹 패키지를 받으면 `CorruptPackage`다. 쿠킹 패키지의 미등록 export 타입은 `UnsupportedVersion`, 에디터는 reader가 읽기 전용으로 표시한다(로드는 18). 커스텀 버전 표는 쿠킹이면 현재 표와 전체가 같아야 하고 에디터는 높거나 모르는 id면 읽기 전용이다. 쿠킹 로드에서 Required 실효 값 누락은 `CorruptPackage`다(writer가 Required null을 거부하므로).
10. **로드 단계 나눔**: `PackageLoadState`(소유 스레드에서 생성 → `Prepare`는 어느 스레드: 헤더·표·섹션 검증, bulk 해제·SHA-256, graph 1단계, 저장 버전·BulkDataRef·Dependencies 대조) → `InstantiatePackages`(소유 스레드, SCC 묶음 한 트랜잭션: Package·노드 생성 → import 해석 → 적용 → Required → id·bulk 설정) → 호출자 Commit. 묶음 밖 import는 `PackageImportResolver`가 찾는다.
11. **bulk 해제 메모리 상한(설계 메모)**: `DecodePackageBulk`는 BulkIndex의 rawBytes 합만큼 출력 버퍼를 먼저 잡은 뒤 entry마다 풀고 SHA-256을 대조한다. codec None은 stored = raw이고 stored가 파일 안에 있어야 하므로 raw가 파일 크기를 넘지 못한다. Zstd entry는 손상된 rawBytes가 작은 stored로 큰 할당을 요구할 수 있으므로 합의 상한 `PackageLoadOptions::maxBulkRawBytes`(넘으면 `CapacityExceeded` — 자원 한도라 손상이 아니다)가 그 할당을 묶는다. 기본값 16 GiB는 시험·도구용 상한이고, 런타임 로더(A1c)는 매니페스트 항목의 bulkRawBytes(10.1절 5 — 패키지 BulkIndex rawBytes 합)와 11.5절 1번의 임시 버퍼 예산으로 로드 작업마다 이 값을 정한다(예산을 넘는 패키지는 대기 — 같은 절). Zstd entry는 블록 컨테이너 헤더의 rawSize를 BulkIndex rawBytes와 해제 전에 대조한다(다르면 `CorruptPackage`).
12. **할당 실패 분류**: reader·writer·Prepare·InstantiatePackages의 모든 할당 실패는 `OutOfMemory`이고 진단 코드는 None이다(손상으로 분류하지 않는다 — 다시 시도하면 열릴 수 있다). 처음 구현은 Summary 타입 표와 export 타입 집합의 대조에서 할당 실패를 `CorruptPackage`로 돌려줬고 할당 실패 주입 시험(`AssetPackage_AllocationFailures`)이 잡아 고쳤다.
13. **import 표 순서·작업량 상한**(빈틈 리뷰 반영): 처음 구현은 import를 (assetId, 전체 객체 경로 byte) 순으로 정렬해 writer가 초안마다 전체 경로 문자열을 만들고(Outer 사슬마다 반복), reader·로드가 import마다 경로를 처음부터 다시 만들었다(깊이² — 깊이 2040 사슬 아래 잎 6만 개면 수억 단계, 경로 문자열 메모리 수백 MiB). 다음으로 바꿨다.
    - 순서는 5.2절 깊이 우선(같은 outer 아래 이름 byte 순)이다. 경로 byte 순은 '-'(0x2D)가 '.'보다 작아 부분 트리가 끊길 수 있어 한 번 훑기로 검증할 수 없다. 깊이 우선이면 reader가 export 표와 같은 경로 스택으로 항목 수에 비례해 검증하고, 경로 길이는 outer 길이 + 1 + 이름 길이로 이어 센다(문자열을 만들지 않는다).
    - writer는 외부 참조 대상의 Outer 사슬을 이미 초안이 있는 객체에서 멈추고(객체 → 초안 해시 표), 패키지마다 대표 에셋을 한 번 찾으며, (asset, outer 초안, 이름) 정렬 한 번과 깊이 우선 순회로 순서를 정한다. 옛 bulkId → 새 bulkId도 해시 표다.
    - 로드(`InstantiatePackages`)는 에셋마다 패키지를 한 번 찾고, outer import 0이면 `ConstructionContext::FindObject(package, 이름)`, 아니면 앞서 푼 outer import 아래 `ConstructionContext::FindChild(outer, 이름)`(ObjectRuntime에 더한 한 단계 조회 — ObjectFoundations 5.4절)로 푼다. `PackageLoadState::ImportPath`는 없앴다.
    - Prepare 대조(타입 표 하한 탐색, 읽기 전용 부분집합·import ↔ 의존 병합 순회)도 항목 수에 비례한다.
    - import 이름은 객체 이름 규칙(`ValidateNameComponent`)을 reader가 확인한다(`CorruptPackage`). 처음 구현은 로드의 `FindObject`가 `InvalidArgument`로 거부해 진단 코드가 None(할당 실패 분류)으로 나갔다.
    - 다른 패키지의 Package 객체 자체를 가리키는 참조는 저장이 `InvalidArgument`로 거부한다(import 대상은 대표 에셋과 그 아래 객체).
    - 시험 `AssetPackage_ImportScale`이 깊이 2040 사슬 + 잎으로 import 65536개(상한)를 쓰기·읽기·Prepare·Instantiate까지 통과시키고 65537개는 쓰기 `CapacityExceeded`임을 본다.
14. **값 대조 규칙 보강**(빈틈 리뷰 반영):
    - Hard 의존은 expectedType이 0이 아니어야 하고(Hard가 아니면 0 — reader), 그 에셋의 대표 에셋 import(빈 이름)가 있으면 그 타입과 같아야 한다(Prepare — 둘 다 `CorruptPackage`). 로드는 대상 패키지 대표 에셋의 실제 타입이 expectedType의 파생인지 보고 아니면 `TypeMismatch`(section Dependencies), 대표 에셋이 없으면 `MissingDependency`다.
    - 빈 `BulkDataRef`(bulkId 0)는 rawBytes·SHA-256도 0이어야 한다. writer는 저장을 `InvalidArgument`로, 로드는 Objects `CorruptPackage`로 거부한다.
    - 모든 BulkIndex entry는 어떤 `BulkDataRef`가 가리켜야 한다(아니면 BulkIndex `CorruptPackage`). 에디터 읽기 전용 열기는 모르는 필드 안의 ref를 보지 못하므로 이 검사를 하지 않는다.
    - 같은 `PackageLoadState`를 `InstantiatePackages`에 다시 넘기면(앞 호출의 성공·실패와 무관) `InvalidArgument`다. 처음 구현은 그래프 Stage의 순서 오류가 `CorruptPackage`로 분류됐다.

15. **쓰이지 않는 import·정준 byte**(재리뷰 반영):
    - 모든 import는 Objects가 가리키거나 뒤 import의 outer여야 한다(아니면 ObjectImports `CorruptPackage`, detail = 위치). writer는 쓰이는 대상과 그 Outer 사슬만 쓰므로 정상 파일은 늘 지킨다. 에디터 읽기 전용 열기는 모르는 필드 안의 참조를 보지 못하므로 BulkIndex 쓰임 검사(14)와 같이 이 검사를 하지 않는다(`AssetPackage_UnusedImportIsCorrupt`).
    - 에디터 Summary의 compatibleWith는 savedBy의 기준선(주·부·0)과 정확히 같아야 한다(처음 구현은 "기준선 이하"를 받아 같은 내용의 다른 byte를 받았다). 기준선보다 높음(패치·부)과 낮음(부 −1·주 −1 — savedBy 1.2.3에 1.1.0·0.9.0)은 모두 Summary `CorruptPackage`다(`AssetPackage_SummaryFields`의 `CompatPatch`·`CompatNewer`·`CompatOlderMinor`·`CompatOlderMajor`).
    - 퍼징(`AssetPackage_FixedSeedMutationFuzz`)이 받아들인 입력은 읽기 전용이 아니면 파일이 기록한 설정으로 다시 써 입력과 byte가 같아야 한다 — 같은 객체로 풀리는 다른 byte(규칙 구멍)를 체계적으로 잡는다.
16. **resolver 결과와 Prepare의 allocator**:
    - `PackageImportResolver`가 준 handle은 그 에셋의 공개된 `Package`여야 한다. null·회수된 handle·Package가 아닌 객체·다른 AssetId는 진단 None + `InvalidArgument`다(`AssetPackage_ResolverResultIsValidated` — 처음 구현은 조용히 잘못 이었다).
    - `Prepare`(아무 스레드)는 상태 allocator만 쓰고 Registry allocator(소유 스레드 전용 — ObjectRegistry `ObjectAllocator`)는 `InstantiatePackages`(소유 스레드)만 쓴다. 압축을 푼 bulk 저장소는 상태 allocator로 잡고 Package가 복사 없이 이어받는다(bulk가 GB일 수 있어 소유 스레드에서 다시 복사하는 비용을 피한다). 그래서 상태 allocator는 그 상태로 만든 Package(Registry)보다 오래 살아야 한다 — 런타임 로더(A1c)는 엔진 수명의 로더 allocator를 쓴다. 시험은 할당 스레드를 기록하는 allocator로 Registry를 만들어 worker Prepare 동안 Registry allocator가 불리지 않음을 단언한다(`AssetPackage_PrepareUsesOnlyStateAllocator`).
17. **ImportOwner·Tags**: ImportRecord의 내용 스키마(ownerAssetId 포함)는 A2(AssetDatabase)가 정하므로 ImportOwner 의존과 ImportRecord의 로드 대조는 A2로 미룬다(지금은 ImportOwner·Build 의존을 쓰고 읽되 대조하지 않는다 — `AssetPackage_ImportOwnerCrossCheckDeferredToA2`). 여러 ImportOwner 항목과 required의 뜻도 A2가 ImportRecord 스키마와 함께 정한다. Tags는 Record envelope·TLV 구조와 키(필드 ID) 엄격 오름차순을 reader와 writer 옵션 검증이 같이 본다(값 스키마는 A2 — `AssetPackage_TagsKeysAreSorted`). A2는 값 스키마를 문자열 키 → 문자열 목록으로 정하고 이 검증을 바꾸며, ImportOwner 대조는 AssetDatabase가 한다(7.1절 2·7).
18. **에디터 읽기 전용 열기의 기대 결과**: 더 새 데이터(모르는 필드·더 새 타입·커스텀 버전·Required가 아닌 모르는 섹션)를 담은 에디터 파일은 `Prepare`가 읽기 전용으로 받고 `InstantiatePackages`·Commit까지 간다. 모르는 필드는 건너뛰므로 그 안의 Hard·Soft·bulk 참조는 기본값(null·빈 값)으로 남고, Dependencies는 부분집합만, 쓰이지 않는 import·bulk entry는 검사하지 않으며, Package는 bulk entry를 그대로 가진다(`AssetPackage_EditorReadOnlyLoadsEndToEnd`). 현재 클래스가 없는 export 타입(미등록, 또는 등록됐지만 클래스가 아닌 타입 id)은 reader가 읽기 전용으로 표시하고(헤더·인덱스 도구) `PackageContents::unavailableExportType`에 남긴다. 객체를 만들 클래스가 없으므로 `Prepare`는 그래프 1단계 전에 이 reader 판정으로 Summary `UnsupportedVersion`(`Unsupported`)을 낸다. 그래프 1단계의 TypeMismatch로 분류하지 않는 것은 그 오류가 노드 Record 손상에서도 나기 때문이다(처음 구현은 미등록은 그래프 NotFound → `UnsupportedVersion`, 클래스가 아닌 타입은 그래프 TypeMismatch → `CorruptPackage`로 갈렸다 — `AssetPackage_NonClassTypeHasNoCurrentVersion`). 미등록 클래스의 자리표시 객체(UE의 누락 클래스 처리)는 그래프 복원에 노드 생략·참조 null화를 더해야 하는 설계 변경이라 이 단계에서 하지 않는다(5.7절 9의 "에디터는 읽기 전용"은 reader 판정의 뜻이다).
19. **로드의 타입·객체 조회 실패**(판정 리뷰 반영): 로드가 찾은 객체·등록 저장소에 다시 묻는 조회의 실패는 단언하지 않고 진단 None + 원래 오류로 돌려준다.
    - import 대상(대표 에셋·하위 객체)의 `AcquireType`: 단언하지 않고 진단 None으로 그 오류를 돌려준다. 파생 판정은 그 lease(`TypeLease::IsA`)로 해 등록 저장소를 다시 묻지 않는다. 닿는 경로는 `InstantiatePackages` 도중 닫히는 TypeRegistry다: `FindObject`·`FindChild`·`Resolve`는 ObjectRegistry의 종료만 보지만 `AcquireType`은 객체의 타입 lease를 복제하고(`AcquireTypeChecked` → `TypeLease::Clone`), 그 복제는 TypeRegistry가 닫혔으면 `ShuttingDown`이다. `TypeRegistry::Shutdown`은 lease가 남아 `Busy`를 돌려줄 때도 닫힘 표시를 먼저 세운다. 시험 `AssetPackage_TypeRegistryClosesDuringInstantiate`는 상태 allocator가 Prepare 뒤 n번째 할당(첫 할당은 import 해석 전의 대상 표 예약)에서 Shutdown을 부르게 해(모든 n) 결과가 성공이거나 진단 None + `ShuttingDown`임을 보고, resolver 장면(Hard 의존의 대표 에셋 대조)과 묶음 장면(EditorOnly 참조만 — 대조 없이 import 대상 조회) 양쪽에서 ObjectImports 지점에 닿음을 센다. 처음 이 항목은 "지금 엔진에서 닿는 로드 경로가 없다"고 적었는데 이 경로를 놓쳤다. 판정 리뷰가 처음 든 경로(정책 없는 Registry의 `RequestDestroy`만 받은 대상)는 그런 Registry에 패키지를 로드할 수 없어 닿지 않는다 — 패키지는 `packageNativeType = Package`인 Registry에만 로드한다(`InstantiatePackages` 인자 `InvalidArgument` — 정책 없음·다른 타입 모두, `AssetPackage_LoadRequiresPackagePolicyRegistry`; 처음 구현은 그런 Registry로 연 트랜잭션도 받아 그래프 생성 단계가 Objects `CorruptPackage`로 거부했다).
    - Prepare의 대표 에셋 Asset 파생 판정: 그래프 1단계 전에 export 1 타입의 lease를 잡아 그 lease로 판정한다. lease를 잡는 조회가 실패하면(Prepare가 worker에서 도는 동안 `TypeRegistry::Shutdown`이 `closed_`를 먼저 세운 경우의 `ShuttingDown` 등) 그 오류를 그대로 돌려준다(5.3절). 처음 구현은 `types_.IsA`를 그래프 뒤에 불러 그 사이 Shutdown이면 단언이 걸렸다. 시험 `AssetPackage_TypeRegistryClosesDuringPrepare`는 Prepare의 상태 allocator가 n번째 할당에서 TypeRegistry를 닫게 해(모든 n) 결과가 성공이거나 `ShuttingDown`(진단 None)임을 보고, 대표 에셋 lease 지점에도 닿는다.
    - writer 쪽 같은 계열 단언(Admit hook의 `GetOutermost`·`AcquireType`, `ImportBuilder::Add`의 Outer 사슬 `GetOuter`·`AcquireType`)은 안전하다. 그래프 저장이 Exclude 정책에서 외부 참조마다 `WriteUnsaveable`로 대상과 조상을 `AcquireType`으로 먼저 확인하므로 hook·import 빌더가 보는 대상은 방금 생존·타입이 확인된 객체다(같은 소유 스레드, 그 사이 Registry 변경 없음). 대상은 언제나 대표 에셋이거나 그 아래이므로 이 확인이 대표 에셋까지 덮는다: 대표 자식(빈 이름)이 있는 Outer는 이름 있는 자식을 가질 수 없고(정체성 규칙 8 — [ObjectFoundations](ObjectFoundations.ko.md) 1.5절), 대표 에셋이 없는 패키지의 객체를 가리키면 import 빌더가 대표 에셋 조회에서 `NotFound`로 먼저 끝난다(`AssetPackage_WriterEdgeRejections`). 그래서 export 순회의 "첫 export가 아니면 부모가 export"와 import 빌더의 "사슬이 패키지에 닿기 전에 대표 에셋을 지남"도 단언이다.

## 6. 에셋 타입·객체 통합·참조

### 6.1 객체 통합

- 로드된 쿠킹 패키지·에셋은 `AssetManager`가 소유하는 **에셋 객체 Registry**(고유 ContextId, 엔진 TypeRegistry 공유)에 있다. 에디터 패키지는 AssetDatabase `EditorPackageSet`이 소유하는 **에디터 객체 Registry**(같은 패키지 정책 Registry·`ReferenceObserver`)에 있다(7.1절 3 — AssetManager·AssetLoader는 쿠킹 매니페스트 전용이다). World Registry와 분리한다(여러 World·Tryout 공유, 에셋끼리 ObjectRef 가능). 접근은 `AssetManager::With`(에셋 문맥 ObjectAccessScope를 중첩해 연다).
- `Package : Object`(Outer·이름 없음 — 정체성은 AssetId): `ROCKS_PROPERTY(Transient, ReadOnly) AssetId id`, 플래그, 하드 의존 패키지 목록(`ROCKS_PROPERTY(Transient) ObjectRefArray<Package>` — AssetManager 조회용 사본이며 GC 보존 역할은 없다, 존치 여부는 A1이 정한다), bulk 접근자(`ROCKS_NATIVE`: 에디터 패키지는 bulk 저장소 — 파일 경로 + 로드 지문 + BulkIndex로 지연 읽기를 하고 필요할 때 파일을 다시 연다(UE `FEditorBulkData`의 PackagePath + offset), 저장 성공 후 새 파일로 다시 잇는다, 5.5절·7.1절 15). 소속 객체(export)는 목록이 아니라 Registry 자식 고리가 붙잡고(패키지 정책 Registry의 outer → inner 강한 간선 — [ObjectFoundations](ObjectFoundations.ko.md) 1.7절) export 순서는 `CollectInners(DepthFirst)`다. `Asset : Object`(`ROCKS_CLASS(Abstract)`)의 Outer는 Package. inner → outer 강한 간선(계획 §5.2)과 패키지 정책의 outer → inner 강한 간선이 함께 있다.
- 저장 속성은 `ROCKS_PROPERTY`. 대용량 데이터는 `ROCKS_STRUCT BulkDataRef`(bulkId 1부터·rawBytes·`Sha256Digest`) 필드이고 내용은 Bulk 섹션. 쿠킹 플랫폼 데이터의 mip별 entry도 bulkId로 가리킨다(파일 offset은 BulkIndex만 안다).
- 대표 에셋의 AssetId는 `ROCKS_PROPERTY(Transient, ReadOnly) AssetId`이며 로드 때 `prepare` hook이 Commit 전에 설정한다.
- 에셋 객체 Registry의 모든 객체는 Package이거나 Outer 체인이 Package에 닿는다(Registry 정책 — 패키지 없는 객체 생성은 거부한다). 그래서 모든 하드 간선에는 소유 패키지가 있다. 패키지는 이름이 없고(정체성은 AssetId), 이 Registry의 `RequestDestroy`는 Unsupported라 소속 객체는 Discard로만 죽는다(ObjectFoundations 1.3·1.7절).
- 패키지 사이 하드 간선의 변경: 에셋 객체 Registry는 서로 다른 패키지에 속한 객체 사이의 `ObjectRef`·`ObjectRefArray` 원소·`StructRefArray` 원소 참조 멤버·`ObjectRefMap` 값(리플렉션 B9f4)의 설정·해제를 관찰자(A0-O `ReferenceObserver` — 순 다중도 합산 표, [ObjectFoundations](ObjectFoundations.ko.md) 3절)로 알린다. 규칙:
  - 의존 계수는 (소유 패키지, 대상 패키지) 쌍의 간선 다중도다. 필드 둘이 같은 대상을 가리키면 2이고 하나를 지워도 1이 남는다.
  - 관찰하지 않는 간선: 공개 전 객체(`ConstructionContext` 안 — Commit할 때 Registry가 공개하는 객체들의 패키지 사이 간선 다중도를 한 번에 보고한다. 로드·임포트·`asset-new`·복제가 모두 이 경로다), `DestroyRequested` 소유자의 간선 제거(Discard 정리 — Discard가 부분 트리에서 나가는 계수를 관찰자에 음수 다중도로 한 번 보고하고 AssetManager는 그것으로 반납한다), 패키지 객체가 소유한 간선 전부(Package의 하드 의존 목록 `ObjectRefArray<Package>` — AssetManager가 직접 유지하는 거울이다).
  - 관찰자는 Registry가 채우는 순 다중도 합산 표다(잠금 안에서 사용자 코드를 부르지 않는다). `AssetLoader::Pump`가 Closing 처리 전에 `TakeChanges`로 꺼내 묶음 전체를 적용한 뒤 계수·Package 의존 목록·dirty를 판정한다(꺼낸 순서는 키가 처음 생긴 순서라 묶음 중간의 합은 음수일 수 있다).
  - 새 간선이 상주 단위 사이에 순환을 만들면 두 단위(와 순환 경로의 단위)를 하나로 합친다(union-find, 11.4절). 합친 단위는 Discard될 때까지 나누지 않는다(보수 규칙 — 계수가 서로를 붙잡지 않는다).
  따라서 도구·에디터 서비스가 바꾼 하드 간선도 수명과 저장에 반영된다.
- GC: 에셋 객체 Registry의 증분 GC는 EngineAssets가 프레임 끝에 한 조각, 도구는 명시 `Collect`. GC는 열린 `ConstructionContext`가 없을 때만 진행한다(11.2절). 종료: World 종료 → lease·scope 해제 → 에셋 객체 Registry Shutdown.

### 6.2 참조 종류

| 종류 | 필드 | 저장 | 로드 | 쿠킹 | 누락 | 타입 불일치 | 순환 |
|---|---|---|---|---|---|---|---|
| Hard | `ObjectRef<T>` / `ObjectRefArray<T>`(다른 패키지 객체) | ObjectImports + Dependencies(Hard) | 같은 로드 작업에서 먼저 준비, 실패 시 참조자도 실패 | 전이 포함 | 저장·`set` 거부, 로드 `MissingDependency` | 저장 거부, 로드 실패 | SCC 함께 로드·상주(11.3·11.4절) |
| Soft | `SoftObjectRef<T>` / 배열 | Dependencies(Soft) | 자동 로드 안 함 | 기본 포함(`CookSoftReferences`) | 저장 경고(`Required`면 거부), 로드 요청 NotFound | `RequestAsset` TypeMismatch | 허용 |
| EditorOnly | `EditorOnly` 표시 Hard·Soft 필드 | Dependencies(EditorOnly) | 런타임 로더 무시 | 제외, 쿠킹 데이터에서 필드 제외 | 에디터 진단만 | 에디터 진단만 | 허용 |
| Build | `DependencyDeclarer`가 선언(16.2절) | Dependencies(Build) | — | 산출물 선행 조건·너겟 키 포함 | 쿠킹 실패 | 쿠킹 실패 | 지원 SCC는 한 단위, 아니면 `UnsupportedCycle` |
| ImportOwner | ImportRecord `ownerAssetId`(다중 출력의 소유 패키지) | Dependencies(ImportOwner) | 런타임 로더 무시 | 제외 | 에디터 진단(`MissingDependency`) | — | 허용 |

동적으로 로드하는 에셋은 `CookSettings`의 AlwaysCook·PrimaryAsset 규칙에 명시한다.

### 6.3 World·게임플레이에서의 참조

World 객체는 에셋 객체 Registry 밖에 있어 `ObjectRef<에셋>`을 걸 수 없다. World 객체의 영속 참조는 `SoftObjectRef<T>`(필드), 런타임 보유는 `AssetLease`(`ROCKS_NATIVE` 멤버)다. "하드" 의미(함께 로드·쿠킹)는 그 World 데이터를 담는 레벨·prefab 패키지(후속)의 Dependencies(Hard)로 표현한다. 에셋이 다른 에셋을 런타임에 붙잡아야 하면 lease가 아니라 Hard 참조를 쓴다(11.4절). 계획 §7.1의 `ObjectRef<MaterialAsset>` 예시는 이 규칙으로 바꾼다(17절).

## 7. AssetIndex(에셋 레지스트리)

- 패키지 파일이 유일한 진실이다. 헤더·섹션 표·Summary·Exports·ObjectImports·Dependencies·ImportRecord·Tags만 읽는다.
- 조회: ID·경로 키·타입(상속 포함)·디렉터리·태그, 정방향·역방향 의존(종류별), 별칭.
- 임포터 태그(Unreal 교환 UXR2, 2026-11-20): Tags 섹션의 키는 타입이 정하는 것 외에 임포터가 정하는 키도 둘 수 있다. 임포터 키는 `<임포터 이름>.` 접두로 타입 키와 겹치지 않게 한다(예: Unreal 교환의 `UnrealExchange.ObjectPath`·`UnrealExchange.FormerPaths` — [Unreal 교환 형식](UnrealExchange.ko.md) 9.2절). AssetIndex는 임포터 키도 같은 방식으로 색인하고 값 일치 조회를 제공한다. 값이 목록인 키(옛 경로 목록)는 원소마다 색인한다.
- 일관성: 프로세스 로컬 최종 일관 인덱스. 쓰기 결정은 잠금 아래에서 디스크를 다시 확인.
- 갱신: 자기 명령은 즉시. 외부 변경은 `DirectoryWatcher`(계약은 `Platform/DirectoryWatch.h`): 감시 시작 → 검색 → 쌓인 이벤트 적용. 이벤트 종류를 믿지 않고 경로를 다시 Stat·범위 읽기(레벨 트리거). `Overflow(path)`는 그 하위 트리 재검색, `RootRemoved`는 감시를 닫고 `RootLost` 진단·1초 간격으로 루트 재출현을 확인해 다시 연다. 쓰는 중이라 `CorruptPackage`인 파일은 크기·시각이 두 번 연속 같아질 때까지 최대 10초 기다린 뒤 다시 읽고, 그래도 손상이면 진단으로 남긴다.
- 캐시: `Intermediate/AssetIndex.cache`(키: 형식·항목 스키마·판정 의미·Unicode·호환 엔진 버전(주·부)·customVersions 다이제스트·이름 정규화 플래그 — 7.1절 16 A2d1. 도구 빌드 ID는 header에 진단용으로만 기록해 엔진을 다시 빌드할 때마다 캐시가 무효화되지 않게, 원자적 기록, 잠금 보유 프로세스만 기록 — 읽기 명령은 캐시를 읽기만, 점검·재구축 명령은 캐시를 쓰지 않는다). 파일별 헤더+섹션 표 SHA-256과 요약. 시작 시 범위(약 1 KiB)만 읽어 비교, 다르면 그 파일만 다시 읽음. 범위 읽기는 WorkerScheduler 병렬.
- `Verify()`: 4.6절 진단 보고만.
- 원본 감시: 원본 SHA-256이 ImportRecord와 다르면 `SourceChanged`. 자동 재임포트는 기본 꺼짐, 켜면 debounce(500 ms) 후 재임포트, 실패 시 기존 보존·진단(계획 §9.4).

### 7.1 구현에서 정한 점(A2 — AssetDatabase, 설계 2026-11-21)

A2는 에디터 쪽 에셋 데이터베이스다(4·5.4·5.5·7·8·13·16절). 출발점은 AssetRuntime의 패키지 writer(메모리 sink)·reader(전체 파일·헤더만)·동기 로드(`PackageLoadState`)·`NewPackage`, PlatformBase의 `FileLock`·`DirectoryWatcher`·`Replace`·`Move`·`SyncDirectory`, Core의 `ValidateNameComponent`·`TryMakeNameComponentKey`·`TryMakeCaselessKey`·JSON, HostConfig의 `FindProjectFrom`이다. 애매한 곳은 UE 5.8을 따른다(`FPackageName` 마운트, `SavePackage`의 임시 파일 → 이동, `FAssetData`·`DiskCachedAssetData`, `MarkPackageDirty`, `GetAssetRegistryTags`, 누락 import는 경고 + null — `LinkerLoad.cpp` ImportFailure, 이동·삭제 전 `ResetLoaders`). 설계 초안은 리뷰 1회로 고쳤다(아래 결정의 "리뷰" 표시).

1. **모듈과 이름**: `Engine/Source/Developer/AssetDatabase`(Developer — 에디터·도구만 링크, 게임은 쿠킹 결과만 읽는다), 의존 `AssetRuntime`·`HostConfig`(프로젝트 탐색 재사용 — 리뷰). 13절 서비스 API의 이름은 `AssetService`이고(명령과 1:1), 그 아래 부품이 `ProjectLayout`·`AssetIndex`·`EditorPackageSet`이다. 시험은 EngineTests(`Engine/Source/Developer/AssetDatabase/Tests`), 시험 에셋 타입은 `AssetTestFixtures`.

2. **AssetRuntime에 더하는 것**(리뷰 — 처음 적은 "부분 읽기와 파일 sink만"은 틀렸다):
   - 범위 읽기 인터페이스 `PackageRangeReader { virtual Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept = 0; virtual uint64 FileBytes() const noexcept = 0; }`와 메모리·파일 구현.
   - 부분 읽기 `ReadPackageIndex(Allocator&, PackageRangeReader&, const PackageReadOptions&, PackageIndexView& out, AssetDiagnostic*)`: 헤더·섹션 표와 Summary·Exports·ObjectImports·Dependencies·ImportRecord·Tags를 범위 읽기로 가져와 5.1절 1–4의 같은 판정과 그 섹션 CRC를 본다. Objects·Bulk·BulkIndex의 내용은 읽지 않는다(표의 위치·겹침·패딩 판정은 표만으로 하고, 패딩 byte 0 검사는 읽지 않는 구간이라 하지 않는다). 결과는 소유 버퍼다(`PackageContents`처럼 파일 byte를 빌리지 않는다).
   - 고정 버퍼 재검증 `VerifyPackageFile(PackageRangeReader&, scratch)`(5.5절 5 — 헤더·표·모든 섹션 CRC·bulk entry CRC를 일정 크기 버퍼로 훑는다).
   - 파일 sink `FilePackageSink`(`platform::File` 위 `Append`·`WriteAt`).
   - bulk 저장소 `PackageBulkStore`(Package의 bulk 접근 추상 — 6.1절): 런타임은 지금처럼 메모리, 에디터는 AssetRuntime `FilePackageBulkStore`(지연 읽기·`Detach`·저장 뒤 새 파일로 다시 잇기 — 7.1절 15). `PackageLoadState`에 bulk를 풀지 않고 저장소만 잇는 선택지를 더한다. 저장 뒤 재매핑은 객체 필드가 아니라 저장소의 id 표로 한다(7.1절 15 — 처음 적은 `RemapBulkReferences`는 버렸다).
   - 메모리 bulk id 이름 공간(리뷰): 저장하지 않은 새 bulk는 `BulkDataRef::bulkId`의 최상위 bit(`UnsavedBulkIdBit = 0x80000000`)가 선 id를 쓰고, 파일 bulk는 1–0x7FFFFFFF다. writer는 저장마다 모두 1부터 다시 매기고 재매핑 표의 키는 옛 id 하나로 유일하다. 패키지 reader는 최상위 bit가 선 id를 `CorruptPackage`로 거부한다.
   - Tags 값 스키마(리뷰 — 5.7절 17이 미룬 것): Tags 섹션은 Record 하나이고 필드 1 = `RecordList[{ key: string, values: string list }]`, key는 byte 순 엄격 오름차순·유일, 값은 문자열 목록(한 값이면 원소 하나 — UE `FAssetData` 태그는 이름 → 문자열이고 목록 키(7절 `UnrealExchange.FormerPaths`)는 원소마다 색인). writer 검증(`ValidateTagsRecord`)을 이 스키마로 바꾼다(하위 호환 없음).
   - Build 의존 선언(16.2절 `DependencyDeclarer`)과 타입별 태그(UE `UObject::GetAssetRegistryTags` 대응)는 AssetRuntime `AssetTypeHookTable`의 hook으로 둔다(구현은 타입 모듈 — 7.1절 15).

3. **에디터 Registry**(리뷰 — 6.1절을 고친다): 에디터·도구의 패키지는 `EditorPackageSet`이 소유한 **에디터 객체 Registry**(패키지 정책 Registry, 고유 ContextId, `ReferenceObserver` 포함)에 있다. 쿠킹 매니페스트를 읽는 `AssetManager`·`AssetLoader`는 에디터 패키지를 읽지 않으므로 쓰지 않는다. A4 쿠커도 이 Registry에서 에디터 패키지를 읽는다. 6.1절의 "도구도 AssetManager를 소유"는 쿠킹 결과를 읽는 도구(`asset-load-cooked`)에만 해당한다.

4. **하위 단계와 순서**(리뷰 — 저장이 색인의 부분 읽기·검색에 기대므로 순서를 바꿨다. 각 단계는 A1과 같은 완료 조건 넷이고, 그 단계의 서비스 API가 생기면 13절 명령도 함께 더한다 — 1절 표의 A6은 문서·검증 보고만):

| 단계 | 내용 | 13절 명령 |
|---|---|---|
| A2a | `ProjectLayout`(프로젝트 루트·`Content`·`Config`·`Intermediate`·`Saved`, 마운트 표 `/Game`), 논리 경로(파싱·NFC·caseless 키·OS 이름 대응), 쓰기 잠금 `AssetDatabaseLock`, 시계·감시 변경 공급 인터페이스(9) | — |
| A2b | AssetRuntime 범위 읽기·부분 읽기, `AssetIndex` 검색·조회·진단(캐시·감시 없이 — 전체 검색) | `asset-list`·`asset-info`·`asset-index-verify` |
| A2c1 | AssetRuntime 저장 기반: 파일 sink·재검증·bulk 저장소(지연 읽기·저장하지 않은 bulk·id 표 다시 잇기·`Detach`)·Defer 열기·`AssetTypeHookTable`(7.1절 15) | — |
| A2c2a | `EditorPackageSet` 열기: Defer·Hard·EditorOnly 닫힘·누락 import null 규칙·ImportRecord 파서·ImportOwner 대조·색인 ImportRecord 요약(7.1절 15) | — |
| A2c2b | 저장(5.5절 1–8)·dirty(5)·Tags·Build 생성(hook)·`AssetIndex::Refresh`·고아 `.tmp`(7.1절 15) | — |
| A2c3 | 명령과 Commandlet 타입 등록(7.1절 15) | `asset-new`·`asset-set`·`asset-info`·`asset-list --type` |
| A2d | 색인 캐시(`Intermediate/AssetIndex.cache`)·감시 갱신(7절) | `asset-index-rebuild` |
| A2e | 작업: 별칭 표, 이동·이름 변경(파일·디렉터리), 복제, 삭제, 중복 ID 해결, 고아 출력(16.1절), `Detach`(6) | `asset-move`·`asset-duplicate`·`asset-delete`·`asset-fix-duplicate-id`·`asset-alias-*`·`asset-orphans-*` |
| A2f | 임포트 프레임워크(세부 7.1절 18 — A2f1a·A2f1b·A2f2): `Importer`(입력 원본 byte·옵션 → 출력 그래프·bulk), 원본 루트(`Config/AssetSources.json`), 임포트·재임포트(제자리 갱신 — 8), `SourceChanged`, ImportOwner 대조(7) | `asset-import`·`asset-reimport`(시험 임포터로 확인, 텍스처 임포터는 A5) |

5. **dirty**(리뷰, A2c 교차 검증으로 고침): 명시 `EditorPackageSet::MarkDirty(package)`(UE `MarkPackageDirty`·`UPackage::SetDirtyFlag`)만 세대를 올린다. `EditorPackageSet`의 편집 진입점(서비스 API 대입·하위 객체 생성·Discard·bulk `AddUnsaved`)이 대상 패키지에 `MarkDirty`를 부른다. `ReferenceObserver`는 계수·진단용이고 dirty에 쓰지 않는다(그것은 다른 패키지 사이 강한 간선의 순 합산만 보아 같은 대상 패키지 안 다른 객체로의 변경·스칼라·Soft·bulk·하위 객체 추가를 놓친다). 저장은 직렬화 시작 때의 세대를 기억하고 끝에 같을 때만 dirty를 지운다(5.5절 7).

6. **열린 패키지와 파일 작업**(리뷰): 이동·삭제·덮어쓰기 전에 그 파일(디렉터리면 아래 모든 파일)의 패키지를 `Detach`한다 — 아직 읽지 않은 bulk를 메모리로 읽어 저장소를 메모리로 바꾸고 캐시된 파일 핸들이 있으면 닫는다(UE `ResetLoaders`). 이동 뒤 `EditorPackageSet`의 경로 표를 고친다. dirty 패키지의 이동은 허용하고(내용은 메모리에 있다) 다음 저장은 새 경로에 쓴다.

7. **ImportRecord**: 스키마는 5.4절 그대로이고 파서·writer는 AssetDatabase에 둔다(AssetRuntime은 스키마를 모른다). ImportOwner 대조(5.7절 17)는 AssetDatabase의 열기·색인이 한다: 자기 자신이 아닌 `ownerAssetId`면 Dependencies에 그 id의 ImportOwner 항목이 정확히 하나(required)여야 하고, 자기 자신이면 ImportOwner 항목이 없어야 한다. 위반은 `CorruptPackage`. 여러 ImportOwner는 허용하지 않는다.

8. **재임포트**: 기존 객체를 제자리에서 갱신한다(UE 재임포트와 같이 출력 객체의 정체성·참조자의 ObjectRef 유지). 임포터가 같은 출력 키에 같은 타입을 내면 필드를 덮어쓰고, 사라진 출력은 `OrphanedOutput`, 타입이 바뀐 출력은 새 객체로 바꾸고 진단한다.

9. **결정적 시험을 위한 주입점**(리뷰):
   - 시계 `AssetClock { virtual int64 NowTicks() noexcept = 0; }` — RootLost 재확인(1초), 쓰는 중 파일 대기(최대 10초, 두 번 연속 같음), 원본 debounce(500 ms), 별칭 생성 UTC·만료 계산이 모두 이 시계를 쓴다. 갱신은 `AssetIndex::Pump(now)`처럼 시각을 받는 호출이라 시험이 고정 시각을 단언한다.
   - 감시 변경 공급 `DirectoryChangeSource`(`DirectoryWatcher`는 `final`이라 감싼다) — 시험은 이벤트를 주입하고, 실제 OS 감시는 `Wait` 루프와 실패 상한으로 한 번만 확인한다(sleep 없음).
   - 파일 시스템은 `platform::FileSystem&`로 받는다(고장 주입 — A1e `ReadManifestFile`과 같은 방식). "저장 중 편집"은 고장 FileSystem의 Flush·Replace hook이 소유 스레드에서 편집을 넣는다.
   - 두 프로세스 잠금·동시 쓰기는 `Platform/Process` 자식과 파이프 gate로 순서를 정한다(Busy는 대기 0으로, "대기 후 성공"은 해제 gate 뒤 획득으로 단언).
   - 병렬 부분 읽기의 결과·진단은 키 순으로 정렬해 완료 순서와 무관하게 한다.

10. **경로**(A2a, 리뷰):
    - 논리 경로 파싱은 4.2절 규칙을 순서대로 판정하고(앞 `/` → 구분자·빈 구성 요소 → 마운트 → 구성 요소마다 NFC 정규화 뒤 `ValidateNameComponent` → 전체 256 byte) 실패하면 `InvalidPath`와 이유(구성 요소 index·`NameComponentError`).
    - OS 이름 대응: 색인 항목은 실제 OS 상대 이름(열거한 byte 그대로)을 보관하고, 있는 파일의 OS 경로는 색인에서 얻는다. 새로 만드는 파일은 NFC 이름으로 만든다. 열거한 이름이 NFC가 아니면 NFC로 바꿔 논리 경로를 만들되, 바이트로 비교하는 파일 시스템(Linux·NTFS)에서는 그 NFC 이름이 실제 파일을 가리키지 않으므로 `InvalidPath`로 남기고 색인하지 않는다(macOS는 NFD 저장이 정상이라 받는다). caseless 키가 같은 두 파일은 `DuplicatePath`(둘 다 진단, 경로 조회에서 뺀다 — ID 조회는 된다).
    - 경로 상한: 논리 경로 256 byte(4.2절)가 프로젝트 기준 이식 가능한 상한이고, 절대 OS 경로(임시 접미사 `.<guid 36>.tmp` 41 byte 포함)는 플랫폼 상한으로 검사한다(넘으면 `InvalidPath`). 상한은 끝 NUL을 뺀 UTF-8 byte다: Windows는 긴 경로 32767 UTF-16 단위에서 NUL과 PlatformBase의 UNC 긴 경로 접두 실질 증가(6)를 뺀 32760, macOS는 PATH_MAX 1024 − 1, 그 밖은 4096 − 1(PATH_MAX는 NUL을 포함한다 — A2a 리뷰). 시험이 그 길이의 파일을 실제로 만든다.
    - 프로젝트 루트는 절대 경로로 바꾼다(UE `FPaths::ConvertRelativePathToFull` — 상대 경로는 작업 디렉터리 기준, Windows 현재 드라이브 루트·드라이브 상대는 거부). 루트뿐인 경로("C:/"·"/")는 그대로 둔다.
    - 패키지 확장자는 ASCII 대소문자를 무시한다(UE `FPackageName::IsAssetPackageExtension` — `Hero.RASSET`도 같은 패키지 자리이고 실제 이름은 색인이 가진다. byte 일치로 두면 대소문자를 무시하는 파일 시스템에서 색인하지 않은 그 파일을 같은 경로의 저장이 덮어쓴다 — A2a 리뷰). 그래서 byte로 비교하는 파일 시스템(Linux)에서는 `Hero.rasset`과 `Hero.RASSET`이 같은 논리 경로가 되며 색인(A2b)이 `DuplicatePath`로 보고한다.
    - 프로젝트 탐색은 HostConfig `FindProjectFrom`을 쓴다(13절 `--project` 기본값).

11. **색인**(A2b·A2d): 항목은 `{assetId, 논리 경로, key, OS 상대 이름, 대표 타입, 지문, 크기, 수정 시각, dependencies, tags, ImportRecord 요약, readOnly}`. 조회는 7절 목록이고 타입 상속 조회는 등록된 타입만 상속을 따르며 등록되지 않은 타입 id는 정확 일치만 한다(리뷰). 캐시는 7절 그대로 범위 읽기로 지문을 비교한다(리뷰 — 크기·수정 시각만 보는 UE `DiskCachedAssetData` 방식은 FAT 2초 해상도와 같은 크기 편집을 놓친다). 부분 읽기와 전체 읽기의 판정 관계는 시험 표로 둔다: 헤더·섹션 표·읽은 섹션의 손상은 같은 진단, Objects·Bulk 내용과 그 패딩의 손상은 부분 읽기 통과·열기 실패.

12. **그 밖**(리뷰 권장): 다중 출력 임포트의 출력 경로는 `<주 출력 경로>_<출력 키>`(UE 다중 임포트의 접미 규칙)이고 충돌하면 임포트를 거부한다. 임포트 옵션의 텍스트 표기는 `ReflectionText`를 쓴다. 별칭이 있는 경로에 새 에셋을 만들거나 이동해 오면 그 경로가 점유된 것으로 보고 거부한다(UE redirector가 경로를 점유하는 것과 같다 — 별칭을 지운 뒤 쓴다). 고아 임시 파일(`*.rasset.<guid>.tmp`)은 잠금을 쥔 저장이 그 디렉터리에서, 잠금을 쥔 전체 검색(`asset-index-rebuild`)이 전체에서 지운다.
13. **A2b 세부**(2026-11-21, 구현 착수 전):
    - 부분 읽기는 `ReadPackage`와 판정 코드를 함께 쓴다. AssetRuntime의 읽기 핵심을 바이트 공급원 추상(전체 파일 span 또는 범위 읽기)으로 바꾸고, 전체 읽기는 지금과 같은 판정·같은 진단을, 부분 읽기는 그중 Objects·Bulk·BulkIndex 섹션의 내용·CRC, 섹션 사이 패딩 byte 0 검사, bulk 대조만 건너뛴다(표의 위치·정렬·겹침·종류별 flags·schemaVersion 판정은 그대로). 부분 읽기는 헤더(64 byte)·섹션 표를 먼저 읽고 고른 섹션마다 범위 읽기를 한다. 범위마다 소유 버퍼를 잡아 `PackageIndexView::buffers`에 두므로 결과 view가 그 버퍼를 가리킨다(이동해도 유효 — 처음 설계의 "합만큼 한 번"은 섹션 크기 검사 전에 합을 믿어야 해서 범위마다로 바꿨다). 64 byte보다 작은 파일은 있는 만큼으로 헤더 판정을 한다(전체 읽기와 같은 진단). 범위 읽기의 IO 오류는 그 오류(진단 None)다.
    - 범위 읽기 `PackageRangeReader`의 구현은 메모리(`MemoryPackageRangeReader`)와 파일(`FilePackageRangeReader` — `platform::FileSystem&`로 열어 Seek + ReadExact, 고장 주입은 FileSystem)이다.
    - Tags 값 스키마(2)도 A2b에서 정한다(색인이 읽는다): `ReadPackageTags`·`WritePackageTags`(AssetRuntime `PackageTags.h`). 배치는 Record(`PackageTagsSchema`){ RecordList[ Record{ 키 String, 값 String } ] }이고 항목은 (키, 값) UTF-8 byte 순 엄격 오름차순이다(목록 값은 같은 키의 항목 여러 개 — 원소마다 색인). 키는 비지 않은 1–256 byte, 값은 4 KiB 이하(패키지 문자열 상한), 둘 다 UTF-8이다. 패키지 형식 층(writer·reader의 Tags 검증)은 지금처럼 "필드 id 오름차순 Record"만 본다 — 형식 층에 값 스키마를 강제하면 형식 시험·golden byte를 모두 바꿔야 하고 형식 층이 값 의미를 알 필요가 없다(처음 설계의 "검증 교체"를 바꿨다). 다른 스키마 Guid의 Tags는 형식으로 유효하고 색인은 `UnsupportedVersion` 진단을 남기고 태그 없이 색인한다.
    - `AssetIndex`(AssetDatabase): `Rebuild(FileSystem&, PackageReadOptions)`가 `Content/`를 재귀 열거해(링크는 따라가지 않는다) `.rasset`(대소문자 무시)만 부분 읽기로 색인하고, `*.tmp`와 그 밖의 파일은 무시한다(UE AssetRegistry가 패키지 확장자만 보는 것과 같다). 항목은 논리 경로 키 → OS 상대 이름 순, 진단은 OS 상대 이름 → 코드 순으로 정렬한다(7.1절 9 — InvalidPath 파일은 논리 경로 키가 없어 진단은 이름으로 정렬한다). A2b는 한 스레드로 차례로 읽는다 — worker 병렬 읽기는 캐시·감시 갱신과 함께 A2d가 더한다(정렬이 완료 순서를 지우므로 결과는 같다).
    - 진단(4.6절 코드): 이름 규칙 위반 `InvalidPath`(LogicalPathError 사유), 패키지 판정 실패는 그 코드(`CorruptPackage`·`UnsupportedVersion`·`NewerVersion`), 쿠킹 패키지는 `CorruptPackage`(에디터 기대), 같은 AssetId 둘 이상 `DuplicateAssetId`(모두 진단, ID 조회는 AlreadyExists), 같은 경로 키 둘 이상 `DuplicatePath`(모두 진단, 경로 조회는 AlreadyExists — 7.1절 10). `Verify`가 더하는 것: 색인에 없는 Hard·Soft 의존 `MissingDependency`(EditorOnly·Build·ImportOwner는 A2c 이후), TypeRegistry가 있을 때 Hard 대상의 대표 타입이 expectedType도 그 파생도 아니면 `TypeMismatch`(로드와 같은 판정 — 5.7절 14). Registry가 없으면 파생 여부를 알 수 없어 형식은 대조하지 않는다(정확 일치로 대조하면 로드되는 참조를 오류로 보고한다 — 리뷰 지적).
    - 조회: ID, 경로(LogicalPath 키), 디렉터리(마운트 또는 논리 경로 아래 — 바로 아래만 또는 재귀), 대표 타입(정확 일치 또는 TypeRegistry로 파생 포함 — 미등록 id는 정확 일치), 태그(키·값 일치, 목록 값은 원소마다), 정방향 의존(종류 묶음), 역방향 참조자(종류 묶음). 결과는 경로 키 순이다.
    - 명령(13절): `asset-list [--under=<path>]`(경로·AssetId·대표 타입 id 한 줄씩), `asset-index-verify`(진단 줄, 있으면 종료 1). 타입 이름 필터(`--type`)와 `asset-info`의 속성 출력은 commandlet이 타입을 등록해야 해서 A2c(`asset-new`·`asset-set`과 함께)로 미룬다.
14. **A2b 구현에서 정한 것**(2026-11-21, 시험·리뷰 중에 정했다):
    - 디렉터리 존재: `FileSystem::FileExists`는 일반 파일만 참이라(디렉터리는 거짓 — 디렉터리는 `DirectoryExists`) Content 존재 확인에 쓰지 않는다 — 실제 디스크 시험이 이것을 잡았다. 열거 결과로 판단한다: 없는 디렉터리(Content가 없거나 검색 중 지워진 하위 디렉터리)는 빈 디렉터리다. Content 자신의 열거 실패(Content가 파일이면 WrongType 대응 오류 포함)는 `Rebuild`의 그 파일 오류이고, 하위 디렉터리의 열거 실패(권한 거부·검색 중 파일로 바뀜)는 그 디렉터리 이름으로 진단 None과 원본 오류를 남기고 그 디렉터리만 뺀다(파일 IO 실패와 같은 규칙). 실패한 `Rebuild`는 색인을 비운다(앞 결과를 남기지 않는다).
    - 파일 하나의 실패는 그 파일만 빼고 계속한다: 열거와 열기 사이에 사라진 파일은 `MissingFile`, 그 밖의 열기·읽기 IO 실패는 진단 None과 원본 오류(쿠킹 로더 10.1절과 같은 규칙), 일반 파일이 아닌 항목(장치 등)은 무시한다. 확장자뿐인 이름(`.rasset` — 배치는 "패키지 파일 아님"으로 보지만 확장자 검사는 통과한다)은 하위 디렉터리의 같은 이름(`Dir/.rasset` → 끝 '/')과 같게 `InvalidPath`(EmptyComponent)다. 유효한 UTF-8이 아닌 OS 이름(POSIX 열거는 byte 그대로 준다)은 논리 경로도, 그 이름으로 다시 열 경로(String은 UTF-8만)도 만들 수 없어 디렉터리·패키지 파일이면 `InvalidPath`(InvalidComponent — 이름 규칙의 잘못된 UTF-8과 같은 사유)로 진단하고 내려가지 않는다(그 밖의 파일은 무시). 할당 실패만 `Rebuild` 오류다.
    - 진단의 이름(`fileName`·`related`)은 표시 이름이다: 한 줄·유효한 UTF-8이고 서로 다른 OS 이름이 같은 표시 이름이 되지 않게 역슬래시·제어 문자(0x00–0x1f·0x7f)와, 이름이 유효한 UTF-8이 아니면 0x80 이상 byte를 16진(역슬래시 x 두 자리)으로 적는다(역슬래시를 그대로 두면 글자 그대로 "\xff"인 이름과 0xFF byte 이름이 같아진다 — 재리뷰 지적). 항목의 `fileName`은 OS 이름 그대로다(색인되는 이름은 논리 경로 규칙상 역슬래시·제어 문자가 없다). 진단 줄은 표시 이름을 그대로 쓴다.
    - 정렬·조회: Core `algo::Sort`(O(n log n), 안정 정렬 아님)와 `algo::LowerBound`를 쓴다(처음 구현의 삽입 정렬은 색인 10만 항목에서 비교 수십억 번이라 리뷰가 바꾸게 했다). 비교는 전순서다: 항목은 OS 상대 이름이 파일마다 다르고, 진단은 한 이름에 코드마다 하나뿐이며(경로·패키지·Tags 진단은 서로 배타, 같은 id·경로 진단은 각각 하나), id 순은 같은 id를 경로 순 위치로 가른다. Tags 쓰기도 같은 정렬 뒤 이웃 비교로 같은 쌍을 거부한다.
    - ImportRecord 요약(7.1절 11 항목 목록)은 ImportRecord 스키마를 정하는 A2c로 미룬다 — A2b 항목은 ImportRecord byte를 읽기만 하고 담지 않는다.
    - 같은 id·경로의 상대(`related`)는 정렬 순서(경로 키 순·id 순)에서 이웃한 파일이다 — 앞 이웃이 같으면 그것, 아니면 뒤 이웃(같은 값이 셋 이상이어도 각자 하나).
    - Tags 읽기: Record·항목 버전이 `PackageTagsVersion` 밖이면 `Unsupported`(더 새 Tags — 색인은 `UnsupportedVersion` 진단, 다른 스키마 Guid와 같다), 그 밖의 형식 위반(키·값 필드 누락·wire 종류·빈 키·길이·UTF-8·순서·같은 쌍·목록 필드 모양·항목 스키마)은 `InvalidArgument`(색인은 `CorruptPackage` 진단, 칸 Tags). 둘 다 태그 없이 색인한다. 쓰기는 목록 전체가 Record 기본 한도를 넘으면 `CapacityExceeded`이고(측정: 값 4 KiB 태그 996개, 값 1 byte 태그 36157개까지), 읽기가 같은 기본 한도·예산으로 읽으므로 쓴 것은 모두 읽힌다.
    - `ListByType`: 타입 0은 `InvalidArgument`. TypeRegistry 판정의 `NotFound`(미등록)·`TypeMismatch`(클래스가 아닌 id)는 파생 아님이고, 그 밖(종료 뒤 `ShuttingDown`)은 그 오류다. `Verify`도 같고, Registry가 없으면 형식 대조를 하지 않는다(7.1절 13). Hard 의존은 패키지 읽기가 expectedType을 반드시 갖게 검사하므로(5.2절) `Verify`는 그 값을 그대로 쓴다.
    - 명령 세부: 옵션은 `--<이름>=<값>`만(13절 문법 — 같은 이름 두 번·빈 값·모르는 이름·위치 인수는 사용법 오류 2, `--help` 하나는 사용법을 표준 출력에 0). 13절 공통 옵션 `--wait=<초>`는 받되(10진 숫자가 아니면 2) 읽기 명령은 잠금을 잡지 않아 쓰지 않는다. 값의 범위(넘침) 검사는 그 값을 실제로 쓰는 A2c 쓰기 명령과 함께 넣는다. `--project`가 없으면 현재 디렉터리에서 위로 `*.rocksproject`를 찾고(HostConfig `FindProjectFrom`), 있으면 그 디렉터리에 기술자가 있어야 한다(오타·하위 디렉터리로 빈 색인을 내지 않게 — 위에서 찾은 다른 디렉터리면 실패 1). 읽기 옵션의 엔진 축은 명령 실행 파일의 엔진 버전(`build::CurrentEngineVersion`)이라 더 새 엔진이 저장한 패키지는 `NewerVersion` 진단이다(UE AssetRegistry도 너무 새 패키지를 읽지 않는다). `asset-index-verify`는 타입 Registry 없이 대조한다(명령은 타입을 등록하지 않는다 — 형식 대조는 타입을 등록하는 A2c 명령부터). 진단 줄은 AssetDatabase `AppendAssetIndexDiagnosticLine`이 만든다: `<진단 코드>: Content/<OS 상대 이름>: <설명>` — 코드 None(파일·하위 디렉터리 IO 실패)은 원본 오류 이름(`ErrorCodeName`)을 코드 자리에 쓰고 설명은 "cannot read the file or directory"다. 요약 `asset-index-verify: <항목 수> packages, <진단 수> diagnostics`는 표준 출력이다. `--under`가 논리 디렉터리가 아니면 `asset-list: InvalidPath: …` 실패 1이다.
    - 명령 시험(C#)은 패키지를 만들 수 없어 C++ 시험이 고정한 fixture 배열(`AssetDatabase/Tests/AssetIndexCommandletFixtures.h` — `AssetIndex_CommandletFixtureBytes`가 writer 출력과 같은지 보고, 다르면 새 내용을 출력한다)을 읽어 프로젝트에 쓴다.

15. **A2c 세부**(2026-11-21, 설계 — 교차 검증 1회 반영: 필수 6·권장 8): 저장·열기는 한 번에 크므로 넷으로 나눠(A2c1 → A2c2a → A2c2b → A2c3, 7.1절 4 표 — 저장 시험이 열기에 기대므로 이 순서) 부분마다 완료 조건 넷(14절)을 지킨다. 출발점 사실(조사): bulk는 로드 때 모두 메모리로 풀고(`DecodePackageBulk` → `PackageAccess::SetPackage`) 지연 읽기·저장소 추상이 없다. 객체의 `BulkDataRef`를 바꿔 쓰는 범용 리플렉션 walker가 없다(`SubstituteStruct`는 직렬화 byte만 바꾼다). `ReflectionText`(Prepare·Publish 대입, JSON 출력)가 있다. 타입별 객체 hook 장치는 없다(구조체 `Validate`만). Commandlet은 타입을 등록하지 않는다.
    - **A2c1 — AssetRuntime 저장 기반**
      - `FilePackageSink`(`platform::File&` 위 — Append는 이어 쓰기, WriteAt은 Seek 후 쓰고 끝으로 돌아간다, 오류는 FileErrorCode 대응).
      - `VerifyPackageFile(PackageRangeReader&, Span<uint8> scratch, AssetDiagnostic*)`(5.5절 5): 읽기 옵션은 `expectedKind = Editor`, `types` null(버전 축으로 거부하지 않는다), 엔진 버전 비움. 부분 읽기 판정(헤더·표·읽는 섹션)에 더해 건너뛰던 Objects·Bulk·BulkIndex의 CRC와 섹션 사이 패딩 0을 scratch 크기 조각으로 훑고, BulkIndex의 entry 배치(`ValidateBulk`)와 entry마다 저장 byte의 crc32, Zstd entry의 블록 컨테이너 헤더 rawSize = rawBytes(해제 없이 — `DecodePackageBulk`가 해제 전에 하는 검사)를 본다. 압축 해제·SHA-256·그래프 해석은 하지 않는다. 같은 손상에 전체 읽기와 같은 진단을 낸다(시험 표로 대조). 한계: CRC는 writer가 자기 출력으로 계산하므로 writer의 논리 오류(잘못된 내용을 올바른 CRC로 씀)는 잡지 못한다 — 짧은 쓰기·배치 오류·sink 버그가 목적이다.
      - bulk 저장소 `PackageBulkStore`(6.1절): Package에 선택적 저장소를 둔다. 런타임은 지금처럼 메모리(저장소 없음), 에디터는 `FilePackageBulkStore`다. 이 저장소는 **파일 핸들을 쥐지 않는다**(열린 패키지가 수백이면 macOS 기본 soft limit 256에서 EMFILE): 파일 경로와 로드 지문, entry 표를 쥐고, entry를 처음 읽을 때 `FileSystem`으로 파일을 열어 헤더 + 섹션 표 지문을 대조한 뒤(다르면 `ExternalModification`) 저장 byte를 읽어 crc32·압축 해제·SHA-256을 확인하고 raw byte를 보관한다. 열린 핸들은 상한 있는 캐시(기본 64, 가장 오래 쓰지 않은 것부터 닫는다)에만 둔다(UE `FEditorBulkData`가 PackagePath + offset으로 필요할 때 여는 것과 같다). 지연 읽기에서 발견한 손상은 그 entry만 `CorruptPackage`이고 `BulkBytes`의 호출부가 받는다(계약에 적는다). 저장은 5.5절 3대로 같은 오류로 실패한다(세탁 금지). `Package::BulkBytes`는 저장소가 있으면 저장소에 묻는다. entry 하나를 푸는 `DecodePackageBulkEntry`를 `DecodePackageBulk`에서 떼어 둘이 같이 쓴다.
      - 지연 열기: `PackageLoadOptions::bulk = Decode(기본, 런타임) | Defer(에디터)`. Defer의 Prepare는 Bulk 섹션 내용을 읽지 않는다 — 읽기 핵심에 "Bulk 내용만 건너뜀" 모드(부분 읽기의 Objects 포함판)를 더해 `PackageRangeReader`로 받고, CrossCheck는 BulkIndex 항목으로 하던 그대로다. `InstantiatePackages` 전에 상태에 저장소를 건다(`SetBulkStore`).
      - **재매핑은 객체 필드를 고치지 않는다**(5.5절 7을 바꿨다): 저장소가 "메모리 id → entry" 표를 갖는다(로드 직후는 항등). 저장의 재매핑 표(옛 id → 새 id)로 이 표를 새 파일 entry로 다시 잇는다. 객체의 `BulkDataRef`는 옛 id를 그대로 갖고, 다음 저장의 writer는 그 옛 id로 저장소에 물어 다시 1부터 매긴다. 근거: 범용 변경 walker가 없고(만들면 struct·배열·map·InstancedStruct·Optional 전부를 쓰기로 다루는 큰 표면), 저장 중 편집과 경합하지 않으며(필드 변경 없음), 관찰되는 동작(`BulkBytes`·다음 저장 byte)이 같다. 버린 대안: 리플렉션 walker로 필드 교체(UE `FBulkData`의 offset 갱신 대응). 이 결정의 전제(교차 검증):
        - (a) bulkId는 패키지 저장소의 지역 핸들이다. 내용 비교는 (rawBytes, sha256)으로 하고 `BulkDataRef::SameContent`를 둔다. 패키지·세션 사이에서 bulkId를 비교하지 않는다(재임포트의 "바뀌지 않음", ReflectionText의 같은 값 생략, 시험의 다시 열어 비교가 모두 `SameContent`).
        - (b) 다른 패키지의 bulk는 `PackageBulkStore::CopyFrom(sourceStore, ref) → 새 저장하지 않은 ref`로만 옮긴다(그대로 넣은 ref는 다른 entry를 가리켜 저장이 `CorruptPackage`가 된다 — 사용자 오류가 손상으로 보고되지 않게 서비스 API가 막는다).
        - (c) 복제(A2e)는 새 저장소에 원본 저장소의 id 표를 복제하고, 원본 파일 entry는 `Detach`처럼 메모리로 읽어 원본과 수명을 분리한다.
        - (d) 다시 잇기·`Detach`는 이미 푼 raw byte를 옮기거나 놓지 않는다(`BulkBytes`가 준 span은 패키지 수명 동안 유효). 저장 결과에 없는(참조하지 않는) entry는 표에서만 지우고 그 raw는 패키지 수명까지 둔다(undo는 범위 밖).
        - 쿠킹(A4)은 저장소를 bulk source로 쓰고 결과 ref는 쿠킹 writer가 새로 매기므로 영향이 없다. 한계: 메모리의 id가 파일 id와 다를 수 있어 진단은 둘을 구분한다(`Package::FileBulkId(ref)`).
      - 저장하지 않은 bulk: 최상위 bit가 선 id(`UnsavedBulkIdBit = 0x80000000`, 저장소가 패키지마다 증가시켜 발급 — `PackageBulkStore::AddUnsaved(Array<uint8>&&) → BulkDataRef`, SHA-256을 그때 계산)이고 메모리에만 있다. writer는 저장소로 읽어 다른 entry와 같이 1부터 매긴다. reader는 최상위 bit가 선 BulkDataRef·entry id를 명시적으로 `CorruptPackage`로 거부한다(지금은 2^31 entry가 없어 암묵적으로만 거부 — 시험으로 고정).
      - `Detach`(7.1절 6)의 저장소 쪽: 아직 읽지 않은 entry를 모두 읽어 raw byte로 두고 캐시 핸들을 닫고 파일 경로를 놓는다(A2e가 쓴다, A2c1에서 시험).
      - 타입 hook 표 `AssetTypeHookTable`(UE `UObject::GetAssetRegistryTags`·16.2절 `DependencyDeclarer` 대응): TypeId → `{ tags, buildDependencies }`. 저장은 대표 에셋 타입에서 상속 사슬을 따라 **가장 가까운 등록 hook 하나**를 부르고 hook은 `CallBase`로 부모 hook을 부를 수 있다(UE `Super::GetAssetRegistryTags`). hook은 `ObjectRegistry&`·`ObjectAccessContext`·객체를 받아 `Result`를 돌려준다(실패하면 저장 실패 — 진단 None과 원본 오류). sink는 같은 (키, 값)을 하나로 합치고 Build 의존은 (assetId, kind)로 정렬·중복 제거한다. 표 항목은 모듈 등록 수명에 묶는다(모듈 해제 때 제거). 런타임은 쓰지 않는다(쿠커 A4가 Build hook을 쓴다).
    - **A2c1 구현에서 정한 것**(2026-11-21):
      - 외부 변경의 오류 코드는 `ExternalModification` / `StaleHandle`이다(파일이 로드 때의 것이 아니다 — 핸들이 낡았다는 뜻과 같다). 지문 읽기 `ReadPackageFingerprint(Allocator&, PackageRangeReader&)`를 AssetRuntime에 두고 캐시·저장(5.5절 1)이 같이 쓴다(헤더 판정 + 섹션 표 CRC만 본다).
      - `VerifyPackageFile`은 Zstd 컨테이너 헤더의 rawSize 대조(교차 검증 권장)를 하지 않는다: 재검증의 계약을 "전체 읽기와 같은 판정"으로 두었고(모든 bit 손상 시험이 그 일치를 고정한다), 컨테이너 헤더는 지연 읽기의 해제(`DecodePackageBulkEntry`)가 해제 전에 본다.
      - 읽기 핵심의 범위는 셋이다(`Full`·`Edit`·`Index`). Objects·BulkIndex는 Required 섹션이라 다른 schemaVersion은 앞 단계가 거부하므로 재검증은 그 내용 CRC를 늘 본다(처음 넣은 schemaVersion 조건은 닿지 않아 뺐다 — 손으로 만든 더 새 BulkIndex는 세 읽기 모두 `NewerVersion`).
      - 핸들 캐시(`PackageFileCache`)는 열 때 지문을 읽어 두고, 같은 경로를 다른 지문으로 찾으면 그 핸들을 닫고 `ExternalModification`이다. 이미 연 핸들은 교체 뒤에도 이전 내용을 읽는다(POSIX 의미 rename — 그 핸들의 지문이 그 내용과 맞다).
      - 저장소에 저장하지 않은 bulk가 있으면 `Attach`를 거부한다(새 패키지는 저장 뒤 `Rebind`로 처음 잇는다 — 파일에 이은 적 없는 저장소의 `Rebind`를 받는다). 새로 만든 패키지에는 `AttachBulkStore(Package&, UniquePtr<PackageBulkStore>&)`로 저장소를 잇는다(로드한 패키지는 `PackageLoadState::SetBulkStore`).
      - 타입 hook의 찾기와 `CallBase`는 한 함수(`Find(start, kind, skipStart)`)다 — `CallBase`는 지금 hook 타입의 base부터 찾는다.
    - **A2c2a — `EditorPackageSet` 열기**
      - 소유(7.1절 3): 패키지 정책 Registry(`packageNativeType = Package`, `rejectWorldOnly`)와 `ReferenceObserver`를 AssetManager와 같은 방식으로 만들어 소유한다(TypeRegistry·NamePool은 호출자 것을 공유). 열린 패키지 표(AssetId → Package handle·파일 이름·로드 지문·세대·읽기 전용·미해결 import).
      - `Open(AssetId | LogicalPath)`: 색인에서 파일을 찾는다(같은 id·경로 둘이면 그 `AlreadyExists`). 이미 열린 패키지는 같은 handle이다. **Hard·EditorOnly 의존 닫힘**(object import를 만드는 두 종류 — 5.7절 5, 색인 Dependencies로 재귀)을 한 트랜잭션으로 열고, 이미 열린 것은 resolver(`PackageImportResolver` — `EditorPackageSet`이 구현)로 잇는다. SCC를 따로 계산하지 않는다(에디터는 상주 단위가 필요 없다). 각 파일은 Defer 열기 + 파일 저장소.
      - 누락 import: Prepare 단계에서 실패한 대상(색인에 없음·파일 없음·손상·새 버전)과 대표 타입이 기대의 파생이 아닌 대상은 묶음에서 빼고 null로 잇고 경고(`MissingDependency`·`TypeMismatch`)를 남긴다(UE `LinkerLoad` ImportFailure — `InstantiatePackages`에 "누락 import는 null" 선택지를 더한다). `InstantiatePackages` 안의 실패는 지금처럼 트랜잭션 전체 실패다(객체를 만든 뒤의 실패는 고정된다). null로 이은 패키지는 미해결 import 표시를 갖고 **저장은 `MissingDependency` / `InvalidArgument`로 거부한다** — 명시 선택(`SaveOptions::dropUnresolved`, 명령 `--force`)일 때만 null로 저장하고 경고를 남긴다(브랜치 전환 등으로 잠시 없던 대상의 id를 잃는 결정을 사용자가 하게 — UE는 경고만 하고 잃는다).
      - ImportRecord 스키마 writer·parser(5.4절)는 AssetDatabase(`ImportRecord.h`)에 두고 열기·색인이 ImportOwner 대조(7.1절 7 — 위반은 열기 `CorruptPackage`)에 쓴다. 색인 항목에 ImportRecord 요약(importer id·이름·버전, 출력 수, 소유자 id)을 더한다(7.1절 11, A2b가 미룬 것).
      - 읽기 전용(더 새 버전) 패키지는 열리지만 저장·복제·ID 수정은 `NewerVersion` / `Unsupported`(4.4절).
      - **null import의 바탕**(A2c2a 착수 때 정함): 그래프 2b의 `ApplyFields`가 null handle import를 "미해결"로 받아 그 참조를 null로 쓴다(ObjectFoundations 5.4절 — 확인을 건너뛴다). `InstantiatePackages`는 `PackageInstantiateOptions{resolver, imports = Strict | NullMissing, unresolved}`를 받는 겹판을 두고, NullMissing은 Strict가 MissingDependency·TypeMismatch로 실패하던 해석(대상 패키지 없음, 대표 에셋 없음, Hard 의존 대표 타입이 expectedType의 파생이 아님, 객체 없음, 객체 타입이 선언 타입의 파생이 아님)을 null import + `PackageUnresolvedImport{package, target, import, code, error}` 기록으로 바꾼다. 에셋 전체가 풀리지 않으면 그 에셋의 첫 import에 한 번 적고 나머지 import도 null이다. outer import가 null이면 그 아래 import도 미해결(MissingDependency)이다. resolver의 다른 오류·잘못된 handle·타입 조회 실패는 Strict와 같다. 버린 대안: 누락 대상 자리에 자리표시 패키지(UE `FLinkerPlaceholder`처럼)를 만들어 id를 보존 — 같은 id의 실제 패키지가 형식만 다를 때(TypeMismatch) 두 패키지가 같은 id를 갖게 되고, 추상 타입은 만들 수 없다. id 보존은 "미해결 import가 있으면 저장 거부"(A2c2a 위)로 한다.
      - **ImportRecord 배치**(5.4절의 논리 필드, 스키마 버전 1): Record(`ImportRecordSchema` = Guid{0x52494d5052454344, 1}){ 1 importerId Guid, 2 importerName String(1–256 byte), 3 importerVersion U32, 4 sources RecordList[ Record(`ImportSourceSchema` = {…, 2}){ 1 role String(1–64), 2 root String(0–256 — 원본 루트 이름, 빈 값은 루트 없음), 3 relativePath String(1–1024), 4 sha256 Bytes(32), 5 bytes U64 } ], 5 options Record(임포터 스키마 — 없을 수 있다), 6 outputs RecordList[ Record(`ImportOutputSchema` = {…, 3}){ 1 outputKey String(0–64 — 빈 값은 주 출력), 2 assetId Guid, 3 typeId Guid } ], 7 ownerAssetId Guid }. 필드 id는 Guid{0, n}이다. 문자열·byte 필드의 payload는 다른 패키지 Record처럼 길이 접두 없는 내용 byte다(TLV 길이가 길이다). 규칙(위반은 InvalidArgument): ownerAssetId는 null이 아니다. 문자열은 유효 UTF-8에 NUL이 없다. 소유 기록(owner = 자기 패키지 id)은 importerId가 null이 아니고 importerName이 있으며 sources가 1–256개(role 유일 — 순서는 쓴 그대로)이고 outputs는 1–1024개로 outputKey byte 순 엄격 오름차순(writer가 정렬한다)·assetId 유일·null 아님·typeId null 아님·자기 id를 포함한다. relativePath는 `/`로 나눈 상대 경로다(앞·뒤 `/`, 빈 성분, `.`·`..` 성분, `\` 금지 — 원본 루트 밖을 가리키지 못한다). 루트가 비면(어느 원본 루트 아래도 아닌 원본 — 4.5절) relativePath는 `/` 구분 절대 경로다: POSIX `/a`, UNC `//server/share/a`(서버·공유 뒤 성분 하나 이상), 드라이브 `X:/a`(뒤는 같은 성분 규칙 — 기록은 호스트와 무관한 문법만 본다, A2f1a에서 고쳤다: 처음 규칙은 빈 루트에도 상대 경로를 요구해 POSIX 절대 경로를 기록할 수 없었다). root는 `/`·`\`를 갖지 않는다. options는 Record envelope 검사(ReadRecordHeader)를 통과해야 하고 비면 필드를 쓰지 않는다. 참조 기록(owner ≠ 자기)은 importerId null·importerName 빈 값·importerVersion 0이고 sources·options·outputs 필드가 없다. 빈 목록 필드·모르는 필드·wire가 다른 필드는 InvalidArgument다(정규형 하나). 다른 스키마 Guid나 지원 범위 밖 버전은 Unsupported다. writer·parser는 AssetDatabase `ImportRecord.h`에 둔다(`WriteImportRecord`·`ReadImportRecord` — AssetRuntime은 스키마를 모른다, 7.1절 7).
      - **ImportOwner 대조**(7.1절 7): 읽은 ImportRecord가 참조 기록이면 Dependencies에 그 owner의 ImportOwner 항목이 정확히 하나, 소유 기록이거나 ImportRecord가 없으면 ImportOwner 항목이 없어야 한다(`CheckImportOwner`). 다른 스키마·더 새 버전 ImportRecord는 참조 기록인지 알 수 없으므로 대조하지 않는다. 위반은 `CorruptPackage`(칸 Dependencies). 열기는 그 패키지를 열지 않고(대상이 의존이면 null + 경고), 색인은 진단을 남기고 그대로 색인한다(조회는 되게 — 고칠 수 있게).
      - 색인 항목의 ImportRecord 요약 `AssetIndexImportSummary{present, importerId, importerName, importerVersion, outputCount, owner}`. 색인은 ImportRecord를 Tags처럼 읽는다: 다른 스키마·더 새 버전은 `UnsupportedVersion` 진단(요약 없음), 형식 위반은 `CorruptPackage` 진단(칸 ImportRecord, 요약 없음) — 둘 다 항목은 색인한다. 한 파일에 같은 코드 진단이 둘 이상 생길 수 있으므로(깨진 Tags + 손상 ImportRecord 등) 진단 정렬은 파일 이름 → 코드 → 칸이다.
      - **`EditorPackageSet` 구현에서 정한 것**:
        - 생성은 `EditorPackageSet(Allocator&, EditorPackageSetConfig)`(allocator는 생성자 인자 — 표 멤버가 생성 때 필요), 설정 `EditorPackageSetConfig{types, files, layout, index, read(customVersions·engineVersion — expectedKind는 Editor, types는 위 것으로 채운다), registryContext, registryOptions(policy·referenceObserver는 집합이 정한다), observerMaxKeys, fileCacheCapacity}`. `Initialize`가 관찰자·Registry·핸들 캐시를 만든다. 소멸은 열린 패키지 Discard(패키지 정책 객체는 Discard로만 죽는다) → RootRef → Registry → 캐시 → 관찰자 순(저장소가 캐시 핸들을 닫으므로 캐시가 Registry보다 오래 산다). 소유 스레드 전용이다.
        - 닫힘은 색인이 아니라 **읽은 파일의 Dependencies**로 넓힌다: 색인은 id → 파일 이름에만 쓰고, Prepare한 패키지의 Hard·EditorOnly 항목을 너비 우선으로 더한다(색인이 낡아도 실제 파일의 의존을 따른다). 색인의 파일을 읽었는데 헤더 id가 요청과 다르면(색인 뒤 파일이 바뀜) `ExternalModification` / `StaleHandle`이다.
        - 실패 분류: 요청한 패키지(루트)의 실패는 `Open` 실패다 — 색인에 없음 `NotFound`(진단 None), 같은 id 둘 `DuplicateAssetId` / `AlreadyExists`, 파일 열기·Prepare 실패는 그 진단, id 불일치, ImportRecord 형식 위반 `CorruptPackage`(칸 ImportRecord), ImportOwner 대조 위반 `CorruptPackage`(칸 Dependencies). 의존 패키지의 같은 실패는 그 패키지를 묶음에서 빼고 경고 `ExcludedDependency`(진단 asset = 뺀 패키지, related = 처음 가리킨 패키지 — 색인에 없음은 `MissingDependency` / `NotFound`)를 남기며, 그것을 가리키는 import는 `InstantiatePackages`의 NullMissing이 null로 잇고 경고 `UnresolvedImport`(asset = 가리킨 패키지, related = 대상, 칸·detail = import 위치)를 남긴다. 묶음 안 대상의 대표 타입이 Hard 의존 expectedType의 파생이 아니면 대상은 그대로 열고 그 import만 null이다(대상 자체는 유효한 패키지다).
        - 이미 열린 패키지의 미해결 import는 나중에 대상이 생기거나 열려도 다시 잇지 않는다(다시 열어야 한다 — 닫고 다시 여는 경로는 A2e의 디렉터리·이름 작업과 함께 정한다). 저장(A2c2b)은 미해결 표로 거부·`dropUnresolved`를 판정한다.
        - 경고 진단의 detail은 import 위치(UnresolvedImport)라 색인 진단의 detail(섹션 안 위치)과 뜻이 다르다 — 명령 출력(A2c3)이 둘을 섞을 때 문구로 구분한다.
        - 다른 스키마·더 새 버전 ImportRecord는 열되 읽기 전용이다(저장하면 이해하지 못하는 기록을 잃거나 그대로 옮겨 잘못 쓸 수 있다 — 더 새 섹션과 같은 규칙, 5.3절).
        - 열린 패키지 정보 `EditorPackageInfo{id, package handle, fileName(Content 상대), fingerprint, readOnly, unresolved(그 패키지의 미해결 import)}`는 id 순 표로 두고 `Find(id)`·`Count`·`At`으로 본다. `FindPackage`(resolver)는 이 표를 본다.
    - **A2c2b — 저장**
      - `Save(package, SaveOptions)`(5.5절 1–8): 잠금 → 대상 지문을 로드 지문과 비교(다르면 `ExternalModification`) → 같은 디렉터리에 `<이름>.rasset.<guid>.tmp` CreateNew → `WritePackage`(파일 sink, bulk source = 그 패키지 저장소 — 옮기는 entry의 crc32·SHA-256은 writer가 대조) → Flush·Close → `VerifyPackageFile` → 캐시 핸들 닫기 → `Replace` → 저장소를 새 파일 경로·지문·재매핑 표로 다시 잇기 → 시작 세대 = 끝 세대면 dirty 해제 → 색인 갱신(`AssetIndex::Refresh(fileName)` — 그 파일 하나를 다시 부분 읽기) → 그 디렉터리의 고아 `.tmp`(정확한 모양만) 삭제. 실패 경로는 5.5절 6·7 그대로(임시 파일 삭제, 교체 실패 뒤 이전 경로 지문 확인, `NotDurable`은 성공 + 경고).
      - 새 패키지(파일 없음)의 저장은 `TryMakeFilePath`의 NFC 이름으로 만든다. 대상이 이미 있거나(Stat) 색인에 같은 caseless 키가 있으면(Linux에서 `hero.rasset`과 `Hero.rasset`이 함께 생기지 않게) `DuplicatePath` / `AlreadyExists`. 별칭 점유(7.1절 12)는 A2e가 같은 지점에 더한다.
      - 쓰기 옵션: 엔진 버전 `build::CurrentEngineVersion()`, 커스텀 버전 표(A2c는 빈 표 — 모르는 커스텀 버전이 든 패키지는 열 때 읽기 전용이라 저장되지 않으므로 안전하다, 타입 모듈의 커스텀 버전 등록은 후속), Tags·Build 의존(hook), ImportOwner 의존과 ImportRecord(열 때 읽은 byte 그대로 — 임포트 A2f가 새로 만든다).
      - dirty(7.1절 5): 세대는 `MarkDirty` 호출에서 바로 오르므로 저장 중 편집과의 순서 문제가 없다. 새 패키지는 만들 때 명시 dirty다.
      - **A2c2b 구현에서 정한 것**(착수 때):
        - API: `Create(LogicalPath, TypeId type, AssetId id = null(새로 발급))`은 새 패키지와 대표 에셋(type)을 만들어 집합에 넣는다(파일 없음, dirty, 세대 1, 빈 파일 저장소를 `AttachBulkStore`로 잇는다 — 첫 저장의 `Rebind`가 처음 잇는다). 같은 경로 키가 색인에 있거나 집합의 저장하지 않은 새 패키지와 겹치면 `DuplicatePath` / `AlreadyExists`, 파일 절대 경로가 OS 상한을 넘으면 `InvalidPath` / `CapacityExceeded`(만들 때 판정한다 — 배치는 바뀌지 않으므로 저장까지 미루지 않는다), 같은 id가 열려 있거나 색인에 있으면 `DuplicateAssetId` / `AlreadyExists`, type이 에셋 클래스가 아니면 Registry의 오류. `MarkDirty(id)`(열리지 않았으면 `NotFound`)는 세대를 올리고 dirty를 켠다. `Save(id, const AssetDatabaseLock&, EditorSaveOptions{dropUnresolved}, warnings, failure)`.
        - 경고 구조를 열기·저장이 같이 쓴다: `EditorWarning{kind, diagnostic}`, 종류 `ExcludedDependency`·`UnresolvedImport`(열기), `DroppedUnresolvedImport`·`NotDurable`·`OrphanNotDeleted`(저장).
        - 설정 추가: `hooks`(`AssetTypeHookTable*` — **저장에는 필수**다: null이면 `Save`가 `InvalidArgument`. 저장이 Tags를 hook으로 다시 만들므로 hook 표를 모르는 host가 모든 태그를 지우지 않게, 태그가 필요 없는 host도 빈 표를 명시해 넘긴다. 명령(A2c3)은 엔진과 같은 타입 모듈의 hook을 등록한 표를 넘겨야 한다 — UX1b의 옛 경로 태그 보존 조건), `writeEngineVersion`(빈 값이면 `build::CurrentEngineVersion()` — 시험은 고정 값), `index`는 갱신하므로 `AssetIndex*`다.
        - 잠금: `Save`는 이 프로젝트의 잡은 `AssetDatabaseLock`을 받는다(잡지 않았거나 다른 프로젝트의 잠금이면 `InvalidArgument` — 잠금은 잡을 때 프로젝트 루트의 SHA-256을 기억하고 `IsHeldFor(layout)`로 대조한다, 할당 없이). 명령은 명령 전체 동안 잠금을 쥐므로(8절) 저장마다 잡지 않는다. 고아 `.tmp` 삭제는 이 잠금 아래에서만 한다.
        - 판정 순서(앞에서 걸리면 아무것도 쓰지 않는다): Initialize 전·hook 표 없음·잠금(위) `InvalidArgument` → 열리지 않음 `NotFound` → 읽기 전용 `NewerVersion` / `Unsupported` → 미해결 import가 있고 `dropUnresolved`가 아니면 `MissingDependency` / `InvalidArgument`(related = 첫 대상) → 대상 경로(새 패키지는 `TryMakeFilePath` — 경로 상한은 Create가 이미 판정했다, 대상이 이미 있으면(Stat) 또는 색인에 같은 경로 키가 있으면 `DuplicatePath` / `AlreadyExists`, Stat의 IO 실패는 진단 None) → 외부 변경(기존 패키지: 대상의 지문을 `ReadPackageFingerprint`로 읽어 열 때 것과 비교 — 다르거나 대상이 없거나 헤더가 깨졌으면 `ExternalModification` / `StaleHandle`, IO 실패는 진단 None과 그 오류).
        - 쓰기: 옵션은 에디터 종류·`writeEngineVersion`·빈 커스텀 버전 표·bulk source = 그 패키지의 파일 저장소, Tags = hook(대표 에셋 타입에서 가장 가까운 것), extraDependencies = hook의 Build 의존 + 열 때 읽은 ImportOwner 항목, importRecord = 열 때 읽은 byte. **Tags는 저장마다 hook으로 다시 만든다**(UE `GetAssetRegistryTags`처럼): 남겨야 하는 메타데이터(Unreal 교환의 옛 경로 등)는 객체 필드로 두고 그 타입 hook이 태그로 낸다 — 파일의 Tags를 옮겨 두지 않는다(hook이 더 내지 않는 키가 남는 낡은 태그를 만들지 않게). UX1b가 이 규칙을 따른다.
        - 새 패키지는 쓰기 전에 대상의 부모 디렉터리를 만든다(`MakeDirectoryTree` — 실패는 진단 None과 그 오류).
        - 진단을 받는 호출(`Prepare`·`WritePackage`·`VerifyPackageFile`·`ReadPackageForEdit`)이 진단을 채우지 않고 실패하면(sink·할당·IO) 진단에 그 오류를 채워 돌려준다(시험이 찾은 것 — 처음 구현은 비어 있는 진단의 오류(None)를 돌려줘 `InvalidArgument`로 보였다).
        - 임시 파일 `<대상>.<36자 소문자 guid>.tmp`(`CreateNew`, guid는 OS 난수) → `FilePackageSink`로 쓰기 → `Flush`·`Close` → 다시 열어 `VerifyPackageFile`(scratch 64 KiB) + `ReadPackageForEdit`(새 파일의 bulk entry·섹션 offset) → 그 핸들 닫기 → **교체 뒤의 일을 모두 준비**: 저장소 `PrepareRebind`(새 경로·지문·entry·재매핑의 검사와 새 표 할당)와 색인 `PrepareRefresh`(임시 파일을 대상 이름으로 부분 읽기해 새 색인 배열·정렬·같은 id·경로 진단을 모두 만들고 자리를 잡는다) → 캐시의 대상 핸들 닫기 → `Replace`. 여기까지의 실패는 임시 파일을 지우고(지우기 실패는 무시 — 다음 저장의 고아 정리가 지운다) 그 진단으로 실패한다: 쓰기는 `WritePackage`의 진단, 만들기·쓰기·Flush·Close·Replace의 IO는 진단 None과 그 오류, 재검증 실패는 그 진단(writer·sink 결함). `NotDurable`은 교체가 이미 보이므로 성공 + 경고다.
        - 교체 뒤(**실패하지 않는다** — 리뷰 권장으로 2단계화): 정보 갱신(지문·파일 이름·새 패키지 표시 해제·미해결 표 비움) → 시작 세대 = 지금 세대면 dirty 해제 → `CommitRebind`·`CommitRefresh`(할당 없는 교환) → 그 디렉터리의 고아 `.tmp`(정확히 `*.rasset.<36자 guid>.tmp` 모양의 일반 파일) 삭제(정리는 저장의 일부가 아니라 실패는 `OrphanNotDeleted` 경고 하나 — 자리는 교체 전에 잡는다). 준비가 실패하면 교체 전이라 임시 파일을 지우고 아무것도 바꾸지 않는다. 교체가 실패하면 준비한 것을 버린다. 교체 실패 뒤의 "이전 경로 지문 확인"(5.5절 6)은 따로 하지 않는다 — 다음 저장이 1단계에서 같은 비교를 하고 지연 bulk 읽기도 캐시에서 지문을 대조하므로 결과가 같다.
        - `dropUnresolved`: 미해결 import마다 `DroppedUnresolvedImport` 경고(asset = 패키지, related = 대상)를 남기고 null로 저장한다. 저장 뒤 미해결 표는 빈다(파일에 그 import가 없다).
        - `AssetIndex::Refresh(files, fileName, options)` = `PrepareRefresh` + `CommitRefresh`: 그 파일 하나를 다시 부분 읽기해 항목을 바꾸거나(없어졌으면 빼고, 새 파일이면 더한다) 그 파일의 진단을 다시 만들고, 같은 id·경로 진단과 표시는 전체에서 다시 계산한다(정렬·이웃 진단은 Rebuild와 같은 코드). 준비는 지금 색인을 바꾸지 않고(항목은 자리 포인터로 가리킨다) 모든 할당을 끝내며, 확정은 할당 없이 옮긴다 — 그래서 실패하면 색인이 그대로다. 준비는 읽을 경로를 따로 받을 수 있다(저장이 교체 전에 임시 파일을 대상 이름으로 색인한다 — 이름 변경은 수정 시각을 바꾸지 않는다). 준비와 확정 사이에 색인을 바꾸면 안 된다(같은 스레드의 저장 안에서만 쓴다). 파일 이름이 `.rasset`이 아니면 `InvalidArgument`, 파일 IO는 Rebuild처럼 진단이다.
    - **A2c3 — 명령**: `asset-new <Type> <path> [<속성>=<값>…]`·`asset-set <path> <속성>=<값>…`(쓰기 잠금, `--wait=<초>` 범위 검사, `--force`는 미해결 import를 null로 저장), `asset-info <path|AssetId>`(기본 `<경로>=<값>` 줄 — ReflectionText에 줄 출력이 없으므로 JSON 출력과 같은 경로 순회로 줄 출력을 ReflectionText에 더한다, `--json`), `asset-list --type=<Type>`. 대입은 `ReflectionText` Prepare → 모두 성공하면 Publish → `MarkDirty` → 저장. 참조 값(논리 경로·AssetId)은 ReflectionText의 참조 해석 hook을 `EditorPackageSet::Open`으로 구현한다(대상 닫힘을 열어 잇는다). 타입 등록: Commandlet이 엔진과 같은 방식으로 모듈 타입을 붙인다(`ModuleTypeRegistrations`·`ModuleLoader::AttachTypeRegistrar`). 명령 시험(C#)의 시험 타입은 엔진 안의 실제 에셋 타입이나 시험 전용 플러그인 모듈로 둔다 — A2c3 착수 때 정한다.
      - **A2c3 착수 때 정한 것**(2026-11-21, 권장안 확정 — 조사 1회):
        - 나눔: **A2c3a**(ReflectionText 줄 출력, 참조 resolver, 서비스 API `AssetService` — C++ 전수 시험)와 **A2c3b**(Commandlet 명령·타입 등록·C# 시험). 부분마다 완료 조건 넷.
        - 명령 시험(C#)의 타입: 엔진의 실제 에셋 타입 `Skeleton`·`AnimationClip`(Animation 모듈 — float·bool·enum·Name·struct·struct 원소 배열·Map·Hard 참조(`skeleton`·`basePoseClip`)·Soft 참조 배열(`compatibleSkeletons`)을 가진다). 버린 대안: 시험 전용 플러그인 — 켠 플러그인 모듈은 주 실행 파일에 링크되므로(BuildTargets BD36) 출하 Commandlet에 시험 코드가 들어간다. 서비스 API의 전수 시험(C++)은 지금처럼 `AssetTestFixtures` 타입을 쓴다.
        - Commandlet 타입 등록: 엔진·WorldBench와 같은 방식(`TypeRegistry` + `ModuleTypeRegistrations` → `ModuleLoader::AttachTypeRegistrar`, 에셋 타입 모듈을 링크해 시작 단계로 시작 — 지금은 Animation, A5 뒤 텍스처 모듈)과 NamePool(Name 필드). hook 표는 모듈이 등록한 것을 넘긴다 — 지금 hook을 등록하는 타입 모듈이 없어 빈 표이고, 모듈의 hook 등록 길은 첫 hook(A5 텍스처)과 함께 더한다(그때까지 저장은 Tags를 비운다 — 지금 Tags를 내는 타입이 없다).
        - 참조 텍스트(4.1·4.2절): `<논리 경로>`(대표 에셋) 또는 `<논리 경로>.<객체 경로>`(하위 객체 — 구성 요소에 `.`가 없으므로 첫 `.`에서 나눈다), AssetId 36자도 같은 꼴(`<AssetId>[.<객체 경로>]`), null은 `none`. resolver `AssetTextResolver`(AssetDatabase): Hard는 `EditorPackageSet::Open`으로 대상 닫힘을 열고 객체를 찾아(대상 타입이 기대 타입의 파생이 아니면 실패 → 진단 Unresolved), Weak은 대입 대상의 패키지 안만 푼다(5.1절 — 저장이 밖의 Weak 대상을 null로 쓰므로 받지 않는다, 열지 않는다 — A2c3a 리뷰 필수: 처음 구현은 Hard처럼 열어 `asset-set`이 성공한 뒤 파일에 null이 남았다). 서비스는 `asset-set`의 대상 패키지를 Weak 범위로 주고(`SetWeakScope`) `asset-new`는 범위가 없다 — 새 패키지의 객체는 공개 전이라 집합·색인에 없으므로 `asset-new`의 Weak·자기 참조(새 패키지 자신과 그 하위 객체를 가리키는 Hard)는 Unresolved이고 만든 뒤 `asset-set`으로 넣는다(한계 — 리뷰 권장), Soft는 색인으로 AssetId만 얻고 열지 않는다(색인에 없는 논리 경로는 실패, AssetId 텍스트는 그대로). 출력은 handle의 패키지 id → 색인 경로(없으면 AssetId) + 객체 경로. 구현 세부(A2c3a): 저장하지 않은 새 패키지는 색인에 없으므로 경로 해석·출력이 집합의 그 경로를 먼저 본다. null AssetId 텍스트는 거부한다(null은 `none`). 객체 경로 형식은 Hard·Weak도 열기 전에 검사한다(형식 거부는 아무것도 열지 않는다). 대소문자는 경로 키·이름 비교 규칙대로 무시한다(Soft의 객체 경로는 받은 그대로 둔다). Package 객체 자신은 텍스트로 나타낼 수 없어 출력이 Unsupported다(빈 객체 경로는 대표 에셋). 같은 id가 색인에 둘이면 출력은 AssetId다. Hard·Weak 해석이 연 패키지의 경고는 resolver에 준 경고 목록에 붙는다(asset-set이 보고한다).
        - `asset-new`의 대입은 공개 전 객체(AssignmentTarget::Staged — ReadOnly 초기값 허용, 10.2절)에 한다: `EditorPackageSet::Create`가 패키지·대표 에셋을 한 트랜잭션으로 만들고 공개 전에 호출자 초기화(대입 묶음 Prepare·Publish)를 부른다 — 실패하면 트랜잭션 rollback으로 아무것도 남지 않는다. 이를 위해 AssetRuntime에 트랜잭션 안 패키지 만들기(`NewPackage(ConstructionContext&, AssetId, PackageKind)`)와 대표 에셋 만들기(`NewRepresentativeAsset(ConstructionContext&, package, type)` — `Asset::Id()`를 패키지 id로 둔다)를 더한다. 그전에는 새 패키지의 대표 에셋을 `NewObject`로 공개 뒤에 만들어 `Id()`가 로드 전까지 null이었다(로더만 채웠다) — Create가 이것을 쓰면서 고친다. 초기화는 `EditorAssetInitializer::Initialize(transaction, asset)`이고 트랜잭션을 끝내지 않는다(Commit·Rollback은 Create가 한다). Commit이 돌려주는 Package root를 그대로 집합의 root로 쥔다(공개 뒤 실패 지점이 줄어든다).
        - 줄 출력(`ExportLines`, ReflectionText — 10.4절 표): 줄바꿈이 든 값·경로는 줄로 쓸 수 없어 `Unsupported`(`--json`을 쓴다).
        - 서비스 API(`AssetService`, AssetDatabase — 13절 명령과 1:1): 설정(타입 Registry·Name pool·Content 파일 시스템·hook 표·읽기 옵션·Registry 문맥)과 초기화한 `ProjectLayout`을 받아 색인(`AssetIndex`)과 에디터 집합(`EditorPackageSet`)을 소유한다. `Initialize`가 색인을 만들고 집합을 연다. 함수: `New(lock, <Type>, <path>, 대입들)`(타입 도구 이름은 `TypeRegistry::AcquireByToolName` — 없으면 NotFound, 에셋 클래스가 아니거나 추상이면 TypeMismatch / InvalidArgument; 대입은 공개 전 객체에 Prepare·Publish한 뒤 함께 공개하고 저장), `Set(lock, <path|AssetId>, 대입들, force)`(열기 → Prepare 모두 검증 → Publish → MarkDirty → 저장, force = dropUnresolved), `Info(<path|AssetId>, json·all, out)`(기본 Edit 용도 줄, `all`은 Report 용도, `json`은 ExportJson), `List(<Type>?, <under>?, out)`(색인 `ListUnder` — `--type`은 그 클래스와 파생(`TypeRegistry::IsA`, UE 콘텐츠 브라우저의 클래스 필터와 같이 파생 포함), 등록되지 않은 타입의 항목은 같은 id일 때만 맞는다). 쓰기 잠금은 호출자가 잡아 넘긴다(명령이 `--wait`로 잡는다). `--type`의 파생 판정 오류는 `AssetIndex::ListByType`과 같은 규칙이다(등록되지 않은 타입 NotFound·파생 아님 TypeMismatch만 "맞지 않음", 그 밖은 오류 — 리뷰 필수: 처음 구현은 모든 오류를 맞지 않음으로 삼켰다; 디렉터리 걸러내기와 합치므로 ListUnder 순회에 같은 판정을 둔다). `EditorPackageSet::Create`의 초기화 안의 Create·Save는 Busy다(재진입 거부 — 경로·id 판정과 표 자리가 바깥 만들기와 엇갈리지 않게, 리뷰 권장). 대입 오류는 호출자의 `TextDiagnostics`에 모두 모이고 함수는 InvalidArgument다(아무것도 저장하지 않는다). 참조 값은 `AssetTextResolver`로 풀고 쓴다(열기 경고는 warnings에).
      - **A2c3b 착수 때 정한 것**(2026-11-21, 권장안 확정):
        - 명령(13절 문법): `asset-new <Type> <path> [<대입>…]`, `asset-set <path|AssetId> <대입>…`(대입 하나 이상 — 없으면 사용법 오류 2), `asset-info <path|AssetId> [--json] [--all]`, `asset-list [--type=<Type>] [--under=<path>]`. 대입은 `<경로>=<값>`(`[...]`·따옴표 밖 첫 `=`) 위치 인자와 목록 옵션 `--unset=<경로 목록>`·`--erase=<경로 목록>`(쉼표로 나누되 `[...]`·따옴표 안의 쉼표는 나누지 않는다, 같은 옵션 여러 번 허용 — 목록 옵션), `--force`(asset-set — 미해결 import를 null로 저장). 공통 `--project=<dir>`·`--wait=<초>`(10진, 0–86400 범위 — 밖이면 사용법 오류; 쓰기 명령은 그 시간까지 잠금을 기다리고 읽기 명령은 형식만 본다). 대입의 진단 위치(sourceIndex)는 대입 인자 순서다.
        - 출력: asset-new는 표준 출력에 `<논리 경로> <AssetId>` 한 줄, asset-set은 출력 없음, asset-info는 서비스 출력 그대로(줄 또는 JSON + 줄 끝), asset-list는 기존 줄(`<논리 경로> <AssetId> <대표 타입 id>` — A2b와 같은 형식, 타입 등록과 무관하게 같은 줄). 진단은 표준 오류에 `<명령>: <코드>: <대상>: <설명>` 한 줄씩이다 — 대입 진단은 코드가 `TextDiagnosticCodeName`, 대상이 대입 텍스트의 경로, 에셋 진단은 코드가 `AssetDiagnosticName`(None이면 원본 `ErrorCodeName`). 설명은 진단 코드가 있으면 원본 `ErrorCodeName`, 없으면(코드 자리가 이미 원본 오류) `failed`이고 관련 대상이 있으면 ` (related <AssetId>)`를 붙인다. 경고(열기·저장)는 `<명령>: <코드>: <AssetId>: warning <종류> (related <AssetId>)`로 표준 오류에 쓰고 종료 코드는 0이다. 환경 실패도 같은 형식이다(`Busy: <프로젝트>: the project is locked by another process…`, `<오류>: <프로젝트>: cannot lock the project`, `<오류>: <Content>: cannot scan the content directory`, `<오류>: Animation: cannot register the asset types`). asset-new는 타입 이름을 서비스 전에 TypeRegistry에서 찾는다(`<오류>: <Type>: unknown type` — 저장 단계의 NotFound와 섞지 않게, A2c3b 리뷰 필수), 에셋 클래스 판정은 `TypeMismatch: <Type>: not a concrete asset class`. 종료 코드 0 성공·1 실행 실패(잠금 Busy 포함)·2 사용법 오류. 쓰기 명령(asset-new·asset-set)은 프로젝트를 연 바로 뒤, 색인을 만들기 전에 잠근다(7·8절 — 쓰기 판정은 잠금 아래에서 본 디스크로, A2c3b 리뷰 필수: 처음 구현은 색인 검색 뒤에 잠가 `--wait` 동안 바뀐 디스크를 놓칠 수 있었다).
        - 타입 등록: Commandlet이 Launch의 프로세스 모듈 로더에서 에셋 타입 모듈(지금 `Animation` — AssetRuntime은 그 의존)을 시작하고 명령마다 `TypeRegistry` + `ModuleTypeRegistrations`를 붙였다 뗀다(WorldBench와 같은 방식, ModuleLoader 3.11절). Name pool은 명령마다 하나. hook 표는 빈 표(타입 모듈의 hook 등록 길은 A5와 함께). asset-list는 `--type`이 있을 때만 타입을 등록한다(없으면 A2b와 같이 색인만).

16. **A2d 세부**(2026-11-21, 설계 — 교차 검증 1회 반영: 필수 5·권장 4): 셋으로 나눠 A2d1(캐시·`asset-index-rebuild`) → A2d3(감시 갱신 — 정확성 기능) → A2d2(병렬 범위 읽기 — 성능, 측정) 순으로 하고 부분마다 완료 조건 넷을 지킨다.
    - **A2d1 캐시**:
      - 형식: little endian 이진 단일 파일. 헤더 = magic `RKAI`, 형식 버전, 항목 스키마 버전(칸 배치), 판정 의미 버전 `AssetIndexCacheSemanticVersion`(색인 판정 규칙 — `ReadPackageIndex`·ImportRecord·Tags 파서·ImportOwner 대조·논리 경로 규칙 — 을 바꾸면 올린다: 초기 단계라 규칙이 자주 바뀌고 도구 빌드 ID는 키가 아니므로), Unicode 버전(NFC·caseless 표), 호환 엔진 버전(주·부 — NewerVersion 판정이 그것만 본다, 패치가 올라도 무효가 아니다. 엔진 축을 끈 실행(빈 엔진 버전)은 정의상 주·부 0이라 켠 실행과 키가 갈린다 — A2d1 리뷰가 권한 따로 된 엔진 축 칸은 같은 정보라 두지 않았다), 읽기 옵션 customVersions 목록의 SHA-256, 이름 정규화 플래그(`FileSystemNormalizesNames`), 도구 빌드 ID(진단용 문자열 — 키 아님), 항목 수, 본문 byte, 본문 SHA-256. 키 하나라도 다르면 캐시 없음과 같다(오류 아님).
      - 색인 읽기는 TypeRegistry를 쓰지 않는다(`Rebuild`가 `options.types`를 비운다 — 색인은 형식 판정만 하고 타입 축은 열기가 판정한다, 그래서 타입 버전이 키가 아니다).
      - 본문(항목마다, 파일 이름 byte 순 — 엄격 증가): Content 상대 OS 이름, 지문, id·대표 타입·readOnly·dependencies·tags·ImportRecord 요약. 파일 크기·수정 시각은 저장하지 않는다(재사용 때 지금 Stat·범위 읽기 값을 쓴다 — 구현에서 정함). 논리 경로·key는 저장하지 않고 파일 이름에서 다시 만든다(`TryMakeLogicalPath`). **진단이 하나라도 있는 파일은 넣지 않는다**(항목과 진단을 함께 가진 파일 — Tags·ImportRecord·ImportOwner 진단 — 의 진단이 캐시 실행에서 사라지지 않게, 다음 시작에 다시 읽는다).
      - 신뢰하지 않는 입력: 본문 SHA-256, 개수·길이는 파일 크기와 상한으로 묶고, 문자열 UTF-8, 의존 종류·태그는 Tags 스키마 규칙(빈 키 없음·키 256 byte·값 패키지 문자열 상한·(키, 값) 엄격 증가), null AssetId·0 대표 타입 거부, 파일 이름은 `.rasset`·이름 규칙으로 검사한다. 판정 의미 버전 정책은 리뷰 검토 항목이다 — `PackageReader`·`ImportRecord`·`PackageTags`·`LogicalPath`의 색인 판정을 바꾸는 변경은 `AssetIndexCacheSemanticVersion`을 함께 올렸는지 본다(A2d1 리뷰 권장 — 자동 검사는 두지 않았다). 하나라도 어긋나면 캐시 전체를 없는 것으로 본다(부분 사용 없음).
      - 재사용: `Rebuild(files, options, cache)`가 `.rasset`마다 `ReadPackageFingerprint`(범위 읽기 — 헤더 + 섹션 표(섹션별 CRC32) + tableCrc의 SHA-256)로 지문을 얻어 캐시의 같은 이름·같은 지문이면 캐시 항목을 쓰고(크기·시각은 지금 Stat 값), 아니면 지금처럼 부분 읽기. 지문 읽기 실패면 부분 읽기(그것이 진단을 만든다). 같은 id·경로 진단은 게시 때 다시 정한다. **한계**: 재사용의 민감도는 섹션 CRC32 수준이다 — 표가 그대로인 섹션 내용 변경(같은 크기·같은 CRC의 우연, bit rot)은 놓친다. 그래서 점검 명령(`asset-index-verify`)과 `asset-index-rebuild`는 캐시를 쓰지 않는다(목록·열기·편집 명령은 쓴다). 크기·수정 시각만 보는 UE `FDiskCachedAssetData` 방식은 7.1절 11 리뷰대로 버린다(파일마다 열기 + 범위 읽기 비용 — A2d2 측정이 본다).
      - 쓰기: 잠금 보유 프로세스만(`IsHeldFor` 아니면 InvalidArgument), 임시 파일 `Intermediate/AssetIndex.cache.<guid>.tmp` → 쓰기·Flush·Close → Replace(원자 교체 — 읽는 쪽은 옛 것 또는 새 것만 본다, 지문 대조 덕에 낡은 항목도 안전). NotDurable은 성공으로 본다(캐시). 쓰는 시점: `AssetService::FlushCache(lock)`를 명령이 끝날 때 부른다(저장마다 O(N) 전체를 쓰지 않는다 — UE `FAssetDataGatherer`도 검색 끝·간격으로만 쓴다), 실패는 경고 `CacheNotWritten`(캐시는 정확성에 필요 없다). 에디터는 종료·간격으로 쓴다(그때 정한다).
      - `asset-index-rebuild [--project] [--wait]`: 쓰기 잠금, 캐시를 쓰지 않는 전체 검색, Content 전체의 고아 임시 파일(`*.rasset.<guid>.tmp` — 저장의 모양 판정을 공용으로 옮긴다)과 캐시 임시 파일 고아(`Intermediate/AssetIndex.cache.<guid>.tmp`) 지우기(지울 수 없으면 경고), 캐시 쓰기. 출력: 진단은 표준 오류(`asset-index-verify`와 같은 줄), 요약 `asset-index-rebuild: <n> packages, <m> diagnostics, <k> orphans removed`. 종료 0(진단이 있어도 — 13절 "경고만 있으면 0", 점검은 verify), 잠금·IO·캐시 쓰기 실패 1.
      - 버린 대안: Record 직렬화(스키마 진화가 필요 없고 고정 이진이 손상 판정이 단순하다), 실패한 파일의 진단 캐시(드물고 일시적 원인이 많다), UE식 shard·ref 분할 캐시(이 규모에는 단일 파일 + 원자 교체가 단순하다), 캐시 쓰기에 잠금 없음(UE — Rocks는 8절대로 잠금 보유자만 바꾼다).
    - **A2d3 감시 갱신**:
      - `AssetIndexWatch`(AssetDatabase): `AssetIndex&`·`FileSystem&`·읽기 옵션·`AssetChangeSource&`(Open(root)·Poll(visitor)·Close — `PlatformAssetChangeSource`가 `platform::DirectoryWatcher`를 감싼다, 시험은 가짜)·`AssetClock&`. 감시가 바꾸는 것은 색인뿐이다 — 이미 열린 패키지의 외부 변경은 저장 1단계(지문)와 지연 읽기가 잡는다.
      - `Start()`: 원천을 Content에 연다 → Rebuild(캐시 사용) → 그 사이 쌓인 이벤트는 다음 `Pump`가 적용(7절 순서 — 검색 중 바뀐 파일은 이벤트로 다시 Stat되고 지문이 같으면 무동작, 자기 저장의 이벤트도 같다).
      - `Pump()`: 이벤트 종류를 믿지 않는다(레벨 트리거). 경로마다 "다시 볼 곳": `.rasset`이면 파일, 디렉터리이거나 모르면 하위 트리, Renamed는 `oldPath`와 `path` 둘 다, `Overflow(path)`는 그 하위 트리(빈 path면 전체), `RootRemoved`는 원천을 닫고 진단 `RootLost`(새 코드)·색인을 비우고(파일이 진실 — UE도 콘텐츠 경로가 사라지면 그 에셋을 뺀다, 열린 패키지는 그대로) 이후 `Pump`가 1초(AssetClock) 간격으로 다시 열어 보고 열리면 전체 다시 검색한다.
      - 경로 대조: 이벤트 경로는 OS byte 그대로 오므로 색인 항목의 `fileName`(OS byte)과 byte로 대조한다(NFC·caseless 키로 대조하면 Linux에서 대소문자만 다른 두 파일이 합쳐진다). 이름 정규화 FS(macOS — `FileSystemNormalizesNames`)에서만 NFC 대조를 더한다.
      - 새 API `AssetIndex::RefreshPaths(files, fileNames, subtrees, options)`: PrepareRefresh·CommitRefresh의 일반화(여러 파일·하위 트리를 한 번에 준비·확정 — 하위 트리는 다시 열거해 없어진 항목을 뺀다). 교체 뒤 실패 없음 계약을 잇는다(할당 실패면 색인 그대로). 기존 Refresh는 그 특수한 경우다.
      - 쓰는 중인 파일: 다시 읽기가 CorruptPackage·Busy면 대기 목록에 넣고, 크기·시각이 같은 관찰이 `AssetClock`으로 500 ms(7절 debounce와 같은 값) 이상 떨어져 두 번 나오면 다시 읽는다(Pump 횟수가 아니라 시간 — 에디터 tick마다 Pump해도 쓰는 도중을 안정으로 보지 않게). 처음 본 뒤 10초를 넘기면 다시 읽어 그 진단을 남기고 뺀다. sleep 없음 — 호출자가 Pump를 돈다.
      - 시험: 가짜 원천으로 이벤트 종류 × 순서 × Overflow·RootRemoved·재열기·쓰는 중 대기(수동 시계), 실제 감시 한 번(Wait는 이벤트 gate, 상한은 실패 판정용으로만). 상태: **구현** — 대조표 14.1.29절, coverage 14.1.30절.
      - 구현 세부(2026-11-21, 권장안으로 확정):
        - `RefreshPaths` 입력 `AssetIndexRefreshPaths{files, subtrees, mayDefer, deferred, normalizesNames}`. 이름은 Content 상대 규칙(유효한 UTF-8, 앞뒤 `/`·빈 구성 요소·`.`·`..` 없음 — 하위 트리만 빈 문자열 = Content 전체 허용), 어기면 InvalidArgument. 같은 이름 여럿·트리 안 트리·트리 안 파일은 정렬 대조로 한 번만 읽는다(이벤트 수천 개에도 O(n log n) — 쌍마다 대조하지 않는다). 하위 트리의 시작 이름은 먼저 Stat: 없음 → 비어 있음, 링크·그 밖의 종류 → 그 아래 없음, `.rasset` 파일 → 그 파일, 디렉터리 → 걷기, 그 밖의 Stat 실패 → 진단 None. Content 전체(빈 이름)의 열거 실패만 오류다(Rebuild와 같다). 캐시는 쓰지 않는다 — 같은 지문 파일도 다시 읽는다(결과가 같으므로 정확성 문제는 없고 비용은 A2d2가 본다).
        - 빼는 것의 대조: 항목은 OS 이름, 진단은 표시 이름(`AssetIndexDiagnostic::fileName`)으로 한다. 하위 트리 아래 진단은 유효한 UTF-8 이름의 표시와 잘못된 UTF-8 이름(0x80 이상 byte까지 escape)의 표시 둘 다로 접두를 본다 — 표시 escape는 `/`를 바꾸지 않고, 유효한 이름의 표시에는 0x80 이상 byte의 escape가 없고 역슬래시는 `\x5c`로 바뀌므로 다른 이름과 겹치지 않는다. `normalizesNames`(기본 `FileSystemNormalizesNames`)면 두 쪽을 NFC로 대조한다(`/`는 결합하지 않아 구성 요소 경계가 유지된다). 새 항목의 이름은 이벤트 철자다(한계).
        - 쓰는 중 대기: `mayDefer`가 참인 파일의 다시 읽기가 패키지를 읽지 못해(항목 없이 진단 하나) CorruptPackage이거나 진단 None·Busy면(읽혀서 항목이 있는 파일의 Tags·ImportRecord 손상 진단은 다 쓴 파일이라 미루지 않는다) 그 파일의 지금 항목·진단을 바꾸지 않고(기다리는 동안 옛 항목이 남는다 — 저장 도중 에셋이 사라졌다 나타나지 않게) `deferred`에 위치를 붙인다. 같은 이름 중 하나라도 거짓이면 미루지 않는다.
        - `AssetIndex::MarkRootLost()`: 항목을 비우고 진단을 `RootLost`(이름 빈 문자열, 원본 오류 NotFound) 하나로 둔다. 할당 실패면 색인 그대로이고 감시가 다음 Pump에 다시 한다. 진단 줄은 `RootLost: Content/: the content directory disappeared`.
        - 감시: `Start(cache)`는 원천 열기 → Rebuild(실패하면 원천을 닫는다). `Pump()`는 시작 전 InvalidArgument. 원천 Poll 오류와 이벤트 기록의 할당 실패는 전체 다시 검색으로 접는다(`DirectoryWatcher`가 메모리 부족을 루트 Overflow로 접는 것과 같다 — 이벤트는 늘 소비한다). 유효한 UTF-8이 아닌 경로는 첫 잘못된 구성 요소 앞 디렉터리(없으면 전체), 규칙에 안 맞는 경로는 전체다. 다시 읽기 실패면 색인 그대로·모은 일 유지(다음 Pump가 다시). 새 대기 목록은 확정 전에 모두 만든다(확정 뒤 실패 없음). 대기 관찰은 복사본에서 하고 성공할 때만 바꾼다(실패하면 대기 그대로 — 다시 읽을 것이 없는 Pump도 관찰은 남긴다, 버리면 안정 판정이 끝나지 않는다). 다시 읽은 이름·덮은 트리·이미 있는 대기의 대조는 정렬 표의 이진 검색이다(A2d3 리뷰 필수 — 이벤트·대기가 수만 개여도 쌍마다 대조하지 않는다).
        - 대기 파일은 Pump마다 관찰한다: 처음 본 뒤 10초 → 다시 읽기(미루지 않음), Stat 실패·일반 파일 아님 → 다시 읽기, 크기·시각이 지난 관찰과 다르면 관찰 갱신, 같고 500 ms 이상 지났으면 다시 읽기. 다시 읽은 결과(손상이어도)를 남기고 대기에서 뺀다. 새 이벤트는 다시 미룰 수 있다(처음 본 시각은 유지 — 10초 상한은 그대로). 다시 검색한 하위 트리가 덮는 대기 파일은 뺀다(검색이 결과를 남겼다 — 대조는 byte, 정규화 철자 차이면 상한까지 남을 뿐이다).
        - 루트 소실 뒤: 1초 간격으로 다시 열기(NotFound는 조용히 다음 간격, 그 밖의 원천 오류는 Pump 오류), 열리면 `RefreshPaths(전체)` — 실패하면 RootLost 색인 그대로이고 다음 간격에 검색만 다시 한다. 시계가 뒤로 가면 기준 시각을 지금으로 당긴다(기다림이 길어질 뿐).
        - `PrepareRefresh`도 `options.types`를 비운다(Rebuild·캐시와 같은 형식 판정만 — A2d1의 결정을 저장 갱신에도 맞춤).
    - **A2d2 병렬 범위 읽기**: Rebuild를 걷기(파일 목록)와 읽기로 나누고 읽기를 `WorkerScheduler`(주입 — 없으면 직렬)로 파일마다 지문·부분 읽기를 해 파일별 결과 칸에 둔다. 결과는 목록 순서로 모아 지금과 같은 정렬·게시를 한다(직렬과 byte 같은 결과를 단언). 계약: 색인 allocator와 `FileSystem` 모두 동시 호출이 안전해야 한다(`PhysicalFileSystem`은 그렇다 — 시험 `MemoryFileSystem`은 잠금을 더하거나 직렬 시험만). 할당 실패는 어느 작업이든 전체 실패(색인 그대로). 완료 조건에 큰 합성 프로젝트(예: 5만 패키지)의 직렬 대 병렬 측정 수치를 넣는다.
      - 구현 세부(2026-11-21, 권장안으로 확정):
        - 주입: `AssetIndex(allocator, layout, WorkerScheduler* scheduler = nullptr)` — 색인 하나가 쓰는 모든 검색(Rebuild·RefreshPaths의 하위 트리)에 같은 scheduler. `AssetServiceConfig::scheduler`가 서비스의 색인에 넘긴다. scheduler가 있으면 색인 allocator·`FileSystem`·배치(`ProjectLayout`)의 allocator(논리 경로 만들기가 쓴다)가 동시 호출을 받아야 한다(헤더 계약 — `SystemAllocator`·`PhysicalFileSystem`은 받는다).
        - 나눔: `ScanTree`가 (1) 걷기 — 디렉터리 열거·잘못된 이름·열거 실패 진단은 호출 스레드가 지금처럼(깊이 우선), 패키지 후보는 (Content 상대 이름, 수정 시각) 목록으로 모은다, (2) 읽기 — 후보마다 결과 칸(항목 0·1개 + 그 파일 진단)에 캐시 재사용 또는 부분 읽기를 한다. 읽기는 `ParallelFor(scheduler, allocator, 후보 수, minBatchSize = 8)`(UE `FAssetDataGatherer`가 파일 묶음을 작업 스레드로 읽는 것과 같은 자리 — 우선순위·취소는 두지 않는다), scheduler가 없거나 후보가 batch 하나 이하면 같은 본문을 호출 스레드가 차례로 돈다. (3) 모으기 — 칸을 후보 순서대로 found·diagnostics에 옮긴다. 게시가 정렬하므로 결과는 직렬과 같다(시험이 항목·진단 모든 칸으로 단언).
        - 실패: 칸 하나의 할당 실패는 그 batch 본문이 OutOfMemory를 돌려 ParallelFor가 가장 앞 실패를 돌려주고(남은 batch는 시작하지 않는다) 검색 전체가 실패한다(Rebuild는 비운 색인, RefreshPaths는 색인 그대로 — 지금 계약과 같다). 할당 실패 시험은 스레드 안전한 실패 allocator(원자 시도 수)로 병렬 경로를 돈다.
        - `RefreshPaths`의 파일 목록 다시 읽기는 직렬로 둔다(이벤트 파일 수가 작고 미루기 판정이 파일마다 있다 — 측정에서 필요가 보이면 같은 나눔을 쓴다).
        - 측정 도구 `Programs/AssetIndexBench`(제품 밖 — fixture 타입으로 합성 프로젝트를 만들고 Rebuild를 직렬·병렬(worker 수별)·캐시로 잰다).
        - 측정(2026-11-21, Windows 11, 논리 프로세서 20개, NVMe, Release, 패키지 5만 개·태그 둘·디렉터리 25개, OS 파일 캐시가 따뜻한 5회 중앙값): 중앙값(최소–최대) — 직렬 2.37 s(2.30–2.39) · 캐시 1.74 s(1.71–1.78), worker 1 + 호출 스레드 1.45 s(1.44–1.51) · 1.05 s(1.01–1.06), **worker 3 + 1 1.13 s(1.10–1.15) · 0.77 s(0.70–1.09)**, worker 7 + 1 1.45 s(1.19–1.52) · 0.90 s(0.73–1.24), worker 19 + 1 1.58 s(1.47–1.73) · 1.11 s(1.07–1.24). 캐시 없는 worker 3과 7은 범위가 겹치지 않는다(캐시 사용 쪽은 첫 round가 OS 캐시를 데워 최댓값이 퍼진다). worker 3개에서 직렬의 약 2.1–2.3배이고 더 늘리면 느려진다(파일 열기·범위 읽기가 OS 쪽에서 겹쳐 막힌다고 본다 — 원인 분석은 후속, 차가운 디스크 수치도 후속).
        - 결정(측정 근거): 명령(Commandlet)은 색인을 만드는 모든 명령(asset-list·asset-index-verify·asset-index-rebuild·편집 세션)에서 worker `CommandletScanWorkers` = 3개(논리 프로세서 − 1이 더 작으면 그 수, 0이면 직렬)의 scheduler를 띄워 넘긴다. 띄우지 못하면 직렬로 계속한다(결과가 같으므로 경고도 없다). 에디터 서비스는 엔진 scheduler를 넘기는 쪽에서 같은 상한을 쓴다(에디터 연결 때). 상태: **구현** — 대조표 14.1.31절, coverage 14.1.32절.

17. **A2e 세부**(2026-11-22, 설계 — 교차 검증 1회 반영: 필수 6·권장 9): 넷으로 나눠 A2e1(별칭 표) → A2e2(Detach·이동 — 이름을 바꿔 색인하는 갱신 준비 포함) → A2e3(메모리 참조 간선 표·삭제·복제) → A2e4(ID 수정·고아 출력) 순으로 하고 부분마다 완료 조건 넷을 지킨다(뒤 단계가 앞 단계의 별칭·Detach를 쓴다). UE 5.8 대조는 조사 결과(AssetRenameManager·ObjectTools·AssetDeleteModel·AssetViewUtils)다 — Rocks는 참조가 AssetId라 UE의 참조자 고치기(fix-up)가 필요 없고, 별칭은 텍스트 경로(설정·스크립트·명령 입력)만을 위한 것이라는 차이에서 갈리는 곳만 적는다. 원칙(A2c2b와 같다): 되돌릴 수 없는 단계(OS Move·Delete) 뒤에는 실패가 없게, 그 앞에서 할당·검사·별칭 쓰기·색인 준비를 끝낸다.

    - **A2e1 별칭 표**(`AssetAliasTable`, AssetDatabase):
      - 파일 `Config/AssetAliases.json` — 도구가 다시 쓰는 파일이라 엄격 JSON(JSON5 정책 — 손으로 고치는 파일만 JSON5). 모양: `{"version": 1, "aliases": [{"path": "/Game/Old", "asset": "<36자 소문자 AssetId>", "created": "2026-11-22T00:00:00Z", "days": 90}, …]}`. 항목은 경로 키(NFC·caseless) byte 순, 같은 키는 하나. 결정적 출력(`JsonWriter` 기본 옵션 — 들여쓰기 2칸·LF)으로 항목 순서가 고정이라 병합 충돌이 항목 단위로 생긴다(한 줄 한 항목은 JsonWriter가 지원하지 않는다 — 항목마다 6줄). 빈 표도 `{"version": 1, "aliases": []}`로 쓴다(버전 관리 대상 파일이 생겼다 사라지지 않게).
      - 읽기: 파일이 없으면 빈 표. 읽기 검증(신뢰하지 않는 입력): version 1, 항목 칸 넷이 모두 있고 다른 키 없음, path는 논리 경로 규칙(마운트 `/Game`), asset은 36자 소문자 non-null, created는 ISO 8601 UTC(`TryParseIso8601`), days는 1–36500, 경로 키 중복 없음, 항목 수 상한 `MaxAssetAliases` = 2²⁰, 파일 상한 `MaxAssetAliasFileBytes` = 64 MiB. 위반은 `InvalidArgument` + 진단(파일·항목 번호·칸). 정렬이 어긋난 파일(손으로 병합)은 받고 다음 쓰기가 정렬한다.
      - 쓰기: 잠금 보유자만(`IsHeldFor`), 임시 파일 `Config/AssetAliases.json.<guid>.tmp` → Flush·Close → Replace(원자 — 저장·캐시와 같은 길), NotDurable은 경고. 쓰기 직전에 파일을 다시 읽어 마지막 적재·쓰기 때 byte(있음·없음 포함)와 다르면(git 병합·손 편집 — 서비스는 표를 오래 쥔다) 쓰지 않고 `ExternalModification` / `StaleHandle`(다시 적재한 뒤 다시 한다 — 패키지 저장 5.5절 1과 같은 원칙, A2e1 리뷰 필수). 메모리 표는 쓰기가 성공해야 바뀐다(준비·확정). 이전 파일 byte를 기억해 두어 호출자가 되돌릴 수 있다(`Restore` — 이동·삭제가 OS 단계 실패 때 쓴다, 되돌리기 실패는 경고 `AliasNotRestored` — 되돌리기도 마지막 쓰기 뒤 파일이 바뀌었으면 하지 않는다).
      - 해석: `AssetTextResolver::ResolveId`와 `AssetService::OpenTarget`이 논리 경로를 색인에서 못 찾을 때만(NotFound — 같은 키 둘·형식 위반은 그대로 오류) 별칭 표를 읽어 그 AssetId로 계속하고 경고 `AliasResolved`(옛 경로 텍스트를 경고에 직접 담는다 — 뒤에 별칭 표가 바뀌어도 그대로, 대상 id)를 warnings에 붙인다. 별칭 표를 읽지 못하면 그 진단으로 실패하고 경고 `AliasTableUnreadable`도 붙인다(참조 값 해석은 진단 칸 없이 불려 대입 진단 Unresolved만 남으므로 원인을 보이게 — A2e1 리뷰 권장). 색인에서 찾은 경로는 별칭 파일이 깨져 있어도 오류가 아니다(별칭 표는 지연 적재 — 처음 필요할 때 읽고 그 오류는 그 호출의 오류다). 별칭 사슬은 없다(이동마다 옛 경로 → id를 직접 둔다 — A → B → C 이동이면 A·B 둘 다 id). 별칭 키와 실제 패키지가 같은 경로면(손으로 파일을 둔 경우) 색인이 이기고 list는 그 항목에 `shadowed`를 붙인다.
      - 점유: 검사는 `AssetService`(New·Move·Duplicate)가 한다 — `EditorPackageSet`(라이브러리 API)은 별칭을 모르므로 에디터도 경로를 만들거나 옮길 때 서비스를 거친다. 대상 경로가 별칭 키면 `DuplicatePath` / `AlreadyExists`(related = 별칭 대상 id) — 7.1절 12. 예외: 이동하는 에셋 자신을 가리키는 별칭 자리로 되돌아가기는 허용하고 그 별칭을 지운다(UE `TryMoveRedirector`와 같다). UE는 다른 대상의 같은 클래스 리다이렉터를 조용히 지우고 덮지만 Rocks는 거부한다(텍스트 참조가 다른 에셋으로 바뀌지 않게).
      - 만료: 항목은 만든 UTC와 만료 일수(기본 `AssetAliasDefaultDays` = 90, `AssetServiceConfig::aliasDays`)를 가진다. 시각은 `AssetServiceConfig::clock`(`AssetClock` — 없으면 시스템 시계, 시험은 수동 시계)이다. `asset-alias-prune`은 `created + days ≤ 지금`인 항목을 고른 뒤 텍스트 참조 검색으로 쓰인 곳을 찾는다 — 검색 대상은 `Config/` 아래 일반 파일(링크는 따라가지 않음, `AssetAliases.json`·임시 파일 제외, 파일 하나 64 MiB·파일 수 `MaxAliasSearchFiles` = 65536·총 byte `MaxAliasSearchBytes` = 1 GiB 상한 — 넘으면 검색을 멈추고 경고 `AliasSearchTruncated`, 그때는 아무것도 지우지 않는다)과 스크립트 루트(Sparks 소스 위치가 정해지면 더한다 — 지금은 `Config/`뿐). 대조는 경로 텍스트의 대소문자 무시 byte 일치(ASCII만 — 비 ASCII 대소문자·NFD 철자 차이는 놓친다, 한계)이고 뒤가 구성 요소 경계(`.`·`"`·`'`·공백·줄끝·`,`·`)`·`]`·파일 끝)다. 앞 경계는 보지 않는다(쓰임으로 잘못 보면 지우지 않는 쪽이라 안전하다). 검색은 파일을 한 번 훑어 마운트(`/Game`)로 시작하는 후보를 뒤 경계까지(논리 경로 상한 256 byte — 넘으면 별칭일 수 없다) 소문자로 잘라 만료 키 정렬 표에서 이진 검색한다 — 논리 경로에는 경계 문자가 없으므로 위 판정과 같고 비용은 byte × log(만료 항목 수)다(A2e1 리뷰 권장 — 처음 구현은 만료 항목마다 파일 전체를 훑었다). 쓰인 항목은 지우지 않고 경고 `AliasInUse`(`<파일>:<줄>` — 파일 이름은 제어 문자 escape)로 남긴다(지우면 그 텍스트가 깨진다 — 강제 삭제는 `asset-alias-remove`). 쓰이지 않은 만료 항목만 지운다.
      - 명령: `asset-alias-list`(잠금 없음 — `<옛 경로> <AssetId> <created> <만료 날짜>[ expired][ shadowed] -> <지금 경로|missing>` 경로 키 순), `asset-alias-remove <옛 path>`(잠금 — 없으면 NotFound 1), `asset-alias-prune`(잠금 — 요약 `asset-alias-prune: N removed, M in use, K kept`).
      - 서비스: `AssetService`가 별칭 표를 소유한다(지연 적재 — Initialize는 읽지 않는다, 실패하면 다음 호출이 다시 읽는다). `Aliases`·`AliasRemove`·`AliasPrune`와 이동·삭제가 쓰는 준비·쓰기·되돌리기(`AssetAliasTable::Write`·`Restore`). 목록은 명령이 표와 색인으로 만든다(타입 등록 없이). 별칭 쓰기 임시 파일은 `asset-index-rebuild`의 고아 정리가 지운다(Config 바로 아래). 색인 캐시와 별칭 표는 원자 쓰기 함수 하나를 함께 쓴다(`AtomicFileWrite`). 상태: **구현** — 대조표 14.1.33절, coverage 14.1.34절.
    - **A2e2 Detach·이동**(`AssetService::Move`, `EditorPackageSet::DetachUnder`·`PrepareRelocate`·`CommitRelocate`):
      - 대상: `<path|디렉터리>` → `<새 path>`(새 경로 전체 — "기존 디렉터리 안으로 넣기"(UE 폴더 드래그)는 지원하지 않는다). 원본이 색인의 패키지 경로면 파일 이동, 집합의 저장하지 않은 새 패키지 경로면 경로 표만 바꾸기(파일·텍스트 참조가 없었으므로 별칭 없음), 아니고 논리 디렉터리(그 아래 색인 항목·저장하지 않은 새 패키지·Content 디렉터리가 있는 경로)면 디렉터리 이동. 셋 다 아니면 NotFound. 같은 경로 키 둘(DuplicatePath)인 원본은 AlreadyExists. 판정 순서가 우선순위다 — `/Game/Hero`가 패키지(`Hero.rasset`)이면서 디렉터리(`Hero/…`)이기도 하면 패키지만 옮긴다(UE에서도 둘은 별개다). 그 디렉터리만 옮기는 표기(끝 '/' 등)는 없다 — 패키지를 먼저 다른 이름으로 옮긴다(A2e2 리뷰 권장).
      - 검사(아무것도 바꾸기 전): 잠금, 대상 마운트 `/Game`, 대상이 원본 자신 아래(디렉터리를 자기 안으로)면 InvalidArgument, 대상 경로 키가 원본과 같으면 대소문자·정규화만 바꾸는 이름 변경(허용 — 별칭을 만들지 않는다, UE와 같다), 아니면 대상이 비어 있어야 한다 — 색인 항목·Content 파일·디렉터리(Stat)·저장하지 않은 새 패키지 경로·별칭(자기 별칭 예외) 어느 것도 없어야 한다(`DuplicatePath` / `AlreadyExists`). 디렉터리면 옮길 패키지마다 새 논리 경로·OS 이름을 만들어(길이·규칙 — InvalidPath) 모두 통과해야 한다. 원본 아래 열린 dirty 패키지·저장하지 않은 새 패키지도 대상이다(내용은 메모리 — 다음 저장이 새 경로에 쓴다, 7.1절 6).
      - 실행(되돌릴 수 없는 OS Move 앞에서 실패할 수 있는 것을 모두 끝낸다): (1) 집합 경로 표 고치기 준비(`Relocate` — 할당), (2) 색인 갱신 준비 — `PrepareRefreshPaths`에 이름 바꾸기 입력(`AssetIndexRefreshPaths::moves` — 원본 이름의 파일·하위 트리를 읽어 대상 이름으로 색인하고 원본 이름 항목·진단을 뺀다, 저장의 readPath와 같은 생각), (3) 원본 아래 열린 패키지마다 `Detach`(저장소의 남은 bulk를 메모리로 읽고 파일 핸들을 닫는다 — 실패하면 그때까지 Detach한 것은 그대로 두고 오류, 아무것도 옮기지 않았다), (4) 별칭 표 준비·쓰기(옮긴 패키지마다 옛 경로 → id, 대소문자만 바꾼 이름이면 없음, 자기 별칭 자리면 그 항목 지움), (5) 대상 부모 디렉터리 만들기(MakeDirectoryTree), (6) OS `Move(replaceExisting = false)` 한 번(디렉터리도 한 번 — UE는 에셋마다 옮기지만 Rocks는 참조가 AssetId라 파일 묶음을 한 번에 옮겨 중간 상태가 없다, 디스크에 원본 디렉터리가 없으면(저장하지 않은 패키지뿐) 건너뛴다) — 실패하면 별칭 파일을 이전 byte로 되돌리고(실패는 경고 `AliasNotRestored`) 그 오류, (7) 확정: 집합 경로 표·색인(실패 없음). Detach는 되돌리지 않는다(메모리로 옮긴 bulk는 동작이 같다).
      - 원본 디렉터리 안의 패키지 아닌 파일·하위 디렉터리도 같이 옮겨진다(OS 이동 그대로). 원본 디렉터리 자체가 옮겨지므로 빈 디렉터리가 남지 않는다. 색인 준비와 OS Move 사이에 다른 프로세스가 원본을 바꾸면 그 변경은 다음 검색이 고친다(잠금은 같은 프로젝트 도구끼리의 약속이다 — 8절).
      - 명령 `asset-move <path|디렉터리> <새 path>`(잠금, 요약 `asset-move: N packages moved, M aliases added`, 캐시 쓰기).
      - 구현 세부(2026-11-22, 권장안으로 확정):
        - 색인: `AssetIndexRefreshPaths::moves`(`AssetIndexMove{from, to}` — Content 상대 이름): from·to 이름 자신과 그 아래의 항목·진단을 빼고, 지금 from에 있는 것을 하위 트리 검색과 같은 규칙으로 읽어 to 아래 이름으로 더한다(`ScanTree`의 `renameTo` — 이름은 바꾸고 내용은 지금 이름에서 읽는다, 저장의 readPath와 같은 생각). 같은 키가 아니면 서로의 아래일 수 없다(InvalidArgument). 이동 전에 `PrepareRefreshPaths`로 준비하고 OS 이동 뒤 `CommitRefresh`로 확정한다.
        - 집합: `EditorPackageSet::PrepareRelocate(from, to, fromFile, toFile, plan)`·`CommitRelocate`(`EditorRelocation`, 확정은 옮기기만). 저장된 패키지(논리 경로를 들고 있지 않다 — 색인 항목의 파일 이름이 권위)는 `fileName`이 `fromFile`(옮기는 실제 OS 이름 — 파일 또는 디렉터리)과 같거나 그 아래면 그 앞부분을 `toFile`로 바꾼다 — OS 이동과 같은 규칙이라 디스크의 실제 이름(NFC가 아닌 하위 이름 포함)과 정확히 맞는다. 저장하지 않은 새 패키지는 `logicalPath`(첫 저장의 경로)가 from과 같거나 그 아래면 to + 나머지 구성 요소(키와 텍스트의 byte 수가 달라 구성 요소 수로 자른다)로 바꾼다(규칙 위반은 InvalidPath / InvalidArgument — 서비스는 이 판정을 먼저 한다). `DetachUnder(fromFile)`은 같은 대조로 저장된 열린 패키지의 저장소를 `Detach`한다. 저장은 `fileName`의 파일이 열 때의 지문인지 보므로(5.5절 1) 옮긴 파일은 같은 지문이라 다음 저장이 새 경로에 쓴다. 처음 구현은 저장된 패키지도 `logicalPath`로 대조해 열린 패키지가 있으면 이동이 InvalidArgument로 실패했다(시험이 찾음).
        - 서비스: `AssetService::Move(lock, source, target, result, warnings, failure)`. 원본 판정 순서: 색인의 패키지 경로 → 저장하지 않은 새 패키지(같은 키) → 논리 디렉터리(그 아래 색인 항목·새 패키지, 또는 Content 디렉터리). 디렉터리의 OS 이름은 그 아래 항목 파일 이름의 앞 구성 요소(실제 철자 — 대소문자 구분 FS), 항목이 없으면 배치 규칙(새 패키지 하나는 파일 이름 `.rasset`, 디렉터리는 디렉터리 이름 — 대소문자 구분 FS에서 실제 철자가 다른, 색인 항목 없는 디렉터리(손상·패키지 아닌 파일뿐)는 찾지 못해 NotFound다, 한계 — A2e2 리뷰 권장). 저장하지 않은 새 패키지 하나인데 그 파일 이름이 디스크에 있으면 색인이 낡은 것이라(색인에 없는 파일) 그 파일을 옮기지 않고 `DuplicatePath` / `AlreadyExists`다(`asset-index-rebuild` 뒤 다시 한다 — 처음 구현은 원본 이름을 디렉터리 이름으로 만들어 같은 이름의 디렉터리를 옮길 수 있었다, 리뷰 권장을 보다 찾음). 논리 디렉터리가 대소문자만 다른 OS 디렉터리 둘 이상(대소문자 구분 FS)에 걸치면 한 번의 OS 이동으로 옮길 수 없어 `DuplicatePath` / `AlreadyExists`(그 항목 id — 하나로 합친 뒤 옮긴다). 원본 Stat이 없음이면 디스크에 없는 것이다: 새 패키지뿐이면 OS 이동·색인 갱신을 건너뛰고, 파일이 있어야 할 색인 항목이 있으면 `MissingFile` / NotFound(그 id). 그 밖의 Stat 오류는 그 오류다. 새 경로마다 규칙(길이)과 별칭 점유(같은 에셋의 별칭이면 지움)를 본다. 별칭 만든 시각은 설정 시계이고 날짜 범위 밖이면 그 오류(Overflow)다. OS 경로 문자열·경고 자리(한 칸 — 한 호출의 경고는 많아야 하나)는 별칭을 쓰기 전에 만든다 — 별칭을 쓴 뒤의 실패는 OS 단계뿐이고 확정 뒤에는 할당 실패가 없다(처음 구현은 확정 뒤 NotDurable 경고를 붙이다 실패하면 옮긴 뒤 OutOfMemory를 돌려줄 수 있었다 — 커버리지 검토가 찾음). 대상 부모 디렉터리는 만든다(UE도 폴더를 만든다). 그 뒤 OS Move가 실패하면 만든 부모(빈 디렉터리)는 남는다 — 색인에 항목이 없어 해롭지 않고, 지우기도 실패할 수 있는 OS 단계라 두는 쪽을 택했다(A2e2 리뷰 권장).
        - 경고: OS 이동 실패 뒤 별칭 되돌리기가 실패하면 `EditorWarningKind::AliasNotRestored`(진단 None과 그 오류 — 남은 별칭은 옮기지 못한 옛 경로 → 그 에셋이라 색인이 이겨 해롭지 않다), 별칭 쓰기의 NotDurable은 `EditorWarningKind::NotDurable`.
      - 상태: **구현** — 대조표 14.1.35절, coverage 14.1.36절.
    - **A2e3 메모리 참조 간선 표·삭제·복제**:
      - 메모리 간선 표: `EditorPackageSet`이 자기 `ReferenceObserver`의 변화(`TakeChanges`)로 패키지 쌍별 강한 간선 다중도 표를 유지한다(AssetManager 상주 단위 표와 같은 방식 — 저장하지 않은 편집 포함). `IncomingReferences(package)`가 대상 패키지로 들어오는 다른 패키지의 간선 수다.
      - 삭제 `AssetService::Delete(lock, target, force)`: 대상은 패키지 하나(`<path|AssetId>`). 디스크 참조자 = 색인 `Referencers(id, 모든 종류)`에서 자기 자신 제외(UE `GetReferencers(Package)` — 하드·소프트 모두), ImportOwner 참조자(대상이 임포트 소유자)도 포함한다(지우면 출력이 고아가 된다). 있으면 거부(`InvalidArgument`, 참조자마다 진단 줄 `ReferencedBy`), `--force`면 지우고 참조자는 다음 점검에서 `MissingDependency`(UE 강제 삭제는 로드된 참조만 null로 바꾸고 디스크의 참조자는 건드리지 않는다 — 디스크를 건드리지 않는 것은 같다). 메모리 참조: 대상이 열려 있고 다른 열린 패키지가 대상 객체를 참조하면(`IncomingReferences` ≠ 0) `--force`여도 `Busy`(객체를 버리면 그 참조가 끊긴 채 저장된다 — UE처럼 null로 바꾸고 dirty로 표시하는 것은 하지 않는다, 그 패키지들을 닫거나 참조를 고친 뒤 지운다). 실행: 검사 → 대상을 가리키는 별칭 지우기 준비·쓰기(UE는 리다이렉터를 남기고 그것이 참조자로 일반 삭제를 막지만, Rocks 별칭은 텍스트용이라 갈 곳이 없어진 항목을 지운다 — 경고 `AliasRemoved`) → 색인 갱신 준비 → Detach → 파일 Delete(실패하면 별칭 되돌리기 후 그 오류) → 확정(색인, 집합에서 닫기 — Discard). 저장하지 않은 새 패키지는 집합에서만 닫는다. 읽기 전용 패키지도 지울 수 있다(내용을 잃는 쓰기가 아니다).
      - 복제 `AssetService::Duplicate(lock, source, target)` → 새 AssetId: 원본을 열고(Hard 닫힘째) 그 메모리 상태(저장하지 않은 편집 포함 — UE `DuplicateSingleObject`와 같다)를 새 id로 대상 경로에 새 파일로 쓴다. writer 옵션 `PackageWriteOptions::duplicate`(AssetRuntime): 헤더 id를 바꾸고(`assetId`), `DuplicateTransient`·`NonTryoutDuplicateTransient` 필드는 클래스 기본값으로 쓴다(UE `DuplicateObject`가 복사하지 않는 필드 — 리플렉션 B9 1절 표), 자기 패키지(원본 id)를 가리키는 Soft 참조는 새 id로 바꾼다(UE `DuplicateAsset`이 패키지 안을 가리키는 soft path를 고치는 것과 같다 — 바꾸지 않으면 사본이 원본을 가리킨다). 패키지 안 Hard·Weak 참조는 export 번호라 그대로 새 사본을 가리키고 밖 참조는 그대로다(UE와 같다). ImportRecord: 소유 기록(owner = 원본)이면 owner와 outputs의 원본 id를 새 id로 바꿔 쓴다(원본 경로를 유지하는 것은 UE AssetImportData와 같다), 참조 기록(다른 임포트의 출력)이면 기록과 ImportOwner 의존을 버리고 경고 `ImportRecordDropped`(그대로 두면 소유자의 outputs에 없어 곧바로 고아다). 읽기 전용 원본은 `NewerVersion`(4.4절), 미해결 import가 있으면 저장과 같이 거부. 대상 점유 검사는 이동과 같다. 새 파일은 저장과 같은 길(임시 → Replace)로 쓰고 색인 Refresh, 집합에는 열지 않는다.
      - 명령 `asset-delete <path> [--force]`, `asset-duplicate <path> <새 path>`(출력: 새 경로와 AssetId 한 줄).
      - 단계: A2e3a(간선 표·닫기·삭제)와 A2e3b(writer 복제 옵션·복제)로 나눠 각각 리뷰한다.
      - A2e3a 구현 세부(2026-11-22, 권장안으로 확정):
        - 간선 표: `EditorPackageSet`이 `(소유 패키지, 대상 패키지) → 다중도` 표(HashMap)를 갖는다. `SyncReferences()`가 관찰자 묶음을 꺼내(`TakeChanges`) 쌍별로 합친 뒤(원인·트랜잭션으로 나뉜 같은 쌍 — AssetManager `CoalesceChanges`와 같다) 표 용량을 먼저 잡고 무할당으로 적용한다 — 할당이 실패하면 묶음을 쥔 채 다음 호출이 다시 한다(잃지 않는다). 음수 다중도는 Registry 결함이라 Fatal이다. `IncomingReferences(id, referencer)`는 동기화한 뒤 다른 열린 패키지에서 그 패키지로 들어오는 다중도 합과 첫 참조자(id 순)다(열리지 않은 id는 0). 관찰자는 키 상한(`observerMaxKeys`)까지만 쌓으므로 Open(새 패키지)·Create·Save·Close가 시작할 때 동기화한다 — 실패하면 그 호출이 아무것도 바꾸지 않고 그 오류다(A2e3a 리뷰 권장: host의 주기 호출에만 기대지 않는다. 끝에서 부르고 실패를 무시하는 길은 버렸다 — 성공한 호출 뒤에 삼킨 할당 실패가 남는다). 그래서 관찰자는 그 호출 사이의 편집만 쌓는다.
        - 닫기: `EditorPackageSet::Close(id)` — 들어오는 참조가 있으면 `ReferencedBy` / Busy(related = 첫 참조자), 아니면 그 패키지를 Discard하고(되돌릴 수 없다 — 저장하지 않은 편집도 버린다) 파일 캐시 핸들을 닫고 표에서 뺀다. Discard의 오류(할당·관찰자 키 상한)면 아무것도 바뀌지 않는다.
        - 색인: `AssetIndexRefreshPaths::removed`(Content 상대 이름 — 그 이름과 그 아래의 항목·진단을 읽지 않고 뺀다). 지우기 전에 준비하고 지운 뒤 확정한다.
        - 서비스 `AssetService::Delete(lock, target, force, result, warnings, failure)`: 대상 텍스트는 `<path|AssetId>`이고 별칭으로 풀지 않는다(지우는 대상은 지금 이름으로 고른다 — 옛 경로는 NotFound). 원본 판정은 색인 항목(같은 id 둘 `DuplicateAssetId` / AlreadyExists — 먼저 `asset-fix-duplicate-id`, 같은 경로 키 둘 `DuplicatePath` / AlreadyExists) → 저장하지 않은 새 패키지(id·경로 키). 디스크 참조자 = 색인 `Referencers(id, 모든 종류)` − 자신(경로 키 순 — `result.referencers`에 경로·id 사본), 있으면 `ReferencedBy` / InvalidArgument(related = 첫 참조자 — force면 지우고 목록만 준다). 메모리 참조(`IncomingReferences`)는 force여도 `ReferencedBy` / Busy. 저장하지 않은 메모리 편집의 Soft 참조는 디스크 참조자에 없다(한계 — 저장된 상태만 본다, UE도 디스크 참조자는 저장된 패키지에서 찾는다).
        - 순서: 검사 → 대상을 가리키는 별칭 키 모으기 → 색인 준비(`removed`) → 경고 자리(별칭 수만큼 — 그 뒤 할당 실패 없음) → 별칭 쓰기 → 닫기(Discard — 할당이 있어 파일 지우기 뒤로 미루지 않는다, 실패하면 별칭 되돌리기 뒤 그 오류) → 파일 Delete(이미 없으면 — 낡은 색인·밖에서 지움 — 바라는 상태라 성공으로 보고 색인을 확정한다(A2e3a 리뷰 권장), 그 밖의 실패면 별칭 되돌리기 뒤 그 오류 — 패키지는 이미 닫혔고 파일·색인·별칭 그대로라 다시 열 수 있다, 사용자가 지우라고 한 패키지의 메모리 편집을 버리는 것은 의도와 같다) → 색인 확정. 위 7.1절 17 실행 순서(Delete 뒤 닫기)에서 바꿨다 — Discard가 실패할 수 있어 파일을 지운 뒤에는 둘 수 없다. 되돌리기 실패는 `AliasNotRestored` 경고.
        - 경고: 지운 별칭마다 `EditorWarningKind::AliasRemoved`(text = 별칭 경로), 별칭 쓰기 NotDurable은 `NotDurable`. 결과 `AssetDeleteResult`: 지운 경로·id, 별칭 수, 디스크 참조자 목록(경로·id 사본 — 색인이 바뀌어도 유효).
        - 명령 `asset-delete <path|AssetId> [--force]`: 성공 `asset-delete: deleted <path> <AssetId>`, 참조자마다 줄 `ReferencedBy: <참조자 path>: <id> references <대상 path>`(거부면 오류 줄 뒤 종료 1, force면 `warning` 줄), 경고 줄(AliasRemoved 등), 캐시 쓰기. 참조자 줄은 13절 진단 줄 형식 `<명령>: <코드>: <대상>: <설명>`에서 대상 자리가 참조자 경로이고 설명이 `<참조자 id> references <지울 경로>`(force면 앞에 `warning `)다 — 예: `asset-delete: ReferencedBy: /Game/Anims/Run: 5e57…0b02 references /Game/Characters/Hero`, 그 뒤 거부 진단 줄 `asset-delete: ReferencedBy: /Game/Characters/Hero: InvalidArgument (related 5e57…0b02)`.
        - 저장하지 않은 편집의 Soft 참조(열린 패키지)는 강한 간선이 아니고 디스크 참조자도 아니라 삭제를 막지 않는다(한계 — 위 서비스 항목, 시험으로 고정). ImportOwner 의존만 있는 참조자(다른 임포트의 출력)도 디스크 참조자다.
        - 자기 참조: 색인 참조자에 자신은 나오지 않는다(writer가 자기 의존을 빼고 reader가 거부한다 — 5.4절) — 삭제는 자기 제외 판정을 두지 않는다(처음 구현의 그 판정은 닿을 수 없었다).
        - 상태: **구현** — 대조표 14.1.37절, coverage 14.1.38절.
      - A2e3b 구현 세부(2026-11-22, 권장안으로 확정):
        - Core 치환 hook: `FieldCodecHooks::SubstituteSoftReference(info) → AssetId`(기본은 `info.asset` 그대로 — 저장은 객체를 바꾸지 않는다). 저장 문맥 `FieldEncodeContext::SubstituteSoftReference`가 null이 아닌 Soft 값에만, hook이 있고 Required 검사 모드가 아닐 때 부르고(struct 치환 `SubstituteStruct`와 같은 조건) 바뀐 id를 쓴 뒤 수집(`CollectSoftReference`)도 바뀐 id로 한다. hook이 null id를 돌려주면 InvalidArgument(경로만 남은 값은 `SoftObjectRef` 규칙이 거부하는 모양 — 복제의 치환은 id만 바꾼다). Map 키·Set 원소의 Soft 값도 치환한다 — 순서는 삽입 순서라 바뀌지 않고, 새 id는 새로 발급한 것이라 키가 겹치지 않는다. 버린 대안: 쓰기 전에 메모리 객체의 Soft 값을 바꿨다 되돌리기(사용자 객체·undo를 건드린다), Objects 섹션 byte를 고쳐 쓰기(codec 밖에서 wire를 안다).
        - writer: `PackageWriteOptions::duplicateId`(에디터 패키지만 — 쿠킹이면 InvalidArgument, 패키지의 지금 id와 같으면 InvalidArgument). 주면 헤더 id가 그것이고, 제외 필드 플래그에 `DuplicateTransient`·`NonTryoutDuplicateTransient`를 더하며(쓰지 않으므로 로드가 클래스 기본값 — UE `DuplicateObject`), 지금 id를 가리키는 Soft 값을 그것으로 치환한다(자기 의존 제외도 새 id로 한다 — 사본의 자기 Soft 참조는 의존이 아니다).
        - 집합: `EditorPackageSet::Duplicate(source, target, newId, lock, warnings, failure)` — 열린 원본의 메모리 상태를 새 파일로 쓴다(사본은 열지 않는다). 검사는 저장과 같다(읽기 전용 `NewerVersion` / Unsupported, 미해결 import `MissingDependency` / InvalidArgument — 복제에는 강제 옵션이 없다), newId는 null이 아니고 열려 있지도 색인에 있지도 않아야 한다(`DuplicateAssetId` / AlreadyExists), 대상은 새 패키지의 첫 저장과 같은 판정(파일 있음·색인 같은 경로 키·저장하지 않은 새 패키지 경로 — `DuplicatePath` / AlreadyExists, OS 경로 상한 InvalidPath / CapacityExceeded). ImportRecord: 소유 기록(owner = 원본)이면 owner를 newId로 하고 outputs에는 원본 자신의 출력 하나만(outputKey 그대로, asset = newId) 남겨 다시 쓴다 — 형제 출력(다중 출력 임포트의 다른 에셋)은 원본 임포트의 것이라 빼고(남기면 사본이 남의 출력의 소유를 주장해 사본 재임포트가 원본의 형제를 덮어쓴다 — A2e3b 리뷰 필수, UE `AssetImportData`가 유지하는 것도 원본 파일 경로이지 형제의 소속이 아니다), 사본을 재임포트할 때 빠진 형제 출력을 새로 만들지는 A2f에서 정한다. 참조 기록이면 기록과 ImportOwner 의존을 버리고 경고 `ImportRecordDropped`(related = 그 소유자). 쓰기는 저장과 같은 길(임시 → 재검증 → Replace → 색인 Refresh → 대상 디렉터리의 고아 임시 파일 정리)이고 원본 정보(파일·지문·dirty·저장소)는 바꾸지 않는다. 타입 hook(Tags·Build 의존)은 원본 객체로 부르되 문맥 `AssetTypeHookContext::asset`에 저장할 id(사본이면 newId, 저장이면 그 패키지 id)를 준다 — 패키지 정체성이 필요한 hook은 객체 값 대신 이것을 쓴다(A2e3b 리뷰 권장) — 저장의 판정·쓰기를 원본/복제 둘 다 쓰게 나눴다(`PlanSave`의 새 파일 대상 판정과 hook 산출물, `WriteAndReplace`의 복제 모드).
        - 서비스 `AssetService::Duplicate(lock, source, target, newId, warnings, failure)`: 원본은 `<path|AssetId>`(열기와 같다 — 별칭을 따른다, 읽기는 파괴가 아니다), 대상 경로 규칙과 별칭 점유(어느 에셋의 별칭이든 — 새 에셋에는 별칭이 없다, related = 별칭 대상), 새 id 발급(`AssetId::TryNew`) 뒤 집합 `Duplicate`.
        - 명령 `asset-duplicate <path|AssetId> <new path>`(잠금): 표준 출력 `<새 경로> <AssetId>` 한 줄(asset-new와 같다), 경고 줄(ImportRecordDropped 등), 캐시 쓰기.
        - 원본 고르기의 저장하지 않은 새 패키지: 서비스의 `<path|AssetId>` 열기(`OpenTarget` — Info·Set·Duplicate 공통)가 같은 경로 키의 저장하지 않은 새 패키지를 그대로 준다(색인에 없다 — 처음 구현은 경로로 고를 수 없었다, 시험이 찾음).
        - 대상 점유의 새 파일 판정(`PlanNewTarget`)은 첫 저장과 복제가 같이 쓴다: 대상 파일이 있음·색인에 같은 경로 키(파일이 밖에서 지워진 낡은 색인도)는 DuplicatePath, Stat의 없음 밖 오류는 그 오류, 경로 상한은 InvalidPath / CapacityExceeded.
        - 시험 fixture: `asset_fixture::DuplicableAsset`(DuplicateTransient·NonTryoutDuplicateTransient 필드, 자기·남 Soft 참조, 제외 멤버를 가진 struct `DuplicableSlot` 필드·배열 — AssetTestFixtures, `ids add`로 발급).
        - 상태: **구현** — 대조표 14.1.39절, coverage 14.1.40절.
    - **A2e4 ID 수정·고아 출력**:
      - `asset-fix-duplicate-id <path>`: 그 경로 항목이 색인에서 `duplicateId`여야 한다(아니면 InvalidArgument). 같은 id 패키지는 집합에 동시에 둘 수 없으므로(Registry가 id로 찾는다) 그 파일을 색인 대조 없이 여는 길(`EditorPackageSet::OpenForIdFix(fileName)` — 같은 id의 다른 파일이 집합에 열려 있으면 Busy, 그 파일이 dirty면 진단 줄이 먼저 저장하라고 알린다)로 열어 새 id로 같은 경로에 다시 쓰고(복제와 같은 writer 옵션, 소유 ImportRecord의 owner·outputs도 바꾼다) 닫는다. 다른 패키지의 그 id 참조는 남은 쪽을 가리킨다(출력에 적는다). 대상이 다른 임포트의 출력이면 소유자의 outputs가 옛 id를 가리키므로 고아가 된다(경고 `OrphanedOutput`).
      - 고아 출력: 색인 ImportRecord 요약에 outputs의 AssetId 목록을 더한다(캐시 형식 버전·판정 의미 버전을 올린다). 고아 = 참조 기록(owner ≠ 자신)인 항목 가운데 owner가 색인에 없거나, owner 항목의 기록이 없거나, owner 기록의 outputs에 자신이 없는 것. `AssetIndex::Orphans(out)`, `Verify`가 고아마다 `OrphanedOutput` 진단(경고)을 더한다 — `asset-index-verify`는 진단 줄을 쓰되 경고만이면 종료 0(13절 "경고만 있으면 0").
      - 명령 `asset-orphans-list`(잠금 없음 — `<path> <AssetId> owner <owner path|AssetId|missing>`), `asset-orphans-delete <path>`(잠금 — 고아가 아니면 InvalidArgument, 그 밖은 삭제와 같은 규칙, 참조자가 있으면 거부 — 강제는 asset-delete --force).
      - 단계: A2e4a(고아 출력·명령 둘)와 A2e4b(`asset-fix-duplicate-id`)로 나눠 각각 리뷰한다.
      - A2e4a 구현 세부(2026-11-22, 권장안으로 확정):
        - 색인 요약: `AssetIndexImportSummary::outputs`(출력 AssetId 목록 — 기록의 outputKey byte 순, 참조 기록은 빈 목록)가 출력 수 칸을 대신한다. 캐시는 항목 칸이 바뀌므로 항목 스키마 버전을 올린다(`AssetIndexCacheSchemaVersion` 2 — 헤더 주석 규칙, 위 "형식·판정 의미 버전"은 이 스키마 버전을 뜻한다 — 판정 규칙은 바뀌지 않는다). 캐시의 출력 수는 `MaxImportOutputs`(1024)를 넘으면 그 캐시를 버린다(신뢰하지 않는 입력).
        - 판정: 참조 기록(owner ≠ 자신)인 항목마다 owner 항목(같은 id 범위 — id 순 색인 `byId_`에서 이진 검색, 폴더를 통째로 복사해 모든 id가 겹쳐도 항목마다 그 범위만 본다)을 찾는다. 그 가운데 하나라도 자신을 적으면 고아가 아니다. 아니면 — 판정할 수 없음(`Undetermined` — 고아가 아니다: 목록·경고·지우기 대상이 아니다, A2e4a 리뷰 필수): owner가 색인에 없는데 그 id의 진단(읽지 못한 패키지 — 헤더 id는 읽었다)이나 id를 모르는 진단(손상·이름 규칙·열거 실패·사라진 파일)이 하나라도 있을 때(읽지 못한 파일이 owner일 수 있다), owner 항목은 있는데 그 id의 ImportRecord를 읽지 못했을 때(칸 ImportRecord 진단 — 더 새 스키마·형식 위반, 그 기록이 자신을 적었을 수 있다). 그 밖에 owner가 없으면 사유 1(OwnerMissing), 소유 기록이 하나라도 있으면 사유 3(NotListed), 없으면 사유 2(OwnerNotImporter). 진단은 그 파일이 캐시에 들어가지 않아 늘 지금 상태다(7.1절 16). `AssetIndex::Orphans(out)`(경로 키 순 — 진단 id 표를 한 번 만든다)와 `OrphanReason(entry)`(진단을 한 번 훑는다 — 할당 없음). 처음 구현은 읽지 못한 기록·패키지를 고아로 단정해 `asset-orphans-delete`가 정상 출력을 지울 수 있었고(동료의 더 새 임포터 기록 등), 같은 id owner를 찾으려 항목 전체를 훑어 O(n²)이 될 수 있었다.
        - 진단: `AssetDiagnosticCode::OrphanedOutput`(경고 — asset = 고아, relatedAsset = owner, detail = 사유). `Verify`가 고아마다 더한다. 줄은 `OrphanedOutput: Content/<파일>: warning the owning import <id> is not in the index` / `… has no import record of its own` / `… does not list this output`. `AssetIndexDiagnosticIsWarning`이 경고 종류를 정하고 `asset-index-verify`는 경고 아닌 진단이 있을 때만 종료 1이다(요약의 진단 수는 경고 포함).
        - 서비스 `AssetService::DeleteOrphan(lock, path, result, warnings, failure)`: 경로(별칭으로 풀지 않는다)의 색인 항목이 고아가 아니면 InvalidArgument(진단 `OrphanedOutput`이 아닌 None — 고아가 아닌 것은 진단이 없다, 같은 경로 키 둘은 DuplicatePath, 없음은 NotFound), 고아면 `Delete(lock, path, force = false)`와 같다(디스크·메모리 참조자가 있으면 거부 — 강제는 `asset-delete --force`).
        - 명령: `asset-orphans-list`(잠금 없음 — 표준 출력에 경로 키 순으로 `<path> <AssetId> owner <owner path|AssetId|missing>`, owner가 색인에 하나면 그 경로, 같은 id 둘 이상이면 AssetId, 없으면 missing, 요약 없음), `asset-orphans-delete <path>`(잠금 — 출력은 asset-delete와 같다).
        - 기존 시험 fixture의 영향: 색인 시험의 Hero는 색인에 없는 owner(ImportOwnerId)를 가리키는 참조 기록이라 이제 고아다 — 의존 대조 시험은 Verify 결과에서 고아 경고를 떼어 그 수를 따로 단언한다(`TakeOrphans`). 명령 시험 fixture(`AssetIndexCommandletFixtures.h`)에 고아 출력 둘(Orphan 2311 — owner 2312 없음, HeroOrphan 2313 — 기록 없는 Hero가 owner)을 더했다(기존 배열은 그대로).
        - 상태: **구현** — 대조표 14.1.41절, coverage 14.1.42절.
      - A2e4b 구현 세부(2026-11-22, 권장안으로 확정):
        - 집합: `EditorPackageSet::FixDuplicateId(fileName, newId, lock, warnings, failure)`. 그 파일의 색인 항목이 `duplicateId`여야 하고(아니면 InvalidArgument), 그 id가 집합에 열려 있으면(어느 파일이든 — Registry가 id로 찾는다) Busy(먼저 닫거나 저장 뒤 다시 연다 — 명령 프로세스는 열지 않은 채 시작한다), 색인 항목이 읽기 전용이면 `NewerVersion` / Unsupported, newId가 null이면 InvalidArgument, 열려 있거나 색인에 있으면 `DuplicateAssetId`. 열기는 그 파일 이름으로 한다(루트 패키지만 색인 id 조회 대신 주어진 파일 — 의존 닫힘은 열기와 같다, `OpenEntry`의 파일 지정). 미해결 import가 있으면 `MissingDependency` / InvalidArgument(복제와 같다 — 연 패키지는 닫는다).
        - ImportRecord: 소유 기록이면 owner와 outputs의 자기 출력을 newId로 바꾸고 형제 출력은 뺀다(복제와 같다 — 형제의 참조 기록은 옛 id를 가리키고, 옛 id는 남는 같은 id 파일의 것이다. 새 기록이 형제를 적으면 형제가 가리키지 않는 출력을 주장한다). 참조 기록이면 그대로 둔다(소유자의 outputs는 옛 id를 적으므로 이 패키지는 고아가 된다). 고아 경고는 쓰기 뒤 색인 판정(`AssetIndex::OrphanReason` — `asset-orphans-list`와 같은 판정, 할당 없음)으로 고친 파일의 새 항목과 형제 id 항목만 본다: 사유가 OwnerMissing·OwnerNotImporter·NotListed면 경고 `OrphanedOutput`(asset = 그 항목, related = 그 항목 기록의 owner, detail = 사유). 처음 설계는 형제를 늘 고아로 경고했으나 남는 같은 id 파일(대개 같은 기록을 가진 원본)이 형제를 적으므로 보통 고아가 아니다 — 남는 파일이 형제를 적지 않을 때(우연한 id 충돌)만 고아다. 경고 자리는 판정 항목 수(1 + 형제 id 항목 수 — 쓰기는 이 파일 항목만 바꾼다)만큼 쓰기 전에 잡는다.
        - 쓰기: 복제 모드 쓰기(writer `duplicateId` — 헤더·자기 Soft·DuplicateTransient)를 같은 경로에 교체로 한다. 교체 직전에 이번 열기가 연 패키지를 모두 닫는다(`CloseOpened` — 열기 전에 열린 id를 적어 두고 그 밖의 열린 패키지: 고칠 패키지와 그 의존 닫힘, Hard 순환 상대 포함. 처음 구현은 고칠 패키지의 참조자 닫힘만 닫아(`CloseGroup`) 이번에 연 의존이 열린 채 남았고 묶음 계산이 고정점 반복이었다 — A2e4b 리뷰 권장. 원래 열려 있던 패키지는 새로 연 패키지를 참조할 수 없다 — 열기는 기존 패키지의 미해결 import를 다시 묶지 않는다 — 그래서 끊긴 참조가 남지 않고 편집을 잃지 않는다). Discard는 할당이 있어 교체 뒤로 미루지 않는다 — 닫기가 실패하면 임시 파일을 지우고 그 오류, 파일·색인 그대로(중간 Discard 실패면 앞서 닫은 것은 닫힌 채이고 남은 것의 닫힌 패키지로의 참조는 Discard된 객체를 가리킨다(Resolve는 Destroyed) — 실패 경로가 같은 기록으로 남은 것을 다시 닫는다), 교체가 실패하면 패키지는 닫힌 채 파일·색인 그대로. 실패 경로(열기 뒤 판정·hook·쓰기 실패)도 이번에 연 것을 닫는다. 교체 뒤 색인 갱신으로 그 파일은 새 id가 되고 같은 id 진단이 사라진다.
        - 서비스 `AssetService::FixDuplicateId(lock, path, result, warnings, failure)`: 경로(별칭으로 풀지 않는다)의 색인 항목(같은 경로 키 둘은 DuplicatePath) → 새 id 발급 → 집합 `FixDuplicateId`. 결과 `AssetFixIdResult`: 경로·옛 id·새 id·옛 id를 쓰는 남은 항목 경로들(다른 패키지의 옛 id 참조는 그쪽을 가리킨다 — 출력에 적는다).
        - 명령 `asset-fix-duplicate-id <path>`(잠금): 표준 출력 `asset-fix-duplicate-id: <path> <옛 id> -> <새 id>` 한 줄과 남은 쪽마다 `asset-fix-duplicate-id: references to <옛 id> resolve to <남은 path>`(남은 것이 하나일 때 — 둘 이상이면 아직 같은 id라 `asset-fix-duplicate-id: <옛 id> is still shared by <남은 path>`, A2e4b 리뷰 권장), 경고 줄(OrphanedOutput 등), 캐시 쓰기.
        - ID 수정은 같은 파일을 다시 쓰므로 부모 디렉터리를 만들지 않는다(복제·첫 저장만 만든다).
        - 상태: **구현** — 대조표 14.1.43절, coverage 14.1.44절.
    - 시험 기반: 메모리 파일 시스템에 Move(파일·디렉터리, `replaceExisting` 거짓, 대소문자만 바꾸는 이름 변경 = 같은 노드, 하위 노드 경로 바꾸기)와 n번째 Move·Delete 실패·오류 종류 주입(Busy·AccessDenied·AlreadyExists)을 더한다. 실제 디스크 시험 하나: 읽지 않은 bulk가 있는 열린 패키지를 가진 디렉터리를 이동(Windows는 안에 열린 핸들이 있는 디렉터리 이름을 바꾸지 못한다 — Detach가 캐시 핸들을 모두 닫는지 본다).
    - 버린 대안: UE처럼 필요할 때만 별칭(Rocks는 텍스트 참조를 고칠 수 없으므로 늘 만든다), 이동 뒤 별칭 쓰기(실패하면 별칭을 더할 명령이 없어 텍스트 참조가 조용히 깨진다 — 교차 검증 필수), 에셋마다 디렉터리 이동(중간 상태가 남는다), 강제 삭제가 디스크 참조자를 null로 고쳐 쓰기(사용자가 모르는 많은 파일을 바꾼다 — UE도 디스크는 건드리지 않는다), 강제 삭제가 열린 참조자를 null·dirty로(UE 방식 — 사용자가 저장하면 조용히 참조를 잃는다, Busy로 먼저 알린다), 복제를 파일 byte 복사 + id 바꾸기로(저장하지 않은 편집을 잃는다), 별칭 점유를 `EditorPackageSet`에(라이브러리가 사용자 데이터 파일을 알게 된다), 별칭 파일 JSON5(도구가 다시 쓰는 파일은 엄격 JSON).

18. **A2f 세부**(2026-11-22, 설계 — 교차 검증 1회 반영: 필수 4·권장 7): 임포트 프레임워크. 셋으로 나눠 A2f1a(진단 등록·원본 루트·원본 읽기·임포터 표) → A2f1b(새 임포트·옵션·설정·`asset-import`·에셋 명령 모듈 이전) → A2f2(재임포트·임포트 상태·`asset-reimport`)로 한다. UE 대응: `UFactory`(형식 목록·`FactoryCreateBinary`)·`FReimportHandler`(`CanReimport`·`Reimport`)·`UAssetImportData`(원본 파일·해시)·Interchange 팩토리(기존 에셋을 같은 클래스로만 갱신). 이름은 Rocks식(`AssetImporter`·`AssetImporterTable`·`AssetSourceRoots`), UE 이름은 대응표에만.
    - 진단 등록(4.6절 이름 그대로 `AssetDiagnosticCode`에 더한다): `MissingSource`·`SourceChanged`·`InvalidSource`·`UnsupportedSourceFormat`·`AbsoluteSourcePath`(경고)·`ImporterOutdated`·`InvalidImportOption`·`UnknownProperty`. `LossySource`·`NonSrgbSource`·`IgnoredSourceChannels`·`SrgbIgnored`·`PreserveBorderWithWrap`은 A5 텍스처 임포터가 더한다.
    - **원본 루트**(`AssetSourceRoots`, 4.5절 — A2f1a): `Config/AssetSources.json` = `{"roots": {"<이름>": "<경로>"}}`(경로는 프로젝트 루트 기준 상대 또는 절대, `/` 또는 OS 구분자, 이름은 1–`MaxImportRootBytes` byte의 `[A-Za-z0-9_]`). 파일이 없거나 `Project`가 없으면 `Project` = 프로젝트 루트를 더한다(있으면 그 값). 읽기 실패·형식 위반은 그 명령 실패(`InvalidArgument`, 진단 `InvalidSource` — 파일·위치). 이 단계는 읽기만 한다(편집 명령 없음 — 사용자가 손으로 쓰는 버전 관리 데이터, 별칭 표와 같다).
      - 기록(절대 OS 경로 → (root, relative)): 경로를 정규화(`.`·`..` 접기, 구분자 `/`)한 뒤 경로 구성요소 단위로 가장 긴 접두 루트를 고른다. 비교는 경로 정책과 같은 규칙(7.1절 10 — 대소문자 무시 caseless 비교, 모든 플랫폼 — Windows·macOS 기본 FS가 대소문자를 무시하고, 대소문자만 다른 두 루트는 같은 루트로 본다)이고 같은 길이면 이름 byte 순 첫째다. relative는 루트 아래 `/` 구분 상대 경로(1–`MaxImportRelativePathBytes`). 어느 루트 아래도 아니면 root = 빈 문자열, relative = 정규화한 절대 경로, 경고 `AbsoluteSourcePath`.
      - 풀기((root, relative) → OS 경로): 모르는 root 이름은 `MissingSource`(detail = 원본 번호), 빈 root는 relative 그대로(절대 경로).
    - **원본 읽기**(A2f1a): 파일 전체를 읽는다(원본 하나 상한 `MaxImportSourceBytes` = 1 GiB, 한 임포트의 합 상한 `MaxImportTotalSourceBytes` = 2 GiB — 넘으면 `InvalidSource` / CapacityExceeded, UE도 원본을 통째로 메모리에 읽는다). SHA-256과 byte 수를 기록한다. 없으면 `MissingSource`, 읽기 IO 오류는 그 원본 오류(진단 `InvalidSource`).
    - **임포터 인터페이스**(AssetDatabase `AssetImporter.h` — A2f1a):
      - `AssetImporterInfo { Guid id; StringView name; uint32 version; TypeId outputType(주 출력 타입 — 등록 에셋 클래스); Span<const StringView> extensions(이 임포터가 기본이 되는 확장자 — 소문자, 점 없음, 빈 것 가능); Span<const StringView> roles(빈 = 원본 하나 "source"); TypeId optionsType(등록 ROCKS_STRUCT, 0 = 옵션 없음) }`.
      - `class AssetImporter { virtual const AssetImporterInfo& Info() const noexcept = 0; virtual Result<void> Import(AssetImportContext&) noexcept = 0; }` — IO를 하지 않고(원본 byte는 문맥이 준다) 결정적이어야 한다(같은 입력 → 같은 출력, 시험이 단언). 계약: 임포트가 정하는 필드는 매번 모두 쓴다(재임포트는 제자리 갱신이라 임포터가 쓰지 않은 옛 임포트 필드가 남는다 — UE와 같다).
      - `AssetImportContext`: `Sources()`(역할·byte·표시 경로), `Options()`(옵션 struct 값 포인터 — 읽기 전용), `Output(key, type)`(출력 객체 — 아래), `AddBulk(output, Array<uint8>&&)`(저장하지 않은 bulk, `BulkDataRef`), `Warn(code, detail)`(경고로 등록된 진단 코드만 — 아니면 InvalidArgument, `EditorWarning`으로 모인다), registry·access(필드 대입), `IsReimport()`.
      - `Output(key, type)`: 빈 key = 주 출력(요청 경로 — 소유 기록을 가진 패키지, type은 `outputType`이어야 한다), 그 밖은 `<path>_<key>`(7.1절 12). key 규칙은 ImportRecord outputKey와 같다(1–`MaxImportOutputKeyBytes`, `[A-Za-z0-9_]`), 같은 key 두 번은 InvalidArgument, 개수는 `MaxImportOutputs`까지. type은 등록 에셋 클래스(추상 아님). 새 임포트는 새 패키지(저장하지 않은 Create)의 공개 전 대표 에셋을, 재임포트는 기록의 같은 key 출력을 연 대표 에셋을 준다(아래 A2f2).
      - 표 `AssetImporterTable`(AssetTypeHookTable과 같은 모양 — 모듈이 등록, `Register`가 등록 해제 토큰): 같은 id·같은 이름(대소문자 무시)·같은 기본 확장자 둘은 `AlreadyExists`(UE `ImportPriority` 경합 대신 확장자마다 기본 하나 — 결정적). 기본 확장자가 없는 임포터(엔진 임포터를 대신하는 프로젝트 플러그인 등)는 `--importer=<이름>`으로만 고른다. 등록은 옵션 필드 이름이 `outputType`의 노출 속성 이름(대소문자 무시 — 13절 속성 이름 규칙)이나 명령 예약 이름(`source`·`importer`·`project`·`wait`·`unset`·`erase`·`force`·`help`와 A5의 `cube`·`array`·`cube-array`·`volume`·`cube-layout`·`volume-tile`)과 겹치면 InvalidArgument로 거부한다(옵션과 설정을 첫 이름으로 나눌 때 모호함이 없게 — 교차 검증 필수 1).
    - **옵션과 설정**(A2f1b): 명령의 `--<경로>=<값>`(13절)을 대입 경로의 첫 이름(대소문자 무시)으로 나눈다 — 옵션 struct의 필드면 옵션(ImportRecord.options — 바뀌면 재임포트), 아니면 주 출력 에셋의 저장 속성(임포트 뒤 대입 — 빌드·쿠킹 설정, 5.4절 옵션 경계). `--unset=`·`--erase=`의 경로 목록도 경로마다 같은 규칙으로 나눈다. 둘 다 아니면 `UnknownProperty`. 옵션은 `AssignmentTarget::Value`로, 설정은 공개 전 대표 에셋(`Staged` — 새 임포트)이나 열린 에셋(`Live` — 재임포트)에 리플렉션 B9 대입으로 한다(값 오류는 `InvalidImportOption`(옵션) / 대입 진단(설정)). 설정은 주 출력에만 한다(다른 출력의 설정은 `asset-set` — 한계). 옵션 Record는 `EncodeStructRecord`로 쓰고 `DecodeStructRecord`로 읽는다(모르는 필드·더 새 버전은 읽기 규칙대로 — 재임포트가 `InvalidImportOption`으로 실패, 기존 보존).
    - **A2f1b 새 임포트**(`AssetService::Import(lock, path, sources, importerName, assignments, diagnostics, result, warnings, failure)` — sources = (역할, OS 경로) 목록, importerName은 비면 확장자 기본): 경로 규칙·점유(New와 같다 — 색인·디스크·저장하지 않은 새 패키지·별칭) → 임포터 고르기(이름 또는 첫 원본 확장자의 기본 — 원본이 여럿이면 모든 원본 확장자의 기본이 같아야, 아니면 `UnsupportedSourceFormat`) → 역할 대조(임포터 roles와 정확히 같은 집합, 아니면 `InvalidImportOption`) → 원본 읽기·기록 → 옵션 대입 → `Import` 호출 → 출력 경로 점유 검사(주 출력 외 — 임포트 뒤에 알 수 있다, New와 같이 별칭까지 본다) → 설정 대입 → 저장.
      - 저장 순서: 주 출력이 아닌 출력을 먼저(참조 기록 owner = 주 출력 id + ImportOwner 의존), 주 출력을 마지막에(소유 기록 — importerId·name·version, sources, options Record, outputs 전체). 그래서 주 출력의 기록이 디스크에 없는 출력을 적는 일이 없다. 저장 전 실패(임포터 오류·검사·대입)면 만든 패키지를 모두 닫고 아무것도 쓰지 않는다. 중간 저장 실패면 이번에 쓴 출력 파일(이번 연산이 만든 새 파일 — 참조자가 없다)을 잠금 아래에서 지우고(색인 갱신), 지우지 못한 것만 고아(OwnerMissing)로 남기고 경고 `OrphanNotDeleted`(그 경로 — `asset-orphans-delete`로 지운다; 남으면 같은 명령을 다시 할 때 그 경로가 `DuplicatePath`). 다중 파일 교체를 원자적으로 할 수 없어서다(UE 임포트는 메모리에 dirty로 만들고 저장은 사용자가 하므로 이 문제가 저장 단계로 미뤄질 뿐이다).
      - 집합 API: `EditorPackageSet::SetImportRecord(id, bytes, owner)` — 열린 패키지의 기록과 ImportOwner를 바꾸고 dirty(저장이 쓴다).
      - 결과 `AssetImportResult { id, path, outputs[(key, id, path)] }`, 경고(AbsoluteSourcePath·임포터 경고·NotDurable·OrphanNotDeleted).
      - 명령 `asset-import <path> <source> [--importer=<이름>] [--<경로>=<값>…]`과 역할이 여럿인 임포터의 `asset-import <path> --source.<역할>=<파일>… [--importer=<이름>] [설정…]`(13절 문법에 더한다 — 텍스처의 `--cube`·`--array` 등 전용 문법은 A5가 이 형태로 바꿔 넘긴다). 출력: `asset-import: <path> <id>` + 출력마다 `asset-import: output <key> <path> <id>`.
      - 명령 시험(교차 검증 필수 4): 에셋 명령 구현(`Programs/Commandlet/Private/Common/Asset*Commandlet.*`·`AssetCommandletSupport.*`)을 Developer 모듈 `AssetCommandlets`(Engine/Source/Developer — 도구 전용, 런타임 층 아님)로 옮기고(동작 불변 이동 커밋을 먼저), 명령 문맥에 임포터 표를 둔다. Commandlet은 그 모듈을 링크하고 실제 임포터만 등록한다(시험 임포터를 제품 실행 파일에 넣지 않는다). EngineTests는 시험 임포터를 등록한 문맥과 출력 포착으로 `RunAssetImportCommand`·`RunAssetReimportCommand`를 프로세스 안에서 불러 성공 줄·출력 줄·경고 줄·캐시 쓰기를 확인한다. C# 시험은 제품 실행 파일의 인자·사용법·임포터 없음(`UnsupportedSourceFormat`)·원본 없음을 확인한다.
    - **A2f2 재임포트**(`AssetService::Reimport(lock, path, sourceOverrides, assignments, diagnostics, result, warnings, failure)`, 8 — UE 재임포트와 같이 제자리 갱신): 대상의 기록이 소유 기록이어야 한다(참조 기록이면 InvalidArgument, related = owner — "owner를 재임포트하라", 기록 없음도 InvalidArgument). 임포터는 기록의 importerId로 찾는다(없으면 `UnsupportedSourceFormat`, 기록 버전 > 등록 버전이면 `NewerVersion` / Unsupported — 옛 임포터가 새 데이터를 잃는다). 원본은 기록을 루트로 풀거나 `--source=`(원본 하나)·`--source.<역할>=`(기록에 없는 역할은 `InvalidImportOption`)으로 바꾼다 — 없으면 `MissingSource`로 실패·기존 보존. 옵션은 기록 Record를 읽고 명시 대입만 바꾼다(16.1절).
      - 출력 찾기(교차 검증 필수 2): 기록 outputs는 AssetId로 찾는다(경로가 아니다 — 출력이 이동했어도 따라간다). 색인에 없으면(지워짐) 그 key는 새 출력으로 다시 만든다(새 id·`<path>_<key>` — 기록을 새 id로 고친다). 같은 id 둘이면 `DuplicateAssetId`, 읽기 전용이면 `NewerVersion` / Unsupported, 그 패키지의 기록이 이 owner를 가리키는 참조 기록이 아니면(ID 수정·손 편집 뒤) InvalidArgument(asset = 그 출력, related = owner) — 모두 실패·기존 보존. 주 출력이나 출력이 열려 있고 dirty면 Busy(저장하지 않은 편집을 재임포트 저장에 섞지 않는다 — 먼저 저장하거나 버린다, UE는 로드된 객체에 덮어쓰고 저장은 사용자가 한다).
      - 출력 갱신: 임포터가 낸 key가 기록에 있고 타입이 같으면(TypeId 정확 일치 — 파생 아님) 그 패키지를 연 대표 에셋을 준다(임포터가 임포트로 정하는 필드만 덮어쓴다 — 설정·참조자의 ObjectRef 유지, bulk는 새로 더하고 이번에 참조하지 않게 된 옛 bulk는 저장 때 빠진다). 기록에 없는 key는 새 출력 패키지. 타입이 다르면 `TypeMismatch`로 실패·기존 보존(UE Interchange 팩토리도 기존 에셋을 같은 클래스로만 갱신한다 — 7.1절 8의 "새 객체로 바꾸고 진단"을 고친다: 같은 AssetId의 대표 타입을 바꾸면 참조자의 typed 참조가 깨진다, 지우고 다시 임포트한다). 기록에 있는데 이번에 내지 않은 출력은 경고 `OrphanedOutput`(그 패키지는 그대로 — 새 기록이 적지 않으므로 고아, `asset-orphans-*`로 처리, 16.1절).
      - 저장은 A2f1b와 같은 순서(바뀐 출력 → 주 출력). 기록은 원본 SHA-256·byte 수·importerVersion(지금 등록 버전)·옵션·outputs를 새로 쓴다. 중간 실패(교차 검증 필수 3): 이번에 새로 만든 key의 출력 파일은 A2f1b와 같이 지운다(못 지우면 `OrphanNotDeleted`). 이미 갱신한 기존 출력은 새 원본 내용으로 남고 주 출력 기록은 옛 SHA·옵션 그대로다 — 임포트 상태가 `SourceChanged`로 보이고, 같은 재임포트를 다시 하면 수렴한다(제자리 갱신은 멱등이다).
      - 임포트 상태 `AssetService::ImportStatus(path, out)` — 원본마다 current·`SourceChanged`(byte 수가 다르면 읽지 않고 곧바로, 같으면 SHA-256 비교 — 원본 시각은 VCS 체크아웃에서 믿을 수 없고 5.4절 스키마에 없으므로 기록하지 않는다)·`MissingSource`, 임포터 `ImporterOutdated`(등록 버전 > 기록 버전 — 자동 재임포트 없음, 16.1절)·`UnsupportedSourceFormat`(등록 없음). `asset-info`가 기록이 있는 패키지면 `import.importer=<이름> <기록 버전>`·`import.source.<역할>=<root>:<relative> <상태>` 줄을 덧붙인다(Edit 형식의 끝 — Report·JSON은 `import` 객체).
      - 명령 `asset-reimport <path> [--source=<파일> | --source.<역할>=<파일>…] [설정…]`: 출력 `asset-reimport: <path> <id>` + 새 출력 줄, 경고 줄.
      - 나눔과 고침(2026-11-22, 구현 착수 — 권장안으로 확정): A2f2a(재임포트 서비스·`asset-reimport`) → A2f2b(임포트 상태·`asset-info` import 줄). 대상(주 출력·색인에 있는 기록 출력)이 열려 있으면 dirty가 아니어도 Busy다(위의 "dirty면 Busy"를 고친다 — 임포터가 열린 깨끗한 패키지를 메모리에서 고친 뒤 실패하면 되돌릴 길이 없다: 닫기는 참조자가 있으면 실패하고, 열린 채 두면 메모리가 파일과 달라진다. A2e4b ID 수정의 "그 id가 열려 있으면 Busy"와 같다 — 에디터의 트랜잭션 되돌리기는 에디터 단계). 그래서 재임포트가 여는 패키지는 모두 이번에 연 것이라 실패하면 `CloseOpened`로 되돌린다(새로 만든 출력 파일은 A2f1b와 같이 지운다, 이미 저장한 기존 출력은 새 내용으로 남는다 — 위 중간 실패). 기록 원본의 역할 집합은 임포터 roles와 같아야 한다(임포터 버전이 역할을 바꿨으면 `InvalidImportOption` — 다시 임포트한다). 기록 옵션의 schema가 지금 옵션 struct가 아니거나(옵션 없는 임포터에 옵션 기록 포함) 버전이 더 새면 `InvalidImportOption`이다. `--source=`는 기록 원본이 하나일 때만 받는다.
    - 시험: C++ 서비스 시험이 시험 임포터(AssetTestFixtures — 원본 텍스트 `value=<n>`·`bulk=<hex>`·`outputs=<key>:<type>,…` → TestAsset, 옵션 struct `TestImportOptions{scale, failAt}`, 역할 둘 변형, 기본 확장자 없는 변형)로 모든 흐름·실패·할당 실패를 확인하고, EngineTests가 명령 함수를 프로세스 안에서 부른다(위).
    - A2f1a 상태: **구현** — 대조표 14.1.45절, coverage 14.1.46절. 구현에서 정한 것: 진단 여덟 개는 `OrphanedOutput` 뒤에 더했다. 표 파일·원본 읽기는 별칭 표의 `ReadWhole`을 모듈 공용(`WholeFileRead.cpp`)으로 옮겨 같이 쓴다. 루트 수 상한은 JSON 노드 상한이 막는다(넘으면 TooManyNodes → CapacityExceeded). 성분 수는 정규화한 `/` 경로의 빈 아닌 성분을 센다(드라이브 `C:`도 하나 — 상대 비교만 쓴다). `ReadImportSource`는 byte 상한 인자를 받는다(여러 원본은 남은 합계 예산을 준다). 원본 위치 검사는 `IsValidImportSourceLocation`(ImportRecord와 같은 판정)이다. 임포터 표의 옵션 struct 조회는 `AcquireValueType(…, Struct)`이고 종류 불일치(TypeMismatch)는 InvalidArgument로 돌려준다. 원본 위치 규칙을 고쳤다(빈 루트 = 절대 경로 — 5.7절 ImportRecord 배치 참고). 주 출력 타입의 속성 이름이 예약 이름과 같으면(예: 시험 TestAsset의 `source`) 그 설정은 `asset-import`가 아닌 `asset-set`으로 넣는다(한계 — 예약 이름은 명령 문법이 먼저 쓴다). 리뷰 반영(권장 넷): 루트 이름은 byte 그대로 기록·비교하므로 대소문자만 다른 두 이름(기본 `Project`와 겹치는 `project` 포함)을 InvalidArgument로 거부한다. Windows 장치 이름공간 경로(`\\?\C:\…`·`\\?\UNC\…`·`\\.\…`)는 Core `Path`가 Unsupported로 거부한다(루트 표·기록 위치 — UNC로 잘못 기록하지 않는다, 벗겨 쓰지 않는다: 그 형태는 사용자가 원본 경로로 줄 까닭이 없다). 기록 규칙이 `\`를 금지하므로 POSIX에서 이름에 `\`가 든 원본(`a\b.png`)은 기록할 수 없다(한계 — InvalidArgument). 원본 역할 문자 규칙은 지금 임포터 표(`[A-Za-z0-9_.]`)가 ImportRecord(길이·UTF-8만)보다 좁다 — A2f1b가 `--source.<역할>` 대조(대소문자 구분, byte 일치)를 정할 때 한 곳(ImportRecord 쪽 공개 판정)으로 모은다.
    - A2f1b 상태: **구현** — 대조표 14.1.47절, coverage 14.1.48절. 구현에서 정한 것:
      - 옵션 Record: schema = 옵션 struct id, version = 그 struct 버전, 필드 1(Guid{0, 1}) = 옵션 값의 리플렉션 JSON 텍스트(`ExportJson` — 들여쓴 정규 표기). 재임포트(A2f2)는 기본값 struct에 `ImportJson`으로 다시 대입한다(모르는 필드는 진단 → `InvalidImportOption`). 처음 생각한 필드 codec 직접 쓰기(`EncodeStructRecord`)는 버렸다 — 단독 struct decode가 객체 생성 트랜잭션(ConstructionContext)을 요구해 Core를 크게 넓혀야 하고, JSON 표기는 리플렉션 B9가 이미 struct 값 대상으로 시험한 공용 경로다.
      - 옵션 값은 임포터 표가 쥔 struct lease의 함수표(size·alignment·construct·destroy)로 만든다. 표는 등록 동안 주 출력·옵션 타입 lease를 쥔다(`OutputType`·`OptionsType` — 등록된 임포터의 타입 등록이 풀리지 않는다, 임포트 때 다시 조회하지 않는다).
      - 출력: `Output`이 그 자리에서 경로를 만들고 별칭 점유를 본 뒤 `EditorPackageSet::Create`(색인·디스크·저장하지 않은 새 패키지 점유 — New와 같다)로 저장하지 않은 패키지를 만들어 그 대표 에셋을 준다(Create는 패키지 핸들을 준다 — 대표 에셋은 이름 빈 자식). 설정 대입은 만든 뒤라 `Live` 대상이다(초안의 `Staged`가 아니다 — Create가 공개까지 하므로).
      - 실패하면 이번에 연 패키지를 `EditorPackageSet::CloseOpened`(A2e4b의 private 함수를 공개)로 닫는다. 집합 API `SetImportRecord(id, bytes, owner)`가 기록·ImportOwner를 바꾸고 dirty로 표시한다.
      - 임포터 경고는 새 경고 종류 `EditorWarningKind::ImportWarning`(진단 = 그 코드)이고, 받는 코드는 `AssetDiagnosticIsWarning`(AssetRuntime — 4.6절·MeshAnimationAssets 11절·UnrealExchange 표의 경고·정보 코드)이다. `AssetImportContext::Fail(code, error, detail)`이 임포터 실패의 진단을 남긴다(없으면 진단 None).
      - 저장 되돌리기: 출력마다 저장 전에 파일 이름 자리(`MaxNativePathBytes`)와 경고 자리(저장 경고 2 + 출력 수)를 잡는다 — 저장 뒤 이름 복사·되돌리기 경고가 실패하지 않는다(처음 구현은 저장 직후 이름 복사의 할당 실패로 그 파일을 되돌리기 목록에 넣지 못해 고아로 남겼다 — 할당 실패 시험이 찾았다).
      - 명령 세션: `AssetCommandContext`에 에셋 타입 모듈 이름(`typeModule` — 비면 `Animation`)과 임포터 등록 hook(`AssetImporterSetup` — 타입 등록 뒤·서비스 만들기 전, 토큰은 세션이 서비스 뒤에 해제)을 둔다. 제품 Commandlet은 hook이 없다(A5가 텍스처 임포터를 등록한다), EngineTests는 시험 타입 모듈(`AssetTestFixtures`)과 시험 임포터를 준다. 타입 모듈 시작 실패 줄은 그 모듈 이름을 쓴다.
      - 리뷰 반영(충족 — 권장 다섯): 주 경로의 별칭·색인·저장하지 않은 새 패키지 점유는 원본을 읽기 전에 본다(큰 원본을 헛되이 읽지 않게 — 디스크에만 있는 파일은 Output의 Create가 본다). 되돌리기는 할당하지 않는다 — OS 경로 버퍼(Content 경로 + `MaxNativePathBytes`)와 색인 갱신 이름 표를 저장 전에 잡는다. 되돌리기의 색인 갱신(RefreshPaths)이 실패하면(읽기 오류) 이 서비스의 색인은 다음 갱신·검색까지 지운 파일 항목을 남긴다(같은 경로 다시 임포트는 DuplicatePath — 새 프로세스·`asset-index-rebuild`는 맞춘다). 이번 임포트의 경고(절대 경로·임포터 경고)는 따로 모았다가 성공할 때만 넘긴다(실패한 임포트가 만들지 않은 id의 경고를 남기지 않는다 — 되돌리기의 OrphanNotDeleted와 저장의 NotDurable은 남는다). 남은 시험 임시 디렉터리는 중단된 실행의 잔재였다.
      - A2f2에 넘기는 것: 옵션 Record는 모든 필드를 적으므로 새 임포터 버전의 옵션 기본값 변경은 재임포트에 반영되지 않는다(16.1절 기존 옵션 유지 — UE와 같다, 바꾸려면 명시 대입).
      - 명령 문법: `--<경로>=<값>` 설정(첫 이름이 예약 이름이면 사용법 오류 — `--cube` 등은 A5), `--unset=`·`--erase=`, `--source.<역할>=`(같은 역할 둘·빈 역할·빈 파일은 사용법 오류, 위치 인자 원본과 섞지 않는다), `--importer=`. 상대 원본 경로는 작업 디렉터리 기준 절대 경로로 바꾼다(ProjectLayout과 같다).
    - A2f2a 상태: **구현** — 대조표 14.1.49절, coverage 14.1.50절. 구현에서 정한 것:
      - 새 임포트와 재임포트는 실행 본문 하나(`AssetService::RunImport(ImportRun&)` — 원본 읽기·옵션·임포터 호출·저장·되돌리기)를 같이 쓰고, 재임포트는 그 앞에 기록 판정과 기존 출력 열기를 더한다. 문맥의 `Output`은 기록에 있는 key면 연 기존 패키지의 대표 에셋을(같은 타입), 없으면 새 패키지를 준다.
      - 판정 순서(앞에서 실패하면 뒤는 보지 않는다 — 모두 파일·색인·열린 패키지 그대로): 경로·색인 → 주 출력 같은 id 둘·읽기 전용·소유 기록 → 열림 Busy → 임포터(기록 id — 없으면 `UnsupportedSourceFormat`, 기록 버전이 더 새면 `NewerVersion`) → 기록 출력마다(같은 id 둘·읽기 전용·기록이 이 owner의 참조 기록이 아님·열림 Busy — 색인에 없으면 새로 만든다) → 별칭·루트 표 → 주 출력 열기 → 미해결 import → 기록 원본·역할·바꾸기 → 기존 출력 열기 → 실행.
      - 주 출력에 미해결 import가 있으면 임포터를 부르기 전에 `MissingDependency`(related = 첫 대상)로 거부한다 — 저장이 미해결 import를 거부하므로(복제·ID 수정과 같은 규칙) 임포터를 헛되이 돌리지 않는다. 진단의 대상은 패키지를 닫기 전에 복사한다.
      - 옵션 Record의 필드 1이 없거나 wire가 String이 아니면 InvalidArgument(`InvalidImportOption`)다. schema가 다르면 TypeMismatch, 버전이 더 새면 Unsupported — 오류 코드는 판정 단계의 것이고 진단은 모두 `InvalidImportOption`이다.
      - 원본 수가 임포터 roles 수와 다르거나 역할 집합이 다르면 `InvalidImportOption`이다(임포터 버전이 역할을 바꿨다 — 다시 임포트한다).
      - 기록에 있는데 이번에 내지 않은 출력의 `OrphanedOutput` 경고(asset = 그 출력, related = 주 출력)는 이번 경고와 같이 성공할 때만 넘긴다.
      - 결과·명령 출력 줄은 이번에 낸 출력 모두다(기존 출력 갱신 포함 — 새로 만든 것만이 아니다). 명령은 `asset-import`와 인자 해석·출력을 같이 쓴다(`--importer=`·위치 인자 원본은 사용법 오류, `--source=`는 한 번, `--source=`와 `--source.<역할>=`은 섞지 않는다).
      - 시험 임포터 계약: 임포트가 정하는 필드(값·`hard` 참조 — 없으면 null)는 매번 모두 쓴다(제자리 갱신이 옛 값을 남기지 않는다), 추가 TestAsset 출력도 값을 쓴다. 재임포트 여부는 `lastReimport`로 확인한다.
      - 리뷰 반영(필수 둘·권장 넷):
        - 성공한 새 임포트·재임포트는 이번에 연 패키지(출력과 그 의존)를 `CloseOpened(before)`로 닫는다. 열린 채 두면 같은 서비스의 다음 재임포트가 "열림 → Busy"에 걸린다. 저장한 깨끗한 패키지이고 원래 열려 있던 패키지는 그것들을 참조할 수 없으므로 잃는 것이 없다. 결과 객체를 보려면 다시 연다(`Info`·`Packages().Open`). 닫기 실패(할당·Discard)는 성공을 바꾸지 않는다 — 저장은 끝났고 남은 패키지는 열린 채다(호출자가 Close한다, 할당 실패 시험이 "삼킨 실패면 GC 표이거나 열린 패키지가 남는다"로 단언한다). 버린 대안: 열린 채 두고 계약에 적기(에디터식 — 임포트 직후 재임포트가 늘 Busy라 쓸 수 없다, 메모리 되돌리기는 에디터 트랜잭션 단계).
        - 판정은 색인 요약으로 하고 열기는 파일 기록으로 하므로, 파일 기록의 owner·임포터 id·버전·출력 id 목록이 색인 요약과 다르면(색인 뒤 밖에서 바뀜) `ExternalModification` / StaleHandle로 거부한다(Open의 "다른 id"와 같은 진단).
        - 기록 출력 타입(주 출력 포함)이 그 패키지의 대표 타입(색인)과 다르면 `TypeMismatch` / TypeMismatch(asset = 그 출력)로 거부한다 — 손 편집 기록에서 임포터가 `Output(key, T)`로 T 아닌 객체를 받지 않게.
        - 옵션 Record 시험은 사례마다 기대 오류를 짝지었다. 할당 실패 시험은 진단 None을 단언하고 먼저 저장된 기존 출력을 반복마다 처음 내용으로 되돌린다.
    - A2f2b 설계(2026-11-22, 착수 — 권장안으로 확정, 위 "임포트 상태"를 고친다):
      - 출력 자리: `asset-info <path|AssetId> --import [--json]` — 속성 대신 임포트 상태만 쓴다. 위 초안의 "Edit 형식의 끝에 덧붙인다"는 버렸다: Edit 줄은 `asset-set`에 그대로 넣는 입력 형식이라(왕복 계약 — 13절, 시험 `asset-info lines round trip through asset-set`) 속성이 아닌 줄이 섞이면 왕복이 깨지고, `import`라는 이름의 속성과 줄·JSON 키가 겹칠 수 있다. `--import`는 `--all`과 함께 쓰지 않는다(사용법 오류).
      - 서비스 `AssetService::ImportStatus(target, out, warnings, failure)`: 대상(`<path|AssetId>` — 별칭은 Info와 같이 풀고 경고 `AliasResolved`를 남긴다, 저장하지 않은 새 패키지는 색인에 없으므로 NotFound)을 색인에서 찾고 **패키지를 열지 않고** 파일의 ImportRecord 칸만 부분 읽기(`ReadPackageIndex`)로 읽는다 — 상태는 디스크 기준이고(열린 패키지의 저장하지 않은 기록이 아니다), 열린 패키지를 남기지 않는다(같은 서비스의 재임포트가 Busy에 걸리지 않게 — A2f2a 리뷰). 잠금을 잡지 않는다(읽기). 파일 헤더 id가 색인과 다르면 `ExternalModification` / StaleHandle.
      - 결과 `AssetImportStatus`: 기록 없음이면 InvalidArgument(진단 None — 재임포트와 같다). 참조 기록(출력 패키지)이면 `owner`만 채운다(`asset-info --import`는 `import.owner=<owner 논리 경로|AssetId> <owner id>` 한 줄 — owner가 색인에 없으면 경로 자리에 id). 소유 기록이면 임포터(이름·기록 버전·상태)와 원본마다(역할·root·relative·상태)를 채운다. 기록을 읽지 못하면(다른 스키마·더 새 버전) `NewerVersion` / Unsupported, 형식 위반 `CorruptPackage`(칸 ImportRecord).
      - 임포터 상태: 기록 id의 임포터가 없으면 `UnsupportedSourceFormat`, 등록 버전 > 기록 버전이면 `ImporterOutdated`, 기록 버전이 더 새면 `NewerVersion`(재임포트가 거부한다), 그 밖 current.
      - 원본 상태: 기록 위치를 지금 루트로 풀어(모르는 루트 `MissingSource` — 재임포트와 같다, 위치 규칙 위반 `InvalidSource`) 파일을 연다. 없으면 `MissingSource`, 크기가 기록 byte 수와 다르면 읽지 않고 `SourceChanged`, 같으면 고정 크기 버퍼(`ImportStatusReadBytes` 64 KiB)로 흘려 읽으며 SHA-256을 계산해 다르면 `SourceChanged`, 같으면 current. 읽기 IO 오류는 `InvalidSource`(그 오류 — 원본마다 따로, 전체를 실패시키지 않는다: 상태 보고가 목적이라 다른 원본의 상태도 보여 준다). 원본을 통째로 메모리에 올리지 않는다(임포트 상한 MaxImportSourceBytes까지 큰 원본이 있다).
      - 줄 형식(`--import`): `import.importer=<이름> <기록 버전> <상태>`, 원본마다 `import.source.<역할>=<root>:<relative> <상태>`(빈 root = 절대 경로 — `:` 뒤가 절대 경로), 상태는 `current` 또는 진단 이름(`SourceChanged`·`MissingSource`·`InvalidSource`·`ImporterOutdated`·`UnsupportedSourceFormat`·`NewerVersion`). JSON(`--import --json`): 소유 기록이면 `{"importer": {"name", "version", "state"}, "sources": [{"role", "root", "relative", "state"}]}`, 참조 기록이면 `{"owner": "<AssetId>", "ownerPath": "<논리 경로>" | null}`. 종료 코드는 상태와 무관하게 0이다(조회 명령 — 낡음 판정은 출력으로, 자동 재임포트 없음 16.1절).
      - UE 대응: `UAssetImportData`의 원본 파일 정보(`FAssetImportInfo` — 상대 경로·타임스탬프·MD5)와 에디터의 원본 변경 감지. UE는 시각을 먼저 보고 해시로 확인하지만 여기서는 시각을 기록하지 않으므로(위) 크기 → SHA-256이다.
    - A2f2b 상태: **구현** — 대조표 14.1.51절, coverage 14.1.52절. 구현에서 정한 것:
      - 원본 대조는 공개 함수 `CheckImportSource(files, path, bytes, digest, buffer, error)`(AssetSourceRoots.h — 할당하지 않는다, 버퍼는 호출자가 `ImportStatusReadBytes`로 한 번 잡는다)다. 크기를 잰 뒤 줄어든 파일(EndOfFile)은 `ReadWhole`과 같이 IO 오류(InvalidSource)다. 빈 버퍼는 InvalidSource / InvalidArgument다.
      - 참조 기록의 owner 경로는 `AssetIndex::FindById`(할당하지 않는다)로 찾고 없음·같은 id 둘이면 빈 경로다 — 원본 루트 표는 읽지 않는다(표가 깨져도 참조 기록 상태는 나온다).
      - 별칭이 가리키는 id가 색인에 없으면(대상을 밖에서 지움) NotFound(asset = 그 id)다.
      - 시험 파일 시스템(`MemoryFileSystem`)에 크기 조회 실패 `MemoryFault::Size`를 더했다(원본 크기 조회 실패를 실제로 만든다 — 미도달 사유로 남기지 않는다).
      - JSON은 `JsonWriter::Finish`가 줄 끝을 붙인다(명령이 더 붙이지 않는다). 읽기 오류의 진단은 `ReadPackageIndex`의 진단(코드·칸)을 그대로 쓰고 asset만 대상 id로 채운다.
      - 리뷰 반영(필수 하나·권장 둘): 잘린 파일(CorruptPackage / InvalidArgument)·원본 루트 표 손상(InvalidSource / InvalidArgument, detail = (줄 << 32) | 열)을 정확한 값으로 단언한다. 원본 상태에 원인 오류가 있으면(InvalidSource·모르는 루트의 NotFound) 줄 끝에 ` (<오류 이름>)`, JSON에 `"error": "<오류 이름>" | null`을 쓴다(결과 구조의 정보를 출력에서 잃지 않게 — Busy·AccessDenied를 가린다). 할당 실패 시험은 결과의 모든 필드(임포터 id·버전, 원본 root·relative·error 포함)를 기준과 비교한다.
      - 버린 권장: `<path|AssetId>` 대상 해석을 private 함수 하나로 모으기. 세 곳의 계약이 다르다 — `Delete`는 별칭을 풀지 않고(지금 이름으로 고른다) 저장하지 않은 새 패키지를 포함하며 경로로 고른 항목도 같은 id 둘이면 거부한다, `OpenTarget`은 `EditorPackageSet::Open`을 거쳐 열기 진단을 쓰고 저장하지 않은 새 패키지를 포함한다, `ImportStatus`는 색인 항목만(파일을 고르므로 경로로 고른 같은 id 둘도 읽는다) 별칭과 함께 본다. 하나로 모으면 선택 인자 셋이 생겨 오히려 갈라짐을 숨긴다 — 경로 형식·AssetId 형식의 진단(InvalidPath)은 세 곳이 같다.
    - 버린 대안: 임포터가 패키지를 직접 만들고 저장(원자성·점유·기록 규칙이 임포터마다 흩어진다 — 문맥이 출력을 준다), 옵션을 객체(UE ImportUI `UObject`)로(공개 전 객체가 패키지에 들지 않는 일회 값이라 struct 값 대입으로 충분), 옵션 접두사 `--options.<필드>=`(13절 문법이 바뀌고 UE 명령행 관례와 다르다 — 등록 때 이름 충돌을 막는다), 확장자 경합 우선순위(UE `ImportPriority` — 결과가 등록 순서에 달린다), 타입이 바뀐 출력을 같은 id 새 객체로 바꾸기(위), 재임포트가 참조 기록 출력에서 owner를 따라가기(무엇이 바뀌는지 숨긴다 — 명시 거부), 명령 성공 경로 시험을 A5로 미루기(지금 닿는 코드를 미구현 사유로 남긴다 — 교차 검증 필수 4), 시험 전용 Commandlet 실행 파일(타깃이 하나 더 생긴다 — 프로세스 안 호출이 작다).

## 8. 동시성

- 쓰기 잠금: `Intermediate/AssetDatabase.lock`을 `FileLock`(Windows `LockFileEx` + 삭제 공유 없는 열기, POSIX `flock` + 잠근 뒤 dev/ino 재확인)으로 잡은 프로세스만 에셋·별칭 표·인덱스 캐시·쿠킹 출력을 바꾼다. 잠금을 잡는 명령은 13절 목록이며 서비스 API도 같은 규칙이다. 쿠킹도 이 잠금을 쥐므로(에디터 패키지가 쿠킹 중 바뀌지 않게) 같은 프로젝트의 쿠킹은 프로파일과 무관하게 직렬화되고 쿠킹 출력에 따로 잠금을 두지 않는다. 못 잡으면 `Busy`(`--wait=<초>`).
- 에셋 너겟(`NuggetFileSystemBackend`)은 키 주소 저장소라 잠금 없이 동시 기록한다(9절 — 같은 키의 동시 게시는 no-replace 이동이 하나만 남기고 나머지는 `Exists`·`Conflict`다). Rockpile backend는 서버가 저장소 잠금을 쥔다(Rockpile 6.8절).

## 9. 에셋 너겟

- 위치: `<AssetNuggets>` = `Config/AssetNuggets.json`의 `localPath`(기본 `<Project>/Intermediate/AssetNuggets/`). 프로젝트 사이 공유 경로도 쓸 수 있다(내용 주소라 안전하다).
- 에셋 너겟은 "캐시 키 → 가공 결과" 저장소다. 같은 에셋이라도 설정·대상 플랫폼이 다르면 결과가 다르므로(예: Windows용·Android용 `T_Wall`은 다른 키) **결과에 영향을 주는 입력은 빠짐없이 키에 들어가야 한다.** 빠진 입력은 오래된 결과를 돌려주는 버그가 되고, 불필요한 입력(경로·시각·locale)은 적중률만 떨어뜨린다(계획 §9.1).
- 키: 처리기 `BuildKey(KeyWriter&)`가 고정 순서·타입 태그로 기록한 바이트의 SHA-256. 모든 처리기에 공통으로 들어가는 것: `"RocksAssetNugget" 형식 버전`, **에셋 너겟 세대 Guid 둘**(엔진 세대 상수와 프로젝트 세대 — `Config/AssetNuggets.json`의 `generation`, 없으면 null Guid. 둘 다 키에 들어가므로 엔진이 세대를 올리면 프로젝트 재정의와 무관하게 모든 키가 바뀐다. 오염 회수의 유일한 보장 수단 — Rockpile 11.4절), 처리기 Guid·처리기 버전 Guid, 결과를 만드는 **인코더·외부 라이브러리의 이름·버전·설정**(블록 압축 인코더 등 — 비압축이면 없음. 처리기가 결과의 일부로 거는 **결과 압축**은 이름·revision·수준·사전이 키에 들어가고, 에셋 너겟 계층이 저장할 때 거는 **저장 압축**(RCBK)은 키에 넣지 않는다 — Rockpile 5.3절), **결정적 수학 버전**(A0-C), **툴체인 지문**(컴파일러 버전·컴파일러/링커 바이너리 해시·대상 triple·부동소수점 모델 — 빌드가 상수로 주입, [ReflectionFoundations](ReflectionFoundations.ko.md) 8.1절), 에셋 TypeId, 입력 bulk SHA-256, 결과에 영향을 주는 설정 전체(기본값 포함, float는 bit 패턴, NaN 단일화, `CompressionCacheId` 포함), 대상 프로파일 id와 출력 설정 전체, 의존 입력 (역할, AssetId, 그 의존 에셋의 키 — 의존 에셋을 처리하는 그 처리기의 구성원 key) 정렬 목록 — 의존의 설정·전이 Build 의존 변화까지 키에 들어간다(Merkle). 경로·시각·locale 제외. 엔진 버전도 넣지 않는다(출력 의미가 바뀌면 처리기 버전 Guid를 바꾼다 — [ReflectionFoundations](ReflectionFoundations.ko.md) 8.2절).
- Build 의존 순환(지원 SCC, 6.2절)에서는 서로의 key가 서로를 포함해 값이 정해지지 않으므로 SCC 단위로 계산한다: SCC key = SHA-256(태그, 구성원 (AssetId, 지역 입력 키 — 위 항목 중 의존을 뺀 것의 `NuggetKeyWriter` 키) 정렬 목록, SCC 밖 의존 튜플 (구성원 AssetId, 역할, 의존 AssetId, 그 의존의 구성원 key) 정렬 목록), 구성원 key = SHA-256(태그, SCC key, AssetId) — 정렬·중복 규칙과 Record 주소는 9.1절 결정 3. 순환이 없어도 이 식 하나만 쓴다(구성원 하나인 SCC — 값 체계를 하나로 둔다).
- 텍스처 키의 구성(위 공통 항목의 텍스처 대응):

  | 입력 | 키에 넣는 값 |
  |---|---|
  | 원본 정체성 | TextureSource의 bulk SHA-256·형식·크기·slice·mip·layer 수(묶음 원본이면 각 원본 에셋의 build key) |
  | 텍스처 종류 | TypeId(Texture2D·Cube·2DArray·CubeArray·Volume) |
  | 색 공간 | `SRGB`·`bUseLegacyGamma`·`SourceColorSettings` 전체 |
  | mip 생성 | `MipGenSettings`·`bPreserveBorder`·`bNormalizeNormals`·`PowerOfTwoMode`·`PaddingColor`·`bPadWithBorderColor`·`ResizeDuringBuildX/Y`·`LODBias`·`MaxTextureSize`·`bFlipGreenChannel`과 프로파일 LOD 그룹 행 |
  | 압축 설정 | `CompressionSettings`·`CompressionNoAlpha`·`CompressionForceAlpha`·`CompressionNone`·`CompressionCacheId`(계약 설정도 값 그대로 — 기본값이 아니면 이미 거부되지만 키는 스키마 전체를 쓴다) |
  | 주소·샘플러 | `AddressX/Y/Z`·`AddressMode`·Border 색(Lanczos 경계 처리가 결과를 바꾼다), 그룹·텍스처에서 정한 Filter·MaxAniso(쿠킹 데이터의 샘플러) |
  | 대상 플랫폼 | 프로파일 id·형식 지원 표·bulk 정렬·codec |
  | 대상 픽셀 형식 | 12.4.3절 규칙으로 확정한 쿠킹 형식·sRGB 표시·swizzle(설정·프로파일에서 유도되지만 유도 규칙의 버그를 막기 위해 결과 형식도 넣는다) |
  | 처리기 버전 | 텍스처 처리기 Guid·버전 Guid |
  | 인코더 버전 | 블록 압축 인코더 라이브러리 이름·버전·품질 설정(비압축 단계에는 없음) |
- 값: 키 하나의 결과 묶음은 **Record**(Rockpile 5.2절 `RockpileRecord` TLV — echo·값 표·소비 입력·meta)와 값 byte다. `NuggetFileSystemBackend`는 이것을 `.rnug` v2 파일 하나(`<AssetNuggets>/<key[0..2]>/<key>.rnug`)로 둔다(9.1절 A3a). 텍스처는 mip·slice가 값 하나씩이다(ValueId).
- 읽기: 모든 읽기를 검증한다 — 파일 머리의 Record SHA-256·Record 형식·echo(namespace·key)·파일 크기·조각 해시 표(크기·ChunkTableHash)·값마다 저장 byte SHA-256(ContentHash)과 해제 뒤 원본 SHA-256(RawHash). 검증 실패 → `CorruptEntry` 진단·삭제·미적중(경합으로 정상 항목을 지워도 재생성되므로 무해 — Rockpile R-0 "틀린 적중 0건"). 보장 범위는 읽은 값 전체다(값 단위 선택 — 읽지 않은 값은 검증하지 않는다). byte 범위 부분 읽기(Rockpile 8.6절 — 조각·블록 CRC 수준)는 계약이다.
- 쓰기: `<AssetNuggets>/tmp/<guid>` 기록·Flush → `Move(no-replace)`. 대상이 있으면 기존 Record의 논리 digest와 비교해 같으면 `Exists`, 다르면 기존 유지·`Conflict`(비결정성·키 누락 진단), 기존이 읽을 수 없거나 손상이면 교체(`Stored`) — Rockpile 6.9절 판정. Put 실패는 치명적이지 않다(`required`일 때만 오류). 전원 차단 뒤의 손상 항목은 읽기 검증이 거른다.
- 정리: 적중 시 `SetModifiedTime`, `nugget-gc --max-bytes --unused-days`(A6 명령, 서비스는 A3b), `tmp` 24시간 경과 삭제.
- 인터페이스: 레코드 모델의 일괄·비동기 계약(Rockpile 12절) — `NuggetGetRequest`·`NuggetPutRequest`(호출자 소유, Core Task 관례), 계층 `NuggetCache` = Memory → 선택된 backend 하나(`NuggetFileSystemBackend` 또는 `NuggetRockpileBackend` — 계약), 세션 고정·소비 입력 대조, `NuggetPolicy`(UE `ECachePolicy`와 같은 비트), SCC Record(Rockpile 12.3절). `--verify-nuggets`(적중 결과 = 재생성 결과 대조, A4).
- 원본 bulk는 에디터 패키지가 권위 저장소(계획 §9.4)이므로 에셋 너겟을 지워도 손실 없음. Virtual Asset(원본 bulk 분리 저장소)은 후속(17절).

### 9.1 A3 세부(2026-11-22 설계 — Rockpile 레코드 계약, 사용자 결정, 교차 검증 2회 반영)

A3는 Rockpile 설계([Rockpile](Rockpile.ko.md) 12·17절)가 요구한 레코드 모델로 처음부터 만든다(사용자 결정 2026-11-22 — 원래 9절의 단건·동기 `NuggetBackend { Get, Put, Remove, Touch }`와 `.rnug` v1은 쓰지 않는다: Rockpile 착수 때 API·형식을 다시 바꾸게 된다). 구현은 `NuggetFileSystemBackend`까지다. `NuggetRockpileBackend`는 계약(설정 `backend: "rockpile"`이면 `Unsupported`)이고 Rockpile 서버·`RockpileClient`·PlatformBase `Process`·`LocalChannel`(Rockpile R0–R2)은 별도 작업이다. ShaderSystem 5.2절 N1–N3(에셋이 아닌 처리기, 여러 부분 값, Rockpile 개정)도 여기서 반영한다. 애매한 곳은 UE 5.8 DerivedDataCache를 따른다(`Developer/DerivedDataCache` — `ICache::Put`·`Get`·`GetValue`·`GetChunks`(`Public/DerivedDataCache.h:56–126`), `ECachePolicy` 비트(`Public/DerivedDataCachePolicy.h:50–78`), 파일 저장소 경로 `Buckets/<bucket>/<b0>/<b1>/<hash>.udd`(`Private/FileSystemCacheStore.cpp:91–96`)).

결정(권장안으로 확정, 교차 검증 2회 — 1회 필수 9·권장 9, 2회 필수 1·권장 3 반영):

1. **모듈**: `Engine/Source/Developer/AssetNugget`(Developer — 에디터·도구만 링크, 게임은 쿠킹 결과만 읽는다). 의존 `Core`·`Serialization`(Record TLV)·`Task`(WorkerScheduler)·`Compression`(RCBK 저장 압축)·`DeterministicMath`(`deterministic::Version`)·`MetaSchema`·`ObjectRuntime`(리플렉션 값 기록 — 결정 2). AssetDatabase에 의존하지 않는다(위치는 호출자가 루트 경로로 준다 — 셰이더 컴파일처럼 프로젝트 없는 엔진 루트 사용도 같은 API, ShaderSystem 10절). 시험은 EngineTests(`Engine/Source/Developer/AssetNugget/Tests`).
2. **키(A3a)**: `NuggetKeyWriter` — 고정 순서로 타입 태그(u8)와 값을 SHA-256에 흘려 넣는다(byte 열을 모으지 않는다).
   - 태그: None(값 없음 — Optional unset 등, 다음 필드와 경계가 섞이지 않게)·Bool·U32·U64·I64·F32·F64(bit 패턴 — NaN은 payload·부호와 무관하게 quiet NaN 하나로 단일화, -0과 +0은 다르다)·Guid(16 byte, 정규 인코딩)·Digest(32 byte)·String(u64 길이 + UTF-8, 유효하지 않은 UTF-8은 InvalidArgument)·Bytes(u64 길이 + byte)·List(u64 원소 수 — 원소는 이어지는 기록)·Map(u64 항목 수 — 항목은 키·값 기록 쌍, 키의 정규 인코딩 byte 사전순으로 정렬해 쓴다)·Set(u64 원소 수 — 정규 인코딩 byte 사전순). Map·Set은 그 컨테이너의 항목 인코딩만 임시로 모아 정렬한 뒤 흘려 넣는다(원소 수는 리플렉션 상한 `MaxArrayElements` 안 — 중첩 컨테이너는 안쪽부터 같은 규칙)·Field(u64 길이 + 필드 이름 — 리플렉션 기록이 필드마다 앞에 둔다)·Struct(struct 시작)·End(struct 끝 — 필드 수를 미리 모르는 struct의 경계, 중첩 struct의 필드가 바깥 필드와 섞이지 않게)·Raw(이미 인코딩한 항목 byte를 그대로 — Map·Set 정렬 뒤 흘려 넣을 때만, 태그를 따로 쓰지 않는다). 길이 접두가 모든 가변 길이에 있으므로 이웃 기록과 경계가 모호하지 않다.
   - 리플렉션 값 기록 `WriteReflected(lease, value)`: struct 필드를 선언 순서로 Field(이름) + 태그 + 값으로 쓴다(재귀 — struct·Array·Map·Set·Optional(unset이면 None)·enum(underlying 정수 — 열거자 이름만 바꾸는 것은 결과에 영향이 없다)·Transform 등 리플렉션 값 종류 전부. 쓸 수 없는 종류(객체 참조·Opaque)는 Unsupported). 순회는 ReflectionText에 공용 visitor(`VisitValues(target, visitor, options)` — ExportJson과 같은 스키마·경로 연산 순회, 필드 이름·FieldValue·컨테이너 시작·끝을 차례로 넘긴다)를 더해 쓴다 — 순회를 AssetNugget에 따로 만들지 않는다(기반 보강, ReflectionExtensions 10절에 함께 적는다). 이름·타입이 들어가므로 같은 타입 필드를 맞바꾸거나 이름만 바꿔도 키가 바뀐다. 처리기는 설정을 손으로 쓰지 않고 이것으로 쓴다(R-0의 가장 흔한 실패 — 필드 빠뜨림 — 를 막는다). 처리기마다 "설정 필드 하나씩 바꾸면 키가 바뀐다" 변이 시험을 의무로 둔다.
   - 키 진단: 선택 기록 sink(`NuggetKeyTrace` — 태그·값을 사람이 읽는 줄로)를 writer에 걸 수 있다(미적중 원인 추적·golden 시험 — UE DDC 키 디버깅 대응). 걸지 않으면 비용이 없다 `WriteRaw`(이미 인코딩한 byte)는 `raw <크기>` 뒤에 내용을 32 byte씩 16진 줄로 남긴다(손으로 정렬해 흘린 Map·Set의 키 불일치를 trace로 찾게 — 2026-11-23 rocks-c1 제안).
   - 공통 머리 `WriteNuggetKeyHeader(writer, {processor, processorVersion}, projectGeneration)`는 `"RocksAssetNugget"`·키 형식 버전(1)·엔진 세대 Guid·프로젝트 세대 Guid(없으면 null)·처리기 Guid·처리기 버전 Guid·`deterministic::Version`·`build::ToolchainFingerprint()` 순서로 쓴다(9절 공통 항목. 인코더 이름·버전·설정은 처리기가 `WriteNuggetEncoder(writer, name, version, settings)` 목록으로 이어 쓴다). 키 = `Finish()`의 SHA-256(`NuggetKey`). 에셋이 아닌 처리기(셰이더 job — N1)는 이 키를 Record 키로 그대로 쓴다.
3. **SCC 키와 Record 주소(A3a)**: 에셋 처리기는 SCC 식(9절)을 쓴다 — `NuggetSccKey(members, external)`.
   - 입력: 구성원 (AssetId, 지역 입력 키), SCC 밖 의존 튜플 (구성원 AssetId, 역할 문자열, 의존 AssetId, 그 의존의 구성원 key). 함수가 정렬한다(입력 순서와 무관한 결과): 비교는 각 원소의 정규 인코딩 byte(AssetId 16 byte, 역할은 u64 길이 + UTF-8)의 사전순이고 튜플은 필드 순서대로 비교한다. 빈 구성원 목록·구성원 AssetId 중복·null AssetId는 InvalidArgument, 튜플의 구성원 AssetId가 구성원이 아니거나 의존 AssetId가 구성원이면(SCC 안 간선은 외부 의존이 아니다) InvalidArgument. 외부 튜플의 완전 중복은 하나로 합친다(같은 의존을 같은 역할로 두 번 참조해도 결과는 같다 — 간선 다중도는 처리 결과에 영향이 없다).
   - SCC key = SHA-256(태그, 정렬된 구성원 목록, 정렬된 외부 튜플 목록). 구성원 key = SHA-256(태그, SCC key, AssetId).
   - **Record는 늘 SCC key로 저장한다**(구성원 하나인 SCC 포함 — 값 체계 하나, Rockpile 12.3절). 구성원 key는 Merkle 입력(다른 에셋이 이 에셋을 의존할 때 튜플의 key)과 구성원 조회 헬퍼(결정 15)의 입력으로만 쓴다. `inputs`에는 Record 키(SCC key)를 적는다. bucket은 구성원 하나거나 처리기가 하나면 그 처리기 bucket, 처리기가 섞이면 `scc`.
4. **세대 Guid**: 엔진 상수 `EngineNuggetGeneration`과 프로젝트 세대(`Config/AssetNuggets.json`의 `generation`, 없으면 null Guid)가 둘 다 키에 들어간다(결정 2 공통 머리). 이 파일은 키에 들어가지 않는다는 규칙(Rockpile 12.4절)의 예외는 **프로젝트 세대 값 하나**다 — 파일 해시가 아니라 이 값만 들어가고 `backend`·`required`·`localPath`는 결과에 영향이 없어 넣지 않는다(CoreDataFoundations 132행·Rockpile 12.4절도 이렇게 고친다).
5. **Record(A3a)**: Rockpile 5.2절 schema 그대로(`RockpileRecord` — 필드 순서 고정, 순서·중복·한도 위반은 `NonCanonical` → 읽기에서 손상). schema Guid·필드 Guid는 A3a가 `AssetNugget`의 `NuggetRecordSchema.h`에 정한다 — 필드 1 namespace(String), 2 bucket(String), 3 key(Bytes 32), 4 values(Bytes — 행 표), 5 inputs(RecordList — 원소 Record { 1 bucket, 2 key, 3 RecordDigest }), 6 meta(Bytes), 순서 = 필드 번호 순, 모르는 필드는 NonCanonical. Rockpile 명세 파일(`RockpileProtocol.json`, Rockpile 8.3절 — R0)이 생기면 같은 값을 그 파일로 옮기고 AssetNugget은 생성 상수를 쓴다(byte 불변 — golden 시험이 지킨다). echo = (namespace `rocks.nuggets`, bucket, key). bucket 규칙 `[a-z][a-z0-9_]{0,63}`(고정 소문자 ASCII 상수 `NuggetBucket` — `texture`·`shader`·`scc`). 값 표는 160 byte 행(ValueId 오름차순·중복 금지, reserved 0). Rcbk 값의 indexLength는 RCBK 헤더 + 블록 표 길이(48 + 8n)이고 IndexHash는 0이 아니다(Rockpile 5.2절). 소비 입력 `inputs` = { bucket, key, RecordDigest } 정렬 목록(정규 인코딩 byte 사전순, 중복 금지). meta ≤ 64 KiB(결정적 정보만). Record ≤ 1 MiB(행 약 6,000). `RecordDigest` = Rockpile 5.3절 정규 인코딩의 SHA-256(저장 표현 — encoding·storedSize·ContentHash·IndexHash·ChunkTableHash — 은 넣지 않는다). encoding은 `Identity`·`Rcbk`를 구현하고 `Codec`(사용자 등록 형식)은 계약(읽기에서 그 코덱이 없으면 미적중 — A3에서는 늘 미적중). 값 하나 상한 16 GiB(Rockpile과 같다). 조각 해시 표(storedSize ≥ 1 MiB 값의 1 MiB 조각 SHA-256)를 만들고 ChunkTableHash를 채운다(같은 저장 압축이면 Rockpile backend와 Record byte가 같다 — `.rnug` v2 안에 표를 둔다).
6. **`.rnug` v2 파일(A3a)**: `{ magic "RNUG", u32 버전 2, u64 Record byte 수, Record SHA-256(32 byte), Record, 값 저장 byte(ValueId 순, 이어서), 조각 해시 표(해당 값만, ValueId 순) }`, little-endian. 읽기는 머리의 Record SHA-256을 Record byte와 대조한 뒤 Record를 해석한다(meta·inputs·echo 밖 칸의 bit 변조도 손상 — Rockpile이 Record 본문을 blob 해시로 검증하는 것과 같은 보장, 6.7절). 파일 크기는 머리·Record·값·표 합과 정확히 같아야 한다(남는 byte는 손상). 조각 해시 표는 값을 읽지 않을 때도 크기(⌈storedSize / 1 MiB⌉ × 32)와 ChunkTableHash를 대조한다(손상된 표가 나중에 Rockpile로 올라가지 않게). 읽기 함수는 메모리 byte와 범위 reader(파일) 둘을 받는다.
7. **`NuggetFileSystemBackend`(A3b)**: 루트(`localPath` — 기본 `<Project>/Intermediate/AssetNuggets/`)를 연다. 연산은 동기 내부 API(Get·Put·Touch·ReportCorrupt·Remove — 요청 객체가 worker에서 부른다).
   - Get은 값 선택(ValueId 목록 — 비면 전체)·`SkipData`·`SkipMeta`를 받고 고른 값만 읽어 검증한다(UE `GetValue`·`GetChunks`의 값 단위 대응). 검증 실패는 `Corrupt` 결과 + 파일 삭제(삭제 실패는 진단만). echo의 key가 다르면 손상, bucket만 다르면(같은 키를 다른 bucket으로 청한 호출자 결함) 삭제하지 않고 `Miss` + 진단이다.
   - Put은 9절 쓰기 판정. `Exists`면 Touch한다(GC가 방금 다시 만든 항목을 지우지 않게 — UE 파일 저장소도 기존 항목을 갱신한다). 한도 위반 Put(Record 1 MiB·값 16 GiB·잘못된 bucket·정렬 위반 inputs)은 `Dropped` + 진단(InvalidArgument·CapacityExceeded)이다.
   - 저장 압축: 값의 원본이 임계(`NuggetStoreCompressBytes` 64 KiB) 이상이면 RCBK(Zstd)로 압축하고 더 작을 때만 `Rcbk`로 쓴다 — 게시 전에 왕복 해제해 RawHash를 확인한다(Rockpile 5.4절 writer 계약).
   - Touch(`SetModifiedTime`)는 최선 노력이다. tmp 24시간 경과 임시 파일(`*.tmp`)은 backend를 열 때와 GC에서 지운다 — 잠금 파일(`*.lock`)은 지우지 않는다(PlatformBase `FileLock` 계약: 지우면 POSIX에서 Release와 unlink 사이에 옛 inode를 잡은 쪽과 새 파일을 잡은 쪽이 함께 잠금을 쥔다 — 그래서 잠금은 개수가 고정된 shard다, 결정 8). 읽기 전용 루트(공유 경로)면 Get은 되고 Put은 `Dropped` + 진단이다.
8. **동시성(A3b)**: 두 프로세스가 같은 키를 동시에 게시하면 no-replace 이동 하나만 이기고 다른 쪽은 기존을 읽어 `Exists`·`Conflict`를 판정한다. 읽는 쪽은 이동으로만 나타나는 완전한 파일만 본다.
   - 손상 항목 교체는 shard 잠금(`<root>/tmp/<k0k1>.lock` — 키 첫 byte로 256개, 개수가 고정이라 쌓이지 않고 지우지 않는다, PlatformBase `FileLock` — 드문 경로라 shard 경합 비용이 없다)을 쥐고 기존을 다시 읽어 여전히 손상·읽을 수 없을 때만 `FileSystem::Replace`한다. 잠금 안에서 정상 항목을 만나면 그것을 기존으로 판정한다(`Exists`·`Conflict` — 다른 프로세스의 교체를 덮어쓰지 않는다). 잠금을 못 잡으면(시간 상한) `Dropped`.
   - Windows: 읽기는 삭제 공유(`FILE_SHARE_DELETE`)로 열어 GC·손상 삭제가 열린 읽기 때문에 실패하지 않게 한다(PlatformBase Win64 열기 규칙). delete-pending 이름으로의 no-replace 이동은 AlreadyExists가 아니라 AccessDenied가 날 수 있다 — 그 Put은 `Dropped`(일시적, 진단). 열린 대상에 대한 Replace 실패·GC 삭제 실패는 그 항목을 건너뛰고 진단한다.
   - 시험은 결정적 gate(파일 존재 대기 아닌 명시 이벤트)로 두 자식 프로세스의 경합 순서를 고정한다(같은 키 동시 Put, GC 대 Put, 교체 대 교체).
9. **GC(A3b)**: `Collect({maxBytes, unusedDays}, now)` — 항목 파일을 훑어(디렉터리 `<k0k1>`·이름 `<key>.rnug`만, 다른 이름은 건드리지 않는다) 마지막 사용 시각(수정 시각)이 `unusedDays`보다 오래된 것을 지우고, 남은 합이 `maxBytes`를 넘으면 오래된 것부터(같은 시각이면 키 byte 순) 지운다. 결과는 지운 수·byte·남은 수·byte·건너뛴 수. 지우는 중 다른 프로세스가 같은 항목을 쓰거나 읽어도 무해하다(지운 뒤 Put은 새로 게시되고, 읽기는 미적중).
10. **`NuggetCache`(A3c)**: Memory 계층(프로세스 안) → backend 하나.
    - Memory 계층은 Record와 값 byte를 쥔다(byte 상한 `NuggetMemoryBytes` 기본 256 MiB, 값 하나가 `NuggetMemoryValueBytes` 64 MiB를 넘으면 그 값은 쥐지 않는다, 넘치면 오래 쓰지 않은 Record부터 버린다). `SkipData` 적중은 Memory에 넣지 않는다(값이 없다). 축출하지 않는 것은 DigestChanged 뒤 다시 만든 Record(결정 11)뿐이다.
    - 설정 `Config/AssetNuggets.json` = `{ "backend": "filesystem" | "rockpile" | "off" | "auto", "required": bool, "localPath": 문자열, "generation": Guid 문자열 }`(엄격 JSON, 모르는 키 거부, 없으면 기본값 — filesystem·false·`Intermediate/AssetNuggets`·null 세대). `auto`는 Rockpile이 생기기 전까지 filesystem이다(경고 없음 — R1a에서 Rockpile 접속 시도를 더한다). `rockpile`은 계약(`Unsupported` — 열기 실패). `off`는 모든 Get이 `Miss`·모든 Put이 `Dropped`(진단 없이). 결정은 `NuggetCache::Open` 때 한 번이다(R-2). 도중 backend 실패는 그 연산의 미적중·`Dropped` + 진단이고 `required`면 그 요청 결과가 오류다.
11. **세션 고정·소비 입력 대조(A3c)**: 캐시가 세션 고정 표 key → (RecordDigest, `inputs`)를 둔다(첫 적중·첫 게시 — 값 byte는 쥐지 않는다, 전체 쿠킹이 거의 모든 Record를 소비해도 표는 키당 수십 byte다). 고정이 있는 키의 Get이 다른 digest를 보면 `DigestChanged`(그 Record에서 받은 값을 버리고 다시 얻거나 다시 만든다). 적중한 Record의 `inputs` 각 항목을 고정 표와 대조해 어긋나면 그 적중을 `Miss`로 다룬다(고정이 없으면 그 digest로 고정). 의존 결과를 소비해 Record를 만드는 처리기는 Put 때 소비한 (bucket, key, digest)를 `inputs`로 준다.
    - FileSystem backend는 옛 판을 남기지 않으므로(Rockpile의 lease가 없다) DigestChanged 뒤 다시 만든 Record의 Put이 `Conflict`가 되어 디스크에 게시되지 않을 수 있다. 그때 세션이 다시 얻을 곳은 Memory뿐이므로 **DigestChanged 뒤 다시 만든 Record만** 값까지 Memory에 고정 보관한다(축출 제외 — 값 하나 64 MiB 제외 규칙의 예외). 고정 보관 합은 별도 상한 `NuggetPinnedBytes`(기본 256 MiB)를 둔다. 넘으면 고정을 포기하고 그 키를 세션 동안 "캐시 없음"(Get은 늘 `Miss` — 처리기가 다시 만든다, 통계·진단)으로 내린다. 고정 표의 digest는 다시 만든 Record의 것으로 바꾼다.
12. **일괄·비동기 요청(A3c)**: Rockpile 12.2절 형태 — `NuggetGetRequest`·`NuggetPutRequest`(호출자 소유·고정 주소·이동 금지, `Start(cache, items, scheduler)`, `RequestCancel`, `Handle()`, `Results()`는 handle 완료 뒤, 소멸자는 취소 요청 뒤 완료 대기 — `CompressionTask`와 같은 수명 규칙, 같은 scheduler worker에서 진행 중 요청을 소멸하면 Fatal). 입력은 Start 때 snapshot한다(값 선택 목록 복사, Put 값 버퍼는 소유권 이전). 항목마다 worker 작업 하나이고 요청 handle은 모든 항목 작업에 의존하는 완료 작업이다(결과는 항목 순서 그대로). 대기 Put byte 상한(`NuggetPendingPutBytes` 기본 1 GiB)을 넘는 Put 항목은 즉시 `Dropped`. 취소는 협력적이다(항목 시작 전·값 사이·IO 단위 사이에서 관측 — 진행 중인 파일 IO는 끝낸다).
    - `NuggetCache::Shutdown(drainMs)` 순서: 새 Start 거부(이후 Start는 모든 항목 `Cancelled`) → 진행 중 Get 취소 요청 → Put은 `drainMs`까지 완료를 기다림 → 남은 Put 취소 요청·진단 → **모든 요청이 실제로 끝날 때까지 기다림**(Rockpile 12.2절 "모든 요청의 완료를 보장"). 진행 중인 요청이 있는데 캐시를 소멸하거나(Shutdown 없이) scheduler worker에서 Shutdown을 부르면 Fatal(계약 위반 — 교착·끊긴 참조를 막는다).
13. **결과 상태**: `Hit`·`Miss`·`Partial`(`PartialRecord`일 때 일부 값만)·`Stored`·`Exists`·`Conflict`(기존 digest를 함께)·`DigestChanged`·`Corrupt`(그 항목은 지웠다)·`Cancelled`·`Unknown`(commit 여부를 모르는 취소된 Put — 원격 전용. FileSystem의 이동은 동기·원자적이라 commit 여부를 늘 알아 생기지 않는다)·`Dropped`. 결과마다 원인 진단(코드·오류)을 담는다. 캐시는 통계(적중·미적중·Conflict·Corrupt·Dropped 수)를 원자 카운터로 둔다(결정성 gate — Conflict 0건 — 의 근거).
14. **`NuggetPolicy`**: UE `ECachePolicy`와 같은 비트(`QueryLocal`·`QueryRemote`·`StoreLocal`·`StoreRemote`·`SkipData`·`SkipMeta`·`PartialRecord`·`KeepAlive`, `Default` = Query | Store). A3에는 원격이 없으므로 `QueryRemote`·`StoreRemote`는 원격 쪽 동작이 없다 — `QueryRemote`만 켠 Get은 `Miss`, `StoreRemote`는 `StoreLocal`을 함의한다(Rockpile 5.6절). `KeepAlive`는 Touch만 한다. 항목별 값이 요청 값보다 우선한다.
15. **SCC Record(A3c)**: SCC 하나를 Record 하나로 둔다(결정 3·Rockpile 12.3절) — key = SCC key, ValueId = (구성원 순번 << 16) | 부분 번호, meta에 구성원 AssetId 목록(16 byte씩, 최대 4,096 — 넘으면 캐시하지 않고 `Dropped` + 진단), 처리기가 섞이면 bucket `scc`. 구성원 key로 찾는 쪽을 위해 `NuggetCache`가 (SCC key, 구성원 AssetId) → 그 구성원 값만 고르는 Get 헬퍼를 둔다.
16. **명령**: `nugget-gc`·`--verify-nuggets`(`--verify-nuggets-source=local|remote`)는 A6·A4 명령이다(서비스와 시험은 A3). Rockpile이 생기면 `nugget-gc`는 `rockpile gc`로 위임한다(Rockpile 12.4절, 13절 문법).

하위 단계(각각 14절 완료 조건 넷 — 대조표·조합·coverage 사유·리뷰어 판정):

| 단계 | 범위 | 시험 |
|---|---|---|
| A3a | ReflectionText `VisitValues`(공용 순회), 키(`NuggetKeyWriter`·태그 전부·`WriteReflected`·`NuggetKeyTrace`·공통 머리·세대 둘·SCC 식), bucket, Record 쓰기·읽기·digest, `.rnug` v2 쓰기·읽기(메모리·범위 reader), 조각 해시 표 | 태그 경계(None·길이 접두·Map·Set 정렬)·NaN payload 변형·-0·유효하지 않은 UTF-8, 공통 머리 항목 하나씩 바꾸면 키가 바뀜, 리플렉션 필드 변이(값·맞바꿈·이름), SCC 입력 순서 섞기·구성원 중복·외부 튜플 중복·같은 접두 역할, Record·머리 byte 변조 위치마다 거부, 한도 경계, golden byte·digest |
| A3b | `NuggetFileSystemBackend`(Get·Put·Touch·ReportCorrupt·Remove, 검증·손상 삭제, Exists·Conflict·교체·shard 잠금, 저장 압축 왕복), GC, tmp 정리(임시 파일만 — 잠금 파일은 지우지 않음) | 적중·미적중, 손상(파일 변조 위치마다)·삭제, bucket만 다른 echo, 같은 키 다른 digest, 두 프로세스 동시 기록·GC 대 Put·교체 대 교체(gate), Windows 삭제 공유·delete-pending 이동·Replace 실패, 읽기 전용 루트, GC 시각·용량·순서, 할당·IO 실패 주입, 실제 디스크 |
| A3c | `NuggetCache`(Memory 계층·설정·backend 선택·세션 고정·소비 입력·통계), 요청 객체(일괄·비동기·취소·Dropped·Shutdown), SCC Record 헬퍼 | 계층 적중 순서, 설정 파일 오류, off·rockpile 계약, DigestChanged 뒤 재고정과 Memory 축출, 많은 Record를 소비한 세션의 메모리 상한(고정 표만 커짐), 64 MiB 넘는 재생성 Record·고정 상한 초과 → 캐시 없음, inputs 어긋남, SkipData·SkipMeta·PartialRecord 조합, 취소 지점마다, 대기 상한, Shutdown 중 Get·drain, 진행 중 요청이 있는 캐시 소멸(Fatal 자식 프로세스), 4,096 구성원 초과 SCC, `required` 오류 경로, 할당 실패 |

- A3a 상태: **구현** — 대조표 14.1.53절, coverage 14.1.54절. 구현에서 정한 것:
  - 모듈 `Engine/Source/Developer/AssetNugget`(`NuggetKey.h`·`NuggetReflected.h`·`NuggetRecord.h`·`NuggetFile.h`), 시험 EngineTests(`Tests/`). 공용 순회 ReflectionText `VisitValues`(`ReflectionText/TextVisit.h` — ReflectionExtensions 10.6절)는 ExportJson과 대상 뿌리 해석(`ReadTargetRoot`)·필드 도구 이름을 함께 쓴다(복제하지 않는다).
  - 키 정규 인코딩: 정수·길이는 little-endian, Guid는 high → low 각각 big-endian(표준 문자열 순서 — byte 사전순이 값 순서와 같다). 태그 번호는 1부터 고정(None 1 … End 17 — 바꾸면 키 형식 버전을 올린다). NaN 단일화 값은 quiet NaN(F32 0x7fc00000·F64 0x7ff8000000000000).
  - 리플렉션 기록: 부호 정수·enum은 I64, 부호 없는 정수는 U64(폭과 무관 — 같은 값이면 같은 키), ClassRef는 TypeId Guid, Soft는 Guid + 경로 String, Name은 표시 문자열을 ASCII 소문자로 접은 것(Name은 ASCII 대소문자 무시 동등이고 표시 철자는 먼저 등록한 쪽을 따르므로 접지 않으면 같은 설정이 프로세스마다 다른 키가 된다 — Name pool이 없는 Registry는 Unsupported), ObjectRef·WeakObjectRef·Text는 Unsupported. Map·Set은 그 컨테이너의 항목만 모아(원소 수만큼 미리 잡는다 — 항목 열기는 할당하지 않는다) 정규 인코딩 사전순으로 흘려 넣는다.
  - SCC key와 구성원 key는 영역 문자열(`RocksNuggetScc`·`RocksNuggetMember`)로 시작해 다른 키 기록과 byte가 겹치지 않는다. null AssetId(구성원·의존)도 InvalidArgument다.
  - Record: schema Guid `ROCKPILE`/1(입력 항목 /2), 필드 1–6 순서·wire를 읽을 때 본다(TLV는 순서를 강제하지 않는다). 정규 형식에 Identity의 storedSize = rawSize·ContentHash = RawHash, indexLength ≤ storedSize, 표 해시는 storedSize ≥ 1 MiB일 때만을 더했다. 소비 입력 비교는 bucket 길이 → 내용 → key → digest(bucket ≤ 64 byte라 LE 길이 byte 사전순과 같다).
  - `.rnug` v2: 쓰기는 계획(`PlanNuggetFile` — 머리 + Record byte·값마다 조각 표, 값 byte는 복사하지 않는다)과 붙이기(`AppendNuggetFile`)로 나눴다(A3b가 큰 값을 파일로 흘려 쓴다). 읽기는 `OpenNuggetFile`(머리·Record·표 검증) → `ReadNuggetValue`(ContentHash) → `DecodeNuggetValue`(Identity 복사·Rcbk 해제와 IndexHash·RawHash). 다른 파일 버전·더 새 Record schema 버전은 Unsupported(손상과 구분 — 읽는 쪽은 지우지 않고 미적중으로 다룬다, 공유 `localPath`의 새 도구 항목을 옛 도구가 지우지 않게). `DecodeNuggetValue`는 공개 함수라 받은 행을 먼저 정규 형식 행 검사(`IsValidNuggetValueRow`)로 거부한다.
  - 리뷰 반영(필수 둘·권장 여섯): 더 새 Record schema의 Unsupported 전달, enum 문장, VisitValues의 "visitor는 순회 중 대상을 바꾸지 않는다" 계약, 항목 열기 실패 전달, Rcbk 색인 규칙과 공개 해제의 행 검사, 할당 실패 시험의 byte 비교, SCC 입력 검사(빈 목록·튜플 구성원·SCC 안 간선), Name 대소문자 접기.

- A3b 구현 세부(2026-11-22, 착수 — 권장안으로 확정, 결정 7–9를 구체화):
  - API `NuggetFileSystemBackend(allocator, files, root, clock)`: `Open()`(루트·`tmp` 만들기 — 만들 수 없으면 읽기 전용 모드, 24시간 지난 tmp 정리), `Get(NuggetBackendGet) -> NuggetGetResult`(상태·digest·Record·값마다 원본 byte, 원인 오류), `Put(NuggetBackendPut) -> NuggetPutResult`(상태·새 digest·기존 digest), `Touch(key)`, `Remove(key)`(관리용 — `ReportCorrupt`도 같다), `Collect({maxBytes, unusedDays}) -> 지운·남은·건너뛴 수와 byte`. Get·Put은 실패를 Result로 돌려주지 않고 상태(`Miss`·`Corrupt`·`Dropped` 등)와 원인 오류로 돌려준다(캐시 실패는 빌드 실패가 아니다 — R-1, `required` 판정은 A3c 캐시가 한다). 상태 enum `NuggetStatus`는 결정 13의 열하나다.
  - 시각은 주입한 `NuggetClock`(기본 `NuggetSystemClock` — `DateTime::UtcNow`의 tick, 시험은 수동 시계)이다. Touch·tmp 나이·GC의 기준이다.
  - 경로: 항목 `<root>/<k0k1>/<key 64자 소문자 hex>.rnug`, 임시 `<root>/tmp/<Guid>.tmp`, shard 잠금 `<root>/tmp/<k0k1>.lock`(지우지 않는다). 잠금은 PlatformBase `FileLock`(물리 파일 시스템)이라 루트는 물리 경로다(시험의 메모리 파일 시스템도 물리 임시 루트 위에 둔다 — A2와 같다). 잠금 대기 상한은 기본 `NuggetReplaceLockMs` 5초(`SetReplaceLockTimeout`으로 바꾼다 — 시험).
  - Put 순서: 값마다 저장 압축 판단(64 KiB 이상이면 RCBK Zstd 기본 수준·블록 64 KiB로 압축해 더 작을 때만, `DecodeNuggetValue`로 왕복해 RawHash 확인) → `PlanNuggetFile` → tmp에 머리·값·표를 흘려 쓰기·Flush·Close → `Move(no-replace)`. 대상이 있으면 기존을 열어 digest 비교(같으면 값 byte의 ContentHash까지 고정 버퍼로 흘려 읽어 확인한 뒤 `Exists` + Touch — 손상 항목을 Exists로 남기지 않는다, 값 하나 16 GiB를 통째로 올리지 않는다, 값이 손상이면 아래 교체로, 다르면 `Conflict` + 기존 digest), 기존이 손상이면 shard 잠금 안에서 다시 열어 여전히 손상일 때만 `Replace`(`Stored`). 기존이 다른 버전(Unsupported — 새 도구의 항목)이면 지우지도 바꾸지도 않고 `Dropped`(Unsupported)다. 어느 경우든 tmp는 지운다(못 지우면 다음 tmp 정리가 지운다).
  - Get: 없음 `Miss`, 다른 버전 `Miss`(Unsupported — 지우지 않는다), 손상 `Corrupt`(지운다), key가 다른 echo는 손상, bucket만 다르면 `Miss`(InvalidArgument — 지우지 않는다), 청한 값이 없으면 `partial`이면 `Partial`(있는 값만) 아니면 `Miss`, 같은 ValueId를 두 번 청하면 `Miss`(InvalidArgument — 행은 ValueId 오름차순이라 이진 검색), `skipData`면 값을 읽지 않는다(Record만 — 조각 표는 연 때 이미 검증), `skipMeta`면 meta를 비운다. 적중이면 Touch(실패는 무시).
  - 시험 hook `NuggetBackendHooks`(지점 `NuggetBackendStep` — 이동 직전 `BeforePublish`, 이동이 AlreadyExists였을 때 판정 직전 `BeforeJudge`, 손상 교체의 잠금 안 재판정 직전 `BeforeJudgeLocked`): 같은 프로세스의 경합 순서와 두 프로세스 시험의 pipe gate를 결정적으로 만든다. 제품 코드는 걸지 않는다.
- A3b 상태: **구현** — 대조표 14.1.55절, coverage 14.1.56절. 구현에서 정한 것:
  - 루트를 만들 수 없으면(자리가 파일 등) 열기 실패다. 읽기 전용 모드는 tmp를 만들 수 없을 때다(이미 있는 루트 디렉터리는 만들기가 성공하므로 루트로는 판정하지 않는다).
  - 같은 digest의 기존 항목은 값 byte의 ContentHash까지 확인한 뒤에만 `Exists`다(열기 검증은 값 byte를 읽지 않으므로 — 손상 값을 가진 항목을 Exists로 남기면 다음 Get까지 손상이 남는다). 손상이면 교체 경로로 간다.
  - 이동이 AlreadyExists였는데 판정 때 항목이 없으면(GC·손상 삭제) 한 번 더 이동하고, 그것도 실패하면 `Dropped`(Busy)다. 이동의 다른 실패(delete-pending 이름의 AccessDenied 등)·디렉터리 만들기·쓰기·Flush·Close 실패는 `Dropped`(그 오류)이고 tmp를 지운다.
  - 기존 항목을 읽을 수 없으면(IO 오류·할당 실패) 손상으로 보지 않는다 — Get은 `Miss`(그 오류), Put은 `Dropped`(그 오류)이고 지우지 않는다. 손상(InvalidArgument)만 지운다.
  - GC는 루트의 16진 두 글자 디렉터리(link 아님) 안의 `<그 두 글자로 시작하는 64자 소문자 16진>.rnug` 일반 파일(link 아님)만 본다. 지우지 못한 항목은 `skipped`이고 남은 합에서 빼지 않는다. tmp 정리는 지난 `.tmp` 일반 파일만이다(디렉터리·다른 이름·잠금 파일은 둔다).
  - `FileSystem::Exists`는 디렉터리에 거짓이다(UE `FileExists`와 같다 — 시험은 디렉터리를 Stat으로 본다). A3 뒤 `FileExists`·`DirectoryExists`로 바꿨다(사용자 지시 2026-11-22, PlatformLayer 문서 2026-11-23).
  - 리뷰 반영(필수 하나·권장 넷): 키별 잠금 파일을 tmp 정리가 try-lock 뒤 지우던 것을 고정 256개 shard 잠금(지우지 않음)으로 바꿨다(FileLock 계약 — POSIX에서 상호 배제가 깨졌다, 처음 설계의 "try-lock 뒤에만 지운다"는 틀렸다). Exists 확인을 고정 버퍼 흘려 읽기로(`VerifyNuggetValue`), 잠금 대기 상한 주입, 값 선택 중복 거부·이진 검색.
  - 남는 경합: 잠금 밖 경로(Get의 손상 삭제 + 다른 Put의 no-replace 이동)가 잠금 안 재판정과 `Replace` 사이에 끼면 새 정상 항목을 다른 정상 값으로 덮을 수 있다. 결과는 정상 값끼리의 교체이고(digest가 다르면 다른 세션에 `DigestChanged`) 틀린 적중은 아니다 — 모든 쓰기를 잠그지 않는 대가로 받아들인다(no-replace 이동이 동시 쓰기의 기본 경로).

- A3c 나눔(2026-11-22): A3c1 동기 캐시 코어(`NuggetCache` — 설정·backend 선택·Memory 계층·세션 고정·소비 입력 대조·정책·통계·SCC 헬퍼), A3c2 요청 객체(`NuggetGetRequest`·`NuggetPutRequest` — 일괄·비동기·취소·대기 Put 상한·`Shutdown`·진행 중 소멸 Fatal). 각각 14절 완료 조건 넷.
- A3c1 구현 세부(권장안으로 확정, 결정 10·11·13–15를 구체화):
  - 정책 `NuggetPolicy`(u32 비트 — `QueryLocal` 1·`QueryRemote` 2·`StoreLocal` 4·`StoreRemote` 8·`SkipData` 16·`SkipMeta` 32·`PartialRecord` 64·`KeepAlive` 128, `Query`·`Store`·`Default`, 비트 연산자). Get은 `QueryLocal`이 없으면 `Miss`(원격이 없다 — 결정 14), Put은 `StoreLocal`·`StoreRemote`가 둘 다 없으면 `Dropped`(오류 없음 — 저장하지 말라는 정책이다), `StoreRemote`는 `StoreLocal`을 함의한다. `KeepAlive`는 Memory 적중에서도 backend Touch를 한다(backend 적중은 A3b 규칙대로 늘 Touch).
  - 설정 `ReadNuggetCacheSettings(allocator, files, projectRoot, failure)`: `Config/AssetNuggets.json`을 Core JSON(엄격)으로 읽는다(ReflectionText가 아닌 이유: 칸 넷짜리 고정 형식이고 모듈이 리플렉션 등록에 의존할 까닭이 없다 — `AssetAliases.json`과 같은 방식). 없으면 기본값(filesystem·false·`Intermediate/AssetNuggets`·null 세대). 거부(InvalidArgument + `NuggetSettingsFailure{field, line, column}`): JSON 문법, 루트가 객체 아님, 모르는 키, 칸 타입, `backend` 값이 넷 밖, `localPath` 빈 문자열·NUL 포함·Windows 현재 드라이브 루트(`\x`·`/x`)·드라이브 상대(`C:x`)·device namespace(프로세스의 현재 드라이브·디렉터리에 따라 위치가 바뀐다 — A2f1a 원본 루트와 같은 규칙, Core `Path::Root`), `generation`이 Guid 표준 문자열 아님. 그 밖 읽기 오류는 그 오류, 할당 실패 OutOfMemory. 상대 `localPath`는 프로젝트 루트 기준이고 루트에 붙은 경로(POSIX 루트·Windows 드라이브 절대·UNC)는 그대로다.
  - `NuggetCache(allocator, files, clock, limits)` → `Open(settings, projectRoot)`: off는 backend 없음(모든 Get `Miss`·Put `Dropped`, 진단 없음 — Memory 계층도 쓰지 않는다), rockpile은 `Unsupported`(열기 실패), auto·filesystem은 `NuggetFileSystemBackend`를 연다. filesystem 열기 실패는 `required`면 Open 실패(그 오류), 아니면 off로 내리고 원인을 `OpenError()`로 남긴다(진단). 결정은 Open 때 한 번이다. Open은 한 번만(두 번째는 InvalidArgument), Open 전 Get·Put은 `Miss`·`Dropped`(InvalidArgument). Open 뒤 Get·Put·`GetSccMember`·`PutScc`·`Stats`는 여러 스레드에서 동시에 부를 수 있다(요청 객체의 worker). 내부 표(Memory·고정)는 mutex 하나로 지키고 backend IO는 잠금 밖에서 한다.
  - 결과: `NuggetCacheGetResult{status, error, failed, source(None·Memory·Local), digest, record, values}`, `NuggetCachePutResult{status, error, failed, digest, existing}`. `failed`는 backend 실패인 Miss·Dropped(`NuggetCache::IsFailure` — IO·할당·잘못된 요청 등)이고 A3c2 요청이 `required`일 때 요청 오류로 올린다. 실패가 아닌 오류: None, Unsupported(다른 버전·읽기 전용), CapacityExceeded(한도 위반 — Record 1 MiB·SCC 4,096 등, 캐시하지 않음 — 결정 7 "Dropped + 진단"), Busy(일시 경합 — 손상 교체 잠금 대기 초과·재게시 실패). Corrupt는 실패가 아니다(지우고 미적중 — 다시 만든다).
  - Memory 계층: 키 → 항목(bucket, digest, Record, 쥔 값 — ValueId 순) + LRU. 항목 크기 = 쥔 값 byte + meta + 행 × 160 + 입력 × `NuggetMemoryInputBytes`(128) + `NuggetMemoryEntryBytes`(256). Get 적중(SkipData 아님)과 Put(Record를 계획한 뒤 — 결과가 Stored·Exists·Conflict·Dropped라도, backend가 실패해도 이 세션이 쓴 값이다)이 넣는다. 값 하나가 `memoryValueBytes`(64 MiB)를 넘으면 그 값은 쥐지 않는다. 같은 digest 항목에는 없는 값을 더한다. 합이 `memoryBytes`(256 MiB)를 넘으면 고정 보관이 아닌 항목을 오래 쓰지 않은 것부터 버린다(크기가 상한보다 큰 항목은 넣지 않는다). Memory 적중 조건: 같은 bucket, SkipData거나 청한 값(비면 Record의 모든 값)을 다 쥠 — PartialRecord면 Record에 없는 값은 빠져도 된다(Partial), Record에 있는데 쥐지 않은 값이 있으면 backend로 간다. Put의 값 버퍼는 Memory로 옮겨 간다(`Span<NuggetValue>` — Put 뒤 bytes가 비어 있을 수 있다, 소유권 이전). 항목의 Record와 값은 공유 버퍼(`SharedPtr`)다: Memory 적중은 잠금 안에서 참조만 얻고 결과로의 복사는 잠금 밖에서 하며, 넣을 후보(Local 적중의 복사·Put의 이동)도 잠금 밖에서 만든 뒤 잠금 안에서는 연결만 한다 — 값 하나가 64 MiB여도 mutex를 쥔 동안 큰 복사가 없다(A3c2의 worker 여럿이 줄을 서지 않게).
  - backend Put 결과에 계획한 Record를 더한다(`NuggetPutResult::record` — 계획이 성공했을 때, A3b API 확장). Memory 항목의 Record는 그것이다(backend 적중의 Record와 같은 저장 표현 — 값은 늘 해제한 원본).
  - 세션 고정 표: key → (digest, 상태 — 보통·`changed`(DigestChanged를 돌려줌)·`regenerated`(그 뒤 다시 만든 Record를 고정 보관함)·`uncached`(캐시 없음)). 결정 11의 "(RecordDigest, inputs)"에서 inputs는 표에 두지 않는다 — 대조는 적중한 Record의 inputs로 하므로 표에 둘 까닭이 없다(키당 약 80 byte). Get 적중(Memory·backend 모두): (1) 고정이 있고 digest가 다르면 `DigestChanged` — 값을 버리고 Memory 항목을 지우고 `changed`로 표시한다. (2) inputs 각 항목의 key가 고정돼 있고 digest가 다르면 그 적중은 `Miss`(InputMismatch 통계 — 고정하지 않는다). (3) 받아들이면 고정 없는 inputs와 이 키를 그 digest로 고정한다. 고정 표 할당 실패는 `Miss`(OutOfMemory, failed). Put: `uncached`면 `Dropped`(오류 없음). `changed`인 키의 Put은 값 전부(64 MiB 규칙 예외)를 고정 보관(축출 제외)하고 고정 digest를 새 Record로 바꾼 뒤 `regenerated`로 둔다 — 고정 보관 합이 `pinnedBytes`(256 MiB)를 넘으면 그 키를 `uncached`로 내린다(이후 Get `Miss`·Put `Dropped`, 통계). 고정이 다른 digest인 보통 키의 Put(DigestChanged 없이 다른 Record — 비결정적 처리기)은 Memory에 넣지 않고 고정을 바꾸지 않는다. 고정이 없거나 같으면 이 Put의 digest로 고정한다(결과가 Conflict여도 — 이 세션은 자기가 만든 값을 이미 썼다. 디스크 판이 다르면 나중 Get이 DigestChanged를 내고 위 경로로 간다).
  - 통계 `NuggetCacheStats`(원자 카운터): Memory 적중·Local 적중·Partial·Miss·DigestChanged·InputMismatch·Corrupt·Stored·Exists·Conflict·Dropped·Uncached 키 수·Memory byte·고정 보관 byte·고정 키 수.
  - SCC 헬퍼(결정 15): `NuggetSccValueId(member, part)` = (member << 16) | part(member < 4,096, part < 65,536 — 넘으면 assert), `WriteNuggetSccMeta(members, meta)`(AssetId 16 byte씩, high → low big-endian, 순서 = 구성원 순번, 빈 목록·null·중복 InvalidArgument, 4,096 초과 CapacityExceeded), `FindNuggetSccMember(meta, asset)`(16 byte 배수 아님 InvalidArgument, 없으면 NotFound). `NuggetCache::PutScc(members, put)`은 meta를 구성원 목록으로 채워 Put한다(4,096 초과는 `Dropped`(CapacityExceeded — 캐시하지 않음, 실패 아님), 값 id의 구성원 순번이 구성원 수 이상이면 `Dropped`(InvalidArgument)). `GetSccMember(bucket, sccKey, asset, policy)`는 SkipData Get으로 meta를 얻어 순번을 찾고 그 순번의 값만 골라 다시 Get한다(구성원이 없으면 `Miss`(NotFound), 그 순번의 값이 없으면 빈 Hit).
- A3c1 상태: **구현** — 대조표 14.1.57절, coverage 14.1.58절. 구현에서 정한 것:
  - 모듈 파일 `NuggetPolicy.h`·`NuggetCacheSettings.h`·`NuggetScc.h`·`NuggetCache.h`. `localPath` 해석은 Core `Path::Root`(경로 문법 분류 — 이미 있는 공용 기능)를 쓴다.
  - 고정 표 크기: 키당 약 80 byte(digest 32 + key 32 + 상태 + 해시 표 칸)이고 세션 동안 줄지 않는다(고정을 지우면 세션 일관성이 깨진다). 전체 쿠킹이 100만 키를 쓰면 약 80 MB다 — 결정 11의 "표만 커짐"이 받아들인 비용이다(값 byte는 쥐지 않는다).
  - 불변식: Memory 항목의 digest는 늘 그 키의 고정 digest다(항목은 고정과 같을 때만 넣고, 고정이 바뀌는 곳 — 다시 만든 Record 고정 보관 — 은 항목을 바꾼다, DigestChanged는 항목을 지운다). 그래서 받아들인 적중의 Memory 합치기 대상은 같은 Record이고, 고정 보관 항목은 값을 다 쥐어 Memory가 늘 답한다(assert). 고정 보관 항목은 지우지 않는다 — DigestChanged(다른 bucket으로 같은 키를 청한 호출자 결함 등)도 건너뛴다.
  - 고정 보관 판정은 Put 앞의 `Changed`와 Put 뒤 잠금 안의 상태를 둘 다 본다(잠금 밖 backend IO 사이에 다른 Put이 먼저 고정 보관했거나 Get이 DigestChanged를 냈으면 남기지 않는다). Get의 판정도 backend IO 뒤 잠금 안에서 캐시 없음을 다시 본다. 시험은 hook 파일 시스템(열기 직전 같은 캐시를 다시 부름)으로 이 창을 결정적으로 만든다.
  - DigestChanged 뒤 디스크가 고정 판으로 돌아오면 적중을 받아들이고(고정 상태는 `Changed` 그대로) Memory에 넣는다 — 그 뒤 다시 만든 Record Put은 그 항목을 지우고 고정 보관한다.
  - Memory 적중이 Record에 없는 값(PartialRecord 아님)을 보면 backend에 묻지 않고 `Miss`다(같은 digest의 Record는 그 값이 없다). 같은 값을 두 번 청하거나 bucket이 다르면 Memory는 답하지 않고 backend가 판정한다.
  - 열기 전 Get·Put은 통계에 세지 않는다(계약 위반). off·Store 없음·캐시 없음 Put은 `Dropped`로 센다.
  - 리뷰 반영(권장 넷): failed에서 한도 위반(CapacityExceeded)·일시 경합(Busy)을 뺐다(`IsFailure`), Memory 복사를 잠금 밖으로(공유 버퍼), `localPath`의 Windows 현재 드라이브 루트·드라이브 상대 거부(처음 구현은 PlatformBase에 상대 판정을 새로 더했으나 Core `Path::Root`가 이미 있어 되돌렸다), 고정 표 크기 기록.

- A3c2 구현 세부(2026-11-22 착수 — 권장안으로 확정, 결정 12를 구체화):
  - 파일 `NuggetRequest.h`. 항목 `NuggetGetItem{bucket, key, values, policy}`·`NuggetPutItem{bucket, key, values(Array<NuggetValue> — Start가 옮겨 간다), inputs, meta, policy}`. Start가 bucket·값 선택 목록·inputs·meta를 복사하고 Put 값 버퍼는 옮긴다(호출자 항목 배열은 Start 뒤 바로 놓아도 된다).
  - `NuggetGetRequest`·`NuggetPutRequest`(호출자 소유·이동·복사 금지 — 결과 slot을 객체 안에 둔다): `Start(cache, items, scheduler)`, `RequestCancel()`, `Handle()`(완료 handle — 의존·Wait용), `Results()`(handle 완료 뒤 항목 순서 그대로, 시작 전 StaleHandle·완료 전 Busy), `Outcome()`(required 캐시에서 failed 항목이 있으면 DependencyFailed — 결정 10 "required면 그 요청 결과가 오류", 아니면 성공 — 결과와 같은 시점 규칙). 진행 중 Start는 Busy, 빈 항목 목록은 바로 완료다. 완료 뒤 같은 객체로 다시 Start할 수 있다.
  - 완료 handle은 `WorkerTaskEvent`다: 남은 수를 (항목 수 + 1)로 두고 항목이 끝날 때마다 원자적으로 줄이며 Start가 제출을 마친 뒤 1을 줄인다 — 0이 되는 쪽이 캐시 진행 중 목록에서 빼고 Trigger한다(maxDependencies(16)에 묶이지 않는다, 제출 중에 완료되지 않는다). 항목마다 worker 작업 하나. 제출 실패(CapacityExceeded·ShuttingDown 등)는 그 항목과 뒤 항목을 `Dropped`(그 오류, 실패 아님)로 끝내고 이미 낸 항목은 끝까지 간다 — Start는 성공한다. 이벤트 만들기·결과 slot·입력 복사의 실패는 Start 실패(그 오류 — 아무것도 내지 않았다, 옮긴 Put 값은 항목에 남는다).
  - 항목 작업의 capture는 ticket(요청·순번)을 쥔다. scheduler가 body를 실행하지 않고 capture를 정리하면(scheduler Shutdown(Cancel)·의존 실패) ticket 소멸자가 그 항목을 `Cancelled`로 끝낸다 — 요청이 영영 완료되지 않는 일이 없다.
  - 취소는 협력적이다: 항목 작업이 시작할 때 요청 취소·작업 취소를 보면 `Cancelled`(Get·Put 모두 — FileSystem Put은 아직 쓰지 않았다), 진행 중 Get·Put은 끝낸다(캐시 연산은 동기 IO 단위). FileSystem에서 `Unknown`은 생기지 않는다(9.1 결정 13).
  - 대기 Put byte 상한 `NuggetPendingPutBytes`(1 GiB — `NuggetCacheLimits::pendingPutBytes`): Start가 Put 항목 값 byte 합을 캐시 원자 카운터에 예약하고, 넘으면 그 항목은 제출하지 않고 `Dropped`(CapacityExceeded, 실패 아님 — 메모리 보호), 항목이 끝나면(실행·취소 모두) 돌려준다.
  - 수명: 소멸자는 진행 중이면 취소를 요청하고 완료를 기다린다. 같은 scheduler의 worker(다른 작업의 body·capture 정리 포함)에서 진행 중인 요청을 소멸하면 Fatal(기다리면 교착 — CompressionTask와 같은 규칙). 요청은 Start 때 캐시 진행 중 목록에 들어가고 마지막 항목이 뺀다.
  - `NuggetCache::Shutdown(drainMs)`: 새 Start 거부(이후 Start는 모든 항목 `Cancelled`, handle 바로 완료) → 진행 중 Get 요청 취소 → Put 요청은 drainMs(단조 시계 기준 전체 기한)까지 완료를 기다림 → 남은 Put 요청 취소 → 모든 요청 완료까지 기다림. 동기 Get·Put도 Shutdown 뒤에는 `Cancelled`(통계 없음 — 계약 밖 호출이 아니라 종료 뒤 호출이다). 진행 중 요청이 있는데 캐시를 소멸하거나 진행 중 요청의 scheduler worker에서 Shutdown을 부르면 Fatal. 두 번째 Shutdown은 아무것도 하지 않는다.
  - 시험: hook 파일 시스템이 열기에서 `ManualResetEvent`를 기다려 항목 작업을 붙잡는다(잠·시간 경합 없음 — 순서는 이벤트로만). Shutdown의 단계 사이는 시험 hook `NuggetCacheHooks`(지점 `AfterCancelGets`·`AfterDrain`)로 잇는다. Fatal은 자식 프로세스(`--process-probe`, `PrepareFatalTestProcess`)로 확인한다.
- A3c2 상태: **구현** — 대조표 14.1.59절, coverage 14.1.60절. 구현에서 정한 것:
  - 요청 항목은 캐시의 내부 Get·Put(종료 검사 없음)을 부른다 — Shutdown의 drain 동안 Put 항목이 끝나야 하므로(동기 Get·Put만 종료 뒤 Cancelled). 시작 전 취소는 요청이 본다.
  - 완료 handle 상태는 Pending(진행 중)이거나 끝난 상태뿐이다(외부 이벤트). scheduler를 Shutdown(Cancel)으로 끝내면 완료 handle이 항목보다 먼저 Cancelled가 된다 — 그 scheduler의 요청은 scheduler Shutdown이 돌아온 뒤에 결과를 보거나 소멸한다(헤더 계약, 보통은 캐시 Shutdown을 먼저 부른다). 남은 항목은 worker가 capture를 정리할 때 ticket이 Cancelled로 끝내고 진행 중 목록에서 뺀다(캐시 Shutdown이 영영 기다리지 않는다).
  - `IsCancelRequested()`(취소 요청 여부)를 공개하고, 시험 hook `NuggetRequestHooks::onCancel`(취소 요청 때 부름 — 진행 중 소멸 시험의 순서 gate)을 둔다.
  - Shutdown의 drain·마지막 대기는 진행 중 목록의 조건 변수(요청이 빠질 때 알림)로 기다린다 — 완료 handle을 기다리지 않으므로 handle이 먼저 끝난 요청(scheduler Cancel·handle 직접 취소)이 목록에 남아도 바쁜 반복이 없다.
  - 항목이 아직 진행 중 목록에 남은 요청의 다시 Start는 Busy다(완료 handle이 먼저 끝난 경우 — 옛 ticket이 새 무장 표를 읽지 않게).
  - 리뷰 반영(권장 셋): 다시 Start 막기·계약(handle 직접 취소 금지 포함), 조건 변수 대기, 진행 중 소멸 시험의 폴링을 취소 hook으로.
  - 시험 hook 파일 시스템은 여러 worker가 동시에 열어도 callback을 한 번만 부른다(꺼내기를 잠금 안에서 — 처음 구현은 두 worker가 함께 callback을 보고 한쪽이 null을 불러 간헐 크래시가 났다).

## 10. Cooker

- `TargetProfile`(엔진 `Engine/Config/TargetProfiles/<id>.json` + 프로젝트 재정의): id, 그래픽 backend, 형식 지원 표(비압축, 블록 압축 지원 = 이번에는 없음), LOD 그룹 표(12.4.5절), bulk 정렬. 엔진 기본: `Windows`, `Android`(형식 표만, 그래픽 backend 후속). JSON은 리플렉션 공용 codec `ReflectionText`(struct 멤버 병합, Map은 선택 `MergeByKey` — ReflectionExtensions 10.4절)로 읽고, 프로젝트 재정의를 그룹 단위로 덮는 방식은 A4가 고른다.
- `CookSettings`(`Config/CookSettings.json`): AlwaysCook·NeverCook(경로·디렉터리), PrimaryAsset 규칙(타입 → 디렉터리), `CookSoftReferences`. 같은 codec으로 읽는다. bundle은 규칙이 아니라 PrimaryAsset의 Soft 참조 필드 지정자에서 온다(저장 때 색인 태그 — 10.2절 결정 9, 2026-11-23 교차 검증으로 고침).
- 대상: 전체 쿠킹은 (AlwaysCook ∪ PrimaryAsset) → Hard 전이 ∪ (설정 시) Soft 전이. 부분 쿠킹은 (인자 경로) → Hard 전이 ∪ (설정 시) Soft 전이 ∪ 역방향 Build 폐포(원본 Texture2D를 고치면 그것을 묶는 Texture2DArray도 다시 쿠킹) ∪ 기존 매니페스트에서 cookKey가 달라진 항목. 부분 쿠킹은 에디터에서 사라진 에셋의 매니페스트 항목을 지우고, 그것을 하드 참조하는 남은 항목은 `MissingDependency`로 실패시킨다. 공통: EditorOnly 제외, NeverCook에 걸리면 `NeverCookReferenced`. 규칙에도 참조에도 없는 에셋은 쿠킹하지 않는다.
- 처리: 타입별 `CookProcessor`(에셋 너겟 사용)가 에디터 객체 → 쿠킹 객체·bulk. 쿠킹 데이터는 같은 타입에서 EditorOnly 필드를 뺀 것이다(EditorOnly 필드는 런타임 바이너리에 있고 쿠킹 데이터에 기록·로드되지 않는다). 항목마다 **cookKey**를 계산한다. 지역 입력 = (에디터 패키지 콘텐츠 해시, 논리 경로(경로 힌트·디렉터리 기반 NeverCook·PrimaryAsset 판정이 바뀐다), 그 항목에 적용된 CookSettings 판정(PrimaryAsset·bundle), 처리기 build key들, 쿠커 버전, 쿠킹 패키지 formatVersion·섹션 schemaVersion 집합, 타입 스키마 버전 표 SHA-256, 패키지 export 구체 클래스별 (TypeId, 클래스 기본값 지문) 정렬 목록(A5 — base 기본값은 파생 레코드에 이미 들어 있다, RuntimeClassMetadata '클래스 기본값 결정'), 프로파일 출력 설정 SHA-256). 엔진 버전은 지역 입력이 아니다 — 쿠커 출력 의미가 바뀌면 쿠커 버전(엔진 버전과 독립된 정수)을 올린다([ReflectionFoundations](ReflectionFoundations.ko.md) 8.2절). 하드 참조 순환을 위해 SCC 단위로 계산한다: SCC key = SHA-256(구성원 지역 입력을 AssetId 순으로, SCC 밖 하드 의존 튜플 (구성원 AssetId, 의존 AssetId, 그 cookKey)의 정렬 목록), cookKey = SHA-256(SCC key, AssetId). 순환이 없어도 같은 식이다.
- 출력: 쿠킹 루트 `<Project>/Saved/Cooked/<Profile>/` 아래 `CookedManifest.rman`과 `Packages/`. 패키지 파일 이름은 내용 주소(4.2절)라 게시된 파일을 덮어쓰지 않는다.
- 게시:
  1. 각 패키지를 `.staging/`에 기록·Flush한 뒤 `Move(no-replace)`로 `Packages/<a>/`에 놓는다. 같은 이름이 이미 있으면 크기·SHA-256을 확인해 같으면 임시 파일만 지우고, 다르면(손상) `CorruptEntry` 경고 후 `Replace`로 바꾼다.
  2. 바뀐 `Packages/<a>/`와 `Packages/`, 그리고 이번 게시에서 새로 만든 상위 디렉터리(첫 쿠킹이면 쿠킹 루트 — 그 안의 `Packages` 항목)를 안쪽부터 `FileSystem::SyncDirectory`(A0-P)로 동기화한다 — 매니페스트보다 패키지 이름이 먼저 매체에 남아야 한다. POSIX는 디렉터리 fsync(macOS `F_FULLFSYNC`), Windows는 항목 추가 권한으로 연 디렉터리의 `FlushFileBuffers`다(관리자 권한 불필요. 로그 순서에 기대지 않으므로 `Packages`가 다른 volume을 가리키는 link여도 순서가 지켜진다). 디렉터리 동기화를 지원하지 않는 FS는 그 FS의 보장이 전부라 성공이다. `NotDurable`은 게시를 멈추고(매니페스트를 바꾸지 않는다) 오류로 보고한다. `Unsupported`(wrapper 기본 구현)도 오류다.
  3. 매니페스트를 임시 파일에 써서 Flush하고 `Replace`로 한 번에 바꾼다(POSIX는 쿠킹 루트도 동기화된다).
  매니페스트 교체가 유일한 전환점이므로 reader는 이전 매니페스트(이전 파일이 모두 있음) 또는 새 매니페스트(새 파일이 모두 있음)만 본다. 실패하면 매니페스트는 그대로이고 놓인 새 파일은 다음 전체 쿠킹이 지운다.
- 전체 쿠킹(`asset-cook --profile=X`)은 매니페스트를 바꾼 뒤 새 매니페스트가 가리키지 않는 패키지 파일을 지운다(이전 매니페스트로 이미 실행 중인 게임은 그 파일을 `NotFound`로 받는다 — 개발용 쿠킹 디렉터리의 한계이며 배포본은 바뀌지 않는다). 부분 쿠킹은 대상만 다시 만들고 기존 매니페스트에 병합하며 패키지 파일은 지우지 않는다(사라진 에셋의 매니페스트 항목만 뺀다). 병합은 기존 매니페스트의 프로파일 id·프로파일 출력 설정 SHA-256·CookSettings SHA-256·쿠커 버전이 모두 같을 때만 하고, 다르면 `IncompatibleCook`(전체 쿠킹 필요)이다. 처리기·포맷·타입 스키마·툴체인 변화는 항목별 cookKey가 잡는다(위 지역 입력).
- 매니페스트 v1(`CookedManifest.rman`): 헤더 `{ "RMAN", 버전 1, 항목 수, 프로파일 id, 프로파일 출력 설정 SHA-256, CookSettings SHA-256, 쿠커 버전, cookedByEngineVersion(주·부·패치 — 진단용, 병합·로드 판정에 쓰지 않음) }` + 항목 `{ assetId, typeId, 경로 힌트, 패키지 SHA-256, 패키지 크기, bulkRawBytes(BulkIndex rawBytes 합 — 10.1절 5), cookKey, PrimaryAsset 표시, bundle 목록, 런타임 의존 목록 { assetId, kind(Hard·Soft) } }`(AssetId 정렬) + 파일 전체 CRC32. 손상은 `CorruptPackage`, 버전 불일치는 `UnsupportedVersion`. 로더는 시작할 때 매니페스트 프로파일 id가 실행 플랫폼의 프로파일과 같은지 확인하고(다르면 `IncompatibleCook`), 패키지를 열 때 크기·SHA-256을 먼저 보고 그다음 패키지 종류·assetId·대표 에셋 타입·bulk raw 합·Summary 프로파일 칸·Dependencies(Hard·Soft)를 매니페스트와 대조한다(다르면 `CorruptPackage` — 10.1절 8 `CheckCookedPackage`). SCC 계산은 매니페스트의 Hard 의존으로 로드 전에 한다.
- 도구 빌드 ID·엔진 버전: 빌드 시스템이 쿠커·에디터 도구 실행 파일마다 주입하는 값이며 진단·추적용이다(병합 조건이 아니다). 쿠킹 보고서(엔진 버전은 CookedManifest 헤더의 cookedByEngineVersion에도)에 기록하고 쿠킹 패키지 Summary·cookKey에는 넣지 않는다 — Summary에 있으면 구성 사이 쿠킹 byte 동일(시나리오 13·25)이 깨지고 쿠킹 패키지 이름(내용 주소)이 엔진 빌드마다 바뀐다.
- 런타임 쿠킹 루트: 도구(`asset-load-cooked`)는 `<Project>/Saved/Cooked/<Profile>/`. 게임은 `EngineAssets`가 부팅 CVar `asset.cooked_root`(설정 파일 또는 `--set=asset.cooked_root=<dir>`, 비면 실행 파일 디렉터리의 `Cooked/`)를 `AssetManager`에 넘긴다(11.8절 3). 쿠킹 결과를 배포 위치로 옮기는 패키징 단계는 후속이다.
- 쿠킹 bulk codec: 프로파일 설정(None·Zstd, 기본 None — 후속 스트리밍의 mip 단위 접근을 위해 entry 단위 압축).
- 결정성(범위: 같은 도구 빌드에서 호스트 무관하게 같은 바이트): 정렬된 순회, 병렬 결과 정렬 후 기록, ID는 에디터 값 복사, 패딩 0. 이미지 처리 모듈은 SIMD 분기 없이 스칼라 경로만 쓰고, 초월 함수(pow·exp2·log2·sin)와 sRGB·감마 변환은 CRT를 쓰지 않는 Core 결정적 수학(A0-C)으로 하며 쿠킹 작업 동안 부동소수점 환경 guard(A0-C)를 건다. 현재 Core `LinearColor::ToSRGB`는 CRT `pow`를 써서 호스트마다 결과가 다를 수 있으므로(Color.h 주석) A0-C에서 결정적 수학의 표·함수로 바꾼다(런타임과 쿠킹의 sRGB 구현을 하나로). 결정적 수학 버전은 너겟 키와 cookKey에 들어간다. 시험: 결정적 수학의 기준값 표, CRT 미사용 링크 검사, 에셋 너겟 빈/찬 상태 출력 비교.
- 오류: 에셋별 원인·경로를 모아 보고, 하나라도 실패하면 게시하지 않는다.
- 릴리스 콘텐츠 서명: 쿠킹 매니페스트에 서명한 봉투 `CookedManifest.rsig`, 로더 검증, `content-sign` 도구는 [콘텐츠 서명](ContentSigning.ko.md) 설계를 A1 구현 때 함께 넣는다(Release는 매니페스트 항목과 실행 파일 안 데이터만 읽는다).

### 10.1 구현에서 정한 점(A1b — 쿠킹 매니페스트 v1)

10절 매니페스트 v1 서술에서 비어 있던 곳을 다음처럼 정했다. 애매한 곳은 UE 5.8 IoStore(container TOC `FIoStoreTocHeader`·`FIoStoreTocEntryMeta`, container header `FIoContainerHeader`의 패키지별 import 목록·soft 참조 목록)와 asset registry 쿠킹 산출물(PrimaryAsset·청크 표시)을 따랐고, 이름은 Rocks식이다(대응표는 아래 끝).

1. **물리 배치**: 모든 정수는 little-endian, 문자열은 `u64 길이 + UTF-8 byte`(종단 NUL 없음 — Serialization `ArchiveWriter`와 같은 표기), Guid는 high·low u64다. 패딩이 없어 같은 모델이면 같은 byte다(결정성).
   ```
   magic "RMAN"(4) | formatVersion u32 = 1 | entryCount u32
   | profileId string | profileSettingsSha256 (32) | cookSettingsSha256 (32) | cookerVersion u32
   | cookedByEngineVersion: major u32, minor u32, patch u32
   | entry × entryCount
   | crc32 u32 (앞의 모든 byte — 파일 끝과 정확히 맞는다)
   entry = assetId Guid | typeId Guid | pathHint string | packageSha256 (32) | packageBytes u64
         | bulkRawBytes u64 | cookKey (32) | flags u32 | bundleCount u32 | bundle × n
         | dependencyCount u32 | { assetId Guid, kind u32 } × m
   bundle = name string | memberCount u32 | member assetId Guid × k
   ```
   UE TOC처럼 고정 폭 표와 따로 둔 문자열 표로 나누지 않는다 — 항목 수가 패키지 수(수만)라 한 번 읽어 모델로 푸는 비용이 작고, 로더는 모델(정렬된 항목 배열)을 이진 탐색한다. 헤더 크기 칸도 없다(형식이 바뀌면 formatVersion을 올린다).
2. **판정 순서와 진단**: reader는 두 단계다. (1) 구조: 길이(최소 크기 이상·파일 상한 이하) → magic → formatVersion(1이 아니면 `UnsupportedVersion` / `Unsupported` — 더 새 것과 옛 것을 구분하지 않는다, 매니페스트는 같은 쿠커 빌드가 다시 만든다) → 파일 CRC32 → 헤더 칸 읽기(개수 칸 상한·남은 byte, 문자열 길이·UTF-8, 엔진 버전 u16 범위) → 항목마다 칸 읽기(bundle·의존 개수 칸 상한·남은 byte, 문자열 길이·UTF-8) → 끝 위치 = crc 앞. (2) 칸 규칙: 다 읽은 모델을 `ValidateCookedManifest`가 헤더 → 항목(번호 순, 항목 안은 assetId 순서 → typeId → 경로 → 패키지 크기 → flags → bundle → 의존) → 항목 사이(경로 caseless 유일 → Hard 대상 존재) 순서로 본다. 검증·writer는 (2)만 본다. 그래서 위반이 여럿이면 reader와 검증·writer의 진단이 다를 수 있다 — 예: 항목 0의 정의되지 않은 flags와 항목 1의 bundle 개수 칸 상한 초과가 함께 있으면 reader는 (1)에서 항목 1 BundleCount, 검증·writer는 (2)에서 항목 0 Flags다(`CookedManifest_JudgementOrder`의 우선순위 표 — 길이·magic·버전·CRC·남는 byte·항목 사이·병합·패키지 대조의 겹친 위반). 위반이 하나면 세 경로의 진단이 같다. 그 밖의 위반은 모두 `CorruptPackage` / `InvalidArgument`이고 `AssetDiagnostic::section`에 `CookedManifestField`(위반한 칸), `detail`에 항목 번호(헤더 칸이면 `CookedManifestNoEntry`), `asset`에 그 항목 AssetId(읽었으면), `related`에 의존 대상(의존 칸이면)을 둔다. 할당 실패는 진단 None과 그 오류다. CRC를 내용보다 먼저 보므로 우발 손상은 칸 판정 전에 잡힌다(의도적 변조는 서명 봉투 — ContentSigning — 가 막는다).
3. **상한**(이름 있는 상수): 파일 `CookedManifestMaxFileBytes` = 1 GiB, 항목 `CookedManifestMaxEntries` = 2²⁰, 항목당 bundle `CookedManifestMaxBundles` = 256, 항목당 의존 `CookedManifestMaxDependencies` = 65536, 프로파일 id·경로 힌트 길이는 각각 4 KiB(패키지 문자열 상한)·256 byte(4.2절). 개수 칸은 남은 byte로 담을 수 있는 최소 항목 수보다 크면 읽기 전에 거부한다(작은 파일이 큰 할당을 요구하지 못한다).
4. **헤더 칸**: profileId는 비어 있지 않은 UTF-8(패키지 Summary profileId와 같은 규칙), cookedByEngineVersion은 u16 범위(진단용 — 병합·로드 판정에 쓰지 않는다, 10절). 문자열 칸(profileId·경로 힌트·bundle)의 잘못된 UTF-8은 reader의 문자열 읽기가 그 칸의 손상으로 거부한다. 모델의 `String`은 유효한 UTF-8만 담으므로(Core 불변식) 검증 함수는 UTF-8을 다시 보지 않는다.
5. **항목 칸**:
   - assetId: null 아님, 항목 사이 엄격 오름차순(Guid (high, low) 부호 없는 순 — 중복 없음).
   - typeId: null 아님(대표 에셋 export 타입 — 8항 `CheckCookedPackage`).
   - pathHint: 4.2절 논리 경로 — 앞 `/`, 구성 요소 둘 이상(마운트 + 이름), 구성 요소마다 Core `ValidateNameComponent`, 전체 ≤ 256 byte. 경로 힌트는 caseless 키(`TryMakeNameComponentKey`를 구성 요소마다 '/'로 이은 것)로 항목 사이 유일하다(`FindByPath`가 한 항목을 찾는다). 마운트 이름의 허용 목록(`/Game` 등)은 ProjectLayout(A2) 몫이라 보지 않는다.
   - packageBytes: 패키지 헤더 64 byte 이상, 패키지 상한 64 GiB 이하.
   - bulkRawBytes: 패키지 BulkIndex rawBytes 합(10절 항목 표에 더한 칸). 로더(A1c)가 `PackageLoadOptions::maxBulkRawBytes`를 이 값으로 정한다(5.7절 11 — 패키지마다 정확한 해제 상한이라 손상된 rawBytes가 큰 할당을 요구하지 못한다). 8항 `CheckCookedPackage`가 같은지 본다. UE TOC의 청크 압축 해제 크기 표(`FIoOffsetAndLength`)에 대응한다.
   - flags: bit0 = PrimaryAsset(`CookedManifestFlags::PrimaryAsset`), 그 밖의 비트는 0.
   - bundles: PrimaryAsset 항목만 가진다(PrimaryAsset이 아니면 0개 — UE에서 bundle은 PrimaryAsset 데이터다). 이름은 `ValidateNameComponent`, 항목 안에서 byte 엄격 오름차순이고 caseless 키로도 중복이 없다.
   - bundle 구성원(A1d 개정, 2026-11-20 사용자 결정 — UE `FAssetBundleData`의 bundle별 에셋 목록 대응): bundle마다 그 bundle을 로드할 때 함께 로드할 에셋 목록이다(계획 §24.2 "bundle은 의미별 dependency 집합"). 구성원은 AssetId 엄격 오름차순, null·자기 자신 금지, 그 항목의 런타임 의존 대상(Hard·Soft 어느 쪽이든)이어야 하고 매니페스트 항목이어야 한다(항목 사이 규칙 — 없으면 `CorruptPackage` Bundle 칸, related = 구성원). 상한 `CookedManifestMaxBundleMembers` = 65536(의존 상한과 같다). 구성원이 0개인 bundle도 쓸 수 있다(대표 에셋만 로드). Hard 대상 구성원은 어차피 함께 로드되지만 무해하므로 허용한다(쿠커가 필드의 bundle 지정만 보고 모으기 때문). 수집: PrimaryAsset의 Soft 참조 필드에 붙은 bundle 지정자(리플렉션 `AssetBundles="…"` — UE `meta=(AssetBundles="…")` 대응, 계약)를 저장 때 타입 hook Tags로 색인 태그에 남기고 쿠커(A4)가 그 태그를 읽어 모으며(10.2절 결정 9 — UE가 `FAssetBundleData`를 AssetRegistry 태그로 남기는 것과 같다), bundle 구성원은 `CookSoftReferences`와 무관하게 쿠킹한다(UE `ModifyCookReferences`와 같다 — 구성원은 매니페스트 항목이어야 하므로). 구조 판정(2): bundle 하나의 최소 byte는 이름 길이 칸 8 + 구성원 개수 칸 4이고, 구성원 개수는 `CookedManifestMaxBundleMembers` 이하이면서 남은 byte / 16 이하여야 한다(작은 파일이 큰 할당을 요구하지 못하게 — 3과 같은 규칙). 새 진단 칸 `BundleMemberCount`(개수 칸)·`BundleMember`(구성원 — 순서·null·자기 자신·의존 대상 아님·매니페스트에 없음, related = 구성원). 칸 규칙 순서: 항목 안에서는 의존 검증 뒤에 구성원이 의존 대상인지 보고, 매니페스트에 있는지는 항목 사이 규칙(6)에서 본다. 형식이 바뀌었으나 v1은 아직 배포 전이라 formatVersion은 1 그대로이고(하위 호환을 두지 않는다), 14.1.3 대조표와 golden byte를 다시 만든다.
   - dependencies: 런타임 의존(Hard·Soft — kind 값은 패키지 `PackageDependencyKind`의 1·2), (assetId, kind) 엄격 오름차순, assetId null·자기 자신 금지. 같은 대상에 Hard와 Soft가 함께 있을 수 있다(패키지 Dependencies와 같다).
6. **항목 사이 규칙**: Hard 의존 대상은 매니페스트에 있어야 한다(쿠킹은 Hard 전이 폐포다 — 없으면 `CorruptPackage`, related = 대상). Soft 대상은 없어도 된다(`CookSoftReferences`가 꺼져 있으면 쿠킹하지 않는다). 순환(SCC)은 허용한다 — 로드 단위 계산은 로더(A1c)가 이 Hard 간선으로 한다.
7. **writer**(`WriteCookedManifest`): 모델을 6까지 같은 규칙으로 검증한 뒤(위반은 쓰지 않고 `InvalidArgument` + 같은 진단 — reader가 거부할 byte를 쓰지 않는다) 결정적으로 직렬화한다. 출력이 `CookedManifestMaxFileBytes`(CRC 칸 포함 파일 전체)를 넘으면 `CapacityExceeded`(진단 None, out 그대로)다 — 처음 구현은 상한에서 CRC 4 byte를 한 번 더 빼 1 GiB − 4까지만 썼다. 상한을 인자로 받는 내부 함수(`package_detail::WriteCookedManifestWithLimit`)로 경계를 시험한다. 모델은 이미 정렬되어 있어야 한다(writer가 정렬하지 않는다 — 쿠커가 정렬해 넘기고, 순서 위반은 오류로 드러난다). 출력은 메모리 버퍼이고 게시(임시 파일·Flush·Replace)는 쿠커(A4) 몫이다(10절 게시 3).
8. **대조 함수**:
   - `CheckCookedManifestProfile`(로더 시작): 매니페스트 profileId ≠ 실행 프로파일이면 `IncompatibleCook` / `Unsupported`.
   - `CheckCookedManifestMerge`(부분 쿠킹 병합): profileId·profileSettings·cookSettings·cookerVersion이 모두 같아야 하고, 아니면 `IncompatibleCook` / `Unsupported`(section = 다른 칸 — 처음 다른 칸 하나).
   - `CheckCookedPackage`(로더가 패키지를 연 뒤 — A1c가 파일 쪽 `CheckCookedPackageFile`과 내용 쪽 `CheckCookedPackageContents`로 나눴고 `CheckCookedPackage`는 둘을 차례로 부른다, 11.6절 3): 파일 길이 = packageBytes, SHA-256 = packageSha256, 그다음 `ReadPackage` 결과와 — 헤더 종류 쿠킹, assetId, 대표 에셋 타입(export 1) = typeId, BulkIndex rawBytes 합 = bulkRawBytes(합이 u64를 넘으면 — 호출자가 손상된 내용을 넘긴 경우 — 2⁶⁴로 감은 값이 같아도 불일치), Summary profileId·profileSettings·cookerVersion = 매니페스트 헤더, 패키지 Dependencies(Hard·Soft, (asset, kind) 순서) = 매니페스트 의존 배열(같은 순서로 하나씩 — 쿠킹 패키지에는 다른 종류가 없다). 다르면 `CorruptPackage` / `InvalidArgument`, section = `CookedManifestField`의 그 칸, detail = `CookedManifestNoEntry`, asset = 그 에셋, 의존이면 related = 처음 다른 자리의 의존 대상(매니페스트 쪽이 있으면 그것, 없으면 패키지 쪽). 크기·SHA-256은 `ReadPackage` 전에 본다(바뀐 파일을 해석하지 않는다).
   - `AppendCookedPackagePath`: 4.2절 파일 이름 `Packages/<a>/<A>-<h>.rpkg`(쿠킹 루트 기준)을 만든다.
9. **로더 API 염두**: 매니페스트 모델은 AssetId 오름차순 배열이고 `Find(AssetId)`(이진 탐색)를 둔다. 경로 힌트 조회 색인(`FindByPath`)과 Hard SCC 계산은 그것을 쓰는 로더·AssetManager(A1c)가 만든다(여기서는 그 입력 — 경로 힌트 caseless 유일성, Hard 대상 존재 — 만 보장한다). 서명 봉투(ContentSigning CS1)는 같은 버퍼를 검증한 뒤 이 reader에 그 매니페스트 byte span을 넘긴다(reader는 파일을 열지 않는다).

| Rocks | UE 5.8 |
|---|---|
| `CookedManifest`(rman) | IoStore container TOC(`.utoc`) + container header(`FIoContainerHeader`) + 쿠킹 asset registry |
| entry assetId·packageSha256·packageBytes | `FIoChunkId`·`FIoStoreTocEntryMeta::ChunkHash`·`FIoOffsetAndLength` |
| bulkRawBytes | TOC 압축 블록 표가 주는 청크 해제 크기 |
| dependencies Hard·Soft | `FFilePackageStoreEntry::ImportedPackages`·`FIoContainerHeaderSoftPackageReferences` |
| primaryAsset·bundles | asset registry의 PrimaryAssetId·`FAssetBundleData` |
| 파일 CRC32 | (TOC는 서명·해시로) — 서명은 Rocks `CookedManifest.rsig` |

### 10.2 A4 구현 세부(2026-11-23 — 권장안으로 확정, 교차 검증 1회: 필수 4·권장 7 반영)

10절을 구체화한다. 애매한 곳은 UE 5.8 Cook(`UCookOnTheFlyServer`·`FPackageData`·`UAssetManager` PrimaryAsset 규칙·`FAssetBundleData`·증분 쿠킹의 TargetDomain 키)을 따르고 이름은 Rocks식이다.

1. **모듈** `Engine/Source/Developer/AssetCooker`(Developer — 에디터·도구만). 의존 `AssetDatabase`(색인·EditorPackageSet)·`AssetRuntime`(WritePackage·CookedManifest)·`AssetNugget`(키 writer·SCC 키·NuggetCache)·`ReflectionText`(설정 codec)·`ObjectRuntime`. 명령은 `AssetCommandlets`의 `asset-cook`.
2. **TargetProfile**(리플렉션 struct): `id`(String — 파일 이름과 같아야 함), `graphicsBackend`(String — A4는 기록만), `bulkCodec`(enum None·Zstd, 기본 None — 10절), `bulkAlignment`(u32, 0이면 쿠킹 기본 4096). 텍스처 칸(형식 지원 표·LOD 그룹 — LOD 그룹 Map은 `MergeByKey`)은 A5가 더한다. 엔진 파일 `Engine/Config/TargetProfiles/<id>.json`(엔진 기본 `Windows`·`Android`) 위에 프로젝트 `Config/TargetProfiles/<id>.json`을 ReflectionText `ImportJson`으로 덮는다 — 의미는 codec 기본: struct는 재귀 멤버 병합, 배열은 통째 교체(10절의 "그룹 단위"를 이것으로 확정). 엔진 파일이 없는 id는 `InvalidSetting`(엔진 플랫폼 목록이 기준 — 프로젝트만의 프로파일은 받지 않는다). **프로파일 출력 설정 SHA-256** = 병합한 TargetProfile을 `NuggetKeyWriter`의 `WriteReflected`로 흘린 SHA-256(필드 이름·타입·값 — 칸을 더하면 바뀐다).
3. **CookSettings**(리플렉션 struct, `Config/CookSettings.json`, 없으면 기본값): `alwaysCook`·`neverCook`(Array<String> — 논리 경로 `/Game/A/B` 또는 디렉터리 `/Game/A/`(끝 `/`)), `primaryAssets`(Array<`PrimaryAssetRule{type: 클래스 도구 이름, directories: Array<String>}`>), `cookSoftReferences`(bool, 기본 false). 경로는 LogicalPath 규칙으로 검증(위반 `InvalidSetting`). **CookSettings SHA-256**도 같은 방식. bundle은 규칙이 아니라 필드 지정자에서 온다(결정 9 — 10절 문장을 고친다).
4. **대상 계산**(색인만 — 패키지를 열지 않는다, UE가 AssetRegistry로 쿠킹 대상을 세는 것과 같다):
   - 뿌리: 전체 쿠킹은 alwaysCook이 가리키는 에셋(디렉터리는 재귀) ∪ PrimaryAsset(규칙 타입 또는 그 파생 타입이며 규칙 디렉터리 아래). 부분 쿠킹은 인자 경로(에셋·디렉터리).
   - 폐포: Hard 의존(+ cookSoftReferences면 Soft) + bundle 구성원(결정 9 — 색인 태그, cookSoftReferences와 무관). EditorOnly·Build·ImportOwner 간선은 따라가지 않는다.
   - NeverCook: 뿌리면 뺀다. alwaysCook과 NeverCook에 함께 걸리면 NeverCook이 이기고 경고 `NeverCookReferenced`(UE와 같은 우선순위). 다른 대상이 Hard로 참조하면 `NeverCookReferenced`(오류), Soft로 참조하면 진단 없이 빠지고 매니페스트 의존 목록에서도 뺀다(로드가 NotFound).
   - EditorOnly 에셋(대표 타입이 EditorOnly 클래스)은 대상이 아니다. Hard로 참조하면 `NeverCookReferenced`와 같은 오류 규칙.
   - 색인에 없는 Hard 의존은 `MissingDependency`(쿠킹 실패), Soft는 경고 `MissingDependency`(의존 목록에서 뺀다).
   - 부분 쿠킹: 위에 역방향 Build 폐포(대상을 Build로 의존하는 에셋 — 색인 Referencers(Build), 전이)와 기존 매니페스트 항목 중 다시 쿠킹할 것(결정 8)을 더한다. 에디터에서 사라진 에셋의 매니페스트 항목은 빼고, 남은 항목이 그것을 Hard로 참조하면 `MissingDependency`.
5. **키 계산**(패키지를 열지 않는다 — UE 증분 쿠킹 TargetDomain 키처럼 로드 없이 판정):
   - **콘텐츠 해시** = 에디터 패키지 파일 전체 SHA-256(패키지 지문은 섹션 CRC32 표의 SHA라 충돌이 CRC 수준이어서 쓰지 않는다). 색인이 지연 계산해 항목과 색인 캐시에 둔다(`AssetIndex::ContentHash(id)` — 크기·수정 시각이 같으면 캐시 값, 다르면 다시 읽음 — 바뀐 파일만 다시 해시, 색인 캐시 형식 버전을 올린다).
   - **처리기 build key** = 9절 너겟 키: 공통 머리(`WriteNuggetKeyHeader(writer, {처리기 id, 처리기 버전}, projectGeneration)`) + 처리기 설정 키(색인 정보만으로 — 기본 처리기는 없음, 설정이 패키지 안에 있으면 콘텐츠 해시가 이미 담는다) + Build 의존 튜플 (역할, AssetId, 그 의존의 build key). Build 그래프에 순환이 있을 수 있으므로 **Build SCC**로 계산한다(`NuggetSccKey` — 구성원 지역 키 = 공통 머리 + 설정 + 콘텐츠 해시, 외부 튜플 = Build 의존의 구성원 key; build key = `NuggetMemberKey`).
   - **cookKey 지역 입력** = 공통 머리(`{CookerProcessorGuid, cookerVersion}`) + 콘텐츠 해시 + 논리 경로 키 + CookSettings 판정(PrimaryAsset 여부·bundle 이름과 구성원) + 처리기 build key + 쿠킹 패키지 formatVersion·섹션 schemaVersion 집합 + **타입 표 지문** + 프로파일 출력 설정 SHA-256. 클래스 기본값 지문 열은 A5가 더한다.
   - **타입 표 지문** = 등록된 리플렉션 타입 전체의 (TypeId, 스키마 버전) 정렬 목록 SHA-256이다. Summary 타입 표는 export 클래스만 담아 필드 안 struct·enum·InstancedStruct 내용 타입의 스키마 변경을 놓치고, InstancedStruct 내용 타입은 실행 중에 정해져 스키마로 폐포를 셀 수 없다 — 그래서 보수적으로 전체 표를 쓴다(어느 타입의 버전이 바뀌어도 전체가 다시 쿠킹된다 — 안전 쪽, 범위를 줄이는 것은 저장된 타입 참조를 색인에 두는 후속).
   - **cookKey** = Hard 그래프 SCC(대상 사이, Tarjan — AssetId 순 결정적): SCC key = `NuggetSccKey(구성원 {AssetId, 지역 키}, 외부 튜플 {구성원, "Hard", 의존, 의존 cookKey})`, cookKey = `NuggetMemberKey(SCC key, AssetId)`. 순서: Build SCC(build key) → Hard SCC 역위상(cookKey).
6. **CookProcessor 표**: TypeId → `CookProcessor{정체성 NuggetProcessorId(처리기 Guid·버전 Guid — 너겟 키 공통 머리와 같다), WriteSettingsKey(writer, 색인 항목)(선택 — 색인 정보만), Cook(context)}`. 대표 에셋 타입에서 상속 사슬을 따라 가장 가까운 등록 처리기 하나(AssetTypeHookTable과 같은 규칙). 기본 처리기(Asset 루트): 에디터 객체 그래프를 그대로 `WritePackage(kind=Cooked, profile 값, bulkCodec·bulkAlignment, bulkSource=에디터 패키지 store)`로 쓴다(EditorOnly 필드는 writer가 뺀다). 처리기는 `CookContext`로 NuggetCache·프로파일·진단 sink를 받는다(텍스처 처리기 A5가 너겟을 쓴다).
7. **쿠킹 실행**: 대상을 Hard SCC 역위상(의존 먼저, 같은 깊이는 AssetId 순)으로 처리한다. 열린 패키지는 "남은 참조자 수"로 관리해 그것을 Hard로 의존하는 대상이 모두 쿠킹되면 닫는다(공통 의존을 대상마다 다시 열지 않는다 — 열기 횟수 ≤ 대상 수 + 의존 수, 시험이 카운터로 본다). 메모리 상한(`CookOpenPackageBytes`)을 넘으면 참조자가 남은 패키지도 닫고 나중에 다시 연다. 처리기는 메모리 sink에 쓰고 패키지 SHA-256·크기·bulk raw 합을 얻는다. 오류는 에셋별로 모아 끝까지 진행하고(보고서), 하나라도 있으면 게시하지 않는다(10절). 병렬은 후속(결정성은 정렬된 순회로 지킨다). 에디터 프로세스 안 쿠킹(서비스 API)은 dirty 패키지가 열려 있으면 `Busy`로 거부한다(쿠킹은 디스크 상태 — Commandlet에는 열린 패키지가 없다). 쿠킹은 에셋 DB 잠금을 쥔다.
8. **부분 쿠킹 재사용**: 기존 매니페스트 항목은 cookKey가 같고 `Packages/`의 그 파일이 있으며 크기·SHA-256이 매니페스트와 같을 때만 재사용한다. 파일이 없거나 다르면 다시 쿠킹한다(사용자가 쿠킹 디렉터리를 일부 지운 경우 — 시나리오 27 손상된 게시 파일의 재게시). 재사용하는 것은 **패키지 byte와 그 byte에서 나오는 칸**(SHA-256·크기·bulk raw 합·대표 타입)뿐이다. 매니페스트 항목의 byte 밖 칸(의존 목록·PrimaryAsset flags·bundle·경로 힌트)은 재사용 항목도 이번 판정(A4b 대상·제외 목록·색인)으로 늘 다시 만든다(A4c1 리뷰 필수 — 예: Soft 의존이 색인에 새로 생기면 의존 목록이 바뀌어야 하는데, cookKey는 byte 입력의 키라 그 판정을 다 담지 않는다. 판정 입력이 늘어도 키를 같이 늘릴 필요가 없다).
9. **bundle**: 리플렉션 지정자 `AssetBundles="a,b"`(Soft 참조 필드 — UE `meta=(AssetBundles=…)`)가 있는 PrimaryAsset은 **저장 때** 타입 hook Tags(A2c)로 bundle 구성원을 색인 태그에 남긴다(UE가 `FAssetBundleData`를 AssetRegistry 태그로 남기는 것과 같다). 쿠커는 태그만 읽는다(2차 열기 없음). MetaCompiler 지정자와 필드 메타는 리플렉션 쪽 요구다(ReflectionExtensions 문서에 적고 리플렉션 담당과 조정한다 — A4e).
10. **게시**(10절 1–3 그대로): `<Project>/Saved/Cooked/<Profile>/.staging/<guid>.tmp` 기록·Flush → `Packages/<a>/<이름>`에 `Move(no-replace)` → 있으면 크기·SHA-256 대조(같으면 임시 삭제, 다르면 `CorruptEntry` 경고 + `Replace`) → 바뀐 디렉터리 안쪽부터 `SyncDirectory` → 매니페스트를 `.staging`에 쓰고 Flush → `Replace`. 전체 쿠킹은 그 뒤 새 매니페스트가 가리키지 않는 `Packages/` 파일을 지운다(못 지우면 경고). 부분 쿠킹은 기존 매니페스트와 병합(`CheckCookedManifestMerge` — 다르면 `IncompatibleCook`)하고 파일은 지우지 않는다. 기존 매니페스트가 없으면 부분 쿠킹은 전체 쿠킹과 같다.
11. **보고서**: `Saved/Cooked/<Profile>/CookReport.json`(결정적 — 진단을 (assetId, 코드) 순), 도구 빌드 ID·엔진 버전(진단용 — 10절), 대상 수·재사용 수·쓴 byte. 명령은 사람이 읽는 줄도 낸다.
12. **명령** `asset-cook --profile=<id> [<path>...] [--verify-nuggets[=local]] [--json]`: 경로가 없으면 전체 쿠킹. `--verify-nuggets`는 처리기가 너겟 적중을 쓰지 않고 다시 만들어 적중 결과와 byte를 대조한다(다르면 `NuggetMismatch` 오류 — A4e가 진단 코드를 등록, 9.1 결정 16). A4의 기본 처리기는 너겟을 쓰지 않아 verify는 늘 같다 — 실질 시험은 A5 텍스처 처리기 때다.

하위 단계(각각 14절 완료 조건 넷):

| 단계 | 범위 | 시험 |
|---|---|---|
| A4a | 모듈·TargetProfile·CookSettings(읽기·병합·검증·SHA-256)·엔진 기본 프로파일 파일 | 병합·거부·SHA 안정·칸 변경 시 SHA 변경 |
| A4b | 대상 계산(전체·부분·NeverCook·AlwaysCook∩NeverCook·Missing·Soft 설정·역방향 Build·사라진 항목·bundle 태그 읽기)·색인 콘텐츠 해시(지연·캐시) | 그래프 경우마다 대상·진단, 해시 캐시 무효화 |
| A4c1 | 기반 둘(TypeRegistry 타입 목록·Core SCC)·CookProcessor 표·키(build key Build SCC·cookKey Hard SCC·타입 표 지문) | 순환 키(시나리오 28)·Build 원본만 바꾸면 배열 cookKey 바뀜·입력마다 키 바뀜·결정성 |
| A4c2 | 기본 처리기·쿠킹 실행(출력 sink, 열기 관리) | EditorOnly 필드 부재(11)·두 번 같은 byte(13)·원본 없이 쿠킹(2)·열기 횟수·에셋별 오류 계속 |
| A4d | 게시·전체 정리·부분 병합·재사용 조건·재사용 항목의 byte 밖 칸 재계산(결정 8)·IncompatibleCook·CorruptEntry·실패 시 게시본 불변·보고서 | 시나리오 13·22·27·재사용 대상의 Soft 의존이 색인에 새로 생기면 매니페스트 의존이 바뀜 |
| A4e | `asset-cook` 명령·`--verify-nuggets`(NuggetMismatch 등록)·리플렉션 요구 둘(필드 지정자 `AssetBundles`·클래스 표지 `EditorOnly` — 리플렉션 담당과 조정)·저장 때 bundle 태그·EditorOnly 에셋 규칙(결정 4) | 명령 출력·verify(A4는 늘 같음)·bundle 구성원·EditorOnly 에셋 |

- A4b 구현 세부(2026-11-23 — 권장안으로 확정, 결정 4·5의 콘텐츠 해시를 구체화):
  - EditorOnly 에셋 규칙(결정 4)은 A4e로 옮긴다 — 클래스 표지 `EditorOnly`가 리플렉션에 아직 없다(`TypeFlags`에 없음). 필드 지정자 `AssetBundles`와 같은 리플렉션 요구라 함께 다룬다.
  - **콘텐츠 해시**(AssetDatabase): `AssetIndexEntry`에 `contentHash`·`contentHashBytes`·`contentHashTicks`·`hasContentHash`를 둔다. `AssetIndex::ContentHash(files, id)`는 Content 파일을 Stat해 크기·수정 시각이 저장된 값과 같으면 저장된 해시를, 아니면 64 KiB씩 흘려 읽어 SHA-256을 계산해 그 Stat 값과 함께 둔다(바뀐 파일만 다시 해시 — 권장 1). 읽기 전후 Stat이 다르면 `Busy`(해시하는 동안 바뀜 — 쿠킹은 그 에셋 오류). 색인 캐시가 네 칸을 보존한다(캐시 schemaVersion 3 — 캐시 항목은 지문으로 재사용되므로 해시는 Stat 대조로만 믿는다). 오류: 없음 NotFound, 같은 id 둘 이상 AlreadyExists, 파일 IO 오류, 할당 실패.
  - **API**(AssetCooker `CookTargets.h`): `ComputeCookTargets(request, out)`. 요청 = 색인·TypeRegistry·CookSettings·부분 쿠킹 경로 목록(비면 전체 쿠킹)·기존 매니페스트(부분, 없으면 null). 결과 = 대상(AssetId 순 — 각각 PrimaryAsset 여부·bundle 목록), 제외 목록(Soft로만 닿은 NeverCook 에셋 — 매니페스트 의존에서 뺄 것), 사라진 항목(기존 매니페스트에 있고 색인에 없음), 진단 목록. 진단은 `CookDiagnostic{code, warning, asset, related, setting}`다 — 같은 코드가 경고·오류로 쓰인다(NeverCookReferenced·MissingDependency·InvalidPath)라 `AssetDiagnostic`(코드별 고정 수준)이 아닌 따로 둔 표지다. 함수 오류는 색인 조회 오류·할당 실패뿐이고 규칙 위반은 진단이다(쿠킹은 오류 진단이 하나라도 있으면 게시하지 않는다 — 10절).
  - 경로 판정은 비교 키로 한다: 에셋 경로는 키 일치, 디렉터리는 키 접두(`<dir key>/`, 마운트 디렉터리는 모든 항목).
  - 규칙(판정 순서):
    1. 뿌리: 전체 쿠킹 = alwaysCook(에셋은 FindByPath, 디렉터리는 접두) ∪ PrimaryAsset 규칙(ListByType(type, types — 파생 포함) 중 디렉터리 하나 아래). 부분 쿠킹 = 인자 경로(같은 판정). 아무것도 가리키지 않는 alwaysCook 항목은 경고 `InvalidPath`(setting = 순번), 인자 경로는 오류 `InvalidPath`.
    2. NeverCook 뿌리는 뺀다. alwaysCook·인자 경로가 직접 가리킨 에셋이면 경고 `NeverCookReferenced`(UE와 같은 우선순위), PrimaryAsset 규칙으로만 닿았으면 진단 없이 뺀다.
    3. 폐포: 대상마다 색인 의존을 본다 — Hard는 늘, Soft는 cookSoftReferences일 때, PrimaryAsset이면 bundle 구성원(태그 `Bundle.<이름>` = 구성원 AssetId 표준 문자열, 구성원마다 태그 하나 — A4e가 쓴다)도 Hard와 같은 규칙. 의존이 색인에 없으면 Hard·bundle은 오류·Soft는 경고 `MissingDependency`(asset = 참조자, related = 의존), 같은 id가 둘 이상이면 오류 `DuplicateAssetId`, NeverCook이면 Hard·bundle은 오류 `NeverCookReferenced`·Soft는 제외 목록. bundle 태그 값이 AssetId가 아니면 오류 `CorruptPackage`(asset = PrimaryAsset). EditorOnly·Build·ImportOwner 간선은 따라가지 않는다.
    4. 부분 쿠킹: 인자 뿌리와 역방향으로 더한 에셋을 Build로 의존하는 에셋(색인 Referencers(Build))을 고정점까지 더한다(NeverCook은 진단 없이 빼고, 더한 에셋도 3의 폐포를 따른다). Hard·Soft·bundle 폐포로만 들어온 에셋에는 적용하지 않는다 — 10절의 의도는 인자 쪽 편집(원본 Texture2D를 고치면 그것을 묶는 Texture2DArray도)이고, 폐포 에셋까지 넓히면 많이 쓰이는 원본 하나가 그것을 묶는 에셋 전부를 다시 쿠킹한다(리뷰 권장, 2026-11-23). 폐포로 먼저 들어온 에셋도 뒤에 역방향으로 닿으면 넓힌다. 기존 매니페스트 항목 중 색인에 없는 것은 사라진 항목이고, 이번에 다시 쿠킹하지 않는 남은 항목이 그것을 Hard로 의존하면 오류 `MissingDependency`(asset = 남은 항목). 다시 쿠킹하는 항목은 옛 매니페스트 의존이 아니라 색인 의존으로 3에서 이미 판정했다(리뷰 필수 — 지운 참조를 옛 의존으로 오류 내지 않는다). 기존 항목 중 다시 쿠킹할 것(결정 8)은 키가 필요하므로 A4c·A4d가 정한다.
    5. PrimaryAsset 여부는 규칙에 맞는지로 정한다(폐포로 들어와도 규칙에 맞으면 PrimaryAsset). bundle은 PrimaryAsset에만 있다(이름 순, 구성원 AssetId 순).
  - 결정성: 대상·제외·사라진 항목은 AssetId(high, low) 순, 진단은 (asset, code, related, setting, 오류 먼저) 순이다(같은 의존의 Hard·Soft 누락은 칸이 같고 수준만 다르다).
- A4c 구현 세부(2026-11-23 — 권장안으로 확정, 결정 5–7을 구체화. A4c1·A4c2로 나눈다):
  - **기반 보강 둘**(우회하지 않는다):
    1. `TypeRegistry::ListTypeVersions(out)`(ObjectRuntime): 등록된 모든 타입의 (TypeId, 종류, 스키마 버전)을 TypeId 순으로 채운다. 등록·해제가 소유 스레드 전용이므로 이 함수도 소유 스레드 전용이다(다른 스레드는 WrongThread). 그래서 개수를 세고, 잠금 밖에서 예약한 뒤 복사하는 사이에 목록이 바뀌지 않는다(할당기 콜백을 잠금 안에서 부르지 않는 규칙). 종료 뒤에는 ShuttingDown이다. enum은 스키마 버전이 없어 0이다 — 저장 wire가 I64 값이라 enum 스키마를 바꿔도 쿠킹 byte가 바뀌지 않는다.
    2. Core `Core/Graph/StrongComponents.h`의 `algo::ForEachStrongComponent`: 반복 Tarjan이고 CSR 그래프를 받는다. SparksCompiler 비공개 구현을 공용으로 올린 것이고 알고리즘은 같다. 의존을 먼저 알린다. 기존 사본 셋(SparksCompiler·AssetLoader·UnrealExchange)은 각 세션에 알려 공용으로 바꾸게 한다(그 파일은 이 단계에서 고치지 않는다).
  - **처리기 정체성**: 버전은 Guid다(`NuggetProcessorId` — 너겟 키 공통 머리와 같다). 결정 6의 `version u32`를 이것으로 고친다. 출력 의미가 바뀌면 버전 Guid를 바꾼다.
  - **CookProcessor**(AssetCooker `CookProcessor.h`) 인터페이스: `Identity()`(NuggetProcessorId), `WriteSettingsKey(writer, 색인 항목)`(기본은 아무것도 쓰지 않음 — 색인 정보만 쓴다), `Cook(context)`(A4c2).
  - **CookProcessorTable**: 생성 인자는 TypeRegistry와 기본 처리기다. `Register(type, processor)`는 0 type이면 InvalidArgument, 같은 type이면 AlreadyExists다. 처리기는 빌림이라 표보다 오래 살아야 한다. `Find(type)`는 type부터 base 사슬로 올라가 가장 가까운 등록 처리기를 주고, 없으면 기본 처리기를 준다(AssetTypeHookTable과 같은 규칙). 타입 조회 오류는 그대로 돌려준다.
  - **키 API**(AssetCooker `CookKeys.h`): `ComputeCookKeys(request, out)`.
    - 요청: 색인(비const — ContentHash가 해시를 둔다), 파일 시스템, TypeRegistry, 처리기 표, A4b 결과(`CookTargetSet`), 프로파일 출력 설정 SHA-256, 프로젝트 세대 Guid, 쿠커 버전(u32 — 매니페스트 `cookerVersion`과 같다).
    - 결과: 대상마다(대상 순) {asset, contentHash, buildKey, cookKey, hasKey}와 진단이다. 함수 오류는 TypeRegistry 오류와 할당 실패뿐이고, 에셋 문제는 진단이다.
  - `CookDiagnostic`에 `error`(ErrorCode, 기본 None) 칸을 더한다. 콘텐츠 해시 실패는 진단 코드가 MissingFile(파일 없음)이거나 None이고, 그 오류를 이 칸에 둔다.
  - **Build 키**(결정 5 그대로 — 9절 너겟 키):
    - 영역 = 대상 ∪ 대상에서 Build 간선으로 닿는 에셋(전이). Build 의존이 색인에 없으면 오류 MissingDependency, 같은 id면 오류 DuplicateAssetId다(asset = 참조자, related = 의존).
    - 지역 키 = 공통 머리(처리기 정체성, 프로젝트 세대) + 처리기 설정 키 + 콘텐츠 해시.
    - Build SCC(AssetId 순 노드·간선 — 결정적)마다 `NuggetSccKey(구성원, 외부 튜플 {구성원, "Build", 의존, 의존 build key})`를 구하고, build key = `NuggetMemberKey`.
  - **cookKey**:
    - 지역 키 = 공통 머리({`CookerProcessorGuid`, `CookerKeyVersion`}, 프로젝트 세대) + 쿠커 버전 + 콘텐츠 해시 + 논리 경로 키 + PrimaryAsset 여부 + bundle(이름·구성원) + 이 대상의 제외된 Soft 의존(AssetId 순) + build key + `PackageFormatVersion` + `PackageSectionSchemaVersion` + 타입 표 지문 + 프로파일 출력 설정 SHA-256.
    - CookSettings SHA-256 전체는 넣지 않는다 — 판정 결과만 넣는다. 그래서 alwaysCook을 더해도 기존 대상의 키는 바뀌지 않는다. 제외된 Soft 의존은 매니페스트 의존 목록을 바꾸므로 넣는다.
    - Hard SCC는 대상 사이 Hard 간선으로 만든다(대상 밖 Hard 의존은 A4b가 이미 오류로 보고했다 — 간선에서 뺀다). 외부 튜플은 {구성원, "Hard", 의존, 의존 cookKey}다.
  - **타입 표 지문** = `ListTypeVersions` 목록(개수, 그리고 칸마다 TypeId·종류·버전)을 NuggetKeyWriter로 흘린 SHA-256이다.
  - **키 실패 전파**: 콘텐츠 해시를 못 구했거나 Build 의존이 없는 에셋은 키가 없다(hasKey 거짓). 그 에셋에 Build·Hard로 기대는 에셋도 키가 없고, 진단은 원인에 하나만 남긴다. 쿠킹은 오류 진단이 있으면 게시하지 않는다.
  - **실행**(A4c2, `CookPackages`):
    - 순서는 Hard SCC 역위상이다(의존 먼저, SCC 안은 AssetId 순 — Tarjan 완료 순이 결정적이다).
    - 패키지 열기는 호출자의 EditorPackageSet으로 한다. 시작 때 dirty 패키지가 있으면 Busy다.
    - 열린 집합 관리(결정 7을 고친다): 대상을 열고 쿠킹한다(이미 열린 의존은 그대로 잇는다). 이번 쿠킹이 연 패키지 수가 상한 `CookMaxOpenPackages`(요청 칸, 기본 4096)를 넘으면 시작 때 열린 것만 남기고 모두 닫는다(`CloseOpened(before)`). 끝에도 닫는다.
    - 상한 판정을 byte 대신 패키지 수로 하는 근거: 결정적이고 O(1)이다. UE도 쿠킹 중 메모리 압박에서 GC로 한꺼번에 내린다. 참조자 수로 하나씩 닫는 방식은 EditorOnly 의존 묶음과 닫기 순서 규칙(참조되는 패키지는 Busy) 때문에 버린다. 열기 횟수 ≤ 대상 수 + 의존 수(상한을 넘지 않을 때)이고, 시험이 파일 열기 계수로 본다.
    - 처리기는 `CookContext`(Registry·접근 문맥·패키지 handle·색인 항목·프로파일·출력 sink·allocator, 선택 NuggetCache)를 받는다.
  - 매니페스트 항목은 재사용 여부와 상관없이 byte 밖 칸을 이번 판정으로 만든다(결정 8) — 의존 목록의 계산(아래)은 재사용 항목에도 같다(재사용 항목은 기존 `Packages/` 파일을 읽어 의존을 얻는다).
    - 기본 처리기는 `WritePackage(kind=Cooked, profileId, profileSettings, cookerVersion, bulkCodec, bulkAlignment(0이면 4096), bulkSource=열린 패키지 store)`다.
    - 결과(`CookedPackage`)는 쓴 byte·SHA-256·크기·bulk raw 합·대표 타입·경로 힌트·flags·bundle·의존이다. 쓴 byte를 다시 읽어 검증하고, 의존 = 쿠킹 패키지의 Hard·Soft에서 제외 목록과 색인에 없는 Soft를 뺀 것이다.
    - 결과는 출력 sink(`CookOutputSink::Put`)로 하나씩 넘긴다(A4d가 바로 staging에 쓴다 — 전체를 메모리에 모으지 않는다). 에셋별 오류(열기·처리기·검증)는 진단으로 모으고 끝까지 진행한다.
- A4c1 상태: **구현** — 대조표 14.1.65절, coverage 14.1.66절. 구현에서 정한 것:
  - `GuidLess`를 AssetRuntime `PackageReader.h`에서 Base `Guid.h`로 옮겼다. ObjectRuntime(ListTypeVersions 정렬)도 쓰므로 공용 자리에 둔다. 이름이 같아 기존 사용처는 그대로다.
  - 진단 순서는 `CookTargets.h`의 `CookDiagnosticBefore`((asset, code, related, setting, 오류 먼저, 원인 오류)) 하나다. 대상 계산·키·이후 보고서가 같은 순서를 쓴다.
  - 키 계산의 영역 표(노드·실패 표지·id 표)는 색인 항목 수만큼 미리 잡는다(영역 ⊆ 색인의 고유 항목). 그래서 노드 넣기는 할당하지 않는다. 진단 push의 할당 실패는 A4b처럼 기록해 두고 끝에서 돌려준다.
  - cookKey 지역 입력 기록은 실패하지 않는다(문자열은 LogicalPath·PackageTags가 검증한 UTF-8이고, 해시 기록기는 할당하지 않는다). 불변식으로 단언한다. 처리기 설정 키의 writer 오류는 처리기 계약 위반이라 함수 오류로 돌려준다.
  - 대상이 아닌 영역 노드로 가는 Hard 간선(A4b가 이미 오류로 보고한 NeverCook 참조 등)은 Hard 그래프에서 뺀다. 그런 대상의 키는 그 간선 없이 정해진다(어차피 게시하지 않는다).
  - 키 진단은 모두 오류다(`CookKeySet::HasErrors` = 진단 있음).
  - 대표 타입의 처리기 조회 실패는 7.1절 8의 진단 모델을 따른다: 이 프로세스에 등록되지 않은 타입(NotFound)은 `UnsupportedVersion`, 등록됐지만 클래스가 아닌 종류(TypeMismatch)는 `TypeMismatch`다(error 칸에 조회 오류 — 리뷰 권장).
  - 타입 표 지문은 프로세스가 로드한 모듈 구성에 달린다. 에디터 안 쿠킹과 Commandlet 쿠킹처럼 로드한 모듈이 다르면 지문이 달라 서로의 매니페스트를 재사용하지 못한다(안전 쪽 — 적중만 잃는다). 범위를 줄이는 후속은 저장된 타입 참조를 색인에 두는 것이다(결정 5).
  - cookKey의 판정 입력(PrimaryAsset·bundle·제외 Soft)은 byte에 영향을 주지 않지만 결정 5대로 보수적으로 넣는다. 매니페스트의 판정 칸은 결정 8대로 매번 다시 만든다.
  - SCC 묶음 표지는 Build·Hard 두 단계에서 번호를 이어 쓴다(다시 비우지 않는다).
  - 리뷰 반영(2026-11-23, 필수 하나·권장 셋): 재사용은 패키지 byte만이고 매니페스트의 byte 밖 칸은 매번 다시 만든다(결정 8 — 시험은 A4d), 미등록 타입 진단을 UnsupportedVersion으로, 타입 표 지문의 모듈 구성 의존 기록, 결정 6 본문의 처리기 버전을 Guid로.
- A4b 상태: **구현** — 대조표 14.1.63절, coverage 14.1.64절. 구현에서 정한 것:
  - 콘텐츠 해시는 `AssetIndexContentHash.cpp`(조회는 `FindById`를 그대로 써서 NotFound·AlreadyExists가 같다). 색인 캐시 schemaVersion 3은 readOnly 뒤에 표지(u8, 0·1 밖은 캐시 없음)·digest·크기(u64)·시각(u64)을 둔다.
  - 뿌리 진단을 더 정했다: 에셋 경로가 같은 경로 키 파일 둘 이상을 가리키면 오류 `DuplicatePath`(asset null, setting = 순번 — 어느 것도 뿌리로 고르지 않는다), 같은 id 파일은 alwaysCook·인자 경로·디렉터리 목록·PrimaryAsset 규칙 어디서 닿아도 오류 `DuplicateAssetId`(asset = 그 id), 역방향 Build 참조자가 같은 id면 오류 `DuplicateAssetId`(asset = 대상, related = 참조자 id). 디렉터리가 가리킨 NeverCook 항목도 "직접 가리킴"이라 경고다.
  - 대상 큐는 색인 항목 포인터다(고유 항목만 들어온다 — 의존·태그를 다시 찾지 않는다). 진단·대상·제외 목록의 할당 실패는 기록해 두고 계속한 뒤 끝에서 OutOfMemory로 돌려준다(호출마다 분기하지 않는다). bundle 구성원은 태그 순서((키, 값) 정렬·중복 없음)를 써서 마지막 bundle에 붙이고 AssetId 순으로 정렬한다.
  - 비교는 `GuidLess`(AssetRuntime)를 쓴다(따로 만든 비교 함수 없음).
  - PrimaryAsset 타입 조회: 없는 타입(NotFound)만 설정 위반 InvalidArgument이고, 그 밖(종료 뒤 ShuttingDown·lease 포화 Overflow)은 TypeRegistry 오류 그대로다.
  - 리뷰 반영(2026-11-23, 필수 하나·권장 넷): 다시 쿠킹하는 기존 항목의 옛 의존 판정 제거(규칙 4), 역방향 Build 범위를 인자 뿌리·역방향 에셋으로 한정(규칙 4), 진단 정렬에 수준, TypeRegistry 오류 보존, 아래 Stat 한계 기록.
  - 콘텐츠 해시의 한계: 크기·수정 시각이 같으면 저장된 해시를 믿으므로, 같은 크기로 다시 쓰면서 수정 시각을 보존한 파일(`cp -p`·`rsync -t`·압축 해제·거친 시각 해상도의 파일 시스템)은 낡은 해시가 남는다. 플랫폼 층 `FileInfo`에 변경 시각(ctime)·파일 id가 아직 없어 더 대조하지 못한다 — 생기면 대조에 더한다. 쿠킹은 이 한계를 줄이려고 A4c 실행 단계에서 쿠킹 전에 색인을 갱신해(감시 갱신 또는 Rebuild) 색인 의존과 파일 내용을 같은 시점으로 맞춘다.
- A4a 상태: **구현** — 대조표 14.1.61절, coverage 14.1.62절. 구현에서 정한 것:
  - 모듈 `Engine/Source/Developer/AssetCooker`(`CookConfig.h` — `TargetProfile`·`PrimaryAssetRule`·`CookSettings`·`CookBulkCodec` 리플렉션 타입, `ReadTargetProfile`·`ReadCookSettings`·`HashTargetProfile`·`HashCookSettings`·`ClassifyCookPath`). 엔진 기본 파일 `Engine/Config/TargetProfiles/Windows.json`(graphicsBackend D3D12)·`Android.json`(빈 backend — 후속).
  - PrimaryAsset 규칙은 `{type, directories}`다(UE `FPrimaryAssetTypeInfo` — 디렉터리는 하나 이상, 빈 목록은 거부, 진단은 규칙 순번 index와 디렉터리 순번 item). 처음 구현은 "codec이 배열 원소 안의 배열에 대입하지 못한다"고 보고 `{type, directory}`로 바꿨으나 틀린 가정이었다 — 그 제약은 Config의 배열 연산 문법(`+=` 등)의 것이고 ImportJson은 원소 안 배열을 통째 교체로 받는다(리뷰 필수 — 확인 없이 스키마를 바꾸지 않는다).
  - 리뷰 반영(필수 둘·권장 둘): `directories` 배열 복원, `WriteReflected` 실패 갈래는 문자열 칸 읽기(FieldValue)가 할당하므로 도달 가능 — 할당 실패 시험에 규칙 SHA-256을 더해 덮었다(처음 근거 "할당하지 않는다"는 틀렸다), 할당 실패 시험의 끝 상태를 기준(시스템 allocator)과 칸·SHA-256까지 대조, 주입했으면 적어도 하나가 실패(삼킨 실패 없음).
  - 프로파일 id는 `[A-Za-z0-9_]` 1–64 byte(파일 이름 — 경로 문자가 들어가지 않게)이고 읽기가 미리 채운다(파일에서 생략 가능, 다르면 거부 — 대소문자 구분). 읽을 때마다 기본값에서 시작한다.
  - 거부는 `InvalidArgument` + `CookConfigFailure{code InvalidSetting, file, field, index, line, column}`, codec 진단은 호출자의 `TextDiagnostics`에 남는다. 읽기·할당 오류는 code None + 그 오류(규칙 위반과 구분 — 경로 판정 `ClassifyCookPath`도 할당 실패를 `OutOfMemory`로 돌려준다, 처음 구현은 규칙 위반으로 바꿨다).
  - 경로: 에셋은 LogicalPath 규칙, 디렉터리는 끝 '/'를 뗀 것이 마운트(`/Game/` — 대소문자 무시)이거나 논리 경로다. 마운트 없는 `/Game`(끝 '/' 없음)은 거부(에셋 경로에는 이름이 필요하다).
  - 설정 파일 상한 `CookConfigMaxFileBytes` 1 MiB.

## 11. 런타임 로딩과 수명(계획 §9.2 계약)

### 11.1 API

```cpp
struct AssetRequest { AssetId id; TypeId expectedType; LoadPriority priority; ReadyMask required; CancelToken cancel; AssetScope* scope; };
Result<LoadTicket> RequestAsset(const AssetRequest&);
Result<void> OnReady(LoadTicket&, TaskLane lane, Function<void(Result<AssetLease>)>&& callback);
Result<AssetLease> LoadBlocking(const AssetRequest&);
```

- LoadTicket = 요청 관심, AssetLease = 상주 수명(별도 계수). 동시 요청은 합류, caller별 취소는 독립, callback은 caller별 정확히 한 번 terminal. `OnReady` 중복 등록은 `AlreadyRegistered`, 등록 실패 시 callback 미등록·소유권 caller 유지(계획 §9.2).
- `ReadyMask`: `Cpu` 구현, `Render` 계약(`UnsupportedCapability`). 오디오 재생 구간의 준비는 `ReadyMask`에 비트를 더하지 않고 오디오 모듈의 준비 티켓(`PreparePlayback`)이 맡는다([오디오 시스템](AudioSystem.ko.md) 3.12.4절, AD18 — 같은 clip의 seek 구간 둘을 따로 준비해야 해서다). `Cpu`(객체 공개)는 그 티켓의 전제다.
- `TaskLane`: `Game` 구현, `Worker`·`Render` 계약(이번 엔진은 Game 스레드 하나라 전달 대상이 없다 — `Unsupported`).
- `AssetScope`: Tryout·World 수명 범위. `Revoke()`는 그 scope의 신규 접근·발급을 막고 진행 중 callback에 `ScopeClosed`를 한 번 전달, lease를 회수한다(다른 scope의 같은 에셋 lease는 유지).
- `LoadBlocking`: 도구·테스트·엔진 초기화 전용이다. AssetRuntime은 Engine을 모르므로 `EngineAssets`가 "프레임 실행 중" 판정 함수를 `AssetManager`에 주입하고, 그 구간에서 호출하면 `WrongContext`다(로딩 화면은 프레임 밖 초기화 단계 또는 `RequestAsset`을 쓴다).
- `AssetLease`의 해제(소멸)는 즉시 계수를 바꾸지 않고 `AssetLoader::Pump`의 해제 큐로 넘긴다(GC 회수 중의 소멸자에서도 안전하다). 큐 항목은 lease를 발급할 때 미리 할당해 lease 안에 두므로 noexcept 소멸자에서 할당 실패가 없다. scope `Revoke`로 이미 회수된 lease의 소멸은 아무것도 하지 않는다(이중 감소 없음).

### 11.2 단계와 스레드

Requested → ResolveManifest(`CookedManifest.rman`, 하드 의존 SCC 계산) → IO(`AsyncReadFile`) → Verify(크기·CRC·SHA-256)/Decompress(`WorkerScheduler`) → DeserializeNative(worker, 바이트 → 중간 표현) → ConstructObjects(Game, `ConstructionContext`) → Fixup(import 연결) → PostLoad(Game) → Ready. Objects 섹션의 graph v2 1단계(`ObjectGraphStage::Validate` — Registry 정책 사본과 Outer 요약으로 검증하고 ObjectRegistry에 닿지 않는다)는 DeserializeNative의 worker에서 할 수 있고(hook·마이그레이션 콜백이 그 스레드에서 불리므로 스레드 안전해야 한다), Registry 상태 확인(`CreateObjects` 사전 확인)·생성·적용·Required 진단·공개는 Game 단계다(ObjectFoundations 5.4절). Game 단계는 `AssetLoader::Pump()`(EngineAssets 프레임 단계, 도구는 대기 루프)에서 예산(시간·건수) 안에서 진행한다. 한 로드 단위(SCC)의 ConstructObjects·Fixup·공개는 한 Pump 호출 안에서 끝내고(예산은 단위 사이에서 검사), PostLoad만 Pump 사이로 나눌 수 있다. 그래서 GC는 열린 `ConstructionContext`를 만나지 않는다. 실패 시 생성 객체·root를 안전 시점에 정리한다.

### 11.3 순환

하드 참조 그래프의 강연결 성분(Tarjan)을 한 로드 단위로 allocate-all → deserialize-all → fixup-all → publish(계획 §9.2). 다른 패키지의 PostLoad를 동기 대기하지 않는다.

### 11.4 수명

- 상주 단위는 하드 참조 SCC다(순환이 없으면 패키지 하나). 로드 시점의 SCC는 쿠킹 매니페스트의 Hard 의존(런타임) 또는 AssetIndex의 Hard 의존(에디터 패키지를 여는 도구)으로 계산하고, 실행 중 생긴 순환은 6.1절 규칙으로 단위를 합친다. 단위 U의 보유 수 = U에 속한 패키지들에 대한 요청 관심 + lease + 로드 작업의 내부 보유 + 다른 단위의 패키지에서 U의 패키지로 오는 하드 간선 다중도의 합이다(U 내부 간선은 세지 않는다 — 순환이 서로를 붙잡아 영구 상주하지 않는다).
- 의존 보유는 로드 중에도 쥔다: 로드 작업은 ResolveManifest에서 찾은 하드 의존 단위마다 내부 보유를 1 걸고, 완료 시 Fixup이 만든 실제 간선 다중도로 바꾸며 실패·취소 시 놓는다(의존 단위가 Fixup 도중 닫히지 않는다).
- 보유 수가 0이 되면 단위는 Closing 대기 목록에 오른다(Resident의 하위 상태). 다음 `AssetLoader::Pump`(어떤 `With` 범위도 열려 있지 않은 시점)가 처리하기 전에 보유 수가 다시 1 이상이 되면 — 같은 ID 요청이든, 다른 로드 작업의 의존 보유든, 새 간선이든 — 목록에서 빼고 되살린다(revive). 이미 Discard된 단위가 필요해지면 새 로드 작업으로 시작한다. Pump가 처리하면 단위의 패키지마다 `Discard`(부분 트리 — 소속 객체 전부)하고, 모두 성공한 뒤에 RootRef를 놓는다(A0-O: 논리 파괴 + 이름 색인·자식 고리에서 즉시 제거. 이후 이름 조회는 실패하고 WeakObjectRef `TryPin`은 `Destroyed`를 반환한다). Discard가 Busy면 RootRef를 쥔 채 다음 Pump에서 다시 한다(이미 Discard한 패키지는 무동작) — RootRef를 먼저 놓으면 그 사이 GC 조각이 Discard 없이 소속 객체를 회수해 관찰자 계수가 샌다([ObjectFoundations](ObjectFoundations.ko.md) 1.7·2절). Discard가 끝나면 Closing이 완료되고 AssetManager의 ID 표에서 빠지므로 같은 ID의 새 요청은 기다리지 않고 새 로드 작업으로 시작한다(패키지는 이름이 없고 하위 이름은 새 패키지 handle 아래라 충돌하지 않는다). 메모리는 이후 GC가 수거한다. Closing 전이와 Discard는 한 Pump 호출 안에서 Game 스레드가 이어서 하므로 그 사이에 발급이 끼지 않는다(계획 §9.2의 "발급이 먼저면 unload 대기, Closing이 먼저면 그 기록의 발급 실패"와 같다 — 새 요청은 새 로드 작업이다).
- "살아 있는 다른 에셋이 Discard된 객체를 가리키지 않는다"는 불변식의 근거: 에셋 객체 Registry의 모든 객체는 패키지에 속하고(6.1절 — 패키지가 소속 객체를 붙잡으므로 Discard 없이는 회수되지 않는다) 패키지 사이 하드 간선은 모두 다중도로 세어지며(동적 변경 포함) 단위의 보유 수가 0일 때만 Discard한다. Discard는 그 단위에서 나가는 간선 계수를 관찰자 보고로 한 번만 반납한다. 패키지에 들어갈 수 있는 클래스는 `AssetLease`를 멤버로 가질 수 없어(`WorldOnly` 규칙, 5.6절) Soft 순환이 lease로 서로를 붙잡는 누수도 없다.
- 정책: 즉시 해제(기본), PrimaryAsset bundle 로드는 bundle lease를 쥔다. 예산 LRU 해제는 후속. 텍스처 mip residency는 Object 수명과 별도(계획 §9.5), 이번에는 전체 mip 상주.

### 11.5 A1 설계 보강 요구사항(2026-11-13)

오브젝트 직렬화·비동기 로딩 고려사항 17개를 현재 구현·계획과 대조한 결과, 아래 네 가지가 비어 있거나 일부만 있다. A1 구현 API 확정안을 쓸 때 반드시 정하고 교차검증 범위에 넣는다(사용자 요청). 나머지 13개는 11.1–11.4절, 계획 §9.2–9.5, ReflectionFoundations 4·6절, ObjectSerialization 구현으로 대응된다.

1. **시간·메모리 예산(부분)**: 그래프 단위 한도(작업량·바이트·필드·마이그레이션 바이트)와 `AssetLoader::Pump`의 프레임당 시간·건수 예산, `LoadPriority`는 있다. 정할 것: 동시 진행 로드 작업 수와 단계별(IO·압축 해제·해석) 임시 버퍼 총량의 상한과 초과 시 대기 규칙, 큰 작업(큰 SCC·큰 bulk)의 분할 단위, 우선순위 변경(요청 중 승격)의 반영 시점, 오래 기다린 요청이 높은 우선순위에 밀려 영원히 진행하지 못하지 않게 하는 규칙(aging 또는 최소 진행 보장).
2. **미준비 객체 동작 정책(없음)**: `OnReady`(콜백 대기)·`LoadBlocking`(명시 동기)·우선순위는 있으나, 게임 명령이 아직 준비되지 않은 대상(로드 중 에셋·Ready가 아닌 객체)에 닿을 때의 정책이 없다. 정할 것: 호출자가 고르는 정책 — 보류(대상 준비 후 실행), 우선 로드(우선순위 승격 후 보류), 명시 실패(`NotReady` 등 오류 반환), 동기 대기(명시 API만) — 과 기본값, 명령을 조용히 버리지 않는 규칙(정책 밖이면 오류), 보류한 명령을 실행하기 전에 대상·문맥(같은 세대·scope가 닫히지 않음·대상이 아직 유효)을 다시 검증하는 규칙, 보류 큐의 상한과 취소.
3. **숨겨진 동기 로딩 추적(부분)**: 조회·포인터 접근은 로드하지 않고 동기 대기는 `LoadBlocking`(도구·초기화 전용)뿐이다. 정할 것: 동기 대기 발생 위치(호출 지점)·대상·대기 시간을 Rocks Lens 이벤트로 남기는 계약, 게임 실행 중(Game 프레임 안) 동기 대기를 금지하거나 경고하는 구성별 규칙.
4. **로딩 상태 관측(부분)**: 직렬화 진단 hook과 Rocks Lens(CPU·Task·GC·IO 구간, asset·object 조회), 순환·중복 요청·취소·월드 종료 경합 시험은 있다. 정할 것: 요청·로드 작업·단위(SCC)마다 지금 단계와 무엇을 기다리는지(의존 단위·IO·Pump 예산)를 조회하는 대기 그래프, 단계별 시간·임시 메모리 사용량, 실패 원인(누락 에셋·검증 실패·마이그레이션 실패), 강제 대기 위치(3번) — 를 도구가 읽는 형식.

### 11.6 구현에서 정한 점(A1c — 로더)

A1c 구현(`Engine/Source/Runtime/AssetRuntime` — `CancelToken.h`·`AssetLoader.h`·`CookedPackageIo.h`, 상태: **구현** — 시험 14.1.5절, coverage 14.1.6절)이 11.1–11.5절에서 비어 있던 것을 다음과 같이 정했다. A1c는 쿠킹 매니페스트에서 패키지를 읽어 에셋 객체 Registry에 공개하는 로더다. 요청 관심·lease·scope·`OnReady` callback·상주 단위의 계수와 Closing(11.1·11.4절)은 그 위의 `AssetManager`(A1d)가 맡는다. A1c의 로더는 공개한 패키지의 RootRef를 쥐고, 언제 놓을지는 AssetManager가 정한다 — `TakePublished`·`PublishedHolds`·`RetirePublished`·`ReleaseRetired`(11.7절 2). AssetManager 없이 로더만 쓰면 `Shutdown`까지 붙잡는다.

1. **요청과 결과**(UE `LoadPackageAsync` + `FLoadPackageAsyncDelegate` 대응을 결과 조회로): `AssetLoader::Request({id, priority, cancel})`는 Game 스레드에서 부르며 `AssetLoadHandle`(번호·세대)을 준다. 결과는 `Status(handle)`(Pending·Succeeded·Failed·Cancelled + 오류·진단·성공이면 패키지 handle)로 읽고 `Release(handle)`로 기록을 돌려준다(Pending이면 취소와 같다). callback 전달은 A1d가 이 결과 위에 얹는다(같은 결과를 두 경로로 내지 않는다). 매니페스트에 없는 id는 요청 때 `NotFound`(진단 `MissingDependency`, asset = 그 id)다. 이미 공개한 패키지 요청은 그 자리에서 Succeeded다. `Initialize`는 받은 매니페스트를 `ValidateCookedManifest`로 다시 확인한다(검증하지 않은 모델의 빠진 Hard 대상이 해석에서 범위 밖 색인이 되지 않게 — 위반은 그 진단 그대로).
   - 버린 대안: 로더가 callback을 직접 부르는 것 — 11.1절의 callback 계약(lane·scope·정확히 한 번)은 AssetManager가 정하므로 로더가 따로 부르면 두 계약이 생긴다.
2. **ResolveManifest·SCC**(UE AsyncLoading2의 `ImportPackages`·`ProcessDependencies` 대응을 로드 전에 한 번에): 요청 에셋에서 매니페스트 Hard 의존을 따라 깊이 우선으로 닿는 패키지(Hard 전이 폐포, Soft는 따라가지 않는다)를 모으고, 이미 공개했거나 진행 중인 단위에 속한 패키지에서 멈춘다(그 단위가 의존 단위가 된다). 새 패키지들에 Tarjan SCC(반복 구현 — 깊이 2만 사슬에서 스택을 쓰지 않는다)를 돌려 SCC마다 로드 단위 하나를 만든다. 단위 사이 간선은 응축 DAG이고 Tarjan 완료 순서가 의존 먼저다. 매니페스트가 Hard 대상의 존재를 보장하므로(10.1절 6) 해석은 실패하지 않는다(할당 실패만 있다).
   - `FindByPath(path)`: 초기화 때 매니페스트 경로 힌트의 caseless 키(`TryMakeNameComponentKey` — 10.1절 6과 같은 키) 정렬 색인을 만들고 이진 탐색한다. 경로 규칙 위반은 `InvalidArgument`, 없으면 `NotFound`다.
3. **패키지 단계**(UE `EAsyncPackageLoadingState2`의 WaitingForIo → ProcessPackageSummary → … → Complete를 줄인 것): Queued(허가 대기) → Reading(`CookedPackageIo`) → Preparing(worker: 크기·SHA-256 확인 → `PackageLoadState::Prepare`(압축 해제·검증, `maxBulkRawBytes` = 매니페스트 bulkRawBytes) → `CheckCookedPackageContents`) → Prepared → (단위의 모든 패키지가 Prepared이고 의존 단위가 모두 공개되면) Instantiating(Game Pump: 한 트랜잭션에 SCC 형제를 모두 `InstantiatePackages` → Commit) → Loaded. 단계 전이는 모두 Game 스레드(`Pump`)가 한다. IO·worker 완료는 완료 큐(뮤텍스 + 패키지 작업 안의 침입 연결 — 게시에 할당이 없다)에 쌓이고 Pump가 꺼낸다.
   - 크기·SHA-256은 해석 전에 본다(바뀐 파일을 해석하지 않는다). 그래서 A1b의 `CheckCookedPackage`를 파일 쪽 `CheckCookedPackageFile`(크기·SHA-256)과 내용 쪽 `CheckCookedPackageContents`(종류·assetId·대표 타입·bulk 합·프로파일·의존)로 나눴다(`CheckCookedPackage`는 둘을 차례로 부른다 — 진단은 같다). 파일 크기는 IO 단계가 먼저 본다(크기가 다르면 읽지 않는다 — PackageBytes 손상).
   - 버린 대안: IO 완료 callback이 바로 worker를 제출하는 것(UE의 ALT처럼 Game을 거치지 않음) — 단계 상태를 두 스레드가 바꾸게 되어 취소·실패 경합이 커진다. 한계: 단계마다 Pump 한 번의 지연이 생긴다(프레임마다 Pump를 부르므로 IO·worker 사이 최대 한 프레임).
4. **패키지 경로와 IO**: 경로는 `<쿠킹 루트>/` + `AppendCookedPackagePath`(4.2절)다. IO는 `CookedPackageIo` 인터페이스(`Begin(read)` — 완료는 아무 스레드에서 정확히 한 번 `read.complete`, 즉시 실패는 반환값, 로더가 완료를 처리한 뒤 `Release(read)`)로 하며 기본 구현 `AsyncCookedPackageIo`가 PlatformBase `AsyncReadFile`을 쓴다(열기 → 크기 → 크기가 매니페스트와 같을 때만 전체 읽기). 시험은 완료를 명시 gate로 내보내는 구현을 끼워 IO 순서·오류를 결정적으로 만든다. 기본 구현의 OS 오류 경로(읽기 완료 실패·크기 조회 실패·즉시 제출 실패)는 PlatformBase 결과 주입(`AsyncIoContext::InjectFault` — PlatformLayer 문서)으로, 크기를 본 뒤 파일이 줄어드는 짧은 읽기는 완료 게시 보류(`HoldCompletions`) 사이에 파일을 다시 써서 결정적으로 만든다. 짧은 읽기는 `InvalidArgument`(진단 None, asset = 그 패키지)다. OS IO 실패는 Core `FileErrorCode` 대응을 그대로 쓰므로(원본 오류 보존) 접근 거부·시스템 오류가 `DependencyFailed`가 되어 의존 실패 전파와 오류 코드가 같다 — 둘은 `diagnostic.code`로 가른다(디스크 오류 None, 의존 실패 `MissingDependency`).
   - 진단: 파일 없음은 `NotFound` / `MissingFile`, 그 밖의 IO 실패는 진단 None과 `FileErrorCode` 대응 오류, 크기·SHA-256 불일치는 `CorruptPackage`(PackageBytes·PackageSha256 칸 — 10.1절 8).
5. **worker 실행**: `AssetLoaderWorker` 인터페이스(`Run(fn, context)` — 할당 없는 함수 포인터, 성공하면 정확히 한 번 실행)로 하며 기본 구현이 `WorkerScheduler`에 제출한다. 시험은 손으로 돌리는 구현을 끼운다.
6. **실패 전파**: 패키지 하나가 실패하면 그 단위(SCC) 전체가 실패한다. 그 패키지를 요청한 요청은 그 진단을, 같은 단위의 다른 패키지와 그 단위에 Hard로 기대는 단위(전이)의 요청은 `DependencyFailed` / `MissingDependency`(asset = 요청 에셋, related = 처음 실패한 패키지)를 받는다. Soft 대상은 로드하지 않으므로 Soft 대상의 실패는 영향이 없다(6.2절). 실패한 단위는 남기지 않는다 — 같은 에셋의 다음 요청은 새 로드다. 오류 코드 `DependencyFailed`는 디스크 오류(4)와 같으므로 의존 실패는 `diagnostic.code == MissingDependency`(related = 원인 패키지)로 가린다.
   - UE는 import 패키지 실패를 경고로 남기고 null import로 계속한다. Rocks는 6.2절 Hard 규칙(실패 시 참조자도 실패)을 따른다.
7. **합류·취소**: 같은 단위에 닿는 요청은 그 단위에 합류한다(진행 중 단위의 패키지 요청, 진행 중 단위에 의존하는 새 단위). 단위의 관심 수 = 직접 요청(Pending) 수 + 그 단위에 기대는 진행 중 단위 수다. 취소 표시(`CancelToken`)는 Pump가 단계 경계에서 본다: 표시된 요청은 Cancelled로 끝나고(다른 요청은 그대로) 관심을 놓는다. 관심이 0이 된 미공개 단위는 버린다(Abandoned) — 진행 중 IO·worker는 끝까지 기다렸다가 결과를 버리고, 버린 단위의 의존 단위 관심도 놓는다(연쇄). 공개된 단위는 버리지 않는다 — 관심이 0이 돼도 PostLoad를 마친다. 다만 그 단위의 PostLoad가 끝나기 전에 취소된 요청은 Cancelled로 끝난다(PostLoad가 끝나 Succeeded가 된 요청은 취소와 무관하다). 한 Pump 안의 순서는 완료 처리 → 취소 확인 → 허가 → 공개다. 그래서 같은 Pump에서 실패가 확정된 요청은 취소 표시가 있어도 실패로 끝난다(실패가 이긴다 — 취소 확인은 이미 끝난 요청을 보지 않는다). 버린 단위가 진행 중 IO를 기다리는 동안 같은 에셋을 다시 요청하면 새 단위로 새로 읽는다(버린 단위의 결과를 쓰지 않는다).
   - `CancelToken`은 원자 표시 + 참조 계수 공유 상태다(`CancelSource::Create(allocator)`로 만들고 `Token()`은 복사, 기본 token은 취소되지 않는다). UE 5.8 로딩에는 개별 패키지 취소가 없고(`FStreamableHandle::CancelHandle`은 관심만 놓는다) Rocks는 관심이 0이면 로드도 멈춘다.
8. **우선순위**(11.5절 1, UE `TAsyncLoadPriority`·`UpdatePackagePriorityRecursive` 대응): `LoadPriority`는 Low·Normal·High·Critical이다. 단위 우선순위 = 직접 요청과 기대는 단위 중 가장 높은 값이고, 올라가기만 한다(합류·`SetPriority` 승격이 의존 단위로 재귀 전파 — UE와 같다, 취소로 내려가지 않는다). 반영 시점: 허가 대기 순서와 Pump의 공개 순서에 다음 결정부터 반영하고, 이미 IO·worker에 들어간 작업은 바꾸지 않는다(UE는 IO 요청 우선순위도 바꾼다 — 기본 IO 구현은 우선순위가 없는 `AsyncReadFile`이라 하지 않는다).
9. **동시 진행·임시 메모리 상한**(11.5절 1): 허가는 단위 단위로 한다. `AssetLoaderLimits{maxPackagesInFlight, maxTempBytes}` — 허가된 패키지 수와 그 임시 byte(파일 크기 + bulkRawBytes) 합이 상한 안일 때만 다음 단위를 허가하고, 허가된 것이 없으면 상한을 넘는 단위도 혼자 허가한다(최소 진행 보장). 임시 byte는 패키지 작업을 지울 때(공개·실패·버림 뒤 진행 중 IO·worker가 끝났을 때) 돌려준다. 합은 모두 포화 덧셈이다 — A1b가 bulkRawBytes 최댓값을 받으므로 파일 크기와 더해 감기면 상한을 우회한다. 작업마다 실제로 더한 양을 기억해 그만큼 뺀다(포화된 합도 0으로 돌아온다). 허가 후보는 의존 단위가 모두 허가됐거나 공개된 단위뿐이다 — 허가된 단위는 언제나 끝까지 갈 수 있어 예산이 서로 기다리는 단위로 막히지 않는다(교착 없음). 고르는 순서는 실효 우선순위 → 생성 순서이고, 첫 후보가 상한에 걸리면 뒤 후보를 건너뛰어 허가하지 않는다(큰 단위가 작은 단위에 영원히 밀리지 않게).
   - aging(굶주림 방지): 실효 우선순위 = min(Critical, 우선순위 + 대기 중 다른 단위가 허가된 횟수 / `agingAdmissions`). 시계가 아니라 허가 횟수로 세어 결정적이다.
   - 큰 작업의 분할: 단위(SCC)는 한 트랜잭션이라 나누지 않는다. 패키지 하나의 압축 해제도 worker 작업 하나다(블록 단위 병렬 해제는 후속 — 한계).
10. **Pump 예산**(11.2절): `Pump({maxUnits, maxNanoseconds})`는 완료 처리·취소 확인·허가를 한 뒤 공개 준비가 된 단위를 실효 우선순위 → 생성 순서로 공개한다. 예산은 단위 사이에서 검사하고, 한 Pump는 준비된 단위가 있으면 적어도 하나를 공개한다. 시간은 설정의 시계 함수(기본 `platform::MonotonicNanoseconds`)로 잰다 — 시험은 단위마다 정해진 만큼 가는 시계를 끼운다.
11. **동기 대기 추적**(11.5절 3): `WaitFor(handle, site)`는 결과가 나올 때까지 Pump와 완료 대기(`WaitForCompletions` — 완료 큐가 비지 않거나 진행 중 비동기 작업이 없을 때까지 조건 변수로 기다린다. 시간 대기가 없다)를 되풀이한다. 설정의 "프레임 실행 중" 판정이 참이면 `WrongContext`(구성과 무관하게 금지 — 11.1절 `LoadBlocking`과 같은 규칙). 대기마다 호출 지점·대상·걸린 시간을 `AssetLoaderObserver::OnSyncWait`로 알리고 횟수·누적 시간을 상태 조회에 남긴다. Rocks Lens 이벤트로의 연결은 EngineAssets가 이 관찰자로 한다.
12. **상태 관측**(11.5절 4): `Describe(snapshot)`가 요청·단위·패키지 표를 채운다 — 요청(에셋·우선순위·상태·오류·진단·단위), 단위(단계·실효 우선순위·관심 수·기다리는 것(허가·의존 단위·IO·worker·Pump)·기다리는 의존 에셋·실패 진단), 패키지(에셋·단계·임시 byte·단계별 누적 시간 ns). 실패해 진행 중 일을 기다리는 단위는 실패 원인(처음 실패한 패키지·오류·진단)을 보인다. 동기 대기는 횟수·누적 시간과 함께 최근 `AssetLoaderSnapshot::RecentSyncWaitCapacity`(8)개의 호출 지점·대상·시간을 고정 배열로 남긴다(11.5절 3의 강제 대기 위치). 도구는 이 구조를 읽는다(텍스트 형식은 도구 몫).
13. **할당 실패**: 요청·해석·허가·완료 처리·공개의 모든 할당 실패는 그 요청(또는 단위)의 `OutOfMemory`(진단 None)이고 로더 상태는 일관되게 남는다(그 요청만 실패, 다른 요청은 계속). 게시 경로(IO·worker 완료)는 할당하지 않는다.
14. **종료**: `Shutdown()`은 새 요청을 막고(`ShuttingDown`) Pending 요청을 `ShuttingDown`으로 끝내며 모든 미공개 단위를 버린 뒤 진행 중 IO·worker가 끝날 때까지 기다리고(완료 대기 — 시간 대기 없음) 공개한 패키지의 RootRef를 놓는다. 소멸자는 Shutdown을 부른다.
15. **스레드**: 로더의 모든 공개 함수(`FindPackage` 포함)는 Game(에셋 객체 Registry 소유) 스레드 전용이다. 다른 스레드에서 부르면 `WrongThread`다. 소유 스레드가 아닌 곳에서 초기화된 로더를 파괴하면 진행 중 IO·worker를 남긴 채 상태를 지우게 되므로 `platform::Fatal`이다(수명 오류). worker는 패키지 작업의 `PackageLoadState`·버퍼만 만지고, 버린 표시는 원자 값으로 본다.
16. **PostLoad 단계**(11.2절, UE AsyncLoading2 `EAsyncPackageLoadingState2::PostLoad`·`DeferredPostLoad` 대응): 단위를 공개한 뒤 구성 패키지마다 설정의 `AssetPostLoadHandler::PostLoad(package, asset)`을 Game 스레드에서 부른다. 순서: 그 단위가 기대는 단위의 PostLoad가 모두 끝난 뒤, 단위 안은 AssetId 순이다(UE도 import 패키지의 PostLoad를 먼저 끝낸다). 다른 패키지의 PostLoad를 동기 대기하지 않고 Pump 사이로 나눈다 — Pump 예산 `maxPostLoads`(호출 수)와 공개와 같은 시간 예산이 묶는다. 준비된 PostLoad가 있으면 한 Pump에 적어도 하나를 부른다 — 공개가 단위 수·시간 예산을 다 써도 PostLoad 하나는 부르고 소진을 표시한다(공개할 단위가 계속 들어와도 PostLoad와 요청 완료가 밀리지 않게). 요청은 그 단위의 PostLoad가 끝나야 Succeeded다(공개만 된 채 PostLoad 대기면 Pending — 상태 관측의 대기 이유 PostLoad). PostLoad는 실패하지 않는다(UE `UObject::PostLoad`와 같다 — 실패할 수 있는 처리는 그 타입이 자기 상태로 남긴다). PostLoad 대기 중 취소는 그 요청만 Cancelled이고 공개된 단위는 PostLoad를 마친다. 종료는 남은 PostLoad를 부르지 않는다. handler가 없으면 공개와 함께 PostLoad가 끝난 것으로 본다. `FindPackage`는 PostLoad 전 패키지도 돌려준다(다른 단위의 import 해석이 공개된 패키지를 찾아야 하므로 — PostLoad 완료 여부는 요청 결과로 본다).
   - 판단 근거: ObjectRuntime·리플렉션에는 클래스별 PostLoad(`UObject::PostLoad` 대응 가상 함수·클래스 메타데이터 hook)가 없다. 단계의 순서·분할·대기 규칙은 로더가 정해야 하므로 단계와 호출 경계는 A1c가 구현한다. 객체마다 어느 함수를 부를지(클래스 hook 등록·dispatch)는 그것이 처음 필요한 A5(텍스처 — 플랫폼 데이터 정리)가 RuntimeClassMetadata와 함께 정하고, 그 dispatch를 이 handler로 연결한다(EngineAssets가 handler를 준다). 그때까지 빠지는 것: 클래스가 스스로 선언하는 PostLoad 처리(로드 뒤 파생 상태 계산 등)는 handler를 준 쪽만 할 수 있다.
17. **시험 hook**: `AssetLoaderConfig::waitEntered`는 완료 대기가 실제로 조건 변수에서 기다리기 직전(큐 뮤텍스를 쥔 채)에 불린다. 시험이 "Game 스레드가 기다리기 시작한 뒤"를 사건으로 받아 완료를 내보내게 한다(시간에 기대지 않는다). 다른 콜백과 같이 18의 재진입 규칙을 따른다 — 상태를 바꾸는 호출은 `WrongContext`이고(큐 뮤텍스를 다시 잡는 경로 — 완료 대기·Pump의 완료 처리 — 에 닿기 전에 거부되므로 교착이 없다), 읽기(`Status`·`Describe`·`FindByPath`·`FindPackage`)는 큐 뮤텍스를 잡지 않고 Game 스레드가 상태를 바꾸지 않는 동안 불리므로 허용한다.
18. **콜백 재진입**: 로더가 Game 스레드에서 동기로 부르는 모든 바깥 코드 — PostLoad handler·관찰자(`OnSyncWait`)·시험 hook(`waitEntered`)·IO(`CookedPackageIo::Begin`·`Release`)·worker(`AssetLoaderWorker::Run`)·시계(`clock`)·프레임 판정(`inFrame`)·공개 트랜잭션(`ConstructionContext` 생성부터 소멸까지 — `InstantiatePackages`·`Commit`과 실패 때의 롤백. 그 안에서 클래스 생성자·소멸자, 필드 codec·Validate hook·Required 검사, Registry allocator가 불린다) — 가 불리는 동안 로더는 "콜백 중" 표시를 쥔다(허가 순회가 단위 참조를 쥔 채 Begin을 부르는 등, 그 사이 상태가 바뀌면 같은 수명 결함이 나므로 주석이 아니라 표시로 막는다). 표시는 호출마다 범위 guard 하나(`LoaderCallbackScope`)가 세우고 끝나면 이전 값으로 되돌린다. 그래서 겹친 호출(트랜잭션 안에서 다른 바깥 코드에 닿는 경로가 생겨도)이 바깥 표시를 먼저 풀지 않는다. 표시 밖에 두는 것과 그 이유: 로더 allocator(`AssetLoaderConfig::allocator`)는 worker·IO 완료 스레드에서도 불리는 메모리 서비스라 Game 스레드 표시로 덮을 수 없고, Core `Allocator` 계약상 호출자에게 재진입하지 않는다(UE `FMalloc`도 같다). worker에서 도는 Prepare와 IO 완료 함수는 Game 스레드가 아니므로 거기서 부른 공개 함수는 15의 `WrongThread`가 막는다. 그 사이 상태를 바꾸는 공개 함수(`Request`·`SetPriority`·`Release`·`Pump`·`WaitForCompletions`·`WaitFor`·`Shutdown`)는 `WrongContext`를 돌려주고 아무것도 바꾸지 않는다(단위 표 재할당·같은 단위의 중첩 완료를 막는다). 읽기(`Status`·`Describe`·`FindByPath`·`FindPackage`)는 된다. 콜백이 더 일을 시키려면 돌아간 뒤 Pump 밖에서 한다(UE도 PostLoad 안 동기 로드는 금지·경고한다). 콜백 안에서 로더를 파괴하면 돌아온 뒤 해제된 상태를 읽으므로 소멸자가 `platform::Fatal`로 수명 오류를 드러낸다.

### 11.7 구현에서 정한 점(A1d — AssetManager)

A1d(`Engine/Source/Runtime/AssetRuntime` — `AssetManager.h`, 상태: 구현)가 11.1·11.4·11.5절 2와 6.1절에서 비어 있던 것을 다음과 같이 정했다. 애매한 곳은 UE 5.8 `UAssetManager`·`FStreamableManager`·`FStreamableHandle`을 따른다. 설계 교차 검증(리뷰어 1명, 지적 11개)을 반영했다.

1. **구성과 소유**: `AssetManager`가 에셋 객체 Registry(패키지 정책, 고유 ContextId, 엔진 TypeRegistry 공유, `ReferenceObserver` 설치 — 6.1절, 관찰자 `maxKeys`는 설정)와 `AssetLoader`(A1c)를 소유한다. 설정은 로더 설정(매니페스트·쿠킹 루트·IO·worker·시계·프레임 판정·PostLoad handler·상한)에 Registry 설정·관찰자 키 상한·보류 명령 큐 상한을 더한 것이다. 모든 공개 함수는 Game(Registry 소유) 스레드 전용이고(`WrongThread`), 예외는 `AssetLease`·`LoadTicket`·`PrimaryAssetTicket`·`PrimaryAssetLease`의 소멸(아무 스레드 — 해제 큐, 5)이다. `AssetScope`의 소멸은 Game 스레드 전용이다(다른 스레드면 `platform::Fatal`). 에셋 접근은 `With(lease, fn)`이 에셋 문맥 `ObjectAccessScope`를 열고 부른다 — lease가 있으면 보유가 1 이상이라 그 단위는 Closing·Discarding일 수 없다(불변식).
   - `Registry()`·`Loader()` 계약(리뷰 지적으로 명시): `Registry()`로는 객체 읽기·필드 쓰기·하드 참조 편집과 상주 패키지 안 생성 트랜잭션(asset-new·복제 경로)을 할 수 있다 — 그 간선 변화는 관찰자 묶음으로 계수된다(3). AssetManager가 넘겨받은 패키지와 그 객체의 수명 변경(`Discard`·`RequestDestroy`·패키지 RootRef 조작)은 계약 위반이다(상주 단위가 모르는 사이 객체가 사라진다). `Loader()`는 조회(`FindPackage`·`FindByPath`·`Status`·`Describe`·`PublishedHolds`)만 할 수 있고, 요청·해제·우선순위·`Pump`·`WaitFor`·`Shutdown`·넘겨받기 API(`TakePublished`·`RetirePublished`·`ReleaseRetired`)를 직접 부르는 것은 계약 위반이다(티켓·상주 단위 계수를 우회한다). UE `UAssetManager::GetStreamableManager()`가 공개인 것과 같이 조회·도구용으로 열어 두되 소유는 AssetManager다. 위반은 진단하지 않는다(그 결과로 생기는 `RetirePublished` 실패는 `platform::Fatal` — 14.1.8).
2. **로더 확장**(A1c에 더하는 API — 공개 패키지의 수명 결정을 넘겨받는 경계):
   - `TakePublished(out)`: PostLoad까지 끝난 단위(로더 `FinishUnit`)를 끝난 순서대로 꺼낸다 — 구성 AssetId·패키지 handle·공개 순번(group)·RootRef 칸 번호. PostLoading 단위는 넘기지 않는다(로더가 지킨다 — 그 사이 AssetManager가 Discard해 로더가 Discard된 패키지에 PostLoad를 부르는 일이 없다). 넘긴 뒤에도 로더가 RootRef를 쥔다.
   - `PublishedHolds(asset)`: 진행 중(공개 전) 단위 가운데 그 공개 패키지에 Hard로 기대는 단위 수(11.4절 "로드 작업의 내부 보유"). 해석 때 PostLoad까지 마친 공개 패키지를 만나면 1을 걸고, 의존 단위가 PostLoad를 마쳐 사라질 때 아직 공개 전인 기대는 단위로 이어서 걸며(그 구성 패키지마다), 기대는 단위가 공개(Commit — 그 뒤는 Commit 간선 다중도가 대신한다)·실패·버림으로 끝날 때 놓는다.
   - `RetirePublished(assets)`: 그 id들을 공개되지 않은 것으로 돌린다 — `Request`는 새 로드, `FindPackage`는 NotFound, 해석은 새 단위를 만든다. RootRef는 그대로 쥔다(칸은 퇴출 상태). PostLoad 중이거나 공개 보유가 남은 id·공개되지 않은 id는 `InvalidArgument`. 할당하지 않는다.
   - `ReleaseRetired(slots)`: 퇴출된 칸의 RootRef를 놓고 칸을 돌려준다(퇴출 상태가 아닌 칸은 `InvalidArgument`). 같은 id의 옛 패키지(Discarding)와 새로 로드한 패키지가 함께 있을 수 있다(패키지는 이름이 없다).
   - 버린 대안: 로더가 계수·Closing까지 맡는 것 — 티켓·lease·scope 계수는 11.1절 계약이고 AssetManager가 정하므로 둘로 나뉜 계수가 생긴다. RootRef를 AssetManager로 옮기는 것 — 로더의 `FindPackage`·이미 공개 판정과 두 곳이 같은 상태를 들게 된다. Discard 전에 로더가 공개 상태를 지우는 한 번짜리 API — Discard가 Busy로 여러 Pump에 걸치면 그 사이 요청이 Discard된 패키지를 받는다.
3. **상주 단위와 계수**(11.4절): 상주 단위는 `TakePublished`로 넘어온 로드 단위(SCC)에서 시작한다. 상태는 Resident → Closing(대기 — revive 가능) → Discarding → 끝이다. 보유 = 티켓 관심(그 단위의 패키지를 요청한 살아 있는 티켓 — Succeeded 뒤에도 티켓이 살아 있는 동안) + lease + 보류 명령(9 — 내부 티켓의 보유를 티켓 칸이 아니라 보류 칸에 센다, 관측 11) + 로더 공개 보유(2, 판정 때 다시 읽는다) + 다른 단위(또는 아직 넘어오지 않은 PostLoading 단위)의 패키지에서 오는 하드 간선 다중도다(같은 단위 안의 간선은 세지 않는다).
   - 간선 표: `Pump`가 `ReferenceObserver::TakeChanges` 묶음 전체를 (소유 패키지, 대상 패키지) 쌍 표에 더한 뒤 판정한다(중간 합이 음수일 수 있으므로 묶음을 다 더한 뒤 — 6.1절). 관찰자 키는 원인·트랜잭션까지 나누므로 같은 쌍이 한 묶음에 여러 항목(Edit −1과 Commit +1 등)으로 오고 순서는 키가 처음 생긴 순서다 — 그래서 묶음을 받으면 먼저 쌍별로 합치고(제자리 정렬 뒤 이웃 합산, 0인 쌍은 뺀다, 할당 없음) 그 합으로 판정한다. 쌍별 합은 실제 다중도 변화라 표 값이 음수가 되지 않는다(되면 불변식 위반 `platform::Fatal`). 처음 구현은 항목마다 판정해 "X → B 설정, 같은 패키지의 새 객체 Y → B Commit, 둘 다 지움" 순서에서 Fatal에 닿았다(리뷰 지적 — 고침). 쌍 표는 대상 패키지별 들어오는 간선 목록으로도 찾을 수 있게 둔다. 상주 단위가 아닌 패키지(PostLoading 단위 등)가 소유자인 간선도 대상 단위의 보유다. 상주 단위가 만들어질 때 그 구성 패키지로 들어오는 간선을 쌍 표에서 모아 보유에 더한다.
   - 병합(union-find): 새 단위 간선 U→V(0 → 양수)가 V에서 U로 가는 경로를 만들면, V에서 닿고 U로 닿는 단위 전부(전방 도달 ∩ 후방 도달)를 하나로 합친다(묶음 안 간선 순서와 무관하다). 합칠 때 단위 간선을 다시 모으고(구성원 사이 간선은 빼고) 보유를 다시 계산하며, 합쳐지는 단위 가운데 Closing 대기는 되살린다. 합친 단위는 Discard까지 나누지 않는다(보수 규칙 — 간선이 0이 돼도 나누지 않는다). Discarding 단위는 병합하지 않는다.
   - Closing: 보유가 0이 된 Resident 단위는 Closing 목록에 오른다. 판정 때 보유가 다시 1 이상이면 Resident로 되살린다(revive). 처리: 구성 패키지 첫째를 `ObjectRegistry::Discard`해서 성공하면 그 자리에서 `RetirePublished(구성 전부)`를 부르고 Discarding이 된다. 첫 Discard가 Busy(그 패키지 아래 공개 전 객체 — 열린 `ConstructionContext`)·OutOfMemory·CapacityExceeded(관찰자 키 상한)면 아무것도 바뀌지 않았으니 Closing으로 남아 다음 Pump에서 다시 판정한다(revive 가능).
   - Discarding: 남은 패키지를 Discard하고(같은 실패는 다음 Pump에서 다시 — 이미 Discard한 패키지는 무동작), 모두 성공하면 `ReleaseRetired`로 RootRef를 놓고 단위를 표에서 뺀다(그 할당이 실패하면 Discarding으로 남아 다음 Pump에서 `ReleaseRetired`만 다시 한다). Pump 결과 `releasedPackages`는 이 Pump가 RootRef를 놓은 패키지 수다 — Discard한 패키지는 RootRef를 놓기 전까지 GC가 회수하지 못하므로 Registry GC를 거는 쪽(EngineAssets — 11.8절 5)은 `discardedPackages`가 아니라 이 값을 본다(A1e 리뷰 뒤 시험에서 찾았다: `ReleaseRetired` 할당 실패를 다음 Pump가 회복해도 그 Pump는 Discard가 0이라 GC 요청이 서지 않고 객체가 재시도 시간까지 남았다). revive하지 않는다 — 같은 id의 요청은 로더에서 새 로드다(11.4절 "이미 Discard된 단위가 필요해지면 새 로드"). 한계: Discarding이 여러 Pump에 걸치는 동안 아직 Discard하지 않은 패키지로 새 하드 간선이 생기면(약한 참조 TryPin 뒤 하드 참조 설정) 그 간선은 Discard 뒤 Destroyed를 가리킨다. AssetManager는 그런 간선을 진단(관측 칸 `lateEdgesIntoDiscarding`과 Warning 로그 한 번)으로 드러낸다. 첫 Discard가 성공하면 같은 Pump 안에서 퇴출까지 끝나므로 이 창은 Busy가 첫 패키지 뒤에서 날 때만 생긴다.
   - Discard가 나가는 간선을 음수로 보고하므로 의존 단위는 다음 Pump에서 Closing에 오른다(연쇄 — 결정적).
   - Pump 예산 `maxDiscards`(패키지 수, 기본 무제한)가 한 Pump의 Discard 수를 묶는다(큰 단위의 Discard가 프레임을 튀게 하지 않게).
4. **Pump 순서**(한 Pump 안의 판정 순서를 고정한다): (1) 해제 큐 비우기(lease·티켓·보류 명령 감소) → (2) 로더 `Pump` → (3) `TakePublished`로 상주 단위 만들기 → (4) `TakeChanges` 묶음 적용과 병합 → (5) 끝난 로더 요청을 티켓에 반영하고 callback 목록 만들기 → (6) Closing 판정(보유 전체를 이 시점에 다시 계산) → (7) Closing·Discarding 처리 → (8) callback·보류 명령 실행. `TakeChanges`가 실패하면(Busy·인자) 그 Pump의 (6)·(7)을 건너뛴다. `RequestAsset`·`LoadPrimaryAsset`·`RunWhenReady`는 대상이 상주(Closing 포함, Discarding 제외)면 그 자리에서 단위 보유를 1 올린다(revive) — 로더가 즉시 Succeeded로 답하는 요청이 같은 Pump의 Closing 처리에 먼저 Discard되지 않게.
5. **티켓과 callback**(11.1절, UE `FStreamableHandle` 대응): `RequestAsset(request)`가 로더 요청 하나와 티켓 슬롯을 만들고 `LoadTicket`(move-only)을 준다. 같은 에셋의 요청은 로더에서 합류하고 티켓은 caller마다 따로다(개별 취소). `OnReady(ticket, lane, callback)`: 티켓당 하나(`AlreadyRegistered`), lane은 Game만(`Worker`·`Render`는 `Unsupported` — 11.1절), 등록 실패면 callback은 등록되지 않고 caller가 갖는다. callback은 그 요청이 끝난 뒤의 Pump (8)에서 정확히 한 번 `Result<AssetLease>`로 불린다: 성공이면 lease(보유 1), 실패면 로더 결과(오류·진단), `expectedType`이 있으면 대표 에셋이 그 타입(IsA)이 아닐 때 `TypeMismatch`, 티켓을 먼저 놓으면 `Cancelled`, scope Revoke면 `ScopeClosed`, Shutdown이면 `ShuttingDown`. 이미 끝난 티켓에 등록하면 다음 Pump에서 부른다(등록 함수 안에서 부르지 않는다 — 호출 문맥이 하나다). 티켓을 먼저 놓으면 끝났더라도 callback을 아직 부르지 않았으면 `Cancelled`다(처음 구현은 끝난 티켓의 보유만 놓고 결과를 성공으로 남겨, 전달 때 놓은 보유로 lease를 만들려다 Fatal이었다 — 리뷰 지적, 고침. 묶음 티켓은 처음부터 이 규칙이었다). `ReadyMask::Render`는 `UnsupportedCapability`.
   - 재진입: callback은 내부 표 순회가 끝난 뒤 모아 둔 목록으로 부른다. callback 안의 요청은 표와 전달 목록 용량을 늘릴 수 있으므로 목록은 색인으로 순회하고 기록은 색인으로 다시 찾는다(처음 구현은 range-for로 돌아 목록이 다시 잡히면 해제된 버퍼를 읽었다 — 리뷰 지적, 고침). (8)은 티켓 callback → 묶음 callback → 보류 명령 순이고 각 목록은 자기 차례에 정해진다 — 그래서 티켓 callback 안에서 낸 티켓의 callback은 다음 Pump, 묶음 callback과 보류 명령은 같은 Pump의 뒤 차례에서 부른다(어느 쪽이든 등록 함수 안에서는 부르지 않는다). callback 안에서 `RequestAsset`·`OnReady`·`Revoke`·티켓·lease 해제·`RunWhenReady`는 할 수 있고, `Pump`·`LoadBlocking`·`Shutdown`은 `WrongContext`다. 로더 콜백(PostLoad handler 등) 안에서는 로더 규칙(11.6절 18)대로 로더를 거치는 호출(`RequestAsset` 포함)이 `WrongContext`다.
6. **lease·티켓 슬롯과 해제 큐**(11.1절): `AssetLease`(move-only)는 단위 보유 1이다. 해제(소멸)는 계수를 바로 바꾸지 않고 해제 큐에 넣는다 — 큐 노드는 슬롯 안에 있고, 슬롯은 주소가 고정된 블록(고정 크기 블록을 이어 붙이고 옮기지 않는다)에 둔다. 큐는 뮤텍스 + 침입 연결이라 소멸자(noexcept, GC 회수 중 포함, 아무 스레드)에서 할당하지 않는다. 세대 비교와 계수 변경은 Game 스레드 Pump에서만 한다. scope Revoke·Shutdown이 회수한 슬롯은 회수 표시만 하고(보유는 그때 줄인다), 그 lease·티켓 객체가 소멸해 큐를 거친 뒤에야 슬롯을 다시 쓴다(늦은 소멸이 새 lease의 노드를 넣지 않는다 — 이중 감소 없음). `LoadTicket`·`PrimaryAssetTicket`·`PrimaryAssetLease`도 같은 슬롯 방식이다.
   - 상한: 슬롯 블록은 256칸, 블록 표는 4096칸(고정 크기 — 다른 스레드의 소멸이 Game 스레드의 표 확장과 경쟁하지 않는다)이라 동시에 살아 있는 handle 객체는 `AssetManagerMaxLiveHandles`(2²⁰)개까지다. 넘으면 새 요청·보류 명령·묶음 로드·동기 로드는 `CapacityExceeded`이고, 이미 걸어 둔 callback·보류 명령·묶음 callback은 lease를 만들 수 없어 그 오류를 받는다(조용히 버리지 않는다).
   - 전달 목록: callback·묶음 callback을 부르기 전에 모으는 목록의 용량은 티켓·묶음 표가 자랄 때 함께 잡는다(`TakeTicket`·`TakePrimary` — 잡지 못하면 그 요청이 `OutOfMemory`). 그래서 Pump·Shutdown의 전달은 할당하지 않는다. 처음 구현은 Pump·Shutdown에서 목록을 잡았고, 그 할당이 실패하면 Shutdown이 `ShuttingDown` 전달을 건너뛸 수 있었다(coverage 작업 중 발견 — 고침).
   - scope 회수와 해제 큐: 소멸했지만 아직 해제 큐에 남은 lease도 scope 회수가 회수한다(`objectAlive`는 큐를 처리할 때 칸과 함께 지운다). 큐 처리는 회수된 lease의 보유를 다시 빼지 않는다.
   - 수명: 6.1절 종료 순서(World 종료 → lease·scope 해제 → Registry 종료)가 계약이다. AssetManager를 파괴할 때 살아 있는 lease·티켓 객체가 있으면 `platform::Fatal`이다(큐가 사라진 뒤의 소멸을 조용히 넘기지 않는다 — Registry의 "남은 소유권을 무시한 파괴는 즉시 종료"와 같다). 버린 대안: 큐를 참조 계수 공유 상태로 AssetManager보다 오래 살리는 것(UE `TSharedPtr<FStreamableHandle>`) — 종료 순서 위반을 숨긴다.
7. **scope**(11.1절): `CreateScope()`가 `AssetScope`(move-only 소유 handle)를 준다. `Revoke()` 뒤 그 scope의 새 `RequestAsset`·`LoadPrimaryAsset`·`RunWhenReady`는 `ScopeClosed`다. 그 scope의 티켓은 진행 중이든 Succeeded든 관심을 놓고(로더 요청 Release), 아직 부르지 않은 callback에 `ScopeClosed`를 한 번 전달하며, 그 scope의 lease·보류 명령은 회수된다(보유 감소, 보류 명령은 실패 함수에 `ScopeClosed`). 다른 scope의 같은 에셋 lease는 그대로다. callback 안에서도 Revoke할 수 있다. scope handle이 소멸되면 Revoke와 같다.
8. **LoadBlocking**(11.1·11.5절 3): 프레임 판정이 참이거나 callback 안이면 `WrongContext`. 로더 콜백(PostLoad handler 등) 안이면 로더가 요청을 `WrongContext`로 거절해 그 오류를 돌려준다(`Pump`도 같다). 아니면 요청을 내고 로더 `WaitFor`(동기 대기 추적 — 호출 지점·대상·시간이 관찰자와 상태 조회에 남는다)와 AssetManager Pump 처리를 결과가 나올 때까지 되풀이해 lease를 준다.
9. **미준비 대상 정책**(11.5절 2): `RunWhenReady(asset, policy, scope, run, fail)` — 정책은 `Fail`(기본 — 대상이 상주 Resident(PostLoad까지 끝남)가 아니면 새 오류 `NotReady`, Core `ErrorCode`와 이름 함수에 더한다), `Defer`(Normal 우선순위로 요청해 준비되면 실행), `PromoteAndDefer`(High로 요청 — 이미 진행 중이면 승격), 동기 대기는 이 함수에 없다(`LoadBlocking`만). 보류 명령은 등록 때부터 실행까지 내부 티켓을 쥔다 — 대상 로드를 붙잡고(로드 중이 아니면 요청을 낸다), 상주면 보유 1이라 그 사이 Closing·Discard되지 않는다(그래서 "다른 인스턴스에 실행" 문제가 생기지 않는다). 실행은 늘 Pump (8)에서 한다(준비돼 있어도 등록 함수 안에서 실행하지 않는다). scope 회수는 그 scope의 아직 실행하지 않은 명령에 실패 사유(`ScopeClosed`)를 바로 적으므로 실행 때 scope를 다시 보지 않는다(처음 구현의 실행 직전 재확인은 닿을 수 없는 분기라 뺐다). 끝난 명령의 handle을 먼저 놓아도 명령은 실행되고 그 뒤 칸이 회수된다. 두 번째 `Cancel`·끝난 뒤 `Cancel`은 무동작 성공이다. 로드 실패·scope Revoke·Shutdown·handle 취소면 `run` 대신 `fail`(오류·진단)을 정확히 한 번 부른다(조용히 버리지 않는다). 큐 상한(`maxDeferredCommands`, 기본 4096)을 넘으면 `CapacityExceeded`. UE에는 대응하는 일반 정책이 없다(게임 코드가 `FStreamableDelegate`로 직접 한다) — 계획 요구에 따라 Rocks가 정한다.
10. **PrimaryAsset과 bundle**(계획 §24.2, UE `UAssetManager::LoadPrimaryAsset(id, bundles)` 대응): `LoadPrimaryAsset(asset, bundles, priority, scope)`는 매니페스트 항목이 PrimaryAsset인지(아니면 `InvalidArgument`) 보고, 이름마다 그 항목의 bundle이 있는지(caseless 키 — 10.1절 5, 없으면 `NotFound`) 본 뒤, 대표 에셋과 지정 bundle 구성원(bundle 사이 중복은 한 번)을 요청하는 묶음 티켓 `PrimaryAssetTicket`을 준다. 준비 callback은 구성 요청이 모두 끝나면 한 번이고, 성공이면 구성 lease 전부를 쥔 `PrimaryAssetLease`(bundle lease — 11.4절 정책), 하나라도 실패하면 처음 실패(구성 순서 — 대표 먼저, 그다음 AssetId 순)의 결과이고 이미 받은 lease는 놓는다. `PrimaryAssetTicket` 소멸은 구성 요청 전부를 취소한다. UE와 다른 점: UE는 일부가 실패해도 handle이 완료되고 실패한 에셋은 null이다 — Rocks는 6.2절 Hard 규칙(참조 대상 실패면 참조자도 실패)과 같은 엄격함으로 처음 실패로 끝낸다. 용량 audit(계획 §24.2): `EstimatePrimaryAsset(asset, bundles)`가 대표·구성원의 Hard 전이 폐포에 대한 매니페스트 packageBytes·bulkRawBytes 합을 준다(로드하지 않고 매니페스트만 읽는다, 합은 포화 덧셈 — bulkRawBytes는 u64 최댓값까지 받는다).
11. **관측**: `Describe(snapshot)`가 로더 상태(11.6절 12)에 상주 단위 표(구성·상태·보유 내역 — 티켓·lease·보류·로더 공개 보유·간선)와 보류 명령 수, `lateEdgesIntoDiscarding`을 더한다.
12. **종료**: `Shutdown()`은 먼저 종료 중으로 표시해 새 요청을 막고(`ShuttingDown` — 이어지는 전달의 callback·`fail` 안에서 낸 요청·보류 명령·묶음 로드·scope와 티켓·묶음 티켓의 `OnReady` 등록도 `ShuttingDown`이다 — 다시 전달할 Pump가 없으므로 받지 않고 callback은 caller에 남는다. 처음 구현은 전달 뒤에 표시해 그 안에서 낸 요청의 callback이 영영 불리지 않을 수 있었다 — 리뷰 지적, 고침) 모든 scope를 Revoke하고 아직 부르지 않은 callback·보류 명령 실패 함수에 `ShuttingDown`을 한 번 전달한 뒤 로더를 Shutdown하고(공개 패키지 RootRef를 놓는다) Registry를 끝낸다. 그 뒤 lease·티켓 객체의 소멸은 회수된 슬롯이라 무동작이다. AssetManager 파괴 때 lease·티켓 객체가 남아 있으면 `platform::Fatal`(6).
13. **시험 방법**: Discard Busy는 그 패키지 아래 공개 전 객체를 만든 `ConstructionContext`를 열어 두어 결정적으로 만든다(Discard 계약 — 부분 트리에 Reserved 객체가 있으면 Busy). 다른 스레드 해제는 스레드에서 lease 소멸 → Join → Pump(명시 순서). 시나리오 23·28은 상태(Resident·Closing·revive·Discarding·끝)와 전이마다 시험을 대응시킨다(14.1.7절 대조표).

### 11.8 구현에서 정한 점(A1e — EngineAssets)

A1e(`Engine/Source/Runtime/EngineAssets`, 상태: 구현 — 14.1.9·14.1.10절)가 3절의 연결 모듈과 11.4절(런타임 쿠킹 루트)·11.5절 3(프레임 판정 주입)·6.1절(GC·종료)을 다음과 같이 정한다. 애매한 곳은 UE 5.8을 따른다: 비동기 로드 진행은 World Tick 전(`UGameEngine::Tick`의 `StaticTick` → `ProcessAsyncLoading`, GameEngine.cpp:1854), 로드·GC 시간 예산은 console 변수(`s.AsyncLoadingTimeLimit` 기본 5 ms — CoreSettings.cpp:14, GC 증분 5 ms — GarbageCollection.cpp:310). 설계 교차 검증(리뷰어 1명, 필수 4·권장 8)을 반영했다.

UE와 다른 점과 이유:
- UE는 엔진이 늘 AssetManager를 만든다(`UEngine::InitializeObjectReferences`, 클래스가 틀리면 Fatal — UnrealEngine.cpp:3817–3834). Rocks는 게임이 Subsystem을 넣어야 생긴다: Engine은 AssetRuntime에 의존하지 않고(3절 계층) 쿠킹 콘텐츠 없는 헤드리스 실행이 있다.
- UE는 종료 전에 `FlushAsyncLoading`으로 대기 로드를 끝내고(LaunchEngineLoop.cpp:5053) AssetManager는 UObject 종료와 함께 사라진다. Rocks는 대기 요청을 취소(`ShuttingDown` 전달)하고 엔진 종료 단계 안에서 명시 순서로 끝낸다(6) — 종료가 남은 로드 시간에 기대지 않게.

1. **모듈**: `EngineAssets`(Runtime) → PublicDependencies {Engine, AssetRuntime}(Config·Task는 Engine이 공개 의존으로 준다 — 3절 그래프와 같다). Engine 층 기본 로딩 단계 `PreDefault`(ModuleLoader 3.11절 — 엔진 묶음)이고 진입점 파일과 `RegisterSettings`(CVar 등록 — LogHost 모듈 방식)를 둔다. 시험은 `EngineAssets/Tests`(EngineTests). AssetRuntime은 계속 Engine을 모른다.
2. **등록과 접근**: `EngineAssetsSubsystem : EngineSubsystem`이 `AssetManager` 하나를 소유한다. 게임은 `EngineConfig::subsystems`에 `EngineAssetsSubsystemType()`을 넣는다(엔진은 Subsystem을 스스로 더하지 않는다). 접근은 `engine.GetSubsystem<EngineAssetsSubsystem>()->Manager()`. 한 프로세스의 Engine이 여럿이면 각자 하나씩이고 에셋 Registry 문맥 id는 이 모듈 고정 상위 값(엔진 문맥·World 문맥과 겹치지 않는 값) + 프로세스 안 순번이다.
3. **설정**(CVar — `asset.*`, 등록 함수가 콘솔 이름 영역 `asset`을 먼저 더한다(`RegisterConsoleRegion` — 기본 표에 없다, TraceControl의 `trace`와 같은 방식, 두 번째 등록은 그대로 성공), Engine 층 모듈은 설정 struct를 등록할 경로가 없어 CVar로 둔다: HostSettings가 모듈 `RegisterSettings`를 엔진 묶음 타입 등록 전에 부르므로 리플렉션 설정 struct의 TypeId가 아직 없다. UE도 같은 값들이 console 변수다):
   - 부팅 고정(`cvar::Boot` — 시작 때 한 번 읽는다): `asset.cooked_root`(String, 빈 값이면 실행 파일 디렉터리 + `/Cooked` — 실행 파일 경로는 진입점이 캡처한 `ProcessCommandLine::ExecutablePath()`, 캡처 전이면 InvalidArgument), `asset.runtime_profile`(String, 빈 값이면 빌드 플랫폼 이름 `Windows`·`Mac`·`Android` — 10절 TargetProfile id. 전용 서버 타깃은 아직 없고(BuildTargets — TargetType Game·Program) 생기면 플랫폼 이름 + `Server`를 기본으로 더한다), `asset.loader.workers`(int32, 로더 worker 수, 기본 2), `asset.io.workers`(int32, 비동기 IO worker pool 스레드 수, 기본 2 — POSIX worker pool backend만 쓴다. Windows 기본 backend(Native IOCP)는 완료 스레드 하나라 무시한다, PlatformLayer AsyncIoContext), `asset.loader.max_packages_in_flight`·`asset.loader.max_temp_bytes`(11.6절 9 기본값).
   - 실행 중 조정(`cvar::Var` — 프레임마다 설정 sync 뒤 읽는다): `asset.pump.time_ms`(double, 기본 5 — UE `s.AsyncLoadingTimeLimit`), `asset.pump.max_units`·`asset.pump.max_post_loads`(무제한), `asset.pump.max_discards`(무제한), `asset.gc.time_ms`(double, 기본 5), `asset.gc.max_objects`(GcBudget 기본), `asset.gc.retry_seconds`(double, 기본 60 — 5).
   - 명령행은 따로 두지 않는다: `--set=asset.cooked_root=<dir>`(Config 세션 층 — 다른 설정과 같은 규약). 11.4절의 `-CookedRoot` 표기는 이것으로 바뀐다. 개발 실행은 프로젝트 설정 파일에 `{SavedDir}/Cooked/<profile>` 같은 값을 둔다(도구 `asset-load-cooked`와 같은 위치).
   - 설정 문맥이 필수다: CVar `Get()`은 이 스레드에 묶인 설정 snapshot을 읽고 묶음이 없으면 `platform::Fatal`이며(CVarSystem 7.1절), 부팅 값은 `CompleteStartup` 뒤 봉인된 snapshot을 읽는다. 그래서 `Initialize`는 먼저 `Engine::Settings()`가 null이면 `InvalidArgument`로 실패한다(기록 — "EngineAssets requires an engine config context"). CVar 등록은 모듈 `RegisterSettings`(호스트의 HostSettings가 부른다)가 하고, 시험은 같은 공개 함수 `RegisterEngineAssetsSettings(ConfigContext&)`를 직접 부른다. 등록 누락은 Policy `cvar-registration`이 빌드에서 잡는다.
   - 쿠킹 루트 결정(설정 값 → 실행 파일 기본)은 순수 함수 `ResolveCookedRoot(configured, executableDir, out)`이고 시험이 직접 부른다.
4. **초기화**(`Initialize(Engine&)` — 생성 순서, 멤버는 이 순서로 선언해 역순으로 소멸한다): 쿠킹 루트 결정 → 매니페스트(`<root>/CookedManifest.rsig` 서명 봉투가 있으면 그것만 읽어 `VerifyContentEnvelope`로 검증하고 같은 버퍼의 매니페스트 byte를 쓴다. 없으면 Release는 실패, 그 밖은 Warning 한 번 뒤 `<root>/CookedManifest.rman`을 읽는다 — A1f, [ContentSigning](ContentSigning.ko.md) 7절 4. 크기 상한은 봉투 `ContentEnvelopeMaxBytes`·rman `CookedManifestMaxFileBytes`) → `ReadCookedManifest`(검증 포함) → `platform::AsyncIoContext::Start(backend, asset.io.workers)` → `AsyncCookedPackageIo` → 로더 `WorkerScheduler`(`asset.loader.workers`) → `WorkerSchedulerLoaderWorker` → `AssetManagerConfig`(allocator = `engine.GetAllocator()` — 여러 스레드의 할당·해제를 받아야 한다는 로더 요구, TypeRegistry = 엔진 TypeRegistry, 프레임 판정 = `Engine::IsInFrame`, 로더 관찰자·PostLoad handler 없음 — 9) → `AssetManager::Initialize`. 실패하면 Initialize가 자기 부분 상태를 역순으로 되돌리고(실패한 Subsystem은 Deinitialize되지 않는다 — Subsystem 규칙) 엔진 초기화가 실패한다. 쿠킹 루트·파일 경로·오류·진단을 한 번 기록한다(`LogEngineAssets`, 영어). 매니페스트가 없는 것도 실패다(빈 콘텐츠로 돌지 않는다). 소멸자는 부분 초기화에도 안전하다.
5. **프레임**:
   - `OnFrameBegin`(엔진 3단계 — World Tick 전): `AssetManager::Pump`(CVar 예산). 그 프레임의 World Tick이 결과·callback을 본다(UE 순서). Pump 실패는 처음 한 번만 Error로 남기고 횟수를 센다 — 프레임 안 Pump가 실패하는 경우(AssetManager를 직접 Shutdown한 뒤 ShuttingDown 등)는 되돌아오지 않으므로 회복 기록이 없다.
   - callback은 엔진 접근 범위 안에서 불린다(Pump가 엔진 단계 안이다). World 객체를 다루는 callback은 스스로 그 World 접근 범위(`World::With`)로 들어간다. World 파괴 뒤에도 놓인 티켓의 `Cancelled`·종료의 `ShuttingDown`이 전달되므로 callback은 World 객체를 raw 포인터로 붙잡지 않는다(약한 참조 `WeakObjectRef` — 11.7절 5·12, 사용 규칙).
   - `OnFrameEnd`(엔진 6단계): 에셋 객체 Registry GC. 에셋 Registry 객체는 Discard로만 죽으므로(6.1절) Discard 주도다: Pump 결과 `releasedPackages > 0`(Discard를 마치고 로더 RootRef를 놓은 패키지가 있다 — 11.7절 3 Discarding. Discard만 하고 RootRef를 아직 쥔 패키지는 회수할 수 없으므로 `discardedPackages`로 걸지 않는다)이면 수집 요청 표시를 세운다. 표시가 있고 Registry가 Idle이면 `BeginCollection` 뒤 `StepCollection`(CVar 예산, scheduler는 `engine.GcScheduler()` — World GC와 같은 게임 스레드에서 차례로 쓴다), 진행 중이면 `StepCollection`만 한다. cycle 도중 RootRef를 놓은 패키지가 더 있으면 끝난 뒤 한 번 더 돈다. cycle이 끝났는데 Discard된 객체가 남아 있으면(`ObjectStats::destroyRequested` — 게임 코드의 RootRef·Borrow·하드 간선이 쥐고 있다) 엔진 프레임 시간(delta 합)으로 `asset.gc.retry_seconds`(기본 60 — UE `gc.TimeBetweenPurgingPendingKillObjects` 60 s, UnrealEngine.cpp:1777)가 지난 뒤 cycle을 다시 돈다(쥔 쪽이 놓았으면 그때 회수한다 — Discard 주도만으로는 다음 Discard까지 남는다). 회수 보유(`BackendLease` 등)가 풀린 Retired 객체의 `DrainRetired`는 에셋 Registry 객체에 그런 보유가 생기는 A5(텍스처 렌더 자원)에서 더한다. Busy(열린 `ConstructionContext`·Borrow)는 이번 프레임을 건너뛴다(정상 — 기록하지 않음). 그 밖의 실패(OutOfMemory)는 그 프레임만의 일이라 매번 기록하고 다음 프레임에 다시 한다. `Engine::RequestFullCollection`(World 객체용)에는 참여하지 않는다 — Discard가 없으면 에셋 Registry에서 더 회수할 것이 없다. 관찰용 `EngineAssetsSubsystem::Stats()`는 GC cycle·Busy 건너뜀·Pump 실패·GC 실패·병렬 표식 조각(엔진 GC scheduler를 넘긴 조각) 수, 수집 요청·재시도 대기 표지, 로더 worker 수(`asset.loader.workers` 전달 확인)다.
   - 프레임 판정 `Engine::IsInFrame()`: `RunFrame` 전체(1–6단계, 5단계 World 지연 파괴 포함) 동안 참인 새 표지다(기존 `inFrame_`은 5단계에서 거짓이라 쓰지 않는다). 프레임 안 `LoadBlocking`은 `WrongContext`(11.5절 3). 로딩 화면은 프레임 밖 초기화 단계나 `RequestAsset`을 쓴다.
6. **종료**(엔진 순서: BeginShutdown → client 종료 → World 파괴 → CompleteInFlightWork(Subsystem 역순 → hooks) → Subsystem 정리(역순) → 엔진 Registry 최종 GC → hook 정리):
   - `CompleteInFlightWork`: `AssetManager::Shutdown` — 아직 부르지 않은 callback·보류 명령에 `ShuttingDown`을 전달하고 로더가 진행 중 IO·worker를 기다린다(엔진의 "진행 중 비동기 작업 완료" 단계 계약). 결과는 엔진 Shutdown 결과로 보고된다.
   - `Deinitialize`: AssetManager 파괴 → loader worker·scheduler 종료 → IO 구성 종료 → AsyncIoContext 종료(4의 역순, 각 결과 기록).
   - 계약: lease·티켓·scope·명령 handle 객체는 EngineAssets Subsystem 정리 전에 소멸해야 한다 — World가 쥔 것은 World 파괴가, `EngineClient` 구현은 `OnEngineShutdown`까지(`OnEngineStart`가 실패를 돌려주면 `OnEngineShutdown`이 불리지 않으므로 그 반환 전에 — 리뷰 2 권장), `EngineLoopHooks`는 `CompleteInFlightWork`까지, EngineAssets에 의존하는 다른 Subsystem은 자기 Deinitialize(EngineAssets보다 먼저 불린다)까지 놓는다. 남으면 AssetManager 파괴 `platform::Fatal`(11.7절 6, 6.1절 종료 순서 계약). `AssetLease` 리플렉션 필드는 WorldOnly 클래스만 가진다(5.6절)라 엔진 Registry 객체는 lease를 필드로 쥐지 않는다.
   - 버린 대안: AssetManager 파괴를 엔진 Registry 최종 GC 뒤로 미루는 hook — 엔진 종료 단계를 늘리고, 그 사이 회수되는 객체의 handle 소멸이 Subsystem 정리 뒤 상태를 건드린다.
7. **Engine 변경**(작게, 공개 함수 둘과 표지 둘): `bool Engine::IsInFrame() const`(게임 스레드 — `RunFrame` 전체 표지, 5), `config::ConfigContext* Engine::Settings() const`(빌린 설정 문맥 — 없으면 null, 3), `Engine::Shutdown` 재진입 Busy(초기화 중 — Subsystem Initialize·client 시작 함수·초기화 실패 정리와 그 안의 callback — 이나 종료 단계 안 — 종료 hook·Subsystem·client 함수와 그 안에서 불린 callback, 예: `CompleteInFlightWork`나 초기화 실패 정리의 AssetManager 파괴가 전달한 `ShuttingDown` callback — 에서 부르면 종료 단계를 돌아 바깥 호출(Initialize·Shutdown)이 쓰는 상태를 지우므로 거절한다. 리뷰 두 번에서 찾았다 — 그 전에는 callback 안 재종료가 정리 중인 AssetManager에 닿아 Fatal이었다).
8. **시험**(EngineTests `EngineAssets/Tests` — 14.1.9절 대조표, 시간·경쟁에 기대지 않는다):
   - 픽스처: AssetRuntime의 `PackageWriter`·`WriteCookedManifest`로 시험 임시 디렉터리에 쿠킹 장면을 쓴다(AssetCooker 의존 없음). 설정 문맥에 `asset.cooked_root`를 넣어 엔진을 띄운다.
   - 결정적 로드 순서: 프레임 밖(Run 전) `LoadBlocking`으로 상주시키고(동기 — 완료까지 기다림), 프레임 안(client Tick)에서 같은 에셋을 `RequestAsset`하면 다음 프레임 Pump에서 callback이 World Tick 전에 불린다. 디스크 IO가 끝나는 프레임에 기대는 단언은 없다.
   - client Tick·World 지연 파괴 단계 안 `LoadBlocking` WrongContext, 프레임 밖 성공
   - GC: 놓은 패키지가 Discard된 뒤 GC 조각들로 회수됨(시간 예산 CVar를 크게, 객체 수 예산으로 조각 수를 정한다), cycle 도중 Discard면 한 번 더, 열린 `ConstructionContext` 프레임은 건너뜀, RootRef로 쥔 Discard 객체는 놓은 뒤 재시도 시간이 지나면 다음 cycle이 회수(고정 delta로 결정적)
   - 초기화 실패: 매니페스트 없음·손상·프로파일 다름(IncompatibleCook)·IO context 시작 실패·할당 실패의 모든 지점 → 엔진 초기화 실패, 기록 한 번, 누수 없음
   - `ResolveCookedRoot`: 설정 값 있음·없음, 실행 파일 디렉터리 기본
   - 종료: World Subsystem이 쥔 티켓은 World 파괴 뒤 다음 Pump에서 callback이 Cancelled, CompleteInFlightWork에서 대기 callback ShuttingDown, client가 놓지 않은 lease → 자식 프로세스 Fatal(문구 대조), 규칙대로 놓으면 정상 종료
   - CVar: 실행 중 Pump·GC 예산 변경이 다음 프레임부터 적용, 부팅 고정 값은 시작 뒤 바뀌지 않음
   - Pump 실패 기록(처음 한 번, 횟수), GC 조각 할당 실패(매번 기록, 다음 프레임에 다시)
9. **후속**: Rocks Lens 이벤트(로더 관찰자 → Trace, TraceSystem), 클래스별 PostLoad dispatch(A5 — handler를 여기서 연결), 쿠킹 결과를 배포 위치로 옮기는 패키징(후속).

## 12. 텍스처

### 12.1 타입 구성

- 계층은 UE 5.8과 같다: `Asset` → `StreamableRenderAsset`(추상) → `Texture`(공통 설정, 12.4.1절) → 차원별 `Texture2D`·`TextureCube`·`Texture2DArray`·`TextureCubeArray`·`VolumeTexture`(UE 5.8과 같은 다섯 클래스, 고유 설정은 12.4.2절).
- `StreamableRenderAsset`(UE `UStreamableRenderAsset`, Engine/Classes/Engine/StreamableRenderAsset.h:38): 텍스처와 뒤에 올 메시(UE `UStaticMesh`·`USkinnedAsset` — StaticMesh.h:602, SkinnedAsset.h:46)가 함께 쓰는 LOD·mip 스트리밍 기반이다. 메시가 나중에 같은 기반을 쓸 때 필드를 옮기지 않도록(필드 ID·직렬화가 바뀐다) 텍스처 단계에서 미리 나눈다(2026-11-17 사용자 결정).
  - 위치: AssetRuntime. 그래픽 의존 없는 데이터 기반이라 AssetRuntime의 의존 방향(3절)을 바꾸지 않는다.
  - 저장 설정: `NeverStream`, `bGlobalForceMipLevelsToBeResident`, `NumCinematicMipLevels`(UE StreamableRenderAsset.h:342–374, 상태는 12.4.1절 표). UE는 앞의 둘을 `uint8 : 1` bitfield로 두며, Rocks는 리플렉션 B9m(bitfield bool) 뒤라 같은 모양으로 둘 수 있다.
  - 실행 중 상태(UE는 모두 이 객체의 transient 멤버 — 상주·요청 LOD 수 `CachedSRRState`, `StreamingIndex`(duplicatetransient), 강제 상주 표시·시각, 시네마틱 mip 사용, 갱신 대기 표시, mip 변경 콜백, StreamableRenderAsset.h:300–402): 객체에는 게임 lane만 읽고 쓰는 transient **스트리밍 표 handle** 하나만 둔다(UE `StreamingIndex`). 나머지(상주 mip 수, 진행 중 전송·요청 세대, 갱신 대기, 강제 상주·시네마틱 mip 사용 판정, mip 변경 통지 구독)는 TextureStreaming 모듈의 표에 둔다([TextureStreaming](TextureStreaming.ko.md) 9절, 개정 3). 렌더 lane이 바꾸는 값을 게임 lane 객체에 쓰지 않기 위해서다(RenderBase 5절 lane 소유). 게임 쪽이 상주 mip 수를 읽을 때는 sync 지점에서 복사한 snapshot을 읽는다. 저장 설정 세 개는 상주 계획이 메타로 읽는다. Graphics 타입은 이 클래스에 들이지 않는다.
- 에디터 데이터 `TextureSource`(`ROCKS_STRUCT`, `ROCKS_PROPERTY(EditorOnly)` 필드): 형식·크기·slice 수·mip 수·layer 수·블록 목록·`BulkDataRef`(Zstd, raw SHA-256). 리플렉션으로 저장하므로 네이티브 직렬화 경로가 없다(5.6절). **원본 저장 형식은 디코딩한 정규 픽셀**이다(원본 파일 바이트는 저장하지 않는다 — UE는 PNG·JPEG 바이트를 그대로 둘 수 있다, 12.4.4절). 근거: 재임포트 없이 재빌드, 디코더 버전과 무관한 동일 입력, 빌드 경로에 원본 포맷 코드가 없음. 원본 파일 SHA-256은 ImportRecord에 있다.
- 플랫폼 데이터 `TexturePlatformData`(`ROCKS_STRUCT`, 쿠킹 데이터): dimension·format·sRGB·**swizzle**(출력 R·G·B·A 각각이 형식의 R·G·B·A·0·1 중 무엇인지 — 렌더러가 view swizzle로 적용, A0-G)·크기·mip 수·slice 수·mip별 `{ 크기(X·Y·Z), 바이트 크기(패딩 제외), 위치 }`(위치는 스트리밍 mip이면 그 mip의 덩어리 `BulkDataRef`, 상주 꼬리 mip이면 꼬리 덩어리 안 오프셋 — 한 mip의 모든 slice·면·깊이를 함께 담는다, [렌더 기반](RenderBase.ko.md) 6.5절 mip-major 배치)·상주 꼬리 덩어리 `BulkDataRef`·**상주 꼬리 mip 수**(쿠킹 때 계산, 12.5.1절 — 렌더는 다시 계산하지 않는다)·**샘플러**(텍스처·LOD 그룹에서 정한 Filter·MaxAniso·축별 주소·Border 색 — 렌더러가 그대로 쓴다).
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

2026-11-23 사용자 결정(2026-11-20 결정을 대체): JPEG 원본을 UE처럼 받는다. UE 5.8은 JPEG를 임포트할 때 기본 설정(`[TextureImporter] RetainJpegFormat` = True — BaseEditor.ini)에서 JPEG 바이트를 원본으로 보관하고 빌드 때 푼다(EditorFactories.cpp 2960–3006). Rocks는 받는 범위를 UE와 같게 하되 저장은 12.1절 원칙대로 **임포트 때 풀어 정규 픽셀로** 둔다 — 근거: 디코더 버전과 무관한 동일 빌드 입력, 빌드·쿠킹 경로에 원본 형식 코드가 없음(결정성 — 10절). 대가는 원본 크기(JPEG보다 크다 — Zstd로 줄인다)이고, JPEG 바이트 보관(UE `RetainJpegFormat` 대응 — 빌드가 결정적 정수 IDCT로 푼다)은 크기가 문제가 될 때의 후속 선택이다. 디코더는 외부 라이브러리 libjpeg-turbo다(ThirdParty, 버전 고정 — UE ImageWrapper와 같은 구현. 사용자 결정 2026-11-23: 구현 난이도가 큰 것은 외부 라이브러리, 간단한 것은 자체 구현 — progressive·Huffman·IDCT·업샘플링·마커 변형을 직접 만들고 검증하는 비용이 크다). 정수 IDCT `JDCT_ISLOW`, SIMD 없이 C 경로로 빌드한다(NASM 빌드 의존을 두지 않는다 — 임포트 때 한 번 푸는 용도라 속도보다 빌드 단순성, 결과는 정수 산술이라 호스트와 무관하다). ImageCore가 libjpeg-turbo 호출을 감싸 다른 원본 디코더와 같은 결과 형식(G8·RGBA8)·진단(`InvalidSource`·`UnsupportedSourceFormat`)으로 낸다. 다른 원본 디코더(BMP·TGA·PNG·PSD·DDS·TIFF·HDR·EXR)는 자체 구현 그대로다(zlib 등 범용 압축은 Core Compression). [Unreal 교환 형식](UnrealExchange.ko.md)(4.3절)으로 가져오는 JPEG·UEJPEG 원본 텍스처도 옵션 없이 받는다(해제한 픽셀 — `--allow-lossy-origin`·`LossyOriginAccepted`는 없앴다).

원본별 규칙:

- BMP: 16비트 BI_RGB는 555(최상위 비트 무시), 32비트 BI_RGB는 4번째 byte가 예약값이므로 알파 255로 읽는다. BI_BITFIELDS·BI_ALPHABITFIELDS는 마스크를 따르며, 알파 마스크는 BI_ALPHABITFIELDS 또는 V4·V5 헤더의 알파 마스크가 0이 아닐 때만 쓴다(BI_RGB에서는 V5의 알파 마스크도 무시). 마스크는 연속 비트여야 하고 서로 겹치지 않아야 하며(아니면 `InvalidSource`), 폭 w의 성분은 v × 255 / (2^w − 1)을 round-half-up해 8비트로 만든다(w > 8도 RGBA8로 줄인다). BI_JPEG·BI_PNG는 `UnsupportedSourceFormat`.
- TGA: image descriptor의 알파 비트 수(bit 0–3)가 0이면 4번째 성분·16비트의 attribute 비트를 무시한다(알파 255). TGA 2.0 확장 영역의 attributes type이 0–2(알파 없음·미정의·무시)면 무시, 3이면 일반 알파, 4(premultiplied)면 성분을 8비트로 확장한 뒤 straight 알파로 되돌린다(min(255, round-half-up(c × 255 / a)), 알파 0이면 색 0).
- PSD: 색 모드 RGB·Grayscale·Indexed·Bitmap을 받고 CMYK·Lab·Multichannel·Duotone은 `UnsupportedSourceFormat`. 병합(composite) 이미지 데이터를 쓴다 — image resource 0x0421(Version Info)의 hasRealMergedData가 false면(Maximize Compatibility를 끄고 저장) 레이어 합성은 지원하지 않으므로 `UnsupportedSourceFormat`. Indexed는 색표 크기를 resource 0x0416(색 수)으로, 투명 항목을 0x0417(투명 index)로 정한다(그 항목은 알파 0). 레이어·마스크 정보의 레이어 수가 음수일 때만 첫 추가 채널이 병합 결과의 투명도이므로 알파로 쓰고(PSD 규약), 그 밖의 추가 채널(저장된 선택 영역·spot 채널)은 `IgnoredSourceChannels` 경고와 함께 버린다.
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

대조 근거: UE 5.8(5.8.4, commit `f45729c0b6f0`) 원본의 헤더 — `Engine/Source/Runtime/Engine/Classes/Engine/`의 `Texture.h`(`UTexture`·`FTextureSource`·`FTextureSourceColorSettings`), `StreamableRenderAsset.h`(기반 `UStreamableRenderAsset`의 스트리밍 멤버), `Texture2D.h`·`TextureCube.h`·`Texture2DArray.h`·`TextureCubeArray.h`·`VolumeTexture.h`, `TextureDefines.h`(관련 enum), `TextureLODSettings.h`(`FTextureLODGroup`). 처음에는 UE 5.6 공식 API 문서(C++ `UTexture` 멤버 75개, `FTextureLODGroup` 멤버 29개, Python `unreal.Texture`·`Texture2D`·`TextureCube`·`Texture2DArray`·`TextureCubeArray`·`VolumeTexture` 편집 속성 — 2026-11-12 확인)로 작성했고, 2026-11-15에 5.8 원본으로 다시 확인했다. `UTexture`·`FTextureSource`·차원별 클래스의 데이터 멤버는 5.6과 같다(추가·삭제 없음). 5.8에서 바뀐 점은 해당 행에 "5.8에서 바뀜"으로 적는다: TextureGroup 82종, `ETextureSourceEncoding`의 `HLG` 추가, `FTextureLODGroup`의 품질 등급별 상한 4개 추가(33개). 속성 이름은 UE C++ 이름이며 Rocks 필드는 `ROCKS_PROPERTY(Name="…")`로 그 이름을 노출한다(5.6절, 명령행 이름). enum 값 이름은 UE 접두사(`TC_`·`TMGS_`·`TEXTUREGROUP_`·`TF_`·`TA_` 등)를 뗀 나머지 그대로다(예: `TC_HDR_Compressed` → `HDR_Compressed`). **계약** 설정은 스키마에 있고 기본값이 아니면 `UnsupportedSetting`이며, 사용자가 정한 순서(기반 → 화면 → 압축·보정·스트리밍)의 뒤 단계에서 구현한다.

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
| `bUseNewMipFilter`(5.6 공개 문서에는 의미가 "현대적 mip 필터, 끄면 옛 동작"으로만 있었다. 5.8 원본 주석: mip 필터만이 아니라 옛 텍스처 처리 경로와 개선 경로를 고르는 스위치이고, 표시 이름은 "Use Improved Image Processing", 멤버 기본값은 false지만 새 텍스처·재임포트 때 `SetModernSettingsForNewOrChangedTexture`가 켠다 — Texture.h 1376–1382, 5.6도 같다) | 계약(기본값 false만 받는다. 의미는 보정 단계에서 UE 동작과 대조해 확정) |
| 보정: `AdjustBrightness`·`AdjustBrightnessCurve`·`AdjustVibrance`·`AdjustSaturation`·`AdjustRGBCurve`·`AdjustHue`·`AdjustMinAlpha`·`AdjustMaxAlpha`, `bChromaKeyTexture`·`ChromaKeyThreshold`·`ChromaKeyColor`, `bDoScaleMipsForAlphaCoverage`·`AlphaCoverageThresholds`, `CompositeTexture`·`CompositeTextureMode`(`Disabled`·`NormalRoughnessToRed/Green/Blue/Alpha`)·`CompositePower`, `Downscale`(플랫폼별 — `OrderedMap<Name, float>`를 담은 struct, 리플렉션 B9f·B9c)·`DownscaleOptions`(`Default`·`Unfiltered`·`SimpleAverage`·`Sharpen0`–`Sharpen10`) | 계약 |
| 색 관리: `SourceColorSettings`의 그 밖 `EncodingOverride`(`ST2084`·`Gamma22`·`BT1886`·`Gamma26`·`Cineon`·`REDLog`·`REDLog3G10`·`SLog1`–`SLog3`·`AlexaV3LogC`·`CanonLog`·`ProTune`·`VLog`. 5.8에서 바뀜: `HLG`(17)가 더해졌다 — 이 표에는 아직 분류하지 않았다), `ColorSpace`(`Rec2020`·`ACESAP0`·`ACESAP1`·`P3DCI`·`P3D65`·`REDWideGamut`·`SonySGamut3`·`SonySGamut3Cine`·`AlexaWideGamut`·`CanonCinemaGamut`·`GoProProtuneNative`·`PanasonicVGamut`·`Custom`), 색도 좌표 4개, `ChromaticAdaptationMethod`(`None`·`Bradford`·`CAT02`) | 계약 |
| 압축: `CompressionYCoCg`, `LayerFormatSettings`(층별 형식 설정, UE 내부), `LossyCompressionAmount`(7종), `CompressionQuality`(ASTC 6종), `bOodlePreserveExtremes`, `OodleTextureSdkVersion`(UE FName — Name 필드, 리플렉션 B9c) | 계약 |
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

**메시 LOD 그룹**(메시 에셋 스펙 MR9, 2026-11-20). 메시도 텍스처처럼 그룹 이름(`lodGroup`)으로 TargetProfile의 기본값을 받는다([MeshAnimationAssets](MeshAnimationAssets.ko.md) 4.7절). TargetProfile에 텍스처 LOD 그룹 표와 나란히 메시 LOD 그룹 표를 둔다. 항목은 UE 5.8 `FStaticMeshLODGroup`(Engine/Public/StaticMeshResources.h:58)의 멤버에 대응한다: 기본 LOD 수(`DefaultNumLODs`), 스트리밍·선택 LOD 수 상한(`DefaultMaxNumStreamedLODs`·`DefaultMaxNumOptionalLODs`), LOD 스트리밍 지원(`bSupportLODStreaming`), 기본 라이트맵 해상도(`DefaultLightMapResolution`), LOD별 기본 축소 설정(`DefaultSettings[]`)과 축소 설정 보정(`SettingsBias`·`BasePercentTrianglesMult`·`BasePercentVerticesMult`). 화면 크기·최소 LOD는 그룹이 아니라 메시 에셋의 플랫폼별 값이다(텍스처와 같은 덮어쓰기 표). 축소 관련 항목은 축소기가 생길 때까지 보존 상태다(MeshAnimationAssets 4.2절).

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

2026-11-20 렌더 세션 요구([텍스처 스트리밍](TextureStreaming.ko.md) 4.3·4.4·11절, [렌더 기반](RenderBase.ko.md) 6.5절)를 쿠킹 규칙으로 정한다. 렌더는 상주 mip 수를 다시 계산하지 않고 아래 메타를 쓴다. N = 쿠킹 뒤 mip 수(12.5절 4번에서 d개를 버린 뒤), X·Y = 쿠킹 뒤 상위 mip의 크기다.

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

## 13. 명령행(Programs/Commandlet)

레지스트리 이름은 단일 토큰이라 모든 명령을 하이픈으로 이은 한 단어로 등록한다(`asset-<대상>-<동사>` 또는 `asset-<동사>`). 미커밋 `import-texture`는 `asset-import`로 대체한다.

```
asset-import <path> <source> [--cube-layout=<배치>|--volume-tile=<W>x<H>] [설정…]
asset-import <path> (--cube=<cube 지정>|--array=<파일 목록>|--cube-array=<cube 지정>;…|--volume=<파일 목록>) [설정…]
asset-import <path> --source.<역할>=<파일>… [--importer=<이름>] [설정…]
asset-reimport <path> [--source=<파일> | --source.<역할>=<파일>…] [설정…]
asset-new <Type> <path> [<속성>=<값>…]
asset-set <path> <속성>=<값> [<속성>=<값>…]
asset-list [--type=<Type>] [--under=<path>]
asset-info <path|AssetId> [--json] [--all | --import]
asset-move <path|디렉터리> <새 path>
asset-duplicate <path> <새 path>
asset-delete <path> [--force]
asset-fix-duplicate-id <path>
asset-index-rebuild
asset-index-verify
asset-alias-list
asset-alias-remove <옛 path>
asset-alias-prune
asset-orphans-list
asset-orphans-delete <path>
asset-cook --profile=<id> [<path>…] [--verify-nuggets] [--verify-nuggets-source=local|remote]
asset-load-cooked --profile=<id> <AssetId|경로 힌트>
nugget-gc --max-bytes=<n> --unused-days=<n>   (Rockpile backend면 `rockpile gc`로 위임 — --max-bytes → maxBytes, --unused-days → maxUnusedDays)
공통 옵션: --project=<dir>(기본: 현재 디렉터리에서 위로 *.rocksproject 탐색), --wait=<초>(쓰기 잠금 대기, 기본 0)
```

문법:

- 옵션은 값이 있는 `--<이름>=<값>`과 값 없는 bool 플래그 `--<이름>`(`--force`·`--verify-nuggets`) 두 형태다. 공백으로 나눈 값·짧은 옵션은 없다.
- 설정(`[설정…]`)은 `--<속성>=<값>`, `asset-new`·`asset-set`의 속성은 `<속성>=<값>`이다. 값 표기·해석은 리플렉션 공용 codec `ReflectionText`가 맡는다([리플렉션 B9](ReflectionExtensions.ko.md) 10절 — 대입은 Prepare로 모두 검증한 뒤 한꺼번에 게시하고, 오류는 모두 모아 보고한다). `<Type>`은 타입 도구 이름(한정하지 않은 C++ 이름, 충돌하면 `Name="…"` — ReflectionExtensions 5.2절)이다. 속성 이름은 `ROCKS_PROPERTY(Name="…")`로 노출한 이름(텍스처는 UE C++ 이름, 대소문자 무시)이고 구조체 필드는 `<속성>.<필드>`(예: `SourceColorSettings.ColorSpace=sRGB`)다. 모르는 이름은 `UnknownProperty`.
- 값: enum은 열거자 이름(대소문자 무시, UE 접두사는 붙여도 되고 빼도 된다 — 12.4절), 정수·실수는 10진, bool은 `on`·`off`·`true`·`false`, 문자열은 그대로, 참조는 논리 경로 또는 36자 AssetId(`none` = null), 배열은 쉼표 목록(빈 배열은 빈 값). 리플렉션 B9가 더하는 표기(ReflectionExtensions 10.4절): Map은 `k:v` 쉼표 목록(처음 나오는 escape되지 않은 `:`에서 나눔), Flags enum은 `A+B`, Map 항목·Set 원소는 경로 key 단계 `<속성>[키]`(특수 문자가 있을 때만 큰따옴표), struct 기본값은 `{}`, Optional unset은 `--unset=<경로 목록>`, Map 키·Set 원소 제거는 `--erase=<경로 목록>`(목록 옵션이라 같은 옵션 두 번에 해당하지 않는다), `none`은 참조·클래스 참조·InstancedStruct의 null 전용이다. 대입은 `[...]`·따옴표 밖의 첫 `=`에서 나눈다. `asset-info` 기본 출력은 `asset-set`에 그대로 넣을 수 있는(값 보존) `<경로>=<값>` 줄이고, `--json`은 JSON, `--all`은 보고용 전체 출력이다(ReflectionExtensions 10.4절).
- 목록 구분자는 `,`, `--cube-array`의 cube 구분자는 `;`, `--cube`의 면 이름 구분자는 `:`(앞 2자 `px`…`nz`만 면 이름으로 본다). 값 안의 `\,`·`\;`만 이스케이프(→ `,`·`;` — 리플렉션 B9가 Map 키 안의 `\:`와, 경로 key 따옴표 안의 `\\`·`\"`·`\n`·`\r`·`\t`(UE `ParseQuotedString`의 escape 가운데 다섯)를 더한다 — ReflectionExtensions 1.5절)이고 그 밖의 `\`는 글자 그대로다(Windows 경로 `C:\Art\a.png`·UNC `\\server\share\a.png`는 그대로 쓴다). 논리 경로는 경로 정책상 `,`·`;`·`\`를 가질 수 없다. `;`가 든 인자는 셸 구분자이므로 따옴표로 감싼다.
- 위치 인자 원본은 하나뿐이다. `--cube-layout`·`--volume-tile`은 위치 인자 원본과 함께, `--cube`·`--array`·`--cube-array`·`--volume`은 위치 인자 원본 없이 쓴다.
- `asset-reimport`의 원본 재지정: 원본이 하나면 `--source=<파일>`, 여럿이면 역할마다 `--source.<역할>=<파일>`(역할은 5.4절: `px`…`nz`, slice `0`…, CubeArray `<i>.<면>`). 기록에 없는 역할은 `InvalidImportOption`.
- 종료 코드: 0 성공(경고만 있어도 0), 1 실행 실패(진단은 표준 오류에 `<진단 코드>: <path>: <설명>` 한 줄씩 — 임포터가 거부한 옵션 값은 `InvalidImportOption`), 2 사용법 오류(모르는 명령·옵션, 같은 옵션 두 번, 값이 필요한 옵션에 값 없음, 위치 인자 수·조합 오류).
- 쓰기 잠금(8절)을 잡는 명령: `asset-import`·`asset-reimport`·`asset-new`·`asset-set`·`asset-move`·`asset-duplicate`·`asset-delete`·`asset-fix-duplicate-id`·`asset-alias-remove`·`asset-alias-prune`·`asset-orphans-delete`·`asset-index-rebuild`(캐시 기록)·`asset-cook`(쿠킹 동안 에디터 패키지가 바뀌지 않게, 쿠킹 출력 보호). 나머지는 잠금 없이 읽는다(캐시는 읽기만).

서비스 API(`AssetService`, AssetDatabase)는 명령과 1:1 함수이며 에디터가 나중에 같은 함수를 쓴다. A2 명령은 그 서비스 API가 생기는 하위 단계에서 함께 더한다(7.1절 4 표).

## 14. 검증 시나리오

1. PNG 임포트·저장 → 새 프로세스에서 같은 AssetId·픽셀
2. 원본 PNG 삭제 후 에디터 패키지만으로 쿠킹
3. 원본 변경 재임포트: ID·참조 유지, 해시 갱신, 에셋 너겟 무효화
4. 잘못된 PNG·원본 누락·잘못된 옵션 재임포트 실패 시 기존 패키지 바이트 불변
5. 이동·이름 변경(디렉터리 포함) 후 ID 참조 유지, 별칭 해석·경고·만료
6. 복제본 새 ID·내부 참조 재매핑
7. 중복 ID·깨진 참조·TypeMismatch·UnsupportedCycle 진단, 하드 순환 SCC 로드
8. 인덱스 캐시 삭제 후 재구축, 에디터 밖 이동·삭제 후 재검색, 감시 Overflow·RootLost 복구
9. 에셋 너겟 적중·설정 변경 무효화·툴체인 지문 변경 무효화·세대 변경 무효화·손상 복구·두 프로세스 동시 기록·GC(A3는 `NuggetFileSystemBackend` 경로 — Rockpile 경로는 Rockpile R1a가 같은 시나리오를 `NuggetRockpileBackend`로 다시 통과한다)
10. mip 크기·대표 픽셀(12.5절 표: sRGB·알파·홀수·Unfiltered·PreserveBorder·FlipGreen·NoAlpha(비압축 결과 불변)·Normalize·Volume·Cube)
11. 쿠킹 패키지만으로 로드, EditorOnly 데이터·필드 부재
12. 잘못된 패키지(magic·CRC·섹션 순서·겹침·범위·크기)·UnsupportedVersion·NewerVersion
13. 쿠킹 재현성(에셋 너겟 빈/찬, 두 번 쿠킹 동일 바이트, Debug·Release 도구 사이 동일 바이트 — Rockpile 경로는 R1b), 부분 쿠킹 병합·전체 쿠킹 정리, 실패 시 게시본 불변
14. 저장 실패 주입 시 기존 보존, 열린 대상 교체, 잠금 Busy, ExternalModification
15. 경로 정책: NFC/NFD 같은 이름, 대소문자 중복, 예약 이름, 길이, CaseCollision
16. 소프트 참조 비자동 로드, 요청 합류·개별 취소, OnReady 중복 AlreadyRegistered, scope Revoke·ScopeClosed, 의존 retention, lease 해제 후 회수
17. PlatformBase A0-P: DirectoryWatcher 8건·파일 내구성 8건 Windows 통과(깊은 작업 디렉터리 포함), Android 빌드, macOS 미검증. SyncDirectory 시험(디렉터리·끝 구분자·현재 디렉터리·디렉터리 link 성공, 파일·끝 구분자 붙인 파일·파일 link WrongType, 없는 경로·파일을 지나는 경로 NotFound, 잘못된 경로 InvalidArgument, wrapper 기본 Unsupported) Windows 통과
18. PNG 적합성: PngSuite 대조
19. UnsupportedSetting(계약 설정 값), NeverCookReferenced, LossySource 거부
20. 다중 출력: 시험 전용 임포터(원본 하나 → 출력 둘)로 출력 대응 유지·사라진 출력 OrphanedOutput 표시·명시 삭제
21. Cube·2DArray·Volume 임포트·쿠킹·로드(원본 구성별)
22. 매니페스트 게시: 교체 전후 reader가 일관된 집합만 보는지, 쿠킹 실패 주입 시 이전 매니페스트 유지, 부분 쿠킹 `IncompatibleCook`
23. 수명: Closing 직후 같은 ID 재요청이 GC 전에 새 로드로 성공(Discard), Closing 대기 중 재요청 revive, 하드 순환 SCC 단위의 해제, 로드 중 의존 단위 보유, 동적 하드 간선의 계수 반영, lease 해제 지연
24. 12.4절 표: 모든 계약 설정에 기본값이 아닌 값 → `UnsupportedSetting`, 숨김 CompressionSettings → `InvalidSetting`, 선형 설정·float 출력 + SRGB on → `SrgbIgnored`, CompressionSettings 15종 × 원본 형식의 쿠킹 형식·swizzle, sRGB 출력 규칙, LOD 그룹 합성·필터 조합
25. 결정적 수학 기준값·CRT 초월 함수 미사용 링크 검사, 서로 다른 호스트·빌드 구성의 쿠킹 바이트 비교
26. 원본 규칙: BMP 32비트 BI_RGB 알파 무시·V5 알파 마스크, TGA 알파 비트·확장 영역(premultiplied), PSD 색 모드·hasRealMergedData·추가 채널, PNG 회색 16비트 + tRNS
27. 매니페스트 대조: 프로파일 불일치 `IncompatibleCook`, 크기·SHA·의존 불일치 `CorruptPackage`, 손상된 게시 파일의 재게시
28. 순환 키: 하드 순환 A↔B·3-node 순환의 cookKey와 Build 순환의 너겟 키가 정의되고, 구성원 하나를 바꾸면 SCC 전체의 키가 바뀐다. 동적 간선으로 생긴 순환의 단위 병합과 해제, 간선 다중도(필드 둘 중 하나 제거)
29. 저장: 이전 파일의 손상 bulk entry 복사 거부, 저장소 id 표 다시 잇기 후 지연 읽기, 저장 중 편집 시 재연결 생략, 대체 경로(FAT 등)에서 자기 핸들 때문에 실패하지 않음

### 14.1 시험 fixture 에셋(A1부터)

**결정적 AssetId 발급기**(Unreal 교환 UXR6, 2026-11-20). 임포트 결과의 bytes 결정성을 비교하는 시험(같은 입력을 두 번 임포트)은 무작위 AssetId 때문에 그대로는 비교할 수 없다. ObjectImports가 AssetId 순으로 정렬되므로 ID만 빼고 비교해도 bytes가 다르다. 그래서 AssetDatabase에 시험 전용 AssetId 발급기 주입 지점을 둔다(시드로 정해지는 순서열, 시험 빌드에서만 노출 — 기존 시험 접근 정책과 같은 방식). 제품 경로의 발급은 무작위 v4 그대로다(4.1절).

에셋 시스템 시험은 실제 에셋 타입(A5의 Texture 계열 등)이 생기기 전에도 직렬화·로드 경로를 그대로 타도록 시험 전용 리플렉션 에셋 클래스를 쓴다. 위치는 `Engine/Source/Developer/AssetTestFixtures`(EngineTests 전용 header-only fixture 모듈, identity 명세 `AssetTestFixtures.rocksids.json`), 결정적 값 생성기·필드 비교 함수는 `Engine/Source/Runtime/AssetRuntime/Tests/AssetFixtureValues.h`다. 새 에셋 타입(A5 Texture 등)도 같은 방식으로 "Fill(seed) → 저장 → 다른 Registry에 로드 → Check(seed) 필드 비교 → 다시 저장해 byte 동일"을 붙인다(`FixtureAssetTests.cpp`의 `RunRoundTrip`).

| fixture | 단계 | 담는 것 | 시험 |
|---|---|---|---|
| `SimpleAsset` | 단순 | int32·int8·uint64·float·double·bool·Name·String·enum·Guid | `FixtureAsset_SimpleRoundTrip` |
| `ValueAsset`(+`FixturePoint`·`FixtureGroup`·`FixturePayload`) | 값 묶음 | 중첩 struct·struct 배열·Optional 멤버, 정적 배열, Array, OrderedMap·OrderedSet, Optional(String·struct), Text, Bytes(`Array<uint8>`), InstancedStruct, TangentCurve, `InterpCurve<float>`, Transform·Vec3·Rotator(UE 단위: cm·도) | `FixtureAsset_ValueRoundTrip` |
| `BulkAsset` | 큰 데이터 | 4 MiB 압축되는 무늬·2 MiB 압축되지 않는 무늬·0 byte bulk·bulk 배열(같은 ref 두 번) | `FixtureAsset_BulkRoundTrip`(None·Zstd × 에디터·쿠킹) |
| `BaseGameAsset`·`DerivedGameAsset` | 상속 | 기반 필드·파생 필드 | `FixtureAsset_InheritanceRoundTrip` |
| `ValidatedAsset`(+`FixtureRange` Validate hook) | 검증 hook | 필드 struct·struct 배열 원소의 hook | `FixtureAsset_ValidateHook`(유효 왕복, 거부 값은 로드가 CorruptPackage) |
| `TestAsset`·`TestPart`·`TestScratch`·`TestRequiredAsset` | 참조·구조 | 다른 패키지 Hard(단일·`ObjectRefArray<Object>`)·Soft·EditorOnly Hard·EditorOnly 값·bulk, 하위 객체 사슬(`Body.Socket`)·같은 패키지 안 참조, 저장 불가(Transient) 하위 객체, Required Hard | `AssetPackage_*`(PackageFormatTests·PackageFuzzTests) |
| `TestHookAsset` | 사용자 코드 hook | 생성자·소멸자가 시험이 건 hook(`ActiveTestHookAssetHooks()`)을 부른다, 기대 타입이 TestAsset인 Hard 참조 하나(오래된 의존 쿠킹 결과로 공개를 실패시켜 롤백 소멸자에 닿는다) | `AssetLoader_TransactionCodeCannotReenter` |
| `GameDataAsset`·`GameComponent`·`FixtureEntry` | 복합 | 하위 객체 여러 개(순환 사슬)·다른 에셋 Hard 배열과 하위 객체의 다른 패키지 참조·Soft·weak·커브·bulk·struct 배열·Map·hook struct | `AssetPackage_CompositeGameDataRoundTrip` |
| `ComboAsset`·`ComboEditorAsset`·`ComboRequiredAsset`·`ComboPart`·`ComboPartDerived`(+종류별 `Combo*` struct) | 모든 조합 | 아래 "지원하는 모든 조합" | `FixtureAsset_CombinationsCoverPropertyTable`·`FixtureAsset_CombinationRoundTrip`·`FixtureAsset_CombinationRequired` |

**지원하는 모든 조합**(`AssetTestFixtures/CombinationAssets.h`, 생성기·비교기 `Tests/AssetCombination.h`): 리플렉션 성질 표(ReflectionExtensions 1.3절, `Meta/TypeSchema.h` `ValueKindProperties`)의 값 종류마다 struct 하나(`ComboBool`…`ComboText`)가 그 종류를 표가 허용하는 모든 원소 자리에 둔다 — `value`(struct 멤버), `array`(Array 원소), `fixed`(정적 배열 원소), `keyed`(Map 키), `mapped`(Map 값), `set`(Set 원소), `maybe`(Optional 내부). 멤버 이름이 종류마다 같아 생성기·비교기 템플릿 하나가 모든 종류를 채우고 비교한다. 클래스 필드 자리는 `ComboAsset`의 `f*` 필드(종류마다 하나, 클래스 필드 자리의 컨테이너 다섯, ObjectRef 자리 — `ObjectRefArray`·정적 배열·`ObjectRefMap`·참조 struct·`StructRefArray`), EditorOnly 실효 플래그는 같은 이름·모양의 `ComboEditorAsset`(모든 필드 EditorOnly — 쿠킹에서는 기본값, 에디터에서는 왕복)이 덮는다. 표 밖 축:
- 참조 종류: ObjectRef·WeakObjectRef 값은 대상 표 7칸(null, 같은 패키지 대표·하위·깊은 하위, 다른 패키지 대표·하위·깊은 하위)을 돌아가며 가리키고(weak의 다른 패키지 대상은 저장이 null로 바꾼다), Soft는 null·다른 패키지 대표·하위·깊은 하위·없는 패키지·자기 패키지 하위, ClassRef는 null과 Asset 파생 클래스 다섯이다.
- 지정자: Required는 그것을 받는 종류(ObjectRef·ClassRef·SoftObjectRef) × 그 종류의 모든 자리(필드·struct 멤버·Array·정적 배열·Map 값·Set)에 `ComboRequiredAsset`이 두고, 칸 하나씩 null이면 쿠킹·에디터 저장이 거부한다. bitfield(클래스 필드·struct 멤버, uint8·16·32 폭), 검증 hook(`ComboChecked` — 필드·멤버·Array·정적 배열·Map 값·Optional·InstancedStruct 내용).
- 참조 struct(`ComboRefLink` — 필드·멤버·`StructRefArray`·정적 배열)와 Registry 컨테이너 struct(`ComboObjectRefs`), Flags enum(`ComboMask`, 모든 enum 자리).
- 중첩 깊이: `ComboTop` → `ComboMiddle` → `ComboLeaf`(Array·Map·Optional 안 struct 안 Array·Set).
- 클래스 구조: Asset이 아닌 하위 객체(`ComboPart`)와 그 파생(`ComboPartDerived` — 기반 필드 + 파생 필드), 하위 객체에서 같은·다른 패키지 참조.
- 왕복 축: 쿠킹·에디터 × 홀수·짝수 seed(Optional 있음·없음, bulk None·Zstd, InstancedStruct·Text 세 형태) × 두 클래스, 같은 상태 두 번 쓰기 byte 동일, 로드 → 필드 비교(다른 seed의 기대값과는 달라야 한다) → 재저장 byte 동일.

빠짐 검사 `FixtureAsset_CombinationsCoverPropertyTable`은 조합 클래스(`Combo*` — 다른 fixture가 칸을 대신 채우지 않게 이것만)의 스키마(기반 클래스·struct 멤버 재귀)를 순회해 (값 종류, 자리, EditorOnly 실효) 칸, Required 칸, 검증 hook·참조 struct·Flags enum 자리, 중첩 깊이 3 이상을 모으고 성질 표의 허용 칸과 하나라도 다르면 빠진 칸 이름으로 실패한다(표에 행·열이 생기면 fixture를 고치라는 신호). InstancedStruct 내용 열(Struct만 ○)은 실행 중 값이라 스키마에 없으므로 그 열의 모양(Struct만)과 InstancedStruct 칸의 존재를 확인하고, 실제 내용(`ComboPayload`·hook `ComboChecked`)은 왕복 비교가 본다. 표가 막는 칸은 ToolTests `Meta.EveryRejectedValueKindCellIsRejected`가 C# 표(`ValueKindTable.Rows`)를 순회해 원소 자리 × 종류의 모든 × 칸을 원본 선언으로 만들고, MetaCompiler 진단의 코드 집합이 칸마다 정한 코드 하나와 같은지 본다(Array 원소 RMC2304, Map 키·값·Set 원소 RMC2305, Optional 내부 RMC2306, 정적 배열 원소 RMC2307, C 배열을 템플릿 원소로 쓰는 칸만 Clang 단계 RMC3000이고 그 사유가 컨테이너의 static_assert). 명세 누락(RMC2100)·입력 오류(RMC1xxx)·엉뚱한 Clang 오류로 거부되면 실패다. 대조 시험 `Meta.EveryValueKindSpellingIsAcceptedWhereAllowed`는 같은 preamble·명세·철자로 표가 ○인 자리(클래스 필드 + 허용 원소 자리)를 모두 선언해 생성에 성공시킨다 — 처음 시험은 probe 명세에 `probe::Tone`·`probe::Holder::cell`이 없고 `Holder`에 Allocator 생성자가 없어 컨테이너 칸이 모두 "기본 생성자 삭제" RMC3000으로 거부되는데도 "error RMC"만 보고 통과했다. `Meta.EveryRejectedValueKindCellHasASpelling`은 새 종류 행에 시험 철자가 빠지지 않게 하고, 표가 쓰는 성질 비트가 모두 원소 자리·필드·멤버·InstancedStruct 내용·자리가 아닌 성질로 분류되는지(새 자리 열이 생기면 실패) 본다. C++ 빠짐 검사도 같은 분류를 static_assert로 건다(필드·멤버 자리는 Opaque 말고 모두 ○라 거부 칸이 없다).

#### 14.1.1 요구 대조표(A1a — 패키지 포맷 v1)

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/AssetRuntime/Tests` — 정의된 시험 79개(AssetPackage 71·FixtureAsset 8)를 모두 표가 인용한다, ObjectRuntime `ConstructionFindChild_FindsOneComponentUnderAnyVisibleOuter`, ToolTests meta 다섯). "범위 밖"은 A1a가 만들지 않는 것(다른 단계가 맡는다), "시험 불가"는 이유를 적은 것이다. 시험이 없는 항목은 0개다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 헤더(5.1) | 크기 < 64(헤더 byte·Stat 크기·빈 입력) | `AssetPackage_HeaderFields` |
| | magic 네 byte 각각 | `AssetPackage_HeaderFields`, `AssetPackage_CorruptionAndVersionChecks` |
| | headerCrc(칸 자체·덮는 범위 0–59) | `AssetPackage_HeaderFields` |
| | flags(0·둘 다·정의되지 않은 비트, 버전보다 먼저 판정) | `AssetPackage_HeaderFields` |
| | formatVersion(더 새 것: 쿠킹 UnsupportedVersion·에디터 NewerVersion, 최댓값, 더 옛 것 UnsupportedVersion — 에디터·쿠킹 0) | `AssetPackage_HeaderFields`, `AssetPackage_HeaderUpperBounds` |
| | headerBytes ≠ 64 | `AssetPackage_HeaderFields` |
| | sectionCount(0·33·실제와 다름) | `AssetPackage_HeaderFields` |
| | sectionTableOffset(헤더 안·8 정렬 아님·64 GiB 초과·실제와 다름) | `AssetPackage_HeaderFields`, `AssetPackage_HeaderUpperBounds`(식·Stat이 맞는 64 GiB + 8) |
| | fileBytes(식·Stat 크기·덧붙음·잘림·64 GiB 초과 — 식·Stat이 맞는 64 GiB + 36 포함), 파일을 모든 길이로 자름(쿠킹·에디터) | `AssetPackage_HeaderFields`, `AssetPackage_HeaderUpperBounds`, `AssetPackage_TruncatedSections` |
| | assetId(null 거부, 다른 id는 그 id로 읽힘) | `AssetPackage_HeaderFields` |
| | bulkAlignment(0·2의 거듭제곱 아님·8 미만·65536 초과, 경계 8·65536·기본값) | `AssetPackage_HeaderFields`, `AssetPackage_BulkAlignmentBounds` |
| | reserved 8 byte 각각 | `AssetPackage_HeaderFields` |
| | 기대 종류(쿠킹 기대 + 에디터 = UnsupportedVersion, 반대 = CorruptPackage) | `AssetPackage_HeaderFields`, `AssetPackage_EditorRoundTripKeepsEditorOnly` |
| 섹션 표(5.1) | tableCrc | `AssetPackage_SectionTableFields` |
| | kind 순서·중복 | `AssetPackage_SectionTableFields`, `AssetPackage_CorruptionAndVersionChecks` |
| | flags 정의되지 않은 비트 | `AssetPackage_SectionTableFields` |
| | offset(헤더 안·표 너머·정렬·빈 틈) | `AssetPackage_SectionTableFields` |
| | bytes(표 너머·0·겹침) | `AssetPackage_SectionTableFields`, `AssetPackage_CorruptionAndVersionChecks` |
| | 섹션 사이·표 앞 패딩(0만) | `AssetPackage_SectionTableFields`, `AssetPackage_CorruptionAndVersionChecks` |
| | PerEntryChecksum 섹션의 crc32 칸 = 0 | `AssetPackage_SectionTableFields` |
| | 필수 섹션(Summary·Exports·Objects), Bulk·BulkIndex 짝, 쿠킹의 ImportRecord·Tags 금지 | `AssetPackage_SectionTableFields` |
| | 모르는 kind(Required × 쿠킹·에디터, 비-Required 에디터 읽기 전용) | `AssetPackage_SectionTableFields` |
| | 아는 kind의 flags = 5.2절 표 | `AssetPackage_SectionTableFields` |
| | schemaVersion(쿠킹 높낮이, 에디터 낮음·높음 × Required) | `AssetPackage_SectionTableFields` |
| | 섹션 crc32(비-Bulk 섹션마다) | `AssetPackage_SectionTableFields` |
| 상한(5.1) | 파일 64 GiB | `AssetPackage_HeaderFields`(fileBytes·표 offset) |
| | 비-Bulk 섹션 64 MiB(초과 거부·상한 정확히 받음) | `AssetPackage_SectionSizeLimit` |
| | export 4096(정확히 4096 쓰기·읽기·로드, 4097 쓰기 CapacityExceeded, 표 count 4097 거부) | `AssetPackage_WriterBoundaries`, `AssetPackage_ExportsFields` |
| | object import 65536(표 count 65537 거부) | `AssetPackage_ObjectImportsFields`. 쓰기 쪽 65537개는 그래프 외부 참조 한도(`ObjectGraphLimits::maxExternalReferences` = 65536, ObjectSerialization 시험)가 먼저 막는다 |
| | 문자열 4 KiB(export 이름 4096 받음·4097 거부·프로파일 id), 객체 경로 4 KiB(import 경로 4096 받음·4097 거부) | `AssetPackage_ExportsFields`, `AssetPackage_SummaryFields`, `AssetPackage_WriterOptionValidation`, `AssetPackage_ObjectImportsFields` |
| | 작업량(깊이 2040 사슬 + 잎, import 65536 쓰기·읽기·Prepare·Instantiate, 65537 쓰기 CapacityExceeded — 5.7-13) | `AssetPackage_ImportScale` |
| | writer 비-Bulk 섹션 64 MiB 초과(ImportRecord 64 MiB + 1) | `AssetPackage_WriterEdgeRejections` |
| Summary(5.2·5.7-1) | 스키마 GUID·버전, 필드 수(에디터 4·쿠킹 5), 잘림(모든 길이 — 쿠킹·에디터) | `AssetPackage_SummaryFields`, `AssetPackage_TruncatedSections` |
| | 버전 표 모양(RecordList·항목 필드 수 1·3·id·version 없음·wire·wire는 맞고 payload 크기가 틀림)·정렬·중복 — 타입 표·커스텀 버전 표 각각 | `AssetPackage_SummaryFields` |
| | 엔진 버전 Record(필드 수·없는 칸·u16 범위·wire)·compatibleWith = savedBy 기준선(주·부·0 — 패치·부가 더 높음, 부 −1·주 −1로 더 낮음 모두 손상, 5.7-15) | `AssetPackage_SummaryFields` |
| | 프로파일 id(wire·빈 값·4 KiB·UTF-8)·설정 SHA-256(wire·32 byte)·쿠커 버전(wire·payload 크기) | `AssetPackage_SummaryFields` |
| | 타입 표 = export 타입 집합(빠짐·남음) | `AssetPackage_TypeAndCustomVersionAxes` |
| | 고장 없는 재구성 = writer byte | `AssetPackage_SummaryFields` |
| Exports(5.1·5.2·5.7-1·3·4) | count(0·4097·실제와 다름)·잘림(모든 길이)·남는 byte·이름 길이 | `AssetPackage_ExportsFields`, `AssetPackage_TruncatedSections` |
| | flags(정의되지 않은 비트·예약 EditorOnly·Primary 위치) | `AssetPackage_ExportsFields`, `AssetPackage_CorruptionAndVersionChecks` |
| | 타입 0, 대표(빈 이름·outer 0·Primary), 하위(이름·outer 범위) | `AssetPackage_ExportsFields` |
| | 깊이 우선 순서·같은 Outer 안 이름 순서·중복 | `AssetPackage_ExportsFields`, `AssetPackage_WriterBoundaries`(역순 생성 → 정렬) |
| ObjectImports(5.1·5.2) | count(0·65537·실제와 다름)·잘림(모든 길이)·남는 byte | `AssetPackage_ObjectImportsFields`, `AssetPackage_TruncatedSections` |
| | 에셋 null·자기 에셋·타입 0 | `AssetPackage_ObjectImportsFields` |
| | outer import(앞·같은 에셋·이름 있음)·outer 있는 항목의 이름 | `AssetPackage_ObjectImportsFields` |
| | assetId 순·에셋 안 깊이 우선·같은 outer 아래 이름 엄격 오름차순(대표 에셋 항목 첫째, '-' 이름), 깊은 경로 | `AssetPackage_ObjectImportsFields` |
| | 이름 규칙(제어 문자·비NFC·'.'·공백·첫 문자·예약 이름·65 byte·잘못된 UTF-8 — 읽기·로드 모두 CorruptPackage) | `AssetPackage_ObjectImportsFields`, `AssetPackage_InvalidImportNameIsCorruptOnLoad` |
| | 깊은 하위 객체만 참조(outer 전용 import, 쿠킹·에디터, 묶음 안·resolver), 중간 객체 타입 다름 TypeMismatch·없음 MissingDependency | `AssetPackage_DeepSubobjectImportOnly`, `AssetPackage_DeepImportResolutionFailures` |
| | 다른 에셋의 outer 전용 import(의존 없음 ObjectImports 손상, Hard를 더하면 Dependencies 손상), 외부 번호 수 불일치(그래프 Objects 손상) | `AssetPackage_OuterOnlyImportCrossChecks`, `AssetPackage_PrepareCrossChecks` |
| | 쓰이지 않는 import(이름 있는 잎·깊은 사슬 끝 잎·자식 없는 outer 전용 — ObjectImports 손상, 읽기 전용은 받음 — 5.7-15) | `AssetPackage_UnusedImportIsCorrupt` |
| | 선언 타입이 기대 타입보다 넓은 import(하위 객체·대표 에셋 — 연결 단계 TypeMismatch, section Objects) | `AssetPackage_ImportDeclaredBaseTypeMismatch` |
| Dependencies(5.2·5.7-5) | count(0·크기 상한·실제와 다름)·잘림(모든 길이)·남는 byte | `AssetPackage_DependenciesFields`, `AssetPackage_TruncatedSections` |
| | kind 범위(0·6·최댓값)·flags 정의되지 않은 비트 | `AssetPackage_DependenciesFields` |
| | 에셋 null·자기, 쿠킹의 EditorOnly·Build·ImportOwner, Hard 밖 expectedType, 정렬·중복 | `AssetPackage_DependenciesFields` |
| | Hard expectedType(0 거부, 대표 에셋 import 타입과 다름 거부, 로드 대상 대표 에셋이 파생 아님 TypeMismatch·없음 MissingDependency — 5.7-14) | `AssetPackage_OuterOnlyImportCrossChecks`, `AssetPackage_DeepImportResolutionFailures` |
| | 합성 규칙(Hard·Soft·EditorOnly·required, expectedType = 대표 타입, 자기·null 제외) | `AssetPackage_MergeDependencyUsesRules`, `AssetPackage_CookedRoundTrip`, `AssetPackage_EditorRoundTripKeepsEditorOnly`, `AssetPackage_DiamondDependencies`, `AssetPackage_CompositeGameDataRoundTrip`, `FixtureAsset_CombinationRoundTrip` |
| | 로드 대조(누락·남음·종류·required, Build·ImportOwner는 대조 안 함(`AssetPackage_ImportOwnerCrossCheckDeferredToA2`), 읽기 전용 부분집합, import의 Hard·EditorOnly 의존) | `AssetPackage_PrepareCrossChecks`, `AssetPackage_ImportTypeMismatchAndDependencyCrossCheck`, `AssetPackage_WriterRejections` |
| | 자기 참조는 의존·import 없음 | `AssetPackage_SelfReferencesMakeNoDependency` |
| Objects(5.1 참조 표기) | graph v2 byte, 참조 값(0·export·import), 노드 저장 버전 = Summary | `AssetPackage_CookedRoundTrip`, `FixtureAsset_CombinationRoundTrip`, `AssetPackage_PrepareCrossChecks`, `AssetPackage_FixedSeedMutationFuzz`(값 표기의 변조는 그래프 1단계 — ObjectSerialization 시험이 칸별로 본다) |
| ImportRecord·Tags(5.2) | 에디터만, Record envelope, 생략 | `AssetPackage_SectionTableFields`, `AssetPackage_WriterOptionValidation` |
| | Tags 키(필드 ID) 엄격 오름차순·같은 키 둘·Record 형식(writer 옵션 InvalidArgument·reader Tags 손상), 검사의 할당 실패(쓰기·읽기 OutOfMemory) | `AssetPackage_TagsKeysAreSorted` |
| | ImportOwner ↔ ImportRecord 대조, 여러 ImportOwner·required의 뜻 | 범위 밖(A2 — ImportRecord 스키마, 5.7-17). 지금 대조하지 않음을 `AssetPackage_ImportOwnerCrossCheckDeferredToA2`가 고정 |
| Bulk·BulkIndex(5.2·5.7-6) | count·잘림(모든 길이)·남는 byte·codec 범위 | `AssetPackage_BulkIndexFields`, `AssetPackage_TruncatedSections` |
| | bulkId 연속·offset = 정렬 값·flags·쿠킹 EditorOnly·None의 stored = raw·범위(Zstd entry의 stored, 정렬 값이지만 섹션 밖인 offset)·패딩·entry CRC·끝 = 섹션 크기 | `AssetPackage_BulkIndexFields`, `AssetPackage_CorruptionAndVersionChecks` |
| | 모든 entry가 어떤 ref에 쓰임(읽기 전용은 제외), 빈 ref의 크기·해시 0(writer InvalidArgument, 로드 Objects 손상 — 5.7-14) | `AssetPackage_BulkValueRules` |
| | 해제: raw 합 상한·Zstd 실패·크기·SHA-256·Zstd가 아닌 블록 컨테이너(LZ4)·블록 저장 byte 손상(블록 CRC), 실패 시 출력 길이 복원 | `AssetPackage_DecodeBulk` |
| | Zstd 블록 압축(메모리 피크가 entry 크기와 무관 — 5.5-3, 5.7-6), Zstd codec이면 0 byte가 아닌 entry는 줄지 않아도 블록 컨테이너(raw 블록 포함)이고 None은 0 byte entry뿐 | `AssetPackage_BulkBlockCompressionPeak` |
| | BulkDataRef ↔ BulkIndex(id 범위·크기·SHA-256) | `AssetPackage_PrepareCrossChecks` |
| | 첫 출현 순 재번호·같은 옛 id = 같은 새 id·재매핑 표·EditorOnly 표시(EditorOnly·일반 위치에 함께 쓰이면 표시 없음 — `AssetPackage_SharedBulkEditorOnlyFlag`)·0 byte entry(Bulk 0 byte) | `AssetPackage_DeterministicBytesAndFingerprint`, `FixtureAsset_BulkRoundTrip`, `AssetPackage_EditorRoundTripKeepsEditorOnly`, `AssetPackage_WriterBoundaries` |
| | bulk 원본 크기·SHA-256 불일치 CorruptPackage, 원본 오류 전파, 같은 옛 id 다른 값 거부, EditorOnly 위치에서만 두 번 쓰인 entry의 EditorOnly 표시 | `AssetPackage_WriterRejections`, `AssetPackage_WriterGraphRejections`, `AssetPackage_BulkWriterRules` |
| 버전 축(5.3) | 컨테이너 | `AssetPackage_HeaderFields` |
| | 섹션 | `AssetPackage_SectionTableFields` |
| | 타입(쿠킹 정확히·미등록, 에디터 낮음·높음·미등록, 클래스가 아닌 타입 id), 종료된 TypeRegistry는 판정이 아니라 그 오류(ShuttingDown, 진단 None) | `AssetPackage_TypeAndCustomVersionAxes`, `AssetPackage_NonClassTypeHasNoCurrentVersion`, `AssetPackage_ClosedTypeRegistryIsNotVersionVerdict` |
| | 커스텀 버전(쿠킹 전체 일치, 에디터 높음·모름 읽기 전용, 낮음·같음) | `AssetPackage_TypeAndCustomVersionAxes`, `AssetPackage_CorruptionAndVersionChecks` |
| | 엔진(compatibleWith 부·주가 더 새 NewerVersion, 패치는 보지 않음, 더 오래된 패키지 받음, 쿠킹은 기록 없음) | `AssetPackage_EditorRoundTripKeepsEditorOnly`, `AssetPackage_SummaryFields`, `AssetPackage_EngineVersionBoundaries` |
| | 쿠킹의 Required 실효 누락 = CorruptPackage, 에디터는 계속(hook 분류와 파일 변조 end-to-end) | `AssetPackage_LoadHooksClassifyDiagnostics`, `AssetPackage_RequiredReferenceZeroed` |
| 저장(5.5) | Objects 먼저 → 물리 순서 → 헤더 patch, 결정적 byte·지문 | `AssetPackage_DeterministicBytesAndFingerprint`, `FixtureAsset_CombinationRoundTrip` |
| | 고정 byte(풍부한 에디터 — import·Zstd 블록 bulk·커스텀 버전·ImportRecord·Tags·Build 의존, 쿠킹 None·쿠킹 Zstd — import·커스텀 버전 — 출력 SHA-256 고정; 체크인한 v1 byte 둘 — 대표 에셋 하나뿐인 B, 같은 섹션을 모두 담은 풍부한 A — 를 읽고 다시 저장 — 5.3 옛 fixture 씨앗) | `AssetPackage_GoldenBytes`, `AssetPackage_GoldenV1Resaves` |
| | 정준 byte(받아들인 입력은 다시 쓰면 같은 byte — 퍼징, compatibleWith는 savedBy 기준선과 정확히 같음 — 높음·낮음 거부) | `AssetPackage_FixedSeedMutationFuzz`, `AssetPackage_SummaryFields` |
| | 쓰기 실패 전파(Append 매 지점·헤더 WriteAt — 에디터, 쿠킹 + Zstd(Bulk 앞 패딩 0 아님), ImportRecord·Tags) | `AssetPackage_WriterSinkFailures` |
| | 임시 파일·Replace·잠금·외부 변경·재매핑 적용 | 범위 밖(A2 AssetDatabase — 파일 저장) |
| 옵션(PackageWriteOptions) | 종류·정렬·codec·엔진 버전·프로파일·ImportRecord·Tags·extraDependencies·커스텀 버전(정렬·중복, 셋을 정상으로) | `AssetPackage_WriterOptionValidation`, `AssetPackage_WriterRejections`, `AssetPackage_WriterEdgeRejections` |
| 대표 에셋(4.1·5.1) | 패키지 아님·없음·이름 있음·Asset 아님·저장 불가뿐 | `AssetPackage_WriterRepresentativeRules`, `AssetPackage_WriterRejections`, `AssetPackage_RepresentativeMustBeAsset`(export 1과 노드가 함께 Asset이 아닌 타입 — 로드 Exports 손상 detail 1) |
| 저장 그래프(5.1·6.2) | 자기 Package 참조 거부, 다른 Package 객체 자체 참조 거부, null id 패키지(쓰기·그 객체 참조) 거부, UnpackagedReference, Required null 거부(종류 × 자리), 저장 불가 객체 제외(그 아래 저장 가능한 자식 포함), weak 밖 대상 null | `AssetPackage_WriterGraphRejections`, `AssetPackage_WriterEdgeRejections`, `FixtureAsset_CombinationRequired`, `AssetPackage_CookedRoundTrip`, `FixtureAsset_CombinationRoundTrip` |
| 로드(5.7-10·11.2·11.3) | Prepare 한 번, 대표 에셋 타입, 그래프 1단계 분류(할당·스레드 그대로, 미등록·버전 UnsupportedVersion, 그 밖 CorruptPackage) | `AssetPackage_ImportTypeMismatchAndDependencyCrossCheck`, `AssetPackage_PrepareCrossChecks`, `AssetPackage_FixedSeedMutationFuzz`, `FixtureAsset_ValidateHook` |
| | InstantiatePackages 인자(빈 묶음·크기·null·준비 안 됨·다른 Registry·같은 id·이미 넘긴 상태 — 성공 뒤·실패 뒤) | `AssetPackage_InstantiateArgumentsAndResolver`, `AssetPackage_InstantiateTransactionErrors` |
| | 트랜잭션 오류 분류(다른 스레드 WrongThread·다른 접근 문맥 WrongContext·고정된 ShuttingDown·Busy — 진단 None) | `AssetPackage_InstantiateTransactionErrors` |
| | resolver 결과 검증(다른 AssetId·Package가 아닌 객체·null·회수된 handle — InvalidArgument — 5.7-16) | `AssetPackage_ResolverResultIsValidated` |
| | Prepare는 상태 allocator만(Registry allocator는 소유 스레드 Instantiate만), bulk 저장소 이어받기 | `AssetPackage_PrepareUsesOnlyStateAllocator` |
| | 에디터 읽기 전용 끝까지(원인 넷 — 모르는 필드(안의 Hard·Soft·bulk는 기본값)·더 새 타입 버전·모르는 커스텀 버전·Required가 아닌 모르는 섹션 — 마다 Instantiate·Commit), 현재 클래스가 없는 export 타입(미등록·클래스가 아닌 타입 id — reader 읽기 전용·Prepare가 reader 판정으로 Summary UnsupportedVersion — 5.7-18) | `AssetPackage_EditorReadOnlyLoadsEndToEnd`, `AssetPackage_NonClassTypeHasNoCurrentVersion` |
| | export 이름 규칙(제어 문자·'.'·예약 이름·65 byte·대소문자만 다른 형제 — Objects 손상) | `AssetPackage_ExportNameRulesOnLoad` |
| | import 해석(묶음 안·resolver·NotFound MissingDependency·그 밖 오류·경로 없음·TypeMismatch) | `AssetPackage_InstantiateArgumentsAndResolver`, `AssetPackage_HardCycleLoadsInOneTransaction`, `AssetPackage_ImportTypeMismatchAndDependencyCrossCheck` |
| | 조회 실패 전달(5.7-19): 패키지 정책이 Package가 아닌 Registry 거부(정책 없음·다른 타입 — InstantiatePackages 인자 InvalidArgument, 진단 없음, 객체 없음), Prepare·Instantiate 도중 닫히는 TypeRegistry(모든 할당 지점에서 Shutdown — 성공 또는 진단 None + ShuttingDown; Prepare는 대표 에셋 lease 지점, Instantiate는 Hard 대표 에셋 대조·import 대상 조회 지점 포함) | `AssetPackage_LoadRequiresPackagePolicyRegistry`, `AssetPackage_TypeRegistryClosesDuringPrepare`, `AssetPackage_TypeRegistryClosesDuringInstantiate` |
| | SCC 한 트랜잭션(2노드·3노드 모든 순서·다이아몬드·자기 참조), 실패 전파(부분 공개 없음) | `AssetPackage_HardCycleLoadsInOneTransaction`, `AssetPackage_ThreeNodeCycleLoadsInAnyOrder`, `AssetPackage_DiamondDependencies`, `AssetPackage_SelfReferencesMakeNoDependency`, `AssetPackage_FailurePropagatesThroughBatch` |
| | 스레드(Prepare 다른 스레드, 쓰기 소유 스레드 — WrongThread) | `AssetPackage_ThreadRules` |
| | 취소 지점 | A1c 로더 — 14.1.5절(`AssetLoader_CancelAtEachStage`) |
| Package·Asset(6.1·5.7-7) | id·종류·bulk·대표 assetId 설정, `BulkBytes` 오류(빈 ref·없는 id·다른 크기·같은 크기 다른 SHA-256), `NewPackage` null id·정의되지 않은 종류·다른 스레드·할당 실패(남는 패키지 없음) | `AssetPackage_CookedRoundTrip`, `AssetPackage_WriterRepresentativeRules`, `AssetPackage_PackageArgumentErrors`, `AssetPackage_NewPackageFailures` |
| 진단(4.6·5.7-8) | A1a가 내는 코드(CorruptPackage·UnsupportedVersion·NewerVersion·MissingDependency·TypeMismatch·UnpackagedReference), 원래 오류 보존, 이름표 | 위 시험들, `AssetPackage_DiagnosticNames` |
| 자원 실패 | 할당 실패(쓰기·읽기·Prepare·Instantiate 상태 allocator·Registry 객체·bulk 저장소 — 모든 지점, 진단 None, 주입한 실패는 모두 결과로 나온다 — Registry는 할당 실패를 trace로 대신하는 참조 위치 표를 끄고 잰다; 큰 시나리오: outer import 사슬·커스텀 버전 셋·타입 여럿·Soft·묶음 둘·resolver) | `AssetPackage_AllocationFailures`, `AssetPackage_RegistryAllocationFailures`, `AssetPackage_AllocationFailuresLargeScenario` |
| 변조·퍼징 | 고정 seed 4000회 × 쿠킹·에디터(타입 저장소 넘김, 정해진 오류 코드만, 읽기 거부·로드 거부·받아들임 모두 일어남, 객체 남지 않음) | `AssetPackage_FixedSeedMutationFuzz` |
| 모든 조합(14.1) | 성질 표 허용 칸 × EditorOnly 실효, Required 칸, hook·참조 struct·Flags enum 자리, 깊이 | `FixtureAsset_CombinationsCoverPropertyTable` |
| | 허용 칸 왕복(필드 비교·재저장 byte 동일, 쿠킹·에디터, Optional 두 상태, 참조 대상 7종) | `FixtureAsset_CombinationRoundTrip` |
| | 종류별 왕복(단순 스칼라 — 정수 폭·실수·bool·Name·String·enum·Guid, 값 묶음 — 중첩 struct·정적 배열·Array·Map·Set·Optional·Text·Bytes·InstancedStruct·커브·UE 단위 값, 상속 — 파생 객체의 기반·파생 필드) | `FixtureAsset_SimpleRoundTrip`, `FixtureAsset_ValueRoundTrip`, `FixtureAsset_InheritanceRoundTrip` |
| | 거부 칸(원소 자리 × 종류의 모든 × — 칸마다 정확한 코드), 허용 자리 대조, 자리 열 분류 | ToolTests `Meta.EveryRejectedValueKindCellIsRejected`, `Meta.EveryValueKindSpellingIsAcceptedWhereAllowed`, `Meta.EveryRejectedValueKindCellHasASpelling` |
| | 지정자 거부 칸(클래스 필드·struct 멤버 자리 — AcceptsRequired ×인 종류의 Required(원소 자리 포함), Map 키 Required, 가변 길이 컨테이너(Array·Map·Set)가 아닌 종류의 MaxElements(Container ○인 Optional·정적 배열 포함), 부호 없는 정수가 아닌 종류의 bitfield — 칸마다 정확한 코드)와 같은 원본 생성기의 허용 대조(AcceptsRequired ○인 종류 × 필드·struct 멤버·허용 원소 자리, Array·Map·Set의 MaxElements, 부호 없는 정수 bitfield) | ToolTests `Meta.EveryRejectedSpecifierCellIsRejected`, `Meta.EverySpecifierIsAcceptedWhereAllowed` |

#### 14.1.2 Coverage(A1a)

측정은 clang source-based coverage다. Rocks.Build `Coverage` 구성으로 한다.
- 계측: Debug 코드 생성에 compile 인자 `-fprofile-instr-generate -fcoverage-mapping`을 더한다. 코드 생성 옵션이라 MetaCompiler AST 프로필에는 넣지 않는다.
- 링크: compiler resource 디렉터리에서 찾은 `clang_rt.profile`을 넣는다. Swift 툴체인은 triple 디렉터리 이름이 driver의 추정과 달라서 경로를 직접 넘긴다.
- CRT: 릴리스 CRT를 쓴다. Asan과 같다.

측정 명령은 하나이며 재현할 수 있다.

```
pwsh Build/MeasureCoverage.ps1 -Target EngineTests -Filter AssetPackage,FixtureAsset -Sources Engine/Source/Runtime/AssetRuntime/Private,Engine/Source/Runtime/AssetRuntime/Public
```

이 명령이 하는 일은 다음과 같다.
1. Coverage 구성으로 빌드한다.
2. 시험을 filter마다 한 프로세스로 실행한다. `LLVM_PROFILE_FILE`에 프로세스마다 다른 파일 이름을 준다.
3. `llvm-profdata merge`로 결과를 합친다.
4. `llvm-cov export·show`로 보고한다. 도구는 compiler와 같은 Swift 툴체인 `usr/bin`의 것을 쓴다.

결과는 `out/coverage/enginetests/`에 남는다.
- `report.txt`: 파일별 줄·분기·함수와 합계
- `uncovered.txt`: 줄·분기 실행 횟수
- `summary.json`

측정 결과(2026-11-20, 검증 worktree — origin/feature/engine-runtime 493f9a0d(GC 1–5·생성 소스 분리 GS1 포함) 위에 판정 리뷰 반영; AssetRuntime Private·Public 13개 파일, AssetPackage 71개·FixtureAsset 8개 시험 모두 통과): 줄 99.89%(2624/2627), 분기 97.11%(1546/1592), 함수 100%(122/122). 리뷰 전 측정(줄 95.13%, 분기 93.15%, 닿지 않은 분기 쪽 110)에서 빈틈 리뷰(51)·재리뷰(51 — 그 가운데 45는 불변식 검사)·판정 리뷰를 거쳤다.

| 파일 | 줄 | 분기 |
|---|---|---|
| Package.cpp | 37/37 | 22/24 |
| PackageAccess.h | 13/13 | - |
| PackageInternal.h | 34/34 | 18/18 |
| PackageLoad.cpp | 453/453 | 270/276 |
| PackageReader.cpp | 947/947 | 650/667 |
| PackageWriter.cpp | 1055/1057 | 557/576 |
| Public 7개(AssetDiagnostic.h·BulkData.h·Package.h·PackageFormat.h·PackageLoad.h·PackageReader.h·PackageWriter.h) | 85/86 | 29/31 |

닿지 않은 분기 쪽 46개 가운데 42개는 불변식 검사 `ROCKS_ASSERT`(Core/Assert.h)의 실패 쪽이다(Package.cpp 2, PackageLoad.cpp 6, PackageReader.cpp 16, PackageWriter.cpp 18). 매크로 전개 안의 분기라 `uncovered.txt`(전개를 펼치지 않은 `llvm-cov show`)에는 보이지 않고 합계에만 든다(`-show-expansions`로 확인). 나머지 4개는 아래에 적었다. 닿지 않은 줄 3개는 아래 1번(PackageWriter.cpp 2줄)·2번(PackageFormat.h 1줄)이다.

빌드 주의: Rocks.Build는 시험 소스가 상대 경로로 include한 Private 헤더(`Tests/PackageEditing.h` → `../Private/PackageInternal.h`)의 변경으로 그 시험 번역 단위를 다시 컴파일하지 않는다(헤더 의존 추적 — UnityBuildPch UB0 선행 과제). 측정 전에 Coverage 구성의 EngineTests·AssetRuntime 오브젝트를 지운다(옛 inline 함수의 기록이 남으면 함수 수가 틀린다).

닿지 않던 분기는 먼저 입력 변조·실패 주입으로 닿게 했고, 닿을 수 없는 것은 이유를 확인해 불변식 검사로 바꾸거나 아래에 사유를 적었다.
- 입력 변조로 닿음(재리뷰): Summary Record의 wire 종류는 맞고 payload 크기만 틀린 필드(Record reader는 payload 크기를 wire 종류와 대조하지 않는다, `AssetPackage_SummaryFields`), Zstd가 아닌(LZ4) 블록 컨테이너와 블록 저장 byte 손상(`AssetPackage_DecodeBulk`), 저장 불가 객체 아래의 저장 가능한 자식(`AssetPackage_CookedRoundTrip`), Tags의 같은 키 둘(`AssetPackage_TagsKeysAreSorted`), 클래스가 아닌 export 타입 id(`AssetPackage_NonClassTypeHasNoCurrentVersion`).
- 실패 주입으로 닿음: Tags 검사의 할당 실패(쓰기·읽기 sweep), 종료된 TypeRegistry(`AssetPackage_ClosedTypeRegistryIsNotVersionVerdict`), Prepare 도중 닫히는 TypeRegistry(`AssetPackage_TypeRegistryClosesDuringPrepare` — 모든 할당 지점, 대표 에셋 lease 지점 포함), Instantiate 도중 닫히는 TypeRegistry(`AssetPackage_TypeRegistryClosesDuringInstantiate` — 로드 import 대상의 `AcquireType` 실패 두 곳: Hard 의존의 대표 에셋 대조와 import 대상 조회. 앞 측정은 이 두 곳을 "닿는 로드 경로가 없다"로 적었는데 틀렸다), 패키지 정책이 Package가 아닌 Registry(`AssetPackage_LoadRequiresPackagePolicyRegistry`).
- 불변식 검사: 표 섹션 `ArchiveReader::Create`(섹션 표 검증이 크기 상한을 먼저 확인), resolver 검증 뒤 `Get`, 쿠킹의 읽기 전용(reader는 쿠킹을 읽기 전용으로 표시하지 않고 그래프 1단계 쿠킹 정책은 모르는 필드·더 새 버전을 실패로 끝낸다), 검증한 Package의 대표 에셋 조회 실패 = NotFound(빈 이름은 이름 검사·키 할당이 없다), writer 쪽 조회(그래프 저장이 생존·타입을 먼저 확인한 대상 — 5.7-19), writer 대표 에셋 타입 표 조회(Hard 의존은 모두 `ImportBuilder::Add`를 거친다), 블록 인덱스 완성(`TryFinishIndex`), export 순회의 경로 pop·첫 export의 부모·나머지 export의 부모가 export(정체성 규칙 8 — 대표 자식이 있는 패키지는 이름 있는 자식을 가질 수 없다), import 빌더의 사슬이 패키지에 닿기 전에 대표 에셋을 지남(같은 규칙), Dependencies·BulkIndex 앞 칸 읽기, `StoredVersion`, Zstd 설정.

닿지 않은 분기 4개(위치는 `uncovered.txt`):
1. **물리적으로 만들 수 없는 크기**: bulk entry 2³²−1개(PackageWriter.cpp — 메모리에 만들 수 없다).
2. **컴파일 시간 상수·완전한 switch**: `PackageFormat.h`의 `PackageSectionRequiredFlags` switch 밖과 `IsKnownPackageSection`의 한쪽(실행 시간 호출의 모르는 kind는 `AssetPackage_PackageArgumentErrors`), reader 섹션 switch의 열거자 밖(알려진 kind만 들어온다).

규모 시험(`AssetPackage_ImportScale`)의 두 Registry는 시험 안의 `ScaleAllocator`(플랫폼 정렬 할당·살아 있는 수만 셈)를 쓴다. SystemAllocator는 살아 있는 할당의 선형 원장이라 객체 13만 개 Registry의 종료 회수가 제곱 시간이었다(GC 병합 뒤 시험 하나가 약 11분 — 대부분 Registry 종료). 남은 시험 시간(Debug 약 70초)의 대부분은 그래프 저장이 외부 참조마다 Outer 사슬(깊이 2042)을 걸으며 저장 불가 조상을 찾는 비용이다(ObjectSerialization `GraphEncodeContext::WriteUnsaveable` — 참조 수 × 깊이, Core 쪽 과제).

A1a 상태(2026-11-20): **구현**. 완료 조건 넷을 모두 갖췄다. 빈틈 리뷰(지적 14개), 재리뷰(미충족 판정), 판정 리뷰(미충족 — 중간 4·낮음 7), 재판정(미충족 — 중간 1)의 지적을 반영했다.
- 요구 대조표(14.1.1): 시험 없는 항목 0, 정의된 시험 79개를 모두 인용
- 모든 조합 fixture·빠짐 검사(조합 클래스만, 자리 열 분류 static_assert)·거부 칸 시험(칸마다 정확한 코드, 허용 자리 대조)(14.1)
- coverage 측정과 닿지 않은 분기의 이유(14.1.2)
- 빠진 시험 관점 리뷰어 판정: 충족(dbd46152 기준). 남은 참고 하나: 지정자 시험의 `MaxElementsKinds`는 성질 표가 아니라 손으로 적은 집합이다(성질 표가 C#·C++ 미러라 성질 추가는 모듈 간 변경). 표에 새 가변 컨테이너 행이 생기면 거부 칸 시험이 실패해 드러난다.

#### 14.1.3 요구 대조표(A1b — 쿠킹 매니페스트 v1)

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/AssetRuntime/Tests/ManifestTests.cpp`·`ManifestScaleTests.cpp` — 정의된 시험 17개를 모두 표가 인용한다. A1d에서 bundle 구성원(10.1절 5)을 더하며 다시 맞췄다). 시험 도구(`ManifestTestSupport.h`)의 시험 인코더는 검증하지 않고 10.1절 1 배치로 쓰며(규칙을 어긴 모델을 reader에 넣는다), writer 출력과 byte가 같음을 왕복 시험이 확인한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 배치·결정성(10.1-1) | 쓰고 읽으면 같은 모델, 다시 쓰면 같은 byte, 시험 인코더 = writer byte, 빈 매니페스트, 비ASCII NFC 경로·bundle(`/Game/Ärger`·`Äb`) 왕복, `Find`(기본 장면의 있는 id 셋, 10·12·14 장면에서 있는 id 셋·없는 id 앞 1·사이 11·13·뒤 15) | `CookedManifest_RoundTrip` |
| | 고정 byte(고정 장면 출력 SHA-256, 체크인한 v1 byte와 같음, 그 byte를 읽고 다시 쓰면 같음) | `CookedManifest_GoldenBytes` |
| 판정 순서·진단(10.1-2) | 최소 길이 미만(빈 입력·모든 길이), 파일 상한 초과(내용 읽기 전 — 길이만 큰 span이라 이 순서에 기댄다), magic 네 byte, formatVersion 0·2·최댓값(UnsupportedVersion / Unsupported, detail = NoEntry·asset null), CRC(끝 칸, 개수 칸 최상위 byte — CRC 미보정이면 Crc·보정하면 EntryCount라 CRC가 구조보다 먼저), 남는 byte, 모든 길이 자르기(CRC 맞춤 — 언제나 손상, 빈 매니페스트는 헤더 칸 다섯 각각의 잘림에 닿음) | `CookedManifest_StructureTampering` |
| | 항목 칸 잘림: 항목 둘인 매니페스트를 항목 1 안의 모든 위치로 자름 → 칸 12개(assetId·typeId·경로·SHA-256·packageBytes·bulkRawBytes·cookKey·flags·bundle 개수·bundle 이름·bundle 구성원 개수와 구성원·의존 개수) 범위표대로 그 칸·항목 1·asset(assetId 잘림은 null, 나머지 11) | `CookedManifest_EntryTruncation` |
| | 겹친 위반의 우선순위 표 10줄: 길이 10 + magic → File, magic + 버전 → Magic, 버전 + CRC → UnsupportedVersion, 빈 프로파일 + CRC → Crc, 남는 byte + 빈 프로파일 → File, 항목 0 flags=2 + 항목 1 bundle 257개 → reader BundleCount/1·검증·writer Flags/0, 경로 중복 + 쿠킹되지 않은 Hard → PathHint/2(세 경로), 병합 profileId + cookerVersion → ProfileId, 패키지 대조 길이 다른 파일 + 다른 패키지 내용 → PackageBytes, SHA만 다른 파일 + 다른 패키지 내용 → PackageSha256(같은 파일이면 AssetId — 대조) | `CookedManifest_JudgementOrder` |
| 상한(10.1-3) | 항목 수(모델 2²⁰ + 1 — 검증·쓰기, 항목 배열 약 226 MB를 한 번에 잡는 시험; 개수 칸 상한 + 1·남은 byte로 담을 수 없음·실제보다 큼·작음), bundle 256 받음·257 거부·개수 칸 초과, bundle 구성원 65536 받음(모두 쿠킹된 Soft 의존 대상 — 65537개 항목 매니페스트 왕복)·65537 거부(`BundleMemberCount`), 의존 65536 받음·65537 거부·개수 칸 초과, 프로파일 4096 byte 받음(쓰기·읽기)·4 KiB + 1·길이 칸이 남은 byte보다 큼, 경로 256 받음·257 거부(정확히 상한 + 1)·265 거부 | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering`, `CookedManifest_EntryCountLimit`, `CookedManifest_BundleMemberLimit` |
| | 할당 상한: 개수 칸이 큰 입력(항목 1000·bundle 100000·의존 50000)과 정상 입력에서 reader의 가장 큰 할당 요청 ≤ 입력 byte × 4(기록 allocator) | `CookedManifest_AllocationBound` |
| 헤더 칸(10.1-4) | 프로파일 빈 값·잘못된 UTF-8(byte), 엔진 버전 세 칸 각각 u16 초과·세 칸 모두 0xFFFF 받음 | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering` |
| 항목 칸(10.1-5) | 위반마다 칸·항목과 asset·related를 검증·writer·reader 세 경로에서 단언(헤더 칸 asset null, 경로 중복은 그 항목 asset), assetId null·중복·역순, typeId null, 경로(빈 값·상대·마운트만·빈 구성 요소·끝 '/'·공백·'.'·예약 이름·비 NFC·'\\'·길이·잘못된 UTF-8 byte), packageBytes 63 거부·64 받음·64 GiB 받음·+1 거부, bulkRawBytes 최댓값 받음, 정의되지 않은 flags 비트 | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering` |
| | bundle(PrimaryAsset이 아닌 항목·이름 규칙·역순·중복·caseless 중복(ASCII·비ASCII `Äb`·`äb`)·잘못된 UTF-8 byte), bundle 구성원(`BundleMember` — null·자기 자신·역순·중복·그 항목의 의존 대상 아님, related = 구성원), 의존(null — related null·자기·kind 0·3·4·5·6·최댓값·역순 — related 5·중복, 의존 위반은 related = 그 의존 대상) | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering` |
| 항목 사이(10.1-6) | 경로 caseless 중복(둘·셋·비ASCII `/Game/Ärger`·`/game/ärger` — 진단 항목은 정렬 뒤 첫 짝의 뒤), 쿠킹되지 않은 Hard 대상은 related = 그 대상(99), 매니페스트에 없는 bundle 구성원(Soft 의존 대상 99)은 `BundleMember`·related 99, 쿠킹되지 않은 Hard 대상(작은 장면·2만 항목 사슬 가운데 삭제), 쿠킹되지 않은 Soft 대상 받음, Hard 순환 받음 | `CookedManifest_FieldRules`, `CookedManifest_LargeManifest`, `CookedManifest_AllCombinations` |
| writer(10.1-7) | 위반은 쓰지 않음(미리 채운 out `{0xAA, 0xBB}` 그대로·검증과 같은 진단 — 위반 목록 전부와 항목 수 상한) | `CookedManifest_FieldRules`, `CookedManifest_EntryCountLimit` |
| | 파일 상한은 CRC 칸 포함 전체: `WriteCookedManifestWithLimit` 상한 = 출력 크기는 받음(공개 writer와 같은 byte), 출력 크기 − 1은 `CapacityExceeded`(진단 None, out 그대로) | `CookedManifest_WriteLimit` |
| 대조(10.1-8) | 실행 프로파일 같음·다름·대소문자만 다름(IncompatibleCook), 병합 네 칸 각각 다름(그 칸)·엔진 버전만 다름은 받음 | `CookedManifest_ProfileAndMergeChecks` |
| | 패키지 대조: A1a writer가 쓴 쿠킹 패키지(Hard·Soft 의존 — Soft 대상 1700이 Hard 대상 1801보다 앞인 (asset, kind) 순서, 같은 대상 Hard+Soft 패키지 — ·Zstd bulk, EditorOnly bulk는 raw 합에서 빠짐)가 그 결과로 만든 항목과 맞음, 크기(파일 덧붙임·항목)·SHA-256(파일·항목)·종류(에디터 패키지)·assetId·대표 에셋 타입·export 없는 내용·bulk raw 합(하나 작음·u64를 넘는 합 — 감으면 같은 값)·프로파일(id·설정·쿠커 버전)·의존(빠짐·남음·종류·대상·전부 없음·종류 먼저 순서·같은 대상의 Soft 빠짐·종류 뒤바뀜 — related는 처음 다른 의존 대상 1801·1900·1801·1899·1700) | `CookedManifest_PackageCrossCheck` |
| 파일 경로(4.2) | `Packages/<a>/<A>-<h>.rpkg`(앞 내용 뒤에 붙임), 모든 할당 지점 실패 | `CookedManifest_PackagePath` |
| 자원 실패 | 쓰기·읽기·검증의 모든 할당 지점(진단 None, 미리 채운 쓰기 out 그대로 — 읽기의 bundle 구성원 목록 예약, 비교 키 성장, NFC quick check가 MAYBE인 경로·bundle(U+1EA1 U+0301)의 NFC 확인 scratch와 비ASCII caseless 키 할당 포함) | `CookedManifest_AllocationFailures` |
| 조합(14 완료 조건 2) | (PrimaryAsset × bundle 없음·하나·여럿·하나(구성원)·여럿(구성원) + 일반) 6 × 의존 특성 다섯(쿠킹된 Hard·Hard 순환·쿠킹된 Soft·쿠킹되지 않은 Soft·같은 대상 Hard+Soft)의 부분집합 32 × 경로(얕음·깊음·비ASCII NFC) 3의 원시 공간 576에서 허용 조합 564개 전부(구성원 없는 모양 4 × 32 × 3 = 384, 구성원 있는 모양 2 × 쿠킹된 의존 대상이 있는 부분집합 30 × 3 = 180 — 구성원은 쿠킹된 의존 대상 전부). 읽은 항목마다 원시 번호를 다시 계산해 만든 번호(id)와 같고 허용 조합인지 봄 — 일대일, 조합 수와 플래그가 바뀌면 `static_assert`가 막음. 금지 조합(일반 항목의 bundle, 쿠킹된 의존 대상이 없는 항목의 구성원)은 칸 규칙 | `CookedManifest_AllCombinations`, `CookedManifest_FieldRules` |
| 규모 | 항목 2만(깊이 2만 Hard 사슬·8개 단위 순환·쿠킹되지 않은 Soft·PrimaryAsset bundle) 왕복·Find·같은 byte | `CookedManifest_LargeManifest` |
| 변조·퍼징 | 고정 seed 4000회(byte 바꾸기·자르기·덧붙이기, 절반 CRC 맞춤 — 정해진 진단만, 거부·받아들임 모두, 받아들인 입력은 다시 쓰면 같은 byte) | `CookedManifest_FixedSeedMutationFuzz` |
| | 게시(임시 파일·Flush·Replace)·부분 쿠킹 병합 실행·cookKey 계산 | 범위 밖(A4 쿠커 — 대조 함수는 여기) |
| | 서명 봉투 `CookedManifest.rsig` | 범위 밖(A1f — reader는 span을 받는다) |
| | Hard SCC 계산·`FindByPath` 색인·`maxBulkRawBytes` 적용·패키지를 열 때 대조 호출 | A1c 로더 — 14.1.5절 |

#### 14.1.4 Coverage(A1b)

측정(2026-11-20, 14.1.2와 같은 명령에 filter `CookedManifest`를 더함 — `-Filter AssetPackage,FixtureAsset,CookedManifest`, AssetPackage 71개·FixtureAsset 8개·CookedManifest 16개 시험 모두 통과): 매니페스트 파일 `Private/CookedManifest.cpp` 줄 100%(540/540)·분기 100%(296/296)·함수 100%(24/24), `Public/AssetRuntime/CookedManifest.h` 줄 100%(6/6)·함수 100%(4/4)(분기 없음). 닿지 않은 분기가 없다. AssetRuntime 전체(A1a 파일 포함)는 줄 99.91%(3170/3173)·분기 97.56%(1842/1888)·함수 100%(150/150)이고 닿지 않은 분기는 14.1.2의 A1a 항목 그대로다.

A1d 재측정(2026-11-20, bundle 구성원을 더한 뒤 — 14.1.8과 같은 명령): `Private/CookedManifest.cpp` 줄 100%(611/611)·분기 100%(340/340)·함수 100%(29/29). 처음 재측정에서 읽기의 구성원 목록 예약 실패 분기가 남아 할당 실패 sweep 장면에 구성원을 더했다.

`ROCKS_ASSERT`는 쓰지 않지만, 결과를 확인하지 않는 불변식이 12곳 있다(`Result::Value()`는 실패 결과면 `platform::Fatal`로 멈추고, `(void)`로 버린 결과는 실패하면 조용히 빠진다 — 그래서 각각 입력으로 실패에 닿을 수 없음을 근거로 둔다). 위치는 `Private/CookedManifest.cpp`의 함수 이름으로 적는다(줄 번호는 병합·개명으로 밀린다).

| 위치 | 불변식 | 근거 |
|---|---|---|
| `ValidateBundles` | bundle 비교 키 `keys.TryEmplaceBack(scratch).Value()` | 바로 앞에서 bundle 수만큼 `TryReserve`했고, 빈 `String` 추가는 할당하지 않는다 |
| `ValidateCookedManifest` | 경로 비교 키 `keys.TryEmplaceBack(scratch).Value()` | 항목 수만큼 예약했다(같은 이유) |
| `ValidateCookedManifest` | `(void)order.TryPushBack(...)` | 항목 수만큼 예약했다 — 예약 안 추가는 실패하지 않는다 |
| `ReadCookedManifest` | `ArchiveReader::Create(...).Value()` | 넘기는 span 길이 ≤ `limits.maxTotalBytes`(= body)라 Create의 유일한 실패(`CapacityExceeded`)가 없다 |
| `ReadCookedManifest` | 항목 수 칸 `in.ReadU32()` 결과의 `Value()` | 길이 ≥ `MinFileBytes`(magic·version·개수·crc)를 먼저 보아 개수 칸 4 byte가 있다 |
| `ReadCookedManifest` | 항목 `manifest.entries.TryEmplaceBack(allocator)` 결과의 `Value()` | 개수 칸 값만큼 예약했다(항목 생성자는 할당하지 않는다) |
| `ReadCookedManifest` | bundle `entry.bundles.TryEmplaceBack(allocator).Value()` | bundle 개수만큼 예약했다 |
| `ReadCookedManifest` | bundle 구성원 `in.ReadGuid().Value()` | 구성원 개수 ≤ 남은 byte / 16을 먼저 보았다 — 구성원 칸은 잘리지 않는다 |
| `ReadCookedManifest` | `(void)slot.members.TryPushBack(...)` | 구성원 개수만큼 예약했다 |
| `ReadCookedManifest` | 의존 대상 `in.ReadGuid().Value()` | 의존 개수 ≤ 남은 byte / 20을 먼저 보았다 — 의존 칸은 잘리지 않는다 |
| `ReadCookedManifest` | 의존 kind `in.ReadU32().Value()` | 같은 개수 검사 |
| `ReadCookedManifest` | `(void)entry.dependencies.TryPushBack(...)` | 의존 개수만큼 예약했다 |

닿게 한 방법과 그 과정에서 바꾼 것:
- 입력 변조: 시험 인코더(검증하지 않음)로 칸마다 위반 모델을 reader에 넣고, 원시 칸 덮어쓰기(개수·길이·엔진 버전·kind·magic·formatVersion·남는 byte)와 CRC를 다시 맞춘 자르기(항목이 있는 매니페스트는 개수 칸 판정에서, 빈 매니페스트는 헤더 칸마다, 항목 둘인 매니페스트는 항목 1의 칸마다 — `CookedManifest_EntryTruncation`)로 구조 판정에 닿는다. 잘못된 UTF-8은 모델 `String`이 담을 수 없어 인코딩한 byte를 고쳐 넣는다. 파일 상한 초과 시험은 길이만 큰 span을 넘기므로 읽기 전에 길이로 거부하는 순서에 기댄다(순서가 바뀌면 경계 밖 읽기가 된다).
- 실패 주입: 쓰기·읽기·검증의 모든 할당 지점(NFailAllocator sweep). 비교 키 builder는 내장 용량을 작게 두어(7 byte) 구성 요소 키와 구분자 양쪽에서 자라고, sweep 장면의 경로 길이(첫 구성 요소 6·13·27 byte — 용량을 꼭 채운다)로 두 성장 지점에 닿는다. 장면의 경로·bundle 하나씩은 NFC quick check가 MAYBE인 문자열(U+1EA1 U+0301)이라 `ValidateNameComponent`의 NFC 확인 scratch 할당(bundle 쪽 실패 → 진단 None 경로 포함)과 비ASCII caseless 키 할당에도 닿는다.
- writer 상한: 상한을 인자로 받는 내부 함수(`package_detail::WriteCookedManifestWithLimit`)로 `CapacityExceeded`에 닿는다(1 GiB 모델 없이). 처음 구현은 상한에서 CRC 4 byte를 한 번 더 빼 1 GiB − 4까지만 썼다(리뷰 지적 — 고침).
- 처음 구현에서 없앤 죽은 분기(입력으로 닿을 수 없음을 확인): 예약한 배열에 빈 String을 더하는 실패, 최소 길이가 보장한 개수 칸 읽기 실패, 모델 `String`의 UTF-8 재검사(Core 불변식), CRC 칸 쓰기 실패(자리를 본문과 함께 쓰고 다 쓴 뒤 채운다). 이번 라운드: bulk raw 합 넘침 검사를 `overflow || ...` 누적에서 넘치면 멈추는 반복으로 바꿨다(넘친 뒤 항목이 없으면 누적의 단락 쪽은 닿을 수 없었다 — 동작은 같다).


A1b 상태(2026-11-20): **구현**. 완료 조건 넷을 모두 갖췄다. 빠진 시험 리뷰(미충족 — 판정 순서·진단 값·조합 축·경계·writer 상한·bulk 합 넘침 등 12개)를 반영했고, 재판정은 충족(3794978d 기준)이다.
- 요구 대조표(14.1.3): 시험 없는 항목 0, 정의된 시험 16개를 모두 인용
- 조합 384개 전부와 일대일 빠짐 검사, 금지 조합은 칸 규칙(14.1.3 조합 행)
- coverage: 매니페스트 파일 줄·분기·함수 100%, 결과 불변식 10곳의 근거(14.1.4)
- 빠진 시험 관점 리뷰어 판정: 충족. 남은 참고 하나: 할당 실패 sweep이 비ASCII 할당 지점에 닿는지는 코드로 확인했고 시험이 직접 단언하지는 않는다.

#### 14.1.5 요구 대조표(A1c — 로더)

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/AssetRuntime/Tests/LoaderTests.cpp`·`LoaderFailureTests.cpp`·`LoaderIoTests.cpp`·`LoaderStageTests.cpp`·`LoaderCombinationTests.cpp`·`LoaderStagedCombinationTests.cpp`·`LoaderFaultCombinationTests.cpp` — 정의된 시험 47개를 모두 표가 인용한다. PlatformBase 결과 주입은 `AsyncFile_InjectedFaultsAreDeterministic`). 시험 도구(`LoaderTestSupport.h`)는 A1a writer로 쓴 쿠킹 패키지와 그 결과로 만든 매니페스트(장면), 완료를 시험이 내보내는 IO(`TestPackageIo` — 경로마다 없음·완료 오류·Begin 즉시 실패를 끼운다), 손으로 돌리는 worker(`TestLoaderWorker`), 부를 때마다 정해진 만큼 가는 시계를 준다. 실제 스레드를 쓰는 시험(디스크 IO·WorkerScheduler·다른 스레드 호출·종료 대기)은 결과만 단언한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 요청·결과(11.6-1) | 사슬 요청 → Succeeded·패키지 handle = FindPackage, 공개한 패키지 요청은 그 자리에서 Succeeded(다시 읽지 않음), Release 뒤 StaleHandle(칸 재사용 뒤 세대 불일치 포함), 매니페스트에 없는 id NotFound / MissingDependency, 범위 밖 우선순위 InvalidArgument, Initialize의 매니페스트 재검증(빠진 Hard 대상 → CorruptPackage Dependency) | `AssetLoader_LoadsChainAndJoins`, `AssetLoader_InitializeAndArguments`, `AssetLoader_InitializeValidatesManifest` |
| ResolveManifest·SCC(11.6-2) | Hard 폐포(Soft는 따라가지 않음 — 쿠킹되지 않은 Soft 대상), 요청 직후 단위 수·구성 수가 SCC와 같음(단일·둘·셋 순환·다이아몬드·진행 중 순환에 기대는 사슬·꼬리가 나가는 순환), 단위 하나 예산에서도 순환 구성은 함께 공개, 순환 참조가 서로의 대표 에셋을 가리킴, 큰 그래프(10개 순환 블록 40개 + 깊이 1500 사슬) | `AssetLoader_HardSccShapes`, `AssetLoader_LargeGraph`, `AssetLoader_LoadsChainAndJoins` |
| | `FindByPath`(caseless 키·없는 경로 NotFound·규칙 위반 InvalidArgument, 비ASCII NFC 경로를 대문자 Ạ·소문자 마운트로 찾음) | `AssetLoader_FindByPath`, `AssetLoader_FindByPathNonAscii` |
| | 공개된 의존에 기대는 요청(단위 하나·대기 이유 Admission·Hard 참조 연결), 허가된 의존 단위에 기대는 늦은 요청 | `AssetLoader_PublishedDependencyIsResolved`, `AssetLoader_LoadsChainAndJoins` |
| 패키지 단계(11.6-3) | IO → worker(크기·SHA-256 → Prepare → 내용 대조) → Prepared → 의존 공개 대기 → Pump 공개, 단계별 대기 이유·누적 시간·임시 byte | `AssetLoader_DescribeTracksStages` |
| 경로·IO(11.6-4) | `<루트>/` + `AppendCookedPackagePath`, 디스크 파일을 AsyncReadFile 조각 읽기로(bulk 그대로), 파일 없음 NotFound / MissingFile, 디스크 크기 다름 PackageBytes, 완료 오류 → 진단 None과 그 오류, Begin 즉시 실패(순환 둘째 패키지에서 — 첫째 IO는 끝난 뒤 버림), 기본 IO의 OS 오류(결과 주입 — 크기 조회·둘째 조각 제출·첫 조각 완료 실패 → 진단 None·DependencyFailed — 결과 주입은 Release 구성에 없어 이 장면은 Release에서 빠진다), 짧게 읽힘(완료 게시 보류 사이 파일을 1/4로 다시 씀 → InvalidArgument·진단 None) | `AssetLoader_LoadsFromDiskWithWorkers`, `AssetLoader_IoAndWorkerErrors` |
| | PlatformBase 결과 주입(Size·Submit·Completion, skip, 한 번 뒤 사라짐, ClearFault, 두 backend — Release 구성에서는 시험째 빠진다) | `AsyncFile_InjectedFaultsAreDeterministic` |
| worker(11.6-5) | WorkerScheduler worker로 끝까지 로드, 제출 실패(scheduler 시작 전·작업 묶음 할당 실패) → 그 오류·실행 안 함, 로더 worker 제출 실패 → 요청 실패 | `AssetLoader_LoadsFromDiskWithWorkers`, `AssetLoader_SchedulerWorkerReportsSubmitFailures`, `AssetLoader_IoAndWorkerErrors` |
| 손상(11.6-3·4, 10.1-8) | SHA-256 → PackageSha256, 짧은 파일 → PackageBytes, 내용 손상(매니페스트 SHA·내용 주소 맞춤) → 패키지 reader CorruptPackage, bulkRawBytes가 실제보다 작음 → `maxBulkRawBytes` CapacityExceeded, 큼 → BulkRawBytes, 의존 → PackageDependencies, 대표 타입 → TypeId, 오래된 의존 쿠킹 결과의 대표 타입이 기대 타입이 아님 → 공개 단계 TypeMismatch(의존 2·3은 공개된 채) | `AssetLoader_CorruptionIsDiagnosed`, `AssetLoader_StaleDependencyTypeFailsInstantiate` |
| 실패 전파(11.6-6) | 사슬(손상 3 → 2·1 DependencyFailed / MissingDependency, related 3), SCC 형제, 다이아몬드(다른 가지 단독 요청은 성공), 한 정리 묶음에서 두 번 닿는 단위(82 → 81 → 80, 82 → 80 — 원인 80 유지), Soft 대상 손상 무영향(읽지 않음), 파일을 고친 뒤 다시 요청하면 새로 로드, 허가 전(Queued) 단위의 의존 실패(Begin 없이 DependencyFailed) | `AssetLoader_HardFailurePropagates`, `AssetLoader_AllCombinations`, `AssetLoader_QueuedUnitsFailWithTheirDependency` |
| 합류·취소(11.6-7) | 같은 에셋·진행 중 의존 에셋 요청 합류(관심 수), 단계별 합류(IO·worker·Ready — 같은 패키지 handle), 단계마다 취소(허가 대기·IO·worker(Prepare 없이 버림 — 그동안 로더 allocator 할당 0)·의존 대기·Pump 대기·공개 뒤 무영향) 뒤 버린 단위 정리·다시 요청하면 새 로드, 합류 요청 중 하나만 취소, 의존 단위를 직접 요청한 쪽만 취소(의존은 공개), Release(Pending) = 취소, 두 요청을 모두 놓으면 공유 의존까지 버림, 정리 중 재요청(버린 단위 + 새 단위, 경로 두 번 읽음, 옛 완료 먼저여도 성공 — Begin 실패로 실패한 순환도) | `AssetLoader_LoadsChainAndJoins`, `AssetLoader_CancelAtEachStage`, `AssetLoader_CancelOneOfJoinedRequests`, `AssetLoader_JoinAtEachStage`, `AssetLoader_CancelDirectRequestOfSharedDependency`, `AssetLoader_RequestAgainWhileRetiredUnitDrains` |
| | 실패·취소 겹침: 같은 Pump에서 완료 처리가 취소 확인보다 먼저(실패가 이긴다) | `AssetLoader_FailureWinsOverCancelInOnePump`, `AssetLoader_StagedCombinations` |
| | `CancelToken`(기본 token·복사·이동·대입·source 소멸 뒤 읽기·다른 스레드 Cancel·공유 상태 할당 실패) | `AssetLoader_CancelTokenContract` |
| 우선순위(11.6-8) | 허가 순서(Critical → High → Normal → Low, 같으면 생성 순), SetPriority 승격의 의존 전파·내리는 값은 단위 그대로·끝난 요청 무동작, 합류 요청의 승격 전파, Pump 공개 순서 | `AssetLoader_PriorityOrder`, `AssetLoader_InstantiateOrderFollowsPriority`, `AssetLoader_LoadsChainAndJoins` |
| 상한·aging(11.6-9) | 동시 진행 상한(정점 = 상한, 상한보다 큰 순환 단위는 진행 중이 없을 때 혼자 — 그 사이 뒤 후보도 허가 안 함), 임시 메모리 상한이 패키지 하나보다 작아도 하나씩, 임시 byte 포화(파일 + bulkRawBytes = 2⁶⁴ — 진행 중이 있으면 대기, 없으면 혼자, 진행 중 합 포화·끝나면 0), aging 1이면 오래 기다린 Low가 셋째 High보다 먼저·aging 없으면 맨 뒤 | `AssetLoader_AdmissionLimits`, `AssetLoader_AgingPreventsStarvation`, `AssetLoader_TempBytesSaturate` |
| Pump 예산(11.6-10) | maxUnits 2·0(최소 하나)·무제한, 시간 예산 경계 10·11·20·21·31 ns(단계 10 ns 시계), 소진 표시, 남은 단위의 대기 이유 Pump | `AssetLoader_PumpBudgetBoundaries` |
| 동기 대기(11.6-11) | WaitFor 결과·관찰자(호출 지점·대상)·횟수(이미 끝난 요청 포함), 프레임 안 WrongContext, 최근 8개 고리(10번 뒤 셋째부터 열째) | `AssetLoader_LoadsFromDiskWithWorkers`, `AssetLoader_RecentSyncWaitsAreKept` |
| 상태 관측(11.6-12) | 요청·단위·패키지 표(단계·대기 이유·기다리는 의존 에셋·우선순위·실효 우선순위·관심 수·임시 byte·누적 시간), 실패 단위의 원인(패키지·오류·진단), Loading 단위의 대기 대상은 PostLoad 중인 의존이 아니라 공개 안 된 의존, 이름 함수(모르는 값 "Unknown") | `AssetLoader_DescribeTracksStages`, `AssetLoader_LoadsChainAndJoins`, `AssetLoader_DescribeShowsFailedUnitCause`, `AssetLoader_DependencyWaitSkipsPostLoadingUnits` |
| 할당 실패(11.6-13) | 로더·Registry allocator 모든 지점(초기화·요청·합류·허가·긴 루트 경로·비ASCII 경로 색인·완료·Prepare·공개·Commit·상태 조회) → 그 단계 OutOfMemory(진단 None) 또는 의존 DependencyFailed, 독립 요청 50과 요청 1 계열이 함께 실패하지 않음(그 요청만), 같은 로더에서 실패를 끄고 다시 요청하면 성공 | `AssetLoader_AllocationFailures` |
| 종료(11.6-14) | Pending → ShuttingDown, 진행 중 IO가 끝날 때까지 대기(다른 스레드가 Game 스레드의 대기 진입 hook 뒤에 완료 — 대기 분기가 일정과 무관하게 닿는다), 버린·실패한 단위가 IO를 기다리는 채 종료, 허가 전 사슬, 공개 패키지 놓음, 뒤 Request·Pump·WaitFor ShuttingDown, 두 번째 Shutdown 성공 | `AssetLoader_ShutdownDrainsAndReleases` |
| 스레드·초기화(11.6-15) | 다른 스레드 호출 WrongThread(FindPackage 포함 모든 공개 함수·Shutdown), 소유 스레드 밖 소멸은 자식 프로세스 Fatal(문구 대조), 설정 누락·0 한도·다른 TypeRegistry InvalidArgument, 프로파일 다름 IncompatibleCook, 두 번째 Initialize, 초기화 전 호출, 상태 할당 실패(진단 out 없음) | `AssetLoader_RejectsOtherThreads`, `AssetLoader_DestroyOffOwnerThreadIsFatal`, `AssetLoader_InitializeAndArguments` |
| PostLoad(11.6-16) | 의존 단위 먼저·단위 안 AssetId 순(2·3·4·1), maxPostLoads(0이어도 Pump마다 하나)·시간 예산(첫 호출 뒤 다하면 소진 표시)·공개가 단위 예산을 다 써도 PostLoad 하나(굶주림 방지), 요청은 PostLoad 뒤 Succeeded(그 전 PostLoading — 의존 PostLoad 대기 → 자기 차례), PostLoading 단위 합류, PostLoading 중 취소는 Cancelled지만 PostLoad 불림, PostLoad 중인 의존에 기대는 새 단위는 그 뒤(그 의존을 공개 대기로 세지 않음), 종료는 남은 PostLoad를 부르지 않음, PostLoad 대기 중 기대는 단위가 실패해도 그 단위는 버려지지 않고 PostLoad됨·의존이 아직 PostLoad 대기일 때 고쳐 다시 요청하면 새 단위는 바로 허가 후보(Admission)이고 공개 뒤 PostLoadDependency로 기다려 의존 뒤에 PostLoad, handler가 받은 package = FindPackage | `AssetLoader_PostLoadOrderAndBudget`, `AssetLoader_PostLoadTimeBudget`, `AssetLoader_PostLoadRunsWhenInstantiateBudgetIsSpent`, `AssetLoader_DependentFailsWhileDependencyPostLoads`, `AssetLoader_PostLoadJoinCancelAndShutdown`, `AssetLoader_DependencyWaitSkipsPostLoadingUnits` |
| 콜백 재진입(11.6-17·18) | PostLoad handler·동기 대기 관찰자·완료 대기 진입 hook(큐 뮤텍스를 쥔 채 — 다른 스레드가 그 뒤에 완료)·IO Begin·IO Release·worker Run·시계·프레임 판정 안의 Request·SetPriority·Release·Pump·WaitForCompletions·WaitFor·Shutdown은 WrongContext, Status·Describe·FindByPath는 됨(교착 없음), 로더는 이어서 정상 완료, 콜백 안 파괴는 자식 프로세스 Fatal(문구 대조) | `AssetLoader_CallbacksCannotReenter`, `AssetLoader_BoundaryCallbacksCannotReenter`, `AssetLoader_DestroyOffOwnerThreadIsFatal` |
| | 공개 트랜잭션 안의 사용자 코드: 클래스 생성자(`InstantiatePackages` — `TestHookAsset`)와 공개 실패 롤백의 소멸자(오래된 의존 쿠킹 결과 → TypeMismatch) 안의 같은 일곱 호출은 WrongContext, 읽기는 됨, hook이 실제로 불림(생성·소멸 횟수), 뒤 요청은 정상 완료(성공한 쪽은 Succeeded·공개, 실패한 쪽은 TypeMismatch) — `Commit`은 같은 범위 guard 안이다 | `AssetLoader_TransactionCodeCannotReenter` |
| 조합(14 완료 조건 2) | 요청 단위 모양 3 × 의존 4 × 결함 5 × 진행 방식 2의 허가 조합 106개 전부(금지: 단일의 형제 손상·의존 없음의 의존 손상), 번호 ↔ 조합 일대일·실행 수로 빠짐 검사 | `AssetLoader_AllCombinations` |
| | 단계 조합: 모양 2 × 의존 상태 5(없음·새로·진행 중·PostLoad 대기·공개됨) × 결함 단계 5(없음·IO 완료 오류·Begin 즉시 실패·worker 손상·공개 실패 — 오래된 쿠킹 결과를 Hard로 가리킴) × 결함 대상 2(요청 패키지·의존) × 합류 3(없음·처음·PostLoad 대기 때) × 취소 시점 6(없음·허가 전·IO·worker·Ready·PostLoad 대기) × 완료 순서 2 × PostLoad handler 2(없음·기록)의 허가 조합 3496개(handler 있음 2376·없음 1120). 금지: 결함 없음의 대상 의존(대상이 없어 요청 패키지와 같은 조합), 의존 없음·PostLoad 대기·공개된 의존의 의존 결함(실패할 의존이 없거나 이미 공개됨), handler 없이 PostLoad 대기 의존·합류·취소(PostLoad 대기가 생기지 않음). 실패가 취소보다 먼저인 시점(같은 Pump에서 완료 처리가 취소 확인보다 먼저): Begin은 허가 전 취소 밖 전부, IO 완료 오류는 IO 취소부터, worker 손상은 Ready 취소부터, 공개 실패는 PostLoad 대기 취소만(Ready 취소는 공개 전에 버린다). 조합마다 요청 상태(결함별 진단 — 오류·진단 코드·관련 에셋, 합류·진행 중 의존 요청 포함)·모든 구성·의존·오래된 대상 패키지의 공개 여부(공개 실패면 의존·오래된 대상은 공개 Pump까지 살아 있으면 공개된 채)·PostLoad 정확히 한 번·의존 먼저·handler package = FindPackage, 끝나면 단위·IO·임시 byte 0, 번호 ↔ 조합 일대일·실행 수(시험 시간 약 1.0 s) | `AssetLoader_StagedCombinations` |
| | 결함 단계·종료 조합: 결함 단계 5(없음·IO 완료 오류·Begin 즉시 실패·worker 손상·공개 실패 — 오래된 의존 쿠킹 결과의 대표 타입) × 의존 2(없음·Hard) × 종료 시점 5(없음·IO·worker·Ready·PostLoad 대기) × 허가 상한 2(무제한·1) × 종료 시점의 단위 2(요청 패키지·의존)의 허가 조합 110개. 금지: 의존 없는 공개 실패(오래된 의존이 없음), 의존 없는 조합의 단위 의존, 무제한·종료 없음의 단위 의존(무제한은 의존과 요청 패키지가 같은 단계로 함께 가고, 종료가 없으면 시점이 없어 요청 패키지와 같은 조합). 무제한은 한 로더에서 모든 장면을 함께, 상한 1은 조합마다 로더를 따로 띄우고(장면 파일은 함께 쓴다) Critical 순환 감시 단위 하나(10개 — 진행 중이 없을 때 혼자 허가)가 Pump마다 PostLoad를 가져가 의존을 PostLoad 대기에 둔 채 단계를 맞춘다. 상한 1의 기대: 의존 단계에서 종료하면 요청 단위는 Queued(대기 이유 Admission)라 ShuttingDown이고 의존은 PostLoad 대기 시점에만 공개돼 있다, 요청 패키지 단계에서 종료하면 의존은 공개된 채 PostLoad 대기(아직 PostLoad 안 됨)이고 요청은 그 시점까지 확정된 실패면 그 진단, 아니면 ShuttingDown. 모든 종료 조합에서 종료는 공개 패키지를 놓고 남은 PostLoad를 부르지 않는다. 종료 없음은 결함별 진단·공개 여부·PostLoad 횟수·상한 1이면 의존이 먼저 끝까지. 끝나면 단위·IO·임시 byte 0, 번호 ↔ 조합 일대일·실행 수(시험 시간 약 0.1 s) | `AssetLoader_FaultStageAndShutdownCombinations` |
| | 요청 관심·lease·scope·OnReady callback·상주 단위 해제(Closing·Discard)·ReadyMask·expectedType | 범위 밖(A1d AssetManager) |
| | 클래스별 PostLoad dispatch(`UObject::PostLoad` 대응 클래스 hook 등록) | 범위 밖(A5 — RuntimeClassMetadata와 함께, 단계·handler 경계는 여기) |
| | 미준비 객체 동작 정책(11.5절 2) | 범위 밖(A1d) |
| | Rocks Lens 이벤트 연결·프레임 판정 주입 | 범위 밖(EngineAssets — 관찰자·판정 함수는 여기) |

#### 14.1.6 Coverage(A1c)

측정(2026-11-20, 14.1.4와 같은 명령에 filter `AssetLoader`를 더함 — `-Filter AssetPackage,FixtureAsset,CookedManifest,AssetLoader`, AssetPackage 71개·FixtureAsset 8개·CookedManifest 16개·AssetLoader 47개 시험 모두 통과):

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `Private/AssetLoader.cpp` | 99.8%(1626/1630) | 99.7%(648/650) | 100%(89/89) |
| `Private/CancelToken.cpp` | 100%(90/90) | 100%(24/24) | 100%(17/17) |
| `Private/CookedPackageIo.cpp` | 100%(77/77) | 100%(24/24) | 100%(6/6) |
| `Private/CookedManifest.cpp`(파일·내용 대조 분리 뒤) | 100%(559/559) | 100%(298/298) | 100%(28/28) |
| `Public/AssetRuntime/AssetLoader.h`·`CancelToken.h` | 100%(7/7·8/8) | 분기 없음 | 100%(3/3·4/4) |

AssetRuntime 전체는 줄 99.82%(5042/5051)·분기 98.09%(2561/2611)·함수 100%(279/279)이고, A1a 파일의 닿지 않은 분기는 14.1.2 그대로다.

이 측정은 Coverage 구성(개발용 기능이 있는 비 Release)이다. Release 구성(UE Shipping)에서는 PlatformBase 결과 주입이 빠지므로 `CookedPackageIo.cpp`의 OS 오류 세 분기(읽기 완료 실패·즉시 제출 실패·크기 조회 실패)는 구성상 닿지 않는다(짧은 읽기는 결과 주입 없이 닿는다). Release에서 돈 시험(2026-11-20): EngineTests Release `--filter AssetLoader` 47개 통과(디스크 시험의 결과 주입 장면은 빠진 채), FoundationTests Release `--filter AsyncFile` 13개 통과(`AsyncFile_InjectedFaultsAreDeterministic`는 Release에 없다).

닿지 않은 곳(`AssetLoader.cpp` 두 분기와 그 네 줄): 소멸자의 "소유 스레드가 아님 → `platform::Fatal`"과 "콜백 안 파괴 → `platform::Fatal`"이다. 자식 프로세스 probe(`AssetLoader_DestroyOffOwnerThreadIsFatal` — 문구 대조)가 실제로 지나지만, Fatal은 프로세스를 즉시 끝내 그 자식의 coverage profile이 기록되지 않는다(측정은 부모 프로세스 profile만 모은다).

닿게 한 방법과 그 과정에서 바꾼 것:
- 결정적 순서: IO·worker를 시험이 내보내는 구현(`TestPackageIo`·`TestLoaderWorker` — 앞부터·뒤부터 완료)으로 단계마다 멈추고, 실제 스레드 시험(디스크·WorkerScheduler·다른 스레드 호출·종료)은 결과만 단언한다. 종료 대기는 대기 진입 hook(11.6절 17)이 보낸 사건 뒤에 다른 스레드가 완료를 내보내 대기 분기에 일정과 무관하게 닿는다. 시간 예산은 부를 때마다 정해진 만큼 가는 시계로 본다.
- 기본 IO의 OS 오류 경로: PlatformBase 결과 주입(`AsyncIoContext::InjectFault` — 크기 조회·조각 제출·완료, PlatformLayer 문서)으로 결정적으로 닿고, 짧은 읽기는 완료 게시 보류 사이에 파일을 다시 써서 닿는다. 처음 이 네 분기는 "결정적으로 만들 수 없다"고 적었는데 틀렸다(완료 보류가 이미 있었고 주입 지점은 기반을 보강해 더했다).
- 할당 실패: 로더 allocator와 에셋 Registry allocator를 같은 NFailAllocator로 묶어 모든 지점을 돈다(초기화·요청·합류·독립 요청·Tarjan 작업 공간(깊이 16 사슬)·허가·긴 쿠킹 루트·비ASCII 경로 색인 키·완료·Prepare·공개·Commit·상태 조회).
- 처음 구현에서 없앤 죽은 분기(입력으로 닿을 수 없음을 확인): 정리 목록에 이미 Failed·Abandoned인 단위가 다시 오는 검사, 관심 수 0에서의 감소 방어, 지울 작업의 IO 반납, 구성 항목의 단위 번호 대조, 허가 때 기대는 단위의 미허가 수·단계 방어, 임시 byte 상한의 중복 비교, 버린 작업 찾기 실패, Pending 요청의 단위 없음, Pump 반복 조건, WaitFor 안의 Pump 실패(검사 뒤 내부 Pump를 부른다), IO `Release`의 상태 없음. Loading 단위의 대기 대상 탐색은 "목록 끝까지 못 찾음"이 불변식상 없으므로(공개 안 된 의존이 있어야 Loading이다) 끝 검사 없는 탐색으로 썼다. 여러 할당 단계의 처음 실패를 모으던 템플릿 도우미는 비템플릿 `KeepFirst`로 바꿨다.
- 리뷰가 찾은 것(판정 "미충족" 반영): 임시 byte 합의 감김(포화 덧셈으로), `FindPackage`의 스레드 확인 누락, 실패 단위 진단·최근 동기 대기 위치의 상태 관측 누락, 실패·취소 겹침 규칙 미기술, PostLoad 단계 누락(11.6절 16으로 구현), Initialize의 매니페스트 재검증 누락, 소유 스레드 밖 소멸, 시험 runner의 process probe 등록 칸(8 → 32 — 로더 probe를 더하자 넘쳐 다른 probe 시험이 실패했다). 재판정 반영: 공개가 예산을 다 쓰면 PostLoad를 한 번도 부르지 않던 굶주림, PostLoad handler가 단위 참조를 쥔 채 재진입할 수 있던 수명 결함(콜백 중 표시 — 11.6절 18).
- 시험이 찾은 것: 내용 손상 시험이 처음에 MissingFile로 나왔다 — 패키지 경로가 내용 주소(SHA-256 앞부분)라 매니페스트 SHA를 고치면 경로도 바뀐다(로더 동작이 맞고 시험이 새 이름으로 다시 등록하게 고쳤다).


A1c 상태(2026-11-20): **구현**. 완료 조건 넷을 모두 갖췄다. 빠진 시험 리뷰를 다섯 번 거쳤다(미충족 네 번 — overflow·스레드 확인·PostLoad 단계·콜백 재진입·조합 축 등 — 을 반영, 다섯 번째 판정 충족, f044b621 기준).
- 요구 대조표(14.1.5): 시험 없는 항목 0, 정의된 시험 47개를 모두 인용
- 조합: 단계 조합 3496개·결함 단계와 종료 조합 110개, 금지 조합은 명시하고 일대일·실행 수로 빠짐 검사
- coverage(14.1.6): 닿지 않은 곳은 소멸자 Fatal 둘뿐(자식 probe가 문구 대조), Release 구성에서 빠지는 결과 주입은 따로 적음
- 리뷰어 참고(병합을 막지 않음): `ActiveTestHookAssetHooks()`는 전역 하나라 시험 병렬 실행 때 다시 볼 것, 결함 단계와 종료 조합에는 취소·합류 축이 없다(`ShutdownDrainsAndReleases`가 덮음)

#### 14.1.7 요구 대조표(A1d — AssetManager)

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/AssetRuntime/Tests/ManagerLifetimeTests.cpp`·`ManagerHandleTests.cpp`·`ManagerPrimaryTests.cpp`·`ManagerFailureTests.cpp`·`ManagerContractTests.cpp`·`ManagerGraphTests.cpp`의 AssetManager 시험 51개와 `LoaderHandoffTests.cpp`의 로더 넘겨받기 시험 6개 — 정의된 시험 57개를 모두 표가 인용한다). 시험 도구(`ManagerTestSupport.h`)는 로더 시험의 쿠킹 장면·완료를 시험이 내보내는 IO·손으로 돌리는 worker 위에 AssetManager 하나를 띄우고, 상태 조회(`Describe`로 단위 찾기)·동적 하드 간선(`hard` 필드·`hardList` 쓰기·비우기)·callback 기록을 돕는다. 순서는 모두 시험이 정하고(시간·경쟁에 기대지 않는다), 다른 스레드 시험은 스레드 Join 뒤 Pump로 결과만 단언한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 구성·소유·스레드(11.7-1) | Initialize 전 모든 API InvalidArgument(Shutdown은 무동작 성공), 설정 누락(allocator·TypeRegistry·매니페스트)·Registry를 넘김·관찰자 키 0·보류 명령 상한 0은 InvalidArgument와 진단, 두 번째 Initialize, 다른 스레드의 모든 API(scope Revoke·로더 넘겨받기 API·Shutdown 포함) WrongThread이고 상태 불변, 기본 scope의 Revoke InvalidArgument, `With`가 lease의 대표 에셋을 에셋 문맥으로 부름 | `AssetManager_UninitializedAndBadConfig`, `AssetManager_OffThreadCallsAreRejected`, `AssetManager_HandleEdgeCases` |
| | `Registry()` 허용 쪽: 상주 패키지 안 생성 트랜잭션·필드 편집의 간선이 계수됨. `Registry()`로 넘겨받은 패키지의 수명을 바꾸거나 `Loader()`로 요청·넘겨받기 API를 직접 부르는 것은 진단하지 않는 계약 위반(헤더·11.7-1에 명시 — 시험 대상 아님) | `AssetManager_EdgeBatchCoalescesPerPair` |
| 로더 확장(11.7-2) | `TakePublished`는 PostLoad까지 끝난 단위만(PostLoad 예산 1로 순환 단위의 PostLoad 하나 뒤에는 넘기지 않음), 같은 단위는 같은 group·다른 slot, 다른 단위는 큰 group, out 기존 내용 유지, 다시 부르면 없음 | `AssetLoader_TakePublishedAfterPostLoad` |
| | `PublishedHolds`: IO 중인 의존자가 있으면 1(그동안 퇴출 거부)·끝나면 0, 한 단위의 두 구성원이 같은 공개 패키지에 기대도 1, 공개 안 된 id 0·없는 id NotFound, 의존 단위가 PostLoad를 끝내기 전에 공개된 의존자는 넘겨받는 보유가 없음(공개 때 놓음) | `AssetLoader_PublishedHoldsTrackLoadingDependents`, `AssetLoader_DependencyFinishesAfterDependentPublished` |
| | `RetirePublished`: 없는 id NotFound·공개 안 됨·PostLoad 중·같은 id 두 번 InvalidArgument이고 앞의 유효한 id도 그대로(모두 확인 뒤 바꿈), 퇴출 뒤 FindPackage NotFound·같은 id 요청은 새 로드, Shutdown 뒤 ShuttingDown. `ReleaseRetired`: 범위 밖·퇴출 아닌 칸·같은 칸 두 번 InvalidArgument, 할당 실패는 아무것도 바꾸지 않음, 돌려준 칸은 다음 공개가 다시 씀. `TakePublished` 할당 실패는 목록을 남김 | `AssetLoader_RetireAndReleaseRetired` |
| | 네 API의 초기화 전 InvalidArgument·다른 스레드 WrongThread, PostLoad 안에서 `TakePublished`·`RetirePublished`·`ReleaseRetired` WrongContext(`PublishedHolds`는 읽기라 됨) | `AssetLoader_HandoffContractErrors` |
| | 공개 패키지에 기대는 요청의 모든 할당 지점 실패 → 그 요청만 OutOfMemory, 공개 보유가 남지 않음, 다시 요청하면 성공 | `AssetLoader_PublishedHoldAllocationFailures` |
| | 로드 중 의존자의 로더 공개 보유가 상주 의존을 붙잡고, 의존자가 공개되면 Commit 간선이 이어서 붙잡음 | `AssetManager_LoadingDependentHoldsResidentDependency` |
| 상주 단위·계수(11.7-3, 시나리오 16·23·28) | 티켓·lease가 붙잡고 둘 다 놓으면 Discard, 같은 id는 새 로드(다른 패키지), 하드 순환 단위는 함께 Discard(내부 간선은 보유 아님), 의존 보유와 연쇄 Discard(다음 Pump), 동적 간선 다중도(필드 둘 → 2, 하나 지우면 1), 동적 순환 병합과 나누지 않음 | `AssetManager_ReleaseDiscardsAndReloads`, `AssetManager_HardCycleUnitReleasesTogether`, `AssetManager_DependencyHeldByEdgeUntilReferrerDiscarded`, `AssetManager_DynamicEdgeMultiplicityHolds`, `AssetManager_DynamicCycleMergesUnits` |
| | 병합 경계: 순환 탐색은 닿은 뒤에도 더 가고 전방 집합 밖에서 오는 간선은 거름(11·12·13만 합침, 보유는 10에서 오는 간선 하나), 한 Pump에 몰린 네 간선은 지운 단위의 남은 후보를 건너뛰고 한 단위, 쿠킹 간선을 모두 지운 단위({1, 2})는 간선 칸 없는 멤버를 가진 채 남고 새 순환에서 셋을 합침·지운 간선 칸 재사용 | `AssetManager_CycleSearchBoundaries`, `AssetManager_BatchedEdgesMergeOnce`, `AssetManager_MergeKeepsEdgelessMembers` |
| | 넘겨받기 전에 쌓인 간선: PostLoad 예산 1로 Commit과 넘겨받기 사이 Pump에 생긴 간선(상주 5 → 아직 넘어오지 않은 1)은 표에만 있고 순환을 보지 않음(아직 넘어오지 않은 1의 Commit 간선 1 → 5는 5의 보유), 넘겨받을 때 들어오는·나가는 간선으로 합침. 함께 PostLoad 중인 두 단위 사이 간선은 먼저 넘어오는 쪽이 상주 아닌 상대를 건너뛰고 나중에 합침, 그 사이 상주 단위끼리의 순환 탐색은 상주 아닌 패키지로 가는·에서 오는 간선을 지남 | `AssetManager_EdgesBeforeIngestMergeCycles`, `AssetManager_ConcurrentPostLoadUnitsMergeOnIngest` |
| | 한 묶음 안 같은 쌍의 여러 항목(관찰자 키가 원인·트랜잭션으로 나눔 — Edit −1이 Commit +1보다 먼저): 쌍별로 합쳐 판정해 Fatal 없음, 변형 셋(0인 쌍만 — 끝에서 뺌, 0인 쌍 뒤 다른 쌍 — 가운데서 뺌, +1 남김 — 순환 병합) | `AssetManager_EdgeBatchCoalescesPerPair` |
| | Closing과 되살리기(Discard 예산 0으로 멈춤 → 요청이 그 자리에서 보유), 첫 Discard Busy(열린 `ConstructionContext`)면 Closing으로 남고 되살릴 수 있고 트랜잭션이 닫히면 Discard | `AssetManager_ClosingUnitRevivesOnRequest`, `AssetManager_DiscardBusyKeepsClosing` |
| | Discarding: 순환 단위의 둘째가 Busy면 Discarding이고 로더에서 둘 다 퇴출, 같은 id는 새 로드(옛 단위와 함께), 옛 패키지로 생긴 늦은 간선은 `lateEdgesIntoDiscarding`(둘째부터 경고 없이 셈, 지우기는 세지 않음), 옛 패키지에서 나가는 새 간선은 보유지만 순환 후보 아님, 순환 탐색은 Discarding 단위를 지나지 않음, 트랜잭션이 닫히면 끝나고 반납 간선은 다음 Pump. Discard 예산 1이면 단위 중간에서 멈추고 다음 Pump가 이어감 | `AssetManager_DiscardingRetiresAndReportsLateEdges`, `AssetManager_DiscardBudgetStopsMidUnit` |
| Pump 순서(11.7-4) | 요청이 상주 대상에 그 자리에서 보유(같은 Pump의 Closing에 먼저 Discard되지 않음), 연쇄 Discard가 다음 Pump, 넘겨받기가 간선 적용보다 먼저 | `AssetManager_ClosingUnitRevivesOnRequest`, `AssetManager_DependencyHeldByEdgeUntilReferrerDiscarded`, `AssetManager_EdgesBeforeIngestMergeCycles` |
| 티켓·callback(11.7-5) | 끝난 뒤 Pump에서 정확히 한 번, AlreadyRegistered(callback은 caller에), Game이 아닌 lane Unsupported, 빈 callback InvalidArgument(묶음 callback 포함), 늦은 등록은 다음 Pump, 먼저 놓은 티켓 Cancelled, 로드 실패 진단, 기대 타입 TypeMismatch, Render 준비 UnsupportedCapability·모르는 비트·0 InvalidArgument, 기본 handle StaleHandle, 없는 id NotFound(티켓 칸을 남기지 않음), 무효 scope InvalidArgument | `AssetManager_ReadyCallbackContract`, `AssetManager_DroppedTicketCancelsAndFailureCarriesDiagnostic`, `AssetManager_ExpectedTypeAndReadyMask`, `AssetManager_InvalidHandlesAndArguments` |
| | 끝난 티켓을 callback 전달 전에 놓으면 Cancelled 한 번·보유 0·다음 Pump Discard(등록 뒤 놓기, 끝난 뒤 늦게 등록하고 놓기) | `AssetManager_DoneTicketDroppedBeforeDeliveryIsCancelled` |
| | callback 안에서 허용된 호출(빈 티켓 칸이 없을 때 요청·등록·묶음 로드·보류 명령·lease 놓기 — 표와 전달 목록이 다시 잡혀도 같은 목록의 둘째 callback이 정확히 한 번, 새 티켓 callback은 다음 Pump, 묶음 callback·보류 명령은 같은 Pump의 뒤 차례) | `AssetManager_CallbackMayRequestAndRelease` |
| | callback 안의 Pump·Shutdown·LoadBlocking WrongContext, 로더 PostLoad 안의 LoadBlocking·Pump WrongContext(로더가 거절) | `AssetManager_CallbackContextRejectsPumpAndShutdown`, `AssetManager_HandleEdgeCases`, `AssetManager_LoadBlockingFailures` |
| 슬롯·해제 큐(11.7-6) | 다른 스레드 소멸 → 해제 큐 → 다음 Pump, handle 이동 대입(쥔 handle 위에 대입하면 옛 것을 놓음·자기 대입 무동작·scope는 Revoke), 기본 lease 무효(빈 id·handle), 소멸했지만 큐에 남은 lease도 scope 회수가 회수하고 큐 처리는 다시 빼지 않음, 상한 `AssetManagerMaxLiveHandles`(새 요청·보류 명령·묶음·동기 로드 CapacityExceeded, 걸어 둔 callback·보류 명령·묶음 callback은 그 오류, 하나 놓으면 다음 Pump 뒤 다시 됨) | `AssetManager_ReleaseFromOtherThread`, `AssetManager_HandleMoveAssignment`, `AssetManager_HandleEdgeCases`, `AssetManager_RevokeSkipsReleasedLease`, `AssetManager_HandleCapacity` |
| | 수명 위반은 자식 프로세스 Fatal(문구 대조): 살아 있는 lease를 남긴 파괴, 소유 스레드 밖 파괴, 자기 callback 안 파괴, 소유 스레드 밖 scope 소멸 | `AssetManager_LifetimeViolationsAreFatal` |
| scope(11.7-7) | Revoke: 진행 중 callback ScopeClosed 한 번·lease 회수·새 요청 ScopeClosed·다른 scope는 그대로·소멸도 Revoke, 닫은 scope 칸 재사용, 회수된 동기 lease의 `With`는 StaleHandle, 닫힌 scope의 LoadBlocking ScopeClosed, 두 번째 Revoke 무동작, Shutdown 뒤 회수하지 않은 scope의 Revoke·소멸은 다시 회수하지 않음 | `AssetManager_ScopeRevokeReclaims`, `AssetManager_HandleEdgeCases`, `AssetManager_InvalidHandlesAndArguments`, `AssetManager_AfterShutdown`, `AssetManager_ScopeOutlivesShutdown` |
| LoadBlocking(11.7-8) | 결과까지 기다려 lease, 프레임 실행 중 WrongContext(Pump 중 판정과 설정 판정 둘 다), 없는 id NotFound와 진단, 손상 패키지는 오류와 진단을 failure에, 준비 수준 0 InvalidArgument, 열린 scope로 동기 로드, 처음 판정은 거짓이고 로더 대기가 다시 볼 때 참이면 로더가 WrongContext(티켓·lease 0) | `AssetManager_LoadBlockingWaitsAndRejectsFrames`, `AssetManager_LoadBlockingInFrame`, `AssetManager_LoadBlockingFrameTurnsDuringWait`, `AssetManager_LoadBlockingFailures`, `AssetManager_InvalidHandlesAndArguments`, `AssetManager_HandleEdgeCases` |
| 미준비 대상(11.7-9) | Fail은 상주가 아니면 NotReady·상주면 다음 Pump run, Defer·PromoteAndDefer는 요청을 내고 준비되면 run, 실패·Cancel·scope Revoke는 fail 한 번, 큐 상한 CapacityExceeded, 빈 run·fail·범위 밖 정책 InvalidArgument, 끝난 명령 칸 재사용, 두 번 Cancel·끝난 뒤 Cancel 무동작(fail 한 번), 명령 handle을 먼저 놓아도 실행 | `AssetManager_RunWhenReadyPolicies`, `AssetManager_HandleEdgeCases`, `AssetManager_HandleMoveAssignment` |
| PrimaryAsset·bundle(11.7-10) | bundle 없이는 대표와 Hard 의존만, caseless bundle 이름, 구성원 로드와 lease 순서(대표 → AssetId), bundle 사이 중복 한 번, 오류(PrimaryAsset 아님·없는 bundle·잘못된 UTF-8 이름 NotFound·없는 id·AlreadyRegistered·lane), 처음 실패, 먼저 놓은 묶음 티켓 Cancelled·scope Revoke ScopeClosed, 회수된 묶음은 티켓을 놓아도 ScopeClosed 한 번, 끝났지만 callback 전에 놓은 묶음은 Cancelled이고 같은 Pump의 회수가 다시 끝내지 않음, 용량 추정(로드 안 함, bulkRawBytes 최댓값 둘의 합은 최댓값 — 포화) | `AssetManager_EstimatePrimaryAssetSaturates`, `AssetManager_LoadPrimaryAssetWithBundles`, `AssetManager_LoadPrimaryAssetErrors`, `AssetManager_LoadPrimaryAssetFirstFailure`, `AssetManager_PrimaryTicketCancelAndRevoke`, `AssetManager_PrimaryReleaseOrderings`, `AssetManager_EstimatePrimaryAsset`, `AssetManager_InvalidHandlesAndArguments` |
| 관측(11.7-11) | 단위별 보유 내역·멤버 id·살아 있는 티켓·lease·보류 명령 수·로더 상태, 다시 부르면 비우고 채움, 초기화 전 InvalidArgument·다른 스레드 WrongThread, out 할당의 모든 지점 OutOfMemory | `AssetManager_DescribeReportsState` |
| 종료(11.7-12) | 아직 부르지 않은 callback·보류 명령 fail에 ShuttingDown 한 번, lease 회수, 뒤 요청·Pump·scope·보류 명령·묶음 로드 ShuttingDown, 두 번째 Shutdown 성공, 종료 전달 안(티켓 callback·보류 명령 fail·묶음 callback)의 요청·보류 명령·묶음 로드·scope 만들기·다른 티켓·묶음 티켓의 OnReady ShuttingDown, 종료 뒤 살아 있는 티켓·묶음 티켓의 OnReady ShuttingDown(callback은 caller에 남음) | `AssetManager_ShutdownDeliversShuttingDown`, `AssetManager_AfterShutdown`, `AssetManager_ShutdownDeliveryRejectsNewWork` |
| 할당 실패 | AssetManager allocator의 모든 지점(Initialize·묶음 로드(전달 목록 확장을 묶음 쪽에서)·scope·요청·callback·보류 명령·비ASCII bundle 이름 키·용량 추정·넘겨받기·간선 표·병합·Discard·lease·동기 로드) 한 번 실패 → 상태 일관, 실패를 끄고 모두 놓으면 단위·handle·로더 단위 0, 마지막 실행은 실패 없이 전 과정 | `AssetManager_AllocationFailures` |
| 조합(14 완료 조건 2) | 오래 쥐는 보유 4종(티켓·lease·묶음 lease·들어오는 하드 간선) × 단위 모양 3(단일·순환·사슬) × 해제 순서 4! = 24(계승 진법 — 서로 다른 순서 24개 확인) × 해제 방식 3(객체 소멸·scope Revoke·다른 스레드 소멸 → Join, 간선은 필드 비우기) × 꼬리 4(없음·같은 프레임 보류 명령 — 보유는 보류 칸 1(티켓 칸 아님)이고 한 Pump 붙잡고 run 한 번 뒤 0·같은 프레임 로더 공개 보유 — 4의 IO 동안 로더 보유, 공개 뒤 Commit 간선, 4를 놓은 다음 Pump에 Discard·Closing 대기 중 revive) = 864 전부: 마지막 보유 전까지 Resident와 남은 보유 수, 꼬리별 붙잡음과 Discard 시점(사슬은 다음 Pump에 의존), 보유자 유지. 보류 명령과 로더 공개 보유는 한 Pump만 살거나 간선 보유로 넘어가는 보유라 순서 축이 아니라 꼬리 축이다. Shutdown 해제는 모든 보유를 한 번에 놓으므로 `AssetManager_ShutdownDeliversShuttingDown`·`AssetManager_AfterShutdown`이 덮는다 | `AssetManager_HoldReleaseOrderCombinations` |
| | 매니페스트 bundle 구성원 축(564 조합)은 14.1.3 조합 행 | `CookedManifest_AllCombinations` |
| | bundle 구성원 수집(리플렉션 `AssetBundles` 지정자 → 매니페스트) | 범위 밖(A4 쿠커 — 형식·로드는 여기) |
| | 엔진 프레임 연결(EngineSubsystem 등록·프레임 Pump·에셋 Registry GC 조각·프레임 판정 주입·`-CookedRoot`) | 범위 밖(A1e EngineAssets — 판정 함수·Pump는 여기) |
| | `ReadyMask::Render` 준비 | 범위 밖(렌더 단계 — 지금은 UnsupportedCapability) |

#### 14.1.8 Coverage(A1d)

측정(2026-11-20, `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter AssetPackage,FixtureAsset,CookedManifest,AssetLoader,AssetManager -Sources Engine/Source/Runtime/AssetRuntime/Private,Engine/Source/Runtime/AssetRuntime/Public`, AssetPackage 74개·FixtureAsset 8개·CookedManifest 17개·AssetLoader 53개·AssetManager 51개 시험 모두 통과, 리뷰 반영 뒤 2026-11-21 재측정):

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `Private/AssetManager.cpp` | 98.7%(600/608) | 96.7%(267/276) | 100%(41/41) |
| `Private/AssetManagerHandles.cpp` | 98.7%(388/393) | 95.2%(177/186) | 100%(22/22) |
| `Private/AssetManagerPrimary.cpp` | 99.5%(373/375) | 98.8%(164/166) | 100%(18/18) |
| `Private/AssetManagerUnits.cpp` | 98.9%(521/527) | 96.3%(260/270) | 100%(22/22) |
| `Private/AssetManagerInternal.h` | 100%(8/8) | 분기 없음 | 100%(4/4) |
| `Public/AssetRuntime/AssetManager.h` | 95.6%(43/45) | 50%(1/2) | 100%(20/20) |
| `Private/AssetLoader.cpp`(분할 뒤 공개 API) | 99.4%(674/678) | 99.4%(336/338) | 100%(28/28) |
| `Private/AssetLoaderResolve.cpp`·`AssetLoaderStages.cpp`·`AssetLoaderPublish.cpp`·`AssetLoaderInternal.h` | 100%(441/441·352/352·236/236·80/80) | 100%(164/164·108/108·104/104·16/16) | 100% |
| `Private/CookedManifest.cpp` | 100%(611/611) | 100%(340/340) | 100%(29/29) |

AssetRuntime 전체는 줄 99.56%(7187/7219)·분기 97.77%(3552/3633)·함수 100%(414/414)이고, A1a 파일의 닿지 않은 분기는 14.1.2 그대로다.

닿지 않은 곳과 근거(위치는 함수 이름 — 줄 번호는 밀린다):

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `~AssetManager`·`AssetScope::Close`·`~AssetLoader` | 소유 스레드 밖·자기 callback 안 파괴, 살아 있는 handle을 남긴 파괴, 소유 스레드 밖 scope 소멸의 `platform::Fatal` | 자식 프로세스 probe(`AssetManager_LifetimeViolationsAreFatal`·`AssetLoader_DestroyOffOwnerThreadIsFatal` — 문구 대조)가 지나지만 Fatal은 프로세스를 즉시 끝내 그 profile이 기록되지 않는다 |
| `Impl::DrainReleases` | `ManagerSlotKind::Free` → Fatal, 해제 switch의 Free 쪽 | 해제 큐에는 살아 있는 handle 객체의 칸만 들어간다(소멸자가 넣고, 칸은 큐 처리 때만 Free가 된다) — 불변식 위반 진단 |
| `Impl::ApplyEdgeChanges` | 다중도 음수 → Fatal, `before == 0`이면서 `after ≤ 0`, `after == 0`이면서 표에 없음, 새 쌍의 `after ≤ 0` | 묶음을 받자마자 쌍별로 합치고 0인 쌍은 뺀다(`CoalesceChanges`) — 쌍별 합은 Registry가 실제로 바꾼 다중도라 표에 없는 쌍의 합은 양수이고 표 값을 음수로 만들지 못한다. 처음 근거("키마다 순 변화")는 틀렸다: 관찰자 키는 원인·트랜잭션까지 나눠 같은 쌍의 음수 항목이 먼저 올 수 있었고 Fatal에 닿았다(리뷰 — 고치고 `AssetManager_EdgeBatchCoalescesPerPair`로 시험, 고치기 전 코드에서 그 시험이 Fatal로 끝남을 확인) |
| `Impl::ApplyEdgeChanges` | `TakeChanges` 실패 | 실패 조건(소유 스레드 아님·out이 비지 않음·다른 allocator·예약 콜백 재진입 Busy)을 AssetManager가 만들지 않는다: Pump는 소유 스레드 전용, 묶음을 다 적용한 뒤에만 비어 있는 `changes`로 부르고, 관찰자와 같은 allocator를 쓰고, 할당 콜백 안에서 Pump를 부를 수 없다(사용자 allocator의 재진입은 계약 위반) |
| `Impl::UnlinkEdge` | 목록 끝까지 못 찾음 | 표에 있는 키의 간선은 양쪽 이웃 목록에 반드시 있다(`LinkEdge`가 같이 넣는다) |
| `Impl::ProcessClosing` | `RetirePublished` 실패 → Fatal | `Loader()`로 요청·넘겨받기 API를 직접 부르지 않는다는 계약(11.7-1, 헤더) 아래에서: 단위 멤버는 모두 PostLoad까지 마친 공개 패키지(넘겨받은 것)이고 같은 Pump의 판정이 로더 공개 보유 0을 보았으며 사이에 로더 활동이 없다. 콜백 밖·종료 전에서만 부른다 |
| `Impl::LoaderHolds` | `PublishedHolds` 실패 | 멤버 id는 매니페스트 항목이다(NotFound 없음), 소유 스레드에서 부른다 |
| `Impl::IngestPublished` | 대표 에셋 찾기 실패 | 로더는 대표 에셋 타입을 매니페스트와 대조한 패키지만 공개하고(A1c, 10.1-8), `Registry()`로 넘겨받는 중인 패키지 객체의 수명을 바꾸지 않는다는 계약(11.7-1)이 그 사이를 지킨다 |
| `Impl::ValidSlot`·`AssetLease::IsValid`·`OnReady`·`Cancel`·`BeginWith` | 칸 범위 밖·세대·종류·`objectAlive` 불일치 | handle 객체가 살아 있는 동안 그 칸은 같은 세대·종류로 살아 있다(칸은 객체가 소멸해 큐를 거친 뒤에만 다시 쓰고, 생성자는 비공개라 만들 수 없다). 다른 AssetManager·기본 handle은 `manager_` 비교에서 먼저 거른다(시험함) — 방어 검사 |
| `Impl::TakeSlot`·`AssetScope::Close` | 세대 증가가 0으로 감김 | 한 칸을 2³²번 다시 써야 한다 |
| `Impl::CompleteTicket` | 로더 `Status` 실패 | 티켓이 쥔 로더 handle은 끝낼 때까지 유효하다(놓을 때만 Release) |
| `Impl::FinishTicketResult` | `AcquireType` 실패 | 대표 에셋은 넘겨받은 살아 있는 객체이고 그 타입은 등록돼 있다(실패하지 않는 쪽 `IsA` 불일치는 시험함) |
| `AssetManager::EstimatePrimaryAsset` | 의존 대상이 매니페스트에 없음 | 설정의 매니페스트는 읽은(검증한) 것이다 — Hard 대상과 bundle 구성원은 매니페스트에 있다(10.1-6). 손으로 만든 매니페스트를 넘기는 계약 위반에 대한 방어 |
| `AssetManager::With`(`AssetManager.h`) | 람다 타입마다 다른 인스턴스의 한쪽 분기 | 템플릿이 호출하는 람다마다 따로 인스턴스화되어 인스턴스별로 센다 — 성공·StaleHandle·InvalidArgument·WrongThread 쪽은 모두 시험이 지난다 |

닿게 한 방법과 그 과정에서 바꾼 것:
- 결정적 장면: Discard Busy는 그 패키지 부분 트리에 공개 전 객체를 만든 `ConstructionContext`(패키지 바로 아래는 대표 자식 규칙 때문에 대표 에셋 아래)를 열어 둔다. Commit과 넘겨받기 사이 간선은 PostLoad handler와 PostLoad 예산 1로 단위를 Pump 여러 번에 걸쳐 끝내 만든다(handler가 없으면 로더가 공개 자리에서 단위를 끝낸다). handle 상한은 실제로 2²⁰ − 4개 lease를 쥐어 닿는다(Debug 약 1 s). 비ASCII bundle 이름은 키 정규화가 할당하므로 할당 실패 sweep이 그 지점에 닿는다(ASCII 이름은 내장 용량 안이라 할당하지 않는다 — 이름 상한 64 byte와 키 내장 용량이 같다).
- 리뷰가 찾은 것(1회차 판정 "미충족" — 반영): (D1) 끝난 티켓을 callback 전달 전에 놓으면 놓은 보유로 lease를 만들려다 Fatal — Cancelled로 끝내게 고침. (D2) callback 안의 요청이 전달 목록을 다시 잡으면 range-for가 해제된 버퍼를 읽음 — 색인 순회로 고침. (D3) Shutdown 전달 안에서 낸 요청이 받아들여져 callback이 영영 불리지 않음 — 전달 전에 종료 중 표시. (D4) 간선 묶음을 키마다 판정해 Fatal에 닿음 — 쌍별 합산. (D5) `Registry()`·`Loader()` 계약이 문서에 없어 14.1.8 근거 둘의 전제가 빠짐 — 11.7-1·헤더에 명시. 2회차 판정(미충족 — 반영): 종료 뒤·종료 전달 안의 `OnReady`가 등록을 받아 callback이 영영 불리지 않음(D3과 같은 종류) — 두 `OnReady`가 `ShuttingDown`, 보류 명령의 내부 티켓이 보유를 티켓 칸에 걸어 보유 내역의 "보류"가 늘 0 — 내부 티켓 표시(`deferredHold`)로 보류 칸에 걸고 조합 꼬리에서 단언. 그 밖에 `LoadBlocking`의 `WaitFor` 실패는 상태가 있는 프레임 판정으로 닿음(근거를 지우고 시험), 조합에 해제 방식·꼬리 축을 더함(72 → 864), 용량 추정을 포화 덧셈으로(Base `TryAdd`). 리뷰 참고 중 `JudgeClosing`의 로더 보유 조회는 이미 다른 보유가 모두 0인 단위만 묻는다(`||` 단락).
- 시험이 찾은 결함: 할당 실패 시험이 `DisableFailure`(시도 수를 0으로 되돌림) 뒤에 실패 여부를 읽어 sweep이 일찍 끝났다(실패 여부를 먼저 읽게 고침). Pump·Shutdown의 전달 목록 할당이 실패하면 Shutdown이 `ShuttingDown` 전달을 건너뛸 수 있었다(용량을 표가 자랄 때 잡게 고침 — 11.7절 6).
- 없앤 죽은 분기(입력으로 닿을 수 없음을 확인): 보류 명령 실행 직전의 scope 재확인(회수가 먼저 실패 사유를 적는다 — 11.7절 9), `ScopeOpen`의 "scope 없음" 쪽(부르는 곳이 모두 scope가 있을 때만 부른다), `CompleteTicket`의 로더 요청 없음(끝나지 않은 티켓은 늘 쥔다), 묶음을 두 번 앞당겨 끝내는 경우(묶음은 한 번만 — 놓기는 칸을 비우고 회수는 칸을 회수 표시한다), 묶음 전달의 "끝나지 않았지만 멈춤"(앞당겨 끝내면 구성 티켓이 모두 끝난다), 순환 검사의 같은 단위(쌓을 때 다르고 병합은 단위를 지우기만 한다), scope 회수의 `objectAlive` 거짓(큐 처리 때 칸과 함께 지운다), 로더 해석의 "단위 없음이면서 공개 안 됨"(FindComponents가 공개 안 된 Hard 대상을 모두 새 단위로 넣는다)과 의존 단위가 끝날 때 기대는 단위의 단계 검사(보유 표시는 공개 때 내리므로 표시가 있으면 공개 전이다 — `IsBeforePublish` 제거).


A1d 상태(2026-11-21): **구현**. 완료 조건 넷을 모두 갖췄다. 빠진 시험 리뷰를 세 번 거쳤다(미충족 두 번 — D1–D5·근거 2건·조합 축, 종료 뒤 `OnReady`·보류 칸 — 를 반영, 세 번째 판정 충족, 2ac34fa4 기준).
- 요구 대조표(14.1.7): 시험 없는 항목 0, 정의된 시험 57개를 모두 인용
- 조합: 보유 4종 × 단위 모양 3 × 해제 순서 24 × 해제 방식 3 × 꼬리 4 = 864개 전부(서로 다른 순서 확인), 매니페스트 bundle 구성원 축을 더한 564개(14.1.3)
- coverage(14.1.8): 닿지 않은 곳은 모두 근거(Fatal probe·불변식·실패 조건을 만들지 않는 호출·템플릿 인스턴스)
- 빠진 시험 관점 리뷰어 판정: 충족

#### 14.1.9 요구 대조표(A1e — EngineAssets)

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/EngineAssets/Tests/EngineAssetsTests.cpp`·`EngineAssetsLifecycleTests.cpp`·`EngineAssetsSettingsTests.cpp`·`EngineAssetsManifestFileTests.cpp`, 공용 도구 `EngineAssetsTestSupport.h` — 정의된 시험 28개를 모두 표가 인용한다. 리뷰 뒤 더한 Engine 시험 `Engine_ReentrantShutdownIsBusy`와 AssetRuntime 시험 보강도 함께 적는다). 시험 장면은 AssetRuntime 시험 도구(`LoaderScene`)로 쿠킹해 시험 임시 디렉터리(`rocks-engine-assets-<tag>-<pid>`)에 패키지와 `CookedManifest.rman`을 쓰고, 설정 문맥(`ConfigFixture`)에 `asset.*`를 넣은 엔진(`test::ProcessModules()`, 시험 에셋 타입은 `registerModuleTypes`)을 띄운다. 로드 순서는 프레임 밖 동기 로드로 상주시킨 뒤 프레임 안 요청의 다음 Pump callback으로 정하고, GC 조각 수는 객체 수 예산으로 정한다. IO 진행 중 상태는 로더 Pump 순서(완료를 먼저 거두고 허가가 IO를 낸다 — 허가한 Pump 안에서는 끝나지 않는다)로 정하고 `Describe`로 확인한다(디스크 IO 완료 시점에 기대는 단언 없음). 고정 delta(1/60 s)로 엔진 시간이 결정적이다. 로그 규칙은 Manual 모드 LogSystem에 붙인 `EngineAssets` 범주 capture sink로 확인한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 모듈·등록(11.8-1·2) | 모듈 표의 EngineAssets 행에 `RegisterSettings`(HostSettings 경로)가 있고 영역·변수를 등록, Subsystem 생성 정보로 엔진이 만들고 `GetSubsystem`으로 접근 | `EngineAssets_ModuleEntryRegistersSettings`, `EngineAssets_LoadsThroughEngineFrames` |
| 설정(11.8-3) | 등록 함수가 영역 `asset`을 더하고 두 번째 호출도 성공(멱등), 다른 스레드 WrongThread, 부팅 변수는 시작 뒤 쓰기 거부 | `EngineAssets_RegisterSettings`, `EngineAssets_PumpFailureAndBootValues` |
| | 부팅 변수 전달: `asset.loader.workers`(기본 2·설정 3 → `Stats().loaderWorkers`), `asset.loader.max_packages_in_flight`·`asset.loader.max_temp_bytes`(아래 허가 조합 — 로더 `Describe`의 진행 중 패키지 수·허가 대기 단위) | `EngineAssets_LoaderWorkersVariable`, `EngineAssets_AdmissionVariables` |
| | 실행 중 변수: 예산 변환(ms → ns 소수 버림, 개수 0 → 무제한 표지, 그 밖은 그대로 — Pump 시간·단위·PostLoad·Discard, GC 시간·객체 수), Pump Discard 예산 1(같은 프레임에 놓은 두 단위를 Pump마다 하나씩), 실행 중 바꾼 값이 다음 프레임 sync부터 적용(무제한 → 1 → 무제한), GC 객체 수·시간, 재시도 시간 | `EngineAssets_BudgetMapping`, `EngineAssets_PumpBudgetVariables`, `EngineAssets_RuntimeVariableChange`, `EngineAssets_GarbageCollectionCombinations`, `EngineAssets_HeldDiscardedObjectsRetry` |
| | 쿠킹 루트 결정(설정 값 그대로·비면 실행 파일 디렉터리 + /Cooked·둘 다 없으면 InvalidArgument·할당 실패), 실행 프로파일 기본값(플랫폼 이름), 실행 파일 디렉터리(구분자 '/'·'\\'·섞임·루트·없음), 설정이 비면 진입점이 캡처한 실행 파일 옆 Cooked를 찾음(없음 NotFound) | `EngineAssets_ResolveCookedRoot`, `EngineAssets_ResolveRuntimeProfile`, `EngineAssets_ExecutableDirectoryOf`, `EngineAssets_ManifestLocationFailures` |
| 초기화(11.8-4) | 설정 문맥 없음 InvalidArgument(그 Error 로그 하나·단계 로그 없음), 매니페스트 상태 6 × 프로파일 2 조합(아래 — 실패 단계·오류·진단 이름을 담은 Error 로그 정확히 하나, 성공이면 시작 Info 하나), 매니페스트 자리가 디렉터리 NotFound·빈 파일 InvalidArgument, 결정된 쿠킹 루트·프로파일, 엔진 할당의 모든 지점 실패 → OutOfMemory로 Failed(정리)·성공이면 프레임·종료 정상 | `EngineAssets_InitializationFailures`, `EngineAssets_InitializationCombinations`, `EngineAssets_ManifestLocationFailures`, `EngineAssets_LoadsThroughEngineFrames`, `EngineAssets_InitializationAllocationFailures` |
| | 매니페스트 파일 읽기(고장 난 FileSystem): 존재 확인·열기·크기·읽기·닫기 실패는 그 파일 오류, 없음 NotFound, 상한 초과 CapacityExceeded(읽지 않음), 빈 파일, 정상 내용, 할당 실패 | `EngineAssets_ManifestFileReadFailures` |
| | 한 프로세스의 두 엔진: 에셋 Registry 문맥 id의 상위 값은 같고 하위 값(순번)이 다름 | `EngineAssets_EnginesHaveDistinctContexts` |
| 프레임(11.8-5) | 프레임 밖 LoadBlocking 성공·IsInFrame 거짓, client Tick(3단계) IsInFrame 참·LoadBlocking WrongContext, 상주 에셋 요청 callback은 다음 프레임 World Tick 전 Pump에서 한 번, World 지연 파괴(5단계) 안 IsInFrame 참·LoadBlocking WrongContext | `EngineAssets_LoadsThroughEngineFrames`, `EngineAssets_WorldReleasedTicketIsCancelled` |
| | GC: RootRef 반환 주도(Discard 전 프레임은 수집하지 않음 — `releasedPackages`), 표식 scheduler × 조각 크기 × cycle 도중 Discard × 열린 트랜잭션 프레임 조합(아래), 쥔 Discard 객체는 lingering이 되고 재시도 시간(엔진 프레임 시간)마다 다시 돌며 놓은 뒤 회수, lingering 중 새 Discard는 재시도 시간과 무관하게 그 프레임 끝에 cycle을 시작하고(표식 → 다음 조각 회수 — GC 시간 예산을 넉넉히 줘 결정적) 놓은 것만 회수하며 쥔 것은 계속 lingering | `EngineAssets_GarbageCollectionCombinations`, `EngineAssets_HeldDiscardedObjectsRetry`, `EngineAssets_LingeringThenNewDiscard` |
| | Pump 실패(AssetManager를 직접 Shutdown한 뒤): 두 프레임 실패에 Error 로그 하나(`ShuttingDown`)·횟수 2, 프레임은 계속, 엔진 종료 성공. 프레임 할당 실패(Discard 프레임과 다음 프레임의 모든 지점): 그 프레임만 실패·GC 조각 실패마다 Error 로그(수 = `gcFailures`)·이어지는 프레임이 cycle을 끝내고 끝 상태가 실패 없는 기준 실행과 같음(남은 회수·Discard 요청·재시도 대기 없음, 살아 있는 객체 수 같음 — 이 단언이 `ReleaseRetired` 할당 실패 뒤 GC 요청이 서지 않는 빈 곳을 찾아 `releasedPackages`를 더했다) | `EngineAssets_PumpFailureAndBootValues`, `EngineAssets_FrameAllocationFailures` |
| 종료(11.8-6) | 종료 조합(아래 — Pump 전·IO 진행 중 × callback 안 재종료), 마지막 프레임의 요청은 CompleteInFlightWork에서 ShuttingDown 한 번, World Subsystem이 쥔 티켓은 World 파괴 뒤 다음 Pump에서 Cancelled(전제: 허가한 Pump 안에서는 끝나지 않는다), GC cycle 도중 종료 성공, 초기화 실패 정리(client 시작 함수 실패) 중 AssetManager 파괴가 남은 요청 callback에 ShuttingDown 한 번·그 안의 엔진 재종료 Busy·Failed로 정리, client가 OnEngineShutdown에서 놓으면 정상 종료·놓지 않은 lease는 Subsystem 정리에서 자식 프로세스 Fatal(문구 대조) | `EngineAssets_ShutdownCombinations`, `EngineAssets_ShutdownDeliversShuttingDown`, `EngineAssets_WorldReleasedTicketIsCancelled`, `EngineAssets_ShutdownDuringGcCycle`, `EngineAssets_ShutdownDuringFailedInitialization`, `EngineAssets_LeaseHeldPastShutdownIsFatal` |
| Engine 변경(11.8-7) | `IsInFrame`(RunFrame 전체 — 3·5단계 참, 밖 거짓), `Settings`(없으면 null → InvalidArgument), `Shutdown` 재진입 Busy(초기화 중 Subsystem Initialize·client OnEngineStart 2곳과 종료 중 Subsystem BeginShutdown·CompleteInFlightWork·Deinitialize, client OnEngineShutdown, hook 다섯 단계 9곳 — 11곳 모두 Busy, 초기화는 Running, 바깥 종료는 Stopped·성공, 끝난 뒤 다시 부르면 무동작 성공. EngineAssets callback 안 재종료도 종료 중·초기화 실패 정리 중 모두 Busy) | `EngineAssets_LoadsThroughEngineFrames`, `EngineAssets_WorldReleasedTicketIsCancelled`, `EngineAssets_InitializationFailures`, `Engine_ReentrantShutdownIsBusy`, `EngineAssets_ShutdownCombinations`, `EngineAssets_ShutdownDuringFailedInitialization` |
| AssetManager 변경(11.7-3) | Pump 결과 `releasedPackages`: 단위 일부만 Discard한 Pump는 0, 단위를 끝낸 Pump가 그 패키지 수, 모든 할당 지점 실패 시나리오에서 Discard 합 = RootRef 반환 합 | `AssetManager_DiscardBudgetStopsMidUnit`, `AssetManager_AllocationFailures` |
| 조합(14 완료 조건 2) | 초기화: 매니페스트 상태 6(정상·없음·자리가 디렉터리·빈 파일·손상·형식 버전 다름) × 프로파일 2(일치·불일치) = 12 — 정상·일치만 성공, 매니페스트 실패는 프로파일과 무관(검사 순서), 정상·불일치 Unsupported/IncompatibleCook(단계 `asset manager`), 버전 다름 Unsupported/UnsupportedVersion(단계 `manifest parse`) | `EngineAssets_InitializationCombinations` |
| | GC: 표식 scheduler 2(엔진 gcWorkers 0·2 — 엔진 GC scheduler 유무·worker 수 확인) × 조각 크기 2(무제한·객체 1) × cycle 도중 Discard 2 × 열린 트랜잭션 프레임 2 = 16 — 병렬 축은 엔진 GC scheduler가 있고 EngineAssets가 그것을 조각에 넘김(`gcParallelSteps`), cycle 수(1 + 도중 Discard), Busy 횟수, scheduler마다 예산 1이 무제한보다 많은 프레임, 회수 뒤 객체 감소·실패 없음·lingering 없음 | `EngineAssets_GarbageCollectionCombinations` |
| | 허가: 동시 진행 상한 2(기본·1) × 임시 byte 상한 2(기본·1) = 4 — 둘 다 기본일 때만 두 패키지 허가, 아니면 하나(진행 중인 것이 없으면 상한을 넘어도 하나는 허가)와 허가 대기 단위 하나 | `EngineAssets_AdmissionVariables` |
| | 종료: 종료 시점 2(Pump 전·IO 진행 중) × callback 안 재종료 2 = 4 — callback ShuttingDown 한 번, 재종료 Busy, 바깥 종료 Stopped | `EngineAssets_ShutdownCombinations` |
| | 로더 관찰자(Rocks Lens)·클래스별 PostLoad dispatch·패키징 | 범위 밖(9 — 후속·A5). 서명 봉투는 A1f(14.1.11) |

#### 14.1.10 Coverage(A1e)

측정(2026-11-21, 리뷰 반영 뒤, `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter EngineAssets,Engine_ -Sources Engine/Source/Runtime/EngineAssets/Private,Engine/Source/Runtime/EngineAssets/Public`, EngineAssets 27개·Engine 29개 시험 모두 통과. 전체 EngineTests Debug 3527개, Release EngineAssets 27·AssetManager 51·Engine 29개 통과):

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `Private/EngineAssetsSubsystem.cpp` | 95.2%(279/293) | 95.1%(97/102) | 100%(15/15) |
| `Private/EngineAssetsSettings.cpp` | 100%(67/67) | 100%(22/22) | 100%(8/8) |
| `Private/EngineAssetsModule.cpp` | 100%(3/3) | 분기 없음 | 100%(1/1) |
| `Public/EngineAssets/EngineAssetsSubsystem.h` | 100%(12/12) | 분기 없음 | 100%(4/4) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `Initialize` | 진입점이 명령줄을 캡처하지 않음(`ProcessCommandLine::IsCaptured` 거짓) → InvalidArgument | 시험 실행 파일도 진입점(FoundationTests `Main.cpp`)이 시작 때 캡처하고 프로세스 수명 동안 되돌릴 수 없다. 진입점 없이 엔진을 띄우는 호스트(캡처 전)만 닿고, 그런 호스트는 `asset.cooked_root`를 주면 이 경로를 쓰지 않는다 |
| `Initialize` | 비동기 IO context 시작 실패 | worker 수는 CVar 범위(1–64)로 막혀 있고 남은 실패는 스레드·OS 객체 생성 실패다(AsyncIoContext 결과 주입은 크기·제출·완료 지점만 — PlatformLayer) |
| `CompleteInFlightWork` | `AssetManager::Shutdown` 실패 기록 | 실패 조건은 소유 스레드 밖·AssetManager callback 안이다. 엔진 종료는 게임 스레드에서 이 단계를 부르고, AssetManager callback이 도는 곳(프레임 Pump·종료의 이 단계·초기화 실패 정리의 AssetManager 파괴) 어디서 엔진 `Shutdown`을 다시 불러도 엔진이 Busy로 거절해(프레임 중·종료 중·초기화 중) 이 단계에 닿지 않는다. 리뷰 두 번이 그 경로를 찾아(종료 중 callback, 초기화 실패 정리 중 callback) `Engine::Shutdown` 재진입 Busy를 넓혔다 — `EngineAssets_ShutdownCombinations`·`EngineAssets_ShutdownDuringFailedInitialization`이 Busy를 확인한다 |
| `Teardown` | loader scheduler·IO context 종료 실패 기록 | scheduler는 게임 스레드에서 끝내고(worker 스레드 호출이 아니다), IO context는 로더 종료와 package IO 파괴 뒤라 열린 파일이 없다(Busy 조건 없음) |

닿게 한 방법과 그 과정에서 바꾼 것:
- 결정적 장면: 디스크 IO 완료 프레임에 기대지 않게 프레임 밖 동기 로드로 상주시킨 뒤 프레임 안에서 요청한다. IO 진행 중 상태는 로더 Pump 순서(완료를 먼저 거두고 허가가 IO를 낸다)로 만든다. GC 조각 수는 객체 수 예산, 재시도 시간은 고정 delta 엔진 시간으로 정한다. 매니페스트 파일의 OS 오류는 `ReadManifestFile`이 `FileSystem&`를 받게 해 고장 난 FileSystem으로 닿는다(처음 구현은 물리 파일 시스템을 직접 썼다). 로그 규칙은 Manual LogSystem capture로 수와 문구를 단언한다.
- 구현 중 발견해 설계를 고친 것: CVar 첫 마디 `asset`은 기본 영역 표에 없어 등록이 거부됐다(등록 함수가 영역을 더한다). 설정 문맥이 없으면 CVar 읽기가 Fatal이라 Initialize가 먼저 거부한다. 프레임 안 Pump 실패는 되돌아오지 않아 회복 요약을 뺐다. Discard 객체를 게임 코드가 쥔 채 cycle이 끝나면 다음 Discard까지 회수되지 않는 빈 곳이 있어 UE 주기 수집처럼 재시도 시간(`asset.gc.retry_seconds`, 기본 60 s)을 더했다. `DrainRetired` 분기는 에셋 Registry에 회수 보유가 없어 닿을 수 없어 빼고 A5로 넘겼다.
- 리뷰(1회 — 미충족) 뒤 고친 것: 콜백 안 엔진 재종료가 Fatal로 끝나던 경로(`Engine::Shutdown` 재진입 Busy), 실행 파일 경로를 진입점 캡처(`ProcessCommandLine`)에서 얻기(4096 byte 버퍼 OS 조회 제거), 모르는 플랫폼의 기본 프로파일을 빌드 오류로(`static_assert`), `asset.io.workers`가 Windows IOCP에서 무시됨을 설명, 로더 worker 수 관측(`loaderWorkers`), 그리고 끝 상태 단언을 강화하다 찾은 `ReleaseRetired` 할당 실패 뒤 GC 요청 누락(`AssetManagerPumpResult::releasedPackages`로 GC를 건다 — 11.7절 3).

A1e 상태(2026-11-21): **완료**(리뷰 3회 — 미충족 2회 반영 뒤 충족). 완료 조건:
- 요구 대조표(14.1.9): 시험 없는 항목 0, 정의된 시험 28개를 모두 인용
- 조합: 초기화 12개·GC 16개·허가 4개·종료 4개 전부
- coverage(14.1.10): 닿지 않은 곳은 모두 근거
- 빠진 시험 관점 리뷰어 판정: 1회 미충족 → 반영, 2회 미충족(초기화 중 재종료 1건) → 반영, 3회 **충족**

#### 14.1.11 요구 대조표(A1f — 콘텐츠 서명)

설계는 [ContentSigning](ContentSigning.ko.md) 3·4·7절이다. 시험은 셋이다: AssetRuntime `Tests/ContentSignatureTests.cpp`(EngineTests, 11개 — 봉투 형식·검증·키 줄), EngineAssets `Tests/EngineAssetsSigningTests.cpp`(3개 — 엔진 정책)와 시험 장면(`DiskScene`이 시험 키로 서명한 봉투를 쓰고 신뢰 정보를 등록 — EngineAssets 시험 31개 전부가 서명 봉투 경로로 시작하므로 Release 구성에서도 돈다), `Programs/Rocks.Commandlet.Tests`(C#, content-sign 53항목). golden은 독립 구현이 만든 byte다: 처음에는 RFC 8032 5.1 순수 Python 구현(RFC 7.3 벡터로 확인)으로 만들었고, 재현할 수 있게 같은 알고리즘의 C# 구현 `Programs/Rocks.Commandlet.Tests/Ed25519phReference.cs`(BigInteger, 엔진·libsodium 코드 없음 — 리뷰 권장)를 저장소에 두었다. C# 시험이 참조 구현을 RFC 8032 7.3 Ed25519ph 벡터("abc")로 먼저 확인하고, 그것으로 golden 봉투를 다시 만들어 hex를 대조하며, `content-sign sign`이 쓴 봉투의 서명과 헤더 칸도 엔진과 무관하게 확인한다. 신뢰 키로 서명한 깨진 매니페스트 봉투도 참조 구현으로 만들어 `content-sign verify`의 매니페스트 판정을 확인한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 형식(3, 7-1) | 봉투 배치(magic·버전 1·알고리즘 2·예약 0·keyId BE·contentVersion LE·scope·길이 LE·서명·매니페스트), 서명 메시지(label ‖ 헤더 ‖ 매니페스트), 같은 입력이면 같은 byte, 독립 구현과 byte 일치, 검증이 같은 버퍼 안의 매니페스트 span(복사 없음)과 헤더 값(info) | `ContentSignature_GoldenEnvelope` |
| 검증 순서·진단(4, 7-2) | 모든 byte의 모든 bit 변조(1336회)가 칸별 단계로 거부(magic·예약·길이 CorruptPackage, 버전·알고리즘 UnsupportedVersion, keyId·낮아진 버전·scope·서명·매니페스트 ContentNotTrusted / AuthenticationFailed, 버전이 커지는 변조는 서명 단계), 되돌리면 통과 | `ContentSignature_EveryBitFlip` |
| | 자르기(모든 길이 — 136 미만 File, 이상 ManifestLength), 1 byte 늘리기, 길이 칸까지 맞춘 자르기는 서명이 거부, 봉투 상한 초과는 내용을 읽기 전에 File | `ContentSignature_TruncateAndExtend` |
| | 이웃한 두 단계를 함께 어기면 앞 단계가 이긴다(8쌍) | `ContentSignature_OverlappingViolations` |
| | 칸 이름(고유, 칸이 아닌 값 None) | `ContentSignature_FieldNames` |
| 신뢰 정보(CS3·CS4, 7-1) | 최소 버전 3 × 키 위치 3 × scope 2 = 18 조합(아래), 키 없음·null id·같은 id 두 번 InvalidArgument(진단 None·봉투 보기 전), id는 맞고 공개키가 다르면 Signature | `ContentSignature_TrustCombinations`, `ContentSignature_InvalidTrust` |
| scope(7-1) | ASCII 식별자 규칙(빈 값·NUL·공백·비ASCII·'/'·범위 경계 문자 거부), 구분자 0으로 ("ab","c")≠("a","bc"), 값 = SHA-256(productId ‖ 0 ‖ channel) | `ContentSignature_ScopeNames` |
| 쓰기(7-1) | null keyId InvalidArgument, 상한 초과 CapacityExceeded(읽기 전), 빈 키 StaleHandle, 실패하면 out 그대로, 빈 매니페스트 봉투, 서명·검증 상태의 모든 할당 실패(그 오류, out 그대로, 진단 None) | `ContentSignature_WriteErrors`, `ContentSignature_AllocationFailures` |
| 키 줄(7-5) | 형식(keyId hex 32 ‖ 공백 ‖ 공개키 hex 64, 소문자), 왕복, 끝 `\n`·`\r\n` 하나, 대문자 hex, 길이·구분자·hex 아님·줄바꿈만·줄바꿈 둘·null keyId 거부 | `ContentSignature_TrustedKeyText` |
| 엔진 정책(7-3·4) | 봉투 3(없음·서명·변조) × 신뢰 등록 2 × rman 2 = 12 조합(아래) — 결과·단계 이름·진단 이름의 Error 한 줄, 서명 안 됨 Warning 수, `contentSigned`·`contentVersion`, 봉투가 있으면 rman(깨진 byte)을 읽지 않음, Release는 봉투만 | `EngineAssets_SigningPolicyCombinations` |
| | 봉투 자리가 디렉터리면 봉투 없음과 같다(Release NotFound, 그 밖 Warning 한 번 뒤 rman), 봉투 읽기·검증 상태의 할당 실패가 단계 이름(`content signature read`·`content signature (None)`)으로 기록됨 | `EngineAssets_EnvelopeDirectoryIsAbsent`, `EngineAssets_InitializationAllocationFailures` |
| | 시험 키 서명이라도 낮은 contentVersion(ContentVersion)·다른 scope(Scope) 거부, 최소와 같은 버전 통과, 등록한 신뢰 정보가 잘못됐으면 InvalidArgument(칸 None), 봉투 안 매니페스트가 깨지면 봉투 통과 뒤 manifest parse(CorruptPackage) | `EngineAssets_SignedContentRejections` |
| | 신뢰 정보 등록(`SetContentTrust`)·해제, 서명 봉투로 시작하는 기존 엔진 경로 전부(초기화 조합·할당 실패·프레임·종료) | `EngineAssets_*`(30개 — `DiskScene`) |
| commandlet(5, 7-5) | keygen(공개 줄 98 byte 형식, 덮어쓰기 거부, 공개 자리만 있으면 만든 키 파일을 지움, 비밀번호 선택지 없음 사용법 2·파일 없음 1·빈 비밀번호 1), sign(성공·다시 서명은 원자적 교체·임시 파일 없음, 출력 디렉터리 없음·출력 자리가 디렉터리면 실패하고 임시 파일을 지움, 틀린 비밀번호, 다른 종류 키 파일, 매니페스트 아님·없음, 키·비밀번호 파일 없음, 위치 인자 부족·필수 선택지 없음·빈 값·버전·product 형식·같은 선택지 두 번·다른 하위 명령 선택지·모르는 이름 사용법 2), verify(성공, 다른 channel Scope, 높은 최소 버전 ContentVersion, 변조 Signature, 봉투 아님 File, 신뢰 키로 서명된 깨진 매니페스트(참조 구현이 만든 봉투), 키 줄 아님, 봉투·공개 파일 없음, 위치 인자·trust·product 없음·최소 버전 형식 사용법 2), 하위 명령 없음·모름·도움말 뒤 인자 사용법 2, 도움말, 참조 구현의 RFC 8032 벡터 확인·golden 재생성·sign 출력 서명 확인 | `Rocks.Commandlet.Tests`(content-sign 53항목) |
| 조합(14 완료 조건 2) | 신뢰: 최소 버전 3(봉투보다 낮음·같음·높음) × 키 위치 3(하나·여럿 가운데 마지막·없음) × scope 2 = 18 — 겹치면 Key → ContentVersion → Scope | `ContentSignature_TrustCombinations` |
| | 엔진 정책: 봉투 3 × 신뢰 등록 2 × rman 2 = 12, 기대값은 구성(IsReleaseBuild)으로 고르고 Debug·Release 실행이 양쪽을 덮는다 | `EngineAssets_SigningPolicyCombinations` |
| | 패처 (keyId → 받은 최고 버전) 기록, 빌드 규칙의 신뢰 정보 생성, 부분 읽기 조각 해시 | 범위 밖(ContentSigning 7-6 — 후속) |

#### 14.1.12 Coverage(A1f)

측정(2026-11-21, `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter ContentSignature,EngineAssets,Engine_ -Sources Engine/Source/Runtime/AssetRuntime/Private,Engine/Source/Runtime/EngineAssets/Private,Engine/Source/Runtime/EngineAssets/Public`):

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetRuntime/Private/ContentSignature.cpp` | 98.1%(257/262) | 97.9%(137/140) | 100%(14/14) |
| `EngineAssets/Private/EngineAssetsSubsystem.cpp` | 95.4%(329/345) | 94.7%(108/114) | 100%(17/17) |
| `Programs/Commandlet/Private/Common/ContentSignCommandlet.cpp` | 93.0%(319/343) | 84.8%(139/164) | 100%(14/14) |
| `Programs/Commandlet/Private/Common/CommandletFiles.cpp` | 75.0%(51/68) | 59.1%(13/22) | 100%(6/6) |

commandlet은 리뷰 지적으로 측정했다(처음 측정은 EngineTests만이었다): `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target Commandlet -Executable RocksCommandlet -Runner 'dotnet,run,--project,Programs/Rocks.Commandlet.Tests,--,--executable,{executable}' -Sources Programs/Commandlet/Private/Common` — 스크립트에 외부 구동기(`-Runner`)와 실행 파일 이름(`-Executable`) 인자를 더했다. 함께 잰 기존 파일(`LogCommandlet.cpp` 82.9%·`CommandletHost.cpp` 76.6%)은 A1f 범위 밖이다(파일 도우미를 `CommandletFiles`로 옮긴 것 말고 바꾸지 않았다).

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `WriteContentEnvelope` | 서명 상태에 조각 넣기 실패 | 방금 만든 상태라 실패 조건(끝난 상태·옮겨 간 객체 — Crypto 9.1절)이 없다. 오류를 그대로 잇는다 |
| `VerifyContentEnvelope` | 검증 상태에 조각 넣기 실패, 서명 확인의 AuthenticationFailed 밖 오류 | 같은 이유(방금 만든 상태 — 끝남·옮겨 감·이전 실패가 없다). 할당 실패는 상태 만들기에서 나고 시험이 닿는다 |
| `EngineAssetsSubsystem::Initialize` | 봉투가 없을 때 rman 경로 다시 만들기 실패 | 두 파일 이름이 같은 길이(`CookedManifest.rsig`·`.rman`)라 앞에서 만든 문자열 용량으로 다시 쓴다(할당 없음) |
| `EngineAssetsSubsystem` | A1e 근거 넷(14.1.10) | 그대로다 |
| `ContentSignCommandlet` keygen | OS 난수·키 저장소 실패로 키·KeyId 만들기 실패, Argon2id 작업 메모리(256 MiB) 부족으로 감싸기 실패 | OS·메모리 조건이고 실행 파일에는 주입 지점이 없다(Crypto 시험이 같은 오류 경로를 덮는다) |
| `ContentSignCommandlet` keygen | 두 파일을 만든 뒤 쓰기·닫기 실패와 두 파일 정리 | 새로 만든 파일의 쓰기·닫기 실패(디스크 가득 참·장치 오류)다. 정리 순서는 log keygen과 같고 공개 자리만 있는 경우의 정리는 시험이 닿는다 |
| `ContentSignCommandlet` 임시 파일 | OS 난수 실패로 이름 만들기 실패, 임시 파일 쓰기·flush·닫기 실패와 그 정리 | OS 조건(디스크 가득 참·장치 오류). 만들기 실패·교체 실패와 그 정리는 시험이 닿는다 |
| `ContentSignCommandlet` sign | 봉투 쓰기 실패 | 입력이 이미 검증한 매니페스트(상한 안)·연 키·non-null keyId라 남은 실패는 할당 실패다 |
| `ContentSignCommandlet`·`CommandletFiles` | 출력 채널 쓰기 실패 분기(`Write`), 파일 크기·읽기·쓰기·닫기 실패, 읽기 버퍼 할당 실패 | 표준 출력·오류 실패와 OS 파일 오류·할당 실패다. 출력 실패는 Windows 무효 핸들 시험이 사용법 경로에서 닿는다(같은 `Write` 패턴) |

A1f 상태(2026-11-21): **완료**(리뷰 3회 — 미충족 2회 반영 뒤 충족). 완료 조건:
- 요구 대조표(14.1.11): 시험 없는 항목 0
- 조합: 신뢰 18·엔진 정책 12·bit 변조 1336·자르기 전 길이
- coverage(14.1.12): 닿지 않은 곳은 모두 근거
- 빠진 시험 관점 리뷰어 판정: 1회 미충족(commandlet coverage 측정 밖) → 측정·시험·근거 반영, 2회 미충족(verify 깨진 매니페스트 근거가 사실 아님 — 참조 구현으로 만들 수 있음) → 시험으로 닿게 함, 3회 **충족**

#### 14.1.13 요구 대조표(A2a — 경로·배치·잠금·주입점)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/`(`LogicalPathTests.cpp`·`ProjectLayoutTests.cpp`·`AssetDatabaseLockTests.cpp`, 공용 `AssetDatabaseTestSupport.h` — 정의된 시험 20개를 모두 인용한다). 다른 프로세스의 잠금은 자식 프로세스(`--process-probe asset-database-lock`)와 표준 입출력 pipe gate로 순서를 정한다(시간에 기대지 않는다 — Android는 자식 프로세스가 없어 그 시험을 건너뛴다). OS 감시 확인만 Wait 반복 상한(200 ms × 50)으로 끝난다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 논리 경로(4.2, 7.1-10) | 양성: 마운트 철자 정규화(대소문자 무시), ASCII·비ASCII 숫자(N*) 첫 문자, '_'·'-', 비ASCII(L*), NFD 입력의 NFC 정규화, 접근자(Text·Key·Name·Directory·Relative·Mount) | `LogicalPath_ValidPaths` |
| | 음성(판정 순서): 빈 값, 앞 '/' 없음, '\', 빈 구성 요소('/'·'//'·끝 '/'), 마운트뿐, 모르는 마운트(잘못된 UTF-8 포함), 구성 요소 규칙 위반 전부(`.`·`..`·'_'·결합 문자(M*) 첫 문자 InvalidFirst, 공백·'.' InvalidCharacter, CON·com1·LPT¹ 예약 이름, U+200B Default_Ignorable, 잘못된 UTF-8, NUL, 65 byte TooLong), 전체 256 초과. 위반 위치는 NFC로 정규화한 구성 요소 안의 byte offset(공백·'.'·NFD 입력으로 위치가 당겨지는 경우). 이웃 규칙의 겹친 위반 일곱 쌍(규칙 1·2·3·4·5 — 앞 '/' 대 '\', '\' 대 빈 구성 요소, 빈 구성 요소 대 마운트, 마운트뿐 대 마운트, 마운트 대 구성 요소, 구성 요소 둘, 구성 요소 대 전체 길이(268 byte에 마지막 구성 요소 위반)) | `LogicalPath_InvalidPaths` |
| | 경계: 구성 요소 64 byte·전체 256 byte 받음, 257 거부, 정규화 뒤 길이로 셈 — NFD가 NFC로 줄어 받음, 조합 제외 문자(U+0958)가 NFC로 늘어 구성 요소 TooLong·전체 TooLong(넷은 받고 다섯은 거부) | `LogicalPath_LengthBoundaries` |
| | 비교 키: 대소문자·정규화 형태만 다르면 같은 키, simple case folding('ß' ≠ "ss"), 다른 경로는 다른 키 | `LogicalPath_CaselessKeys` |
| | 복사, 옮겨 간 값의 접근자(Mount 포함)는 빈 view이고 그 복사도 빈 값(범위 밖을 읽지 않음 — 공개 생성자는 없다), 사유 이름(고유·범위 밖 "Unknown"), 모든 할당 지점 실패(비ASCII 긴 경로·TooLong 전에 자라는 경로·NFC 뒤에도 애매한 결합 문자 q + U+0301, 복사) — OutOfMemory·사유 None, 실패 없으면 같은 결과 | `LogicalPath_CloneAndNames`, `LogicalPath_AllocationFailures` |
| 배치(4.2, 7.1-10) | 디렉터리 이름(Content·Config·Intermediate·Saved), 절대 루트 정규화('\'·반복·끝 구분자), 루트뿐인 경로("C:/"·"/"), 상대 루트는 작업 디렉터리 기준 절대 경로, 빈 루트·현재 드라이브 루트·드라이브 상대 InvalidArgument, 해석할 수 없는 루트(NUL InvalidArgument·장치 이름 공간 Unsupported) — 이전 값 유지 | `ProjectLayout_Directories` |
| | Initialize 전 배치는 파일 경로를 만들지 않음(InvalidArgument) | `ProjectLayout_Uninitialized` |
| | 논리 경로 → 새 파일 경로 → 논리 경로 왕복, 확장자 ASCII 대소문자 무시(".RASSET"·".RaSsEt" 받음), 패키지가 아닌 이름(임시 파일·확장자뿐·확장자 없음·다른 확장자·빈 값 — 사유 None), 규칙 위반 이름의 사유 | `ProjectLayout_PathRoundTrip`, `ProjectLayout_NonPackageNames` |
| | 비 NFC 파일 이름: byte 비교 파일 시스템 NotNfcOnDisk, 이름 정규화 파일 시스템(macOS)은 NFC 경로로 받음, NFC 이름은 어느 쪽이든 받음, 사유 자리 없이도 같은 판정. 겹친 위반(확장자 → 파싱 → NotNfc 순서): 비 NFC + 규칙 위반 InvalidComponent, 비 NFC + 너무 김 TooLong, 디렉터리 구성 요소만 비 NFC | `ProjectLayout_NonNfcFileNames` |
| | OS 경로 상한(임시 접미사 41 byte 포함): 같으면 받음, 넘으면 CapacityExceeded(out 그대로). 상한 길이의 절대 경로 파일을 실제로 만들 수 있음(OS가 받는 범위 안 — 드라이브 경로로 확인, UNC 경로의 실제 경계는 환경이 필요해 미검증) | `ProjectLayout_NativePathLimit`, `ProjectLayout_NativePathLimitOnDisk` |
| | 모든 할당 지점 실패(600 byte 넘는 루트·상대 루트의 작업 디렉터리 버퍼·640 byte 넘는 상대 이름 — 인라인 저장소를 넘겨 자람, 긴 이름은 TooLong): 그 오류, 실패한 Initialize는 바로 앞 값 유지 | `ProjectLayout_AllocationFailures` |
| 쓰기 잠금(8, 7.1-4) | Intermediate를 만들고 잠금, 같은 객체 다시 잡기 AlreadyExists, 같은 프로세스 다른 객체 Busy(대기 0), 놓으면 다른 객체가 잡음, 두 번 놓기 무동작, 놓인 잠금은 대기가 있어도 바로 잡음, Initialize 전 배치 InvalidArgument | `AssetDatabaseLock_SameProcess` |
| | 다른 프로세스: 자식이 쥐면 부모 Busy, gate 뒤 자식이 놓으면 부모가 잡음, 부모가 쥐면 자식 Busy, 쥔 자식을 강제 종료하면 OS가 풀어 부모가 잡음(OS가 푸는 시점은 정해지지 않아 — LockFileEx — 대기는 실패 상한으로만 쓴다). Android는 자식 프로세스가 없어 건너뛰고 그 사실을 출력한다 | `AssetDatabaseLock_OtherProcess` |
| | Intermediate 자리가 파일이면 WrongType(→ InvalidArgument, 잡지 않음), 잠금 경로가 OS 상한을 넘으면 CapacityExceeded, 긴 경로의 모든 할당 지점 실패 | `AssetDatabaseLock_Failures`, `AssetDatabaseLock_AllocationFailures` |
| 주입점(7.1-9) | 시스템 시계(UtcTicks 표현), 시험 시계(고정·Advance), 단위 상수 | `AssetClock_Sources` |
| | OS 감시 공급(`WatcherChangeSource`)이 만든 파일의 Added를 Wait·Poll로 전달 | `WatcherChangeSource_ReportsAddedFile` |
| 조합(14 완료 조건 2) | 논리 경로 규칙은 판정 순서가 있는 단일 축이라 조합 대신 규칙별 양성·음성과 이웃한 규칙의 겹친 위반(모든 이웃 쌍)으로 덮는다. 파일 이름 대응은 확장자 → 파싱 → NotNfc 순서의 겹친 위반을 따로 본다. 잠금은 보유자(없음·같은 객체·같은 프로세스 다른 객체·다른 프로세스·비정상 종료한 프로세스) × 요청자 대기(0·있음) 축을 위 시험이 지난다 | 위 시험 |

#### 14.1.14 Coverage(A2a)

측정(2026-11-21, Windows, `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter LogicalPath,ProjectLayout,AssetDatabaseLock,AssetClock,WatcherChangeSource -Sources Engine/Source/Developer/AssetDatabase/Private,Engine/Source/Developer/AssetDatabase/Public`): 줄 99.0%(398/402)·분기 97.6%(160/164)·함수 100%(38/38). `if constexpr`로 갈리는 macOS·Linux 쪽(`FileSystemNormalizesNames` 참, `MaxNativePathBytes`의 POSIX 값)은 Windows 측정 밖이고 미검증이다(macOS 실행은 후속).

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `ParseLogicalPath` | 정규화가 거부한 구성 요소를 이름 규칙이 받아들이는 경우(사유 None) | Core 계약상 `TryNormalize`의 InvalidArgument는 잘못된 UTF-8뿐이고, `ValidateNameComponent`는 잘못된 UTF-8을 할당 없이 InvalidCharacter로 거부한다. 둘이 어긋나면 사유 없는 거부 대신 그 결과를 그대로 돌려주는 방어다 |
| `ProjectLayout::Initialize` | 작업 디렉터리 조회 실패 | Windows에서는 OS 조회 실패뿐이다(버퍼는 상한 + 1). POSIX에서는 작업 디렉터리가 지워졌거나(`getcwd` ENOENT) 상한보다 깊으면(ERANGE) 닿지만, 프로세스 전역 작업 디렉터리를 바꾸는 시험이라 다른 시험과 함께 돌릴 수 없어 하지 않는다(macOS 실행 때 따로 확인) |

닿게 한 방법: 할당 실패 시험의 입력을 임시 저장소의 인라인 크기(경로 320 byte·잠금 경로 512 byte)를 넘는 길이로 골라 저장소가 자라는 지점을 지나게 했고, 짧은 조각 여러 개를 한 호출(`AppendAll`)로 묶어 실패 지점을 실제로 자랄 수 있는 곳으로 모았다. 리뷰(1회)가 지적한 것: 경로 상한 경계(NUL·UNC 접두), 상대 루트, 빈 경로 접근자의 범위 밖 읽기(처음 coverage를 맞추려고 뺀 검사가 그것을 막고 있었다 — 생성자를 비공개로 하고 빈 값 처리를 되돌렸다), 확장자 대소문자, 규칙 4 대 5 순서 시험. 구분자 바꾸기 반복은 Core Path가 이미 '/'로 정규화하고 POSIX에서 '\'는 이름 문자라 뺐다.

A2a 상태(2026-11-21): **완료**(리뷰 3회 — 미충족 2회 반영 뒤 충족). 완료 조건: 대조표(14.1.13) 시험 없는 항목 0·정의된 시험 20개 모두 인용, 조합(위 표 마지막 행), coverage(14.1.14 — Windows), 리뷰어 판정 1회 미충족 → 반영, 2회 미충족(옮겨 간 값의 Mount, 강제 종료 뒤 잠금 해제 시점) → 반영, 3회 **충족**.

#### 14.1.15 요구 대조표(A2b — 부분 읽기·Tags·색인·명령)

시험은 EngineTests `Engine/Source/Runtime/AssetRuntime/Tests/PackageIndexTests.cpp`(9개)와 `Engine/Source/Developer/AssetDatabase/Tests/AssetIndexTests.cpp`·`AssetIndexCommandletFixtureTests.cpp`(7개)의 정의된 시험 16개를 모두 인용하고, 명령은 C# `Programs/Rocks.Commandlet.Tests`의 asset 항목 30개(Windows 전용 5개 포함)다. 메모리 파일 시스템(`MemoryFileSystem` — byte 그대로의 노드 표, 열기·읽기·열거 고장 주입, 링크·장치 항목, NTFS에서 만들 수 없는 대소문자만 다른 이름, 잘못된 UTF-8 이름)과 실제 디스크(`TempProject`)를 함께 쓴다. 읽기 핵심을 바꾼 뒤 기존 패키지 시험(AssetPackage 74·FixtureAsset·CookedManifest·AssetLoader·AssetManager)이 그대로 통과한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 부분 읽기(7.1-13) | 풍부한 에디터 패키지(import·Zstd bulk 셋·커스텀 버전·ImportRecord·Tags 값 스키마·Build 의존)에서 헤더·섹션 수·지문·readOnly·Exports·Imports·Dependencies·Summary 표·ImportRecord·Tags가 전체 읽기와 같고 Objects·Bulk·bulk 항목은 비움, 옮긴 view 유효, 읽은 byte가 파일보다 적음 | `PackageIndex_MatchesFullRead` |
| | 판정 관계: 패키지 모양 셋(풍부한 장면·최소 패키지(선택 섹션 없음)·모르는 비 Required 섹션(readOnly))마다 모든 byte의 모든 bit(8개) 손상에서 읽는 구간(헤더·표·읽는 섹션)은 두 읽기가 같은 오류·같은 진단(코드·칸·위치·에셋), 건너뛰는 구간(Objects·Bulk·BulkIndex 내용과 패딩)은 부분 읽기 통과·전체 읽기 거부, 모르는 섹션 내용은 둘 다 통과(readOnly) | `PackageIndex_EveryBitFlip` |
| | 모양 셋마다 모든 길이(0 … 파일 크기)로 자른 파일과 1 byte 늘린 파일은 두 읽기가 같은 진단으로 거부(64 byte 미만은 있는 만큼으로 헤더 판정) | `PackageIndex_TruncateAndExtend` |
| | n번째 범위 읽기 실패(모든 n)는 그 오류·진단 None, 할당 실패(모든 지점)는 OutOfMemory·진단 None | `PackageIndex_ReadAndAllocationFailures` |
| 범위 읽기(7.1-13) | 메모리·파일: 끝 4 byte·빈 범위·끝 위치 빈 범위 성공, 파일 밖 InvalidArgument, 파일 구현의 부분 읽기 = 메모리, 없는 파일 NotFound(크기 0), 다시 열기는 앞 파일을 닫음, 실패한 다시 열기는 크기 0 | `PackageRangeReader_Bounds` |
| | 열기·크기 실패는 Open의 그 오류(크기 0), 이동·읽기 실패는 ReadAt의 그 오류(빈 범위는 이동만) | `PackageRangeReader_FileFaults` |
| Tags 값 스키마(7.1-13·14) | (키, 값) 정렬 쓰기·읽기(목록 값), 빈 목록 = 빈 byte. 쓰기 거부: 빈 키·257 byte 키·4 KiB 넘는 값·잘못된 UTF-8(키·값)·같은 쌍. 읽기: 다른 스키마·더 새 Record·더 새 항목 Unsupported, 깨진 byte·순서 뒤집힘·같은 쌍·키 없음·값 없음·키 wire·값 wire·빈 키·긴 키·잘못된 UTF-8 키·값·긴 값·목록 필드 없음·목록 wire·항목 스키마 다름 InvalidArgument(17가지 손으로 만든 Record 표) | `PackageTags_WriteAndRead` |
| | 쓰기·읽기의 모든 할당 지점 실패, 실패 없으면 같은 byte | `PackageTags_AllocationFailures` |
| | 한도: 값 4 KiB·1 byte 태그마다 쓰기가 받는 가장 큰 수를 이분으로 찾아(받음·CapacityExceeded 경계) 그 수로 쓴 것은 그대로 읽히고(읽기 예산 포함) 하나 더하면 CapacityExceeded | `PackageTags_Limits` |
| 검색(7.1-13·14) | 실제 디스크: Content 없음 = 빈 색인, 재귀 검색, 대문자 확장자 색인, `.tmp`·다른 확장자·빈 디렉터리·`.rasset` 이름의 디렉터리 무시, 비 NFC 파일 이름 InvalidPath(NotNfcOnDisk — 이름 정규화 파일 시스템은 색인), 항목 값(경로·키·실제 파일 이름·대표 타입·지문 = 전체 읽기·크기·시각·의존·태그), 파일을 지우고 다시 검색하면 반영 | `AssetIndex_DiskScanAndQueries` |
| | 없는 디렉터리는 빈 디렉터리(Content 없음·열거에서 사라진 하위 디렉터리), Content 열거 실패와 Content가 파일(WrongType)은 Rebuild 오류이고 색인을 비움, 하위 디렉터리 열거 실패는 진단 None·원본 오류로 그 디렉터리만 뺌. 확장자뿐인 이름(Content 바로 아래 `.rasset`·`.RASSET`, `Dir/.rasset`)은 InvalidPath(EmptyComponent), 잘못된 UTF-8 이름(Content 바로 아래·하위 디렉터리의 패키지 파일과 디렉터리 InvalidPath(InvalidComponent), 그 디렉터리 안으로 내려가지 않음, 다른 확장자 무시), 표시 이름이 겹치지 않음(글자 그대로 "\xff"인 이름은 역슬래시 16진·Backslash, 개행·0x7f 이름은 16진·InvalidComponent) | `AssetIndex_RebuildFailures` |
| 진단(7.1-13·14) | InvalidPath(InvalidComponent), 깨진 패키지·쿠킹 패키지(부분 읽기와 같은 코드·오류·칸·위치·에셋), 다른 스키마 Tags UnsupportedVersion·깨진 Tags CorruptPackage(칸 Tags, 태그 없이 색인), 사라진 파일 MissingFile, 열기 Busy·읽기 실패 진단 None과 원본 오류, 파일·디렉터리 링크·장치·다른 확장자 무시, 같은 경로 키(대소문자·확장자 철자만 다름) 둘 다 DuplicatePath, 같은 id 둘 다 DuplicateAssetId(상대 = 정렬 이웃), 조회 AlreadyExists(id·경로·Dependencies), 진단·항목 정렬(뒤에 열거된 것이 맨 앞으로 — 경로·파일 이름·id 순), high가 다른 id, 모르는 커스텀 버전으로 저장한 항목만 readOnly | `AssetIndex_Diagnostics` |
| | 진단 줄: 코드마다(None·InvalidPath·MissingFile·DuplicateAssetId·DuplicatePath·MissingDependency·TypeMismatch·패키지 판정) 정확한 문구, 줄 앞을 0–160 byte 채워 저장소가 자라는 지점을 모든 조각 위로 옮기며 모든 할당 실패 | `AssetIndex_DiagnosticLines` |
| 조회(7.1-13) | id·경로(대소문자 무시 키·없음·끝보다 뒤), 디렉터리(마운트 철자, 바로 아래·재귀, 구성 요소 경계 "/Game/Char", 항목 자신, 잘못된 디렉터리 여섯 가지 — 5 byte 마운트 아닌 "/Gamx" 포함), 타입(정확·파생 포함·미등록 id·0 InvalidArgument), 태그(목록 원소마다·키 대소문자 구분·키와 값 바꿈), 정방향 의존·역방향 참조자를 모든 종류 묶음(32개) × 대상 8개로 저장 내용에서 직접 고른 것과 대조 | `AssetIndex_DiskScanAndQueries` |
| Verify(7.1-13·14) | Hard·Soft 누락만(EditorOnly·Build·ImportOwner 대조 안 함), 형식 대조: Registry가 없으면 하지 않음(파생 대상·파생 아님 모두 진단 없음), 등록 Registry는 파생 통과·파생 아님 TypeMismatch, 빈 Registry(미등록)는 TypeMismatch, 대상 없음 MissingDependency, 대상 id가 둘이면 대조 안 함, 종료한 Registry ShuttingDown(Verify·ListByType), 대표 타입이 구조체 id인 패키지(Summary·Exports 타입 id를 바꿔 만듦)는 파생 아님·TypeMismatch | `AssetIndex_VerifyTypes`, `AssetIndex_DiskScanAndQueries` |
| 할당 실패(7.1-13) | 진단 장면 + 형제 디렉터리 여덟(검색 대기 표가 자람) + 512 byte 넘는 프로젝트 루트(경로 저장소가 자람) + 깊은 디렉터리(TooLong) + 잘못된 UTF-8 긴 이름(표시 이름) + 열거할 수 없는 하위 디렉터리 + 맨 앞으로 옮겨지는 깨진 파일에서 Rebuild와 모든 조회(따로 둔 출력 배열)의 모든 할당 지점 — OutOfMemory, Rebuild 실패면 색인이 빔, 실패 없으면 같은 결과 | `AssetIndex_AllocationFailures` |
| 명령(13, 7.1-14) | asset-list: 줄 모양·순서(정확히 두 줄), --under(있음·없음·논리 디렉터리 아님 1), 위로 프로젝트 찾기·없음 1, --project에 기술자 없음·하위 디렉터리 1, 쓸 수 없는 루트(Windows 드라이브 상대) 1, 빈 프로젝트, Content가 파일 1, --help 0, --wait=5 받음·--wait=5s 2, 위치 인수·같은 옵션 두 번·빈 값·모르는 옵션(--type) 2 | C# `asset-list *` |
| | asset-index-verify: 누락 의존 줄과 요약 1, 빈 프로젝트 0(--wait=0 포함), Content가 파일·프로젝트 없음 1, --help 0, --under 거부 2, 문제 프로젝트의 일곱 줄을 순서까지(InvalidPath·DuplicateAssetId 둘·CorruptPackage·NewerVersion(엔진 99.0.0 저장)·MissingDependency 둘 — 같은 id에 파생 대표를 둔 Base는 Registry 없는 대조라 줄이 없다)와 요약, 다른 프로세스가 공유 없이 연 파일(Windows — 원본 오류 이름), 무효 표준 핸들에서 목록 줄·진단 줄·요약 쓰기 실패 1(Windows) | C# `asset-index-verify *`, `invalid output handles *` |
| | fixture byte(Hero·Base·같은 id의 Derived 대표·Newer, 엔진 0.1.0 저장)가 writer 출력과 같음 | `AssetIndex_CommandletFixtureBytes` |
| 조합(14 완료 조건 2) | 부분 읽기 대 전체 읽기: 모든 offset × 8 bit × 구간(읽음·건너뜀), 모든 자르기 길이, 모든 IO 실패 위치·할당 지점. 조회: 종류 묶음 32 × 대상 8 × 항목. 부분 읽기 손상·자르기는 모양 3. Verify: 장면 5(정상·파생 대상·파생 아님·대상 없음·대상 중복) × Registry 3(없음·등록·빈 것) + 종료 Registry + 구조체 대표. Tags 읽기: 손으로 만든 Record 17가지. 진단: 코드별 장면 하나씩 + 겹침(같은 파일의 DuplicatePath·DuplicateAssetId) | 위 시험 |

#### 14.1.16 Coverage(A2b)

측정(2026-11-21, Windows): `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter AssetPackage,PackageIndex,PackageTags,PackageRangeReader,AssetIndex -Sources Engine/Source/Runtime/AssetRuntime/Private,Engine/Source/Developer/AssetDatabase/Private`와 명령(`-Target Commandlet -Executable RocksCommandlet -Runner dotnet run --project Programs/Rocks.Commandlet.Tests -- --executable {executable} -Sources Programs/Commandlet/Private`).

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetDatabase/Private/AssetIndex.cpp` | 100%(608/608) | 100%(318/318) | 100%(27/27) |
| `AssetRuntime/Private/PackageTags.cpp` | 100%(144/144) | 100%(84/84) | 100%(9/9) |
| `AssetRuntime/Private/PackageRangeReader.cpp` | 100%(46/46) | 100%(20/20) | 100%(4/4) |
| `AssetRuntime/Private/PackageReader.cpp`(읽기 핵심 변경) | 100%(1077/1077) | 97.7%(736/753) | 100%(44/44) |
| `Programs/Commandlet/Private/Common/AssetIndexCommandlet.cpp` | 84.0%(158/188) | 85.4%(82/96) | 100%(7/7) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `PackageReader.cpp` | 분기 17개: 불변식 검사 `ROCKS_ASSERT`의 실패 쪽 16개와 섹션 내용 `switch`의 암묵 default | A1 측정(14.1.x)과 같다 — 모르는 섹션 종류는 그 앞에서 건너뛰므로(`IsKnownPackageSection`) switch에 닿는 종류는 모두 case가 있다 |
| `AssetIndexCommandlet` | 출력 줄 저장소의 할당 실패(`Fail`·목록 줄·진단 줄·요약), `Rebuild`·`ListUnder`·`Verify`의 할당 실패 | 프로세스 시험은 명령의 allocator에 실패를 주입할 수 없다. 같은 결과를 만드는 라이브러리 경로(색인·조회·Verify·진단 줄의 모든 할당 지점)는 `AssetIndex_AllocationFailures`·`AssetIndex_DiagnosticLines`가 덮는다 |
| `AssetIndexCommandlet` | 현재 디렉터리를 절대 경로로 못 만듦, 프로젝트 찾기의 파일 시스템 오류(NotFound 밖) | OS 조건(작업 디렉터리 조회 실패·상위 디렉터리 열거 권한 거부)이다. 작업 디렉터리를 지우는 시험은 프로세스 전역 상태라 하지 않는다(14.1.14 ProjectLayout과 같다) |

macOS·Linux 쪽(`FileSystemNormalizesNames`가 참인 비 NFC 이름 색인)은 Windows 측정 밖이고 미검증이다. 대소문자만 다른 두 파일은 Windows·macOS 기본 파일 시스템에 만들 수 없어 명령 시험에는 DuplicatePath 줄이 없다(색인은 메모리 파일 시스템, 줄 문구는 `AssetIndex_DiagnosticLines`가 덮는다). 잘못된 UTF-8 OS 이름도 Windows(UTF-16)에서는 만들 수 없어 메모리 파일 시스템으로만 시험한다.

닿게 한 방법과 시험이 찾은 것: 실제 디스크 시험이 `Exists`가 디렉터리에 거짓인 것을 잡아 Content 판정을 열거 결과로 바꿨다. coverage가 보여 준 죽은 코드를 지웠다 — 확장자 검사 뒤의 `.tmp` 검사(임시 파일은 확장자가 달라 이미 걸러진다), Verify의 expectedType 0 검사(읽기가 보장), 같은 id·경로 상대를 찾는 전체 순회(정렬 이웃으로 바꿔 끝에 닿지 않는 반복이 없어졌다), `ListUnder`의 접두 저장소(논리 경로 256 byte라 할당이 생기지 않았다 — 키 비교로 바꿨다), 바이트 비교 함수 둘(Core `StringView::CompareBytes`로 바꿨다). 진단 줄 서식은 명령에서 AssetDatabase로 옮겨 모든 코드·할당 실패를 C++에서 시험한다.

리뷰 1회(미충족) 반영: 필수 셋 — Content 바로 아래 `.rasset` 파일 하나가 색인 전체를 멈추던 것(사유 None을 할당 실패로 보던 판정 → InvalidPath(EmptyComponent)), Registry 없는 형식 대조가 로드되는 파생 참조를 TypeMismatch로 보고하던 것(→ 대조 안 함), 삽입 정렬(O(n²) → Core `algo::Sort`·`LowerBound`). 권장 반영 — 하위 디렉터리 열거 실패를 진단으로, 패키지 모양 셋의 손상·자르기, readOnly 항목, Tags 쓰기 한도와 대칭 시험, `--wait`, 명령 진단 줄 순서 단언, 스펙의 버퍼·진단 정렬 문구, ImportRecord 요약의 A2c 이월. 반영 중에 찾은 것: POSIX 열거는 잘못된 UTF-8 이름을 그대로 주는데 `String`이 그것을 거부해 이름 하나가 색인 전체를 멈추었다 → 표시 이름 + InvalidPath, 진단 줄 서식이 잘못된 UTF-8 인자를 거부하던 것도 이것으로 없어졌다(줄에는 제어 문자 escape만 남김).

재리뷰(2회) **충족**. 그 권장 둘도 반영했다: 표시 이름이 역슬래시를 그대로 두어 다른 이름과 겹치던 것(→ 역슬래시도 16진, 표시 이름을 진단 하나에서 만들고 진단 줄은 그대로 씀), 하위 디렉터리 실패에도 "package file" 문구가 나가던 것(→ "cannot read the file or directory"). `--wait` 범위 검사는 A2c로 넘긴다(7.1절 14).

A2b 상태(2026-11-21): **완료**. 완료 조건: 대조표(14.1.15) 시험 없는 항목 0·정의된 시험 16개 모두 인용, 조합(대조표 마지막 행), coverage(14.1.16 — Windows), 리뷰어 판정 1회 미충족 → 반영, 2회 **충족**.

#### 14.1.17 요구 대조표(A2c1 — AssetRuntime 저장 기반)

시험은 EngineTests `Engine/Source/Runtime/AssetRuntime/Tests/`의 `PackageStoreTests.cpp`(11개)·`BulkStoreTests.cpp`(11개)·`AssetTypeHookTests.cpp`(5개) — 정의된 시험 27개를 모두 인용한다. 공용 `StoreTestSupport.h`는 bulk 셋을 가진 장면(Zstd·None·모르는 섹션), 경로마다 byte를 가진 메모리 파일 시스템(열기·크기·이동·읽기·쓰기 고장, n번째 이동·읽기 실패, 내용 바꾸기 — 외부 변경), n번째 범위 읽기 실패를 준다. 읽기 핵심을 세 범위로 바꾼 뒤 EngineTests 전체(3702개)가 통과한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 재검증(5.5-5, 7.1-15) | 모양 셋의 모든 byte 모든 bit 손상에서 `VerifyPackageFile` = `ReadPackage`(성패·오류·진단 칸·위치·에셋), 손상 없는 파일은 조각 1·13·65536 byte 모두 통과 | `PackageVerify_EveryBitFlipMatchesFullRead` |
| | 모든 길이 자르기·1 byte 늘리기 = 전체 읽기 진단, n번째 범위 읽기 실패(모든 n) 그 오류·진단 None, 모든 할당 지점 실패, 빈 scratch InvalidArgument | `PackageVerify_TruncationIoAndAllocation` |
| | 손으로 만든 섹션(CRC 맞음): BulkIndex 배치 위반 CorruptPackage(BulkIndex), 더 새 BulkIndex NewerVersion, bulk 없는 패키지 통과 — 전체·편집 읽기·재검증 같은 진단 | `PackageVerify_CraftedSections` |
| 편집 읽기(7.1-15) | 내용 = 전체 읽기(bulk span만 비고 bulkEntries·Objects·지문·readOnly 같음), 모양 셋 모든 bit: 읽는 구간 같은 진단·Bulk 내용과 섹션 사이 패딩 편집 통과·전체 거부·모르는 섹션 둘 다 통과, 모든 자르기 같은 진단 | `PackageEditRead_RelationToFullRead` |
| 지문(5.5-1, 7.1-15) | = ReadPackage 지문, 헤더·표 CRC 손상·짧은 파일은 전체 읽기와 같은 진단, 범위 읽기 실패(헤더·표) 진단 None, 할당 실패 | `PackageFingerprint_Read` |
| entry 해제(7.1-15) | None·Zstd entry 풀기와 SHA-256, 저장·raw 크기 불일치·SHA 불일치·깨진 컨테이너 InvalidArgument, Zstd 해제의 모든 할당 실패 | `PackageBulkEntry_Decode` |
| | Array 겹판: out을 rawBytes로 키워 같은 byte, 키우기 전에 컨테이너 rawSize 대조(2^44 rawBytes는 키우지 않고 InvalidArgument), 저장 크기·깨진 컨테이너 InvalidArgument, 모든 할당 실패 | `PackageBulkEntry_DecodeIntoArray` |
| 파일 sink(7.1-15) | 파일 sink byte = 메모리 sink byte(WritePackage), 쓴 수, 빈 Append, 범위 밖 WriteAt InvalidArgument, 쓰기·이동·끝으로 돌아가는 이동 실패는 파일 오류 | `FilePackageSink_WritesSameBytes` |
| 지연 로드(7.1-15) | Defer + 범위 읽기 Prepare + SetBulkStore → InstantiatePackages가 Package에 저장소를 옮김, 처음 BulkBytes에서만 파일을 열고 같은 span, Decode의 범위 읽기 Prepare·두 번째 Prepare·Prepare 전·null·인스턴스 뒤 SetBulkStore InvalidArgument, `SameContent`(id 무시·크기 비교) | `PackageLoad_DeferredBulk` |
| | 범위 읽기 Prepare의 판정 실패 진단, Decode 상태 SetBulkStore InvalidArgument(저장소는 호출자에게), BulkDataRef의 저장하지 않은 id 표지 CorruptPackage(BulkIndex, detail = id) | `PackageLoad_DeferredFailures` |
| | bulk entry가 있는 Defer 상태를 저장소 없이 InstantiatePackages에 넘기면 InvalidArgument, bulk 없는 패키지는 저장소 없이 열림 | `PackageLoad_DeferRequiresStore` |
| 저장소(6.1, 7.1-15) | 지연 읽기(Zstd·None 내용 일치, 처음만 열기, 같은 span, ReadBulk), FileBulkId, ref 판정(빈 InvalidArgument·없는 id NotFound·크기·SHA TypeMismatch), 다시 Attach·빈 경로 InvalidArgument | `FilePackageBulkStore_LazyRead` |
| | 손상(CRC·풀기·SHA) 그 entry만 CorruptPackage(Bulk, detail = 파일 id, 에셋)·다른 entry 읽힘, 진단 없는 ReadBulk도 그 오류, 외부 변경 ExternalModification / StaleHandle(캐시 비움), 연 핸들은 교체 뒤 이전 내용, 파일 없음 NotFound, 열기·크기·읽기·저장 byte 읽기 실패 진단 None | `FilePackageBulkStore_LazyFailures` |
| | 지연 읽기의 모든 할당 실패 뒤 다시 읽으면 같은 byte, Attach의 모든 할당 실패 뒤 빈 저장소·다시 Attach | `FilePackageBulkStore_ReadAllocationFailures` |
| | 저장하지 않은 bulk(최상위 bit id 차례, SHA-256, 읽기, 파일 id 0), 저장하지 않은 bulk가 있으면 Attach 거부, CopyFrom(새 ref·원본 오류 전달), 모든 할당 실패 | `FilePackageBulkStore_UnsavedAndCopy` |
| | 저장(저장소를 bulk source로) 뒤 Rebind: 옛 ref(파일·저장하지 않은 id)가 새 파일 entry, 빠진 id NotFound·이미 준 span 유효, 잘못된 표(없는 옛 id·0·범위 밖·내용 다름)와 손으로 만든 entry 표(id·크기·SHA 다름)·빈 경로 InvalidArgument로 그대로, 모든 할당 실패도 그대로, 읽지 않은 채 빠진 entry는 Detach가 건너뜀, 이은 적 없는 저장소의 Rebind | `FilePackageBulkStore_RebindAfterSave` |
| | Detach: 남은 entry를 다 읽고 연결 끊기(파일을 지워도 읽힘·캐시 닫힘), 손상 entry면 그 오류·연결 유지, 두 번째 무동작 | `FilePackageBulkStore_Detach` |
| | 빈 재매핑 Rebind → Detach → 다시 Attach: Attach의 모든 할당 실패에서도 이미 준 span 유효·저장소 빔, 성공 뒤 다시 읽힘 | `FilePackageBulkStore_ReattachKeepsSpans` |
| | 손상된 표(Zstd entry rawBytes 2^44): raw를 잡기 전에 CorruptPackage(할당 실패가 아님) | `FilePackageBulkStore_HugeRawBytesRejectedBeforeAllocation` |
| | 새 패키지에 AttachBulkStore → BulkBytes가 저장소에(진단 포함), null·이미 저장소·로드한 bulk가 있는 패키지 InvalidArgument(저장소는 호출자에게) | `Package_AttachBulkStore` |
| 핸들 캐시(7.1-15) | 상한 2 LRU(뒤 항목이 오래된 경우·앞 항목이 오래된 경우), 다른 지문 ExternalModification·핸들 닫음, 없는 경로 Close 무동작, 상한 0 InvalidArgument, 헤더 손상 진단 | `PackageFileCache_LeastRecentlyUsed` |
| | 저장소 소멸이 자기 핸들을 닫음, CloseUnder(구성 요소 경계 — "Content/A"는 "Content/AB"를 닫지 않음·파일 경로는 디렉터리가 아님·다른 접두 무동작) | `PackageFileCache_CloseOnStoreDestructionAndUnder` |
| 타입 hook(7.1-15) | 가장 가까운 hook(Base만 등록 → Derived에 Base hook), CallBase(둘 다)·안 부름(부모 끔), 종류마다 따로 찾기(Derived에 Build hook 없으면 Base의 것), hook 없는 타입 무동작, 최상위 CallBase 무동작 | `AssetTypeHooks_NearestAndCallBase` |
| | 0 타입·hook 둘 다 null InvalidArgument, 같은 타입 AlreadyExists, hook 오류 전달(직접·CallBase 거쳐·Build), 미등록 타입 NotFound(Collect·CallBase) | `AssetTypeHooks_Errors` |
| | 등록 handle 소멸·Release·이동·이동 대입·자기 대입, 두 번째 Release 무동작 | `AssetTypeHooks_RegistrationLifetime` |
| | 태그 sink(PackageTags 규칙 검사·합치기·(키, 값) 정렬 삽입 — 같은 키 목록 값·정렬된 Write·빈 Write), 의존 sink(null 거부·정렬·유일·종류 Build), 모든 할당 실패 | `AssetTypeHooks_Sinks` |
| | 등록의 모든 할당 실패 뒤 표 그대로 | `AssetTypeHooks_RegisterAllocationFailures` |
| 조합(14 완료 조건 2) | 읽기 셋(전체·편집·재검증) × 패키지 모양 셋(Zstd·None·모르는 섹션) × 모든 offset·bit·자르기 길이, 손으로 만든 섹션 셋. 저장소: 파일 entry·저장하지 않은 bulk·복사 bulk × (읽기·저장·다시 잇기·Detach), 고장(열기·크기·읽기·외부 변경·손상 셋) × 첫 접근·캐시된 핸들. hook: 등록 위치(없음·Base·Derived·둘 다) × CallBase(함·안 함) × 종류 둘 | 위 시험 |

#### 14.1.18 Coverage(A2c1)

측정(2026-11-21, Windows, `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter AssetPackage,PackageIndex,PackageVerify,PackageEditRead,PackageFingerprint,PackageBulkEntry,FilePackageSink,PackageLoad_Deferred,FilePackageBulkStore,PackageFileCache,Package_AttachBulkStore,AssetTypeHooks,AssetLoader -Sources Engine/Source/Runtime/AssetRuntime/Private`):

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `FilePackageBulkStore.cpp`(새) | 99.3%(303/305) | 99.3%(139/140) | 100%(21/21) |
| `AssetTypeHooks.cpp`(새) | 98.8%(171/173) | 97.6%(80/82) | 100%(17/17) |
| `PackageReader.cpp`(범위 셋·재검증·지문·entry 해제) | 100%(1281/1281) | 98.0%(842/859) | 100%(51/51) |
| `PackageLoad.cpp`(Defer·저장소·저장하지 않은 id) | 100%(487/487) | 98.0%(296/302) | 100%(17/17) |
| `Package.cpp`(저장소 연결) | 100%(47/47) | 93.8%(30/32) | 100%(4/4) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `FilePackageBulkStore::AddUnsaved` | 저장하지 않은 id 소진(CapacityExceeded) | 2^31 − 1개를 더해야 닿는다(그만큼의 slot 메모리). 넘침을 Fatal로 두지 않고 오류로 돌려준다 |
| `AssetTypeHookTable::Remove` | 찾는 타입이 표에 없을 때의 반복 끝 | 등록 handle이 있는 타입만 지운다(handle은 Release·소멸에서 한 번만 부른다) — 항목이 없을 수 없다 |
| `~AssetTypeHookTable` | 등록이 남은 채 소멸하면 Fatal | 수명 계약 위반(남은 handle의 Release가 해제된 표를 쓴다)이라 프로세스를 끝낸다 — 시험 프로세스 안에서 부를 수 없다(`ReferenceObserver` 소멸 검사와 같다) |
| `PackageReader.cpp` | 분기 17개(ROCKS_ASSERT 16·섹션 내용 switch의 암묵 default) | A2b(14.1.16)와 같다. 새 범위 판정은 if 사슬로 써서 암묵 default가 없다 |
| `PackageLoad.cpp`·`Package.cpp` | 분기 6·2개 | `ROCKS_ASSERT` 전개 안의 실패 쪽이다(`uncovered.txt`에 보이지 않고 합계에만 든다 — 14.1.x A1과 같다) |

시험이 찾은 것: 재검증이 더 새 schemaVersion의 BulkIndex를 어떻게 다룰지 넣은 조건이 닿지 않았다 — BulkIndex는 Required라 부분 읽기가 먼저 `NewerVersion`으로 거부한다(조건을 빼고 손으로 만든 섹션 시험으로 고정했다). hook의 `CallBase`를 base 사슬 찾기와 한 함수로 합쳐 닿지 않던 오류 분기를 없앴다. 시험 지원 헤더 하나가 편집기 프로세스에 잠겨 쓸 수 없어 새 이름(`StoreTestSupport.h`)으로 옮겼다.

리뷰(1회) **충족**. 권장 다섯도 반영했다: Attach의 할당 실패가 Detach 뒤 남은 slot(이미 준 span)을 지우던 것(→ 호출 전 slot까지만 되돌림), 닫은 패키지의 캐시 핸들이 남던 것(→ 저장소 소멸자가 닫음·`CloseUnder` — A2e 디렉터리 작업용), 저장소 없는 Defer 상태를 조용히 받던 것(→ InvalidArgument), 손상된 표의 큰 지연 할당(→ 컨테이너 rawSize를 먼저 대조하는 `DecodePackageBulkEntry` Array 겹판), 등록이 남은 hook 표 소멸(→ Fatal). 태그 sink는 정렬 삽입(이분 찾기)으로 바꿨다. 복제의 저장소 표 복제(F5 c)는 A2e에서 더한다.

A2c1 상태(2026-11-21): **완료**(리뷰 1회 충족 + 권장 반영). 완료 조건: 대조표(14.1.17) 시험 없는 항목 0·정의된 시험 27개 모두 인용, 조합(대조표 마지막 행), coverage(14.1.18 — Windows), 리뷰어 판정 **충족**.

#### 14.1.19 요구 대조표(A2c2a — null import·ImportRecord·색인 요약·열기)

시험은 FoundationTests `GraphExternalLoadTests.cpp`의 1개, EngineTests `Engine/Source/Runtime/AssetRuntime/Tests/PackageImportPolicyTests.cpp`(2개), `Engine/Source/Developer/AssetDatabase/Tests/`의 `ImportRecordTests.cpp`(6개)·`AssetIndexTests.cpp`의 `AssetIndex_ImportRecords`(1개 — 기존 색인 시험은 Hero에 참조 기록을 더해 그대로 통과)·`EditorPackageSetTests.cpp`(8개) — 정의된 시험 18개를 모두 인용한다. 메모리 파일 시스템(`MemoryFileSystem.h` — 색인 시험에서 옮기고 핸들을 풀로 바꿔 bulk 캐시가 쥔 동안 다른 파일을 연다)과 디스크 쓰기 도우미는 공용 시험 헤더로 옮겼다(동작 불변).

| 요구(절) | 항목 | 시험 |
|---|---|---|
| null import(ObjectFoundations 5.4, 7.1-15) | imports의 null handle은 확인을 건너뛰고 그 외부 참조를 읽는 단일 참조·배열 원소를 null로(다른 필드 그대로), 기대 형식이 있는 선언도 판정하지 않음 | `GraphExternalLoad_NullImportIsUnresolved` |
| NullMissing(7.1-15) | B 모양마다(온전·없음·대표 타입 다름·하위 객체 없음·하위 객체 타입 다름) 미해결 기록(위치·코드·오류)과 null 참조(Hard·배열), outer 미해결의 연쇄, Strict는 없는 대상 MissingDependency 실패 | `PackageImportPolicy_NullMissing` |
| | NullMissing에 기록 표 null InvalidArgument(상태 재사용 가능), 기록 할당 실패 OutOfMemory | `PackageImportPolicy_ArgumentsAndAllocation` |
| ImportRecord 배치(5.4, 7.1-15) | 소유 기록 쓰기 = 손으로 만든 정규 배치(출력 key 순 정렬), 모든 값 왕복(원본은 쓴 순서), options 없는 소유 기록·참조 기록도 정규 배치와 같고 왕복, 소유 기록을 다른 self로·참조 기록을 owner로 읽으면 InvalidArgument | `ImportRecord_RoundTripAndLayout` |
| | 경계 양성: 이름 256·role 64·root 256·빈 root·relativePath 1024·점이 든 성분(".hidden"·".a"·"..."·"a.")·outputKey 64·빈 key, 원본 256개·출력 1024개(역순 입력 → key 순), bytes 0·최대 | `ImportRecord_BoundaryValues` |
| | 쓰기 규칙 위반 43가지(self·owner null, 임포터 id·이름 빈 값·257·UTF-8·NUL, 원본·출력 0개·상한+1, options envelope, role 빈 값·65·NUL·중복, root 257·`/`·`\`·UTF-8, 경로 빈 값·1025·앞·뒤·연속 `/`·`.`·`..`·안쪽 `..`·`\`·UTF-8, key 65·UTF-8, 출력 asset·type null, key·asset 중복, self 없음, 참조 기록의 각 칸) 모두 InvalidArgument | `ImportRecord_WriteRejects` |
| | 읽기 손상 70가지: 67가지 InvalidArgument(필드 없음 4·wire·크기 6·owner null·모르는 필드·임포터 규칙 4·목록 없음 2·목록·options wire 3·options 빈·envelope, 원본 목록 빈·초과·잘림·항목 schema, 원본 항목 필드 없음 5·추가·wire·크기 7·규칙 3·role 중복, 출력 목록 빈·초과·항목 schema, 출력 항목 필드 없음 3·추가·wire·크기 3·규칙 3·순서·key·asset 중복·self 없음, 참조 기록의 각 칸 6)와 기록·원본·출력 항목 더 새 버전 3가지 Unsupported, 빈 byte·null self·header 아닌 byte InvalidArgument, 더 새 envelope·다른 스키마(Tags) Unsupported | `ImportRecord_ReadRejects` |
| ImportOwner 대조(7.1-7·15) | 기록 없음·소유 기록은 ImportOwner 없어야, 참조 기록은 owner를 가리키는 항목 정확히 하나(없음·다른 대상·둘 위반), 다른 종류 항목은 세지 않음, self null InvalidArgument | `ImportRecord_CheckImportOwner` |
| | 소유·참조 기록 쓰기·읽기의 모든 할당 실패 OutOfMemory, options가 값 한도(4 MiB)를 넘으면 CapacityExceeded | `ImportRecord_AllocationAndCapacity` |
| 색인 요약(7.1-11·15) | 소유 기록 요약(임포터 id·이름·버전·출력 수·owner = 자기), 참조 기록 + 맞는 ImportOwner 요약(owner만)·진단 없음, 다른 스키마·더 새 버전 UnsupportedVersion·형식 위반 CorruptPackage(칸 ImportRecord, 요약 없음, 대조 안 함), 대조 위반 넷 CorruptPackage(칸 Dependencies), 모두 색인, 한 파일의 같은 코드 진단 둘은 칸 순, 모든 할당 실패 | `AssetIndex_ImportRecords` |
| | Hero의 참조 기록 요약(owner = 2104, 나머지 빈 값)·기록 없는 항목 present 거짓(디스크 색인) | `AssetIndex_DiskScanAndQueries` |
| 열기 닫힘(7.1-3·15) | Hard·EditorOnly 닫힘 한 번에(Soft 대상 열지 않음), 참조 이어짐(Hard·typed·EditorOnly), 정보 id 순·파일 이름·지문 = 색인·읽기 전용 아님·미해결 없음, resolver는 열린 것만, 지연 bulk 처음만 파일 열기, 이미 연 패키지 같은 handle(다시 읽지 않음)·경로 열기 같은 handle, 경고 표 없이 열기, 순환 한 트랜잭션 | `EditorPackageSet_OpenClosure` |
| | 이어 열기: 먼저 연 Hero를 Villain이 resolver로 잇고 Villain만 읽음, 먼저 연 Base의 대표 타입이 Hero의 기대와 다르면 resolver로 찾은 대상도 Hero import만 null(UnresolvedImport TypeMismatch) | `EditorPackageSet_IncrementalOpen` |
| 누락 의존(7.1-15) | Base 모양 16가지(정상·색인에 없음·같은 id 둘·색인 뒤 사라짐·열기 Busy·읽기 실패·손상·다른 id·ImportRecord 손상·ImportOwner 위반·다른 스키마 기록·모르는 커스텀 버전·대표 타입 다름·더 새 엔진·유효 소유 기록·유효 참조 기록 + 맞는 ImportOwner)마다 Hero 열기: 열린 수, Base 열림·읽기 전용, ExcludedDependency(코드·오류·칸 — 손상·버전은 같은 byte를 직접 Prepare한 진단과 같음, asset Base·related Hero), UnresolvedImport(asset Hero·related Base·ObjectImports import 0·MissingDependency 또는 TypeMismatch)와 정보의 미해결 표, EditorOnly 참조 영향 없음 | `EditorPackageSet_DependencyFailures` |
| 요청 패키지 실패(7.1-15) | 같은 16 모양을 Base로 직접 열면 뺀 경고의 진단이 Open 실패(색인에 없음은 진단 None)·아무것도 안 열림, 읽기 전용·타입 다름·정상·유효 기록 둘은 열림, 색인에 없는 id NotFound, 경로 없음 NotFound, 같은 경로 키 둘 DuplicatePath / AlreadyExists | `EditorPackageSet_RootFailures` |
| 인자·수명(7.1-15) | 설정 null 넷·캐시 용량 0·관찰자 키 0 InvalidArgument, Initialize 전 Open(id·경로) InvalidArgument(진단 None), 두 번째 Initialize·null id InvalidArgument, 파괴하면 집합 allocator 할당이 모두 돌아오고 bulk 캐시 핸들이 닫힘 | `EditorPackageSet_ArgumentsAndLifetime` |
| | Initialize·Open(유효 기록이 있는 Hub 닫힘 8개 — 닫힘 표 성장·기록 읽기 포함, 경고 있는 닫힘)의 모든 할당 실패 OutOfMemory·아무것도 안 열림, Registry가 계약으로 삼키는 GC 참조 칸 표 할당 실패 하나만 성공(그 할당에 고유 tag GCRT를 주었다 — 리뷰 권장) | `EditorPackageSet_AllocationFailures` |
| | EditorOnly로만 가리킨 대상 누락: 그 import(1)만 null·UnresolvedImport(Hard는 이어짐), 깊이 2 제외: 빠진 Base의 related는 중간 Hero·미해결 import는 Hero의 것(Villain 참조 이어짐) | `EditorPackageSet_EditorOnlyAndDeepExclusion` |
| | 실제 디스크: 물리 파일 시스템으로 열기·지연 bulk 읽기·참조 | `EditorPackageSet_Disk` |
| 조합(14 완료 조건 2) | ImportRecord: 기록 종류 둘(소유·참조) × options 유무 × 모든 필드의 경계·위반, 쓰기 위반 43 × 읽기 손상 70 + 입력 5(빈 byte·null self·header 아님·더 새 envelope·다른 스키마). 색인: 기록 모양(없음·소유·참조·다른 스키마·더 새·손상) × ImportOwner 항목(없음·맞음·다름) × 깨진 Tags 유무. 열기: Base 모양 16 × 열림 위치(의존·요청한 패키지)·대표 타입 다름 × 해석 경로(같은 묶음·resolver), 열린 상태(처음·이미 연 의존·이미 연 요청), 경로·id 열기, 경고 표 유무 | 위 시험 |

#### 14.1.20 Coverage(A2c2a)

측정(2026-11-21, Windows, `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter ImportRecord_,AssetIndex_,EditorPackageSet_,PackageImportPolicy_,PackageLoad,AssetPackage -Sources Engine/Source/Developer/AssetDatabase/Private,Engine/Source/Runtime/AssetRuntime/Private`). 그래프 쪽 null import(`GraphExternal.cpp`·`GraphApply.cpp`의 몇 줄)는 FoundationTests `GraphExternalLoad_NullImportIsUnresolved`가 지난다.

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `ImportRecord.cpp`(새) | 100%(497/497) | 100%(366/366) | 100%(32/32) |
| `EditorPackageSet.cpp`(새) | 99.5%(372/374) | 99.4%(171/172) | 100%(18/18) |
| `AssetIndex.cpp`(요약·대조·진단 정렬) | 100%(662/662) | 100%(336/336) | 100%(28/28) |
| `PackageLoad.cpp`(NullMissing) | 100%(568/568) | 98.2%(334/340) | 100%(19/19) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `EditorPackageSet::PreparePending` | `SetBulkStore` 실패 | 그 계약의 실패는 인자 위반뿐이다(Decode 옵션·Prepare 전·인스턴스 뒤·null 저장소). 여기서는 Defer로 Prepare를 마친 새 상태에 방금 만든 저장소를 건다 — 닿을 수 없다. Release에서도 안전하게 오류로 돌려준다 |
| `PackageLoad.cpp` | 분기 6개 | `ROCKS_ASSERT` 전개 안의 실패 쪽이다(14.1.18과 같다) |

시험이 찾은 것: 처음 측정에서 유효한 ImportRecord를 가진 패키지를 여는 경로·닫힘 표 성장의 할당 실패·NullMissing의 "대표 에셋 없음"·outer 연쇄 기록의 할당 실패·경로 성분 ".a"가 닿지 않아 시험을 더했다(Base 모양 둘·Hub 닫힘·import 정책 변형·경계값). ImportRecord의 정렬 동률 처리(index 순)는 같은 원소를 어차피 중복으로 거부하므로 닿지 않는 분기라 뺐다. 할당 실패 시험이 Registry의 GC 참조 표 할당 실패를 삼키는 계약(실패하면 trace로 대신 — 정확성 같음)을 드러냈다 — 그 할당은 GC 타입 표와 tag(GCTY)를 같이 써서 구분되지 않았으므로 고유 tag(GCRT)를 주고 시험은 그 tag의 실패만 성공으로 받는다. 시험용 메모리 파일 시스템은 핸들 하나만 열 수 있어 bulk 캐시가 핸들을 쥔 동안 다른 파일을 열지 못했다 — 핸들 풀로 바꿔 공용 헤더로 옮겼다.

리뷰(1회) **충족**. 권장 셋을 반영했다: Registry가 삼키는 GC 참조 칸 표 할당에 고유 tag(GCRT — 같은 tag를 실패를 돌려주는 GC 타입 표가 쓰고 있었다), EditorOnly로만 가리킨 대상 누락·깊이 2 제외 시험(`EditorPackageSet_EditorOnlyAndDeepExclusion`), 스펙 문구(소멸 순서의 Discard, 열린 패키지의 미해결 import는 다시 잇지 않음, 경고 detail의 뜻).

A2c2a 상태(2026-11-21): **완료**(리뷰 1회 충족 + 권장 반영). 완료 조건: 대조표(14.1.19) 시험 없는 항목 0·정의된 시험 18개 모두 인용, 조합(대조표 마지막 행), coverage(14.1.20 — Windows), 리뷰어 판정 **충족**.

#### 14.1.21 요구 대조표(A2c2b — 저장·새 패키지·색인 Refresh)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/EditorPackageSaveTests.cpp`(14개)와 리뷰 반영으로 더한 `Engine/Source/Runtime/AssetRuntime/Tests/BulkStoreTests.cpp`의 `FilePackageBulkStore_TwoPhaseRebind`, `AssetDatabaseLockTests.cpp`의 `AssetDatabaseLock_SameProcess`(잠금 대조 단언 추가) — 정의된 시험 16개를 모두 인용한다(열기 시험의 할당 실패도 참조 기록 모양을 더해 다시 돈다). 저장 장면은 루트를 디스크 임시 디렉터리로 두어 이 프로젝트의 잠금을 잡고(Content 파일은 메모리), 빈 hook 표를 넘긴다. 열기 시험의 장면 도구(원본 패키지 묶음·메모리 프로젝트·설정·tag 기록 allocator)는 `EditorPackageSetTestSupport.h`로 옮겼고(동작 불변), 메모리 파일 시스템은 쓰기(만들기·n번째 쓰기 실패·n번째 쓰기 손상·Flush·Close·Replace·NotDurable·Delete·Stat·디렉터리 만들기 고장)를 더했다 — 내용은 노드와 열린 핸들이 함께 쥐는 공유 버퍼라 교체 뒤에도 연 핸들은 이전 내용을 읽는다. 잠금은 디스크 임시 프로젝트에서 잡는다(메모리 루트는 디스크에 없다).

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 저장(5.5-1–8, 7.1-15) | 고친 Hero 저장: 교체 한 번·임시 파일 없음·경고 없음, dirty 해제(세대 그대로), 지문 = 새 파일 = 색인 항목, 저장 전에 읽지 않은 bulk가 새 파일에서 읽힘(Rebind), 바뀌지 않은 패키지 다시 저장 = 같은 byte(결정적)·이미 준 span 유효, 다른 집합으로 다시 열면 값·bulk(`SameContent`)·Hard 참조 그대로 | `EditorPackageSave_RoundTrip` |
| 새 패키지(7.1-15) | Create(dirty·세대 1·파일 없음·논리 경로), 저장하지 않은 bulk와 열린 패키지 참조를 담아 첫 저장: 부모 디렉터리 만들기·NFC 이름·색인 등록, 다시 열면 값·bulk·참조 그대로. 만들기 거부: type 0·Initialize 전 InvalidArgument, 색인의 같은 경로·저장하지 않은 새 패키지의 같은 경로 DuplicatePath, 열린 id·색인 id DuplicateAssetId, 에셋 클래스가 아닌 type은 Registry 오류(남는 것 없음) | `EditorPackageSave_NewPackage` |
| | 첫 저장 거부: 대상 파일이 생김·색인에 대소문자만 다른 같은 경로 키(하나·둘) DuplicatePath / AlreadyExists, 절대 경로가 OS 상한 초과면 만들기가 InvalidPath / CapacityExceeded(아무것도 안 만듦), 대상 Stat 실패 진단 None, 만들기의 색인 같은 경로 키 둘·같은 id 둘 거부, MarkDirty·Save의 없는 id NotFound, 디렉터리 만들기 실패 진단 None(아무것도 안 씀 — 고장을 걷으면 저장됨) | `EditorPackageSave_NewPackageRefusals` |
| 판정 순서(7.1-15) | Initialize 전·잠금 안 잡음·hook 표 없음·다른 프로젝트의 잠금 InvalidArgument, 열리지 않음 NotFound(열린 것보다 작은·큰 id), 편집이 대표 에셋을 Discard한 패키지(등록 hook·빈 hook 표 모두 NotFound — 아무것도 안 씀), 읽기 전용 NewerVersion / Unsupported, 미해결 import MissingDependency / InvalidArgument(related), dropUnresolved: DroppedUnresolvedImport(related·import 위치)·미해결 표 비움·다시 열면 null·경고 없음·Base 의존 없음, 거부는 아무것도 안 씀 | `EditorPackageSave_Refusals` |
| 외부 변경(5.5-1) | 연 뒤 대상이 다른 패키지·지워짐·헤더 손상 ExternalModification / StaleHandle, 대상 열기 Busy·읽기 실패 진단 None과 그 오류, 아무것도 안 씀·정보 그대로 | `EditorPackageSave_ExternalModification` |
| IO 고장(5.5-2–8) | 만들기·n번째 쓰기(모든 n)·Flush·Close·검증용 다시 열기·교체 실패 진단 None과 그 오류, n번째 쓰기 손상(모든 n) 재검증 CorruptPackage(나중에 덮어쓰는 헤더 자리 하나만 저장되고 결과 = 깨끗한 저장), 대상 byte·지문·dirty 그대로·임시 파일 지워짐, 교체와 지우기가 둘 다 실패하면 임시 파일이 남고 다음 저장이 고아로 지움(지울 수 없으면 OrphanNotDeleted 경고 — 경고 표 없이도 같고, 지우기 실패가 둘이어도 첫 오류 하나, 고장을 걷으면 지워짐), NotDurable 교체는 성공 + NotDurable 경고, 디렉터리 열거 실패는 OrphanNotDeleted 경고 | `EditorPackageSave_IoFailures` |
| dirty·hook(7.1-5·15) | MarkDirty: 열리지 않음 NotFound·세대 오름, hook 오류(태그·Build)·태그가 Record 한도 초과(CapacityExceeded) 진단 None과 그 오류·아무것도 안 씀, 저장 중(hook 안) MarkDirty면 dirty 남음, Tags는 hook으로 다시 만듦(파일의 옛 태그 사라짐, hook 없으면 빈 Tags), Build 의존 기록 | `EditorPackageSave_DirtyAndHooks` |
| ImportRecord 보존(7.1-15) | 소유 기록·참조 기록 + ImportOwner를 가진 Base 저장: 기록 byte 그대로·ImportOwner 의존 그대로·색인 요약 같음 | `EditorPackageSave_ImportRecordPreserved` |
| 할당 실패(7.1-15) | 기존 패키지(bulk)·새 패키지 저장(hook·경고 표·고아·색인 allocator까지)의 모든 할당 실패 OutOfMemory이고 언제나 교체 전이다(교체 뒤의 다시 잇기·색인 갱신은 교체 전에 준비 — 리뷰 권장): 대상·정보·색인 그대로·교체 없음, 고아 정리의 할당 실패는 저장 성공·고아 남음·OrphanNotDeleted / OutOfMemory 경고(잡아 둔 자리) | `EditorPackageSave_AllocationFailures` |
| | 호출자의 TypeRegistry가 닫힌 뒤 hook 있는 저장: 대표 에셋 타입 조회 ShuttingDown(진단 None·아무것도 안 씀 — 리뷰 필수) | `EditorPackageSave_TypeRegistryClosed` |
| 2단계 다시 잇기(7.1-15) | PrepareRebind: 잘못된 표·빈 경로 InvalidArgument·모든 할당 실패에서 저장소 그대로·plan 준비 안 됨, CommitRebind 뒤 옛 ref가 새 파일 entry·재매핑에 없는 id NotFound(준 span 유효)·plan 비움 | `FilePackageBulkStore_TwoPhaseRebind` |
| 잠금 대조(7.1-15) | IsHeldFor: 잡은 루트와 같은 배치(다른 객체 포함)만 참, 다른 루트·Initialize 전·놓은 뒤·잡지 않은 객체 거짓 | `AssetDatabaseLock_SameProcess` |
| | Create의 모든 할당 실패 OutOfMemory(진단 None)·집합에 남는 것 없음·다시 만들 수 있음 | `EditorPackageSave_CreateAllocationFailures` |
| 고아 모양(5.5-8) | 정확한 모양만 지움(".rasset" 대소문자 무시): 다른 디렉터리·대문자 guid·".TMP"·guid 앞 '.' 없음·".rasset" 아님·빈 이름·짧은 이름·하이픈 자리·16진 아님·디렉터리·링크·UTF-8 아님은 남김 | `EditorPackageSave_OrphanShapes` |
| 색인 Refresh(7.1-15) | 바뀐 파일 항목 교체, 새 파일 더함(같은 id 둘 → 둘 다 DuplicateAssetId), 없어진 파일 뺌(풀림), 같은 경로 키 진단 다시 정함, 이름 규칙 위반 InvalidPath, 링크·디렉터리 색인 안 함, Stat 실패 진단 None(이름 순), `.rasset` 아님·확장자뿐 InvalidArgument, 모든 할당 실패(긴 Content 경로·Stat 진단 포함)에서 색인 그대로(준비·확정) | `AssetIndex_Refresh` |
| 실제 디스크 | 물리 파일 시스템에서 기존 패키지 저장·새 디렉터리의 새 패키지 저장, 다시 색인·열기(임시 파일 없음·진단 없음) | `EditorPackageSave_Disk` |
| 조합(14 완료 조건 2) | 저장 대상(기존·새) × 판정 거부 다섯 × 외부 변경 다섯, IO 고장 종류 × 쓰기 위치 전부(쓰기 실패·쓰기 손상), 교체 결과(성공·실패·NotDurable) × 임시 파일 지우기(성공·실패) × 다음 저장의 고아 정리(성공·실패), hook(없음·태그·Build·오류 둘·안에서 MarkDirty), ImportRecord(소유·참조), 할당 실패 위치 전부 × (교체 전·뒤) | 위 시험 |

#### 14.1.22 Coverage(A2c2b)

측정(2026-11-21, Windows — 리뷰 반영 뒤 다시): `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target EngineTests -Filter EditorPackageSave_,AssetIndex_,EditorPackageSet_,ImportRecord_,PackageImportPolicy_,FilePackageBulkStore,AssetDatabaseLock -Sources Engine/Source/Developer/AssetDatabase/Private`, 저장소는 `-Filter FilePackageBulkStore,PackageFileCache,Package_AttachBulkStore,PackageLoad_Deferred,EditorPackageSave_ -Sources Engine/Source/Runtime/AssetRuntime/Private`.

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `EditorPackageSet.cpp`(열기 + 만들기·저장) | 99.1%(933/941) | 98.4%(424/431) | 100%(37/37) |
| `AssetIndex.cpp`(Refresh 준비·확정, 게시 준비·확정) | 100%(804/804) | 99.5%(417/419) | 100%(34/34) |
| `AssetDatabaseLock.cpp`(잠금 대조) | 100%(36/36) | 100%(18/18) | 100%(3/3) |
| `ImportRecord.cpp` | 100%(497/497) | 100%(366/366) | 100%(32/32) |
| `FilePackageBulkStore.cpp`(2단계 다시 잇기) | 99.4%(333/335) | 98.0%(148/151) | 100%(23/23) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `PreparePending` | `SetBulkStore` 실패 | 14.1.20과 같다(계약의 실패가 인자 위반뿐) |
| `Create`의 id 발급, `WriteAndReplace`의 임시 이름 guid | `AssetId::TryNew`·`TryNewGuid` 실패 | OS 보안 난수 실패(Unsupported)다 — 시험이 OS 난수를 실패시킬 길이 없다. 오류로 돌려준다 |
| `PlanSave`·`WriteAndReplace`의 줄 둘(`TryReserve` 여러 줄 문의 첫 줄, `reread.contents` 참조) | 줄 실행 수 0 | 앞뒤 줄이 모두 실행되는 직선 코드다 — llvm-cov의 여러 줄 문·참조 선언 줄 귀속 오차로 본다(분기 없음) |
| `EditorPackageSet.cpp` 분기 4개·`AssetIndex.cpp` 2개·`FilePackageBulkStore.cpp` 2개 | `ROCKS_ASSERT` 전개 | Create의 방금 공개한 Package 고정·접근·저장소 잇기, 확정의 준비 상태·변경 세대 대조 — 소유 스레드·준비 계약상 실패할 수 없다(TypeRegistry가 닫힐 수 있는 타입 조회는 리뷰 필수로 오류로 바꿨다) |
| `FilePackageBulkStore::AddUnsaved` | 저장하지 않은 id 소진 | 14.1.18과 같다(2^31 − 1개) |

시험이 찾은 것: (1) 진단을 받는 호출이 진단을 채우지 않고 실패하면(sink 쓰기·할당) `Save`가 빈 진단의 오류(None)를 돌려줘 `InvalidArgument`로 보였다 — 실제 오류를 진단에 채우게 고쳤다(`EditorComplete`). (2) 고아 정리 경고가 할당 실패로 사라질 수 있었다 — 자리를 교체 전에 잡고 실패를 경고 하나로 모은다. (3) Create의 할당 실패 시험이 실패한 Create가 늘린 표 용량 때문에 저장소 할당을 건너뛰었다 — 시도마다 같은 상태의 새 집합에서 만든다(같은 함정을 다른 할당 실패 시험에서도 확인했다 — 모두 시도마다 새 상태다). (4) extra 의존 모으기는 자리를 한 번에 잡아 닿지 않던 두 번째 할당 실패 분기를 없앴다. 리뷰(1회 미충족)가 찾은 것: 대표 에셋 타입 조회를 단언했는데 호출자의 TypeRegistry가 닫히면 실패한다 — 오류로 바꾸고 시험을 더했다. 리뷰 권장으로 교체 뒤의 실패를 없앴다(저장소 다시 잇기·색인 갱신을 준비·확정으로 나눠 준비를 교체 전에 — 처음 구현은 교체 뒤 할당 실패에서 정보를 새 파일로 두고 dirty를 지우는 계약이었다), 잠금을 프로젝트와 대조하고, 저장에 hook 표를 필수로 받는다(경로 상한 판정은 그래서 Create로 옮겼다 — 긴 루트는 디스크 잠금을 잡을 수 없다).

리뷰 2회(1회 미충족 — 필수: 단언한 타입 조회가 TypeRegistry 종료로 실패함) 끝에 **충족**. 권장 넷(교체 뒤 실패 제거, 잠금 대조, hook 표 필수, 문구)도 반영했다.

A2c2b 상태(2026-11-21): **완료**. 완료 조건: 대조표(14.1.21) 시험 없는 항목 0·정의된 시험 16개 모두 인용, 조합(대조표 마지막 행), coverage(14.1.22 — Windows), 리뷰어 판정 **충족**.

#### 14.1.23 요구 대조표(A2c3a — 줄 출력·트랜잭션 만들기·참조 resolver·서비스 API)

시험은 FoundationTests `Core/Source/Runtime/ReflectionText/Tests/TextLinesTests.cpp`(7개)와 EngineTests `Engine/Source/Runtime/AssetRuntime/Tests/PackageGapTests.cpp`의 트랜잭션 판 2개, `Engine/Source/Developer/AssetDatabase/Tests/EditorPackageSaveTests.cpp`의 `EditorPackageSave_CreateInitializer`, `AssetTextResolverTests.cpp`(8개), `AssetServiceTests.cpp`(9개) — 정의된 시험 27개를 모두 인용한다(리뷰 반영으로 넷을 더했다). 줄 출력 시험의 대상 타입은 ReflectionTestFixtures(TextHolder·InstancedHolder·FixedOwner·BytesHolder·TextFieldHolder — 모든 ValueKind·컨테이너·Optional·InstancedStruct·Text)이고, 에셋 쪽은 AssetTestFixtures(TestAsset·TestPart·BaseGameAsset·DerivedGameAsset)와 A2c2 장면(Base·Derived·Hero·Villain·Leaf·Hub)이다. 서비스 장면은 저장 장면처럼 루트를 디스크 임시 디렉터리로 두어 잠금을 잡고 Content는 메모리다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 줄 출력(RE 10.4) | 모든 단일 값 종류(bool·정수 경계·실수 nan·Guid·AssetId·Digest·String escape·Name·enum·Flags·Soft·ClassRef)·목록·Set·Map(String·enum·Name→struct·ClassRef·Soft 키)·struct 멤버·struct 원소 배열·struct 값 Map·Optional set(단일·struct)·unset(`--unset=`)·null 참조 `none`의 줄, Edit의 Transient 제외·ReadOnly 출력, 줄을 같은 live 객체(ReadOnly 같은 값 무동작)·새 staged 객체에 넣어 다시 쓰면 같은 줄(값 보존 왕복), 기본 객체 왕복 | `TextLines_RoundTripsEveryKind` |
| | InstancedStruct(필드·배열 원소·정적 배열 원소·Map 값·struct 멤버·struct 원소 멤버·비어 있음 `none`), struct 원소 정적 배열(원소마다 멤버 줄, 비우기 줄 없음), 강한 참조 정적 배열(원소마다 `f[i]=` — 통째는 준비 단위가 없다), 바이트 배열(Base64, 빈 배열, struct 안·struct 원소·Optional·struct 값 Map)·바이트 정적 배열(목록), Invariant Text·빈 Text의 왕복 | `TextLines_InstancedStaticBytesAndText` |
| | 줄로 쓸 수 없는 값 Unsupported: 줄바꿈 든 String 값(`\n`·`\r`)·단일 값 Map의 String 키, Localized Text, resolver 없는 non-null Hard, resolver가 쓰지 못한 참조, 이름 없는 enum 값. 숨김 열거자는 이름으로 쓰고 재입력은 HiddenEnumerator. resolver가 쓰는 참조는 그 텍스트로 왕복 | `TextLines_UnsupportedValuesAndReferences` |
| | 용도(Edit — Transient·NonTryoutDuplicateTransient 제외, Report — Transient만 제외, TextExportTransient·DuplicateTransient는 둘 다 출력), struct 값 대상·staged 대상, 같은 Name pool 명시 = 기본, 잘못된 값 대상(저장소 없음·빈 lease·클래스 lease)·옵션 검사 실패·다른 Name pool InvalidArgument, 사라진 대상 오류 | `TextLines_UsesTargetsAndOptions` |
| | Opaque 필드(손으로 등록한 클래스)·도구 이름 없는 struct(손으로 등록 — RegisterStruct)를 담은 InstancedStruct·경로 단계 상한 초과(C++ Emplace로 struct 멤버에 깊이 16 사슬 — 깊이 15는 쓴다) Unsupported | `TextLines_OpaqueAndTooDeepAreUnsupported` |
| | FormatTextPath의 InstancedStruct 뒤 멤버 단계: 문맥 resolver 없음 InvalidArgument, 있음 `value.x`·`hosts[0].value.x`, 비어 있는 값 NotFound | `TextLines_FormatTextPathUsesContextInstancedTypes` |
| | 모든 줄 모양(기본 객체의 unset 줄 포함, 장면 넷)마다 값 allocator·출력 문자열의 모든 할당 실패 OutOfMemory, 끝나면 같은 출력 | `TextLines_AllocationFailures` |
| 트랜잭션 만들기(7.1-15 A2c3) | NewPackage(transaction): null id·모르는 종류 InvalidArgument(트랜잭션 고정 안 함), 대표 에셋 Id() = 패키지 id(공개 전·후), Commit root = Package, 공개된 Package·Package 아닌 staged 객체·두 번째 대표 자식·에셋 아닌 타입 거부, Rollback·소멸이 남기는 것 없음 | `AssetPackage_NewPackageInTransaction` |
| | NewPackage·NewRepresentativeAsset·Commit의 모든 할당 실패 OutOfMemory·남는 것 없음 | `AssetPackage_NewPackageInTransactionAllocationFailures` |
| 공개 전 초기화(7.1-15 A2c3) | 초기화는 공개 전 대표 에셋(Id 채움)에 한 번, 값·하위 객체가 공개됨, 초기화 중 다른 패키지 열기(참조 해석)에도 새 패키지가 집합에 들어감, 초기화 안의 Create·Save Busy(재진입 거부), 초기화 실패(값 쓴 뒤·하위 객체 만든 뒤 — NotFound·OutOfMemory) 진단 None·남는 것 없음(Registry 객체 수·집합 그대로), 오류를 삼킨 초기화는 Commit이 그 오류로 실패, 초기화 없는 Create도 Id = 패키지 id | `EditorPackageSave_CreateInitializer` |
| 참조 resolver(4.1·4.2·7.1-15) | Hard: 논리 경로(대소문자 무시)·AssetId가 대표 에셋을 열어(닫힘째) handle, 이미 연 패키지 그대로, 하위 객체·중첩 하위 객체(경로·AssetId), 기대 타입(0·자신·조상 받음, 아님 TypeMismatch), 없는 객체 NotFound, 저장하지 않은 새 패키지(경로·AssetId), 열기 경고(Hub)가 warnings에 | `AssetTextResolver_ResolvesHard` |
| | Weak(5.1절 — 대입 대상 패키지 안만): 범위 없음 InvalidArgument, 범위 패키지의 대표·하위·중첩(경로·AssetId)·기대 타입, 다른 패키지 InvalidArgument(열지 않음), 범위 패키지가 열려 있지 않음 NotFound | `AssetTextResolver_WeakStaysInPackage` |
| | TypeRegistry가 닫힌 뒤 기대 타입 있는 Hard ShuttingDown | `AssetTextResolver_TypeRegistryClosed` |
| | 형식 거부 InvalidArgument(Hard·Weak·Soft 셋 모두): `/` 없는 이름·대문자·35·37자·null AssetId·논리 경로 규칙(빈 구성 요소·끝 `/`·`\`)·빈 객체 경로·객체 경로 구성 요소 규칙(빈·공백) — 아무것도 열지 않음, 색인에 없는 경로 NotFound, 같은 경로 키 둘 AlreadyExists, 없는 id 열기 NotFound, 같은 id 둘 열기 AlreadyExists | `AssetTextResolver_RejectsText` |
| | Soft는 열지 않음: 경로 → id·객체 경로, AssetId(색인에 없어도) 그대로, 저장하지 않은 새 패키지 경로 | `AssetTextResolver_ResolvesSoftWithoutOpening` |
| | 출력: 대표 에셋·하위 객체·중첩 경로(Hard·Weak), 저장하지 않은 새 패키지 경로, Soft(색인 경로·객체 경로·새 패키지·색인에 없음·같은 id 둘 → AssetId), Package 객체 자신 Unsupported, 사라진 handle 오류 | `AssetTextResolver_Formats` |
| | 대입 묶음·줄 출력 왕복: Hard·typed·ObjectRefArray·Soft 하위 객체 대입 → 대상 패키지 열림 → 줄 출력이 같은 텍스트 → 재입력 받음, 기대 타입 위반 진단 Unresolved | `AssetTextResolver_RoundTripsThroughAssignments` |
| | resolver allocator·값 문자열·출력 버퍼의 모든 할당 실패 OutOfMemory(해석 셋·출력 셋 — 긴 이름 하위 객체 포함) | `AssetTextResolver_AllocationFailures` |
| 서비스 API(13, 7.1-15 A2c3) | asset-new: 대입(값·문자열·Hard·Soft)과 저장(파일·색인 항목·dirty 해제), 새 서비스가 같은 값, 파생 에셋 클래스 | `AssetService_NewCreatesAndSaves` |
| | asset-new 거부: 모르는 타입 NotFound·빈 이름 InvalidArgument(진단 None), 에셋 아닌 클래스·추상 에셋 클래스·struct TypeMismatch, 경로 규칙 InvalidPath, 있는 경로 DuplicatePath, 대입 오류 셋을 모두 모음(UnknownProperty·InvalidValue·Unresolved — 아무것도 안 만듦), 저장 실패(만든 패키지는 저장하지 않은 채 남음), 잡지 않은 잠금 | `AssetService_NewRefusals` |
| | asset-set: 경로·AssetId 대상, 빈 대입도 저장, force 없는 미해결 import MissingDependency·force면 저장 + DroppedUnresolvedImport, 대입 오류 모두 모음(파일 그대로), 대상 형식 InvalidPath(넷), 없는 경로·id NotFound, 잡지 않은 잠금 | `AssetService_SetAssignsAndSaves` |
| | asset-info: Edit 줄(참조는 논리 경로·색인에 없는 Soft는 AssetId)·Report 줄·JSON·AssetId 대상, 줄로 쓸 수 없는 값 Unsupported(JSON은 씀), 대상 거부, 호출자 문자열 할당 실패 | `AssetService_InfoFormats` |
| | asset-list: 경로 키 순 전체, --type 파생 포함(TestAsset·BaseGameAsset·DerivedGameAsset·추상 Asset·에셋 아닌 클래스), 모르는 이름·struct 거부, --under(새 디렉터리·없는 디렉터리·논리 디렉터리 아님), 타입을 등록하지 않은 Registry의 서비스, 호출자 목록 할당 실패 | `AssetService_ListFilters` |
| | Weak 대입: asset-set의 같은 패키지 대상은 저장·다시 읽기 그대로, 다른 패키지 Unresolved, asset-new의 Weak Unresolved(한계) | `AssetService_WeakReferences` |
| | 대표 에셋을 Discard한 열린 패키지의 asset-info·asset-set NotFound(진단 None·패키지 id), TypeRegistry가 닫힌 뒤 asset-list --type ShuttingDown | `AssetService_DiscardedRepresentativeAndClosedTypes` |
| | 수명: Initialize 전 모든 함수 InvalidArgument, 두 번째 Initialize, 타입·파일 시스템 없는 설정, 파일 캐시 용량 0, Content가 파일(색인 Rebuild 실패) | `AssetService_Lifecycle` |
| | New(대입·참조·ObjectRefArray — 초기화가 참조 대상을 연다)·Set·Info(줄·JSON)·List의 서비스 allocator 할당을 하나씩 실패: OutOfMemory 또는 대체 경로로 성공 — 이 시험이 Create의 표 자리를 초기화 앞에 잡던 결함을 찾았다 | `AssetService_AllocationFailures` |
| 조합(14 완료 조건 2) | 줄 출력: ValueKind 전부 × 자리(필드·struct 멤버·배열 원소·정적 배열 원소·Map 값·Optional) × 값 모양(기본·set·unset·null·비어 있음), 대상 종류(Live·Staged·Value) × 용도(Edit·Report). 참조: 텍스트 모양(경로·AssetId × 대표·하위·중첩) × 종류(Hard·Weak·Soft) × 패키지 상태(색인만·열림·저장하지 않음·없음·같은 경로 둘·같은 id 둘) × Weak 범위(없음·같은 패키지·다른 패키지·열리지 않음). 서비스: 명령 넷 × 대상 모양(경로·AssetId) × 거부 종류 × 할당 실패 위치 전부 | 위 시험 |

#### 14.1.24 Coverage(A2c3a)

측정(2026-11-21, Windows): 줄 출력은 `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target Foundation -Executable FoundationTests -Filter TextLines -Sources Core/Source/Runtime/ReflectionText/Private`, 에셋 쪽은 `-Target EngineTests -Filter AssetService_,EditorPackageSave_,EditorPackageSet_,AssetTextResolver -Sources Engine/Source/Developer/AssetDatabase/Private`와 `-Filter AssetPackage_,Package_AttachBulkStore,EditorPackageSave_ -Sources Engine/Source/Runtime/AssetRuntime/Private`.

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `TextLineExport.cpp`(줄 출력) | 97.7%(302/309) | 96.5%(164/170) | 100%(10/10) |
| `AssetTextResolver.cpp` | 97.8%(174/178) | 97.8%(90/92) | 100%(8/8) |
| `AssetService.cpp` | 99.5%(221/222) | 97.4%(111/114) | 100%(12/12) |
| `EditorPackageSet.cpp`(만들기 초기화·재진입 거부 포함) | 99.2%(950/958) | 98.6%(433/439) | 100%(37/37) |
| `Package.cpp`(트랜잭션 판) | 100%(69/69) | 92.7%(38/41) | 100%(6/6) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `TextLineExport.cpp` 경로 연산 넷(`PathReadInstancedType` 두 곳·`PathIsOptionalSet`·`PathCount`) | 실패 | 경로는 방금 스키마와 그 자리의 값에서 만든 소유 스레드의 유효 경로이고(대상 lease를 쥐고 있다) 이 연산들은 할당하지 않는다 — 할당 실패 시험이 닿지 않음을 확인했다. JSON 출력도 같은 판정이다 |
| `TextLineExport.cpp` 대상 종류 switch의 암묵 default, 값 대상의 null lease 포인터 | 분기 | AssignmentTarget 종류는 셋뿐이고 Value 대상은 늘 lease 참조로 만든다 |
| `AssetTextResolver::Format`의 outermost Package 해석·포인터 실패 | 실패 | 방금 GetOutermost가 준(살아 있는) outermost다. 집합 Registry는 패키지 정책이라 outermost는 늘 Package다(ObjectIdentity.h RegistryPolicy). (처음 근거에 넣었던 Resolve의 타입 조회 실패와 OpenTarget의 대표 에셋 없음은 리뷰가 도달 가능함을 짚어 시험을 더했다 — TypeRegistry 닫힘 ShuttingDown, 호출자의 대표 에셋 Discard) |
| `AssetService::List`의 IsA 그 밖 오류(NotFound·TypeMismatch가 아닌 것) | 실패 | `--type` 이름을 같은 호출에서 방금 TypeRegistry로 찾았고(닫혔으면 그 조회가 ShuttingDown — 시험) 색인 항목 타입은 클래스 레코드나 미등록(NotFound)이다. 소유 스레드 호출이라 그 사이 닫히지 않는다 |
| `EditorPackageSet.cpp` | 14.1.22와 같은 지점(SetBulkStore 계약·OS 난수·llvm-cov 줄 귀속) | 14.1.22와 같다 — 만들기의 표 자리 잡기는 초기화 뒤로 옮겼고 그 실패는 서비스 할당 실패 시험이 닿는다 |
| `Package.cpp` 분기 셋 | `ROCKS_ASSERT` 전개 | 방금 만든 이 트랜잭션 객체의 GetForWrite(Required 검사 중이면 앞 Create가 먼저 Busy)·쥔 root가 있는 공개 Package의 typed root |

리뷰(1회 미충족)가 찾은 것: Weak 참조를 Hard처럼 다른 패키지에서 풀어 저장 뒤 null이 되던 것(같은 패키지 안만 받게 고침), asset-list --type이 파생 판정 오류를 삼키던 것, 미도달 근거 둘이 사실과 다르던 것(시험 추가), Create 초기화 안의 재진입(거부), asset-new 자기 참조의 한계 기록. 시험이 찾은 것: (1) 줄 출력의 경로 표기가 InstancedStruct 뒤 멤버를 풀지 못했다(`FormatTextPath`가 문맥의 InstancedStruct resolver를 넘기지 않았다) — 넘기게 고치고 줄 출력이 대상 값 resolver를 건다. (2) 강한 참조 정적 배열 통째 줄은 대입 묶음이 받지 않는다(준비 단위 없음) — 원소 줄로 바꿨다(10.4절). (3) 줄·`.` 한 글자 붙이기가 앞 붙이기가 늘린 용량 때문에 할당 실패에 닿지 않았다 — 자리를 한 번에 잡게 고쳤다. (4) **만들기가 표 자리를 초기화 앞에 잡았는데 초기화(참조 해석)가 다른 패키지를 열며 그 자리를 써, 할당 실패 때 공개 뒤 삽입이 조용히 실패해 새 패키지가 집합에서 빠졌다**(뒤 Save가 NotFound) — 자리를 초기화 뒤·Commit 앞에 잡는다. (5) 새 패키지의 대표 에셋 `Id()`가 로드 전까지 null이었다 — 트랜잭션 판 대표 에셋 만들기가 채운다.

A2c3a 상태(2026-11-21): **완료**. 완료 조건: 대조표(14.1.23) 시험 없는 항목 0·정의된 시험 27개 모두 인용, 조합(대조표 마지막 행), coverage(14.1.24 — Windows), 리뷰어 판정 **충족**(1회 미충족 — 필수 3 반영). 다음은 A2c3b(Commandlet 명령·타입 등록·C# 시험).

#### 14.1.25 요구 대조표(A2c3b — Commandlet 명령)

시험은 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 A2c3b 항목 95개(Windows 전용 16개 포함 — 잠금·고아·무효 출력 핸들과 그 장면)와 바꾼 A2b 항목 하나(`asset-list unknown type` — `--type`이 사용법 오류에서 옵션이 됐다)다. 명령은 실제 에셋 타입 Skeleton·AnimationClip(Animation 모듈 — Commandlet이 시작해 등록한다)으로 디스크 임시 프로젝트에서 돈다. 서비스 수준의 전수 시험(조합·할당 실패)은 14.1.23의 C++ 시험이다 — 명령 시험은 인수·출력·종료 코드·타입 등록·잠금·실제 파일을 본다.

| 요구(절) | 항목 | 시험(C# 라벨) |
|---|---|---|
| asset-new(13) | Skeleton 만들기(출력 `<경로> <AssetId>` 한 줄·파일 생김), Clip 대입(Hard·float·bool·enum·struct 멤버·struct 원소 배열·Name), 거부(모르는 타입 NotFound·추상 TypeMismatch·에셋 아닌 클래스·struct·있는 경로 대소문자 무시 DuplicatePath·경로 규칙 InvalidPath·대입 오류 모두 보고와 아무것도 안 만듦·Hero 없는 참조 Unresolved), 열기 경고와 성공(ExcludedDependency) | `asset-new skeleton`·`asset-new output shape`·`asset-new clip with assignments`·`asset-new unknown type`·`asset-new abstract type`·`asset-new not an asset`·`asset-new struct type`·`asset-new existing path`·`asset-new invalid path`·`asset-new assignment errors`·`asset-new assignment errors create nothing`·`asset-new clip without skeleton`·`asset-new with open warnings`·`asset-new skeleton again`·`asset-new clip again` |
| asset-info(13) | Edit 줄(참조는 논리 경로)·AssetId 대상 같은 출력·JSON(줄 끝)·--all·--json --all, 거부(없는 경로·없는 id NotFound·대상 형식 InvalidPath), 잠금 없이 읽기, 프로젝트 찾기(위로)·없음, 열기 경고 + 성공 | `asset-info lines`·`asset-info by AssetId`·`asset-info json`·`asset-info all`·`asset-info json all`·`asset-info missing`·`asset-info missing id`·`asset-info invalid target`·`asset-info without lock`·`asset-info finds the project upward`·`asset-info without a project`·`asset-info excluded dependency` |
| 왕복(RE 10.4) | asset-info 줄 전체를 asset-set에 그대로 넣으면 받고 출력이 같다 | `asset-info lines round trip through asset-set` |
| asset-set(13) | AssetId 대상, Soft 참조 배열·bool, --erase(원소·따옴표 key의 쉼표·escape가 든 목록), 대입 오류 넷 모두 보고(UnknownProperty·InvalidValue·Unresolved·--unset 비 Optional)·파일 byte 그대로, Soft 미해결, 미해결 import MissingDependency·--force + DroppedUnresolvedImport·null 저장 | `asset-set by AssetId`·`asset-set result`·`asset-set by path`·`asset-set by path result`·`asset-set soft array`·`asset-set soft array result`·`asset-set erase`·`asset-set erase result`·`asset-set quoted keys`·`asset-set quoted keys result`·`asset-set erase quoted list`·`asset-set erase quoted list result`·`asset-set errors`·`asset-set errors leave the file`·`asset-set soft unresolved`·`asset-set missing dependency`·`asset-set force`·`asset-set force result` |
| asset-list --type(13, 7.1-14) | 그 클래스만(한 줄)·파생(AnimationAsset → Clip)·추상 Asset 전체(경로 순)·--under와 함께·빈 결과·논리 디렉터리 아님·클래스 아님·같은 옵션 두 번 사용법 오류·모르는 타입(A2b 프로젝트)·프로젝트 없음 | `asset-list type`·`asset-list type lines`·`asset-list derived type`·`asset-list base type`·`asset-list base type order`·`asset-list type under`·`asset-list type under nothing`·`asset-list type invalid under`·`asset-list not a class`·`asset-list type twice`·`asset-list unknown type`·`asset-list type without a project` |
| 사용법(13) | 위치 인수 수·대입 아닌 위치 인수·명령에 없는 옵션(--force·--json·--unset)·빈 목록·빈 목록 항목·같은 옵션 두 번(--project·--force·--json)·--wait 범위 초과·형식·빈 값·빈 타입 이름·asset-set 대입 없음·--help에 다른 인수, 명령마다 --help(0) | `usage error: …`(20개)·`asset-new help`·`asset-set help`·`asset-info help` |
| 잠금·환경(8, 13) | 다른 쪽이 잡은 잠금: asset-set 바로 실패·asset-new --wait=1 뒤 실패(Busy), Intermediate가 파일이면 잠금 오류, Content가 파일이면 cannot scan(asset-info·asset-list --type), 지울 수 없는 고아 임시 파일 OrphanNotDeleted(저장 성공)·다음 저장이 지움 | `asset-set locked`·`asset-new locked with wait`·`asset-new cannot lock`·`asset-info content is a file`·`asset-list type content is a file`·`asset-set orphan not deleted`·`asset-set orphan deleted`·`orphan removed` |
| 출력 실패(13) | 무효 출력 핸들: asset-info(경고 있음 둘)·asset-list --type·asset-new(출력 줄·경고)·asset-set(대입 진단·경고) 모두 1 | `invalid output handles (… edit)`(7개) |
| 조합(14 완료 조건 2) | 명령 넷 × 대상 모양(경로·AssetId) × 결과(성공·대입 오류·대상 거부·열기 경고·저장 거부) × 출력(줄·JSON·all), 목록 옵션 × 경로 모양(원소·따옴표 key) | 위 시험 |

#### 14.1.26 Coverage(A2c3b)

측정(2026-11-21, Windows): `pwsh -NoProfile -File Build/MeasureCoverage.ps1 -Target Commandlet -Executable RocksCommandlet -Runner 'dotnet','out/dotnet/bin/Rocks.Commandlet.Tests/Debug/net10.0/Rocks.Commandlet.Tests.dll','--executable','{executable}' -Sources Programs/Commandlet/Private/Common`. 명령 시험은 실행 한 번마다 원시 프로파일을 남겨 수백 개가 되므로 측정 스크립트가 `llvm-profdata merge`에 목록 파일(`--input-files`)을 넘기게 고쳤다(명령줄 길이 초과).

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetEditCommandlet.cpp` | 96.1%(292/304) | 91.4%(159/174) | 100%(12/12) |
| `AssetIndexCommandlet.cpp`(--type 포함) | 90.2%(148/164) | 88.0%(81/92) | 100%(5/5) |
| `AssetCommandletSupport.cpp`(옮긴 프로젝트 찾기·타입 등록·실행 상태) | 79.7%(110/138) | 72.4%(42/58) | 100%(7/7) |
| `AssetCommandletSupport.h`(`Fail` 템플릿) | 76.9%(20/26) | 62.5%(5/8) | 100%(5/5) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| 세 파일·헤더 | 할당 실패(출력 줄·`Fail`·JSON 줄 끝·서비스 만들기·`Initialize`의 OutOfMemory 분기·목록·Verify) | 프로세스 시험은 명령의 allocator에 실패를 주입할 수 없다(14.1.16과 같다). 같은 결과를 만드는 라이브러리 경로는 14.1.23의 할당 실패 시험(`AssetService_AllocationFailures` 등)이 모든 지점을 돈다 |
| `AssetCommandletSupport.cpp` `OpenProject` | 현재 디렉터리를 절대 경로로 못 만듦·프로젝트 찾기의 NotFound 밖 오류 | 14.1.16과 같다(OS 조건 — 이 커밋에서 동작 그대로 옮겼다) |
| `AssetCommandTypes::Start` | 모듈 시작 실패·등록기 붙이기 실패·Name pool 시작 실패와 그 출력(`cannot register the asset types`) | Animation은 실행 파일에 정적으로 링크된 모듈이라 표에 늘 있고 시작 콜백이 실패하지 않는다. 등록기 붙이기의 실패는 등록 할당 실패(위와 같다)와 Fatal 조건(같은 등록기 두 번·Load 중 — 명령 구조상 없다)이고, pool 시작 실패는 할당 실패다 |
| `AssetEditCommandlet.cpp` 경고 이름 NotDurable·switch 밖 반환 | 분기 | NotDurable은 교체 뒤 디렉터리 동기화 실패(OS 조건 — 서비스 시험이 메모리 파일 시스템으로 단언한다), 반환은 열거형 밖 값이 없다 |
| `AssetEditCommandlet.cpp` asset-info의 Unsupported 줄 | 줄로 쓸 수 없는 값 | Animation 타입에는 줄바꿈을 담을 수 있는 값(String)이 없다 — 서비스 시험 `AssetService_InfoFormats`가 Unsupported를 단언하고 출력 문구는 이 한 줄이다 |
| `AssetIndexCommandlet.cpp` --type 실패 설명 `failed` | 분기 | `AssetService::List`의 실패는 이름 찾기 NotFound·클래스 아님 TypeMismatch·디렉터리 InvalidPath·할당 실패이고, 그 밖(파생 판정의 ShuttingDown 등)은 14.1.24의 근거대로 이 호출에서 생기지 않는다 |

리뷰(1회 미충족)가 찾은 것: 쓰기 명령이 색인을 만든 뒤에 잠갔다(잠금 아래에서 다시 보지 않은 색인으로 쓰기 판정), asset-new의 "NotFound + 진단 None = 모르는 타입" 판정이 저장 단계의 NotFound(임시 파일·교체)와 섞였다(타입을 먼저 찾게 고침), 경고 줄이 스펙 형식과 달랐다(형식을 맞추고 환경 실패·일반 실패 줄도 같은 형식·관련 대상 포함으로 다듬음). 권장으로 path 대상 asset-set 성공을 따로 단언했다. 읽기 전용(더 새 엔진) 대상 장면은 명령 시험이 Animation 패키지를 더 새 엔진 버전으로 만들 수 없어(헤더 버전을 바꾸면 무결성 검사에 걸린다) 서비스·집합 시험(`EditorPackageSave_Refusals` NewerVersion)으로 둔다.

A2c3b 상태(2026-11-21): **완료**. 완료 조건: 대조표(14.1.25) C# 항목 95개 모두 인용, 조합(대조표 마지막 행), coverage(14.1.26 — Windows), 리뷰어 판정 **충족**(1회 미충족 — 필수 3 반영). 이로써 A2c3(명령) 완료 — 다음은 A2d.

#### 14.1.27 요구 대조표(A2d1 — 색인 캐시·asset-index-rebuild)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetIndexCacheTests.cpp`(9개)와 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-index-rebuild·색인 캐시" 항목 23개(Windows 전용 3개)다. C++ 장면은 루트를 디스크 임시 디렉터리(잠금)로 두고 그 루트에 놓인 메모리 파일 시스템에 Content(Base·Derived·Hero·Villain·Sub/Leaf·Copy(같은 id)·derived.RASSET(같은 경로 키)·Tagged(태그·ImportRecord)·BadTags(Tags 진단)·Junk(읽기 실패))와 Intermediate를 둔다. 손상 입력은 시험 쪽에서 형식을 따로 써서 만든다(구현과 독립).

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 왕복·재사용(7, 7.1-16) | 캐시 없는 검색 → 캐시 쓰기(임시 파일 없음) → 읽기: 파일 자신의 진단이 있는 BadTags·Junk는 없고 같은 id·같은 경로 진단의 파일은 있다, 항목 칸이 색인과 같고 이름·지문이 다르면 찾지 못함, 캐시를 쓴 검색 = 캐시 없는 검색(진단 포함), 도구 빌드 ID 기록, 지문이 같으면 다시 읽지 않음(캐시의 태그 값을 바꾸면 그 값), 지문이 다르면 다시 읽음 | `AssetIndexCache_RoundTripsAndReuses` |
| 키(7, 7.1-16) | 패치만 다르면 같은 키, 주·부·customVersions(값·개수) 다르면 다른 키, Unicode 텍스트 SHA-256, 빈 엔진 버전(엔진 축 끔)은 켠 실행과 다른 키·패치와 상관없이 같은 키, 키 칸 여덟(형식·스키마·의미·Unicode·주·부·customVersions·정규화) 각각 다르면 캐시 없음(양방향) | `AssetIndexCache_KeyAxes` |
| 신뢰하지 않는 입력(7.1-16) | 헤더 위반(짧음·magic·예약·정규화 2·본문 byte·SHA-256·항목 수 상한·적은 항목 수·크기 상한), 항목 위반 19종(이름 UTF-8·빈 이름·.rasset 아님·이름 길이 경계·readOnly 2·의존 수 경계·종류 0·6·required 2·태그 수 경계·빈 키·태그 순서·같은 태그·요약 표지 2·요약 이름 UTF-8·null id·0 타입·태그 키 257 byte·태그 값 4097 byte), 같은 이름·역순·남은 byte·모든 길이의 잘림·항목 수 초과 — 모두 비운 채 캐시 없음, 경계 값의 올바른 항목(태그 키 256 byte 포함)은 받음 | `AssetIndexCache_RejectsUntrustedInput` |
| 재사용 가장자리(7.1-16) | 빈 캐시, 열 때 사라진 파일·지문을 읽을 수 없는 파일(부분 읽기가 같은 진단), 규칙을 어긴 캐시 이름(InvalidPath — 쓰지 않음), 조작한 값(readOnly·required 의존)이 색인과 다시 쓴 캐시에 남음, 캐시를 쓰는 검색의 모든 할당 실패 | `AssetIndexCache_ReuseEdges` |
| 읽기(7.1-16) | 없음·디렉터리·크기 0·열기 없음·사용 중·읽기 실패는 캐시 없음, 모든 할당 실패 OutOfMemory | `AssetIndexCache_LoadFailures` |
| 쓰기(7, 8, 7.1-16) | 잠금 아님 InvalidArgument(아무것도 안 씀), 디렉터리 만들기·만들기·쓰기·Flush·Close·교체 실패는 오류·이전 캐시 그대로·임시 파일 없음, NotDurable 성공, 모든 할당 실패(임시 파일 없음) | `AssetIndexCache_WriteFailures` |
| TypeRegistry 무시(7.1-16) | options.types가 있어도 같은 색인 | `AssetIndexCache_IndexIgnoresTypes` |
| 고아 정리(7.1-12·16) | Content(하위 포함)의 `.rasset.<guid>.tmp`·Intermediate 바로 아래 캐시 임시만 지움, 남김 9종(대문자 guid·다른 확장자·빈 이름·Content의 캐시 임시·대문자 .TMP·다른 캐시 이름·Intermediate 하위·잠금 파일·잘못된 UTF-8 이름)과 링크·디렉터리·기타 종류, 이미 사라진 파일은 지운 것으로 셈, 지울 수 없음·열거할 수 없는 디렉터리는 실패 목록(계속), 잠금 아님·Content가 파일·없는 디렉터리, 모든 할당 실패(실패 기록·하위 디렉터리 여럿 장면) | `AssetIndexCache_DeletesTemporaryOrphans` |
| 서비스(7.1-16) | 캐시를 읽음(바꾼 값), useCache 끔, 캐시 파일 시스템 따로, FlushCache(Initialize 전·잠금 아님 InvalidArgument·성공 뒤 다시 읽힘), 캐시를 읽는 Initialize의 모든 할당 실패 | `AssetIndexCache_ServiceUsesCache` |
| 명령(13, 7.1-16) | asset-new가 캐시를 씀, asset-list가 캐시로 같은 목록, asset-index-rebuild(요약·고아 둘 지움·다른 이름 남김·캐시 씀), 진단이 있어도 0, 손상 캐시를 목록·점검·출력이 무시하고 asset-set이 다시 씀, 캐시를 쓰지 못하면 asset-set·asset-new 경고 CacheNotWritten(0)·rebuild 실패 1, 지울 수 없는 고아 경고·잠금 Busy·무효 출력 핸들(Windows), 사용법 다섯·도움말·프로젝트 없음·Content가 파일·잠금 실패 | C# `asset-new writes the index cache` … `asset-index-rebuild cannot lock`(23개) |
| 조합(14 완료 조건 2) | 캐시 상태(없음·맞음·키 다름·손상·빈 것·조작) × 파일 상태(같은 지문·다른 지문·사라짐·손상·이름 위반) × 진단 종류(파일 자신·같은 id·같은 경로), 쓰기 고장 위치 전부, 명령 × 캐시 사용(읽기·쓰기·무시) | 위 시험 |

#### 14.1.28 Coverage(A2d1)

측정(2026-11-21, Windows): `-Target EngineTests -Filter AssetIndexCache,AssetIndex_,AssetService_ -Sources Engine/Source/Developer/AssetDatabase/Private`, 명령은 `ROCKS_MSVC_TOOLSET=14.44`로 `-Target Commandlet -Executable RocksCommandlet -Runner …C# 시험… -Sources Programs/Commandlet/Private/Common`(VS 2026 설치로 기본 MSVC가 14.52가 되어 Clang 21로는 STL1000 — 측정만 14.44로 고정했다).

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetIndexCache.cpp` | 98.9%(466/471) | 98.1%(204/208) | 100%(21/21) |
| `TemporaryFiles.cpp` | 100%(128/128) | 100%(66/66) | 100%(7/7) |
| `AssetIndex.cpp`(캐시 재사용 포함) | 100%(890/890) | 99.4%(462/465) | 100%(35/35) |
| `AssetService.cpp`(캐시 적재·FlushCache) | 99.2%(239/241) | 97.6%(121/124) | 100%(13/13) |
| `Programs/Commandlet/Private/Common/AssetIndexCommandlet.cpp`(rebuild·목록 캐시) | 85.4%(211/247) | 84.1%(111/132) | 100%(6/6) |
| `Programs/Commandlet/Private/Common/AssetEditCommandlet.cpp`(캐시 쓰기 경고) | 94.4%(303/321) | 90.1%(164/182) | 100%(13/13) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `AssetIndexCache::Load` 크기 조회 실패·1 GiB 초과 | 분기 | 파일 크기 조회는 연 파일에서 실패하지 않고(메모리·물리 모두), 1 GiB 넘는 캐시는 시험이 만들 수 없다. 같은 상한은 `Parse`가 단언한다(크기 상한 시험) |
| `EncodeAssetIndexCache` 정규화 플래그 참 쪽 | 분기 | `FileSystemNormalizesNames`는 플랫폼 상수(macOS만 참) — Windows 측정 밖이다. 읽기 쪽은 키 칸 시험이 두 값을 모두 본다 |
| `WriteAssetIndexCache` 임시 이름 guid 발급 실패 | 실패 | OS 보안 난수 실패(14.1.22와 같다) |
| `AssetIndex::IndexCached` 논리 경로 만들기의 할당 실패 | 분기 | 배치(ProjectLayout)의 allocator로 만든다 — 색인 allocator 할당 실패 시험 밖이다(IndexFile의 같은 호출과 같다) |
| `AssetService::List` IsA 그 밖 오류 | 실패 | 14.1.24와 같다 |
| 명령 파일들 | 할당 실패(출력 줄·목록·검색·Verify·정리), rebuild의 정리 오류(할당 밖) | 프로세스 시험은 명령 allocator에 실패를 주입할 수 없다(14.1.26과 같다 — 같은 경로는 C++ 할당 실패 시험이 돈다). 정리 오류는 Content·Intermediate 뿌리의 열거 실패뿐인데 그 둘은 앞 단계(검색 — cannot scan, 잠금 — cannot lock)가 먼저 실패시킨다(C# 시험) |

시험이 찾은 것: (1) 잘린 항목의 몇 단계(의존 읽기)는 앞의 개수 상한 검사 때문에 닿을 수 없었다 — 그 `reader.Failed()` 검사를 지우고 근거를 적었다. 헤더 읽기의 실패 검사도 크기 검사 뒤라 지웠다. (2) 고아 정리가 템플릿이라 두 인스턴스의 분기가 따로 셌다 — 판정 함수 포인터로 바꿨다. (3) 경로 버퍼(inline StringBuilder)는 짧은 경로에서 할당하지 않아 할당 실패에 닿지 않았다 — 할당하는 String으로 바꿨다. (4) Windows는 대소문자를 구분하지 않아 대문자 guid 파일과 소문자 고아가 같은 파일이었다(C# 시험 이름을 바꿨다).

#### 14.1.29 요구 대조표(A2d3 — 부분 갱신·감시)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetIndexWatchTests.cpp`(12개)다. 장면은 루트를 디스크 임시 디렉터리로 두고 그 루트에 놓인 메모리 파일 시스템에 Content(Base·Hero·Sub/Leaf)를 둔다. 결과는 늘 지금 파일 상태의 새 검색(`Rebuild`)과 항목·진단의 모든 칸이 같은지(`AssetIndexCompare.h`의 `SameIndex` — 캐시 시험과 공용)로 단언한다. 감시 시험은 가짜 원천(`FakeChangeSource` — 이벤트·열기·Poll 오류·호출 수)과 수동 시계로 결정적이다. 실제 감시기는 한 번 돈다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 파일 갱신(7.1-16 A2d3) | 내용 바뀜·새 파일·없어짐·디렉터리가 됨·같은 이름 여러 번(한 번 읽음 — 열기 수)·바뀌지 않은 이름, 같은 id 진단이 생기고 사라짐, 빈 입력, 읽기·Stat IO 진단, Refresh도 TypeRegistry를 쓰지 않음 | `AssetIndexRefreshPaths_FilesMatchRebuild` |
| 하위 트리(7.1-16 A2d3) | 새 중첩 디렉터리·없어진 디렉터리·디렉터리가 파일이 됨·이름이 패키지 파일·링크·없는 이름·열거 실패·Stat 실패(진단, 다시 읽으면 빠짐)·트리 안 트리·트리 안 파일(한 번), 잘못된 UTF-8 이름 아래 진단(유효한 비ASCII 디렉터리 아래 — escape가 다른 표시)과 유효한 디렉터리 아래 둘 다 빠짐, Content 전체(빈 이름 — 다른 트리·파일을 덮음), Content 열거 실패는 오류·색인 그대로 | `AssetIndexRefreshPaths_Subtrees` |
| 입력 검사(7.1-16 A2d3) | 파일 11종(빈 이름·앞뒤 `/`·빈 구성 요소·`.`·`..`·중간 `.`·`..`·확장자 아님·확장자뿐·잘못된 UTF-8)·트리 7종, mayDefer 길이 불일치·deferred 없음 — InvalidArgument·색인 그대로 | `AssetIndexRefreshPaths_RejectsInvalidInput` |
| 쓰는 중 미루기(7.1-16 A2d3) | 손상·Busy·새 파일 손상 미룸(같은 이름 위치 모두, 색인 그대로), 같은 이름 중 거짓 하나면 안 미룸, 트리가 덮으면 안 미룸, 읽기 IO 실패·Tags 손상(항목 있음)·InvalidPath·없는 파일은 안 미룸, 완성되면 바꿈 | `AssetIndexRefreshPaths_Defers` |
| 정규화 대조(7.1-16 A2d3) | 끄면 NFD 이름이 다른 이름, 켜면 NFD 파일·트리가 NFC 항목·진단을 가리킴, 같은 키(NFC·NFD)는 한 번 읽음 | `AssetIndexRefreshPaths_NormalizesNames` |
| 할당 실패(7.1-16 A2d3) | 파일·트리·미루기·정규화를 섞은 갱신의 모든 할당 실패 OutOfMemory·색인 그대로, 끝 상태는 실패 없는 기준과 같음, MarkRootLost 할당 실패 | `AssetIndexRefreshPaths_AllocationFailures` |
| RootLost(4.6, 7.1-16 A2d3) | 항목 비움·진단 하나(빈 이름·NotFound)·조회 NotFound·진단 줄·코드 이름·Layout | `AssetIndex_MarkRootLost` |
| 시작·정지(7.1-16 A2d3) | Start 전 Pump·두 번 Start InvalidArgument, 원천 열기 실패(시작 안 함), Rebuild 실패(원천 닫음), 캐시 사용, Content 경로로 열기, 이벤트 없는 Pump, Stop 두 번, 소멸자가 닫음 | `AssetIndexWatch_StartsAndStops` |
| 이벤트 대응(7.1-16 A2d3) | Added·Removed·Modified × (`.rasset` 파일·모름 → 파일, `.rasset` 디렉터리 → 트리만, 그 밖 디렉터리·모름 → 트리, 그 밖 파일 → 무시), Renamed 파일·디렉터리 옛·새 이름 | `AssetIndexWatch_MapsEvents` |
| 다시 검색 범위(7.1-16 A2d3) | Overflow(경로)는 그 트리만, 전체로 접힘 다섯(Overflow 빈 경로·원천 Poll 오류·앞 `/`·빈 구성 요소·첫 구성 요소 잘못된 UTF-8), 전체가 잡힌 뒤 경로는 더하지 않음, 중간 잘못된 UTF-8은 그 앞 디렉터리 | `AssetIndexWatch_RescansOnOverflow` |
| 루트 소실(7.1-16 A2d3) | RootRemoved·같은 Poll 뒤 이벤트 버림, MarkRootLost 할당 실패는 다음 Pump가 Poll 없이, 1초 전 안 엶, NotFound 조용히, 그 밖 원천 오류는 Pump 오류, 시계 역행은 다시 1초, 다시 검색 실패는 RootLost 그대로·원천 열린 채·다음 간격에 검색만, 성공하면 새 검색과 같음, 다시 감시 | `AssetIndexWatch_RecoversLostRoot` |
| 쓰는 중 대기(7.1-16 A2d3) | 같은 파일 이벤트 둘은 대기 하나, 499 ms 아직·500 ms 다시 읽어 손상 남김, 크기 바뀜은 관찰 재시작·완성 이벤트는 대기 뺌, 계속 바뀌어도 10초 상한(새 이벤트가 처음 본 시각을 바꾸지 않음), Busy, 사라짐·디렉터리가 됨·링크가 됨은 바로 다시 읽음, 시각만 바뀜도 관찰 재시작, 트리 덮음(같은 이름·그 아래는 빼고 접두만 같은 이름·트리보다 짧은 이름은 남김), 시계 역행, 전체 다시 검색이 덮음, 정지가 대기·모은 일을 버림 | `AssetIndexWatch_WaitsForWritingFiles` |
| 감시 할당 실패(7.1-16 A2d3) | 대기 관찰(복사본)·이벤트·트리·Renamed·새 대기·정렬 대조 표를 섞은 Pump의 모든 할당 실패: 실패면 색인·대기 그대로·다음 Pump가 기준과 같게, 이벤트 기록 실패는 전체로 접혀 성공(새 검색과 같음), 전체 다시 검색의 할당 실패(전체 대기 유지 → 다음 Pump) | `AssetIndexWatch_AllocationFailures` |
| 실제 감시기(7.1-16 A2d3) | 디스크에 쓰기·고쳐 쓰기·지우기가 이벤트로 색인에 반영(Wait는 이벤트 gate, 횟수 상한은 실패 판정용), 없는 디렉터리 열기 NotFound | `AssetIndexWatch_RealWatcher` |
| 조합(14 완료 조건 2) | 이벤트 종류 × 항목 종류 × 이름 종류, 다시 볼 곳(파일·트리·전체) × 겹침(중복·덮음·트리 안 트리), 파일 상태(새·바뀜·없어짐·디렉터리·링크·손상·Busy·IO 실패·이름 위반) × 미루기(참·거짓·섞임), 대기 상태(새·관찰·안정·바뀜·상한·사라짐·덮임) × 시계(앞·역행), 루트(있음·소실·재열기 실패 종류·검색 실패) | 위 시험 |

#### 14.1.30 Coverage(A2d3)

측정(2026-11-21, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetIndexRefreshPaths,AssetIndex_,AssetIndexWatch,AssetIndexCache,AssetService_,EditorPackage -Sources Engine/Source/Developer/AssetDatabase/Private`.

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetIndexWatch.cpp`(리뷰 반영 — 정렬 대조·복사본 관찰) | 100%(406/406) | 99.5%(191/192) | 100%(24/24) |
| `AssetIndex.cpp`(부분 갱신·RootLost 포함) | 100%(1186/1186) | 99.5%(628/631) | 100%(46/46) |
| `AssetDatabaseInternal.h`(이름 규칙·`NameSet` — 색인·감시 공용) | 100%(133/133) | 100%(84/84) | 100%(14/14) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `PlatformAssetChangeSource::Poll` 실패 | 분기 | OS 감시 오류(`ReadDirectoryChangesExW` 실패 등)라 시험이 만들 수 없다. 감시의 Poll 오류 처리(전체 다시 검색)는 가짜 원천으로 시험한다 |
| `AssetIndex.cpp` 분기 3개 | 14.1.22·14.1.28과 같다 | `ROCKS_ASSERT` 전개 2개(확정의 준비 상태·세대 대조), `IndexCached` 논리 경로의 배치 allocator 할당 실패 |

리뷰가 찾은 것: 감시의 대조(다시 읽은 이름·대기·덮은 트리)가 이중 반복이었다(이벤트·대기가 수만 개면 Pump 한 번에 수십억 비교) — 정렬 표의 이진 검색으로 바꿨다. 관찰을 대기 그 자리에서 바꿔 실패해도 대기가 바뀌었다 — 복사본에서 하고 성공할 때만(관찰만 있는 Pump도) 바꾼다.

시험이 찾은 것: (1) 다시 검색한 하위 트리가 덮은 대기 파일이 그 Pump에 다시 읽을 차례가 아니면 대기에 남았다 — 덮인 대기를 빼게 고쳤다. (2) 쓰는 중 판정이 "진단 코드가 CorruptPackage"여서 다 쓴 파일의 Tags 손상(항목이 있음)도 미뤘다 — "항목 없이 진단 하나"로 좁혔다. (3) 정규화 키의 비할당 실패 대체·`ValidPrefix`의 마지막 반환·누적 `||`는 입력 계약상 닿을 수 없었다 — 구조를 바꿔 없앴다(단언으로 바꾸지 않았다). (4) 메모리 파일 시스템은 크기와 시각이 함께 바뀌어 "시각만 바뀜"에 닿지 않았다 — 시각 이동 값(`modifiedShift`)을 더했다.

#### 14.1.31 요구 대조표(A2d2 — 병렬 범위 읽기)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetIndexParallelTests.cpp`(2개 — 디스크 프로젝트, `PhysicalFileSystem`)와, 직렬 경로가 그대로임을 보이는 기존 색인·캐시·감시·서비스 시험(메모리 파일 시스템 — scheduler 없음), 명령 경로의 C# `Programs/Rocks.Commandlet.Tests`(253개 — 명령이 worker 3개로 검색하므로 모든 색인 명령 시험이 병렬 경로를 탄다)다. 측정은 `Programs/AssetIndexBench`(7.1-16 A2d2 구현 세부의 수치).

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 병렬 = 직렬(7.1-16 A2d2) | 패키지 300개(중첩 디렉터리 15개·같은 id·Tags 손상·잘린 파일·빈 중첩 디렉터리) Rebuild가 worker 1·2·4·7에서 직렬과 항목·진단 모든 칸이 같음, 캐시 재사용도 같음, 하위 트리 RefreshPaths도 같음 | `AssetIndexParallel_MatchesSerial` |
| 호출 스레드 혼자(7.1-16 A2d2, ParallelFor 계약) | 시작하지 않은 scheduler, 그 scheduler의 worker 안에서 부른 검색 — 결과 같음 | `AssetIndexParallel_MatchesSerial` |
| 할당 실패(7.1-16 A2d2) | 병렬 검색의 모든 할당 시도(스레드 안전 실패 allocator): 검색 실패면 OutOfMemory·비운 색인, 검색이 삼킨 실패(helper 제출)면 직렬과 같음, 실패가 없어지면 직렬과 같음. 병렬이라 n번째 시도가 떨어지는 지점은 실행마다 다를 수 있다 — 단언은 결과 분류(실패·삼킴·성공)로 결정적이고 반복은 시도 수가 유한해 끝난다. 배치 allocator 실패는 시험 밖(14.1.30 `IndexCached`와 같은 사유) | `AssetIndexParallel_AllocationFailures` |
| 직렬 경로(7.1-16 A2d2) | scheduler 없음 = 같은 본문을 호출 스레드가 차례로 — 기존 시험 전부(색인 32·캐시 9·감시 12·서비스 9·패키지 집합 22) | 기존 시험 |
| 명령 연결(7.1-16 A2d2 결정) | asset-list·asset-index-verify·asset-index-rebuild·편집 세션(asset-new·asset-set·asset-info)이 worker 3개로 검색해도 출력이 같음 | C# 시험 253개 |
| 서비스 주입(7.1-16 A2d2) | `AssetServiceConfig::scheduler` → 색인 | C# 편집 명령(편집 세션이 넘긴다) |
| 조합(14 완료 조건 2) | worker 수(없음·시작 전·1·2·4·7·3(명령)) × 캐시(없음·있음) × 검색(전체·하위 트리) × 파일 상태(정상·같은 id·Tags 손상·잘림·빈 디렉터리) × 할당 실패 위치(호출 스레드·worker·helper 제출) | 위 시험 |

#### 14.1.32 Coverage(A2d2)

측정(2026-11-21, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetIndexParallel,AssetIndexRefreshPaths,AssetIndex_,AssetIndexWatch,AssetIndexCache,AssetService_,EditorPackage -Sources Engine/Source/Developer/AssetDatabase/Private`, 명령은 C# 시험 구동기.

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetIndex.cpp`(걷기·읽기 나눔, 병렬 읽기) | 100%(1263/1263) | 99.5%(654/657) | 100%(48/48) |
| `AssetService.cpp`(scheduler 주입) | 99.2%(239/241) | 97.6%(121/124) | 100%(13/13) |
| `Programs/Commandlet/Private/Common/AssetCommandletSupport.cpp`(`ScanScheduler`·편집 세션 주입) | 81.6%(124/152) | 69.1%(47/68) | 100%(9/9) |
| `Programs/Commandlet/Private/Common/AssetIndexCommandlet.cpp`(색인 명령 셋의 scheduler) | 85.6%(214/250) | 84.1%(111/132) | 100%(6/6) |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `AssetIndex.cpp` 분기 3개 | 14.1.30과 같다 | `ROCKS_ASSERT` 전개 2개, `IndexCached` 논리 경로의 배치 allocator 할당 실패 |
| `AssetService.cpp` | 14.1.24·14.1.28과 같다 | List IsA 그 밖 오류 등(이번에 바뀐 줄은 생성자 인자 하나) |
| `ScanScheduler` 생성자·소멸자 분기 5개 | 논리 프로세서 1개(worker 0 → 직렬)·3개 이하(상한보다 작은 worker 수)·스레드 생성 실패(`Start` 실패 → 직렬)와 그 소멸자 쪽 | 기기의 프로세서 수와 OS 스레드 생성 실패에 달렸다 — 측정 기기(20개)에서는 늘 worker 3개로 시작한다. 직렬로 떨어진 뒤의 동작은 scheduler 없는 색인 시험 전부가 같은 본문으로 덮는다 |
| 명령 파일의 그 밖 줄 | 14.1.26·14.1.28과 같다 | 프로세스 시험이 명령 allocator 실패·현재 디렉터리 실패를 만들 수 없다(바뀐 줄은 위 셋뿐) |

시험이 찾은 것: 기대 항목 수를 셀 때 잘린 파일(색인 안 됨)과 Tags 손상 파일(항목 있음)을 헷갈렸다(시험 수정). 측정이 찾은 것: worker를 3개보다 늘리면 느려진다 — 명령의 기본값을 그 측정으로 정했다.

#### 14.1.33 요구 대조표(A2e1 — 경로 별칭 표)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetAliasTableTests.cpp`(6개)와 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-alias-*·별칭 해석" 항목 39개(Windows 전용 7개)다. C++ 장면은 루트를 디스크 임시 디렉터리(잠금)로 두고 그 루트에 놓인 메모리 파일 시스템에 Content·Config를 둔다. 왕복 결과는 golden 텍스트(결정적 byte)로, 시각은 수동 시계로 단언한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 왕복(4.3, 7.1-17 A2e1) | 파일 없음 = 빈 표(읽은 상태), 더하기 정렬·같은 키는 뒤가 이김(caseless), golden byte(JsonWriter 기본 — 항목마다 6줄, 시각 서식 100 ns까지), 다시 읽으면 같음, caseless Find·없음, 지우기·바꾸기, 빈 표도 파일, 만료 시각(넘침 포화) | `AssetAliasTable_RoundTrips` |
| 신뢰하지 않는 입력(7.1-17 A2e1) | JSON 문법(줄·열)·중복 키, 최상위(배열·빠진 키·남는 키·이름이 다른 같은 수 키), version(2·문자열·1.5), aliases(객체), 항목(객체 아님·빠진 키·남는 키·이름이 다른 키), path(문자열 아님·모르는 마운트·이름 규칙), asset(문자열 아님·대문자·null), created(문자열 아님·없는 날짜), days(0·36501·문자열·1.5·음수), 같은 경로 키(정렬 뒤 뒤쪽 번호), 항목 수 상한(CapacityExceeded), 파일 크기 상한, 경계 값(days 1·36500·시간대 표기)은 받음, 정렬이 어긋난 파일은 받아 정렬, 읽기 IO(Busy·읽기 실패 — 진단 None), 진단 칸 없는 실패 | `AssetAliasTable_RejectsInvalidFile` |
| 쓰기(7.1-17 A2e1) | 외부 변경(내용 1 byte·다른 표·지워짐·없던 파일이 생김·너무 커짐 — ExternalModification / StaleHandle, 파일·표 그대로)·다시 읽기 실패(진단 None), 읽지 않은 표·잠금 아님·경로 규칙(InvalidPath)·null id·days 범위 밖·항목 수 상한 — 파일·표 그대로, IO 고장(만들기·쓰기·Flush·Close·교체·Config가 파일) — 임시 파일 없음·그대로, NotDurable 성공 표시, 지우기와 더하기를 함께 한 쓰기의 모든 할당 실패 | `AssetAliasTable_WriteFailures` |
| 되돌리기(7.1-17 A2e1) | 쓰기 전 파일 byte·표로, 없던 파일은 지움, 마지막 쓰기 뒤 밖에서 지우거나 고쳤으면 StaleHandle(표 그대로)·다시 읽기 실패, 되돌릴 것 없음, 잠금 아님, 쓰기·지우기 IO 실패, 있던·없던 상태의 할당 실패 | `AssetAliasTable_Restores` |
| prune(4.3, 7.1-17 A2e1) | 만료 경계(created + days = 지금은 만료, 1 tick 전은 아님), 쓰임 아닌 잡음(256 byte를 넘는 후보·마운트 아닌 앞부분·파일 끝에서 잘린 마운트), 경계 문자 열 가지·파일 끝·ASCII 대소문자 무시·하위 디렉터리·줄 번호, 경계가 아닌 뒤 글자는 쓰임 아님, 링크 파일·링크 디렉터리·별칭 표 임시 파일·일반 파일 아님 건너뜀, 제어 문자 파일 이름 escape, 검색 상한 셋(파일 수·총 byte·파일 하나 — 아무것도 지우지 않음, 진단 칸 있고 없음), 하위 디렉터리 열거 실패·읽기 실패 오류, 열거 뒤 사라진 파일·디렉터리 건너뜀, 만료 없음·모두 쓰임은 쓰지 않음, 읽지 않은 표·잠금 아님, 할당 실패(형제 디렉터리 여럿·지울 항목 하나) | `AssetAliasTable_Prunes` |
| 서비스(7.1-17 A2e1) | Initialize 전 InvalidArgument, 경고가 옛 경로 텍스트를 담음, 깨진 표의 참조 값 해석은 경고 AliasTableUnreadable(진단 CorruptAliasTable), 지연 적재(깨진 표도 색인에 있는 경로는 열림, 별칭이 필요한 호출 — 열기·만들기·remove·prune — 은 CorruptAliasTable, 다음 호출이 다시 읽음), 열기가 옛 경로를 풀고 AliasResolved(detail = 항목 번호, 경고 목록 없이도), 색인이 이김, 없는 경로 NotFound(진단 None), MissingFile은 별칭으로 가지 않음, 경고를 붙이지 못하면 OutOfMemory, 참조 텍스트(asset-set Hard)도 풂, 별칭 경로에 새 패키지 DuplicatePath / AlreadyExists(related, 진단 칸 없이도), remove(경로 규칙·없음·성공), prune(설정 시계), 시스템 시계 서비스의 열기·remove·prune 할당 실패 | `AssetAliasTable_ServiceResolves` |
| Core JsonWriter 첫 오류(7.1-17 A2e1 리뷰) | 모든 할당 시도에서 실패하면 이후 호출·Finish가 OutOfMemory, 잘못된 사용 뒤 Finish는 InvalidArgument | `Json_WriterKeepsFirstError` |
| 고아 임시 파일(7.1-12·17) | Config 바로 아래 `AssetAliases.json.<guid>.tmp`는 지우고 하위 디렉터리의 같은 이름은 남김, 재구축 명령이 지움 | `AssetAliasTable_ServiceResolves`, C# `asset-index-rebuild removes alias temporary files` |
| 명령(13, 7.1-17 A2e1) | asset-alias-list(만료·shadowed·missing·지금 경로 줄), asset-info가 별칭으로 풀고 경고 줄, asset-new 별칭 경로 거부, asset-alias-prune(쓰인 곳 경고 줄·요약·검색 상한 경고), asset-alias-remove(성공·없음·경로 규칙), 깨진 표 진단 줄 셋(JSON 줄·열, 칸·항목 번호, 헤더 칸), 깨진 표에도 색인 경로 asset-info 성공, 별칭 표 자리가 디렉터리, 교체 실패·검색 읽기 실패·무효 출력 핸들 셋·잠금 Busy(Windows), Content가 파일·Intermediate가 파일, 사용법 여덟·도움말 셋·프로젝트 없음 셋 | C# 39개 |
| 조합(14 완료 조건 2) | 표 상태(없음·빈·정상·깨짐 종류마다·정렬 어긋남) × 연산(읽기·쓰기·되돌리기·prune·해석·점유), 항목 상태(만료·아님·경계) × 쓰임(경계 문자·대소문자·하위·링크·제외 대상), 실패 위치(입력 검사·IO 단계마다·할당) | 위 시험 |

#### 14.1.34 Coverage(A2e1)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetAliasTable,AssetIndexCache,AssetService_,AssetTextResolver,AssetIndex_ -Sources Engine/Source/Developer/AssetDatabase/Private`, 명령은 C# 시험 구동기.

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `AssetAliasTable.cpp` | 99.3%(747/752) | 99.5%(380/382) | 100%(26/26) |
| `AtomicFileWrite.cpp`(캐시·별칭 공용 원자 쓰기) | 97.3%(72/74) | 96.7%(29/30) | 100%(2/2) |
| `AssetService.cpp`(별칭 해석·점유·remove·prune) | 99.4%(351/353) | 98.4%(179/182) | 100%(18/18) |
| `TemporaryFiles.cpp`(Config 별칭 임시 파일) | 100%(136/136) | 100%(68/68) | 100%(8/8) |
| `Programs/Commandlet/Private/Common/AssetAliasCommandlet.cpp` | 89.1%(253/284) | 85.4%(111/130) | 100%(9/9) |
| `Core/Source/Runtime/Core/Private/Text/JsonWriter.cpp`(첫 오류 — 바뀐 줄 다섯) | 바뀐 줄 모두 닿음 | 같음 | — |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `AssetAliasTable` 파일 크기 조회 실패 | 분기 | 연 파일의 크기 조회는 메모리 파일 시스템에서 실패하지 않고 실제 OS에서도 열린 핸들로는 거의 실패하지 않는다(14.1.28 캐시 크기 조회와 같다) |
| prune 읽기 때 상한 초과(열거 뒤 커진 파일) | 분기 | 열거한 크기와 읽을 때 크기가 다른 경합이다 — 메모리 파일 시스템은 같다. 열거 크기 상한(같은 판정)은 시험한다 |
| `AtomicFileWrite` 임시 이름 guid 발급 실패 | 실패 | OS 보안 난수 실패(14.1.22와 같다) |
| `AssetService.cpp` 2줄·분기 3 | 14.1.24와 같다 | List IsA 그 밖 오류(이번 단계가 바꾼 줄이 아니다) |
| 명령의 할당 실패(표 적재·색인·서식·경로) | 실패 | 프로세스 시험은 명령 allocator에 실패를 주입할 수 없다(14.1.26과 같다 — 같은 경로는 C++ 할당 실패 시험이 돈다) |
| 명령의 NotDurable 경고(remove·prune) | 분기 | 디렉터리 동기화 실패는 파일 시스템 성질이라 프로세스 시험이 만들 수 없다(C++ 쓰기 시험이 NotDurable 표시를 본다) |

리뷰가 찾은 것(필수 둘): 서비스가 쥔 별칭 표가 그 사이 바뀐 파일을 덮어썼다(외부 변경 판정을 더했다), JSON writer 문제를 인코더에서 우회했다(Core `JsonWriter`가 첫 오류를 기억해 이후 호출·Finish가 그 오류로 실패하게 고치고 `Json_WriterKeepsFirstError` 시험을 더했다 — 인코더 우회는 지웠다). 권장 셋(경고에 옛 경로 텍스트, 별칭 표를 못 읽은 해석의 경고, prune 검색 비용)도 반영했다.

시험이 찾은 것: (1) JSON writer는 한 번 실패하면 Finish가 첫 오류가 아니라 InvalidArgument를 돌려준다 — 별칭 표 쓰기의 할당 실패가 OutOfMemory로 보고되지 않았다(위 리뷰 필수 둘로 Core에서 고쳤다). (2) 시각을 tick으로 들고 다녀 인코딩 때 변환 실패 분기가 닿을 수 없었다 — DateTime으로 바꿔 없앴다. (3) 크기를 잰 뒤 파일이 줄어든 경우의 별도 처리는 닿을 수 없었다 — IO 오류로 보고한다.

#### 14.1.35 요구 대조표(A2e2 — Detach·이동)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetMoveTests.cpp`(6개)와 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-move" 항목 18개(Windows 전용 1개)다. C++ 장면은 루트를 디스크 임시 디렉터리(잠금)로 두고 그 루트에 놓인 메모리 파일 시스템(대소문자 구분, `Move` 고장 주입 — `MemoryFaults::move`)에 Content·Config를 둔다. 색인 결과는 매번 이동 뒤 같은 파일 상태의 새 검색(`Rebuild`)과 같은지로 단언한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 색인 이름 바꾸기(7.1-17 A2e2) | 디렉터리(중첩·손상 파일·열거 실패 하위 디렉터리·잘못된 UTF-8 패키지·디렉터리 이름 — 진단 이름도 새 이름 아래, 옛 이름 진단 없음, 패키지 아닌 잘못된 이름 파일은 무시), 파일, 대소문자만, 정규화 대조로 부른 이동, 없는 원본, 링크(따라가지 않음), 패키지가 아닌 파일, 같은 이름(같은 키), 시작 이름 Stat 실패(새 이름의 진단 None·그 오류, 옛 항목 빠짐), 규칙 위반 다섯(빈 이름·앞 '/'·'..'·서로의 아래 둘 — InvalidArgument, 색인 그대로), 준비 → OS 이동 → 확정이 새 검색과 같음, 할당 실패 다섯 장면(디렉터리·파일·열거 실패 진단뿐·잘못된 UTF-8 이름뿐·Stat 실패 — 이름 할당이 처음 일어나는 곳이 다르다) | `AssetMove_IndexRenames` |
| 서비스 이동(7.1-17 A2e2) | 파일(별칭 하나·옛 경로가 별칭으로 열림 — AliasResolved), 자기 별칭 자리로 되돌아가기(그 별칭 지움·새 별칭), 디렉터리(열린 패키지 Detach·fileName 새 이름·logicalPath 없음, 저장하지 않은 새 패키지 logicalPath 새 경로·별칭 없음, 하위 디렉터리, 별칭 둘), 이름이 Dir로 시작하는 형제(DirX 열린 패키지·DirNew 새 패키지)는 그대로, 새 경로에 저장(옮긴 열린 패키지 덮어쓰기·새 패키지 첫 저장), 대소문자만(별칭 없음·fileName 철자), 디스크에 없는 디렉터리(새 패키지뿐)·새 패키지 하나(OS 이동 없음), 빈 디렉터리 | `AssetMove_MovesPackagesAndDirectories` |
| 거부·되돌리기(7.1-17 A2e2) | Initialize 전·잠금 아님 InvalidArgument, 원본·대상 경로 규칙(InvalidPath), 대상이 원본 아래, 원본 없음(NotFound·진단 None), 원본 경로 키 둘, 대소문자만 다른 OS 디렉터리 둘에 걸친 디렉터리, 대상 점유 — 색인 항목(related)·색인 디렉터리·디스크 파일·다른 에셋 별칭(대소문자 무시·related)·새 패키지 경로와 그 아래(related) — 모두 DuplicatePath / AlreadyExists, 새 경로 규칙(디렉터리 아래 이름만 256 byte를 넘음 — 그 id), 대상·원본 Stat 실패(그 오류·진단 None), 색인 항목의 파일이 사라짐(MissingFile·그 id), 별칭 쓰기 실패, 대상 부모가 파일·OS Move 실패(별칭 byte 되돌림·경고 없음)·부모를 만든 뒤 Move 실패(빈 부모가 남음)·새 패키지 하나의 같은 이름 디렉터리는 옮기지 않음·낡은 색인의 같은 이름 파일 거부, 되돌리기 실패(AliasNotRestored — 경고 목록 있고 없음), 별칭 표 손상(CorruptAliasTable), Detach 실패(읽을 수 없게 된 열린 패키지 — 아무것도 옮기지 않음), 시계 범위 밖(Overflow), 경고 자리 할당 실패(바꾸기 전 OutOfMemory), 별칭 없는 이동의 OS 실패, 집합을 바로 부른 경로 바꾸기의 새 패키지 경로 위반(InvalidPath·그 id, 진단 칸 없이도), NotDurable 성공 경고(경고 목록 없이도 성공) — 실패는 모두 파일·별칭 파일 byte·색인 그대로 | `AssetMove_RejectsAndRestores` |
| OS 경로 상한(4.2, 7.1-10) | 깊은 디스크 루트에서 대상 이름·패키지 없는 원본 디렉터리의 절대 OS 경로가 상한을 넘으면 InvalidPath / CapacityExceeded(아무것도 바꾸지 않음), 짧은 이름은 옮겨짐 | `AssetMove_NativePathLimit` |
| 할당 실패(7.1-17 A2e2) | 다섯 장면(열린 패키지·새 패키지가 든 디렉터리, 파일, 자기 별칭 되돌아가기, 디스크에 없는 디렉터리, 빈 디렉터리)의 모든 할당 실패 — OutOfMemory, 경고 없음, 파일·별칭 파일·색인·새 패키지 경로 그대로, 실패가 없어지면 끝까지 | `AssetMove_AllocationFailures` |
| 실제 디스크(7.1-6·17) | bulk를 읽어 캐시 핸들이 열린 패키지가 든 디렉터리를 Windows에서 옮김(Detach가 핸들을 닫지 않으면 이름 바꾸기 실패), 옮긴 뒤 bulk는 메모리에서 읽힘, 새 경로에 저장, 옛 디렉터리 없음, 옛 경로는 별칭으로 열림 | `AssetMove_RealDisk` |
| 명령(13, 7.1-17 A2e2) | 파일(대상 부모 만듦)·디렉터리(하위째) 요약 줄, 옛 경로 asset-info 경고 줄, 새 경로 열림, 대소문자만(별칭 0·디렉터리 철자), 패키지 위·다른 에셋 별칭 위(related id)·없는 원본·자기 안으로 거부 줄, 잠금 Busy(Windows), 사용법 다섯·도움말·프로젝트 없음 | C# 18개 |
| 조합(14 완료 조건 2) | 원본 종류(색인 패키지·새 패키지·디렉터리 — 항목·새 패키지·빈) × 열림 상태(닫힘·열림·bulk 읽음·dirty) × 대상 상태(빈·점유 다섯 종류·자기 별칭·대소문자만·부모 없음·부모가 파일) × 실패 위치(검사·준비·Detach·별칭 쓰기·부모 만들기·OS Move·되돌리기·할당) | 위 시험 |

#### 14.1.36 Coverage(A2e2)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetMove,AssetIndex -Sources Engine/Source/Developer/AssetDatabase/Private`, 명령은 C# 시험 구동기(`-Target Commandlet`).

| 범위 | 줄 | 분기 |
|---|---|---|
| `AssetService::Move`(`AssetService.cpp`) | 모두 닿음 | 모두 닿음 |
| `EditorPackageSet::PrepareRelocate`·`CommitRelocate`·`DetachUnder` | 모두 닿음 | 모두 닿음 |
| `AssetIndex.cpp`(이름 바꾸기 — `ScanTree` renameTo·moves 검사·빼기) | 파일 100%(1381/1381) | 이름 바꾸기 분기 모두 닿음(파일 711/715 — 남은 넷은 이번 단계가 바꾼 줄이 아니다) |
| `AssetDatabaseInternal.h`(`IsSameOrUnder`·`AfterComponents`·`LogicalComponents`) | 모두 닿음 | 모두 닿음 |
| `RunAssetMoveCommand`(`AssetEditCommandlet.cpp`) | 3곳 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `RunAssetMoveCommand` 경고 줄·요약 줄 쓰기 실패, 요약 서식 실패 | 실패 | 출력 실패는 무효 출력 핸들 시험(14.1.34와 같은 모양)이 다른 명령에서 같은 `Write`·`WriteWarnings` 경로를 돈다. 서식 실패는 할당 실패뿐이다 — 프로세스 시험은 명령 allocator에 실패를 주입할 수 없다(14.1.26과 같다) |
| `CommandletHost` `asset-move` 등록 실패 | 실패 | 명령 표는 고정이고 이름이 겹치지 않는다(다른 명령 등록과 같다) |

리뷰(판정 충족, 필수 없음) 권장 넷: 패키지·디렉터리 같은 이름의 우선순위, 항목 없는 디렉터리의 철자 한계, 낡은 색인의 새 패키지 이동, 남는 부모 디렉터리 — 모두 위 7.1절 17에 적고 앞의 둘은 한계로, 셋째는 거부로 고쳤다(새 패키지 하나의 원본 이름이 디렉터리 이름이던 결함도 함께), 넷째는 시험에 이름을 붙였다.

시험이 찾은 것: (1) 저장된 열린 패키지는 `logicalPath`가 비어 있는데(색인 항목이 권위) 집합 경로 바꾸기·Detach·대상 점유 검사가 모든 열린 패키지의 `logicalPath`를 해석해 열린 패키지가 하나라도 있으면 이동이 InvalidArgument였다 — 저장된 패키지는 OS 이름(`fileName`)으로 대조하게 고쳤다. (2) 원본 Stat의 없음 밖 오류를 "디스크에 없음"으로 보아 OS 이동 없이 경로 표만 바꿀 수 있었다 — 그 오류로 실패하고, 파일이 있어야 할 색인 항목이 없으면 MissingFile로 고쳤다. (3) 대소문자 구분 FS에서 한 논리 디렉터리가 OS 디렉터리 둘에 걸치면 하나만 옮겨졌다 — 거부한다. (4) OS 경로 문자열을 별칭을 쓴 뒤 만들어 그 할당 실패가 되돌리기를 거쳤고, 확정 뒤 NotDurable 경고 할당 실패가 옮긴 뒤 OutOfMemory를 돌려줄 수 있었다 — 별칭 쓰기 전에 모두 만들고 경고 자리를 잡는다. (5) 대상 부모 찾기의 `slash > 0` 판정은 닿을 수 없었다(toFull은 Content 뒤 '/'를 품는다) — 판정을 없앴다. (6) 긴 대상 시험이 대상 자체의 구성 요소 길이(64 byte)에서 거부되어 의도한 "아래 이름만 상한 밖"을 보지 못했다 — 구성 요소를 나눠 고쳤다.

#### 14.1.37 요구 대조표(A2e3a — 간선 표·닫기·삭제)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetDeleteTests.cpp`(8개)와 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-delete" 항목 16개(Windows 전용 1개)다. 장면은 A2e2와 같다(디스크 임시 루트·메모리 파일 시스템·잡은 잠금 — 집합 시험은 메모리 프로젝트). 색인 결과는 매번 새 검색과 같은지로 단언한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 간선 표(7.1-17 A2e3a) | Initialize 전 InvalidArgument(동기화·조회·닫기), 열기의 다중도(Villain → Hero 2·Hero → Base·Derived), 첫 참조자(id 순 — 두 참조자), 열리지 않은 id 0, 편집(끊기·다시 잇기 — 같은 원인 합산), 열기와 편집이 한 묶음(Commit + Edit 합 0 — 표에 없는 쌍, 합 1), 닫기가 나가는 간선 반납, 소유 스레드가 아님 WrongThread(묶음 그대로) | `AssetDelete_EdgeTableAndClose` |
| 닫기(7.1-17 A2e3a) | 들어오는 참조 ReferencedBy / Busy(related), 닫기·다시 닫기 NotFound(진단 칸 있고 없음), 닫은 패키지 다시 열기, 저장하지 않은 새 패키지 닫기, Create 초기화 안 Busy | `AssetDelete_EdgeTableAndClose` |
| 간선·닫기 할당 실패(7.1-17 A2e3a) | 동기화의 모든 할당 실패 — 묶음을 쥐고 다음 호출이 같은 다중도, 쌓인 묶음이 있을 때 새 패키지 열기·만들기의 할당 실패(시작의 동기화 포함 — 아무것도 열거나 만들지 않음), 닫기(안의 동기화·경로·Discard) 모든 할당 실패 — 패키지·다중도 그대로(GC 표 실패는 Registry가 삼킨 성공) | `AssetDelete_EdgeTableAllocationFailures` |
| 색인 빼기(7.1-17 A2e3a) | 파일·디렉터리(그 아래 항목·손상·열거 실패 진단)·없는 이름을 지우기 전 준비 → 지우기 → 확정이 새 검색과 같음, 이름 규칙 위반 셋 InvalidArgument, 할당 실패 | `AssetDelete_IndexRemoved` |
| 임포트 소유자·Soft 편집(7.1-17 A2e3a) | ImportOwner 의존만 있는 참조자가 소유자 삭제를 막음(ReferencedBy·related)·force는 지움, 열린 패키지의 저장하지 않은 Soft 참조는 삭제를 막지 않음(한계 고정) | `AssetDelete_ImportOwnerAndSoftEdits` |
| 삭제(7.1-17 A2e3a) | 닫힌 패키지(별칭 하나 지움 — AliasRemoved 경고 text·asset, 다른 에셋 별칭은 남음), id로 고른 열린 패키지(별칭 없음 — 별칭 파일 안 씀, 닫힘), 자기만 참조하는 패키지, 저장하지 않은 새 패키지(경로 — 열린 저장 패키지·다른 새 패키지를 지나서, id — 파일 지우기 없음), 디스크 참조자 거부(InvalidArgument·related·목록 경로·id), 열린 참조자는 force여도 Busy, 참조자를 닫은 뒤 force(목록, 참조자 MissingDependency), NotDurable 경고(경고 목록 있고 없음), 열린 읽기 전용 패키지(모르는 커스텀 버전), 파일이 이미 없는 낡은 색인(성공·색인 확정) | `AssetDelete_DeletesPackages` |
| 거부·되돌리기(7.1-17 A2e3a) | Initialize 전·잠금 아님, 경로·id 텍스트 형식·null id(InvalidPath), 없음(경로·id·별칭 경로는 따르지 않음), 같은 경로 키 둘(DuplicatePath), 같은 id 파일 둘(id·경로 — DuplicateAssetId), 열렸지만 색인에서 빠진 저장 패키지(NotFound — 색인이 권위), 별칭 표 손상, 별칭 쓰기 실패, 경고 자리 할당 실패, 지우기 실패(별칭 되돌림·패키지는 닫힌 채 다시 열림), 되돌리기 실패(AliasNotRestored — 경고 목록 있고 없음) — 실패는 모두 파일·별칭 byte·색인 그대로 | `AssetDelete_RejectsAndRestores` |
| 할당 실패(7.1-17 A2e3a) | 네 장면(별칭 있는 열린 패키지, 디스크 참조자가 있는 force, 나가는 간선이 있는 열린 패키지, 저장하지 않은 새 패키지)의 모든 할당 실패(결과 목록 포함) — OutOfMemory, 경고 없음, 파일·별칭·색인 그대로 | `AssetDelete_AllocationFailures` |
| 실제 디스크(7.1-6·17) | bulk를 읽어 캐시 핸들이 열린 패키지를 Windows에서 지움(닫기가 핸들을 닫지 않으면 실패), 집합에서 빠짐 | `AssetDelete_RealDisk` |
| 명령(13, 7.1-17 A2e3a) | 지우기 요약 줄·AliasRemoved 경고 줄·옛 경로 NotFound, 참조자 줄과 ReferencedBy 진단 줄(종료 1), --force 경고 줄·검증 MissingDependency, id로 고르기, 없음·경로 규칙 진단 줄, 잠금 Busy(Windows), 사용법 넷·도움말·프로젝트 없음 | C# 16개 |
| 조합(14 완료 조건 2) | 대상 종류(색인 항목·열린 저장 패키지·저장하지 않은 새 패키지·색인에서 빠진 열린 패키지) × 고르기(경로·id) × 참조 상태(없음·디스크·메모리·자기) × force × 별칭(없음·대상·다른 에셋) × 실패 위치(검사·별칭 쓰기·닫기·지우기·되돌리기·할당) | 위 시험 |

#### 14.1.38 Coverage(A2e3a)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetDelete,AssetMove,AssetIndex,EditorPackage -Sources Engine/Source/Developer/AssetDatabase/Private`, 명령은 C# 시험 구동기(`-Target Commandlet`).

| 범위 | 줄 | 분기 |
|---|---|---|
| `AssetService::Delete` | 모두 닿음 | 모두 닿음 |
| `EditorPackageSet::SyncReferences`·`IncomingReferences`·`Close`, Open·Create·Save 시작의 동기화 | 1곳 제외 모두 | 같음 |
| `AssetIndex.cpp`(`removed` 검사·빼기) | 파일 100%(1423/1423) | 바꾼 줄 모두 닿음(파일 722/725 — 남은 셋은 이번 단계가 바꾼 줄이 아니다) |
| `RunAssetDeleteCommand`·경고 줄 `AliasRemoved` | 출력 쓰기·서식 실패 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `SyncReferences` 음수 다중도 Fatal | 줄 | 관찰자는 실제 간선 변화만 기록하고 묶음을 합친 뒤 적용하므로 음수는 Registry 결함일 때뿐이다(AssetManager 간선 표와 같은 불변식) |
| `RunAssetDeleteCommand` 참조자·경고·요약 줄 쓰기 실패, 요약 서식 실패 | 실패 | 14.1.36과 같다(무효 출력 핸들 시험이 같은 `Fail`·`Write` 경로를 다른 명령에서 돈다, 서식 실패는 할당 실패) |

리뷰(판정 충족, 필수 없음) 권장 넷: 관찰자 자동 동기화(Open·Create·Save·Close 시작), 빠진 조합 셋(읽기 전용·ImportOwner 참조자·저장하지 않은 Soft 편집), 파일이 이미 없는 삭제의 성공, 참조자 줄 형식 예 — 모두 반영했다.

시험이 찾은 것: (1) 관찰자가 붙어 있는데 아무도 묶음을 꺼내지 않아 편집이 쌓이면 키 상한에서 Registry 쓰기가 CapacityExceeded가 될 수 있었다 — 간선 표가 꺼내고 오래 사는 host는 주기적으로 부른다(7.1절 17). (2) 자기 참조 제외 판정은 닿을 수 없었다(색인에 자기 의존이 없다) — 지웠다. (3) handle 정렬의 세대 비교는 한 묶음에 같은 칸 다른 세대가 오지 않아 닿을 수 없었다 — 칸·세대를 한 64-bit 키로 비교한다. (4) 파일이 이미 없는 삭제가 오류로 끝나 낡은 색인이 다시 검색할 때까지 남았다 — 성공으로 본다(리뷰 권장). (5) 명령의 "메모리 참조 Busy" 구분은 명령 프로세스에서 닿을 수 없다(대상을 열지 않은 새 프로세스) — 참조자 줄은 목록이 있으면 쓰고 지웠을 때만 warning으로 단순화했다.

#### 14.1.39 요구 대조표(A2e3b — 복제)

시험은 Foundation `Core/Source/Runtime/ObjectSerialization/Tests/CodecGraphTests.cpp`·`MapGraphTests.cpp`(각 1개), EngineTests `Engine/Source/Runtime/AssetRuntime/Tests/PackageDuplicateTests.cpp`(1개)·`Engine/Source/Developer/AssetDatabase/Tests/AssetDuplicateTests.cpp`(6개)와 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-duplicate" 항목 15개(Windows 전용 1개)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| Core Soft 치환 hook(7.1-17 A2e3b) | null이 아닌 값마다 부름(경로·기대 타입·Required 플래그·필드 경로), 바뀐 asset을 쓰고 경로는 그대로, 수집도 바뀐 asset, 바꾸지 않는 값 그대로, null asset 반환 InvalidArgument, hook 실패 전파 | `Codec_SoftReferencesSubstitutedOnSave` |
| Core 치환 — 컨테이너(7.1-17 A2e3b) | Map 키(같은 asset·다른 경로 둘 — 둘 다 남고 삽입 순서 그대로, null 키는 부르지 않음)·Set 원소·Map 값 struct 멤버 | `MapGraph_SoftKeysSubstitutedOnSave` |
| writer 복제 옵션(7.1-17 A2e3b) | 헤더 id = 사본 id, DuplicateTransient·NonTryoutDuplicateTransient는 로드에서 기본값(struct 필드·원소 배열 안 멤버 포함), 자기 Soft는 사본 id·자기 의존 아님, 남 Soft 그대로, 원본 객체 그대로, 복제가 아닌 쓰기 그대로, 지금 id와 같은 사본 id·쿠킹 복제 InvalidArgument | `AssetPackage_DuplicateWrite` |
| 메모리 상태 복제(7.1-17 A2e3b) | 저장하지 않은 편집·bulk·의존(Hard·EditorOnly·Soft)이 사본에, 헤더 id, 새 디렉터리, 원본 정보(dirty·파일·저장소) 그대로, 사본은 열지 않음·색인에 새 항목, id로 고른 자기 참조 패키지(사본 Soft = 사본 id·의존 없음), 이동(Detach) 뒤 bulk가 메모리에만 있는 원본(bulk째), 별칭 경로 원본(AliasResolved), 저장하지 않은 새 패키지 원본(저장하지 않은 새 bulk `AddUnsaved`째 — 원본은 저장하지 않은 채), NotDurable 경고(목록 있고 없음) | `AssetDuplicate_CopiesMemoryState` |
| ImportRecord(7.1-17 A2e3b) | 소유 기록 — owner·원본 자신의 출력만 사본 id로(형제 출력은 빠짐, 원본·임포터 그대로), 참조 기록 — 기록과 ImportOwner 의존을 버리고 ImportRecordDropped(related = 소유자, 경고 목록 없이도 성공) | `AssetDuplicate_ImportRecords` |
| 거부·실패(7.1-17 A2e3b) | 서비스: Initialize 전·잠금 아님·대상 경로 규칙·다른 에셋 별칭(대소문자 무시·related)·대상 점유(색인 항목·디스크 파일·저장하지 않은 새 패키지 — related)·대상 Stat 실패·색인에만 있는 대상·원본 없음·읽기 전용 원본(NewerVersion)·미해결 import 원본(MissingDependency·related)·별칭 표 손상·hook 실패·경고 자리 할당 실패, hook의 Tags·Build 의존은 사본에 다시 만듦(hook 문맥의 asset = 사본 id, 저장은 그 패키지 id). 집합: null id·잠금 아님·원본이 열리지 않음·새 id가 색인·집합에 있음(DuplicateAssetId)·hook 표 없음(Initialize 전후)·만들기 초기화 안 Busy. IO: 부모 디렉터리·임시 파일 만들기·쓰기·Flush·Close·교체 실패 — 임시·대상 파일 없음·색인 그대로 | `AssetDuplicate_RejectsAndFailures` |
| OS 경로 상한(4.2, 7.1-10) | 깊은 디스크 루트에서 대상이 상한을 넘으면 InvalidPath / CapacityExceeded(아무것도 쓰지 않음), 짧은 이름은 복제 | `AssetDuplicate_NativePathLimit` |
| 할당 실패(7.1-17 A2e3b) | 편집한 열린 Hero(bulk·의존)와 소유 기록 Leaf, 저장하지 않은 새 패키지·쌓인 간선 편집이 있는 상태의 모든 할당 실패 — OutOfMemory, 경고 없음, 대상 파일·색인 그대로 | `AssetDuplicate_AllocationFailures` |
| hook 문맥(7.1-17 A2e3b 리뷰) | `AssetTypeHookContext::asset`이 CallBase로 부른 base hook까지 그대로 감 | `AssetTypeHooks_NearestAndCallBase` |
| 실제 디스크(7.1-17 A2e3b) | bulk를 읽어 캐시 핸들이 열린 Hero를 복제, 사본을 열어 bulk 읽기, 원본은 원본 파일에 이어진 채 | `AssetDuplicate_RealDisk` |
| 명령(13, 7.1-17 A2e3b) | 경로·id로 고른 원본의 사본 줄(새 id·새 파일·원본 남음), 사본 asset-info(값·참조), 대상이 패키지·다른 에셋 별칭(related)·없는 원본·대상 경로 규칙 진단 줄, 잠금 Busy(Windows), 사용법 넷·도움말·프로젝트 없음 | C# 15개 |
| 조합(14 완료 조건 2) | 원본 종류(닫힘·열림·편집·저장하지 않은 새 패키지·읽기 전용·미해결) × 고르기(경로·id·별칭) × 기록(없음·소유·참조) × 대상 상태(빈·점유 넷·별칭·상한) × 실패 위치(검사·hook·쓰기 IO 단계·할당) | 위 시험 |

#### 14.1.40 Coverage(A2e3b)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetDuplicate,AssetPackage_Duplicate,EditorPackage,AssetDelete,AssetMove -Sources Engine/Source/Developer/AssetDatabase/Private,Engine/Source/Runtime/AssetRuntime/Private`, `-Target Foundation -Executable FoundationTests -Filter Codec_,MapGraph_,RequiredCheck -Sources Core/Source/Runtime/ObjectRuntime/Public/Object`, 명령은 C# 시험 구동기.

| 범위 | 줄 | 분기 |
|---|---|---|
| `FieldEncodeContext::SubstituteSoftReference`, `WriteValueWire`의 Soft 치환 | 모두 닿음(쓰기 실패 두 곳은 아래) | 모두 닿음 |
| `PackageWriter.cpp`(복제 옵션·치환 hook·검사) | 모두 닿음 | 모두 닿음 |
| `EditorPackageSet::Duplicate`·`PlanNewTarget`·`PlanHookOutputs`·`WriteAndReplace` 복제 모드 | 1곳 제외 모두 | 같음 |
| `AssetService::Duplicate`, `OpenTarget`의 새 패키지 고르기 | 1곳 제외 모두 | 같음 |
| `RunAssetDuplicateCommand` | 출력·서식 실패 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `AssetService::Duplicate` 새 id 발급 실패, `WriteAndReplace` 임시 이름 guid 발급 실패 | 실패 | OS 보안 난수 실패(14.1.22와 같다) |
| `WriteValueWire` Soft 값의 guid·경로 쓰기 실패 | 실패 | 이번 단계 전에도 닿지 않던 줄이다(변수 이름만 바뀜) — 값 쓰기는 Archive 한도·할당 실패뿐이고 그 실패는 같은 함수의 다른 값 종류가 돈다 |
| `RunAssetDuplicateCommand` 출력 쓰기·서식 실패 | 실패 | 14.1.36과 같다 |
| 커버리지 표의 `if (auto reserved =`·`const PackageContents& contents` 0 | 표기 | 여러 줄 문장의 첫 줄 region 표기다 — 다음 줄들이 1,000번 넘게 실행된다 |

리뷰: 1차 미충족(필수 하나 — 소유 기록 복제가 형제 출력까지 사본 기록에 남겨 사본이 남의 출력의 소유를 주장했다, 원본 자신의 출력만 남기게 고쳤다). 권장 셋 반영: hook 문맥에 저장할 id(`AssetTypeHookContext::asset`), 빠진 조합(struct·원소 안 DuplicateTransient — fixture `DuplicableSlot`, Detach 뒤 원본, `AddUnsaved` bulk), 복제도 고아 임시 파일 정리.

시험이 찾은 것: (1) 서비스의 `<path|AssetId>` 열기가 저장하지 않은 새 패키지를 경로로 찾지 못했다(Info·Set·Duplicate 공통) — 같은 경로 키의 새 패키지를 준다. (2) 실제 디스크 시험이 다음 Open 뒤의 `EditorPackageInfo` 포인터를 다시 써 크래시했다(시험 결함 — API 계약대로 다시 찾는다).

#### 14.1.41 요구 대조표(A2e4a — 고아 출력)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetOrphanTests.cpp`(3개), 기존 색인·캐시 시험의 갱신(`AssetIndex_*` 넷 — 고아 경고를 떼고 단언, `AssetIndexCache_RejectsUntrustedInput` — 출력 목록 형식·상한, `AssetIndex_CommandletFixtureBytes` — 고아 fixture 둘)과 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-orphans-*·verify 경고" 항목 20개(Windows 전용 2개)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 판정 불가(7.1-17 A2e4a 리뷰) | owner 기록 읽지 못함(다른 스키마 UnsupportedVersion·형식 위반 CorruptPackage — 칸 ImportRecord), owner 패키지 읽지 못함(헤더 id가 있는 손상 — 칸이 ImportRecord가 아님), id를 모르는 진단(손상 파일)이 있을 때의 없는 owner — 모두 Undetermined·목록·경고 없음·DeleteOrphan 거부, 판정 사실 표의 할당 실패, 손상 파일을 지우면 OwnerMissing으로 바뀜 | `AssetOrphan_Undetermined` |
| 판정(7.1-17 A2e4a) | 소유 기록이 적은 출력 아님, 적지 않음 NotListed, owner 없음 OwnerMissing, owner가 기록 없음·참조 기록 OwnerNotImporter, 같은 id owner 둘 — 하나라도 적으면 아님·소유 기록 있고 적지 않으면 NotListed·기록 없으면 OwnerNotImporter, 기록 없는 항목·소유 기록 항목·같은 id 항목 자신은 아님 | `AssetOrphan_Judgement` |
| 목록·진단(7.1-17 A2e4a, 13) | Orphans 경로 키 순, Verify 고아마다 경고(asset·relatedAsset·detail·NotFound), 줄 셋(is not in the index·has no import record of its own·does not list this output), 경고 판정(고아만), Orphans·Verify 할당 실패 | `AssetOrphan_Judgement` |
| 캐시(7.1-16, 7.1-17 A2e4a) | 출력 목록 왕복(캐시 재사용 색인이 새 검색과 같음), 출력 수 상한 초과 캐시 버림, 스키마 버전 2 | `AssetOrphan_Judgement`, `AssetIndexCache_RejectsUntrustedInput` |
| 기존 색인 시험(7.1-13·14) | 고아 경고를 뗀 나머지 의존 대조 그대로, 고아 수(id를 모르는 진단이 없는 장면은 Hero 하나, 손상·이름 규칙 진단이 있는 장면은 판정 불가라 0) | `AssetIndex_DiskScanAndQueries`·`AssetIndex_Diagnostics`·`AssetIndex_VerifyTypes`·`AssetIndex_AllocationFailures` |
| 서비스 DeleteOrphan(7.1-17 A2e4a) | Initialize 전·잠금 아님 InvalidArgument, 경로 규칙 InvalidPath, 없음 NotFound, 같은 경로 키 둘 DuplicatePath, 고아 아님 InvalidArgument(진단 None·asset), 참조자가 있는 고아 ReferencedBy(related·목록), 고아 지우기(색인이 새 검색과 같음), 긴 경로 해석·지우기의 할당 실패, 같은 서비스에서 owner를 지운 뒤(force — 출력이 ImportOwner로 참조) 그 출력이 곧바로 고아로 지워짐 | `AssetOrphan_ServiceDelete` |
| 명령(13, 7.1-17 A2e4a) | 목록 줄(경로 키 순·owner 경로·missing·같은 id owner는 AssetId), 경고만인 verify 종료 0과 요약, 오류와 경고가 섞인 verify 종료 1, 고아 아님 거부 줄, 고아 지우기 줄·다시 목록, 없는 고아 NotFound, Content가 파일이면 검색 실패, 잠금 Busy·잠금 없는 목록(Windows), 사용법 다섯·도움말 둘·프로젝트 없음 둘 | C# 20개 |
| 조합(14 완료 조건 2) | 기록 상태(없음·소유·참조) × owner 상태(있음·없음·기록 없음·참조 기록·기록 읽지 못함·패키지 읽지 못함·같은 id 둘의 조합) × 다른 진단(없음·id를 모르는 진단) × 출력 목록(적음·적지 않음) × 연산(판정·목록·검증·지우기·캐시) | 위 시험 |

#### 14.1.42 Coverage(A2e4a)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetOrphan,AssetIndex -Sources Engine/Source/Developer/AssetDatabase/Private`, 명령은 C# 시험 구동기.

| 범위 | 줄 | 분기 |
|---|---|---|
| `AssetIndex::OrphanReason`·`OrphanReasonWith`·`MakeOrphanFacts`·`Orphans`·Verify 고아 진단·진단 줄, 요약 출력 목록(검색·캐시 재사용) | 모두 닿음 | 모두 닿음 |
| `AssetIndexCache.cpp` 출력 목록 읽기·쓰기 | 모두 닿음 | 모두 닿음 |
| `AssetService::DeleteOrphan` | 모두 닿음 | 모두 닿음 |
| `RunAssetOrphansListCommand`·verify 종료 코드·`RunDelete`(asset-delete·asset-orphans-delete 공용) | 할당·출력 실패 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `RunAssetOrphansListCommand` 캐시 적재·Orphans·줄 서식 할당 실패, 검색의 할당 실패 | 실패 | 프로세스 시험은 명령 allocator에 실패를 주입할 수 없다(14.1.26과 같다 — 같은 경로는 C++ 할당 실패 시험이 돈다) |
| 목록·verify·지우기 줄 쓰기 실패 | 실패 | 14.1.36과 같다 |

리뷰: 1차 미충족(필수 둘 — 판정할 수 없는 경우를 고아로 단정, 같은 id owner 찾기의 O(n²)). `Undetermined`와 id 범위 이진 검색으로 고쳤고, 권장(같은 서비스에서 owner를 지운 뒤 출력 판정 — 시험, `TakeOrphans` 뒤 빈 줄)도 반영했다. 판정 불가는 목록에 나오지 않으므로 `unknown` 표기는 두지 않는다. 기록을 읽지 못한 진단 판정은 칸(ImportRecord)만 본다 — 그 칸의 진단은 UnsupportedVersion·CorruptPackage뿐이라 코드 대조는 닿을 수 없는 분기였다.

#### 14.1.43 요구 대조표(A2e4b — 같은 id 고치기)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetFixIdTests.cpp`(6개)와 C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-fix-duplicate-id" 항목 17개(Windows 전용 1개)다. `Close`를 `OpenCachePath`·`DiscardOpen`으로 나눈 것은 기존 삭제 시험(`AssetDelete_*`)이 그대로 돈다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 다시 쓰기(7.1-17 A2e4b) | 헤더 id 새 id, bulk·값 그대로(새 id로 열어 읽음), 의존(Hard Base)·hook 출력(Tags·Build — hook이 받은 id는 새 id), 원본 파일 byte 그대로, 색인이 새 검색과 같고 같은 id 진단이 사라짐(옛 id·새 id 하나씩), 이번에 연 패키지(사본·의존 Base·Derived) 모두 닫힘·전에 열린 패키지(Hero·Base)는 그대로, 고친 뒤 원본도 id로 열림, 자기 Soft 참조는 새 id(자기 의존 아님), 같은 id 셋이면 남은 경로 둘(경로 키 순)·셋째를 고치면 옛 id 하나, NotDurable 경고(asset = 새 id), 경고 목록 없이도(Hard 의존이 있는 Villain 사본) | `AssetFixId_RewritesCopy` |
| ImportRecord·고아 경고(7.1-17 A2e4b) | 소유 기록: owner·자기 출력만 새 id, 형제 출력 뺌, 임포터·원본 그대로. 남는 같은 id 파일이 형제를 적음(대개의 복사) — 경고·고아 없음, 적지 않음(우연한 충돌) — 형제 OwnerNotImporter 경고(asset·related = 옛 id·detail)와 `Orphans` 한 항목, 남는 파일의 기록을 읽지 못함 — Undetermined라 경고·고아 없음. 참조 기록: byte 그대로(owner Base), 고친 패키지 NotListed 경고와 고아 한 항목. 고치기 전 고아 없음, 경고 목록 없이도 성공 | `AssetFixId_ImportRecords` |
| Hard 순환(7.1-17 A2e4b) | CycleA 사본을 고치면 열기가 CycleB까지 열고 묶음째 닫힘(둘 다·새 id 열리지 않음), 고친 파일의 Hard 의존 CycleB 그대로, 고친 뒤 CycleB를 열면 남은 CycleA, 고친 파일도 새 id로 열림 | `AssetFixId_HardCycle` |
| 서비스 거부(7.1-17 A2e4b) | Initialize 전·잠금 아님 InvalidArgument, 경로 규칙 InvalidPath, 없음 NotFound(옛 id·경로 비움), 별칭 경로는 풀지 않음 NotFound, 같은 경로 키 둘 DuplicatePath, 같은 id 아님 InvalidArgument(asset·결과 옛 id·경로·남은 것 없음), 읽기 전용 NewerVersion / Unsupported, 미해결 import MissingDependency(related·닫힘·파일 그대로) — 모두 파일·색인 그대로 | `AssetFixId_RejectsAndFailures` |
| 집합 거부(7.1-17 A2e4b) | null 새 id·잠금 아님·hook 없음·Initialize 전 InvalidArgument, 파일 없음 NotFound, 같은 id 아님 InvalidArgument(asset), 새 id가 색인에 있음·색인에 같은 id 둘·열린 저장하지 않은 패키지 DuplicateAssetId(related·asset = 옛 id), 만들기 초기화 안 Busy, 옛 id가 열려 있음(먼저 연 Base에 밖에서 사본이 생김) — 사본·원본 모두 Busy | `AssetFixId_RejectsAndFailures` |
| 실패(7.1-17 A2e4b) | hook 실패(Busy — 아무것도 쓰지 않음·닫힘), 경고 자리 할당 실패, 임시 파일 만들기·쓰기·Flush·Close·교체 실패(임시 파일 없음·asset = 옛 id·닫힘·파일·색인 그대로) | `AssetFixId_RejectsAndFailures` |
| 할당 실패(7.1-17 A2e4b) | 소유 기록 Leaf 사본(형제 고아 경고 경로)·Hard 순환 사본(이번에 연 것 닫기의 중간 Discard 실패 포함)·Hero 사본(bulk·의존·hook)의 모든 할당 실패가 OutOfMemory·파일·색인 그대로·옛 id와 이번에 연 의존 닫힘·먼저 연 Derived는 성공·실패 모두 열린 채(열린 id 기록의 할당 실패 포함, GC 표 실패는 Registry가 삼킨 성공) | `AssetFixId_AllocationFailures` |
| 실제 디스크(7.1-17 A2e4b) | 디스크의 Hero 사본을 고친 뒤 새 id로 열어 bulk 읽기, 원본도 열림 | `AssetFixId_RealDisk` |
| 명령(13, 7.1-17 A2e4b) | 고치기 전 verify DuplicateAssetId, 출력 두 줄(옛 id → 새 id·남은 원본), 같은 id 셋에서 하나 고치기(still shared by 둘)·마지막 사본 고치기(원본으로 풀림), 고친 뒤 verify 깨끗(종료 0), 원본을 참조하는 AnimationClip 그대로, 목록의 id 둘, 같은 id 아님 InvalidArgument 줄, 없음 NotFound, 경로 규칙 InvalidPath, 잠금 Busy(Windows), 사용법 셋·도움말·프로젝트 없음. OrphanedOutput 경고 줄은 명령이 임포트 기록을 만들 수 없어(A2f 전 — 시험 fixture는 명령에서 편집용으로 열리지 않는다) C++ 시험이 경고를 단언한다(줄 서식은 다른 경고와 같은 공용 경로) | C# 15개 |
| 조합(14 완료 조건 2) | 기록 상태(없음·소유·참조) × 남는 같은 id 파일(같은 기록·기록 없음·기록 읽지 못함) × 의존 모양(없음·Hard·Hard 순환·bulk) × 경고 목록(있음·없음) × 결과(성공·거부·IO 실패·할당 실패) | 위 시험 |

#### 14.1.44 Coverage(A2e4b)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter Asset -Sources Engine/Source/Developer/AssetDatabase/Private`(329개 통과), 명령은 C# 시험 구동기(378 PASS).

| 범위 | 줄 | 분기 |
|---|---|---|
| `EditorPackageSet::FixDuplicateId`·`CloseOpened`·`DiscardOpen`·`OpenCachePath`·`Close` | 모두 닿음 | 모두 닿음 |
| `WriteAndReplace` ID 수정 모드(이번에 연 것 닫기·디렉터리 만들기 생략) | 기존 4곳 제외 모두 | 같음 |
| `AssetService::FixDuplicateId` | 1곳 제외 모두 | 같음 |
| `RunAssetFixDuplicateIdCommand` | 출력 실패 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `AssetService::FixDuplicateId` 새 id 발급 실패 | 실패 | OS 보안 난수 실패(14.1.22와 같다) |
| `WriteAndReplace` 경고 자리·임시 이름 guid·다시 읽기 열기 실패 | 실패 | 14.1.40과 같다(이번 단계가 바꾸지 않은 줄 — ID 수정은 경고 자리를 미리 넉넉히 잡는다) |
| `RunAssetFixDuplicateIdCommand` 경고 줄·결과 줄 서식·쓰기 실패 | 실패 | 14.1.36과 같다 |

리뷰: 충족(필수 없음). 권장 넷을 반영했다 — 이번 열기가 연 의존이 열린 채 남음과 묶음 계산의 고정점 반복(참조자 닫힘 `CloseGroup` → 열기 전 기록 밖을 닫는 `CloseOpened`, 시험에서 닫힘·그대로 단언), 중간 Discard 실패 뒤 남은 참조의 상태(주석·7.1-17), 남은 같은 id가 둘 이상일 때의 결과 줄(still shared by).

#### 14.1.45 요구 대조표(A2f1a — 임포트 기반)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetImportFoundationTests.cpp`(6개), 기존 시험의 빈 루트 원본을 규칙에 맞게 고친 것(`ImportRecord_*`·`AssetIndex_ImportRecords`·`AssetIndexCache_*`·`AssetOrphan_*`·`EditorPackage*`·`AssetDuplicate_*`·`AssetFixId_*` — 루트 `Raw`나 절대 경로), `AssetPackage_DiagnosticNames`(마지막 코드 UnknownProperty), Foundation `StringViewSliceBytesBorrowsTheRange`·`StringViewSliceBytesOutOfRangeIsFatal`(자식 프로세스)이다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 루트 표 읽기(4.5절, 7.1-18) | 파일 없음 → Project 하나(프로젝트 루트), 상대·`.`·끝 `/`·`..`·절대(OS 구분자) 정규화, 이름 byte 순 정렬, Project 없으면 더함·있으면 덮어씀, 빈 roots, 다시 읽으면 바꿈 | `AssetSourceRoots_Load` |
| 루트 표 거부(7.1-18) | 대소문자만 다른 두 이름(`Art`·`ART`, `project`와 기본 Project), 장치 이름공간 경로 Unsupported(Windows), JSON 문법(줄·열)·최상위 배열·빈 객체·다른 키·키 둘·roots 배열·값 숫자·빈 값·이름 규칙(빈·`-`·`{`·`:`·`[`·`` ` ``·`@`·`/`·257 byte, 256 byte·`My_Root9` 받음)·중복 키, Windows 드라이브 상대·현재 드라이브 루트, 루트 위로 벗어나는 `..`, 루트 256개 받음·257개 CapacityExceeded, 파일 1 MiB 초과 CapacityExceeded, 읽기 IO(진단 None), 진단 없이도, 할당 실패(진단 None·빈 표) — 모두 실패하면 빈 표 | `AssetSourceRoots_RejectsUntrustedInput` |
| 기록 위치(4.5절, 7.1-18) | 가장 긴 접두(Deep > Art), 성분 경계(SourceArtX), 대소문자 무시(원래 철자 유지), `.`·`..`, 루트 자신은 위 루트 아래, 드라이브 루트(끝 `/`), 연속 `/`로 시작하는 루트(UNC·POSIX 축약), 밖의 절대 경로(absolute)·UNC(Windows), 드라이브 상대·현재 드라이브 루트·상대·빈·NUL·루트뿐인 경로 InvalidArgument, 장치 이름공간(`\\?\C:\…`·`\\?\UNC\…`) Unsupported(Windows), 기록 1024 byte 받음·1025 CapacityExceeded, 할당 실패 | `AssetSourceRoots_LocateAndResolve` |
| 풀기(4.5절, 7.1-18) | 루트 + 상대, 빈 루트 + 절대, 드라이브 루트는 `/`를 겹치지 않음, 모르는 루트 NotFound, `..`·빈 루트 상대·루트 이름 `/` InvalidArgument, 할당 실패 | `AssetSourceRoots_LocateAndResolve` |
| 원본 읽기(7.1-18) | 내용·SHA-256, 빈 파일, 상한과 같은 크기 받음·초과 InvalidSource / CapacityExceeded, 없음 MissingSource / NotFound, 읽기 IO InvalidSource / 그 오류, 할당 실패(진단 None) | `AssetImportSource_Read` |
| 원본 위치 규칙(5.7절 ImportRecord 배치, 7.1-18) | 루트 있음: 상대(드라이브 모양도 상대 성분), `/` 시작 거부. 빈 루트: POSIX·UNC(서버·공유 뒤 성분)·드라이브(대소문자) 받음, `//server/share`·`//server`·숫자·`[`·`{` 드라이브·드라이브 상대·`C:/`·`/`·상대·`..`·`//`·끝 `/`·`\`·빈 거부, 루트에 `/` 거부 | `ImportRecord_SourceLocations` |
| 임포터 표(7.1-18) | 등록·찾기(id, 이름 대소문자 무시 — 숫자·밑줄, 확장자 대소문자 무시), 기본 확장자 없는 임포터(이름으로만), 같은 id·이름(대소문자만 다름)·기본 확장자 AlreadyExists, 겹치지 않는 확장자, 이동·Release·이동 대입·빈 토큰 대입·토큰 소멸이 해제 | `AssetImporterTable_RegisterAndFind` |
| 임포터 규칙(7.1-18) | null id, 이름(빈·`-`·공백·`{`·`:`·`[`·`` ` ``·`.`·257 byte, 256 byte 받음), 확장자(대문자·빈·`.`·중복·`_`·`:`·`{`·`/`·17 byte, 32개 받음·33개 거부), 역할(중복·빈·`/`·`-`, 257개 거부), 주 출력(없음 NotFound·struct·에셋 아닌 클래스·추상 Asset InvalidArgument), 옵션(없음 NotFound·클래스 InvalidArgument·주 출력 속성과 도구 이름 충돌(대소문자만 다름)·예약 이름 `source`), 할당 실패 | `AssetImporterTable_RegisterAndFind` |
| 진단 등록(4.6절, 7.1-18) | 여덟 코드 이름·유일·Unknown 경계 | `AssetPackage_DiagnosticNames` |
| Core `StringView::SliceBytes`(CoreDetailedSpec T01) | 앞·가운데·끝·빈·전체, 원본 storage 빌림, 빈 원본 (0, 0), constexpr, 범위 밖(offset·count·overflow) Fatal | Foundation 두 시험 |
| 조합(14 완료 조건 2) | 루트 모양(상대·절대·드라이브 루트·UNC·`..`) × 경로 모양(아래·경계·대소문자·밖·루트 자신·상대·NUL) × 연산(읽기·기록·풀기·원본 읽기) × 실패(형식·상한·IO·할당), 임포터 정보 칸마다 위반 × 표 상태(빈·찬) | 위 시험 |

#### 14.1.46 Coverage(A2f1a)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetSourceRoots,AssetImportSource,ImportRecord,AssetImporterTable,AssetAlias -Sources Engine/Source/Developer/AssetDatabase/Private`.

| 범위 | 줄 | 분기 |
|---|---|---|
| `ImportRecord.cpp`(원본 위치 규칙 포함) | 모두 닿음 | 모두 닿음 |
| `AssetImporter.cpp` | 모두 닿음 | 모두 닿음 |
| `AssetSourceRoots.cpp` | 호스트 전용 3줄 제외 모두 | 호스트 전용 제외 모두 |
| `WholeFileRead.cpp`(옮긴 `ReadWhole`) | 크기 조회 실패 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `NormalizeAbsolute`의 `PosixRoot` 갈래, Windows가 아닌 호스트의 마무리(`/` 바꾸기 없음) | Windows 호스트에서 | 호스트 문법 갈래다 — POSIX 호스트(Linux·macOS CI)는 이 갈래만 돈다(Windows 갈래가 닿지 않는다). switch case 표시는 llvm-cov의 case 분기 형식(False 0) |
| `ReadWhole` 크기 조회 실패 | 실패 | 열린 핸들의 크기 조회만 실패시킬 수 없다(메모리 파일 시스템에 그 고장 주입이 없다 — 별칭 표 A2e1과 같은 줄을 옮겼다) |

처음 구현의 주 출력 종류 검사(`Kind() != Class`)는 `Acquire`가 클래스만 주므로 닿을 수 없어 지웠다.

#### 14.1.47 요구 대조표(A2f1b — 새 임포트)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetImportTests.cpp`(7개 — 시험 임포터 `AssetImportTestSupport.h`), `Engine/Source/Developer/AssetCommandlets/Tests/AssetImportCommandletTests.cpp`(3개 — 명령 함수를 프로세스 안에서), `AssetImporterTable_RegisterAndFind`(타입 lease 조회 더함), C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-import" 항목 10개(제품 실행 파일 — 임포터 없음)다. 에셋 명령 구현은 동작 불변 이동 커밋(d596e9aa)으로 Developer 모듈 `AssetCommandlets`에 옮겼다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 리뷰 반영(7.1-18 A2f1b) | 주 경로 점유를 원본 읽기 전에(별칭 — 진단 인자 없이도·색인·저장하지 않은 새 패키지, 디스크에만 있는 파일은 Create), 실패한 임포트는 이번 경고를 남기지 않음, 할당 실패 장면에 저장하지 않은 새 패키지 | `AssetImport_Rejects`·`AssetImport_AllocationFailures` |
| 원본 하나(7.1-18 A2f1b) | 주 출력 파일·헤더 id, 소유 기록(임포터 id·이름·버전, 원본 역할·루트 Project·상대 경로·SHA-256·byte 수, 옵션 Record schema·version·JSON, 출력 하나), 옵션(scale)·설정(note) 나누기, 값·라벨·bulk, 색인 요약, 임포터 한 번, 경고 없음, 이름으로 고르기(대소문자 무시)·경고 목록 없이·옵션 기본값 | `AssetImport_NewAsset` |
| 추가 출력·경고(7.1-18 A2f1b) | Output 순서·`<path>_<key>`, 참조 기록(owner)·ImportOwner 의존, 소유 기록 출력 목록(key 순·타입), 출력 사이 Hard 참조, 고아 없음, ImportWarning(임포터 경고 코드·detail·asset), AbsoluteSourcePath(Project를 옮겨 — 절대 경로 기록), 경고 목록 없이도 | `AssetImport_MultipleOutputs` |
| 역할(7.1-18 A2f1b) | 역할 순 기록·값의 합·옵션 없는 기록, 역할 빠짐·넘침·같은 역할 둘·모르는 역할 InvalidImportOption, 옵션 없는 임포터의 옵션 이름 UnknownProperty(detail) | `AssetImport_Roles` |
| 거부(7.1-18 A2f1b) | Initialize 전·잠금 아님·원본 없음, 경로 규칙, 별칭(related)·색인 점유, 이름 없음 NotFound, 확장자 모름·없음·섞임(detail)·다른 임포터 둘·경로 해석 오류, 같은 임포터 원본 둘(역할), 모르는 이름(detail·`.`·`[`), 옵션 값 InvalidImportOption(진단), 설정 값(진단 None), 상대 원본·없는 원본 MissingSource(detail)·루트 표 손상, 임포터 실패 셋(Fail 진단·진단 없음·출력 뒤), 주 출력 없음, 출력 규칙(주 출력 타입·key 글자·길이 65·같은 key·추상·에셋 아님·미등록 NotFound·struct·점유·별칭 related·남의 AddBulk·경고 아닌 Warn·출력 1025개), 집합 SetImportRecord(열리지 않음 NotFound·Initialize 전), 임포터 표 없는 서비스 — 모두 파일·색인·열린 패키지 그대로 | `AssetImport_Rejects` |
| 저장 실패(7.1-18 A2f1b) | 첫 출력 저장 실패(아무것도 안 씀)·주 출력 저장 실패(먼저 쓴 출력 지움·색인에서 뺌), 지우기 실패 OrphanNotDeleted(asset)·고아로 남음(OwnerMissing), 경고 목록 없이도 | `AssetImport_SaveFailures` |
| 할당 실패(7.1-18 A2f1b) | 추가 출력·옵션·설정·bulk·임포터 경고·절대 경로 경고·열린 패키지가 있는 임포트의 모든 할당 실패 OutOfMemory·파일·색인·열린 패키지 그대로(GC 표 실패는 삼킨 성공) | `AssetImport_AllocationFailures` |
| 실제 디스크(7.1-18 A2f1b) | 디스크 원본 임포트·다른 서비스로 다시 열어 bulk | `AssetImport_RealDisk` |
| 명령(13, 7.1-18 A2f1b — 프로세스 안) | 결과 줄·출력 줄·경고 줄·패키지·색인 캐시, 역할 원본(`--source.<역할>=`·`--importer=`), 상대 원본 경로, 실패 줄(MissingSource·옵션 값·설정 대입 진단 줄·임포터 없음·등록 실패·타입 모듈 시작 실패의 모듈 이름·절대 원본·루트 위로 벗어나는 상대 경로 InvalidSource·`--erase` UnknownProperty), 등록 hook 할당 실패와 출력 쓰기 실패(표준 출력·경고 줄)는 실행 오류, 사용법 17가지·도움말 | `AssetImportCommand_*` |
| 명령(13 — 제품 실행 파일) | 임포터 없음·모르는 이름 UnsupportedSourceFormat, 사용법 여섯·도움말·프로젝트 없음 | C# 10개 |
| 조합(14 완료 조건 2) | 원본 수(하나·역할 둘) × 고르기(이름·확장자) × 출력(주·추가·참조) × 대입(옵션·설정·둘 다·없음) × 경고(있음·없음·목록 없음) × 결과(성공·거부·임포터 실패·저장 실패·할당 실패) × 실행(서비스·명령 함수·제품 실행 파일) | 위 시험 |

#### 14.1.48 Coverage(A2f1b)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetImport -Sources Engine/Source/Developer/AssetDatabase/Private`, `-Filter AssetImportCommand -Sources Engine/Source/Developer/AssetCommandlets/Private`.

| 범위 | 줄 | 분기 |
|---|---|---|
| `AssetImporter.cpp`(표 — lease 포함) | 모두 닿음 | 모두 닿음 |
| `AssetServiceImport.cpp` | 아래 제외 모두 | 같음 |
| `EditorPackageSet::SetImportRecord` | 모두 닿음 | 모두 닿음 |
| `RunAssetImportCommand`·`ParseImport`·세션 hook | 아래 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| 주 출력·추가 출력 id 발급 실패 | 실패 | OS 보안 난수 실패(14.1.22와 같다) |
| 옵션·설정 대입 묶음 `Publish` 실패 | 실패 | Prepare와 Publish 사이에 대상이 바뀌거나 죽을 수 없다(같은 스레드, 사이에 다른 일이 없다) — 값 단위는 무할당으로 맞바꾼다(ReflectionText 10.2절). 검사는 API 계약대로 둔다 |
| 명령의 인자·경로·줄 서식 할당 실패, 작업 디렉터리 조회 실패 | 실패 | 명령 allocator는 세션 전체(TypeRegistry 등)에 쓰여 그 실패는 서비스·세션 할당 실패 시험이 덮는다(14.1.26과 같은 줄 서식 근거), 작업 디렉터리 조회는 OS 실패 |

#### 14.1.49 요구 대조표(A2f2a — 재임포트)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetImportTests.cpp`의 `AssetReimport_*` 4개(기록은 실제 임포트로 만들거나 `PutCrafted`로 손으로 만든다 — 주 출력 Leaf·기록 출력 Villain), `Engine/Source/Developer/AssetCommandlets/Tests/AssetImportCommandletTests.cpp`의 `AssetReimportCommand_Reimports`, C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-reimport" 항목 7개(제품 실행 파일)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 리뷰 반영(7.1-18 A2f2a) | 같은 서비스에서 이어서(재임포트 → 재임포트, 새 임포트 → 재임포트 — 성공 뒤 열린 패키지 없음), 기록 출력 타입이 대표 타입과 다름(주 출력·출력) TypeMismatch, 색인 뒤 밖에서 바뀐 기록(출력 목록·임포터 버전·임포터) ExternalModification, 옵션 사례별 오류 코드, 할당 실패의 삼킨 실패(GC 표·성공 뒤 닫기) | `AssetReimport_InPlace`·`AssetReimport_Rejects`·`AssetReimport_AllocationFailures`·`AssetImport_AllocationFailures` |
| 제자리 갱신(7.1-18 A2f2) | 같은 id·이동한 출력도 색인 경로로, 기존 출력 key는 같은 패키지·새 key는 새 출력(`<path>_<key>`), 다른 패키지의 참조가 그대로 풀림, 기록 원본 SHA-256·byte 수·임포터 버전 새로, 문맥 `IsReimport`, 기록 옵션 유지·명시 대입만 바꿈, `--source=`·`--source.<역할>=` 바꾸기(원본 둘 중 하나), 청하지 않은 기존 출력 OrphanedOutput(asset·related)·고아, 지워진 기록 출력은 새 id 새 출력, 경고 목록 없이도 | `AssetReimport_InPlace`·`AssetReimport_Rejects`(역할 하나 바꾸기) |
| 거부(7.1-18 A2f2, 나눔과 고침) | Initialize 전·잠금 아님·경로 규칙·없음·같은 경로 키 둘, 기록 없음·참조 기록(related), 임포터 없음·임포터 표 없음·기록 버전이 더 새, 주 출력·출력 열림 Busy(dirty 아니어도), 출력 다른 owner·읽기 전용·같은 id 둘, 주 출력 같은 id 둘·읽기 전용, 역할 다름·원본 수 다름·모르는 루트·원본 없음 MissingSource·모르는 바꾸기 역할·원본 둘인 기록에 `--source=`, 옵션 Record 여섯(schema·버전·wire·JSON·모르는 필드·필드 1 없음)·옵션 없는 임포터에 기록, 손상된 출력 파일, 주 출력 없음, 미해결 import MissingDependency(related), 타입이 다른 요청 TypeMismatch, 실패 진단 인자 없이도 — 모두 파일·색인·열린 패키지 그대로 | `AssetReimport_Rejects` |
| 중간 실패(7.1-18 A2f2) | 주 출력 저장 실패: 새로 만든 출력 파일 지움, 먼저 저장한 기존 출력은 새 내용, 주 출력 파일 그대로(옛 SHA), 같은 재임포트로 수렴 | `AssetReimport_PartialFailure` |
| 할당 실패(7.1-18 A2f2) | 기존 출력·새 출력·고아 출력·옵션 기록·명시 대입·무관한 열린 패키지가 있는 재임포트의 모든 할당 실패 OutOfMemory·주 출력 파일·열린 패키지 그대로(GC 표 실패는 삼킨 성공 — 처음 상태로 되돌려 이어 감) | `AssetReimport_AllocationFailures` |
| 명령(13, 7.1-18 A2f2 — 프로세스 안) | 결과 줄·출력 줄(기존 출력 포함)·임포터 재임포트 여부, `--source=` 바꾸기와 옵션, 실패 줄(기록 없음 InvalidArgument·원본 없음 MissingSource), 사용법 일곱·도움말 | `AssetReimportCommand_Reimports` |
| 명령(13 — 제품 실행 파일) | 기록 없음 InvalidArgument, 사용법 넷·도움말·프로젝트 없음 | C# 7개 |
| 조합(14 완료 조건 2) | 원본(기록 그대로·`--source=`·역할 하나 바꾸기) × 출력(기존 갱신·새 key·지워져 다시 만듦·고아) × 대입(기록 옵션·명시 옵션·설정·없음) × 경고(있음·없음·목록 없음) × 결과(성공·거부·임포터 실패·저장 실패·할당 실패) × 실행(서비스·명령 함수·제품 실행 파일) | 위 시험 |

#### 14.1.50 Coverage(A2f2a)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetReimport,AssetImport -Sources Engine/Source/Developer/AssetDatabase/Private,Engine/Source/Developer/AssetCommandlets/Private`. `AssetServiceImport.cpp` 줄 1053/1063·분기 506/518.

| 범위 | 줄 | 분기 |
|---|---|---|
| `AssetServiceImport.cpp`(`Reimport`·`RunImport`·문맥 기존 출력·기록 옵션) | 아래 제외 모두 | 같음 |
| `RunAssetReimportCommand`·`ParseImport`(재임포트 분기) | 아래 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| 새 출력·주 출력 id 발급 실패 | 실패 | OS 보안 난수 실패(14.1.48과 같다) |
| 옵션·설정 대입 묶음 `Publish` 실패(새 임포트·재임포트·기록 옵션) | 실패 | 14.1.48과 같다 — Prepare와 Publish 사이에 대상이 바뀌거나 죽을 수 없다 |
| 기록 옵션 `Find`의 NotFound 아닌 실패 | 실패 | Find는 없음(NotFound)과 예산 소진만 실패한다. 기본 한도로 연 Record는 필드 4096개 이하라 연산 예산(1M)을 Open·Find가 다 쓸 수 없다. 검사는 API 계약대로 둔다 |
| 명령의 원본 목록·경로·줄 서식 할당 실패, 작업 디렉터리 조회 실패 | 실패 | 14.1.48과 같다 |

#### 14.1.51 요구 대조표(A2f2b — 임포트 상태)

시험은 EngineTests `Engine/Source/Developer/AssetDatabase/Tests/AssetImportTests.cpp`의 `AssetImportStatus_*` 4개, `Engine/Source/Developer/AssetCommandlets/Tests/AssetImportCommandletTests.cpp`의 `AssetInfoImportCommand_Reports`, C# `Programs/Rocks.Commandlet.Tests/Program.cs`의 "asset-info import"·"asset-info usage"·"asset-info help mentions import" 항목 5개다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 소유 기록 상태(7.1-18 A2f2b) | 임포터 id·이름·버전·current, 원본 역할·루트·상대 경로·current(error None), 경로·AssetId 대상 같은 결과, 패키지를 열지 않음(열린 패키지 0·같은 서비스 재임포트 성공) | `AssetImportStatus_Imported` |
| 원본 상태 출력(7.1-18 A2f2b 리뷰 반영) | 원인 오류의 줄 끝 ` (<오류>)`·JSON `error`(없으면 null) | `AssetInfoImportCommand_Reports` |
| 원본 상태(7.1-18 A2f2b) | 같은 크기 다른 내용·다른 크기·기록 byte 수와 다름 SourceChanged, 빈 파일(크기 같음·SHA-256 다름) SourceChanged, 없음 MissingSource(error None), 모르는 루트 MissingSource(NotFound), 디렉터리·열기 사용 중·크기 조회 실패·읽기 실패 InvalidSource(error), 절대 경로 원본(빈 루트), 원본 둘(역할 순), 대조 함수의 빈 버퍼·작은 버퍼 여러 번 읽기·다른 다이제스트 | `AssetImportStatus_Imported`·`AssetImportStatus_Crafted` |
| 임포터 상태(7.1-18 A2f2b) | 등록 버전이 더 새 ImporterOutdated, 기록 버전이 더 새 NewerVersion, 모르는 임포터·임포터 표 없음 UnsupportedSourceFormat(원본 상태는 그대로) | `AssetImportStatus_Crafted`·`AssetImportStatus_Imported` |
| 참조 기록(7.1-18 A2f2b) | owner·ownerPath(색인), owner가 색인에 없음·같은 id 둘이면 빈 경로, 원본 루트 표가 깨져도 나옴 | `AssetImportStatus_Imported`·`AssetImportStatus_Crafted`·`AssetImportStatus_Rejects` |
| 대상(7.1-18 A2f2b) | 별칭으로 푼 대상(경고 AliasResolved), 별칭 대상이 색인에 없음 NotFound(asset) | `AssetImportStatus_Imported`·`AssetImportStatus_Rejects` |
| 거부(7.1-18 A2f2b) | Initialize 전, 경로 규칙·AssetId 아님·null id InvalidPath, 없음(경로·id) NotFound, 같은 경로 키 둘 DuplicatePath, 같은 id 둘 DuplicateAssetId(asset), 기록 없음 InvalidArgument(asset), 더 새 스키마 기록 NewerVersion / Unsupported, 필드 빠진 기록 CorruptPackage(칸 ImportRecord), 색인 뒤 사라진 파일 MissingFile, 다른 패키지 ExternalModification / StaleHandle, 열기 사용 중 Busy, 잘린 파일 CorruptPackage / InvalidArgument(asset), 원본 루트 표 손상 InvalidSource / InvalidArgument(detail 줄·열), 진단 인자 없이도 — 모두 열린 패키지 0 | `AssetImportStatus_Rejects` |
| 할당 실패(7.1-18 A2f2b) | 소유 기록(원본 둘)·참조 기록의 모든 할당 실패 OutOfMemory·진단 None, 성공 결과의 모든 필드가 기준과 같음 | `AssetImportStatus_AllocationFailures` |
| 명령(13, 7.1-18 A2f2b — 프로세스 안) | 소유 기록 줄(정확한 두 줄)·JSON(이름·버전·상대 경로·state), owner 줄·JSON ownerPath, 원본을 바꾼 뒤 SourceChanged 줄, 원본 자리가 디렉터리면 InvalidSource와 원인(줄·JSON), 임포터 표 없는 세션 UnsupportedSourceFormat(종료 0), 별칭 경고 줄, owner가 색인에 없으면 경로 자리에 id·JSON null, 실패 줄(NotFound), 출력·경고 줄 쓰기 실패는 실행 오류, 사용법(`--import --all`·`--import` 두 번) | `AssetInfoImportCommand_Reports` |
| 명령(13 — 제품 실행 파일) | 기록 없음 InvalidArgument, 없는 경로 NotFound(`--json`), 사용법 둘, 도움말에 `--import` | C# 5개 |
| 조합(14 완료 조건 2) | 기록(소유·참조·없음·읽지 못함) × 원본 상태(current·SourceChanged 크기·내용·MissingSource 파일·루트·InvalidSource 넷) × 임포터 상태(넷) × 대상(경로·id·별칭) × 출력(줄·JSON) × 실행(서비스·명령 함수·제품 실행 파일) | 위 시험 |

#### 14.1.52 Coverage(A2f2b)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter AssetImportStatus,AssetInfoImportCommand -Sources Engine/Source/Developer/AssetDatabase/Private,Engine/Source/Developer/AssetCommandlets/Private`. `AssetServiceImportStatus.cpp` 줄 178/178·분기 92/94.

| 범위 | 줄 | 분기 |
|---|---|---|
| `AssetServiceImportStatus.cpp` | 모두 닿음 | 아래 제외 모두 |
| `CheckImportSource` | 모두 닿음 | 모두 닿음 |
| `RunImportStatus`·`AppendImportLines`·`AppendImportJson`·`--import` 해석 | 아래 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `ImportStatus`의 원본 Resolve 오류 판정 | InvalidArgument(위치 규칙 위반) 갈래 | 기록을 읽을 때(`ReadImportRecord`) 원본마다 같은 규칙(`ImportValidRoot`·`ImportValidSourcePath`)을 이미 통과했다 — Resolve는 모르는 루트(NotFound)와 할당 실패만 낸다. Resolve 계약대로 판정은 둔다 |
| 명령의 줄·JSON 서식 실패(`TryAppendFormat`·`JsonWriter`) | 실패 | 출력 버퍼는 명령 allocator로 자라고 그 실패는 할당 실패뿐이다 — 14.1.48과 같다(명령 allocator는 세션 전체에 쓰인다) |

#### 14.1.53 요구 대조표(A3a — 키·Record·파일)

시험은 EngineTests `Engine/Source/Developer/AssetNugget/Tests/`(`NuggetKey_*` 8·`NuggetReflected_*` 7·`NuggetRecord_*` 7·`NuggetFile_*` 7)와 FoundationTests `Core/Source/Runtime/ReflectionText/Tests/TextVisitTests.cpp`(`TextVisit_*` 8)다. 리플렉션 기록의 fixture는 `AssetTestFixtures/NuggetAssets.h`(KeyedSettings — 키에 쓸 수 있는 모든 값 종류)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 순회(RE 10.6) | 이벤트 문법(필드 순서·단일 값·Optional·struct·배열 셋·byte 배열·Set·Map 저장 순서·InstancedStruct), 대상 셋(살아 있는 객체·staged·struct 값) 같은 이벤트, Edit·Report 제외, 거부(Opaque·다른 Name pool·잘못된 codec 옵션·struct 값 대상 lease·storage), 명시한 같은 pool, 경로 깊이 상한 Unsupported, visitor 오류가 그 이벤트에서 멈춤(모든 n — Instanced·byte 배열 포함), 할당 실패 | `TextVisit_*` |
| 키 태그(9.1 결정 2) | 태그 번호 고정·인코딩 golden(정수 LE·Guid·길이 접두·Raw), 해시 = byte의 SHA-256, NaN payload·부호 단일화·-0 구분·무한대, 경계(None·길이 접두·태그 종류), 오류 상태(UTF-8 String·Field·Fail 첫 오류·None 무시·buffer Finish·두 번 Finish·끝난 뒤 무시·buffer 할당 실패), 추적 줄(모든 기록·오류 뒤 없음·키 불변) | `NuggetKey_EncodingGolden`·`NuggetKey_FloatsAreBitPatterns`·`NuggetKey_BoundariesAreUnambiguous`·`NuggetKey_ErrorStates`·`NuggetKey_TraceLines` |
| 공통 머리·세대(9.1 결정 2·4) | 순서 golden(형식·버전·엔진 세대·프로젝트 세대·처리기·버전·결정적 수학·툴체인), 프로젝트 세대·처리기·버전 하나씩 바꾸면 키가 바뀜, 인코더 기록 | `NuggetKey_HeaderAndEncoder` |
| SCC(9.1 결정 3) | 입력 순서 무관, 튜플 중복 합침, 빈 구성원·튜플 구성원이 구성원 아님·의존이 구성원 InvalidArgument, 구성원 사이 AssetId의 의존은 밖, 지역 키·외부 key(Merkle)·역할·같은 접두 역할·구성원 추가·튜플 빼기로 바뀜, 구성원 하나, 구성원 중복·null 구성원·null 의존·null 소유·유효하지 않은 UTF-8 역할 InvalidArgument, 구성원 key, 할당 실패 | `NuggetKey_SccKeys`·`NuggetKey_SccAllocationFailures` |
| 리플렉션 기록(9.1 결정 2) | 필드마다 변이(정수 폭 넷 포함 31가지 — 서로·기준과 모두 다름), Transient 키 밖, Map·Set·struct 값 Map 순서 무관·값 다르면 다름, 배열 순서는 결과, NaN payload 무관·-0 구분, Optional unset·set(0), Optional<struct>와 InstancedStruct 구분, 필드 이름이 키에 들어감, Name ASCII 소문자 접기, 추적 golden, 거부(Text·객체 참조·이미 실패한 기록기·Name pool 없음·다른 pool 토큰), ClassRef TypeId, 할당 실패 | `NuggetReflected_*` |
| Record(9.1 결정 5) | 왕복·결정적 byte·빈 Record, bucket·namespace 경계(길이 64·65·첫 글자·'z' 뒤 문자·숫자), 정규 형식 규칙 21가지(Rcbk 색인 48 + 8n·IndexHash 포함)·1 MiB CapacityExceeded, TLV 변조(schema·버전 Unsupported·Record 아님·필드 빠짐·순서·wire 여섯·추가 필드·key 크기·행 길이·reserved 셋·이름 규칙·입력 항목 schema·필드 빠짐·key·digest 크기·추가 필드), byte 뒤집기 전부(거부하거나 다른 Record), 논리 digest(저장 표현 여섯 무관·논리 칸 열셋 반영), 입력 순서(길이·같은 길이 내용·key·digest), 할당 실패 | `NuggetRecord_*` |
| `.rnug` v2(9.1 결정 6) | 왕복(머리 golden·Identity·1 MiB 이상 조각 표·Rcbk·값 없는 파일), 머리·Record byte 뒤집기 전부(버전 Unsupported·그 밖 손상), 값 byte 변조는 그 값만, 표 변조·잘림·남는 byte·머리보다 짧음·Record 크기 초과, 해시가 맞는 깨진 Record, 더 새 Record schema Unsupported, 해제 검증(크기·RawHash·Identity rawSize·Rcbk 색인 해시·색인 길이·짧은 색인·원본 크기·손상 블록·헤더, Codec Unsupported, 실패 시 out 길이 불변), index 범위, 계획 거부·붙이기 불일치, 범위 reader 경계, 읽기 IO 오류 전파(모든 ReadAt), 할당 실패 | `NuggetFile_*` |
| 조합(14 완료 조건 2) | 기록 종류(태그 17) × 맥락(root·Map 키·Map 값·Set 원소) × 결과(성공·오류 상태·할당 실패), Record(값 encoding 셋 × 표 유무 × 입력·meta 유무) × 경로(쓰기·읽기·변조·할당 실패) | 위 시험 |

#### 14.1.54 Coverage(A3a)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter NuggetKey,NuggetReflected,NuggetRecord,NuggetFile -Sources Engine/Source/Developer/AssetNugget/Private`(줄 1226/1233·분기 580/589), `-Target Foundation -Executable FoundationTests -Filter TextVisit,TextJson -Sources Core/Source/Runtime/ReflectionText/Private`(`TextVisit.cpp` 줄 186/196·분기 93/98).

| 범위 | 줄 | 분기 |
|---|---|---|
| `NuggetKey.cpp`·`NuggetRecord.cpp`·`NuggetFile.cpp` | 모두 닿음 | 아래 제외 모두 |
| `NuggetReflected.cpp` | 아래 제외 모두 | 같음 |
| `TextVisit.cpp`·`ReadTargetRoot` | 아래 제외 모두 | 같음 |

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `NuggetMemberKey`의 `ROCKS_ASSERT` | 실패 | 고정 영역 문자열(유효한 UTF-8)과 고정 폭 값만 쓰는 해시 기록기라 Finish가 실패할 수 없다(불변식) |
| `nugget_detail::CompareBytes`의 접두 갈래(길이가 다르고 공통 접두가 같음) | 그 갈래 | 모듈 안 호출은 정규 인코딩(태그·길이 접두 — prefix-free)이거나 같은 길이(key·digest·같은 길이 bucket)라 한쪽이 다른 쪽의 접두일 수 없다. 일반 비교 규칙으로 둔다 |
| `KeyVisitor`의 항목 열기(`StartEntry`) 실패 | 실패 | BeginSet·BeginMap이 원소 수만큼 자리를 잡고 순회 문법상 항목 이벤트 수가 그 수와 같다 — 실패는 순회 결함일 때만이고 그때 오류로 멈춘다(조용히 앞 항목에 섞지 않는다) |
| `DecodeNuggetValue`의 Rcbk `TryRead` 할당 실패 갈래 | OutOfMemory | 해제 문맥(디코더)을 `TryCreate`에서 만들고 원본 전체를 destination에 바로 해제하므로 `TryRead`는 할당하지 않는다(블록 일부 읽기만 문맥 버퍼를 쓴다). 그 밖의 실패는 손상이다 |
| `TextVisit.cpp`의 `PathReadInstancedType`·`PathIsOptionalSet`·`PathCount`·byte 배열 `PathRead` 실패 | 실패 | 계약상 visitor는 순회 중 대상을 바꾸지 않는다(TextVisit.h — 바꾸면 경로 연산 오류가 날 수 있다). 그 전제에서 순회는 스키마로 만든 경로로 소유 스레드에서 동기로 읽으며 이 넷은 할당하지 않는 읽기다(타입·set 여부·원소 수·uint8 원소) — 경로 오류·대상 소멸이 생길 수 없다. ExportJson의 같은 자리도 같다. 할당이 있는 읽기(값·키·key wire·byte 배열 자리)는 할당 실패 시험이 덮는다 |

#### 14.1.55 요구 대조표(A3b — 파일 시스템 backend)

시험은 EngineTests `Engine/Source/Developer/AssetNugget/Tests/NuggetBackendTests.cpp`(`NuggetBackend_*` 6 — 실제 디스크)와 `NuggetBackendRaceTests.cpp`(`NuggetBackendRace_*` 12 — 메모리 파일 시스템 실패 주입·hook 경합·두 프로세스·할당 실패)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 왕복(9.1 결정 7) | Stored·Hit(값 byte·digest·Record·inputs·meta), 저장 압축 판단(압축되는 큰 값만 Rcbk, 작은 값·압축되지 않는 큰 값 Identity), 값 선택·partial(Partial)·없는 값 Miss·같은 ValueId 두 번 Miss(InvalidArgument)·skipData·skipMeta, 없는 키 Miss(None), 다른 bucket Miss(InvalidArgument — 유지), 1 MiB 넘는 값(조각 표), 정규 형식 위반 Put Dropped(InvalidArgument·잘못된 bucket), 임시 파일 없음 | `NuggetBackend_RoundTrip` |
| 쓰기 판정(9절·9.1 결정 7) | 같은 Record Exists(기존 digest·Touch), 다른 Record Conflict(기존 유지), 손상(머리·Record·값·끝 위치) Get Corrupt 삭제, 손상 위 Put 교체, 다른 버전 Get Miss·Put Dropped(Unsupported — 유지), 다른 키 파일(echo) 손상, 기록 RawHash가 틀린 파일 Corrupt | `NuggetBackend_ExistsAndConflict`·`NuggetBackend_CorruptionAndVersions`·`NuggetBackend_SystemClockAndWrongRawHash` |
| GC·관리(9.1 결정 9) | Touch(지금 시각)·없는 키 NotFound, Get 적중 Touch·touch 끔, Remove 있음·없음, unusedDays 제거, maxBytes 오래된 것부터·같은 시각 경로 순, 항목이 아닌 이름(16진 아닌 두 글자·`zz`·하위 디렉터리·길이·확장자·16진 아닌 키·접두 불일치·link 디렉터리·link 파일), 지난 tmp 지움·젊은 tmp·잠금 파일(잡힘·안 잡힘 모두 — 지우지 않는다)·다른 이름·디렉터리 유지, 시스템 시계 | `NuggetBackend_TouchRemoveAndCollect`·`NuggetBackendRace_CollectFilterAndAllocations`·`NuggetBackendRace_LinksAndDurability` |
| 열기(A3b 상태) | 빈 루트 InvalidArgument, 루트 자리가 파일, 열기 전 Get·Put·Collect InvalidArgument, 읽기 전용(tmp 못 만듦 — Get Hit·Put Dropped Unsupported), tmp 열거 실패 무시 | `NuggetBackend_OpenAndReadOnly`·`NuggetBackendRace_TemporaryListingFailure` |
| IO 실패(9.1 결정 8) | 만들기·쓰기 두 번째까지·Flush·Close·이동 AccessDenied·디렉터리 만들기 Dropped(tmp·항목 없음), 이동 AlreadyExists인데 항목 없고 재이동 실패 Dropped(Busy), 기존 항목 열기 사용 중·크기·읽기 실패·값 byte만 읽기 실패(Exists 확인 — 손상 아님) Put Dropped·Get Miss(유지), 교체 실패 Dropped, 교체 NotDurable Stored, 읽기 열기 실패 Miss, Remove 지우기 실패, 손상 삭제 실패 Corrupt 그대로, Touch 실패(적중은 무시), GC 지우기 실패 skipped·하위 열거 실패 건너뜀·루트 열거 실패 오류·tmp 지우기 실패 세지 않음 | `NuggetBackendRace_WriteFailures`·`NuggetBackendRace_SettleFailures`·`NuggetBackendRace_ManagementFailures`·`NuggetBackendRace_LinksAndDurability` |
| 경합(9.1 결정 8) | 이동 직전 다른 backend 게시(같은 Record Exists·다른 Record Conflict — 먼저 게시한 쪽 유지), 판정 직전 지워짐 → 재게시 Stored, GC 대 Put, 손상 교체 잠금 안에서 다른 쪽이 고침(같은 Record Exists·다른 Record Conflict — 덮어쓰지 않음), 두 프로세스(pipe gate — 자식 Exists·Conflict), 다른 프로세스가 쥔 shard 잠금(주입한 대기 상한 뒤 Dropped Busy·항목 유지, 풀린 뒤 교체) | `NuggetBackendRace_InProcess`·`NuggetBackendRace_ConditionalReplace`·`NuggetBackendRace_TwoProcesses`·`NuggetBackendRace_HeldShardLock` |
| 흘려 읽는 값 검증(A3b 구현 세부) | `VerifyNuggetValue`: 버퍼 크기(1·7·값−1·값·값+1) 무관 통과, 빈 값, index 범위 밖·빈 버퍼 InvalidArgument, 값 byte 변조는 그 값만(ReadNuggetValue와 같은 판정), 읽기 IO 오류(첫·중간·마지막 조각) 전파, 조각 수만큼 읽음 | `NuggetFile_VerifyValue`·`NuggetFile_ValueTableAndSizeTampering` |
| 할당 실패 | Put(Dropped OutOfMemory·항목 없음)·Get(Miss·항목 유지)·같은 Put 판정 경로·손상 교체 경로·값 선택 partial Get·열기·Touch·Remove·GC(하위 디렉터리·지난 tmp) — 실패 없는 결과는 기준과 같다 | `NuggetBackendRace_AllocationFailures`·`NuggetBackendRace_MoreAllocationFailures`·`NuggetBackendRace_CollectFilterAndAllocations` |
| 조합(14 완료 조건 2) | 기존 항목 상태(없음·정상 같은 digest·정상 다른 digest·손상·다른 버전·읽을 수 없음·판정 중 사라짐·잠금 안 고쳐짐) × 경로(Get·Put·Collect) × 실행(같은 프로세스·두 프로세스) × 실패(IO·할당) | 위 시험 |

#### 14.1.56 Coverage(A3b)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`, 리뷰 반영 뒤): `-Target EngineTests -Filter Nugget -Sources Engine/Source/Developer/AssetNugget/Private` — `NuggetFileSystemBackend.cpp` 줄 788/794·분기 357/366, `NuggetFile.cpp`(`VerifyNuggetValue` 포함) 줄 313/313·분기 159/160(남은 하나는 14.1.54의 `TryRead` 할당 실패 갈래).

닿지 않은 곳과 근거:

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `FileReader::ReadAt`의 범위 검사 | 범위 밖 | 부르는 쪽(`OpenNuggetFile`·`ReadNuggetValue`)이 파일 크기와 부분 합을 먼저 검증하고 그 안만 읽는다 |
| `FileReader::ReadAt`의 Seek 실패 | 실패 | 연 파일 안 위치로의 Seek는 OS 실패 말고는 실패하지 않는다(메모리 파일 시스템도 실패를 주입하지 않는다) — 읽기 실패는 `MemoryFault::Read`가 덮는다 |
| `ChooseStorage`의 압축 설정 실패 | 실패 | 상수 인자(Zstd 기본 수준·블록 2^16 — 12–24 안)이고 Zstd는 이 저장소의 Core ThirdParty로 늘 빌드된다 |
| Put의 임시 파일 이름 Guid 발급 실패 | 실패 | OS 보안 난수 실패(14.1.22와 같다) |
| Get·판정 switch의 암묵 default | 그 갈래 | `EntryState` 다섯 값을 모두 다룬다(clang이 default 없는 switch의 빠짐 갈래를 센다) |

#### 14.1.57 요구 대조표(A3c1 — 동기 캐시 코어)

시험은 EngineTests `Engine/Source/Developer/AssetNugget/Tests/NuggetCacheTests.cpp`(`NuggetCache_*` 12 — 실제 디스크·메모리 파일 시스템·hook 파일 시스템), `NuggetCacheSettingsTests.cpp`(`NuggetSettings_*` 4), `NuggetSccTests.cpp`(`NuggetScc_*` 3), `NuggetBackendTests.cpp`(Put 결과 Record)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 설정(9.1 결정 4·10) | 없는 파일 기본값·빈 객체, 모든 칸, backend 넷, 거부(문법 위치·failure 없이·루트 객체 아님 둘·모르는 키(대소문자)·칸 타입·backend 대문자·모르는 값·빈 localPath·NUL·Guid 32자·중괄호·잘못된 글자·숫자·주석·끝 쉼표·중복 키), 대문자 Guid 받음, 64 KiB 초과 CapacityExceeded·상한 그대로 읽음, 열기·크기·읽기 오류, 할당 실패, localPath 해석(상대·절대·드라이브 — 호스트 문법·할당 실패) | `NuggetSettings_*` |
| localPath 경로 문법(A3c1 구현 세부) | 상대·POSIX 루트·드라이브 절대·드라이브 상대·현재 드라이브 루트·UNC·device namespace × 호스트(Windows 거부 셋 — 해석·설정 읽기 둘 다, POSIX는 이름·루트), 할당 실패 | `NuggetSettings_ResolveLocalPath` |
| 열기(결정 10) | 열기 전 Get·Put InvalidArgument(통계 없음), off(Miss·Dropped 오류 없음, 통계), 두 번째 Open, rockpile Unsupported(다시 열 수 있음), auto·filesystem 루트 만듦, 루트 자리가 파일(required 실패·아니면 off + OpenError), required 성공, 세대·required 접근자 | `NuggetCache_OpenModes` |
| Memory 계층(결정 10) | Put이 넣음(디스크를 지워도 Memory), Local 적중 뒤 Memory, 값 선택 순서, Record에 없는 값(Partial·확정 Miss), SkipData, SkipMeta(backend에는 meta를 청해 Memory가 쥠), 같은 값 두 번·다른 bucket은 backend 판정, 64 MiB 규칙(큰 값 안 쥠 — 그 값은 Local, 새 항목도, Put의 큰 값이 앞서도 작은 값만), 합치기(뒤·가운데 삽입·중복 없음), KeepAlive Touch, 축출(LRU·Get이 순서 갱신·상한보다 큰 항목 안 넣음·상한 0), SkipData 적중은 안 넣음, 통계 | `NuggetCache_MemoryLayer`·`NuggetCache_Eviction`·`NuggetCache_Policies` |
| 세션 고정(결정 11) | DigestChanged(값 버림·반복), 다시 만든 Record 고정 보관(Conflict여도 — 64 MiB·Memory 상한 예외, 이후 Memory), 다시 만든 키의 다른 Put·같은 Put은 고정 보관 그대로, 고정 보관 상한 초과(합·항목 하나) 캐시 없음(Get Miss·Put Dropped·통계), 비결정 Put(Conflict·고정 유지), 같은 Record Put Exists, 디스크가 고정 판으로 돌아옴(적중·Memory, 이후 고정 보관이 항목을 바꿈) | `NuggetCache_PinsAndRegeneration`·`NuggetCache_RegenerationEdges` |
| 잠금 밖 경합 창(A3c1 상태) | Get 읽기 사이 고정 보관(바깥 DigestChanged·고정 보관 항목 유지), Get 읽기 사이 캐시 없음(바깥 Miss), 다시 만든 Record Put 둘 겹침(늦은 쪽 고정 보관 그대로), 보통 Put 사이 DigestChanged(남기지 않음·이후 고정 보관) | `NuggetCache_Windows` |
| 소비 입력(결정 11) | 다른 digest 소비 Miss(InputMismatch·고정 안 함), 같은 digest 적중, 고정 없는 입력 고정(이후 DigestChanged), Memory 적중도 대조(입력 키 다시 만든 뒤 Miss) | `NuggetCache_InputMismatch` |
| 정책(결정 14) | Store 없음 Dropped(오류 없음), StoreRemote만 저장, QueryRemote만·Query 없음 Miss, QueryLocal 적중 | `NuggetCache_Policies` |
| 결과 상태·실패 표지(결정 13) | 쓰기 실패 Dropped failed, 읽기 실패 Miss failed, 다른 버전 Miss Unsupported(실패 아님), 손상 Corrupt(지움·실패 아님·통계), 다른 버전 위 Put Dropped Unsupported(실패 아님·유지), 재게시 실패 Dropped Busy(실패 아님), Record 1 MiB 초과 Dropped CapacityExceeded(실패 아님), 다른 bucket Miss InvalidArgument failed, `IsFailure` 분류 표 | `NuggetCache_FailureFlags`·`NuggetCache_MemoryLayer` |
| SCC(결정 15) | 값 번호·경계, meta golden·찾기·다시 쓰기·4,096 경계, 거부(빈·null·중복·초과·크기·상한 넘는 meta), PutScc·GetSccMember(부분 여럿·SkipMeta·값 없는 구성원 빈 Hit·구성원 아님 NotFound·Record 없음 Miss·4,096 초과 Dropped Capacity 실패 아님·순번 밖 값·중복 구성원 InvalidArgument·잘못된 meta InvalidArgument), 할당 실패 | `NuggetScc_*`·`NuggetCache_Scc` |
| 할당 실패 | Open, PutScc·Get(Memory)·GetSccMember, 다시 만든 Record Put(고정 보관 실패 → 캐시 없음), Get Local 경로(입력·키 고정·Memory 넣기), Memory 적중 복사, 합치기 — 실패하면 Miss·Dropped(OutOfMemory, failed)이거나 Memory에 넣지 않은 적중, 실패 없는 결과는 기준과 같다 | `NuggetCache_AllocationFailures`·`NuggetCache_GetAllocationFailures` |
| 동시 호출 | 네 스레드 × 20회 같은 키·다른 키 Put·Get(같은 키 Stored 하나 + Exists, 모든 적중 같은 digest, Conflict·Dropped·DigestChanged 0) | `NuggetCache_Concurrency` |
| backend 확장 | Put 결과의 계획 Record(digest·key·값·Rcbk 표현·inputs·meta) | `NuggetBackend_RoundTrip` |
| 조합(14 완료 조건 2) | 적중 출처(Memory·Local·없음) × 고정 상태(없음·Normal·Changed·Regenerated·Uncached) × 연산(Get·Put·PutScc·GetSccMember) × 정책(Query·Store·SkipData·SkipMeta·PartialRecord·KeepAlive) × 실패(IO·할당·경합 창) | 위 시험 |

#### 14.1.58 Coverage(A3c1)

측정(2026-11-22, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter Nugget -Sources Engine/Source/Developer/AssetNugget/Private` — `NuggetCache.cpp` 줄 716/717·분기 365/371, `NuggetCacheSettings.cpp` 줄 153/153·분기 92/92, `NuggetScc.cpp` 줄 46/46·분기 29/31(리뷰 반영 뒤).

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `NuggetCache::Open`·`Get`(판정 적용)의 switch 암묵 default | 그 갈래 | `NuggetBackendKind` 넷·`Verdict` 다섯 값을 모두 다룬다 |
| `Evict`·`Insert`·`Settle`의 `ROCKS_ASSERT` | 실패 | 불변식(A3c1 상태): 고정 보관 항목은 지우지 않는다(EvictKey가 건너뜀), 받아들인 적중의 Memory 항목은 같은 Record이고 고정 보관 항목에는 오지 않는다, 고정은 지우지 않으므로 Changed를 본 Put에는 고정이 있다 |
| `NuggetSccValueId`의 `ROCKS_ASSERT` | 실패 | 호출자 결함(순번·부분 번호 범위)이다 — 계약으로 막는다 |
| `Put`의 `Array<NuggetPutValue>` 선언 줄 | 줄 수 0 | 측정 산물이다 — 앞 잠금 블록의 return 뒤 gap 영역이 그 줄에 붙는다. 다음 줄(같은 실행 흐름)도 실행된다 |

#### 14.1.59 요구 대조표(A3c2 — 요청 객체·Shutdown)

시험은 EngineTests `Engine/Source/Developer/AssetNugget/Tests/NuggetRequestTests.cpp`(`NuggetRequest_*` 11 — 실제 디스크·메모리 파일 시스템·hook 파일 시스템 gate·자식 프로세스)다. 시험 공용 도구는 `Tests/NuggetTestSupport.h`(hook 파일 시스템·gate).

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 일괄 요청(9.1 결정 12) | Put 값 이동(항목은 Start 뒤 빈다)·inputs·meta 복사, 결과 항목 순서(Stored·Hit·Miss·값 선택·SkipData), Outcome 성공, Start 전 Results·Outcome StaleHandle·빈 handle, 빈 요청 바로 완료, 끝난 객체로 다시 Start, 겹친 두 요청(나중 요청이 먼저 끝남 — 목록 중간 해제) | `NuggetRequest_PutThenGet` |
| 진행 중·취소 | 진행 중 다시 Start Busy(Get·Put), Results·Outcome Busy, 취소 두 번, 시작 안 한 항목 Cancelled·진행 중 항목은 끝냄(Get·Put) | `NuggetRequest_BusyAndCancel`·`NuggetRequest_PutThenGet` |
| 대기 Put 상한 | 합이 넘는 항목·상한보다 큰 항목 Dropped(CapacityExceeded, 실패 아님), 상한 그대로 받음, 끝난·취소된·제출 실패한·진행 중 소멸된 항목의 예약 반환 | `NuggetRequest_PendingPutBytes`·`NuggetRequest_SubmitFailures`·`NuggetRequest_DestroyWhileRunning` |
| 제출 실패 | scheduler 시작 전 Start 실패(값은 항목에 남음 — Get·Put), maxInFlight 초과 항목과 뒤 항목 Dropped(그 오류·실패 아님)·낸 항목은 끝까지(Get·Put) | `NuggetRequest_SubmitFailures` |
| required(결정 10) | failed 항목(Put 쓰기 실패·Get 읽기 실패) → Outcome DependencyFailed, required 아니면 성공, 실패 아닌 Dropped는 required여도 성공 | `NuggetRequest_RequiredOutcome` |
| Shutdown(결정 12) | 요청 없음(이후 Start 모두 Cancelled·바로 완료, 동기 Get·Put Cancelled, 두 번째 무시, IsShutDown), drain 0(진행 중 Put 끝냄·남은 Put·Get Cancelled), drain 1 ms 기한 초과(같음), drain 끝까지(Put 모두 끝냄·Get Cancelled), hook 두 지점, Shutdown이 돌아오면 모든 항목이 끝남 | `NuggetRequest_Shutdown` |
| 버려진 항목 | scheduler Shutdown(Cancel)이 시작 안 한 항목의 body를 버리면 ticket이 Cancelled로 끝내고 목록에서 뺌(캐시 Shutdown이 바로 돌아옴) | `NuggetRequest_AbandonedItems` |
| 완료 handle이 먼저 끝남 | handle 직접 취소(scheduler Cancel과 같은 상황) 뒤 항목이 목록에 남은 동안 다시 Start Busy(값은 항목에 남음), 캐시 Shutdown이 목록이 빌 때까지 기다림, 그 뒤 다시 Start | `NuggetRequest_RestartWhileSettling` |
| 수명 | worker 아닌 스레드의 진행 중 소멸(취소 요청 → 완료 대기 — 취소 hook gate), Fatal 셋(같은 scheduler worker에서 진행 중 소멸·진행 중 요청의 worker에서 Shutdown·진행 중 요청이 있는 캐시 소멸 — 자식 프로세스 비정상 종료·문구) | `NuggetRequest_DestroyWhileRunning`·`NuggetRequest_Fatal` |
| 할당 실패 | 요청 allocator: 결과 slot·입력 복사(bucket·값 선택·inputs·meta)·예약 표 실패는 Start 실패(OutOfMemory·값은 항목에 남음), 항목 capture 실패는 그 항목과 뒤 항목 Dropped(OutOfMemory), 실패 없는 결과는 기준과 같다(Get·Put) | `NuggetRequest_AllocationFailures` |
| 조합(14 완료 조건 2) | 요청 종류(Get·Put) × 항목 경로(실행·시작 전 취소·제출 실패·상한 초과·capture 정리) × 종료(없음·drain 0·기한 초과·끝까지·scheduler Cancel) × 실패(IO·할당·Fatal) | 위 시험 |

#### 14.1.60 Coverage(A3c2)

측정(2026-11-23, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter Nugget -Sources Engine/Source/Developer/AssetNugget/Private` — `NuggetRequest.cpp` 줄 352/354·분기 125/126, `NuggetCache.cpp` 줄 828/834·분기 409/419(A3c1 몫 포함, 리뷰 반영 뒤).

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `NuggetRequest::WaitForCompletion`·`~NuggetCache`·`NuggetCache::Shutdown`의 `platform::Fatal` | 그 갈래 | 자식 프로세스(`NuggetRequest_Fatal`)에서 실행하고 문구까지 확인한다 — 부모 측정에는 자식의 profile이 합쳐지지 않는다 |
| `ReservePutBytes`의 CAS 재시도 | 재시도 | 다른 스레드가 같은 순간 예약할 때만이다(경합 — 결정적으로 만들 수 없다). 재시도는 같은 판정을 다시 한다 |
| `Shutdown`의 `Notify(AfterCancelGets)` 줄 | 줄 수 0 | 측정 산물이다 — 앞 잠금 블록(Fatal 갈래 포함) 뒤 gap 영역이 그 줄에 붙는다. hook 두 지점은 시험이 확인한다 |

#### 14.1.61 요구 대조표(A4a — 쿠킹 설정)

시험은 EngineTests `Engine/Source/Developer/AssetCooker/Tests/CookConfigTests.cpp`(`CookConfig_*` 7 — 메모리 파일 시스템, 엔진 기본 파일은 저장소 `Engine/Config`)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 프로파일 병합(10.2 결정 2) | 엔진만(id 채움·기본값), 엔진 위 프로젝트(멤버 병합 — 없는 칸은 엔진 값), 다시 읽으면 기본값에서 시작, 엔진 기본 파일 Windows·Android | `CookConfig_ProfileMerge` |
| 프로파일 거부 | id 형식(빈·공백·`..`·`-`·`~`·65 byte, 64 byte는 형식 통과), 엔진 파일 없음(프로젝트만 있어도), 파일 id 다름(엔진·프로젝트, 대소문자), bulkAlignment(4·12·131072·3 거부, 0·8·65536 받음), JSON 문법 위치, 모르는 키·enum 값·타입(진단), 크기 상한(CapacityExceeded), 읽기·크기·열기 실패(code None), failure 없이 거부 | `CookConfig_ProfileRejects`·`CookConfig_Boundaries` |
| 쿠킹 규칙(결정 3) | 파일 없으면 기본값, 모든 칸(규칙의 디렉터리 둘), 경로 형식 13가지(에셋·디렉터리·마운트·대소문자·끝 '/' 없는 마운트·다른 마운트·빈 구성 요소·공백·빈·'/'), 경로 판정의 할당 실패 | `CookConfig_SettingsFields` |
| 쿠킹 규칙 거부 | alwaysCook·neverCook 경로(순번), PrimaryAsset 타입(모름·struct·Asset 아닌 클래스 — 순번)·디렉터리(에셋 경로·다른 마운트·여럿 중 셋째 — 디렉터리 순번·빈 목록·칸 없음), JSON 문법, 대입 진단(배열 칸에 문자열 — 원소 안 배열 포함·모르는 키), 읽기 실패 | `CookConfig_SettingsRejects` |
| SHA-256(결정 2·3) | 같은 내용 같음(다시 읽기), 프로파일 칸 넷마다·규칙 변형 여덟(값·원소·순서·Soft·PrimaryAsset)마다 서로 다름, 타입 미등록 NotFound | `CookConfig_Hashes`·`CookConfig_Boundaries` |
| 할당 실패 | 프로파일·규칙 읽기·두 SHA-256 — 실패한 호출은 모두 OutOfMemory(규칙 위반으로 바꾸지 않는다), 주입하면 적어도 하나 실패, 실패 없는 결과는 기준과 칸·SHA-256까지 같다 | `CookConfig_AllocationFailures` |
| 조합(14 완료 조건 2) | 파일(엔진·프로젝트·규칙) × 상태(없음·정상·문법·진단·규칙 위반·크기·IO) × 칸 | 위 시험 |

#### 14.1.62 Coverage(A4a)

측정(2026-11-23, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Target EngineTests -Filter CookConfig -Sources Engine/Source/Developer/AssetCooker/Private` — `CookConfig.cpp` 줄 264/266·분기 143/144(리뷰 반영 뒤 — 남은 줄 둘은 아래 Publish 갈래).

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `ApplyJsonFile`의 `Publish` 실패 | 실패 | 값 대상의 게시는 준비한 사본과 무할당 맞바꾸기다(ReflectionText TextAssignments.h — 실패는 살아 있는 객체의 묶음 게시에만 있다) |

#### 14.1.63 요구 대조표(A4b — 대상 계산·콘텐츠 해시)

시험은 EngineTests `Engine/Source/Developer/AssetCooker/Tests/CookTargetsTests.cpp`(`CookTargets_*` 7 — 메모리 Content에 노드 그래프를 쓰고 색인을 만든다, 의존 섹션은 시험이 다시 써서 import 없이 어떤 그래프든 만든다)와 `Engine/Source/Developer/AssetDatabase/Tests/AssetIndexContentHashTests.cpp`(`AssetIndex_ContentHash*` 4), 캐시 형식은 `AssetIndexCacheTests.cpp`다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 전체 쿠킹(A4b 규칙 1·3·5) | alwaysCook ∪ PrimaryAsset 규칙, Hard 폐포(두 단계), Soft는 설정일 때만, EditorOnly·Build·ImportOwner 따라가지 않음, bundle(이름 순·구성원 AssetId 순), PrimaryAsset 아닌 에셋의 bundle 태그 무시, 파생 타입 규칙(Base 규칙이 Derived 포함·TestAsset 제외), 규칙 밖 뿌리는 PrimaryAsset 아님·폐포로 들어온 규칙 에셋은 PrimaryAsset, 디렉터리 구성 요소 경계, out 비움 | `CookTargets_FullCook` |
| 뿌리 진단(규칙 1) | 디렉터리 재귀, 없는 에셋·디렉터리 경고 InvalidPath(순번), 같은 경로 DuplicatePath, 같은 id(alwaysCook 경로·PrimaryAsset 규칙·마운트 목록의 두 파일), 같은 뿌리 두 번은 한 번, 마운트는 전체 | `CookTargets_Roots` |
| NeverCook(규칙 2·3) | 에셋·디렉터리 직접 지정 경고(순번), 규칙으로만 닿으면 조용히, Hard·bundle 오류, Soft 설정이면 제외 목록(둘 — AssetId 순)·아니면 없음, 마운트 NeverCook(경고만 — HasErrors 거짓) | `CookTargets_NeverCook` |
| 누락·같은 id·bundle 태그(규칙 3) | Hard 누락 오류·Soft 누락 경고(같은 의존의 Hard·Soft 누락은 둘 다 — 오류 먼저), 같은 id Hard·Soft·bundle 오류, bundle 값 문자열·빈 이름(`Bundle.`)·null·대문자 CorruptPackage, bundle 목록은 올바른 태그만(누락·같은 id 구성원도 목록에는 있다), Soft 설정이 아니면 Soft 의존을 보지 않음 | `CookTargets_MissingAndDuplicates` |
| 부분 쿠킹(규칙 4) | 인자 경로(없음 오류·NeverCook 경고·디렉터리), 역방향 Build 고정점(두 단계·더한 에셋의 Hard 폐포·NeverCook 참조자 조용히·같은 id 참조자 오류), 역방향 범위(Hard 폐포로만 들어온 에셋의 Build 참조자는 더하지 않음, 폐포로 먼저 들어온 에셋이 뒤에 역방향으로 닿으면 그 참조자도), PrimaryAsset 규칙에 맞는 폐포 에셋의 bundle, 기존 매니페스트(사라진 항목, 같은 id 항목은 남음, 다시 쿠킹하지 않는 남은 항목의 Hard → 사라짐 오류, 다시 쿠킹하는 항목의 옛 Hard → 사라짐은 오류 아님, 색인에 있는 Hard·Soft는 아님, 사라진 항목끼리는 보지 않음), 전체 쿠킹은 기존 매니페스트를 보지 않음 | `CookTargets_Partial` |
| 요청 거부 | 색인·TypeRegistry·설정 null(out 비움), 형식이 틀린 alwaysCook·neverCook·PrimaryAsset 디렉터리·인자 경로, 등록되지 않은 타입 — InvalidArgument, 종료한 TypeRegistry는 ShuttingDown 그대로 | `CookTargets_Rejects` |
| 결정성 | 대상·제외·사라진 항목 AssetId 순, 진단 (asset, code, related, setting, 오류 먼저) 순(시험 쪽 비교로 정렬해 대조) | 위 시험 모두 |
| 할당 실패(대상) | 전체(bundle·Soft·제외·경고)·부분(역방향 Build 참조자·같은 id 참조자·기존 매니페스트·진단) — 모든 지점 OutOfMemory, 다 지나면 기준과 대상·PrimaryAsset·bundle 수·제외·사라진 항목·진단 수가 같다 | `CookTargets_AllocationFailures` |
| 콘텐츠 해시(결정 5) | 처음 계산(전체 SHA-256·Stat 값), Stat 같으면 열지 않음, 시각만·크기만 달라도 다시 읽음, 내용 바뀌면 새 해시, 큰 파일(조각 셋 + 끝)·빈 파일 | `AssetIndex_ContentHashComputesAndReuses` |
| 콘텐츠 해시 거부 | 없는 id·null NotFound, 같은 id AlreadyExists, 첫 Stat·두 번째 Stat·열기·읽기 실패·파일 없음, 읽는 동안 크기만·시각만 바뀌면 Busy — 해시를 두지 않음, 고장을 걷으면 계산 | `AssetIndex_ContentHashRejects` |
| 캐시 보존 | 네 칸 왕복(SameIndex), 캐시를 쓴 색인은 Stat 같으면 열지 않음, 캐시 뒤 시각만 바뀐 파일(같은 지문 — 항목 재사용)은 다시 읽음, 해시 없는 항목은 없음 그대로 | `AssetIndex_ContentHashCachePersists` |
| 캐시 형식(schemaVersion 3) | 네 칸을 가진 항목을 받음(값 그대로), 표지 2 거부, 잘림(모든 칸 위치) 거부 | `AssetIndexCache_RejectsUntrustedInput` |
| 할당 실패(해시) | 경로·읽기 버퍼 — OutOfMemory·해시 없음, 다 지나면 계산 | `AssetIndex_ContentHashAllocationFailures` |
| 조합(14 완료 조건 2) | 뿌리 출처(alwaysCook 에셋·디렉터리·마운트·규칙·인자) × 항목 상태(정상·같은 id·같은 경로·NeverCook·없음) × 간선(Hard·Soft·bundle·Build 역방향·따라가지 않는 셋) × Soft 설정 × 전체·부분 | 위 시험 |

#### 14.1.64 Coverage(A4b)

측정(2026-11-23, Windows, `ROCKS_MSVC_TOOLSET=14.44`): `-Filter CookTargets_ -Sources Engine/Source/Developer/AssetCooker` — `CookTargets.cpp` 줄 436/436·분기 228/228(리뷰 반영 뒤), `CookTargets.h` 분기 4/4. `-Filter AssetIndex_ContentHash -Sources Engine/Source/Developer/AssetDatabase` — `AssetIndexContentHash.cpp` 줄 61/61·분기 30/30. `-Filter AssetIndexCache_` — `AssetIndexCache.cpp`의 새 칸 읽기·쓰기는 모두 닿았다(남은 분기 셋 — 캐시 파일 크기 조회 실패·키의 normalizesNames 쓰기·readOnly 참 쓰기 — 은 A2d1 때부터 있던 것이다).

닿지 않는 곳 없음.

#### 14.1.65 요구 대조표(A4c1 — 키·처리기 표·기반 둘)

시험은 EngineTests `Engine/Source/Developer/AssetCooker/Tests/CookKeysTests.cpp`(`CookKeys_*` 6·`CookProcessorTable_*` 1 — 그래프 장면은 공용 `CookTestSupport.h`)와 FoundationTests `Core/Source/Runtime/Core/Tests/StrongComponentsTests.cpp`(`StrongComponents_*` 3), `Core/Source/Runtime/ObjectRuntime/Tests/Reflection/TypeVersionListTests.cpp`(`TypeRegistry_ListTypeVersions*` 2), `Core/Source/Runtime/Core/Tests/GuidTests.cpp`(`Guid_LessOrdersHighThenLow`)다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 처리기 표(A4c 세부) | 0 type·같은 type 등록 거부, 정확한 type·가장 가까운 base(Derived → Base)·없으면 fallback, 미등록 NotFound, 0 type InvalidArgument, 등록 할당 실패, 기본 설정 키(재정의 없음) | `CookProcessorTable_RegisterAndFind`·`CookKeys_*`(PlainProcessor) |
| 키 기본(결정 5) | 대상 순·모두 키, 콘텐츠 해시 = 파일 전체 SHA-256, cookKey·build key 서로 다름, 다시 계산하면 같음, 타입 표 지문 = HashTypeTable, Build로만 닿는 에셋은 키 목록 밖, out 비움 | `CookKeys_BasicAndDeterministic` |
| 입력 → 키 | 프로파일(cook만), 프로젝트 세대(둘 다), 쿠커 버전(cook만), 처리기 정체성(그 타입·파생의 build·cook, Hard로 기대는 에셋의 cook만, 나머지 그대로), 처리기 버전 Guid, 설정 키(쓰기 시작·값) | `CookKeys_InputsChangeKeys` |
| 내용·순환(시나리오 28) | Build 원본만 바뀌면 그 Build 사용자의 build·cook만, Hard 의존 내용은 사슬의 cook(사용자 build는 그대로), Hard 순환은 한쪽 내용으로 둘 다, Build 순환(대상 아닌 상대)의 내용으로 대상 build·cook, 경로만 바뀌면 cook만 | `CookKeys_ContentAndCycles` |
| 판정·타입 표 | PrimaryAsset(bundle 포함)이면 그 에셋 cook만, Soft 설정만으로는 키 그대로, 제외 Soft 의존이 생기면 cook(build 그대로), 타입 하나 더 등록하면 모든 cook·build 그대로 | `CookKeys_DecisionsAndTypeTable` |
| 실패 | 파일 없음 MissingFile(error NotFound)·읽기 실패 None(error), Build 의존 없음 MissingDependency·같은 id DuplicateAssetId, 처리기 없음(미등록 → UnsupportedVersion(error NotFound)·종류 다름 → TypeMismatch(error TypeMismatch)), 원인과 Hard·Build로 기대는 에셋 키 없음, 나머지 키 있음, 대상 밖 Hard 간선(NeverCook — A4b 오류)은 빼고 계산 | `CookKeys_Failures` |
| 요청 거부 | null 다섯 InvalidArgument(out 비움), 다른 색인의 대상 InvalidArgument, 종료한 TypeRegistry(요청·처리기 표) ShuttingDown, 설정 키 writer 오류 그대로 | `CookKeys_Rejects` |
| 할당 실패 | 키 계산의 모든 지점(실패 장면 포함) OutOfMemory·다 지나면 기준과 같음, 색인 allocator 실패(콘텐츠 해시)도 OutOfMemory | `CookKeys_AllocationFailures` |
| 기반: SCC | 빈 그래프, 간선 없는 노드·자기 간선 cyclic, 순환과 꼬리(의존 먼저)·떨어진 묶음 순서(결정적), 깊은 사슬 20만(재귀 없음)·고리 하나, visit 실패 멈춤, 모든 할당 지점(긴 사슬 포함) | `StrongComponents_*` |
| 기반: 타입 목록 | 빈 Registry, 클래스·struct·enum(버전 0) TypeId 순, 해제 뒤 빠짐, out 비움, 다른 스레드 WrongThread, 할당 실패, Shutdown 뒤 ShuttingDown(셋 다 out 빈다) | `TypeRegistry_ListTypeVersions*` |
| 기반: GuidLess | high 먼저·low 부호 없는 수치·같으면 거짓·constexpr | `Guid_LessOrdersHighThenLow` |
| 조합(14 완료 조건 2) | 키 종류(build·cook) × 입력 축(위 열둘) × 그래프 모양(사슬·Hard 순환·Build 순환·Build 원본·대상 밖) × 실패 원인(넷) | 위 시험 |

#### 14.1.66 Coverage(A4c1)

측정(2026-11-23, Windows, `ROCKS_MSVC_TOOLSET=14.44`):
- `-Filter Cook -Sources Engine/Source/Developer/AssetCooker`: `CookKeys.cpp` 줄 335/335·분기 154/155, `CookProcessor.cpp` 줄 33/33·분기 16/16, `CookProcessor.h`·`CookKeys.h` 전부.
- `-Target Foundation -Executable FoundationTests`: `TypeRegistry.cpp`의 `ListTypeVersions`는 모든 줄·분기가 닿았다(파일의 나머지 미도달은 이 단계와 무관한 기존 함수). `StrongComponents.h`는 합친 보기에서 0회 줄이 없다. 요약 86/88·44/46은 템플릿 인스턴스별 집계다(깊은 사슬 시험의 visit 인스턴스는 할당 실패를 주입하지 않는다).

| 위치 | 닿지 않는 것 | 근거 |
|---|---|---|
| `CookKeys.cpp` `CookLocalKey`의 `ROCKS_ASSERT(key)` | 단언 실패 쪽 | 기록하는 문자열은 LogicalPath 키·PackageTags가 검증한 UTF-8이고 해시 기록기는 할당하지 않는다 — 실패할 수 없는 불변식이다 |

## 15. PNG 디코더 범위

색 형식 0/2/3/4/6, 비트 깊이 1–16, Adam7, PLTE·tRNS, chunk CRC, zlib(Core `Compression`)·Adler-32, 한 변 16384·해제 크기 = IHDR 계산값. gAMA·cHRM·sRGB·iCCP는 변환하지 않고 sRGB로 보며 sRGB가 아닌 표시가 있으면 `NonSrgbSource` 경고. APNG는 기본 이미지. 시험은 PngSuite(자유 사용 라이선스, `TextureTools/Tests/Fixtures/PngSuite`).

## 16. 임포트·빌드 의존

### 16.1 재임포트

기존 옵션 유지·명시 값만 변경, 잘못된 옵션 `InvalidImportOption`, 임포터 버전이 기록보다 높으면 `ImporterOutdated` 표시(자동 실행 없음), 사라진 출력은 `OrphanedOutput` 표시·`asset-orphans-*`로 처리.

### 16.2 Build 의존 선언

`DependencyDeclarer`(AssetRuntime 인터페이스, 타입별 등록)가 에디터 객체에서 Build 의존(예: Texture2DArray의 원본 Texture2D)을 선언한다. AssetDatabase는 저장 때 이 인터페이스로 목록을 얻어 Dependencies에 기록하고(쿠커를 부르지 않는다 — 의존 방향 유지), AssetCooker는 같은 선언을 너겟 키·선행 조건에 쓴다.

## 17. 계획 문서 개정 사항

계획 문서에 함께 반영한다.

- 2026-11-22 Rockpile 17절의 AssetSystem 개정 반영(A3 착수, 사용자 결정 — Rockpile 레코드 계약): 1절 A3 행, 2절 프로세스 행, 3절 에셋 너겟 상태, 8절 동시 기록 문장, 9절(세대 Guid 둘·결과 압축과 저장 압축·Record·읽기 검증 보장 범위·쓰기 판정·인터페이스), 9.1절 A3 세부, 13절(`--verify-nuggets-source`·`nugget-gc` 위임), 14절 시나리오 9·13, 18절 계약 목록. 같은 날 CoreDataFoundations 132행·Rockpile 12.4절(프로젝트 세대 예외)도 고쳤다. Rockpile 17절의 나머지(계획 문서·Compression·PlatformLayer 등)는 Rockpile 서버 착수 때 반영한다.

- 하드 참조 = 다른 패키지 객체로의 `ObjectRef`(에셋 객체 Registry 안), 소프트 = `SoftObjectRef<T>`(AssetId + 객체 경로), World 객체 → 에셋은 `SoftObjectRef` + `AssetLease`(6.3절). §7.1의 `ObjectRef<MaterialAsset>` 예시 수정.
- 패키지당 대표 에셋 ID = 패키지 ID. 쿠킹 패키지 파일 이름은 `<AssetId>-<패키지 SHA-256 앞 16자>`(내용 주소, 4.2절).
- §9.1: "AssetRegistry"는 AssetIndex(에디터 패키지 인덱스, 재검색으로 복원되는 캐시)이고, 별칭 표는 인덱스 안이 아니라 `Config/AssetAliases.json`의 버전 관리 사용자 데이터다(재검색으로 복원되지 않는다, 4.3절).
- 로딩 API 추가: `AssetRequest.expectedType`·`scope`, `LoadBlocking`(도구·초기화 전용), `OnReady(LoadTicket&)` 참조 인자. `OnReady` lane·`ReadyMask`의 이번 구현 범위는 개정이 아니라 11.1절의 `계약` 상태다.
- §9.2 수명: 상주 단위 = 하드 참조 SCC, Closing 대기 중 revive, Pump 안의 Closing → Discard(11.4절). 계획의 발급·Closing 선형화 규칙과 같다.
- 임포트 worker process(§9.1): 장시간 실행 호스트(에디터)를 임포터 실패로부터 보호하는 장치이므로 에디터와 함께 구현한다. 이번 범위의 임포트 호스트는 명령행 프로세스 자체이며(실패해도 원자적 저장으로 에셋 보존), `Importer` 인터페이스는 입력(원본 바이트·옵션)·출력(객체 그래프·bulk)만 주고받아 프로세스 경계로 옮길 수 있게 한다.
- Virtual Asset(§9.4): 원본 bulk의 분리 저장소는 후속. 이번에는 에디터 패키지 안 bulk가 권위 저장소다(에셋 너겟에만 있는 원본은 허용하지 않는다는 원칙은 지킨다).
- 런타임 VFS(APK)는 Android 그래픽·패키징과 함께 후속(쿠킹 매니페스트는 쿠킹 루트 기준 상대 경로만 사용).
- 용어: UE의 DDC(Derived Data Cache)에 해당하는 것을 **에셋 너겟**(AssetNugget)이라 부른다(사용자 결정). 저장되는 가공 결과 하나가 너겟이고, 모듈은 `AssetNugget`, 모듈 안 타입은 짧은 `Nugget` 접두(`NuggetCache`·`NuggetBackend`·너겟 키)를 쓴다. 2026-11-13에 "빌드 캐시(BuildCache)"에서 바꿨다 — 코드 빌드의 캐시와 헷갈렸기 때문이다. 코드 산출물 캐시는 계획 문서의 `BuildArtifactCache`다. 계획 문서 전체에 반영.
- 반영한 계획 문서 위치: §3.1 모듈 표(AssetRuntime은 에셋 너겟 reader가 아니라 쿠킹 매니페스트 reader), §7.1 예시(`SoftObjectRef<MaterialAsset>`), §9.1 AssetIndex·별칭·worker process, §9.2 `AssetRequest`·수명, §9.4 Virtual Asset 후속·쿠킹 패키지 이름(내용 주소), VFS 후속, A21 위험 항목.

## 18. 범위 밖과 후속

메시·스켈레톤·스킨 메시·애니메이션 클립 에셋 타입은 [MeshAnimationAssets](MeshAnimationAssets.ko.md)가, UE 콘텐츠를 가져오는 교환 경로는 [Unreal 교환 형식](UnrealExchange.ko.md)이 설계했다(2026-11-20). 두 스펙의 단계(MA·UX)는 이 문서의 A1–A6 뒤이며 착수는 사용자가 정한다.

후속(이번 범위에 API도 없음): 에디터 UI, GPU 업로드·텍스처 mip 스트리밍(계획 §9.5), equirect Cube, Cube 면 경계 연속 mip, 임포트 worker process, Virtual Asset, VFS, 예산 LRU 해제, 실제 다중 출력 임포터(FBX·glTF), UE의 다른 텍스처 계열(렌더 타깃 에셋·IES `TextureLightProfile`·`RuntimeVirtualTexture`·미디어·동적 텍스처), 일반 에셋 사용자 데이터(`AssetUserData`), 쿠킹 결과 패키징(배포 위치 배치).

계약(API·스키마는 있고 기본값이 아니거나 호출하면 명시 오류)으로 이번에 두는 것은 3·11·12절 표에 있다: `NuggetRockpileBackend`(설정 `backend: "rockpile"` → Unsupported, Rockpile R1a), Record `Codec` encoding(늘 미적중), 너겟 byte 범위 부분 읽기(Rockpile 8.6절), `NuggetPolicy`의 원격 비트(원격 동작 없음), 블록 압축, 12.4절 계약 설정(가상 텍스처·스트리밍·보정·색 관리·압축 설정 포함), `ReadyMask::Render`, `TaskLane::Worker`·`Render`.
