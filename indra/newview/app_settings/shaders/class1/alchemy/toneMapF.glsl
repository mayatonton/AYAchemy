/**
 * @file toneMapF.glsl
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2021, Rye Mutt
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

uniform int   uShadingMode;

// Filmic系オプション（全トーンマッピングタイプで有効）
uniform int   uToneMapType;       // 0..6 （Filmic=5,6）
uniform float uExposureEV;
uniform float uWB_TempK;
uniform float uWB_Tint;
uniform float uFilmicContrast;
uniform float uFilmicSaturation;
uniform float uFilmicAmount;      // 0..1
uniform float uFilmicShadowLift;  // 0..0.2 目安
uniform float uFilmicHighlightDesat; // 0..1
uniform float uFilmicTealOrange;  // 0..1

vec3 srgb_to_linear(vec3 cl);
vec3 linear_to_srgb(vec3 cl);

#if TONEMAP_METHOD == 3
void RunLPMFilter(inout vec3 diff);
#endif

// -------------------- ACES (Hill fitted) --------------------
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

// -------------------- Uchimura --------------------
vec3 uchimura(vec3 x, float P, float a, float m, float l, float c, float b)
{
    float l0 = ((P - m) * l) / a;
    float L0 = m - m / a;
    float L1 = m + (1.0 - m) / a;
    float S0 = m + l0;
    float S1 = m + a * l0;
    float C2 = (a * P) / (P - S1);
    float CP = -C2 / P;

    vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
    vec3 w2 = vec3(step(m + l0, x));
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
    float P = tone_uchimura_a.x; // max display brightness
    float a = tone_uchimura_a.y; // contrast
    float m = tone_uchimura_a.z; // linear section start
    float l = tone_uchimura_b.x; // linear section length
    float c = tone_uchimura_b.y; // black
    float b = 0.0;               // pedestal
    return uchimura(x, P, a, m, l, c, b);
}

// -------------------- Hable/Uncharted --------------------
uniform vec3 tone_uncharted_a = vec3(0.22, 0.30, 0.10); // A, B, C
uniform vec3 tone_uncharted_b = vec3(0.20, 0.01, 0.30); // D, E, F
uniform vec3 tone_uncharted_c = vec3(8.0, 2.0, 0.0);    // W, ExposureBias, Unused
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

// -------------------- Filmic補助（共通適用） --------------------
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// シャドウ持ち上げ（低輝度をsoftに持ち上げる）
vec3 liftShadows(vec3 c, float lift)
{
    // 低輝度ほど持ち上がる形：c + lift*(1 - c) に重み
    float L = luma(c);
    float f = smoothstep(0.0, 0.5, 1.0 - L); // シャドウ領域で1に近い
    return mix(c, c + lift * (1.0 - c), f);
}

// ハイライトロールオフ（肩を丸める）
vec3 rolloffHighlights(vec3 c)
{
    float L = luma(c);
    // 0.6以上で徐々に圧縮
    float t = smoothstep(0.6, 1.0, L);
    // ロールオフカーブ：上に行くほど抑える
    vec3 r = 1.0 - (1.0 - c) * (1.0 - 0.3 * t);
    return mix(c, r, t);
}

// ハイライト脱彩
vec3 desaturateHL(vec3 c, float amount)
{
    float L = luma(c);
    float t = smoothstep(0.6, 1.0, L);
    vec3 grey = vec3(L);
    return mix(c, mix(c, grey, amount), t);
}

// ティール＆オレンジのスプリットトーニング
vec3 tealOrange(vec3 c, float amount)
{
    float L = luma(c);
    vec3 shadowTint    = vec3(0.95, 1.03, 1.08); // やや青緑
    vec3 highlightTint = vec3(1.08, 1.02, 0.95); // やや橙
    float t = smoothstep(0.25, 0.85, L);
    vec3 tinted = c * mix(shadowTint, highlightTint, t);
    return mix(c, tinted, amount);
}

// コントラスト/彩度（相対）
vec3 contrastSaturation(vec3 c, float contrast, float saturation)
{
    c = (c - 0.5) * contrast + 0.5;
    float L = luma(c);
    return mix(vec3(L), c, saturation);
}

vec3 ACES_Fitted(vec3 c)
{
    c = ACESInputMat * c;
    c = RRTAndODTFit(c);
    c = ACESOutputMat * c;
    return clamp(c, 0.0, 1.0);
}

// 簡易ホワイトバランス（色温度/ティント）
vec3 whiteBalanceGains(float tempK, float tint)
{
    float t = clamp((tempK - 6500.0) / 1000.0, -6.0, 6.0);
    float r = 1.0 + 0.08 * (-t);
    float b = 1.0 + 0.10 * ( t);
    float g = 1.0 + 0.02 * (-t);
    g *= (1.0 + clamp(tint, -1.0, 1.0) * 0.1);
    return vec3(max(r, 0.01), max(g, 0.01), max(b, 0.01));
}
vec3 applyWhiteBalance(vec3 color, float tempK, float tint)
{
    if (tempK <= 0.0) tempK = 6500.0;
    vec3 gains = whiteBalanceGains(tempK, tint);
    return color * (1.0 / gains);
}
vec3 adjustContrastSaturation(vec3 c, float contrast, float saturation)
{
    c = (c - 0.5) * ((contrast > 0.0) ? contrast : 1.0) + 0.5;
    float L = luma(c);
    c = mix(vec3(L), c, (saturation > 0.0) ? saturation : 1.0);
    return c;
}
vec3 desaturateHighlights(vec3 c)
{
    float L = luma(c);
    float t = smoothstep(0.6, 1.0, L);
    vec3 grey = vec3(L);
    return mix(c, grey, t * 0.35);
}

void main()
{
    vec4 diff = texture(diffuseRect, vary_fragcoord);

#if TONEMAP_METHOD != 0
    float exp_scale = texture(exposureMap, vec2(0.5,0.5)).r;
    diff.rgb *= exposure * exp_scale;
#endif

    // EV（全方式）
    if (uExposureEV != 0.0)
    {
        diff.rgb *= exp2(uExposureEV);
    }

    // WB（全方式、トーンマップ前）
    float tempK = (uWB_TempK > 0.0) ? uWB_TempK : 6500.0;
    diff.rgb = applyWhiteBalance(diff.rgb, tempK, uWB_Tint);

    // 方式別トーンマップ（プログラム切替はC++側の選択に依存）
#if TONEMAP_METHOD == 1 // ACES Hill
    diff.rgb = mix(ACES_Hill(diff.rgb), diff.rgb, aces_mix);
#elif TONEMAP_METHOD == 2 // Uchimura
    diff.rgb = uchimura(diff.rgb);
#elif TONEMAP_METHOD == 3 // AMD LPM
    RunLPMFilter(diff.rgb);
#elif TONEMAP_METHOD == 4 // Uncharted(Hable)
    diff.rgb = uncharted2(diff.rgb);
#else
    // TONEMAP_METHOD == 0 (HDR Debug) など
#endif

    // フィニッシュ（Filmic=5,6 のときだけ適用して差を明確化）
    if (uToneMapType == 5 || uToneMapType == 6)
    {
        vec3 base = diff.rgb;

        // 1) シャドウ持ち上げ → 2) ハイライトロールオフ → 3) ハイライト脱彩 → 4) ティール＆オレンジ → 5) コントラスト/彩度
        vec3 c1 = liftShadows(base, uFilmicShadowLift);
        vec3 c2 = rolloffHighlights(c1);
        vec3 c3 = desaturateHL(c2, uFilmicHighlightDesat);
        vec3 c4 = tealOrange(c3, uFilmicTealOrange);

        float contr = (uFilmicContrast > 0.0) ? uFilmicContrast : 1.0;
        float sat   = (uFilmicSaturation > 0.0) ? uFilmicSaturation : 1.0;
        vec3  c5    = contrastSaturation(c4, contr, sat);

        // 強度ブレンド
        diff.rgb = mix(base, c5, clamp(uFilmicAmount, 0.0, 1.0));
    }

    diff.rgb = clamp(diff.rgb, 0.0, 1.0);
    frag_color = diff;
}