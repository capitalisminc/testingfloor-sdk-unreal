#include "TestingFloor/TestingFloorClient.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProperties.h"
#include "Internationalization/Internationalization.h"
#include "Internationalization/Culture.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogTestingFloor, Log, All);

namespace TestingFloor
{
namespace Private
{
int32 RecordingEventCount(TConstArrayView<FEvent> Events)
{
	int32 Count = 0;
	for (const FEvent& Event : Events)
	{
		if (Event.Recording.IsSet() && Event.Name != TEXT("$tf_telemetry_complete")) ++Count;
	}
	return Count;
}

constexpr int32 EventByteCap = 32 << 10;
constexpr int32 BodyByteCap = (1 << 20) - (8 << 10);
constexpr double StatePollSeconds = 1.0;
constexpr double KeyframeSeconds = 300.0;
constexpr double ObjectMinMoveCentimeters = 25.0;
constexpr int32 ObjectRecordMaxSamples = 300;
/** Leaves room under the event cap for the envelope and copied level values. */
constexpr int32 ObjectRecordByteBudget = 24 << 10;
const TCHAR* const StateRecordName = TEXT("$tf_state");
const TCHAR* const ObjectsRecordName = TEXT("$tf_objects");

FString Encode(const TSharedRef<FJsonObject>& Object)
{
	FString Json;
	FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
	return Json;
}

int32 Utf8Length(const FString& Text)
{
	return FTCHARToUTF8(*Text).Length();
}

FString DecodeWindowsQuotes(const FString& Text)
{
	FString Result;
	Result.Reserve(Text.Len());
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		if (Text[Index] != TEXT('\\'))
		{
			Result.AppendChar(Text[Index]);
			continue;
		}
		const int32 Start = Index;
		while (Index < Text.Len() && Text[Index] == TEXT('\\'))
		{
			++Index;
		}
		const int32 Count = Index - Start;
		const bool bBeforeQuote = Index < Text.Len() && Text[Index] == TEXT('"');
		Result += FString::ChrN(bBeforeQuote ? Count / 2 : Count, TEXT('\\'));
		if (Index < Text.Len())
		{
			Result.AppendChar(Text[Index]);
		}
	}
	return Result;
}

TOptional<FRecordingContext> ParseCommandLineValue(const FString& Value, FDateTime Now)
{
	int32 Start = 0;
	while (Start < Value.Len() && (FChar::IsWhitespace(Value[Start]) || Value[Start] == TEXT('"') || Value[Start] == TEXT('\'')))
	{
		++Start;
	}
	if (Start == Value.Len() || Value[Start] != TEXT('{'))
	{
		return {};
	}
	int32 Depth = 0;
	bool bInString = false;
	bool bEscaped = false;
	for (int32 Index = Start; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if (bInString)
		{
			if (bEscaped)
			{
				bEscaped = false;
			}
			else if (Character == TEXT('\\'))
			{
				bEscaped = true;
			}
			else if (Character == TEXT('"'))
			{
				bInString = false;
			}
			continue;
		}
		if (Character == TEXT('"'))
		{
			bInString = true;
		}
		else if (Character == TEXT('{'))
		{
			++Depth;
		}
		else if (Character == TEXT('}') && --Depth == 0)
		{
			const int32 Next = Index + 1;
			if (Next < Value.Len() && !FChar::IsWhitespace(Value[Next]) && Value[Next] != TEXT('"') && Value[Next] != TEXT('\''))
			{
				return {};
			}
			return FRecordingContext::Parse(Value.Mid(Start, Index - Start + 1), Now);
		}
	}
	return {};
}

double MovementThreshold(double Value)
{
	return FMath::IsFinite(Value) ? FMath::Max(0.0, Value) : 0.0;
}

double Distance(const TOptional<FVector>& Current, const TOptional<FVector>& Previous)
{
	return Current.IsSet() && Previous.IsSet() ? FVector::Distance(Current.GetValue(), Previous.GetValue()) : 0.0;
}

FVector4d ActivityStep(const FActivitySample& Current, const FActivitySample& Previous, const FMovementSettings& Settings)
{
	const double DistanceMeters = Distance(Current.PlayerPosition, Previous.PlayerPosition) / 100.0;
	const double CameraDistance = Distance(Current.CameraPosition, Previous.CameraPosition) / 100.0;
	const double Rotation = Current.CameraRotation.IsSet() && Previous.CameraRotation.IsSet()
		? FMath::RadiansToDegrees(Current.CameraRotation.GetValue().AngularDistance(Previous.CameraRotation.GetValue()))
		: 0.0;
	const double FOV = Current.CameraFOV.IsSet() && Previous.CameraFOV.IsSet()
		? FMath::Abs(Current.CameraFOV.GetValue() - Previous.CameraFOV.GetValue())
		: 0.0;
	return FVector4d(
		DistanceMeters > 0.0 && DistanceMeters >= Settings.MinStep ? DistanceMeters : 0.0,
		Rotation > 0.0 && Rotation >= Settings.MinStepRotationDegrees ? Rotation : 0.0,
		FOV > 0.0 && FOV >= Settings.MinStepFOVDegrees ? FOV : 0.0,
		CameraDistance > 0.0 && CameraDistance >= Settings.MinStepCameraDistance ? CameraDistance : 0.0);
}

bool SameValue(const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
{
	return Left.IsValid() && Right.IsValid() && FJsonValue::CompareEqual(*Left, *Right);
}

TOptional<FVector> FinitePosition(const TOptional<FVector>& Position)
{
	return Position.IsSet() && !Position->ContainsNaN() ? Position : TOptional<FVector>();
}

/** Native centimeters rounded to 1 cm, without a negative zero. */
TSharedPtr<FJsonValue> Centimeters(double Value)
{
	const double Rounded = FMath::RoundToDouble(Value);
	return MakeShared<FJsonValueNumber>(Rounded == 0.0 ? 0.0 : Rounded);
}

int32 EstimateBytes(const FString& ID, const FString& Kind, bool bSample, const TArray<TPair<FString, TSharedPtr<FJsonValue>>>& State, const FString& Reason)
{
	int32 Bytes = bSample ? ID.Len() + Kind.Len() + 48 : 0;
	for (const auto& Pair : State)
	{
		Bytes += ID.Len() + Pair.Key.Len() + (Pair.Value->Type == EJson::String ? Pair.Value->AsString().Len() : 24) + 12;
	}
	return Bytes + (Reason.IsEmpty() ? 0 : ID.Len() + Reason.Len() + 8);
}
} // namespace Private

void FProperties::Put(const FString& Key, TSharedPtr<FJsonValue> Value)
{
	if (!Key.TrimStartAndEnd().IsEmpty() && !Key.StartsWith(TEXT("$")))
	{
		Properties.Add(Key, MoveTemp(Value));
	}
}

FProperties& FProperties::Set(const FString& Key, const FString& Value)
{
	Put(Key, MakeShared<FJsonValueString>(Value));
	return *this;
}

FProperties& FProperties::Set(const FString& Key, const TCHAR* Value)
{
	return Value ? Set(Key, FString(Value)) : *this;
}

FProperties& FProperties::Set(const FString& Key, int32 Value)
{
	return Set(Key, static_cast<int64>(Value));
}

FProperties& FProperties::Set(const FString& Key, int64 Value)
{
	Put(Key, MakeShared<FJsonValueNumberString>(LexToString(Value)));
	return *this;
}

FProperties& FProperties::Set(const FString& Key, float Value)
{
	return Set(Key, static_cast<double>(Value));
}

FProperties& FProperties::Set(const FString& Key, double Value)
{
	if (FMath::IsFinite(Value))
	{
		Put(Key, MakeShared<FJsonValueNumber>(Value));
	}
	return *this;
}

FProperties& FProperties::Set(const FString& Key, bool bValue)
{
	Put(Key, MakeShared<FJsonValueBoolean>(bValue));
	return *this;
}

FProperties& FProperties::Set(const FString& Key, const FGuid& Value)
{
	return Set(Key, Value.ToString(EGuidFormats::DigitsWithHyphensLower));
}

FProperties& FProperties::Set(const FString& Key, const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Array;
	for (const FString& Value : Values)
	{
		Array.Add(MakeShared<FJsonValueString>(Value));
	}
	Put(Key, MakeShared<FJsonValueArray>(MoveTemp(Array)));
	return *this;
}

FProperties& FProperties::Set(const FString& Key, const TArray<int32>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Array;
	for (const int32 Value : Values)
	{
		Array.Add(MakeShared<FJsonValueNumber>(Value));
	}
	Put(Key, MakeShared<FJsonValueArray>(MoveTemp(Array)));
	return *this;
}

FProperties& FProperties::SetIfPresent(const FString& Key, const FString& Value)
{
	if (!Value.TrimStartAndEnd().IsEmpty())
	{
		Set(Key, Value);
	}
	return *this;
}

FProperties& FProperties::SetVector(const FString& Prefix, const FVector& Position)
{
	return Set(Prefix + TEXT(".x"), Position.X).Set(Prefix + TEXT(".y"), Position.Y).Set(Prefix + TEXT(".z"), Position.Z);
}

void FProperties::Merge(const FProperties& Overrides)
{
	for (const auto& Pair : Overrides.Properties)
	{
		Properties.Add(Pair.Key, Pair.Value);
	}
}

void FProperties::Remove(const FString& Key)
{
	Properties.Remove(Key);
}

const TMap<FString, TSharedPtr<FJsonValue>>& FProperties::GetValues() const
{
	return Properties;
}

TOptional<FRecordingContext> FRecordingContext::Discover()
{
	FString Sidecar;
	TArray<FString> Payloads;
	if (FFileHelper::LoadFileToString(Sidecar, *(FPaths::ProjectSavedDir() / TEXT("TestingFloor/recording-payload.json"))))
	{
		Payloads.Add(MoveTemp(Sidecar));
	}
	return Resolve(FCommandLine::Get(), Payloads, FDateTime::UtcNow());
}

TOptional<FRecordingContext> FRecordingContext::Parse(const FString& Json, FDateTime Now)
{
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
	{
		return {};
	}
	FRecordingContext Result;
	Object->TryGetStringField(TEXT("recording_uuid"), Result.RecordingUUID);
	Object->TryGetStringField(TEXT("telemetry_directory"), Result.TelemetryDirectory);
	Result.RecordingUUID.TrimStartAndEndInline();
	if (Result.RecordingUUID.IsEmpty())
	{
		return {};
	}
	int64 CreatedAt = 0;
	const int64 NowMilliseconds = Now.ToUnixTimestamp() * 1000 + Now.GetMillisecond();
	if (Object->TryGetNumberField(TEXT("created_at_unix_ms"), CreatedAt) && CreatedAt > 0
		&& CreatedAt < NowMilliseconds - 12LL * 60 * 60 * 1000)
	{
		return {};
	}
	int64 PlaytestID = 0;
	if (Object->TryGetNumberField(TEXT("playtest_id"), PlaytestID) && PlaytestID > 0)
	{
		Result.PlaytestID = PlaytestID;
	}
	Object->TryGetBoolField(TEXT("suppress_qr"), Result.bSuppressQR);
	return Result;
}

TOptional<FRecordingContext> FRecordingContext::FromCommandLine(const FString& CommandLine, FDateTime Now)
{
	const FString Prefix = TEXT("--testing-floor=");
	int32 Index = 0;
	while (Index < CommandLine.Len())
	{
		while (Index < CommandLine.Len() && FChar::IsWhitespace(CommandLine[Index]))
		{
			++Index;
		}
		int32 ArgumentStart = Index;
		if (ArgumentStart < CommandLine.Len() && (CommandLine[ArgumentStart] == TEXT('"') || CommandLine[ArgumentStart] == TEXT('\'')))
		{
			++ArgumentStart;
		}
		if (CommandLine.Mid(ArgumentStart, Prefix.Len()) == Prefix)
		{
			const FString Value = CommandLine.Mid(ArgumentStart + Prefix.Len());
			if (const auto Recording = Private::ParseCommandLineValue(Value, Now); Recording.IsSet())
			{
				return Recording;
			}
			return Private::ParseCommandLineValue(Private::DecodeWindowsQuotes(Value), Now);
		}
		TCHAR Quote = 0;
		while (Index < CommandLine.Len())
		{
			const TCHAR Character = CommandLine[Index];
			if (!Quote && FChar::IsWhitespace(Character))
			{
				break;
			}
			if (Character == TEXT('\\') && Quote != TEXT('\''))
			{
				const int32 Start = Index;
				while (Index < CommandLine.Len() && CommandLine[Index] == TEXT('\\'))
				{
					++Index;
				}
				if ((Index - Start) % 2 != 0 && Index < CommandLine.Len() && CommandLine[Index] == TEXT('"'))
				{
					++Index;
				}
				continue;
			}
			if (Character == TEXT('"') || Character == TEXT('\''))
			{
				if (!Quote)
				{
					Quote = Character;
				}
				else if (Quote == Character)
				{
					Quote = 0;
				}
			}
			++Index;
		}
	}
	return {};
}

TOptional<FRecordingContext> FRecordingContext::Resolve(const FString& CommandLine, TConstArrayView<FString> SidecarPayloads, FDateTime Now)
{
	if (const auto Recording = FromCommandLine(CommandLine, Now); Recording.IsSet())
	{
		return Recording;
	}
	for (const FString& Json : SidecarPayloads)
	{
		if (const auto Recording = Parse(Json, Now); Recording.IsSet())
		{
			return Recording;
		}
	}
	return {};
}

bool ShouldCollect(const FSettings& Settings, const FEnvironment& Environment)
{
	return Settings.bEnabled && !Environment.bAutomation && (!Environment.bEditor || Settings.bEnableInEditor)
		&& !Settings.WriteKey.TrimStartAndEnd().IsEmpty() && !Settings.Endpoint.TrimStartAndEnd().IsEmpty();
}

FString SerializeEvent(const FEvent& Event, const FSettings& Settings)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const auto& Pair : Event.Properties.GetValues())
	{
		Object->SetField(Pair.Key, Pair.Value);
	}
	Object->SetStringField(TEXT("$id"), Event.ID.ToString(EGuidFormats::DigitsWithHyphensLower));
	Object->SetStringField(TEXT("$name"), Event.Name);
	Object->SetStringField(TEXT("$time"), Event.Timestamp.ToIso8601());
	Object->SetNumberField(TEXT("$schema"), 2);
	Object->SetStringField(TEXT("$session_id"), Event.SessionID);
	if (!Settings.DeviceID.IsEmpty())
	{
		Object->SetStringField(TEXT("$device_id"), Settings.DeviceID);
	}
	if (!Settings.UserID.IsEmpty())
	{
		Object->SetStringField(TEXT("$user_id"), Settings.UserID);
	}
	const TSharedRef<FJsonObject> Platform = MakeShared<FJsonObject>();
	Platform->SetStringField(TEXT("sdk_version"), TEXT("0.3.0"));
	if (Event.Sequence > 0)
	{
		Platform->SetField(TEXT("seq"), MakeShared<FJsonValueNumberString>(LexToString(Event.Sequence)));
	}
	if (Event.bKeyframe)
	{
		Platform->SetBoolField(TEXT("keyframe"), true);
	}
	Platform->SetStringField(TEXT("engine"), TEXT("Unreal ") + Settings.EngineVersion);
	Platform->SetStringField(TEXT("app_version"), Settings.AppVersion);
	Platform->SetStringField(TEXT("os"), Settings.OperatingSystem);
	Platform->SetStringField(TEXT("locale"), Settings.Locale);
	if (!Settings.DeviceModel.IsEmpty())
	{
		Platform->SetStringField(TEXT("device_model"), Settings.DeviceModel);
	}
	const FIntPoint ScreenSize = Event.ScreenSize.X > 0 && Event.ScreenSize.Y > 0 ? Event.ScreenSize : Settings.ScreenSize;
	if (ScreenSize.X > 0)
	{
		Platform->SetNumberField(TEXT("screen_width"), ScreenSize.X);
	}
	if (ScreenSize.Y > 0)
	{
		Platform->SetNumberField(TEXT("screen_height"), ScreenSize.Y);
	}
	if (Event.Recording.IsSet())
	{
		const FRecordingContext& Recording = Event.Recording.GetValue();
		Platform->SetStringField(TEXT("recording_uuid"), Recording.RecordingUUID);
		if (Recording.PlaytestID.IsSet())
		{
			Platform->SetField(TEXT("playtest_id"), MakeShared<FJsonValueNumberString>(LexToString(Recording.PlaytestID.GetValue())));
		}
	}
	Object->SetObjectField(TEXT("$tf"), Platform);
	return Private::Encode(Object);
}

