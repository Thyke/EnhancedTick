# TickAggregator for Unreal Engine

TickAggregator is a performance optimization subsystem for Unreal Engine that efficiently manages and consolidates tick functions. By grouping actors and components based on their tick groups, it allows for more efficient processing, leading to improved performance in scenes with many tickable objects.

## Features

- **Efficient Tick Management**: Consolidates tick functions of actors and components.
- **Tick Group Optimization**: Organizes objects into predefined Unreal Engine tick groups for optimal update order.
- **Performance Improvement**: Reduces overhead in scenes with numerous tickable objects.
- **Easy Integration**: Implements as a WorldSubsystem for seamless integration with existing Unreal Engine projects.
- **Debug Visualization**: Includes on-screen debug messages for tick group activities.
- **Thread-Safe Operations**: Utilizes critical sections for thread-safe object management.

## Installation

1. Copy the `TickAggregator` folder into your Unreal Engine project's `Plugins` directory.
2. Rebuild your project.
3. Enable the TickAggregator plugin in your project settings.

## Usage

### Accessing the Subsystem

```cpp
UTickAggregatorSubsystem* TickAggregator = GetWorld()->GetSubsystem<UTickAggregatorSubsystem>();
```

### Registering Tickable Objects

For individual actors or components:

```cpp
// Register a single actor
TickAggregator->RegisterTickableActor(YourActor);

// Register a single component
TickAggregator->RegisterTickableComponent(YourComponent);
```

For multiple actors or components:

```cpp
// Register multiple actors
TArray<AActor*> ActorsToRegister;
// ... populate ActorsToRegister ...
TickAggregator->RegisterTickableActors(ActorsToRegister);

// Register multiple components
TArray<UActorComponent*> ComponentsToRegister;
// ... populate ComponentsToRegister ...
TickAggregator->RegisterTickableComponents(ComponentsToRegister);
```

### Unregistering Tickable Objects

```cpp
TickAggregator->UnregisterTickableActor(YourActor);
TickAggregator->UnregisterTickableComponent(YourComponent);
```

### Enabling Debug Visualization

```cpp
TickAggregator->ShowDebug(true);
```

## Tick Groups

TickAggregator uses Unreal Engine's built-in tick groups:

- TG_PrePhysics
- TG_StartPhysics
- TG_DuringPhysics
- TG_EndPhysics
- TG_PostPhysics
- TG_PostUpdateWork
- TG_LastDemotable

Objects are automatically assigned to these groups based on their existing tick group settings.

## Best Practices

- Register objects that don't require independent tick control for best performance gains.
- Use the debug visualization to monitor tick group activities and identify potential optimizations.
- Be mindful of thread safety when registering or unregistering objects from multiple threads.

## Performance Considerations

- TickAggregator uses Stats system for performance tracking. Monitor `STATGROUP_TickAggregator` for detailed performance metrics.
- The subsystem automatically disables individual tick on registered actors and components to prevent duplicate ticking.

## License

This project is licensed under the MIT License - see the [LICENSE.md](LICENSE.md) file for details.

