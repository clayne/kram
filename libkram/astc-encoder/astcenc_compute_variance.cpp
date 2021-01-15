// SPDX-License-Identifier: Apache-2.0
// ----------------------------------------------------------------------------
// Copyright 2011-2020 Arm Limited
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not
// use this file except in compliance with the License. You may obtain a copy
// of the License at:
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
// ----------------------------------------------------------------------------

#if !defined(ASTCENC_DECOMPRESS_ONLY)

/**
 * @brief Functions to calculate variance per channel in a NxN footprint.
 *
 * We need N to be parametric, so the routine below uses summed area tables in
 * order to execute in O(1) time independent of how big N is.
 *
 * The addition uses a Brent-Kung-based parallel prefix adder. This uses the
 * prefix tree to first perform a binary reduction, and then distributes the
 * results. This method means that there is no serial dependency between a
 * given element and the next one, and also significantly improves numerical
 * stability allowing us to use floats rather than doubles.
 */

#include "astcenc_internal.h"

#include <cassert>

#define USE_2DARRAY 1

/**
 * @brief Generate a prefix-sum array using Brent-Kung algorithm.
 *
 * This will take an input array of the form:
 *     v0, v1, v2, ...
 * ... and modify in-place to turn it into a prefix-sum array of the form:
 *     v0, v0+v1, v0+v1+v2, ...
 *
 * @param d      The array to prefix-sum.
 * @param items  The number of items in the array.
 * @param stride The item spacing in the array; i.e. dense arrays should use 1.
 */
static void brent_kung_prefix_sum(
	float4* d,
	size_t items,
	int stride
) {
	if (items < 2)
		return;

	size_t lc_stride = 2;
	size_t log2_stride = 1;

	// The reduction-tree loop
	do {
		size_t step = lc_stride >> 1;
		size_t start = lc_stride - 1;
		size_t iters = items >> log2_stride;

		float4 *da = d + (start * stride);
		ptrdiff_t ofs = -(ptrdiff_t)(step * stride);
		size_t ofs_stride = stride << log2_stride;

		while (iters)
		{
			*da = *da + da[ofs];
			da += ofs_stride;
			iters--;
		}

		log2_stride += 1;
		lc_stride <<= 1;
	} while (lc_stride <= items);

	// The expansion-tree loop
	do {
		log2_stride -= 1;
		lc_stride >>= 1;

		size_t step = lc_stride >> 1;
		size_t start = step + lc_stride - 1;
		size_t iters = (items - step) >> log2_stride;

		float4 *da = d + (start * stride);
		ptrdiff_t ofs = -(ptrdiff_t)(step * stride);
		size_t ofs_stride = stride << log2_stride;

		while (iters)
		{
			*da = *da + da[ofs];
			da += ofs_stride;
			iters--;
		}
	} while (lc_stride > 2);
}

/**
 * @brief Compute averages and variances for a pixel region.
 *
 * The routine computes both in a single pass, using a summed-area table to
 * decouple the running time from the averaging/variance kernel size.
 *
 * @param arg The input parameter structure.
 */
