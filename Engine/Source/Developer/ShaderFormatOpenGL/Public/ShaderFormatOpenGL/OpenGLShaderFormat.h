#pragma once

#include <ShaderCompilerCommon/ShaderFormatBackend.h>

// OpenGL ES 형식 처리기다(Docs/Design/ShaderSystem 2절, 11.1절 SC1·SH1e E5 — 워커가 링크한다).
// GLES31_ANDROID·GLES31_PC는 DXC → SPIR-V(vulkan1.1, 형식 dxPositionW) → spirv-val → SPIRV-Cross
// ESSL이다: #version = 형식 esslVersion(320) es, 기본 float 정밀도 highp,
// fixup_clipspace·flip_vert_y는 형식 값(Android 켬, PC 끔), 분리 texture·sampler는 결합 sampler로
// 바꾼다. 형식의 unsupportedImageFormats에 든 storage image 형식(SPIR-V OpTypeImage의 image
// format)을 쓰면 CompileError다. 결과 값 0(Code)은 ESSL 원문 UTF-8 byte(끝 NUL 없음)다. SH2c(16.1절
// SSD42)부터 SPIR-V 고쳐 쓰기·공용 규칙·GLES 배치(binding 재매김·combined sampler 이름·상수 버퍼
// 에뮬레이션 재작성)를 거치고 값 1(바인딩 layout)·값 2(GLES 바인딩 머리)를 함께 낸다.
//
// 사용 예(워커):
//   OpenGLShaderFormatBackend gles;
//   ShaderBackendContext context;
//   context.tools = &tools;
//   gles.Compile(format, job, context, result);
namespace rocks::shader {

// 처리기 버전이다(형식 정의 processor.version).
inline constexpr uint32 OpenGLShaderProcessorVersion = 3;

class ROCKS_SHADERFORMATOPENGL_API OpenGLShaderFormatBackend final : public ShaderFormatBackend {
public:
    ShaderFormatModule Module() const noexcept override {
        return ShaderFormatModule::OpenGL;
    }

    uint32 ProcessorVersion() const noexcept override {
        return OpenGLShaderProcessorVersion;
    }

    void Compile(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderBackendContext& context,
                 ShaderWorkerJobResult& result) noexcept override;
};

// SPIR-V 모듈이 쓰는 storage image 형식 가운데 unsupported(ImageFormatName 색인 비트)에 든 첫
// 형식의 색인이다. 없거나 모듈이 단어 정렬이 아니면 IndexNone이다.
ROCKS_SHADERFORMATOPENGL_API usize FindUnsupportedImageFormat(Span<const uint8> spirv,
                                                              uint64 unsupported) noexcept;

} // namespace rocks::shader
