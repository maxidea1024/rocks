## 10. 정점 입력 타입

### 10.1 UE 5.8 방식

- 클래스마다 정적 타입 객체(이름·`.ush` 원문 경로·기능 플래그 22종·파라미터 생성·layout·정점 요소·컴파일 거르기·컴파일 환경 수정)를 등록한다(`RC/Public/VertexFactory.h:135–160, 561–578`). 생성자는 확장자 검사·이름 hash 등록을 하고, 셰이더 타입 초기화 뒤에 생기면 check 실패다(`RC/Private/VertexFactory.cpp:102–161`).
- 메시 셰이더는 셰이더 타입과 정점 입력 타입이 둘 다 허용해야 컴파일된다(`Engine/Private/Materials/MeshMaterialShader.cpp:178–213`). 셰이더 키에 정점 입력 타입이 별도 축이다(`RC/Public/Shader.h:644–671`).
- 공용 메시 셰이더는 고정 가상 경로 `/Engine/Generated/VertexFactory.ush`를 include하고, 컴파일 환경이 이를 그 타입의 `.ush`로 잇는다(`VertexFactory.h:457–476`). `.ush`는 고정 함수 인터페이스(입력 구조체·중간값·월드 위치·보간값)를 구현한다(`Engine/Shaders/Private/BasePassVertexShader.usf:47–86`). 원문 hash는 셰이더 맵 의존이다(`VertexFactory.h:442, 593–610`).
- stream 성분(버퍼·offset·stride ≤ 255·원소 타입·용도: instancing·override·manual fetch, `RC/Public/VertexStreamComponent.h:10–72`)을 모아 같은 버퍼·stride·용도를 합치고 정점 요소(stream·offset·타입·attribute 번호·stride·인스턴스 여부, `RHI/Public/RHI.h:171–182`)를 만든다(`VertexFactory.h:799–813`). attribute 번호는 셰이더 `ATTRIBUTEn`과 같다(`Engine/Shaders/Private/LocalVertexFactory.ush:52–106`, `Engine/Private/LocalVertexFactory.cpp:520–610`).
- 정점 배치는 기본·위치 전용·위치+법선 전용(깊이 pass) 셋이다(`VertexFactory.cpp:398–413`). 원소 타입 20종(`RHIDefinitions.h:477–500`의 `VET_Float1`–`VET_UInt`).
- manual vertex fetch는 ES3.1 초과 기능 수준에서만이다. GLES는 늘 고정 입력 경로다(`VertexFactory.cpp:180–184, 262–276`).
- PSO 미리 준비를 지원하는 타입은 인스턴스 없이 정적 정점 요소 목록을 낸다(`VertexFactory.h:331, 393, 686–688`).

### 10.2 Rocks 규칙

- **등록.** 이름, `.rsh` 가상 경로, 기능 표시, 거르기 함수, 정적 정점 배치(인스턴스 데이터와 무관), 소유 파라미터 구조체를 가진다. 정적 등록 표이고, 셰이더 타입 등록(셰이더 시스템 12.2절)과 같은 시점에 닫힌다. 늦게 로드되는 모듈의 등록은 거부한다.
- **세 가지 정점 입력.** (1) **정점 입력 없음**: 전체화면·compute·유틸리티 셰이더(속성 없는 파이프라인, 11.2절). (2) **엔진 기본 정점 입력 타입**: 내장 메시와 fallback 메시 셰이더(계획 9.6절)가 쓰는 고정 배치(위치·UV·색, 지금 Sandbox 샘플 렌더러의 "Rocks 정점 형식", `Programs/Sandbox/Private/SampleRenderer.h` `BuiltinShaderHlsl` 주석). (3) **메시 정점 입력 타입**: 메시 렌더링 단계에서 정의한다. 메시 에셋 쪽 stream 집합은 [메시·스켈레톤·애니메이션 에셋](../MeshAnimationAssets.ko.md) 5.3·6.3절이 정했다(10.4절 끝 "메시 에셋 요구").
- **셰이더 키 축.** 셰이더 시스템 키에 정점 입력 타입을 더한다(SX4, 결정 R9 권장안 (a) 키 필드). 거르기는 셰이더 타입 조건 AND 정점 입력 타입 조건이다. 완결성 검사(9.2절)는 타입 × 정점 입력 타입 × 퍼뮤테이션을 열거한다.
- **include 연결.** 메시 셰이더는 고정 가상 경로 `/Engine/Generated/VertexInput.rsh`를 include한다. 워커 입력을 만드는 메인 프로세스 전처리기가 이 경로를 그 정점 입력 타입의 `.rsh`로 잇는다(셰이더 시스템 결정 6). 그래서 셰이더 맵 키에 정점 입력 타입이 들어가야 한다(SX4). `.rsh`가 구현할 함수 인터페이스(입력 구조체, 월드 위치, 단계 사이 변수 채우기)는 첫 메시 셰이더 착수 전에 정한다.
- **정점 배치.** 속성마다 (location, 형식, stream 번호, offset), stream마다 (stride, 정점/인스턴스, 인스턴스 step 1). Metal 정점 buffer 상한(셰이더 시스템 7.2절 4항: 정점 stream 8개)과 맞춰 stream ≤ 8이다. 같은 버퍼·stride·용도의 stream 합치기는 UE와 같다.
- **검사.** 파이프라인 생성 때 정점 배치 ⊇ 셰이더 입력 마스크, 형식 기본 타입 일치(SX9). 위치 전용 같은 부분 배치는 깊이 pass 단계에서(후속).
- **형식.** `VertexFormat`은 Float2/3/4뿐이다(`DeviceTypes.h:27–31`). 메시 에셋 단계 전에 패킹 법선(8 bit·10:10:10:2), half UV, 8 bit 정규화 색, 정수형을 더한다(UE `PackedNormal.h`·`RHIDefinitions.h:484–498`). A0-G(텍스처·샘플러) 범위 밖이므로 메시 에셋 착수 전 별도 결정이다(15절).
- **manual vertex fetch**는 후속이다(GLES 불가, UE도 같다).

