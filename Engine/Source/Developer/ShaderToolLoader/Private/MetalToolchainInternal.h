#pragma once

#include <ShaderToolLoader/MetalToolchain.h>

// Metal 도구 찾기의 플랫폼 부분이다(Private/<플랫폼>이 구현한다).
namespace rocks::shader::metal_toolchain_detail {

// 이 호스트의 metal·metallib 후보 절대 경로를 쓴다(있는지는 호출자가 본다). 후보가 없으면 false다.
bool FindMetalExecutables(char (&metal)[MaxShaderToolPathBytes],
                          char (&metallib)[MaxShaderToolPathBytes]) noexcept;

} // namespace rocks::shader::metal_toolchain_detail
