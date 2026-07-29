#include "MonolithGameplayMessageCommon.h"

#include "GameplayTagContainer.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/UObjectGlobals.h"

namespace MonolithGameplayMessage
{
	namespace
	{
		bool HasBoundaryPrefix(const FString& Path, const FString& Directory)
		{
			if (Path.Equals(Directory, ESearchCase::IgnoreCase))
			{
				return true;
			}

			FString Prefix = Directory;
			if (!Prefix.EndsWith(TEXT("/")))
			{
				Prefix += TEXT("/");
			}
			return Path.StartsWith(Prefix, ESearchCase::IgnoreCase);
		}

		bool ResolveCanonicalDirectory(
			const FString& Input,
			const FString& AllowedDirectory,
			FString& OutResolvedRoot,
			FString& OutError)
		{
			OutResolvedRoot.Reset();
			OutError.Reset();

			if (Input.IsEmpty())
			{
				OutError = TEXT("Source root must not be empty");
				return false;
			}
			if (!Input.Equals(Input.TrimStartAndEnd(), ESearchCase::CaseSensitive))
			{
				OutError = TEXT("Source root must not contain leading or trailing whitespace");
				return false;
			}
			if (Input.Contains(TEXT("\\")))
			{
				OutError = TEXT("Source root must use forward slashes");
				return false;
			}

			FString Resolved = Input;
			if (FPaths::IsRelative(Resolved))
			{
				Resolved = FPaths::Combine(FPaths::ProjectDir(), Resolved);
			}
			Resolved = FPaths::ConvertRelativePathToFull(Resolved);
			FPaths::CollapseRelativeDirectories(Resolved);
			FPaths::NormalizeDirectoryName(Resolved);

			FString Allowed = FPaths::ConvertRelativePathToFull(AllowedDirectory);
			FPaths::CollapseRelativeDirectories(Allowed);
			FPaths::NormalizeDirectoryName(Allowed);

			if (!HasBoundaryPrefix(Resolved, Allowed))
			{
				OutError = FString::Printf(
					TEXT("Source root '%s' resolves outside the allowed directory '%s'"),
					*Input,
					*Allowed);
				return false;
			}
			if (!FPaths::DirectoryExists(Resolved))
			{
				OutError = FString::Printf(TEXT("Source root does not exist: %s"), *Resolved);
				return false;
			}

			OutResolvedRoot = MoveTemp(Resolved);
			return true;
		}
	}

	FStrictParamReader::FStrictParamReader(const TSharedPtr<FJsonObject>& InParams)
		: Params(InParams.IsValid() ? InParams : MakeShared<FJsonObject>())
	{
	}

	bool FStrictParamReader::SetError(const FString& InError)
	{
		if (Error.IsEmpty())
		{
			Error = InError;
		}
		return false;
	}

