// Copyright (C) Thyke 2025 All Rights Reserved.

#include "EnhancedTickSystem.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Engine/Engine.h"
#include "DrawDebugHelpers.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "EngineUtils.h" // For TActorIterator
#include "HAL/ThreadManager.h"
#include "HAL/LowLevelMemTracker.h"

// Helper for prefetching data on platforms that support prefetching
#if !UE_BUILD_SHIPPING && !UE_BUILD_TEST
#define ENHANCED_TICK_PREFETCH_DATA(Ptr) FPlatformMisc::Prefetch(Ptr)
#else
#define ENHANCED_TICK_PREFETCH_DATA(Ptr)
#endif

// L1 and L2 cache line size (usually 64 bytes)
#define CACHE_LINE_SIZE 64

// Definition of statistic variables - these were declared as extern in the header
DEFINE_STAT(STAT_EnhancedTick_Total);
DEFINE_STAT(STAT_EnhancedTick_TypeBatches);
DEFINE_STAT(STAT_EnhancedTick_SpatialBatches);
DEFINE_STAT(STAT_EnhancedTick_CacheMisses);

// Statistic definitions should not be repeated here if they were declared with DECLARE_STAT in the header.
// STAT definitions must be done only once.

//////////////////////////////////////////////////////////////////////////
// FComponentTypeBatch Implementation

void FComponentTypeBatch::TickBatch(float DeltaTime)
{
    LastFrameTickCount = 0;
    
    if (TickEntities.Num() == 0 || !BatchTickFunction)
    {
        return;
    }
    
    // Sort for optimized tick processing if cache locality sorting is enabled
    if (bSortByCacheLocality)
    {
        SortForCacheLocality();
    }
    
    // For low priority batches when FPS is below a certain threshold,
    // tick not every frame but once every few frames.
    if (EnumHasAnyFlags(Flags, ETickBatchFlags::LowPrio) && FApp::GetCurrentTime() < 30.0f)
    {
        // Skip low priority batches under low FPS conditions
        return;
    }
    
    // Filter active entities
    TArray<FTickEntityData> ActiveEntities;
    ActiveEntities.Reserve(TickEntities.Num());
    
    for (auto& Entity : TickEntities)
    {
        if (Entity.bEnabled && IsValid(Entity.Object))
        {
            ActiveEntities.Add(Entity);
        }
    }
    
    if (ActiveEntities.Num() == 0)
    {
        return;
    }
    
    // Call the BatchTickFunction for all active objects
    const double StartTime = FPlatformTime::Seconds();
    
    // Optimize cache usage using prefetch (load the first entity)
    if (ActiveEntities.Num() > 0)
    {
        ENHANCED_TICK_PREFETCH_DATA(ActiveEntities[0].Object);
    }
    
    // Lock check - now using a shared pointer for the lock
    if (BatchLock.IsValid())
    {
        FScopeLock Lock(BatchLock.Get());
        BatchTickFunction(ActiveEntities, DeltaTime);
    }
    else
    {
        BatchTickFunction(ActiveEntities, DeltaTime);
    }
    
    // Update statistics
    const double EndTime = FPlatformTime::Seconds();
    AverageTickTimeNs = float((EndTime - StartTime) * 1.0e9) / ActiveEntities.Num();
    LastFrameTickCount = ActiveEntities.Num();
}


void FComponentTypeBatch::TickBatchParallel(float DeltaTime)
{
    LastFrameTickCount = 0;
    
    if (TickEntities.Num() == 0 || !BatchTickFunction || !CanTickInParallel())
    {
        TickBatch(DeltaTime);
        return;
    }
    
    // Sort entities if necessary
    if (bSortByCacheLocality)
    {
        SortForCacheLocality();
    }
    
    // Filter active entities (preparing for parallel processing)
    TArray<FTickEntityData> ActiveEntities;
    ActiveEntities.Reserve(TickEntities.Num());
    
    // Lock check - using shared pointer for the lock
    if (BatchLock.IsValid())
    {
        FScopeLock Lock(BatchLock.Get());
        for (auto& Entity : TickEntities)
        {
            if (Entity.bEnabled && IsValid(Entity.Object))
            {
                ActiveEntities.Add(Entity);
            }
        }
    }
    else
    {
        for (auto& Entity : TickEntities)
        {
            if (Entity.bEnabled && IsValid(Entity.Object))
            {
                ActiveEntities.Add(Entity);
            }
        }
    }
    
    if (ActiveEntities.Num() == 0)
    {
        return;
    }
    
    // IMPORTANT CHECK FOR THREAD SAFETY:
    // Check for components (UPrimitiveComponent, USceneComponent, UCharacterMovementComponent)
    // that may include transform updates and are not thread-safe.
    bool bMightContainUnsafeComponents = false;
    
    for (const auto& Entity : ActiveEntities)
    {
        UObject* Object = Entity.Object;
        if (Object && (Object->IsA<UPrimitiveComponent>() || 
                       Object->IsA<USceneComponent>() || 
                       Object->IsA<UCharacterMovementComponent>()))
        {
            bMightContainUnsafeComponents = true;
            break;
        }
    }
    
    // Use the standard tick for thread-unsafe components
    if (bMightContainUnsafeComponents)
    {
        TickBatch(DeltaTime);
        return;
    }
    
    const double StartTime = FPlatformTime::Seconds();
    LastFrameTickCount = ActiveEntities.Num();
    
    // Determine the number of worker threads available
    const int32 NumThreads = FPlatformMisc::NumberOfWorkerThreadsToSpawn();
    const int32 EntitiesPerThread = FMath::Max(1, FMath::CeilToInt(float(ActiveEntities.Num()) / NumThreads));
    
    // Use TaskGraph for parallel processing
    FGraphEventArray Tasks;
    Tasks.Reserve(NumThreads);
    
    for (int32 ThreadIdx = 0; ThreadIdx < NumThreads; ++ThreadIdx)
    {
        const int32 StartIdx = ThreadIdx * EntitiesPerThread;
        const int32 EndIdx = FMath::Min(StartIdx + EntitiesPerThread, ActiveEntities.Num());
        
        if (StartIdx >= EndIdx)
        {
            continue;
        }
        
        // Create a task for each thread
        Tasks.Add(FFunctionGraphTask::CreateAndDispatchWhenReady([this, &ActiveEntities, StartIdx, EndIdx, DeltaTime]()
        {
            // Tick the entities assigned to this thread
            TArrayView<FTickEntityData> ThreadView(&ActiveEntities[StartIdx], EndIdx - StartIdx);
            
            // Optimize sub-tasks with prefetch
            for (int32 i = 0; i < ThreadView.Num(); ++i)
            {
                // Prefetch the next entity to increase cache hit rate
                if (i + 1 < ThreadView.Num())
                {
                    ENHANCED_TICK_PREFETCH_DATA(ThreadView[i + 1].Object);
                }
                
                // Tick a single object
                if (ThreadView[i].TickFunction)
                {
                    ThreadView[i].TickFunction(DeltaTime);
                }
            }
        }, TStatId(), nullptr, ENamedThreads::AnyThread));
    }
    
    // Wait for all tasks to complete
    FTaskGraphInterface::Get().WaitUntilTasksComplete(Tasks);
    
    // Update statistics
    const double EndTime = FPlatformTime::Seconds();
    AverageTickTimeNs = float((EndTime - StartTime) * 1.0e9) / ActiveEntities.Num();
}