FString BuildBatch(TConstArrayView<FEvent> Events, const FSettings& Settings, FDateTime SentAt, int32& OutConsumed, int32* OutWritten, int32* OutRecordingDropped)
{
	FString Body;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("$write_key"), Settings.WriteKey);
	Writer->WriteValue(TEXT("$sent_at"), SentAt.ToIso8601());
	Writer->WriteArrayStart(TEXT("events"));
	int32 BodyBytes = Private::Utf8Length(Body) + 2;
	int32 Written = 0;
	int32 RecordingDropped = 0;
	OutConsumed = 0;
	for (const FEvent& Event : Events)
	{
		// A completion marker is its own request, after preceding requests succeed.
		if ((Event.Name == TEXT("$tf_telemetry_complete") && OutConsumed > 0)
			|| (OutConsumed > 0 && Events[0].Name == TEXT("$tf_telemetry_complete"))) break;
		if (Written >= FMath::Clamp(Settings.BatchMaxEvents, 1, 2000))
		{
			break;
		}
		const FString EventJson = SerializeEvent(Event, Settings);
		const int32 EventBytes = Private::Utf8Length(EventJson);
		if (EventBytes > Private::EventByteCap)
		{
			if (Event.Recording.IsSet() && Event.Name != TEXT("$tf_telemetry_complete")) ++RecordingDropped;
			++OutConsumed;
			continue;
		}
		if (BodyBytes + EventBytes + 1 > Private::BodyByteCap)
		{
			break;
		}
		Writer->WriteRawJSONValue(FStringView(EventJson));
		BodyBytes += EventBytes + 1;
		++Written;
		++OutConsumed;
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();
	if (OutWritten) *OutWritten = Written;
	if (OutRecordingDropped) *OutRecordingDropped = RecordingDropped;
	return Written > 0 ? Body : FString();
}

