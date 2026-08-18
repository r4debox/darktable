/*
 * Phocus Layers for darktable
 *
 * Implements Phocus-style local adjustment layers with masks.
 * In-tree IOP module.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Use real darktable headers for struct definitions */
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "develop/tiling.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/colorspaces.h"
#include "common/colorspaces_inline_conversions.h"
#include "common/darktable.h"
#include "control/control.h"

/* Module version - must match darktable's DT_MODULE_VERSION */
DT_MODULE(1)

/* ============================================================================
 * PHOCUS ADJUSTMENT TYPES
 * ============================================================================ */

typedef enum {
  PHOCUS_ADJ_EXPOSURE = 0,
  PHOCUS_ADJ_CONTRAST = 1,
  PHOCUS_ADJ_SATURATION = 2,
  PHOCUS_ADJ_BRIGHTNESS = 3,
  PHOCUS_ADJ_SHADOW = 4,
  PHOCUS_ADJ_HIGHLIGHT = 5,
  PHOCUS_ADJ_CLARITY = 6,
  PHOCUS_ADJ_COUNT
} phocus_adj_type_t;

/* ============================================================================
 * PHOCUS MASK TYPES
 * ============================================================================ */

typedef enum {
  PHOCUS_MASK_DISABLED = 0,
  PHOCUS_MASK_ELLIPSE = 1,
  PHOCUS_MASK_BRUSH = 2,
  PHOCUS_MASK_GRADIENT = 3,
  PHOCUS_MASK_COUNT
} phocus_mask_type_t;

/* ============================================================================
 * PHOCUS LAYER PARAMETERS
 * ============================================================================ */

typedef struct {
  /* mask type for this layer */
  int mask_type;              // phocus_mask_type_t
  
  /* ellipse mask parameters */
  float mask_center_x;        // [0,1]
  float mask_center_y;        // [0,1]
  float mask_radius_x;        // [0,1]
  float mask_radius_y;        // [0,1]
  float mask_rotation;        // radians
  float mask_feather;         // [0,1] - edge softness
  
  /* gradient mask parameters */
  float gradient_from_x;      // [0,1]
  float gradient_from_y;      // [0,1]
  float gradient_to_x;        // [0,1]
  float gradient_to_y;        // [0,1]
  float gradient_offset;      // [-1,1]
  float gradient_curvature;   // [-1,1]
  
  /* brush mask - placeholder for future implementation */
  // brush strokes would be stored here
  
  /* adjustment type for this layer */
  int adjustment_type;        // phocus_adj_type_t
  
  /* adjustment value [-1, 1] */
  float value;
  
  /* mask inversion */
  int invert_mask;            // boolean
  
  /* layer enabled */
  int enabled;                // boolean
} phocus_layer_params_t;

/* ============================================================================
 * MODULE PARAMETERS
 * ============================================================================ */

#define MAX_LAYERS 16

typedef struct {
  /* number of active layers */
  int num_layers;
  
  /* layer parameters */
  phocus_layer_params_t layers[MAX_LAYERS];
} dt_iop_phocus_params_t;

/* ============================================================================
 * MODULE GLOBAL DATA
 * ============================================================================ */

typedef struct {
  /* cached mask textures or other global data */
  int placeholder;
} dt_iop_phocus_global_data_t;

/* ============================================================================
 * MODULE INSTANCE DATA
 * ============================================================================ */

typedef struct {
  /* per-instance data (not needed for basic implementation) */
  int placeholder;
} dt_iop_phocus_data_t;

/* ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================ */



/* ============================================================================
 * MASK GENERATION
 * ============================================================================ */

/* Generate ellipse mask value at (x, y) */
static float generate_ellipse_mask(float x, float y, float center_x, float center_y,
                                   float radius_x, float radius_y, float rotation, float feather) {
  /* Translate to ellipse center */
  float dx = x - center_x;
  float dy = y - center_y;
  
  /* Rotate to ellipse local space */
  float cos_r = cosf(-rotation);
  float sin_r = sinf(-rotation);
  float lx = dx * cos_r - dy * sin_r;
  float ly = dx * sin_r + dy * cos_r;
  
  /* Ellipse equation: (lx/rx)^2 + (ly/ry)^2 = 1 */
  float dist = sqrtf((lx * lx) / (radius_x * radius_x + 1e-10f) +
                     (ly * ly) / (radius_y * radius_y + 1e-10f));
  
  /* Apply feathering */
  if (feather < 1e-6f) {
    return (dist <= 1.0f) ? 1.0f : 0.0f;
  }
  
  /* Smooth falloff */
  if (dist <= 1.0f - feather) {
    return 1.0f;
  } else if (dist >= 1.0f + feather) {
    return 0.0f;
  } else {
    /* Smooth step */
    float t = (dist - (1.0f - feather)) / (2.0f * feather);
    return 1.0f - t * t * (3.0f - 2.0f * t);
  }
}

