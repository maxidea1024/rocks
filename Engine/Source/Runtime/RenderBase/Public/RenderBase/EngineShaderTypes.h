#pragma once

#include <Core/Containers/Span.h>
#include <ShaderCore/ShaderType.h>

// RenderBase가 등록하는 엔진 셰이더 타입이다(Docs/Design/RenderBase 11.4절 2항). UE도 화면 VS·
// 복사·clear 같은 유틸리티 셰이더를 RenderCore가 등록한다. 원문은 Engine/Shaders(/Engine)에 있다.
//
// 공개 헤더에는 이름·해시 상수만 둔다(desc는 정의 모듈의 cpp — ShaderSystem 12.2.1절 SSD2). 엔진
// 호스트는 RenderBase 모듈 수명 객체가 호스트 등록부(ShaderTypeRegistry 서비스)에 넣는다. 등록부를
// 직접 만드는 호스트(Sandbox·시험)는 EngineShaderTypes()를 자기 목록과 함께 등록한다.
//
// 사용 예(전체화면 복사 — RenderBase/FullscreenPass.h):
//   auto copy = set.FindEntry(render::CopyTexturePixelType.hash);
//   auto request = render::FullscreenPipelineRequest(set, copy.Value(), PixelFormat::RGBA8Unorm);
namespace rocks::render {

// 전체화면 삼각형 VS다(/Engine/Private/Utility/FullscreenTriangle.rsf). 출력은 SV_Position과
// TEXCOORD0(화면 UV — 왼쪽 위 (0,0))이다. 정점 버퍼 없이 Draw(3)으로 그린다.
ROCKS_SHADER_TYPE_NAME(FullscreenTriangleVertexType, "FullscreenTriangleVertex");

// 2D 텍스처 복사 PS다(/Engine/Private/Utility/CopyTexture.rsf). 텍스처 slot 이름은
// CopyTextureSourceName이고 FullscreenTriangleVertex와 짝이다.
ROCKS_SHADER_TYPE_NAME(CopyTexturePixelType, "CopyTexturePixel");

// CopyTexturePixel의 텍스처 이름이다(PipelineBindingNames::texture).
inline constexpr char CopyTextureSourceName[] = "Source";

// fallback 메시 VS다(/Engine/Private/Utility/BuiltinMesh.rsf, 정점 입력 축 — 정점 입력 타입은
// BuiltinVertexInput만). 그리기별 상수는 BuiltinMeshConstants(80 byte,
// RenderBase/BuiltinResources.h)다.
ROCKS_SHADER_TYPE_NAME(BuiltinMeshVertexType, "BuiltinMeshVertex");

// fallback 메시 PS다(Albedo 표본 × 정점 색 × 그리기 색). 텍스처 이름은 BuiltinMeshAlbedoName이다.
ROCKS_SHADER_TYPE_NAME(BuiltinMeshPixelType, "BuiltinMeshPixel");

// BuiltinMeshPixel의 텍스처 이름이다(PipelineBindingNames::texture).
inline constexpr char BuiltinMeshAlbedoName[] = "Albedo";

// 최소 tone map PS다(/Engine/Private/PostProcess/TonemapPass.rsf, Docs/Design/RenderBase 18.5절 3항).
// FullscreenTriangleVertex와 짝이다. 텍스처 이름은 TonemapSceneColorName, 그리기별 상수는
// TonemapConstants다. 출력은 선형 표시 색이고 sRGB 부호화는 대상 view가 한다. 렌더러 RS0에서
// RendererShaders 모듈로 옮긴다.
ROCKS_SHADER_TYPE_NAME(TonemapPixelType, "TonemapPixel");

// TonemapPixel의 텍스처 이름이다(PipelineBindingNames::texture).
inline constexpr char TonemapSceneColorName[] = "SceneColor";

// TonemapPixel의 그리기별 상수다(48 byte, TonemapPass.rsf TonemapConstants). 기본값은 UE 5.8
// FPostProcessSettings 기본값(Engine/Private/Scene.cpp — BlueCorrection 0.6, ExpandGamut 1,
// ToneCurveAmount 1, FilmSlope 0.88, FilmToe 0.55, FilmShoulder 0.26, FilmBlackClip 0, FilmWhiteClip
// 0.04)과 노출 배율 1이다.
//
// 사용 예:
//   render::TonemapConstants tonemap;
//   tonemap.exposure[0] = exposureScale;
//   commands.SetDrawConstants(Span<const uint8>(reinterpret_cast<const uint8*>(&tonemap),
//                                               sizeof(tonemap)));
struct alignas(16) TonemapConstants {
    static constexpr float DefaultExposureScale = 1.0f;
    static constexpr float DefaultBlueCorrection = 0.6f;
    static constexpr float DefaultExpandGamut = 1.0f;
    static constexpr float DefaultToneCurveAmount = 1.0f;
    static constexpr float DefaultFilmSlope = 0.88f;
    static constexpr float DefaultFilmToe = 0.55f;
    static constexpr float DefaultFilmShoulder = 0.26f;
    static constexpr float DefaultFilmBlackClip = 0.0f;
    static constexpr float DefaultFilmWhiteClip = 0.04f;

