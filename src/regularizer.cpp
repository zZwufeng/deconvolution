#include "regularizer.hpp"
#include <iostream>

namespace deconvolution {

void incrementBase(const std::vector<int>& extents, int subproblem, std::vector<int>& base) {
    for (int pos = 0; pos < int(extents.size()); ++pos) {
        if (pos == subproblem)
            continue;
        base[pos]++;
        if (base[pos] < extents[pos])
            return;
        else
            base[pos] = 0;
    }
}

template <int D>
double GridRegularizer<D>::evaluate(int subproblem, const double* lambda_a, double smoothing, double lambdaScale, double* gradient) const {
    assert(subproblem >= 0 && subproblem < D);
    double objective = 0;

    std::vector<int> lambdaExtents = _extents;
    lambdaExtents.push_back(numLabels());
    assert(lambdaExtents.size() == D+1);
    auto L = boost::const_multi_array_ref<double, D+1>{lambda_a, lambdaExtents};
    auto G = boost::multi_array_ref<double, D+1>{gradient, lambdaExtents};

    int width = _extents[subproblem]; // Length along this dimension of the grid
    std::vector<int> base(D, 0);
    int numBases = 1;
    for (int i = 0; i < D; ++i)
        numBases *= (i == subproblem) ? 1 : _extents[i];

    std::vector<double> lambdaSlice(_numLabels*width, 0);
    std::vector<double> m_L(_numLabels*width, 0);
    std::vector<double> m_R(_numLabels*width, 0);
    std::vector<double> logMarg(_numLabels, 0);
    std::vector<double> labelCosts(_numLabels, 0);
    const double smoothingMult = 1.0/smoothing;
    for (int countBase = 0; countBase < numBases; ++countBase, incrementBase(_extents, subproblem, base)) {
        int baseIdx = 0;
        for (int i = 0; i < D; ++i) baseIdx += base[i]*L.strides()[i];
        int stride = L.strides()[subproblem]/_numLabels;
        assert(L.strides()[D] == 1);
        assert(stride*_numLabels == L.strides()[subproblem]);
        /*
        std::cout << "\tbase: ";
        for (auto idx : base) std::cout << idx << " ";
        std::cout << "\n";
        */

        for (int j = 0; j < width; ++j)
            for (int l = 0; l < _numLabels; ++l)
                lambdaSlice[j*_numLabels + l] = L.data()[baseIdx + j*stride*_numLabels + l];

        // Compute log m_L
        // Base step
        for (int l = 0; l < _numLabels; ++l)
            m_L[l] = lambdaScale*smoothingMult*lambdaSlice[l];
        // Inductive step
        for (int j = 1; j < width; ++j) {
            int idx = baseIdx/_numLabels+j*stride;
            for (int lCurr = 0; lCurr < _numLabels; ++lCurr) {
                double maxMessage = std::numeric_limits<double>::lowest();
                double domainLCurr = _getLabel(idx, lCurr);
                for (int lPrev = 0; lPrev < _numLabels; ++lPrev) {
                    double domainLPrev = _getLabel(idx-stride, lPrev);
                    labelCosts[lPrev] = -_edgeFn(domainLPrev, domainLCurr)*smoothingMult + m_L[(j-1)*_numLabels+lPrev];
                    maxMessage = std::max(maxMessage, labelCosts[lPrev]);
                }
                double sumExp = 0;
                for (int lPrev = 0; lPrev < _numLabels; ++lPrev)
                    sumExp += exp(labelCosts[lPrev] - maxMessage);
                m_L[j*_numLabels+lCurr] = lambdaScale*lambdaSlice[j*_numLabels+lCurr]*smoothingMult + maxMessage + log(sumExp);
            }
        }

        // Compute log m_R
        for (int lCurr = 0; lCurr < _numLabels; ++lCurr)
            m_R[(width-1)*_numLabels+lCurr] = 0.0;
        for (int j = width-2; j >= 0; --j) {
            int idx = baseIdx/_numLabels+j*stride;
            for (int lCurr = 0; lCurr < _numLabels; ++lCurr) {
                double maxMessage = std::numeric_limits<double>::lowest();
                double domainLCurr = _getLabel(idx, lCurr);
                for (int lPrev = 0; lPrev < _numLabels; ++lPrev) {
                    double domainLPrev = _getLabel(idx+stride, lPrev);
                    labelCosts[lPrev] = -(_edgeFn(domainLCurr, domainLPrev) - lambdaScale*lambdaSlice[(j+1)*_numLabels+lPrev])*smoothingMult + m_R[(j+1)*_numLabels+lPrev];
                    maxMessage = std::max(maxMessage, labelCosts[lPrev]);
                }
                double sumExp = 0;
                for (int lPrev = 0; lPrev < _numLabels; ++lPrev)
                    sumExp += exp(labelCosts[lPrev] - maxMessage);
                m_R[j*_numLabels+lCurr] = maxMessage + log(sumExp);
            }
        }

        // Compute marginals, put them in G
        double logSumExp = 0;
        for (int j = 0; j < width; ++j) {
            double maxMarg = std::numeric_limits<double>::lowest();
            for (int l = 0; l < _numLabels; ++l) {
                logMarg[l] = m_L[j*_numLabels+l] + m_R[j*_numLabels+l];
                maxMarg = std::max(maxMarg, logMarg[l]);
            }
            double sumExp = 0;
            for (int l = 0; l < _numLabels; ++l)
                sumExp += exp(logMarg[l] - maxMarg);
            logSumExp = maxMarg + log(sumExp);
            for (int l = 0; l < _numLabels; ++l)
                G.data()[baseIdx + j*stride*_numLabels + l] = -lambdaScale*exp(logMarg[l] - logSumExp);
        }
        objective += -smoothing*logSumExp;
    }
    return objective;
}

template <int D>
double GridRegularizer<D>::primal(const double* x) const {
    double objective = 0;
    const auto X = boost::const_multi_array_ref<double, D>{x, _extents};

    for (int subproblem = 0; subproblem < D; ++subproblem) {
        int width = _extents[subproblem]; // Length along this dimension of the grid
        std::vector<int> base(D, 0);
        int numBases = 1;
        for (int i = 0; i < D; ++i)
            numBases *= (i == subproblem) ? 1 : _extents[i];

        for (int countBase = 0; countBase < numBases; ++countBase, incrementBase(_extents, subproblem, base)) {
            int baseIdx = 0;
            for (int i = 0; i < D; ++i) baseIdx += base[i]*X.strides()[i];
            int stride = X.strides()[subproblem];
            for (int i = 0; i < width-1; ++i) {
                objective += _edgeFn(x[baseIdx+i*stride], x[baseIdx+(i+1)*stride]);
            }
        }
    }
    return objective;
}

template <int D>
void GridRegularizer<D>::sampleLabels(const Array<D>& x, double scale) {
    for (int i = 0; i < D; ++i)
        assert(static_cast<int>(x.shape()[i]) == _extents[i]);
    int n = std::accumulate(_extents.begin(), _extents.end(), 1, [](int i, int j) { return i*j; });
    for (int i = 0; i < n; ++i) {
        double val = x.data()[i];
        for (int l = 0; l < _numLabels; ++l) {
            _labels[i*_numLabels+l] = val+scale*(l-(_numLabels-2)/2);
            _labels[i*_numLabels+l] = std::min(_labels[i*_numLabels+l], _numLabels*_labelScale);
            _labels[i*_numLabels+l] = std::max(_labels[i*_numLabels+l], 0.0);
        }
    }
}

#define INSTANTIATE_DECONVOLVE_REGULARIZER(d) \
    template class GridRegularizer<d>;
INSTANTIATE_DECONVOLVE_REGULARIZER(1)
INSTANTIATE_DECONVOLVE_REGULARIZER(2)
INSTANTIATE_DECONVOLVE_REGULARIZER(3)
#undef INSTANTIATE_DECONVOLVE_REGULARIZER

}
