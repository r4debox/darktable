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
 * phocus_color: Hasselblad Phocus global color pipeline.
 *
 * Implements the full Phocus raw-to-output color science:
 *   1. Log-domain 3x3 color matrix (illuminant-interpolated)
 *   2. CbCr chroma correction LUT (bilinear interpolation)
 *   3. Film curve (hermite spline tone mapping)
 *   4. Gamma 2.2 TRC
 *
 * Color profile data for Hasselblad 60MP5 sensor.
 *
 * Operates in linear RGB on the display-referred portion of the pipeline.
 * Place after colorin, before colorout in the pixel pipe.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "phocus_colordata_60mp5.h"

DT_MODULE_INTROSPECTION(1, dt_iop_phocus_color_params_t)

/* ============================================================
 * Film curve types (FCI index from Phocus)
 * ============================================================ */
typedef enum dt_iop_phocus_filmcurve_t
{
  PHOCUS_FILM_LINEAR = 0,       // $DESCRIPTION: "Linear"
  PHOCUS_FILM_STANDARD,         // $DESCRIPTION: "Standard"
  PHOCUS_FILM_PORTRAIT,         // $DESCRIPTION: "Portrait"
  PHOCUS_FILM_PRODUCT,          // $DESCRIPTION: "Product"
  PHOCUS_FILM_NATURE,           // $DESCRIPTION: "Nature"
  PHOCUS_FILM_LANDSCAPE,        // $DESCRIPTION: "Landscape"
  PHOCUS_FILM_HIGHKEY,          // $DESCRIPTION: "High Key"
  PHOCUS_FILM_LOWKEY,           // $DESCRIPTION: "Low Key"
  PHOCUS_FILM_REPRO,            // $DESCRIPTION: "Reproduction"
} dt_iop_phocus_filmcurve_t;

#define PHOCUS_FILM_CURVE_SAMPLES 256

/* ============================================================
 * Illuminant mode: controls which CbCr LUT to use
 * ============================================================ */
typedef enum dt_iop_phocus_illuminant_t
{
  PHOCUS_ILLUM_FLASH = 0,       // $DESCRIPTION: "Flash (5600K)"
  PHOCUS_ILLUM_TUNGSTEN,        // $DESCRIPTION: "Tungsten (3000K)"
} dt_iop_phocus_illuminant_t;

/* ============================================================
 * CbCr LUT selection (standard vs repro)
 * ============================================================ */
typedef enum dt_iop_phocus_chroma_mode_t
{
  PHOCUS_CHROMA_STD = 0,        // $DESCRIPTION: "Standard"
  PHOCUS_CHROMA_REPRO,          // $DESCRIPTION: "Reproduction"
} dt_iop_phocus_chroma_mode_t;

typedef struct dt_iop_phocus_color_params_t
{
  dt_iop_phocus_filmcurve_t film_curve;     // $DEFAULT: PHOCUS_FILM_STANDARD $DESCRIPTION: "film curve"
  dt_iop_phocus_illuminant_t illuminant;     // $DEFAULT: PHOCUS_ILLUM_FLASH $DESCRIPTION: "illuminant"
  dt_iop_phocus_chroma_mode_t chroma_mode;   // $DEFAULT: PHOCUS_CHROMA_STD $DESCRIPTION: "chroma LUT"
  float matrix_strength;                      // $DEFAULT: 1.0 $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "matrix strength"
  float chroma_strength;                      // $DEFAULT: 1.0 $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "chroma strength"
  float curve_strength;                       // $DEFAULT: 1.0 $MIN: 0.0 $MAX: 1.0 $DESCRIPTION: "curve strength"
} dt_iop_phocus_color_params_t;

typedef struct dt_iop_phocus_color_data_t
{
  dt_iop_phocus_filmcurve_t film_curve;
  dt_iop_phocus_illuminant_t illuminant;
  dt_iop_phocus_chroma_mode_t chroma_mode;
  float matrix_strength;
  float chroma_strength;
  float curve_strength;
  /* precomputed film curve LUT */
  float film_lut[PHOCUS_FILM_CURVE_SAMPLES + 1];
} dt_iop_phocus_color_data_t;

