/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "runtime.hpp"
#include "version.h"
#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "ini_file.hpp"
#include "addon_manager.hpp"
#include "input.hpp"
#include "input_gamepad.hpp"
#include "com_ptr.hpp"
#include "platform_utils.hpp"
#include "reshade_api_object_impl.hpp"
#include <set>
#include <thread>
#include <cmath> // std::abs, std::fmod
#include <cctype> // std::toupper
#include <cwctype> // std::towlower
#include <cstdio> // std::snprintf
#include <cstdlib> // std::malloc, std::rand, std::strtod, std::strtol
#include <cstring> // std::memcpy, std::memset, std::strlen
#include <charconv> // std::to_chars
#include <algorithm> // std::all_of, std::copy_n, std::equal, std::fill_n, std::find, std::find_if, std::for_each, std::max, std::min, std::replace, std::remove, std::remove_if, std::reverse, std::search, std::set_symmetric_difference, std::sort, std::stable_sort, std::swap, std::transform

bool resolve_path(std::filesystem::path &path, std::error_code &ec)
{
	// First convert path to an absolute path
	// Ignore the working directory and instead start relative paths at the DLL location
	if (path.is_relative())
		path = g_reshade_base_path / path;
	// Finally try to canonicalize the path too
	if (std::filesystem::path canonical_path = std::filesystem::canonical(path, ec); !ec)
		path = std::move(canonical_path);
	else
		path = path.lexically_normal();
	return !ec; // The canonicalization step fails if the path does not exist
}

bool resolve_preset_path(std::filesystem::path &path, std::error_code &ec)
{
	ec.clear();
	// First make sure the extension matches, before diving into the file system
	if (const std::filesystem::path ext = path.extension();
		ext != L".ini" && ext != L".txt")
		return false;
	// A non-existent path is valid for a new preset
	// Otherwise ensure the file has a technique list, which should make it a preset
	return !resolve_path(path, ec) || ini_file::load_cache(path).has({}, "Techniques");
}

static std::filesystem::path make_relative_path(const std::filesystem::path &path)
{
	if (path.empty())
		return path;
	// Use ReShade DLL directory as base for relative paths (see 'resolve_path')
	std::filesystem::path proximate_path = path.lexically_proximate(g_reshade_base_path);
	if (proximate_path.native().rfind(L"..", 0) != std::wstring::npos)
		return path; // Do not use relative path if preset is in a parent directory
	if (proximate_path.is_relative() && !proximate_path.empty() && proximate_path.native().front() != L'.')
		// Prefix preset path with dot character to better indicate it being a relative path
		proximate_path = L"." / proximate_path;
	return proximate_path;
}

