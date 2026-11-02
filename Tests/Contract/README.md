# Testing Floor SDK contract

These files define what every Testing Floor game SDK sends and how it behaves.
The Unity and Unreal SDKs, and any SDK for another engine, follow them.

| File | What it holds |
| --- | --- |
| `telemetry_contract.json` | Wire field names, record names, value types and limits. |
| `SDK_CONTRACT.md` | Shared behavior: recording handoff, editor policy, delivery, retries, context. |
| `scenarios/` | Machine-readable checks of that behavior. Each SDK runs them in its own tests. See [scenarios/README.md](scenarios/README.md). |

## Where to change it

Testing Floor publishes these files to
[capitalisminc/testingfloor-sdk-contract](https://github.com/capitalisminc/testingfloor-sdk-contract)
from its main repository on every change. That repository is a read-only copy.
Changes are made in Testing Floor's main repository, together with the collector
or app change that needs them.

## How an SDK uses it

Each SDK keeps an unedited copy of the whole folder with its tests, and records
the published commit it came from. To refresh the copy, run the SDK's update
script, then run its tests. When a test fails, either the SDK is wrong, or the
contract is wrong and needs a change here first. Never edit the copy in an SDK
to make a test pass.

An SDK for a new engine can start from these files: implement the wire format in
`telemetry_contract.json`, then write a runner for the scenarios.
