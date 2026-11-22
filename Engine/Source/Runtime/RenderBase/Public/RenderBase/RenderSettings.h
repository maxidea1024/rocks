#pragma once

#include <Core/Memory/SharedPtr.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/TypeKey.h>

// 프레임 packet의 렌더 설정 snapshot 참조다(Docs/Design/RenderBase.ko.md 12절, ConfigSystem.ko.md
// 5.3절 — RB1 설정 snapshot). 게임 lane이 sync 지점 뒤 얻은 "렌더 읽기" 설정 snapshot(Config
// `ConfigSnapshot`)을 프레임 slot에 담고, 렌더 lane은 그 프레임의 snapshot만 읽는다(설정 registry는
// 읽지 않는다). UE는 렌더 스레드 사본 주소에 쓰는 명령으로 전파하지만, Rocks는 불변 snapshot을
// 프레임마다 참조로 넘긴다.
//
// RenderBase는 ObjectRuntime에 의존하지 않으므로(계획 3.1절 렌더 층 "Object 없음") Config 타입을
// 직접 이름 짓지 않는다. 이 참조는 snapshot의 공유 소유권과 타입 키만 들고, 꺼낼 때 같은
// 타입으로만 돌려준다. Config를 아는 쪽(실행 파일·렌더러 모듈)이 타입을 정한다. 타입 키는
// TypeKeyOf<T>()다(Core/TypeKey.h): 담는 쪽(게임 lane)과 꺼내는 쪽(렌더러 모듈)이 다른 모듈이면
// T가 export 키를 선언해야 한다 — ConfigSnapshot은 선언한다(Docs/Design/BuildTargets.ko.md 3.8절
// BS8e).
//
// 사용 예:
//   // 게임 lane(Sync 뒤)
//   auto ticket = lane.BeginFrame();
//   lane.AttachSettings(ticket.Value(), RenderSettingsRef::Share(config.Acquire()));
//   lane.SubmitFrame(Move(ticket.Value()), packet, &DrawScene);
//   // 렌더 lane(DrawScene 안)
//   const auto* settings = context.Settings().Get<config::ConfigSnapshot>();
//   const FieldValue* immediate =
//       settings != nullptr ? settings->Find("render.graph.immediate") : nullptr;
//
// 수명: 참조가 snapshot을 붙잡는다. 프레임 slot에 담긴 참조는 그 slot이 다음 프레임에 다시 확보될
// 때(또는 lane 정지 때) 게임 lane에서 놓인다. 그래서 snapshot은 그 프레임의 GPU 완료 뒤까지 산다.
// 스레드: 참조 계수만 원자적이다. snapshot 자체는 불변이어야 한다(ConfigSnapshot이 그렇다).
namespace rocks::render {

class RenderSettingsRef {
public:
    RenderSettingsRef() noexcept = default;

    // snapshot의 공유 소유권을 담는다. T는 `uint64 Generation() const`를 가진 불변 snapshot
    // 타입이다(Config `ConfigSnapshot`). snapshot이 비어 있으면 빈 참조다.
    template <class T>
    static RenderSettingsRef Share(const SharedPtr<T>& snapshot) noexcept {
        RenderSettingsRef ref;
        if (snapshot) {
            const void* object = snapshot.Get();
            ref.owner_ = SharedPtr<const uint8>(snapshot, static_cast<const uint8*>(object));
            ref.tag_ = TypeKeyOf<RemoveConstT<T>>();
            ref.generation_ = snapshot->Generation();
        }

        return ref;
    }

    // 담은 snapshot이다. 비었거나 Share 때 타입이 T가 아니면 null이다.
    template <class T>
    const T* Get() const noexcept {
        if (!owner_ || tag_ != TypeKeyOf<RemoveConstT<T>>()) {
            return nullptr;
        }

        return static_cast<const T*>(static_cast<const void*>(owner_.Get()));
    }

    // snapshot의 세대 번호다(빈 참조는 0).
    uint64 Generation() const noexcept {
        return generation_;
    }

    bool IsValid() const noexcept {
        return static_cast<bool>(owner_);
    }

    // 참조를 놓는다.
    void Reset() noexcept {
        owner_.Reset();
        tag_ = nullptr;
        generation_ = 0;
    }

private:
    SharedPtr<const uint8> owner_;
    const uint8* tag_ = nullptr;
    uint64 generation_ = 0;
};

} // namespace rocks::render
