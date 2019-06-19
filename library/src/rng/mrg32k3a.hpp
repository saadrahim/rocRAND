// Copyright (c) 2017 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCRAND_RNG_MRG32K3A_H_
#define ROCRAND_RNG_MRG32K3A_H_

#include <algorithm>
#include <hip/hip_runtime.h>

#include <rocrand/rocrand.h>
#include <rocrand/rocrand_mrg32k3a_precomputed.h>

#include "generator_type.hpp"
#include "device_engines.hpp"
#include "distributions.hpp"

// MRG32K3A constants
#ifndef ROCRAND_MRG32K3A_NORM_DOUBLE
#define ROCRAND_MRG32K3A_NORM_DOUBLE (2.3283065498378288e-10) // 1/ROCRAND_MRG32K3A_M1
#endif
#ifndef ROCRAND_MRG32K3A_UINT_NORM
#define ROCRAND_MRG32K3A_UINT_NORM (1.000000048661606966) // ROCRAND_MRG32K3A_POW32/ROCRAND_MRG32K3A_M1
#endif

namespace rocrand_host {
namespace detail {

    typedef ::rocrand_device::mrg32k3a_engine mrg32k3a_device_engine;

    __global__
    void init_engines_kernel(mrg32k3a_device_engine * engines,
                             unsigned long long seed,
                             unsigned long long offset)
    {
        const unsigned int engine_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        engines[engine_id] = mrg32k3a_device_engine(seed, engine_id, offset);
    }

    template<class T, class Distribution>
    __global__
    void generate_kernel(mrg32k3a_device_engine * engines,
                         T * data, const size_t n,
                         Distribution distribution)
    {
        constexpr unsigned int input_width = Distribution::input_width;
        constexpr unsigned int output_width = Distribution::output_width;

        using vec_type = aligned_vec_type<T, output_width>;

        const unsigned int engine_id = hipBlockIdx_x * hipBlockDim_x + hipThreadIdx_x;
        const unsigned int stride = hipGridDim_x * hipBlockDim_x;
        size_t index = engine_id;

        // Load device engine
        mrg32k3a_device_engine engine = engines[engine_id];

        unsigned int input[input_width];
        T output[output_width];

        const uintptr_t uintptr = reinterpret_cast<uintptr_t>(data);
        const size_t misalignment =
            (
                output_width - uintptr / sizeof(T) % output_width
            ) % output_width;
        const unsigned int head_size = min(n, misalignment);
        const unsigned int tail_size = (n - head_size) % output_width;
        const size_t vec_n = (n - head_size) / output_width;

        vec_type * vec_data = reinterpret_cast<vec_type *>(data + misalignment);
        while(index < vec_n)
        {
            for(unsigned int i = 0; i < input_width; i++)
            {
                input[i] = engine();
            }
            distribution(input, output);

            vec_data[index] = *reinterpret_cast<vec_type *>(output);
            // Next position
            index += stride;
        }

        // Check if we need to save head and tail.
        // Those numbers should be generated by the thread that would
        // save next vec_type.
        if(output_width > 1 && index == vec_n)
        {
            // If data is not aligned by sizeof(vec_type)
            if(head_size > 0)
            {
                for(unsigned int i = 0; i < input_width; i++)
                {
                    input[i] = engine();
                }
                distribution(input, output);

                for(unsigned int o = 0; o < output_width; o++)
                {
                    if(o < head_size)
                    {
                        data[o] = output[o];
                    }
                }
            }

            if(tail_size > 0)
            {
                for(unsigned int i = 0; i < input_width; i++)
                {
                    input[i] = engine();
                }
                distribution(input, output);

                for(unsigned int o = 0; o < output_width; o++)
                {
                    if(o < tail_size)
                    {
                        data[n - tail_size + o] = output[o];
                    }
                }
            }
        }

        // Save engine with its state
        engines[engine_id] = engine;
    }

    template<class T>
    struct mrg_uniform_distribution;

