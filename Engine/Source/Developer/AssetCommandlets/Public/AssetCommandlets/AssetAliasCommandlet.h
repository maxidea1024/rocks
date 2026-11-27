#pragma once

#include <Core/Commandlet/Commandlet.h>

// asset-alias-list·asset-alias-remove·asset-alias-prune 명령이다(Docs/Design/AssetSystem 4.3·
// 13절, 7.1절 17 A2e1). context는 명령의 Allocator다.
namespace rocks::commandlet_host {

ROCKS_ASSETCOMMANDLETS_API Result<int32>
RunAssetAliasListCommand(void* context, Span<const StringView> args,
                         CommandletOutput& output) noexcept;
ROCKS_ASSETCOMMANDLETS_API Result<int32>
RunAssetAliasRemoveCommand(void* context, Span<const StringView> args,
                           CommandletOutput& output) noexcept;
ROCKS_ASSETCOMMANDLETS_API Result<int32>
RunAssetAliasPruneCommand(void* context, Span<const StringView> args,
                          CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