void FComponentTypeBatch::SortForCacheLocality()
{
    if (TickEntities.Num() < 2)
    {
        return;
    }
    
    // Sorting strategy for cache locality
    // More advanced spatial sorting such as Hilbert curves could be used here
    
    // Simple distance-based greedy sorting
    TArray<FTickEntityData> SortedEntities;
    SortedEntities.Reserve(TickEntities.Num());
    
    // Filter out inactive or invalid objects
    TArray<FTickEntityData> ValidEntities;
    for (auto& Entity : TickEntities)
    {
        if (Entity.bEnabled && IsValid(Entity.Object))
        {
            ValidEntities.Add(Entity);
        }
    }
    
    if (ValidEntities.Num() < 2)
    {
        return;
    }
    
    // Start by adding the first object
    SortedEntities.Add(ValidEntities[0]);
    ValidEntities.RemoveAt(0);
    
    // Iteratively find the nearest next object
    while (ValidEntities.Num() > 0)
    {
        const FTickEntityData& LastAdded = SortedEntities.Last();
        int32 BestIdx = 0;
        float BestDistance = LastAdded.GetDistance(ValidEntities[0]);
        
        // Find the closest neighbor
        for (int32 i = 1; i < ValidEntities.Num(); ++i)
        {
            const float Dist = LastAdded.GetDistance(ValidEntities[i]);
            if (Dist < BestDistance)
            {
                BestDistance = Dist;
                BestIdx = i;
            }
        }
        
        // Add the closest neighbor
        SortedEntities.Add(ValidEntities[BestIdx]);
        ValidEntities.RemoveAt(BestIdx);
    }
    
    // Copy the sorted array back to the TickEntities array
    TickEntities = SortedEntities;
}

//////////////////////////////////////////////////////////////////////////
// FSpatialEntityBatch Implementation

uint16 FSpatialEntityBatch::CalculateGridCell(const FVector& Position) const
{
    // Convert 3D position to 2D grid ID
    // Simple hash function (more advanced spatial hash functions can be used)
    const int32 CellX = FMath::FloorToInt(Position.X / GridCellSize);
    const int32 CellY = FMath::FloorToInt(Position.Y / GridCellSize);
    const int32 CellZ = FMath::FloorToInt(Position.Z / GridCellSize);
    
    // Convert 3D position into a single 16-bit hash
    // This is just an example - a better hash might be required in a real implementation
    const uint16 CombinedHash = (uint16)(
        ((CellX & 0x3F) << 10) |  // 6 bits for X
        ((CellY & 0x3F) << 4) |   // 6 bits for Y
        (CellZ & 0xF)             // 4 bits for Z
    );
    
    return CombinedHash;
}

void FSpatialEntityBatch::AddEntity(FTickEntityData* Entity)
{
    if (!Entity || !Entity->Object)
    {
        return;
    }
    
    // Lock check for thread safety using shared pointer
    if (SpatialLock.Get())
    {
        FScopeLock Lock(SpatialLock.Get());
        
        // Calculate grid cell
        const uint16 GridCell = CalculateGridCell(Entity->Position);
        Entity->SpatialBucketId = GridCell;
        
        // Add to grid cell
        GridCells.FindOrAdd(GridCell).Add(Entity);
        AllSpatialEntities.Add(Entity);
    }
    else
    {
        // Calculate grid cell
        const uint16 GridCell = CalculateGridCell(Entity->Position);
        Entity->SpatialBucketId = GridCell;
        
        // Add to grid cell
        GridCells.FindOrAdd(GridCell).Add(Entity);
        AllSpatialEntities.Add(Entity);
    }
}

