/*
 * reverse_synthid.c
 *
 * C port of aloshdenny/reverse-SynthID
 * https://github.com/aloshdenny/reverse-SynthID
 *
 * Implements:
 *   - SynthID watermark DETECTION via spread-spectrum phase correlation
 *   - SynthID watermark BYPASS (removal) via frequency-domain subtraction
 *   - Codebook EXTRACTION from a directory of reference images
 *
 * Multi-resolution: one .bin file can hold profiles for multiple image sizes
 * (e.g. 256, 512, 1024).  Detection auto-selects the matching profile.
 *
 * Dependencies (single-file, header-only):
 *   stb_image.h / stb_image_write.h  -- PNG/JPEG I/O
 *
 * Build:
 *   gcc -O2 -o reverse_synthid reverse_synthid.c -lm
 *
 * Usage:
 *   reverse_synthid detect  <image.png> <codebook.bin>
 *   reverse_synthid bypass  <input.png> <output.png> <codebook.bin> [gentle|moderate|aggressive|maximum]
 *   reverse_synthid extract <image_dir/> <codebook.bin>
 */

#define _CRT_SECURE_NO_WARNINGS
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _MSC_VER
#  ifndef strcasecmp
#    define strcasecmp _stricmp
#  endif
#else
#  include <strings.h>
#endif
#include <math.h>
#include <stdint.h>
#include <limits.h>

#ifdef _MSC_VER
#include <windows.h>
struct dirent {
    char d_name[MAX_PATH];
};

typedef struct DIR {
    HANDLE hFind;
    WIN32_FIND_DATAA findData;
    int first;
    struct dirent ent;
} DIR;