static void compute_pixel_region_variance(
	astcenc_context& ctx,
	const pixel_region_variance_args* arg
) {
	// Unpack the memory structure into local variables
	const astcenc_image* img = arg->img;
	float rgb_power = arg->rgb_power;
	float alpha_power = arg->alpha_power;
	astcenc_swizzle swz = arg->swz;
	int have_z = arg->have_z;

	int size_x = arg->size.r;
	int size_y = arg->size.g;
	int size_z = arg->size.b;

	int offset_x = arg->offset.r;
	int offset_y = arg->offset.g;
	int offset_z = arg->offset.b;

	int avg_var_kernel_radius = arg->avg_var_kernel_radius;
	int alpha_kernel_radius = arg->alpha_kernel_radius;

	float  *input_alpha_averages = ctx.input_alpha_averages;
	float4 *input_averages = ctx.input_averages;
	float4 *input_variances = ctx.input_variances;
	float4 *work_memory = arg->work_memory;

	// Compute memory sizes and dimensions that we need
	int kernel_radius = MAX(avg_var_kernel_radius, alpha_kernel_radius);
	int kerneldim = 2 * kernel_radius + 1;
	int kernel_radius_xy = kernel_radius;
	int kernel_radius_z = have_z ? kernel_radius : 0;

	int padsize_x = size_x + kerneldim;
	int padsize_y = size_y + kerneldim;
	int padsize_z = size_z + (have_z ? kerneldim : 0);
	int sizeprod = padsize_x * padsize_y * padsize_z;

	int zd_start = have_z ? 1 : 0;
	int are_powers_1 = (rgb_power == 1.0f) && (alpha_power == 1.0f);

	float4 *varbuf1 = work_memory;
	float4 *varbuf2 = work_memory + sizeprod;

	// Scaling factors to apply to Y and Z for accesses into the work buffers
	int yst = padsize_x;
	int zst = padsize_x * padsize_y;

	// Scaling factors to apply to Y and Z for accesses into result buffers
	int ydt = img->dim_x;
	int zdt = img->dim_x * img->dim_y;

	// Macros to act as accessor functions for the work-memory
	#define VARBUF1(z, y, x) varbuf1[z * zst + y * yst + x]
	#define VARBUF2(z, y, x) varbuf2[z * zst + y * yst + x]

    // True if any non-identity swizzle
    bool needs_swz = (swz.r != ASTCENC_SWZ_R) || (swz.g != ASTCENC_SWZ_G) ||
                     (swz.b != ASTCENC_SWZ_B) || (swz.a != ASTCENC_SWZ_A);

	// Load N and N^2 values into the work buffers
	if (img->data_type == ASTCENC_TYPE_U8)
	{
#if USE_2DARRAY
        uint8_t* data8 = static_cast<uint8_t*>(img->data);
#else
		uint8_t*** data8 = static_cast<uint8_t***>(img->data);
#endif
		// Swizzle data structure 4 = ZERO, 5 = ONE
		uint8_t data[6];
		data[ASTCENC_SWZ_0] = 0;
		data[ASTCENC_SWZ_1] = 255;

		for (int z = zd_start; z < padsize_z; z++)
		{
			int z_src = (z - zd_start) + offset_z - kernel_radius_z;
			z_src = astc::clamp(z_src, 0, (int)(img->dim_z - 1));

			for (int y = 1; y < padsize_y; y++)
			{
				int y_src = (y - 1) + offset_y - kernel_radius_xy;
				y_src = astc::clamp(y_src, 0, (int)(img->dim_y - 1));

				for (int x = 1; x < padsize_x; x++)
				{
					int x_src = (x - 1) + offset_x - kernel_radius_xy;
					x_src = astc::clamp(x_src, 0, (int)(img->dim_x - 1));

                    float4 d;
#if USE_2DARRAY
                    int px = (y_src * img->dim_x + x_src) * 4;
                    
                    uint8_t r = data8[px + 0];
                    uint8_t g = data8[px + 1];
                    uint8_t b = data8[px + 2];
                    uint8_t a = data8[px + 3];
                    
                    if (needs_swz)
                    {
                        data[0] = r;
                        data[1] = g;
                        data[2] = b;
                        data[3] = a;
                        
                        r = data[swz.r];
                        g = data[swz.g];
                        b = data[swz.b];
                        a = data[swz.a];
                    }
#else
					data[0] = data8[z_src][y_src][4 * x_src    ];
					data[1] = data8[z_src][y_src][4 * x_src + 1];
					data[2] = data8[z_src][y_src][4 * x_src + 2];
					data[3] = data8[z_src][y_src][4 * x_src + 3];

                    uint8_t r = data[swz.r];
                    uint8_t g = data[swz.g];
                    uint8_t b = data[swz.b];
                    uint8_t a = data[swz.a];
#endif
                    // int to float conversion
                    d = float4((float)r, (float)g, (float)b, float(a));
                    d = d * (1.0f / 255.0f);

					if (!are_powers_1)
					{
						d.r = powf(MAX(d.r, 1e-6f), rgb_power);
						d.g = powf(MAX(d.g, 1e-6f), rgb_power);
						d.b = powf(MAX(d.b, 1e-6f), rgb_power);
						d.a = powf(MAX(d.a, 1e-6f), alpha_power);
					}

					VARBUF1(z, y, x) = d;
					VARBUF2(z, y, x) = d * d;
				}
			}
		}
	}
	else if (img->data_type == ASTCENC_TYPE_F16)
	{
// TODO: apply USE_2DARRAY to FP16 inputs
		uint16_t*** data16 = static_cast<uint16_t***>(img->data);

		// Swizzle data structure 4 = ZERO, 5 = ONE (in FP16)
		uint16_t data[6];
		data[ASTCENC_SWZ_0] = 0;
		data[ASTCENC_SWZ_1] = 0x3C00;

		for (int z = zd_start; z < padsize_z; z++)
		{
			int z_src = (z - zd_start) + offset_z - kernel_radius_z;
			z_src = astc::clamp(z_src, 0, (int)(img->dim_z - 1));

			for (int y = 1; y < padsize_y; y++)
			{
				int y_src = (y - 1) + offset_y - kernel_radius_xy;
				y_src = astc::clamp(y_src, 0, (int)(img->dim_y - 1));

				for (int x = 1; x < padsize_x; x++)
				{
					int x_src = (x - 1) + offset_x - kernel_radius_xy;
					x_src = astc::clamp(x_src, 0, (int)(img->dim_x - 1));

					data[0] = data16[z_src][y_src][4 * x_src    ];
					data[1] = data16[z_src][y_src][4 * x_src + 1];
					data[2] = data16[z_src][y_src][4 * x_src + 2];
					data[3] = data16[z_src][y_src][4 * x_src + 3];

					uint16_t r = data[swz.r];
					uint16_t g = data[swz.g];
					uint16_t b = data[swz.b];
					uint16_t a = data[swz.a];

					float4 d = float4(sf16_to_float(r),
					                  sf16_to_float(g),
					                  sf16_to_float(b),
					                  sf16_to_float(a));

					if (!are_powers_1)
					{
						d.r = powf(MAX(d.r, 1e-6f), rgb_power);
						d.g = powf(MAX(d.g, 1e-6f), rgb_power);
						d.b = powf(MAX(d.b, 1e-6f), rgb_power);
						d.a = powf(MAX(d.a, 1e-6f), alpha_power);
					}

					VARBUF1(z, y, x) = d;
					VARBUF2(z, y, x) = d * d;
				}
			}
		}
	}
	else // if (img->data_type == ASTCENC_TYPE_F32)
	{
		assert(img->data_type == ASTCENC_TYPE_F32);
#if USE_2DARRAY
        float4* data32 = static_cast<float4*>(img->data);
#else
		float*** data32 = static_cast<float***>(img->data);
#endif
		// Swizzle data structure 4 = ZERO, 5 = ONE (in FP16)
		float data[6];
		data[ASTCENC_SWZ_0] = 0.0f;
		data[ASTCENC_SWZ_1] = 1.0f;

		for (int z = zd_start; z < padsize_z; z++)
		{
			int z_src = (z - zd_start) + offset_z - kernel_radius_z;
			z_src = astc::clamp(z_src, 0, (int)(img->dim_z - 1));

			for (int y = 1; y < padsize_y; y++)
			{
				int y_src = (y - 1) + offset_y - kernel_radius_xy;
				y_src = astc::clamp(y_src, 0, (int)(img->dim_y - 1));

				for (int x = 1; x < padsize_x; x++)
				{
					int x_src = (x - 1) + offset_x - kernel_radius_xy;
					x_src = astc::clamp(x_src, 0, (int)(img->dim_x - 1));

#if USE_2DARRAY
                    assert(z_src == 0);
                    float4 d = data32[y_src * img->dim_x + x_src];
                    
                    if (needs_swz)
                    {
                        data[0] = d.r;
                        data[1] = d.g;
                        data[2] = d.b;
                        data[3] = d.a;
                        
                        float r = data[swz.r];
                        float g = data[swz.g];
                        float b = data[swz.b];
                        float a = data[swz.a];
                        
                        d = float4(r,g,b,a);
                    }
#else
					data[0] = data32[z_src][y_src][4 * x_src    ];
					data[1] = data32[z_src][y_src][4 * x_src + 1];
					data[2] = data32[z_src][y_src][4 * x_src + 2];
					data[3] = data32[z_src][y_src][4 * x_src + 3];
                    
					float r = data[swz.r];
					float g = data[swz.g];
					float b = data[swz.b];
					float a = data[swz.a];

					float4 d = float4(r, g, b, a);
#endif

					if (!are_powers_1)
					{
						d.r = powf(MAX(d.r, 1e-6f), rgb_power);
						d.g = powf(MAX(d.g, 1e-6f), rgb_power);
						d.b = powf(MAX(d.b, 1e-6f), rgb_power);
						d.a = powf(MAX(d.a, 1e-6f), alpha_power);
					}

					VARBUF1(z, y, x) = d;
					VARBUF2(z, y, x) = d * d;
				}
			}
		}
	}

	// Pad with an extra layer of 0s; this forms the edge of the SAT tables
	float4 vbz = float4(0.0f);
	for (int z = 0; z < padsize_z; z++)
	{
		for (int y = 0; y < padsize_y; y++)
		{
			VARBUF1(z, y, 0) = vbz;
			VARBUF2(z, y, 0) = vbz;
		}

		for (int x = 0; x < padsize_x; x++)
		{
			VARBUF1(z, 0, x) = vbz;
			VARBUF2(z, 0, x) = vbz;
		}
	}

	if (have_z)
	{
		for (int y = 0; y < padsize_y; y++)
		{
			for (int x = 0; x < padsize_x; x++)
			{
				VARBUF1(0, y, x) = vbz;
				VARBUF2(0, y, x) = vbz;
			}
		}
	}

	// Generate summed-area tables for N and N^2; this is done in-place, using
	// a Brent-Kung parallel-prefix based algorithm to minimize precision loss
	for (int z = zd_start; z < padsize_z; z++)
	{
		for (int y = 1; y < padsize_y; y++)
		{
			brent_kung_prefix_sum(&(VARBUF1(z, y, 1)), padsize_x - 1, 1);
			brent_kung_prefix_sum(&(VARBUF2(z, y, 1)), padsize_x - 1, 1);
		}
	}

	for (int z = zd_start; z < padsize_z; z++)
	{
		for (int x = 1; x < padsize_x; x++)
		{
			brent_kung_prefix_sum(&(VARBUF1(z, 1, x)), padsize_y - 1, yst);
			brent_kung_prefix_sum(&(VARBUF2(z, 1, x)), padsize_y - 1, yst);
		}
	}

	if (have_z)
	{
		for (int y = 1; y < padsize_y; y++)
		{
			for (int x = 1; x < padsize_x; x++)
			{
				brent_kung_prefix_sum(&(VARBUF1(1, y, x)), padsize_z - 1, zst);
				brent_kung_prefix_sum(&(VARBUF2(1, y, x)), padsize_z - 1, zst);
			}
		}
	}

	int avg_var_kdim = 2 * avg_var_kernel_radius + 1;
	int alpha_kdim = 2 * alpha_kernel_radius + 1;

	// Compute a few constants used in the variance-calculation.
	float avg_var_samples;
	float alpha_rsamples;
	float mul1;

	if (have_z)
	{
		avg_var_samples = (float)(avg_var_kdim * avg_var_kdim * avg_var_kdim);
		alpha_rsamples = 1.0f / (float)(alpha_kdim * alpha_kdim * alpha_kdim);
	}
	else
	{
		avg_var_samples = (float)(avg_var_kdim * avg_var_kdim);
		alpha_rsamples = 1.0f / (float)(alpha_kdim * alpha_kdim);
	}

	float avg_var_rsamples = 1.0f / avg_var_samples;
	if (avg_var_samples == 1)
	{
		mul1 = 1.0f;
	}
	else
	{
		mul1 = 1.0f / (float)(avg_var_samples * (avg_var_samples - 1));
	}

	float mul2 = avg_var_samples * mul1;

	// Use the summed-area tables to compute variance for each neighborhood
	if (have_z)
	{
		for (int z = 0; z < size_z; z++)
		{
			int z_src = z + kernel_radius_z;
			int z_dst = z + offset_z;
			int z_low  = z_src - alpha_kernel_radius;
			int z_high = z_src + alpha_kernel_radius + 1;

			astc::clamp(z_src,  0, (int)(img->dim_z - 1));
			astc::clamp(z_low,  0, (int)(img->dim_z - 1));
			astc::clamp(z_high, 0, (int)(img->dim_z - 1));


			for (int y = 0; y < size_y; y++)
			{
				int y_src = y + kernel_radius_xy;
				int y_dst = y + offset_y;
				int y_low  = y_src - alpha_kernel_radius;
				int y_high = y_src + alpha_kernel_radius + 1;

				astc::clamp(y_src,  0, (int)(img->dim_y - 1));
				astc::clamp(y_low,  0, (int)(img->dim_y - 1));
				astc::clamp(y_high, 0, (int)(img->dim_y - 1));

				for (int x = 0; x < size_x; x++)
				{
					int x_src = x + kernel_radius_xy;
					int x_dst = x + offset_x;
					int x_low  = x_src - alpha_kernel_radius;
					int x_high = x_src + alpha_kernel_radius + 1;

					astc::clamp(x_src,  0, (int)(img->dim_x - 1));
					astc::clamp(x_low,  0, (int)(img->dim_x - 1));
					astc::clamp(x_high, 0, (int)(img->dim_x - 1));

					// Summed-area table lookups for alpha average
					float vasum = (  VARBUF1(z_high, y_low,  x_low).a
					               - VARBUF1(z_high, y_low,  x_high).a
					               - VARBUF1(z_high, y_high, x_low).a
					               + VARBUF1(z_high, y_high, x_high).a) -
					              (  VARBUF1(z_low,  y_low,  x_low).a
					               - VARBUF1(z_low,  y_low,  x_high).a
					               - VARBUF1(z_low,  y_high, x_low).a
					               + VARBUF1(z_low,  y_high, x_high).a);

					int out_index = z_dst * zdt + y_dst * ydt + x_dst;
					input_alpha_averages[out_index] = (vasum * alpha_rsamples);

					// Summed-area table lookups for RGBA average and variance
					float4 v1sum = (  VARBUF1(z_high, y_low,  x_low)
					                - VARBUF1(z_high, y_low,  x_high)
					                - VARBUF1(z_high, y_high, x_low)
					                + VARBUF1(z_high, y_high, x_high)) -
					               (  VARBUF1(z_low,  y_low,  x_low)
					                - VARBUF1(z_low,  y_low,  x_high)
					                - VARBUF1(z_low,  y_high, x_low)
					                + VARBUF1(z_low,  y_high, x_high));

					float4 v2sum = (  VARBUF2(z_high, y_low,  x_low)
					                - VARBUF2(z_high, y_low,  x_high)
					                - VARBUF2(z_high, y_high, x_low)
					                + VARBUF2(z_high, y_high, x_high)) -
					               (  VARBUF2(z_low,  y_low,  x_low)
					                - VARBUF2(z_low,  y_low,  x_high)
					                - VARBUF2(z_low,  y_high, x_low)
					                + VARBUF2(z_low,  y_high, x_high));

					// Compute and emit the average
					float4 avg = v1sum * avg_var_rsamples;
					input_averages[out_index] = avg;

					// Compute and emit the actual variance
					float4 variance = mul2 * v2sum - mul1 * (v1sum * v1sum);
					input_variances[out_index] = variance;
				}
			}
		}
	}
	else
	{
		for (int y = 0; y < size_y; y++)
		{
			int y_src = y + kernel_radius_xy;
			int y_dst = y + offset_y;
			int y_low  = y_src - alpha_kernel_radius;
			int y_high = y_src + alpha_kernel_radius + 1;

			astc::clamp(y_src,  0, (int)(img->dim_y - 1));
			astc::clamp(y_low,  0, (int)(img->dim_y - 1));
			astc::clamp(y_high, 0, (int)(img->dim_y - 1));

			for (int x = 0; x < size_x; x++)
			{
				int x_src = x + kernel_radius_xy;
				int x_dst = x + offset_x;
				int x_low  = x_src - alpha_kernel_radius;
				int x_high = x_src + alpha_kernel_radius + 1;

				astc::clamp(x_src,  0, (int)(img->dim_x - 1));
				astc::clamp(x_low,  0, (int)(img->dim_x - 1));
				astc::clamp(x_high, 0, (int)(img->dim_x - 1));

				// Summed-area table lookups for alpha average
				float vasum = VARBUF1(0, y_low,  x_low).a
				            - VARBUF1(0, y_low,  x_high).a
				            - VARBUF1(0, y_high, x_low).a
				            + VARBUF1(0, y_high, x_high).a;

				int out_index = y_dst * ydt + x_dst;
				input_alpha_averages[out_index] = (vasum * alpha_rsamples);

				// summed-area table lookups for RGBA average and variance
				float4 v1sum = VARBUF1(0, y_low,  x_low)
				             - VARBUF1(0, y_low,  x_high)
				             - VARBUF1(0, y_high, x_low)
				             + VARBUF1(0, y_high, x_high);

				float4 v2sum = VARBUF2(0, y_low,  x_low)
				             - VARBUF2(0, y_low,  x_high)
				             - VARBUF2(0, y_high, x_low)
				             + VARBUF2(0, y_high, x_high);

				// Compute and emit the average
				float4 avg = v1sum * avg_var_rsamples;
				input_averages[out_index] = avg;

				// Compute and emit the actual variance
				float4 variance = mul2 * v2sum - mul1 * (v1sum * v1sum);
				input_variances[out_index] = variance;
			}
		}
	}
}