void FSpatialEntityBatch::RemoveEntity(FTickEntityData* Entity)
{
    if (!Entity || !Entity->Object)
    {
        return;
    }
    
    // Lock check for thread safety
    if (SpatialLock.Get())
    {
        FScopeLock Lock(SpatialLock.Get());
        
        // Remove from grid cell
        const uint16 GridCell = Entity->SpatialBucketId;
        if (TArray<FTickEntityData*>* Cell = GridCells.Find(GridCell))
        {
            Cell->Remove(Entity);
            if (Cell->Num() == 0)
            {
                GridCells.Remove(GridCell);
            }
        }
        
        AllSpatialEntities.Remove(Entity);
    }
    else
    {
        // Remove from grid cell
        const uint16 GridCell = Entity->SpatialBucketId;
        if (TArray<FTickEntityData*>* Cell = GridCells.Find(GridCell))
        {
            Cell->Remove(Entity);
            if (Cell->Num() == 0)
            {
                GridCells.Remove(GridCell);
            }
        }
        
        AllSpatialEntities.Remove(Entity);
    }
}

void FSpatialEntityBatch::TickAllGrids(float DeltaTime)
{
    SCOPE_CYCLE_COUNTER(STAT_EnhancedTick_SpatialBatches);
    
    // First, acquire the lock - check the SpatialLock shared pointer
    FScopeLock ScopeLock(SpatialLock.Get());
    
    // Tick each grid cell individually
    for (auto& GridPair : GridCells)
    {
        uint16 GridId = GridPair.Key;
        TArray<FTickEntityData*>& Entities = GridPair.Value;
        
        // If there are too few entities in the grid, skip ticking
        if (Entities.Num() < 1)
        {
            continue;
        }
        
        // Prefetch the first entity to optimize cache usage
        if (Entities.Num() > 0 && Entities[0] && Entities[0]->Object)
        {
            ENHANCED_TICK_PREFETCH_DATA(Entities[0]->Object);
        }
        
        // Tick each entity - add NULL checks
        for (int32 i = 0; i < Entities.Num(); ++i)
        {
            FTickEntityData* Entity = Entities[i];
            
            // NULL check
            if (!Entity || !IsValid(Entity->Object))
            {
                continue;
            }
            
            // Prefetch the next entity
            if (i + 1 < Entities.Num() && Entities[i + 1] && Entities[i + 1]->Object)
            {
                ENHANCED_TICK_PREFETCH_DATA(Entities[i + 1]->Object);
            }
            
            // Tick the entity
            if (Entity->bEnabled && Entity->TickFunction)
            {
                Entity->TickFunction(DeltaTime);
            }
        }
    }
}

TArray<FTickEntityData*> FSpatialEntityBatch::GetNearbyEntities(const FVector& Position, float Radius)
{
    TArray<FTickEntityData*> NearbyEntities;
    
    // Calculate the center grid cell and the surrounding cells
    const uint16 CenterGridId = CalculateGridCell(Position);
    
    // Grid radius (number of cells to check)
    const int32 GridRadius = FMath::CeilToInt(Radius / GridCellSize);
    
    // Calculate grid IDs for nearby cells
    TArray<uint16> NearbyGrids;
    NearbyGrids.Add(CenterGridId); // Add the center grid
    
    // Add grids surrounding the center (simplified)
    // Note: In a real implementation, a more accurate neighbor calculation should be used
    
    // Convert the center grid ID into X, Y, Z coordinates from the hash
    const int32 CenterX = (CenterGridId >> 10) & 0x3F;
    const int32 CenterY = (CenterGridId >> 4) & 0x3F;
    const int32 CenterZ = CenterGridId & 0xF;
    
    // Calculate neighboring grid cells
    for (int32 dx = -GridRadius; dx <= GridRadius; ++dx)
    {
        for (int32 dy = -GridRadius; dy <= GridRadius; ++dy)
        {
            for (int32 dz = -GridRadius; dz <= GridRadius; ++dz)
            {
                const int32 NbX = CenterX + dx;
                const int32 NbY = CenterY + dy;
                const int32 NbZ = CenterZ + dz;
                
                // Check grid boundaries
                if (NbX < 0 || NbX > 0x3F || NbY < 0 || NbY > 0x3F || NbZ < 0 || NbZ > 0xF)
                {
                    continue;
                }
                
                // Calculate the neighboring grid ID
                const uint16 NbGridId = (uint16)(
                    ((NbX & 0x3F) << 10) |
                    ((NbY & 0x3F) << 4) |
                    (NbZ & 0xF)
                );
                
                if (NbGridId != CenterGridId)
                {
                    NearbyGrids.Add(NbGridId);
                }
            }
        }
    }
    
    // Check the entities in each nearby grid
    for (uint16 GridId : NearbyGrids)
    {
        if (TArray<FTickEntityData*>* GridEntities = GridCells.Find(GridId))
        {
            for (FTickEntityData* Entity : *GridEntities)
            {
                // Check the distance
                if (Entity && Entity->bEnabled && Entity->Object)
                {
                    const float Distance = FVector::Distance(Position, Entity->Position);
                    if (Distance <= Radius)
                    {
                        NearbyEntities.Add(Entity);
                    }
                }
            }
        }
    }
    
    return NearbyEntities;
}

//////////////////////////////////////////////////////////////////////////
// UEnhancedTickSystem Implementation

UEnhancedTickSystem::UEnhancedTickSystem()
    : BatchesLock(MakeShared<FCriticalSection>())
    , FrameCounter(0)
    , bDebugMode(false)
    , bVerboseDebug(false)
{
    
}

void UEnhancedTickSystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    
    UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem initialized"));
    
    // Initialize tick groups
    static TArray<ETickingGroup> AllGroups = {
        TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics,
        TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable
    };
    
    for (ETickingGroup Group : AllGroups)
    {
        GroupedBatches.Add(Group, TArray<FComponentTypeBatch*>());
    }
    
    // Set spatial grid size (e.g., 2000.0f units, which corresponds to 20 meters)
    SpatialBatch.GridCellSize = 2000.0f;
}

void UEnhancedTickSystem::Deinitialize()
{
    Super::Deinitialize();
    
    UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem shut down"));
    
    // Clear all batches
    TypeBatches.Empty();
    GroupedBatches.Empty();
}

