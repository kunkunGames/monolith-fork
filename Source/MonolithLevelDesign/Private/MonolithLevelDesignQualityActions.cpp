#include "MonolithLevelDesignQualityActions.h"
#include "MonolithMeshUtils.h"
#include "MonolithToolRegistry.h"
#include "MonolithParamSchema.h"

#include "Components/LightComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Editor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
	TArray<TSharedPtr<FJsonValue>> VectorToJsonArray(const FVector& V)
	{
		TArray<TSharedPtr<FJsonValue>> Arr;
		Arr.Reserve(3);
		Arr.Add(MakeShared<FJsonValueNumber>(V.X));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Y));
		Arr.Add(MakeShared<FJsonValueNumber>(V.Z));
		return Arr;
	}
}
void FMonolithLevelDesignQualityActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("leveldesign"), TEXT("analyze_framing"),
		TEXT("Camera composition scoring: rule of thirds placement, depth layering, leading lines. Projects actors to screen space from a camera viewpoint and analyzes composition."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelDesignQualityActions::AnalyzeFraming),
		FParamSchemaBuilder()
			.Required(TEXT("camera_location"), TEXT("array"), TEXT("Camera position [x, y, z]"))
			.Required(TEXT("camera_rotation"), TEXT("array"), TEXT("Camera rotation [pitch, yaw, roll]"))
			.Optional(TEXT("focal_actor"), TEXT("string"), TEXT("Name of the focal point actor"))
			.Optional(TEXT("fov"), TEXT("number"), TEXT("Field of view in degrees"), TEXT("90"))
			.Optional(TEXT("aspect_ratio"), TEXT("number"), TEXT("Aspect ratio (width/height)"), TEXT("1.777"))
			.Build());

	Registry.RegisterAction(TEXT("leveldesign"), TEXT("evaluate_monster_reveal"),
		TEXT("Score a monster reveal moment: silhouette quality (screen coverage), backlight potential, distance rating, partial visibility, player camera alignment. Uses traces and sightline analysis."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelDesignQualityActions::EvaluateMonsterReveal),
		FParamSchemaBuilder()
			.Required(TEXT("player_location"), TEXT("array"), TEXT("Player camera position [x, y, z]"))
			.Required(TEXT("player_rotation"), TEXT("array"), TEXT("Player camera rotation [pitch, yaw, roll]"))
			.Required(TEXT("monster_actor"), TEXT("string"), TEXT("Name of the monster/creature actor"))
			.Optional(TEXT("fov"), TEXT("number"), TEXT("Field of view in degrees"), TEXT("90"))
			.Build());

	Registry.RegisterAction(TEXT("leveldesign"), TEXT("analyze_co_op_balance"),
		TEXT("Analyze spatial design for co-op play: coverage blind spots, separation opportunities, communication distances. Given multiple player positions, evaluate the level's co-op balance."),
		FMonolithActionHandler::CreateStatic(&FMonolithLevelDesignQualityActions::AnalyzeCoOpBalance),
		FParamSchemaBuilder()
			.Required(TEXT("player_positions"), TEXT("array"), TEXT("Array of player positions [[x,y,z], ...]"))
			.Optional(TEXT("region_min"), TEXT("array"), TEXT("Min corner of analysis region [x, y, z]"))
			.Optional(TEXT("region_max"), TEXT("array"), TEXT("Max corner of analysis region [x, y, z]"))
			.Build());
}


// ============================================================================
// 6. analyze_framing
// ============================================================================

