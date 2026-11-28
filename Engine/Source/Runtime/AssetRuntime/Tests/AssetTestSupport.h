#pragma once

#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageLoad.h>
#include <AssetRuntime/PackageWriter.h>
#include <AssetTestFixtures/TestAssets.generated.h>
#include <AssetTestFixtures/TestAssets.h>
#include <Base/Endian.h>
#include <Core/Encoding/Crc32.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Object/ConstructionContext.h>
#include <Object/ModuleRegistration.h>
#include <TestSupport/Test.h>

// AssetRuntime 시험 공용 환경이다: 타입 등록(AssetRuntime + AssetTestFixtures)과 패키지 정책
// Registry, 객체 만들기·쓰기·읽기 도우미.
namespace rocks::asset_test {

using asset_fixture::TestAsset;
using asset_fixture::TestPart;
using asset_fixture::TestRequiredAsset;
using asset_fixture::TestScratch;

inline TypeId TestAssetType() noexcept {
    return GeneratedType<TestAsset>::Id();
}

inline TypeId TestPartType() noexcept {
    return GeneratedType<TestPart>::Id();
}

inline TypeId TestScratchType() noexcept {
    return GeneratedType<TestScratch>::Id();
}

inline TypeId TestRequiredType() noexcept {
    return GeneratedType<TestRequiredAsset>::Id();
}

// 시험에서 고정한 에셋 id다(golden이 실행마다 같도록).
inline AssetId FixedId(uint64 low) noexcept {
    return AssetId{Guid{0x5e570000a55e7000ull, low}};
}

// 에디터 패키지 시험에 쓰는 고정 엔진 버전이다(AS 5.3절 — golden이 버전 올림에 흔들리지 않게).
inline constexpr EngineVersion TestEngineVersion{1, 2, 3};

// AssetRuntime과 fixture 모듈(AssetTestFixtures)의 타입을 modules에 등록한다(AssetEnv와 따로 만든
// TypeRegistry가 쓴다 — 정의는 AssetTestSupport.cpp, 모듈 생성 헤더를 거기서만 include한다).
Result<void> RegisterAssetTestTypes(ModuleRegistration& modules) noexcept;

// 패키지 정책 에셋 Registry 둘(쓰기·읽기)과 타입 Registry를 가진 환경이다. 수명 순서: Registry →
// ModuleRegistration → TypeRegistry(멤버 선언 역순 소멸).
struct AssetEnv {
    // AssetRuntime과 fixture 모듈(AssetTestFixtures)의 타입을 등록하고 Registry 둘을 만든다. 등록
    // 함수는 모듈 생성 헤더에 선언되어 있고 손으로 쓴 헤더는 그 헤더를 include하지 않으므로 정의는
    // AssetTestSupport.cpp에 있다.
    AssetEnv() noexcept;

    ~AssetEnv() {
        source.Reset();
        target.Reset();
        CHECK(modules.Unregister().IsOk());
    }

