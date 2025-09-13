/**
* @file alrenderutils.cpp
* @brief Alchemy Render Utility
*
* $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2021, Alchemy Viewer Project.
 * Copyright (C) 2021, Rye Mutt <rye@alchemyviewer.org>
*
* This library is free software; you can redistribute it and/or
* modify it under the terms of the GNU Lesser General Public
* License as published by the Free Software Foundation;
* version 2.1 of the License only.
*
* This library is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
* Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public
* License along with this library; if not, write to the Free Software
* Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*
* $/LicenseInfo$
*/
#include "llviewerprecompiledheaders.h"
#include "alrenderutils.h"
#include "llimagebmp.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llimagetga.h"
#include "llimagewebp.h"
#include "llrendertarget.h"
#include "llvertexbuffer.h"
#include "alcontrolcache.h"
#include "llenvironment.h"
#include "llfloatertools.h"
#include "llviewercontrol.h"
#include "llviewershadermgr.h"
#include "pipeline.h"

extern BOOL gSnapshotNoPost;
extern LLPointer<LLImageGL> gEXRImage;

#ifndef LL_WINDOWS
#define A_GCC 1
#if LL_GNUC
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wrestrict"
#endif
#endif

#define A_CPU 1
uint32_t LPM_CONTROL_BLOCK[24 * 4] = {};

#include "app_settings/shaders/class1/alchemy/LPMUtil.glsl"
#include "app_settings/shaders/class1/alchemy/CASF.glsl"

const U32 ALRENDER_BUFFER_MASK = LLVertexBuffer::MAP_VERTEX | LLVertexBuffer::MAP_TEXCOORD0 | LLVertexBuffer::MAP_TEXCOORD1;

static LLStaticHashedString al_exposure("exposure");
static LLStaticHashedString tone_uchimura_a("tone_uchimura_a");
static LLStaticHashedString tone_uchimura_b("tone_uchimura_b");
static LLStaticHashedString tonemap_amd_params("tonemap_amd");
static LLStaticHashedString tonemap_amd_params_shoulder("tonemap_amd_shoulder");
static LLStaticHashedString tone_uncharted_a("tone_uncharted_a");
static LLStaticHashedString tone_uncharted_b("tone_uncharted_b");
static LLStaticHashedString tone_uncharted_c("tone_uncharted_c");
static LLStaticHashedString sharpen_params("sharpen_params");

static LLStaticHashedString uShadingMode("uShadingMode");
static LLStaticHashedString uExposureEV("uExposureEV");
static LLStaticHashedString uWB_TempK("uWB_TempK");
static LLStaticHashedString uWB_Tint("uWB_Tint");
static LLStaticHashedString uFilmicContrast("uFilmicContrast");
static LLStaticHashedString uFilmicSaturation("uFilmicSaturation");

// Filmic UI の有効/無効（RenderShadingMode==1）を同期
static void updateFilmicUIEnabled()
{
    gSavedSettings.setBOOL("RenderFilmicUIEnabled", gSavedSettings.getU32("RenderToneMapType") == 5);
}

// LutCube（省略せず元のまま）
class LutCube
{
public:
    std::vector<unsigned char> colorCube;
    int                        size = 0;
    LutCube(const std::string& file);
    LutCube() = default;
private:
    float minX = 0.0f;
    float minY = 0.0f;
    float minZ = 0.0f;
    float maxX = 1.0f;
    float maxY = 1.0f;
    float maxZ = 1.0f;
    int currentX = 0;
    int currentY = 0;
    int currentZ = 0;
    void writeColor(int x, int y, int z, unsigned char r, unsigned char g, unsigned char b);
    void parseLine(std::string line);
    void splitTripel(std::string tripel, float& x, float& y, float& z);
    void clampTripel(float x, float y, float z, unsigned char& outX, unsigned char& outY, unsigned char& outZ);
    std::string skipWhiteSpace(std::string text);
};

LutCube::LutCube(const std::string& file)
{
    llifstream cubeStream(file);
    if (!cubeStream.good())
    {
        LL_WARNS() << "lut cube file does not exist" << LL_ENDL;
        return;
    }
    std::string line;
    while (std::getline(cubeStream, line))
    {
        parseLine(line);
    }
}

