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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sigutils/sampling.h>
#include <sigutils/sigutils.h>
#include <sigutils/detect.h>

#include "test_list.h"
#include "test_param.h"

#define SU_TEST_CHANNEL_DETECTOR_SIGNAL_FREQ 1e-1

SUPRIVATE SUBOOL
__su_test_channel_detector_qpsk(su_test_context_t *ctx, SUBOOL noisy)
{
  SUBOOL ok = SU_FALSE;
  SUCOMPLEX *input = NULL;
  SUFLOAT N0 = 0;
  SUCOMPLEX *tx = NULL;
  SUCOMPLEX *data = NULL;
  SUCOMPLEX bbs = 1;
  SUCOMPLEX symbols[] = {1, I, -1, -I};
  SUCOMPLEX phi0 = 1;
  SUFLOAT *fft = NULL;
  SUFLOAT sigma;

  unsigned int filter_period;
  unsigned int symbol_period;
  unsigned int message;
  unsigned int msgbuf;

  su_ncqo_t ncqo = su_ncqo_INITIALIZER;
  su_iir_filt_t mf = su_iir_filt_INITIALIZER;
  struct sigutils_channel_detector_params params =
      sigutils_channel_detector_params_INITIALIZER;
  su_channel_detector_t *detector = NULL;
  unsigned int p = 0;
  unsigned int sym;
  unsigned int n = 0;
  unsigned int i;
  struct sigutils_channel *channel;

  SU_TEST_START_TICKLESS(ctx);

  /* Initialize some parameters */
  symbol_period = SU_TEST_COSTAS_SYMBOL_PERIOD;
  filter_period = SU_TEST_MF_SYMBOL_SPAN * symbol_period; /* Span: 6 symbols */
  message       = 0x414c4f48; /* Some greeting message */

  if (noisy)
    N0          = SU_POWER_MAG(-10);
  else
    N0          = SU_POWER_MAG(-20);

  sigma         = sqrt(N0 / 2);

  phi0          = SU_C_EXP(I * M_PI / 4); /* Phase offset */

  /* Initialize channel detector */
  params.samp_rate = 250000;
  params.alpha = 1e-1;
  params.window_size = 4096;

  /* Initialize buffers */
  SU_TEST_ASSERT(input    = su_test_ctx_getc(ctx, "x"));
  SU_TEST_ASSERT(data     = su_test_ctx_getc(ctx, "data"));
  SU_TEST_ASSERT(tx       = su_test_ctx_getc(ctx, "tx"));
  SU_TEST_ASSERT(
      fft      = su_test_ctx_getf_w_size(ctx, "spectrogram", params.window_size));

  SU_TEST_ASSERT(detector = su_channel_detector_new(&params));

  /* Initialize oscillator */
  su_ncqo_init(&ncqo, SU_TEST_CHANNEL_DETECTOR_SIGNAL_FREQ);

#ifndef SU_TEST_COSTAS_USE_RRC
  SU_TEST_ASSERT(
      su_iir_rrc_init(
          &mf,
          filter_period,
          symbol_period,
          1));
#else
  SU_TEST_ASSERT(
      su_iir_brickwall_init(
          &mf,
          filter_period,
          SU_TEST_COSTAS_BANDWIDTH));
#endif

  /* Create QPSK signal */
  msgbuf = message;
  SU_INFO("Modulating 0x%x in QPSK...\n", msgbuf);
  SU_INFO("  Noise: %lg dBFS\n", SU_POWER_DB(N0));
  SU_INFO("  Window size: %d samples\n", params.window_size);
  SU_INFO("  Baudrate at fs=%d: %lg\n",
          params.samp_rate,
          SU_NORM2ABS_BAUD(params.samp_rate, 1. / symbol_period));


  for (p = 0; p < ctx->params->buffer_size; ++p) {
    if (p % symbol_period == 0) {
      if (n == 32)
        n = 0;
      msgbuf = message >> n;
      sym = msgbuf & 3;
      n += 2;
      bbs = symbol_period * symbols[sym];
    } else {
      bbs = 0;
    }

    data[p] = bbs;
    input[p] = su_iir_filt_feed(&mf, data[p]);
    tx[p] = phi0 * input[p] * su_ncqo_read(&ncqo) + sigma * su_c_awgn();
  }

  SU_TEST_TICK(ctx);

  /* Feed detector */
  for (p = 0; p < ctx->params->buffer_size; ++p)
    su_channel_detector_feed(detector, tx[p]);

  SU_INFO(
      "Frequency step: %lg Hz\n",
      (double) params.samp_rate / (double) params.window_size);

  SU_TEST_ASSERT(
      channel = su_channel_detector_lookup_valid_channel(
          detector,
          SU_NORM2ABS_FREQ(
              params.samp_rate,
              SU_TEST_CHANNEL_DETECTOR_SIGNAL_FREQ)));

  SU_INFO("Channel found by detector:\n");
  SU_INFO("  Actual frequency: %lg Hz\n",
          SU_NORM2ABS_FREQ(
              params.samp_rate,
              SU_TEST_CHANNEL_DETECTOR_SIGNAL_FREQ));
  SU_INFO("  Detected frequency: %lg Hz\n", channel->fc);
  SU_INFO("  Bandwidth: %lg Hz\n", channel->bw);
  SU_INFO("  SNR: %lg dB\n", channel->snr);
  SU_INFO("  Noise floor: %lg dB\n", channel->N0);
  SU_INFO("  Signal peak: %lg dB\n", channel->S0);
  ok = SU_TRUE;

done:
  if (detector != NULL) {
    memcpy(fft, detector->spect, params.window_size * sizeof(SUFLOAT));
    su_channel_detector_destroy(detector);
  }

  su_iir_filt_finalize(&mf);

  return ok;
}

