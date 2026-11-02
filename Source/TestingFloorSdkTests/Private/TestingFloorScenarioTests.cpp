#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloor/TestingFloorClient.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * Runs the shared SDK scenarios in Tests/Contract/scenarios, one automation test per file. The format is
 * in Tests/Contract/scenarios/README.md. When a scenario fails, fix the SDK, or have the contract changed
 * and copy it again. Never edit the copy to make a test pass.
 */
namespace TestingFloorScenarioTests
{
using FObject = TSharedPtr<FJsonObject>;
using FValue = TSharedPtr<FJsonValue>;

FString ScenarioDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("TestingFloor"));
	return Plugin.IsValid() ? Plugin->GetBaseDir() / TEXT("Tests/Contract/scenarios") : FString();
}

FObject ParseObject(const FString& Json)
{
	FObject Object;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object);
	return Object;
}

/** A payload given as an object is passed as compact JSON; a string is passed as it is. */
FString PayloadText(const FValue& Value)
{
	if (Value->Type == EJson::String)
	{
		return Value->AsString();
	}
	FString Text;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Value->AsObject().ToSharedRef(), Writer);
	return Text;
}

/** A JSON value as it would appear in a scenario, for failure messages. */
FString ValueText(const FValue& Value)
{
	switch (Value->Type)
	{
	case EJson::String:
		return TEXT("\"") + Value->AsString() + TEXT("\"");
	case EJson::Number:
		return FString::SanitizeFloat(Value->AsNumber(), 0);
	case EJson::Boolean:
		return Value->AsBool() ? TEXT("true") : TEXT("false");
	case EJson::Object:
		return PayloadText(Value);
	case EJson::Array:
		return TEXT("an array");
	default:
		return TEXT("null");
	}
}

FDateTime FromUnixMilliseconds(int64 Milliseconds)
{
	return FDateTime::FromUnixTimestamp(Milliseconds / 1000) + FTimespan::FromMilliseconds(static_cast<double>(Milliseconds % 1000));
}

FString JsonEscaped(const FString& Text)
{
	return Text.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
}

void RunHandoff(FAutomationTestBase& Test, const FJsonObject& Scenario)
{
	const FDateTime Now = FromUnixMilliseconds(static_cast<int64>(Scenario.GetNumberField(TEXT("clock_unix_ms"))));
	for (const FValue& Entry : Scenario.GetArrayField(TEXT("cases")))
	{
		const FJsonObject& Case = *Entry->AsObject();
		const FString Name = Case.GetStringField(TEXT("name"));
		const FValue Launch = Case.TryGetField(TEXT("launch_argument"));
		const FValue Sidecar = Case.TryGetField(TEXT("sidecar"));
		TArray<FString> Sidecars;
		if (Sidecar.IsValid())
		{
			Sidecars.Add(PayloadText(Sidecar));
		}
		int64 EditorProcessID = 0;
		Case.TryGetNumberField(TEXT("editor_process_id"), EditorProcessID);
		const FString CommandLine = Launch.IsValid() ? TEXT("--testing-floor=") + PayloadText(Launch) : FString();
		const auto Recording = TestingFloor::FRecordingContext::Resolve(CommandLine, Sidecars, Now, EditorProcessID);

		const FValue Expect = Case.TryGetField(TEXT("expect"));
		if (!Expect.IsValid() || Expect->IsNull())
		{
			Test.TestFalse(Name, Recording.IsSet());
			continue;
		}
		if (!Test.TestTrue(Name, Recording.IsSet()))
		{
			continue;
		}
		const FJsonObject& Fields = *Expect->AsObject();
		Test.TestEqual(Name + TEXT(": recording_uuid"), Recording->RecordingUUID, Fields.GetStringField(TEXT("recording_uuid")));
		if (const FValue Playtest = Fields.TryGetField(TEXT("playtest_id")))
		{
			const bool bMatches = Playtest->IsNull() ? !Recording->PlaytestID.IsSet()
				: Recording->PlaytestID.IsSet() && Recording->PlaytestID.GetValue() == static_cast<int64>(Playtest->AsNumber());
			Test.TestTrue(Name + TEXT(": playtest_id"), bMatches);
		}
		bool bSuppressQR = false;
		if (Fields.TryGetBoolField(TEXT("suppress_qr"), bSuppressQR))
		{
			Test.TestEqual(Name + TEXT(": suppress_qr"), Recording->bSuppressQR, bSuppressQR);
		}
	}
}

