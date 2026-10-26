#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <limits>

namespace TestingFloorSchemaTests
{
class FTransport final : public TestingFloor::ITransport
{
public:
	TArray<FString> Bodies;
	TFunction<void(int32, bool)> Completion;
	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> InCompletion) override
	{
		Bodies.Add(Body);
		Completion = MoveTemp(InCompletion);
	}
	bool Complete(int32 Code)
	{
		if (!Completion)
		{
			return false;
		}
		TFunction<void(int32, bool)> Callback = MoveTemp(Completion);
		Callback(Code, Code >= 200 && Code < 300);
		return true;
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

TSharedRef<TestingFloor::FClient> Create(const TestingFloor::FSettings& InSettings, const TSharedRef<FTransport>& Transport = MakeShared<FTransport>())
{
	return TestingFloor::FClient::Create(InSettings, {}, Transport);
}

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

/** Every queued record, serialized and parsed the way the collector receives it. */
TArray<TSharedPtr<FJsonObject>> Records(const TestingFloor::FClient& Client, const FString& Name = {})
{
	TArray<TSharedPtr<FJsonObject>> Result;
	for (const TestingFloor::FEvent& Event : Client.GetQueuedEvents())
	{
		if (Name.IsEmpty() || Event.Name == Name)
		{
			Result.Add(Parse(TestingFloor::SerializeEvent(Event, Settings())));
		}
	}
	return Result;
}

int64 Seq(const TSharedPtr<FJsonObject>& Record)
{
	int64 Value = 0;
	Record->GetObjectField(TEXT("$tf"))->TryGetNumberField(TEXT("seq"), Value);
	return Value;
}

bool HasRecording(const TSharedPtr<FJsonObject>& Record)
{
	return Record->GetObjectField(TEXT("$tf"))->HasField(TEXT("recording_uuid"));
}

bool IsKeyframe(const TSharedPtr<FJsonObject>& Record)
{
	bool bValue = false;
	return Record->GetObjectField(TEXT("$tf"))->TryGetBoolField(TEXT("keyframe"), bValue) && bValue;
}

/** Sorted property keys without the `$` envelope fields, joined for readable failures. */
FString Keys(const TSharedPtr<FJsonObject>& Record)
{
	TArray<FString> Result;
	for (const auto& Pair : Record->Values)
	{
		const FString Key(Pair.Key.ToView());
		if (!Key.StartsWith(TEXT("$")))
		{
			Result.Add(Key);
		}
	}
	Result.Sort();
	return FString::Join(Result, TEXT(","));
}

/** A record array joined as text: strings as written, numbers in shortest form. */
FString Array(const TSharedPtr<FJsonObject>& Record, const TCHAR* Key)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Record->TryGetArrayField(Key, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(Value->Type == EJson::Number ? FString::Printf(TEXT("%g"), Value->AsNumber()) : Value->AsString());
		}
	}
	return FString::Join(Result, TEXT(","));
}

