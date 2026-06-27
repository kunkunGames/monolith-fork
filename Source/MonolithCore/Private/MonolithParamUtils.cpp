// Copyright Monolith. All Rights Reserved.

#include "MonolithParamUtils.h"
#include "Editor.h"
#include "Engine/World.h"
#include "Misc/Paths.h"

namespace MonolithParamUtils
{

namespace
{
FString ParamTypeError(const FString& Key, const TCHAR* ExpectedType)
{
	return FString::Printf(TEXT("Parameter '%s' must be %s"), *Key, ExpectedType);
}

FString MissingParamError(const FString& Key)
{
	return FString::Printf(TEXT("Missing required field: %s"), *Key);
}

FString NonEmptyParamError(const FString& Key)
{
	return FString::Printf(TEXT("Parameter '%s' must be a non-empty string"), *Key);
}

bool IsJsonString(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::String;
}

bool IsJsonNumber(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::Number;
}

bool IsJsonBool(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::Boolean;
}

bool IsJsonArray(const TSharedPtr<FJsonValue>& Value)
{
	return Value.IsValid() && Value->Type == EJson::Array;
}
}

bool GetRequiredStringParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, FString& OutValue, FString& OutError, bool bTrim)
{
	OutValue.Reset();
	if (!Params.IsValid())
	{
		OutError = MissingParamError(Key);
		return false;
	}
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
	if (!Value.IsValid())
	{
		OutError = MissingParamError(Key);
		return false;
	}
	if (!IsJsonString(Value) || !Value->TryGetString(OutValue))
	{
		OutError = ParamTypeError(Key, TEXT("a string"));
		return false;
	}
	if (bTrim)
	{
		OutValue.TrimStartAndEndInline();
	}
	if (OutValue.IsEmpty())
	{
		OutError = NonEmptyParamError(Key);
		return false;
	}
	return true;
}

bool GetOptionalStringParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, FString& OutValue, FString& OutError, const FString& DefaultValue, bool bTrim)
{
	OutValue = DefaultValue;
	if (!Params.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
	if (!Value.IsValid())
	{
		return true;
	}
	if (!IsJsonString(Value) || !Value->TryGetString(OutValue))
	{
		OutError = ParamTypeError(Key, TEXT("a string"));
		return false;
	}
	if (bTrim)
	{
		OutValue.TrimStartAndEndInline();
	}
	return true;
}

bool GetOptionalClampedIntParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, int32& OutValue, FString& OutError, int32 DefaultValue, int32 MinValue, int32 MaxValue)
{
	const int32 Lo = FMath::Min(MinValue, MaxValue);
	const int32 Hi = FMath::Max(MinValue, MaxValue);
	OutValue = FMath::Clamp(DefaultValue, Lo, Hi);
	if (!Params.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
	if (!Value.IsValid())
	{
		return true;
	}
	double Raw = 0.0;
	if (!IsJsonNumber(Value) || !Value->TryGetNumber(Raw))
	{
		OutError = ParamTypeError(Key, TEXT("an integer"));
		return false;
	}
	const double Rounded = FMath::RoundToDouble(Raw);
	if (!FMath::IsNearlyEqual(Raw, Rounded))
	{
		OutError = FString::Printf(TEXT("Parameter '%s' must be an integer, got %g"), *Key, Raw);
		return false;
	}
	OutValue = FMath::Clamp(static_cast<int32>(Rounded), Lo, Hi);
	return true;
}

bool GetOptionalClampedDoubleParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, double& OutValue, FString& OutError, double DefaultValue, double MinValue, double MaxValue)
{
	const double Lo = FMath::Min(MinValue, MaxValue);
	const double Hi = FMath::Max(MinValue, MaxValue);
	OutValue = FMath::Clamp(DefaultValue, Lo, Hi);
	if (!Params.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
	if (!Value.IsValid())
	{
		return true;
	}
	double Raw = 0.0;
	if (!IsJsonNumber(Value) || !Value->TryGetNumber(Raw))
	{
		OutError = ParamTypeError(Key, TEXT("a number"));
		return false;
	}
	OutValue = FMath::Clamp(Raw, Lo, Hi);
	return true;
}

bool GetOptionalBoolParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, bool& OutValue, FString& OutError, bool DefaultValue)
{
	OutValue = DefaultValue;
	if (!Params.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
	if (!Value.IsValid())
	{
		return true;
	}
	if (!IsJsonBool(Value) || !Value->TryGetBool(OutValue))
	{
		OutError = ParamTypeError(Key, TEXT("a boolean"));
		return false;
	}
	return true;
}

bool GetOptionalStringArrayParam(const TSharedPtr<FJsonObject>& Params, const FString& Key, TArray<FString>& OutValues, FString& OutError, const TArray<FString>& DefaultValues)
{
	OutValues = DefaultValues;
	if (!Params.IsValid())
	{
		return true;
	}
	const TSharedPtr<FJsonValue> Value = Params->TryGetField(Key);
	if (!Value.IsValid())
	{
		return true;
	}
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!IsJsonArray(Value) || !Value->TryGetArray(Values) || !Values)
	{
		OutError = ParamTypeError(Key, TEXT("an array of strings"));
		return false;
	}
	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (int32 Index = 0; Index < Values->Num(); ++Index)
	{
		FString Item;
		if (!IsJsonString((*Values)[Index]) || !(*Values)[Index]->TryGetString(Item))
		{
			OutError = FString::Printf(TEXT("Parameter '%s[%d]' must be a string"), *Key, Index);
			return false;
		}
		OutValues.Add(Item);
	}
	return true;
}

