# SDK scenarios

Each JSON file here describes a situation and what an SDK must do in it. Every
SDK has one runner that reads these files and drives the SDK through its test
hooks, with a fake clock, a temporary telemetry directory and a fake collector.
A scenario passes only when the SDK does what the file says.

When a scenario and an SDK disagree, check `SDK_CONTRACT.md`. Fix the SDK if it
breaks the contract. Change the scenario, here, only if the contract changed.

## Common fields

| Field | Meaning |
| --- | --- |
| `format` | Always `1`. A runner fails on a format it does not know. |
| `kind` | `handoff` or `lifecycle`. |
| `summary` | One sentence about what the file checks. |
| `covers` | `SDK_CONTRACT.md` section headings that the file checks. |

## Handoff files

A handoff file checks which recording an SDK picks from the desktop app's
handoffs. It has a `clock_unix_ms`, the SDK's current time, and a list of
`cases`:

| Field | Meaning |
| --- | --- |
| `name` | What the case shows. |
| `launch_argument` | The text after `--testing-floor=`, as one launch argument. Absent when the game was launched without one. |
| `sidecar` | The full text of the engine's `recording-payload.json`. Absent when there is no file. |
| `editor_process_id` | The process ID of the editor running Play. `0`, the default, means a built game. |
| `expect` | `null` when the SDK must pick no recording. Otherwise the fields the picked recording must have: `recording_uuid`, and when present, `playtest_id` (`null` means none) and `suppress_qr`. |

A `launch_argument` or `sidecar` that is a JSON object is passed as its compact
JSON text. A string, such as a malformed payload, is passed as it is. The runner
passes the case to the same function that the SDK uses to resolve a handoff at
startup and on each poll.

## Lifecycle files

A lifecycle file runs one Play session step by step.

| Field | Meaning |
| --- | --- |
| `clock_start_unix_ms` | The wall-clock time when the scenario starts. |
| `environment` | `build` for a built game, `editor` for Play in the editor. |
| `settings` | Optional. `queue_max_events`, `enable_in_editor` and `enable_in_editor_while_recording`. Others keep their defaults. The write key is always set. |
| `collector` | `accept`: the fake collector answers every request with 200 at once. `manual`: each request waits until a `respond` step answers it. |
| `requires` | Optional. `unwritable_directory`: the telemetry directory starts readable but not writable, as a sandboxed macOS app sees it. A runner that cannot set that up skips the scenario and reports the skip. |
| `steps` | What happens, in order. |
| `expect` | What the fake collector must have received at the end. |

The runner turns off the SDK's timed flush. Records leave only on `flush` and
`shutdown`, and through the SDK's own retries and completion marker.

### Steps

Each step is an object with one of these keys.

| Step | What happens |
| --- | --- |
| `{"handoff": {...}}` | From now on the desktop app offers this recording payload. The SDK reads it through its normal parsing at the current clock, at start and on each poll. `{"handoff": null}` removes it. |
| `{"start": {}}` | Play begins: the runner creates the SDK with the scenario's settings and environment. |
| `{"track": "name"}` | The game tracks an event with no properties. Add `"accepted": true` or `false` to check what the SDK returned. |
| `{"advance_ms": 250}` | The wall clock and the SDK's real-time clock move forward. |
| `{"poll": {}}` | One SDK update, as the engine's tick runs it: recording refresh, flush timer and retries. |
| `{"flush": {}}` | The game asks the SDK to send queued records now. |
| `{"respond": 200}` | With a `manual` collector, the collector answers the oldest open request with this HTTP status. |
| `{"create_file": "closed.json"}` | The desktop app writes this file in the telemetry directory. The desktop app can always write there. |
| `{"shutdown": {}}` | The game exits. The SDK sends what it can within its normal bounded drain. |
| `{"check": {...}}` | Checks the SDK's state now. See below. |

A `check` can hold:

| Key | Meaning |
| --- | --- |
| `collecting` | Whether the SDK accepts events now. |
| `recording_uuid` | The recording the SDK has attached, or `null` for none. |
| `files_exist` | File names that must exist in the telemetry directory. |
| `files_absent` | File names that must not exist there. |
| `status` | Fields that the Play run's status file, `{play_session_id}.status.json`, must contain. |

### Expectations

`expect` holds either `requests`, every request the collector received in order,
or `events`, every event across all requests in order. Before the comparison,
the runner removes events whose `$name` no expectation in the file mentions, and
then requests that are left empty. A scenario only checks the records it names.

An expected request is an object:

| Key | Meaning |
| --- | --- |
| `events` | The request's events, in order. |
| `exclusive` | When `true`, the request held exactly these events and nothing else, before any removal. |
| `repeats` | The 1-based position of an earlier request. This request carries the same events with the same `$id` and `$time`: it is a retry. |

An expected event lists only the fields to check, in wire form. `$tf` is an
object. A `null` value means the field must be absent.

### Placeholders

Runners replace these in strings before use:

| Placeholder | Value |
| --- | --- |
| `{telemetry_directory}` | The absolute path of the scenario's temporary telemetry directory, escaped for JSON. |
| `{play_session_id}` | The SDK's Play-session ID. |
