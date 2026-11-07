// Lab diagnostic: log every pointer from the savestate image to memory outside it (other modules, the process heap,
// D3D / Miles objects, exported functions).
#pragma once

namespace census {
void Run(const char* tag);
}
