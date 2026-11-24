#pragma once

#include <ShaderCore/ShaderBindingLayout.h>

// ShaderCompilerCommon 안에서 함께 쓰는 도우미다.
namespace rocks::shader {

// layout 자원을 (종류, slot) 순으로 다시 맞추고 멤버 범위(firstMember)를 그 순서로 잇는다(slot을
// backend 위치로 바꾼 뒤 — Metal·GLES 배정, 16.1절 SSD39·SSD42).
void SortShaderBindingResources(ShaderBindingLayout& layout) noexcept;

} // namespace rocks::shader
