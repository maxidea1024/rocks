#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Optional.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>

namespace rocks::reflection_fixture {

// 바이트 배열 fixture다(Docs/Design/CurveData.ko.md D18 — CV4 `Array<uint8>` Bytes 블록 wire,
// BytesGraphTests·TextBytesTests). BytesBlob은 struct 멤버 자리, BytesHolder는 클래스
// 필드(MaxElements = MaxArrayElements· MaxElements 8)·struct 멤버·struct 원소 배열 안 멤버·정적
// 배열 `uint8 x[4]`(ValueArray 그대로)와 Optional·Map 값·InstancedStruct 내용 안 struct의 멤버를
// 담는다.
ROCKS_STRUCT()
struct BytesBlob {
    explicit BytesBlob(Allocator& allocator) noexcept : data(allocator) {
    }

    ROCKS_PROPERTY() Array<uint8> data;
    ROCKS_PROPERTY() uint32 tag = 0;
};

ROCKS_CLASS()
class BytesHolder : public Object {
public:
    explicit BytesHolder(Allocator& allocator) noexcept
        : payload(allocator), small(allocator), blob(allocator), blobs(allocator),
          byKey(allocator) {
    }

    ROCKS_PROPERTY(MaxElements = 1048576) Array<uint8> payload;
    ROCKS_PROPERTY(MaxElements = 8) Array<uint8> small;
    ROCKS_PROPERTY() BytesBlob blob;
    ROCKS_PROPERTY() Array<BytesBlob> blobs;
    ROCKS_PROPERTY() uint8 fixed[4] = {};
    ROCKS_PROPERTY() int32 plain = 0;
    ROCKS_PROPERTY() Optional<BytesBlob> maybe;
    ROCKS_PROPERTY() OrderedMap<int32, BytesBlob> byKey;
    ROCKS_PROPERTY() InstancedStruct<> any;
};

} // namespace rocks::reflection_fixture
