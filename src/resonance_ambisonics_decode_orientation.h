#pragma once

#include <phonon.h>

namespace resonance {

/// Fills out_orientation for IPLAmbisonicsDecodeEffectParams from two row-major 4×4 matrices:
/// the HOA bed's local→world transform and the listener's world→listener rotation (plus translation row).
/// Forward and up basis vectors are taken from the standard layout (columns in rows 8–10 and 4–6 of the bed matrix).
void ambisonics_decode_orientation_row_major(const float source_row_major4[16], const float listener_row_major4[16],
                                             IPLCoordinateSpace3* out_orientation);

} // namespace resonance
