#include "PackedNeuralNetwork.hpp"
#include "Accumulator.hpp"
#include "Memory.hpp"
#include "Math.hpp"

#include <cassert>
#include <cmath>
#include <algorithm>
#include <iostream>
#include <vector>

#if defined(PLATFORM_LINUX)
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/mman.h>
    #include <sys/stat.h>
#endif // PLATFORM_LINUX


namespace nn {

static_assert(sizeof(PackedNeuralNetwork::Header) % CACHELINE_SIZE == 0, "Network header size must be multiple of cacheline size");

INLINE static void ClippedReLU_Accum(uint32_t size, IntermediateType* output, const AccumulatorType* input)
{
    static_assert(std::is_same_v<AccumulatorType, int16_t>, "Invalid type");

#if defined(NN_USE_AVX2)
    constexpr uint32_t inRegisterWidth = 256 / 16;
    constexpr uint32_t outRegisterWidth = 256 / 8;
    ASSERT(size % outRegisterWidth == 0);
    const uint32_t numOutChunks = size / outRegisterWidth;
    ASSERT((size_t)output % 32 == 0);
    ASSERT((size_t)input % 32 == 0);

    for (uint32_t i = 0; i < numOutChunks; ++i)
    {
        const __m256i in0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(input));
        input += inRegisterWidth;
        const __m256i in1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(input));
        input += inRegisterWidth;

        const __m256i result =
            // packs changes the order, so we need to fix that with a permute
            _mm256_permute4x64_epi64(
                // packs saturates to 127, so we only need to clamp from below
                _mm256_max_epi8(_mm256_packs_epi16(in0, in1), _mm256_setzero_si256()),
                0b11'01'10'00
            );

        _mm256_store_si256(reinterpret_cast<__m256i*>(output), result);
        output += outRegisterWidth;
    }

#elif defined(NN_USE_SSE4)
    constexpr uint32_t inRegisterWidth = 128 / 16;
    constexpr uint32_t outRegisterWidth = 128 / 8;
    ASSERT(size % outRegisterWidth == 0);
    const uint32_t numOutChunks = size / outRegisterWidth;
    ASSERT((size_t)output % 16 == 0);
    ASSERT((size_t)input % 16 == 0);

    for (uint32_t i = 0; i < numOutChunks; ++i)
    {
        const __m128i in0 = _mm_load_si128(reinterpret_cast<const __m128i*>(input));
        input += inRegisterWidth;
        const __m128i in1 = _mm_load_si128(reinterpret_cast<const __m128i*>(input));
        input += inRegisterWidth;

        // packs saturates to 127, so we only need to clamp from below
        const __m128i result = _mm_max_epi8(_mm_packs_epi16(in0, in1), _mm_setzero_si128());

        _mm_store_si128(reinterpret_cast<__m128i*>(output), result);
        output += outRegisterWidth;
    }

#else // no SIMD support
    for (uint32_t i = 0; i < size; ++i)
    {
        output[i] = (IntermediateType)std::clamp<AccumulatorType>(input[i], 0, std::numeric_limits<IntermediateType>::max());
    }
#endif
}

#ifdef USE_AVX2

INLINE static void m256_add_dpbusd_epi32(__m256i& acc, __m256i a, __m256i b)
{
#if defined (NN_USE_VNNI)
    acc = _mm256_dpbusd_epi32(acc, a, b);
#else
    __m256i product0 = _mm256_maddubs_epi16(a, b);
    product0 = _mm256_madd_epi16(product0, _mm256_set1_epi16(1));
    acc = _mm256_add_epi32(acc, product0);
#endif
}

INLINE static __m128i m256_haddx4(__m256i a, __m256i b, __m256i c, __m256i d)
{
    a = _mm256_hadd_epi32(a, b);
    c = _mm256_hadd_epi32(c, d);
    a = _mm256_hadd_epi32(a, c);
    const __m128i sum128lo = _mm256_castsi256_si128(a);
    const __m128i sum128hi = _mm256_extracti128_si256(a, 1);
    return _mm_add_epi32(sum128lo, sum128hi);
}

INLINE static int32_t m256_hadd(__m256i a)
{
    const __m256i sum1 = _mm256_hadd_epi32(a, a);
    const __m256i sum2 = _mm256_hadd_epi32(sum1, sum1);
    const __m128i sum3 = _mm256_extracti128_si256(sum2, 1);
    return _mm_cvtsi128_si32(_mm_add_epi32(_mm256_castsi256_si128(sum2), sum3));
}

#endif // USE_AVX2

#ifdef USE_AVX512

INLINE static int32_t m512_hadd(__m512i v)
{
    const __m256i sum256 = _mm256_add_epi32(
        _mm512_castsi512_si256(v),
        _mm512_extracti64x4_epi64(v, 1));
    return m256_hadd(sum256);
}

#endif // USE_AVX512

#ifdef USE_SSE4

INLINE static void m128_add_dpbusd_epi32(__m128i& acc, __m128i a, __m128i b)
{
#if defined (USE_VNNI)
    acc = _mm_dpbusd_epi32(acc, a, b);
#else
    __m128i product0 = _mm_maddubs_epi16(a, b);
    product0 = _mm_madd_epi16(product0, _mm_set1_epi16(1));
    acc = _mm_add_epi32(acc, product0);
#endif
}

