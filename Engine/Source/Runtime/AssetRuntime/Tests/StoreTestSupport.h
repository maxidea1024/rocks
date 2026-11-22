#pragma once

#include "AssetFixtureValues.h"
#include "PackageEditing.h"

#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <string.h>

// 저장 기반 시험 공용 도구다(Docs/Design/AssetSystem.ko.md 7.1절 15 — A2c1): bulk 셋을 가진 에디터
// 장면(Zstd·None), 경로마다 byte를 가진 메모리 파일 시스템(열기·크기·이동·읽기·쓰기 고장 주입, 내용
// 바꾸기 — 외부 변경 흉내), n번째 범위 읽기를 실패시키는 읽기.
namespace rocks::asset_test {

// bulk 셋(pixels 압축 잘 됨 3000 byte, source 압축 안 됨 64 byte, Part data 64 byte)을 가진
// 에디터 패키지다. 모르는 섹션을 더한 모양도 만든다.
struct StoreScene {
    explicit StoreScene(Allocator& allocator) noexcept
        : pixels(allocator), data(allocator), zstd(allocator), raw(allocator), unknown(allocator) {
    }

    RootRef<Package> package;
    ObjectHandle asset{};
    Array<uint8> pixels;
    Array<uint8> data;
    TestBulkSource bulk;
    Array<uint8> zstd;
    Array<uint8> raw;
    Array<uint8> unknown;
};

inline bool MakeStoreScene(AssetEnv& env, StoreScene& scene, uint64 id) noexcept {
    ObjectRegistry& registry = *env.source;
    auto made = NewPackage(registry, registry.OwnerAccess(), FixedId(id), PackageKind::Editor);
    if (!made) {
        return false;
    }

    scene.package = Move(made.Value());
    scene.asset = Make<TestAsset>(registry, TestAssetType(), scene.package.Handle(), "");
    const ObjectHandle part = Make<TestPart>(registry, TestPartType(), scene.asset, "Part");
    TestAsset* pointer = Pointer<TestAsset>(registry, scene.asset);
    if (part.IsNull() || pointer == nullptr) {
        return false;
    }

    FillCompressible(scene.pixels, 3000, static_cast<uint32>(id));
    FillIncompressible(scene.data, 64, static_cast<uint32>(id));
    pointer->pixels = scene.bulk.Add(1, scene.pixels.View());
    pointer->source = scene.bulk.Add(2, scene.data.View());
    Pointer<TestPart>(registry, part)->data = scene.bulk.Add(3, scene.data.View());
    PackageWriteOptions options = EditorOptions();
    options.bulkSource = &scene.bulk;
    options.bulkCodec = BulkCodec::Zstd;
    scene.zstd = Write(env.allocator, registry, scene.package.Handle(), options);
    options.bulkCodec = BulkCodec::None;
    scene.raw = Write(env.allocator, registry, scene.package.Handle(), options);
    EditablePackage edit(env.allocator);
    if (scene.zstd.IsEmpty() || scene.raw.IsEmpty() || !edit.Parse(scene.raw.View())) {
        return false;
    }

    EditableSection extra(env.allocator);
    extra.kind = 200;
    const uint8 payload[] = {9, 8, 7};
    if (!extra.bytes.TryAppend(Span<const uint8>(payload)) ||
        !edit.sections.TryPushBack(Move(extra))) {
        return false;
    }

    scene.unknown = edit.Build();
    return !scene.unknown.IsEmpty();
}

// 메모리 파일 시스템의 고장이다.
enum class FileFault {
    None,
    Open,
    Size,
    Seek,
    Read,
    Write,
    Flush
};

inline constexpr platform::FileStatus FakeFileError{platform::FileCode::SystemError, 13};

// 경로마다 byte를 가진 파일 시스템이다. 열린 handle은 열 때의 byte를 복사해 갖는다(교체해도 그
// handle은 이전 내용을 본다 — POSIX 의미 rename과 같다). 쓰기로 연 handle은 닫을 때 경로에
// 게시한다.
class MemoryPackageFiles final : public platform::FileSystem {
public:
    explicit MemoryPackageFiles(Allocator& allocator) noexcept
        : allocator_(&allocator), paths_(allocator), contents_(allocator), handles_(allocator) {
    }

    ~MemoryPackageFiles() override = default;

    // path의 내용을 정한다(없으면 만든다).
    bool Set(StringView path, Span<const uint8> bytes) noexcept {
        for (usize index = 0; index < paths_.Size(); ++index) {
            if (paths_[index].View() == path) {
                contents_[index].Clear();
                return static_cast<bool>(contents_[index].TryAppend(bytes));
            }
        }

        String name(*allocator_);
        Array<uint8> content(*allocator_);
        return name.TryAssign(path) && content.TryAppend(bytes) &&
               paths_.TryPushBack(rocks::Move(name)) && contents_.TryPushBack(rocks::Move(content));
    }