static bool find_file(const std::vector<std::filesystem::path> &search_paths, std::filesystem::path &path)
{
	std::error_code ec;
	// Do not have to perform a search if the path is already absolute
	if (path.is_absolute())
		return std::filesystem::exists(path, ec);

	for (std::filesystem::path search_path : search_paths)
	{
		const bool recursive_search = search_path.filename() == L"**";
		if (recursive_search)
			search_path.remove_filename();

		if (resolve_path(search_path, ec))
		{
			// Append relative file path to absolute search path
			if (std::filesystem::path search_sub_path = search_path / path;
				std::filesystem::exists(search_sub_path, ec))
			{
				path = std::move(search_sub_path);
				return true;
			}

			if (recursive_search)
			{
				for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(search_path, std::filesystem::directory_options::skip_permission_denied, ec))
				{
					if (!entry.is_directory(ec))
						continue;

					if (std::filesystem::path search_sub_path = entry / path;
						std::filesystem::exists(search_sub_path, ec))
					{
						path = std::move(search_sub_path);
						return true;
					}
				}
			}
		}
		else
		{
			reshade::log::message(reshade::log::level::warning, "Failed to resolve search path '%s' with error code %d.", search_path.u8string().c_str(), ec.value());
		}
	}

	return false;
}
static std::vector<std::filesystem::path> find_files(const std::vector<std::filesystem::path> &search_paths, std::initializer_list<std::filesystem::path> extensions)
{
	std::error_code ec;
	std::vector<std::filesystem::path> files;
	std::vector<std::pair<std::filesystem::path, bool>> resolved_search_paths;

	// First resolve all search paths and ensure they are all unique
	for (std::filesystem::path search_path : search_paths)
	{
		const bool recursive_search = search_path.filename() == L"**";
		if (recursive_search)
			search_path.remove_filename();

		if (resolve_path(search_path, ec))
		{
			if (const auto it = std::find_if(resolved_search_paths.begin(), resolved_search_paths.end(),
					[&search_path](const std::pair<std::filesystem::path, bool> &recursive_search_path) {
						return recursive_search_path.first == search_path;
					});
				it != resolved_search_paths.end())
				it->second |= recursive_search;
			else
				resolved_search_paths.push_back(std::make_pair(std::move(search_path), recursive_search));
		}
		else
		{
			reshade::log::message(reshade::log::level::warning, "Failed to resolve search path '%s' with error code %d.", search_path.u8string().c_str(), ec.value());
		}
	}

	// Then iterate through all files in those search paths and add those with a matching extension
	const auto check_and_add_file = [&extensions, &ec, &files](const std::filesystem::directory_entry &entry) {
		if (!entry.is_directory(ec) &&
			std::find(extensions.begin(), extensions.end(), entry.path().extension()) != extensions.end())
			files.emplace_back(entry); // Construct path from directory entry in-place
	};

	for (const std::pair<std::filesystem::path, bool> &resolved_search_path : resolved_search_paths)
	{
		if (resolved_search_path.second)
			for (const std::filesystem::directory_entry &entry : std::filesystem::recursive_directory_iterator(resolved_search_path.first, std::filesystem::directory_options::skip_permission_denied, ec))
				check_and_add_file(entry);
		else
			for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(resolved_search_path.first, std::filesystem::directory_options::skip_permission_denied, ec))
				check_and_add_file(entry);
	}

	return files;
}

reshade::runtime::runtime(api::swapchain *swapchain, api::command_queue *graphics_queue, const std::filesystem::path &config_path) :
	_swapchain(swapchain),
	_device(swapchain->get_device()),
	_graphics_queue(graphics_queue),
	_start_time(std::chrono::high_resolution_clock::now()),
	_last_present_time(_start_time),
	_last_frame_duration(std::chrono::milliseconds(1))
{
	assert(swapchain != nullptr && graphics_queue != nullptr);

	_device->get_property(api::device_properties::vendor_id, &_vendor_id);
	_device->get_property(api::device_properties::device_id, &_device_id);

	_device->get_property(api::device_properties::api_version, &_renderer_id);
	switch (_device->get_api())
	{
	case api::device_api::d3d9:
	case api::device_api::d3d10:
	case api::device_api::d3d11:
	case api::device_api::d3d12:
		break;
	case api::device_api::opengl:
		_renderer_id |= 0x10000;
		break;
	case api::device_api::vulkan:
		_renderer_id |= 0x20000;
		break;
	}

	char device_description[256] = "";
	_device->get_property(api::device_properties::description, device_description);

	if (uint32_t driver_version = 0;
		_device->get_property(api::device_properties::driver_version, &driver_version))
		log::message(log::level::info, "Running on %s Driver %u.%u.", device_description, driver_version / 100, driver_version % 100);
	else
		log::message(log::level::info, "Running on %s.", device_description);

#if RESHADE_GUI
	init_gui();
#endif
}
reshade::runtime::~runtime()
{
	assert(_worker_threads.empty());
	assert(!_is_initialized && _techniques.empty() && _technique_sorting.empty());

#if RESHADE_GUI
	deinit_gui();
#endif
}

