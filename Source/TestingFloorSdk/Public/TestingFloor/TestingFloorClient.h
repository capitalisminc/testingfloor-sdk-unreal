#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"

namespace TestingFloor
{
class FClient;

/** Typed, flattened properties. Call-site values override context with the same key. */
class TESTINGFLOORSDK_API FProperties
{
public:
	FProperties& Set(const FString& Key, const FString& Value);
	FProperties& Set(const FString& Key, const TCHAR* Value);
	FProperties& Set(const FString& Key, int32 Value);
	FProperties& Set(const FString& Key, int64 Value);
	FProperties& Set(const FString& Key, float Value);
	FProperties& Set(const FString& Key, double Value);
	FProperties& Set(const FString& Key, bool bValue);
	FProperties& Set(const FString& Key, const FGuid& Value);
	FProperties& Set(const FString& Key, const TArray<FString>& Values);
	FProperties& Set(const FString& Key, const TArray<int32>& Values);
	/** Unsupported custom values must be converted explicitly at the call site. */
	template <typename T> FProperties& Set(const FString& Key, T Value) = delete;
	FProperties& SetIfPresent(const FString& Key, const FString& Value);
	FProperties& SetVector(const FString& Prefix, const FVector& Position);
	void Merge(const FProperties& Overrides);
	void Remove(const FString& Key);
	const TMap<FString, TSharedPtr<FJsonValue>>& GetValues() const;

private:
	/** The client also writes the null values and number arrays that schema 2 records need. */
	friend class FClient;
	void Put(const FString& Key, TSharedPtr<FJsonValue> Value);
	TMap<FString, TSharedPtr<FJsonValue>> Properties;
};

struct TESTINGFLOORSDK_API FRecordingContext
{
	FString RecordingUUID;
	FString TelemetryDirectory;
	TOptional<int64> PlaytestID;
	bool bSuppressQR = true;
	/** Read the current launch argument or Saved/TestingFloor/recording-payload.json. */
	static TOptional<FRecordingContext> Discover();
	static TOptional<FRecordingContext> Parse(const FString& Json, FDateTime Now);
	/** Handles raw argv flattened by Unreal, shell quoting and Windows escaped JSON. */
	static TOptional<FRecordingContext> FromCommandLine(const FString& CommandLine, FDateTime Now);
	/** Host supplies sidecar contents in priority order and retains ownership of their lifetime. */
	static TOptional<FRecordingContext> Resolve(const FString& CommandLine, TConstArrayView<FString> SidecarPayloads, FDateTime Now);
};

struct FSettings
{
	bool bEnabled = true;
	bool bEnableInEditor = false;
	FString WriteKey;
	FString Endpoint = TEXT("https://dataentry.testingfloor.com");
	FString DeviceID;
	FString UserID;
	FString AppVersion;
	FString EngineVersion;
	FString OperatingSystem;
	FString Locale;
	FString DeviceModel;
	FIntPoint ScreenSize = FIntPoint::ZeroValue;
	TOptional<FRecordingContext> Recording;
	int32 BatchMaxEvents = 50;
	double FlushIntervalSeconds = 0.25;
	int32 QueueMaxEvents = 4096;
	bool bMovementTracking = false;
	/** Tracked-object checks per second of unscaled time, clamped to 1-10. */
	double ObjectChecksPerSecond = 4.0;
	/** Live tracked objects. Further registrations are ignored, with one warning. */
	int32 MaxTrackedObjects = 256;
};

struct FEnvironment
{
	bool bEditor = false;
	bool bAutomation = false;
};

enum class EState : uint8
{
	Disabled,
	NotConfigured,
	Ok,
	NetworkDown,
	Rejected
};

struct FEvent
{
	FGuid ID;
	FString Name;
	FDateTime Timestamp;
	FString SessionID;
	FProperties Properties;
	FIntPoint ScreenSize = FIntPoint::ZeroValue;
	TOptional<FRecordingContext> Recording;
	/** Play-session record counter (`$tf.seq`), assigned when the record is queued. Zero omits it. */
	int64 Sequence = 0;
	bool bKeyframe = false;
};

/** Sources supply native Unreal centimeters and degrees. Movement metrics/thresholds remain meters. */
struct TESTINGFLOORSDK_API FActivitySample
{
	TOptional<FVector> PlayerPosition;
	TOptional<FVector> CameraPosition;
	TOptional<FQuat> CameraRotation;
	/** Optional host coordinate-system override for the serialized camera.euler fields. */
	TOptional<FVector> CameraEulerDegrees;
	TOptional<double> CameraFOV;
	FIntPoint ViewportSize = FIntPoint::ZeroValue;
	bool IsEmpty() const;
};

struct FMovementSettings
{
	double MinPingDistance = 0.5;
	double MinPingIntervalSeconds = 0.5;
	double StartGraceSeconds = 0.05;
	double StopGraceSeconds = 0.25;
	double MinStep = 0.005;
	double MinPingRotationDegrees = 5.0;
	double MinStepRotationDegrees = 0.1;
	double MinPingFOVDegrees = 2.0;
	double MinStepFOVDegrees = 0.05;
	double MinPingCameraDistance = 0.5;
	double MinStepCameraDistance = 0.005;
};

/** HTTP seam is injectable: automation can exercise the complete queue without sockets. */
class TESTINGFLOORSDK_API ITransport
{
public:
	virtual ~ITransport() = default;
	virtual void Send(const FString& Endpoint, const FString& Body, TFunction<void(int32, bool)> Completion) = 0;
	virtual void Pump(float DeltaSeconds)
	{
	}
};

TESTINGFLOORSDK_API TSharedRef<ITransport> MakeHttpTransport();
TESTINGFLOORSDK_API bool ShouldCollect(const FSettings& Settings, const FEnvironment& Environment);
TESTINGFLOORSDK_API FString SerializeEvent(const FEvent& Event, const FSettings& Settings);
TESTINGFLOORSDK_API FString BuildBatch(TConstArrayView<FEvent> Events, const FSettings& Settings, FDateTime SentAt, int32& OutConsumed, int32* OutWritten = nullptr, int32* OutRecordingDropped = nullptr);

/** Game-thread native client. No game module, UObjects, assets, or global singleton. */
class TESTINGFLOORSDK_API FClient final : public TSharedFromThis<FClient>
{
public:
	using FContextProvider = TFunction<void(FProperties&)>;
	using FActivitySource = TFunction<FActivitySample()>;
	using FClock = TFunction<FDateTime()>;
	/** Returns native centimeters, or unset once the object is gone. */
	using FPositionSource = TFunction<TOptional<FVector>()>;

