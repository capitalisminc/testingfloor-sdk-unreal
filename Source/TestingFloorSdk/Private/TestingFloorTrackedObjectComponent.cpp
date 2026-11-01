#include "TestingFloor/TestingFloorTrackedObjectComponent.h"

#include "GameFramework/Actor.h"
#include "TestingFloor/TestingFloorClient.h"

namespace TestingFloor::Private
{
/** Components whose play began before a default client existed. Game thread only. */
TSet<TWeakObjectPtr<UTestingFloorTrackedObjectComponent>>& WaitingComponents()
{
	static TSet<TWeakObjectPtr<UTestingFloorTrackedObjectComponent>> Components;
	return Components;
}
} // namespace TestingFloor::Private

UTestingFloorTrackedObjectComponent::UTestingFloorTrackedObjectComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	bAutoActivate = true;
}

uint64 UTestingFloorTrackedObjectComponent::GetHandle() const
{
	return Handle;
}

void UTestingFloorTrackedObjectComponent::Activate(bool bReset)
{
	Super::Activate(bReset);
	StartTracking();
}

void UTestingFloorTrackedObjectComponent::Deactivate()
{
	Super::Deactivate();
	StopTracking(TEXT("disabled"));
}

void UTestingFloorTrackedObjectComponent::BeginPlay()
{
	Super::BeginPlay();
	StartTracking();
}

void UTestingFloorTrackedObjectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopTracking(TEXT("destroyed"));
	Super::EndPlay(EndPlayReason);
}

void UTestingFloorTrackedObjectComponent::OnRegister()
{
	Super::OnRegister();
	StartTracking();
}

void UTestingFloorTrackedObjectComponent::OnUnregister()
{
	StopTracking(TEXT("disabled"));
	Super::OnUnregister();
}

void UTestingFloorTrackedObjectComponent::RegisterWaiting()
{
	const TArray<TWeakObjectPtr<UTestingFloorTrackedObjectComponent>> Components = TestingFloor::Private::WaitingComponents().Array();
	TestingFloor::Private::WaitingComponents().Reset();
	for (const TWeakObjectPtr<UTestingFloorTrackedObjectComponent>& Component : Components)
	{
		if (Component.IsValid())
		{
			Component->StartTracking();
		}
	}
}

void UTestingFloorTrackedObjectComponent::StartTracking()
{
	// Registration and activation also happen in editor worlds and before play; only play is tracked.
	if (Handle != 0 || !HasBegunPlay() || !IsActive() || !IsRegistered() || !IsValid(GetOwner()))
	{
		return;
	}
	const TSharedPtr<TestingFloor::FClient> Default = TestingFloor::FClient::GetDefault();
	if (!Default.IsValid())
	{
		TestingFloor::Private::WaitingComponents().Add(this);
		return;
	}
	TestingFloor::Private::WaitingComponents().Remove(this);
	Handle = Default->TrackObject(Kind, GetOwner(), ID);
	if (Handle != 0)
	{
		Client = Default;
	}
}

void UTestingFloorTrackedObjectComponent::StopTracking(const TCHAR* Reason)
{
	TestingFloor::Private::WaitingComponents().Remove(this);
	if (Handle == 0)
	{
		return;
	}
	if (const TSharedPtr<TestingFloor::FClient> Owner = Client.Pin())
	{
		Owner->RemoveObject(Handle, Reason);
	}
	Handle = 0;
	Client.Reset();
}
