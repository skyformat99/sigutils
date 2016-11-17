/*

  Copyright (C) 2016 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/
#include <stdlib.h>
#include <string.h>
#include "block.h"
#include "ncqo.h"
#include "iir.h"
#include "taps.h"

/* A tuner is just a NCQO + Low pass filter */
struct sigutils_tuner {
  su_iir_filt_t lpf; /* Lowpass filter */
  su_ncqo_t  lo;     /* Local oscillator */
  SUFLOAT   *h;      /* RRC filter coefcients */
  SUCOMPLEX *x;      /* Signal input buffer */
  unsigned int x_p;  /* Buffer pointer */
  unsigned int d;    /* Decimation */

  /* Filter params */
  SUFLOAT T;           /* Samples per symbol */
  unsigned int h_size; /* Filter size */
  SUFLOAT beta;        /* Roloff */

  /* Configurable params */
  SUFLOAT      rq_T;
  unsigned int rq_h_size;
  SUFLOAT      rq_beta;
  SUFLOAT      rq_fc; /* Center frequency (1 ~ fs/2), hcps */
};

typedef struct sigutils_tuner su_tuner_t;

SUPRIVATE SUBOOL
su_tuner_filter_has_changed(su_tuner_t *tu)
{
  return
      tu->rq_T != tu->T       ||
      tu->rq_beta != tu->beta ||
      tu->rq_h_size != tu->h_size;
}

SUPRIVATE SUBOOL
su_tuner_lo_has_changed(su_tuner_t *tu)
{
  return -su_ncqo_get_freq(&tu->lo) != tu->rq_fc;
}

SUPRIVATE void
su_tuner_feed(su_tuner_t *tu, const SUCOMPLEX *data, size_t size)
{
  unsigned int i;
  SUCOMPLEX losamp;

  /* Mixing happens here */
  for (i = 0; i < size; ++i) {
    losamp = su_ncqo_read(&tu->lo);
    tu->x[tu->x_p++] = su_iir_filt_feed(&tu->lpf, data[i] * losamp);
    if (tu->x_p == tu->h_size)
      tu->x_p = 0;
  }
}

SUPRIVATE SUCOMPLEX
su_tuner_read(const su_tuner_t *tu)
{
  unsigned int i;
  unsigned int n;
  SUCOMPLEX result = 0;

  n = tu->x_p;

  for (i = 0; i < tu->h_size; ++i) {
    n = n > 0 ? n - 1 : tu->h_size - 1;
    result += tu->x[n] * tu->h[i];
  }

  return result;
}

SUPRIVATE SUBOOL
su_tuner_update_filter(su_tuner_t *tu)
{
  su_iir_filt_t lpf_new = su_iir_filt_INITIALIZER;
  SUFLOAT *h_new = tu->h;
  SUCOMPLEX *x_new = tu->x;
  SUBOOL reallocate_rrc = SU_FALSE;
  SUBOOL reallocate_lpf = SU_FALSE;

  reallocate_rrc = tu->rq_h_size > tu->h_size;
  reallocate_lpf = tu->rq_T != tu->T;

  /* Filter has grown */
  if (reallocate_rrc) {
    if ((h_new = malloc(tu->rq_h_size * sizeof(SUFLOAT))) == NULL)
      goto fail;

    if ((x_new = calloc(sizeof(SUCOMPLEX), tu->rq_h_size)) == NULL)
      goto fail;

  } else {
    h_new = tu->h;
    x_new = tu->x;
  }

  /* If baudrate has changed, we must change the LPF */
  if (reallocate_lpf) {
    if (!su_iir_bwlpf_init(&lpf_new, 5, 1. / tu->rq_T))
      goto fail;
  }

  /* Initialize it */
  su_taps_rrc_init(h_new, tu->rq_T, tu->rq_beta, tu->rq_h_size);

  /* Update filter params. Nothing must fail from here */
  if (reallocate_rrc) {
    if (tu->h != NULL)
      free(tu->h);
    if (tu->x != NULL)
      free(tu->x);
    tu->h = h_new;
    tu->x = x_new; /* TODO: copy old samples */

  }

  if (reallocate_lpf) {
    su_iir_filt_finalize(&tu->lpf);
    tu->lpf = lpf_new;
    tu->T   = tu->rq_T;
  }

  tu->beta = tu->rq_beta;
  tu->h_size = tu->rq_h_size;
  return SU_TRUE;

fail:
  if (reallocate_rrc) {
    if (h_new != NULL)
      free(h_new);

    if (x_new != NULL)
      free(x_new);
  }

  if (reallocate_lpf)
    su_iir_filt_finalize(&lpf_new);

  return SU_FALSE;
}

