/*
 * Copyright (C) 2014 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#if RESHADE_GUI

#include "runtime.hpp"
#include "runtime_internal.hpp"
#include "version.h"
#include "dll_log.hpp"
#include "dll_resources.hpp"
#include "ini_file.hpp"
#include "addon_manager.hpp"
#include "input.hpp"
#include "input_gamepad.hpp"
#include "imgui_widgets.hpp"
#include "localization.hpp"
#include "platform_utils.hpp"
#include "fonts/forkawesome.inl"
#include "fonts/glyph_ranges.hpp"
#include <cmath> // std::abs, std::ceil, std::floor
#include <cctype> // std::tolower
#include <cstdlib> // std::lldiv, std::strtol
#include <cstring> // std::memcmp, std::memcpy
#include <algorithm> // std::any_of, std::count_if, std::find, std::find_if, std::max, std::min, std::replace, std::rotate, std::search, std::swap, std::transform

extern bool resolve_path(std::filesystem::path &path, std::error_code &ec);

static bool string_contains(const std::string_view text, const std::string_view filter)
{
	return filter.empty() ||
		std::search(text.cbegin(), text.cend(), filter.cbegin(), filter.cend(),
			[](const char c1, const char c2) { // Search case-insensitive
				return (('a' <= c1 && c1 <= 'z') ? static_cast<char>(c1 - ' ') : c1) == (('a' <= c2 && c2 <= 'z') ? static_cast<char>(c2 - ' ') : c2);
			}) != text.cend();
}
static auto is_invalid_path_element(ImGuiInputTextCallbackData *data) -> int
{
	return data->EventChar == L'\"' || data->EventChar == L'*' || data->EventChar == L':' || data->EventChar == L'<' || data->EventChar == L'>' || data->EventChar == L'?' || data->EventChar == L'|';
}
static auto is_invalid_filename_element(ImGuiInputTextCallbackData *data) -> int
{
	// A file name cannot contain any of the following characters
	return is_invalid_path_element(data) || data->EventChar == L'/' || data->EventChar == L'\\';
}

template <typename F>
static void parse_errors(const std::string_view errors, F &&callback)
{
	for (size_t offset = 0, next; offset != std::string_view::npos; offset = next)
	{
		const size_t pos_error = errors.find(": ", offset);
		const size_t pos_error_line = errors.rfind('(', pos_error); // Paths can contain '(', but no ": ", so search backwards from the error location to find the line info
		if (pos_error == std::string_view::npos || pos_error_line == std::string_view::npos || pos_error_line < offset)
			break;

		const size_t pos_linefeed = errors.find('\n', pos_error);

		next = pos_linefeed != std::string_view::npos ? pos_linefeed + 1 : std::string_view::npos;

		const std::string_view error_file = errors.substr(offset, pos_error_line - offset);
		int error_line = static_cast<int>(std::strtol(errors.data() + pos_error_line + 1, nullptr, 10));
		const std::string_view error_text = errors.substr(pos_error + 2 /* skip space */, pos_linefeed - pos_error - 2);

		callback(error_file, error_line, error_text);
	}
}

template <typename T>
static std::string_view get_localized_annotation(T &object, const std::string_view ann_name, [[maybe_unused]] std::string language)
{
#if RESHADE_LOCALIZATION
	if (language.size() >= 2)
	{
		// Transform language name from e.g. 'en-US' to 'en_us'
		std::replace(language.begin(), language.end(), '-', '_');
		std::transform(language.begin(), language.end(), language.begin(),
			[](std::string::value_type c) {
				return static_cast<std::string::value_type>(std::tolower(c));
			});

		for (int attempt = 0; attempt < 2; ++attempt)
		{
			const std::string_view localized_result = object.annotation_as_string(std::string(ann_name) + '_' + language);
			if (!localized_result.empty())
				return localized_result;
			else if (attempt == 0)
				language.erase(2); // Remove location information from language name, so that it e.g. becomes 'en'
		}
	}
#endif

	return object.annotation_as_string(ann_name);
}

static const ImVec4 COLOR_RED = ImColor(240, 100, 100);
static const ImVec4 COLOR_YELLOW = ImColor(204, 204, 0);

void reshade::runtime::init_gui()
{
	// Default shortcut: Home
	_overlay_key_data[0] = 0x24;
	_overlay_key_data[1] = false;
	_overlay_key_data[2] = false;
	_overlay_key_data[3] = false;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	_imgui_context = ImGui::CreateContext();

	ImGuiIO &imgui_io = _imgui_context->IO;
	imgui_io.IniFilename = nullptr;
	imgui_io.ConfigFlags = ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
	imgui_io.BackendFlags = ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_RendererHasVtxOffset;

	ImGuiStyle &imgui_style = _imgui_context->Style;
	// Disable rounding by default
	imgui_style.GrabRounding = 0.0f;
	imgui_style.FrameRounding = 0.0f;
	imgui_style.ChildRounding = 0.0f;
	imgui_style.ScrollbarRounding = 0.0f;
	imgui_style.WindowRounding = 0.0f;
	imgui_style.WindowBorderSize = 0.0f;

	// Restore previous context in case this was called from a new runtime being created from an add-on event triggered by an existing runtime
	ImGui::SetCurrentContext(backup_context);
}
void reshade::runtime::deinit_gui()
{
	ImGui::DestroyContext(_imgui_context);
}

void reshade::runtime::build_font_atlas()
{
	ImFontAtlas *const atlas = _imgui_context->IO.Fonts;

	if (atlas->IsBuilt())
		return;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	// Remove any existing fonts from atlas first
	atlas->Clear();

	std::error_code ec;
	const ImWchar *glyph_ranges = nullptr;
	std::filesystem::path resolved_font_path;

#if RESHADE_LOCALIZATION
	std::string language = _selected_language;
	if (language.empty())
		language = resources::get_current_language();

	if (language.find("bg") == 0 || language.find("ru") == 0)
	{
		glyph_ranges = atlas->GetGlyphRangesCyrillic();

		_default_font_path = L"C:\\Windows\\Fonts\\calibri.ttf";
	}
	else
	if (language.find("ja") == 0)
	{
		glyph_ranges = atlas->GetGlyphRangesJapanese();

		// Morisawa BIZ UDGothic Regular, available since Windows 10 October 2018 Update (1809) Build 17763.1
		_default_font_path = L"C:\\Windows\\Fonts\\BIZ-UDGothicR.ttc";
		if (!std::filesystem::exists(_default_font_path, ec))
			_default_font_path = L"C:\\Windows\\Fonts\\msgothic.ttc"; // MS Gothic
	}
	else
	if (language.find("ko") == 0)
	{
		glyph_ranges = atlas->GetGlyphRangesKorean();

		_default_font_path = L"C:\\Windows\\Fonts\\malgun.ttf"; // Malgun Gothic
	}
	else
	if (language.find("zh") == 0)
	{
		glyph_ranges = GetGlyphRangesChineseSimplifiedGB2312();

		_default_font_path = L"C:\\Windows\\Fonts\\msyh.ttc"; // Microsoft YaHei
		if (!std::filesystem::exists(_default_font_path, ec))
			_default_font_path = L"C:\\Windows\\Fonts\\simsun.ttc"; // SimSun
	}
	else
#endif
	{
		glyph_ranges = atlas->GetGlyphRangesDefault();

		_default_font_path.clear();
	}

	const auto add_font_from_file = [atlas](std::filesystem::path &font_path, ImFontConfig cfg, const ImWchar *glyph_ranges, std::error_code &ec) -> bool {
		if (font_path.empty())
			return true;

		if (!resolve_path(font_path, ec))
			return false;

		if (FILE *const file = _wfsopen(font_path.c_str(), L"rb", SH_DENYNO))
		{
			fseek(file, 0, SEEK_END);
			const size_t data_size = ftell(file);
			fseek(file, 0, SEEK_SET);

			void *data = IM_ALLOC(data_size);
			const size_t data_size_read = fread(data, 1, data_size, file);
			fclose(file);
			if (data_size_read != data_size)
			{
				IM_FREE(data);
				return false;
			}

			ImFormatString(cfg.Name, IM_ARRAYSIZE(cfg.Name), "%s, %.0fpx", font_path.stem().u8string().c_str(), cfg.SizePixels);

			return atlas->AddFontFromMemoryTTF(data, static_cast<int>(data_size), cfg.SizePixels, &cfg, glyph_ranges) != nullptr;
		}

		return false;
	};

	ImFontConfig cfg;
	cfg.GlyphOffset.y = std::floor(_font_size / 13.0f); // Not used in AddFontDefault()
	cfg.SizePixels = static_cast<float>(_font_size);

#if RESHADE_LOCALIZATION
	// Add latin font
	resolved_font_path = _latin_font_path;
	if (!_default_font_path.empty())
	{
		if (!add_font_from_file(resolved_font_path, cfg, atlas->GetGlyphRangesDefault(), ec))
		{
			log::message(log::level::error, "Failed to load latin font from '%s' with error code %d!", resolved_font_path.u8string().c_str(), ec.value());
			resolved_font_path.clear();
		}

		if (resolved_font_path.empty())
			atlas->AddFontDefault(&cfg);

		cfg.MergeMode = true;
	}
#endif

	// Add main font
	resolved_font_path = _font_path.empty() ? _default_font_path : _font_path;
	{
		if (!add_font_from_file(resolved_font_path, cfg, glyph_ranges, ec))
		{
			log::message(log::level::error, "Failed to load font from '%s' with error code %d!", resolved_font_path.u8string().c_str(), ec.value());
			resolved_font_path.clear();
		}

		// Use default font if custom font failed to load
		if (resolved_font_path.empty())
			atlas->AddFontDefault(&cfg);

		// Merge icons into main font
		cfg.MergeMode = true;
		cfg.PixelSnapH = true;

		// This need to be static so that it doesn't fall out of scope before the atlas is built below
		static constexpr ImWchar icon_ranges[] = { ICON_MIN_FK, ICON_MAX_FK, 0 }; // Zero-terminated list

		atlas->AddFontFromMemoryCompressedBase85TTF(FONT_ICON_BUFFER_NAME_FK, cfg.SizePixels, &cfg, icon_ranges);
	}

	// Add editor font
	resolved_font_path = _editor_font_path.empty() ? _default_editor_font_path : _editor_font_path;
	if (resolved_font_path != _font_path || _editor_font_size != _font_size)
	{
		cfg = ImFontConfig();
		cfg.SizePixels = static_cast<float>(_editor_font_size);

		if (!add_font_from_file(resolved_font_path, cfg, glyph_ranges, ec))
		{
			log::message(log::level::error, "Failed to load editor font from '%s' with error code %d!", resolved_font_path.u8string().c_str(), ec.value());
			resolved_font_path.clear();
		}

		if (resolved_font_path.empty())
			atlas->AddFontDefault(&cfg);
	}

	if (atlas->Build())
	{
#if RESHADE_VERBOSE_LOG
		log::message(log::level::debug, "Font atlas size: %dx%d", atlas->TexWidth, atlas->TexHeight);
#endif
	}
	else
	{
		log::message(log::level::error, "Failed to build font atlas!");

		_font_path.clear();
		_latin_font_path.clear();
		_editor_font_path.clear();

		atlas->Clear();

		// If unable to build font atlas due to an invalid custom font, revert to the default font
		for (int i = 0; i < (_editor_font_size != _font_size ? 2 : 1); ++i)
		{
			cfg = ImFontConfig();
			cfg.SizePixels = static_cast<float>(i == 0 ? _font_size : _editor_font_size);

			atlas->AddFontDefault(&cfg);
		}
	}

	ImGui::SetCurrentContext(backup_context);

	_show_splash = true;

	int width, height;
	unsigned char *pixels;
	// This will also build the font atlas again if that previously failed above
	atlas->GetTexDataAsRGBA32(&pixels, &width, &height);

	// Make sure font atlas is not currently in use before destroying it
	_graphics_queue->wait_idle();

	_device->destroy_resource(_font_atlas_tex);
	_font_atlas_tex = {};
	_device->destroy_resource_view(_font_atlas_srv);
	_font_atlas_srv = {};

	const api::subresource_data initial_data = { pixels, static_cast<uint32_t>(width * 4), static_cast<uint32_t>(width * height * 4) };

	// Create font atlas texture and upload it
	if (!_device->create_resource(
			api::resource_desc(width, height, 1, 1, api::format::r8g8b8a8_unorm, 1, api::memory_heap::gpu_only, api::resource_usage::shader_resource),
			&initial_data, api::resource_usage::shader_resource, &_font_atlas_tex))
	{
		log::message(log::level::error, "Failed to create front atlas resource!");
		return;
	}

	// Texture data is now uploaded, so can free the memory
	atlas->ClearTexData();

	if (!_device->create_resource_view(_font_atlas_tex, api::resource_usage::shader_resource, api::resource_view_desc(api::format::r8g8b8a8_unorm), &_font_atlas_srv))
	{
		log::message(log::level::error, "Failed to create font atlas resource view!");
		return;
	}

	_device->set_resource_name(_font_atlas_tex, "ImGui font atlas");
}

