# Contributing

Use synthetic examples and test fixtures. Keep credentials, customer or game
details, private infrastructure, and local machine paths out of source, docs,
logs, issues, and pull requests. See [AGENTS.md](AGENTS.md).

## Release checks

From the repository root, with Python 3.9 or later:

```sh
python3 scripts/check_release.py
python3 -m unittest discover -s Tests/Release -v
```

These checks validate the distributable's file allowlist, plugin modules,
licensing, vendored checksums, and common credential patterns. They do not replace
a manual privacy review or an Unreal build. CI runs these lightweight checks;
it does not have a licensed Unreal Engine installation.

## Unreal automation

Install and enable the plugin in a C++ host project, then build that project's
Editor target using your engine's normal build command. The editor-only
`TestingFloorSdkTests` module registers tests under `TestingFloor.Sdk`.

Run that filter from **Tools -> Test Automation**, or with your engine's
`UnrealEditor-Cmd` executable:

```sh
UnrealEditor-Cmd /path/to/MyGame.uproject \
  -ExecCmds="Automation RunTests TestingFloor.Sdk" \
  -TestExit="Automation Test Queue Empty" -unattended -nop4 -nosplash -NullRHI
```

Tests exercise typed payloads, collection gates, retry identity/order, queue and
body caps, movement, teleports, recorder parsing, engine context and persisted
IDs, actor and component tracking in a test world, QR reference matrices, pixels,
capacity, and timing. All transports are mocks; no write key or network is needed.
The ID tests write to a temporary save slot and a temporary folder in the host
project's `Saved` directory, and delete both.

Before publishing a version, build the runtime target and Editor target and run
the automation filter on every platform and engine version you claim to support.
Verify any game-specific lifecycle and overlay integration in its host project.

## SDK contract

`Tests/Contract` is an unedited copy of the published
[Testing Floor SDK contract](https://github.com/capitalisminc/testingfloor-sdk-contract),
which every Testing Floor SDK follows:

- `telemetry_contract.json` holds wire field names, types and limits. The
  `TestingFloor.Sdk.Contract` tests compare what the client sends with it: the
  request and event envelope, `$tf` keys, platform record names, tracked-object
  fields, the SDK's own properties and events, value types and limits.
- `SDK_CONTRACT.md` describes the shared behavior.
- `scenarios/` checks that behavior. `TestingFloor.Sdk.Scenarios` runs each file
  as one automation test through `FClient`'s clock, recording-source and
  transport seams; `scenarios/README.md` describes the format.

`Tests/contract-source.json` records the published commit the copy came from.
Refresh the copy with `python3 scripts/update_contract.py`, then run the
`TestingFloor.Sdk` tests. `--check` confirms the copy still matches its recorded
commit, and `--from <folder>` tries an unpublished change locally. Never edit the
copy to make a test pass. Fix the SDK, or have the contract changed where it is
published and copy it again. The copy ships with the plugin so that the tests can
run in a host project; `Config/FilterPlugin.ini` keeps it in `BuildPlugin` packages.

## Package

Create a source ZIP without repository history, local configuration, generated
files, or build output:

```sh
python3 scripts/package.py --output dist/TestingFloor-0.6.0.zip
```

The archive contains a `TestingFloor/` directory ready to copy into a project's
`Plugins/` directory. It includes the plugin, SDK tests and the telemetry contract
they read, README, changelog, and licenses. It does not include these contributor
tools or Git metadata.

For a platform-specific binary package, use your installed engine's `RunUAT`
`BuildPlugin` command with `-Plugin=/path/to/TestingFloor.uplugin`, an output
`-Package` directory outside the source tree, and explicit `-TargetPlatforms`.
Review the resulting archive before uploading it. Never publish a host game's
configuration, content, binaries, or repository history with the SDK.

Change shared behavior in the published SDK contract first, with a scenario that shows it, then update the SDK and its observable regression tests. Follow Epic's [plugin module guidance](https://dev.epicgames.com/documentation/en-us/unreal-engine/plugins-in-unreal-engine).
