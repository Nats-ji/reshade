/*
 * Copyright (C) 2023 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "state_block.hpp"
#include "d3d11/d3d11_impl_state_block.hpp"

void reshade::api::create_state_block(api::device *device, state_block *out_state_block)
{
	switch (device->get_api())
	{
	case api::device_api::d3d11:
		*out_state_block = { reinterpret_cast<uintptr_t>(new d3d11::state_block(reinterpret_cast<ID3D11Device *>(device->get_native()))) };
		break;
	default:
		*out_state_block = { 0 };
	}
}
void reshade::api::destroy_state_block(api::device *device, state_block state_block)
{
	switch (device->get_api())
	{
	case api::device_api::d3d11:
		delete reinterpret_cast<d3d11::state_block *>(state_block.handle);
		break;
	}
}

void reshade::api::apply_state(api::command_list *cmd_list, state_block state_block)
{
	api::device *const device = cmd_list->get_device();

	switch (device->get_api())
	{
	case api::device_api::d3d11:
		reinterpret_cast<d3d11::state_block *>(state_block.handle)->apply_and_release();
		break;
	}
}
void reshade::api::capture_state(api::command_list *cmd_list, state_block state_block)
{
	api::device *const device = cmd_list->get_device();

	switch (device->get_api())
	{
	case api::device_api::d3d11:
		reinterpret_cast<d3d11::state_block *>(state_block.handle)->capture(reinterpret_cast<ID3D11DeviceContext *>(cmd_list->get_native()));
		break;
	}
}
