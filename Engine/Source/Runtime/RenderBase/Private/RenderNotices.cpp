#include <RenderBase/RenderNotices.h>

#include <Core/Move.h>
#include <Platform/Memory.h>

namespace rocks::render {

RenderNotices::RenderNotices(Allocator& allocator) noexcept : queue_(allocator) {
}

void RenderNotices::Push(const RenderNotice& notice) noexcept {
    platform::LockGuard lock(gate_);
    auto pushed = queue_.TryPushBack(notice);
    if (!pushed) {
        ++dropped_;
    }
}

Result<void> RenderNotices::TakeAll(Array<RenderNotice>& out) noexcept {
    platform::LockGuard lock(gate_);
    auto appended = out.TryAppend(queue_.View());
    if (!appended) {
        return appended;
    }

    queue_.Clear();
    return Result<void>::Success();
}

usize RenderNotices::Count() const noexcept {
    platform::LockGuard lock(gate_);
    return queue_.Size();
}

uint64 RenderNotices::Dropped() const noexcept {
    platform::LockGuard lock(gate_);
    return dropped_;
}

RenderBlob::RenderBlob(RenderBlob&& other) noexcept
    : allocator_(other.allocator_), data_(other.data_), size_(other.size_) {
    other.allocator_ = nullptr;
    other.data_ = nullptr;
    other.size_ = 0;
}

RenderBlob& RenderBlob::operator=(RenderBlob&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        data_ = other.data_;
        size_ = other.size_;
        other.allocator_ = nullptr;
        other.data_ = nullptr;
        other.size_ = 0;
    }

    return *this;
}

RenderBlob::~RenderBlob() noexcept {
    Release();
}

void RenderBlob::Release() noexcept {
    if (data_ != nullptr) {
        (void)allocator_->Deallocate(data_);
    }

    allocator_ = nullptr;
    data_ = nullptr;
    size_ = 0;
}

Result<RenderBlob> RenderBlob::TryCopy(Allocator& allocator, Span<const uint8> bytes) noexcept {
    RenderBlob blob;
    if (bytes.Size() == 0) {
        return Result<RenderBlob>::Success(Move(blob));
    }

    auto memory = allocator.Allocate(bytes.Size(), 16);
    if (!memory) {
        return Result<RenderBlob>::Failure(memory.Error());
    }

    blob.allocator_ = &allocator;
    blob.data_ = static_cast<uint8*>(memory.Value());
    blob.size_ = bytes.Size();
    platform::MemoryCopy(blob.data_, bytes.Data(), bytes.Size());
    return Result<RenderBlob>::Success(Move(blob));
}

} // namespace rocks::render
