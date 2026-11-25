#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatFacts.h>
#include <ShaderCore/ShaderReadStatus.h>
#include <ShaderCore/ShaderType.h>

// 셰이더 라이브러리 파일 형식 v1(.rshl)이다(Docs/Design/ShaderSystem 12.2.1절 SSD7·SSD8, UE
// FSerializedShaderArchive 대응).
//
// 형식 하나·라이브러리(Engine·Project) 하나의 쿠킹된 셰이더 전부다. 쿠커(CookShaderLibraries)와
// Development 공급자(메모리 라이브러리, SSD16)가 BuildShaderLibrary로 같은 byte를 만들고, 런타임
// 셰이더 집합이 ShaderLibraryView로 연다(RenderBase 9.2절).
//
// byte 형식(little-endian, 섹션은 8 byte 정렬, 정렬·stride 사이 빈 byte는 0):
//   머리(ShaderLibraryHeaderBytes):
//     0   magic "RSHL"                    4   u32 형식 버전(= ShaderLibraryVersion)
//     8   u8 라이브러리(0 Engine·1 Project), 0 × 7
//     16  형식 id(MaxShaderFormatIdBytes, NUL 종료·남는 byte 0)
//     48  형식 정의 SHA-256               80  셰이더 타입 집합 SHA-256(SSD3)
//     112 형식 사실: u8 backend, u8 셰이더 모델 major, u8 minor, 0, u32 features,
//         u32 deviceRequirements, u32 상한 13개(constantBuffers, sampledTextures, storageBuffers,
//         storageImages, samplers, uavTotal, uavStages, renderTargets, interStageVectors,
//         rootConstantBytes, uniformVectors VS·PS·CS)
//     176 u32 항목 수, u32 코드 수, u32 이름 수, 0 × 4
//     192 u64 항목 표 오프셋, 코드 표 오프셋, 이름 표 오프셋, 코드 부분 오프셋, 코드 부분 byte 수
//     232 파일 SHA-256(이 칸을 0으로 둔 파일 전체의 SHA-256)
//   항목 표(키 오름차순, ShaderLibraryEntryStride): u64 묶음 id, u64 타입 nameHash, u64 정점 입력
//     타입 id, u32 퍼뮤테이션 ID, u8 단계, u8 개발 전용(0·1), 0 × 2, u32 런타임 확인 기능 비트,
//     u32 코드 색인, 레이아웃 서명(32).
//   코드 표(코드 SHA-256 오름차순, ShaderLibraryCodeStride): 코드 SHA-256, u64 오프셋(코드 부분
//     기준), u64 byte 수. 코드는 코드 부분에 표 순서로 8 byte 정렬해 이어 둔다.
//   이름 표(nameHash 오름차순, ShaderLibraryNameStride): u64 nameHash, u8 이름 byte 수, 0 × 7,
//     이름(MaxShaderTypeNameBytes, 남는 byte 0). 항목에 나오는 nameHash마다 한 줄이다.
//   코드 하나: u32 값 수, 값마다 (u32 값 id, u32 byte 수, byte). 값 id는 오름차순이고 0 Code가
//     반드시 있으며 0·1·2만 담는다(3 DebugSymbols는 따로 — SH4).
//
// 이름 표(CP7): 이름이 정체성이고 nameHash는 조회용이다. 열 때 이름에서 ShaderTypeNameHash를 다시
// 계산해 저장된 해시와 다르면 CorruptPackage다.
//
// 사용 예(런타임 — bytes가 view보다 오래 살아야 한다):
//   ShaderLibraryView library;
//   if (const ShaderReadStatus opened = library.Open(bytes); !opened) { ... }
//   ShaderLibraryEntry entry;
//   if (library.Find(ShaderLibraryKey{0, FullscreenTriangleVS.hash, NoVertexInputType, 0}, entry))
//   {
//       const Span<const uint8> code = library.Value(entry, ShaderLibraryValueCode);
//   }
namespace rocks::shader {

inline constexpr char ShaderLibraryMagic[4] = {'R', 'S', 'H', 'L'};
inline constexpr uint32 ShaderLibraryVersion = 1;
// 파일 확장자다(점 없이).
inline constexpr char ShaderLibraryExtension[] = "rshl";

inline constexpr usize ShaderLibraryHeaderBytes = 264;
inline constexpr usize ShaderLibraryEntryStride = 72;
inline constexpr usize ShaderLibraryCodeStride = 48;
inline constexpr usize ShaderLibraryNameStride = 80;
// 섹션·코드 정렬 byte 수다.
inline constexpr usize ShaderLibraryAlignment = 8;

// 라이브러리에 담는 셰이더 Record 값 id다(ShaderCompiler ShaderNuggetValue와 같은 번호 — 10절 값
// 표).
inline constexpr uint32 ShaderLibraryValueCode = 0;
inline constexpr uint32 ShaderLibraryValueBindingLayout = 1;
inline constexpr uint32 ShaderLibraryValueGlesCopyTable = 2;
// 코드 하나의 최대 값 수다(0·1·2).
inline constexpr uint32 MaxShaderLibraryValues = 3;

// 표 원소 수 상한이다(u32 칸 — 쿠킹 결과가 이보다 크면 조합 폭발이다).
inline constexpr uint32 MaxShaderLibraryEntries = 1u << 24;

// 라이브러리 이름이다(SSD3 — 소속 Engine·EnginePlugin → Engine, Project·ProjectPlugin → Project).
enum class ShaderLibraryKind : uint8 {
    Engine = 0,
    Project = 1
};

// 라이브러리 이름("Engine"·"Project", 정적 수명)이다. 범위 밖이면 "unknown".
ROCKS_SHADERCORE_API const char* ShaderLibraryKindName(ShaderLibraryKind kind) noexcept;

// 소속이 담기는 라이브러리다.
constexpr ShaderLibraryKind ShaderLibraryKindOf(ShaderTypeOwner owner) noexcept {
    return owner == ShaderTypeOwner::Project || owner == ShaderTypeOwner::ProjectPlugin
               ? ShaderLibraryKind::Project
               : ShaderLibraryKind::Engine;
}

// 항목 키다(정렬 순서: bundleId → nameHash → vertexInputType → permutationId).
struct ShaderLibraryKey {
    // 묶음 id(엔진 셰이더 0, 머티리얼은 셰이더 집합 id — RenderBase SX5)다.
    uint64 bundleId = 0;
    uint64 nameHash = 0;
    VertexInputTypeId vertexInputType = NoVertexInputType;
    uint32 permutationId = 0;

