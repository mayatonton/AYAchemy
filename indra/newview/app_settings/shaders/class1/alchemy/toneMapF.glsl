/**
 * @file toneMapF.glsl
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2021, Rye Mutt<rye@alchemyviewer.org>
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
/*[EXTRA_CODE_HERE]*/
out vec4 frag_color;
in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform sampler2D exposureMap;
uniform float exposure;
uniform float aces_mix;

// 0=Current, 1=Filmic, 2=Toon
uniform int   uShadingMode;
uniform float uExposureEV;      // EV offset
uniform float uToonLevels;      // 2..8 推奨
uniform float uOutlineOpacity;  // 0..1
uniform float uEdgeThreshold;   // 0.02..0.5
uniform vec2  screen_res;       // set by C++ (DEFERRED_SCREEN_RES)

vec3 srgb_to_linear(vec3 cl);
vec3 linear_to_srgb(vec3 cl);

#if TONEMAP_METHOD == 3
void RunLPMFilter(inout vec3 diff);
#endif

// ACES (Hill) ----------------------------------------------------
const mat3 ACESInputMat = mat3
(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777
);
const mat3 ACESOutputMat = mat3
(
    1.60475, -0.10208, -0.00327,
    -0.53108,  1.10813, -0.07276,
    -0.07367, -0.00605,  1.07602
);
vec3 RRTAndODTFit(vec3 color)
{
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}
vec3 ACES_Hill(vec3 color)
{
    color = ACESInputMat * color;
    color = RRTAndODTFit(color);
    color = ACESOutputMat * color;
    return color;
}

// Uchimura -------------------------------------------------------
vec3 uchimura(vec3 x, float P, float a, float m, float l, float c, float b)
{
    float l0 = ((P - m) * l) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
    vec3 w2 = vec3(step(S0, x));
    vec3 w1 = vec3(1.0 - w0 - w2);

    vec3 T = vec3(m * pow(x / m, vec3(c)) + b);
    vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
    vec3 L = vec3(m + a * (x - m));

    return T * w0 + L * w1 + S * w2;
}
uniform vec3 tone_uchimura_a = vec3(1.0, 1.0, 0.22);
uniform vec3 tone_uchimura_b = vec3(0.4, 1.33, 0.0);
vec3 uchimura(vec3 x)
{
    float P = tone_uchimura_a.x;
    float a = tone_uchimura_a.y;
    float m = tone_uchimura_a.z;
    float l = tone_uchimura_b.x;
    float c = tone_uchimura_b.y;
    float b = 0.0;
    return uchimura(x, P, a, m, l, c, b);
}

// Hable/Uncharted ------------------------------------------------
uniform vec3 tone_uncharted_a = vec3(0.22, 0.30, 0.10); // A,B,C
uniform vec3 tone_uncharted_b = vec3(0.20, 0.01, 0.30); // D,E,F
uniform vec3 tone_uncharted_c = vec3(8.0, 2.0, 0.0);    // W,ExposureBias,Unused
vec3 Uncharted2Tonemap(vec3 x)
{
    float ExposureBias = tone_uncharted_c.y;
    float A = tone_uncharted_a.x * ExposureBias * ExposureBias;
    float B = tone_uncharted_a.y * ExposureBias;
    float C = tone_uncharted_a.z;
    float D = tone_uncharted_b.x;
    float E = tone_uncharted_b.y;
    float F = tone_uncharted_b.z;
    return ((x*(A*x+C*B)+D*E)/(x*(A*x+B)+D*F))-E/F;
}
vec3 uncharted2(vec3 col)
{
    return Uncharted2Tonemap(col)/Uncharted2Tonemap(vec3(tone_uncharted_c.x));
}

// Utility --------------------------------------------------------
float hash21(vec2 p){ return fract(sin(dot(p, vec2(12.9898,78.233))) * 43758.5453); }
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }

// 簡易バイラテラルフィルタ（3x3）
vec3 bilateral3x3(sampler2D tex, vec2 uv, vec2 px, float sigmaS, float sigmaR)
{
    vec3 c0 = texture(tex, uv).rgb;
    float twoSigmaS2 = 2.0 * sigmaS * sigmaS;
    float twoSigmaR2 = 2.0 * sigmaR * sigmaR;

    vec3 sum = vec3(0.0);
    float wsum = 0.0;

    for (int j=-1;j<=1;++j){
        for (int i=-1;i<=1;++i){
            vec2 offs = vec2(i,j)*px;
            vec3 c = texture(tex, uv + offs).rgb;
            float gs = exp(-(dot(offs,offs)) / twoSigmaS2);
            float gr = exp(-(dot(c - c0, c - c0)) / twoSigmaR2);
            float w = gs * gr;
            sum += c * w;
            wsum += w;
        }
    }
    return sum / max(wsum, 1e-6);
}

// Toon量子化（輝度基準で彩度維持）
vec3 toonQuantize(vec3 color, float levels, vec2 fc)
{
    float eps = 1e-5;
    float L = luma(color);
    float q = floor(L * levels + 0.5) / max(levels - 1.0, 1.0);
    vec3 outc = color * (q / max(L, eps));
    outc += (hash21(fc) - 0.5) / 255.0; // 微ディザ
    return max(outc, 0.0);
}

