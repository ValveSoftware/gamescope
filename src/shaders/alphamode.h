#ifndef ALPHAMODE_H
#define ALPHAMODE_H

const int alpha_mode_premult = 0;
const int alpha_mode_coverage = 1;
const int alpha_mode_none = 2;

const int alpha_mode_max_bits = 4;

uint get_layer_alphamode(uint layerIdx) {
    return bitfieldExtract(u_alphaMode, int(layerIdx) * alpha_mode_max_bits, alpha_mode_max_bits);
}

vec4 BlendLayer( uint layerIdx, vec4 outputValue, vec4 layerColor, float opacity )
{
    float layerAlpha = opacity * layerColor.a;
    
    uint alphaMode = get_layer_alphamode( layerIdx );
    if ( alphaMode == alpha_mode_premult )
    {
        // wl_surfaces come with premultiplied alpha, so that's them being
        // premultiplied by layerColor.a.
        // We need to then multiply that by the layer's opacity to get to our
        // final premultiplied state.
        // For the other side of things, we need to multiply by (1.0f - (layerColor.a * opacity))
        outputValue = layerColor * opacity + outputValue * (1.0f - layerAlpha);
    }
    else if ( alphaMode == alpha_mode_coverage ) // coverage for accessibility looks
    {
        outputValue = layerColor * layerAlpha + outputValue * (1.0f - layerAlpha);
    }
    else // none
    {
        outputValue = layerColor * opacity;
    }

    return outputValue;
}

vec3 BlendLayer( uint layerIdx, vec3 outputValue, vec4 layerColor, float opacity )
{
    return BlendLayer( layerIdx, vec4( outputValue, 1 ), layerColor, opacity ).rgb;
}

#endif