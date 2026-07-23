// Copyright tumourlove. All Rights Reserved.
#include "MonolithImageGenSvgSourceActions.h"

#include "MonolithAssetTextureIngestActions.h"
#include "MonolithJsonUtils.h"
#include "MonolithPackagePathValidator.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "MonolithAssetUtils.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/MetaData.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "XmlFile.h"
#include "XmlNode.h"

namespace MonolithImageGen::SvgSource
{
namespace
{
	static constexpr int32 MaxSvgInputChars = 256 * 1024;
	static constexpr int32 MaxSvgOutputChars = 512 * 1024;
	static constexpr int32 MaxSvgElements = 512;
	static constexpr int32 MaxSvgDepth = 32;
	static constexpr int32 MaxPathCommands = 4096;
	static constexpr double MaxCoordinateMagnitude = 1000000.0;
	static constexpr double GeometryEpsilon = 0.001;
	static constexpr const TCHAR* DefaultVectorAssetPath = TEXT("/Game/GeneratedImages/Vector");
	static constexpr const TCHAR* DefaultVectorModel = TEXT("monolith/local-svg-v1");
	static constexpr const TCHAR* VectorDirectoryName = TEXT("GeneratedImages");

	struct FViewBox
	{
		double X = 0.0;
		double Y = 0.0;
		double W = 128.0;
		double H = 128.0;
		bool bValid = false;
	};

	struct FPoint2
	{
		double X = 0.0;
		double Y = 0.0;
	};

	struct FContour
	{
		TArray<FPoint2> Points;
		bool bClosed = false;
		FString Source;
		FString FillRule = TEXT("nonzero");
	};

	struct FBounds
	{
		double MinX = 0.0;
		double MinY = 0.0;
		double MaxX = 0.0;
		double MaxY = 0.0;
		bool bValid = false;
	};

	enum class ESvgActionMode : uint8
	{
		Generate,
		Import,
		Validate
	};

	struct FSvgOptions
	{
		FString Profile = TEXT("editor");
		FString GeometryPolicy = TEXT("validate");
		FString FillRulePolicy = TEXT("preserve");
		bool bStrict = false;
		bool bReturnSvg = false;
		bool bSave = false;
		double Margin = 0.0;
	};

	struct FSvgState
	{
		FString OriginalSvg;
		FString SanitizedSvg;
		FString OriginalHash;
		FString SvgHash;
		FString Profile;
		FString GeometryPolicy;
		FString FillRulePolicy;
		FString EffectiveFillRule = TEXT("nonzero");
		FViewBox ViewBox;
		FBounds Bounds;
		TArray<FString> Warnings;
		TArray<FString> Removed;
		TArray<FString> Blockers;
		TArray<FString> WindingIssues;
		TArray<FString> RepairActions;
		TArray<FContour> Contours;
		int32 ElementCount = 0;
		int32 PathCount = 0;
		int32 PathCommandCount = 0;
		int32 OpenContourCount = 0;
		int32 SelfIntersectionCount = 0;
		int32 OverlapCount = 0;
		bool bSvgValid = false;
		bool bSanitized = false;
		bool bGeometryValid = false;
		bool bMsdfReady = false;
		bool bSawText = false;
		bool bSawGradient = false;
		bool bSawStroke = false;
		bool bSawTransform = false;
	};

	static void AddUnique(TArray<FString>& Values, const FString& Value)
	{
		if (!Value.IsEmpty())
		{
			Values.AddUnique(Value);
		}
	}

