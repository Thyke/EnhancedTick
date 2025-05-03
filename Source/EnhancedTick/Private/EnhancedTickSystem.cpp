// Copyright (C) Thyke 2025 All Rights Reserved.

#include "EnhancedTickSystem.h"
#include "GameFramework/Actor.h"
#include "Components/ActorComponent.h"
#include "DrawDebugHelpers.h"

// Stat defs
DEFINE_STAT(STAT_ETS_Total);
DEFINE_STAT(STAT_ETS_TypeBatches);
DEFINE_STAT(STAT_ETS_Spatial);
DEFINE_STAT(STAT_ETS_CacheMisses);

// SpatialBatch
uint16 FSpatialBatch::Calc(const FVector& P) const
{
    // We're using FMath::FloorToInt to first divide and then mask —
    // otherwise we would get a float '&' int error.
    const int32 X = FMath::FloorToInt(P.X / CellSize) & 0x3F; // 6‑bit X
    const int32 Y = FMath::FloorToInt(P.Y / CellSize) & 0x3F; // 6‑bit Y
    const int32 Z = FMath::FloorToInt(P.Z / CellSize) & 0xF;  // 4‑bit Z

    return uint16((X << 10) | (Y << 4) | Z);
}

void FSpatialBatch::Add(int32 Idx, uint16 Id)
{ 
    Grid.FindOrAdd(Id).Add(Idx);
} 

void FSpatialBatch::Remove(int32 Idx, uint16 Id)
{ 
    if (auto A = Grid.Find(Id))
    {
        A->Remove(Idx); 
        if (!A->Num()) 
            Grid.Remove(Id);
    }
}

// TypeBatch cache sort
void FComponentTypeBatch::SortCache(float Cell)
{ 
    if (!bCacheSort || Entities.Num() < 2) 
        return; 
    
    TArray<int32> Idx; 
    Idx.Reserve(Entities.Num()); 
    
    for (auto It = Entities.CreateConstIterator(); It; ++It) 
        Idx.Add(It.GetIndex()); 
    
    Idx.Sort([&](int32 A, int32 B){ 
        return Entities[A].Morton < Entities[B].Morton; 
    }); 
    
    TSparseArray<FTickEntityData> Sorted; 
    Sorted.Reserve(Entities.Num()); 
    
    for (int32 I : Idx) 
        Sorted.Add(Entities[I]); 
    
    Entities = MoveTemp(Sorted);
}

UEnhancedTickSystem::UEnhancedTickSystem()
{
}

void UEnhancedTickSystem::Initialize(FSubsystemCollectionBase&)
{ 
    CachedThreads = FPlatformMisc::NumberOfWorkerThreadsToSpawn(); 
    
    static ETickingGroup Gs[] = {
        TG_PrePhysics, 
        TG_StartPhysics, 
        TG_DuringPhysics, 
        TG_EndPhysics, 
        TG_PostPhysics, 
        TG_PostUpdateWork, 
        TG_LastDemotable
    }; 
    
    for (auto G : Gs) 
        Grouped.Add(G); 
    
    Spatial.CellSize = 2000.f; 
}

void UEnhancedTickSystem::Deinitialize()
{ 
    TypeBatches.Empty(); 
    Grouped.Empty(); 
}

// Registration
void UEnhancedTickSystem::RegisterComponent(UActorComponent* C, ETickBatchFlags F)
{ 
    if (!IsValid(C)) 
        return; 
    
    FScopeLock L(CS.Get()); 
    PendingReg.Add({C, F}); 
    C->PrimaryComponentTick.bCanEverTick = false; 
}

void UEnhancedTickSystem::RegisterActor(AActor* A, ETickBatchFlags F, bool bInc)
{ 
    if (!IsValid(A)) 
        return; 
    
    FScopeLock L(CS.Get()); 
    PendingReg.Add({A, F}); 
    
    if (bInc)
    {
        TArray<UActorComponent*> Comps;
        A->GetComponents(Comps); 
        
        for (auto* C : Comps)
        { 
            if (IsValid(C) && C->PrimaryComponentTick.bCanEverTick) 
                PendingReg.Add({C, F}); 
        }
    } 
    
    A->SetActorTickEnabled(false);
}

void UEnhancedTickSystem::UnregisterComponent(UActorComponent* C)
{ 
    if (!IsValid(C)) 
        return; 
    
    FScopeLock L(CS.Get()); 
    PendingUnreg.Add(C); 
    C->PrimaryComponentTick.bCanEverTick = true;
}

void UEnhancedTickSystem::UnregisterActor(AActor* A, bool bInc)
{ 
    if (!IsValid(A)) 
        return; 
    
    FScopeLock L(CS.Get()); 
    PendingUnreg.Add(A); 
    
    if (bInc)
    {
        TArray<UActorComponent*> Cs;
        A->GetComponents(Cs); 
        
        for (auto* C : Cs)
        { 
            if (IsValid(C))
            { 
                PendingUnreg.Add(C); 
                C->PrimaryComponentTick.bCanEverTick = true; 
            }
        }
    } 
    
    A->SetActorTickEnabled(true);
}

