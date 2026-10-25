# Testing Floor SDK for Unreal Engine

Add Testing Floor telemetry and recording sync to an Unreal Engine game.

This is a native C++ plugin. Your game owns the client lifetime, configuration,
player and camera sources, and any QR overlay. Blueprint nodes and an automatic
settings or overlay UI are not included.

## Install

1. Copy this repository into your project's `Plugins/TestingFloor` directory:

   ```sh
   git clone https://github.com/capitalisminc/testingfloor-sdk-unreal.git Plugins/TestingFloor
   ```

2. Enable **Testing Floor SDK** in **Edit -> Plugins** and regenerate your project files.
3. Add `"TestingFloorSdk"` to your game module's `PrivateDependencyModuleNames`
   in its `.Build.cs`. Use a public dependency if your public headers expose SDK types.

The plugin uses Unreal's `Core`, `Json`, and `HTTP` modules and includes no game
assets. It was developed against Unreal Engine 5.8; other engine versions and
target platforms need verification in your project. Building requires an Unreal
C++ toolchain, including when you install it in a Blueprint project.

## Create a Client

Keep a `TSharedPtr<TestingFloor::FClient>` in your game instance or subsystem and
initialize it once per play session:

```cpp
#include "TestingFloor/TestingFloorClient.h"

TestingFloor::FSettings Settings;
Settings.WriteKey = WriteKey; // Load your game's Testing Floor write key.
Client = TestingFloor::FClient::Create(Settings);
```

The default factory ticks on Unreal's core ticker, including during pause, and
reads the current recorder handoff. It fills engine, OS, and locale metadata.
Collection is off in the editor by default: set `bEnableInEditor` to opt in during
integration. Automation always disables collection. Missing credentials or an
empty endpoint disable sends. Keep real write keys out of source control.

Supply optional `DeviceID` (a stable installation UUID persisted by your game),
`UserID` (your profile identity), `AppVersion`, and other metadata in `FSettings`.
The SDK generates a fresh `GetPlaySessionID()` for every client. No synthetic
session bookend events are emitted.

At the end of the play session, drain before releasing the client:

```cpp
Client->Shutdown(2.0);
Client.Reset();
```

Call client methods and transport completions on the game thread. Keep captured
objects alive or use weak pointers in callbacks. Shutdown stops the automatic
ticker and closes the client; create a new client for the next Play run.

Custom transports and deterministic hosts can use the advanced
`Create(Settings, Environment, Transport, Clock)` overload. That overload does
not register a ticker or discover host metadata. Call `Tick()` or
`Tick(monotonicSeconds)` yourself, including during pause, and pass the real
automation flag when a custom environment is used.

## Track Events

```cpp
Client->Track(TEXT("weapon_fire"), TestingFloor::FProperties()
    .Set(TEXT("weapon.id"), TEXT("plasma_rifle"))
    .Set(TEXT("damage"), 42));
```

`Track` returns whether the event entered the queue, not whether it reached the
server. Properties support strings, GUIDs, booleans, signed 32/64-bit integers,
finite floats/doubles, string arrays, and integer arrays. `SetIfPresent` omits
empty or whitespace-only strings. `SetVector` flattens vectors into `.x`, `.y`,
and `.z` properties. Nested objects are not supported. Keys beginning with `$`
are reserved.

Add shared game state to every event:

```cpp
Client->SetContextProvider([](TestingFloor::FProperties& Context) {
    Context.Set(TEXT("game_level.id"), TEXT("example-level"));
});
```

Keep providers inexpensive: they run synchronously when events are captured.
Event properties override shared context. Captured values, timestamps, and IDs
remain stable across retries. `SetEventProcessor` can finalize the merged
snapshot before queueing, for example to convert a game's coordinate frame.

## Player & Camera Context

Provide a snapshot from your player and camera code:

```cpp
Client->SetActivitySource([] {
    TestingFloor::FActivitySample Sample;
    Sample.PlayerPosition = FVector(100.0, 200.0, 0.0);
    Sample.CameraPosition = FVector(100.0, 200.0, 180.0);
    Sample.CameraRotation = FQuat::Identity;
    Sample.CameraFOV = 90.0;
    Sample.ViewportSize = FIntPoint(1920, 1080);
    return Sample;
});
```

Replace the example values with current game state; leave optional samples unset
when unavailable. Every event carries the available `player.position.*`,
`camera.position.*`, `camera.euler.*`, `camera.forward.*`, `camera.fov`, and
`viewport.width` / `viewport.height` fields. FOV is horizontal, with
`camera.fov_axis=horizontal`. `CameraEulerDegrees` can override quaternion-derived
Euler values.