bool reshade::runtime::on_init()
{
	assert(!_is_initialized);

	const api::resource_desc back_buffer_desc = _device->get_resource_desc(_swapchain->get_back_buffer(0));

	// Avoid initializing on very small swap chains (e.g. implicit swap chain in The Sims 4, which is not used to present in windowed mode)
	if (back_buffer_desc.texture.width <= 16 && back_buffer_desc.texture.height <= 16)
		return false;

	_width = back_buffer_desc.texture.width;
	_height = back_buffer_desc.texture.height;
	_back_buffer_format = api::format_to_default_typed(back_buffer_desc.texture.format);
	_back_buffer_samples = back_buffer_desc.texture.samples;
	_back_buffer_color_space = _swapchain->get_color_space();

	// Create resolve texture and copy pipeline (do this before creating effect resources, to ensure correct back buffer format is set up)
	if (back_buffer_desc.texture.samples > 1 ||
		// Always use resolve texture in OpenGL to flip vertically and support sRGB + binding effect stencil
		(_device->get_api() == api::device_api::opengl) ||
		// Some effects rely on there being an alpha channel available, so create resolve texture if that is not the case
		(_back_buffer_format == api::format::r8g8b8x8_unorm || _back_buffer_format == api::format::b8g8r8x8_unorm))
	{
		switch (_back_buffer_format)
		{
		case api::format::r8g8b8x8_unorm:
			_back_buffer_format = api::format::r8g8b8a8_unorm;
			break;
		case api::format::b8g8r8x8_unorm:
			_back_buffer_format = api::format::b8g8r8a8_unorm;
			break;
		}

		const bool need_copy_pipeline =
			_device->get_api() == api::device_api::d3d10 ||
			_device->get_api() == api::device_api::d3d11 ||
			_device->get_api() == api::device_api::d3d12;

		api::resource_usage usage = api::resource_usage::render_target | api::resource_usage::copy_dest | api::resource_usage::resolve_dest;
		if (need_copy_pipeline)
			usage |= api::resource_usage::shader_resource;
		else
			usage |= api::resource_usage::copy_source;

		if (!_device->create_resource(
				api::resource_desc(_width, _height, 1, 1, api::format_to_typeless(_back_buffer_format), 1, api::memory_heap::gpu_only, usage),
				nullptr, back_buffer_desc.texture.samples == 1 ? api::resource_usage::copy_dest : api::resource_usage::resolve_dest, &_back_buffer_resolved) ||
			!_device->create_resource_view(
				_back_buffer_resolved,
				api::resource_usage::render_target,
				api::resource_view_desc(api::format_to_default_typed(_back_buffer_format, 0)),
				&_back_buffer_targets.emplace_back()) ||
			!_device->create_resource_view(
				_back_buffer_resolved,
				api::resource_usage::render_target,
				api::resource_view_desc(api::format_to_default_typed(_back_buffer_format, 1)),
				&_back_buffer_targets.emplace_back()))
		{
			log::message(log::level::error, "Failed to create resolve texture resource!");
			goto exit_failure;
		}

		if (need_copy_pipeline)
		{
			if (!_device->create_resource_view(
					_back_buffer_resolved,
					api::resource_usage::shader_resource,
					api::resource_view_desc(_back_buffer_format),
					&_back_buffer_resolved_srv))
			{
				log::message(log::level::error, "Failed to create resolve shader resource view!");
				goto exit_failure;
			}

			api::sampler_desc sampler_desc = {};
			sampler_desc.filter = api::filter_mode::min_mag_mip_point;
			sampler_desc.address_u = api::texture_address_mode::clamp;
			sampler_desc.address_v = api::texture_address_mode::clamp;
			sampler_desc.address_w = api::texture_address_mode::clamp;

			api::pipeline_layout_param layout_params[2];
			layout_params[0] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::all, 1, api::descriptor_type::sampler };
			layout_params[1] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::all, 1, api::descriptor_type::shader_resource_view };

			const resources::data_resource vs = resources::load_data_resource(IDR_FULLSCREEN_VS);
			const resources::data_resource ps = resources::load_data_resource(IDR_COPY_PS);

			api::shader_desc vs_desc = { vs.data, vs.data_size };
			api::shader_desc ps_desc = { ps.data, ps.data_size };

			std::vector<api::pipeline_subobject> subobjects;
			subobjects.push_back({ api::pipeline_subobject_type::vertex_shader, 1, &vs_desc });
			subobjects.push_back({ api::pipeline_subobject_type::pixel_shader, 1, &ps_desc });

			if (!_device->create_pipeline_layout(2, layout_params, &_copy_pipeline_layout) ||
				!_device->create_pipeline(_copy_pipeline_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &_copy_pipeline) ||
				!_device->create_sampler(sampler_desc, &_copy_sampler_state))
			{
				log::message(log::level::error, "Failed to create copy pipeline!");
				goto exit_failure;
			}
		}
	}

	// Create an empty texture, which is bound to shader resource view slots with an unknown semantic (since it is not valid to bind a zero handle in Vulkan, unless the 'VK_EXT_robustness2' extension is enabled)
	if (_empty_tex == 0)
	{
		// Use VK_FORMAT_R16_SFLOAT format, since it is mandatory according to the spec (see https://www.khronos.org/registry/vulkan/specs/1.1/html/vkspec.html#features-required-format-support)
		if (!_device->create_resource(
				api::resource_desc(1, 1, 1, 1, api::format::r16_float, 1, api::memory_heap::gpu_only, api::resource_usage::shader_resource),
				nullptr, api::resource_usage::shader_resource, &_empty_tex))
		{
			log::message(log::level::error, "Failed to create empty texture resource!");
			goto exit_failure;
		}

		_device->set_resource_name(_empty_tex, "ReShade empty texture");

		if (!_device->create_resource_view(_empty_tex, api::resource_usage::shader_resource, api::resource_view_desc(api::format::r16_float), &_empty_srv))
		{
			log::message(log::level::error, "Failed to create empty texture shader resource view!");
			goto exit_failure;
		}
	}

	// Create effect color and stencil resource
	api::format stencil_format = api::format::unknown;
	{
		// Find a supported stencil format with the smallest footprint (since the depth component is not used)
		constexpr api::format possible_stencil_formats[] = {
			api::format::s8_uint,
			api::format::d16_unorm_s8_uint,
			api::format::d24_unorm_s8_uint,
			api::format::d32_float_s8_uint
		};

		for (const api::format format : possible_stencil_formats)
		{
			if (_device->check_format_support(format, api::resource_usage::depth_stencil))
			{
				stencil_format = format;
				break;
			}
		}
	}

	// Create render targets for the back buffer resources
	for (uint32_t i = 0, count = _swapchain->get_back_buffer_count(); i < count; ++i)
	{
		const api::resource back_buffer_resource = _swapchain->get_back_buffer(i);

		if (!_device->create_resource_view(
				back_buffer_resource,
				api::resource_usage::render_target,
				api::resource_view_desc(
					back_buffer_desc.texture.samples > 1 ? api::resource_view_type::texture_2d_multisample : api::resource_view_type::texture_2d,
					api::format_to_default_typed(back_buffer_desc.texture.format, 0), 0, 1, 0, 1),
				&_back_buffer_targets.emplace_back()) ||
			!_device->create_resource_view(
				back_buffer_resource,
				api::resource_usage::render_target,
				api::resource_view_desc(
					back_buffer_desc.texture.samples > 1 ? api::resource_view_type::texture_2d_multisample : api::resource_view_type::texture_2d,
					api::format_to_default_typed(back_buffer_desc.texture.format, 1), 0, 1, 0, 1),
				&_back_buffer_targets.emplace_back()))
		{
			log::message(log::level::error, "Failed to create back buffer render targets!");
			goto exit_failure;
		}
	}

	create_state_block(_device, &_app_state);

