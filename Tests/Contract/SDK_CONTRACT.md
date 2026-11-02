# SDK contract

Every Testing Floor SDK shares this behavior. Each SDK's API follows its engine's
conventions. For setup and code examples, start with the README in the SDK you use.
The files in [scenarios](scenarios/README.md) check these rules; every SDK runs
them in its own tests. Each SDK keeps an unedited copy of this file, the
scenarios and the field reference.

## Field reference

The Testing Floor telemetry contract, `telemetry_contract.json`, is the reference
for wire names and limits: the request and event envelope, `$tf` context keys,
platform record names, reserved names, value types, SDK properties and events,
tracked-object fields and limits. Each SDK keeps an unedited copy with its tests.
Those tests fail when the SDK and the copy disagree. This document describes
behavior. If it and the contract give a different field name, type or limit, the
contract is authoritative.

## Identity and recording

- One SDK play-session ID per game launch or editor Play run. It changes for the next Play run.
- A recording UUID identifies a capture that can span several Play runs.
- `$session_id` is the play-session ID, shown as one run in Testing Floor.
- The SDK generates a profile ID, sent as `$user_id`, and keeps it with the game's
  saved data, so it follows the player wherever that data syncs. It also generates
  an installation ID, sent as `$device_id`, and keeps it on the machine. An ID the
  game sets replaces the generated one. Each README says where its SDK keeps them.
- The Player ID shown in Testing Floor is `$user_id`, falling back to `$device_id` when no user ID is present.
- `$tf.recording_uuid` is the recording UUID.
- No automatic `session_start`, `session_end`, or `$tf_session_*` events are emitted.
  Track meaningful gameplay events such as `level_started` yourself.
- The current handoff is `--testing-floor={"recording_uuid":"example-recording"}`.
  Optional fields are `playtest_id`, `suppress_qr` (defaults true), and `created_at_unix_ms`.
- Editor sidecars are `Library/TestingFloor/recording-payload.json` for Unity,
  `Saved/TestingFloor/recording-payload.json` for Unreal and
  `.godot/TestingFloor/recording-payload.json` for Godot. The recorder owns their lifetime.
  Godot runs Play from the editor in a separate process, so its handoff is
  project-scoped and `editor_pid` is not compared.
- A versioned recording lease sidecar supersedes the launch argument, including
  when its lease has expired, so expiry cannot revive an old launch association.
  Otherwise a valid launch argument takes precedence over a legacy sidecar.
  Malformed or expired arguments permit sidecar fallback. Timestamped handoffs
  older than 12 hours are ignored unless they hold a live lease.
- A live lease has `lease_version:1`, `telemetry_requested:true`, a telemetry
  directory, and a `lease_expires_at_unix_ms` after the current time but no more
  than 30 seconds after it. The desktop app sets each lease to expire 5 seconds
  ahead and renews it every second, so a later expiry can only come from a
  broken handoff, and it must not keep a recording attached.
- Old recorder session aliases and `session-payload.json` are not supported.

## Editor collection policy and dynamic recording

Ordinary editor collection defaults off (`enableInEditor` / `bEnableInEditor`).
Recording-only collection defaults on (`enableInEditorWhileRecording` /
`bEnableInEditorWhileRecording`). Both false means Never; the ordinary editor
switch true means Always. Master disable, force-disable where provided,
automation restrictions and missing configuration still prevent collection.

A recording-only editor association requires the desktop app's live lease, as
defined under Identity and recording, alongside the recording identity. Legacy
static payloads do not override editor-off policy.
Discovery continues independently of collection and gameplay clocks in editor,
standalone and packaged hosts. A valid
recording can attach during an already-running Play session; successive
recordings retain the Play-session ID while changing the recording UUID.

Each attachment/renewal starts state and object keyframes and resets movement
baselines. Dormant integrations retain providers and object handles without
sampling. Detach stops admitting recording events, while data already accepted
can drain afterward. Queued records never acquire the next recording's UUID.
Interactive editors validate an optional `editor_pid`; standalone child Play is
project-scoped without a direct editor PID comparison. Observing a live capture
handoff is independent of telemetry attachment: QR suppression still applies
when collection is blocked by policy or missing configuration. An expired or
closed handoff cannot keep suppressing the marker.

