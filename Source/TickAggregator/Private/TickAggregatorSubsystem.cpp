// Copyright (C) Thyke. All Rights Reserved.


#include "TickAggregatorSubsystem.h"
#include "Stats/Stats.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "Engine.h"

DECLARE_CYCLE_STAT(TEXT("Tick Aggregator - Total Tick Time"), STAT_TickAggregator_Tick, STATGROUP_TickAggregator);
DECLARE_CYCLE_STAT(TEXT("Tick Aggregator - Tick Components"), STAT_TickAggregator_TickComponents, STATGROUP_TickAggregator);
DECLARE_CYCLE_STAT(TEXT("Tick Aggregator - Tick Actors"), STAT_TickAggregator_TickActors, STATGROUP_TickAggregator);

UTickAggregatorSubsystem::UTickAggregatorSubsystem()
{
    TArray<ETickingGroup> AllGroups = {
    TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics,
    TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable
    };

    for (ETickingGroup Group : AllGroups)
    {
        GroupedComponents.Add(Group, TArray<TWeakObjectPtr<UActorComponent>>());
        GroupedActors.Add(Group, TArray<TWeakObjectPtr<AActor>>());
    }
}

void UTickAggregatorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    UE_LOG(LogTemp, Warning, TEXT("GroupedWorldSubsystem Initialized"));
    ShowDebug(true);

}

void UTickAggregatorSubsystem::Deinitialize()
{
    Super::Deinitialize();
    UE_LOG(LogTemp, Warning, TEXT("GroupedWorldSubsystem Deinitialized"));
}

void UTickAggregatorSubsystem::Tick(float DeltaTime)
{
    SCOPE_CYCLE_COUNTER(STAT_TickAggregator_Tick);
    UE_LOG(LogTemp, Warning, TEXT("GroupedWorldSubsystem Ticking"));

    for (const auto& Pair : GroupedComponents)
    {
        UE_LOG(LogTemp, Warning, TEXT("TickGroup %s has %d components"), *TickGroupToString(Pair.Key), Pair.Value.Num());
    }

    for (const auto& Pair : GroupedActors)
    {
        UE_LOG(LogTemp, Warning, TEXT("TickGroup %s has %d actors"), *TickGroupToString(Pair.Key), Pair.Value.Num());
    }
    FScopeLock Lock(&GroupedObjectsCriticalSection);

    TArray<ETickingGroup> TickOrder = {
        TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics,
        TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable
    };

    for (ETickingGroup Group : TickOrder)
    {
        TickGroupObjects(Group, DeltaTime);
    }
}

TStatId UTickAggregatorSubsystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UTickAggregatorSubsystem, STATGROUP_TickAggregator);
}

void UTickAggregatorSubsystem::TickGroupObjects(ETickingGroup Group, float DeltaTime)
{
    FString GroupName = TickGroupToString(Group);
    UE_LOG(LogTemp, Warning, TEXT("Ticking group: %s"), *GroupName);

    // Tick components
    if (auto* Components = GroupedComponents.Find(Group))
    {
        UE_LOG(LogTemp, Warning, TEXT("Ticking %d components in group %s"), Components->Num(), *GroupName);
        for (auto CompIt = Components->CreateIterator(); CompIt; ++CompIt)
        {
            if (UActorComponent* Component = CompIt->Get())
            {
                UE_LOG(LogTemp, Verbose, TEXT("Ticking component: %s"), *Component->GetName());
                Component->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
            }
        }
    }

    // Tick actors
    if (auto* Actors = GroupedActors.Find(Group))
    {
        UE_LOG(LogTemp, Warning, TEXT("Ticking %d actors in group %s"), Actors->Num(), *GroupName);
        for (auto ActorIt = Actors->CreateIterator(); ActorIt; ++ActorIt)
        {
            if (AActor* Actor = ActorIt->Get())
            {
                UE_LOG(LogTemp, Verbose, TEXT("Ticking actor: %s"), *Actor->GetName());
                Actor->Tick(DeltaTime);
            }
        }
    }
    // Visual debug message
    if (GEngine && bDebug)
    {
        GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Yellow,
            FString::Printf(TEXT("Ticked Group: %s"), *GroupName));
    }
}

void UTickAggregatorSubsystem::RegisterTickableComponent(UActorComponent* Component)
{
    if (IsValid(Component))
    {
        ETickingGroup TickGroup = GetComponentTickGroup(Component);
        FScopeLock Lock(&GroupedObjectsCriticalSection);
        GroupedComponents.FindOrAdd(TickGroup).AddUnique(Component);
        Component->PrimaryComponentTick.bCanEverTick = false;  // Disable component's own tick
        Component->SetActive(true);
        UE_LOG(LogTemp, Log, TEXT("Registered Component: %s in TickGroup: %s"),
            *Component->GetName(), *TickGroupToString(TickGroup));
    }
}