/* Generate gradient mask value at (x, y) */
static float generate_gradient_mask(float x, float y, float from_x, float from_y,
                                    float to_x, float to_y, float offset, float curvature) {
  /* Calculate gradient direction */
  float dx = to_x - from_x;
  float dy = to_y - from_y;
  float len = sqrtf(dx * dx + dy * dy);
  
  if (len < 1e-10f) return 0.5f;
  
  /* Normalize direction */
  dx /= len;
  dy /= len;
  
  /* Project point onto gradient line */
  float px = x - from_x;
  float py = y - from_y;
  float proj = px * dx + py * dy;
  
  /* Normalize to [0, 1] range */
  float t = proj / len;
  
  /* Apply offset and curvature */
  t += offset;
  if (curvature != 0.0f) {
    t = t + curvature * t * (1.0f - t);
  }
  
  /* Clamp to [0, 1] */
  return fmaxf(0.0f, fminf(1.0f, t));
}

/* ============================================================================
 * ADJUSTMENT APPLICATION
 * ============================================================================ */

/* Apply exposure adjustment */
static inline void apply_exposure(float *pixel, float value) {
  float factor = powf(2.0f, value * 4.0f);  /* ±4 stops */
  pixel[0] *= factor;
  pixel[1] *= factor;
  pixel[2] *= factor;
}

/* Apply contrast adjustment */
static inline void apply_contrast(float *pixel, float value) {
  float mid = 0.5f;
  float scale = 1.0f + value * 2.0f;  /* ±2x contrast */
  pixel[0] = mid + (pixel[0] - mid) * scale;
  pixel[1] = mid + (pixel[1] - mid) * scale;
  pixel[2] = mid + (pixel[2] - mid) * scale;
}

/* Apply saturation adjustment */
static inline void apply_saturation(float *pixel, float value) {
  /* Luminance-preserving saturation */
  float lum = 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
  float scale = 1.0f + value * 2.0f;  /* ±2x saturation */
  pixel[0] = lum + (pixel[0] - lum) * scale;
  pixel[1] = lum + (pixel[1] - lum) * scale;
  pixel[2] = lum + (pixel[2] - lum) * scale;
}

/* Apply brightness adjustment */
static inline void apply_brightness(float *pixel, float value) {
  pixel[0] += value;
  pixel[1] += value;
  pixel[2] += value;
}

/* Apply shadow adjustment */
static inline void apply_shadow(float *pixel, float value) {
  /* Lift shadows (affects dark areas more) */
  float threshold = 0.2f;
  float lum = 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
  float shadow_weight = fmaxf(0.0f, 1.0f - lum / threshold);
  float lift = value * shadow_weight * 0.5f;
  pixel[0] += lift;
  pixel[1] += lift;
  pixel[2] += lift;
}

/* Apply highlight adjustment */
static inline void apply_highlight(float *pixel, float value) {
  /* Compress highlights (affects bright areas more) */
  float threshold = 0.8f;
  float lum = 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
  float highlight_weight = fmaxf(0.0f, (lum - threshold) / (1.0f - threshold));
  float compress = value * highlight_weight * 0.5f;
  pixel[0] -= compress;
  pixel[1] -= compress;
  pixel[2] -= compress;
}

/* Apply clarity adjustment */
static inline void apply_clarity(float *pixel, float value) {
  /* Local contrast enhancement - simplified version */
  /* In real implementation, would need surrounding pixels */
  float lum = 0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2];
  float local_contrast = fabsf(lum - 0.5f);
  float clarity_boost = value * local_contrast * 0.2f;
  pixel[0] += clarity_boost * (pixel[0] - 0.5f);
  pixel[1] += clarity_boost * (pixel[1] - 0.5f);
  pixel[2] += clarity_boost * (pixel[2] - 0.5f);
}

