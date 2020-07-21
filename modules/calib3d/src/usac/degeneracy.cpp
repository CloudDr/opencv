// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.

#include "../precomp.hpp"
#include "../usac.hpp"

namespace cv { namespace usac {
class EpipolarGeometryDegeneracyImpl : public EpipolarGeometryDegeneracy {
private:
    const Mat *points_mat;
    const float * const points; // i-th row xi1 yi1 xi2 yi2
    const int sample_size;
public:
    explicit EpipolarGeometryDegeneracyImpl (const Mat &points_, int sample_size_) :
            points_mat(&points_), points ((float*) points_.data), sample_size (sample_size_) {}
    /*
     * Do oriented constraint to verify if epipolar geometry is in front or behind the camera.
     * Return: true if all points are in front of the camers w.r.t. tested epipolar geometry - satisfies constraint.
     *         false - otherwise.
     */
    inline bool isModelValid(const Mat &F, const std::vector<int> &sample) const override {
        const int sample_size_ = static_cast<int>(sample.size());
        Mat ec;
        epipole(ec, F);
        const auto * const F_ptr = (double *) F.data;
        const auto * const ec_ptr = (double *) ec.data;

        // without loss of generality, let the first point in sample be in front of the camera.
        double sig1 = getorisig(F_ptr, ec_ptr, 4*sample[0]);

        for (int i = 1; i < sample_size_; i++)
            // if signum of first point and tested point differs
            // then two points are on different sides of the camera.
            if (sig1 * getorisig(F_ptr, ec_ptr, 4*sample[i]) < 0)
                return false;
        return true;
    }
    Ptr<Degeneracy> clone(int /*state*/) const override {
        return makePtr<EpipolarGeometryDegeneracyImpl>(*points_mat, sample_size);
    }
private:
    /* Oriented constraint:
     * x'^T F x = 0
     * e' × x' ~+ Fx   <=>  λe' × x' = Fx, λ > 0
     * e  × x ~+ x'^T F
     */
    inline void epipole(Mat &ec, const Mat &F) const {
        // F is of rank 2, taking cross product of two rows we obtain null vector of F
        ec = F.row(0).cross(F.row(2));
        const auto * const ec_ = (double *) ec.data; // of size 3x1

        // if e_i is not zero then return
        if ((ec_[0] > 1.9984e-15) || (ec_[0] < -1.9984e-15)) return;
        if ((ec_[1] > 1.9984e-15) || (ec_[1] < -1.9984e-15)) return;
        if ((ec_[2] > 1.9984e-15) || (ec_[2] < -1.9984e-15)) return;

        // e is zero vector, recompute e
        ec = F.row(1).cross(F.row(2));
    }

    // F is 9x1 row-major ordered F matrix. ec is 3x1
    inline double getorisig(const double * const F, const double * const ec, int pt_idx) const {
        // s1 = F11 * x2 + F21 * y2 + F31 * 1
        // s2 = e'_2 * 1 - e'_3 * y1
        // return: s1 * s2
        return (F[0] * points[pt_idx+2] + F[3] * points[pt_idx+3] + F[6]) *
               (ec[1] - ec[2] * points[pt_idx+1]);
    }
};
void EpipolarGeometryDegeneracy::recoverRank (Mat &model) {
    /*
     * Do singular value decomposition.
     * Make last eigen value zero of diagonal matrix of singular values.
     */
    Matx33d U, Vt;
    Vec3d w;
    SVD::compute(model, w, U, Vt, SVD::FULL_UV + SVD::MODIFY_A);
    Matx33d W (w(0), 0, 0, 0, w(1), 0, 0, 0, 0);
    model = Mat(U * W * Vt);
}
Ptr<EpipolarGeometryDegeneracy> EpipolarGeometryDegeneracy::create (const Mat &points_,
        int sample_size_) {
    return makePtr<EpipolarGeometryDegeneracyImpl>(points_, sample_size_);
}

class HomographyDegeneracyImpl : public HomographyDegeneracy {
private:
    const Mat * points_mat;
    const float * const points;
public:
    explicit HomographyDegeneracyImpl (const Mat &points_) :
            points_mat(&points_), points ((float *)points_.data) {}

