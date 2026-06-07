#include "MonolithAnimationRuntimeActions.h"
#include "MonolithParamSchema.h"

#include "Editor.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimClassInterface.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"

DEFINE_LOG_CATEGORY_STATIC(LogMonolithAnimRuntime, Log, All);

namespace
{
	// ── PIE actor + skeletal mesh + anim instance resolution ────────────
	struct FAnimPIELookup
	{
		AActor* Actor = nullptr;
		USkeletalMeshComponent* MeshComp = nullptr;
		UAnimInstance* AnimInstance = nullptr;
		FMonolithActionResult Error;
		bool bSuccess = false;
	};

	FAnimPIELookup FindAnimInstanceInPIE(const TSharedPtr<FJsonObject>& Params)
	{
		FAnimPIELookup Result;

		FString ActorName = Params->GetStringField(TEXT("actor"));
		if (ActorName.IsEmpty())
		{
			Result.Error = FMonolithActionResult::Error(TEXT("Missing required param 'actor'"));
			return Result;
		}

		FString ComponentName;
		if (Params->HasField(TEXT("component_name")))
		{
			ComponentName = Params->GetStringField(TEXT("component_name"));
		}

		FWorldContext* PIEContext = GEditor ? GEditor->GetPIEWorldContext() : nullptr;
		if (!PIEContext || !PIEContext->World())
		{
			Result.Error = FMonolithActionResult::Error(TEXT("PIE not running — start Play-In-Editor first"));
			return Result;
		}
		UWorld* PIEWorld = PIEContext->World();

		for (TActorIterator<AActor> It(PIEWorld); It; ++It)
		{
			if (It->GetActorLabel() == ActorName || It->GetName() == ActorName)
			{
				Result.Actor = *It;
				break;
			}
		}
		if (!Result.Actor)
		{
			Result.Error = FMonolithActionResult::Error(FString::Printf(TEXT("Actor '%s' not found in PIE world"), *ActorName));
			return Result;
		}

		for (UActorComponent* C : Result.Actor->GetComponents())
		{
			USkeletalMeshComponent* MC = Cast<USkeletalMeshComponent>(C);
			if (!MC) continue;
			if (ComponentName.IsEmpty() || MC->GetName() == ComponentName)
			{
				Result.MeshComp = MC;
				break;
			}
		}
		if (!Result.MeshComp)
		{
			Result.Error = FMonolithActionResult::Error(FString::Printf(
				TEXT("No skeletal mesh component%s found on actor '%s'"),
				ComponentName.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" named '%s'"), *ComponentName),
				*ActorName));
			return Result;
		}

		Result.AnimInstance = Result.MeshComp->GetAnimInstance();
		Result.bSuccess = true;
		return Result;
	}

	FString AnimationModeToString(EAnimationMode::Type Mode)
	{
		switch (Mode)
		{
			case EAnimationMode::AnimationBlueprint: return TEXT("AnimationBlueprint");
			case EAnimationMode::AnimationSingleNode: return TEXT("AnimationSingleNode");
			case EAnimationMode::AnimationCustomMode: return TEXT("AnimationCustomMode");
			default: return TEXT("Unknown");
		}
	}

	// Read a single live UPROPERTY value off an object into a JSON value.
	// Handles the common scalar types directly; falls back to ExportText for
	// structs/enums/anything else.
	TSharedPtr<FJsonValue> ReadPropertyValue(UObject* Obj, const FString& VarName, FString& OutTypeName)
	{
		if (!Obj) return nullptr;
		FProperty* Prop = Obj->GetClass()->FindPropertyByName(FName(*VarName));
		if (!Prop)
		{
			OutTypeName = TEXT("<not found>");
			return nullptr;
		}
		OutTypeName = Prop->GetCPPType(nullptr, 0u);
		const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Obj);

		if (const FBoolProperty* BoolProp = CastField<FBoolProperty>(Prop))
		{
			return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
		}
		if (const FFloatProperty* FloatProp = CastField<FFloatProperty>(Prop))
		{
			return MakeShared<FJsonValueNumber>(FloatProp->GetPropertyValue(ValuePtr));
		}
		if (const FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Prop))
		{
			return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue(ValuePtr));
		}
		if (const FIntProperty* IntProp = CastField<FIntProperty>(Prop))
		{
			return MakeShared<FJsonValueNumber>(IntProp->GetPropertyValue(ValuePtr));
		}
		if (const FStrProperty* StrProp = CastField<FStrProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
		}
		if (const FNameProperty* NameProp = CastField<FNameProperty>(Prop))
		{
			return MakeShared<FJsonValueString>(NameProp->GetPropertyValue(ValuePtr).ToString());
		}
		if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
		{
			UObject* Val = ObjProp->GetObjectPropertyValue_InContainer(Obj);
			return MakeShared<FJsonValueString>(Val ? Val->GetPathName() : TEXT("None"));
		}

		// Generic fallback (enums, structs, vectors, etc.)
		FString Exported;
		Prop->ExportTextItem_Direct(Exported, ValuePtr, nullptr, Obj, PPF_None);
		return MakeShared<FJsonValueString>(Exported);
	}
}

