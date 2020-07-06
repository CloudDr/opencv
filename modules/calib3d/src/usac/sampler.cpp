// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "../precomp.hpp"
#include "../usac.hpp"

namespace cv { namespace usac {
/*
* Uniform Sampler:
* Choose uniformly m (sample size) points from N (points size).
* Uses Fisher-Yates shuffle.
*/
class UniformSamplerImpl : public UniformSampler {
private:
    std::vector<int> points_random_pool;
    int sample_size, random_pool_size, points_size = 0;
    RNG rng;
public:

    UniformSamplerImpl (int state, int sample_size_, int points_size_) : rng(state) {
        sample_size = sample_size_;
        setPointsSize (points_size_);
    }
    void setNewPointsSize (int points_size_) override {
        setPointsSize(points_size_);
    }
    void generateSample (std::vector<int> &sample) override {
        random_pool_size = points_size; // random points of entire range
        for (int i = 0; i < sample_size; i++) {
            // get random point index
            const int array_random_index = rng.uniform(0, random_pool_size);
            // get point by random index
            // store sample
            sample[i] = points_random_pool[array_random_index];
            // swap random point with the end of random pool
            std::swap(points_random_pool[array_random_index],
                      points_random_pool[--random_pool_size]);
        }
    }
    /*
     * For different points size is better to not use array random generator
     * to avoid filling array with points.
     */
    void generateSample (std::vector<int> &sample, int points_size_) override {
        assert(sample_size <= points_size_);

        int num, j;
        sample[0] = rng.uniform(0, points_size_);
        for (int i = 1; i < sample_size;) {
            num = rng.uniform(0, points_size_);
            for (j = i - 1; j >= 0; j--)
                if (num == sample[j])
                    break;
            if (j == -1) sample[i++] = num;
        }
    }
    int getSampleSize () const override { return sample_size; }
    Ptr<Sampler> clone () const override {
        return makePtr<UniformSamplerImpl>(abs((int)rng.state)/10+10, sample_size, points_size);
    }
private:
    void setPointsSize (int points_size_) {
        assert (sample_size <= points_size_);

        if (points_size_ > points_size)
            points_random_pool = std::vector<int>(points_size_);

        if (points_size != points_size_) {
            points_size  = points_size_;

            for (int i = 0; i < points_size; i++)
                points_random_pool[i] = i;
        }
    }
};

Ptr<UniformSampler> UniformSampler::create(int state, int sample_size_, int points_size_) {
    return Ptr<UniformSamplerImpl>(new UniformSamplerImpl(state, sample_size_, points_size_));
}
}}