### 10.3 완료 기준(RB5)

등록 결정성(등록 순서와 무관한 키), 늦은 등록 거부, 키 축 포함 라이브러리 왕복, 정점 배치 ⊇ 입력 마스크·타입 불일치의 음성 대조, 다중 stream·인스턴스 stream 픽셀 대조(D3D12·Vulkan·GLES 미리보기).

### 10.4 정점 배치와 정점 원소 형식(GX3a)

정점 입력 타입(10.2절)의 바탕인 Graphics 쪽 정점 배치를 셰이더 시스템보다 먼저 완성한다(2026-11-16 사용자 지시 "하시오", "이번에는 제대로 해야함"). 셰이더와 독립이고, A0-G 착수 전에 Graphics 계약 변경과 함께 합친다. 정점 입력 타입 등록·셰이더 키 축(RB5)은 셰이더 시스템 P14·SH2 뒤다.

**정점 원소 형식.** UE 5.8 정점 원소 타입 20종 전부를 둔다.
- 형식은 메시 에셋이 직렬화하므로 GraphicsTypes에 둔다. 텍스처 형식과 같은 방식으로 고정 번호표와 안정 이름을 쓰고 enum 순서 값은 직렬화하지 않는다(6.5절과 같은 규칙).
- "셰이더 타입"은 VS 입력 변수의 기본 타입이다. 정규화·float 형식은 float 계열, 정규화 없는 정수 형식은 int·uint 계열이어야 한다.
- UE 대응 근거:
  - D3D12: `D3D12RHI/Private/D3D12VertexDeclaration.cpp:27–41`
  - Vulkan: `VulkanRHI/Private/VulkanRHIPrivate.h:405–450`
  - Metal: `Apple/MetalRHI/Private/MetalVertexDeclaration.cpp:26–59`
  - OpenGL: `OpenGLDrv/Private/OpenGLVertexDeclaration.cpp:49–77`
  - 목록: `RHI/Public/RHIDefinitions.h:477–500`

| 번호 | Rocks | UE 5.8 | 바이트 | 셰이더 타입 | D3D12 | Vulkan | Metal | OpenGL ES |
|---|---|---|---|---|---|---|---|---|
| 1 | `Float1` | `VET_Float1` | 4 | float | `R32_FLOAT` | `R32_SFLOAT` | `Float` | `GL_FLOAT`×1 |
| 2 | `Float2` | `VET_Float2` | 8 | float2 | `R32G32_FLOAT` | `R32G32_SFLOAT` | `Float2` | `GL_FLOAT`×2 |
| 3 | `Float3` | `VET_Float3` | 12 | float3 | `R32G32B32_FLOAT` | `R32G32B32_SFLOAT` | `Float3` | `GL_FLOAT`×3 |
| 4 | `Float4` | `VET_Float4` | 16 | float4 | `R32G32B32A32_FLOAT` | `R32G32B32A32_SFLOAT` | `Float4` | `GL_FLOAT`×4 |
| 5 | `Byte4Snorm` | `VET_PackedNormal` | 4 | float4 | `R8G8B8A8_SNORM` | `R8G8B8A8_SNORM` | `Char4Normalized` | `GL_BYTE`×4 정규화 |
| 6 | `UByte4` | `VET_UByte4` | 4 | uint4 | `R8G8B8A8_UINT` | `R8G8B8A8_UINT` | `UChar4` | `GL_UNSIGNED_BYTE`×4 정수 |
| 7 | `UByte4Unorm` | `VET_UByte4N` | 4 | float4 | `R8G8B8A8_UNORM` | `R8G8B8A8_UNORM` | `UChar4Normalized` | `GL_UNSIGNED_BYTE`×4 정규화 |
| 8 | `Bgra8Unorm` | `VET_Color` | 4 | float4 | `B8G8R8A8_UNORM` | `B8G8R8A8_UNORM` | `UChar4Normalized_BGRA` | 미지원(`UnsupportedCapability`) |
| 9 | `Short2` | `VET_Short2` | 4 | int2 | `R16G16_SINT` | `R16G16_SINT` | `Short2` | `GL_SHORT`×2 정수 |
| 10 | `Short4` | `VET_Short4` | 8 | int4 | `R16G16B16A16_SINT` | `R16G16B16A16_SINT` | `Short4` | `GL_SHORT`×4 정수 |
| 11 | `Short2Snorm` | `VET_Short2N` | 4 | float2 | `R16G16_SNORM` | `R16G16_SNORM` | `Short2Normalized` | `GL_SHORT`×2 정규화 |
| 12 | `Short4Snorm` | `VET_Short4N` | 8 | float4 | `R16G16B16A16_SNORM` | `R16G16B16A16_SNORM` | `Short4Normalized` | `GL_SHORT`×4 정규화 |
| 13 | `Half2` | `VET_Half2` | 4 | float2 | `R16G16_FLOAT` | `R16G16_SFLOAT` | `Half2` | `GL_HALF_FLOAT`×2 |
| 14 | `Half4` | `VET_Half4` | 8 | float4 | `R16G16B16A16_FLOAT` | `R16G16B16A16_SFLOAT` | `Half4` | `GL_HALF_FLOAT`×4 |
| 15 | `UShort2` | `VET_UShort2` | 4 | uint2 | `R16G16_UINT` | `R16G16_UINT` | `UShort2` | `GL_UNSIGNED_SHORT`×2 정수 |
| 16 | `UShort4` | `VET_UShort4` | 8 | uint4 | `R16G16B16A16_UINT` | `R16G16B16A16_UINT` | `UShort4` | `GL_UNSIGNED_SHORT`×4 정수 |
| 17 | `UShort2Unorm` | `VET_UShort2N` | 4 | float2 | `R16G16_UNORM` | `R16G16_UNORM` | `UShort2Normalized` | `GL_UNSIGNED_SHORT`×2 정규화 |
| 18 | `UShort4Unorm` | `VET_UShort4N` | 8 | float4 | `R16G16B16A16_UNORM` | `R16G16B16A16_UNORM` | `UShort4Normalized` | `GL_UNSIGNED_SHORT`×4 정규화 |
| 19 | `Rgb10A2Unorm` | `VET_URGB10A2N` | 4 | float4 | `R10G10B10A2_UNORM` | `A2B10G10R10_UNORM_PACK32` | `UInt1010102Normalized` | `GL_UNSIGNED_INT_2_10_10_10_REV`×4 정규화 |
| 20 | `UInt1` | `VET_UInt` | 4 | uint | `R32_UINT` | `R32_UINT` | `UInt` | `GL_UNSIGNED_INT`×1 정수 |

