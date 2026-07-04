// SPDX-License-Identifier: MIT
// FMonolithDryRunGuard implementation. This file only handles flag parsing and
// report serialization; write prevention remains the caller's branch contract.

#include "Reflection/MonolithDryRunGuard.h"
#include "MonolithToolRegistry.h"
#include "MonolithJsonUtils.h"
#include "Dom/JsonObject.h"

FMonolithDryRunGuard::FMonolithDryRunGuard(const TSharedPtr<FJsonObject>& Params)
{
	if (!Params.IsValid())
	{
		return;
	}
	TSharedPtr<FJsonValue> DryrunField = Params->TryGetField(TEXT("dry_run"));
	if (DryrunField.IsValid() && !DryrunField->TryGetBool(bDryRun))
	{
		bHasParseError = true;
		ParseErrorMsg = TEXT("Parameter 'dry_run' must be a boolean");
		return;
	}
	TSharedPtr<FJsonValue> StrictField = Params->TryGetField(TEXT("strict"));
	if (StrictField.IsValid() && !StrictField->TryGetBool(bStrict))
	{
		bHasParseError = true;
		ParseErrorMsg = TEXT("Parameter 'strict' must be a boolean");
		return;
	}
}

TSharedPtr<FJsonObject> FMonolithDryRunGuard::ReportToJson(const FDryRunReport& Report)
{
	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();

	// field_writes
	{
		TArray<TSharedPtr<FJsonValue>> Writes;
		Writes.Reserve(Report.FieldWrites.Num());
		for (const FBulkFillFieldWrite& W : Report.FieldWrites)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("path"), W.Path);
			O->SetStringField(TEXT("current"), W.CurrentValue);
			O->SetStringField(TEXT("proposed"), W.ProposedValue);
			O->SetBoolField(TEXT("ok"), W.bOk);
			if (!W.bOk)
			{
				O->SetStringField(TEXT("reason"), W.Reason);
			}
			Writes.Add(MakeShared<FJsonValueObject>(O));
		}
		Root->SetArrayField(TEXT("field_writes"), Writes);
	}

	// silent_drops (same shape as field_writes — adapters annotate these).
	{
		TArray<TSharedPtr<FJsonValue>> Drops;
		Drops.Reserve(Report.SilentDrops.Num());
		for (const FBulkFillFieldWrite& W : Report.SilentDrops)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("path"), W.Path);
			O->SetStringField(TEXT("current"), W.CurrentValue);
			O->SetStringField(TEXT("proposed"), W.ProposedValue);
			O->SetBoolField(TEXT("ok"), W.bOk);
			O->SetStringField(TEXT("reason"), W.Reason);
			Drops.Add(MakeShared<FJsonValueObject>(O));
		}
		Root->SetArrayField(TEXT("silent_drops"), Drops);
	}

	// would_create / would_modify (string-array convenience surface).
	{
		TArray<TSharedPtr<FJsonValue>> Creates;
		Creates.Reserve(Report.WouldCreate.Num());
		for (const FString& S : Report.WouldCreate) { Creates.Add(MakeShared<FJsonValueString>(S)); }
		Root->SetArrayField(TEXT("would_create"), Creates);

		TArray<TSharedPtr<FJsonValue>> Mods;
		Mods.Reserve(Report.WouldModify.Num());
		for (const FString& S : Report.WouldModify) { Mods.Add(MakeShared<FJsonValueString>(S)); }
		Root->SetArrayField(TEXT("would_modify"), Mods);
	}

	Root->SetBoolField(TEXT("would_apply"), Report.bWouldApply);
	Root->SetNumberField(TEXT("errors"), Report.Errors);
	return Root;
}

FMonolithActionResult FMonolithDryRunGuard::MakeDryRunResponse(const FDryRunReport& Report) const
{
	return FMonolithActionResult::Success(ReportToJson(Report));
}
