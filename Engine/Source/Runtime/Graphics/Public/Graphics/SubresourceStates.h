#pragma once

#include <Base/NumericLimits.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Graphics/ResourceAccess.h>
#include <Platform/Fatal.h>

namespace rocks::graphics {

// 텍스처 subresource(mip × slice)마다 상태 하나를 두는 표다(backend의 자원 상태 추적, RenderBase
// GX5). index는 slice × mipLevels + mip이다(D3D subresource 순서에서 plane을 뺀 것). 칸이
// InlineCapacity 이하면(slice 하나인 텍스처의 mip 전체) 객체 안에 두고, 넘으면(배열·cube)
// allocator에서 한 번 받는다. 기본 생성은 빈 표다. 이동만 된다.
//
// 사용 예(D3D12 기록):
//   SubresourceStates<D3D12_RESOURCE_STATES> committed;
//   if (!committed.Reset(allocator, mipLevels, slices, D3D12_RESOURCE_STATE_COPY_DEST)) { ... }
//   committed.At(mip, slice) = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
//
// 스레드: 동기화하지 않는다(장치 잠금 안에서 쓴다).
template <class State>
class SubresourceStates {
public:
    // 객체 안에 두는 칸 수다(backend MaxMipLevels와 같다).
    static constexpr uint32 InlineCapacity = 16;

    SubresourceStates() noexcept = default;

    SubresourceStates(const SubresourceStates&) = delete;
    SubresourceStates& operator=(const SubresourceStates&) = delete;

    SubresourceStates(SubresourceStates&& other) noexcept {
        MoveFrom(other);
    }

    SubresourceStates& operator=(SubresourceStates&& other) noexcept {
        if (this != &other) {
            Release();
            MoveFrom(other);
        }
        return *this;
    }

    ~SubresourceStates() noexcept {
        Release();
    }

    // mipLevels × slices 칸을 모두 initial로 둔다. 실패: 0 크기 InvalidArgument, 할당 실패
    // allocator 오류(표는 빈 채로 남는다).
    Result<void> Reset(Allocator& allocator, uint32 mipLevels, uint32 slices,
                       State initial) noexcept {
        Release();
        const uint64 count = uint64{mipLevels} * slices;
        if (count == 0 || count > NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        State* storage = inline_;
        if (count > InlineCapacity) {
            auto memory =
                allocator.Allocate(static_cast<usize>(count) * sizeof(State), alignof(State));
            if (!memory) {
                return Result<void>::Failure(memory.Error());
            }

            storage = static_cast<State*>(memory.Value());
            allocator_ = &allocator;
        }

        for (uint64 index = 0; index < count; ++index) {
            storage[index] = initial;
        }

        heap_ = storage == inline_ ? nullptr : storage;
        mipLevels_ = mipLevels;
        slices_ = slices;
        return Result<void>::Success();
    }

    uint32 MipLevels() const noexcept {
        return mipLevels_;
    }

    uint32 Slices() const noexcept {
        return slices_;
    }

    uint32 Count() const noexcept {
        return mipLevels_ * slices_;
    }

    static constexpr uint32 Index(uint32 mip, uint32 slice, uint32 mipLevels) noexcept {
        return slice * mipLevels + mip;
    }

    State& At(uint32 mip, uint32 slice) noexcept {
        return (*this)[Index(mip, slice, mipLevels_)];
    }

    const State& At(uint32 mip, uint32 slice) const noexcept {
        return (*this)[Index(mip, slice, mipLevels_)];
    }

    State& operator[](uint32 index) noexcept {
        if (index >= Count()) {
            platform::Fatal("SubresourceStates index out of range");
        }
        return Data()[index];
    }

    const State& operator[](uint32 index) const noexcept {
        if (index >= Count()) {
            platform::Fatal("SubresourceStates index out of range");
        }
        return Data()[index];
    }

    // other의 칸을 모두 복사한다(크기가 같아야 한다, 다르면 Fatal).
    void CopyFrom(const SubresourceStates& other) noexcept {
        if (other.Count() != Count()) {
            platform::Fatal("SubresourceStates size mismatch");
        }
        for (uint32 index = 0; index < Count(); ++index) {
            Data()[index] = other.Data()[index];
        }
    }

private:
    State* Data() noexcept {
        return heap_ != nullptr ? heap_ : inline_;
    }

    const State* Data() const noexcept {
        return heap_ != nullptr ? heap_ : inline_;
    }

    void Release() noexcept {
        if (heap_ != nullptr) {
            auto freed = allocator_->Deallocate(heap_);
            (void)freed;
        }
        heap_ = nullptr;
        allocator_ = nullptr;
        mipLevels_ = 0;
        slices_ = 0;
    }

    void MoveFrom(SubresourceStates& other) noexcept {
        for (uint32 index = 0; index < InlineCapacity; ++index) {
            inline_[index] = other.inline_[index];
        }
        heap_ = other.heap_;
        allocator_ = other.allocator_;
        mipLevels_ = other.mipLevels_;
        slices_ = other.slices_;
        other.heap_ = nullptr;
        other.allocator_ = nullptr;
        other.mipLevels_ = 0;
        other.slices_ = 0;
    }

    State inline_[InlineCapacity] = {};
    State* heap_ = nullptr;
    Allocator* allocator_ = nullptr;
    uint32 mipLevels_ = 0;
    uint32 slices_ = 0;
};

} // namespace rocks::graphics