    inline bool isSampleGood (const std::vector<int> &sample) const override {
        const int smpl1 = 4*sample[0], smpl2 = 4*sample[1], smpl3 = 4*sample[2], smpl4 = 4*sample[3];
        const float x1 = points[smpl1], y1 = points[smpl1+1], X1 = points[smpl1+2], Y1 = points[smpl1+3];
        const float x2 = points[smpl2], y2 = points[smpl2+1], X2 = points[smpl2+2], Y2 = points[smpl2+3];
        const float x3 = points[smpl3], y3 = points[smpl3+1], X3 = points[smpl3+2], Y3 = points[smpl3+3];
        const float x4 = points[smpl4], y4 = points[smpl4+1], X4 = points[smpl4+2], Y4 = points[smpl4+3];

        const float ab_cross_x = y1 - y2, ab_cross_y = x2 - x1, ab_cross_z = x1 * y2 - y1 * x2;
        const float AB_cross_x = Y1 - Y2, AB_cross_y = X2 - X1, AB_cross_z = X1 * Y2 - Y1 * X2;

        // check ab cross with point c and d
        if ((ab_cross_x * x3 + ab_cross_y * y3 + ab_cross_z) *
            (AB_cross_x * X3 + AB_cross_y * Y3 + AB_cross_z) < 0)
            return false;
        if ((ab_cross_x * x4 + ab_cross_y * y4 + ab_cross_z) *
            (AB_cross_x * X4 + AB_cross_y * Y4 + AB_cross_z) < 0)
            return false;

        const float cd_cross_x = y3 - y4, cd_cross_y = x4 - x3, cd_cross_z = x3 * y4 - y3 * x4;
        const float CD_cross_x = Y3 - Y4, CD_cross_y = X4 - X3, CD_cross_z = X3 * Y4 - Y3 * X4;

        // check ab cross with point a and b
        if ((cd_cross_x * x1 + cd_cross_y * y1 + cd_cross_z) *
            (CD_cross_x * X1 + CD_cross_y * Y1 + CD_cross_z) < 0)
            return false;
        if ((cd_cross_x * x2 + cd_cross_y * y2 + cd_cross_z) *
            (CD_cross_x * X2 + CD_cross_y * Y2 + CD_cross_z) < 0)
            return false;
        return true;
    }
    Ptr<Degeneracy> clone(int /*state*/) const override {
        return makePtr<HomographyDegeneracyImpl>(*points_mat);
    }
};
Ptr<HomographyDegeneracy> HomographyDegeneracy::create (const Mat &points_) {
    return makePtr<HomographyDegeneracyImpl>(points_);
}

///////////////////////////////// Fundamental Matrix Degeneracy ///////////////////////////////////
class FundamentalDegeneracyImpl : public FundamentalDegeneracy {
private:
    RNG rng;
    const Ptr<Quality> quality;
    const float * const points;
    const Mat * points_mat;
    const Ptr<ReprojectionErrorForward> h_reproj_error;
    const EpipolarGeometryDegeneracyImpl ep_deg;
    // threshold to find inliers for homography model
    const double homography_threshold, log_conf = log(0.05);
    // points (1-7) to verify in sample
    std::vector<std::vector<int>> h_sample {{0,1,2},{3,4,5},{0,1,6},{3,4,6},{2,5,6}};
    const int points_size, sample_size;
public:

    FundamentalDegeneracyImpl (int state, const Ptr<Quality> &quality_, const Mat &points_,
            int points_size_, int sample_size_, double homography_threshold_) :
            rng (state), quality(quality_), points((float *) points_.data), points_mat(&points_),
            h_reproj_error(ReprojectionErrorForward::create(points_)),
            ep_deg (points_, sample_size_), homography_threshold (homography_threshold_),
            points_size (points_size_), sample_size (sample_size_) {
        if (sample_size_ == 8) {
            h_sample.emplace_back(std::vector<int>{0, 1, 7});
            h_sample.emplace_back(std::vector<int>{0, 2, 7});
            h_sample.emplace_back(std::vector<int>{3, 5, 7});
            h_sample.emplace_back(std::vector<int>{3, 6, 7});
            h_sample.emplace_back(std::vector<int>{2, 4, 7});
        }
    }
    inline bool isModelValid(const Mat &F, const std::vector<int> &sample) const override {
        return ep_deg.isModelValid(F, sample);
    }