/* ============================================================================
 * MODULE METADATA
 * ============================================================================ */

const char *name()
{
  return "phocus layers";
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int flags() {
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

/* ============================================================================
 * MODULE INITIALIZATION
 * ============================================================================ */

void init(dt_iop_module_t *self)
{
  self->params_size = sizeof(dt_iop_phocus_params_t);
  self->default_params = calloc(1, self->params_size);
  self->params = calloc(1, self->params_size);
  self->default_enabled = 0;
  self->gui_data = NULL;
  self->data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  free(self->default_params);
  self->params = NULL;
  self->default_params = NULL;
}

/* ============================================================================
 * PIPELINE OPERATIONS
 * ============================================================================ */

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece) {
  piece->data = calloc(1, sizeof(dt_iop_phocus_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece) {
  free(piece->data);
  piece->data = NULL;
}

void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece) {
  dt_iop_phocus_data_t *d = (dt_iop_phocus_data_t *)piece->data;
  dt_iop_phocus_params_t *p = (dt_iop_phocus_params_t *)params;
  /* Copy params to pipeline data if needed */
  (void)d;  /* unused for now */
  (void)p;
}

/* ============================================================================
 * IMAGE PROCESSING
 * ============================================================================ */

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const i, void *const o, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out) {
  dt_iop_phocus_params_t *params = (dt_iop_phocus_params_t *)self->params;
  
  const float *in = (const float *)i;
  float *out = (float *)o;
  const int width = roi_out->width;
  const int height = roi_out->height;
  const int ch = piece->colors;  /* channels per pixel */
  
  /* Copy input to output (dt_iop_image_copy_by_size handles aliased in==out) */
  dt_iop_image_copy_by_size(out, in, width, height, ch);
  
  /* Apply each layer */
  for (int layer_idx = 0; layer_idx < params->num_layers && layer_idx < MAX_LAYERS; layer_idx++) {
    phocus_layer_params_t *layer = &params->layers[layer_idx];
    
    if (!layer->enabled) continue;
    
    /* Process each pixel */
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        /* Normalize coordinates to [0,1] */
        float nx = (float)x / (float)width;
        float ny = (float)y / (float)height;
        
        /* Generate mask value */
        float mask = 1.0f;
        
        switch (layer->mask_type) {
          case PHOCUS_MASK_ELLIPSE:
            mask = generate_ellipse_mask(nx, ny,
                                         layer->mask_center_x, layer->mask_center_y,
                                         layer->mask_radius_x, layer->mask_radius_y,
                                         layer->mask_rotation, layer->mask_feather);
            break;
            
          case PHOCUS_MASK_GRADIENT:
            mask = generate_gradient_mask(nx, ny,
                                         layer->gradient_from_x, layer->gradient_from_y,
                                         layer->gradient_to_x, layer->gradient_to_y,
                                         layer->gradient_offset, layer->gradient_curvature);
            break;
            
          case PHOCUS_MASK_BRUSH:
            /* Brush mask - placeholder */
            mask = 1.0f;
            break;
            
          default:
            mask = 1.0f;
            break;
        }
        
        /* Invert mask if requested */
        if (layer->invert_mask) {
          mask = 1.0f - mask;
        }
        
        /* Apply adjustment with mask */
        float *pixel = &out[(y * width + x) * ch];
        
        switch (layer->adjustment_type) {
          case PHOCUS_ADJ_EXPOSURE:
            apply_exposure(pixel, layer->value * mask);
            break;
          case PHOCUS_ADJ_CONTRAST:
            apply_contrast(pixel, layer->value * mask);
            break;
          case PHOCUS_ADJ_SATURATION:
            apply_saturation(pixel, layer->value * mask);
            break;
          case PHOCUS_ADJ_BRIGHTNESS:
            apply_brightness(pixel, layer->value * mask);
            break;
          case PHOCUS_ADJ_SHADOW:
            apply_shadow(pixel, layer->value * mask);
            break;
          case PHOCUS_ADJ_HIGHLIGHT:
            apply_highlight(pixel, layer->value * mask);
            break;
          case PHOCUS_ADJ_CLARITY:
            apply_clarity(pixel, layer->value * mask);
            break;
          default:
            break;
        }

        /* clamp pixels to safe range after adjustment to prevent NaN/inf propagation */
        pixel[0] = fminf(fmaxf(pixel[0], 0.0f), 1.0f);
        pixel[1] = fminf(fmaxf(pixel[1], 0.0f), 1.0f);
        pixel[2] = fminf(fmaxf(pixel[2], 0.0f), 1.0f);
      }
    }
  }
}