An expired lease suspends association and marks an interruption. It does not
emit a final completion until authoritative closure, replacement, or Play exit.
Renewal can resume an uncompleted pair and requires fresh keyframes. Completed
recording/Play-session pairs never reopen. Local status is written separately
as `<session-id>.status.json` with recording/session identity, engine/SDK version,
state, reason, capabilities, sticky interruption and `updated_at_unix_ms`.

The game can read the active recording, get a notification when it changes, and
ask whether the SDK is collecting, with a machine-readable reason when it is not.

## Recording delivery completion

The recorder supplies `telemetry_directory` in the recording handoff. It creates
this directory and keeps it for the recording. Each SDK Play run registers by
creating `<play-session-id>.session` before admitting recording events. If the
directory is missing or `closed.json` already exists, it admits no recording
events. The recorder writes `closed.json` before taking its final participant
list, so a concurrent Play boot cannot produce an undiscovered run.

Registration is best effort. A Play run that can read the directory but cannot
write to it, such as a sandboxed macOS app, still admits recording events,
watches `closed.json` and sends the completion marker. It is missing from the
recorder's participant list, so consumers find it by the recording UUID on its
events.

The SDK watches `closed.json` while Play continues. Closing the recording or
shutting down a Play run stops attaching new events to that recording and queues
`$tf_telemetry_complete`. The marker carries the recording UUID, Play-session ID
and recording-scoped `dropped_events`. It owns a separate request, sent only after preceding
requests succeed or are explicitly counted as dropped. It bypasses the gameplay
queue capacity. A nonzero dropped count or `interrupted:true` means incomplete telemetry.
Drop and completion bookkeeping is scoped by recording UUID and Play-session ID,
including when data for an earlier recording is still draining.

The receiver acknowledges batches only after its warehouse write succeeds.
Seeing a marker from every registered participant, and from every other run whose
events carry the recording UUID, therefore closes the recording's delivery
barrier. An empty participant list with no such runs means the recording has no
telemetry. Crashes or failed shutdown flushes can leave a participant without a
marker; consumers must use a bounded timeout and preserve the incomplete status.

## Browser games

A game running in a web page cannot read launch arguments, files or an editor
sidecar, so its recording handoff and recording state reach it over the web.

- When Testing Floor opens a browser game for a recording, it adds the recording
  to the game's URL. The desktop app adds the query parameters
  `tf_recording_uuid` and `tf_suppress_qr`. A `#testing-floor=<JSON>` fragment,
  holding the same fields as the launch argument, wins over them. The SDK keeps
  the handoff in the tab's session storage, so reloading the tab keeps the
  recording, and removes the fragment from the address bar.
- The SDK reads the recording's state with `POST
  https://api.testingfloor.com/telemetry/recording_state`, sending
  `{"$write_key": "...", "recording_uuid": "..."}` as `text/plain` so the
  browser skips a preflight. An open recording answers `"closed": false` with a
  `lease_expires_at_unix_ms` 20 seconds ahead and `"state"` `recording` or
  `paused`. A closed one answers `"closed": true`. An unknown one answers 404.
- The SDK polls about every 5 seconds while a recording is attached and uses the
  answers as a desktop SDK uses the lease and `closed.json`: a lease that runs out
  suspends the association and marks an interruption, and `"closed": true`
  completes it. Until the first answer, and for a recording the endpoint does not
  know, events carry the recording only within one bounded startup confirmation window. A 404 never extends that window or revives an expired confirmed lease. The current browser release uses 20 seconds.
- A browser run does not register and writes no status file. Consumers find it
  by the recording UUID on its events.
- The SDK completes the run when the page is hidden for good (`pagehide`) and
  hands what is still queued to `sendBeacon`. Beacon acceptance is unconfirmed delivery: its completion marker is interrupted, even when dropped_events is zero. Only acknowledgement of preceding requests establishes a complete ordered delivery.
