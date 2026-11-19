#version 450
// The game's FXAA pass with no anti-aliasing (vk_dlaa.c, MTF_AA off or dlaa): the same input and texture binding as
// the translated FXAA fragment shader (fs 58e0efdd02dcb671: texture coordinates in input 0, the picture at set 0
// binding 1, read through its RGBA8 view), and its centre sample written unchanged.
layout(location = 0) noperspective in vec4 coord;
layout(set = 0, binding = 1) uniform sampler2D picture;
layout(location = 0) out vec4 color;

void main() {
    color = textureLod(picture, coord.xy, 0.0);
}