void LutCube::parseLine(std::string line)
{
    if (line.length() == 0) { return; }
    if (line[0] == '#') { return; }
    if (line.find("LUT_3D_SIZE") != std::string::npos)
    {
        line = line.substr(line.find("LUT_3D_SIZE") + 11);
        line = skipWhiteSpace(line);
        size = std::stoi(line);
        colorCube = std::vector<unsigned char>(size * size * size * 4, 255);
        return;
    }
    if (line.find("DOMAIN_MIN") != std::string::npos)
    {
        line = line.substr(line.find("DOMAIN_MIN") + 10);
        float x,y,z; splitTripel(line, x, y, z);
        return;
    }
    if (line.find("DOMAIN_MAX") != std::string::npos)
    {
        line = line.substr(line.find("DOMAIN_MAX") + 10);
        float x,y,z; splitTripel(line, x, y, z);
        return;
    }
    if (line.find_first_of("0123456789") == 0)
    {
        float x, y, z; unsigned char outX, outY, outZ;
        splitTripel(line, x, y, z);
        clampTripel(x, y, z, outX, outY, outZ);
        writeColor(currentX, currentY, currentZ, outX, outY, outZ);
        if (currentX != size - 1) { currentX++; }
        else if (currentY != size - 1) { currentY++; currentX = 0; }
        else if (currentZ != size - 1) { currentZ++; currentX = 0; currentY = 0; }
        return;
    }
}
std::string LutCube::skipWhiteSpace(std::string text)
{
    while (text.size() > 0 && (text[0] == ' ' || text[0] == '\t')) { text = text.substr(1); }
    return text;
}
void LutCube::splitTripel(std::string tripel, float& x, float& y, float& z)
{
    tripel = skipWhiteSpace(tripel);
    size_t after = tripel.find_first_of(" \n");
    x = std::stof(tripel.substr(0, after));
    tripel = tripel.substr(after);
    tripel = skipWhiteSpace(tripel);
    after = tripel.find_first_of(" \n");
    y = std::stof(tripel.substr(0, after));
    tripel = tripel.substr(after);
    tripel = skipWhiteSpace(tripel);
    z = std::stof(tripel);
}
void LutCube::clampTripel(float x, float y, float z, unsigned char& outX, unsigned char& outY, unsigned char& outZ)
{
    outX = (unsigned char)255 * (x / (1.0f - 0.0f));
    outY = (unsigned char)255 * (y / (1.0f - 0.0f));
    outZ = (unsigned char)255 * (z / (1.0f - 0.0f));
}
void LutCube::writeColor(int x, int y, int z, unsigned char r, unsigned char g, unsigned char b)
{
    static const int colorSize = 4;
    int locationR = (((z * size) + y) * size + x) * colorSize;
    colorCube[locationR + 0] = r;
    colorCube[locationR + 1] = g;
    colorCube[locationR + 2] = b;
}

// ---------------------------------------------------------------

