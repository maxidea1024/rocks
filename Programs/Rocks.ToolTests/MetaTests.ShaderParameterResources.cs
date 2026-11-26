namespace Rocks.ToolTests;

// MetaCompiler 셰이더 파라미터 모드의 자원 멤버 시험이다(Docs/Design/ShaderSystem 16.2절 SSD66 2): 양성 생성물(자원 목록·HLSL 전역 선언·포함
// 구조체 펼치기)과 배치·서술자·이름 규칙의 음성 진단. 자원 틀(RenderBase·RenderGraph)은 같은 철자의 stub이다 — 생성기는 정규 철자로 틀을 안다.
internal static partial class MetaTests
{
    private const string ShaderResourcePreamble = "#pragma once\n#include <ShaderCore/ShaderParameterTypes.h>\n#include <ShaderCore/ShaderResourceTypes.h>\n" +
        "namespace rocks::render {\n" +
        "template <class D> struct alignas(16) DeviceTextureParameter { unsigned value[8]; };\n" +
        "template <class D> struct alignas(16) DeviceBufferParameter { unsigned value[2]; };\n" +
        "template <class D> struct alignas(16) GraphTextureParameter { unsigned value[8]; };\n" +
        "template <class D> struct alignas(16) GraphTextureSrvParameter { unsigned value[8]; };\n" +
        "template <class D> struct alignas(16) GraphTextureUavParameter { unsigned value[2]; };\n" +
        "template <class D> struct alignas(16) GraphBufferSrvParameter { unsigned value[2]; };\n" +
        "template <class D> struct alignas(16) GraphBufferUavParameter { unsigned value[2]; };\n" +
        "struct alignas(16) GraphTextureAccessParameter { unsigned value[8]; };\n" +
        "struct alignas(16) GraphBufferAccessParameter { unsigned value[4]; };\n" +
        "struct alignas(16) GraphRenderTargetParameter { unsigned value[32]; };\n" +
        "}\n" +
        "namespace probe {\nusing namespace rocks::shader;\nusing namespace rocks::render;\n";