void UTickAggregatorSubsystem::UnregisterTickableComponent(UActorComponent* Component)
{
    if (IsValid(Component))
    {
        FScopeLock Lock(&GroupedObjectsCriticalSection);
        for (auto& Pair : GroupedComponents)
        {
            Pair.Value.Remove(Component);
        }
        Component->PrimaryComponentTick.bCanEverTick = true;  // Re-enable component's own tick
        UE_LOG(LogTemp, Log, TEXT("Unregistered Component: %s"), *Component->GetName());
    }
}

void UTickAggregatorSubsystem::RegisterTickableActor(AActor* Actor)
{
    if (IsValid(Actor))
    {
        ETickingGroup TickGroup = GetActorTickGroup(Actor);
        FScopeLock Lock(&GroupedObjectsCriticalSection);
        GroupedActors.FindOrAdd(TickGroup).AddUnique(Actor);
        Actor->SetActorTickEnabled(false);  // Disable actor's own tick
        UE_LOG(LogTemp, Log, TEXT("Registered Actor: %s in TickGroup: %s"),
            *Actor->GetName(), *TickGroupToString(TickGroup));
    }
}

void UTickAggregatorSubsystem::UnregisterTickableActor(AActor* Actor)
{
    if (IsValid(Actor))
    {
        FScopeLock Lock(&GroupedObjectsCriticalSection);
        for (auto& Pair : GroupedActors)
        {
            Pair.Value.Remove(Actor);
        }
        Actor->SetActorTickEnabled(true);  // Re-enable actor's own tick
        UE_LOG(LogTemp, Log, TEXT("Unregistered Actor: %s"), *Actor->GetName());
    }
}

void UTickAggregatorSubsystem::ShowDebug(bool Debug)
{
    bDebug = Debug;
}

ETickingGroup UTickAggregatorSubsystem::GetComponentTickGroup(UActorComponent* Component)
{
    if (IsValid(Component))
    {
        return static_cast<ETickingGroup>(Component->PrimaryComponentTick.TickGroup);
    }
    return TG_PrePhysics;
}

ETickingGroup UTickAggregatorSubsystem::GetActorTickGroup(AActor* Actor)
{
    if (IsValid(Actor) && Actor->GetRootComponent())
    {
        return Actor->PrimaryActorTick.TickGroup;
    }
    return TG_PrePhysics;
}

FString UTickAggregatorSubsystem::TickGroupToString(ETickingGroup TickGroup)
{
    switch (TickGroup)
    {
    case ETickingGroup::TG_PrePhysics: return TEXT("PrePhysics");
    case ETickingGroup::TG_StartPhysics: return TEXT("StartPhysics");
    case ETickingGroup::TG_DuringPhysics: return TEXT("DuringPhysics");
    case ETickingGroup::TG_EndPhysics: return TEXT("EndPhysics");
    case ETickingGroup::TG_PostPhysics: return TEXT("PostPhysics");
    case ETickingGroup::TG_PostUpdateWork: return TEXT("PostUpdateWork");
    case ETickingGroup::TG_LastDemotable: return TEXT("LastDemotable");
    default: return TEXT("Unknown");
    }
}


void UTickAggregatorSubsystem::RegisterTickableActors(const TArray<AActor*>& Actors)
{
    FScopeLock Lock(&GroupedObjectsCriticalSection);
    for (AActor* Actor : Actors)
    {
        if (IsValid(Actor))
        {
            ETickingGroup TickGroup = GetActorTickGroup(Actor);
            GroupedActors.FindOrAdd(TickGroup).AddUnique(Actor);
            Actor->SetActorTickEnabled(false);  // Disable actor's own tick
            UE_LOG(LogTemp, Log, TEXT("Registered Actor: %s in TickGroup: %s"),
                *Actor->GetName(), *TickGroupToString(TickGroup));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Attempted to register invalid Actor"));
        }
    }
}

void UTickAggregatorSubsystem::RegisterTickableComponents(const TArray<UActorComponent*>& Components)
{
    FScopeLock Lock(&GroupedObjectsCriticalSection);
    for (UActorComponent* Component : Components)
    {
        if (IsValid(Component))
        {
            ETickingGroup TickGroup = GetComponentTickGroup(Component);
            GroupedComponents.FindOrAdd(TickGroup).AddUnique(Component);
            Component->PrimaryComponentTick.bCanEverTick = false;  // Disable component's own tick
            Component->SetActive(true);
            UE_LOG(LogTemp, Log, TEXT("Registered Component: %s in TickGroup: %s"),
                *Component->GetName(), *TickGroupToString(TickGroup));
        }
        else
        {
            UE_LOG(LogTemp, Warning, TEXT("Attempted to register invalid Component"));
        }
    }
}