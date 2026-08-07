/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

/*
 * phocus_color: Hasselblad Phocus 3D LUT color transform.
 *
 * Applies the full Phocus color pipeline (color matrix + CbCr chroma
 * correction + film curve) as a single pre-baked 33^3 trilinear LUT
 * lookup. The LUT data was extracted from Phocus 4.1 Colormaps and
 * verified against the reference Hasselblad color science.
 *
 * LUTs are loaded at runtime from ~/.config/darktable/luts/.
 * The pipeline operates in linear RGB space (darktable's internal
 * representation). No log-domain transform or gamma encoding is needed
 * since the LUT already encodes the complete Phocus response.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/file_location.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "iop/iop_api.h"
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DT_MODULE_INTROSPECTION(1, dt_iop_phocus_color_params_t)

/* ============================================================
 * Sensor type enum
 * ============================================================ */
typedef enum dt_iop_phocus_sensor_t
{
  PHOCUS_SENSOR_100MP = 0,          // $DESCRIPTION: "100MP"
  PHOCUS_SENSOR_100MP2,             // $DESCRIPTION: "100MP2"
  PHOCUS_SENSOR_100MP3,             // $DESCRIPTION: "100MP3"
  PHOCUS_SENSOR_20MP1INCH,          // $DESCRIPTION: "20MP 1-inch"
  PHOCUS_SENSOR_22MPC,              // $DESCRIPTION: "22MPC"
  PHOCUS_SENSOR_31MP,               // $DESCRIPTION: "31MP"
  PHOCUS_SENSOR_31MPC,              // $DESCRIPTION: "31MPC"
  PHOCUS_SENSOR_39MP,               // $DESCRIPTION: "39MP"
  PHOCUS_SENSOR_39MPC,              // $DESCRIPTION: "39MPC"
  PHOCUS_SENSOR_40MP5,              // $DESCRIPTION: "40MP5"
  PHOCUS_SENSOR_40MPC,              // $DESCRIPTION: "40MPC"
  PHOCUS_SENSOR_50MP5,              // $DESCRIPTION: "50MP5"
  PHOCUS_SENSOR_50MPC,              // $DESCRIPTION: "50MPC"
  PHOCUS_SENSOR_51MP5,              // $DESCRIPTION: "51MP5"
  PHOCUS_SENSOR_51MPMK2,            // $DESCRIPTION: "51MP mk2"
  PHOCUS_SENSOR_60MP5,              // $DESCRIPTION: "60MP5"
  PHOCUS_SENSOR_60MP52,             // $DESCRIPTION: "60MP52"
  PHOCUS_SENSOR_60MPC,              // $DESCRIPTION: "60MPC"
  PHOCUS_SENSOR_80MP52,             // $DESCRIPTION: "80MP52"
  PHOCUS_SENSOR_IXPRESS,            // $DESCRIPTION: "Ixpress"
  PHOCUS_SENSOR_LEICA,              // $DESCRIPTION: "Leica"
  PHOCUS_SENSOR_TZ,                 // $DESCRIPTION: "TZ"
  PHOCUS_SENSOR_COUNT               // sentinel
} dt_iop_phocus_sensor_t;

/* sensor enum value -> filename suffix mapping */
static const char *_sensor_filenames[] = {
  "100mp", "100mp2", "100mp3", "20mp1inch", "22mpc",
  "31mp", "31mpc", "39mp", "39mpc", "40mp5",
  "40mpc", "50mp5", "50mpc", "51mp5", "51mpmk2",
  "60mp5", "60mp52", "60mpc", "80mp52", "ixpress",
  "leica", "tz"
};

#define PHOCUS_LUT_SIZE 33
#define PHOCUS_LUT_ENTRIES (PHOCUS_LUT_SIZE * PHOCUS_LUT_SIZE * PHOCUS_LUT_SIZE)
#define PHOCUS_LUT_FLOATS (PHOCUS_LUT_ENTRIES * 3)

/* ============================================================
 * Params / data structs
 * ============================================================ */
typedef struct dt_iop_phocus_color_params_t
{
  dt_iop_phocus_sensor_t sensor;     // $DEFAULT: 15 $DESCRIPTION: "sensor"
  float strength;                    // $DEFAULT: 1.0 $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "strength"
} dt_iop_phocus_color_params_t;

