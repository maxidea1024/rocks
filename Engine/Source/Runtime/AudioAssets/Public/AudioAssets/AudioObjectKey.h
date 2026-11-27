#pragma once

#include <Core/Hash.h>
#include <Object/ObjectHandle.h>

// 재생 스냅숏의 동시 발음 키(Docs/Design/AudioSystem 3.41.2절)다. 객체 handle 전체(Registry·slot·
// generation)의 실행 중 해시라 World·Registry 사이에도 겹치지 않는다(UE UObject::GetUniqueID 대응).
// 0은 "키 없음"이라 피한다.
namespace rocks {

inline uint64 AudioObjectKey(ObjectHandle handle) noexcept {
    const uint64 key = Hash<ObjectHandle>{}(handle);
    return key != 0 ? key : 1;
}

} // namespace rocks
