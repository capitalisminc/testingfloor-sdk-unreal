#include "TestingFloor/TestingFloorClient.h"

#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace TestingFloor
{
namespace Private
{
constexpr int32 EventByteCap = 32 << 10;
constexpr int32 BodyByteCap = (1 << 20) - (8 << 10);

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
	return Set(Key, FString(Value ? Value : TEXT("")));
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

TOptional<FRecordingContext> FRecordingContext::Parse(const FString& Json, FDateTime Now)
{
	TSharedPtr<FJsonObject> Object;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object) || !Object.IsValid())
	{
		return {};
	}
	FRecordingContext Result;
	Object->TryGetStringField(TEXT("session_id"), Result.SessionID);
	Object->TryGetStringField(TEXT("recording_uuid"), Result.RecordingUUID);
	Result.SessionID.TrimStartAndEndInline();
	Result.RecordingUUID.TrimStartAndEndInline();
	if (Result.RecordingUUID.IsEmpty())
	{
		Result.RecordingUUID = Result.SessionID;
	}
	if (Result.RecordingUUID.IsEmpty() && Result.SessionID.IsEmpty())
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
	Object->SetNumberField(TEXT("$schema"), 1);
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
	Platform->SetStringField(TEXT("sdk_version"), TEXT("unreal-0.1.0"));
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
	if (Settings.Recording.IsSet())
	{
		const FRecordingContext& Recording = Settings.Recording.GetValue();
		if (!Recording.SessionID.IsEmpty())
		{
			Platform->SetStringField(TEXT("session_id"), Recording.SessionID);
		}
		Platform->SetStringField(TEXT("recording_uuid"), Recording.RecordingUUID);
		if (Recording.PlaytestID.IsSet())
		{
			Platform->SetField(TEXT("playtest_id"), MakeShared<FJsonValueNumberString>(LexToString(Recording.PlaytestID.GetValue())));
		}
	}
	Object->SetObjectField(TEXT("$tf"), Platform);
	return Private::Encode(Object);
}

FString BuildBatch(TConstArrayView<FEvent> Events, const FSettings& Settings, FDateTime SentAt, int32& OutConsumed)
{
	FString Body;
	const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Body);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("$write_key"), Settings.WriteKey);
	Writer->WriteValue(TEXT("$sent_at"), SentAt.ToIso8601());
	Writer->WriteArrayStart(TEXT("events"));
	int32 BodyBytes = Private::Utf8Length(Body) + 2;
	int32 Written = 0;
	OutConsumed = 0;
	for (const FEvent& Event : Events)
	{
		if (Written >= FMath::Clamp(Settings.BatchMaxEvents, 1, 1000))
		{
			break;
		}
		const FString EventJson = SerializeEvent(Event, Settings);
		const int32 EventBytes = Private::Utf8Length(EventJson);
		if (EventBytes > Private::EventByteCap)
		{
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
	return Written > 0 ? Body : FString();
}

bool FActivitySample::IsEmpty() const
{
	return !PlayerPosition.IsSet() && !CameraPosition.IsSet() && !CameraRotation.IsSet() && !CameraFOV.IsSet();
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
}

void FClient::SetContextProvider(FContextProvider Provider)
{
	ContextProvider = MoveTemp(Provider);
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
		Properties.Set(TEXT("camera.fov"), Sample.CameraFOV.GetValue()).Set(TEXT("camera.fov_axis"), TEXT("horizontal"));
	}
	if (Sample.ViewportSize.X > 0 && Sample.ViewportSize.Y > 0)
	{
		Properties.Set(TEXT("viewport.width"), Sample.ViewportSize.X).Set(TEXT("viewport.height"), Sample.ViewportSize.Y);
	}
}

bool FClient::Track(const FString& Name, const FProperties& Properties)
{
	check(IsInGameThread());
	if ((State != EState::Ok && State != EState::NetworkDown) || Name.TrimStartAndEnd().IsEmpty() || bSessionEnded)
	{
		return false;
	}
	if (GetPendingEventCount() >= FMath::Max(1, Settings.QueueMaxEvents))
	{
		++DroppedEvents;
		return false;
	}
	FEvent Event{ FGuid::NewGuid(), Name, Clock(), SessionID, {} };
	if (ContextProvider)
	{
		ContextProvider(Event.Properties);
	}
	if (ActivitySource)
	{
		const FActivitySample Sample = ActivitySource();
		FillActivityContext(Event.Properties, Sample);
		Event.ScreenSize = Sample.ViewportSize;
	}
	Event.Properties.Merge(Properties);
	if (EventProcessor)
	{
		EventProcessor(Event.Properties);
	}
	if (Private::Utf8Length(SerializeEvent(Event, Settings)) > Private::EventByteCap)
	{
		++DroppedEvents;
		return false;
	}
	Queue.Add(MoveTemp(Event));
	return true;
}

void FClient::StartSession()
{
	if (!bSessionStarted)
	{
		bSessionStarted = true;
		Track(TEXT("session_start"));
	}
}

void FClient::EndSession()
{
	if (!bSessionEnded)
	{
		Track(TEXT("session_end"));
		bSessionEnded = true;
		Flush();
	}
}

void FClient::Tick(double NowSeconds)
{
	check(IsInGameThread());
	CurrentSeconds = NowSeconds;
	if (Settings.bMovementTracking && !bSessionEnded && ActivitySource && (State == EState::Ok || State == EState::NetworkDown))
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
	if (!Queue.IsEmpty() && !bRequestInFlight && NowSeconds >= NextRetrySeconds
		&& (bFlushRequested || Queue.Num() >= FMath::Max(1, Settings.BatchMaxEvents)
			|| NowSeconds - LastFlushSeconds >= FMath::Max(0.0, Settings.FlushIntervalSeconds)))
	{
		Flush();
	}
}

void FClient::Flush()
{
	bFlushRequested = true;
	if (Queue.IsEmpty() || bRequestInFlight || CurrentSeconds < NextRetrySeconds || (State != EState::Ok && State != EState::NetworkDown))
	{
		return;
	}
	int32 Consumed = 0;
	const FString Body = BuildBatch(Queue, Settings, Clock(), Consumed);
	if (Consumed == 0)
	{
		return;
	}
	InFlight.Append(Queue.GetData(), Consumed);
	Queue.RemoveAt(0, Consumed, EAllowShrinking::No);
	if (Body.IsEmpty())
	{
		DroppedEvents += InFlight.Num();
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
		InFlight.Reset();
		Queue.Reset();
	}
	else if (ResponseCode >= 400 && ResponseCode < 500 && ResponseCode != 408 && ResponseCode != 429)
	{
		DroppedEvents += InFlight.Num();
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
	EndSession();
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
	Track(TEXT("player_teleported"), FProperties().SetIfPresent(TEXT("teleport.reason"), Reason).SetVector(TEXT("teleport.start.position"), StartCentimeters).SetVector(TEXT("teleport.end.position"), EndCentimeters).Set(TEXT("teleport.duration"), FMath::Max(0.0, DurationSeconds)).Set(TEXT("teleport.distance"), FVector::Distance(StartCentimeters, EndCentimeters) / 100.0));
	ResetMovement();
}

EState FClient::GetState() const
{
	return State;
}
const FString& FClient::GetSessionID() const
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
