/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2025, various authors, as described in the GitHub
 * transaction log.
 *
 * Surge XT is released under the GNU General Public Licence v3
 * or later (GPL-3.0-or-later). The license is found in the "LICENSE"
 * file in the root of this repository, or at
 * https://www.gnu.org/licenses/gpl-3.0.en.html
 *
 * Surge was a commercial product from 2004-2018, copyright and ownership
 * held by Claes Johanson at Vember Audio during that period.
 * Claes made Surge open source in September 2018.
 *
 * All source for Surge XT is available at
 * https://github.com/surge-synthesizer/surge
 */

/*
 * A stand-in for the x86 SSE1 header, for C code that includes it directly.
 *
 * Emscripten ships real x86 intrinsic headers implemented on WASM SIMD128;
 * plain clang for wasm ships none. pffft only knows how to vectorize through
 * <xmmintrin.h> (it is told it is on i386 for that purpose), so give it one:
 * simde's SSE1, with the native names switched on, which lowers onto WASM
 * SIMD128 when -msimd128 is set. This directory is only put on pffft's include
 * path, so nothing else picks it up.
 */

#ifndef SURGE_WASM_SIMDE_XMMINTRIN_H
#define SURGE_WASM_SIMDE_XMMINTRIN_H

#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse.h"

#endif // SURGE_WASM_SIMDE_XMMINTRIN_H
