/**
 * @file toneMapF.glsl
 *
 * $LicenseInfo:firstyear=2021&license=viewerlgpl$
 * Alchemy Viewer Source Code
 * Copyright (C) 2021
 * $/LicenseInfo$
 */
/*[EXTRA_CODE_HERE]*/
out vec4 frag_color;
in vec2 vary_fragcoord;

uniform sampler2D diffuseRect;
uniform sampler2D exposureMap;
uniform float exposure;
uniform float aces_mix;

// Shading mode (Current=0, Filmic=1)
uniform int   uShadingMode;

// Filmic options
uniform float uExposureEV;
uniform float uWB_TempK;
uniform float uWB_Tint;
uniform float uFilmicContrast;
uniform float uFilmicSaturation;

vec3 srgb_to_linear(vec3 cl);
vec3 linear_to_srgb(vec3 cl);

#if TONEMAP_METHOD == 3
void RunLPMFilter(inout vec3 diff);
#endif

// ACES Hill fit
const mat3 ACESInputMat = mat3(
    0.59719, 0.07600, 0.02840,
    0.35458, 0.90834, 0.13383,
    0.04823, 0.01566, 0.83777);
const mat3 ACESOutputMat = mat3(
    1.60475, -0.10208, -0.00327,
   -0.53108,  1.10813, -0.07276,
   -0.07367, -0.00605,  1.07602);

vec3 RRTAndODTFit(vec3 color){
    vec3 a = color * (color + 0.0245786) - 0.000090537;
    vec3 b = color * (0.983729 * color + 0.4329510) + 0.238081;
    return a / b;
}
vec3 ACES_Fitted(vec3 c){
    c = ACESInputMat * c;
    c = RRTAndODTFit(c);
    c = ACESOutputMat * c;
    return clamp(c, 0.0, 1.0);
}

// Uchimura
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
    float P = tone_uchimura_a.x;
    float a = tone_uchimura_a.y;
    float m = tone_uchimura_a.z;
    float l = tone_uchimura_b.x;
    float c = tone_uchimura_b.y;
    float b = 0.0;
    return uchimura(x, P, a, m, l, c, b);
}

// Uncharted/Hable
uniform vec3 tone_uncharted_a = vec3(0.22, 0.30, 0.10);
uniform vec3 tone_uncharted_b = vec3(0.20, 0.01, 0.30);
uniform vec3 tone_uncharted_c = vec3(8.0, 2.0, 0.0);
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

// Filmic helpers
float luma(vec3 c){ return dot(c, vec3(0.2126,0.7152,0.0722)); }
vec3 whiteBalanceGains(float tempK, float tint)
{
    float t = clamp((tempK - 6500.0) / 1000.0, -6.0, 6.0);
    float r = 1.0 + 0.08 * (-t);
    float b = 1.0 + 0.10 * ( t);
    float g = 1.0 + 0.02 * (-t);
    g *= (1.0 + clamp(tint, -1.0, 1.0) * 0.1);
    return vec3(max(r,0.01), max(g,0.01), max(b,0.01));
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

    // Filmic: ShadingMode==1 のときのみ適用
    if (uShadingMode == 1)
    {
        if (uExposureEV != 0.0)
        {
            diff.rgb *= exp2(uExposureEV);
        }
        float tempK = (uWB_TempK > 0.0) ? uWB_TempK : 6500.0;
        float tint  = uWB_Tint;
        float contr = (uFilmicContrast > 0.0) ? uFilmicContrast : 1.0;
        float sat   = (uFilmicSaturation > 0.0) ? uFilmicSaturation : 1.0;

        vec3 c = applyWhiteBalance(diff.rgb, tempK, tint);
        c = ACES_Fitted(c);
        c = desaturateHighlights(c);
        c = adjustContrastSaturation(c, contr, sat);

        diff.rgb = clamp(c, 0.0, 1.0);
        frag_color = diff;
        return;
    }

    // Current
#if TONEMAP_METHOD == 1 // Aces Hill method
    diff.rgb = mix(ACES_Fitted(diff.rgb), diff.rgb, aces_mix);
#elif TONEMAP_METHOD == 2 // Uchimura's Gran Turismo method
    diff.rgb = uchimura(diff.rgb);
#elif TONEMAP_METHOD == 3 // AMD Tonemapper
    RunLPMFilter(diff.rgb);
#elif TONEMAP_METHOD == 4 // Uncharted
    diff.rgb = uncharted2(diff.rgb);
#endif

    diff.rgb = clamp(diff.rgb, 0.0, 1.0);
    frag_color = diff;
}