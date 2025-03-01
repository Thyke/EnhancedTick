# Enhanced Tick System

*Read this in other languages: [English](#enhanced-tick-system-english), [Turkish](#enhanced-tick-system-türkçe)*

## Enhanced Tick System (English)

This system provides an optimized tick management solution for Unreal Engine, allowing for cache-coherent batching of tick operations to significantly improve performance in scenes with many ticking components, especially for AI characters, physics objects, and other CPU-intensive systems.

### Features

- Cache-optimized, batched tick processing for improved CPU performance
- Type-based grouping of similar components for better cache coherency
- Spatial awareness for location-based optimization
- Automatic thread safety detection for transform-related components
- Customizable tick priorities and conditional ticking
- Support for multi-threading of safe tick operations
- Detailed profiling and performance monitoring tools

### Installation

1. Copy the module to your project's "Source" directory
2. Add the module dependency to your project's `.Build.cs` file:
   ```csharp
   PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedTick" });
   ```
3. Rebuild your project
4. The `UEnhancedTickSystem` will be available as a WorldSubsystem

### Usage

#### Basic Usage

```cpp
// In your GameMode or other appropriate initialization class
void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();
    
    // Get the Enhanced Tick System
    UEnhancedTickSystem* TickSystem = GetWorld()->GetSubsystem<UEnhancedTickSystem>();
    if (!TickSystem)
        return;
    
    // Register all CharacterMovementComponents for optimized ticking
    TickSystem->RegisterAllComponentsOfType(UCharacterMovementComponent::StaticClass());
    
    // Register a specific actor for optimized ticking
    TickSystem->RegisterActor(MyActor);
}
```

#### Advanced Usage

```cpp
// Register components with specific flags
TickSystem->RegisterComponent(Component, ETickBatchFlags::SpatialAware | ETickBatchFlags::LowPrio);

// Register a collection of AI component
TArray<UActorComponent*> AIComponents;
for (TActorIterator<AAIController> It(GetWorld()); It; ++It)
{
    AAIController* Controller = *It;
    if (Controller && Controller->GetPawn())
    {
        ACharacter* Character = Cast<ACharacter>(Controller->GetPawn());
        if (Character && Character->GetCharacterMovement())
        {
            AIComponents.Add(Character->GetCharacterMovement());
        }
    }
}
TickSystem->RegisterTickableComponents(AIComponents);

// Run performance optimization
TickSystem->OptimizeBatches();

// Show debug information
TickSystem->SetDebugMode(true, false);
```

#### Blueprint Usage

You can also use the system from Blueprints:

1. Get the Enhanced Tick System:
   ```
   Get World Subsystem (Class: Enhanced Tick System)
   ```

2. Register components/actors:
   ```
   Register Tickable Component
   Register All Components Of Type
   Register Tickable Actor
   ```

3. Use other functions like `OptimizeBatches` for further improvements.

### TickBatch Flags

The following flags are available for customizing tick behavior:

- **UseParallel**: Allows the component to be ticked in parallel (if thread-safe)
- **CacheHot**: Marks the batch as frequently accessed data for cache optimization
- **Conditional**: Only ticks when specific conditions are met
- **HighPrio**: High-priority ticking (processed first)
- **LowPrio**: Low-priority ticking (can be skipped in heavy frames)
- **SpatialAware**: Uses spatial organization for location-based optimizations
- **StateDependent**: Batches entities based on their internal state

### Performance Recommendations

- Group similar components together for better cache coherency
- Use `SpatialAware` for components that interact with nearby objects
- Apply `LowPrio` to components that don't need updates every frame
- Avoid registering components that need precise and immediate tick responses
- For best results, register components at the beginning of gameplay
- Consider unregistering rarely active components and re-registering when needed

### Technical Details

#### CPU Cache Optimization

The system organizes components by type and state to maximize CPU cache hit rates. This significantly reduces cache misses which are a major performance bottleneck in modern CPUs. For example, when processing CharacterMovementComponents, all components in a similar movement state (walking, falling, etc.) are processed together to keep related code and data in the CPU cache.

#### Thread Safety

The system automatically detects components that are not thread-safe (like USceneComponent and derivatives) and processes them in a single thread. Other components can be safely parallelized for additional performance gains on multi-core systems.

#### Spatial Organization

For spatially-aware components, the system organizes them in a grid system, processing nearby objects together. This improves performance for systems like AI perception, physics, and other location-based interactions.

#### Performance Statistics

You can access detailed performance metrics using:

```cpp
FString Stats = TickSystem->GetDetailedStats();
UE_LOG(LogTemp, Display, TEXT("Tick System Stats: %s"), *Stats);
```

This provides information on batch counts, processing time, active entities, and cache performance.

### Examples

#### Optimizing AI Characters

```cpp
void AGameMode::OptimizeAICharacters()
{
    UEnhancedTickSystem* TickSystem = GetWorld()->GetSubsystem<UEnhancedTickSystem>();
    if (!TickSystem)
        return;
        
    // Find all AI controllers
    TArray<UCharacterMovementComponent*> MovementComponents;
    for (TActorIterator<AAIController> It(GetWorld()); It; ++It)
    {
        AAIController* AIController = *It;
        if (AIController && AIController->GetPawn())
        {
            if (ACharacter* Character = Cast<ACharacter>(AIController->GetPawn()))
            {
                if (UCharacterMovementComponent* CMC = Character->GetCharacterMovement())
                {
                    MovementComponents.Add(CMC);
                }
            }
        }
    }
    
    // Register all movement components
    for (UCharacterMovementComponent* CMC : MovementComponents)
    {
        TickSystem->RegisterComponent(CMC, ETickBatchFlags::SpatialAware);
    }
    
    UE_LOG(LogTemp, Display, TEXT("Optimized %d AI character movement components"), MovementComponents.Num());
}
```

#### Dynamic Priority Management

```cpp
void ACombatManager::UpdateTickPriorities()
{
    UEnhancedTickSystem* TickSystem = GetWorld()->GetSubsystem<UEnhancedTickSystem>();
    if (!TickSystem)
        return;
        
    // Characters in combat get high priority
    for (AActor* CombatActor : ActorsInCombat)
    {
        ACharacter* Character = Cast<ACharacter>(CombatActor);
        if (Character && Character->GetCharacterMovement())
        {
            TickSystem->UnregisterComponent(Character->GetCharacterMovement());
            TickSystem->RegisterComponent(Character->GetCharacterMovement(), 
                ETickBatchFlags::HighPrio | ETickBatchFlags::SpatialAware);
        }
    }
    
    // Characters far from player get low priority
    for (AActor* FarActor : ActorsFarFromPlayer)
    {
        ACharacter* Character = Cast<ACharacter>(FarActor);
        if (Character && Character->GetCharacterMovement())
        {
            TickSystem->UnregisterComponent(Character->GetCharacterMovement());
            TickSystem->RegisterComponent(Character->GetCharacterMovement(), 
                ETickBatchFlags::LowPrio | ETickBatchFlags::SpatialAware);
        }
    }
    
    // Optimize after changes
    TickSystem->OptimizeBatches();
}
```

---

## Enhanced Tick System (Türkçe)

Bu sistem, Unreal Engine için CPU önbellek uyumlu tick gruplandırma yönetimi sağlayarak, özellikle AI karakterler, fizik nesneleri ve diğer CPU yoğun sistemler için performansı önemli ölçüde artıran optimize edilmiş bir çözüm sunar.

### Özellikler

- Gelişmiş CPU performansı için önbellek-optimize edilmiş toplu tick işleme
- Benzer bileşenlerin daha iyi önbellek uyumluluğu için tip-tabanlı gruplandırma
- Konum tabanlı optimizasyon için uzamsal farkındalık
- Transform ile ilgili bileşenler için otomatik thread güvenliği algılama
- Özelleştirilebilir tick öncelikleri ve koşullu tick
- Güvenli tick işlemleri için çok iş parçacığı desteği
- Detaylı profilleme ve performans izleme araçları

### Kurulum

1. Modülü projenizin "Source" dizinine kopyalayın
2. Projenizin `.Build.cs` dosyasına modül bağımlılığını ekleyin:
   ```csharp
   PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedTick" });
   ```
3. Projenizi yeniden derleyin
4. `UEnhancedTickSystem` bir WorldSubsystem olarak kullanılabilir olacaktır

### Kullanım

#### Temel Kullanım

```cpp
// GameMode veya başka uygun bir başlatma sınıfında
void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();
    
    // Enhanced Tick System'i al
    UEnhancedTickSystem* TickSystem = GetWorld()->GetSubsystem<UEnhancedTickSystem>();
    if (!TickSystem)
        return;
    
    // Tüm CharacterMovementComponent'leri optimize edilmiş tick için kaydet
    TickSystem->RegisterAllComponentsOfType(UCharacterMovementComponent::StaticClass());
    
    // Belirli bir aktörü optimize edilmiş tick için kaydet
    TickSystem->RegisterActor(MyActor);
}
```

#### İleri Düzey Kullanım

```cpp
// Belirli bayraklarla bileşenleri kaydet
TickSystem->RegisterComponent(Component, ETickBatchFlags::SpatialAware | ETickBatchFlags::LowPrio);

// Bir AI bileşen koleksiyonunu kaydet
TArray<UActorComponent*> AIComponents;
for (TActorIterator<AAIController> It(GetWorld()); It; ++It)
{
    AAIController* Controller = *It;
    if (Controller && Controller->GetPawn())
    {
        ACharacter* Character = Cast<ACharacter>(Controller->GetPawn());
        if (Character && Character->GetCharacterMovement())
        {
            AIComponents.Add(Character->GetCharacterMovement());
        }
    }
}
TickSystem->RegisterTickableComponents(AIComponents);

// Performans optimizasyonu çalıştır
TickSystem->OptimizeBatches();

// Hata ayıklama bilgilerini göster
TickSystem->SetDebugMode(true, false);
```

#### Blueprint Kullanımı

Sistemi Blueprint'lerden de kullanabilirsiniz:

1. Enhanced Tick System'i alın:
   ```
   Get World Subsystem (Class: Enhanced Tick System)
   ```

2. Bileşenleri/aktörleri kaydedin:
   ```
   Register Tickable Component
   Register All Components Of Type
   Register Tickable Actor
   ```

3. Daha fazla iyileştirme için `OptimizeBatches` gibi diğer fonksiyonları kullanın.

### TickBatch Bayrakları

Tick davranışını özelleştirmek için aşağıdaki bayraklar kullanılabilir:

- **UseParallel**: Bileşenin paralel olarak tick edilmesine izin verir (thread güvenli ise)
- **CacheHot**: Batch'i önbellek optimizasyonu için sık erişilen veri olarak işaretler
- **Conditional**: Sadece belirli koşullar karşılandığında tick eder
- **HighPrio**: Yüksek öncelikli tick (önce işlenir)
- **LowPrio**: Düşük öncelikli tick (yoğun frame'lerde atlanabilir)
- **SpatialAware**: Konum tabanlı optimizasyonlar için uzamsal organizasyon kullanır
- **StateDependent**: Varlıkları iç durumlarına göre gruplandırır

### Performans Önerileri

- Daha iyi önbellek uyumluluğu için benzer bileşenleri birlikte gruplandırın
- Yakındaki nesnelerle etkileşime giren bileşenler için `SpatialAware` kullanın
- Her frame'de güncelleme gerektirmeyen bileşenler için `LowPrio` uygulayın
- Hassas ve anında tick yanıtı gerektiren bileşenleri kaydetmekten kaçının
- En iyi sonuçlar için, bileşenleri oyunun başlangıcında kaydedin
- Nadiren aktif olan bileşenleri sistemden çıkarıp gerektiğinde yeniden kaydetmeyi düşünün

### Teknik Detaylar

#### CPU Önbellek Optimizasyonu

Sistem, CPU önbellek isabet oranlarını en üst düzeye çıkarmak için bileşenleri tür ve duruma göre düzenler. Bu, modern CPU'larda önemli bir performans darboğazı olan önbellek ıskalamalarını önemli ölçüde azaltır. Örneğin, CharacterMovementComponent işlenirken, benzer hareket durumundaki (yürüme, düşme vb.) tüm bileşenler, ilgili kod ve verileri CPU önbelleğinde tutmak için birlikte işlenir.

#### Thread Güvenliği

Sistem, USceneComponent ve türevleri gibi thread-güvenli olmayan bileşenleri otomatik olarak algılar ve bunları tek bir thread'de işler. Diğer bileşenler, çok çekirdekli sistemlerde ek performans kazanımları için güvenli bir şekilde paralelleştirilebilir.

#### Uzamsal Organizasyon

Uzamsal farkındalığa sahip bileşenler için sistem, onları bir grid sisteminde düzenleyerek yakındaki nesneleri birlikte işler. Bu, AI algılama, fizik ve diğer konum tabanlı etkileşimler için performansı artırır.

#### Performans İstatistikleri

Aşağıdaki kodu kullanarak detaylı performans metriklerine erişebilirsiniz:

```cpp
FString Stats = TickSystem->GetDetailedStats();
UE_LOG(LogTemp, Display, TEXT("Tick System Stats: %s"), *Stats);
```

Bu, batch sayıları, işlem süresi, aktif varlıklar ve önbellek performansı hakkında bilgi sağlar.

### Örnekler

#### AI Karakterlerini Optimize Etme

```cpp
void AGameMode::OptimizeAICharacters()
{
    UEnhancedTickSystem* TickSystem = GetWorld()->GetSubsystem<UEnhancedTickSystem>();
    if (!TickSystem)
        return;
        
    // Tüm AI kontrolcüleri bul
    TArray<UCharacterMovementComponent*> MovementComponents;
    for (TActorIterator<AAIController> It(GetWorld()); It; ++It)
    {
        AAIController* AIController = *It;
        if (AIController && AIController->GetPawn())
        {
            if (ACharacter* Character = Cast<ACharacter>(AIController->GetPawn()))
            {
                if (UCharacterMovementComponent* CMC = Character->GetCharacterMovement())
                {
                    MovementComponents.Add(CMC);
                }
            }
        }
    }
    
    // Tüm hareket bileşenlerini kaydet
    for (UCharacterMovementComponent* CMC : MovementComponents)
    {
        TickSystem->RegisterComponent(CMC, ETickBatchFlags::SpatialAware);
    }
    
    UE_LOG(LogTemp, Display, TEXT("%d AI karakter hareket bileşeni optimize edildi"), MovementComponents.Num());
}
```

#### Dinamik Öncelik Yönetimi

```cpp
void ACombatManager::UpdateTickPriorities()
{
    UEnhancedTickSystem* TickSystem = GetWorld()->GetSubsystem<UEnhancedTickSystem>();
    if (!TickSystem)
        return;
        
    // Çatışmadaki karakterler yüksek öncelik alır
    for (AActor* CombatActor : ActorsInCombat)
    {
        ACharacter* Character = Cast<ACharacter>(CombatActor);
        if (Character && Character->GetCharacterMovement())
        {
            TickSystem->UnregisterComponent(Character->GetCharacterMovement());
            TickSystem->RegisterComponent(Character->GetCharacterMovement(), 
                ETickBatchFlags::HighPrio | ETickBatchFlags::SpatialAware);
        }
    }
    
    // Oyuncudan uzak karakterler düşük öncelik alır
    for (AActor* FarActor : ActorsFarFromPlayer)
    {
        ACharacter* Character = Cast<ACharacter>(FarActor);
        if (Character && Character->GetCharacterMovement())
        {
            TickSystem->UnregisterComponent(Character->GetCharacterMovement());
            TickSystem->RegisterComponent(Character->GetCharacterMovement(), 
                ETickBatchFlags::LowPrio | ETickBatchFlags::SpatialAware);
        }
    }
    
    // Değişikliklerden sonra optimize et
    TickSystem->OptimizeBatches();
}
```