#pragma once

#include "Misc/AutomationTest.h"

#define TF_REQUIRE_TRUE(Condition, Description)                 \
	do                                                         \
	{                                                          \
		if (!TestTrue(TEXT(Description), static_cast<bool>(Condition))) \
		{                                                      \
			return false;                                      \
		}                                                      \
	} while (false)

#define TF_REQUIRE(Pointer, Description) TF_REQUIRE_TRUE((Pointer) != nullptr, Description)