namespace
{
	/** Project a world point to normalized screen coords (0-1) given camera params */
	bool ProjectToScreen(const FVector& WorldPoint, const FVector& CamLoc, const FRotator& CamRot,
		float FOVDeg, float AspectRatio, FVector2D& OutScreenPos)
	{
		FVector ToPoint = WorldPoint - CamLoc;
		FVector Forward = CamRot.Vector();
		FVector Right = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Y);
		FVector Up = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Z);

		float Depth = FVector::DotProduct(ToPoint, Forward);
		if (Depth <= 0.0f)
		{
			return false; // Behind camera
		}

		float HalfFovRad = FMath::DegreesToRadians(FOVDeg * 0.5f);
		float HalfWidth = Depth * FMath::Tan(HalfFovRad);
		float HalfHeight = HalfWidth / AspectRatio;

		float ScreenX = FVector::DotProduct(ToPoint, Right);
		float ScreenY = FVector::DotProduct(ToPoint, Up);

		OutScreenPos.X = (ScreenX / HalfWidth) * 0.5f + 0.5f;
		OutScreenPos.Y = (-ScreenY / HalfHeight) * 0.5f + 0.5f; // Flip Y

		return OutScreenPos.X >= 0.0f && OutScreenPos.X <= 1.0f &&
		       OutScreenPos.Y >= 0.0f && OutScreenPos.Y <= 1.0f;
	}

	/** Score how close a normalized screen point is to rule-of-thirds intersections */
	float ScoreRuleOfThirds(const FVector2D& ScreenPos)
	{
		// Four rule-of-thirds intersection points
		static const FVector2D ThirdsPoints[] = {
			{1.0f/3.0f, 1.0f/3.0f}, {2.0f/3.0f, 1.0f/3.0f},
			{1.0f/3.0f, 2.0f/3.0f}, {2.0f/3.0f, 2.0f/3.0f}
		};

		float BestDist = TNumericLimits<float>::Max();
		for (const FVector2D& Pt : ThirdsPoints)
		{
			float Dist = FVector2D::Distance(ScreenPos, Pt);
			BestDist = FMath::Min(BestDist, Dist);
		}

		// Score: 1.0 if exactly on a thirds point, 0.0 if far away (>0.3 normalized)
		return FMath::Clamp(1.0f - (BestDist / 0.3f), 0.0f, 1.0f);
	}
}