void UEnhancedTickSystem::Tick(float DeltaTime)
{
    SCOPE_CYCLE_COUNTER(STAT_EnhancedTick_Total);
    
    // Debug output
    if (bDebugMode)
    {
        UE_LOG(LogTemp, Verbose, TEXT("EnhancedTickSystem Tick: DeltaTime=%.4f"), DeltaTime);
        
        if (bVerboseDebug && GEngine)
        {
            GEngine->AddOnScreenDebugMessage(-1, DeltaTime, FColor::Green,
                FString::Printf(TEXT("EnhancedTickSystem: %d batches, %d entities"),
                TypeBatches.Num(), Stats.TotalRegisteredEntities));
        }
    }
    
    // Update the frame counter (for low priority ticks)
    FrameCounter = (FrameCounter + 1) % 1000; // Cycle every 1000 frames
    
    // Process deferred operations
    ProcessDeferredOperations();
    
    // Update conditional tick objects
    UpdateConditionalTicks();
    
    // First, sort the groups that need to be ticked
    SortBatchesByPriority();
    
    // Process tick groups in order
    TArray<ETickingGroup> TickOrder = {
        TG_PrePhysics, TG_StartPhysics, TG_DuringPhysics, TG_EndPhysics,
        TG_PostPhysics, TG_PostUpdateWork, TG_LastDemotable
    };
    
    for (ETickingGroup Group : TickOrder)
    {
        TickGroupBatches(Group, DeltaTime);
    }
    
    // Tick spatially aware entities - include error checks
    if (!SpatialBatch.GridCells.IsEmpty() && SpatialBatch.SpatialLock.IsValid())
    {
        SpatialBatch.TickAllGrids(DeltaTime);
    }
    
    // If it's time for optimization, optimize batches
    if (FrameCounter % 300 == 0) // Optimize every 300 frames
    {
        OptimizeBatches();
    }
}

TStatId UEnhancedTickSystem::GetStatId() const
{
    RETURN_QUICK_DECLARE_CYCLE_STAT(UEnhancedTickSystem, STATGROUP_EnhancedTick);
}

void UEnhancedTickSystem::RegisterComponent(UActorComponent* Component, ETickBatchFlags Flags, UObject* CustomTickTarget, FName CustomTickFunction)
{
    if (!IsValid(Component))
    {
        UE_LOG(LogTemp, Warning, TEXT("EnhancedTickSystem: Invalid component cannot be registered"));
        return;
    }
    
    // THREAD SAFETY CHECK:
    // For USceneComponent and its derivatives, disable parallel processing if needed
    if ((Component->IsA<USceneComponent>() || Component->IsA<UPrimitiveComponent>() || 
        Component->IsA<UCharacterMovementComponent>()) && 
        EnumHasAnyFlags(Flags, ETickBatchFlags::UseParallel))
    {
        // Remove the parallel processing flag
        Flags &= ~ETickBatchFlags::UseParallel;
        
        if (bVerboseDebug)
        {
            UE_LOG(LogTemp, Warning, TEXT("EnhancedTickSystem: Parallel processing disabled for %s - not thread-safe"),
                *Component->GetName());
        }
    }
    
    // Queue the component for asynchronous registration
    if (BatchesLock.Get())
    {
        FScopeLock Lock(BatchesLock.Get());
        PendingRegistrations.Add(TPair<UObject*, ETickBatchFlags>(Component, Flags));
    }
    else
    {
        PendingRegistrations.Add(TPair<UObject*, ETickBatchFlags>(Component, Flags));
    }
    
    // Disable the component's standard tick
    Component->PrimaryComponentTick.bCanEverTick = false;
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Component queued for registration: %s"), *Component->GetName());
    }
}

void UEnhancedTickSystem::RegisterActor(AActor* Actor, ETickBatchFlags Flags, bool bIncludeComponents)
{
    if (!IsValid(Actor))
    {
        UE_LOG(LogTemp, Warning, TEXT("EnhancedTickSystem: Invalid actor cannot be registered"));
        return;
    }
    
    // Queue the actor for registration
    if (BatchesLock.Get())
    {
        FScopeLock Lock(BatchesLock.Get());
        PendingRegistrations.Add(TPair<UObject*, ETickBatchFlags>(Actor, Flags));
        
        // Also register all components of the actor if required
        if (bIncludeComponents)
        {
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            
            for (UActorComponent* Component : Components)
            {
                if (IsValid(Component) && Component->PrimaryComponentTick.bCanEverTick)
                {
                    PendingRegistrations.Add(TPair<UObject*, ETickBatchFlags>(Component, Flags));
                    Component->PrimaryComponentTick.bCanEverTick = false;
                }
            }
        }
    }
    else
    {
        PendingRegistrations.Add(TPair<UObject*, ETickBatchFlags>(Actor, Flags));
        
        // Also register all components of the actor if required
        if (bIncludeComponents)
        {
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            
            for (UActorComponent* Component : Components)
            {
                if (IsValid(Component) && Component->PrimaryComponentTick.bCanEverTick)
                {
                    PendingRegistrations.Add(TPair<UObject*, ETickBatchFlags>(Component, Flags));
                    Component->PrimaryComponentTick.bCanEverTick = false;
                }
            }
        }
    }
    
    // Disable the actor's standard tick
    Actor->SetActorTickEnabled(false);
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Actor queued for registration: %s"), *Actor->GetName());
    }
}

