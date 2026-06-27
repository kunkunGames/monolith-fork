#include "MonolithSlateInspectorActions.h"

#include "MonolithJsonUtils.h"
#include "MonolithParamSchema.h"
#include "MonolithSettings.h"
#include "MonolithToolRegistry.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Framework/Application/SlateApplication.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "Misc/Base64.h"
#include "Misc/Crc.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SEditableText.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/STextBlock.h"

namespace MonolithSlate::SlateInspectorInternal
{
	constexpr int32 RefTtlMs = 5000;
	constexpr int32 MaxDepthLimit = 32;
	constexpr int32 MaxWidgetsLimit = 1000;
	constexpr int32 MaxChildrenInDescribe = 32;
	constexpr int32 MaxTextChars = 160;

	struct FCachedSlateRef
	{
		TWeakPtr<SWidget> Widget;
		int32 Generation = 0;
		int32 WindowIndex = INDEX_NONE;
		int32 WidgetIndex = INDEX_NONE;
		double CreatedAtSeconds = 0.0;
		FString Type;
		FString Ref;
	};

	static FCriticalSection GCacheLock;
	static int32 GGeneration = 0;
	static TMap<FString, FCachedSlateRef> GRefCache;
	static TMap<TWeakPtr<SWidget>, FString> GWidgetToRef;
	static double GLastSnapshotSeconds = 0.0;

	static double NowSeconds()
	{
		return FPlatformTime::Seconds();
	}

	static int32 GetClampedInt(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, int32 DefaultValue, int32 MinValue, int32 MaxValue)
	{
		double RawValue = 0.0;
		if (Params.IsValid() && Params->TryGetNumberField(Field, RawValue))
		{
			return FMath::Clamp(static_cast<int32>(RawValue), MinValue, MaxValue);
		}
		return DefaultValue;
	}

	static bool GetBool(const TSharedPtr<FJsonObject>& Params, const TCHAR* Field, bool bDefaultValue)
	{
		bool bValue = bDefaultValue;
		if (Params.IsValid())
		{
			Params->TryGetBoolField(Field, bValue);
		}
		return bValue;
	}

	static FString TrimText(FString Value, int32 MaxChars = MaxTextChars)
	{
		Value.ReplaceInline(TEXT("\r"), TEXT(" "));
		Value.ReplaceInline(TEXT("\n"), TEXT(" "));
		Value.ReplaceInline(TEXT("\t"), TEXT(" "));
		Value.TrimStartAndEndInline();
		if (Value.Len() > MaxChars)
		{
			Value.LeftInline(MaxChars);
			Value += TEXT("...");
		}
		return Value;
	}

	static bool LooksPathLike(const FString& Text)
	{
		return Text.Contains(TEXT(":\\")) || Text.Contains(TEXT(":/")) || Text.Contains(TEXT("\\\\")) || Text.Contains(TEXT("/Users/")) || Text.Contains(TEXT("/home/"));
	}

	static FString RedactTitle(const FString& Title, bool& bOutRedacted)
	{
		FString Sanitized = TrimText(Title, 96);
		bOutRedacted = LooksPathLike(Sanitized);
		return bOutRedacted ? TEXT("<redacted-path-title>") : Sanitized;
	}

	static FString VisibilityToString(EVisibility Visibility)
	{
		if (Visibility == EVisibility::Visible)
		{
			return TEXT("Visible");
		}
		if (Visibility == EVisibility::Collapsed)
		{
			return TEXT("Collapsed");
		}
		if (Visibility == EVisibility::Hidden)
		{
			return TEXT("Hidden");
		}
		if (Visibility == EVisibility::HitTestInvisible)
		{
			return TEXT("HitTestInvisible");
		}
		if (Visibility == EVisibility::SelfHitTestInvisible)
		{
			return TEXT("SelfHitTestInvisible");
		}
		return TEXT("Unknown");
	}

	static bool IsVisibleForSnapshot(TSharedRef<SWidget> Widget)
	{
		const EVisibility Visibility = Widget->GetVisibility();
		return Visibility != EVisibility::Collapsed && Visibility != EVisibility::Hidden;
	}

