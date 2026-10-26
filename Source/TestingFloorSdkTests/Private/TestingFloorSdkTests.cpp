#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "TestingFloor/TestingFloorQr.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

namespace TestingFloorClientTests
{
enum class ECustomProperty { Example };
template <typename T> concept CPropertyValue = requires(TestingFloor::FProperties& Properties, T Value) {
	Properties.Set(TEXT("value"), Value);
};
static_assert(CPropertyValue<FString> && CPropertyValue<int64> && CPropertyValue<const TCHAR*>);
static_assert(!CPropertyValue<ECustomProperty> && !CPropertyValue<TArray<double>>);

class FTransport final : public TestingFloor::ITransport
{
public:
	TArray<FString> Bodies;
	TArray<FString> Endpoints;
	TFunction<void(int32, bool)> Completion;
	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> InCompletion) override
	{
		Endpoints.Add(Endpoint);
		Bodies.Add(Body);
		Completion = MoveTemp(InCompletion);
	}
	bool Complete(int32 Code, bool bSucceeded)
	{
		if (!Completion)
		{
			return false;
		}
		TFunction<void(int32, bool)> Callback = MoveTemp(Completion);
		Callback(Code, bSucceeded);
		return true;
	}
};

TestingFloor::FSettings Settings()
{
	TestingFloor::FSettings Result;
	Result.WriteKey = TEXT("test-only-never-transmitted");
	Result.Endpoint = TEXT("https://collector.invalid/");
	Result.DeviceID = TEXT("device-test");
	Result.UserID = TEXT("profile-test");
	Result.bMovementTracking = false;
	return Result;
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

TArray<TSharedPtr<FJsonObject>> ParseEvents(const FString& Json)
{
	const TSharedPtr<FJsonObject> Envelope = Parse(Json);
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Envelope.IsValid() || !Envelope->TryGetArrayField(TEXT("events"), Values) || !Values)
	{
		return {};
	}
	TArray<TSharedPtr<FJsonObject>> Events;
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::Object || !Value->AsObject().IsValid())
		{
			return {};
		}
		Events.Add(Value->AsObject());
	}
	return Events;
}
} // namespace TestingFloorClientTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorGateTest, "TestingFloor.Sdk.CollectionGate", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorGateTest::RunTest(const FString& Parameters)
{
	auto Settings = TestingFloorClientTests::Settings();
	TestTrue(TEXT("Configured playtest collects"), TestingFloor::ShouldCollect(Settings, {}));
	TestFalse(TEXT("Editor is off by default"), TestingFloor::ShouldCollect(Settings, { true, false }));
	Settings.bEnableInEditor = true;
	TestTrue(TEXT("Explicit editor opt-in collects"), TestingFloor::ShouldCollect(Settings, { true, false }));
	TestFalse(TEXT("Automation always disables collection"), TestingFloor::ShouldCollect(Settings, { false, true }));
	TestFalse(TEXT("Editor automation ignores opt-in"), TestingFloor::ShouldCollect(Settings, { true, true }));
	const auto Automatic = TestingFloor::FClient::Create(Settings);
	TestEqual(TEXT("Default factory uses the real automation environment"), Automatic->GetState(), TestingFloor::EState::Disabled);
	TestFalse(TEXT("Automatic clients cannot send during automation"), Automatic->Track(TEXT("example")));
	Settings.WriteKey.Reset();
	TestFalse(TEXT("SDK without a host-provided key cannot collect"), TestingFloor::ShouldCollect(Settings, {}));
	const auto Transport = MakeShared<TestingFloorClientTests::FTransport>();
	const auto Client = TestingFloor::FClient::Create(Settings, {}, Transport);
	TestFalse(TEXT("Disabled event rejected"), Client->Track(TEXT("player_weapon_fire")));
	Client->Flush();
	TestEqual(TEXT("Disabled SDK sends no request"), Transport->Bodies.Num(), 0);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorPayloadTest, "TestingFloor.Sdk.TypedPayloadAndSnapshot", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorPayloadTest::RunTest(const FString& Parameters)
{
	const auto Transport = MakeShared<TestingFloorClientTests::FTransport>();
	const FDateTime Timestamp(2026, 10, 7, 12, 34, 56, 789);
	const auto Client = TestingFloor::FClient::Create(TestingFloorClientTests::Settings(), {}, Transport, [Timestamp] { return Timestamp; });
	TestingFloor::FProperties Context;
	Context.Set(TEXT("weapon.id"), TEXT("context-value")).Set(TEXT("captured"), TEXT("before"));
	Client->SetContextProvider([&Context](TestingFloor::FProperties& Properties) { Properties.Merge(Context); });
	TestingFloor::FProperties Event;
	Event.Set(TEXT("weapon.id"), TEXT("plasma_rifle")).Set(TEXT("hit"), true).SetVector(TEXT("hit.point"), FVector(1.5, 2.0, 3.0)).Set(TEXT("count"), static_cast<int64>(9007199254740993LL)).Set(TEXT("names"), TArray<FString>{ TEXT("a"), TEXT("b") }).Set(TEXT("$name"), TEXT("spoofed"));
	Client->Track(TEXT("player_weapon_fire"), Event);
	Context.Set(TEXT("captured"), TEXT("after"));
	Event.Set(TEXT("weapon.id"), TEXT("mutated-after-send"));
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 1, "One request built");
	const TSharedPtr<FJsonObject> Envelope = TestingFloorClientTests::Parse(Transport->Bodies[0]);
	TF_REQUIRE(Envelope.Get(), "Valid collector JSON");
	TestEqual(TEXT("Collector endpoint"), Transport->Endpoints[0], FString(TEXT("https://collector.invalid/v1/batch")));
	TestEqual(TEXT("Envelope write key"), Envelope->GetStringField(TEXT("$write_key")), FString(TEXT("test-only-never-transmitted")));
	const auto Events = TestingFloorClientTests::ParseEvents(Transport->Bodies[0]);
	TF_REQUIRE_TRUE(Events.Num() == 1, "Exactly one valid event object");
	const TSharedPtr<FJsonObject> Payload = Events[0];
	TestEqual(TEXT("Event name cannot be overwritten by properties"), Payload->GetStringField(TEXT("$name")), FString(TEXT("player_weapon_fire")));
	TestEqual(TEXT("Timestamp captured at enqueue"), Payload->GetStringField(TEXT("$time")), Timestamp.ToIso8601());
	TestEqual(TEXT("Schema matches collector"), Payload->GetIntegerField(TEXT("$schema")), 2);
	TestEqual(TEXT("Event overrides context, then owns snapshot"), Payload->GetStringField(TEXT("weapon.id")), FString(TEXT("plasma_rifle")));
	TestEqual(TEXT("Context is captured before later edits"), Payload->GetStringField(TEXT("captured")), FString(TEXT("before")));
	TestTrue(TEXT("Boolean remains boolean"), Payload->GetBoolField(TEXT("hit")));
	TestEqual(TEXT("Position preserves the supplied native units"), Payload->GetNumberField(TEXT("hit.point.x")), 1.5);
	TestTrue(TEXT("64-bit integer keeps exact wire precision"), Transport->Bodies[0].Contains(TEXT("9007199254740993")));
	const TSharedPtr<FJsonObject>* Metadata = nullptr;
	TF_REQUIRE_TRUE(Payload->TryGetObjectField(TEXT("$tf"), Metadata) && Metadata && Metadata->IsValid(), "SDK metadata object");
	TestEqual(TEXT("SDK metadata"), (*Metadata)->GetStringField(TEXT("sdk_version")), FString(TEXT("0.4.0")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorRetryTest, "TestingFloor.Sdk.RetryRetainsEventIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorRetryTest::RunTest(const FString& Parameters)
{
	const auto Transport = MakeShared<TestingFloorClientTests::FTransport>();
	const auto Client = TestingFloor::FClient::Create(TestingFloorClientTests::Settings(), {}, Transport);
	Client->Track(TEXT("objective_complete"));
	Client->Tick(10.0);
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 1, "Initial attempt");
	const auto FirstEvents = TestingFloorClientTests::ParseEvents(Transport->Bodies[0]);
	TF_REQUIRE_TRUE(FirstEvents.Num() == 1, "Initial attempt contains one valid event");
	const FString FirstID = FirstEvents[0]->GetStringField(TEXT("$id"));
	TF_REQUIRE_TRUE(Transport->Complete(503, false), "Initial attempt has completion callback");
	Client->Track(TEXT("puzzle_complete"));
	Client->Tick(10.5);
	TestEqual(TEXT("Backoff prevents tight retry"), Transport->Bodies.Num(), 1);
	Client->Tick(11.0);
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 2, "Retry after backoff");
	const auto Retried = TestingFloorClientTests::ParseEvents(Transport->Bodies[1]);
	TF_REQUIRE_TRUE(Retried.Num() == 2, "Retry contains both valid events");
	TestEqual(TEXT("Retry stable event ID"), Retried[0]->GetStringField(TEXT("$id")), FirstID);
	TestEqual(TEXT("Order preserved before later event"), Retried[1]->GetStringField(TEXT("$name")), FString(TEXT("puzzle_complete")));
	TF_REQUIRE_TRUE(Transport->Complete(401, false), "Retry has completion callback");
	TestEqual(TEXT("Revoked key disables session"), Client->GetState(), TestingFloor::EState::Rejected);
	TestFalse(TEXT("Revoked session accepts no further events"), Client->Track(TEXT("player_moved")));
	Client->Tick(60.0);
	TestEqual(TEXT("No repeated auth attempts"), Transport->Bodies.Num(), 2);
	const auto InvalidTransport = MakeShared<TestingFloorClientTests::FTransport>();
	const auto Recovering = TestingFloor::FClient::Create(TestingFloorClientTests::Settings(), {}, InvalidTransport);
	Recovering->Track(TEXT("example"));
	Recovering->Tick(10.0);
	TF_REQUIRE_TRUE(InvalidTransport->Complete(503, false), "Transient failure received");
	Recovering->Tick(11.0);
	TF_REQUIRE_TRUE(InvalidTransport->Complete(400, false), "Invalid request response received");
	TestEqual(TEXT("A responding collector is no longer reported as offline"), Recovering->GetState(), TestingFloor::EState::Ok);
	TestEqual(TEXT("Invalid batch is dropped"), Recovering->GetPendingEventCount(), 0);

	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorBatchLimitsTest, "TestingFloor.Sdk.BatchLimitsAndQueueBound", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorBatchLimitsTest::RunTest(const FString& Parameters)
{
	auto Settings = TestingFloorClientTests::Settings();
	Settings.BatchMaxEvents = 2;
	Settings.QueueMaxEvents = 3;
	const auto Transport = MakeShared<TestingFloorClientTests::FTransport>();
	const auto Client = TestingFloor::FClient::Create(Settings, {}, Transport);
	TestFalse(TEXT("Oversized event rejected before network"), Client->Track(TEXT("large"), TestingFloor::FProperties().Set(TEXT("text"), FString::ChrN(33000, TEXT('x')))));
	Client->Track(TEXT("one"));
	Client->Track(TEXT("two"));
	Client->Track(TEXT("three"));
	TestFalse(TEXT("Bounded memory drops newest excess"), Client->Track(TEXT("four")));
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 1, "Batch sent");
	TF_REQUIRE_TRUE(TestingFloorClientTests::ParseEvents(Transport->Bodies[0]).Num() == 2, "Event count cap with valid event objects");
	TestEqual(TEXT("In-flight events still count toward capacity"), Client->GetPendingEventCount(), 3);
	TF_REQUIRE_TRUE(Transport->Complete(200, true), "First batch has completion callback");
	Client->Tick(1.0);
	TestEqual(TEXT("Remaining event drains in second batch"), Transport->Bodies.Num(), 2);
	TestEqual(TEXT("Two deliberate drops recorded"), Client->GetDroppedEventCount(), 2);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorMovementTeleportTest, "TestingFloor.Sdk.MovementCameraAndTeleport", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorMovementTeleportTest::RunTest(const FString& Parameters)
{
	auto Settings = TestingFloorClientTests::Settings();
	Settings.bMovementTracking = true;
	Settings.FlushIntervalSeconds = 1000.0;
	const auto Transport = MakeShared<TestingFloorClientTests::FTransport>();
	const auto Client = TestingFloor::FClient::Create(Settings, {}, Transport);
	TestingFloor::FActivitySample Sample;
	Sample.PlayerPosition = FVector::ZeroVector;
	Sample.CameraRotation = FQuat::Identity;
	Client->SetActivitySource([&Sample] { return Sample; });
	Client->Tick(0.0);
	Sample.CameraRotation = FRotator(0.0, 3.0, 0.0).Quaternion();
	Client->Tick(0.1);
	Sample.CameraRotation = FRotator(0.0, 6.0, 0.0).Quaternion();
	Client->Tick(0.2);
	Sample.CameraRotation = FRotator(0.0, 12.0, 0.0).Quaternion();
	Client->Tick(0.8);
	Client->Tick(1.1);
	const auto& Queue = Client->GetQueuedEvents();
	TF_REQUIRE_TRUE(Queue.Num() == 3, "Camera-only activity produces start, ping, stop");
	const auto* StartReason = Queue[0].Properties.GetValues().Find(TEXT("movement.reason"));
	const auto* PingReason = Queue[1].Properties.GetValues().Find(TEXT("movement.reason"));
	const auto* StopReason = Queue[2].Properties.GetValues().Find(TEXT("movement.reason"));
	TF_REQUIRE_TRUE(StartReason && StartReason->IsValid() && (*StartReason)->Type == EJson::String, "Start reason is a string");
	TF_REQUIRE_TRUE(PingReason && PingReason->IsValid() && (*PingReason)->Type == EJson::String, "Ping reason is a string");
	TF_REQUIRE_TRUE(StopReason && StopReason->IsValid() && (*StopReason)->Type == EJson::String, "Stop reason is a string");
	TestEqual(TEXT("Start reason"), (*StartReason)->AsString(), FString(TEXT("start")));
	TestEqual(TEXT("Ping reason"), (*PingReason)->AsString(), FString(TEXT("ping")));
	TestTrue(TEXT("Rotation metric populated"), Queue[1].Properties.GetValues().Contains(TEXT("movement.rotation_degrees")));
	TestEqual(TEXT("Stop reason"), (*StopReason)->AsString(), FString(TEXT("stop")));
	const uint64 Teleport = Client->BeginTeleport(TEXT("save_load"));
	Sample.PlayerPosition = FVector(10000.0, 0.0, 0.0);
	Client->Tick(1.2);
	TestEqual(TEXT("Teleport suppresses interpolation samples"), Client->GetQueuedEvents().Num(), 3);
	Client->EndTeleport(Teleport);
	TF_REQUIRE_TRUE(Client->GetQueuedEvents().Num() == 4, "Teleport recorded once");
	const auto& Event = Client->GetQueuedEvents().Last();
	TestEqual(TEXT("Teleport event"), Event.Name, FString(TEXT("player_teleported")));
	const auto* Distance = Event.Properties.GetValues().Find(TEXT("teleport.distance"));
	TF_REQUIRE_TRUE(Distance && Distance->IsValid() && (*Distance)->Type == EJson::Number, "Teleport distance is a number");
	TestEqual(TEXT("Teleport distance meters"), (*Distance)->AsNumber(), 100.0);
	Client->Tick(1.3);
	TestEqual(TEXT("Destination resets movement baseline"), Client->GetQueuedEvents().Num(), 4);
	Client->EndTeleport(Teleport);
	TestEqual(TEXT("Duplicate token completion ignored"), Client->GetQueuedEvents().Num(), 4);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorRecordingTest, "TestingFloor.Sdk.PlaySessionAndShutdown", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorRecordingTest::RunTest(const FString& Parameters)
{
	const auto Recording = TestingFloor::FRecordingContext::Parse(TEXT(R"TF({"recording_uuid":"example-recording","playtest_id":42,"suppress_qr":false})TF"), FDateTime::UtcNow());
	TF_REQUIRE_TRUE(Recording.IsSet(), "Current recording handoff accepted");
	auto Settings = TestingFloorClientTests::Settings();
	Settings.Recording = Recording;
	const auto Transport = MakeShared<TestingFloorClientTests::FTransport>();
	const auto Client = TestingFloor::FClient::Create(Settings, {}, Transport);
	TestEqual(TEXT("Creation emits no synthetic session events"), Client->GetPendingEventCount(), 0);
	TestNotEqual(TEXT("Play identity differs from recording"), Client->GetPlaySessionID(), Recording->RecordingUUID);
	Client->Track(TEXT("level_started"));
	Client->Shutdown(0.0);
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 1, "Shutdown flushes explicitly tracked events");
	const auto Events = TestingFloorClientTests::ParseEvents(Transport->Bodies[0]);
	TF_REQUIRE_TRUE(Events.Num() == 1, "No legacy bookends added during shutdown");
	TestEqual(TEXT("Play session is the envelope identity"), Events[0]->GetStringField(TEXT("$session_id")), Client->GetPlaySessionID());
	TestEqual(TEXT("Recording identity captured separately"), Events[0]->GetObjectField(TEXT("$tf"))->GetStringField(TEXT("recording_uuid")), Recording->RecordingUUID);
	TestFalse(TEXT("Retired recorder session metadata is absent"), Events[0]->GetObjectField(TEXT("$tf"))->HasField(TEXT("session_id")));
	TestFalse(TEXT("Closed clients reject later events"), Client->Track(TEXT("after_shutdown")));
	Client->Shutdown(0.0);
	TestEqual(TEXT("Repeated shutdown does not create events or requests"), Transport->Bodies.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorBodyCapTest, "TestingFloor.Sdk.CollectorBodyCap", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorBodyCapTest::RunTest(const FString& Parameters)
{
	auto Settings = TestingFloorClientTests::Settings();
	Settings.BatchMaxEvents = 1000;
	TArray<TestingFloor::FEvent> Events;
	for (int32 Index = 0; Index < 80; ++Index)
	{
		Events.Add({ FGuid::NewGuid(), TEXT("bounded"), FDateTime::UtcNow(), TEXT("session"), TestingFloor::FProperties().Set(TEXT("text"), FString::ChrN(25000, TEXT('x'))) });
	}
	int32 Consumed = 0;
	const FString Body = TestingFloor::BuildBatch(Events, Settings, FDateTime::UtcNow(), Consumed);
	TestTrue(TEXT("Collector byte cap splits batch before 1 MiB"), FTCHARToUTF8(*Body).Length() < 1024 * 1024);
	TF_REQUIRE_TRUE(Consumed > 0 && Consumed < Events.Num(), "Remaining events retained for later batch");
	TestEqual(TEXT("Consumed count equals valid serialized events"), TestingFloorClientTests::ParseEvents(Body).Num(), Consumed);
	return true;
}

#endif