	static TArray<TSharedPtr<FJsonValue>> ToJsonStringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& Value : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(Value));
		}
		return Out;
	}

	static void SetStringArray(TSharedPtr<FJsonObject> Object, const TCHAR* Field, const TArray<FString>& Values)
	{
		Object->SetArrayField(Field, ToJsonStringArray(Values));
	}

	static FString TrimNumberString(FString Value)
	{
		if (Value.Contains(TEXT(".")))
		{
			while (Value.EndsWith(TEXT("0")))
			{
				Value.LeftChopInline(1);
			}
			if (Value.EndsWith(TEXT(".")))
			{
				Value.LeftChopInline(1);
			}
		}
		if (Value == TEXT("-0"))
		{
			Value = TEXT("0");
		}
		return Value;
	}

	static FString FormatNumber(double Value)
	{
		return TrimNumberString(FString::Printf(TEXT("%.4f"), Value));
	}

	static FString FormatViewBox(const FViewBox& ViewBox)
	{
		return FString::Printf(
			TEXT("%s %s %s %s"),
			*FormatNumber(ViewBox.X),
			*FormatNumber(ViewBox.Y),
			*FormatNumber(ViewBox.W),
			*FormatNumber(ViewBox.H));
	}

	static FString XmlEscape(FString Text)
	{
		Text.ReplaceInline(TEXT("&"), TEXT("&amp;"));
		Text.ReplaceInline(TEXT("<"), TEXT("&lt;"));
		Text.ReplaceInline(TEXT(">"), TEXT("&gt;"));
		Text.ReplaceInline(TEXT("\""), TEXT("&quot;"));
		Text.ReplaceInline(TEXT("'"), TEXT("&apos;"));
		return Text;
	}

	static FString StripNamespace(const FString& Tag)
	{
		int32 ColonIndex = INDEX_NONE;
		if (Tag.FindChar(TCHAR(':'), ColonIndex))
		{
			return Tag.Mid(ColonIndex + 1).ToLower();
		}
		return Tag.ToLower();
	}

	static FString HashStringUtf8(const FString& Text)
	{
		FMD5 Md5;
		const auto Utf8 = StringCast<UTF8CHAR>(*Text);
		Md5.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length() * sizeof(UTF8CHAR));

		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	static FString CompactBase64Payload(const FString& Input)
	{
		FString Compact;
		Compact.Reserve(Input.Len());
		for (const TCHAR Ch : Input)
		{
			if (!FChar::IsWhitespace(Ch))
			{
				Compact.AppendChar(Ch);
			}
		}
		return Compact;
	}

	static FString StripSvgDataUrlPrefix(const FString& Input, FString& InOutFormatHint)
	{
		int32 CommaIndex = INDEX_NONE;
		if (!Input.FindChar(TCHAR(','), CommaIndex))
		{
			return Input;
		}

		const FString Header = Input.Left(CommaIndex).ToLower();
		if (!Header.Contains(TEXT("base64")))
		{
			return Input;
		}
		if (Header.Contains(TEXT("image/svg+xml")) || Header.Contains(TEXT("text/xml")))
		{
			InOutFormatHint = TEXT("svg");
		}
		return Input.RightChop(CommaIndex + 1);
	}

	static bool IsSeparator(TCHAR Ch)
	{
		return FChar::IsWhitespace(Ch) || Ch == TCHAR(',');
	}

	static void SkipSeparators(const FString& Text, int32& Index)
	{
		while (Index < Text.Len() && IsSeparator(Text[Index]))
		{
			++Index;
		}
	}

	static bool ReadNumberToken(const FString& Text, int32& Index, double& OutValue)
	{
		SkipSeparators(Text, Index);
		if (Index >= Text.Len())
		{
			return false;
		}

		const int32 Start = Index;
		if (Text[Index] == TCHAR('+') || Text[Index] == TCHAR('-'))
		{
			++Index;
		}

		bool bSawDigit = false;
		while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
		{
			bSawDigit = true;
			++Index;
		}
		if (Index < Text.Len() && Text[Index] == TCHAR('.'))
		{
			++Index;
			while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
			{
				bSawDigit = true;
				++Index;
			}
		}
		if (!bSawDigit)
		{
			Index = Start;
			return false;
		}

		if (Index < Text.Len() && (Text[Index] == TCHAR('e') || Text[Index] == TCHAR('E')))
		{
			const int32 ExpStart = Index;
			++Index;
			if (Index < Text.Len() && (Text[Index] == TCHAR('+') || Text[Index] == TCHAR('-')))
			{
				++Index;
			}
			bool bSawExpDigit = false;
			while (Index < Text.Len() && FChar::IsDigit(Text[Index]))
			{
				bSawExpDigit = true;
				++Index;
			}
			if (!bSawExpDigit)
			{
				Index = ExpStart;
			}
		}

		const FString NumberString = Text.Mid(Start, Index - Start);
		if (!LexTryParseString(OutValue, *NumberString) || !FMath::IsFinite(OutValue))
		{
			Index = Start;
			return false;
		}
		return true;
	}

	static bool ExtractNumberList(const FString& Text, TArray<double>& OutValues, FString& OutError)
	{
		int32 Index = 0;
		while (Index < Text.Len())
		{
			SkipSeparators(Text, Index);
			if (Index >= Text.Len())
			{
				break;
			}

			double Value = 0.0;
			if (!ReadNumberToken(Text, Index, Value))
			{
				OutError = FString::Printf(TEXT("Expected number near '%s'"), *Text.Mid(Index, 24));
				return false;
			}
			if (FMath::Abs(Value) > MaxCoordinateMagnitude)
			{
				OutError = FString::Printf(TEXT("Coordinate magnitude %.3f exceeds %.0f"), Value, MaxCoordinateMagnitude);
				return false;
			}
			OutValues.Add(Value);
		}
		return true;
	}

	static bool ParseLength(const FString& Raw, double& OutValue)
	{
		FString Clean = Raw.TrimStartAndEnd().ToLower();
		if (Clean.EndsWith(TEXT("px")))
		{
			Clean.LeftChopInline(2);
		}
		return LexTryParseString(OutValue, *Clean) && FMath::IsFinite(OutValue);
	}

	static bool ParseViewBoxString(const FString& Raw, FViewBox& OutViewBox, FString& OutError)
	{
		TArray<double> Values;
		if (!ExtractNumberList(Raw, Values, OutError))
		{
			return false;
		}
		if (Values.Num() != 4)
		{
			OutError = TEXT("viewBox must contain exactly four numbers");
			return false;
		}
		if (Values[2] <= 0.0 || Values[3] <= 0.0)
		{
			OutError = TEXT("viewBox width and height must be positive");
			return false;
		}
		OutViewBox.X = Values[0];
		OutViewBox.Y = Values[1];
		OutViewBox.W = Values[2];
		OutViewBox.H = Values[3];
		OutViewBox.bValid = true;
		return true;
	}

	static bool ParseViewBoxJson(const TSharedPtr<FJsonValue>& Value, FViewBox& OutViewBox, FString& OutError)
	{
		if (!Value.IsValid() || Value->IsNull())
		{
			return false;
		}

		if (Value->Type == EJson::String)
		{
			return ParseViewBoxString(Value->AsString(), OutViewBox, OutError);
		}
		if (Value->Type == EJson::Array)
		{
			const TArray<TSharedPtr<FJsonValue>>& Values = Value->AsArray();
			if (Values.Num() != 4)
			{
				OutError = TEXT("view_box array must contain [x, y, width, height]");
				return false;
			}
			double Numbers[4] = { 0.0, 0.0, 0.0, 0.0 };
			for (int32 Index = 0; Index < 4; ++Index)
			{
				if (!Values[Index].IsValid() || !Values[Index]->TryGetNumber(Numbers[Index]))
				{
					OutError = TEXT("view_box array values must be numbers");
					return false;
				}
			}
			if (Numbers[2] <= 0.0 || Numbers[3] <= 0.0)
			{
				OutError = TEXT("view_box width and height must be positive");
				return false;
			}
			OutViewBox.X = Numbers[0];
			OutViewBox.Y = Numbers[1];
			OutViewBox.W = Numbers[2];
			OutViewBox.H = Numbers[3];
			OutViewBox.bValid = true;
			return true;
		}
		if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid()
				|| !Obj->TryGetNumberField(TEXT("x"), OutViewBox.X)
				|| !Obj->TryGetNumberField(TEXT("y"), OutViewBox.Y)
				|| !Obj->TryGetNumberField(TEXT("width"), OutViewBox.W)
				|| !Obj->TryGetNumberField(TEXT("height"), OutViewBox.H))
			{
				OutError = TEXT("view_box object must contain x, y, width, and height");
				return false;
			}
			if (OutViewBox.W <= 0.0 || OutViewBox.H <= 0.0)
			{
				OutError = TEXT("view_box width and height must be positive");
				return false;
			}
			OutViewBox.bValid = true;
			return true;
		}

		OutError = TEXT("view_box must be a string, array, or object");
		return false;
	}

	static FString NormalizeProfile(const TSharedPtr<FJsonObject>& Params, FString& OutError)
	{
		FString Profile;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("profile"), Profile) || Profile.IsEmpty())
		{
			Profile = TEXT("editor");
		}
		Profile = Profile.ToLower();
		if (Profile != TEXT("web") && Profile != TEXT("editor") && Profile != TEXT("msdf_source"))
		{
			OutError = TEXT("profile must be 'web', 'editor', or 'msdf_source'");
			return FString();
		}
		return Profile;
	}

	static FString NormalizeEnumParam(
		const TSharedPtr<FJsonObject>& Params,
		const TCHAR* Field,
		const FString& DefaultValue,
		const TArray<FString>& Allowed,
		FString& OutError)
	{
		FString Value;
		if (!Params.IsValid() || !Params->TryGetStringField(Field, Value) || Value.IsEmpty())
		{
			Value = DefaultValue;
		}
		Value = Value.ToLower();
		if (!Allowed.Contains(Value))
		{
			OutError = FString::Printf(TEXT("%s must be one of: %s"), Field, *FString::Join(Allowed, TEXT(", ")));
			return FString();
		}
		return Value;
	}

	static FSvgOptions ResolveOptions(const TSharedPtr<FJsonObject>& Params, ESvgActionMode Mode, FString& OutError)
	{
		FSvgOptions Options;
		Options.Profile = NormalizeProfile(Params, OutError);
		if (!OutError.IsEmpty())
		{
			return Options;
		}

		const FString GeometryDefault = TEXT("validate");
		Options.GeometryPolicy = NormalizeEnumParam(
			Params, TEXT("geometry_policy"), GeometryDefault, GetGeometryPolicies(), OutError);
		if (!OutError.IsEmpty())
		{
			return Options;
		}

		const FString FillRuleDefault = Options.Profile == TEXT("msdf_source") ? TEXT("nonzero") : TEXT("preserve");
		Options.FillRulePolicy = NormalizeEnumParam(
			Params, TEXT("fill_rule_policy"), FillRuleDefault, GetFillRulePolicies(), OutError);
		if (!OutError.IsEmpty())
		{
			return Options;
		}

		if (!Params->TryGetBoolField(TEXT("strict"), Options.bStrict))
		{
			Options.bStrict = Mode != ESvgActionMode::Validate && Options.Profile == TEXT("msdf_source");
		}
		if (!Params->TryGetBoolField(TEXT("save"), Options.bSave))
		{
			Options.bSave = Mode != ESvgActionMode::Validate;
		}
		if (!Params->TryGetBoolField(TEXT("return_svg"), Options.bReturnSvg))
		{
			Options.bReturnSvg = !Options.bSave || Mode == ESvgActionMode::Validate;
		}
		Params->TryGetNumberField(TEXT("margin"), Options.Margin);
		if (!FMath::IsFinite(Options.Margin) || Options.Margin < 0.0 || Options.Margin > 10000.0)
		{
			OutError = TEXT("margin must be a finite number in [0, 10000]");
		}
		return Options;
	}

	static bool IsSecurityForbiddenTag(const FString& Tag)
	{
		return Tag == TEXT("script")
			|| Tag == TEXT("foreignobject")
			|| Tag == TEXT("iframe")
			|| Tag == TEXT("object")
			|| Tag == TEXT("embed")
			|| Tag == TEXT("audio")
			|| Tag == TEXT("video")
			|| Tag == TEXT("canvas")
			|| Tag == TEXT("image");
	}

	static bool IsAnimationTag(const FString& Tag)
	{
		return Tag == TEXT("animate")
			|| Tag == TEXT("animatetransform")
			|| Tag == TEXT("animatemotion")
			|| Tag == TEXT("set");
	}

	static bool IsAllowedTag(const FString& Tag)
	{
		return Tag == TEXT("svg")
			|| Tag == TEXT("g")
			|| Tag == TEXT("path")
			|| Tag == TEXT("rect")
			|| Tag == TEXT("circle")
			|| Tag == TEXT("ellipse")
			|| Tag == TEXT("polygon")
			|| Tag == TEXT("polyline")
			|| Tag == TEXT("line")
			|| Tag == TEXT("text")
			|| Tag == TEXT("defs")
			|| Tag == TEXT("lineargradient")
			|| Tag == TEXT("radialgradient")
			|| Tag == TEXT("stop");
	}

	static bool RawSvgPreflight(const FString& SvgText, FString& OutError)
	{
		if (SvgText.IsEmpty())
		{
			OutError = TEXT("SVG input is empty");
			return false;
		}
		if (SvgText.Len() > MaxSvgInputChars)
		{
			OutError = FString::Printf(TEXT("SVG input exceeds max size of %d characters"), MaxSvgInputChars);
			return false;
		}

		const FString Lower = SvgText.ToLower();
		if (Lower.Contains(TEXT("<!doctype")))
		{
			OutError = TEXT("SVG input must not contain a DOCTYPE declaration");
			return false;
		}
		if (Lower.Contains(TEXT("<!entity")))
		{
			OutError = TEXT("SVG input must not contain entity declarations");
			return false;
		}
		if (Lower.Contains(TEXT("<?xml-stylesheet")))
		{
			OutError = TEXT("SVG input must not contain xml-stylesheet processing instructions");
			return false;
		}
		if (Lower.Contains(TEXT("@import")))
		{
			OutError = TEXT("SVG input must not contain CSS imports");
			return false;
		}
		return true;
	}

	static bool AttributeHasExternalReference(const FString& Value)
	{
		const FString Lower = Value.ToLower();
		return Lower.Contains(TEXT("javascript:"))
			|| Lower.Contains(TEXT("data:"))
			|| Lower.Contains(TEXT("http://"))
			|| Lower.Contains(TEXT("https://"))
			|| Lower.Contains(TEXT("file:"))
			|| Lower.Contains(TEXT("//"));
	}

	static bool ValidateCommonAttributes(const FXmlNode& Node, FSvgState& State, FString& OutError)
	{
		for (const FXmlAttribute& Attribute : Node.GetAttributes())
		{
			const FString Name = Attribute.GetTag().ToLower();
			const FString Value = Attribute.GetValue();
			if (Name == TEXT("xmlns") || Name.StartsWith(TEXT("xmlns:")))
			{
				continue;
			}
			if (Name.StartsWith(TEXT("on")))
			{
				OutError = FString::Printf(TEXT("Event attribute '%s' is not allowed"), *Name);
				return false;
			}
			if (Name == TEXT("style"))
			{
				OutError = TEXT("Inline style attributes are not allowed in generated SVG sources");
				return false;
			}
			if (Name == TEXT("href") || Name == TEXT("xlink:href"))
			{
				const FString Trimmed = Value.TrimStartAndEnd();
				if (!Trimmed.StartsWith(TEXT("#")))
				{
					OutError = TEXT("href/xlink:href may not reference external resources");
					return false;
				}
				AddUnique(State.Removed, TEXT("href_reference"));
				AddUnique(State.Warnings, TEXT("href references are stripped; use concrete geometry instead"));
				return false;
			}
			if (AttributeHasExternalReference(Value))
			{
				OutError = FString::Printf(TEXT("Attribute '%s' contains an external or executable reference"), *Name);
				return false;
			}
			const FString Lower = Value.ToLower();
			if (Lower.Contains(TEXT("url(")) && !Lower.Contains(TEXT("url(#")))
			{
				OutError = FString::Printf(TEXT("Attribute '%s' contains a non-local url() reference"), *Name);
				return false;
			}
		}
		return true;
	}

	static bool IsSupportedPathCommand(TCHAR Command)
	{
		switch (FChar::ToUpper(Command))
		{
			case TCHAR('M'):
			case TCHAR('L'):
			case TCHAR('H'):
			case TCHAR('V'):
			case TCHAR('C'):
			case TCHAR('Q'):
			case TCHAR('Z'):
				return true;
			default:
				return false;
		}
	}

	static FPoint2 MakePoint(double X, double Y)
	{
		FPoint2 Point;
		Point.X = X;
		Point.Y = Y;
		return Point;
	}

	static FPoint2 AddPoints(const FPoint2& A, const FPoint2& B)
	{
		return MakePoint(A.X + B.X, A.Y + B.Y);
	}

	static FPoint2 EvalQuadratic(const FPoint2& P0, const FPoint2& P1, const FPoint2& P2, double T)
	{
		const double U = 1.0 - T;
		return MakePoint(
			U * U * P0.X + 2.0 * U * T * P1.X + T * T * P2.X,
			U * U * P0.Y + 2.0 * U * T * P1.Y + T * T * P2.Y);
	}

	static FPoint2 EvalCubic(const FPoint2& P0, const FPoint2& P1, const FPoint2& P2, const FPoint2& P3, double T)
	{
		const double U = 1.0 - T;
		return MakePoint(
			U * U * U * P0.X + 3.0 * U * U * T * P1.X + 3.0 * U * T * T * P2.X + T * T * T * P3.X,
			U * U * U * P0.Y + 3.0 * U * U * T * P1.Y + 3.0 * U * T * T * P2.Y + T * T * T * P3.Y);
	}

	static bool PointsNear(const FPoint2& A, const FPoint2& B)
	{
		return FMath::Abs(A.X - B.X) <= GeometryEpsilon && FMath::Abs(A.Y - B.Y) <= GeometryEpsilon;
	}

	static bool ReadPathPoint(const FString& PathData, int32& Index, FPoint2& OutPoint)
	{
		double X = 0.0;
		double Y = 0.0;
		if (!ReadNumberToken(PathData, Index, X))
		{
			return false;
		}
		if (!ReadNumberToken(PathData, Index, Y))
		{
			return false;
		}
		OutPoint = MakePoint(X, Y);
		return true;
	}

	static bool HasMoreNumbersBeforeCommand(const FString& PathData, int32 Index)
	{
		SkipSeparators(PathData, Index);
		return Index < PathData.Len() && !FChar::IsAlpha(PathData[Index]);
	}

	static FContour* EnsureCurrentContour(TArray<FContour>& Contours, const FString& Source)
	{
		if (Contours.Num() == 0)
		{
			FContour& Contour = Contours.AddDefaulted_GetRef();
			Contour.Source = Source;
		}
		return &Contours.Last();
	}

	static bool ParsePathData(
		const FString& PathData,
		const FString& FillRule,
		TArray<FContour>& OutContours,
		int32& OutCommandCount,
		FString& OutError)
	{
		OutCommandCount = 0;
		int32 Index = 0;
		TCHAR Command = 0;
		FPoint2 Current = MakePoint(0.0, 0.0);
		FPoint2 SubpathStart = MakePoint(0.0, 0.0);
		FContour* CurrentContour = nullptr;

		while (Index < PathData.Len())
		{
			SkipSeparators(PathData, Index);
			if (Index >= PathData.Len())
			{
				break;
			}

			if (FChar::IsAlpha(PathData[Index]))
			{
				Command = PathData[Index++];
				if (!IsSupportedPathCommand(Command))
				{
					OutError = FString::Printf(TEXT("Unsupported SVG path command '%c'"), Command);
					return false;
				}
				if (FChar::ToUpper(Command) == TCHAR('Z'))
				{
					if (!CurrentContour || CurrentContour->Points.Num() == 0)
					{
						OutError = TEXT("Path close command appears before a move command");
						return false;
					}
					CurrentContour->bClosed = true;
					Current = SubpathStart;
					++OutCommandCount;
					SkipSeparators(PathData, Index);
					if (Index < PathData.Len() && !FChar::IsAlpha(PathData[Index]))
					{
						OutError = TEXT("Path close command must not be followed by numeric parameters");
						return false;
					}
					continue;
				}
			}
			else if (Command == 0)
			{
				OutError = TEXT("Path data must start with a command");
				return false;
			}

			const TCHAR Upper = FChar::ToUpper(Command);
			const bool bRelative = FChar::IsLower(Command);
			if (Upper == TCHAR('M'))
			{
				FPoint2 MoveTo;
				if (!ReadPathPoint(PathData, Index, MoveTo))
				{
					OutError = TEXT("Move command requires x and y parameters");
					return false;
				}
				if (bRelative)
				{
					MoveTo = AddPoints(Current, MoveTo);
				}
				Current = MoveTo;
				SubpathStart = MoveTo;
				CurrentContour = &OutContours.AddDefaulted_GetRef();
				CurrentContour->Source = TEXT("path");
				CurrentContour->FillRule = FillRule;
				CurrentContour->Points.Add(MoveTo);
				++OutCommandCount;

				while (HasMoreNumbersBeforeCommand(PathData, Index))
				{
					FPoint2 LineTo;
					if (!ReadPathPoint(PathData, Index, LineTo))
					{
						OutError = TEXT("Implicit line command after move requires x and y parameters");
						return false;
					}
					if (bRelative)
					{
						LineTo = AddPoints(Current, LineTo);
					}
					CurrentContour->Points.Add(LineTo);
					Current = LineTo;
					++OutCommandCount;
				}
				Command = bRelative ? TCHAR('l') : TCHAR('L');
				continue;
			}

			if (!CurrentContour)
			{
				OutError = TEXT("Path geometry command appears before a move command");
				return false;
			}

			if (Upper == TCHAR('L'))
			{
				bool bReadAny = false;
				while (HasMoreNumbersBeforeCommand(PathData, Index))
				{
					FPoint2 LineTo;
					if (!ReadPathPoint(PathData, Index, LineTo))
					{
						OutError = TEXT("Line command requires x and y parameters");
						return false;
					}
					if (bRelative)
					{
						LineTo = AddPoints(Current, LineTo);
					}
					CurrentContour->Points.Add(LineTo);
					Current = LineTo;
					++OutCommandCount;
					bReadAny = true;
				}
				if (!bReadAny)
				{
					OutError = TEXT("Line command requires at least one point");
					return false;
				}
				continue;
			}

			if (Upper == TCHAR('H') || Upper == TCHAR('V'))
			{
				bool bReadAny = false;
				while (HasMoreNumbersBeforeCommand(PathData, Index))
				{
					double Value = 0.0;
					if (!ReadNumberToken(PathData, Index, Value))
					{
						OutError = Upper == TCHAR('H') ? TEXT("Horizontal line command requires x parameters") : TEXT("Vertical line command requires y parameters");
						return false;
					}
					FPoint2 LineTo = Current;
					if (Upper == TCHAR('H'))
					{
						LineTo.X = bRelative ? Current.X + Value : Value;
					}
					else
					{
						LineTo.Y = bRelative ? Current.Y + Value : Value;
					}
					CurrentContour->Points.Add(LineTo);
					Current = LineTo;
					++OutCommandCount;
					bReadAny = true;
				}
				if (!bReadAny)
				{
					OutError = Upper == TCHAR('H') ? TEXT("Horizontal line command requires at least one x parameter") : TEXT("Vertical line command requires at least one y parameter");
					return false;
				}
				continue;
			}

			if (Upper == TCHAR('Q'))
			{
				bool bReadAny = false;
				while (HasMoreNumbersBeforeCommand(PathData, Index))
				{
					FPoint2 C;
					FPoint2 End;
					if (!ReadPathPoint(PathData, Index, C) || !ReadPathPoint(PathData, Index, End))
					{
						OutError = TEXT("Quadratic curve command requires control and end points");
						return false;
					}
					if (bRelative)
					{
						C = AddPoints(Current, C);
						End = AddPoints(Current, End);
					}
					const FPoint2 Start = Current;
					for (int32 Step = 1; Step <= 8; ++Step)
					{
						CurrentContour->Points.Add(EvalQuadratic(Start, C, End, static_cast<double>(Step) / 8.0));
					}
					Current = End;
					++OutCommandCount;
					bReadAny = true;
				}
				if (!bReadAny)
				{
					OutError = TEXT("Quadratic curve command requires at least one curve segment");
					return false;
				}
				continue;
			}

			if (Upper == TCHAR('C'))
			{
				bool bReadAny = false;
				while (HasMoreNumbersBeforeCommand(PathData, Index))
				{
					FPoint2 C1;
					FPoint2 C2;
					FPoint2 End;
					if (!ReadPathPoint(PathData, Index, C1) || !ReadPathPoint(PathData, Index, C2) || !ReadPathPoint(PathData, Index, End))
					{
						OutError = TEXT("Cubic curve command requires two control points and an end point");
						return false;
					}
					if (bRelative)
					{
						C1 = AddPoints(Current, C1);
						C2 = AddPoints(Current, C2);
						End = AddPoints(Current, End);
					}
					const FPoint2 Start = Current;
					for (int32 Step = 1; Step <= 12; ++Step)
					{
						CurrentContour->Points.Add(EvalCubic(Start, C1, C2, End, static_cast<double>(Step) / 12.0));
					}
					Current = End;
					++OutCommandCount;
					bReadAny = true;
				}
				if (!bReadAny)
				{
					OutError = TEXT("Cubic curve command requires at least one curve segment");
					return false;
				}
				continue;
			}
		}

		if (OutCommandCount > MaxPathCommands)
		{
			OutError = FString::Printf(TEXT("Path command count exceeds limit %d"), MaxPathCommands);
			return false;
		}
		return true;
	}

	static double Cross(const FPoint2& A, const FPoint2& B, const FPoint2& C)
	{
		return (B.X - A.X) * (C.Y - A.Y) - (B.Y - A.Y) * (C.X - A.X);
	}

	static bool OnSegment(const FPoint2& A, const FPoint2& B, const FPoint2& P)
	{
		return FMath::Abs(Cross(A, B, P)) <= GeometryEpsilon
			&& P.X >= FMath::Min(A.X, B.X) - GeometryEpsilon
			&& P.X <= FMath::Max(A.X, B.X) + GeometryEpsilon
			&& P.Y >= FMath::Min(A.Y, B.Y) - GeometryEpsilon
			&& P.Y <= FMath::Max(A.Y, B.Y) + GeometryEpsilon;
	}

	static bool SegmentsIntersect(const FPoint2& A, const FPoint2& B, const FPoint2& C, const FPoint2& D)
	{
		const double C1 = Cross(A, B, C);
		const double C2 = Cross(A, B, D);
		const double C3 = Cross(C, D, A);
		const double C4 = Cross(C, D, B);

		if (((C1 > GeometryEpsilon && C2 < -GeometryEpsilon) || (C1 < -GeometryEpsilon && C2 > GeometryEpsilon))
			&& ((C3 > GeometryEpsilon && C4 < -GeometryEpsilon) || (C3 < -GeometryEpsilon && C4 > GeometryEpsilon)))
		{
			return true;
		}

		return OnSegment(A, B, C) || OnSegment(A, B, D) || OnSegment(C, D, A) || OnSegment(C, D, B);
	}

	static double SignedArea(const FContour& Contour)
	{
		if (Contour.Points.Num() < 3)
		{
			return 0.0;
		}

		double Area = 0.0;
		const int32 Count = Contour.Points.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FPoint2& A = Contour.Points[Index];
			const FPoint2& B = Contour.Points[(Index + 1) % Count];
			Area += A.X * B.Y - B.X * A.Y;
		}
		return Area * 0.5;
	}

	static bool PointInPolygon(const FPoint2& Point, const FContour& Contour)
	{
		bool bInside = false;
		const int32 Count = Contour.Points.Num();
		for (int32 I = 0, J = Count - 1; I < Count; J = I++)
		{
			const FPoint2& Pi = Contour.Points[I];
			const FPoint2& Pj = Contour.Points[J];
			const bool bCrosses = ((Pi.Y > Point.Y) != (Pj.Y > Point.Y))
				&& (Point.X < (Pj.X - Pi.X) * (Point.Y - Pi.Y) / (Pj.Y - Pi.Y) + Pi.X);
			if (bCrosses)
			{
				bInside = !bInside;
			}
		}
		return bInside;
	}

	static void ExpandBounds(FBounds& Bounds, const FPoint2& Point)
	{
		if (!Bounds.bValid)
		{
			Bounds.MinX = Bounds.MaxX = Point.X;
			Bounds.MinY = Bounds.MaxY = Point.Y;
			Bounds.bValid = true;
			return;
		}
		Bounds.MinX = FMath::Min(Bounds.MinX, Point.X);
		Bounds.MinY = FMath::Min(Bounds.MinY, Point.Y);
		Bounds.MaxX = FMath::Max(Bounds.MaxX, Point.X);
		Bounds.MaxY = FMath::Max(Bounds.MaxY, Point.Y);
	}

	static void ValidateGeometry(FSvgState& State)
	{
		State.Bounds = FBounds();
		State.OpenContourCount = 0;
		State.SelfIntersectionCount = 0;
		State.OverlapCount = 0;
		State.WindingIssues.Reset();

		if (State.GeometryPolicy == TEXT("sanitize_only"))
		{
			State.bGeometryValid = false;
			AddUnique(State.Warnings, TEXT("geometry_policy=sanitize_only skipped topology validation"));
			State.bMsdfReady = false;
			return;
		}

		for (FContour& Contour : State.Contours)
		{
			for (const FPoint2& Point : Contour.Points)
			{
				ExpandBounds(State.Bounds, Point);
			}

			if (!Contour.bClosed)
			{
				++State.OpenContourCount;
				AddUnique(State.Blockers, TEXT("open_contour"));
			}
			if (Contour.Points.Num() < 3 || FMath::Abs(SignedArea(Contour)) <= GeometryEpsilon)
			{
				AddUnique(State.Blockers, TEXT("zero_area_contour"));
			}
			for (int32 Index = 1; Index < Contour.Points.Num(); ++Index)
			{
				if (PointsNear(Contour.Points[Index - 1], Contour.Points[Index]))
				{
					AddUnique(State.Blockers, TEXT("duplicate_adjacent_points"));
					break;
				}
			}
			if (Contour.bClosed && Contour.Points.Num() > 1 && PointsNear(Contour.Points[0], Contour.Points.Last()))
			{
				AddUnique(State.Blockers, TEXT("duplicate_adjacent_points"));
			}
		}

		for (int32 ContourIndex = 0; ContourIndex < State.Contours.Num(); ++ContourIndex)
		{
			const FContour& Contour = State.Contours[ContourIndex];
			if (!Contour.bClosed || Contour.Points.Num() < 4)
			{
				continue;
			}
			const int32 SegmentCount = Contour.Points.Num();
			for (int32 AIndex = 0; AIndex < SegmentCount; ++AIndex)
			{
				const FPoint2& A0 = Contour.Points[AIndex];
				const FPoint2& A1 = Contour.Points[(AIndex + 1) % SegmentCount];
				for (int32 BIndex = AIndex + 1; BIndex < SegmentCount; ++BIndex)
				{
					const bool bAdjacent = FMath::Abs(AIndex - BIndex) <= 1 || (AIndex == 0 && BIndex == SegmentCount - 1);
					if (bAdjacent)
					{
						continue;
					}
					const FPoint2& B0 = Contour.Points[BIndex];
					const FPoint2& B1 = Contour.Points[(BIndex + 1) % SegmentCount];
					if (SegmentsIntersect(A0, A1, B0, B1))
					{
						++State.SelfIntersectionCount;
					}
				}
			}
		}
		if (State.SelfIntersectionCount > 0)
		{
			AddUnique(State.Blockers, TEXT("self_intersection"));
		}

		for (int32 AContour = 0; AContour < State.Contours.Num(); ++AContour)
		{
			const FContour& A = State.Contours[AContour];
			if (!A.bClosed || A.Points.Num() < 3)
			{
				continue;
			}
			for (int32 BContour = AContour + 1; BContour < State.Contours.Num(); ++BContour)
			{
				const FContour& B = State.Contours[BContour];
				if (!B.bClosed || B.Points.Num() < 3)
				{
					continue;
				}
				for (int32 AIndex = 0; AIndex < A.Points.Num(); ++AIndex)
				{
					const FPoint2& A0 = A.Points[AIndex];
					const FPoint2& A1 = A.Points[(AIndex + 1) % A.Points.Num()];
					for (int32 BIndex = 0; BIndex < B.Points.Num(); ++BIndex)
					{
						const FPoint2& B0 = B.Points[BIndex];
						const FPoint2& B1 = B.Points[(BIndex + 1) % B.Points.Num()];
						if (SegmentsIntersect(A0, A1, B0, B1))
						{
							++State.OverlapCount;
						}
					}
				}

				const bool bBInsideA = PointInPolygon(B.Points[0], A);
				const bool bAInsideB = PointInPolygon(A.Points[0], B);
				if (bBInsideA || bAInsideB)
				{
					const double AreaA = SignedArea(A);
					const double AreaB = SignedArea(B);
					if ((AreaA > 0.0 && AreaB > 0.0) || (AreaA < 0.0 && AreaB < 0.0))
					{
						AddUnique(State.WindingIssues, TEXT("contained contour uses same winding as container"));
					}
				}
			}
		}
		if (State.OverlapCount > 0)
		{
			AddUnique(State.Blockers, TEXT("intersecting_or_overlapping_contours"));
		}
		if (State.WindingIssues.Num() > 0)
		{
			AddUnique(State.Blockers, TEXT("wrong_hole_winding"));
		}

		if (State.Profile == TEXT("msdf_source"))
		{
			if (State.bSawText)
			{
				AddUnique(State.Blockers, TEXT("text_not_converted_to_paths"));
			}
			if (State.bSawGradient)
			{
				AddUnique(State.Blockers, TEXT("gradient_fill"));
			}
			if (State.bSawStroke)
			{
				AddUnique(State.Blockers, TEXT("stroke_not_expanded"));
			}
			if (State.bSawTransform)
			{
				AddUnique(State.Blockers, TEXT("unflattened_transform"));
			}
			if (State.EffectiveFillRule == TEXT("evenodd"))
			{
				AddUnique(State.Blockers, TEXT("evenodd_fill_rule"));
			}
			if (State.Contours.Num() == 0)
			{
				AddUnique(State.Blockers, TEXT("no_path_contours"));
			}
		}

		State.bGeometryValid = State.Blockers.Num() == 0;
		State.bMsdfReady = State.bGeometryValid && State.Contours.Num() > 0;
	}

	static bool ParsePointsAttribute(const FString& PointsText, TArray<FPoint2>& OutPoints, FString& OutError)
	{
		TArray<double> Values;
		if (!ExtractNumberList(PointsText, Values, OutError))
		{
			return false;
		}
		if (Values.Num() < 4 || (Values.Num() % 2) != 0)
		{
			OutError = TEXT("points must contain an even number of coordinates");
			return false;
		}
		for (int32 Index = 0; Index + 1 < Values.Num(); Index += 2)
		{
			OutPoints.Add(MakePoint(Values[Index], Values[Index + 1]));
		}
		return true;
	}

	static void AddRectContour(FSvgState& State, double X, double Y, double W, double H, const FString& FillRule)
	{
		FContour& Contour = State.Contours.AddDefaulted_GetRef();
		Contour.Source = TEXT("rect");
		Contour.FillRule = FillRule;
		Contour.bClosed = true;
		Contour.Points.Add(MakePoint(X, Y));
		Contour.Points.Add(MakePoint(X + W, Y));
		Contour.Points.Add(MakePoint(X + W, Y + H));
		Contour.Points.Add(MakePoint(X, Y + H));
	}

	static void AddEllipseContour(FSvgState& State, double CX, double CY, double RX, double RY, const FString& FillRule)
	{
		FContour& Contour = State.Contours.AddDefaulted_GetRef();
		Contour.Source = TEXT("ellipse");
		Contour.FillRule = FillRule;
		Contour.bClosed = true;
		for (int32 Index = 0; Index < 24; ++Index)
		{
			const double Angle = 2.0 * PI * static_cast<double>(Index) / 24.0;
			Contour.Points.Add(MakePoint(CX + FMath::Cos(Angle) * RX, CY + FMath::Sin(Angle) * RY));
		}
	}

	static FString GetAttributeOrDefault(const FXmlNode& Node, const TCHAR* Name, const FString& DefaultValue)
	{
		const FString Value = Node.GetAttribute(Name);
		return Value.IsEmpty() ? DefaultValue : Value;
	}

	static FString NormalizeFillRule(const FString& RawFillRule, const FSvgOptions& Options, FSvgState& State)
	{
		FString FillRule = RawFillRule.TrimStartAndEnd().ToLower();
		if (FillRule.IsEmpty())
		{
			FillRule = Options.FillRulePolicy == TEXT("nonzero") ? TEXT("nonzero") : TEXT("nonzero");
		}
		if (FillRule != TEXT("nonzero") && FillRule != TEXT("evenodd"))
		{
			AddUnique(State.Warnings, FString::Printf(TEXT("Unsupported fill-rule '%s' normalized to nonzero"), *FillRule));
			AddUnique(State.RepairActions, TEXT("fill_rule_nonzero"));
			FillRule = TEXT("nonzero");
		}
		if (FillRule == TEXT("evenodd"))
		{
			State.EffectiveFillRule = TEXT("evenodd");
			if (Options.Profile == TEXT("msdf_source"))
			{
				AddUnique(State.Blockers, TEXT("evenodd_fill_rule"));
			}
		}
		return FillRule;
	}

	static FString SanitizePaintValue(const FString& RawValue, const FString& DefaultValue, FSvgState& State)
	{
		FString Value = RawValue.TrimStartAndEnd();
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}
		const FString Lower = Value.ToLower();
		if (Lower.StartsWith(TEXT("url(#")))
		{
			State.bSawGradient = true;
			return Value;
		}
		if (Lower == TEXT("none"))
		{
			return TEXT("none");
		}
		return Value;
	}

	static bool AppendNumericAttribute(
		const FXmlNode& Node,
		const TCHAR* AttributeName,
		TArray<FString>& OutAttributes,
		double& OutValue,
		FString& OutError,
		bool bRequired = true,
		double DefaultValue = 0.0)
	{
		const FString Raw = Node.GetAttribute(AttributeName);
		if (Raw.IsEmpty())
		{
			if (!bRequired)
			{
				OutValue = DefaultValue;
				return true;
			}
			OutError = FString::Printf(TEXT("Missing required numeric attribute '%s'"), AttributeName);
			return false;
		}
		if (!ParseLength(Raw, OutValue) || FMath::Abs(OutValue) > MaxCoordinateMagnitude)
		{
			OutError = FString::Printf(TEXT("Invalid numeric attribute '%s'"), AttributeName);
			return false;
		}
		OutAttributes.Add(FString::Printf(TEXT("%s=\"%s\""), AttributeName, *FormatNumber(OutValue)));
		return true;
	}

	static bool AppendPassthroughAttribute(
		const FXmlNode& Node,
		const TCHAR* AttributeName,
		TArray<FString>& OutAttributes,
		const FString& DefaultValue = FString())
	{
		FString Value = Node.GetAttribute(AttributeName);
		if (Value.IsEmpty())
		{
			Value = DefaultValue;
		}
		if (!Value.IsEmpty())
		{
			OutAttributes.Add(FString::Printf(TEXT("%s=\"%s\""), AttributeName, *XmlEscape(Value)));
		}
		return !Value.IsEmpty();
	}

	static bool CheckUnsupportedAttributes(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, FString& OutError)
	{
		if (!ValidateCommonAttributes(Node, State, OutError))
		{
			return false;
		}

		const FString Transform = Node.GetAttribute(TEXT("transform"));
		if (!Transform.IsEmpty())
		{
			State.bSawTransform = true;
			AddUnique(State.Removed, TEXT("transform"));
			AddUnique(State.Warnings, TEXT("transform attributes are not flattened by the P1 SVG source sanitizer"));
			if (Options.Profile == TEXT("msdf_source"))
			{
				AddUnique(State.Blockers, TEXT("unflattened_transform"));
			}
		}

		const FString Stroke = Node.GetAttribute(TEXT("stroke"));
		if (!Stroke.IsEmpty() && Stroke.ToLower() != TEXT("none"))
		{
			State.bSawStroke = true;
			if (Options.Profile == TEXT("msdf_source"))
			{
				AddUnique(State.Blockers, TEXT("stroke_not_expanded"));
			}
		}
		return true;
	}

	static bool SanitizeNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, int32 Depth, FString& OutXml, FString& OutError);

	static bool SanitizeChildren(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, int32 Depth, FString& OutXml, FString& OutError)
	{
		for (const FXmlNode* Child : Node.GetChildrenNodes())
		{
			if (Child)
			{
				FString ChildXml;
				if (!SanitizeNode(*Child, Options, State, Depth + 1, ChildXml, OutError))
				{
					return false;
				}
				OutXml += ChildXml;
			}
		}
		return true;
	}

	static bool SanitizePathNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, FString& OutXml, FString& OutError)
	{
		const FString PathData = Node.GetAttribute(TEXT("d"));
		if (PathData.IsEmpty())
		{
			OutError = TEXT("path element is missing required d attribute");
			return false;
		}

		const FString FillRule = NormalizeFillRule(Node.GetAttribute(TEXT("fill-rule")), Options, State);
		TArray<FContour> PathContours;
		int32 CommandCount = 0;
		if (!ParsePathData(PathData, FillRule, PathContours, CommandCount, OutError))
		{
			return false;
		}
		State.PathCommandCount += CommandCount;
		if (State.PathCommandCount > MaxPathCommands)
		{
			OutError = FString::Printf(TEXT("SVG path command count exceeds limit %d"), MaxPathCommands);
			return false;
		}
		State.PathCount += 1;
		State.Contours.Append(PathContours);

		TArray<FString> Attributes;
		Attributes.Add(FString::Printf(TEXT("d=\"%s\""), *XmlEscape(PathData)));
		Attributes.Add(FString::Printf(TEXT("fill=\"%s\""), *XmlEscape(SanitizePaintValue(Node.GetAttribute(TEXT("fill")), TEXT("#ffffff"), State))));
		if (FillRule != TEXT("nonzero"))
		{
			Attributes.Add(FString::Printf(TEXT("fill-rule=\"%s\""), *FillRule));
		}
		const FString Stroke = Node.GetAttribute(TEXT("stroke"));
		if (!Stroke.IsEmpty() && Options.Profile != TEXT("msdf_source"))
		{
			Attributes.Add(FString::Printf(TEXT("stroke=\"%s\""), *XmlEscape(Stroke)));
			AppendPassthroughAttribute(Node, TEXT("stroke-width"), Attributes);
		}

		OutXml = FString::Printf(TEXT("<path %s/>"), *FString::Join(Attributes, TEXT(" ")));
		return true;
	}

	static bool SanitizeRectNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, FString& OutXml, FString& OutError)
	{
		TArray<FString> Attributes;
		double X = 0.0;
		double Y = 0.0;
		double W = 0.0;
		double H = 0.0;
		if (!AppendNumericAttribute(Node, TEXT("x"), Attributes, X, OutError, false, 0.0)
			|| !AppendNumericAttribute(Node, TEXT("y"), Attributes, Y, OutError, false, 0.0)
			|| !AppendNumericAttribute(Node, TEXT("width"), Attributes, W, OutError)
			|| !AppendNumericAttribute(Node, TEXT("height"), Attributes, H, OutError))
		{
			return false;
		}
		if (W <= 0.0 || H <= 0.0)
		{
			OutError = TEXT("rect width and height must be positive");
			return false;
		}
		const FString FillRule = NormalizeFillRule(Node.GetAttribute(TEXT("fill-rule")), Options, State);
		Attributes.Add(FString::Printf(TEXT("fill=\"%s\""), *XmlEscape(SanitizePaintValue(Node.GetAttribute(TEXT("fill")), TEXT("#ffffff"), State))));
		if (FillRule != TEXT("nonzero"))
		{
			Attributes.Add(FString::Printf(TEXT("fill-rule=\"%s\""), *FillRule));
		}
		AddRectContour(State, X, Y, W, H, FillRule);
		OutXml = FString::Printf(TEXT("<rect %s/>"), *FString::Join(Attributes, TEXT(" ")));
		return true;
	}

	static bool SanitizeEllipseNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, const FString& Tag, FString& OutXml, FString& OutError)
	{
		TArray<FString> Attributes;
		double CX = 0.0;
		double CY = 0.0;
		double RX = 0.0;
		double RY = 0.0;
		if (!AppendNumericAttribute(Node, TEXT("cx"), Attributes, CX, OutError, false, 0.0)
			|| !AppendNumericAttribute(Node, TEXT("cy"), Attributes, CY, OutError, false, 0.0))
		{
			return false;
		}
		if (Tag == TEXT("circle"))
		{
			double R = 0.0;
			if (!AppendNumericAttribute(Node, TEXT("r"), Attributes, R, OutError) || R <= 0.0)
			{
				OutError = TEXT("circle r must be positive");
				return false;
			}
			RX = RY = R;
		}
		else
		{
			if (!AppendNumericAttribute(Node, TEXT("rx"), Attributes, RX, OutError)
				|| !AppendNumericAttribute(Node, TEXT("ry"), Attributes, RY, OutError)
				|| RX <= 0.0
				|| RY <= 0.0)
			{
				OutError = TEXT("ellipse rx and ry must be positive");
				return false;
			}
		}
		const FString FillRule = NormalizeFillRule(Node.GetAttribute(TEXT("fill-rule")), Options, State);
		Attributes.Add(FString::Printf(TEXT("fill=\"%s\""), *XmlEscape(SanitizePaintValue(Node.GetAttribute(TEXT("fill")), TEXT("#ffffff"), State))));
		if (FillRule != TEXT("nonzero"))
		{
			Attributes.Add(FString::Printf(TEXT("fill-rule=\"%s\""), *FillRule));
		}
		AddEllipseContour(State, CX, CY, RX, RY, FillRule);
		OutXml = FString::Printf(TEXT("<%s %s/>"), *Tag, *FString::Join(Attributes, TEXT(" ")));
		return true;
	}

	static bool SanitizePolygonNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, const FString& Tag, FString& OutXml, FString& OutError)
	{
		const FString PointsText = Node.GetAttribute(TEXT("points"));
		TArray<FPoint2> Points;
		if (!ParsePointsAttribute(PointsText, Points, OutError))
		{
			return false;
		}

		const FString FillRule = NormalizeFillRule(Node.GetAttribute(TEXT("fill-rule")), Options, State);
		FContour& Contour = State.Contours.AddDefaulted_GetRef();
		Contour.Source = Tag;
		Contour.FillRule = FillRule;
		Contour.Points = Points;
		Contour.bClosed = Tag == TEXT("polygon");
		if (!Contour.bClosed && Options.Profile == TEXT("msdf_source"))
		{
			AddUnique(State.Blockers, TEXT("open_contour"));
		}

		TArray<FString> Attributes;
		Attributes.Add(FString::Printf(TEXT("points=\"%s\""), *XmlEscape(PointsText)));
		Attributes.Add(FString::Printf(TEXT("fill=\"%s\""), *XmlEscape(SanitizePaintValue(Node.GetAttribute(TEXT("fill")), Tag == TEXT("polygon") ? TEXT("#ffffff") : TEXT("none"), State))));
		if (FillRule != TEXT("nonzero"))
		{
			Attributes.Add(FString::Printf(TEXT("fill-rule=\"%s\""), *FillRule));
		}
		OutXml = FString::Printf(TEXT("<%s %s/>"), *Tag, *FString::Join(Attributes, TEXT(" ")));
		return true;
	}

	static bool SanitizeLineNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, FString& OutXml, FString& OutError)
	{
		TArray<FString> Attributes;
		double X1 = 0.0;
		double Y1 = 0.0;
		double X2 = 0.0;
		double Y2 = 0.0;
		if (!AppendNumericAttribute(Node, TEXT("x1"), Attributes, X1, OutError, false, 0.0)
			|| !AppendNumericAttribute(Node, TEXT("y1"), Attributes, Y1, OutError, false, 0.0)
			|| !AppendNumericAttribute(Node, TEXT("x2"), Attributes, X2, OutError, false, 0.0)
			|| !AppendNumericAttribute(Node, TEXT("y2"), Attributes, Y2, OutError, false, 0.0))
		{
			return false;
		}
		FContour& Contour = State.Contours.AddDefaulted_GetRef();
		Contour.Source = TEXT("line");
		Contour.Points.Add(MakePoint(X1, Y1));
		Contour.Points.Add(MakePoint(X2, Y2));
		Contour.bClosed = false;
		State.bSawStroke = true;
		if (Options.Profile == TEXT("msdf_source"))
		{
			AddUnique(State.Blockers, TEXT("open_contour"));
			AddUnique(State.Blockers, TEXT("stroke_not_expanded"));
		}
		Attributes.Add(FString::Printf(TEXT("stroke=\"%s\""), *XmlEscape(SanitizePaintValue(Node.GetAttribute(TEXT("stroke")), TEXT("#ffffff"), State))));
		AppendPassthroughAttribute(Node, TEXT("stroke-width"), Attributes, TEXT("1"));
		OutXml = FString::Printf(TEXT("<line %s/>"), *FString::Join(Attributes, TEXT(" ")));
		return true;
	}

	static bool SanitizeGradientNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, const FString& Tag, FString& OutXml, FString& OutError)
	{
		State.bSawGradient = true;
		if (Options.Profile == TEXT("msdf_source"))
		{
			AddUnique(State.Blockers, TEXT("gradient_fill"));
		}
		TArray<FString> Attributes;
		AppendPassthroughAttribute(Node, TEXT("id"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("x1"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("y1"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("x2"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("y2"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("cx"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("cy"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("r"), Attributes);

		FString Children;
		if (!SanitizeChildren(Node, Options, State, 0, Children, OutError))
		{
			return false;
		}
		OutXml = FString::Printf(TEXT("<%s %s>%s</%s>"), *Tag, *FString::Join(Attributes, TEXT(" ")), *Children, *Tag);
		return true;
	}

	static bool SanitizeStopNode(const FXmlNode& Node, FSvgState&, FString& OutXml)
	{
		TArray<FString> Attributes;
		AppendPassthroughAttribute(Node, TEXT("offset"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("stop-color"), Attributes);
		AppendPassthroughAttribute(Node, TEXT("stop-opacity"), Attributes);
		OutXml = FString::Printf(TEXT("<stop %s/>"), *FString::Join(Attributes, TEXT(" ")));
		return true;
	}

	static bool SanitizeNode(const FXmlNode& Node, const FSvgOptions& Options, FSvgState& State, int32 Depth, FString& OutXml, FString& OutError)
	{
		if (Depth > MaxSvgDepth)
		{
			OutError = FString::Printf(TEXT("SVG XML depth exceeds limit %d"), MaxSvgDepth);
			return false;
		}
		++State.ElementCount;
		if (State.ElementCount > MaxSvgElements)
		{
			OutError = FString::Printf(TEXT("SVG element count exceeds limit %d"), MaxSvgElements);
			return false;
		}

		const FString Tag = StripNamespace(Node.GetTag());
		if (IsSecurityForbiddenTag(Tag) || IsAnimationTag(Tag) || Tag == TEXT("style"))
		{
			OutError = FString::Printf(TEXT("SVG tag <%s> is not allowed"), *Tag);
			return false;
		}
		if (!IsAllowedTag(Tag))
		{
			AddUnique(State.Removed, FString::Printf(TEXT("unsupported_tag:%s"), *Tag));
			AddUnique(State.Warnings, FString::Printf(TEXT("Unsupported SVG tag <%s> stripped"), *Tag));
			return true;
		}
		if (!CheckUnsupportedAttributes(Node, Options, State, OutError))
		{
			return false;
		}

		if (Tag == TEXT("path"))
		{
			return SanitizePathNode(Node, Options, State, OutXml, OutError);
		}
		if (Tag == TEXT("rect"))
		{
			return SanitizeRectNode(Node, Options, State, OutXml, OutError);
		}
		if (Tag == TEXT("circle") || Tag == TEXT("ellipse"))
		{
			return SanitizeEllipseNode(Node, Options, State, Tag, OutXml, OutError);
		}
		if (Tag == TEXT("polygon") || Tag == TEXT("polyline"))
		{
			return SanitizePolygonNode(Node, Options, State, Tag, OutXml, OutError);
		}
		if (Tag == TEXT("line"))
		{
			return SanitizeLineNode(Node, Options, State, OutXml, OutError);
		}
		if (Tag == TEXT("defs"))
		{
			FString Children;
			if (!SanitizeChildren(Node, Options, State, Depth, Children, OutError))
			{
				return false;
			}
			OutXml = Children.IsEmpty() ? FString() : FString::Printf(TEXT("<defs>%s</defs>"), *Children);
			return true;
		}
		if (Tag == TEXT("lineargradient") || Tag == TEXT("radialgradient"))
		{
			return SanitizeGradientNode(Node, Options, State, Tag, OutXml, OutError);
		}
		if (Tag == TEXT("stop"))
		{
			return SanitizeStopNode(Node, State, OutXml);
		}
		if (Tag == TEXT("text"))
		{
			State.bSawText = true;
			if (Options.Profile == TEXT("msdf_source"))
			{
				AddUnique(State.Blockers, TEXT("text_not_converted_to_paths"));
			}
			TArray<FString> Attributes;
			AppendPassthroughAttribute(Node, TEXT("x"), Attributes);
			AppendPassthroughAttribute(Node, TEXT("y"), Attributes);
			AppendPassthroughAttribute(Node, TEXT("font-size"), Attributes);
			Attributes.Add(FString::Printf(TEXT("fill=\"%s\""), *XmlEscape(SanitizePaintValue(Node.GetAttribute(TEXT("fill")), TEXT("#ffffff"), State))));
			OutXml = FString::Printf(TEXT("<text %s>%s</text>"), *FString::Join(Attributes, TEXT(" ")), *XmlEscape(Node.GetContent()));
			return true;
		}
		if (Tag == TEXT("g"))
		{
			FString Children;
			if (!SanitizeChildren(Node, Options, State, Depth, Children, OutError))
			{
				return false;
			}
			OutXml = Children.IsEmpty() ? FString() : FString::Printf(TEXT("<g>%s</g>"), *Children);
			return true;
		}

		OutError = TEXT("Internal sanitizer tag routing error");
		return false;
	}

	static bool SanitizeSvgText(const FString& SvgText, const FSvgOptions& Options, FSvgState& State, FString& OutError)
	{
		if (!RawSvgPreflight(SvgText, OutError))
		{
			return false;
		}

		FXmlFile Xml(SvgText, EConstructMethod::ConstructFromBuffer);
		if (!Xml.IsValid() || !Xml.GetRootNode())
		{
			OutError = FString::Printf(TEXT("SVG XML parse failed: %s"), *Xml.GetLastError());
			return false;
		}

		const FXmlNode* Root = Xml.GetRootNode();
		if (StripNamespace(Root->GetTag()) != TEXT("svg"))
		{
			OutError = TEXT("SVG root element must be <svg>");
			return false;
		}
		if (!ValidateCommonAttributes(*Root, State, OutError))
		{
			return false;
		}

		FViewBox ViewBox;
		const FString ViewBoxAttr = Root->GetAttribute(TEXT("viewBox"));
		if (!ViewBoxAttr.IsEmpty())
		{
			if (!ParseViewBoxString(ViewBoxAttr, ViewBox, OutError))
			{
				return false;
			}
		}
		else
		{
			double Width = 0.0;
			double Height = 0.0;
			if (!ParseLength(Root->GetAttribute(TEXT("width")), Width) || !ParseLength(Root->GetAttribute(TEXT("height")), Height) || Width <= 0.0 || Height <= 0.0)
			{
				OutError = TEXT("SVG root requires a valid viewBox or positive width and height");
				return false;
			}
			ViewBox.X = 0.0;
			ViewBox.Y = 0.0;
			ViewBox.W = Width;
			ViewBox.H = Height;
			ViewBox.bValid = true;
			AddUnique(State.RepairActions, TEXT("derived_viewBox_from_width_height"));
		}
		State.ViewBox = ViewBox;

		FString ChildrenXml;
		if (!SanitizeChildren(*Root, Options, State, 0, ChildrenXml, OutError))
		{
			return false;
		}

		TArray<FString> RootAttributes;
		RootAttributes.Add(TEXT("xmlns=\"http://www.w3.org/2000/svg\""));
		RootAttributes.Add(FString::Printf(TEXT("viewBox=\"%s\""), *FormatViewBox(ViewBox)));
		const FString WidthAttr = Root->GetAttribute(TEXT("width"));
		const FString HeightAttr = Root->GetAttribute(TEXT("height"));
		double Width = 0.0;
		double Height = 0.0;
		if (!WidthAttr.IsEmpty() && ParseLength(WidthAttr, Width) && Width > 0.0)
		{
			RootAttributes.Add(FString::Printf(TEXT("width=\"%s\""), *FormatNumber(Width)));
		}
		if (!HeightAttr.IsEmpty() && ParseLength(HeightAttr, Height) && Height > 0.0)
		{
			RootAttributes.Add(FString::Printf(TEXT("height=\"%s\""), *FormatNumber(Height)));
		}

		State.SanitizedSvg = FString::Printf(TEXT("<svg %s>%s</svg>"), *FString::Join(RootAttributes, TEXT(" ")), *ChildrenXml);
		if (State.SanitizedSvg.Len() > MaxSvgOutputChars)
		{
			OutError = FString::Printf(TEXT("Sanitized SVG output exceeds max size of %d characters"), MaxSvgOutputChars);
			return false;
		}
		State.bSvgValid = true;
		State.bSanitized = true;
		ValidateGeometry(State);
		State.SvgHash = HashStringUtf8(State.SanitizedSvg);
		return true;
	}

	static FString SanitizeVectorName(const FString& Input)
	{
		FString Sanitized = Input.Left(64);
		const FString InvalidChars = TEXT(" .,:;'\"\\/?!@#$%^&*()[]{}|<>~`+=\t\r\n");
		for (int32 Index = 0; Index < InvalidChars.Len(); ++Index)
		{
			const FString InvalidChar = InvalidChars.Mid(Index, 1);
			Sanitized = Sanitized.Replace(*InvalidChar, TEXT("_"));
		}
		while (Sanitized.Contains(TEXT("__")))
		{
			Sanitized = Sanitized.Replace(TEXT("__"), TEXT("_"));
		}
		Sanitized.TrimStartAndEndInline();
		while (Sanitized.StartsWith(TEXT("_")))
		{
			Sanitized.RightChopInline(1);
		}
		while (Sanitized.EndsWith(TEXT("_")))
		{
			Sanitized.LeftChopInline(1);
		}
		if (Sanitized.IsEmpty())
		{
			Sanitized = TEXT("GeneratedVector");
		}
		if (FChar::IsDigit(Sanitized[0]))
		{
			Sanitized = TEXT("Generated_") + Sanitized;
		}
		if (!Sanitized.StartsWith(TEXT("V_")))
		{
			Sanitized = TEXT("V_") + Sanitized;
		}
		return Sanitized;
	}

	static FString PromptToVectorName(const FString& Prompt)
	{
		return SanitizeVectorName(Prompt.Left(48));
	}

	static bool ResolveSvgDestinationPackage(const TSharedPtr<FJsonObject>& Params, const FString& FallbackAssetName, FString& OutPackagePath, FString& OutError)
	{
		FString Destination;
		Params->TryGetStringField(TEXT("destination"), Destination);
		if (!Destination.IsEmpty())
		{
			if (Destination.EndsWith(TEXT(".svg")))
			{
				Destination.LeftChopInline(4);
			}
			if (Destination.EndsWith(TEXT(".uasset")))
			{
				Destination.LeftChopInline(7);
			}
			OutError = MonolithCore::ValidatePackagePath(Destination);
			OutPackagePath = Destination;
			return OutError.IsEmpty();
		}

		FString AssetPath;
		if (!Params->TryGetStringField(TEXT("asset_path"), AssetPath) || AssetPath.IsEmpty())
		{
			AssetPath = DefaultVectorAssetPath;
		}
		if (AssetPath.EndsWith(TEXT("/")))
		{
			AssetPath.LeftChopInline(1);
		}
		if (const FString PathError = MonolithCore::ValidatePackagePath(AssetPath / TEXT("__Probe")); !PathError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid asset_path '%s': %s"), *AssetPath, *PathError);
			return false;
		}

		FString AssetName;
		if (!Params->TryGetStringField(TEXT("asset_name"), AssetName) || AssetName.IsEmpty())
		{
			AssetName = FallbackAssetName;
		}
		OutPackagePath = AssetPath / SanitizeVectorName(AssetName);
		OutError = MonolithCore::ValidatePackagePath(OutPackagePath);
		return OutError.IsEmpty();
	}

	static FString ResolveSvgSourcePath(const FString& PackagePath)
	{
		FString RelativePath = PackagePath;
		const FString GeneratedRoot = FString(TEXT("/Game/GeneratedImages/"));
		if (RelativePath.StartsWith(GeneratedRoot))
		{
			RelativePath.RightChopInline(GeneratedRoot.Len());
		}
		else if (RelativePath.StartsWith(TEXT("/Game/")))
		{
			RelativePath.RightChopInline(6);
		}
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (RelativePath.StartsWith(TEXT("/")))
		{
			RelativePath.RightChopInline(1);
		}
		RelativePath.ReplaceInline(TEXT(".."), TEXT("_"));
		if (RelativePath.IsEmpty())
		{
			RelativePath = TEXT("Vector/GeneratedVector");
		}
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), VectorDirectoryName, RelativePath + TEXT(".svg")));
	}

	static void MakeUniqueSvgPaths(FString& InOutPackagePath, FString& InOutSvgPath)
	{
		if (!FPaths::FileExists(InOutSvgPath))
		{
			return;
		}

		const FString BasePackage = InOutPackagePath;
		const FString PackageDir = FPackageName::GetLongPackagePath(BasePackage);
		const FString BaseName = FPackageName::GetLongPackageAssetName(BasePackage);
		for (int32 Index = 1; Index < 1000; ++Index)
		{
			const FString CandidateName = FString::Printf(TEXT("%s_%03d"), *BaseName, Index);
			const FString CandidatePackage = PackageDir / CandidateName;
			const FString CandidatePath = ResolveSvgSourcePath(CandidatePackage);
			if (!FPaths::FileExists(CandidatePath))
			{
				InOutPackagePath = CandidatePackage;
				InOutSvgPath = CandidatePath;
				return;
			}
		}
	}

	static FString JsonObjectToString(const TSharedRef<FJsonObject>& Object)
	{
		FString Output;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Object, Writer);
		return Output;
	}

	static bool SaveUtf8TextFile(const FString& Path, const FString& Text, FString& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			OutError = FString::Printf(TEXT("Failed to write '%s'"), *Path);
			return false;
		}
		return true;
	}

	static TSharedPtr<FJsonObject> BoundsToJson(const FSvgState& State)
	{
		TSharedPtr<FJsonObject> Obj = MakeShared<FJsonObject>();
		if (State.Bounds.bValid)
		{
			Obj->SetNumberField(TEXT("min_x"), State.Bounds.MinX);
			Obj->SetNumberField(TEXT("min_y"), State.Bounds.MinY);
			Obj->SetNumberField(TEXT("max_x"), State.Bounds.MaxX);
			Obj->SetNumberField(TEXT("max_y"), State.Bounds.MaxY);
			Obj->SetNumberField(TEXT("width"), State.Bounds.MaxX - State.Bounds.MinX);
			Obj->SetNumberField(TEXT("height"), State.Bounds.MaxY - State.Bounds.MinY);
		}
		else
		{
			Obj->SetNumberField(TEXT("min_x"), State.ViewBox.X);
			Obj->SetNumberField(TEXT("min_y"), State.ViewBox.Y);
			Obj->SetNumberField(TEXT("max_x"), State.ViewBox.X + State.ViewBox.W);
			Obj->SetNumberField(TEXT("max_y"), State.ViewBox.Y + State.ViewBox.H);
			Obj->SetNumberField(TEXT("width"), State.ViewBox.W);
			Obj->SetNumberField(TEXT("height"), State.ViewBox.H);
		}
		return Obj;
	}

	static TSharedPtr<FJsonObject> BuildProvenance(
		const FString& Provider,
		const FString& Model,
		const FString& Source,
		const FString& Prompt,
		const FString& OriginalHash)
	{
		TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("kind"), TEXT("svg"));
		Provenance->SetStringField(TEXT("provider"), Provider);
		Provenance->SetStringField(TEXT("model"), Model);
		Provenance->SetStringField(TEXT("source"), Source);
		Provenance->SetStringField(TEXT("prompt_hash"), Prompt.IsEmpty() ? TEXT("") : HashStringUtf8(Prompt));
		Provenance->SetBoolField(TEXT("prompt_redacted"), true);
		Provenance->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
		if (!OriginalHash.IsEmpty())
		{
			Provenance->SetStringField(TEXT("original_svg_hash"), OriginalHash);
		}
		return Provenance;
	}

	static TSharedPtr<FJsonObject> BuildResultObject(
		const FSvgState& State,
		const FSvgOptions& Options,
		const TSharedPtr<FJsonObject>& Provenance,
		const FString& SourceSvgPath,
		const FString& SidecarPath,
		const FString& PackagePath,
		bool bSaved)
	{
		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("profile"), State.Profile);
		Result->SetStringField(TEXT("geometry_policy"), State.GeometryPolicy);
		Result->SetStringField(TEXT("fill_rule_policy"), State.FillRulePolicy);
		Result->SetStringField(TEXT("fill_rule"), State.EffectiveFillRule);
		Result->SetBoolField(TEXT("saved"), bSaved);
		if (!PackagePath.IsEmpty())
		{
			Result->SetStringField(TEXT("asset_path"), PackagePath);
		}
		if (!SourceSvgPath.IsEmpty())
		{
			Result->SetStringField(TEXT("source_svg_path"), SourceSvgPath);
		}
		if (!SidecarPath.IsEmpty())
		{
			Result->SetStringField(TEXT("sidecar_path"), SidecarPath);
		}
		Result->SetStringField(TEXT("svg_hash"), State.SvgHash);
		if (!State.OriginalHash.IsEmpty())
		{
			Result->SetStringField(TEXT("original_svg_hash"), State.OriginalHash);
		}
		Result->SetStringField(TEXT("view_box"), FormatViewBox(State.ViewBox));
		Result->SetObjectField(TEXT("bounds"), BoundsToJson(State));
		Result->SetNumberField(TEXT("element_count"), State.ElementCount);
		Result->SetNumberField(TEXT("path_count"), State.PathCount);
		Result->SetNumberField(TEXT("path_command_count"), State.PathCommandCount);
		Result->SetNumberField(TEXT("contour_count"), State.Contours.Num());
		Result->SetNumberField(TEXT("open_contour_count"), State.OpenContourCount);
		Result->SetNumberField(TEXT("self_intersection_count"), State.SelfIntersectionCount);
		Result->SetNumberField(TEXT("overlap_count"), State.OverlapCount);
		Result->SetBoolField(TEXT("svg_valid"), State.bSvgValid);
		Result->SetBoolField(TEXT("sanitized"), State.bSanitized);
		Result->SetBoolField(TEXT("geometry_valid"), State.bGeometryValid);
		Result->SetBoolField(TEXT("msdf_ready"), State.bMsdfReady);
		SetStringArray(Result, TEXT("sanitizer_removed"), State.Removed);
		SetStringArray(Result, TEXT("warnings"), State.Warnings);
		SetStringArray(Result, TEXT("msdf_blockers"), State.Blockers);
		SetStringArray(Result, TEXT("winding_issues"), State.WindingIssues);
		SetStringArray(Result, TEXT("repair_actions"), State.RepairActions);
		Result->SetObjectField(TEXT("provenance"), Provenance);
		if (Options.bReturnSvg)
		{
			Result->SetStringField(TEXT("svg_text"), State.SanitizedSvg);
		}
		return Result;
	}

	static bool SaveSidecar(const FString& SidecarPath, const TSharedPtr<FJsonObject>& Result, FString& OutError)
	{
		TSharedPtr<FJsonObject> Sidecar = MakeShared<FJsonObject>();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Result))
		{
			if (Pair.Key != TEXT("svg_text"))
			{
				Sidecar->SetField(Pair.Key, Pair.Value);
			}
		}
		return SaveUtf8TextFile(SidecarPath, JsonObjectToString(Sidecar.ToSharedRef()), OutError);
	}

	struct FMsdfSegment
	{
		FPoint2 A;
		FPoint2 B;
		int32 ChannelMask = 0;
	};

	struct FMsdfOptions
	{
		int32 Size = 128;
		int32 PixelRange = 8;
		bool bSave = true;
		bool bSaveSourcePng = true;
		bool bReturnPng = false;
		bool bVerifySamples = true;
		bool bCreateMaterial = true;
		bool bVerifyMaterialRender = true;
		FString OverwritePolicy = TEXT("unique");
		FString MaterialOverwritePolicy = TEXT("unique");
	};

	struct FMsdfBake
	{
		TArray<uint8> RawBgra;
		TArray<uint8> PngBytes;
		int32 Width = 0;
		int32 Height = 0;
		double RangeSvg = 0.0;
		double ChannelSpreadMax = 0.0;
		double MedianMin = 1.0;
		double MedianMax = 0.0;
		TArray<TSharedPtr<FJsonValue>> Samples;
	};

	static double Clamp01(double Value)
	{
		return FMath::Clamp(Value, 0.0, 1.0);
	}

	static double DistanceToSegment(const FPoint2& Point, const FPoint2& A, const FPoint2& B)
	{
		const double VX = B.X - A.X;
		const double VY = B.Y - A.Y;
		const double WX = Point.X - A.X;
		const double WY = Point.Y - A.Y;
		const double LenSq = VX * VX + VY * VY;
		if (LenSq <= GeometryEpsilon * GeometryEpsilon)
		{
			const double DX = Point.X - A.X;
			const double DY = Point.Y - A.Y;
			return FMath::Sqrt(DX * DX + DY * DY);
		}
		const double T = FMath::Clamp((WX * VX + WY * VY) / LenSq, 0.0, 1.0);
		const double PX = A.X + T * VX;
		const double PY = A.Y + T * VY;
		const double DX = Point.X - PX;
		const double DY = Point.Y - PY;
		return FMath::Sqrt(DX * DX + DY * DY);
	}

	static int32 WindingNumberForContour(const FPoint2& Point, const FContour& Contour)
	{
		int32 Winding = 0;
		if (!Contour.bClosed || Contour.Points.Num() < 3)
		{
			return Winding;
		}

		const int32 Count = Contour.Points.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FPoint2& A = Contour.Points[Index];
			const FPoint2& B = Contour.Points[(Index + 1) % Count];
			if (A.Y <= Point.Y)
			{
				if (B.Y > Point.Y && Cross(A, B, Point) > GeometryEpsilon)
				{
					++Winding;
				}
			}
			else if (B.Y <= Point.Y && Cross(A, B, Point) < -GeometryEpsilon)
			{
				--Winding;
			}
		}
		return Winding;
	}

	static bool IsPointInsideMsdfShape(const FPoint2& Point, const FSvgState& State)
	{
		if (State.EffectiveFillRule == TEXT("evenodd"))
		{
			bool bInside = false;
			for (const FContour& Contour : State.Contours)
			{
				if (PointInPolygon(Point, Contour))
				{
					bInside = !bInside;
				}
			}
			return bInside;
		}

		int32 Winding = 0;
		for (const FContour& Contour : State.Contours)
		{
			Winding += WindingNumberForContour(Point, Contour);
		}
		return Winding != 0;
	}

	static void BuildMsdfSegments(const FSvgState& State, TArray<FMsdfSegment>& OutSegments)
	{
		OutSegments.Reset();
		int32 SegmentIndex = 0;
		for (const FContour& Contour : State.Contours)
		{
			if (!Contour.bClosed || Contour.Points.Num() < 2)
			{
				continue;
			}
			const int32 Count = Contour.Points.Num();
			for (int32 Index = 0; Index < Count; ++Index)
			{
				FMsdfSegment Segment;
				Segment.A = Contour.Points[Index];
				Segment.B = Contour.Points[(Index + 1) % Count];
				switch (SegmentIndex % 3)
				{
				case 0: Segment.ChannelMask = 0x1 | 0x2; break; // R + G
				case 1: Segment.ChannelMask = 0x2 | 0x4; break; // G + B
				default: Segment.ChannelMask = 0x4 | 0x1; break; // B + R
				}
				OutSegments.Add(Segment);
				++SegmentIndex;
			}
		}
	}

	static double Median3(double A, double B, double C)
	{
		return FMath::Max(FMath::Min(A, B), FMath::Min(FMath::Max(A, B), C));
	}

	static double MaxChannelDelta(double A, double B, double C)
	{
		return FMath::Max(FMath::Abs(A - B), FMath::Max(FMath::Abs(B - C), FMath::Abs(C - A)));
	}

	static uint8 EncodeDistanceByte(double SignedDistance, double RangeSvg)
	{
		const double Encoded = Clamp01(0.5 + SignedDistance / FMath::Max(RangeSvg * 2.0, GeometryEpsilon));
		return static_cast<uint8>(FMath::RoundToInt(Encoded * 255.0));
	}

	static FPoint2 PixelToSvgPoint(int32 X, int32 Y, int32 Width, int32 Height, const FViewBox& ViewBox)
	{
		return MakePoint(
			ViewBox.X + (static_cast<double>(X) + 0.5) / static_cast<double>(Width) * ViewBox.W,
			ViewBox.Y + (static_cast<double>(Y) + 0.5) / static_cast<double>(Height) * ViewBox.H);
	}

	static void AddMsdfSample(
		const TCHAR* Name,
		int32 X,
		int32 Y,
		const FMsdfBake& Bake,
		TArray<TSharedPtr<FJsonValue>>& OutSamples)
	{
		if (Bake.RawBgra.Num() == 0 || Bake.Width <= 0 || Bake.Height <= 0)
		{
			return;
		}
		X = FMath::Clamp(X, 0, Bake.Width - 1);
		Y = FMath::Clamp(Y, 0, Bake.Height - 1);
		const int32 Offset = (Y * Bake.Width + X) * 4;
		if (!Bake.RawBgra.IsValidIndex(Offset + 3))
		{
			return;
		}

		const double B = static_cast<double>(Bake.RawBgra[Offset + 0]) / 255.0;
		const double G = static_cast<double>(Bake.RawBgra[Offset + 1]) / 255.0;
		const double R = static_cast<double>(Bake.RawBgra[Offset + 2]) / 255.0;
		const double A = static_cast<double>(Bake.RawBgra[Offset + 3]) / 255.0;
		TSharedPtr<FJsonObject> Sample = MakeShared<FJsonObject>();
		Sample->SetStringField(TEXT("name"), Name);
		Sample->SetNumberField(TEXT("x"), X);
		Sample->SetNumberField(TEXT("y"), Y);
		Sample->SetNumberField(TEXT("r"), R);
		Sample->SetNumberField(TEXT("g"), G);
		Sample->SetNumberField(TEXT("b"), B);
		Sample->SetNumberField(TEXT("a"), A);
		Sample->SetNumberField(TEXT("median"), Median3(R, G, B));
		Sample->SetNumberField(TEXT("channel_spread"), MaxChannelDelta(R, G, B));
		OutSamples.Add(MakeShared<FJsonValueObject>(Sample));
	}

	static bool EncodeRawBgraAsPng(const TArray<uint8>& RawBgra, int32 Width, int32 Height, TArray<uint8>& OutPngBytes)
	{
		OutPngBytes.Reset();
		if (RawBgra.Num() != Width * Height * 4)
		{
			return false;
		}
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> PngWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!PngWrapper.IsValid() || !PngWrapper->SetRaw(RawBgra.GetData(), RawBgra.Num(), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8> PngBytes64 = PngWrapper->GetCompressed(100);
		OutPngBytes.Append(PngBytes64.GetData(), PngBytes64.Num());
		return OutPngBytes.Num() > 0;
	}

	static FString HashBytes(const TArray<uint8>& Bytes)
	{
		FMD5 Md5;
		if (Bytes.Num() > 0)
		{
			Md5.Update(Bytes.GetData(), Bytes.Num());
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}

	static bool BakeMsdf(const FSvgState& State, const FMsdfOptions& Options, FMsdfBake& OutBake, FString& OutError)
	{
		if (!State.bMsdfReady)
		{
			OutError = FString::Printf(TEXT("SVG is not msdf_ready: %s"), *FString::Join(State.Blockers, TEXT(", ")));
			return false;
		}

		TArray<FMsdfSegment> Segments;
		BuildMsdfSegments(State, Segments);
		if (Segments.Num() == 0)
		{
			OutError = TEXT("No contour segments available for MSDF generation");
			return false;
		}

		OutBake.Width = Options.Size;
		OutBake.Height = Options.Size;
		OutBake.RangeSvg = static_cast<double>(Options.PixelRange)
			* FMath::Max(State.ViewBox.W / static_cast<double>(Options.Size), State.ViewBox.H / static_cast<double>(Options.Size));
		OutBake.RawBgra.SetNumZeroed(Options.Size * Options.Size * 4);
		OutBake.ChannelSpreadMax = 0.0;
		OutBake.MedianMin = 1.0;
		OutBake.MedianMax = 0.0;

		for (int32 Y = 0; Y < Options.Size; ++Y)
		{
			for (int32 X = 0; X < Options.Size; ++X)
			{
				const FPoint2 Point = PixelToSvgPoint(X, Y, Options.Size, Options.Size, State.ViewBox);
				const bool bInside = IsPointInsideMsdfShape(Point, State);
				double GlobalDistance = TNumericLimits<double>::Max();
				double ChannelDistances[3] = {
					TNumericLimits<double>::Max(),
					TNumericLimits<double>::Max(),
					TNumericLimits<double>::Max()
				};

				for (const FMsdfSegment& Segment : Segments)
				{
					const double Distance = DistanceToSegment(Point, Segment.A, Segment.B);
					GlobalDistance = FMath::Min(GlobalDistance, Distance);
					if ((Segment.ChannelMask & 0x1) != 0) { ChannelDistances[0] = FMath::Min(ChannelDistances[0], Distance); }
					if ((Segment.ChannelMask & 0x2) != 0) { ChannelDistances[1] = FMath::Min(ChannelDistances[1], Distance); }
					if ((Segment.ChannelMask & 0x4) != 0) { ChannelDistances[2] = FMath::Min(ChannelDistances[2], Distance); }
				}

				const double Sign = bInside ? 1.0 : -1.0;
				for (double& ChannelDistance : ChannelDistances)
				{
					if (!FMath::IsFinite(ChannelDistance) || ChannelDistance == TNumericLimits<double>::Max())
					{
						ChannelDistance = GlobalDistance;
					}
				}

				const uint8 R = EncodeDistanceByte(Sign * ChannelDistances[0], OutBake.RangeSvg);
				const uint8 G = EncodeDistanceByte(Sign * ChannelDistances[1], OutBake.RangeSvg);
				const uint8 B = EncodeDistanceByte(Sign * ChannelDistances[2], OutBake.RangeSvg);
				const int32 Offset = (Y * Options.Size + X) * 4;
				OutBake.RawBgra[Offset + 0] = B;
				OutBake.RawBgra[Offset + 1] = G;
				OutBake.RawBgra[Offset + 2] = R;
				OutBake.RawBgra[Offset + 3] = 255;

				const double Rn = static_cast<double>(R) / 255.0;
				const double Gn = static_cast<double>(G) / 255.0;
				const double Bn = static_cast<double>(B) / 255.0;
				const double Median = Median3(Rn, Gn, Bn);
				OutBake.MedianMin = FMath::Min(OutBake.MedianMin, Median);
				OutBake.MedianMax = FMath::Max(OutBake.MedianMax, Median);
				OutBake.ChannelSpreadMax = FMath::Max(
					OutBake.ChannelSpreadMax,
					MaxChannelDelta(Rn, Gn, Bn));
			}
		}

		const int32 BoundsMinX = State.Bounds.bValid ? FMath::RoundToInt((State.Bounds.MinX - State.ViewBox.X) / State.ViewBox.W * Options.Size) : Options.Size / 4;
		const int32 BoundsMidY = State.Bounds.bValid ? FMath::RoundToInt((((State.Bounds.MinY + State.Bounds.MaxY) * 0.5) - State.ViewBox.Y) / State.ViewBox.H * Options.Size) : Options.Size / 2;
		AddMsdfSample(TEXT("center"), Options.Size / 2, Options.Size / 2, OutBake, OutBake.Samples);
		AddMsdfSample(TEXT("outside_corner"), 1, 1, OutBake, OutBake.Samples);
		AddMsdfSample(TEXT("edge_mid"), BoundsMinX, BoundsMidY, OutBake, OutBake.Samples);

		if (!EncodeRawBgraAsPng(OutBake.RawBgra, OutBake.Width, OutBake.Height, OutBake.PngBytes))
		{
			OutError = TEXT("Failed to encode MSDF raw BGRA pixels as PNG");
			return false;
		}
		return true;
	}

	static bool TryGetNamedSampleMedian(
		const FMsdfBake& Bake,
		const FString& SampleName,
		double& OutMedian)
	{
		for (const TSharedPtr<FJsonValue>& Value : Bake.Samples)
		{
			const TSharedPtr<FJsonObject> Sample = Value.IsValid() ? Value->AsObject() : nullptr;
			FString Name;
			if (Sample.IsValid() && Sample->TryGetStringField(TEXT("name"), Name) && Name == SampleName)
			{
				return Sample->TryGetNumberField(TEXT("median"), OutMedian);
			}
		}
		return false;
	}

	static bool ValidateMsdfBakeSamples(const FMsdfBake& Bake, FString& OutError)
	{
		double CenterMedian = 0.0;
		double OutsideMedian = 0.0;
		double EdgeMedian = 0.0;
		if (!TryGetNamedSampleMedian(Bake, TEXT("center"), CenterMedian)
			|| !TryGetNamedSampleMedian(Bake, TEXT("outside_corner"), OutsideMedian)
			|| !TryGetNamedSampleMedian(Bake, TEXT("edge_mid"), EdgeMedian))
		{
			OutError = TEXT("MSDF sample validation failed: required center/outside_corner/edge_mid samples were not produced");
			return false;
		}
		if (CenterMedian <= 0.55)
		{
			OutError = FString::Printf(TEXT("MSDF sample validation failed: center median %.3f is not inside-positive"), CenterMedian);
			return false;
		}
		if (OutsideMedian >= 0.45)
		{
			OutError = FString::Printf(TEXT("MSDF sample validation failed: outside_corner median %.3f is not outside-negative"), OutsideMedian);
			return false;
		}
		if (FMath::Abs(EdgeMedian - 0.5) > 0.18)
		{
			OutError = FString::Printf(TEXT("MSDF sample validation failed: edge_mid median %.3f is too far from the signed-distance edge"), EdgeMedian);
			return false;
		}
		if (Bake.MedianMax <= 0.75 || Bake.MedianMin >= 0.25)
		{
			OutError = FString::Printf(TEXT("MSDF sample validation failed: encoded median range %.3f..%.3f is too narrow"), Bake.MedianMin, Bake.MedianMax);
			return false;
		}
		if (Bake.ChannelSpreadMax <= (1.0 / 255.0))
		{
			OutError = TEXT("MSDF sample validation failed: channels are identical; not a multi-channel distance field");
			return false;
		}
		return true;
	}

	static FString SanitizeTextureName(FString Input)
	{
		Input = SanitizeVectorName(Input);
		if (Input.StartsWith(TEXT("V_")))
		{
			Input = TEXT("T_") + Input.RightChop(2);
		}
		if (!Input.StartsWith(TEXT("T_")))
		{
			Input = TEXT("T_") + Input;
		}
		return Input;
	}

	static FString SanitizeMaterialName(FString Input)
	{
		Input = SanitizeTextureName(Input);
		if (Input.StartsWith(TEXT("T_")))
		{
			Input = TEXT("M_") + Input.RightChop(2);
		}
		if (!Input.StartsWith(TEXT("M_")))
		{
			Input = TEXT("M_") + Input;
		}
		return Input;
	}

	static FString ResolveMsdfPackagePath(
		const TSharedPtr<FJsonObject>& Params,
		const FString& DestinationField,
		const FString& AssetPathField,
		const FString& AssetNameField,
		const FString& DefaultFolder,
		const FString& FallbackAssetName,
		bool bMaterialName,
		FString& OutError)
	{
		FString Destination;
		Params->TryGetStringField(DestinationField, Destination);
		if (!Destination.IsEmpty())
		{
			if (Destination.EndsWith(TEXT(".uasset")))
			{
				Destination.LeftChopInline(7);
			}
			OutError = MonolithCore::ValidatePackagePath(Destination);
			return Destination;
		}

		FString AssetPath;
		if (!Params->TryGetStringField(AssetPathField, AssetPath) || AssetPath.IsEmpty())
		{
			AssetPath = DefaultFolder;
		}
		if (AssetPath.EndsWith(TEXT("/")))
		{
			AssetPath.LeftChopInline(1);
		}
		if (const FString PathError = MonolithCore::ValidatePackagePath(AssetPath / TEXT("__Probe")); !PathError.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Invalid %s '%s': %s"), *AssetPathField, *AssetPath, *PathError);
			return FString();
		}

		FString AssetName;
		if (!Params->TryGetStringField(AssetNameField, AssetName) || AssetName.IsEmpty())
		{
			AssetName = FallbackAssetName;
		}
		AssetName = bMaterialName ? SanitizeMaterialName(AssetName) : SanitizeTextureName(AssetName);
		const FString PackagePath = AssetPath / AssetName;
		OutError = MonolithCore::ValidatePackagePath(PackagePath);
		return PackagePath;
	}

	static void MakeUniquePackagePath(FString& InOutPackagePath)
	{
		if (!FPackageName::DoesPackageExist(InOutPackagePath))
		{
			return;
		}
		const FString PackageDir = FPackageName::GetLongPackagePath(InOutPackagePath);
		const FString BaseName = FPackageName::GetLongPackageAssetName(InOutPackagePath);
		for (int32 Index = 1; Index < 1000; ++Index)
		{
			const FString Candidate = PackageDir / FString::Printf(TEXT("%s_%03d"), *BaseName, Index);
			if (!FPackageName::DoesPackageExist(Candidate))
			{
				InOutPackagePath = Candidate;
				return;
			}
		}
	}

	static FString ObjectPathFromPackagePath(const FString& PackagePath)
	{
		if (PackagePath.Contains(TEXT(".")))
		{
			return PackagePath;
		}
		return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
	}

	static FString ResolveMsdfPngPath(const FString& TexturePackagePath)
	{
		FString RelativePath = TexturePackagePath;
		const FString GeneratedRoot = FString(TEXT("/Game/GeneratedImages/"));
		if (RelativePath.StartsWith(GeneratedRoot))
		{
			RelativePath.RightChopInline(GeneratedRoot.Len());
		}
		else if (RelativePath.StartsWith(TEXT("/Game/")))
		{
			RelativePath.RightChopInline(6);
		}
		RelativePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (RelativePath.StartsWith(TEXT("/")))
		{
			RelativePath.RightChopInline(1);
		}
		RelativePath.ReplaceInline(TEXT(".."), TEXT("_"));
		if (RelativePath.IsEmpty())
		{
			RelativePath = TEXT("MSDF/T_GeneratedMsdf");
		}
		return FPaths::ConvertRelativePathToFull(
			FPaths::Combine(FPaths::ProjectDir(), VectorDirectoryName, RelativePath + TEXT(".png")));
	}

	static bool SaveBytesFile(const FString& Path, const TArray<uint8>& Bytes, FString& OutError)
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (!FFileHelper::SaveArrayToFile(Bytes, *Path))
		{
			OutError = FString::Printf(TEXT("Failed to write '%s'"), *Path);
			return false;
		}
		return true;
	}

	static bool ApplyMsdfProvenance(
		const FString& TexturePackagePath,
		const TSharedPtr<FJsonObject>& Provenance,
		bool bSave,
		FString& OutError)
	{
		UTexture2D* Texture = FMonolithAssetUtils::LoadAssetByPath<UTexture2D>(ObjectPathFromPackagePath(TexturePackagePath));
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Imported MSDF texture '%s' could not be loaded for provenance"), *TexturePackagePath);
			return false;
		}
#if WITH_METADATA
		UPackage* Package = Texture->GetOutermost();
		FMetaData& MetaData = Package->GetMetaData();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Provenance))
		{
			FString Value;
			if (Pair.Value.IsValid())
			{
				if (Pair.Value->Type == EJson::String)
				{
					Value = Pair.Value->AsString();
				}
				else
				{
					TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
					Wrapper->SetField(TEXT("value"), Pair.Value);
					FString Json = JsonObjectToString(Wrapper.ToSharedRef());
					Json.RemoveFromStart(TEXT("{\"value\":"));
					Json.RemoveFromEnd(TEXT("}"));
					Value = Json;
				}
			}
			MetaData.SetValue(Texture, *FString::Printf(TEXT("Monolith.Generated.%s"), *Pair.Key), *Value);
		}
		Package->MarkPackageDirty();
		if (bSave)
		{
			const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			SaveArgs.SaveFlags = SAVE_NoError;
			if (!UPackage::SavePackage(Package, Texture, *PackageFilename, SaveArgs))
			{
				OutError = FString::Printf(TEXT("Failed to save MSDF texture provenance for '%s'"), *TexturePackagePath);
				return false;
			}
		}
