//
// Copyright 2019 Google LLC
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
//

#ifndef ZETASQL_REFERENCE_IMPL_REFERENCE_DRIVER_H_
#define ZETASQL_REFERENCE_IMPL_REFERENCE_DRIVER_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "google/protobuf/compiler/importer.h"
#include "zetasql/compliance/test_driver.h"
#include "zetasql/public/language_options.h"
#include "zetasql/public/options.pb.h"
#include "zetasql/public/simple_catalog.h"
#include "zetasql/public/type.h"
#include "zetasql/public/value.h"
#include "absl/status/status.h"
#include "zetasql/base/statusor.h"
#include "absl/time/time.h"
#include "zetasql/base/ret_check.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_builder.h"

namespace zetasql {

// Implements the test driver for the reference implementation. It is used in
// compliance tests to ensure the conformance of the reference implementation
// and can be used for comparing the statement results produced by individual
// engines with those produced by the reference implementation.
//
// The reference implementation can run queries as of different ZetaSQL
// language versions or operations, according to LanguageOptions.
class ReferenceDriver : public TestDriver {
 public:
  // Options for ExecuteStatement.
  struct ExecuteStatementOptions {
    PrimaryKeyMode primary_key_mode = PrimaryKeyMode::DEFAULT;
  };

  ReferenceDriver();
  explicit ReferenceDriver(const LanguageOptions& options);
  ReferenceDriver(const ReferenceDriver&) = delete;
  ReferenceDriver& operator=(const ReferenceDriver&) = delete;
  ~ReferenceDriver() override;

  LanguageOptions GetSupportedLanguageOptions() override {
    return language_options_;
  }

  // The ReferenceDriver has some extra work to do in addition to the normal
  // TestDriver workflow.
  //
  // The ReferenceDriver is used to produce new tables for other test drivers.
  // Tables are represented as Value objects and the ReferenceDriver executes
  // queries to produce Value objects. Although we can call
  // CreateDatabase(TestDatabase) to create tables on a test driver, we cannot
  // do the same to the ReferenceDriver. This is because
  // CreateDatabase(TestDatabase) will reset the type factory and invalidate
  // all existing Value objects.
  //
  // Instead, the ReferenceDriver needs to add tables incrementally to an
  // existing database.
  //
  // Because tables created by the ReferenceDriver may have proto or enum typed
  // Values, it is convenient to be able to load proto and enum types
  // incrementally, as well.
  //
  // The overall workflow for the ReferenceDriver in terms of method signatures
  // is:
  //   1. CreateDatabase(TestDatabase{}) x 1
  //   2. LoadProtoEnumTypes() x n
  //   3. AddTable() x m

  // Incrementally add a table to bypass resetting type factory.
  void AddTable(const std::string& table_name, const TestTable& table);

  // Incrementally loads proto and enum types.
  absl::Status LoadProtoEnumTypes(const std::set<std::string>& filenames,
                                  const std::set<std::string>& proto_names,
                                  const std::set<std::string>& enum_names);

  // Must be called prior to ExecuteQuery().
  absl::Status CreateDatabase(const TestDatabase& test_db) override;

  // Set the current LanguageOptions, which will control what features and
  // functions are available and how they behave.
  // This can be called between ExecuteQuery calls to change options.
  void SetLanguageOptions(const LanguageOptions& options);

  // Implements TestDriver::ExecuteStatement(), which documents that this method
  // is not supposed be called because IsReferenceImplementation() returns true.
  zetasql_base::StatusOr<Value> ExecuteStatement(
      const std::string& sql, const std::map<std::string, Value>& parameters,
      TypeFactory* type_factory) override {
    ZETASQL_RET_CHECK_FAIL()
        << "ExecuteStatement() is not supported for the reference "
        << "implementation; call  "
        << "ReferenceDriver::ExecuteStatementForReferenceDriver() instead";
  }

  // The same as TestDriver::ExecuteStatement(), but with more arguments. Uses
  // INVALID_ARGUMENT errors to represent parser/analyzer errors and
  // OUT_OF_RANGE to represent runtime errors.
  //
  // 'is_deterministic_output' must not be null. When reference evaluation
  //     succeeds, this will be set to 'false' if the reference evluation engine
  //     detected non-determinism in the query result and true otherwise.
  // 'uses_unsupported_type' must not be null. When the reference driver fails
  //     the test because it detects use of types not supported by the current
  //     language options, this is set to true. Otherwise it is set to false.
  //     Currently only the output type of the query is checked for unsupported
  //     types.
  zetasql_base::StatusOr<Value> ExecuteStatementForReferenceDriver(
      const std::string& sql, const std::map<std::string, Value>& parameters,
      const ExecuteStatementOptions& options, TypeFactory* type_factory,
      bool* is_deterministic_output, bool* uses_unsupported_type);

  bool IsReferenceImplementation() const override { return true; }

  // Sets a new query evalution duration that is less than
  // --reference_driver_query_eval_timeout_sec or returns an error.
  absl::Status SetStatementEvaluationTimeout(absl::Duration timeout) override;

  // Returns a pointer to the owned catalog.
  SimpleCatalog* catalog() const { return catalog_.get(); }

  // Returns a pointer to the owned reference type factory.
  TypeFactory* type_factory() { return type_factory_.get(); }

  const absl::TimeZone GetDefaultTimeZone() const override;
  absl::Status SetDefaultTimeZone(const std::string& time_zone) override;

 private:
  struct TableInfo {
    std::string table_name;
    std::set<LanguageFeature> required_features;
    bool is_value_table;
    Value array;
  };

  std::unique_ptr<TypeFactory> type_factory_;
  LanguageOptions language_options_;
  std::vector<TableInfo> tables_;
  std::unique_ptr<SimpleCatalog> catalog_;

  std::vector<std::string> errors_;
  std::unique_ptr<google::protobuf::compiler::SourceTree> proto_source_tree_;
  std::unique_ptr<google::protobuf::compiler::MultiFileErrorCollector>
      proto_error_collector_;
  std::unique_ptr<google::protobuf::compiler::Importer> importer_;

  // Defaults to America/Los_Angeles.
  absl::TimeZone default_time_zone_;
  absl::Duration statement_evaluation_timeout_;

  // The name of dumping catalog for fuzz testing.
  std::string fuzzing_catalog_name_;
};

}  // namespace zetasql

#endif  // ZETASQL_REFERENCE_IMPL_REFERENCE_DRIVER_H_
