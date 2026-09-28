// SPDX-License-Identifier: GPL-3.0-or-later
// ENW Zombies: a T4 (World at War) GfxWorld dumper for OpenAssetTools (GPL-3.0).
//
// WHY. OAT's T4 zone loader already deserialises GfxWorld out of a map fastfile, but OAT
// has no writer for it (docs/SupportedAssetTypes.md: GfxWorld dump/load both unsupported).
// Before this, the only way to a WaW world shell was Husky reading the RUNNING game's
// memory -- one game launch per map (docs/kickstart/replay.md section 4, 16). This dumper
// makes the shell an offline, file-only step: `Unlinker --include-assets gfxworld <map>.ff`.
//
// WHAT IT WRITES (both under <out>/world/):
//   <bsp>.bin   vertexCount x 8 float32 (x y z  nx ny nz  u v), then indexCount x uint16
//   <bsp>.json  counts, bounds, every surface (material, first vertex, base index, tri
//               count, brush model it belongs to), the material table (colour-map image
//               per material), every static model placement (smodelDrawInsts: model,
//               origin, 3x3 axis, scale) and the brush model table.
// Positions are raw engine units (inches), Z up -- the frame the replay is recorded in.
// Nothing is scaled or rotated. UVs are the engine's (top-left origin), as glTF wants.
//
// A surface's triangle k is  firstVertex + indices[baseIndex + 3k + {0,1,2}]  (srfTriangles_t).
//
// LIGHTING (added 2026-09-28, replay.md section 16; ADDITIVE -- every v1 field above is
// unchanged, so a reader that knows only v1 keeps working and never opens the new files):
//   <bsp>.lmap.bin            a second vertex stream, vertexCount x 12 bytes, same vertex order:
//                             f32 lmapU, f32 lmapV, u8[4] vertex colour as stored (D3DCOLOR,
//                             i.e. B G R A in memory)
//   <bsp>.lightmap<N>_primary.dds    L8, 1024x1024 on stock maps: the baked visibility of the
//                                    surface's PRIMARY light (the sun, on the stock maps)
//   <bsp>.lightmap<N>_secondary.dds  A8R8G8B8, 512x1024: two 512x512 halves over the same
//                                    lmap charts (see replay.md 16 for how they combine)
//   json "lighting"           { vertexStream, sunParse, sunLight, lightmaps[], ... }
//   json "surfaceLighting"    per surface, parallel to "surfaces":
//                             [lightmapIndex, primaryLightIndex, reflectionProbeIndex]
// Optional debugging (env ENW_GFXWORLD_SHADERS=1): the vertex/pixel shader bytecode and
// arguments of the lit techniques of the first few lightmapped world materials, under
// world/shaders/, so the engine's own lightmap maths can be disassembled (D3DDisassemble).
#include "GfxWorldDumperT4.h"

#include "Image/DdsWriter.h"
#include "Image/Dx9TextureLoader.h"
#include "Image/IwiTypes.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>
#include <cmath>
#include <format>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

using namespace T4;
using namespace nlohmann;

namespace
{
    std::string CleanImageName(const char* name)
    {
        if (!name)
            return {};
        std::string s(name);
        while (!s.empty() && (s.front() == ',' || s.front() == '*'))
            s.erase(s.begin());
        return s;
    }

    // Same preference order as OAT's XModelToCommonConverter GetMaterialColorMap for T4.
    const GfxImage* ColorMapOf(const Material* material)
    {
        if (!material || !material->textureTable)
            return nullptr;
        std::vector<const MaterialTextureDef*> c;
        for (auto i = 0u; i < material->textureCount; i++)
            if (material->textureTable[i].semantic == TS_COLOR_MAP)
                c.push_back(&material->textureTable[i]);
        if (c.empty())
            return nullptr;
        for (const auto* d : c)
            if (tolower(d->nameStart) == 'c' && tolower(d->nameEnd) == 'p')
                return d->u.image;
        for (const auto* d : c)
            if (tolower(d->nameStart) == 'r' && tolower(d->nameEnd) == 'k')
                return d->u.image;
        for (const auto* d : c)
            if (tolower(d->nameStart) == 'd' && tolower(d->nameEnd) == 'p')
                return d->u.image;
        return c[0]->u.image;
    }

