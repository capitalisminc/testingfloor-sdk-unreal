#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "TestingFloor/TestingFloorTrackedObjectComponent.h"
#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace TestingFloorObjectLifetimeTests
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

/** A game world that has begun play, so actors spawned and components registered in it begin play too. */
class FPlayWorld
{
public:
	FPlayWorld()
	{
		World = UWorld::CreateWorld(EWorldType::Game, false);
		GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
		World->InitializeActorsForPlay(FURL());
		World->SetBegunPlay(true);
	}
	~FPlayWorld()
	{
		World->EndPlay(EEndPlayReason::Quit);
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	}
	AActor* Spawn(const FVector& Location) const
	{
		AActor* Actor = World->SpawnActor<AActor>();
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		Actor->SetActorLocation(Location);
		return Actor;
	}
	UWorld* World = nullptr;
};

/** Every queued `$tf_objects` record, serialized and parsed the way the collector receives it. */
TArray<TSharedPtr<FJsonObject>> ObjectRecords(const TestingFloor::FClient& Client)
{
	TArray<TSharedPtr<FJsonObject>> Result;
	for (const TestingFloor::FEvent& Event : Client.GetQueuedEvents())
	{
		if (Event.Name == TEXT("$tf_objects"))
		{
			TSharedPtr<FJsonObject> Object;
			FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TestingFloor::SerializeEvent(Event, Settings())), Object);
			Result.Add(Object);
		}
	}
	return Result;
}

/** A record array joined as text: strings as written, numbers in shortest form. */
FString Array(const TSharedPtr<FJsonObject>& Record, const TCHAR* Key)
{
	TArray<FString> Result;
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (Record.IsValid() && Record->TryGetArrayField(Key, Values))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			Result.Add(Value->Type == EJson::Number ? FString::Printf(TEXT("%g"), Value->AsNumber()) : Value->AsString());
		}
	}
	return FString::Join(Result, TEXT(","));
}
} // namespace TestingFloorObjectLifetimeTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorWeakObjectTest, "TestingFloor.Sdk.Objects.WeakActorsAndComponents", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorWeakObjectTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorObjectLifetimeTests;
	const FPlayWorld Play;
	AActor* Crate = Play.Spawn(FVector(100.0, 200.0, 300.0));
	AActor* Holder = Play.Spawn(FVector::ZeroVector);
	USceneComponent* Marker = NewObject<USceneComponent>(Holder, TEXT("Marker"));
	Marker->SetupAttachment(Holder->GetRootComponent());
	Marker->RegisterComponent();
	Marker->SetRelativeLocation(FVector(0.0, 0.0, 50.0));
	const auto Client = TestingFloor::FClient::Create(Settings(), {}, MakeShared<FTransport>());
	TestEqual(TEXT("A null actor is not tracked"), Client->TrackObject(TEXT("crate"), static_cast<AActor*>(nullptr)), 0ULL);
	TF_REQUIRE_TRUE(Client->TrackObject(TEXT("crate"), Crate, TEXT("crate")) != 0, "Actor tracked");
	TF_REQUIRE_TRUE(Client->TrackObject(TEXT("marker"), Marker, TEXT("marker")) != 0, "Scene component tracked");
	Client->Tick(0.0);
	auto Records = ObjectRecords(*Client);
	TF_REQUIRE_TRUE(Records.Num() == 1, "Registration samples");
	TestEqual(TEXT("Actor and component world locations"), Array(Records[0], TEXT("sample.z")), FString(TEXT("300,50")));

	Crate->SetActorLocation(FVector(400.0, 200.0, 300.0));
	Crate->Destroy();
	Marker->DestroyComponent();
	Client->Tick(0.25);
	Records = ObjectRecords(*Client);
	TF_REQUIRE_TRUE(Records.Num() == 2, "Removal record");
	TestEqual(TEXT("Destroyed objects are removed"), Array(Records[1], TEXT("removed.id")), FString(TEXT("crate,marker")));
	TestEqual(TEXT("They are removed as destroyed"), Array(Records[1], TEXT("removed.reason")), FString(TEXT("destroyed,destroyed")));
	TestEqual(TEXT("A destroyed object ends at its last checked position"), Array(Records[1], TEXT("sample.x")), FString(TEXT("100,0")));
	TestEqual(TEXT("Nothing is live"), Client->GetTrackedObjectCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorTrackedComponentTest, "TestingFloor.Sdk.Objects.TrackedObjectComponent", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorTrackedComponentTest::RunTest(const FString& Parameters)
{
	using namespace TestingFloorObjectLifetimeTests;
	const FPlayWorld Play;
	const auto Client = TestingFloor::FClient::Create(Settings(), {}, MakeShared<FTransport>());
	TestingFloor::FClient::SetDefault(nullptr);
	AActor* Guard = Play.Spawn(FVector(100.0, 0.0, 0.0));
	UTestingFloorTrackedObjectComponent* Tracked = NewObject<UTestingFloorTrackedObjectComponent>(Guard);
	Tracked->Kind = TEXT("guard");
	Tracked->RegisterComponent();
	TestTrue(TEXT("The component began play with its actor"), Tracked->HasBegunPlay());
	TestEqual(TEXT("Without a default client the component waits"), Tracked->GetHandle(), 0ULL);
	TestingFloor::FClient::SetDefault(Client);
	TestTrue(TEXT("A new default client registers waiting components"), Tracked->GetHandle() != 0);
	Client->Tick(0.0);
	auto Records = ObjectRecords(*Client);
	TF_REQUIRE_TRUE(Records.Num() == 1, "Registration sample");
	TestEqual(TEXT("Generated ID"), Array(Records[0], TEXT("sample.id")), FString(TEXT("guard#1")));
	TestEqual(TEXT("Owner location"), Array(Records[0], TEXT("sample.x")), FString(TEXT("100")));

	Tracked->Deactivate();
	TestEqual(TEXT("Deactivation stops tracking"), Tracked->GetHandle(), 0ULL);
	Client->Tick(0.25);
	Records = ObjectRecords(*Client);
	TF_REQUIRE_TRUE(Records.Num() == 2, "Removal record");
	TestEqual(TEXT("Deactivation removes the object as disabled"), Array(Records[1], TEXT("removed.reason")), FString(TEXT("disabled")));

	Tracked->Activate();
	TestTrue(TEXT("Activation tracks again"), Tracked->GetHandle() != 0);
	Client->Tick(0.5);
	Records = ObjectRecords(*Client);
	TF_REQUIRE_TRUE(Records.Num() == 3, "New path");
	TestEqual(TEXT("A new lifetime gets a new generated ID"), Array(Records[2], TEXT("sample.id")), FString(TEXT("guard#2")));

	Guard->Destroy();
	TestEqual(TEXT("Ending play stops tracking"), Tracked->GetHandle(), 0ULL);
	Client->Tick(0.75);
	Records = ObjectRecords(*Client);
	TF_REQUIRE_TRUE(Records.Num() == 4, "Removal record");
	TestEqual(TEXT("Removed lifetime"), Array(Records[3], TEXT("removed.id")), FString(TEXT("guard#2")));
	TestEqual(TEXT("Destroying the actor removes the object as destroyed"), Array(Records[3], TEXT("removed.reason")), FString(TEXT("destroyed")));
	TestingFloor::FClient::SetDefault(nullptr);
	return true;
}

#endif
