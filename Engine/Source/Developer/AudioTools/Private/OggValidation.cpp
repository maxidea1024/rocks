#include "OggValidation.h"

#include <Core/Containers/Array.h>
#include <Platform/Memory.h>

#include <ogg/ogg.h>

namespace rocks::audio::sourcedetail {

namespace {

// libogg sync 버퍼에 한 번에 넣는 byte 수다(원본 전체를 복사하지 않게 나눠 넣는다).
constexpr usize OggFeedBytes = usize{64} * 1024;

// ogg_sync_pageseek 결과 0은 "page를 끝내려면 data가 더 필요하다"다.
constexpr long NeedMoreData = 0;

// ogg_sync_state 수명이다.
class OggSync {
public:
    OggSync() noexcept {
        ogg_sync_init(&state_);
    }

    ~OggSync() {
        ogg_sync_clear(&state_);
    }

    OggSync(const OggSync&) = delete;
    OggSync& operator=(const OggSync&) = delete;

    ogg_sync_state* Get() noexcept {
        return &state_;
    }

private:
    ogg_sync_state state_;
};

// 지금 열린 logical stream 상태다.
struct OggStreamCursor {
    bool open = false;
    uint32 serial = 0;
    long nextPage = 0;
};

// page 하나를 stream 규칙에 맞춰 본다.
Result<void> CheckPage(const ogg_page& page, OggStreamCursor& cursor,
                       Array<uint32>& seenSerials) noexcept {
    const uint32 serial = static_cast<uint32>(ogg_page_serialno(&page));
    const long number = ogg_page_pageno(&page);
    if (!cursor.open) {
        // stream 밖에서는 새 stream의 BOS page만 올 수 있다. 같은 serial이 다시 오면 규격 위반이다.
        if (!ogg_page_bos(&page)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize index = 0; index < seenSerials.Size(); ++index) {
            if (seenSerials[index] == serial) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        if (auto added = seenSerials.TryPushBack(serial); !added) {
            return added;
        }

        cursor.open = true;
        cursor.serial = serial;
        cursor.nextPage = number;
    } else if (serial != cursor.serial) {
        // 열린 stream 안에 다른 stream의 BOS가 오면 다중화다. 그 밖은 끼어든 page라 손상이다.
        return Result<void>::Failure(ogg_page_bos(&page) ? ErrorCode::Unsupported
                                                         : ErrorCode::InvalidArgument);
    } else if (ogg_page_bos(&page)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // page 번호가 이어져야 한다(빠지거나 겹친 page).
    if (number != cursor.nextPage) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ++cursor.nextPage;
    if (ogg_page_eos(&page)) {
        cursor.open = false;
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ValidateOggPages(Span<const uint8> file, Allocator& allocator) noexcept {
    OggSync sync;
    OggStreamCursor cursor;
    Array<uint32> seenSerials(allocator);
    usize fed = 0;
    usize consumed = 0;
    while (fed < file.Size()) {
        const usize chunk = file.Size() - fed < OggFeedBytes ? file.Size() - fed : OggFeedBytes;
        char* buffer = ogg_sync_buffer(sync.Get(), static_cast<long>(chunk));
        if (buffer == nullptr) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        platform::MemoryCopy(buffer, file.Data() + fed, chunk);
        ogg_sync_wrote(sync.Get(), static_cast<long>(chunk));
        fed += chunk;
        while (true) {
            ogg_page page;
            const long result = ogg_sync_pageseek(sync.Get(), &page);
            if (result == NeedMoreData) {
                break;
            }

            // 음수는 page가 아닌 byte(쓰레기·CRC 불일치)를 건너뛴 것이다.
            if (result < 0) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            consumed += static_cast<usize>(result);
            if (auto checked = CheckPage(page, cursor, seenSerials); !checked) {
                return checked;
            }
        }
    }

    // 끝에 덜 찬 page가 남거나, stream이 EOS 없이 끝났거나(잘림), page가 하나도 없으면 손상이다.
    if (consumed != file.Size() || cursor.open || seenSerials.Size() == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks::audio::sourcedetail
