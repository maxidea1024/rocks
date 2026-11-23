#pragma once

#include <Base/Compiler.h>
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/String.h>
#include <Platform/File.h>
#include <string.h>

// AssetDatabase 시험의 메모리 파일 시스템이다(Docs/Design/AssetSystem 7.1절 — 색인·열기·저장
// 시험의 진단 장면: 링크·장치·같은 경로 키·IO 고장 주입).
namespace rocks::asset_database_test {

// 노드 하나의 고장 주입이다.
enum class MemoryFault {
    None,
    // 열기에서 없음(열거와 열기 사이에 사라진 파일)·사용 중.
    OpenMissing,
    OpenBusy,
    // 읽기 실패.
    Read,
    // 열린 핸들의 크기 조회(Size) 실패.
    Size,
    // 디렉터리 열거 실패·열거에서 없음(검색 중 지워진 디렉터리).
    Enumerate,
    EnumerateMissing
};

constexpr platform::FileStatus MemoryError{platform::FileCode::SystemError, 7};
constexpr platform::FileStatus MemoryBusy{platform::FileCode::Busy, 32};
constexpr platform::FileStatus MemoryMissing{platform::FileCode::NotFound, 2};
constexpr platform::FileStatus MemoryWrongType{platform::FileCode::WrongType, 0};
constexpr platform::FileStatus MemoryExists{platform::FileCode::AlreadyExists, 80};
constexpr platform::FileStatus MemoryNotDurable{platform::FileCode::NotDurable, 5};
// MemoryFaults::readFailFrom의 "끔" 표지다.
constexpr uint64 MemoryNoReadLimit = ~uint64{0};

// 파일 시스템 전체의 고장 주입이다(저장 경로 — 성공 상태면 주입하지 않는다).
struct MemoryFaults {
    // 새 파일 만들기(CreateNew·CreateAlways)·디렉터리 만들기의 실패다.
    platform::FileStatus create{};
    platform::FileStatus makeDirectory{};
    // 쓰기 핸들의 n번째 Write 호출(1부터, 0은 끔)이 실패한다.
    uint32 failWriteCall = 0;
    // 쓰기 핸들의 n번째 Write 호출(1부터, 0은 끔)이 첫 byte를 뒤집어 쓰고 성공한다(쓰기 손상 —
    // 저장의 재검증이 잡아야 한다).
    uint32 corruptWriteCall = 0;
    // 쓰기 핸들의 Flush·Close 실패다.
    platform::FileStatus flush{};
    platform::FileStatus close{};
    // Replace의 결과다. NotDurable이면 교체한 뒤 그 상태를 돌려주고, 그 밖의 실패는 교체하지
    // 않는다.
    platform::FileStatus replace{};
    // 읽기 열기(ReadExisting)의 실패다 — 경로가 readSuffix로 끝날 때만(예: 저장 임시 파일 ".tmp"를
    // 다시 열 때).
    platform::FileStatus openRead{};
    const char* readSuffix = nullptr;
    // 읽기가 이 위치(byte)를 넘어 읽으려 하면 실패한다(MemoryNoReadLimit은 끔) — 앞쪽(머리·표)은
    // 읽히고 뒤쪽(값 byte)만 실패하는 파일. 모든 핸들에 적용된다.
    uint64 readFailFrom = MemoryNoReadLimit;
    // Delete·Stat의 실패다(Stat는 statPath가 비어 있지 않으면 그 Content 상대 경로에만).
    platform::FileStatus remove{};
    // Move의 실패다(7.1절 17 A2e2 — 이동 시험).
    platform::FileStatus move{};
    platform::FileStatus stat{};
    const char* statPath = nullptr;
    // SyncDirectory의 결과다(NotDurable은 성공처럼 세고 그 상태를 돌려준다).
    platform::FileStatus sync{};
};

class MemoryHandle final : public platform::FileHandle {
public:
    void Reset(SharedPtr<Array<uint8>> content, bool failRead, bool failSize, bool writable,
               MemoryFaults* faults, uint32* writeCalls) noexcept {
        content_ = rocks::Move(content);
        failRead_ = failRead;
        failSize_ = failSize;
        writable_ = writable;
        faults_ = faults;
        writeCalls_ = writeCalls;
        position_ = 0;
        inUse = true;
    }