static DIR *opendir(const char *dir_path) {
    char search_path[MAX_PATH];
    sprintf_s(search_path, MAX_PATH, "%s\\*", dir_path);
    DIR *d = malloc(sizeof(DIR));
    if (!d) return NULL;
    d->hFind = FindFirstFileA(search_path, &d->findData);
    if (d->hFind == INVALID_HANDLE_VALUE) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

static struct dirent *readdir(DIR *d) {
    if (d->first) {
        d->first = 0;
    } else {
        if (!FindNextFileA(d->hFind, &d->findData)) {
            return NULL;
        }
    }
    strcpy_s(d->ent.d_name, MAX_PATH, d->findData.cFileName);
    return &d->ent;
}

static int closedir(DIR *d) {
    if (d) {
        FindClose(d->hFind);
        free(d);
    }
    return 0;
}
#else
#  include <dirent.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Intentionally-ignored fread (optional fields at end of file) */
#define fread_optional(ptr,sz,n,f) do { if(fread(ptr,sz,n,f)){} } while(0)

/* -----------------------------------------------------------------------
 * Tiny 2-D FFT  (Cooley-Tukey radix-2, power-of-two dimensions)
 * ----------------------------------------------------------------------- */
typedef struct { float re, im; } cf32;

static void fft1d(cf32 *x, int n, int inverse)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { cf32 t = x[i]; x[i] = x[j]; x[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = 2.0 * M_PI / len * (inverse ? 1 : -1);
        cf32 wlen = { (float)cos(ang), (float)sin(ang) };
        for (int i = 0; i < n; i += len) {
            cf32 w = { 1.0f, 0.0f };
            for (int j = 0; j < len/2; j++) {
                cf32 u = x[i+j];
                cf32 v = { x[i+j+len/2].re*w.re - x[i+j+len/2].im*w.im,
                           x[i+j+len/2].re*w.im + x[i+j+len/2].im*w.re };
                x[i+j]       = (cf32){ u.re+v.re, u.im+v.im };
                x[i+j+len/2] = (cf32){ u.re-v.re, u.im-v.im };
                cf32 nw = { w.re*wlen.re - w.im*wlen.im,
                            w.re*wlen.im + w.im*wlen.re };
                w = nw;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; i++) { x[i].re /= n; x[i].im /= n; }
}

static void fft2d(cf32 *data, int H, int W, int inverse)
{
    cf32 *tmp = malloc((H > W ? H : W) * sizeof(cf32));
    for (int r = 0; r < H; r++) {
        memcpy(tmp, data + r*W, W * sizeof(cf32));
        fft1d(tmp, W, inverse);
        memcpy(data + r*W, tmp, W * sizeof(cf32));
    }
    for (int c = 0; c < W; c++) {
        for (int r = 0; r < H; r++) tmp[r] = data[r*W + c];
        fft1d(tmp, H, inverse);
        for (int r = 0; r < H; r++) data[r*W + c] = tmp[r];
    }
    free(tmp);
}

static void fftshift2d(cf32 *data, int H, int W)
{
    int hh = H/2, hw = W/2;
    cf32 *tmp = malloc(hh * hw * sizeof(cf32));  /* quarter-plane buffer */
    /* swap Q1<->Q3 and Q2<->Q4 */
    for (int r = 0; r < hh; r++) {
        for (int c = 0; c < hw; c++) {
            cf32 a = data[ r     *W + c   ];
            cf32 b = data[ r     *W + c+hw];
            cf32 c3= data[(r+hh)*W + c   ];
            cf32 d = data[(r+hh)*W + c+hw];
            data[ r     *W + c   ] = d;
            data[ r     *W + c+hw] = c3;
            data[(r+hh)*W + c   ] = b;
            data[(r+hh)*W + c+hw] = a;
            (void)tmp;
        }
    }
    free(tmp);
}

/* -----------------------------------------------------------------------
 * stb_image
 * ----------------------------------------------------------------------- */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#if __has_include("stb_image.h")
#  include "stb_image.h"
#  include "stb_image_write.h"
#  define HAVE_STB 1
#else
#  warning "stb_image.h not found – image loading stubs active. Download from https://github.com/nothings/stb"
#  define HAVE_STB 0
   static unsigned char *stbi_load(const char *f,int *x,int *y,int *c,int r){
       (void)f;(void)x;(void)y;(void)c;(void)r;
       fprintf(stderr,"ERROR: stb_image not available.\n"); return NULL; }
   static void stbi_image_free(void *p){free(p);}
   static int stbi_write_png(const char*f,int w,int h,int c,const void*d,int s){
       (void)f;(void)w;(void)h;(void)c;(void)d;(void)s; return 0; }
#endif

static int next_pow2(int n){ int p=1; while(p<n) p<<=1; return p; }

/* -----------------------------------------------------------------------
 * Image
 * ----------------------------------------------------------------------- */
typedef struct { int W, H; float *ch[3]; } Image;

static Image *image_load(const char *path)
{
    int w, h, c;
    unsigned char *raw = stbi_load(path, &w, &h, &c, 3);
    if (!raw) { fprintf(stderr, "Cannot load %s\n", path); return NULL; }
    Image *img = calloc(1, sizeof(Image));
    img->W = w; img->H = h;
    for (int ch = 0; ch < 3; ch++) {
        img->ch[ch] = malloc(w * h * sizeof(float));
        for (int i = 0; i < w*h; i++)
            img->ch[ch][i] = (float)raw[i*3+ch];
    }
    stbi_image_free(raw);
    return img;
}

static void image_save(const char *path, const Image *img)
{
    unsigned char *raw = malloc(img->W * img->H * 3);
    for (int i = 0; i < img->W * img->H; i++)
        for (int ch = 0; ch < 3; ch++) {
            float v = img->ch[ch][i];
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            raw[i*3+ch] = (unsigned char)(v + 0.5f);
        }
    stbi_write_png(path, img->W, img->H, 3, raw, img->W*3);
    free(raw);
}

static void image_free(Image *img)
{
    if (!img) return;
    for (int ch = 0; ch < 3; ch++) free(img->ch[ch]);
    free(img);
}

/* Nearest-neighbour resize (cheap, good enough for watermark detection) */
static Image *image_resize(const Image *src, int newW, int newH)
{
    Image *dst = calloc(1, sizeof(Image));
    dst->W = newW; dst->H = newH;
    for (int ch = 0; ch < 3; ch++) {
        dst->ch[ch] = malloc(newW * newH * sizeof(float));
        for (int r = 0; r < newH; r++) {
            int sr = r * src->H / newH;
            for (int c = 0; c < newW; c++) {
                int sc = c * src->W / newW;
                dst->ch[ch][r*newW+c] = src->ch[ch][sr*src->W+sc];
            }
        }
    }
    return dst;
}

/* -----------------------------------------------------------------------
 * Box-blur denoiser: noise = image - blur(image)
 * ----------------------------------------------------------------------- */
static float *make_noise(const float *ch, int W, int H, int radius)
{
    int diam = 2*radius + 1;
    float *tmp      = malloc(W * H * sizeof(float));
    float *blurred  = malloc(W * H * sizeof(float));
    float *noise    = malloc(W * H * sizeof(float));

    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            float s = 0;
            for (int d = -radius; d <= radius; d++) {
                int cc = c+d; if(cc<0)cc=0; if(cc>=W)cc=W-1;
                s += ch[r*W+cc];
            }
            tmp[r*W+c] = s / diam;
        }
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            float s = 0;
            for (int d = -radius; d <= radius; d++) {
                int rr = r+d; if(rr<0)rr=0; if(rr>=H)rr=H-1;
                s += tmp[rr*W+c];
            }
            blurred[r*W+c] = s / diam;
        }
    free(tmp);
    for (int i = 0; i < W*H; i++) noise[i] = ch[i] - blurred[i];
    free(blurred);
    return noise;
}

#define NOISE_RADIUS 3

static cf32 *channel_fft(const float *noise, int W, int H)
{
    cf32 *buf = malloc(W * H * sizeof(cf32));
    for (int i = 0; i < W*H; i++) { buf[i].re = noise[i]; buf[i].im = 0; }
    fft2d(buf, H, W, 0);
    fftshift2d(buf, H, W);
    return buf;
}

/* -----------------------------------------------------------------------
 * Codebook — multi-resolution
 *
 * File format (binary, little-endian):
 *   4 bytes  : magic  "SCBM"   (M = multi-res; "SCDB" single-res still loaded)
 *   4 bytes  : int32  n_profiles
 *   For each profile:
 *     4 bytes  : int32  W
 *     4 bytes  : int32  H
 *     W*H*8*3  : cf32[W*H] x3 channels  (FFT, shifted)
 *     4 bytes  : int32  n_carriers
 *     n*4      : int32[] carrier_r
 *     n*4      : int32[] carrier_c
 *     n*4 x3   : float[] ref_phase per channel
 *     n*4 x3   : float[] ref_mag   per channel
 *     4 bytes  : float  threshold_correlation
 *     4 bytes  : float  threshold_phase
 *     4 bytes  : float  corr_mean
 *     4 bytes  : float  corr_std
 * ----------------------------------------------------------------------- */
#define CODEBOOK_MAGIC_MULTI  "SCBM"
#define CODEBOOK_MAGIC_SINGLE "SCDB"
#define MAX_CARRIERS   512
#define MAX_PROFILES   16

typedef struct {
    int   W, H;
    cf32 *fft[3];
    int   n_carriers;
    int   carrier_r[MAX_CARRIERS];
    int   carrier_c[MAX_CARRIERS];
    float ref_phase[3][MAX_CARRIERS];
    float ref_mag[3][MAX_CARRIERS];
    float threshold_correlation;
    float threshold_phase;
    float corr_mean;
    float corr_std;
} Profile;

typedef struct {
    int      n_profiles;
    Profile *profiles[MAX_PROFILES];
} Codebook;

static Profile *profile_alloc(int W, int H)
{
    Profile *p = calloc(1, sizeof(Profile));
    p->W = W; p->H = H;
    for (int ch = 0; ch < 3; ch++)
        p->fft[ch] = calloc(W * H, sizeof(cf32));
    p->threshold_correlation = 0.010f;
    p->threshold_phase       = 0.45f;
    return p;
}

static void profile_free(Profile *p)
{
    if (!p) return;
    for (int ch = 0; ch < 3; ch++) free(p->fft[ch]);
    free(p);
}

static void codebook_free(Codebook *cb)
{
    if (!cb) return;
    for (int i = 0; i < cb->n_profiles; i++) profile_free(cb->profiles[i]);
    free(cb);
}

/* Find the profile whose size exactly matches W×H, or NULL */
static Profile *codebook_find_profile(const Codebook *cb, int W, int H)
{
    for (int i = 0; i < cb->n_profiles; i++)
        if (cb->profiles[i]->W == W && cb->profiles[i]->H == H)
            return cb->profiles[i];
    return NULL;
}

static int profile_write(FILE *f, const Profile *p)
{
    fwrite(&p->W, 4, 1, f);
    fwrite(&p->H, 4, 1, f);
    for (int ch = 0; ch < 3; ch++)
        fwrite(p->fft[ch], sizeof(cf32), (size_t)(p->W * p->H), f);
    fwrite(&p->n_carriers, 4, 1, f);
    fwrite(p->carrier_r, 4, (size_t)p->n_carriers, f);
    fwrite(p->carrier_c, 4, (size_t)p->n_carriers, f);
    for (int ch = 0; ch < 3; ch++)
        fwrite(p->ref_phase[ch], 4, (size_t)p->n_carriers, f);
    for (int ch = 0; ch < 3; ch++)
        fwrite(p->ref_mag[ch],   4, (size_t)p->n_carriers, f);
    fwrite(&p->threshold_correlation, 4, 1, f);
    fwrite(&p->threshold_phase,       4, 1, f);
    fwrite(&p->corr_mean,             4, 1, f);
    fwrite(&p->corr_std,              4, 1, f);
    return 0;
}

static Profile *profile_read(FILE *f)
{
    int W, H;
    if (fread(&W, 4, 1, f) != 1 || fread(&H, 4, 1, f) != 1) return NULL;
    Profile *p = profile_alloc(W, H);
    for (int ch = 0; ch < 3; ch++)
        if (fread(p->fft[ch], sizeof(cf32), (size_t)(W*H), f) != (size_t)(W*H))
            { profile_free(p); return NULL; }
    if (fread(&p->n_carriers, 4, 1, f) != 1) { profile_free(p); return NULL; }
    if (fread(p->carrier_r, 4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers ||
        fread(p->carrier_c, 4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers)
        { profile_free(p); return NULL; }
    for (int ch = 0; ch < 3; ch++)
        if (fread(p->ref_phase[ch], 4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers)
            { profile_free(p); return NULL; }
    for (int ch = 0; ch < 3; ch++)
        if (fread(p->ref_mag[ch],   4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers)
            { profile_free(p); return NULL; }
    fread_optional(&p->threshold_correlation, 4, 1, f);
    fread_optional(&p->threshold_phase,       4, 1, f);
    fread_optional(&p->corr_mean,             4, 1, f);
    fread_optional(&p->corr_std,              4, 1, f);
    return p;
}

static int codebook_save(const char *path, const Codebook *cb)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    fwrite(CODEBOOK_MAGIC_MULTI, 1, 4, f);
    fwrite(&cb->n_profiles, 4, 1, f);
    for (int i = 0; i < cb->n_profiles; i++)
        profile_write(f, cb->profiles[i]);
    fclose(f);
    return 0;
}

static Codebook *codebook_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    char magic[4];
    if (fread(magic, 1, 4, f) != 4) { fclose(f); return NULL; }

    Codebook *cb = calloc(1, sizeof(Codebook));

    if (memcmp(magic, CODEBOOK_MAGIC_MULTI, 4) == 0) {
        /* New multi-res format */
        if (fread(&cb->n_profiles, 4, 1, f) != 1) { free(cb); fclose(f); return NULL; }
        for (int i = 0; i < cb->n_profiles; i++) {
            cb->profiles[i] = profile_read(f);
            if (!cb->profiles[i]) { codebook_free(cb); fclose(f); return NULL; }
        }
    } else if (memcmp(magic, CODEBOOK_MAGIC_SINGLE, 4) == 0) {
        /* Legacy single-res "SCDB" format — wrap in one profile */
        /* Re-read W,H then delegate to profile_read which expects W,H first */
        /* But profile_read reads W,H itself, so seek back 0 bytes — W,H not
           yet consumed after magic.  Just read directly. */
        Profile *p = profile_alloc(0, 0);
        if (fread(&p->W, 4, 1, f) != 1 || fread(&p->H, 4, 1, f) != 1)
            { profile_free(p); free(cb); fclose(f); return NULL; }
        for (int ch = 0; ch < 3; ch++) {
            free(p->fft[ch]);
            p->fft[ch] = malloc((size_t)p->W * p->H * sizeof(cf32));
            if (fread(p->fft[ch], sizeof(cf32), (size_t)(p->W*p->H), f)
                    != (size_t)(p->W*p->H))
                { profile_free(p); free(cb); fclose(f); return NULL; }
        }
        if (fread(&p->n_carriers, 4, 1, f) != 1)
            { profile_free(p); free(cb); fclose(f); return NULL; }
        if (fread(p->carrier_r, 4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers ||
            fread(p->carrier_c, 4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers)
            { profile_free(p); free(cb); fclose(f); return NULL; }
        for (int ch = 0; ch < 3; ch++)
            if (fread(p->ref_phase[ch], 4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers)
                { profile_free(p); free(cb); fclose(f); return NULL; }
        for (int ch = 0; ch < 3; ch++)
            if (fread(p->ref_mag[ch],   4, (size_t)p->n_carriers, f) != (size_t)p->n_carriers)
                { profile_free(p); free(cb); fclose(f); return NULL; }
        fread_optional(&p->threshold_correlation, 4, 1, f);
        fread_optional(&p->threshold_phase,       4, 1, f);
        fread_optional(&p->corr_mean,             4, 1, f);
        fread_optional(&p->corr_std,              4, 1, f);
        cb->profiles[0] = p;
        cb->n_profiles  = 1;
    } else {
        fprintf(stderr, "Bad codebook magic\n");
        free(cb); fclose(f); return NULL;
    }
    fclose(f);
    return cb;
}

/* -----------------------------------------------------------------------
 * Carrier selection: top-N bins by magnitude, excluding DC neighbourhood
 * ----------------------------------------------------------------------- */
#define TOP_CARRIERS 128

static void select_carriers(Profile *p)
{
    int W = p->W, H = p->H;
    int cx = W/2, cy = H/2;
    float *mags = malloc(W * H * sizeof(float));
    for (int i = 0; i < W*H; i++) {
        cf32 z = p->fft[1][i];
        mags[i] = sqrtf(z.re*z.re + z.im*z.im);
    }
    /* mask DC */
    for (int r = cy-4; r <= cy+4; r++)
        for (int c = cx-4; c <= cx+4; c++)
            if (r>=0&&r<H&&c>=0&&c<W) mags[r*W+c] = 0;

    /* find kth-largest threshold */
    int N = W * H;
    float topk[TOP_CARRIERS];
    for (int k = 0; k < TOP_CARRIERS; k++) topk[k] = -1;
    for (int i = 0; i < N; i++) {
        if (mags[i] > topk[TOP_CARRIERS-1]) {
            topk[TOP_CARRIERS-1] = mags[i];
            for (int k = TOP_CARRIERS-1; k > 0 && topk[k] > topk[k-1]; k--) {
                float t = topk[k]; topk[k] = topk[k-1]; topk[k-1] = t;
            }
        }
    }
    float threshold = topk[TOP_CARRIERS-1];

    p->n_carriers = 0;
    for (int i = 0; i < N && p->n_carriers < TOP_CARRIERS; i++) {
        if (mags[i] < threshold) continue;
        int r = i / W, c = i % W;
        p->carrier_r[p->n_carriers] = r;
        p->carrier_c[p->n_carriers] = c;
        for (int ch = 0; ch < 3; ch++) {
            cf32 z = p->fft[ch][i];
            p->ref_phase[ch][p->n_carriers] = atan2f(z.im, z.re);
            p->ref_mag[ch][p->n_carriers]   = sqrtf(z.re*z.re + z.im*z.im);
        }
        p->n_carriers++;
    }
    free(mags);
}

/* -----------------------------------------------------------------------
 * DETECT on a single profile
 * ----------------------------------------------------------------------- */
typedef struct {
    int   watermarked;
    float confidence;
    float correlation;
    float phase_match;
    float structure_ratio;
    int   profile_W, profile_H;
} DetectResult;

static DetectResult detect_with_profile(const Image *img, const Profile *p)
{
    DetectResult res = {0};
    res.profile_W = p->W;
    res.profile_H = p->H;

    int n = p->n_carriers;
    float corr_sum = 0, phase_sum = 0;
    static const float weights[3] = { 0.85f, 1.0f, 0.70f }; /* R, G, B */

    for (int ch = 0; ch < 3; ch++) {
        float *noise = make_noise(img->ch[ch], img->W, img->H, NOISE_RADIUS);
        cf32  *fft   = channel_fft(noise, img->W, img->H);
        free(noise);

        for (int k = 0; k < n; k++) {
            int idx = p->carrier_r[k] * p->W + p->carrier_c[k];
            cf32  z     = fft[idx];
            float mag   = sqrtf(z.re*z.re + z.im*z.im);
            float phase = atan2f(z.im, z.re);

            float ref_re = p->ref_mag[ch][k] * cosf(p->ref_phase[ch][k]);
            float ref_im = p->ref_mag[ch][k] * sinf(p->ref_phase[ch][k]);
            corr_sum += weights[ch] * (z.re*ref_re + z.im*ref_im)
                      / (mag * p->ref_mag[ch][k] + 1e-9f);

            float dp = fabsf(phase - p->ref_phase[ch][k]);
            while (dp > (float)M_PI) dp = fabsf(dp - 2.0f*(float)M_PI);
            phase_sum += weights[ch] * (1.0f - dp / (float)M_PI);
        }
        free(fft);
    }

    float total_weight = (weights[0] + weights[1] + weights[2]) * n;
    res.correlation = corr_sum  / (total_weight + 1e-9f);
    res.phase_match = phase_sum / (total_weight + 1e-9f);

    /* Structure ratio from green channel */
    {
        float e_in = 0, e_out = 0;
        float *noise = make_noise(img->ch[1], img->W, img->H, NOISE_RADIUS);
        cf32  *fft   = channel_fft(noise, img->W, img->H);
        free(noise);
        for (int i = 0; i < img->W * img->H; i++) {
            cf32 z = fft[i];
            e_out += z.re*z.re + z.im*z.im;
        }
        for (int k = 0; k < n; k++) {
            int idx = p->carrier_r[k] * p->W + p->carrier_c[k];
            cf32 z  = fft[idx];
            float m = z.re*z.re + z.im*z.im;
            e_in  += m;
            e_out -= m;
        }
        free(fft);
        res.structure_ratio = e_in / (e_out / (img->W*img->H - n) * n + 1e-9f);
    }

    res.watermarked = (res.correlation     > p->threshold_correlation &&
                       res.phase_match     > p->threshold_phase       &&
                       res.structure_ratio > 0.5f);

    float score  = res.correlation * 0.6f + res.phase_match * 0.4f;
    float centre = p->threshold_correlation * 0.6f + p->threshold_phase * 0.4f;
    res.confidence = 1.0f / (1.0f + expf(-12.0f * (score - centre)));
    return res;
}

/* -----------------------------------------------------------------------
 * DETECT — auto-selects profile, or resizes image to nearest available size
 * ----------------------------------------------------------------------- */
static DetectResult detect(const Image *img, const Codebook *cb)
{
    DetectResult best = {0};

    /* Try exact match first */
    Profile *exact = codebook_find_profile(cb, img->W, img->H);
    if (exact)
        return detect_with_profile(img, exact);

    /* No exact match: try every profile after resizing the image */
    printf("No exact profile for %dx%d — trying all %d profile(s):\n",
           img->W, img->H, cb->n_profiles);

    for (int i = 0; i < cb->n_profiles; i++) {
        Profile *p = cb->profiles[i];
        Image *resized = image_resize(img, p->W, p->H);
        DetectResult r = detect_with_profile(resized, p);
        image_free(resized);
        printf("  [%dx%d] corr=%.4f phase=%.4f sratio=%.4f -> %s\n",
               p->W, p->H, r.correlation, r.phase_match, r.structure_ratio,
               r.watermarked ? "YES" : "no");
        if (r.confidence > best.confidence)
            best = r;
    }
    return best;
}

/* -----------------------------------------------------------------------
 * BYPASS
 * ----------------------------------------------------------------------- */
static Image *bypass(const Image *img, const Codebook *cb, int strength)
{
    Profile *p = codebook_find_profile(cb, img->W, img->H);
    if (!p) {
        /* Use nearest profile size */
        int best_diff = INT_MAX;
        for (int i = 0; i < cb->n_profiles; i++) {
            int diff = abs(cb->profiles[i]->W - img->W)
                     + abs(cb->profiles[i]->H - img->H);
            if (diff < best_diff) { best_diff = diff; p = cb->profiles[i]; }
        }
        fprintf(stderr, "No exact bypass profile; using %dx%d (image is %dx%d).\n",
                p->W, p->H, img->W, img->H);
    }

    static const float fracs[4] = { 0.25f, 0.50f, 0.75f, 0.95f };
    float frac = fracs[strength < 4 ? strength : 2];
    float cap  = 0.30f;
    static const float cw[3] = { 0.85f, 1.0f, 0.70f };

    /* Work at the profile's resolution */
    Image *work = (img->W == p->W && img->H == p->H)
                ? NULL : image_resize(img, p->W, p->H);
    const Image *src = work ? work : img;

    Image *out = calloc(1, sizeof(Image));
    out->W = src->W; out->H = src->H;

    for (int ch = 0; ch < 3; ch++) {
        out->ch[ch] = malloc(src->W * src->H * sizeof(float));
        cf32 *fft = malloc(src->W * src->H * sizeof(cf32));
        for (int i = 0; i < src->W*src->H; i++) {
            fft[i].re = src->ch[ch][i]; fft[i].im = 0;
        }
        fft2d(fft, src->H, src->W, 0);
        fftshift2d(fft, src->H, src->W);

        for (int k = 0; k < p->n_carriers; k++) {
            int idx = p->carrier_r[k] * p->W + p->carrier_c[k];
            cf32 z  = fft[idx];
            float img_mag = sqrtf(z.re*z.re + z.im*z.im);
            float wm_mag  = p->ref_mag[ch][k] * frac * cw[ch] * 3.8525f * src->W * src->H;
            if (wm_mag > cap * img_mag) wm_mag = cap * img_mag;
            fft[idx].re -= wm_mag * cosf(p->ref_phase[ch][k]);
            fft[idx].im -= wm_mag * sinf(p->ref_phase[ch][k]);
            int symr = src->H - p->carrier_r[k]; if(symr>=src->H)symr=0;
            int symc = src->W - p->carrier_c[k]; if(symc>=src->W)symc=0;
            int sym  = symr * p->W + symc;
            fft[sym].re -= wm_mag * cosf(p->ref_phase[ch][k]);
            fft[sym].im += wm_mag * sinf(p->ref_phase[ch][k]);
        }
        fftshift2d(fft, src->H, src->W);
        fft2d(fft, src->H, src->W, 1);
        for (int i = 0; i < src->W * src->H; i++) {
            float v = fft[i].re;
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            out->ch[ch][i] = v;
        }
        free(fft);
    }
    if (work) image_free(work);
    return out;
}

/* -----------------------------------------------------------------------
 * EXTRACT — build a codebook from a directory of reference images
 * ----------------------------------------------------------------------- */
static Codebook *extract_codebook(const char *dir_path)
{
    DIR *d = opendir(dir_path);
    if (!d) { perror(dir_path); return NULL; }

    char **names = NULL; int nnames = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *n = ent->d_name; size_t L = strlen(n);
        if (L < 4) continue;
        const char *ext = n + L - 4;
        if (strcasecmp(ext,".png")!=0 && strcasecmp(ext,".jpg")!=0 &&
            (L<5||strcasecmp(n+L-5,".jpeg")!=0)) continue;
        names = realloc(names, (nnames+1)*sizeof(char*));
        char *full = malloc(strlen(dir_path)+strlen(n)+2);
        sprintf(full, "%s/%s", dir_path, n);
        names[nnames++] = full;
    }
    closedir(d);
    if (!nnames) { fprintf(stderr, "No images in %s\n", dir_path); return NULL; }
    printf("Found %d reference images.\n", nnames);

    Image *first = image_load(names[0]);
    if (!first) return NULL;
    int baseW = next_pow2(first->W); (void)next_pow2(first->H);
    image_free(first);

    /* Build one profile per power-of-two scale from baseW/4 up to baseW */
    int scales[4]; int nscales = 0;
    for (int s = baseW/4; s <= baseW && nscales < 4; s *= 2)
        if (s >= 64) scales[nscales++] = s;

    Codebook *cb = calloc(1, sizeof(Codebook));
    cb->n_profiles = nscales;
    for (int si = 0; si < nscales; si++)
        cb->profiles[si] = profile_alloc(scales[si], scales[si]);

    int *counts = calloc(nscales, sizeof(int));

    for (int fi = 0; fi < nnames; fi++) {
        Image *img = image_load(names[fi]);
        if (!img) { free(names[fi]); continue; }

        for (int si = 0; si < nscales; si++) {
            int S = scales[si];
            Image *scaled = (img->W == S && img->H == S)
                          ? img : image_resize(img, S, S);
            Profile *p = cb->profiles[si];

            for (int ch = 0; ch < 3; ch++) {
                float *noise = make_noise(scaled->ch[ch], S, S, NOISE_RADIUS);
                cf32  *buf   = calloc(S * S, sizeof(cf32));
                for (int k = 0; k < S*S; k++) { buf[k].re = noise[k]; }
                free(noise);
                fft2d(buf, S, S, 0);
                fftshift2d(buf, S, S);
                for (int k = 0; k < S*S; k++) {
                    p->fft[ch][k].re += buf[k].re;
                    p->fft[ch][k].im += buf[k].im;
                }
                free(buf);
            }
            if (scaled != img) image_free(scaled);
        }
        counts[0]++;  /* same count for all scales */
        image_free(img);
        free(names[fi]);
        if ((counts[0]) % 10 == 0)
            printf("  %d / %d ...\r", counts[0], nnames);
        fflush(stdout);
    }
    free(names); free(counts);
    printf("\nProcessed %d images across %d scales.\n", counts ? 0 : 0, nscales);

    for (int si = 0; si < nscales; si++) {
        Profile *p = cb->profiles[si];
        int loaded = nnames;  /* reuse nnames as count */
        for (int ch = 0; ch < 3; ch++)
            for (int k = 0; k < p->W * p->H; k++) {
                p->fft[ch][k].re /= loaded;
                p->fft[ch][k].im /= loaded;
            }
        select_carriers(p);
        printf("  Scale %dx%d: %d carriers\n", p->W, p->H, p->n_carriers);
    }
    return cb;
}

/* -----------------------------------------------------------------------
 * CLI
 * ----------------------------------------------------------------------- */
static void usage(const char *a){
    fprintf(stderr,
        "Usage:\n"
        "  %s detect  <image.png> <codebook.bin>\n"
        "  %s bypass  <input.png> <output.png> <codebook.bin> [gentle|moderate|aggressive|maximum]\n"
        "  %s extract <image_dir/> <codebook.bin>\n",a,a,a);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 1; }
    const char *cmd = argv[1];

    if (strcmp(cmd, "detect") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        Image    *img = image_load(argv[2]);
        Codebook *cb  = codebook_load(argv[3]);
        if (!img || !cb) return 1;

        DetectResult r = detect(img, cb);
        printf("Detection Results:\n");
        printf("  Watermarked    : %s\n",   r.watermarked ? "YES" : "NO");
        printf("  Confidence     : %.4f\n", r.confidence);
        printf("  Profile used   : %dx%d\n",r.profile_W, r.profile_H);
        printf("  Correlation    : %.4f\n", r.correlation);
        printf("  Phase Match    : %.4f\n", r.phase_match);
        printf("  Structure Ratio: %.4f\n", r.structure_ratio);

        /* Print all available profiles */
        printf("Profiles in codebook: %d\n", cb->n_profiles);
        for (int i = 0; i < cb->n_profiles; i++)
            printf("  [%d] %dx%d  %d carriers  corr_thresh=%.4f\n",
                   i, cb->profiles[i]->W, cb->profiles[i]->H,
                   cb->profiles[i]->n_carriers,
                   cb->profiles[i]->threshold_correlation);

        image_free(img); codebook_free(cb);
        return r.watermarked ? 0 : 2;

    } else if (strcmp(cmd, "bypass") == 0) {
        if (argc < 5) { usage(argv[0]); return 1; }
        int strength = 2;
        if (argc >= 6) {
            if      (!strcmp(argv[5],"gentle"))     strength=0;
            else if (!strcmp(argv[5],"moderate"))   strength=1;
            else if (!strcmp(argv[5],"aggressive")) strength=2;
            else if (!strcmp(argv[5],"maximum"))    strength=3;
        }
        Image    *img = image_load(argv[2]);
        Codebook *cb  = codebook_load(argv[4]);
        if (!img || !cb) return 1;
        Image *out = bypass(img, cb, strength);
        if (!out) return 1;
        image_save(argv[3], out);
        printf("Saved to %s\n", argv[3]);
        image_free(img); image_free(out); codebook_free(cb);
        return 0;

    } else if (strcmp(cmd, "extract") == 0) {
        if (argc < 4) { usage(argv[0]); return 1; }
        Codebook *cb = extract_codebook(argv[2]);
        if (!cb) return 1;
        if (codebook_save(argv[3], cb) == 0)
            printf("Codebook saved to %s (%d profiles)\n", argv[3], cb->n_profiles);
        codebook_free(cb);
        return 0;

    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage(argv[0]); return 1;
    }
}
