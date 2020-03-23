//   Copyright (c) 2020 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TORCH_ESAGENT_H
#define TORCH_ESAGENT_H

#include <memory>
#include <string>
#include "optimizer.h"
#include "utils.h"
#include "gaussian_sampling.h"
#include "deepes.pb.h"

namespace DeepES{

/* DeepES agent for Torch.
 * Our implemtation is flexible to support any model that subclass torch::nn::Module.
 * That is, we can instantiate an agent by: es_agent = ESAgent<Model>(model);
 * After that, users can clone an agent for multi-thread processing, add parametric noise for exploration,
 * and update the parameteres, according to the evaluation resutls of noisy parameters.
 *
 */
template <class T>
class ESAgent{
public:
  ESAgent() {}

  ~ESAgent() {
    delete[] _noise;
    if (!_is_sampling_agent)
      delete[] _neg_gradients;
  }

  ESAgent(std::shared_ptr<T> model, std::string config_path): _model(model) {
    _is_sampling_agent = false;
    _config = std::make_shared<DeepESConfig>();
    load_proto_conf(config_path, *_config);
    _sampling_method = std::make_shared<GaussianSampling>();
    _sampling_method->load_config(*_config);
    _optimizer = std::make_shared<SGDOptimizer>(_config->optimizer().base_lr());
    // Origin agent can't be used to sample, so keep it same with _model for evaluating.
    _sampled_model = model;
    _param_size = _calculate_param_size();

    _noise = new float [_param_size];
    _neg_gradients = new float [_param_size];
  }

  std::shared_ptr<ESAgent> clone() {
    std::shared_ptr<ESAgent> new_agent = std::make_shared<ESAgent>();

    new_agent->_model = _model;
    std::shared_ptr<T> new_model = _model->clone();
    new_agent->_sampled_model = new_model;
  
    new_agent->_is_sampling_agent = true;
    new_agent->_sampling_method = _sampling_method;
    new_agent->_param_size = _param_size;

    float* new_noise = new float [_param_size];
    new_agent->_noise = new_noise;

    return new_agent;
  }

  torch::Tensor predict(const torch::Tensor& x) {
    return _sampled_model->forward(x);
  }

  bool update(std::vector<SamplingKey>& noisy_keys, std::vector<float>& noisy_rewards) {
    if (_is_sampling_agent) {
      LOG(ERROR) << "[DeepES] Cloned ESAgent cannot call update function, please use original ESAgent.";
      return false;
    }

    compute_centered_ranks(noisy_rewards);

    memset(_neg_gradients, 0, _param_size * sizeof(float));
    for (int i = 0; i < noisy_keys.size(); ++i) {
      int key = noisy_keys[i].key(0);
      float reward = noisy_rewards[i];
      bool success = _sampling_method->resampling(key, _noise, _param_size);
      for (int64_t j = 0; j < _param_size; ++j) {
        _neg_gradients[j] += _noise[j] * reward;
      }
    }
    for (int64_t j = 0; j < _param_size; ++j) {
      _neg_gradients[j] /= -1.0 * noisy_keys.size();
    }

    //update
    auto params = _model->named_parameters();
    int64_t counter = 0;
    for (auto& param: params) {
      torch::Tensor tensor = param.value().view({-1});
      auto tensor_a = tensor.accessor<float,1>();
      _optimizer->update(tensor_a, _neg_gradients+counter, tensor.size(0));
      counter += tensor.size(0);
    }

    return true;
  }

  bool add_noise(SamplingKey& sampling_key) {
    if (!_is_sampling_agent) {
      LOG(ERROR) << "[DeepES] Original ESAgent cannot call add_noise function, please use cloned ESAgent.";
      return false;
    }

    auto sampled_params = _sampled_model->named_parameters();
    auto params = _model->named_parameters();
    int key = _sampling_method->sampling(_noise, _param_size);
    sampling_key.add_key(key);
    int64_t counter = 0;
    for (auto& param: sampled_params) {
      torch::Tensor sampled_tensor = param.value().view({-1});
      std::string param_name = param.key();
      torch::Tensor tensor = params.find(param_name)->view({-1});
      auto sampled_tensor_a = sampled_tensor.accessor<float,1>();
      auto tensor_a = tensor.accessor<float,1>();
      for (int64_t j = 0; j < tensor.size(0); ++j) {
        sampled_tensor_a[j] = tensor_a[j] + _noise[counter + j];
      }
      counter += tensor.size(0);
    }
    return true;
  }

  

private:
  std::shared_ptr<T> _sampled_model;
  std::shared_ptr<T> _model;
  bool _is_sampling_agent;
  std::shared_ptr<SamplingMethod> _sampling_method;
  std::shared_ptr<Optimizer> _optimizer;
  std::shared_ptr<DeepESConfig> _config;
  int64_t _param_size;
  // malloc memory of noise and neg_gradients in advance.
  float* _noise;
  float* _neg_gradients;

  int64_t _calculate_param_size() {
    auto params = _model->named_parameters();
    for (auto& param: params) {
      torch::Tensor tensor = param.value().view({-1});
      _param_size += tensor.size(0);
    }
    return _param_size;
  }
};

}

#endif /* TORCH_ESAGENT_H */
