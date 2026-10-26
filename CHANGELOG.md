# Changelog

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