bool FActivitySample::IsEmpty() const
{
	return !PlayerPosition.IsSet() && !CameraPosition.IsSet() && !CameraRotation.IsSet() && !CameraFOV.IsSet();
}

TSharedRef<FClient> FClient::Create(FSettings Settings)
{
	if (!Settings.Recording.IsSet()) Settings.Recording = FRecordingContext::Discover();
	if (Settings.EngineVersion.IsEmpty()) Settings.EngineVersion = FEngineVersion::Current().ToString();
	if (Settings.OperatingSystem.IsEmpty()) Settings.OperatingSystem = FPlatformProperties::PlatformName();
	if (Settings.Locale.IsEmpty()) Settings.Locale = FInternationalization::Get().GetCurrentCulture()->GetName();
	const auto Client = Create(MoveTemp(Settings), {GIsEditor, GIsAutomationTesting}, MakeHttpTransport());
	if (Client->GetState() == EState::Ok)
	{
		const TWeakPtr<FClient> WeakClient = Client;
		Client->TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakClient](float) {
			if (const auto Active = WeakClient.Pin())
			{
				Active->Tick();
				return true;
			}
			return false;
		}));
	}
	return Client;
}

FClient::~FClient()
{
	FTSTicker::RemoveTicker(TickerHandle);
}