void UEnhancedTickSystem::RegisterAllComponentsOfType(TSubclassOf<UActorComponent> ComponentClass, ETickBatchFlags Flags)
{
    if (!ComponentClass)
    {
        UE_LOG(LogTemp, Warning, TEXT("EnhancedTickSystem: Invalid component class specified"));
        return;
    }
    
    // Find all components of the specified type in the world
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    
    // Iterate through all actors in the world
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        AActor* Actor = *It;
        if (!IsValid(Actor))
        {
            continue;
        }
        
        // Get the actor's components of the specified type
        TArray<UActorComponent*> Components;
        Actor->GetComponents(ComponentClass, Components);
        
        for (UActorComponent* Component : Components)
        {
            if (IsValid(Component) && Component->PrimaryComponentTick.bCanEverTick)
            {
                RegisterComponent(Component, Flags);
            }
        }
    }
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: All components of type %s registered"), 
            *ComponentClass->GetName());
    }
}

void UEnhancedTickSystem::UnregisterComponent(UActorComponent* Component)
{
    if (!IsValid(Component))
    {
        return;
    }
    
    // Queue the component for deferred unregistration
    if (BatchesLock.Get())
    {
        FScopeLock Lock(BatchesLock.Get());
        PendingUnregistrations.Add(Component);
    }
    else
    {
        PendingUnregistrations.Add(Component);
    }
    
    // Re-enable the component's standard tick
    Component->PrimaryComponentTick.bCanEverTick = true;
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Component queued for unregistration: %s"), 
            *Component->GetName());
    }
}

void UEnhancedTickSystem::UnregisterActor(AActor* Actor, bool bIncludeComponents)
{
    if (!IsValid(Actor))
    {
        return;
    }
    
    // Queue the actor for deferred unregistration
    if (BatchesLock.Get())
    {
        FScopeLock Lock(BatchesLock.Get());
        PendingUnregistrations.Add(Actor);
        
        // Also queue all components of the actor for removal if required
        if (bIncludeComponents)
        {
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            
            for (UActorComponent* Component : Components)
            {
                if (IsValid(Component))
                {
                    PendingUnregistrations.Add(Component);
                    Component->PrimaryComponentTick.bCanEverTick = true;
                }
            }
        }
    }
    else
    {
        PendingUnregistrations.Add(Actor);
        
        // Also queue all components of the actor for removal if required
        if (bIncludeComponents)
        {
            TArray<UActorComponent*> Components;
            Actor->GetComponents(Components);
            
            for (UActorComponent* Component : Components)
            {
                if (IsValid(Component))
                {
                    PendingUnregistrations.Add(Component);
                    Component->PrimaryComponentTick.bCanEverTick = true;
                }
            }
        }
    }
    
    // Re-enable the actor's standard tick
    Actor->SetActorTickEnabled(true);
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Actor queued for unregistration: %s"), 
            *Actor->GetName());
    }
}

void UEnhancedTickSystem::SetDebugMode(bool bEnable, bool bVerbose)
{
    bDebugMode = bEnable;
    bVerboseDebug = bVerbose;
    
    UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Debug mode %s"), 
        bEnable ? TEXT("enabled") : TEXT("disabled"));
}

void UEnhancedTickSystem::OptimizeBatches()
{
    SCOPE_CYCLE_COUNTER(STAT_EnhancedTick_Total);
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Batch optimization started"));
    }
    
    // Analyze the current state for profiling data
    AnalyzeCurrentState();
    
    // Apply optimization hints
    ApplyOptimizationHints();
    
    // Optimize for specific component types
    for (auto& Pair : TypeBatches)
    {
        UClass* Class = Pair.Key;
        FComponentTypeBatch& Batch = Pair.Value;
        
        // Special optimization for CharacterMovementComponent
        if (Class->IsChildOf(UCharacterMovementComponent::StaticClass()))
        {
            OptimizeCharacterMovementBatch(Batch);
            UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Batch optimization UCharacterMovementComponent"));
        }
        // Special optimization for AIPerceptionComponent
        else if (Class->IsChildOf(UAIPerceptionComponent::StaticClass()))
        {
            OptimizeAIPerceptionBatch(Batch);
            UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Batch optimization UAIPerceptionComponent"));
        }
    }
    
    if (bVerboseDebug)
    {
        UE_LOG(LogTemp, Log, TEXT("EnhancedTickSystem: Batch optimization completed"));
    }
}

TMap<FString, float> UEnhancedTickSystem::GetBatchProfilingData() const
{
    TMap<FString, float> ProfilingData;
    
    for (const auto& Pair : TypeBatches)
    {
        UClass* Class = Pair.Key;
        const FComponentTypeBatch& Batch = Pair.Value;
        
        if (Class)
        {
            ProfilingData.Add(Class->GetName(), Batch.AverageTickTimeNs);
        }
    }
    
    return ProfilingData;
}

FString UEnhancedTickSystem::GetDetailedStats() const
{
    FString StatsString;
    
    StatsString += FString::Printf(TEXT("Total Registered Entities: %d\n"), Stats.TotalRegisteredEntities);
    StatsString += FString::Printf(TEXT("Active Entities: %d\n"), Stats.ActiveEntities);
    StatsString += FString::Printf(TEXT("Parallel Batch Count: %d\n"), Stats.ParallelBatchCount);
    StatsString += FString::Printf(TEXT("Spatial Batch Count: %d\n"), Stats.SpatialBatchCount);
    StatsString += FString::Printf(TEXT("Total Tick Time: %.4f ms\n"), Stats.TotalTickTimeMs);
    StatsString += FString::Printf(TEXT("Cache Miss Count: %d\n"), Stats.CacheMissCount);
    
    return StatsString;
}