    bool Ready() const noexcept {
        return registered && source.Get() != nullptr && target.Get() != nullptr;
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    // Name 필드의 토큰 pool이다(두 Registry가 같이 쓴다 — 로드한 Name이 쓴 Name과 같은 토큰이다).
    NamePool names;
    bool registered = false;
    UniquePtr<ObjectRegistry> source;
    UniquePtr<ObjectRegistry> target;
};

template <class T>
T* Pointer(ObjectRegistry& registry, ObjectHandle handle) noexcept {
    auto borrow = registry.Resolve<T>(registry.OwnerAccess(), handle);
    if (!borrow) {
        return nullptr;
    }

    auto pointer = borrow.Value().Get();
    return pointer ? pointer.Value() : nullptr;
}

// owner의 Borrow를 쥔 채 fn(owner 값)을 부른다(Registry 컨테이너 live 변경은 호출자가 owner
// Borrow를 쥐어야 한다).
template <class T, class Fn>
bool WithBorrow(ObjectRegistry& registry, ObjectHandle owner, Fn&& fn) noexcept {
    auto borrow = registry.Resolve<T>(registry.OwnerAccess(), owner);
    if (!borrow) {
        return false;
    }

    auto pointer = borrow.Value().Get();
    return pointer && fn(*pointer.Value());
}

// outer 아래 name으로 T를 만들고 handle을 돌려준다(패키지 정책이 outer → inner로 붙잡는다).
template <class T>
ObjectHandle Make(ObjectRegistry& registry, TypeId type, ObjectHandle outer,
                  StringView name) noexcept {
    auto made = registry.NewObject<T>(registry.OwnerAccess(), type, outer, name);
    return made ? made.Value().Handle() : ObjectHandle{};
}

// 시험 bulk 원본이다(옛 bulkId → byte).
struct TestBulkSource final : PackageBulkSource {
    struct Entry {
        BulkDataRef ref{};
        Span<const uint8> bytes{};
        PackageBulkPlacement placement = PackageBulkPlacement::Package;
    };

    Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept override {
        for (usize index = 0; index < count; ++index) {
            if (entries[index].ref.bulkId == ref.bulkId) {
                return Result<Span<const uint8>>::Success(entries[index].bytes);
            }
        }

        return Result<Span<const uint8>>::Failure(ErrorCode::NotFound);
    }

    PackageBulkPlacement Placement(const BulkDataRef& ref) const noexcept override {
        for (usize index = 0; index < count; ++index) {
            if (entries[index].ref.bulkId == ref.bulkId) {
                return entries[index].placement;
            }
        }

        return PackageBulkPlacement::Package;
    }

    // bytes를 id로 등록하고 그 ref를 돌려준다(placement는 쓰기의 배치 — AS 20.1 6).
    BulkDataRef Add(uint32 id, Span<const uint8> bytes,
                    PackageBulkPlacement placement = PackageBulkPlacement::Package) noexcept {
        BulkDataRef ref;
        ref.bulkId = id;
        ref.rawBytes = bytes.Size();
        ref.sha256 = Sha256::Hash(bytes);
        entries[count++] = Entry{ref, bytes, placement};
        return ref;
    }

    Entry entries[8];
    usize count = 0;
};

// 로드한 패키지의 bulk를 원본으로 쓰는 source다(다시 쓰기 시험).
struct PackageBulkReader final : PackageBulkSource {
    PackageBulkReader(ObjectRegistry& registry, ObjectHandle package) noexcept
        : registry_(registry), package_(package) {
    }

