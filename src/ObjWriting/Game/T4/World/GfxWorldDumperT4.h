// SPDX-License-Identifier: GPL-3.0-or-later
// ENW Zombies: a T4 (World at War) GfxWorld dumper for OpenAssetTools (GPL-3.0).
// A derivative of OpenAssetTools, so GPL-3.0; built into our OAT fork's Unlinker
// (tools/maps/oat-t4-world/build-oat.ps1, tools/maps/oat-fork), which the ENW Zombies launcher
// ships as a separate program with its source published (launcher/THIRD_PARTY/OpenAssetTools).
#pragma once

#include "Dumping/AbstractAssetDumper.h"
#include "Game/T4/T4.h"

namespace gfx_world
{
    class DumperT4 final : public AbstractAssetDumper<T4::AssetGfxWorld>
    {
    protected:
        void DumpAsset(AssetDumpingContext& context, const XAssetInfo<T4::GfxWorld>& asset) override;
    };
} // namespace gfx_world
