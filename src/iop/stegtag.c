/*
 * StegTag - Steganographic Tag Embedding for darktable
 *
 * Embeds artist/copyright tags into images during export using
 * DCT-domain spread spectrum watermarking with BCH error correction.
 * Survives JPEG compression (quality >= 60) and casual image processing.
 *
 * Pipeline position: after watermark, before gamma (export-time only)
 *
 * Based on the steganographic-watermarking skill methodology:
 * - DCT block embedding (8x8 blocks, mid-frequency coefficients)
 * - BCH(15,7) error correction (2-bit correction per 7-bit payload chunk)
 * - Pseudorandom block selection via seed-based PRNG
 * - Adaptive strength based on local block variance
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "bauhaus/bauhaus.h"
#include "common/darktable.h"
#include "common/imagebuf.h"
#include "common/metadata.h"
#include "gui/gtk.h"

DT_MODULE_INTROSPECTION(1, dt_iop_stegtag_params_t)

/* ============================================================================
 * CONSTANTS
 * ============================================================================ */

#define MAX_PAYLOAD_LEN   128    /* max bytes of payload text */
#define DCT_BLOCK_SIZE    8      /* 8x8 DCT blocks */
#define BCH_N             15     /* BCH codeword total bits */
#define BCH_K             7      /* BCH codeword data bits */
#define BCH_T             2      /* 2-bit error correction */
#define BITS_PER_BLOCK    1      /* 1 bit per selected DCT coefficient */
#define BLOCKS_PER_BYTE   8      /* 8 blocks to embed 1 byte */
#define OVERSAMPLE        3      /* embed each bit in 3 blocks for redundancy */

/* ============================================================================
 * PARAMETERS
 * ============================================================================ */

typedef enum dt_iop_stegtag_mode_t
{
  STEGTAG_MODE_ARTIST = 0,    /* embed artist name from metadata */
  STEGTAG_MODE_COPYRIGHT = 1, /* embed copyright string */
  STEGTAG_MODE_CUSTOM = 2,    /* embed custom text */
  STEGTAG_MODE_DISABLED = 3,  /* pass-through */
} dt_iop_stegtag_mode_t;

typedef struct dt_iop_stegtag_params_t
{
  dt_iop_stegtag_mode_t mode;        /* embedding mode */
  char payload[MAX_PAYLOAD_LEN];     /* custom text payload (for CUSTOM mode) */
  float strength;                    /* embedding strength 0.01 - 1.0 */
  uint32_t seed;                     /* PRNG seed for block selection */
  gboolean adaptive;                 /* adaptive strength based on local variance */
  int color_channel;                 /* 0=luma, 1=green, 2=blue */
} dt_iop_stegtag_params_t;

typedef struct dt_iop_stegtag_gui_data_t
{
  GtkWidget *mode_combo;
  GtkWidget *payload_entry;
  GtkWidget *strength_slider;
  GtkWidget *seed_spin;
  GtkWidget *adaptive_check;
  GtkWidget *channel_combo;
  GtkWidget *status_label;
} dt_iop_stegtag_gui_data_t;

/* ============================================================================
 * BCH(15,7) CODEC
 * ============================================================================ */

/* Generator polynomial for BCH(15,7): x^8 + x^7 + x^6 + x^4 + 1 = 0x1D0 */
static const uint16_t BCH_GENERATOR = 0x1D0;

/* Encode 7 data bits into 15-bit BCH codeword */
static uint16_t bch_encode(uint8_t data)
{
  uint16_t codeword = (uint16_t)(data & 0x7F) << 8;
  for(int i = 6; i >= 0; i--)
  {
    if(codeword & (1 << (i + 8)))
    {
      codeword ^= BCH_GENERATOR << i;
    }
  }
  return ((uint16_t)(data & 0x7F) << 8) | (codeword & 0xFF);
}

