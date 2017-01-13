#include "mtf/SM/GridTrackerCV.h"
#include "mtf/Utilities/miscUtils.h"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/video/tracking.hpp"
#include "opencv2/highgui/highgui.hpp"
#include <stdexcept>

_MTF_BEGIN_NAMESPACE


GridTrackerCVParams::GridTrackerCVParams(
int _grid_size_x, int _grid_size_y,
int _search_window_x, int _search_window_y,
bool _reset_at_each_frame, bool _patch_centroid_inside,
double _backward_err_thresh,
int _pyramid_levels, bool _use_min_eig_vals,
double _min_eig_thresh, int _max_iters,
double _epsilon, bool _show_pts,
bool _debug_mode) :
grid_size_x(_grid_size_x),
grid_size_y(_grid_size_y),
search_window_x(_search_window_x),
search_window_y(_search_window_y),
reset_at_each_frame(_reset_at_each_frame),
patch_centroid_inside(_patch_centroid_inside),
backward_err_thresh(_backward_err_thresh),
pyramid_levels(_pyramid_levels),
use_min_eig_vals(_use_min_eig_vals),
min_eig_thresh(_min_eig_thresh),
max_iters(_max_iters),
epsilon(_epsilon),
show_pts(_show_pts),
debug_mode(_debug_mode){
	updateRes();

}
GridTrackerCVParams::GridTrackerCVParams(const GridTrackerCVParams *params) :
grid_size_x(GTCV_GRID_SIZE_X),
grid_size_y(GTCV_GRID_SIZE_Y),
search_window_x(GTCV_SEARCH_WINDOW_X),
search_window_y(GTCV_SEARCH_WINDOW_Y),
reset_at_each_frame(GTCV_RESET_AT_EACH_FRAME),
patch_centroid_inside(GTCV_PATCH_CENTROID_INSIDE),
backward_err_thresh(GTCV_BACKWARD_EST_THRESH),
pyramid_levels(GTCV_PYRAMID_LEVELS),
use_min_eig_vals(GTCV_USE_MIN_EIG_VALS),
min_eig_thresh(GTCV_MIN_EIG_THRESH),
max_iters(GTCV_MAX_ITERS), epsilon(GTCV_EPSILON),
show_pts(GTCV_SHOW_PTS),
debug_mode(GTCV_DEBUG_MODE){
	if(params){
		grid_size_x = params->grid_size_x;
		grid_size_y = params->grid_size_y;
		search_window_x = params->search_window_x;
		search_window_y = params->search_window_y;
		reset_at_each_frame = params->reset_at_each_frame;
		patch_centroid_inside = params->patch_centroid_inside;
		backward_err_thresh = params->backward_err_thresh;
		pyramid_levels = params->pyramid_levels;
		use_min_eig_vals = params->use_min_eig_vals;
		min_eig_thresh = params->min_eig_thresh;
		max_iters = params->max_iters;
		epsilon = params->epsilon;
		show_pts = params->show_pts;
		debug_mode = params->debug_mode;
	}
	updateRes();
}
void GridTrackerCVParams::updateRes(){
	if(patch_centroid_inside){
		resx = grid_size_x + 1;
		resy = grid_size_y + 1;
	} else{
		resx = grid_size_x;
		resy = grid_size_y;
	}
}