void compute_averages_and_variances(
	astcenc_context& ctx,
	const avg_var_args &ag
) {
	pixel_region_variance_args arg = ag.arg;
	arg.work_memory = new float4[ag.work_memory_size];

	int size_x = ag.img_size.r;
	int size_y = ag.img_size.g;
	int size_z = ag.img_size.b;

	int step_x = ag.blk_size.r;
	int step_y = ag.blk_size.g;
	int step_z = ag.blk_size.b;

	int y_tasks = (size_y + step_y - 1) / step_y;

	// All threads run this processing loop until there is no work remaining
	while (true)
	{
		unsigned int count;
		unsigned int base = ctx.manage_avg_var.get_task_assignment(1, count);
		if (!count)
		{
			break;
		}

		assert(count == 1);
		int z = (base / (y_tasks)) * step_z;
		int y = (base - (z * y_tasks)) * step_y;

		arg.size.b = MIN(step_z, size_z - z);
		arg.offset.b = z;

		arg.size.g = MIN(step_y, size_y - y);
		arg.offset.g = y;

		for (int x = 0; x < size_x; x += step_x)
		{
			arg.size.r = MIN(step_x, size_x - x);
			arg.offset.r = x;
			compute_pixel_region_variance(ctx, &arg);
		}

		ctx.manage_avg_var.complete_task_assignment(count);
	}

	delete[] arg.work_memory;
}

