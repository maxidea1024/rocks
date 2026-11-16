using Rocks.Build.Rules;

// SPIRV-Reflect(Vulkan SDK 1.4.363.0 태그) C 번역 단위를 Rocks 경고 규칙으로 빌드한다(Apache-2.0, 동봉 spirv.h는 MIT).
// 셰이더 워커가 SPIR-V 리플렉션에 쓴다(Docs/Design/ShaderSystem.ko.md 8절 5항). 소비 모듈은 C API만 쓴다(C++ 래퍼는 STL을 쓰므로
// consumerDefines의 SPIRV_REFLECT_DISABLE_CPP_BINDINGS로 끈다). 소스·정의·파일 해시는 같은 폴더의 ThirdParty.json이 단일 출처다.
public sealed class SpirvReflect : ThirdPartyModuleRules
{
    protected override string ModuleName => "SpirvReflect";
}
