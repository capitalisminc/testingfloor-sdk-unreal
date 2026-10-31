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
Ordinary editor collection is off by default. `bEnableInEditorWhileRecording`
defaults to true, so a live desktop recording can temporarily enable collection
without enabling ordinary editor analytics. `bEnableInEditor` still opts into
always-on editor collection. Set both options false for no editor telemetry.
Recording never overrides `bEnabled=false`, automation restrictions, missing
credentials, or an empty endpoint. Keep real write keys out of source control.

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
`Create(Settings, Environment, Transport, Clock, RecordingSource)` overload.
Call `Tick()` or `Tick(monotonicSeconds)` yourself, including during pause, and
pass the real automation flag. This overload does not sample gameplay through an
automatic ticker or discover host metadata. Without an injected
`RecordingSource`, it installs a separate context-only monitor in editor,
standalone and packaged hosts, so a host that gates its gameplay tick on
`GetState()` can still attach a recording.
With an injected source, call `PollRecordingContext()` or `Tick()` from your host;
tests can drive the clock and context without reading the real project handoff.

## Record an existing editor Play run

The SDK checks the selected project's recording handoff every 250 ms while Play
is running, including when collection is dormant or gameplay is paused. Starting
recording can attach to an existing Play run, and stopping and starting another
recording does not require another Play boot. The Play-session ID stays stable;
each event captures its own recording UUID before queueing.

The desktop app publishes a renewable lease with `lease_version:1`,
`telemetry_requested:true`, `lease_expires_at_unix_ms`, and `telemetry_directory`.
Only an unexpired requested lease can enable recording-only editor collection.
A static legacy handoff can still associate events when ordinary editor
collection is allowed, but cannot enable editor-off collection. A lease sidecar
supersedes a launch argument so a new recording can replace the old one.

Attachment registers the Play run and sends full state and live-object
keyframes. Stop closes the association, drains accepted records, and sends its
completion marker before the next recording's records. Lease expiry suspends
new recording telemetry; renewal starts from fresh keyframes and preserves
`interrupted:true` in local status and final completion. A final completed pair
of recording UUID and Play-session ID never reopens.

When supplied, `editor_pid` must match the current process for an interactive
editor. Standalone child Play processes use the project-scoped handoff without
that direct PID comparison; hosts must scope which project/children they launch.
The SDK does not collect gameplay from editor-only work outside a Play run.

`<session-id>.status.json` in the telemetry directory reports attachment,
suspension, closure or the policy/configuration preventing attachment. Its
capabilities are `recording_lease_v1` and `dynamic_recording_attachment`.
This acknowledges local SDK participation, not warehouse delivery. Use the
existing completion barrier to determine delivery. No recording modifies saved
SDK settings. `GetRecordingContext()` exposes only an attached telemetry
association. Use the fresh `GetRecordingHandoff()` for independent capture
policy such as QR suppression, even when telemetry is disabled or unconfigured.

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

Add small values that describe each moment, such as level, health and game
clock, to every event:

```cpp
Client->SetContextProvider([](TestingFloor::FProperties& Context) {
    Context.Set(TEXT("game_level.id"), TEXT("example-level"));
});
```

Keep providers inexpensive: they run synchronously when events are captured.
Event properties override shared context. Captured values, timestamps, and IDs
remain stable across retries. `SetEventProcessor` can finalize the merged
snapshot before queueing, for example to convert a game's coordinate frame.
Keep values that row-level queries filter by, such as `game_level.id` and the
game clock, in the context provider.

## Session State

Values that stay true until they change, such as settings, device, build,
inventory and outfit, belong in a state provider instead:

```cpp
Client->SetStateProvider([](TestingFloor::FProperties& State) {
    State.Set(TEXT("pref.vsync"), true).Set(TEXT("player.outfit.id"), TEXT("example-outfit"));
});
Client->SetState(TEXT("difficulty"), TEXT("hard")); // Or set one value directly.
Client->ClearState(TEXT("difficulty"));
```

The SDK reads state before each event and once a second, including while
gameplay is paused. When a value changes, it queues a `$tf_state` record with
only the changed keys before the event, so the state at every event is exact. A
key that is no longer supplied is sent as `null`. `SetState` values override the
provider's value for the same key and accept the same types as `FProperties`.

A keyframe repeats every current value. The SDK sends one at the start of the
play session, every recording attachment or renewal, every 5 minutes, and
after it drops any record. The SDK's own `viewport.width`, `viewport.height` and
`camera.fov_axis` are state. Testing Floor shows each event with the state that
was current at that moment. A game without a state provider keeps all of its
context on every event.

## Tracked Objects

Register NPCs, props, vehicles and projectiles to draw their paths on the map:

```cpp
const TWeakObjectPtr<AActor> Weak = Guard;
const uint64 Token = Client->TrackObject(TEXT("guard"), [Weak]() -> TOptional<FVector> {
    return Weak.IsValid() ? TOptional<FVector>(Weak->GetActorLocation()) : TOptional<FVector>();
}, TEXT("captain_example"));
Client->SetObjectState(Token, TEXT("behavior"), TEXT("searching"));
Client->RemoveObject(Token, TEXT("destroyed"));
```

Pass a stable ID when your events refer to the object; otherwise the SDK assigns
`kind#n`, counting per kind for each play session. Hold engine objects through
weak pointers. A position source that returns unset removes the object with
reason `gone`. Pass `destroyed` when the engine object is destroyed and
`disabled` when tracking is turned off; any other reason string is passed
through. Registering a live ID again replaces that object.

The SDK checks live objects 4 times a second in unscaled time
(`FSettings::ObjectChecksPerSecond`, 1–10). It sends a sample when an object is
registered and afterwards only when the object has moved at least 0.25 m
(25 cm). Removal sends the final position and the reason. Object state values
are scalars, and only changed values are sent. Every 5 minutes, and after a drop,
a keyframe repeats every live object with its position and state.

Samples, state changes and removals from one check are batched into
`$tf_objects` records of parallel arrays, with positions in native centimeters
rounded to 1 cm and at most 300 samples per record. Records copy the
`game_level.*` values of the most recent event. `FSettings::MaxTrackedObjects`
(default 256) limits live objects; further registrations return 0 and log one
warning. While editor collection is dormant, registrations remain available without
sampling; each recording starts from the live objects. Disabled/unconfigured
integrations return 0. Handles of 0 or removed objects do nothing.

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
`camera.position.*`, `camera.euler.*`, `camera.forward.*`, and `camera.fov`
fields. FOV is horizontal: `camera.fov_axis=horizontal` and `viewport.width` /
`viewport.height` are session state. `CameraEulerDegrees` can override
quaternion-derived Euler values.

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
`FRecordingContext::Resolve` prefers a versioned lease sidecar, then falls back
to the command line and legacy sidecar JSON supplied by your game. The desktop
app owns sidecar file lifetime. Static handoffs older than 12 hours are ignored;
a live renewable lease remains valid beyond that static age limit.

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
  holds 4,096 records including requests in flight. A full queue drops new records.
  `$tf_state` and `$tf_objects` records share the queue and the counters.
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