    // 노출 배율·blue correction·색역 확장·곡선 양이다.
    float exposure[4] = {DefaultExposureScale, DefaultBlueCorrection, DefaultExpandGamut,
                         DefaultToneCurveAmount};
    // 기울기·toe·shoulder·black clip이다.
    float film[4] = {DefaultFilmSlope, DefaultFilmToe, DefaultFilmShoulder, DefaultFilmBlackClip};
    // white clip(나머지는 0)이다.
    float filmWhite[4] = {DefaultFilmWhiteClip, 0.0f, 0.0f, 0.0f};
};

static_assert(sizeof(TonemapConstants) == 48, "TonemapConstants must match TonemapPass.rsf");

// 텍스처 표시 PS다(/Engine/Private/Debug/TextureInspector.rsf, Docs/Design/RenderBase 18.5절 6항).
// FullscreenTriangleVertex와 짝이고 차원마다 타입 하나다. 텍스처 이름은 아래 *SourceName, 그리기별
// 상수는 TextureInspectorConstants다. cube·cube 배열은 가로 십자(4 × 3 칸)로 펼친다.
ROCKS_SHADER_TYPE_NAME(TextureInspector2DPixelType, "TextureInspector2DPixel");
ROCKS_SHADER_TYPE_NAME(TextureInspector2DArrayPixelType, "TextureInspector2DArrayPixel");
ROCKS_SHADER_TYPE_NAME(TextureInspectorCubePixelType, "TextureInspectorCubePixel");
ROCKS_SHADER_TYPE_NAME(TextureInspectorCubeArrayPixelType, "TextureInspectorCubeArrayPixel");
ROCKS_SHADER_TYPE_NAME(TextureInspectorVolumePixelType, "TextureInspectorVolumePixel");

inline constexpr char TextureInspector2DSourceName[] = "Source2D";
inline constexpr char TextureInspector2DArraySourceName[] = "Source2DArray";
inline constexpr char TextureInspectorCubeSourceName[] = "SourceCube";
inline constexpr char TextureInspectorCubeArraySourceName[] = "SourceCubeArray";
inline constexpr char TextureInspectorVolumeSourceName[] = "SourceVolume";

// 텍스처 표시 방식이다(TextureInspectorConstants::selection[3]).
enum class TextureInspectorDisplay : uint8 {
    // 선형 값을 그대로 표시 색으로(LDR·sRGB 텍스처).
    Ldr,
    // 노출 × filmic 곡선(HDR 텍스처 — TonemapPixel과 같은 식).
    Hdr,
    // 빨강 채널을 회색으로(깊이).
    Depth
};

// TextureInspector*Pixel의 그리기별 상수다(64 byte). 앞 셋은 TonemapConstants와 같은 배치다.
//
// 사용 예:
//   render::TextureInspectorConstants inspector;
//   inspector.selection[0] = mip;
//   inspector.selection[1] = slice;                 // 배열 층·cube 배열 원소·volume z texel
//   inspector.selection[2] = depth;                 // volume 깊이(texel 수)
//   inspector.selection[3] = static_cast<float>(render::TextureInspectorDisplay::Hdr);
struct alignas(16) TextureInspectorConstants {
    float exposure[4] = {TonemapConstants::DefaultExposureScale,
                         TonemapConstants::DefaultBlueCorrection,
                         TonemapConstants::DefaultExpandGamut,
                         TonemapConstants::DefaultToneCurveAmount};
    float film[4] = {TonemapConstants::DefaultFilmSlope, TonemapConstants::DefaultFilmToe,
                     TonemapConstants::DefaultFilmShoulder, TonemapConstants::DefaultFilmBlackClip};
    float filmWhite[4] = {TonemapConstants::DefaultFilmWhiteClip, 0.0f, 0.0f, 0.0f};
    // mip·층·volume 깊이·표시 방식이다.
    float selection[4] = {0.0f, 0.0f, 1.0f, 0.0f};
};

static_assert(sizeof(TextureInspectorConstants) == 64,
              "TextureInspectorConstants must match TextureInspector.rsf");

// 엔진 기본 정점 입력 타입이다(Docs/Design/RenderBase 10.6절 3항,
// /Engine/Private/VertexInput/BuiltinVertexInput.rsh — 위치·UV·색, 배치는 BuiltinVertexLayout).
ROCKS_SHADER_TYPE_NAME(BuiltinVertexInputName, "BuiltinVertexInput");

// RenderBase가 등록하는 엔진 셰이더 타입 목록이다(정적 수명).
ROCKS_RENDERBASE_API Span<const shader::ShaderTypeDesc* const> EngineShaderTypes() noexcept;

// RenderBase가 등록하는 정점 입력 타입 목록이다(정적 수명, BuiltinVertexInput).
ROCKS_RENDERBASE_API Span<const shader::ShaderVertexInputType> EngineVertexInputTypes() noexcept;

} // namespace rocks::render
