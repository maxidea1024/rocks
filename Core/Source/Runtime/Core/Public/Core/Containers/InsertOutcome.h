#pragma once

#include <Base/Types.h>

namespace rocks {

// 키 컨테이너(HashMap·HashSet·OrderedMap·OrderedSet) 삽입의 결과다. Result의 성공·실패와 따로,
// 새 항목을 만들었는지(Inserted) 같은 키가 이미 있었는지(AlreadyPresent)를 알린다.
enum class InsertOutcome : uint8 {
    Inserted,
    AlreadyPresent
};

// value는 컨테이너가 소유한 빌린 포인터다. 재해시·삭제 등의 무효화 규약을 지킨다.
template <typename V>
struct FindOrAddResult {
    V* value;
    InsertOutcome outcome;
};

} // namespace rocks