INLINE static __m128i m128_haddx4(__m128i a, __m128i b, __m128i c, __m128i d)
{
    return _mm_hadd_epi32(_mm_hadd_epi32(a, b), _mm_hadd_epi32(c, d));
}

INLINE static int32_t m128_hadd(__m128i a)
{
    a = _mm_hadd_epi32(a, a);
    a = _mm_hadd_epi32(a, a);
    return _mm_cvtsi128_si32(a);
}

#endif // USE_SSE4

INLINE static void LinearLayer(
    const HiddenLayerWeightType* weights, const HiddenLayerBiasType* biases,
    uint32_t numInputs, uint32_t numOutputs, int32_t* output, const IntermediateType* input)
{
#if defined(NN_USE_AVX2)
    if (numInputs >= 32)
    {
        constexpr uint32_t registerWidth = 256 / 8;
        const uint32_t numOutChunks = numOutputs / 4u;
        ASSERT(numInputs % registerWidth == 0);
        ASSERT(numOutputs % 4u == 0);
        ASSERT((size_t)weights % 32 == 0);
        ASSERT((size_t)biases % 32 == 0);
        ASSERT((size_t)output % 32 == 0);
        ASSERT((size_t)input % 32 == 0);

        for (uint32_t i = 0; i < numOutChunks; ++i)
        {
            // Prepare weight offsets. One offset for one row of weights.
            // This is a simple index into a 2d array.
            const uint32_t offset0 = (i * 4u + 0u) * numInputs;
            const uint32_t offset1 = (i * 4u + 1u) * numInputs;
            const uint32_t offset2 = (i * 4u + 2u) * numInputs;
            const uint32_t offset3 = (i * 4u + 3u) * numInputs;

            // Accumulation starts from 0, we add the bias only at the end.
            __m256i sum0 = _mm256_setzero_si256();
            __m256i sum1 = _mm256_setzero_si256();
            __m256i sum2 = _mm256_setzero_si256();
            __m256i sum3 = _mm256_setzero_si256();

            // Each innermost loop processes a 32x4 chunk of weights, so 128 weights at a time!
            for (uint32_t j = 0; j < numInputs; j += registerWidth)
            {
                // We unroll by 4 so that we can reuse this value, reducing the number of memory operations required.
                const __m256i in = _mm256_load_si256(reinterpret_cast<const __m256i*>(input + j));

                // This function processes a 32x1 chunk of int8 and produces a 8x1 chunk of int32.
                const HiddenLayerWeightType* weightsBase = weights + j;
                m256_add_dpbusd_epi32(sum0, in, _mm256_load_si256(reinterpret_cast<const __m256i*>(weightsBase + offset0)));
                m256_add_dpbusd_epi32(sum1, in, _mm256_load_si256(reinterpret_cast<const __m256i*>(weightsBase + offset1)));
                m256_add_dpbusd_epi32(sum2, in, _mm256_load_si256(reinterpret_cast<const __m256i*>(weightsBase + offset2)));
                m256_add_dpbusd_epi32(sum3, in, _mm256_load_si256(reinterpret_cast<const __m256i*>(weightsBase + offset3)));
            }

            const __m128i bias = _mm_load_si128(reinterpret_cast<const __m128i*>(&biases[i * 4u]));
            // This function adds horizontally 8 values from each sum together, producing 4 int32 values.
            __m128i outVal = m256_haddx4(sum0, sum1, sum2, sum3);
            outVal = _mm_add_epi32(outVal, _mm_set1_epi32(WeightScale / 2)); // divide with rounding to nearest
            outVal = _mm_add_epi32(outVal, bias);
            outVal = _mm_srai_epi32(outVal, WeightScaleShift);
            _mm_store_si128(reinterpret_cast<__m128i*>(&output[i * 4]), outVal);
        }
        return;
    }
#endif // NN_USE_AVX2

#if defined(NN_USE_SSE4)
    constexpr uint32_t registerWidth = 128 / 8;
    const uint32_t numOutChunks = numOutputs / 4u;
    ASSERT(numInputs % registerWidth == 0);
    ASSERT(numOutputs % 4u == 0);
    ASSERT((size_t)weights % 16 == 0);
    ASSERT((size_t)biases % 16 == 0);
    ASSERT((size_t)output % 16 == 0);
    ASSERT((size_t)input % 16 == 0);

    for (uint32_t i = 0; i < numOutChunks; ++i)
    {
        // Prepare weight offsets. One offset for one row of weights.
        // This is a simple index into a 2d array.
        const uint32_t offset0 = (i * 4u + 0u) * numInputs;
        const uint32_t offset1 = (i * 4u + 1u) * numInputs;
        const uint32_t offset2 = (i * 4u + 2u) * numInputs;
        const uint32_t offset3 = (i * 4u + 3u) * numInputs;

        // Accumulation starts from 0, we add the bias only at the end.
        __m128i sum0 = _mm_setzero_si128();
        __m128i sum1 = _mm_setzero_si128();
        __m128i sum2 = _mm_setzero_si128();
        __m128i sum3 = _mm_setzero_si128();

        // Each innermost loop processes a 32x4 chunk of weights, so 128 weights at a time!
        for (uint32_t j = 0; j < numInputs; j += registerWidth)
        {
            // We unroll by 4 so that we can reuse this value, reducing the number of memory operations required.
            const __m128i in = _mm_load_si128(reinterpret_cast<const __m128i*>(input + j));

            // This function processes a 32x1 chunk of int8 and produces a 8x1 chunk of int32.
            const HiddenLayerWeightType* weightsBase = weights + j;
            m128_add_dpbusd_epi32(sum0, in, _mm_load_si128(reinterpret_cast<const __m128i*>(weightsBase + offset0)));
            m128_add_dpbusd_epi32(sum1, in, _mm_load_si128(reinterpret_cast<const __m128i*>(weightsBase + offset1)));
            m128_add_dpbusd_epi32(sum2, in, _mm_load_si128(reinterpret_cast<const __m128i*>(weightsBase + offset2)));
            m128_add_dpbusd_epi32(sum3, in, _mm_load_si128(reinterpret_cast<const __m128i*>(weightsBase + offset3)));
        }

        const __m128i bias = _mm_load_si128(reinterpret_cast<const __m128i*>(&biases[i * 4u]));
        // This function adds horizontally 8 values from each sum together, producing 4 int32 values.
        __m128i outVal = m128_haddx4(sum0, sum1, sum2, sum3);
        outVal = _mm_add_epi32(outVal, _mm_set1_epi32(WeightScale / 2)); // divide with rounding to nearest
        outVal = _mm_add_epi32(outVal, bias);
        outVal = _mm_srai_epi32(outVal, WeightScaleShift);
        _mm_store_si128(reinterpret_cast<__m128i*>(&output[i * 4]), outVal);
    }
#else // no SIMD support
    for (uint32_t i = 0; i < numOutputs; ++i)
    {
        int32_t val = biases[i];
        for (uint32_t j = 0; j < numInputs; ++j)
        {
            val += weights[i * numInputs + j] * (int32_t)input[j];
        }
        // divide with rounding to nearest
        output[i] = (val + (WeightScale / 2)) >> WeightScaleShift;
    }
#endif
}