    Result<Span<const uint8>> ReadBulk(const BulkDataRef& ref) noexcept override {
        Package* package = Pointer<Package>(registry_, package_);
        if (package == nullptr) {
            return Result<Span<const uint8>>::Failure(ErrorCode::StaleHandle);
        }

        return package->BulkBytes(ref);
    }

private:
    ObjectRegistry& registry_;
    ObjectHandle package_;
};

// AssetId → 생존 패키지 handle 표다.
struct TestResolver final : PackageImportResolver {
    Result<ObjectHandle> FindPackage(AssetId asset) noexcept override {
        for (usize index = 0; index < count; ++index) {
            if (ids[index] == asset) {
                return Result<ObjectHandle>::Success(handles[index]);
            }
        }

        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    void Add(AssetId id, ObjectHandle handle) noexcept {
        ids[count] = id;
        handles[count] = handle;
        ++count;
    }

    AssetId ids[8];
    ObjectHandle handles[8];
    usize count = 0;
};

inline PackageWriteOptions CookedOptions() noexcept {
    PackageWriteOptions options;
    options.kind = PackageKind::Cooked;
    options.profileId = "Windows";
    options.cookerVersion = 7;
    options.profileSettings =
        Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>("windows-profile"), 15));
    return options;
}

inline PackageWriteOptions EditorOptions() noexcept {
    PackageWriteOptions options;
    options.kind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

inline PackageLoadOptions LoadOptions(PackageKind kind) noexcept {
    PackageLoadOptions options;
    options.read.expectedKind = kind;
    options.read.engineVersion = TestEngineVersion;
    return options;
}

// registry의 package를 bytes로 쓴다(실패하면 빈 배열).
inline Array<uint8> Write(Allocator& allocator, ObjectRegistry& registry, ObjectHandle package,
                          const PackageWriteOptions& options,
                          AssetDiagnostic* failure = nullptr) noexcept {
    MemoryPackageSink sink(allocator);
    auto written =
        WritePackage(allocator, registry, registry.OwnerAccess(), package, options, sink, failure);
    if (!written) {
        return Array<uint8>(allocator);
    }

    return sink.Take();
}

// registry에 로드한 package를 읽은 파일이 기록한 설정(종류·정렬·Zstd 여부·커스텀 버전, 에디터는
// 저장 엔진 버전·Build·ImportOwner 의존·ImportRecord·Tags, 쿠킹은 프로파일·쿠커 버전)으로 다시
// 쓴다. bulk는 로드한 Package에서 읽는다. contents는 그 파일의 ReadPackage 결과다(실패하면 빈
// 배열).
inline Array<uint8> ResaveAsRead(Allocator& allocator, ObjectRegistry& registry,
                                 ObjectHandle package, const PackageContents& contents) noexcept {
    PackageBulkReader bulk(registry, package);
    PackageWriteOptions options;
    options.kind = contents.header.kind;
    options.bulkAlignment = contents.header.bulkAlignment;
    options.bulkSource = &bulk;
    for (const PackageBulkEntry& entry : contents.bulkEntries) {
        if (entry.codec == BulkCodec::Zstd) {
            options.bulkCodec = BulkCodec::Zstd;
        }
    }

    options.customVersions = contents.summary.customVersions.View();
    Array<PackageDependencyEntry> extra(allocator);
    if (contents.header.kind == PackageKind::Editor) {
        options.engineVersion = contents.summary.savedBy;
        for (const PackageDependencyEntry& entry : contents.dependencies) {
            if (entry.kind == PackageDependencyKind::Build ||
                entry.kind == PackageDependencyKind::ImportOwner) {
                (void)extra.TryPushBack(entry);
            }
        }

        options.extraDependencies = extra.View();
        options.importRecord = contents.importRecord;
        options.tags = contents.tags;
    } else {
        options.profileId = contents.summary.profileId.View();
        options.profileSettings = contents.summary.profileSettings;
        options.cookerVersion = contents.summary.cookerVersion;
    }

    return Write(allocator, registry, package, options);
}

// bytes 패키지들을 registry에 한 트랜잭션으로 로드하고 Commit 뒤 이벤트 바인딩을 만든다.
// roots·handles는 packages 수만큼이다.
inline Result<void> LoadAll(AssetEnv& env, ObjectRegistry& registry, Span<Array<uint8>> files,
                            PackageKind kind, PackageImportResolver* resolver,
                            Span<RootRef<Object>> roots, Span<ObjectHandle> handles,
                            AssetDiagnostic* failure = nullptr) noexcept {
    const PackageLoadOptions options = LoadOptions(kind);
    UniquePtr<PackageLoadState> states[4];
    PackageLoadState* pointers[4] = {};
    for (usize index = 0; index < files.Size(); ++index) {
        auto made = UniquePtr<PackageLoadState>::TryMake(
            env.allocator, env.allocator, registry, env.types, registry.OwnerAccess(), options);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        states[index] = Move(made.Value());
        pointers[index] = states[index].Get();
        if (auto prepared = states[index]->Prepare(Move(files[index]), failure); !prepared) {
            return prepared;
        }
    }

    ConstructionContext transaction(registry, registry.OwnerAccess());
    if (auto made =
            InstantiatePackages(transaction, Span<PackageLoadState* const>(pointers, files.Size()),
                                resolver, handles, failure);
        !made) {
        return made;
    }

    if (auto committed = transaction.Commit(handles, roots); !committed) {
        return committed;
    }

    // 게시 경로처럼 패키지마다 이벤트 바인딩을 다시 만든다(EventReflection ED21).
    for (usize index = 0; index < files.Size(); ++index) {
        if (auto bound = pointers[index]->BindEvents(); !bound) {
            return Result<void>::Failure(bound.Error());
        }
    }

    return Result<void>::Success();
}

// 섹션 표에서 kind 항목의 위치(헤더 기준 offset)와 크기를 찾는다.
inline bool FindSection(Span<const uint8> file, PackageSectionKind kind, usize& row, uint64& offset,
                        uint64& bytes) noexcept {
    const uint64 table = LoadLittleEndian<uint64>(file.Data() + 16);
    const uint32 count = LoadLittleEndian<uint32>(file.Data() + 12);
    for (uint32 index = 0; index < count; ++index) {
        const usize at = static_cast<usize>(table) + PackageSectionEntryBytes * index;
        if (LoadLittleEndian<uint32>(file.Data() + at) == static_cast<uint32>(kind)) {
            row = at;
            offset = LoadLittleEndian<uint64>(file.Data() + at + 8);
            bytes = LoadLittleEndian<uint64>(file.Data() + at + 16);
            return true;
        }
    }

    return false;
}

// 헤더 CRC를 다시 계산한다.
inline void FixHeaderCrc(Array<uint8>& file) noexcept {
    StoreLittleEndian<uint32>(file.Data() + 60, Crc32(Span<const uint8>(file.Data(), 60)));
}

// 섹션 표 CRC를 다시 계산한다.
inline void FixTableCrc(Array<uint8>& file) noexcept {
    const usize table = static_cast<usize>(LoadLittleEndian<uint64>(file.Data() + 16));
    const usize bytes = PackageSectionEntryBytes * LoadLittleEndian<uint32>(file.Data() + 12);
    StoreLittleEndian<uint32>(file.Data() + table + bytes,
                              Crc32(Span<const uint8>(file.Data() + table, bytes)));
}

// 헤더·섹션 표·비-Bulk 섹션 CRC를 모두 다시 맞춘다(구조가 망가져 위치를 읽을 수 없으면 그대로
// 둔다).
inline void FixAllCrcs(Array<uint8>& file) noexcept {
    if (file.Size() < PackageHeaderBytes) {
        return;
    }

    const uint64 table = LoadLittleEndian<uint64>(file.Data() + 16);
    const uint32 count = LoadLittleEndian<uint32>(file.Data() + 12);
    if (count <= PackageMaxSections && table <= file.Size() &&
        uint64{PackageSectionEntryBytes} * count + 4 <= file.Size() - table) {
        for (uint32 index = 0; index < count; ++index) {
            uint8* row = file.Data() + table + PackageSectionEntryBytes * index;
            const uint64 offset = LoadLittleEndian<uint64>(row + 8);
            const uint64 bytes = LoadLittleEndian<uint64>(row + 16);
            const uint32 flags = LoadLittleEndian<uint32>(row + 4);
            if ((flags & static_cast<uint32>(PackageSectionFlags::PerEntryChecksum)) == 0 &&
                offset <= file.Size() && bytes <= file.Size() - offset) {
                StoreLittleEndian<uint32>(
                    row + 24,
                    Crc32(Span<const uint8>(file.Data() + offset, static_cast<usize>(bytes))));
            }
        }

        FixTableCrc(file);
    }

    FixHeaderCrc(file);
}

// kind 섹션의 CRC 칸과 표 CRC를 다시 계산한다(내용을 고친 뒤 — 손상이 CRC가 아닌 내용 검증에
// 걸리게).
inline void FixSectionCrc(Array<uint8>& file, PackageSectionKind kind) noexcept {
    usize row = 0;
    uint64 offset = 0;
    uint64 bytes = 0;
    if (FindSection(file.View(), kind, row, offset, bytes)) {
        StoreLittleEndian<uint32>(
            file.Data() + row + 24,
            Crc32(Span<const uint8>(file.Data() + offset, static_cast<usize>(bytes))));
        FixTableCrc(file);
    }
}

inline Array<uint8> Copy(Allocator& allocator, Span<const uint8> bytes) noexcept {
    Array<uint8> copy(allocator);
    (void)copy.TryAppend(bytes);
    return copy;
}

inline bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::asset_test
