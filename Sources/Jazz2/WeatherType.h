﻿#pragma once

#include "../Main.h"

namespace Jazz2
{
	/** @brief Weather type, supports a bitwise combination of its member values */
	enum class WeatherType : std::uint8_t
	{
		None,

		Snow,
		Flowers,
		Rain,
		Leaf,

		OutdoorsOnly = 0x80
	};

	DEATH_ENUM_FLAGS(WeatherType);
}