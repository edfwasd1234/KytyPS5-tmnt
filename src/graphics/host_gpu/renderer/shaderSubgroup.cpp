#include "graphics/host_gpu/renderer/shaderSubgroup.h"

#include "graphics/shader/recompiler/SpirvEmitter.h"

#include <cstdlib>

namespace Libs::Graphics {

ShaderLaneMaskMode SelectGraphicsLaneMaskMode(const GraphicContext& context,
                                              uint32_t              guest_wave_size) {
	// Diagnostic override: KYTY_LANE_MASK=per|native forces the graphics lane-mask model so the
	// wave-uniform (NativeWave) and per-lane (PerInvocation) models can be compared on the same
	// frame.
	static const int override_mode = [] {
		const char* v = std::getenv("KYTY_LANE_MASK");
		if (v == nullptr) {
			return 0;
		}
		if (v[0] == 'p') {
			return 1;
		}
		if (v[0] == 'n') {
			return 2;
		}
		return 0;
	}();
	if (override_mode == 1) {
		return ShaderLaneMaskMode::PerInvocation;
	}
	if (override_mode == 2) {
		return ShaderLaneMaskMode::NativeWave;
	}
	// NativeWave models EXEC/VCC as a fixed guest_wave_size-wide mask, so it is only sound when the
	// host is guaranteed to run every graphics stage at exactly that width.
	// VkPhysicalDeviceVulkan11Properties::subgroupSize is merely the device's default/preferred
	// width: when minSubgroupSize != maxSubgroupSize the driver may pick either end per stage, and
	// graphics pipelines cannot pin it because requiredSubgroupSizeStages typically omits the
	// vertex stage. RDNA reports subgroupSize=64 but runs fragment shaders at wave32; the 64-lane
	// EXEC/ballot math then collapses predicated colour exports to one lane per wave, writing a
	// single pixel per 8x8 fragment tile. The per-invocation model is width-independent, so use it
	// whenever the width is not fixed.
	const bool width_is_fixed = context.subgroup_size == guest_wave_size &&
	                            context.min_subgroup_size == context.max_subgroup_size;
	return width_is_fixed ? ShaderLaneMaskMode::NativeWave : ShaderLaneMaskMode::PerInvocation;
}

ShaderSubgroupConfiguration ConfigureShaderSubgroup(const GraphicContext&                context,
                                                    VkShaderStageFlagBits                stage,
                                                    const ShaderRecompiler::IR::Program& program) {
	const auto guest_wave_size = program.wave_size;
	if (guest_wave_size != 32u && guest_wave_size != 64u) {
		return {};
	}
	if (stage != VK_SHADER_STAGE_COMPUTE_BIT) {
		const auto expected = SelectGraphicsLaneMaskMode(context, guest_wave_size);
		if (program.lane_mask_mode != expected || (context.subgroup_size != guest_wave_size &&
		                                           expected != ShaderLaneMaskMode::PerInvocation)) {
			if (context.subgroup_size_control_enabled &&
			    (context.required_subgroup_size_stages & stage) != 0 &&
			    guest_wave_size >= context.min_subgroup_size &&
			    guest_wave_size <= context.max_subgroup_size) {
				return {ShaderSubgroupMode::Controlled, guest_wave_size};
			}
			return {};
		}
		return {expected == ShaderLaneMaskMode::NativeWave
		            ? ShaderSubgroupMode::Natural
		            : ShaderSubgroupMode::PerInvocationGraphics,
		        0};
	}
	if (program.lane_mask_mode != ShaderLaneMaskMode::NativeWave) {
		return {};
	}
	if (context.subgroup_size == guest_wave_size) {
		return {ShaderSubgroupMode::Natural, 0};
	}
	if (context.subgroup_size_control_enabled &&
	    (context.required_subgroup_size_stages & stage) != 0 &&
	    guest_wave_size >= context.min_subgroup_size &&
	    guest_wave_size <= context.max_subgroup_size) {
		return {ShaderSubgroupMode::Controlled, guest_wave_size};
	}
	if (!ShaderRecompiler::Spirv::ProgramRequiresExactSubgroupSize(program)) {
		return {ShaderSubgroupMode::FlattenedMasks, 0};
	}
	return {};
}

} // namespace Libs::Graphics
