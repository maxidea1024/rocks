#include <Core/Text/String.h>
#include <Base/Checked.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

bool IsContinuation(uint8 value) noexcept {
    return (value & 0xC0u) == 0x80u;
}

} // namespace

Result<void> StringView::ValidateUtf8() const noexcept {
    return IsValidUtf8() ? Result<void>::Success()
                         : Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<StringView> StringView::TrySliceBytes(usize offset, usize byteCount) const noexcept {
    if (offset > bytes_ || byteCount > bytes_ - offset) {
        return Result<StringView>::Failure(ErrorCode::InvalidArgument);
    }

    auto valid = ValidateUtf8();
    if (!valid) {
        return Result<StringView>::Failure(valid.Error());
    }

    const usize end = offset + byteCount;
    if ((offset < bytes_ && IsContinuation(static_cast<uint8>(data_[offset]))) ||
        (end < bytes_ && IsContinuation(static_cast<uint8>(data_[end])))) {
        return Result<StringView>::Failure(ErrorCode::InvalidArgument);
    }

    const char* begin = data_ == nullptr ? nullptr : data_ + offset;
    return Result<StringView>::Success(StringView(begin, byteCount));
}

String::String(Allocator& allocator) noexcept : allocator_(&allocator) {
}

String::~String() noexcept {
    Release();
}

String::String(String&& other) noexcept
    : allocator_(other.allocator_), data_(other.data_), bytes_(other.bytes_),
      capacity_(other.capacity_) {
    // 원본은 allocator를 유지한 빈 문자열이므로 다시 사용할 수 있다.
    other.data_ = nullptr;
    other.bytes_ = 0;
    other.capacity_ = 0;
}

String& String::operator=(String&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        data_ = other.data_;
        bytes_ = other.bytes_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.bytes_ = 0;
        other.capacity_ = 0;
    }

    return *this;
}

Result<String> String::TryClone(Allocator& allocator) const noexcept {
    String clone(allocator);
    auto assigned = clone.TryAssign(View());
    if (!assigned) {
        return Result<String>::Failure(assigned.Error());
    }

    return Result<String>::Success(Move(clone));
}