	/** Create an automatically ticked client using the engine environment and HTTP transport. */
	static TSharedRef<FClient> Create(FSettings Settings);
	~FClient();
	FClient(const FClient&) = delete;
	FClient& operator=(const FClient&) = delete;
	/** Advanced creation seam for custom transports and deterministic clocks. */
	static TSharedRef<FClient> Create(FSettings Settings, FEnvironment Environment, TSharedRef<ITransport> Transport, FClock Clock = [] { return FDateTime::UtcNow(); });
	/** Small values that describe each moment, such as level, health and game clock. Added to every event. */
	void SetContextProvider(FContextProvider Provider);
	/** Values that stay true until they change. Sent as `$tf_state` changes and keyframes, not on every event. */
	void SetStateProvider(FContextProvider Provider);
	/** Set one state value directly. Accepts the types FProperties::Set accepts and overrides the state provider. */
	template <typename T> void SetState(const FString& Key, T&& Value)
	{
		DirectState.Set(Key, Forward<T>(Value));
	}
	/** Remove a value set with SetState. A key that no source supplies any more is sent as null. */
	void ClearState(const FString& Key);
	void SetActivitySource(FActivitySource Source);
	/** Finalize captured event fields after context/activity/call-site merging, before queueing. */
	void SetEventProcessor(FContextProvider Processor);
	void SetMovementSettings(const FMovementSettings& Settings);
	void SetMovementTrackingEnabled(bool bEnabled);
	bool Track(const FString& Name, const FProperties& Properties = {});
	/** Tick with monotonic real time, including during gameplay pause. */
	void Tick();
	void Tick(double NowSeconds);
	void Flush();
	/** Bounded shutdown drain. A deadline never stalls game exit indefinitely. */
	void Shutdown(double TimeoutSeconds);
	uint64 BeginTeleport(const FString& Reason);
	void EndTeleport(uint64 Token);
	void CancelTeleport(uint64 Token);
	void RecordTeleport(const FVector& StartCentimeters, const FVector& EndCentimeters, const FString& Reason, double DurationSeconds = 0.0);
	void ResetMovement();
	/**
	 * Track a world object's path in `$tf_objects` records. Without an ID the SDK assigns `kind#n`.
	 * Returns 0, a handle that does nothing, when collection is off or the live-object limit is reached.
	 * A position source that returns unset removes the object with reason `gone`.
	 */
	uint64 TrackObject(const FString& Kind, FPositionSource PositionCentimeters, const FString& ID = {});
	void SetObjectState(uint64 Token, const FString& Key, const FString& Value);
	void SetObjectState(uint64 Token, const FString& Key, const TCHAR* Value);
	void SetObjectState(uint64 Token, const FString& Key, int32 Value);
	void SetObjectState(uint64 Token, const FString& Key, int64 Value);
	void SetObjectState(uint64 Token, const FString& Key, float Value);
	void SetObjectState(uint64 Token, const FString& Key, double Value);
	void SetObjectState(uint64 Token, const FString& Key, bool bValue);
	/** Object state values are scalars. Convert other values at the call site. */
	template <typename T> void SetObjectState(uint64 Token, const FString& Key, T Value) = delete;
	/** Send the object's final position and a reason, such as `destroyed` or `disabled`. */
	void RemoveObject(uint64 Token, const FString& Reason);
	int32 GetTrackedObjectCount() const;
	EState GetState() const;
	const FString& GetPlaySessionID() const;
	const FString& GetDeviceID() const;
	const FString& GetUserID() const;
	const TOptional<FRecordingContext>& GetRecordingContext() const;
	int32 GetPendingEventCount() const;
	int32 GetDroppedEventCount() const;
	const TArray<FEvent>& GetQueuedEvents() const;

private:
	FClient(FSettings Settings, FEnvironment Environment, TSharedRef<ITransport> Transport, FClock Clock);
	void CompleteSend(int32 ResponseCode, bool bSucceeded);
	void FinishRecording();
	bool bRecordingClosed = false;
	bool bRecordingRegistered = false;
	void SampleMovement(double NowSeconds, const FActivitySample& Sample);
	void EmitMovement(const TCHAR* Reason, double Distance, double Duration, double Rotation, double FOV, double CameraDistance);
	void FillActivityContext(FProperties& Properties, const FActivitySample& Sample) const;
	bool IsCollecting() const;
	bool Enqueue(FEvent Record);
	/** RecordingCount of the dropped records were attached to the recording; they count toward `dropped_events`. */
	void NoteDropped(int32 Count, int32 RecordingCount = 0);
	TOptional<FRecordingContext> ActiveRecording() const;
	void PollState(FDateTime Timestamp, const FActivitySample* Sample);
	void SetObjectStateValue(uint64 Token, const FString& Key, const FProperties& Value);
	void CheckObjects();
	struct FObjectRows;
	void QueueObjectRecords(TArray<FObjectRows>& Rows, bool bKeyframe);