TSharedRef<FClient> FClient::Create(FSettings Settings, FEnvironment Environment, TSharedRef<ITransport> Transport, FClock Clock)
{
	return MakeShareable(new FClient(MoveTemp(Settings), Environment, MoveTemp(Transport), MoveTemp(Clock)));
}

FClient::FClient(FSettings InSettings, FEnvironment Environment, TSharedRef<ITransport> InTransport, FClock InClock)
	: Settings(MoveTemp(InSettings)), Transport(MoveTemp(InTransport)), Clock(MoveTemp(InClock))
{
	State = ShouldCollect(Settings, Environment) ? EState::Ok
		: !Settings.bEnabled || Environment.bAutomation || (Environment.bEditor && !Settings.bEnableInEditor)
		? EState::Disabled
		: EState::NotConfigured;
	SessionID = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	if (State == EState::Ok && Settings.Recording.IsSet() && !Settings.Recording->TelemetryDirectory.IsEmpty())
	{
		const FString Path = Settings.Recording->TelemetryDirectory / (SessionID + TEXT(".session"));
		// The recorder creates the directory. Register before admitting events.
		bRecordingRegistered = IFileManager::Get().DirectoryExists(*Settings.Recording->TelemetryDirectory)
			&& FFileHelper::SaveStringToFile(TEXT(""), *Path);
		if (!bRecordingRegistered) Settings.Recording.Reset();
	}

}

void FClient::SetContextProvider(FContextProvider Provider)
{
	ContextProvider = MoveTemp(Provider);
}

void FClient::SetStateProvider(FContextProvider Provider)
{
	StateProvider = MoveTemp(Provider);
}

void FClient::ClearState(const FString& Key)
{
	DirectState.Remove(Key);
}

void FClient::SetActivitySource(FActivitySource Source)
{
	ActivitySource = MoveTemp(Source);
	ResetMovement();
}

void FClient::SetEventProcessor(FContextProvider Processor)
{
	EventProcessor = MoveTemp(Processor);
}

void FClient::SetMovementSettings(const FMovementSettings& InSettings)
{
	MovementSettings = InSettings;
	MovementSettings.MinPingDistance = Private::MovementThreshold(InSettings.MinPingDistance);
	MovementSettings.MinPingIntervalSeconds = Private::MovementThreshold(InSettings.MinPingIntervalSeconds);
	MovementSettings.StartGraceSeconds = Private::MovementThreshold(InSettings.StartGraceSeconds);
	MovementSettings.StopGraceSeconds = Private::MovementThreshold(InSettings.StopGraceSeconds);
	MovementSettings.MinStep = Private::MovementThreshold(InSettings.MinStep);
	MovementSettings.MinPingRotationDegrees = Private::MovementThreshold(InSettings.MinPingRotationDegrees);
	MovementSettings.MinStepRotationDegrees = Private::MovementThreshold(InSettings.MinStepRotationDegrees);
	MovementSettings.MinPingFOVDegrees = Private::MovementThreshold(InSettings.MinPingFOVDegrees);
	MovementSettings.MinStepFOVDegrees = Private::MovementThreshold(InSettings.MinStepFOVDegrees);
	MovementSettings.MinPingCameraDistance = Private::MovementThreshold(InSettings.MinPingCameraDistance);
	MovementSettings.MinStepCameraDistance = Private::MovementThreshold(InSettings.MinStepCameraDistance);
	ResetMovement();
}

void FClient::SetMovementTrackingEnabled(bool bEnabled)
{
	if (Settings.bMovementTracking != bEnabled)
	{
		Settings.bMovementTracking = bEnabled;
		ResetMovement();
	}
}

void FClient::FillActivityContext(FProperties& Properties, const FActivitySample& Sample) const
{
	if (Sample.PlayerPosition.IsSet())
	{
		Properties.SetVector(TEXT("player.position"), Sample.PlayerPosition.GetValue());
	}
	if (Sample.CameraPosition.IsSet())
	{
		Properties.SetVector(TEXT("camera.position"), Sample.CameraPosition.GetValue());
	}
	if (Sample.CameraEulerDegrees.IsSet())
	{
		Properties.SetVector(TEXT("camera.euler"), Sample.CameraEulerDegrees.GetValue());
	}
	else if (Sample.CameraRotation.IsSet())
	{
		const FRotator Rotation = Sample.CameraRotation.GetValue().Rotator();
		Properties.SetVector(TEXT("camera.euler"), FVector(Rotation.Roll, Rotation.Pitch, Rotation.Yaw));
	}
	if (Sample.CameraRotation.IsSet())
	{
		Properties.SetVector(TEXT("camera.forward"), Sample.CameraRotation.GetValue().GetForwardVector());
	}
	if (Sample.CameraFOV.IsSet())
	{
		Properties.Set(TEXT("camera.fov"), Sample.CameraFOV.GetValue());
	}
}

bool FClient::IsCollecting() const
{
	return (State == EState::Ok || State == EState::NetworkDown) && !bShutdown;
}

bool FClient::Track(const FString& Name, const FProperties& Properties)
{
	check(IsInGameThread());
	if (!IsCollecting() || Name.TrimStartAndEnd().IsEmpty())
	{
		return false;
	}
	if (bRecordingRegistered && !bRecordingClosed
		&& IFileManager::Get().FileExists(*(Settings.Recording->TelemetryDirectory / TEXT("closed.json")))) FinishRecording();
	const FDateTime Timestamp = Clock();
	TOptional<FActivitySample> Sample;
	if (ActivitySource)
	{
		Sample = ActivitySource();
	}
	// State changes are queued first, so the state at every event is exact.
	PollState(Timestamp, Sample.GetPtrOrNull());
	if (GetPendingEventCount() >= FMath::Max(1, Settings.QueueMaxEvents))
	{
		NoteDropped(1, ActiveRecording().IsSet() ? 1 : 0);
		return false;
	}
	FEvent Event{ FGuid::NewGuid(), Name, Timestamp, SessionID, {} };
	Event.Recording = ActiveRecording();
	if (ContextProvider)
	{
		ContextProvider(Event.Properties);
	}
	if (Sample.IsSet())
	{
		FillActivityContext(Event.Properties, Sample.GetValue());
		Event.ScreenSize = Sample->ViewportSize;
	}
	Event.Properties.Merge(Properties);
	if (EventProcessor)
	{
		EventProcessor(Event.Properties);
	}
	FProperties Level;
	for (const auto& Pair : Event.Properties.GetValues())
	{
		if (Pair.Key.StartsWith(TEXT("game_level.")))
		{
			Level.Put(Pair.Key, Pair.Value);
		}
	}
	if (!Enqueue(MoveTemp(Event)))
	{
		return false;
	}
	LevelContext = MoveTemp(Level);
	return true;
}

