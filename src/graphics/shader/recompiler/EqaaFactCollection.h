#pragma once

#include <cstdlib>
#include <cstring>

namespace Libs::Graphics::ShaderRecompiler::Diagnostics {

// Process-frozen modes. Admission facts are independent of disk shader dumps.
inline bool EqaaReduced2xRequested() {
	static const bool requested = [] {
		const auto* value = std::getenv("KYTY_EXPERIMENTAL_EQAA_2X");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

inline bool EqaaShaderCaptureEnabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_CAPTURE_EQAA_SHADER_STATE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

inline bool RetainEqaaRawRegisters(bool graphics_debug_dump) {
	return graphics_debug_dump || EqaaReduced2xRequested() || EqaaShaderCaptureEnabled();
}

inline bool CollectEqaaPixelFacts() {
	return EqaaReduced2xRequested() || EqaaShaderCaptureEnabled();
}

inline bool PrintEqaaFullState(bool graphics_debug_dump) {
	return graphics_debug_dump || EqaaShaderCaptureEnabled();
}

} // namespace Libs::Graphics::ShaderRecompiler::Diagnostics
