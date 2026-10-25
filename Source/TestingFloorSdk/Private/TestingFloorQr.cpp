#include "TestingFloor/TestingFloorQr.h"

#include "ThirdParty/QrCodeGenerator/qrcodegen.h"

namespace TestingFloor
{
namespace Private
{
constexpr int32 QrMaxVersion = 10;
constexpr int32 QrMaxSize = 4 * QrMaxVersion + 17;
constexpr int32 QrMaxTextBytes = 513; // V10/ECC-M numeric capacity, the most compact supported mode.
constexpr int32 QrMaxPixelDimension = 4096;

FString EscapeQrSession(const FString& SessionID)
{
	const FTCHARToUTF8 Utf8(*SessionID, SessionID.Len());
	FString Result;
	Result.Reserve(Utf8.Length());
	const TCHAR* Hex = TEXT("0123456789ABCDEF");
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		const uint8 Character = static_cast<uint8>(Utf8.Get()[Index]);
		if ((Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z')
			|| (Character >= '0' && Character <= '9') || Character == '-' || Character == '_' || Character == '.' || Character == '~')
		{
			Result.AppendChar(static_cast<TCHAR>(Character));
		}
		else
		{
			Result.AppendChar(TEXT('%'));
			Result.AppendChar(Hex[Character >> 4]);
			Result.AppendChar(Hex[Character & 15]);
		}
	}
	return Result;
}
} // namespace Private

bool HasForceQrArgument(const FString& CommandLine)
{
	static constexpr TCHAR ForceFlag[] = TEXT("--forceqr");
	constexpr int32 FlagLength = UE_ARRAY_COUNT(ForceFlag) - 1;
	static constexpr TCHAR NativeFlag[] = TEXT("-forceqr");
	constexpr int32 NativeFlagLength = UE_ARRAY_COUNT(NativeFlag) - 1;
	int32 Index = 0;
	while (Index < CommandLine.Len())
	{
		while (Index < CommandLine.Len() && FChar::IsWhitespace(CommandLine[Index]))
		{
			++Index;
		}
		TCHAR Quote = 0;
		int32 Length = 0;
		bool bMatches = true;
		bool bMatchesNative = true;
		const auto AppendCharacter = [&Length, &bMatches, &bMatchesNative](TCHAR Character) {
			bMatches = bMatches && Length < FlagLength && FChar::ToLower(Character) == ForceFlag[Length];
			bMatchesNative = bMatchesNative && Length < NativeFlagLength && FChar::ToLower(Character) == NativeFlag[Length];
			++Length;
		};
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
				const int32 Count = Index - Start;
				const bool bBeforeQuote = Index < CommandLine.Len() && CommandLine[Index] == TEXT('"');
				for (int32 Slash = 0; Slash < (bBeforeQuote ? Count / 2 : Count); ++Slash)
				{
					AppendCharacter(TEXT('\\'));
				}
				if (bBeforeQuote && Count % 2 != 0)
				{
					AppendCharacter(TEXT('"'));
					++Index;
				}
				continue;
			}
			if ((Character == TEXT('"') || Character == TEXT('\'')) && (!Quote || Quote == Character))
			{
				Quote = Quote ? 0 : Character;
			}
			else
			{
				AppendCharacter(Character);
			}
			++Index;
		}
		if (!Quote && ((bMatches && Length == FlagLength) || (bMatchesNative && Length == NativeFlagLength)))
		{
			return true;
		}
	}
	return false;
}

bool FQrPolicy::IsEnabled() const
{
	if (bAutomation)
	{
		return false;
	}
	if (bForceQR)
	{
		return true;
	}
	if (bSuppressQR && bEnabledWithRecorderSession)
	{
		return true;
	}
	return bEnabled && !bSuppressQR;
}

bool FQrMatrix::GetModule(int32 X, int32 Y) const
{
	return Size > 0 && Size <= Private::QrMaxSize && X >= 0 && X < Size && Y >= 0 && Y < Size
		&& Modules.Num() == Size * Size && Modules[Y * Size + X] != 0;
}