bool FClient::Enqueue(FEvent Record)
{
	if (GetPendingEventCount() >= FMath::Max(1, Settings.QueueMaxEvents))
	{
		NoteDropped(1, Record.Recording.IsSet() ? 1 : 0);
		return false;
	}
	Record.Sequence = NextSequence;
	if (Private::Utf8Length(SerializeEvent(Record, Settings)) > Private::EventByteCap)
	{
		NoteDropped(1, Record.Recording.IsSet() ? 1 : 0);
		return false;
	}
	++NextSequence;
	Queue.Add(MoveTemp(Record));
	return true;
}

TOptional<FRecordingContext> FClient::ActiveRecording() const
{
	return bRecordingClosed ? TOptional<FRecordingContext>() : Settings.Recording;
}

void FClient::NoteDropped(int32 Count, int32 RecordingCount)
{
	RecordingDroppedEvents += RecordingCount;
	if (Count > 0)
	{
		DroppedEvents += Count;
		// Keyframes let a reader recover from any lost record.
		bStateKeyframeDue = true;
		bObjectKeyframeDue = true;
	}
}

void FClient::PollState(FDateTime Timestamp, const FActivitySample* Sample)
{
	FProperties Current;
	if (Sample && Sample->CameraFOV.IsSet())
	{
		Current.Set(TEXT("camera.fov_axis"), TEXT("horizontal"));
	}
	if (Sample && Sample->ViewportSize.X > 0 && Sample->ViewportSize.Y > 0)
	{
		Current.Set(TEXT("viewport.width"), Sample->ViewportSize.X).Set(TEXT("viewport.height"), Sample->ViewportSize.Y);
	}
	if (StateProvider)
	{
		StateProvider(Current);
	}
	Current.Merge(DirectState);
	const TMap<FString, TSharedPtr<FJsonValue>>& Values = Current.GetValues();
	const bool bKeyframe = bStateKeyframeDue;
	FEvent Record{ FGuid::NewGuid(), Private::StateRecordName, Timestamp, SessionID, {} };
	Record.bKeyframe = bKeyframe;
	if (bKeyframe)
	{
		Record.Properties = Current;
	}
	else
	{
		for (const auto& Pair : Values)
		{
			if (!Private::SameValue(SentState.FindRef(Pair.Key), Pair.Value))
			{
				Record.Properties.Put(Pair.Key, Pair.Value);
			}
		}
		for (const auto& Pair : SentState)
		{
			if (!Values.Contains(Pair.Key))
			{
				Record.Properties.Put(Pair.Key, MakeShared<FJsonValueNull>());
			}
		}
	}
	if (Record.Properties.GetValues().IsEmpty() || !Enqueue(MoveTemp(Record)))
	{
		return;
	}
	SentState = Values;
	if (bKeyframe)
	{
		bStateKeyframeDue = false;
		StateKeyframeSeconds = bClockStarted ? CurrentSeconds : -1.0;
	}
}

void FClient::Tick()
{
	Tick(FPlatformTime::Seconds());
}

void FClient::Tick(double NowSeconds)
{
	check(IsInGameThread());
	CurrentSeconds = NowSeconds;
	if (bRecordingRegistered && !bRecordingClosed
		&& IFileManager::Get().FileExists(*(Settings.Recording->TelemetryDirectory / TEXT("closed.json")))) FinishRecording();
	if (!bClockStarted)
	{
		bClockStarted = true;
		StateKeyframeSeconds = ObjectKeyframeSeconds = NowSeconds;
	}
	if (Settings.bMovementTracking && IsCollecting() && ActivitySource)
	{
		if (Teleports.IsEmpty())
		{
			SampleMovement(NowSeconds, ActivitySource());
		}
		else
		{
			ResetMovement();
		}
	}
	if (IsCollecting())
	{
		if (NowSeconds - StateKeyframeSeconds >= Private::KeyframeSeconds)
		{
			bStateKeyframeDue = true;
		}
		if (LastStatePollSeconds < 0.0 || NowSeconds - LastStatePollSeconds >= Private::StatePollSeconds)
		{
			LastStatePollSeconds = NowSeconds;
			TOptional<FActivitySample> Sample;
			if (ActivitySource)
			{
				Sample = ActivitySource();
			}
			PollState(Clock(), Sample.GetPtrOrNull());
		}
		if (NowSeconds - ObjectKeyframeSeconds >= Private::KeyframeSeconds)
		{
			bObjectKeyframeDue = true;
		}
		if (NowSeconds >= NextObjectCheckSeconds)
		{
			const double Interval = 1.0 / FMath::Clamp(FMath::IsFinite(Settings.ObjectChecksPerSecond) ? Settings.ObjectChecksPerSecond : 4.0, 1.0, 10.0);
			NextObjectCheckSeconds = NextObjectCheckSeconds < 0.0 || NextObjectCheckSeconds + Interval <= NowSeconds
				? NowSeconds + Interval
				: NextObjectCheckSeconds + Interval;
			CheckObjects();
		}
	}
	if (!Queue.IsEmpty() && !bRequestInFlight && NowSeconds >= NextRetrySeconds
		&& (bFlushRequested || Queue.Num() >= FMath::Max(1, Settings.BatchMaxEvents)
			|| NowSeconds - LastFlushSeconds >= FMath::Max(0.0, Settings.FlushIntervalSeconds)))
	{
		Flush();
	}
}

void FClient::FinishRecording()
{
	if (bRecordingClosed || !bRecordingRegistered) return;
	bRecordingClosed = true;
	FEvent Marker{ FGuid::NewGuid(), TEXT("$tf_telemetry_complete"), Clock(), SessionID, {} };
	Marker.Recording = Settings.Recording;
	// Control records must remain possible even when the gameplay queue is full.
	Queue.Add(MoveTemp(Marker));
	bFlushRequested = true;
}

