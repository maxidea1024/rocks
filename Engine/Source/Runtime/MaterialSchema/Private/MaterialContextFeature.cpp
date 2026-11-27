#include <MaterialSchema/MaterialContextFeature.h>

// 문맥 공급 기능 이름 표다(Docs/Design/MaterialSystem 19.3절).
namespace rocks::material {

namespace {

struct FeatureNames {
    StringView define;
    StringView key;
};

// MaterialContextFeature 값 순서와 같다.
constexpr FeatureNames FeatureNameRows[MaterialContextFeatureCount] = {
    {"VIEW", "view"},
    {"VERTEX", "vertex"},
    {"PRIMITIVE", "primitive"},
    {"SKIN", "skin"},
    {"LIGHTMAP", "lightmap"},
    {"LIGHT", "light"},
    {"SKY", "sky"},
    {"POST_PROCESS", "post"},
    {"SCENE", "scene"},
    {"DECAL", "decal"},
    {"PARTICLE", "particle"},
    {"DISTANCE_FIELD", "distancefield"},
    {"HAIR", "hair"},
    {"WATER", "water"},
    {"VOLUME", "volume"},
    {"RUNTIME_VIRTUAL_TEXTURE", "rvt"},
    {"MESH_PAINT", "meshpaint"},
    {"PHYSICS_FIELD", "physicsfield"},
    {"SPEED_TREE", "speedtree"},
    {"NEURAL", "neural"},
    {"FIRST_PERSON", "firstperson"},
    {"XR", "xr"},
    {"PATH_TRACING", "pathtracing"},
    {"USER_INTERFACE", "ui"},
};

static_assert(static_cast<uint32>(MaterialContextFeature::UserInterface) + 1 ==
                  MaterialContextFeatureCount,
              "MaterialContextFeatureCount must follow the last MaterialContextFeature value");

} // namespace

StringView MaterialContextFeatureDefineName(MaterialContextFeature feature) noexcept {
    const auto index = static_cast<uint32>(feature);
    return index < MaterialContextFeatureCount ? FeatureNameRows[index].define : StringView();
}

StringView MaterialContextFeatureKey(MaterialContextFeature feature) noexcept {
    const auto index = static_cast<uint32>(feature);
    return index < MaterialContextFeatureCount ? FeatureNameRows[index].key : StringView();
}

} // namespace rocks::material
