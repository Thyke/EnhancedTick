// Copyright (C) Thyke. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "Stats/Stats.h"
#include "TickAggregatorSubsystem.generated.h"

DECLARE_STATS_GROUP(TEXT("TickAggregator"), STATGROUP_TickAggregator, STATCAT_Advanced);

UCLASS()
class TICKAGGREGATOR_API UTickAggregatorSubsystem : public UWorldSubsystem, public FTickableGameObject
{
	GENERATED_BODY()
	
public:
	UTickAggregatorSubsystem();
	// USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject interface
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	virtual bool IsTickableWhenPaused() const override { return false; }
	virtual bool IsTickableInEditor() const override { return false; }

	// Registration functions
	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void RegisterTickableComponent(UActorComponent* Component);

	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void UnregisterTickableComponent(UActorComponent* Component);

	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void RegisterTickableActor(AActor* Actor);

	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void UnregisterTickableActor(AActor* Actor);

	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void RegisterTickableActors(const TArray<AActor*>& Actors);

	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void RegisterTickableComponents(const TArray<UActorComponent*>& Components);

	UFUNCTION(BlueprintCallable, Category = "TickAggregator")
	void ShowDebug(bool Debug);

private:
	TMap<ETickingGroup, TArray<TWeakObjectPtr<UActorComponent>>> GroupedComponents;
	TMap<ETickingGroup, TArray<TWeakObjectPtr<AActor>>> GroupedActors;

	FCriticalSection GroupedObjectsCriticalSection;

	void TickGroupObjects(ETickingGroup Group, float DeltaTime);
	ETickingGroup GetComponentTickGroup(UActorComponent* Component);
	ETickingGroup GetActorTickGroup(AActor* Actor);
	FString TickGroupToString(ETickingGroup TickGroup);

	bool bDebug;
};