void reshade::runtime::load_config_gui(const ini_file &config)
{
	if (_input_gamepad != nullptr)
		_imgui_context->IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
	else
		_imgui_context->IO.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;

	const auto config_get = [&config](const std::string &section, const std::string &key, auto &values) {
		if (config.get(section, key, values))
			return true;
		// Fall back to global configuration when an entry does not exist in the local configuration
		return global_config().get(section, key, values);
	};

	config_get("INPUT", "KeyOverlay", _overlay_key_data);
	config_get("INPUT", "KeyFPS", _fps_key_data);
	config_get("INPUT", "KeyFrameTime", _frametime_key_data);
	config_get("INPUT", "InputProcessing", _input_processing_mode);

#if RESHADE_LOCALIZATION
	config_get("OVERLAY", "Language", _selected_language);
#endif

	config.get("OVERLAY", "ClockFormat", _clock_format);
	config.get("OVERLAY", "FPSPosition", _fps_pos);
	config.get("OVERLAY", "NoFontScaling", _no_font_scaling);
	config.get("OVERLAY", "ShowClock", _show_clock);
	config.get("OVERLAY", "ShowForceLoadEffectsButton", _show_force_load_effects_button);
	config.get("OVERLAY", "ShowFPS", _show_fps);
	config.get("OVERLAY", "ShowFrameTime", _show_frametime);
	config.get("OVERLAY", "ShowPresetName", _show_preset_name);
	config.get("OVERLAY", "ShowScreenshotMessage", _show_screenshot_message);
	if (!global_config().get("OVERLAY", "TutorialProgress", _tutorial_index))
		config.get("OVERLAY", "TutorialProgress", _tutorial_index);
	config.get("OVERLAY", "VariableListHeight", _variable_editor_height);
	config.get("OVERLAY", "VariableListUseTabs", _variable_editor_tabs);
	config.get("OVERLAY", "AutoSavePreset", _auto_save_preset);
	config.get("OVERLAY", "ShowPresetTransitionMessage", _show_preset_transition_message);

	ImGuiStyle &imgui_style = _imgui_context->Style;
	config.get("STYLE", "Alpha", imgui_style.Alpha);
	config.get("STYLE", "ChildRounding", imgui_style.ChildRounding);
	config.get("STYLE", "ColFPSText", _fps_col);
	config.get("STYLE", "EditorFont", _editor_font_path);
	config.get("STYLE", "EditorFontSize", _editor_font_size);
	config.get("STYLE", "EditorStyleIndex", _editor_style_index);
	config.get("STYLE", "Font", _font_path);
	config.get("STYLE", "FontSize", _font_size);
	config.get("STYLE", "FPSScale", _fps_scale);
	config.get("STYLE", "FrameRounding", imgui_style.FrameRounding);
	config.get("STYLE", "GrabRounding", imgui_style.GrabRounding);
	config.get("STYLE", "LatinFont", _latin_font_path);
	config.get("STYLE", "PopupRounding", imgui_style.PopupRounding);
	config.get("STYLE", "ScrollbarRounding", imgui_style.ScrollbarRounding);
	config.get("STYLE", "StyleIndex", _style_index);
	config.get("STYLE", "TabRounding", imgui_style.TabRounding);
	config.get("STYLE", "WindowRounding", imgui_style.WindowRounding);
	config.get("STYLE", "HdrOverlayBrightness", _hdr_overlay_brightness);
	config.get("STYLE", "HdrOverlayOverwriteColorSpaceTo", reinterpret_cast<int &>(_hdr_overlay_overwrite_color_space));

	// For compatibility with older versions, set the alpha value if it is missing
	if (_fps_col[3] == 0.0f)
		_fps_col[3]  = 1.0f;

	load_custom_style();

	if (_imgui_context->SettingsLoaded)
		return;

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	// Call all pre-read handlers, before reading config data (since they affect state that is then updated in the read handlers below)
	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
		if (handler.ReadInitFn)
			handler.ReadInitFn(_imgui_context, &handler);

	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
	{
		if (std::vector<std::string> lines;
			config.get("OVERLAY", handler.TypeName, lines))
		{
			void *entry_data = nullptr;

			for (const std::string &line : lines)
			{
				if (line.empty())
					continue;

				if (line[0] == '[')
				{
					const size_t name_beg = line.find('[', 1) + 1;
					const size_t name_end = line.rfind(']');

					entry_data = handler.ReadOpenFn(_imgui_context, &handler, line.substr(name_beg, name_end - name_beg).c_str());
				}
				else
				{
					assert(entry_data != nullptr);
					handler.ReadLineFn(_imgui_context, &handler, entry_data, line.c_str());
				}
			}
		}
	}

	_imgui_context->SettingsLoaded = true;

	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
		if (handler.ApplyAllFn)
			handler.ApplyAllFn(_imgui_context, &handler);

	ImGui::SetCurrentContext(backup_context);
}
void reshade::runtime::save_config_gui(ini_file &config) const
{
	config.set("INPUT", "KeyOverlay", _overlay_key_data);
	config.set("INPUT", "KeyFPS", _fps_key_data);
	config.set("INPUT", "KeyFrametime", _frametime_key_data);
	config.set("INPUT", "InputProcessing", _input_processing_mode);

#if RESHADE_LOCALIZATION
	config.set("OVERLAY", "Language", _selected_language);
#endif

	config.set("OVERLAY", "ClockFormat", _clock_format);
	config.set("OVERLAY", "FPSPosition", _fps_pos);
	config.set("OVERLAY", "ShowClock", _show_clock);
	config.set("OVERLAY", "ShowForceLoadEffectsButton", _show_force_load_effects_button);
	config.set("OVERLAY", "ShowFPS", _show_fps);
	config.set("OVERLAY", "ShowFrameTime", _show_frametime);
	config.set("OVERLAY", "ShowPresetName", _show_preset_name);
	config.set("OVERLAY", "ShowScreenshotMessage", _show_screenshot_message);
	global_config().set("OVERLAY", "TutorialProgress", _tutorial_index);
	config.set("OVERLAY", "TutorialProgress", _tutorial_index);
	config.set("OVERLAY", "VariableListHeight", _variable_editor_height);
	config.set("OVERLAY", "VariableListUseTabs", _variable_editor_tabs);
	config.set("OVERLAY", "AutoSavePreset", _auto_save_preset);
	config.set("OVERLAY", "ShowPresetTransitionMessage", _show_preset_transition_message);

	const ImGuiStyle &imgui_style = _imgui_context->Style;
	config.set("STYLE", "Alpha", imgui_style.Alpha);
	config.set("STYLE", "ChildRounding", imgui_style.ChildRounding);
	config.set("STYLE", "ColFPSText", _fps_col);
	config.set("STYLE", "EditorFont", _editor_font_path);
	config.set("STYLE", "EditorFontSize", _editor_font_size);
	config.set("STYLE", "EditorStyleIndex", _editor_style_index);
	config.set("STYLE", "Font", _font_path);
	config.set("STYLE", "FontSize", _font_size);
	config.set("STYLE", "FPSScale", _fps_scale);
	config.set("STYLE", "FrameRounding", imgui_style.FrameRounding);
	config.set("STYLE", "GrabRounding", imgui_style.GrabRounding);
	config.set("STYLE", "LatinFont", _latin_font_path);
	config.set("STYLE", "PopupRounding", imgui_style.PopupRounding);
	config.set("STYLE", "ScrollbarRounding", imgui_style.ScrollbarRounding);
	config.set("STYLE", "StyleIndex", _style_index);
	config.set("STYLE", "TabRounding", imgui_style.TabRounding);
	config.set("STYLE", "WindowRounding", imgui_style.WindowRounding);
	config.set("STYLE", "HdrOverlayBrightness", _hdr_overlay_brightness);
	config.set("STYLE", "HdrOverlayOverwriteColorSpaceTo", static_cast<int>(_hdr_overlay_overwrite_color_space));

	// Do not save custom style colors by default, only when actually used and edited

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	for (ImGuiSettingsHandler &handler : _imgui_context->SettingsHandlers)
	{
		ImGuiTextBuffer buffer;
		handler.WriteAllFn(_imgui_context, &handler, &buffer);

		std::vector<std::string> lines;
		for (int i = 0, offset = 0; i < buffer.size(); ++i)
		{
			if (buffer[i] == '\n')
			{
				lines.emplace_back(buffer.c_str() + offset, i - offset);
				offset = i + 1;
			}
		}

		if (!lines.empty())
			config.set("OVERLAY", handler.TypeName, lines);
	}

	ImGui::SetCurrentContext(backup_context);
}