    // IW3/T4 PackedUnitVec: scale = (w + 192) / 32385, v = (b - 127) * scale; normalised after.
    void UnpackUnitVec(const PackedUnitVec& p, float* out)
    {
        const float scale = (static_cast<float>(p.array[3]) + 192.0f) / 32385.0f;
        float len = 0;
        for (int i = 0; i < 3; i++)
        {
            out[i] = (static_cast<float>(p.array[i]) - 127.0f) * scale;
            len += out[i] * out[i];
        }
        len = std::sqrt(len);
        if (len > 1e-6f)
            for (int i = 0; i < 3; i++)
                out[i] /= len;
        else
        {
            out[0] = out[1] = 0;
            out[2] = 1;
        }
    }

    // An in-zone image (a lightmap is always one) -> DDS through OAT's own loader + writer.
    // Returns its json description, or null when the image has no pixels in the zone.
    json WriteImageDds(AssetDumpingContext& context, const GfxImage* image, const std::string& file)
    {
        if (!image || !image->texture.loadDef || image->texture.loadDef->resourceSize == 0)
            return nullptr;
        const auto& ld = *image->texture.loadDef;
        image::Dx9TextureLoader loader;
        loader.Width(ld.dimensions[0]).Height(ld.dimensions[1]).Depth(std::max<uint16_t>(ld.dimensions[2], 1));
        loader.Type(image::TextureType::T_2D);
        loader.Format(static_cast<oat::D3DFORMAT>(ld.format));
        loader.HasMipMaps(!(ld.flags & image::iwi6::IMG_FLAG_NOMIPMAPS));
        const auto texture = loader.LoadTexture(ld.data);
        if (!texture)
            return nullptr;
        image::DdsWriter writer;
        if (!writer.SupportsImageFormat(texture->GetFormat()))
            return nullptr;
        const auto out = context.OpenAssetFile(std::format("world/{}", file));
        if (!out)
            return nullptr;
        writer.DumpImage(*out, texture.get());
        return {
            {"file",   file                                  },
            {"name",   CleanImageName(image->name)           },
            {"width",  ld.dimensions[0]                      },
            {"height", ld.dimensions[1]                      },
            {"d3dFormat", ld.format                          },
        };
    }

    json Vec3(const float* v)
    {
        return {v[0], v[1], v[2]};
    }

    // ENW_GFXWORLD_SHADERS=1: a lit technique's passes -> world/shaders/<techset>.<tech>.p<k>.{vs,ps}
    // (raw D3D9 bytecode) plus its argument table, so the engine's lightmap maths can be read.
    void DumpTechniqueShaders(AssetDumpingContext& context, const MaterialTechniqueSet* ts, json& index)
    {
        if (!ts || !ts->name)
            return;
        for (int t : {TECHNIQUE_LIT, TECHNIQUE_LIT_SUN, TECHNIQUE_LIT_SUN_SHADOW, TECHNIQUE_UNLIT})
        {
            const auto* tech = ts->techniques[t];
            if (!tech)
                continue;
            for (auto p = 0u; p < tech->passCount; p++)
            {
                const auto& pass = tech->passArray[p];
                const auto stem = std::format("shaders/{}.{}.p{}", ts->name, t, p);
                json jp = {{"techset", ts->name}, {"technique", tech->name ? tech->name : ""}, {"type", t}, {"pass", p}};
                if (pass.vertexShader && pass.vertexShader->prog.loadDef.program)
                {
                    if (const auto f = context.OpenAssetFile(std::format("world/{}.vs", stem)))
                        f->write(reinterpret_cast<const char*>(pass.vertexShader->prog.loadDef.program),
                                 pass.vertexShader->prog.loadDef.programSize * 4u);
                    jp["vs"] = pass.vertexShader->name ? pass.vertexShader->name : "";
                }
                if (pass.pixelShader && pass.pixelShader->prog.loadDef.program)
                {
                    if (const auto f = context.OpenAssetFile(std::format("world/{}.ps", stem)))
                        f->write(reinterpret_cast<const char*>(pass.pixelShader->prog.loadDef.program),
                                 pass.pixelShader->prog.loadDef.programSize * 4u);
                    jp["ps"] = pass.pixelShader->name ? pass.pixelShader->name : "";
                }
                json args = json::array();
                const auto n = pass.perPrimArgCount + pass.perObjArgCount + pass.stableArgCount;
                for (auto a = 0; pass.args && a < n; a++)
                {
                    const auto& arg = pass.args[a];
                    json ja = {{"type", arg.type}, {"dest", arg.dest}};
                    if (arg.type == MTL_ARG_CODE_VERTEX_CONST || arg.type == MTL_ARG_CODE_PIXEL_CONST)
                        ja["code"] = {arg.u.codeConst.index, arg.u.codeConst.firstRow, arg.u.codeConst.rowCount};
                    else if (arg.type == MTL_ARG_CODE_PIXEL_SAMPLER)
                        ja["sampler"] = arg.u.codeSampler;
                    else if (arg.type == MTL_ARG_LITERAL_PIXEL_CONST || arg.type == MTL_ARG_LITERAL_VERTEX_CONST)
                    {
                        if (arg.u.literalConst)
                            ja["literal"] = {(*arg.u.literalConst)[0], (*arg.u.literalConst)[1], (*arg.u.literalConst)[2], (*arg.u.literalConst)[3]};
                    }
                    else
                        ja["nameHash"] = arg.u.nameHash;
                    args.push_back(ja);
                }
                jp["args"] = args;
                jp["stem"] = stem;
                index.push_back(jp);
            }
        }
    }

