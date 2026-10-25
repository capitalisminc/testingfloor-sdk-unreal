#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "TestingFloorTestHelpers.h"
#include "TestingFloor/TestingFloorClient.h"
#include "TestingFloor/TestingFloorQr.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorQrPolicyTest, "TestingFloor.Sdk.QrPolicy", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorQrPolicyTest::RunTest(const FString& Parameters)
{
	const TArray<FString> ForceArguments{
		TEXT("--forceqr"),
		TEXT("-forceqr"),
		TEXT("-windowed --FoRcEqR -log"),
		TEXT(R"TF("--forceqr")TF"),
		TEXT(R"TF('--forceqr')TF"),
		TEXT(R"TF("" --forceqr)TF"),
		TEXT(R"TF(-other="escaped \\" --forceqr)TF")
	};
	for (const FString& CommandLine : ForceArguments)
	{
		TestTrue(TEXT("Exact force flag survives supported quoting and case"), TestingFloor::HasForceQrArgument(CommandLine));
	}
	const TArray<FString> OtherArguments{
		TEXT(R"TF(-other="text --forceqr")TF"),
		TEXT(R"TF(-other="text -forceqr")TF"),
		TEXT(R"TF(-other='text --forceqr')TF"),
		TEXT("-other=--forceqr"),
		TEXT("--forceqr=false"),
		TEXT("prefix--forceqr"),
		TEXT("--forceqrsuffix"),
		TEXT(R"TF("foo"--forceqr)TF"),
		TEXT(R"TF("--forceqr"suffix)TF"),
		TEXT(R"TF(--forceqr")TF"),
		TEXT(R"TF(-other="escaped \\\" --forceqr")TF")
	};
	for (const FString& CommandLine : OtherArguments)
	{
		TestFalse(TEXT("Embedded, extended and unbalanced arguments cannot force QR"), TestingFloor::HasForceQrArgument(CommandLine));
	}
	TestingFloor::FQrPolicy Policy;
	TestFalse(TEXT("QR is opt-in by default"), Policy.IsEnabled());
	Policy.bEnabled = true;
	TestTrue(TEXT("Configured QR appears without recorder suppression"), Policy.IsEnabled());
	Policy.bSuppressQR = true;
	TestFalse(TEXT("Recorder may suppress ordinary QR"), Policy.IsEnabled());
	Policy.bEnabledWithRecorderSession = true;
	TestTrue(TEXT("Recorder-session QR preference overrides suppression"), Policy.IsEnabled());
	Policy.bEnabled = false;
	TestTrue(TEXT("Recorder-session preference also works independently of ordinary QR"), Policy.IsEnabled());
	Policy.bSuppressQR = false;
	TestFalse(TEXT("Recorder-session preference alone does not enable ordinary play"), Policy.IsEnabled());
	Policy.bEnabledWithRecorderSession = false;
	Policy.bSuppressQR = true;
	Policy.bForceQR = true;
	TestTrue(TEXT("Force QR overrides recorder suppression and settings"), Policy.IsEnabled());
	Policy.bAutomation = true;
	TestFalse(TEXT("Automation never displays a QR even when forced"), Policy.IsEnabled());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorQrPayloadTest, "TestingFloor.Sdk.QrSyncPayload", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorQrPayloadTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Recorder URI preserves documented session, wall-clock and sequence fields"),
		TestingFloor::BuildQrSyncPayload(TEXT("example-session"), 1711234567890LL, 42),
		FString(TEXT("tfqr://sync/v1?s=example-session&t=1711234567890&q=42")));
	TestEqual(TEXT("Session values use UTF-8 URI escaping, retaining only unreserved characters"),
		TestingFloor::BuildQrSyncPayload(TEXT("session /?&=+%~._-\u00e9"), 123, 1),
		FString(TEXT("tfqr://sync/v1?s=session%20%2F%3F%26%3D%2B%25~._-%C3%A9&t=123&q=1")));
	TestTrue(TEXT("Missing play session cannot create a sync association"), TestingFloor::BuildQrSyncPayload(TEXT(" \t"), 123, 1).IsEmpty());
	const FString Payload = TestingFloor::BuildQrSyncPayload(TEXT("00000000-0000-4000-8000-000000000001"), 1711234567890LL, 42);
	const auto Matrix = TestingFloor::EncodeQrPayload(Payload);
	TF_REQUIRE_TRUE(Matrix.IsSet(), "Real recorder URI encodes");
	TestTrue(TEXT("Recorder URI fits the same small overlay QR versions as Unity"), Matrix->Size <= 37);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorQrMatrixTest, "TestingFloor.Sdk.QrMatrixAndPixels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorQrMatrixTest::RunTest(const FString& Parameters)
{
	const auto Matrix = TestingFloor::EncodeQrPayload(TEXT("hello"));
	TF_REQUIRE_TRUE(Matrix.IsSet(), "Known lowercase byte-mode payload encodes");
	TestEqual(TEXT("Short payload uses version one"), Matrix->Size, 21);
	// Nayuki v1.8.0 reference output for byte-mode hello, ECC-M, automatic mask and no ECC boost.
	const TCHAR* const ExpectedRows[]{
		TEXT("111111100110001111111"),
		TEXT("100000101100001000001"),
		TEXT("101110100101101011101"),
		TEXT("101110100011001011101"),
		TEXT("101110101100101011101"),
		TEXT("100000100000101000001"),
		TEXT("111111101010101111111"),
		TEXT("000000000011100000000"),
		TEXT("101010100101000010010"),
		TEXT("001011000010001000011"),
		TEXT("010100101110100011111"),
		TEXT("110010000000001000010"),
		TEXT("011010110010101010000"),
		TEXT("000000001111010100111"),
		TEXT("111111100011011100111"),
		TEXT("100000100011110110000"),
		TEXT("101110101011011100011"),
		TEXT("101110100100001100110"),
		TEXT("101110101110100010101"),
		TEXT("100000100100001010010"),
		TEXT("111111101110101100011")
	};
	for (int32 Y = 0; Y < 21; ++Y)
	{
		for (int32 X = 0; X < 21; ++X)
		{
			TestEqual(TEXT("Fixed reference QR module"), Matrix->GetModule(X, Y), ExpectedRows[Y][X] == TEXT('1'));
		}
	}
	TestFalse(TEXT("Out-of-bounds modules are light"), Matrix->GetModule(-1, 0));
	TestFalse(TEXT("Past-end modules are light"), Matrix->GetModule(0, Matrix->Size));
	const int32 Scale = 2;
	const int32 Quiet = 4 * Scale;
	const int32 Dimension = (Matrix->Size + 8) * Scale;
	const auto Normal = Matrix->MakePixels(Scale, false);
	const auto Inverted = Matrix->MakePixels(Scale, true);
	TF_REQUIRE_TRUE(Normal.Num() == Dimension * Dimension && Inverted.Num() == Normal.Num(), "Rendered pixels include quiet zone and module scale");
	for (int32 Y = 0; Y < Dimension; ++Y)
	{
		for (int32 X = 0; X < Dimension; ++X)
		{
			if (X < Quiet || Y < Quiet || X >= Dimension - Quiet || Y >= Dimension - Quiet)
			{
				TestEqual(TEXT("Normal quiet zone is white"), Normal[Y * Dimension + X], FColor::White);
				TestEqual(TEXT("Inverted quiet zone is black"), Inverted[Y * Dimension + X], FColor::Black);
			}
			TestEqual(TEXT("Inversion swaps every opaque pixel"), Inverted[Y * Dimension + X], Normal[Y * Dimension + X] == FColor::Black ? FColor::White : FColor::Black);
		}
	}
	TestEqual(TEXT("Upper-left finder module is placed below the quiet zone"), Normal[Quiet * Dimension + Quiet], FColor::Black);
	TestEqual(TEXT("Nonpositive scale clamps to one pixel per module"), Matrix->MakePixels(0, false).Num(), 29 * 29);
	TestTrue(TEXT("Unbounded raster requests are rejected"), Matrix->MakePixels(MAX_int32, false).IsEmpty());
	TestingFloor::FQrMatrix Invalid;
	Invalid.Size = 21;
	TestFalse(TEXT("Incomplete matrices cannot be sampled"), Invalid.GetModule(0, 0));
	TestTrue(TEXT("Incomplete matrices cannot be rasterized"), Invalid.MakePixels(1, false).IsEmpty());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorQrCapacityTest, "TestingFloor.Sdk.QrCapacity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorQrCapacityTest::RunTest(const FString& Parameters)
{
	const auto MaximumBytes = TestingFloor::EncodeQrPayload(FString::ChrN(213, TEXT('a')));
	TF_REQUIRE_TRUE(MaximumBytes.IsSet(), "Maximum V10 ECC-M byte-mode payload fits");
	TestEqual(TEXT("Byte-mode capacity uses version ten"), MaximumBytes->Size, 57);
	TestFalse(TEXT("One excess byte returns failure without a vendor assertion"), TestingFloor::EncodeQrPayload(FString::ChrN(214, TEXT('a'))).IsSet());
	TestTrue(TEXT("Compact numeric mode remains available through its V10 capacity"), TestingFloor::EncodeQrPayload(FString::ChrN(513, TEXT('1'))).IsSet());
	TestFalse(TEXT("Input beyond the largest supported encoding is rejected early"), TestingFloor::EncodeQrPayload(FString::ChrN(514, TEXT('1'))).IsSet());
	TestFalse(TEXT("Unicode capacity is measured in UTF-8 bytes"), TestingFloor::EncodeQrPayload(FString::ChrN(107, TEXT('\u00e9'))).IsSet());
	FString EmbeddedNull = TEXT("before_after");
	EmbeddedNull[6] = 0;
	TF_REQUIRE_TRUE(EmbeddedNull.Len() == 12 && EmbeddedNull[6] == 0 && EmbeddedNull[7] == TEXT('a'), "Fixture retains stored characters after an embedded null");
	TestFalse(TEXT("Embedded null cannot silently truncate a sync payload"), TestingFloor::EncodeQrPayload(EmbeddedNull).IsSet());
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FTestingFloorQrHeartbeatTest, "TestingFloor.Sdk.QrHeartbeatSchedule", EAutomationTestFlags::EditorContext | EAutomationTestFlags::CommandletContext | EAutomationTestFlags::ProductFilter)
bool FTestingFloorQrHeartbeatTest::RunTest(const FString& Parameters)
{
	TestingFloor::FQrSettings Settings;
	TestTrue(TEXT("Default QR style is inverted"), Settings.bInverted);
	TestEqual(TEXT("Default refresh is fifteen real seconds"), Settings.IntervalSeconds, 15.0);
	Settings.bEnabled = true;
	Settings.VisibleSeconds = 2.0;
	TestingFloor::FQrHeartbeat Heartbeat(TEXT("play-session"));
	const auto& First = Heartbeat.Tick(Settings, 10.0, 1000);
	TestEqual(TEXT("First visible heartbeat starts at sequence one"), First.Sequence, static_cast<int64>(1));
	TestTrue(TEXT("First heartbeat appears immediately"), First.bVisible);
	const uint8* const Modules = First.Matrix.Modules.GetData();
	const TCHAR* const Payload = *First.Payload;
	const auto& Cached = Heartbeat.Tick(Settings, 11.0, 2000);
	TestEqual(TEXT("Frames between deadlines retain the payload sequence"), Cached.Sequence, static_cast<int64>(1));
	TestTrue(TEXT("Cached frame reuses its matrix allocation"), Cached.Matrix.Modules.GetData() == Modules);
	TestTrue(TEXT("Cached frame reuses its payload allocation"), *Cached.Payload == Payload);
	TestFalse(TEXT("Finite visibility expires at its boundary"), Heartbeat.Tick(Settings, 12.0, 3000).bVisible);
	TestEqual(TEXT("Hidden heartbeat waits for its scheduled deadline"), Heartbeat.Tick(Settings, 24.99, 4000).Sequence, static_cast<int64>(1));
	const auto& Refreshed = Heartbeat.Tick(Settings, 25.0, 5000);
	TestEqual(TEXT("Real time advances QR during paused gameplay"), Refreshed.Sequence, static_cast<int64>(2));
	TestEqual(TEXT("Refresh carries the current system clock"), Refreshed.Payload, FString(TEXT("tfqr://sync/v1?s=play-session&t=5000&q=2")));
	TestTrue(TEXT("Scheduled refresh opens another visibility window"), Refreshed.bVisible);
	Settings.bInverted = false;
	TestEqual(TEXT("Polarity change refreshes immediately"), Heartbeat.Tick(Settings, 26.0, 6000).Sequence, static_cast<int64>(3));
	Settings.bEnabled = false;
	TestFalse(TEXT("Disabled setting hides the current frame"), Heartbeat.Tick(Settings, 27.0, 7000).bVisible);
	Settings.bEnabled = true;
	TestEqual(TEXT("Re-enabling refreshes without waiting for the previous deadline"), Heartbeat.Tick(Settings, 28.0, 8000).Sequence, static_cast<int64>(4));
	Settings.IntervalSeconds = 0.0;
	Settings.VisibleSeconds = 10.0;
	TestingFloor::FQrHeartbeat MinimumInterval(TEXT("play-session"));
	MinimumInterval.Tick(Settings, 0.0, 0);
	TestEqual(TEXT("Interval clamps to at least one real second"), MinimumInterval.Tick(Settings, 0.99, 990).Sequence, static_cast<int64>(1));
	TestEqual(TEXT("Minimum interval refreshes at one second"), MinimumInterval.Tick(Settings, 1.0, 1000).Sequence, static_cast<int64>(2));
	Settings.IntervalSeconds = std::numeric_limits<double>::quiet_NaN();
	Settings.VisibleSeconds = 0.0;
	TestingFloor::FQrHeartbeat Continuous(TEXT("play-session"));
	Continuous.Tick(Settings, 0.0, 0);
	TestTrue(TEXT("Zero visibility setting displays continuously"), Continuous.Tick(Settings, 14.99, 14990).bVisible);
	TestEqual(TEXT("Nonfinite interval uses the fifteen-second default"), Continuous.Tick(Settings, 15.0, 15000).Sequence, static_cast<int64>(2));
	TestingFloor::FQrHeartbeat MissingSession(TEXT(" \t"));
	TestFalse(TEXT("Blank session never shows an unassociated QR"), MissingSession.Tick(Settings, 0.0, 0).bVisible);
	TestingFloor::FQrHeartbeat OversizedSession(FString::ChrN(1000, TEXT('s')));
	const auto& Failed = OversizedSession.Tick(Settings, 0.0, 0);
	TestFalse(TEXT("Unencodable session hides the QR"), Failed.bVisible);
	TestEqual(TEXT("Failed encoding does not advance heartbeat sequence"), Failed.Sequence, static_cast<int64>(0));
	return true;
}

#endif
