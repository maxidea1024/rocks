#pragma once

#include "SpikeCommon.h"

// 스파이크 입력 셰이더와 fixture다. 모두 정적 수명이다.
namespace rocks::spike {

// 지금의 내장 Renderer 셰이더(Engine/Source/Runtime/Renderer/Private/Renderer.cpp의 ShaderHlsl)를
// 그대로 옮긴 VS·PS다. 원본은 register를 적는다(7.2절 1항 이전 형태). SPIR-V에서 b0·t0·s0가 모두
// binding 0이 되는 것은 리플렉션 덤프가 보여 주는 규약 검토 자료다.
extern const ShaderInput RendererVertex;
extern const ShaderInput RendererPixel;

// ROCKS_IMAGE_FORMAT 확인(7.2절 1항): DXC [[vk::image_format("...")]] 지정이 SPIR-V OpTypeImage
// 형식으로 나오는지 본다. attribute가 null이면 지정 없음(원소 타입 추론)이다.
struct ImageFormatCase {
    // 산출물 이름 앞부분(예: "ImageFormat.rgba8").
    const char* name;
    // [[vk::image_format]] 인자. null이면 attribute를 쓰지 않는다.
    const char* attribute;
    // RWTexture2D 원소 타입(예: "float4").
    const char* element;
    // 기대하는 SpvImageFormat 값.
    uint32 expectedFormat;
};

extern const ImageFormatCase ImageFormatCases[];
extern const usize ImageFormatCaseCount;

// 시험 compute 셰이더 원문을 source에 만든다(앞 내용은 지운다).
bool BuildImageFormatSource(const ImageFormatCase& test, StringBuilderBase& source) noexcept;

// 상수 초월 함수 fixture(10절 상수 접기): 리터럴 인자의 sin·cos·tan·atan2·exp·log·pow·sqrt를 두
// render target으로 낸다. DXC(LLVM)·SPIRV-Tools가 호스트 libm으로 접으면 결과 byte가 호스트에 따라
// 달라질 수 있다.
extern const ShaderInput TranscendentalPixel;

// 실수 출력 fixture(10절 SPIRV-Cross 실수 출력): 극값 상수를 두 render target으로 낸다.
extern const ShaderInput FloatLiteralPixel;

// FloatLiteralPixel 상수의 기대 float bit 패턴과 HLSL 철자다(같은 순서).
struct FloatLiteralCase {
    const char* spelling;
    uint32 bits;
    const char* meaning;
};

extern const FloatLiteralCase FloatLiteralCases[];
extern const usize FloatLiteralCaseCount;

} // namespace rocks::spike
