#pragma once

#include <AssetRuntime/AssetDiagnostic.h>
#include <AssetRuntime/BulkData.h>
#include <AssetRuntime/PackageReader.h>
#include <Core/Build/EngineVersion.h>
#include <Object/ObjectRegistry.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/File.h>

namespace rocks {

// 패키지 byte를 받는 출력이다. writer는 섹션을 물리 순서대로 Append하고 마지막에 헤더를 offset 0에
// WriteAt으로 쓴다(AS 5.5절 3단계 — 헤더 자리는 처음에 0으로 채운다). 파일 출력(에디터 저장 — A2)과
// 메모리 출력(쿠킹·시험)이 같은 writer를 쓴다.
class PackageSink {
public:
    // bytes를 끝에 붙인다.
    virtual Result<void> Append(Span<const uint8> bytes) noexcept = 0;
    // 이미 쓴 범위 [offset, offset + bytes.Size())를 덮어쓴다(헤더 patch).
    virtual Result<void> WriteAt(uint64 offset, Span<const uint8> bytes) noexcept = 0;

protected:
    ~PackageSink() = default;
};

// 메모리 출력이다. 쓴 byte는 Bytes()·Take()로 얻는다.
//
// 사용 예:
//   MemoryPackageSink sink(allocator);
//   auto written = WritePackage(allocator, registry, access, package, options, sink);
//   Array<uint8> bytes = sink.Take();
class ROCKS_ASSETRUNTIME_API MemoryPackageSink final : public PackageSink {
public:
    explicit MemoryPackageSink(Allocator& allocator) noexcept : bytes_(allocator) {
    }

    Result<void> Append(Span<const uint8> bytes) noexcept override;
    Result<void> WriteAt(uint64 offset, Span<const uint8> bytes) noexcept override;

    Span<const uint8> Bytes() const noexcept {
        return bytes_.View();
    }

    Array<uint8> Take() noexcept {
        return Move(bytes_);
    }

private:
    Array<uint8> bytes_;
};

// 파일 출력이다(에디터 저장의 임시 파일 — AS 5.5절 2–4, 7.1절 15). Append는 이어 쓰고, WriteAt은
// 그 offset으로 옮겨 쓴 뒤 끝으로 돌아간다. 파일은 쓰기로 연 빈 파일이어야 한다(CreateNew). Flush·
// Close는 호출자가 한다.
//
// 오류: 파일 쓰기·이동 오류는 FileErrorCode 대응, 쓴 끝보다 먼 WriteAt은 InvalidArgument.
//
// 사용 예:
//   platform::File file;
//   if (!file.Open(files, temp, tempBytes, platform::FileOpenMode::CreateNew)) { ... }
//   FilePackageSink sink(file);
//   auto written = WritePackage(allocator, registry, access, package, options, sink);
class ROCKS_ASSETRUNTIME_API FilePackageSink final : public PackageSink {
public:
    explicit FilePackageSink(platform::File& file) noexcept : file_(file) {
    }

    Result<void> Append(Span<const uint8> bytes) noexcept override;
    Result<void> WriteAt(uint64 offset, Span<const uint8> bytes) noexcept override;

    // 지금까지 쓴 byte 수다.
    uint64 Written() const noexcept {
        return written_;
    }

private:
    platform::File& file_;
    uint64 written_ = 0;
};

// 이전 패키지 파일 entry의 저장 byte다(PackageBulkSource::ReadStoredBulk — 다시 압축하지 않고
// 복사한다, Docs/Design/AssetSystem 12.12절 저장 byte 재사용).
struct PackageStoredBulk {
    BulkCodec codec = BulkCodec::None;
    uint32 crc32 = 0;
    uint64 rawBytes = 0;
    Sha256Digest sha256{};
    Span<const uint8> stored{};
};

// bulk entry를 둘 곳이다(AS 20.1 1 — 쿠킹 패키지만 Sidecar를 고를 수 있다).
enum class PackageBulkPlacement : uint8 {
    // 패키지 Bulk 섹션(지금까지의 배치).
    Package,
    // 패키지 옆 sidecar 파일(스트리밍 bulk — 런타임 Prepare가 읽지 않고 범위 읽기로 읽는다).
    Sidecar
};

// writer가 bulk entry의 raw byte를 얻는 곳이다(AS 5.5절 — 객체가 메모리에 가진 raw 데이터이거나
// 이전 패키지 파일의 entry). ref는 저장 전(옛) 값이다. 반환 span은 다음 호출이나 writer 반환까지
// 유효해야 한다. writer가 크기·SHA-256을 ref와 대조한다(다르면 저장이 CorruptPackage로 실패 —
// 손상이 새 CRC로 세탁되지 않게).
class PackageBulkSource {
public:
    virtual Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept = 0;

