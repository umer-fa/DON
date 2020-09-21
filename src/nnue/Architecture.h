// Input features and network structure used in NNUE evaluation function
#pragma once

#include "../Type.h"
// Defines the network structure
#include "architectures/HalfKP_256x2-32-32.h"

namespace Evaluator::NNUE {

    static_assert (TransformedFeatureDimensions % MaxSimdWidth == 0, "");
    static_assert (Network::OutputDimensions == 1, "");
    static_assert (std::is_same<Network::OutputType, i32>::value, "");

    // Trigger for full calculation instead of difference calculation
    constexpr auto RefreshTriggers{ RawFeatures::RefreshTriggers };

}  // namespace Evaluator::NNUE