	static FString ExtractWidgetText(TSharedRef<SWidget> Widget)
	{
		const FString Type = Widget->GetTypeAsString();
		if (Type == TEXT("SWindow"))
		{
			bool bRedacted = false;
			return RedactTitle(StaticCastSharedRef<SWindow>(Widget)->GetTitle().ToString(), bRedacted);
		}
		if (Type == TEXT("STextBlock"))
		{
			return TrimText(StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString());
		}
		if (Type == TEXT("SEditableText"))
		{
			return TrimText(StaticCastSharedRef<SEditableText>(Widget)->GetText().ToString());
		}
		if (Type == TEXT("SEditableTextBox"))
		{
			return TrimText(StaticCastSharedRef<SEditableTextBox>(Widget)->GetText().ToString());
		}
		if (Type == TEXT("SMultiLineEditableText"))
		{
			return TrimText(StaticCastSharedRef<SMultiLineEditableText>(Widget)->GetText().ToString());
		}
		if (Type == TEXT("SMultiLineEditableTextBox"))
		{
			return TrimText(StaticCastSharedRef<SMultiLineEditableTextBox>(Widget)->GetText().ToString());
		}
		if (Type == TEXT("SDockTab"))
		{
			return TrimText(StaticCastSharedRef<SDockTab>(Widget)->GetTabLabel().ToString());
		}
		return FString();
	}

	static TSharedPtr<FJsonObject> GeometryToJson(TSharedRef<SWidget> Widget)
	{
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		const FVector2D Position = Geometry.GetAbsolutePosition();
		const FVector2D Size = Geometry.GetLocalSize();

		TSharedPtr<FJsonObject> Bounds = MakeShared<FJsonObject>();
		Bounds->SetNumberField(TEXT("x"), Position.X);
		Bounds->SetNumberField(TEXT("y"), Position.Y);
		Bounds->SetNumberField(TEXT("w"), Size.X);
		Bounds->SetNumberField(TEXT("h"), Size.Y);
		return Bounds;
	}

	static FString MakeRef(int32 Generation, int32 WindowIndex, int32 WidgetIndex, TSharedRef<SWidget> Widget)
	{
		const FGeometry& Geometry = Widget->GetCachedGeometry();
		const FVector2D Position = Geometry.GetAbsolutePosition();
		const FVector2D Size = Geometry.GetLocalSize();
		const FString Seed = FString::Printf(
			TEXT("%d:%d:%d:%s:%.0f:%.0f:%.0f:%.0f"),
			Generation,
			WindowIndex,
			WidgetIndex,
			*Widget->GetTypeAsString(),
			Position.X,
			Position.Y,
			Size.X,
			Size.Y);
		const uint32 ShortHash = FCrc::StrCrc32(*Seed) & 0xffff;
		return FString::Printf(TEXT("slate:%d:%d:%d:%04x"), Generation, WindowIndex, WidgetIndex, ShortHash);
	}

	static FString CacheRef(TSharedRef<SWidget> Widget, int32 WindowIndex, int32 WidgetIndex)
	{
		const FString Ref = MakeRef(GGeneration, WindowIndex, WidgetIndex, Widget);

		FCachedSlateRef Entry;
		Entry.Widget = Widget;
		Entry.Generation = GGeneration;
		Entry.WindowIndex = WindowIndex;
		Entry.WidgetIndex = WidgetIndex;
		Entry.CreatedAtSeconds = NowSeconds();
		Entry.Type = Widget->GetTypeAsString();
		Entry.Ref = Ref;

		GRefCache.Add(Ref, Entry);
		GWidgetToRef.Add(TWeakPtr<SWidget>(Widget), Ref);
		return Ref;
	}

	static FString FindCachedRef(TSharedRef<SWidget> Widget)
	{
		FScopeLock Lock(&GCacheLock);
		if (const FString* Ref = GWidgetToRef.Find(TWeakPtr<SWidget>(Widget)))
		{
			return *Ref;
		}
		return FString();
	}

	static TSharedPtr<FJsonObject> WidgetToJson(TSharedRef<SWidget> Widget, const FString& Ref, const FString& ParentRef, int32 Depth)
	{
		const EVisibility Visibility = Widget->GetVisibility();
		const FString Text = ExtractWidgetText(Widget);
		FChildren* Children = Widget->GetChildren();

		TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
		if (!Ref.IsEmpty())
		{
			Row->SetStringField(TEXT("ref"), Ref);
		}
		if (!ParentRef.IsEmpty())
		{
			Row->SetStringField(TEXT("parent_ref"), ParentRef);
		}
		Row->SetNumberField(TEXT("depth"), Depth);
		Row->SetStringField(TEXT("type"), Widget->GetTypeAsString());
		Row->SetStringField(TEXT("visibility"), VisibilityToString(Visibility));
		Row->SetBoolField(TEXT("visible"), IsVisibleForSnapshot(Widget));
		Row->SetBoolField(TEXT("enabled"), Widget->IsEnabled());
		Row->SetBoolField(TEXT("focused"), Widget->HasKeyboardFocus());
		Row->SetNumberField(TEXT("child_count"), Children ? Children->Num() : 0);
		Row->SetObjectField(TEXT("bounds"), GeometryToJson(Widget));
		if (!Text.IsEmpty())
		{
			Row->SetStringField(TEXT("text"), Text);
		}
		return Row;
	}