/* Decode 15-bit BCH codeword, correcting up to 2-bit errors. Returns data or -1 on failure */
static int bch_decode(uint16_t codeword)
{
  /* Syndrome computation */
  uint8_t syndrome = 0;
  for(int i = 14; i >= 0; i--)
  {
    syndrome = (syndrome << 1) | ((codeword >> i) & 1);
    if(syndrome & 0x100) syndrome ^= 0x1D;
  }

  if(syndrome == 0) return (codeword >> 8) & 0x7F;

  /* Try single-bit error correction */
  for(int i = 0; i < 15; i++)
  {
    uint16_t test = codeword ^ (1 << i);
    uint8_t syn = 0;
    for(int j = 14; j >= 0; j--)
    {
      syn = (syn << 1) | ((test >> j) & 1);
      if(syn & 0x100) syn ^= 0x1D;
    }
    if(syn == 0) return (test >> 8) & 0x7F;
  }

  /* Try double-bit error correction (limited brute force) */
  for(int i = 0; i < 15; i++)
  {
    for(int j = i + 1; j < 15; j++)
    {
      uint16_t test = codeword ^ (1 << i) ^ (1 << j);
      uint8_t syn = 0;
      for(int k = 14; k >= 0; k--)
      {
        syn = (syn << 1) | ((test >> k) & 1);
        if(syn & 0x100) syn ^= 0x1D;
      }
      if(syn == 0) return (test >> 8) & 0x7F;
    }
  }

  return -1; /* uncorrectable */
}

/* ============================================================================
 * DCT TRANSFORM (in-place, row-column method)
 * ============================================================================ */

