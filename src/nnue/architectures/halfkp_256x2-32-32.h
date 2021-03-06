// Definition of input features and network structure used in NNUE evaluation function
#pragma once

#include "../features/feature_set.h"
#include "../features/half_kp.h"

#include "../layers/input_slice.h"
#include "../layers/affine_transform.h"
#include "../layers/clipped_relu.h"

namespace Evaluator::NNUE {

    // Input features used in evaluation function
    using RawFeatures = Features::FeatureSet<Features::HalfKP<Features::Side::FRIEND>>;

    // Number of input feature dimensions after conversion
    constexpr IndexType TransformedFeatureDimensions{ 256 };

    namespace Layers {

        // Define network structure
        using InputLayer = InputSlice<TransformedFeatureDimensions * 2>;
        using HiddenLayer1 = ClippedReLU<AffineTransform<InputLayer, 32>>;
        using HiddenLayer2 = ClippedReLU<AffineTransform<HiddenLayer1, 32>>;
        using OutputLayer = AffineTransform<HiddenLayer2, 1>;

    }

    using Network = Layers::OutputLayer;

}
