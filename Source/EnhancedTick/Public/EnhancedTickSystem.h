// Copyright (C) Thyke 2025 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "Stats/Stats.h"
#include "Containers/StaticArray.h"
#include "HAL/CriticalSection.h"
#include "Async/TaskGraphInterfaces.h"
#include "EnhancedTickSystem.generated.h"

// Define the stats group
DECLARE_STATS_GROUP(TEXT("EnhancedTickSystem"), STATGROUP_EnhancedTick, STATCAT_Advanced);

// Extern variable declarations for stats - definitions will be provided in the CPP file
DECLARE_CYCLE_STAT_EXTERN(TEXT("Enhanced Tick - Total"), STAT_EnhancedTick_Total, STATGROUP_EnhancedTick, ENHANCEDTICK_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Enhanced Tick - Type Batches"), STAT_EnhancedTick_TypeBatches, STATGROUP_EnhancedTick, ENHANCEDTICK_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Enhanced Tick - Spatial Batches"), STAT_EnhancedTick_SpatialBatches, STATGROUP_EnhancedTick, ENHANCEDTICK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Enhanced Tick - Cache Misses"), STAT_EnhancedTick_CacheMisses, STATGROUP_EnhancedTick, ENHANCEDTICK_API);

// Define tick properties as bitflags
UENUM(BlueprintType, meta = (Bitflags))
enum class ETickBatchFlags : uint8
{
    None            = 0,
    UseParallel     = 1 << 0,  // Suitable for parallel processing
    CacheHot        = 1 << 1,  // Frequently accessed data
    Conditional     = 1 << 2,  // Conditional tick (may not tick every frame)
    HighPrio        = 1 << 3,  // High priority
    LowPrio         = 1 << 4,  // Low priority
    SpatialAware    = 1 << 5,  // Spatial awareness (interaction with nearby objects)
    StateDependent  = 1 << 6   // State dependent
};
ENUM_CLASS_FLAGS(ETickBatchFlags);

/**
 * Tick data required for a single instance of a component type.
 * Contains only the necessary data and is designed to be cache-friendly.
 */
struct FTickEntityData
{
    UObject* Object;                      // The object to be ticked (Actor or Component)
    TFunction<void(float)> TickFunction;  // Tick lambda function
    FVector Position;                     // World position (for spatial batching)
    uint16 SpatialBucketId;               // Spatial cell ID (grid-based)
    uint8 Priority;                       // Tick priority (0-255)
    bool bEnabled;                        // Is it enabled?
    
    FTickEntityData() : Object(nullptr), SpatialBucketId(0), Priority(128), bEnabled(true) {}
    
    // Calculates the distance between two tick entities (for cache locality)
    float GetDistance(const FTickEntityData& Other) const
    {
        return FVector::Distance(Position, Other.Position);
    }
};

/**
 * A batch for components of the same type.
 * Optimized for data cache alignment.
 */
USTRUCT()
struct ENHANCEDTICK_API FComponentTypeBatch
{
    GENERATED_BODY()
    
    // Type name (for debugging)
    FString TypeName;
    
    // Batch flags
    ETickBatchFlags Flags;
    
    // Lock for parallel processing (using TSharedPtr since FCriticalSection cannot be copied)
    TSharedPtr<FCriticalSection> BatchLock;
    
    // All objects to be ticked
    TArray<FTickEntityData> TickEntities;
    
    // Function to trigger ticks for this group
    TFunction<void(const TArrayView<FTickEntityData>&, float)> BatchTickFunction;
    
    // Tick group
    ETickingGroup TickGroup;
    
    // Average processing time per tick function (in nanoseconds)
    float AverageTickTimeNs;
    
    // Number of entities ticked in the last frame
    int32 LastFrameTickCount;
    
    // Whether to use reordering based on cache sorting
    bool bSortByCacheLocality;
    
    FComponentTypeBatch() 
        : Flags(ETickBatchFlags::None)
        , BatchLock(MakeShared<FCriticalSection>())
        , TickGroup(TG_PrePhysics)
        , AverageTickTimeNs(0.0f)
        , LastFrameTickCount(0)
        , bSortByCacheLocality(true)
    {}
    
