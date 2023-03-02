#pragma once

#include <inttypes.h>
#include <cstring>
#include <stdlib.h>
#include <math.h>
#include <atomic>
#include <string>
#include <iostream>

#ifdef ARCHITECTURE_X64
    #include <immintrin.h>
#endif

#if defined(_MSC_VER)
    #define PLATFORM_WINDOWS
    #include <intrin.h>
#elif defined(__GNUC__) || defined(__clang__)
    #define PLATFORM_LINUX
#endif

#if defined(PLATFORM_LINUX)
    #include <csignal>
#endif


#if defined(PLATFORM_WINDOWS)
    #define DEBUG_BREAK() __debugbreak()
#elif defined(PLATFORM_LINUX)
    #define DEBUG_BREAK() std::raise(SIGINT)
#endif


#ifndef CONFIGURATION_FINAL
    #define ASSERT(x) do { if (!(x)) { std::cout << "Assertion failed: " << #x << std::endl; DEBUG_BREAK(); } } while (0)
    #define VERIFY(x) do { if (!(x)) { std::cout << "Assertion failed: " << #x << std::endl; DEBUG_BREAK(); } } while (0)
#else
    #define ASSERT(x) do { } while (0)
    #define VERIFY(x) (x)
#endif

#define CACHELINE_SIZE 64u

#if defined(_MSC_VER)

    #define USE_TABLE_BASES

    // "C++ nonstandard extension: nameless struct"
    #pragma warning(disable : 4201)

    // "unreferenced local function"
    #pragma warning(disable : 4505)

    // "structure was padded due to alignment specifier"
    #pragma warning(disable : 4324)

    #define INLINE __forceinline
    #define INLINE_LAMBDA [[msvc::forceinline]]
    #define NO_INLINE __declspec(noinline)

    INLINE uint32_t PopCount(uint8_t x)
    {
#ifdef USE_POPCNT
        return __popcnt16(x);
#else // !USE_POPCNT
        const uint8_t m1 = 0x55;
        const uint8_t m2 = 0x33;
        const uint8_t m4 = 0x0f;
        x = (x & m1) + ((x >> 1) & m1);
        x = (x & m2) + ((x >> 2) & m2);
        x = (x & m4) + ((x >> 4) & m4);
        return x;
#endif // USE_POPCNT
    }

    INLINE uint32_t PopCount(uint16_t x)
    {
#ifdef USE_POPCNT
        return __popcnt16(x);
#else // !USE_POPCNT
        const uint16_t m1 = 0x5555;
        const uint16_t m2 = 0x3333;
        const uint16_t m4 = 0x0f0f;
        const uint16_t m8 = 0x00ff;
        x = (x & m1) + ((x >> 1) & m1);
        x = (x & m2) + ((x >> 2) & m2);
        x = (x & m4) + ((x >> 4) & m4);
        x = (x & m8) + ((x >> 8) & m8);
        return x;
#endif // USE_POPCNT
    }

    INLINE uint32_t PopCount(uint32_t x)
    {
#ifdef USE_POPCNT
        return __popcnt(x);
#else // !USE_POPCNT
        x -= ((x >> 1) & 0x55555555);
        x = (x & 0x33333333) + ((x >> 2) & 0x33333333);
        return (((x + (x >> 4)) & 0xF0F0F0F) * 0x1010101) >> 24;
#endif // USE_POPCNT
    }

    INLINE uint32_t PopCount(uint64_t x)
    {
#ifdef USE_POPCNT
        return (uint32_t)__popcnt64(x);
#else // !USE_POPCNT
        // https://en.wikipedia.org/wiki/Hamming_weight
        const uint64_t m1 = 0x5555555555555555;
        const uint64_t m2 = 0x3333333333333333;
        const uint64_t m4 = 0x0f0f0f0f0f0f0f0f;
        const uint64_t h01 = 0x0101010101010101;
        x -= (x >> 1) & m1;
        x = (x & m2) + ((x >> 2) & m2);
        x = (x + (x >> 4)) & m4;
        return static_cast<uint32_t>((x * h01) >> 56);
#endif // USE_POPCNT
    }

    INLINE uint32_t FirstBitSet(uint64_t x)
    {
#ifdef USE_POPCNT
        unsigned long index;
        _BitScanForward64(&index, x);
        return (uint32_t)index;
#else // !USE_POPCNT
        uint64_t v = x;   // 32-bit word input to count zero bits on right
        uint32_t c = 64;  // c will be the number of zero bits on the right
        v &= -static_cast<int64_t>(v);
        if (v) c--;
        if (v & 0x00000000FFFFFFFFul) c -= 32;
        if (v & 0x0000FFFF0000FFFFul) c -= 16;
        if (v & 0x00FF00FF00FF00FFul) c -= 8;
        if (v & 0x0F0F0F0F0F0F0F0Ful) c -= 4;
        if (v & 0x3333333333333333ul) c -= 2;
        if (v & 0x5555555555555555ul) c -= 1;
        return c;
#endif // USE_POPCNT
    }

    INLINE uint32_t LastBitSet(uint64_t x)
    {
#ifdef USE_POPCNT
        unsigned long index;
        _BitScanReverse64(&index, x);
        return (uint32_t)index;
#else // !USE_POPCNT
        // algorithm by Kim Walisch and Mark Dickinson 
        const uint8_t index64[64] =
        {
             0, 47,  1, 56, 48, 27,  2, 60,
            57, 49, 41, 37, 28, 16,  3, 61,
            54, 58, 35, 52, 50, 42, 21, 44,
            38, 32, 29, 23, 17, 11,  4, 62,
            46, 55, 26, 59, 40, 36, 15, 53,
            34, 51, 20, 43, 31, 22, 10, 45,
            25, 39, 14, 33, 19, 30,  9, 24,
            13, 18,  8, 12,  7,  6,  5, 63,
        };
        x |= x >> 1;
        x |= x >> 2;
        x |= x >> 4;
        x |= x >> 8;
        x |= x >> 16;
        x |= x >> 32;
        return index64[(x * 0x03f79d71b4cb0a89ull) >> 58];
#endif // USE_POPCNT
    }

