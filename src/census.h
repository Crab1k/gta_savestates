// Lab diagnostic: log every pointer from the savestate image to memory outside it (other modules, the process
// heap, D3D / DirectSound objects), and the GPU memory a savestate file would carry.
#pragma once

namespace census {
void Run(const char* tag);
}
