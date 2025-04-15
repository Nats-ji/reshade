/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#pragma once

#include "reshade_api.hpp"
#include "state_block.hpp"
#include "imgui_code_editor.hpp"
#include <chrono>
#include <memory>
#include <filesystem>
#include <atomic>
#include <shared_mutex>

class ini_file;
namespace reshadefx { struct sampler_desc; }

namespace reshade
{
	struct effect;
	struct uniform;
	struct texture;
	struct technique;

	/// <summary>
	/// The main ReShade post-processing effect runtime.
	/// </summary>
	class __declspec(uuid("77FF8202-5BEC-42AD-8CE0-397F3E84EAA6")) runtime : public api::effect_runtime
	{
	public:
		runtime(api::swapchain *swapchain, api::command_queue *graphics_queue, const std::filesystem::path &config_path);
		~runtime();

		bool on_init();
		void on_reset();
		void on_present(api::command_queue *present_queue);

		uint64_t get_native() const final { return _swapchain->get_native(); }

		void get_private_data(const uint8_t guid[16], uint64_t *data) const final { return _swapchain->get_private_data(guid, data); }
		void set_private_data(const uint8_t guid[16], const uint64_t data)  final { return _swapchain->set_private_data(guid, data); }

		api::device *get_device() final { return _device; }
		api::swapchain *get_swapchain() { return _swapchain; }
		api::command_queue *get_command_queue() final { return _graphics_queue; }

		void *get_hwnd() const final { return _swapchain->get_hwnd(); }

		api::resource get_back_buffer(uint32_t index) final { return _swapchain->get_back_buffer(index); }
		uint32_t get_back_buffer_count() const final { return _swapchain->get_back_buffer_count(); }
		uint32_t get_current_back_buffer_index() const final { return _swapchain->get_current_back_buffer_index(); }

		void get_screenshot_width_and_height(uint32_t *out_width, uint32_t *out_height) const final { *out_width = _width; *out_height = _height; }

		bool is_key_down(uint32_t keycode) const final;
		bool is_key_pressed(uint32_t keycode) const final;
		bool is_key_released(uint32_t keycode) const final;
		bool is_mouse_button_down(uint32_t button) const final;
		bool is_mouse_button_pressed(uint32_t button) const final;
		bool is_mouse_button_released(uint32_t button) const final;

		uint32_t last_key_pressed() const final;
		uint32_t last_key_released() const final;

		void get_mouse_cursor_position(uint32_t *out_x, uint32_t *out_y, int16_t *out_wheel_delta) const final;

		void block_input_next_frame() final;

		bool open_overlay(bool open, api::input_source source) final;

	private:
		api::swapchain *const _swapchain;
		api::device *const _device;
		api::command_queue *const _graphics_queue;
		unsigned int _width = 0;
		unsigned int _height = 0;
		unsigned int _vendor_id = 0;
		unsigned int _device_id = 0;
		unsigned int _renderer_id = 0;
		uint16_t _back_buffer_samples = 1;
		api::format _back_buffer_format = api::format::unknown;
		api::color_space _back_buffer_color_space = api::color_space::unknown;

		#pragma region Status

		bool _is_initialized = false;

		bool _ignore_shortcuts = false;
		bool _force_shortcut_modifiers = true;
		std::shared_ptr<class input> _input;
		std::shared_ptr<class input_gamepad> _input_gamepad;

		std::chrono::system_clock::time_point _current_time;
		uint64_t _frame_count = 0;
		std::chrono::high_resolution_clock::duration _last_frame_duration;
		std::chrono::high_resolution_clock::time_point _start_time, _last_present_time;
		#pragma endregion

		#pragma region Effect Loading
		void *_d3d_compiler_module = nullptr;

		std::vector<std::thread> _worker_threads;
		std::chrono::high_resolution_clock::time_point _last_reload_time;
		#pragma endregion

		#pragma region Effect Rendering
		api::resource _empty_tex = {};
		api::resource_view _empty_srv = {};

		api::pipeline _copy_pipeline = {};
		api::pipeline_layout _copy_pipeline_layout = {};
		api::sampler  _copy_sampler_state = {};

		api::resource _back_buffer_resolved = {};
		api::resource_view _back_buffer_resolved_srv = {};
		std::vector<api::resource_view> _back_buffer_targets;

		api::state_block _app_state = {};

		api::fence _queue_sync_fence = {};
		uint64_t _queue_sync_value = 0;
		#pragma endregion

#if RESHADE_GUI
		void init_gui();
		void deinit_gui();
		void build_font_atlas();

		void draw_gui();

		bool init_imgui_resources();
		void render_imgui_draw_data(api::command_list *cmd_list, ImDrawData *draw_data, api::resource_view rtv);
		void destroy_imgui_resources();

		#pragma region Overlay
		ImGuiContext *_imgui_context = nullptr;

		bool _show_overlay = false;
		bool _is_font_scaling = false;
		bool _no_font_scaling = false;
		bool _block_input_next_frame = false;
		unsigned int _overlay_key_data[4];
		unsigned int _input_processing_mode = 2;

		api::resource _font_atlas_tex = {};
		api::resource_view _font_atlas_srv = {};

		api::pipeline _imgui_pipeline = {};
		api::pipeline_layout _imgui_pipeline_layout = {};
		api::sampler  _imgui_sampler_state = {};

		int _imgui_num_indices[4] = {};
		api::resource _imgui_indices[4] = {};
		int _imgui_num_vertices[4] = {};
		api::resource _imgui_vertices[4] = {};
		#pragma endregion

		#pragma region Overlay Settings
		std::string _selected_language, _current_language;
		int _font_size = 0;
		int _editor_font_size = 0;
		int _style_index = 2;
		int _editor_style_index = 0;
		std::filesystem::path _font_path, _default_font_path;
		std::filesystem::path _latin_font_path;
		std::filesystem::path _editor_font_path, _default_editor_font_path;
		std::filesystem::path _file_selection_path;
		float _fps_col[4] = { 1.0f, 1.0f, 0.784314f, 1.0f };
		float _fps_scale = 1.0f;
		float _hdr_overlay_brightness = 203.f; // HDR reference white as per BT.2408
		api::color_space _hdr_overlay_overwrite_color_space = api::color_space::unknown;
		bool  _show_force_load_effects_button = true;
		#pragma endregion
#endif
	};
}