    // Copy constructor - required for use in TMap
    FComponentTypeBatch(const FComponentTypeBatch& Other)
        : TypeName(Other.TypeName)
        , Flags(Other.Flags)
        , BatchLock(Other.BatchLock ? Other.BatchLock : MakeShared<FCriticalSection>())
        , TickEntities(Other.TickEntities)
        , BatchTickFunction(Other.BatchTickFunction)
        , TickGroup(Other.TickGroup)
        , AverageTickTimeNs(Other.AverageTickTimeNs)
        , LastFrameTickCount(Other.LastFrameTickCount)
        , bSortByCacheLocality(Other.bSortByCacheLocality)
    {}
    
    // Assignment operator - required for use in TMap
    FComponentTypeBatch& operator=(const FComponentTypeBatch& Other)
    {
        if (this != &Other)
        {
            TypeName = Other.TypeName;
            Flags = Other.Flags;
            BatchLock = Other.BatchLock ? Other.BatchLock : MakeShared<FCriticalSection>();
            TickEntities = Other.TickEntities;
            BatchTickFunction = Other.BatchTickFunction;
            TickGroup = Other.TickGroup;
            AverageTickTimeNs = Other.AverageTickTimeNs;
            LastFrameTickCount = Other.LastFrameTickCount;
            bSortByCacheLocality = Other.bSortByCacheLocality;
        }
        return *this;
    }
    
    // Returns whether the batch supports parallel ticking
    bool CanTickInParallel() const
    {
        return EnumHasAnyFlags(Flags, ETickBatchFlags::UseParallel);
    }
    
    // Tick the batch (sequential processing)
    void TickBatch(float DeltaTime);
    
    // Tick the batch (parallel processing)
    void TickBatchParallel(float DeltaTime);
    
    // Reorder entities based on cache locality
    void SortForCacheLocality();
};

/**
 * Spatial batch for groups of entities with spatial awareness.
 * Processes nearby entities together using a grid-based approach.
 */
USTRUCT()
struct ENHANCEDTICK_API FSpatialEntityBatch
{
    GENERATED_BODY()
    
    // Grid cell size
    float GridCellSize;
    
    // 3D grid structure (X, Y, Z)
    TMap<uint16, TArray<FTickEntityData*>> GridCells;
    
    // All spatial entities
    TArray<FTickEntityData*> AllSpatialEntities;
    
    // Lock for thread safety
    TSharedPtr<FCriticalSection> SpatialLock;
    
    FSpatialEntityBatch() 
        : GridCellSize(1000.0f)
        , SpatialLock(MakeShared<FCriticalSection>())
    {}
    
    // Copy constructor
    FSpatialEntityBatch(const FSpatialEntityBatch& Other)
        : GridCellSize(Other.GridCellSize)
        , GridCells(Other.GridCells)
        , AllSpatialEntities(Other.AllSpatialEntities)
        , SpatialLock(Other.SpatialLock ? Other.SpatialLock : MakeShared<FCriticalSection>())
    {}
    
    // Assignment operator
    FSpatialEntityBatch& operator=(const FSpatialEntityBatch& Other)
    {
        if (this != &Other)
        {
            GridCellSize = Other.GridCellSize;
            GridCells = Other.GridCells;
            AllSpatialEntities = Other.AllSpatialEntities;
            SpatialLock = Other.SpatialLock ? Other.SpatialLock : MakeShared<FCriticalSection>();
        }
        return *this;
    }
    
    // Calculate grid cell ID for a given position
    uint16 CalculateGridCell(const FVector& Position) const;
    
    // Add an entity to the spatial grouping system
    void AddEntity(FTickEntityData* Entity);
    
    // Remove an entity from the spatial grouping system
    void RemoveEntity(FTickEntityData* Entity);
    
    // Tick all grid cells (processing nearby ones together)
    void TickAllGrids(float DeltaTime);
    
    // Find all nearby entities based on position and radius
    TArray<FTickEntityData*> GetNearbyEntities(const FVector& Position, float Radius);
};

/**
 * Main Tick System
 * Advanced tick mechanism capable of batching both by type and spatial location.
 */
UCLASS(config=Engine, defaultconfig)
class ENHANCEDTICK_API UEnhancedTickSystem : public UWorldSubsystem, public FTickableGameObject
{
    GENERATED_BODY()
    
public:
    UEnhancedTickSystem();
    
    // USubsystem interface
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    
    // FTickableGameObject interface
    virtual void Tick(float DeltaTime) override;
    virtual TStatId GetStatId() const override;
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
    virtual bool IsTickableWhenPaused() const override { return false; }
    virtual bool IsTickableInEditor() const override { return false; }
    