SUPRIVATE void
su_tuner_update_lo(su_tuner_t *tu)
{
  su_ncqo_set_freq(&tu->lo, -tu->rq_fc);
}

void
su_tuner_destroy(su_tuner_t *tu)
{
  if (tu->h != NULL)
    free(tu->h);

  if (tu->x != NULL)
    free(tu->x);

  free(tu);
}

su_tuner_t *
su_tuner_new(SUFLOAT fc, SUFLOAT T, SUFLOAT beta, unsigned int size)
{
  su_tuner_t *new;

  if ((new = calloc(1, sizeof (su_tuner_t))) == NULL)
    goto fail;

  new->rq_fc     = fc;
  new->rq_T      = T;
  new->rq_beta   = beta;
  new->rq_h_size = size;

  if (!su_tuner_update_filter(new))
    goto fail;

  su_ncqo_init(&new->lo, new->rq_fc);

  return new;

fail:
  if (new != NULL)
    su_tuner_destroy(new);

  return NULL;
}

/* Tuner constructor */
SUPRIVATE SUBOOL
su_block_tuner_ctor(struct sigutils_block *block, void **private, va_list ap)
{
  su_tuner_t *tu = NULL;
  SUBOOL ok = SU_FALSE;
  SUFLOAT fc;
  SUFLOAT T;
  SUFLOAT beta;
  unsigned int size;
  unsigned int d;

  fc   = va_arg(ap, SUFLOAT);
  T    = va_arg(ap, SUFLOAT);
  beta = va_arg(ap, SUFLOAT);
  size = va_arg(ap, unsigned int);

  if ((tu = su_tuner_new(fc, T, beta, size)) == NULL)
    goto done;

  /* 1. / T: Symbols per sample (normalized baudrate) */
  block->decimation = SU_CEIL(1. / (4. * T));
  tu->d = block->decimation;

  ok = SU_TRUE;

  /* Set configurable properties */
  ok = ok && su_block_set_property_ref(
      block,
      SU_BLOCK_PROPERTY_TYPE_FLOAT,
      "T",
      &tu->rq_T);

  ok = ok && su_block_set_property_ref(
      block,
      SU_BLOCK_PROPERTY_TYPE_FLOAT,
      "fc",
      &tu->rq_fc);

  ok = ok && su_block_set_property_ref(
      block,
      SU_BLOCK_PROPERTY_TYPE_FLOAT,
      "beta",
      &tu->rq_beta);

  ok = ok && su_block_set_property_ref(
      block,
      SU_BLOCK_PROPERTY_TYPE_INTEGER,
      "size",
      &tu->rq_h_size);

  ok = ok && su_block_set_property_ref(
      block,
      SU_BLOCK_PROPERTY_TYPE_INTEGER,
      "decimation",
      &tu->d);

done:
  if (!ok) {
    if (tu != NULL)
      su_tuner_destroy(tu);
  }
  else
    *private = tu;

  return ok;
}

/* Tuner destructor */
SUPRIVATE void
su_block_tuner_dtor(void *private)
{
  su_tuner_t *tu = (su_tuner_t *) private;

  if (tu != NULL) {
    su_tuner_destroy(tu);
  }
}

/* Acquire */
SUPRIVATE ssize_t
su_block_tuner_acquire(void *priv, su_stream_t *out, su_block_port_t *in)
{
  su_tuner_t *tu = (su_tuner_t *) priv;
  ssize_t size, got;
  int i;
  unsigned int j;
  SUCOMPLEX samp;

  /* Fill this buffer completely */
  size = out->size;

  /* Need to update filter? */
  if (su_tuner_filter_has_changed(tu))
    if (!su_tuner_update_filter(tu)) {
      SU_ERROR("Failed to update filter!\n");
      return SU_BLOCK_PORT_READ_ERROR_ACQUIRE;
    }

  /* Need to update local oscillator? */
  if (su_tuner_lo_has_changed(tu))
    su_tuner_update_lo(tu);

  for (i = 0; i < size; ++i) {
    /* Read d samples */
    for (j = 0; j < tu->d; ++j) {
      /* XXX: this is awful. Please fix */
      if ((got = su_block_port_read(in, &samp, 1)) < 1)
        return got;

      /* Feed tunner */
      su_tuner_feed(tu, &samp, 1);
    }

    /* Read filtered output */
    samp = su_tuner_read(tu);

    su_stream_write(out, &samp, 1);
  }

  return size;
}

struct sigutils_block_class su_block_class_TUNER = {
    "tuner", /* name */
    1,     /* in_size */
    1,     /* out_size */
    su_block_tuner_ctor,    /* constructor */
    su_block_tuner_dtor,    /* destructor */
    su_block_tuner_acquire  /* acquire */
};