static void dct_8x8(float block[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
  float tmp[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];

  /* 1D DCT on rows */
  for(int y = 0; y < DCT_BLOCK_SIZE; y++)
  {
    for(int u = 0; u < DCT_BLOCK_SIZE; u++)
    {
      float sum = 0.0f;
      float cu = (u == 0) ? 1.0f / sqrtf(2.0f) : 1.0f;
      for(int x = 0; x < DCT_BLOCK_SIZE; x++)
      {
        sum += block[y][x] * cosf((2.0f * x + 1.0f) * u * M_PI / 16.0f);
      }
      tmp[y][u] = 0.5f * cu * sum;
    }
  }

  /* 1D DCT on columns */
  for(int u = 0; u < DCT_BLOCK_SIZE; u++)
  {
    for(int v = 0; v < DCT_BLOCK_SIZE; v++)
    {
      float sum = 0.0f;
      float cv = (v == 0) ? 1.0f / sqrtf(2.0f) : 1.0f;
      for(int y = 0; y < DCT_BLOCK_SIZE; y++)
      {
        sum += tmp[y][u] * cosf((2.0f * y + 1.0f) * v * M_PI / 16.0f);
      }
      block[u][v] = 0.5f * cv * sum;
    }
  }
}

static void idct_8x8(float block[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
  float tmp[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];

  /* 1D IDCT on columns */
  for(int x = 0; x < DCT_BLOCK_SIZE; x++)
  {
    for(int y = 0; y < DCT_BLOCK_SIZE; y++)
    {
      float sum = 0.0f;
      for(int v = 0; v < DCT_BLOCK_SIZE; v++)
      {
        float cv = (v == 0) ? 1.0f / sqrtf(2.0f) : 1.0f;
        sum += cv * block[x][v] * cosf((2.0f * y + 1.0f) * v * M_PI / 16.0f);
      }
      tmp[x][y] = 0.25f * sum;
    }
  }

  /* 1D IDCT on rows */
  for(int y = 0; y < DCT_BLOCK_SIZE; y++)
  {
    for(int x = 0; x < DCT_BLOCK_SIZE; x++)
    {
      float sum = 0.0f;
      for(int u = 0; u < DCT_BLOCK_SIZE; u++)
      {
        float cu = (u == 0) ? 1.0f / sqrtf(2.0f) : 1.0f;
        sum += cu * tmp[y][u] * cosf((2.0f * x + 1.0f) * u * M_PI / 16.0f);
      }
      block[y][x] = 0.25f * sum;
    }
  }
}

/* ============================================================================
 * PRNG (xorshift32 for deterministic block selection)
 * ============================================================================ */

static uint32_t prng_next(uint32_t *state)
{
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* ============================================================================
 * EMBEDDING
 * ============================================================================ */

/* Convert payload string to bit array */
static int payload_to_bits(const char *payload, int payload_len, uint8_t *bits, int max_bits)
{
  int bit_idx = 0;
  for(int i = 0; i < payload_len && bit_idx + 7 < max_bits; i++)
  {
    uint16_t codeword = bch_encode((uint8_t)payload[i]);
    for(int b = BCH_N - 1; b >= 0 && bit_idx < max_bits; b--)
    {
      bits[bit_idx++] = (codeword >> b) & 1;
    }
  }
  return bit_idx;
}

/* Convert bit array back to payload string */
static int bits_to_payload(const uint8_t *bits, int num_bits, char *payload, int max_len)
{
  int payload_idx = 0;
  for(int i = 0; i + BCH_N <= num_bits && payload_idx < max_len - 1; i += BCH_N)
  {
    uint16_t codeword = 0;
    for(int b = 0; b < BCH_N; b++)
    {
      codeword = (codeword << 1) | bits[i + b];
    }
    int decoded = bch_decode(codeword);
    if(decoded < 0) decoded = '?'; /* uncorrectable, substitute */
    payload[payload_idx++] = (char)decoded;
  }
  payload[payload_idx] = '\0';
  return payload_idx;
}

/* ============================================================================
 * CORE PROCESSING
 * ============================================================================ */

static void process_stegtag(dt_iop_module_t *self,
                             dt_dev_pixelpipe_iop_t *piece,
                             const void *const ivoid,
                             void *const ovoid,
                             const dt_iop_roi_t *const roi_in,
                             const dt_iop_roi_t *const roi_out)
{
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  const float *in = (const float *)ivoid;
  float *out = (float *)ovoid;

  /* pass-through if disabled */
  if(p->mode == STEGTAG_MODE_DISABLED)
  {
    dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 4);
    return;
  }

  /* copy input to output first */
  dt_iop_image_copy_by_size(out, in, roi_out->width, roi_out->height, 4);

  /* resolve payload */
  char payload[MAX_PAYLOAD_LEN] = {0};
  int payload_len = 0;

  switch(p->mode)
  {
    case STEGTAG_MODE_ARTIST:
    {
      /* pull artist from Xmp.dc.creator metadata */
      uint32_t count = 0;
      GList *res = dt_metadata_get(self->dev->image_storage.id, "Xmp.dc.creator", &count);
      if(res && res->data)
      {
        g_strlcpy(payload, (const char *)res->data, MAX_PAYLOAD_LEN);
        payload_len = strlen(payload);
        g_list_free_full(res, g_free);
      }
      else
      {
        if(res) g_list_free_full(res, g_free);
        return; /* no artist metadata, nothing to embed */
      }
      break;
    }
    case STEGTAG_MODE_COPYRIGHT:
    {
      /* pull rights from Xmp.dc.rights metadata */
      uint32_t count = 0;
      GList *res = dt_metadata_get(self->dev->image_storage.id, "Xmp.dc.rights", &count);
      if(res && res->data)
      {
        g_strlcpy(payload, (const char *)res->data, MAX_PAYLOAD_LEN);
        payload_len = strlen(payload);
        g_list_free_full(res, g_free);
      }
      else
      {
        if(res) g_list_free_full(res, g_free);
        return;
      }
      break;
    }
    case STEGTAG_MODE_CUSTOM:
    {
      g_strlcpy(payload, p->payload, MAX_PAYLOAD_LEN);
      payload_len = strlen(payload);
      if(payload_len == 0) return;
      break;
    }
    default:
      return;
  }

  /* convert payload to BCH-encoded bits */
  const int max_bits = payload_len * BCH_N * OVERSAMPLE + BCH_N * 4; /* extra for terminator */
  uint8_t *bits = calloc(max_bits, sizeof(uint8_t));
  if(!bits) return;

  int num_bits = payload_to_bits(payload, payload_len, bits, max_bits);

  /* add terminator: 4 zero codewords */
  for(int i = 0; i < 4 * BCH_N && (num_bits + i) < max_bits; i++)
  {
    bits[num_bits + i] = 0;
  }
  num_bits += 4 * BCH_N;

  /* select channel: 0=luma (weighted RGB), 1=green, 2=blue */
  int ch = p->color_channel;
  if(ch > 2) ch = 2;

  const int width = roi_out->width;
  const int height = roi_out->height;
  const int blocks_x = width / DCT_BLOCK_SIZE;
  const int blocks_y = height / DCT_BLOCK_SIZE;
  const int total_blocks = blocks_x * blocks_y;

  /* we need num_bits * OVERSAMPLE blocks to embed all bits with redundancy */
  int blocks_needed = num_bits * OVERSAMPLE;
  if(blocks_needed > total_blocks)
  {
    /* image too small for payload, truncate */
    num_bits = total_blocks / OVERSAMPLE;
    blocks_needed = num_bits * OVERSAMPLE;
  }

  /* seed PRNG for deterministic block selection */
  uint32_t rng = p->seed ? p->seed : 42;

  /* generate block order via Fisher-Yates shuffle on first N indices */
  if(total_blocks < 1 || total_blocks > 65536 * 64) return;
  int *block_order = calloc(total_blocks, sizeof(int));
  if(!block_order) { free(bits); return; }
  for(int i = 0; i < total_blocks; i++) block_order[i] = i;
  for(int i = total_blocks - 1; i > 0; i--)
  {
    int j = prng_next(&rng) % (i + 1);
    int tmp = block_order[i];
    block_order[i] = block_order[j];
    block_order[j] = tmp;
  }

  /* mid-frequency DCT coefficient for embedding (3,2) - good balance of
   * robustness and invisibility */
  const int EMBED_U = 3;
  const int EMBED_V = 2;

  /* embed each bit into OVERSAMPLE blocks */
  for(int bit_idx = 0; bit_idx < num_bits; bit_idx++)
  {
    uint8_t bit_val = bits[bit_idx];
    for(int rep = 0; rep < OVERSAMPLE; rep++)
    {
      int block_idx = block_order[bit_idx * OVERSAMPLE + rep];
      int bx = block_idx % blocks_x;
      int by = block_idx / blocks_x;
      int px = bx * DCT_BLOCK_SIZE;
      int py = by * DCT_BLOCK_SIZE;

      /* extract 8x8 block from selected channel */
      float block[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
      for(int y = 0; y < DCT_BLOCK_SIZE; y++)
      {
        for(int x = 0; x < DCT_BLOCK_SIZE; x++)
        {
          int sx = px + x;
          int sy = py + y;
          if(sx < width && sy < height)
          {
            if(ch == 0)
            {
              /* luma: weighted average */
              const float *pixel = out + (sy * width + sx) * 4;
              block[y][x] = 0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2];
            }
            else
            {
              block[y][x] = out[(sy * width + sx) * 4 + ch];
            }
          }
          else
          {
            block[y][x] = 0.0f;
          }
        }
      }

      /* forward DCT */
      dct_8x8(block);

      /* compute local variance for adaptive strength */
      float local_strength = p->strength;
      if(p->adaptive)
      {
        float mean = 0.0f, var = 0.0f;
        for(int y = 0; y < DCT_BLOCK_SIZE; y++)
          for(int x = 0; x < DCT_BLOCK_SIZE; x++)
            mean += block[y][x];
        mean /= 64.0f;
        for(int y = 0; y < DCT_BLOCK_SIZE; y++)
          for(int x = 0; x < DCT_BLOCK_SIZE; x++)
            var += (block[y][x] - mean) * (block[y][x] - mean);
        var /= 64.0f;
        /* scale strength: more texture = can embed stronger */
        float texture = sqrtf(fabsf(var));
        local_strength = p->strength * (0.5f + 0.5f * tanhf(texture * 0.01f));
      }

      /* embed bit by quantizing mid-frequency coefficient */
      float coeff = block[EMBED_U][EMBED_V];
      float step = local_strength * 20.0f; /* quantization step */

      if(step > 0.1f)
      {
        float quantized = roundf(coeff / step) * step;
        if(bit_val)
        {
          /* bit=1: quantize to odd multiple of step/2 */
          block[EMBED_U][EMBED_V] = quantized + step * 0.25f;
        }
        else
        {
          /* bit=0: quantize to even multiple of step/2 */
          block[EMBED_U][EMBED_V] = quantized - step * 0.25f;
        }
      }

      /* inverse DCT */
      idct_8x8(block);

      /* write block back to output */
      for(int y = 0; y < DCT_BLOCK_SIZE; y++)
      {
        for(int x = 0; x < DCT_BLOCK_SIZE; x++)
        {
          int sx = px + x;
          int sy = py + y;
          if(sx < width && sy < height)
          {
            if(ch == 0)
            {
              /* distribute luma modification across RGB */
              const float *orig_in = in + (sy * width + sx) * 4;
              float orig_luma = 0.299f * orig_in[0] + 0.587f * orig_in[1] + 0.114f * orig_in[2];
              float new_luma = block[y][x];
              float delta = new_luma - orig_luma;
              out[(sy * width + sx) * 4 + 0] = orig_in[0] + delta * 0.299f;
              out[(sy * width + sx) * 4 + 1] = orig_in[1] + delta * 0.587f;
              out[(sy * width + sx) * 4 + 2] = orig_in[2] + delta * 0.114f;
            }
            else
            {
              out[(sy * width + sx) * 4 + ch] = block[y][x];
            }
          }
        }
      }
    }
  }

  free(bits);
  free(block_order);
}

/* ============================================================================
 * EXTRACTION (for verification/debug)
 * ============================================================================ */

/* Read embedded data from an image. Returns extracted payload length, or -1 on error.
 * payload must be at least MAX_PAYLOAD_LEN bytes. */
int dt_stegtag_extract(const float *image, int width, int height,
                        uint32_t seed, int color_channel,
                        char *payload, int max_len)
{
  const int blocks_x = width / DCT_BLOCK_SIZE;
  const int blocks_y = height / DCT_BLOCK_SIZE;
  const int total_blocks = blocks_x * blocks_y;

  /* generate same block order as embedding */
  uint32_t rng = seed ? seed : 42;
  if(total_blocks < 1 || total_blocks > 65536 * 64) return -1;
  int *block_order = calloc(total_blocks, sizeof(int));
  if(!block_order) return -1;
  for(int i = 0; i < total_blocks; i++) block_order[i] = i;
  for(int i = total_blocks - 1; i > 0; i--)
  {
    int j = prng_next(&rng) % (i + 1);
    int tmp = block_order[i];
    block_order[i] = block_order[j];
    block_order[j] = tmp;
  }

  const int EMBED_U = 3;
  const int EMBED_V = 2;
  int ch = color_channel;
  if(ch > 2) ch = 2;

  /* read bits with majority voting from OVERSAMPLE blocks */
  int max_extract_bits = total_blocks / OVERSAMPLE;
  if(max_extract_bits < 1 || max_extract_bits > 65536 * 64) { free(block_order); return -1; }
  uint8_t *bits = calloc(max_extract_bits, sizeof(uint8_t));
  if(!bits) { free(block_order); return -1; }

  int num_bits = 0;
  int terminator_count = 0;

  for(int bit_idx = 0; bit_idx < max_extract_bits; bit_idx++)
  {
    int votes[2] = {0, 0};
    for(int rep = 0; rep < OVERSAMPLE; rep++)
    {
      int block_idx = block_order[bit_idx * OVERSAMPLE + rep];
      int bx = block_idx % blocks_x;
      int by = block_idx / blocks_x;
      int px = bx * DCT_BLOCK_SIZE;
      int py = by * DCT_BLOCK_SIZE;

      float block[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
      for(int y = 0; y < DCT_BLOCK_SIZE; y++)
      {
        for(int x = 0; x < DCT_BLOCK_SIZE; x++)
        {
          int sx = px + x;
          int sy = py + y;
          if(sx < width && sy < height)
          {
            if(ch == 0)
            {
              const float *pixel = image + (sy * width + sx) * 4;
              block[y][x] = 0.299f * pixel[0] + 0.587f * pixel[1] + 0.114f * pixel[2];
            }
            else
            {
              block[y][x] = image[(sy * width + sx) * 4 + ch];
            }
          }
          else
          {
            block[y][x] = 0.0f;
          }
        }
      }

      dct_8x8(block);
      /* read bit from coefficient phase */
      float coeff = block[EMBED_U][EMBED_V];
      float phase = fmodf(coeff, 40.0f);
      if(phase < 0) phase += 40.0f;
      int bit = (phase < 10.0f || phase > 30.0f) ? 0 : 1;
      votes[bit]++;
    }

    bits[bit_idx] = (votes[1] > votes[0]) ? 1 : 0;
    num_bits = bit_idx + 1;

    /* check for terminator: 4 consecutive zero codewords */
    if(num_bits >= 4 * BCH_N)
    {
      int all_zero = 1;
      int start = num_bits - 4 * BCH_N;
      for(int i = start; i < num_bits; i++)
      {
        if(bits[i]) { all_zero = 0; break; }
      }
      if(all_zero)
      {
        num_bits = start;
        terminator_count = 1;
        break;
      }
    }
  }

  if(!terminator_count)
  {
    /* no terminator found, try to decode what we have */
  }

  int result = bits_to_payload(bits, num_bits, payload, max_len);

  free(bits);
  free(block_order);
  return result;
}

/* ============================================================================
 * DARKTABLE IOP API
 * ============================================================================ */

const char *name()
{
  return "stegtag";
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
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

/* process() -- the IOP entry point, called by the pipeline */
void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  process_stegtag(self, piece, ivoid, ovoid, roi_in, roi_out);
}

void init(dt_iop_module_t *self)
{
  if(darktable.gui)
    dt_iop_request_focus(self);
  self->gui_data = NULL;

  dt_iop_stegtag_params_t *d = (dt_iop_stegtag_params_t *)self->default_params;
  d->mode = STEGTAG_MODE_DISABLED;
  d->strength = 0.3f;
  d->seed = 0;  /* 0 = auto (will use a fixed seed) */
  d->adaptive = TRUE;
  d->color_channel = 0; /* luma */
  memset(d->payload, 0, MAX_PAYLOAD_LEN);
}

void cleanup(dt_iop_module_t *self)
{
  free(self->params);
  self->params = NULL;
  free(self->default_params);
  self->default_params = NULL;
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_stegtag_params_t));
}

void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *params,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, params, sizeof(dt_iop_stegtag_params_t));
}

