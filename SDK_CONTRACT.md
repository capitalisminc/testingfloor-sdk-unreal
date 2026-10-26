# SDK contract

Unity and Unreal share this behavior. Their APIs follow each engine's conventions.
For setup and code examples, start with the README in the SDK you use.

## Identity and recording

- One SDK play-session ID per game launch or editor Play run. It changes for the next Play run.
- A recording UUID identifies a capture that can span several Play runs.
- `$session_id` is the play-session ID, shown as one run in Testing Floor.
- The Player ID shown in Testing Floor is `$user_id`, falling back to `$device_id` when no user ID is present.
- `$tf.recording_uuid` is the recording UUID.
- No automatic `session_start`, `session_end`, or `$tf_session_*` events are emitted.
  Track meaningful gameplay events such as `level_started` yourself.
- The current handoff is `--testing-floor={"recording_uuid":"example-recording"}`.
  Optional fields are `playtest_id`, `suppress_qr` (defaults true), and `created_at_unix_ms`.
- Editor sidecars are `Library/TestingFloor/recording-payload.json` for Unity and
  `Saved/TestingFloor/recording-payload.json` for Unreal. The recorder owns their lifetime.
- A valid launch argument takes precedence over a sidecar. Malformed or expired
  arguments permit sidecar fallback. Timestamped handoffs older than 12 hours are ignored.
- Old recorder session aliases and `session-payload.json` are not supported.

## Recording delivery completion

The recorder supplies `telemetry_directory` in the recording handoff. It creates
this directory and keeps it for the recording. Each SDK Play run registers by
creating `<play-session-id>.session` before admitting recording events. If
`closed.json` already exists, it admits no recording events. The recorder writes
`closed.json` before taking its final participant list, so a concurrent Play boot
cannot produce an undiscovered run.

The SDK watches `closed.json` while Play continues. Closing the recording or
shutting down a Play run stops attaching new events to that recording and queues
`$tf_telemetry_complete`. The marker carries the recording UUID, Play-session ID
and recording-scoped `dropped_events`. It owns a separate request, sent only after preceding
requests succeed or are explicitly counted as dropped. It bypasses the gameplay
queue capacity. A nonzero dropped count means incomplete telemetry.

The receiver acknowledges batches only after its warehouse write succeeds.
Seeing every registered participant's marker in the warehouse therefore closes
the recording's delivery barrier. An empty participant list is explicit. Crashes
or failed shutdown flushes can leave a participant without a marker; consumers
must use a bounded timeout and preserve the incomplete status.

## Event capture and properties

Event name, UUID, UTC timestamp, play session, recording, and context are captured
before an event is queued. Retries preserve that snapshot and event order.
Call-site properties override shared context. Keep callbacks inexpensive and use
the game/main thread for SDK calls.

Use `game_level.id` for a stable, game-owned level identifier.

Properties accept strings, signed integers, finite floating-point numbers,
booleans, GUIDs, string arrays, and integer arrays. Null values, empty property
keys, keys beginning with `$`, and nonfinite numbers are omitted. Arrays are
copied when set. Nested objects and implicit conversion of custom types are not
supported. Convert enums and other custom values at the call site.

An accepted `Send` / `Track` result means queued in memory, not acknowledged by
the server. Event builders are single-use in Unity. Unreal accepts an immutable
snapshot of typed `FProperties` for each call.

## Transport and limits

| Setting | Default / limit |
| --- | --- |
| Endpoint | `https://dataentry.testingfloor.com/v1/batch` |
| Event envelope | `$id`, `$name`, `$time`, `$schema: 2`, `$session_id`, optional identities and `$tf` |
| Request envelope | `$write_key`, `$sent_at`, `events` |
| Batch size | 50 events; configurable up to 2,000 |
| Flush interval | 0.25 seconds |
| Pending capacity | 4,096 including events in flight; overflow drops newest |
| Event size | 32 KiB |
| Request size | Below 1 MiB |
| In-flight requests | One |
| Request timeout | 10 seconds |
| Retry delay | 1 second, doubling to a maximum of 30 seconds |
| Explicit shutdown flush | Best effort; at most 5 seconds |

Network failures, HTTP 408/429, and server errors retain queued events for retry.
HTTP 401/403 reject collection for the rest of the play session. Other HTTP 4xx
responses drop the invalid batch. Retrying does not generate new event IDs.
Missing configuration and disabled collection do not queue events.

There is no disk spool or guaranteed offline delivery. Process crashes, overflow,
oversized events, rejected keys, invalid requests, or an expired shutdown deadline
can lose events. Pending and dropped counters provide local diagnostics.

## Spatial data and activity

