# 콘텐츠 해시 BLAKE3 전환 설계

기준일: 2026-11-28, 개정 3(초안, 설계만 — 개정 3: UE 교환 fixture 재생성 단계 CH-B3c, rocks-10 담당).

개정 1은 같은 날 사용자 지시로 썼다: "BLAKE3 전환 스펙으로 잡아둬", "왜 Encoding이라는 폴더에 있는거지". 개정 2는 교차 리뷰(막는 문제 3, 중요 9, 경미 7)를 모두 반영했다.

범위는 셋이다.
- 엔진이 파일·네트워크에 기록하는 내부 콘텐츠 해시를 SHA-256에서 BLAKE3로 바꾼다.
- 외부 무결성(upstream이 공개한 SHA-256과의 대조), 암호 프로토콜, 빌드 지문에는 SHA-256을 남긴다.
- 해시 소스는 `Core/Encoding/`에서 `Core/Hash/`로 옮긴다(H0).

기존 너겟·쿠킹 결과·패키지(소스 패키지 포함)는 모두 무효가 되어도 된다. 사용자 결정(2026-11-28)이다: "새로 만드는거니까 그냥 지나가는 과정일뿐", "제대로 쾌적한 상태로 적용하는게 맞음". 그래서 두 해시를 함께 읽는 과도기를 두지 않고 한 번에 바꾼다(CH7).

## 1. 배경과 근거

**측정.**
- Core `Sha256`은 Debug(/Od) 스칼라 구현이 28 MB/s였다. 셰이더 워커는 시작할 때마다 도구 파일 약 33 MB를 확인하느라 워커당 약 1.2 s를 썼다.
- 하드웨어 경로(SHA-NI·ARMv8 SHA2, [CoreDataFoundations](CoreDataFoundations.ko.md) 6절 C7)를 넣자 같은 22 MB가 /Od 40 ms, /O2 9 ms(2.4 GB/s)가 됐다.
- 이 병목은 C7이 푼다. 이 전환(BLAKE3)이 푸는 것이 아니다.
- 다만 SHA-NI가 없는 CPU(Intel Skylake–Comet Lake 데스크톱, 2015–2020)에서는 워커 도구 확인이 계속 스칼라 SHA-256이다. CH11이 이 부분을 BLAKE3로 바꾼다.

**BLAKE3.**
- 암호학적 해시다. 공개 수치로 단일 스레드 SIMD 3–7 GB/s(AVX2·AVX-512)이고, SHA 명령 없이도 빠르다. 트리 구조라 큰 입력을 여러 스레드로 나눌 수 있다.
- SIMD 이점은 1 KiB chunk 여러 개를 동시에 처리할 때 나온다. 그래서 64 B–1 KiB 입력(너겟 키·scope)에서는 SHA-NI와 비슷하거나 느릴 수 있다. 작은 값을 바꾸는 이유는 속도가 아니라 통일성이다(B1에서 작은 입력도 잰다).

**UE 5.8 대조.**
- `FIoHash`(Core/Public/IO/IoHash.h:18,20)는 BLAKE3-256의 앞 20 byte이고, 원문은 "selected for its high performance and its ability to parallelize"다. DDC의 `FBuildKey.Hash`(DerivedDataBuildKey.h:17), `FCacheKey.Hash`(DerivedDataCacheKey.h:95), build action·definition 입력(DerivedDataBuildAction.h:41,54, DerivedDataBuildDefinition.h:43,59)이 이것이다.
- 셰이더 job 키는 32 byte 전체인 `FBlake3Hash`다(RenderCore/Public/ShaderCompilerCore.h:203).
- 셰이더 라이브러리 해시는 비암호 XXH64다(Core/Public/Hash/ShaderHash.h:9-17).
- UE Core는 BLAKE3에 직접 의존한다(Runtime/Core/Core.Build.cs:42).
- 차이점은 2절 결정의 "UE와 다른 점"에 적는다.

**XXH3 같은 비암호 해시는 버린다.**
- 콘텐츠 주소는 Rockpile·SharedCompileStore처럼 여러 사용자·기계가 공유한다. 일부러 충돌을 만들 수 있는 해시를 키로 쓰면 저장소 오염을 막지 못한다.
- UE는 셰이더 라이브러리 해시에 XXH64를 쓴다. 이 부분은 UE와 다르게 간다.