ALRenderUtil::ALRenderUtil()
{
    mSettingConnections.push_back(gSavedSettings.getControl("RenderColorGrade")->getSignal()->connect(boost::bind(&ALRenderUtil::setupColorGrade, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderColorGradeLUT")->getSignal()->connect(boost::bind(&ALRenderUtil::setupColorGrade, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderToneMapType")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderExposure")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDHDRMax")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDExposure")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDContrast")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDSaturationR")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDSaturationG")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDSaturationB")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDCrosstalkR")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDCrosstalkG")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDCrosstalkB")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDShoulderContrast")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapAMDShoulderContrastRange")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapUchimuraMaxBrightness")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapUchimuraContrast")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapUchimuraLinearStart")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapUchimuraLinearLength")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapUchimuraBlackLevel")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicToeStr")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicToeLen")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicShoulderStr")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicShoulderLen")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicShoulderAngle")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicGamma")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("AlchemyToneMapFilmicWhitePoint")->getSignal()->connect(boost::bind(&ALRenderUtil::setupTonemap, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderSharpenMethod")->getSignal()->connect(boost::bind(&ALRenderUtil::setupSharpen, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderSharpenDLSSharpness")->getSignal()->connect(boost::bind(&ALRenderUtil::setupSharpen, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderSharpenDLSDenoise")->getSignal()->connect(boost::bind(&ALRenderUtil::setupSharpen, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderSharpenCASSharpness")->getSignal()->connect(boost::bind(&ALRenderUtil::setupSharpen, this)));
    mSettingConnections.push_back(gSavedSettings.getControl("RenderToneMapType")->getSignal()->connect(boost::bind(&updateFilmicUIEnabled)));
}

ALRenderUtil::~ALRenderUtil()
{
    mSettingConnections.clear();
}

void ALRenderUtil::releaseGLBuffers()
{
    if (mCGLut)
    {
        LLImageGL::deleteTextures(1, &mCGLut);
        mCGLut = 0;
    }
}

void ALRenderUtil::refreshState()
{
    setupTonemap();
    setupColorGrade();
    setupSharpen();
    updateFilmicUIEnabled();
}

bool ALRenderUtil::setupTonemap()
{
    mTonemapType = gSavedSettings.getU32("RenderToneMapType");
    if (mTonemapType >= TONEMAP_COUNT || (mTonemapType == ALTonemap::TONEMAP_AMD &&  !gDeferredPostTonemapLPMProgram.isComplete()))
    {
        mTonemapType = ALTonemap::TONEMAP_ACES_HILL;
    }

    LLGLSLShader* tone_shader = nullptr;
    switch (mTonemapType)
    {
    default:
    case ALTonemap::TONEMAP_ACES_HILL: tone_shader = &gDeferredPostTonemapACESProgram; break;
    case ALTonemap::TONEMAP_UCHIMURA:  tone_shader = &gDeferredPostTonemapUchiProgram; break;
    case ALTonemap::TONEMAP_AMD:       tone_shader = &gDeferredPostTonemapLPMProgram;  break;
    case ALTonemap::TONEMAP_UNCHARTED: tone_shader = &gDeferredPostTonemapHableProgram; break;
    }

    tone_shader->bind();
    F32 tone_exposure = llclamp(gSavedSettings.getF32("RenderExposure"), 0.5f, 4.f);
    tone_shader->uniform1f(al_exposure, tone_exposure);

    switch (mTonemapType)
    {
    default: break;
    case ALTonemap::TONEMAP_UCHIMURA:
    {
        auto u1 = LLVector3(gSavedSettings.getF32("AlchemyToneMapUchimuraMaxBrightness"),
                            gSavedSettings.getF32("AlchemyToneMapUchimuraContrast"),
                            gSavedSettings.getF32("AlchemyToneMapUchimuraLinearStart"));
        auto u2 = LLVector3(gSavedSettings.getF32("AlchemyToneMapUchimuraLinearLength"),
                            gSavedSettings.getF32("AlchemyToneMapUchimuraBlackLevel"), 0.0f);
        tone_shader->uniform3fv(tone_uchimura_a, 1, u1.mV);
        tone_shader->uniform3fv(tone_uchimura_b, 1, u2.mV);
        break;
    }
    case ALTonemap::TONEMAP_AMD:
    {
        const F32 sh_contrast_range = gSavedSettings.getF32("AlchemyToneMapAMDShoulderContrastRange");
        varAF3(saturation) = initAF3(gSavedSettings.getF32("AlchemyToneMapAMDSaturationR"),
                                     gSavedSettings.getF32("AlchemyToneMapAMDSaturationG"),
                                     gSavedSettings.getF32("AlchemyToneMapAMDSaturationB"));
        varAF3(crosstalk)  = initAF3(gSavedSettings.getF32("AlchemyToneMapAMDCrosstalkR"),
                                     gSavedSettings.getF32("AlchemyToneMapAMDCrosstalkG"),
                                     gSavedSettings.getF32("AlchemyToneMapAMDCrosstalkB"));
        LpmSetup(
            sh_contrast_range != 1.0, LPM_CONFIG_709_709, LPM_COLORS_709_709,
            0.0,
            gSavedSettings.getF32("AlchemyToneMapAMDHDRMax"),
            gSavedSettings.getF32("AlchemyToneMapAMDExposure"),
            gSavedSettings.getF32("AlchemyToneMapAMDContrast"),
            sh_contrast_range,
            saturation, crosstalk);
        tone_shader->uniform4uiv(tonemap_amd_params, 24, LPM_CONTROL_BLOCK);
        tone_shader->uniform1i(tonemap_amd_params_shoulder, sh_contrast_range != 1.0);
        break;
    }
    case ALTonemap::TONEMAP_UNCHARTED:
    {
        auto a = LLVector3(gSavedSettings.getF32("AlchemyToneMapFilmicToeStr"),
                           gSavedSettings.getF32("AlchemyToneMapFilmicToeLen"),
                           gSavedSettings.getF32("AlchemyToneMapFilmicShoulderStr"));
        auto b = LLVector3(gSavedSettings.getF32("AlchemyToneMapFilmicShoulderLen"),
                           gSavedSettings.getF32("AlchemyToneMapFilmicShoulderAngle"),
                           gSavedSettings.getF32("AlchemyToneMapFilmicGamma"));
        auto c = LLVector3(gSavedSettings.getF32("AlchemyToneMapFilmicWhitePoint"), 2.0f, 0.0f);
        tone_shader->uniform3fv(tone_uncharted_a, 1, a.mV);
        tone_shader->uniform3fv(tone_uncharted_b, 1, b.mV);
        tone_shader->uniform3fv(tone_uncharted_c, 1, c.mV);
        break;
    }
    }
    tone_shader->unbind();
    return true;
}

void ALRenderUtil::renderTonemap(LLRenderTarget* src, LLRenderTarget* exposure, LLRenderTarget* dst)
{
    dst->bindTarget();

    static LLCachedControl<bool> no_post(gSavedSettings, "RenderDisablePostProcessing", false);
    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", true);

    // RenderToneMapType: 0=HDR Debug, 1=ACES(Hill), 2=Uchimura, 3=AMD LPM, 4=Uncharted(Hable), 5=Filmic
    U32 ttype = gSavedSettings.getU32("RenderToneMapType");
    ttype = llclamp(ttype, 0, 5);

    // GUIの有効/無効を自動更新（Filmic時のみ有効）
    gSavedSettings.setBOOL("RenderFilmicUIEnabled", ttype == 5);

    LLGLSLShader* tone_shader = nullptr;

    if ((no_post && gFloaterTools->isAvailable()) ||
        LLEnvironment::instance().getCurrentSky()->getReflectionProbeAmbiance(should_auto_adjust) == 0.f)
    {
        // no-post or zero ambiance → デバッグ/HDRパス
        tone_shader = &gDeferredPostTonemapProgram;
    }
    else
    {
        switch (ttype)
        {
        case 0: tone_shader = &gDeferredPostTonemapProgram;        break; // HDR Debug
        case 1: tone_shader = &gDeferredPostTonemapACESProgram;    break; // ACES (Hill)
        case 2: tone_shader = &gDeferredPostTonemapUchiProgram;    break; // Uchimura (GT)
        case 3: tone_shader = &gDeferredPostTonemapLPMProgram;     break; // AMD LPM
        case 4: tone_shader = &gDeferredPostTonemapHableProgram;   break; // Uncharted (Hable)
        case 5: tone_shader = &gDeferredPostTonemapHableProgram;   break; // Filmic (シェーダ側で早期適用)
        default: tone_shader = &gDeferredPostTonemapACESProgram;   break;
        }
    }

    tone_shader->bind();
    tone_shader->bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_POINT);
    tone_shader->bindTexture(LLShaderMgr::EXPOSURE_MAP, exposure, false, LLTexUnit::TFO_BILINEAR);
    tone_shader->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, src->getWidth(), src->getHeight());

    static LLStaticHashedString aces_mix("aces_mix");
    tone_shader->uniform1f(aces_mix, gEXRImage.notNull() ? 0.f : 0.3f);

    // 露出（既存）
    F32 tone_exposure = llclamp(gSavedSettings.getF32("RenderExposure"), 0.5f, 4.f);
    tone_shader->uniform1f(al_exposure, tone_exposure);

    // Filmic/トーンマップ種別をシェーダへ
    static LLStaticHashedString uToneMapType_h("uToneMapType");
    tone_shader->uniform1i(uToneMapType_h, ttype);

    // Filmicパラメータ（ttype==5 のときシェーダで使われる）
    static LLStaticHashedString uExposureEV_h("uExposureEV");
    static LLStaticHashedString uWB_TempK_h("uWB_TempK");
    static LLStaticHashedString uWB_Tint_h("uWB_Tint");
    static LLStaticHashedString uFilmicContrast_h("uFilmicContrast");
    static LLStaticHashedString uFilmicSaturation_h("uFilmicSaturation");

    F32 exposure_ev     = gSavedSettings.getF32("RenderExposureEV");       // -5..+5
    F32 wb_tempK        = gSavedSettings.getF32("RenderWBTempK");          // 2500..15000
    F32 wb_tint         = gSavedSettings.getF32("RenderWBTint");           // -1..+1
    F32 film_contrast   = gSavedSettings.getF32("RenderFilmicContrast");   // 0.8..1.2
    F32 film_saturation = gSavedSettings.getF32("RenderFilmicSaturation"); // 0.8..1.2 (UI上限2.0)

    tone_shader->uniform1f(uExposureEV_h, exposure_ev);
    tone_shader->uniform1f(uWB_TempK_h, wb_tempK);
    tone_shader->uniform1f(uWB_Tint_h, wb_tint);
    tone_shader->uniform1f(uFilmicContrast_h, film_contrast);
    tone_shader->uniform1f(uFilmicSaturation_h, film_saturation);

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    stop_glerror();

    tone_shader->unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
    tone_shader->unbindTexture(LLShaderMgr::EXPOSURE_MAP, exposure->getUsage());
    tone_shader->unbind();

    dst->flush();
}

