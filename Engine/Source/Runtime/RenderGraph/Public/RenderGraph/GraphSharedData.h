#pragma once

#include <Base/TypeTraits.h>
#include <Core/Move.h>
#include <RenderGraph/GraphTypes.h>

// 그래프 공유 값(GraphSharedData)과 view 키
// 표(GraphViewTable)다(Docs/Design/RenderBase.ko.md 7.5절, UE FRDGBlackboard 대응). 그래프 하나
// 안에서 pass를 만드는 기능 함수들이 장면 텍스처 묶음 같은 값을 넘겨받는 자리다.
namespace rocks::render {

class RenderGraph;

// 그래프 공유 값 타입의 등록이다. 정의가 없는 기본 template이므로 등록하지 않은 타입을
// GraphSharedData에 쓰면 컴파일 오류다. ROCKS_GRAPH_SHARED_DATA로 특수화한다.
template <class T>
struct GraphSharedDataType;

namespace detail {

// 등록 타입마다 프로세스 안 번호를 하나 준다(그 타입의 Index를 처음 부를 때, 스레드 안전).
uint32 AllocateGraphSharedDataIndex() noexcept;

} // namespace detail

// 타입을 그래프 공유 값으로 등록한다. 전역 namespace에서 쓴다(타입 정의 뒤, 한 타입에 한 번).
//
// 사용 예:
//   struct ShadowSetup { GraphTexture atlas; };
//   ROCKS_GRAPH_SHARED_DATA(ShadowSetup);
#define ROCKS_GRAPH_SHARED_DATA(Type)                                                              \
    template <>                                                                                    \
    struct rocks::render::GraphSharedDataType<Type> {                                              \
        static const char* Name() noexcept {                                                       \
            return #Type;                                                                          \
        }                                                                                          \
                                                                                                   \
        static ::rocks::uint32 Index() noexcept {                                                  \
            static const ::rocks::uint32 index =                                                   \
                ::rocks::render::detail::AllocateGraphSharedDataIndex();                           \
            return index;                                                                          \
        }                                                                                          \
    }

// 그래프 공유 값이다: 등록 타입마다 값 하나, 그래프 수명이다(값은 그래프의 프레임 arena에 만들고
// 그래프가 파괴될 때 렌더 lane에서 소멸자를 부른다). RenderGraph::SharedData()로 얻는다.
//
// 규칙(7.5절):
// - 이미 있는 타입을 Create하면 검증 오류(그래프 전체를 버린다)이고 null이다.
// - 반드시 있어야 하는 조회(Require)에 값이 없으면 검증 오류이고 null이다. 있을 때만 쓰는
//   조회(Find)는 검증 오류 없이 null이다.
// - 값에는 그 그래프의 handle만 담는다. 다른 그래프의 handle은 그것을 쓰는 pass 선언에서 그래프
//   검증이 거부한다.
//
// 사용 예:
//   auto* setup = graph.SharedData().Create<ShadowSetup>();
//   setup->atlas = graph.CreateTexture(atlasDesc, "ShadowAtlas").Value();
//   ... 다른 기능 함수에서:
//   const ShadowSetup* shadows = graph.SharedData().Require<ShadowSetup>();
//
// 스레드: 그래프와 같은 스레드(렌더 lane) 전용이다.
class GraphSharedData {
public:
    GraphSharedData(const GraphSharedData&) = delete;
    GraphSharedData& operator=(const GraphSharedData&) = delete;

    // T 값을 args로 만든다. 실패(이미 있음 검증 오류, arena 할당 실패)하면 null이다.
    template <class T, class... Args>
    T* Create(Args&&... args) noexcept {
        static_assert(IsNothrowConstructibleV<T, Args&&...>,
                      "graph shared data must be nothrow constructible");
        static_assert(IsNothrowDestructibleV<T>, "graph shared data must be nothrow destructible");
        using Type = GraphSharedDataType<T>;
        void* memory = Allocate(Type::Index(), Type::Name(), sizeof(T), alignof(T),
                                [](void* value) noexcept { static_cast<T*>(value)->~T(); });
        return memory != nullptr ? InitializeAt<T>(memory, Forward<Args>(args)...) : nullptr;
    }

    // 있으면 값, 없으면 null이다(검증 오류 없음).
    template <class T>
    T* Find() const noexcept {
        return static_cast<T*>(Lookup(GraphSharedDataType<T>::Index()));
    }

    // 있어야 하는 값이다. 없으면 검증 오류를 남기고 null이다.
    template <class T>
    T* Require() noexcept {
        using Type = GraphSharedDataType<T>;
        return static_cast<T*>(Required(Type::Index(), Type::Name()));
    }

private:
    explicit GraphSharedData(RenderGraph& graph) noexcept : graph_(graph) {
    }

    ~GraphSharedData() noexcept = default;