#elif defined(__GNUC__) || defined(__clang__)

    #define INLINE __attribute__((always_inline)) inline
    #define INLINE_LAMBDA
    #define NO_INLINE __attribute__((noinline))

    INLINE uint32_t PopCount(uint64_t x)
    {
        return (uint32_t)__builtin_popcountll(x);
    }

    INLINE uint32_t FirstBitSet(uint64_t x)
    {
        return (uint32_t)__builtin_ctzll(x);
    }

    INLINE uint32_t LastBitSet(uint64_t x)
    {
        return 63u ^ (uint32_t)__builtin_clzll(x);
    }

#endif

#if defined(__GNUC__)
    #define UNNAMED_STRUCT __extension__
#else
    #define UNNAMED_STRUCT
#endif

#if defined(__GNUC__) || defined(__clang__)
    #define EXPORT __attribute__((visibility("default")))
#else
    #define EXPORT
#endif


template<typename T>
INLINE constexpr bool IsPowerOfTwo(const T n)
{
    return (n & (n - 1)) == 0;
}

template<typename T>
INLINE constexpr T Sqr(const T& x)
{
    return x * x;
}

inline uint64_t ParallelBitsDeposit(uint64_t src, uint64_t mask)
{
#ifdef USE_BMI2
    return _pdep_u64(src, mask);
#else
    uint64_t result = 0;
    for (uint64_t m = 1; mask; m += m)
    {
        if (src & m)
        {
            result |= mask & -static_cast<int64_t>(mask);
        }
        mask &= mask - 1;
    }
    return result;
#endif
}

inline uint32_t ParallelBitsDeposit(uint32_t src, uint32_t mask)
{
#ifdef USE_BMI2
    return _pdep_u32(src, mask);
#else
    uint32_t result = 0;
    for (uint32_t m = 1; mask; m += m)
    {
        if (src & m)
        {
            result |= mask & -static_cast<int32_t>(mask);
        }
        mask &= mask - 1;
    }
    return result;
#endif
}

