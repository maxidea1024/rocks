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
| A2 | AssetDatabase: `ProjectLayout`, 경로 정책, 인덱스(검색·의존·별칭·진단·캐시·감시), 저장·dirty, 이동·복제·삭제, 임포트 프레임워크 | 인덱스·이동·동시 쓰기 시험 |
| A3 | AssetNugget | 적중·무효화·손상·동시 기록·GC 시험 |
| A4 | AssetCooker | 원본 없이 쿠킹·재현성·부분 쿠킹 시험 |
| A5 | 텍스처: `Texture` 계열, `TextureSource`, ImageCore(BMP·TGA·PNG·PSD·DDS·TIFF·Radiance HDR·무손실 OpenEXR — 12.2절), TextureTools(임포터·빌드·쿠킹 처리기), 클래스 기본값 레코드·차이 쓰기·override 집합·지문(cookKey·Summary 지문 열 — A1 Summary·A4 cookKey 산출물을 이때 고친다, RuntimeClassMetadata '클래스 기본값 결정') | 14절 텍스처 시나리오 |
| A6 | 명령행·문서·검증 보고 | 14절 결과 기록 |

## 2. 사전 조사(2026-11-12)

| 영역 | 있는 것 | 부족한 것 → 보강 |
|---|---|---|
| 객체 | `ObjectRegistry`(ContextId 문맥)·`RootRef`·`ObjectRef`(같은 Registry 안에서만)·`WeakObjectRef`·`Borrow`(다른 문맥 Registry는 WrongContext), `ConstructionContext`(스테이징한 객체를 한 번에 공개, RootRef는 첫 객체에만), `TypeId = Guid` | Outer·Name, 다중 root Commit(root마다 RootRef) → A0-O([ObjectFoundations](ObjectFoundations.ko.md)) |
| 리플렉션 | MetaCompiler: 스칼라·Guid·String·`ObjectRef`, 모든 비정적 필드 리플렉션 필수, 상속 base·참조 대상은 같은 생성 단위, 스키마 버전 1 고정, 타깃 소유 메타데이터·명세 | → A0-B([ReflectionFoundations](ReflectionFoundations.ko.md)) |
| 직렬화 | Archive·Record(TLV, 반복 필드 없음), `SaveObjectGraph/LoadObjectGraph`(단일 root, 내부 Commit, 마이그레이션 콜백, maxObjects 1024) | → A0-O graph v2(호출자 저장 집합, 단계형 복원·호출자 Commit, 외부 참조 — ObjectFoundations 5절), `RecordList` → A0-C |
| 비동기 | `WorkerScheduler`(DAG·취소), `AsyncReadFile/AsyncWriteFile`(IOCP, `Flush`) | 로더가 사용(11절) |
| 파일 | `FileSystem`, `CreateNew`, no-replace Move, `Path`, Win64는 `FILE_SHARE_DELETE` 열기 | → A0-P, VFS(APK) → 후속 |
| 해시·텍스트 | CRC32, `NameKey`, UTF-8 검증·`CodePointRange` | SHA-256·Unicode·JSON → A0-C |
| 프로세스 | 없음 | 임포트 worker process는 17절 |
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
| 에셋 너겟 | AssetNugget | 구현(로컬), 공유·원격 backend 계약 |
| Cooker | AssetCooker | 구현(비압축 형식), 블록 압축 계약 |
| Runtime Asset Loader | AssetRuntime `AssetLoader` | 구현(CPU ready), `Render` ready 계약 |
| Asset Manager | AssetRuntime `AssetManager` | 구현(scope·PrimaryAsset·bundle·즉시 해제), 예산 LRU 해제 후속 |
| 메시·스켈레톤·애니메이션 에셋 | `MeshSource`·`Mesh`·`Animation`(Runtime), `MeshTools`·`AnimationTools`(Developer) | `MeshSource` 구현(MA1 — 원본 메시 저장·검증·위상 조회, bulk 쓰기는 `MemoryBulkSource`), 나머지 설계([MeshAnimationAssets](MeshAnimationAssets.ko.md) 8절·3.7절, MA2–MA7) |
| 외부 엔진 교환 임포터 | 엔진 플러그인 `UnrealExchange`(Developer) | 설계([Unreal 교환 형식](UnrealExchange.ko.md) 10절, UX1–UX8) |

이름 구분: 에셋 객체가 사는 `ObjectRegistry`는 **에셋 객체 Registry**, 에디터 패키지 인덱스는 **AssetIndex**(UE AssetRegistry 대응), 쿠킹 결과의 런타임 목록은 **쿠킹 매니페스트**(`CookedManifest.rman`, 10절)라 부른다.

의존 방향(→는 의존):