void reshade::runtime::load_custom_style()
{
	const ini_file &config = ini_file::load_cache(_config_path);

	ImVec4 *const colors = _imgui_context->Style.Colors;
	switch (_style_index)
	{
	case 0:
		ImGui::StyleColorsDark(&_imgui_context->Style);
		break;
	case 1:
		ImGui::StyleColorsLight(&_imgui_context->Style);
		break;
	case 2:
		colors[ImGuiCol_Text] = ImVec4(0.862745f, 0.862745f, 0.862745f, 1.00f);
		colors[ImGuiCol_TextDisabled] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.58f);
		colors[ImGuiCol_WindowBg] = ImVec4(0.117647f, 0.117647f, 0.117647f, 1.00f);
		colors[ImGuiCol_ChildBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 0.00f);
		colors[ImGuiCol_Border] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.30f);
		colors[ImGuiCol_FrameBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 1.00f);
		colors[ImGuiCol_FrameBgHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.470588f);
		colors[ImGuiCol_FrameBgActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.588235f);
		colors[ImGuiCol_TitleBg] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.45f);
		colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.35f);
		colors[ImGuiCol_TitleBgActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.58f);
		colors[ImGuiCol_MenuBarBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 0.57f);
		colors[ImGuiCol_ScrollbarBg] = ImVec4(0.156863f, 0.156863f, 0.156863f, 1.00f);
		colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.31f);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.78f);
		colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_PopupBg] = ImVec4(0.117647f, 0.117647f, 0.117647f, 0.92f);
		colors[ImGuiCol_CheckMark] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.80f);
		colors[ImGuiCol_SliderGrab] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.784314f);
		colors[ImGuiCol_SliderGrabActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Button] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.44f);
		colors[ImGuiCol_ButtonHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.86f);
		colors[ImGuiCol_ButtonActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Header] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.76f);
		colors[ImGuiCol_HeaderHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.86f);
		colors[ImGuiCol_HeaderActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Separator] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.32f);
		colors[ImGuiCol_SeparatorHovered] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.78f);
		colors[ImGuiCol_SeparatorActive] = ImVec4(0.862745f, 0.862745f, 0.862745f, 1.00f);
		colors[ImGuiCol_ResizeGrip] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.20f);
		colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.78f);
		colors[ImGuiCol_ResizeGripActive] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_Tab] = colors[ImGuiCol_Button];
		colors[ImGuiCol_TabSelected] = colors[ImGuiCol_ButtonActive];
		colors[ImGuiCol_TabSelectedOverline] = colors[ImGuiCol_ButtonActive];
		colors[ImGuiCol_TabHovered] = colors[ImGuiCol_ButtonHovered];
		colors[ImGuiCol_TabDimmed] = ImLerp(colors[ImGuiCol_Tab], colors[ImGuiCol_TitleBg], 0.80f);
		colors[ImGuiCol_TabDimmedSelected] = ImLerp(colors[ImGuiCol_TabSelected], colors[ImGuiCol_TitleBg], 0.40f);
		colors[ImGuiCol_TabDimmedSelectedOverline] = colors[ImGuiCol_TabDimmedSelected];
		colors[ImGuiCol_DockingPreview] = colors[ImGuiCol_Header] * ImVec4(1.0f, 1.0f, 1.0f, 0.7f);
		colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.20f, 0.20f, 0.20f, 1.00f);
		colors[ImGuiCol_PlotLines] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.63f);
		colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_PlotHistogram] = ImVec4(0.862745f, 0.862745f, 0.862745f, 0.63f);
		colors[ImGuiCol_PlotHistogramHovered] = ImVec4(0.392157f, 0.588235f, 0.941176f, 1.00f);
		colors[ImGuiCol_TextSelectedBg] = ImVec4(0.392157f, 0.588235f, 0.941176f, 0.43f);
		break;
	case 5:
		colors[ImGuiCol_Text] = ImColor(0xff969483);
		colors[ImGuiCol_TextDisabled] = ImColor(0xff756e58);
		colors[ImGuiCol_WindowBg] = ImColor(0xff362b00);
		colors[ImGuiCol_ChildBg] = ImColor();
		colors[ImGuiCol_PopupBg] = ImColor(0xfc362b00); // Customized
		colors[ImGuiCol_Border] = ImColor(0xff423607);
		colors[ImGuiCol_BorderShadow] = ImColor();
		colors[ImGuiCol_FrameBg] = ImColor(0xfc423607); // Customized
		colors[ImGuiCol_FrameBgHovered] = ImColor(0xff423607);
		colors[ImGuiCol_FrameBgActive] = ImColor(0xff423607);
		colors[ImGuiCol_TitleBg] = ImColor(0xff362b00);
		colors[ImGuiCol_TitleBgActive] = ImColor(0xff362b00);
		colors[ImGuiCol_TitleBgCollapsed] = ImColor(0xff362b00);
		colors[ImGuiCol_MenuBarBg] = ImColor(0xff423607);
		colors[ImGuiCol_ScrollbarBg] = ImColor(0xff362b00);
		colors[ImGuiCol_ScrollbarGrab] = ImColor(0xff423607);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(0xff423607);
		colors[ImGuiCol_ScrollbarGrabActive] = ImColor(0xff423607);
		colors[ImGuiCol_CheckMark] = ImColor(0xff756e58);
		colors[ImGuiCol_SliderGrab] = ImColor(0xff5e5025); // Customized
		colors[ImGuiCol_SliderGrabActive] = ImColor(0xff5e5025); // Customized
		colors[ImGuiCol_Button] = ImColor(0xff423607);
		colors[ImGuiCol_ButtonHovered] = ImColor(0xff423607);
		colors[ImGuiCol_ButtonActive] = ImColor(0xff362b00);
		colors[ImGuiCol_Header] = ImColor(0xff423607);
		colors[ImGuiCol_HeaderHovered] = ImColor(0xff423607);
		colors[ImGuiCol_HeaderActive] = ImColor(0xff423607);
		colors[ImGuiCol_Separator] = ImColor(0xff423607);
		colors[ImGuiCol_SeparatorHovered] = ImColor(0xff423607);
		colors[ImGuiCol_SeparatorActive] = ImColor(0xff423607);
		colors[ImGuiCol_ResizeGrip] = ImColor(0xff423607);
		colors[ImGuiCol_ResizeGripHovered] = ImColor(0xff423607);
		colors[ImGuiCol_ResizeGripActive] = ImColor(0xff756e58);
		colors[ImGuiCol_Tab] = ImColor(0xff362b00);
		colors[ImGuiCol_TabHovered] = ImColor(0xff423607);
		colors[ImGuiCol_TabSelected] = ImColor(0xff423607);
		colors[ImGuiCol_TabSelectedOverline] = ImColor(0xff423607);
		colors[ImGuiCol_TabDimmed] = ImColor(0xff362b00);
		colors[ImGuiCol_TabDimmedSelected] = ImColor(0xff423607);
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImColor(0xff423607);
		colors[ImGuiCol_DockingPreview] = ImColor(0xee837b65); // Customized
		colors[ImGuiCol_DockingEmptyBg] = ImColor();
		colors[ImGuiCol_PlotLines] = ImColor(0xff756e58);
		colors[ImGuiCol_PlotLinesHovered] = ImColor(0xff756e58);
		colors[ImGuiCol_PlotHistogram] = ImColor(0xff756e58);
		colors[ImGuiCol_PlotHistogramHovered] = ImColor(0xff756e58);
		colors[ImGuiCol_TextSelectedBg] = ImColor(0xff756e58);
		colors[ImGuiCol_DragDropTarget] = ImColor(0xff756e58);
		colors[ImGuiCol_NavCursor] = ImColor();
		colors[ImGuiCol_NavWindowingHighlight] = ImColor(0xee969483); // Customized
		colors[ImGuiCol_NavWindowingDimBg] = ImColor(0x20e3f6fd); // Customized
		colors[ImGuiCol_ModalWindowDimBg] = ImColor(0x20e3f6fd); // Customized
		break;
	case 6:
		colors[ImGuiCol_Text] = ImColor(0xff837b65);
		colors[ImGuiCol_TextDisabled] = ImColor(0xffa1a193);
		colors[ImGuiCol_WindowBg] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_ChildBg] = ImColor();
		colors[ImGuiCol_PopupBg] = ImColor(0xfce3f6fd); // Customized
		colors[ImGuiCol_Border] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_BorderShadow] = ImColor();
		colors[ImGuiCol_FrameBg] = ImColor(0xfcd5e8ee); // Customized
		colors[ImGuiCol_FrameBgHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_FrameBgActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TitleBg] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TitleBgActive] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TitleBgCollapsed] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_MenuBarBg] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ScrollbarBg] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_ScrollbarGrab] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ScrollbarGrabHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ScrollbarGrabActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_CheckMark] = ImColor(0xffa1a193);
		colors[ImGuiCol_SliderGrab] = ImColor(0xffc3d3d9); // Customized
		colors[ImGuiCol_SliderGrabActive] = ImColor(0xffc3d3d9); // Customized
		colors[ImGuiCol_Button] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ButtonHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ButtonActive] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_Header] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_HeaderHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_HeaderActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_Separator] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_SeparatorHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_SeparatorActive] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ResizeGrip] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ResizeGripHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_ResizeGripActive] = ImColor(0xffa1a193);
		colors[ImGuiCol_Tab] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TabHovered] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabSelected] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabSelectedOverline] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabDimmed] = ImColor(0xffe3f6fd);
		colors[ImGuiCol_TabDimmedSelected] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_TabDimmedSelectedOverline] = ImColor(0xffd5e8ee);
		colors[ImGuiCol_DockingPreview] = ImColor(0xeea1a193); // Customized
		colors[ImGuiCol_DockingEmptyBg] = ImColor();
		colors[ImGuiCol_PlotLines] = ImColor(0xffa1a193);
		colors[ImGuiCol_PlotLinesHovered] = ImColor(0xffa1a193);
		colors[ImGuiCol_PlotHistogram] = ImColor(0xffa1a193);
		colors[ImGuiCol_PlotHistogramHovered] = ImColor(0xffa1a193);
		colors[ImGuiCol_TextSelectedBg] = ImColor(0xffa1a193);
		colors[ImGuiCol_DragDropTarget] = ImColor(0xffa1a193);
		colors[ImGuiCol_NavCursor] = ImColor();
		colors[ImGuiCol_NavWindowingHighlight] = ImColor(0xee837b65); // Customized
		colors[ImGuiCol_NavWindowingDimBg] = ImColor(0x20362b00); // Customized
		colors[ImGuiCol_ModalWindowDimBg] = ImColor(0x20362b00); // Customized
		break;
	default:
		for (ImGuiCol i = 0; i < ImGuiCol_COUNT; i++)
			config.get("STYLE", ImGui::GetStyleColorName(i), (float(&)[4])colors[i]);
		break;
	}

	switch (_editor_style_index)
	{
	case 0: // Dark
		_editor_palette[imgui::code_editor::color_default] = 0xffffffff;
		_editor_palette[imgui::code_editor::color_keyword] = 0xffd69c56;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff00ff00;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff7070e0;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xffffffff;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff409090;
		_editor_palette[imgui::code_editor::color_identifier] = 0xffaaaaaa;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff9bc64d;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc040a0;
		_editor_palette[imgui::code_editor::color_comment] = 0xff206020;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xff406020;
		_editor_palette[imgui::code_editor::color_background] = 0xff101010;
		_editor_palette[imgui::code_editor::color_cursor] = 0xffe0e0e0;
		_editor_palette[imgui::code_editor::color_selection] = 0x80a06020;
		_editor_palette[imgui::code_editor::color_error_marker] = 0x800020ff;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x8000ffff;
		_editor_palette[imgui::code_editor::color_line_number] = 0xff707000;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x40000000;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x40808080;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x40a0a0a0;
		break;
	case 1: // Light
		_editor_palette[imgui::code_editor::color_default] = 0xff000000;
		_editor_palette[imgui::code_editor::color_keyword] = 0xffff0c06;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff008000;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff2020a0;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xff000000;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff409090;
		_editor_palette[imgui::code_editor::color_identifier] = 0xff404040;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff606010;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc040a0;
		_editor_palette[imgui::code_editor::color_comment] = 0xff205020;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xff405020;
		_editor_palette[imgui::code_editor::color_background] = 0xffffffff;
		_editor_palette[imgui::code_editor::color_cursor] = 0xff000000;
		_editor_palette[imgui::code_editor::color_selection] = 0x80600000;
		_editor_palette[imgui::code_editor::color_error_marker] = 0xa00010ff;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x8000ffff;
		_editor_palette[imgui::code_editor::color_line_number] = 0xff505000;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x40000000;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x40808080;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x40000000;
		break;
	case 3: // Solarized Dark
		_editor_palette[imgui::code_editor::color_default] = 0xff969483;
		_editor_palette[imgui::code_editor::color_keyword] = 0xff0089b5;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xff969483;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff164bcb;
		_editor_palette[imgui::code_editor::color_identifier] = 0xff969483;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff969483;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc4716c;
		_editor_palette[imgui::code_editor::color_comment] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_background] = 0xff362b00;
		_editor_palette[imgui::code_editor::color_cursor] = 0xff969483;
		_editor_palette[imgui::code_editor::color_selection] = 0xa0756e58;
		_editor_palette[imgui::code_editor::color_error_marker] = 0x7f2f32dc;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x7f0089b5;
		_editor_palette[imgui::code_editor::color_line_number] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x7f423607;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x7f423607;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x7f423607;
		break;
	case 4: // Solarized Light
		_editor_palette[imgui::code_editor::color_default] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_keyword] = 0xff0089b5;
		_editor_palette[imgui::code_editor::color_number_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_string_literal] = 0xff98a12a;
		_editor_palette[imgui::code_editor::color_punctuation] = 0xff756e58;
		_editor_palette[imgui::code_editor::color_preprocessor] = 0xff164bcb;
		_editor_palette[imgui::code_editor::color_identifier] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_known_identifier] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_preprocessor_identifier] = 0xffc4716c;
		_editor_palette[imgui::code_editor::color_comment] = 0xffa1a193;
		_editor_palette[imgui::code_editor::color_multiline_comment] = 0xffa1a193;
		_editor_palette[imgui::code_editor::color_background] = 0xffe3f6fd;
		_editor_palette[imgui::code_editor::color_cursor] = 0xff837b65;
		_editor_palette[imgui::code_editor::color_selection] = 0x60a1a193;
		_editor_palette[imgui::code_editor::color_error_marker] = 0x7f2f32dc;
		_editor_palette[imgui::code_editor::color_warning_marker] = 0x7f0089b5;
		_editor_palette[imgui::code_editor::color_line_number] = 0xffa1a193;
		_editor_palette[imgui::code_editor::color_current_line_fill] = 0x7fd5e8ee;
		_editor_palette[imgui::code_editor::color_current_line_fill_inactive] = 0x7fd5e8ee;
		_editor_palette[imgui::code_editor::color_current_line_edge] = 0x7fd5e8ee;
		break;
	case 2:
	default:
		ImVec4 value;
		for (ImGuiCol i = 0; i < imgui::code_editor::color_palette_max; i++)
			value = ImGui::ColorConvertU32ToFloat4(_editor_palette[i]), // Get default value first
			config.get("STYLE",  imgui::code_editor::get_palette_color_name(i), (float(&)[4])value),
			_editor_palette[i] = ImGui::ColorConvertFloat4ToU32(value);
		break;
	}
}
void reshade::runtime::save_custom_style() const
{
	ini_file &config = ini_file::load_cache(_config_path);

	if (_style_index == 3 || _style_index == 4) // Custom Simple, Custom Advanced
	{
		for (ImGuiCol i = 0; i < ImGuiCol_COUNT; i++)
			config.set("STYLE", ImGui::GetStyleColorName(i), (const float(&)[4])_imgui_context->Style.Colors[i]);
	}

	if (_editor_style_index == 2) // Custom
	{
		ImVec4 value;
		for (ImGuiCol i = 0; i < imgui::code_editor::color_palette_max; i++)
			value = ImGui::ColorConvertU32ToFloat4(_editor_palette[i]),
			config.set("STYLE",  imgui::code_editor::get_palette_color_name(i), (const float(&)[4])value);
	}
}

