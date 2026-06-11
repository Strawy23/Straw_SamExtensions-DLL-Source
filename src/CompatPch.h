#pragma once





#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <memory>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <new>
#include <string>
#include <sstream>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>






#ifndef STRAW_SAMCONSOLE_F4SE_LOG_STUBS
#define STRAW_SAMCONSOLE_F4SE_LOG_STUBS
inline void _MESSAGE(const char*, ...) {}
inline void _WARNING(const char*, ...) {}
inline void _ERROR(const char*, ...) {}
inline void _DMESSAGE(const char*, ...) {}
#endif

#include "common/ITypes.h"
