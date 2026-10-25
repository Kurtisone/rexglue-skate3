#pragma once
/**
 * @file        rex/main_android.h
 * @brief       Android process setup shared by the runtime libraries
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <android/api-level.h>

namespace rex {

inline int GetAndroidApiLevel() { return android_get_device_api_level(); }

// Loads the API-level dependent entry points (ASharedMemory_create,
// pthread_getname_np). Call once at startup, before guest memory is created.
void InitializeAndroid();
void ShutdownAndroid();

}  // namespace rex