Result<void> String::TryAssign(StringView source) noexcept {
    if (source.ByteCount() == MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto valid = source.ValidateUtf8();
    if (!valid) {
        return Result<void>::Failure(valid.Error());
    }

    if (source.IsEmpty()) {
        Clear();
        return Result<void>::Success();
    }

    const usize required = source.ByteCount();
    if (required <= capacity_) {
        // 자기 부분 문자열도 source와 destination이 겹칠 수 있으므로 memmove를 사용한다.
        platform::MemoryMove(data_, source.Data(), required);
        data_[required] = '\0';
        bytes_ = required;
        return Result<void>::Success();
    }

    auto allocation = AllocateBuffer(required);
    if (!allocation) {
        return Result<void>::Failure(allocation.Error());
    }

    char* replacement = allocation.Value();
    // source가 이전 buffer를 참조해도 복사가 끝나기 전에는 해제하지 않는다.
    platform::MemoryCopy(replacement, source.Data(), required);
    replacement[required] = '\0';
    FreeBuffer(data_);
    data_ = replacement;
    bytes_ = required;
    capacity_ = required;
    return Result<void>::Success();
}

Result<void> String::TryAppend(StringView source) noexcept {
    usize required = 0;
    if (!TryAdd(bytes_, source.ByteCount(), required) || required == MaxSize) {
        return Result<void>::Failure(ErrorCode::Overflow);
    }

    auto valid = source.ValidateUtf8();
    if (!valid) {
        return Result<void>::Failure(valid.Error());
    }

    if (source.IsEmpty()) {
        return Result<void>::Success();
    }

    if (required <= capacity_) {
        platform::MemoryMove(data_ + bytes_, source.Data(), source.ByteCount());
        data_[required] = '\0';
        bytes_ = required;
        return Result<void>::Success();
    }

    const usize capacity = GrowthCapacity(required);
    auto allocation = AllocateBuffer(capacity);
    if (!allocation) {
        return Result<void>::Failure(allocation.Error());
    }

    char* replacement = allocation.Value();
    if (bytes_ != 0) {
        platform::MemoryCopy(replacement, data_, bytes_);
    }

    // prefix와 추가 source를 모두 읽은 뒤에만 이전 buffer를 회수한다.
    platform::MemoryCopy(replacement + bytes_, source.Data(), source.ByteCount());
    replacement[required] = '\0';
    FreeBuffer(data_);
    data_ = replacement;
    bytes_ = required;
    capacity_ = capacity;
    return Result<void>::Success();
}

void String::AppendPathInPlace(StringView suffix, bool separator) noexcept {
    const usize offset = bytes_ + (separator ? 1 : 0);
    if (!suffix.IsEmpty()) {
        // separator를 먼저 쓰면 겹친 suffix가 손상될 수 있다. memmove로 원본부터 보존한다.
        platform::MemoryMove(data_ + offset, suffix.Data(), suffix.ByteCount());
    }
    if (separator) {
        data_[bytes_] = '/';
    }
    bytes_ = offset + suffix.ByteCount();
    data_[bytes_] = '\0';
}

Result<void> String::TryReserve(usize byteCapacity) noexcept {
    if (byteCapacity <= capacity_) {
        return Result<void>::Success();
    }

    auto allocation = AllocateBuffer(byteCapacity);
    if (!allocation) {
        return Result<void>::Failure(allocation.Error());
    }

    char* replacement = allocation.Value();
    platform::MemoryCopy(replacement, CStr(), bytes_ + 1);
    FreeBuffer(data_);
    data_ = replacement;
    capacity_ = byteCapacity;
    return Result<void>::Success();
}

void String::Clear() noexcept {
    bytes_ = 0;
    if (data_ != nullptr) {
        data_[0] = '\0';
    }
}

void String::Swap(String& other) noexcept {
    Allocator* allocator = allocator_;
    char* data = data_;
    const usize bytes = bytes_;
    const usize capacity = capacity_;
    allocator_ = other.allocator_;
    data_ = other.data_;
    bytes_ = other.bytes_;
    capacity_ = other.capacity_;
    other.allocator_ = allocator;
    other.data_ = data;
    other.bytes_ = bytes;
    other.capacity_ = capacity;
}

Allocator& String::GetAllocator() const noexcept {
    return *allocator_;
}

const char* String::Data() const noexcept {
    return CStr();
}

usize String::ByteCount() const noexcept {
    return bytes_;
}

usize String::ByteCapacity() const noexcept {
    return capacity_;
}

bool String::IsEmpty() const noexcept {
    return bytes_ == 0;
}

StringView String::View() const noexcept {
    return StringView(CStr(), bytes_);
}

const char* String::CStr() const noexcept {
    return data_ == nullptr ? "" : data_;
}

Result<char*> String::AllocateBuffer(usize byteCapacity) noexcept {
    usize allocationBytes = 0;
    if (!TryAdd(byteCapacity, 1, allocationBytes)) {
        return Result<char*>::Failure(ErrorCode::Overflow);
    }

    auto allocation = allocator_->Allocate(allocationBytes, alignof(char));
    if (!allocation) {
        return Result<char*>::Failure(allocation.Error());
    }

    if (allocation.Value() == nullptr) {
        platform::Fatal("Allocator returned null for a nonzero string allocation");
    }

    return Result<char*>::Success(static_cast<char*>(allocation.Value()));
}

usize String::GrowthCapacity(usize required) const noexcept {
    constexpr usize maximumPayload = MaxSize - 1;
    if (capacity_ == 0) {
        return required < 16 ? 16 : required;
    }

    const usize growth = capacity_ / 2 + 1;
    const usize candidate =
        growth > maximumPayload - capacity_ ? maximumPayload : capacity_ + growth;
    return candidate < required ? required : candidate;
}

void String::FreeBuffer(char* buffer) noexcept {
    if (buffer != nullptr && !allocator_->Deallocate(buffer)) {
        platform::Fatal("String storage was not owned by its allocator");
    }
}

void String::Release() noexcept {
    FreeBuffer(data_);
    data_ = nullptr;
    bytes_ = 0;
    capacity_ = 0;
}

} // namespace rocks
