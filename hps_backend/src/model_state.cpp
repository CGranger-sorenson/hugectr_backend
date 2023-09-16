// Copyright (c) 2021, NVIDIA CORPORATION. All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of NVIDIA CORPORATION nor the names of its
//    contributors may be used to endorse or promote products derived
//    from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
// EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <model_state.hpp>
#include <mutex>
#include <numeric>
#include <sstream>
#include <thread>
#include <triton_helpers.hpp>
#include <vector>

namespace triton { namespace backend { namespace hps {

ModelState::ModelState(
    TRITONSERVER_Server* triton_server, TRITONBACKEND_Model* triton_model,
    const char* name, const uint64_t version, uint64_t model_ps_version,
    common::TritonJson::Value&& model_config,
    std::shared_ptr<HugeCTR::HierParameterServerBase> EmbeddingTable_int64,
    HugeCTR::InferenceParams Model_Inference_Para)
    : triton_server_(triton_server), triton_model_(triton_model), name_(name),
      version_(version), version_ps_(model_ps_version),
      model_config_(std::move(model_config)),
      EmbeddingTable_int64(EmbeddingTable_int64),
      Model_Inference_Para(Model_Inference_Para)
{
  // current much model initialization work handled by TritonBackend_Model
}

TRITONSERVER_Error*
ModelState::SetPSModelVersion(uint64_t current_version)
{
  version_ps_ = current_version;
  return nullptr;
}

TRITONSERVER_Error*
ModelState::Create(
    TRITONBACKEND_Model* triton_model, ModelState** state,
    std::shared_ptr<HugeCTR::HierParameterServerBase> EmbeddingTable_int64,
    HugeCTR::InferenceParams Model_Inference_Para, uint64_t model_ps_version)
{
  TRITONSERVER_Message* config_message;
  RETURN_IF_ERROR(TRITONBACKEND_ModelConfig(
      triton_model, 1 /* config_version */, &config_message));

  // We can get the model configuration as a json string from
  // config_message, parse it with our favorite json parser to create
  // DOM that we can access when we need to example the
  // configuration. We use TritonJson, which is a wrapper that returns
  // nice errors (currently the underlying implementation is
  // rapidjson... but others could be added). You can use any json
  // parser you prefer.
  const char* buffer;
  size_t byte_size;
  RETURN_IF_ERROR(
      TRITONSERVER_MessageSerializeToJson(config_message, &buffer, &byte_size));

  common::TritonJson::Value model_config;
  TRITONSERVER_Error* err = model_config.Parse(buffer, byte_size);
  RETURN_IF_ERROR(TRITONSERVER_MessageDelete(config_message));
  RETURN_IF_ERROR(err);

  const char* model_name;
  RETURN_IF_ERROR(TRITONBACKEND_ModelName(triton_model, &model_name));

  uint64_t model_version;
  RETURN_IF_ERROR(TRITONBACKEND_ModelVersion(triton_model, &model_version));

  TRITONSERVER_Server* triton_server;
  RETURN_IF_ERROR(TRITONBACKEND_ModelServer(triton_model, &triton_server));

  *state = new ModelState(
      triton_server, triton_model, model_name, model_version, model_ps_version,
      std::move(model_config), EmbeddingTable_int64, Model_Inference_Para);

  return nullptr;  // success
}

ModelState::~ModelState()
{
  if (support_gpu_cache_ && version_ps_ == version_) {
    EmbeddingTable_int64->destory_embedding_cache_per_model(name_);
    HPS_TRITON_LOG(
        INFO, "******Destorying Embedding Cache for model ", name_,
        " successfully");
  }
  std::cout << "finilize the model state!" << std::endl;
  embedding_cache_map.clear();
  for (auto& ec_refresh_thread : cache_refresh_threads) {
    ec_refresh_thread.join();
  }
  timer.stop();
}

void
ModelState::EmbeddingCacheRefresh(const std::string& model_name, int device_id)
{
  HPS_TRITON_LOG(
      INFO, "The model ", model_name,
      " is refreshing the embedding cache asynchronously on device ", device_id,
      ".");
  if (!freeze_embedding_) {
    EmbeddingTable_int64->update_database_per_model(Model_Inference_Para);
  }
  if (support_gpu_cache_) {
    EmbeddingTable_int64->refresh_embedding_cache(model_name, device_id);
  }
  HPS_TRITON_LOG(
      INFO, "The model ", model_name,
      " has completed the asynchronous refresh of the embedding cache on "
      "device ",
      device_id, ".");
}

void
ModelState::Refresh_Embedding_Cache()
{
  // refresh embedding cache once after delay time
  int64_t count = gpu_shape.size();
  uint64_t exec_start_ns = 0;
  SET_TIMESTAMP(exec_start_ns);
  for (int i = 0; i < count; i++) {
    if (support_gpu_cache_) {
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("The model ") + name_ +
           std::string(" is periodically refreshing the embedding cache "
                       "asynchronously on device ") +
           std::to_string(gpu_shape[i]))
              .c_str());
      EmbeddingTable_int64->refresh_embedding_cache(name_, gpu_shape[i]);
      LOG_MESSAGE(
          TRITONSERVER_LOG_INFO,
          (std::string("The model ") + name_ +
           std::string(
               " has refreshed the embedding cache asynchronously on device ") +
           std::to_string(gpu_shape[i]))
              .c_str());
    }
  }
  uint64_t exec_end_ns = 0;
  SET_TIMESTAMP(exec_end_ns);
  int64_t exe_time = (exec_end_ns - exec_start_ns) / 1000000;
  LOG_MESSAGE(
      TRITONSERVER_LOG_INFO,
      (std::string("Refresh embedding table execution time is ") +
       std::to_string(exe_time) + " ms")
          .c_str());
}

TRITONSERVER_Error*
ModelState::ValidateModelConfig()
{
  // We have the json DOM for the model configuration...
  {
    common::TritonJson::WriteBuffer tmp;
    RETURN_IF_ERROR(model_config_.PrettyWrite(&tmp));
    HPS_TRITON_LOG(INFO, "Verifying model configuration: ", tmp.Contents());
  }

  // There must be 3 inputs.
  {
    common::TritonJson::Value inputs;
    RETURN_IF_ERROR(model_config_.MemberAsArray("input", &inputs));
    HPS_RETURN_TRITON_ERROR_IF_FALSE(
        inputs.ArraySize() == 2, INVALID_ARG, "expect 2 input, got ",
        inputs.ArraySize());

    for (size_t i = 0; i < 2; i++) {
      common::TritonJson::Value input;
      RETURN_IF_ERROR(inputs.IndexAsObject(i, &input));

      // Input name.
      std::string name;
      RETURN_IF_ERROR(TritonJsonHelper::parse(name, input, "name", true));
      HPS_RETURN_TRITON_ERROR_IF_FALSE(
          GetInputmap().count(name) > 0, INVALID_ARG,
          "expected input name as KEYS and NUMKEYS, but got ", name);

      // Datatype.
      std::string data_type;
      RETURN_IF_ERROR(
          TritonJsonHelper::parse(data_type, input, "data_type", true));
      if (name == "KEYS") {
        HPS_RETURN_TRITON_ERROR_IF_FALSE(
            data_type == "TYPE_INT64", INVALID_ARG,
            "expected KEYS input datatype as TYPE_INT64, "
            "got ",
            data_type);
      } else if (name == "NUMKEYS") {
        HPS_RETURN_TRITON_ERROR_IF_FALSE(
            data_type == "TYPE_INT32", INVALID_ARG,
            "expected NUMKEYS input datatype as TYPE_FP32, got ", data_type);
      }

      // Input shape.
      std::vector<int64_t> shape;
      RETURN_IF_ERROR(backend::ParseShape(input, "dims", &shape));
      HPS_RETURN_TRITON_ERROR_IF_FALSE(
          shape[0] == -1, INVALID_ARG, "expected input shape equal -1, got ",
          backend::ShapeToString(shape));
    }
  }

  // And there must be 1 output.
  {
    common::TritonJson::Value outputs;
    RETURN_IF_ERROR(model_config_.MemberAsArray("output", &outputs));
    HPS_RETURN_TRITON_ERROR_IF_FALSE(
        outputs.ArraySize() == 1, INVALID_ARG, "expect 1 output, got ",
        outputs.ArraySize());

    common::TritonJson::Value output;
    RETURN_IF_ERROR(outputs.IndexAsObject(0, &output));

    std::string data_type;
    RETURN_IF_ERROR(
        TritonJsonHelper::parse(data_type, output, "data_type", true));
    HPS_RETURN_TRITON_ERROR_IF_FALSE(
        data_type == "TYPE_FP32", INVALID_ARG,
        "expected  output datatype as TYPE_FP32, got ", data_type);

    // output must have -1 shape
    std::vector<int64_t> shape;
    RETURN_IF_ERROR(backend::ParseShape(output, "dims", &shape));
    HPS_RETURN_TRITON_ERROR_IF_FALSE(
        shape[0] == -1, INVALID_ARG, "expected  output shape equal -1, got ",
        backend::ShapeToString(shape));
  }

  return nullptr;  // success
}

TRITONSERVER_Error*
ModelState::ParseModelConfig()
{
  common::TritonJson::WriteBuffer buffer;
  RETURN_IF_ERROR(model_config_.PrettyWrite(&buffer));
  HPS_TRITON_LOG(INFO, "The model configuration: ", buffer.Contents());

  // Get HugeCTR model configuration
  common::TritonJson::Value instance_group;
  RETURN_IF_ERROR(
      model_config_.MemberAsArray("instance_group", &instance_group));
  HPS_RETURN_TRITON_ERROR_IF_FALSE(
      instance_group.ArraySize() > 0, INVALID_ARG,
      "expect at least one instance in instance group , got ",
      instance_group.ArraySize());
  support_gpu_cache_ = Model_Inference_Para.use_gpu_embedding_cache;
  use_mixed_precision_ = Model_Inference_Para.use_mixed_precision;
  support_int64_key_ = Model_Inference_Para.i64_input_key;
  for (size_t i = 0; i < instance_group.ArraySize(); i++) {
    common::TritonJson::Value instance;
    RETURN_IF_ERROR(instance_group.IndexAsObject(i, &instance));

    std::string kind;
    RETURN_IF_ERROR(instance.MemberAsString("kind", &kind));
    if (Model_Inference_Para.use_gpu_embedding_cache) {
      HPS_RETURN_TRITON_ERROR_IF_FALSE(
          kind == "KIND_GPU", INVALID_ARG,
          "expect GPU kind instance in instance group , got ", kind);
      std::vector<int64_t> gpu_list;
      RETURN_IF_ERROR(backend::ParseShape(instance, "gpus", &gpu_list));
      for (auto id : gpu_list) {
        gpu_shape.push_back(id);
      }
    } else {
      gpu_shape.push_back(0);
    }

    int64_t count;
    RETURN_IF_ERROR(instance.MemberAsInt("count", &count));
    RETURN_ERROR_IF_TRUE(
        count > Model_Inference_Para.number_of_worker_buffers_in_pool,
        TRITONSERVER_ERROR_INVALID_ARG,
        std::string("expect the number of instance(in instance_group) larger "
                    "than number_of_worker_buffers_in_pool that confifured in "
                    "Parameter Server json file , got ") +
            std::to_string(count));
  }

  // Parse HugeCTR model customized configuration.
  common::TritonJson::Value parameters;
  if (model_config_.Find("parameters", &parameters)) {
    common::TritonJson::Value value;
    if (parameters.Find("refresh_interval", &value)) {
      RETURN_IF_ERROR(TritonJsonHelper::parse(
          refresh_interval_, value, "string_value", false));
    } else {
      refresh_interval_ = Model_Inference_Para.refresh_interval;
    }
    HPS_TRITON_LOG(INFO, "refresh_interval = ", refresh_interval_);

    if (parameters.Find("refresh_delay", &value)) {
      RETURN_IF_ERROR(TritonJsonHelper::parse(
          refresh_delay_, value, "string_value", false));
    } else {
      refresh_delay_ = Model_Inference_Para.refresh_delay;
    }
    HPS_TRITON_LOG(INFO, "refresh_delay = ", refresh_delay_);

    if (parameters.Find("freeze_sparse", &value)) {
      RETURN_IF_ERROR(TritonJsonHelper::parse(
          freeze_embedding_, value, "string_value", false));
    }
  }

  if (Model_Inference_Para.maxnum_catfeature_query_per_table_per_sample.size() >
      0) {
    cat_num_ = accumulate(
        Model_Inference_Para.maxnum_catfeature_query_per_table_per_sample
            .begin(),
        Model_Inference_Para.maxnum_catfeature_query_per_table_per_sample.end(),
        0);
  }

  if (cat_num_ <= 0) {
    return HPS_TRITON_ERROR(
        INVALID_ARG, "expected at least one categorical feature, got ",
        cat_num_);
  }

  if (Model_Inference_Para.embedding_vecsize_per_table.size() > 0) {
    embedding_size_ = accumulate(
        Model_Inference_Para.embedding_vecsize_per_table.begin(),
        Model_Inference_Para.embedding_vecsize_per_table.end(), 0);
  }


  model_config_.MemberAsInt("max_batch_size", &max_batch_size_);
  HPS_RETURN_TRITON_ERROR_IF_FALSE(
      max_batch_size_ >= 0, INVALID_ARG,
      "expected max_batch_size should greater than or equal to 0 ",
      "(the configuration should be consistent in Parameter Server json file "
      "and config.pbtxt file), got ",
      max_batch_size_);
  max_batch_size_ = Model_Inference_Para.max_batchsize;
  HPS_TRITON_LOG(
      INFO, "max_batch_size in model config.pbtxt is ", max_batch_size_);

  return nullptr;
}

TRITONSERVER_Error*
ModelState::Create_EmbeddingCache()
{
  int64_t count = gpu_shape.size();
  if (count > 0 && support_gpu_cache_) {
    if (support_int64_key_ &&
        EmbeddingTable_int64->get_embedding_cache(name_, gpu_shape[0]) ==
            nullptr &&
        embedding_cache_map.find(gpu_shape[0]) == embedding_cache_map.end()) {
      HPS_TRITON_LOG(
          INFO, "Parsing network file of ", name_,
          ", which will be used for online deployment. The network file path "
          "is ",
          hugectr_config_);
      HPS_TRITON_LOG(
          INFO, "Update Database of Parameter Server for model ", name_);
      EmbeddingTable_int64->update_database_per_model(Model_Inference_Para);
      HPS_TRITON_LOG(INFO, "Create embedding cache for model ", name_);
      EmbeddingTable_int64->create_embedding_cache_per_model(
          Model_Inference_Para);
    }
  }
  for (int i = 0; i < count; i++) {
    std::vector<int>::iterator iter = find(
        Model_Inference_Para.deployed_devices.begin(),
        Model_Inference_Para.deployed_devices.end(), gpu_shape[i]);
    HPS_RETURN_TRITION_ERROR_IF_TRUE(
        iter == Model_Inference_Para.deployed_devices.end(), INVALID_ARG,
        "Please confirm that device ", gpu_shape[i],
        " is added to 'deployed_device_list' in the ps configuration file");

    if (embedding_cache_map.find(gpu_shape[i]) == embedding_cache_map.end()) {
      HPS_TRITON_LOG(
          INFO, "******Creating Embedding Cache for model ", name_,
          " in device ", gpu_shape[i]);
      if (support_int64_key_) {
        Model_Inference_Para.device_id = gpu_shape[i];
        embedding_cache_map[gpu_shape[i]] =
            EmbeddingTable_int64->get_embedding_cache(name_, gpu_shape[i]);
      }
      if (version_ps_ > 0 && version_ps_ != version_) {
        timer.startonce(
            0,
            std::bind(
                &ModelState::EmbeddingCacheRefresh, this, name_, gpu_shape[i]));
      }
    }
  }

  if (refresh_interval_ > 1e-6) {
    // refresh embedding cache once based on period time
    timer.start(
        refresh_interval_,
        std::bind(&ModelState::Refresh_Embedding_Cache, this));
  }
  HPS_TRITON_LOG(
      INFO, "******Creating Embedding Cache for model ", name_,
      " successfully");
  return nullptr;
}

}}}  // namespace triton::backend::hps