FMonolithActionResult FMonolithLevelDesignQualityActions::AnalyzeFraming(const TSharedPtr<FJsonObject>& Params)
{
	FVector CamLocation;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("camera_location"), CamLocation))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: camera_location"));
	}

	FRotator CamRotation;
	if (!MonolithMeshUtils::ParseRotator(Params, TEXT("camera_rotation"), CamRotation))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: camera_rotation"));
	}

	double FOV = 90.0;
	Params->TryGetNumberField(TEXT("fov"), FOV);
	FOV = FMath::Clamp(FOV, 30.0, 170.0);

	double AspectRatio = 1.777;
	Params->TryGetNumberField(TEXT("aspect_ratio"), AspectRatio);
	AspectRatio = FMath::Clamp(AspectRatio, 0.5, 4.0);

	FString FocalActorName;
	Params->TryGetStringField(TEXT("focal_actor"), FocalActorName);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FVector Forward = CamRotation.Vector();

	// Collect visible actors and project to screen
	struct FScreenActor
	{
		FString Name;
		FVector2D ScreenCenter;
		FVector2D ScreenMin;
		FVector2D ScreenMax;
		float Distance;
		float ScreenArea;
	};

	TArray<FScreenActor> ScreenActors;
	float HalfFovRad = FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f);

	for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);

		FVector2D ScreenCenter;
		if (!ProjectToScreen(Origin, CamLocation, CamRotation, static_cast<float>(FOV), static_cast<float>(AspectRatio), ScreenCenter))
		{
			continue;
		}

		// Project bounds corners for screen extent
		FVector2D ScreenMin(1.0f, 1.0f), ScreenMax(0.0f, 0.0f);
		FVector Corners[8];
		FBox Box(Origin - Extent, Origin + Extent);
		for (int32 i = 0; i < 8; ++i)
		{
			Corners[i] = FVector(
				(i & 1) ? Box.Max.X : Box.Min.X,
				(i & 2) ? Box.Max.Y : Box.Min.Y,
				(i & 4) ? Box.Max.Z : Box.Min.Z
			);
			FVector2D CornerScreen;
			if (ProjectToScreen(Corners[i], CamLocation, CamRotation, static_cast<float>(FOV), static_cast<float>(AspectRatio), CornerScreen))
			{
				ScreenMin.X = FMath::Min(ScreenMin.X, CornerScreen.X);
				ScreenMin.Y = FMath::Min(ScreenMin.Y, CornerScreen.Y);
				ScreenMax.X = FMath::Max(ScreenMax.X, CornerScreen.X);
				ScreenMax.Y = FMath::Max(ScreenMax.Y, CornerScreen.Y);
			}
		}

		FScreenActor SA;
		SA.Name = Actor->GetActorLabel();
		SA.ScreenCenter = ScreenCenter;
		SA.ScreenMin = ScreenMin;
		SA.ScreenMax = ScreenMax;
		SA.Distance = FVector::Dist(CamLocation, Origin);
		SA.ScreenArea = (ScreenMax.X - ScreenMin.X) * (ScreenMax.Y - ScreenMin.Y);
		ScreenActors.Add(SA);
	}

	// Analyze depth layers
	ScreenActors.Sort([](const FScreenActor& A, const FScreenActor& B) { return A.Distance < B.Distance; });

	int32 ForegroundCount = 0, MidgroundCount = 0, BackgroundCount = 0;
	float MaxDist = ScreenActors.Num() > 0 ? ScreenActors.Last().Distance : 1.0f;

	for (const FScreenActor& SA : ScreenActors)
	{
		float NormDist = SA.Distance / FMath::Max(MaxDist, 1.0f);
		if (NormDist < 0.2f) ForegroundCount++;
		else if (NormDist < 0.6f) MidgroundCount++;
		else BackgroundCount++;
	}

	// Focal actor analysis
	float FocalRuleOfThirdsScore = 0.0f;
	float FocalScreenCoverage = 0.0f;
	FString FocalStatus = TEXT("not_specified");

	if (!FocalActorName.IsEmpty())
	{
		bool bFound = false;
		for (const FScreenActor& SA : ScreenActors)
		{
			if (SA.Name == FocalActorName)
			{
				FocalRuleOfThirdsScore = ScoreRuleOfThirds(SA.ScreenCenter);
				FocalScreenCoverage = SA.ScreenArea;
				FocalStatus = TEXT("visible");
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FocalStatus = TEXT("not_visible");
		}
	}

	// Leading lines: trace from screen edges toward focal point
	int32 LeadingLineCount = 0;
	if (!FocalActorName.IsEmpty() && FocalStatus == TEXT("visible"))
	{
		FString FindErr;
		AActor* FocalActor = MonolithMeshUtils::FindActorByName(FocalActorName, FindErr);
		if (FocalActor)
		{
			FVector FocalLoc = FocalActor->GetActorLocation();
			// Sample rays from camera edges toward focal — count hits that create depth lines
			FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(FramingTrace), true);

			const FVector Right = FRotationMatrix(CamRotation).GetScaledAxis(EAxis::Y);
			const FVector Up = FRotationMatrix(CamRotation).GetScaledAxis(EAxis::Z);

			for (int32 i = 0; i < 8; ++i)
			{
				float Angle = (static_cast<float>(i) / 8.0f) * UE_TWO_PI;
				FVector EdgeDir = FMath::Cos(Angle) * Right + FMath::Sin(Angle) * Up;
				FVector RayStart = CamLocation + EdgeDir * 200.0f;
				FVector ToFocal = (FocalLoc - RayStart).GetSafeNormal();

				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, RayStart, RayStart + ToFocal * 5000.0f, ECC_Visibility, TraceParams))
				{
					// Edge hits near the focal direction suggest leading geometry
					float DotToFocal = FVector::DotProduct((Hit.ImpactPoint - CamLocation).GetSafeNormal(), (FocalLoc - CamLocation).GetSafeNormal());
					if (DotToFocal > 0.7f)
					{
						LeadingLineCount++;
					}
				}
			}
		}
	}

	// Overall composition score
	float DepthLayerScore = 0.0f;
	if (ForegroundCount > 0 && MidgroundCount > 0 && BackgroundCount > 0)
	{
		DepthLayerScore = 1.0f;
	}
	else if ((ForegroundCount > 0 && BackgroundCount > 0) || (ForegroundCount > 0 && MidgroundCount > 0) || (MidgroundCount > 0 && BackgroundCount > 0))
	{
		DepthLayerScore = 0.6f;
	}
	else
	{
		DepthLayerScore = 0.2f;
	}

	float LeadingLineScore = FMath::Clamp(static_cast<float>(LeadingLineCount) / 3.0f, 0.0f, 1.0f);
	float OverallScore = (FocalRuleOfThirdsScore * 0.4f + DepthLayerScore * 0.35f + LeadingLineScore * 0.25f) * 100.0f;

	// Build result
	auto Result = MakeShared<FJsonObject>();
	Result->SetArrayField(TEXT("camera_location"), VectorToJsonArray(CamLocation));
	Result->SetNumberField(TEXT("fov"), FOV);
	Result->SetNumberField(TEXT("visible_actors"), ScreenActors.Num());
	Result->SetNumberField(TEXT("overall_composition_score"), FMath::RoundToFloat(OverallScore));

	auto DepthObj = MakeShared<FJsonObject>();
	DepthObj->SetNumberField(TEXT("foreground"), ForegroundCount);
	DepthObj->SetNumberField(TEXT("midground"), MidgroundCount);
	DepthObj->SetNumberField(TEXT("background"), BackgroundCount);
	DepthObj->SetNumberField(TEXT("depth_layer_score"), DepthLayerScore);
	Result->SetObjectField(TEXT("depth_layers"), DepthObj);

	auto FocalObj = MakeShared<FJsonObject>();
	FocalObj->SetStringField(TEXT("status"), FocalStatus);
	FocalObj->SetNumberField(TEXT("rule_of_thirds_score"), FocalRuleOfThirdsScore);
	FocalObj->SetNumberField(TEXT("screen_coverage"), FocalScreenCoverage);
	FocalObj->SetNumberField(TEXT("leading_lines_detected"), LeadingLineCount);
	FocalObj->SetNumberField(TEXT("leading_line_score"), LeadingLineScore);
	Result->SetObjectField(TEXT("focal_analysis"), FocalObj);

	// Top 5 largest screen actors
	TArray<FScreenActor> BySize = ScreenActors;
	BySize.Sort([](const FScreenActor& A, const FScreenActor& B) { return A.ScreenArea > B.ScreenArea; });

	TArray<TSharedPtr<FJsonValue>> TopActorsArr;
	for (int32 i = 0; i < FMath::Min(5, BySize.Num()); ++i)
	{
		auto ActObj = MakeShared<FJsonObject>();
		ActObj->SetStringField(TEXT("name"), BySize[i].Name);
		ActObj->SetNumberField(TEXT("screen_area"), BySize[i].ScreenArea);
		ActObj->SetNumberField(TEXT("distance"), BySize[i].Distance);
		ActObj->SetNumberField(TEXT("rule_of_thirds_score"), ScoreRuleOfThirds(BySize[i].ScreenCenter));
		TopActorsArr.Add(MakeShared<FJsonValueObject>(ActObj));
	}
	Result->SetArrayField(TEXT("dominant_actors"), TopActorsArr);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 7. evaluate_monster_reveal
