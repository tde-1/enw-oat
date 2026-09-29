#pragma once

#include "Dumping/AssetDumpingContext.h"
#include "Zone/ZoneTypes.h"

#include <unordered_set>
#include <vector>

class XAssetInfoGeneric;

enum class ImageOutputFormat_e
{
    DDS,
    IWI,

    COUNT
};

const char* GetImageOutputFormatName(ImageOutputFormat_e imageOutputFormat);

enum class ModelOutputFormat_e
{
    XMODEL_EXPORT,
    XMODEL_BIN,
    OBJ,
    GLTF,
    GLB
};

class ObjWriting
{
public:
    static class Configuration_t
    {
    public:
        std::vector<bool> AssetTypesToHandleBitfield;

        ImageOutputFormat_e ImageOutputFormat = ImageOutputFormat_e::DDS;
        ModelOutputFormat_e ModelOutputFormat = ModelOutputFormat_e::GLB;
        bool MenuLegacyMode = false;

        // --include-names: when set, only the assets in this set are dumped (the named assets and
        // everything they depend on); their types still have to pass --include-assets.
        bool FilterAssetsByName = false;
        std::unordered_set<const XAssetInfoGeneric*> AssetsToDump;

    } Configuration;

    static bool ShouldHandleAssetType(asset_type_t assetType);
    static bool ShouldDumpAsset(const XAssetInfoGeneric* asset);
};