typedef struct dt_iop_phocus_color_gui_data_t
{
  GtkWidget *film_curve;
  GtkWidget *illuminant;
  GtkWidget *chroma_mode;
  GtkWidget *matrix_strength;
  GtkWidget *chroma_strength;
  GtkWidget *curve_strength;
} dt_iop_phocus_color_gui_data_t;


/* ============================================================
 * Hermite spline evaluation (Phocus FilmCurveMaker algorithm)
 * ============================================================ */
static inline float _hermite(float t, float m0, float m1, float p0, float p1)
{
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
  const float h10 = t3 - 2.0f * t2 + t;
  const float h01 = -2.0f * t3 + 3.0f * t2;
  const float h11 = t3 - t2;
  return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
}

/* Generate film curve LUT for a given curve type.
 * The curve maps [0,1] -> [0,1].
 *
 * Phocus uses a two-segment hermite spline with:
 *   - Segment 1: (0,0) to (x1,y1) with tangent m0
 *   - Segment 2: (x1,y1) to (1,1) with tangent m1
 *
 * R, g, b are the fixed hermite coefficients.
 * The curve shape is controlled by the split point (x1,y1)
 * and the tangents at the endpoints.
 *
 * Curves derived from Phocus ICC profile analysis and the
 * FilmCurveMaker class behavior.
 */
static void _generate_film_curve(dt_iop_phocus_filmcurve_t type, float *lut)
{
  /* control points: (split_x, split_y, tangent_in, tangent_out) */
  float x1, y1, m0, m1;

  switch(type)
  {
    case PHOCUS_FILM_LINEAR:
      /* pure linear */
      for(int i = 0; i <= PHOCUS_FILM_CURVE_SAMPLES; i++)
        lut[i] = (float)i / (float)PHOCUS_FILM_CURVE_SAMPLES;
      return;

    case PHOCUS_FILM_STANDARD:
      /* standard Hasselblad response - gentle S-curve
       * Lifts shadows slightly, compresses highlights */
      x1 = 0.5f; y1 = 0.55f; m0 = 1.15f; m1 = 0.85f;
      break;

    case PHOCUS_FILM_PORTRAIT:
      /* softer highlights, gentler rolloff, lifted midtones */
      x1 = 0.45f; y1 = 0.55f; m0 = 1.2f; m1 = 0.78f;
      break;

    case PHOCUS_FILM_PRODUCT:
      /* neutral, accurate - minimal curve, close to linear */
      x1 = 0.5f; y1 = 0.52f; m0 = 1.05f; m1 = 0.95f;
      break;

    case PHOCUS_FILM_NATURE:
      /* higher contrast, punchier - steeper midtone */
      x1 = 0.5f; y1 = 0.58f; m0 = 1.25f; m1 = 0.75f;
      break;

    case PHOCUS_FILM_LANDSCAPE:
      /* enhanced blues/greens - lifted shadows, controlled highlights */
      x1 = 0.48f; y1 = 0.56f; m0 = 1.2f; m1 = 0.80f;
      break;

    case PHOCUS_FILM_HIGHKEY:
      /* bright, airy - strong shadow lift, gentle highlight rolloff */
      x1 = 0.4f; y1 = 0.6f; m0 = 1.4f; m1 = 0.65f;
      break;

    case PHOCUS_FILM_LOWKEY:
      /* dark, moody - deep shadows, contrasty midtones */
      x1 = 0.55f; y1 = 0.48f; m0 = 0.95f; m1 = 1.1f;
      break;

    case PHOCUS_FILM_REPRO:
      /* reproduction - nearly flat, minimal contrast */
      x1 = 0.5f; y1 = 0.505f; m0 = 1.01f; m1 = 0.99f;
      break;

    default:
      for(int i = 0; i <= PHOCUS_FILM_CURVE_SAMPLES; i++)
        lut[i] = (float)i / (float)PHOCUS_FILM_CURVE_SAMPLES;
      return;
  }

  /* two-segment hermite spline */
  for(int i = 0; i <= PHOCUS_FILM_CURVE_SAMPLES; i++)
  {
    const float t = (float)i / (float)PHOCUS_FILM_CURVE_SAMPLES;
    float y;
    if(t <= x1)
    {
      /* segment 1: (0,0) -> (x1,y1) */
      const float s = t / x1;
      y = _hermite(s, m0 * x1, m1 * x1, 0.0f, y1);
    }
    else
    {
      /* segment 2: (x1,y1) -> (1,1) */
      const float s = (t - x1) / (1.0f - x1);
      y = _hermite(s, m1 * (1.0f - x1), m1 * (1.0f - x1), y1, 1.0f);
    }
    /* clamp to [0,1] */
    lut[i] = fmaxf(0.0f, fminf(1.0f, y));
  }
}


