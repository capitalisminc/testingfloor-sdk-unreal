#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

/**
 * These tests compare what the client sends with the Testing Floor telemetry contract in
 * Tests/Contract. When one fails, change the SDK or have the contract changed and copy it again.
 * Never edit the copy to make a test pass.
 */
namespace TestingFloorContractTests
{
class FTransport final : public TestingFloor::ITransport
{
public:
	TArray<FString> Endpoints;
	TArray<FString> Bodies;
	TFunction<void(int32, bool)> Completion;
	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> InCompletion) override
	{
		Endpoints.Add(Endpoint);
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

TSharedPtr<FJsonObject> Parse(const FString& Json)
{
	TSharedPtr<FJsonObject> Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

/** The vendored contract. A missing or unreadable copy fails the calling test. */
TSharedPtr<FJsonObject> LoadContract(FAutomationTestBase& Test)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TestingFloor"));
	if (!Plugin.IsValid())
	{
		Test.AddError(TEXT("The TestingFloor plugin is not loaded"));
		return nullptr;
	}
	const FString Path = Plugin->GetBaseDir() / TEXT("Tests/Contract/telemetry_contract.json");
	FString Text;
	TSharedPtr<FJsonObject> Contract;
	if (FFileHelper::LoadFileToString(Text, *Path))
	{
		Contract = Parse(Text);
	}
	if (!Contract.IsValid())
	{
		Test.AddError(FString::Printf(TEXT("Cannot read the telemetry contract at %s"), *Path));
	}
	return Contract;
}

FString Field(const FJsonObject& Entry, const TCHAR* Name)
{
	FString Value;
	Entry.TryGetStringField(Name, Value);
	return Value;
}

TArray<TSharedPtr<FJsonObject>> Entries(const FJsonObject& Object, const TCHAR* Section)
{
	TArray<TSharedPtr<FJsonObject>> Result;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Object.TryGetArrayField(Section, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			const TSharedPtr<FJsonObject>* Entry = nullptr;
			if (Value.IsValid() && Value->TryGetObject(Entry))
			{
				Result.Add(*Entry);
			}
		}
	}
	return Result;
}

/** Declared keys of a contract section, or names with KeyField `name`, mapped to their declared types. */
TMap<FString, FString> Declared(const FJsonObject& Object, const TCHAR* Section, TFunction<bool(const FJsonObject&)> Filter = {}, const TCHAR* KeyField = TEXT("key"))
{
	TMap<FString, FString> Result;
	for (const TSharedPtr<FJsonObject>& Entry : Entries(Object, Section))
	{
		if (!Filter || Filter(*Entry))
		{
			Result.Add(Field(*Entry, KeyField), Field(*Entry, TEXT("type")));
		}
	}
	return Result;
}

TSharedPtr<FJsonObject> FindEntry(const FJsonObject& Contract, const TCHAR* Section, const TCHAR* KeyField, const FString& Key)
{
	for (const TSharedPtr<FJsonObject>& Candidate : Entries(Contract, Section))
	{
		if (Field(*Candidate, KeyField) == Key)
		{
			return Candidate;
		}
	}
	return nullptr;
}

int64 Limit(const FJsonObject& Contract, const TCHAR* Key)
{
	int64 Value = 0;
	const TSharedPtr<FJsonObject> Declaration = FindEntry(Contract, TEXT("limits"), TEXT("key"), Key);
	return Declaration.IsValid() && Declaration->TryGetNumberField(TEXT("value"), Value) ? Value : 0;
}

/** Whether a value has a contract type, such as integer, timestamp, number[] or scalar[]. */
bool HasType(const FJsonValue& Value, const FString& Type)
{
	if (Type.EndsWith(TEXT("[]")))
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (!Value.TryGetArray(Items))
		{
			return false;
		}
		const FString ItemType = Type.LeftChop(2);
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			if (!Item.IsValid() || !HasType(*Item, ItemType))
			{
				return false;
			}
		}
		return true;
	}
	if (Type == TEXT("scalar"))
	{
		return Value.Type == EJson::String || Value.Type == EJson::Boolean || HasType(Value, TEXT("number"));
	}
	if (Type == TEXT("string"))
	{
		return Value.Type == EJson::String;
	}
	if (Type == TEXT("timestamp"))
	{
		FDateTime Time;
		return Value.Type == EJson::String && FDateTime::ParseIso8601(*Value.AsString(), Time);
	}
	if (Type == TEXT("boolean"))
	{
		return Value.Type == EJson::Boolean;
	}
	if (Type == TEXT("object"))
	{
		return Value.Type == EJson::Object;
	}
	double Number = 0.0;
	if (Value.Type != EJson::Number || !Value.TryGetNumber(Number) || !FMath::IsFinite(Number))
	{
		return false;
	}
	return Type == TEXT("number") || (Type == TEXT("integer") && Number == FMath::RoundToDouble(Number));
}