    const Array<uint8>* Get(StringView path) const noexcept {
        for (usize index = 0; index < paths_.Size(); ++index) {
            if (paths_[index].View() == path) {
                return &contents_[index];
            }
        }

        return nullptr;
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        ++opens;
        if (fault == FileFault::Open) {
            return FakeFileError;
        }

        const StringView name(path, pathBytes);
        const Array<uint8>* existing = Get(name);
        const bool writing = mode != platform::FileOpenMode::ReadExisting;
        if (existing == nullptr && !writing) {
            return platform::FileStatus{platform::FileCode::NotFound, 2};
        }

        auto handle = UniquePtr<Handle>::TryMake(*allocator_, *this, *allocator_);
        if (!handle || !handle.Value()->path.TryAssign(name) ||
            (existing != nullptr && !writing &&
             !handle.Value()->bytes.TryAppend(existing->View()))) {
            return platform::FileStatus{platform::FileCode::OutOfMemory, 0};
        }

        handle.Value()->writing = writing;
        out = handle.Value().Get();
        if (!handles_.TryPushBack(rocks::Move(handle.Value()))) {
            return platform::FileStatus{platform::FileCode::OutOfMemory, 0};
        }

        return {};
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        exists = Get(StringView(path, pathBytes)) != nullptr;
        return {};
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        for (usize index = 0; index < paths_.Size(); ++index) {
            if (paths_[index].View() == StringView(path, pathBytes)) {
                (void)paths_.TryRemoveAt(index);
                (void)contents_.TryRemoveAt(index);
                return {};
            }
        }

        return platform::FileStatus{platform::FileCode::NotFound, 2};
    }

    FileFault fault = FileFault::None;
    uint32 opens = 0;
    // 몇 번째 이동부터 실패시킬지다(0이면 없음).
    uint32 failSeekAt = 0;
    uint32 seeks = 0;
    // 몇 번째 읽기부터 실패시킬지다(0이면 없음 — fault Read와 별개).
    uint32 failReadAt = 0;
    uint32 reads = 0;

private:
    class Handle final : public platform::FileHandle {
    public:
        Handle(MemoryPackageFiles& owner, Allocator& allocator) noexcept
            : owner_(owner), path(allocator), bytes(allocator) {
        }

        platform::FileTransfer Read(void* destination, usize count) noexcept override {
            ++owner_.reads;
            if (owner_.fault == FileFault::Read ||
                (owner_.failReadAt != 0 && owner_.reads >= owner_.failReadAt)) {
                return {FakeFileError, 0};
            }

            const usize left = bytes.Size() - static_cast<usize>(position);
            const usize moved = count < left ? count : left;
            if (moved != 0) {
                memcpy(destination, bytes.Data() + position, moved);
            }

            position += moved;
            return {platform::FileStatus{}, moved};
        }

        platform::FileTransfer Write(const void* source, usize count) noexcept override {
            if (owner_.fault == FileFault::Write) {
                return {FakeFileError, 0};
            }

            const usize end = static_cast<usize>(position) + count;
            if (end > bytes.Size() && !bytes.TryResize(end)) {
                return {platform::FileStatus{platform::FileCode::OutOfMemory, 0}, 0};
            }

            memcpy(bytes.Data() + position, source, count);
            position += count;
            return {platform::FileStatus{}, count};
        }

        platform::FileStatus Seek(int64 offset, platform::FileSeekOrigin,
                                  uint64& now) noexcept override {
            ++owner_.seeks;
            if (owner_.fault == FileFault::Seek ||
                (owner_.failSeekAt != 0 && owner_.seeks >= owner_.failSeekAt)) {
                return FakeFileError;
            }

            position = static_cast<uint64>(offset);
            now = position;
            return {};
        }

        platform::FileStatus Size(uint64& size) noexcept override {
            if (owner_.fault == FileFault::Size) {
                return FakeFileError;
            }

            size = bytes.Size();
            return {};
        }

        platform::FileStatus Flush() noexcept override {
            return owner_.fault == FileFault::Flush ? FakeFileError : platform::FileStatus{};
        }

        platform::FileStatus Close() noexcept override {
            if (writing) {
                (void)owner_.Set(path.View(), bytes.View());
            }

            return {};
        }

        void Destroy() noexcept override {
            owner_.DestroyHandle(this);
        }

        MemoryPackageFiles& owner_;
        String path;
        Array<uint8> bytes;
        uint64 position = 0;
        bool writing = false;
    };

    void DestroyHandle(Handle* handle) noexcept {
        for (usize index = 0; index < handles_.Size(); ++index) {
            if (handles_[index].Get() == handle) {
                (void)handles_.TryRemoveAt(index);
                return;
            }
        }
    }

    Allocator* allocator_;
    Array<String> paths_;
    Array<Array<uint8>> contents_;
    Array<UniquePtr<Handle>> handles_;
};

// n번째 ReadAt부터 실패시키는 범위 읽기다.
class FailingPackageReader final : public PackageRangeReader {
public:
    FailingPackageReader(Span<const uint8> bytes, uint32 failAt) noexcept
        : inner_(bytes), failAt_(failAt) {
    }

    uint64 FileBytes() const noexcept override {
        return inner_.FileBytes();
    }

    Result<void> ReadAt(uint64 offset, Span<uint8> out) noexcept override {
        if (++reads_ == failAt_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        return inner_.ReadAt(offset, out);
    }

    uint32 Reads() const noexcept {
        return reads_;
    }

private:
    MemoryPackageRangeReader inner_;
    uint32 failAt_;
    uint32 reads_ = 0;
};

inline PackageReadOptions EditorReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    return options;
}

inline bool SameFailure(const AssetDiagnostic& a, const AssetDiagnostic& b) noexcept {
    return a.code == b.code && a.error == b.error && a.section == b.section &&
           a.detail == b.detail && a.asset == b.asset;
}

} // namespace rocks::asset_test
