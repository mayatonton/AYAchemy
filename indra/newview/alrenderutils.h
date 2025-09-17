/**
* @file alrenderutils.h
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

#pragma once

#include "llpointer.h"

#include "boost/signals2/connection.hpp"


class LLRenderTarget;
class LLVertexBuffer;

class ALRenderUtil
{
public:
    ALRenderUtil();
    ~ALRenderUtil();

    void releaseGLBuffers();
    void refreshState();

    // Deferred Only Functions
    enum ALTonemap : uint32_t
    {
        TONEMAP_NONE = 0,
        TONEMAP_ACES_HILL,
        TONEMAP_UCHIMURA,
        TONEMAP_AMD,
        TONEMAP_UNCHARTED,
        TONEMAP_FILMIC,
        TONEMAP_PBR,
        TONEMAP_COUNT
    };
    bool setupTonemap();
    // void updateFilmicUIEnabled();
    void renderTonemap(LLRenderTarget* src, LLRenderTarget* exposure, LLRenderTarget* dst);

    bool setupColorGrade();
    void renderColorGrade(LLRenderTarget* src, LLRenderTarget* dst);

    enum ALSharpen : uint32_t
    {
        SHARPEN_NONE = 0,
        SHARPEN_CAS,
        SHARPEN_DLS,
        SHARPEN_COUNT
    };

    bool setupSharpen();
    void renderSharpen(LLRenderTarget* src, LLRenderTarget* dst);
    // End Deferred Only

    U32 getSharpenMethod() { return mSharpenMethod; };

private:
    // State
    U32 mTonemapType = ALTonemap::TONEMAP_NONE;
    U32 mSharpenMethod = ALSharpen::SHARPEN_NONE;

    // Texture Data
    U32 mCGLut;
    LLVector4 mCGLutSize;

    std::vector<boost::signals2::scoped_connection> mSettingConnections;
};
