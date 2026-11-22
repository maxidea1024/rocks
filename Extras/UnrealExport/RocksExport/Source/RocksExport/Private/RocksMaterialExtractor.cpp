#include "RocksMaterialExtractor.h"

#include "Engine/Font.h"
#include "Engine/TextureCollection.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameters.h"
#include "RocksAssetBody.h"
#include "RocksExportCommon.h"
#include "RocksJsonWriter.h"
#include "RocksPropertyDumper.h"
#include "SparseVolumeTexture/SparseVolumeTexture.h"
#include "VT/RuntimeVirtualTexture.h"

namespace RocksMaterialExtractorPrivate
{
	// 2절 머티리얼 계열 교환 클래스다.
	static const TCHAR* const MaterialClasses[] = {
		TEXT("Material"),
		TEXT("MaterialInstanceConstant"),
		TEXT("MaterialFunction"),
		TEXT("MaterialFunctionInstance"),
		TEXT("MaterialParameterCollection"),
	};

	// 그래프 묶음 구조체다(UMaterialEditorOnlyData·UMaterialFunctionEditorOnlyData의 ExpressionCollection). 노드는
	// data.nodes에 따로 쓰므로 settings 덤프에서 뺀다.
	static const TCHAR* const ExpressionCollectionStruct = TEXT("/Script/Engine.MaterialExpressionCollection");

	// 머티리얼 출력 핀 구조체다. MaterialAttributes 핀만 FExpressionInput 계열이고(덤퍼가 늘 뺀다) 나머지 핀은
	// FMaterialInput 계열이다(Material.h:157-265). 핀은 data.outputs에 따로 쓰므로 settings 덤프에서 뺀다.
	static const TCHAR* const ExpressionInputStruct = TEXT("/Script/Engine.ExpressionInput");
	static const TCHAR* const MaterialInputStruct = TEXT("/Script/Engine.MaterialInput");
	static const TCHAR* const MaterialAttributesPin = TEXT("MaterialAttributes");

	// 노드 properties에서 빼는 UMaterialExpression 멤버다(소유 머티리얼·함수 역참조, 위치·guid·출력은 노드 키로 따로 쓴다).
	static const TCHAR* const NodeExcludedProperties[] = {
		TEXT("Material"),
		TEXT("Function"),
		TEXT("MaterialExpressionEditorX"),
		TEXT("MaterialExpressionEditorY"),
		TEXT("MaterialExpressionGuid"),
		TEXT("Outputs"),
	};

	// mask 칸 넷이다(r·g·b·a).
	static void WriteMask(FRocksJsonWriter& Writer, int32 Mask, int32 R, int32 G, int32 B, int32 A)
	{
		if (Mask == 0)
		{
			Writer.Null();
			return;
		}

		Writer.BeginArray();
		Writer.Int(R);
		Writer.Int(G);
		Writer.Int(B);
		Writer.Int(A);
		Writer.EndArray();
	}

	static FString GuidText(const FGuid& Guid)
	{
		return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
	}

	// 연결 대상(노드 이름·출력 번호)이다. 연결이 없으면 null이다. 노드는 객체 이름으로 가리킨다. expression GUID는 복사한
	// 노드끼리 겹치는 일이 엔진 콘텐츠에도 흔해서(8.6절) 식별자로 쓸 수 없고, 이름은 같은 Outer 안에서 UObject가 유일하게 보장한다.
	static void WriteLink(FRocksJsonWriter& Writer, const FExpressionInput& Input)
	{
		if (Input.Expression == nullptr)
		{
			Writer.Null();
			return;
		}

		Writer.BeginObject();
		Writer.Key(TEXT("node"));
		Writer.String(Input.Expression->GetName());
		Writer.Key(TEXT("output"));
		Writer.Int(Input.OutputIndex);
		Writer.EndObject();
	}

