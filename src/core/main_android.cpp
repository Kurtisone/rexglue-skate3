/**
 * @file        rex/core/main_android.cpp
 * @brief       Android process setup shared by the runtime libraries
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>
#if REX_PLATFORM_ANDROID

#include <rex/main_android.h>
#include <rex/memory/utils.h>
#include <rex/thread.h>

namespace rex {

void InitializeAndroid() {
  rex::memory::AndroidInitialize();
  rex::thread::AndroidInitialize();
}

void ShutdownAndroid() {
  rex::thread::AndroidShutdown();
  rex::memory::AndroidShutdown();
}

}  // namespace rex

#endif  // REX_PLATFORM_ANDROID
