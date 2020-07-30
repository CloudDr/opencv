// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "../precomp.hpp"
#include "../usac.hpp"
#include <atomic>

namespace cv { namespace usac {
int mergePoints (InputArray pts1_, InputArray pts2_, Mat &pts, bool ispnp);
void saveMask (OutputArray mask, const std::vector<bool> &inliers_mask);

class RansacOutputImpl : public RansacOutput {
private:
    Mat model;
    // vector of number_inliers size
    std::vector<int> inliers;
    // vector of points size, true if inlier, false-outlier
    std::vector<bool> inliers_mask;
    // vector of points size, value of i-th index corresponds to error of i-th point if i is inlier.
    std::vector<double> errors;

    // the best found score of RANSAC
    double score;

    int seconds, milliseconds, microseconds;
    int time_mcs, number_inliers;
    int number_iterations; // number of iterations of main RANSAC
    int number_estimated_models, number_good_models;
public:
    RansacOutputImpl (const Mat &model_,
        const std::vector<bool> &inliers_mask_,
        int time_mcs_, double score_,
        int number_inliers_, int number_iterations_,
        int number_estimated_models_,
        int number_good_models_) {

        model = model_.clone();
        inliers_mask = inliers_mask_;

        time_mcs = time_mcs_;

        score = score_;
        number_inliers = number_inliers_;
        number_iterations = number_iterations_;
        number_estimated_models = number_estimated_models_;
        number_good_models = number_good_models_;

        microseconds = time_mcs % 1000;
        milliseconds = ((time_mcs - microseconds)/1000) % 1000;
        seconds = ((time_mcs - 1000*milliseconds - microseconds)/(1000*1000)) % 60;
    }

    /*
     * Return inliers' indices.
     * size of vector = number of inliers
     */
    const std::vector<int > &getInliers() override {
        if (inliers.empty()) {
            inliers.reserve(inliers_mask.size());
            int pt_cnt = 0;
            for (bool is_inlier : inliers_mask) {
                if (is_inlier)
                    inliers.emplace_back(pt_cnt);
                pt_cnt++;
            }
        }
        return inliers;
    }

    // Return inliers mask. Vector of points size. 1-inlier, 0-outlier.
    const std::vector<bool> &getInliersMask() const override { return inliers_mask; }

    int getTimeMicroSeconds() const override {return time_mcs; }
    int getTimeMicroSeconds1() const override {return microseconds; }
    int getTimeMilliSeconds2() const override {return milliseconds; }
    int getTimeSeconds3() const override {return seconds; }
    int getNumberOfInliers() const override { return number_inliers; }
    int getNumberOfMainIterations() const override { return number_iterations; }
    int getNumberOfGoodModels () const override { return number_good_models; }
    int getNumberOfEstimatedModels () const override { return number_estimated_models; }
    const Mat &getModel() const override { return model; }
    Ptr<RansacOutput> clone () const override { return makePtr<RansacOutputImpl>(*this); }
};

Ptr<RansacOutput> RansacOutput::create(const Mat &model_,
            const std::vector<bool> &inliers_mask_, int time_mcs_, double score_,
           int number_inliers_, int number_iterations_,
           int number_estimated_models_, int number_good_models_) {
    return makePtr<RansacOutputImpl>(model_, inliers_mask_, time_mcs_,
            score_, number_inliers_, number_iterations_,
            number_estimated_models_, number_good_models_);
}

class Ransac {
protected:
    const Ptr<const Model> params;
    const Ptr<const Estimator> _estimator;
    const Ptr<Quality> _quality;
    const Ptr<Sampler> _sampler;
    const Ptr<TerminationCriteria> _termination_criteria;
    const Ptr<ModelVerifier> _model_verifier;
    const Ptr<Degeneracy> _degeneracy;
    const Ptr<LocalOptimization> _local_optimization;
    const Ptr<FinalModelPolisher> model_polisher;

    const int points_size, state;
    const bool parallel;
public:

    Ransac (const Ptr<const Model> &params_, int points_size_, const Ptr<const Estimator> &estimator_, const Ptr<Quality> &quality_,
            const Ptr<Sampler> &sampler_, const Ptr<TerminationCriteria> &termination_criteria_,
            const Ptr<ModelVerifier> &model_verifier_, const Ptr<Degeneracy> &degeneracy_,
            const Ptr<LocalOptimization> &local_optimization_, const Ptr<FinalModelPolisher> &model_polisher_,
            bool parallel_=false, int state_ = 0) :

            params (params_), _estimator (estimator_), _quality (quality_), _sampler (sampler_),
            _termination_criteria (termination_criteria_), _model_verifier (model_verifier_),
            _degeneracy (degeneracy_), _local_optimization (local_optimization_),
            model_polisher (model_polisher_), points_size (points_size_), state(state_),
            parallel(parallel_) {}

