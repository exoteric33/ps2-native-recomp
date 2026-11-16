#pragma once

namespace ps2 {
// Discard cached window-to-native-code matches after the guest invalidates its
// instruction cache. Retain compiled libraries; their checks select the new image.
void invalidate_window_instances();
}