    platform::FileTransfer Read(void* destination, usize bytes) noexcept override {
        if (failRead_ || (faults_->readFailFrom != MemoryNoReadLimit &&
                          position_ + bytes > faults_->readFailFrom)) {
            return {MemoryError, 0};
        }

        const Array<uint8>& content = *content_.Get();
        const usize left = position_ < content.Size() ? content.Size() - position_ : 0;
        const usize count = bytes < left ? bytes : left;
        if (count != 0) {
            memcpy(destination, content.Data() + position_, count);
        }

        position_ += count;
        return {platform::FileStatus{}, count};
    }

    platform::FileTransfer Write(const void* source, usize bytes) noexcept override {
        if (!writable_) {
            return {MemoryError, 0};
        }

        ++*writeCalls_;
        if (faults_->failWriteCall != 0 && *writeCalls_ == faults_->failWriteCall) {
            return {MemoryError, 0};
        }

        Array<uint8>& content = *content_.Get();
        if (position_ + bytes > content.Size() && !content.TryResize(position_ + bytes)) {
            return {MemoryError, 0};
        }

        if (bytes != 0) {
            memcpy(content.Data() + position_, source, bytes);
            if (faults_->corruptWriteCall != 0 && *writeCalls_ == faults_->corruptWriteCall) {
                content[position_] = static_cast<uint8>(content[position_] ^ 0xff);
            }
        }

        position_ += bytes;
        return {platform::FileStatus{}, bytes};
    }

    platform::FileStatus Seek(int64 offset, platform::FileSeekOrigin origin,
                              uint64& position) noexcept override {
        const int64 base = origin == platform::FileSeekOrigin::Begin
                               ? 0
                               : (origin == platform::FileSeekOrigin::Current
                                      ? static_cast<int64>(position_)
                                      : static_cast<int64>(content_.Get()->Size()));
        position_ = static_cast<usize>(base + offset);
        position = position_;
        return {};
    }

    platform::FileStatus Size(uint64& bytes) noexcept override {
        if (failSize_) {
            return MemoryError;
        }

        bytes = content_.Get()->Size();
        return {};
    }

    platform::FileStatus Flush() noexcept override {
        return writable_ ? faults_->flush : platform::FileStatus{};
    }

    platform::FileStatus Close() noexcept override {
        return writable_ ? faults_->close : platform::FileStatus{};
    }

    // File이 닫히면 풀로 돌아간다(내용 참조를 놓는다).
    void Destroy() noexcept override {
        content_ = SharedPtr<Array<uint8>>();
        inUse = false;
    }