INLINE static void ClippedReLU_32(uint32_t size, IntermediateType* output, const int32_t* input)
{
#if defined(NN_USE_AVX2)
    if (size >= 32)
    {
        constexpr uint32_t inRegisterWidth = 256 / 32;
        constexpr uint32_t outRegisterWidth = 256 / 8;
        ASSERT(size % outRegisterWidth == 0);
        const uint32_t numOutChunks = size / outRegisterWidth;
        ASSERT((size_t)output % 32 == 0);
        ASSERT((size_t)input % 32 == 0);

        for (uint32_t i = 0; i < numOutChunks; ++i)
        {
            __m256i in0 = _mm256_load_si256(reinterpret_cast<const __m256i*>(input)); input += inRegisterWidth;
            __m256i in1 = _mm256_load_si256(reinterpret_cast<const __m256i*>(input)); input += inRegisterWidth;
            __m256i in2 = _mm256_load_si256(reinterpret_cast<const __m256i*>(input)); input += inRegisterWidth;
            __m256i in3 = _mm256_load_si256(reinterpret_cast<const __m256i*>(input)); input += inRegisterWidth;

            in0 = _mm256_packs_epi32(in0, in1);
            in1 = _mm256_packs_epi32(in2, in3);

            const __m256i result =
                _mm256_permutevar8x32_epi32(
                    // packs saturates to 127, so we only need to clamp from below
                    _mm256_max_epi8(
                        _mm256_packs_epi16(in0, in1),
                        _mm256_setzero_si256()
                    ),
                    _mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0)
                );

            _mm256_store_si256(reinterpret_cast<__m256i*>(output), result);
            output += outRegisterWidth;
        }
        return;
    }
#endif

#if defined(NN_USE_SSE4)
    if (size >= 16)
    {
        constexpr uint32_t inRegisterWidth = 128 / 32;
        constexpr uint32_t outRegisterWidth = 128 / 8;
        ASSERT(size % outRegisterWidth == 0);
        const uint32_t numOutChunks = size / outRegisterWidth;
        ASSERT((size_t)output % 16 == 0);
        ASSERT((size_t)input % 16 == 0);

        for (uint32_t i = 0; i < numOutChunks; ++i)
        {
            __m128i in0 = _mm_load_si128(reinterpret_cast<const __m128i*>(input)); input += inRegisterWidth;
            __m128i in1 = _mm_load_si128(reinterpret_cast<const __m128i*>(input)); input += inRegisterWidth;
            __m128i in2 = _mm_load_si128(reinterpret_cast<const __m128i*>(input)); input += inRegisterWidth;
            __m128i in3 = _mm_load_si128(reinterpret_cast<const __m128i*>(input)); input += inRegisterWidth;

            in0 = _mm_packs_epi32(in0, in1);
            in1 = _mm_packs_epi32(in2, in3);

            // packs saturates to 127, so we only need to clamp from below
            const __m128i result = _mm_max_epi8(_mm_packs_epi16(in0, in1), _mm_setzero_si128());

            _mm_store_si128(reinterpret_cast<__m128i*>(output), result);
            output += outRegisterWidth;
        }
        return;
    }