// Queue processing
void UEnhancedTickSystem::ProcessQueues()
{
    // Registrations
    for (auto& P : PendingReg)
    { 
        UObject* O = P.Key; 
        ETickBatchFlags F = P.Value; 
        UActorComponent* C = Cast<UActorComponent>(O); 
        AActor* A = C ? nullptr : Cast<AActor>(O); 
        UClass* Cls = O->GetClass(); 
        auto& B = TypeBatches.FindOrAdd(Cls);
        
        if (B.TypeName.IsEmpty())
        { 
            B.TypeName = Cls->GetName(); 
            B.TickGroup = C ? C->PrimaryComponentTick.TickGroup : A->PrimaryActorTick.TickGroup; 
            B.Flags = F; 
            Grouped[B.TickGroup].Add(&B); 
            B.BatchFn = [](const TSparseArray<FTickEntityData>& E, TArrayView<int32> Idx, float DT)
            { 
                for (int32 i : Idx)
                { 
                    const auto& En = E[i]; 
                    if (En.bEnabled && En.TickFn) 
                        En.TickFn(DT);
                }
            }; 
        }
        
        FTickEntityData D; 
        D.Object = O; 
        D.Position = C ? (C->GetOwner() ? C->GetOwner()->GetActorLocation() : FVector::ZeroVector) : A->GetActorLocation(); 
        D.GridId = Spatial.Calc(D.Position); 
        D.Morton = EncodeMorton3D(D.Position, Spatial.CellSize); 
        
        D.TickFn = [O](float DT)
        { 
            if (auto* Co = Cast<UActorComponent>(O)) 
                Co->TickComponent(DT, ELevelTick::LEVELTICK_All, nullptr); 
            else if (auto* Ac = Cast<AActor>(O)) 
                Ac->Tick(DT); 
        };
        
        int32 NewIdx = B.Entities.Add(MoveTemp(D)); 
        
        if (EnumHasAnyFlags(F, ETickBatchFlags::SpatialAware)) 
            Spatial.Add(NewIdx, D.GridId); 
    }
    
    PendingReg.Empty();
    
    // Unregistrations
    for (UObject* O : PendingUnreg)
    { 
        for (auto& Pair : TypeBatches)
        { 
            auto& Ents = Pair.Value.Entities; 
            
            for (auto It = Ents.CreateIterator(); It; ++It)
            { 
                if (It->Object == O)
                { 
                    Spatial.Remove(It.GetIndex(), It->GridId); 
                    Ents.RemoveAt(It.GetIndex()); 
                    break; 
                }
            }
        }
    }
    
    PendingUnreg.Empty(); 
}

void UEnhancedTickSystem::Tick(float DT)
{ 
    SCOPE_CYCLE_COUNTER(STAT_ETS_Total); 
    FrameCounter = (FrameCounter + 1) % 1000;
    
    { 
        FScopeLock L(CS.Get()); 
        ProcessQueues(); 
    }
    
    const float FPS = FApp::GetDeltaTime() > 0.f ? 1.f / FApp::GetDeltaTime() : 0.f;
    
    for (auto& Gr : Grouped)
    { 
        for (FComponentTypeBatch* B : Gr.Value)
        { 
            if (!B) 
                continue; 
                
            if (EnumHasAnyFlags(B->Flags, ETickBatchFlags::LowPrio) && FPS < 30.f && (FrameCounter % 3) != 0) 
                continue; 
                
            B->SortCache(Spatial.CellSize); 
            TickBatch(*B, DT);
        } 
    }
    
    TickSpatial(DT);
    
    if (FrameCounter % 300 == 0) 
        Optimize(); 
}

void UEnhancedTickSystem::TickBatch(FComponentTypeBatch& B, float DT)
{ 
    SCOPE_CYCLE_COUNTER(STAT_ETS_TypeBatches); 
    
    if (B.Entities.Num() == 0) 
        return; 
        
    TArray<int32, TInlineAllocator<256>> Idx; 
    
    for (auto It = B.Entities.CreateConstIterator(); It; ++It) 
        if (It->bEnabled) 
            Idx.Add(It.GetIndex()); 
            
    B.LastTickCount = Idx.Num(); 
    
    if (!Idx.Num()) 
        return; 
        
    double S = FPlatformTime::Seconds(); 
    
    if (B.CanParallel() && Idx.Num() > 16)
    { 
        ParallelFor(Idx.Num(), [&](int32 i)
        { 
            B.BatchFn(B.Entities, MakeArrayView(&Idx[i], 1), DT); 
        }, EParallelForFlags::Unbalanced);
    } 
    else 
    { 
        B.BatchFn(B.Entities, MakeArrayView(Idx), DT);
    } 
    
    double E = FPlatformTime::Seconds(); 
    B.AvgTickNs = float((E - S) * 1e9) / B.LastTickCount; 
}

void UEnhancedTickSystem::TickSpatial(float DT)
{ 
    SCOPE_CYCLE_COUNTER(STAT_ETS_Spatial); 
    
    for (auto& Pair : Spatial.Grid)
    { 
        for (int32 Idx : Pair.Value)
        { 
            for (auto& TB : TypeBatches)
            { 
                if (TB.Value.Entities.IsValidIndex(Idx))
                { 
                    auto& En = TB.Value.Entities[Idx]; 
                    if (En.bEnabled) 
                        En.TickFn(DT);
                } 
            } 
        } 
    } 
}

void UEnhancedTickSystem::Optimize()
{ 
    for (auto& P : TypeBatches)
    { 
        auto& B = P.Value; 
        
        if (B.AvgTickNs > 1500.f && B.Entities.Num() > 32) 
            B.Flags |= ETickBatchFlags::UseParallel; 
            
        if (B.Entities.Num() < 8) 
            B.bCacheSort = false; 
    } 
}