TArray<FColor> FQrMatrix::MakePixels(int32 ModuleScale, bool bInverted) const
{
	if (Size <= 0 || Size > Private::QrMaxSize || Modules.Num() != Size * Size)
	{
		return {};
	}
	const int32 Scale = FMath::Max(1, ModuleScale);
	const int32 TotalModules = Size + QuietZoneModules * 2;
	if (Scale > Private::QrMaxPixelDimension / TotalModules)
	{
		return {};
	}
	const int32 Dimension = TotalModules * Scale;
	const FColor Light = bInverted ? FColor::Black : FColor::White;
	const FColor Dark = bInverted ? FColor::White : FColor::Black;
	TArray<FColor> Pixels;
	Pixels.Init(Light, Dimension * Dimension);
	for (int32 Y = 0; Y < Size; ++Y)
	{
		for (int32 X = 0; X < Size; ++X)
		{
			if (!GetModule(X, Y))
			{
				continue;
			}
			const int32 PixelX = (X + QuietZoneModules) * Scale;
			const int32 PixelY = (Y + QuietZoneModules) * Scale;
			for (int32 OffsetY = 0; OffsetY < Scale; ++OffsetY)
			{
				for (int32 OffsetX = 0; OffsetX < Scale; ++OffsetX)
				{
					Pixels[(PixelY + OffsetY) * Dimension + PixelX + OffsetX] = Dark;
				}
			}
		}
	}
	return Pixels;
}

FString BuildQrSyncPayload(const FString& SessionID, int64 UnixMilliseconds, int64 Sequence)
{
	if (SessionID.TrimStartAndEnd().IsEmpty())
	{
		return {};
	}
	return FString::Printf(TEXT("tfqr://sync/v1?s=%s&t=%lld&q=%lld"), *Private::EscapeQrSession(SessionID), UnixMilliseconds, Sequence);
}

TOptional<FQrMatrix> EncodeQrPayload(const FString& Payload)
{
	if (Payload.Len() > Private::QrMaxTextBytes)
	{
		return {};
	}
	for (const TCHAR Character : Payload)
	{
		if (Character == 0)
		{
			return {};
		}
	}
	// encodeText takes a C string; the explicitly sized conversion does not append a terminator.
	const FTCHARToUTF8 Utf8(*Payload);
	if (Utf8.Length() > Private::QrMaxTextBytes)
	{
		return {};
	}
	uint8 Scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(Private::QrMaxVersion)];
	uint8 Encoded[qrcodegen_BUFFER_LEN_FOR_VERSION(Private::QrMaxVersion)];
	if (!qrcodegen_encodeText(Utf8.Get(), Scratch, Encoded, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
			Private::QrMaxVersion, qrcodegen_Mask_AUTO, false))
	{
		return {};
	}
	FQrMatrix Result;
	Result.Size = qrcodegen_getSize(Encoded);
	Result.Modules.SetNumUninitialized(Result.Size * Result.Size);
	for (int32 Y = 0; Y < Result.Size; ++Y)
	{
		for (int32 X = 0; X < Result.Size; ++X)
		{
			Result.Modules[Y * Result.Size + X] = qrcodegen_getModule(Encoded, X, Y) ? 1 : 0;
		}
	}
	return Result;
}

FQrHeartbeat::FQrHeartbeat(FString InSessionID) : SessionID(MoveTemp(InSessionID))
{
	bValidSessionID = !SessionID.TrimStartAndEnd().IsEmpty();
}

const FQrFrame& FQrHeartbeat::Tick(const FQrSettings& Settings, double NowSeconds, int64 UnixMilliseconds)
{
	if (!Settings.bEnabled || !bValidSessionID || !FMath::IsFinite(NowSeconds))
	{
		Frame.bVisible = false;
		bWasEnabled = false;
		return Frame;
	}
	if (!bWasEnabled || Settings.bInverted != bLastInverted || NowSeconds >= NextRefreshSeconds)
	{
		bWasEnabled = true;
		bLastInverted = Settings.bInverted;
		const double Interval = FMath::IsFinite(Settings.IntervalSeconds) ? FMath::Max(1.0, Settings.IntervalSeconds) : 15.0;
		NextRefreshSeconds = NowSeconds + Interval;
		const FString Payload = BuildQrSyncPayload(SessionID, UnixMilliseconds, Frame.Sequence + 1);
		auto Matrix = EncodeQrPayload(Payload);
		if (Matrix.IsSet())
		{
			Frame.Payload = Payload;
			Frame.Matrix = MoveTemp(Matrix.GetValue());
			++Frame.Sequence;
			const double Visible = FMath::IsFinite(Settings.VisibleSeconds) ? Settings.VisibleSeconds : 0.0;
			VisibleUntilSeconds = Visible <= 0.0 ? TNumericLimits<double>::Max() : NowSeconds + FMath::Min(Visible, Interval);
		}
		else
		{
			Frame.Payload.Reset();
			Frame.Matrix = {};
			VisibleUntilSeconds = NowSeconds;
		}
	}
	Frame.bVisible = Frame.Matrix.Size > 0 && NowSeconds < VisibleUntilSeconds;
	return Frame;
}
} // namespace TestingFloor