    template<>
    struct mrg_uniform_distribution<unsigned int>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 1;

        __device__
        void operator()(const unsigned int (&input)[1], unsigned int (&output)[1]) const
        {
            unsigned int v = static_cast<unsigned int>(input[0] * ROCRAND_MRG32K3A_UINT_NORM);
            output[0] = v;
        }
    };

    template<>
    struct mrg_uniform_distribution<unsigned char>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 4;

        __device__
        void operator()(const unsigned int (&input)[1], unsigned char (&output)[4]) const
        {
            unsigned int v = static_cast<unsigned int>(input[0] * ROCRAND_MRG32K3A_UINT_NORM);
            *reinterpret_cast<unsigned int *>(output) = v;
        }
    };

    template<>
    struct mrg_uniform_distribution<unsigned short>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 2;

        __device__
        void operator()(const unsigned int (&input)[1], unsigned short (&output)[2]) const
        {
            unsigned int v = static_cast<unsigned int>(input[0] * ROCRAND_MRG32K3A_UINT_NORM);
            *reinterpret_cast<unsigned int *>(output) = v;
        }
    };

    template<>
    struct mrg_uniform_distribution<float>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 1;

        __device__
        void operator()(const unsigned int (&input)[1], float (&output)[1]) const
        {
            output[0] = rocrand_device::detail::mrg_uniform_distribution(input[0]);
        }
    };

    template<>
    struct mrg_uniform_distribution<double>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 1;

        __device__
        void operator()(const unsigned int (&input)[1], double (&output)[1]) const
        {
            output[0] = rocrand_device::detail::mrg_uniform_distribution_double(input[0]);
        }
    };

    template<>
    struct mrg_uniform_distribution<__half>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 2;

        __device__
        void operator()(const unsigned int (&input)[1], __half (&output)[2]) const
        {
            unsigned int v = static_cast<unsigned int>(input[0] * ROCRAND_MRG32K3A_UINT_NORM);
            output[0] = uniform_distribution_half(static_cast<short>(v));
            output[1] = uniform_distribution_half(static_cast<short>(v >> 16));
        }
    };

    template<class T>
    struct mrg_normal_distribution;

    template<>
    struct mrg_normal_distribution<float>
    {
        static constexpr unsigned int input_width = 2;
        static constexpr unsigned int output_width = 2;

        const float mean;
        const float stddev;

        __host__ __device__
        mrg_normal_distribution(float mean, float stddev)
            : mean(mean), stddev(stddev) {}

        __device__
        void operator()(const unsigned int (&input)[2], float (&output)[2]) const
        {
            float2 v = rocrand_device::detail::mrg_normal_distribution2(input[0], input[1]);
            output[0] = mean + v.x * stddev;
            output[1] = mean + v.y * stddev;
        }
    };

    template<>
    struct mrg_normal_distribution<double>
    {
        static constexpr unsigned int input_width = 2;
        static constexpr unsigned int output_width = 2;

        const double mean;
        const double stddev;

        __host__ __device__
        mrg_normal_distribution(double mean, double stddev)
            : mean(mean), stddev(stddev) {}

        __device__
        void operator()(const unsigned int (&input)[2], double (&output)[2]) const
        {
            double2 v = rocrand_device::detail::mrg_normal_distribution_double2(input[0], input[1]);
            output[0] = mean + v.x * stddev;
            output[1] = mean + v.y * stddev;
        }
    };

    template<>
    struct mrg_normal_distribution<__half>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 2;

        const __half mean;
        const __half stddev;

        __host__ __device__
        mrg_normal_distribution(__half mean, __half stddev)
            : mean(mean), stddev(stddev) {}

        __device__
        void operator()(const unsigned int (&input)[1], __half (&output)[2]) const
        {
            unsigned int a = static_cast<unsigned int>(input[0] * ROCRAND_MRG32K3A_UINT_NORM);
            rocrand_half2 v = mrg_box_muller_half(
                uniform_distribution_half(static_cast<short>(a)),
                uniform_distribution_half(static_cast<short>(a >> 16))
            );
            #if defined(__HIP_PLATFORM_HCC__) || ((__CUDA_ARCH__ >= 530) && defined(__HIP_PLATFORM_NVCC__))
            output[0] = __hadd(mean, __hmul(v.x, stddev));
            output[1] = __hadd(mean, __hmul(v.y, stddev));
            #else
            output[0] = __float2half(__half2float(mean) + (__half2float(stddev) * __half2float(v.x)));
            output[1] = __float2half(__half2float(mean) + (__half2float(stddev) * __half2float(v.y)));
            #endif
        }
    };

    template<class T>
    struct mrg_log_normal_distribution;

    template<>
    struct mrg_log_normal_distribution<float>
    {
        static constexpr unsigned int input_width = 2;
        static constexpr unsigned int output_width = 2;

        const float mean;
        const float stddev;

        __host__ __device__
        mrg_log_normal_distribution(float mean, float stddev)
            : mean(mean), stddev(stddev) {}

        __device__
        void operator()(const unsigned int (&input)[2], float (&output)[2]) const
        {
            float2 v = rocrand_device::detail::mrg_normal_distribution2(input[0], input[1]);
            output[0] = expf(mean + v.x * stddev);
            output[1] = expf(mean + v.y * stddev);
        }
    };

    template<>
    struct mrg_log_normal_distribution<double>
    {
        static constexpr unsigned int input_width = 2;
        static constexpr unsigned int output_width = 2;

        const double mean;
        const double stddev;

        __host__ __device__
        mrg_log_normal_distribution(double mean, double stddev)
            : mean(mean), stddev(stddev) {}

        __device__
        void operator()(const unsigned int (&input)[2], double (&output)[2]) const
        {
            double2 v = rocrand_device::detail::mrg_normal_distribution_double2(input[0], input[1]);
            output[0] = exp(mean + v.x * stddev);
            output[1] = exp(mean + v.y * stddev);
        }
    };

    template<>
    struct mrg_log_normal_distribution<__half>
    {
        static constexpr unsigned int input_width = 1;
        static constexpr unsigned int output_width = 2;

        const __half mean;
        const __half stddev;

        __host__ __device__
        mrg_log_normal_distribution(__half mean, __half stddev)
            : mean(mean), stddev(stddev) {}

        __device__
        void operator()(const unsigned int (&input)[1], __half (&output)[2]) const
        {
            unsigned int a = static_cast<unsigned int>(input[0] * ROCRAND_MRG32K3A_UINT_NORM);
            rocrand_half2 v = mrg_box_muller_half(
                uniform_distribution_half(static_cast<short>(a)),
                uniform_distribution_half(static_cast<short>(a >> 16))
            );
            #if defined(__HIP_PLATFORM_HCC__) || ((__CUDA_ARCH__ >= 530) && defined(__HIP_PLATFORM_NVCC__))
            output[0] = hexp(__hadd(mean, __hmul(v.x, stddev)));
            output[1] = hexp(__hadd(mean, __hmul(v.y, stddev)));
            #else
            output[0] = __float2half(expf(__half2float(mean) + (__half2float(stddev) * __half2float(v.x))));
            output[1] = __float2half(expf(__half2float(mean) + (__half2float(stddev) * __half2float(v.y))));
            #endif
        }
    };

} // end namespace detail
} // end namespace rocrand_host

