/*
 * Copyright (C) 2021 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause OR MIT
 */

#pragma once

#include "reshade_api_device.hpp"

namespace reshade { namespace api
{
	/// <summary>
	/// An opaque handle to a technique in an effect.
	/// </summary>
	/// <remarks>
	/// This handle is only valid until effects are next reloaded again (<see cref="addon_event::reshade_reloaded_effects"/>).
	/// </remarks>
	RESHADE_DEFINE_HANDLE(effect_technique);
	/// <summary>
	/// An opaque handle to a texture variable in an effect.
	/// </summary>
	/// <remarks>
	/// This handle is only valid until effects are next reloaded again (<see cref="addon_event::reshade_reloaded_effects"/>).
	/// </remarks>
	RESHADE_DEFINE_HANDLE(effect_texture_variable);
	/// <summary>
	/// An opaque handle to a uniform variable in an effect.
	/// </summary>
	/// <remarks>
	/// This handle is only valid until effects are next reloaded again (<see cref="addon_event::reshade_reloaded_effects"/>).
	/// </remarks>
	RESHADE_DEFINE_HANDLE(effect_uniform_variable);

	/// <summary>
	/// Input source for events triggered by user input.
	/// </summary>
	enum class input_source
	{
		none = 0,
		mouse = 1,
		keyboard = 2,
		gamepad = 3,
		clipboard = 4,
	};

	/// <summary>
	/// A post-processing effect runtime, used to control effects.
	/// <para>ReShade associates an independent post-processing effect runtime with most swap chains.</para>
	/// </summary>
	struct __declspec(novtable) effect_runtime : public device_object
	{
		/// <summary>
		/// Gets the handle of the window associated with this effect runtime.
		/// </summary>
		virtual void *get_hwnd() const = 0;

		/// <summary>
		/// Gets the back buffer resource at the specified <paramref name="index"/> in the swap chain associated with this effect runtime.
		/// </summary>
		/// <param name="index">Index of the back buffer. This has to be between zero and the value returned by <see cref="get_back_buffer_count"/>.</param>
		virtual resource get_back_buffer(uint32_t index) = 0;

		/// <summary>
		/// Gets the number of back buffer resources in the swap chain associated with this effect runtime.
		/// </summary>
		virtual uint32_t get_back_buffer_count() const = 0;

		/// <summary>
		/// Gets the current back buffer resource.
		/// </summary>
		resource get_current_back_buffer() { return get_back_buffer(get_current_back_buffer_index()); }
		/// <summary>
		/// Gets the index of the back buffer resource that can currently be rendered into.
		/// </summary>
		virtual uint32_t get_current_back_buffer_index() const = 0;

		/// <summary>
		/// Gets the main graphics command queue associated with this effect runtime.
		/// This may potentially be different from the presentation queue and should be used to execute graphics commands on.
		/// </summary>
		virtual command_queue *get_command_queue() = 0;

		/// <summary>
		/// Applies post-processing effects to the specified render targets and prevents the usual rendering of effects before swap chain presentation of the current frame.
		/// This can be used to force ReShade to render effects at a certain point during the frame to e.g. avoid effects being applied to user interface elements of the application.
		/// </summary>
		/// <remarks>
		/// The resource the render target views point to has to be in the <see cref="resource_usage::render_target"/> state.
		/// This call may modify current state on the command list (pipeline, render targets, descriptor tables, ...), so it may be necessary for an add-on to backup and restore state around it if the application does not bind all state again afterwards already.
		/// Calling this with <paramref name="rtv"/> set to zero will cause nothing to be rendered, but uniform variables to still be updated.
		/// </remarks>
		/// <param name="cmd_list">Command list to add effect rendering commands to.</param>
		/// <param name="rtv">Render target view to use for passes that write to the back buffer with <c>SRGBWriteEnabled</c> state set to <see langword="false"/> (this should be a render target view of the target resource, created with a non-sRGB format variant).</param>
		/// <param name="rtv_srgb">Render target view to use for passes that write to the back buffer with <c>SRGBWriteEnabled</c> state set to <see langword="true"/> (this should be a render target view of the target resource, created with a sRGB format variant).</param>

