#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "TestingFloorTrackedObjectComponent.generated.h"

namespace TestingFloor
{
class FClient;
}

/**
 * Tracks its owner's location in `$tf_objects` records while active, with no code. It registers with
 * TestingFloor::FClient::GetDefault() when play begins, or once a default client exists. Deactivating or
 * unregistering the component removes the object with reason `disabled`; ending play or destroying the
 * actor removes it with reason `destroyed`.
 */
UCLASS(ClassGroup = (TestingFloor), meta = (BlueprintSpawnableComponent, DisplayName = "Testing Floor Tracked Object"))
class TESTINGFLOORSDK_API UTestingFloorTrackedObjectComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UTestingFloorTrackedObjectComponent();

	/** What this object is, such as guard, crate or rocket. Objects are grouped and colored by kind. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Testing Floor")
	FString Kind = TEXT("object");

	/** Optional stable ID: use the one your events send for this object. Empty gives kind#n. Applies the next time tracking starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Testing Floor")
	FString ID;

	/** The handle from FClient::TrackObject, or 0 while the object is not tracked. */
	uint64 GetHandle() const;

	virtual void Activate(bool bReset = false) override;
	virtual void Deactivate() override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void OnRegister() override;
	virtual void OnUnregister() override;

private:
	friend class TestingFloor::FClient;
	/** Track the components whose play began before a default client existed. FClient::SetDefault calls it. */
	static void RegisterWaiting();
	void StartTracking();
	void StopTracking(const TCHAR* Reason);

	TWeakPtr<TestingFloor::FClient> Client;
	uint64 Handle = 0;
};
