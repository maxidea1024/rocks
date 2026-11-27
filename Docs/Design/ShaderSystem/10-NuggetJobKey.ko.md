# 셰이더 시스템 설계 — 10. P5 에셋 너겟과 셰이더 job 키

[목차](README.ko.md)

## 10. P5 에셋 너겟과 셰이더 job 키

AssetSystem A3(9.1절, 구현 `Engine/Source/Developer/AssetNugget`)을 그대로 쓰고, 셰이더 job도 AssetSystem 9절의 **공통 키 규칙을 그대로 따른다**(2026-11-13 사용자 결정 10). 개정 23(2026-11-23)에서 A3의 실제 API에 맞췄다. 의존: A2 `ProjectLayout`(구현), A3(5.2절 N1–N3, 반영됨), B7(구현).

**너겟 위치와 설정.** 메인 프로세스가 `ReadNuggetCacheSettings(allocator, files, root, &failure)`로 `<root>/Config/AssetNuggets.json`(없으면 기본값)을 읽고 `NuggetCache::Open(settings, root)`로 연다. root는 프로젝트가 있으면 프로젝트 루트(`ProjectLayout::Root`)다. 프로젝트 없이 엔진 셰이더만 컴파일할 때(Sandbox·self-test·엔진 내장 라이브러리)는 root를 엔진 폴더(HostConfig `FindEngineDirectory`, 저장소의 `Engine`)로 준다. 그러면 같은 규칙으로 `Engine/Config/AssetNuggets.json`(선택)을 읽고 기본 위치가 `Engine/Intermediate/AssetNuggets/`가 된다(저장소 `.gitignore`에 `/Engine/Intermediate/`를 더한다). 설정 파일로 `localPath`를 절대 경로로 주면 프로젝트 사이·엔진과 공유할 수 있다(내용 주소라 안전하다). UE 엔진 DDC(`Engine/DerivedDataCache`)의 자리다. 키 공통 머리의 프로젝트 세대는 `NuggetCache::ProjectGeneration()`이다(엔진만 쓰면 기본 null Guid).

**job 키.** 메인 프로세스가 계산한다(도구를 열지 않으므로 형식별 값은 형식 정의와 도구 표에서, Apple 도구 지문은 워커 도구 조회(11절 3항, 11.1절 결정 SC11)에서 얻는다). `NuggetKeyWriter`(`AssetNugget/NuggetKey.h`)에 아래 순서로 기록하고 `Finish()`의 SHA-256이 키다. 셰이더 job에는 AssetId·Build 의존 순환이 없으므로 SCC 식(`NuggetSccKey`)을 거치지 않고 **이 키를 Record 키로 그대로 쓴다**(AssetSystem 9.1절 결정 2 끝, 5.2절 N1). 기록 순서와 번호 표는 고정이며 바꾸면 셰이더 job 처리기 버전 Guid를 바꾼다. 키에 넣는 byte는 **워커에 보내는 바로 그 byte**다: 형식 정의·프로필은 세션을 시작할 때 한 번 읽어 둔 snapshot의 byte로 SHA-256을 계산하고 같은 byte를 워커에 보낸다(디스크를 다시 읽지 않는다), 레이아웃 표는 메시지에 싣는 정규 인코딩 byte의 SHA-256이다.