#endif

    for (uint32_t i = 0; i < size; ++i)
    {
        output[i] = (IntermediateType)std::clamp<int32_t>(input[i], 0, std::numeric_limits<IntermediateType>::max());
    }
}

INLINE static int32_t LinearLayer_SingleOutput(
    const LastLayerWeightType* weights, const LastLayerBiasType* biases,
    uint32_t numInputs, const IntermediateType* input)
{
    int32_t val = biases[0];

#if defined(NN_USE_AVX2)
    if (numInputs >= 16)
    {
        constexpr uint32_t registerWidth = 16;
        ASSERT(numInputs % registerWidth == 0);
        ASSERT((size_t)weights % 32 == 0);
        ASSERT((size_t)biases % 32 == 0);

        __m256i sum = _mm256_setzero_si256();
        for (uint32_t j = 0; j < numInputs; j += registerWidth)
        {
            // load 8bit inputs and expand to 16bit values
            const __m256i in = _mm256_cvtepi8_epi16(_mm_load_si128(reinterpret_cast<const __m128i*>(input + j)));

            // perform 16bit x 16bit multiplication and accumulate to 32bit registers
            const __m256i w = _mm256_load_si256(reinterpret_cast<const __m256i*>(weights + j));
#ifdef NN_USE_VNNI
            sum = _mm256_dpwssd_epi32(sum, in, w);
#else
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(in, w));
#endif // NN_USE_VNNI
        }

        // add 8 int32s horizontally
        val += m256_hadd(sum);

        // divide with rounding to nearest
        return (val + (WeightScale / 2)) >> WeightScaleShift;
    }
#endif // NN_USE_AVX2

#if defined(NN_USE_SSE4)
    constexpr uint32_t registerWidth = 8;
    ASSERT(numInputs % registerWidth == 0);
    ASSERT((size_t)weights % 16 == 0);
    ASSERT((size_t)biases % 16 == 0);

    __m128i sum = _mm_setzero_si128();
    for (uint32_t j = 0; j < numInputs; j += registerWidth)
    {
        // load 8bit inputs and expand to 16bit values
        const __m128i in = _mm_loadu_si64(reinterpret_cast<const __m128i*>(input + j));
        const __m128i in16 = _mm_unpacklo_epi8(in, _mm_cmplt_epi8(in, _mm_setzero_si128()));

        // perform 16bit x 16bit multiplication and accumulate to 32bit registers
        const __m128i w = _mm_load_si128(reinterpret_cast<const __m128i*>(weights + j));
        sum = _mm_add_epi32(sum, _mm_madd_epi16(in16, w));
    }

    // add 4 int32s horizontally
    val += m128_hadd(sum);
#else
    for (uint32_t i = 0; i < numInputs; ++i)
    {
        val += (int32_t)input[i] * (int32_t)weights[i];
    }
#endif

    // divide with rounding to nearest
    return (val + (WeightScale / 2)) >> WeightScaleShift;
}

