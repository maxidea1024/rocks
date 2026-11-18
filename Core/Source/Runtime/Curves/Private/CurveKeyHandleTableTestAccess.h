#pragma once

#include <Base/Types.h>
#include <Curves/CurveKeyHandleTable.h>

// 시험이 키 핸들 표의 발급 카운터를 uint32 끝 근처로 옮기는 모듈 내부 접근점이다(핸들 40억 개를
// 발급하지 않고 Overflow 경계를 시험한다). 공개 API에는 카운터를 지정하는 길을 두지 않는다(핸들이
// 표 안에서 다시 쓰이면 옛 핸들이 다른 키를 가리킨다). Tests/ 소스만 include한다(정책 검사
// test-access-include).
namespace rocks::curves_detail {

struct CurveKeyHandleTableTestAccess {
    template <class Curve>
    static void SetLastIssued(CurveKeyHandleTable<Curve>& table, uint32 lastIssued) noexcept {
        table.lastIssued_ = lastIssued;
    }
};

} // namespace rocks::curves_detail
