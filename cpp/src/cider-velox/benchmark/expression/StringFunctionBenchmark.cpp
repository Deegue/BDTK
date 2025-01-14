/*
 * Copyright(c) 2022-2023 Intel Corporation.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <folly/Benchmark.h>
#include <gflags/gflags.h>

#include "Allocator.h"
#include "cider/CiderOptions.h"
#include "cider/processor/BatchProcessor.h"
#include "exec/module/batch/ArrowABI.h"
#include "exec/nextgen/context/CodegenContext.h"
#include "util/CiderBitUtils.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/expression/Expr.h"
#include "velox/functions/Registerer.h"
#include "velox/functions/lib/RegistrationHelpers.h"
#include "velox/functions/lib/benchmarks/FunctionBenchmarkBase.h"
#include "velox/functions/prestosql/Arithmetic.h"
#include "velox/functions/prestosql/CheckedArithmetic.h"
#include "velox/functions/prestosql/Comparisons.h"
#include "velox/substrait/VeloxToSubstraitPlan.h"
#include "velox/vector/arrow/Bridge.h"
#include "velox/vector/fuzzer/VectorFuzzer.h"

// #include "velox/functions/Udf.h"
#include "velox/functions/prestosql/registration/RegistrationFunctions.h"

// This file refers velox/velox/benchmarks/basic/SimpleArithmetic.cpp
DEFINE_int64(fuzzer_seed, 99887766, "Seed for random input dataset generator");
DEFINE_double(ratio, 0.5, "NULL ratio in batch");
DEFINE_int64(batch_size, 1000, "batch size for one loop");
DEFINE_int64(loop_count, 100'000, "loop count for benchmark");
DEFINE_bool(dump_ir, false, "dump llvm ir");

using namespace cider::exec::processor;
using namespace cider::exec::nextgen::context;
using namespace facebook::velox;
using namespace facebook::velox::memory;
using namespace facebook::velox::exec;
using namespace facebook::velox::test;
using namespace facebook::velox::plugin;
using namespace facebook::velox::functions;
using namespace facebook::velox::exec::test;
using namespace facebook::velox::substrait;

namespace {
std::pair<ArrowArray*, ArrowSchema*> veloxVectorToArrow(RowVectorPtr vec,
                                                        MemoryPool* pool) {
  for (size_t i = 0; i < vec->childrenSize(); i++) {
    vec->childAt(i)->mutableRawNulls();
  }
  ArrowArray* inputArrowArray = CiderBatchUtils::allocateArrowArray();
  exportToArrow(vec, *inputArrowArray, pool);
  ArrowSchema* inputArrowSchema = CiderBatchUtils::allocateArrowSchema();
  exportToArrow(vec, *inputArrowSchema);

  return {inputArrowArray, inputArrowSchema};
}

class StringFunctionBenchmark : public functions::test::FunctionBenchmarkBase {
 public:
  using ArrowArrayReleaser = void (*)(struct ArrowArray*);
  explicit StringFunctionBenchmark(size_t vectorSize) : FunctionBenchmarkBase() {
    functions::prestosql::registerStringFunctions();

    inputType_ = ROW({{"col_str_10", VARCHAR()},
                      {"col_str_100", VARCHAR()},
                      {"col_str_500", VARCHAR()}});

    vectorSize_ = vectorSize;
    // Generate input data.
    VectorFuzzer::Options opts;
    opts.vectorSize = vectorSize;
    opts.nullRatio = FLAGS_ratio;
    opts.stringVariableLength = false;
    VectorFuzzer fuzzer(opts, pool(), FLAGS_fuzzer_seed);

    std::vector<VectorPtr> children;
    // column 1: 10 bytes;
    opts.stringLength = 10;
    fuzzer.setOptions(opts);
    children.emplace_back(fuzzer.fuzzFlat(VARCHAR()));
    // column 2: 100 bytes;
    opts.stringLength = 100;
    fuzzer.setOptions(opts);
    children.emplace_back(fuzzer.fuzzFlat(VARCHAR()));
    // column 3: 500 bytes;
    opts.stringLength = 500;
    fuzzer.setOptions(opts);
    children.emplace_back(fuzzer.fuzzFlat(VARCHAR()));

    rowVector_ = std::make_shared<RowVector>(
        pool(), inputType_, nullptr, vectorSize, std::move(children));

    ArrowSchema* _schema;
    std::tie(inputArray_, _schema) = veloxVectorToArrow(rowVector_, execCtx_.pool());
    // hack: make processor don't release inputArray_, otherwise we can't use inputArray_
    // multi times.
    inputReleaser_ = inputArray_->release;
    inputArray_->release = nullptr;
    _schema->release(_schema);
  }

  ~StringFunctionBenchmark() { inputReleaser_(inputArray_); }

  size_t veloxCompute(const std::string& expression) {
    folly::BenchmarkSuspender suspender;
    auto exprSet = compileExpression(expression, inputType_);
    suspender.dismiss();

    size_t count = 0;
    for (auto i = 0; i < FLAGS_loop_count; i++) {
      count += evaluate(exprSet, rowVector_)->size();
    }
    return count;
  }

  size_t nextgenCompile(const std::string& expression) {
    folly::BenchmarkSuspender suspender;
    google::protobuf::Arena arena;
    auto veloxPlan = PlanBuilder().values({rowVector_}).project({expression}).planNode();
    std::shared_ptr<VeloxToSubstraitPlanConvertor> v2SPlanConvertor =
        std::make_shared<VeloxToSubstraitPlanConvertor>();
    auto plan = v2SPlanConvertor->toSubstrait(arena, veloxPlan);
    suspender.dismiss();

    auto allocator = std::make_shared<CiderDefaultAllocator>();
    auto context = std::make_shared<BatchProcessorContext>(allocator);
    auto processor = cider::exec::processor::BatchProcessor::Make(plan, context);
    return 1;
  }

  size_t nextgenCompute(const std::string& expression,
                        CodegenOptions cgo = CodegenOptions{}) {
    folly::BenchmarkSuspender suspender;
    google::protobuf::Arena arena;
    auto veloxPlan = PlanBuilder().values({rowVector_}).project({expression}).planNode();
    std::shared_ptr<VeloxToSubstraitPlanConvertor> v2SPlanConvertor =
        std::make_shared<VeloxToSubstraitPlanConvertor>();
    auto plan = v2SPlanConvertor->toSubstrait(arena, veloxPlan);

    cgo.co.dump_ir = FLAGS_dump_ir;
    cgo.co.enable_vectorize = true;
    cgo.co.enable_avx2 = true;
    cgo.co.enable_avx512 = true;

    auto allocator = std::make_shared<CiderDefaultAllocator>();
    auto context = std::make_shared<BatchProcessorContext>(allocator);
    auto processor = cider::exec::processor::BatchProcessor::Make(plan, context, cgo);

    suspender.dismiss();

    size_t rows_size = 0;
    for (auto i = 0; i < FLAGS_loop_count; i++) {
      processor->processNextBatch(inputArray_);

      struct ArrowArray output_array;
      struct ArrowSchema output_schema;

      processor->getResult(output_array, output_schema);
      rows_size += output_array.length;

      output_array.release(&output_array);
      output_schema.release(&output_schema);
    }

    return rows_size;
  }

 private:
  TypePtr inputType_;
  RowVectorPtr rowVector_;
  ArrowArray* inputArray_;
  ArrowArrayReleaser inputReleaser_;
  size_t vectorSize_;
};

std::unique_ptr<StringFunctionBenchmark> benchmark;

// col_str_10, col_str_100, col_str_500; (short, mid, long)

#define GEN_BENCHMARK(name, expr)                                         \
  BENCHMARK(velox_##name) { benchmark->veloxCompute(expr); }              \
  BENCHMARK_RELATIVE(nextgen_##name) { benchmark->nextgenCompute(expr); } \
  BENCHMARK_DRAW_LINE();

// concat
GEN_BENCHMARK(concat_short_short, "concat(col_str_10, col_str_10)");
GEN_BENCHMARK(concat_short_literal, "concat(col_str_10, 'abcdefg123')");

GEN_BENCHMARK(concat_mid_mid, "concat(col_str_100, col_str_100)");
GEN_BENCHMARK(concat_mid_literal, "concat(col_str_100, 'abcdefg123')");

GEN_BENCHMARK(concat_long_long, "concat(col_str_500, col_str_500)");
GEN_BENCHMARK(concat_long_literal, "concat(col_str_500, 'abcdefg123')");

GEN_BENCHMARK(concat_long_short, "concat(col_str_500, col_str_10)");
GEN_BENCHMARK(concat_long_mid, "concat(col_str_500, col_str_100)");
GEN_BENCHMARK(concat_mid_short, "concat(col_str_100, col_str_10)");

GEN_BENCHMARK(concat_short_mid_short,
              "concat(col_str_10, concat(col_str_100, col_str_10))");
GEN_BENCHMARK(concat_short_mid_mid,
              "concat(col_str_10, concat(col_str_100, col_str_100))");
GEN_BENCHMARK(concat_short_mid_long,
              "concat(col_str_10, concat(col_str_100, col_str_500))");

// substring
GEN_BENCHMARK(substring_short_1, "substr(col_str_10, 1, 10)");
GEN_BENCHMARK(substring_short_2, "substr(col_str_10, 5, 5)");

GEN_BENCHMARK(substring_mid_1, "substr(col_str_100, 1, 100)");
GEN_BENCHMARK(substring_mid_2, "substr(col_str_100, 50, 50)");

GEN_BENCHMARK(substring_long_1, "substr(col_str_500, 1, 500)");
GEN_BENCHMARK(substring_long_2, "substr(col_str_500, 50, 100)");
GEN_BENCHMARK(substring_long_3, "substr(col_str_500, 50, 300)");

GEN_BENCHMARK(substring_nested_1, "substr(substr(col_str_500, 50, 300), 20, 100)");

// lower/upper
GEN_BENCHMARK(lower_short, "lower(col_str_10)");
GEN_BENCHMARK(upper_short, "upper(col_str_10)");

GEN_BENCHMARK(lower_mid, "lower(col_str_100)");
GEN_BENCHMARK(upper_mid, "upper(col_str_100)");

GEN_BENCHMARK(lower_long, "lower(col_str_500)");
GEN_BENCHMARK(upper_long, "upper(col_str_500)");

// nested
GEN_BENCHMARK(nested_1, "concat(col_str_10, substr(col_str_100, 10, 20))");
GEN_BENCHMARK(nested_2, "concat(col_str_10, lower(col_str_10))");
GEN_BENCHMARK(nested_3, "concat(col_str_10, upper(col_str_10))");
GEN_BENCHMARK(nested_4, "upper(concat(col_str_10, col_str_100))");
GEN_BENCHMARK(nested_5, "upper(substr(concat(col_str_10, col_str_100), 5, 30))");

}  // namespace

int main(int argc, char* argv[]) {
  gflags::ParseCommandLineFlags(&argc, &argv, true);

  benchmark = std::make_unique<StringFunctionBenchmark>(FLAGS_batch_size);
  folly::runBenchmarks();
  benchmark.reset();
  return 0;
}