class rocrand_mrg32k3a : public rocrand_generator_type<ROCRAND_RNG_PSEUDO_MRG32K3A>
{
public:
    using base_type = rocrand_generator_type<ROCRAND_RNG_PSEUDO_MRG32K3A>;
    using engine_type = ::rocrand_host::detail::mrg32k3a_device_engine;

    rocrand_mrg32k3a(unsigned long long seed = 0,
                     unsigned long long offset = 0,
                     hipStream_t stream = 0)
        : base_type(seed, offset, stream),
          m_engines_initialized(false), m_engines(NULL), m_engines_size(s_threads * s_blocks)
    {
        // Allocate device random number engines
        auto error = hipMalloc(&m_engines, sizeof(engine_type) * m_engines_size);
        if(error != hipSuccess)
        {
            throw ROCRAND_STATUS_ALLOCATION_FAILED;
        }
        if(m_seed == 0)
        {
            m_seed = ROCRAND_MRG32K3A_DEFAULT_SEED;
        }
    }

    ~rocrand_mrg32k3a()
    {
        hipFree(m_engines);
    }

    void reset()
    {
        m_engines_initialized = false;
    }

    /// Changes seed to \p seed and resets generator state.
    ///
    /// New seed value should not be zero. If \p seed_value is equal
    /// zero, value \p ROCRAND_MRG32K3A_DEFAULT_SEED is used instead.
    void set_seed(unsigned long long seed)
    {
        if(seed == 0)
        {
            seed = ROCRAND_MRG32K3A_DEFAULT_SEED;
        }
        m_seed = seed;
        m_engines_initialized = false;
    }

