.. SPDX-License-Identifier: GPL-2.0

=========================================
Microchip Image Sensor Controller (ISC)
=========================================

The Microchip ISC driver (``microchip-isc``) supports two SoC variants:

- **SAMA5D2** — ``atmel-isc`` (non-bipartite gamma mode)
- **SAMA7G5** — ``microchip-xisc`` (bipartite gamma mode)

Controls
========

White Balance
-------------

The ISC exposes an auto-cluster of controls for white balance.  When
``V4L2_CID_AUTO_WHITE_BALANCE`` is enabled the driver computes gains and
offsets from a built-in histogram engine and updates the following controls
automatically (they remain readable as volatile):

``ISC_CID_R_GAIN``, ``ISC_CID_B_GAIN``, ``ISC_CID_GR_GAIN``, ``ISC_CID_GB_GAIN``
  Per-BAYER-channel multipliers, unsigned 0:4:9 fixed-point (default 512 = 1.0×).

``ISC_CID_R_OFFSET``, ``ISC_CID_B_OFFSET``, ``ISC_CID_GR_OFFSET``, ``ISC_CID_GB_OFFSET``
  Per-BAYER-channel signed offsets, format 1:12:0 (default 0).

When ``V4L2_CID_AUTO_WHITE_BALANCE`` is disabled (manual mode), calling
``V4L2_CID_DO_WHITE_BALANCE`` performs a one-shot adjustment and makes the
resulting coefficients available for userspace to save and restore.

Per-channel Gamma LUT Override
-------------------------------

``ISC_CID_GAMMA_B_LUT``, ``ISC_CID_GAMMA_G_LUT``, ``ISC_CID_GAMMA_R_LUT``
  Type: ``V4L2_CTRL_TYPE_U32``, 64 elements, range 0–1023.

  These controls allow userspace (e.g. a libcamera IPA) to supply a fully
  custom tone curve for each output channel, computed from real sensor
  histogram data.  Each element ``i`` (0–63) specifies the desired 10-bit
  output value for sensor input values in the range ``[i*16 .. (i+1)*16-1]``.

  The driver converts the simple linear array to the packed hardware
  piecewise-linear register format:

  - **SAMA7G5** (bipartite mode): ``hw[i] = (lut[i] << 16) | ((lut[i+1] - lut[i]) * 32)``
    where the delta is a Q9 per-step increment (× 512 / 16 input steps).
  - **SAMA5D2** (non-bipartite mode): ``hw[i] = (lut[i] << 16) | (lut[i+1] - lut[i])``
    where the delta is a plain per-segment increment.

  Setting any of the three controls activates the custom LUT path for all
  three channels and takes precedence over ``V4L2_CID_GAMMA``.

  To revert to the preset curve, write ``V4L2_CID_GAMMA`` with the desired
  preset index.  This clears the LUT override flag.

Example (libcamera IPA pseudo-code)::

    /* Compute a curve from histogram statistics */
    std::array<uint32_t, 64> lut;
    for (int i = 0; i < 64; i++)
        lut[i] = tone_map(i * 16, histogram);

    v4l2_ext_control ctrl{};
    ctrl.id   = ISC_CID_GAMMA_R_LUT;
    ctrl.size = 64 * sizeof(uint32_t);
    ctrl.p_u32 = lut.data();
    /* repeat for ISC_CID_GAMMA_G_LUT and ISC_CID_GAMMA_B_LUT */
    ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
