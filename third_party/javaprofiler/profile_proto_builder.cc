// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "third_party/javaprofiler/display.h"
#include "third_party/javaprofiler/stacktrace_fixer.h"
#include "third_party/javaprofiler/profile_proto_builder.h"

using std::unique_ptr;

namespace google {
namespace javaprofiler {

const int kCount = 0;
const int kMetric = 1;

ProfileProtoBuilder::ProfileProtoBuilder(jvmtiEnv *jvmti_env,
                                         ProfileFrameCache *native_cache,
                                         int64 sampling_rate,
                                         const SampleType &count_type,
                                         const SampleType &metric_type)
    : jvmti_env_(jvmti_env), native_cache_(native_cache),
    location_builder_(&builder_), sampling_rate_(sampling_rate) {
  AddSampleType(count_type);
  AddSampleType(metric_type);
  SetPeriodType(metric_type);
}

void ProfileProtoBuilder::AddTraces(const ProfileStackTrace *traces,
                                    int num_traces) {
  native_cache_->ProcessTraces(traces, num_traces);

  for (int i = 0; i < num_traces; ++i) {
    AddTrace(traces[i], 1);
  }
}

void ProfileProtoBuilder::AddTraces(const ProfileStackTrace *traces,
                                    const int32 *counts,
                                    int num_traces) {
  native_cache_->ProcessTraces(traces, num_traces);

  for (int i = 0; i < num_traces; ++i) {
    AddTrace(traces[i], counts[i]);
  }
}

void ProfileProtoBuilder::AddArtificialTrace(const string& name, int count,
    int sampling_rate) {
  perftools::profiles::Location *location = location_builder_.LocationFor(
      name, name, "", -1);

  auto profile = builder_.mutable_profile();
  auto sample = profile->add_sample();
  sample->add_location_id(location->id());
  InitSampleValues(sample, count, count * sampling_rate);
}

void ProfileProtoBuilder::UnsampleMetrics() {
  auto profile = builder_.mutable_profile();

  for (int i = 0; i < profile->sample_size(); ++i) {
    auto sample = profile->mutable_sample(i);

    auto count = sample->value(kCount);
    auto metric_value = sample->value(kMetric);
    double ratio = CalculateSamplingRatio(sampling_rate_, count, metric_value);

    sample->set_value(kCount, static_cast<double>(count) * ratio);
    sample->set_value(kMetric, static_cast<double>(metric_value) * ratio);
  }
}

unique_ptr<perftools::profiles::Profile>
    ProfileProtoBuilder::CreateSampledProto() {
  builder_.Finalize();
  return unique_ptr<perftools::profiles::Profile>(builder_.Consume());
}

unique_ptr<perftools::profiles::Profile>
    ProfileProtoBuilder::CreateUnsampledProto() {
  UnsampleMetrics();
  builder_.Finalize();
  return unique_ptr<perftools::profiles::Profile>(builder_.Consume());
}

void ProfileProtoBuilder::AddSampleType(const SampleType &sample_type) {
  auto sample_type_proto = builder_.mutable_profile()->add_sample_type();
  sample_type_proto->set_type(builder_.StringId(sample_type.type.c_str()));
  sample_type_proto->set_unit(builder_.StringId(sample_type.unit.c_str()));
}

void ProfileProtoBuilder::SetPeriodType(const SampleType &metric_type) {
  auto period_type = builder_.mutable_profile()->mutable_period_type();
  period_type->set_type(builder_.StringId(metric_type.type.c_str()));
  period_type->set_unit(builder_.StringId(metric_type.unit.c_str()));
}

void ProfileProtoBuilder::UpdateSampleValues(
    perftools::profiles::Sample *sample, jint count, jint size) {
  sample->set_value(kCount, sample->value(kCount) + count);
  sample->set_value(kMetric, sample->value(kMetric) + size);
}

void ProfileProtoBuilder::InitSampleValues(
    perftools::profiles::Sample *sample, jint metric) {
  InitSampleValues(sample, 1, metric);
}

void ProfileProtoBuilder::InitSampleValues(
    perftools::profiles::Sample *sample, jint count, jint metric) {
  sample->add_value(count);
  sample->add_value(metric);
}

void ProfileProtoBuilder::AddTrace(const ProfileStackTrace &trace,
                                   int32 count) {
  auto sample = trace_samples_.SampleFor(*trace.trace);

  if (sample != nullptr) {
    UpdateSampleValues(sample, count, trace.metric_value);
    return;
  }

  auto profile = builder_.mutable_profile();
  sample = profile->add_sample();

  trace_samples_.Add(*trace.trace, sample);

  InitSampleValues(sample, count, trace.metric_value);

  int first_frame = SkipTopNativeFrames(*trace.trace);

  StackState stack_state;

  for (int i = first_frame; i < trace.trace->num_frames; ++i) {
    auto &jvm_frame = trace.trace->frames[i];

    if (jvm_frame.lineno == kNativeFrameLineNum) {
      AddNativeInfo(jvm_frame, profile, sample, &stack_state);
    } else {
      AddJavaInfo(jvm_frame, profile, sample, &stack_state);
    }
  }
}

void ProfileProtoBuilder::AddJavaInfo(
    const google::javaprofiler::JVMPI_CallFrame &jvm_frame,
    perftools::profiles::Profile *profile,
    perftools::profiles::Sample *sample,
    StackState *stack_state) {
  stack_state->JavaFrame();

  if (!jvm_frame.method_id) {
    perftools::profiles::Location *location = location_builder_.LocationFor(
        "", "Unknown method", "", 0);
    sample->add_location_id(location->id());
    return;
  }

  string file_name;
  string class_name;
  string method_name;
  string signature;
  int line_number;
  google::javaprofiler::GetStackFrameElements(jvmti_env_, jvm_frame,
                                              &file_name, &class_name,
                                              &method_name, &signature,
                                              &line_number);

  ::google::javaprofiler::FixMethodParameters(&signature);
  string full_method_name = class_name + "." + method_name + signature;

  perftools::profiles::Location *location = location_builder_.LocationFor(
      class_name, full_method_name, file_name, line_number);

  sample->add_location_id(location->id());
}

void ProfileProtoBuilder::AddNativeInfo(
    const google::javaprofiler::JVMPI_CallFrame &jvm_frame,
    perftools::profiles::Profile *profile,
    perftools::profiles::Sample *sample,
    StackState *stack_state) {
  string function_name = native_cache_->GetFunctionName(jvm_frame);
  perftools::profiles::Location *location =
    native_cache_->GetLocation(jvm_frame,
                               &location_builder_);


  stack_state->NativeFrame(function_name);

  if (!stack_state->SkipFrame()) {
    location->set_address(reinterpret_cast<uint64>(jvm_frame.method_id));
    sample->add_location_id(location->id());
  }
}

size_t LocationBuilder::LocationInfoHash::operator()(
    const LocationInfo &info) const {

  unsigned int h = 1;

  hash<string> hash_string;
  hash<int> hash_int;

  h = 31U * h + hash_string(info.class_name);
  h = 31U * h + hash_string(info.function_name);
  h = 31U * h + hash_string(info.file_name);
  h = 31U * h + hash_int(info.line_number);

  return h;
}

bool LocationBuilder::LocationInfoEquals::operator()(
    const LocationInfo &info1, const LocationInfo &info2) const {
  return info1.class_name == info2.class_name &&
      info1.function_name == info2.function_name &&
      info1.file_name == info2.file_name &&
      info1.line_number == info2.line_number;
}

perftools::profiles::Location *LocationBuilder::LocationFor(
      const string &class_name,
      const string &function_name,
      const string &file_name,
      int line_number) {
  auto profile = builder_->mutable_profile();

  LocationInfo info{ class_name, function_name, file_name, line_number };

  if (locations_.count(info) > 0) {
    return locations_.find(info)->second;
  }

  auto location_id = profile->location_size() + 1;
  perftools::profiles::Location *location = profile->add_location();
  location->set_id(location_id);

  locations_[info] = location;

  auto line = location->add_line();

  // TODO: Handle library name etc too...
  auto function_id = builder_->FunctionId(
      function_name.c_str(), "", file_name.c_str(), 0);

  line->set_function_id(function_id);
  line->set_line(line_number);

  return location;
}

size_t TraceSamples::TraceHash::operator()(const JVMPI_CallTrace &trace) const {
  unsigned int h = 1;
  for (int f = 0; f < trace.num_frames; f++) {
    {
      int len = sizeof(jint);
      char *arr = reinterpret_cast<char *>(&trace.frames[f].lineno);
      for (int i = 0; i < len; i++) {
        h = 31U * h + arr[i];
      }
    }
    {
      int len = sizeof(jmethodID);
      char *arr = reinterpret_cast<char *>(&trace.frames[f].method_id);
      for (int i = 0; i < len; i++) {
        h = 31U * h + arr[i];
      }
    }
  }
  return h;
}

bool TraceSamples::TraceEquals::operator()(
    const JVMPI_CallTrace &trace1, const JVMPI_CallTrace &trace2) const {
  if (trace1.num_frames != trace2.num_frames) {
    return false;
  }

  for (int i = 0; i < trace1.num_frames; ++i) {
    if (trace1.frames[i].method_id != trace2.frames[i].method_id) {
      return false;
    }

    if (trace1.frames[i].lineno != trace2.frames[i].lineno) {
      return false;
    }
  }

  return true;
}

perftools::profiles::Sample *TraceSamples::SampleFor(
    const JVMPI_CallTrace &trace) const {
  auto found = traces_.find(trace);
  if (found == traces_.end()) {
    return nullptr;
  }

  return found->second;
}

void TraceSamples::Add(const JVMPI_CallTrace &trace,
                       perftools::profiles::Sample *sample) {
  traces_[trace] = sample;
}

double CalculateSamplingRatio(int64 rate, int64 count, int64 metric_value) {
  if (rate <= 1) {
    return 1.0;
  }

  if (count < 1) {
    return 1.0;
  }

  double m = static_cast<double>(metric_value);
  double c = static_cast<double>(count);
  double r = static_cast<double>(rate);

  double size = m / c;

  return 1 / (1 - exp(-size / r));
}

std::unique_ptr<ProfileProtoBuilder> ProfileProtoBuilder::ForHeap(
    jvmtiEnv *jvmti_env, int64 sampling_rate, ProfileFrameCache *cache) {
  return std::unique_ptr<ProfileProtoBuilder>(new HeapProfileProtoBuilder(
      jvmti_env, sampling_rate, cache));
}

std::unique_ptr<ProfileProtoBuilder> ProfileProtoBuilder::ForCpu(
    jvmtiEnv *jvmti_env, int64 sampling_rate, ProfileFrameCache *cache) {
  return std::unique_ptr<ProfileProtoBuilder>(
      new CpuProfileProtoBuilder(jvmti_env, sampling_rate, cache));
}

std::unique_ptr<ProfileProtoBuilder> ProfileProtoBuilder::ForContention(
    jvmtiEnv *jvmti_env, int64 sampling_rate, ProfileFrameCache *cache) {
  return std::unique_ptr<ProfileProtoBuilder>(
      new ContentionProfileProtoBuilder(jvmti_env, sampling_rate, cache));
}

}  // namespace javaprofiler
}  // namespace google
