#pragma once

#include "CoreMinimal.h"

namespace TestingFloor
{
struct TESTINGFLOORSDK_API FQrPolicy
{
	bool bEnabled = false;
	bool bEnabledWithRecorderSession = false;
	bool bSuppressQR = false;
	bool bForceQR = false;
	bool bAutomation = false;
	bool IsEnabled() const;
};

struct FQrSettings
{
	bool bEnabled = false;
	bool bInverted = true;
	double IntervalSeconds = 15.0;
	double VisibleSeconds = 0.0;
};

struct TESTINGFLOORSDK_API FQrMatrix
{
	static constexpr int32 QuietZoneModules = 4;
	int32 Size = 0;
	/** Row-major modules from the upper left, excluding the quiet zone. */
	TArray<uint8> Modules;
	bool GetModule(int32 X, int32 Y) const;
	/** Row-major upper-left pixels, with the four-module quiet zone and opaque alpha. */
	TArray<FColor> MakePixels(int32 ModuleScale, bool bInverted) const;
};

struct FQrFrame
{
	FString Payload;
	FQrMatrix Matrix;
	int64 Sequence = 0;
	bool bVisible = false;
};

/** Matches exact --forceqr or native -forceqr arguments, including quoted launch arguments. */
TESTINGFLOORSDK_API bool HasForceQrArgument(const FString& CommandLine);

TESTINGFLOORSDK_API FString BuildQrSyncPayload(const FString& SessionID, int64 UnixMilliseconds, int64 Sequence);
/** ECC-M, versions 1 through 10, with the same automatic masking policy as Unity. */
TESTINGFLOORSDK_API TOptional<FQrMatrix> EncodeQrPayload(const FString& Payload);

/** Pure heartbeat state. Supply monotonic real time so pausing gameplay does not pause recording sync. */
class TESTINGFLOORSDK_API FQrHeartbeat final
{
public:
	explicit FQrHeartbeat(FString SessionID);
	const FQrFrame& Tick(const FQrSettings& Settings, double NowSeconds, int64 UnixMilliseconds);

private:
	FString SessionID;
	FQrFrame Frame;
	double NextRefreshSeconds = 0.0;
	double VisibleUntilSeconds = 0.0;
	bool bValidSessionID = false;
	bool bWasEnabled = false;
	bool bLastInverted = true;
};
} // namespace TestingFloor
