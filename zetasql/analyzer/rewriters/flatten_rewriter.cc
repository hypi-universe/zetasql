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

#include <memory>

#include "zetasql/parser/parser.h"
#include "zetasql/public/analyzer.h"
#include "zetasql/public/types/type_factory.h"
#include "zetasql/resolved_ast/resolved_ast.h"
#include "zetasql/resolved_ast/resolved_ast_deep_copy_visitor.h"
#include "zetasql/resolved_ast/rewrite_utils.h"
#include "zetasql/resolved_ast/validator.h"
#include "absl/status/status.h"

namespace zetasql {
namespace {

// A visitor that rewrites ResolvedFlatten nodes into standard UNNESTs.
class FlattenRewriterVisitor : public ResolvedASTDeepCopyVisitor {
 public:
  explicit FlattenRewriterVisitor(ColumnFactory* column_factory)
      : column_factory_(column_factory) {}

 private:
  absl::Status VisitResolvedArrayScan(const ResolvedArrayScan* node) override;

  absl::Status VisitResolvedFlatten(const ResolvedFlatten* node) override;

  // Takes the components of a ResolvedFlatten (its expr, 'flatten_expr' and its
  // 'get_field_list' and converts it into a resulting ResolvedScan that is
  // functionally equivalent.
  //
  // When 'flatten_expr' uses ColumnRefs from a scan, 'input_scan' must be
  // provided to be that input scan.
  //
  // When 'order_results' is true, the generated scan ends with an OrderByScan
  // to retain order (using offsets from array scans).
  //
  // Note that the final OrderByScan is not needed for a case like
  // SELECT ... FROM t, UNNEST(t.a.b.c) since the UNNEST produces an unordered
  // relation. The final OrderByScan is needed for explicit FLATTEN(t.a.b.c) or
  // UNNEST with OFFSET.
  zetasql_base::StatusOr<std::unique_ptr<ResolvedScan>> FlattenToScan(
      std::unique_ptr<ResolvedExpr> flatten_expr,
      const std::vector<std::unique_ptr<const ResolvedExpr>>& get_field_list,
      std::unique_ptr<ResolvedScan> input_scan, bool order_results);

  ColumnFactory* column_factory_;
};

absl::Status FlattenRewriterVisitor::VisitResolvedArrayScan(
    const ResolvedArrayScan* node) {
  if (!node->array_expr()->Is<ResolvedFlatten>()) {
    return CopyVisitResolvedArrayScan(node);
  }
  const ResolvedFlatten* flatten = node->array_expr()->GetAs<ResolvedFlatten>();
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedScan> input_scan,
                   ProcessNode(node->input_scan()));

  bool need_offset_column = node->array_offset_column() != nullptr;
  if (need_offset_column) {
    // If we need an offset column, we rewrite each row to a subquery to
    // generate a single array and then do an array scan over that. This allows
    // us to have a single ordered offset.
    //
    // Without doing so we end up with one offset per repeated pivot and no way
    // to combine them.
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> flatten_expr,
                     CorrelateColumnRefs(*flatten->expr()));
    ZETASQL_ASSIGN_OR_RETURN(
        std::unique_ptr<ResolvedScan> scan,
        FlattenToScan(std::move(flatten_expr), flatten->get_field_list(),
                      MakeResolvedSingleRowScan(), need_offset_column));

    std::vector<std::unique_ptr<const ResolvedColumnRef>> column_refs;
    ZETASQL_RETURN_IF_ERROR(CollectColumnRefs(*flatten->expr(), &column_refs));
    std::unique_ptr<ResolvedSubqueryExpr> subquery = MakeResolvedSubqueryExpr(
        flatten->type(), ResolvedSubqueryExpr::ARRAY, std::move(column_refs),
        /*in_expr=*/nullptr, std::move(scan));
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedColumnHolder> offset_column,
                     ProcessNode(node->array_offset_column()));
    PushNodeToStack(MakeResolvedArrayScan(
        node->column_list(), std::move(input_scan), std::move(subquery),
        node->element_column(), std::move(offset_column),
        /*join_expr=*/nullptr, /*is_outer=*/false));
    return absl::OkStatus();
  }

  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> flatten_expr,
                   ProcessNode(flatten->expr()));
  ZETASQL_ASSIGN_OR_RETURN(
      std::unique_ptr<ResolvedScan> scan,
      FlattenToScan(std::move(flatten_expr), flatten->get_field_list(),
                    std::move(input_scan), /*order_results=*/false));

  // Project the flatten result back to the expected output column.
  std::vector<std::unique_ptr<const ResolvedComputedColumn>> expr_list;
  expr_list.push_back(MakeResolvedComputedColumn(
      node->element_column(),
      MakeResolvedColumnRef(scan->column_list().back().type(),
                            scan->column_list().back(),
                            /*is_correlated=*/false)));
  PushNodeToStack(MakeResolvedProjectScan(
      node->column_list(), std::move(expr_list), std::move(scan)));
  return absl::OkStatus();
}

