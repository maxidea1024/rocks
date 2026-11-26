#pragma once

#include "ExternalAllocation.h"

#include <rocksjpeg.h>
#include <tif_rocks.h>

// libtiff(tif_rocks)·libjpeg-turbo(jmemrocks) 할당을 이 스레드의 ExternalAllocationScope로 잇는
// 묶음이다 (Docs/Design/AssetSystem 12.10·12.11절). 두 라이브러리는 할당 함수에 user_data가 없어
// 스레드별로 묶고, 블록 머리에 할당 표를 적어 해제는 묶음과 무관하다. TIFF 디코딩은 둘을 함께
// 건다(JPEG TIFF의 libjpeg 할당). ExternalAllocationScope 안에서 쓴다.
namespace rocks::image::detail {

inline const RocksTiffAllocator tiffAllocator = {ExternalAllocate, ExternalReallocate,
                                                 ExternalFree};
inline const RocksJpegAllocator jpegAllocator = {ExternalAllocate, ExternalFree};

// libtiff 할당 묶음이다(TIFFClose까지 덮어야 새 할당이 된다).
class TiffAllocatorBinding {
public:
    TiffAllocatorBinding() noexcept : previous_(RocksTiffBindAllocator(&tiffAllocator)) {
    }

    ~TiffAllocatorBinding() noexcept {
        RocksTiffBindAllocator(previous_);
    }

    TiffAllocatorBinding(const TiffAllocatorBinding&) = delete;
    TiffAllocatorBinding& operator=(const TiffAllocatorBinding&) = delete;

private:
    const RocksTiffAllocator* previous_;
};

// libjpeg-turbo 할당 묶음이다.
class JpegAllocatorBinding {
public:
    JpegAllocatorBinding() noexcept : previous_(RocksJpegBindAllocator(&jpegAllocator)) {
    }

    ~JpegAllocatorBinding() noexcept {
        RocksJpegBindAllocator(previous_);
    }

    JpegAllocatorBinding(const JpegAllocatorBinding&) = delete;
    JpegAllocatorBinding& operator=(const JpegAllocatorBinding&) = delete;

private:
    const RocksJpegAllocator* previous_;
};

} // namespace rocks::image::detail