- `$tf.os` is `Web`. `$tf.engine` is the game's framework and version when the
  game supplies it.

## Event capture and properties

Event name, UUID, UTC timestamp, play session, recording, and context are captured
before an event is queued. Retries preserve that snapshot and event order.
Call-site properties override shared context. Keep callbacks inexpensive and use
the game/main thread for SDK calls.

An optional event processor runs last over each tracked event's merged properties:
context provider values, player and camera values, then call-site properties. It
can read, change or remove them before the event is queued, for example to
convert a game's coordinate frame. The `game_level.*` values that object records
copy come from its result. It also sees `player_moved` and `player_teleported`,
but not the SDK's `$tf_` records.

Use `game_level.id` for a stable, game-owned level identifier.

Properties accept strings, signed integers, finite floating-point numbers,
booleans, GUIDs, string arrays, and integer arrays. Null values, empty property
keys, keys beginning with `$`, and nonfinite numbers are omitted. Arrays are
copied when set. Nested objects and implicit conversion of custom types are not
supported. Convert enums and other custom values at the call site.

Event names beginning with `$tf_`, in any casing, are reserved for the SDK's own
records. `Track` refuses them and sends nothing. It logs one warning per name.
While the SDK is collecting, each refused event counts as dropped in the local
diagnostics, but not toward a recording's `dropped_events`, and it starts no
keyframe. The SDK's own records are unaffected.

`$time` and `$sent_at` are ISO 8601 times in UTC with exactly three fractional
digits and a `Z` suffix, such as `2026-04-19T18:32:00.000Z`.

An accepted `Send` / `Track` result means queued in memory, not acknowledged by
the server. Event builders are single-use in Unity. Unreal accepts an immutable
snapshot of typed `FProperties` for each call.

## Record context

Every record carries `$tf`: the SDK version, `engine`, `editor`, `os`, `locale`,
`app_version`, `device_model`, `screen_width` / `screen_height`, the recording's
`recording_uuid` and `playtest_id` while one is attached, `seq`, and `keyframe` on
keyframes.

- `editor` is `true` for Play inside the engine editor, else `false`.
- `os` is the operating system family: `Windows`, `macOS`, `Linux`, `iOS`,
  `Android`, `tvOS`, `visionOS`, `Web`, `PlayStation`, `Xbox` or `Switch`. Any
  other platform sends the engine's platform name.
- `locale` is a BCP 47 tag such as `en-US`, from the OS or UI culture. When the
  SDK cannot tell the region it sends a language-only tag such as `en`, and when
  it cannot tell the language it leaves `locale` out. It never sends a display
  name such as `English`.
- `app_version` is the engine's version setting. `device_model` and the screen
  size are read from the engine. Where an SDK lets the game supply one of these
  values, the game's value wins.
- The SDK never sends an empty string or a partial value such as an engine name
  without its version. It leaves out a key whose value it does not know.

## Transport and limits

| Setting | Current release default / wire limit |
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

Batch sizes, flush intervals, pending capacities, request deadlines, retry cadence,
shutdown budgets and sampling cadence above are release tuning defaults. They
may differ by engine and change between releases. Wire size/count limits and
observable semantics remain normative: ordered retries retain IDs and captured
values, capacity is bounded, dropped records are counted, and unacknowledged
delivery never becomes proof of completeness. Scenario fixtures may select
specific timings to exercise these semantics without promising those defaults.

Network failures, HTTP 408/429, and server errors retain queued events for retry.
HTTP 401/403 reject collection for the rest of the play session. Other HTTP 4xx
responses drop the invalid batch. Retrying does not generate new event IDs.
Missing configuration and disabled collection do not queue events.

There is no disk spool or guaranteed offline delivery. Process crashes, overflow,
oversized events, rejected keys, invalid requests, or an expired shutdown deadline
can lose events. Pending and dropped counters provide local diagnostics.

## Spatial data and activity