    bool run(Ptr<RansacOutput> &ransac_output) {
        if (points_size < params->getSampleSize())
            return false;

        const auto begin_time = std::chrono::steady_clock::now();

        // check if LO
        const bool LO = params->getLO() != LocalOptimMethod::NullLO;
        const bool is_magsac = params->getLO() == LocalOptimMethod::SIGMA;

        Score best_score;
        Mat best_model;
        int final_iters;

        if (! parallel) {
            Mat non_degenerate_model;
            Score current_score, lo_score, non_denegenerate_model_score;

            // reallocate memory for models
            // do not use loop over models (i.e., auto &m : models)!
            std::vector<Mat> models(_estimator->getMaxNumSolutions());

            // allocate memory for sample
            std::vector<int> sample(_estimator->getMinimalSampleSize());
            int iters = 0, max_iters = params->getMaxIters();
            for (; iters < max_iters; iters++) {
                _sampler->generateSample(sample);
                const int number_of_models = _estimator->estimateModels(sample, models);

                for (int i = 0; i < number_of_models; i++) {
                    if (!_model_verifier->isModelGood(models[i]))
                        continue;

                    if (is_magsac) {
                        if (best_model.empty())
                            models[i].copyTo(best_model);
                        _local_optimization->refineModel
                                      (best_model, best_score, models[i], current_score);
                    } else if (!_model_verifier->getScore(current_score))
                        current_score = _quality->getScore(models[i]);

                    if (current_score.isBetter(best_score)) {
                        // if number of non degenerate models is zero then input model is good
                        if (_degeneracy->recoverIfDegenerate(sample, models[i],
                                non_degenerate_model, non_denegenerate_model_score)) {
                            // check if best non degenerate model is better than so far the best model
                            if (non_denegenerate_model_score.isBetter(best_score)) {
                                best_score = non_denegenerate_model_score;
                                non_degenerate_model.copyTo(best_model);
                            } else
                                // non degenerate models are worse then so far the best model.
                                continue;
                        } else {
                            // copy current score to best score
                            best_score = current_score;
                            // remember best model
                            models[i].copyTo(best_model);
                        }

                        // update quality to save evaluation time of a model
                        // with no chance of being better than so-far-the-best
                        _quality->setBestScore(best_score.score);

                        // update upper bound of iterations
                        max_iters = _termination_criteria->update
                                (best_model, best_score.inlier_number);
                        if (iters > max_iters)
                            break;

                        if (LO && !is_magsac) {
                            // update model by Local optimization
                            Mat lo_model;
                            if (_local_optimization->refineModel
                                            (best_model, best_score, lo_model, lo_score))
                                if (lo_score.isBetter(best_score)) {
                                    best_score = lo_score;
                                    lo_model.copyTo(best_model);
                                    // update quality and verifier and termination again
                                    _quality->setBestScore(best_score.score);
                                    _model_verifier->update(best_score.inlier_number);
                                    max_iters = _termination_criteria->update
                                            (best_model, best_score.inlier_number);
                                    if (iters > max_iters)
                                        break;
                                }
                        }
                    } // end of if so far the best score
                } // end loop of number of models
            } // end main while loop

            final_iters = iters;
        } else {
            const int MAX_THREADS = getNumThreads();
            const bool is_prosac = params->getSampler() == SamplingMethod::Prosac;

            std::atomic_bool success(false);
            std::atomic_int num_hypothesis_tested(0);
            std::atomic_int thread_cnt(0);
            std::vector<Score> best_scores(MAX_THREADS);
            std::vector<Mat> best_models(MAX_THREADS);

            Mutex mutex; // only for prosac

            ///////////////////////////////////////////////////////////////////////////////////////////////////////
            parallel_for_(Range(0, MAX_THREADS), [&](const Range & /*range*/) {
            if (!success) { // cover all if not success to avoid thread creating new variables
                const int thread_rng_id = thread_cnt++;
                int thread_state = state + 10*thread_rng_id;

                Ptr<Estimator> estimator = _estimator->clone();
                Ptr<Degeneracy> degeneracy = _degeneracy->clone(thread_state++);
                Ptr<Quality> quality = _quality->clone();
                Ptr<ModelVerifier> model_verifier = _model_verifier->clone(thread_state++); // update verifier
                Ptr<LocalOptimization> local_optimization = _local_optimization->clone(thread_state++);
                Ptr<TerminationCriteria> termination_criteria = _termination_criteria->clone();
                Ptr<Sampler> sampler;
                if (!is_prosac)
                   sampler = _sampler->clone(thread_state);

                Mat best_model_thread, non_degenerate_model, lo_model;
                Score best_score_thread, current_score, non_denegenerate_model_score, lo_score,
                      best_score_all_threads;
                std::vector<int> sample(estimator->getMinimalSampleSize());
                std::vector<Mat> models(estimator->getMaxNumSolutions());
                int iters, max_iters = params->getMaxIters();
                auto update_best = [&] (const Score &new_score, const Mat &new_model) {
                    // copy new score to best score
                    best_score_thread = new_score;
                    best_scores[thread_rng_id] = best_score_thread;
                    // remember best model
                    new_model.copyTo(best_model_thread);
                    best_model_thread.copyTo(best_models[thread_rng_id]);
                    best_score_all_threads = best_score_thread;
                };

                for (iters = 0; iters < max_iters && !success; iters++) {
                    success = num_hypothesis_tested++ > max_iters;

                    if (iters % 10) {
                        // Synchronize threads. just to speed verification of model.
                        int best_thread_idx = thread_rng_id;
                        bool updated = false;
                        for (int t = 0; t < MAX_THREADS; t++) {
                            if (best_scores[t].isBetter(best_score_all_threads)) {
                                best_score_all_threads = best_scores[t];
                                updated = true;
                                best_thread_idx = t;
                            }
                        }
                        if (updated && best_thread_idx != thread_rng_id) {
                            quality->setBestScore(best_score_all_threads.score);
                            model_verifier->update(best_score_all_threads.inlier_number);
                        }
                    }

                    if (is_prosac) {
                        // use global sampler
                        mutex.lock();
                        _sampler->generateSample(sample);
                        mutex.unlock();
                    } else sampler->generateSample(sample); // use local sampler

                    const int number_of_models = estimator->estimateModels(sample, models);
                    for (int i = 0; i < number_of_models; i++) {
                        if (!model_verifier->isModelGood(models[i]))
                           continue;

                        if (is_magsac) {
                            if (best_model_thread.empty())
                                models[i].copyTo(best_model_thread);
                            local_optimization->refineModel(best_model_thread, best_score_thread,
                                    models[i], current_score);
                        } else if (!model_verifier->getScore(current_score))
                            current_score = quality->getScore(models[i]);

                        if (current_score.isBetter(best_score_all_threads)) {
                            // if number of non degenerate models is zero then input model is good
                            if (degeneracy->recoverIfDegenerate(sample, models[i],
                                        non_degenerate_model, non_denegenerate_model_score)) {
                                // check if best non degenerate model is better than so far the best model
                                if (non_denegenerate_model_score.isBetter(best_score_thread))
                                    update_best(non_denegenerate_model_score, non_degenerate_model);
                                else
                                    // non degenerate models are worse then so far the best model.
                                    continue;
                            } else
                                update_best(current_score, models[i]);

                            // update upper bound of iterations
                            max_iters = termination_criteria->update
                                    (best_model_thread, best_score_thread.inlier_number);
                            if (num_hypothesis_tested > max_iters) {
                                success = true; break;
                            }

                            if (LO && !is_magsac) {
                                // update model by Local optimizaion
                                if (local_optimization->refineModel
                                       (best_model_thread, best_score_thread, lo_model, lo_score))
                                    if (lo_score.isBetter(best_score_thread)) {
                                        update_best(lo_score, lo_model);
                                        // update termination again
                                        max_iters = termination_criteria->update
                                                (best_model_thread, best_score_thread.inlier_number);
                                        if (num_hypothesis_tested > max_iters) {
                                            success = true;
                                            break;
                                        }
                                    }
                            }
                        } // end of if so far the best score
                    } // end loop of number of models
                } // end of loop over iters
            }}); // end parallel
            ///////////////////////////////////////////////////////////////////////////////////////////////////////
            // find best model from all threads' models
            best_score = best_scores[0];
            int best_thread_idx = 0;
            for (int i = 1; i < MAX_THREADS; i++) {
                if (best_scores[i].isBetter(best_score)) {
                    best_score = best_scores[i];
                    best_thread_idx = i;
                }
            }
            best_model = best_models[best_thread_idx];
            final_iters = num_hypothesis_tested;
        }

        // if best model has 0 inliers then return fail
        if (best_score.inlier_number == 0)
            return false;

        // polish final model
        if (params->getFinalPolisher() != PolishingMethod::NonePolisher) {
            Mat polished_model;
            Score polisher_score;
            if (model_polisher->polishSoFarTheBestModel(best_model, best_score,
                     polished_model, polisher_score))
                if (polisher_score.isBetter(best_score)) {
                    best_score = polisher_score;
                    polished_model.copyTo(best_model);
                }
        }

        // ================= here is ending ransac main implementation ===========================
        std::vector<bool> inliers_mask;
        if (params->isMaskRequired() >= 1) {
            inliers_mask = std::vector<bool>(points_size);
            // get final inliers from the best model
            _quality->getInliers(best_model, inliers_mask);
        }
        // Store results
        ransac_output = RansacOutput::create(best_model, inliers_mask,
                static_cast<int>(std::chrono::duration_cast<std::chrono::microseconds>
                (std::chrono::steady_clock::now() - begin_time).count()), best_score.score,
                best_score.inlier_number, final_iters, -1, -1);
        return true;
    }
};

/*
 * pts1, pts2 are matrices either N x a, N x b or a x N or b x N, where N > a and N > b
 * pts1 are image points, if pnp pts2 are object points otherwise - image points as well.
 * output is matrix of size N x (a + b)
 * return points_size = N
 */
int mergePoints (InputArray pts1_, InputArray pts2_, Mat &pts, bool ispnp) {
    Mat pts1 = pts1_.getMat(), pts2 = pts2_.getMat();
    auto convertPoints = [] (Mat &points, bool is_vector, int pt_dim) {
        points.convertTo(points, CV_32F); // convert points to have float precision
        if (is_vector)
            points = Mat(points.total(), pt_dim, CV_32F, points.data);
        else {
            if (points.channels() > 1)
                points = points.reshape(1, points.total()); // convert point to have 1 channel
            if (points.rows < points.cols)
                transpose(points, points); // transpose so points will be in rows
            CV_CheckGE(points.cols, pt_dim, "Invalid dimension of point");
            if (points.cols != pt_dim) // in case when image points are 3D convert them to 2D
                points = points.colRange(0, pt_dim);
        }
    };

    convertPoints(pts1, pts1_.isVector(), 2); // pts1 are always image points
    convertPoints(pts2, pts2_.isVector(), ispnp ? 3 : 2); // for PnP points are 3D

    // points are of size [Nx2 Nx2] = Nx4 for H, F, E
    // points are of size [Nx2 Nx3] = Nx5 for PnP
    hconcat(pts1, pts2, pts);
    return pts.rows;
}

void saveMask (OutputArray mask, const std::vector<bool> &inliers_mask) {
    if (mask.needed()) {
        const int points_size = (int) inliers_mask.size();
        mask.create(1, points_size, CV_8U);
        auto * maskptr = mask.getMat().ptr<uchar>();
        for (int i = 0; i < points_size; i++)
            maskptr[i] = (uchar) inliers_mask[i];
    }
}

Mat findHomography (InputArray srcPoints, InputArray dstPoints, int method, double thr,
    OutputArray mask, const int maxIters, const double confidence) {

    Mat points;
    int points_size = mergePoints(srcPoints, dstPoints, points, false);

    Ptr<Model> params = Model::create(thr, EstimationMethod ::Homography,
                  SamplingMethod::Uniform, confidence, maxIters, ScoreMethod::MSAC);

    params->maskRequired(mask.needed());
    params->setLocalOptimization(LocalOptimMethod ::InLORsc);
    params->setPolisher(PolishingMethod ::LSQPolisher);
    params->setVerifier(VerificationMethod ::SprtVerifier);

    int state = 0;
    Ptr<Error> error = ReprojectionErrorForward::create(points);
    Ptr<Degeneracy> degeneracy = HomographyDegeneracy::create(points);
    Ptr<MinimalSolver> h_min = HomographyMinimalSolver4ptsGEM::create(points);
    Ptr<NonMinimalSolver> h_non_min = HomographyNonMinimalSolver::create(points);
    Ptr<Estimator> estimator = HomographyEstimator::create(h_min, h_non_min, degeneracy);
    Ptr<Quality> quality = MsacQuality::create(points_size, params->getThreshold(), error);
    Ptr<ModelVerifier> verifier = SPRT::create(state++, error, points_size,
                  params->getThreshold(), params->getSPRTepsilon(), params->getSPRTdelta(),
                  params->getTimeForModelEstimation(), params->getSPRTavgNumModels(), 1);
    Ptr<FinalModelPolisher> polisher =LeastSquaresPolishing::create(estimator,quality,points_size);
    Ptr<Sampler> sampler = UniformSampler::create(state++, params->getSampleSize(), points_size);
    Ptr<TerminationCriteria> termination = StandardTerminationCriteria::create(
            params->getConfidence(), points_size, params->getSampleSize(), params->getMaxIters());
    Ptr<Sampler> lo_sampler = UniformSampler::create
            (state++, params->getMaxSampleSizeLO(), points_size);
    Ptr<LocalOptimization> inner_lo_rsc = InnerLocalOptimization::create
            (estimator, quality, lo_sampler, points_size, 10 /*lo iters*/);

    Ptr<RansacOutput> ransac_output;
    Ransac ransac (params, points_size, estimator, quality, sampler,
       termination, verifier, degeneracy, inner_lo_rsc, polisher, method == USAC_PARALLEL, state);

    if (!ransac.run(ransac_output)) return Mat();

    saveMask(mask, ransac_output->getInliersMask());
    return ransac_output->getModel() / ransac_output->getModel().at<double>(2,2);
}

Mat findFundamentalMat( InputArray points1, InputArray points2,
        int method, double ransacReprojThreshold, double confidence,
        int maxIters, OutputArray mask ) {
    Mat points;
    int points_size = mergePoints(points1, points2, points, false);

    Ptr<Model> params = Model::create(ransacReprojThreshold,
         (method == USAC_DEFAULT || method == USAC_PARALLEL) ? EstimationMethod ::Fundamental :
                                                              EstimationMethod ::Fundamental8,
        SamplingMethod::Uniform, confidence, maxIters, ScoreMethod::MSAC);

    params->maskRequired(mask.needed());
    params->setLocalOptimization(LocalOptimMethod ::InLORsc);
    params->setPolisher(PolishingMethod ::LSQPolisher);
    params->setVerifier(VerificationMethod ::SprtVerifier);

    int state = 0;
    Ptr<Error> error = SampsonError::create(points);
    Ptr<Quality> quality = MsacQuality::create(points_size, params->getThreshold(), error);
    Ptr<Degeneracy> degeneracy = FundamentalDegeneracy::create
            (state++, quality, points, points_size, params->getSampleSize());
    Ptr<MinimalSolver> h_min = FundamentalMinimalSolver7pts::create(points);
    Ptr<NonMinimalSolver> h_non_min = FundamentalNonMinimalSolver::create(points);
    Ptr<Estimator> estimator = FundamentalEstimator::create(h_min, h_non_min, degeneracy);
    Ptr<ModelVerifier> verifier = SPRT::create(state++, error, points_size,
                   params->getThreshold(), params->getSPRTepsilon(), params->getSPRTdelta(),
                   params->getTimeForModelEstimation(), params->getSPRTavgNumModels(), 1);
    Ptr<FinalModelPolisher> polisher =LeastSquaresPolishing::create(estimator,quality,points_size);
    Ptr<Sampler> sampler = UniformSampler::create(state++, params->getSampleSize(), points_size);
    Ptr<TerminationCriteria> termination = StandardTerminationCriteria::create(
            params->getConfidence(), points_size, params->getSampleSize(), params->getMaxIters());

    Ptr<Sampler> lo_sampler = UniformSampler::create(state, params->getMaxSampleSizeLO(), points_size);
    Ptr<LocalOptimization> inner_lo_rsc = InnerLocalOptimization::create
            (estimator, quality, lo_sampler, points_size, 10 /*lo iters*/);

    Ransac ransac (params, points_size, estimator, quality, sampler,
       termination, verifier, degeneracy, inner_lo_rsc, polisher, method == USAC_PARALLEL, state);

    Ptr<RansacOutput> ransac_output;
    if (!ransac.run (ransac_output)) return Mat();

    saveMask(mask, ransac_output->getInliersMask());
    return ransac_output->getModel();
}

Mat findEssentialMat( InputArray points1, InputArray points2, InputArray cameraMatrix1,
        InputArray cameraMatrix2, int method, double prob, double threshold, int maxIters,
        OutputArray mask) {

    Mat points;
    int points_size = mergePoints(points1, points2, points, false);

    Mat K1 = cameraMatrix1.getMat(), K2 = cameraMatrix2.getMat();
    K1.convertTo(K1, CV_64F); K1.convertTo(K2, CV_64F);

    Mat calibrated_pts;
    Utils::calibratePoints(K1, K2, points, calibrated_pts);
    const double cal_thr = Utils::getCalibratedThreshold(threshold, K1, K2);

    Ptr<Model> params = Model::create(cal_thr, EstimationMethod ::Essential,
           SamplingMethod::Uniform, prob, maxIters, ScoreMethod::MSAC);

    params->maskRequired(mask.needed());
    params->setLocalOptimization(LocalOptimMethod ::InLORsc);
    params->setPolisher(PolishingMethod ::LSQPolisher);
    params->setVerifier(VerificationMethod ::SprtVerifier);

    int state = 0;
    Ptr<Error> error = SymmetricGeometricDistance::create(calibrated_pts);
    Ptr<Degeneracy> degeneracy = EssentialDegeneracy::create(calibrated_pts, params->getSampleSize());
    Ptr<MinimalSolver> h_min = EssentialMinimalSolverStewenius5pts::create(calibrated_pts);
    Ptr<NonMinimalSolver> h_non_min = EssentialNonMinimalSolver::create(points);
    Ptr<Estimator> estimator = EssentialEstimator::create(h_min, h_non_min, degeneracy);
    Ptr<Quality> quality = MsacQuality::create(points_size, params->getThreshold(), error);
    Ptr<ModelVerifier> verifier = SPRT::create(state++, error, points_size,
                   params->getThreshold(), params->getSPRTepsilon(), params->getSPRTdelta(),
                   params->getTimeForModelEstimation(), params->getSPRTavgNumModels(), 1);
    Ptr<FinalModelPolisher> polisher = LeastSquaresPolishing::create(estimator, quality, points_size);
    Ptr<Sampler> sampler = UniformSampler::create(state++, params->getSampleSize(), points_size);
    Ptr<TerminationCriteria> termination = StandardTerminationCriteria::create(
            params->getConfidence(), points_size, params->getSampleSize(), params->getMaxIters());

    Ptr<Sampler> lo_sampler = UniformSampler::create(state++, params->getMaxSampleSizeLO(), points_size);
    Ptr<LocalOptimization> inner_lo_rsc = InnerLocalOptimization::create(estimator,
            quality, lo_sampler, points_size, 7 /*lo iters*/);

    Ransac ransac (params, points_size, estimator, quality, sampler,
                   termination, verifier, degeneracy, inner_lo_rsc, polisher, method==USAC_PARALLEL, state);

    Ptr<RansacOutput> ransac_output;
    if (!ransac.run (ransac_output)) return Mat();

    saveMask(mask, ransac_output->getInliersMask());
    return ransac_output->getModel();
}

bool solvePnPRansac( InputArray objectPoints, InputArray imagePoints,
         InputArray cameraMatrix, InputArray distCoeffs,
         OutputArray rvec, OutputArray tvec,
         bool /*useExtrinsicGuess*/, int iterationsCount,
         float reprojectionError, double confidence,
         OutputArray inliers, int flags) {
    Mat points, undistored_pts;
    int points_size;
    if (! distCoeffs.empty()) {
        undistortPoints(imagePoints, undistored_pts, cameraMatrix, distCoeffs);
        points_size = mergePoints(undistored_pts, objectPoints, points, true);
    } else
        points_size = mergePoints(imagePoints, objectPoints, points, true);

    Ptr<Model> params;
    Ptr<MinimalSolver> min_solver;
    Ptr<NonMinimalSolver> non_min;

    Mat K = cameraMatrix.getMat(), calib_norm_points;
    if (cameraMatrix.empty()) {
        params = Model::create(reprojectionError, EstimationMethod ::P6P,
          SamplingMethod::Uniform, confidence, iterationsCount, ScoreMethod::MSAC);
        min_solver = PnPMinimalSolver6Pts::create(points);
        non_min = PnPNonMinimalSolver::create(points);
    } else {
        params = Model::create(reprojectionError, EstimationMethod ::P3P,
          SamplingMethod::Uniform, confidence, iterationsCount, ScoreMethod::MSAC);
        K.convertTo(K, CV_64F);
        Utils::calibrateAndNormalizePointsPnP(K, points, calib_norm_points);
        min_solver = P3PSolver::create(points, calib_norm_points, K);
        non_min = DLSPnP::create(points, calib_norm_points, K);
    }

    params->maskRequired(inliers.needed());
    params->setLocalOptimization(LocalOptimMethod ::InLORsc);
    params->setPolisher(PolishingMethod ::LSQPolisher);
    params->setVerifier(VerificationMethod ::SprtVerifier);

    int state = 0;
    Ptr<Error> error = ReprojectionErrorPmatrix::create(points);
    Ptr<Degeneracy> degeneracy = makePtr<Degeneracy>();
    Ptr<Estimator> estimator = PnPEstimator::create(min_solver, non_min, degeneracy);
    Ptr<Quality> quality = MsacQuality::create(points_size, params->getThreshold(), error);
    Ptr<ModelVerifier> verifier = SPRT::create(state++, error, points_size,
            params->getThreshold(), params->getSPRTepsilon(), params->getSPRTdelta(),
            params->getTimeForModelEstimation(), params->getSPRTavgNumModels(), 1);

    Ptr<FinalModelPolisher> polisher = LeastSquaresPolishing::create(estimator, quality, points_size);
    Ptr<Sampler> sampler = UniformSampler::create(state++, params->getSampleSize(), points_size);
    Ptr<TerminationCriteria> termination = StandardTerminationCriteria::create(
            params->getConfidence(), points_size, params->getSampleSize(), params->getMaxIters());

    Ptr<Sampler> lo_sampler = UniformSampler::create(state++, params->getMaxSampleSizeLO(), points_size);
    Ptr<LocalOptimization> inner_lo_rsc = InnerLocalOptimization::create(estimator, quality, lo_sampler, points_size, 3);
//    Ptr<LocalOptimization> inner_lo_rsc;

    Ransac ransac (params, points_size, estimator, quality, sampler,
                    termination, verifier, degeneracy, inner_lo_rsc, polisher, flags == USAC_PARALLEL, state);

    Ptr<RansacOutput> ransac_output;
    if (!ransac.run (ransac_output)) return false;

    saveMask(inliers, ransac_output->getInliersMask());

    if (! cameraMatrix.empty()) {
        Mat Rt = K.inv() * ransac_output->getModel();
        Rt.col(3).copyTo(tvec);
        Rodrigues(Rt.colRange(0,3), rvec);
    } else {
        Mat R, t;
        Utils::decomposeProjection (ransac_output->getModel(), K, R, t);
        t.copyTo(tvec);
        Rodrigues(R, rvec);
    }

    return true;
}

class ModelImpl : public Model {
private:
    // main parameters:
    double threshold, confidence;
    int sample_size, max_iterations;