void FClient::Flush()
{
	bFlushRequested = true;
	if (Queue.IsEmpty() || bRequestInFlight || CurrentSeconds < NextRetrySeconds || (State != EState::Ok && State != EState::NetworkDown))
	{
		return;
	}
	int32 Consumed = 0;
	if (Queue[0].Name == TEXT("$tf_telemetry_complete"))
	{
		Queue[0].Properties.Set(TEXT("dropped_events"), RecordingDroppedEvents);
	}
	int32 Written = 0;
	int32 RecordingSkipped = 0;
	const FString Body = BuildBatch(Queue, Settings, Clock(), Consumed, &Written, &RecordingSkipped);
	NoteDropped(Consumed - Written, RecordingSkipped);
	if (Consumed == 0)
	{
		return;
	}
	InFlight.Append(Queue.GetData(), Consumed);
	Queue.RemoveAt(0, Consumed, EAllowShrinking::No);
	if (Body.IsEmpty())
	{
		InFlight.Reset();
		return;
	}
	bRequestInFlight = true;
	LastFlushSeconds = CurrentSeconds;
	const TWeakPtr<FClient> WeakThis = AsShared();
	FString Endpoint = Settings.Endpoint.TrimStartAndEnd();
	while (Endpoint.EndsWith(TEXT("/")))
	{
		Endpoint.LeftChopInline(1);
	}
	Transport->Send(Endpoint + TEXT("/v1/batch"), Body,
		[WeakThis](int32 Code, bool bSucceeded) {
			if (const TSharedPtr<FClient> Client = WeakThis.Pin())
			{
				Client->CompleteSend(Code, bSucceeded);
			}
		});
}

void FClient::CompleteSend(int32 ResponseCode, bool bSucceeded)
{
	check(IsInGameThread());
	bRequestInFlight = false;
	if (bSucceeded && ResponseCode >= 200 && ResponseCode < 300)
	{
		State = EState::Ok;
		InFlight.Reset();
		RetryDelaySeconds = 1.0;
		NextRetrySeconds = 0.0;
		bFlushRequested = !Queue.IsEmpty();
	}
	else if (ResponseCode == 401 || ResponseCode == 403)
	{
		State = EState::Rejected;
		DroppedEvents += InFlight.Num() + Queue.Num();
		RecordingDroppedEvents += Private::RecordingEventCount(InFlight);
		RecordingDroppedEvents += Private::RecordingEventCount(Queue);
		InFlight.Reset();
		Queue.Reset();
	}
	else if (ResponseCode >= 400 && ResponseCode < 500 && ResponseCode != 408 && ResponseCode != 429)
	{
		State = EState::Ok;
		RetryDelaySeconds = 1.0;
		NextRetrySeconds = 0.0;
		NoteDropped(InFlight.Num(), Private::RecordingEventCount(InFlight));
		InFlight.Reset();
		bFlushRequested = !Queue.IsEmpty();
	}
	else
	{
		State = EState::NetworkDown;
		Queue.Insert(InFlight, 0);
		InFlight.Reset();
		NextRetrySeconds = CurrentSeconds + RetryDelaySeconds;
		RetryDelaySeconds = FMath::Min(30.0, RetryDelaySeconds * 2.0);
	}
}

void FClient::Shutdown(double TimeoutSeconds)
{
	FTSTicker::RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	if (IsCollecting())
	{
		// Send registrations, removals and state changes that are waiting for the next check.
		CheckObjects();
	}
	bShutdown = true;
	FinishRecording();
	Flush();
	const double Deadline = FPlatformTime::Seconds() + FMath::Clamp(TimeoutSeconds, 0.0, 5.0);
	double Previous = FPlatformTime::Seconds();
	while (GetPendingEventCount() > 0 && FPlatformTime::Seconds() < Deadline)
	{
		const double Now = FPlatformTime::Seconds();
		Transport->Pump(static_cast<float>(Now - Previous));
		Tick(CurrentSeconds + Now - Previous);
		Previous = Now;
		FPlatformProcess::SleepNoStats(0.01f);
	}
}

void FClient::ResetMovement()
{
	LastSample.Reset();
	bMoving = false;
	MovementStartSeconds = -1.0;
	RunTotals = FVector4d::Zero();
	PingTotals = FVector4d::Zero();
}

void FClient::SampleMovement(double NowSeconds, const FActivitySample& Sample)
{
	if (Sample.IsEmpty())
	{
		ResetMovement();
		return;
	}
	if (!LastSample.IsSet())
	{
		LastSample = Sample;
		LastPingSeconds = NowSeconds;
		return;
	}
	const FVector4d Step = Private::ActivityStep(Sample, LastSample.GetValue(), MovementSettings);
	LastSample = Sample;
	const bool bMoved = Step.X > 0.0 || Step.Y > 0.0 || Step.Z > 0.0 || Step.W > 0.0;
	if (bMoved)
	{
		RunTotals += Step;
		PingTotals += Step;
		LastMovementSeconds = NowSeconds;
		if (!bMoving)
		{
			if (MovementStartSeconds < 0.0)
			{
				MovementStartSeconds = NowSeconds;
			}
			if (NowSeconds - MovementStartSeconds >= FMath::Max(0.0, MovementSettings.StartGraceSeconds))
			{
				bMoving = true;
				RunStartSeconds = MovementStartSeconds;
				MovementStartSeconds = -1.0;
				LastPingSeconds = NowSeconds;
				PingTotals = FVector4d::Zero();
				EmitMovement(TEXT("start"), 0.0, 0.0, 0.0, 0.0, 0.0);
			}
		}
	}
	else if (!bMoving)
	{
		MovementStartSeconds = -1.0;
	}
	if (bMoving && NowSeconds - LastMovementSeconds >= FMath::Max(0.0, MovementSettings.StopGraceSeconds))
	{
		EmitMovement(TEXT("stop"), RunTotals.X, NowSeconds - RunStartSeconds, RunTotals.Y, RunTotals.Z, RunTotals.W);
		bMoving = false;
		MovementStartSeconds = -1.0;
		RunTotals = FVector4d::Zero();
		PingTotals = FVector4d::Zero();
	}
	if (bMoving && NowSeconds - LastPingSeconds >= FMath::Max(0.0, MovementSettings.MinPingIntervalSeconds)
		&& (PingTotals.X >= MovementSettings.MinPingDistance || PingTotals.Y >= MovementSettings.MinPingRotationDegrees
			|| PingTotals.Z >= MovementSettings.MinPingFOVDegrees || PingTotals.W >= MovementSettings.MinPingCameraDistance))
	{
		EmitMovement(TEXT("ping"), PingTotals.X, NowSeconds - LastPingSeconds, PingTotals.Y, PingTotals.Z, PingTotals.W);
		PingTotals = FVector4d::Zero();
		LastPingSeconds = NowSeconds;
	}
}

