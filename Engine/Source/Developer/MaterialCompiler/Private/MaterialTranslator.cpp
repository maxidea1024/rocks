#include <MaterialCompiler/MaterialTranslator.h>

#include "Translation/Translator.h"

// 머티리얼 그래프 → IR 번역 진입점이다(Docs/Design/MaterialSystem 6.8절).
namespace rocks::material {

Result<void> TranslateMaterial(Allocator& allocator, const MaterialTranslationInput& input,
                               MaterialTranslation& out) noexcept {
    detail::Translator translator(allocator, input, out);
    auto translated = translator.Run();
    if (!translated) {
        out.module.Clear();
        out.succeeded = false;
    }

    return translated;
}

} // namespace rocks::material