Positions and teleport endpoints use native Unreal centimeters. Movement
thresholds and scalar movement/teleport distances use meters; angles use degrees.
For maps using the standard Unreal coordinate frame, choose X/Y projection axes,
100 world units per meter, and map images with +X to the right and +Y upward.

## Movement Events

After registering an activity source, opt into debounced movement events:

```cpp
Client->SetMovementTrackingEnabled(true);
```

`player_moved` reports `movement.reason` as `start`, `ping`, or `stop`. Movement
includes camera position, rotation, and FOV activity. Nonzero metrics include
`movement.distance`, `movement.camera_distance`, `movement.rotation_degrees`,
`movement.fov_delta`, and `movement.duration`. Ping metrics cover the segment
since the previous ping; stop metrics cover the full movement run.

Defaults target about two path samples per second. Tune `FMovementSettings` with
`SetMovementSettings`, or enable movement by default with
`FSettings::bMovementTracking`. Movement is off by default.

## Teleports

Wrap multi-frame teleports so intermediate samples do not draw a continuous path:

```cpp
const uint64 Token = Client->BeginTeleport(TEXT("respawn"));
// Move the player, potentially over multiple frames.
Client->EndTeleport(Token);
```

Use `CancelTeleport(Token)` if the operation is canceled. For an instantaneous
teleport, call `RecordTeleport(StartCentimeters, EndCentimeters, Reason)` after
moving. The SDK emits `player_teleported` with endpoints, distance, duration, and
an optional reason, then resets the movement baseline.

## Recording Sync

Recording identity is separate from the client's fresh per-play session ID.
`FRecordingContext::FromCommandLine` accepts `--testing-floor={"recording_uuid":"..."}`.
`FRecordingContext::Resolve` tries the command line first, then sidecar JSON
contents supplied by your game. The recorder owns sidecar file lifetime.
Handoffs with a timestamp more than 12 hours old are ignored.

For external video capture, `TestingFloor/TestingFloorQr.h` provides
`FQrPolicy`, `FQrHeartbeat`, and QR pixel generation. Your game renders the
overlay and ticks it during pause. QR is opt-in and defaults to an inverted,
continuously visible marker refreshed every 15 seconds. Set policy automation
and recorder suppression flags from the same environment and recording context
used by the client; `HasForceQrArgument` recognizes `--forceqr` / `-forceqr`.

```cpp
#include "TestingFloor/TestingFloorQr.h"

TestingFloor::FQrHeartbeat Heartbeat(Client->GetPlaySessionID());
TestingFloor::FQrSettings QrSettings;
QrSettings.bEnabled = true; // Set from FQrPolicy::IsEnabled() in your integration.
const FDateTime Now = FDateTime::UtcNow();
const auto& Frame = Heartbeat.Tick(QrSettings, FPlatformTime::Seconds(),
    Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond());
if (Frame.bVisible)
{
    const auto Pixels = Frame.Matrix.MakePixels(4, QrSettings.bInverted);
    // Upload Pixels to your overlay texture. Cache it until Frame.Sequence changes.
}
```

Retain the heartbeat across frames. Pixels are opaque, row-major, and start at
the upper left. The square dimension is `(Frame.Matrix.Size + 8) * ModuleScale`,
including the four-module quiet zone. The SDK supplies neither an overlay widget
nor recorder process control.

## Performance & Delivery

- Events queue in memory; avoid per-frame event spam and large properties.
- Batches default to 50 events or a 0.25-second flush interval, with one request
  in flight. `Flush()` requests an immediate send when backoff permits.
- Events over 32 KiB are rejected, batches stay below 1 MiB, and the default queue
  holds 4,096 events including requests in flight. A full queue drops new events.
- Network errors, HTTP 408/429, and server errors retry with backoff. HTTP 401/403
  disable the session; other invalid client requests are dropped.
- `Shutdown` is best effort and clamps its wait to five seconds. There is no disk
  spool; a crash, queue overflow, or expired shutdown deadline can lose events.
- Inspect `GetState`, `GetPendingEventCount`, and `GetDroppedEventCount` for diagnostics.

## Development

See [SDK_CONTRACT.md](SDK_CONTRACT.md) for the contract shared with the
[Unity SDK](https://github.com/capitalisminc/testingfloor-sdk-unity).
See [CONTRIBUTING.md](CONTRIBUTING.md) for release checks, Unreal automation tests,
and source packaging.

## License

MIT. See [LICENSE.md](LICENSE.md) and [ThirdPartyNotices.md](ThirdPartyNotices.md).
