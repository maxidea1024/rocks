## 1. 결정 요약

| 항목 | 결정 | 근거 |
|---|---|---|
| 이름 | `enum class PixelFormat`, 정보 `PixelFormatInfo`, 능력 `PixelFormatCaps`. 형식 이름은 채널 대문자 + 비트 + 성분 타입(`RGBA8Unorm`·`BC7UnormSrgb`·`ASTC6x6Hdr`) | 사용자 지시 "PixelFormat으로", 기존 `RGBA8Unorm` 이름 유지 |
| 지원 범위 | UE `EPixelFormat` 94개 값(0–93) 가운데 `PF_Unknown`을 뺀 93개를 전부 검토하였다. 73개는 Rocks 형식으로 대응하고, 6개는 형식이 아닌 다른 개념(스텐실 view plane, ASTC + swizzle)으로 옮기고, 14개는 사유를 적어 제외한다. Rocks 형식은 86개다(권장안 (가) 기준, sRGB 변형 16개 포함) | 3·5절 |
| 같은 저장 형식 | UE가 이름만 다르게 둔 같은 저장 형식(`PF_G8`·`PF_R8`, `PF_FloatRGB`·`PF_FloatR11G11B10`, `*_FILTER` 변형 등)은 Rocks 형식 하나로 합친다. 회색·알파 같은 채널 의미는 형식이 아니라 텍스처 swizzle(A0-G)로 표현한다 | 5.3절 |
| 정보 표 | GraphicsTypes에 장치 없는 정보 표를 둔다. 블록 크기·블록 바이트·채널·성분 타입·압축 계열·깊이/스텐실 비트·plane·sRGB 짝 | 6절. UE `PF.cpp:31–150` |
| 능력 | 장치마다 형식별 능력 플래그를 시작 때 한 번 채운다(GX1). D3D12·Vulkan은 API 질의, OpenGL ES는 사양 표 + 확장 조건 정적 표, Metal은 GPU family 정적 표 | 7·8절 |
| 대체 금지 | 장치가 형식을 지원하지 않을 때 다른 형식으로 몰래 바꾸지 않는다. 생성은 `UnsupportedCapability`로 실패한다. 대체 형식 선택은 쿠킹 프로필이나 호출자가 능력 질의 결과로 한다 | 8.3절. UE의 대체 사슬(`VK:768`·`VK:781`·`GL:1039–1044`)은 쿠킹 바이트 계약(RenderBase 6.5절)과 맞지 않는다 |
| sRGB 표현 | sRGB 변형을 별도 형식으로 둔다(결정 PX1 (가), 2026-11-17) | 9절 |
| 깊이·스텐실 | UE의 "플랫폼이 고르는 깊이 형식" 대신 명시 형식 5개(`D16Unorm`·`D24Unorm`·`D32Float`·`D24UnormS8Uint`·`D32FloatS8Uint`)를 둔다. 스텐실 읽기는 형식이 아니라 view의 plane 선택이다 | 10절 |
| 비디오 형식 | `NV12`·`P010`을 번호표와 정보 표에 넣고(plane 2개) D3D12 생성까지 지금 구현한다(결정 PX3, 2026-11-17). 다른 backend는 그 backend 대응 때 한다 | 11절 |
| 저장 값 | 번호를 이번에 다시 매긴다. 기존 값 0–2가 저장된 쿠킹 데이터·fixture는 없다 | 12절 |
| 문서 위치 | RenderBase(974줄)에 넣지 않고 별도 문서로 둔다. RenderBase GX1·GX2·6.5절과 AssetSystem A0-G가 이 문서를 가리킨다 | 2절 |