- OpenGL ES의 정규화 없는 정수 형식은 정수 경로(`glVertexAttribIFormat`, ES 3.1 vertex attrib binding. 옛 API로는 `glVertexAttribIPointer`)로 설정한다(float 경로로 설정하면 float로 변환되어 int 입력과 맞지 않는다).
- `Bgra8Unorm`의 OpenGL ES 처리:
  - ES에는 BGRA 정점 배열이 없다. UE는 이 경우 RGBA로 읽어 채널이 뒤바뀐다(`OpenGLVertexDeclaration.cpp:56–64`).
  - Rocks는 이 형식을 OpenGL backend(ES 3.2 공통 API, 셰이더 시스템 5.2절 G2)에서 미지원으로 공개한다. 이 형식이 든 파이프라인 생성은 `UnsupportedCapability`다(지금 Graphics 오류 코드로는 `Unsupported`, [OpenGL ES backend](../OpenGLESGraphics.ko.md) 5.1절).
  - 메시 쿠커는 GLES 형식 프로필에서 색을 `UByte4Unorm`으로 쓴다.
  - 지원 여부는 backend별 형식 지원 표(GX1, A0-G G1과 같은 방식)로 조회한다.
- 정규화 변환 규칙은 그래픽 API 표준을 따른다. snorm은 max(c / (2^(b−1) − 1), −1), unorm은 c / (2^b − 1)이다. 32-bit 정수 형식은 정규화하지 않는다.

**정점 배치.**
- **속성** = (location 0–15, 형식, stream 0–7, offset)이다.
  - location은 HLSL `ATTRIBUTEn`의 n이다. 지금 원문 셰이더의 `TEXCOORDn`도 이번에 `ATTRIBUTEn`으로 바꾼다(셰이더 시스템 7.2절 7항).
  - 속성 수 상한은 16이다. Vulkan `maxVertexInputAttributes`와 OpenGL ES `GL_MAX_VERTEX_ATTRIBS`의 최소 보장이고, UE는 17이다.
- **stream** = (stride, 정점·인스턴스 구분)이다.
  - stream 상한은 8이다(Metal 정점 buffer 상한, 셰이더 시스템 7.2절 4항).
  - 인스턴스 stream은 인스턴스마다 한 번 진행한다. 진행 간격 1만 지원한다. UE도 instancing 여부 하나만 둔다(`RHI/Public/RHI.h:171–182` `bUseInstanceIndex`).
- **한계**:
  - offset ≤ 2047, stride ≤ 2048, 둘 다 4 byte 배수다. Vulkan `maxVertexInputAttributeOffset`·`maxVertexInputBindingStride`, OpenGL ES `GL_MAX_VERTEX_ATTRIB_RELATIVE_OFFSET`·`GL_MAX_VERTEX_ATTRIB_STRIDE`의 최소 보장이고, Metal stride는 4 byte 배수다.
  - UE stream 성분은 offset·stride ≤ 255다(`RenderCore/Public/VertexStreamComponent.h:57–58`). Rocks는 장치 최소 보장 쪽을 쓴다.
  - 한계 값은 이름 있는 상수로 둔다.