#endif
		return true;
	}

	static bool ApplyMsdfTextureSettings(
		const FString& TexturePackagePath,
		int32 ExpectedSize,
		FString& OutError)
	{
		UTexture2D* Texture = FMonolithAssetUtils::LoadAssetByPath<UTexture2D>(ObjectPathFromPackagePath(TexturePackagePath));
		if (!Texture)
		{
			OutError = FString::Printf(TEXT("Imported MSDF texture '%s' could not be loaded for settings"), *TexturePackagePath);
			return false;
		}

		Texture->Modify();
		Texture->CompressionSettings = TC_Masks;
		Texture->SRGB = false;
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->LODGroup = TEXTUREGROUP_UI;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->NeverStream = true;
		Texture->MaxTextureSize = ExpectedSize;
		Texture->UpdateResource();
#if WITH_EDITOR
		Texture->PostEditChange();
#endif
		Texture->MarkPackageDirty();
		return true;
	}

	static TSharedPtr<FJsonObject> DecodePngFileStats(const FString& FilePath)
	{
		TSharedPtr<FJsonObject> Stats = MakeShared<FJsonObject>();
		Stats->SetStringField(TEXT("file_path"), FilePath);
		Stats->SetBoolField(TEXT("exists"), FPaths::FileExists(FilePath));
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FilePath) || Bytes.Num() == 0)
		{
			Stats->SetBoolField(TEXT("decoded"), false);
			return Stats;
		}
		Stats->SetNumberField(TEXT("bytes"), Bytes.Num());
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		TArray<uint8> RawBgra;
		if (!Wrapper.IsValid() || !Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Wrapper->GetRaw(ERGBFormat::BGRA, 8, RawBgra))
		{
			Stats->SetBoolField(TEXT("decoded"), false);
			return Stats;
		}
		const int32 Width = Wrapper->GetWidth();
		const int32 Height = Wrapper->GetHeight();
		int32 NonBlack = 0;
		uint8 MinLuma = 255;
		uint8 MaxLuma = 0;
		for (int32 Offset = 0; Offset + 3 < RawBgra.Num(); Offset += 4)
		{
			const uint8 Luma = static_cast<uint8>((static_cast<int32>(RawBgra[Offset + 0]) + RawBgra[Offset + 1] + RawBgra[Offset + 2]) / 3);
			MinLuma = FMath::Min(MinLuma, Luma);
			MaxLuma = FMath::Max(MaxLuma, Luma);
			if (Luma > 4 || RawBgra[Offset + 3] > 4)
			{
				++NonBlack;
			}
		}
		Stats->SetBoolField(TEXT("decoded"), true);
		Stats->SetNumberField(TEXT("width"), Width);
		Stats->SetNumberField(TEXT("height"), Height);
		Stats->SetNumberField(TEXT("non_black_pixels"), NonBlack);
		Stats->SetNumberField(TEXT("min_luma"), MinLuma);
		Stats->SetNumberField(TEXT("max_luma"), MaxLuma);
		Stats->SetBoolField(TEXT("non_empty"), NonBlack > 0);
		Stats->SetBoolField(TEXT("non_uniform"), MaxLuma > MinLuma);
		return Stats;
	}

	static TSharedPtr<FJsonObject> CreateMsdfMaterialAndPreview(
		const FString& TexturePackagePath,
		const FString& MaterialPath,
		bool bVerifyRender,
		FString& OutError)
	{
		TSharedPtr<FJsonObject> MaterialResult = MakeShared<FJsonObject>();
		MaterialResult->SetStringField(TEXT("material_path"), MaterialPath);
		MaterialResult->SetBoolField(TEXT("created"), false);
		MaterialResult->SetBoolField(TEXT("rendered"), false);

		FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("MonolithMaterial"));
		FMonolithToolRegistry& Registry = FMonolithToolRegistry::Get();
		if (!Registry.HasAction(TEXT("material"), TEXT("create_material"))
			|| !Registry.HasAction(TEXT("material"), TEXT("build_material_graph")))
		{
			OutError = TEXT("MonolithMaterial actions are not registered; cannot create MSDF material");
			return MaterialResult;
		}

		TSharedPtr<FJsonObject> CreateParams = MakeShared<FJsonObject>();
		CreateParams->SetStringField(TEXT("asset_path"), MaterialPath);
		CreateParams->SetStringField(TEXT("blend_mode"), TEXT("Masked"));
		CreateParams->SetStringField(TEXT("shading_model"), TEXT("Unlit"));
		CreateParams->SetStringField(TEXT("material_domain"), TEXT("Surface"));
		CreateParams->SetBoolField(TEXT("two_sided"), true);
		FMonolithActionResult Create = Registry.ExecuteAction(TEXT("material"), TEXT("create_material"), CreateParams);
		if (!Create.bSuccess)
		{
			OutError = Create.ErrorMessage;
			return MaterialResult;
		}
		MaterialResult->SetBoolField(TEXT("created"), true);

		TSharedPtr<FJsonObject> GraphSpec = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Nodes;
		TSharedPtr<FJsonObject> TextureNode = MakeShared<FJsonObject>();
		TextureNode->SetStringField(TEXT("id"), TEXT("MsdfTexture"));
		TextureNode->SetStringField(TEXT("class"), TEXT("TextureSample"));
		TSharedPtr<FJsonObject> TextureProps = MakeShared<FJsonObject>();
		TextureProps->SetStringField(TEXT("Texture"), ObjectPathFromPackagePath(TexturePackagePath));
		TextureProps->SetStringField(TEXT("SamplerType"), TEXT("SAMPLERTYPE_Masks"));
		TextureNode->SetObjectField(TEXT("props"), TextureProps);
		TArray<TSharedPtr<FJsonValue>> TexturePos;
		TexturePos.Add(MakeShared<FJsonValueNumber>(-500.0));
		TexturePos.Add(MakeShared<FJsonValueNumber>(0.0));
		TextureNode->SetArrayField(TEXT("pos"), TexturePos);
		Nodes.Add(MakeShared<FJsonValueObject>(TextureNode));
		GraphSpec->SetArrayField(TEXT("nodes"), Nodes);

		TArray<TSharedPtr<FJsonValue>> CustomNodes;
		TSharedPtr<FJsonObject> MedianNode = MakeShared<FJsonObject>();
		MedianNode->SetStringField(TEXT("id"), TEXT("MsdfMedian"));
		MedianNode->SetStringField(TEXT("output_type"), TEXT("Float1"));
		MedianNode->SetStringField(TEXT("code"), TEXT("float sd = max(min(RGB.r, RGB.g), min(max(RGB.r, RGB.g), RGB.b));\nreturn saturate((sd - 0.5f) * 32.0f + 0.5f);"));
		TArray<TSharedPtr<FJsonValue>> MedianInputs;
		TSharedPtr<FJsonObject> MedianInput = MakeShared<FJsonObject>();
		MedianInput->SetStringField(TEXT("name"), TEXT("RGB"));
		MedianInputs.Add(MakeShared<FJsonValueObject>(MedianInput));
		MedianNode->SetArrayField(TEXT("inputs"), MedianInputs);
		TArray<TSharedPtr<FJsonValue>> MedianPos;
		MedianPos.Add(MakeShared<FJsonValueNumber>(-150.0));
		MedianPos.Add(MakeShared<FJsonValueNumber>(0.0));
		MedianNode->SetArrayField(TEXT("pos"), MedianPos);
		CustomNodes.Add(MakeShared<FJsonValueObject>(MedianNode));
		GraphSpec->SetArrayField(TEXT("custom_hlsl_nodes"), CustomNodes);

		auto MakeConnection = [](const FString& From, const FString& FromPin, const FString& To, const FString& ToPin)
		{
			TSharedPtr<FJsonObject> Conn = MakeShared<FJsonObject>();
			Conn->SetStringField(TEXT("from"), From);
			Conn->SetStringField(TEXT("from_pin"), FromPin);
			Conn->SetStringField(TEXT("to"), To);
			Conn->SetStringField(TEXT("to_pin"), ToPin);
			return MakeShared<FJsonValueObject>(Conn);
		};
		TArray<TSharedPtr<FJsonValue>> Connections;
		Connections.Add(MakeConnection(TEXT("MsdfTexture"), TEXT("RGB"), TEXT("MsdfMedian"), TEXT("RGB")));
		GraphSpec->SetArrayField(TEXT("connections"), Connections);

		auto MakeOutput = [](const FString& From, const FString& ToProperty)
		{
			TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("from"), From);
			Out->SetStringField(TEXT("to_property"), ToProperty);
			return MakeShared<FJsonValueObject>(Out);
		};
		TArray<TSharedPtr<FJsonValue>> Outputs;
		Outputs.Add(MakeOutput(TEXT("MsdfMedian"), TEXT("EmissiveColor")));
		Outputs.Add(MakeOutput(TEXT("MsdfMedian"), TEXT("OpacityMask")));
		GraphSpec->SetArrayField(TEXT("outputs"), Outputs);

		TSharedPtr<FJsonObject> BuildParams = MakeShared<FJsonObject>();
		BuildParams->SetStringField(TEXT("asset_path"), MaterialPath);
		BuildParams->SetObjectField(TEXT("graph_spec"), GraphSpec);
		BuildParams->SetBoolField(TEXT("clear_existing"), true);
		FMonolithActionResult Build = Registry.ExecuteAction(TEXT("material"), TEXT("build_material_graph"), BuildParams);
		if (!Build.bSuccess)
		{
			OutError = Build.ErrorMessage;
			return MaterialResult;
		}
		bool bBuildHasErrors = false;
		if (Build.Result.IsValid() && Build.Result->TryGetBoolField(TEXT("has_errors"), bBuildHasErrors) && bBuildHasErrors)
		{
			MaterialResult->SetObjectField(TEXT("build_result"), Build.Result);
			OutError = TEXT("material.build_material_graph reported graph errors");
			return MaterialResult;
		}
		MaterialResult->SetBoolField(TEXT("graph_built"), true);
		MaterialResult->SetObjectField(TEXT("build_result"), Build.Result);

		if (bVerifyRender)
		{
			if (!Registry.HasAction(TEXT("material"), TEXT("render_preview")))
			{
				OutError = TEXT("material.render_preview is not registered; cannot verify MSDF material render");
				return MaterialResult;
			}
			TSharedPtr<FJsonObject> PreviewParams = MakeShared<FJsonObject>();
			PreviewParams->SetStringField(TEXT("asset_path"), MaterialPath);
			PreviewParams->SetNumberField(TEXT("resolution"), 96);
			FMonolithActionResult Preview = Registry.ExecuteAction(TEXT("material"), TEXT("render_preview"), PreviewParams);
			if (!Preview.bSuccess)
			{
				OutError = Preview.ErrorMessage;
				return MaterialResult;
			}
			MaterialResult->SetBoolField(TEXT("rendered"), true);
			MaterialResult->SetObjectField(TEXT("render_preview"), Preview.Result);
			FString PreviewPath;
			if (Preview.Result.IsValid() && Preview.Result->TryGetStringField(TEXT("file_path"), PreviewPath) && !PreviewPath.IsEmpty())
			{
				MaterialResult->SetObjectField(TEXT("render_preview_stats"), DecodePngFileStats(PreviewPath));
			}
		}
		return MaterialResult;
	}

	static bool AppendGeneratedPathElement(const TSharedPtr<FJsonValue>& Value, FString& OutChildren, FString& OutError)
	{
		FString PathData;
		FString Fill = TEXT("#ffffff");
		FString FillRule;
		if (Value->Type == EJson::String)
		{
			PathData = Value->AsString();
		}
		else if (Value->Type == EJson::Object)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			Obj->TryGetStringField(TEXT("d"), PathData);
			Obj->TryGetStringField(TEXT("fill"), Fill);
			Obj->TryGetStringField(TEXT("fill_rule"), FillRule);
			if (FillRule.IsEmpty())
			{
				Obj->TryGetStringField(TEXT("fill-rule"), FillRule);
			}
		}
		if (PathData.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("svg_spec.paths entries require path data");
			return false;
		}
		OutChildren += FString::Printf(TEXT("<path d=\"%s\" fill=\"%s\""), *XmlEscape(PathData), *XmlEscape(Fill));
		if (!FillRule.IsEmpty())
		{
			OutChildren += FString::Printf(TEXT(" fill-rule=\"%s\""), *XmlEscape(FillRule));
		}
		OutChildren += TEXT("/>");
		return true;
	}

	static bool AppendGeneratedRectElement(const TSharedPtr<FJsonValue>& Value, FString& OutChildren, FString& OutError)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutError = TEXT("svg_spec.rects entries must be objects");
			return false;
		}
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		double X = 0.0;
		double Y = 0.0;
		double W = 0.0;
		double H = 0.0;
		if (!Obj->TryGetNumberField(TEXT("x"), X))
		{
			X = 0.0;
		}
		if (!Obj->TryGetNumberField(TEXT("y"), Y))
		{
			Y = 0.0;
		}
		if (!Obj->TryGetNumberField(TEXT("width"), W) || !Obj->TryGetNumberField(TEXT("height"), H) || W <= 0.0 || H <= 0.0)
		{
			OutError = TEXT("svg_spec.rects entries require positive width and height");
			return false;
		}
		FString Fill;
		if (!Obj->TryGetStringField(TEXT("fill"), Fill) || Fill.IsEmpty())
		{
			Fill = TEXT("#ffffff");
		}
		OutChildren += FString::Printf(
			TEXT("<rect x=\"%s\" y=\"%s\" width=\"%s\" height=\"%s\" fill=\"%s\"/>"),
			*FormatNumber(X),
			*FormatNumber(Y),
			*FormatNumber(W),
			*FormatNumber(H),
			*XmlEscape(Fill));
		return true;
	}

	static bool AppendGeneratedPolygonElement(const TSharedPtr<FJsonValue>& Value, FString& OutChildren, FString& OutError)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object)
		{
			OutError = TEXT("svg_spec.polygons entries must be objects");
			return false;
		}
		const TSharedPtr<FJsonObject> Obj = Value->AsObject();
		FString PointsText;
		if (!Obj->TryGetStringField(TEXT("points"), PointsText) || PointsText.TrimStartAndEnd().IsEmpty())
		{
			OutError = TEXT("svg_spec.polygons entries require points");
			return false;
		}
		FString Fill;
		if (!Obj->TryGetStringField(TEXT("fill"), Fill) || Fill.IsEmpty())
		{
			Fill = TEXT("#ffffff");
		}
		OutChildren += FString::Printf(TEXT("<polygon points=\"%s\" fill=\"%s\"/>"), *XmlEscape(PointsText), *XmlEscape(Fill));
		return true;
	}

	static bool BuildSvgFromSpec(const TSharedPtr<FJsonObject>& Params, FString& OutSvg, FString& OutFallbackName, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* SpecPtr = nullptr;
		if (!Params->TryGetObjectField(TEXT("svg_spec"), SpecPtr) || !SpecPtr || !SpecPtr->IsValid())
		{
			FString Prompt;
			if (!Params->TryGetStringField(TEXT("prompt"), Prompt) || Prompt.TrimStartAndEnd().IsEmpty())
			{
				OutError = TEXT("generate_svg requires svg_spec or prompt placeholder input");
				return false;
			}
			const uint32 Hash = GetTypeHash(Prompt);
			const uint8 R = static_cast<uint8>(64 + (Hash & 0x7f));
			const uint8 G = static_cast<uint8>(64 + ((Hash >> 8) & 0x7f));
			const uint8 B = static_cast<uint8>(64 + ((Hash >> 16) & 0x7f));
			const FString Fill = FString::Printf(TEXT("#%02x%02x%02x"), R, G, B);
			OutSvg = FString::Printf(
				TEXT("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 128 128\" width=\"128\" height=\"128\"><path d=\"M 64 12 L 116 64 L 64 116 L 12 64 Z\" fill=\"%s\"/></svg>"),
				*Fill);
			OutFallbackName = PromptToVectorName(Prompt);
			return true;
		}

		const TSharedPtr<FJsonObject> Spec = *SpecPtr;
		FViewBox ViewBox;
		FString ViewBoxError;
		if (const TSharedPtr<FJsonValue> ViewBoxValue = Spec->TryGetField(TEXT("viewBox")); ViewBoxValue.IsValid())
		{
			if (!ParseViewBoxJson(ViewBoxValue, ViewBox, ViewBoxError))
			{
				OutError = ViewBoxError;
				return false;
			}
		}
		else if (const TSharedPtr<FJsonValue> ViewBoxValue2 = Spec->TryGetField(TEXT("view_box")); ViewBoxValue2.IsValid())
		{
			if (!ParseViewBoxJson(ViewBoxValue2, ViewBox, ViewBoxError))
			{
				OutError = ViewBoxError;
				return false;
			}
		}
		else
		{
			double Width = 128.0;
			double Height = 128.0;
			Spec->TryGetNumberField(TEXT("width"), Width);
			Spec->TryGetNumberField(TEXT("height"), Height);
			if (Width <= 0.0 || Height <= 0.0)
			{
				OutError = TEXT("svg_spec width and height must be positive");
				return false;
			}
			ViewBox.X = 0.0;
			ViewBox.Y = 0.0;
			ViewBox.W = Width;
			ViewBox.H = Height;
			ViewBox.bValid = true;
		}

		FString Children;
		const TArray<TSharedPtr<FJsonValue>>* Paths = nullptr;
		if (Spec->TryGetArrayField(TEXT("paths"), Paths) && Paths)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Paths)
			{
				if (!AppendGeneratedPathElement(Value, Children, OutError))
				{
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Rects = nullptr;
		if (Spec->TryGetArrayField(TEXT("rects"), Rects) && Rects)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Rects)
			{
				if (!AppendGeneratedRectElement(Value, Children, OutError))
				{
					return false;
				}
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* Polygons = nullptr;
		if (Spec->TryGetArrayField(TEXT("polygons"), Polygons) && Polygons)
		{
			for (const TSharedPtr<FJsonValue>& Value : *Polygons)
			{
				if (!AppendGeneratedPolygonElement(Value, Children, OutError))
				{
					return false;
				}
			}
		}

		if (Children.IsEmpty())
		{
			OutError = TEXT("svg_spec must contain at least one path, rect, or polygon");
			return false;
		}

		OutSvg = FString::Printf(
			TEXT("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"%s\" width=\"%s\" height=\"%s\">%s</svg>"),
			*FormatViewBox(ViewBox),
			*FormatNumber(ViewBox.W),
			*FormatNumber(ViewBox.H),
			*Children);
		FString Name;
		if (!Params->TryGetStringField(TEXT("asset_name"), Name) || Name.IsEmpty())
		{
			Name = TEXT("GeneratedVector");
		}
		OutFallbackName = SanitizeVectorName(Name);
		return true;
	}

	static bool LoadSvgInputFromParams(
		const TSharedPtr<FJsonObject>& Params,
		FString& OutSvg,
		FString& OutSourceKind,
		FString& OutFallbackName,
		FString& OutError)
	{
		if (Params->TryGetStringField(TEXT("svg_text"), OutSvg) && !OutSvg.IsEmpty())
		{
			OutSourceKind = TEXT("external_text");
			OutFallbackName = TEXT("ImportedVector");
			return true;
		}

		FString FormatHint;
		Params->TryGetStringField(TEXT("format_hint"), FormatHint);
		FormatHint = FormatHint.TrimStartAndEnd().ToLower();
		if (!FormatHint.IsEmpty() && FormatHint != TEXT("svg") && FormatHint != TEXT("svg+xml") && FormatHint != TEXT("image/svg+xml"))
		{
			OutError = TEXT("format_hint must be 'svg' or 'svg+xml' for SVG import");
			return false;
		}

		FString BytesB64;
		if (Params->TryGetStringField(TEXT("bytes_b64"), BytesB64) && !BytesB64.IsEmpty())
		{
			BytesB64 = StripSvgDataUrlPrefix(BytesB64, FormatHint);
			BytesB64 = CompactBase64Payload(BytesB64);
			TArray<uint8> DecodedBytes;
			if (!FBase64::Decode(BytesB64, DecodedBytes) || DecodedBytes.Num() == 0)
			{
				OutError = TEXT("Base64 decode of SVG bytes_b64 failed or produced empty buffer");
				return false;
			}
			if (DecodedBytes.Num() > MaxSvgInputChars)
			{
				OutError = FString::Printf(TEXT("SVG bytes exceed max size of %d bytes"), MaxSvgInputChars);
				return false;
			}
			FFileHelper::BufferToString(OutSvg, DecodedBytes.GetData(), DecodedBytes.Num());
			OutSourceKind = TEXT("external_bytes");
			OutFallbackName = TEXT("ImportedVector");
			return true;
		}

		FString FilePath;
		if (!Params->TryGetStringField(TEXT("file_path"), FilePath) || FilePath.IsEmpty())
		{
			Params->TryGetStringField(TEXT("path"), FilePath);
		}
		if (!FilePath.IsEmpty())
		{
			if (!FPaths::FileExists(FilePath))
			{
				OutError = FString::Printf(TEXT("SVG file does not exist: %s"), *FilePath);
				return false;
			}
			if (!FFileHelper::LoadFileToString(OutSvg, *FilePath))
			{
				OutError = FString::Printf(TEXT("Failed to read SVG file: %s"), *FilePath);
				return false;
			}
			OutSourceKind = TEXT("external_file");
			OutFallbackName = FPaths::GetBaseFilename(FilePath);
			return true;
		}

		OutError = TEXT("SVG input requires svg_text, bytes_b64, file_path, or path");
		return false;
	}

	static FMonolithActionResult ProcessSvg(
		const TSharedPtr<FJsonObject>& Params,
		ESvgActionMode Mode,
		const FString& InputSvg,
		const FString& SourceKind,
		const FString& FallbackName,
		const FString& Provider,
		const FString& Model)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
		}

		FString OptionsError;
		const FSvgOptions Options = ResolveOptions(Params, Mode, OptionsError);
		if (!OptionsError.IsEmpty())
		{
			return FMonolithActionResult::Error(OptionsError, -32602);
		}

		FSvgState State;
		State.OriginalSvg = InputSvg;
		State.OriginalHash = Mode == ESvgActionMode::Generate ? FString() : HashStringUtf8(InputSvg);
		State.Profile = Options.Profile;
		State.GeometryPolicy = Options.GeometryPolicy;
		State.FillRulePolicy = Options.FillRulePolicy;
		State.EffectiveFillRule = TEXT("nonzero");

		FString SanitizeError;
		if (!SanitizeSvgText(InputSvg, Options, State, SanitizeError))
		{
			return FMonolithActionResult::Error(SanitizeError, -32602);
		}

		if (Options.bStrict && (State.Warnings.Num() > 0 || State.Removed.Num() > 0))
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("SVG sanitizer produced warnings under strict=true: %s"), *FString::Join(State.Warnings, TEXT("; "))),
				-32602);
		}
		if (Options.bStrict && Options.Profile == TEXT("msdf_source") && !State.bMsdfReady)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("SVG is not msdf_ready: %s"), *FString::Join(State.Blockers, TEXT(", "))),
				-32602);
		}

		FString Prompt;
		Params->TryGetStringField(TEXT("prompt"), Prompt);
		TSharedPtr<FJsonObject> Provenance = BuildProvenance(Provider, Model, SourceKind, Prompt, State.OriginalHash);
		Provenance->SetStringField(TEXT("svg_hash"), State.SvgHash);
		Provenance->SetStringField(TEXT("profile"), State.Profile);
		Provenance->SetBoolField(TEXT("msdf_ready"), State.bMsdfReady);

		FString PackagePath;
		FString SourceSvgPath;
		FString SidecarPath;
		if (Options.bSave)
		{
			FString DestinationError;
			if (!ResolveSvgDestinationPackage(Params, FallbackName, PackagePath, DestinationError))
			{
				return FMonolithActionResult::Error(DestinationError, -32602);
			}

			FString OverwritePolicy;
			if (!Params->TryGetStringField(TEXT("overwrite_policy"), OverwritePolicy) || OverwritePolicy.IsEmpty())
			{
				OverwritePolicy = TEXT("unique");
			}
			OverwritePolicy = OverwritePolicy.ToLower();
			if (OverwritePolicy != TEXT("unique") && OverwritePolicy != TEXT("fail"))
			{
				return FMonolithActionResult::Error(TEXT("overwrite_policy must be 'unique' or 'fail'"), -32602);
			}

			SourceSvgPath = ResolveSvgSourcePath(PackagePath);
			if (OverwritePolicy == TEXT("fail") && FPaths::FileExists(SourceSvgPath))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("SVG source already exists: %s"), *SourceSvgPath), -32602);
			}
			if (OverwritePolicy == TEXT("unique"))
			{
				MakeUniqueSvgPaths(PackagePath, SourceSvgPath);
			}
			SidecarPath = FPaths::ChangeExtension(SourceSvgPath, TEXT("monolith.json"));

			FString SaveError;
			if (!SaveUtf8TextFile(SourceSvgPath, State.SanitizedSvg, SaveError))
			{
				return FMonolithActionResult::Error(SaveError, -32603);
			}
			Provenance->SetStringField(TEXT("source_svg_path"), SourceSvgPath);
			Provenance->SetStringField(TEXT("source_svg_hash"), State.SvgHash);
		}

		TSharedPtr<FJsonObject> Result = BuildResultObject(State, Options, Provenance, SourceSvgPath, SidecarPath, PackagePath, Options.bSave);
		if (Options.bSave)
		{
			FString SaveError;
			if (!SaveSidecar(SidecarPath, Result, SaveError))
			{
				return FMonolithActionResult::Error(SaveError, -32603);
			}
		}
		return FMonolithActionResult::Success(Result);
	}
}
}