void reshade::runtime::draw_gui()
{
	assert(_is_initialized);

	bool show_overlay = _show_overlay;
	api::input_source show_overlay_source = api::input_source::keyboard;

	if (_input != nullptr)
	{
		if (_show_overlay && !_ignore_shortcuts && _input->is_key_pressed(0x1B /* VK_ESCAPE */) &&
			(_input_processing_mode == 2 || (_input_processing_mode == 1 && (_imgui_context->IO.WantCaptureMouse || _imgui_context->IO.WantCaptureKeyboard))) && !_imgui_context->IO.NavVisible)
			show_overlay = false; // Close when pressing the escape button, input focus is on the overlay and not currently navigating with the keyboard
		else if (!_ignore_shortcuts && _input->is_key_pressed(_overlay_key_data, _force_shortcut_modifiers) && _imgui_context->ActiveId == 0)
			show_overlay = !_show_overlay;

		if (!_ignore_shortcuts)
		{
			if (_input->is_key_pressed(_fps_key_data, _force_shortcut_modifiers))
				_show_fps = _show_fps ? 0 : 1;
			if (_input->is_key_pressed(_frametime_key_data, _force_shortcut_modifiers))
				_show_frametime = _show_frametime ? 0 : 1;
		}
	}

	if (_input_gamepad != nullptr)
	{
		if (_input_gamepad->is_button_down(input_gamepad::button_left_shoulder) &&
			_input_gamepad->is_button_down(input_gamepad::button_right_shoulder) &&
			_input_gamepad->is_button_pressed(input_gamepad::button_start))
		{
			show_overlay = !_show_overlay;
			show_overlay_source = api::input_source::gamepad;
		}
	}

	if (show_overlay != _show_overlay)
		open_overlay(show_overlay, show_overlay_source);

	const bool show_splash_window = _show_splash && (is_loading() || (_reload_count <= 1 && (_last_present_time - _last_reload_time) < std::chrono::seconds(5)) || (!_show_overlay && _tutorial_index == 0 && _input != nullptr));

	// Do not show this message in the same frame the screenshot is taken (so that it won't show up on the GUI screenshot)
	const bool show_screenshot_message = (_show_screenshot_message || !_last_screenshot_save_successful) && !_should_save_screenshot && (_last_present_time - _last_screenshot_time) < std::chrono::seconds(_last_screenshot_save_successful ? 3 : 5);
	const bool show_preset_transition_message = _show_preset_transition_message && _is_in_preset_transition;
	const bool show_message_window = show_screenshot_message || show_preset_transition_message || !_preset_save_successful;

	const bool show_clock = _show_clock == 1 || (_show_overlay && _show_clock > 1);
	const bool show_fps = _show_fps == 1 || (_show_overlay && _show_fps > 1);
	const bool show_frametime = _show_frametime == 1 || (_show_overlay && _show_frametime > 1);
	const bool show_preset_name = _show_preset_name == 1 || (_show_overlay && _show_preset_name > 1);
	bool show_statistics_window = show_clock || show_fps || show_frametime || show_preset_name;

	_ignore_shortcuts = false;
	_block_input_next_frame = false;
	_gather_gpu_statistics = false;
	_effects_expanded_state &= 2;

	if (!show_splash_window && !show_message_window && !show_statistics_window && !_show_overlay && _preview_texture == 0)
	{
		if (_input != nullptr)
		{
			_input->block_mouse_input(false);
			_input->block_keyboard_input(false);
			_input->block_mouse_cursor_warping(false);
		}
		return; // Early-out to avoid costly ImGui calls when no GUI elements are on the screen
	}

	build_font_atlas();
	if (_font_atlas_srv == 0)
		return; // Cannot render GUI without font atlas

	ImGuiContext *const backup_context = ImGui::GetCurrentContext();
	ImGui::SetCurrentContext(_imgui_context);

	ImGuiIO &imgui_io = _imgui_context->IO;
	imgui_io.DeltaTime = _last_frame_duration.count() * 1e-9f;
	imgui_io.DisplaySize.x = static_cast<float>(_width);
	imgui_io.DisplaySize.y = static_cast<float>(_height);
	imgui_io.Fonts->TexID = _font_atlas_srv.handle;

	if (_input != nullptr)
	{
		imgui_io.MouseDrawCursor = _show_overlay && (!_should_save_screenshot || !_screenshot_save_gui);

		// Scale mouse position in case render resolution does not match the window size
		unsigned int max_position[2];
		_input->max_mouse_position(max_position);
		imgui_io.AddMousePosEvent(
			_input->mouse_position_x() * (imgui_io.DisplaySize.x / max_position[0]),
			_input->mouse_position_y() * (imgui_io.DisplaySize.y / max_position[1]));

		// Add wheel delta to the current absolute mouse wheel position
		imgui_io.AddMouseWheelEvent(0.0f, _input->mouse_wheel_delta());

		// Update all the button states
		constexpr std::pair<ImGuiKey, unsigned int> key_mappings[] = {
			{ ImGuiKey_Tab, 0x09 /* VK_TAB */ },
			{ ImGuiKey_LeftArrow, 0x25 /* VK_LEFT */ },
			{ ImGuiKey_RightArrow, 0x27 /* VK_RIGHT */ },
			{ ImGuiKey_UpArrow, 0x26 /* VK_UP */ },
			{ ImGuiKey_DownArrow, 0x28 /* VK_DOWN */ },
			{ ImGuiKey_PageUp, 0x21 /* VK_PRIOR */ },
			{ ImGuiKey_PageDown, 0x22 /* VK_NEXT */ },
			{ ImGuiKey_End, 0x23 /* VK_END */ },
			{ ImGuiKey_Home, 0x24 /* VK_HOME */ },
			{ ImGuiKey_Insert, 0x2D /* VK_INSERT */ },
			{ ImGuiKey_Delete, 0x2E /* VK_DELETE */ },
			{ ImGuiKey_Backspace, 0x08 /* VK_BACK */ },
			{ ImGuiKey_Space, 0x20 /* VK_SPACE */ },
			{ ImGuiKey_Enter, 0x0D /* VK_RETURN */ },
			{ ImGuiKey_Escape, 0x1B /* VK_ESCAPE */ },
			{ ImGuiKey_LeftCtrl, 0xA2 /* VK_LCONTROL */ },
			{ ImGuiKey_LeftShift, 0xA0 /* VK_LSHIFT */ },
			{ ImGuiKey_LeftAlt, 0xA4 /* VK_LMENU */ },
			{ ImGuiKey_LeftSuper, 0x5B /* VK_LWIN */ },
			{ ImGuiKey_RightCtrl, 0xA3 /* VK_RCONTROL */ },
			{ ImGuiKey_RightShift, 0xA1 /* VK_RSHIFT */ },
			{ ImGuiKey_RightAlt, 0xA5 /* VK_RMENU */ },
			{ ImGuiKey_RightSuper, 0x5C /* VK_RWIN */ },
			{ ImGuiKey_Menu, 0x5D /* VK_APPS */ },
			{ ImGuiKey_0, '0' },
			{ ImGuiKey_1, '1' },
			{ ImGuiKey_2, '2' },
			{ ImGuiKey_3, '3' },
			{ ImGuiKey_4, '4' },
			{ ImGuiKey_5, '5' },
			{ ImGuiKey_6, '6' },
			{ ImGuiKey_7, '7' },
			{ ImGuiKey_8, '8' },
			{ ImGuiKey_9, '9' },
			{ ImGuiKey_A, 'A' },
			{ ImGuiKey_B, 'B' },
			{ ImGuiKey_C, 'C' },
			{ ImGuiKey_D, 'D' },
			{ ImGuiKey_E, 'E' },
			{ ImGuiKey_F, 'F' },
			{ ImGuiKey_G, 'G' },
			{ ImGuiKey_H, 'H' },
			{ ImGuiKey_I, 'I' },
			{ ImGuiKey_J, 'J' },
			{ ImGuiKey_K, 'K' },
			{ ImGuiKey_L, 'L' },
			{ ImGuiKey_M, 'M' },
			{ ImGuiKey_N, 'N' },
			{ ImGuiKey_O, 'O' },
			{ ImGuiKey_P, 'P' },
			{ ImGuiKey_Q, 'Q' },
			{ ImGuiKey_R, 'R' },
			{ ImGuiKey_S, 'S' },
			{ ImGuiKey_T, 'T' },
			{ ImGuiKey_U, 'U' },
			{ ImGuiKey_V, 'V' },
			{ ImGuiKey_W, 'W' },
			{ ImGuiKey_X, 'X' },
			{ ImGuiKey_Y, 'Y' },
			{ ImGuiKey_Z, 'Z' },
			{ ImGuiKey_F1, 0x70 /* VK_F1 */ },
			{ ImGuiKey_F2, 0x71 /* VK_F2 */ },
			{ ImGuiKey_F3, 0x72 /* VK_F3 */ },
			{ ImGuiKey_F4, 0x73 /* VK_F4 */ },
			{ ImGuiKey_F5, 0x74 /* VK_F5 */ },
			{ ImGuiKey_F6, 0x75 /* VK_F6 */ },
			{ ImGuiKey_F7, 0x76 /* VK_F7 */ },
			{ ImGuiKey_F8, 0x77 /* VK_F8 */ },
			{ ImGuiKey_F9, 0x78 /* VK_F9 */ },
			{ ImGuiKey_F10, 0x79 /* VK_F10 */ },
			{ ImGuiKey_F11, 0x80 /* VK_F11 */ },
			{ ImGuiKey_F12, 0x81 /* VK_F12 */ },
			{ ImGuiKey_Apostrophe, 0xDE /* VK_OEM_7 */ },
			{ ImGuiKey_Comma, 0xBC /* VK_OEM_COMMA */ },
			{ ImGuiKey_Minus, 0xBD /* VK_OEM_MINUS */ },
			{ ImGuiKey_Period, 0xBE /* VK_OEM_PERIOD */ },
			{ ImGuiKey_Slash, 0xBF /* VK_OEM_2 */ },
			{ ImGuiKey_Semicolon, 0xBA /* VK_OEM_1 */ },
			{ ImGuiKey_Equal, 0xBB /* VK_OEM_PLUS */ },
			{ ImGuiKey_LeftBracket, 0xDB /* VK_OEM_4 */ },
			{ ImGuiKey_Backslash, 0xDC /* VK_OEM_5 */ },
			{ ImGuiKey_RightBracket, 0xDD /* VK_OEM_6 */ },
			{ ImGuiKey_GraveAccent, 0xC0 /* VK_OEM_3 */ },
			{ ImGuiKey_CapsLock, 0x14 /* VK_CAPITAL */ },
			{ ImGuiKey_ScrollLock, 0x91 /* VK_SCROLL */ },
			{ ImGuiKey_NumLock, 0x90 /* VK_NUMLOCK */ },
			{ ImGuiKey_PrintScreen, 0x2C /* VK_SNAPSHOT */ },
			{ ImGuiKey_Pause, 0x13 /* VK_PAUSE */ },
			{ ImGuiKey_Keypad0, 0x60 /* VK_NUMPAD0 */ },
			{ ImGuiKey_Keypad1, 0x61 /* VK_NUMPAD1 */ },
			{ ImGuiKey_Keypad2, 0x62 /* VK_NUMPAD2 */ },
			{ ImGuiKey_Keypad3, 0x63 /* VK_NUMPAD3 */ },
			{ ImGuiKey_Keypad4, 0x64 /* VK_NUMPAD4 */ },
			{ ImGuiKey_Keypad5, 0x65 /* VK_NUMPAD5 */ },
			{ ImGuiKey_Keypad6, 0x66 /* VK_NUMPAD6 */ },
			{ ImGuiKey_Keypad7, 0x67 /* VK_NUMPAD7 */ },
			{ ImGuiKey_Keypad8, 0x68 /* VK_NUMPAD8 */ },
			{ ImGuiKey_Keypad9, 0x69 /* VK_NUMPAD9 */ },
			{ ImGuiKey_KeypadDecimal, 0x6E /* VK_DECIMAL */ },
			{ ImGuiKey_KeypadDivide, 0x6F /* VK_DIVIDE */ },
			{ ImGuiKey_KeypadMultiply, 0x6A /* VK_MULTIPLY */ },
			{ ImGuiKey_KeypadSubtract, 0x6D /* VK_SUBTRACT */ },
			{ ImGuiKey_KeypadAdd, 0x6B /* VK_ADD */ },
			{ ImGuiMod_Ctrl, 0x11 /* VK_CONTROL */ },
			{ ImGuiMod_Shift, 0x10 /* VK_SHIFT */ },
			{ ImGuiMod_Alt, 0x12 /* VK_MENU */ },
			{ ImGuiMod_Super, 0x5D /* VK_APPS */ },
		};

		for (const std::pair<ImGuiKey, unsigned int> &mapping : key_mappings)
			imgui_io.AddKeyEvent(mapping.first, _input->is_key_down(mapping.second));
		for (ImGuiMouseButton i = 0; i < ImGuiMouseButton_COUNT; i++)
			imgui_io.AddMouseButtonEvent(i, _input->is_mouse_button_down(i));
		for (ImWchar16 c : _input->text_input())
			imgui_io.AddInputCharacterUTF16(c);
	}

	if (_input_gamepad != nullptr)
	{
		if (_input_gamepad->is_connected())
		{
			imgui_io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

			constexpr std::pair<ImGuiKey, input_gamepad::button> button_mappings[] = {
				{ ImGuiKey_GamepadStart, input_gamepad::button_start },
				{ ImGuiKey_GamepadBack, input_gamepad::button_back },
				{ ImGuiKey_GamepadFaceLeft, input_gamepad::button_x },
				{ ImGuiKey_GamepadFaceRight, input_gamepad::button_b },
				{ ImGuiKey_GamepadFaceUp, input_gamepad::button_y },
				{ ImGuiKey_GamepadFaceDown, input_gamepad::button_a },
				{ ImGuiKey_GamepadDpadLeft, input_gamepad::button_dpad_left },
				{ ImGuiKey_GamepadDpadRight, input_gamepad::button_dpad_right },
				{ ImGuiKey_GamepadDpadUp, input_gamepad::button_dpad_up },
				{ ImGuiKey_GamepadDpadDown, input_gamepad::button_dpad_down },
				{ ImGuiKey_GamepadL1, input_gamepad::button_left_shoulder },
				{ ImGuiKey_GamepadR1, input_gamepad::button_right_shoulder },
				{ ImGuiKey_GamepadL3, input_gamepad::button_left_thumb },
				{ ImGuiKey_GamepadR3, input_gamepad::button_right_thumb },
			};

			for (const std::pair<ImGuiKey, input_gamepad::button> &mapping : button_mappings)
				imgui_io.AddKeyEvent(mapping.first, _input_gamepad->is_button_down(mapping.second));

			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadL2, _input_gamepad->left_trigger_position() != 0.0f, _input_gamepad->left_trigger_position());
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadR2, _input_gamepad->right_trigger_position() != 0.0f, _input_gamepad->right_trigger_position());
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, _input_gamepad->left_thumb_axis_x() < 0.0f, -std::min(_input_gamepad->left_thumb_axis_x(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, _input_gamepad->left_thumb_axis_x() > 0.0f, std::max(_input_gamepad->left_thumb_axis_x(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, _input_gamepad->left_thumb_axis_y() > 0.0f, std::max(_input_gamepad->left_thumb_axis_y(), 0.0f));
			imgui_io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, _input_gamepad->left_thumb_axis_y() < 0.0f, -std::min(_input_gamepad->left_thumb_axis_y(), 0.0f));
		}
		else
		{
			imgui_io.BackendFlags &= ~ImGuiBackendFlags_HasGamepad;
		}
	}

	ImGui::NewFrame();

	ImVec2 viewport_offset = ImVec2(0, 0);
	const bool show_spinner = _reload_count > 1 && _tutorial_index != 0;

	if (_show_overlay)
	{
		const ImGuiViewport *const viewport = ImGui::GetMainViewport();
		static bool showdemo = false;
		if (showdemo) ImGui::ShowDemoWindow(&showdemo);

		ImGui::Begin("Demo");
		ImGui::Text("hello world");
		if (ImGui::Button("ShowDemo")) showdemo = !showdemo;
		ImGui::End();

		const ImGuiID root_space_id = ImGui::GetID("ViewportDockspace");

		// Set up default dock layout if this was not done yet
		const bool init_window_layout = !ImGui::DockBuilderGetNode(root_space_id);
		if (init_window_layout)
		{
			// Add the root node
			ImGui::DockBuilderAddNode(root_space_id, ImGuiDockNodeFlags_DockSpace);
			ImGui::DockBuilderSetNodeSize(root_space_id, viewport->Size);

			// Split root node into two spaces
			ImGuiID main_space_id = 0;
			ImGuiID right_space_id = 0;
			ImGui::DockBuilderSplitNode(root_space_id, ImGuiDir_Left, 0.35f, &main_space_id, &right_space_id);

			// Commit the layout
			ImGui::DockBuilderFinish(root_space_id);
		}

		ImGui::SetNextWindowPos(viewport->Pos + viewport_offset);
		ImGui::SetNextWindowSize(viewport->Size - viewport_offset);
		ImGui::SetNextWindowViewport(viewport->ID);
		ImGui::Begin("Viewport", nullptr,
			ImGuiWindowFlags_NoDecoration |
			ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoMove |
			ImGuiWindowFlags_NoDocking | // This is the background viewport, the docking space is a child of it
			ImGuiWindowFlags_NoFocusOnAppearing |
			ImGuiWindowFlags_NoBringToFrontOnFocus |
			ImGuiWindowFlags_NoBackground);
		ImGui::DockSpace(root_space_id, ImVec2(0, 0), ImGuiDockNodeFlags_PassthruCentralNode);
		ImGui::End();

		if (_imgui_context->NavInputSource > ImGuiInputSource_Mouse && _imgui_context->NavWindowingTarget == nullptr)
		{
			// Reset input source to mouse when the cursor is moved
			if (_input != nullptr && (_input->mouse_movement_delta_x() != 0 || _input->mouse_movement_delta_y() != 0))
				_imgui_context->NavInputSource = ImGuiInputSource_Mouse;
			// Ensure there is always a window that has navigation focus when keyboard or gamepad navigation is used (choose the first overlay window created next)
			else if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow))
				ImGui::SetNextWindowFocus();
		}

	}

	// Disable keyboard shortcuts while typing into input boxes
	_ignore_shortcuts |= ImGui::IsAnyItemActive();

	// Render ImGui widgets and windows
	ImGui::Render();

	if (_input != nullptr)
	{
		const bool block_input = _input_processing_mode != 0 && (_show_overlay || _block_input_next_frame);
		const bool block_mouse_input = block_input && (imgui_io.WantCaptureMouse || _input_processing_mode == 2);
		const bool block_keyboard_input = block_input && (imgui_io.WantCaptureKeyboard || _input_processing_mode == 2);

		_input->block_mouse_input(block_mouse_input);
		_input->block_keyboard_input(block_keyboard_input);
		_input->block_mouse_cursor_warping(_show_overlay || _block_input_next_frame || block_mouse_input);
	}

	if (ImDrawData *const draw_data = ImGui::GetDrawData();
		draw_data != nullptr && draw_data->CmdListsCount != 0 && draw_data->TotalVtxCount != 0)
	{
		api::command_list *const cmd_list = _graphics_queue->get_immediate_command_list();

		if (_back_buffer_resolved != 0)
		{
			render_imgui_draw_data(cmd_list, draw_data, _back_buffer_targets[0]);
		}
		else
		{
			uint32_t back_buffer_index = get_current_back_buffer_index() * 2;
			const api::resource back_buffer_resource = _device->get_resource_from_view(_back_buffer_targets[back_buffer_index]);

			cmd_list->barrier(back_buffer_resource, api::resource_usage::present, api::resource_usage::render_target);
			render_imgui_draw_data(cmd_list, draw_data, _back_buffer_targets[back_buffer_index]);
			cmd_list->barrier(back_buffer_resource, api::resource_usage::render_target, api::resource_usage::present);
		}
	}

	ImGui::SetCurrentContext(backup_context);
}