| Value | Unity | Unreal | Godot |
| --- | --- | --- | --- |
| Position / teleport endpoints | Native meters | Native centimeters | Native meters in 3D; pixels in 2D, with Z 0 |
| Movement thresholds and scalar distances | Meters | Meters | Meters |
| Angles and rotation metrics | Degrees | Degrees | Degrees |
| `camera.fov_axis` | `vertical` | `horizontal` | `vertical` |
| Default map plane | X/Z | X/Y | X/Z in 3D, X/Y in 2D |
| Default world units per meter | 1 | 100 | 1 |

Every available player or camera sample attaches `player.position.*`,
`camera.position.*`, `camera.euler.*`, `camera.forward.*` and `camera.up.*` to
events, with `camera.fov` for a perspective camera or `camera.ortho_width`, the
view's width in native units, for an orthographic one. A game can supply the
point at the center of the view, such as a strategy camera's pivot, sent as
`camera.focus.*`. `camera.projection` (`perspective` or `orthographic`),
`camera.fov_axis` (perspective only) and `viewport.width` / `viewport.height`
are session state. Missing sources omit their fields. Native camera Euler
conventions differ; use `camera.forward` and the map's configured axes for
spatial projection.

Movement defaults on. Once the game supplies a player or camera sample, the SDK
emits debounced `player_moved` events for player movement or camera-only
position, rotation, FOV and orthographic width changes, and a game can turn
them off. Pings come at most every 0.1 seconds by default, so a half-second
jump gets several samples; a game may raise the interval to send fewer. Reasons
are `start`, `ping`, and `stop`. Ping metrics cover the segment
since the previous ping; stop metrics cover the whole run. Zero movement must
remain still, even when a threshold is zero. Sampling and QR timing continue
while gameplay pauses.

Teleport scopes suppress movement sampling and reset it at the destination.
`player_teleported` carries `teleport.kind=teleport`, optional `teleport.reason`,
endpoints, duration, and distance. Cancellation resets the baseline without
emitting a teleport event.

## Safe defaults

Collection requires a write key. Ordinary editor collection and QR markers
default off. Automated engine tests cannot enable live collection or
markers, including through a force-QR override. Do not put personal data, write
keys, or other credentials in event properties or public examples.

QR payloads use `tfqr://sync/v1?s=<play-session-id>&t=<unix-ms>&q=<sequence>`.
Defaults are a 15-second interval, continuous visibility, inverted colors, and a
four-module quiet zone. Recorder suppression hides the marker unless explicitly
overridden; `--forceqr` and `-forceqr` are recognized. Unity and Godot supply the
overlay; Unreal supplies marker pixels for the host game's UI.

## Schema 2: session state and tracked objects

Every record is sent with `$schema: 2` and `$tf.seq`: a counter that starts at 1 for each play session and increases by one for every record the SDK queues, including events, `$tf_state` and `$tf_objects`. A retried record keeps its seq.

### Session state (`$tf_state`)

- State providers supply values that stay true until they change, such as settings, device, build, inventory and outfit. The existing context provider stays for small values that describe each moment, such as level, health and game clock, and its values are still added to every event.
- A value set directly with `SetState` accepts the same types as a provider: string, integer, double, boolean, string list and integer list. A direct value replaces the provider's value for the same key.
- The SDK reads state providers before each event and once a second, in unscaled time, including while gameplay is paused. If a value changed, it queues one `$tf_state` record that holds only the changed keys, before the event. A removed key is sent as `null`.
- A keyframe repeats every current value and sets `$tf.keyframe: true`. The SDK sends one when the play session starts, when a recording starts, every 5 minutes, and after it drops any record.
- The SDK sends its device, engine and build fields in `$tf` on every record, not as state. State carries `viewport.width`, `viewport.height`, `camera.projection` and `camera.fov_axis` and the game's state values. Camera pose, `camera.fov`, `camera.ortho_width` and the player position stay on events.
- `$tf_state` records carry no context-provider values and no player or camera fields.

### Tracked objects (`$tf_objects`)