typedef struct dt_iop_phocus_color_data_t
{
  dt_iop_phocus_sensor_t sensor;
  float strength;
  float lut[PHOCUS_LUT_FLOATS];
  int lut_valid;
} dt_iop_phocus_color_data_t;

typedef struct dt_iop_phocus_color_gui_data_t
{
  GtkWidget *sensor;
  GtkWidget *strength;
} dt_iop_phocus_color_gui_data_t;


/* ============================================================
 * .cube file parser
 * ============================================================ */
static int _parse_cube(const char *path, float *out, int max_entries)
{
  FILE *fp = g_fopen(path, "r");
  if(!fp) return 0;

  char line[512];
  int count = 0;

  while(fgets(line, sizeof(line), fp))
  {
    if(line[0] == '#' || line[0] == 'T' || line[0] == 'L' ||
       line[0] == 'D' || line[0] == '\n' || line[0] == '\r')
      continue;

    float r, g, b;
    if(sscanf(line, "%f %f %f", &r, &g, &b) == 3)
    {
      if(count < max_entries)
      {
        out[count * 3 + 0] = r;
        out[count * 3 + 1] = g;
        out[count * 3 + 2] = b;
        count++;
      }
    }
  }
  fclose(fp);
  return count;
}


/* ============================================================
 * Load LUT from disk into data struct
 * ============================================================ */
static int _load_lut(dt_iop_phocus_color_data_t *d)
{
  if(d->sensor < 0 || d->sensor >= PHOCUS_SENSOR_COUNT)
  {
    d->lut_valid = 0;
    return 0;
  }

  /* build path: ~/.config/darktable/luts/hasselblad_<sensor>_flash.cube */
  char path[1024];
  char lutdir[PATH_MAX] = { 0 };
  dt_loc_get_user_config_dir(lutdir, sizeof(lutdir));
  snprintf(path, sizeof(path), "%s%c%s%s%s",
           lutdir, G_DIR_SEPARATOR,
           "luts" G_DIR_SEPARATOR_S,
           "hasselblad_", _sensor_filenames[d->sensor]);
  snprintf(path + strlen(path), sizeof(path) - strlen(path), "%s", "_flash.cube");

  const int n = _parse_cube(path, d->lut, PHOCUS_LUT_ENTRIES);
  d->lut_valid = (n == PHOCUS_LUT_ENTRIES);

  if(!d->lut_valid)
    fprintf(stderr, "[phocus_color] failed to load LUT: %s (got %d/%d entries)\n",
            path, n, PHOCUS_LUT_ENTRIES);

  return d->lut_valid;
}


/* ============================================================
 * Main processing: 33^3 trilinear LUT lookup
 * ============================================================ */
void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  const dt_iop_phocus_color_data_t *const d = piece->data;
  const int width = roi_in->width;
  const int height = roi_in->height;
  const int ch = piece->colors;
  const size_t npixels = (size_t)width * height;

  if(ch != 4 || !d->lut_valid) return;

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;

  if(in != out)
    dt_iop_image_copy_by_size(out, in, width, height, ch);

  const float str = d->strength;
  if(str <= 0.0f) return;

  const int N = PHOCUS_LUT_SIZE;      /* 33 */
  const int N2 = N * N;                /* 1089 */
  const float scale = (float)(N - 1);  /* 32.0 */

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, npixels, str, N, N2, scale) \
    shared(d) \
    schedule(static)
