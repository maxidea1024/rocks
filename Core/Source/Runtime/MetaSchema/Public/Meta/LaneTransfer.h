#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Core/Result.h>
#include <Meta/GeneratedType.h>

// 게임 lane에서 다른 lane(렌더 lane 등 전용 lane)으로 넘기는 값의 "lane 전달 가능" 표시다
// (Docs/Design/RenderBase 5.2절 "전달 가능한 타입", 계획 8.1절 Render lane "Game 주소 없음").
// 렌더 lane 작업(RenderBase RenderLane::SubmitFrame·PostResource·PostSurface)과 UI 장면 추출
// 게시처럼 게임 lane 밖에서 소비되는 값을 받는 API는 이 표시를 명시 등록한 값 타입만 받는다.
// 등록하지 않은 타입을 넘기면 컴파일 오류다(제약을 만족하는 overload가 없다). 장치·그래프를 모르는
// 모듈(RenderData·UiCore 등)도 같은 규칙을 쓰도록 MetaSchema(헤더 전용)에 둔다.
//
// 사용 예(전역 이름공간에서 등록한다):
//   namespace game {
//   struct SpritePacket {
//       render::RenderHandle texture;
//       Mat4 world;
//   };
//   }
//   ROCKS_LANE_TRANSFERABLE(game::SpritePacket);
//
//   template <LaneTransferable T>
//   Result<void> Post(T&& value) noexcept {
//       auto checked = CheckLaneTransfer<T>();   // Development 스키마 검사
//       if (!checked) { return checked; }
//       ...
//   }
//
// 등록 규칙:
// - 값은 받는 lane이 소유하게 된다(이동으로 넘긴다). 게임 객체 주소·Object 참조·게임 lane이 계속
//   쓰는 메모리를 가리키는 포인터를 담지 않는다. 가변 길이 목록은 받는 쪽이 정한 프레임 수명
//   메모리(렌더 lane은 그 프레임 slot의 RenderLane::Arena)에 두고 Span으로 가리킨다.
// - nothrow 이동 생성·nothrow 소멸이어야 하고 참조·포인터 타입 자체는 등록할 수 없다(컴파일 오류).
// - 강제 범위: C++ 타입 검사로는 trivially copyable 구조체 안의 원시 포인터를 걸러낼 수 없다. 등록
//   타입이 리플렉션 스키마를 가지면(생성 헤더를 포함한 번역 단위에서 게시할 때) Development·Debug
//   구성에서 Object 참조(ObjectRef·ObjectRefArray·ObjectRefMap)·WeakObjectRef 필드(컨테이너 원소·
//   값 포함)나 이벤트 필드(Event<Args...> — 구독 대상·발행자를 붙잡는다)가 있으면
//   CheckLaneTransfer가 InvalidArgument를 돌려준다. 중첩 struct 안의 필드와 Map 키는 보지 않는다.
//   이 두 단계가 강제할 수 있는 범위다.
//
// UE 5.8에는 직접 대응이 없다. 가장 가까운 것은 렌더 명령 lambda(ENQUEUE_RENDER_COMMAND)의 캡처
// 관례인데, UE는 무엇을 캡처하는지 컴파일·실행 중에 검사하지 않는다(게임 소유 객체 포인터 캡처도
// 허용된다 — RenderBase 1절 결정표 "게임 → 렌더 전달").
namespace rocks {

// 등록 표지다. ROCKS_LANE_TRANSFERABLE이 특수화한다. 직접 특수화하지 않는다.
template <class T>
inline constexpr bool LaneTransferRegistered = false;

// lane 사이로 넘길 수 있는 값 타입이다(위 등록 규칙).
template <class T>
concept LaneTransferable = LaneTransferRegistered<T> && !IsReferenceV<T> && !IsPointerV<T> &&
                           IsNothrowMoveConstructibleV<T> && IsNothrowDestructibleV<T>;

namespace detail {

// 스키마가 lane 전달을 허용하면 true다: Object 참조·WeakObjectRef 필드(컨테이너 원소·값 포함)와
// 이벤트 필드(IsEventField)가 없다. 중첩 struct 안과 Map 키는 보지 않는다.
//
// 사용 예:
//   static_assert(detail::LaneTransferSchemaAllowed(GeneratedType<Packet>::Schema()));
constexpr bool LaneTransferSchemaAllowed(const TypeSchema& schema) noexcept {
    for (const FieldSchema& field : schema.fields) {
        // 이벤트 필드는 강한 참조가 아니지만 구독(대상 객체·함수)과 발행자를 붙잡는다 — 받는 lane이
        // 게임 객체를 만지게 되므로 거부한다(Docs/Design/RenderBase 5.2절 "전달 가능한 타입").
        if (field.kind == FieldKind::ObjectReference || IsEventField(field)) {
            return false;
        }

        // 컨테이너면 원소·값 형식, 아니면 값 형식이다(Array<WeakObjectRef>·OrderedMap<K,
        // WeakObjectRef> 포함).
        const ValueKind target = FieldTargetType(field).kind;
        if (field.value.kind == ValueKind::ObjectReference ||
            field.value.kind == ValueKind::WeakObjectReference ||
            target == ValueKind::ObjectReference || target == ValueKind::WeakObjectReference) {
            return false;
        }
    }

    return true;
}

} // namespace detail

// 등록 타입 T의 Development 검사다(위 "강제 범위"). 리플렉션 스키마가 없거나 Release 구성이면
// 언제나 성공이다. 실패는 InvalidArgument다. 어느 스레드에서나 부를 수 있다. 리플렉션 타입인데 이
// 번역 단위에 생성 헤더가 보이지 않으면 검사를 건너뛰지 않고 컴파일 오류다.
template <LaneTransferable T>
Result<void> CheckLaneTransfer() noexcept {
    if constexpr (::rocks::detail::GeneratedHeaderVisible<T>() && !IsReleaseBuild &&
                  requires { GeneratedType<T>::Schema(); }) {
        if (!detail::LaneTransferSchemaAllowed(GeneratedType<T>::Schema())) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks

// T를 lane 전달 가능으로 등록한다. 전역 이름공간에서 한 번 쓴다(위 등록 규칙).
#define ROCKS_LANE_TRANSFERABLE(Type)                                                              \
    template <>                                                                                    \
    inline constexpr bool ::rocks::LaneTransferRegistered<Type> = true