	// 그래프 노드 하나다(8.1절).
	static void WriteNode(UMaterialExpression& Expression, FRocksPropertyDumper& Dumper, FRocksJsonWriter& Writer, FRocksAssetBody& Body)
	{
		Writer.BeginObject();
		Writer.Key(TEXT("guid"));
		Writer.String(GuidText(Expression.MaterialExpressionGuid));
		Writer.Key(TEXT("name"));
		Writer.String(Expression.GetName());
		Writer.Key(TEXT("class"));
		Writer.String(Expression.GetClass()->GetName());
		Writer.Key(TEXT("position"));
		Writer.BeginArray();
		Writer.Int(Expression.MaterialExpressionEditorX);
		Writer.Int(Expression.MaterialExpressionEditorY);
		Writer.EndArray();

		// 입력 열거는 CountInputs·GetInput·GetInputName이다(MaterialExpression.h:328-345, 8.1절).
		Writer.Key(TEXT("inputs"));
		Writer.BeginArray();
		for (int32 Index = 0; Index < Expression.CountInputs(); ++Index)
		{
			const FExpressionInput* Input = Expression.GetInput(Index);
			Writer.BeginObject();
			Writer.Key(TEXT("name"));
			Writer.String(Expression.GetInputName(Index).ToString());
			Writer.Key(TEXT("from"));
			if (Input != nullptr)
			{
				WriteLink(Writer, *Input);
			}
			else
			{
				Writer.Null();
			}

			Writer.Key(TEXT("mask"));
			if (Input != nullptr && Input->Expression != nullptr)
			{
				WriteMask(Writer, Input->Mask, Input->MaskR, Input->MaskG, Input->MaskB, Input->MaskA);
			}
			else
			{
				Writer.Null();
			}

			Writer.EndObject();
		}

		Writer.EndArray();
		Writer.Key(TEXT("outputs"));
		Writer.BeginArray();
		for (const FExpressionOutput& Output : Expression.GetOutputs())
		{
			Writer.BeginObject();
			Writer.Key(TEXT("name"));
			Writer.String(Output.OutputName.IsNone() ? FString() : Output.OutputName.ToString());
			Writer.Key(TEXT("mask"));
			WriteMask(Writer, Output.Mask, Output.MaskR, Output.MaskG, Output.MaskB, Output.MaskA);
			Writer.EndObject();
		}

		Writer.EndArray();
		Writer.Key(TEXT("properties"));
		Dumper.DumpObject(&Expression, Writer);
		Writer.EndObject();

		const UMaterialExpressionCustom* Custom = Cast<UMaterialExpressionCustom>(&Expression);
		if (Custom != nullptr && !Custom->IncludeFilePaths.IsEmpty())
		{
			Body.Diagnostics.Add({TEXT("CustomIncludeSkipped"), RocksExport::SeverityInfo,
				FString::Printf(TEXT("custom node %s includes %d files that are not exported"), *Expression.GetName(), Custom->IncludeFilePaths.Num())});
		}
	}

	// 노드끼리 참조(이름 있는 reroute 등)와 settings 쪽 참조는 그 자리에서 노드를 펼치지 않는다({ "subobject": 이름 }).
	// settings 덤프 전에 부른다.
	static void MarkNodesExpanded(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, TConstArrayView<TObjectPtr<UMaterialExpressionComment>> Comments,
		FRocksPropertyDumper& Dumper)
	{
		for (const TObjectPtr<UMaterialExpression>& Expression : Expressions)
		{
			Dumper.MarkExpanded(Expression.Get());
		}

		for (const TObjectPtr<UMaterialExpressionComment>& Comment : Comments)
		{
			Dumper.MarkExpanded(Comment.Get());
		}
	}

	// 그래프 노드 배열이다(expression 다음 comment, 각자 UE 배열 순서).
	static void WriteGraph(TConstArrayView<TObjectPtr<UMaterialExpression>> Expressions, TConstArrayView<TObjectPtr<UMaterialExpressionComment>> Comments,
		FRocksPropertyDumper& Dumper, FRocksJsonWriter& Writer, FRocksAssetBody& Body)
	{
		Writer.Key(TEXT("nodes"));
		Writer.BeginArray();
		for (const TObjectPtr<UMaterialExpression>& Expression : Expressions)
		{
			if (Expression != nullptr)
			{
				WriteNode(*Expression, Dumper, Writer, Body);
			}
		}

		for (const TObjectPtr<UMaterialExpressionComment>& Comment : Comments)
		{
			if (Comment != nullptr)
			{
				WriteNode(*Comment, Dumper, Writer, Body);
			}
		}

		Writer.EndArray();
	}