- **배치 자체 검사**(파이프라인 생성 때, 위반은 `InvalidArgument`):
  - location이 겹치지 않는다.
  - offset + 형식 크기 ≤ stride.
  - 선언된 stream마다 속성이 하나 이상 있다.
  - 속성이 없는 배치(정점 입력 없음, 11.2절)도 유효하다.
- **셰이더 입력 대조**(파이프라인 생성 때):
  - 셰이더가 읽는 모든 location이 배치에 있어야 하고(배치 ⊇ 입력), 형식의 셰이더 타입 계열(float·int·uint)이 셰이더 입력 타입과 맞아야 한다. 위반은 `InvalidArgument`이고 진단 문자열에 location·형식·셰이더 타입을 적는다.
  - 셰이더가 읽지 않는 속성은 허용한다. 깊이 pass처럼 위치만 읽는 셰이더가 같은 배치를 쓰기 때문이다.
  - 입력은 쿠킹된 `ShaderBindingLayout`(값 1 v3)의 입력 마스크와 int·uint attribute 마스크에서 얻는다(셰이더 시스템 7.2절 3·7항, Graphics `MatchShaderVertexInputs`). 네 backend가 같은 함수를 쓴다. SH2e2(2026-11-24) 전에는 D3D12가 컴파일한 VS 입력 signature를, OpenGL이 링크 뒤 active attribute를 읽었다.

**버퍼와 그리기**(GX4 일부를 함께 당긴다).
- 버퍼 용도에 index를 더한다. index 형식은 UInt16·UInt32다.
- 기록 객체에 다음을 둔다.
  - `SetVertexBuffers(첫 stream, {버퍼, offset} 목록)`
  - `SetIndexBuffer(버퍼, 형식, offset)`
  - `Draw(정점 수, 인스턴스 수, 첫 정점)`
  - `DrawIndexed(index 수, 인스턴스 수, 첫 index)`
- base vertex·base instance는 0으로 고정한다. `SV_VertexID`·`SV_InstanceID`의 base 포함 여부가 backend마다 다르기 때문이다(셰이더 시스템 7.2절 7항).
- 정점 버퍼 없는 그리기(속성 없는 배치)를 허용한다. 지금은 정점 버퍼가 없으면 그리기를 무시한다(`Device.h:18–20`).
- 묶이지 않은 stream이나 범위를 넘는 index는 Development 진단이다. 범위 검사는 index 버퍼 내용을 CPU에서 아는 경우에만 한다.

**CPU 패킹 helper.**
- 메시 쿠커와 시험이 쓰는 변환을 Core 수학에 둔다(공용 기능 중복 금지).
  - float ↔ half(IEEE binary16, round-to-nearest-even, 비정규수·무한대·NaN 보존)
  - float → snorm8·unorm8·snorm16·unorm16(위 정규화 규칙의 역, 반올림, clamp)
  - 10:10:10:2 패킹
  - 법선·탄젠트를 `Byte4Snorm`에 담는 규칙(w에 탄젠트 부호. UE `RenderCore/Public/PackedNormal.h`와 같은 의미)
- 변환 결과는 호스트와 무관하게 비트가 같아야 한다.

**완료 기준.**
- 형식 20종 각각에 대해 다음을 확인한다.
  - CPU 시험: 크기·셰이더 타입 계열·번호표 왕복.
  - 픽셀 대조: 그 형식으로 색이나 위치를 넣은 그리기를 D3D12(WARP 포함)와 OpenGL에서 기대 픽셀과 대조한다. `Bgra8Unorm`의 OpenGL은 `UnsupportedCapability` 음성 대조다.
- 다중 stream(위치 stream 0, 색 stream 1), 인스턴스 stream(인스턴스별 offset·색), indexed draw(UInt16·UInt32), 정점 버퍼 없는 그리기를 픽셀 대조한다.
- 음성 대조:
  - location 중복, offset·stride 한계·정렬 위반, offset + 크기 > stride
  - 셰이더가 읽는 location이 배치에 없음
  - int 입력에 정규화 형식
- half 변환은 65536개 전체 왕복과 경계값(반올림 경계·비정규수·무한대·NaN)을 시험한다. 정규화 패킹은 경계값과 clamp를 시험한다.
- 검증 계층 오류 0.

**구현(2026-11-17).** `GraphicsTypes/VertexFormat.h`(형식 20종·고정 번호표·안정 이름·크기·성분·셰이더 타입 계열·정규화 여부, Vulkan·Metal 대응은 주석 표), `Graphics/DeviceTypes.h`의 `VertexAttribute`·`VertexStream`·`VertexLayout`·한계 상수·`ValidateVertexLayout`·`BufferUsage::Index`·`IndexFormat`·`VertexBufferBinding`, `CommandContext::SetVertexBuffers`·`SetIndexBuffer`·`Draw(정점 수, 인스턴스 수, 첫 정점)`·`DrawIndexed`, `Core/Math/VertexPacking.h`(snorm·unorm 8·16, 10:10:10:2, `PackNormal`·`PackTangent`, half는 기존 `Float16`). 셰이더 입력 대조는 D3D12가 `D3DReflect` 입력 signature(시맨틱 `ATTRIBUTEn`·성분 타입), OpenGL이 링크 뒤 active attribute로 한다. 원문 셰이더의 정점 입력 시맨틱은 `ATTRIBUTEn`으로 바꿨다. 구현 메모:
- "UnsupportedCapability"는 Core 오류 코드에 없어 `ErrorCode::Unsupported`로 공개한다.
- index 값 범위 검사는 하지 않는다. 장치가 index 버퍼 내용을 CPU에 남기지 않기 때문이다(메시 쿠커·RB5에서 내용을 아는 쪽이 검사한다). 묶이지 않은 stream은 그리기를 무시하고 `IgnoredDraws`를 올린다.
- 정규화 패킹 반올림은 가장 가까운 정수(가운데는 0에서 먼 쪽)이며 double로 곱해 호스트와 무관한 결과를 낸다.