    // ref가 이전 패키지 파일의 entry면 그 저장 byte와 표 값을 준다(12.12절). writer는 codec이 쓰기
    // codec과 같고 rawBytes·sha256이 ref와 같으면 crc32를 확인하고(다르면 CorruptPackage — 원래
    // CRC를 옮기므로 손상이 세탁되지 않는다) 그대로 복사한다. 그 밖(codec이 다르거나 이 함수가
    // 실패하면)은 ReadBulk로 raw를 얻어 부호화한다. 반환 span의 수명은 ReadBulk와 같다. 기본은
    // NotFound(메모리 raw만 있는 저장소). 오류: 파일 entry가 아님 NotFound, 읽기 오류.
    virtual Result<PackageStoredBulk> ReadStoredBulk(const BulkDataRef& ref) noexcept {
        (void)ref;
        return Result<PackageStoredBulk>::Failure(ErrorCode::NotFound);
    }

    // ref(저장 전 값)를 둘 곳이다(AS 20.1 6 — 기본 패키지 안). Sidecar는 쿠킹 쓰기에 sidecar 출력이
    // 있을 때만 받는다(아니면 저장이 InvalidArgument).
    virtual PackageBulkPlacement Placement(const BulkDataRef& ref) const noexcept {
        (void)ref;
        return PackageBulkPlacement::Package;
    }

protected:
    ~PackageBulkSource() = default;
};

// 저장 전 bulkId → 저장된 bulkId 재매핑 항목이다(새 id 순). 저장 뒤 호출자가 메모리의 BulkDataRef를
// 이 표로 바꾼다(AS 5.5절 7단계 — A2).
struct BulkRemapEntry {
    uint32 oldId = 0;
    uint32 newId = 0;
};

// 패키지 쓰기 옵션이다.
struct PackageWriteOptions {
    PackageKind kind = PackageKind::Cooked;
    // 0이면 종류의 기본값(에디터 64, 쿠킹 4096)이다. 2의 거듭제곱 8–65536이 아니면
    // InvalidArgument다.
    uint32 bulkAlignment = 0;
    BulkCodec bulkCodec = BulkCodec::None;
    // 에디터 패키지 Summary의 savedBy다(compatibleWith는 CompatibleBaseline). 호출자는
    // build::CurrentEngineVersion()을, 시험은 고정 값을 넘긴다(AS 5.3절 엔진 버전 축). 쿠킹
    // 패키지는 기록하지 않는다. 에디터 패키지에서 빈 값이면 InvalidArgument다.
    EngineVersion engineVersion{};
    // 쿠킹 패키지 Summary 값이다(프로파일 id 1–4096 byte, 프로파일 출력 설정 SHA-256, 쿠커 버전).
    StringView profileId{};
    Sha256Digest profileSettings{};
    uint32 cookerVersion = 0;
    // 커스텀 버전 표다(id 오름차순·중복 없음이 아니면 InvalidArgument).
    Span<const PackageCustomVersion> customVersions{};
    // 저장 결과로 만들 수 없는 Dependencies(Build·ImportOwner — 에디터 패키지만)다. 다른 종류나
    // 쿠킹 패키지에서 주면 InvalidArgument다.
    Span<const PackageDependencyEntry> extraDependencies{};
    // 에디터 패키지의 ImportRecord·Tags Record byte다(비면 섹션 생략). 쿠킹 패키지에서 주면
    // InvalidArgument다.
    Span<const uint8> importRecord{};
    Span<const uint8> tags{};
    // bulk raw byte를 얻는 곳이다. 저장 그래프에 빈 값이 아닌 BulkDataRef가 있는데 null이면
    // InvalidArgument다.
    PackageBulkSource* bulkSource = nullptr;
    // sidecar 파일 출력이다(AS 20.1 3 — 쿠킹 패키지만). bulkSource가 Sidecar를 고른 bulk가 있을
    // 때만 쓴다(없으면 아무것도 쓰지 않는다). Sidecar bulk가 있는데 null이거나 에디터 패키지면
    // InvalidArgument다. 빈 출력이어야 한다(머리부터 쓴다).
    PackageSink* sidecar = nullptr;
    // 복제다(AssetSystem 7.1절 17 A2e3b — 에디터 패키지만, null이면 복제가 아니다). 주면 헤더
    // id가 이것이고, DuplicateTransient·NonTryoutDuplicateTransient 필드를 쓰지 않으며(로드가
    // 클래스 기본값 — UE DuplicateObject), 패키지의 지금 id를 가리키는 Soft 참조 값을 이 id로
    // 바꾼다(자기 의존 제외도 이 id로 한다). 쿠킹 패키지에서 주거나 지금 id와 같으면
    // InvalidArgument다.
    AssetId duplicateId{};
    // 에디터 패키지의 override 집합 조회다(Docs/Design/RuntimeClassMetadata.ko.md 'A5b' — 기준과
    // 같아도 집합 단위 값은 쓴다, null이면 집합 없음). 패키지는 늘 차이 형식이다: 에디터는 Editor,
    // 쿠킹은 Cooked 기준 레코드와 같은 값을 쓰지 않는다. 쿠킹 패키지는 집합을 쓰지 않는다(무시한다
    // — 런타임은 같은 빌드 레코드로 읽는다).
    const GraphOverrides* overrides = nullptr;
};

// 쓰기 결과다.
struct PackageWriteResult {
    explicit PackageWriteResult(Allocator& allocator) noexcept : bulkRemap(allocator) {
    }