#if RESHADE_GUI
	if (!init_imgui_resources())
		goto exit_failure;
#endif

	const input::window_handle window = get_hwnd();
	if (window != nullptr)
		_input = input::register_window(window);
	else
		_input.reset();

	// GTK 3 enables transparency for windows, which messes with effects that do not return an alpha value, so disable that again
	if (window != nullptr)
		utils::set_window_transparency(window, false);

	// Reset frame count to zero so effects are loaded in 'update_effects'
	_frame_count = 0;

	_is_initialized = true;
	_last_reload_time = std::chrono::high_resolution_clock::now(); // Intentionally set to current time, so that duration to last reload is valid even when there is no reload on init

#if RESHADE_ADDON
	invoke_addon_event<addon_event::init_effect_runtime>(this);
#endif

	return true;

exit_failure:
	_device->destroy_resource(_empty_tex);
	_empty_tex = {};
	_device->destroy_resource_view(_empty_srv);
	_empty_srv = {};

	_device->destroy_pipeline(_copy_pipeline);
	_copy_pipeline = {};
	_device->destroy_pipeline_layout(_copy_pipeline_layout);
	_copy_pipeline_layout = {};
	_device->destroy_sampler(_copy_sampler_state);
	_copy_sampler_state = {};

	_device->destroy_resource(_back_buffer_resolved);
	_back_buffer_resolved = {};
	_device->destroy_resource_view(_back_buffer_resolved_srv);
	_back_buffer_resolved_srv = {};

	for (const api::resource_view view : _back_buffer_targets)
		_device->destroy_resource_view(view);
	_back_buffer_targets.clear();

	destroy_state_block(_device, _app_state);
	_app_state = {};

