#pragma once

#include <AssetRuntime/BulkStore.h>
#include <AssetRuntime/PackageReader.h>
#include <AssetRuntime/PackageWriter.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/String.h>
#include <Platform/File.h>

// 에디터 패키지의 파일 bulk 저장소와 파일 핸들 캐시다(Docs/Design/AssetSystem 7.1절 15 —
// A2c1). 추상과 bulkId 규칙은 BulkStore.h에 있다.
namespace rocks {

// 에디터 패키지 파일 핸들 캐시의 기본 크기다(열린 패키지 수와 무관한 상한 — 7.1절 15).
inline constexpr usize DefaultPackageFileCacheCapacity = 64;

// 에디터 패키지 파일 핸들 캐시다. 열린 패키지 수와 무관하게 핸들을 capacity개까지만 연다(가장
// 오래 쓰지 않은 것부터 닫는다 — macOS 기본 soft limit 256에서 EMFILE을 내지 않게). 파일을 열 때
// 헤더 + 섹션 표 지문을 읽어 둔다.
//
// 수명: 이 캐시를 쓰는 모든 저장소보다 오래 살아야 한다(저장소 소멸자가 자기 핸들을 닫는다).
//
// 스레드: 한 스레드(에디터 Registry 소유 스레드)에서 쓴다.
class ROCKS_ASSETRUNTIME_API PackageFileCache {
public:
    PackageFileCache(Allocator& allocator, platform::FileSystem& files,
                     usize capacity = DefaultPackageFileCacheCapacity) noexcept;
    ~PackageFileCache();
    PackageFileCache(const PackageFileCache&) = delete;
    PackageFileCache& operator=(const PackageFileCache&) = delete;

    // path의 범위 읽기를 얻는다. 열린 핸들이 없으면 열어 지문을 읽고, 지문이 expected와 다르면
    // ExternalModification / StaleHandle이다(그 핸들은 닫는다). 반환 포인터는 다음
    // Acquire·Close까지 유효하다. 오류: capacity 0 InvalidArgument, 열기·읽기 IO 오류는 진단
    // None과 그 오류, 헤더·표 손상은 ReadPackageHeader의 진단, 할당 실패.
    Result<PackageRangeReader*> Acquire(StringView path, const PackageFingerprint& expected,
                                        AssetDiagnostic* failure = nullptr) noexcept;

    // path의 핸들이 있으면 닫는다(저장 교체·Detach 전).
    void Close(StringView path) noexcept;

    // directory(구분자 '/', 끝 '/' 없음) 아래 모든 핸들을 닫는다(디렉터리 이동·삭제 전 — A2e).
    void CloseUnder(StringView directory) noexcept;

    usize OpenCount() const noexcept {
        return entries_.Size();
    }

    platform::FileSystem& Files() noexcept {
        return *files_;
    }

private:
    struct Entry;

    Allocator* allocator_;
    platform::FileSystem* files_;
    usize capacity_;
    uint64 clock_ = 0;
    Array<UniquePtr<Entry>> entries_;
};

// 저장소를 새 파일에 다시 잇는 준비다(FilePackageBulkStore::PrepareRebind가 채우고 CommitRebind가
// 쓴다 — 저장이 교체 전에 준비해 교체 뒤에는 실패하지 않게, 7.1절 15 A2c2b). 이동만 된다. 준비와
// 확정 사이에 그 저장소를 바꾸면(Attach·AddUnsaved·CopyFrom·Rebind·Detach) 안 된다 — 확정은 할당
// 없이 준비한 표로 바꾸므로 그 사이 더한 id를 담을 수 없다(확정이 저장소 변경 세대로 단언한다).
class BulkRebindPlan {
public:
    explicit BulkRebindPlan(Allocator& allocator) noexcept
        : table_(allocator), path_(allocator), updates_(allocator), kept_(allocator) {
    }

    BulkRebindPlan(BulkRebindPlan&&) noexcept = default;
    BulkRebindPlan& operator=(BulkRebindPlan&&) noexcept = default;
    BulkRebindPlan(const BulkRebindPlan&) = delete;
    BulkRebindPlan& operator=(const BulkRebindPlan&) = delete;

    // 준비를 마쳐 확정할 수 있는지다.
    bool IsReady() const noexcept {
        return ready_;
    }

private:
    friend class FilePackageBulkStore;

    // slot 하나의 새 파일 entry다.
    struct Update {
        usize slot = 0;
        PackageBulkEntry entry{};
        uint32 fileId = 0;
        uint64 fileOffset = 0;
    };