    friend constexpr bool operator==(const ShaderLibraryKey&,
                                     const ShaderLibraryKey&) noexcept = default;
};

// 키 a가 b보다 앞서는가.
constexpr bool ShaderLibraryKeyLess(const ShaderLibraryKey& a, const ShaderLibraryKey& b) noexcept {
    if (a.bundleId != b.bundleId) {
        return a.bundleId < b.bundleId;
    }

    if (a.nameHash != b.nameHash) {
        return a.nameHash < b.nameHash;
    }

    if (a.vertexInputType != b.vertexInputType) {
        return a.vertexInputType < b.vertexInputType;
    }

    return a.permutationId < b.permutationId;
}

// 코드의 값 하나다(만들기 입력).
struct ShaderLibraryValue {
    uint32 id = 0;
    Span<const uint8> bytes;
};

// 만들기 입력 항목이다. values는 값 id 오름차순이어야 하고 0 Code를 담아야 한다.
struct ShaderLibraryBuildEntry {
    ShaderLibraryKey key;
    // 셰이더 타입 이름(nameHash = ShaderTypeNameHash(typeName)이어야 한다)이다.
    StringView typeName;
    uint32 stage = 0;
    bool developmentOnly = false;
    // 요구 기능 가운데 런타임 확인 기능(RuntimeCheckedShaderFeatures)만이다.
    uint32 runtimeFeatures = 0;
    Sha256Digest layoutSignature{};
    Span<const ShaderLibraryValue> values;
};

struct ShaderLibraryBuildDesc {
    ShaderLibraryKind kind = ShaderLibraryKind::Engine;
    ShaderFormatFacts facts;
    Sha256Digest formatDefinitionSha256{};
    Sha256Digest typeSetSha256{};
    // 순서는 상관없다(만들기가 정렬한다).
    Span<const ShaderLibraryBuildEntry> entries;
};

// desc로 v1 byte를 만들어 out을 채운다(기존 내용은 지운다). 같은 입력(순서 무관)이면 같은 byte다.
// 같은 코드(값 byte가 모두 같음)는 한 번만 담는다. 오류(InvalidArgument — reason에 영어 정적 문구):
// 형식 id·backend·라이브러리가 잘못됨, 항목 수 상한 초과, 같은 키 둘, 타입 이름이 식별자가 아니거나
// 해시가 맞지 않음, 같은 해시의 다른 이름, 단계가 한 비트가 아님, runtimeFeatures가 런타임 확인
// 기능 밖, 값이 오름차순이 아니거나 0이 없거나 0·1·2 밖, 값 byte가 u32를 넘음. 할당 실패는
// OutOfMemory다.
ROCKS_SHADERCORE_API Result<void> BuildShaderLibrary(const ShaderLibraryBuildDesc& desc,
                                                     Allocator& allocator, Array<uint8>& out,
                                                     const char** reason = nullptr) noexcept;

// 열린 라이브러리의 항목 하나다.
struct ShaderLibraryEntry {
    ShaderLibraryKey key;
    uint32 stage = 0;
    bool developmentOnly = false;
    uint32 runtimeFeatures = 0;
    Sha256Digest layoutSignature{};
    uint32 codeIndex = 0;
};

// 라이브러리 byte를 읽는 view다(할당하지 않는다 — 원본 byte를 가리킨다). Open이 모든 규칙을 한 번
// 검사하므로 그 뒤 조회는 검사 없이 읽는다. 열린 뒤 읽기 전용이며 여러 스레드가 함께 읽어도 된다.
class ROCKS_SHADERCORE_API ShaderLibraryView final {
public:
    // bytes를 검사하고 연다. 검사(실패하면 view는 닫힌 상태):
    // - 길이·magic(CorruptPackage), 형식 버전(1이 아니면 UnsupportedVersion — SHA보다 먼저 본다),
    //   파일 SHA-256.
    // - 머리: 라이브러리·backend 범위, 형식 id 철자, 빈 byte 0, 섹션 오프셋이 정규 값, 파일 길이.
    // - 항목: 키 순증가, 단계 한 비트, 개발 전용 0·1, 런타임 기능 범위, 코드 색인 범위, nameHash가
    //   이름 표에 있음.
    // - 코드: SHA 순증가, 오프셋 정규 값, 코드 SHA = 내용 SHA, 값 틀(오름차순·0 있음·0·1·2만·길이
    //   정확), 빈 byte 0.
    // - 이름: 해시 순증가, 이름이 셰이더 타입 이름 규칙, 해시 = ShaderTypeNameHash(이름), 빈 byte
    // 0,
    //   (쓰이지 않는 코드·이름은 읽기가 막지 않는다 — 할당 없이 셀 수 없어서다.
    //   BuildShaderLibrary는 쓰이는 것만 담는다.)
    ShaderReadStatus Open(Span<const uint8> bytes) noexcept;

