// 머티리얼 텍스처 표본 보조다(Docs/Design/EngineShaders 04.05절 A05, Docs/Design/RenderBase 6.5절
// "swizzle의 뜻과 적용"). 쿠킹 텍스처의 swizzle은 UE sampler type 계약 데이터이고 텍스처 view
// swizzle은 늘 항등이다(D3D11에 view swizzle이 없어 모든 backend 결과를 같게, 두 번 적용하지 않게).
// 그래서 단일 채널 복제와 노멀 채널 배치는 셰이더가 한다(UE MaterialTexture.ush의 sampler type별
// 복제와 같은 규칙).
//
// 지금 담는 것(RB 18.5절 7항 — SM7에서 먼저): sampler type 복제와 쿠킹 swizzle 적용. view mip bias
// 표본·법선 복원·sampling scale·미분 지정 표본은 SL2에서 더한다.

#pragma once

// 쿠킹 swizzle 한 칸의 출처 번호다(GraphicsTypes SwizzleSource·Texture TextureSwizzle과 같은 번호).
static const float SwizzleSourceRed = 0.0;
static const float SwizzleSourceGreen = 1.0;
static const float SwizzleSourceBlue = 2.0;
static const float SwizzleSourceAlpha = 3.0;
static const float SwizzleSourceZero = 4.0;
static const float SwizzleSourceOne = 5.0;

// 단일 채널 sampler type(Grayscale·LinearGrayscale·Alpha·DistanceFieldFont)의 복제다. 단일 채널
// 형식은 D3D11·Vulkan·GLES 모두 (r, 0, 0, 1)로 읽히므로 빨강을 네 칸에 둔다(.rrrr).
// 인자: 텍스처에서 읽은 값. 반환: (r, r, r, r).
float4 ReplicateRedChannel(float4 value) {
    return value.rrrr;
}

// 출처 번호 하나의 값이다(Red–Alpha는 value의 그 칸, Zero·One은 상수). 범위 밖 번호는 0이다.
float SelectSwizzleSource(float4 value, float source) {
    if (source == SwizzleSourceRed) {
        return value.r;
    }

    if (source == SwizzleSourceGreen) {
        return value.g;
    }

    if (source == SwizzleSourceBlue) {
        return value.b;
    }

    if (source == SwizzleSourceAlpha) {
        return value.a;
    }

    if (source == SwizzleSourceOne) {
        return 1.0;
    }

    return 0.0;
}

// 쿠킹 swizzle 네 칸(출력 R·G·B·A마다 출처 번호 — TexturePlatformData swizzleR–A)을 적용한다.
// sampler type 대신 메타를 그대로 읽는 도구(텍스처 표시)용이다. 머티리얼 생성 코드는 sampler type
// 함수(ReplicateRedChannel 등)를 쓴다. 항등(0, 1, 2, 3)이면 value 그대로다.
// 인자: value 텍스처에서 읽은 값, sources 출처 번호 넷. 반환: 적용한 값.
float4 ApplyCookedChannelSwizzle(float4 value, float4 sources) {
    return float4(SelectSwizzleSource(value, sources.x), SelectSwizzleSource(value, sources.y),
                  SelectSwizzleSource(value, sources.z), SelectSwizzleSource(value, sources.w));
}
