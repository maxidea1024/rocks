#pragma once

namespace rocks {

// Core 동시 컨테이너의 단계 정책이다(Docs/Design/Synchronization 12.1·14.3절, D24). 컨테이너는 원자
// 연산 사이에서 StepPolicy::Step("이름")을 부른다. 제품은 이 기본 정책(빈 inline 함수, 비용 0)을
// 쓰고, 시험은 다른 정책 타입으로 별도 instantiation을 만들어 interleaving을 정한 순서로 돌린다
// (정의가 번역 단위마다 달라지지 않으므로 ODR 위반이 없다).
//
// 사용 예(시험 정책):
//   struct TestSteps { static void Step(const char* name) noexcept { scheduler.Reached(name); } };
//   BoundedSpscQueue<uint32, TestSteps> queue;
struct NoSteps {
    static void Step(const char*) noexcept {
    }
};

} // namespace rocks