**메시 에셋 요구(MeshAnimationAssets MR1·MR2·MR7, 2026-11-20 반영).** 메시 에셋 쿠킹 데이터가 내는 stream 집합이다. 메시 정점 입력 타입(10.2절 (3))은 이 집합을 받는다. 정점 입력 타입 이름·`.rsh` 인터페이스는 RB5 메시 단계에서 정한다. 아래 결정 R18–R20은 2026-11-20 권장안으로 확정했다(사용자 지시 "MA2 전 결정 사항들도 권장안대로").

| 입력 | stream | 형식 |
|---|---|---|
| 정적 메시 | 위치 | `Float3` |
| 〃 | tangent 기저(tangent, 법선 w = 쌍법선 부호) | `Byte4Snorm` × 2, 고정밀 설정이면 `Short4Snorm` × 2 |
| 〃 | UV(두 채널을 한 속성에 묶음, 1–4 속성) | `Half4`, 전체 정밀도 설정이면 `Float4`. 홀수 채널의 마지막은 `Half2`·`Float2` |
| 〃 | 색(선택 — 모두 흰색이면 없음) | 쿠킹 프로필별: D3D·Vulkan `Bgra8Unorm`, OpenGL ES `UByte4Unorm`(이 절 위 GL 규칙) |
| skinned 메시 | 위 전부 + 영향 번호(LOD 통합 bone map의 번호 — 결정 R18) | `UByte4`(통합 본 256개 이하) 또는 `UShort4`(초과), 영향 4개면 1속성·8개면 2속성 |
| 〃 | 영향 가중치 | `UByte4Unorm` 또는 `UShort4Unorm`(고정밀 가중치 설정), 번호와 같은 수 |
| 〃 | morph 변화량 | 정점 stream이 아니다. 희소 목록(렌더 정점 번호, 위치·법선 변화량 `Half4`)을 렌더러가 compute 또는 CPU로 적용한다(RS12) |

- **속성 수.** 정적 메시 최대 위치 1 + tangent 기저 2 + UV 4 + 색 1 = 8, skinned 최대 + 영향 4 = 12다. 속성 상한 16 안에 인스턴스 stream 자리가 남는다. UV를 쌍으로 묶은 이유가 이것이다(UE도 UV를 쌍으로 묶는다).
- **결정 R18 — skinned 본 행렬과 섹션(확정 2026-11-20).**
  - **섹션을 본 수로 나누지 않는다.** UE 5.8도 섹션 분할을 없앴다. 섹션당 최대 본 설정 `MaxSkinBones`는 "Sections are no longer split based on bone count"로 deprecated다(`Engine/Classes/Engine/RendererSettings.h:1520–1527`). 모든 플랫폼이 16비트 본 번호를 지원한다는 이유로 CPU skinning 강제도 없앴다(`Engine/Public/Rendering/SkeletalMeshRenderData.h:95–98`).
  - **LOD 통합 bone map.** LOD 하나의 모든 섹션이 bone map 하나를 같이 쓴다. bone map은 그 LOD가 쓰는 메시 본 번호의 정렬 목록이고, 정점의 영향 번호는 이 목록의 위치다. UE의 통합 bone map(`bUnifiedBoneMap`·`UnifiedBoneMap` — `Engine/Public/Rendering/SkeletalMeshLODRenderData.h:47, 173`)과 같다. 섹션별 bone map은 두지 않는다.
  - **번호 폭.** 통합 본이 256개 이하이면 `UByte4`, 넘으면 `UShort4`다. UE와 같은 조건이다(`Engine/Private/SkeletalMeshLODRenderData.cpp:496` `GetNumUnifiedBones() > 256`). 메시 본 상한은 `MeshMaxBones`(= 65535)다.
  - **본 행렬 전달.** 인스턴스 데이터와 같은 경로다(10.5절). `RGBA32Float` typed buffer이고 VS가 typed buffer SRV로 읽는다. 본 하나는 `float4` 3개다. 행 벡터 규약(`p·M`)의 4×3 affine 행렬을 열 3개로 담고, 셰이더는 `dot(float4(p, 1), c_i)`로 성분 i를 계산한다. 현재·이전 pose(velocity, Renderer RS12)는 같은 버퍼의 다른 구간이다. 그리기의 시작 위치는 루트 상수 `MeshDrawConstants`의 예약 padding 자리에 둔다.
  - **용량.** 원소 상한은 `texelBufferElements`다(10.5절 용량). 최소 보장 기기(65536)에서 view 하나에 본 행렬 약 21,845개가 들어간다. 프레임의 모든 skinned 인스턴스(현재 + 이전 pose)가 이를 넘으면 10.5절 넘침 규칙(결정 R16)처럼 조각 view로 나누고, 그래도 안 되면 `Unsupported` + `RenderNotices`다. 조용히 빼지 않는다.