    // 쓴 파일 크기다.
    uint64 fileBytes = 0;
    // 헤더 + 섹션 표의 SHA-256이다(PackageContents::fingerprint와 같다).
    PackageFingerprint fingerprint{};
    // bulk 재매핑 표다(새 id 순).
    Array<BulkRemapEntry> bulkRemap;
    // 쓴 sidecar 파일의 크기·SHA-256이다(Sidecar bulk가 없으면 0 — 매니페스트 sidecar 칸).
    uint64 sidecarBytes = 0;
    Sha256Digest sidecarSha256{};
};

// package(패키지 정책 Registry의 Package)와 소속 객체를 패키지 포맷 v1로 sink에
// 쓴다(AS 5.1·5.2·5.5절 3단계). 같은 Registry 상태와 옵션이면 같은 byte다(결정적).
//
// 처리:
// 1. export 집합: CollectInners(DepthFirst)에서 저장 불가 객체(IsUnsaveableType)와 그 부분 트리를
// 뺀다.
//    export 1은 대표 에셋(Asset 파생, 빈 이름)이어야 한다.
// 2. Objects: SaveObjectGraphSet(Admit — 다른 패키지 객체는 import, weak 밖 대상은 null, 저장 불가
//    Exclude, Required 거부, 쿠킹은 EditorOnly 필드 제외). BulkDataRef는 1부터 다시 번호를 매긴다.
// 3. ObjectImports: 외부 참조 대상과 그 Outer 사슬(대표 에셋 아래)을 (assetId, 객체 경로) 순으로.
// 4. Dependencies: 외부 참조(Hard·EditorOnly)와 Soft 참조(Soft·EditorOnly), extraDependencies.
// 5. Summary·Bulk(+BulkIndex)·섹션 표·헤더.
//
// 오류: 에디터 데이터 없는 빌드(ROCKS_WITH_EDITOR_DATA=0)의 에디터 패키지 Unsupported(컴파일에서
// 빠진 EditorOnly 값을 잃는다 — Docs/Design/BuildTargets 3.4절 BD26, 에셋 도구는 WithEditorData
// Program이다), 인자·옵션 위반 InvalidArgument, 대표 에셋 없음 InvalidArgument, 패키지에 속하지
// 않은 객체 참조는 UnpackagedReference / InvalidArgument, bulk source의 byte가 ref와 다르면
// CorruptPackage / InvalidArgument, 그래프 저장의 오류(Required·외부 참조 거부 InvalidArgument,
// 한도 CapacityExceeded, 접근 WrongThread·WrongContext, 회수된 대상 StaleHandle·Destroyed)와 sink
// 오류는 그대로다. 실패하면 sink에 쓴 byte는 정의되지 않는다(호출자가 버린다 — 파일 저장은 임시
// 파일).
//
// 스레드: Registry 소유 스레드 전용이다(그래프 저장이 encode 고정을 쥔다).
ROCKS_ASSETRUNTIME_API Result<PackageWriteResult>
WritePackage(Allocator& allocator, ObjectRegistry& registry, const ObjectAccessContext& access,
             ObjectHandle package, const PackageWriteOptions& options, PackageSink& sink,
             AssetDiagnostic* failure = nullptr) noexcept;

} // namespace rocks