void FClient::EmitMovement(const TCHAR* Reason, double Distance, double Duration, double Rotation, double FOV, double CameraDistance)
{
	FProperties Properties;
	Properties.Set(TEXT("movement.reason"), Reason);
	if (Distance > 0.0)
		Properties.Set(TEXT("movement.distance"), Distance);
	if (Duration > 0.0)
		Properties.Set(TEXT("movement.duration"), Duration);
	if (Rotation > 0.0)
		Properties.Set(TEXT("movement.rotation_degrees"), Rotation);
	if (FOV > 0.0)
		Properties.Set(TEXT("movement.fov_delta"), FOV);
	if (CameraDistance > 0.0)
		Properties.Set(TEXT("movement.camera_distance"), CameraDistance);
	Track(TEXT("player_moved"), Properties);
}

uint64 FClient::BeginTeleport(const FString& Reason)
{
	if (State != EState::Ok && State != EState::NetworkDown)
	{
		return 0;
	}
	const uint64 Token = NextTeleportToken++;
	FTeleport Teleport{ Reason, ActivitySource ? ActivitySource().PlayerPosition : TOptional<FVector>(), Clock() };
	Teleports.Add(Token, MoveTemp(Teleport));
	ResetMovement();
	return Token;
}

void FClient::EndTeleport(uint64 Token)
{
	FTeleport Teleport;
	if (!Teleports.RemoveAndCopyValue(Token, Teleport))
	{
		return;
	}
	const TOptional<FVector> End = ActivitySource ? ActivitySource().PlayerPosition : TOptional<FVector>();
	if (Teleport.Start.IsSet() && End.IsSet())
	{
		RecordTeleport(Teleport.Start.GetValue(), End.GetValue(), Teleport.Reason, (Clock() - Teleport.StartedAt).GetTotalSeconds());
	}
	ResetMovement();
}

void FClient::CancelTeleport(uint64 Token)
{
	Teleports.Remove(Token);
	ResetMovement();
}

void FClient::RecordTeleport(const FVector& StartCentimeters, const FVector& EndCentimeters, const FString& Reason, double DurationSeconds)
{
	Track(TEXT("player_teleported"), FProperties().Set(TEXT("teleport.kind"), TEXT("teleport")).SetIfPresent(TEXT("teleport.reason"), Reason).SetVector(TEXT("teleport.start.position"), StartCentimeters).SetVector(TEXT("teleport.end.position"), EndCentimeters).Set(TEXT("teleport.duration"), FMath::Max(0.0, DurationSeconds)).Set(TEXT("teleport.distance"), FVector::Distance(StartCentimeters, EndCentimeters) / 100.0));
	ResetMovement();
}

uint64 FClient::TrackObject(const FString& Kind, FPositionSource PositionCentimeters, const FString& ID)
{
	check(IsInGameThread());
	if (!IsCollecting() || !PositionCentimeters)
	{
		return 0;
	}
	FString ObjectKind = Kind.TrimStartAndEnd();
	if (ObjectKind.IsEmpty())
	{
		ObjectKind = TEXT("object");
	}
	FString ObjectID = ID.TrimStartAndEnd();
	if (ObjectID.IsEmpty())
	{
		ObjectID = FString::Printf(TEXT("%s#%d"), *ObjectKind, ++ObjectCounters.FindOrAdd(ObjectKind));
	}
	if (const uint64* Existing = ObjectTokens.Find(ObjectID))
	{
		// Registering a live ID again replaces that object; its path continues.
		Objects.Remove(*Existing);
	}
	else if (Objects.Num() >= FMath::Max(0, Settings.MaxTrackedObjects))
	{
		if (!bWarnedObjectLimit)
		{
			bWarnedObjectLimit = true;
			UE_LOG(LogTestingFloor, Warning, TEXT("Testing Floor ignores new tracked objects: %d objects are already live."), Objects.Num());
		}
		return 0;
	}
	if (PendingRemovals.ContainsByPredicate([&ObjectID](const FObjectRows& Rows) { return Rows.ID == ObjectID; }))
	{
		// A reader must see the old path end before the new one starts.
		CheckObjects();
	}
	FTrackedObject Object{ ObjectID, ObjectKind, MoveTemp(PositionCentimeters) };
	Object.LastKnown = Private::FinitePosition(Object.Position());
	const uint64 Token = NextObjectToken++;
	ObjectTokens.Add(ObjectID, Token);
	Objects.Add(Token, MoveTemp(Object));
	return Token;
}

void FClient::SetObjectState(uint64 Token, const FString& Key, const FString& Value)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, Value));
}

void FClient::SetObjectState(uint64 Token, const FString& Key, const TCHAR* Value)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, Value));
}

void FClient::SetObjectState(uint64 Token, const FString& Key, int32 Value)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, Value));
}

void FClient::SetObjectState(uint64 Token, const FString& Key, int64 Value)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, Value));
}

void FClient::SetObjectState(uint64 Token, const FString& Key, float Value)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, Value));
}

void FClient::SetObjectState(uint64 Token, const FString& Key, double Value)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, Value));
}

void FClient::SetObjectState(uint64 Token, const FString& Key, bool bValue)
{
	SetObjectStateValue(Token, Key, FProperties().Set(Key, bValue));
}

void FClient::SetObjectStateValue(uint64 Token, const FString& Key, const FProperties& Value)
{
	FTrackedObject* Object = Objects.Find(Token);
	const TSharedPtr<FJsonValue> Scalar = Value.GetValues().FindRef(Key);
	if (!Object || !Scalar.IsValid() || Private::SameValue(Object->State.FindRef(Key), Scalar))
	{
		return;
	}
	Object->State.Add(Key, Scalar);
	Object->ChangedState.AddUnique(Key);
}

void FClient::RemoveObject(uint64 Token, const FString& Reason)
{
	FTrackedObject Object;
	if (!Objects.RemoveAndCopyValue(Token, Object))
	{
		return;
	}
	ObjectTokens.Remove(Object.ID);
	const TOptional<FVector> Final = Private::FinitePosition(Object.Position());
	FObjectRows Rows{ Object.ID, Object.Kind, Final.IsSet() ? Final : Object.LastKnown };
	if (!Rows.Sample.IsSet())
	{
		// An object that never had a position never appeared.
		return;
	}
	for (const FString& Key : Object.ChangedState)
	{
		Rows.State.Emplace(Key, Object.State.FindRef(Key));
	}
	Rows.RemovedReason = Reason.TrimStartAndEnd().IsEmpty() ? FString(TEXT("removed")) : Reason.TrimStartAndEnd();
	PendingRemovals.Add(MoveTemp(Rows));
}