	// 출력 핀 구조체인가다(FExpressionInput 계열 또는 FMaterialInput 계열).
	static bool IsPinStruct(const UScriptStruct* Struct)
	{
		const UScriptStruct* ExpressionInput = FindObject<UScriptStruct>(nullptr, ExpressionInputStruct);
		const UScriptStruct* MaterialInput = FindObject<UScriptStruct>(nullptr, MaterialInputStruct);
		return (ExpressionInput != nullptr && Struct->IsChildOf(ExpressionInput)) || (MaterialInput != nullptr && Struct->IsChildOf(MaterialInput));
	}

	// 미연결 상수다. 핀 구조체가 직접 선언한 UseConstant 밖의 멤버가 상수 칸이다(Constant 하나, FVector2MaterialInput은
	// ConstantX·ConstantY). 칸이 하나면 그 값, 여럿이면 선언 순 배열이다. UseConstant가 없거나 꺼져 있으면 null이다.
	static void WritePinConstant(const UScriptStruct* PinStruct, const void* Value, FRocksPropertyDumper& Dumper, FRocksJsonWriter& Writer)
	{
		const FBoolProperty* UseConstant = CastField<FBoolProperty>(PinStruct->FindPropertyByName(TEXT("UseConstant")));
		if (UseConstant == nullptr || !UseConstant->GetPropertyValue_InContainer(Value))
		{
			Writer.Null();
			return;
		}

		TArray<const FProperty*> Fields;
		for (TFieldIterator<FProperty> It(PinStruct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			if (*It != UseConstant)
			{
				Fields.Add(*It);
			}
		}

		if (Fields.Num() == 1)
		{
			Dumper.DumpValue(Fields[0], Fields[0]->ContainerPtrToValuePtr<void>(Value), Writer);
			return;
		}

		Writer.BeginArray();
		for (const FProperty* Field : Fields)
		{
			Dumper.DumpValue(Field, Field->ContainerPtrToValuePtr<void>(Value), Writer);
		}

		Writer.EndArray();
	}

	// 머티리얼 출력 핀이다(8.1절): bUseMaterialAttributes면 MaterialAttributes 핀 하나, 아니면 나머지 핀 전부(선언 순).
	// 핀마다 연결 또는 미연결 상수다. 고정 배열 핀(CustomizedUVs[8])은 칸마다 "이름[번호]"로 쓴다.
	static void WriteMaterialOutputs(UMaterial& Material, FRocksPropertyDumper& Dumper, FRocksJsonWriter& Writer)
	{
		UMaterialEditorOnlyData* EditorOnly = Material.GetEditorOnlyData();
		Writer.Key(TEXT("outputs"));
		Writer.BeginArray();
		if (EditorOnly != nullptr)
		{
			for (TFieldIterator<FStructProperty> It(EditorOnly->GetClass()); It; ++It)
			{
				const FStructProperty* Pin = *It;
				if (!IsPinStruct(Pin->Struct) || Pin->HasAnyPropertyFlags(CPF_Deprecated))
				{
					continue;
				}

				if ((Pin->GetName() == MaterialAttributesPin) != bool(Material.bUseMaterialAttributes))
				{
					continue;
				}

				for (int32 Slot = 0; Slot < Pin->ArrayDim; ++Slot)
				{
					const void* Value = Pin->ContainerPtrToValuePtr<void>(EditorOnly, Slot);
					const FExpressionInput& Input = *static_cast<const FExpressionInput*>(Value);
					Writer.BeginObject();
					Writer.Key(TEXT("name"));
					Writer.String(Pin->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Pin->GetName(), Slot) : Pin->GetName());
					Writer.Key(TEXT("from"));
					WriteLink(Writer, Input);
					Writer.Key(TEXT("mask"));
					if (Input.Expression != nullptr)
					{
						WriteMask(Writer, Input.Mask, Input.MaskR, Input.MaskG, Input.MaskB, Input.MaskA);
					}
					else
					{
						Writer.Null();
					}

					Writer.Key(TEXT("constant"));
					WritePinConstant(Pin->Struct, Value, Dumper, Writer);
					Writer.EndObject();
				}
			}
		}

		Writer.EndArray();
	}