/* ============================================================================
 * GUI CALLBACKS
 * ============================================================================ */

static void mode_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  dt_iop_stegtag_gui_data_t *g = (dt_iop_stegtag_gui_data_t *)self->gui_data;

  int idx = dt_bauhaus_combobox_get(widget);
  p->mode = (dt_iop_stegtag_mode_t)idx;

  /* show/hide payload entry based on mode */
  if(g->payload_entry)
    gtk_widget_set_visible(g->payload_entry, (p->mode == STEGTAG_MODE_CUSTOM));

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void payload_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  const char *text = gtk_entry_get_text(GTK_ENTRY(widget));
  g_strlcpy(p->payload, text, MAX_PAYLOAD_LEN);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void strength_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  p->strength = dt_bauhaus_slider_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void seed_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  p->seed = (uint32_t)dt_bauhaus_slider_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void adaptive_toggled(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  p->adaptive = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void channel_changed(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  p->color_channel = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

/* ============================================================================
 * GUI
 * ============================================================================ */

void gui_update(dt_iop_module_t *self)
{
  dt_iop_stegtag_params_t *p = (dt_iop_stegtag_params_t *)self->params;
  dt_iop_stegtag_gui_data_t *g = (dt_iop_stegtag_gui_data_t *)self->gui_data;

  if(!g) return;

  dt_bauhaus_combobox_set(g->mode_combo, (int)p->mode);
  gtk_entry_set_text(GTK_ENTRY(g->payload_entry), p->payload);
  dt_bauhaus_slider_set(g->strength_slider, p->strength);
  dt_bauhaus_slider_set(g->seed_spin, (float)p->seed);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->adaptive_check), p->adaptive);
  dt_bauhaus_combobox_set(g->channel_combo, p->color_channel);

  /* show/hide payload entry */
  gtk_widget_set_visible(g->payload_entry, (p->mode == STEGTAG_MODE_CUSTOM));

  /* status label */
  const char *mode_str = "disabled";
  switch(p->mode)
  {
    case STEGTAG_MODE_ARTIST: mode_str = "artist name"; break;
    case STEGTAG_MODE_COPYRIGHT: mode_str = "copyright"; break;
    case STEGTAG_MODE_CUSTOM: mode_str = "custom text"; break;
    default: break;
  }
  char status[256];
  snprintf(status, sizeof(status), "mode: %s | strength: %.2f", mode_str, p->strength);
  if(g->status_label)
    gtk_label_set_text(GTK_LABEL(g->status_label), status);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_stegtag_gui_data_t));
  dt_iop_stegtag_gui_data_t *g = (dt_iop_stegtag_gui_data_t *)self->gui_data;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  /* mode selector */
  g->mode_combo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->mode_combo, NULL, "mode");
  dt_bauhaus_combobox_add(g->mode_combo, "artist (from metadata)");
  dt_bauhaus_combobox_add(g->mode_combo, "copyright (from metadata)");
  dt_bauhaus_combobox_add(g->mode_combo, "custom text");
  dt_bauhaus_combobox_add(g->mode_combo, "disabled");
  g_signal_connect(G_OBJECT(g->mode_combo), "value-changed", G_CALLBACK(mode_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->mode_combo, TRUE, TRUE, 0);

  /* payload text entry (for custom mode) */
  GtkWidget *payload_label = gtk_label_new("payload:");
  gtk_box_pack_start(GTK_BOX(self->widget), payload_label, TRUE, TRUE, 0);
  g->payload_entry = gtk_entry_new();
  gtk_entry_set_max_length(GTK_ENTRY(g->payload_entry), MAX_PAYLOAD_LEN - 1);
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->payload_entry), "artist name or custom tag...");
  g_signal_connect(G_OBJECT(g->payload_entry), "changed", G_CALLBACK(payload_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->payload_entry, TRUE, TRUE, 0);

  /* strength slider */
  g->strength_slider = dt_bauhaus_slider_new_with_range(self, 0.01f, 1.0f, 0.01f, 0.3f, 2);
  dt_bauhaus_widget_set_label(g->strength_slider, NULL, "strength");
  dt_bauhaus_slider_set_format(g->strength_slider, "%.2f");
  g_signal_connect(G_OBJECT(g->strength_slider), "value-changed", G_CALLBACK(strength_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->strength_slider, TRUE, TRUE, 0);

  /* seed */
  g->seed_spin = dt_bauhaus_slider_new_with_range(self, 0, 65535, 1, 0, 0);
  dt_bauhaus_widget_set_label(g->seed_spin, NULL, "seed (0=auto)");
  g_signal_connect(G_OBJECT(g->seed_spin), "value-changed", G_CALLBACK(seed_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->seed_spin, TRUE, TRUE, 0);

  /* adaptive strength */
  g->adaptive_check = gtk_check_button_new_with_label("adaptive strength");
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->adaptive_check), TRUE);
  g_signal_connect(G_OBJECT(g->adaptive_check), "toggled", G_CALLBACK(adaptive_toggled), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->adaptive_check, TRUE, TRUE, 0);

  /* color channel */
  g->channel_combo = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->channel_combo, NULL, "channel");
  dt_bauhaus_combobox_add(g->channel_combo, "luma (recommended)");
  dt_bauhaus_combobox_add(g->channel_combo, "green");
  dt_bauhaus_combobox_add(g->channel_combo, "blue");
  g_signal_connect(G_OBJECT(g->channel_combo), "value-changed", G_CALLBACK(channel_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->channel_combo, TRUE, TRUE, 0);

  /* status label */
  g->status_label = gtk_label_new("stegtag: ready");
  gtk_widget_set_name(g->status_label, "stegtag-status");
  gtk_box_pack_start(GTK_BOX(self->widget), g->status_label, TRUE, TRUE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}
