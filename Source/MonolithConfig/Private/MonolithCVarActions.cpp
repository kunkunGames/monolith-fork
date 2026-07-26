#include "MonolithCVarActions.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"

namespace MonolithCVarActionsDetail
{
	static const TCHAR* JsonTypeName(EJson Type)
	{
		switch (Type)
		{
		case EJson::None:    return TEXT("none");
		case EJson::Null:    return TEXT("null");
		case EJson::String:  return TEXT("string");
		case EJson::Number:  return TEXT("number");
		case EJson::Boolean: return TEXT("boolean");
		case EJson::Array:   return TEXT("array");
		case EJson::Object:  return TEXT("object");
		default:             return TEXT("unknown");
		}
	}

	static bool ReadString(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Name,
		bool bRequired,
		const FString& DefaultValue,
		FString& OutValue,
		FMonolithActionResult& OutError)
	{
		OutValue = DefaultValue;
		if (!Params.IsValid())
		{
			if (!bRequired)
			{
				return true;
			}

			OutError = FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required parameter: %s"), Name),
				FMonolithJsonUtils::ErrInvalidParams);
			return false;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(Name);
		if (!Value.IsValid())
		{
			if (!bRequired)
			{
				return true;
			}

			OutError = FMonolithActionResult::Error(
				FString::Printf(TEXT("Missing required parameter: %s"), Name),
				FMonolithJsonUtils::ErrInvalidParams);
			return false;
		}

		if (Value->Type != EJson::String)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Invalid parameter '%s': expected string, got %s"),
					Name,
					JsonTypeName(Value->Type)),
				FMonolithJsonUtils::ErrInvalidParams);
			return false;
		}

		OutValue = Value->AsString();
		if (bRequired && OutValue.IsEmpty())
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(TEXT("Parameter '%s' must not be empty"), Name),
				FMonolithJsonUtils::ErrInvalidParams);
			return false;
		}

		return true;
	}

	static bool ReadLimit(
		const TSharedPtr<FJsonObject>& Params,
		int32& OutLimit,
		FMonolithActionResult& OutError)
	{
		constexpr int32 DefaultLimit = 100;
		constexpr int32 MaxLimit = 200;
		OutLimit = DefaultLimit;
		if (!Params.IsValid())
		{
			return true;
		}

		const TSharedPtr<FJsonValue> Value = Params->TryGetField(TEXT("limit"));
		if (!Value.IsValid())
		{
			return true;
		}

		if (Value->Type != EJson::Number)
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Invalid parameter 'limit': expected integer, got %s"),
					JsonTypeName(Value->Type)),
				FMonolithJsonUtils::ErrInvalidParams);
			return false;
		}

		const double Number = Value->AsNumber();
		if (!FMath::IsFinite(Number)
			|| Number != FMath::TruncToDouble(Number)
			|| Number < 1.0
			|| Number > static_cast<double>(MaxLimit))
		{
			OutError = FMonolithActionResult::Error(
				FString::Printf(
					TEXT("Invalid parameter 'limit': expected an integer from 1 to %d"),
					MaxLimit),
				FMonolithJsonUtils::ErrInvalidParams);
			return false;
		}

		OutLimit = static_cast<int32>(Number);
		return true;
	}

	static TSharedPtr<FJsonObject> CVarToJson(
		const FString& Name,
		IConsoleVariable* Variable,
		bool bIncludeHelp)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		Object->SetStringField(TEXT("name"), Name);
		Object->SetBoolField(TEXT("found"), Variable != nullptr);
		if (!Variable)
		{
			return Object;
		}

		Object->SetStringField(TEXT("value"), Variable->GetString());
		if (bIncludeHelp)
		{
			Object->SetStringField(TEXT("help"), Variable->GetHelp());
		}
		Object->SetNumberField(
			TEXT("flags"),
			static_cast<double>(static_cast<uint32>(Variable->GetFlags())));
		Object->SetBoolField(TEXT("read_only"), Variable->TestFlags(ECVF_ReadOnly));
		Object->SetBoolField(TEXT("cheat"), Variable->TestFlags(ECVF_Cheat));
		Object->SetStringField(TEXT("set_by"), GetConsoleVariableSetByName(Variable->GetFlags()));
		return Object;
	}

	static bool LessByStableName(const FString& A, const FString& B)
	{
		const int32 IgnoreCaseOrder = A.Compare(B, ESearchCase::IgnoreCase);
		return IgnoreCaseOrder == 0
			? A.Compare(B, ESearchCase::CaseSensitive) < 0
			: IgnoreCaseOrder < 0;
	}
}

void FMonolithCVarActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(
		TEXT("config"),
		TEXT("get_cvar"),
		TEXT("Get one live console variable value, help text, flags, and set-by source. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithCVarActions::GetCVar),
		FParamSchemaBuilder()
			.Required(TEXT("name"), TEXT("string"), TEXT("Exact console variable name"))
			.Build());

	Registry.RegisterAction(
		TEXT("config"),
		TEXT("find_cvars"),
		TEXT("Find live console variables by prefix or substring with deterministic, bounded results. Read-only."),
		FMonolithActionHandler::CreateStatic(&FMonolithCVarActions::FindCVars),
		FParamSchemaBuilder()
			.Optional(TEXT("query"), TEXT("string"), TEXT("Prefix or substring to search for"))
			.Optional(TEXT("mode"), TEXT("string"), TEXT("Search mode: prefix or contains"), TEXT("prefix"))
			.Optional(TEXT("limit"), TEXT("integer"), TEXT("Maximum rows to return (1-200)"), TEXT("100"))
			.Build());

	Registry.SetActionAnnotations(
		TEXT("config"),
		TEXT("get_cvar"),
		true,
		false,
		true,
		TEXT("Get console variable"));
	Registry.SetActionAnnotations(
		TEXT("config"),
		TEXT("find_cvars"),
		true,
		false,
		true,
		TEXT("Find console variables"));
}

FMonolithActionResult FMonolithCVarActions::GetCVar(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCVarActionsDetail;

	FString Name;
	FMonolithActionResult ParamError;
	if (!ReadString(Params, TEXT("name"), true, FString(), Name, ParamError))
	{
		return ParamError;
	}

	IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
	return FMonolithActionResult::Success(CVarToJson(Name, Variable, true));
}

FMonolithActionResult FMonolithCVarActions::FindCVars(const TSharedPtr<FJsonObject>& Params)
{
	using namespace MonolithCVarActionsDetail;

	FString Query;
	FString Mode;
	int32 Limit = 0;
	FMonolithActionResult ParamError;
	if (!ReadString(Params, TEXT("query"), false, FString(), Query, ParamError)
		|| !ReadString(Params, TEXT("mode"), false, TEXT("prefix"), Mode, ParamError)
		|| !ReadLimit(Params, Limit, ParamError))
	{
		return ParamError;
	}

	Mode = Mode.ToLower();
	if (Mode != TEXT("prefix") && Mode != TEXT("contains"))
	{
		return FMonolithActionResult::Error(
			TEXT("Invalid parameter 'mode': expected 'prefix' or 'contains'"),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<FString> MatchingNames;
	FConsoleObjectVisitor Visitor = FConsoleObjectVisitor::CreateLambda(
		[&MatchingNames](const TCHAR* RawName, IConsoleObject* Object)
		{
			if (RawName && Object && Object->AsVariable())
			{
				MatchingNames.Add(RawName);
			}
		});

	if (Mode == TEXT("contains") && !Query.IsEmpty())
	{
		IConsoleManager::Get().ForEachConsoleObjectThatContains(Visitor, *Query);
	}
	else
	{
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(Visitor, *Query);
	}

	MatchingNames.Sort(&LessByStableName);

	const int32 ReturnedCount = FMath::Min(Limit, MatchingNames.Num());
	TArray<TSharedPtr<FJsonValue>> Rows;
	Rows.Reserve(ReturnedCount);
	for (int32 Index = 0; Index < ReturnedCount; ++Index)
	{
		const FString& Name = MatchingNames[Index];
		IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*Name);
		if (Variable)
		{
			Rows.Add(MakeShared<FJsonValueObject>(CVarToJson(Name, Variable, false)));
		}
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("query"), Query);
	Result->SetStringField(TEXT("mode"), Mode);
	Result->SetNumberField(TEXT("limit"), Limit);
	Result->SetNumberField(TEXT("matched_count"), MatchingNames.Num());
	Result->SetNumberField(TEXT("returned_count"), Rows.Num());
	Result->SetBoolField(TEXT("truncated"), MatchingNames.Num() > Rows.Num());
	Result->SetNumberField(
		TEXT("truncated_remaining"),
		FMath::Max(0, MatchingNames.Num() - Rows.Num()));
	Result->SetArrayField(TEXT("cvars"), Rows);
	return FMonolithActionResult::Success(Result);
}
