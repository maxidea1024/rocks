#include "RocksPropertyDumper.h"

#include "RocksJsonWriter.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace RocksPropertyDumperPrivate
{
	// 기본 제외 구조체다(머티리얼 입력 핀 — 8.1절 inputs로 옮기므로 properties에서 뺀다).
	static const TCHAR* const ExpressionInputStruct = TEXT("/Script/Engine.ExpressionInput");

	// 참조로 모으지 않는 패키지 접두다(네이티브 클래스·CDO, 메모리 패키지).
	static const TCHAR* const NonAssetPrefixes[] = {
		TEXT("/Script/"),
		TEXT("/Memory/"),
	};

	struct FSortEntry
	{
		TArray<uint8> Key;
		const uint8* Element = nullptr;
		const uint8* Value = nullptr;
	};

	static bool IsKeyLess(const FSortEntry& Left, const FSortEntry& Right)
	{
		const int32 Shared = FMath::Min(Left.Key.Num(), Right.Key.Num());
		const int32 Order = Shared > 0 ? FMemory::Memcmp(Left.Key.GetData(), Right.Key.GetData(), Shared) : 0;
		return Order != 0 ? Order < 0 : Left.Key.Num() < Right.Key.Num();
	}

	static bool IsUnsignedInteger(const FProperty* Property)
	{
		return Property->IsA<FByteProperty>() || Property->IsA<FUInt16Property>() || Property->IsA<FUInt32Property>() || Property->IsA<FUInt64Property>();
	}

	static void WriteEnum(const UEnum* Enum, int64 Value, FRocksJsonWriter& Writer)
	{
		if (Enum != nullptr && Enum->IsValidEnumValue(Value))
		{
			Writer.String(Enum->GetNameStringByValue(Value));
			return;
		}

		Writer.Int(Value);
	}
}

FRocksPropertyDumper::FRocksPropertyDumper(const UObject* InDumpRoot)
	: DumpRoot(InDumpRoot)
{
	if (const UScriptStruct* ExpressionInput = FindObject<UScriptStruct>(nullptr, RocksPropertyDumperPrivate::ExpressionInputStruct))
	{
		ExcludedStructs.Add(ExpressionInput);
	}
}

void FRocksPropertyDumper::ExcludeStruct(const UScriptStruct* Struct)
{
	if (Struct != nullptr)
	{
		ExcludedStructs.AddUnique(Struct);
	}
}

bool FRocksPropertyDumper::ShouldSkip(const FProperty* Property) const
{
	if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated))
	{
		return true;
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		for (const UScriptStruct* Excluded : ExcludedStructs)
		{
			if (StructProperty->Struct->IsChildOf(Excluded))
			{
				return true;
			}
		}
	}

	return false;
}

void FRocksPropertyDumper::DumpObject(const UObject* Object, FRocksJsonWriter& Writer)
{
	check(Object != nullptr);
	Writer.BeginObject();
	for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
	{
		DumpProperty(*It, Object, Writer);
	}

	Writer.EndObject();
}

void FRocksPropertyDumper::DumpStruct(const UStruct* Struct, const void* Data, FRocksJsonWriter& Writer)
{
	check(Struct != nullptr && Data != nullptr);
	Writer.BeginObject();
	for (TFieldIterator<FProperty> It(Struct); It; ++It)
	{
		DumpProperty(*It, Data, Writer);
	}

	Writer.EndObject();
}

void FRocksPropertyDumper::DumpProperty(const FProperty* Property, const void* ContainerData, FRocksJsonWriter& Writer)
{
	if (ShouldSkip(Property))
	{
		return;
	}

	Writer.Key(Property->GetName());
	if (Property->ArrayDim == 1)
	{
		DumpValue(Property, Property->ContainerPtrToValuePtr<void>(ContainerData), Writer);
		return;
	}

	Writer.BeginArray();
	for (int32 Index = 0; Index < Property->ArrayDim; ++Index)
	{
		DumpValue(Property, Property->ContainerPtrToValuePtr<void>(ContainerData, Index), Writer);
	}

	Writer.EndArray();
}

