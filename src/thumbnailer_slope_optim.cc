// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "thumbnailer.h"

namespace libwebp {

Thumbnailer::Status Thumbnailer::GenerateAnimationSlopeOptim(
    WebPData* const webp_data) {
  for (auto& frame : frames_) {
    // Initialize 'lossy_data' array.
    std::fill(frame.lossy_data, frame.lossy_data + 101,
              std::make_pair(-1, -1.0));
  }

  CHECK_THUMBNAILER_STATUS(LossyEncodeSlopeOptim(webp_data));
  CHECK_THUMBNAILER_STATUS(NearLosslessEqual(webp_data));

  // if there is no frame encoded in near-lossless, generate animation with
  // lossy encoding so that all frames have the same quality value.
  int num_near_ll_frames = 0;
  for (auto& frame : frames_)
    if (frame.near_lossless) {
      ++num_near_ll_frames;
    }
  if (!num_near_ll_frames) {
    CHECK_THUMBNAILER_STATUS(GenerateAnimationEqualQuality(webp_data));
    return kOk;
  }

  int curr_anim_size = webp_data->size;
  const int KMaxIter = 5;
  for (int i = 0; i < KMaxIter; ++i) {
    CHECK_THUMBNAILER_STATUS(LossyEncodeNoSlopeOptim(webp_data));
    if (curr_anim_size == webp_data->size) break;
    curr_anim_size = webp_data->size;
  }
  CHECK_THUMBNAILER_STATUS(ExtraLossyEncode(webp_data));

  return kOk;
}

Thumbnailer::Status Thumbnailer::FindMedianSlope(float* const median_slope) {
  std::vector<float> slopes;

  int curr_ind = 0;
  for (auto& frame : frames_) {
    frame.config.quality = 100;
    float psnr_100;  // pic's psnr value with quality = 100.
    int size_100;    // pic'size with quality = 100.
    CHECK_THUMBNAILER_STATUS(GetPictureStats(curr_ind, &size_100, &psnr_100));

    int min_quality = 0;
    int max_quality = 100;
    float pic_final_slope;

    // Use binary search to find the leftmost point on the curve so that the
    // difference in PSNR between this point and the one with quality value 100
    // is roughly 1.
    while (min_quality <= max_quality) {
      int mid_quality = (min_quality + max_quality) / 2;
      frame.config.quality = mid_quality;
      float new_psnr;
      int new_size;

      CHECK_THUMBNAILER_STATUS(GetPictureStats(curr_ind, &new_size, &new_psnr));

      if (psnr_100 - new_psnr <= 1.0) {
        pic_final_slope = (psnr_100 - new_psnr) / float(size_100 - new_size);
        max_quality = mid_quality - 1;
      } else {
        min_quality = mid_quality + 1;
      }
    }

    slopes.push_back(pic_final_slope);

    ++curr_ind;
  }

  std::sort(slopes.begin(), slopes.end());
  *median_slope = slopes[slopes.size() / 2];

  return kOk;
}

Thumbnailer::Status Thumbnailer::ComputeSlope(int ind, int low_quality,
                                              int high_quality,
                                              float* const slope) {
  frames_[ind].config.quality = low_quality;
  int low_size;
  float low_psnr;
  CHECK_THUMBNAILER_STATUS(GetPictureStats(ind, &low_size, &low_psnr));

  frames_[ind].config.quality = high_quality;
  int high_size;
  float high_psnr;
  CHECK_THUMBNAILER_STATUS(GetPictureStats(ind, &high_size, &high_psnr));

  if (high_size == low_size) {
    *slope = 0;
  } else {
    *slope = (high_psnr - low_psnr) / float(high_size - low_size);
  }

  return kOk;
}

Thumbnailer::Status Thumbnailer::LossyEncodeSlopeOptim(
    WebPData* const webp_data) {
  // Sort frames.
  std::sort(frames_.begin(), frames_.end(),
            [](const FrameData& a, const FrameData& b) -> bool {
              return a.timestamp_ms < b.timestamp_ms;
            });

  float limit_slope;
  CHECK_THUMBNAILER_STATUS(FindMedianSlope(&limit_slope));

  int min_quality = minimum_lossy_quality_;
  int max_quality = 100;
  WebPData new_webp_data;
  WebPDataInit(&new_webp_data);

  std::vector<int> optim_list;  // Vector of frames needed to find the quality
                                // in the next binary search loop.
  for (int i = 0; i < frames_.size(); ++i) {
    optim_list.push_back(i);
  }

  // Use binary search with slope optimization to find quality values that makes
  // the animation fit the given byte budget. The quality value for each frame
  // can be different.
  while (min_quality <= max_quality && !optim_list.empty()) {
    int mid_quality = (min_quality + max_quality) / 2;

    std::vector<int> new_optim_list;

    for (int curr_frame : optim_list) {
      float curr_slope;
      CHECK_THUMBNAILER_STATUS(
          ComputeSlope(curr_frame, min_quality, max_quality, &curr_slope));

      if (frames_[curr_frame].final_quality == -1 || curr_slope > limit_slope) {
        frames_[curr_frame].config.quality = mid_quality;
        new_optim_list.push_back(curr_frame);
      }
    }

    if (new_optim_list.empty()) break;

    CHECK_THUMBNAILER_STATUS(GenerateAnimationNoBudget(&new_webp_data));

    if (new_webp_data.size <= byte_budget_) {
      for (int curr_frame : new_optim_list) {
        frames_[curr_frame].final_quality = mid_quality;
      }
      WebPDataClear(webp_data);
      *webp_data = new_webp_data;
      min_quality = mid_quality + 1;
    } else {
      max_quality = mid_quality - 1;
      WebPDataClear(&new_webp_data);
    }

    optim_list = new_optim_list;
  }

  std::cerr << "Final qualities with slope optimization:" << std::endl;
  int curr_ind = 0;
  for (auto& frame : frames_) {
    frame.config.quality = frame.final_quality;
    CHECK_THUMBNAILER_STATUS(
        GetPictureStats(curr_ind++, &frame.encoded_size, &frame.final_psnr));
    std::cerr << frame.config.quality << " ";
  }
  std::cerr << std::endl;

  return (webp_data->size > 0) ? kOk : kByteBudgetError;
}

Thumbnailer::Status Thumbnailer::LossyEncodeNoSlopeOptim(
    WebPData* const webp_data) {
  int anim_size = GetAnimationSize(webp_data);

  // if the 'anim_size' exceed the 'byte_budget', keep the webp_data generated
  // by the previous steps as result and do nothing here.
  if (anim_size > byte_budget_) return kOk;

  int num_remaining_frames = frames_.size();
  int curr_ind = 0;

  // For each frame, find the best quality value that can produce the higher
  // PSNR than the current one if possible.
  for (auto& frame : frames_) {
    int min_quality = 70;
    if (!frame.config.lossless) {
      min_quality = frame.final_quality;
    }
    int max_quality = std::min(min_quality + 30, 100);
    frame.config.lossless = 0;
    while (min_quality <= max_quality) {
      int mid_quality = (min_quality + max_quality) / 2;
      int new_size;
      float new_psnr;
      frame.config.quality = mid_quality;
      CHECK_THUMBNAILER_STATUS(GetPictureStats(curr_ind, &new_size, &new_psnr));

      if (new_psnr > frame.final_psnr || ((new_psnr == frame.final_psnr) &&
                                          (new_size <= frame.encoded_size))) {
        const float extra_budget =
            (byte_budget_ - anim_size) / num_remaining_frames;
        if (float(new_size - frame.encoded_size) <= extra_budget) {
          anim_size = anim_size - frame.encoded_size + new_size;
          frame.encoded_size = new_size;
          frame.final_psnr = new_psnr;
          frame.final_quality = mid_quality;
          frame.near_lossless = false;
          min_quality = mid_quality + 1;
        } else {
          max_quality = mid_quality - 1;
        }
      } else {
        min_quality = mid_quality + 1;
      }
    }

    num_remaining_frames--;
    ++curr_ind;
  }

  for (auto& frame : frames_) {
    frame.config.quality = frame.final_quality;
    frame.config.lossless = frame.near_lossless;
  }

  WebPData new_webp_data;
  WebPDataInit(&new_webp_data);
  CHECK_THUMBNAILER_STATUS(GenerateAnimationNoBudget(&new_webp_data));

  if (new_webp_data.size <= byte_budget_) {
    WebPDataClear(webp_data);
    *webp_data = new_webp_data;
  } else {
    WebPDataClear(&new_webp_data);
    return kOk;
  }

  std::cerr << "(Final quality, Near-lossless) :" << std::endl;
  for (auto& frame : frames_) {
    std::cerr << "(" << frame.final_quality << ", " << frame.near_lossless
              << ") ";
  }
  std::cerr << std::endl;

  return (webp_data->size > 0) ? kOk : kByteBudgetError;
}

Thumbnailer::Status Thumbnailer::ExtraLossyEncode(WebPData* const webp_data) {
  // Encode frames following the ascending order of slope between frame's
  // current quality and quality 100.
  std::vector<std::pair<float, int>> encoding_order;
  for (int i = 0; i < frames_.size(); ++i)
    if (!frames_[i].near_lossless) {
      float slope;
      CHECK_THUMBNAILER_STATUS(
          ComputeSlope(i, frames_[i].final_quality, 100, &slope));
      encoding_order.push_back(std::make_pair(slope, i));
    }
  std::sort(encoding_order.begin(), encoding_order.end(),
            [](const std::pair<float, int>& x, const std::pair<float, int>& y) {
              return x.first < y.first;
            });

  WebPData new_webp_data;
  WebPDataInit(&new_webp_data);
  int num_remaining_frames = encoding_order.size();

  while (!encoding_order.empty()) {
    int min_quality = 100;
    for (int i = 0; i < num_remaining_frames; ++i) {
      const int curr_ind = encoding_order[i].second;
      min_quality = std::min(min_quality, frames_[curr_ind].final_quality + 1);
    }
    int max_quality = std::min(min_quality + 30, 100);
    int final_quality = -1;

    while (min_quality <= max_quality) {
      int mid_quality = (min_quality + max_quality) / 2;
      for (int i = 0; i < num_remaining_frames; ++i) {
        const int curr_ind = encoding_order[i].second;
        frames_[curr_ind].config.quality =
            std::max(frames_[curr_ind].final_quality, mid_quality);
      }
      CHECK_THUMBNAILER_STATUS(GenerateAnimationNoBudget(&new_webp_data));
      if (new_webp_data.size <= byte_budget_) {
        final_quality = mid_quality;
        WebPDataClear(webp_data);
        *webp_data = new_webp_data;
        min_quality = mid_quality + 1;
      } else {
        max_quality = mid_quality - 1;
        WebPDataClear(&new_webp_data);
      }
    }
    if (final_quality != -1) {
      for (int i = 0; i < num_remaining_frames; ++i) {
        const int curr_ind = encoding_order[i].second;
        if (frames_[curr_ind].final_quality < final_quality) {
          frames_[curr_ind].config.quality = final_quality;
          frames_[curr_ind].final_quality = final_quality;
          CHECK_THUMBNAILER_STATUS(
              GetPictureStats(curr_ind, &frames_[curr_ind].encoded_size,
                              &frames_[curr_ind].final_psnr));
        }
      }
    } else {
      break;
    }

    encoding_order.erase(encoding_order.begin());
    --num_remaining_frames;
  }

  std::cerr << "(Final quality, Near-lossless) :" << std::endl;
  for (auto& frame : frames_) {
    std::cerr << "(" << frame.final_quality << ", " << frame.near_lossless
              << ") ";
  }
  std::cerr << std::endl;

  return (webp_data->size > 0) ? kOk : kByteBudgetError;
}

}  // namespace libwebp