		/// <summary>
		/// Captures a screenshot of the current back buffer resource and returns its image data.
		/// </summary>
		/// <param name="pixels">Pointer to an array of <c>width * height * bpp</c> bytes the image data is written to (where <c>bpp</c> is the number of bytes per pixel of the back buffer format).</param>

		/// <summary>
		/// Gets the current buffer dimensions of the swap chain.
		/// </summary>
		virtual void get_screenshot_width_and_height(uint32_t *out_width, uint32_t *out_height) const = 0;

		/// <summary>
		/// Gets the current status of the specified key.
		/// </summary>
		/// <param name="keycode">The virtual key code to check.</param>
		/// <returns><see langword="true"/> if the key is currently pressed down, <see langword="false"/> otherwise.</returns>
		virtual bool is_key_down(uint32_t keycode) const = 0;
		/// <summary>
		/// Gets whether the specified key was pressed this frame.
		/// </summary>
		/// <param name="keycode">The virtual key code to check.</param>
		/// <returns><see langword="true"/> if the key was pressed this frame, <see langword="false"/> otherwise.</returns>
		virtual bool is_key_pressed(uint32_t keycode) const = 0;
		/// <summary>
		/// Gets whether the specified key was released this frame.
		/// </summary>
		/// <param name="keycode">The virtual key code to check.</param>
		/// <returns><see langword="true"/> if the key was released this frame, <see langword="false"/> otherwise.</returns>
		virtual bool is_key_released(uint32_t keycode) const = 0;
		/// <summary>
		/// Gets the current status of the specified mouse button.
		/// </summary>
		/// <param name="button">The mouse button index to check (0 = left, 1 = middle, 2 = right).</param>
		/// <returns><see langword="true"/> if the mouse button is currently pressed down, <see langword="false"/> otherwise.</returns>
		virtual bool is_mouse_button_down(uint32_t button) const = 0;
		/// <summary>
		/// Gets whether the specified mouse button was pressed this frame.
		/// </summary>
		/// <param name="button">The mouse button index to check (0 = left, 1 = middle, 2 = right).</param>
		/// <returns><see langword="true"/> if the mouse button was pressed this frame, <see langword="false"/> otherwise.</returns>
		virtual bool is_mouse_button_pressed(uint32_t button) const = 0;
		/// <summary>
		/// Gets whether the specified mouse button was released this frame.
		/// </summary>
		/// <param name="button">The mouse button index to check (0 = left, 1 = middle, 2 = right).</param>
		/// <returns><see langword="true"/> if the mouse button was released this frame, <see langword="false"/> otherwise.</returns>
		virtual bool is_mouse_button_released(uint32_t button) const = 0;

		/// <summary>
		/// Gets the current absolute position of the mouse cursor in screen coordinates.
		/// </summary>
		/// <param name="out_x">Pointer to a variable that is set to the X coordinate of the current cursor position.</param>
		/// <param name="out_y">Pointer to a variable that is set to the Y coordinate of the current cursor position.</param>
		/// <param name="out_wheel_delta">Optional pointer to a variable that is set to the mouse wheel delta since the last frame.</param>
		virtual void get_mouse_cursor_position(uint32_t *out_x, uint32_t *out_y, int16_t *out_wheel_delta = nullptr) const = 0;

		/// <summary>
		/// Makes ReShade block any keyboard and mouse input from reaching the game for the duration of the next frame.
		/// Call this every frame for as long as input should be blocked. This can be used to ensure input is only applied to overlays created in a <see cref="addon_event::reshade_overlay"/> callback.
		/// </summary>
		virtual void block_input_next_frame() = 0;

		/// <summary>
		/// Gets the virtual key code of the last key that was pressed.
		/// </summary>
		virtual uint32_t last_key_pressed() const = 0;
		/// <summary>
		/// Gets the virtual key code of the last key that was released.
		/// </summary>
		virtual uint32_t last_key_released() const = 0;

		/// <summary>
		/// Open or close the ReShade overlay.
		/// </summary>
		/// <param name="open">Requested overlay state.</param>
		/// <param name="source">Source of this request.</param>
		/// <returns><see langword="true"/> if the overlay state was changed, <see langword="false"/> otherwise.</returns>
		virtual bool open_overlay(bool open, input_source source) = 0;
	};
} }