INLINE static int32_t LinearLayer_Accum_SingleOutput(
    const LastLayerWeightType* weights, const LastLayerBiasType* biases,
    const AccumulatorType* inputA, const AccumulatorType* inputB)
{
    int32_t val = biases[0];

#if defined(NN_USE_AVX512)
    constexpr uint32_t registerWidth = 32;
    ASSERT((size_t)weights % (2 * registerWidth) == 0);
    ASSERT((size_t)biases % (2 * registerWidth) == 0);

    // unroll 2x so two sums can be calculated independently
    __m512i sumA = _mm512_setzero_si512();
    __m512i sumB = _mm512_setzero_si512();
    for (uint32_t j = 0; j < AccumulatorSize; j += registerWidth)
    {
        __m512i inA = Int16VecLoad(inputA + j);
        __m512i inB = Int16VecLoad(inputB + j);

        // apply clipped-ReLU
        inA = _mm512_min_epi16(_mm512_max_epi16(inA, _mm512_setzero_si512()), _mm512_set1_epi16(127));
        inB = _mm512_min_epi16(_mm512_max_epi16(inB, _mm512_setzero_si512()), _mm512_set1_epi16(127));

        // perform 16bit x 16bit multiplication and accumulate to 32bit registers
        const __m512i wA = Int16VecLoad(weights + j);
        const __m512i wB = Int16VecLoad(weights + j + AccumulatorSize);
        sumA = _mm512_add_epi32(sumA, _mm512_madd_epi16(inA, wA));
        sumB = _mm512_add_epi32(sumB, _mm512_madd_epi16(inB, wB));
    }

    // add 16 int32s horizontally
    val += m512_hadd(_mm512_add_epi32(sumA, sumB));

#elif defined(NN_USE_AVX2)
    constexpr uint32_t registerWidth = 16;
    ASSERT((size_t)weights % (2 * registerWidth) == 0);
    ASSERT((size_t)biases % (2 * registerWidth) == 0);

    // unroll 2x so two sums can be calculated independently
    __m256i sumA = _mm256_setzero_si256();
    __m256i sumB = _mm256_setzero_si256();
    for (uint32_t j = 0; j < AccumulatorSize; j += registerWidth)
    {
        __m256i inA = _mm256_load_si256(reinterpret_cast<const __m256i*>(inputA + j));
        __m256i inB = _mm256_load_si256(reinterpret_cast<const __m256i*>(inputB + j));

        // apply clipped-ReLU
        inA = _mm256_min_epi16(_mm256_max_epi16(inA, _mm256_setzero_si256()), _mm256_set1_epi16(127));
        inB = _mm256_min_epi16(_mm256_max_epi16(inB, _mm256_setzero_si256()), _mm256_set1_epi16(127));

        // perform 16bit x 16bit multiplication and accumulate to 32bit registers
        const __m256i wA = _mm256_load_si256(reinterpret_cast<const __m256i*>(weights + j));
        const __m256i wB = _mm256_load_si256(reinterpret_cast<const __m256i*>(weights + j + AccumulatorSize));
#ifdef NN_USE_VNNI
        sumA = _mm256_dpwssd_epi32(sumA, inA, wA);
        sumB = _mm256_dpwssd_epi32(sumB, inB, wB);
#else
        sumA = _mm256_add_epi32(sumA, _mm256_madd_epi16(inA, wA));
        sumB = _mm256_add_epi32(sumB, _mm256_madd_epi16(inB, wB));
#endif // NN_USE_VNNI
    }

    // add 8 int32s horizontally
    val += m256_hadd(_mm256_add_epi32(sumA, sumB));

#elif defined(NN_USE_SSE4)
    constexpr uint32_t registerWidth = 8;
    static_assert(AccumulatorSize % registerWidth == 0, "");
    ASSERT((size_t)weights % (2 * registerWidth) == 0);
    ASSERT((size_t)biases % (2 * registerWidth) == 0);

    // unroll 2x so two sums can be calculated independently
    __m128i sumA = _mm_setzero_si128();
    __m128i sumB = _mm_setzero_si128();
    for (uint32_t j = 0; j < AccumulatorSize; j += registerWidth)
    {
        __m128i inA = _mm_load_si128(reinterpret_cast<const __m128i*>(inputA + j));
        __m128i inB = _mm_load_si128(reinterpret_cast<const __m128i*>(inputB + j));

        // apply clipped-ReLU
        inA = _mm_min_epi16(_mm_max_epi16(inA, _mm_setzero_si128()), _mm_set1_epi16(127));
        inB = _mm_min_epi16(_mm_max_epi16(inB, _mm_setzero_si128()), _mm_set1_epi16(127));

        // perform 16bit x 16bit multiplication and accumulate to 32bit registers
        const __m128i wA = _mm_load_si128(reinterpret_cast<const __m128i*>(weights + j));
        const __m128i wB = _mm_load_si128(reinterpret_cast<const __m128i*>(weights + j + AccumulatorSize));
        sumA = _mm_add_epi32(sumA, _mm_madd_epi16(inA, wA));
        sumB = _mm_add_epi32(sumB, _mm_madd_epi16(inB, wB));
    }

    // add 8 int32s horizontally
    val += m128_hadd(_mm_add_epi32(sumA, sumB));

#elif defined(NN_USE_ARM_NEON)

    constexpr uint32_t registerWidth = 8;
    static_assert(AccumulatorSize % registerWidth == 0, "");
    ASSERT((size_t)weights % (2 * registerWidth) == 0);
    ASSERT((size_t)biases % (2 * registerWidth) == 0);

    int32x4_t sumA = vdupq_n_s32(0);
    int32x4_t sumB = vdupq_n_s32(0);
    int32x4_t sumC = vdupq_n_s32(0);
    int32x4_t sumD = vdupq_n_s32(0);
    for (uint32_t j = 0; j < AccumulatorSize; j += registerWidth)
    {
        // load 8 16bit inputs
        int16x8_t inA = vld1q_s16(inputA + j);
        int16x8_t inB = vld1q_s16(inputB + j);

        // apply clipped-ReLU
        inA = vminq_s16(vmaxq_s16(inA, vdupq_n_s16(0)), vdupq_n_s16(127));
        inB = vminq_s16(vmaxq_s16(inB, vdupq_n_s16(0)), vdupq_n_s16(127));

        // load 8 16bit weights
        const int16x8_t wA = vld1q_s16(weights + j);
        const int16x8_t wB = vld1q_s16(weights + j + AccumulatorSize);

        // perform 16bit x 16bit multiplication and accumulate to 32bit registers
        sumA = vaddq_s32(sumA, vmull_s16(vget_low_s16(wA), vget_low_s16(inA)));
        sumB = vaddq_s32(sumB, vmull_high_s16(wA, inA));
        sumC = vaddq_s32(sumC, vmull_s16(vget_low_s16(wB), vget_low_s16(inB)));
        sumD = vaddq_s32(sumD, vmull_high_s16(wB, inB));
    }

    // add int32s horizontally
    val += vaddvq_s32(vaddq_s32(vaddq_s32(sumA, sumB), vaddq_s32(sumC, sumD)));

#else
    for (uint32_t i = 0; i < AccumulatorSize; ++i)
    {
        const AccumulatorType in = std::clamp<AccumulatorType>(inputA[i], 0, std::numeric_limits<IntermediateType>::max());
        val += (int32_t)in * (int32_t)weights[i];
    }
    for (uint32_t i = 0; i < AccumulatorSize; ++i)
    {
        const AccumulatorType in = std::clamp<AccumulatorType>(inputB[i], 0, std::numeric_limits<IntermediateType>::max());
        val += (int32_t)in * (int32_t)weights[i + AccumulatorSize];
    }
#endif

    // divide with rounding to nearest
    return (val + (WeightScale / 2)) >> WeightScaleShift;
}

