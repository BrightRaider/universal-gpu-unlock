/* SPDX-License-Identifier: GPL-2.0-only */
#pragma once

#define GU_ADA_DEVICE_IDS(X) \
  X(0x2684U) /* RTX 4090 */ \
  X(0x2685U) /* RTX 4090D */ \
  X(0x2702U) \
  X(0x2704U) /* RTX 4080 */ \
  X(0x2703U) /* RTX 4080 SUPER */ \
  X(0x2705U) /* RTX 4070 Ti SUPER */ \
  X(0x2782U) /* RTX 4070 Ti (AD104) */ \
  X(0x2783U) /* RTX 4070 SUPER (AD104) */ \
  X(0x2786U) /* RTX 4070 (AD104) */ \
  X(0x2860U) /* RTX 4060 Ti (AD106) */ \
  X(0x2882U) /* RTX 4060 (AD107) */

#define GU_AMPERE_DEVICE_IDS(X) \
  X(0x2203U) /* RTX 3090 Ti */ \
  X(0x2204U) /* RTX 3090 */ \
  X(0x2205U) /* RTX 3080 Ti OEM */ \
  X(0x2206U) /* RTX 3080 (10GB) */ \
  X(0x2208U) /* RTX 3080 Ti */ \
  X(0x220aU) /* RTX 3080 (10GB) */ \
  X(0x220bU) /* RTX 3080 */ \
  X(0x2216U) /* RTX 3080 12GB */ \
  X(0x2414U) /* RTX 3080 (GA103) */ \
  X(0x2207U) /* CMP 90HX */ \
  X(0x2482U) /* RTX 3070 Ti (GA104) */ \
  X(0x2484U) /* RTX 3070 (GA104) */ \
  X(0x2486U) /* RTX 3060 Ti (GA104) */ \
  X(0x2487U) X(0x2488U) X(0x2489U) /* RTX 3060 Ti (GA104) */ \
  X(0x2501U) /* RTX 3050 OEM (GA106) */ \
  X(0x2503U) X(0x2504U) X(0x2507U) X(0x2508U) /* RTX 3060 (GA106) */ \
  X(0x2582U) /* RTX 3050 8GB (GA106) */ \
  X(0x2520U) X(0x2560U)

#define GU_TURING_DEVICE_IDS(X) \
  X(0x1e04U) X(0x1e07U) X(0x1e02U) /* RTX 2080 Ti, TITAN RTX (TU102) */ \
  X(0x1e09U)                       /* CMP 50HX */ \
  X(0x1e30U) X(0x1e35U)             /* Quadro RTX 6000, 8000 */ \
  X(0x1e81U) X(0x1e82U) X(0x1e87U) /* RTX 2080 SUPER, RTX 2080 (TU104) */ \
  X(0x1e84U) X(0x1eb0U) X(0x1eb1U) /* RTX 2070 SUPER, Quadro RTX 5000/4000 */ \
  X(0x1f02U) X(0x1f07U) X(0x1f06U) /* RTX 2070, 2060 SUPER (TU106) */ \
  X(0x1f47U) X(0x1f03U) X(0x1f08U) X(0x1f12U) X(0x1f36U)