bool ALRenderUtil::setupColorGrade()
{
    if (mCGLut)
    {
        LLImageGL::deleteTextures(1, &mCGLut);
        mCGLut = 0;
    }
    if (LLPipeline::sRenderDeferred)
    {
        std::string lut_name = gSavedSettings.getString("RenderColorGradeLUT");
        if (gSavedSettings.getBOOL("RenderColorGrade") && !lut_name.empty())
        {
            std::string lut_path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "colorlut", lut_name);
            if(!LLFile::isfile(lut_path))
            {
                lut_path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "colorlut", lut_name);
            }
            if (LLFile::isfile(lut_path))
            {
                std::string temp_exten = gDirUtilp->getExtension(lut_path);
                bool decode_success = false;
                LLPointer<LLImageRaw> raw_image;
                bool flip_green = true;
                bool swap_bluegreen = true;

                if (temp_exten == "cube")
                {
                    LutCube  lutCube(lut_path);
                    if (!lutCube.colorCube.empty())
                    {
                        try
                        {
                            raw_image = new LLImageRaw(lutCube.colorCube.data(), lutCube.size * lutCube.size, lutCube.size, 4);
                        }
                        catch (const std::bad_alloc&)
                        {
                            return true;
                        }
                        flip_green = false;
                        swap_bluegreen = false;
                        decode_success = true;
                    }
                }
                else
                {
                    enum class ELutExt { EXT_IMG_TGA=0, EXT_IMG_PNG, EXT_IMG_JPEG, EXT_IMG_BMP, EXT_IMG_WEBP, EXT_NONE };
                    ELutExt extension = ELutExt::EXT_NONE;
                    if (temp_exten == "tga") extension = ELutExt::EXT_IMG_TGA;
                    else if (temp_exten == "png") extension = ELutExt::EXT_IMG_PNG;
                    else if (temp_exten == "jpg" || temp_exten == "jpeg") extension = ELutExt::EXT_IMG_JPEG;
                    else if (temp_exten == "bmp") extension = ELutExt::EXT_IMG_BMP;
                    else if (temp_exten == "webp") extension = ELutExt::EXT_IMG_WEBP;

                    raw_image = new LLImageRaw;
                    switch (extension)
                    {
                    default: break;
                    case ELutExt::EXT_IMG_TGA:
                    {
                        LLPointer<LLImageTGA> tga_image = new LLImageTGA;
                        if (tga_image->load(lut_path) && tga_image->decode(raw_image, 0.0f)) { decode_success = true; }
                        break;
                    }
                    case ELutExt::EXT_IMG_PNG:
                    {
                        LLPointer<LLImagePNG> png_image = new LLImagePNG;
                        if (png_image->load(lut_path) && png_image->decode(raw_image, 0.0f)) { decode_success = true; }
                        break;
                    }
                    case ELutExt::EXT_IMG_JPEG:
                    {
                        LLPointer<LLImageJPEG> jpg_image = new LLImageJPEG;
                        if (jpg_image->load(lut_path) && jpg_image->decode(raw_image, 0.0f)) { decode_success = true; }
                        break;
                    }
                    case ELutExt::EXT_IMG_BMP:
                    {
                        LLPointer<LLImageBMP> bmp_image = new LLImageBMP;
                        if (bmp_image->load(lut_path) && bmp_image->decode(raw_image, 0.0f)) { decode_success = true; }
                        break;
                    }
                    case ELutExt::EXT_IMG_WEBP:
                    {
                        LLPointer<LLImageWebP> webp_image = new LLImageWebP;
                        if (webp_image->load(lut_path) && webp_image->decode(raw_image, 0.0f)) { decode_success = true; }
                        break;
                    }
                    }
                }

                if (decode_success && raw_image)
                {
                    U32 primary_format = 0;
                    U32 int_format = 0;
                    switch (raw_image->getComponents())
                    {
                    case 3: primary_format = GL_RGB;  int_format = GL_RGB8;  break;
                    case 4: primary_format = GL_RGBA; int_format = GL_RGBA8; break;
                    default:
                    {
                        LL_WARNS() << "Color LUT has invalid number of color components: " << raw_image->getComponents() << LL_ENDL;
                        return true;
                    }
                    };

                    S32 image_height = raw_image->getHeight();
                    S32 image_width  = raw_image->getWidth();
                    if ((image_height > 0 && image_height <= gGLManager.mGLMaxTextureSize)
                        && ((image_height * image_height) == image_width))
                    {
                        mCGLutSize = LLVector4(image_height, (float)flip_green, (float)swap_bluegreen);
                        LLImageGL::generateTextures(1, &mCGLut);
                        gGL.getTexUnit(0)->bindManual(LLTexUnit::TT_TEXTURE_3D, mCGLut);
                        {
                            stop_glerror();
                            glTexImage3D(LLTexUnit::getInternalType(LLTexUnit::TT_TEXTURE_3D), 0, int_format,
                                         image_height, image_height, image_height, 0, primary_format, GL_UNSIGNED_BYTE, raw_image->getData());
                            stop_glerror();
                        }
                        gGL.getTexUnit(0)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
                        gGL.getTexUnit(0)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
                        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE_3D);
                    }
                    else
                    {
                        LL_WARNS() << "Color LUT is invalid width or height: " << image_height << " x " << image_width << " at path " << lut_path << LL_ENDL;
                    }
                }
                else
                {
                    LL_WARNS() << "Failed to decode color grading LUT: " << lut_path << LL_ENDL;
                }
            }
        }
    }
    return true;
}