///

PackedNeuralNetwork::PackedNeuralNetwork()
{
}

PackedNeuralNetwork::~PackedNeuralNetwork()
{
    Release();
}

void PackedNeuralNetwork::Release()
{
    if (mappedData)
    {
        ReleaseFileMapping();
        weightsBuffer = nullptr;
    }

    if (weightsBuffer)
    {
        AlignedFree(weightsBuffer);
        weightsBuffer = nullptr;
    }

    header = Header{};
}

size_t PackedNeuralNetwork::GetWeightsBufferSize() const
{
    return layerDataSizes[0] + layerDataSizes[1] + layerDataSizes[2] + layerDataSizes[3];
}

bool PackedNeuralNetwork::Resize(const std::vector<uint32_t>& layerSizes,
                                 const std::vector<uint32_t>& numVariantsPerLayer)
{
    Release();

    if (layerSizes.size() < 2 || layerSizes.size() > MaxNumLayers)
    {
        return false;
    }

    header.magic = MagicNumber;
    header.version = CurrentVersion;

    for (size_t i = 0; i < layerSizes.size(); ++i)
    {
        header.layerSizes[i] = layerSizes[i];
        header.layerVariants[i] = i < numVariantsPerLayer.size() ? numVariantsPerLayer[i] : 1;
    }
    numActiveLayers = (uint32_t)layerSizes.size();

    InitLayerDataSizes();

    const size_t weightsSize = GetWeightsBufferSize();
    weightsBuffer = (uint8_t*)AlignedMalloc(weightsSize, CACHELINE_SIZE);

    InitLayerDataPointers();

    if (!weightsBuffer)
    {
        Release();
        std::cerr << "Failed to allocate weights buffer" << std::endl;
        return false;
    }

    return true;
}

void PackedNeuralNetwork::InitLayerDataSizes()
{
    ASSERT(numActiveLayers >= 2);

    memset(layerDataSizes, 0, sizeof(layerDataSizes));

    layerDataSizes[0] = header.layerVariants[0] * RoundUp<uint32_t, CACHELINE_SIZE>(
        (header.layerSizes[0] * (header.layerSizes[1] / 2) * sizeof(FirstLayerWeightType) +
        (header.layerSizes[1] / 2) * sizeof(FirstLayerBiasType)));
    ASSERT(layerDataSizes[0] > 0);

    for (uint32_t i = 1; i + 1 < numActiveLayers; ++i)
    {
        layerDataSizes[i] = header.layerVariants[i] * RoundUp<uint32_t, CACHELINE_SIZE>(
            (header.layerSizes[i] * header.layerSizes[i+1] * sizeof(HiddenLayerWeightType) +
             header.layerSizes[i+1] * sizeof(HiddenLayerBiasType)));
        ASSERT(layerDataSizes[i] > 0);
    }

    layerDataSizes[numActiveLayers-1] = header.layerVariants[numActiveLayers-1] * RoundUp<uint32_t, CACHELINE_SIZE>(
        (header.layerSizes[numActiveLayers-1] * OutputSize * sizeof(LastLayerWeightType) +
        OutputSize * sizeof(LastLayerBiasType)));
    ASSERT(layerDataSizes[numActiveLayers-1]);
}

void PackedNeuralNetwork::InitLayerDataPointers()
{
    ASSERT(numActiveLayers >= 2);

    memset(layerDataPointers, 0, sizeof(layerDataPointers));

    ASSERT((size_t)weightsBuffer % CACHELINE_SIZE == 0);
    layerDataPointers[0] = weightsBuffer;

    for (uint32_t i = 1; i + 1 < numActiveLayers; ++i)
    {
        ASSERT(layerDataSizes[i - 1] > 0);
        layerDataPointers[i] = layerDataPointers[i - 1] + layerDataSizes[i - 1];
        ASSERT((size_t)layerDataPointers[i] % CACHELINE_SIZE == 0);
    }

    ASSERT(layerDataSizes[numActiveLayers - 2] > 0);
    layerDataPointers[numActiveLayers - 1] = layerDataPointers[numActiveLayers - 2] + layerDataSizes[numActiveLayers - 2];
    ASSERT((size_t)layerDataPointers[numActiveLayers - 1] % CACHELINE_SIZE == 0);
}

