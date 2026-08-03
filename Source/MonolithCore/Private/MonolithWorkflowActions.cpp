#include "MonolithWorkflowActions.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		Result.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Result.Add(MakeShared<FJsonValueString>(Value));
		}
		return Result;
	}

	TSharedPtr<FJsonObject> MakeActionParams(const FString& ParamName, const FString& AssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(ParamName, AssetPath);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeEmptyParams()
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> MakeStringArrayParams(const FString& ParamName, const TArray<FString>& Values)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(ParamName, StringsToJson(Values));
		return Params;
	}

	void CopyJsonField(const TSharedPtr<FJsonObject>& Source, const TCHAR* FieldName, const TSharedPtr<FJsonObject>& Dest)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}

		TSharedPtr<FJsonValue> Value = Source->TryGetField(FieldName);
		if (Value.IsValid())
		{
			Dest->SetField(FieldName, Value);
		}
	}

	void CopyJsonFields(const TSharedPtr<FJsonObject>& Source, const TSharedPtr<FJsonObject>& Dest)
	{
		if (!Source.IsValid() || !Dest.IsValid())
		{
			return;
		}

		for (const auto& Pair : Source->Values)
		{
			if (Pair.Value.IsValid())
			{
				Dest->SetField(Pair.Key, Pair.Value);
			}
		}
	}

	TSharedPtr<FJsonObject> MakeWorkflowStep(
		const FString& Id,
		const FString& ActionId,
		const FString& Status,
		const FString& Description)
	{
		TSharedPtr<FJsonObject> Step = MakeShared<FJsonObject>();
		Step->SetStringField(TEXT("id"), Id);
		Step->SetStringField(TEXT("action_id"), ActionId);
		Step->SetStringField(TEXT("status"), Status);
		Step->SetStringField(TEXT("description"), Description);
		return Step;
	}

	TSharedPtr<FJsonObject> MakeActionRow(
		const FString& ActionId,
		const FString& Status,
		bool bExecuted,
		bool bAvailable,
		const TSharedPtr<FJsonObject>& Params)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("action_id"), ActionId);
		Row->SetStringField(TEXT("status"), Status);
		Row->SetBoolField(TEXT("executed"), bExecuted);
		Row->SetBoolField(TEXT("available"), bAvailable);
		Row->SetBoolField(TEXT("requires_live_editor"), true);
		if (Params.IsValid())
		{
			Row->SetObjectField(TEXT("params"), Params);
		}
		return Row;
	}

	TSharedPtr<FJsonObject> MakeNextAction(
		const FString& ActionId,
		bool bAvailable,
		bool bRequiresLiveEditor,
		const FString& Reason,
		const TSharedPtr<FJsonObject>& Params = nullptr)
	{
		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetStringField(TEXT("action"), ActionId);
		Row->SetBoolField(TEXT("available"), bAvailable);
		Row->SetBoolField(TEXT("requires_live_editor"), bRequiresLiveEditor);
		Row->SetStringField(TEXT("reason"), Reason);
		if (Params.IsValid())
		{
			Row->SetObjectField(TEXT("params"), Params);
		}
		return Row;
	}

	TSharedPtr<FJsonObject> MakeUnavailableProof(const FString& Status, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetStringField(TEXT("status"), Status);
		Obj->SetStringField(TEXT("reason"), Reason);
		return Obj;
	}

	bool IsMobileOrConsoleVisualProfileName(const FString& ProfileName)
	{
		return ProfileName.Contains(TEXT("mobile"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("console"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("handheld"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("switch"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("steamdeck"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("xbox"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("playstation"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("ps5"), ESearchCase::IgnoreCase)
			|| ProfileName.Contains(TEXT("tv"), ESearchCase::IgnoreCase);
	}

	TSharedPtr<FJsonObject> MakeUiProfileFinding(
		const FString& Severity,
		const FString& RuleId,
		const FString& ProfileName,
		const FString& Message,
		const FString& SuggestedFix)
	{
		TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
		Finding->SetStringField(TEXT("severity"), Severity);
		Finding->SetStringField(TEXT("category"), RuleId);
		Finding->SetStringField(TEXT("rule_id"), RuleId);
		Finding->SetStringField(TEXT("profile"), ProfileName);
		Finding->SetStringField(TEXT("message"), Message);
		Finding->SetStringField(TEXT("suggested_fix"), SuggestedFix);
		return Finding;
	}

	bool ValidateUiVisualProfilesForProof(
		const TArray<TSharedPtr<FJsonValue>>* VisualProfiles,
		bool bVisualProofRequested,
		TSharedPtr<FJsonObject>& OutProof,
		TArray<FString>& OutErrors)
	{
		OutProof = MakeShared<FJsonObject>();
		OutProof->SetStringField(TEXT("schema_version"), TEXT("ui_visual_profile_proof.v1"));
		OutProof->SetBoolField(TEXT("visual_artifacts_required"), bVisualProofRequested);

		TArray<TSharedPtr<FJsonValue>> Findings;
		int32 ProfileCount = 0;
		int32 RequiredProfileCount = 0;
		int32 ErrorCount = 0;

		if (!bVisualProofRequested)
		{
			OutProof->SetStringField(TEXT("status"), TEXT("not_requested"));
			OutProof->SetNumberField(TEXT("profile_count"), VisualProfiles ? VisualProfiles->Num() : 0);
			OutProof->SetNumberField(TEXT("mobile_console_profile_count"), 0);
			OutProof->SetNumberField(TEXT("error_count"), 0);
			OutProof->SetArrayField(TEXT("findings"), Findings);
			return true;
		}

		if (!VisualProfiles || VisualProfiles->Num() == 0)
		{
			OutProof->SetStringField(TEXT("status"), TEXT("planned_default_desktop"));
			OutProof->SetNumberField(TEXT("profile_count"), 0);
			OutProof->SetNumberField(TEXT("mobile_console_profile_count"), 0);
			OutProof->SetNumberField(TEXT("error_count"), 0);
			OutProof->SetArrayField(TEXT("findings"), Findings);
			return true;
		}

		ProfileCount = VisualProfiles->Num();
		for (int32 Index = 0; Index < VisualProfiles->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& ProfileValue = (*VisualProfiles)[Index];
			const TSharedPtr<FJsonObject> Profile = ProfileValue.IsValid() ? ProfileValue->AsObject() : nullptr;
			if (!Profile.IsValid())
			{
				++ErrorCount;
				const FString ProfileName = FString::Printf(TEXT("visual_profiles[%d]"), Index);
				Findings.Add(MakeShared<FJsonValueObject>(MakeUiProfileFinding(
					TEXT("error"),
					TEXT("VisualProfileShapeInvalid"),
					ProfileName,
					TEXT("Visual proof profile must be a JSON object."),
					TEXT("Pass visual_profiles rows as objects such as {\"name\":\"mobile\",\"resolution\":[1280,720],\"dpi_scale\":1.0,\"safe_zone\":{\"left\":48,\"top\":24,\"right\":48,\"bottom\":24}}."))));
				OutErrors.Add(FString::Printf(TEXT("VisualProfileShapeInvalid: %s must be an object."), *ProfileName));
				continue;
			}

			FString ProfileName;
			if (!Profile->TryGetStringField(TEXT("name"), ProfileName) || ProfileName.IsEmpty())
			{
				ProfileName = FString::Printf(TEXT("profile_%d"), Index);
			}

			if (!IsMobileOrConsoleVisualProfileName(ProfileName))
			{
				continue;
			}

			++RequiredProfileCount;
			double DpiScale = 0.0;
			const bool bHasDpi = Profile->TryGetNumberField(TEXT("dpi_scale"), DpiScale) && DpiScale > 0.0;
			const TSharedPtr<FJsonObject>* SafeZone = nullptr;
			const bool bHasSafeZone = Profile->TryGetObjectField(TEXT("safe_zone"), SafeZone) && SafeZone && SafeZone->IsValid();
			if (!bHasDpi || !bHasSafeZone)
			{
				++ErrorCount;
				TSharedPtr<FJsonObject> Finding = MakeUiProfileFinding(
					TEXT("error"),
					TEXT("DpiSafeZoneProfileMissing"),
					ProfileName,
					TEXT("Mobile/console visual proof profile omits explicit DPI scale or safe-zone inputs."),
					TEXT("Add dpi_scale and safe_zone:{left,top,right,bottom} to the visual profile, or rename the profile if it is not a mobile/console proof target."));
				Finding->SetBoolField(TEXT("has_dpi_scale"), bHasDpi);
				Finding->SetBoolField(TEXT("has_safe_zone"), bHasSafeZone);
				Findings.Add(MakeShared<FJsonValueObject>(Finding));
				OutErrors.Add(FString::Printf(TEXT("DpiSafeZoneProfileMissing: %s requires dpi_scale and safe_zone."), *ProfileName));
			}
		}

		OutProof->SetStringField(TEXT("status"), ErrorCount == 0 ? TEXT("pass") : TEXT("blocked"));
		OutProof->SetNumberField(TEXT("profile_count"), ProfileCount);
		OutProof->SetNumberField(TEXT("mobile_console_profile_count"), RequiredProfileCount);
		OutProof->SetNumberField(TEXT("error_count"), ErrorCount);
		OutProof->SetArrayField(TEXT("findings"), Findings);
		return ErrorCount == 0;
	}

	TSharedPtr<FJsonObject> MakeUiMeasureWidgetLayoutParams(
		const FString& WidgetAssetPath,
		const TArray<TSharedPtr<FJsonValue>>* VisualProfiles,
		const TSharedPtr<FJsonValue>& PreviewResolution)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
		Params->SetBoolField(TEXT("check_overlap"), true);
		Params->SetBoolField(TEXT("check_safe_zone"), true);
		Params->SetNumberField(TEXT("max_allowed_overlap_ratio"), 0.0);

		TArray<TSharedPtr<FJsonValue>> Profiles;
		if (VisualProfiles && VisualProfiles->Num() > 0 && (*VisualProfiles)[0].IsValid())
		{
			Profiles.Add((*VisualProfiles)[0]);
		}
		else if (PreviewResolution.IsValid())
		{
			TSharedPtr<FJsonObject> DefaultProfile = MakeShared<FJsonObject>();
			DefaultProfile->SetStringField(TEXT("name"), TEXT("desktop"));
			DefaultProfile->SetField(TEXT("resolution"), PreviewResolution);
			Profiles.Add(MakeShared<FJsonValueObject>(DefaultProfile));
		}

		if (Profiles.Num() > 0)
		{
			Params->SetArrayField(TEXT("profiles"), Profiles);
		}
		return Params;
	}

	constexpr int32 MaxSemanticPayloadDepth = 32;
	constexpr int32 MaxSemanticPayloadNodes = 16384;

	FString JsonTypeName(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return TEXT("invalid");
		}

		switch (Value->Type)
		{
		case EJson::None: return TEXT("none");
		case EJson::Null: return TEXT("null");
		case EJson::String: return TEXT("string");
		case EJson::Number: return TEXT("number");
		case EJson::Boolean: return TEXT("boolean");
		case EJson::Array: return TEXT("array");
		case EJson::Object: return TEXT("object");
		default: return TEXT("unknown");
		}
	}

	bool IsPrimitiveFailureStatus(const FString& Status)
	{
		FString Normalized = Status.TrimStartAndEnd().ToLower();
		Normalized.ReplaceInline(TEXT("-"), TEXT("_"));
		Normalized.ReplaceInline(TEXT(" "), TEXT("_"));

		static const TSet<FString> FailureTokens = {
			TEXT("fail"),
			TEXT("failed"),
			TEXT("failure"),
			TEXT("error"),
			TEXT("errored"),
			TEXT("critical"),
			TEXT("fatal"),
			TEXT("blocked"),
			TEXT("unavailable"),
			TEXT("invalid"),
			TEXT("rejected"),
			TEXT("cancelled"),
			TEXT("canceled"),
			TEXT("aborted"),
			TEXT("timeout"),
			TEXT("missing"),
			TEXT("disabled"),
			TEXT("unsupported"),
			TEXT("issues")
		};

		TArray<FString> Tokens;
		Normalized.ParseIntoArray(Tokens, TEXT("_"), true);
		for (const FString& Token : Tokens)
		{
			if (FailureTokens.Contains(Token))
			{
				return true;
			}
		}

		static const TArray<FString> FailurePhrases = {
			TEXT("not_found"),
			TEXT("not_supported"),
			TEXT("not_available"),
			TEXT("not_installed"),
			TEXT("not_started"),
			TEXT("timed_out")
		};
		for (const FString& FailurePhrase : FailurePhrases)
		{
			if (Normalized == FailurePhrase || Normalized.EndsWith(FString(TEXT("_")) + FailurePhrase))
			{
				return true;
			}
		}

		return Normalized == TEXT("no_editor_world")
			|| Normalized == TEXT("module_not_loaded");
	}

	bool IsHighSeverityFinding(const FString& Severity)
	{
		return Severity.Equals(TEXT("high"), ESearchCase::IgnoreCase)
			|| Severity.Equals(TEXT("error"), ESearchCase::IgnoreCase)
			|| Severity.Equals(TEXT("critical"), ESearchCase::IgnoreCase)
			|| Severity.Equals(TEXT("fatal"), ESearchCase::IgnoreCase);
	}

	void AddMalformedSemanticField(
		const FString& FieldPath,
		const TCHAR* ExpectedType,
		const TSharedPtr<FJsonValue>& ActualValue,
		TArray<FString>& OutFailures)
	{
		OutFailures.AddUnique(FString::Printf(
			TEXT("malformed semantic field %s: expected %s, got %s"),
			*FieldPath,
			ExpectedType,
			*JsonTypeName(ActualValue)));
	}

	void ValidatePrimitiveSemanticObject(
		const TSharedPtr<FJsonObject>& Object,
		const FString& Path,
		TArray<FString>& OutFailures)
	{
		if (!Object.IsValid())
		{
			OutFailures.AddUnique(FString::Printf(TEXT("malformed semantic object at %s"), *Path));
			return;
		}

		static const TCHAR* BooleanFailureFields[] = {
			TEXT("ok"),
			TEXT("success"),
			TEXT("bSuccess"),
			TEXT("passed")
		};
		for (const TCHAR* FieldName : BooleanFailureFields)
		{
			const TSharedPtr<FJsonValue>* FieldValue = Object->Values.Find(FieldName);
			if (!FieldValue)
			{
				continue;
			}

			const FString FieldPath = Path + TEXT(".") + FieldName;
			if (!FieldValue->IsValid() || (*FieldValue)->Type != EJson::Boolean)
			{
				AddMalformedSemanticField(FieldPath, TEXT("boolean"), FieldValue->IsValid() ? *FieldValue : nullptr, OutFailures);
			}
			else if (!(*FieldValue)->AsBool())
			{
				OutFailures.AddUnique(FieldPath + TEXT("=false"));
			}
		}

		if (const TSharedPtr<FJsonValue>* StatusValue = Object->Values.Find(TEXT("status")))
		{
			const FString FieldPath = Path + TEXT(".status");
			if (!StatusValue->IsValid() || (*StatusValue)->Type != EJson::String)
			{
				AddMalformedSemanticField(FieldPath, TEXT("string"), StatusValue->IsValid() ? *StatusValue : nullptr, OutFailures);
			}
			else
			{
				const FString Status = (*StatusValue)->AsString();
				if (Status.TrimStartAndEnd().IsEmpty())
				{
					OutFailures.AddUnique(FieldPath + TEXT(" is empty"));
				}
				else if (IsPrimitiveFailureStatus(Status))
				{
					OutFailures.AddUnique(FString::Printf(TEXT("%s=%s"), *FieldPath, *Status));
				}
			}
		}

		if (const TSharedPtr<FJsonValue>* ErrorCountValue = Object->Values.Find(TEXT("error_count")))
		{
			const FString FieldPath = Path + TEXT(".error_count");
			if (!ErrorCountValue->IsValid() || (*ErrorCountValue)->Type != EJson::Number)
			{
				AddMalformedSemanticField(FieldPath, TEXT("non-negative integer"), ErrorCountValue->IsValid() ? *ErrorCountValue : nullptr, OutFailures);
			}
			else
			{
				const double ErrorCount = (*ErrorCountValue)->AsNumber();
				if (!FMath::IsFinite(ErrorCount) || ErrorCount < 0.0 || ErrorCount != FMath::RoundToDouble(ErrorCount))
				{
					OutFailures.AddUnique(FString::Printf(TEXT("malformed semantic field %s: expected non-negative integer, got %g"), *FieldPath, ErrorCount));
				}
				else if (ErrorCount > 0.0)
				{
					OutFailures.AddUnique(FString::Printf(TEXT("%s=%g"), *FieldPath, ErrorCount));
				}
			}
		}

		if (const TSharedPtr<FJsonValue>* ErrorsValue = Object->Values.Find(TEXT("errors")))
		{
			const FString FieldPath = Path + TEXT(".errors");
			if (!ErrorsValue->IsValid() || (*ErrorsValue)->Type != EJson::Array)
			{
				AddMalformedSemanticField(FieldPath, TEXT("array"), ErrorsValue->IsValid() ? *ErrorsValue : nullptr, OutFailures);
			}
			else if ((*ErrorsValue)->AsArray().Num() > 0)
			{
				OutFailures.AddUnique(FString::Printf(TEXT("%s[%d] is non-empty"), *FieldPath, (*ErrorsValue)->AsArray().Num()));
			}
		}

		if (const TSharedPtr<FJsonValue>* FindingsValue = Object->Values.Find(TEXT("findings")))
		{
			const FString FieldPath = Path + TEXT(".findings");
			if (!FindingsValue->IsValid() || (*FindingsValue)->Type != EJson::Array)
			{
				AddMalformedSemanticField(FieldPath, TEXT("array"), FindingsValue->IsValid() ? *FindingsValue : nullptr, OutFailures);
			}
		}

		const TSharedPtr<FJsonValue>* RequiredValue = Object->Values.Find(TEXT("required"));
		const TSharedPtr<FJsonValue>* SeverityValue = Object->Values.Find(TEXT("severity"));
		if (RequiredValue && SeverityValue)
		{
			const FString RequiredPath = Path + TEXT(".required");
			const FString SeverityPath = Path + TEXT(".severity");
			const bool bRequiredValid = RequiredValue->IsValid() && (*RequiredValue)->Type == EJson::Boolean;
			const bool bSeverityValid = SeverityValue->IsValid() && (*SeverityValue)->Type == EJson::String;
			if (!bRequiredValid)
			{
				AddMalformedSemanticField(RequiredPath, TEXT("boolean"), RequiredValue->IsValid() ? *RequiredValue : nullptr, OutFailures);
			}
			if (!bSeverityValid)
			{
				AddMalformedSemanticField(SeverityPath, TEXT("string"), SeverityValue->IsValid() ? *SeverityValue : nullptr, OutFailures);
			}
			if (bRequiredValid && bSeverityValid && (*RequiredValue)->AsBool())
			{
				const FString Severity = (*SeverityValue)->AsString();
				if (IsHighSeverityFinding(Severity))
				{
					OutFailures.AddUnique(FString::Printf(TEXT("required %s finding at %s"), *Severity, *Path));
				}
			}
		}
	}

	bool WalkPrimitiveSemanticValue(
		const TSharedPtr<FJsonValue>& Value,
		const FString& Path,
		int32 Depth,
		int32& InOutNodeCount,
		TArray<FString>& OutFailures)
	{
		if (Depth > MaxSemanticPayloadDepth)
		{
			OutFailures.AddUnique(FString::Printf(
				TEXT("semantic payload depth budget exceeded at %s (max=%d)"),
				*Path,
				MaxSemanticPayloadDepth));
			return false;
		}

		++InOutNodeCount;
		if (InOutNodeCount > MaxSemanticPayloadNodes)
		{
			OutFailures.AddUnique(FString::Printf(
				TEXT("semantic payload node budget exceeded at %s (max=%d)"),
				*Path,
				MaxSemanticPayloadNodes));
			return false;
		}

		if (!Value.IsValid())
		{
			OutFailures.AddUnique(FString::Printf(TEXT("malformed JSON value at %s"), *Path));
			return true;
		}

		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Object = Value->AsObject();
			ValidatePrimitiveSemanticObject(Object, Path, OutFailures);
			if (!Object.IsValid())
			{
				return true;
			}

			for (const auto& Pair : Object->Values)
			{
				if (!WalkPrimitiveSemanticValue(
					Pair.Value,
					Path + TEXT(".") + Pair.Key,
					Depth + 1,
					InOutNodeCount,
					OutFailures))
				{
					return false;
				}
			}
		}
		else if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Array = Value->AsArray();
			for (int32 Index = 0; Index < Array.Num(); ++Index)
			{
				if (!WalkPrimitiveSemanticValue(
					Array[Index],
					FString::Printf(TEXT("%s[%d]"), *Path, Index),
					Depth + 1,
					InOutNodeCount,
					OutFailures))
				{
					return false;
				}
			}
		}

		return true;
	}

	void CollectPrimitiveSemanticFailures(
		const TSharedPtr<FJsonObject>& Payload,
		TArray<FString>& OutFailures)
	{
		if (!Payload.IsValid())
		{
			OutFailures.AddUnique(TEXT("semantic payload is missing at $"));
			return;
		}

		int32 NodeCount = 0;
		WalkPrimitiveSemanticValue(
			MakeShared<FJsonValueObject>(Payload),
			TEXT("$"),
			0,
			NodeCount,
			OutFailures);
		OutFailures.Sort();
	}

	bool IsPrimitiveResultSuccessful(
		const FMonolithActionResult& Result,
		TArray<FString>& OutSemanticFailures)
	{
		if (!Result.bSuccess)
		{
			return false;
		}

		CollectPrimitiveSemanticFailures(Result.Result, OutSemanticFailures);
		return OutSemanticFailures.Num() == 0;
	}

	void AddSemanticFailureFields(
		const TSharedPtr<FJsonObject>& Object,
		const TArray<FString>& SemanticFailures)
	{
		if (!Object.IsValid() || SemanticFailures.Num() == 0)
		{
			return;
		}

		Object->SetBoolField(TEXT("transport_success"), true);
		Object->SetBoolField(TEXT("semantic_ok"), false);
		Object->SetArrayField(TEXT("semantic_failures"), StringsToJson(SemanticFailures));
	}

	void AddPrimitiveFailureError(
		const FString& ActionId,
		const TArray<FString>& SemanticFailures,
		TArray<FString>& OutErrors)
	{
		OutErrors.Add(FString::Printf(
			TEXT("%s semantic gate failed: %s"),
			*ActionId,
			*FString::Join(SemanticFailures, TEXT("; "))));
	}

	TSharedPtr<FJsonObject> MakeActionResultProof(const FMonolithActionResult& Result)
	{
		TArray<FString> SemanticFailures;
		const bool bSucceeded = IsPrimitiveResultSuccessful(Result, SemanticFailures);
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		Obj->SetBoolField(TEXT("success"), bSucceeded);
		Obj->SetStringField(TEXT("status"), bSucceeded ? TEXT("succeeded") : TEXT("failed"));
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Obj->SetObjectField(TEXT("result"), Result.Result);
		}
		if (!Result.bSuccess)
		{
			Obj->SetStringField(TEXT("error"), Result.ErrorMessage);
			Obj->SetNumberField(TEXT("error_code"), Result.ErrorCode);
			if (Result.ErrorData.IsValid())
			{
				Obj->SetObjectField(TEXT("error_data"), Result.ErrorData);
			}
		}
		else
		{
			AddSemanticFailureFields(Obj, SemanticFailures);
		}
		return Obj;
	}

	bool ExecuteReadOnlyPrimitive(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutProof,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<FString>& OutErrors)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		const FString ActionId = Namespace + TEXT(".") + Action;
		const bool bAvailable = Registry.HasAction(Namespace, Action);
		if (!bAvailable)
		{
			OutProof = MakeUnavailableProof(TEXT("unavailable"), ActionId + TEXT(" is not registered in the current Monolith profile."));
			OutActions.Add(MakeShared<FJsonValueObject>(MakeActionRow(ActionId, TEXT("unavailable"), false, false, Params)));
			OutErrors.Add(ActionId + TEXT(" unavailable"));
			return false;
		}

		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, Params);
		TArray<FString> SemanticFailures;
		const bool bSucceeded = IsPrimitiveResultSuccessful(Result, SemanticFailures);
		OutProof = MakeActionResultProof(Result);
		TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, bSucceeded ? TEXT("succeeded") : TEXT("failed"), true, true, Params);
		AddSemanticFailureFields(Row, SemanticFailures);
		OutActions.Add(MakeShared<FJsonValueObject>(Row));
		if (!Result.bSuccess)
		{
			OutErrors.Add(ActionId + TEXT(": ") + Result.ErrorMessage);
		}
		else if (!bSucceeded)
		{
			AddPrimitiveFailureError(ActionId, SemanticFailures, OutErrors);
		}
		return bSucceeded;
	}

	bool PlanOrExecutePrimitive(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		bool bExecute,
		bool bUnavailableBlocks,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<TSharedPtr<FJsonValue>>& OutProofRows,
		TArray<FString>& OutErrors)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		const FString ActionId = Namespace + TEXT(".") + Action;
		const bool bAvailable = Registry.HasAction(Namespace, Action);

		if (!bExecute)
		{
			TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, TEXT("planned"), false, bAvailable, Params);
			OutActions.Add(MakeShared<FJsonValueObject>(Row));
			OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
			return true;
		}

		if (!bAvailable)
		{
			TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, TEXT("unavailable"), false, false, Params);
			Row->SetStringField(TEXT("reason"), ActionId + TEXT(" is not registered in the current Monolith profile."));
			OutActions.Add(MakeShared<FJsonValueObject>(Row));
			OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
			if (bUnavailableBlocks)
			{
				OutErrors.Add(ActionId + TEXT(" unavailable"));
			}
			return !bUnavailableBlocks;
		}

		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, Params);
		TArray<FString> SemanticFailures;
		const bool bSucceeded = IsPrimitiveResultSuccessful(Result, SemanticFailures);
		TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, bSucceeded ? TEXT("succeeded") : TEXT("failed"), true, true, Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Row->SetObjectField(TEXT("result"), Result.Result);
		}
		else if (!Result.bSuccess)
		{
			Row->SetStringField(TEXT("error"), Result.ErrorMessage);
			Row->SetNumberField(TEXT("error_code"), Result.ErrorCode);
			if (Result.ErrorData.IsValid())
			{
				Row->SetObjectField(TEXT("error_data"), Result.ErrorData);
			}
			OutErrors.Add(ActionId + TEXT(": ") + Result.ErrorMessage);
		}
		if (Result.bSuccess)
		{
			AddSemanticFailureFields(Row, SemanticFailures);
		}
		if (Result.bSuccess && !bSucceeded)
		{
			AddPrimitiveFailureError(ActionId, SemanticFailures, OutErrors);
		}
		OutActions.Add(MakeShared<FJsonValueObject>(Row));
		OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
		return bSucceeded;
	}

	TSharedPtr<FJsonObject> MakeSourceControlObject(const FString& Status, const TArray<FString>& Paths, const TArray<FString>& Blockers)
	{
		TSharedPtr<FJsonObject> SourceControl = MakeShared<FJsonObject>();
		SourceControl->SetStringField(TEXT("provider"), TEXT(""));
		SourceControl->SetBoolField(TEXT("prepared"), false);
		SourceControl->SetStringField(TEXT("status"), Status);
		SourceControl->SetArrayField(TEXT("paths"), StringsToJson(Paths));
		SourceControl->SetArrayField(TEXT("checked_out"), {});
		SourceControl->SetArrayField(TEXT("marked_for_add"), {});
		SourceControl->SetArrayField(TEXT("blocked"), StringsToJson(Blockers));
		return SourceControl;
	}

	TSharedPtr<FJsonObject> MakeTouchedObject(
		const TArray<FString>& Actors,
		const TArray<FString>& Assets,
		const TArray<FString>& Packages,
		const TArray<FString>& Files)
	{
		TSharedPtr<FJsonObject> Touched = MakeShared<FJsonObject>();
		Touched->SetArrayField(TEXT("actors"), StringsToJson(Actors));
		Touched->SetArrayField(TEXT("assets"), StringsToJson(Assets));
		Touched->SetArrayField(TEXT("packages"), StringsToJson(Packages));
		Touched->SetArrayField(TEXT("files"), StringsToJson(Files));
		return Touched;
	}

	TSharedPtr<FJsonObject> MakePlanObject(
		const TArray<TSharedPtr<FJsonValue>>& Steps,
		const TArray<FString>& Preconditions,
		const TArray<FString>& OptionalDependencies)
	{
		TSharedPtr<FJsonObject> Plan = MakeShared<FJsonObject>();
		Plan->SetArrayField(TEXT("steps"), Steps);
		Plan->SetArrayField(TEXT("preconditions"), StringsToJson(Preconditions));
		Plan->SetArrayField(TEXT("optional_dependencies"), StringsToJson(OptionalDependencies));
		return Plan;
	}

	TSharedPtr<FJsonObject> GetObjectFieldOrEmpty(const TSharedPtr<FJsonObject>& Parent, const TCHAR* FieldName)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		if (Parent.IsValid() && Parent->TryGetObjectField(FieldName, Obj) && Obj && Obj->IsValid())
		{
			return *Obj;
		}
		return MakeShared<FJsonObject>();
	}

	TArray<FString> GetStringArrayField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* FieldName)
	{
		TArray<FString> Result;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Obj.IsValid() || !Obj->TryGetArrayField(FieldName, Values) || !Values)
		{
			return Result;
		}

		Result.Reserve(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString StringValue;
			if (Value.IsValid() && Value->TryGetString(StringValue) && !StringValue.IsEmpty())
			{
				Result.Add(StringValue);
			}
		}
		return Result;
	}

	void AddUniqueAssetPath(TArray<FString>& OutAssets, const FString& Value)
	{
		if (Value.StartsWith(TEXT("/Game")) && !OutAssets.Contains(Value))
		{
			OutAssets.Add(Value);
		}
	}

	void AddUniqueWorkflowString(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty() && !Values.Contains(Value))
		{
			Values.Add(Value);
		}
	}

	FString AssetSearchTokenFromPath(const FString& AssetPath)
	{
		FString Token = AssetPath;
		int32 SlashIndex = INDEX_NONE;
		if (Token.FindLastChar(TEXT('/'), SlashIndex))
		{
			Token = Token.RightChop(SlashIndex + 1);
		}

		int32 DotIndex = INDEX_NONE;
		if (Token.FindChar(TEXT('.'), DotIndex))
		{
			Token.LeftInline(DotIndex);
		}
		return Token.IsEmpty() ? AssetPath : Token;
	}

	TSharedPtr<FJsonObject> MakeAudioSearchParams(const FString& AssetPath, const FString& AssetKind)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetStringField(TEXT("query"), AssetSearchTokenFromPath(AssetPath));
		Params->SetNumberField(TEXT("limit"), 10);
		if (!AssetKind.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
		{
			Params->SetStringField(TEXT("type"), AssetKind);
		}
		return Params;
	}

	void CollectAssetPathsFromValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutAssets);

	void CollectAssetPathsFromObject(const TSharedPtr<FJsonObject>& Obj, TArray<FString>& OutAssets)
	{
		if (!Obj.IsValid())
		{
			return;
		}

		for (const auto& Pair : Obj->Values)
		{
			CollectAssetPathsFromValue(Pair.Value, OutAssets);
		}
	}

	void CollectAssetPathsFromValue(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutAssets)
	{
		if (!Value.IsValid())
		{
			return;
		}

		if (Value->Type == EJson::String)
		{
			FString StringValue;
			if (Value->TryGetString(StringValue))
			{
				AddUniqueAssetPath(OutAssets, StringValue);
			}
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (Value->TryGetObject(Obj) && Obj)
			{
				CollectAssetPathsFromObject(*Obj, OutAssets);
			}
		}
		else if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Value->TryGetArray(Array) && Array)
			{
				for (const TSharedPtr<FJsonValue>& Item : *Array)
				{
					CollectAssetPathsFromValue(Item, OutAssets);
				}
			}
		}
	}

	TSharedPtr<FJsonObject> MakeRuntimeProofParams(const TSharedPtr<FJsonObject>& Runtime)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		if (!Runtime.IsValid())
		{
			return Params;
		}

		FString Actor;
		Runtime->TryGetStringField(TEXT("actor"), Actor);
		if (!Actor.IsEmpty())
		{
			Params->SetStringField(TEXT("actor"), Actor);
		}

		FString EventTag;
		Runtime->TryGetStringField(TEXT("event_tag"), EventTag);
		if (!EventTag.IsEmpty())
		{
			Params->SetStringField(TEXT("event_tag"), EventTag);
		}

		FString CueTag;
		Runtime->TryGetStringField(TEXT("cue_tag"), CueTag);
		if (!CueTag.IsEmpty())
		{
			Params->SetStringField(TEXT("cue_tag"), CueTag);
		}

		TSharedPtr<FJsonObject> Trigger = MakeShared<FJsonObject>();
		Trigger->SetStringField(TEXT("namespace"), TEXT("editor"));
		Trigger->SetStringField(TEXT("action"), TEXT("pie_inject_input_action"));
		TSharedPtr<FJsonObject> TriggerParams = MakeShared<FJsonObject>();
		FString InputAction;
		Runtime->TryGetStringField(TEXT("input_action"), InputAction);
		if (!InputAction.IsEmpty())
		{
			TriggerParams->SetStringField(TEXT("input_action"), InputAction);
		}
		bool bValue = true;
		Runtime->TryGetBoolField(TEXT("value"), bValue);
		TriggerParams->SetBoolField(TEXT("value"), bValue);
		TriggerParams->SetNumberField(TEXT("player_index"), 0);
		TriggerParams->SetNumberField(TEXT("repeat_frames"), 1);
		Trigger->SetObjectField(TEXT("params"), TriggerParams);
		Params->SetObjectField(TEXT("trigger_action"), Trigger);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeStaticMeshPlanObject(
		const FString& MeshAssetPath,
		const FString& MaterialAssetPath,
		bool bDryRun,
		bool bRunValidation,
		bool bIncludeMaterialDiagnostics)
	{
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(5);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("plan_asset"),
			TEXT("workflow.game_ready_asset_static_mesh"),
			TEXT("planned"),
			TEXT("Normalize the requested StaticMesh/material workflow and declare proof gates."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("validate_game_ready"),
			TEXT("mesh.validate_game_ready"),
			bDryRun || !bRunValidation ? TEXT("planned") : TEXT("ready"),
			TEXT("Run the StaticMesh game-readiness checklist."))));
		if (!MaterialAssetPath.IsEmpty())
		{
			Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
				TEXT("material_diagnostics"),
				TEXT("material.validate_material + material.get_compilation_stats"),
				bDryRun || !bIncludeMaterialDiagnostics ? TEXT("planned") : TEXT("ready"),
				TEXT("Collect material validation and compile/budget diagnostics."))));
		}
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("preview_artifact"),
			TEXT("material.render_preview"),
			TEXT("blocked"),
			TEXT("Preview capture is intentionally not performed by this read-only first slice."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(
			TEXT("save_and_report"),
			TEXT("asset.save_asset + source_control_prepare"),
			TEXT("blocked"),
			TEXT("Saving and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("mesh_asset_path must identify a UStaticMesh asset."),
				TEXT("material_asset_path, when supplied, must identify a material or material instance asset."),
				TEXT("Mutation remains out of scope for this first read-only workflow slice.")
			},
			{
				TEXT("mesh.validate_game_ready"),
				TEXT("material.validate_material"),
				TEXT("material.get_compilation_stats"),
				TEXT("material.render_preview"),
				TEXT("asset.save_asset"),
				TEXT("source_control provider")
			});
	}

	TSharedPtr<FJsonObject> MakeGameplayPlanObject(bool bDryRun, bool bRuntimeProofRequired)
	{
		const FString ValidationStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(9);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("plan_feature"), TEXT("workflow.gameplay_feature_manifest"), TEXT("planned"), TEXT("Normalize the cross-domain gameplay feature manifest."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("validate_feature_manifest"), TEXT("workflow.gameplay_feature_manifest"), TEXT("planned"), TEXT("Check manifest sections and asset references before any authoring action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("input_preflight"), TEXT("input.get_input_action + input.get_input_mapping_context + input.validate_input_mappings"), ValidationStatus, TEXT("Inspect Enhanced Input assets and mapping conflicts."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("gas_preflight"), TEXT("gas.validate_gas_setup + gas.validate_ability_blueprint + gas.validate_effect"), ValidationStatus, TEXT("Inspect GAS setup, abilities, effects, cues, and input bindings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("blueprint_preflight"), TEXT("blueprint.get_blueprint_info + blueprint.get_components + blueprint.validate_blueprint"), ValidationStatus, TEXT("Inspect actor/controller/component Blueprints that host the feature."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("ai_readiness"), TEXT("ai.validate_behavior_tree + ai.validate_state_tree + ai.validate_ai_controller"), ValidationStatus, TEXT("Inspect AI assets that can drive or react to the feature."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("gamefeatures_gate"), TEXT("gamefeatures.get_status + gamefeatures.validate_plugin"), ValidationStatus, TEXT("Inspect optional GameFeature plugin readiness without activation."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("world_conditions_gate"), TEXT("world_conditions.get_status + world_conditions.describe_query"), ValidationStatus, TEXT("Inspect optional WorldConditions/SmartObject gates without mutation."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("runtime_proof_declared"), TEXT("gas.expect_event_cue + editor.pie_inject_input_action"), bRuntimeProofRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Declare the PIE runtime proof chain; this first slice does not start PIE."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("feature_id must identify the gameplay feature being preflighted."),
				TEXT("manifest must group input, GAS, Blueprint, AI, GameFeatures, WorldConditions, and runtime proof requests."),
				TEXT("dry_run=true is read-only and must not dirty packages."),
				TEXT("runtime proof requires a later confirmed PIE workflow slice.")
			},
			{
				TEXT("input"),
				TEXT("gas"),
				TEXT("blueprint"),
				TEXT("ai"),
				TEXT("gamefeatures"),
				TEXT("world_conditions"),
				TEXT("editor PIE runtime actions")
			});
	}

	TSharedPtr<FJsonObject> MakeUiPlanObject(bool bDryRun)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(8);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_tree"), TEXT("ui.get_widget_tree"), ReadStatus, TEXT("Read the Widget Blueprint tree."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("ui_spec"), TEXT("ui.dump_ui_spec"), ReadStatus, TEXT("Dump the UI spec for roundtrip/read-back proof."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("binding_inventory"), TEXT("ui.get_widget_bindings"), ReadStatus, TEXT("Inventory property/data bindings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("layout_accessibility"), TEXT("ui.audit_widget_layout + ui.audit_accessibility"), ReadStatus, TEXT("Audit layout and accessibility readiness."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("navigation_focus_commonui"), TEXT("ui.dump_widget_navigation + ui.audit_focus_chain + ui.audit_commonui_widget"), ReadStatus, TEXT("Audit navigation, focus, and optional CommonUI readiness."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("compile_blocker"), TEXT("ui.dump_blueprint_compile_log"), TEXT("blocked"), TEXT("Fresh compile/read-back is declared as an explicit next action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("preview_blocker"), TEXT("editor.capture_scene_preview"), TEXT("blocked"), TEXT("Preview capture is declared as an explicit next action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_blocker"), TEXT("asset.save_asset + source_control.checkout_or_add"), TEXT("blocked"), TEXT("Save and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("widget_asset_path must identify a Widget Blueprint asset."),
				TEXT("The workflow must preserve the UI/ViewModel boundary and report binding/audit gaps."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("ui.get_widget_tree"),
				TEXT("ui.dump_ui_spec"),
				TEXT("ui.get_widget_bindings"),
				TEXT("ui.audit_widget_layout"),
				TEXT("ui.audit_accessibility"),
				TEXT("ui.dump_widget_navigation"),
				TEXT("ui.audit_focus_chain"),
				TEXT("ui.audit_commonui_widget"),
				TEXT("editor.capture_scene_preview"),
				TEXT("asset.save_asset"),
				TEXT("source_control.checkout_or_add")
			});
	}

	TSharedPtr<FJsonObject> MakeLevelPlanObject(bool bDryRun, bool bConfirm, bool bSave, bool bPrepareSourceControl)
	{
		const FString MutatingStatus = bDryRun ? TEXT("planned") : (bConfirm ? TEXT("ready") : TEXT("blocked"));
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(11);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("dirty_preflight"), TEXT("editor.list_dirty_packages"), ReadStatus, TEXT("Report dirty packages before touching a map."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("create_and_load_map"), TEXT("editor.create_empty_map + editor.load_level"), MutatingStatus, TEXT("Create a blank map and load it only when confirm=true."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("world_context"), TEXT("scene.get_world_context"), ReadStatus, TEXT("Read active editor world context."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("blockout_volume"), TEXT("scene.spawn_volume + worldgen.setup_blockout_volume"), MutatingStatus, TEXT("Create and tag one blockout volume."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("blockout_primitives"), TEXT("worldgen.create_blockout_primitives_batch"), MutatingStatus, TEXT("Create deterministic blockout primitives when supplied."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("scatter_and_settle"), TEXT("worldgen.scatter_props + worldgen.settle_props"), MutatingStatus, TEXT("Optionally scatter and settle props with the supplied non-zero seed."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("read_back"), TEXT("worldgen.get_blockout_volume_info + worldgen.export_blockout_layout + scene.get_scene_statistics + scene.get_level_actors"), ReadStatus, TEXT("Read back the generated blockout and scene statistics."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("leveldesign_analysis"), TEXT("leveldesign.analyze_sightlines + leveldesign.analyze_room_acoustics"), ReadStatus, TEXT("Run optional level-design analysis passes."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("collection_report"), TEXT("collection.create_collection + collection.add_assets + collection.list_assets"), MutatingStatus, TEXT("Optionally add the map to a Content Browser collection."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_report"), TEXT("editor.save_packages"), bSave ? MutatingStatus : TEXT("planned"), TEXT("Save is explicit and scoped to the requested map package."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("source_control_report"), TEXT("source_control.get_capabilities + source_control.get_status + source_control.checkout_or_add"), bPrepareSourceControl ? MutatingStatus : TEXT("planned"), TEXT("Source-control preparation is explicit and path-scoped."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("map_path must be a new /Game map package path."),
				TEXT("volume must include name, location, extent, and room_type."),
				TEXT("seed must be non-zero so scatter/settle proof is deterministic."),
				TEXT("dry_run=false requires confirm=true before any map or scene mutation."),
				TEXT("primitive count is capped at 200 before mutation.")
			},
			{
				TEXT("editor"),
				TEXT("scene"),
				TEXT("worldgen"),
				TEXT("leveldesign"),
				TEXT("collection"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeShotRenderPlanObject(bool bDryRun, bool bRenderRequired)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(6);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("sequence_bindings"), TEXT("level_sequence.list_bindings"), ReadStatus, TEXT("Read Level Sequence bindings and bound classes."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("director_events"), TEXT("level_sequence.get_director_info + level_sequence.list_event_bindings"), ReadStatus, TEXT("Inspect Director Blueprint and event-track bindings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("anim_mixer_optional"), TEXT("level_sequence.get_anim_mixer_status + level_sequence.list_anim_mixer_tracks"), ReadStatus, TEXT("Inspect optional Sequencer Anim Mixer state without hard-linking the plugin."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("mrq_queue_readiness"), TEXT("movie_render.get_queue + movie_render.is_rendering + movie_render.list_settings"), ReadStatus, TEXT("Read Movie Render Queue state and available settings."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("mrq_job_plan"), TEXT("movie_render.load_queue + movie_render.add_job"), TEXT("planned"), TEXT("Declare the queue/job setup without mutating the editor queue in this first slice."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("render_blocker"), TEXT("movie_render.render_queue"), bRenderRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Rendering requires an explicit confirmed follow-up action."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("sequence_asset_path must identify a Level Sequence asset."),
				TEXT("render_required=true is reported as blocked; this first slice never starts MRQ rendering."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("level_sequence"),
				TEXT("movie_render"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeAudioShippingPlanObject(bool bDryRun, const FString& AssetKind)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(7);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("asset_discovery"), TEXT("audio.search_audio_assets"), ReadStatus, TEXT("Find the requested audio asset and report candidate type context."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("metasound_validation"), TEXT("audio.get_metasound_info + audio.validate_metasound"), AssetKind.Equals(TEXT("MetaSoundSource"), ESearchCase::IgnoreCase) ? ReadStatus : TEXT("planned"), TEXT("Inspect MetaSound graph and validation status when the target is a MetaSound."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("sound_cue_validation"), TEXT("audio.get_sound_cue_graph + audio.validate_sound_cue"), AssetKind.Equals(TEXT("SoundCue"), ESearchCase::IgnoreCase) ? ReadStatus : TEXT("planned"), TEXT("Inspect SoundCue graph, duration, and validation status when the target is a SoundCue."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("sound_wave_budget"), TEXT("audio.get_sound_wave_info"), AssetKind.Equals(TEXT("SoundWave"), ESearchCase::IgnoreCase) ? ReadStatus : TEXT("planned"), TEXT("Inspect SoundWave duration/compression context when the target is a SoundWave."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("perception_binding"), TEXT("audio.get_sound_perception_binding"), ReadStatus, TEXT("Read optional sound perception binding without changing user data."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("preview_blocker"), TEXT("audio.preview_sound"), TEXT("blocked"), TEXT("Audible preview remains an explicit user-triggered action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_blocker"), TEXT("asset.save_asset + source_control.checkout_or_add"), TEXT("blocked"), TEXT("Save and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("audio_asset_path must identify a SoundWave, SoundCue, MetaSoundSource, or other SoundBase-derived asset."),
				TEXT("asset_kind=auto does not guess type-specific validators during execution; pass SoundWave, SoundCue, or MetaSoundSource for targeted proof."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("audio"),
				TEXT("asset"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeLocalizationShippingPlanObject(bool bDryRun, bool bExportRequested)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(5);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("culture_inventory"), TEXT("localization.list_cultures"), ReadStatus, TEXT("Read available Unreal cultures."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("string_table_readback"), TEXT("localization.get_string_table"), ReadStatus, TEXT("Read capped StringTable entries for proof."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("string_table_validation"), TEXT("localization.validate_string_table"), ReadStatus, TEXT("Validate empty keys, empty strings, duplicate-looking keys, and large-output risks."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("csv_export_plan"), TEXT("localization.export_string_table_csv"), bExportRequested ? TEXT("blocked") : TEXT("planned"), TEXT("CSV export writes a file and remains an explicit follow-up action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("save_blocker"), TEXT("asset.save_asset + source_control.checkout_or_add"), TEXT("blocked"), TEXT("Save and source-control preparation remain explicit follow-up actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("string_table_path must identify a StringTable asset under /Game."),
				TEXT("CSV import/export and entry mutation require explicit localization actions with dry_run or confirm."),
				TEXT("dry_run=true is read-only and must not dirty packages.")
			},
			{
				TEXT("localization"),
				TEXT("asset"),
				TEXT("source_control")
			});
	}

	TSharedPtr<FJsonObject> MakeSlateEuwPlanObject(bool bDryRun, bool bInteractionRequired, bool bCaptureRequired)
	{
		const FString ReadStatus = bDryRun ? TEXT("planned") : TEXT("ready");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(7);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("inspector_status"), TEXT("slate.get_inspector_status"), ReadStatus, TEXT("Read Slate inspector capability and test-mode readiness."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("window_inventory"), TEXT("slate.list_windows"), ReadStatus, TEXT("List visible top-level Slate windows for target disambiguation."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_snapshot"), TEXT("slate.snapshot_widgets"), ReadStatus, TEXT("Capture a structured widget tree snapshot for the requested target."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_description"), TEXT("slate.describe_widget"), ReadStatus, TEXT("Read target widget geometry, text, state, and focus data when available."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("wait_for_widget"), TEXT("slate.wait_for_widget"), ReadStatus, TEXT("Plan or run a bounded wait for the target widget."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("capture_widget"), TEXT("slate.capture_widget"), bCaptureRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Widget capture writes an artifact and remains an explicit follow-up action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("interaction_blocker"), TEXT("slate.click/type/key"), bInteractionRequired ? TEXT("blocked") : TEXT("planned"), TEXT("Click/type/key simulation is not exposed in this Monolith slice; the workflow returns an explicit blocker instead of pretending proof exists."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("target must identify a Slate widget, window, text, path, or Editor Utility Widget surface to inspect."),
				TEXT("dry_run=true is read-only and must not send input or write capture artifacts."),
				TEXT("production input simulation is unavailable unless a future test-mode gated Slate action exposes it explicitly.")
			},
			{
				TEXT("slate.get_inspector_status"),
				TEXT("slate.list_windows"),
				TEXT("slate.snapshot_widgets"),
				TEXT("slate.describe_widget"),
				TEXT("slate.wait_for_widget"),
				TEXT("slate.capture_widget")
			});
	}

	TArray<TSharedPtr<FJsonValue>> MakePositionArray(int32 X, int32 Y)
	{
		TArray<TSharedPtr<FJsonValue>> Values;
		Values.Add(MakeShared<FJsonValueNumber>(X));
		Values.Add(MakeShared<FJsonValueNumber>(Y));
		return Values;
	}

	FString NormalizeUiWidgetDelegateName(const FString& EventName)
	{
		const FString Trimmed = EventName.TrimStartAndEnd();
		if (Trimmed.StartsWith(TEXT("On")))
		{
			return Trimmed;
		}

		if (Trimmed.Equals(TEXT("Clicked"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("Click"), ESearchCase::IgnoreCase))
		{
			return TEXT("OnClicked");
		}
		if (Trimmed.Equals(TEXT("Pressed"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("Press"), ESearchCase::IgnoreCase))
		{
			return TEXT("OnPressed");
		}
		if (Trimmed.Equals(TEXT("Released"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("Release"), ESearchCase::IgnoreCase))
		{
			return TEXT("OnReleased");
		}
		if (Trimmed.Equals(TEXT("Hovered"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("Hover"), ESearchCase::IgnoreCase))
		{
			return TEXT("OnHovered");
		}
		if (Trimmed.Equals(TEXT("Unhovered"), ESearchCase::IgnoreCase)
			|| Trimmed.Equals(TEXT("Unhover"), ESearchCase::IgnoreCase))
		{
			return TEXT("OnUnhovered");
		}
		return TEXT("On") + Trimmed;
	}

	TSharedPtr<FJsonObject> MakeUiBindEventPlanObject(bool bDryRun, bool bConfirm, bool bCompile)
	{
		const FString ApplyStatus = (!bDryRun && bConfirm) ? TEXT("ready") : TEXT("planned");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(5);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("boundary_policy"), TEXT("workflow.ui_bind_widget_event"), TEXT("ready"), TEXT("Reject direct gameplay Actor/Pawn/Controller calls and require a ViewModel command intent for runtime UI."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_event_resolve"), TEXT("blueprint.resolve_node"), bDryRun ? TEXT("planned") : TEXT("ready"), TEXT("Resolve the Widget Blueprint component-bound delegate node without mutating first."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("viewmodel_resolve"), TEXT("blueprint.resolve_node"), bDryRun ? TEXT("planned") : TEXT("ready"), TEXT("Resolve ViewModel variable getter and command call node pins before wiring."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("event_binding_apply"), TEXT("blueprint.add_node + blueprint.connect_pins"), ApplyStatus, TEXT("Create the component-bound event node, ViewModel getter, command call node, then wire exec and target pins."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("compile_readback"), TEXT("blueprint.compile_blueprint + blueprint.get_graph_summary"), bCompile ? ApplyStatus : TEXT("not_requested"), TEXT("Compile and read back the edited graph when requested."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("asset_path must identify a Widget Blueprint asset."),
				TEXT("widget_name must be a named widget variable exposing the requested BlueprintAssignable delegate."),
				TEXT("intent.kind must be viewmodel_command; direct gameplay access is rejected for runtime UI."),
				TEXT("dry_run=false requires confirm=true before Blueprint graph mutation.")
			},
			{
				TEXT("blueprint.resolve_node"),
				TEXT("blueprint.add_node"),
				TEXT("blueprint.connect_pins"),
				TEXT("blueprint.compile_blueprint"),
				TEXT("blueprint.get_graph_summary"),
				TEXT("ui.dump_blueprint_compile_log")
			});
	}

	TSharedPtr<FJsonObject> MakeUiBindEventNodeParams(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& WidgetName,
		const FString& DelegateName)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetStringField(TEXT("node_type"), TEXT("ComponentBoundEvent"));
		Params->SetStringField(TEXT("component_name"), WidgetName);
		Params->SetStringField(TEXT("delegate_property_name"), DelegateName);
		Params->SetArrayField(TEXT("position"), MakePositionArray(0, 0));
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiBindVariableGetParams(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& ViewModelVariable)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetStringField(TEXT("node_type"), TEXT("VariableGet"));
		Params->SetStringField(TEXT("variable_name"), ViewModelVariable);
		Params->SetArrayField(TEXT("position"), MakePositionArray(320, 120));
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiBindCommandCallParams(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& Command,
		const FString& ViewModelClass)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetStringField(TEXT("node_type"), TEXT("CallFunction"));
		Params->SetStringField(TEXT("function_name"), Command);
		if (!ViewModelClass.IsEmpty())
		{
			Params->SetStringField(TEXT("target_class"), ViewModelClass);
		}
		Params->SetArrayField(TEXT("position"), MakePositionArray(560, 0));
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiBindResolveNodeParams(const TSharedPtr<FJsonObject>& NodeParams)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		CopyJsonField(NodeParams, TEXT("asset_path"), Params);
		CopyJsonField(NodeParams, TEXT("node_type"), Params);
		CopyJsonField(NodeParams, TEXT("function_name"), Params);
		CopyJsonField(NodeParams, TEXT("target_class"), Params);
		CopyJsonField(NodeParams, TEXT("variable_name"), Params);
		CopyJsonField(NodeParams, TEXT("component_name"), Params);
		CopyJsonField(NodeParams, TEXT("delegate_property_name"), Params);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiBindConnectionParams(
		const FString& AssetPath,
		const FString& GraphName,
		const FString& SourceNode,
		const FString& SourcePin,
		const FString& TargetNode,
		const FString& TargetPin)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), AssetPath);
		Params->SetStringField(TEXT("graph_name"), GraphName);
		Params->SetStringField(TEXT("source_node"), SourceNode);
		Params->SetStringField(TEXT("source_pin"), SourcePin);
		Params->SetStringField(TEXT("target_node"), TargetNode);
		Params->SetStringField(TEXT("target_pin"), TargetPin);
		return Params;
	}

	FMonolithActionResult ExecutePrimitiveAndRecord(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<TSharedPtr<FJsonValue>>& OutProofRows,
		TArray<FString>& OutErrors)
	{
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		const FString ActionId = Namespace + TEXT(".") + Action;
		const bool bAvailable = Registry.HasAction(Namespace, Action);
		if (!bAvailable)
		{
			TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, TEXT("unavailable"), false, false, Params);
			Row->SetStringField(TEXT("reason"), ActionId + TEXT(" is not registered in the current Monolith profile."));
			OutActions.Add(MakeShared<FJsonValueObject>(Row));
			OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
			OutErrors.Add(ActionId + TEXT(" unavailable"));
			return FMonolithActionResult::Error(ActionId + TEXT(" unavailable"));
		}

		const FMonolithActionResult Result = Registry.ExecuteAction(Namespace, Action, Params);
		TSharedPtr<FJsonObject> Row = MakeActionRow(ActionId, Result.bSuccess ? TEXT("succeeded") : TEXT("failed"), true, true, Params);
		if (Result.bSuccess && Result.Result.IsValid())
		{
			Row->SetObjectField(TEXT("result"), Result.Result);
		}
		else if (!Result.bSuccess)
		{
			Row->SetStringField(TEXT("error"), Result.ErrorMessage);
			Row->SetNumberField(TEXT("error_code"), Result.ErrorCode);
			if (Result.ErrorData.IsValid())
			{
				Row->SetObjectField(TEXT("error_data"), Result.ErrorData);
			}
			OutErrors.Add(ActionId + TEXT(": ") + Result.ErrorMessage);
		}
		OutActions.Add(MakeShared<FJsonValueObject>(Row));
		OutProofRows.Add(MakeShared<FJsonValueObject>(Row));
		return Result;
	}

	bool TryGetWorkflowNodeId(const TSharedPtr<FJsonObject>& Result, FString& OutNodeId)
	{
		if (!Result.IsValid())
		{
			return false;
		}
		return Result->TryGetStringField(TEXT("node_id"), OutNodeId)
			|| Result->TryGetStringField(TEXT("id"), OutNodeId);
	}

	bool PinMatches(const TSharedPtr<FJsonObject>& Pin, const FString& Direction, bool bExec)
	{
		if (!Pin.IsValid())
		{
			return false;
		}

		FString PinDirection;
		FString PinType;
		if (!Pin->TryGetStringField(TEXT("direction"), PinDirection)
			|| !Pin->TryGetStringField(TEXT("type"), PinType)
			|| !PinDirection.Equals(Direction, ESearchCase::IgnoreCase))
		{
			return false;
		}
		return bExec ? PinType.Equals(TEXT("exec"), ESearchCase::IgnoreCase) : !PinType.Equals(TEXT("exec"), ESearchCase::IgnoreCase);
	}

	bool FindPinNameInNode(
		const TSharedPtr<FJsonObject>& Node,
		const FString& Direction,
		bool bExec,
		const TArray<FString>& PreferredNames,
		FString& OutPinName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Pins = nullptr;
		if (!Node.IsValid() || !Node->TryGetArrayField(TEXT("pins"), Pins) || !Pins)
		{
			return false;
		}

		for (const FString& PreferredName : PreferredNames)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Pins)
			{
				const TSharedPtr<FJsonObject> Pin = Value.IsValid() ? Value->AsObject() : nullptr;
				FString PinName;
				if (PinMatches(Pin, Direction, bExec)
					&& Pin->TryGetStringField(TEXT("name"), PinName)
					&& PinName.Equals(PreferredName, ESearchCase::IgnoreCase))
				{
					OutPinName = PinName;
					return true;
				}
			}
		}

		for (const TSharedPtr<FJsonValue>& Value : *Pins)
		{
			const TSharedPtr<FJsonObject> Pin = Value.IsValid() ? Value->AsObject() : nullptr;
			if (PinMatches(Pin, Direction, bExec) && Pin->TryGetStringField(TEXT("name"), OutPinName))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> MakeUiBindBoundaryProof(const FString& Status, const FString& Reason)
	{
		TSharedPtr<FJsonObject> Boundary = MakeShared<FJsonObject>();
		Boundary->SetStringField(TEXT("schema_version"), TEXT("ui_event_binding_boundary.v1"));
		Boundary->SetStringField(TEXT("status"), Status);
		Boundary->SetStringField(TEXT("policy"), TEXT("runtime_ui_must_route_intent_through_viewmodel"));
		Boundary->SetStringField(TEXT("reason"), Reason);
		return Boundary;
	}

	bool IsAsciiIdentifierStart(TCHAR C)
	{
		return C == TCHAR('_')
			|| (C >= TCHAR('A') && C <= TCHAR('Z'))
			|| (C >= TCHAR('a') && C <= TCHAR('z'));
	}

	bool IsAsciiIdentifierChar(TCHAR C)
	{
		return IsAsciiIdentifierStart(C) || (C >= TCHAR('0') && C <= TCHAR('9'));
	}

	bool IsValidHlslIdentifier(const FString& Name)
	{
		if (Name.IsEmpty() || !IsAsciiIdentifierStart(Name[0]))
		{
			return false;
		}
		for (int32 Index = 1; Index < Name.Len(); ++Index)
		{
			if (!IsAsciiIdentifierChar(Name[Index]))
			{
				return false;
			}
		}
		return true;
	}

	TSharedPtr<FJsonObject> AnalyzeUiHlsl(
		const FString& Hlsl,
		const TArray<TSharedPtr<FJsonValue>>& Parameters)
	{
		TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
		Proof->SetStringField(TEXT("schema_version"), TEXT("ui_hlsl_lint.v1"));
		Proof->SetStringField(TEXT("output_type"), TEXT("Float4"));
		Proof->SetNumberField(TEXT("code_length"), Hlsl.Len());
		Proof->SetBoolField(TEXT("assumes_float4_return"), true);

		TArray<TSharedPtr<FJsonValue>> Findings;
		auto AddFinding = [&Findings](const FString& Severity, const FString& RuleId, const FString& Message)
		{
			TSharedPtr<FJsonObject> Finding = MakeShared<FJsonObject>();
			Finding->SetStringField(TEXT("severity"), Severity);
			Finding->SetStringField(TEXT("rule_id"), RuleId);
			Finding->SetStringField(TEXT("message"), Message);
			Findings.Add(MakeShared<FJsonValueObject>(Finding));
		};

		const TArray<FString> RiskyTokens = {
			TEXT("clip"),
			TEXT("discard"),
			TEXT("ddx"),
			TEXT("ddy"),
			TEXT("fwidth"),
			TEXT("SceneTexture")
		};
		for (const FString& Token : RiskyTokens)
		{
			if (Hlsl.Contains(Token, ESearchCase::IgnoreCase))
			{
				AddFinding(TEXT("warning"), TEXT("RiskyHlslToken"), FString::Printf(TEXT("HLSL contains token '%s'; verify UI-domain shader behavior and platform support."), *Token));
			}
		}
		if (Hlsl.Contains(TEXT("tex2D"), ESearchCase::IgnoreCase)
			|| Hlsl.Contains(TEXT("Texture2DSample"), ESearchCase::IgnoreCase)
			|| Hlsl.Contains(TEXT(".Sample"), ESearchCase::IgnoreCase))
		{
			AddFinding(TEXT("warning"), TEXT("TextureSamplingNeedsMetadata"), TEXT("Texture sampling detected; confirm sampler inputs, texture parameter ownership, and UI material sampler budget."));
		}

		TArray<TSharedPtr<FJsonValue>> ParameterProof;
		for (const TSharedPtr<FJsonValue>& Value : Parameters)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				AddFinding(TEXT("error"), TEXT("InvalidParameterDescriptor"), TEXT("Each parameter entry must be an object."));
				continue;
			}

			FString Name;
			Obj->TryGetStringField(TEXT("name"), Name);
			FString Type;
			Obj->TryGetStringField(TEXT("type"), Type);
			if (Type.IsEmpty())
			{
				Obj->TryGetStringField(TEXT("kind"), Type);
			}

			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("name"), Name);
			Row->SetStringField(TEXT("type"), Type.IsEmpty() ? TEXT("unknown") : Type);
			Row->SetBoolField(TEXT("valid_hlsl_identifier"), IsValidHlslIdentifier(Name));
			ParameterProof.Add(MakeShared<FJsonValueObject>(Row));

			if (!IsValidHlslIdentifier(Name))
			{
				AddFinding(TEXT("error"), TEXT("InvalidHlslIdentifier"), FString::Printf(TEXT("Parameter name '%s' is not a valid HLSL identifier."), *Name));
			}
		}

		Proof->SetArrayField(TEXT("parameters"), ParameterProof);
		Proof->SetArrayField(TEXT("findings"), Findings);
		Proof->SetStringField(TEXT("status"), Findings.Num() > 0 ? TEXT("warning") : TEXT("pass"));
		return Proof;
	}

	TArray<TSharedPtr<FJsonValue>> MakeCustomNodeInputsFromParameters(const TArray<TSharedPtr<FJsonValue>>& Parameters)
	{
		TArray<TSharedPtr<FJsonValue>> Inputs;
		for (const TSharedPtr<FJsonValue>& Value : Parameters)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (Obj.IsValid() && Obj->TryGetStringField(TEXT("name"), Name) && IsValidHlslIdentifier(Name))
			{
				TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
				Input->SetStringField(TEXT("name"), Name);
				FString Type;
				if (Obj->TryGetStringField(TEXT("type"), Type) || Obj->TryGetStringField(TEXT("kind"), Type))
				{
					Input->SetStringField(TEXT("type"), Type);
				}
				Inputs.Add(MakeShared<FJsonValueObject>(Input));
			}
		}
		return Inputs;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialPlanObject(
		bool bDryRun,
		bool bConfirm,
		bool bCreateMaterial,
		bool bCompile,
		bool bBindWidget,
		bool bRunWidgetProof,
		bool bAutoComponentMaskAlpha)
	{
		const FString ApplyStatus = (!bDryRun && bConfirm) ? TEXT("ready") : TEXT("planned");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(10);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("hlsl_lint"), TEXT("workflow.ui_material_hlsl_effect"), TEXT("ready"), TEXT("Lint the UI HLSL contract and parameter identifiers before touching assets."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_create"), TEXT("material.create_material"), bCreateMaterial ? ApplyStatus : TEXT("not_requested"), TEXT("Optionally create the base material through the material owner action with material_domain=UI."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("ui_domain_enforce"), TEXT("material.set_material_property"), ApplyStatus, TEXT("Force UI material domain and UI-safe blend/shading defaults through the material owner action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("custom_hlsl_node"), TEXT("material.create_custom_hlsl_node + material.update_custom_hlsl_node"), ApplyStatus, TEXT("Create or update the Custom HLSL node using existing material graph primitives."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_output_wiring"), TEXT("material.connect_expressions"), ApplyStatus, TEXT("Wire the Custom node to material output properties through material.connect_expressions."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("opacity_component_mask"), TEXT("material.build_material_graph"), bAutoComponentMaskAlpha ? ApplyStatus : TEXT("not_requested"), TEXT("Optionally insert a ComponentMask node through material.build_material_graph(clear_existing=false) so a float4 Custom output alpha channel can drive Opacity without a duplicate UI/HLSL action."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_compile_stats"), TEXT("material.recompile_material + material.validate_material + material.get_compilation_stats + material.get_material_properties + material.get_full_connection_graph"), bCompile ? ApplyStatus : TEXT("not_requested"), TEXT("Compile/read material stats and confirm domain/properties/connections through material read actions."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_binding"), TEXT("ui.set_image/ui.set_brush + ui.compile_widget + ui.dump_blueprint_compile_log"), bBindWidget ? ApplyStatus : TEXT("not_requested"), TEXT("Bind the UI material to an Image or brush property through existing UI owner actions."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_lifecycle_audit"), TEXT("ui.audit_widget_material_lifecycle"), bBindWidget ? ApplyStatus : TEXT("not_requested"), TEXT("Audit the target Widget Blueprint graph for repeated-lifecycle dynamic material creation sites after binding."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("visual_proof"), TEXT("workflow.ui_shipping_widget_blueprint"), bRunWidgetProof ? ApplyStatus : TEXT("planned"), TEXT("Optional shipping proof composes compile, capture, layout, visual artifact verification, and follow-up save/source-control actions."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("material_path must identify the material to create or update."),
				TEXT("hlsl must be a Custom-node program that returns float4 for UI use."),
				TEXT("bind_to.asset_path and bind_to.widget_name are required when widget binding is requested."),
				TEXT("dry_run=false requires confirm=true before material or widget mutation."),
				TEXT("This workflow composes material/ui/workflow owner actions; it does not register external hlsl_* or material_* aliases.")
			},
			{
				TEXT("material.create_material"),
				TEXT("material.set_material_property"),
				TEXT("material.create_custom_hlsl_node"),
				TEXT("material.update_custom_hlsl_node"),
				TEXT("material.connect_expressions"),
				TEXT("material.build_material_graph"),
				TEXT("material.recompile_material"),
				TEXT("material.validate_material"),
				TEXT("material.get_compilation_stats"),
				TEXT("material.get_material_properties"),
				TEXT("material.get_full_connection_graph"),
				TEXT("ui.set_image"),
				TEXT("ui.set_brush"),
				TEXT("ui.audit_widget_material_lifecycle"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("workflow.ui_shipping_widget_blueprint")
			});
	}

	TSharedPtr<FJsonObject> MakeUiMaterialCreateParams(
		const FString& MaterialPath,
		const FString& BlendMode,
		const FString& ShadingModel)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), MaterialPath);
		Params->SetStringField(TEXT("material_domain"), TEXT("UI"));
		Params->SetStringField(TEXT("blend_mode"), BlendMode);
		Params->SetStringField(TEXT("shading_model"), ShadingModel);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialPropertyParams(
		const FString& MaterialPath,
		const FString& BlendMode,
		const FString& ShadingModel)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), MaterialPath);
		Params->SetStringField(TEXT("material_domain"), TEXT("UI"));
		Params->SetStringField(TEXT("blend_mode"), BlendMode);
		Params->SetStringField(TEXT("shading_model"), ShadingModel);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialCustomParams(
		const FString& MaterialPath,
		const FString& Hlsl,
		const FString& OutputType,
		const TArray<TSharedPtr<FJsonValue>>& Inputs,
		const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), MaterialPath);
		Params->SetStringField(TEXT("code"), Hlsl);
		Params->SetStringField(TEXT("description"), TEXT("Monolith UI HLSL effect"));
		Params->SetStringField(TEXT("output_type"), OutputType);
		Params->SetArrayField(TEXT("inputs"), Inputs);
		CopyJsonField(Source, TEXT("additional_outputs"), Params);
		CopyJsonField(Source, TEXT("pos_x"), Params);
		CopyJsonField(Source, TEXT("pos_y"), Params);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialCustomUpdateParams(
		const FString& MaterialPath,
		const FString& ExpressionName,
		const FString& Hlsl,
		const FString& OutputType,
		const TArray<TSharedPtr<FJsonValue>>& Inputs,
		const TSharedPtr<FJsonObject>& Source)
	{
		TSharedPtr<FJsonObject> Params = MakeUiMaterialCustomParams(MaterialPath, Hlsl, OutputType, Inputs, Source);
		Params->SetStringField(TEXT("expression_name"), ExpressionName);
		CopyJsonField(Source, TEXT("include_file_paths"), Params);
		CopyJsonField(Source, TEXT("additional_defines"), Params);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialConnectParams(
		const FString& MaterialPath,
		const FString& ExpressionName,
		const FString& FromOutput,
		const FString& ToProperty)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), MaterialPath);
		Params->SetStringField(TEXT("from_expression"), ExpressionName);
		if (!FromOutput.IsEmpty())
		{
			Params->SetStringField(TEXT("from_output"), FromOutput);
		}
		Params->SetStringField(TEXT("to_property"), ToProperty);
		return Params;
	}

	FString NormalizeUiMaterialMaskChannel(const FString& InChannel)
	{
		FString Channel = InChannel;
		Channel.TrimStartAndEndInline();
		Channel = Channel.ToUpper();
		return Channel.IsEmpty() ? TEXT("A") : Channel;
	}

	bool IsUiMaterialMaskChannelValid(const FString& Channel)
	{
		return Channel == TEXT("R") || Channel == TEXT("G") || Channel == TEXT("B") || Channel == TEXT("A");
	}

	bool JsonObjectArrayFieldHasName(
		const TSharedPtr<FJsonObject>& Source,
		const FString& FieldName,
		const FString& ExpectedName)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Source.IsValid() || ExpectedName.IsEmpty() || !Source->TryGetArrayField(FieldName, Values) || !Values)
		{
			return false;
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject> Obj = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (Obj.IsValid()
				&& Obj->TryGetStringField(TEXT("name"), Name)
				&& Name.Equals(ExpectedName, ESearchCase::IgnoreCase))
			{
				return true;
			}
		}
		return false;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialAlphaMaskGraphParams(
		const FString& MaterialPath,
		const FString& SourceExpressionName,
		const FString& SourceOutputPin,
		const FString& MaskNodeId,
		const FString& Channel)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), MaterialPath);
		Params->SetBoolField(TEXT("clear_existing"), false);

		TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
		Props->SetBoolField(TEXT("R"), Channel == TEXT("R"));
		Props->SetBoolField(TEXT("G"), Channel == TEXT("G"));
		Props->SetBoolField(TEXT("B"), Channel == TEXT("B"));
		Props->SetBoolField(TEXT("A"), Channel == TEXT("A"));
		Props->SetStringField(TEXT("Desc"), TEXT("Monolith UI alpha-to-opacity mask"));

		TSharedPtr<FJsonObject> MaskNode = MakeShared<FJsonObject>();
		MaskNode->SetStringField(TEXT("id"), MaskNodeId);
		MaskNode->SetStringField(TEXT("class"), TEXT("ComponentMask"));
		MaskNode->SetNumberField(TEXT("pos_x"), 420.0);
		MaskNode->SetNumberField(TEXT("pos_y"), 120.0);
		MaskNode->SetObjectField(TEXT("properties"), Props);

		TArray<TSharedPtr<FJsonValue>> Nodes;
		Nodes.Add(MakeShared<FJsonValueObject>(MaskNode));

		TSharedPtr<FJsonObject> SourceToMask = MakeShared<FJsonObject>();
		SourceToMask->SetStringField(TEXT("from"), SourceExpressionName);
		if (!SourceOutputPin.IsEmpty())
		{
			SourceToMask->SetStringField(TEXT("from_pin"), SourceOutputPin);
		}
		SourceToMask->SetStringField(TEXT("to"), MaskNodeId);
		SourceToMask->SetStringField(TEXT("to_pin"), TEXT("Input"));

		TSharedPtr<FJsonObject> MaskToOpacity = MakeShared<FJsonObject>();
		MaskToOpacity->SetStringField(TEXT("from"), MaskNodeId);
		MaskToOpacity->SetStringField(TEXT("to_property"), TEXT("Opacity"));

		TArray<TSharedPtr<FJsonValue>> Connections;
		Connections.Add(MakeShared<FJsonValueObject>(SourceToMask));
		Connections.Add(MakeShared<FJsonValueObject>(MaskToOpacity));

		TSharedPtr<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
		GraphSpec->SetArrayField(TEXT("nodes"), Nodes);
		GraphSpec->SetArrayField(TEXT("connections"), Connections);
		Params->SetObjectField(TEXT("graph_spec"), GraphSpec);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialOpacityWiringProof(
		bool bRequested,
		bool bAutoComponentMaskAlpha,
		const FString& Channel,
		const FString& SourceExpressionName,
		const FString& SourceOutputPin,
		const FString& DirectOutputPin,
		const FString& MaskNodeId,
		const FString& Status)
	{
		TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
		Proof->SetStringField(TEXT("schema_version"), TEXT("ui_material_opacity_wiring.v1"));
		Proof->SetBoolField(TEXT("requested"), bRequested);
		Proof->SetStringField(TEXT("status"), Status);
		if (!bRequested)
		{
			Proof->SetStringField(TEXT("mode"), TEXT("not_requested"));
			Proof->SetStringField(TEXT("reason"), TEXT("connect_opacity=false and auto_component_mask_alpha=false."));
			return Proof;
		}

		if (bAutoComponentMaskAlpha)
		{
			Proof->SetStringField(TEXT("mode"), TEXT("component_mask"));
			Proof->SetStringField(TEXT("owner_action"), TEXT("material.build_material_graph"));
			Proof->SetStringField(TEXT("source_expression"), SourceExpressionName);
			Proof->SetStringField(TEXT("source_output_pin"), SourceOutputPin);
			Proof->SetStringField(TEXT("component_mask_node_id"), MaskNodeId);
			Proof->SetStringField(TEXT("component"), Channel);
			Proof->SetStringField(TEXT("to_property"), TEXT("Opacity"));
			Proof->SetStringField(TEXT("reason"), TEXT("A ComponentMask node is composed through material.build_material_graph(clear_existing=false), then connected to the material Opacity output."));
			return Proof;
		}

		Proof->SetStringField(TEXT("mode"), TEXT("direct_custom_output"));
		Proof->SetStringField(TEXT("owner_action"), TEXT("material.connect_expressions"));
		Proof->SetStringField(TEXT("source_expression"), SourceExpressionName);
		Proof->SetStringField(TEXT("source_output_pin"), DirectOutputPin);
		Proof->SetStringField(TEXT("to_property"), TEXT("Opacity"));
		Proof->SetStringField(TEXT("reason"), TEXT("Opacity is wired from an explicit Custom-node output pin."));
		return Proof;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialBindParams(
		const FString& BindingAction,
		const FString& WidgetAssetPath,
		const FString& WidgetName,
		const FString& PropertyName,
		const FString& MaterialPath,
		bool bCompileWidget)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
		Params->SetStringField(TEXT("widget_name"), WidgetName);
		Params->SetStringField(TEXT("material_path"), MaterialPath);
		Params->SetBoolField(TEXT("compile"), bCompileWidget);
		if (BindingAction.Equals(TEXT("set_brush"), ESearchCase::IgnoreCase))
		{
			Params->SetStringField(TEXT("property_name"), PropertyName.IsEmpty() ? TEXT("Brush") : PropertyName);
			Params->SetStringField(TEXT("draw_type"), TEXT("Image"));
		}
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiMaterialLifecycleAuditParams(const FString& WidgetAssetPath)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
		Params->SetBoolField(TEXT("include_advisory"), true);
		Params->SetBoolField(TEXT("treat_warnings_as_errors"), false);
		return Params;
	}

	TSharedPtr<FJsonObject> MakeUiRetainerPlanObject(
		bool bDryRun,
		bool bConfirm,
		bool bCompile,
		bool bRunReadOnlyChecks,
		bool bRunWidgetProof,
		bool bRequestRender)
	{
		const FString ApplyStatus = (!bDryRun && bConfirm) ? TEXT("ready") : TEXT("planned");
		const FString ReadStatus = ((!bDryRun && bConfirm) || bRunReadOnlyChecks) ? TEXT("ready") : TEXT("planned");
		TArray<TSharedPtr<FJsonValue>> Steps;
		Steps.Reserve(7);
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_parameter_readback"), TEXT("material.get_material_parameters"), ReadStatus, TEXT("Read texture parameters from the effect material and require an exact Retainer texture-parameter match."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_domain_readback"), TEXT("material.get_material_properties"), ReadStatus, TEXT("Read material domain and blend/shading details without duplicating material logic in UI."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("retainer_binding"), TEXT("ui.set_retainer_effect_material"), ApplyStatus, TEXT("Bind the effect material and texture parameter through the RetainerBox owner API, not raw reflection."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("widget_compile_log"), TEXT("ui.compile_widget + ui.dump_blueprint_compile_log"), bCompile ? ApplyStatus : TEXT("not_requested"), TEXT("Compile/read the Widget Blueprint after Retainer binding."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("material_lifecycle_audit"), TEXT("ui.audit_widget_material_lifecycle"), ReadStatus, TEXT("Audit the Widget Blueprint graph for repeated-lifecycle dynamic material creation before treating the Retainer material workflow as production-ready."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("freshness_request"), TEXT("URetainerBox::RequestRender"), bRequestRender ? ApplyStatus : TEXT("not_requested"), TEXT("Optionally request a fresh Retainer render for visual proof; this is not a performance profile."))));
		Steps.Add(MakeShared<FJsonValueObject>(MakeWorkflowStep(TEXT("visual_proof"), TEXT("workflow.ui_shipping_widget_blueprint"), bRunWidgetProof ? ApplyStatus : TEXT("planned"), TEXT("Optional visual artifact proof after Retainer effect binding."))));

		return MakePlanObject(
			Steps,
			{
				TEXT("material_path must identify a UI-domain material or material instance."),
				TEXT("bind_to.asset_path and bind_to.retainer_widget_name/widget_name are required."),
				TEXT("texture_parameter defaults to Texture and must exist exactly on the effect material."),
				TEXT("dry_run=false requires confirm=true before Widget Blueprint mutation."),
				TEXT("Retainer/Invalidation performance recommendations require a separate measured before/after profile.")
			},
			{
				TEXT("material.get_material_parameters"),
				TEXT("material.get_material_properties"),
				TEXT("ui.set_retainer_effect_material"),
				TEXT("ui.compile_widget"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("ui.audit_widget_material_lifecycle"),
				TEXT("workflow.ui_shipping_widget_blueprint")
			});
	}

	TSharedPtr<FJsonObject> MakeUiRetainerBindParams(
		const FString& WidgetAssetPath,
		const FString& RetainerWidgetName,
		const FString& MaterialPath,
		const FString& TextureParameter,
		bool bCompileWidget,
		bool bRequestRender)
	{
		TSharedPtr<FJsonObject> Params = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
		Params->SetStringField(TEXT("widget_name"), RetainerWidgetName);
		Params->SetStringField(TEXT("material_path"), MaterialPath);
		Params->SetStringField(TEXT("texture_parameter"), TextureParameter);
		Params->SetBoolField(TEXT("require_ui_material"), true);
		Params->SetBoolField(TEXT("compile"), bCompileWidget);
		Params->SetBoolField(TEXT("request_render"), bRequestRender);
		return Params;
	}

	FMonolithActionExecutionPolicy MakeWorkflowMutationPolicy()
	{
		FMonolithActionExecutionPolicy Policy;
		Policy.PolicyId = TEXT("transaction_optional");
		Policy.bDefaulted = false;
		Policy.bDirtyPackageTracking = true;
		Policy.bTransactionWrapping = true;
		Policy.bPostEditValidation = false;
		Policy.bEnforced = true;
		return Policy;
	}
}

#if WITH_DEV_AUTOMATION_TESTS
namespace MonolithWorkflowActionsTestSupport
{
	bool ExecuteReadOnlyPrimitiveForTest(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		TSharedPtr<FJsonObject>& OutProof,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<FString>& OutErrors)
	{
		return ExecuteReadOnlyPrimitive(
			Namespace,
			Action,
			Params,
			OutProof,
			OutActions,
			OutErrors);
	}

	bool ExecutePrimitiveForTest(
		const FString& Namespace,
		const FString& Action,
		const TSharedPtr<FJsonObject>& Params,
		TArray<TSharedPtr<FJsonValue>>& OutActions,
		TArray<TSharedPtr<FJsonValue>>& OutProofRows,
		TArray<FString>& OutErrors)
	{
		return PlanOrExecutePrimitive(
			Namespace,
			Action,
			Params,
			true,
			true,
			OutActions,
			OutProofRows,
			OutErrors);
	}
}
#endif

void FMonolithWorkflowActions::RegisterAll()
{
	FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("game_ready_asset_static_mesh"),
		TEXT("Compose a read-only StaticMesh game-ready asset workflow proof envelope: provenance, mesh validation plan/result, material diagnostics plan/result, save/source-control status, blockers, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleGameReadyAssetStaticMesh),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("mesh_asset_path"), TEXT("StaticMesh asset path to validate and report as the primary game-ready target."), { TEXT("asset_path"), TEXT("static_mesh") })
			.OptionalAssetPath(TEXT("material_asset_path"), TEXT("Optional material or material instance path to include in compile/budget diagnostics."), { TEXT("material"), TEXT("material_path") })
			.Optional(TEXT("provenance"), TEXT("object"), TEXT("Optional source/provenance sidecar supplied by import, imagegen, modelgen, or interchange steps."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only the plan/proof envelope without loading or validating assets."), TEXT("true"))
			.Optional(TEXT("run_validation"), TEXT("boolean"), TEXT("When dry_run=false, run read-only mesh/material diagnostics through existing actions."), TEXT("true"))
			.Optional(TEXT("include_material_diagnostics"), TEXT("boolean"), TEXT("When dry_run=false and material_asset_path is set, run material validation and compile stats."), TEXT("true"))
			.Optional(TEXT("preview_required"), TEXT("boolean"), TEXT("When true, report the explicit preview blocker and next action."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker with asset.save_asset next actions."), TEXT("false"))
			.Build(),
		TEXT("asset_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("game ready asset"),
				TEXT("static mesh workflow"),
				TEXT("asset proof envelope"),
				TEXT("mesh material validation"),
				TEXT("source control prepare")
			},
			{
				TEXT("ship ready asset"),
				TEXT("static mesh proof"),
				TEXT("content workflow")
			},
			{
				TEXT("prove this static mesh is game ready"),
				TEXT("compose mesh validation material diagnostics and save status")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-asset"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Use mesh.validate_game_ready for the StaticMesh proof."),
				TEXT("Use material.validate_material and material.get_compilation_stats for material proof when a material path is supplied.")
			},
			{
				TEXT("status:string"),
				TEXT("workflow_id:string"),
				TEXT("plan.steps[]"),
				TEXT("actions[]"),
				TEXT("touched.assets[]"),
				TEXT("dirty_packages[]"),
				TEXT("source_control:{prepared,status,blocked[]}"),
				TEXT("validation:{compile,asset_validation,budget}"),
				TEXT("proof:{read_back,preview_artifacts,logs,benchmarks}"),
				TEXT("warnings[]"),
				TEXT("errors[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("mesh.validate_game_ready"),
				TEXT("material.validate_material"),
				TEXT("material.get_compilation_stats"),
				TEXT("material.render_preview"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("game_ready_asset_static_mesh"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan StaticMesh game-ready asset proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		TEXT("Compose a read-only gameplay feature manifest preflight across Enhanced Input, GAS, Blueprint, AI, GameFeatures, WorldConditions, and a declared PIE runtime proof chain."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleGameplayFeatureManifest),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("feature_id"), TEXT("string"), TEXT("Stable gameplay feature identifier for this manifest workflow."))
			.Required(TEXT("manifest"), TEXT("object"), TEXT("Feature manifest with input, gas, blueprint, ai, gamefeatures, world_conditions, and runtime sections."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, only return planned preflight rows and proof contract."), TEXT("true"))
			.Optional(TEXT("run_validation"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only validators."), TEXT("true"))
			.Optional(TEXT("runtime_proof_required"), TEXT("boolean"), TEXT("When true, block this first slice and declare the PIE proof chain."), TEXT("false"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Reserved for later mutating/runtime slices. confirm=true is blocked in this read-only first slice."), TEXT("false"))
			.Build(),
		TEXT("gameplay_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("gameplay feature workflow"),
				TEXT("feature manifest"),
				TEXT("enhanced input gas blueprint ai"),
				TEXT("runtime proof chain")
			},
			{
				TEXT("gameplay feature preflight"),
				TEXT("input gas workflow"),
				TEXT("feature proof")
			},
			{
				TEXT("preflight a gameplay feature manifest"),
				TEXT("compose input gas blueprint ai runtime proof")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-gas"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Provide manifest sections for each domain that participates in the feature."),
				TEXT("runtime_proof_required=true is declared but blocked until a later confirmed PIE workflow.")
			},
			{
				TEXT("workflow_id:gameplay_feature"),
				TEXT("workflow_slice:manifest_read_only_preflight_v1"),
				TEXT("validation:{input,gas,blueprint,ai,gamefeatures,world_conditions,runtime}"),
				TEXT("proof.read_back[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("input.get_input_action"),
				TEXT("input.validate_input_mappings"),
				TEXT("gas.validate_gas_setup"),
				TEXT("blueprint.validate_blueprint"),
				TEXT("ai.validate_behavior_tree"),
				TEXT("gamefeatures.validate_plugin"),
				TEXT("world_conditions.describe_query"),
				TEXT("gas.expect_event_cue")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("gameplay_feature_manifest"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan gameplay feature manifest proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		TEXT("Compose a deterministic level/world-builder blockout workflow: dirty preflight, blank map creation, one tagged volume, optional primitives/scatter/analysis/collection/save/source-control proof, and recovery limits."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleLevelWorldBuilderBlockout),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("map_path"), TEXT("New /Game UWorld path for the blockout map."))
			.Required(TEXT("volume"), TEXT("object"), TEXT("Blockout volume spec: {name, location, extent, room_type, rotation?}."))
			.Required(TEXT("seed"), TEXT("integer"), TEXT("Non-zero deterministic seed for scatter/settle behavior."))
			.Optional(TEXT("primitives"), TEXT("array"), TEXT("Optional blockout primitive specs. Hard cap: 200."))
			.Optional(TEXT("scatter"), TEXT("object"), TEXT("Optional prop scatter spec: {asset_paths,count,min_spacing?,random_scale_range?,collision_mode?}."))
			.Optional(TEXT("analysis"), TEXT("object"), TEXT("Optional leveldesign analysis spec."))
			.Optional(TEXT("collection"), TEXT("object"), TEXT("Optional Content Browser collection spec: {name,share_type}."))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("When true and confirm=true, save only the requested map package."), TEXT("false"))
			.Optional(TEXT("prepare_source_control"), TEXT("boolean"), TEXT("When true and confirm=true, prepare map path through source_control.checkout_or_add."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only the plan/proof envelope without mutation."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required for any map or scene mutation."), TEXT("false"))
			.Build(),
		TEXT("level_workflow"),
		MakeWorkflowMutationPolicy(),
		FMonolithActionSearchMetadata{
			{
				TEXT("level workflow"),
				TEXT("world builder blockout"),
				TEXT("deterministic seed"),
				TEXT("blank map blockout volume")
			},
			{
				TEXT("level blockout workflow"),
				TEXT("worldgen blockout proof"),
				TEXT("map blockout composer")
			},
			{
				TEXT("create a deterministic blockout map"),
				TEXT("plan build validate save source control for level blockout")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-worldgen"),
			{
				TEXT("dry_run=false requires confirm=true."),
				TEXT("seed must be non-zero."),
				TEXT("primitives are capped at 200 before mutation."),
				TEXT("save and source-control preparation are explicit booleans.")
			},
			{
				TEXT("workflow_id:level_workflow"),
				TEXT("workflow_slice:blockout_volume_v1"),
				TEXT("validation:{world_context,scene_statistics,blockout,leveldesign,save}"),
				TEXT("touched:{actors,assets,packages,files}"),
				TEXT("dirty_packages[]"),
				TEXT("source_control{}"),
				TEXT("proof.read_back[]")
			},
			{
				TEXT("editor.create_empty_map"),
				TEXT("editor.load_level"),
				TEXT("scene.spawn_volume"),
				TEXT("worldgen.setup_blockout_volume"),
				TEXT("worldgen.create_blockout_primitives_batch"),
				TEXT("worldgen.scatter_props"),
				TEXT("leveldesign.analyze_sightlines"),
				TEXT("editor.save_packages"),
				TEXT("source_control.checkout_or_add")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("level_world_builder_blockout"),
		/*bReadOnly=*/false,
		/*bDestructive=*/false,
		/*bIdempotent=*/false,
		TEXT("Apply or dry-run deterministic level blockout workflow"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		TEXT("Compose a read-only UI shipping readiness workflow for one Widget Blueprint: tree/spec/binding read-back, layout/accessibility/navigation/CommonUI audits, compile/preview/save/source-control blockers, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleUiShippingWidgetBlueprint),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("widget_asset_path"), TEXT("Widget Blueprint asset path to preflight."), { TEXT("asset_path"), TEXT("wbp_path") })
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only UI checks."), TEXT("true"))
			.Optional(TEXT("include_layout_audit"), TEXT("boolean"), TEXT("Include ui.audit_widget_layout."), TEXT("true"))
			.Optional(TEXT("include_accessibility_audit"), TEXT("boolean"), TEXT("Include ui.audit_accessibility."), TEXT("true"))
			.Optional(TEXT("include_navigation_audit"), TEXT("boolean"), TEXT("Include ui.dump_widget_navigation and ui.audit_focus_chain when registered."), TEXT("true"))
			.Optional(TEXT("include_commonui_audit"), TEXT("boolean"), TEXT("Include ui.audit_commonui_widget when registered."), TEXT("true"))
			.Optional(TEXT("include_binding_inventory"), TEXT("boolean"), TEXT("Include ui.get_widget_bindings."), TEXT("true"))
			.Optional(TEXT("binding_expectations"), TEXT("object"), TEXT("Optional expectations echoed into validation.ui.binding_expectations."))
			.Optional(TEXT("treat_warnings_as_errors"), TEXT("boolean"), TEXT("Forward to ui.audit_widget_layout."), TEXT("false"))
			.Optional(TEXT("layout_rule_profile"), TEXT("string"), TEXT("Forwarded to ui.audit_widget_layout rule_profile: advisory, shipping, or strict. Default shipping, strict for proof_profile=runtime."), TEXT("shipping"))
			.Optional(TEXT("suppress_layout_rule_ids"), TEXT("array"), TEXT("Forwarded to ui.audit_widget_layout suppress_rule_ids for intentional layout exceptions."))
			.Optional(TEXT("proof_profile"), TEXT("string"), TEXT("Proof profile: minimal, visual, or runtime. minimal preserves the conservative read-back workflow; visual executes compile/capture/artifact verification when dry_run=false; runtime reports async PIE proof blockers until a runtime_flow can be executed."), TEXT("minimal"))
			.Optional(TEXT("run_layout_measure"), TEXT("boolean"), TEXT("For proof_profile=visual/runtime, compose ui.measure_widget_layout as authored bounds, overlap, and safe-zone evidence. Default true."), TEXT("true"))
			.Optional(TEXT("round_trip_check"), TEXT("string"), TEXT("UISpec round-trip policy: auto, force, or off. v1 records the policy and leaves non-representable edits as limitations."), TEXT("auto"))
			.Optional(TEXT("run_id"), TEXT("string"), TEXT("Optional proof run id used for generated capture and manifest paths."))
			.OptionalDiskPath(TEXT("output_dir"), TEXT("Optional output directory for generated visual proof artifacts."))
			.Optional(TEXT("visual_profiles"), TEXT("array"), TEXT("Optional visual profile rows. v1 uses the first row: {name, resolution:[w,h], dpi_scale}."))
			.Optional(TEXT("runtime_flow"), TEXT("object"), TEXT("Optional runtime flow manifest for future async PIE proof. v1 reports concrete runtime blockers."))
			.Optional(TEXT("preview_required"), TEXT("boolean"), TEXT("When true, report editor.capture_scene_preview blocker and next action."), TEXT("false"))
			.OptionalDiskPath(TEXT("preview_output_path"), TEXT("Optional preview output path for the explicit capture next action."))
			.Optional(TEXT("preview_resolution"), TEXT("array"), TEXT("Optional preview resolution array for editor.capture_scene_preview next action."))
			.Optional(TEXT("preview_scale"), TEXT("number"), TEXT("Optional preview scale for editor.capture_scene_preview next action."), TEXT("1.0"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker."), TEXT("false"))
			.Build(),
		TEXT("ui_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("ui shipping workflow"),
				TEXT("widget blueprint readiness"),
				TEXT("umg audit proof"),
				TEXT("accessibility navigation commonui")
			},
			{
				TEXT("WBP shipping proof"),
				TEXT("UI readiness workflow"),
				TEXT("widget proof")
			},
			{
				TEXT("preflight a widget blueprint for shipping"),
				TEXT("audit WBP layout accessibility bindings and preview blockers")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-ui"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("proof_profile=minimal preserves the conservative plan-only compile/preview behavior."),
				TEXT("proof_profile=visual executes compile, widget capture, and ui.verify_widget_visual_artifacts when dry_run=false and run_read_only_checks=true."),
				TEXT("Save and source-control prepare remain explicit next actions."),
				TEXT("CommonUI checks are optional and availability-marked.")
			},
			{
				TEXT("workflow_id:ui_shipping"),
				TEXT("workflow_slice:widget_blueprint_readiness_proof_v1"),
				TEXT("validation:{compile,asset_validation,accessibility,ui}"),
				TEXT("proof.read_back[]"),
				TEXT("proof.preview_artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("ui.get_widget_tree"),
				TEXT("ui.dump_ui_spec"),
				TEXT("ui.get_widget_bindings"),
				TEXT("ui.audit_widget_layout"),
				TEXT("ui.measure_widget_layout"),
				TEXT("ui.audit_accessibility"),
				TEXT("ui.dump_widget_navigation"),
				TEXT("ui.audit_focus_chain"),
				TEXT("ui.audit_commonui_widget"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("editor.capture_scene_preview"),
				TEXT("ui.verify_widget_visual_artifacts"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("ui_shipping_widget_blueprint"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan UI shipping Widget Blueprint proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		TEXT("Compose a ViewModel-safe UMG widget event binding workflow. Uses existing Blueprint graph primitives for component-bound widget events, ViewModel command calls, compile, and read-back proof instead of registering duplicate UI graph-edit actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleUiBindWidgetEvent),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("asset_path"), TEXT("Widget Blueprint asset path to edit."), { TEXT("widget_asset_path"), TEXT("wbp_path") })
			.Required(TEXT("widget_name"), TEXT("string"), TEXT("Named widget variable whose delegate should be bound, e.g. StartButton."))
			.Required(TEXT("event"), TEXT("string"), TEXT("Widget event/delegate name, e.g. OnClicked, Clicked, OnHovered."))
			.Required(TEXT("intent"), TEXT("object"), TEXT("Event intent. Supported v1 shape: {kind:'viewmodel_command', viewmodel_variable:'ViewModel', viewmodel_class?, command:'StartGame', optional pin overrides}."))
			.Optional(TEXT("graph_name"), TEXT("string"), TEXT("Event graph name. Defaults to EventGraph."), TEXT("EventGraph"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return the child-action plan only."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false before Blueprint graph mutation."), TEXT("false"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile the Widget Blueprint after confirmed graph edits."), TEXT("true"))
			.Optional(TEXT("run_read_back"), TEXT("boolean"), TEXT("Resolve nodes before mutation and read graph summary after confirmed edits."), TEXT("true"))
			.Build(),
		TEXT("ui_workflow"),
		MakeWorkflowMutationPolicy(),
		FMonolithActionSearchMetadata{
			{
				TEXT("ui event binding workflow"),
				TEXT("umg button onclicked viewmodel command"),
				TEXT("widget event graph proof"),
				TEXT("viewmodel boundary")
			},
			{
				TEXT("bind widget event"),
				TEXT("button clicked workflow"),
				TEXT("WBP event binding")
			},
			{
				TEXT("bind StartButton OnClicked to ViewModel.StartGame"),
				TEXT("create a UMG event binding with compile proof"),
				TEXT("wire widget event through a ViewModel command")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-ui"),
			{
				TEXT("Use dry_run=true first; the workflow reports the exact Blueprint actions it will compose."),
				TEXT("Only intent.kind=viewmodel_command is accepted for runtime UI; direct Actor/Pawn/Controller calls are rejected."),
				TEXT("dry_run=false requires confirm=true and then executes blueprint.add_node/connect_pins/compile_blueprint where available."),
				TEXT("Use widget_name for the named UMG widget variable and event for its BlueprintAssignable delegate.")
			},
			{
				TEXT("workflow_id:ui_event_binding"),
				TEXT("workflow_slice:viewmodel_command_event_binding_v1"),
				TEXT("validation:{boundary,event_binding,compile,read_back}"),
				TEXT("proof.read_back[]"),
				TEXT("actions[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("blueprint.resolve_node"),
				TEXT("blueprint.add_node"),
				TEXT("blueprint.connect_pins"),
				TEXT("blueprint.compile_blueprint"),
				TEXT("blueprint.get_graph_summary"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("ui_bind_widget_event"),
		/*bReadOnly=*/false,
		/*bDestructive=*/false,
		/*bIdempotent=*/false,
		TEXT("Bind UMG widget event through ViewModel-safe Blueprint workflow"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		TEXT("Compose a UI-domain material Custom-HLSL effect workflow. Uses existing material and UI owner actions for material domain enforcement, Custom node authoring, compile/stat proof, Image/brush binding, and optional widget shipping proof instead of adding duplicate hlsl_* or external material_* APIs."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleUiMaterialHlslEffect),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("material_path"), TEXT("Material asset path to create or update."))
			.Required(TEXT("hlsl"), TEXT("string"), TEXT("Custom HLSL program. v1 assumes it returns float4 for UI material use."))
			.Required(TEXT("bind_to"), TEXT("object"), TEXT("Widget binding target: {asset_path, widget_name, property_name?, binding_action?}."))
			.Optional(TEXT("parameters"), TEXT("array"), TEXT("Array of HLSL parameter descriptors {name,type,default?}. Names become Custom-node inputs when valid."))
			.Optional(TEXT("additional_outputs"), TEXT("array"), TEXT("Optional Custom-node additional output descriptors forwarded to material.create_custom_hlsl_node/update_custom_hlsl_node. Use with opacity_output_pin for direct alpha wiring."))
			.Optional(TEXT("include_file_paths"), TEXT("array"), TEXT("Optional Custom-node include file paths forwarded when updating an existing Custom HLSL node."))
			.Optional(TEXT("additional_defines"), TEXT("object"), TEXT("Optional Custom-node preprocessor defines forwarded when updating an existing Custom HLSL node."))
			.Optional(TEXT("expression_name"), TEXT("string"), TEXT("Existing UMaterialExpressionCustom name to update. Omit to create a Custom node."))
			.Optional(TEXT("output_type"), TEXT("string"), TEXT("Custom node output type. Defaults to Float4."), TEXT("Float4"))
			.Optional(TEXT("create_material"), TEXT("boolean"), TEXT("Create material first through material.create_material. Default false to support existing-material round-trips."), TEXT("false"))
			.Optional(TEXT("blend_mode"), TEXT("string"), TEXT("UI material blend mode. Defaults to Translucent."), TEXT("Translucent"))
			.Optional(TEXT("shading_model"), TEXT("string"), TEXT("UI material shading model. Defaults to Unlit."), TEXT("Unlit"))
			.Optional(TEXT("connect_output_pin"), TEXT("string"), TEXT("Custom node output pin to connect to EmissiveColor. Empty uses default output."))
			.Optional(TEXT("connect_opacity"), TEXT("boolean"), TEXT("Also connect opacity_output_pin to Opacity, or use auto_component_mask_alpha to split the float4 output alpha channel. Default false."), TEXT("false"))
			.Optional(TEXT("opacity_output_pin"), TEXT("string"), TEXT("Custom node output pin for direct Opacity wiring when connect_opacity=true and auto_component_mask_alpha=false."), TEXT("Alpha"))
			.Optional(TEXT("auto_component_mask_alpha"), TEXT("boolean"), TEXT("When true, compose material.build_material_graph(clear_existing=false) to insert a ComponentMask that routes the Custom node float4 alpha to Opacity. Implies connect_opacity=true."), TEXT("false"))
			.Optional(TEXT("opacity_source_channel"), TEXT("string"), TEXT("Component channel to route through the generated ComponentMask when auto_component_mask_alpha=true. One of R/G/B/A; default A."), TEXT("A"))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Run material compile/stat proof and widget compile after binding."), TEXT("true"))
			.Optional(TEXT("run_widget_proof"), TEXT("boolean"), TEXT("Run workflow.ui_shipping_widget_blueprint after binding. Default false; returned as next action otherwise."), TEXT("false"))
			.Optional(TEXT("pos_x"), TEXT("number"), TEXT("Optional editor graph X position forwarded to material Custom node authoring."))
			.Optional(TEXT("pos_y"), TEXT("number"), TEXT("Optional editor graph Y position forwarded to material Custom node authoring."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return owner-action plan only."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false before material/widget mutation."), TEXT("false"))
			.Build(),
		TEXT("ui_workflow"),
		MakeWorkflowMutationPolicy(),
		FMonolithActionSearchMetadata{
			{
				TEXT("ui material hlsl workflow"),
				TEXT("umg material brush custom hlsl"),
				TEXT("ui domain material proof"),
				TEXT("widget material binding")
			},
			{
				TEXT("create UI hlsl material"),
				TEXT("bind material to image brush"),
				TEXT("material custom node workflow")
			},
			{
				TEXT("create a UI-domain HLSL material and bind it to an Image"),
				TEXT("lint UI HLSL and compile material before widget visual proof"),
				TEXT("wire material custom node through existing material owner actions")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-ui"),
			{
				TEXT("Use dry_run=true first; the workflow reports every material/ui owner action it will compose."),
				TEXT("No hlsl namespace or external material_* aliases are registered; low-level work stays in material and ui."),
				TEXT("create_material defaults false so existing material round-trips do not fail on already-existing assets."),
				TEXT("run_widget_proof=false returns workflow.ui_shipping_widget_blueprint as an explicit next action.")
			},
			{
				TEXT("workflow_id:ui_material_hlsl_effect"),
				TEXT("validation:{hlsl,material_proof,binding_proof,widget_proof}"),
				TEXT("proof.material[]"),
				TEXT("proof.widget[]"),
				TEXT("actions[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("material.create_material"),
				TEXT("material.set_material_property"),
				TEXT("material.create_custom_hlsl_node"),
				TEXT("material.update_custom_hlsl_node"),
				TEXT("material.connect_expressions"),
				TEXT("material.build_material_graph"),
				TEXT("material.recompile_material"),
				TEXT("material.validate_material"),
				TEXT("material.get_compilation_stats"),
				TEXT("material.get_material_properties"),
				TEXT("material.get_full_connection_graph"),
				TEXT("ui.set_image"),
				TEXT("ui.set_brush"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("workflow.ui_shipping_widget_blueprint"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("ui_material_hlsl_effect"),
		/*bReadOnly=*/false,
		/*bDestructive=*/false,
		/*bIdempotent=*/false,
		TEXT("Create/update UI-domain HLSL material and bind it to UMG"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("ui_retainer_effect_material"),
		TEXT("Compose a RetainerBox effect-material workflow. Uses material.get_material_parameters/get_material_properties plus ui.set_retainer_effect_material to prove UI-domain material binding and exact Retainer texture-parameter match without adding duplicate low-level material or raw UMG reflection actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleUiRetainerEffectMaterial),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("material_path"), TEXT("UI-domain Retainer effect material asset path."))
			.Required(TEXT("bind_to"), TEXT("object"), TEXT("Retainer binding target: {asset_path, retainer_widget_name|widget_name, texture_parameter?}."))
			.Optional(TEXT("compile"), TEXT("boolean"), TEXT("Compile/read Widget Blueprint after binding."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("Execute material readback even in dry_run mode. Default false so planning can run without assets."), TEXT("false"))
			.Optional(TEXT("request_render"), TEXT("boolean"), TEXT("Call RetainerBox RequestRender during confirmed binding."), TEXT("false"))
			.Optional(TEXT("run_widget_proof"), TEXT("boolean"), TEXT("Run workflow.ui_shipping_widget_blueprint after binding. Default false; returned as next action otherwise."), TEXT("false"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return owner-action plan only unless run_read_only_checks=true."), TEXT("true"))
			.Optional(TEXT("confirm"), TEXT("boolean"), TEXT("Required with dry_run=false before Widget Blueprint mutation."), TEXT("false"))
			.Build(),
		TEXT("ui_workflow"),
		MakeWorkflowMutationPolicy(),
		FMonolithActionSearchMetadata{
			{
				TEXT("ui retainer effect material workflow"),
				TEXT("retainerbox material texture parameter proof"),
				TEXT("umg retainer blur effect"),
				TEXT("retainer exact texture parameter")
			},
			{
				TEXT("bind material to retainer box"),
				TEXT("prove retainer texture parameter"),
				TEXT("set RetainerBox effect material")
			},
			{
				TEXT("bind a UI-domain effect material to a RetainerBox"),
				TEXT("verify RetainerBox texture parameter exists on the material"),
				TEXT("compose material readback and UI owner action instead of raw reflection")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-ui"),
			{
				TEXT("Use dry_run=true first; the workflow reports material readback and Retainer owner binding steps."),
				TEXT("Do not auto-insert RetainerBox or InvalidationBox for optimization; this workflow only binds an existing RetainerBox."),
				TEXT("Texture parameter defaults to Texture, matching UE RetainerBox defaults, but exact material readback is required before proof."),
				TEXT("Performance claims require a separate measured before/after runtime profile.")
			},
			{
				TEXT("workflow_id:ui_retainer_effect_material"),
				TEXT("validation:{material_parameter_proof,material_domain_proof,binding_proof,widget_proof,runtime_profile}"),
				TEXT("proof.material[]"),
				TEXT("proof.widget[]"),
				TEXT("actions[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("material.get_material_parameters"),
				TEXT("material.get_material_properties"),
				TEXT("ui.set_retainer_effect_material"),
				TEXT("ui.compile_widget"),
				TEXT("ui.dump_blueprint_compile_log"),
				TEXT("workflow.ui_shipping_widget_blueprint"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("ui_retainer_effect_material"),
		/*bReadOnly=*/false,
		/*bDestructive=*/false,
		/*bIdempotent=*/false,
		TEXT("Bind UI-domain RetainerBox effect material with texture-parameter proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("shot_render_level_sequence"),
		TEXT("Compose a read-only cinematic shot render readiness workflow: Level Sequence bindings/director proof, optional Anim Mixer read-back, Movie Render Queue state, render blocker, artifacts, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleShotRenderLevelSequence),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("sequence_asset_path"), TEXT("Level Sequence asset path to preflight for shot rendering."), { TEXT("asset_path"), TEXT("level_sequence") })
			.OptionalAssetPath(TEXT("queue_asset_path"), TEXT("Optional Movie Render Queue asset to load in an explicit follow-up action."))
			.OptionalAssetPath(TEXT("map_path"), TEXT("Optional map/world object path for the MRQ job plan."))
			.Optional(TEXT("job_name"), TEXT("string"), TEXT("Optional MRQ job name for the planned movie_render.add_job action."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only sequence and MRQ checks."), TEXT("true"))
			.Optional(TEXT("include_anim_mixer"), TEXT("boolean"), TEXT("Include optional Sequencer Anim Mixer readiness checks."), TEXT("true"))
			.Optional(TEXT("render_required"), TEXT("boolean"), TEXT("When true, return a blocked render gate and movie_render.render_queue next action."), TEXT("false"))
			.OptionalDiskPath(TEXT("output_directory"), TEXT("Optional intended render output directory, echoed into artifacts and next actions."))
			.Build(),
		TEXT("cinematic_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("shot render workflow"),
				TEXT("movie render queue"),
				TEXT("level sequence proof"),
				TEXT("cinematic readiness")
			},
			{
				TEXT("MRQ proof"),
				TEXT("shot render"),
				TEXT("sequence render readiness")
			},
			{
				TEXT("preflight a level sequence for movie render queue"),
				TEXT("compose sequence binding director and MRQ render proof")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-level-sequences"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Rendering requires explicit movie_render.render_queue with confirm=true."),
				TEXT("MRQ queue mutations are declared as next actions, not executed by this workflow slice.")
			},
			{
				TEXT("workflow_id:shot_render"),
				TEXT("workflow_slice:level_sequence_mrq_readiness_proof_v1"),
				TEXT("validation:{asset_validation,render,runtime}"),
				TEXT("proof.read_back[]"),
				TEXT("artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("level_sequence.list_bindings"),
				TEXT("level_sequence.get_director_info"),
				TEXT("movie_render.get_queue"),
				TEXT("movie_render.add_job"),
				TEXT("movie_render.render_queue")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("shot_render_level_sequence"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan cinematic shot render proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("audio_shipping_asset"),
		TEXT("Compose a read-only audio shipping readiness workflow for one audio asset: type-aware graph/read-back validation, perception binding proof, preview/save blockers, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleAudioShippingAsset),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("audio_asset_path"), TEXT("Audio asset path to preflight."))
			.Optional(TEXT("asset_kind"), TEXT("string"), TEXT("auto | SoundWave | SoundCue | MetaSoundSource. Type-specific validators run only when a concrete kind is supplied."), TEXT("auto"))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only audio checks."), TEXT("true"))
			.Optional(TEXT("include_perception_binding"), TEXT("boolean"), TEXT("Include audio.get_sound_perception_binding."), TEXT("true"))
			.Optional(TEXT("preview_required"), TEXT("boolean"), TEXT("When true, report audio.preview_sound blocker and next action."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker."), TEXT("false"))
			.Build(),
		TEXT("audio_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("audio shipping workflow"),
				TEXT("metasound soundcue soundwave readiness"),
				TEXT("sound preview blocker"),
				TEXT("perception binding proof")
			},
			{
				TEXT("audio proof"),
				TEXT("sound asset shipping"),
				TEXT("metasound shipping")
			},
			{
				TEXT("preflight an audio asset for shipping"),
				TEXT("validate metasound sound cue or sound wave and report preview blockers")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-audio"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Pass asset_kind for type-specific read-back; asset_kind=auto avoids guessed validators."),
				TEXT("Preview, save, and source-control prepare are explicit follow-up actions.")
			},
			{
				TEXT("workflow_id:audio_shipping"),
				TEXT("workflow_slice:audio_asset_readiness_proof_v1"),
				TEXT("validation:{asset_validation,runtime,budget}"),
				TEXT("proof.read_back[]"),
				TEXT("proof.preview_artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("audio.search_audio_assets"),
				TEXT("audio.validate_metasound"),
				TEXT("audio.validate_sound_cue"),
				TEXT("audio.get_sound_wave_info"),
				TEXT("audio.preview_sound"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("audio_shipping_asset"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan audio shipping asset proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("localization_shipping_string_table"),
		TEXT("Compose a read-only localization shipping readiness workflow for one StringTable: cultures, table read-back, validation, CSV export blocker, save/source-control status, and next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleLocalizationShippingStringTable),
		FParamSchemaBuilder()
			.EnableValidation()
			.RequiredAssetPath(TEXT("string_table_path"), TEXT("StringTable asset path to preflight."))
			.Optional(TEXT("cultures"), TEXT("array"), TEXT("Optional target culture names echoed into validation expectations."))
			.OptionalDiskPath(TEXT("csv_path"), TEXT("Optional intended CSV path for explicit export/import follow-up actions."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading assets."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only localization checks."), TEXT("true"))
			.Optional(TEXT("export_requested"), TEXT("boolean"), TEXT("When true, return a blocked export gate and localization.export_string_table_csv next action."), TEXT("false"))
			.Optional(TEXT("save"), TEXT("boolean"), TEXT("Request save reporting. The first slice never saves directly; save=true returns a blocker."), TEXT("false"))
			.Build(),
		TEXT("localization_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("localization shipping workflow"),
				TEXT("string table validation"),
				TEXT("culture proof"),
				TEXT("CSV export blocker")
			},
			{
				TEXT("string table proof"),
				TEXT("localization readiness"),
				TEXT("l10n shipping")
			},
			{
				TEXT("preflight a string table for localization shipping"),
				TEXT("validate string table cultures and CSV export readiness")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-localization"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("CSV import/export and entry writes remain explicit localization actions."),
				TEXT("Save and source-control prepare are explicit follow-up actions.")
			},
			{
				TEXT("workflow_id:localization_shipping"),
				TEXT("workflow_slice:string_table_readiness_proof_v1"),
				TEXT("validation:{asset_validation,localization}"),
				TEXT("proof.read_back[]"),
				TEXT("artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("localization.list_cultures"),
				TEXT("localization.get_string_table"),
				TEXT("localization.validate_string_table"),
				TEXT("localization.export_string_table_csv"),
				TEXT("asset.save_asset")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("localization_shipping_string_table"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan localization shipping StringTable proof"));

	Registry.RegisterAction(
		TEXT("workflow"),
		TEXT("slate_euw_test_flow"),
		TEXT("Compose a read-only Slate/EUW interaction-test readiness workflow: inspector status, window/widget snapshot, target description, capture blocker, input-simulation blocker, and availability-marked next actions."),
		FMonolithActionHandler::CreateStatic(&FMonolithWorkflowActions::HandleSlateEuwTestFlow),
		FParamSchemaBuilder()
			.EnableValidation()
			.Required(TEXT("target"), TEXT("string"), TEXT("Slate widget path/text, window title, or Editor Utility Widget surface to inspect."))
			.Optional(TEXT("target_kind"), TEXT("string"), TEXT("window | widget | text | path | euw_asset | auto."), TEXT("auto"))
			.Optional(TEXT("ref"), TEXT("string"), TEXT("Optional opaque Slate ref returned by slate.snapshot_widgets for describe/capture follow-up rows."))
			.Optional(TEXT("dry_run"), TEXT("boolean"), TEXT("When true, return only planned proof rows without reading live Slate state."), TEXT("true"))
			.Optional(TEXT("run_read_only_checks"), TEXT("boolean"), TEXT("When dry_run=false, run available read-only Slate checks."), TEXT("true"))
			.Optional(TEXT("include_snapshot"), TEXT("boolean"), TEXT("Include slate.snapshot_widgets."), TEXT("true"))
			.Optional(TEXT("include_wait"), TEXT("boolean"), TEXT("Include slate.wait_for_widget as a bounded read-only readiness row."), TEXT("true"))
			.Optional(TEXT("wait_timeout_sec"), TEXT("number"), TEXT("Timeout for the planned or executed wait row."), TEXT("2.0"))
			.Optional(TEXT("capture_required"), TEXT("boolean"), TEXT("When true, report slate.capture_widget as a blocked explicit follow-up artifact action."), TEXT("false"))
			.OptionalDiskPath(TEXT("capture_output_path"), TEXT("Optional output path for the explicit capture next action."))
			.Optional(TEXT("interaction_required"), TEXT("boolean"), TEXT("When true, return blocked status because click/type/key actions are not exposed by this first slice."), TEXT("false"))
			.Optional(TEXT("interaction_plan"), TEXT("array"), TEXT("Optional planned click/type/key/wait steps echoed into validation.interaction.plan."))
			.Build(),
		TEXT("slate_workflow"),
		FMonolithActionExecutionPolicy::DefaultReadOnly(),
		FMonolithActionSearchMetadata{
			{
				TEXT("slate euw test flow"),
				TEXT("editor utility widget interaction proof"),
				TEXT("slate input simulation blocker"),
				TEXT("widget capture workflow")
			},
			{
				TEXT("Slate EUW proof"),
				TEXT("editor UI test flow"),
				TEXT("Slate interaction blocker")
			},
			{
				TEXT("preflight a Slate or Editor Utility Widget interaction test"),
				TEXT("inspect live editor UI and report unavailable click type key proof")
			}
		},
		FMonolithActionPlanningMetadata{
			TEXT("unreal-slate"),
			{
				TEXT("Use dry_run=true first; this first slice is read-only."),
				TEXT("Click/type/key simulation is not exposed and is reported as an explicit blocker."),
				TEXT("Widget capture writes an artifact and remains an explicit follow-up action.")
			},
			{
				TEXT("workflow_id:slate_euw_test_flow"),
				TEXT("workflow_slice:slate_euw_readiness_proof_v1"),
				TEXT("validation:{ui,runtime,interaction}"),
				TEXT("proof.read_back[]"),
				TEXT("proof.preview_artifacts[]"),
				TEXT("next_actions[]")
			},
			{
				TEXT("slate.get_inspector_status"),
				TEXT("slate.list_windows"),
				TEXT("slate.snapshot_widgets"),
				TEXT("slate.describe_widget"),
				TEXT("slate.wait_for_widget"),
				TEXT("slate.capture_widget")
			}
		});

	Registry.SetActionAnnotations(
		TEXT("workflow"),
		TEXT("slate_euw_test_flow"),
		/*bReadOnly=*/true,
		/*bDestructive=*/false,
		/*bIdempotent=*/true,
		TEXT("Plan Slate/EUW interaction-test proof"));
}

FMonolithActionResult FMonolithWorkflowActions::HandleGameReadyAssetStaticMesh(const TSharedPtr<FJsonObject>& Params)
{
	FString MeshAssetPath;
	Params->TryGetStringField(TEXT("mesh_asset_path"), MeshAssetPath);

	FString MaterialAssetPath;
	Params->TryGetStringField(TEXT("material_asset_path"), MaterialAssetPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);

	bool bRunValidation = true;
	Params->TryGetBoolField(TEXT("run_validation"), bRunValidation);

	bool bIncludeMaterialDiagnostics = true;
	Params->TryGetBoolField(TEXT("include_material_diagnostics"), bIncludeMaterialDiagnostics);

	bool bPreviewRequired = false;
	Params->TryGetBoolField(TEXT("preview_required"), bPreviewRequired);

	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("game_ready_asset"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("static_mesh_read_only_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("mesh_asset_path"), MeshAssetPath);
	if (!MaterialAssetPath.IsEmpty())
	{
		Input->SetStringField(TEXT("material_asset_path"), MaterialAssetPath);
	}
	const TSharedPtr<FJsonObject>* ProvenancePtr = nullptr;
	if (Params->TryGetObjectField(TEXT("provenance"), ProvenancePtr) && ProvenancePtr && ProvenancePtr->IsValid())
	{
		Input->SetObjectField(TEXT("provenance"), *ProvenancePtr);
	}
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeStaticMeshPlanObject(MeshAssetPath, MaterialAssetPath, bDryRun, bRunValidation, bIncludeMaterialDiagnostics));

	TArray<FString> TouchedAssets = { MeshAssetPath };
	if (!MaterialAssetPath.IsEmpty())
	{
		TouchedAssets.Add(MaterialAssetPath);
	}
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, TouchedAssets, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Budget = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Runtime = MakeUnavailableProof(TEXT("not_applicable"), TEXT("StaticMesh asset proof does not run PIE/runtime checks in this first slice."));
	TSharedPtr<FJsonObject> Accessibility = MakeUnavailableProof(TEXT("not_applicable"), TEXT("No accessibility proof applies to the StaticMesh asset slice."));

	if (bDryRun || !bRunValidation)
	{
		AssetValidation->SetObjectField(TEXT("mesh"), MakeUnavailableProof(
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			bDryRun ? TEXT("dry_run=true; mesh.validate_game_ready is planned but not executed.") : TEXT("run_validation=false.")));
		Actions.Add(MakeShared<FJsonValueObject>(MakeActionRow(
			TEXT("mesh.validate_game_ready"),
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			false,
			FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("validate_game_ready")),
			MakeActionParams(TEXT("asset_path"), MeshAssetPath))));
	}
	else
	{
		TSharedPtr<FJsonObject> MeshProof;
		ExecuteReadOnlyPrimitive(
			TEXT("mesh"),
			TEXT("validate_game_ready"),
			MakeActionParams(TEXT("asset_path"), MeshAssetPath),
			MeshProof,
			Actions,
			Errors);
		AssetValidation->SetObjectField(TEXT("mesh"), MeshProof);
		if (MeshProof.IsValid() && MeshProof->HasField(TEXT("result")))
		{
			const TSharedPtr<FJsonObject>* ResultObj = nullptr;
			if (MeshProof->TryGetObjectField(TEXT("result"), ResultObj) && ResultObj && ResultObj->IsValid())
			{
				Budget->SetObjectField(TEXT("mesh"), *ResultObj);
			}
			else
			{
				return FMonolithActionResult::Error(TEXT("Invalid params: mesh proof result is not an object"), FMonolithJsonUtils::ErrInvalidParams);
			}
		}
	}

	if (MaterialAssetPath.IsEmpty())
	{
		Compile->SetObjectField(TEXT("material"), MakeUnavailableProof(TEXT("not_requested"), TEXT("No material_asset_path supplied.")));
	}
	else if (bDryRun || !bIncludeMaterialDiagnostics)
	{
		Compile->SetObjectField(TEXT("material"), MakeUnavailableProof(
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			bDryRun ? TEXT("dry_run=true; material diagnostics are planned but not executed.") : TEXT("include_material_diagnostics=false.")));
		Actions.Add(MakeShared<FJsonValueObject>(MakeActionRow(
			TEXT("material.validate_material"),
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			false,
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("validate_material")),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
		Actions.Add(MakeShared<FJsonValueObject>(MakeActionRow(
			TEXT("material.get_compilation_stats"),
			bDryRun ? TEXT("planned") : TEXT("not_requested"),
			false,
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_compilation_stats")),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
	}
	else
	{
		TSharedPtr<FJsonObject> MaterialValidationProof;
		ExecuteReadOnlyPrimitive(
			TEXT("material"),
			TEXT("validate_material"),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath),
			MaterialValidationProof,
			Actions,
			Errors);
		Compile->SetObjectField(TEXT("material_validation"), MaterialValidationProof);

		TSharedPtr<FJsonObject> CompilationStatsProof;
		ExecuteReadOnlyPrimitive(
			TEXT("material"),
			TEXT("get_compilation_stats"),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath),
			CompilationStatsProof,
			Actions,
			Errors);
		Compile->SetObjectField(TEXT("material"), CompilationStatsProof);
		Budget->SetObjectField(TEXT("material"), CompilationStatsProof);
	}

	Validation->SetObjectField(TEXT("compile"), Compile);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	Validation->SetObjectField(TEXT("runtime"), Runtime);
	Validation->SetObjectField(TEXT("budget"), Budget);
	Validation->SetObjectField(TEXT("accessibility"), Accessibility);
	Result->SetObjectField(TEXT("validation"), Validation);

	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{},
		{ TEXT("This first workflow slice is read-only; use explicit asset/source_control actions for checkout and save.") }));

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (bPreviewRequired)
	{
		TSharedPtr<FJsonObject> PreviewBlocker = MakeUnavailableProof(
			TEXT("blocked"),
			TEXT("Preview rendering is not performed by this read-only first slice; call material.render_preview with an explicit output path."));
		PreviewBlocker->SetStringField(TEXT("next_action"), TEXT("material.render_preview"));
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(PreviewBlocker));
		Warnings.Add(TEXT("preview_required=true but this first slice only reports the preview blocker and next action."));
	}

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), Actions);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);

	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.game_ready_asset_static_mesh is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("mesh.validate_game_ready"),
		FMonolithToolRegistry::Get().HasAction(TEXT("mesh"), TEXT("validate_game_ready")),
		true,
		TEXT("Run or repeat the StaticMesh game-ready checklist."),
		MakeActionParams(TEXT("asset_path"), MeshAssetPath))));
	if (!MaterialAssetPath.IsEmpty())
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("material.validate_material"),
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("validate_material")),
			true,
			TEXT("Validate material graph connections and issues."),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("material.get_compilation_stats"),
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_compilation_stats")),
			true,
			TEXT("Collect shader instruction/sampler budget diagnostics."),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
			TEXT("material.render_preview"),
			FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("render_preview")),
			true,
			TEXT("Produce a preview artifact with an explicit output path."),
			MakeActionParams(TEXT("asset_path"), MaterialAssetPath))));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("asset.save_asset"),
		FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")),
		true,
		TEXT("Persist reviewed mesh/material packages explicitly; this workflow slice does not save."),
		MakeActionParams(TEXT("asset_path"), MeshAssetPath))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No mutation is performed by this first slice, so automatic rollback is unnecessary."),
		TEXT("Import/generate, material build, save, and source-control prepare remain explicit follow-up actions.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	if (bDryRun)
	{
		Result->SetStringField(TEXT("status"), TEXT("planned"));
	}
	else if (Errors.Num() > 0 || bSaveRequested)
	{
		Result->SetStringField(TEXT("status"), TEXT("blocked"));
	}
	else
	{
		Result->SetStringField(TEXT("status"), TEXT("partial"));
		Warnings.Add(TEXT("Read-only proof completed where available; save/source-control/preview remain explicit follow-up steps."));
	}

	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleGameplayFeatureManifest(const TSharedPtr<FJsonObject>& Params)
{
	FString FeatureId;
	Params->TryGetStringField(TEXT("feature_id"), FeatureId);

	const TSharedPtr<FJsonObject>* ManifestPtr = nullptr;
	Params->TryGetObjectField(TEXT("manifest"), ManifestPtr);
	TSharedPtr<FJsonObject> Manifest = (ManifestPtr && ManifestPtr->IsValid()) ? *ManifestPtr : MakeShared<FJsonObject>();

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunValidation = true;
	Params->TryGetBoolField(TEXT("run_validation"), bRunValidation);
	bool bRuntimeProofRequired = false;
	Params->TryGetBoolField(TEXT("runtime_proof_required"), bRuntimeProofRequired);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);

	const bool bExecuteReadOnly = !bDryRun && bRunValidation;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TArray<FString> TouchedAssets;
	CollectAssetPathsFromObject(Manifest, TouchedAssets);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("feature_id"), FeatureId);
	Input->SetObjectField(TEXT("manifest"), Manifest);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("gameplay_feature"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("manifest_read_only_preflight_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeGameplayPlanObject(bDryRun, bRuntimeProofRequired));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, TouchedAssets, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{},
		{ TEXT("Gameplay feature first slice is read-only; authoring, PIE, save, and source-control actions remain explicit follow-ups.") }));

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> InputValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GasValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> BlueprintValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AiValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> GameFeaturesValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> WorldConditionsValidation = MakeShared<FJsonObject>();

	TArray<TSharedPtr<FJsonValue>> InputRows;
	TSharedPtr<FJsonObject> InputSection = GetObjectFieldOrEmpty(Manifest, TEXT("input"));
	const TArray<FString> InputActions = GetStringArrayField(InputSection, TEXT("input_actions"));
	const TArray<FString> MappingContexts = GetStringArrayField(InputSection, TEXT("mapping_contexts"));
	for (const FString& AssetPath : InputActions)
	{
		PlanOrExecutePrimitive(TEXT("input"), TEXT("get_input_action"), MakeActionParams(TEXT("asset_path"), AssetPath), bExecuteReadOnly, true, Actions, InputRows, Errors);
	}
	for (const FString& AssetPath : MappingContexts)
	{
		PlanOrExecutePrimitive(TEXT("input"), TEXT("get_input_mapping_context"), MakeActionParams(TEXT("asset_path"), AssetPath), bExecuteReadOnly, true, Actions, InputRows, Errors);
	}
	if (MappingContexts.Num() > 0)
	{
		PlanOrExecutePrimitive(TEXT("input"), TEXT("validate_input_mappings"), MakeStringArrayParams(TEXT("context_paths"), MappingContexts), bExecuteReadOnly, true, Actions, InputRows, Errors);
	}
	InputValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	InputValidation->SetArrayField(TEXT("read_back"), InputRows);

	TArray<TSharedPtr<FJsonValue>> GasRows;
	TSharedPtr<FJsonObject> GasSection = GetObjectFieldOrEmpty(Manifest, TEXT("gas"));
	PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_gas_setup"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, GasRows, Errors);
	FString GasActorPath;
	GasSection->TryGetStringField(TEXT("actor_path"), GasActorPath);
	if (!GasActorPath.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("get_ability_input_bindings"), MakeActionParams(TEXT("actor_path"), GasActorPath), bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	for (const FString& AbilityPath : GetStringArrayField(GasSection, TEXT("ability_paths")))
	{
		TSharedPtr<FJsonObject> AbilityParams = MakeActionParams(TEXT("asset_path"), AbilityPath);
		AbilityParams->SetBoolField(TEXT("release_input_supported"), false);
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_ability_blueprint"), AbilityParams, bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	for (const FString& EffectPath : GetStringArrayField(GasSection, TEXT("effect_paths")))
	{
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_effect"), MakeActionParams(TEXT("asset_path"), EffectPath), bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	FString CuePathFilter;
	GasSection->TryGetStringField(TEXT("cue_path_filter"), CuePathFilter);
	if (!CuePathFilter.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("gas"), TEXT("validate_cue_coverage"), MakeActionParams(TEXT("path_filter"), CuePathFilter), bExecuteReadOnly, true, Actions, GasRows, Errors);
	}
	GasValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	GasValidation->SetArrayField(TEXT("read_back"), GasRows);

	TArray<TSharedPtr<FJsonValue>> BlueprintRows;
	TSharedPtr<FJsonObject> BlueprintSection = GetObjectFieldOrEmpty(Manifest, TEXT("blueprint"));
	TArray<FString> BlueprintPaths;
	FString PathValue;
	if (BlueprintSection->TryGetStringField(TEXT("pawn_path"), PathValue) && !PathValue.IsEmpty())
	{
		BlueprintPaths.Add(PathValue);
	}
	if (BlueprintSection->TryGetStringField(TEXT("controller_path"), PathValue) && !PathValue.IsEmpty())
	{
		BlueprintPaths.Add(PathValue);
	}
	BlueprintPaths.Append(GetStringArrayField(BlueprintSection, TEXT("component_paths")));
	for (const FString& BlueprintPath : BlueprintPaths)
	{
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("get_blueprint_info"), MakeActionParams(TEXT("asset_path"), BlueprintPath), bExecuteReadOnly, true, Actions, BlueprintRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("get_components"), MakeActionParams(TEXT("asset_path"), BlueprintPath), bExecuteReadOnly, true, Actions, BlueprintRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("validate_blueprint"), MakeActionParams(TEXT("asset_path"), BlueprintPath), bExecuteReadOnly, true, Actions, BlueprintRows, Errors);
	}
	BlueprintValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	BlueprintValidation->SetArrayField(TEXT("read_back"), BlueprintRows);

	TArray<TSharedPtr<FJsonValue>> AiRows;
	TSharedPtr<FJsonObject> AiSection = GetObjectFieldOrEmpty(Manifest, TEXT("ai"));
	if (AiSection->TryGetStringField(TEXT("behavior_tree_path"), PathValue) && !PathValue.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("ai"), TEXT("validate_behavior_tree"), MakeActionParams(TEXT("asset_path"), PathValue), bExecuteReadOnly, true, Actions, AiRows, Errors);
	}
	if (AiSection->TryGetStringField(TEXT("state_tree_path"), PathValue) && !PathValue.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("ai"), TEXT("validate_state_tree"), MakeActionParams(TEXT("asset_path"), PathValue), bExecuteReadOnly, true, Actions, AiRows, Errors);
	}
	if (AiSection->TryGetStringField(TEXT("ai_controller_path"), PathValue) && !PathValue.IsEmpty())
	{
		PlanOrExecutePrimitive(TEXT("ai"), TEXT("validate_ai_controller"), MakeActionParams(TEXT("asset_path"), PathValue), bExecuteReadOnly, true, Actions, AiRows, Errors);
	}
	AiValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AiValidation->SetArrayField(TEXT("read_back"), AiRows);

	TArray<TSharedPtr<FJsonValue>> GameFeatureRows;
	TSharedPtr<FJsonObject> GameFeatureSection = GetObjectFieldOrEmpty(Manifest, TEXT("gamefeatures"));
	PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("get_status"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
	FString PluginName;
	GameFeatureSection->TryGetStringField(TEXT("plugin_name"), PluginName);
	FString GameFeatureDataPath;
	GameFeatureSection->TryGetStringField(TEXT("asset_path"), GameFeatureDataPath);
	if (!PluginName.IsEmpty() || !GameFeatureDataPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> GameFeatureParams = MakeShared<FJsonObject>();
		if (!PluginName.IsEmpty())
		{
			GameFeatureParams->SetStringField(TEXT("plugin_name"), PluginName);
		}
		if (!GameFeatureDataPath.IsEmpty())
		{
			GameFeatureParams->SetStringField(TEXT("asset_path"), GameFeatureDataPath);
		}
		PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("find_game_feature_data"), GameFeatureParams, bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
		PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("describe_game_feature_data"), GameFeatureParams, bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
		if (!PluginName.IsEmpty())
		{
			PlanOrExecutePrimitive(TEXT("gamefeatures"), TEXT("validate_plugin"), MakeActionParams(TEXT("plugin_name"), PluginName), bExecuteReadOnly, false, Actions, GameFeatureRows, Warnings);
		}
	}
	GameFeaturesValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"));
	GameFeaturesValidation->SetArrayField(TEXT("read_back"), GameFeatureRows);

	TArray<TSharedPtr<FJsonValue>> WorldConditionRows;
	TSharedPtr<FJsonObject> WorldConditionSection = GetObjectFieldOrEmpty(Manifest, TEXT("world_conditions"));
	PlanOrExecutePrimitive(TEXT("world_conditions"), TEXT("get_status"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, WorldConditionRows, Warnings);
	FString WorldConditionAssetPath;
	WorldConditionSection->TryGetStringField(TEXT("asset_path"), WorldConditionAssetPath);
	if (!WorldConditionAssetPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> WorldConditionParams = MakeActionParams(TEXT("asset_path"), WorldConditionAssetPath);
		FString Query;
		WorldConditionSection->TryGetStringField(TEXT("query"), Query);
		if (!Query.IsEmpty())
		{
			WorldConditionParams->SetStringField(TEXT("query"), Query);
		}
		double SlotIndex = 0.0;
		if (WorldConditionSection->TryGetNumberField(TEXT("slot_index"), SlotIndex))
		{
			WorldConditionParams->SetNumberField(TEXT("slot_index"), SlotIndex);
		}
		PlanOrExecutePrimitive(TEXT("world_conditions"), TEXT("describe_query"), WorldConditionParams, bExecuteReadOnly, false, Actions, WorldConditionRows, Warnings);
	}
	WorldConditionsValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"));
	WorldConditionsValidation->SetArrayField(TEXT("read_back"), WorldConditionRows);

	TSharedPtr<FJsonObject> RuntimeSection = GetObjectFieldOrEmpty(Manifest, TEXT("runtime"));
	TSharedPtr<FJsonObject> Runtime = MakeUnavailableProof(
		bRuntimeProofRequired ? TEXT("blocked") : TEXT("planned"),
		bRuntimeProofRequired
			? TEXT("runtime_proof_required=true, but this first slice does not start PIE or inject input.")
			: TEXT("Runtime proof is declared as a later PIE workflow step."));
	TArray<TSharedPtr<FJsonValue>> RuntimeNext;
	const TSharedPtr<FJsonObject>* TriggerAction = nullptr;
	const TSharedPtr<FJsonObject>* TriggerParams = nullptr;
	TSharedPtr<FJsonObject> MakeRuntimeProofParamsResult = MakeRuntimeProofParams(RuntimeSection);
	if (!MakeRuntimeProofParamsResult.IsValid()
		|| !MakeRuntimeProofParamsResult->TryGetObjectField(TEXT("trigger_action"), TriggerAction)
		|| !TriggerAction
		|| !TriggerAction->IsValid()
		|| !(*TriggerAction)->TryGetObjectField(TEXT("params"), TriggerParams)
		|| !TriggerParams
		|| !TriggerParams->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("Invalid params: trigger_action params are not an object"), FMonolithJsonUtils::ErrInvalidParams);
	}

	RuntimeNext.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("editor.pie_inject_input_action"),
		FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("pie_inject_input_action")),
		true,
		TEXT("Inject the manifest input action during a confirmed PIE proof slice."),
		*TriggerParams)));
	RuntimeNext.Add(MakeShared<FJsonValueObject>(MakeNextAction(
		TEXT("gas.expect_event_cue"),
		FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("expect_event_cue")),
		true,
		TEXT("Observe the expected GameplayEvent and GameplayCue after input injection."),
		MakeRuntimeProofParams(RuntimeSection))));
	Runtime->SetArrayField(TEXT("next_actions"), RuntimeNext);

	Validation->SetObjectField(TEXT("input"), InputValidation);
	Validation->SetObjectField(TEXT("gas"), GasValidation);
	Validation->SetObjectField(TEXT("blueprint"), BlueprintValidation);
	Validation->SetObjectField(TEXT("ai"), AiValidation);
	Validation->SetObjectField(TEXT("gamefeatures"), GameFeaturesValidation);
	Validation->SetObjectField(TEXT("world_conditions"), WorldConditionsValidation);
	Validation->SetObjectField(TEXT("runtime"), Runtime);
	Result->SetObjectField(TEXT("validation"), Validation);

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("input.create_input_action"), FMonolithToolRegistry::Get().HasAction(TEXT("input"), TEXT("create_input_action")), true, TEXT("Author missing Input Action assets in a later confirmed slice."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("input.add_input_mapping"), FMonolithToolRegistry::Get().HasAction(TEXT("input"), TEXT("add_input_mapping")), true, TEXT("Author missing key mappings in a later confirmed slice; triggers/modifiers remain a schema gap."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("gas.bind_ability_to_input"), FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("bind_ability_to_input")), true, TEXT("Bind GAS abilities to input after preflight proof."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("blueprint.compile_blueprint"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("compile_blueprint")), true, TEXT("Compile touched actor/controller Blueprints after any later mutation."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.start_pie"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("start_pie")), true, TEXT("Start a confirmed PIE proof slice."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("gas.expect_event_cue"), FMonolithToolRegistry::Get().HasAction(TEXT("gas"), TEXT("expect_event_cue")), true, TEXT("Runtime proof hook for input->GAS event/cue verification."), MakeRuntimeProofParams(RuntimeSection))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("This first slice performs no mutation, so rollback is unnecessary."),
		TEXT("Future authoring and PIE proof slices must disclose dirty packages and source-control state.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	if (bConfirm)
	{
		Errors.Add(TEXT("confirm=true is reserved for later authoring/runtime slices; workflow.gameplay_feature_manifest is read-only."));
	}
	if (bRuntimeProofRequired)
	{
		Errors.Add(TEXT("runtime_proof_required=true requested; this first slice only declares gas.expect_event_cue + editor.pie_inject_input_action."));
	}

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only gameplay feature preflight completed where actions were available; authoring, compile, PIE, save, and source-control remain follow-up slices."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleUiShippingWidgetBlueprint(const TSharedPtr<FJsonObject>& Params)
{
	FString WidgetAssetPath;
	Params->TryGetStringField(TEXT("widget_asset_path"), WidgetAssetPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludeLayout = true;
	Params->TryGetBoolField(TEXT("include_layout_audit"), bIncludeLayout);
	bool bIncludeAccessibility = true;
	Params->TryGetBoolField(TEXT("include_accessibility_audit"), bIncludeAccessibility);
	bool bIncludeNavigation = true;
	Params->TryGetBoolField(TEXT("include_navigation_audit"), bIncludeNavigation);
	bool bIncludeCommonUI = true;
	Params->TryGetBoolField(TEXT("include_commonui_audit"), bIncludeCommonUI);
	bool bIncludeBindings = true;
	Params->TryGetBoolField(TEXT("include_binding_inventory"), bIncludeBindings);
	bool bTreatWarningsAsErrors = false;
	Params->TryGetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
	bool bPreviewRequired = false;
	Params->TryGetBoolField(TEXT("preview_required"), bPreviewRequired);
	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);
	bool bRunLayoutMeasure = true;
	Params->TryGetBoolField(TEXT("run_layout_measure"), bRunLayoutMeasure);

	FString ProofProfile = TEXT("minimal");
	Params->TryGetStringField(TEXT("proof_profile"), ProofProfile);
	ProofProfile = ProofProfile.ToLower();
	if (ProofProfile.IsEmpty())
	{
		ProofProfile = TEXT("minimal");
	}
	if (ProofProfile != TEXT("minimal") && ProofProfile != TEXT("visual") && ProofProfile != TEXT("runtime"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid proof_profile '%s'. Expected one of: minimal, visual, runtime."), *ProofProfile),
			FMonolithJsonUtils::ErrInvalidParams);
	}
	const bool bVisualProofRequested = ProofProfile == TEXT("visual") || ProofProfile == TEXT("runtime");
	const bool bRuntimeProofRequested = ProofProfile == TEXT("runtime");
	const bool bShouldMeasureLayout = bVisualProofRequested && bRunLayoutMeasure;

	FString LayoutRuleProfile = bRuntimeProofRequested ? TEXT("strict") : TEXT("shipping");
	Params->TryGetStringField(TEXT("layout_rule_profile"), LayoutRuleProfile);
	LayoutRuleProfile = LayoutRuleProfile.ToLower();
	if (LayoutRuleProfile.IsEmpty())
	{
		LayoutRuleProfile = bRuntimeProofRequested ? TEXT("strict") : TEXT("shipping");
	}
	if (LayoutRuleProfile != TEXT("advisory") && LayoutRuleProfile != TEXT("shipping") && LayoutRuleProfile != TEXT("strict"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid layout_rule_profile '%s'. Expected one of: advisory, shipping, strict."), *LayoutRuleProfile),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	FString RoundTripCheck = TEXT("auto");
	Params->TryGetStringField(TEXT("round_trip_check"), RoundTripCheck);
	RoundTripCheck = RoundTripCheck.ToLower();
	if (RoundTripCheck.IsEmpty())
	{
		RoundTripCheck = TEXT("auto");
	}
	if (RoundTripCheck != TEXT("auto") && RoundTripCheck != TEXT("force") && RoundTripCheck != TEXT("off"))
	{
		return FMonolithActionResult::Error(
			FString::Printf(TEXT("Invalid round_trip_check '%s'. Expected one of: auto, force, off."), *RoundTripCheck),
			FMonolithJsonUtils::ErrInvalidParams);
	}

	const bool bExecuteReadOnly = !bDryRun && bRunChecks;
	const bool bExecuteVisualProof = bVisualProofRequested && bExecuteReadOnly;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;
	const TArray<TSharedPtr<FJsonValue>>* VisualProfiles = nullptr;
	Params->TryGetArrayField(TEXT("visual_profiles"), VisualProfiles);
	TSharedPtr<FJsonValue> PreviewResolution = Params->TryGetField(TEXT("preview_resolution"));

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("widget_asset_path"), WidgetAssetPath);
	Input->SetStringField(TEXT("proof_profile"), ProofProfile);
	Input->SetStringField(TEXT("layout_rule_profile"), LayoutRuleProfile);
	Input->SetStringField(TEXT("round_trip_check"), RoundTripCheck);
	Input->SetBoolField(TEXT("run_layout_measure"), bRunLayoutMeasure);
	TSharedPtr<FJsonValue> SuppressLayoutRules = Params->TryGetField(TEXT("suppress_layout_rule_ids"));
	if (SuppressLayoutRules.IsValid())
	{
		Input->SetField(TEXT("suppress_layout_rule_ids"), SuppressLayoutRules);
	}
	const TSharedPtr<FJsonObject>* BindingExpectations = nullptr;
	if (Params->TryGetObjectField(TEXT("binding_expectations"), BindingExpectations) && BindingExpectations && BindingExpectations->IsValid())
	{
		Input->SetObjectField(TEXT("binding_expectations"), *BindingExpectations);
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("ui_shipping"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("widget_blueprint_readiness_proof_v1"));
	Result->SetStringField(TEXT("proof_profile"), ProofProfile);
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeUiPlanObject(bDryRun));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { WidgetAssetPath }, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{ WidgetAssetPath },
		{ TEXT("UI shipping first slice is read-only; use explicit source_control.checkout_or_add and asset.save_asset follow-ups.") }));

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Accessibility = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UiValidation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Compile = MakeShared<FJsonObject>();
	if (bVisualProofRequested)
	{
		TArray<TSharedPtr<FJsonValue>> CompileRows;
		PlanOrExecutePrimitive(
			TEXT("ui"),
			TEXT("dump_blueprint_compile_log"),
			MakeActionParams(TEXT("asset_path"), WidgetAssetPath),
			bExecuteVisualProof,
			true,
			Actions,
			CompileRows,
			Errors);
		Compile->SetStringField(TEXT("status"), bExecuteVisualProof ? TEXT("checked") : TEXT("planned"));
		Compile->SetArrayField(TEXT("read_back"), CompileRows);
	}
	else
	{
		Compile = MakeUnavailableProof(TEXT("blocked"), TEXT("Fresh compile is declared as ui.dump_blueprint_compile_log next action and is not run by the minimal proof profile."));
		Compile->SetStringField(TEXT("next_action"), TEXT("ui.dump_blueprint_compile_log"));
	}

	TArray<TSharedPtr<FJsonValue>> WidgetRows;
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("get_widget_tree"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteReadOnly, true, Actions, WidgetRows, Errors);
	TSharedPtr<FJsonObject> DumpSpecParams = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
	DumpSpecParams->SetBoolField(TEXT("emit_defaults"), false);
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("dump_ui_spec"), DumpSpecParams, bExecuteReadOnly, true, Actions, WidgetRows, Errors);
	if (bIncludeBindings)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("get_widget_bindings"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteReadOnly, true, Actions, WidgetRows, Errors);
	}
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), WidgetRows);

	TArray<TSharedPtr<FJsonValue>> AccessibilityRows;
	if (bIncludeLayout)
	{
		TSharedPtr<FJsonObject> LayoutParams = MakeStringArrayParams(TEXT("asset_paths"), { WidgetAssetPath });
		LayoutParams->SetBoolField(TEXT("include_tests"), false);
		LayoutParams->SetBoolField(TEXT("treat_warnings_as_errors"), bTreatWarningsAsErrors);
		LayoutParams->SetStringField(TEXT("rule_profile"), LayoutRuleProfile);
		TSharedPtr<FJsonValue> SuppressRuleIds = Params->TryGetField(TEXT("suppress_layout_rule_ids"));
		if (SuppressRuleIds.IsValid())
		{
			LayoutParams->SetField(TEXT("suppress_rule_ids"), SuppressRuleIds);
		}
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_widget_layout"), LayoutParams, bExecuteReadOnly, true, Actions, AccessibilityRows, Errors);
	}
	if (bIncludeAccessibility)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_accessibility"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteReadOnly, true, Actions, AccessibilityRows, Errors);
	}
	Accessibility->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	Accessibility->SetArrayField(TEXT("read_back"), AccessibilityRows);

	TArray<TSharedPtr<FJsonValue>> LayoutMeasureRows;
	TSharedPtr<FJsonObject> LayoutMeasureParams = MakeUiMeasureWidgetLayoutParams(WidgetAssetPath, VisualProfiles, PreviewResolution);
	TSharedPtr<FJsonObject> LayoutMeasureProof;
	if (bShouldMeasureLayout)
	{
		PlanOrExecutePrimitive(
			TEXT("ui"),
			TEXT("measure_widget_layout"),
			LayoutMeasureParams,
			bExecuteVisualProof,
			true,
			Actions,
			LayoutMeasureRows,
			Errors);
		LayoutMeasureProof = MakeUnavailableProof(
			bExecuteVisualProof ? TEXT("checked") : TEXT("planned"),
			TEXT("Authored bounds, overlap, and safe-zone evidence is composed through ui.measure_widget_layout."));
		LayoutMeasureProof->SetArrayField(TEXT("read_back"), LayoutMeasureRows);
		LayoutMeasureProof->SetObjectField(TEXT("params"), LayoutMeasureParams);
	}
	else
	{
		LayoutMeasureProof = MakeUnavailableProof(
			bVisualProofRequested ? TEXT("skipped") : TEXT("not_requested"),
			bVisualProofRequested
				? TEXT("run_layout_measure=false.")
				: TEXT("proof_profile=minimal does not require authored layout measurement proof."));
	}

	TArray<TSharedPtr<FJsonValue>> UiRows;
	if (bIncludeNavigation)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("dump_widget_navigation"), MakeActionParams(TEXT("wbp_path"), WidgetAssetPath), bExecuteReadOnly, false, Actions, UiRows, Warnings);
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_focus_chain"), MakeActionParams(TEXT("wbp_path"), WidgetAssetPath), bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}
	if (bIncludeCommonUI)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_commonui_widget"), MakeActionParams(TEXT("wbp_path"), WidgetAssetPath), bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}
	if (BindingExpectations && BindingExpectations->IsValid())
	{
		UiValidation->SetObjectField(TEXT("binding_expectations"), *BindingExpectations);
	}
	UiValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"));
	UiValidation->SetArrayField(TEXT("read_back"), UiRows);

	Validation->SetObjectField(TEXT("compile"), Compile);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	Validation->SetObjectField(TEXT("accessibility"), Accessibility);
	Validation->SetObjectField(TEXT("layout_measure"), LayoutMeasureProof);
	Validation->SetObjectField(TEXT("ui"), UiValidation);
	if (bRuntimeProofRequested)
	{
		TSharedPtr<FJsonObject> Runtime = MakeUnavailableProof(TEXT("blocked"), TEXT("proof_profile=runtime requires an async PIE/CommonUI flow proof; v1 reports concrete Monolith next actions instead of claiming runtime proof."));
		Runtime->SetStringField(TEXT("failure_code"), TEXT("runtime_assertion_failed"));
		const TSharedPtr<FJsonObject>* RuntimeFlow = nullptr;
		if (Params->TryGetObjectField(TEXT("runtime_flow"), RuntimeFlow) && RuntimeFlow && RuntimeFlow->IsValid())
		{
			Runtime->SetObjectField(TEXT("runtime_flow"), *RuntimeFlow);
		}
		Validation->SetObjectField(TEXT("runtime"), Runtime);
		Errors.Add(TEXT("proof_profile=runtime requested; async PIE/CommonUI runtime proof is blocked in this v1 workflow slice."));
	}
	else
	{
		Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("UI shipping proof_profile=minimal/visual does not run PIE/runtime UI interaction proof.")));
	}
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No material/shader budget proof applies to this UI first slice.")));
	TSharedPtr<FJsonObject> VisualProfileProof;
	ValidateUiVisualProfilesForProof(VisualProfiles, bVisualProofRequested, VisualProfileProof, Errors);
	Validation->SetObjectField(TEXT("visual_profile"), VisualProfileProof);
	Result->SetObjectField(TEXT("validation"), Validation);

	TSharedPtr<FJsonObject> PreviewParams = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
	PreviewParams->SetStringField(TEXT("asset_type"), TEXT("widget"));
	double PreviewScale = 1.0;
	Params->TryGetNumberField(TEXT("preview_scale"), PreviewScale);
	FString VisualProfileName = TEXT("desktop");
	if (VisualProfiles && VisualProfiles->Num() > 0)
	{
		const TSharedPtr<FJsonObject>* FirstProfile = nullptr;
		if ((*VisualProfiles)[0].IsValid() && (*VisualProfiles)[0]->TryGetObject(FirstProfile) && FirstProfile && FirstProfile->IsValid())
		{
			FString RequestedProfileName;
			if ((*FirstProfile)->TryGetStringField(TEXT("name"), RequestedProfileName) && !RequestedProfileName.IsEmpty())
			{
				VisualProfileName = RequestedProfileName;
			}
			double DpiScale = 0.0;
			if ((*FirstProfile)->TryGetNumberField(TEXT("dpi_scale"), DpiScale) && DpiScale > 0.0)
			{
				PreviewScale = DpiScale;
			}
			TSharedPtr<FJsonValue> ProfileResolution = (*FirstProfile)->TryGetField(TEXT("resolution"));
			if (ProfileResolution.IsValid())
			{
				PreviewParams->SetField(TEXT("resolution"), ProfileResolution);
			}
		}
	}
	PreviewParams->SetNumberField(TEXT("scale"), PreviewScale);
	FString PreviewOutputPath;
	if (Params->TryGetStringField(TEXT("preview_output_path"), PreviewOutputPath) && !PreviewOutputPath.IsEmpty())
	{
		PreviewParams->SetStringField(TEXT("output_path"), PreviewOutputPath);
	}
	if (PreviewResolution.IsValid())
	{
		PreviewParams->SetField(TEXT("resolution"), PreviewResolution);
	}
	if (bVisualProofRequested && PreviewOutputPath.IsEmpty())
	{
		FString RunId;
		Params->TryGetStringField(TEXT("run_id"), RunId);
		if (RunId.IsEmpty())
		{
			Params->TryGetStringField(TEXT("request_id"), RunId);
		}
		if (RunId.IsEmpty())
		{
			RunId = TEXT("ui_shipping_visual");
		}
		FString OutputDir;
		Params->TryGetStringField(TEXT("output_dir"), OutputDir);
		if (OutputDir.IsEmpty())
		{
			OutputDir = FPaths::ProjectSavedDir() / TEXT("Monolith/UIEvidence") / RunId;
		}
		FPaths::NormalizeFilename(OutputDir);
		const FString SafeProfileName = FPaths::MakeValidFileName(VisualProfileName.IsEmpty() ? TEXT("desktop") : VisualProfileName);
		PreviewOutputPath = FPaths::Combine(OutputDir, SafeProfileName + TEXT(".png"));
		PreviewParams->SetStringField(TEXT("output_path"), PreviewOutputPath);
	}

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	TSharedPtr<FJsonObject> VisualVerifyParams;
	if (bVisualProofRequested)
	{
		TArray<TSharedPtr<FJsonValue>> CaptureRows;
		const bool bCaptureOk = PlanOrExecutePrimitive(
			TEXT("editor"),
			TEXT("capture_scene_preview"),
			PreviewParams,
			bExecuteVisualProof,
			true,
			Actions,
			CaptureRows,
			Errors);
		PreviewArtifacts.Append(CaptureRows);

		TSharedPtr<FJsonObject> VerifyParams = MakeActionParams(TEXT("asset_path"), WidgetAssetPath);
		FString RequestId;
		if (Params->TryGetStringField(TEXT("request_id"), RequestId) && !RequestId.IsEmpty())
		{
			VerifyParams->SetStringField(TEXT("request_id"), RequestId);
		}
		FString RunId;
		if (Params->TryGetStringField(TEXT("run_id"), RunId) && !RunId.IsEmpty())
		{
			VerifyParams->SetStringField(TEXT("run_id"), RunId);
		}
		FString OutputDir;
		if (Params->TryGetStringField(TEXT("output_dir"), OutputDir) && !OutputDir.IsEmpty())
		{
			VerifyParams->SetStringField(TEXT("output_dir"), OutputDir);
		}
		VerifyParams->SetBoolField(TEXT("fail_on_blank"), true);

		TSharedPtr<FJsonObject> CaptureSpec = MakeShared<FJsonObject>();
		CaptureSpec->SetStringField(TEXT("profile"), VisualProfileName);
		CaptureSpec->SetStringField(TEXT("path"), PreviewOutputPath);
		TSharedPtr<FJsonValue> VerifyResolution = PreviewParams->TryGetField(TEXT("resolution"));
		if (VerifyResolution.IsValid())
		{
			CaptureSpec->SetField(TEXT("expected_resolution"), VerifyResolution);
		}
		TArray<TSharedPtr<FJsonValue>> VerifyCaptures;
		VerifyCaptures.Add(MakeShared<FJsonValueObject>(CaptureSpec));
		VerifyParams->SetArrayField(TEXT("captures"), VerifyCaptures);
		VisualVerifyParams = VerifyParams;

		TArray<TSharedPtr<FJsonValue>> VerifyRows;
		if (!bExecuteVisualProof || bCaptureOk)
		{
			PlanOrExecutePrimitive(
				TEXT("ui"),
				TEXT("verify_widget_visual_artifacts"),
				VerifyParams,
				bExecuteVisualProof && bCaptureOk,
				true,
				Actions,
				VerifyRows,
				Errors);
			PreviewArtifacts.Append(VerifyRows);
		}
	}
	else if (bPreviewRequired)
	{
		TSharedPtr<FJsonObject> PreviewBlocker = MakeUnavailableProof(TEXT("blocked"), TEXT("Preview capture is not run by this first slice; call editor.capture_scene_preview explicitly."));
		PreviewBlocker->SetStringField(TEXT("next_action"), TEXT("editor.capture_scene_preview"));
		PreviewBlocker->SetObjectField(TEXT("params"), PreviewParams);
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(PreviewBlocker));
		Warnings.Add(TEXT("preview_required=true but this first slice only reports the preview blocker and next action."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("layout_measure"), LayoutMeasureRows);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	TSharedPtr<FJsonObject> UiEvidence = MakeShared<FJsonObject>();
	UiEvidence->SetStringField(TEXT("schema_version"), TEXT("ui_workflow_proof.v1"));
	UiEvidence->SetStringField(TEXT("proof_profile"), ProofProfile);
	UiEvidence->SetStringField(TEXT("round_trip_check"), RoundTripCheck);
	UiEvidence->SetStringField(TEXT("layout_rule_profile"), LayoutRuleProfile);
	if (VisualProfileProof.IsValid())
	{
		FString VisualProfileStatus;
		if (VisualProfileProof->TryGetStringField(TEXT("status"), VisualProfileStatus))
		{
			UiEvidence->SetStringField(TEXT("visual_profile_status"), VisualProfileStatus);
		}
		else
		{
			return FMonolithActionResult::Error(TEXT("Invalid params: visual profile proof status is not a string"), FMonolithJsonUtils::ErrInvalidParams);
		}
	}
	UiEvidence->SetStringField(TEXT("visual_artifacts_status"),
		bVisualProofRequested ? (bExecuteVisualProof ? TEXT("checked") : TEXT("planned")) : TEXT("not_requested"));
	UiEvidence->SetStringField(TEXT("layout_measure_status"),
		bShouldMeasureLayout ? (bExecuteVisualProof ? TEXT("checked") : TEXT("planned")) : (bVisualProofRequested ? TEXT("skipped") : TEXT("not_requested")));
	UiEvidence->SetBoolField(TEXT("visual_artifacts_required"), bVisualProofRequested);
	UiEvidence->SetBoolField(TEXT("runtime_required"), bRuntimeProofRequested);
	TArray<FString> UiEvidenceLimitations;
	UiEvidenceLimitations.Add(TEXT("round_trip_check=auto runs only for spec-authored or clearly FUISpecDocument-representable changes; this workflow records the policy but does not infer representability from arbitrary WBP edits."));
	if (bRuntimeProofRequested)
	{
		UiEvidenceLimitations.Add(TEXT("proof_profile=runtime is blocked until an async PIE/CommonUI proof chain is supplied and executed."));
	}
	UiEvidence->SetArrayField(TEXT("limitations"), StringsToJson(UiEvidenceLimitations));
	Proof->SetObjectField(TEXT("ui_evidence"), UiEvidence);
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.ui_shipping_widget_blueprint is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	const bool bVisualProofAlreadyAttempted = bVisualProofRequested && bExecuteVisualProof;
	const bool bKeepVisualProofFollowUps = !bVisualProofAlreadyAttempted || Errors.Num() > 0;
	if (!bVisualProofRequested || bKeepVisualProofFollowUps)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.dump_blueprint_compile_log"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")), true, TEXT("Run a fresh compile/read-back proof explicitly."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.capture_scene_preview"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("capture_scene_preview")), true, TEXT("Produce a widget preview artifact with explicit output parameters."), PreviewParams)));
	}
	if (bShouldMeasureLayout && bKeepVisualProofFollowUps)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.measure_widget_layout"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("measure_widget_layout")), true, TEXT("Measure authored widget bounds, overlap, and safe-zone evidence for the visual proof profile."), LayoutMeasureParams)));
	}
	if (bVisualProofRequested && bKeepVisualProofFollowUps && VisualVerifyParams.IsValid())
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.verify_widget_visual_artifacts"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("verify_widget_visual_artifacts")), true, TEXT("Validate the widget PNG artifact before treating the visual proof as evidence."), VisualVerifyParams)));
	}
	if (bRuntimeProofRequested)
	{
		const TSharedPtr<FJsonObject>* RuntimeFlow = nullptr;
		TSharedPtr<FJsonObject> RuntimeFlowParams = MakeShared<FJsonObject>();
		if (Params->TryGetObjectField(TEXT("runtime_flow"), RuntimeFlow) && RuntimeFlow && RuntimeFlow->IsValid())
		{
			CopyJsonFields(*RuntimeFlow, RuntimeFlowParams);
		}
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.validate_frontend_menu_flow"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("validate_frontend_menu_flow")), true, TEXT("Validate the declared CommonUI/frontend contract before starting PIE."), RuntimeFlowParams)));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.list_errored_blueprints"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("list_errored_blueprints")), true, TEXT("Run the exact PIE compile-error preflight gate before runtime UI proof."))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.run_pie_smoke"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("run_pie_smoke")), true, TEXT("Start the async PIE proof only after frontend validation and compile preflight pass."), RuntimeFlowParams)));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.poll_pie_smoke"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("poll_pie_smoke")), true, TEXT("Poll the async PIE proof using the session_id returned by editor.run_pie_smoke."))));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.capture_pie_movement_clip"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("capture_pie_movement_clip")), true, TEXT("Capture runtime frames only when interaction proof requires viewport evidence."), RuntimeFlowParams)));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.pie_inject_input_action"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("pie_inject_input_action")), true, TEXT("Inject the declared Enhanced Input action during the async PIE session when the runtime flow needs interaction."), RuntimeFlowParams)));
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.stop_pie_smoke"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("stop_pie_smoke")), true, TEXT("Stop the async PIE proof session if polling does not reach a terminal state."))));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist the reviewed Widget Blueprint explicitly."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the Widget Blueprint package path through source control."), MakeStringArrayParams(TEXT("paths"), { WidgetAssetPath }))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("This first slice performs no mutation, so rollback is unnecessary."),
		TEXT("Future UI authoring, compile, preview, save, and source-control slices must disclose dirty packages.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	FString Status = TEXT("partial");
	if (Errors.Num() > 0)
	{
		Status = TEXT("blocked");
	}
	else if (bDryRun || !bRunChecks)
	{
		Status = TEXT("planned");
	}
	else if (bVisualProofRequested)
	{
		Status = TEXT("pass");
	}
	Result->SetStringField(TEXT("status"), Status);
	if (!bDryRun && Errors.Num() == 0)
	{
		if (!bRunChecks)
		{
			Warnings.Add(TEXT("run_read_only_checks=false returned the proof plan without executing read-only checks."));
		}
		else if (bVisualProofRequested)
		{
			Warnings.Add(TEXT("UI visual proof completed where actions were available; save and source-control remain explicit follow-ups."));
		}
		else
		{
			Warnings.Add(TEXT("Read-only UI proof completed where actions were available; compile, preview, save, and source-control remain explicit follow-ups."));
		}
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleUiBindWidgetEvent(const TSharedPtr<FJsonObject>& Params)
{
	FString AssetPath;
	Params->TryGetStringField(TEXT("asset_path"), AssetPath);
	FString WidgetName;
	Params->TryGetStringField(TEXT("widget_name"), WidgetName);
	FString EventName;
	Params->TryGetStringField(TEXT("event"), EventName);
	FString GraphName = TEXT("EventGraph");
	Params->TryGetStringField(TEXT("graph_name"), GraphName);
	if (GraphName.IsEmpty())
	{
		GraphName = TEXT("EventGraph");
	}

	const TSharedPtr<FJsonObject>* IntentPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("intent"), IntentPtr) || !IntentPtr || !IntentPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("intent must be an object."), FMonolithJsonUtils::ErrInvalidParams);
	}
	const TSharedPtr<FJsonObject> Intent = *IntentPtr;

	FString IntentKind = TEXT("viewmodel_command");
	Intent->TryGetStringField(TEXT("kind"), IntentKind);
	if (!IntentKind.Equals(TEXT("viewmodel_command"), ESearchCase::IgnoreCase))
	{
		TSharedPtr<FJsonObject> ErrorData = MakeUiBindBoundaryProof(
			TEXT("rejected"),
			TEXT("workflow.ui_bind_widget_event v1 only accepts intent.kind='viewmodel_command'."));
		ErrorData->SetStringField(TEXT("rejected_intent_kind"), IntentKind);
		return FMonolithActionResult::Error(
			TEXT("UI event binding rejected: runtime UI must route user intent through a ViewModel command, not direct gameplay or arbitrary Blueprint calls."),
			FMonolithJsonUtils::ErrInvalidParams)
			.WithErrorData(ErrorData)
			.WithRelatedActions({ TEXT("blueprint.add_node"), TEXT("blueprint.connect_pins"), TEXT("ui.get_widget_bindings") });
	}

	const TArray<FString> DirectGameplayFields = {
		TEXT("actor"),
		TEXT("actor_path"),
		TEXT("pawn"),
		TEXT("pawn_path"),
		TEXT("controller"),
		TEXT("controller_path"),
		TEXT("component"),
		TEXT("component_path"),
		TEXT("gameplay_target")
	};
	for (const FString& Field : DirectGameplayFields)
	{
		if (Intent->HasField(Field))
		{
			TSharedPtr<FJsonObject> ErrorData = MakeUiBindBoundaryProof(
				TEXT("rejected"),
				TEXT("Direct gameplay targets are not accepted by runtime UI event workflows."));
			ErrorData->SetStringField(TEXT("rejected_field"), Field);
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("UI event binding rejected: intent.%s would bypass the ViewModel boundary."), *Field),
				FMonolithJsonUtils::ErrInvalidParams)
				.WithErrorData(ErrorData)
				.WithRelatedActions({ TEXT("blueprint.add_node"), TEXT("ui.get_widget_bindings") });
		}
	}

	FString ViewModelVariable;
	Intent->TryGetStringField(TEXT("viewmodel_variable"), ViewModelVariable);
	FString Command;
	Intent->TryGetStringField(TEXT("command"), Command);
	if (ViewModelVariable.IsEmpty() || Command.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("intent.viewmodel_variable and intent.command are required for viewmodel_command."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString ViewModelClass;
	Intent->TryGetStringField(TEXT("viewmodel_class"), ViewModelClass);
	if (ViewModelClass.IsEmpty())
	{
		Intent->TryGetStringField(TEXT("target_class"), ViewModelClass);
	}

	FString EventExecPinOverride;
	Intent->TryGetStringField(TEXT("event_exec_pin"), EventExecPinOverride);
	FString CommandExecPinOverride;
	Intent->TryGetStringField(TEXT("command_exec_pin"), CommandExecPinOverride);
	FString ViewModelValuePinOverride;
	Intent->TryGetStringField(TEXT("viewmodel_value_pin"), ViewModelValuePinOverride);
	FString CommandTargetPinOverride;
	Intent->TryGetStringField(TEXT("command_target_pin"), CommandTargetPinOverride);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bRunReadBack = true;
	Params->TryGetBoolField(TEXT("run_read_back"), bRunReadBack);
	const bool bExecuteMutation = !bDryRun && bConfirm;

	const FString DelegateName = NormalizeUiWidgetDelegateName(EventName);
	TSharedPtr<FJsonObject> EventParams = MakeUiBindEventNodeParams(AssetPath, GraphName, WidgetName, DelegateName);
	TSharedPtr<FJsonObject> ViewModelParams = MakeUiBindVariableGetParams(AssetPath, GraphName, ViewModelVariable);
	TSharedPtr<FJsonObject> CommandParams = MakeUiBindCommandCallParams(AssetPath, GraphName, Command, ViewModelClass);
	TSharedPtr<FJsonObject> EventResolveParams = MakeUiBindResolveNodeParams(EventParams);
	TSharedPtr<FJsonObject> ViewModelResolveParams = MakeUiBindResolveNodeParams(ViewModelParams);
	TSharedPtr<FJsonObject> CommandResolveParams = MakeUiBindResolveNodeParams(CommandParams);

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBackRows;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("asset_path"), AssetPath);
	Input->SetStringField(TEXT("widget_name"), WidgetName);
	Input->SetStringField(TEXT("event"), EventName);
	Input->SetStringField(TEXT("delegate_property_name"), DelegateName);
	Input->SetStringField(TEXT("graph_name"), GraphName);
	Input->SetObjectField(TEXT("intent"), Intent);
	Input->SetBoolField(TEXT("compile"), bCompile);
	Input->SetBoolField(TEXT("run_read_back"), bRunReadBack);

	if (!bDryRun && !bConfirm)
	{
		Errors.Add(TEXT("dry_run=false requires confirm=true before Blueprint graph mutation."));
	}

	const bool bExecuteReadBack = bExecuteMutation && bRunReadBack;
	PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("resolve_node"), EventResolveParams, bExecuteReadBack, true, Actions, ReadBackRows, Errors);
	PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("resolve_node"), ViewModelResolveParams, bExecuteReadBack, true, Actions, ReadBackRows, Errors);
	PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("resolve_node"), CommandResolveParams, bExecuteReadBack, true, Actions, ReadBackRows, Errors);

	TSharedPtr<FJsonObject> EventBindingProof = MakeUnavailableProof(
		bExecuteMutation ? TEXT("attempted") : TEXT("planned"),
		TEXT("Component-bound event, ViewModel getter, command call, and pin wiring are composed from existing Blueprint owner actions."));
	EventBindingProof->SetStringField(TEXT("schema_version"), TEXT("ui_event_binding_proof.v1"));
	EventBindingProof->SetStringField(TEXT("delegate_property_name"), DelegateName);
	EventBindingProof->SetStringField(TEXT("viewmodel_variable"), ViewModelVariable);
	EventBindingProof->SetStringField(TEXT("command"), Command);

	FString EventNodeId;
	FString ViewModelNodeId;
	FString CommandNodeId;
	if (!bExecuteMutation)
	{
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("add_node"), EventParams, false, true, Actions, ReadBackRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("add_node"), ViewModelParams, false, true, Actions, ReadBackRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("add_node"), CommandParams, false, true, Actions, ReadBackRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("connect_pins"), MakeUiBindConnectionParams(AssetPath, GraphName, TEXT("${event_node.id}"), EventExecPinOverride.IsEmpty() ? TEXT("then") : EventExecPinOverride, TEXT("${command_node.id}"), CommandExecPinOverride.IsEmpty() ? TEXT("execute") : CommandExecPinOverride), false, true, Actions, ReadBackRows, Errors);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("connect_pins"), MakeUiBindConnectionParams(AssetPath, GraphName, TEXT("${viewmodel_node.id}"), ViewModelValuePinOverride.IsEmpty() ? ViewModelVariable : ViewModelValuePinOverride, TEXT("${command_node.id}"), CommandTargetPinOverride.IsEmpty() ? TEXT("self") : CommandTargetPinOverride), false, true, Actions, ReadBackRows, Errors);
	}
	else
	{
		const FMonolithActionResult EventResult = ExecutePrimitiveAndRecord(TEXT("blueprint"), TEXT("add_node"), EventParams, Actions, ReadBackRows, Errors);
		const FMonolithActionResult ViewModelResult = ExecutePrimitiveAndRecord(TEXT("blueprint"), TEXT("add_node"), ViewModelParams, Actions, ReadBackRows, Errors);
		const FMonolithActionResult CommandResult = ExecutePrimitiveAndRecord(TEXT("blueprint"), TEXT("add_node"), CommandParams, Actions, ReadBackRows, Errors);

		const bool bHaveNodes = EventResult.bSuccess
			&& ViewModelResult.bSuccess
			&& CommandResult.bSuccess
			&& TryGetWorkflowNodeId(EventResult.Result, EventNodeId)
			&& TryGetWorkflowNodeId(ViewModelResult.Result, ViewModelNodeId)
			&& TryGetWorkflowNodeId(CommandResult.Result, CommandNodeId);

		if (bHaveNodes)
		{
			FString EventExecPin = EventExecPinOverride;
			if (EventExecPin.IsEmpty())
			{
				FindPinNameInNode(EventResult.Result, TEXT("output"), true, { TEXT("then"), TEXT("execute"), TEXT("Then") }, EventExecPin);
			}
			FString CommandExecPin = CommandExecPinOverride;
			if (CommandExecPin.IsEmpty())
			{
				FindPinNameInNode(CommandResult.Result, TEXT("input"), true, { TEXT("execute"), TEXT("Exec"), TEXT("then") }, CommandExecPin);
			}
			FString ViewModelValuePin = ViewModelValuePinOverride;
			if (ViewModelValuePin.IsEmpty())
			{
				FindPinNameInNode(ViewModelResult.Result, TEXT("output"), false, { ViewModelVariable, TEXT("ReturnValue") }, ViewModelValuePin);
			}
			FString CommandTargetPin = CommandTargetPinOverride;
			if (CommandTargetPin.IsEmpty())
			{
				FindPinNameInNode(CommandResult.Result, TEXT("input"), false, { TEXT("self"), TEXT("Target"), TEXT("Object") }, CommandTargetPin);
			}

			if (EventExecPin.IsEmpty() || CommandExecPin.IsEmpty() || ViewModelValuePin.IsEmpty() || CommandTargetPin.IsEmpty())
			{
				Errors.Add(TEXT("Could not infer all required pins for event exec and ViewModel target wiring. Retry with intent.event_exec_pin, command_exec_pin, viewmodel_value_pin, and command_target_pin overrides."));
			}
			else
			{
				ExecutePrimitiveAndRecord(TEXT("blueprint"), TEXT("connect_pins"),
					MakeUiBindConnectionParams(AssetPath, GraphName, EventNodeId, EventExecPin, CommandNodeId, CommandExecPin),
					Actions, ReadBackRows, Errors);
				ExecutePrimitiveAndRecord(TEXT("blueprint"), TEXT("connect_pins"),
					MakeUiBindConnectionParams(AssetPath, GraphName, ViewModelNodeId, ViewModelValuePin, CommandNodeId, CommandTargetPin),
					Actions, ReadBackRows, Errors);

				EventBindingProof->SetStringField(TEXT("event_node_id"), EventNodeId);
				EventBindingProof->SetStringField(TEXT("viewmodel_node_id"), ViewModelNodeId);
				EventBindingProof->SetStringField(TEXT("command_node_id"), CommandNodeId);
				EventBindingProof->SetStringField(TEXT("event_exec_pin"), EventExecPin);
				EventBindingProof->SetStringField(TEXT("command_exec_pin"), CommandExecPin);
				EventBindingProof->SetStringField(TEXT("viewmodel_value_pin"), ViewModelValuePin);
				EventBindingProof->SetStringField(TEXT("command_target_pin"), CommandTargetPin);
			}
		}
	}

	if (bCompile)
	{
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("compile_blueprint"), MakeActionParams(TEXT("asset_path"), AssetPath), bExecuteMutation, true, Actions, ReadBackRows, Errors);
	}
	if (bRunReadBack)
	{
		TSharedPtr<FJsonObject> GraphSummaryParams = MakeActionParams(TEXT("asset_path"), AssetPath);
		GraphSummaryParams->SetStringField(TEXT("graph_name"), GraphName);
		PlanOrExecutePrimitive(TEXT("blueprint"), TEXT("get_graph_summary"), GraphSummaryParams, bExecuteMutation, true, Actions, ReadBackRows, Errors);
	}

	if (bExecuteMutation && Errors.Num() == 0)
	{
		EventBindingProof->SetStringField(TEXT("status"), TEXT("pass"));
		EventBindingProof->SetStringField(TEXT("reason"), TEXT("Blueprint event binding child actions executed and proof rows were recorded."));
	}

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("ui_event_binding"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("viewmodel_command_event_binding_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeUiBindEventPlanObject(bDryRun, bConfirm, bCompile));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { AssetPath }, { AssetPath }, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetObjectField(TEXT("boundary"), MakeUiBindBoundaryProof(TEXT("pass"), TEXT("Intent routes through ViewModel command.")));
	Validation->SetObjectField(TEXT("event_binding"), EventBindingProof);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(bCompile ? (bExecuteMutation ? TEXT("attempted") : TEXT("planned")) : TEXT("not_requested"), bCompile ? TEXT("See proof.read_back for blueprint.compile_blueprint row.") : TEXT("compile=false.")));
	Validation->SetObjectField(TEXT("read_back"), MakeUnavailableProof(bRunReadBack ? (bExecuteMutation ? TEXT("attempted") : TEXT("planned")) : TEXT("not_requested"), bRunReadBack ? TEXT("See proof.read_back for resolve_node/get_graph_summary rows.") : TEXT("run_read_back=false.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBackRows);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Proof->SetStringField(TEXT("runtime_note"), TEXT("This workflow proves graph authoring and compile/read-back. Runtime interaction proof remains a separate PIE workflow."));
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("blueprint.get_graph_summary"), FMonolithToolRegistry::Get().HasAction(TEXT("blueprint"), TEXT("get_graph_summary")), true, TEXT("Read back the edited event graph."), MakeActionParams(TEXT("asset_path"), AssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.dump_blueprint_compile_log"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")), true, TEXT("Compile/read Widget Blueprint errors and warnings through the UI owner action."), MakeActionParams(TEXT("asset_path"), AssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("workflow.ui_shipping_widget_blueprint"), FMonolithToolRegistry::Get().HasAction(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint")), true, TEXT("Run the UI shipping proof after binding the interaction."), MakeActionParams(TEXT("widget_asset_path"), AssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist the reviewed Widget Blueprint explicitly."), MakeActionParams(TEXT("asset_path"), AssetPath))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("Rollback is source-control/editor-undo based; the workflow records touched package scope but does not auto-delete graph nodes."),
		TEXT("If pin inference fails after node creation, inspect proof.read_back rows and revert the package or delete the created nodes explicitly.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("pass")));
	if (bDryRun)
	{
		Warnings.Add(TEXT("dry_run=true returned the Blueprint graph plan only; no graph nodes were created."));
	}
	else if (!bConfirm)
	{
		Warnings.Add(TEXT("confirm=true is required before the workflow can mutate a Widget Blueprint event graph."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleUiMaterialHlslEffect(const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	FString Hlsl;
	Params->TryGetStringField(TEXT("hlsl"), Hlsl);
	if (MaterialPath.IsEmpty() || Hlsl.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("material_path and hlsl are required."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TSharedPtr<FJsonObject>* BindToPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("bind_to"), BindToPtr) || !BindToPtr || !BindToPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("bind_to must be an object with asset_path and widget_name."), FMonolithJsonUtils::ErrInvalidParams);
	}
	const TSharedPtr<FJsonObject> BindTo = *BindToPtr;

	FString WidgetAssetPath;
	BindTo->TryGetStringField(TEXT("asset_path"), WidgetAssetPath);
	if (WidgetAssetPath.IsEmpty())
	{
		BindTo->TryGetStringField(TEXT("widget_asset_path"), WidgetAssetPath);
	}
	if (WidgetAssetPath.IsEmpty())
	{
		BindTo->TryGetStringField(TEXT("wbp_path"), WidgetAssetPath);
	}
	FString WidgetName;
	BindTo->TryGetStringField(TEXT("widget_name"), WidgetName);
	if (WidgetName.IsEmpty())
	{
		BindTo->TryGetStringField(TEXT("image_widget_name"), WidgetName);
	}
	if (WidgetAssetPath.IsEmpty() || WidgetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("bind_to.asset_path and bind_to.widget_name are required."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString PropertyName = TEXT("Brush");
	BindTo->TryGetStringField(TEXT("property_name"), PropertyName);
	FString BindingAction;
	BindTo->TryGetStringField(TEXT("binding_action"), BindingAction);
	if (BindingAction.IsEmpty())
	{
		BindingAction = PropertyName.Equals(TEXT("Brush"), ESearchCase::IgnoreCase) ? TEXT("set_image") : TEXT("set_brush");
	}
	if (!BindingAction.Equals(TEXT("set_image"), ESearchCase::IgnoreCase)
		&& !BindingAction.Equals(TEXT("set_brush"), ESearchCase::IgnoreCase))
	{
		return FMonolithActionResult::Error(TEXT("bind_to.binding_action must be set_image or set_brush."), FMonolithJsonUtils::ErrInvalidParams);
	}

	TArray<TSharedPtr<FJsonValue>> Parameters;
	const TArray<TSharedPtr<FJsonValue>>* ParametersPtr = nullptr;
	if (Params->TryGetArrayField(TEXT("parameters"), ParametersPtr) && ParametersPtr)
	{
		Parameters = *ParametersPtr;
	}
	const TArray<TSharedPtr<FJsonValue>> CustomInputs = MakeCustomNodeInputsFromParameters(Parameters);
	const TSharedPtr<FJsonObject> HlslProof = AnalyzeUiHlsl(Hlsl, Parameters);

	FString ExpressionName;
	Params->TryGetStringField(TEXT("expression_name"), ExpressionName);
	FString OutputType = TEXT("Float4");
	Params->TryGetStringField(TEXT("output_type"), OutputType);
	if (OutputType.IsEmpty())
	{
		OutputType = TEXT("Float4");
	}
	FString BlendMode = TEXT("Translucent");
	Params->TryGetStringField(TEXT("blend_mode"), BlendMode);
	if (BlendMode.IsEmpty())
	{
		BlendMode = TEXT("Translucent");
	}
	FString ShadingModel = TEXT("Unlit");
	Params->TryGetStringField(TEXT("shading_model"), ShadingModel);
	if (ShadingModel.IsEmpty())
	{
		ShadingModel = TEXT("Unlit");
	}
	FString ConnectOutputPin;
	Params->TryGetStringField(TEXT("connect_output_pin"), ConnectOutputPin);
	const bool bOpacityOutputPinExplicit = Params->HasField(TEXT("opacity_output_pin"));
	FString OpacityOutputPin = TEXT("Alpha");
	Params->TryGetStringField(TEXT("opacity_output_pin"), OpacityOutputPin);
	FString OpacitySourceChannel = TEXT("A");
	Params->TryGetStringField(TEXT("opacity_source_channel"), OpacitySourceChannel);
	OpacitySourceChannel = NormalizeUiMaterialMaskChannel(OpacitySourceChannel);

	bool bCreateMaterial = false;
	Params->TryGetBoolField(TEXT("create_material"), bCreateMaterial);
	bool bConnectOpacity = false;
	Params->TryGetBoolField(TEXT("connect_opacity"), bConnectOpacity);
	const bool bAutoComponentMaskAlphaExplicit = Params->HasField(TEXT("auto_component_mask_alpha"));
	bool bAutoComponentMaskAlpha = false;
	Params->TryGetBoolField(TEXT("auto_component_mask_alpha"), bAutoComponentMaskAlpha);
	const bool bHasDirectOpacityOutput = JsonObjectArrayFieldHasName(Params, TEXT("additional_outputs"), OpacityOutputPin);
	if (bConnectOpacity
		&& !bAutoComponentMaskAlpha
		&& !bAutoComponentMaskAlphaExplicit
		&& !bOpacityOutputPinExplicit
		&& !bHasDirectOpacityOutput)
	{
		bAutoComponentMaskAlpha = true;
	}
	if (bAutoComponentMaskAlpha)
	{
		bConnectOpacity = true;
	}
	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bRunWidgetProof = false;
	Params->TryGetBoolField(TEXT("run_widget_proof"), bRunWidgetProof);
	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	const bool bExecute = !bDryRun && bConfirm;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	if (!bDryRun && !bConfirm)
	{
		Errors.Add(TEXT("dry_run=false requires confirm=true before material or widget mutation."));
	}
	if (!bConnectOpacity)
	{
		Warnings.Add(TEXT("connect_opacity=false: the workflow wires the Custom node to EmissiveColor only. Use connect_opacity=true with an explicit opacity_output_pin, or auto_component_mask_alpha=true to route float4 alpha through a ComponentMask."));
	}
	if (bAutoComponentMaskAlpha && bConnectOpacity && !bAutoComponentMaskAlphaExplicit)
	{
		Warnings.Add(TEXT("connect_opacity=true selected auto_component_mask_alpha because no explicit opacity_output_pin or matching Custom additional output was provided."));
	}
	if (bAutoComponentMaskAlpha && !IsUiMaterialMaskChannelValid(OpacitySourceChannel))
	{
		Errors.Add(FString::Printf(TEXT("opacity_source_channel must be one of R/G/B/A when auto_component_mask_alpha=true; got '%s'."), *OpacitySourceChannel));
	}
	if (bAutoComponentMaskAlpha && !OutputType.Contains(TEXT("4")))
	{
		Errors.Add(FString::Printf(TEXT("auto_component_mask_alpha requires output_type Float4/float4 so a component can be selected; got '%s'."), *OutputType));
	}

	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> MaterialRows;
	TArray<TSharedPtr<FJsonValue>> WidgetRows;

	const TSharedPtr<FJsonObject> CreateParams = MakeUiMaterialCreateParams(MaterialPath, BlendMode, ShadingModel);
	const TSharedPtr<FJsonObject> PropertyParams = MakeUiMaterialPropertyParams(MaterialPath, BlendMode, ShadingModel);
	const TSharedPtr<FJsonObject> CreateCustomParams = MakeUiMaterialCustomParams(MaterialPath, Hlsl, OutputType, CustomInputs, Params);

	bool bHasAdvancedCustomFields = Params->HasField(TEXT("include_file_paths")) || Params->HasField(TEXT("additional_defines"));
	FString EffectiveExpressionName = ExpressionName;
	const FString MaskNodeId = EffectiveExpressionName.IsEmpty()
		? TEXT("${custom_node.expression_name}_OpacityMask")
		: EffectiveExpressionName + TEXT("_OpacityMask");
	TSharedPtr<FJsonObject> OpacityMaskGraphParams;

	if (!bExecute)
	{
		if (bCreateMaterial)
		{
			PlanOrExecutePrimitive(TEXT("material"), TEXT("create_material"), CreateParams, false, true, Actions, MaterialRows, Errors);
		}
		PlanOrExecutePrimitive(TEXT("material"), TEXT("set_material_property"), PropertyParams, false, true, Actions, MaterialRows, Errors);
		if (ExpressionName.IsEmpty())
		{
			PlanOrExecutePrimitive(TEXT("material"), TEXT("create_custom_hlsl_node"), CreateCustomParams, false, true, Actions, MaterialRows, Errors);
			EffectiveExpressionName = TEXT("${custom_node.expression_name}");
			if (bHasAdvancedCustomFields)
			{
				PlanOrExecutePrimitive(TEXT("material"), TEXT("update_custom_hlsl_node"), MakeUiMaterialCustomUpdateParams(MaterialPath, EffectiveExpressionName, Hlsl, OutputType, CustomInputs, Params), false, true, Actions, MaterialRows, Errors);
			}
		}
		else
		{
			PlanOrExecutePrimitive(TEXT("material"), TEXT("update_custom_hlsl_node"), MakeUiMaterialCustomUpdateParams(MaterialPath, ExpressionName, Hlsl, OutputType, CustomInputs, Params), false, true, Actions, MaterialRows, Errors);
		}
		PlanOrExecutePrimitive(TEXT("material"), TEXT("connect_expressions"), MakeUiMaterialConnectParams(MaterialPath, EffectiveExpressionName, ConnectOutputPin, TEXT("EmissiveColor")), false, true, Actions, MaterialRows, Errors);
		if (bConnectOpacity)
		{
			if (bAutoComponentMaskAlpha)
			{
				OpacityMaskGraphParams = MakeUiMaterialAlphaMaskGraphParams(MaterialPath, EffectiveExpressionName, ConnectOutputPin, MaskNodeId, OpacitySourceChannel);
				PlanOrExecutePrimitive(TEXT("material"), TEXT("build_material_graph"), OpacityMaskGraphParams, false, true, Actions, MaterialRows, Errors);
			}
			else
			{
				PlanOrExecutePrimitive(TEXT("material"), TEXT("connect_expressions"), MakeUiMaterialConnectParams(MaterialPath, EffectiveExpressionName, OpacityOutputPin, TEXT("Opacity")), false, true, Actions, MaterialRows, Errors);
			}
		}
		if (bCompile)
		{
			TSharedPtr<FJsonObject> RecompileParams = MakeActionParams(TEXT("asset_path"), MaterialPath);
			RecompileParams->SetBoolField(TEXT("include_stats"), true);
			PlanOrExecutePrimitive(TEXT("material"), TEXT("recompile_material"), RecompileParams, false, true, Actions, MaterialRows, Errors);
			PlanOrExecutePrimitive(TEXT("material"), TEXT("validate_material"), MakeActionParams(TEXT("asset_path"), MaterialPath), false, true, Actions, MaterialRows, Errors);
			PlanOrExecutePrimitive(TEXT("material"), TEXT("get_compilation_stats"), MakeActionParams(TEXT("asset_path"), MaterialPath), false, true, Actions, MaterialRows, Errors);
			PlanOrExecutePrimitive(TEXT("material"), TEXT("get_material_properties"), MakeActionParams(TEXT("asset_path"), MaterialPath), false, true, Actions, MaterialRows, Errors);
			PlanOrExecutePrimitive(TEXT("material"), TEXT("get_full_connection_graph"), MakeActionParams(TEXT("asset_path"), MaterialPath), false, true, Actions, MaterialRows, Errors);
		}
	}
	else
	{
		if (bCreateMaterial)
		{
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("create_material"), CreateParams, Actions, MaterialRows, Errors);
		}
		ExecutePrimitiveAndRecord(TEXT("material"), TEXT("set_material_property"), PropertyParams, Actions, MaterialRows, Errors);
		if (ExpressionName.IsEmpty())
		{
			const FMonolithActionResult CustomResult = ExecutePrimitiveAndRecord(TEXT("material"), TEXT("create_custom_hlsl_node"), CreateCustomParams, Actions, MaterialRows, Errors);
			if (CustomResult.bSuccess && CustomResult.Result.IsValid())
			{
				CustomResult.Result->TryGetStringField(TEXT("expression_name"), EffectiveExpressionName);
			}
			if (EffectiveExpressionName.IsEmpty())
			{
				Errors.Add(TEXT("material.create_custom_hlsl_node did not return expression_name; cannot wire material outputs."));
			}
			else if (bHasAdvancedCustomFields)
			{
				ExecutePrimitiveAndRecord(TEXT("material"), TEXT("update_custom_hlsl_node"), MakeUiMaterialCustomUpdateParams(MaterialPath, EffectiveExpressionName, Hlsl, OutputType, CustomInputs, Params), Actions, MaterialRows, Errors);
			}
		}
		else
		{
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("update_custom_hlsl_node"), MakeUiMaterialCustomUpdateParams(MaterialPath, ExpressionName, Hlsl, OutputType, CustomInputs, Params), Actions, MaterialRows, Errors);
		}
		if (!EffectiveExpressionName.IsEmpty())
		{
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("connect_expressions"), MakeUiMaterialConnectParams(MaterialPath, EffectiveExpressionName, ConnectOutputPin, TEXT("EmissiveColor")), Actions, MaterialRows, Errors);
			if (bConnectOpacity)
			{
				if (bAutoComponentMaskAlpha)
				{
					const FString ExecutedMaskNodeId = EffectiveExpressionName + TEXT("_OpacityMask");
					OpacityMaskGraphParams = MakeUiMaterialAlphaMaskGraphParams(MaterialPath, EffectiveExpressionName, ConnectOutputPin, ExecutedMaskNodeId, OpacitySourceChannel);
					ExecutePrimitiveAndRecord(TEXT("material"), TEXT("build_material_graph"), OpacityMaskGraphParams, Actions, MaterialRows, Errors);
				}
				else
				{
					ExecutePrimitiveAndRecord(TEXT("material"), TEXT("connect_expressions"), MakeUiMaterialConnectParams(MaterialPath, EffectiveExpressionName, OpacityOutputPin, TEXT("Opacity")), Actions, MaterialRows, Errors);
				}
			}
		}
		if (bCompile)
		{
			TSharedPtr<FJsonObject> RecompileParams = MakeActionParams(TEXT("asset_path"), MaterialPath);
			RecompileParams->SetBoolField(TEXT("include_stats"), true);
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("recompile_material"), RecompileParams, Actions, MaterialRows, Errors);
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("validate_material"), MakeActionParams(TEXT("asset_path"), MaterialPath), Actions, MaterialRows, Errors);
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("get_compilation_stats"), MakeActionParams(TEXT("asset_path"), MaterialPath), Actions, MaterialRows, Errors);
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("get_material_properties"), MakeActionParams(TEXT("asset_path"), MaterialPath), Actions, MaterialRows, Errors);
			ExecutePrimitiveAndRecord(TEXT("material"), TEXT("get_full_connection_graph"), MakeActionParams(TEXT("asset_path"), MaterialPath), Actions, MaterialRows, Errors);
		}
	}

	const TSharedPtr<FJsonObject> BindParams = MakeUiMaterialBindParams(BindingAction, WidgetAssetPath, WidgetName, PropertyName, MaterialPath, false);
	PlanOrExecutePrimitive(TEXT("ui"), BindingAction, BindParams, bExecute, true, Actions, WidgetRows, Errors);
	if (bCompile)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("compile_widget"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecute, true, Actions, WidgetRows, Errors);
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("dump_blueprint_compile_log"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecute, true, Actions, WidgetRows, Errors);
	}
	const TSharedPtr<FJsonObject> MaterialLifecycleAuditParams = MakeUiMaterialLifecycleAuditParams(WidgetAssetPath);
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_widget_material_lifecycle"), MaterialLifecycleAuditParams, bExecute, false, Actions, WidgetRows, Warnings);
	TSharedPtr<FJsonObject> WidgetProofParams = MakeActionParams(TEXT("widget_asset_path"), WidgetAssetPath);
	WidgetProofParams->SetStringField(TEXT("proof_profile"), TEXT("visual"));
	WidgetProofParams->SetBoolField(TEXT("dry_run"), !bExecute);
	WidgetProofParams->SetBoolField(TEXT("run_read_only_checks"), true);
	PlanOrExecutePrimitive(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint"), WidgetProofParams, bExecute && bRunWidgetProof, true, Actions, WidgetRows, Errors);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("material_path"), MaterialPath);
	Input->SetStringField(TEXT("widget_asset_path"), WidgetAssetPath);
	Input->SetStringField(TEXT("widget_name"), WidgetName);
	Input->SetStringField(TEXT("binding_action"), BindingAction);
	Input->SetStringField(TEXT("property_name"), PropertyName);
	Input->SetStringField(TEXT("expression_name"), EffectiveExpressionName);
	Input->SetBoolField(TEXT("create_material"), bCreateMaterial);
	Input->SetBoolField(TEXT("connect_opacity"), bConnectOpacity);
	Input->SetBoolField(TEXT("auto_component_mask_alpha"), bAutoComponentMaskAlpha);
	Input->SetStringField(TEXT("opacity_source_channel"), OpacitySourceChannel);
	Input->SetBoolField(TEXT("compile"), bCompile);
	Input->SetBoolField(TEXT("run_widget_proof"), bRunWidgetProof);

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetObjectField(TEXT("hlsl"), HlslProof);
	Validation->SetObjectField(TEXT("material_proof"), MakeUnavailableProof(Errors.Num() > 0 ? TEXT("blocked") : (bExecute ? TEXT("attempted") : TEXT("planned")), TEXT("UI material domain, Custom HLSL node, output wiring, compile, stats, and connection graph readback are composed through material owner actions.")));
	Validation->SetObjectField(TEXT("opacity_wiring"), MakeUiMaterialOpacityWiringProof(
		bConnectOpacity,
		bAutoComponentMaskAlpha,
		OpacitySourceChannel,
		EffectiveExpressionName,
		ConnectOutputPin,
		OpacityOutputPin,
		bAutoComponentMaskAlpha ? (EffectiveExpressionName.IsEmpty() ? MaskNodeId : EffectiveExpressionName + TEXT("_OpacityMask")) : TEXT(""),
		Errors.Num() > 0 ? TEXT("blocked") : (bExecute ? TEXT("attempted") : TEXT("planned"))));
	Validation->SetObjectField(TEXT("binding_proof"), MakeUnavailableProof(Errors.Num() > 0 ? TEXT("blocked") : (bExecute ? TEXT("attempted") : TEXT("planned")), TEXT("Widget material binding is composed through ui.set_image or ui.set_brush plus ui.compile_widget.")));
	Validation->SetObjectField(TEXT("material_lifecycle"), MakeUnavailableProof(Errors.Num() > 0 ? TEXT("blocked") : (bExecute ? TEXT("attempted") : TEXT("planned")), TEXT("Widget Blueprint dynamic-material lifecycle lint is composed through ui.audit_widget_material_lifecycle.")));
	Validation->SetObjectField(TEXT("widget_proof"), MakeUnavailableProof(bRunWidgetProof ? (bExecute ? TEXT("attempted") : TEXT("planned")) : TEXT("next_action"), bRunWidgetProof ? TEXT("workflow.ui_shipping_widget_blueprint is part of this workflow run.") : TEXT("Run workflow.ui_shipping_widget_blueprint explicitly for visual artifact proof.")));

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("material"), MaterialRows);
	Proof->SetArrayField(TEXT("widget"), WidgetRows);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Proof->SetStringField(TEXT("runtime_note"), TEXT("This workflow proves material graph authoring, compile/stat read-back, and widget binding. Retainer/runtime perf proof remains a separate workflow slice."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("ui_material_hlsl_effect"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("ui_material_custom_hlsl_brush_binding_v2"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeUiMaterialPlanObject(bDryRun, bConfirm, bCreateMaterial, bCompile, true, bRunWidgetProof, bAutoComponentMaskAlpha));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { MaterialPath, WidgetAssetPath }, { MaterialPath, WidgetAssetPath }, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("validation"), Validation);
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("workflow.ui_shipping_widget_blueprint"), FMonolithToolRegistry::Get().HasAction(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint")), true, TEXT("Run visual widget proof after material binding."), WidgetProofParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.audit_widget_material_lifecycle"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("audit_widget_material_lifecycle")), true, TEXT("Audit Widget Blueprint graph MID lifetime after material binding."), MaterialLifecycleAuditParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("material.get_material_properties"), FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_material_properties")), true, TEXT("Read back material domain/blend/shading properties."), MakeActionParams(TEXT("asset_path"), MaterialPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.dump_blueprint_compile_log"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")), true, TEXT("Read Widget Blueprint compile warnings/errors through the UI owner action."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist reviewed material and widget assets explicitly."), MakeActionParams(TEXT("asset_path"), MaterialPath))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("Rollback is source-control/editor-undo based; the workflow records touched assets but does not auto-delete material expressions."),
		TEXT("auto_component_mask_alpha inserts a ComponentMask through material.build_material_graph but does not yet deduplicate older masks from repeated confirmed runs."),
		TEXT("RetainerBox effect-material proof is handled by workflow.ui_retainer_effect_material.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("pass")));
	if (bDryRun)
	{
		Warnings.Add(TEXT("dry_run=true returned the material/ui owner-action plan only; no material or widget assets were mutated."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleUiRetainerEffectMaterial(const TSharedPtr<FJsonObject>& Params)
{
	FString MaterialPath;
	Params->TryGetStringField(TEXT("material_path"), MaterialPath);
	if (MaterialPath.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("material_path is required."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TSharedPtr<FJsonObject>* BindToPtr = nullptr;
	if (!Params->TryGetObjectField(TEXT("bind_to"), BindToPtr) || !BindToPtr || !BindToPtr->IsValid())
	{
		return FMonolithActionResult::Error(TEXT("bind_to must be an object with asset_path and retainer_widget_name/widget_name."), FMonolithJsonUtils::ErrInvalidParams);
	}
	const TSharedPtr<FJsonObject> BindTo = *BindToPtr;

	FString WidgetAssetPath;
	BindTo->TryGetStringField(TEXT("asset_path"), WidgetAssetPath);
	if (WidgetAssetPath.IsEmpty())
	{
		BindTo->TryGetStringField(TEXT("widget_asset_path"), WidgetAssetPath);
	}
	if (WidgetAssetPath.IsEmpty())
	{
		BindTo->TryGetStringField(TEXT("wbp_path"), WidgetAssetPath);
	}

	FString RetainerWidgetName;
	BindTo->TryGetStringField(TEXT("retainer_widget_name"), RetainerWidgetName);
	if (RetainerWidgetName.IsEmpty())
	{
		BindTo->TryGetStringField(TEXT("widget_name"), RetainerWidgetName);
	}
	if (WidgetAssetPath.IsEmpty() || RetainerWidgetName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("bind_to.asset_path and bind_to.retainer_widget_name/widget_name are required."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString TextureParameter = TEXT("Texture");
	BindTo->TryGetStringField(TEXT("texture_parameter"), TextureParameter);
	if (TextureParameter.IsEmpty())
	{
		Params->TryGetStringField(TEXT("texture_parameter"), TextureParameter);
	}
	if (TextureParameter.IsEmpty())
	{
		TextureParameter = TEXT("Texture");
	}

	bool bCompile = true;
	Params->TryGetBoolField(TEXT("compile"), bCompile);
	bool bRunReadOnlyChecks = false;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunReadOnlyChecks);
	bool bRequestRender = false;
	Params->TryGetBoolField(TEXT("request_render"), bRequestRender);
	bool bRunWidgetProof = false;
	Params->TryGetBoolField(TEXT("run_widget_proof"), bRunWidgetProof);
	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	const bool bExecuteMutation = !bDryRun && bConfirm;
	const bool bExecuteReadOnly = bExecuteMutation || bRunReadOnlyChecks;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	if (!bDryRun && !bConfirm)
	{
		Errors.Add(TEXT("dry_run=false requires confirm=true before RetainerBox mutation."));
	}
	if (!bRunWidgetProof)
	{
		Warnings.Add(TEXT("run_widget_proof=false: visual artifact proof is returned as an explicit next action."));
	}
	Warnings.Add(TEXT("Retainer/invalidation performance proof is not claimed by this workflow; run a measured before/after runtime profile before treating RetainerBox as an optimization."));

	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> MaterialRows;
	TArray<TSharedPtr<FJsonValue>> WidgetRows;

	PlanOrExecutePrimitive(TEXT("material"), TEXT("get_material_parameters"), MakeActionParams(TEXT("asset_path"), MaterialPath), bExecuteReadOnly, true, Actions, MaterialRows, Errors);
	PlanOrExecutePrimitive(TEXT("material"), TEXT("get_material_properties"), MakeActionParams(TEXT("asset_path"), MaterialPath), bExecuteReadOnly, true, Actions, MaterialRows, Errors);

	const TSharedPtr<FJsonObject> RetainerBindParams = MakeUiRetainerBindParams(
		WidgetAssetPath,
		RetainerWidgetName,
		MaterialPath,
		TextureParameter,
		false,
		bRequestRender);
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("set_retainer_effect_material"), RetainerBindParams, bExecuteMutation, true, Actions, WidgetRows, Errors);
	if (bCompile)
	{
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("compile_widget"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteMutation, true, Actions, WidgetRows, Errors);
		PlanOrExecutePrimitive(TEXT("ui"), TEXT("dump_blueprint_compile_log"), MakeActionParams(TEXT("asset_path"), WidgetAssetPath), bExecuteMutation, true, Actions, WidgetRows, Errors);
	}
	const TSharedPtr<FJsonObject> MaterialLifecycleAuditParams = MakeUiMaterialLifecycleAuditParams(WidgetAssetPath);
	PlanOrExecutePrimitive(TEXT("ui"), TEXT("audit_widget_material_lifecycle"), MaterialLifecycleAuditParams, bExecuteReadOnly, false, Actions, WidgetRows, Warnings);

	TSharedPtr<FJsonObject> WidgetProofParams = MakeActionParams(TEXT("widget_asset_path"), WidgetAssetPath);
	WidgetProofParams->SetStringField(TEXT("proof_profile"), TEXT("visual"));
	WidgetProofParams->SetBoolField(TEXT("dry_run"), !bExecuteMutation);
	WidgetProofParams->SetBoolField(TEXT("run_read_only_checks"), true);
	PlanOrExecutePrimitive(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint"), WidgetProofParams, bExecuteMutation && bRunWidgetProof, true, Actions, WidgetRows, Errors);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("material_path"), MaterialPath);
	Input->SetStringField(TEXT("widget_asset_path"), WidgetAssetPath);
	Input->SetStringField(TEXT("retainer_widget_name"), RetainerWidgetName);
	Input->SetStringField(TEXT("texture_parameter"), TextureParameter);
	Input->SetBoolField(TEXT("compile"), bCompile);
	Input->SetBoolField(TEXT("run_read_only_checks"), bRunReadOnlyChecks);
	Input->SetBoolField(TEXT("request_render"), bRequestRender);
	Input->SetBoolField(TEXT("run_widget_proof"), bRunWidgetProof);

	const FString ReadStatus = Errors.Num() > 0 ? TEXT("blocked") : (bExecuteReadOnly ? TEXT("attempted") : TEXT("planned"));
	const FString MutateStatus = Errors.Num() > 0 ? TEXT("blocked") : (bExecuteMutation ? TEXT("attempted") : TEXT("planned"));
	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetObjectField(TEXT("material_parameter_proof"), MakeUnavailableProof(ReadStatus, TEXT("Exact Retainer texture-parameter existence is proved through material.get_material_parameters and enforced by ui.set_retainer_effect_material.")));
	Validation->SetObjectField(TEXT("material_domain_proof"), MakeUnavailableProof(ReadStatus, TEXT("UI material domain is read through material.get_material_properties and enforced by the Retainer owner action.")));
	Validation->SetObjectField(TEXT("binding_proof"), MakeUnavailableProof(MutateStatus, TEXT("Retainer effect material binding uses ui.set_retainer_effect_material, not raw set_widget_property.")));
	Validation->SetObjectField(TEXT("material_lifecycle"), MakeUnavailableProof(ReadStatus, TEXT("Widget Blueprint dynamic-material lifecycle lint is composed through ui.audit_widget_material_lifecycle.")));
	Validation->SetObjectField(TEXT("widget_proof"), MakeUnavailableProof(bRunWidgetProof ? MutateStatus : TEXT("next_action"), bRunWidgetProof ? TEXT("workflow.ui_shipping_widget_blueprint is part of this workflow run.") : TEXT("Run workflow.ui_shipping_widget_blueprint explicitly for visual artifact proof.")));
	Validation->SetObjectField(TEXT("runtime_profile"), MakeUnavailableProof(TEXT("not_claimed"), TEXT("Retainer performance benefit requires a separate measured before/after runtime profile.")));

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("material"), MaterialRows);
	Proof->SetArrayField(TEXT("widget"), WidgetRows);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Proof->SetStringField(TEXT("runtime_note"), TEXT("This workflow proves RetainerBox effect-material binding and exact texture-parameter contract. It does not claim Retainer performance gains without a separate profile."));

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("ui_retainer_effect_material"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("ui_retainer_effect_texture_parameter_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeUiRetainerPlanObject(bDryRun, bConfirm, bCompile, bRunReadOnlyChecks, bRunWidgetProof, bRequestRender));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { MaterialPath, WidgetAssetPath }, { WidgetAssetPath }, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("validation"), Validation);
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("workflow.ui_shipping_widget_blueprint"), FMonolithToolRegistry::Get().HasAction(TEXT("workflow"), TEXT("ui_shipping_widget_blueprint")), true, TEXT("Run visual widget proof after Retainer effect binding."), WidgetProofParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.audit_widget_material_lifecycle"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("audit_widget_material_lifecycle")), true, TEXT("Audit Widget Blueprint graph MID lifetime before treating the Retainer workflow as production-ready."), MaterialLifecycleAuditParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("material.get_material_parameters"), FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_material_parameters")), true, TEXT("Repeat exact texture-parameter readback if the material changed."), MakeActionParams(TEXT("asset_path"), MaterialPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("material.get_material_properties"), FMonolithToolRegistry::Get().HasAction(TEXT("material"), TEXT("get_material_properties")), true, TEXT("Read back material domain/blend/shading properties."), MakeActionParams(TEXT("asset_path"), MaterialPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("ui.dump_blueprint_compile_log"), FMonolithToolRegistry::Get().HasAction(TEXT("ui"), TEXT("dump_blueprint_compile_log")), true, TEXT("Read Widget Blueprint compile warnings/errors through the UI owner action."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist reviewed Widget Blueprint and material assets explicitly."), MakeActionParams(TEXT("asset_path"), WidgetAssetPath))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("Rollback is source-control/editor-undo based; the workflow records touched assets but does not auto-clear RetainerBox effect fields."),
		TEXT("This workflow binds an existing RetainerBox. It does not insert RetainerBox or InvalidationBox as an optimization."),
		TEXT("Runtime/performance proof is intentionally separate from static Retainer material binding proof.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("pass")));
	if (bDryRun && !bRunReadOnlyChecks)
	{
		Warnings.Add(TEXT("dry_run=true returned the Retainer owner-action plan only; pass run_read_only_checks=true to execute material parameter/domain readback without mutation."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleShotRenderLevelSequence(const TSharedPtr<FJsonObject>& Params)
{
	FString SequenceAssetPath;
	Params->TryGetStringField(TEXT("sequence_asset_path"), SequenceAssetPath);
	FString QueueAssetPath;
	Params->TryGetStringField(TEXT("queue_asset_path"), QueueAssetPath);
	FString MapPath;
	Params->TryGetStringField(TEXT("map_path"), MapPath);
	FString JobName;
	Params->TryGetStringField(TEXT("job_name"), JobName);
	FString OutputDirectory;
	Params->TryGetStringField(TEXT("output_directory"), OutputDirectory);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludeAnimMixer = true;
	Params->TryGetBoolField(TEXT("include_anim_mixer"), bIncludeAnimMixer);
	bool bRenderRequired = false;
	Params->TryGetBoolField(TEXT("render_required"), bRenderRequired);

	const bool bExecuteReadOnly = !bDryRun && bRunChecks;
	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TArray<FString> TouchedAssets;
	AddUniqueWorkflowString(TouchedAssets, SequenceAssetPath);
	AddUniqueWorkflowString(TouchedAssets, QueueAssetPath);
	AddUniqueWorkflowString(TouchedAssets, MapPath);

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("sequence_asset_path"), SequenceAssetPath);
	if (!QueueAssetPath.IsEmpty())
	{
		Input->SetStringField(TEXT("queue_asset_path"), QueueAssetPath);
	}
	if (!MapPath.IsEmpty())
	{
		Input->SetStringField(TEXT("map_path"), MapPath);
	}
	if (!JobName.IsEmpty())
	{
		Input->SetStringField(TEXT("job_name"), JobName);
	}
	if (!OutputDirectory.IsEmpty())
	{
		Input->SetStringField(TEXT("output_directory"), OutputDirectory);
	}
	Input->SetBoolField(TEXT("render_required"), bRenderRequired);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("shot_render"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("level_sequence_mrq_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeShotRenderPlanObject(bDryRun, bRenderRequired));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, TouchedAssets, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		TouchedAssets,
		{ TEXT("Shot render first slice is read-only; queue mutation, render launch, save, and source-control actions remain explicit follow-ups.") }));

	TArray<TSharedPtr<FJsonValue>> SequenceRows;
	PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("list_bindings"), MakeActionParams(TEXT("asset_path"), SequenceAssetPath), bExecuteReadOnly, true, Actions, SequenceRows, Errors);
	PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("get_director_info"), MakeActionParams(TEXT("asset_path"), SequenceAssetPath), bExecuteReadOnly, true, Actions, SequenceRows, Errors);
	PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("list_event_bindings"), MakeActionParams(TEXT("asset_path"), SequenceAssetPath), bExecuteReadOnly, true, Actions, SequenceRows, Errors);
	if (bIncludeAnimMixer)
	{
		PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("get_anim_mixer_status"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, SequenceRows, Warnings);
		TSharedPtr<FJsonObject> AnimMixerParams = MakeActionParams(TEXT("asset_path"), SequenceAssetPath);
		AnimMixerParams->SetBoolField(TEXT("include_layers"), true);
		PlanOrExecutePrimitive(TEXT("level_sequence"), TEXT("list_anim_mixer_tracks"), AnimMixerParams, bExecuteReadOnly, false, Actions, SequenceRows, Warnings);
	}

	TArray<TSharedPtr<FJsonValue>> RenderRows;
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("get_queue"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, RenderRows, Errors);
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("is_rendering"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, RenderRows, Errors);
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("list_settings"), MakeEmptyParams(), bExecuteReadOnly, false, Actions, RenderRows, Warnings);
	if (!QueueAssetPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> LoadQueueParams = MakeActionParams(TEXT("asset_path"), QueueAssetPath);
		LoadQueueParams->SetBoolField(TEXT("prompt_on_dirty"), false);
		PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("load_queue"), LoadQueueParams, false, true, Actions, RenderRows, Errors);
	}
	TSharedPtr<FJsonObject> AddJobParams = MakeShared<FJsonObject>();
	AddJobParams->SetStringField(TEXT("sequence_path"), SequenceAssetPath);
	AddJobParams->SetBoolField(TEXT("clear_existing"), QueueAssetPath.IsEmpty());
	AddJobParams->SetBoolField(TEXT("enabled"), true);
	if (!MapPath.IsEmpty())
	{
		AddJobParams->SetStringField(TEXT("map_path"), MapPath);
	}
	if (!JobName.IsEmpty())
	{
		AddJobParams->SetStringField(TEXT("job_name"), JobName);
	}
	PlanOrExecutePrimitive(TEXT("movie_render"), TEXT("add_job"), AddJobParams, false, true, Actions, RenderRows, Errors);

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), SequenceRows);
	TSharedPtr<FJsonObject> RenderValidation = MakeUnavailableProof(
		bRenderRequired ? TEXT("blocked") : (bExecuteReadOnly ? TEXT("checked") : TEXT("planned")),
		bRenderRequired ? TEXT("Rendering is declared but not launched by this first slice; call movie_render.render_queue with confirm=true.") : TEXT("MRQ readiness rows are in proof.read_back."));
	RenderValidation->SetArrayField(TEXT("read_back"), RenderRows);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	Validation->SetObjectField(TEXT("render"), RenderValidation);
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Shot render readiness does not run PIE/gameplay runtime proof.")));
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No Blueprint compile gate applies to the shot render first slice.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Render cost/budget proof requires a later MRQ output analysis slice.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No accessibility proof applies to cinematic render readiness.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (!OutputDirectory.IsEmpty() || bRenderRequired)
	{
		TSharedPtr<FJsonObject> Artifact = MakeUnavailableProof(TEXT("planned"), TEXT("Render output artifact is produced only by an explicit movie_render.render_queue follow-up."));
		if (!OutputDirectory.IsEmpty())
		{
			Artifact->SetStringField(TEXT("path"), OutputDirectory);
		}
		Artifact->SetStringField(TEXT("type"), TEXT("movie_render_output"));
		Artifacts.Add(MakeShared<FJsonValueObject>(Artifact));
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(Artifact));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), Artifacts);

	if (bRenderRequired)
	{
		Errors.Add(TEXT("render_required=true requested, but workflow.shot_render_level_sequence is read-only; call movie_render.render_queue explicitly with confirm=true after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	if (!QueueAssetPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> LoadQueueParams = MakeActionParams(TEXT("asset_path"), QueueAssetPath);
		LoadQueueParams->SetBoolField(TEXT("prompt_on_dirty"), false);
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.load_queue"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("load_queue")), true, TEXT("Load the reviewed queue asset explicitly."), LoadQueueParams)));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.add_job"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("add_job")), true, TEXT("Add or refresh the MRQ job after reviewing sequence proof."), AddJobParams)));
	TSharedPtr<FJsonObject> RenderParams = MakeShared<FJsonObject>();
	RenderParams->SetBoolField(TEXT("confirm"), true);
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.render_queue"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("render_queue")), true, TEXT("Launch the reviewed MRQ render explicitly."), RenderParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("movie_render.render_progress"), FMonolithToolRegistry::Get().HasAction(TEXT("movie_render"), TEXT("render_progress")), true, TEXT("Poll render progress after launch."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare queue/sequence/map assets through source control if they will be changed."), MakeStringArrayParams(TEXT("paths"), TouchedAssets))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No queue mutation or render launch is performed by this first slice."),
		TEXT("MRQ renders must be cancelled via movie_render.cancel_render if a later explicit render action is launched.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only shot render proof completed where actions were available; queue mutation and render launch remain follow-up actions."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleAudioShippingAsset(const TSharedPtr<FJsonObject>& Params)
{
	FString AudioAssetPath;
	Params->TryGetStringField(TEXT("audio_asset_path"), AudioAssetPath);
	FString AssetKind = TEXT("auto");
	Params->TryGetStringField(TEXT("asset_kind"), AssetKind);
	if (AssetKind.IsEmpty())
	{
		AssetKind = TEXT("auto");
	}

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludePerception = true;
	Params->TryGetBoolField(TEXT("include_perception_binding"), bIncludePerception);
	bool bPreviewRequired = false;
	Params->TryGetBoolField(TEXT("preview_required"), bPreviewRequired);
	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);

	const bool bExecuteReadOnly = !bDryRun && bRunChecks;
	const bool bIsMetaSound = AssetKind.Equals(TEXT("MetaSoundSource"), ESearchCase::IgnoreCase) || AssetKind.Equals(TEXT("MetaSound"), ESearchCase::IgnoreCase);
	const bool bIsSoundCue = AssetKind.Equals(TEXT("SoundCue"), ESearchCase::IgnoreCase);
	const bool bIsSoundWave = AssetKind.Equals(TEXT("SoundWave"), ESearchCase::IgnoreCase);
	const bool bIsAuto = AssetKind.Equals(TEXT("auto"), ESearchCase::IgnoreCase);

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("audio_asset_path"), AudioAssetPath);
	Input->SetStringField(TEXT("asset_kind"), AssetKind);
	Input->SetBoolField(TEXT("preview_required"), bPreviewRequired);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("audio_shipping"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("audio_asset_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeAudioShippingPlanObject(bDryRun, AssetKind));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { AudioAssetPath }, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{ AudioAssetPath },
		{ TEXT("Audio shipping first slice is read-only; preview, save, and source-control actions remain explicit follow-ups.") }));

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> AssetRows;
	PlanOrExecutePrimitive(TEXT("audio"), TEXT("search_audio_assets"), MakeAudioSearchParams(AudioAssetPath, AssetKind), bExecuteReadOnly, true, Actions, AssetRows, Errors);
	if (bIsMetaSound)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_metasound_info"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("validate_metasound"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_metasound_graph"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_metasound_dependencies"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, AssetRows, Warnings);
	}
	else if (bIsSoundCue)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_cue_graph"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("validate_sound_cue"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_cue_duration"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, AssetRows, Warnings);
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("find_sound_waves_in_cue"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, AssetRows, Warnings);
	}
	else if (bIsSoundWave)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_wave_info"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, true, Actions, AssetRows, Errors);
	}
	else if (!bIsAuto)
	{
		Warnings.Add(TEXT("asset_kind is not one of auto, SoundWave, SoundCue, or MetaSoundSource; only generic discovery/perception proof is planned."));
	}
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), AssetRows);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);

	TArray<TSharedPtr<FJsonValue>> RuntimeRows;
	if (bIncludePerception)
	{
		PlanOrExecutePrimitive(TEXT("audio"), TEXT("get_sound_perception_binding"), MakeActionParams(TEXT("asset_path"), AudioAssetPath), bExecuteReadOnly, false, Actions, RuntimeRows, Warnings);
	}
	TSharedPtr<FJsonObject> RuntimeValidation = MakeUnavailableProof(bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"), TEXT("Sound perception binding proof is read-only; runtime audio playback is not started by this first slice."));
	RuntimeValidation->SetArrayField(TEXT("read_back"), RuntimeRows);
	Validation->SetObjectField(TEXT("runtime"), RuntimeValidation);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No Blueprint compile gate applies to audio shipping readiness.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("planned"), TEXT("Use type-specific audio rows for duration/compression/graph complexity proof; no package mutation is performed.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No accessibility proof applies to audio asset readiness.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	if (bPreviewRequired)
	{
		TSharedPtr<FJsonObject> PreviewBlocker = MakeUnavailableProof(TEXT("blocked"), TEXT("Audio preview is not played by this first slice; call audio.preview_sound explicitly."));
		PreviewBlocker->SetStringField(TEXT("next_action"), TEXT("audio.preview_sound"));
		PreviewBlocker->SetObjectField(TEXT("params"), MakeActionParams(TEXT("asset_path"), AudioAssetPath));
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(PreviewBlocker));
		Warnings.Add(TEXT("preview_required=true but this first slice only reports the preview blocker and next action."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.audio_shipping_asset is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	TArray<TSharedPtr<FJsonValue>> NextActions;
	if (bIsMetaSound)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.validate_metasound"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("validate_metasound")), true, TEXT("Run or repeat MetaSound validation."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	}
	else if (bIsSoundCue)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.validate_sound_cue"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("validate_sound_cue")), true, TEXT("Run or repeat SoundCue validation."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	}
	else if (bIsSoundWave)
	{
		NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.get_sound_wave_info"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("get_sound_wave_info")), true, TEXT("Read SoundWave duration/compression details."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	}
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("audio.preview_sound"), FMonolithToolRegistry::Get().HasAction(TEXT("audio"), TEXT("preview_sound")), true, TEXT("Preview the reviewed sound asset explicitly."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist reviewed audio package explicitly."), MakeActionParams(TEXT("asset_path"), AudioAssetPath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the audio package path through source control."), MakeStringArrayParams(TEXT("paths"), { AudioAssetPath }))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No audio asset mutation or preview playback is performed by this first slice."),
		TEXT("Later preview playback must be stopped via audio.stop_preview if needed.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only audio shipping proof completed where actions were available; preview, save, and source-control remain follow-up actions."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleLocalizationShippingStringTable(const TSharedPtr<FJsonObject>& Params)
{
	FString StringTablePath;
	Params->TryGetStringField(TEXT("string_table_path"), StringTablePath);
	FString CsvPath;
	Params->TryGetStringField(TEXT("csv_path"), CsvPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bExportRequested = false;
	Params->TryGetBoolField(TEXT("export_requested"), bExportRequested);
	bool bSaveRequested = false;
	Params->TryGetBoolField(TEXT("save"), bSaveRequested);
	const bool bExecuteReadOnly = !bDryRun && bRunChecks;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("string_table_path"), StringTablePath);
	CopyJsonField(Params, TEXT("cultures"), Input);
	if (!CsvPath.IsEmpty())
	{
		Input->SetStringField(TEXT("csv_path"), CsvPath);
	}
	Input->SetBoolField(TEXT("export_requested"), bExportRequested);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("localization_shipping"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("string_table_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeLocalizationShippingPlanObject(bDryRun, bExportRequested));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, { StringTablePath }, {}, CsvPath.IsEmpty() ? TArray<FString>{} : TArray<FString>{ CsvPath }));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_requested_read_only_first_slice"),
		{ StringTablePath },
		{ TEXT("Localization shipping first slice is read-only; CSV export/import, save, and source-control actions remain explicit follow-ups.") }));

	TArray<TSharedPtr<FJsonValue>> LocalizationRows;
	TSharedPtr<FJsonObject> CultureParams = MakeShared<FJsonObject>();
	CopyJsonField(Params, TEXT("cultures"), CultureParams);
	CultureParams->SetBoolField(TEXT("include_derived"), true);
	PlanOrExecutePrimitive(TEXT("localization"), TEXT("list_cultures"), CultureParams, bExecuteReadOnly, false, Actions, LocalizationRows, Warnings);
	TSharedPtr<FJsonObject> TableParams = MakeActionParams(TEXT("asset_path"), StringTablePath);
	TableParams->SetBoolField(TEXT("include_metadata"), true);
	TableParams->SetNumberField(TEXT("limit"), 200);
	PlanOrExecutePrimitive(TEXT("localization"), TEXT("get_string_table"), TableParams, bExecuteReadOnly, true, Actions, LocalizationRows, Errors);
	PlanOrExecutePrimitive(TEXT("localization"), TEXT("validate_string_table"), MakeActionParams(TEXT("asset_path"), StringTablePath), bExecuteReadOnly, true, Actions, LocalizationRows, Errors);
	if (bExportRequested || !CsvPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> ExportParams = MakeActionParams(TEXT("asset_path"), StringTablePath);
		ExportParams->SetStringField(TEXT("file_path"), CsvPath.IsEmpty() ? TEXT("Saved/Monolith/Localization/string_table_export.csv") : CsvPath);
		ExportParams->SetBoolField(TEXT("include_metadata"), true);
		ExportParams->SetBoolField(TEXT("dry_run"), true);
		ExportParams->SetBoolField(TEXT("confirm"), false);
		PlanOrExecutePrimitive(TEXT("localization"), TEXT("export_string_table_csv"), ExportParams, false, true, Actions, LocalizationRows, Errors);
	}

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> AssetValidation = MakeShared<FJsonObject>();
	AssetValidation->SetStringField(TEXT("status"), bExecuteReadOnly ? TEXT("checked") : TEXT("planned"));
	AssetValidation->SetArrayField(TEXT("read_back"), LocalizationRows);
	Validation->SetObjectField(TEXT("asset_validation"), AssetValidation);
	TSharedPtr<FJsonObject> LocalizationValidation = MakeUnavailableProof(
		bExecuteReadOnly ? TEXT("checked") : TEXT("planned"),
		TEXT("Culture, StringTable read-back, and validation rows are in proof.read_back; CSV export is explicit."));
	LocalizationValidation->SetArrayField(TEXT("read_back"), LocalizationRows);
	Validation->SetObjectField(TEXT("localization"), LocalizationValidation);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Localization target gather/compile is not implemented in this first StringTable slice.")));
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Runtime culture switching is not performed by this first slice.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No render/audio budget proof applies to localization readiness.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("StringTable readability is covered by localization validation, not UI accessibility audit.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> Artifacts;
	if (bExportRequested || !CsvPath.IsEmpty())
	{
		TSharedPtr<FJsonObject> CsvArtifact = MakeUnavailableProof(TEXT("blocked"), TEXT("CSV export writes a file and must be run explicitly through localization.export_string_table_csv."));
		CsvArtifact->SetStringField(TEXT("type"), TEXT("string_table_csv"));
		CsvArtifact->SetStringField(TEXT("path"), CsvPath.IsEmpty() ? TEXT("Saved/Monolith/Localization/string_table_export.csv") : CsvPath);
		Artifacts.Add(MakeShared<FJsonValueObject>(CsvArtifact));
		Errors.Add(TEXT("export_requested/csv_path supplied, but workflow.localization_shipping_string_table is read-only; call localization.export_string_table_csv explicitly after reviewing proof."));
	}
	if (bSaveRequested)
	{
		Errors.Add(TEXT("save=true requested, but workflow.localization_shipping_string_table is read-only in this first slice; call asset.save_asset explicitly after reviewing proof."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), Artifacts);

	TArray<TSharedPtr<FJsonValue>> NextActions;
	TSharedPtr<FJsonObject> ExportParams = MakeActionParams(TEXT("asset_path"), StringTablePath);
	ExportParams->SetStringField(TEXT("file_path"), CsvPath.IsEmpty() ? TEXT("Saved/Monolith/Localization/string_table_export.csv") : CsvPath);
	ExportParams->SetBoolField(TEXT("include_metadata"), true);
	ExportParams->SetBoolField(TEXT("dry_run"), true);
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("localization.export_string_table_csv"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("export_string_table_csv")), true, TEXT("Export reviewed StringTable rows explicitly; run dry_run first."), ExportParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("localization.set_string_entry"), FMonolithToolRegistry::Get().HasAction(TEXT("localization"), TEXT("set_string_entry")), true, TEXT("Patch missing or invalid source strings explicitly with dry_run/confirm."), MakeActionParams(TEXT("asset_path"), StringTablePath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("asset.save_asset"), FMonolithToolRegistry::Get().HasAction(TEXT("asset"), TEXT("save_asset")), true, TEXT("Persist reviewed StringTable package explicitly."), MakeActionParams(TEXT("asset_path"), StringTablePath))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the StringTable package path through source control."), MakeStringArrayParams(TEXT("paths"), { StringTablePath }))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No StringTable mutation, CSV write, or save is performed by this first slice."),
		TEXT("Later CSV export/import files must be deleted or reverted explicitly if they are no longer wanted.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only localization shipping proof completed where actions were available; CSV export/import, save, and source-control remain follow-up actions."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleSlateEuwTestFlow(const TSharedPtr<FJsonObject>& Params)
{
	FString Target;
	Params->TryGetStringField(TEXT("target"), Target);
	FString TargetKind = TEXT("auto");
	Params->TryGetStringField(TEXT("target_kind"), TargetKind);
	if (TargetKind.IsEmpty())
	{
		TargetKind = TEXT("auto");
	}
	FString Ref;
	Params->TryGetStringField(TEXT("ref"), Ref);
	FString CaptureOutputPath;
	Params->TryGetStringField(TEXT("capture_output_path"), CaptureOutputPath);

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bRunChecks = true;
	Params->TryGetBoolField(TEXT("run_read_only_checks"), bRunChecks);
	bool bIncludeSnapshot = true;
	Params->TryGetBoolField(TEXT("include_snapshot"), bIncludeSnapshot);
	bool bIncludeWait = true;
	Params->TryGetBoolField(TEXT("include_wait"), bIncludeWait);
	bool bCaptureRequired = false;
	Params->TryGetBoolField(TEXT("capture_required"), bCaptureRequired);
	bool bInteractionRequired = false;
	Params->TryGetBoolField(TEXT("interaction_required"), bInteractionRequired);

	double WaitTimeoutSec = 2.0;
	Params->TryGetNumberField(TEXT("wait_timeout_sec"), WaitTimeoutSec);
	const int32 WaitTimeoutMs = FMath::Clamp(FMath::RoundToInt(WaitTimeoutSec * 1000.0), 16, 5000);
	const bool bExecuteReadOnly = !bDryRun && bRunChecks;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("target"), Target);
	Input->SetStringField(TEXT("target_kind"), TargetKind);
	if (!Ref.IsEmpty())
	{
		Input->SetStringField(TEXT("ref"), Ref);
	}
	Input->SetBoolField(TEXT("capture_required"), bCaptureRequired);
	Input->SetBoolField(TEXT("interaction_required"), bInteractionRequired);
	CopyJsonField(Params, TEXT("interaction_plan"), Input);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("slate_euw_test_flow"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("slate_euw_readiness_proof_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), false);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeSlateEuwPlanObject(bDryRun, bInteractionRequired, bCaptureRequired));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject({}, {}, {}, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});
	Result->SetObjectField(TEXT("source_control"), MakeSourceControlObject(
		TEXT("not_applicable_read_only_first_slice"),
		{},
		{ TEXT("Slate/EUW test-flow readiness does not touch assets or source-control paths.") }));

	TArray<TSharedPtr<FJsonValue>> UiRows;
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("get_inspector_status"), MakeEmptyParams(), bExecuteReadOnly, true, Actions, UiRows, Errors);
	TSharedPtr<FJsonObject> WindowParams = MakeShared<FJsonObject>();
	WindowParams->SetBoolField(TEXT("include_titles"), true);
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("list_windows"), WindowParams, bExecuteReadOnly, false, Actions, UiRows, Warnings);
	if (bIncludeSnapshot)
	{
		TSharedPtr<FJsonObject> SnapshotParams = MakeShared<FJsonObject>();
		SnapshotParams->SetNumberField(TEXT("window_index"), -1);
		SnapshotParams->SetNumberField(TEXT("max_depth"), 8);
		SnapshotParams->SetNumberField(TEXT("max_widgets"), 200);
		SnapshotParams->SetBoolField(TEXT("include_hidden"), false);
		PlanOrExecutePrimitive(TEXT("slate"), TEXT("snapshot_widgets"), SnapshotParams, bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}
	TSharedPtr<FJsonObject> DescribeParams = MakeShared<FJsonObject>();
	if (!Ref.IsEmpty())
	{
		DescribeParams->SetStringField(TEXT("ref"), Ref);
	}
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("describe_widget"), DescribeParams, bExecuteReadOnly && !Ref.IsEmpty(), false, Actions, UiRows, Warnings);
	if (Ref.IsEmpty())
	{
		Warnings.Add(TEXT("No Slate ref supplied; describe_widget is planned only. Run slate.snapshot_widgets first and pass a returned ref for describe/capture proof."));
	}

	if (bIncludeWait)
	{
		TSharedPtr<FJsonObject> WaitParams = MakeShared<FJsonObject>();
		if (TargetKind.Equals(TEXT("type"), ESearchCase::IgnoreCase) || TargetKind.Equals(TEXT("widget"), ESearchCase::IgnoreCase))
		{
			WaitParams->SetStringField(TEXT("type"), Target);
		}
		else
		{
			WaitParams->SetStringField(TEXT("text_contains"), Target);
		}
		WaitParams->SetBoolField(TEXT("visible"), true);
		WaitParams->SetNumberField(TEXT("timeout_ms"), WaitTimeoutMs);
		WaitParams->SetNumberField(TEXT("poll_interval_ms"), 100);
		WaitParams->SetNumberField(TEXT("max_depth"), 12);
		PlanOrExecutePrimitive(TEXT("slate"), TEXT("wait_for_widget"), WaitParams, bExecuteReadOnly, false, Actions, UiRows, Warnings);
	}

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> UiValidation = MakeUnavailableProof(bExecuteReadOnly ? TEXT("checked_optional") : TEXT("planned"), TEXT("Slate inspector, window inventory, widget snapshot, and target description rows are in proof.read_back where available."));
	UiValidation->SetArrayField(TEXT("read_back"), UiRows);
	Validation->SetObjectField(TEXT("ui"), UiValidation);
	Validation->SetObjectField(TEXT("runtime"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("Slate/EUW editor UI readiness does not start PIE or runtime gameplay.")));
	TSharedPtr<FJsonObject> InteractionValidation = MakeUnavailableProof(
		bInteractionRequired ? TEXT("blocked") : TEXT("planned"),
		TEXT("Click/type/key simulation is not exposed by the current Slate namespace; this workflow keeps the limitation explicit."));
	CopyJsonField(Params, TEXT("interaction_plan"), InteractionValidation);
	Validation->SetObjectField(TEXT("interaction"), InteractionValidation);
	Validation->SetObjectField(TEXT("compile"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No Blueprint compile gate applies to Slate/EUW interaction readiness.")));
	Validation->SetObjectField(TEXT("asset_validation"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No asset validation runs unless a future EUW asset-specific slice is added.")));
	Validation->SetObjectField(TEXT("accessibility"), MakeUnavailableProof(TEXT("planned"), TEXT("Accessibility proof depends on the target widget surface and can be composed with ui audit actions for Widget Blueprints.")));
	Validation->SetObjectField(TEXT("budget"), MakeUnavailableProof(TEXT("not_applicable"), TEXT("No render/audio/material budget proof applies to Slate/EUW interaction readiness.")));
	Result->SetObjectField(TEXT("validation"), Validation);

	TArray<TSharedPtr<FJsonValue>> PreviewArtifacts;
	TArray<TSharedPtr<FJsonValue>> Artifacts;
	TSharedPtr<FJsonObject> CaptureParams = MakeShared<FJsonObject>();
	if (!Ref.IsEmpty())
	{
		CaptureParams->SetStringField(TEXT("ref"), Ref);
	}
	CaptureParams->SetNumberField(TEXT("max_bytes"), 1048576);
	if (!CaptureOutputPath.IsEmpty())
	{
		CaptureParams->SetStringField(TEXT("output_path"), CaptureOutputPath);
	}
	PlanOrExecutePrimitive(TEXT("slate"), TEXT("capture_widget"), CaptureParams, false, false, Actions, PreviewArtifacts, Warnings);
	if (bCaptureRequired)
	{
		TSharedPtr<FJsonObject> CaptureBlocker = MakeUnavailableProof(TEXT("blocked"), TEXT("Slate capture writes an artifact and must be run explicitly through slate.capture_widget."));
		CaptureBlocker->SetStringField(TEXT("next_action"), TEXT("slate.capture_widget"));
		CaptureBlocker->SetObjectField(TEXT("params"), CaptureParams);
		PreviewArtifacts.Add(MakeShared<FJsonValueObject>(CaptureBlocker));
		Artifacts.Add(MakeShared<FJsonValueObject>(CaptureBlocker));
		Errors.Add(TEXT("capture_required=true requested, but workflow.slate_euw_test_flow is read-only; call slate.capture_widget explicitly after reviewing proof."));
	}
	if (bInteractionRequired)
	{
		Errors.Add(TEXT("interaction_required=true requested, but click/type/key simulation is not exposed by the current Slate namespace; add a test-mode gated Slate input action before claiming interaction proof."));
	}

	ReadBack = Actions;
	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), PreviewArtifacts);
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), Artifacts);

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.snapshot_widgets"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("snapshot_widgets")), true, TEXT("Refresh widget refs before describe/capture/interaction planning."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.describe_widget"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("describe_widget")), true, TEXT("Describe the target widget after selecting an opaque ref."), DescribeParams)));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.wait_for_widget"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("wait_for_widget")), true, TEXT("Poll for the target widget using text/type matching."))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("slate.capture_widget"), FMonolithToolRegistry::Get().HasAction(TEXT("slate"), TEXT("capture_widget")), true, TEXT("Capture the reviewed widget/window explicitly."), CaptureParams)));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("No input event is sent and no editor widget is mutated by this first slice."),
		TEXT("Future click/type/key actions must be test-mode gated and must disclose any modal or focus side effects.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (!bDryRun && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Read-only Slate/EUW proof completed where actions were available; capture and input simulation remain explicit follow-ups or blockers."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}

FMonolithActionResult FMonolithWorkflowActions::HandleLevelWorldBuilderBlockout(const TSharedPtr<FJsonObject>& Params)
{
	FString MapPath;
	Params->TryGetStringField(TEXT("map_path"), MapPath);

	const TSharedPtr<FJsonObject>* VolumePtr = nullptr;
	Params->TryGetObjectField(TEXT("volume"), VolumePtr);
	TSharedPtr<FJsonObject> Volume = (VolumePtr && VolumePtr->IsValid()) ? *VolumePtr : MakeShared<FJsonObject>();

	double SeedValue = 0.0;
	Params->TryGetNumberField(TEXT("seed"), SeedValue);
	const int32 Seed = FMath::RoundToInt(SeedValue);
	if (Seed == 0)
	{
		return FMonolithActionResult::Error(TEXT("seed must be a non-zero integer for deterministic level workflow proof."), FMonolithJsonUtils::ErrInvalidParams);
	}

	FString VolumeName;
	Volume->TryGetStringField(TEXT("name"), VolumeName);
	FString RoomType;
	Volume->TryGetStringField(TEXT("room_type"), RoomType);
	if (VolumeName.IsEmpty() || RoomType.IsEmpty() || !Volume->HasField(TEXT("location")) || !Volume->HasField(TEXT("extent")))
	{
		return FMonolithActionResult::Error(TEXT("volume must include name, location, extent, and room_type before any mutation can be planned or applied."), FMonolithJsonUtils::ErrInvalidParams);
	}

	const TArray<TSharedPtr<FJsonValue>>* Primitives = nullptr;
	if (Params->TryGetArrayField(TEXT("primitives"), Primitives) && Primitives && Primitives->Num() > 200)
	{
		return FMonolithActionResult::Error(TEXT("primitives exceeds the hard cap of 200 entries."), FMonolithJsonUtils::ErrInvalidParams);
	}

	bool bDryRun = true;
	Params->TryGetBoolField(TEXT("dry_run"), bDryRun);
	bool bConfirm = false;
	Params->TryGetBoolField(TEXT("confirm"), bConfirm);
	bool bSave = false;
	Params->TryGetBoolField(TEXT("save"), bSave);
	bool bPrepareSourceControl = false;
	Params->TryGetBoolField(TEXT("prepare_source_control"), bPrepareSourceControl);
	const bool bExecute = !bDryRun && bConfirm;

	TArray<FString> Warnings;
	TArray<FString> Errors;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TArray<TSharedPtr<FJsonValue>> ReadBack;

	const TSharedPtr<FJsonObject>* ScatterPtr = nullptr;
	const bool bHasScatter = Params->TryGetObjectField(TEXT("scatter"), ScatterPtr) && ScatterPtr && ScatterPtr->IsValid();
	const TSharedPtr<FJsonObject>* AnalysisPtr = nullptr;
	const bool bHasAnalysis = Params->TryGetObjectField(TEXT("analysis"), AnalysisPtr) && AnalysisPtr && AnalysisPtr->IsValid();
	const TSharedPtr<FJsonObject>* CollectionPtr = nullptr;
	const bool bHasCollection = Params->TryGetObjectField(TEXT("collection"), CollectionPtr) && CollectionPtr && CollectionPtr->IsValid();

	TArray<FString> TouchedActors = { VolumeName };
	TArray<FString> TouchedAssets = { MapPath };
	if (bHasScatter)
	{
		TouchedAssets.Append(GetStringArrayField(*ScatterPtr, TEXT("asset_paths")));
	}
	TArray<FString> TouchedPackages = { MapPath };

	TSharedPtr<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("map_path"), MapPath);
	Input->SetObjectField(TEXT("volume"), Volume);
	Input->SetNumberField(TEXT("seed"), Seed);
	CopyJsonField(Params, TEXT("primitives"), Input);
	CopyJsonField(Params, TEXT("scatter"), Input);
	CopyJsonField(Params, TEXT("analysis"), Input);
	CopyJsonField(Params, TEXT("collection"), Input);
	Input->SetBoolField(TEXT("save"), bSave);
	Input->SetBoolField(TEXT("prepare_source_control"), bPrepareSourceControl);

	TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("workflow_id"), TEXT("level_workflow"));
	Result->SetStringField(TEXT("workflow_slice"), TEXT("blockout_volume_v1"));
	Result->SetBoolField(TEXT("dry_run"), bDryRun);
	Result->SetBoolField(TEXT("confirm"), bConfirm);
	Result->SetObjectField(TEXT("input"), Input);
	Result->SetObjectField(TEXT("plan"), MakeLevelPlanObject(bDryRun, bConfirm, bSave, bPrepareSourceControl));
	Result->SetObjectField(TEXT("touched"), MakeTouchedObject(TouchedActors, TouchedAssets, TouchedPackages, {}));
	Result->SetArrayField(TEXT("dirty_packages"), {});

	if (!bDryRun && !bConfirm)
	{
		Errors.Add(TEXT("dry_run=false requires confirm=true before editor.create_empty_map, editor.load_level, scene, worldgen, save, or source-control actions can run."));
	}

	PlanOrExecutePrimitive(TEXT("editor"), TEXT("list_dirty_packages"), MakeStringArrayParams(TEXT("scope_paths"), { TEXT("/Game") }), bExecute, true, Actions, ReadBack, Errors);

	TSharedPtr<FJsonObject> CreateMapParams = MakeActionParams(TEXT("path"), MapPath);
	CreateMapParams->SetStringField(TEXT("map_template"), TEXT("blank"));
	PlanOrExecutePrimitive(TEXT("editor"), TEXT("create_empty_map"), CreateMapParams, bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("editor"), TEXT("load_level"), MakeActionParams(TEXT("path"), MapPath), bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("get_world_context"), MakeEmptyParams(), bExecute, true, Actions, ReadBack, Errors);

	TSharedPtr<FJsonObject> SpawnVolumeParams = MakeShared<FJsonObject>();
	SpawnVolumeParams->SetStringField(TEXT("type"), TEXT("blocking"));
	CopyJsonField(Volume, TEXT("location"), SpawnVolumeParams);
	CopyJsonField(Volume, TEXT("extent"), SpawnVolumeParams);
	CopyJsonField(Volume, TEXT("rotation"), SpawnVolumeParams);
	SpawnVolumeParams->SetStringField(TEXT("name"), VolumeName);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("spawn_volume"), SpawnVolumeParams, bExecute, true, Actions, ReadBack, Errors);

	TSharedPtr<FJsonObject> SetupVolumeParams = MakeActionParams(TEXT("volume_name"), VolumeName);
	SetupVolumeParams->SetStringField(TEXT("room_type"), RoomType);
	CopyJsonField(Volume, TEXT("tags"), SetupVolumeParams);
	CopyJsonField(Volume, TEXT("density"), SetupVolumeParams);
	CopyJsonField(Volume, TEXT("floor_height"), SetupVolumeParams);
	SetupVolumeParams->SetBoolField(TEXT("allow_physics"), false);
	PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("setup_blockout_volume"), SetupVolumeParams, bExecute, true, Actions, ReadBack, Errors);

	if (Primitives && Primitives->Num() > 0)
	{
		TSharedPtr<FJsonObject> PrimitiveParams = MakeActionParams(TEXT("volume_name"), VolumeName);
		PrimitiveParams->SetArrayField(TEXT("primitives"), *Primitives);
		PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("create_blockout_primitives_batch"), PrimitiveParams, bExecute, true, Actions, ReadBack, Errors);
	}

	if (bHasScatter)
	{
		TSharedPtr<FJsonObject> ScatterParams = MakeShared<FJsonObject>();
		CopyJsonFields(*ScatterPtr, ScatterParams);
		ScatterParams->SetStringField(TEXT("volume_name"), VolumeName);
		ScatterParams->SetNumberField(TEXT("seed"), Seed);
		PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("scatter_props"), ScatterParams, bExecute, true, Actions, ReadBack, Errors);

		TSharedPtr<FJsonObject> SettleParams = MakeActionParams(TEXT("volume_name"), VolumeName);
		SettleParams->SetNumberField(TEXT("seed"), Seed);
		PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("settle_props"), SettleParams, bExecute, true, Actions, ReadBack, Errors);
	}

	PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("get_blockout_volume_info"), MakeActionParams(TEXT("volume_name"), VolumeName), bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("worldgen"), TEXT("export_blockout_layout"), MakeActionParams(TEXT("volume_name"), VolumeName), bExecute, true, Actions, ReadBack, Errors);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("get_scene_statistics"), MakeEmptyParams(), bExecute, true, Actions, ReadBack, Errors);
	TSharedPtr<FJsonObject> LevelActorsParams = MakeActionParams(TEXT("volume_name"), VolumeName);
	LevelActorsParams->SetNumberField(TEXT("limit"), 200);
	PlanOrExecutePrimitive(TEXT("scene"), TEXT("get_level_actors"), LevelActorsParams, bExecute, true, Actions, ReadBack, Errors);

	if (bHasAnalysis)
	{
		TSharedPtr<FJsonObject> Analysis = *AnalysisPtr;
		TSharedPtr<FJsonValue> SightlineLocation = Analysis->TryGetField(TEXT("sightline_location"));
		if (SightlineLocation.IsValid())
		{
			TSharedPtr<FJsonObject> SightlineParams = MakeShared<FJsonObject>();
			SightlineParams->SetField(TEXT("location"), SightlineLocation);
			CopyJsonField(Analysis, TEXT("forward"), SightlineParams);
			CopyJsonField(Analysis, TEXT("fov"), SightlineParams);
			CopyJsonField(Analysis, TEXT("ray_count"), SightlineParams);
			CopyJsonField(Analysis, TEXT("max_distance"), SightlineParams);
			PlanOrExecutePrimitive(TEXT("leveldesign"), TEXT("analyze_sightlines"), SightlineParams, bExecute, true, Actions, ReadBack, Errors);
		}

		bool bRoomAcoustics = false;
		Analysis->TryGetBoolField(TEXT("room_acoustics"), bRoomAcoustics);
		if (bRoomAcoustics)
		{
			TSharedPtr<FJsonObject> AcousticsParams = MakeActionParams(TEXT("volume_name"), VolumeName);
			CopyJsonField(Analysis, TEXT("ray_count"), AcousticsParams);
			PlanOrExecutePrimitive(TEXT("leveldesign"), TEXT("analyze_room_acoustics"), AcousticsParams, bExecute, true, Actions, ReadBack, Errors);
		}
	}

	if (bHasCollection)
	{
		FString CollectionName;
		(*CollectionPtr)->TryGetStringField(TEXT("name"), CollectionName);
		if (!CollectionName.IsEmpty())
		{
			FString ShareType = TEXT("local");
			(*CollectionPtr)->TryGetStringField(TEXT("share_type"), ShareType);

			TSharedPtr<FJsonObject> CollectionParams = MakeActionParams(TEXT("name"), CollectionName);
			CollectionParams->SetStringField(TEXT("share_type"), ShareType);
			CollectionParams->SetStringField(TEXT("storage_mode"), TEXT("static"));
			PlanOrExecutePrimitive(TEXT("collection"), TEXT("create_collection"), CollectionParams, bExecute, true, Actions, ReadBack, Errors);

			TSharedPtr<FJsonObject> AddAssetsParams = MakeActionParams(TEXT("name"), CollectionName);
			AddAssetsParams->SetStringField(TEXT("share_type"), ShareType);
			AddAssetsParams->SetArrayField(TEXT("asset_paths"), StringsToJson({ MapPath }));
			PlanOrExecutePrimitive(TEXT("collection"), TEXT("add_assets"), AddAssetsParams, bExecute, true, Actions, ReadBack, Errors);

			TSharedPtr<FJsonObject> ListAssetsParams = MakeActionParams(TEXT("name"), CollectionName);
			ListAssetsParams->SetStringField(TEXT("share_type"), ShareType);
			ListAssetsParams->SetStringField(TEXT("recursive"), TEXT("self"));
			PlanOrExecutePrimitive(TEXT("collection"), TEXT("list_assets"), ListAssetsParams, bExecute, true, Actions, ReadBack, Errors);
		}
	}

	TSharedPtr<FJsonObject> SaveValidation = MakeUnavailableProof(bSave ? TEXT("planned") : TEXT("not_requested"), bSave ? TEXT("Save is explicitly requested and scoped to map_path.") : TEXT("save=false."));
	if (bSave)
	{
		TSharedPtr<FJsonObject> SaveParams = MakeStringArrayParams(TEXT("packages"), { MapPath });
		SaveParams->SetBoolField(TEXT("fail_on_unrequested_dirty"), true);
		SaveParams->SetArrayField(TEXT("scope_paths"), StringsToJson({ MapPath }));
		SaveParams->SetBoolField(TEXT("dry_run"), bDryRun || !bConfirm);
		PlanOrExecutePrimitive(TEXT("editor"), TEXT("save_packages"), SaveParams, bExecute, true, Actions, ReadBack, Errors);
		SaveValidation->SetStringField(TEXT("status"), bExecute ? TEXT("attempted") : TEXT("planned"));
		SaveValidation->SetArrayField(TEXT("dirty_packages_before"), {});
		SaveValidation->SetArrayField(TEXT("dirty_packages_after"), {});
	}

	TSharedPtr<FJsonObject> SourceControl = MakeSourceControlObject(
		bPrepareSourceControl ? TEXT("planned") : TEXT("not_requested"),
		{ MapPath },
		bPrepareSourceControl ? TArray<FString>{ TEXT("source_control.checkout_or_add runs only with confirm=true.") } : TArray<FString>{});
	if (bPrepareSourceControl)
	{
		PlanOrExecutePrimitive(TEXT("source_control"), TEXT("get_capabilities"), MakeEmptyParams(), bExecute, true, Actions, ReadBack, Errors);
		PlanOrExecutePrimitive(TEXT("source_control"), TEXT("get_status"), MakeStringArrayParams(TEXT("paths"), { MapPath }), bExecute, true, Actions, ReadBack, Errors);
		TSharedPtr<FJsonObject> CheckoutParams = MakeStringArrayParams(TEXT("paths"), { MapPath });
		CheckoutParams->SetBoolField(TEXT("dry_run"), bDryRun || !bConfirm);
		PlanOrExecutePrimitive(TEXT("source_control"), TEXT("checkout_or_add"), CheckoutParams, bExecute, true, Actions, ReadBack, Errors);
		SourceControl->SetStringField(TEXT("status"), bExecute ? TEXT("attempted") : TEXT("planned"));
	}
	Result->SetObjectField(TEXT("source_control"), SourceControl);

	TSharedPtr<FJsonObject> Validation = MakeShared<FJsonObject>();
	Validation->SetObjectField(TEXT("world_context"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), TEXT("See proof.read_back for scene.get_world_context.")));
	Validation->SetObjectField(TEXT("scene_statistics"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), TEXT("See proof.read_back for scene.get_scene_statistics and scene.get_level_actors.")));
	Validation->SetObjectField(TEXT("blockout"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), TEXT("See proof.read_back for worldgen blockout rows.")));
	Validation->SetObjectField(TEXT("leveldesign"), MakeUnavailableProof(bExecute ? TEXT("attempted") : TEXT("planned"), bHasAnalysis ? TEXT("Optional leveldesign rows are included in proof.read_back.") : TEXT("analysis not requested.")));
	Validation->SetObjectField(TEXT("save"), SaveValidation);
	Result->SetObjectField(TEXT("validation"), Validation);

	TSharedPtr<FJsonObject> Proof = MakeShared<FJsonObject>();
	Proof->SetArrayField(TEXT("read_back"), ReadBack);
	Proof->SetArrayField(TEXT("preview_artifacts"), {});
	Proof->SetArrayField(TEXT("logs"), {});
	Proof->SetArrayField(TEXT("benchmarks"), {});
	Proof->SetStringField(TEXT("preview_note"), TEXT("No preview capture is performed by this blockout first slice."));
	Result->SetObjectField(TEXT("proof"), Proof);
	Result->SetArrayField(TEXT("actions"), Actions);
	Result->SetArrayField(TEXT("artifacts"), {});

	TArray<TSharedPtr<FJsonValue>> NextActions;
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.list_dirty_packages"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("list_dirty_packages")), true, TEXT("Audit dirty state before and after the map workflow."), MakeStringArrayParams(TEXT("scope_paths"), { TEXT("/Game") }))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("editor.save_packages"), FMonolithToolRegistry::Get().HasAction(TEXT("editor"), TEXT("save_packages")), true, TEXT("Save the requested map package after reviewing proof."), MakeStringArrayParams(TEXT("packages"), { MapPath }))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("source_control.checkout_or_add"), FMonolithToolRegistry::Get().HasAction(TEXT("source_control"), TEXT("checkout_or_add")), true, TEXT("Prepare the map package path through source control."), MakeStringArrayParams(TEXT("paths"), { MapPath }))));
	NextActions.Add(MakeShared<FJsonValueObject>(MakeNextAction(TEXT("leveldesign.analyze_sightlines"), FMonolithToolRegistry::Get().HasAction(TEXT("leveldesign"), TEXT("analyze_sightlines")), true, TEXT("Run or repeat sightline analysis for the blockout."))));
	Result->SetArrayField(TEXT("next_actions"), NextActions);

	TSharedPtr<FJsonObject> Rollback = MakeShared<FJsonObject>();
	Rollback->SetBoolField(TEXT("automatic"), false);
	Rollback->SetArrayField(TEXT("limitations"), StringsToJson({
		TEXT("Created map/package deletion is manual or source-control revert/delete."),
		TEXT("Scene actors are created through child actions and must be reverted through editor undo or explicit delete/revert workflows.")
	}));
	Result->SetObjectField(TEXT("rollback"), Rollback);

	Result->SetStringField(TEXT("status"), Errors.Num() > 0 ? TEXT("blocked") : (bDryRun ? TEXT("planned") : TEXT("partial")));
	if (bDryRun)
	{
		Warnings.Add(TEXT("dry_run=true returned the mutation plan only; no child action executed."));
	}
	else if (bExecute && Errors.Num() == 0)
	{
		Warnings.Add(TEXT("Level blockout workflow executed child actions where available; inspect dirty_packages/source_control before committing."));
	}
	Result->SetArrayField(TEXT("warnings"), StringsToJson(Warnings));
	Result->SetArrayField(TEXT("errors"), StringsToJson(Errors));
	return FMonolithActionResult::Success(Result);
}