/* ============================================================================
 * GUI DEFINITIONS
 * ============================================================================ */

typedef struct {
  GtkWidget *num_layers_spin;
  GtkWidget *layer_box[MAX_LAYERS];
  GtkWidget *mask_type_combo[MAX_LAYERS];
  GtkWidget *adjustment_combo[MAX_LAYERS];
  GtkWidget *value_slider[MAX_LAYERS];
  GtkWidget *invert_check[MAX_LAYERS];
  GtkWidget *enabled_check[MAX_LAYERS];
  GtkWidget *center_x_slider[MAX_LAYERS];
  GtkWidget *center_y_slider[MAX_LAYERS];
  GtkWidget *radius_x_slider[MAX_LAYERS];
  GtkWidget *radius_y_slider[MAX_LAYERS];
  GtkWidget *rotation_slider[MAX_LAYERS];
  GtkWidget *feather_slider[MAX_LAYERS];
  GtkWidget *gradient_from_x[MAX_LAYERS];
  GtkWidget *gradient_from_y[MAX_LAYERS];
  GtkWidget *gradient_to_x[MAX_LAYERS];
  GtkWidget *gradient_to_y[MAX_LAYERS];
} dt_iop_phocus_gui_data_t;

/* ============================================================================
 * GUI CALLBACKS
 * ============================================================================ */

static void _num_layers_changed(GtkWidget *widget, dt_iop_module_t *self) {
  dt_iop_phocus_params_t *p = (dt_iop_phocus_params_t *)self->params;
  dt_iop_phocus_gui_data_t *g = (dt_iop_phocus_gui_data_t *)self->gui_data;
  
  p->num_layers = (int)gtk_spin_button_get_value(GTK_SPIN_BUTTON(g->num_layers_spin));
  
  /* Show/hide layer controls */
  for (int i = 0; i < MAX_LAYERS; i++) {
    if (g->layer_box[i]) {
      gtk_widget_set_visible(g->layer_box[i], i < p->num_layers);
    }
  }
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _value_changed(GtkWidget *widget, dt_iop_module_t *self) {
  dt_iop_phocus_params_t *p = (dt_iop_phocus_params_t *)self->params;
  dt_iop_phocus_gui_data_t *g = (dt_iop_phocus_gui_data_t *)self->gui_data;
  
  /* Find which slider changed */
  for (int i = 0; i < MAX_LAYERS; i++) {
    if (widget == g->value_slider[i]) {
      p->layers[i].value = dt_bauhaus_slider_get(widget);
      break;
    }
  }
  
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/* ============================================================================
 * GUI INITIALIZATION
 * ============================================================================ */

void gui_update(dt_iop_module_t *self)
{
  dt_iop_phocus_params_t *p = (dt_iop_phocus_params_t *)self->params;
  dt_iop_phocus_gui_data_t *g = (dt_iop_phocus_gui_data_t *)self->gui_data;
  
  if (!g) return;
  
  /* Update number of layers */
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(g->num_layers_spin), p->num_layers);
  
  /* Update layer controls */
  for (int i = 0; i < MAX_LAYERS; i++) {
    if (i < p->num_layers) {
      gtk_widget_set_visible(g->layer_box[i], TRUE);
      
      /* Update mask type combo */
      gtk_combo_box_set_active(GTK_COMBO_BOX(g->mask_type_combo[i]), p->layers[i].mask_type);
      
      /* Update adjustment combo */
      gtk_combo_box_set_active(GTK_COMBO_BOX(g->adjustment_combo[i]), p->layers[i].adjustment_type);
      
      /* Update value slider */
      dt_bauhaus_slider_set(g->value_slider[i], p->layers[i].value);
      
      /* Update invert checkbox */
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->invert_check[i]), p->layers[i].invert_mask);
      
      /* Update enabled checkbox */
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->enabled_check[i]), p->layers[i].enabled);
      
      /* Update mask geometry sliders */
      dt_bauhaus_slider_set(g->center_x_slider[i], p->layers[i].mask_center_x);
      dt_bauhaus_slider_set(g->center_y_slider[i], p->layers[i].mask_center_y);
      dt_bauhaus_slider_set(g->radius_x_slider[i], p->layers[i].mask_radius_x);
      dt_bauhaus_slider_set(g->radius_y_slider[i], p->layers[i].mask_radius_y);
      dt_bauhaus_slider_set(g->rotation_slider[i], p->layers[i].mask_rotation);
      dt_bauhaus_slider_set(g->feather_slider[i], p->layers[i].mask_feather);
      
      /* Update gradient sliders */
      dt_bauhaus_slider_set(g->gradient_from_x[i], p->layers[i].gradient_from_x);
      dt_bauhaus_slider_set(g->gradient_from_y[i], p->layers[i].gradient_from_y);
      dt_bauhaus_slider_set(g->gradient_to_x[i], p->layers[i].gradient_to_x);
      dt_bauhaus_slider_set(g->gradient_to_y[i], p->layers[i].gradient_to_y);
    } else {
      gtk_widget_set_visible(g->layer_box[i], FALSE);
    }
  }
}