	static void WalkWidget(
		TSharedRef<SWidget> Widget,
		int32 WindowIndex,
		int32 Depth,
		int32 MaxDepth,
		int32 MaxWidgets,
		bool bIncludeHidden,
		const FString& ParentRef,
		TArray<TSharedPtr<FJsonValue>>& OutRows,
		int32& InOutWidgetIndex,
		bool& bOutTruncated)
	{
		if (bOutTruncated || Depth > MaxDepth)
		{
			return;
		}
		if (!bIncludeHidden && !IsVisibleForSnapshot(Widget))
		{
			return;
		}
		if (OutRows.Num() >= MaxWidgets)
		{
			bOutTruncated = true;
			return;
		}

		const int32 ThisWidgetIndex = InOutWidgetIndex++;
		const FString Ref = CacheRef(Widget, WindowIndex, ThisWidgetIndex);
		OutRows.Add(MakeShared<FJsonValueObject>(WidgetToJson(Widget, Ref, ParentRef, Depth)));

		FChildren* Children = Widget->GetChildren();
		if (!Children)
		{
			return;
		}

		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			WalkWidget(Children->GetChildAt(ChildIndex), WindowIndex, Depth + 1, MaxDepth, MaxWidgets, bIncludeHidden, Ref, OutRows, InOutWidgetIndex, bOutTruncated);
			if (bOutTruncated)
			{
				return;
			}
		}
	}

	static TArray<TSharedRef<SWindow>> GetVisibleWindows()
	{
		TArray<TSharedRef<SWindow>> Windows;
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().GetAllVisibleWindowsOrdered(Windows);
		}
		return Windows;
	}

	static FMonolithActionResult ResolveCurrentRef(const FString& Ref, TSharedPtr<SWidget>& OutWidget, FString& OutErrorCode)
	{
		if (!Ref.StartsWith(TEXT("slate:")))
		{
			OutErrorCode = TEXT("invalid_ref");
			return FMonolithActionResult::Error(TEXT("Invalid Slate ref. Expected slate:<generation>:<window_index>:<widget_index>:<hash>"), FMonolithJsonUtils::ErrInvalidParams);
		}

		FScopeLock Lock(&GCacheLock);
		const FCachedSlateRef* Entry = GRefCache.Find(Ref);
		if (!Entry)
		{
			OutErrorCode = TEXT("stale_ref");
			return FMonolithActionResult::Error(TEXT("Slate ref is not in the current ref cache"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (Entry->Generation != GGeneration)
		{
			OutErrorCode = TEXT("stale_ref");
			return FMonolithActionResult::Error(TEXT("Slate ref generation is stale"), FMonolithJsonUtils::ErrInvalidParams);
		}
		if (((NowSeconds() - Entry->CreatedAtSeconds) * 1000.0) > static_cast<double>(RefTtlMs))
		{
			OutErrorCode = TEXT("stale_ref");
			return FMonolithActionResult::Error(TEXT("Slate ref expired; call slate.snapshot_widgets again"), FMonolithJsonUtils::ErrInvalidParams);
		}

		OutWidget = Entry->Widget.Pin();
		if (!OutWidget.IsValid())
		{
			OutErrorCode = TEXT("stale_ref");
			return FMonolithActionResult::Error(TEXT("Slate widget no longer exists"), FMonolithJsonUtils::ErrInvalidParams);
		}

		FMonolithActionResult Ok;
		Ok.bSuccess = true;
		return Ok;
	}

	static FMonolithActionResult HandleGetInspectorStatus(const TSharedPtr<FJsonObject>& Params)
	{
		const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
		const bool bFeatureEnabled = Settings && Settings->bEnableSlateInspectorActions;
		const TArray<TSharedRef<SWindow>> Windows = GetVisibleWindows();

		TArray<TSharedPtr<FJsonValue>> RegisteredActions;
		RegisteredActions.Add(MakeShared<FJsonValueString>(TEXT("get_inspector_status")));
		if (bFeatureEnabled)
		{
			RegisteredActions.Add(MakeShared<FJsonValueString>(TEXT("list_windows")));
			RegisteredActions.Add(MakeShared<FJsonValueString>(TEXT("snapshot_widgets")));
			RegisteredActions.Add(MakeShared<FJsonValueString>(TEXT("describe_widget")));
			RegisteredActions.Add(MakeShared<FJsonValueString>(TEXT("capture_widget")));
			RegisteredActions.Add(MakeShared<FJsonValueString>(TEXT("wait_for_widget")));
		}

		TArray<TSharedPtr<FJsonValue>> GatedActions;
		if (!bFeatureEnabled)
		{
			GatedActions.Add(MakeShared<FJsonValueString>(TEXT("list_windows")));
			GatedActions.Add(MakeShared<FJsonValueString>(TEXT("snapshot_widgets")));
			GatedActions.Add(MakeShared<FJsonValueString>(TEXT("describe_widget")));
			GatedActions.Add(MakeShared<FJsonValueString>(TEXT("capture_widget")));
			GatedActions.Add(MakeShared<FJsonValueString>(TEXT("wait_for_widget")));
		}

		TArray<TSharedPtr<FJsonValue>> SupportedOperations;
		SupportedOperations.Add(MakeShared<FJsonValueString>(TEXT("status")));
		SupportedOperations.Add(MakeShared<FJsonValueString>(TEXT("window_list")));
		SupportedOperations.Add(MakeShared<FJsonValueString>(TEXT("snapshot")));
		SupportedOperations.Add(MakeShared<FJsonValueString>(TEXT("describe")));
		SupportedOperations.Add(MakeShared<FJsonValueString>(TEXT("slate_screenshot")));
		SupportedOperations.Add(MakeShared<FJsonValueString>(TEXT("wait_poll")));

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("enabled"), bFeatureEnabled);
		Result->SetBoolField(TEXT("slate_initialized"), FSlateApplication::IsInitialized());
		Result->SetNumberField(TEXT("visible_window_count"), Windows.Num());
		Result->SetNumberField(TEXT("generation"), GGeneration);
		Result->SetNumberField(TEXT("refs_expire_after_ms"), RefTtlMs);
		Result->SetBoolField(TEXT("input_actions_registered"), false);
		Result->SetArrayField(TEXT("registered_actions"), RegisteredActions);
		Result->SetArrayField(TEXT("gated_actions"), GatedActions);
		Result->SetArrayField(TEXT("supported_operations"), SupportedOperations);
		return FMonolithActionResult::Success(Result);
	}

	static FMonolithActionResult HandleListWindows(const TSharedPtr<FJsonObject>& Params)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FMonolithActionResult::Error(TEXT("Slate application not initialized"));
		}

		const bool bIncludeTitles = GetBool(Params, TEXT("include_titles"), true);
		const TArray<TSharedRef<SWindow>> Windows = GetVisibleWindows();
		TArray<TSharedPtr<FJsonValue>> Rows;
		Rows.Reserve(Windows.Num());

		for (int32 Index = 0; Index < Windows.Num(); ++Index)
		{
			const TSharedRef<SWindow>& Window = Windows[Index];
			TSharedPtr<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetNumberField(TEXT("index"), Index);
			Row->SetBoolField(TEXT("visible"), Window->IsVisible());
			Row->SetBoolField(TEXT("enabled"), Window->IsEnabled());
			Row->SetNumberField(TEXT("width"), Window->GetSizeInScreen().X);
			Row->SetNumberField(TEXT("height"), Window->GetSizeInScreen().Y);
			if (bIncludeTitles)
			{
				bool bRedacted = false;
				Row->SetStringField(TEXT("title"), RedactTitle(Window->GetTitle().ToString(), bRedacted));
				Row->SetBoolField(TEXT("title_redacted"), bRedacted);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("window_count"), Windows.Num());
		Result->SetArrayField(TEXT("windows"), Rows);
		return FMonolithActionResult::Success(Result);
	}

	static FMonolithActionResult HandleSnapshotWidgets(const TSharedPtr<FJsonObject>& Params)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FMonolithActionResult::Error(TEXT("Slate application not initialized"));
		}

		const int32 WindowIndexFilter = GetClampedInt(Params, TEXT("window_index"), -1, -1, 100000);
		const int32 MaxDepth = GetClampedInt(Params, TEXT("max_depth"), 8, 0, MaxDepthLimit);
		const int32 MaxWidgets = GetClampedInt(Params, TEXT("max_widgets"), 200, 1, MaxWidgetsLimit);
		const bool bIncludeHidden = GetBool(Params, TEXT("include_hidden"), false);
		const TArray<TSharedRef<SWindow>> Windows = GetVisibleWindows();

		if (WindowIndexFilter >= Windows.Num())
		{
			return FMonolithActionResult::Error(TEXT("window_index is out of range"), FMonolithJsonUtils::ErrInvalidParams);
		}

		TArray<TSharedPtr<FJsonValue>> Rows;
		bool bTruncated = false;
		int32 WidgetIndex = 0;
		int32 SnapshotGeneration = 0;

		{
			FScopeLock Lock(&GCacheLock);
			++GGeneration;
			SnapshotGeneration = GGeneration;
			GRefCache.Empty();
			GWidgetToRef.Empty();
			GLastSnapshotSeconds = NowSeconds();

			for (int32 WindowIndex = 0; WindowIndex < Windows.Num(); ++WindowIndex)
			{
				if (WindowIndexFilter != -1 && WindowIndexFilter != WindowIndex)
				{
					continue;
				}
				WalkWidget(Windows[WindowIndex], WindowIndex, 0, MaxDepth, MaxWidgets, bIncludeHidden, FString(), Rows, WidgetIndex, bTruncated);
				if (bTruncated)
				{
					break;
				}
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("generation"), SnapshotGeneration);
		Result->SetNumberField(TEXT("refs_expire_after_ms"), RefTtlMs);
		Result->SetNumberField(TEXT("window_count"), Windows.Num());
		Result->SetNumberField(TEXT("returned_widget_count"), Rows.Num());
		Result->SetBoolField(TEXT("truncated"), bTruncated);
		Result->SetArrayField(TEXT("widgets"), Rows);
		return FMonolithActionResult::Success(Result);
	}

	static FMonolithActionResult HandleDescribeWidget(const TSharedPtr<FJsonObject>& Params)
	{
		FString Ref;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("ref"), Ref))
		{
			return FMonolithActionResult::Error(TEXT("ref is required"), FMonolithJsonUtils::ErrInvalidParams);
		}

		TSharedPtr<SWidget> Widget;
		FString ErrorCode;
		FMonolithActionResult ResolveResult = ResolveCurrentRef(Ref, Widget, ErrorCode);
		if (!ResolveResult.bSuccess)
		{
			if (!ErrorCode.IsEmpty())
			{
				TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
				ErrorData->SetStringField(TEXT("error"), ErrorCode);
				ResolveResult.WithErrorData(ErrorData);
			}
			return ResolveResult;
		}

		TSharedRef<SWidget> WidgetRef = Widget.ToSharedRef();
		TSharedPtr<FJsonObject> Result = WidgetToJson(WidgetRef, Ref, FString(), 0);

		TSharedPtr<SWidget> Parent = Widget->GetParentWidget();
		if (Parent.IsValid())
		{
			TSharedPtr<FJsonObject> ParentJson = MakeShared<FJsonObject>();
			const FString ParentRef = FindCachedRef(Parent.ToSharedRef());
			if (!ParentRef.IsEmpty())
			{
				ParentJson->SetStringField(TEXT("ref"), ParentRef);
			}
			ParentJson->SetStringField(TEXT("type"), Parent->GetTypeAsString());
			ParentJson->SetStringField(TEXT("text"), ExtractWidgetText(Parent.ToSharedRef()));
			Result->SetObjectField(TEXT("parent"), ParentJson);
		}

		TArray<TSharedPtr<FJsonValue>> ChildRows;
		FChildren* Children = Widget->GetChildren();
		const int32 ChildCount = Children ? Children->Num() : 0;
		const int32 ChildLimit = FMath::Min(ChildCount, MaxChildrenInDescribe);
		ChildRows.Reserve(ChildLimit);
		for (int32 Index = 0; Index < ChildLimit; ++Index)
		{
			TSharedRef<SWidget> Child = Children->GetChildAt(Index);
			TSharedPtr<FJsonObject> ChildJson = MakeShared<FJsonObject>();
			const FString ChildRef = FindCachedRef(Child);
			if (!ChildRef.IsEmpty())
			{
				ChildJson->SetStringField(TEXT("ref"), ChildRef);
			}
			ChildJson->SetStringField(TEXT("type"), Child->GetTypeAsString());
			ChildJson->SetStringField(TEXT("visibility"), VisibilityToString(Child->GetVisibility()));
			ChildJson->SetBoolField(TEXT("visible"), IsVisibleForSnapshot(Child));
			ChildJson->SetBoolField(TEXT("enabled"), Child->IsEnabled());
			const FString ChildText = ExtractWidgetText(Child);
			if (!ChildText.IsEmpty())
			{
				ChildJson->SetStringField(TEXT("text"), ChildText);
			}
			ChildRows.Add(MakeShared<FJsonValueObject>(ChildJson));
		}
		Result->SetArrayField(TEXT("children"), ChildRows);
		Result->SetBoolField(TEXT("children_truncated"), ChildCount > ChildLimit);
		return FMonolithActionResult::Success(Result);
	}

	static FMonolithActionResult HandleCaptureWidget(const TSharedPtr<FJsonObject>& Params)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FMonolithActionResult::Error(TEXT("Slate application not initialized"));
		}

		TSharedPtr<SWidget> TargetWidget;
		FString Ref;
		FString CaptureSource = TEXT("slate_active_window");
		if (Params.IsValid() && Params->TryGetStringField(TEXT("ref"), Ref) && !Ref.IsEmpty())
		{
			FString ErrorCode;
			FMonolithActionResult ResolveResult = ResolveCurrentRef(Ref, TargetWidget, ErrorCode);
			if (!ResolveResult.bSuccess)
			{
				if (!ErrorCode.IsEmpty())
				{
					TSharedPtr<FJsonObject> ErrorData = MakeShared<FJsonObject>();
					ErrorData->SetStringField(TEXT("error"), ErrorCode);
					ResolveResult.WithErrorData(ErrorData);
				}
				return ResolveResult;
			}
			CaptureSource = TEXT("slate_widget");
		}
		else
		{
			TargetWidget = FSlateApplication::Get().GetActiveTopLevelWindow();
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("ref"), Ref);
		Result->SetStringField(TEXT("capture_source"), CaptureSource);
		Result->SetBoolField(TEXT("viewport_fallback_used"), false);

		if (!TargetWidget.IsValid())
		{
			Result->SetBoolField(TEXT("captured"), false);
			Result->SetStringField(TEXT("reason"), TEXT("target_unavailable"));
			return FMonolithActionResult::Success(Result);
		}

		TArray<FColor> ColorData;
		FIntVector Size;
		if (!FSlateApplication::Get().TakeScreenshot(TargetWidget.ToSharedRef(), ColorData, Size) || ColorData.Num() == 0 || Size.X <= 0 || Size.Y <= 0)
		{
			Result->SetBoolField(TEXT("captured"), false);
			Result->SetStringField(TEXT("reason"), TEXT("capture_unavailable"));
			return FMonolithActionResult::Success(Result);
		}

		TArray64<uint8> PngData;
		FImageView ImageView((void*)ColorData.GetData(), Size.X, Size.Y, ERawImageFormat::BGRA8);
		FImageUtils::CompressImage(PngData, TEXT(".png"), ImageView);
		if (PngData.Num() == 0)
		{
			Result->SetBoolField(TEXT("captured"), false);
			Result->SetStringField(TEXT("reason"), TEXT("png_compress_failed"));
			return FMonolithActionResult::Success(Result);
		}

		const int32 MaxBytes = GetClampedInt(Params, TEXT("max_bytes"), 1024 * 1024, 1, 8 * 1024 * 1024);
		Result->SetNumberField(TEXT("width"), Size.X);
		Result->SetNumberField(TEXT("height"), Size.Y);
		Result->SetNumberField(TEXT("byte_count"), static_cast<double>(PngData.Num()));
		Result->SetStringField(TEXT("mime_type"), TEXT("image/png"));
		if (PngData.Num() > MaxBytes)
		{
			Result->SetBoolField(TEXT("captured"), false);
			Result->SetStringField(TEXT("reason"), TEXT("capture_too_large"));
			Result->SetNumberField(TEXT("max_bytes"), MaxBytes);
			return FMonolithActionResult::Success(Result);
		}

		Result->SetBoolField(TEXT("captured"), true);
		Result->SetStringField(TEXT("encoding"), TEXT("base64"));
		Result->SetStringField(TEXT("bytes_b64"), FBase64::Encode(PngData.GetData(), static_cast<uint32>(PngData.Num())));
		return FMonolithActionResult::Success(Result);
	}

	static bool WidgetMatches(TSharedRef<SWidget> Widget, const FString& TextContains, const FString& TypeContains, bool bRequireVisible)
	{
		if (bRequireVisible && !IsVisibleForSnapshot(Widget))
		{
			return false;
		}

		if (!TypeContains.IsEmpty() && !Widget->GetTypeAsString().Contains(TypeContains, ESearchCase::IgnoreCase))
		{
			return false;
		}

		if (!TextContains.IsEmpty())
		{
			const FString Text = ExtractWidgetText(Widget);
			if (!Text.Contains(TextContains, ESearchCase::IgnoreCase))
			{
				return false;
			}
		}

		return true;
	}

	static bool FindMatchingWidget(TSharedRef<SWidget> Widget, const FString& TextContains, const FString& TypeContains, bool bRequireVisible, int32 MaxDepth, int32 Depth, TSharedPtr<FJsonObject>& OutMatch)
	{
		if (Depth > MaxDepth)
		{
			return false;
		}
		if (WidgetMatches(Widget, TextContains, TypeContains, bRequireVisible))
		{
			OutMatch = WidgetToJson(Widget, FString(), FString(), Depth);
			return true;
		}

		FChildren* Children = Widget->GetChildren();
		if (!Children)
		{
			return false;
		}
		for (int32 ChildIndex = 0; ChildIndex < Children->Num(); ++ChildIndex)
		{
			if (FindMatchingWidget(Children->GetChildAt(ChildIndex), TextContains, TypeContains, bRequireVisible, MaxDepth, Depth + 1, OutMatch))
			{
				return true;
			}
		}
		return false;
	}

	static FMonolithActionResult HandleWaitForWidget(const TSharedPtr<FJsonObject>& Params)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FMonolithActionResult::Error(TEXT("Slate application not initialized"));
		}

		FString TextContains;
		FString TypeContains;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("text_contains"), TextContains);
			Params->TryGetStringField(TEXT("type"), TypeContains);
		}
		if (TextContains.IsEmpty() && TypeContains.IsEmpty())
		{
			return FMonolithActionResult::Error(TEXT("text_contains or type is required"), FMonolithJsonUtils::ErrInvalidParams);
		}

		const int32 MaxDepth = GetClampedInt(Params, TEXT("max_depth"), 12, 0, MaxDepthLimit);
		const bool bRequireVisible = GetBool(Params, TEXT("visible"), true);
		const double Start = NowSeconds();

		// Action handlers run synchronously on the editor game thread (see
		// MonolithCrashBreadcrumb). The previous implementation polled with
		// FPlatformProcess::Sleep + a reentrant FSlateApplication::Tick for up
		// to timeout_ms (<=5s), which froze editor responsiveness and stalled
		// every other queued tool action for that whole window. This is now a
		// single non-blocking scan: callers that need to wait for a widget
		// should poll this action client-side instead of blocking the editor
		// loop. timeout_ms / poll_interval_ms remain accepted by the param
		// schema for call-site compatibility but no longer block here.
		TSharedPtr<FJsonObject> Match;
		bool bFound = false;
		const TArray<TSharedRef<SWindow>> Windows = GetVisibleWindows();
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			if (FindMatchingWidget(Window, TextContains, TypeContains, bRequireVisible, MaxDepth, 0, Match))
			{
				bFound = true;
				break;
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("found"), bFound);
		Result->SetNumberField(TEXT("polls"), 1);
		Result->SetNumberField(TEXT("elapsed_ms"), (NowSeconds() - Start) * 1000.0);
		Result->SetStringField(TEXT("text_contains"), TextContains);
		Result->SetStringField(TEXT("type"), TypeContains);
		Result->SetBoolField(TEXT("visible_required"), bRequireVisible);
		Result->SetBoolField(TEXT("blocking_wait"), false);
		if (!bFound)
		{
			Result->SetStringField(TEXT("note"),
				TEXT("Single non-blocking check; server-side waiting was removed to avoid stalling the editor game thread. Poll this action client-side to wait for a widget."));
		}
		if (Match.IsValid())
		{
			Result->SetObjectField(TEXT("match"), Match);
		}
		return FMonolithActionResult::Success(Result);
	}
}