	FTSTicker::FDelegateHandle TickerHandle;
	FSettings Settings;
	TSharedRef<ITransport> Transport;
	FClock Clock;
	FContextProvider ContextProvider;
	FContextProvider StateProvider;
	FActivitySource ActivitySource;
	FContextProvider EventProcessor;
	FMovementSettings MovementSettings;
	EState State;
	FString SessionID;
	TArray<FEvent> Queue;
	TArray<FEvent> InFlight;
	double LastFlushSeconds = 0.0;
	double CurrentSeconds = 0.0;
	double NextRetrySeconds = 0.0;
	double RetryDelaySeconds = 1.0;
	int32 DroppedEvents = 0;
	int32 RecordingDroppedEvents = 0;
	bool bRequestInFlight = false;
	bool bShutdown = false;
	bool bFlushRequested = false;
	bool bClockStarted = false;
	int64 NextSequence = 1;
	/** `game_level.*` from the most recent event, copied onto `$tf_objects` records. */
	FProperties LevelContext;

	FProperties DirectState;
	TMap<FString, TSharedPtr<FJsonValue>> SentState;
	bool bStateKeyframeDue = true;
	double StateKeyframeSeconds = -1.0;
	double LastStatePollSeconds = -1.0;

	struct FTrackedObject
	{
		FString ID;
		FString Kind;
		FPositionSource Position;
		TOptional<FVector> LastKnown;
		TOptional<FVector> LastSent;
		TMap<FString, TSharedPtr<FJsonValue>> State;
		TArray<FString> ChangedState;
	};
	struct FObjectRows
	{
		FString ID;
		FString Kind;
		TOptional<FVector> Sample;
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> State;
		FString RemovedReason;
	};
	TMap<uint64, FTrackedObject> Objects;
	TMap<FString, uint64> ObjectTokens;
	TMap<FString, int32> ObjectCounters;
	TArray<FObjectRows> PendingRemovals;
	uint64 NextObjectToken = 1;
	double NextObjectCheckSeconds = -1.0;
	bool bObjectKeyframeDue = false;
	double ObjectKeyframeSeconds = -1.0;
	bool bWarnedObjectLimit = false;

	TOptional<FActivitySample> LastSample;
	bool bMoving = false;
	double MovementStartSeconds = -1.0;
	double LastMovementSeconds = 0.0;
	double RunStartSeconds = 0.0;
	double LastPingSeconds = 0.0;
	FVector4d RunTotals = FVector4d::Zero();
	FVector4d PingTotals = FVector4d::Zero();
	struct FTeleport
	{
		FString Reason;
		TOptional<FVector> Start;
		FDateTime StartedAt;
	};
	TMap<uint64, FTeleport> Teleports;
	uint64 NextTeleportToken = 1;
};
} // namespace TestingFloor