    EstimationMethod estimator;
    SamplingMethod sampler;
    ScoreMethod score;

    // optional default parameters:

    // for neighborhood graph
    int k_nearest_neighbors = 8; // for FLANN
    int cell_size = 25; // pixels, for grid neighbors searching
//    double radius = 15; // pixels, for radius-search neighborhood graph
//    int flann_search_params = 32;
    NeighborSearchMethod neighborsType = NeighborSearchMethod::Grid;

    // Local Optimization parameters
    LocalOptimMethod lo = LocalOptimMethod ::NullLO;
    int lo_sample_size=14, lo_inner_iterations=10, lo_iterative_iterations=5,
            lo_threshold_multiplier=4, lo_iter_sample_size = 30;
    bool sample_size_limit = true; // parameter for Iterative LO-RANSAC

    // Graph cut parameters
    const double spatial_coherence_term = 0.1;

    // apply polisher for final RANSAC model
    PolishingMethod polisher = PolishingMethod ::LSQPolisher;

    // preemptive verification test
    VerificationMethod verifier = VerificationMethod ::NullVerifier;
    const int max_hypothesis_test_before_verification = 10;

    // sprt parameters
    double sprt_eps, sprt_delta, avg_num_models, time_for_model_est;

    // randomization of RANSAC
    bool reset_random_generator = false;

