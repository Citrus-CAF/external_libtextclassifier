/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lang_id/lang-id.h"

#include <stdio.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "common/algorithm.h"
#include "common/embedding-network-params-from-proto.h"
#include "common/embedding-network.pb.h"
#include "common/embedding-network.h"
#include "common/feature-extractor.h"
#include "common/file-utils.h"
#include "common/list-of-strings.pb.h"
#include "common/memory_image/in-memory-model-data.h"
#include "common/mmap.h"
#include "common/softmax.h"
#include "common/task-context.h"
#include "lang_id/custom-tokenizer.h"
#include "lang_id/lang-id-brain-interface.h"
#include "lang_id/language-identifier-features.h"
#include "lang_id/light-sentence-features.h"
#include "lang_id/light-sentence.h"
#include "lang_id/relevant-script-feature.h"
#include "util/base/logging.h"
#include "util/base/macros.h"

using ::libtextclassifier::nlp_core::file_utils::ParseProtoFromMemory;

namespace libtextclassifier {
namespace nlp_core {
namespace lang_id {

namespace {
// Default value for the probability threshold; see comments for
// LangId::SetProbabilityThreshold().
static const float kDefaultProbabilityThreshold = 0.50;

// Initial value for the default language for LangId::FindLanguage().  The
// default language can be changed (for an individual LangId object) using
// LangId::SetDefaultLanguage().
static const char kInitialDefaultLanguage[] = "";
}  // namespace

// Class that performs all work behind LangId.
class LangIdImpl {
 public:
  explicit LangIdImpl(const std::string &filename) {
    // Using mmap as a fast way to read the model bytes.
    ScopedMmap scoped_mmap(filename);
    MmapHandle mmap_handle = scoped_mmap.handle();
    Initialize(mmap_handle);
  }

  explicit LangIdImpl(int fd) {
    // Using mmap as a fast way to read the model bytes.
    ScopedMmap scoped_mmap(fd);
    MmapHandle mmap_handle = scoped_mmap.handle();
    Initialize(mmap_handle);
  }

  void Initialize(const MmapHandle &mmap_handle) {
    // Will set valid_ to true only on successful initialization.
    valid_ = false;

    // Make sure all relevant features are registered:
    ContinuousBagOfNgramsFunction::RegisterClass();
    RelevantScriptFeature::RegisterClass();

    if (!mmap_handle.ok()) {
      TC_LOG(ERROR) << "Unable to read model bytes.";
      return;
    }
    StringPiece model_bytes = mmap_handle.to_stringpiece();

    // NOTE(salcianu): code below relies on the fact that the current features
    // do not rely on data from a TaskInput.  Otherwise, one would have to use
    // the more complex model registration mechanism, which requires more code.
    InMemoryModelData model_data(model_bytes);
    TaskContext context;
    if (!model_data.GetTaskSpec(context.mutable_spec())) {
      TC_LOG(ERROR) << "Unable to get model TaskSpec";
      return;
    }

    if (!ParseNetworkParams(model_data, &context)) {
      return;
    }
    if (!ParseListOfKnownLanguages(model_data, &context)) {
      return;
    }

    network_.reset(new EmbeddingNetwork(network_params_.get()));
    if (!network_->is_valid()) {
      return;
    }

    probability_threshold_ =
        context.Get("reliability_thresh", kDefaultProbabilityThreshold);
    if (!lang_id_brain_interface_.Init(&context)) {
      return;
    }
    valid_ = true;
  }

  void SetProbabilityThreshold(float threshold) {
    probability_threshold_ = threshold;
  }

  void SetDefaultLanguage(const std::string &lang) { default_language_ = lang; }

  std::string FindLanguage(const std::string &text) const {
    std::vector<float> scores = ScoreLanguages(text);
    if (scores.empty()) {
      return default_language_;
    }

    // Softmax label with max score.
    int label = GetArgMax(scores);
    float probability = scores[label];
    if (probability < probability_threshold_) {
      return default_language_;
    }
    return GetLanguageForSoftmaxLabel(label);
  }

  std::vector<std::pair<std::string, float>> FindLanguages(
      const std::string &text) const {
    std::vector<float> scores = ScoreLanguages(text);

    std::vector<std::pair<std::string, float>> result;
    for (int i = 0; i < scores.size(); i++) {
      result.push_back({GetLanguageForSoftmaxLabel(i), scores[i]});
    }
    return result;
  }

  std::vector<float> ScoreLanguages(const std::string &text) const {
    if (!is_valid()) {
      return {};
    }

    // Create a Sentence storing the input text.
    LightSentence sentence;
    TokenizeTextForLangId(text, &sentence);

    // TODO(salcianu): reuse vector<FeatureVector>.
    std::vector<FeatureVector> features(
        lang_id_brain_interface_.NumEmbeddings());
    lang_id_brain_interface_.GetFeatures(&sentence, &features);

    // Predict language.
    EmbeddingNetwork::Vector scores;
    network_->ComputeFinalScores(features, &scores);

    return ComputeSoftmax(scores);
  }

  bool is_valid() const { return valid_; }

 private:
  // Returns name of the (in-memory) file for the indicated TaskInput from
  // context.
  static std::string GetInMemoryFileNameForTaskInput(
      const std::string &input_name, TaskContext *context) {
    TaskInput *task_input = context->GetInput(input_name);
    if (task_input->part_size() != 1) {
      TC_LOG(ERROR) << "TaskInput " << input_name << " has "
                    << task_input->part_size() << " parts";
      return "";
    }
    return task_input->part(0).file_pattern();
  }