void reshade::runtime::open_code_editor(size_t effect_index, size_t permutation_index, const std::string &entry_point)
{
	assert(effect_index < _effects.size());

	const std::filesystem::path &path = _effects[effect_index].source_file;

	if (const auto it = std::find_if(_editors.begin(), _editors.end(),
			[effect_index, permutation_index, &path, &entry_point](const editor_instance &instance) {
				return instance.effect_index == effect_index && instance.permutation_index == permutation_index && instance.file_path == path && instance.generated && instance.entry_point_name == entry_point;
			});
		it != _editors.end())
	{
		it->selected = true;
		open_code_editor(*it);
	}
	else
	{
		editor_instance instance { effect_index, permutation_index, path, entry_point, true, true };
		open_code_editor(instance);
		_editors.push_back(std::move(instance));
	}
}
void reshade::runtime::open_code_editor(size_t effect_index, const std::filesystem::path &path)
{
	assert(effect_index < _effects.size());

	if (const auto it = std::find_if(_editors.begin(), _editors.end(),
			[effect_index, &path](const editor_instance &instance) {
				return instance.effect_index == effect_index && instance.file_path == path && !instance.generated;
			});
		it != _editors.end())
	{
		it->selected = true;
		open_code_editor(*it);
	}
	else
	{
		editor_instance instance { effect_index, std::numeric_limits<size_t>::max(), path, std::string(), true, false };
		open_code_editor(instance);
		_editors.push_back(std::move(instance));
	}
}
void reshade::runtime::open_code_editor(editor_instance &instance) const
{
	const effect &effect = _effects[instance.effect_index];

	if (instance.generated)
	{
		const effect::permutation &permutation = effect.permutations[instance.permutation_index];

		if (instance.entry_point_name.empty())
			instance.editor.set_text(permutation.generated_code);
		else
			instance.editor.set_text(permutation.assembly_text.at(instance.entry_point_name));
		instance.editor.set_readonly(true);
		return; // Errors only apply to the effect source, not generated code
	}

	// Only update text if there is no undo history (in which case it can be assumed that the text is already up-to-date)
	if (!instance.editor.is_modified() && !instance.editor.can_undo())
	{
		if (FILE *const file = _wfsopen(instance.file_path.c_str(), L"rb", SH_DENYWR))
		{
			fseek(file, 0, SEEK_END);
			const size_t file_size = ftell(file);
			fseek(file, 0, SEEK_SET);

			std::string text(file_size, '\0');
			fread(text.data(), 1, file_size, file);

			fclose(file);

			instance.editor.set_text(text);
			instance.editor.set_readonly(false);
		}
	}

	instance.editor.clear_errors();

	parse_errors(effect.errors,
		[&instance](const std::string_view file, int line, const std::string_view message) {
			// Ignore errors that aren't in the current source file
			if (file != instance.file_path.u8string())
				return;

			instance.editor.add_error(line, message, message.find("error") == std::string::npos);
		});
}