/* Public function, see header file for detailed documentation */
unsigned int init_compute_averages_and_variances(
	astcenc_image& img,
	float rgb_power,
	float alpha_power,
	int avg_var_kernel_radius,
	int alpha_kernel_radius,
	astcenc_swizzle swz,
	pixel_region_variance_args& arg,
	avg_var_args& ag
) {
	int size_x = img.dim_x;
	int size_y = img.dim_y;
	int size_z = img.dim_z;

	// Compute maximum block size and from that the working memory buffer size
	int kernel_radius = MAX(avg_var_kernel_radius, alpha_kernel_radius);
	int kerneldim = 2 * kernel_radius + 1;

	int have_z = (size_z > 1);
	int max_blk_size_xy = have_z ? 16 : 32;
	int max_blk_size_z = MIN(size_z, have_z ? 16 : 1);

	int max_padsize_xy = max_blk_size_xy + kerneldim;
	int max_padsize_z = max_blk_size_z + (have_z ? kerneldim : 0);

	// Perform block-wise averages-and-variances calculations across the image
	// Initialize fields which are not populated until later
	arg.size = int3(0);
	arg.offset = int3(0);
	arg.work_memory = nullptr;

	arg.img = &img;
	arg.rgb_power = rgb_power;
	arg.alpha_power = alpha_power;
	arg.swz = swz;
	arg.have_z = have_z;
	arg.avg_var_kernel_radius = avg_var_kernel_radius;
	arg.alpha_kernel_radius = alpha_kernel_radius;

	ag.arg = arg;
	ag.img_size = int3(size_x, size_y, size_z);
	ag.blk_size = int3(max_blk_size_xy, max_blk_size_xy, max_blk_size_z);
	ag.work_memory_size = 2 * max_padsize_xy * max_padsize_xy * max_padsize_z;

	// The parallel task count
	int z_tasks = (size_z + max_blk_size_z - 1) / max_blk_size_z;
	int y_tasks = (size_y + max_blk_size_xy - 1) / max_blk_size_xy;
	return z_tasks * y_tasks;
}

#endif