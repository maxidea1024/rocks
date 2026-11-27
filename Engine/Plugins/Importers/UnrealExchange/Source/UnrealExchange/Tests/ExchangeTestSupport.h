#pragma once

#include <Core/Containers/Array.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <UnrealExchange/ExchangeBundle.h>
#include <UnrealExchange/ExchangeImportOrder.h>
#include <UnrealExchange/ExchangePaths.h>

// Unreal 교환 리더 시험 공용 도구다(Docs/Design/UnrealExchange 12.1절): 메모리 파일
// 시스템(변조· 실패 주입), 저장소 fixture
// 묶음(Engine/Plugins/Importers/UnrealExchange/Tests/Bundles) 복사, 합성 묶음 작성.

namespace rocks::exchange_test {

// 메모리 파일 시스템 안 묶음 디렉터리 이름이다.
inline constexpr StringView FixtureDirectory = "Basic.rxbundle";
inline constexpr StringView SyntheticDirectory = "Synthetic.rxbundle";

// 메모리 파일에 주입하는 실패다.
enum class FileFault : uint8 {
    None,
    Open,     // 열기 실패
    Size,     // 크기 조회 실패
    Read,     // 읽기 실패
    Short,    // 크기를 실제보다 1 크게 알림(읽는 중 파일이 줄어든 경우)
    HugeSize, // 크기를 UnrealExchangeMaxJsonBytes + 1로 알림(내용 없이 상한 시험)
    ReadLate, // 첫 읽기는 성공하고 그다음 읽기부터 실패(조각 경계 뒤의 읽기 실패)
    Stat      // 정보 조회(Stat) 실패
};

inline constexpr platform::FileStatus FaultStatus{platform::FileCode::SystemError, 0};

// 메모리 파일 하나를 읽는 handle이다.
class MemoryHandle final : public platform::FileHandle {
public:
    platform::FileTransfer Read(void* destination, usize bytes) noexcept override {
        const bool late = fault == FileFault::ReadLate && reads != 0;
        ++reads;
        if (fault == FileFault::Read || late) {
            return {FaultStatus, 0};
        }

        const usize available = data->Size() - position;
        const usize count = bytes < available ? bytes : available;
        if (count != 0) {
            platform::MemoryCopy(destination, data->Data() + position, count);
        }

        position += count;
        return {{}, count};
    }

    platform::FileTransfer Write(const void*, usize) noexcept override {
        return {{platform::FileCode::Unsupported, 0}, 0};
    }

    platform::FileStatus Seek(int64 offset, platform::FileSeekOrigin origin,
                              uint64& out) noexcept override {
        const int64 base = origin == platform::FileSeekOrigin::Begin ? 0
                           : origin == platform::FileSeekOrigin::Current
                               ? static_cast<int64>(position)
                               : static_cast<int64>(data->Size());
        position = static_cast<usize>(base + offset);
        out = position;
        return {};
    }

    platform::FileStatus Size(uint64& bytes) noexcept override {
        if (fault == FileFault::Size) {
            return FaultStatus;
        }

        bytes = data->Size();
        bytes += fault == FileFault::Short ? 1 : 0;
        bytes = fault == FileFault::HugeSize ? UnrealExchangeMaxJsonBytes + 1 : bytes;
        return {};
    }

    platform::FileStatus Close() noexcept override {
        return {};
    }

    void Destroy() noexcept override {
        inUse = false;
    }

    const Array<uint8>* data = nullptr;
    usize position = 0;
    usize reads = 0;
    FileFault fault = FileFault::None;
    bool inUse = false;
};

// 경로 → byte의 메모리 파일 시스템이다. 경로는 정확히 같은 문자열로 찾는다.
class MemoryFiles final : public platform::FileSystem {
public:
    static constexpr usize HandleCount = 4;

    explicit MemoryFiles(Allocator& allocator) noexcept
        : allocator_(&allocator), entries_(allocator), faultPath(allocator) {
    }

    // path의 내용을 bytes로 둔다(없으면 더한다).
    bool Put(StringView path, Span<const uint8> bytes) noexcept {
        Array<uint8>* found = Find(path);
        if (found == nullptr) {
            Entry entry(*allocator_);
            if (!entry.path.TryAssign(path) || !entries_.TryPushBack(rocks::Move(entry))) {
                return false;
            }

            found = &entries_[entries_.Size() - 1].bytes;
        }

        found->Clear();
        return static_cast<bool>(found->TryAppend(bytes));
    }

