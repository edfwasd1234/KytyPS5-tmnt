#include "common/assert.h"
#include "common/common.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "graphics/guest_gpu/gpu_defs.h"
#include "graphics/guest_gpu/hardwareContext.h"
#include "graphics/guest_gpu/tile.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/objects/textureCommon.h"
#include "graphics/host_gpu/renderer/descriptorCache.h"
#include "graphics/host_gpu/renderer/framebufferCache.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/renderer/renderContext.h"
#include "graphics/host_gpu/renderer/renderState.h"
#include "graphics/host_gpu/utils.h"
#include "graphics/presentation/displayBuffer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <vulkan/vk_enum_string_helper.h>

namespace Libs::Graphics {

[[noreturn]] static void DepthFatal(const char* format, ...) {
	std::fputs("Depth target fatal: ", stderr);
	va_list args;
	va_start(args, format);
	std::vfprintf(stderr, format, args);
	va_end(args);
	std::fputc('\n', stderr);
	std::fflush(stderr);
	EXIT("unsupported render state; details were printed above\n");
}

static VkStencilOp ConvertStencilOp(uint8_t value) {
	switch (static_cast<Prospero::StencilOp>(value)) {
		case Prospero::StencilOp::kKeep: return VK_STENCIL_OP_KEEP;
		case Prospero::StencilOp::kZero: return VK_STENCIL_OP_ZERO;
		case Prospero::StencilOp::kReplaceTest:
		case Prospero::StencilOp::kReplaceOp: return VK_STENCIL_OP_REPLACE;
		case Prospero::StencilOp::kAddClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
		case Prospero::StencilOp::kSubClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
		case Prospero::StencilOp::kInvert: return VK_STENCIL_OP_INVERT;
		case Prospero::StencilOp::kAddWrap: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
		case Prospero::StencilOp::kSubWrap: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
		default: DepthFatal("unsupported stencil operation");
	}
}

static bool UsesStencilOpValue(uint8_t fail, uint8_t pass, uint8_t depth_fail) {
	return fail == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp) ||
	       pass == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp) ||
	       depth_fail == Prospero::GpuEnumValue(Prospero::StencilOp::kReplaceOp);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void ResolveRenderDepthTarget(uint64_t submit_id, CommandBuffer* buffer, const HW::Context& hw,
                              RenderDepthInfo* r) {
	KYTY_PROFILER_FUNCTION();
	EXIT_IF(r == nullptr);
	(void)submit_id;
	(void)buffer;
	const auto& z  = hw.GetDepthRenderTarget();
	const auto& rc = hw.GetRenderControl();
	const auto& dc = hw.GetDepthControl();
	const auto& sc = hw.GetStencilControl();
	const auto& sm = hw.GetStencilMask();
	const bool  depth_active =
	    dc.z_enable || dc.z_write_enable || dc.depth_bounds_enable || rc.depth_clear_enable;
	const bool stencil_active = dc.stencil_enable || rc.stencil_clear_enable;
	if (!depth_active && !stencil_active) {
		return;
	}
	const bool attachment_unbound =
	    z.z_info.format == Prospero::GpuEnumValue(Prospero::DepthFormat::kInvalid) &&
	    z.stencil_info.format == Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid) &&
	    z.z_info.tile_mode_index == 0 && z.z_info.num_samples == 0 &&
	    z.z_info.zrange_precision <= 1 && !z.z_info.expclear_enabled &&
	    !z.z_info.embedded_sample_locations && !z.z_info.partially_resident &&
	    z.z_info.num_mip_levels == 0 && z.z_info.plane_compression == 0 &&
	    z.stencil_info.tile_mode_index == 0 && z.stencil_info.tile_split == 0 &&
	    !z.stencil_info.expclear_enabled && !z.stencil_info.texture_compatible_stencil &&
	    !z.stencil_info.partially_resident && z.depth_view.slice_start == 0 &&
	    z.depth_view.slice_max == 0 && z.depth_view.current_mip_level == 0 &&
	    !z.depth_view.depth_write_disable && !z.depth_view.stencil_write_disable &&
	    z.depth_info.addr5_swizzle_mask == 0 && z.depth_info.array_mode == 0 &&
	    z.depth_info.pipe_config == 0 && z.depth_info.bank_width == 0 &&
	    z.depth_info.bank_height == 0 && z.depth_info.macro_tile_aspect == 0 &&
	    z.depth_info.num_banks == 0 && z.htile_surface.linear == 0 &&
	    z.htile_surface.full_cache == 0 && z.htile_surface.htile_uses_preload_win == 0 &&
	    z.htile_surface.preload == 0 && z.htile_surface.prefetch_width == 0 &&
	    z.htile_surface.prefetch_height == 0 && z.htile_surface.dst_outside_zero_to_one == 0 &&
	    z.z_read_base_addr == 0 && z.z_write_base_addr == 0 && z.stencil_read_base_addr == 0 &&
	    z.stencil_write_base_addr == 0 && z.htile_data_base_addr == 0 &&
	    !z.z_info.tile_surface_enable && !z.size.valid && !z.width_height_valid &&
	    !z.pitch_height_valid && z.size.x_max == 0 && z.size.y_max == 0 &&
	    z.pitch_div8_minus1 == 0 && z.height_div8_minus1 == 0 && z.slice_div64_minus1 == 0 &&
	    z.width == 0 && z.height == 0;
	if (attachment_unbound) {
		static std::atomic_bool logged = false;
		if (!logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: ignoring enabled depth/stencil state without a bound attachment\n");
		}
		return;
	}
	const bool has_stencil =
	    z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::kInvalid);
	const bool has_htile            = z.z_info.tile_surface_enable;
	const bool msaa_compat          = depth_msaa_single_sample_compatible(z.z_info.num_samples);
	const bool htile_stencil_compat = depth_htile_stencil_acceleration_compatible(
	    has_stencil, has_htile, z.stencil_info.tile_stencil_disable);
	const auto view = ResolveTargetViewInfo(z.depth_view.slice_start, z.depth_view.slice_max);
	switch (view.type) {
		case TargetViewType::Image2D: break;
		case TargetViewType::Image2DArray:
			DepthFatal("layered depth views are unsupported: base=%u count=%u", view.base_layer,
			           view.layer_count);
		case TargetViewType::Unsupported:
			DepthFatal("invalid depth view: base=%u last=%u", z.depth_view.slice_start,
			           z.depth_view.slice_max);
	}
	// Prospero defines the compression-disable bits as tile writeback policy. Vulkan attachments
	// expose the same logical depth/stencil values regardless of the driver's backing compression.
	if ((stencil_active && !has_stencil) || rc.resummarize_enable || rc.copy_centroid ||
	    rc.copy_sample != 0 || z.z_info.expclear_enabled || z.stencil_info.expclear_enabled ||
	    z.z_info.embedded_sample_locations || z.z_info.partially_resident ||
	    z.stencil_info.partially_resident || z.z_info.plane_compression != 0 ||
	    (z.z_info.num_samples != 0 && !msaa_compat) || z.z_info.num_mip_levels != 0 ||
	    z.z_info.tile_mode_index != 0 || z.z_info.zrange_precision > 1 ||
	    z.depth_view.current_mip_level != 0 || z.depth_info.addr5_swizzle_mask != 0 ||
	    z.depth_info.array_mode != 0 || z.depth_info.pipe_config != 0 ||
	    z.depth_info.bank_width != 0 || z.depth_info.bank_height != 0 ||
	    z.depth_info.macro_tile_aspect != 0 || z.depth_info.num_banks != 0 ||
	    z.htile_surface.linear != 0 || z.htile_surface.full_cache != 0 ||
	    z.htile_surface.htile_uses_preload_win != 0 || z.htile_surface.preload != 0 ||
	    z.htile_surface.prefetch_width != 0 || z.htile_surface.prefetch_height != 0 ||
	    z.htile_surface.dst_outside_zero_to_one != 0 || z.z_read_base_addr == 0 ||
	    z.z_write_base_addr != z.z_read_base_addr || (z.z_read_base_addr & 0xffffu) != 0 ||
	    dc.zfunc > static_cast<uint8_t>(VK_COMPARE_OP_ALWAYS)) {
		DepthFatal("unsupported depth register state");
	}
	if (msaa_compat) {
		static std::atomic<uint32_t> logged_fragments = 0;
		const uint32_t               bit              = 1u << z.z_info.num_samples;
		if ((logged_fragments.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
			LOGF("DepthTarget: compatibility: rendering PS5 %ux depth fragments as single-sample\n",
			     bit);
		}
	}
	if (has_stencil) {
		// Prospero defines Hi-Stencil as HTile-backed acceleration of the logical stencil plane.
		// Keep the plane native in Vulkan while tracking HTile separately.
		if (z.stencil_info.format != Prospero::GpuEnumValue(Prospero::StencilFormat::k8UInt) ||
		    z.stencil_info.tile_mode_index != 0 || z.stencil_info.tile_split != 0 ||
		    !htile_stencil_compat || z.stencil_info.texture_compatible_stencil ||
		    z.stencil_read_base_addr == 0 ||
		    z.stencil_write_base_addr != z.stencil_read_base_addr ||
		    (z.stencil_read_base_addr & 0xffffu) != 0 || z.depth_view.stencil_write_disable) {
			DepthFatal("unsupported stencil attachment state");
		}
		if (!z.stencil_info.tile_stencil_disable) {
			static std::atomic_bool logged = false;
			if (!logged.load(std::memory_order_relaxed) &&
			    !logged.exchange(true, std::memory_order_relaxed)) {
				LOGF("DepthTarget: compatibility: using native stencil with PS5 HTILE "
				     "acceleration\n");
			}
		}
	} else if (z.stencil_read_base_addr != 0 || z.stencil_write_base_addr != 0 ||
	           z.stencil_info.tile_mode_index != 0 || z.stencil_info.tile_split != 0 ||
	           !htile_stencil_compat || z.stencil_info.texture_compatible_stencil) {
		DepthFatal("stencil state without an active stencil attachment");
	}
	if (has_htile) {
		if (z.htile_data_base_addr == 0 || (z.htile_data_base_addr & 0x7fffu) != 0) {
			DepthFatal("invalid HTile metadata address");
		}
		if (z.depth_view.slice_max >= 32) {
			DepthFatal("HTile clear tracking supports at most 32 slices");
		}
	} else if (z.htile_data_base_addr != 0) {
		DepthFatal("HTile address without an enabled tile surface");
	}
	const bool size_xy_valid = z.size.valid && (z.size.x_max != 0 || z.size.y_max != 0);
	const bool wh_valid      = z.width_height_valid && z.width != 0 && z.height != 0;
	// DB_DEPTH_SIZE encodes the surface pitch/height in 8-pixel units. It is normally only
	// cross-checked against the extent below, but a depth surface can be bound with just this
	// register (Unity's HTile/quad clear path does exactly that), so use it as a third source.
	const bool encoded_extent_valid =
	    !size_xy_valid && !wh_valid && z.pitch_height_valid &&
	    (z.pitch_div8_minus1 != 0 || z.height_div8_minus1 != 0);
	// Last resort: a depth/stencil surface can be bound with valid base addresses but no extent
	// register at all (Unity binds one this way for stencil-only passes). Every attachment in a
	// render pass shares the framebuffer extent, so fall back to a bound colour target; if there
	// is no colour target either, the screen scissor bounds the render area.
	uint32_t fallback_width  = 0;
	uint32_t fallback_height = 0;
	for (uint32_t slot = 0; slot < 8 && fallback_width == 0; slot++) {
		const auto& ct = hw.GetRenderTarget(slot);
		if (ct.base.addr != 0 && ct.size.width != 0 && ct.size.height != 0) {
			fallback_width  = ct.size.width;
			fallback_height = ct.size.height;
		}
	}
	const char* fallback_source = "colour target";
	const auto& sv              = hw.GetScreenViewport();
	if (fallback_width == 0 || fallback_height == 0) {
		// The viewport transform bounds the region a draw can address: x spans
		// [xoffset - xscale, xoffset + xscale] and y likewise (yscale is negative for the
		// PS5's flipped clip space). This is the only extent the guest always sets, because
		// it is expressed as floats rather than as a size register.
		const auto& vp     = sv.viewports[0];
		const float x_max  = vp.xoffset + std::fabs(vp.xscale);
		const float y_max  = vp.yoffset + std::fabs(vp.yscale);
		if (x_max >= 1.0f && y_max >= 1.0f && x_max <= 16384.0f && y_max <= 16384.0f) {
			fallback_width  = static_cast<uint32_t>(std::ceil(x_max));
			fallback_height = static_cast<uint32_t>(std::ceil(y_max));
			fallback_source = "viewport";
		}
	}
	if (fallback_width == 0 || fallback_height == 0) {
		if (sv.screen_scissor_right > sv.screen_scissor_left &&
		    sv.screen_scissor_bottom > sv.screen_scissor_top) {
			fallback_width  = static_cast<uint32_t>(sv.screen_scissor_right - sv.screen_scissor_left);
			fallback_height = static_cast<uint32_t>(sv.screen_scissor_bottom - sv.screen_scissor_top);
			fallback_source = "screen scissor";
		}
	}
	const bool fallback_extent_valid = !size_xy_valid && !wh_valid && !encoded_extent_valid &&
	                                   fallback_width != 0 && fallback_height != 0;
	if (fallback_extent_valid) {
		static std::atomic_bool logged = false;
		if (!logged.load(std::memory_order_relaxed) &&
		    !logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: compatibility: no depth extent register set; using %s extent "
			     "%ux%u\n",
			     fallback_source, fallback_width, fallback_height);
		}
	}
	// A depth surface whose extent registers were explicitly written as zero is degenerate: the
	// guest carries the depth/stencil state purely so a fast-clear can be issued, and the actual
	// clear is performed by the HTile compute path rather than by a render pass. A zero-sized
	// attachment cannot be created in Vulkan, so drop it for this draw instead of aborting.
	if (!size_xy_valid && !wh_valid && !encoded_extent_valid && !fallback_extent_valid &&
	    !depth_active) {
		static std::atomic_bool logged = false;
		if (!logged.load(std::memory_order_relaxed) &&
		    !logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: compatibility: dropping zero-extent depth/stencil attachment "
			     "(stencil_clear=%d stencil_test=%d); the fast-clear path owns this surface\n",
			     static_cast<int>(rc.stencil_clear_enable), static_cast<int>(dc.stencil_enable));
		}
		return;
	}
	if (!size_xy_valid && !wh_valid && !encoded_extent_valid && !fallback_extent_valid) {
		for (uint32_t slot = 0; slot < 8; slot++) {
			const auto& ct = hw.GetRenderTarget(slot);
			if (ct.base.addr != 0 || ct.size.width != 0 || ct.size.height != 0) {
				LOGF("  colour target %u: addr=0x%016" PRIx64 " %ux%u\n", slot, ct.base.addr,
				     ct.size.width, ct.size.height);
			}
		}
		const auto& sv = hw.GetScreenViewport();
		LOGF("  screen scissor: %d,%d..%d,%d  generic: %d,%d..%d,%d  window: %d,%d..%d,%d\n",
		     sv.screen_scissor_left, sv.screen_scissor_top, sv.screen_scissor_right,
		     sv.screen_scissor_bottom, sv.generic_scissor_left, sv.generic_scissor_top,
		     sv.generic_scissor_right, sv.generic_scissor_bottom, sv.window_scissor_left,
		     sv.window_scissor_top, sv.window_scissor_right, sv.window_scissor_bottom);
		LOGF("  viewport0: xscale=%f yscale=%f xoffset=%f yoffset=%f\n", sv.viewports[0].xscale,
		     sv.viewports[0].yscale, sv.viewports[0].xoffset, sv.viewports[0].yoffset);
		DepthFatal("missing depth extent: size.valid=%d x_max=%u y_max=%u wh_valid=%d w=%u h=%u "
		           "pitch_height_valid=%d pitch8=%u height8=%u slice64=%u z_fmt=%u s_fmt=%u "
		           "htile=%d z_base=0x%016" PRIx64 " s_base=0x%016" PRIx64
		           " htile_base=0x%016" PRIx64 " depth_active=%d stencil_active=%d "
		           "z_en=%d z_wr=%d z_clear=%d s_clear=%d",
		           static_cast<int>(z.size.valid), z.size.x_max, z.size.y_max,
		           static_cast<int>(z.width_height_valid), z.width, z.height,
		           static_cast<int>(z.pitch_height_valid), z.pitch_div8_minus1,
		           z.height_div8_minus1, z.slice_div64_minus1, z.z_info.format,
		           z.stencil_info.format, static_cast<int>(has_htile), z.z_read_base_addr,
		           z.stencil_read_base_addr, z.htile_data_base_addr,
		           static_cast<int>(depth_active), static_cast<int>(stencil_active),
		           static_cast<int>(dc.z_enable), static_cast<int>(dc.z_write_enable),
		           static_cast<int>(rc.depth_clear_enable),
		           static_cast<int>(rc.stencil_clear_enable));
	}
	if (encoded_extent_valid) {
		static std::atomic_bool logged = false;
		if (!logged.load(std::memory_order_relaxed) &&
		    !logged.exchange(true, std::memory_order_relaxed)) {
			LOGF("DepthTarget: deriving depth extent from encoded pitch/height (%ux%u)\n",
			     (z.pitch_div8_minus1 + 1u) * 8u, (z.height_div8_minus1 + 1u) * 8u);
		}
	}
	const uint32_t width  = size_xy_valid           ? static_cast<uint32_t>(z.size.x_max) + 1u
	                        : wh_valid             ? z.width
	                        : encoded_extent_valid ? (z.pitch_div8_minus1 + 1u) * 8u
	                                               : fallback_width;
	const uint32_t height = size_xy_valid           ? static_cast<uint32_t>(z.size.y_max) + 1u
	                        : wh_valid             ? z.height
	                        : encoded_extent_valid ? (z.height_div8_minus1 + 1u) * 8u
	                                               : fallback_height;
	if (width > 16384 || height > 16384 ||
	    (size_xy_valid && wh_valid && (width != z.width || height != z.height)) ||
	    (!z.pitch_height_valid &&
	     (z.pitch_div8_minus1 != 0 || z.height_div8_minus1 != 0 || z.slice_div64_minus1 != 0))) {
		DepthFatal("inconsistent depth extent or encoded layout");
	}
	uint32_t guest_format = 0;
	uint32_t bytes        = 0;
	switch (static_cast<Prospero::DepthFormat>(z.z_info.format)) {
		case Prospero::DepthFormat::kZ16:
			if (has_stencil) {
				DepthFatal("Z16 plus stencil is unsupported");
			}
			r->format    = VK_FORMAT_D16_UNORM;
			guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k16UNorm);
			bytes        = 2;
			break;
		case Prospero::DepthFormat::kZ32F:
			r->format    = has_stencil ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D32_SFLOAT;
			guest_format = Prospero::GpuEnumValue(Prospero::BufferFormat::k32Float);
			bytes        = 4;
			break;
		default: DepthFatal("unsupported depth format");
	}
	const auto pitch = TileGetTexturePitch(guest_format, width, 1,
	                                       Prospero::GpuEnumValue(Prospero::TileMode::kDepth));
	if (z.pitch_height_valid && ((static_cast<uint64_t>(z.pitch_div8_minus1) + 1u) * 8u != pitch ||
	                             (static_cast<uint64_t>(z.height_div8_minus1) + 1u) * 8u !=
	                                 ((static_cast<uint64_t>(height) + 7u) & ~7ull))) {
		DepthFatal("encoded depth pitch or height mismatch");
	}
	const uint32_t block_width = bytes == 2 ? 256u : 128u;
	const uint64_t padded_width =
	    (static_cast<uint64_t>(pitch) + block_width - 1u) & ~(block_width - 1u);
	const uint64_t padded_height =
	    (static_cast<uint64_t>(height) + 127u) & ~static_cast<uint64_t>(127u);
	if (padded_width > UINT64_MAX / padded_height ||
	    padded_width * padded_height > UINT64_MAX / bytes) {
		DepthFatal("depth footprint overflow");
	}
	const uint64_t expected_size = padded_width * padded_height * bytes;
	TileSizeAlign  depth_size {};
	TileSizeAlign  stencil_size {};
	TileSizeAlign  htile_size {};
	if (has_stencil || has_htile) {
		if (!TileGetDepthSize(width, height, 0, z.z_info.format, z.stencil_info.format, has_htile,
		                      &stencil_size, &htile_size, &depth_size) ||
		    depth_size.align != 65536 || depth_size.size != expected_size ||
		    (has_stencil != (stencil_size.align == 65536 && stencil_size.size != 0)) ||
		    (has_htile != (htile_size.align == 32768 && htile_size.size != 0))) {
			DepthFatal("unsupported depth/stencil/HTile footprint");
		}
	} else {
		TileGetTextureTotalSize(guest_format, width, height, 1, pitch, 1,
		                        Prospero::GpuEnumValue(Prospero::TileMode::kDepth), false,
		                        &depth_size);
	}
	if (expected_size == 0 || expected_size > UINT32_MAX || depth_size.align != 65536 ||
	    depth_size.size != expected_size ||
	    (z.pitch_height_valid &&
	     (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u != expected_size)) {
		DepthFatal("depth footprint mismatch: extent=%ux%u pitch=%u expected=0x%016" PRIx64
		           " calculated=0x%016" PRIx64 "/0x%016" PRIx64
		           " encoded_valid=%u encoded=0x%016" PRIx64,
		           width, height, pitch, expected_size, depth_size.size, depth_size.align,
		           z.pitch_height_valid ? 1u : 0u,
		           (static_cast<uint64_t>(z.slice_div64_minus1) + 1u) * 64u);
	}
	if (depth_size.size > UINT64_MAX / view.image_layers ||
	    stencil_size.size > UINT64_MAX / view.image_layers ||
	    htile_size.size > UINT64_MAX / view.image_layers) {
		DepthFatal("layered depth footprint overflow");
	}
	const auto depth_backing_size   = depth_size.size * view.image_layers;
	const auto stencil_backing_size = stencil_size.size * view.image_layers;
	const auto htile_backing_size   = htile_size.size * view.image_layers;
	if (depth_backing_size > TRACKER_ADDRESS_SIZE - z.z_read_base_addr ||
	    (has_stencil && stencil_backing_size > TRACKER_ADDRESS_SIZE - z.stencil_read_base_addr) ||
	    (has_htile && htile_backing_size > TRACKER_ADDRESS_SIZE - z.htile_data_base_addr)) {
		DepthFatal("layered depth backing range is invalid");
	}
	r->htile                = has_htile;
	r->width                = width;
	r->height               = height;
	r->depth_buffer_size    = depth_backing_size;
	r->depth_buffer_vaddr   = z.z_read_base_addr;
	r->stencil_buffer_size  = has_stencil ? stencil_backing_size : 0;
	r->stencil_buffer_vaddr = has_stencil ? z.stencil_read_base_addr : 0;
	r->htile_buffer_size    = has_htile ? htile_backing_size : 0;
	r->htile_buffer_vaddr   = has_htile ? z.htile_data_base_addr : 0;
	auto* cache             = g_render_ctx->GetTextureCache();
	if (has_htile) {
		cache->RegisterMeta(r->htile_buffer_vaddr, r->htile_buffer_size, view.image_layers);
	}
	if (has_htile && rc.depth_clear_enable && !cache->ClearMeta(z.htile_data_base_addr)) {
		DepthFatal("failed to acquire HTile metadata for a depth clear");
	}
	const bool meta_clear =
	    has_htile && cache->IsMetaCleared(z.htile_data_base_addr, z.depth_view.slice_start);
	r->depth_clear_enable      = rc.depth_clear_enable;
	r->depth_meta_clear_enable = meta_clear;
	r->depth_load_clear_enable = r->depth_clear_enable || r->depth_meta_clear_enable;
	r->depth_clear_value       = hw.GetDepthClearValue();
	r->depth_test_enable       = dc.z_enable;
	r->depth_write_enable      = dc.z_write_enable && !z.depth_view.depth_write_disable;
	r->depth_compare_op        = static_cast<VkCompareOp>(dc.zfunc);

	r->depth_bounds_test_enable = dc.depth_bounds_enable;
	r->depth_min_bounds         = hw.GetDepthBoundsMin();
	r->depth_max_bounds         = hw.GetDepthBoundsMax();

	r->stencil_clear_enable = rc.stencil_clear_enable;
	r->stencil_clear_value  = hw.GetStencilClearValue();
	r->stencil_test_enable  = dc.stencil_enable;
	if (dc.stencil_enable) {
		if (dc.stencilfunc > static_cast<uint8_t>(VK_COMPARE_OP_ALWAYS) ||
		    (dc.backface_enable &&
		     dc.stencilfunc_bf > static_cast<uint8_t>(VK_COMPARE_OP_ALWAYS)) ||
		    (UsesStencilOpValue(sc.stencil_fail, sc.stencil_zpass, sc.stencil_zfail) &&
		     sm.stencil_opval != sm.stencil_testval) ||
		    (dc.backface_enable &&
		     UsesStencilOpValue(sc.stencil_fail_bf, sc.stencil_zpass_bf, sc.stencil_zfail_bf) &&
		     sm.stencil_opval_bf != sm.stencil_testval_bf)) {
			DepthFatal("unsupported stencil compare or replacement state");
		}
		r->stencil_static_front = {
		    ConvertStencilOp(sc.stencil_fail), ConvertStencilOp(sc.stencil_zpass),
		    ConvertStencilOp(sc.stencil_zfail), static_cast<VkCompareOp>(dc.stencilfunc)};
		r->stencil_dynamic_front = {sm.stencil_mask,
		                            rc.stencil_clear_enable ? 0u : sm.stencil_writemask,
		                            sm.stencil_testval};
		if (dc.backface_enable) {
			r->stencil_static_back = {
			    ConvertStencilOp(sc.stencil_fail_bf), ConvertStencilOp(sc.stencil_zpass_bf),
			    ConvertStencilOp(sc.stencil_zfail_bf), static_cast<VkCompareOp>(dc.stencilfunc_bf)};
			r->stencil_dynamic_back = {sm.stencil_mask_bf,
			                           rc.stencil_clear_enable ? 0u : sm.stencil_writemask_bf,
			                           sm.stencil_testval_bf};
		} else {
			r->stencil_static_back  = r->stencil_static_front;
			r->stencil_dynamic_back = r->stencil_dynamic_front;
		}
	}
	r->vaddr_num = has_stencil ? 2 : 1;
	r->vaddr[0]  = r->depth_buffer_vaddr;
	r->size[0]   = r->depth_buffer_size;
	if (has_stencil) {
		r->vaddr[1] = r->stencil_buffer_vaddr;
		r->size[1]  = r->stencil_buffer_size;
	}
	DepthTargetInfo info {};
	info.address            = r->depth_buffer_vaddr;
	info.size               = r->depth_buffer_size;
	info.stencil_address    = r->stencil_buffer_vaddr;
	info.stencil_size       = r->stencil_buffer_size;
	info.htile_address      = r->htile_buffer_vaddr;
	info.htile_size         = r->htile_buffer_size;
	info.format             = r->format;
	info.guest_format       = guest_format;
	info.width              = width;
	info.height             = height;
	info.pitch              = pitch;
	info.bytes_per_element  = bytes;
	info.tile_mode          = Prospero::GpuEnumValue(Prospero::TileMode::kDepth);
	info.layers             = view.image_layers;
	info.depth_load_clear   = r->depth_load_clear_enable;
	info.stencil_load_clear = rc.stencil_clear_enable;
	info.stencil_access =
	    r->stencil_clear_enable ||
	    (r->stencil_test_enable &&
	     (stencil_face_accesses_attachment(r->stencil_static_front, r->stencil_dynamic_front) ||
	      stencil_face_accesses_attachment(r->stencil_static_back, r->stencil_dynamic_back)));
	info.stencil_htile_compressed =
	    has_stencil && has_htile && !z.stencil_info.tile_stencil_disable;
	r->vulkan_buffer = cache->FindDepthTarget(buffer, g_render_ctx->GetGraphicCtx(), info);
	r->vulkan_view   = cache->GetDepthTargetAttachmentView(
	    g_render_ctx->GetGraphicCtx(), r->vulkan_buffer, view.base_layer, view.layer_count);
	if (meta_clear && !cache->TouchMeta(z.htile_data_base_addr, z.depth_view.slice_start, false)) {
		DepthFatal("failed to consume HTile clear state");
	}
}

void MarkRenderTargetGpuWritten(const RenderDepthInfo& target) {
	const bool with_depth = target.format != VK_FORMAT_UNDEFINED && target.vulkan_buffer != nullptr;

	if (with_depth && !depth_attachment_read_only(&target)) {
		g_render_ctx->GetTextureCache()->MarkGpuWritten(target.vulkan_buffer);
	}
}

} // namespace Libs::Graphics