## 2. 결정

| 번호 | 결정 | 이유 · 버린 대안 · UE와 다른 점 |
|---|---|---|
| CH1 | 내부 콘텐츠 해시는 **BLAKE3-256**이다(기본 출력 32 byte, keyed·derive_key 모드 없음) | UE 셰이더 job 키(`FBlake3Hash`, 32 byte)와 같다. UE `FIoHash`는 앞 20 byte로 자르는데, 우리는 자르지 않는다. 지금 형식의 32 byte 칸(`Digest256` wire 포함)을 그대로 쓸 수 있고 충돌 여유가 크다 |
| CH2 | 값 타입은 목적 이름 **`ContentHash`**다(32 byte, `ToHex`·`ParseHex`·`==`·`SeededHash<ContentHash>`). Core `Core/Hash/ContentHash.h`에 둔다. 계산기는 **`ContentHasher`**다(증분 `Update`·`Finish`, `Hash(bytes)`, 파일 해시 `HashFileContent`) | 값 타입에 알고리즘 이름(`Blake3Digest`)을 넣지 않는다. 알고리즘을 바꿀 때마다 형식·리플렉션 이름이 흔들리기 때문이다. UE `FIoHash`도 목적 이름이다 |
| CH3 | 계산기는 새 Runtime 모듈 **`ContentHashing`**(Core/Source/Runtime/ContentHashing)이 제공한다. 이 모듈은 제3자 모듈 **`Blake3`**(Core/Source/ThirdParty/Blake3, 공식 C 구현)를 감싼다. 값 타입만 Core에 있다 | Core는 외부 의존 없이 유지한다(Crypto D7). 리플렉션(ObjectRuntime)은 값 타입만 알면 된다. UE와 다른 점: UE Core는 BLAKE3에 직접 의존한다. 버린 대안: BLAKE3 자체 구현(SIMD 커널 넷과 트리라 [어려우면 외부 lib] 기준에 해당), Core 의존 예외 |
| CH4 | 제3자 모듈은 항상 최적화로 빌드된다(`ThirdPartyModuleRules` `CodeOptimization.Always`). SIMD 커널(SSE2·SSE4.1·AVX2·AVX-512, NEON)은 원본의 **intrinsics 판**을 쓴다. ISA별 target은 **libsodium 선례**대로 건다: `Rocks.patch`와 manifest `modifications`로 upstream 커널 파일(`blake3_sse2.c`·`_sse41.c`·`_avx2.c`·`_avx512.c`) 머리에 `#pragma clang attribute push(__attribute__((target("..."))), apply_to = function)`과 pop을 넣고, manifest `pragmaDirectives`에 기록한다 | upstream 디렉터리는 `NativeSources` 검사에서 빠지고 pragma는 manifest가 기록하므로, 정책 개정 없이 기존 기반으로 된다(Sodium/ThirdParty.json). 버린 대안 둘: 글루 TU에 pragma(글루는 라이브러리 폴더 바로 아래에만 둘 수 있고 외부 원본 검사에서 빠지지 않아 `target-attribute`에 걸림, PolicyChecks.ThirdParty.cs:225-228·PolicyChecks.cs:521-547), 파일별 `-m*` 인자(InputValidation.cs:765-766 결정 7·manifest `compileArguments` 허용 목록 ThirdPartyManifest.cs:220이 막음). 어셈블리 판은 MASM·GAS로 갈라져 버린다 |
| CH5 | **SHA-256을 남기는 곳**: (1) 제3자 manifest의 upstream 대조(`files`·archive·`licenseSource`의 `sha256`)와 bootstrap 검사. (2) Crypto(HMAC·HKDF 등 프로토콜 알고리즘)와 ContentSigning `MakeContentScope`(ContentSignature.h:82, 프로토콜 값). (3) C# 도구끼리의 무결성(SDK manifest 파일 해시 SdkAssembly.cs:152·InstalledSdk.cs:643, 메타 캐시, BuildReuse 저장소 키). (4) 빌드 지문(CH9). (5) Metal system 도구의 정체 해시(MetalToolchain.h:44 — 셰이더 도구 지문의 일부이며, upstream 대조가 아니다) | (1)은 upstream이 공개한 값과 대조한다. (2)는 표준·프로토콜이다. (3)은 엔진 형식에 들어가지 않고, .NET `SHA256`이 OS 구현이라 하드웨어 가속이 된다. (5)는 셰이더 도구 표 지문과 같은 범주로 두되, 엔진 콘텐츠가 아니라 도구 정체다 |
| CH6 | **BLAKE3로 바꾸는 곳**은 엔진이 기록하는 콘텐츠 주소와 동일성 판정 전부다(3절 표). 표에 없는 새 콘텐츠 해시도 `ContentHash`를 쓴다 | 같은 목적에 두 해시를 섞지 않는다 |
| CH7 | 하위 호환은 두지 않는다. 바뀌는 형식은 모두 버전을 올리고, 옛 너겟·쿠킹 결과·패키지는 `UnsupportedVersion`이다. **소스 패키지도 무효**다(`BulkDataRef` 해시가 소스 패키지에 기록되고 읽을 때 대조한다 — BulkData.h:13-30, MeshSourceStorage.cpp:32). 원본 파일에서 다시 import한다. TestContent `.rasset` 20개도 다시 import한다 | [하위호환 없음] 규칙이다. 옛 패키지를 읽어 새 해시로 다시 쓰는 변환 도구는 만들지 않는다. CH-B0에서 에셋 세션이 "원본 파일 없이 Bulk에만 있는 데이터"가 있는지 확인한다. 있으면 그 원본을 먼저 저장소에 넣는다. 확인 결과(에셋 세션, 2026-11-28): 그런 데이터는 없다. TestContent `.rasset` 20개는 모두 `Programs/Sandbox/TestContent/Sources/`의 추적 원본(png 26개, Radiance.hdr)을 가리키고, 그 밖에 추적하는 패키지는 없다 |
| CH8 | Rockpile C# 서버와 Rocks.Build(프로토콜 구현을 같이 씀, Rockpile 407행)는 BLAKE3를 NuGet 패키지로 계산한다(후보 `Blake3`, xoofx — Rust `blake3` crate 바인딩, 네이티브 의존). 토큰·설치 경로 SHA-256(Rockpile 478·543행)은 CH5 (3)으로 남긴다 | C#은 패키지를 허용한다([C++ 편의 lib 금지, C# 허용]). .NET 10에는 BLAKE3가 없다. Rockpile 891행의 "공유 프레임워크만으로 SHA-256 해결" 결정은 바뀐다(네이티브 NuGet 의존이 생긴다). Rockpile 코드는 아직 없으므로, 지금 할 일은 Rockpile 문서 개정이다 |
| CH9 | **빌드 지문**은 별도 타입 **`BuildFingerprint`**(32 byte, Core `Core/Build/`)로 나누고 SHA-256으로 남긴다. 대상은 C# `ToolchainFingerprint`·`ToolBuildId`이고, Rocks.Build가 C++ 코드로 생성한다(BuildInfo.cs:360,376,688-697, Core/Build/BuildInfo.h:29,36). 너겟 키·AssetIndexCache·셰이더 워커 프로토콜·Trace build id·CookProject는 이 값을 불투명 식별자로 기록한다 | C#이 계산해 C++ 상수로 넣는 값이고, 엔진이 다시 계산하지 않는다. 타입을 나누면 전환 뒤 `Sha256Digest` 사용처를 CH5 목록으로 검사할 수 있다(CH-B5) |
| CH10 | BLAKE3 커널 선택은 **엔진 CPU 판별과 자가 시험**을 따른다. `blake3_dispatch.c`의 자체 CPUID(`get_cpu_features`, `static g_cpu_features`)를 `Rocks.patch`로 Rocks 공급 함수 `rocks_blake3_cpu_features()`로 바꾼다. `ContentHashing`이 첫 사용 때 다음을 한다: `platform::QueryCpuFeatures()`를 BLAKE3 기능 bit로 옮긴다 → 공개 커널 `blake3_hash_many_*`마다 알려진 답 자가 시험 → 실패한 커널의 bit를 끄고 stderr 진단 한 줄을 남긴다. 고른 최고 커널은 `ContentHashing::ActiveKernelName()`으로 보고한다. 시험은 기능 mask를 강제하는 시험 hook으로 커널마다 돈다. manifest `globalState`에 그 static을 기록한다 | 엔진 규칙(C7과 같은 감지·자가 시험·진단, [조용한 대체 금지])과 맞춘다. 버린 대안: upstream 감지를 그대로 쓰기(고른 커널을 보고·강제할 수 없고 `BLAKE3_TESTING` 밖에서는 static) |
| CH11 | 셰이더 도구 적재 검사도 BLAKE3로 바꾼다. 제3자 manifest의 tool 파일에 **`blake3`** 칸을 더한다. refresh 도구가 upstream SHA-256을 확인한 다음 그 파일의 BLAKE3를 기록하고, 이 값은 저장소에 커밋되는 신뢰 기준이다. bootstrap은 SHA-256으로, ShaderToolLoader는 `blake3`로 확인한다 | SHA-NI가 없는 CPU에서도 워커 시작 확인이 빨라진다(I3). 신뢰 기준은 계속 저장소 manifest다. 버린 대안: 크기·시각 도장으로 검사 생략(파일을 바꾼 뒤 mtime이 그대로이면 놓친다) |
| CH12 | 큰 입력의 병렬 해시(Task로 subtree를 나눠 계산)는 후속(CH-L1)이다. 공식 C API는 subtree 계산을 공개하지 않는다(TBB 판 `blake3_hasher_update_tbb`만 있다) | 첫 단계는 단일 스레드 SIMD만으로도 SHA-256 HW 이상이다. 병렬은 측정 뒤 정한다 |
| CH13 | 리플렉션은 `ValueKind::Digest256` wire를 그대로 두고 C++ 타입만 `Sha256Digest`에서 `ContentHash`로 바꾼다. 전환 뒤 `Sha256Digest`는 **반사할 수 없다**(GeneratedFieldAccess.h:261·GeneratedKeyedPathAccess.h:31·ArrayFieldCodec.h:290·RuntimeTypeOps.h:99·FieldValueParse.h:29·PathKey.h:39의 타입 분기, MetaCompiler SchemaValidator.cs:25 형식 표와 SchemaModel.cs:341,380,468 안내문, 시험 MetaTests.RejectedCells.cs:58). `sha256` 같은 필드 이름(`BulkDataRef::sha256` 등)은 `hash`·`contentHash`로 바꾸고 `.rocksids.json`에 이름 변경을 등록한다(RMC2100) | wire byte가 같으므로 직렬화 계층은 그대로다. 버린 대안: 두 타입 모두 반사(같은 wire에 두 C++ 타입이 생겨 경로 키·맵 키 분기가 두 배가 된다) |
| H0 | 해시 소스를 **`Core/Hash/`**로 옮긴다. 공개 헤더: `Sha256.h`·`Sha256File.h`·`Crc32.h`, 그리고 `Core/Hash.h`를 `Core/Hash/TypeHash.h`로(UE `Templates/TypeHash.h` 대응, 40곳). 비공개: `Sha256*.cpp`·`Sha256Kernels.h`, 시험. C# 경로 상수(PolicyChecks.TargetIntrinsics SHA 계열 디렉터리, PolicyChecks.ApiMacros.cs:28, SelfTests.Policy.cs:1318, MetaTests.RejectedCells.cs:87, ModuleMetaTests.Modules.cs:393)도 같이 바꾼다. `Base64`·`ByteReader`·`ByteWriter`는 `Encoding/`에 남는다 | 해시는 부호화가 아니다(UE `Core/Public/Hash/`). `Core/Hash/TypeHash.h` 파일과 `Core/Hash/` 폴더가 나란히 있으면 헷갈리므로 같이 옮긴다. 내용 변경 없는 이동 커밋이다(include 약 140곳) |

