#pragma once

#include <Base/Compiler.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/String.h>
#include <Platform/File.h>
#include <string.h>

// AssetDatabase 시험의 메모리 파일 시스템이다(Docs/Design/AssetSystem.ko.md 7.1절 — 색인·열기
// 시험의 진단 장면: 링크·장치·같은 경로 키·IO 고장 주입).
namespace rocks::asset_database_test {

// 메모리 파일 시스템의 고장 주입이다.
enum class MemoryFault {
    None,
    // 열기에서 없음(열거와 열기 사이에 사라진 파일)·사용 중.
    OpenMissing,
    OpenBusy,
    // 읽기 실패.
    Read,
    // 디렉터리 열거 실패·열거에서 없음(검색 중 지워진 디렉터리).
    Enumerate,
    EnumerateMissing
};

constexpr platform::FileStatus MemoryError{platform::FileCode::SystemError, 7};
constexpr platform::FileStatus MemoryBusy{platform::FileCode::Busy, 32};
constexpr platform::FileStatus MemoryMissing{platform::FileCode::NotFound, 2};
constexpr platform::FileStatus MemoryWrongType{platform::FileCode::WrongType, 0};

class MemoryHandle final : public platform::FileHandle {
public:
    void Reset(Span<const uint8> bytes, bool failRead) noexcept {
        bytes_ = bytes;
        failRead_ = failRead;
        position_ = 0;
        inUse = true;
    }

    platform::FileTransfer Read(void* destination, usize bytes) noexcept override {
        if (failRead_) {
            return {MemoryError, 0};
        }

        const usize left = bytes_.Size() - position_;
        const usize count = bytes < left ? bytes : left;
        if (count != 0) {
            memcpy(destination, bytes_.Data() + position_, count);
        }

        position_ += count;
        return {platform::FileStatus{}, count};
    }

    platform::FileTransfer Write(const void*, usize) noexcept override {
        return {MemoryError, 0};
    }

    platform::FileStatus Seek(int64 offset, platform::FileSeekOrigin,
                              uint64& position) noexcept override {
        position_ = static_cast<usize>(offset);
        position = position_;
        return {};
    }

    platform::FileStatus Size(uint64& bytes) noexcept override {
        bytes = bytes_.Size();
        return {};
    }

    platform::FileStatus Close() noexcept override {
        return {};
    }

    // File이 닫히면 풀로 돌아간다.
    void Destroy() noexcept override {
        inUse = false;
    }

    bool inUse = false;

private:
    Span<const uint8> bytes_;
    bool failRead_ = false;
    usize position_ = 0;
};

// 노드 표로 만든 파일 시스템이다(경로는 Content 절대 경로 + "/" + 상대). 핸들은 풀에서 빌려 여러
// 파일을 함께 열 수 있다(bulk 핸들 캐시가 쥔 동안 다른 파일을 연다). 파일 byte는 빌린다(호출자가
// 시스템보다 오래 살린다). opens는 성공한 열기 수다.
class MemoryFileSystem final : public platform::FileSystem {
public:
    struct Node {
        explicit Node(Allocator& allocator) noexcept : path(allocator), name(allocator) {
        }

        Node(Node&&) noexcept = default;
        Node& operator=(Node&&) noexcept = default;

        // 경로·이름은 byte 그대로다(POSIX처럼 잘못된 UTF-8도 — String은 UTF-8만 받는다). 이름은
        // NUL로 끝난다.
        Array<char> path;
        Array<char> name;
        platform::FileType type = platform::FileType::File;
        bool isLink = false;
        Span<const uint8> bytes{};
        MemoryFault fault = MemoryFault::None;
    };

    MemoryFileSystem(Allocator& allocator, StringView content) noexcept
        : allocator_(&allocator), content_(allocator), nodes_(allocator), handles_(allocator) {
        ready_ = static_cast<bool>(content_.TryAssign(content)) &&
                 AddDirectory("", false, MemoryFault::None);
    }

    bool Ready() const noexcept {
        return ready_;
    }

    bool AddDirectory(StringView relative, bool link = false,
                      MemoryFault fault = MemoryFault::None) noexcept {
        return Add(relative, platform::FileType::Directory, link, {}, fault);
    }

    bool AddFile(StringView relative, Span<const uint8> bytes,
                 MemoryFault fault = MemoryFault::None,
                 platform::FileType type = platform::FileType::File, bool link = false) noexcept {
        return Add(relative, type, link, bytes, fault);
    }