| 순서 | `NuggetKeyWriter` 기록 | AssetSystem 9절 항목 |
|---|---|---|
| 1 | `WriteNuggetKeyHeader(writer, ShaderJobProcessor, projectGeneration)`: `"RocksAssetNugget"`·키 형식 버전·엔진 세대 Guid·프로젝트 세대 Guid·처리기 Guid·처리기 버전 Guid·`deterministic::Version`·`build::ToolchainFingerprint()`. `ShaderJobProcessor`는 셰이더 job 처리기 하나다(형식마다 나누지 않는다. 형식 쪽 출력 의미 변화는 형식 정의 `processor.version`이 3행 SHA로 키에 들어간다). 메인 프로세스의 지문이 워커의 지문과 같다는 것은 워커가 입력 머리로 확인한다(11.1절 결정 SC5·SC6) | 형식 버전, 세대 Guid 둘, 처리기 Guid·버전 Guid, 결정적 수학 버전, 툴체인 지문 |
| 2 | `WriteList(n)` 뒤 도구마다 `WriteNuggetEncoder(name, version, settings)`. 순서는 형식 정의 `tools` 목록 순서(형식 정의 byte가 키에 들어가므로 순서도 고정이다). 저장소 도구: name·version은 도구 표 값, settings는 이 호스트 도구 표 파일 가운데 출력에 관여하는 role(`Runtime`·`Redist`·`Tool`)의 SHA-256(32 byte)을 표 순서로 이은 byte(`Symbols`·`Notice`는 넣지 않는다 — PDB·라이선스만 바뀌어도 모두 미적중이 되지 않게). `status: system` 도구(Apple `MetalToolchain`): version은 워커가 잰 `metal -v`의 버전 줄과 `Target:` 줄(경로가 든 `InstalledDir:` 등은 뺀다), settings는 도구 조회가 `xcrun --find`로 고정한 `metal`·`metallib` 실행 파일의 SHA-256을 이은 64 byte(11.1절 결정 SC11). SPIRV-Reflect(워커에 컴파일되는 C 원본)는 리플렉션을 쓰는 SH2부터 목록 끝에 name `SpirvReflect`·manifest 버전·manifest SHA-256으로 넣는다(SH1 출력은 리플렉션을 쓰지 않는다) | 외부 도구의 이름·버전·설정 |
| 3 | `WriteString(formatId)`, `WriteDigest(형식 정의 byte의 SHA-256)` | 외부 도구 설정(형식 정의가 셰이더 모델·target env·경로·배포 대상·상한·처리기 버전을 가진다) |
| 4 | `WriteU32(stage)` — 단계는 이미 있는 고정 값 `ShaderStageBits`의 한 비트(vertex 1·pixel 2·compute 4, 형식 정의 `stages` 어휘 `vertex`·`pixel`·`compute`와 1:1), `WriteString(entry)` | 에셋 TypeId 자리 |
| 5 | `WriteDigest(전처리 결과 SHA-256)` — 가상 경로만 담긴 `#line`을 가진 원문이고 정의(`#define`)·`Platform.rsh`는 이미 펼쳐져 있다. 워커는 이 원문 밖의 정의를 컴파일러에 넘기지 않는다 | 입력 bulk SHA-256 |
| 6 | `WriteReflected(writer, options)` — 컴파일 옵션 `ShaderCompileOptions`(`ROCKS_STRUCT`, 11.1절 결정 SC9)를 AssetSystem 9.1절 결정 2의 리플렉션 기록으로 쓴다(기본값 포함 전체, 필드 이름이 키와 trace에 들어간다). 그 뒤 파라미터 레이아웃 서명: P7 전에는 손으로 쓴 레이아웃 표(7.2절 5항)의 정규 인코딩 byte SHA-256을 `WriteDigest`, 레이아웃이 없는 job(SH1 시험·도구 원문)은 `WriteNone` | 결과에 영향을 주는 설정 전체 |
| 7 | (개정 32에서 지움 — 12.3.1절 SSD24) 프로필 id·byte SHA는 쓰지 않는다. 형식 정의 SHA-256이 3행에 있고 프로필의 형식 목록·순서는 출력 byte에 영향이 없다. SH2a가 코드에서 뺀다 | — |
| — | 없다. include 폐포는 전처리 결과에 들어 있다 | 의존 목록 |

**Record와 값(5.2절 N2).** bucket `shader`(`NuggetBucketShader`), namespace `rocks.nuggets`(고정), key = job 키. `inputs`는 비어 있다(다른 Record를 소비하지 않는다). `meta`는 비워 둔다. 값은 ValueId 오름차순의 부분이다:

| ValueId | 이름(`ShaderNuggetValue`) | 내용 | 단계 |
|---|---|---|---|
| 0 | `Code` | 형식 산출물: DXIL·DXBC·SPIR-V byte, ESSL·MSL UTF-8 텍스트, metallib byte | SH1 |
| 1 | `BindingLayout` | 정규화된 바인딩 layout(7.2절) | SH2 |
| 2 | `GlesCopyTable` | GLES 바인딩 머리·상수 버퍼 복사 표(7.2절 4항) | SH2 |
| 3 | `DebugSymbols` | 디버그 심볼(PDB·SPIR-V debug 등 — 배치 `ShaderDebugSymbols`, 16.3절 SSD73) | SH4 |

- 진단은 Record에 넣지 않는다. 진단 문자열은 작업 디렉터리·도구 버전 문구가 섞여 같은 키에서 digest를 흔들 수 있다(`Conflict`). 진단은 그 job을 컴파일한 실행의 결과로만 돌려준다: 실패 job의 진단과, 성공 job의 경고(`warningsAsErrors` false인 job·D3D11 대체 경로 성공 — 개정 64, 16.4절 SSD79·SSD80). 적중 job의 진단은 비어 있다(UE는 진단을 함께 캐시한다 — UE와 다른 점).
- 실패한 job(컴파일 오류·도구 오류)은 게시하지 않는다. 같은 입력을 다시 컴파일하면 같은 진단이 나오고, 실패를 캐시하면 도구·환경 문제의 회복이 늦어진다.
- 조회: 값 선택을 비워 Record 전체를 청한다(값을 골라 청하면 없는 값 때문에 `Miss`가 되고 다시 만든 Put이 `Exists`가 되는 고리가 생긴다 — A3 Get은 청한 값이 하나라도 없으면 `PartialRecord` 없이 `Miss`다). 정책은 `Default`(`PartialRecord` 없음). 적중했는데 값 0이 없으면 손상으로 보고 진단·통계를 남긴 뒤 다시 컴파일한다.
- 컴파일 묶음은 `NuggetGetRequest`로 모든 job을 한 번에 조회하고, 미적중만 워커에 보내고, 결과를 `NuggetPutRequest`로 게시한다(AssetSystem 9.1절 결정 12). 요청을 시작하고 기다리는 것은 관리자의 조정 스레드이며 그 scheduler의 worker가 아니다(같은 scheduler worker에서 진행 중 요청을 기다리거나 소멸하면 교착·Fatal — `NuggetRequest.h` 계약). 필요 시 컴파일(11.1절 결정 SC12) 한 건은 동기 `NuggetCache::Get`·`Put`이다.
- Put은 값 버퍼를 옮겨 간다(`NuggetPutRequest::Start`·`NuggetCache::Put`). 관리자는 호출자에게 돌려줄 값을 게시 전에 결과 쪽에 복사해 둔다.
- 결과 처리: `Hit` → 값 0을 쓴다. `Miss`·`Corrupt`·`DigestChanged`·`Dropped` → 다시 컴파일해 게시한다(DigestChanged 뒤 다시 만든 Record는 캐시가 Memory에 고정 보관한다 — 결정 11). `Cancelled`(캐시 Shutdown) → 그 job은 취소로 끝난다. Put 결과 `Conflict`는 결정성 위반이라 진단과 통계(결정성 gate)를 남기고 이번 결과를 쓴다. 캐시 실패(`failed`)는 설정 `required`면 job 오류, 아니면 진단만이다(R-1).

공통 규칙을 따른 결과:
- 툴체인 지문에 대상 triple(아키텍처·OS·배포 대상)이 들어가므로 Windows x64와 macOS arm64는 같은 셰이더도 키가 다르다. 셰이더 너겟은 호스트 사이에 공유하지 않는다(계획 §2.5 "host가 다르면 우선 분리"와 같은 방향). 키가 달라도 결정성 규칙(아래)은 그대로 적용한다.
- 한 호스트만 만들 수 있는 형식(Windows의 DXBC, 15절 결정 5의 metallib)은 다른 호스트에서 해당 형식 job이 `UnsupportedCapability`로 실패한다(계획 §27.9). 원격 워커(계획 §2.5 "macOS는 같은 action을 Windows worker에 전달", §27.7)가 생기면 그쪽으로 보낸다. 이때 키는 요청한 호스트의 것이다.
- 프로파일의 출력 설정(텍스처 LOD 그룹, 셰이더 형식 목록 등)이 바뀌면 그 프로파일의 셰이더 키가 모두 바뀐다. 반대로 셰이더 형식 목록을 바꾸면 같은 프로파일의 텍스처 키도 바뀐다. 이것은 공통 규칙의 비용으로 받아들인다.
- 엔진 세대 Guid(`EngineNuggetGeneration`)를 바꾸면 셰이더 키도 모두 바뀐다(오염 회수 — AssetSystem 9.1절 결정 4).