    void Close() noexcept;

    bool IsOpen() const noexcept {
        return !bytes_.IsEmpty();
    }

    ShaderLibraryKind Kind() const noexcept {
        return kind_;
    }

    const ShaderFormatFacts& Facts() const noexcept {
        return facts_;
    }

    const Sha256Digest& FormatDefinitionSha256() const noexcept {
        return formatDefinitionSha256_;
    }

    const Sha256Digest& TypeSetSha256() const noexcept {
        return typeSetSha256_;
    }

    const Sha256Digest& FileSha256() const noexcept {
        return fileSha256_;
    }

    uint32 EntryCount() const noexcept {
        return entryCount_;
    }

    uint32 CodeCount() const noexcept {
        return codeCount_;
    }

    uint32 NameCount() const noexcept {
        return nameCount_;
    }

    // index번째 항목(키 순)이다. index < EntryCount()여야 한다.
    ShaderLibraryEntry Entry(uint32 index) const noexcept;

    // key의 항목을 찾는다(이진 탐색). 없으면 false다.
    bool Find(const ShaderLibraryKey& key, ShaderLibraryEntry& out) const noexcept;

    // 항목 코드의 값 byte다(원본을 가리킨다). 없는 값이면 빈 Span이다.
    Span<const uint8> Value(const ShaderLibraryEntry& entry, uint32 valueId) const noexcept;

    // 코드의 SHA-256(코드 표)이다. codeIndex < CodeCount()여야 한다.
    Sha256Digest CodeSha256(uint32 codeIndex) const noexcept;

    // nameHash의 타입 이름이다(원본을 가리킨다). 없으면 빈 문자열이다.
    StringView TypeName(uint64 nameHash) const noexcept;

private:
    Span<const uint8> CodeBytes(uint32 codeIndex) const noexcept;

    Span<const uint8> bytes_;
    ShaderLibraryKind kind_ = ShaderLibraryKind::Engine;
    ShaderFormatFacts facts_;
    Sha256Digest formatDefinitionSha256_{};
    Sha256Digest typeSetSha256_{};
    Sha256Digest fileSha256_{};
    uint32 entryCount_ = 0;
    uint32 codeCount_ = 0;
    uint32 nameCount_ = 0;
    usize entryOffset_ = 0;
    usize codeOffset_ = 0;
    usize nameOffset_ = 0;
    usize blobOffset_ = 0;
};

} // namespace rocks::shader