    // estimator error
    ErrorMetric est_error;

    // fill with zeros (if is not known)
    int img1_width = 0, img1_height = 0, img2_width = 2, img2_height = 0;

    // progressive napsac
    double relax_coef = 0.1;
    int sampler_length = 20;
    // for building neighborhood graphs
    const std::vector<int> grid_cell_number = {16, 8, 4, 2};

    //for final least squares polisher
    int final_lsq_iters = 3;

    bool need_mask = true;

    // magsac parameters for H, F, E
    int DoF = 4;
    double sigma_quantile = 3.64, upper_incomplete_of_sigma_quantile = 0.00365,
    lower_incomplete_of_sigma_quantile = 1.30122, C = 0.25, maximum_thr = 10.;
public:
    ModelImpl (double threshold_, EstimationMethod estimator_, SamplingMethod sampler_, double confidence_=0.95,
               int max_iterations_=5000, ScoreMethod score_ =ScoreMethod::RANSAC) {
        estimator = estimator_;
        switch (estimator_) {
            case (EstimationMethod::Similarity): sample_size = 2; est_error = ErrorMetric ::FORW_REPR_ERR; break;
            case (EstimationMethod::Affine): sample_size = 3; est_error = ErrorMetric ::FORW_REPR_ERR; break;
            case (EstimationMethod::Homography): sample_size = 4; est_error = ErrorMetric ::FORW_REPR_ERR;
                // time_for_model_est = 1.03;
                break;
            case (EstimationMethod::Fundamental): sample_size = 7; est_error = ErrorMetric ::SAMPSON_ERR; break;
            case (EstimationMethod::Fundamental8): sample_size = 8; est_error = ErrorMetric ::SAMPSON_ERR; break;
            case (EstimationMethod::Essential): sample_size = 5; est_error = ErrorMetric ::SGD_ERR; break;
            case (EstimationMethod::P3P): sample_size = 3; est_error = ErrorMetric ::RERPOJ; break;
            case (EstimationMethod::P6P): sample_size = 6; est_error = ErrorMetric ::RERPOJ; break;
            default: CV_Assert(0 && "Estimator has not implemented yet!");
        }

        sprt_eps = 0.011; // lower bound estimate is 1.1% of inliers
        sprt_delta = 0.01; avg_num_models = 1;
        time_for_model_est = 100;
        // for lower time sprt becomes super strict, so for the same iteration number
        // ransac will always be faster but less accurate.
        if (estimator == Essential || estimator == Fundamental || estimator == Fundamental8) {
            lo_sample_size = 14;
            // epipolar geometry usually have more inliers
            if (sample_size == 7) { // F seven points
                avg_num_models = 2.38;
                time_for_model_est = 125;
            } else if (sample_size == 5) { // E five points
                avg_num_models = 4.5;
                time_for_model_est = 150;
            } else if (sample_size == 6) { // E six points
                avg_num_models = 5;
            } else if (sample_size == 8) {
                avg_num_models = 1;
            }
        } else if (estimator_ == EstimationMethod::P3P) {
            avg_num_models = 1.4;
            time_for_model_est = 150;
        } else if (estimator_ == EstimationMethod::P6P) {
            avg_num_models = 1;
            time_for_model_est = 150;
        }

        /*
         * Measured reprojected error in homography is (x-x')^2 + (y-y)^2 (without squared root),
         * so threshold must be squared.
         */
        threshold = threshold_;
        if (est_error == ErrorMetric::FORW_REPR_ERR || est_error == ErrorMetric::SYMM_REPR_ERR
                || est_error == ErrorMetric ::RERPOJ)
            threshold *= threshold_;

        sampler = sampler_;
        confidence = confidence_;
        max_iterations = max_iterations_;
        score = score_;
    }
    void setVerifier (VerificationMethod verifier_) override {
        verifier = verifier_;
    }
    void setPolisher (PolishingMethod polisher_) override {
        polisher = polisher_;
    }
    void setError (ErrorMetric error_) override {
        est_error = error_;
    }
    void setLocalOptimization (LocalOptimMethod lo_) override {
        lo = lo_;
    }
    void setKNearestNeighhbors (int knn_) override {
        k_nearest_neighbors = knn_;
    }
    void setNeighborsType (NeighborSearchMethod neighbors) override {
        neighborsType = neighbors;
    }
    void setCellSize (int cell_size_) override {
        cell_size = cell_size_;
    }
    void setResetRandomGenerator (bool reset) override {
        reset_random_generator = reset;
    }
    void maskRequired (bool need_mask_) override { need_mask = need_mask_; }
    bool isMaskRequired () const override { return need_mask; }
    void setSPRT (double sprt_eps_ = 0.005, double sprt_delta_ = 0.0025,
                  double avg_num_models_ = 1, double time_for_model_est_ = 5e2) override {
        sprt_eps = sprt_eps_; sprt_delta = sprt_delta_;
        avg_num_models = avg_num_models_; time_for_model_est = time_for_model_est_;
    }
    void setImageSize (int img1_width_, int img1_height_,
                       int img2_width_, int img2_height_) override {
        img1_width = img1_width_, img1_height = img1_height_,
        img2_width = img2_width_, img2_height = img2_height_;
    }
    NeighborSearchMethod getNeighborsSearch () const override {
        return neighborsType;
    }
    int getKNN () const override {
        return k_nearest_neighbors;
    }
    ErrorMetric getError () const override {
        return est_error;
    }
    EstimationMethod getEstimator () const override {
        return estimator;
    }
    int getSampleSize () const override {
        return sample_size;
    }
    int getSamplerLengthPNAPSAC () const override {
        return sampler_length;
    }
    int getFinalLSQIterations () const override {
        return final_lsq_iters;
    }
    int getDegreesOfFreedom () const override {
        return DoF;
    }
    double getSigmaQuantile () const override {
        return sigma_quantile;
    }
    double getUpperIncompleteOfSigmaQuantile () const override {
        return upper_incomplete_of_sigma_quantile;
    }
    double getLowerIncompleteOfSigmaQuantile () const override {
        return lower_incomplete_of_sigma_quantile;
    }
    double getC () const override {
        return C;
    }
    double getMaximumThreshold () const override {
        return maximum_thr;
    }
    double getGraphCutSpatialCoherenceTerm () const override {
        return spatial_coherence_term;
    }
    int getLOSampleSize () const override {
        return lo_sample_size;
    }
    bool resetRandomGenerator () const override {
        return reset_random_generator;
    }
    int getMaxNumHypothesisToTestBeforeRejection() const override {
        return max_hypothesis_test_before_verification;
    }
    PolishingMethod getFinalPolisher () const override {
        return polisher;
    }
    int getLOThresholdMultiplier() const override {
        return lo_threshold_multiplier;
    }
    int getLOIterativeSampleSize() const override {
        return lo_iter_sample_size;
    }
    Size2i getImage1Size () const override {
        return Size2i(img1_width, img1_height);
    }
    Size2i getImage2Size () const override {
        return Size2i(img2_width, img2_height);
    }
    int getLOIterativeMaxIters() const override {
        return lo_iterative_iterations;
    }
    int getLOInnerMaxIters() const override {
        return lo_inner_iterations;
    }
    LocalOptimMethod getLO () const override {
        return lo;
    }
    ScoreMethod getScore () const override {
        return score;
    }
    int getMaxIters () const override {
        return max_iterations;
    }
    double getConfidence () const override {
        return confidence;
    }
    double getThreshold () const override {
        return threshold;
    }
    VerificationMethod getVerifier () const override {
        return verifier;
    }
    SamplingMethod getSampler () const override {
        return sampler;
    }
    int getMaxSampleSizeLO () const override {
        return lo_inner_iterations;
    }
    int getMaxSampleSizeLOiterative () const override {
        return lo_iter_sample_size;
    }
    double getSPRTdelta () const override {
        return sprt_delta;
    }
    double getSPRTepsilon () const override {
        return sprt_eps;
    }
    double getSPRTavgNumModels () const override {
        return avg_num_models;
    }
    int getCellSize () const override {
        return cell_size;
    }
    double getTimeForModelEstimation () const override {
        return time_for_model_est;
    }
    bool isSampleLimit () const override {
        return sample_size_limit;
    }
    double getRelaxCoef () const override {
        return relax_coef;
    }
    const std::vector<int> &getGridCellNumber () const override {
        return grid_cell_number;
    }
    bool isFundamental () const override {
        return estimator == EstimationMethod ::Fundamental ||
               estimator == EstimationMethod ::Fundamental8;
    }
    bool isHomography () const override {
        return estimator == EstimationMethod ::Homography;
    }
    bool isEssential () const override {
        return estimator == EstimationMethod ::Essential;
    }
    bool isPnP() const override {
        return estimator == EstimationMethod ::P3P || estimator == EstimationMethod ::P6P;
    }
};

Ptr<Model> Model::create(double threshold_, EstimationMethod estimator_, SamplingMethod sampler_,
                         double confidence_, int max_iterations_, ScoreMethod score_) {
    return makePtr<ModelImpl>(threshold_, estimator_, sampler_, confidence_,
                              max_iterations_, score_);
}
}}