    /**
     * Registers a single component.
     * @param Component - The component to be registered.
     * @param Flags - Tick behavior flags.
     * @param CustomTickTarget - Custom tick target (if null, default tick is used).
     * @param CustomTickFunction - Custom tick function name.
     */
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void RegisterComponent(UActorComponent* Component, ETickBatchFlags Flags = ETickBatchFlags::None, 
                           UObject* CustomTickTarget = nullptr, FName CustomTickFunction = NAME_None);
    
    /**
     * Registers a single actor.
     * @param Actor - The actor to be registered.
     * @param Flags - Tick behavior flags.
     * @param bIncludeComponents - Whether to also register all components of the actor.
     */
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void RegisterActor(AActor* Actor, ETickBatchFlags Flags = ETickBatchFlags::None, bool bIncludeComponents = true);
    
    /**
     * Automatically registers all components of a specified type.
     * @param ComponentClass - The component class to register.
     * @param Flags - Tick behavior flags.
     */
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void RegisterAllComponentsOfType(TSubclassOf<UActorComponent> ComponentClass, ETickBatchFlags Flags = ETickBatchFlags::None);
    
    // Unregistration functions
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void UnregisterComponent(UActorComponent* Component);
    
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void UnregisterActor(AActor* Actor, bool bIncludeComponents = true);
    
    // Various helper functions
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void SetDebugMode(bool bEnable, bool bVerbose = false);
    
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    void OptimizeBatches();
    
    // Retrieve profiling data
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    TMap<FString, float> GetBatchProfilingData() const;
    
    // Retrieve detailed statistical information
    UFUNCTION(BlueprintCallable, Category = "Enhanced Tick System")
    FString GetDetailedStats() const;

private:
    // Batches based on component type
    TMap<UClass*, FComponentTypeBatch> TypeBatches;
    
    // Spatial batches
    FSpatialEntityBatch SpatialBatch;
    
    // Batches sorted by tick groups
    TMap<ETickingGroup, TArray<FComponentTypeBatch*>> GroupedBatches;
    
    // Critical section lock
    TSharedPtr<FCriticalSection> BatchesLock;
    
    // Queues for deferred registration and unregistration
    TArray<TPair<UObject*, ETickBatchFlags>> PendingRegistrations;
    TArray<UObject*> PendingUnregistrations;
    
    // Frame counter for low priority ticks
    int32 FrameCounter;
    
    // Debug mode flags
    bool bDebugMode;
    bool bVerboseDebug;
    
    // Analyze current state for batch optimization
    void AnalyzeCurrentState();
    
    // Apply optimization hints for tick functions
    void ApplyOptimizationHints();
    
    // Determine the best tick function for a batch based on the component class
    TFunction<void(const TArrayView<FTickEntityData>&, float)> DetermineBestTickFunction(UClass* Class);
    
    // Execute batches based on tick groups
    void TickGroupBatches(ETickingGroup Group, float DeltaTime);
    
    // Process deferred operations
    void ProcessDeferredOperations();
    
    // Internal deferred operations processing (called within a lock)
    void ProcessDeferredOperationsImpl();
    
    // Helper function for batch profiling
    void UpdateBatchProfilingData(FComponentTypeBatch& Batch, float ExecutionTimeMs);
    
    // Check conditional tick objects
    void UpdateConditionalTicks();
    
    // AI optimization helpers for specialized component types
    void OptimizeCharacterMovementBatch(FComponentTypeBatch& Batch);
    void OptimizeAIPerceptionBatch(FComponentTypeBatch& Batch);
    
    // Calculate the appropriate spatial grid for an entity in a batch
    uint16 CalculateSpatialBucketId(const FVector& Position);
    
    // Sort batches by priority
    void SortBatchesByPriority();
    
    // Counters for statistical collection
    struct FTickStats
    {
        int32 TotalRegisteredEntities;
        int32 ActiveEntities;
        int32 ParallelBatchCount;
        int32 SpatialBatchCount;
        float TotalTickTimeMs;
        int32 CacheMissCount;
        
        FTickStats() 
          : TotalRegisteredEntities(0)
          , ActiveEntities(0)
          , ParallelBatchCount(0)
          , SpatialBatchCount(0)
          , TotalTickTimeMs(0.0f)
          , CacheMissCount(0)
        {}
    } Stats;
};