#if RESHADE_GUI
	destroy_imgui_resources();
#endif

	return false;
}
void reshade::runtime::on_reset()
{
	if (_is_initialized)
		// Update initialization state immediately, so that any effect loading still in progress can abort early
		_is_initialized = false;
	else
		return; // Nothing to do if the runtime was already destroyed or not successfully initialized in the first place

	_device->destroy_resource(_empty_tex);
	_empty_tex = {};
	_device->destroy_resource_view(_empty_srv);
	_empty_srv = {};

	_device->destroy_pipeline(_copy_pipeline);
	_copy_pipeline = {};
	_device->destroy_pipeline_layout(_copy_pipeline_layout);
	_copy_pipeline_layout = {};
	_device->destroy_sampler(_copy_sampler_state);
	_copy_sampler_state = {};

	_device->destroy_resource(_back_buffer_resolved);
	_back_buffer_resolved = {};
	_device->destroy_resource_view(_back_buffer_resolved_srv);
	_back_buffer_resolved_srv = {};

	for (const api::resource_view view : _back_buffer_targets)
		_device->destroy_resource_view(view);
	_back_buffer_targets.clear();

	destroy_state_block(_device, _app_state);
	_app_state = {};

	_device->destroy_fence(_queue_sync_fence);
	_queue_sync_fence = {};

	_width = _height = 0;
	_back_buffer_format = api::format::unknown;
	_back_buffer_samples = 1;
	_back_buffer_color_space = api::color_space::unknown;

#if RESHADE_GUI
	destroy_imgui_resources();
#endif

#if RESHADE_ADDON
	invoke_addon_event<addon_event::destroy_effect_runtime>(this);
#endif
}
void reshade::runtime::on_present(api::command_queue *present_queue)
{
	assert(present_queue != nullptr);

	if (!_is_initialized)
		return;

	// If the application is presenting with a different queue than rendering, synchronize these two queues first
	// This ensures that it has finished rendering before ReShade applies its own rendering
	if (present_queue != _graphics_queue)
	{
		if (_queue_sync_fence == 0 &&
			!_device->create_fence(_queue_sync_value, api::fence_flags::none, &_queue_sync_fence))
		{
			log::message(log::level::error, "Failed to create queue synchronization fence!");
			return;
		}

		_queue_sync_value++;

		// Signal from the queue the application is presenting with
		if (present_queue->signal(_queue_sync_fence, _queue_sync_value))
			// Wait on that before the immediate command list flush below
			_graphics_queue->wait(_queue_sync_fence, _queue_sync_value);
	}

	api::command_list *const cmd_list = _graphics_queue->get_immediate_command_list();

	capture_state(cmd_list, _app_state);

	uint32_t back_buffer_index = (_back_buffer_resolved != 0 ? 2 : 0) + _swapchain->get_current_back_buffer_index() * 2;
	const api::resource back_buffer_resource = _device->get_resource_from_view(_back_buffer_targets[back_buffer_index]);

	// Resolve MSAA back buffer if MSAA is active or copy when format conversion is required
	if (_back_buffer_resolved != 0)
	{
		if (_back_buffer_samples == 1)
		{
			cmd_list->barrier(back_buffer_resource, api::resource_usage::present, api::resource_usage::copy_source);
			cmd_list->copy_texture_region(back_buffer_resource, 0, nullptr, _back_buffer_resolved, 0, nullptr);
			cmd_list->barrier(_back_buffer_resolved, api::resource_usage::copy_dest, api::resource_usage::render_target);
		}
		else
		{
			cmd_list->barrier(back_buffer_resource, api::resource_usage::present, api::resource_usage::resolve_source);
			cmd_list->resolve_texture_region(back_buffer_resource, 0, nullptr, _back_buffer_resolved, 0, 0, 0, 0, _back_buffer_format);
			cmd_list->barrier(_back_buffer_resolved, api::resource_usage::resolve_dest, api::resource_usage::render_target);
		}
	}

	// Lock input so it cannot be modified by other threads while we are reading it here
	std::unique_lock<std::recursive_mutex> input_lock;
	if (_input != nullptr)
		input_lock = _input->lock();

	_current_time = std::chrono::system_clock::now();

	_frame_count++;
	const auto current_time = std::chrono::high_resolution_clock::now();
	_last_frame_duration = current_time - _last_present_time; _last_present_time = current_time;

#if RESHADE_GUI
	// Draw overlay
	draw_gui();
#endif

	// Stretch main render target back into MSAA back buffer if MSAA is active or copy when format conversion is required
	if (_back_buffer_resolved != 0)
	{
		const api::resource resources[2] = { back_buffer_resource, _back_buffer_resolved };
		const api::resource_usage state_old[2] = { api::resource_usage::copy_source | api::resource_usage::resolve_source, api::resource_usage::render_target };
		const api::resource_usage state_final[2] = { api::resource_usage::present, api::resource_usage::resolve_dest };

		if (_device->get_api() == api::device_api::d3d10 ||
			_device->get_api() == api::device_api::d3d11 ||
			_device->get_api() == api::device_api::d3d12)
		{
			const api::resource_usage state_new[2] = { api::resource_usage::render_target, api::resource_usage::shader_resource };

			cmd_list->barrier(2, resources, state_old, state_new);

			cmd_list->bind_pipeline(api::pipeline_stage::all_graphics, _copy_pipeline);

			cmd_list->push_descriptors(api::shader_stage::pixel, _copy_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler, &_copy_sampler_state });
			cmd_list->push_descriptors(api::shader_stage::pixel, _copy_pipeline_layout, 1, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::shader_resource_view, &_back_buffer_resolved_srv });

			const api::viewport viewport = { 0.0f, 0.0f, static_cast<float>(_width), static_cast<float>(_height), 0.0f, 1.0f };
			cmd_list->bind_viewports(0, 1, &viewport);
			const api::rect scissor_rect = { 0, 0, static_cast<int32_t>(_width), static_cast<int32_t>(_height) };
			cmd_list->bind_scissor_rects(0, 1, &scissor_rect);

			const bool srgb_write_enable = (_back_buffer_format == api::format::r8g8b8a8_unorm_srgb || _back_buffer_format == api::format::b8g8r8a8_unorm_srgb);
			cmd_list->bind_render_targets_and_depth_stencil(1, &_back_buffer_targets[back_buffer_index + srgb_write_enable]);

			cmd_list->draw(3, 1, 0, 0);

			cmd_list->barrier(2, resources, state_new, state_final);
		}
		else
		{
			const api::resource_usage state_new[2] = { api::resource_usage::copy_dest, api::resource_usage::copy_source };

			cmd_list->barrier(2, resources, state_old, state_new);
			cmd_list->copy_texture_region(_back_buffer_resolved, 0, nullptr, back_buffer_resource, 0, nullptr);
			cmd_list->barrier(2, resources, state_new, state_final);
		}
	}

	// Apply previous state from application
	apply_state(cmd_list, _app_state);

	if (present_queue != _graphics_queue)
	{
		_queue_sync_value++;

		if (_graphics_queue->signal(_queue_sync_fence, _queue_sync_value))
			present_queue->wait(_queue_sync_fence, _queue_sync_value);
	}

	// Update input status
	if (_input != nullptr)
		_input->next_frame();
	if (_input_gamepad != nullptr)
		_input_gamepad->next_frame();

