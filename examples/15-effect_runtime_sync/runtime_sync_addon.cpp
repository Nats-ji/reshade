/*
 * Copyright (C) 2024 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <imgui.h>
#include <reshade.hpp>
#include <vector>
#include <shared_mutex>
#include <algorithm> // std::remove

using namespace reshade::api;

static std::shared_mutex s_mutex;
static bool s_sync = false;
static std::vector<effect_runtime *> s_runtimes;

static void on_init(effect_runtime *runtime)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	s_runtimes.push_back(runtime);

	if (!reshade::get_config_value(nullptr, "ADDON", "SyncEffectRuntimes", s_sync))
		// Enable synchronization by default if application is using VR
#ifndef _WIN64
		s_sync = GetModuleHandleW(L"vrclient.dll") != nullptr || GetModuleHandleW(L"openxr_loader.dll") != nullptr;
#else
		s_sync = GetModuleHandleW(L"vrclient_x64.dll") != nullptr || GetModuleHandleW(L"openxr_loader.dll") != nullptr;
#endif
}
static void on_destroy(effect_runtime *runtime)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	s_runtimes.erase(std::remove(s_runtimes.begin(), s_runtimes.end(), runtime), s_runtimes.end());
}

static bool on_reshade_set_uniform_value(effect_runtime *runtime, effect_uniform_variable variable, const void *new_value, size_t new_value_size)
{
	return false;
}
static bool on_reshade_set_effects_state(effect_runtime *runtime, bool enabled)
{
	return false;
}
static bool on_reshade_set_technique_state(effect_runtime *runtime, effect_technique technique, bool enabled)
{
	return false;
}
static void on_reshade_set_current_preset_path(effect_runtime *runtime, const char *path)
{
	if (!s_sync)
		return;

	const std::shared_lock<std::shared_mutex> lock(s_mutex);

	for (effect_runtime *synced_runtime : s_runtimes)
	{
		if (synced_runtime == runtime)
			continue;

		//synced_runtime->set_current_preset_path(path);
	}
}
static bool on_reshade_reorder_techniques(effect_runtime *runtime, size_t count, effect_technique *techniques)
{
	return false;
}

static void apply_preset_to_all(effect_runtime *runtime)
{
}

static void draw_settings_overlay(effect_runtime *runtime)
{
	const std::unique_lock<std::shared_mutex> lock(s_mutex);

	if (s_runtimes.size() == 1)
	{
		assert(s_runtimes[0] == runtime);

		ImGui::TextUnformatted("This is the only active effect runtime instance.");
		return;
	}

	if (ImGui::Button("Apply preset of this effect runtime to all other instances", ImVec2(-1, 0)))
		apply_preset_to_all(runtime);

	if (bool sync = s_sync;
		ImGui::Checkbox("Synchronize effect runtimes", &sync))
	{
		reshade::set_config_value(nullptr, "ADDON", "SyncEffectRuntimes", sync);
		if (sync)
			apply_preset_to_all(runtime);
		s_sync = sync; // Change global value only after applying preset, since it calls the uniform value/technique state events, which would deadlock attempting to lock 's_mutex' again
	}

	if (!s_sync)
		return;

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	ImGui::Text("%p is being synchronized with:", runtime);

	for (effect_runtime *const synced_runtime : s_runtimes)
	{
		if (synced_runtime == runtime)
			continue;

		ImGui::Text("> %p", synced_runtime);
	}
}

void register_addon_effect_runtime_sync()
{
	reshade::register_overlay(nullptr, draw_settings_overlay);

	reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init);
	reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy);

	reshade::register_event<reshade::addon_event::reshade_set_uniform_value>(on_reshade_set_uniform_value);
	reshade::register_event<reshade::addon_event::reshade_set_effects_state>(on_reshade_set_effects_state);
	reshade::register_event<reshade::addon_event::reshade_set_technique_state>(on_reshade_set_technique_state);
	reshade::register_event<reshade::addon_event::reshade_set_current_preset_path>(on_reshade_set_current_preset_path);
	reshade::register_event<reshade::addon_event::reshade_reorder_techniques>(on_reshade_reorder_techniques);
}
void unregister_addon_effect_runtime_sync()
{
	reshade::unregister_event<reshade::addon_event::init_effect_runtime>(on_init);
	reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(on_destroy);

	reshade::unregister_event<reshade::addon_event::reshade_set_uniform_value>(on_reshade_set_uniform_value);
	reshade::unregister_event<reshade::addon_event::reshade_set_effects_state>(on_reshade_set_effects_state);
	reshade::unregister_event<reshade::addon_event::reshade_set_technique_state>(on_reshade_set_technique_state);
	reshade::unregister_event<reshade::addon_event::reshade_set_current_preset_path>(on_reshade_set_current_preset_path);
	reshade::unregister_event<reshade::addon_event::reshade_reorder_techniques>(on_reshade_reorder_techniques);
}

#ifndef BUILTIN_ADDON

extern "C" __declspec(dllexport) const char *NAME = "Effect Runtime Sync";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Adds preset synchronization between different effect runtime instances, e.g. to have changes in a desktop window reflect in VR.";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		register_addon_effect_runtime_sync();
		break;
	case DLL_PROCESS_DETACH:
		unregister_addon_effect_runtime_sync();
		reshade::unregister_addon(hModule);
		break;
	}

	return TRUE;
}

#endif