bool TryParseStrictInt(const FString& Text, int32& OutValue, FString& OutError, const FString& Context)
{
	const FString Trimmed = Text.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Invalid integer for %s: value is empty"), *Context);
		return false;
	}
	int32 DigitStart = 0;
	if (Trimmed[0] == TEXT('+') || Trimmed[0] == TEXT('-'))
	{
		DigitStart = 1;
	}
	if (DigitStart >= Trimmed.Len())
	{
		OutError = FString::Printf(TEXT("Invalid integer for %s: '%s'"), *Context, *Trimmed);
		return false;
	}
	for (int32 Index = DigitStart; Index < Trimmed.Len(); ++Index)
	{
		if (!FChar::IsDigit(Trimmed[Index]))
		{
			OutError = FString::Printf(TEXT("Invalid integer for %s: '%s'"), *Context, *Trimmed);
			return false;
		}
	}
	if (!LexTryParseString(OutValue, *Trimmed))
	{
		OutError = FString::Printf(TEXT("Invalid integer for %s: '%s'"), *Context, *Trimmed);
		return false;
	}
	return true;
}

bool ParseVector(const TSharedPtr<FJsonObject>& Params, const FString& Key, FVector& Out)
{
	// Try array format: [x, y, z]
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Params->TryGetArrayField(Key, Arr) && Arr && Arr->Num() >= 3)
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
		if ((*Arr)[0]->TryGetNumber(X) && (*Arr)[1]->TryGetNumber(Y) && (*Arr)[2]->TryGetNumber(Z))
		{
			Out.X = X;
			Out.Y = Y;
			Out.Z = Z;
			return true;
		}
	}

	// Try object format: {x, y, z}
	const TSharedPtr<FJsonObject>* Obj;
	if (Params->TryGetObjectField(Key, Obj))
	{
		double X = 0.0, Y = 0.0, Z = 0.0;
		if ((*Obj)->TryGetNumberField(TEXT("x"), X) &&
			(*Obj)->TryGetNumberField(TEXT("y"), Y) &&
			(*Obj)->TryGetNumberField(TEXT("z"), Z))
		{
			Out.X = X;
			Out.Y = Y;
			Out.Z = Z;
			return true;
		}
	}

	return false;
}

bool ParseRotator(const TSharedPtr<FJsonObject>& Params, const FString& Key, FRotator& Out)
{
	// Try array format: [pitch, yaw, roll]
	const TArray<TSharedPtr<FJsonValue>>* Arr;
	if (Params->TryGetArrayField(Key, Arr) && Arr && Arr->Num() >= 3)
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		if ((*Arr)[0]->TryGetNumber(Pitch) && (*Arr)[1]->TryGetNumber(Yaw) && (*Arr)[2]->TryGetNumber(Roll))
		{
			Out.Pitch = Pitch;
			Out.Yaw = Yaw;
			Out.Roll = Roll;
			return true;
		}
	}

	// Try object format: {pitch, yaw, roll}
	const TSharedPtr<FJsonObject>* Obj;
	if (Params->TryGetObjectField(Key, Obj))
	{
		double Pitch = 0.0, Yaw = 0.0, Roll = 0.0;
		if ((*Obj)->TryGetNumberField(TEXT("pitch"), Pitch) &&
			(*Obj)->TryGetNumberField(TEXT("yaw"), Yaw) &&
			(*Obj)->TryGetNumberField(TEXT("roll"), Roll))
		{
			Out.Pitch = Pitch;
			Out.Yaw = Yaw;
			Out.Roll = Roll;
			return true;
		}
	}

	return false;
}

UWorld* GetEditorWorld()
{
	if (GEditor)
	{
		UWorld* World = GEditor->GetEditorWorldContext().World();
		if (World)
		{
			return World;
		}
	}
	return nullptr;
}

TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
{
	TArray<TSharedPtr<FJsonValue>> Arr;
	Arr.Reserve(3);
	Arr.Add(MakeShared<FJsonValueNumber>(V.X));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
	Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
	return Arr;
}

FString NormalizeBlueprintClassPath(const FString& BlueprintPath)
{
	FString ClassPath = BlueprintPath;
	if (!ClassPath.Contains(TEXT(".")))
	{
		FString BaseName = FPaths::GetBaseFilename(ClassPath);
		ClassPath = ClassPath + TEXT(".") + BaseName + TEXT("_C");
	}
	else if (!ClassPath.EndsWith(TEXT("_C")))
	{
		ClassPath += TEXT("_C");
	}
	return ClassPath;
}

bool ParseMobility(const FString& MobilityStr, EComponentMobility::Type& OutMobility)
{
	if (MobilityStr.Equals(TEXT("static"), ESearchCase::IgnoreCase))
	{
		OutMobility = EComponentMobility::Static;
		return true;
	}
	if (MobilityStr.Equals(TEXT("stationary"), ESearchCase::IgnoreCase))
	{
		OutMobility = EComponentMobility::Stationary;
		return true;
	}
	if (MobilityStr.Equals(TEXT("movable"), ESearchCase::IgnoreCase))
	{
		OutMobility = EComponentMobility::Movable;
		return true;
	}
	return false;
}

} // namespace MonolithParamUtils