    // fix degenerate Fundamental matrix.
    bool recoverIfDegenerate (const std::vector<int> &sample, const Mat &F_best,
                 Mat &non_degenerate_model, Score &non_degenerate_model_score) override {
        non_degenerate_model_score = Score(); // set worst case

        // According to Two-view Geometry Estimation Unaffected by a Dominant Plane
        // (http://cmp.felk.cvut.cz/~matas/papers/chum-degen-cvpr05.pdf)
        // only 5 homographies enough to test
        // triplets {1,2,3}, {4,5,6}, {1,2,7}, {4,5,7} and {3,6,7}

        // H = A - e' (M^-1 b)^T
        // A = [e']_x F
        // b_i = (x′i × (A xi))^T (x′i × e′)‖x′i×e′‖^−2,
        // M is a 3×3 matrix with rows x^T_i
        // epipole e' is left nullspace of F s.t. e′^T F=0,

        // find e', null space of F^T
        Vec3d e_prime = F_best.col(0).cross(F_best.col(2));
        if (fabs(e_prime(0)) < 1e-10 && fabs(e_prime(1)) < 1e-10 &&
            fabs(e_prime(2)) < 1e-10) // if e' is zero
            e_prime = F_best.col(1).cross(F_best.col(2));

        const Matx33d A = Math::getSkewSymmetric(e_prime) * Matx33d(F_best);

        Vec3d xi_prime(0,0,1), xi(0,0,1), b;
        Matx33d M(0,0,1,0,0,1,0,0,1); // last column of M is 1

        bool is_model_degenerate = false;
        for (const auto &h_i : h_sample) { // only 5 samples
            for (int pt_i = 0; pt_i < 3; pt_i++) {
                // find b and M
                const int smpl = 4*sample[h_i[pt_i]];
                xi[0] = points[smpl];
                xi[1] = points[smpl+1];
                xi_prime[0] = points[smpl+2];
                xi_prime[1] = points[smpl+3];

                // (x′i × e')
                const Vec3d xprime_X_eprime = xi_prime.cross(e_prime);

                // (x′i × (A xi))
                const Vec3d xprime_X_Ax = xi_prime.cross(A * xi);

                // x′i × (A xi))^T (x′i × e′) / ‖x′i×e′‖^2,
                b[pt_i] = xprime_X_Ax.dot(xprime_X_eprime) /
                           std::pow(norm(xprime_X_eprime), 2);

                // M from x^T
                M(pt_i, 0) = xi[0];
                M(pt_i, 1) = xi[1];
            }

            // compute H
            const Matx33d H = A - e_prime * (M.inv() * b).t();

            int inliers_on_plane = 0;
            h_reproj_error->setModelParameters(Mat(H));

            // find inliers from sample, points related to H, x' ~ Hx
            for (int s = 0; s < sample_size; s++)
                if (h_reproj_error->getError(sample[s]) < homography_threshold)
                    inliers_on_plane++;

            // if there are at least 5 points lying on plane then F is degenerate
            if (inliers_on_plane >= 5) {
                is_model_degenerate = true;

                Mat newF;
                Score newF_score = planeAndParallaxRANSAC(H, newF);
                if (newF_score.isBetter(non_degenerate_model_score)) {
                    // store non degenerate model
                    non_degenerate_model_score = newF_score;
                    newF.copyTo(non_degenerate_model);
                }
            }
        }
        return is_model_degenerate;
    }
    Ptr<Degeneracy> clone(int state) const override {
        return makePtr<FundamentalDegeneracyImpl>(state, quality->clone(), *points_mat,
            points_size, sample_size, homography_threshold);
    }
private:
    // RANSAC with plane-and-parallax to find new Fundamental matrix
    Score planeAndParallaxRANSAC (const Matx33d &H, Mat &best_F) {
        int max_iters = 100; // with 95% confidence assume at least 17% of inliers
        Score best_score;
        for (int iters = 0; iters < max_iters; iters++) {
            // draw two random points
            int h_outlier1 = rng.uniform(0, points_size);
            int h_outlier2 = rng.uniform(0, points_size);
            while (h_outlier1 == h_outlier2)
                h_outlier2 = rng.uniform(0, points_size);

            // find outliers of homography H
            if (h_reproj_error->getError(h_outlier1) > homography_threshold &&
                h_reproj_error->getError(h_outlier2) > homography_threshold) {

                // do plane and parallax with outliers of H
                Vec3d pt1 (points[4*h_outlier1], points[4*h_outlier1+1], 1);
                Vec3d pt2 (points[4*h_outlier2], points[4*h_outlier2+1], 1);
                Vec3d pt1_prime (points[4*h_outlier1+2],points[4*h_outlier1+3],1);
                Vec3d pt2_prime (points[4*h_outlier2+2],points[4*h_outlier2+3],1);

                // F = [(p1' x Hp1) x (p2' x Hp2)]_x H
                const Matx33d F = Math::getSkewSymmetric((pt1_prime.cross(H * pt1)).cross
                                                         (pt2_prime.cross(H * pt2))) * H;

                const Score score = quality->getScore(Mat(F));
                if (score.isBetter(best_score)) {
                    best_score = score;
                    best_F = Mat(F);
                    const double predicted_iters = log_conf / log(1 - std::pow
                            (static_cast<double>(score.inlier_number) / points_size, 2));

                    if (! std::isinf(predicted_iters) && predicted_iters < max_iters)
                        max_iters = static_cast<int>(predicted_iters);
                }
            }
        }
        return best_score;
    }
};
Ptr<FundamentalDegeneracy> FundamentalDegeneracy::create (int state, const Ptr<Quality> &quality_,
        const Mat &points_, int points_size_, int sample_size_, double homography_threshold_) {
    return makePtr<FundamentalDegeneracyImpl>(state, quality_, points_, points_size_, sample_size_,
            homography_threshold_);
}
}}