inline uint64_t ParallelBitsExtract(uint64_t src, uint64_t mask)
{
#ifdef USE_BMI2
    return _pext_u64(src, mask);
#else
    uint64_t result = 0;
    for (uint64_t bb = 1; mask; bb += bb)
    {
        if (src & mask & -static_cast<int64_t>(mask))
        {
            result |= bb;
        }
        mask &= mask - 1;
    }
    return result;
#endif
}

inline uint64_t ParallelBitsExtract(uint32_t src, uint32_t mask)
{
#ifdef USE_BMI2
    return _pext_u32(src, mask);
#else
    uint32_t result = 0;
    for (uint32_t bb = 1; mask; bb += bb)
    {
        if (src & mask & -static_cast<int32_t>(mask))
        {
            result |= bb;
        }
        mask &= mask - 1;
    }
    return result;
#endif
}

inline uint64_t SwapBytes(uint64_t x)
{
#if defined(PLATFORM_WINDOWS)
    return _byteswap_uint64(x);
#else
    return __builtin_bswap64(x);
#endif
}

// return high bits of a 64 bit multiplication
inline uint64_t MulHi64(uint64_t a, uint64_t b)
{
#if defined(__GNUC__) && defined(ARCHITECTURE_X64)
    return ((unsigned __int128)a * (unsigned __int128)b) >> 64;
#elif defined(_MSC_VER) && defined(ARCHITECTURE_X64)
    return (uint64_t)__umulh(a, b);
#else
    uint64_t aLow = (uint32_t)a, aHi = a >> 32;
    uint64_t bLow = (uint32_t)b, bHi = b >> 32;
    uint64_t c1 = (aLow * bLow) >> 32;
    uint64_t c2 = aHi * bLow + c1;
    uint64_t c3 = aLow * bHi + (uint32_t)c2;
    return aHi * bHi + (c2 >> 32) + (c3 >> 32);
#endif
}

inline uint8_t ReverseBits(uint8_t x)
{
    constexpr uint8_t lookup[16] = { 0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe, 0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf };
    return (lookup[x & 0xf] << 4) | lookup[x >> 4];
}

template<typename T, T multiple>
INLINE constexpr const T RoundUp(const T x)
{
    return ((x + (multiple - 1)) / multiple) * multiple;
}

template<typename T>
INLINE void AtomicMax(std::atomic<T>& outMax, T const& value) noexcept
{
    T prev = outMax;
    while (prev < value && !outMax.compare_exchange_weak(prev, value)) { }
}

class SpinLock
{
public:
    void lock()
    {
        for (;;)
        {
            if (!lock_.exchange(true, std::memory_order_acquire)) break;
            while (lock_.load(std::memory_order_relaxed));
        }
    }

    void unlock()
    {
        lock_.store(false, std::memory_order_release);
    }

private:
    std::atomic<bool> lock_ = { false };
};


union MaterialKey;
class Position;
struct TTEntry;
struct Move;
struct PackedMove;
class MoveList;
class Game;
class TranspositionTable;
struct NodeInfo;
struct NNEvaluatorContext;
enum class NetworkInputMapping : uint8_t;

using ScoreType = int16_t;

static constexpr ScoreType InfValue             = 32767;
static constexpr ScoreType InvalidValue         = INT16_MAX;
static constexpr ScoreType CheckmateValue       = 32000;
static constexpr ScoreType TablebaseWinValue    = 31000;
static constexpr ScoreType KnownWinValue        = 20000;

static constexpr uint16_t MaxSearchDepth    = 256;

// maximum number of pieces in "normal" chess position
static constexpr uint32_t MaxNumPieces      = 32;

static constexpr ScoreType DrawScoreRandomness = 2;

extern int32_t g_TunedParameter;

// initialize all engine subsystems
EXPORT void InitEngine();

// get exe path
std::string GetExecutablePath();