/** Keys joined in sorted order, so a failure shows both sides. */
FString Sorted(TArray<FString> Keys)
{
	Keys.Sort();
	return FString::Join(Keys, TEXT(","));
}

FString Sorted(const TSet<FString>& Keys)
{
	return Sorted(Keys.Array());
}

FString Sorted(const TMap<FString, FString>& Declarations)
{
	TArray<FString> Keys;
	Declarations.GetKeys(Keys);
	return Sorted(MoveTemp(Keys));
}

TArray<FString> Keys(const FJsonObject& Object)
{
	TArray<FString> Result;
	for (const auto& Pair : Object.Values)
	{
		Result.Add(FString(Pair.Key.ToView()));
	}
	return Result;
}

/** Every declared key in a record has its declared type. */
void TestTypes(FAutomationTestBase& Test, const FString& What, const FJsonObject& Record, const TMap<FString, FString>& Types)
{
	for (const auto& Pair : Record.Values)
	{
		const FString Key(Pair.Key.ToView());
		if (const FString* Type = Types.Find(Key))
		{
			Test.TestTrue(FString::Printf(TEXT("%s %s is %s"), *What, *Key, **Type), Pair.Value.IsValid() && HasType(*Pair.Value, *Type));
		}
	}
}

/** Every event of every request, in the order the collector receives them. */
TArray<TSharedPtr<FJsonObject>> Sent(const FTransport& Transport)
{
	TArray<TSharedPtr<FJsonObject>> Result;
	for (const FString& Body : Transport.Bodies)
	{
		const TSharedPtr<FJsonObject> Request = Parse(Body);
		const TArray<TSharedPtr<FJsonValue>>* Events = nullptr;
		if (Request.IsValid() && Request->TryGetArrayField(TEXT("events"), Events))
		{
			for (const TSharedPtr<FJsonValue>& Event : *Events)
			{
				const TSharedPtr<FJsonObject>* Object = nullptr;
				if (Event.IsValid() && Event->TryGetObject(Object))
				{
					Result.Add(*Object);
				}
			}
		}
	}
	return Result;
}

TArray<TSharedPtr<FJsonObject>> Named(const TArray<TSharedPtr<FJsonObject>>& Records, const FString& Name)
{
	return Records.FilterByPredicate([&Name](const TSharedPtr<FJsonObject>& Record) { return Field(*Record, TEXT("$name")) == Name; });
}

/**
 * A recorded run that sends every record the SDK writes: a gameplay event, session state, tracked
 * objects with samples, state and a removal, and the completion marker. Every optional identity and
 * context value is set, so every envelope and `$tf` key the SDK can write appears.
 */
struct FRecordedRun
{
	TestingFloor::FSettings Settings;
	TSharedRef<FTransport> Transport = MakeShared<FTransport>();
	TArray<TSharedPtr<FJsonObject>> Records;
};

