/*
 * Copyright (C) 2012 Fergus Noble <fergus@swift-nav.com>
 *
 * This source is subject to the license found in the file 'LICENSE' which must
 * be be distributed together with this source. All other rights reserved.
 *
 * THIS CODE AND INFORMATION IS PROVIDED "AS IS" WITHOUT WARRANTY OF ANY KIND,
 * EITHER EXPRESSED OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND/OR FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <math.h>

#include "pvt.h"
#include "prns.h"
#include "track.h"
#include "ephemeris.h"
#include "tropo.h"
#include "coord_system.h"

/** \addtogroup lib
 * \{ */
/** \defgroup track Tracking
 * Functions used in tracking.
 * \{ */

/** \defgroup track_loop Tracking Loops
 * Functions used by the tracking loops.
 * \{ */

void calc_loop_coeff(double BW, double zeta, double k, double *tau1,
                     double *tau2) {
  /* Solve for the natural frequency. */
  double omega_n = BW*8*zeta / (4*zeta*zeta + 1);

  *tau1 = k / (omega_n*omega_n);
  *tau2 = 2*zeta/omega_n;
}

/** Calculate coefficients for a 2nd order digital PLL / DLL loop filter.
 *
 * A second order digital PLL consists of a first-order filter and a
 * Numerically Controlled Oscillator (NCO). A linearised model of a digital
 * second order PLL is shown below:
 *
 * \image html 2nd_order_dpll.png Linearised digital second order PLL model.
 *
 * Where \f$K_d\f$ is the discriminator gain and \f$F[z]\f$ and \f$N[z]\f$ are
 * the loop filter and NCO transfer functions. The NCO is essentially an
 * accumulator and hence has a transfer function:
 *
 * \f[
 *   N[z] = \frac{K_0 z^{-1}}{1 - z^{-1}}
 * \f]
 *
 * The first-order loop filter is shown below:
 *
 * \image html 2nd_order_loop_filter.png Digital loop filter block diagram.
 *
 * and has transfer function:
 *
 * \f[
 *   F[z] = \frac{(k_p+k_i) - k_p z^{-1}}{1 - z^{-1}}
 * \f]
 *
 * It is useful to be able to calculate the loop filter coefficients, \f$k_p\f$
 * and \f$k_i\f$ by comparison to the parameters used in analog PLL design. By
 * comparison between the digital and analog PLL transfer functions we can show
 * the digital loop filter coefficients are related to the analog PLL natural
 * frequency, \f$\omega_n\f$ and damping ratio, \f$\zeta\f$ by:
 *
 * \f[
 *   k_p = \frac{1}{k} \frac{8 \zeta \omega_n T}
 *         {4 + 4 \zeta \omega_n T + \omega_n^2 T^2}
 * \f]
 * \f[
 *   k_i = \frac{1}{k} \frac{4 \omega_n^2 T^2}
 *         {4 + 4 \zeta \omega_n T + \omega_n^2 T^2}
 * \f]
 *
 * Where \f$T\f$ is the sampling time and the overall loop gain, \f$k\f$, is
 * the product of the NCO and discriminator gains, \f$ k = K_0 K_d \f$. The
 * natural frequency is related to the loop noise bandwidth, \f$B_L\f$ by the
 * following relationship:
 *
 * \f[
 *   \omega_n = \frac{8 \zeta B_L}{4 \zeta^2 + 1}
 * \f]
 *
 * These coefficients are applicable to both the Carrier phase Costas loop and
 * the Code phase DLL.
 *
 * References:
 *  -# Performance analysis of an all-digital BPSK direct-sequence
 *     spread-spectrum IF receiver architecture.
 *     B-Y. Chung, C. Chien, H. Samueli, and R. Jain.
 *     IEEE Journal on Selected Areas in Communications, 11:1096–1107, 1993.
 *
 * \param bw          The loop noise bandwidth, \f$B_L\f$.
 * \param zeta        The damping ratio, \f$\zeta\f$.
 * \param k           The loop gain, \f$k\f$.
 * \param sample_freq The sampling frequency, \f$1/T\f$.
 * \param pgain       Where to store the calculated proportional gain,
 *                    \f$k_p\f$.
 * \param igain       Where to store the calculated integral gain, \f$k_i\f$.
 */
void calc_loop_gains(double bw, double zeta, double k, double sample_freq,
                     double *pgain, double *igain) {
  /* Find the natural frequency. */
  double omega_n = bw*8*zeta / (4*zeta*zeta + 1);

  /* Some intermmediate values. */
  double T = 1. / sample_freq;
  double denominator = k*(4 + 4*zeta*omega_n*T + omega_n*omega_n*T*T);

  *pgain = 8*zeta*omega_n*T / denominator;
  *igain = 4*omega_n*omega_n*T*T / denominator;
}

/** Phase discriminator for a Costas loop.
 *
 * \image html costas_loop.png Costas loop block diagram.
 *
 * Implements the \f$\tan^{-1}\f$ Costas loop discriminator.
 *
 * \f[
 *   \varepsilon_k = \tan^{-1} \left(\frac{I_k}{Q_k}\right)
 * \f]
 *
 * \param I The prompt in-phase correlation, \f$I_k\f$.
 * \param Q The prompt quadrature correlation, \f$Q_k\f$.
 * \return The discriminator value, \f$\varepsilon_k\f$.
 */