absl::Status FlattenRewriterVisitor::VisitResolvedFlatten(
    const ResolvedFlatten* node) {
  ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> flatten_expr,
                   CorrelateColumnRefs(*node->expr()));
  ZETASQL_ASSIGN_OR_RETURN(
      std::unique_ptr<ResolvedScan> scan,
      FlattenToScan(std::move(flatten_expr), node->get_field_list(),
                    /*input_scan=*/nullptr, /*order_results=*/true));

  std::vector<std::unique_ptr<const ResolvedColumnRef>> column_refs;
  ZETASQL_RETURN_IF_ERROR(CollectColumnRefs(*node->expr(), &column_refs));
  PushNodeToStack(MakeResolvedSubqueryExpr(
      node->type(), ResolvedSubqueryExpr::ARRAY, std::move(column_refs),
      /*in_expr=*/nullptr, std::move(scan)));
  return absl::OkStatus();
}

zetasql_base::StatusOr<std::unique_ptr<ResolvedScan>>
FlattenRewriterVisitor::FlattenToScan(
    std::unique_ptr<ResolvedExpr> flatten_expr,
    const std::vector<std::unique_ptr<const ResolvedExpr>>& get_field_list,
    std::unique_ptr<ResolvedScan> input_scan, bool order_results) {
  std::vector<ResolvedColumn> column_list;
  if (input_scan != nullptr) column_list = input_scan->column_list();
  ResolvedColumn column = column_factory_->MakeCol(
      "$flatten", "injected", flatten_expr->type()->AsArray()->element_type());
  column_list.push_back(column);

  std::vector<ResolvedColumn> offset_columns;
  ResolvedColumn offset_column;
  if (order_results) {
    offset_column =
        column_factory_->MakeCol("$offset", "injected", types::Int64Type());
    offset_columns.push_back(offset_column);
    column_list.push_back(offset_column);
  }

  std::unique_ptr<ResolvedScan> scan = MakeResolvedArrayScan(
      column_list, std::move(input_scan), std::move(flatten_expr), column,
      order_results ? MakeResolvedColumnHolder(offset_column) : nullptr,
      /*join_expr=*/nullptr, /*is_outer=*/false);

  // Keep track of pending Get*Field on non-array fields.
  std::unique_ptr<const ResolvedExpr> input;

  for (const auto& const_get_field : get_field_list) {
    ZETASQL_ASSIGN_OR_RETURN(std::unique_ptr<ResolvedExpr> get_field,
                     ProcessNode(const_get_field.get()));
    // Change the input from the FlattenedArg to instead be a ColumnRef or the
    // built-up non-array expression.
    if (input == nullptr) {
      input = MakeResolvedColumnRef(column.type(), column,
                                    /*is_correlated=*/false);
    }
    if (get_field->Is<ResolvedGetProtoField>()) {
      get_field->GetAs<ResolvedGetProtoField>()->set_expr(std::move(input));
    } else {
      ZETASQL_RET_CHECK(get_field->Is<ResolvedGetStructField>());
      get_field->GetAs<ResolvedGetStructField>()->set_expr(std::move(input));
    }
    input = nullptr;  // already null, but avoids ClangTidy "use after free"

    if (!get_field->type()->IsArray()) {
      // Not an array so can't turn it into an ArrayScan.
      // Collect as input for next array.
      input = std::move(get_field);
    } else {
      column = column_factory_->MakeCol(
          "$flatten", "injected", get_field->type()->AsArray()->element_type());
      column_list.push_back(column);

      if (order_results) {
        offset_column =
            column_factory_->MakeCol("$offset", "injected", types::Int64Type());
        offset_columns.push_back(offset_column);
        column_list.push_back(offset_column);
      }
      scan = MakeResolvedArrayScan(
          column_list, std::move(scan), std::move(get_field), column,
          order_results ? MakeResolvedColumnHolder(offset_column) : nullptr,
          /*join_expr=*/nullptr,
          /*is_outer=*/false);
    }
  }

  if (input != nullptr) {
    // We have leftover "gets" that resulted in non-arrays.
    // Use a ProjectScan to resolve them to the expected column.
    column = column_factory_->MakeCol("$flatten", "injected", input->type());
    // node->type()->AsArray()->element_type());
    column_list.push_back(column);
    std::vector<std::unique_ptr<const ResolvedComputedColumn>> expr_list;
    expr_list.push_back(MakeResolvedComputedColumn(column, std::move(input)));
    scan = MakeResolvedProjectScan(column_list, std::move(expr_list),
                                   std::move(scan));
  }

  if (order_results) {
    std::vector<std::unique_ptr<const ResolvedOrderByItem>> order_by;
    order_by.reserve(offset_columns.size());
    for (const ResolvedColumn& c : offset_columns) {
      order_by.push_back(MakeResolvedOrderByItem(
          MakeResolvedColumnRef(c.type(), c, /*is_correlated=*/false),
          /*collation_name=*/nullptr, /*is_descending=*/false,
          ResolvedOrderByItemEnums::ORDER_UNSPECIFIED));
    }
    scan =
        MakeResolvedOrderByScan({column}, std::move(scan), std::move(order_by));
    scan->set_is_ordered(true);
  }
  return scan;
}

}  // namespace

zetasql_base::StatusOr<std::unique_ptr<const ResolvedStatement>> RewriteResolvedFlatten(
    const ResolvedStatement& resolved_statement,
    ColumnFactory& column_factory) {
  FlattenRewriterVisitor rewriter(&column_factory);
  ZETASQL_RETURN_IF_ERROR(resolved_statement.Accept(&rewriter));
  return rewriter.ConsumeRootNode<ResolvedStatement>();
}

}  // namespace zetasql