void ALRenderUtil::renderColorGrade(LLRenderTarget* src, LLRenderTarget* dst)
{
    dst->bindTarget();

    static LLCachedControl<bool> buildNoPost(gSavedSettings, "RenderDisablePostProcessing", false);
    static LLCachedControl<bool> should_auto_adjust(gSavedSettings, "RenderSkyAutoAdjustLegacy", true);

    bool no_post = gSnapshotNoPost || (buildNoPost && gFloaterTools->isAvailable());

    LLGLSLShader* tone_shader = nullptr;
    if (mCGLut != 0)
    {
        tone_shader = no_post && gFloaterTools->isAvailable() ? &gDeferredPostColorCorrectLUTProgram[2] :
            LLEnvironment::instance().getCurrentSky()->getReflectionProbeAmbiance(should_auto_adjust) == 0.f ? &gDeferredPostColorCorrectLUTProgram[1] :
            &gDeferredPostColorCorrectLUTProgram[0];
    }
    else
    {
        tone_shader = no_post && gFloaterTools->isAvailable() ? &gDeferredPostColorCorrectProgram[2] :
            LLEnvironment::instance().getCurrentSky()->getReflectionProbeAmbiance(should_auto_adjust) == 0.f ? &gDeferredPostColorCorrectProgram[1] :
            &gDeferredPostColorCorrectProgram[0];
    }

    tone_shader->bind();
    tone_shader->bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_POINT);
    tone_shader->uniform2f(LLShaderMgr::DEFERRED_SCREEN_RES, src->getWidth(), src->getHeight());

    S32 channel = -1;
    if (mCGLut != 0)
    {
        channel = tone_shader->enableTexture(LLShaderMgr::COLORGRADE_LUT, LLTexUnit::TT_TEXTURE_3D);
        if (channel > -1)
        {
            gGL.getTexUnit(channel)->bindManual(LLTexUnit::TT_TEXTURE_3D, mCGLut);
            gGL.getTexUnit(channel)->setTextureFilteringOption(LLTexUnit::TFO_BILINEAR);
            gGL.getTexUnit(channel)->setTextureAddressMode(LLTexUnit::TAM_CLAMP);
        }
        tone_shader->uniform4fv(LLShaderMgr::COLORGRADE_LUT_SIZE, 1, mCGLutSize.mV);
    }

    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    stop_glerror();

    if (channel > -1)
    {
        gGL.getTexUnit(channel)->unbind(LLTexUnit::TT_TEXTURE_3D);
    }

    tone_shader->unbindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src->getUsage());
    tone_shader->unbind();

    dst->flush();
}

