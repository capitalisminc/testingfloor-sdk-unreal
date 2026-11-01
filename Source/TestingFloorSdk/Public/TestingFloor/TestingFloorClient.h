#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonValue.h"

class AActor;
class USceneComponent;

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
	int32 LeaseVersion = 0;
	int64 EditorProcessID = 0;
	int64 LeaseExpiresAtUnixMilliseconds = 0;
	bool bTelemetryRequested = false;
	bool HasLiveLease(FDateTime Now) const;
	bool HasActiveLease(FDateTime Now) const;
	/** Read the current launch argument or Saved/TestingFloor/recording-payload.json. */
	static TOptional<FRecordingContext> Discover(FDateTime Now = FDateTime::UtcNow());
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
	/** Permit editor telemetry only while attached to a live desktop recording lease. */
	bool bEnableInEditorWhileRecording = true;
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
	/** Whether the record comes from Play inside the editor (`$tf.editor`), captured when it is queued. */
	bool bEditor = false;
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
TESTINGFLOORSDK_API bool ShouldCollect(const FSettings& Settings, const FEnvironment& Environment, FDateTime Now = FDateTime::UtcNow());
TESTINGFLOORSDK_API FString SerializeEvent(const FEvent& Event, const FSettings& Settings);
TESTINGFLOORSDK_API FString BuildBatch(TConstArrayView<FEvent> Events, const FSettings& Settings, FDateTime SentAt, int32& OutConsumed, int32* OutWritten = nullptr, int32* OutRecordingDropped = nullptr);
/**
 * Fill each unset field the way the default factory does: engine version, OS family, language tag,
 * device model and project version. While Settings is enabled and automation is not running, it also
 * loads or creates the profile ID (UserID) and installation ID (DeviceID). Values already set are kept.
 */
TESTINGFLOORSDK_API void ApplyEngineDefaults(FSettings& Settings);
/** The `$tf.os` family for an Unreal platform name, such as macOS for Mac, or the name itself when it has no family. */
TESTINGFLOORSDK_API FString OperatingSystemFamily(const FString& PlatformName);
/** A BCP 47 tag such as en-US for an Unreal culture name, the language alone when the rest is malformed, or empty. */
TESTINGFLOORSDK_API FString LanguageTag(const FString& CultureName);
/** A device model from FPlatformMisc::GetDeviceMakeAndModel's `make|model` text, such as `Apple iPhone14,2`. */
TESTINGFLOORSDK_API FString DeviceModelName(const FString& MakeAndModel);
/** The profile ID sent as `$user_id`, kept in a slot of the platform save system so it travels with the player's saves. Empty if it cannot be saved. */
TESTINGFLOORSDK_API FString LoadOrCreateProfileID(const FString& SlotName = TEXT("TestingFloorProfile"));
/** The installation ID sent as `$device_id`, kept in a file in the project's Saved folder on this machine. Empty if it cannot be saved. */
TESTINGFLOORSDK_API FString LoadOrCreateInstallationID(const FString& Path = {});

/**
 * Game-thread native client, not a UObject. The default factory also makes it the default client that
 * UTestingFloorTrackedObjectComponent uses.
 */
class TESTINGFLOORSDK_API FClient final : public TSharedFromThis<FClient>
{
public:
	using FContextProvider = TFunction<void(FProperties&)>;
	using FActivitySource = TFunction<FActivitySample()>;
	using FClock = TFunction<FDateTime()>;
	using FRecordingSource = TFunction<TOptional<FRecordingContext>(FDateTime)>;
	/** Returns native centimeters, or unset once the object is gone. */
	using FPositionSource = TFunction<TOptional<FVector>()>;
	/** The new attached recording, or unset when the recording detaches, closes or is suspended. */
	using FRecordingChanged = TMulticastDelegate<void(const TOptional<FRecordingContext>&)>;

