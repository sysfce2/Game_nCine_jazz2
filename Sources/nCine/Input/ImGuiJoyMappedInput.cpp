#if defined(WITH_IMGUI)

#include "IInputManager.h"
#include "../Application.h"

#include <imgui.h>

namespace nCine
{
	bool imGuiJoyMappedInput()
	{
		ImGuiIO& io = ImGui::GetIO();

		if (theApplication().GetInputManager().isJoyMapped(0)) {
			const JoyMappedState& state = theApplication().GetInputManager().joyMappedState(0);

			// clang-format off
			#define IM_SATURATE(V)                      (V < 0.0f ? 0.0f : V > 1.0f ? 1.0f : V)
			#define MAP_BUTTON(KEY_NO, BUTTON_NO)       { io.AddKeyEvent(KEY_NO, state.isButtonPressed(BUTTON_NO)); }
			#define MAP_ANALOG(KEY_NO, AXIS_NO, V0, V1) { float vn = static_cast<float>(state.axisValue(AXIS_NO) - V0) / static_cast<float>(V1 - V0);\
		                                                    vn = IM_SATURATE(vn); io.AddKeyAnalogEvent(KEY_NO, vn > 0.1f, vn); }

			constexpr int thumbDeadZone = 8000; // SDL_gamecontroller.h suggests using this value.
			MAP_BUTTON(ImGuiKey_GamepadStart, ButtonName::Start);
			MAP_BUTTON(ImGuiKey_GamepadBack, ButtonName::Back);
			MAP_BUTTON(ImGuiKey_GamepadFaceDown, ButtonName::A);		// Xbox A, PS Cross
			MAP_BUTTON(ImGuiKey_GamepadFaceRight, ButtonName::B);		// Xbox B, PS Circle
			MAP_BUTTON(ImGuiKey_GamepadFaceLeft, ButtonName::X);		// Xbox X, PS Square
			MAP_BUTTON(ImGuiKey_GamepadFaceUp, ButtonName::Y);			// Xbox Y, PS Triangle
			MAP_BUTTON(ImGuiKey_GamepadDpadLeft, ButtonName::Left);
			MAP_BUTTON(ImGuiKey_GamepadDpadRight, ButtonName::Right);
			MAP_BUTTON(ImGuiKey_GamepadDpadUp, ButtonName::Up);
			MAP_BUTTON(ImGuiKey_GamepadDpadDown, ButtonName::Down);
			MAP_BUTTON(ImGuiKey_GamepadL1, ButtonName::LeftBumper);
			MAP_BUTTON(ImGuiKey_GamepadR1, ButtonName::RightBumper);
			MAP_ANALOG(ImGuiKey_GamepadL2, AxisName::LeftTrigger, 0.0f, 32767);
			MAP_ANALOG(ImGuiKey_GamepadR2, AxisName::RightTrigger, 0.0f, 32767);
			MAP_BUTTON(ImGuiKey_GamepadL3, ButtonName::LeftStick);
			MAP_BUTTON(ImGuiKey_GamepadR3, ButtonName::RightStick);
			MAP_ANALOG(ImGuiKey_GamepadLStickLeft, AxisName::LeftX, -thumbDeadZone, -32768);
			MAP_ANALOG(ImGuiKey_GamepadLStickRight, AxisName::LeftX, +thumbDeadZone, +32767);
			MAP_ANALOG(ImGuiKey_GamepadLStickUp, AxisName::LeftY, -thumbDeadZone, -32768);
			MAP_ANALOG(ImGuiKey_GamepadLStickDown, AxisName::LeftY, +thumbDeadZone, +32767);
			MAP_ANALOG(ImGuiKey_GamepadRStickLeft, AxisName::RightX, -thumbDeadZone, -32768);
			MAP_ANALOG(ImGuiKey_GamepadRStickRight, AxisName::RightX, +thumbDeadZone, +32767);
			MAP_ANALOG(ImGuiKey_GamepadRStickUp, AxisName::RightY, -thumbDeadZone, -32768);
			MAP_ANALOG(ImGuiKey_GamepadRStickDown, AxisName::RightY, +thumbDeadZone, +32767);
			#undef MAP_BUTTON
			#undef MAP_ANALOG
			// clang-format on

			io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
			return true;
		}

		io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		return false;
	}
}

#endif