template<class SSM>
GridTrackerCV<SSM>::GridTrackerCV(const ParamType *grid_params,
	const EstimatorParams *_est_params, const SSMParams *_ssm_params) :
	GridBase(), ssm(_ssm_params), params(grid_params),
	est_params(_est_params), enable_backward_est(false){
	printf("\n");
	printf("Using OpenCV Grid tracker with:\n");
	printf("grid_size: %d x %d\n", params.grid_size_x, params.grid_size_y);
	printf("search window size: %d x %d\n", params.search_window_x, params.search_window_y);
	printf("reset_at_each_frame: %d\n", params.reset_at_each_frame);
	printf("patch_centroid_inside: %d\n", params.patch_centroid_inside);
	printf("backward_err_thresh: %f\n", params.backward_err_thresh);
	printf("pyramid_levels: %d\n", params.pyramid_levels);
	printf("use_min_eig_vals: %d\n", params.use_min_eig_vals);
	printf("min_eig_thresh: %f\n", params.min_eig_thresh);
	printf("max_iters: %d\n", params.max_iters);
	printf("show_pts: %d\n", params.show_pts);
	printf("debug_mode: %d\n", params.debug_mode);
	printf("\n");

	printf("Using %s estimator with:\n", ssm.name.c_str());
	est_params.print();

	name = "grid_cv";

	if(ssm.getResX() != params.getResX() || ssm.getResY() != params.getResY()){
		throw std::invalid_argument(
			cv::format("GridTrackerCV: SSM has invalid sampling resolution: %d x %d",
			ssm.getResX(), ssm.getResY()));
	}

	n_pts = params.grid_size_x *params.grid_size_y;
	search_window = cv::Size(params.search_window_x, params.search_window_x);
	lk_term_criteria = cv::TermCriteria(cv::TermCriteria::COUNT + cv::TermCriteria::EPS,
		params.max_iters, params.epsilon);

	patch_corners.create(2, 4, CV_64FC1);
	cv_corners_mat.create(2, 4, CV_64FC1);

	prev_pts_mat.create(n_pts, 2, CV_32FC1);
	curr_pts_mat.create(n_pts, 2, CV_32FC1);

	prev_pts.resize(n_pts);
	curr_pts.resize(n_pts);

	ssm_update.resize(ssm.getStateSize());
	lk_status.resize(n_pts);
	pix_mask.resize(n_pts);
	std::fill(pix_mask.begin(), pix_mask.end(), 1);
	pause_seq = 0;

	if(params.use_min_eig_vals){
		lk_flags = cv::OPTFLOW_LK_GET_MIN_EIGENVALS;
	} else{
		lk_flags = 0;
	}

	int sub_regions_x = params.grid_size_x + 1, sub_regions_y = params.grid_size_y + 1;
	_linear_idx.resize(sub_regions_y, sub_regions_x);
	for(int idy = 0; idy < sub_regions_y; idy++){
		for(int idx = 0; idx < sub_regions_x; idx++){
			_linear_idx(idy, idx) = idy * sub_regions_x + idx;
		}
	}
	if(params.backward_err_thresh > 0){
		printf("Backward estimation is enabled\n");
		enable_backward_est = true;
		est_prev_pts.resize(n_pts);
		backward_est_mask.resize(n_pts);
	}
	if(params.show_pts){
		patch_win_name = "Optical Flow Points";
		cv::namedWindow(patch_win_name);
	}
}

template<class SSM>
void GridTrackerCV<SSM>::initialize(const cv::Mat &corners) {
	curr_img_float.convertTo(curr_img, curr_img.type());

	ssm.initialize(corners);
	resetPts();
	for(int pt_id = 0; pt_id < n_pts; pt_id++){
		curr_pts[pt_id].x = prev_pts[pt_id].x;
		curr_pts[pt_id].y = prev_pts[pt_id].y;
	}
	curr_img.copyTo(prev_img);

	ssm.getCorners(cv_corners_mat);
	if(params.show_pts){ showPts(); }
}
template<class SSM>
void GridTrackerCV<SSM>::update() {
	curr_img_float.convertTo(curr_img, curr_img.type());
	cv::calcOpticalFlowPyrLK(prev_img, curr_img,
		prev_pts, curr_pts, lk_status, lk_error,
		search_window, params.pyramid_levels,
		lk_term_criteria, lk_flags, params.min_eig_thresh);

	if(enable_backward_est){
		cv::calcOpticalFlowPyrLK(curr_img, prev_img,
			curr_pts, est_prev_pts, lk_status, lk_error,
			search_window, params.pyramid_levels,
			lk_term_criteria, lk_flags, params.min_eig_thresh);
		std::vector<cv::Point2f> prev_pts_masked, curr_pts_masked;
		for(int pt_id = 0; pt_id < n_pts; ++pt_id){
			double diff_x = est_prev_pts[pt_id].x - prev_pts[pt_id].x;
			double diff_y = est_prev_pts[pt_id].y - prev_pts[pt_id].y;

			if(diff_x*diff_x + diff_y*diff_y > params.backward_err_thresh){
				backward_est_mask[pt_id] = false;
			} else{
				backward_est_mask[pt_id] = true;
				prev_pts_masked.push_back(prev_pts[pt_id]);
				curr_pts_masked.push_back(curr_pts[pt_id]);
			}
		}
		if(prev_pts_masked.size() < est_params.n_model_pts){
			for(int pt_id = 0; pt_id < n_pts; ++pt_id){
				if(backward_est_mask[pt_id]){ continue; }
				prev_pts_masked.push_back(prev_pts[pt_id]);
				curr_pts_masked.push_back(curr_pts[pt_id]);
				backward_est_mask[pt_id] = true;
				if(prev_pts_masked.size() == est_params.n_model_pts){
					break;
				}
			}
		}
		std::vector<uchar> pix_mask_est(prev_pts_masked.size());
		ssm.estimateWarpFromPts(ssm_update, pix_mask_est, prev_pts_masked,
			curr_pts_masked, est_params);
		if(pix_mask_needed){
			int est_pt_id = 0;
			for(int pt_id = 0; pt_id < n_pts; ++pt_id){
				pix_mask[pt_id] = backward_est_mask[pt_id] ?
					pix_mask_est[est_pt_id++] : 0;
			}
		}
	} else{
		ssm.estimateWarpFromPts(ssm_update, pix_mask, prev_pts, curr_pts, est_params);
	}

	Matrix24d opt_warped_corners;
	ssm.applyWarpToCorners(opt_warped_corners, ssm.getCorners(), ssm_update);
	ssm.setCorners(opt_warped_corners);

	if(params.reset_at_each_frame){
		resetPts();
	} else{
		for(int pt_id = 0; pt_id < n_pts; ++pt_id){
			prev_pts[pt_id].x = curr_pts[pt_id].x;
			prev_pts[pt_id].y = curr_pts[pt_id].y;
		}
	}
	ssm.getCorners(cv_corners_mat);
	curr_img.copyTo(prev_img);

	if(params.show_pts){ showPts(); }
}
template<class SSM>
void GridTrackerCV<SSM>::setImage(const cv::Mat &img){
	if(curr_img_float.empty()){
		prev_img.create(img.rows, img.cols, CV_8UC1);
		curr_img.create(img.rows, img.cols, CV_8UC1);
	}
	if(params.show_pts && curr_img_uchar.empty()){
		curr_img_uchar.create(img.rows, img.cols, CV_8UC3);
	}
	curr_img_float = img;
}

