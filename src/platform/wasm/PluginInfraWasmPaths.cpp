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
 * The sst-plugininfra path lookups for a wasi-targeted WASM module.
 *
 * The Linux implementation asks dladdr() where the shared library lives and
 * the environment where home is; a wasi module has neither. The Emscripten
 * build keeps the Linux code because Emscripten emulates both, so this file is
 * only used with the clang/wasi toolchain. The answers mirror what Emscripten
 * would give (a home of /home/web_user), so that the engine's data and user
 * paths come out the same either way. Nothing here has to exist: the host
 * decides what, if anything, backs the filesystem.
 */

#include "filesystem/import.h"
#include "sst/plugininfra/paths.h"

namespace sst::plugininfra::paths
{
fs::path homePath() { return fs::path{"/home/web_user"}; }

fs::path sharedLibraryBinaryPath() { return fs::path{"/surge-xt.clap.wasm"}; }

fs::path bestDocumentsVendorFolderPathFor(const std::string &vendorName,
                                          const std::string &productName)
{
    auto documents = homePath() / "Documents";
    if (vendorName.empty())
        return documents / productName;
    return documents / vendorName / productName;
}

fs::path bestLibrarySharedVendorFolderPathFor(const std::string &vendorName,
                                              const std::string &productName, bool userLevel)
{
    auto base = userLevel ? homePath() / ".local" / "share" : fs::path{"/usr"} / "share";
    if (vendorName.empty())
        return base / productName;
    return base / vendorName / productName;
}
} // namespace sst::plugininfra::paths