double costas_discriminator(double I, double Q) {
  return atan(I/Q)/(2*M_PI);
}

double dll_discriminator(correlation_t cs[3]) {
  double early_mag = sqrt((double)cs[0].I*cs[0].I + (double)cs[0].Q*cs[0].Q);
  double late_mag = sqrt((double)cs[2].I*cs[2].I + (double)cs[2].Q*cs[2].Q);

  return (early_mag - late_mag) / (early_mag + late_mag);
}

/** \} */

void calc_navigation_measurement(u8 n_channels, channel_measurement_t meas[], navigation_measurement_t nav_meas[], double nav_time, ephemeris_t ephemerides[])
{
  channel_measurement_t* meas_ptrs[n_channels];
  navigation_measurement_t* nav_meas_ptrs[n_channels];
  ephemeris_t* ephemerides_ptrs[n_channels];

  for (u8 i=0; i<n_channels; i++) {
    meas_ptrs[i] = &meas[i];
    nav_meas_ptrs[i] = &nav_meas[i];
    ephemerides_ptrs[i] = &ephemerides[i];
  }

  calc_navigation_measurement_(n_channels, meas_ptrs, nav_meas_ptrs, nav_time, ephemerides_ptrs);
}

void calc_navigation_measurement_(u8 n_channels, channel_measurement_t* meas[], navigation_measurement_t* nav_meas[], double nav_time, ephemeris_t* ephemerides[])
{
  double TOTs[n_channels];
  double mean_TOT = 0;

  for (u8 i=0; i<n_channels; i++) {
    TOTs[i] = 1e-3 * meas[i]->time_of_week_ms;
    TOTs[i] += meas[i]->code_phase_chips / 1.023e6;
    TOTs[i] += (nav_time - meas[i]->receiver_time) * meas[i]->code_phase_rate / 1.023e6;

    nav_meas[i]->TOT = TOTs[i];
    mean_TOT += TOTs[i];
    nav_meas[i]->pseudorange_rate = NAV_C * -meas[i]->carrier_freq / GPS_L1_HZ;
  }

  mean_TOT = mean_TOT/n_channels;

  double clock_err, clock_rate_err;

  for (u8 i=0; i<n_channels; i++) {
    nav_meas[i]->pseudorange = (mean_TOT - TOTs[i])*NAV_C + NOMINAL_RANGE;

    calc_sat_pos(nav_meas[i]->sat_pos, nav_meas[i]->sat_vel, &clock_err, &clock_rate_err, ephemerides[i], TOTs[i]);

    nav_meas[i]->pseudorange += clock_err*NAV_C;
    nav_meas[i]->pseudorange_rate -= clock_rate_err*NAV_C;
  }
}

void apply_tropo_correction(u8 n_channels, navigation_measurement_t* nav_meas[], double ref_ecef[3])
{
  /*double ref_ecef[3] = {3428027.88064438,   603837.64228578,  5326788.33674493};*/
  double az, el;

  for (u8 i=0; i<n_channels; i++) {
    wgsecef2azel(nav_meas[i]->sat_pos, ref_ecef, &az, &el);
    nav_meas[i]->pseudorange -= tropo_correction(el);
  }
}


void track_correlate(s8* samples, s8* code,
                     double* init_code_phase, double code_step, double* init_carr_phase, double carr_step,
                     double* I_E, double* Q_E, double* I_P, double* Q_P, double* I_L, double* Q_L, u32* num_samples)
{
  double code_phase = *init_code_phase;
  double carr_phase = *init_carr_phase;

  double carr_sin = sin(carr_phase);
  double carr_cos = cos(carr_phase);
  double sin_delta = sin(carr_step);
  double cos_delta = cos(carr_step);

  *I_E = *Q_E = *I_P = *Q_P = *I_L = *Q_L = 0;

  double code_E, code_P, code_L;
  double baseband_Q, baseband_I;

  *num_samples = (int)ceil((1023.0 - code_phase) / code_step);

  for (u32 i=0; i<*num_samples; i++) {
    /*code_E = get_chip(code, (int)ceil(code_phase-0.5));*/
    /*code_P = get_chip(code, (int)ceil(code_phase));*/
    /*code_L = get_chip(code, (int)ceil(code_phase+0.5));*/
    code_E = code[(int)ceil(code_phase-0.5)];
    code_P = code[(int)ceil(code_phase)];
    code_L = code[(int)ceil(code_phase+0.5)];

    baseband_Q = carr_cos * samples[i];
    baseband_I = carr_sin * samples[i];

    double carr_sin_ = carr_sin*cos_delta + carr_cos*sin_delta;
    double carr_cos_ = carr_cos*cos_delta - carr_sin*sin_delta;
    double i_mag = (3.0 - carr_sin_*carr_sin_ - carr_cos_*carr_cos_) / 2.0;
    carr_sin = carr_sin_ * i_mag;
    carr_cos = carr_cos_ * i_mag;

    *I_E += code_E * baseband_I;
    *Q_E += code_E * baseband_Q;
    *I_P += code_P * baseband_I;
    *Q_P += code_P * baseband_Q;
    *I_L += code_L * baseband_I;
    *Q_L += code_L * baseband_Q;

    code_phase += code_step;
    carr_phase += carr_step;
  }
  *init_code_phase = code_phase - 1023;
  *init_carr_phase = fmod(carr_phase, 2*M_PI);
}

/** \} */
/** \} */