SUBOOL
su_test_channel_detector_qpsk(su_test_context_t *ctx)
{
  return __su_test_channel_detector_qpsk(ctx, SU_FALSE);
}

SUBOOL
su_test_channel_detector_qpsk_noisy(su_test_context_t *ctx)
{
  return __su_test_channel_detector_qpsk(ctx, SU_TRUE);
}

SUBOOL
su_test_channel_detector_real_capture(su_test_context_t *ctx)
{
  SUBOOL ok = SU_FALSE;
  complex float *input = (complex float *) -1; /* Required by mmap */
  SUFLOAT *spect;
  SUFLOAT *spmax;
  SUFLOAT *spmin;
  SUFLOAT *n0est;

  struct sigutils_channel_detector_params params =
      sigutils_channel_detector_params_INITIALIZER;
  su_channel_detector_t *detector = NULL;
  struct stat sbuf;
  unsigned int i;
  unsigned int n = 0;
  unsigned int j = 0;
  SUSCOUNT samples;
  int fd = -1;
  struct sigutils_channel **channel_list;
  unsigned int channel_count;

  SU_TEST_START_TICKLESS(ctx);

  if (access(SU_CHANNEL_DETECTOR_SAMPLE_CAPTURE, F_OK) == -1) {
    SU_INFO("Sample capture file not present, skipping test...\n");
    ok = SU_TRUE;
    goto done;
  }

  SU_TEST_ASSERT(stat(SU_CHANNEL_DETECTOR_SAMPLE_CAPTURE, &sbuf) != -1);

  SU_TEST_ASSERT((fd = open(
      SU_CHANNEL_DETECTOR_SAMPLE_CAPTURE,
      O_RDONLY)) != -1);

  SU_TEST_ASSERT((input = (complex float *) mmap(
      NULL,         /* addr */
      sbuf.st_size, /* size */
      PROT_READ,    /* prot */
      MAP_PRIVATE,  /* flags */
      fd,           /* fd */
      0             /* offset */)) != (complex float *) -1);

  close(fd); /* We don't need this anymore */
  fd = -1;

  /*
   * Note GQRX samples are pairs of complex floats
   * (32 bits for I and 32 bits for Q). SUCOMPLEX is
   * probably something different
   */
  samples = sbuf.st_size / sizeof(complex float);

  /* Initialize channel detector */
  params.samp_rate = 250000;
  params.alpha = 1e-2;
  params.window_size = 4096;

  /* Create debug buffers */
  SU_TEST_ASSERT(
      spect = su_test_ctx_getf_w_size(ctx, "spect", params.window_size));
  SU_TEST_ASSERT(
      spmax = su_test_ctx_getf_w_size(ctx, "spmax", params.window_size));
  SU_TEST_ASSERT(
      spmin = su_test_ctx_getf_w_size(ctx, "spmin", params.window_size));
  SU_TEST_ASSERT(
      n0est = su_test_ctx_getf_w_size(
          ctx,
          "n0est",
          samples / params.window_size
          + !!(samples % params.window_size)));

  SU_TEST_ASSERT(detector = su_channel_detector_new(&params));

  /* Go, go, go */
  SU_INFO("Feeding %d samples to channel detector\n", samples);
  SU_INFO(
      "Capture is %02d:%02d long\n",
      samples / (params.samp_rate * 60),
      (samples / (params.samp_rate)) % 60);

  SU_TEST_TICK(ctx);

  for (i = 0; i < samples; ++i) {
    SU_TEST_ASSERT(
        su_channel_detector_feed(
            detector,
            SU_C_CONJ((SUCOMPLEX) input[i]))); /* Gqrx inverts the Q channel */

    if ((i % params.window_size) == 0)
      n0est[j++] = detector->N0;
  }

  /* Print results */
  su_channel_detector_get_channel_list(
      detector,
      &channel_list,
      &channel_count);

  SU_INFO(
      "Computed noise floor: %lg dB\n",
      SU_POWER_DB(detector->N0));
  for (i = 0; i < channel_count; ++i)
    if (channel_list[i] != NULL)
      if (SU_CHANNEL_IS_VALID(channel_list[i])) {
        ++n;
        SU_INFO(
            "%2d. | %+8.1lf Hz | %+7.1lf Hz | %5.1lf dB\n",
            n,
            channel_list[i]->fc,
            channel_list[i]->bw,
            channel_list[i]->snr);
      }

  /* Copy spectrums */

  for (i = 0; i < params.window_size; ++i) {
    spect[i] = detector->spect[i];
    spmax[i] = detector->spmax[i];
    spmin[i] = detector->spmin[i];
  }

  ok = SU_TRUE;

done:
  if (detector != NULL)
    su_channel_detector_destroy(detector);

  if (fd != -1)
    close(fd);

  return ok;
}


