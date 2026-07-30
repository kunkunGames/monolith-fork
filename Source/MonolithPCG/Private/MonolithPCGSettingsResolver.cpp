#include "MonolithPCGSettingsResolver.h"

#include "PCGSettings.h"

#include "UObject/UObjectHash.h"

namespace
{
void AddAlias(TSet<FString>& Aliases, const FString& Value)
{
	FString Trimmed = Value;
	Trimmed.TrimStartAndEndInline();
	if (!Trimmed.IsEmpty())
	{
		Aliases.Add(Trimmed);
	}
}

TSet<FString> BuildAliases(const FMonolithPCGSettingsTypeInfo& Type)
{
	TSet<FString> Aliases;
	AddAlias(Aliases, Type.ClassName);
	AddAlias(Aliases, TEXT("U") + Type.ClassName);
	AddAlias(Aliases, Type.ClassPath);
	AddAlias(Aliases, Type.FriendlyName);
	AddAlias(Aliases, Type.DefaultNodeTitle);
	return Aliases;
}
} // namespace

FString FMonolithPCGSettingsResolver::Canonicalize(const FString& Value)
{
	FString Canonical;
	Canonical.Reserve(Value.Len());
	for (const TCHAR Character : Value)
	{
		if (FChar::IsAlnum(Character))
		{
			Canonical.AppendChar(FChar::ToLower(Character));
		}
	}
	return Canonical;
}

bool FMonolithPCGSettingsResolver::IsUsableSettingsClass(const UClass* Candidate)
{
	if (!Candidate || !Candidate->IsChildOf(UPCGSettings::StaticClass()) || Candidate == UPCGSettings::StaticClass() ||
		Candidate->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_Hidden))
	{
		return false;
	}

	const UPCGSettings* DefaultSettings = Candidate->GetDefaultObject<UPCGSettings>();
	return DefaultSettings && DefaultSettings->bExposeToLibrary && !DefaultSettings->OnlyExposePreconfiguredSettings();
}

FString FMonolithPCGSettingsResolver::MakeFriendlyName(const UClass* SettingsClass)
{
	if (!SettingsClass)
	{
		return FString();
	}

	FString FriendlyName = SettingsClass->GetName();
	FriendlyName.RemoveFromStart(TEXT("PCG"), ESearchCase::CaseSensitive);
	FriendlyName.RemoveFromEnd(TEXT("Settings"), ESearchCase::CaseSensitive);
	return FriendlyName.IsEmpty() ? SettingsClass->GetName() : FriendlyName;
}

TArray<FMonolithPCGSettingsTypeInfo> FMonolithPCGSettingsResolver::ListTypes()
{
	TArray<UClass*> DerivedClasses;
	GetDerivedClasses(UPCGSettings::StaticClass(), DerivedClasses, true);

	TArray<FMonolithPCGSettingsTypeInfo> Types;
	Types.Reserve(DerivedClasses.Num());
	for (UClass* SettingsClass : DerivedClasses)
	{
		if (!IsUsableSettingsClass(SettingsClass))
		{
			continue;
		}

		FMonolithPCGSettingsTypeInfo& Type = Types.AddDefaulted_GetRef();
		Type.SettingsClass = SettingsClass;
		Type.ClassName = SettingsClass->GetName();
		Type.ClassPath = SettingsClass->GetClassPathName().ToString();
		Type.FriendlyName = MakeFriendlyName(SettingsClass);

#if WITH_EDITOR
		if (const UPCGSettings* DefaultSettings = SettingsClass->GetDefaultObject<UPCGSettings>())
		{
			Type.DefaultNodeTitle = DefaultSettings->GetDefaultNodeTitle().ToString();
		}
#endif
	}

	Types.Sort(
		[](const FMonolithPCGSettingsTypeInfo& A, const FMonolithPCGSettingsTypeInfo& B)
		{
			const int32 FriendlyCompare = A.FriendlyName.Compare(B.FriendlyName, ESearchCase::IgnoreCase);
			return FriendlyCompare == 0 ? A.ClassPath < B.ClassPath : FriendlyCompare < 0;
		});
	return Types;
}

UClass* FMonolithPCGSettingsResolver::Resolve(const FString& TypeToken, TArray<FString>& OutCandidates,
											  FString& OutError)
{
	OutCandidates.Reset();
	OutError.Reset();

	FString TrimmedToken = TypeToken;
	TrimmedToken.TrimStartAndEndInline();
	if (TrimmedToken.IsEmpty())
	{
		OutError = TEXT("node_type must not be empty");
		return nullptr;
	}

	if (TrimmedToken.StartsWith(TEXT("/Script/")))
	{
		if (UClass* ExactPathClass = FindObject<UClass>(nullptr, *TrimmedToken))
		{
			if (IsUsableSettingsClass(ExactPathClass))
			{
				return ExactPathClass;
			}
		}
	}

	const TArray<FMonolithPCGSettingsTypeInfo> Types = ListTypes();

	// An exact class path is unambiguous by construction, so it still resolves
	// immediately.
	for (const FMonolithPCGSettingsTypeInfo& Type : Types)
	{
		if (Type.ClassPath.Equals(TrimmedToken, ESearchCase::IgnoreCase))
		{
			return Type.SettingsClass;
		}
	}

	// A short class name is not unique: two loaded modules can each define a
	// UPCGSettings subclass with the same name. Returning the first sorted match
	// created a node of the wrong project/plugin class, so collect every
	// short-name match and only resolve when exactly one exists. Otherwise fall
	// through to the ambiguity reporting below, which requires an exact path.
	TArray<UClass*> ShortNameMatches;
	for (const FMonolithPCGSettingsTypeInfo& Type : Types)
	{
		if (Type.ClassName.Equals(TrimmedToken, ESearchCase::IgnoreCase) ||
			(TEXT("U") + Type.ClassName).Equals(TrimmedToken, ESearchCase::IgnoreCase))
		{
			ShortNameMatches.AddUnique(Type.SettingsClass);
		}
	}
	if (ShortNameMatches.Num() == 1)
	{
		return ShortNameMatches[0];
	}
	if (ShortNameMatches.Num() > 1)
	{
		for (const UClass* Match : ShortNameMatches)
		{
			OutCandidates.AddUnique(Match->GetClassPathName().ToString());
		}
		OutCandidates.Sort();
		return nullptr;
	}

	const FString CanonicalToken = Canonicalize(TrimmedToken);
	TArray<UClass*> Matches;
	for (const FMonolithPCGSettingsTypeInfo& Type : Types)
	{
		const TSet<FString> Aliases = BuildAliases(Type);
		for (const FString& Alias : Aliases)
		{
			if (Canonicalize(Alias) == CanonicalToken)
			{
				Matches.AddUnique(Type.SettingsClass);
				break;
			}
		}
	}

	for (const UClass* Match : Matches)
	{
		OutCandidates.Add(Match->GetClassPathName().ToString());
	}
	OutCandidates.Sort();

	if (Matches.Num() == 1)
	{
		return Matches[0];
	}
	if (Matches.IsEmpty())
	{
		OutError = FString::Printf(TEXT("PCG settings class not found for node_type '%s'"), *TrimmedToken);
	}
	else
	{
		OutError = FString::Printf(TEXT("node_type '%s' is ambiguous; use an exact "
										"class name or /Script class path"),
								   *TrimmedToken);
	}
	return nullptr;
}
