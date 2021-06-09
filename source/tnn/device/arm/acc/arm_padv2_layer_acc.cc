// Tencent is pleased to support the open source community by making TNN available.
//
// Copyright (C) 2020 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "tnn/device/arm/acc/arm_layer_acc.h"
#include "tnn/utils/data_type_utils.h"
#include "tnn/utils/dims_utils.h"

namespace TNN_NS {

DECLARE_ARM_ACC(PadV2, LAYER_PADV2);

// Common pad in height and width directions
static void CommonPadImpl(float *input_data, float *output_data, int batch_c_r4, int ih, int iw, int oh, int ow,
                          int pad_t, int pad_b, int pad_l, int iw_bytes, Float4 &vvalue) {
    for (int c = 0; c < batch_c_r4; c += 4) {
        auto input_ptr_c  = input_data + c * ih * iw;
        auto output_ptr_c = output_data + c * oh * ow;

        if (pad_t)
            for (int i = 0; i < ow * pad_t; ++i)
                Float4::save(output_ptr_c + i * 4, vvalue);

        for (int h = 0; h < ih; ++h) {
            auto output_ptr_h = output_ptr_c + ow * (h + pad_t) * 4;
            auto input_ptr_h  = input_ptr_c + iw * h * 4;
            for (int i = 0; i < pad_l; i++)
                Float4::save(output_ptr_h + i * 4, vvalue);

            memcpy(output_ptr_h + pad_l * 4, input_ptr_h, iw_bytes);

            for (int i = iw + pad_l; i < ow; i++)
                Float4::save(output_ptr_h + i * 4, vvalue);
        }

        if (pad_b) {
            auto output_ptr_h = output_ptr_c + ow * (ih + pad_t) * 4;
            for (int i = 0; i < ow * pad_b; ++i)
                Float4::save(output_ptr_h + i * 4, vvalue);
        }
    }
}

static void CalculatePad(Float4 &src, const Float4 &vvalue, const int padded_zero) {
    if (padded_zero)
        src = Float4::pad(src, vvalue, padded_zero);
}

// ic_mapped is not alligned to 4 when pad_c_b % 4 != 0
static void ChannelPadNotAligned(float *input_data_base, float *output_ptr_c, int ic_mapped, int ic, int ih, int iw,
                                 int oh, int ow, int pad_t, int pad_b, int pad_l, int pad_r, int iw_bytes,
                                 Float4 &vvalue) {
    int ic_r4 = ROUND_UP(ic, 4);
    // some channel may already be padded with zero
    int padded_zero  = ic_r4 - ic;
    auto ic_mapped_1 = ROUND_UP(ic_mapped, 4);
    auto ic_mapped_0 = ic_mapped_1 - 4;
    // shift_c is used to extract 4 values from two vectors
    auto shift_c = ic_mapped - ic_mapped_0;
    if (ic_mapped_1 < 0 || ic_mapped_0 >= ic_r4) {
        // pad with vvalue
        for (int i = 0; i < ow * oh; ++i)
            Float4::save(output_ptr_c + i * 4, vvalue);
    } else {
        auto input_ptr_c0 = input_data_base + ic_mapped_0 * ih * iw;
        auto input_ptr_c1 = input_data_base + ic_mapped_1 * ih * iw;
        if (pad_t)
            for (int i = 0; i < ow * pad_t; ++i)
                Float4::save(output_ptr_c + i * 4, vvalue);

        for (int h = 0; h < ih; ++h) {
            auto output_ptr_h = output_ptr_c + ow * (h + pad_t) * 4;
            auto input_ptr_h0 = input_ptr_c0 + iw * h * 4;
            auto input_ptr_h1 = input_ptr_c1 + iw * h * 4;
            for (int i = 0; i < pad_l; i++) {
                Float4::save(output_ptr_h, vvalue);
                output_ptr_h += 4;
            }

            if (ic_mapped_0 >= 0 && ic_mapped_1 < ic_r4 - 4) {
                // extract from two vectors
                for (int i = 0; i < iw; i++) {
                    Float4 res = Float4::extract(Float4::load(input_ptr_h0), Float4::load(input_ptr_h1), shift_c);
                    Float4::save(output_ptr_h, res);
                    input_ptr_h0 += 4;
                    input_ptr_h1 += 4;
                    output_ptr_h += 4;
                }
            } else if (ic_mapped_0 < 0) {
                // extract from vvalue && left boundary
                for (int i = 0; i < iw; i++) {
                    Float4 src = Float4::load(input_ptr_h1);
                    if (ic_mapped_1 == ic_r4 - 4)
                        CalculatePad(src, vvalue, padded_zero);
                    Float4 res = Float4::extract(vvalue, src, shift_c);
                    Float4::save(output_ptr_h, res);
                    input_ptr_h1 += 4;
                    output_ptr_h += 4;
                }
            } else if (ic_mapped_1 == ic_r4 - 4) {
                // extract from two vectors, the right one at the boundary
                for (int i = 0; i < iw; i++) {
                    Float4 src = Float4::load(input_ptr_h1);
                    CalculatePad(src, vvalue, padded_zero);
                    Float4 res = Float4::extract(Float4::load(input_ptr_h0), src, shift_c);
                    Float4::save(output_ptr_h, res);
                    input_ptr_h0 += 4;
                    input_ptr_h1 += 4;
                    output_ptr_h += 4;
                }
            } else {
                // extract from right boundary && vvalue
                for (int i = 0; i < iw; i++) {
                    Float4 src = Float4::load(input_ptr_h0);
                    CalculatePad(src, vvalue, padded_zero);
                    Float4 res = Float4::extract(src, vvalue, shift_c);
                    Float4::save(output_ptr_h, res);
                    input_ptr_h0 += 4;
                    output_ptr_h += 4;
                }
            }

            for (int i = 0; i < pad_r; i++) {
                Float4::save(output_ptr_h, vvalue);
                output_ptr_h += 4;
            }
        }

        if (pad_b) {
            auto output_ptr_h = output_ptr_c + ow * (ih + pad_t) * 4;
            for (int i = 0; i < ow * pad_b; ++i)
                Float4::save(output_ptr_h + i * 4, vvalue);
        }
    }
}

// ic_mapped is alligned to 4 when pad_c_b % 4 == 0
static void ChannelPadAligned(float *input_data_base, float *output_ptr_c, int ic_mapped, int ic, int ih, int iw,
                              int oh, int ow, int pad_t, int pad_b, int pad_l, int pad_r, int iw_bytes,
                              Float4 &vvalue) {
    int ic_r4       = ROUND_UP(ic, 4);
    bool ic_aligned = ((ic % 4) == 0);
    // some channel may already be padded with zero
    int padded_zero = ic_r4 - ic;
    if (ic_mapped < 0 || ic_mapped >= ic_r4) {
        for (int i = 0; i < ow * oh; ++i)
            Float4::save(output_ptr_c + i * 4, vvalue);
    } else {
        auto input_ptr_c = input_data_base + ic_mapped * ih * iw;
        if (pad_t)
            for (int i = 0; i < ow * pad_t; ++i)
                Float4::save(output_ptr_c + i * 4, vvalue);

        for (int h = 0; h < ih; ++h) {
            auto output_ptr_h = output_ptr_c + ow * (h + pad_t) * 4;
            auto input_ptr_h  = input_ptr_c + iw * h * 4;
            for (int i = 0; i < pad_l; i++) {
                Float4::save(output_ptr_h, vvalue);
                output_ptr_h += 4;
            }

            if (ic_aligned || ic_mapped <= ic - 4) {
                memcpy(output_ptr_h, input_ptr_h, iw_bytes);
                output_ptr_h += iw * 4;
            } else {
                for (int i = 0; i < iw; i++) {
                    Float4 res = Float4::pad(Float4::load(input_ptr_h), vvalue, padded_zero);
                    Float4::save(output_ptr_h, res);
                    input_ptr_h += 4;
                    output_ptr_h += 4;
                }
            }

            for (int i = 0; i < pad_r; i++) {
                Float4::save(output_ptr_h, vvalue);
                output_ptr_h += 4;
            }
        }
        if (pad_b) {
            auto output_ptr_h = output_ptr_c + ow * (ih + pad_t) * 4;
            for (int i = 0; i < ow * pad_b; ++i)
                Float4::save(output_ptr_h + i * 4, vvalue);
        }
    }
}

// Channel pad in channel, height and width directions
static void ChannelPadImpl(float *input_data, float *output_data, int batch, int c_r4, int oh, int ow, int ic, int ih,
                           int iw, int pad_t, int pad_b, int pad_l, int pad_r, int pad_c_b, int pad_c_e, int iw_bytes,
                           Float4 &vvalue) {
    int ic_r4          = ROUND_UP(ic, 4);
    bool pad_c_aligned = ((pad_c_b % 4) == 0);
    for (int n = 0; n < batch; ++n) {
        auto input_data_base  = input_data + n * ic_r4 * ih * iw;
        auto output_data_base = output_data + n * c_r4 * oh * ow;
        for (int c = 0; c < c_r4; c += 4) {
            auto output_ptr_c = output_data_base + c * oh * ow;
            auto ic_mapped    = c - pad_c_b;
            if (pad_c_aligned) {
                ChannelPadAligned(input_data_base, output_ptr_c, ic_mapped, ic, ih, iw, oh, ow, pad_t, pad_b, pad_l,
                                  pad_r, iw_bytes, vvalue);
            } else {
                ChannelPadNotAligned(input_data_base, output_ptr_c, ic_mapped, ic, ih, iw, oh, ow, pad_t, pad_b, pad_l,
                                     pad_r, iw_bytes, vvalue);
            }
        }
    }
}

static Status ConstPadV2(float *input_data, float *output_data, DimsVector input_dims, DimsVector output_dims,
                         PadLayerParam *layer_param) {
    const int batch    = output_dims[0];
    const int c_r4     = ROUND_UP(output_dims[1], 4);
    const int oh       = output_dims[2];
    const int ow       = output_dims[3];
    const int ic       = input_dims[1];
    const int ih       = input_dims[2];
    const int iw       = input_dims[3];
    const int iw_bytes = iw * sizeof(float) * 4;
    int pad_l = 0, pad_r = 0, pad_t = 0, pad_b = 0, pad_c_b = 0, pad_c_e = 0, pad_b_b = 0, pad_b_e = 0;
    auto pads = layer_param->pads;
    if (pads.size() == 8 && pads[0] == 0 && pads[4] == 0) {
        pad_b_b = pads[0];  // pad batch begin
        pad_c_b = pads[1];  // pad channel begin
        pad_t   = pads[2];  // pad height begin
        pad_l   = pads[3];  // pad width begin
        pad_b_e = pads[4];  // pad batch end
        pad_c_e = pads[5];  // pad channel end
        pad_b   = pads[6];  // pad height end
        pad_r   = pads[7];  // pad width end
    } else {
        LOGE("Arm Padv2 only support pads size is 8\n");
        return Status(TNNERR_UNKNOWN_LAYER, "Arm Padv2 only support pads size is 8");
    }
    Float4 value_v = Float4(layer_param->value);
    if (pad_c_b == 0 && pad_c_e == 0) {
        CommonPadImpl(input_data, output_data, batch * c_r4, ih, iw, oh, ow, pad_t, pad_b, pad_l, iw_bytes, value_v);
    } else {
        ChannelPadImpl(input_data, output_data, batch, c_r4, oh, ow, ic, ih, iw, pad_t, pad_b, pad_l, pad_r, pad_c_b,
                       pad_c_e, iw_bytes, value_v);
    }
    return TNN_OK;
}

static Status ReflectPadV2(float *input_data, float *output_data, DimsVector input_dims, DimsVector output_dims,
                           PadLayerParam *layer_param) {
    const int batch     = output_dims[0];
    const int c_r4      = ROUND_UP(output_dims[1], 4);
    const int oh        = output_dims[2];
    const int ow        = output_dims[3];
    const int ic        = input_dims[1];
    const int ih        = input_dims[2];
    const int iw        = input_dims[3];
    const int byte_size = sizeof(float);
    const int iw_bytes  = iw * byte_size * 4;
    int pad_l = 0, pad_r = 0, pad_t = 0, pad_b = 0, pad_c_b = 0, pad_c_e = 0, pad_b_b = 0, pad_b_e = 0;
    auto pads = layer_param->pads;
    if (pads.size() == 8 && pads[0] == 0 && pads[4] == 0) {
        pad_b_b = pads[0];  // pad batch begin
        pad_c_b = pads[1];  // pad channel begin
        pad_t   = pads[2];  // pad height begin
        pad_l   = pads[3];  // pad width begin
        pad_b_e = pads[4];  // pad batch end
        pad_c_e = pads[5];  // pad channel end
        pad_b   = pads[6];  // pad height end
        pad_r   = pads[7];  // pad width end
    } else {
        LOGE("Arm Padv2 only support pads size is 8\n");
        return Status(TNNERR_UNKNOWN_LAYER, "Arm Padv2 only support pads size is 8");
    }
    for (int c = 0; c < batch * c_r4; c += 4) {
        auto input_ptr_c  = input_data + c * ih * iw;
        auto output_ptr_c = output_data + c * oh * ow;

        for (int h = 0; h < ih; ++h) {
            auto output_ptr_h = output_ptr_c + ow * (h + pad_t) * 4;
            auto input_ptr_h  = input_ptr_c + iw * h * 4;
            for (int i = 0; i < pad_l; i++) {
                Float4::save(output_ptr_h + i * 4, Float4::load(input_ptr_h + (pad_l - i) * 4));
            }

            memcpy(output_ptr_h + pad_l * 4, input_ptr_h, iw_bytes);

            for (int i = 0; i < pad_r; i++) {
                Float4::save(output_ptr_h + (i + pad_l + iw) * 4, Float4::load(input_ptr_h + (iw - 1 - (i + 1)) * 4));
            }
        }
        // pad: copy from output
        for (int h = 0; h < pad_t; h++) {
            auto output_ptr_h = output_ptr_c + ow * h * 4;
            auto output_ref_h = output_ptr_c + ow * (pad_t + pad_t - h) * 4;
            memcpy(output_ptr_h, output_ref_h, ow * byte_size * 4);
        }

        for (int h = 0; h < pad_b; h++) {
            auto output_ptr_h = output_ptr_c + ow * (h + ih + pad_t) * 4;
            auto output_ref_h = output_ptr_c + ow * (ih + pad_t - 1 - (h + 1)) * 4;
            memcpy(output_ptr_h, output_ref_h, ow * byte_size * 4);
        }
    }
    return TNN_OK;
}

Status ArmPadV2LayerAcc::DoForward(const std::vector<Blob *> &inputs, const std::vector<Blob *> &outputs) {
    auto layer_param = dynamic_cast<PadLayerParam *>(param_);
    if (!layer_param) {
        LOGE("Error: layer param is nil\n");
        return Status(TNNERR_PARAM_ERR, "Error: layer param is nil");
    }
    auto input_blob  = inputs[0];
    auto output_blob = outputs[0];

    auto input_dims  = input_blob->GetBlobDesc().dims;
    auto output_dims = output_blob->GetBlobDesc().dims;

    if (output_blob->GetBlobDesc().data_type == DATA_TYPE_FLOAT ||
        output_blob->GetBlobDesc().data_type == DATA_TYPE_INT32 ||
        output_blob->GetBlobDesc().data_type == DATA_TYPE_UINT32) {
        auto input_data  = reinterpret_cast<float *>(GetBlobHandlePtr(input_blob->GetHandle()));
        auto output_data = reinterpret_cast<float *>(GetBlobHandlePtr(output_blob->GetHandle()));

        if (layer_param->type == 0) {
            // mode: const
            return ConstPadV2(input_data, output_data, input_dims, output_dims, layer_param);
        } else if (layer_param->type == 1) {
            // mode: reflect
            return ReflectPadV2(input_data, output_data, input_dims, output_dims, layer_param);
        } else {
            LOGE("Error: CpuPadV2LayerAcc layer param is not supported: type:%d\n", layer_param->type);
            return Status(TNNERR_PARAM_ERR, "Error: CpuPadV2LayerAcc layer param is not supported");
        }
    } else if (output_blob->GetBlobDesc().data_type == DATA_TYPE_INT8) {
        LOGE("Error: CpuPadV2LayerAcc layer acc dont support datatype: %d\n", output_blob->GetBlobDesc().data_type);
        return Status(TNNERR_MODEL_ERR, "Error: CpuPadV2LayerAcc layer acc dont support datatype");
    } else {
        LOGE("Error: CpuPadV2LayerAcc layer acc dont support datatype: %d\n", output_blob->GetBlobDesc().data_type);
        return Status(TNNERR_MODEL_ERR, "Error: CpuPadV2LayerAcc layer acc dont support datatype");
    }
    return TNN_OK;
}

REGISTER_ARM_ACC(PadV2, LAYER_PADV2);
// REGISTER_ARM_LAYOUT(LAYER_PADV2, DATA_FORMAT_NCHW);
REGISTER_ARM_LAYOUT(LAYER_PADV2, DATA_FORMAT_NC4HW4);

}  // namespace TNN_NS