#endif
  for(size_t idx = 0; idx < npixels; idx++)
  {
    float *const px = out + idx * 4;

    /* map [0,1] to [0,32] grid coordinates */
    const float rf = fminf(fmaxf(px[0], 0.0f), 1.0f) * scale;
    const float gf = fminf(fmaxf(px[1], 0.0f), 1.0f) * scale;
    const float bf = fminf(fmaxf(px[2], 0.0f), 1.0f) * scale;

    /* integer grid positions */
    const int r0 = (int)rf;
    const int g0 = (int)gf;
    const int b0 = (int)bf;
    const int r1 = (r0 < N - 1) ? r0 + 1 : r0;
    const int g1 = (g0 < N - 1) ? g0 + 1 : g0;
    const int b1 = (b0 < N - 1) ? b0 + 1 : b0;

    /* fractional parts for interpolation */
    const float fr = rf - (float)r0;
    const float fg = gf - (float)g0;
    const float fb = bf - (float)b0;

    /* 8 corner indices into the flat LUT (3 floats per entry) */
    #define LIDX(ri, gi, bi) (((bi) * N2 + (gi) * N + (ri)) * 3)

    const int i000 = LIDX(r0, g0, b0);
    const int i100 = LIDX(r1, g0, b0);
    const int i010 = LIDX(r0, g1, b0);
    const int i110 = LIDX(r1, g1, b0);
    const int i001 = LIDX(r0, g0, b1);
    const int i101 = LIDX(r1, g0, b1);
    const int i011 = LIDX(r0, g1, b1);
    const int i111 = LIDX(r1, g1, b1);

    #undef LIDX

    /* weights */
    const float w000 = (1-fr) * (1-fg) * (1-fb);
    const float w100 = fr     * (1-fg) * (1-fb);
    const float w010 = (1-fr) * fg     * (1-fb);
    const float w110 = fr     * fg     * (1-fb);
    const float w001 = (1-fr) * (1-fg) * fb;
    const float w101 = fr     * (1-fg) * fb;
    const float w011 = (1-fr) * fg     * fb;
    const float w111 = fr     * fg     * fb;

    const float *const lut = d->lut;

    /* trilinear interpolation per channel */
    for(int c = 0; c < 3; c++)
    {
      const float val = w000 * lut[i000 + c]
                       + w100 * lut[i100 + c]
                       + w010 * lut[i010 + c]
                       + w110 * lut[i110 + c]
                       + w001 * lut[i001 + c]
                       + w101 * lut[i101 + c]
                       + w011 * lut[i011 + c]
                       + w111 * lut[i111 + c];

      /* blend with original based on strength */
      px[c] = px[c] * (1.0f - str) + val * str;
    }
  }
}


/* ============================================================
 * Commit parameters
 * ============================================================ */
void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_phocus_color_data_t *d = piece->data;
  const dt_iop_phocus_color_params_t *p = (dt_iop_phocus_color_params_t *)params;

  d->strength = p->strength;

  /* reload LUT if sensor changed */
  if(d->sensor != p->sensor || !d->lut_valid)
  {
    d->sensor = p->sensor;
    _load_lut(d);
  }
}


/* ============================================================
 * Module boilerplate
 * ============================================================ */
const char *name()
{
  return "phocus color";
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  return 1;
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);  // sets params_size + allocates params from introspection
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  free(self->default_params);
  self->params = NULL;
  self->default_params = NULL;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_phocus_color_data_t *d = calloc(1, sizeof(dt_iop_phocus_color_data_t));
  piece->data = d;

  /* pre-load LUT for the current sensor */
  const dt_iop_phocus_color_params_t *p = self->params;
  d->sensor = p->sensor;
  d->strength = p->strength;
  _load_lut(d);
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_phocus_color_gui_data_t *g = self->gui_data;
  dt_iop_phocus_color_params_t *p = self->params;
  if(!g) return;

  dt_bauhaus_combobox_set(g->sensor, p->sensor);
  dt_bauhaus_slider_set(g->strength, p->strength);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_phocus_color_gui_data_t *g = malloc(sizeof(dt_iop_phocus_color_gui_data_t));
  self->gui_data = g;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->sensor = dt_bauhaus_combobox_from_params(self, "sensor");
  gtk_widget_set_tooltip_text(g->sensor,
    "Hasselblad sensor type. LUTs are loaded from "
    "~/.config/darktable/luts/hasselblad_<sensor>_flash.cube");

  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength,
    "Blend strength of the Hasselblad Phocus color transform (0=off, 1=full). "
    "The LUT encodes the complete Phocus color pipeline: color matrix, "
    "CbCr chroma correction, and film curve.");
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

/*
 * clang-format off
 * modelines: These editor modelines have been configured for use in this source file.
 * vim: set ts=2 sw=2 sts=2 noet:
 * clang-format on
 */
