// Copyright (C) Thyke 2025 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Engine/EngineBaseTypes.h"
#include "Stats/Stats.h"
#include "EngineUtils.h"
#include "EnhancedTickSystem.generated.h"


// Forward decls for traits
class USceneComponent;
class UPrimitiveComponent;
class UCharacterMovementComponent;



//  Stats
DECLARE_STATS_GROUP(TEXT("EnhancedTickSystem"), STATGROUP_EnhancedTick, STATCAT_Advanced);
DECLARE_CYCLE_STAT_EXTERN(TEXT("ETS – Total"),            STAT_ETS_Total,            STATGROUP_EnhancedTick, ENHANCEDTICK_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("ETS – TypeBatches"),       STAT_ETS_TypeBatches,      STATGROUP_EnhancedTick, ENHANCEDTICK_API);
DECLARE_CYCLE_STAT_EXTERN(TEXT("ETS – Spatial"),           STAT_ETS_Spatial,          STATGROUP_EnhancedTick, ENHANCEDTICK_API);
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("ETS – CacheMisses"),STAT_ETS_CacheMisses,      STATGROUP_EnhancedTick, ENHANCEDTICK_API);

//  Flags
UENUM(BlueprintType, meta=(Bitflags))
enum class ETickBatchFlags : uint8
{
    None            = 0,
    UseParallel     = 1 << 0,
    CacheHot        = 1 << 1,
    Conditional     = 1 << 2,
    HighPrio        = 1 << 3,
    LowPrio         = 1 << 4,
    SpatialAware    = 1 << 5,
    StateDependent  = 1 << 6
};
ENUM_CLASS_FLAGS(ETickBatchFlags);

/* Helper – compile‑time thread‑safety trait (defaults to true) */
template<typename T> struct TThreadSafeTick : TIntegralConstant<bool, true> {};
template<> struct TThreadSafeTick<USceneComponent> : TIntegralConstant<bool, false> {};
template<> struct TThreadSafeTick<UPrimitiveComponent> : TIntegralConstant<bool, false> {};
template<> struct TThreadSafeTick<UCharacterMovementComponent> : TIntegralConstant<bool, false> {};

/* Morton 3‑D helper (10 bits per axis → 30‑bit morton) */
FORCEINLINE uint32 EncodeMorton3D(const FVector& P, float Cell)
{
    const FVector V = (P / Cell).GetAbs();
    auto Part = [](uint32 v){ 
        v = (v | (v << 16)) & 0x030000FF; 
        v = (v | (v << 8)) & 0x0300F00F; 
        v = (v | (v << 4)) & 0x030C30C3; 
        v = (v | (v << 2)) & 0x09249249; 
        return v; 
    };
    return (Part((uint32)V.X) << 2) | (Part((uint32)V.Y) << 1) | Part((uint32)V.Z);
}

/* Entity data */
struct FTickEntityData
{
    UObject*                         Object = nullptr;
    TFunction<void(float)>           TickFn;
    FVector                          Position = FVector::ZeroVector;
    uint16                           GridId = 0;
    uint32                           Morton = 0;
    uint8                            Priority = 128;
    bool                             bEnabled = true;
    bool                             bDirty   = true;   // transform changed
};

/* Type Batch */
struct FComponentTypeBatch
{
    FString                          TypeName;
    ETickBatchFlags                  Flags = ETickBatchFlags::None;
    TSparseArray<FTickEntityData>    Entities;     // pointer‑stable
    TFunction<void(const TSparseArray<FTickEntityData>&, TArrayView<int32>, float)> BatchFn;
    TSharedPtr<FCriticalSection>     Lock = MakeShared<FCriticalSection>();
    ETickingGroup                    TickGroup = TG_PrePhysics;
    float                            AvgTickNs = 0.f;
    int32                            LastTickCount = 0;
    bool                             bCacheSort = true;

    bool CanParallel() const { return EnumHasAnyFlags(Flags, ETickBatchFlags::UseParallel); }
    void SortCache(float CellSize);
};

/* Spatial Grid */
struct FSpatialBatch
{
    float                            CellSize = 2000.f;
    TSharedPtr<FCriticalSection>     Lock = MakeShared<FCriticalSection>();
    TMap<uint16, TArray<int32>>      Grid;           // gridId -> entity indices

    uint16 Calc(const FVector& P) const;
    void   Add(int32 Idx, uint16 GridId);
    void   Remove(int32 Idx, uint16 GridId);
};

/* Tick System */
UCLASS(config=Engine, defaultconfig)
class ENHANCEDTICK_API UEnhancedTickSystem final : public UWorldSubsystem, public FTickableGameObject
{
    GENERATED_BODY()
public:
    UEnhancedTickSystem();

    // WorldSubsystem
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    // FTickableGameObject
    virtual void Tick(float DT) override;
    virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UEnhancedTickSystem, STATGROUP_EnhancedTick); }
    virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }

    // API
    UFUNCTION(BlueprintCallable)
    void RegisterComponent(UActorComponent* Comp, ETickBatchFlags Flags = ETickBatchFlags::None);
    UFUNCTION(BlueprintCallable)
    void RegisterActor(AActor* Actor, ETickBatchFlags Flags = ETickBatchFlags::None, bool bIncludeComponents = true);
    UFUNCTION(BlueprintCallable)
    void UnregisterComponent(UActorComponent* Comp);
    UFUNCTION(BlueprintCallable)
    void UnregisterActor(AActor* Actor, bool bIncludeComponents = true);

private:
    void  ProcessQueues();
    void  TickBatch(FComponentTypeBatch& Batch, float DT);
    void  TickSpatial(float DT);
    void  Optimize();

    TMap<UClass*, FComponentTypeBatch>                 TypeBatches;
    TMap<ETickingGroup, TArray<FComponentTypeBatch*>>  Grouped;
    FSpatialBatch                                      Spatial;

    TArray<TPair<UObject*, ETickBatchFlags>>           PendingReg;
    TArray<UObject*>                                   PendingUnreg;

    TSharedPtr<FCriticalSection>                       CS = MakeShared<FCriticalSection>();

    int32 CachedThreads = 1;
    int32 FrameCounter  = 0;
};