void gui_init(dt_iop_module_t *self) {
  dt_iop_phocus_gui_data_t *g = calloc(1, sizeof(dt_iop_phocus_gui_data_t));
  if(!g) return;
  self->gui_data = g;
  
  GtkWidget *widget = self->widget;
  
  /* Number of layers */
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(widget), hbox, FALSE, FALSE, 0);
  
  GtkWidget *label = gtk_label_new("layers");
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, FALSE, 0);
  
  g->num_layers_spin = gtk_spin_button_new_with_range(0, MAX_LAYERS, 1);
  gtk_box_pack_start(GTK_BOX(hbox), g->num_layers_spin, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(g->num_layers_spin), "value-changed", G_CALLBACK(_num_layers_changed), self);
  
  /* Layer controls */
  for (int i = 0; i < MAX_LAYERS; i++) {
    char buf[64];
    
    /* Layer frame */
    GtkWidget *frame = gtk_frame_new(NULL);
    g->layer_box[i] = frame;
    gtk_box_pack_start(GTK_BOX(widget), frame, FALSE, FALSE, 0);
    gtk_widget_set_visible(frame, FALSE);
    
    GtkWidget *layer_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(frame), layer_vbox);
    
    /* Layer header with enable checkbox */
    GtkWidget *layer_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(layer_vbox), layer_hbox, FALSE, FALSE, 0);
    
    snprintf(buf, sizeof(buf), "layer %d", i + 1);
    label = gtk_label_new(buf);
    gtk_box_pack_start(GTK_BOX(layer_hbox), label, FALSE, FALSE, 0);
    
    g->enabled_check[i] = gtk_check_button_new_with_label("on");
    gtk_box_pack_start(GTK_BOX(layer_hbox), g->enabled_check[i], FALSE, FALSE, 0);
    
    g->invert_check[i] = gtk_check_button_new_with_label("invert");
    gtk_box_pack_start(GTK_BOX(layer_hbox), g->invert_check[i], FALSE, FALSE, 0);
    
    /* Mask type */
    GtkWidget *mask_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(layer_vbox), mask_hbox, FALSE, FALSE, 0);
    
    label = gtk_label_new("mask");
    gtk_box_pack_start(GTK_BOX(mask_hbox), label, FALSE, FALSE, 0);
    
    g->mask_type_combo[i] = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->mask_type_combo[i]), "disabled");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->mask_type_combo[i]), "ellipse");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->mask_type_combo[i]), "brush");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->mask_type_combo[i]), "gradient");
    gtk_box_pack_start(GTK_BOX(mask_hbox), g->mask_type_combo[i], TRUE, TRUE, 0);
    
    /* Adjustment type */
    GtkWidget *adj_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(layer_vbox), adj_hbox, FALSE, FALSE, 0);
    
    label = gtk_label_new("adj");
    gtk_box_pack_start(GTK_BOX(adj_hbox), label, FALSE, FALSE, 0);
    
    g->adjustment_combo[i] = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "exposure");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "contrast");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "saturation");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "brightness");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "shadow");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "highlight");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(g->adjustment_combo[i]), "clarity");
    gtk_box_pack_start(GTK_BOX(adj_hbox), g->adjustment_combo[i], TRUE, TRUE, 0);
    
    /* Value slider */
    g->value_slider[i] = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.01, 0.0, 2);
    dt_bauhaus_widget_set_label(g->value_slider[i], NULL, "value");
    gtk_box_pack_start(GTK_BOX(layer_vbox), g->value_slider[i], FALSE, FALSE, 0);
    g_signal_connect(G_OBJECT(g->value_slider[i]), "value-changed", G_CALLBACK(_value_changed), self);
    
    /* Mask geometry - ellipse */
    GtkWidget *geom_frame = gtk_frame_new("ellipse mask");
    gtk_box_pack_start(GTK_BOX(layer_vbox), geom_frame, FALSE, FALSE, 0);
    
    GtkWidget *geom_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(geom_frame), geom_vbox);
    
    g->center_x_slider[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.5, 2);
    dt_bauhaus_widget_set_label(g->center_x_slider[i], NULL, "center x");
    gtk_box_pack_start(GTK_BOX(geom_vbox), g->center_x_slider[i], FALSE, FALSE, 0);
    
    g->center_y_slider[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.5, 2);
    dt_bauhaus_widget_set_label(g->center_y_slider[i], NULL, "center y");
    gtk_box_pack_start(GTK_BOX(geom_vbox), g->center_y_slider[i], FALSE, FALSE, 0);
    
    g->radius_x_slider[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.3, 2);
    dt_bauhaus_widget_set_label(g->radius_x_slider[i], NULL, "radius x");
    gtk_box_pack_start(GTK_BOX(geom_vbox), g->radius_x_slider[i], FALSE, FALSE, 0);
    
    g->radius_y_slider[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.3, 2);
    dt_bauhaus_widget_set_label(g->radius_y_slider[i], NULL, "radius y");
    gtk_box_pack_start(GTK_BOX(geom_vbox), g->radius_y_slider[i], FALSE, FALSE, 0);
    
    g->rotation_slider[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 360.0, 1.0, 0.0, 1);
    dt_bauhaus_widget_set_label(g->rotation_slider[i], NULL, "rotation");
    gtk_box_pack_start(GTK_BOX(geom_vbox), g->rotation_slider[i], FALSE, FALSE, 0);
    
    g->feather_slider[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.1, 2);
    dt_bauhaus_widget_set_label(g->feather_slider[i], NULL, "feather");
    gtk_box_pack_start(GTK_BOX(geom_vbox), g->feather_slider[i], FALSE, FALSE, 0);
    
    /* Mask geometry - gradient */
    GtkWidget *grad_frame = gtk_frame_new("gradient mask");
    gtk_box_pack_start(GTK_BOX(layer_vbox), grad_frame, FALSE, FALSE, 0);
    
    GtkWidget *grad_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(grad_frame), grad_vbox);
    
    g->gradient_from_x[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.5, 2);
    dt_bauhaus_widget_set_label(g->gradient_from_x[i], NULL, "from x");
    gtk_box_pack_start(GTK_BOX(grad_vbox), g->gradient_from_x[i], FALSE, FALSE, 0);
    
    g->gradient_from_y[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.5, 2);
    dt_bauhaus_widget_set_label(g->gradient_from_y[i], NULL, "from y");
    gtk_box_pack_start(GTK_BOX(grad_vbox), g->gradient_from_y[i], FALSE, FALSE, 0);
    
    g->gradient_to_x[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.5, 2);
    dt_bauhaus_widget_set_label(g->gradient_to_x[i], NULL, "to x");
    gtk_box_pack_start(GTK_BOX(grad_vbox), g->gradient_to_x[i], FALSE, FALSE, 0);
    
    g->gradient_to_y[i] = dt_bauhaus_slider_new_with_range(self, 0.0, 1.0, 0.01, 0.5, 2);
    dt_bauhaus_widget_set_label(g->gradient_to_y[i], NULL, "to y");
    gtk_box_pack_start(GTK_BOX(grad_vbox), g->gradient_to_y[i], FALSE, FALSE, 0);
  }
  
  /* Initialize GUI from params */
  gui_update(self);
}

void gui_cleanup(dt_iop_module_t *self) {
  free(self->gui_data);
  self->gui_data = NULL;
}