/* ============================================================
 * Color matrix lookup and interpolation
 * ============================================================ */
static inline void _get_matrix(dt_iop_phocus_illuminant_t illum, const float **matrix)
{
  switch(illum)
  {
    case PHOCUS_ILLUM_FLASH:
      *matrix = phocus60mp5_flash_matrix;
      break;
    case PHOCUS_ILLUM_TUNGSTEN:
      *matrix = phocus60mp5_tungsten_matrix;
      break;
    default:
      *matrix = phocus60mp5_flash_matrix;
      break;
  }
}

static inline void _get_chroma_lut(dt_iop_phocus_illuminant_t illum,
                                    dt_iop_phocus_chroma_mode_t mode,
                                    const float **lut, int *cb_s, int *cb_e, int *cr_s, int *cr_e)
{
  *cb_s = phocus60mp5_cb_s;
  *cb_e = phocus60mp5_cb_e;
  *cr_s = phocus60mp5_cr_s;
  *cr_e = phocus60mp5_cr_e;

  if(illum == PHOCUS_ILLUM_FLASH)
    *lut = (mode == PHOCUS_CHROMA_REPRO) ? phocus60mp5_chroma_flash_repro : phocus60mp5_chroma_flash_std;
  else
    *lut = (mode == PHOCUS_CHROMA_REPRO) ? phocus60mp5_chroma_ts_repro : phocus60mp5_chroma_ts_std;
}


/* ============================================================
 * Main processing
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

  if(ch != 4) return;

  const float *const in = (const float *)ivoid;
  float *const out = (float *)ovoid;

  if(in != out)
    dt_iop_image_copy_by_size(out, in, width, height, ch);

  /* --- stage 1: log-domain 3x3 color matrix --- */
  const float *matrix;
  _get_matrix(d->illuminant, &matrix);

  if(d->matrix_strength > 0.0f)
  {
    const float str = d->matrix_strength;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, npixels, matrix, str) \
    schedule(static)
#endif
    for(size_t idx = 0; idx < npixels; idx++)
    {
      float *const px = out + idx * 4;

      /* convert to log2 domain, clamping to avoid -inf */
      const float lr = log2f(fmaxf(px[0], 1e-10f));
      const float lg = log2f(fmaxf(px[1], 1e-10f));
      const float lb = log2f(fmaxf(px[2], 1e-10f));

      /* apply 3x3 matrix in log domain */
      const float or_ = matrix[0] * lr + matrix[1] * lg + matrix[2] * lb;
      const float og = matrix[3] * lr + matrix[4] * lg + matrix[5] * lb;
      const float ob = matrix[6] * lr + matrix[7] * lg + matrix[8] * lb;

      /* convert back to linear and blend with strength */
      const float nr = exp2f(or_);
      const float ng = exp2f(og);
      const float nb = exp2f(ob);

      px[0] = px[0] * (1.0f - str) + nr * str;
      px[1] = px[1] * (1.0f - str) + ng * str;
      px[2] = px[2] * (1.0f - str) + nb * str;
    }
  }

  /* --- stage 2: CbCr chroma correction --- */
  const float *chroma_lut;
  int cb_s, cb_e, cr_s, cr_e;
  _get_chroma_lut(d->illuminant, d->chroma_mode, &chroma_lut, &cb_s, &cb_e, &cr_s, &cr_e);

  const int cb_range = cb_e - cb_s + 1;   /* 105 */
  const int cr_range = cr_e - cr_s + 1;   /* 89 */
  const float div_factor = (float)phocus60mp5_div_factor;
  const float chroma_str = d->chroma_strength;

  if(chroma_str > 0.0f)
  {
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, npixels, chroma_lut, cb_s, cb_e, cr_s, cr_e, \
                        cb_range, cr_range, div_factor, chroma_str) \
    schedule(static)
