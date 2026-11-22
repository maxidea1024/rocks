#pragma once

#include <AssetRuntime/PackageReader.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Guid.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Object/AssetId.h>

// 에디터 패키지 ImportRecord 섹션의 값 스키마다(Docs/Design/AssetSystem.ko.md 5.4절, 7.1절 15 —
// A2c2a). 패키지 형식 층(AssetRuntime)은 ImportRecord를 Record envelope로만 검사하고, 이 스키마는
// AssetDatabase가 쓰고 읽는다. UE의 AssetImportData(원본 파일 목록·해시·임포트 옵션)에 대응한다.
//
// 기록은 둘 중 하나다:
//   - 소유 기록(owner = 자기 패키지 id): 임포터·원본·옵션·출력 전체. 다중 출력 임포트(예: 메시와
//     머티리얼 여러 개)는 출력 하나가 기록을 소유한다.
//   - 참조 기록(owner ≠ 자기): 소유 패키지만 가리킨다. 그 패키지의 Dependencies에 owner를 가리키는
//     ImportOwner 항목이 하나 있어 소유자 삭제를 막는다(CheckImportOwner).
//
// 배치(스키마 버전 1, 필드 id = Guid{0, n}, 문자열·byte payload는 길이 접두 없는 내용):
//   Record(ImportRecordSchema){ 1 importerId Guid, 2 importerName String, 3 importerVersion U32,
//     4 sources RecordList[Record(ImportSourceSchema){ 1 role String, 2 root String,
//       3 relativePath String, 4 sha256 Bytes(32), 5 bytes U64 }],
//     5 options Record(임포터 스키마), 6 outputs RecordList[Record(ImportOutputSchema){
//       1 outputKey String, 2 assetId Guid, 3 typeId Guid }], 7 ownerAssetId Guid }
//
// 사용 예(텍스처 하나를 임포트한 소유 기록):
//   const ImportSourceInput sources[] = {{"source", "Raw", "Textures/Rock.png", digest, size}};
//   const ImportOutputInput outputs[] = {{"", self, textureType}};
//   ImportRecordInput input;
//   input.owner = self;
//   input.importerId = textureImporterId;
//   input.importerName = "TextureImporter";
//   input.importerVersion = 3;
//   input.sources = sources;
//   input.outputs = outputs;
//   auto bytes = WriteImportRecord(allocator, self, input);   // PackageWriteOptions::importRecord
//   auto read = ReadImportRecord(allocator, self, contents.importRecord);
namespace rocks {

inline constexpr Guid ImportRecordSchema{0x52494d5052454344ull, 0x0000000000000001ull};
inline constexpr Guid ImportSourceSchema{0x52494d5052454344ull, 0x0000000000000002ull};
inline constexpr Guid ImportOutputSchema{0x52494d5052454344ull, 0x0000000000000003ull};
inline constexpr uint32 ImportRecordVersion = 1;

inline constexpr Guid ImportRecordImporterIdField{0, 1};
inline constexpr Guid ImportRecordImporterNameField{0, 2};
inline constexpr Guid ImportRecordImporterVersionField{0, 3};
inline constexpr Guid ImportRecordSourcesField{0, 4};
inline constexpr Guid ImportRecordOptionsField{0, 5};
inline constexpr Guid ImportRecordOutputsField{0, 6};
inline constexpr Guid ImportRecordOwnerField{0, 7};

inline constexpr Guid ImportSourceRoleField{0, 1};
inline constexpr Guid ImportSourceRootField{0, 2};
inline constexpr Guid ImportSourcePathField{0, 3};
inline constexpr Guid ImportSourceSha256Field{0, 4};
inline constexpr Guid ImportSourceBytesField{0, 5};

inline constexpr Guid ImportOutputKeyField{0, 1};
inline constexpr Guid ImportOutputAssetField{0, 2};
inline constexpr Guid ImportOutputTypeField{0, 3};

// 문자열 byte 상한이다(하한: 임포터 이름·role·relativePath는 1, root·outputKey는 0).
inline constexpr usize MaxImporterNameBytes = 256;
inline constexpr usize MaxImportRoleBytes = 64;
inline constexpr usize MaxImportRootBytes = 256;
inline constexpr usize MaxImportRelativePathBytes = 1024;
inline constexpr usize MaxImportOutputKeyBytes = 64;
// 소유 기록의 원본·출력 개수 상한이다(하한은 둘 다 1).
inline constexpr usize MaxImportSources = 256;
inline constexpr usize MaxImportOutputs = 1024;

// 쓰기 입력의 원본 하나다(호출 동안만 빌린다). role은 원본의 역할(2D "source", Cube 면 "px" 등 —
// 5.4절), root는 원본 루트 이름(빈 값은 루트 없음), relativePath는 그 루트 기준 `/` 상대 경로,
// sha256·bytes는 임포트 때 원본 내용의 해시와 크기다.
struct ImportSourceInput {
    StringView role;
    StringView root;
    StringView relativePath;
    Sha256Digest sha256{};
    uint64 bytes = 0;
};

// 쓰기 입력의 출력 하나다. outputKey는 임포터가 정한 출력 이름(빈 값은 주 출력)이다.
struct ImportOutputInput {
    StringView outputKey;
    AssetId asset{};
    TypeId type{};
};

// 쓰기 입력이다. 참조 기록은 owner만 채운다(나머지는 기본값). options는 임포터 스키마로 만든
// Record byte(빈 값은 옵션 없음)다.
struct ImportRecordInput {
    AssetId owner{};
    Guid importerId{};
    StringView importerName{};
    uint32 importerVersion = 0;
    Span<const ImportSourceInput> sources{};
    Span<const uint8> options{};
    Span<const ImportOutputInput> outputs{};
};

// 읽은 원본 하나다(소유).
struct ImportSource {
    explicit ImportSource(Allocator& allocator) noexcept
        : role(allocator), root(allocator), relativePath(allocator) {
    }