FRecordedRun RunRecording()
{
	FRecordedRun Run;
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("TestingFloorTests") / FGuid::NewGuid().ToString();
	IFileManager::Get().MakeDirectory(*Directory, true);
	TestingFloor::FSettings& RunSettings = Run.Settings;
	// The default endpoint, so the test can compare the request URL; the transport is a mock.
	RunSettings.WriteKey = TEXT("test-only-never-transmitted");
	RunSettings.FlushIntervalSeconds = 1000.0;
	RunSettings.DeviceID = TEXT("example-installation");
	RunSettings.UserID = TEXT("example-player");
	RunSettings.AppVersion = TEXT("1.2.3");
	RunSettings.EngineVersion = TEXT("5.0.0");
	RunSettings.OperatingSystem = TEXT("Mac");
	RunSettings.Locale = TEXT("en-US");
	RunSettings.DeviceModel = TEXT("example-device");
	TestingFloor::FRecordingContext Recording;
	Recording.RecordingUUID = TEXT("example-recording");
	Recording.TelemetryDirectory = Directory;
	Recording.PlaytestID = 42;
	RunSettings.Recording = Recording;
	{
		const auto Client = TestingFloor::FClient::Create(RunSettings, {}, Run.Transport);
		TestingFloor::FActivitySample Sample;
		Sample.PlayerPosition = FVector(100.0, 200.0, 0.0);
		Sample.ViewportSize = FIntPoint(1920, 1080);
		Client->SetActivitySource([Sample] { return Sample; });
		Client->SetContextProvider([](TestingFloor::FProperties& Context) { Context.Set(TEXT("game_level.id"), TEXT("example-level")); });
		Client->SetState(TEXT("difficulty"), TEXT("hard"));
		const uint64 Crate = Client->TrackObject(TEXT("crate"), [] { return TOptional<FVector>(FVector(100.0, 0.0, 0.0)); }, TEXT("crate-1"));
		Client->SetObjectState(Crate, TEXT("label"), TEXT("Supply crate"));
		Client->SetObjectState(Crate, TEXT("health"), 100);
		Client->SetObjectState(Crate, TEXT("open"), false);
		Client->Track(TEXT("level_started"));
		Client->Tick(0.0);
		Client->RemoveObject(Crate, TEXT("destroyed"));
		Client->Tick(0.25);
		Client->Flush();
		Run.Transport->Complete(200);
		FFileHelper::SaveStringToFile(TEXT("{}"), *(Directory / TEXT("closed.json")));
		for (double Now = 0.5; Now <= 2.0; Now += 0.25)
		{
			Client->Tick(Now);
			Run.Transport->Complete(200);
		}
		Client->Shutdown(0.0);
	}
	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	Run.Records = Sent(*Run.Transport);
	return Run;
}