    bool inUse = false;

private:
    SharedPtr<Array<uint8>> content_;
    bool failRead_ = false;
    bool failSize_ = false;
    bool writable_ = false;
    MemoryFaults* faults_ = nullptr;
    uint32* writeCalls_ = nullptr;
    usize position_ = 0;
};

// 노드 표로 만든 파일 시스템이다(경로는 Content 절대 경로 + "/" + 상대). 핸들은 풀에서 빌려 여러
// 파일을 함께 열 수 있다(bulk 핸들 캐시가 쥔 동안 다른 파일을 연다). 파일 내용은 노드와 열린 핸들이
// 함께 쥐는 공유 버퍼다: Replace·Delete 뒤에도 이미 연 핸들은 이전 내용을 읽는다(POSIX 의미
// rename과 같다). AddFile은 byte를 복사한다. opens는 성공한 열기 수다. Content의 조상 디렉터리는
// 있는 디렉터리로 본다.
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
        SharedPtr<Array<uint8>> content;
        MemoryFault fault = MemoryFault::None;
    };

    MemoryFileSystem(Allocator& allocator, StringView content) noexcept
        : journal(allocator), allocator_(&allocator), content_(allocator), nodes_(allocator),
          handles_(allocator) {
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

    // relative 파일의 내용이다(없으면 빈 span — Has로 구분한다).
    Span<const uint8> Bytes(StringView relative) const noexcept {
        for (const Node& node : nodes_) {
            if (Relative(node) == relative && node.content.Get() != nullptr) {
                return node.content.Get()->View();
            }
        }

        return {};
    }

    bool Has(StringView relative) const noexcept {
        for (const Node& node : nodes_) {
            if (Relative(node) == relative) {
                return true;
            }
        }

        return false;
    }

    // 노드 수(Content 자신 포함)와 i번째 노드의 Content 상대 경로다.
    usize NodeCount() const noexcept {
        return nodes_.Size();
    }

    StringView NodeName(usize index) const noexcept {
        return Relative(nodes_[index]);
    }

    platform::FileStatus Open(const char* path, usize pathBytes, platform::FileOpenMode mode,
                              platform::FileHandle*& out) noexcept override {
        const StringView target(path, pathBytes);
        Node* node = Find(target);
        const bool create = mode == platform::FileOpenMode::CreateNew ||
                            mode == platform::FileOpenMode::CreateAlways;
        if (create) {
            if (!faults.create) {
                return faults.create;
            }

            if (node != nullptr && mode == platform::FileOpenMode::CreateNew) {
                return MemoryExists;
            }

            if (!ParentIsDirectory(target)) {
                return MemoryMissing;
            }

            if (node == nullptr) {
                if (!Add(StringView(path + content_.View().ByteCount() + 1,
                                    pathBytes - content_.View().ByteCount() - 1),
                         platform::FileType::File, false, {}, MemoryFault::None)) {
                    return MemoryError;
                }

                node = &nodes_[nodes_.Size() - 1];
            } else {
                node->content.Get()->Clear();
            }
        }

        if (node == nullptr || node->fault == MemoryFault::OpenMissing) {
            return MemoryMissing;
        }

        if (mode == platform::FileOpenMode::ReadExisting && !faults.openRead &&
            faults.readSuffix != nullptr &&
            target.EndsWith(StringView(faults.readSuffix), StringComparison::CaseSensitive)) {
            return faults.openRead;
        }

        if (node->fault == MemoryFault::OpenBusy) {
            return MemoryBusy;
        }

        if (node->type == platform::FileType::Directory) {
            return MemoryWrongType;
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

        const bool writable = mode != platform::FileOpenMode::ReadExisting;
        handle->Reset(node->content, node->fault == MemoryFault::Read,
                      node->fault == MemoryFault::Size, writable, &faults, &writeCalls_);
        out = handle;
        ++opens;
        return {};
    }

    // 지금까지 쓰기 핸들의 Write 호출 수다(n번째 쓰기 고장의 기준).
    uint32 WriteCalls() const noexcept {
        return writeCalls_;
    }

    // 지금 열린 핸들 수다.
    usize OpenHandles() const noexcept {
        usize count = 0;
        for (const UniquePtr<MemoryHandle>& pooled : handles_) {
            count += pooled->inUse ? 1 : 0;
        }

        return count;
    }

    platform::FileStatus FileExists(const char* path, usize pathBytes,
                                    bool& exists) noexcept override {
        exists = Find(StringView(path, pathBytes)) != nullptr;
        return {};
    }

    platform::FileStatus Delete(const char* path, usize pathBytes) noexcept override {
        if (!faults.remove) {
            return faults.remove;
        }

        Node* node = Find(StringView(path, pathBytes));
        if (node == nullptr) {
            return MemoryMissing;
        }

        if (node->type == platform::FileType::Directory) {
            return MemoryWrongType;
        }

        ++deletes;
        Remove(Relative(*node));
        return {};
    }

    platform::FileStatus Stat(const char* path, usize pathBytes,
                              platform::FileInfo& info) noexcept override {
        const StringView target(path, pathBytes);
        if (!faults.stat &&
            (faults.statPath == nullptr || RelativeOf(target) == StringView(faults.statPath))) {
            return faults.stat;
        }

        if (IsContentAncestor(target)) {
            info = platform::FileInfo{};
            info.type = platform::FileType::Directory;
            return {};
        }

        const Node* node = Find(target);
        if (node == nullptr) {
            return MemoryMissing;
        }

        info = platform::FileInfo{};
        info.type = node->type;
        info.isLink = node->isLink;
        info.size = node->content.Get() != nullptr ? node->content.Get()->Size() : 0;
        info.modifiedTicks = static_cast<int64>(info.size) + 1 + modifiedShift;
        return {};
    }

    platform::FileStatus MakeDirectory(const char* path, usize pathBytes) noexcept override {
        const StringView target(path, pathBytes);
        if (IsContentAncestor(target) || Find(target) != nullptr) {
            return MemoryExists;
        }

        if (!faults.makeDirectory) {
            return faults.makeDirectory;
        }

        if (!ParentIsDirectory(target)) {
            return MemoryMissing;
        }

        return AddDirectory(RelativeOf(target)) ? platform::FileStatus{} : MemoryError;
    }

    // 디렉터리 동기화다(메모리라 매체가 없다 — 성공). faults.sync가 실패 상태면 그것을(NotDurable
    // 등), 없으면 NotFound, 디렉터리가 아니면 WrongType이다. syncs는 성공·NotDurable 호출 수다.
    platform::FileStatus SyncDirectory(const char* path, usize pathBytes) noexcept override {
        const StringView target(path, pathBytes);
        if (!IsContentAncestor(target)) {
            const Node* node = Find(target);
            if (node == nullptr) {
                return MemoryMissing;
            }

            if (node->type != platform::FileType::Directory) {
                return MemoryWrongType;
            }
        }

        if (!faults.sync && faults.sync.code != platform::FileCode::NotDurable) {
            return faults.sync;
        }

        ++syncs;
        Record("sync ", target);
        return faults.sync;
    }

    platform::FileStatus Replace(const char* from, usize fromBytes, const char* to,
                                 usize toBytes) noexcept override {
        if (!faults.replace && faults.replace.code != platform::FileCode::NotDurable) {
            return faults.replace;
        }

        Node* source = Find(StringView(from, fromBytes));
        if (source == nullptr) {
            return MemoryMissing;
        }

        // 대상 노드를 빼고(열린 핸들은 내용을 쥐고 있다) 원본의 이름을 바꾼다.
        const StringView target(to, toBytes);
        const StringView relative = RelativeOf(target);
        Node renamed(*allocator_);
        if (!renamed.path.TryAppend(Span<const char>(to, toBytes))) {
            return MemoryError;
        }

        usize slash = relative.ByteCount();
        while (slash > 0 && relative.Data()[slash - 1] != '/') {
            --slash;
        }

        if (!renamed.name.TryAppend(
                Span<const char>(relative.Data() + slash, relative.ByteCount() - slash)) ||
            !renamed.name.TryPushBack('\0')) {
            return MemoryError;
        }

        renamed.type = source->type;
        renamed.content = source->content;
        Remove(RelativeOf(StringView(from, fromBytes)));
        Remove(relative);
        if (!nodes_.TryPushBack(rocks::Move(renamed))) {
            return MemoryError;
        }

        ++replaces;
        Record("replace ", target);
        return faults.replace;
    }

    // 기록이 켜져 있으면 "<kind><상대 경로>"를 남긴다(할당 실패는 기록을 버린다 — 시험 보조).
    void Record(StringView kind, StringView path) noexcept {
        if (!recordJournal) {
            return;
        }

        String line(*allocator_);
        if (line.TryAssign(kind) && line.TryAppend(RelativeOf(path))) {
            (void)journal.TryPushBack(rocks::Move(line));
        }
    }

    // 같은 파일 시스템 안 이동이다(파일·디렉터리 — 디렉터리면 하위 노드 경로를 모두 바꾼다).
    // faults.move가 실패 상태면 아무것도 바꾸지 않고 그것을 돌려준다. 원본이 없으면 NotFound,
    // 대상이 있으면 AlreadyExists(디렉터리는 replaceExisting이어도), 대상 부모가 디렉터리가 아니면
    // NotFound다. 같은 경로면 아무것도 하지 않는다.
    platform::FileStatus Move(const char* from, usize fromBytes, const char* to, usize toBytes,
                              bool replaceExisting) noexcept override {
        if (!faults.move) {
            return faults.move;
        }

        const StringView source(from, fromBytes);
        const StringView target(to, toBytes);
        Node* node = Find(source);
        if (node == nullptr) {
            return MemoryMissing;
        }

        if (source == target) {
            return {};
        }

        Node* existing = Find(target);
        if (existing != nullptr &&
            (!replaceExisting || existing->type == platform::FileType::Directory ||
             node->type == platform::FileType::Directory)) {
            return MemoryExists;
        }

        if (!ParentIsDirectory(target)) {
            return MemoryMissing;
        }

        if (existing != nullptr) {
            Remove(RelativeOf(target));
            node = Find(source);
        }

        // 원본과 그 아래 노드의 경로 앞부분을 바꾼다(이름은 마지막 구성 요소).
        Array<char> fromPath(*allocator_);
        if (!fromPath.TryAppend(Span<const char>(from, fromBytes))) {
            return MemoryError;
        }

        for (Node& each : nodes_) {
            const StringView path(each.path.Data(), each.path.Size());
            const StringView prefix(fromPath.Data(), fromPath.Size());
            const bool self = path == prefix;
            const bool below = path.ByteCount() > prefix.ByteCount() &&
                               path.StartsWith(prefix, StringComparison::CaseSensitive) &&
                               path.Data()[prefix.ByteCount()] == '/';
            if (!self && !below) {
                continue;
            }

            Array<char> moved(*allocator_);
            if (!moved.TryAppend(Span<const char>(to, toBytes)) ||
                !moved.TryAppend(Span<const char>(path.Data() + prefix.ByteCount(),
                                                  path.ByteCount() - prefix.ByteCount()))) {
                return MemoryError;
            }

            each.path = rocks::Move(moved);
            if (self) {
                const StringView relative = RelativeOf(target);
                usize slash = relative.ByteCount();
                while (slash > 0 && relative.Data()[slash - 1] != '/') {
                    --slash;
                }

                each.name.Clear();
                if (!each.name.TryAppend(
                        Span<const char>(relative.Data() + slash, relative.ByteCount() - slash)) ||
                    !each.name.TryPushBack('\0')) {
                    return MemoryError;
                }
            }
        }

        ++moves;
        return {};
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
            entry.info.size = child.content.Get() != nullptr ? child.content.Get()->Size() : 0;
            entry.info.modifiedTicks = static_cast<int64>(entry.info.size) + 1 + modifiedShift;
            if (!visitor(entry, context)) {
                break;
            }
        }

        return {};
    }

    MemoryFaults faults;
    // 모든 수정 시각에 더하는 값이다(크기는 같고 시각만 바뀐 파일 — 감시의 관찰).
    int64 modifiedShift = 0;
    uint32 opens = 0;
    uint32 syncs = 0;
    // recordJournal이면 성공한 SyncDirectory·Replace를 "sync <상대 경로>"·"replace <상대 경로>"로
    // 순서대로 남긴다(게시 순서 시험).
    bool recordJournal = false;
    Array<String> journal;
    uint32 deletes = 0;
    uint32 replaces = 0;
    uint32 moves = 0;

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

        if (type != platform::FileType::Directory) {
            auto content = SharedPtr<Array<uint8>>::TryMake(*allocator_, *allocator_);
            if (!content || !content.Value().Get()->TryAppend(bytes)) {
                return false;
            }

            node.content = rocks::Move(content.Value());
        }

        node.type = type;
        node.isLink = link;
        node.fault = fault;
        return static_cast<bool>(nodes_.TryPushBack(rocks::Move(node)));
    }

    StringView Relative(const Node& node) const noexcept {
        return RelativeOf(StringView(node.path.Data(), node.path.Size()));
    }

    // 절대 경로의 Content 상대 부분이다(Content 자신은 빈 문자열).
    StringView RelativeOf(StringView path) const noexcept {
        const usize contentBytes = content_.View().ByteCount();
        const usize skip = path.ByteCount() <= contentBytes ? path.ByteCount() : contentBytes + 1;
        return StringView(path.Data() + skip, path.ByteCount() - skip);
    }

    // Content의 조상(또는 Content 자신의 앞부분 디렉터리)인지다.
    bool IsContentAncestor(StringView path) const noexcept {
        const StringView content = content_.View();
        return path.ByteCount() < content.ByteCount() &&
               content.StartsWith(path, StringComparison::CaseSensitive) &&
               content.Data()[path.ByteCount()] == '/';
    }

    bool ParentIsDirectory(StringView path) const noexcept {
        usize slash = path.ByteCount();
        while (slash > 0 && path.Data()[slash - 1] != '/') {
            --slash;
        }

        if (slash == 0) {
            return false;
        }

        const StringView parent(path.Data(), slash - 1);
        if (IsContentAncestor(parent)) {
            return true;
        }

        const Node* node = Find(parent);
        return node != nullptr && node->type == platform::FileType::Directory;
    }

    Node* Find(StringView path) noexcept {
        for (Node& node : nodes_) {
            if (StringView(node.path.Data(), node.path.Size()) == path) {
                return &node;
            }
        }

        return nullptr;
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
    uint32 writeCalls_ = 0;
    bool ready_ = false;
};

// 메모리 장면의 프로젝트 루트다(디스크는 보지 않는다).
constexpr const char* MemoryRoot = IsWindows ? "C:/MemoryProject" : "/MemoryProject";

} // namespace rocks::asset_database_test