bool ALRenderUtil::setupSharpen()
{
    if (LLPipeline::sRenderDeferred)
    {
        mSharpenMethod = gSavedSettings.getU32("RenderSharpenMethod");
        if (mSharpenMethod >= SHARPEN_COUNT) { mSharpenMethod = ALSharpen::SHARPEN_CAS; }
        if (mSharpenMethod == ALSharpen::SHARPEN_CAS && !gDeferredPostCASProgram.isComplete())
        { mSharpenMethod = ALSharpen::SHARPEN_DLS; }
        if (mSharpenMethod == ALSharpen::SHARPEN_DLS && !gDeferredPostDLSProgram.isComplete())
        { mSharpenMethod = ALSharpen::SHARPEN_NONE; }

        LLGLSLShader* sharpen_shader = nullptr;
        switch (mSharpenMethod)
        {
        case ALSharpen::SHARPEN_DLS:
        {
            sharpen_shader = &gDeferredPostDLSProgram;
            sharpen_shader->bind();
            LLVector3 params = LLVector3(gSavedSettings.getF32("RenderSharpenDLSSharpness"),
                                         gSavedSettings.getF32("RenderSharpenDLSDenoise"), 0.f);
            params.clamp(LLVector3::zero, LLVector3::all_one);
            sharpen_shader->uniform3fv(sharpen_params, 1, params.mV);
            sharpen_shader->unbind();
            break;
        }
        default:
        case ALSharpen::SHARPEN_NONE:
            break;
        }
    }
    else
    {
        mSharpenMethod = ALSharpen::SHARPEN_NONE;
    }
    return true;
}

