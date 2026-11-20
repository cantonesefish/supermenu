#pragma once

// The small subset of Atrium's shell_icon that the tray host needs: HICON
// conversion, square scaling, and executable-path normalisation.
#include "icon_image.h"

#include <windows.h>

#include <string>

namespace atrium {

bool HIconToImage(HICON icon, IconImage& out);
IconImage FitToSquare(const IconImage& src, int size);
std::wstring AppExecutable(const std::wstring& path);

}  // namespace atrium