- **결정 R19 — 고정밀 형식(확정 2026-11-20).** tangent·UV·가중치의 고정밀 여부와 영향 번호 8·16비트는 퍼뮤테이션 축이 아니라 stream 형식 차이로 처리한다. `Byte4Snorm`·`Short4Snorm`·`Half`·`Float`·`UByte4Unorm`·`UShort4Unorm`은 모두 float 계열이고 `UByte4`·`UShort4`는 uint 계열이라, VS 입력 변수의 기본 타입이 같아 같은 셰이더를 쓴다(10.4절 "셰이더 타입" 계열 규칙). 정점 배치(파이프라인 상태)만 다르다. UE도 정밀도별로 셰이더를 나누지 않고 정점 선언으로 처리한다.
- **결정 R20 — `IndexFormat` 이동(확정 2026-11-20, MR7).** `IndexFormat`은 지금 Graphics `DeviceTypes.h:324`에 0부터 번호가 매겨져 있다(`UInt16`, `UInt32`). 메시 에셋이 index 폭을 직렬화하므로 정점 원소 형식과 같이 GraphicsTypes(`GraphicsTypes/IndexFormat.h`)로 옮긴다.
  - 고정 번호표는 1부터다(`UInt16` = 1, `UInt32` = 2). 0은 값 없음이다(`VertexFormat`과 같은 규칙). 안정 이름은 `UInt16`·`UInt32`이고 `IndexFormatSize`도 함께 옮긴다.
  - Graphics 쪽 별칭·호환 계층은 두지 않는다. 쓰는 곳(현재 약 20곳)을 모두 바꾼다(하위호환 없음 원칙).
  - 이동은 메시 에셋 단계(MA2) 착수 전에 렌더 쪽이 한다. 메시 에셋은 이 번호를 `uint32`로 저장하고 로드 때 검증한다. 자체 열거 `MeshIndexWidth`는 두지 않는다(MeshAnimationAssets 결정 MA-D10 개정).

### 10.5 인스턴스 데이터 읽기(Renderer RQ8)

메시 정점 입력 타입(10.2절 (3))이 그리는 인스턴스의 변환을 얻는 규약이다(개정 15). 인스턴스 데이터 내용(현재·이전 local-to-world 상위·하위, bounds, 표시, custom data)은 렌더러 GPU Scene이 정하고(Renderer 5.1절, 14.7절 MRD7), 이 절은 셰이더가 그것을 찾는 길만 정한다. 구현은 Renderer RS0이다.

**UE 5.8.** 정점 셰이더는 그리기마다 상수인 instance id offset(인스턴스 step 0인 정점 stream)과 `SV_InstanceID`를 더한 자리에서 instance id 버퍼를 읽어 인스턴스 번호를 얻고, 그 번호로 GPU Scene 인스턴스 데이터를 읽는다(`Engine/Shaders/Private/SceneData.ush:1340–1390`, 목록 읽기 `:1379`). ES3.1은 primitive uniform buffer 경로를 따로 둔다(`:1293–1316`).

**Rocks 규칙.**
- **인스턴스 데이터는 typed buffer.** GPU Scene 인스턴스 데이터는 `RGBA32Float` typed buffer이고 VS가 typed buffer SRV로 읽는다(Renderer 결정 RN9). 인스턴스 데이터를 정점 stream(10.4절 인스턴스 stream)으로 넣지 않는다. GLES VS의 storage block 최소 보장이 0이라 구조화 버퍼를 쓸 수 없고, 모든 backend를 한 경로로 둔다. GLES에서 typed buffer SRV는 VS 텍스처 유닛을 차지한다(셰이더 시스템 7.2절 4항).
- **인스턴스 번호.** `instanceIndex = InstanceIndices[instanceListOffset + SV_InstanceID]`다. `InstanceIndices`는 `R32Uint` typed buffer(공유 자원 구조체 `SceneResources`, 9.5·9.6절)이고 `instanceListOffset`은 루트 상수다. 그리기는 늘 첫 인스턴스 0으로 하므로 `SV_InstanceID`는 그리기 안 번호 0…n−1이다. base instance가 시스템 값에 들어가는지가 backend마다 다른 문제를 피한다(셰이더 시스템 7.2절 7항). GLES 3.1 간접 draw의 base instance 0 제약에도 그대로 맞는다(Renderer 15.2절).
- **늘 목록을 거친다(결정 R13).** CPU 가시성만 쓰는 단계(Renderer RS0–RS3)에도 렌더러가 그리기 항목마다 인스턴스 번호 목록을 프레임 업로드로 쓰고, GPU culling(RS4)은 같은 목록을 GPU가 쓴다. 셰이더 경로가 하나라 RS4 전후로 pass 셰이더가 바뀌지 않는다. UE도 일반 경로는 늘 instance id 버퍼를 거친다.
- **용량(개정 16).** typed buffer view 하나의 원소 수는 `DeviceCaps.limits.texelBufferElements` 이하다(GX1: D3D 2^27, Vulkan `maxTexelBufferElements` 최소 보장 65536, GLES `GL_MAX_TEXTURE_BUFFER_SIZE` 최소 보장 65536). 인스턴스 데이터 용량 = `texelBufferElements` ÷ 인스턴스당 `float4` 수(약 10–12로 추정, RS0에서 확정)다. 최소 보장 장치에서는 약 5,000 인스턴스이고 D3D는 사실상 제한이 없다. 실제 기기 값은 Renderer 19.2절 측정으로 본다. UE도 typed buffer view의 원소 상한을 장치 값으로 두고(`RHI/Public/RHIGlobals.h:321`, Vulkan `VulkanRHI/Private/VulkanRHI.cpp:1005`, OpenGL ES `OpenGLDrv/Private/OpenGLDevice.cpp:921–924`) 넘는 view 생성을 검증 오류로 본다(`RHI/Private/RHIValidation.cpp:1061–1062`).
- **넘침(결정 R16).** typed buffer를 다른 자원으로 몰래 바꾸지 않는다.
  - 인스턴스 데이터: 용량을 넘는 인스턴스 추가는 `Unsupported`로 실패하고 `RenderNotices`로 게임 lane에 알리며 계수를 올린다. 이미 있는 인스턴스는 그대로 그린다. 조용히 빠뜨리지 않는다(계획 13.1절).
  - `InstanceIndices`: 프레임 목록이 원소 상한을 넘으면 목록을 상한 크기 조각으로 나누고, 그리기 항목은 자기 조각의 view를 그리기별 바인딩(9.3절)으로 묶는다. 셰이더 경로는 그대로다.
  - light grid: 목록 용량을 원소 상한 이하로 두고, 넘치면 Renderer 8.1절의 넘침 보수 대체(그 셀에서 전체 순회)를 쓴다.
  - 최소 보장 기기가 대상 Android 기기에 실제로 있으면 GPU Scene 저장 형태(Renderer 결정 RN9, 2D 텍스처 대체 등)를 다시 연다. 재검토 조건은 RN9의 측정 MR4에 더했다.