namespace MonolithImageGen::SvgSource
{
	FString GetDefaultVectorAssetPath()
	{
		return DefaultVectorAssetPath;
	}

	FString GetDefaultVectorSourceDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), VectorDirectoryName, TEXT("Vector")));
	}

	TArray<FString> GetSupportedProfiles()
	{
		return { TEXT("web"), TEXT("editor"), TEXT("msdf_source") };
	}

	TArray<FString> GetGeometryPolicies()
	{
		return { TEXT("sanitize_only"), TEXT("validate"), TEXT("normalize") };
	}

	TArray<FString> GetFillRulePolicies()
	{
		return { TEXT("preserve"), TEXT("nonzero"), TEXT("reject_evenodd") };
	}

	void AddSvgDefaults(TSharedPtr<FJsonObject> Result)
	{
		if (!Result.IsValid())
		{
			return;
		}
		Result->SetStringField(TEXT("vector_asset_path"), DefaultVectorAssetPath);
		Result->SetStringField(TEXT("vector_source_dir"), GetDefaultVectorSourceDir());
		Result->SetStringField(TEXT("svg_model"), DefaultVectorModel);
		Result->SetStringField(TEXT("svg_prompt_policy"), TEXT("redacted: SVG provenance stores prompt_hash only"));
		Result->SetStringField(TEXT("svg_runtime_policy"), TEXT("runtime must consume precomputed Texture2D or MSDF assets; SVG parsing is editor/import/cook-time only"));
		SetStringArray(Result, TEXT("svg_profiles"), GetSupportedProfiles());
		SetStringArray(Result, TEXT("svg_geometry_policies"), GetGeometryPolicies());
		SetStringArray(Result, TEXT("svg_fill_rule_policies"), GetFillRulePolicies());
		SetStringArray(Result, TEXT("svg_actions"), {
			TEXT("imagegen.generate_svg"),
			TEXT("imagegen.import_generated_svg"),
			TEXT("imagegen.validate_svg"),
			TEXT("imagegen.generate_msdf_from_svg")
		});
		Result->SetStringField(TEXT("msdf_default_asset_path"), TEXT("/Game/GeneratedImages/MSDF"));
		Result->SetStringField(TEXT("msdf_default_model"), TEXT("monolith/local-msdf-cpu-v1"));
		Result->SetStringField(TEXT("msdf_texture_settings"), TEXT("TC_Masks, sRGB=false, TMGS_NoMipmaps, clamp addressing"));
	}

	void AddSvgModelEntries(TArray<TSharedPtr<FJsonValue>>& Models)
	{
		TSharedPtr<FJsonObject> LocalSvg = MakeShared<FJsonObject>();
		LocalSvg->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		LocalSvg->SetStringField(TEXT("model"), DefaultVectorModel);
		LocalSvg->SetBoolField(TEXT("available"), true);
		LocalSvg->SetBoolField(TEXT("network_required"), false);
		LocalSvg->SetStringField(TEXT("output_format"), TEXT("svg"));
		LocalSvg->SetStringField(TEXT("boundary_action"), TEXT("imagegen.generate_svg"));
		SetStringArray(LocalSvg, TEXT("profiles"), GetSupportedProfiles());
		Models.Add(MakeShared<FJsonValueObject>(LocalSvg));

		TSharedPtr<FJsonObject> ExternalSvg = MakeShared<FJsonObject>();
		ExternalSvg->SetStringField(TEXT("provider"), TEXT("external"));
		ExternalSvg->SetStringField(TEXT("model"), TEXT("caller_supplied_svg"));
		ExternalSvg->SetBoolField(TEXT("available"), true);
		ExternalSvg->SetBoolField(TEXT("network_required"), false);
		ExternalSvg->SetStringField(TEXT("output_format"), TEXT("svg"));
		ExternalSvg->SetStringField(TEXT("boundary_action"), TEXT("imagegen.import_generated_svg"));
		ExternalSvg->SetStringField(TEXT("secret_policy"), TEXT("Monolith stores no SVG prompt text or provider credentials."));
		SetStringArray(ExternalSvg, TEXT("profiles"), GetSupportedProfiles());
		Models.Add(MakeShared<FJsonValueObject>(ExternalSvg));

		TSharedPtr<FJsonObject> LocalMsdf = MakeShared<FJsonObject>();
		LocalMsdf->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		LocalMsdf->SetStringField(TEXT("model"), TEXT("monolith/local-msdf-cpu-v1"));
		LocalMsdf->SetBoolField(TEXT("available"), true);
		LocalMsdf->SetBoolField(TEXT("network_required"), false);
		LocalMsdf->SetStringField(TEXT("output_format"), TEXT("png_texture2d_msdf"));
		LocalMsdf->SetStringField(TEXT("boundary_action"), TEXT("imagegen.generate_msdf_from_svg"));
		LocalMsdf->SetStringField(TEXT("input_profile"), TEXT("msdf_source"));
		Models.Add(MakeShared<FJsonValueObject>(LocalMsdf));
	}

	FMonolithActionResult HandleGenerateSvg(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
		}

		FString Svg;
		FString FallbackName;
		FString Error;
		if (!BuildSvgFromSpec(Params, Svg, FallbackName, Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}

		return ProcessSvg(
			Params,
			ESvgActionMode::Generate,
			Svg,
			TEXT("local_deterministic"),
			FallbackName,
			TEXT("local_deterministic"),
			DefaultVectorModel);
	}

	FMonolithActionResult HandleImportGeneratedSvg(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
		}

		FString Svg;
		FString SourceKind;
		FString FallbackName;
		FString Error;
		if (!LoadSvgInputFromParams(Params, Svg, SourceKind, FallbackName, Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}

		FString Provider;
		if (!Params->TryGetStringField(TEXT("provider"), Provider) || Provider.IsEmpty())
		{
			Provider = TEXT("external");
		}
		FString Model;
		if (!Params->TryGetStringField(TEXT("model"), Model) || Model.IsEmpty())
		{
			Model = TEXT("unknown");
		}

		return ProcessSvg(
			Params,
			ESvgActionMode::Import,
			Svg,
			SourceKind,
			SanitizeVectorName(FallbackName),
			Provider,
			Model);
	}

	FMonolithActionResult HandleValidateSvg(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
		}

		FString Svg;
		FString SourceKind;
		FString FallbackName;
		FString Error;
		if (!LoadSvgInputFromParams(Params, Svg, SourceKind, FallbackName, Error))
		{
			return FMonolithActionResult::Error(Error, -32602);
		}

		TSharedPtr<FJsonObject> EffectiveParams = MakeShared<FJsonObject>();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Params))
		{
			EffectiveParams->SetField(Pair.Key, Pair.Value);
		}
		EffectiveParams->SetBoolField(TEXT("save"), false);
		if (Params->HasField(TEXT("return_sanitized_svg")))
		{
			bool bReturnSanitizedSvg = false;
			if (!Params->TryGetBoolField(TEXT("return_sanitized_svg"), bReturnSanitizedSvg))
			{
				return FMonolithActionResult::Error(TEXT("Invalid parameter type: return_sanitized_svg must be a boolean"), -32602);
			}
			EffectiveParams->SetBoolField(TEXT("return_svg"), bReturnSanitizedSvg);
		}

		return ProcessSvg(
			EffectiveParams,
			ESvgActionMode::Validate,
			Svg,
			SourceKind,
			FallbackName,
			TEXT("validator"),
			TEXT("none"));
	}

	FMonolithActionResult HandleGenerateMsdfFromSvg(const TSharedPtr<FJsonObject>& Params)
	{
		if (!Params.IsValid())
		{
			return FMonolithActionResult::Error(TEXT("Missing params object"), -32602);
		}

		FMsdfOptions MsdfOptions;
		double SizeValue = static_cast<double>(MsdfOptions.Size);
		if (Params->TryGetNumberField(TEXT("size"), SizeValue) || Params->TryGetNumberField(TEXT("resolution"), SizeValue))
		{
			MsdfOptions.Size = static_cast<int32>(SizeValue);
		}
		double PixelRangeValue = static_cast<double>(MsdfOptions.PixelRange);
		if (Params->TryGetNumberField(TEXT("pixel_range"), PixelRangeValue))
		{
			MsdfOptions.PixelRange = static_cast<int32>(PixelRangeValue);
		}
		if (MsdfOptions.Size < 16 || MsdfOptions.Size > 2048)
		{
			return FMonolithActionResult::Error(TEXT("size/resolution must be in [16, 2048]"), -32602);
		}
		if (MsdfOptions.PixelRange < 1 || MsdfOptions.PixelRange > 64)
		{
			return FMonolithActionResult::Error(TEXT("pixel_range must be in [1, 64]"), -32602);
		}
		Params->TryGetBoolField(TEXT("save"), MsdfOptions.bSave);
		if (Params->HasField(TEXT("save_source_png")))
		{
			if (!Params->TryGetBoolField(TEXT("save_source_png"), MsdfOptions.bSaveSourcePng))
			{
				return FMonolithActionResult::Error(TEXT("Invalid parameter type: save_source_png must be a boolean"), -32602);
			}
		}
		else
		{
			MsdfOptions.bSaveSourcePng = MsdfOptions.bSave;
		}
		Params->TryGetBoolField(TEXT("return_png"), MsdfOptions.bReturnPng);
		Params->TryGetBoolField(TEXT("verify_samples"), MsdfOptions.bVerifySamples);
		Params->TryGetBoolField(TEXT("create_material"), MsdfOptions.bCreateMaterial);
		Params->TryGetBoolField(TEXT("verify_material_render"), MsdfOptions.bVerifyMaterialRender);
		Params->TryGetStringField(TEXT("overwrite_policy"), MsdfOptions.OverwritePolicy);
		Params->TryGetStringField(TEXT("material_overwrite_policy"), MsdfOptions.MaterialOverwritePolicy);
		MsdfOptions.OverwritePolicy = MsdfOptions.OverwritePolicy.ToLower();
		MsdfOptions.MaterialOverwritePolicy = MsdfOptions.MaterialOverwritePolicy.ToLower();
		if (MsdfOptions.OverwritePolicy != TEXT("unique") && MsdfOptions.OverwritePolicy != TEXT("fail"))
		{
			return FMonolithActionResult::Error(TEXT("overwrite_policy must be 'unique' or 'fail'"), -32602);
		}
		if (MsdfOptions.MaterialOverwritePolicy != TEXT("unique") && MsdfOptions.MaterialOverwritePolicy != TEXT("fail"))
		{
			return FMonolithActionResult::Error(TEXT("material_overwrite_policy must be 'unique' or 'fail'"), -32602);
		}

		FString Svg;
		FString SourceKind;
		FString FallbackName;
		FString InputError;
		if (Params->HasTypedField<EJson::Object>(TEXT("svg_spec"))
			|| (!Params->HasField(TEXT("svg_text")) && !Params->HasField(TEXT("bytes_b64")) && !Params->HasField(TEXT("file_path")) && !Params->HasField(TEXT("path")) && Params->HasField(TEXT("prompt"))))
		{
			if (!BuildSvgFromSpec(Params, Svg, FallbackName, InputError))
			{
				return FMonolithActionResult::Error(InputError, -32602);
			}
			SourceKind = TEXT("local_deterministic");
		}
		else if (!LoadSvgInputFromParams(Params, Svg, SourceKind, FallbackName, InputError))
		{
			return FMonolithActionResult::Error(InputError, -32602);
		}

		TSharedPtr<FJsonObject> EffectiveParams = MakeShared<FJsonObject>();
		for (const auto& Pair : FMonolithJsonUtils::GetFields(Params))
		{
			EffectiveParams->SetField(Pair.Key, Pair.Value);
		}
		EffectiveParams->SetStringField(TEXT("profile"), TEXT("msdf_source"));
		EffectiveParams->SetStringField(TEXT("geometry_policy"), TEXT("validate"));
		EffectiveParams->SetBoolField(TEXT("save"), false);
		EffectiveParams->SetBoolField(TEXT("strict"), false);

		FString OptionsError;
		const FSvgOptions SvgOptions = ResolveOptions(EffectiveParams, ESvgActionMode::Validate, OptionsError);
		if (!OptionsError.IsEmpty())
		{
			return FMonolithActionResult::Error(OptionsError, -32602);
		}

		FSvgState State;
		State.OriginalSvg = Svg;
		State.OriginalHash = HashStringUtf8(Svg);
		State.Profile = SvgOptions.Profile;
		State.GeometryPolicy = SvgOptions.GeometryPolicy;
		State.FillRulePolicy = SvgOptions.FillRulePolicy;
		State.EffectiveFillRule = TEXT("nonzero");

		FString SanitizeError;
		if (!SanitizeSvgText(Svg, SvgOptions, State, SanitizeError))
		{
			return FMonolithActionResult::Error(SanitizeError, -32602);
		}
		if (!State.bMsdfReady)
		{
			return FMonolithActionResult::Error(
				FString::Printf(TEXT("SVG is not msdf_ready: %s"), *FString::Join(State.Blockers, TEXT(", "))),
				-32602);
		}

		FMsdfBake Bake;
		FString BakeError;
		if (!BakeMsdf(State, MsdfOptions, Bake, BakeError))
		{
			return FMonolithActionResult::Error(BakeError, -32603);
		}
		if (MsdfOptions.bVerifySamples)
		{
			FString SampleValidationError;
			if (!ValidateMsdfBakeSamples(Bake, SampleValidationError))
			{
				return FMonolithActionResult::Error(SampleValidationError, -32603);
			}
		}

		FString TexturePackageError;
		FString TexturePackagePath = ResolveMsdfPackagePath(
			Params,
			TEXT("destination"),
			TEXT("asset_path"),
			TEXT("asset_name"),
			TEXT("/Game/GeneratedImages/MSDF"),
			FallbackName,
			false,
			TexturePackageError);
		if (!TexturePackageError.IsEmpty())
		{
			return FMonolithActionResult::Error(TexturePackageError, -32602);
		}
		if (MsdfOptions.OverwritePolicy == TEXT("fail") && FPackageName::DoesPackageExist(TexturePackagePath))
		{
			return FMonolithActionResult::Error(FString::Printf(TEXT("MSDF texture package already exists: %s"), *TexturePackagePath), -32602);
		}
		if (MsdfOptions.OverwritePolicy == TEXT("unique"))
		{
			MakeUniquePackagePath(TexturePackagePath);
		}

		FString MsdfPngPath;
		FString MsdfPngHash = HashBytes(Bake.PngBytes);
		if (MsdfOptions.bSaveSourcePng)
		{
			MsdfPngPath = ResolveMsdfPngPath(TexturePackagePath);
			FString SavePngError;
			if (!SaveBytesFile(MsdfPngPath, Bake.PngBytes, SavePngError))
			{
				return FMonolithActionResult::Error(SavePngError, -32603);
			}
		}

		TSharedPtr<FJsonObject> TextureSettings = MakeShared<FJsonObject>();
		TextureSettings->SetStringField(TEXT("compression_settings"), TEXT("TC_Masks"));
		TextureSettings->SetBoolField(TEXT("srgb"), false);
		TextureSettings->SetStringField(TEXT("mip_gen_settings"), TEXT("TMGS_NoMipmaps"));
		TextureSettings->SetStringField(TEXT("lod_group"), TEXT("TEXTUREGROUP_UI"));
		TextureSettings->SetStringField(TEXT("address_x"), TEXT("TA_Clamp"));
		TextureSettings->SetStringField(TEXT("address_y"), TEXT("TA_Clamp"));
		TextureSettings->SetBoolField(TEXT("never_stream"), true);
		TextureSettings->SetNumberField(TEXT("max_texture_size"), MsdfOptions.Size);

		TSharedPtr<FJsonObject> ImportParams = MakeShared<FJsonObject>();
		ImportParams->SetStringField(TEXT("destination"), TexturePackagePath);
		ImportParams->SetStringField(TEXT("bytes_b64"), FBase64::Encode(Bake.PngBytes));
		ImportParams->SetStringField(TEXT("format_hint"), TEXT("png"));
		ImportParams->SetBoolField(TEXT("save"), MsdfOptions.bSave);
		ImportParams->SetObjectField(TEXT("settings"), TextureSettings);
		FMonolithActionResult ImportResult = MonolithAsset::FTextureIngestActions::HandleImportTextureFromBytes(ImportParams);
		if (!ImportResult.bSuccess)
		{
			return ImportResult;
		}
		FString ImportedTexturePath = TexturePackagePath;
		if (ImportResult.Result.IsValid())
		{
			ImportResult.Result->TryGetStringField(TEXT("asset_path"), ImportedTexturePath);
		}
		FString TextureSettingsError;
		if (!ApplyMsdfTextureSettings(ImportedTexturePath, MsdfOptions.Size, TextureSettingsError))
		{
			return FMonolithActionResult::Error(TextureSettingsError, -32603);
		}

		TSharedPtr<FJsonObject> Provenance = MakeShared<FJsonObject>();
		Provenance->SetStringField(TEXT("kind"), TEXT("msdf"));
		Provenance->SetStringField(TEXT("provider"), TEXT("local_deterministic"));
		Provenance->SetStringField(TEXT("model"), TEXT("monolith/local-msdf-cpu-v1"));
		Provenance->SetStringField(TEXT("source"), SourceKind);
		Provenance->SetStringField(TEXT("svg_hash"), State.SvgHash);
		Provenance->SetStringField(TEXT("original_svg_hash"), State.OriginalHash);
		Provenance->SetStringField(TEXT("msdf_png_hash"), MsdfPngHash);
		Provenance->SetStringField(TEXT("generated_at_utc"), FDateTime::UtcNow().ToIso8601());
		Provenance->SetStringField(TEXT("msdf_generator"), TEXT("monolith_cpu_contour_msdf_v1"));
		Provenance->SetBoolField(TEXT("prompt_redacted"), true);
		FString Prompt;
		if (Params->TryGetStringField(TEXT("prompt"), Prompt) && !Prompt.IsEmpty())
		{
			Provenance->SetStringField(TEXT("prompt_hash"), HashStringUtf8(Prompt));
		}
		if (!MsdfPngPath.IsEmpty())
		{
			Provenance->SetStringField(TEXT("source_png_path"), MsdfPngPath);
			Provenance->SetStringField(TEXT("source_png_hash"), MsdfPngHash);
		}
		FString ProvenanceError;
		if (!ApplyMsdfProvenance(ImportedTexturePath, Provenance, MsdfOptions.bSave, ProvenanceError))
		{
			return FMonolithActionResult::Error(ProvenanceError, -32603);
		}

		FString MaterialPath;
		TSharedPtr<FJsonObject> MaterialResult;
		if (MsdfOptions.bCreateMaterial)
		{
			FString MaterialPathError;
			FString RequestedMaterialDestination;
			Params->TryGetStringField(TEXT("material_destination"), RequestedMaterialDestination);
			if (!RequestedMaterialDestination.IsEmpty())
			{
				TSharedPtr<FJsonObject> MaterialParams = MakeShared<FJsonObject>();
				MaterialParams->SetStringField(TEXT("material_destination"), RequestedMaterialDestination);
				MaterialPath = ResolveMsdfPackagePath(
					MaterialParams,
					TEXT("material_destination"),
					TEXT("material_asset_path"),
					TEXT("material_asset_name"),
					FPackageName::GetLongPackagePath(ImportedTexturePath),
					SanitizeMaterialName(FPackageName::GetLongPackageAssetName(ImportedTexturePath)),
					true,
					MaterialPathError);
			}
			else
			{
				TSharedPtr<FJsonObject> MaterialParams = MakeShared<FJsonObject>();
				FString MaterialAssetPath;
				if (Params->TryGetStringField(TEXT("material_asset_path"), MaterialAssetPath) && !MaterialAssetPath.IsEmpty())
				{
					MaterialParams->SetStringField(TEXT("material_asset_path"), MaterialAssetPath);
				}
				FString MaterialAssetName;
				if (Params->TryGetStringField(TEXT("material_asset_name"), MaterialAssetName) && !MaterialAssetName.IsEmpty())
				{
					MaterialParams->SetStringField(TEXT("material_asset_name"), MaterialAssetName);
				}
				MaterialPath = ResolveMsdfPackagePath(
					MaterialParams,
					TEXT("material_destination"),
					TEXT("material_asset_path"),
					TEXT("material_asset_name"),
					FPackageName::GetLongPackagePath(ImportedTexturePath),
					SanitizeMaterialName(FPackageName::GetLongPackageAssetName(ImportedTexturePath)),
					true,
					MaterialPathError);
			}
			if (!MaterialPathError.IsEmpty())
			{
				return FMonolithActionResult::Error(MaterialPathError, -32602);
			}
			if (MsdfOptions.MaterialOverwritePolicy == TEXT("fail") && FPackageName::DoesPackageExist(MaterialPath))
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("MSDF material package already exists: %s"), *MaterialPath), -32602);
			}
			if (MsdfOptions.MaterialOverwritePolicy == TEXT("unique"))
			{
				MakeUniquePackagePath(MaterialPath);
			}
			FString MaterialError;
			MaterialResult = CreateMsdfMaterialAndPreview(ImportedTexturePath, MaterialPath, MsdfOptions.bVerifyMaterialRender, MaterialError);
			if (!MaterialError.IsEmpty() && MsdfOptions.bVerifyMaterialRender)
			{
				return FMonolithActionResult::Error(FString::Printf(TEXT("MSDF material verification failed: %s"), *MaterialError), -32603);
			}
		}

		TSharedPtr<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetBoolField(TEXT("success"), true);
		Result->SetStringField(TEXT("asset_path"), ImportedTexturePath);
		Result->SetStringField(TEXT("object_path"), ObjectPathFromPackagePath(ImportedTexturePath));
		Result->SetStringField(TEXT("msdf_texture_asset_path"), ImportedTexturePath);
		Result->SetNumberField(TEXT("width"), Bake.Width);
		Result->SetNumberField(TEXT("height"), Bake.Height);
		Result->SetNumberField(TEXT("pixel_range"), MsdfOptions.PixelRange);
		Result->SetNumberField(TEXT("range_svg"), Bake.RangeSvg);
		Result->SetStringField(TEXT("svg_hash"), State.SvgHash);
		Result->SetStringField(TEXT("msdf_png_hash"), MsdfPngHash);
		Result->SetNumberField(TEXT("png_bytes"), Bake.PngBytes.Num());
		Result->SetNumberField(TEXT("channel_spread_max"), Bake.ChannelSpreadMax);
		Result->SetNumberField(TEXT("median_min"), Bake.MedianMin);
		Result->SetNumberField(TEXT("median_max"), Bake.MedianMax);
		Result->SetStringField(TEXT("msdf_generator"), TEXT("monolith_cpu_contour_msdf_v1"));
		if (!MsdfPngPath.IsEmpty())
		{
			Result->SetStringField(TEXT("msdf_png_path"), MsdfPngPath);
		}
		Result->SetArrayField(TEXT("channel_samples"), Bake.Samples);
		Result->SetObjectField(TEXT("texture_import"), ImportResult.Result);
		Result->SetObjectField(TEXT("texture_settings"), TextureSettings);
		Result->SetObjectField(TEXT("provenance"), Provenance);
		Result->SetStringField(TEXT("view_box"), FormatViewBox(State.ViewBox));
		Result->SetObjectField(TEXT("bounds"), BoundsToJson(State));
		Result->SetNumberField(TEXT("contour_count"), State.Contours.Num());
		if (MaterialResult.IsValid())
		{
			Result->SetStringField(TEXT("material_path"), MaterialPath);
			Result->SetObjectField(TEXT("material"), MaterialResult);
		}
		if (MsdfOptions.bReturnPng)
		{
			Result->SetStringField(TEXT("png_b64"), FBase64::Encode(Bake.PngBytes));
		}
		return FMonolithActionResult::Success(Result);
	}
}
