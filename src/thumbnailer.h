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

#ifndef THUMBNAILER_SRC_CLASS_H_
#define THUMBNAILER_SRC_CLASS_H_

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cassert>
#include <memory>
#include <utility>
#include <vector>

#include "../imageio/image_dec.h"
#include "../imageio/imageio_util.h"
#include "thumbnailer.pb.h"
#include "webp/encode.h"
#include "webp/mux.h"

namespace libwebp {

// Takes time stamped images as an input and produces an animation.
class Thumbnailer {
 public:
  Thumbnailer();
  Thumbnailer(const thumbnailer::ThumbnailerOption& thumbnailer_option);
  ~Thumbnailer();

  // Status codes for adding frame and generating animation.
  enum Status {
    kOk = 0,            // On success.
    kMemoryError,       // In case of memory error.
    kImageFormatError,  // If frame dimensions are mismatched.
    kByteBudgetError,   // If there is no quality that makes the animation fit
                        // the byte budget.
    kStatsError         // In case of error while getting frame's size and PSNR.
  };

  // Adds a frame with a timestamp (in millisecond). The 'pic' argument must
  // outlive the last GenerateAnimation() call.
  Status AddFrame(const WebPPicture& pic, int timestamp_ms);

  // Generates the animation.
  Status GenerateAnimationNoBudget(WebPData* const webp_data);

  // Finds the best quality that makes the animation fit right below the given
  // byte budget and generates the animation. The 'webp_data' argument is
  // expected to be initialized (otherwise WebPDataClear() might free some
  // random memory somewhere because the pointer is undefined).
  Status GenerateAnimation(WebPData* const webp_data);

  // Compute the size (in bytes) and PSNR of a re-encoded WebPPicture at given
  // config. The resulting size and PSNR will be stored in '*pic_size' and
  // '*pic_PSNR' respectively.
  Status GetPictureStats(const WebPPicture& pic, const WebPConfig& config,
                         int* const pic_size, float* const pic_PSNR);

  // Generates the animation so that all frames have similar PSNR (all) values.
  // In case of failure, takes the animation generated by GenerateAnimation() as
  // result.
  Status GenerateAnimationEqualPSNR(WebPData* const webp_data);

  // Tries near-lossless to encode each frame. Either GenerateAnimation() or
  // GenerateAnimationEqualPSNR() must be called before to generate animation
  // with lossy encoding. In case of failure, keeps the latest lossy encoded
  // frame.
  Status TryNearLossless(WebPData* const webp_data);

  Status SetLoopCount(WebPData* const webp_data);

 private:
  struct FrameData {
    WebPPicture pic;
    int timestamp_ms;
    WebPConfig config;
    int encoded_size;
    int final_quality;
    float final_psnr;
  };
  std::vector<FrameData> frames_;
  WebPAnimEncoder* enc_ = NULL;
  WebPAnimEncoderOptions anim_config_;
  int loop_count_;
  int byte_budget_;
  int minimum_lossy_quality_;
};

}  // namespace libwebp

#endif  // THUMBNAILER_SRC_CLASS_H_
