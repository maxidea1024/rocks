using Rocks.MetaCompiler;

namespace Rocks.ToolTests;

// 각 테스트는 실제 도구와 동일한 읽기 전용 명세를 제공한다. 테스트 데이터의 ID는
// 저작 API가 한 번 발급하며 선언 본문에 GUID를 직접 넣지 않는다.
internal static class MetaProbeIdentity
{
    public static void Create(string path)
    {
        StableIdManifest.Update(path, true, manifest =>
        {
            foreach (string name in new[] { "probe::Value", "probe::Outer::Inner", "probe::Object", "probe::Parent",
                "first::Good", "example::Invalid", "a::Thing", "b::Thing", "a::ThingExtended", "probe::Mode", "probe::Value::Nested",
                "probe::Leaf", "probe::Middle", "probe::Holder", "probe::Outer", "probe::WorldBits", "probe::WorldValue", "probe::Box",
                "probe::Zeta", "probe::Alpha", "probe::Vec3Binding", "probe::Vec3dBinding", "probe::PairBinding", "probe::OtherBinding",
                "probe::RealBinding", "probe::ABinding", "probe::BBinding", "probe::MixedBinding", "probe::HiddenBinding",
                "probe::FixedBinding", "probe::SplitBinding", "probe::Bundle", "probe::WorldBundle", "probe::Movable", "probe::Node", "probe::MovableBundle",
                "probe::Tiny", "probe::Signed", "probe::Short", "probe::Wide", "probe::Huge", "probe::Bits",
                "probe::Point", "probe::Segment", "probe::Plain", "probe::Derived", "probe::Other", "probe::none",
                "probe::Target", "probe::Slot", "probe::Host" }
                .Concat(Enumerable.Range(1, 17).Select(level => $"probe::Chain{level}")))
            {
                manifest.Register("type", name);
            }

            foreach (string name in new[] { "probe::Value::count", "probe::Value::next", "probe::Value::other", "probe::Object::value",
                "probe::Value::mode", "probe::Value::nested",
                "example::Invalid::valid", "example::Invalid::bad",
                "probe::Leaf::count", "probe::Leaf::label", "probe::Leaf::mode", "probe::Middle::leaf", "probe::Middle::offset",
                "probe::Holder::target", "probe::Holder::middle", "probe::Holder::flag", "probe::Outer::holder", "probe::Outer::inner",
                "probe::WorldBits::value", "probe::Value::holder", "probe::Value::plain", "probe::Value::extra", "probe::Value::bits",
                "probe::Value::leaf", "probe::WorldValue::bits", "probe::Box::count", "probe::Chain1::value",
                "probe::Zeta::v", "probe::Alpha::x", "probe::Value::alpha",
                "probe::Vec3Binding::x", "probe::Vec3Binding::y", "probe::Vec3Binding::z", "probe::Vec3Binding::a", "probe::Vec3Binding::count",
                "probe::Vec3dBinding::x", "probe::Vec3dBinding::y", "probe::Vec3dBinding::z", "probe::PairBinding::a", "probe::PairBinding::b",
                "probe::OtherBinding::x", "probe::OtherBinding::y", "probe::OtherBinding::z", "probe::RealBinding::inner", "probe::ABinding::inner",
                "probe::BBinding::inner", "probe::MixedBinding::a", "probe::MixedBinding::b", "probe::HiddenBinding::a",
                "probe::Value::position", "probe::Value::pair", "probe::FixedBinding::v", "probe::SplitBinding::x", "probe::SplitBinding::y",
                "probe::SplitBinding::z",
                "probe::Leaf::codes", "probe::Holder::weights", "probe::Bundle::refs", "probe::Bundle::holders", "probe::WorldBundle::inner",
                "probe::WorldValue::bundle", "probe::Movable::target", "probe::Node::children", "probe::Node::target",
                "probe::Value::ints", "probe::Value::names", "probe::Value::modes", "probe::Value::softs", "probe::Value::leaves",
                "probe::Value::refs", "probe::Value::anyRefs", "probe::Value::holders", "probe::Value::bundle", "probe::Value::boxes",
                "probe::Value::cache", "probe::Value::values", "probe::Value::bundles", "probe::Value::movables",
                "probe::Value::points", "probe::Box::refs", "probe::MovableBundle::refs",
                "probe::Value::tiny", "probe::Value::signedValue", "probe::Value::shortValue", "probe::Value::wide", "probe::Value::huge",
                "probe::Value::narrow", "probe::Value::tinies", "probe::Value::narrows", "probe::Leaf::tiny", "probe::Leaf::narrow",
                "probe::Value::letter", "probe::Value::letters", "probe::Value::label", "probe::Leaf::socket", "probe::Leaf::aliases",
                "probe::Point::x", "probe::Point::y", "probe::Segment::from", "probe::Segment::owner", "probe::Plain::at",
                "probe::Value::point", "probe::Value::segment", "probe::Value::target", "probe::Value::targets", "probe::Value::peers",
                "probe::Leaf::anything", "probe::Leaf::links", "probe::Value::strong",
                "probe::Value::kind", "probe::Value::kinds", "probe::Value::self", "probe::Leaf::kind", "probe::Leaf::kinds",
                "probe::Derived::extra", "probe::Other::value",
                "probe::Leaf::weight", "probe::Value::text", "probe::Value::name", "probe::Value::list", "probe::Value::pointer",
                "probe::Leaf::x", "probe::Leaf::tags", "probe::Value::byMode", "probe::Value::assets", "probe::Value::byRatio",
                "probe::Value::byObject", "probe::Value::byWeak", "probe::Value::byLeaf", "probe::Value::ratios", "probe::Value::lists",
                "probe::Value::sets", "probe::Value::maybe", "probe::Value::maybeSet", "probe::Value::maybeKey", "probe::Value::unordered",
                "probe::Value::maybeMap", "probe::Value::byAsset", "probe::Value::tags", "probe::Value::custom", "probe::Value::unorderedSet",
                "probe::Value::constKey", "probe::Value::constValue", "probe::Value::pointers", "probe::Leaf::named",
                "probe::Bundle::byName", "probe::Value::bySlot", "probe::Value::anything", "probe::Value::required", "probe::Value::notClass",
                "probe::Value::maps", "probe::Value::nestedRefs", "probe::Value::slots", "probe::Slot::material", "probe::Slot::byKey",
                "probe::Slot::target", "probe::Slot::codes", "probe::Value::watchers", "probe::Value::smalls", "probe::Value::shorts",
                "probe::Value::grid", "probe::Value::fixed", "probe::Leaf::target",
                "probe::Value::row", "probe::Value::rows", "probe::Value::table", "probe::Value::host", "probe::Host::row",
                "probe::Host::count", "probe::Plain::count" }
                .Concat(Enumerable.Range(2, 16).Select(level => $"probe::Chain{level}::inner")))
            {
                manifest.Register("field", name);
            }

            return 0;
        });
    }
}