void ALRenderUtil::renderSharpen(LLRenderTarget* src, LLRenderTarget* dst)
{
    if (mSharpenMethod == ALSharpen::SHARPEN_NONE)
    {
        gPipeline.copyRenderTarget(src, dst);
        return;
    }

    LLGLSLShader* sharpen_shader = nullptr;
    switch (mSharpenMethod)
    {
    case ALSharpen::SHARPEN_CAS: sharpen_shader = &gDeferredPostCASProgram; break;
    case ALSharpen::SHARPEN_DLS: sharpen_shader = &gDeferredPostDLSProgram; break;
    default:
    case ALSharpen::SHARPEN_NONE:
        gPipeline.copyRenderTarget(src, dst);
        return;
    }

    dst->bindTarget();
    sharpen_shader->bind();

    if (mSharpenMethod == ALSharpen::SHARPEN_CAS)
    {
        static LLCachedControl<F32> cas_sharpness(gSavedSettings, "RenderSharpenCASSharpness", 0.6f);
        static LLStaticHashedString cas_param_0("cas_param_0");
        static LLStaticHashedString cas_param_1("cas_param_1");
        static LLStaticHashedString out_screen_res("out_screen_res");
        varAU4(const0); varAU4(const1);
        CasSetup(const0, const1, cas_sharpness, src->getWidth(), src->getHeight(), dst->getWidth(), dst->getHeight());
        sharpen_shader->uniform4uiv(cas_param_0, 1, const0);
        sharpen_shader->uniform4uiv(cas_param_1, 1, const1);
        sharpen_shader->uniform2f(out_screen_res, dst->getWidth(), dst->getHeight());
    }

    sharpen_shader->bindTexture(LLShaderMgr::DEFERRED_DIFFUSE, src, false, LLTexUnit::TFO_POINT);
    gPipeline.mScreenTriangleVB->setBuffer();
    gPipeline.mScreenTriangleVB->drawArrays(LLRender::TRIANGLES, 0, 3);
    sharpen_shader->unbind();
    dst->flush();
}