// 輪郭（輝度Sobel）
float edgeStrengthLuma(vec2 uv)
{
    vec2 px = 1.0 / max(screen_res, vec2(1.0));
    float tl = luma(texture(diffuseRect, uv + vec2(-px.x, -px.y)).rgb);
    float tc = luma(texture(diffuseRect, uv + vec2( 0.0  , -px.y)).rgb);
    float tr = luma(texture(diffuseRect, uv + vec2( px.x, -px.y)).rgb);
    float ml = luma(texture(diffuseRect, uv + vec2(-px.x,  0.0  )).rgb);
    float mc = luma(texture(diffuseRect, uv).rgb);
    float mr = luma(texture(diffuseRect, uv + vec2( px.x,  0.0  )).rgb);
    float bl = luma(texture(diffuseRect, uv + vec2(-px.x,  px.y)).rgb);
    float bc = luma(texture(diffuseRect, uv + vec2( 0.0  ,  px.y)).rgb);
    float br = luma(texture(diffuseRect, uv + vec2( px.x,  px.y)).rgb);

    float gx = (-1.0*tl + 1.0*tr) + (-2.0*ml + 2.0*mr) + (-1.0*bl + 1.0*br);
    float gy = (-1.0*tl - 2.0*tc - 1.0*tr) + (1.0*bl + 2.0*bc + 1.0*br);
    return sqrt(gx*gx + gy*gy);
}

// Filmic ルック（Hable後に適用）: コントラスト強化＋ハイライト脱彩＋ティール＆オレンジ気味の色相
vec3 applyFilmicLook(vec3 c)
{
    float L = luma(c);

    // コントラスト（ミッドレンジ強化）
    float contrast = 1.1;
    c = (c - 0.5) * contrast + 0.5;

    // ハイライト脱彩、シャドウはやや増彩
    float sat = mix(1.10, 0.85, smoothstep(0.45, 0.95, L));
    vec3 grey = vec3(L);
    c = mix(grey, c, sat);

    // ティール＆オレンジ風の軽いカラーグレード
    vec3 shadowTint    = vec3(1.03, 0.98, 0.95); // 暖色寄り
    vec3 highlightTint = vec3(0.95, 1.02, 1.06); // 寒色寄り
    float t = smoothstep(0.25, 0.85, L);
    c *= mix(shadowTint, highlightTint, t);

    return clamp(c, 0.0, 1.0);
}

void main()
{
    vec4 diff = texture(diffuseRect, vary_fragcoord);

#if TONEMAP_METHOD != 0 
    float exp_scale = texture(exposureMap, vec2(0.5,0.5)).r;
    diff.rgb *= exposure * exp_scale;
#endif

    // ShadingMode 分岐
    if (uShadingMode == 2)
    {
        // Toon: EV → エッジ保持でフラット化 → 段階化 → 輪郭
        diff.rgb *= exp2(uExposureEV);

        vec2 px = 1.0 / max(screen_res, vec2(1.0));
        // sigmaS(空間), sigmaR(色差) は経験値。sigmaRを小さめにするとエッジ保持強く
        vec3 flat = bilateral3x3(diffuseRect, vary_fragcoord, px, 1.0, 0.08);

        vec3 toon = toonQuantize(flat, max(uToonLevels, 2.0), vary_fragcoord);

        float e = edgeStrengthLuma(vary_fragcoord) * 4.0; // 係数で見やすく
        float th = (uEdgeThreshold > 0.0) ? uEdgeThreshold : 0.12;
        float edge = smoothstep(th, th * 2.5, e);
        float op = clamp(uOutlineOpacity, 0.0, 1.0);

        vec3 outlined = mix(toon, vec3(0.0), edge * op);

        diff.rgb = clamp(outlined, 0.0, 1.0);
        frag_color = diff;
        return;
    }

    if (uShadingMode == 1)
    {
        // Filmic: EV だけ先に適用
        diff.rgb *= exp2(uExposureEV);
        // 以降のトーンマップはTONEMAP_METHODに委譲（C++側でHableを選択させる）
    }

    // Current または Filmic（EV適用済）: 選択トーンマップ
#if TONEMAP_METHOD == 1 // ACES Hill method
    diff.rgb = mix(ACES_Hill(diff.rgb), diff.rgb, aces_mix);
#elif TONEMAP_METHOD == 2 // Uchimura
    diff.rgb = uchimura(diff.rgb);
#elif TONEMAP_METHOD == 3 // AMD Tonemapper
    RunLPMFilter(diff.rgb);
#elif TONEMAP_METHOD == 4 // Uncharted (Hable)
    diff.rgb = uncharted2(diff.rgb);
#endif

    // Filmic のときだけ追加のフィルム調ルックを適用
    if (uShadingMode == 1)
    {
        diff.rgb = applyFilmicLook(diff.rgb);
    }

    diff.rgb = clamp(diff.rgb, 0, 1);
    frag_color = diff;
}