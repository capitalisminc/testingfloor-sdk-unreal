# Telemetry contract

`telemetry_contract.json` is an unedited copy of the Testing Floor telemetry
contract. It is the reference for wire names and limits: the request and event
envelope, `$tf` context keys, platform record names, reserved names, value types,
SDK properties and events, tracked-object fields and limits.

The `TestingFloor.Sdk.Contract` automation tests read this copy and compare it
with what `TestingFloor::FClient` sends. They fail when the SDK and the contract
disagree. The runtime module never reads the file. It ships with the plugin only
so that the tests can run in a host project.

Do not edit the file here. When a contract test fails:

1. If the SDK is wrong, change the SDK.
2. If the contract is wrong, have it changed in the Testing Floor telemetry
   contract, then refresh this copy.

To refresh the copy, replace `telemetry_contract.json` with the current contract,
byte for byte. Then run the `TestingFloor.Sdk.Contract` tests (see
[CONTRIBUTING.md](../../CONTRIBUTING.md)), and update the SDK and
[SDK_CONTRACT.md](../../SDK_CONTRACT.md) where they changed. The Unity SDK keeps
the same copy at the same path.
