#include "deconvolve.hpp"
#include <limits>
#include <iostream>
#include <chrono>
#include <random>

#include "quadratic-min.hpp"
#include "util.hpp"
#include "regularizer.hpp"
#include "array-util.hpp"


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#include "optimization.h"
#pragma clang diagnostic pop

namespace deconvolution{

using namespace alglib;

template <int D>
boost::general_storage_order<D+1> lambdaOrder(int majorDim) {
    std::array<size_t, D+1> order;
    std::array<bool, D+1> ascending;
    for (int i = 0; i < D+1; ++i) {
        order[i] = i;
        ascending[i] = true;
    }
    std::swap(order[majorDim], order[D-1]);
    return boost::general_storage_order<D+1>(order.begin(), ascending.begin());
}

template <int D, typename Shape>
std::vector<Array<D+1>> allocLambda(const Shape& shape) {
    std::vector<Array<D+1>> lambda;
    for (int i = 0; i < D; ++i)
        lambda.emplace_back(shape, lambdaOrder<D>(i));
    return lambda;
}

template <int D>
void addUnaries(const Regularizer<D>& R, const Array<D>& nu, Array<D+1>& result);


template <int D>
Array<D> DeconvolveConvexBP(
        const Array<D>& y,
        const LinearSystem<D>& H,
        const LinearSystem<D>& Ht,
        Regularizer<D>& R,
        ProgressCallback<D>& pc,
        DeconvolveParams& params,
        DeconvolveStats& s) {
    Array<D> b = 2*Ht(y);
    const auto shape = arrayExtents(b);
    Array<D> x(shape);
    LinearSystem<D> Q = [&](const Array<D>& x) -> Array<D> { 
        return Ht(H(x)) + params.dataSmoothing*x; 
    };
    //double constantTerm = dot(y, y);

    int numPrimalVars = x.num_elements();
    const auto lambdaShape = arrayExtents(x)[R.numLabels()];
    auto lambda = allocLambda<D>(lambdaShape);
    int numLambda = numPrimalVars*R.numSubproblems()*R.numLabels();
    int numDualVars = numLambda + numPrimalVars; // Including all lambda vars + vector nu
    auto dualVars = std::vector<double>(numDualVars, 0.0);
    auto primalMu_i = std::vector<double>(numPrimalVars*R.numLabels(), 0.0);

    auto modifiedUnaries = Array<D+1>{lambdaShape};
    auto nu = Array<D>{shape};

    // Initialize x to 0 to find the least norm solution to Qx = b
    for (size_t i = 0; i < x.num_elements(); ++i) {
        x.data()[i] = 0;
    }
    std::cout << "Finding least-squares fit\n";
    quadraticMinCG<D>(Q, b, x);

    bool converged = false;
    while (!converged) {

       // Compute modifiedUnaries = unaries + sum of lambda - lambda_alpha
       addUnaries(R, nu, modifiedUnaries);
       for (int i = 0; i < D; ++i)
           plusEquals(modifiedUnaries, lambda[i]);
       
       // run min-marginals with modifiedUnaries
       // compute lambda as result of min-marginal - modified unary
       // update modifiedUnaries
       //
       // run steps of gradient descent on data-term + soft-min of modified unaries

    }

    return x;
}

#define INSTANTIATE_DECONVOLVE(d)                                              \
    template Array<d> DeconvolveConvexBP<d>(const Array<d>& y,                 \
		const LinearSystem<d>& H,                                              \
		const LinearSystem<d>& Q,                                              \
		Regularizer<d>& R,                                                     \
		ProgressCallback<d>& pc,                                               \
		DeconvolveParams& params,                                              \
		DeconvolveStats& s);

INSTANTIATE_DECONVOLVE(1)
INSTANTIATE_DECONVOLVE(2)
INSTANTIATE_DECONVOLVE(3)
#undef INSTANTIATE_DECONVOLVE
}