```
Programs/Commandlet → TextureTools → { AssetCooker, AssetDatabase, AssetNugget, ImageCore, Texture }
AssetCooker → { AssetDatabase, AssetNugget }        AssetDatabase → AssetRuntime
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
| `MissingSource`, `SourceChanged`, `InvalidSource`(원본 해석 실패), `UnsupportedSourceFormat`(지원하지 않는 원본 변형, 예: PSD CMYK), `LossySource`, `NonSrgbSource`, `IgnoredSourceChannels`(경고), `AbsoluteSourcePath` | 원본 |
| `ImporterOutdated`, `InvalidImportOption`, `OrphanedOutput`, `UnknownProperty`(모르는 속성 이름), `UnsupportedSetting`(계약 설정의 기본값이 아닌 값), `InvalidSetting`(모순된 조합·숨김 값), `SrgbIgnored`(경고 — 선형 설정·float 출력의 SRGB), `PreserveBorderWithWrap`(경고) | 임포트·설정 |
| `CorruptEntry`(에셋 너겟 항목·게시 패키지 파일 손상), `IncompatibleCook`(부분 쿠킹 병합 불가·런타임 프로파일 불일치), `NeverCookReferenced` | 에셋 너겟·쿠킹 |
| `AliasResolved`, `RootLost`(감시 루트 소실) | 그 밖 |
| `InvalidMeshData`, `NonFiniteAttribute`, `UvOutOfRange`, `NonNormalizedWeights`, `TooManyInfluences`, `InfluencesTrimmed`, `MaterialSlotFallback`, `NegativeBuildScale`, `UnsupportedCollisionShape`, `SkeletonMismatch`, `UnknownBoneTrack`, `InvalidClipData`, `EventPropertiesDropped` | 메시·애니메이션([MeshAnimationAssets](MeshAnimationAssets.ko.md) 11절 — 앞 다섯 코드는 MA1이 `AssetDiagnosticCode`에 등록, 나머지는 설계) |
| `DuplicateLevelScript`(`levelScript` 말고 또 있는 LevelScript 파생 Actor), `LevelScriptNotInLevel`(`levelScript`가 `actors` 원소가 아님), `CrossLevelReference`(레벨에 저장하는 참조가 다른 Level, 또는 영속 Entity 참조 전 다른 셀의 객체를 가리킴), `NotPlaceableClass`(편집기 배치 명령이 `NotPlaceable` 클래스를 받음), `LevelScriptCollider`(LevelScript의 Collider component) | 레벨(설계 — [레벨 스크립트](LevelScript.ko.md) 4.9절. 저장·쿡은 InvalidArgument, cooked 로드는 CorruptPackage) |
| `CorruptBundle`, `UnsupportedBundleVersion`, `UnknownField`, `UnsupportedAsset`, `LossyOriginAccepted`, `UnsupportedSourceLayout`, `GammaMismatch`, `GeneratedLodBaked`, `BakedColorQuantized`, `UnrealPathRenamed`, `UnrealPathTooLong`, `UnrealPathCollision`, `MissingUnrealDependency`, `UntranslatedMaterialNode`, `MaterialBakedFallback`, `ParameterDefaultConflict`, `ParameterResolveMismatch` | Unreal 교환 임포트(설계 — [Unreal 교환 형식](UnrealExchange.ko.md) 10.3절) |

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
3. Objects를 먼저 메모리에 직렬화한다(그 결과의 외부 참조 목록이 있어야 ObjectImports·Dependencies를 만든다). 그다음 섹션을 물리 순서대로 스트리밍 기록하며 CRC를 증분 계산한다. bulk는 블록 단위로 압축·CRC·SHA-256을 계산하며, 원본은 객체가 메모리에 가진 raw 데이터이거나 아직 읽지 않은 entry면 이전 패키지 파일의 열린 핸들(6.1절)이다. 이전 파일에서 옮기는 entry는 저장 crc32와 raw SHA-256을 검증하며 다르면 저장을 `CorruptPackage`로 실패시킨다(손상이 새 CRC로 세탁되지 않게). bulkId는 저장마다 객체·필드 순서로 1부터 다시 매기고 writer가 (옛 id → 새 id) 재매핑 표를 만든다. BulkIndex·섹션 표를 쓰고 offset 0으로 돌아가 헤더를 쓴다. 메모리 피크(객체와 이미 상주한 raw 데이터 제외)는 Objects 바이트 + 다른 비-Bulk 섹션 하나 + bulk 블록 하나다(비-Bulk 섹션은 각각 64 MiB 상한).
4. `File::Flush` → Close.
5. 고정 크기 버퍼로 다시 읽어 헤더·표·섹션·entry CRC를 검증한다(writer 버그·짧은 쓰기 검출. 페이지 캐시를 읽으므로 매체 손상 검출이 목적은 아니다).
6. `FileSystem::Replace(temp, target)`(구현, `Platform/File.h` 계약). Windows: `SetFileInformationByHandle(FileRenameInfoEx, REPLACE_IF_EXISTS | POSIX_SEMANTICS)`(다른 쪽이 `FILE_SHARE_DELETE`로 연 대상도 교체 — 이전 파일의 열린 핸들도 계속 유효하다). 이 정보 클래스가 없는 볼륨(FAT·일부 네트워크 FS)은 `MoveFileExW(REPLACE_EXISTING)`로 대신하며, 그 볼륨에서 열린 대상은 `Busy`·`AccessDenied`로 끝나고 대상은 바뀌지 않는다. `ReplaceFileW`는 쓰지 않는다(이전 대상의 속성·스트림을 합치고, `ERROR_UNABLE_TO_MOVE_REPLACEMENT`에서 대상을 지운 채 남긴다). Windows의 이름 변경 매체 기록은 NTFS 메타데이터 로그에 맡긴다(반환 직후 전원 차단 시 이전 상태일 수 있다). POSIX: 부모 디렉터리를 먼저 열고 `rename` 후 그 디렉터리를 `fsync`(macOS `F_FULLFSYNC`). 오류: 원본이 일반 파일이 아니거나 대상이 디렉터리·link면 `WrongType`, 대상이 읽기 전용이거나 권한이 없으면 `AccessDenied`, 공유 위반이면 `Busy`.
   교체 직전에 임시 파일의 읽기 핸들을 열고 패키지가 쥔 이전 파일 핸들을 닫는다(POSIX 의미 rename이 없는 볼륨의 `MoveFileExW`는 자기 핸들이 열려 있으면 실패하므로 모든 경로에서 같은 순서로 한다). 교체가 실패하면 이전 경로를 다시 열고 지문이 로드 때와 같은지 확인한다(다르면 이후 저장은 `ExternalModification`).
7. 실패하면 임시 파일을 지우고 보고(기존 보존). `NotDurable`(POSIX 디렉터리 동기화만 실패)은 교체가 이미 보이는 상태이므로 임시 파일 정리 없이 저장 성공으로 처리하고 `NotDurable` 경고를 남긴다. 성공(경고 포함) 후: 저장 세대와 무관하게 메모리의 모든 `BulkDataRef`를 재매핑 표(옛 id·SHA-256 → 새 id)로 바꾸고 패키지의 bulk 핸들·BulkIndex를 새 파일(6번에서 연 핸들)로 다시 잇는다(아직 읽지 않은 entry도 새 파일에 있다). 직렬화를 시작한 세대 = 현재 세대일 때만 dirty를 지운다(저장 중 편집이 있었으면 dirty 유지 — 편집으로 새로 생긴 raw 데이터는 메모리에 있으므로 다음 저장이 쓴다). 마지막으로 인덱스를 갱신. 재매핑은 저장 직렬화의 값 치환 hook(B4가 구현한 `SubstituteStruct` — graph v2가 쓴다)이 직렬화 바이트에 새 id를 쓰고, 같은 옛 id를 가리키는 여러 참조는 같은 새 id로 바꾼다.
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
17. **ImportOwner·Tags**: ImportRecord의 내용 스키마(ownerAssetId 포함)는 A2(AssetDatabase)가 정하므로 ImportOwner 의존과 ImportRecord의 로드 대조는 A2로 미룬다(지금은 ImportOwner·Build 의존을 쓰고 읽되 대조하지 않는다 — `AssetPackage_ImportOwnerCrossCheckDeferredToA2`). 여러 ImportOwner 항목과 required의 뜻도 A2가 ImportRecord 스키마와 함께 정한다. Tags는 Record envelope·TLV 구조와 키(필드 ID) 엄격 오름차순을 reader와 writer 옵션 검증이 같이 본다(값 스키마는 A2 — `AssetPackage_TagsKeysAreSorted`).
18. **에디터 읽기 전용 열기의 기대 결과**: 더 새 데이터(모르는 필드·더 새 타입·커스텀 버전·Required가 아닌 모르는 섹션)를 담은 에디터 파일은 `Prepare`가 읽기 전용으로 받고 `InstantiatePackages`·Commit까지 간다. 모르는 필드는 건너뛰므로 그 안의 Hard·Soft·bulk 참조는 기본값(null·빈 값)으로 남고, Dependencies는 부분집합만, 쓰이지 않는 import·bulk entry는 검사하지 않으며, Package는 bulk entry를 그대로 가진다(`AssetPackage_EditorReadOnlyLoadsEndToEnd`). 현재 클래스가 없는 export 타입(미등록, 또는 등록됐지만 클래스가 아닌 타입 id)은 reader가 읽기 전용으로 표시하고(헤더·인덱스 도구) `PackageContents::unavailableExportType`에 남긴다. 객체를 만들 클래스가 없으므로 `Prepare`는 그래프 1단계 전에 이 reader 판정으로 Summary `UnsupportedVersion`(`Unsupported`)을 낸다. 그래프 1단계의 TypeMismatch로 분류하지 않는 것은 그 오류가 노드 Record 손상에서도 나기 때문이다(처음 구현은 미등록은 그래프 NotFound → `UnsupportedVersion`, 클래스가 아닌 타입은 그래프 TypeMismatch → `CorruptPackage`로 갈렸다 — `AssetPackage_NonClassTypeHasNoCurrentVersion`). 미등록 클래스의 자리표시 객체(UE의 누락 클래스 처리)는 그래프 복원에 노드 생략·참조 null화를 더해야 하는 설계 변경이라 이 단계에서 하지 않는다(5.7절 9의 "에디터는 읽기 전용"은 reader 판정의 뜻이다).
19. **로드의 타입·객체 조회 실패**(판정 리뷰 반영): 로드가 찾은 객체·등록 저장소에 다시 묻는 조회의 실패는 단언하지 않고 진단 None + 원래 오류로 돌려준다.
    - import 대상(대표 에셋·하위 객체)의 `AcquireType`: 단언하지 않고 진단 None으로 그 오류를 돌려준다. 파생 판정은 그 lease(`TypeLease::IsA`)로 해 등록 저장소를 다시 묻지 않는다. 닿는 경로는 `InstantiatePackages` 도중 닫히는 TypeRegistry다: `FindObject`·`FindChild`·`Resolve`는 ObjectRegistry의 종료만 보지만 `AcquireType`은 객체의 타입 lease를 복제하고(`AcquireTypeChecked` → `TypeLease::Clone`), 그 복제는 TypeRegistry가 닫혔으면 `ShuttingDown`이다. `TypeRegistry::Shutdown`은 lease가 남아 `Busy`를 돌려줄 때도 닫힘 표시를 먼저 세운다. 시험 `AssetPackage_TypeRegistryClosesDuringInstantiate`는 상태 allocator가 Prepare 뒤 n번째 할당(첫 할당은 import 해석 전의 대상 표 예약)에서 Shutdown을 부르게 해(모든 n) 결과가 성공이거나 진단 None + `ShuttingDown`임을 보고, resolver 장면(Hard 의존의 대표 에셋 대조)과 묶음 장면(EditorOnly 참조만 — 대조 없이 import 대상 조회) 양쪽에서 ObjectImports 지점에 닿음을 센다. 처음 이 항목은 "지금 엔진에서 닿는 로드 경로가 없다"고 적었는데 이 경로를 놓쳤다. 판정 리뷰가 처음 든 경로(정책 없는 Registry의 `RequestDestroy`만 받은 대상)는 그런 Registry에 패키지를 로드할 수 없어 닿지 않는다 — 패키지는 `packageNativeType = Package`인 Registry에만 로드한다(`InstantiatePackages` 인자 `InvalidArgument` — 정책 없음·다른 타입 모두, `AssetPackage_LoadRequiresPackagePolicyRegistry`; 처음 구현은 그런 Registry로 연 트랜잭션도 받아 그래프 생성 단계가 Objects `CorruptPackage`로 거부했다).
    - Prepare의 대표 에셋 Asset 파생 판정: 그래프 1단계 전에 export 1 타입의 lease를 잡아 그 lease로 판정한다. lease를 잡는 조회가 실패하면(Prepare가 worker에서 도는 동안 `TypeRegistry::Shutdown`이 `closed_`를 먼저 세운 경우의 `ShuttingDown` 등) 그 오류를 그대로 돌려준다(5.3절). 처음 구현은 `types_.IsA`를 그래프 뒤에 불러 그 사이 Shutdown이면 단언이 걸렸다. 시험 `AssetPackage_TypeRegistryClosesDuringPrepare`는 Prepare의 상태 allocator가 n번째 할당에서 TypeRegistry를 닫게 해(모든 n) 결과가 성공이거나 `ShuttingDown`(진단 None)임을 보고, 대표 에셋 lease 지점에도 닿는다.
    - writer 쪽 같은 계열 단언(Admit hook의 `GetOutermost`·`AcquireType`, `ImportBuilder::Add`의 Outer 사슬 `GetOuter`·`AcquireType`)은 안전하다. 그래프 저장이 Exclude 정책에서 외부 참조마다 `WriteUnsaveable`로 대상과 조상을 `AcquireType`으로 먼저 확인하므로 hook·import 빌더가 보는 대상은 방금 생존·타입이 확인된 객체다(같은 소유 스레드, 그 사이 Registry 변경 없음). 대상은 언제나 대표 에셋이거나 그 아래이므로 이 확인이 대표 에셋까지 덮는다: 대표 자식(빈 이름)이 있는 Outer는 이름 있는 자식을 가질 수 없고(정체성 규칙 8 — [ObjectFoundations](ObjectFoundations.ko.md) 1.5절), 대표 에셋이 없는 패키지의 객체를 가리키면 import 빌더가 대표 에셋 조회에서 `NotFound`로 먼저 끝난다(`AssetPackage_WriterEdgeRejections`). 그래서 export 순회의 "첫 export가 아니면 부모가 export"와 import 빌더의 "사슬이 패키지에 닿기 전에 대표 에셋을 지남"도 단언이다.

## 6. 에셋 타입·객체 통합·참조

### 6.1 객체 통합

- 로드된 패키지·에셋은 `AssetManager`가 소유하는 **에셋 객체 Registry**(고유 ContextId, 엔진 TypeRegistry 공유)에 있다. World Registry와 분리한다(여러 World·Tryout 공유, 에셋끼리 ObjectRef 가능). 접근은 `AssetManager::With`(에셋 문맥 ObjectAccessScope를 중첩해 연다).
- `Package : Object`(Outer·이름 없음 — 정체성은 AssetId): `ROCKS_PROPERTY(Transient, ReadOnly) AssetId id`, 플래그, 하드 의존 패키지 목록(`ROCKS_PROPERTY(Transient) ObjectRefArray<Package>` — AssetManager 조회용 사본이며 GC 보존 역할은 없다, 존치 여부는 A1이 정한다), bulk 접근자(`ROCKS_NATIVE`: 에디터 패키지 파일의 열린 핸들 + BulkIndex. 지연 읽기를 하며 저장 성공 후 새 파일로 다시 잇는다, 5.5절). 소속 객체(export)는 목록이 아니라 Registry 자식 고리가 붙잡고(패키지 정책 Registry의 outer → inner 강한 간선 — [ObjectFoundations](ObjectFoundations.ko.md) 1.7절) export 순서는 `CollectInners(DepthFirst)`다. `Asset : Object`(`ROCKS_CLASS(Abstract)`)의 Outer는 Package. inner → outer 강한 간선(계획 §5.2)과 패키지 정책의 outer → inner 강한 간선이 함께 있다.
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
- 캐시: `Intermediate/AssetIndex.cache`(형식·Unicode·스키마 버전 키 — 도구 빌드 ID는 header에 진단용으로만 기록해 엔진을 다시 빌드할 때마다 캐시가 무효화되지 않게, 원자적 기록, 잠금 보유 프로세스만 기록 — 읽기 명령은 캐시를 읽기만). 파일별 헤더+섹션 표 SHA-256과 요약. 시작 시 범위(약 1 KiB)만 읽어 비교, 다르면 그 파일만 다시 읽음. 범위 읽기는 WorkerScheduler 병렬.
- `Verify()`: 4.6절 진단 보고만.
- 원본 감시: 원본 SHA-256이 ImportRecord와 다르면 `SourceChanged`. 자동 재임포트는 기본 꺼짐, 켜면 debounce(500 ms) 후 재임포트, 실패 시 기존 보존·진단(계획 §9.4).

## 8. 동시성

- 쓰기 잠금: `Intermediate/AssetDatabase.lock`을 `FileLock`(Windows `LockFileEx` + 삭제 공유 없는 열기, POSIX `flock` + 잠근 뒤 dev/ino 재확인)으로 잡은 프로세스만 에셋·별칭 표·인덱스 캐시·쿠킹 출력을 바꾼다. 잠금을 잡는 명령은 13절 목록이며 서비스 API도 같은 규칙이다. 쿠킹도 이 잠금을 쥐므로(에디터 패키지가 쿠킹 중 바뀌지 않게) 같은 프로젝트의 쿠킹은 프로파일과 무관하게 직렬화되고 쿠킹 출력에 따로 잠금을 두지 않는다. 못 잡으면 `Busy`(`--wait=<초>`).
- 에셋 너겟은 내용 주소 저장소라 잠금 없이 동시 기록(9절).

## 9. 에셋 너겟

- 위치: `<AssetNuggets>` = `Config/AssetNuggets.json`의 `localPath`(기본 `<Project>/Intermediate/AssetNuggets/`). 프로젝트 사이 공유 경로도 쓸 수 있다(내용 주소라 안전하다).
- 에셋 너겟은 "캐시 키 → 가공 결과" 저장소다. 같은 에셋이라도 설정·대상 플랫폼이 다르면 결과가 다르므로(예: Windows용·Android용 `T_Wall`은 다른 키) **결과에 영향을 주는 입력은 빠짐없이 키에 들어가야 한다.** 빠진 입력은 오래된 결과를 돌려주는 버그가 되고, 불필요한 입력(경로·시각·locale)은 적중률만 떨어뜨린다(계획 §9.1).
- 키: 처리기 `BuildKey(KeyWriter&)`가 고정 순서·타입 태그로 기록한 바이트의 SHA-256. 모든 처리기에 공통으로 들어가는 것: `"RocksAssetNugget" 형식 버전`, 처리기 Guid·처리기 버전 Guid, 결과를 만드는 **인코더·외부 라이브러리의 이름·버전·설정**(블록 압축 인코더 등 — 비압축이면 없음), **결정적 수학 버전**(A0-C), **툴체인 지문**(컴파일러 버전·컴파일러/링커 바이너리 해시·대상 triple·부동소수점 모델 — 빌드가 상수로 주입, [ReflectionFoundations](ReflectionFoundations.ko.md) 8.1절), 에셋 TypeId, 입력 bulk SHA-256, 결과에 영향을 주는 설정 전체(기본값 포함, float는 bit 패턴, NaN 단일화, `CompressionCacheId` 포함), 대상 프로파일 id와 출력 설정 전체, 의존 입력 (역할, AssetId, 그 의존 에셋의 같은 처리기 build key) 정렬 목록 — 의존의 설정·전이 Build 의존 변화까지 키에 들어간다(Merkle). 경로·시각·locale 제외. 엔진 버전도 넣지 않는다(출력 의미가 바뀌면 처리기 버전 Guid를 바꾼다 — [ReflectionFoundations](ReflectionFoundations.ko.md) 8.2절).
- Build 의존 순환(지원 SCC, 6.2절)에서는 서로의 key가 서로를 포함해 값이 정해지지 않으므로 SCC 단위로 계산한다: SCC key = SHA-256(구성원의 지역 입력(위 항목 중 의존을 뺀 것)을 AssetId 순으로, SCC 밖 의존 튜플 (구성원 AssetId, 역할, 의존 AssetId, 그 key)의 정렬 목록), 구성원 key = SHA-256(SCC key, AssetId). 순환이 없어도 이 식 하나만 쓴다(구성원 하나인 SCC — 값 체계를 하나로 둔다).
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
- 값: `<AssetNuggets>/<key[0..2]>/<key>.rnug` = `{ "RNUG", 버전, key, 부분 수, 부분별(크기·SHA-256), 본문 }`(텍스처는 mip·slice별 부분).
- 읽기: 검증 실패 → `CorruptEntry` 진단·삭제·미적중(경합으로 정상 항목을 지워도 재생성되므로 무해).
- 쓰기: `<AssetNuggets>/tmp/<guid>` 기록·Flush → `Move(no-replace)`. 대상이 있으면 성공(게시 항목 불변). Put 실패는 치명적이지 않다. 전원 차단 뒤의 손상 항목은 읽기 검증이 거른다.
- 정리: 적중 시 `SetModifiedTime`, `nugget-gc --max-bytes --unused-days`, `tmp` 24시간 경과 삭제.
- 인터페이스: `NuggetBackend { Get, Put, Remove, Touch }`, 계층 `NuggetCache`(공유·원격은 계약). `--verify-nuggets`(적중 결과 = 재생성 결과 대조).
- 원본 bulk는 에디터 패키지가 권위 저장소(계획 §9.4)이므로 에셋 너겟을 지워도 손실 없음. Virtual Asset(원본 bulk 분리 저장소)은 후속(17절).

## 10. Cooker

- `TargetProfile`(엔진 `Engine/Config/TargetProfiles/<id>.json` + 프로젝트 재정의): id, 그래픽 backend, 형식 지원 표(비압축, 블록 압축 지원 = 이번에는 없음), LOD 그룹 표(12.4.5절), bulk 정렬. 엔진 기본: `Windows`, `Android`(형식 표만, 그래픽 backend 후속). JSON은 리플렉션 공용 codec `ReflectionText`(struct 멤버 병합, Map은 선택 `MergeByKey` — ReflectionExtensions 10.4절)로 읽고, 프로젝트 재정의를 그룹 단위로 덮는 방식은 A4가 고른다.
- `CookSettings`(`Config/CookSettings.json`): AlwaysCook·NeverCook(경로·디렉터리), PrimaryAsset 규칙(타입 → 디렉터리·bundle 목록), `CookSoftReferences`. 같은 codec으로 읽는다.
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
- 런타임 쿠킹 루트: 도구(`asset-load-cooked`)는 `<Project>/Saved/Cooked/<Profile>/`. 게임은 `EngineAssets`가 명령행 `-CookedRoot=<dir>`(없으면 실행 파일 디렉터리의 `Cooked/`)를 `AssetManager`에 넘긴다. 쿠킹 결과를 배포 위치로 옮기는 패키징 단계는 후속이다.
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
         | bulkRawBytes u64 | cookKey (32) | flags u32 | bundleCount u32 | bundle string × n
         | dependencyCount u32 | { assetId Guid, kind u32 } × m
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

## 11. 런타임 로딩과 수명(계획 §9.2 계약)

### 11.1 API

```cpp
struct AssetRequest { AssetId id; TypeId expectedType; LoadPriority priority; ReadyMask required; CancelToken cancel; AssetScope* scope; };
Result<LoadTicket> RequestAsset(const AssetRequest&);
Result<void> OnReady(LoadTicket&, TaskLane lane, Function<void(Result<AssetLease>)>&& callback);
Result<AssetLease> LoadBlocking(const AssetRequest&);
```

- LoadTicket = 요청 관심, AssetLease = 상주 수명(별도 계수). 동시 요청은 합류, caller별 취소는 독립, callback은 caller별 정확히 한 번 terminal. `OnReady` 중복 등록은 `AlreadyRegistered`, 등록 실패 시 callback 미등록·소유권 caller 유지(계획 §9.2).
- `ReadyMask`: `Cpu` 구현, `Render` 계약(`UnsupportedCapability`).
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

A1c 구현(`Engine/Source/Runtime/AssetRuntime` — `CancelToken.h`·`AssetLoader.h`·`CookedPackageIo.h`, 상태: **구현** — 시험 14.1.5절, coverage 14.1.6절)이 11.1–11.5절에서 비어 있던 것을 다음과 같이 정했다. A1c는 쿠킹 매니페스트에서 패키지를 읽어 에셋 객체 Registry에 공개하는 로더다. 요청 관심·lease·scope·`OnReady` callback·상주 단위의 계수와 Closing(11.1·11.4절)은 그 위의 `AssetManager`(A1d)가 맡는다. A1c의 로더는 공개한 패키지를 `Shutdown`까지 붙잡는다(해제 경로는 A1d가 단위 계수로 더한다).

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
17. **시험 hook**: `AssetLoaderConfig::waitEntered`는 완료 대기가 실제로 조건 변수에서 기다리기 직전(큐 뮤텍스를 쥔 채)에 불린다. 시험이 "Game 스레드가 기다리기 시작한 뒤"를 사건으로 받아 완료를 내보내게 한다(시간에 기대지 않는다). 로더를 다시 부르면 안 된다.
18. **콜백 재진입**: PostLoad handler·관찰자(`OnSyncWait`)·시험 hook(`waitEntered`)이 불리는 동안 로더는 "콜백 중" 표시를 쥔다. 그 사이 상태를 바꾸는 공개 함수(`Request`·`SetPriority`·`Release`·`Pump`·`WaitForCompletions`·`WaitFor`·`Shutdown`)는 `WrongContext`를 돌려주고 아무것도 바꾸지 않는다(단위 표 재할당·같은 단위의 중첩 완료를 막는다). 읽기(`Status`·`Describe`·`FindByPath`·`FindPackage`)는 된다. 콜백이 더 일을 시키려면 돌아간 뒤 Pump 밖에서 한다(UE도 PostLoad 안 동기 로드는 금지·경고한다).

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
| JPEG, 손실 WebP, 손실 압축 EXR(B44·B44A·DWAA·DWAB, 32비트 float 채널의 PXR24), JPEG 압축 TIFF, 블록 압축 DDS(BC1–7 등) | `LossySource`로 거부(사용자 결정: 무손실만). 블록 압축 DDS는 UE 5.8도 원본으로 받지 않는다 — DDS 파서는 BC를 인식하지만 원본 형식 대응표(`DXGIRawFormatMap`, `DXGIFormatGetClosestRawFormat`)에 없어 "DXGIFormat not supported"로 거부(ImageCore/Private/DDSFile.cpp 1289·1350, UnrealEd/Private/Factories/EditorFactories.cpp `UTextureFactory::ImportDDS` 3289·3320–3325) |

2026-11-13 사용자 결정: DDS(비압축)·TIFF(무손실)·Radiance HDR·무손실 OpenEXR 디코더를 A5 범위에 넣는다(HDR·EXR은 처음에 "후속"이었다). PCX는 UE가 받지만 쓰이지 않으므로 지원하지 않는다. IES(조명 프로파일)는 텍스처 원본이 아니라 별도 에셋 종류라 조명 단계, UDIM은 가상 텍스처(12.4절 계약)와 함께 다룬다. 블록 압축(BC·ASTC·ETC)은 원본이 아니라 쿠킹 결과 형식이며 텍스처 파이프라인의 압축 단계(화면 마일스톤 뒤, 사용자 결정 순서)에서 인코더와 함께 구현한다.

2026-11-20 사용자 결정(무손실 원칙의 좁은 예외): UE 5.8은 JPEG로 임포트한 텍스처를 기본 설정에서 JPEG 바이트 그대로 원본으로 보관한다. 그래서 [Unreal 교환 형식](UnrealExchange.ko.md)(4.3절)으로 가져오는 텍스처 가운데 원본부터 손실 데이터인 것이 있다. 이런 텍스처는 기본으로 `LossySource` 오류다. 명시 옵션 `--allow-lossy-origin`을 주면 해제한 픽셀을 원본으로 받고, ImportRecord의 `options`에 옵션을, 결과에 `lossyOrigin` 표시를 남기며 `LossyOriginAccepted` 경고를 낸다. 옵션이 `options`에 남으므로 재임포트도 같게 판단한다. 이 예외는 Unreal 교환 경로에만 있고, 파일 원본 임포트(위 표)의 JPEG 거부는 그대로다.

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
| `CompressionFormat`(None·PNG·JPEG·UEJPEG·UEDELTA) | 정규 픽셀 + Zstd(원본 압축 바이트를 저장하지 않음 — 12.1절 근거, JPEG는 손실이라 받지 않음) | 구현(설계 차이) |
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
asset-reimport <path> [--source=<파일> | --source.<역할>=<파일>…] [설정…]
asset-new <Type> <path> [<속성>=<값>…]
asset-set <path> <속성>=<값> [<속성>=<값>…]
asset-list [--type=<Type>] [--under=<path>]
asset-info <path|AssetId>
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
asset-cook --profile=<id> [<path>…] [--verify-nuggets]
asset-load-cooked --profile=<id> <AssetId|경로 힌트>
nugget-gc --max-bytes=<n> --unused-days=<n>
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

서비스 API(`AssetService`, AssetDatabase)는 명령과 1:1 함수이며 에디터가 나중에 같은 함수를 쓴다.

## 14. 검증 시나리오

1. PNG 임포트·저장 → 새 프로세스에서 같은 AssetId·픽셀
2. 원본 PNG 삭제 후 에디터 패키지만으로 쿠킹
3. 원본 변경 재임포트: ID·참조 유지, 해시 갱신, 에셋 너겟 무효화
4. 잘못된 PNG·원본 누락·잘못된 옵션 재임포트 실패 시 기존 패키지 바이트 불변
5. 이동·이름 변경(디렉터리 포함) 후 ID 참조 유지, 별칭 해석·경고·만료
6. 복제본 새 ID·내부 참조 재매핑
7. 중복 ID·깨진 참조·TypeMismatch·UnsupportedCycle 진단, 하드 순환 SCC 로드
8. 인덱스 캐시 삭제 후 재구축, 에디터 밖 이동·삭제 후 재검색, 감시 Overflow·RootLost 복구
9. 에셋 너겟 적중·설정 변경 무효화·툴체인 지문 변경 무효화·손상 복구·두 프로세스 동시 기록·GC
10. mip 크기·대표 픽셀(12.5절 표: sRGB·알파·홀수·Unfiltered·PreserveBorder·FlipGreen·NoAlpha(비압축 결과 불변)·Normalize·Volume·Cube)
11. 쿠킹 패키지만으로 로드, EditorOnly 데이터·필드 부재
12. 잘못된 패키지(magic·CRC·섹션 순서·겹침·범위·크기)·UnsupportedVersion·NewerVersion
13. 쿠킹 재현성(에셋 너겟 빈/찬, 두 번 쿠킹 동일 바이트, Debug·Release 도구 사이 동일 바이트), 부분 쿠킹 병합·전체 쿠킹 정리, 실패 시 게시본 불변
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
29. 저장: 이전 파일의 손상 bulk entry 복사 거부, bulkId 재매핑 후 지연 읽기, 저장 중 편집 시 재연결 생략, 대체 경로(FAT 등)에서 자기 핸들 때문에 실패하지 않음

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

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/AssetRuntime/Tests/ManifestTests.cpp`·`ManifestScaleTests.cpp` — 정의된 시험 16개를 모두 표가 인용한다). 시험 도구(`ManifestTestSupport.h`)의 시험 인코더는 검증하지 않고 10.1절 1 배치로 쓰며(규칙을 어긴 모델을 reader에 넣는다), writer 출력과 byte가 같음을 왕복 시험이 확인한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 배치·결정성(10.1-1) | 쓰고 읽으면 같은 모델, 다시 쓰면 같은 byte, 시험 인코더 = writer byte, 빈 매니페스트, 비ASCII NFC 경로·bundle(`/Game/Ärger`·`Äb`) 왕복, `Find`(기본 장면의 있는 id 셋, 10·12·14 장면에서 있는 id 셋·없는 id 앞 1·사이 11·13·뒤 15) | `CookedManifest_RoundTrip` |
| | 고정 byte(고정 장면 출력 SHA-256, 체크인한 v1 byte와 같음, 그 byte를 읽고 다시 쓰면 같음) | `CookedManifest_GoldenBytes` |
| 판정 순서·진단(10.1-2) | 최소 길이 미만(빈 입력·모든 길이), 파일 상한 초과(내용 읽기 전 — 길이만 큰 span이라 이 순서에 기댄다), magic 네 byte, formatVersion 0·2·최댓값(UnsupportedVersion / Unsupported, detail = NoEntry·asset null), CRC(끝 칸, 개수 칸 최상위 byte — CRC 미보정이면 Crc·보정하면 EntryCount라 CRC가 구조보다 먼저), 남는 byte, 모든 길이 자르기(CRC 맞춤 — 언제나 손상, 빈 매니페스트는 헤더 칸 다섯 각각의 잘림에 닿음) | `CookedManifest_StructureTampering` |
| | 항목 칸 잘림: 항목 둘인 매니페스트를 항목 1 안의 모든 위치로 자름 → 칸 11개(assetId·typeId·경로·SHA-256·packageBytes·bulkRawBytes·cookKey·flags·bundle 개수·bundle·의존 개수) 범위표대로 그 칸·항목 1·asset(assetId 잘림은 null, 나머지 11) | `CookedManifest_EntryTruncation` |
| | 겹친 위반의 우선순위 표 10줄: 길이 10 + magic → File, magic + 버전 → Magic, 버전 + CRC → UnsupportedVersion, 빈 프로파일 + CRC → Crc, 남는 byte + 빈 프로파일 → File, 항목 0 flags=2 + 항목 1 bundle 257개 → reader BundleCount/1·검증·writer Flags/0, 경로 중복 + 쿠킹되지 않은 Hard → PathHint/2(세 경로), 병합 profileId + cookerVersion → ProfileId, 패키지 대조 길이 다른 파일 + 다른 패키지 내용 → PackageBytes, SHA만 다른 파일 + 다른 패키지 내용 → PackageSha256(같은 파일이면 AssetId — 대조) | `CookedManifest_JudgementOrder` |
| 상한(10.1-3) | 항목 수(모델 2²⁰ + 1 — 검증·쓰기, 항목 배열 약 226 MB를 한 번에 잡는 시험; 개수 칸 상한 + 1·남은 byte로 담을 수 없음·실제보다 큼·작음), bundle 256 받음·257 거부·개수 칸 초과, 의존 65536 받음·65537 거부·개수 칸 초과, 프로파일 4096 byte 받음(쓰기·읽기)·4 KiB + 1·길이 칸이 남은 byte보다 큼, 경로 256 받음·257 거부(정확히 상한 + 1)·265 거부 | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering`, `CookedManifest_EntryCountLimit` |
| | 할당 상한: 개수 칸이 큰 입력(항목 1000·bundle 100000·의존 50000)과 정상 입력에서 reader의 가장 큰 할당 요청 ≤ 입력 byte × 4(기록 allocator) | `CookedManifest_AllocationBound` |
| 헤더 칸(10.1-4) | 프로파일 빈 값·잘못된 UTF-8(byte), 엔진 버전 세 칸 각각 u16 초과·세 칸 모두 0xFFFF 받음 | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering` |
| 항목 칸(10.1-5) | 위반마다 칸·항목과 asset·related를 검증·writer·reader 세 경로에서 단언(헤더 칸 asset null, 경로 중복은 그 항목 asset), assetId null·중복·역순, typeId null, 경로(빈 값·상대·마운트만·빈 구성 요소·끝 '/'·공백·'.'·예약 이름·비 NFC·'\\'·길이·잘못된 UTF-8 byte), packageBytes 63 거부·64 받음·64 GiB 받음·+1 거부, bulkRawBytes 최댓값 받음, 정의되지 않은 flags 비트 | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering` |
| | bundle(PrimaryAsset이 아닌 항목·이름 규칙·역순·중복·caseless 중복(ASCII·비ASCII `Äb`·`äb`)·잘못된 UTF-8 byte), 의존(null — related null·자기·kind 0·3·4·5·6·최댓값·역순 — related 5·중복, 의존 위반은 related = 그 의존 대상) | `CookedManifest_FieldRules`, `CookedManifest_StructureTampering` |
| 항목 사이(10.1-6) | 경로 caseless 중복(둘·셋·비ASCII `/Game/Ärger`·`/game/ärger` — 진단 항목은 정렬 뒤 첫 짝의 뒤), 쿠킹되지 않은 Hard 대상은 related = 그 대상(99), 쿠킹되지 않은 Hard 대상(작은 장면·2만 항목 사슬 가운데 삭제), 쿠킹되지 않은 Soft 대상 받음, Hard 순환 받음 | `CookedManifest_FieldRules`, `CookedManifest_LargeManifest`, `CookedManifest_AllCombinations` |
| writer(10.1-7) | 위반은 쓰지 않음(미리 채운 out `{0xAA, 0xBB}` 그대로·검증과 같은 진단 — 위반 목록 전부와 항목 수 상한) | `CookedManifest_FieldRules`, `CookedManifest_EntryCountLimit` |
| | 파일 상한은 CRC 칸 포함 전체: `WriteCookedManifestWithLimit` 상한 = 출력 크기는 받음(공개 writer와 같은 byte), 출력 크기 − 1은 `CapacityExceeded`(진단 None, out 그대로) | `CookedManifest_WriteLimit` |
| 대조(10.1-8) | 실행 프로파일 같음·다름·대소문자만 다름(IncompatibleCook), 병합 네 칸 각각 다름(그 칸)·엔진 버전만 다름은 받음 | `CookedManifest_ProfileAndMergeChecks` |
| | 패키지 대조: A1a writer가 쓴 쿠킹 패키지(Hard·Soft 의존 — Soft 대상 1700이 Hard 대상 1801보다 앞인 (asset, kind) 순서, 같은 대상 Hard+Soft 패키지 — ·Zstd bulk, EditorOnly bulk는 raw 합에서 빠짐)가 그 결과로 만든 항목과 맞음, 크기(파일 덧붙임·항목)·SHA-256(파일·항목)·종류(에디터 패키지)·assetId·대표 에셋 타입·export 없는 내용·bulk raw 합(하나 작음·u64를 넘는 합 — 감으면 같은 값)·프로파일(id·설정·쿠커 버전)·의존(빠짐·남음·종류·대상·전부 없음·종류 먼저 순서·같은 대상의 Soft 빠짐·종류 뒤바뀜 — related는 처음 다른 의존 대상 1801·1900·1801·1899·1700) | `CookedManifest_PackageCrossCheck` |
| 파일 경로(4.2) | `Packages/<a>/<A>-<h>.rpkg`(앞 내용 뒤에 붙임), 모든 할당 지점 실패 | `CookedManifest_PackagePath` |
| 자원 실패 | 쓰기·읽기·검증의 모든 할당 지점(진단 None, 미리 채운 쓰기 out 그대로 — 비교 키 성장, NFC quick check가 MAYBE인 경로·bundle(U+1EA1 U+0301)의 NFC 확인 scratch와 비ASCII caseless 키 할당 포함) | `CookedManifest_AllocationFailures` |
| 조합(14 완료 조건 2) | (PrimaryAsset × bundle 없음·하나·여럿 + 일반) 4 × 의존 특성 다섯(쿠킹된 Hard·Hard 순환·쿠킹된 Soft·쿠킹되지 않은 Soft·같은 대상 Hard+Soft)의 부분집합 32 × 경로(얕음·깊음·비ASCII NFC) 3 = 384 조합 전부(읽은 항목마다 조합 번호를 다시 계산해 만든 번호와 같음 — 일대일, 플래그가 늘면 `static_assert`가 막음), 금지 조합(일반 항목의 bundle)은 칸 규칙 | `CookedManifest_AllCombinations`, `CookedManifest_FieldRules` |
| 규모 | 항목 2만(깊이 2만 Hard 사슬·8개 단위 순환·쿠킹되지 않은 Soft·PrimaryAsset bundle) 왕복·Find·같은 byte | `CookedManifest_LargeManifest` |
| 변조·퍼징 | 고정 seed 4000회(byte 바꾸기·자르기·덧붙이기, 절반 CRC 맞춤 — 정해진 진단만, 거부·받아들임 모두, 받아들인 입력은 다시 쓰면 같은 byte) | `CookedManifest_FixedSeedMutationFuzz` |
| | 게시(임시 파일·Flush·Replace)·부분 쿠킹 병합 실행·cookKey 계산 | 범위 밖(A4 쿠커 — 대조 함수는 여기) |
| | 서명 봉투 `CookedManifest.rsig` | 범위 밖(A1f — reader는 span을 받는다) |
| | Hard SCC 계산·`FindByPath` 색인·`maxBulkRawBytes` 적용·패키지를 열 때 대조 호출 | A1c 로더 — 14.1.5절 |

#### 14.1.4 Coverage(A1b)

측정(2026-11-20, 14.1.2와 같은 명령에 filter `CookedManifest`를 더함 — `-Filter AssetPackage,FixtureAsset,CookedManifest`, AssetPackage 71개·FixtureAsset 8개·CookedManifest 16개 시험 모두 통과): 매니페스트 파일 `Private/CookedManifest.cpp` 줄 100%(540/540)·분기 100%(296/296)·함수 100%(24/24), `Public/AssetRuntime/CookedManifest.h` 줄 100%(6/6)·함수 100%(4/4)(분기 없음). 닿지 않은 분기가 없다. AssetRuntime 전체(A1a 파일 포함)는 줄 99.91%(3170/3173)·분기 97.56%(1842/1888)·함수 100%(150/150)이고 닿지 않은 분기는 14.1.2의 A1a 항목 그대로다.

`ROCKS_ASSERT`는 쓰지 않지만, 결과를 확인하지 않는 불변식이 10곳 있다(`Result::Value()`는 실패 결과면 `platform::Fatal`로 멈추고, `(void)`로 버린 결과는 실패하면 조용히 빠진다 — 그래서 각각 입력으로 실패에 닿을 수 없음을 근거로 둔다). 위치는 `Private/CookedManifest.cpp`의 함수 이름으로 적는다(줄 번호는 병합·개명으로 밀린다).

| 위치 | 불변식 | 근거 |
|---|---|---|
| `ValidateBundles` | bundle 비교 키 `keys.TryEmplaceBack(scratch).Value()` | 바로 앞에서 bundle 수만큼 `TryReserve`했고, 빈 `String` 추가는 할당하지 않는다 |
| `ValidateCookedManifest` | 경로 비교 키 `keys.TryEmplaceBack(scratch).Value()` | 항목 수만큼 예약했다(같은 이유) |
| `ValidateCookedManifest` | `(void)order.TryPushBack(...)` | 항목 수만큼 예약했다 — 예약 안 추가는 실패하지 않는다 |
| `ReadCookedManifest` | `ArchiveReader::Create(...).Value()` | 넘기는 span 길이 ≤ `limits.maxTotalBytes`(= body)라 Create의 유일한 실패(`CapacityExceeded`)가 없다 |
| `ReadCookedManifest` | 항목 수 칸 `in.ReadU32()` 결과의 `Value()` | 길이 ≥ `MinFileBytes`(magic·version·개수·crc)를 먼저 보아 개수 칸 4 byte가 있다 |
| `ReadCookedManifest` | 항목 `manifest.entries.TryEmplaceBack(allocator)` 결과의 `Value()` | 개수 칸 값만큼 예약했다(항목 생성자는 할당하지 않는다) |
| `ReadCookedManifest` | bundle `entry.bundles.TryEmplaceBack(allocator).Value()` | bundle 개수만큼 예약했다 |
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

항목마다 그것을 확인하는 시험이다(EngineTests, `Engine/Source/Runtime/AssetRuntime/Tests/LoaderTests.cpp`·`LoaderFailureTests.cpp`·`LoaderIoTests.cpp`·`LoaderStageTests.cpp`·`LoaderCombinationTests.cpp`·`LoaderStagedCombinationTests.cpp` — 정의된 시험 44개를 모두 표가 인용한다. PlatformBase 결과 주입은 `AsyncFile_InjectedFaultsAreDeterministic`). 시험 도구(`LoaderTestSupport.h`)는 A1a writer로 쓴 쿠킹 패키지와 그 결과로 만든 매니페스트(장면), 완료를 시험이 내보내는 IO(`TestPackageIo` — 경로마다 없음·완료 오류·Begin 즉시 실패를 끼운다), 손으로 돌리는 worker(`TestLoaderWorker`), 부를 때마다 정해진 만큼 가는 시계를 준다. 실제 스레드를 쓰는 시험(디스크 IO·WorkerScheduler·다른 스레드 호출·종료 대기)은 결과만 단언한다.

| 요구(절) | 항목 | 시험 |
|---|---|---|
| 요청·결과(11.6-1) | 사슬 요청 → Succeeded·패키지 handle = FindPackage, 공개한 패키지 요청은 그 자리에서 Succeeded(다시 읽지 않음), Release 뒤 StaleHandle(칸 재사용 뒤 세대 불일치 포함), 매니페스트에 없는 id NotFound / MissingDependency, 범위 밖 우선순위 InvalidArgument, Initialize의 매니페스트 재검증(빠진 Hard 대상 → CorruptPackage Dependency) | `AssetLoader_LoadsChainAndJoins`, `AssetLoader_InitializeAndArguments`, `AssetLoader_InitializeValidatesManifest` |
| ResolveManifest·SCC(11.6-2) | Hard 폐포(Soft는 따라가지 않음 — 쿠킹되지 않은 Soft 대상), 요청 직후 단위 수·구성 수가 SCC와 같음(단일·둘·셋 순환·다이아몬드·진행 중 순환에 기대는 사슬·꼬리가 나가는 순환), 단위 하나 예산에서도 순환 구성은 함께 공개, 순환 참조가 서로의 대표 에셋을 가리킴, 큰 그래프(10개 순환 블록 40개 + 깊이 1500 사슬) | `AssetLoader_HardSccShapes`, `AssetLoader_LargeGraph`, `AssetLoader_LoadsChainAndJoins` |
| | `FindByPath`(caseless 키·없는 경로 NotFound·규칙 위반 InvalidArgument, 비ASCII NFC 경로를 대문자 Ạ·소문자 마운트로 찾음) | `AssetLoader_FindByPath`, `AssetLoader_FindByPathNonAscii` |
| | 공개된 의존에 기대는 요청(단위 하나·대기 이유 Admission·Hard 참조 연결), 허가된 의존 단위에 기대는 늦은 요청 | `AssetLoader_PublishedDependencyIsResolved`, `AssetLoader_LoadsChainAndJoins` |
| 패키지 단계(11.6-3) | IO → worker(크기·SHA-256 → Prepare → 내용 대조) → Prepared → 의존 공개 대기 → Pump 공개, 단계별 대기 이유·누적 시간·임시 byte | `AssetLoader_DescribeTracksStages` |
| 경로·IO(11.6-4) | `<루트>/` + `AppendCookedPackagePath`, 디스크 파일을 AsyncReadFile 조각 읽기로(bulk 그대로), 파일 없음 NotFound / MissingFile, 디스크 크기 다름 PackageBytes, 완료 오류 → 진단 None과 그 오류, Begin 즉시 실패(순환 둘째 패키지에서 — 첫째 IO는 끝난 뒤 버림), 기본 IO의 OS 오류(결과 주입 — 크기 조회·둘째 조각 제출·첫 조각 완료 실패 → 진단 None·DependencyFailed), 짧게 읽힘(완료 게시 보류 사이 파일을 1/4로 다시 씀 → InvalidArgument·진단 None) | `AssetLoader_LoadsFromDiskWithWorkers`, `AssetLoader_IoAndWorkerErrors` |
| | PlatformBase 결과 주입(Size·Submit·Completion, skip, 한 번 뒤 사라짐, ClearFault, 두 backend) | `AsyncFile_InjectedFaultsAreDeterministic` |
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
| PostLoad(11.6-16) | 의존 단위 먼저·단위 안 AssetId 순(2·3·4·1), maxPostLoads(0이어도 Pump마다 하나)·시간 예산(첫 호출 뒤 다하면 소진 표시)·공개가 단위 예산을 다 써도 PostLoad 하나(굶주림 방지), 요청은 PostLoad 뒤 Succeeded(그 전 PostLoading — 의존 PostLoad 대기 → 자기 차례), PostLoading 단위 합류, PostLoading 중 취소는 Cancelled지만 PostLoad 불림, PostLoad 중인 의존에 기대는 새 단위는 그 뒤(그 의존을 공개 대기로 세지 않음), 종료는 남은 PostLoad를 부르지 않음, PostLoad 대기 중 기대는 단위가 실패해도 그 단위는 버려지지 않고 PostLoad됨·고쳐 다시 요청하면 그 뒤에 PostLoad, handler가 받은 package = FindPackage | `AssetLoader_PostLoadOrderAndBudget`, `AssetLoader_PostLoadTimeBudget`, `AssetLoader_PostLoadRunsWhenInstantiateBudgetIsSpent`, `AssetLoader_DependentFailsWhileDependencyPostLoads`, `AssetLoader_PostLoadJoinCancelAndShutdown`, `AssetLoader_DependencyWaitSkipsPostLoadingUnits` |
| 콜백 재진입(11.6-18) | PostLoad handler·동기 대기 관찰자 안의 Request·SetPriority·Release·Pump·WaitForCompletions·WaitFor·Shutdown은 WrongContext, Status·Describe·FindByPath는 됨, 로더는 이어서 정상 완료 | `AssetLoader_CallbacksCannotReenter` |
| 조합(14 완료 조건 2) | 요청 단위 모양 3 × 의존 4 × 결함 5 × 진행 방식 2의 허가 조합 106개 전부(금지: 단일의 형제 손상·의존 없음의 의존 손상), 번호 ↔ 조합 일대일·실행 수로 빠짐 검사 | `AssetLoader_AllCombinations` |
| | 단계 조합: 모양 2 × 의존 상태 5(없음·새로·진행 중·PostLoad 대기·공개됨) × 결함 3 × 합류 3(없음·처음·PostLoad 대기 때) × 취소 시점 6(없음·허가 전·IO·worker·Ready·PostLoad 대기) × 완료 순서 2 × PostLoad handler 2(없음·기록)의 허가 조합 1264개(handler 있음 864·없음 400 — 금지: 의존 없음·PostLoad 대기·공개된 의존의 의존 손상, handler 없이 PostLoad 대기 의존·합류·취소), 조합마다 요청 상태·모든 구성·의존 패키지의 공개 여부·PostLoad 정확히 한 번·의존 먼저·handler package = FindPackage, 끝나면 단위·IO·임시 byte 0, 번호 ↔ 조합 일대일·실행 수(시험 시간 약 0.4 s) | `AssetLoader_StagedCombinations` |
| | 요청 관심·lease·scope·OnReady callback·상주 단위 해제(Closing·Discard)·ReadyMask·expectedType | 범위 밖(A1d AssetManager) |
| | 클래스별 PostLoad dispatch(`UObject::PostLoad` 대응 클래스 hook 등록) | 범위 밖(A5 — RuntimeClassMetadata와 함께, 단계·handler 경계는 여기) |
| | 미준비 객체 동작 정책(11.5절 2) | 범위 밖(A1d) |
| | Rocks Lens 이벤트 연결·프레임 판정 주입 | 범위 밖(EngineAssets — 관찰자·판정 함수는 여기) |

#### 14.1.6 Coverage(A1c)

측정(2026-11-20, 14.1.4와 같은 명령에 filter `AssetLoader`를 더함 — `-Filter AssetPackage,FixtureAsset,CookedManifest,AssetLoader`, AssetPackage 71개·FixtureAsset 8개·CookedManifest 16개·AssetLoader 44개 시험 모두 통과):

| 파일 | 줄 | 분기 | 함수 |
|---|---|---|---|
| `Private/AssetLoader.cpp` | 99.9%(1591/1593) | 99.8%(643/644) | 100%(87/87) |
| `Private/CancelToken.cpp` | 100%(90/90) | 100%(24/24) | 100%(17/17) |
| `Private/CookedPackageIo.cpp` | 100%(77/77) | 100%(24/24) | 100%(6/6) |
| `Private/CookedManifest.cpp`(파일·내용 대조 분리 뒤) | 100%(559/559) | 100%(298/298) | 100%(28/28) |
| `Public/AssetRuntime/AssetLoader.h`·`CancelToken.h` | 100%(7/7·8/8) | 분기 없음 | 100%(3/3·4/4) |

AssetRuntime 전체는 줄 99.90%(4963/4968)·분기 98.10%(2533/2582)·함수 100%(271/271)이고, A1a 파일의 닿지 않은 분기는 14.1.2 그대로다.

닿지 않은 곳(`AssetLoader.cpp` 한 분기와 그 두 줄): 소멸자의 "소유 스레드가 아님 → `platform::Fatal`"이다. 자식 프로세스 probe(`AssetLoader_DestroyOffOwnerThreadIsFatal` — 문구 대조)가 실제로 지나지만, Fatal은 프로세스를 즉시 끝내 그 자식의 coverage profile이 기록되지 않는다(측정은 부모 프로세스 profile만 모은다).

닿게 한 방법과 그 과정에서 바꾼 것:
- 결정적 순서: IO·worker를 시험이 내보내는 구현(`TestPackageIo`·`TestLoaderWorker` — 앞부터·뒤부터 완료)으로 단계마다 멈추고, 실제 스레드 시험(디스크·WorkerScheduler·다른 스레드 호출·종료)은 결과만 단언한다. 종료 대기는 대기 진입 hook(11.6절 17)이 보낸 사건 뒤에 다른 스레드가 완료를 내보내 대기 분기에 일정과 무관하게 닿는다. 시간 예산은 부를 때마다 정해진 만큼 가는 시계로 본다.
- 기본 IO의 OS 오류 경로: PlatformBase 결과 주입(`AsyncIoContext::InjectFault` — 크기 조회·조각 제출·완료, PlatformLayer 문서)으로 결정적으로 닿고, 짧은 읽기는 완료 게시 보류 사이에 파일을 다시 써서 닿는다. 처음 이 네 분기는 "결정적으로 만들 수 없다"고 적었는데 틀렸다(완료 보류가 이미 있었고 주입 지점은 기반을 보강해 더했다).
- 할당 실패: 로더 allocator와 에셋 Registry allocator를 같은 NFailAllocator로 묶어 모든 지점을 돈다(초기화·요청·합류·독립 요청·Tarjan 작업 공간(깊이 16 사슬)·허가·긴 쿠킹 루트·비ASCII 경로 색인 키·완료·Prepare·공개·Commit·상태 조회).
- 처음 구현에서 없앤 죽은 분기(입력으로 닿을 수 없음을 확인): 정리 목록에 이미 Failed·Abandoned인 단위가 다시 오는 검사, 관심 수 0에서의 감소 방어, 지울 작업의 IO 반납, 구성 항목의 단위 번호 대조, 허가 때 기대는 단위의 미허가 수·단계 방어, 임시 byte 상한의 중복 비교, 버린 작업 찾기 실패, Pending 요청의 단위 없음, Pump 반복 조건, WaitFor 안의 Pump 실패(검사 뒤 내부 Pump를 부른다), IO `Release`의 상태 없음. Loading 단위의 대기 대상 탐색은 "목록 끝까지 못 찾음"이 불변식상 없으므로(공개 안 된 의존이 있어야 Loading이다) 끝 검사 없는 탐색으로 썼다. 여러 할당 단계의 처음 실패를 모으던 템플릿 도우미는 비템플릿 `KeepFirst`로 바꿨다.
- 리뷰가 찾은 것(판정 "미충족" 반영): 임시 byte 합의 감김(포화 덧셈으로), `FindPackage`의 스레드 확인 누락, 실패 단위 진단·최근 동기 대기 위치의 상태 관측 누락, 실패·취소 겹침 규칙 미기술, PostLoad 단계 누락(11.6절 16으로 구현), Initialize의 매니페스트 재검증 누락, 소유 스레드 밖 소멸, 시험 runner의 process probe 등록 칸(8 → 32 — 로더 probe를 더하자 넘쳐 다른 probe 시험이 실패했다). 재판정 반영: 공개가 예산을 다 쓰면 PostLoad를 한 번도 부르지 않던 굶주림, PostLoad handler가 단위 참조를 쥔 채 재진입할 수 있던 수명 결함(콜백 중 표시 — 11.6절 18).
- 시험이 찾은 것: 내용 손상 시험이 처음에 MissingFile로 나왔다 — 패키지 경로가 내용 주소(SHA-256 앞부분)라 매니페스트 SHA를 고치면 경로도 바뀐다(로더 동작이 맞고 시험이 새 이름으로 다시 등록하게 고쳤다).

## 15. PNG 디코더 범위

색 형식 0/2/3/4/6, 비트 깊이 1–16, Adam7, PLTE·tRNS, chunk CRC, zlib(Core `Compression`)·Adler-32, 한 변 16384·해제 크기 = IHDR 계산값. gAMA·cHRM·sRGB·iCCP는 변환하지 않고 sRGB로 보며 sRGB가 아닌 표시가 있으면 `NonSrgbSource` 경고. APNG는 기본 이미지. 시험은 PngSuite(자유 사용 라이선스, `TextureTools/Tests/Fixtures/PngSuite`).

## 16. 임포트·빌드 의존

### 16.1 재임포트

기존 옵션 유지·명시 값만 변경, 잘못된 옵션 `InvalidImportOption`, 임포터 버전이 기록보다 높으면 `ImporterOutdated` 표시(자동 실행 없음), 사라진 출력은 `OrphanedOutput` 표시·`asset-orphans-*`로 처리.

### 16.2 Build 의존 선언

`DependencyDeclarer`(AssetRuntime 인터페이스, 타입별 등록)가 에디터 객체에서 Build 의존(예: Texture2DArray의 원본 Texture2D)을 선언한다. AssetDatabase는 저장 때 이 인터페이스로 목록을 얻어 Dependencies에 기록하고(쿠커를 부르지 않는다 — 의존 방향 유지), AssetCooker는 같은 선언을 너겟 키·선행 조건에 쓴다.

## 17. 계획 문서 개정 사항

계획 문서에 함께 반영한다.

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

계약(API·스키마는 있고 기본값이 아니거나 호출하면 명시 오류)으로 이번에 두는 것은 3·11·12절 표에 있다: 공유·원격 에셋 너겟 backend, 블록 압축, 12.4절 계약 설정(가상 텍스처·스트리밍·보정·색 관리·압축 설정 포함), `ReadyMask::Render`, `TaskLane::Worker`·`Render`.