  bool ParseNetworkParams(const InMemoryModelData &model_data,
                          TaskContext *context) {
    const std::string input_name = "language-identifier-network";
    const std::string input_file_name =
        GetInMemoryFileNameForTaskInput(input_name, context);
    if (input_file_name.empty()) {
      TC_LOG(ERROR) << "No input file name for TaskInput " << input_name;
      return false;
    }
    StringPiece bytes = model_data.GetBytesForInputFile(input_file_name);
    if (bytes.data() == nullptr) {
      TC_LOG(ERROR) << "Unable to get bytes for TaskInput " << input_name;
      return false;
    }
    std::unique_ptr<EmbeddingNetworkProto> proto(new EmbeddingNetworkProto());
    if (!ParseProtoFromMemory(bytes, proto.get())) {
      TC_LOG(ERROR) << "Unable to parse EmbeddingNetworkProto";
      return false;
    }
    network_params_.reset(
        new EmbeddingNetworkParamsFromProto(std::move(proto)));
    if (!network_params_->is_valid()) {
      TC_LOG(ERROR) << "EmbeddingNetworkParamsFromProto not valid";
      return false;
    }
    return true;
  }

  // Parses dictionary with known languages (i.e., field languages_) from a
  // TaskInput of context.  Note: that TaskInput should be a ListOfStrings proto
  // with a single element, the serialized form of a ListOfStrings.
  //
  // TC_STRIP
  //
  // Indeed, we have two nested ListOfStrings.  Here's why:
  //
  // The first (outermost) ListOfStrings: in the original model trained by our
  // workflow (on server side), we had a recordio file with a single record (the
  // serialized dictionary); we don't have simple code to read recordio on
  // mobile; instead, we convert a recordio file into a list of strings, one
  // string for each record.
  //
  // The inner ListOfStrings: our server-side code uses a specialized proto
  // (DictionaryProto).  That has the same wire-format as ListOfStrings: to save
  // code size on mobile, we reuse ListOfStrings.
  //
  // TC_END_STRIP
  bool ParseListOfKnownLanguages(const InMemoryModelData &model_data,
                                 TaskContext *context) {
    const std::string input_name = "language-name-id-map";
    const std::string input_file_name =
        GetInMemoryFileNameForTaskInput(input_name, context);
    if (input_file_name.empty()) {
      TC_LOG(ERROR) << "No input file name for TaskInput " << input_name;
      return false;
    }
    StringPiece bytes = model_data.GetBytesForInputFile(input_file_name);
    if (bytes.data() == nullptr) {
      TC_LOG(ERROR) << "Unable to get bytes for TaskInput " << input_name;
      return false;
    }
    ListOfStrings records;
    if (!ParseProtoFromMemory(bytes, &records)) {
      TC_LOG(ERROR) << "Unable to parse ListOfStrings from TaskInput "
                    << input_name;
      return false;
    }
    if (records.element_size() != 1) {
      TC_LOG(ERROR) << "Wrong number of records in TaskInput " << input_name
                    << " : " << records.element_size();
      return false;
    }
    if (!ParseProtoFromMemory(std::string(records.element(0)), &languages_)) {
      TC_LOG(ERROR) << "Unable to parse dictionary with known languages";
      return false;
    }
    return true;
  }

  // Returns language code for a softmax label.  See comments for languages_
  // field.  If label is out of range, returns default_language_.
  std::string GetLanguageForSoftmaxLabel(int label) const {
    if ((label >= 0) && (label < languages_.element_size())) {
      return languages_.element(label);
    } else {
      TC_LOG(ERROR) << "Softmax label " << label << " outside range [0, "
                    << languages_.element_size() << ")";
      return default_language_;
    }
  }

  LangIdBrainInterface lang_id_brain_interface_;

  // Parameters for the neural network network_ (see below).
  std::unique_ptr<EmbeddingNetworkParamsFromProto> network_params_;

  // Neural network to use for scoring.
  std::unique_ptr<EmbeddingNetwork> network_;

  // True if this object is ready to perform language predictions.
  bool valid_;

  // Only predictions with a probability (confidence) above this threshold are
  // reported.  Otherwise, we report default_language_.
  float probability_threshold_ = kDefaultProbabilityThreshold;

  // Known languages: softmax label i (an integer) means languages_.element(i)
  // (something like "en", "fr", "ru", etc).
  ListOfStrings languages_;

  // Language code to return in case of errors.
  std::string default_language_ = kInitialDefaultLanguage;

  TC_DISALLOW_COPY_AND_ASSIGN(LangIdImpl);
};

LangId::LangId(const std::string &filename) : pimpl_(new LangIdImpl(filename)) {
  if (!pimpl_->is_valid()) {
    TC_LOG(ERROR) << "Unable to construct a valid LangId based "
                  << "on the data from " << filename
                  << "; nothing should crash, but "
                  << "accuracy will be bad.";
  }
}

LangId::LangId(int fd) : pimpl_(new LangIdImpl(fd)) {
  if (!pimpl_->is_valid()) {
    TC_LOG(ERROR) << "Unable to construct a valid LangId based "
                  << "on the data from descriptor " << fd
                  << "; nothing should crash, "
                  << "but accuracy will be bad.";
  }
}

LangId::~LangId() = default;

void LangId::SetProbabilityThreshold(float threshold) {
  pimpl_->SetProbabilityThreshold(threshold);
}

void LangId::SetDefaultLanguage(const std::string &lang) {
  pimpl_->SetDefaultLanguage(lang);
}

std::string LangId::FindLanguage(const std::string &text) const {
  return pimpl_->FindLanguage(text);
}

std::vector<std::pair<std::string, float>> LangId::FindLanguages(
    const std::string &text) const {
  return pimpl_->FindLanguages(text);
}

bool LangId::is_valid() const { return pimpl_->is_valid(); }

}  // namespace lang_id
}  // namespace nlp_core
}  // namespace libtextclassifier