    // path 항목의 종류·link 여부를 바꾼다(일반 파일이 아닌 항목 시험 — 10.4절 19번). 없으면
    // false다.
    bool SetKind(StringView path, platform::FileType type, bool isLink) noexcept {
        for (Entry& entry : entries_) {
            if (entry.path.View() == path) {
                entry.type = type;
                entry.isLink = isLink;
                return true;
            }
        }

        return false;
    }

    bool PutText(StringView path, StringView text) noexcept {
        return Put(
            path, Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
    }

    Array<uint8>* Find(StringView path) noexcept {
        for (Entry& entry : entries_) {
            if (entry.path.View() == path) {
                return &entry.bytes;
            }
        }

        return nullptr;
    }

    bool Remove(StringView path) noexcept {
        for (usize index = 0; index < entries_.Size(); ++index) {
            if (entries_[index].path.View() == path) {
                return static_cast<bool>(entries_.TryRemoveAt(index));
            }
        }

        return false;
    }

    // path 파일 안의 from 한 곳(정확히 한 번 나와야 한다)을 to로 바꾼다.
    bool ReplaceOnce(StringView path, StringView from, StringView to) noexcept {
        Array<uint8>* bytes = Find(path);
        if (bytes == nullptr) {
            return false;
        }

        const StringView text(reinterpret_cast<const char*>(bytes->Data()), bytes->Size());
        const usize at = text.Find(from, StringComparison::CaseSensitive);
        if (at == StringView::NotFound ||
            text.Find(from, StringComparison::CaseSensitive, StringSearchDirection::FromStart,
                      at + 1) != StringView::NotFound) {
            return false;
        }

        StringBuilder<256> edited(*allocator_);
        if (!edited.TryAppend(StringView(text.Data(), at)) || !edited.TryAppend(to) ||
            !edited.TryAppend(StringView(text.Data() + at + from.ByteCount(),
                                         text.ByteCount() - at - from.ByteCount()))) {
            return false;
        }

        return PutText(path, edited.View());
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        ++opens;
        const StringView name(path, pathBytes);
        const bool faulty = !faultPath.View().IsEmpty() && name == faultPath.View();
        if (faulty && fault == FileFault::Open) {
            return FaultStatus;
        }

        Array<uint8>* found = Find(name);
        if (found == nullptr || mode != platform::FileOpenMode::ReadExisting) {
            return {platform::FileCode::NotFound, 0};
        }

        for (MemoryHandle& handle : handles_) {
            if (!handle.inUse) {
                handle.inUse = true;
                handle.data = found;
                handle.position = 0;
                handle.reads = 0;
                handle.fault = faulty ? fault : FileFault::None;
                out = &handle;
                return {};
            }
        }

        return {platform::FileCode::Busy, 0};
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        const StringView name(path, pathBytes);
        if (!faultPath.View().IsEmpty() && name == faultPath.View() && fault == FileFault::Stat) {
            return FaultStatus;
        }

        for (const Entry& entry : entries_) {
            if (entry.path.View() == name) {
                info.type = entry.type;
                info.isLink = entry.isLink;
                info.size = entry.type == platform::FileType::File ? entry.bytes.Size() : 0;
                return {};
            }
        }

        return {platform::FileCode::NotFound, 0};
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        exists = Find(StringView(path, pathBytes)) != nullptr;
        return {};
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        return Remove(StringView(path, pathBytes))
                   ? platform::FileStatus{}
                   : platform::FileStatus{platform::FileCode::NotFound, 0};
    }

    // 열린 handle 수다(시험 끝에 0이어야 한다).
    usize OpenHandles() const noexcept {
        usize count = 0;
        for (const MemoryHandle& handle : handles_) {
            count += handle.inUse ? 1 : 0;
        }

        return count;
    }

private:
    struct Entry {
        explicit Entry(Allocator& allocator) noexcept : path(allocator), bytes(allocator) {
        }

        String path;
        Array<uint8> bytes;
        platform::FileType type = platform::FileType::File;
        bool isLink = false;
    };

    Allocator* allocator_;
    Array<Entry> entries_;
    MemoryHandle handles_[HandleCount];

public:
    // 이 경로를 열 때 fault를 낸다.
    String faultPath;
    FileFault fault = FileFault::None;
    // Open 호출 수다.
    usize opens = 0;
};

// 메모리 묶음 안 상대 경로의 전체 경로다(`<directory>/<relative>`).
inline bool BundlePath(StringView directory, StringView relative, StringBuilderBase& out) noexcept {
    return out.TryAppend(directory) && out.TryAppendAscii('/') && out.TryAppend(relative);
}

namespace detail {

struct DirectoryNames {
    Allocator* allocator;
    Array<String>* names;
    Array<uint8>* directories;
    bool failed;
};

inline bool CollectDirectoryEntry(const platform::DirectoryEntry& entry, void* context) noexcept {
    DirectoryNames& names = *static_cast<DirectoryNames*>(context);
    const StringView name(entry.name, entry.nameBytes);
    if (name == StringView(".") || name == StringView("..")) {
        return true;
    }

    String copy(*names.allocator);
    if (!copy.TryAssign(name) || !names.names->TryPushBack(Move(copy)) ||
        !names.directories->TryPushBack(entry.info.type == platform::FileType::Directory ? 1 : 0)) {
        names.failed = true;
        return false;
    }

    return true;
}

} // namespace detail

// 저장소 fixture 묶음 디렉터리(Engine/Plugins/Importers/UnrealExchange/Tests/Bundles/<name>)의 전체
// 경로를 out에 붙인다.
inline bool FixtureSourcePath(StringView name, StringBuilderBase& out) noexcept {
    constexpr usize PathCapacity = 4096;
    char engine[PathCapacity];
    auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
    if (!found) {
        return false;
    }

    return out.TryAppend(StringView(engine, found.Value())) &&
           out.TryAppend("/Plugins/Importers/UnrealExchange/Tests/Bundles/") && out.TryAppend(name);
}

// 엔진 디렉터리 기준 상대 경로(`/Plugins/...`) 파일의 내용을 out에 읽는다.
inline bool ReadRepositoryFile(Allocator& allocator, StringView relative,
                               Array<char>& out) noexcept {
    constexpr usize PathCapacity = 4096;
    char engine[PathCapacity];
    auto found = FindEngineDirectory(Span<char>(engine, PathCapacity));
    StringBuilder<512> path(allocator);
    if (!found || !path.TryAppend(StringView(engine, found.Value())) || !path.TryAppend(relative)) {
        return false;
    }

    platform::File file;
    uint64 size = 0;
    if (!file.Open(platform::PhysicalFileSystem(), path.CString(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting) ||
        !file.Size(size) || !out.TryResize(static_cast<usize>(size))) {
        return false;
    }

    return static_cast<bool>(file.ReadExact(out.Data(), out.Size()).status);
}

// 실제 파일 시스템의 source 디렉터리 아래 모든 파일을 files의 target 아래로 복사한다.
inline bool CopyTree(Allocator& allocator, StringView source, StringView target,
                     MemoryFiles& files) noexcept {
    Array<String> pending(allocator);
    String empty(allocator);
    if (!pending.TryPushBack(Move(empty))) {
        return false;
    }

    platform::FileSystem& physical = platform::PhysicalFileSystem();
    while (!pending.IsEmpty()) {
        String relative = Move(pending[pending.Size() - 1]);
        if (!pending.TryRemoveAt(pending.Size() - 1)) {
            return false;
        }

        StringBuilder<512> directory(allocator);
        if (!directory.TryAppend(source) ||
            (!relative.View().IsEmpty() &&
             (!directory.TryAppendAscii('/') || !directory.TryAppend(relative.View())))) {
            return false;
        }

        Array<String> names(allocator);
        Array<uint8> directories(allocator);
        detail::DirectoryNames context{&allocator, &names, &directories, false};
        if (!physical.EnumerateDirectory(directory.CString(), directory.ByteCount(),
                                         &detail::CollectDirectoryEntry, &context) ||
            context.failed) {
            return false;
        }

        for (usize index = 0; index < names.Size(); ++index) {
            String child(allocator);
            if ((!relative.View().IsEmpty() &&
                 (!child.TryAppend(relative.View()) || !child.TryAppend("/"))) ||
                !child.TryAppend(names[index].View())) {
                return false;
            }

            if (directories[index] != 0) {
                if (!pending.TryPushBack(Move(child))) {
                    return false;
                }

                continue;
            }

            StringBuilder<512> path(allocator);
            if (!path.TryAppend(source) || !path.TryAppendAscii('/') ||
                !path.TryAppend(child.View())) {
                return false;
            }

            platform::File file;
            uint64 size = 0;
            if (!file.Open(physical, path.CString(), path.ByteCount(),
                           platform::FileOpenMode::ReadExisting) ||
                !file.Size(size)) {
                return false;
            }

            Array<uint8> bytes(allocator);
            if (!bytes.TryResize(static_cast<usize>(size)) ||
                (size != 0 && !file.ReadExact(bytes.Data(), bytes.Size()).status)) {
                return false;
            }

            StringBuilder<512> memoryPath(allocator);
            if (!BundlePath(target, child.View(), memoryPath) ||
                !files.Put(memoryPath.View(), bytes.View())) {
                return false;
            }
        }
    }

    return true;
}

// 저장소 fixture "Basic.rxbundle"을 files의 FixtureDirectory 아래로 복사한다.
inline bool LoadFixture(Allocator& allocator, MemoryFiles& files) noexcept {
    StringBuilder<512> source(allocator);
    return FixtureSourcePath("Basic.rxbundle", source) &&
           CopyTree(allocator, source.View(), FixtureDirectory, files);
}

// fixture 안 파일의 메모리 경로다.
inline bool FixtureFile(StringView relative, StringBuilderBase& out) noexcept {
    return BundlePath(FixtureDirectory, relative, out);
}

// 합성 묶음의 의존 항목이다.
struct SyntheticDependency {
    StringView objectPath;
    StringView className;
    bool soft = false;
};

// 합성 묶음의 에셋이다. data는 에셋 JSON의 data 객체 원문이다.
struct SyntheticAsset {
    StringView objectPath;
    StringView className;
    Span<const SyntheticDependency> dependencies;
    StringView data = "{}";
};

struct SyntheticRedirect {
    StringView from;
    StringView to;
};

// 합성 묶음을 files의 SyntheticDirectory 아래에 쓴다. assets는 objectPath 순, 의존은 objectPath
// 순이어야 한다(시험이 정렬 위반을 따로 만든다). 문자열에 `"`·`\`가 없어야 한다.
inline bool WriteSyntheticBundle(Allocator& allocator, MemoryFiles& files,
                                 Span<const SyntheticAsset> assets,
                                 Span<const SyntheticRedirect> redirects = {}) noexcept {
    StringBuilder<256> manifest(allocator);
    bool ok = static_cast<bool>(manifest.TryAppend(
        "{\"formatVersion\": 1, \"exporter\": {\"plugin\": \"RocksExport\", \"pluginVersion\": "
        "\"1.0.0\", \"engineVersion\": \"5.8.4\", \"engineCommit\": \"0\"}, \"projectSettings\": "
        "{\"substrate\": false, \"retainJpegFormat\": false}, \"assets\": ["));
    for (usize index = 0; ok && index < assets.Size(); ++index) {
        const SyntheticAsset& asset = assets[index];
        const StringView package = UnrealPackagePath(asset.objectPath);
        ok = (index == 0 || manifest.TryAppend(", ")) &&
             manifest.TryAppend("{\"objectPath\": \"") && manifest.TryAppend(asset.objectPath) &&
             manifest.TryAppend("\", \"class\": \"") && manifest.TryAppend(asset.className) &&
             manifest.TryAppend("\", \"file\": \"assets") && manifest.TryAppend(package) &&
             manifest.TryAppend(".json\"}");
        if (!ok || FindExchangeClass(asset.className) == ExchangeClass::Unsupported) {
            continue;
        }

        StringBuilder<1024> text(allocator);
        ok = text.TryAppend("{\"objectPath\": \"") && text.TryAppend(asset.objectPath) &&
             text.TryAppend("\", \"class\": \"") && text.TryAppend(asset.className) &&
             text.TryAppend("\", \"ueClass\": \"/Script/Engine.") &&
             text.TryAppend(asset.className) && text.TryAppend("\", \"dependencies\": [");
        for (usize at = 0; ok && at < asset.dependencies.Size(); ++at) {
            const SyntheticDependency& dependency = asset.dependencies[at];
            ok = (at == 0 || text.TryAppend(", ")) && text.TryAppend("{\"objectPath\": \"") &&
                 text.TryAppend(dependency.objectPath) && text.TryAppend("\", \"class\": \"") &&
                 text.TryAppend(dependency.className) && text.TryAppend("\", \"kind\": \"") &&
                 text.TryAppend(dependency.soft ? StringView("soft") : StringView("hard")) &&
                 text.TryAppend("\"}");
        }

        ok = ok && text.TryAppend("], \"diagnostics\": [], \"settings\": {}, \"data\": ") &&
             text.TryAppend(asset.data) && text.TryAppend("}\n");
        StringBuilder<512> path(allocator);
        ok = ok && path.TryAppend(SyntheticDirectory) && path.TryAppend("/assets") &&
             path.TryAppend(package) && path.TryAppend(".json") &&
             files.PutText(path.View(), text.View());
    }

    ok = ok && manifest.TryAppend("], \"redirects\": [");
    for (usize index = 0; ok && index < redirects.Size(); ++index) {
        ok = (index == 0 || manifest.TryAppend(", ")) && manifest.TryAppend("{\"from\": \"") &&
             manifest.TryAppend(redirects[index].from) && manifest.TryAppend("\", \"to\": \"") &&
             manifest.TryAppend(redirects[index].to) && manifest.TryAppend("\"}");
    }

    StringBuilder<512> path(allocator);
    return ok && manifest.TryAppend("]}\n") &&
           BundlePath(SyntheticDirectory, "bundle.json", path) &&
           files.PutText(path.View(), manifest.View());
}

// bytes를 내용 주소 blob 파일로 쓰고 그 참조 JSON을 out에 붙인다.
inline bool WriteSyntheticBlob(MemoryFiles& files, Span<const uint8> bytes, StringView layout,
                               uint64 components, uint64 count, StringBuilderBase& out) noexcept {
    const Sha256Digest digest = Sha256::Hash(bytes);
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    StringBuilder<256> path(out.GetAllocator());
    const bool written = path.TryAppend(SyntheticDirectory) && path.TryAppend("/blobs/") &&
                         path.TryAppend(StringView(hex, 2)) && path.TryAppendAscii('/') &&
                         path.TryAppend(StringView(hex, Sha256Digest::HexBytes)) &&
                         path.TryAppend(".bin") && files.Put(path.View(), bytes);
    return written && out.TryAppend("{\"blob\": \"") &&
           out.TryAppend(StringView(hex, Sha256Digest::HexBytes)) &&
           out.TryAppendFormat("\", \"bytes\": {}, \"layout\": \"{}\", \"components\": {}, "
                               "\"count\": {}}}",
                               bytes.Size(), layout, components, count);
}

// little-endian 조각을 이어 붙이는 blob 작성기다.
class BlobBytes {
public:
    explicit BlobBytes(Allocator& allocator) noexcept : bytes(allocator) {
    }

    bool Word(uint32 value) noexcept {
        const uint8 raw[] = {static_cast<uint8>(value), static_cast<uint8>(value >> 8),
                             static_cast<uint8>(value >> 16), static_cast<uint8>(value >> 24)};
        return static_cast<bool>(bytes.TryAppend(Span<const uint8>(raw)));
    }

    bool Half(uint16 value) noexcept {
        const uint8 raw[] = {static_cast<uint8>(value), static_cast<uint8>(value >> 8)};
        return static_cast<bool>(bytes.TryAppend(Span<const uint8>(raw)));
    }

    bool Text(StringView text) noexcept {
        return Word(static_cast<uint32>(text.ByteCount())) &&
               bytes.TryAppend(Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()),
                                                 text.ByteCount()));
    }

    bool Fill(usize count, uint8 value) noexcept {
        for (usize index = 0; index < count; ++index) {
            if (!bytes.TryPushBack(value)) {
                return false;
            }
        }

        return true;
    }

    Array<uint8> bytes;
};

// 진단 목록에서 (code, issue) 진단 수다.
inline usize CountIssues(const ExchangeDiagnostics& diagnostics, AssetDiagnosticCode code,
                         ExchangeIssue issue) noexcept {
    usize count = 0;
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        count += item.code == code && item.issue == issue ? 1 : 0;
    }

    return count;
}

// asset 번호의 첫 진단이다. 없으면 null이다.
inline const ExchangeDiagnostic* FindAssetDiagnostic(const ExchangeDiagnostics& diagnostics,
                                                     uint32 asset) noexcept {
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        if (item.asset == asset) {
            return &item;
        }
    }

    return nullptr;
}

} // namespace rocks::exchange_test
