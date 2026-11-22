#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "ContentBrowserMenuContexts.h"
#include "DesktopPlatformModule.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "Materials/MaterialFunctionInterface.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollection.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "RocksExportCommon.h"
#include "RocksExporter.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "RocksExport"

DEFINE_LOG_CATEGORY(LogRocksExport);

// 플러그인 모듈이다(11.6절 FRocksExportModule). 에디터 UI가 있을 때 콘텐츠 브라우저 우클릭 메뉴에
// "Export to Rocks..."를 더한다(11.2절). commandlet에서는 메뉴를 등록하지 않는다.
class FRocksExportModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (!IsRunningCommandlet())
		{
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FRocksExportModule::RegisterMenus));
		}
	}

	virtual void ShutdownModule() override
	{
		if (UObjectInitialized())
		{
			UToolMenus::UnRegisterStartupCallback(this);
			UToolMenus::UnregisterOwner(this);
		}
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped Owner(this);

		// 2절 범위의 상위 클래스 메뉴에 한 번씩 건다. 하위 클래스 메뉴는 상위 메뉴 항목을 물려받는다.
		UClass* const MenuClasses[] = {
			UTexture::StaticClass(),
			UStaticMesh::StaticClass(),
			USkeletalMesh::StaticClass(),
			USkeleton::StaticClass(),
			UAnimSequence::StaticClass(),
			UMaterialInterface::StaticClass(),
			UMaterialFunctionInterface::StaticClass(),
			UMaterialParameterCollection::StaticClass(),
		};

		for (UClass* MenuClass : MenuClasses)
		{
			UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(MenuClass);
			Menu->AddDynamicSection(TEXT("RocksExport"), FNewToolMenuDelegate::CreateStatic(&FRocksExportModule::FillMenu));
		}
	}

	static void FillMenu(UToolMenu* Menu)
	{
		const UContentBrowserAssetContextMenuContext* Context = Menu->FindContext<UContentBrowserAssetContextMenuContext>();
		if (Context == nullptr || Context->SelectedAssets.IsEmpty())
		{
			return;
		}

		const TArray<FAssetData> Selected = Context->SelectedAssets;
		FToolMenuSection& Section = Menu->FindOrAddSection(TEXT("GetAssetActions"));
		Section.AddMenuEntry(
			TEXT("RocksExport"),
			LOCTEXT("ExportToRocks", "Export to Rocks..."),
			LOCTEXT("ExportToRocksTooltip", "Export the selected assets and their dependencies as a Rocks exchange bundle."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([Selected]() { ExportSelection(Selected); })));
	}

	// 출력 디렉터리를 고르고 닫힘을 켠 채 내보낸다. 굽기 같은 추출 옵션은 그 단계(UX6 등)에서 대화상자에 더한다.
	static void ExportSelection(const TArray<FAssetData>& Selected)
	{
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
		if (DesktopPlatform == nullptr)
		{
			return;
		}

		FString Directory;
		const void* ParentWindow = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		if (!DesktopPlatform->OpenDirectoryDialog(ParentWindow, LOCTEXT("ChooseBundle", "Choose the Rocks exchange bundle directory").ToString(), FString(), Directory))
		{
			return;
		}

		IAssetRegistry& Registry = IAssetRegistry::GetChecked();
		if (Registry.IsLoadingAssets())
		{
			Registry.WaitForCompletion();
		}

		FRocksExportOptions Options;
		Options.SelectedAssets = Selected;
		Options.OutputDirectory = Directory;
		FRocksExportResult Result;
		if (FRocksExporter::Export(Options, Result))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(
				LOCTEXT("ExportDone", "Exported {0} assets ({1} outside the exchange scope, {2} skipped) and {3} redirects to {4}. Diagnostics: {5}. See the RocksExport log for details."),
				Result.AssetCount, Result.UnsupportedCount, Result.SkippedCount, Result.RedirectCount, FText::FromString(Result.OutputDirectory), Result.DiagnosticCount));
		}
		else
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("ExportFailed", "Rocks export failed: {0}"), FText::FromString(Result.Error)));
		}
	}
};

IMPLEMENT_MODULE(FRocksExportModule, RocksExport)

#undef LOCTEXT_NAMESPACE
