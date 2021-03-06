#pragma once
// Class for difference calculation of NNUE evaluation function

#include "architecture.h"

namespace Evaluator::NNUE {

    // Class that holds the result of affine transformation of input features
    struct alignas(CacheLineSize) Accumulator {

        int16_t accumulation[2][RefreshTriggers.size()][TransformedFeatureDimensions];
        bool accumulationComputed;
    };

}