    String role;
    String root;
    String relativePath;
    Sha256Digest sha256{};
    uint64 bytes = 0;
};

// 읽은 출력 하나다(소유).
struct ImportOutput {
    explicit ImportOutput(Allocator& allocator) noexcept : outputKey(allocator) {
    }

    String outputKey;
    AssetId asset{};
    TypeId type{};
};

// 읽은 기록이다(소유 — 입력 byte를 빌리지 않는다). 참조 기록은 owner만 의미가 있고 나머지는
// 기본값·빈 목록이다. outputs는 outputKey byte 순이다.
struct ImportRecord {
    explicit ImportRecord(Allocator& allocator) noexcept
        : importerName(allocator), sources(allocator), options(allocator), outputs(allocator) {
    }

    ImportRecord(ImportRecord&&) noexcept = default;
    ImportRecord& operator=(ImportRecord&&) noexcept = default;
    ImportRecord(const ImportRecord&) = delete;
    ImportRecord& operator=(const ImportRecord&) = delete;

    AssetId owner{};
    Guid importerId{};
    String importerName;
    uint32 importerVersion = 0;
    Array<ImportSource> sources;
    Array<uint8> options;
    Array<ImportOutput> outputs;

    // 패키지 self의 기록이 소유 기록인지다(owner == self).
    bool OwnedBy(AssetId self) const noexcept {
        return owner == self;
    }
};

// 원본 위치(root·relativePath)가 기록 규칙에 맞는지다(쓰기·읽기와 같은 판정 — AS 4.5절, 7.1절
// 15·18): root는 0–MaxImportRootBytes byte·`/`·`\` 없음, root가 있으면 relativePath는 `/` 구분 상대
// 경로, 비면 `/` 구분 절대 경로(POSIX `/a`·UNC `//server/share/a`·드라이브 `X:/a`).
// AssetSourceRoots가 기록을 풀기 전에 쓴다.
bool IsValidImportSourceLocation(StringView root, StringView relativePath) noexcept;

// self 패키지의 ImportRecord byte를 쓴다(PackageWriteOptions::importRecord에 넘긴다). outputs는
// outputKey byte 순으로 정렬해 쓰고 sources는 준 순서대로 쓴다.
// 오류: self·owner가 null이거나 스펙 규칙(AS 7.1절 15 — 소유 기록: importerId·importerName 있음,
// 원본 1–256개·role 유일, 출력 1–1024개·outputKey 유일·asset 유일·null 아님·type null 아님·self
// 포함, 문자열 상한·UTF-8·NUL 없음, relativePath 형태(IsValidImportSourceLocation), root에 `/`·`\`
// 없음, options는 Record envelope. 참조 기록: importerId null·이름 빈 값·버전 0·원본·옵션·출력
// 없음)을 어기면 InvalidArgument, 전체가 Record 기본 한도를 넘으면 CapacityExceeded, 할당 실패.
Result<Array<uint8>> WriteImportRecord(Allocator& allocator, AssetId self,
                                       const ImportRecordInput& input) noexcept;

// self 패키지의 ImportRecord byte를 읽는다. 오류: self가 null이거나 bytes가 비었으면
// InvalidArgument(섹션이 없으면 부르지 않는다), 스키마 Guid가 ImportRecordSchema가 아니거나 Record·
// 항목 버전이 ImportRecordVersion 밖이면 Unsupported(다른 스키마·더 새 기록 — 색인은
// UnsupportedVersion), 형식·순서·크기·규칙 위반(WriteImportRecord와 같은 규칙, 모르는 필드·빈 목록
// 필드·wire가 다른 필드 포함)은 InvalidArgument, 할당 실패.
Result<ImportRecord> ReadImportRecord(Allocator& allocator, AssetId self,
                                      Span<const uint8> bytes) noexcept;

// self 패키지의 ImportRecord와 Dependencies의 ImportOwner 항목을 대조한다(AS 7.1절 7·15). record가
// null이면 ImportRecord 섹션이 없다는 뜻이다. 참조 기록이면 ImportOwner 항목이 정확히 하나이고 그
// asset이 owner여야 하며, 소유 기록이거나 기록이 없으면 ImportOwner 항목이 없어야 한다. 다른
// 스키마·더 새 기록(ReadImportRecord가 Unsupported)은 참조 기록인지 알 수 없으므로 부르지 않는다.
// 오류: 위반은 InvalidArgument(호출자는 CorruptPackage — 칸 Dependencies로 보고한다).
Result<void> CheckImportOwner(AssetId self, const ImportRecord* record,
                              Span<const PackageDependencyEntry> dependencies) noexcept;

} // namespace rocks
