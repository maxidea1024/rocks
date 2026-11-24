#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <TextLayoutBackend/TextLayoutBackend.h>
#include <Base/PlacementNew.h>

// 글꼴 backend 시험의 공용 도우미다. ICU 데이터(IcuData 부분집합)를 프로세스 수명 버퍼에 한 번 읽어
// InitializeTextLibraries를 부른다. ICU·HarfBuzz를 쓰는 시험은 모두 먼저 이것을 부른다(할당 hook이
// 첫 사용보다 먼저 걸려야 한다 — Docs/Design/Font 6.8절).
namespace rocks::textlayouttest {

inline constexpr const char* IcuDataPath =
    "Engine/Source/ThirdParty/IcuData/icudata-79.1-rocks/icudt79l.dat";

inline bool ReadTestFile(const char* path, Array<uint8>& out) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

// 처음 부를 때 데이터를 읽어 초기화하고 결과를 기억한다. 데이터 버퍼는 프로세스 끝까지 산다.
inline bool EnsureTextLibraries() noexcept {
    alignas(SystemAllocator) static unsigned char allocatorStorage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator =
        ::new (static_cast<void*>(allocatorStorage)) SystemAllocator();
    alignas(Array<uint8>) static unsigned char dataStorage[sizeof(Array<uint8>)];
    static Array<uint8>* data = ::new (static_cast<void*>(dataStorage)) Array<uint8>(*allocator);
    static const bool ready = [] {
        if (!ReadTestFile(IcuDataPath, *data) || data->Size() == 0) {
            return false;
        }

        return static_cast<bool>(
            text::InitializeTextLibraries(Span<const uint8>(data->Data(), data->Size())));
    }();
    return ready;
}

} // namespace rocks::textlayouttest