| Value | Unity | Unreal |
| --- | --- | --- |
| Position / teleport endpoints | Native meters | Native centimeters |
| Movement thresholds and scalar distances | Meters | Meters |
| Angles and rotation metrics | Degrees | Degrees |
| `camera.fov_axis` | `vertical` | `horizontal` |
| Default map plane | X/Z | X/Y |
| Default world units per meter | 1 | 100 |

Every available player or camera sample attaches `player.position.*`,
`camera.position.*`, `camera.euler.*`, `camera.forward.*`, and `camera.fov` to
events. `camera.fov_axis` and `viewport.width` / `viewport.height` are session
state. Missing sources omit their fields. Native camera Euler conventions differ;
use `camera.forward` and the map's configured axes for spatial projection.

Movement defaults off. Opting in emits debounced `player_moved` events for
player movement or camera-only position, rotation, and FOV changes. Reasons are
`start`, `ping`, and `stop`. Ping metrics cover the segment since the previous
ping; stop metrics cover the whole run. Zero movement must remain still, even
when a threshold is zero. Sampling and QR timing continue while gameplay pauses.

Teleport scopes suppress movement sampling and reset it at the destination.
`player_teleported` carries `teleport.kind=teleport`, optional `teleport.reason`,
endpoints, duration, and distance. Cancellation resets the baseline without
emitting a teleport event.

## Safe defaults

Collection requires a write key. Editor collection, movement events, and QR
markers default off. Automated engine tests cannot enable live collection or
markers, including through a force-QR override. Do not put personal data, write
keys, or other credentials in event properties or public examples.

QR payloads use `tfqr://sync/v1?s=<play-session-id>&t=<unix-ms>&q=<sequence>`.
Defaults are a 15-second interval, continuous visibility, inverted colors, and a
four-module quiet zone. Recorder suppression hides the marker unless explicitly
overridden; `--forceqr` and `-forceqr` are recognized. Unity supplies the overlay;
Unreal supplies marker pixels for the host game's UI.

## Schema 2: session state and tracked objects

Every record is sent with `$schema: 2` and `$tf.seq`: a counter that starts at 1 for each play session and increases by one for every record the SDK queues, including events, `$tf_state` and `$tf_objects`. A retried record keeps its seq.

### Session state (`$tf_state`)

- State providers supply values that stay true until they change, such as settings, device, build, inventory and outfit. The existing context provider stays for small values that describe each moment, such as level, health and game clock, and its values are still added to every event.
- The SDK reads state providers before each event and once a second, in unscaled time, including while gameplay is paused. If a value changed, it queues one `$tf_state` record that holds only the changed keys, before the event. A removed key is sent as `null`.
- A keyframe repeats every current value and sets `$tf.keyframe: true`. The SDK sends one when the play session starts, when a recording starts, every 5 minutes, and after it drops any record.
- The SDK moves its own device, engine and build fields, `viewport.width`, `viewport.height` and `camera.fov_axis` into state. Camera pose, `camera.fov` and the player position stay on events.
- `$tf_state` records carry no context-provider values and no player or camera fields.

### Tracked objects (`$tf_objects`)

- A game registers an object with a kind, a position source and an optional id. Without an id the SDK assigns `kind#n`, counting per kind for each play session.
- The SDK checks live objects 4 times a second (setting: 1–10) in unscaled time. It sends a sample when an object is registered, and after that only when the object has moved at least 0.25 m since its last sent sample.
- Removing an object sends its final position and a reason. SDK reasons are `destroyed` (the engine object is gone), `gone` (the position source returned nothing) and `disabled` (a tracking component was disabled).
- Object state values are scalars. Only changed values are sent.
- Every 5 minutes, and after the SDK drops any record, a keyframe record repeats every live object with its position and full state.
- A record holds parallel arrays: `sample.id`, `sample.kind`, `sample.x`, `sample.y`, `sample.z`; `removed.id`, `removed.reason`; `state.id`, `state.key`, `state.value`. Positions use native units, rounded to 1 cm. A record holds at most 300 samples; a larger check is split into several records with the same `$time`. Records copy the `game_level.*` values from the most recent event and carry no other context and no player or camera fields. The SDK sends no record when nothing changed.
- At most 256 objects are live (setting). Further registrations are ignored, with one warning.
- With collection disabled, registration returns a handle that does nothing, and nothing is sampled.

### With recording delivery completion

- `$tf_telemetry_complete` is a record too. It takes the next `$tf.seq`.
- Dropped `$tf_state` and `$tf_objects` records count toward `dropped_events`, and the SDK sends a keyframe after any drop.
- After a recording closes, `$tf_state` and `$tf_objects` records stop carrying its recording UUID, the same as events. When a Play run registers with a new recording, the SDK sends a keyframe of session state and live objects.