void UEnhancedTickSystem::AnalyzeCurrentState()
{
    // Analyze profiling data for each batch
    for (auto& Pair : TypeBatches)
    {
        UClass* Class = Pair.Key;
        FComponentTypeBatch& Batch = Pair.Value;
        
        // Evaluate the potential for parallel processing based on the batch tick time
        if (Batch.AverageTickTimeNs > 1000.0f && Batch.TickEntities.Num() > 10)
        {
            Batch.Flags |= ETickBatchFlags::UseParallel;
            Stats.ParallelBatchCount++;
        }
        
        // Disable cache optimization for batches with very few entities
        if (Batch.TickEntities.Num() < 5)
        {
            Batch.bSortByCacheLocality = false;
        }
        
        // Mark batches as low priority if tick time is very low and not high priority
        if (Batch.AverageTickTimeNs < 100.0f && !EnumHasAnyFlags(Batch.Flags, ETickBatchFlags::HighPrio))
        {
            Batch.Flags |= ETickBatchFlags::LowPrio;
        }
    }
}

void UEnhancedTickSystem::ApplyOptimizationHints()
{
    // Identify spatially aware components
    for (auto& Pair : TypeBatches)
    {
        UClass* Class = Pair.Key;
        FComponentTypeBatch& Batch = Pair.Value;
        
        if (!IsValid(Class))
        {
            continue;
        }
        
        // Components that require location information
        const bool bIsSpatialComponent = 
            Class->IsChildOf(UPrimitiveComponent::StaticClass()) ||
            Class->IsChildOf(UAIPerceptionComponent::StaticClass()) ||
            Class->GetName().Contains(TEXT("Spatial")) ||
            Class->GetName().Contains(TEXT("Physics"));
        
        if (bIsSpatialComponent && !EnumHasAnyFlags(Batch.Flags, ETickBatchFlags::SpatialAware))
        {
            Batch.Flags |= ETickBatchFlags::SpatialAware;
            Stats.SpatialBatchCount++;
            
            // Move spatially aware entities to the spatial batch
            for (auto& Entity : Batch.TickEntities)
            {
                if (Entity.bEnabled && IsValid(Entity.Object))
                {
                    // Add a NULL check for safety
                    if (SpatialBatch.SpatialLock.IsValid())
                    {
                        SpatialBatch.AddEntity(&Entity);
                    }
                }
            }
        }
    }
}

TFunction<void(const TArrayView<FTickEntityData>&, float)> UEnhancedTickSystem::DetermineBestTickFunction(UClass* Class)
{
    // Special handling for CharacterMovementComponent - disable parallel processing as it is not thread-safe
    if (Class->IsChildOf(UCharacterMovementComponent::StaticClass()))
    {
        return [](const TArrayView<FTickEntityData>& Entities, float DeltaTime)
        {
            // Prefetch the first component if available
            if (Entities.Num() > 0 && Entities[0].Object)
            {
                ENHANCED_TICK_PREFETCH_DATA(Entities[0].Object);
            }
            
            // NO PARALLEL PROCESSING - standard sequential tick for thread safety
            for (int32 i = 0; i < Entities.Num(); ++i)
            {
                const FTickEntityData& Entity = Entities[i];
                UCharacterMovementComponent* CMC = Cast<UCharacterMovementComponent>(Entity.Object);
                
                if (!CMC || !Entity.bEnabled)
                {
                    continue;
                }
                
                // Prefetch the next component if available
                if (i + 1 < Entities.Num() && Entities[i + 1].Object)
                {
                    ENHANCED_TICK_PREFETCH_DATA(Entities[i + 1].Object);
                }
                
                // Tick the component (sequential processing)
                CMC->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
            }
        };
    }
    
    // Default tick function for general components
    return [](const TArrayView<FTickEntityData>& Entities, float DeltaTime)
    {
        // Prefetch the first component if available
        if (Entities.Num() > 0 && Entities[0].Object)
        {
            ENHANCED_TICK_PREFETCH_DATA(Entities[0].Object);
        }
        
        // Tick each component sequentially
        for (int32 i = 0; i < Entities.Num(); ++i)
        {
            const FTickEntityData& Entity = Entities[i];
            
            // Prefetch the next component if available
            if (i + 1 < Entities.Num() && Entities[i + 1].Object)
            {
                ENHANCED_TICK_PREFETCH_DATA(Entities[i + 1].Object);
            }
            
            // Use the custom tick function if provided, otherwise call the default tick
            if (Entity.TickFunction)
            {
                Entity.TickFunction(DeltaTime);
            }
            else if (UActorComponent* Component = Cast<UActorComponent>(Entity.Object))
            {
                Component->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
            }
            else if (AActor* Actor = Cast<AActor>(Entity.Object))
            {
                Actor->Tick(DeltaTime);
            }
        }
    };
}

void UEnhancedTickSystem::TickGroupBatches(ETickingGroup Group, float DeltaTime)
{
    SCOPE_CYCLE_COUNTER(STAT_EnhancedTick_TypeBatches);
    
    if (TArray<FComponentTypeBatch*>* Batches = GroupedBatches.Find(Group))
    {
        for (FComponentTypeBatch* Batch : *Batches)
        {
            if (!Batch)
            {
                continue;
            }
            
            // For low priority batches, skip ticking every frame based on the frame counter
            if (EnumHasAnyFlags(Batch->Flags, ETickBatchFlags::LowPrio))
            {
                if ((FrameCounter % 3) != 0)
                {
                    continue;
                }
            }
            
            // Choose the appropriate ticking method based on parallel capability and entity count
            if (Batch->CanTickInParallel() && Batch->TickEntities.Num() > 10)
            {
                // Parallel tick
                Batch->TickBatchParallel(DeltaTime);
            }
            else
            {
                // Standard sequential tick
                Batch->TickBatch(DeltaTime);
            }
            
            // Update profiling data for the batch (convert ns to ms)
            UpdateBatchProfilingData(*Batch, Batch->AverageTickTimeNs / 1000000.0f);
        }
    }
}