#endif
    for(size_t idx = 0; idx < npixels; idx++)
    {
      float *const px = out + idx * 4;
      const float r = px[0], g = px[1], b = px[2];

      /* convert to YCbCr (BT.601 weights) */
      const float y  = 0.299f * r + 0.587f * g + 0.114f * b;
      float cb = (b - y) * 0.564f + 0.5f;   /* normalize to [0,1] */
      float cr = (r - y) * 0.713f + 0.5f;

      /* scale to LUT grid coordinates */
      const float cb_grid = cb * (float)cb_range + (float)cb_s;
      const float cr_grid = cr * (float)cr_range + (float)cr_s;

      /* clamp to grid bounds */
      const float cb_clamped = fmaxf((float)cb_s, fminf((float)cb_e, cb_grid));
      const float cr_clamped = fmaxf((float)cr_s, fminf((float)cr_e, cr_grid));

      /* bilinear interpolation indices */
      const int cb0 = (int)floorf(cb_clamped);
      const int cr0 = (int)floorf(cr_clamped);
      const int cb1 = fminf(cb0 + 1, cb_e);
      const int cr1 = fminf(cr0 + 1, cr_e);

      const float fb = cb_clamped - (float)cb0;
      const float fr = cr_clamped - (float)cr0;

      /* LUT index: (cb - cb_s) * cr_range + (cr - cr_s), pairs of (dCb, dCr) */
      #define LUT_IDX(cbi, cri) (((cbi) - cb_s) * cr_range + ((cri) - cr_s)) * 2

      const int i00 = LUT_IDX(cb0, cr0);
      const int i10 = LUT_IDX(cb1, cr0);
      const int i01 = LUT_IDX(cb0, cr1);
      const int i11 = LUT_IDX(cb1, cr1);

      /* bilinear blend of dCb */
      const float dcb = (1.0f - fb) * (1.0f - fr) * chroma_lut[i00]
                       + fb * (1.0f - fr) * chroma_lut[i10]
                       + (1.0f - fb) * fr * chroma_lut[i01]
                       + fb * fr * chroma_lut[i11];

      /* bilinear blend of dCr */
      const float dcr = (1.0f - fb) * (1.0f - fr) * chroma_lut[i00 + 1]
                       + fb * (1.0f - fr) * chroma_lut[i10 + 1]
                       + (1.0f - fb) * fr * chroma_lut[i01 + 1]
                       + fb * fr * chroma_lut[i11 + 1];

      #undef LUT_IDX

      /* apply chroma correction, scaled by DivFactor */
      cb += dcb / div_factor;
      cr += dcr / div_factor;

      /* convert back from YCbCr to RGB */
      const float new_r = y + 1.402f * (cr - 0.5f);
      const float new_b = y + 1.772f * (cb - 0.5f);
      const float new_g = (y - 0.299f * new_r - 0.114f * new_b) / 0.587f;

      px[0] = px[0] * (1.0f - chroma_str) + fmaxf(0.0f, new_r) * chroma_str;
      px[1] = px[1] * (1.0f - chroma_str) + fmaxf(0.0f, new_g) * chroma_str;
      px[2] = px[2] * (1.0f - chroma_str) + fmaxf(0.0f, new_b) * chroma_str;
    }
  }

  /* --- stage 3: film curve --- */
  const float *const flut = d->film_lut;
  const float curve_str = d->curve_strength;

  if(d->film_curve != PHOCUS_FILM_LINEAR && curve_str > 0.0f)
  {
    const float scale = (float)PHOCUS_FILM_CURVE_SAMPLES;
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(out, npixels, flut, scale, curve_str) \
    schedule(static)
#endif
    for(size_t idx = 0; idx < npixels; idx++)
    {
      float *const px = out + idx * 4;
      for(int c = 0; c < 3; c++)
      {
        const float x = fmaxf(0.0f, fminf(1.0f, px[c]));
        const float fi = x * scale;
        const int i = (int)fi;
        const float f = fi - (float)i;
        const float curve_val = flut[i] * (1.0f - f) + flut[i + 1] * f;
        px[c] = px[c] * (1.0f - curve_str) + curve_val * curve_str;
      }
    }
  }

  /* --- stage 4: gamma 2.2 TRC --- */
  /* Hasselblad RGB uses gamma 2.2 as its TRC.
   * darktable works in linear space internally, so we DON'T apply gamma here.
   * The gamma is part of the output profile (colorout). Leaving this as a
   * comment for documentation - the CbCr LUT data already accounts for the
   * gamma in its correction values since it was extracted from the ICC profile
   * pipeline which includes the TRC.
   *
   * If needed, gamma can be enabled as an option in the GUI.
   */
}