#if RESHADE_ADDON == 1
	// Detect high network traffic
	extern volatile long g_network_traffic;

	static int cooldown = 0, traffic = 0;
	if (cooldown-- > 0)
	{
		traffic += g_network_traffic > 0;
	}
	else
	{
		const bool was_enabled = addon_enabled;
		addon_enabled = traffic < 10;
		traffic = 0;
		cooldown = 60;

		if (addon_enabled != was_enabled)
		{
			if (was_enabled)
				_backup_texture_semantic_bindings = _texture_semantic_bindings;

			for (const auto &info : _backup_texture_semantic_bindings)
			{
				if (info.second.first == _effect_permutations[0].color_srv[0] && info.second.second == _effect_permutations[0].color_srv[1])
					continue;

				update_texture_bindings(info.first.c_str(), addon_enabled ? info.second.first : api::resource_view { 0 }, addon_enabled ? info.second.second : api::resource_view { 0 });
			}
		}
	}

	if (std::numeric_limits<long>::max() != g_network_traffic)
		g_network_traffic = 0;
#endif
}

static std::string expand_macro_string(const std::string &input, std::vector<std::pair<std::string, std::string>> macros, std::chrono::system_clock::time_point now)
{
	const auto now_seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);

	char timestamp[21];
	const std::time_t t = std::chrono::system_clock::to_time_t(now_seconds);
	struct tm tm; localtime_s(&tm, &t);

	std::snprintf(timestamp, std::size(timestamp), "%.4d-%.2d-%.2d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
	macros.emplace_back("Date", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.4d", tm.tm_year + 1900);
	macros.emplace_back("DateYear", timestamp);
	macros.emplace_back("Year", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.2d", tm.tm_mon + 1);
	macros.emplace_back("DateMonth", timestamp);
	macros.emplace_back("Month", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.2d", tm.tm_mday);
	macros.emplace_back("DateDay", timestamp);
	macros.emplace_back("Day", timestamp);

	std::snprintf(timestamp, std::size(timestamp), "%.2d-%.2d-%.2d", tm.tm_hour, tm.tm_min, tm.tm_sec);
	macros.emplace_back("Time", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.2d", tm.tm_hour);
	macros.emplace_back("TimeHour", timestamp);
	macros.emplace_back("Hour", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.2d", tm.tm_min);
	macros.emplace_back("TimeMinute", timestamp);
	macros.emplace_back("Minute", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.2d", tm.tm_sec);
	macros.emplace_back("TimeSecond", timestamp);
	macros.emplace_back("Second", timestamp);
	std::snprintf(timestamp, std::size(timestamp), "%.3lld", std::chrono::duration_cast<std::chrono::milliseconds>(now - now_seconds).count());
	macros.emplace_back("TimeMillisecond", timestamp);
	macros.emplace_back("Millisecond", timestamp);
	macros.emplace_back("TimeMS", timestamp);

	std::string result;

	for (size_t offset = 0, macro_beg, macro_end; offset < input.size(); offset = macro_end + 1)
	{
		macro_beg = input.find('%', offset);
		macro_end = input.find('%', macro_beg + 1);

		if (macro_beg == std::string::npos || macro_end == std::string::npos)
		{
			result += input.substr(offset);
			break;
		}
		else
		{
			result += input.substr(offset, macro_beg - offset);

			if (macro_end == macro_beg + 1) // Handle case of %% to escape percentage symbol
			{
				result += '%';
				continue;
			}
		}

		std::string_view replacing(input);
		replacing = replacing.substr(macro_beg + 1, macro_end - (macro_beg + 1));
		size_t colon_pos = replacing.find(':');

		std::string name;
		if (colon_pos == std::string_view::npos)
			name = replacing;
		else
			name = replacing.substr(0, colon_pos);

		std::string value;
		for (const std::pair<std::string, std::string> &macro : macros)
		{
			if (_stricmp(name.c_str(), macro.first.c_str()) == 0)
			{
				value = macro.second;
				break;
			}
		}

		if (colon_pos == std::string_view::npos)
		{
			result += value;
		}
		else
		{
			std::string_view param = replacing.substr(colon_pos + 1);

			if (const size_t insert_pos = param.find('$');
				insert_pos != std::string_view::npos)
			{
				result += param.substr(0, insert_pos);
				result += value;
				result += param.substr(insert_pos + 1);
			}
			else
			{
				result += param;
			}
		}
	}

	return result;
}