int32 FClient::GetTrackedObjectCount() const
{
	return Objects.Num();
}

void FClient::CheckObjects()
{
	const bool bKeyframe = bObjectKeyframeDue && !Objects.IsEmpty();
	if (bObjectKeyframeDue)
	{
		bObjectKeyframeDue = false;
		ObjectKeyframeSeconds = CurrentSeconds;
	}
	TArray<uint64> Tokens;
	Objects.GetKeys(Tokens);
	Tokens.Sort();
	TArray<FObjectRows> Rows;
	for (const uint64 Token : Tokens)
	{
		FTrackedObject& Object = Objects[Token];
		const TOptional<FVector> Position = Private::FinitePosition(Object.Position());
		if (!Position.IsSet())
		{
			RemoveObject(Token, TEXT("gone"));
			continue;
		}
		Object.LastKnown = Position;
		FObjectRows Changes{ Object.ID, Object.Kind };
		if (bKeyframe || !Object.LastSent.IsSet()
			|| FVector::Distance(Position.GetValue(), Object.LastSent.GetValue()) >= Private::ObjectMinMoveCentimeters)
		{
			Changes.Sample = Position;
			Object.LastSent = Position;
		}
		if (bKeyframe)
		{
			for (const auto& Pair : Object.State)
			{
				Changes.State.Emplace(Pair.Key, Pair.Value);
			}
		}
		else
		{
			for (const FString& Key : Object.ChangedState)
			{
				Changes.State.Emplace(Key, Object.State.FindRef(Key));
			}
		}
		Object.ChangedState.Reset();
		if (Changes.Sample.IsSet() || !Changes.State.IsEmpty())
		{
			Rows.Add(MoveTemp(Changes));
		}
	}
	Rows.Append(MoveTemp(PendingRemovals));
	PendingRemovals.Reset();
	QueueObjectRecords(Rows, bKeyframe);
}

void FClient::QueueObjectRecords(TArray<FObjectRows>& Rows, bool bKeyframe)
{
	const FDateTime Timestamp = Clock();
	for (int32 Start = 0, End = 0; Start < Rows.Num(); Start = End)
	{
		// Keep each object's rows together so its state never arrives before its first sample.
		int32 Samples = 0;
		int32 Bytes = 0;
		for (End = Start; End < Rows.Num(); ++End)
		{
			const FObjectRows& Row = Rows[End];
			const int32 RowSamples = Row.Sample.IsSet() ? 1 : 0;
			const int32 RowBytes = Private::EstimateBytes(Row.ID, Row.Kind, Row.Sample.IsSet(), Row.State, Row.RemovedReason);
			if (End > Start && (Samples + RowSamples > Private::ObjectRecordMaxSamples || Bytes + RowBytes > Private::ObjectRecordByteBudget))
			{
				break;
			}
			Samples += RowSamples;
			Bytes += RowBytes;
		}
		TArray<TSharedPtr<FJsonValue>> SampleIDs, Kinds, X, Y, Z, RemovedIDs, Reasons, StateIDs, StateKeys, StateValues;
		for (int32 Index = Start; Index < End; ++Index)
		{
			const FObjectRows& Row = Rows[Index];
			if (Row.Sample.IsSet())
			{
				SampleIDs.Add(MakeShared<FJsonValueString>(Row.ID));
				Kinds.Add(MakeShared<FJsonValueString>(Row.Kind));
				X.Add(Private::Centimeters(Row.Sample->X));
				Y.Add(Private::Centimeters(Row.Sample->Y));
				Z.Add(Private::Centimeters(Row.Sample->Z));
			}
			for (const auto& Pair : Row.State)
			{
				StateIDs.Add(MakeShared<FJsonValueString>(Row.ID));
				StateKeys.Add(MakeShared<FJsonValueString>(Pair.Key));
				StateValues.Add(Pair.Value);
			}
			if (!Row.RemovedReason.IsEmpty())
			{
				RemovedIDs.Add(MakeShared<FJsonValueString>(Row.ID));
				Reasons.Add(MakeShared<FJsonValueString>(Row.RemovedReason));
			}
		}
		FEvent Record{ FGuid::NewGuid(), Private::ObjectsRecordName, Timestamp, SessionID, LevelContext };
		Record.bKeyframe = bKeyframe;
		const auto PutArray = [&Record](const TCHAR* Key, TArray<TSharedPtr<FJsonValue>>& Values) {
			if (!Values.IsEmpty())
			{
				Record.Properties.Put(Key, MakeShared<FJsonValueArray>(MoveTemp(Values)));
			}
		};
		PutArray(TEXT("sample.id"), SampleIDs);
		PutArray(TEXT("sample.kind"), Kinds);
		PutArray(TEXT("sample.x"), X);
		PutArray(TEXT("sample.y"), Y);
		PutArray(TEXT("sample.z"), Z);
		PutArray(TEXT("removed.id"), RemovedIDs);
		PutArray(TEXT("removed.reason"), Reasons);
		PutArray(TEXT("state.id"), StateIDs);
		PutArray(TEXT("state.key"), StateKeys);
		PutArray(TEXT("state.value"), StateValues);
		if (!Enqueue(MoveTemp(Record)))
		{
			// The keyframe that follows a drop repeats live objects, but not removals.
			for (int32 Index = Start; Index < End; ++Index)
			{
				if (!Rows[Index].RemovedReason.IsEmpty())
				{
					PendingRemovals.Add(Rows[Index]);
				}
			}
		}
	}
}

EState FClient::GetState() const
{
	return State;
}
const FString& FClient::GetPlaySessionID() const
{
	return SessionID;
}
const FString& FClient::GetDeviceID() const
{
	return Settings.DeviceID;
}
const FString& FClient::GetUserID() const
{
	return Settings.UserID;
}
const TOptional<FRecordingContext>& FClient::GetRecordingContext() const
{
	return Settings.Recording;
}
int32 FClient::GetPendingEventCount() const
{
	return Queue.Num() + InFlight.Num();
}
int32 FClient::GetDroppedEventCount() const
{
	return DroppedEvents;
}
const TArray<FEvent>& FClient::GetQueuedEvents() const
{
	return Queue;
}
} // namespace TestingFloor