void UEnhancedTickSystem::ProcessDeferredOperations()
{
    // Secure the locked block using shared pointer for thread safety
    if (BatchesLock.IsValid())
    {
        FScopeLock Lock(BatchesLock.Get());
        ProcessDeferredOperationsImpl();
    }
    else
    {
        ProcessDeferredOperationsImpl();
    }
}

// Internal implementation of deferred operations (called within lock)
void UEnhancedTickSystem::ProcessDeferredOperationsImpl()
{
    // Process pending registrations
    for (const auto& RegPair : PendingRegistrations)
    {
        UObject* Object = RegPair.Key;
        ETickBatchFlags Flags = RegPair.Value;
        
        if (!IsValid(Object))
        {
            continue;
        }
        
        // Determine if the object is a component or an actor
        UActorComponent* Component = Cast<UActorComponent>(Object);
        AActor* Actor = Component ? nullptr : Cast<AActor>(Object);
        
        if (Component)
        {
            // Add the component to the appropriate batch
            UClass* ComponentClass = Component->GetClass();
            if (!IsValid(ComponentClass))
            {
                continue;
            }
            
            FComponentTypeBatch& Batch = TypeBatches.FindOrAdd(ComponentClass);
            
            // If the batch is created for the first time
            if (Batch.TypeName.IsEmpty())
            {
                Batch.TypeName = ComponentClass->GetName();
                Batch.TickGroup = Component->PrimaryComponentTick.TickGroup;
                Batch.Flags = Flags;
                Batch.BatchTickFunction = DetermineBestTickFunction(ComponentClass);
                
                // Add the batch to the appropriate tick group
                GroupedBatches.FindOrAdd(Batch.TickGroup).Add(&Batch);
            }
            
            // Create tick data for the component
            FTickEntityData EntityData;
            EntityData.Object = Component;
            EntityData.Position = Component->GetOwner() ? Component->GetOwner()->GetActorLocation() : FVector::ZeroVector;
            EntityData.SpatialBucketId = CalculateSpatialBucketId(EntityData.Position);
            EntityData.Priority = Component->PrimaryComponentTick.TickGroup == TG_PostPhysics ? 200 : 100;
            EntityData.bEnabled = Component->IsActive();
            
            // Define a custom tick function for the component
            EntityData.TickFunction = [Component](float DeltaTime) {
                if (Component && Component->IsActive())
                {
                    Component->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
                }
            };
            
            // Add the component to the batch
            Batch.TickEntities.Add(EntityData);
            
            // For spatially aware components, also add them to the spatial batch
            if (EnumHasAnyFlags(Flags, ETickBatchFlags::SpatialAware) && SpatialBatch.SpatialLock.IsValid())
            {
                SpatialBatch.AddEntity(&Batch.TickEntities.Last());
            }
            
            Stats.TotalRegisteredEntities++;
        }
        else if (Actor)
        {
            // Create a batch for the actor
            UClass* ActorClass = Actor->GetClass();
            if (!IsValid(ActorClass))
            {
                continue;
            }
            
            FComponentTypeBatch& Batch = TypeBatches.FindOrAdd(ActorClass);
            
            // If the batch is created for the first time
            if (Batch.TypeName.IsEmpty())
            {
                Batch.TypeName = ActorClass->GetName();
                Batch.TickGroup = Actor->PrimaryActorTick.TickGroup;
                Batch.Flags = Flags;
                Batch.BatchTickFunction = [](const TArrayView<FTickEntityData>& Entities, float DeltaTime) {
                    for (const auto& Entity : Entities)
                    {
                        if (AActor* Actor = Cast<AActor>(Entity.Object))
                        {
                            Actor->Tick(DeltaTime);
                        }
                    }
                };
                
                // Add the batch to the appropriate tick group
                GroupedBatches.FindOrAdd(Batch.TickGroup).Add(&Batch);
            }
            
            // Create tick data for the actor
            FTickEntityData EntityData;
            EntityData.Object = Actor;
            EntityData.Position = Actor->GetActorLocation();
            EntityData.SpatialBucketId = CalculateSpatialBucketId(EntityData.Position);
            EntityData.Priority = 100;
            EntityData.bEnabled = Actor->IsActorTickEnabled();
            
            // Define a custom tick function for the actor
            EntityData.TickFunction = [Actor](float DeltaTime) {
                if (Actor)
                {
                    Actor->Tick(DeltaTime);
                }
            };
            
            // Add the actor to the batch
            Batch.TickEntities.Add(EntityData);
            
            // For spatially aware actors, also add them to the spatial batch
            if (EnumHasAnyFlags(Flags, ETickBatchFlags::SpatialAware) && SpatialBatch.SpatialLock.IsValid())
            {
                SpatialBatch.AddEntity(&Batch.TickEntities.Last());
            }
            
            Stats.TotalRegisteredEntities++;
        }
    }
    
    // Process pending unregistrations
    for (UObject* Object : PendingUnregistrations)
    {
        if (!IsValid(Object))
        {
            continue;
        }
        
        // Remove the object from all batches
        for (auto& Pair : TypeBatches)
        {
            FComponentTypeBatch& Batch = Pair.Value;
            
            // Find and remove the matching entity
            for (int32 i = 0; i < Batch.TickEntities.Num(); ++i)
            {
                if (Batch.TickEntities[i].Object == Object)
                {
                    // First, remove from the spatial batch
                    if (SpatialBatch.SpatialLock.IsValid())
                    {
                        SpatialBatch.RemoveEntity(&Batch.TickEntities[i]);
                    }
                    
                    // Then remove from the batch
                    Batch.TickEntities.RemoveAt(i);
                    Stats.TotalRegisteredEntities--;
                    break;
                }
            }
        }
    }
    
    // Clear the registration and unregistration queues
    PendingRegistrations.Empty();
    PendingUnregistrations.Empty();
}

