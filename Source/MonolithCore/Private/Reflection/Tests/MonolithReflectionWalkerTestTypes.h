// SPDX-License-Identifier: MIT
// Test-only reflected types for FMonolithReflectionWalker automation.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture2D.h"
#include "GameFramework/Actor.h"
#include "MonolithReflectionWalkerTestTypes.generated.h"

UENUM()
enum class EMonolithReflectionWalkerTestEnum : uint8
{
	Light,
	Heavy
};

USTRUCT()
struct FMonolithReflectionWalkerNestedTestStruct
{
	GENERATED_BODY()

	UPROPERTY()
	int32 NestedCount = 0;

	UPROPERTY()
	FString NestedLabel;

	UPROPERTY()
	TSubclassOf<AActor> HardActorClass;

	UPROPERTY()
	TSoftClassPtr<AActor> SoftActorClass;
};

UCLASS()
class UMonolithReflectionWalkerTestObject : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 IntValue = 0;

	UPROPERTY()
	float FloatValue = 0.0f;

	UPROPERTY()
	FName NameValue;

	UPROPERTY()
	FString StringValue;

	UPROPERTY()
	FVector VectorValue = FVector::ZeroVector;

	UPROPERTY()
	TArray<int32> IntArray;

	UPROPERTY()
	TMap<FName, FString> NameMap;

	UPROPERTY()
	TSet<FName> NameSet;

	UPROPERTY()
	TSoftObjectPtr<UTexture2D> SoftTexture;

	UPROPERTY()
	EMonolithReflectionWalkerTestEnum EnumValue = EMonolithReflectionWalkerTestEnum::Light;

	UPROPERTY()
	FMonolithReflectionWalkerNestedTestStruct Nested;

	UPROPERTY()
	TSubclassOf<AActor> HardActorClass;

	UPROPERTY()
	TSoftClassPtr<AActor> SoftActorClass;

	UPROPERTY()
	TArray<TSubclassOf<AActor>> HardActorClassArray;

	UPROPERTY()
	TArray<TSoftClassPtr<AActor>> SoftActorClassArray;

	UPROPERTY()
	TMap<FName, TSubclassOf<AActor>> HardActorClassMap;

	UPROPERTY()
	TMap<FName, TSoftClassPtr<AActor>> SoftActorClassMap;

	UPROPERTY()
	TSet<TSubclassOf<AActor>> HardActorClassSet;

	UPROPERTY()
	TSet<TSoftClassPtr<AActor>> SoftActorClassSet;
};
