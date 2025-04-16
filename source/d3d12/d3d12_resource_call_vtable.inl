/*
 * Copyright (C) 2022 Patrick Mours
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "hook_manager.hpp"

#define ID3D12Resource_GetDevice reshade::hooks::call_vtable<7, HRESULT, ID3D12Resource, REFIID, void **>

#define ID3D12Resource_Map(p, a, b, c) (p)->Map(a, b, c)
#define ID3D12Resource_Unmap(p, a, b) (p)->Unmap(a, b)