    void set_offset(unsigned long long offset)
    {
        m_offset = offset;
        m_engines_initialized = false;
    }

    rocrand_status init()
    {
        if (m_engines_initialized)
            return ROCRAND_STATUS_SUCCESS;

        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(rocrand_host::detail::init_engines_kernel),
            dim3(s_blocks), dim3(s_threads), 0, m_stream,
            m_engines, m_seed, m_offset
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        m_engines_initialized = true;

        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T, class Distribution = rocrand_host::detail::mrg_uniform_distribution<T> >
    rocrand_status generate(T * data, size_t data_size,
                            Distribution distribution = Distribution())
    {
        rocrand_status status = init();
        if (status != ROCRAND_STATUS_SUCCESS)
            return status;

        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(rocrand_host::detail::generate_kernel),
            dim3(s_blocks), dim3(s_threads), 0, m_stream,
            m_engines, data, data_size, distribution
        );
        // Check kernel status
        if(hipPeekAtLastError() != hipSuccess)
            return ROCRAND_STATUS_LAUNCH_FAILURE;

        return ROCRAND_STATUS_SUCCESS;
    }

    template<class T>
    rocrand_status generate_uniform(T * data, size_t data_size)
    {
        rocrand_host::detail::mrg_uniform_distribution<T> distribution;
        return generate(data, data_size, distribution);
    }

    template<class T>
    rocrand_status generate_normal(T * data, size_t data_size, T mean, T stddev)
    {
        rocrand_host::detail::mrg_normal_distribution<T> distribution(mean, stddev);
        return generate(data, data_size, distribution);
    }

    template<class T>
    rocrand_status generate_log_normal(T * data, size_t data_size, T mean, T stddev)
    {
        rocrand_host::detail::mrg_log_normal_distribution<T> distribution(mean, stddev);
        return generate(data, data_size, distribution);
    }

    rocrand_status generate_poisson(unsigned int * data, size_t data_size, double lambda)
    {
        try
        {
            m_poisson.set_lambda(lambda);
        }
        catch(rocrand_status status)
        {
            return status;
        }
        return generate(data, data_size, m_poisson.dis);
    }

private:
    bool m_engines_initialized;
    engine_type * m_engines;
    size_t m_engines_size;
    #ifdef __HIP_PLATFORM_NVCC__
    static const uint32_t s_threads = 128;
    static const uint32_t s_blocks = 128;
    #else
    static const uint32_t s_threads = 256;
    static const uint32_t s_blocks = 512;
    #endif

    // For caching of Poisson for consecutive generations with the same lambda
    poisson_distribution_manager<> m_poisson;

    // m_seed from base_type
    // m_offset from base_type
};

#endif // ROCRAND_RNG_MRG32K3A_H_