    private static void RegisterShaderParameterResources(MetaContext context)
    {
        var (suite, options, actualEnvironment, generator, compiler) = context;
        suite.Add("Meta.ShaderParametersGenerateResourceMembers", async () =>
        {
            using ShaderParameterProbe probe = new(options, actualEnvironment, generator, compiler);
            int exit = await probe.RunAsync(
                "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) SceneTextures {\nGraphTextureParameter<ShaderTexture2D<float>> depth;\n" +
                "GraphTextureSrvParameter<ShaderTexture2D<>> color;\n};\n" +
                "ROCKS_SHADER_PARAMETERS(Name = \"Pass\")\nstruct alignas(16) PassParameters {\nShaderFloat4 tint;\nfloat scale;\n" +
                "DeviceTextureParameter<ShaderTexture2D<>> albedo;\nDeviceTextureParameter<ShaderTexture2DArray<ShaderUint4>> codes;\n" +
                "DeviceTextureParameter<ShaderTexture2D<float, ShaderComparisonSampler>> shadow;\nDeviceTextureParameter<ShaderTexture2DMS<>> coverage;\n" +
                "DeviceBufferParameter<ShaderBuffer<ShaderFloat4>> lights;\nGraphTextureUavParameter<ShaderRWTexture2D<float, ShaderImageR32f>> target;\n" +
                "GraphBufferSrvParameter<ShaderStructuredBuffer<ShaderFloat4x4>> bones;\nGraphBufferUavParameter<ShaderRWByteAddressBuffer> words;\n" +
                "GraphBufferUavParameter<ShaderRWBuffer<rocks::uint32, ShaderImageR32ui>> histogram;\nGraphTextureAccessParameter history;\n" +
                "GraphRenderTargetParameter targets;\nSceneTextures scene;\n};\n", ShaderResourcePreamble);
            Verify.Equal(0, exit, probe.LastStdErr);
            string header = probe.Header;
            Verify.Contains("static constexpr Span<const ShaderParameterMember> Members{};", header, "resource-only struct has no constant members");
            Verify.Contains("{\"albedo\", \"albedoSampler\", ShaderParameterResourceKind::DeviceTexture, 32, ShaderResourceShape::Texture2D, " +
                "ShaderComponentType::Float, 0, ShaderParameterSampler::Filter},", header, "device texture row after the 32-byte constant part");
            Verify.Contains("{\"codes\", \"\", ShaderParameterResourceKind::DeviceTexture, 64, ShaderResourceShape::Texture2DArray, " +
                "ShaderComponentType::Uint, 0, ShaderParameterSampler::None},", header, "integer textures have no sampler pair");
            Verify.Contains("ShaderParameterSampler::Comparison", header, "comparison sampler pair");
            Verify.Contains("ShaderResourceShape::StructuredBuffer, ShaderComponentType::None, 64,", header, "structured stride");
            Verify.Contains("{\"history\", \"\", ShaderParameterResourceKind::GraphTextureAccess,", header, "access-only member row");
            Verify.Contains("{\"scene_depth\", \"scene_depthSampler\", ShaderParameterResourceKind::GraphTexture,", header, "included struct is flattened with a prefix");
            Verify.Contains("\"Pass\", false, 32, Members,\n                                                Resources,", header, "constant part size and resource list");
            Verify.Contains("ROCKS_OFFSET_OF(::probe::PassParameters, scene) ==", header, "include member offset assertion");
            Verify.Contains("#include <ShaderCore/ShaderParameterLayout.h>", header, "layout header");
            string pass = probe.Shader("PassParameters");
            Verify.Contains("cbuffer Pass {\n    float4 tint;\n    float scale;\n};\n\nTexture2D<float4> albedo;\nSamplerState albedoSampler;\n" +
                "Texture2DArray<uint4> codes;\nTexture2D<float> shadow;\nSamplerComparisonState shadowSampler;\nTexture2DMS<float4> coverage;\n" +
                "Buffer<float4> lights;\nROCKS_IMAGE_FORMAT(r32f) RWTexture2D<float> target;\nStructuredBuffer<float4x4> bones;\n" +
                "RWByteAddressBuffer words;\nROCKS_IMAGE_FORMAT(r32ui) RWBuffer<uint> histogram;\nTexture2D<float> scene_depth;\n" +
                "SamplerState scene_depthSampler;\nTexture2D<float4> scene_color;\nSamplerState scene_colorSampler;\n", pass, "global resource declarations");
            Verify.That(!pass.Contains("history", StringComparison.Ordinal) && !pass.Contains("targets", StringComparison.Ordinal),
                "access-only and render target members are not in the shader source");
            Verify.Contains("#pragma once\n\nTexture2D<float> depth;\nSamplerState depthSampler;\n", probe.Shader("SceneTextures"),
                "resource-only struct source has no cbuffer");
        });

        // 자원 멤버 음성 입력이다(16.2절 SSD59–SSD61).
        const string texture = "GraphTextureParameter<ShaderTexture2D<>>";
        (string Name, string Body, string Code, string Reason)[] rejected =
        [
            ("ConstantAfterResource", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {{\n{texture} t;\nfloat x;\n}};\n", "RMC6009",
                "constant members come before resource members"),
            ("RootResource", $"ROCKS_SHADER_PARAMETERS(Name = \"R\", RootConstants)\nstruct alignas(16) S {{\nShaderFloat4 v;\n{texture} t;\n}};\n",
                "RMC6009", "root constants take"),
            ("WrongDescriptor", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nGraphTextureParameter<ShaderBuffer<float>> t;\n};\n", "RMC6011",
                "descriptor"),
            ("StructuredThree", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nGraphBufferSrvParameter<ShaderStructuredBuffer<ShaderFloat3>> b;\n};\n",
                "RMC6011", "3-component"),
            ("MatrixTexture", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nGraphTextureParameter<ShaderTexture2D<ShaderFloat4x4>> t;\n};\n",
                "RMC6011", "element type"),
            ("FormatMismatch", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nGraphTextureUavParameter<ShaderRWTexture2D<ShaderFloat4, ShaderImageR32f>> t;\n};\n",
                "RMC6011", "image format"),
            ("ComparisonInt", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nGraphTextureParameter<ShaderTexture2D<rocks::uint32, ShaderComparisonSampler>> t;\n};\n",
                "RMC6011", "comparison samplers"),
            ("ResourceArray", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {{\n{texture} t[2];\n}};\n", "RMC6007", "resource arrays"),
            ("PairNameClash", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {{\n{texture} t;\n{texture} tSampler;\n}};\n", "RMC6010",
                "repeats another shader name"),
            ("ReservedName", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {{\n{texture} SharedWrap;\n}};\n", "RMC6010", "reserved"),
            ("ConstantNameClash", $"ROCKS_SHADER_PARAMETERS(Name = \"t\")\nstruct alignas(16) S {{\nShaderFloat4 v;\n{texture} t;\n}};\n", "RMC6010",
                "repeats another shader name"),
            ("IncludeClash", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) I {{\n{texture} b;\n}};\n" +
                $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {{\n{texture} a_b;\nI a;\n}};\n", "RMC6010", "repeats another shader name"),
            ("IncludeWithConstants", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) I {{\nShaderFloat4 v;\n{texture} t;\n}};\n" +
                "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nShaderFloat4 w;\nI inner;\n};\n", "RMC6007", "included resource struct"),
            ("IncludeRenderTarget", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) I {{\n{texture} t;\nGraphRenderTargetParameter targets;\n}};\n" +
                "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nI inner;\n};\n", "RMC6007", "included resource struct"),
            ("IncludeTwoLevels", $"ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) A {{\n{texture} t;\n}};\n" +
                "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) B {\nA a;\n};\nROCKS_SHADER_PARAMETERS()\nstruct alignas(16) C {\nB b;\n};\n",
                "RMC6007", "included resource struct"),
            ("TwoRenderTargets", "ROCKS_SHADER_PARAMETERS()\nstruct alignas(16) S {\nGraphRenderTargetParameter a;\nGraphRenderTargetParameter b;\n};\n",
                "RMC6007", "at most one render target"),
        ];
        foreach ((string name, string body, string code, string reason) in rejected)
        {
            suite.Add("Meta.ShaderParametersRejectResource" + name, async () =>
            {
                using ShaderParameterProbe probe = new(options, actualEnvironment, generator, compiler);
                await probe.RejectAsync(body, code, reason, ShaderResourcePreamble);
            });
        }
    }
}