Apple 도구 지문: `metal -v --target=…`의 버전 줄과 `Target:` 줄(UE도 이 출력을 해석한다, `MetalShaderFormat.cpp:575–660`, 호출 `:882–920`)과 도구 조회가 `xcrun --find`로 고정한 `metal`·`metallib` 실행 파일의 SHA-256이다. 배포 대상(`-mmacosx-version-min` 등)은 SDK 기본값에 기대지 않고 형식 정의에 명시한다.

참고로 UE 5.8의 셰이더 job 키(BLAKE3)는 엔진을 빌드한 컴파일러·호스트를 넣지 않는다. 캐시 버전 GUID(`Runtime/RenderCore/Private/ShaderCompilerCore.cpp:5`), 형식 버전, 대상, 엔트리, 컴파일 환경, 전처리 결과, 루트 파라미터 레이아웃 서명, `PreferredShaderCodeFormat`·`bBindlessEnabled`·셰이더 파이프라인 여부·메타데이터 지시문 등을 넣는다(`ShaderCore.cpp:4612–4671`, `:4627`·`:4634`·`:4636`·`:4645–4652`). 형식 버전에 넣는 도구 버전 해시(DXC는 `dxcompiler.dll`·`dxil.dll` 이미지의 xxhash, ShaderConductor도 같은 방식)는 Windows가 아니면 상수라(`DXCWrapper.cpp:28–67, 109–116, 137–139`, `:64–66`), macOS에서 만드는 Metal·Vulkan·GL 형식은 도구가 바뀌어도 UE 키가 바뀌지 않는다(`Developer/Apple/MetalShaderFormat/Private/MetalShaderFormat.cpp:117,560`, `Developer/VulkanShaderFormat/Private/VulkanShaderFormat.cpp:65,117`, `ShaderFormatOpenGL.cpp:145`). Rocks는 모든 호스트에서 도구 SHA를 키에 넣고, 공통 규칙을 우선해 툴체인 지문도 넣는다.

**값과 결정성.** AssetSystem §10 결정성 규칙(같은 도구 빌드에서 호스트 무관하게 같은 바이트)을 셰이더에도 적용한다.

