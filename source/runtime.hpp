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

		/// <summary>
		/// Gets the path to the configuration file used by this effect runtime.
		/// </summary>
		const std::filesystem::path &get_config_path() const { return _config_path; }

		/// <summary>
		/// Gets a boolean indicating whether effects are being loaded.
		/// </summary>
		bool is_loading() const { return _reload_remaining_effects != std::numeric_limits<size_t>::max() || !_reload_create_queue.empty(); }

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

#if RESHADE_ADDON
		bool _is_in_api_call = false;
		bool _is_in_present_call = false;
#endif

		#pragma region Status
		static unsigned int s_latest_version[3];

		bool _is_initialized = false;
		bool _preset_save_successful = true;
		std::filesystem::path _config_path;

		bool _ignore_shortcuts = false;
		bool _force_shortcut_modifiers = true;
		std::shared_ptr<class input> _input;
		std::shared_ptr<class input_gamepad> _input_gamepad;

		bool _effects_enabled = true;
		bool _effects_rendered_this_frame = false;
		unsigned int _effects_key_data[4] = {};

		std::chrono::system_clock::time_point _current_time;
		uint64_t _frame_count = 0;
		std::chrono::high_resolution_clock::duration _last_frame_duration;
		std::chrono::high_resolution_clock::time_point _start_time, _last_present_time;
		#pragma endregion

		#pragma region Effect Loading
		bool _no_debug_info = true;
		bool _no_effect_cache = false;
		bool _no_reload_on_init = false;
		bool _performance_mode = false;
		bool _effect_load_skipping = false;
		unsigned int _reload_key_data[4] = {};
		unsigned int _performance_mode_key_data[4] = {};

		std::vector<std::pair<std::string, std::string>> _global_preprocessor_definitions;
		std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> _preset_preprocessor_definitions;
		std::vector<std::pair<size_t, size_t>> _reload_required_effects;

		std::filesystem::path _effect_cache_path;
		std::vector<std::filesystem::path> _effect_search_paths;
		std::vector<std::filesystem::path> _texture_search_paths;

		std::atomic<bool> _last_reload_successful = true;
		std::shared_mutex _reload_mutex;
		std::vector<std::pair<size_t, size_t>> _reload_create_queue;
		std::atomic<size_t> _reload_remaining_effects = std::numeric_limits<size_t>::max();
		void *_d3d_compiler_module = nullptr;

		std::vector<std::thread> _worker_threads;
		std::chrono::high_resolution_clock::time_point _last_reload_time;
		#pragma endregion

		#pragma region Effect Rendering
		struct effect_permutation
		{
			unsigned int width = 0;
			unsigned int height = 0;
			api::color_space color_space = api::color_space::unknown;
			api::format color_format = api::format::unknown;
			api::resource color_tex = {};
			api::resource_view color_srv[2] = {};
			api::format stencil_format = api::format::unknown;
			api::resource stencil_tex = {};
			api::resource_view stencil_dsv = {};
		};
		std::vector<effect_permutation> _effect_permutations;

		api::resource _empty_tex = {};
		api::resource_view _empty_srv = {};

		std::unordered_map<size_t, api::sampler> _effect_sampler_states;
		std::unordered_map<std::string, std::pair<api::resource_view, api::resource_view>> _texture_semantic_bindings;
#if RESHADE_ADDON == 1
		std::unordered_map<std::string, std::pair<api::resource_view, api::resource_view>> _backup_texture_semantic_bindings;
#endif
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

		#pragma region Screenshot
		bool _screenshot_save_before = false;
		bool _screenshot_include_preset = false;
#if RESHADE_GUI
		bool _screenshot_save_gui = false;