	bool FStrictParamReader::ReadExactString(
		const TCHAR* FieldName,
		bool bRequired,
		FString& OutValue,
		const FString& DefaultValue)
	{
		if (!Params->HasField(FieldName))
		{
			if (bRequired)
			{
				return SetError(FString::Printf(TEXT("Missing required param '%s'"), FieldName));
			}
			OutValue = DefaultValue;
			return true;
		}

		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::String
			|| !(*FieldValue)->TryGetString(OutValue))
		{
			return SetError(FString::Printf(TEXT("Param '%s' must be a string"), FieldName));
		}
		if (!OutValue.Equals(OutValue.TrimStartAndEnd(), ESearchCase::CaseSensitive))
		{
			return SetError(FString::Printf(
				TEXT("Param '%s' must not contain leading or trailing whitespace"),
				FieldName));
		}
		if (bRequired && OutValue.IsEmpty())
		{
			return SetError(FString::Printf(TEXT("Param '%s' must not be empty"), FieldName));
		}
		return true;
	}

	bool FStrictParamReader::RequiredString(const TCHAR* FieldName, FString& OutValue)
	{
		return ReadExactString(FieldName, true, OutValue, FString());
	}

	bool FStrictParamReader::OptionalString(
		const TCHAR* FieldName,
		FString& OutValue,
		const FString& DefaultValue)
	{
		return ReadExactString(FieldName, false, OutValue, DefaultValue);
	}

	bool FStrictParamReader::OptionalBool(const TCHAR* FieldName, bool& OutValue, bool DefaultValue)
	{
		if (!Params->HasField(FieldName))
		{
			OutValue = DefaultValue;
			return true;
		}
		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::Boolean
			|| !(*FieldValue)->TryGetBool(OutValue))
		{
			return SetError(FString::Printf(TEXT("Param '%s' must be a boolean"), FieldName));
		}
		return true;
	}

	bool FStrictParamReader::OptionalInt(
		const TCHAR* FieldName,
		int32& OutValue,
		int32 DefaultValue,
		int32 MinValue,
		int32 MaxValue)
	{
		if (!Params->HasField(FieldName))
		{
			OutValue = DefaultValue;
			return true;
		}

		double Number = 0.0;
		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::Number
			|| !(*FieldValue)->TryGetNumber(Number)
			|| !FMath::IsFinite(Number)
			|| FMath::TruncToDouble(Number) != Number
			|| Number < static_cast<double>(MinValue)
			|| Number > static_cast<double>(MaxValue))
		{
			return SetError(FString::Printf(
				TEXT("Param '%s' must be an integer in range %d..%d"),
				FieldName,
				MinValue,
				MaxValue));
		}

		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool FStrictParamReader::OptionalStringArray(
		const TCHAR* FieldName,
		TArray<FString>& OutValues,
		int32 MaxValues)
	{
		OutValues.Reset();
		if (!Params->HasField(FieldName))
		{
			return true;
		}

		const TSharedPtr<FJsonValue>* FieldValue = Params->Values.Find(FieldName);
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!FieldValue
			|| !FieldValue->IsValid()
			|| (*FieldValue)->Type != EJson::Array
			|| !(*FieldValue)->TryGetArray(Values)
			|| !Values)
		{
			return SetError(FString::Printf(TEXT("Param '%s' must be an array of strings"), FieldName));
		}
		if (Values->Num() > MaxValues)
		{
			return SetError(FString::Printf(
				TEXT("Param '%s' may contain at most %d entries"),
				FieldName,
				MaxValues));
		}

		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			FString Text;
			if (!Value.IsValid()
				|| Value->Type != EJson::String
				|| !Value->TryGetString(Text)
				|| Text.IsEmpty()
				|| !Text.Equals(Text.TrimStartAndEnd(), ESearchCase::CaseSensitive))
			{
				return SetError(FString::Printf(
					TEXT("Param '%s' must contain only non-empty exact strings"),
					FieldName));
			}
			OutValues.Add(Text);
		}
		return true;
	}

	FExactObjectLoad LoadExactObjectPath(const FString& ObjectPath)
	{
		FExactObjectLoad Result;
		Result.RequestedPath = ObjectPath;

		if (ObjectPath.IsEmpty())
		{
			Result.ErrorCode = TEXT("empty_object_path");
			Result.ErrorDetail = TEXT("Object path must not be empty");
			return Result;
		}
		if (!ObjectPath.Equals(ObjectPath.TrimStartAndEnd(), ESearchCase::CaseSensitive))
		{
			Result.ErrorCode = TEXT("object_path_whitespace");
			Result.ErrorDetail = TEXT("Object path must not contain leading or trailing whitespace");
			return Result;
		}
		if (ObjectPath.Contains(TEXT("\\")) || ObjectPath.Contains(TEXT(":")))
		{
			Result.ErrorCode = TEXT("object_path_noncanonical");
			Result.ErrorDetail = TEXT("Object path must not contain backslashes or a subobject delimiter");
			return Result;
		}
		if (ObjectPath.EndsWith(TEXT(".uasset"), ESearchCase::IgnoreCase)
			|| ObjectPath.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
		{
			Result.ErrorCode = TEXT("object_path_extension");
			Result.ErrorDetail = TEXT("Object path must not contain a package file extension");
			return Result;
		}

		FText InvalidReason;
		if (!FPackageName::IsValidObjectPath(ObjectPath, &InvalidReason))
		{
			Result.ErrorCode = TEXT("invalid_object_path");
			Result.ErrorDetail = InvalidReason.ToString();
			return Result;
		}

		Result.Object = StaticLoadObject(
			UObject::StaticClass(),
			nullptr,
			*ObjectPath,
			nullptr,
			LOAD_NoWarn | LOAD_NoRedirects);
		if (!Result.Object)
		{
			Result.ErrorCode = TEXT("object_not_found");
			Result.ErrorDetail = FString::Printf(TEXT("Object not found at exact path '%s'"), *ObjectPath);
			return Result;
		}

		Result.ResolvedPath = Result.Object->GetPathName();
		if (!Result.ResolvedPath.Equals(ObjectPath, ESearchCase::CaseSensitive))
		{
			Result.ErrorCode = TEXT("object_path_mismatch");
			Result.ErrorDetail = FString::Printf(
				TEXT("Loaded object path '%s' does not exactly match requested path '%s'"),
				*Result.ResolvedPath,
				*ObjectPath);
			Result.Object = nullptr;
			return Result;
		}
		if (Result.Object->IsA<UObjectRedirector>())
		{
			Result.ErrorCode = TEXT("redirector_rejected");
			Result.ErrorDetail = FString::Printf(TEXT("Object path resolves to a redirector: %s"), *ObjectPath);
			Result.Object = nullptr;
			return Result;
		}

		return Result;
	}

	bool IsCanonicalGameplayTagString(const FString& TagString, FString& OutError)
	{
		FText EngineError;
		if (!FGameplayTag::IsValidGameplayTagString(TagString, &EngineError))
		{
			OutError = EngineError.ToString();
			return false;
		}

		for (const TCHAR Character : TagString)
		{
			if (FChar::IsWhitespace(Character))
			{
				OutError = TEXT("Gameplay tags must not contain whitespace");
				return false;
			}
		}

		if (TagString.Contains(TEXT(".."), ESearchCase::CaseSensitive))
		{
			OutError = TEXT("Gameplay tags must not contain empty dot-delimited segments");
			return false;
		}

		return true;
	}

	bool ResolveProjectSourceRoot(const FString& Input, FString& OutResolvedRoot, FString& OutError)
	{
		FString Resolved;
		if (!ResolveCanonicalDirectory(Input, FPaths::ProjectDir(), Resolved, OutError))
		{
			return false;
		}

		FString ProjectSource = FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), TEXT("Source")));
		FString ProjectPlugins = FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir());
		FPaths::CollapseRelativeDirectories(ProjectSource);
		FPaths::CollapseRelativeDirectories(ProjectPlugins);
		FPaths::NormalizeDirectoryName(ProjectSource);
		FPaths::NormalizeDirectoryName(ProjectPlugins);

		if (!HasBoundaryPrefix(Resolved, ProjectSource)
			&& !HasBoundaryPrefix(Resolved, ProjectPlugins))
		{
			OutError = FString::Printf(
				TEXT("Source root '%s' must resolve under the project's Source or Plugins directory"),
				*Input);
			return false;
		}

		OutResolvedRoot = MoveTemp(Resolved);
		return true;
	}

	bool ResolveEngineGameplayMessageSourceRoot(FString& OutResolvedRoot, FString& OutError)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("GameplayMessageRouter"));
		if (!Plugin.IsValid())
		{
			OutError = TEXT("GameplayMessageRouter plugin is not installed");
			return false;
		}

		const FString SourceDirectory = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Source"));
		return ResolveCanonicalDirectory(SourceDirectory, Plugin->GetBaseDir(), OutResolvedRoot, OutError);
	}

	bool IsMonolithSourcePath(const FString& Path)
	{
		FString Normalized = Path;
		FPaths::NormalizeFilename(Normalized);
		return Normalized.Contains(TEXT("/Plugins/Monolith/Source/"), ESearchCase::IgnoreCase);
	}

	bool HasSupportedSourceExtension(const FString& File)
	{
		const FString Extension = FPaths::GetExtension(File, false).ToLower();
		return Extension == TEXT("cpp")
			|| Extension == TEXT("h")
			|| Extension == TEXT("hpp")
			|| Extension == TEXT("inl");
	}

	FString MakeProjectRelativePath(const FString& File)
	{
		FString Display = File;
		if (FPaths::MakePathRelativeTo(Display, *FPaths::ProjectDir()))
		{
			FPaths::NormalizeFilename(Display);
			return BoundText(Display, 1024);
		}

		FPaths::NormalizeFilename(Display);
		return BoundText(Display, 1024);
	}

	FString BoundText(FString Value, int32 MaxChars)
	{
		Value.TrimStartAndEndInline();
		Value.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Value.Contains(TEXT("  ")))
		{
			Value.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		if (Value.Len() <= MaxChars)
		{
			return Value;
		}
		return Value.Left(FMath::Max(0, MaxChars - 3)) + TEXT("...");
	}

	TArray<TSharedPtr<FJsonValue>> StringsToJson(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Rows.Add(MakeShared<FJsonValueString>(BoundText(Value)));
		}
		return Rows;
	}
}
