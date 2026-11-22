#include "ExchangeTestSupport.h"

#include <AssetRuntime/AssetDiagnostic.h>
#include <TestSupport/Environment.h>
#include <TestSupport/Test.h>
#include <stdio.h>

// UE가 내보낸 외부 묶음을 리더로 읽는 선택 시험이다(Docs/Design/UnrealExchange.ko.md 11.7절 18번 —
// UX1b 왕복 검증). 환경 변수 ROCKS_UNREAL_EXCHANGE_BUNDLE이 묶음 디렉터리를 가리키면 읽고 결과를
// 출력한다. 변수가 없으면 [SKIP]을 출력하고 통과한다(기본 CI는 바뀌지 않는다).
//
// 판정: 읽기와 임포트 순서 계산이 성공하고, 오류 진단이 없으며, 범위 안 클래스 에셋은 모두 Ready,
// 범위 밖은 Unsupported, 실패한 저장 단위가 없다.
using namespace rocks;
using namespace rocks::exchange_test;
namespace exchange_external_bundle_tests_file {

constexpr char BundleVariable[] = "ROCKS_UNREAL_EXCHANGE_BUNDLE";

const char* StateName(ExchangeAssetState state) noexcept {
    switch (state) {
    case ExchangeAssetState::Ready:
        return "Ready";
    case ExchangeAssetState::Unsupported:
        return "Unsupported";
    default:
        return "Failed";
    }
}

const char* SeverityName(ExchangeSeverity severity) noexcept {
    switch (severity) {
    case ExchangeSeverity::Error:
        return "error";
    case ExchangeSeverity::Warning:
        return "warning";
    default:
        return "info";
    }
}

// printf의 %.*s 인자 길이다.
int PrintLength(StringView text) noexcept {
    return static_cast<int>(text.ByteCount());
}

void PrintDiagnostics(const char* stage, const ExchangeDiagnostics& diagnostics) noexcept {
    for (const ExchangeDiagnostic& item : diagnostics.Items()) {
        const StringView text = item.text.View();
        printf("  %s diagnostic: %s %s/%s asset=%u %.*s\n", stage, SeverityName(item.severity),
               AssetDiagnosticName(item.code), ExchangeIssueName(item.issue), item.asset,
               PrintLength(text), text.Data());
    }
}

TEST(UnrealExchange_ExternalBundle) {
    SystemAllocator allocator;
    StringBuilder<512> directory(allocator);
    if (!test::ReadEnvironmentVariable(BundleVariable, directory)) {
        printf("[SKIP] UnrealExchange_ExternalBundle: %s is not set\n", BundleVariable);
        return;
    }

    ExchangeBundle bundle(allocator);
    ExchangeDiagnostics diagnostics(allocator);
    const Result<void> read = ReadExchangeBundle(platform::PhysicalFileSystem(), directory.View(),
                                                 {}, bundle, diagnostics);
    printf("Bundle %.*s: read %s, %zu assets, %zu redirects\n", PrintLength(directory.View()),
           directory.View().Data(), read ? "ok" : "failed", bundle.Assets().Size(),
           bundle.Redirects().Size());
    PrintDiagnostics("read", diagnostics);
    REQUIRE(read);
    CHECK(diagnostics.Count(ExchangeSeverity::Error) == 0 && diagnostics.Dropped() == 0);
    printf("Exporter %.*s %.*s, engine %.*s (%.*s)\n", PrintLength(bundle.exporter.plugin),
           bundle.exporter.plugin.Data(), PrintLength(bundle.exporter.pluginVersion),
           bundle.exporter.pluginVersion.Data(), PrintLength(bundle.exporter.engineVersion),
           bundle.exporter.engineVersion.Data(), PrintLength(bundle.exporter.engineCommit),
           bundle.exporter.engineCommit.Data());

    for (usize index = 0; index < bundle.Assets().Size(); ++index) {
        const ExchangeAsset& asset = bundle.Assets()[index];
        printf("  [%zu] %s %.*s %.*s (%zu dependencies)\n", index, StateName(asset.state),
               PrintLength(asset.className), asset.className.Data(), PrintLength(asset.objectPath),
               asset.objectPath.Data(), asset.dependencies.Size());
        const bool inScope = asset.exchangeClass != ExchangeClass::Unsupported;
        CHECK(asset.state ==
              (inScope ? ExchangeAssetState::Ready : ExchangeAssetState::Unsupported));
    }

    for (const ExchangeRedirect& redirect : bundle.Redirects()) {
        printf("  redirect %.*s -> %.*s\n", PrintLength(redirect.from), redirect.from.Data(),
               PrintLength(redirect.to), redirect.to.Data());
    }

    ExchangeImportPlan plan(allocator);
    ExchangeDiagnostics orderDiagnostics(allocator);
    const Result<void> planned = PlanExchangeImport(bundle, {}, plan, orderDiagnostics);
    PrintDiagnostics("order", orderDiagnostics);
    REQUIRE(planned);
    CHECK(orderDiagnostics.Count(ExchangeSeverity::Error) == 0);
    usize failed = 0;
    for (const ExchangeImportUnit& unit : plan.Units()) {
        failed += unit.state == ExchangeUnitState::Failed ? 1 : 0;
    }

    printf("Import order: %zu units, %zu failed\n", plan.Units().Size(), failed);
    CHECK(failed == 0);
}

} // namespace exchange_external_bundle_tests_file