// ============================================================================

FMonolithActionResult FMonolithLevelDesignQualityActions::EvaluateMonsterReveal(const TSharedPtr<FJsonObject>& Params)
{
	FVector PlayerLoc;
	if (!MonolithMeshUtils::ParseVector(Params, TEXT("player_location"), PlayerLoc))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: player_location"));
	}

	FRotator PlayerRot;
	if (!MonolithMeshUtils::ParseRotator(Params, TEXT("player_rotation"), PlayerRot))
	{
		return FMonolithActionResult::Error(TEXT("Missing or invalid required param: player_rotation"));
	}

	FString MonsterName;
	if (!Params->TryGetStringField(TEXT("monster_actor"), MonsterName) || MonsterName.IsEmpty())
	{
		return FMonolithActionResult::Error(TEXT("Required: monster_actor (name of the creature actor)"));
	}

	double FOV = 90.0;
	Params->TryGetNumberField(TEXT("fov"), FOV);
	FOV = FMath::Clamp(FOV, 30.0, 170.0);

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	FString FindErr;
	AActor* MonsterActor = MonolithMeshUtils::FindActorByName(MonsterName, FindErr);
	if (!MonsterActor)
	{
		return FMonolithActionResult::Error(FString::Printf(TEXT("Monster actor not found: %s — %s"), *MonsterName, *FindErr));
	}

	FVector MonsterOrigin, MonsterExtent;
	MonsterActor->GetActorBounds(false, MonsterOrigin, MonsterExtent);
	float Distance = FVector::Dist(PlayerLoc, MonsterOrigin);

	FVector Forward = PlayerRot.Vector();
	FVector ToMonster = (MonsterOrigin - PlayerLoc).GetSafeNormal();

	// --- Camera alignment ---
	float DotForward = FVector::DotProduct(Forward, ToMonster);
	float AlignmentScore = FMath::Clamp(DotForward, 0.0f, 1.0f);
	bool bInFOV = DotForward > FMath::Cos(FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f));

	// --- Silhouette (screen coverage) ---
	FVector2D ScreenCenter;
	float SilhouetteScore = 0.0f;
	float ScreenCoverage = 0.0f;
	if (bInFOV)
	{
		ProjectToScreen(MonsterOrigin, PlayerLoc, PlayerRot, static_cast<float>(FOV), 1.777f, ScreenCenter);

		// Approximate screen coverage from bounds
		float AngularSize = FMath::Atan2(MonsterExtent.Size(), FMath::Max(Distance, 1.0f));
		ScreenCoverage = FMath::Clamp(AngularSize / FMath::DegreesToRadians(static_cast<float>(FOV) * 0.5f), 0.0f, 1.0f);

		// Best silhouette: 10-30% of screen coverage
		if (ScreenCoverage >= 0.1f && ScreenCoverage <= 0.3f)
		{
			SilhouetteScore = 1.0f;
		}
		else if (ScreenCoverage > 0.3f)
		{
			SilhouetteScore = FMath::Clamp(1.0f - (ScreenCoverage - 0.3f) / 0.4f, 0.0f, 1.0f);
		}
		else
		{
			SilhouetteScore = FMath::Clamp(ScreenCoverage / 0.1f, 0.0f, 1.0f);
		}
	}

	// --- Distance rating ---
	// Ideal reveal: 8-20 meters
	float DistanceScore;
	if (Distance >= 800.0f && Distance <= 2000.0f)
	{
		DistanceScore = 1.0f;
	}
	else if (Distance < 800.0f)
	{
		DistanceScore = FMath::Clamp(Distance / 800.0f, 0.0f, 1.0f);
	}
	else
	{
		DistanceScore = FMath::Clamp(1.0f - (Distance - 2000.0f) / 3000.0f, 0.0f, 1.0f);
	}

	// --- Partial visibility (concealment from player POV) ---
	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(RevealTrace), true);
	TraceParams.AddIgnoredActor(MonsterActor);

	// Trace to multiple points on the monster to check partial visibility
	int32 VisiblePoints = 0;
	int32 TotalPoints = 0;
	for (int32 z = -1; z <= 1; ++z)
	{
		for (int32 x = -1; x <= 1; ++x)
		{
			FVector TestPoint = MonsterOrigin + FVector(
				MonsterExtent.X * static_cast<float>(x) * 0.7f,
				0.0f,
				MonsterExtent.Z * static_cast<float>(z) * 0.7f
			);
			TotalPoints++;

			FHitResult Hit;
			if (!World->LineTraceSingleByChannel(Hit, PlayerLoc, TestPoint, ECC_Visibility, TraceParams))
			{
				VisiblePoints++;
			}
		}
	}

	float VisibilityRatio = TotalPoints > 0 ? static_cast<float>(VisiblePoints) / static_cast<float>(TotalPoints) : 0.0f;

	// Partial visibility is best: 30-70% visible creates mystery
	float PartialScore;
	if (VisibilityRatio >= 0.3f && VisibilityRatio <= 0.7f)
	{
		PartialScore = 1.0f;
	}
	else if (VisibilityRatio < 0.3f)
	{
		PartialScore = FMath::Clamp(VisibilityRatio / 0.3f, 0.0f, 1.0f);
	}
	else
	{
		PartialScore = FMath::Clamp(1.0f - (VisibilityRatio - 0.7f) / 0.3f, 0.2f, 1.0f);
	}

	// --- Backlight potential ---
	// Trace from monster in directions away from player to find lights
	float BacklightScore = 0.0f;
	int32 BacklightsFound = 0;

	FVector MonsterToPlayer = (PlayerLoc - MonsterOrigin).GetSafeNormal();
	FVector BackDirection = -MonsterToPlayer;

	for (TActorIterator<AActor> LightIt(World); LightIt; ++LightIt)
	{
		ULightComponent* LightComp = (*LightIt)->FindComponentByClass<ULightComponent>();
		if (!LightComp || !LightComp->IsVisible())
		{
			continue;
		}

		FVector LightLoc = LightComp->GetComponentLocation();
		FVector MonsterToLight = (LightLoc - MonsterOrigin).GetSafeNormal();

		// Light is behind monster relative to player if dot with back direction > 0.3
		float DotBack = FVector::DotProduct(MonsterToLight, BackDirection);
		if (DotBack > 0.3f)
		{
			// Check line of sight to light
			FHitResult LightHit;
			if (!World->LineTraceSingleByChannel(LightHit, MonsterOrigin, LightLoc, ECC_Visibility, TraceParams))
			{
				BacklightsFound++;
				BacklightScore = FMath::Max(BacklightScore, DotBack);
			}
		}
	}

	// --- Overall reveal score ---
	float OverallScore = (
		SilhouetteScore * 0.25f +
		DistanceScore * 0.2f +
		PartialScore * 0.25f +
		AlignmentScore * 0.15f +
		BacklightScore * 0.15f
	) * 100.0f;

	// Quality tier
	FString Tier;
	if (OverallScore >= 80.0f) Tier = TEXT("Excellent");
	else if (OverallScore >= 60.0f) Tier = TEXT("Good");
	else if (OverallScore >= 40.0f) Tier = TEXT("Mediocre");
	else if (OverallScore >= 20.0f) Tier = TEXT("Poor");
	else Tier = TEXT("Bad");

	// Recommendations
	TArray<TSharedPtr<FJsonValue>> Tips;
	if (!bInFOV)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Monster is outside player FOV — no reveal happens. Reposition or adjust approach angle.")));
	}
	if (SilhouetteScore < 0.5f && ScreenCoverage < 0.1f)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Monster is too small on screen. Move closer or use a narrower corridor.")));
	}
	if (VisibilityRatio > 0.9f)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Monster is fully visible — add partial occlusion (doorframe, fog, corner) for mystery.")));
	}
	if (BacklightsFound == 0)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("No backlight detected. Place a light behind the monster for silhouette definition.")));
	}
	if (Distance < 300.0f)
	{
		Tips.Add(MakeShared<FJsonValueString>(TEXT("Too close for a reveal — this is a jumpscare distance. Pull back to 8-20m for dread.")));
	}

	auto Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("monster_actor"), MonsterName);
	Result->SetNumberField(TEXT("distance_cm"), Distance);
	Result->SetBoolField(TEXT("in_fov"), bInFOV);
	Result->SetNumberField(TEXT("visibility_ratio"), VisibilityRatio);
	Result->SetNumberField(TEXT("screen_coverage"), ScreenCoverage);
	Result->SetNumberField(TEXT("backlights_found"), BacklightsFound);

	auto ScoresObj = MakeShared<FJsonObject>();
	ScoresObj->SetNumberField(TEXT("silhouette"), FMath::RoundToFloat(SilhouetteScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("distance"), FMath::RoundToFloat(DistanceScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("partial_visibility"), FMath::RoundToFloat(PartialScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("camera_alignment"), FMath::RoundToFloat(AlignmentScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("backlight"), FMath::RoundToFloat(BacklightScore * 100.0f));
	ScoresObj->SetNumberField(TEXT("overall"), FMath::RoundToFloat(OverallScore));
	Result->SetObjectField(TEXT("scores"), ScoresObj);

	Result->SetStringField(TEXT("tier"), Tier);
	Result->SetArrayField(TEXT("recommendations"), Tips);

	return FMonolithActionResult::Success(Result);
}

// ============================================================================
// 8. analyze_co_op_balance
// ============================================================================

FMonolithActionResult FMonolithLevelDesignQualityActions::AnalyzeCoOpBalance(const TSharedPtr<FJsonObject>& Params)
{
	const TArray<TSharedPtr<FJsonValue>>* PositionsArr;
	if (!Params->TryGetArrayField(TEXT("player_positions"), PositionsArr) || PositionsArr->Num() < 2)
	{
		return FMonolithActionResult::Error(TEXT("Required: player_positions (array of at least 2 player positions [[x,y,z], ...])"));
	}

	if (PositionsArr->Num() > 8)
	{
		return FMonolithActionResult::Error(TEXT("Maximum 8 player positions supported"));
	}

	UWorld* World = MonolithMeshUtils::GetEditorWorld();
	if (!World)
	{
		return FMonolithActionResult::Error(TEXT("No editor world available"));
	}

	// Parse player positions
	TArray<FVector> Positions;
	for (const TSharedPtr<FJsonValue>& PosVal : *PositionsArr)
	{
		const TArray<TSharedPtr<FJsonValue>>* PosArr;
		if (!PosVal->TryGetArray(PosArr) || PosArr->Num() < 3)
		{
			return FMonolithActionResult::Error(TEXT("Each player position must be [x, y, z]"));
		}

		FVector Pos;
		Pos.X = (*PosArr)[0]->AsNumber();
		Pos.Y = (*PosArr)[1]->AsNumber();
		Pos.Z = (*PosArr)[2]->AsNumber();
		Positions.Add(Pos);
	}

	FCollisionQueryParams TraceParams(SCENE_QUERY_STAT(CoopTrace), true);

	// --- Pairwise analysis ---
	TArray<TSharedPtr<FJsonValue>> PairAnalysis;
	float TotalSeparation = 0.0f;
	int32 LOSCount = 0;
	int32 PairCount = 0;

	for (int32 i = 0; i < Positions.Num(); ++i)
	{
		for (int32 j = i + 1; j < Positions.Num(); ++j)
		{
			PairCount++;
			float Dist = FVector::Dist(Positions[i], Positions[j]);
			TotalSeparation += Dist;

			// Check line of sight
			FHitResult Hit;
			bool bHasLOS = !World->LineTraceSingleByChannel(Hit, Positions[i], Positions[j], ECC_Visibility, TraceParams);
			if (bHasLOS) LOSCount++;

			auto PairObj = MakeShared<FJsonObject>();
			PairObj->SetStringField(TEXT("pair"), FString::Printf(TEXT("Player%d-Player%d"), i, j));
			PairObj->SetNumberField(TEXT("distance_cm"), Dist);
			PairObj->SetBoolField(TEXT("line_of_sight"), bHasLOS);

			// Communication distance rating
			FString CommRating;
			if (Dist < 500.0f) CommRating = TEXT("close");
			else if (Dist < 1500.0f) CommRating = TEXT("comfortable");
			else if (Dist < 3000.0f) CommRating = TEXT("strained");
			else CommRating = TEXT("separated");
			PairObj->SetStringField(TEXT("communication_rating"), CommRating);

			PairAnalysis.Add(MakeShared<FJsonValueObject>(PairObj));
		}
	}

	// --- Coverage analysis: radial sweep from each player for blind spots ---
	int32 TotalDirections = 16;
	TArray<bool> CoveredDirections;
	CoveredDirections.SetNumZeroed(TotalDirections);

	FVector Centroid = FVector::ZeroVector;
	for (const FVector& Pos : Positions)
	{
		Centroid += Pos;
	}
	Centroid /= static_cast<float>(Positions.Num());

	// For each direction from centroid, check if any player covers it
	for (int32 d = 0; d < TotalDirections; ++d)
	{
		float Angle = (static_cast<float>(d) / static_cast<float>(TotalDirections)) * UE_TWO_PI;
		FVector Dir(FMath::Cos(Angle), FMath::Sin(Angle), 0.0f);
		FVector TestPoint = Centroid + Dir * 2000.0f;

		for (const FVector& Pos : Positions)
		{
			FVector PlayerToTest = (TestPoint - Pos).GetSafeNormal();
			FVector PlayerForward = (Centroid - Pos).GetSafeNormal(); // Approximate forward as toward group center

			// Player "covers" this direction if it's within 90 degrees of their facing
			if (FVector::DotProduct(PlayerToTest, (TestPoint - Pos).GetSafeNormal()) > 0.0f)
			{
				FHitResult CoverHit;
				if (!World->LineTraceSingleByChannel(CoverHit, Pos, TestPoint, ECC_Visibility, TraceParams))
				{
					CoveredDirections[d] = true;
					break;
				}
			}
		}
	}

	int32 CoveredCount = 0;
	TArray<TSharedPtr<FJsonValue>> BlindSpots;
	for (int32 d = 0; d < TotalDirections; ++d)
	{
		if (CoveredDirections[d])
		{
			CoveredCount++;
		}
		else
		{
			float Angle = (static_cast<float>(d) / static_cast<float>(TotalDirections)) * 360.0f;
			BlindSpots.Add(MakeShared<FJsonValueNumber>(Angle));
		}
	}

	float CoveragePercent = static_cast<float>(CoveredCount) / static_cast<float>(TotalDirections) * 100.0f;

	// --- Separation opportunities ---
	// Check for walls/geometry between players that could force separation
	int32 SeparationOpportunities = PairCount - LOSCount;

	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("player_count"), Positions.Num());
	Result->SetNumberField(TEXT("average_separation_cm"), PairCount > 0 ? TotalSeparation / PairCount : 0.0);
	Result->SetNumberField(TEXT("line_of_sight_pairs"), LOSCount);
	Result->SetNumberField(TEXT("total_pairs"), PairCount);
	Result->SetNumberField(TEXT("separation_opportunities"), SeparationOpportunities);
	Result->SetNumberField(TEXT("coverage_percent"), CoveragePercent);
	Result->SetArrayField(TEXT("blind_spot_angles"), BlindSpots);
	Result->SetArrayField(TEXT("pair_analysis"), PairAnalysis);
	Result->SetArrayField(TEXT("centroid"), VectorToJsonArray(Centroid));

	// Co-op suitability score
	float LOSRatio = PairCount > 0 ? static_cast<float>(LOSCount) / static_cast<float>(PairCount) : 0.0f;
	float AvgSep = PairCount > 0 ? TotalSeparation / PairCount : 0.0f;

	// Ideal: some but not all LOS, moderate separation, good coverage
	float CoopScore = 0.0f;
	CoopScore += (LOSRatio >= 0.3f && LOSRatio <= 0.7f) ? 30.0f : (LOSRatio > 0.7f ? 20.0f : 10.0f);
	CoopScore += (AvgSep >= 500.0f && AvgSep <= 2000.0f) ? 30.0f : 15.0f;
	CoopScore += CoveragePercent * 0.4f;

	Result->SetNumberField(TEXT("co_op_balance_score"), FMath::RoundToFloat(CoopScore));
	Result->SetStringField(TEXT("note"), TEXT("Basic spatial analysis; navmesh pathing, encounter zones, and dynamic difficulty are not included in this action."));

	return FMonolithActionResult::Success(Result);
}