// ── Registration ────────────────────────────────────────────────────

void FMonolithAnimationRuntimeActions::RegisterActions(FMonolithToolRegistry& Registry)
{
	Registry.RegisterAction(TEXT("animation"), TEXT("sample_pie_anim_instance"),
		TEXT("Sample a live PIE actor's animation state: active AnimInstance/AnimClass, animation mode, active state-machine states, active montage, requested anim-instance variables, and requested bone/socket transforms"),
		FMonolithActionHandler::CreateStatic(&HandleSamplePIEAnimInstance),
		FParamSchemaBuilder()
			.Required(TEXT("actor"), TEXT("string"), TEXT("Actor label or name in the PIE world"))
			.Optional(TEXT("component_name"), TEXT("string"), TEXT("Skeletal mesh component name (if the actor has multiple)"))
			.Optional(TEXT("variables"), TEXT("array"), TEXT("Anim-instance variable names to read live via reflection"))
			.Optional(TEXT("bones"), TEXT("array"), TEXT("Bone names (strings) to report world-space transforms for"))
			.Optional(TEXT("sockets"), TEXT("array"), TEXT("Socket names (strings) to report world-space transforms for"))
			.Optional(TEXT("state_machines"), TEXT("array"), TEXT("State machine names to report active state for. If omitted, all baked state machines are enumerated"))
			.Build());

	UE_LOG(LogMonolithAnimRuntime, Log, TEXT("MonolithAnimation Runtime: registered 1 action"));
}

// ── sample_pie_anim_instance ────────────────────────────────────────

