#version 450
#extension GL_GOOGLE_include_directive : enable
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_spirv_intrinsics : require

#include "d3d9_fixed_function_common.glsl"

layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

const uint MaxClipPlaneCount = 6;

in gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[MaxClipPlaneCount];
} gl_in[];

out gl_PerVertex {
    vec4 gl_Position;
    float gl_ClipDistance[MaxClipPlaneCount];
};

layout(location = 0) in vec4 in_Normal[];
layout(location = 1) in vec4 in_Texcoord0[];
layout(location = 2) in vec4 in_Texcoord1[];
layout(location = 3) in vec4 in_Texcoord2[];
layout(location = 4) in vec4 in_Texcoord3[];
layout(location = 5) in vec4 in_Texcoord4[];
layout(location = 6) in vec4 in_Texcoord5[];
layout(location = 7) in vec4 in_Texcoord6[];
layout(location = 8) in vec4 in_Texcoord7[];
layout(location = 9) in vec4 in_Color0[];
layout(location = 10) in vec4 in_Color1[];
layout(location = 11) in float in_Fog[];

layout(location = 0) out vec4 out_Normal;
layout(location = 1) out vec4 out_Texcoord[8];
layout(location = 9) out vec4 out_Color0;
layout(location = 10) out vec4 out_Color1;
layout(location = 11) out float out_Fog;

vec4 texCoord(uint stage, uint vertex) {
    switch (stage) {
        case 0: return in_Texcoord0[vertex];
        case 1: return in_Texcoord1[vertex];
        case 2: return in_Texcoord2[vertex];
        case 3: return in_Texcoord3[vertex];
        case 4: return in_Texcoord4[vertex];
        case 5: return in_Texcoord5[vertex];
        case 6: return in_Texcoord6[vertex];
        case 7: return in_Texcoord7[vertex];
    }

    return vec4(0.0f);
}

void main() {
    vec4 shifted[8][3];

    for (uint stage = 0; stage < TextureStageCount; stage++) {
        uint mask = bitfieldExtract(texCoordWrap, int(stage) * 4, 4);
        vec4 weights = vec4(0.0f);

        for (uint i = 0; i < 4; i++)
            weights[i] = (mask & (1u << i)) != 0u ? 1.0f : 0.0f;

        vec4 v0 = texCoord(stage, 0);
        vec4 v1 = texCoord(stage, 1);
        vec4 v2 = texCoord(stage, 2);
        shifted[stage][0] = v0;
        shifted[stage][1] = v1 - weights * floor(v1 - v0 + 0.5f);
        shifted[stage][2] = v2 - weights * floor(v2 - v0 + 0.5f);
    }

    for (uint vertex = 0; vertex < 3; vertex++) {
        gl_Position = gl_in[vertex].gl_Position;

        for (uint plane = 0; plane < MaxClipPlaneCount; plane++)
            gl_ClipDistance[plane] = gl_in[vertex].gl_ClipDistance[plane];

        out_Normal = in_Normal[vertex];

        for (uint stage = 0; stage < TextureStageCount; stage++)
            out_Texcoord[stage] = shifted[stage][vertex];

        out_Color0 = in_Color0[vertex];
        out_Color1 = in_Color1[vertex];
        out_Fog = in_Fog[vertex];
        EmitVertex();
    }

    EndPrimitive();
}