## 3. 바뀌는 곳(조사 2026-11-28, 시험 제외 엔진 코드의 `Sha256` 참조 수)

| 영역 | 모듈(참조 수) | 내용 |
|---|---|---|
| 에셋 | AssetRuntime(111), AssetNugget(109), AssetCooker(81), AssetDatabase(41), AssetCommandlets, AssetTestFixtures, MeshSource | 소스·쿠킹 패키지 해시와 `BulkDataRef`, 쿠킹 매니페스트(`ArtifactSha256` 등), sidecar, 너겟 키·값(`NuggetKey` `WriteDigest`는 모든 쿠커가 같이 쓴다), cookKey, 원본 해시, AssetIndex(`AssetIndex::ContentHash()` 이름 정리) |
| 셰이더 | ShaderCore(57), ShaderCompiler(56), ShaderCompilerCommon(30), ShaderWorker, ShaderToolLoader(CH11), RenderBase(5) | job 키, 셰이더 집합 id(묶음 id = 앞 8 byte 규칙은 그대로), 라이브러리 형식, 형식 정의 digest, `TypeSetSha256`, 도구 적재 검사 |
| 머티리얼 | MaterialCompiler(13), MaterialCook | 번역 너겟 키, layout 서명(MSR14), 셰이더 집합 id 입력 |
| 리플렉션 | ObjectRuntime(32), MetaSchema, ObjectSerialization, Rocks.MetaCompiler | CH13 |
| 서명 | ContentSigning, ContentSignCommandlet(Programs/Commandlet) | 서명 체인의 매니페스트 패키지 해시는 BLAKE3로 바꾸고, `MakeContentScope`는 남긴다(CH5). ContentSigning 문서 버전을 올린다 |
| UE 교환 | UnrealExchange(36), UE 플러그인 `Extras/UnrealExport/RocksExport` | 플러그인의 자체 `FRocksSha256`(RocksBundleWriter.cpp:230, RocksMeshDescriptionWriter.cpp:1132)을 UE `FBlake3`(Hash/Blake3.h:20, 32 byte)로 바꾸고, UnrealExchange 검증(ExchangeBlob.cpp:268, ExchangeBundle.cpp:564-596)과 교환 번들 형식 버전을 같이 바꾼다 |
| 도구 | MeshTools(16), TextureTools, AudioTools, CurveAtlasTools | 원본·중간 결과 해시 |
| 그 밖의 런타임 | Compression(사전 digest), Audio(플랫폼 데이터), NodeGraph(의미 해시·시드 id), SparksCompiler(IR dump) | 동일성 판정·식별 |
| 빌드 지문 | Core/Build/BuildInfo.h, Launch, TraceCapture, Rocks.Build BuildInfo.cs | CH9 `BuildFingerprint`(SHA-256 유지, 타입만 분리) |
| Rockpile | 문서(`Rockpile.ko.md` 3절 51행, 5.1·5.2(91-94·121), 6.7(285·670·750), 891행 결정), Record 필드 `ContentHash` 이름 정리 | CH8 |