FMonolithActionResult FMonolithAnimationRuntimeActions::HandleSamplePIEAnimInstance(const TSharedPtr<FJsonObject>& Params)
{
	FAnimPIELookup Lookup = FindAnimInstanceInPIE(Params);
	if (!Lookup.bSuccess) return Lookup.Error;

	USkeletalMeshComponent* MeshComp = Lookup.MeshComp;
	UAnimInstance* AnimInstance = Lookup.AnimInstance;

	TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("actor"), Lookup.Actor->GetActorLabel());
	Root->SetStringField(TEXT("component"), MeshComp->GetName());

	// Animation mode + class info.
	Root->SetStringField(TEXT("animation_mode"), AnimationModeToString(MeshComp->GetAnimationMode()));
	if (UClass* AnimClass = MeshComp->GetAnimClass())
	{
		Root->SetStringField(TEXT("mesh_anim_class"), AnimClass->GetPathName());
	}
	if (USkeletalMesh* SkelMesh = MeshComp->GetSkeletalMeshAsset())
	{
		Root->SetStringField(TEXT("skeletal_mesh"), SkelMesh->GetPathName());
	}

	if (!AnimInstance)
	{
		Root->SetStringField(TEXT("note"), TEXT("No active AnimInstance on this component (animation mode may not be AnimationBlueprint)"));
		return FMonolithActionResult::Success(Root);
	}
	Root->SetStringField(TEXT("anim_instance_class"), AnimInstance->GetClass()->GetPathName());

	// ── Active state-machine state(s) ────────────────────────────────
	TArray<TSharedPtr<FJsonValue>> StateMachinesArr;
	TArray<FName> MachineNames;

	const TArray<TSharedPtr<FJsonValue>>* RequestedSMs = nullptr;
	if (Params->TryGetArrayField(TEXT("state_machines"), RequestedSMs) && RequestedSMs)
	{
		for (const TSharedPtr<FJsonValue>& V : *RequestedSMs)
		{
			FString Name;
			if (V.IsValid() && V->TryGetString(Name) && !Name.IsEmpty())
			{
				MachineNames.Add(FName(*Name));
			}
		}
	}
	else
	{
		// Enumerate all baked state machines from the generated anim class.
		if (IAnimClassInterface* AnimClassInterface = IAnimClassInterface::GetFromClass(AnimInstance->GetClass()))
		{
			for (const FBakedAnimationStateMachine& Baked : AnimClassInterface->GetBakedStateMachines())
			{
				MachineNames.Add(Baked.MachineName);
			}
		}
	}

	for (const FName& MachineName : MachineNames)
	{
		const int32 MachineIndex = AnimInstance->GetStateMachineIndex(MachineName);
		TSharedPtr<FJsonObject> SMObj = MakeShared<FJsonObject>();
		SMObj->SetStringField(TEXT("machine_name"), MachineName.ToString());
		SMObj->SetNumberField(TEXT("machine_index"), MachineIndex);
		if (MachineIndex != INDEX_NONE)
		{
			SMObj->SetStringField(TEXT("active_state"), AnimInstance->GetCurrentStateName(MachineIndex).ToString());
			SMObj->SetNumberField(TEXT("machine_weight"), AnimInstance->GetInstanceMachineWeight(MachineIndex));
		}
		else
		{
			SMObj->SetStringField(TEXT("note"), TEXT("machine name not found on this anim instance"));
		}
		StateMachinesArr.Add(MakeShared<FJsonValueObject>(SMObj));
	}
	Root->SetArrayField(TEXT("state_machines"), StateMachinesArr);

	// ── Active montage ───────────────────────────────────────────────
	if (UAnimMontage* ActiveMontage = AnimInstance->GetCurrentActiveMontage())
	{
		TSharedPtr<FJsonObject> MontageObj = MakeShared<FJsonObject>();
		MontageObj->SetStringField(TEXT("montage"), ActiveMontage->GetPathName());
		MontageObj->SetBoolField(TEXT("is_playing"), AnimInstance->Montage_IsPlaying(ActiveMontage));
		MontageObj->SetStringField(TEXT("current_section"), AnimInstance->Montage_GetCurrentSection(ActiveMontage).ToString());
		Root->SetObjectField(TEXT("active_montage"), MontageObj);
	}
	else
	{
		Root->SetField(TEXT("active_montage"), MakeShared<FJsonValueNull>());
	}

	// ── Requested anim-instance variables (live reflection) ──────────
	const TArray<TSharedPtr<FJsonValue>>* RequestedVars = nullptr;
	if (Params->TryGetArrayField(TEXT("variables"), RequestedVars) && RequestedVars)
	{
		TSharedPtr<FJsonObject> VarsObj = MakeShared<FJsonObject>();
		for (const TSharedPtr<FJsonValue>& V : *RequestedVars)
		{
			FString VarName;
			if (!V.IsValid() || !V->TryGetString(VarName) || VarName.IsEmpty()) continue;
			FString TypeName;
			TSharedPtr<FJsonValue> Val = ReadPropertyValue(AnimInstance, VarName, TypeName);
			if (Val.IsValid())
			{
				VarsObj->SetField(VarName, Val);
			}
			else
			{
				VarsObj->SetStringField(VarName, FString::Printf(TEXT("<%s>"), *TypeName));
			}
		}
		Root->SetObjectField(TEXT("variables"), VarsObj);
	}

	// ── Bone transforms (resolve name -> index, then world transform) ─
	const TArray<TSharedPtr<FJsonValue>>* RequestedBones = nullptr;
	if (Params->TryGetArrayField(TEXT("bones"), RequestedBones) && RequestedBones)
	{
		TArray<TSharedPtr<FJsonValue>> BonesArr;
		for (const TSharedPtr<FJsonValue>& V : *RequestedBones)
		{
			FString BoneName;
			if (!V.IsValid() || !V->TryGetString(BoneName) || BoneName.IsEmpty()) continue;

			TSharedPtr<FJsonObject> BoneObj = MakeShared<FJsonObject>();
			BoneObj->SetStringField(TEXT("bone"), BoneName);

			// CRITICAL: resolve the bone NAME to an index before GetBoneTransform(index).
			const int32 BoneIndex = MeshComp->GetBoneIndex(FName(*BoneName));
			if (BoneIndex == INDEX_NONE)
			{
				BoneObj->SetStringField(TEXT("error"), TEXT("bone not found on skeleton"));
				BonesArr.Add(MakeShared<FJsonValueObject>(BoneObj));
				continue;
			}
			BoneObj->SetNumberField(TEXT("index"), BoneIndex);

			const FTransform XForm = MeshComp->GetBoneTransform(BoneIndex);
			const FVector Loc = XForm.GetLocation();
			const FRotator Rot = XForm.Rotator();
			const FVector Scale = XForm.GetScale3D();
			TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), Loc.X); LocObj->SetNumberField(TEXT("y"), Loc.Y); LocObj->SetNumberField(TEXT("z"), Loc.Z);
			TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
			RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch); RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw); RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
			TSharedPtr<FJsonObject> ScaleObj = MakeShared<FJsonObject>();
			ScaleObj->SetNumberField(TEXT("x"), Scale.X); ScaleObj->SetNumberField(TEXT("y"), Scale.Y); ScaleObj->SetNumberField(TEXT("z"), Scale.Z);
			BoneObj->SetObjectField(TEXT("location"), LocObj);
			BoneObj->SetObjectField(TEXT("rotation"), RotObj);
			BoneObj->SetObjectField(TEXT("scale"), ScaleObj);
			BoneObj->SetStringField(TEXT("space"), TEXT("world"));
			BonesArr.Add(MakeShared<FJsonValueObject>(BoneObj));
		}
		Root->SetArrayField(TEXT("bones"), BonesArr);
	}

	// ── Socket transforms ────────────────────────────────────────────
	const TArray<TSharedPtr<FJsonValue>>* RequestedSockets = nullptr;
	if (Params->TryGetArrayField(TEXT("sockets"), RequestedSockets) && RequestedSockets)
	{
		TArray<TSharedPtr<FJsonValue>> SocketsArr;
		for (const TSharedPtr<FJsonValue>& V : *RequestedSockets)
		{
			FString SocketName;
			if (!V.IsValid() || !V->TryGetString(SocketName) || SocketName.IsEmpty()) continue;

			TSharedPtr<FJsonObject> SockObj = MakeShared<FJsonObject>();
			SockObj->SetStringField(TEXT("socket"), SocketName);
			if (!MeshComp->DoesSocketExist(FName(*SocketName)))
			{
				SockObj->SetStringField(TEXT("error"), TEXT("socket not found"));
				SocketsArr.Add(MakeShared<FJsonValueObject>(SockObj));
				continue;
			}

			const FTransform XForm = MeshComp->GetSocketTransform(FName(*SocketName), RTS_World);
			const FVector Loc = XForm.GetLocation();
			const FRotator Rot = XForm.Rotator();
			TSharedPtr<FJsonObject> LocObj = MakeShared<FJsonObject>();
			LocObj->SetNumberField(TEXT("x"), Loc.X); LocObj->SetNumberField(TEXT("y"), Loc.Y); LocObj->SetNumberField(TEXT("z"), Loc.Z);
			TSharedPtr<FJsonObject> RotObj = MakeShared<FJsonObject>();
			RotObj->SetNumberField(TEXT("pitch"), Rot.Pitch); RotObj->SetNumberField(TEXT("yaw"), Rot.Yaw); RotObj->SetNumberField(TEXT("roll"), Rot.Roll);
			SockObj->SetObjectField(TEXT("location"), LocObj);
			SockObj->SetObjectField(TEXT("rotation"), RotObj);
			SockObj->SetStringField(TEXT("space"), TEXT("world"));
			SocketsArr.Add(MakeShared<FJsonValueObject>(SockObj));
		}
		Root->SetArrayField(TEXT("sockets"), SocketsArr);
	}

	// Asset-player weight note: direct per-asset-player weight getters require a
	// node index (GetInstanceAssetPlayerTime etc. are time-only). State/machine
	// weights ARE reachable via GetInstanceStateWeight / GetInstanceMachineWeight
	// (machine weight reported above). Full per-node weight walk deferred this pass.
	Root->SetStringField(TEXT("asset_player_weight_note"),
		TEXT("Per-asset-player blend weights have no direct public getter; state-machine weights are reported under state_machines[].machine_weight. Full node-graph weight walk deferred."));

	return FMonolithActionResult::Success(Root);
}