void PackedNeuralNetwork::GetLayerWeightsAndBiases(uint32_t layerIndex, uint32_t layerVariant, const void*& outWeights, const void*& outBiases) const
{
    ASSERT(layerIndex < MaxNumLayers);
    ASSERT(layerVariant < header.layerVariants[layerIndex]);
    ASSERT(header.layerSizes[layerIndex] > 0);

    size_t weightSize = sizeof(HiddenLayerWeightType);
    size_t biasSize = sizeof(HiddenLayerBiasType);

    if (layerIndex == 0)
    {
        weightSize = sizeof(FirstLayerWeightType);
        biasSize = sizeof(FirstLayerBiasType);
    }
    else if (layerIndex + 1 == numActiveLayers)
    {
        weightSize = sizeof(LastLayerWeightType);
        biasSize = sizeof(LastLayerBiasType);
    }

    const size_t nextLayerSize = (layerIndex + 1 < numActiveLayers) ? header.layerSizes[layerIndex + 1] : 1;
    const size_t weightsBlockSize = weightSize * (size_t)header.layerSizes[layerIndex] * nextLayerSize;
    const size_t biasesBlockSize = biasSize * nextLayerSize;
    ASSERT(weightsBlockSize > 0);
    ASSERT(biasesBlockSize > 0);

    uint8_t* basePointer = layerDataPointers[layerIndex];
    ASSERT(basePointer != nullptr);

    uint8_t* weightsPointer = basePointer + layerVariant * RoundUp<size_t,CACHELINE_SIZE>(weightsBlockSize + biasesBlockSize);
    uint8_t* biasesPointer = weightsPointer + weightsBlockSize;

    outWeights = weightsPointer;
    outBiases = biasesPointer;
}

bool PackedNeuralNetwork::Save(const char* filePath) const
{
    if (!IsValid())
    {
        std::cerr << "Failed to save neural network: " << "invalid network" << std::endl;
        return false;
    }

    FILE* file = fopen(filePath, "wb");
    if (!file)
    {
        std::cerr << "Failed to save neural network: " << "cannot open file" << std::endl;
        return false;
    }

    if (1 != fwrite(&header, sizeof(Header), 1, file))
    {
        fclose(file);
        std::cerr << "Failed to save neural network: " << "cannot write header" << std::endl;
        return false;
    }
    
    if (1 != fwrite(weightsBuffer, GetWeightsBufferSize(), 1, file))
    {
        fclose(file);
        std::cerr << "Failed to save neural network: " << "cannot write weights" << std::endl;
        return false;
    }

    fclose(file);
    return true;
}

void PackedNeuralNetwork::ReleaseFileMapping()
{
#if defined(PLATFORM_WINDOWS)
    if (fileMapping == INVALID_HANDLE_VALUE)
    {
        CloseHandle(fileMapping);
        fileMapping = INVALID_HANDLE_VALUE;
    }

    if (fileHandle == INVALID_HANDLE_VALUE)
    {
        CloseHandle(fileHandle);
        fileHandle = INVALID_HANDLE_VALUE;
    }
#else
    if (mappedData)
    {
        if (0 != munmap(mappedData, mappedSize))
        {
            perror("munmap");
        }
    }

    if (fileDesc != -1)
    {
        close(fileDesc);
        fileDesc = -1;
    }
#endif // PLATFORM_WINDOWS

    mappedData = nullptr;
    mappedSize = 0;
}