	// 해석된 파라미터 값 하나다(8.2절 resolved).
	static void WriteParameterValue(const FMaterialParameterValue& Value, FRocksPropertyDumper& Dumper, FRocksJsonWriter& Writer)
	{
		switch (Value.Type)
		{
		case EMaterialParameterType::Scalar:
			Writer.Float(Value.Float[0]);
			return;
		case EMaterialParameterType::Vector:
			Writer.BeginArray();
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Writer.Float(Value.Float[Index]);
			}

			Writer.EndArray();
			return;
		case EMaterialParameterType::DoubleVector:
			Writer.BeginArray();
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Writer.Double(Value.Double[Index]);
			}

			Writer.EndArray();
			return;
		case EMaterialParameterType::StaticSwitch:
			Writer.Bool(Value.Bool[0]);
			return;
		case EMaterialParameterType::StaticComponentMask:
			Writer.BeginArray();
			for (int32 Index = 0; Index < 4; ++Index)
			{
				Writer.Bool(Value.Bool[Index]);
			}

			Writer.EndArray();
			return;
		case EMaterialParameterType::Font:
			Writer.BeginObject();
			Writer.Key(TEXT("font"));
			Dumper.DumpObjectReference(Value.Font.Value, Writer);
			Writer.Key(TEXT("page"));
			Writer.Int(Value.Font.Page);
			Writer.EndObject();
			return;
		case EMaterialParameterType::Texture:
			Dumper.DumpObjectReference(Value.Texture, Writer);
			return;
		case EMaterialParameterType::TextureCollection:
			Dumper.DumpObjectReference(Value.TextureCollection, Writer);
			return;
		case EMaterialParameterType::RuntimeVirtualTexture:
			Dumper.DumpObjectReference(Value.RuntimeVirtualTexture, Writer);
			return;
		case EMaterialParameterType::SparseVolumeTexture:
			Dumper.DumpObjectReference(Value.SparseVolumeTexture, Writer);
			return;
		case EMaterialParameterType::ParameterCollection:
			Dumper.DumpObjectReference(Value.ParameterCollection, Writer);
			return;
		default:
			Writer.Null();
			return;
		}
	}

	// 파라미터 종류 이름이다. UEnum 이름 조회는 쓰지 않는다: StaticComponentMask가 NumRuntime과 같은 값이라
	// (MaterialParameters.h:187-205) 값으로 찾으면 NumRuntime이 나온다.
	static const TCHAR* ParameterTypeName(EMaterialParameterType Type)
	{
		switch (Type)
		{
		case EMaterialParameterType::Scalar:
			return TEXT("Scalar");
		case EMaterialParameterType::Vector:
			return TEXT("Vector");
		case EMaterialParameterType::DoubleVector:
			return TEXT("DoubleVector");
		case EMaterialParameterType::Texture:
			return TEXT("Texture");
		case EMaterialParameterType::TextureCollection:
			return TEXT("TextureCollection");
		case EMaterialParameterType::Font:
			return TEXT("Font");
		case EMaterialParameterType::RuntimeVirtualTexture:
			return TEXT("RuntimeVirtualTexture");
		case EMaterialParameterType::SparseVolumeTexture:
			return TEXT("SparseVolumeTexture");
		case EMaterialParameterType::StaticSwitch:
			return TEXT("StaticSwitch");
		case EMaterialParameterType::ParameterCollection:
			return TEXT("ParameterCollection");
		case EMaterialParameterType::StaticComponentMask:
			return TEXT("StaticComponentMask");
		default:
			return TEXT("Unknown");
		}
	}

	struct FResolved
	{
		TArray<uint8> Key;
		EMaterialParameterType Type;
		FMaterialParameterInfo Info;
		FMaterialParameterValue Value;
	};

	// 인스턴스가 해석한 최종 파라미터 값이다(GetAllParametersOfType — 파라미터 정보의 compact JSON 순, 같으면 타입 순).
	static void WriteResolved(UMaterialInterface& Instance, FRocksPropertyDumper& Dumper, FRocksJsonWriter& Writer)
	{
		TArray<FResolved> Entries;
		for (int32 Type = 0; Type < int32(EMaterialParameterType::Num); ++Type)
		{
			TMap<FMaterialParameterInfo, FMaterialParameterMetadata> Parameters;
			Instance.GetAllParametersOfType(EMaterialParameterType(Type), Parameters);
			for (const TPair<FMaterialParameterInfo, FMaterialParameterMetadata>& Parameter : Parameters)
			{
				FRocksJsonWriter KeyWriter(FRocksJsonWriter::EStyle::Compact);
				FRocksPropertyDumper KeyDumper(nullptr);
				KeyDumper.DumpStruct(FMaterialParameterInfo::StaticStruct(), &Parameter.Key, KeyWriter);
				Entries.Add({KeyWriter.Finish(), EMaterialParameterType(Type), Parameter.Key, Parameter.Value.Value});
			}
		}

		Entries.Sort([](const FResolved& Left, const FResolved& Right)
			{
				const int32 Shared = FMath::Min(Left.Key.Num(), Right.Key.Num());
				const int32 Order = Shared > 0 ? FMemory::Memcmp(Left.Key.GetData(), Right.Key.GetData(), Shared) : 0;
				if (Order != 0 || Left.Key.Num() != Right.Key.Num())
				{
					return Order != 0 ? Order < 0 : Left.Key.Num() < Right.Key.Num();
				}

				return uint8(Left.Type) < uint8(Right.Type);
			});

		Writer.Key(TEXT("resolved"));
		Writer.BeginArray();
		for (const FResolved& Entry : Entries)
		{
			Writer.BeginObject();
			Writer.Key(TEXT("parameter"));
			Dumper.DumpStruct(FMaterialParameterInfo::StaticStruct(), &Entry.Info, Writer);
			Writer.Key(TEXT("type"));
			Writer.String(ParameterTypeName(Entry.Type));
			Writer.Key(TEXT("value"));
			WriteParameterValue(Entry.Value, Dumper, Writer);
			Writer.EndObject();
		}

		Writer.EndArray();
	}
}