template<class SSM>
void GridTrackerCV<SSM>::setRegion(const cv::Mat& corners) {
	ssm.setCorners(corners);
	resetPts();
	ssm.getCorners(cv_corners_mat);
}

template<class SSM>
void GridTrackerCV<SSM>::resetPts(){
	for(int pt_id = 0; pt_id < n_pts; ++pt_id){
		if(params.patch_centroid_inside){
			int row_id = pt_id / params.grid_size_x;
			int col_id = pt_id % params.grid_size_x;

			patch_corners.at<double>(0, 0) = ssm.getPts()(0, _linear_idx(row_id, col_id));
			patch_corners.at<double>(1, 0) = ssm.getPts()(1, _linear_idx(row_id, col_id));

			patch_corners.at<double>(0, 1) = ssm.getPts()(0, _linear_idx(row_id, col_id + 1));
			patch_corners.at<double>(1, 1) = ssm.getPts()(1, _linear_idx(row_id, col_id + 1));

			patch_corners.at<double>(0, 2) = ssm.getPts()(0, _linear_idx(row_id + 1, col_id + 1));
			patch_corners.at<double>(1, 2) = ssm.getPts()(1, _linear_idx(row_id + 1, col_id + 1));

			patch_corners.at<double>(0, 3) = ssm.getPts()(0, _linear_idx(row_id + 1, col_id));
			patch_corners.at<double>(1, 3) = ssm.getPts()(1, _linear_idx(row_id + 1, col_id));

			utils::getCentroid(prev_pts[pt_id], patch_corners);
		} else{
			prev_pts[pt_id].x = ssm.getPts()(0, pt_id);
			prev_pts[pt_id].y = ssm.getPts()(1, pt_id);
		}
	}
}

template<class SSM>
void GridTrackerCV<SSM>::showPts(){
	curr_img_float.convertTo(curr_img_uchar, curr_img_uchar.type());
	cv::cvtColor(curr_img_uchar, curr_img_uchar, CV_GRAY2BGR);
	utils::drawRegion(curr_img_uchar, cv_corners_mat, CV_RGB(0, 0, 255), 2);
	for(int pt_id = 0; pt_id < n_pts; pt_id++) {
		cv::Scalar pt_color;
		if(enable_backward_est){
			pt_color = backward_est_mask[pt_id] ?
				pix_mask[pt_id] ? CV_RGB(0, 255, 0) : CV_RGB(255, 0, 0) :
				CV_RGB(0, 0, 255);
		} else{
			pt_color = pix_mask[pt_id] ? CV_RGB(0, 255, 0) : CV_RGB(255, 0, 0);
		}
		circle(curr_img_uchar, curr_pts[pt_id], 2, pt_color, 2);
	}
	imshow(patch_win_name, curr_img_uchar);
	//int key = cv::waitKey(1 - pause_seq);
	//if(key == 32){
	//	pause_seq = 1 - pause_seq;
	//}
}

_MTF_END_NAMESPACE

#ifndef HEADER_ONLY_MODE
#include "mtf/Macros/register.h"
_REGISTER_TRACKERS_SSM(GridTrackerCV);
#endif