남는 `Sha256` 사용처는 CH5 목록으로 한정한다. CH-B5에서 정책 검사 `sha256-usage`(허용 디렉터리 목록)를 더해, 새 사용이 생기면 실패하게 한다.

## 4. 단계

병합 순서: H0은 단독 push다. 그 뒤 **두 단계 일괄**로 간다. (A) 타입 이름만 바꾸는 기계적 일괄 커밋(`ContentHash`·`ContentHasher`를 쓰되 내부는 아직 SHA-256, 출력 byte 동일). (B) 한 커밋에서 알고리즘을 BLAKE3로 바꾸고 형식 버전·골든을 일괄 갱신. 너겟 키 작성 API와 셰이더 집합 id 같은 경계 값을 여러 세션이 같이 쓰기 때문에, 영역별로 따로 병합하지 않는다. A·B는 렌더 세션이 하고, 각 담당 세션은 push 직후 바로 병합한다(바꾸는 동안 해당 파일 편집 중지 공지).

| 순서 | 내용 | 완료 기준 |
|---|---|---|
| CH-B0(완료 2026-11-28 — 각 문서 머리에 전환 예고, Crypto 101행·D7 수정, ShaderSystem 개정 68. 본문 치환은 일괄 B와 같이) | 문서 일괄 개정(스펙 먼저): Crypto 101행·D7(1099행), ContentSigning, AssetSystem(패키지·매니페스트·너겟 형식), ShaderSystem(job 키·라이브러리·도구 검사), Rockpile(CH8 목록), UnrealExchange(번들 형식), 리플렉션 문서(타입 표). 에셋 세션의 원본 확인(CH7) | 문서 개정 번호, 원본 누락 없음 확인 |
| H0 | 해시 폴더 이동(2절 H0) | 빌드·format·policy, Sha256 시험 |
| CH-B1 | 제3자 `Blake3`: 공식 C 구현을 릴리스 태그와 archive SHA-256으로 고정한다. 라이선스(CC0-1.0 / Apache-2.0 계열)와 `test_vectors/test_vectors.json`(`c/` 밖이라 `files`에 포함)을 manifest에 넣고, `Rocks.patch`(CH4 pragma, CH10 감지)와 `pragmaDirectives`·`globalState`를 기록한다. 대상 매트릭스: Windows X64·Arm64, Android Arm64·X64, Mac Arm64(Mac은 미빌드라 확인은 그 플랫폼 빌드 때) | 공식 벡터 전체(길이 0–102,400, chunk 경계), 커널별 대조(시험 hook mask). 이 CPU에 없는 커널(예: AVX-512)은 건너뛰었다고 진단을 남긴다. 처리량을 작은 입력(64 B·1 KiB)과 큰 입력(1 MiB·22 MB)에서 Debug·Release로 재고, SHA-256 HW와 비교해 기록한다 |
| CH-B2 | Core `ContentHash`·`SeededHash`, `BuildFingerprint`, 모듈 `ContentHashing`(`ContentHasher`·`HashFileContent`·`ActiveKernelName`), 리플렉션 타입 교체(CH13) | API 시험, 리플렉션 왕복, MetaCompiler 형식 표·시험 |
| CH-B3a | 일괄 A: 3절 모든 영역의 타입·이름 교체(출력 byte 동일), UE 플러그인 포함 | 전체 빌드·영역 시험(골든 변화 없음) |
| CH-B3b | 일괄 B: `ContentHasher`를 BLAKE3로 바꾸고, 형식 버전·골든·manifest `blake3`(CH11) 갱신 | 영역 시험, 형식 버전 시험(옛 버전 `UnsupportedVersion`) |
| CH-B3c | UE 교환(rocks-10 담당, 일괄 B push 직후): 플러그인 `FRocksSha256` → UE `FBlake3` 전환, `Tests/Bundles`의 Golden·GoldenSubstrate·GoldenBaked `.rxbundle`을 UE 5.8 플러그인으로 다시 내보냄(GoldenBaked는 GPU 굽기라 UE 작업 환경 필요), Basic(손으로 만든 fixture)은 해시만 다시 계산 | UnrealExchange 시험. 일괄 B와 이 커밋 사이에는 교환 fixture 시험이 실패한다(공지에 적는다) |
| CH-B5 | 소스 에셋 다시 import(TestContent 20개 포함), 엔진·TestContent 재쿠킹, Sandbox self-test, 정책 `sha256-usage` | 여덟 장치 self-test, 정책 통과 |
| CH-L1 | 큰 입력 병렬 해시(Task subtree) | 측정 뒤 |
| CH-L2 | Rockpile C#·Rocks.Build BLAKE3 구현(Rockpile R0 코드 단계에 포함) | C++ ↔ C# 골든 byte 일치(RT01) |

## 5. 리스크

- **일괄 B의 충돌.** 경계 값을 한 커밋에서 바꾸므로 그동안 다른 세션의 편집과 충돌한다. 공지 → 편집 중지 → push → 즉시 병합 순서를 지킨다.
- **CH4 pragma 방식.** libsodium 선례와 같지만, BLAKE3 커널 파일이 target 함수 안에서 다른 target 없는 inline 함수를 부르면 inline이 막혀 느려질 수 있다. B1 처리량 측정으로 확인한다.
- **소스 재import.** 원본 파일이 저장소 밖에 있는 에셋은 CH-B0 확인에서 드러난다. 없으면 CH-B3b 전에 원본을 넣는다.
- **이름 겹침.** Rockpile Record 필드 `ContentHash`와 `AssetIndex::ContentHash()`는 새 타입 이름과 같다. CH-B0·B3a에서 각각 `StoredHash`·`ContentDigest()` 같은 이름으로 정리한다(담당 문서에서 확정).