    // relative 노드(또는 Content 자신 — 빈 문자열)를 지운다.
    void Remove(StringView relative) noexcept {
        for (usize index = 0; index < nodes_.Size(); ++index) {
            if (Relative(nodes_[index]) == relative) {
                (void)nodes_.TryRemoveAt(index);
                return;
            }
        }
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode,
                              platform::FileHandle*& out) noexcept override {
        const Node* node = Find(StringView(path, pathBytes));
        if (node == nullptr || node->fault == MemoryFault::OpenMissing) {
            return MemoryMissing;
        }

        if (node->fault == MemoryFault::OpenBusy) {
            return MemoryBusy;
        }

        MemoryHandle* handle = nullptr;
        for (UniquePtr<MemoryHandle>& pooled : handles_) {
            handle = handle == nullptr && !pooled->inUse ? pooled.Get() : handle;
        }

        if (handle == nullptr) {
            auto made = UniquePtr<MemoryHandle>::TryMake(*allocator_);
            if (!made || !handles_.TryPushBack(rocks::Move(made.Value()))) {
                return MemoryError;
            }

            handle = handles_[handles_.Size() - 1].Get();
        }

        handle->Reset(node->bytes, node->fault == MemoryFault::Read);
        out = handle;
        ++opens;
        return {};
    }

    // 지금 열린 핸들 수다.
    usize OpenHandles() const noexcept {
        usize count = 0;
        for (const UniquePtr<MemoryHandle>& pooled : handles_) {
            count += pooled->inUse ? 1 : 0;
        }

        return count;
    }

    uint32 opens = 0;

    platform::FileStatus Exists(const char* path, usize pathBytes, bool& exists) noexcept override {
        exists = Find(StringView(path, pathBytes)) != nullptr;
        return {};
    }

    platform::FileStatus Delete(const char*, usize) noexcept override {
        return MemoryError;
    }

    platform::FileStatus EnumerateDirectory(const char* path, usize pathBytes,
                                            platform::DirectoryVisitor visitor,
                                            void* context) noexcept override {
        const StringView directory(path, pathBytes);
        const Node* node = Find(directory);
        if (node == nullptr || node->fault == MemoryFault::EnumerateMissing) {
            return MemoryMissing;
        }

        if (node->type != platform::FileType::Directory) {
            return MemoryWrongType;
        }

        if (node->fault == MemoryFault::Enumerate) {
            return MemoryError;
        }

        for (const Node& child : nodes_) {
            const StringView childPath(child.path.Data(), child.path.Size());
            if (childPath.ByteCount() <= directory.ByteCount() + 1 ||
                !childPath.StartsWith(directory, StringComparison::CaseSensitive) ||
                childPath.Data()[directory.ByteCount()] != '/' ||
                StringView(childPath.Data() + directory.ByteCount() + 1,
                           childPath.ByteCount() - directory.ByteCount() - 1)
                    .Contains("/", StringComparison::CaseSensitive)) {
                continue;
            }

            platform::DirectoryEntry entry;
            entry.name = child.name.Data();
            entry.nameBytes = child.name.Size() - 1;
            entry.info.type = child.type;
            entry.info.isLink = child.isLink;
            entry.info.size = child.bytes.Size();
            entry.info.modifiedTicks = static_cast<int64>(child.bytes.Size()) + 1;
            if (!visitor(entry, context)) {
                break;
            }
        }

        return {};
    }

private:
    bool Add(StringView relative, platform::FileType type, bool link, Span<const uint8> bytes,
             MemoryFault fault) noexcept {
        Node node(*allocator_);
        const StringView pieces[] = {content_.View(), relative.IsEmpty() ? "" : "/", relative};
        for (StringView piece : pieces) {
            if (!node.path.TryAppend(Span<const char>(piece.Data(), piece.ByteCount()))) {
                return false;
            }
        }

        usize slash = relative.ByteCount();
        while (slash > 0 && relative.Data()[slash - 1] != '/') {
            --slash;
        }

        if (!node.name.TryAppend(
                Span<const char>(relative.Data() + slash, relative.ByteCount() - slash)) ||
            !node.name.TryPushBack('\0')) {
            return false;
        }

        node.type = type;
        node.isLink = link;
        node.bytes = bytes;
        node.fault = fault;
        return static_cast<bool>(nodes_.TryPushBack(rocks::Move(node)));
    }

    StringView Relative(const Node& node) const noexcept {
        const StringView path(node.path.Data(), node.path.Size());
        const usize skip = path.ByteCount() == content_.View().ByteCount()
                               ? path.ByteCount()
                               : content_.View().ByteCount() + 1;
        return StringView(path.Data() + skip, path.ByteCount() - skip);
    }

    const Node* Find(StringView path) const noexcept {
        for (const Node& node : nodes_) {
            if (StringView(node.path.Data(), node.path.Size()) == path) {
                return &node;
            }
        }

        return nullptr;
    }

    Allocator* allocator_;
    String content_;
    Array<Node> nodes_;
    Array<UniquePtr<MemoryHandle>> handles_;
    bool ready_ = false;
};

// 메모리 장면의 프로젝트 루트다(디스크는 보지 않는다).
constexpr const char* MemoryRoot = IsWindows ? "C:/MemoryProject" : "/MemoryProject";

} // namespace rocks::asset_database_test