- A game registers an object with a kind, a position source and an optional id. Without an id the SDK assigns `kind#n`, counting per kind for each play session.
- An object can also be tracked from an engine object, without keeping it alive, or with a no-code tracking component. The SDK removes it as `destroyed` when the engine object is gone and as `disabled` when the component is disabled.
- The SDK checks live objects 4 times a second (setting: 1–10) in unscaled time. It sends a sample when an object is registered, and after that only when the object has moved at least 0.25 m since its last sent sample.
- Removing an object sends its final position and a reason. SDK reasons are `destroyed` (the engine object is gone), `gone` (the position source returned nothing), `disabled` (a tracking component was disabled) and `removed` (the game removed the object without a reason).
- Object state values are scalars. Only changed values are sent.
- Every 5 minutes, and after the SDK drops any record, a keyframe record repeats every live object with its position and full state.
- A record holds parallel arrays: `sample.id`, `sample.kind`, `sample.x`, `sample.y`, `sample.z`; `removed.id`, `removed.reason`; `state.id`, `state.key`, `state.value`. Positions use native units, rounded to 1 cm. A record holds at most 300 samples; a larger check is split into several records with the same `$time`. Each check's records get a `$time` later than the previous check's, so readers that order by time keep checks apart. Records copy the `game_level.*` values from the most recent event and carry no other context and no player or camera fields. The SDK sends no record when nothing changed.
- At most 256 objects are live (setting). Further registrations are ignored, with one warning.
- With collection disabled, registration returns a handle that does nothing, and nothing is sampled.

### With recording delivery completion

- `$tf_telemetry_complete` is a record too. It takes the next `$tf.seq`.
- Dropped `$tf_state` and `$tf_objects` records count toward `dropped_events`, and the SDK sends a keyframe after any drop.
- After a recording closes, `$tf_state` and `$tf_objects` records stop carrying its recording UUID, the same as events. When a Play run registers with a new recording, the SDK sends a keyframe of session state and live objects.

+
+## Build identity and spatial conversion
+
+`buildId` (`build_id` on the wire) is an optional, immutable game-owned identifier,
+such as a CI artifact identity. Configure the same value in the SDK, build upload
+and map publication. `appVersion` remains a readable label and a legacy map
+fallback. Never infer immutable identity from a version label or rotating key.
+
+Every host offers an optional `recording_only` collection mode. Recording
+association discovery continues while producers are dormant. An active recording
+permits collection, pause/expiry suspends it, and a fresh active lease resumes it.
+The existing always-collect mode remains the default; editor gates still apply.
+
+Automatic movement and teleport events use `$tf_player_moved` and
+`$tf_player_teleported`. They remain gameplay samples for analytics and spatial
+readers; they are not state/object control records to exclude. Readers continue
+to accept historical `player_moved` and `player_teleported` data without renaming
+customer-authored events with those names.
+
+An optional declared coordinate transform is one uniform similarity transform:
+three distinct signed source axes, positive uniform scale, and an output offset.
+For example axes `[1, -2, 3]`, scale `2`, offset `[10, 20, 30]` maps native
+`[1, 2, 3]` to `[12, 16, 36]`. Points include offset; directions apply only the
+signed axis permutation; lengths apply scale. This covers event player/camera
+points, camera forward/up, focus points, teleport endpoints and distances,
+movement length totals, orthographic widths and tracked-object samples.
+
+Without a transform, existing native-coordinate behavior stays intact. With a
+transform, emitted lengths use the output coordinate unit. Unreal converts its
+legacy metre-valued scalar fields back to source centimetres before applying
+the transform. Sampling thresholds retain their documented source units and are
+compared in source space, which is equivalent to scaling both distance and
+threshold. Translation never changes direction or length. Raw Euler metadata is
+omitted when a transform is active; forward/up express orientation in output
+space. Arbitrary game properties and object state are not guessed to be spatial.
+An event processor runs after conversion and may intentionally override fields.
+Remove an old event-processor coordinate conversion when enabling this seam.
+
+
+Map bounds, selected axes and world-units-per-metre settings describe the output
+coordinate space. A transform does not rewrite a previously uploaded map image
+or historical events. Changing the game's coordinate declaration requires a new
+build ID and matching map publication. Object sample quantization retains its
+native precision before conversion rather than rounding small output units away.
+