/** The fake collector. `accept` answers every request at once; `manual` waits for respond steps. */
class FCollector final : public TestingFloor::ITransport
{
public:
	bool bManual = false;
	TArray<FString> Bodies;
	TArray<TFunction<void(int32, bool)>> Open;
	virtual void Send(const FString&, const FString& Body, TFunction<void(int32, bool)> Completion) override
	{
		Bodies.Add(Body);
		if (bManual)
		{
			Open.Add(MoveTemp(Completion));
		}
		else
		{
			Completion(200, true);
		}
	}
};

class FLifecycle
{
public:
	FLifecycle(FAutomationTestBase& InTest, const FJsonObject& InScenario) : Test(InTest), Scenario(InScenario)
	{
	}

	void Run()
	{
		Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("TestingFloorTests") / FGuid::NewGuid().ToString());
		IFileManager::Get().MakeDirectory(*Directory, true);
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		ON_SCOPE_EXIT
		{
			if (Client.IsValid())
			{
				Client->Shutdown(0.0);
			}
			PlatformFile.SetReadOnly(*Directory, false);
			IFileManager::Get().DeleteDirectory(*Directory, false, true);
		};
		const TArray<FValue>* Requires = nullptr;
		if (Scenario.TryGetArrayField(TEXT("requires"), Requires)
			&& Requires->ContainsByPredicate([](const FValue& Value) { return Value->AsString() == TEXT("unwritable_directory"); }))
		{
			// A sandboxed macOS app can read the desktop app's directory but cannot create files in it.
			PlatformFile.SetReadOnly(*Directory, true);
			if (FFileHelper::SaveStringToFile(TEXT(""), *(Directory / TEXT("probe"))))
			{
				Test.AddInfo(TEXT("Skipped: this platform cannot make a directory unwritable."));
				return;
			}
		}
		NowMilliseconds = static_cast<int64>(Scenario.GetNumberField(TEXT("clock_start_unix_ms")));
		bEditor = Scenario.GetStringField(TEXT("environment")) == TEXT("editor");
		Collector->bManual = Scenario.GetStringField(TEXT("collector")) == TEXT("manual");
		for (const FValue& Step : Scenario.GetArrayField(TEXT("steps")))
		{
			if (!RunStep(*Step->AsObject()))
			{
				return;
			}
		}
		CompareRequests();
	}