    // 값 하나의 자리다.
    struct Entry {
        uint32 index = 0;
        void* value = nullptr;
        void (*destroy)(void*) noexcept = nullptr;
    };

    void* Allocate(uint32 index, const char* name, usize bytes, usize alignment,
                   void (*destroy)(void*) noexcept) noexcept;
    void* Lookup(uint32 index) const noexcept;
    void* Required(uint32 index, const char* name) noexcept;
    // 만든 역순으로 소멸자를 부른다(RenderGraph 파괴).
    void DestroyAll() noexcept;

    RenderGraph& graph_;
    Entry* entries_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;

    friend class RenderGraph;
};

// (ViewSet 번호, view 번호) 키다(7.5절 SceneTextureTable 키). ViewSet 단위 값은 view를 0으로 둔다.
struct GraphViewKey {
    uint32 viewSet = 0;
    uint32 view = 0;

    bool operator==(const GraphViewKey&) const noexcept = default;
};

// GraphViewTable의 형식 지운 저장소다(값은 자명하게 복사되는 byte로 그래프 프레임 arena에 둔다).
class GraphViewTableStorage {
public:
    GraphViewTableStorage(const GraphViewTableStorage&) = delete;
    GraphViewTableStorage& operator=(const GraphViewTableStorage&) = delete;

    usize Size() const noexcept {
        return size_;
    }

protected:
    GraphViewTableStorage(RenderGraph& graph, const char* name, usize bytes,
                          usize alignment) noexcept
        : graph_(graph), name_(name), bytes_(bytes), alignment_(alignment) {
    }

    ~GraphViewTableStorage() noexcept = default;

    // key 자리를 새로 만든다. 이미 있으면 검증 오류이고 null, 할당 실패도 null이다(오류 코드는
    // error).
    void* Insert(GraphViewKey key, ErrorCode& error) noexcept;
    // key 자리다. 없으면 null이고, required면 검증 오류를 남긴다.
    const void* Lookup(GraphViewKey key, bool required) const noexcept;

private:
    RenderGraph& graph_;
    const char* name_;
    usize bytes_;
    usize alignment_;
    GraphViewKey* keys_ = nullptr;
    uint8* values_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

// (ViewSet, view) 키로 값을 찾는 그래프 수명 표다(7.5절). 렌더러는 packet 전체의 장면 텍스처를
// SceneTextureTable(= GraphViewTable<SceneTextureSet>) 하나로 GraphSharedData에 둔다: 타입마다 값
// 하나 규칙 아래에서 ViewSet 여럿·view 여럿을 담기 위해서다. ViewSet 단위 값(GPU Scene 자원·공유
// 그림자)도 같은 방식이다(view 0). Value는 자명하게 복사되는 값(그래프 handle 묶음)이다.
//
// 규칙: 같은 키를 두 번 Add하면 검증 오류, 없는 키의 Get(장면 텍스처를 만들기 전에 꺼냄)도 검증
// 오류다. Find는 검증 오류 없이 null이다.
//
// 사용 예(렌더러):
//   using SceneTextureTable = GraphViewTable<SceneTextureSet>;
//   ROCKS_GRAPH_SHARED_DATA(SceneTextureTable);   // 전역 namespace
//   auto* table = graph.SharedData().Create<SceneTextureTable>(graph, "SceneTextureTable");
//   table->Add({viewSet, view}, sceneTextures);
//   ... pass 구성 함수에서:
//   const SceneTextureSet* textures =
//       graph.SharedData().Require<SceneTextureTable>()->Get({viewSet, view});
template <class Value>
class GraphViewTable final : public GraphViewTableStorage {
    static_assert(IsTriviallyCopyableV<Value> && IsTriviallyDestructibleV<Value>,
                  "graph view table values must be trivially copyable and destructible");

public:
    // name은 정적 수명 문자열(진단)이다.
    GraphViewTable(RenderGraph& graph, const char* name) noexcept
        : GraphViewTableStorage(graph, name, sizeof(Value), alignof(Value)) {
    }

    // key의 값을 넣는다. 실패: 이미 있는 키 InvalidArgument(검증 오류), arena 할당 실패는 그
    // 오류다.
    Result<void> Add(GraphViewKey key, const Value& value) noexcept {
        ErrorCode error = ErrorCode::None;
        void* slot = Insert(key, error);
        if (slot == nullptr) {
            return Result<void>::Failure(error);
        }

        InitializeAt<Value>(slot, value);
        return Result<void>::Success();
    }

    // key의 값이다. 없으면 검증 오류를 남기고 null이다.
    const Value* Get(GraphViewKey key) const noexcept {
        return static_cast<const Value*>(Lookup(key, true));
    }

    // key의 값이다. 없으면 null이다(검증 오류 없음).
    const Value* Find(GraphViewKey key) const noexcept {
        return static_cast<const Value*>(Lookup(key, false));
    }
};

} // namespace rocks::render