bool PackedNeuralNetwork::Load(const char* filePath)
{
    Release();

#if defined(PLATFORM_WINDOWS)

    DWORD sizeLow = 0, sizeHigh = 0;
    
    // open file
    {
#ifdef _UNICODE
        wchar_t wideFilePath[4096];
        size_t len = 0;
        mbstowcs_s(&len, wideFilePath, 4096, filePath, _TRUNCATE);
        fileHandle = ::CreateFile(wideFilePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
#else
        fileHandle = ::CreateFile(filePath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
#endif

        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            fprintf(stderr, "CreateFile() failed, error = %lu.\n", GetLastError());
            goto onError;
        }
    }
    
    sizeLow = ::GetFileSize(fileHandle, &sizeHigh);
    fileMapping = ::CreateFileMapping(fileHandle, NULL, PAGE_READONLY, sizeHigh, sizeLow, NULL);
    if (fileMapping == INVALID_HANDLE_VALUE)
    {
        fprintf(stderr, "CreateFileMapping() failed, error = %lu.\n", GetLastError());
        goto onError;
    }

    mappedSize = (uint64_t)sizeLow + ((uint64_t)sizeHigh << 32);
    mappedData = (void*)MapViewOfFile(fileMapping, FILE_MAP_READ, 0, 0, 0);
    if (mappedData == nullptr)
    {
        fprintf(stderr, "MapViewOfFile() failed, error = %lu.\n", GetLastError());
        goto onError;
    }

#else

    fileDesc = open(filePath, O_RDONLY);
    if (fileDesc == -1)
    {
        perror("open");
        goto onError;
    }

    struct stat statbuf;
    if (fstat(fileDesc, &statbuf))
    {
        perror("fstat");
        goto onError;
    }

    mappedSize = statbuf.st_size;
    mappedData = mmap(NULL, statbuf.st_size, PROT_READ, MAP_SHARED, fileDesc, 0);
    if (mappedData == MAP_FAILED)
    {
        perror("mmap");
        goto onError;
    }

#endif // PLATFORM_WINDOWS

    memcpy(&header, mappedData, sizeof(Header));

    if (header.magic != MagicNumber)
    {
        std::cerr << "Failed to load neural network: " << "invalid magic" << std::endl;
        goto onError;
    }

    if (header.version != CurrentVersion)
    {
        std::cerr << "Failed to load neural network: " << "unsupported version" << std::endl;
        goto onError;
    }

    if (header.layerSizes[0] == 0 || header.layerSizes[0] > MaxInputs)
    {
        std::cerr << "Failed to load neural network: " << "invalid number of inputs" << std::endl;
        goto onError;
    }

    if (header.layerSizes[1] == 0 || header.layerSizes[1] / 2 != AccumulatorSize)
    {
        std::cerr << "Failed to load neural network: " << "invalid first layer size" << std::endl;
        goto onError;
    }

    numActiveLayers = 0;
    for (uint32_t i = 0; i < MaxNumLayers; ++i)
    {
        if (header.layerSizes[i] == 0) break;

        // handle pre-variants format
        if (header.layerVariants[i] == 0) header.layerVariants[i] = 1;

        if (header.layerVariants[i] != 1 && header.layerVariants[i] != NumVariants)
        {
            std::cerr << "Failed to load neural network: " << "unexpected number of variants" << std::endl;
            goto onError;
        }

        numActiveLayers = i + 1;
    }

    if (numActiveLayers < 2)
    {
        std::cerr << "Failed to load neural network: " << "invalid number of layers" << std::endl;
        goto onError;
    }

    weightsBuffer = (uint8_t*)mappedData + sizeof(Header);

    InitLayerDataSizes();
    InitLayerDataPointers();

    if (sizeof(Header) + GetWeightsBufferSize() > mappedSize)
    {
        std::cerr << "Failed to load neural network: " << "file is too small" << std::endl;
        goto onError;
    }

    return true;

onError:
    Release();
    return false;
}

int32_t PackedNeuralNetwork::Run(const Accumulator& stmAccum, const Accumulator& nstmAccum, uint32_t variant) const
{
    ASSERT(numActiveLayers > 1);
    ASSERT(GetAccumulatorSize() == AccumulatorSize);
    ASSERT(GetLayerSize(2) <= MaxNeuronsInHiddenLayers);
    ASSERT(GetLayerSize(3) <= MaxNeuronsInHiddenLayers);

    //if (numActiveLayers == 2)
    {
        constexpr uint32_t lastLayerIdx = 1;

        const void* weights;
        const void* biases;
        GetLayerWeightsAndBiases(lastLayerIdx, variant, weights, biases);

        return LinearLayer_Accum_SingleOutput(
            reinterpret_cast<const LastLayerWeightType*>(weights),
            reinterpret_cast<const LastLayerBiasType*>(biases),
            stmAccum.values,
            nstmAccum.values);
    }
    /*
    else
    {
        alignas(CACHELINE_SIZE) IntermediateType tempA[AccumulatorSize];
        alignas(CACHELINE_SIZE) int32_t tempB[MaxNeuronsInHiddenLayers];

        // apply activation function on the accumulator
        ClippedReLU_Accum(GetLayerSize(1), tempA, accumulator.values);

        // hidden layers
        for (uint32_t i = 1; i + 1 < numActiveLayers; ++i)
        {
            const uint32_t thisLayerSize = GetLayerSize(i);
            const uint32_t nextLayerSize = GetLayerSize(i + 1);

            const HiddenLayerWeightType* weights = GetLayerWeights<HiddenLayerWeightType>(i, variant);
            const HiddenLayerBiasType* biases = GetLayerBiases<HiddenLayerBiasType>(i, variant);

            LinearLayer(weights, biases, thisLayerSize, nextLayerSize, tempB, tempA);
            ClippedReLU_32(nextLayerSize, tempA, tempB);
        }

        const uint32_t lastLayerIdx = numActiveLayers - 1;
        const LastLayerWeightType* weights = GetLayerWeights<LastLayerWeightType>(lastLayerIdx, variant);
        const LastLayerBiasType* biases = GetLayerBiases<LastLayerBiasType>(lastLayerIdx, variant);
        return LinearLayer_SingleOutput(weights, biases, GetLayerSize(lastLayerIdx), tempA);
    }
    */
}

int32_t PackedNeuralNetwork::Run(const uint16_t* stmFeatures, const uint32_t stmNumFeatures, const uint16_t* nstmFeatures, const uint32_t nstmNumFeatures, uint32_t variant) const
{
    Accumulator stmAccum;
    stmAccum.Refresh(GetAccumulatorWeights(), GetAccumulatorBiases(), stmNumFeatures, stmFeatures);

    Accumulator nstmAccum;
    nstmAccum.Refresh(GetAccumulatorWeights(), GetAccumulatorBiases(), nstmNumFeatures, nstmFeatures);

    return Run(stmAccum, nstmAccum, variant);
}

} // namespace nn
