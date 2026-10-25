#pragma once
/**
 * @file        rex/ui/surface_android.h
 * @brief       Android ANativeWindow surface
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <android/native_window.h>

#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class AndroidNativeWindowSurface final : public Surface {
 public:
  explicit AndroidNativeWindowSurface(ANativeWindow* window) : window_(window) {}
  TypeIndex GetType() const override { return kTypeIndex_AndroidNativeWindow; }
  ANativeWindow* window() const { return window_; }

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    int32_t width = ANativeWindow_getWidth(window_);
    int32_t height = ANativeWindow_getHeight(window_);
    if (width <= 0 || height <= 0) {
      return false;
    }
    width_out = uint32_t(width);
    height_out = uint32_t(height);
    return true;
  }

 private:
  ANativeWindow* window_;
};

}  // namespace ui
}  // namespace rex