/** A run that sends every SDK-owned property: pose on events, viewport and FOV axis in state, movement and a teleport. */
TArray<TSharedPtr<FJsonObject>> RunActivity()
{
	auto RunSettings = Settings();
	RunSettings.bMovementTracking = true;
	const auto Transport = MakeShared<FTransport>();
	{
		const auto Client = TestingFloor::FClient::Create(RunSettings, {}, Transport);
		TestingFloor::FActivitySample Sample;
		Sample.PlayerPosition = FVector::ZeroVector;
		Sample.CameraPosition = FVector(0.0, 0.0, 150.0);
		Sample.CameraRotation = FQuat::Identity;
		Sample.CameraFOV = 90.0;
		Sample.ViewportSize = FIntPoint(1920, 1080);
		Client->SetActivitySource([&Sample] { return Sample; });
		int32 Steps = 0;
		// Each step moves the player and camera 1 m, turns the camera 10 degrees and widens the FOV 5 degrees.
		const auto Step = [&Sample, &Steps] {
			++Steps;
			Sample.PlayerPosition = FVector(100.0 * Steps, 0.0, 0.0);
			Sample.CameraPosition = FVector(100.0 * Steps, 0.0, 150.0);
			Sample.CameraRotation = FRotator(0.0, 10.0 * Steps, 0.0).Quaternion();
			Sample.CameraFOV = 90.0 + 5.0 * Steps;
		};
		Client->Tick(0.0);
		Step();
		Client->Tick(0.1);
		Step();
		Client->Tick(0.2);
		Step();
		Client->Tick(0.8);
		Client->Tick(1.2);
		Client->RecordTeleport(FVector::ZeroVector, FVector(1000.0, 0.0, 0.0), TEXT("respawn"), 1.5);
		Client->Flush();
	}
	return Sent(*Transport);
}
} // namespace TestingFloorContractTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractRequestTest, "TestingFloor.Sdk.Contract.RequestEnvelope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractRequestTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const FRecordedRun Run = RunRecording();
	TF_REQUIRE_TRUE(Run.Transport->Bodies.Num() >= 2, "The run sends records and the completion marker");
	const TMap<FString, FString> Fields = Declared(*Contract, TEXT("request_fields"));
	const TMap<FString, FString> Required = Declared(*Contract, TEXT("request_fields"), [](const FJsonObject& Entry) { return Entry.GetBoolField(TEXT("required")); });
	for (const FString& Body : Run.Transport->Bodies)
	{
		const TSharedPtr<FJsonObject> Request = Parse(Body);
		TF_REQUIRE(Request.Get(), "Each request is a JSON object");
		const TArray<FString> RequestKeys = Keys(*Request);
		TestEqual(TEXT("Request keys are the contract's request fields"), Sorted(RequestKeys), Sorted(Fields));
		for (const auto& Pair : Required)
		{
			TestTrue(FString::Printf(TEXT("Request has required %s"), *Pair.Key), RequestKeys.Contains(Pair.Key));
		}
		TestTypes(*this, TEXT("Request"), *Request, Fields);
	}
	const TSharedPtr<FJsonObject> Endpoint = Contract->GetObjectField(TEXT("endpoint"));
	for (const FString& Url : Run.Transport->Endpoints)
	{
		TestEqual(TEXT("The default endpoint posts to the contract URL"), Url, Field(*Endpoint, TEXT("url")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractEventTest, "TestingFloor.Sdk.Contract.EventEnvelope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractEventTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const FRecordedRun Run = RunRecording();
	TF_REQUIRE_TRUE(!Run.Records.IsEmpty(), "Records sent");
	const TMap<FString, FString> Fields = Declared(*Contract, TEXT("event_fields"));
	const TMap<FString, FString> Required = Declared(*Contract, TEXT("event_fields"), [](const FJsonObject& Entry) { return Entry.GetBoolField(TEXT("required")); });
	const int64 Schema = Contract->GetIntegerField(TEXT("wire_schema_version"));
	TArray<int64> Accepted;
	for (const TSharedPtr<FJsonValue>& Version : Contract->GetArrayField(TEXT("accepted_wire_schema_versions")))
	{
		Accepted.Add(static_cast<int64>(Version->AsNumber()));
	}
	TestTrue(TEXT("The collector accepts the current wire schema"), Accepted.Contains(Schema));
	TSet<FString> Envelope;
	for (const TSharedPtr<FJsonObject>& Record : Run.Records)
	{
		const FString Name = Field(*Record, TEXT("$name"));
		for (const FString& Key : Keys(*Record))
		{
			if (Key.StartsWith(TEXT("$")))
			{
				Envelope.Add(Key);
			}
		}
		for (const auto& Pair : Required)
		{
			TestTrue(FString::Printf(TEXT("%s has required %s"), *Name, *Pair.Key), Record->HasField(Pair.Key));
		}
		TestTypes(*this, Name, *Record, Fields);
		int64 RecordSchema = 0;
		TestTrue(FString::Printf(TEXT("%s sends the contract's wire schema"), *Name), Record->TryGetNumberField(TEXT("$schema"), RecordSchema) && RecordSchema == Schema);
		const FString Time = Field(*Record, TEXT("$time"));
		TestTrue(FString::Printf(TEXT("%s $time is UTC with milliseconds: %s"), *Name, *Time), Time.Len() == 24 && Time[19] == TEXT('.') && Time.EndsWith(TEXT("Z")));
	}
	// With identities set, the SDK writes every envelope field the contract declares and no other `$` key.
	TestEqual(TEXT("Envelope keys are the contract's event fields"), Sorted(Envelope), Sorted(Fields));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractContextTest, "TestingFloor.Sdk.Contract.ContextFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractContextTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const FRecordedRun Run = RunRecording();
	TF_REQUIRE_TRUE(!Run.Records.IsEmpty(), "Records sent");
	const TMap<FString, FString> SdkFields = Declared(*Contract, TEXT("context_fields"), [](const FJsonObject& Entry) { return Field(Entry, TEXT("set_by")) == TEXT("sdk"); });
	TSet<FString> ContextKeys;
	for (const TSharedPtr<FJsonObject>& Record : Run.Records)
	{
		const TSharedPtr<FJsonObject>* Context = nullptr;
		TF_REQUIRE_TRUE(Record->TryGetObjectField(TEXT("$tf"), Context) && Context && Context->IsValid(), "Every record has $tf");
		ContextKeys.Append(Keys(**Context));
		TestTypes(*this, TEXT("$tf"), **Context, SdkFields);
	}
	// Keys Testing Floor sets when it relays desktop app events must never come from the SDK.
	TestEqual(TEXT("$tf keys are the contract's SDK context fields"), Sorted(ContextKeys), Sorted(SdkFields));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractRecordsTest, "TestingFloor.Sdk.Contract.PlatformRecords", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractRecordsTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const FRecordedRun Run = RunRecording();
	const TMap<FString, FString> SdkRecords = Declared(*Contract, TEXT("platform_records"), [](const FJsonObject& Entry) {
		return Field(Entry, TEXT("sent_by")) == TEXT("sdk") && Field(Entry, TEXT("status")) == TEXT("current");
	}, TEXT("name"));
	TSet<FString> RecordNames;
	for (const TSharedPtr<FJsonObject>& Record : Run.Records)
	{
		const FString Name = Field(*Record, TEXT("$name"));
		if (Name.StartsWith(TEXT("$")))
		{
			RecordNames.Add(Name);
		}
	}
	TestEqual(TEXT("Platform record names are the contract's current SDK records"), Sorted(RecordNames), Sorted(SdkRecords));

	const TSharedPtr<FJsonObject> Completion = FindEntry(*Contract, TEXT("platform_records"), TEXT("id"), TEXT("telemetry_complete"));
	TF_REQUIRE(Completion.Get(), "Contract declares the completion record");
	const TMap<FString, FString> CompletionFields = Declared(*Completion, TEXT("fields"));
	const TArray<TSharedPtr<FJsonObject>> Markers = Named(Run.Records, Field(*Completion, TEXT("name")));
	TF_REQUIRE_TRUE(Markers.Num() == 1, "One completion marker");
	TArray<FString> MarkerFields = Keys(*Markers[0]).FilterByPredicate([](const FString& Key) { return !Key.StartsWith(TEXT("$")); });
	TestEqual(TEXT("Completion marker fields are the contract's"), Sorted(MarkerFields), Sorted(CompletionFields));
	TestTypes(*this, TEXT("Completion marker"), *Markers[0], CompletionFields);

	// Properties the contract reserves for Testing Floor or retired SDKs are never sent.
	for (const TSharedPtr<FJsonObject>& Reserved : Entries(*Contract, TEXT("reserved")))
	{
		if (Field(*Reserved, TEXT("kind")) == TEXT("property"))
		{
			const FString Key = Field(*Reserved, TEXT("pattern"));
			TestFalse(FString::Printf(TEXT("No record sends reserved %s"), *Key), Run.Records.ContainsByPredicate([&Key](const TSharedPtr<FJsonObject>& Record) { return Record->HasField(Key); }));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractObjectsTest, "TestingFloor.Sdk.Contract.TrackedObjectFields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractObjectsTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const FRecordedRun Run = RunRecording();
	const TSharedPtr<FJsonObject> Objects = FindEntry(*Contract, TEXT("platform_records"), TEXT("id"), TEXT("objects"));
	TF_REQUIRE(Objects.Get(), "Contract declares the tracked-objects record");
	const TArray<TSharedPtr<FJsonObject>> Records = Named(Run.Records, Field(*Objects, TEXT("name")));
	TF_REQUIRE_TRUE(!Records.IsEmpty(), "Tracked-object records sent");
	const TMap<FString, FString> Fields = Declared(*Contract, TEXT("tracked_object_fields"));
	TSet<FString> ObjectKeys;
	TSet<FString> Level;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		for (const FString& Key : Keys(*Record))
		{
			// Tracked-object records copy game_level.* values from the latest event.
			if (!Key.StartsWith(TEXT("$")))
			{
				(Key.StartsWith(TEXT("game_level.")) ? Level : ObjectKeys).Add(Key);
			}
		}
		TestTypes(*this, TEXT("Tracked objects"), *Record, Fields);
	}
	TestEqual(TEXT("Tracked-object keys are the contract's tracked-object fields"), Sorted(ObjectKeys), Sorted(Fields));
	TestTrue(TEXT("Records copy the suggested level ID"), Level.Contains(TEXT("game_level.id")) && FindEntry(*Contract, TEXT("conventional_properties"), TEXT("key"), TEXT("game_level.id")).IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractPropertiesTest, "TestingFloor.Sdk.Contract.SdkProperties", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractPropertiesTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const TArray<TSharedPtr<FJsonObject>> Records = RunActivity();
	TF_REQUIRE_TRUE(!Records.IsEmpty(), "Records sent");
	// The SDK writes its own current properties and every property of its own events, including a game's teleport reason.
	TMap<FString, TSharedPtr<FJsonObject>> Owned;
	for (const TSharedPtr<FJsonObject>& Property : Entries(*Contract, TEXT("conventional_properties")))
	{
		if (Field(*Property, TEXT("status")) == TEXT("current") && (Field(*Property, TEXT("set_by")) == TEXT("sdk") || Property->HasField(TEXT("event"))))
		{
			Owned.Add(Field(*Property, TEXT("key")), Property);
		}
	}
	TMap<FString, FString> Types;
	for (const auto& Pair : Owned)
	{
		Types.Add(Pair.Key, Field(*Pair.Value, TEXT("type")));
	}
	const TMap<FString, FString> Events = Declared(*Contract, TEXT("sdk_events"), {}, TEXT("name"));
	TSet<FString> PropertyKeys;
	TSet<FString> EventNames;
	for (const TSharedPtr<FJsonObject>& Record : Records)
	{
		const FString Name = Field(*Record, TEXT("$name"));
		if (!Name.StartsWith(TEXT("$")))
		{
			EventNames.Add(Name);
		}
		for (const FString& Key : Keys(*Record))
		{
			if (Key.StartsWith(TEXT("$")))
			{
				continue;
			}
			PropertyKeys.Add(Key);
			const TSharedPtr<FJsonObject>* Property = Owned.Find(Key);
			if (!Property)
			{
				continue;
			}
			FString Event;
			if ((*Property)->TryGetStringField(TEXT("event"), Event))
			{
				TestEqual(FString::Printf(TEXT("%s is sent only on %s"), *Key, *Event), Name, Event);
			}
			else if (Field(**Property, TEXT("description")).StartsWith(TEXT("Session state")))
			{
				TestEqual(FString::Printf(TEXT("%s is session state"), *Key), Name, FString(TEXT("$tf_state")));
			}
			else
			{
				TestFalse(FString::Printf(TEXT("%s is sent on events, not on %s"), *Key, *Name), Name.StartsWith(TEXT("$")));
			}
		}
		TestTypes(*this, Name, *Record, Types);
	}
	TestEqual(TEXT("SDK property keys are the contract's SDK properties"), Sorted(PropertyKeys), Sorted(Types));
	TestEqual(TEXT("SDK event names are the contract's SDK events"), Sorted(EventNames), Sorted(Events));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractValuesTest, "TestingFloor.Sdk.Contract.ValueTypes", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractValuesTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	TArray<FString> Allowed;
	for (const TSharedPtr<FJsonValue>& Type : Contract->GetObjectField(TEXT("value_types"))->GetArrayField(TEXT("allowed")))
	{
		Allowed.Add(Type->AsString());
	}
	TArray<FString> Disallowed;
	for (const TSharedPtr<FJsonObject>& Reserved : Entries(*Contract, TEXT("reserved")))
	{
		if (Field(*Reserved, TEXT("kind")) == TEXT("property_prefix") && Field(*Reserved, TEXT("rule")) == TEXT("disallowed"))
		{
			Disallowed.Add(Field(*Reserved, TEXT("pattern")) + TEXT("example"));
		}
	}
	TF_REQUIRE_TRUE(!Disallowed.IsEmpty(), "Contract reserves a property prefix");
	TestingFloor::FProperties Properties;
	Properties.Set(TEXT("example.text"), TEXT("value"))
		.Set(TEXT("example.count"), 3)
		.Set(TEXT("example.large"), static_cast<int64>(1) << 40)
		.Set(TEXT("example.ratio"), 0.5f)
		.Set(TEXT("example.precise"), 0.25)
		.Set(TEXT("example.flag"), true)
		.Set(TEXT("example.guid"), FGuid::NewGuid())
		.Set(TEXT("example.tags"), TArray<FString>{ TEXT("a"), TEXT("b") })
		.Set(TEXT("example.ids"), TArray<int32>{ 1, 2 })
		.Set(TEXT("example.nan"), std::numeric_limits<double>::quiet_NaN())
		.Set(TEXT("example.infinite"), std::numeric_limits<double>::infinity());
	for (const FString& Key : Disallowed)
	{
		Properties.Set(Key, 1);
	}
	const auto Transport = MakeShared<FTransport>();
	const auto Client = TestingFloor::FClient::Create(Settings(), {}, Transport);
	TF_REQUIRE_TRUE(Client->Track(TEXT("example_event"), Properties), "Event queued");
	Client->Flush();
	const TArray<TSharedPtr<FJsonObject>> Events = Named(Sent(*Transport), TEXT("example_event"));
	TF_REQUIRE_TRUE(Events.Num() == 1, "Event sent");
	const FJsonObject& Event = *Events[0];
	for (const auto& Pair : Event.Values)
	{
		const FString Key(Pair.Key.ToView());
		if (!Key.StartsWith(TEXT("$")))
		{
			TestTrue(FString::Printf(TEXT("%s has an allowed value type"), *Key), Pair.Value.IsValid() && Allowed.ContainsByPredicate([&Pair](const FString& Type) { return HasType(*Pair.Value, Type); }));
		}
	}
	TestEqual(TEXT("Only finite, allowed properties are sent"), Sorted(Keys(Event).FilterByPredicate([](const FString& Key) { return !Key.StartsWith(TEXT("$")); })),
		FString(TEXT("example.count,example.flag,example.guid,example.ids,example.large,example.precise,example.ratio,example.tags,example.text")));
	const TSharedPtr<FJsonValue> Guid = Event.TryGetField(TEXT("example.guid"));
	TestTrue(TEXT("GUIDs are sent as strings"), Guid.IsValid() && Guid->Type == EJson::String);
	for (const FString& Key : Disallowed)
	{
		TestFalse(FString::Printf(TEXT("Reserved property %s is dropped"), *Key), Event.HasField(Key));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractEventBytesTest, "TestingFloor.Sdk.Contract.EventBytesLimit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractEventBytesTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const int32 EventBytes = static_cast<int32>(Limit(*Contract, TEXT("event_bytes")));
	TF_REQUIRE_TRUE(EventBytes > 1000, "Contract declares an event size limit");
	const auto ClientSettings = Settings();
	const auto Transport = MakeShared<FTransport>();
	const auto Client = TestingFloor::FClient::Create(ClientSettings, {}, Transport);
	const auto Padded = [](int32 Length) { return TestingFloor::FProperties().Set(TEXT("padding"), FString::ChrN(Length, TEXT('x'))); };
	TF_REQUIRE_TRUE(Client->Track(TEXT("probe"), Padded(1000)), "Probe queued");
	const int32 ProbeBytes = FTCHARToUTF8(*TestingFloor::SerializeEvent(Client->GetQueuedEvents().Last(), ClientSettings)).Length();
	// Later events differ from the probe only in padding: their names, IDs, times and one-digit seq have the same length.
	const int32 Fill = 1000 + EventBytes - ProbeBytes;
	TF_REQUIRE_TRUE(Client->Track(TEXT("exact"), Padded(Fill)), "An event of exactly the limit is queued");
	TestEqual(TEXT("The queued event is exactly the limit"), FTCHARToUTF8(*TestingFloor::SerializeEvent(Client->GetQueuedEvents().Last(), ClientSettings)).Length(), EventBytes);
	TestFalse(TEXT("An event one byte over the limit is dropped"), Client->Track(TEXT("above"), Padded(Fill + 1)));
	Client->Flush();
	TestEqual(TEXT("The event at the limit is sent"), Named(Sent(*Transport), TEXT("exact")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractRequestLimitsTest, "TestingFloor.Sdk.Contract.RequestLimits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractRequestLimitsTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const int32 MaxEvents = static_cast<int32>(Limit(*Contract, TEXT("events_per_request")));
	const int32 RequestBytes = static_cast<int32>(Limit(*Contract, TEXT("request_bytes")));
	const int32 EventBytes = static_cast<int32>(Limit(*Contract, TEXT("event_bytes")));
	TF_REQUIRE_TRUE(MaxEvents > 0 && RequestBytes > EventBytes && EventBytes > 1000, "Contract declares request limits");

	auto ManySettings = Settings();
	ManySettings.BatchMaxEvents = MaxEvents * 2;
	ManySettings.QueueMaxEvents = MaxEvents + 10;
	const auto Many = MakeShared<FTransport>();
	{
		const auto Client = TestingFloor::FClient::Create(ManySettings, {}, Many);
		for (int32 Index = 0; Index <= MaxEvents; ++Index)
		{
			Client->Track(TEXT("e"));
		}
		Client->Flush();
	}
	TF_REQUIRE_TRUE(Many->Bodies.Num() == 1, "One request in flight");
	TestEqual(TEXT("A larger configured batch is capped at the contract's events per request"), Sent(*Many).Num(), MaxEvents);

	auto LargeSettings = Settings();
	LargeSettings.BatchMaxEvents = MaxEvents;
	const auto Large = MakeShared<FTransport>();
	const int32 Count = RequestBytes / (EventBytes / 2) + 4;
	{
		const auto Client = TestingFloor::FClient::Create(LargeSettings, {}, Large);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			TF_REQUIRE_TRUE(Client->Track(TEXT("large"), TestingFloor::FProperties().Set(TEXT("padding"), FString::ChrN(EventBytes - 1024, TEXT('x')))), "Large event queued");
		}
		Client->Flush();
	}
	TF_REQUIRE_TRUE(Large->Bodies.Num() == 1, "One request in flight");
	TestTrue(TEXT("The request is within the contract's request size"), FTCHARToUTF8(*Large->Bodies[0]).Length() <= RequestBytes);
	TestTrue(TEXT("Events that would exceed the request size wait for the next request"), Sent(*Large).Num() < Count);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContractObjectLimitsTest, "TestingFloor.Sdk.Contract.ObjectLimits", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContractObjectLimitsTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorContractTests;
	const TSharedPtr<FJsonObject> Contract = LoadContract(*this);
	TF_REQUIRE(Contract.Get(), "Contract loaded");
	const int32 Samples = static_cast<int32>(Limit(*Contract, TEXT("object_samples_per_record")));
	const int32 RecordBytes = static_cast<int32>(Limit(*Contract, TEXT("object_record_bytes")));
	const int32 LiveObjects = static_cast<int32>(Limit(*Contract, TEXT("live_tracked_objects")));
	TF_REQUIRE_TRUE(Samples > 1 && RecordBytes > 0 && LiveObjects > 0, "Contract declares object limits");
	const auto ObjectRecords = [](const TestingFloor::FClient& Client, const TestingFloor::FSettings& ClientSettings) {
		TArray<TSharedPtr<FJsonObject>> Result;
		for (const TestingFloor::FEvent& Event : Client.GetQueuedEvents())
		{
			if (Event.Name == TEXT("$tf_objects"))
			{
				Result.Add(Parse(TestingFloor::SerializeEvent(Event, ClientSettings)));
			}
		}
		return Result;
	};
	const auto SampleCount = [](const TSharedPtr<FJsonObject>& Record) {
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		return Record.IsValid() && Record->TryGetArrayField(TEXT("sample.id"), Values) ? Values->Num() : 0;
	};

	// Short IDs: the sample count, not the byte target, splits the check.
	auto ManySettings = Settings();
	ManySettings.MaxTrackedObjects = Samples + 1;
	{
		const auto Client = TestingFloor::FClient::Create(ManySettings, {}, MakeShared<FTransport>());
		for (int32 Index = 0; Index <= Samples; ++Index)
		{
			Client->TrackObject(TEXT("p"), [Index] { return TOptional<FVector>(FVector(Index * 100.0, 0.0, 0.0)); });
		}
		Client->Tick(0.0);
		const auto Records = ObjectRecords(*Client, ManySettings);
		TF_REQUIRE_TRUE(Records.Num() == 2, "One sample over the limit splits the check");
		TestEqual(TEXT("A record holds the contract's samples per record"), SampleCount(Records[0]), Samples);
		TestEqual(TEXT("The rest follow"), SampleCount(Records[1]), 1);
	}

	// IDs long enough that one record of this many samples would pass the byte target: the target splits the check.
	auto LongSettings = Settings();
	LongSettings.MaxTrackedObjects = Samples;
	{
		const auto Client = TestingFloor::FClient::Create(LongSettings, {}, MakeShared<FTransport>());
		const int32 IDLength = RecordBytes / Samples + 1;
		for (int32 Index = 0; Index < Samples; ++Index)
		{
			const FString Number = FString::FromInt(Index);
			Client->TrackObject(TEXT("p"), [Index] { return TOptional<FVector>(FVector(Index * 100.0, 0.0, 0.0)); }, FString::ChrN(IDLength - Number.Len(), TEXT('x')) + Number);
		}
		Client->Tick(0.0);
		const auto Records = ObjectRecords(*Client, LongSettings);
		TestTrue(TEXT("Records that would pass the byte target are split"), Records.Num() > 1);
		int32 Total = 0;
		for (const TSharedPtr<FJsonObject>& Record : Records)
		{
			Total += SampleCount(Record);
		}
		TestEqual(TEXT("Every sample is sent"), Total, Samples);
		for (const TestingFloor::FEvent& Event : Client->GetQueuedEvents())
		{
			if (Event.Name == TEXT("$tf_objects"))
			{
				TestTrue(TEXT("Each record is within the contract's record target"), FTCHARToUTF8(*TestingFloor::SerializeEvent(Event, LongSettings)).Length() <= RecordBytes);
			}
		}
	}

	TestEqual(TEXT("The default live-object limit is the contract's"), TestingFloor::FSettings().MaxTrackedObjects, LiveObjects);
	const auto Capped = TestingFloor::FClient::Create(Settings(), {}, MakeShared<FTransport>());
	int32 Accepted = 0;
	for (int32 Index = 0; Index < LiveObjects; ++Index)
	{
		Accepted += Capped->TrackObject(TEXT("p"), [] { return TOptional<FVector>(FVector::ZeroVector); }) != 0 ? 1 : 0;
	}
	TestEqual(TEXT("The limit's objects are live"), Accepted, LiveObjects);
	AddExpectedMessage(TEXT("objects are already live"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("A registration over the limit is ignored"), Capped->TrackObject(TEXT("p"), [] { return TOptional<FVector>(FVector::ZeroVector); }), 0ULL);
	return true;
}

#endif