bool FRocksMaterialExtractor::IsMaterialClass(const FString& ExchangeClass)
{
	for (const TCHAR* Name : RocksMaterialExtractorPrivate::MaterialClasses)
	{
		if (ExchangeClass.Equals(Name, ESearchCase::CaseSensitive))
		{
			return true;
		}
	}

	return false;
}

void FRocksMaterialExtractor::Extract(UObject& Asset, FRocksAssetBody& OutBody)
{
	using namespace RocksMaterialExtractorPrivate;

	FRocksPropertyDumper Dumper(&Asset);
	Dumper.ExcludeStruct(FindObject<UScriptStruct>(nullptr, ExpressionCollectionStruct));
	Dumper.ExcludeStruct(FindObject<UScriptStruct>(nullptr, MaterialInputStruct));
	for (const TCHAR* Name : NodeExcludedProperties)
	{
		Dumper.ExcludeProperty(FindFProperty<FProperty>(UMaterialExpression::StaticClass(), Name));
	}

	UMaterial* Material = Cast<UMaterial>(&Asset);
	UMaterialFunction* Function = Cast<UMaterialFunction>(&Asset);
	if (Material != nullptr)
	{
		MarkNodesExpanded(Material->GetExpressions(), Material->GetEditorComments(), Dumper);
	}
	else if (Function != nullptr)
	{
		MarkNodesExpanded(Function->GetExpressions(), Function->GetEditorComments(), Dumper);
	}

	FRocksJsonWriter Settings(FRocksJsonWriter::EStyle::Pretty, 1);
	Dumper.DumpObject(&Asset, Settings);
	OutBody.Settings = Settings.FinishFragment();

	FRocksJsonWriter Data(FRocksJsonWriter::EStyle::Pretty, 1);
	Data.BeginObject();
	if (Material != nullptr)
	{
		WriteGraph(Material->GetExpressions(), Material->GetEditorComments(), Dumper, Data, OutBody);
		WriteMaterialOutputs(*Material, Dumper, Data);
	}
	else if (Function != nullptr)
	{
		WriteGraph(Function->GetExpressions(), Function->GetEditorComments(), Dumper, Data, OutBody);
	}
	else if (UMaterialInstanceConstant* Instance = Cast<UMaterialInstanceConstant>(&Asset))
	{
		WriteResolved(*Instance, Dumper, Data);
	}

	Data.EndObject();
	OutBody.Data = Data.FinishFragment();
	OutBody.References = Dumper.GetReferences();
}
