/*******************************************************************************
* Copyright 2016 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
#include "c_types_map.hpp"
#include "nstl.hpp"

#include "jit_avx2_pooling_generator_f32.hpp"

#define ymm_store_mask      Ymm(15)
#define ymm_input           Ymm(14)
#define ymm_index           Ymm(13)
#define xmm_index           Xmm(13)
#define ymm_simd            Ymm(12)
#define xmm_simd            Xmm(12)
#define ymm_simd_stride_w   Ymm(11)
#define xmm_simd_stride_w   Xmm(11)
#define ymm_ki_offset       Ymm(10)
#define xmm_ki_offset       Xmm(10)
#define ymm_ji_offset       Ymm(9)
#define xmm_ji_offset       Xmm(9)
#define ymm_tmp             Ymm(8)
#define xmm_tmp             Xmm(8)
#define ymm_offset_base     Ymm(7)
#define xmm_offset_base     Xmm(7)

namespace mkldnn {
namespace impl {
namespace cpu {

inline void jit_avx2_pooling_generator_f32::oh_step(
    jit_pooling_param_t *params, uint32_t ur_w,
    int pad_l, int pad_r, const char* kh_lable)
{
    using Xbyak::Ymm;
    using Xbyak::Xmm;

    unsigned char _cmp = 1;
    union {
        float _flt_max;
        int32_t _flt_max_int;
    } cvt;
    cvt._flt_max = -FLT_MAX;

    uint32_t IW = params->iw;
    uint32_t KW = params->kw;
    uint32_t stride_w = params->stride_w;

    vpxor(ymm_store_mask, ymm_store_mask);

    mov(tmp_gpr, cvt._flt_max_int);
    movq(xmm_tmp, tmp_gpr);
    vbroadcastss(ymm_tmp, xmm_tmp);
    for (uint32_t jj = 0; jj < ur_w; jj++)
        vmovaps(Ymm(jj), ymm_tmp);

    mov(aux_reg_input , reg_input);
    xor_(kj, kj);
    L(kh_lable); {
        if (this->_is_training) {
            vpxor(ymm_ki_offset, ymm_ki_offset);
        }
        for (uint32_t ki = 0; ki < KW; ki++) {
            int jj_start = nstl::max(0, pad_l-(int)ki);
            int jj_end   = (int)ur_w -
                nstl::max(0, (int)ki+pad_r - (int)(KW-1));
            if (this->_is_training) {
                vmovaps(ymm_index, ymm_ki_offset);
                vmovaps(ymm_ji_offset, ymm_offset_base);
                if (jj_start != 0) {
                    mov(tmp_gpr,(jj_start * stride_w * params->c_block));
                    movq(xmm_tmp, tmp_gpr);
                    vpbroadcastd(ymm_tmp, xmm_tmp);
                    vpaddd(ymm_ji_offset, ymm_ji_offset, ymm_tmp);
                }
            }
            for (int jj = jj_start; jj  < jj_end; jj++) {
                int aux_input_offset = (ki+jj*stride_w-pad_l)*params->c_block;
                if (aux_input_offset > (int)IW*(int)params->c_block)
                    continue;
                if (this->_is_training) {
                    vpaddd(ymm_index, ymm_ki_offset, ymm_ji_offset);
                }
                vmovups(ymm_input,
                    ptr [ aux_reg_input + sizeof(float)*aux_input_offset ]);
                vcmpps(ymm_store_mask, Ymm(jj), ymm_input, _cmp);
                vblendvps(Ymm(jj), Ymm(jj), ymm_input, ymm_store_mask);
                if (this->_is_training) {
                    vblendvps(Ymm(ur_w+jj), Ymm(ur_w+jj), ymm_index,
                        ymm_store_mask);
                    vpaddd(ymm_ji_offset, ymm_ji_offset , ymm_simd_stride_w);
                }
            }
            if (this->_is_training) {
                vpaddd(ymm_ki_offset, ymm_ki_offset , ymm_simd);
            }
        }
        add(aux_reg_input,  sizeof(float)*IW*params->c_block);
        inc(kj);
        cmp(kj, reg_kh);
        jl(kh_lable, T_NEAR);
    }

    for (uint32_t jj = 0; jj < ur_w; jj++) {
        vmovups(YWORD[reg_output + sizeof(float)*jj*params->c_block], Ymm(jj));
        if (this->_is_training)
            vmovdqa(YWORD[reg_index + sizeof(uint32_t)*jj*params->c_block],
                    Ymm(ur_w+jj));
    }
}

jit_avx2_pooling_generator_f32::jit_avx2_pooling_generator_f32(
        jit_pooling_param_t *params, bool is_training, void* code_ptr,
        size_t code_size)
    : jit_generator(code_ptr, code_size)
    , _is_training(is_training)
{
    using Xbyak::Ymm;
    this->preamble();

    int n_oi = params->ow / params->ur_w;

    mov(reg_input , ptr [ this->param1 ]);
    mov(reg_output, ptr [ this->param1 + 8]);
    if (this->_is_training)
        mov(reg_index , ptr [ this->param1 + 16]);
    mov(reg_kh    , ptr [ this->param1 + 48]);
    if (this->_is_training)
        mov(reg_arr_init, ptr [ this->param1 + 80]);

    if (this->_is_training) {
        mov(tmp_gpr,(params->c_block));
        movq(xmm_simd, tmp_gpr);
        vpbroadcastd(ymm_simd, xmm_simd);

        mov(tmp_gpr,(params->stride_w * params->c_block));
        movq(xmm_simd_stride_w, tmp_gpr);
        vpbroadcastd(ymm_simd_stride_w, xmm_simd_stride_w);

        vmovdqu(ymm_offset_base, ptr [ reg_arr_init ]);
        if (params->l_pad > 0) {
            mov(tmp_gpr,(params->l_pad * params->c_block));
            movq(xmm_tmp, tmp_gpr);
            vpbroadcastd(ymm_tmp, xmm_tmp);
            vpsubd(ymm_offset_base, ymm_offset_base, ymm_tmp);
        }
    }

    int r_pad  = nstl::max(0, (int)((params->ow-1)*params->stride_w) +
        (int)params->kw - 1 - (int)(params->iw + params->l_pad - 1 ));
    int r_pad1 = (int)(params->ur_w*n_oi - 1)*params->stride_w +
        params->kw - 1 - (params->iw + params->l_pad - 1);
    if (r_pad1 > 0) n_oi--;

    if (params->l_pad > 0) {
        n_oi--;
        if (n_oi < 0 && r_pad1 > 0) {
            oh_step(params, params->ur_w, params->l_pad, r_pad1,
                  ".kh_loop_oimain_padwl");
        } else  {
            oh_step(params, params->ur_w, params->l_pad, 0,
                  ".kh_loop_oimain_padwl");
        }

        add(reg_input,  sizeof(float)*(params->ur_w*params->stride_w -
                               params->l_pad)*params->c_block);
        add(reg_output,  sizeof(float)*params->ur_w*params->c_block);
        if (this->_is_training)
            add(reg_index, sizeof(uint32_t)*params->ur_w*params->c_block);
    }

    xor_(oi_iter, oi_iter);
    if (n_oi > 0) {
        L(".ow_loop"); {
            oh_step(params, params->ur_w, 0, 0, ".kh_loop_oimain");
            add(reg_input,
               sizeof(float)*params->ur_w*params->stride_w*params->c_block);
            add(reg_output, sizeof(float)*params->ur_w*params->c_block);
            if (this->_is_training)
                add(reg_index, sizeof(uint32_t)*params->ur_w*params->c_block);

            inc(oi_iter);
            cmp(oi_iter, n_oi); jl(".ow_loop", T_NEAR);
        } L(".ow_loop_end");
    }

    if (r_pad1 > 0 && n_oi >= 0) {
        oh_step(params, params->ur_w, 0, r_pad1, ".kh_loop_oimain_padwr");
        add(reg_input,
               sizeof(float)*params->ur_w*params->stride_w*params->c_block);
        add(reg_output,sizeof(float)*params->ur_w*params->c_block);
        if (this->_is_training)
            add(reg_index, sizeof(uint32_t) * params->ur_w * params->c_block);
    }

    if (params->ur_w_tail != 0)
        oh_step(params, params->ur_w_tail, 0, r_pad, ".kh_loop_oitail");

    this->postamble();
    return;
}

}
}
}

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s