    std::string BaseName(const std::string& assetName)
    {
        // "maps/nazi_zombie_prototype.d3dbsp" -> "nazi_zombie_prototype"
        auto s = assetName;
        const auto slash = s.find_last_of("/\\");
        if (slash != std::string::npos)
            s = s.substr(slash + 1);
        const auto dot = s.find('.');
        if (dot != std::string::npos)
            s = s.substr(0, dot);
        return s;
    }
} // namespace

namespace gfx_world
{
    void DumperT4::DumpAsset(AssetDumpingContext& context, const XAssetInfo<GfxWorld>& asset)
    {
        const auto* world = asset.Asset();
        if (!world)
            return;
        const auto bsp = BaseName(asset.m_name);

        const auto vertexCount = world->vd.vertices ? world->vertexCount : 0u;
        const auto indexCount = world->indices ? static_cast<unsigned>(std::max(world->indexCount, 0)) : 0u;

        // ---- geometry -------------------------------------------------------------------
        {
            const auto bin = context.OpenAssetFile(std::format("world/{}.bin", bsp));
            if (!bin)
                return;
            std::vector<float> row(8);
            for (auto i = 0u; i < vertexCount; i++)
            {
                const auto& v = world->vd.vertices[i];
                row[0] = v.xyz[0];
                row[1] = v.xyz[1];
                row[2] = v.xyz[2];
                UnpackUnitVec(v.normal, &row[3]);
                row[6] = v.texCoord[0];
                row[7] = v.texCoord[1];
                bin->write(reinterpret_cast<const char*>(row.data()), sizeof(float) * 8);
            }
            if (indexCount)
                bin->write(reinterpret_cast<const char*>(world->indices), sizeof(uint16_t) * indexCount);
        }

        // ---- lighting stream: lightmap UV + vertex colour, same vertex order -------------
        bool wroteLmapStream = false;
        if (vertexCount)
        {
            if (const auto lm = context.OpenAssetFile(std::format("world/{}.lmap.bin", bsp)))
            {
                std::vector<char> buf(static_cast<size_t>(vertexCount) * 12u);
                for (auto i = 0u; i < vertexCount; i++)
                {
                    const auto& v = world->vd.vertices[i];
                    auto* p = buf.data() + static_cast<size_t>(i) * 12u;
                    std::memcpy(p, v.lmapCoord, 8);
                    std::memcpy(p + 8, v.color.array, 4);
                }
                lm->write(buf.data(), static_cast<std::streamsize>(buf.size()));
                wroteLmapStream = true;
            }
        }

        // ---- which brush model owns each surface (model 0 is the world itself) ----------
        const auto surfaceCount = world->dpvs.surfaces ? static_cast<unsigned>(std::max(world->surfaceCount, 0)) : 0u;
        std::vector<int> owner(surfaceCount, -1);
        json jModels = json::array();
        for (auto m = 0; world->models && m < world->modelCount; m++)
        {
            const auto& bm = world->models[m];
            jModels.push_back({
                {"index",           m                                                                        },
                {"startSurf",       bm.startSurfIndex                                                        },
                {"surfaceCount",    bm.surfaceCount                                                          },
                {"mins",            {bm.bounds[0][0], bm.bounds[0][1], bm.bounds[0][2]}                      },
                {"maxs",            {bm.bounds[1][0], bm.bounds[1][1], bm.bounds[1][2]}                      },
            });
            if (m == 0)
                continue;
            for (auto s = bm.startSurfIndex; s < bm.startSurfIndex + bm.surfaceCount && s < surfaceCount; s++)
                owner[s] = m;
        }

        // ---- surfaces + material table ---------------------------------------------------
        std::unordered_map<const Material*, size_t> matIndex;
        json jMats = json::array();
        json jSurfs = json::array();
        json jSurfLight = json::array();
        const bool dumpShaders = std::getenv("ENW_GFXWORLD_SHADERS") != nullptr;
        std::set<const MaterialTechniqueSet*> shaderSets;
        json jShaders = json::array();
        for (auto s = 0u; s < surfaceCount; s++)
        {
            const auto& surf = world->dpvs.surfaces[s];
            jSurfLight.push_back({static_cast<int>(static_cast<unsigned char>(surf.lightmapIndex)),
                                  static_cast<int>(static_cast<unsigned char>(surf.primaryLightIndex)),
                                  static_cast<int>(static_cast<unsigned char>(surf.reflectionProbeIndex))});
            if (dumpShaders && surf.material && surf.material->techniqueSet && shaderSets.size() < 8
                && static_cast<unsigned char>(surf.lightmapIndex) < static_cast<unsigned>(std::max(world->lightmapCount, 0))
                && shaderSets.insert(surf.material->techniqueSet).second)
                DumpTechniqueShaders(context, surf.material->techniqueSet, jShaders);
            size_t mi = 0;
            const auto found = matIndex.find(surf.material);
            if (found == matIndex.end())
            {
                mi = jMats.size();
                matIndex.emplace(surf.material, mi);
                const auto* mat = surf.material;
                const auto* cm = ColorMapOf(mat);
                jMats.push_back({
                    {"name",      mat && mat->info.name ? mat->info.name : ""                                   },
                    {"colorMap",  cm ? CleanImageName(cm->name) : ""                                             },
                    {"technique", mat && mat->techniqueSet && mat->techniqueSet->name ? mat->techniqueSet->name : ""},
                    {"sortKey",   mat ? mat->info.sortKey : 0                                                    },
                    {"gameFlags", mat ? mat->info.gameFlags : 0                                                  },
                    {"surfaceTypeBits", mat ? mat->info.surfaceTypeBits : 0u                                     },
                });
            }
            else
                mi = found->second;

            jSurfs.push_back({
                surf.tris.firstVertex,
                surf.tris.vertexCount,
                surf.tris.baseIndex,
                surf.tris.triCount,
                mi,
                owner[s],
                static_cast<int>(static_cast<unsigned char>(surf.flags)),
            });
        }

        // ---- static models (the props a stock map bakes in at compile time) --------------
        json jSmodels = json::array();
        for (auto i = 0u; world->dpvs.smodelDrawInsts && i < world->dpvs.smodelCount; i++)
        {
            const auto& inst = world->dpvs.smodelDrawInsts[i];
            const auto& p = inst.placement;
            jSmodels.push_back({
                {"model",  inst.model && inst.model->name ? inst.model->name : ""},
                {"origin", {p.origin[0], p.origin[1], p.origin[2]}               },
                {"axis",
                 {p.axis[0][0], p.axis[0][1], p.axis[0][2], p.axis[1][0], p.axis[1][1], p.axis[1][2], p.axis[2][0], p.axis[2][1], p.axis[2][2]}},
                {"scale",  p.scale                                               },
            });
        }

        json jSky = json::array();
        for (auto i = 0; world->skyStartSurfs && i < world->skySurfCount; i++)
            jSky.push_back(world->skyStartSurfs[i]);

        // ---- lighting: sunParse, the sun light, the lightmap pages -----------------------
        const auto& sp = world->sunParse;
        std::string spName(sp.name, strnlen(sp.name, sizeof(sp.name)));
        json jLightmaps = json::array();
        for (auto i = 0; world->lightmaps && i < world->lightmapCount; i++)
        {
            jLightmaps.push_back({
                {"primary",   WriteImageDds(context, world->lightmaps[i].primary, std::format("{}.lightmap{}_primary.dds", bsp, i))    },
                {"secondary", WriteImageDds(context, world->lightmaps[i].secondary, std::format("{}.lightmap{}_secondary.dds", bsp, i))},
            });
        }
        json jSunLight = nullptr;
        if (world->sunLight)
        {
            const auto* L = world->sunLight;
            jSunLight = {
                {"type",   L->type          },
                {"color",  Vec3(L->color)   },
                {"dir",    Vec3(L->dir)     },
                {"origin", Vec3(L->origin)  },
                {"radius", L->radius        },
            };
        }
        json jLighting = {
            {"_version",       1                                                                         },
            {"vertexStream",   wroteLmapStream ? json{{"file", std::format("{}.lmap.bin", bsp)}, {"stride", 12},
                                                      {"layout", "f32 lmapU lmapV, u8 color[4] as stored (B G R A)"}}
                                               : json(nullptr)                                           },
            {"sunParse",
             {
                 {"name", spName},
                 {"ambientScale", sp.ambientScale},
                 {"ambientColor", Vec3(sp.ambientColor)},
                 {"diffuseFraction", sp.diffuseFraction},
                 {"sunLight", sp.sunLight},
                 {"sunColor", Vec3(sp.sunColor)},
                 {"diffuseColor", Vec3(sp.diffuseColor)},
                 {"diffuseColorHasBeenSet", sp.diffuseColorHasBeenSet},
                 {"angles", Vec3(sp.angles)},
             }                                                                                           },
            {"sunLight",       jSunLight                                                                 },
            {"sunPrimaryLightIndex", world->sunPrimaryLightIndex                                         },
            {"primaryLightCount",    world->primaryLightCount                                            },
            {"lightmapCount",  std::max(world->lightmapCount, 0)                                         },
            {"lightmaps",      jLightmaps                                                                },
            {"surfaceLightingFields", {"lightmapIndex", "primaryLightIndex", "reflectionProbeIndex"}      },
        };
        if (dumpShaders)
            jLighting["shaders"] = jShaders;

        json root = {
            {"_type",             "enw-gfxworld"                                           },
            {"_version",          1                                                        },
            {"_game",             "t4"                                                     },
            {"name",              world->name ? world->name : ""                           },
            {"baseName",          world->baseName ? world->baseName : ""                   },
            {"units",             "engine inches, z up"                                    },
            {"vertexCount",       vertexCount                                              },
            {"vertexStride",      32                                                       },
            {"vertexLayout",      "f32 x y z nx ny nz u v"                                 },
            {"indexCount",        indexCount                                               },
            {"surfaceCount",      surfaceCount                                             },
            {"staticSurfaceCount", world->dpvs.staticSurfaceCount                          },
            {"litSurfs",          {world->dpvs.litSurfsBegin, world->dpvs.litSurfsEnd}      },
            {"decalSurfs",        {world->dpvs.decalSurfsBegin, world->dpvs.decalSurfsEnd}  },
            {"emissiveSurfs",     {world->dpvs.emissiveSurfsBegin, world->dpvs.emissiveSurfsEnd}},
            {"skyStartSurfs",     jSky                                                     },
            {"skyBoxModel",       world->skyBoxModel ? world->skyBoxModel : ""             },
            {"mins",              {world->mins[0], world->mins[1], world->mins[2]}         },
            {"maxs",              {world->maxs[0], world->maxs[1], world->maxs[2]}         },
            {"sunColorFromBsp",   {world->sunColorFromBsp[0], world->sunColorFromBsp[1], world->sunColorFromBsp[2]}},
            {"surfaceFields",     {"firstVertex", "vertexCount", "baseIndex", "triCount", "material", "brushModel", "flags"}},
            {"materials",         jMats                                                    },
            {"surfaces",          jSurfs                                                   },
            {"brushModels",       jModels                                                  },
            {"staticModels",      jSmodels                                                 },
            {"lighting",          jLighting                                                },
            {"surfaceLighting",   jSurfLight                                               },
        };

        const auto js = context.OpenAssetFile(std::format("world/{}.json", bsp));
        if (!js)
            return;
        *js << root.dump() << "\n";
    }
} // namespace gfx_world