private:
	FAutomationTestBase& Test;
	const FJsonObject& Scenario;
	FString Directory;
	int64 NowMilliseconds = 0;
	double Seconds = 0.0;
	bool bEditor = false;
	TOptional<FString> Handoff;
	TSharedRef<FCollector> Collector = MakeShared<FCollector>();
	TSharedPtr<TestingFloor::FClient> Client;

	FString Substituted(FString Text) const
	{
		Text.ReplaceInline(TEXT("{telemetry_directory}"), *JsonEscaped(Directory));
		if (Client.IsValid())
		{
			Text.ReplaceInline(TEXT("{play_session_id}"), *Client->GetPlaySessionID());
		}
		return Text;
	}

	void Start()
	{
		TestingFloor::FSettings Settings;
		Settings.WriteKey = TEXT("test-only-never-transmitted");
		Settings.Endpoint = TEXT("https://collector.invalid");
		// Records leave only on flush, shutdown, retries and the completion marker.
		Settings.FlushIntervalSeconds = 1.0e9;
		const FObject* Values = nullptr;
		if (Scenario.TryGetObjectField(TEXT("settings"), Values))
		{
			(*Values)->TryGetNumberField(TEXT("queue_max_events"), Settings.QueueMaxEvents);
			(*Values)->TryGetBoolField(TEXT("enable_in_editor"), Settings.bEnableInEditor);
			(*Values)->TryGetBoolField(TEXT("enable_in_editor_while_recording"), Settings.bEnableInEditorWhileRecording);
		}
		TestingFloor::FEnvironment Environment;
		Environment.bEditor = bEditor;
		// Handoffs reach the client through the same resolution it uses for the launch argument or sidecar.
		const auto Source = [this](FDateTime Now) -> TOptional<TestingFloor::FRecordingContext> {
			if (!Handoff.IsSet())
			{
				return {};
			}
			return bEditor ? TestingFloor::FRecordingContext::Resolve(FString(), TArray<FString>{ Handoff.GetValue() }, Now)
						   : TestingFloor::FRecordingContext::Resolve(TEXT("--testing-floor=") + Handoff.GetValue(), {}, Now);
		};
		Client = TestingFloor::FClient::Create(Settings, Environment, Collector, [this] { return FromUnixMilliseconds(NowMilliseconds); }, Source);
	}

	bool RunStep(const FJsonObject& Step)
	{
		const FString Action = [&Step]() -> FString {
			for (const auto& Field : Step.Values)
			{
				const FString Key(*Field.Key);
				if (Key != TEXT("accepted"))
				{
					return Key;
				}
			}
			return FString();
		}();
		const FValue Value = Step.TryGetField(Action);
		if (Action == TEXT("handoff"))
		{
			Handoff = Value->IsNull() ? TOptional<FString>() : TOptional<FString>(Substituted(PayloadText(Value)));
			return true;
		}
		if (Action == TEXT("start"))
		{
			Start();
			return true;
		}
		if (Action == TEXT("advance_ms"))
		{
			NowMilliseconds += static_cast<int64>(Value->AsNumber());
			Seconds += Value->AsNumber() / 1000.0;
			return true;
		}
		if (Action == TEXT("create_file"))
		{
			// The desktop app can always write in its own directory.
			FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*Directory, false);
			FFileHelper::SaveStringToFile(TEXT("{}"), *(Directory / Value->AsString()));
			return true;
		}
		if (!Client.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("Step %s comes before start"), *Action));
			return false;
		}
		if (Action == TEXT("track"))
		{
			const bool bAccepted = Client->Track(Value->AsString());
			bool bExpected = false;
			if (Step.TryGetBoolField(TEXT("accepted"), bExpected))
			{
				Test.TestEqual(FString::Printf(TEXT("track %s accepted"), *Value->AsString()), bAccepted, bExpected);
			}
		}
		else if (Action == TEXT("poll"))
		{
			Client->Tick(Seconds);
		}
		else if (Action == TEXT("flush"))
		{
			Client->Flush();
		}
		else if (Action == TEXT("respond"))
		{
			if (Collector->Open.IsEmpty())
			{
				Test.AddError(FString::Printf(TEXT("respond %d: no request is waiting for an answer"), static_cast<int32>(Value->AsNumber())));
				return false;
			}
			const auto Completion = Collector->Open[0];
			Collector->Open.RemoveAt(0);
			Completion(static_cast<int32>(Value->AsNumber()), true);
		}
		else if (Action == TEXT("shutdown"))
		{
			Client->Shutdown(1.0);
		}
		else if (Action == TEXT("check"))
		{
			Check(*Value->AsObject());
		}
		else
		{
			Test.AddError(FString::Printf(TEXT("Unknown step %s"), *Action));
			return false;
		}
		return true;
	}

	void Check(const FJsonObject& Expected)
	{
		bool bCollecting = false;
		if (Expected.TryGetBoolField(TEXT("collecting"), bCollecting))
		{
			Test.TestEqual(TEXT("check collecting"), Client->IsCollecting(), bCollecting);
		}
		if (const FValue Recording = Expected.TryGetField(TEXT("recording_uuid")))
		{
			const auto& Attached = Client->GetRecordingContext();
			Test.TestEqual(TEXT("check recording_uuid"), Attached.IsSet() ? Attached->RecordingUUID : FString(TEXT("null")),
				Recording->IsNull() ? FString(TEXT("null")) : Recording->AsString());
		}
		const TArray<FValue>* Names = nullptr;
		if (Expected.TryGetArrayField(TEXT("files_exist"), Names))
		{
			for (const FValue& Name : *Names)
			{
				Test.TestTrue(TEXT("check file exists: ") + Name->AsString(), IFileManager::Get().FileExists(*(Directory / Substituted(Name->AsString()))));
			}
		}
		if (Expected.TryGetArrayField(TEXT("files_absent"), Names))
		{
			for (const FValue& Name : *Names)
			{
				Test.TestFalse(TEXT("check file absent: ") + Name->AsString(), IFileManager::Get().FileExists(*(Directory / Substituted(Name->AsString()))));
			}
		}
		const FObject* Status = nullptr;
		if (Expected.TryGetObjectField(TEXT("status"), Status))
		{
			FString Text;
			FFileHelper::LoadFileToString(Text, *(Directory / Client->GetPlaySessionID() + TEXT(".status.json")));
			TArray<FString> Problems;
			Match(ParseObject(Text), **Status, TEXT("status"), Problems);
			for (const FString& Problem : Problems)
			{
				Test.AddError(TEXT("check ") + Problem);
			}
		}
	}

	/** Every expected field must match; a null expectation means the field is absent. */
	void Match(const FObject& Actual, const FJsonObject& Expected, const FString& Path, TArray<FString>& Problems) const
	{
		for (const auto& Field : Expected.Values)
		{
			const FString Key(*Field.Key);
			const FString Name = Path + TEXT(".") + Key;
			const FValue Found = Actual.IsValid() ? Actual->TryGetField(Key) : nullptr;
			const FValue& Want = Field.Value;
			if (Want->IsNull())
			{
				if (Found.IsValid() && !Found->IsNull())
				{
					Problems.Add(Name + TEXT(" should be absent"));
				}
			}
			else if (!Found.IsValid())
			{
				Problems.Add(Name + TEXT(" is missing"));
			}
			else if (Want->Type == EJson::Object)
			{
				Match(Found->Type == EJson::Object ? Found->AsObject() : nullptr, *Want->AsObject(), Name, Problems);
			}
			else if (Want->Type == EJson::String ? Found->Type != EJson::String || Found->AsString() != Substituted(Want->AsString())
					 : Want->Type == EJson::Number ? Found->Type != EJson::Number || Found->AsNumber() != Want->AsNumber()
					 : Want->Type == EJson::Boolean ? Found->Type != EJson::Boolean || Found->AsBool() != Want->AsBool()
					 : true)
			{
				Problems.Add(FString::Printf(TEXT("%s is %s, expected %s"), *Name, *ValueText(Found), *ValueText(Want)));
			}
		}
	}

	void CompareRequests()
	{
		const FJsonObject& Expect = *Scenario.GetObjectField(TEXT("expect"));
		const TArray<FValue>* ExpectedRequests = nullptr;
		const TArray<FValue>* ExpectedEvents = nullptr;
		Expect.TryGetArrayField(TEXT("requests"), ExpectedRequests);
		Expect.TryGetArrayField(TEXT("events"), ExpectedEvents);

		// Only the records a scenario names are compared.
		TSet<FString> Named;
		const auto Collect = [&Named](const TArray<FValue>& Events) {
			for (const FValue& Event : Events)
			{
				Named.Add(Event->AsObject()->GetStringField(TEXT("$name")));
			}
		};
		if (ExpectedRequests)
		{
			for (const FValue& Request : *ExpectedRequests)
			{
				Collect(Request->AsObject()->GetArrayField(TEXT("events")));
			}
		}
		if (ExpectedEvents)
		{
			Collect(*ExpectedEvents);
		}
		struct FRequest
		{
			TArray<FObject> All;
			TArray<FObject> Named;
		};
		TArray<FRequest> Requests;
		for (const FString& Body : Collector->Bodies)
		{
			FRequest Request;
			const FObject Batch = ParseObject(Body);
			for (const FValue& Event : Batch->GetArrayField(TEXT("events")))
			{
				Request.All.Add(Event->AsObject());
				if (Named.Contains(Event->AsObject()->GetStringField(TEXT("$name"))))
				{
					Request.Named.Add(Event->AsObject());
				}
			}
			if (!Request.Named.IsEmpty())
			{
				Requests.Add(MoveTemp(Request));
			}
		}

		const auto Describe = [](const TArray<FObject>& Events) {
			TArray<FString> Names;
			for (const FObject& Event : Events)
			{
				Names.Add(Event->GetStringField(TEXT("$name")));
			}
			return TEXT("[") + FString::Join(Names, TEXT(", ")) + TEXT("]");
		};
		const auto CompareEvents = [&](const TArray<FObject>& Actual, const TArray<FValue>& Expected, const FString& Where) {
			if (Actual.Num() != Expected.Num())
			{
				Test.AddError(FString::Printf(TEXT("%s: received %s, expected %d events"), *Where, *Describe(Actual), Expected.Num()));
				return;
			}
			for (int32 Index = 0; Index < Actual.Num(); ++Index)
			{
				TArray<FString> Problems;
				Match(Actual[Index], *Expected[Index]->AsObject(), Where + FString::Printf(TEXT(" event %d"), Index + 1), Problems);
				for (const FString& Problem : Problems)
				{
					Test.AddError(Problem);
				}
			}
		};

		if (ExpectedEvents)
		{
			TArray<FObject> Events;
			for (const FRequest& Request : Requests)
			{
				Events.Append(Request.Named);
			}
			CompareEvents(Events, *ExpectedEvents, TEXT("events"));
			return;
		}
		if (!ExpectedRequests || !Test.TestEqual(TEXT("request count"), Requests.Num(), ExpectedRequests->Num()))
		{
			for (int32 Index = 0; Index < Requests.Num(); ++Index)
			{
				Test.AddInfo(FString::Printf(TEXT("request %d: %s"), Index + 1, *Describe(Requests[Index].All)));
			}
			return;
		}
		for (int32 Index = 0; Index < Requests.Num(); ++Index)
		{
			const FJsonObject& Expected = *(*ExpectedRequests)[Index]->AsObject();
			const FString Where = FString::Printf(TEXT("request %d"), Index + 1);
			const TArray<FValue>& Events = Expected.GetArrayField(TEXT("events"));
			CompareEvents(Requests[Index].Named, Events, Where);
			bool bExclusive = false;
			if (Expected.TryGetBoolField(TEXT("exclusive"), bExclusive) && bExclusive && Requests[Index].All.Num() != Events.Num())
			{
				Test.AddError(FString::Printf(TEXT("%s must hold only %d events, but held %s"), *Where, Events.Num(), *Describe(Requests[Index].All)));
			}
			int32 Repeats = 0;
			if (Expected.TryGetNumberField(TEXT("repeats"), Repeats) && Repeats >= 1 && Repeats <= Index)
			{
				const TArray<FObject>& Original = Requests[Repeats - 1].Named;
				bool bSame = Original.Num() == Requests[Index].Named.Num();
				for (int32 Event = 0; bSame && Event < Original.Num(); ++Event)
				{
					bSame = Original[Event]->GetStringField(TEXT("$id")) == Requests[Index].Named[Event]->GetStringField(TEXT("$id"))
						&& Original[Event]->GetStringField(TEXT("$time")) == Requests[Index].Named[Event]->GetStringField(TEXT("$time"));
				}
				Test.TestTrue(FString::Printf(TEXT("%s repeats request %d with the same $id and $time"), *Where, Repeats), bSame);
			}
		}
	}
};
} // namespace TestingFloorScenarioTests

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FTestingFloorScenarioTest, "TestingFloor.Sdk.Scenarios", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)