    HashMap<uint32, usize> table_;
    String path_;
    PackageFingerprint fingerprint_{};
    Array<Update> updates_;
    // 새 표에 남는 slot인지다(준비 때의 slot 수).
    Array<bool> kept_;
    // 준비 때 저장소의 변경 세대다.
    uint64 revision_ = 0;
    bool ready_ = false;
};

// 에디터 패키지의 파일 bulk 저장소다(7.1절 15). 파일 핸들을 쥐지 않고 경로·로드 지문·entry 표를
// 쥐며, entry를 처음 읽을 때 캐시로 파일을 열어(지문 대조) 저장 byte를 읽고 crc32·압축 해제·
// SHA-256을 확인한 raw byte를 보관한다. 저장하지 않은 bulk(AddUnsaved)는 메모리에만 있다.
//
// 저장(5.5절 7): writer의 bulk source로 쓴다(ReadBulk). 성공 뒤 Rebind로 새 파일 entry에 다시
// 잇는다(객체의 BulkDataRef는 그대로).
//
// 스레드: 한 스레드에서 쓴다.
class ROCKS_ASSETRUNTIME_API FilePackageBulkStore final : public PackageBulkStore,
                                                          public PackageBulkSource {
public:
    // cache는 이 저장소보다 오래 살아야 한다(소멸할 때 자기 경로의 캐시 핸들을 닫는다).
    FilePackageBulkStore(Allocator& allocator, PackageFileCache& cache) noexcept;
    ~FilePackageBulkStore() override;
    FilePackageBulkStore(const FilePackageBulkStore&) = delete;
    FilePackageBulkStore& operator=(const FilePackageBulkStore&) = delete;

    // 패키지 파일에 잇는다(로드 직후 — id 표는 항등). asset은 진단에 쓰는 패키지 id, entries는 그
    // 파일의 BulkIndex, bulkSectionOffset은 Bulk 섹션의 파일 offset이다. 오류: 이미 이었거나 표가
    // 비지 않은 저장소 InvalidArgument, 빈 path InvalidArgument, 할당 실패.
    Result<void> Attach(StringView path, const PackageFingerprint& fingerprint, AssetId asset,
                        uint64 bulkSectionOffset, Span<const PackageBulkEntry> entries) noexcept;

    Result<Span<const uint8>> Read(const BulkDataRef& ref,
                                   AssetDiagnostic* failure = nullptr) noexcept override;
    Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept override;

    // 저장하지 않은 bulk를 더하고 그 ref(최상위 bit id)를 준다. SHA-256을 이때 계산한다. 오류:
    // id 소진 CapacityExceeded, 할당 실패(raw는 실패해도 소비된다).
    Result<BulkDataRef> AddUnsaved(Array<uint8>&& raw) noexcept;

    // 다른 패키지 저장소의 bulk를 이 저장소의 저장하지 않은 bulk로 복사한다(패키지 사이 bulk 이동의
    // 유일한 길 — 7.1절 15 (b)). 오류: source.Read의 오류, AddUnsaved의 오류.
    Result<BulkDataRef> CopyFrom(PackageBulkStore& source, const BulkDataRef& ref,
                                 AssetDiagnostic* failure = nullptr) noexcept;

    // 저장 성공 뒤 새 파일에 다시 잇는다: remap(옛 id → 새 파일 id)의 옛 id는 새 파일 entry를
    // 가리키고, remap에 없는 id는 표에서 빠진다(이미 푼 raw는 이 저장소 수명까지 둔다 — span 유효).
    // 이전 경로의 캐시 핸들을 닫는다. 오류: remap의 옛 id가 표에 없거나 새 id가 entries에 없거나
    // rawBytes·sha256이 다르면 InvalidArgument(바꾸지 않는다), 할당 실패(바꾸지 않는다).
    Result<void> Rebind(StringView path, const PackageFingerprint& fingerprint,
                        uint64 bulkSectionOffset, Span<const PackageBulkEntry> entries,
                        Span<const BulkRemapEntry> remap) noexcept;

    // Rebind를 두 단계로 한다: PrepareRebind는 같은 검사와 새 표·경로 할당을 모두 plan에 하고
    // 저장소를 바꾸지 않는다(오류는 Rebind와 같다 — plan은 준비되지 않은 채 남는다). CommitRebind는
    // 준비한 plan을 할당 없이 적용하고(실패하지 않는다) plan을 비운다. 준비되지 않은 plan, 준비 뒤
    // 바뀐 저장소의 확정은 계약 위반이다(단언).
    Result<void> PrepareRebind(BulkRebindPlan& plan, StringView path,
                               const PackageFingerprint& fingerprint, uint64 bulkSectionOffset,
                               Span<const PackageBulkEntry> entries,
                               Span<const BulkRemapEntry> remap) const noexcept;
    void CommitRebind(BulkRebindPlan& plan) noexcept;

    // 아직 읽지 않은 entry를 모두 읽어 메모리로 두고 파일과의 연결(경로·캐시 핸들)을 끊는다(이동·
    // 삭제 전 — 7.1절 6, UE ResetLoaders). 오류: Read의 오류(그때까지 읽은 것은 남고 연결은
    // 그대로).
    Result<void> Detach(AssetDiagnostic* failure = nullptr) noexcept;

    // ref가 가리키는 현재 파일 entry의 id다(메모리에만 있으면 0 — 진단용). 오류: 없는 id NotFound.
    Result<uint32> FileBulkId(const BulkDataRef& ref) const noexcept;

    // 파일에 이어져 있는지다(Detach 뒤 거짓).
    bool IsAttached() const noexcept {
        return !path_.IsEmpty();
    }

    // 표의 id 수다.
    usize Count() const noexcept {
        return table_.Count();
    }

private:
    struct Slot;

    Result<Slot*> FindSlot(const BulkDataRef& ref) noexcept;
    Result<void> Load(Slot& slot, AssetDiagnostic* failure) noexcept;

    Allocator* allocator_;
    PackageFileCache* cache_;
    String path_;
    PackageFingerprint fingerprint_{};
    AssetId asset_{};
    Array<UniquePtr<Slot>> slots_;
    // 메모리 id → slots_ index.
    HashMap<uint32, usize> table_;
    uint32 nextUnsaved_ = 1;
    // 표·연결을 바꿀 때마다 오르는 세대다(BulkRebindPlan 대조).
    uint64 revision_ = 0;
};

} // namespace rocks
