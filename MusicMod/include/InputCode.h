#pragma once

enum class InputSource
{
	KBM,
	GAMEPAD
};

struct InputCode
{
	int code;
	InputSource source;

	bool operator==(const InputCode& other) const {
		return source == other.source && code == other.code;
	}
};