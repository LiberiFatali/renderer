#include "graphics.h"
#include <assert.h>
#include <float.h>
#include <stdlib.h>
#include <string.h>
#include "geometry.h"
#include "image.h"

static const int BUFFER_CHANNELS = 3;

/* context management */

context_t *gfx_create_context(int width, int height) {
    context_t *context = (context_t*)malloc(sizeof(context_t));
    assert(width > 0 && height > 0);
    context->colorbuffer = image_create(width, height, BUFFER_CHANNELS);
    context->depthbuffer = (float*)malloc(sizeof(float) * width * height);
    context->viewport    = mat4_viewport(0, 0, width, height);
    gfx_clear_buffers(context);
    return context;
}

void gfx_release_context(context_t *context) {
    image_release(context->colorbuffer);
    free(context->depthbuffer);
    free(context);
}

void gfx_clear_buffers(context_t *context) {
    image_t *colorbuffer = context->colorbuffer;
    float *depthbuffer = context->depthbuffer;
    int width = colorbuffer->width;
    int height = colorbuffer->height;
    int i;
    memset(colorbuffer->buffer, 0, width * height * BUFFER_CHANNELS);
    for (i = 0; i < width * height; i++) {
        depthbuffer[i] = FLT_MAX;
    }
}

/* triangle rasterization */

static vec3_t calculate_weights(vec2_t A, vec2_t B, vec2_t C, vec2_t P) {
    /*
     * for barycentric coordinates, see http://blackpawn.com/texts/pointinpoly/
     *
     * solve
     *     P = A + s * AB + t * AC  -->  AP = s * AB + t * AC
     * then
     *     s = (AC.y * AP.x - AC.x * AP.y) / (AB.x * AC.y - AB.y * AC.x)
     *     t = (AB.x * AP.y - AB.y * AP.x) / (AB.x * AC.y - AB.y * AC.x)
     *
     * if s < 0 or t < 0, we've walked in the wrong direction
     * if s > 1 or t > 1, we've walked too far in a direction
     * if s + t > 1, we've crossed the edge BC
     * therefore, P is in ABC only if (s >= 0) && (t >= 0) && (1 - s - t >= 0)
     *
     * note that
     *     P = A + s * AB + t * AC
     *       = A + s * (B - A) + t * (C - A)
     *       = (1 - s - t) * A + s * B + t * C
     */
    vec2_t AB = vec2_sub(B, A);
    vec2_t AC = vec2_sub(C, A);
    vec2_t AP = vec2_sub(P, A);

    float denom = AB.x * AC.y - AB.y * AC.x;
    float s = (AC.y * AP.x - AC.x * AP.y) / denom;
    float t = (AB.x * AP.y - AB.y * AP.x) / denom;

    return vec3_new(1.0f - s - t, s, t);
}

typedef struct {int min_x, min_y, max_x, max_y;} box_t;

static float min_float(float a, float b, float c, float lower_bound) {
    float min = a;
    min = (b < min) ? b : min;
    min = (c < min) ? c : min;
    min = (min < lower_bound) ? lower_bound : min;
    return min;
}

static float max_float(float a, float b, float c, float upper_bound) {
    float max = a;
    max = (b > max) ? b : max;
    max = (c > max) ? c : max;
    max = (max > upper_bound) ? upper_bound : max;
    return max;
}

static box_t find_bounding_box(int width, int height,
                               vec2_t p0, vec2_t p1, vec2_t p2) {
    box_t box;
    box.min_x = (int)min_float(p0.x, p1.x, p2.x, 0.0f);
    box.min_y = (int)min_float(p0.y, p1.y, p2.y, 0.0f);
    box.max_x = (int)(max_float(p0.x, p1.x, p2.x, (float)(width - 1)) + 0.5f);
    box.max_y = (int)(max_float(p0.y, p1.y, p2.y, (float)(height - 1)) + 0.5f);
    return box;
}

static int is_vertex_invisible(vec4_t clip_coord) {
    float x = clip_coord.x;
    float y = clip_coord.y;
    float z = clip_coord.z;
    float w = clip_coord.w;
    return (x < -w || x > w || y < -w || y > w || z < -w || z > w);
}

static int is_back_facing(vec4_t ndc_coords[3]) {
    vec3_t A = vec3_from_vec4(ndc_coords[0]);
    vec3_t B = vec3_from_vec4(ndc_coords[1]);
    vec3_t C = vec3_from_vec4(ndc_coords[2]);
    vec3_t AB = vec3_sub(B, A);
    vec3_t AC = vec3_sub(C, A);
    int is_clockwise = vec3_cross(AB, AC).z < 0;
    return is_clockwise;
}

static float calculate_depth(vec4_t screen_coords[3], vec3_t weights) {
    float depth = screen_coords[0].z * weights.x
                  + screen_coords[1].z * weights.y
                  + screen_coords[2].z * weights.z;
    return depth;
}

static float clamp_float(float a, float min, float max) {
    return (a < min) ? min : ((a > max) ? max : a);
}

