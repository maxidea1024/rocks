#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Text/StringView.h>

// 셰이더 도구 표다(Docs/Design/ShaderSystem.ko.md 9절). 빌드가 Engine 루트의 kind: binary manifest
// (Engine/Source/ThirdParty/<Lib>/ThirdParty.json)에서 정의
// TU(<출력>/GeneratedSources/ShaderToolTable/ ShaderToolTable.Generated.cpp)를 만들어 이 모듈에
// 넣는다(BuildInfo와 같은 생성 방식). 그래서 메인 프로세스(job 키 계산)와 워커(도구 열기)가 같은
// manifest에서 만든 같은 표를 가진다.
//
// 표는 메타데이터만 가진다: 도구 이름·버전·출처, 사용자 저장소 디렉터리 이름, 빌드 대상 호스트의
// 파일 (경로·SHA-256·크기·role). 파일을 열거나 저장소를 보지 않는다. 도구를 여는 것은 워커 쪽
// ShaderToolLoader다.
//
// 참조 제한: 이 헤더와 모듈은 Developer/ShaderCompiler·워커(Programs/ShaderCompileWorker)·
// Developer/ShaderFormat*·Developer/ShaderToolLoader만 쓴다. Runtime 모듈이 참조하면 정책 검사
// shader-tool-table이 거부한다.
//
// 사용 예(job 키에 도구 지문을 넣는다):
//   const shader::ShaderTool* dxc = shader::FindShaderTool(shader::DxcToolName);
//   const shader::ShaderToolFile* runtime =
//       dxc != nullptr ? shader::FindShaderToolFile(*dxc, shader::ShaderToolFileRole::Runtime) :
//       nullptr;
//   if (runtime == nullptr) { ... }      // 이 호스트에 DXC가 없다(manifest에 호스트 항목이 없음)
//   hasher.Update(Span<const uint8>(runtime->sha256.bytes, Sha256Digest::Bytes));
//
// 모든 함수는 실패하지 않고, 돌려준 포인터·문자열은 정적 수명이다(스레드 안전).
namespace rocks::shader {

// 도구 출처다(manifest 필드). Build: Rocks 오프라인 도구 빌드가 게시한 archive. Sdk: 버전을 고정한
// 설치 SDK에서 bootstrap이 복사한 파일(예: FXC). System: 저장소로 받지 않는 시스템 도구(Apple
// metal·metallib, 최소 버전만 고정하고 파일 SHA가 없다).
enum class ShaderToolOrigin : uint8 {
    Build,
    Sdk,
    System
};

// 파일 역할이다(manifest role). Runtime: 워커가 여는 공유 라이브러리. Redist: Runtime 실행 파일
// 옆에 복사만 하는 재배포 파일. Symbols: PDB·dSYM. Tool: 실행 파일. Notice: 라이선스 고지.
enum class ShaderToolFileRole : uint8 {
    Runtime,
    Redist,
    Symbols,
    Tool,
    Notice
};

// 빌드 대상 호스트의 파일 하나다. path는 저장소 디렉터리 기준 '/' 상대 경로(System은 실행 파일
// 이름)다. hasDigest가 false(System)이면 sha256·size는 0이다.
struct ShaderToolFile {
    const char* path = nullptr;
    Sha256Digest sha256;
    uint64 size = 0;
    ShaderToolFileRole role = ShaderToolFileRole::Runtime;
    bool hasDigest = false;
};

// 도구 하나다. name은 manifest 이름(아래 *ToolName), version은 고정 버전(System은 최소 버전)이다.
// storeKey는 사용자 저장소의 디렉터리 이름(build는 archive SHA-256, sdk는 파일 SHA-256, 64자 소문자
// 16진수)이며 System이거나 이 호스트 파일이 없으면 nullptr다. 이 호스트 파일이 없으면 fileCount가
// 0이다(다른 호스트 산출물로 대신하지 않는다).
struct ShaderTool {
    const char* name = nullptr;
    const char* version = nullptr;
    ShaderToolOrigin origin = ShaderToolOrigin::System;
    const char* storeKey = nullptr;
    const ShaderToolFile* files = nullptr;
    usize fileCount = 0;
};

// 셰이더 시스템이 쓰는 도구의 manifest 이름이다(Engine/Source/ThirdParty/<이름>).
inline constexpr const char DxcToolName[] = "DirectXShaderCompiler";
inline constexpr const char FxcToolName[] = "D3DCompiler";
inline constexpr const char SpirvCrossToolName[] = "SpirvCross";
inline constexpr const char SpirvToolsToolName[] = "SpirvTools";
inline constexpr const char MetalToolName[] = "MetalToolchain";

// 표를 만든 빌드 대상 호스트 키다("windows-x64"·"macos-arm64" 등). 개발 호스트가 아닌 대상
// (Android)은 빈 문자열이고 모든 도구의 fileCount가 0이다. 정의는 생성 TU에 있다.
ROCKS_SHADERTOOLTABLE_API const char* ShaderToolHostKey() noexcept;

// 모든 binary 도구다(이름 ordinal 순). 정의는 생성 TU에 있다.
ROCKS_SHADERTOOLTABLE_API Span<const ShaderTool> ShaderTools() noexcept;

// 이름이 정확히 같은(대소문자 구분) 도구다. 없으면 nullptr다.
ROCKS_SHADERTOOLTABLE_API const ShaderTool* FindShaderTool(StringView name) noexcept;

// tool의 이 호스트 파일 가운데 role이 같은 첫 파일이다. 없으면 nullptr다.
ROCKS_SHADERTOOLTABLE_API const ShaderToolFile*
FindShaderToolFile(const ShaderTool& tool, ShaderToolFileRole role) noexcept;

} // namespace rocks::shader