bool reshade::runtime::init_imgui_resources()
{
	// Adjust default font size based on the vertical resolution
	if (_font_size == 0)
		_editor_font_size = _font_size = _height >= 2160 ? 26 : _height >= 1440 ? 20 : 13;

	const bool has_combined_sampler_and_view = _device->check_capability(api::device_caps::sampler_with_resource_view);

	if (_imgui_sampler_state == 0)
	{
		api::sampler_desc sampler_desc = {};
		sampler_desc.filter = api::filter_mode::min_mag_mip_linear;
		sampler_desc.address_u = api::texture_address_mode::clamp;
		sampler_desc.address_v = api::texture_address_mode::clamp;
		sampler_desc.address_w = api::texture_address_mode::clamp;

		if (!_device->create_sampler(sampler_desc, &_imgui_sampler_state))
		{
			log::message(log::level::error, "Failed to create ImGui sampler object!");
			return false;
		}
	}

	if (_imgui_pipeline_layout == 0)
	{
		uint32_t num_layout_params = 0;
		api::pipeline_layout_param layout_params[3];

		if (has_combined_sampler_and_view)
		{
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::sampler_with_resource_view }; // s0
		}
		else
		{
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::sampler }; // s0
			layout_params[num_layout_params++] = api::descriptor_range { 0, 0, 0, 1, api::shader_stage::pixel, 1, api::descriptor_type::shader_resource_view }; // t0
		}

		layout_params[num_layout_params++] = api::constant_range { 0, 0, 0, 18, api::shader_stage::vertex | api::shader_stage::pixel }; // b0

		if (!_device->create_pipeline_layout(num_layout_params, layout_params, &_imgui_pipeline_layout))
		{
			log::message(log::level::error, "Failed to create ImGui pipeline layout!");
			return false;
		}
	}

	if (_imgui_pipeline != 0)
		return true;

	const resources::data_resource vs_res = resources::load_data_resource(
		_renderer_id >= 0x20000 ? IDR_IMGUI_VS_SPIRV :
		_renderer_id >= 0x10000 ? IDR_IMGUI_VS_GLSL :
		_renderer_id >= 0x0a000 ? IDR_IMGUI_VS_4_0 : IDR_IMGUI_VS_3_0);
	api::shader_desc vs_desc;
	vs_desc.code = vs_res.data;
	vs_desc.code_size = vs_res.data_size;

	const resources::data_resource ps_res = resources::load_data_resource(
		_renderer_id >= 0x20000 ? IDR_IMGUI_PS_SPIRV :
		_renderer_id >= 0x10000 ? IDR_IMGUI_PS_GLSL :
		_renderer_id >= 0x0a000 ? IDR_IMGUI_PS_4_0 : IDR_IMGUI_PS_3_0);
	api::shader_desc ps_desc;
	ps_desc.code = ps_res.data;
	ps_desc.code_size = ps_res.data_size;

	std::vector<api::pipeline_subobject> subobjects;
	subobjects.push_back({ api::pipeline_subobject_type::vertex_shader, 1, &vs_desc });
	subobjects.push_back({ api::pipeline_subobject_type::pixel_shader, 1, &ps_desc });

	const api::input_element input_layout[3] = {
		{ 0, "POSITION", 0, api::format::r32g32_float,   0, offsetof(ImDrawVert, pos), sizeof(ImDrawVert), 0 },
		{ 1, "TEXCOORD", 0, api::format::r32g32_float,   0, offsetof(ImDrawVert, uv ), sizeof(ImDrawVert), 0 },
		{ 2, "COLOR",    0, api::format::r8g8b8a8_unorm, 0, offsetof(ImDrawVert, col), sizeof(ImDrawVert), 0 }
	};
	subobjects.push_back({ api::pipeline_subobject_type::input_layout, 3, (void *)input_layout });

	api::primitive_topology topology = api::primitive_topology::triangle_list;
	subobjects.push_back({ api::pipeline_subobject_type::primitive_topology, 1, &topology });

	api::blend_desc blend_state;
	blend_state.blend_enable[0] = true;
	blend_state.source_color_blend_factor[0] = api::blend_factor::source_alpha;
	blend_state.dest_color_blend_factor[0] = api::blend_factor::one_minus_source_alpha;
	blend_state.color_blend_op[0] = api::blend_op::add;
	blend_state.source_alpha_blend_factor[0] = api::blend_factor::one;
	blend_state.dest_alpha_blend_factor[0] = api::blend_factor::one_minus_source_alpha;
	blend_state.alpha_blend_op[0] = api::blend_op::add;
	blend_state.render_target_write_mask[0] = 0xF;
	subobjects.push_back({ api::pipeline_subobject_type::blend_state, 1, &blend_state });

	api::rasterizer_desc rasterizer_state;
	rasterizer_state.cull_mode = api::cull_mode::none;
	rasterizer_state.scissor_enable = true;
	subobjects.push_back({ api::pipeline_subobject_type::rasterizer_state, 1, &rasterizer_state });

	api::depth_stencil_desc depth_stencil_state;
	depth_stencil_state.depth_enable = false;
	depth_stencil_state.stencil_enable = false;
	subobjects.push_back({ api::pipeline_subobject_type::depth_stencil_state, 1, &depth_stencil_state });

	// Always choose non-sRGB format variant, since 'render_imgui_draw_data' is called with the non-sRGB render target (see 'draw_gui')
	api::format render_target_format = api::format_to_default_typed(_back_buffer_format, 0);
	subobjects.push_back({ api::pipeline_subobject_type::render_target_formats, 1, &render_target_format });

	if (!_device->create_pipeline(_imgui_pipeline_layout, static_cast<uint32_t>(subobjects.size()), subobjects.data(), &_imgui_pipeline))
	{
		log::message(log::level::error, "Failed to create ImGui pipeline!");
		return false;
	}

	return true;
}
void reshade::runtime::render_imgui_draw_data(api::command_list *cmd_list, ImDrawData *draw_data, api::resource_view rtv)
{
	// Need to multi-buffer vertex data so not to modify data below when the previous frame is still in flight
	const size_t buffer_index = _frame_count % std::size(_imgui_vertices);

	// Create and grow vertex/index buffers if needed
	if (_imgui_num_indices[buffer_index] < draw_data->TotalIdxCount)
	{
		if (_imgui_indices[buffer_index] != 0)
		{
			_graphics_queue->wait_idle(); // Be safe and ensure nothing still uses this buffer

			_device->destroy_resource(_imgui_indices[buffer_index]);
		}

		const int new_size = draw_data->TotalIdxCount + 10000;
		if (!_device->create_resource(api::resource_desc(new_size * sizeof(ImDrawIdx), api::memory_heap::cpu_to_gpu, api::resource_usage::index_buffer), nullptr, api::resource_usage::cpu_access, &_imgui_indices[buffer_index]))
		{
			log::message(log::level::error, "Failed to create ImGui index buffer!");
			return;
		}

		_device->set_resource_name(_imgui_indices[buffer_index], "ImGui index buffer");

		_imgui_num_indices[buffer_index] = new_size;
	}
	if (_imgui_num_vertices[buffer_index] < draw_data->TotalVtxCount)
	{
		if (_imgui_vertices[buffer_index] != 0)
		{
			_graphics_queue->wait_idle();

			_device->destroy_resource(_imgui_vertices[buffer_index]);
		}

		const int new_size = draw_data->TotalVtxCount + 5000;
		if (!_device->create_resource(api::resource_desc(new_size * sizeof(ImDrawVert), api::memory_heap::cpu_to_gpu, api::resource_usage::vertex_buffer), nullptr, api::resource_usage::cpu_access, &_imgui_vertices[buffer_index]))
		{
			log::message(log::level::error, "Failed to create ImGui vertex buffer!");
			return;
		}

		_device->set_resource_name(_imgui_vertices[buffer_index], "ImGui vertex buffer");

		_imgui_num_vertices[buffer_index] = new_size;
	}

#ifndef NDEBUG
	cmd_list->begin_debug_event("ReShade overlay");
#endif

	if (ImDrawIdx *idx_dst;
		_device->map_buffer_region(_imgui_indices[buffer_index], 0, UINT64_MAX, api::map_access::write_only, reinterpret_cast<void **>(&idx_dst)))
	{
		for (int n = 0; n < draw_data->CmdListsCount; ++n)
		{
			const ImDrawList *const draw_list = draw_data->CmdLists[n];
			std::memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
			idx_dst += draw_list->IdxBuffer.Size;
		}

		_device->unmap_buffer_region(_imgui_indices[buffer_index]);
	}
	if (ImDrawVert *vtx_dst;
		_device->map_buffer_region(_imgui_vertices[buffer_index], 0, UINT64_MAX, api::map_access::write_only, reinterpret_cast<void **>(&vtx_dst)))
	{
		for (int n = 0; n < draw_data->CmdListsCount; ++n)
		{
			const ImDrawList *const draw_list = draw_data->CmdLists[n];
			std::memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
			vtx_dst += draw_list->VtxBuffer.Size;
		}

		_device->unmap_buffer_region(_imgui_vertices[buffer_index]);
	}

	api::render_pass_render_target_desc render_target = {};
	render_target.view = rtv;

	cmd_list->begin_render_pass(1, &render_target, nullptr);

	// Setup render state
	cmd_list->bind_pipeline(api::pipeline_stage::all_graphics, _imgui_pipeline);

	cmd_list->bind_index_buffer(_imgui_indices[buffer_index], 0, sizeof(ImDrawIdx));
	cmd_list->bind_vertex_buffer(0, _imgui_vertices[buffer_index], 0, sizeof(ImDrawVert));

	const api::viewport viewport = { 0, 0, draw_data->DisplaySize.x, draw_data->DisplaySize.y, 0.0f, 1.0f };
	cmd_list->bind_viewports(0, 1, &viewport);

	// Setup orthographic projection matrix
	const bool flip_y = (_renderer_id & 0x10000) != 0;
	const bool adjust_half_pixel = _renderer_id < 0xa000; // Bake half-pixel offset into matrix in D3D9
	const bool depth_clip_zero_to_one = (_renderer_id & 0x10000) == 0;

	const struct {
		float ortho_projection[16];
		api::color_space color_space;
		float hdr_overlay_brightness;
	} push_constants = {
		{
			2.0f / draw_data->DisplaySize.x, 0.0f, 0.0f, 0.0f,
			0.0f, (flip_y ? 2.0f : -2.0f) / draw_data->DisplaySize.y, 0.0f, 0.0f,
			0.0f,                            0.0f, depth_clip_zero_to_one ? 0.5f : -1.0f, 0.0f,
							   -(2 * draw_data->DisplayPos.x + draw_data->DisplaySize.x + (adjust_half_pixel ? 1.0f : 0.0f)) / draw_data->DisplaySize.x,
			(flip_y ? -1 : 1) * (2 * draw_data->DisplayPos.y + draw_data->DisplaySize.y + (adjust_half_pixel ? 1.0f : 0.0f)) / draw_data->DisplaySize.y, depth_clip_zero_to_one ? 0.5f : 0.0f, 1.0f,
		},
		_hdr_overlay_overwrite_color_space != api::color_space::unknown ?
			_hdr_overlay_overwrite_color_space :
			// Workaround for early HDR games, RGBA16F without a color space defined is pretty much guaranteed to be HDR for games
			_back_buffer_format == api::format::r16g16b16a16_float ?
				api::color_space::extended_srgb_linear : _back_buffer_color_space,
		_hdr_overlay_brightness
	};

	const bool has_combined_sampler_and_view = _device->check_capability(api::device_caps::sampler_with_resource_view);

	cmd_list->push_constants(api::shader_stage::vertex | api::shader_stage::pixel, _imgui_pipeline_layout, has_combined_sampler_and_view ? 1 : 2, 0, (_renderer_id != 0x9000 ? sizeof(push_constants) : sizeof(push_constants.ortho_projection)) / 4, &push_constants);
	if (!has_combined_sampler_and_view)
		cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler, &_imgui_sampler_state });

	int vtx_offset = 0, idx_offset = 0;
	for (int n = 0; n < draw_data->CmdListsCount; ++n)
	{
		const ImDrawList *const draw_list = draw_data->CmdLists[n];

		for (const ImDrawCmd &cmd : draw_list->CmdBuffer)
		{
			if (cmd.UserCallback != nullptr)
			{
				cmd.UserCallback(draw_list, &cmd);
				continue;
			}

			assert(cmd.TextureId != 0);

			const api::rect scissor_rect = {
				static_cast<int32_t>(cmd.ClipRect.x - draw_data->DisplayPos.x),
				flip_y ? static_cast<int32_t>(_height - cmd.ClipRect.w + draw_data->DisplayPos.y) : static_cast<int32_t>(cmd.ClipRect.y - draw_data->DisplayPos.y),
				static_cast<int32_t>(cmd.ClipRect.z - draw_data->DisplayPos.x),
				flip_y ? static_cast<int32_t>(_height - cmd.ClipRect.y + draw_data->DisplayPos.y) : static_cast<int32_t>(cmd.ClipRect.w - draw_data->DisplayPos.y)
			};

			cmd_list->bind_scissor_rects(0, 1, &scissor_rect);

			const api::resource_view srv = { (uint64_t)cmd.TextureId };
			if (has_combined_sampler_and_view)
			{
				api::sampler_with_resource_view sampler_and_view = { _imgui_sampler_state, srv };
				cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 0, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::sampler_with_resource_view, &sampler_and_view });
			}
			else
			{
				cmd_list->push_descriptors(api::shader_stage::pixel, _imgui_pipeline_layout, 1, api::descriptor_table_update { {}, 0, 0, 1, api::descriptor_type::shader_resource_view, &srv });
			}

			cmd_list->draw_indexed(cmd.ElemCount, 1, cmd.IdxOffset + idx_offset, cmd.VtxOffset + vtx_offset, 0);
		}

		idx_offset += draw_list->IdxBuffer.Size;
		vtx_offset += draw_list->VtxBuffer.Size;
	}

	cmd_list->end_render_pass();

