#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "Dom/JsonObject.h"
#include "GeneralProjectSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PlatformFeatures.h"
#include "SaveGameSystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#if PLATFORM_MAC || PLATFORM_LINUX
#include <sys/stat.h>
#endif

namespace TestingFloorEngineContextTests
{
class FTransport final : public TestingFloor::ITransport
{
public:
	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> Completion) override
	{
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

/** The first queued record with this name, serialized and parsed as the collector receives it. */
TSharedPtr<FJsonObject> Serialized(const TestingFloor::FClient& Client, const TestingFloor::FSettings& ClientSettings, const FString& Name)
{
	for (const TestingFloor::FEvent& Event : Client.GetQueuedEvents())
	{
		if (Event.Name == Name)
		{
			TSharedPtr<FJsonObject> Object;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TestingFloor::SerializeEvent(Event, ClientSettings)), Object);
			return Object;
		}
	}
	return nullptr;
}

/** A record's sorted `$tf` keys, joined for readable failures. */
FString ContextKeys(const TSharedPtr<FJsonObject>& Record)
{
	TArray<FString> Keys;
	const TSharedPtr<FJsonObject>* Context = nullptr;
	if (Record.IsValid() && Record->TryGetObjectField(TEXT("$tf"), Context))
	{
		for (const auto& Pair : (*Context)->Values)
		{
			Keys.Add(FString(Pair.Key.ToView()));
		}
	}
	Keys.Sort();
	return FString::Join(Keys, TEXT(","));
}
} // namespace TestingFloorEngineContextTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorOperatingSystemTest, "TestingFloor.Sdk.EngineContext.OperatingSystemFamily", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorOperatingSystemTest::RunTest(const FString& Parameters)
{
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{ TEXT("Windows"), TEXT("Windows") }, { TEXT("WinGDK"), TEXT("Windows") }, { TEXT("Mac"), TEXT("macOS") },
		{ TEXT("MacEditor"), TEXT("macOS") }, { TEXT("Linux"), TEXT("Linux") }, { TEXT("LinuxArm64"), TEXT("Linux") },
		{ TEXT("LinuxServer"), TEXT("Linux") }, { TEXT("IOS"), TEXT("iOS") }, { TEXT("TVOS"), TEXT("tvOS") },
		{ TEXT("VisionOS"), TEXT("visionOS") }, { TEXT("Android"), TEXT("Android") }, { TEXT("PS5"), TEXT("PlayStation") },
		{ TEXT("XSX"), TEXT("Xbox") }, { TEXT("XboxOneGDK"), TEXT("Xbox") }, { TEXT("Switch"), TEXT("Switch") },
		{ TEXT("ExamplePlatform"), TEXT("ExamplePlatform") },
	};
	for (const auto& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("%s family"), Case.Key), TestingFloor::OperatingSystemFamily(Case.Key), FString(Case.Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorLanguageTagTest, "TestingFloor.Sdk.EngineContext.LanguageTag", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorLanguageTagTest::RunTest(const FString& Parameters)
{
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{ TEXT("en-US"), TEXT("en-US") }, { TEXT("en_us"), TEXT("en-US") }, { TEXT("zh-hans-cn"), TEXT("zh-Hans-CN") },
		{ TEXT("es-419"), TEXT("es-419") }, { TEXT("de"), TEXT("de") }, { TEXT("en-United States"), TEXT("en") },
		{ TEXT("English"), TEXT("") }, { TEXT(""), TEXT("") }, { TEXT("  fr-FR  "), TEXT("fr-FR") },
	};
	for (const auto& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("Tag for \"%s\""), Case.Key), TestingFloor::LanguageTag(Case.Key), FString(Case.Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorDeviceModelTest, "TestingFloor.Sdk.EngineContext.DeviceModel", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorDeviceModelTest::RunTest(const FString& Parameters)
{
	const TPair<const TCHAR*, const TCHAR*> Cases[] = {
		{ TEXT("Apple|Apple M2 Max"), TEXT("Apple M2 Max") }, { TEXT("Apple|iPhone14,2"), TEXT("Apple iPhone14,2") },
		{ TEXT("samsung|SM-G991B"), TEXT("samsung SM-G991B") }, { TEXT("ExampleVendor|Example CPU|Unknown"), TEXT("ExampleVendor Example CPU") },
		{ TEXT("|"), TEXT("") }, { TEXT("Unknown|Unknown"), TEXT("") },
	};
	for (const auto& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("Model for \"%s\""), Case.Key), TestingFloor::DeviceModelName(Case.Key), FString(Case.Value));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorEngineDefaultsTest, "TestingFloor.Sdk.EngineContext.EngineDefaults", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorEngineDefaultsTest::RunTest(const FString& Parameters)
{
	TestingFloor::FSettings Defaults;
	TestingFloor::ApplyEngineDefaults(Defaults);
	TestFalse(TEXT("Engine version read"), Defaults.EngineVersion.IsEmpty());
#if PLATFORM_MAC
	TestEqual(TEXT("A Mac reports the macOS family"), Defaults.OperatingSystem, FString(TEXT("macOS")));
#endif
	TestTrue(TEXT("Locale is a language tag"), !Defaults.Locale.IsEmpty() && TestingFloor::LanguageTag(Defaults.Locale) == Defaults.Locale);
	TestFalse(TEXT("Device model read"), Defaults.DeviceModel.IsEmpty());
	TestEqual(TEXT("App version is the project version"), Defaults.AppVersion, GetDefault<UGeneralProjectSettings>()->ProjectVersion.TrimStartAndEnd());
	TestTrue(TEXT("Automation creates no identity"), Defaults.UserID.IsEmpty() && Defaults.DeviceID.IsEmpty());

	TestingFloor::FSettings Game;
	Game.OperatingSystem = TEXT("ExampleOS");
	Game.Locale = TEXT("fr-FR");
	Game.DeviceModel = TEXT("example-device");
	Game.AppVersion = TEXT("1.2.3");
	Game.UserID = TEXT("example-player");
	TestingFloor::ApplyEngineDefaults(Game);
	TestEqual(TEXT("Game OS kept"), Game.OperatingSystem, FString(TEXT("ExampleOS")));
	TestEqual(TEXT("Game locale kept"), Game.Locale, FString(TEXT("fr-FR")));
	TestEqual(TEXT("Game device model kept"), Game.DeviceModel, FString(TEXT("example-device")));
	TestEqual(TEXT("Game app version kept"), Game.AppVersion, FString(TEXT("1.2.3")));
	TestEqual(TEXT("Game player ID kept"), Game.UserID, FString(TEXT("example-player")));

	const auto Automatic = TestingFloor::FClient::Create(TestingFloorEngineContextTests::Settings());
	TestTrue(TEXT("The default factory makes the default client"), TestingFloor::FClient::GetDefault().Get() == &Automatic.Get());
	Automatic->Shutdown(0.0);
	TestFalse(TEXT("Shutdown releases the default client"), TestingFloor::FClient::GetDefault().IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorIdentityTest, "TestingFloor.Sdk.EngineContext.PersistedIdentity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorIdentityTest::RunTest(const FString& Parameters)
{
	const FString Directory = FPaths::ProjectSavedDir() / TEXT("TestingFloorTests") / FGuid::NewGuid().ToString();
	const FString Path = Directory / TEXT("installation-id");
	const FString Installation = TestingFloor::LoadOrCreateInstallationID(Path);
	TestEqual(TEXT("A new installation ID is 32 hex digits"), Installation.Len(), 32);
	TestTrue(TEXT("The installation ID is saved on this machine"), IFileManager::Get().FileExists(*Path));
	TestEqual(TEXT("The installation ID persists"), TestingFloor::LoadOrCreateInstallationID(Path), Installation);
	FFileHelper::SaveStringToFile(TEXT("  \n"), *Path);
	const FString Replaced = TestingFloor::LoadOrCreateInstallationID(Path);
	TestTrue(TEXT("An unusable stored ID is replaced"), Replaced.Len() == 32 && Replaced != Installation);

	ISaveGameSystem* SaveGames = IPlatformFeaturesModule::Get().GetSaveGameSystem();
	TF_REQUIRE(SaveGames, "Platform save system");
	const FString Slot = TEXT("TestingFloorTest") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Profile = TestingFloor::LoadOrCreateProfileID(Slot);
	TestEqual(TEXT("A new profile ID is 32 hex digits"), Profile.Len(), 32);
	TestTrue(TEXT("The profile ID is in the game's save data"), SaveGames->DoesSaveGameExist(*Slot, 0));
	TestEqual(TEXT("The profile ID persists"), TestingFloor::LoadOrCreateProfileID(Slot), Profile);
	TestNotEqual(TEXT("Profile and installation IDs differ"), Profile, Installation);
#if PLATFORM_MAC || PLATFORM_LINUX
	// A stored ID that cannot be read is left in place: that run sends none rather than a new one.
	const auto Unreadable = [](const FString& File, bool bReadable) {
		return chmod(TCHAR_TO_UTF8(*FPaths::ConvertRelativePathToFull(File)), bReadable ? 0644 : 0) == 0;
	};
	const auto Stored = [](const FString& File) {
		FString Value;
		FFileHelper::LoadFileToString(Value, *File);
		return Value.TrimStartAndEnd();
	};
	TF_REQUIRE_TRUE(Unreadable(Path, false), "Make the installation ID unreadable");
	AddExpectedMessage(TEXT("could not read its installation ID"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestEqual(TEXT("An unreadable installation ID sends none"), TestingFloor::LoadOrCreateInstallationID(Path), FString());
	Unreadable(Path, true);
	TestEqual(TEXT("The unreadable installation ID is kept"), Stored(Path), Replaced);
	const FString SlotFile = FPaths::ProjectSavedDir() / TEXT("SaveGames") / Slot + TEXT(".sav");
	if (IFileManager::Get().FileExists(*SlotFile))
	{
		TF_REQUIRE_TRUE(Unreadable(SlotFile, false), "Make the profile slot unreadable");
		AddExpectedMessage(TEXT("could not read its profile ID"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		// The platform save system reports the failed read itself.
		AddExpectedMessage(TEXT("Failed to read file"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
		TestEqual(TEXT("An unreadable profile slot sends none"), TestingFloor::LoadOrCreateProfileID(Slot), FString());
		Unreadable(SlotFile, true);
		TestEqual(TEXT("The unreadable profile ID is kept"), Stored(SlotFile), Profile);
	}
#endif
	SaveGames->DeleteGame(false, *Slot, 0);
	IFileManager::Get().DeleteDirectory(*Directory, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorContextOmissionTest, "TestingFloor.Sdk.EngineContext.UnknownContextAndEditorFlag", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorContextOmissionTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorEngineContextTests;
	const auto Empty = Settings();
	const auto Bare = TestingFloor::FClient::Create(Empty, {}, MakeShared<FTransport>());
	TF_REQUIRE_TRUE(Bare->Track(TEXT("example")), "Event queued");
	const auto Event = Serialized(*Bare, Empty, TEXT("example"));
	TF_REQUIRE(Event.Get(), "Event serialized");
	TestEqual(TEXT("The advanced overload leaves unknown context out"), ContextKeys(Event), FString(TEXT("editor,sdk_version,seq")));
	TestFalse(TEXT("No empty player IDs"), Event->HasField(TEXT("$user_id")) || Event->HasField(TEXT("$device_id")));
	TestFalse(TEXT("A built game is not an editor run"), Event->GetObjectField(TEXT("$tf"))->GetBoolField(TEXT("editor")));

	auto Blank = Settings();
	Blank.EngineVersion = TEXT("  ");
	Blank.AppVersion = TEXT(" ");
	Blank.Locale = TEXT("\t");
	Blank.DeviceModel = TEXT(" ");
	Blank.UserID = TEXT(" ");
	Blank.DeviceID = TEXT(" ");
	const auto Spaces = TestingFloor::FClient::Create(Blank, {}, MakeShared<FTransport>());
	Spaces->Track(TEXT("example"));
	const auto Spaced = Serialized(*Spaces, Blank, TEXT("example"));
	TF_REQUIRE(Spaced.Get(), "Event serialized");
	TestEqual(TEXT("Whitespace values are unknown too"), ContextKeys(Spaced), FString(TEXT("editor,sdk_version,seq")));
	TestFalse(TEXT("Whitespace player IDs are unknown"), Spaced->HasField(TEXT("$user_id")) || Spaced->HasField(TEXT("$device_id")));

	auto Full = Settings();
	Full.EngineVersion = TEXT("5.0.0");
	Full.AppVersion = TEXT("1.2.3");
	Full.OperatingSystem = TEXT("macOS");
	Full.Locale = TEXT("en-US");
	Full.DeviceModel = TEXT("example-device");
	Full.bEnableInEditor = true;
	const auto Editor = TestingFloor::FClient::Create(Full, { true, false }, MakeShared<FTransport>());
	Editor->SetState(TEXT("difficulty"), TEXT("hard"));
	Editor->Track(TEXT("example"));
	const auto EditorEvent = Serialized(*Editor, Full, TEXT("example"));
	const auto EditorState = Serialized(*Editor, Full, TEXT("$tf_state"));
	TF_REQUIRE_TRUE(EditorEvent.IsValid() && EditorState.IsValid(), "Event and state serialized");
	TestEqual(TEXT("Known context is sent"), ContextKeys(EditorEvent), FString(TEXT("app_version,device_model,editor,engine,locale,os,sdk_version,seq")));
	TestEqual(TEXT("Engine name and version"), EditorEvent->GetObjectField(TEXT("$tf"))->GetStringField(TEXT("engine")), FString(TEXT("Unreal 5.0.0")));
	TestTrue(TEXT("Editor Play marks events"), EditorEvent->GetObjectField(TEXT("$tf"))->GetBoolField(TEXT("editor")));
	TestTrue(TEXT("Editor Play marks session state"), EditorState->GetObjectField(TEXT("$tf"))->GetBoolField(TEXT("editor")));
	return true;
}

#endif
