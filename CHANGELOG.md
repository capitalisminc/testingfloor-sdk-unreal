# Changelog

All notable changes to this package are documented here. Versions follow SemVer.

## Unreleased

### Added

- Initial public source package, version `0.1.0`.
- Native C++ telemetry client with typed event properties and shared context.
- Bounded batching, retries, captured event identity, and best-effort shutdown.
- Optional player/camera activity, debounced movement, and teleport events.
- Recorder handoff parsing with separate recording and play-session identities.
- QR sync payloads, encoding, heartbeat policy, and pixel generation.
- Editor automation tests using mock transports and synthetic fixtures.

The game supplies configuration, client lifetime, identity persistence, activity
sources, and overlay rendering. No automatic game integration is included.