- **호스트 사이 동일.** 두 호스트가 모두 만드는 형식(DXIL·SPIR-V·ESSL·MSL 텍스트)은 같은 원본 태그·patch로 빌드한 도구에서 Windows와 macOS가 같은 바이트를 내야 한다. 도구 바이너리는 호스트마다 다르므로 SH0에서 잰다(측정 S3). 다르면 원인(도구 빌드 차이, 호스트 libm, 출력 형식화)을 고칠 방법을 정하기 전에는 그 형식을 공유 쿠킹에 쓰지 않고, 방법은 사용자 결정으로 올린다. 한 호스트만 만드는 형식(DXBC·metallib)은 해당 없음이다.
- 값은 바이트코드, 정규화된 바인딩 layout, GLES 바인딩 머리·복사 표, 진단, 디버그 심볼을 부분으로 나눈다(`.rnug` 부분 구조, 5.2절 N2).
- 경로 누출 금지: 입력은 메모리 버퍼와 가상 이름으로 넘기고 `#line`에는 가상 경로만 둔다. 디버그 정보는 별도 부분으로 빼고 Apple 도구에는 prefix map을 쓴다. "작업 디렉터리를 바꿔도 같은 바이트" 시험을 둔다.
- SPIRV-Cross의 실수 출력(`%.32g`)에서 locale 소수점은 SPIRV-Cross가 보정하므로(업스트림 `spirv_common.hpp`의 `fixup_radix_point`) CRT `%g` 구현 차이만 남는다. 같은 호스트 키 안에서도 OS의 CRT 버전은 다를 수 있다. 워커를 C locale로 고정하고, 극값(비정규수·최대값·17자리 이상 필요한 값) fixture로 출력이 같은지 시험한다. 다르면 추적 가능한 SPIRV-Cross patch(8절 4항)로 고정한다. MSL·ESSL·HLSL 5.0 모두 해당한다.
- **상수 접기.** DXC(LLVM)와 SPIRV-Tools 최적화기가 초월 함수 상수식을 호스트 libm으로 접으면, 같은 호스트 키 안에서도 CPU(예: UCRT의 FMA3 분기, [핵심 데이터 기반](../CoreDataFoundations.ko.md))에 따라 바이트가 다를 수 있다. SH0에서 상수 초월 함수 fixture를 FMA3 유무와 OS 버전을 바꿔 비교한다(측정 S3). 다르면 결정성 규칙을 지킬 방법(도구 빌드 patch, 해당 최적화 끄기 등)을 정하기 전에는 그 형식을 공유 캐시에 올리지 않고, 방법은 사용자 결정으로 올린다.
- **dxil 서명.** UE 5.8은 DXC 1.8.2403 기반 수정본을 쓰고 `dxil.dll`을 명시적으로 열며, 이 DLL이 없으면 서명되지 않은 DXIL이 조용히 나온다고 적는다(`DXCWrapper.cpp:100`, `:111–113`). DXC 1.8.2505부터는 외부 `dxil.dll`을 찾지 않고 내장 validator가 서명한다(업스트림 DXC `docs/ReleaseNotes.md`, 1.8.2502에서 validator hash 공개. UE 동봉본(1.8.2403)으로는 확인할 수 없어 측정 S1로 확인한다). Rocks는 DXC를 1.8.2505 이상으로 고정하고 `dxil.dll`을 쓰지 않는다. 결과 DXIL container 헤더의 hash digest가 0이거나 preview용 우회 값(`PreviewByPassHash`, 0x02×16, 업스트림 `include/dxc/DxilContainer/DxilContainer.h`)이면 job을 실패시킨다. macOS DXC에서도 서명되는지 SH0에서 실측하고(측정 S1), 서명되지 않으면 `D3D12_SM6`의 생산 호스트를 형식 정의에서 Windows로 고정한다.

완료 기준: A3 시험(적중·무효화·손상·동시 기록·GC — A3 구현에 있다)에 더해 다음을 시험한다(SH1b·SH1d).
- job 키의 입력별 무효화: 옵션, 형식 정의, 도구 SHA, Apple 도구 지문, 툴체인 지문, 결정적 수학 버전, 세대 Guid 둘, 처리기 버전, 단계·엔트리, 전처리 결과, 레이아웃 서명, 프로파일 출력 설정을 하나씩 바꾼다. 경로·시각·locale·작업 디렉터리·옵션 기록 순서만 바꾸면 같은 키다.
- 키 golden: `NuggetKeyTrace` 줄의 순서·태그·필드 이름이 고정 fixture와 같다. 호스트·빌드마다 다른 줄(공통 머리의 툴체인 지문·결정적 수학 버전, 도구 SHA)은 실행 중 값으로 기대값을 채운다(A3 `NuggetKeyTests`와 같은 방식). 리터럴 키 byte는 그 두 항목을 고정값으로 넣는 시험용 기록 경로에서만 비교한다.
- 빈 캐시와 찬 캐시의 결과 byte가 같고, system 도구가 없는 형식의 찬 캐시는 워커를 띄우지 않는다(system 도구 형식은 키 계산에 도구 조회가 필요하다 — 결정 SC11).
- 만들 수 없는 형식의 `UnsupportedCapability`(macOS의 DXBC).
- 실수 출력·상수 접기 fixture.
