// SPDX-License-Identifier: MIT
// Shared domain-free helpers for Monolith bulk-fill adapters.

#pragma once

#include "CoreMinimal.h"
#include "MonolithBulkFillTypes.h"

class FMonolithBulkFillReportUtils
{
public:
	static FBulkFillFieldWrite MakeFieldWrite(
		const FString& Path,
		const FString& ProposedValue,
		bool bOk,
		const FString& Reason = FString())
	{
		FBulkFillFieldWrite Write;
		Write.Path = Path;
		Write.ProposedValue = ProposedValue;
		Write.bOk = bOk;
		Write.Reason = Reason;
		return Write;
	}

	static void AddFieldWrite(
		FDryRunReport& Report,
		const FString& Path,
		const FString& ProposedValue,
		bool bOk,
		const FString& Reason = FString())
	{
		Report.FieldWrites.Add(MakeFieldWrite(Path, ProposedValue, bOk, Reason));
		if (!bOk)
		{
			++Report.Errors;
		}
	}

	static FDryRunReport MakeFailureReport(
		const FString& Reason,
		const FString& Path = TEXT("(adapter)"))
	{
		FDryRunReport Report;
		AddFieldWrite(Report, Path, FString(), false, Reason);
		Report.bWouldApply = false;
		return Report;
	}
};

class FMonolithBulkFillJsonUtils
{
public:
	static bool TryGetRequiredFillKind(
		const FBulkFillSpec& Spec,
		const FString& AdapterName,
		const FString& SupportedKindsText,
		FString& OutFillKind,
		FDryRunReport& OutFailureReport)
	{
		if (!Spec.Tree.IsValid())
		{
			OutFailureReport = FMonolithBulkFillReportUtils::MakeFailureReport(
				FString::Printf(TEXT("%s adapter: spec.tree is null"), *AdapterName));
			return false;
		}

		Spec.Tree->TryGetStringField(TEXT("fill_kind"), OutFillKind);
		if (OutFillKind.IsEmpty())
		{
			OutFailureReport = FMonolithBulkFillReportUtils::MakeFailureReport(
				FString::Printf(TEXT("%s adapter: spec.tree.fill_kind required - one of %s"),
					*AdapterName,
					*SupportedKindsText));
			return false;
		}
		return true;
	}
};

class FMonolithBulkFillDescriptorUtils
{
public:
	static FSchemaDescriptor MakeNamespaceRoot(
		const FString& FieldPath,
		const FString& TypeName,
		const FString& ImportTextForm)
	{
		FSchemaDescriptor Root;
		Root.FieldPath = FieldPath;
		Root.TypeName = TypeName;
		Root.ImportTextForm = ImportTextForm;
		return Root;
	}

	static FSchemaDescriptor MakeFillKind(
		const FString& FieldPath,
		const FString& ImportTextForm,
		const FString& TypeName = TEXT("fill_kind"))
	{
		FSchemaDescriptor Kind;
		Kind.FieldPath = FieldPath;
		Kind.TypeName = TypeName;
		Kind.ImportTextForm = ImportTextForm;
		return Kind;
	}

	static FSchemaDescriptor MakeDocNote(const FString& Note)
	{
		FSchemaDescriptor Doc;
		Doc.FieldPath = TEXT("(note)");
		Doc.TypeName = TEXT("doc");
		Doc.ImportTextForm = Note;
		return Doc;
	}
};