	/**
	 * Create an automatically ticked client using the engine environment and HTTP transport. It applies
	 * ApplyEngineDefaults, reads the game viewport for `$tf.screen_*`, and becomes the default client.
	 */
	static TSharedRef<FClient> Create(FSettings Settings);
	/** The client tracked-object components use, if it is still alive. */
	static TSharedPtr<FClient> GetDefault();
	/** Replace the default client, such as one made by the advanced overload. Components waiting for a client register with it. */
	static void SetDefault(const TSharedPtr<FClient>& Client);
	~FClient();
	FClient(const FClient&) = delete;
	FClient& operator=(const FClient&) = delete;
	/**
	 * Advanced creation seam for custom transports and deterministic clocks. It reads no engine metadata or
	 * identity: call ApplyEngineDefaults first for those. Unset context is left out of every record.
	 */
	static TSharedRef<FClient> Create(FSettings Settings, FEnvironment Environment, TSharedRef<ITransport> Transport, FClock Clock = [] { return FDateTime::UtcNow(); }, FRecordingSource RecordingSource = {});
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
	/** Queue a game event. Names beginning with `$tf_` are reserved for Testing Floor records and are refused. */
	bool Track(const FString& Name, const FProperties& Properties = {});
	/** Tick with monotonic real time, including during gameplay pause. */
	void Tick();
	void Tick(double NowSeconds);
	/** Refresh only the recording lifecycle, without sampling movement or duplicating a host Tick. */
	void PollRecordingContext();
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
	 * Returns 0 when the integration is disabled/unconfigured or the live-object limit is reached.
	 * Dormant editor clients retain registrations without sampling until recording starts.
	 * A position source that returns unset removes the object with reason `gone`.
	 */
	uint64 TrackObject(const FString& Kind, FPositionSource PositionCentimeters, const FString& ID = {});
	/** Track an actor's location. The SDK holds it weakly and removes it with reason `destroyed` once it is gone. */
	uint64 TrackObject(const FString& Kind, AActor* Actor, const FString& ID = {});
	/** Track a scene component's world location. The SDK holds it weakly and removes it with reason `destroyed` once it is gone. */
	uint64 TrackObject(const FString& Kind, USceneComponent* Component, const FString& ID = {});
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
	/** The attached telemetry context, absent while collection is blocked or suspended. */
	const TOptional<FRecordingContext>& GetRecordingContext() const;
	/** Live desktop handoff for independent capture policy, including QR suppression. */
	TOptional<FRecordingContext> GetRecordingHandoff() const;
	/** Broadcast when GetRecordingContext changes, before the new recording's state and object keyframes are queued. */
	FRecordingChanged& OnRecordingChanged();
	/** Whether events are accepted now, including the editor recording policy. */
	bool IsCollecting() const;
	/**
	 * Why collection is inactive, or empty while collecting: `shutdown`, `automation`, `master_disabled`,
	 * `missing_configuration`, `collector_rejected` or `editor_policy_disabled`.
	 */
	FString GetCollectionReason() const;
	int32 GetPendingEventCount() const;
	int32 GetDroppedEventCount() const;
	const TArray<FEvent>& GetQueuedEvents() const;

private:
	FClient(FSettings Settings, FEnvironment Environment, TSharedRef<ITransport> Transport, FClock Clock, FRecordingSource RecordingSource);
	void CompleteSend(int32 ResponseCode, bool bSucceeded);
	void FinishRecording(const TCHAR* Reason = TEXT("recording_closed"));
	void CheckRecordingClosed();
	void UpdateRecordingContext(bool bForce);
	bool AttachRecording(const FRecordingContext& Recording);
	void WriteRecordingStatus(const FRecordingContext& Recording, const TCHAR* Status, const TCHAR* Reason);
	bool CollectionPermitted() const;
	bool ConfigurationPermitted() const;
	void CountDroppedRecords(TConstArrayView<FEvent> Records);
	void ResetRecordingBaselines();
	void ObserveRecordingHandoff(const TOptional<FRecordingContext>& Candidate, FDateTime Now);
	void SendAttachmentKeyframes();
	bool bRefreshingRecording = false;
	bool bRecordingSuspended = false;
	bool bRecordingInterrupted = false;
	TOptional<FRecordingContext> ExposedRecording;
	TOptional<FRecordingContext> DiscoveredRecording;
	TOptional<FRecordingContext> LegacyRecordingFallback;
	TSet<FString> RetiredHandoffs;
	TSet<FString> CompletedRecordings;
	TMap<FString, int32> RecordingDrops;
	FDateTime NextRecordingPoll;
	/** Queue state changes and object rows that are waiting for the next poll or check. At shutdown,
	 *  removals the queue still refuses are abandoned; otherwise they wait for later checks. */
	void SendBufferedRecords(bool bShuttingDown);
	bool bRecordingClosed = false;
	bool bRecordingRegistered = false;
	void SampleMovement(double NowSeconds, const FActivitySample& Sample);
	void EmitMovement(const TCHAR* Reason, double Distance, double Duration, double Rotation, double FOV, double CameraDistance);
	void FillActivityContext(FProperties& Properties, const FActivitySample& Sample) const;
	/** Capture the record's editor flag and, when no other source gives one, the engine viewport size. */
	void StampRecord(FEvent& Record) const;
	bool Enqueue(FEvent Record);
	void NotifyRecordingChanged();
	uint64 AddTrackedObject(const FString& Kind, FPositionSource PositionCentimeters, const FString& ID, const TCHAR* GoneReason);
	/** RecordingCount of the dropped records were attached to the recording; they count toward `dropped_events`. */
	void NoteDropped(int32 Count, int32 RecordingCount = 0);
	TOptional<FRecordingContext> ActiveRecording() const;
	/** Returns whether a state keyframe was queued. */
	bool PollState(FDateTime Timestamp, const FActivitySample* Sample);
	void SetObjectStateValue(uint64 Token, const FString& Key, const FProperties& Value);
	/** Returns whether work waits for a later check: a carried removal or an object held behind one. */
	bool CheckObjects();
	bool HasPendingRemoval(const FString& ID) const;
	void TrimPendingRemovals();
	struct FObjectRows;
	void QueueObjectRecords(TArray<FObjectRows>& Rows, bool bKeyframe);

