#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "TestingFloor/TestingFloorQr.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

namespace TestingFloorRecordingTests
{
class FTransport final : public TestingFloor::ITransport
{
public:
	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> Completion) override
	{
		Completion(200, true);
	}
};

TestingFloor::FSettings Settings()
{
	TestingFloor::FSettings Result;
	Result.WriteKey = TEXT("test-only-never-transmitted");
	Result.Endpoint = TEXT("https://collector.invalid");
	Result.FlushIntervalSeconds = 1000.0;
	return Result;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}
} // namespace TestingFloorRecordingTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorRecordingCommandLineTest, "TestingFloor.Sdk.RecordingCommandLineQuoting", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorRecordingCommandLineTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 10, 19, 12, 0, 0);
	const TArray<FString> Arguments{
		TEXT(R"TF(--testing-floor={"recording_uuid":"recording with spaces","session_id":"recorder","suppress_qr":false})TF"),
		TEXT(R"TF(-windowed --testing-floor={ "recording_uuid": "recording with spaces", "session_id": "recorder", "suppress_qr": false } -log)TF"),
		TEXT(R"TF(--testing-floor='{"recording_uuid":"recording with spaces","session_id":"recorder","suppress_qr":false}' -log)TF"),
		TEXT(R"TF(--testing-floor="{\"recording_uuid\":\"recording with spaces\",\"session_id\":\"recorder\",\"suppress_qr\":false}" -log)TF"),
		TEXT(R"TF("--testing-floor={\"recording_uuid\":\"recording with spaces\",\"session_id\":\"recorder\",\"suppress_qr\":false}" -log)TF"),
		TEXT(R"TF(--testing-floor={\"recording_uuid\":\"recording with spaces\",\"session_id\":\"recorder\",\"suppress_qr\":false})TF")
	};
	for (const FString& CommandLine : Arguments)
	{
		const auto Recording = TestingFloor::FRecordingContext::FromCommandLine(CommandLine, Now);
		TF_REQUIRE_TRUE(Recording.IsSet(), "Recorder JSON survives supported command-line quoting");
		TestEqual(TEXT("Full recording identity includes spaces"), Recording->RecordingUUID, FString(TEXT("recording with spaces")));
		TestEqual(TEXT("Recorder session stays separate"), Recording->SessionID, FString(TEXT("recorder")));
		TestFalse(TEXT("Explicit QR visibility survives quoting"), Recording->bSuppressQR);
	}
	const auto Escaped = TestingFloor::FRecordingContext::FromCommandLine(TEXT(R"TF(--testing-floor={"recording_uuid":"recording } \"quoted\" \\ path","metadata":{"nested":true}} -log)TF"), Now);
	TF_REQUIRE_TRUE(Escaped.IsSet(), "Closing braces inside strings and nested objects do not truncate JSON");
	TestEqual(TEXT("JSON string escaping is retained"), Escaped->RecordingUUID, FString(TEXT("recording } \"quoted\" \\ path")));
	const auto WindowsEscaped = TestingFloor::FRecordingContext::FromCommandLine(TEXT(R"TF("--testing-floor={\"recording_uuid\":\"recording } \\\"quoted\\\" \\ path\"}" -log)TF"), Now);
	TF_REQUIRE_TRUE(WindowsEscaped.IsSet(), "Windows escaping retains JSON escapes inside string values");
	TestEqual(TEXT("Windows quote escaping removes only the command-line layer"), WindowsEscaped->RecordingUUID, Escaped->RecordingUUID);
	TestFalse(TEXT("Other argument values cannot masquerade as a recording switch"), TestingFloor::FRecordingContext::FromCommandLine(TEXT(R"TF(-other=--testing-floor={"recording_uuid":"wrong"})TF"), Now).IsSet());
	TestFalse(TEXT("TF-looking text inside another quoted argument is ignored"), TestingFloor::FRecordingContext::FromCommandLine(TEXT(R"TF(-other="some --testing-floor={\"recording_uuid\":\"wrong\"}")TF"), Now).IsSet());
	TestFalse(TEXT("Adjacent malformed argument suffix is rejected"), TestingFloor::FRecordingContext::FromCommandLine(TEXT(R"TF(--testing-floor={"recording_uuid":"wrong"}garbage)TF"), Now).IsSet());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorRecordingResolutionTest, "TestingFloor.Sdk.RecordingFallbackAndFreshness", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorRecordingResolutionTest::RunTest(const FString& Parameters)
{
	const FDateTime Now(2026, 10, 19, 12, 0, 0, 789);
	const int64 NowMilliseconds = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	const int64 Boundary = NowMilliseconds - 12LL * 60 * 60 * 1000;
	const FString Fresh = FString::Printf(TEXT(R"TF({"recording_uuid":"fresh","playtest_id":42,"created_at_unix_ms":%lld})TF"), Boundary);
	const FString Stale = FString::Printf(TEXT(R"TF({"recording_uuid":"stale","created_at_unix_ms":%lld})TF"), Boundary - 1);
	TestTrue(TEXT("Exactly twelve-hour-old payload remains valid to the millisecond"), TestingFloor::FRecordingContext::Parse(Fresh, Now).IsSet());
	TestFalse(TEXT("One millisecond beyond recorder lifetime is stale"), TestingFloor::FRecordingContext::Parse(Stale, Now).IsSet());
	TestTrue(TEXT("Outlying future timestamps do not overflow date construction"), TestingFloor::FRecordingContext::Parse(TEXT(R"TF({"recording_uuid":"future","created_at_unix_ms":9223372036854775807})TF"), Now).IsSet());
	const TArray<FString> Sidecars{ TEXT("invalid-json"), Stale, Fresh, TEXT(R"TF({"recording_uuid":"lower-priority"})TF") };
	for (const FString& CommandLine : { FString(), FString(TEXT("--testing-floor=invalid-json")), TEXT("--testing-floor=") + Stale })
	{
		const auto Recording = TestingFloor::FRecordingContext::Resolve(CommandLine, Sidecars, Now);
		TF_REQUIRE_TRUE(Recording.IsSet(), "Missing, malformed or stale CLI permits valid sidecar fallback");
		TestEqual(TEXT("Invalid sidecars are skipped in recorder priority order"), Recording->RecordingUUID, FString(TEXT("fresh")));
		TestTrue(TEXT("Playtest numeric metadata retained"), Recording->PlaytestID.IsSet() && Recording->PlaytestID.GetValue() == 42);
		TestTrue(TEXT("Recorder handoff suppresses QR unless explicitly overridden"), Recording->bSuppressQR);
	}
	const auto Preferred = TestingFloor::FRecordingContext::Resolve(TEXT(R"TF(--testing-floor={"session_id":"cli-recorder"})TF"), Sidecars, Now);
	TF_REQUIRE_TRUE(Preferred.IsSet(), "Valid command line takes priority");
	TestEqual(TEXT("Session-only handoff supplies recording identity"), Preferred->RecordingUUID, FString(TEXT("cli-recorder")));
	TestFalse(TEXT("All invalid candidates leave recording unassociated"), TestingFloor::FRecordingContext::Resolve(TEXT("--testing-floor={}"), TArray<FString>{ Stale, TEXT("{}") }, Now).IsSet());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorRecordingMetadataTest, "TestingFloor.Sdk.RecordingIdentityAndPlatformSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorRecordingMetadataTest::RunTest(const FString& Parameters)
{
	auto Settings = TestingFloorRecordingTests::Settings();
	Settings.DeviceModel = TEXT("test-device");
	Settings.ScreenSize = FIntPoint(640, 480);
	Settings.Recording = TestingFloor::FRecordingContext::Parse(TEXT(R"TF({"recording_uuid":"one-recording","session_id":"one-recorder-session","playtest_id":42})TF"), FDateTime::UtcNow());
	TF_REQUIRE_TRUE(Settings.Recording.IsSet(), "Recording context exists");
	const auto Client = TestingFloor::FClient::Create(Settings, {}, MakeShared<TestingFloorRecordingTests::FTransport>());
	const auto Restarted = TestingFloor::FClient::Create(Settings, {}, MakeShared<TestingFloorRecordingTests::FTransport>());
	TestNotEqual(TEXT("Each play boot has a fresh analytics session within one recording"), Client->GetSessionID(), Restarted->GetSessionID());
	TestNotEqual(TEXT("Analytics play session differs from recorder session"), Client->GetSessionID(), Settings.Recording->SessionID);
	TestingFloor::FActivitySample Sample;
	Sample.CameraRotation = FRotator(30.0, 90.0, 10.0).Quaternion();
	Sample.CameraFOV = 90.0;
	Sample.CameraEulerDegrees = FVector(12.0, 34.0, 56.0);
	Sample.ViewportSize = FIntPoint(1920, 1080);
	Client->SetActivitySource([&Sample] { return Sample; });
	Client->Track(TEXT("first-frame"));
	Sample.ViewportSize = FIntPoint(2560, 1440);
	Sample.CameraEulerDegrees = FVector::ZeroVector;
	Client->Track(TEXT("after-resize"));
	Sample.ViewportSize = FIntPoint::ZeroValue;
	Client->Track(TEXT("without-viewport"));
	TF_REQUIRE_TRUE(Client->GetQueuedEvents().Num() == 3, "Three captured event snapshots");
	const auto First = TestingFloorRecordingTests::Parse(TestingFloor::SerializeEvent(Client->GetQueuedEvents()[0], Settings));
	TF_REQUIRE(First.Get(), "First event serialized");
	const TSharedPtr<FJsonObject>* FirstMetadata = nullptr;
	TF_REQUIRE_TRUE(First->TryGetObjectField(TEXT("$tf"), FirstMetadata) && FirstMetadata && FirstMetadata->IsValid(), "Platform metadata object");
	TestEqual(TEXT("Device model collector field"), (*FirstMetadata)->GetStringField(TEXT("device_model")), Settings.DeviceModel);
	TestEqual(TEXT("Screen width captured with event"), (*FirstMetadata)->GetIntegerField(TEXT("screen_width")), 1920);
	TestEqual(TEXT("Screen height captured with event"), (*FirstMetadata)->GetIntegerField(TEXT("screen_height")), 1080);
	TestEqual(TEXT("Recording groups multiple play sessions"), (*FirstMetadata)->GetStringField(TEXT("recording_uuid")), Settings.Recording->RecordingUUID);
	TestEqual(TEXT("Recorder session preserved separately"), (*FirstMetadata)->GetStringField(TEXT("session_id")), Settings.Recording->SessionID);
	TestEqual(TEXT("Collector top-level session remains the play session"), First->GetStringField(TEXT("$session_id")), Client->GetSessionID());
	TestEqual(TEXT("Host Euler conversion overrides Unreal Euler convention"), First->GetNumberField(TEXT("camera.euler.x")), 12.0);
	TestEqual(TEXT("Host Euler conversion snapshots Y"), First->GetNumberField(TEXT("camera.euler.y")), 34.0);
	TestEqual(TEXT("Host Euler conversion snapshots Z"), First->GetNumberField(TEXT("camera.euler.z")), 56.0);
	TestTrue(TEXT("Camera direction is serialized in native axes independently of Euler overrides"), FMath::IsNearlyEqual(First->GetNumberField(TEXT("camera.forward.y")), FMath::Cos(FMath::DegreesToRadians(30.0)), 0.0001));
	TestEqual(TEXT("Native FOV retains its horizontal interpretation"), First->GetStringField(TEXT("camera.fov_axis")), FString(TEXT("horizontal")));
	TestEqual(TEXT("Native FOV angle is unchanged"), First->GetNumberField(TEXT("camera.fov")), 90.0);
	const auto Resized = TestingFloorRecordingTests::Parse(TestingFloor::SerializeEvent(Client->GetQueuedEvents()[1], Settings));
	TF_REQUIRE(Resized.Get(), "Resized event serialized");
	TestEqual(TEXT("Later viewport size belongs to later event"), Resized->GetObjectField(TEXT("$tf"))->GetIntegerField(TEXT("screen_width")), 2560);
	const auto Fallback = TestingFloorRecordingTests::Parse(TestingFloor::SerializeEvent(Client->GetQueuedEvents()[2], Settings));
	TF_REQUIRE(Fallback.Get(), "Fallback event serialized");
	TestEqual(TEXT("Host screen fallback fills a missing viewport"), Fallback->GetObjectField(TEXT("$tf"))->GetIntegerField(TEXT("screen_width")), 640);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorMovementPolicyTest, "TestingFloor.Sdk.MovementOptInAndReconfiguration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorMovementPolicyTest::RunTest(const FString& Parameters)
{
	const auto Client = TestingFloor::FClient::Create(TestingFloorRecordingTests::Settings(), {}, MakeShared<TestingFloorRecordingTests::FTransport>());
	TestingFloor::FActivitySample Sample;
	Sample.PlayerPosition = FVector::ZeroVector;
	Client->SetActivitySource([&Sample] { return Sample; });
	Client->Tick(0.0);
	Sample.PlayerPosition = FVector(1000.0, 0.0, 0.0);
	Client->Tick(1.0);
	TestEqual(TEXT("Reusable SDK requires movement opt-in like Unity"), Client->GetPendingEventCount(), 0);
	TestingFloor::FMovementSettings Movement;
	Movement.StartGraceSeconds = -1.0;
	Movement.MinPingIntervalSeconds = 0.0;
	Movement.MinPingDistance = 2.0;
	Client->SetMovementSettings(Movement);
	Client->SetMovementTrackingEnabled(true);
	Client->Tick(2.0);
	Sample.PlayerPosition = FVector(1025.0, 0.0, 0.0);
	Client->Tick(2.1);
	TestEqual(TEXT("Negative start grace clamps to zero"), Client->GetPendingEventCount(), 1);
	Sample.PlayerPosition = FVector(1050.0, 0.0, 0.0);
	Client->Tick(2.2);
	TestEqual(TEXT("Configured distance suppresses premature ping"), Client->GetPendingEventCount(), 1);
	Sample.PlayerPosition = FVector(1250.0, 0.0, 0.0);
	Client->Tick(2.3);
	TestEqual(TEXT("Configured distance emits movement ping"), Client->GetPendingEventCount(), 2);
	for (const auto& Event : Client->GetQueuedEvents())
	{
		TestFalse(TEXT("Player-only samples never report phantom camera distance"), Event.Properties.GetValues().Contains(TEXT("movement.camera_distance")));
	}
	Client->SetMovementTrackingEnabled(false);
	Sample.PlayerPosition = FVector(10000.0, 0.0, 0.0);
	Client->Tick(3.0);
	TestEqual(TEXT("Disabling tracking stops movement events"), Client->GetPendingEventCount(), 2);
	Client->SetMovementTrackingEnabled(true);
	Client->Tick(4.0);
	TestEqual(TEXT("Re-enabling establishes a new baseline"), Client->GetPendingEventCount(), 2);
	Sample.PlayerPosition = FVector(10025.0, 0.0, 0.0);
	Client->Tick(4.1);
	TestEqual(TEXT("New movement starts without attributing disabled travel"), Client->GetPendingEventCount(), 3);
	return true;
}

#endif