int32 Count(const TSharedPtr<FJsonObject>& Record, const TCHAR* Key)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Record->TryGetArrayField(Key, Values) ? Values->Num() : 0;
}
} // namespace TestingFloorSchemaTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorSequenceTest, "TestingFloor.Sdk.Schema2.SequenceAndEnvelope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorSequenceTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const auto Transport = MakeShared<FTransport>();
	const auto Client = Create(Settings(), Transport);
	Client->Track(TEXT("level_loaded"));
	Client->Track(TEXT("objective_complete"));
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 1, "First attempt sent");
	const TSharedPtr<FJsonObject> First = Parse(Transport->Bodies[0]);
	TF_REQUIRE(First.Get(), "Valid collector JSON");
	const TArray<TSharedPtr<FJsonValue>>& FirstEvents = First->GetArrayField(TEXT("events"));
	TF_REQUIRE_TRUE(FirstEvents.Num() == 2, "Both events sent");
	TestEqual(TEXT("Every record is schema 2"), FirstEvents[0]->AsObject()->GetIntegerField(TEXT("$schema")), 2);
	TestEqual(TEXT("SDK version"), FirstEvents[0]->AsObject()->GetObjectField(TEXT("$tf"))->GetStringField(TEXT("sdk_version")), FString(TEXT("0.4.0")));
	TestEqual(TEXT("Seq starts at 1 for the play session"), Seq(FirstEvents[0]->AsObject()), 1LL);
	TestEqual(TEXT("Seq increases by one per queued record"), Seq(FirstEvents[1]->AsObject()), 2LL);
	TestFalse(TEXT("Events are not keyframes"), IsKeyframe(FirstEvents[0]->AsObject()));
	TF_REQUIRE_TRUE(Transport->Complete(503), "Transient failure");
	Client->Track(TEXT("player_death"));
	Client->Tick(2.0);
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 2, "Retry after backoff");
	const TSharedPtr<FJsonObject> Retry = Parse(Transport->Bodies[1]);
	TF_REQUIRE(Retry.Get(), "Valid retry JSON");
	const TArray<TSharedPtr<FJsonValue>>& Retried = Retry->GetArrayField(TEXT("events"));
	TF_REQUIRE_TRUE(Retried.Num() == 3, "Retry carries the retained events and the new one");
	TestEqual(TEXT("A retried record keeps its ID"), Retried[0]->AsObject()->GetStringField(TEXT("$id")), FirstEvents[0]->AsObject()->GetStringField(TEXT("$id")));
	TestEqual(TEXT("A retried record keeps its seq"), Seq(Retried[0]->AsObject()), 1LL);
	TestEqual(TEXT("A retried record keeps its seq"), Seq(Retried[1]->AsObject()), 2LL);
	TestEqual(TEXT("A later record continues the counter"), Seq(Retried[2]->AsObject()), 3LL);
	const auto Next = Create(Settings());
	Next->Track(TEXT("level_loaded"));
	TestEqual(TEXT("A new client starts a new play-session counter"), Seq(Records(*Next)[0]), 1LL);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorStateDiffTest, "TestingFloor.Sdk.Schema2.StateDiffsAndOrdering", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorStateDiffTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const auto Client = Create(Settings());
	TestingFloor::FProperties Provided;
	Provided.Set(TEXT("pref.dlss_enabled"), true).Set(TEXT("player.outfit.id"), TEXT("guard_uniform")).Set(TEXT("inventory.items"), TArray<FString>{ TEXT("key_red") });
	Client->SetStateProvider([&Provided](TestingFloor::FProperties& State) { State.Merge(Provided); });
	Client->SetContextProvider([](TestingFloor::FProperties& Context) { Context.Set(TEXT("game_level.id"), TEXT("harbor")).Set(TEXT("player.hp"), 90); });
	TestingFloor::FActivitySample Sample;
	Sample.PlayerPosition = FVector(100.0, 200.0, 0.0);
	Sample.CameraFOV = 90.0;
	Sample.ViewportSize = FIntPoint(1920, 1080);
	Client->SetActivitySource([&Sample] { return Sample; });
	Client->SetState(TEXT("difficulty"), TEXT("hard"));

	Client->Track(TEXT("level_loaded"));
	auto All = Records(*Client);
	TF_REQUIRE_TRUE(All.Num() == 2, "One state record, then the event");
	const auto& Keyframe = All[0];
	const auto& Event = All[1];
	TestEqual(TEXT("State is queued before its event"), Keyframe->GetStringField(TEXT("$name")), FString(TEXT("$tf_state")));
	TestTrue(TEXT("The first state record is a keyframe"), IsKeyframe(Keyframe));
	TestEqual(TEXT("State seq precedes the event's"), Seq(Keyframe) + 1, Seq(Event));
	TestEqual(TEXT("State and event share the event's time"), Keyframe->GetStringField(TEXT("$time")), Event->GetStringField(TEXT("$time")));
	TestEqual(TEXT("Keyframe holds every state value and the SDK's own viewport and FOV axis, and nothing else"), Keys(Keyframe),
		FString(TEXT("camera.fov_axis,difficulty,inventory.items,player.outfit.id,pref.dlss_enabled,viewport.height,viewport.width")));
	TestEqual(TEXT("SDK viewport state"), Keyframe->GetIntegerField(TEXT("viewport.width")), 1920);
	TestTrue(TEXT("Event keeps context"), Event->HasField(TEXT("game_level.id")) && Event->HasField(TEXT("player.hp")));
	TestTrue(TEXT("Event keeps player position and camera FOV"), Event->HasField(TEXT("player.position.x")) && Event->HasField(TEXT("camera.fov")));
	TestFalse(TEXT("Event no longer repeats state"), Event->HasField(TEXT("pref.dlss_enabled")) || Event->HasField(TEXT("difficulty")));
	TestFalse(TEXT("Viewport and FOV axis moved to state"), Event->HasField(TEXT("viewport.width")) || Event->HasField(TEXT("camera.fov_axis")));

	Provided.Set(TEXT("pref.dlss_enabled"), false);
	Provided.Remove(TEXT("player.outfit.id"));
	Client->Track(TEXT("player_weapon_fire"));
	All = Records(*Client);
	TF_REQUIRE_TRUE(All.Num() == 4, "A change queues one state record before the next event");
	TestEqual(TEXT("Change record name"), All[2]->GetStringField(TEXT("$name")), FString(TEXT("$tf_state")));
	TestFalse(TEXT("A change is not a keyframe"), IsKeyframe(All[2]));
	TestEqual(TEXT("Only changed keys are sent"), Keys(All[2]), FString(TEXT("player.outfit.id,pref.dlss_enabled")));
	TestFalse(TEXT("Changed value"), All[2]->GetBoolField(TEXT("pref.dlss_enabled")));
	TestTrue(TEXT("A removed key is sent as null"), All[2]->HasTypedField<EJson::Null>(TEXT("player.outfit.id")));
	TestEqual(TEXT("Event follows its state change"), All[3]->GetStringField(TEXT("$name")), FString(TEXT("player_weapon_fire")));

	Client->Track(TEXT("player_weapon_fire"));
	TestEqual(TEXT("No change sends no state record"), Records(*Client, TEXT("$tf_state")).Num(), 2);

	Provided.Set(TEXT("difficulty"), TEXT("easy"));
	Client->Track(TEXT("player_weapon_fire"));
	TestEqual(TEXT("A direct value overrides the provider's value"), Records(*Client, TEXT("$tf_state")).Num(), 2);
	Client->ClearState(TEXT("difficulty"));
	Client->Track(TEXT("player_weapon_fire"));
	All = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(All.Num() == 3, "Clearing a direct value exposes the provider's value");
	TestEqual(TEXT("Provider value after clearing"), All[2]->GetStringField(TEXT("difficulty")), FString(TEXT("easy")));
	Provided.Remove(TEXT("difficulty"));
	Client->Track(TEXT("player_weapon_fire"));
	All = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(All.Num() == 4, "A key no source supplies is removed");
	TestTrue(TEXT("Removed direct value is null"), All[3]->HasTypedField<EJson::Null>(TEXT("difficulty")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorStateKeyframeTest, "TestingFloor.Sdk.Schema2.StateKeyframes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorStateKeyframeTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	auto ClientSettings = Settings();
	ClientSettings.QueueMaxEvents = 5;
	const auto Transport = MakeShared<FTransport>();
	const auto Client = Create(ClientSettings, Transport);
	int32 Volume = 5;
	Client->SetStateProvider([&Volume](TestingFloor::FProperties& State) {
		State.Set(TEXT("pref.volume"), Volume).Set(TEXT("app.commit"), TEXT("abc123"));
	});
	Client->Tick(0.0);
	auto States = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(States.Num() == 1, "Play session start sends a keyframe without an event");
	TestTrue(TEXT("Start keyframe"), IsKeyframe(States[0]));

	Volume = 6;
	Client->Tick(0.5);
	TestEqual(TEXT("State is read once a second"), Records(*Client, TEXT("$tf_state")).Num(), 1);
	Client->Tick(1.0);
	States = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(States.Num() == 2, "The once-a-second read finds the change");
	TestEqual(TEXT("Delta holds only the change"), Keys(States[1]), FString(TEXT("pref.volume")));

	Client->Tick(299.0);
	TestEqual(TEXT("No keyframe before 5 minutes"), Records(*Client, TEXT("$tf_state")).Num(), 2);
	Client->Tick(300.0);
	States = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(States.Num() == 3, "Keyframe every 5 minutes");
	TestTrue(TEXT("Periodic keyframe"), IsKeyframe(States[2]));
	TestEqual(TEXT("Keyframe repeats unchanged values"), Keys(States[2]), FString(TEXT("app.commit,pref.volume")));

	Client->Track(TEXT("one"));
	Client->Track(TEXT("two"));
	TestFalse(TEXT("A full queue drops the event"), Client->Track(TEXT("three")));
	TestEqual(TEXT("Drop counted"), Client->GetDroppedEventCount(), 1);
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Complete(200), "Queue drained");
	Client->Tick(301.0);
	States = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(States.Num() == 1, "A drop triggers a keyframe on the next read");
	TestTrue(TEXT("Recovery keyframe"), IsKeyframe(States[0]));
	TestEqual(TEXT("Recovery keyframe repeats every value"), Keys(States[0]), FString(TEXT("app.commit,pref.volume")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorEmptyKeyframeTest, "TestingFloor.Sdk.Schema2.EmptyStateKeyframe", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorEmptyKeyframeTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const auto Client = Create(Settings());
	Client->Tick(0.0);
	TestEqual(TEXT("An empty start keyframe is skipped"), Records(*Client, TEXT("$tf_state")).Num(), 0);
	Client->SetState(TEXT("difficulty"), TEXT("hard"));
	Client->Tick(1.0);
	auto States = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(States.Num() == 1 && IsKeyframe(States[0]), "The first value goes out as the start keyframe");
	Client->ClearState(TEXT("difficulty"));
	Client->Tick(301.0);
	States = Records(*Client, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(States.Num() == 2, "Clearing the last value when a keyframe is due still sends the keyframe");
	TestTrue(TEXT("Empty keyframe"), IsKeyframe(States[1]));
	TestEqual(TEXT("An empty keyframe clears every value"), Keys(States[1]), FString());
	Client->Tick(302.0);
	TestEqual(TEXT("Nothing changes after the empty keyframe"), Records(*Client, TEXT("$tf_state")).Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorReusedIDTest, "TestingFloor.Sdk.Schema2.ReusedIDWaitsForRemoval", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorReusedIDTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	auto ClientSettings = Settings();
	ClientSettings.QueueMaxEvents = 2;
	const auto Transport = MakeShared<FTransport>();
	const auto Client = Create(ClientSettings, Transport);
	const uint64 Old = Client->TrackObject(TEXT("crate"), [] { return TOptional<FVector>(FVector::ZeroVector); }, TEXT("crate"));
	Client->Tick(0.0);
	Client->Track(TEXT("level_loaded"));
	Client->RemoveObject(Old, TEXT("destroyed"));
	Client->TrackObject(TEXT("crate"), [] { return TOptional<FVector>(FVector(500.0, 0.0, 0.0)); }, TEXT("crate"));
	Client->Tick(0.25);
	TestEqual(TEXT("A full queue drops the removal record"), Client->GetDroppedEventCount(), 1);
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Complete(200), "Queue drained");
	Client->Tick(0.5);
	Client->Tick(0.75);
	const auto Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 2, "The removal, then the new path");
	TestEqual(TEXT("The retried removal ends the old path"), Array(Objects[0], TEXT("removed.id")), FString(TEXT("crate")));
	TestEqual(TEXT("The removal record holds only the old position"), Array(Objects[0], TEXT("sample.x")), FString(TEXT("0")));
	TestEqual(TEXT("The new path starts in a later record"), Array(Objects[1], TEXT("sample.x")), FString(TEXT("500")));
	TestFalse(TEXT("The new path's record has no removal"), Objects[1]->HasField(TEXT("removed.id")));
	TestTrue(TEXT("The removal is queued first"), Seq(Objects[0]) < Seq(Objects[1]));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorNonFinitePositionTest, "TestingFloor.Sdk.Schema2.NonFinitePositionIsSkipped", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorNonFinitePositionTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const auto Client = Create(Settings());
	FVector Crate(std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0);
	Client->TrackObject(TEXT("crate"), [&Crate] { return TOptional<FVector>(Crate); }, TEXT("crate"));
	Client->Tick(0.0);
	TestEqual(TEXT("A NaN registration position is not sent"), Records(*Client, TEXT("$tf_objects")).Num(), 0);
	TestEqual(TEXT("A NaN position does not end the object"), Client->GetTrackedObjectCount(), 1);
	Crate = FVector(100.0, 0.0, 100.0);
	Client->Tick(0.25);
	auto Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 1, "The first finite position is sent");
	TestEqual(TEXT("First finite position"), Array(Objects[0], TEXT("sample.x")), FString(TEXT("100")));
	Crate = FVector(500.0, std::numeric_limits<double>::infinity(), 500.0);
	Client->Tick(0.5);
	TestEqual(TEXT("An infinite position is skipped"), Records(*Client, TEXT("$tf_objects")).Num(), 1);
	TestEqual(TEXT("An infinite position does not end the object"), Client->GetTrackedObjectCount(), 1);
	Crate = FVector(500.0, 0.0, 500.0);
	Client->Tick(0.75);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 2, "The next finite position is sent");
	TestEqual(TEXT("Next finite position"), Array(Objects[1], TEXT("sample.x")), FString(TEXT("500")));
	for (const TestingFloor::FEvent& Event : Client->GetQueuedEvents())
	{
		const FString Json = TestingFloor::SerializeEvent(Event, Settings());
		TestFalse(TEXT("No non-finite number reaches the wire"), Json.Contains(TEXT("nan"), ESearchCase::IgnoreCase) || Json.Contains(TEXT("inf"), ESearchCase::IgnoreCase));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorObjectSamplingTest, "TestingFloor.Sdk.Schema2.ObjectSamplingAndRemoval", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorObjectSamplingTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const auto Client = Create(Settings());
	Client->SetContextProvider([](TestingFloor::FProperties& Context) {
		Context.Set(TEXT("game_level.id"), TEXT("harbor")).Set(TEXT("game_level.variant"), TEXT("night")).Set(TEXT("player.hp"), 90);
	});
	TestingFloor::FActivitySample Sample;
	Sample.PlayerPosition = FVector(5.0, 5.0, 0.0);
	Client->SetActivitySource([&Sample] { return Sample; });
	Client->Track(TEXT("level_loaded"));

	TOptional<FVector> Crate = FVector(12.4, -0.3, 990.0);
	const FVector Guard(1241.0, 0.0, 4813.0);
	const uint64 CrateToken = Client->TrackObject(TEXT("crate"), [&Crate] { return Crate; });
	const uint64 GuardToken = Client->TrackObject(TEXT("guard"), [&Guard] { return TOptional<FVector>(Guard); }, TEXT("captain"));
	TF_REQUIRE_TRUE(CrateToken != 0 && GuardToken != 0, "Registration returns live handles");
	Client->SetObjectState(GuardToken, TEXT("behavior"), TEXT("patrol"));
	Client->Tick(0.0);
	auto Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 1, "Registration sends a sample at the next check");
	TestEqual(TEXT("Generated and supplied IDs"), Array(Objects[0], TEXT("sample.id")), FString(TEXT("crate#1,captain")));
	TestEqual(TEXT("Kinds"), Array(Objects[0], TEXT("sample.kind")), FString(TEXT("crate,guard")));
	TestEqual(TEXT("Native units rounded to 1 cm"), Array(Objects[0], TEXT("sample.x")), FString(TEXT("12,1241")));
	TestEqual(TEXT("Rounded y"), Array(Objects[0], TEXT("sample.y")), FString(TEXT("0,0")));
	TestEqual(TEXT("State rows"), Array(Objects[0], TEXT("state.value")), FString(TEXT("patrol")));
	TestEqual(TEXT("Level values copied from the latest event"), Objects[0]->GetStringField(TEXT("game_level.variant")), FString(TEXT("night")));
	TestEqual(TEXT("Objects carry only level values and arrays"), Keys(Objects[0]),
		FString(TEXT("game_level.id,game_level.variant,sample.id,sample.kind,sample.x,sample.y,sample.z,state.id,state.key,state.value")));

	Crate = FVector(36.0, -0.3, 990.0);
	Client->Tick(0.25);
	TestEqual(TEXT("23.6 cm is under the 0.25 m threshold"), Records(*Client, TEXT("$tf_objects")).Num(), 1);
	Crate = FVector(37.5, -0.3, 990.0);
	Client->Tick(0.5);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 2, "25.1 cm from the last sent sample is sent");
	TestEqual(TEXT("Only the moved object"), Array(Objects[1], TEXT("sample.id")), FString(TEXT("crate#1")));
	TestEqual(TEXT("Rounded x"), Array(Objects[1], TEXT("sample.x")), FString(TEXT("38")));
	Crate = FVector(1000.0, 0.0, 990.0);
	Client->Tick(0.6);
	TestEqual(TEXT("Checks run 4 times a second"), Records(*Client, TEXT("$tf_objects")).Num(), 2);
	Client->Tick(0.75);
	TestEqual(TEXT("The next check sends the move"), Records(*Client, TEXT("$tf_objects")).Num(), 3);
	Client->SetObjectState(GuardToken, TEXT("behavior"), TEXT("patrol"));
	Client->Tick(1.0);
	TestEqual(TEXT("Nothing changed, no record"), Records(*Client, TEXT("$tf_objects")).Num(), 3);

	Client->SetObjectState(GuardToken, TEXT("behavior"), TEXT("searching"));
	Client->RemoveObject(GuardToken, TEXT("knocked_out"));
	TestEqual(TEXT("Removal frees the slot"), Client->GetTrackedObjectCount(), 1);
	Client->Tick(1.25);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 4, "Removal record");
	TestEqual(TEXT("Removal sends the final position"), Array(Objects[3], TEXT("sample.id")), FString(TEXT("captain")));
	TestEqual(TEXT("Final position"), Array(Objects[3], TEXT("sample.z")), FString(TEXT("4813")));
	TestEqual(TEXT("Last state change goes with the removal"), Array(Objects[3], TEXT("state.value")), FString(TEXT("searching")));
	TestEqual(TEXT("Removed ID"), Array(Objects[3], TEXT("removed.id")), FString(TEXT("captain")));
	TestEqual(TEXT("Game reason"), Array(Objects[3], TEXT("removed.reason")), FString(TEXT("knocked_out")));

	Crate.Reset();
	Client->Tick(1.5);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 5, "Gone record");
	TestEqual(TEXT("A source with no position removes the object as gone"), Array(Objects[4], TEXT("removed.reason")), FString(TEXT("gone")));
	TestEqual(TEXT("Gone sends the last known position"), Array(Objects[4], TEXT("sample.x")), FString(TEXT("1000")));
	TestEqual(TEXT("No live objects"), Client->GetTrackedObjectCount(), 0);

	Client->SetObjectState(GuardToken, TEXT("behavior"), TEXT("ignored"));
	Client->RemoveObject(0, TEXT("destroyed"));
	Client->Tick(1.75);
	TestEqual(TEXT("Stale and zero handles do nothing"), Records(*Client, TEXT("$tf_objects")).Num(), 5);
	const uint64 Next = Client->TrackObject(TEXT("crate"), [] { return TOptional<FVector>(FVector::ZeroVector); });
	Client->Tick(2.0);
	TestTrue(TEXT("Second handle"), Next != 0);
	TestEqual(TEXT("Generated IDs count per kind"), Array(Records(*Client, TEXT("$tf_objects")).Last(), TEXT("sample.id")), FString(TEXT("crate#2")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorObjectFirstSampleTest, "TestingFloor.Sdk.Schema2.ObjectFirstSample", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorObjectFirstSampleTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const auto Client = Create(Settings());
	FVector Drone = FVector::ZeroVector;
	FVector Cart = FVector::ZeroVector;
	FVector Rocket(0.0, 0.0, 100.0);
	Client->TrackObject(TEXT("drone"), [&Drone] { return TOptional<FVector>(Drone); }, TEXT("drone"));
	Client->TrackObject(TEXT("cart"), [&Cart] { return TOptional<FVector>(Cart); }, TEXT("cart"));
	const uint64 RocketToken = Client->TrackObject(TEXT("rocket"), [&Rocket] { return TOptional<FVector>(Rocket); }, TEXT("rocket"));
	Drone = FVector(30.0, 0.0, 0.0);
	Cart = FVector(10.0, 0.0, 0.0);
	Rocket = FVector(100.0, 0.0, 100.0);
	Client->RemoveObject(RocketToken, TEXT("exploded"));
	Client->Tick(0.0);
	auto Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 1, "One record for the first check");
	TestEqual(TEXT("Registration position first, then a move of 0.25 m in the same record"), Array(Objects[0], TEXT("sample.id")), FString(TEXT("drone,drone,cart,rocket,rocket")));
	TestEqual(TEXT("A smaller move keeps only the registration position; a removed object gets a start and an end"), Array(Objects[0], TEXT("sample.x")), FString(TEXT("0,30,0,0,100")));
	TestEqual(TEXT("Removal follows its samples"), Array(Objects[0], TEXT("removed.reason")), FString(TEXT("exploded")));
	Cart = FVector(25.0, 0.0, 0.0);
	Client->Tick(0.25);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 2, "The registration sample is the baseline for the next move");
	TestEqual(TEXT("25 cm from the registration position"), Array(Objects[1], TEXT("sample.id")), FString(TEXT("cart")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorObjectKeyframeTest, "TestingFloor.Sdk.Schema2.ObjectKeyframes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorObjectKeyframeTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	auto ClientSettings = Settings();
	ClientSettings.QueueMaxEvents = 3;
	const auto Transport = MakeShared<FTransport>();
	const auto Client = Create(ClientSettings, Transport);
	const uint64 Door = Client->TrackObject(TEXT("door"), [] { return TOptional<FVector>(FVector(100.0, 0.0, 0.0)); }, TEXT("vault_door"));
	Client->SetObjectState(Door, TEXT("open"), false);
	Client->SetObjectState(Door, TEXT("label"), TEXT("Vault"));
	Client->Tick(0.0);
	Client->Tick(299.75);
	auto Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 1, "A still object costs nothing");
	TestFalse(TEXT("Registration is not a keyframe"), IsKeyframe(Objects[0]));
	Client->Tick(300.0);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 2, "Keyframe every 5 minutes");
	TestTrue(TEXT("Periodic keyframe"), IsKeyframe(Objects[1]));
	TestEqual(TEXT("Keyframe repeats the unmoved object"), Array(Objects[1], TEXT("sample.id")), FString(TEXT("vault_door")));
	TestEqual(TEXT("Keyframe repeats full state"), Array(Objects[1], TEXT("state.key")), FString(TEXT("open,label")));

	Client->Track(TEXT("one"));
	TestFalse(TEXT("A full queue drops the event"), Client->Track(TEXT("two")));
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Complete(200), "Queue drained");
	Client->Tick(300.25);
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Objects.Num() == 1, "A drop triggers an object keyframe at the next check");
	TestTrue(TEXT("Recovery keyframe"), IsKeyframe(Objects[0]));
	TestEqual(TEXT("Recovery keyframe repeats live objects"), Array(Objects[0], TEXT("sample.id")), FString(TEXT("vault_door")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorObjectLimitsTest, "TestingFloor.Sdk.Schema2.ObjectSplitLimitAndDisabled", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorObjectLimitsTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	auto ManySettings = Settings();
	ManySettings.MaxTrackedObjects = 650;
	const auto Many = Create(ManySettings);
	for (int32 Index = 0; Index < 650; ++Index)
	{
		Many->TrackObject(TEXT("prop"), [Index] { return TOptional<FVector>(FVector(Index * 100.0, 0.0, 0.0)); });
	}
	Many->Tick(0.0);
	const auto Split = Records(*Many, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(Split.Num() == 3, "650 samples split into three records");
	TestEqual(TEXT("First record holds 300 samples"), Count(Split[0], TEXT("sample.id")), 300);
	TestEqual(TEXT("Second record holds 300 samples"), Count(Split[1], TEXT("sample.id")), 300);
	TestEqual(TEXT("Third record holds the rest"), Count(Split[2], TEXT("sample.id")), 50);
	TestEqual(TEXT("Split records share the check's time"), Split[0]->GetStringField(TEXT("$time")), Split[2]->GetStringField(TEXT("$time")));
	TestEqual(TEXT("Split records are consecutive"), Seq(Split[0]) + 2, Seq(Split[2]));

	const auto Capped = Create(Settings());
	for (int32 Index = 0; Index < 256; ++Index)
	{
		Capped->TrackObject(TEXT("prop"), [] { return TOptional<FVector>(FVector::ZeroVector); });
	}
	AddExpectedMessage(TEXT("objects are already live"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("Registration over the limit is ignored"), Capped->TrackObject(TEXT("prop"), [] { return TOptional<FVector>(FVector::ZeroVector); }), 0ULL);
	TestEqual(TEXT("Ignored again, with only one warning"), Capped->TrackObject(TEXT("prop"), [] { return TOptional<FVector>(FVector::ZeroVector); }), 0ULL);
	TestEqual(TEXT("256 objects live"), Capped->GetTrackedObjectCount(), 256);
	const uint64 Replacement = Capped->TrackObject(TEXT("prop"), [] { return TOptional<FVector>(FVector::ZeroVector); }, TEXT("prop#1"));
	TestTrue(TEXT("Registering a live ID again replaces it, even at the limit"), Replacement != 0);
	TestEqual(TEXT("Replacement keeps the count"), Capped->GetTrackedObjectCount(), 256);

	auto DisabledSettings = Settings();
	DisabledSettings.bEnabled = false;
	const auto Disabled = Create(DisabledSettings);
	bool bSampled = false;
	const uint64 Ignored = Disabled->TrackObject(TEXT("guard"), [&bSampled] {
		bSampled = true;
		return TOptional<FVector>(FVector::ZeroVector);
	});
	TestEqual(TEXT("Disabled collection returns a handle that does nothing"), Ignored, 0ULL);
	Disabled->SetObjectState(Ignored, TEXT("behavior"), TEXT("patrol"));
	Disabled->RemoveObject(Ignored, TEXT("destroyed"));
	Disabled->Tick(0.0);
	Disabled->Tick(1.0);
	TestFalse(TEXT("Disabled collection samples nothing"), bSampled);
	TestEqual(TEXT("Disabled collection queues nothing"), Disabled->GetPendingEventCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorCompletionRecordsTest, "TestingFloor.Sdk.Schema2.RecordingCompletion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorCompletionRecordsTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorSchemaTests;
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("TestingFloorTests") / FGuid::NewGuid().ToString();
	IFileManager::Get().MakeDirectory(*Directory, true);
	auto ClientSettings = Settings();
	TestingFloor::FRecordingContext Recording;
	Recording.RecordingUUID = TEXT("example-recording");
	Recording.TelemetryDirectory = Directory;
	ClientSettings.Recording = Recording;
	ClientSettings.QueueMaxEvents = 3;
	const auto Transport = MakeShared<FTransport>();
	const auto Client = Create(ClientSettings, Transport);
	int32 Volume = 1;
	Client->SetStateProvider([&Volume](TestingFloor::FProperties& State) { State.Set(TEXT("pref.volume"), Volume); });
	FVector Crate = FVector::ZeroVector;
	Client->TrackObject(TEXT("crate"), [&Crate] { return TOptional<FVector>(Crate); }, TEXT("crate"));
	Client->Tick(0.0);
	auto States = Records(*Client, TEXT("$tf_state"));
	auto Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(States.Num() == 1 && Objects.Num() == 1, "Registration sends state and objects");
	TestTrue(TEXT("Registration starts a state keyframe"), IsKeyframe(States[0]));
	TestTrue(TEXT("Registration starts an object keyframe"), IsKeyframe(Objects[0]));
	TestTrue(TEXT("State carries the recording"), HasRecording(States[0]));
	TestTrue(TEXT("Objects carry the recording"), HasRecording(Objects[0]));

	Client->Track(TEXT("level_loaded"));
	Volume = 2;
	Client->Tick(1.0);
	TestEqual(TEXT("A full queue drops the state change and the object keyframe it triggers"), Client->GetDroppedEventCount(), 2);
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Complete(200), "Queue drained");
	Client->Tick(2.0);
	States = Records(*Client, TEXT("$tf_state"));
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(States.Num() == 1 && Objects.Num() == 1, "Recovery records after the drops");
	TestTrue(TEXT("State keyframe after a drop"), IsKeyframe(States[0]));
	TestTrue(TEXT("Object keyframe after a drop"), IsKeyframe(Objects[0]));
	const int64 LastSeq = Seq(Objects[0]);
	Client->Flush();
	TF_REQUIRE_TRUE(Transport->Complete(200), "Recovery records delivered");

	FFileHelper::SaveStringToFile(TEXT("1"), *(Directory / TEXT("closed.json")));
	Volume = 3;
	Crate = FVector(100.0, 0.0, 0.0);
	Client->Tick(3.0);
	TF_REQUIRE_TRUE(Transport->Bodies.Num() == 3, "Closing the recording sends the marker in its own request");
	const TSharedPtr<FJsonObject> Body = Parse(Transport->Bodies.Last());
	TF_REQUIRE(Body.Get(), "Valid marker JSON");
	const TArray<TSharedPtr<FJsonValue>>& Sent = Body->GetArrayField(TEXT("events"));
	TF_REQUIRE_TRUE(Sent.Num() == 1, "Marker alone");
	const TSharedPtr<FJsonObject> Marker = Sent[0]->AsObject();
	TestEqual(TEXT("Marker"), Marker->GetStringField(TEXT("$name")), FString(TEXT("$tf_telemetry_complete")));
	TestEqual(TEXT("The marker takes the next seq"), Seq(Marker), LastSeq + 1);
	TestEqual(TEXT("Dropped state and object records count toward dropped_events"), Marker->GetIntegerField(TEXT("dropped_events")), 2);
	States = Records(*Client, TEXT("$tf_state"));
	Objects = Records(*Client, TEXT("$tf_objects"));
	TF_REQUIRE_TRUE(States.Num() == 1 && Objects.Num() == 1, "State and objects continue after the recording closes");
	TestFalse(TEXT("State after closure omits the recording"), HasRecording(States[0]));
	TestFalse(TEXT("Objects after closure omit the recording"), HasRecording(Objects[0]));
	TestTrue(TEXT("Records after the marker follow it"), Seq(States[0]) > Seq(Marker) && Seq(Objects[0]) > Seq(Marker));
	TF_REQUIRE_TRUE(Transport->Complete(200), "Marker acknowledged");
	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

#endif