#ifndef NDEBUG
	cmd_list->end_debug_event();
#endif
}
void reshade::runtime::destroy_imgui_resources()
{
	_imgui_context->IO.Fonts->Clear();

	_device->destroy_resource(_font_atlas_tex);
	_font_atlas_tex = {};
	_device->destroy_resource_view(_font_atlas_srv);
	_font_atlas_srv = {};

	for (size_t i = 0; i < std::size(_imgui_vertices); ++i)
	{
		_device->destroy_resource(_imgui_indices[i]);
		_imgui_indices[i] = {};
		_imgui_num_indices[i] = 0;
		_device->destroy_resource(_imgui_vertices[i]);
		_imgui_vertices[i] = {};
		_imgui_num_vertices[i] = 0;
	}

	_device->destroy_sampler(_imgui_sampler_state);
	_imgui_sampler_state = {};
	_device->destroy_pipeline(_imgui_pipeline);
	_imgui_pipeline = {};
	_device->destroy_pipeline_layout(_imgui_pipeline_layout);
	_imgui_pipeline_layout = {};
}

bool reshade::runtime::open_overlay(bool open, api::input_source source)
{
#if RESHADE_ADDON
	if (!_is_in_api_call)
	{
		_is_in_api_call = true;
		const bool skip = invoke_addon_event<addon_event::reshade_open_overlay>(this, open, source);
		_is_in_api_call = false;
		if (skip)
			return false;
	}
#endif

	_show_overlay = open;

	if (open)
		_imgui_context->NavInputSource = static_cast<ImGuiInputSource>(source);

	return true;
}

#endif