void UEnhancedTickSystem::UpdateBatchProfilingData(FComponentTypeBatch& Batch, float ExecutionTimeMs)
{
    // Update profiling data using exponential averaging (to smooth out sudden changes)
    constexpr float Alpha = 0.2f; // Weight factor (0-1)
    
    Stats.TotalTickTimeMs += ExecutionTimeMs;
    Stats.ActiveEntities += Batch.LastFrameTickCount;
}

void UEnhancedTickSystem::UpdateConditionalTicks()
{
    // Check conditional tick objects
    for (auto& Pair : TypeBatches)
    {
        FComponentTypeBatch& Batch = Pair.Value;
        
        if (!EnumHasAnyFlags(Batch.Flags, ETickBatchFlags::Conditional))
        {
            continue;
        }
        
        // Update the status of objects with conditional ticks
        for (auto& Entity : Batch.TickEntities)
        {
            if (!IsValid(Entity.Object))
            {
                Entity.bEnabled = false;
                continue;
            }
            
            // For components, check if active
            if (UActorComponent* Component = Cast<UActorComponent>(Entity.Object))
            {
                Entity.bEnabled = Component->IsActive();
            }
            // For actors, check if active (use IsValid instead of IsPendingKill in UE5)
            else if (AActor* Actor = Cast<AActor>(Entity.Object))
            {
                Entity.bEnabled = IsValid(Actor) && Actor->IsActorTickEnabled();
            }
        }
    }
}

void UEnhancedTickSystem::OptimizeCharacterMovementBatch(FComponentTypeBatch& Batch)
{
    // Special tick function for CharacterMovementComponent
    Batch.BatchTickFunction = [](const TArrayView<FTickEntityData>& Entities, float DeltaTime)
    {
        // CharacterMovementComponents are not thread-safe due to transform updates;
        // therefore, we process them sequentially.
        
        // Prefetch the first component if available
        if (Entities.Num() > 0 && Entities[0].Object)
        {
            ENHANCED_TICK_PREFETCH_DATA(Entities[0].Object);
        }
        
        // Process all components on a single thread - no parallel processing
        for (int32 i = 0; i < Entities.Num(); ++i)
        {
            const FTickEntityData& Entity = Entities[i];
            UCharacterMovementComponent* CMC = Cast<UCharacterMovementComponent>(Entity.Object);
            
            if (!CMC || !Entity.bEnabled)
            {
                continue;
            }
            
            // Prefetch the next component if available
            if (i + 1 < Entities.Num() && Entities[i + 1].Object)
            {
                ENHANCED_TICK_PREFETCH_DATA(Entities[i + 1].Object);
            }
            
            // Tick the component (sequential processing)
            CMC->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
        }
    };
    
    // Disable parallel processing for CharacterMovementComponents
    Batch.Flags &= ~ETickBatchFlags::UseParallel;
    
    // Enable spatial awareness for this group
    Batch.Flags |= ETickBatchFlags::SpatialAware;
}

void UEnhancedTickSystem::OptimizeAIPerceptionBatch(FComponentTypeBatch& Batch)
{
    // Optimized tick lambda for AIPerceptionComponent
    Batch.BatchTickFunction = [this](const TArrayView<FTickEntityData>& Entities, float DeltaTime)
    {
        // Special AI perception optimization
        // Use spatial cell grouping to optimize overlapping perception regions
        
        // Prefetch the first component if available
        if (Entities.Num() > 0 && Entities[0].Object)
        {
            ENHANCED_TICK_PREFETCH_DATA(Entities[0].Object);
        }
        
        for (int32 i = 0; i < Entities.Num(); ++i)
        {
            const FTickEntityData& Entity = Entities[i];
            UAIPerceptionComponent* PerceptionComp = Cast<UAIPerceptionComponent>(Entity.Object);
            
            if (!PerceptionComp || !Entity.bEnabled)
            {
                continue;
            }
            
            // Prefetch the next component if available
            if (i + 1 < Entities.Num() && Entities[i + 1].Object)
            {
                ENHANCED_TICK_PREFETCH_DATA(Entities[i + 1].Object);
            }
            
            // Tick the AI perception component
            PerceptionComp->TickComponent(DeltaTime, ELevelTick::LEVELTICK_All, nullptr);
            
            // Find nearby AIs and share perception results (this part can be customized based on game logic)
        }
    };
    
    // Enable spatial awareness for this group
    Batch.Flags |= ETickBatchFlags::SpatialAware;
}

uint16 UEnhancedTickSystem::CalculateSpatialBucketId(const FVector& Position)
{
    return SpatialBatch.CalculateGridCell(Position);
}

void UEnhancedTickSystem::SortBatchesByPriority()
{
    // Sort batches within each tick group by priority
    for (auto& GroupPair : GroupedBatches)
    {
        TArray<FComponentTypeBatch*>& Batches = GroupPair.Value;
        
        Batches.Sort([](const FComponentTypeBatch& A, const FComponentTypeBatch& B) {
            // High priority batches first
            const bool bAHighPrio = EnumHasAnyFlags(A.Flags, ETickBatchFlags::HighPrio);
            const bool bBHighPrio = EnumHasAnyFlags(B.Flags, ETickBatchFlags::HighPrio);
            
            if (bAHighPrio != bBHighPrio)
            {
                return bAHighPrio > bBHighPrio;
            }
            
            // Low priority batches later
            const bool bALowPrio = EnumHasAnyFlags(A.Flags, ETickBatchFlags::LowPrio);
            const bool bBLowPrio = EnumHasAnyFlags(B.Flags, ETickBatchFlags::LowPrio);
            
            if (bALowPrio != bBLowPrio)
            {
                return bALowPrio < bBLowPrio;
            }
            
            // For batches with equal priority, sort by the number of entities
            return A.TickEntities.Num() > B.TickEntities.Num();
        });
    }
}