- **루트 상수 배치.** 메시 pass 셰이더의 루트 상수 구조체는 앞 16 byte에 `MeshDrawConstants`(첫 `uint`가 `instanceListOffset`, 나머지 12 byte는 예약 padding)를 펼쳐 넣는다. 정점 입력 `.rsh`가 이 이름으로 읽는다. 나머지 112 byte는 pass가 쓴다(루트 상수 128 byte, 셰이더 시스템 7.2절 2·5항). UE는 같은 값을 정점 stream으로 넘긴다. Rocks는 루트 상수가 모든 형식에 있으므로(GLES는 uniform 에뮬레이션) 정점 stream·attribute 자리를 쓰지 않는다(stream 8개·attribute 16개 상한, 10.2절·SX9).
- **`.rsh` 인터페이스.** 메시 정점 입력 `.rsh`의 함수 인터페이스(10.2절 include 연결)에 "인스턴스 번호 → 현재·이전 변환" 단계를 넣는다. 읽기 함수는 GPU Scene 배치를 아는 렌더러 공용 `.rsh`가 제공하고 정점 입력 `.rsh`는 그것을 부른다. world 위치는 `(인스턴스 상위 − view 상위) + 상대 변환`으로 카메라 상대 값을 만든다(Renderer 5.1절, `View` slot의 이동 보정). 행렬은 행 벡터 `p·M`이고 셰이더는 `mul(v, M)`이다(셰이더 시스템 7.2절 6항).
- **앞면(개정 16).** 컬 방향은 파이프라인 상태라 한 그리기 안에서 인스턴스마다 바꿀 수 없다. 그래서 local-to-world 행렬식 부호는 인스턴스마다 보고, 부호가 다른 인스턴스는 한 그리기에 묶지 않는다(렌더러 상태 묶음 키에 부호를 넣는다, 셰이더 시스템 7.2절 8항 음수 행렬식). 변환 갱신으로 부호가 바뀐 인스턴스는 그 그리기 항목을 다시 분류한다(재질 교체와 같은 캐시 무효화, Renderer 4.2절).
- 엔진 기본 정점 입력 타입과 정점 입력 없음(10.2절 (1)·(2))은 이 규약 밖이다. 내장 메시는 지금처럼 그리기별 상수의 변환을 쓴다.
- **완료 기준(Renderer RS0에서).** (`instanceListOffset`, `SV_InstanceID`) → 인스턴스 변환이 CPU 참조와 같은지 픽셀 대조(D3D12·Vulkan·GLES 미리보기, GLES는 VS texel buffer 경로), offset이 0이 아닌 그리기, 행렬식이 음수인 인스턴스의 별도 그리기와 컬링 픽셀.

### 10.6 정점 입력 타입 등록 구현 설계(개정 40, RB5)

18.3절 RR4–RR6이다. 결정은 권장안으로 확정했다.