void FTestingFloorScenarioTest::GetTests(TArray<FString>& OutBeautifiedNames, TArray<FString>& OutTestCommands) const
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(TestingFloorScenarioTests::ScenarioDirectory() / TEXT("*.json")), true, false);
	Files.Sort();
	for (const FString& File : Files)
	{
		OutBeautifiedNames.Add(FPaths::GetBaseFilename(File));
		OutTestCommands.Add(TestingFloorScenarioTests::ScenarioDirectory() / File);
	}
}

bool FTestingFloorScenarioTest::RunTest(const FString& Parameters)
{
	FString Text;
	const TestingFloorScenarioTests::FObject Scenario = FFileHelper::LoadFileToString(Text, *Parameters) ? TestingFloorScenarioTests::ParseObject(Text) : nullptr;
	if (!Scenario.IsValid())
	{
		AddError(FString::Printf(TEXT("Cannot read the scenario at %s"), *Parameters));
		return false;
	}
	if (Scenario->GetNumberField(TEXT("format")) != 1)
	{
		AddError(TEXT("This runner reads scenario format 1 only"));
		return false;
	}
	const FString Kind = Scenario->GetStringField(TEXT("kind"));
	if (Kind == TEXT("handoff"))
	{
		TestingFloorScenarioTests::RunHandoff(*this, *Scenario);
	}
	else if (Kind == TEXT("lifecycle"))
	{
		TestingFloorScenarioTests::FLifecycle(*this, *Scenario).Run();
	}
	else
	{
		AddError(TEXT("Unknown scenario kind ") + Kind);
	}
	return true;
}

#endif
