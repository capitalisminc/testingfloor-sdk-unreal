# Changelog

## Unreleased

Telemetry schema 2: values that rarely change are sent once, and world objects get paths.

### Changed (breaking)

- Every record is sent with `$schema: 2` and `$tf.seq`, a per-play-session counter that a retry keeps.
- `viewport.width`, `viewport.height` and `camera.fov_axis` are session state instead of event properties. Camera pose, `camera.fov` and the player position stay on events.
- Pending and dropped counts include `$tf_state` and `$tf_objects` records.

### Added

- `SetStateProvider`, `SetState` and `ClearState` for values that stay true until they change. The SDK sends changes as `$tf_state` records before the next event and once a second, with keyframes at play-session start, every 5 minutes and after a drop. `SetContextProvider` stays for small per-event values.
- `TrackObject`, `SetObjectState` and `RemoveObject` for NPCs, props and projectiles. The SDK checks live objects 4 times a second (`FSettings::ObjectChecksPerSecond`, 1–10) and batches positions, state and removals into `$tf_objects` records. `FSettings::MaxTrackedObjects` (default 256) limits live objects.

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