#endif
		bool _screenshot_clear_alpha = true;
		unsigned int _screenshot_count = 0;
		unsigned int _screenshot_format = 1;
		unsigned int _screenshot_hdr_bits = 11;
		unsigned int _screenshot_jpeg_quality = 90;
		unsigned int _screenshot_key_data[4] = {};
		std::filesystem::path _screenshot_sound_path;
		std::filesystem::path _screenshot_path;
		std::string _screenshot_name;
		std::filesystem::path _screenshot_post_save_command;
		std::string _screenshot_post_save_command_arguments;
		std::filesystem::path _screenshot_post_save_command_working_directory;
		bool _screenshot_post_save_command_hide_window = false;

		bool _should_save_screenshot = false;
		std::atomic<bool> _last_screenshot_save_successful = true;
		bool _screenshot_directory_creation_successful = true;
		std::filesystem::path _last_screenshot_file;
		std::chrono::high_resolution_clock::time_point _last_screenshot_time;
		#pragma endregion

		#pragma region Preset Switching
		unsigned int _prev_preset_key_data[4] = {};
		unsigned int _next_preset_key_data[4] = {};
		unsigned int _preset_transition_duration = 1000;
		std::filesystem::path _startup_preset_path;
		std::filesystem::path _current_preset_path;

		bool _is_in_preset_transition = false;
		std::chrono::high_resolution_clock::time_point _last_preset_switching_time;

		struct preset_shortcut
		{
			std::filesystem::path preset_path;
			unsigned int key_data[4] = {};
		};
		std::vector<preset_shortcut> _preset_shortcuts;
		#pragma endregion

#if RESHADE_GUI
		void init_gui();
		void deinit_gui();
		void build_font_atlas();

		void load_config_gui(const ini_file &config);
		void save_config_gui(ini_file &config) const;

		void load_custom_style();
		void save_custom_style() const;

		void draw_gui();

		bool init_imgui_resources();
		void render_imgui_draw_data(api::command_list *cmd_list, ImDrawData *draw_data, api::resource_view rtv);
		void destroy_imgui_resources();

		#pragma region Overlay
		ImGuiContext *_imgui_context = nullptr;

		bool _show_splash = true;
		bool _show_overlay = false;
		unsigned int _show_fps = 2;
		unsigned int _show_clock = false;
		unsigned int _show_frametime = false;
		unsigned int _show_preset_name = false;
		bool _show_screenshot_message = true;
		bool _show_preset_transition_message = true;
		unsigned int _reload_count = 0;

		bool _is_font_scaling = false;
		bool _no_font_scaling = false;
		bool _block_input_next_frame = false;
		unsigned int _overlay_key_data[4];
		unsigned int _fps_key_data[4] = {};
		unsigned int _frametime_key_data[4] = {};
		unsigned int _fps_pos = 1;
		unsigned int _clock_format = 0;
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

		#pragma region Overlay Home
		char _effect_filter[32] = {};
		bool _variable_editor_tabs = false;
		bool _auto_save_preset = true;
		bool _preset_is_modified = false;
		bool _inherit_current_preset = false;
		std::filesystem::path _template_preset_path;
		bool _was_preprocessor_popup_edited = false;
		size_t _focused_effect = std::numeric_limits<size_t>::max();
		size_t _selected_technique = std::numeric_limits<size_t>::max();
		unsigned int _tutorial_index = 0;
		unsigned int _effects_expanded_state = 2;
		float _variable_editor_height = 200.0f;
		#pragma endregion

		#pragma region Overlay Add-ons
		char _addons_filter[32] = {};
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

		#pragma region Overlay Statistics
		bool _gather_gpu_statistics = false;
		api::resource_view _preview_texture = {};
		unsigned int _preview_size[3] = { 0, 0, 0xFFFFFFFF };
		uint64_t _timestamp_frequency = 0;
		#pragma endregion

		#pragma region Overlay Log
		char _log_filter[32] = {};
		bool _log_wordwrap = false;
		uintmax_t _last_log_size;
		std::vector<std::string> _log_lines;
		#pragma endregion

		#pragma region Overlay Code Editor
		struct editor_instance
		{
			size_t effect_index;
			size_t permutation_index;
			std::filesystem::path file_path;
			std::string entry_point_name;
			bool selected = false;
			bool generated = false;
			imgui::code_editor editor;
		};

		std::vector<editor_instance> _editors;
		uint32_t _editor_palette[imgui::code_editor::color_palette_max];
		#pragma endregion
#endif
	};
}
