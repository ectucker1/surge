/*
 * Surge XT - a free and open source hybrid synthesizer,
 * built by Surge Synth Team
 *
 * Learn more at https://surge-synthesizer.github.io/
 *
 * Copyright 2018-2026, various authors, as described in the GitHub
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
 * WASM implementations of the sst-plugininfra platform pieces whose Linux
 * versions don't compile under Emscripten: execinfo.h backtraces in
 * misc_linux.cpp, and cpuid plus MXCSR access in cpufeatures.cpp.
 */

#include <sst/plugininfra/misc_platform.h>
#include <sst/plugininfra/cpufeatures.h>

#include <cerrno>
#include <cstring>

namespace sst::plugininfra::misc_platform
{
bool isDarkMode() { return true; }

void allocateConsole() {}

std::string toOSCase(const std::string &text) { return text; }

std::string stackTraceToString(int depth) { return "(stack traces unavailable on WASM)"; }

std::string getLastSystemError() { return std::strerror(errno); }
} // namespace sst::plugininfra::misc_platform

namespace sst::plugininfra::cpufeatures
{
std::string brand() { return "WebAssembly"; }

bool isArm() { return false; }
bool isX86() { return false; }
bool hasSSE2() { return false; }
bool hasAVX() { return false; }

// WASM has no FTZ/DAZ or rounding mode state to guard
FPUStateGuard::FPUStateGuard() { priorS = 0; }
FPUStateGuard::~FPUStateGuard() = default;
} // namespace sst::plugininfra::cpufeatures