void MonolithSlate::FSlateInspectorActions::Register(FMonolithToolRegistry& Registry)
{
	using namespace SlateInspectorInternal;

	const FString Cat(TEXT("Slate Inspector"));
	Registry.RegisterAction(
		TEXT("slate"), TEXT("get_inspector_status"),
		TEXT("Report Slate inspector flag state, Slate availability, visible window count, ref generation, and gated read-only actions."),
		FMonolithActionHandler::CreateStatic(&HandleGetInspectorStatus),
		FParamSchemaBuilder().Build(),
		Cat);

	const UMonolithSettings* Settings = GetDefault<UMonolithSettings>();
	if (!Settings || !Settings->bEnableSlateInspectorActions)
	{
		return;
	}

	Registry.RegisterAction(
		TEXT("slate"), TEXT("list_windows"),
		TEXT("List visible top-level Slate windows with bounded metadata and redacted titles."),
		FMonolithActionHandler::CreateStatic(&HandleListWindows),
		FParamSchemaBuilder()
			.Optional(TEXT("include_titles"), TEXT("bool"), TEXT("Include redacted top-level window titles."), TEXT("true"))
			.Build(),
		Cat);

	Registry.RegisterAction(
		TEXT("slate"), TEXT("snapshot_widgets"),
		TEXT("Return a bounded live Slate widget snapshot and short-lived opaque refs. Rebuilds the ref cache."),
		FMonolithActionHandler::CreateStatic(&HandleSnapshotWidgets),
		FParamSchemaBuilder()
			.Optional(TEXT("window_index"), TEXT("number"), TEXT("-1 for all visible windows, otherwise a list_windows index."), TEXT("-1"))
			.Optional(TEXT("max_depth"), TEXT("number"), TEXT("Maximum child depth to walk."), TEXT("8"))
			.Range(TEXT("max_depth"), 0, MaxDepthLimit)
			.Optional(TEXT("max_widgets"), TEXT("number"), TEXT("Maximum widgets returned across the snapshot."), TEXT("200"))
			.Range(TEXT("max_widgets"), 1, MaxWidgetsLimit)
			.Optional(TEXT("include_hidden"), TEXT("bool"), TEXT("Include hidden/collapsed widgets when Slate exposes them."), TEXT("false"))
			.Build(),
		Cat);

	Registry.RegisterAction(
		TEXT("slate"), TEXT("describe_widget"),
		TEXT("Describe a current Slate ref: type, visibility, focus, geometry, text, parent, and capped children."),
		FMonolithActionHandler::CreateStatic(&HandleDescribeWidget),
		FParamSchemaBuilder()
			.Required(TEXT("ref"), TEXT("string"), TEXT("Opaque ref returned by slate.snapshot_widgets."))
			.Build(),
		Cat);

	Registry.RegisterAction(
		TEXT("slate"), TEXT("capture_widget"),
		TEXT("Capture a Slate widget or active top-level window with FSlateApplication::TakeScreenshot. No level viewport fallback."),
		FMonolithActionHandler::CreateStatic(&HandleCaptureWidget),
		FParamSchemaBuilder()
			.Optional(TEXT("ref"), TEXT("string"), TEXT("Opaque ref returned by slate.snapshot_widgets. Omit to capture active top-level window."))
			.Optional(TEXT("max_bytes"), TEXT("number"), TEXT("Maximum compressed PNG bytes to return as base64."), TEXT("1048576"))
			.Range(TEXT("max_bytes"), 1, 8 * 1024 * 1024)
			.Build(),
		Cat);

	Registry.RegisterAction(
		TEXT("slate"), TEXT("wait_for_widget"),
		TEXT("Poll visible Slate widgets for text/type/visibility state without input simulation."),
		FMonolithActionHandler::CreateStatic(&HandleWaitForWidget),
		FParamSchemaBuilder()
			.Optional(TEXT("text_contains"), TEXT("string"), TEXT("Case-insensitive text fragment to find."))
			.Optional(TEXT("type"), TEXT("string"), TEXT("Case-insensitive Slate type fragment to find, for example SButton."))
			.Optional(TEXT("visible"), TEXT("bool"), TEXT("Require visible widgets only."), TEXT("true"))
			.Optional(TEXT("timeout_ms"), TEXT("number"), TEXT("Maximum poll duration, capped at 5000 ms."), TEXT("1000"))
			.Optional(TEXT("poll_interval_ms"), TEXT("number"), TEXT("Poll interval, clamped 16..1000 ms."), TEXT("100"))
			.Range(TEXT("poll_interval_ms"), 16, 1000)
			.Optional(TEXT("max_depth"), TEXT("number"), TEXT("Maximum child depth to walk per poll."), TEXT("12"))
			.Range(TEXT("max_depth"), 0, MaxDepthLimit)
			.Build(),
		Cat);
}