/* ============================================================
 * Commit parameters -> precompute film curve LUT
 * ============================================================ */
void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_phocus_color_data_t *d = piece->data;
  const dt_iop_phocus_color_params_t *p = (dt_iop_phocus_color_params_t *)params;

  d->film_curve    = p->film_curve;
  d->illuminant    = p->illuminant;
  d->chroma_mode   = p->chroma_mode;
  d->matrix_strength = p->matrix_strength;
  d->chroma_strength = p->chroma_strength;
  d->curve_strength  = p->curve_strength;

  _generate_film_curve(p->film_curve, d->film_lut);
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
  return 1; /* no legacy versions yet */
}

void init(dt_iop_module_t *self)
{
  if(darktable.gui)
    dt_iop_request_focus(self);
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
  piece->data = calloc(1, sizeof(dt_iop_phocus_color_data_t));
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

  dt_bauhaus_combobox_set(g->film_curve, p->film_curve);
  dt_bauhaus_combobox_set(g->illuminant, p->illuminant);
  dt_bauhaus_combobox_set(g->chroma_mode, p->chroma_mode);
  dt_bauhaus_slider_set(g->matrix_strength, p->matrix_strength);
  dt_bauhaus_slider_set(g->chroma_strength, p->chroma_strength);
  dt_bauhaus_slider_set(g->curve_strength, p->curve_strength);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_phocus_color_gui_data_t *g = malloc(sizeof(dt_iop_phocus_color_gui_data_t));
  self->gui_data = g;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->film_curve = dt_bauhaus_combobox_from_params(self, "film_curve");
  gtk_widget_set_tooltip_text(g->film_curve,
    "Hasselblad film curve type. Standard is the default Phocus response curve.");

  g->illuminant = dt_bauhaus_combobox_from_params(self, "illuminant");
  gtk_widget_set_tooltip_text(g->illuminant,
    "Illuminant matrix: Flash (5600K) or Tungsten (3000K). Affects the3x3 color transform and chroma LUT.");

  g->chroma_mode = dt_bauhaus_combobox_from_params(self, "chroma_mode");
  gtk_widget_set_tooltip_text(g->chroma_mode,
    "Chroma LUT mode: Standard for general photography, Reproduction for copy/flat work.");

  g->matrix_strength = dt_bauhaus_slider_from_params(self, "matrix_strength");
  dt_bauhaus_slider_set_format(g->matrix_strength, "%");
  gtk_widget_set_tooltip_text(g->matrix_strength,
    "Strength of the Hasselblad 3x3 color matrix transform (0=off, 1=full).");

  g->chroma_strength = dt_bauhaus_slider_from_params(self, "chroma_strength");
  dt_bauhaus_slider_set_format(g->chroma_strength, "%");
  gtk_widget_set_tooltip_text(g->chroma_strength,
    "Strength of the CbCr chroma correction LUT (0=off, 1=full).");

  g->curve_strength = dt_bauhaus_slider_from_params(self, "curve_strength");
  dt_bauhaus_slider_set_format(g->curve_strength, "%");
  gtk_widget_set_tooltip_text(g->curve_strength,
    "Strength of the film curve tone mapping (0=off, 1=full).");
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
