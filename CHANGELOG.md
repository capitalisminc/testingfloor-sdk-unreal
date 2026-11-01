# Changelog

## 0.5.0 — Unreleased

- Enable editor telemetry only while a live desktop recording is attached, with a separate opt-out and all existing master/configuration/automation gates.
- Attach to already-running Play, suspend on expired recording leases, renew with fresh state/object keyframes, and record again without restarting Play.
- Preserve Play identity and immutable event association while isolating recording completion/drop counts.
- Refuse `Track` event names that begin with `$tf_` in any casing, which the telemetry contract reserves for Testing Floor records, with one warning per name. Refusals count in `GetDroppedEventCount` while collecting, but not toward a recording's `dropped_events`. Before, a game event named `$tf_telemetry_complete` was batched as a completion marker and, without a recording, failed a check on flush.
- Acknowledge SDK participation through local status and preserve interruption evidence in completion markers.
- Attach a recording when the Play run cannot create its registration file in the recording's telemetry directory, as in a sandboxed macOS game. Events still carry the recording, closure still sends the completion marker, and a warning is logged. Before, the SDK refused the recording, so none of the run's telemetry was linked to it.
- Ship an unedited copy of the Testing Floor telemetry contract in `Tests/Contract`. `TestingFloor.Sdk.Contract` automation tests fail when the envelope, `$tf` keys, record names, tracked-object fields, SDK properties or limits differ from it.

### Aligned with the Unity SDK

- Generate a profile ID, sent as `$user_id`, in the `TestingFloorProfile` slot of the platform save system, and an installation ID, sent as `$device_id`, in `Saved/TestingFloorSdk/installation-id`. `FSettings::UserID` and `DeviceID` still override them. Before, events had no Player ID unless the game set one.
- Fill `$tf.app_version` from the project version, `$tf.device_model` from `FPlatformMisc::GetDeviceMakeAndModel` and `$tf.screen_width` / `screen_height` from the game viewport. Game-supplied values still win.
- Send `$tf.os` as the OS family (`Windows`, `macOS`, `Linux`, `iOS`, `Android`, `tvOS`, `visionOS`, `PlayStation`, `Xbox`, `Switch`) instead of Unreal's platform name such as `Mac`, and `$tf.locale` as a BCP 47 tag.
- Add `$tf.editor` to every record: `true` for Play inside the editor.
- Leave unknown context out instead of sending empty strings, or `engine` as `"Unreal "`, including from the advanced `Create` overload. `ApplyEngineDefaults` fills the same metadata and IDs as the default factory.
- Track actors and scene components weakly with new `TrackObject` overloads, removing them as `destroyed` once they are gone. Add `UTestingFloorTrackedObjectComponent`, which tracks its owner with no code and removes it as `disabled` or `destroyed`. The default factory makes its client the default for these components; `FClient::SetDefault` sets another.
- Give each object check a `$time` later than the previous check's.
- Add `OnRecordingChanged()`, broadcast before the new recording's keyframes, and `IsCollecting()` / `GetCollectionReason()`.
- In the editor, ignore a launch argument whose `editor_pid` names another editor, as handoff files already were. Such an argument could attach this editor's telemetry to another editor's recording. `FRecordingContext::Resolve` takes the editor process ID to check both sources.
- `SDK_CONTRACT.md` matches the Unity SDK's copy again, and the vendored telemetry contract declares `$tf.editor`.

## 0.4.0 — Unreleased

Telemetry schema 2: values that rarely change are sent once, and world objects get paths.

### Changed (breaking)

- Every record is sent with `$schema: 2` and `$tf.seq`, a per-play-session counter that a retry keeps.
- `viewport.width`, `viewport.height` and `camera.fov_axis` are session state instead of event properties. Camera pose, `camera.fov` and the player position stay on events.
- Pending and dropped counts include `$tf_state` and `$tf_objects` records.

### Added

- `SetStateProvider`, `SetState` and `ClearState` for values that stay true until they change. The SDK sends changes as `$tf_state` records before the next event and once a second, with keyframes at play-session start, every 5 minutes and after a drop. `SetContextProvider` stays for small per-event values.
- `TrackObject`, `SetObjectState` and `RemoveObject` for NPCs, props and projectiles. The SDK checks live objects 4 times a second (`FSettings::ObjectChecksPerSecond`, 1–10) and batches positions, state and removals into `$tf_objects` records. `FSettings::MaxTrackedObjects` (default 256) limits live objects.
- Recording delivery completion covers the new records: `$tf_telemetry_complete` takes the next `$tf.seq`, dropped `$tf_state` and `$tf_objects` records count toward `dropped_events`, state and object records stop carrying a closed recording's UUID, and registering with a recording starts state and object keyframes.

## 0.3.0

- Register each Play run with its recorder and close recording telemetry with an acknowledged flush marker.
- Keep completion markers behind earlier batch acknowledgments, report dropped events, and detach later gameplay from stopped recordings.

## 0.2.0 — Unreleased

### Changed (breaking)

- Canonical recorder handoffs require `recording_uuid`; removed recorder-session aliases and the old sidecar filename.
- Play-session identity is distinct from recording identity, with no automatic session bookend events.
- Typed properties reserve `$` keys, omit nonfinite values, and capture arrays by value.
- Both SDKs use a bounded 4,096-event queue including requests in flight, drop newest on overflow, and retain transient failures for ordered retry.
- Automated engine tests cannot enable live collection or forced QR markers.

- `Create(Settings)` supplies automatic ticking, recorder discovery, and environment metadata. The custom-transport overload remains explicitly host-driven.
- Renamed the session accessor to `GetPlaySessionID()` and removed `StartSession` / `EndSession`; use `Shutdown` at teardown.

### Added

- A shared SDK contract covering identity, units, fields, delivery limits, retry behavior, and safe defaults.
- Current setup, lifecycle, diagnostics, testing, and migration documentation.
