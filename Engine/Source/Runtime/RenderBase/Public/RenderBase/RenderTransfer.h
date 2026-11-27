#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Core/Result.h>
#include <Meta/GeneratedType.h>

// 게임 lane → 렌더 lane으로 넘기는 값의 "렌더 전달 가능" 표시다(Docs/Design/RenderBase 5.2절,
// 계획 8.1절 Render lane "Game 주소 없음"). 렌더 lane 작업(RenderLane::SubmitFrame·PostResource·
// PostSurface)은 이 표시를 명시 등록한 값 타입만 받는다. 등록하지 않은 타입을 넘기면 컴파일 오류다
// (작업 함수의 제약을 만족하는 overload가 없다).
//
// 사용 예(전역 이름공간에서 등록한다):
//   namespace game {
//   struct SpritePacket {
//       render::RenderHandle texture;
//       Mat4 world;
//   };
//   }
//   ROCKS_RENDER_TRANSFERABLE(game::SpritePacket);
//
// 등록 규칙:
// - 값은 렌더 lane이 소유하게 된다(이동으로 넘긴다). 게임 객체 주소·Object 참조·게임 lane이 계속
//   쓰는 메모리를 가리키는 포인터를 담지 않는다. 가변 길이 목록은 그 프레임 slot의 프레임
//   arena(RenderLane::Arena)에 두고 Span으로 가리킨다. 렌더 lane이 소유하는 byte는 RenderBlob을
//   쓴다.
// - nothrow 이동 생성·nothrow 소멸이어야 하고 참조·포인터 타입 자체는 등록할 수 없다(컴파일 오류).
// - 강제 범위: C++ 타입 검사로는 trivially copyable 구조체 안의 원시 포인터를 걸러낼 수 없다. 등록
//   타입이 리플렉션 스키마를 가지면(생성 헤더를 포함한 번역 단위에서 작업을 게시할 때) Development·
//   Debug 구성에서 Object 참조(ObjectRef·ObjectRefArray·ObjectRefMap)·WeakObjectRef 필드나 이벤트
//   필드(Event<Args...> — 구독 대상·발행자를 붙잡는다)가 있으면 게시를 InvalidArgument로 거부한다.
//   중첩 struct 안의 필드는 보지 않는다. 이 두 단계가 강제할 수 있는 범위다.
namespace rocks::render {

// 등록 표지다. ROCKS_RENDER_TRANSFERABLE이 특수화한다. 직접 특수화하지 않는다.
template <class T>
inline constexpr bool RenderTransferRegistered = false;

// 렌더 lane 작업이 받을 수 있는 값 타입이다.
template <class T>
concept RenderTransferable = RenderTransferRegistered<T> && !IsReferenceV<T> && !IsPointerV<T> &&
                             IsNothrowMoveConstructibleV<T> && IsNothrowDestructibleV<T>;

namespace detail {

// 스키마에 Object 참조·WeakObjectRef 필드와 이벤트 필드(IsEventField)가 없으면 true다(중첩 struct
// 안은 보지 않는다).
ROCKS_RENDERBASE_API bool SchemaHasNoObjectReferences(const TypeSchema& schema) noexcept;

} // namespace detail

// 등록 타입 T의 Development 검사다(위 "강제 범위"). 리플렉션 스키마가 없거나 Release 구성이면
// 언제나 성공이다. 실패는 InvalidArgument다. 어느 스레드에서나 부를 수 있다.
template <RenderTransferable T>
Result<void> CheckRenderTransfer() noexcept {
    // 관문: 리플렉션 타입인데 생성 헤더가 보이지 않으면 검사를 건너뛰지 않고 컴파일 오류를 낸다.
    if constexpr (::rocks::detail::GeneratedHeaderVisible<T>() && !IsReleaseBuild &&
                  requires { GeneratedType<T>::Schema(); }) {
        if (!detail::SchemaHasNoObjectReferences(GeneratedType<T>::Schema())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::render

// T를 렌더 전달 가능으로 등록한다. 전역 이름공간에서 한 번 쓴다(위 등록 규칙).
#define ROCKS_RENDER_TRANSFERABLE(Type)                                                            \
    template <>                                                                                    \
    inline constexpr bool ::rocks::render::RenderTransferRegistered<Type> = true
