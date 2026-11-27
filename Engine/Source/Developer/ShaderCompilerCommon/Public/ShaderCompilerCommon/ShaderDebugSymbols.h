#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 셰이더 Record 값 3(DebugSymbols)의 배치다(Docs/Design/ShaderSystem 16.3절 SSD73). 워커의 형식
// 처리기가 만들고 메인 프로세스(라이브러리 만들기의 심볼 파일 쓰기 — SSD74, shader-compile --out)가
// 읽는다. 쿠킹 라이브러리에는 넣지 않는다(12.3절).
//
// 배치(little-endian): u32 배치 버전(ShaderDebugSymbolsVersion), u8 종류(ShaderDebugSymbolKind),
// u16 파일 이름 byte 수, 파일 이름(디렉터리 없는 UTF-8 — 비면 메인 프로세스가 "<job 키
// hex>.<확장자>"로 정한다), 나머지가 내용이다.
//
// 형식별 내용:
// - D3D12_SM6(DXC): Pdb — DXC_OUT_PDB와 그 이름(-Zsb의 "<hash>.pdb", DXIL 디버그 이름 part가
// 가리킨다).
// - D3D11_SM5(FXC): Pdb — D3DGetBlobPart(D3D_BLOB_PDB)와 D3D_BLOB_DEBUG_NAME의 이름.
// - VULKAN_SM5: SpirvModule — 같은 원문을 DXC -Zi로 다시 컴파일한 SPIR-V(OpSource·OpLine, 이름
// 없음).
// - GLES31_PC·METAL_SM5(텍스트 형식): 값 3이 없다.
//
// 사용 예(형식 처리기):
//   Array<uint8> value(allocator);
//   if (auto encoded = EncodeShaderDebugSymbols(ShaderDebugSymbolKind::Pdb, pdbName, pdb, value);
//       !encoded) { ... }
// 사용 예(메인 프로세스):
//   auto symbols = DecodeShaderDebugSymbols(values.Part(ShaderNuggetValue::DebugSymbols).View());
//   if (symbols) { ... symbols.Value().name, symbols.Value().contents ... }
namespace rocks::shader {

inline constexpr uint32 ShaderDebugSymbolsVersion = 1;
// 파일 이름의 최대 byte 수다(DXC·FXC 이름은 hash 64자 + 확장자다).
inline constexpr usize MaxShaderDebugSymbolNameBytes = 200;

enum class ShaderDebugSymbolKind : uint8 {
    // D3D PDB(DXC·FXC).
    Pdb = 1,
    // 디버그 정보(OpSource·OpString·OpLine)가 든 SPIR-V 모듈.
    SpirvModule = 2
};

// 종류의 파일 확장자(".pdb"·".spv")다. 범위 밖이면 ".bin".
ROCKS_SHADERCOMPILERCOMMON_API const char*
ShaderDebugSymbolExtension(ShaderDebugSymbolKind kind) noexcept;

// 읽은 값 3이다. view는 읽은 byte를 빌린다.
struct ShaderDebugSymbolsView {
    ShaderDebugSymbolKind kind = ShaderDebugSymbolKind::Pdb;
    StringView name;
    Span<const uint8> contents;
};

// 값 3 byte를 out에 만든다(out은 먼저 비운다). 오류: 종류가 범위 밖이거나 이름이
// MaxShaderDebugSymbolNameBytes를 넘거나 '/'·'\\'·':'·NUL이 있거나 "."·".."이면 InvalidArgument,
// 할당 실패 OutOfMemory.
ROCKS_SHADERCOMPILERCOMMON_API Result<void> EncodeShaderDebugSymbols(ShaderDebugSymbolKind kind,
                                                                     StringView name,
                                                                     Span<const uint8> contents,
                                                                     Array<uint8>& out) noexcept;

// 값 3 byte를 읽는다. 짧거나 배치 버전·종류가 다르거나 이름이 위 규칙을 어기면 CorruptData다.
ROCKS_SHADERCOMPILERCOMMON_API Result<ShaderDebugSymbolsView>
DecodeShaderDebugSymbols(Span<const uint8> bytes) noexcept;

} // namespace rocks::shader