static void write_fragment_color(image_t *colorbuffer, vec4_t color_,
                                 int x, int y) {
    int index = y * colorbuffer->width * BUFFER_CHANNELS + x * BUFFER_CHANNELS;
    unsigned char *pixel = &(colorbuffer->buffer[index]);
    float color[4];
    int i;
    vec4_to_array(color_, color);
    for (i = 0; i < BUFFER_CHANNELS; i++) {
        float clamped = clamp_float(color[i], 0.0f, 1.0f);
        pixel[i] = (unsigned char)(clamped * 255.0f);
    }
}

void gfx_draw_triangle(context_t *context, program_t *program) {
    /* for convenience */
    int width = context->colorbuffer->width;
    int height = context->colorbuffer->height;

    vec4_t clip_coords[3];
    vec4_t ndc_coords[3];
    vec4_t screen_coords[3];
    vec2_t screen_points[3];
    box_t box;
    int i, x, y;

    /* calculate clip coordinates */
    for (i = 0; i < 3; i++) {
        clip_coords[i] = program->vertex_shader(
            i, program->attribs, program->varyings, program->uniforms
        );
        /* naive view volume culling */
        if (is_vertex_invisible(clip_coords[i])) {
            return;
        }
    }

    /* perspective division */
    for (i = 0; i < 3; i++) {
        float factor = 1.0f / clip_coords[i].w;
        ndc_coords[i] = vec4_scale(clip_coords[i], factor);
    }
    /* back-face culling */
    if (is_back_facing(ndc_coords)) {
        return;
    }

    /* calculate screen coordinates */
    for (i = 0; i < 3; i++) {
        screen_coords[i] = mat4_mul_vec4(context->viewport, ndc_coords[i]);
        screen_points[i] = vec2_new(screen_coords[i].x, screen_coords[i].y);
    }

    /* perform rasterization */
    box = find_bounding_box(
        width, height, screen_points[0], screen_points[1], screen_points[2]
    );
    for (x = box.min_x; x <= box.max_x; x++) {
        for (y = box.min_y; y <= box.max_y; y++) {
            vec2_t point = vec2_new((float)x, (float)y);
            vec3_t weights = calculate_weights(
                screen_points[0], screen_points[1], screen_points[2], point
            );
            if (weights.x >= 0 && weights.y >= 0 && weights.z >= 0) {
                int index = y * width + x;
                float depth = calculate_depth(screen_coords, weights);
                /* early depth testing */
                if (context->depthbuffer[index] > depth) {
                    vec4_t color;
                    program->interp_varyings(program->varyings, weights);
                    color = program->fragment_shader(
                        program->varyings, program->uniforms
                    );
                    write_fragment_color(context->colorbuffer, color, x, y);
                    context->depthbuffer[index] = depth;
                }
            }
        }
    }
}

/* vector interpolation */

vec2_t gfx_interp_vec2(vec2_t vs[3], vec3_t weights_) {
    int i;
    vec2_t out = {0.0f, 0.0f};
    float weights[3];
    vec3_to_array(weights_, weights);
    for (i = 0; i < 3; i++) {
        out.x += vs[i].x * weights[i];
        out.y += vs[i].y * weights[i];
    }
    return out;
}

vec3_t gfx_interp_vec3(vec3_t vs[3], vec3_t weights_) {
    int i;
    vec3_t out = {0.0f, 0.0f, 0.0f};
    float weights[3];
    vec3_to_array(weights_, weights);
    for (i = 0; i < 3; i++) {
        out.x += vs[i].x * weights[i];
        out.y += vs[i].y * weights[i];
        out.z += vs[i].z * weights[i];
    }
    return out;
}

vec4_t gfx_interp_vec4(vec4_t vs[3], vec3_t weights_) {
    int i;
    vec4_t out = {0.0f, 0.0f, 0.0f, 0.0f};
    float weights[3];
    vec3_to_array(weights_, weights);
    for (i = 0; i < 3; i++) {
        out.x += vs[i].x * weights[i];
        out.y += vs[i].y * weights[i];
        out.z += vs[i].z * weights[i];
        out.w += vs[i].w * weights[i];
    }
    return out;
}

/* utility functions */

vec4_t gfx_sample_texture(image_t *texture, vec2_t texcoord) {
    float u = texcoord.x;
    float v = texcoord.y;
    int x = (int)((texture->width - 1) * u + 0.5f);
    int y = (int)((texture->height - 1) * v + 0.5f);
    int channels = texture->channels;
    int index = y * texture->width * channels + x * channels;
    unsigned char *pixel = &(texture->buffer[index]);
    float color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    int i;
    assert(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f);
    assert(channels >= 1 && channels <= 4);
    for (i = 0; i < channels; i++) {
        color[i] = pixel[i] / 255.0f;
    }
    return vec4_new(color[0], color[1], color[2], color[3]);
}

vec3_t gfx_reflect_light(vec3_t light, vec3_t normal) {
    /*
     * light: the incident vector
     * normal: the normal vector, should be normalized
     *
     * reflected = light - 2 * dot(light, normal) * normal
     */
    float factor = 2.0f * vec3_dot(light, normal);
    return vec3_sub(light, vec3_scale(normal, factor));
}