1. **등록부 자리 — 셰이더 타입 등록부에 함께 둔다.** ShaderCore `ShaderTypeRegistry`(셰이더 시스템 12.2.1절 SSD3)가 정점 입력 타입도 받고 같은 `Freeze`로 닫는다. 셰이더 시스템 SSD5의 "정점 입력 타입 등록부는 RenderBase 10절 몫"을 고친다(셰이더 시스템 개정 48).
   - 근거: 쿠커(Developer ShaderCompiler)·Development 공급자·셰이더 집합이 장치 없이 같은 목록을 써야 하고, 10.2절대로 셰이더 타입 등록과 같은 시점에 닫혀야 한다. UE도 정점 입력 타입(vertex factory type)과 셰이더 타입을 같은 모듈의 같은 초기화 단계에서 다루고, 셰이더 타입 초기화 뒤에 생긴 타입은 check 실패다(`RC/Private/VertexFactory.cpp:102–161`).
   - desc `ShaderVertexInputType`(정적 수명): `name`·`id`(= `ShaderTypeNameHash(name)`), `sourcePath`(`.rsh` 가상 경로 — 2항의 연결 대상), `owner`(셰이더 타입과 같은 소속 규칙: 경로 루트와 맞아야 한다), `shouldCompile`(기존 서명).
   - API: `RegisterVertexInputTypes(Span<const ShaderVertexInputType* const>)`·`UnregisterVertexInputTypes`, `VertexInputTypes()`(Freeze 뒤 id 순 — 등록 순서와 무관), `FindVertexInputType(id)`. 검사(위반은 목록 전체 거부, `InvalidArgument`): 이름 문법·길이(셰이더 타입 이름과 같은 규칙), id = 이름 해시, 정점 입력 타입끼리 이름·해시 중복, 경로 문법·`.rsh` 확장자·소속 루트, `NoVertexInputType` 금지. Freeze 뒤 등록은 `ScopeClosed`(늦은 등록 거부). `ShaderTypeModuleRegistration`이 셰이더 타입과 정점 입력 타입 목록을 함께 받는다.
   - 쓰는 쪽(하위호환 없음): `ShaderSetDesc::vertexInputTypes`·`ShaderProvisionRequest::vertexInputTypes`·`ShaderLibraryCookDesc::vertexInputTypes`를 지우고 등록부 목록을 쓴다.
   - 타입 집합 digest(SSD3)는 셰이더 타입만이다. 정점 입력 타입이 실행 파일과 라이브러리 사이에 다르면 완결성 검사(`MissingEntry`)가 잡는다. 버린 대안: digest에 정점 입력 타입을 넣기 — 축 있는 셰이더가 쓰지 않는 정점 입력 타입을 더하기만 해도 맞는 라이브러리가 거부된다.
2. **include 연결(`/Engine/Generated/VertexInput.rsh`).** 정점 입력 축 job(셰이더 시스템 SSD6)은 그 정점 입력 타입의 `sourcePath`를 받는다(`ShaderCompileJob::vertexInputSource`). 전처리기는 생성 include 표(`ShaderGeneratedInclude{가상 경로, 실제 가상 경로}`)를 받아, 그 가상 경로를 include하면 실제 경로 파일을 읽는다. include 폐포(키 5행)에는 실제 경로가 들어가므로 정점 입력 타입마다 키가 다르다. 연결이 없는 job(축 없는 타입)이 이 경로를 include하면 지금처럼 `GeneratedSource` 오류다(소스 트리 규칙 — `Generated`는 실제 파일이 아니다). UE 대응: 공용 메시 셰이더가 고정 가상 경로를 include하고 컴파일 환경이 그 타입의 원문으로 잇는다(`RC/Public/VertexFactory.h:457–476`). SSD6의 "정점 입력 타입이 주는 정의"는 지금 없다(include 연결로 충분하다 — 필요하면 desc에 정의 목록을 더한다).
3. **엔진 기본 정점 입력 타입 `BuiltinVertexInput`.** 이름 `BuiltinVertexInput`, 원문 `/Engine/Private/VertexInput/BuiltinVertexInput.rsh`(엔진 셰이더 A05), 소속 Engine, RenderBase가 등록한다(11.4절 2항의 엔진 셰이더 타입과 함께).
   - 배치(RenderBase 상수 `BuiltinVertexLayout()`·`BuiltinVertex`): stream 0 하나, stride 36 byte — 위치 `Float3`(ATTRIBUTE0, 0)·UV `Float2`(ATTRIBUTE1, 12)·색 `Float4`(ATTRIBUTE2, 20). 지금 Sandbox 샘플 렌더러 정점 형식과 같다(10.2절 (2)).
   - `.rsh` 인터페이스(이 타입만 — 메시 정점 입력 인터페이스는 RR16): 입력 구조체 `VertexInputAttributes`, `GetVertexInputLocalPosition`·`GetVertexInputTexCoord`·`GetVertexInputColor`.
   - 정적 정점 배치를 등록 desc에 넣지 않는다(ShaderCore는 Graphics 타입을 모른다). 일반 정적 배치 표는 PSO 미리 준비와 함께 RR15다.
4. **완료 기준(10.3절 중 이 개정 몫).** EngineTests: 등록 결정성(두 순서로 등록해 같은 목록·같은 라이브러리 byte), 늦은 등록 `ScopeClosed`, 검사 음성 대조(이름·해시·경로·확장자·소속·중복), 생성 include 연결(같은 셰이더 타입·퍼뮤테이션의 두 정점 입력 타입이 다른 코드·다른 키, 연결 없는 include의 `GeneratedSource`), 등록부 목록으로 쿠킹·셰이더 집합 열기 왕복. Sandbox: 기본 정점 입력 타입으로 그린 내장 메시 픽셀(11.4절 7항).