	FTSTicker::FDelegateHandle TickerHandle;
	FTSTicker::FDelegateHandle RecordingTickerHandle;
	FSettings Settings;
	FEnvironment Environment;
	FRecordingSource RecordingSource;
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
	/** Compares names exactly: the same name in another casing is another name. */
	struct FExactNameKeyFuncs : BaseKeyFuncs<FString, FString>
	{
		static const FString& GetSetKey(const FString& Element) { return Element; }
		static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
		static uint32 GetKeyHash(const FString& Key) { return GetTypeHash(Key); }
	};
	TSet<FString, FExactNameKeyFuncs> WarnedReservedNames;
	bool bRequestInFlight = false;
	bool bShutdown = false;
	bool bFlushRequested = false;
	bool bClockStarted = false;
	int64 NextSequence = 1;
	/** `game_level.*` from the most recent event, copied onto `$tf_objects` records. */
	FProperties LevelContext;

	FProperties DirectState;
	TMap<FString, TSharedPtr<FJsonValue>> SentState;
	bool bStateSent = false;
	bool bStateKeyframeDue = true;
	double StateKeyframeSeconds = -1.0;
	double LastStatePollSeconds = -1.0;

	struct FTrackedObject
	{
		FString ID;
		FString Kind;
		FPositionSource Position;
		/** The position read at registration until the first check, then the latest checked position. */
		TOptional<FVector> LastKnown;
		TOptional<FVector> LastSent;
		TMap<FString, TSharedPtr<FJsonValue>> State;
		TArray<FString> ChangedState;
		/** The removal reason once the position source returns unset. */
		FString GoneReason = TEXT("gone");
	};
	struct FObjectRows
	{
		FString ID;
		FString Kind;
		TArray<FVector> Samples;
		TArray<TPair<FString, TSharedPtr<FJsonValue>>> State;
		FString RemovedReason;
	};
	TMap<uint64, FTrackedObject> Objects;
	TMap<FString, uint64> ObjectTokens;
	TMap<FString, int32> ObjectCounters;
	TArray<FObjectRows> PendingRemovals;
	/** Objects whose rows a full queue refused; the next check sends them first. */
	TSet<FString> RefusedObjectIDs;
	uint64 NextObjectToken = 1;
	double NextObjectCheckSeconds = -1.0;
	bool bObjectKeyframeDue = false;
	double ObjectKeyframeSeconds = -1.0;
	bool bWarnedObjectLimit = false;
	/** `$time` of the last object check, in milliseconds. Each check is sent later than the one before. */
	int64 LastObjectCheckMilliseconds = 0;
	TFunction<FIntPoint()> ScreenSizeSource;
	FRecordingChanged RecordingChanged;
	FString NotifiedRecordingUUID;

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