void FRocksPropertyDumper::AddReference(const FSoftObjectPath& Path, bool bHard)
{
	if (bKeyPass || Path.IsNull())
	{
		return;
	}

	const FString AssetPath = Path.GetWithoutSubPath().ToString();
	for (const TCHAR* Prefix : RocksPropertyDumperPrivate::NonAssetPrefixes)
	{
		if (AssetPath.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return;
		}
	}

	bool& bStoredHard = References.FindOrAdd(AssetPath, false);
	bStoredHard = bStoredHard || bHard;
}

void FRocksPropertyDumper::DumpObjectReference(const UObject* Object, FRocksJsonWriter& Writer)
{
	if (Object == nullptr)
	{
		Writer.Null();
		return;
	}

	// 덤프 루트 아래 하위 객체는 처음 한 번만 펼친다(공유·순환 참조는 경로만).
	if (DumpRoot != nullptr && Object->IsIn(DumpRoot))
	{
		const FString Relative = Object->GetPathName(DumpRoot);
		const bool bExpand = !bKeyPass && !Expanded.Contains(Object);
		Writer.BeginObject();
		Writer.Key(TEXT("subobject"));
		Writer.String(Relative);
		if (bExpand)
		{
			Expanded.Add(Object);
			Writer.Key(TEXT("class"));
			Writer.String(Object->GetClass()->GetPathName());
			Writer.Key(TEXT("properties"));
			DumpObject(Object, Writer);
		}

		Writer.EndObject();
		return;
	}

	Writer.BeginObject();
	Writer.Key(TEXT("objectPath"));
	Writer.String(Object->GetPathName());
	Writer.EndObject();
	if (Object->GetOutermost() != GetTransientPackage())
	{
		AddReference(FSoftObjectPath(Object), true);
	}
}

void FRocksPropertyDumper::DumpSoftReference(const FSoftObjectPath& Path, FRocksJsonWriter& Writer)
{
	if (Path.IsNull())
	{
		Writer.Null();
		return;
	}

	Writer.BeginObject();
	Writer.Key(TEXT("objectPath"));
	Writer.String(Path.ToString());
	Writer.EndObject();
	AddReference(Path, false);
}

TArray<uint8> FRocksPropertyDumper::SortKey(const FProperty* Property, const void* Value)
{
	FRocksJsonWriter KeyWriter(FRocksJsonWriter::EStyle::Compact);
	const bool bSaved = bKeyPass;
	bKeyPass = true;
	DumpValue(Property, Value, KeyWriter);
	bKeyPass = bSaved;
	return KeyWriter.Finish();
}

