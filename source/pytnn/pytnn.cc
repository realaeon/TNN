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

#include <tnn/core/tnn.h>
#include <tnn/core/mat.h>
#include <pytnn/pytnn.h>

namespace py = pybind11; 

namespace TNN_NS {

Module* Load(const std::string& model_path) {
    printf("model_path: %s \n", model_path.c_str());
    ModelConfig model_config;
    model_config.model_type = MODEL_TYPE_TORCHSCRIPT;
    model_config.params.push_back(model_path);
    TNN net;
    Status ret = net.Init(model_config);
    InputShapesMap shapes_map;
    shapes_map["0"]={1,3,224,224};
    NetworkConfig network_config;
    network_config.device_type = DEVICE_CUDA;
    network_config.network_type = NETWORK_TYPE_TNNTORCH;
    auto instance = net.CreateInst(network_config, ret, shapes_map); 
    return new Module(instance);
}

Module::Module(std::shared_ptr<Instance> instance) {
    instance_ = instance;
}

py::array_t<float> Module::Forward(py::array_t<float> input) {
    py::buffer_info input_info = input.request();
    float *input_ptr = static_cast<float *>(input_info.ptr);
    DimsVector input_dims;
    for(auto dim : input_info.shape) {
        input_dims.push_back(dim);
    }
    auto input_mat = std::make_shared<TNN_NS::Mat>(DEVICE_NAIVE, NCHW_FLOAT, input_dims, input_ptr);
    instance_->SetInputMat(input_mat, MatConvertParam()); 
    instance_->Forward();
    std::shared_ptr<TNN_NS::Mat> output_mat;
    instance_->GetOutputMat(output_mat, MatConvertParam(),
                        "", DEVICE_NAIVE, NCHW_FLOAT);
    auto output_dims = output_mat->GetDims();
    std::vector<size_t> shape;
    for(auto dim : output_dims) {
	shape.push_back(dim);
    }
    int stride = sizeof(float);
    std::vector<size_t> strides(output_dims.size());
    for(int i = output_dims.size() - 1; i >=0; --i) {
     	strides[i] = stride;
        stride *= output_dims[i];
    }
    py::array_t<float> output =  py::array_t<float>(
            py::buffer_info(
                output_mat->GetData(),
                sizeof(float), //itemsize
                py::format_descriptor<float>::format(),
                shape.size(), // ndim
		shape, // shape
		strides // strides
           )
    );
    return output;
}

PYBIND11_MODULE(pytnn, m) {
    m.doc() = "pybind11 tnn torch plugin"; // optional module docstring

    m.def("echo", &echo, "A function which show demo", py::arg("i"), py::arg("j"));

    //DataType
    py::enum_<DataType>(m, "DataType")
    .value("DATA_TYPE_AUTO", DataType::DATA_TYPE_AUTO)
    .value("DATA_TYPE_FLOAT", DataType::DATA_TYPE_FLOAT)
    .value("DATA_TYPE_HALF", DataType::DATA_TYPE_HALF)
    .value("DATA_TYPE_INT8", DataType::DATA_TYPE_INT8)
    .value("DATA_TYPE_INT32", DataType::DATA_TYPE_INT32)
    .value("DATA_TYPE_BFP16", DataType::DATA_TYPE_BFP16)
    .value("DATA_TYPE_INT64", DataType::DATA_TYPE_INT64)
    .value("DATA_TYPE_UINT32", DataType::DATA_TYPE_UINT32)
    .export_values();

    //DeviceType
    py::enum_<DeviceType>(m, "DeviceType")
    .value("DEVICE_NAIVE", DeviceType::DEVICE_NAIVE)
    .value("DEVICE_X86", DeviceType::DEVICE_X86)
    .value("DEVICE_ARM", DeviceType::DEVICE_ARM)
    .value("DEVICE_OPENCL", DeviceType::DEVICE_OPENCL)
    .value("DEVICE_METAL", DeviceType::DEVICE_METAL)
    .value("DEVICE_CUDA", DeviceType::DEVICE_CUDA)
    .export_values();

    //MatType
    py::enum_<MatType>(m, "MatType")
    .value("NCHW_FLOAT", MatType::NCHW_FLOAT)
    .export_values();

    //Mat
    py::class_<Mat>(m, "Mat") 
    .def(py::init<DeviceType, MatType, std::vector<int>>())
    .def("GetDims", &Mat::GetDims);

    m.def("load", &Load, "pytnn load");

    py::class_<Module>(m, "Module")
    .def("forward", &Module::Forward);
}

} // TNN_NS