void FRocksPropertyDumper::DumpValue(const FProperty* Property, const void* Value, FRocksJsonWriter& Writer)
{
	using namespace RocksPropertyDumperPrivate;

	if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
	{
		Writer.Bool(BoolProperty->GetPropertyValue(Value));
		return;
	}

	if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
	{
		WriteEnum(EnumProperty->GetEnum(), EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(Value), Writer);
		return;
	}

	if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty != nullptr && ByteProperty->Enum != nullptr)
	{
		WriteEnum(ByteProperty->Enum, *static_cast<const uint8*>(Value), Writer);
		return;
	}

	if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
	{
		Writer.Float(FloatProperty->GetPropertyValue(Value));
		return;
	}

	if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
	{
		Writer.Double(DoubleProperty->GetPropertyValue(Value));
		return;
	}

	if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property); NumericProperty != nullptr && NumericProperty->IsInteger())
	{
		if (IsUnsignedInteger(Property))
		{
			Writer.UInt(NumericProperty->GetUnsignedIntPropertyValue(Value));
		}
		else
		{
			Writer.Int(NumericProperty->GetSignedIntPropertyValue(Value));
		}

		return;
	}

	if (const FStrProperty* StringProperty = CastField<FStrProperty>(Property))
	{
		Writer.String(StringProperty->GetPropertyValue(Value));
		return;
	}

	if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
	{
		Writer.String(NameProperty->GetPropertyValue(Value).ToString());
		return;
	}

	if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
	{
		// 표시 문자열은 언어마다 다르므로 원문(source string)을 쓴다.
		const FText& Text = TextProperty->GetPropertyValue(Value);
		const FString* Source = FTextInspector::GetSourceString(Text);
		Writer.String(Source != nullptr ? *Source : Text.ToString());
		return;
	}

	if (const FSoftObjectProperty* SoftProperty = CastField<FSoftObjectProperty>(Property))
	{
		DumpSoftReference(SoftProperty->GetPropertyValue(Value).ToSoftObjectPath(), Writer);
		return;
	}

	if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
	{
		DumpObjectReference(ObjectProperty->GetObjectPropertyValue(Value), Writer);
		return;
	}

	if (const FInterfaceProperty* InterfaceProperty = CastField<FInterfaceProperty>(Property))
	{
		DumpObjectReference(InterfaceProperty->GetPropertyValue(Value).GetObject(), Writer);
		return;
	}

	if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
	{
		if (StructProperty->Struct->IsChildOf(TBaseStructure<FSoftObjectPath>::Get()))
		{
			DumpSoftReference(*static_cast<const FSoftObjectPath*>(Value), Writer);
			return;
		}

		DumpStruct(StructProperty->Struct, Value, Writer);
		return;
	}

	if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper Helper(ArrayProperty, Value);
		Writer.BeginArray();
		for (int32 Index = 0; Index < Helper.Num(); ++Index)
		{
			DumpValue(ArrayProperty->Inner, Helper.GetRawPtr(Index), Writer);
		}

		Writer.EndArray();
		return;
	}

	if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
	{
		FScriptSetHelper Helper(SetProperty, Value);
		TArray<FSortEntry> Entries;
		for (FScriptSetHelper::FIterator It(Helper); It; ++It)
		{
			const uint8* Element = Helper.GetElementPtr(It);
			Entries.Add({SortKey(SetProperty->ElementProp, Element), Element, nullptr});
		}

		Entries.StableSort(IsKeyLess);
		Writer.BeginArray();
		for (const FSortEntry& Entry : Entries)
		{
			DumpValue(SetProperty->ElementProp, Entry.Element, Writer);
		}

		Writer.EndArray();
		return;
	}

	if (const FMapProperty* MapProperty = CastField<FMapProperty>(Property))
	{
		FScriptMapHelper Helper(MapProperty, Value);
		TArray<FSortEntry> Entries;
		for (FScriptMapHelper::FIterator It(Helper); It; ++It)
		{
			const uint8* Key = Helper.GetKeyPtr(It);
			Entries.Add({SortKey(MapProperty->KeyProp, Key), Key, Helper.GetValuePtr(It)});
		}

		Entries.StableSort(IsKeyLess);
		Writer.BeginArray();
		for (const FSortEntry& Entry : Entries)
		{
			Writer.BeginObject();
			Writer.Key(TEXT("key"));
			DumpValue(MapProperty->KeyProp, Entry.Element, Writer);
			Writer.Key(TEXT("value"));
			DumpValue(MapProperty->ValueProp, Entry.Value, Writer);
			Writer.EndObject();
		}

		Writer.EndArray();
		return;
	}

	// 그 밖(delegate·field path·optional·문자열 변형 등)은 UE 텍스트 내보내기 결과다.
	FString Text;
	Property->ExportTextItem_Direct(Text, Value, nullptr, nullptr, PPF_None);
	Writer.String(Text);
}

TArray<FRocksPropertyDumper::FReference> FRocksPropertyDumper::GetReferences() const
{
	TArray<FReference> Result;
	for (const TPair<FString, bool>& Reference : References)
	{
		Result.Add({Reference.Key, Reference.Value});
	}

	Result.Sort([](const FReference& Left, const FReference& Right) { return FRocksJsonWriter::CompareUtf8(Left.Path, Right.Path) < 0; });
	return Result;
}
