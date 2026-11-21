#include "duckdb/optimizer/join_build_deduplication.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"

namespace duckdb {

JoinBuildDeduplicationMode JoinBuildDeduplication::GetMode(ClientContext &context) {
	Value value;
	if (!ClientConfig::GetConfig(context).GetUserVariable(string("join_build_deduplication"), value)) {
		return JoinBuildDeduplicationMode::OFF;
	}
	if (value.type() == LogicalType::VARCHAR && !value.IsNull()) {
		auto mode = StringUtil::Lower(value.GetValue<string>());
		if (mode == "off") {
			return JoinBuildDeduplicationMode::OFF;
		}
		if (mode == "logical") {
			return JoinBuildDeduplicationMode::LOGICAL;
		}
		if (mode == "physical") {
			return JoinBuildDeduplicationMode::PHYSICAL;
		}
	}
	throw InvalidInputException("join_build_deduplication must be 'off', 'logical' or 'physical'");
}

bool JoinBuildDeduplication::SupportsKey(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

static void InsertBuildDistinct(unique_ptr<LogicalOperator> &op) {
	for (auto &child : op->children) {
		InsertBuildDistinct(child);
	}
	if (op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return;
	}
	auto &join = op->Cast<LogicalComparisonJoin>();
	if ((join.join_type != JoinType::SEMI && join.join_type != JoinType::ANTI && join.join_type != JoinType::MARK) ||
	    join.conditions.size() != 1 || join.HasArbitraryConditions() || !join.mark_types.empty() ||
	    !join.duplicate_eliminated_columns.empty()) {
		return;
	}
	auto &condition = join.conditions[0];
	if (!condition.IsComparison() ||
	    (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
	     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) ||
	    condition.GetLHS().IsVolatile() ||
	    condition.GetRHS().GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    !JoinBuildDeduplication::SupportsKey(condition.GetRHS().GetReturnType())) {
		return;
	}
	auto &input = join.children[1];
	auto bindings = input->GetColumnBindings();
	if (bindings.size() != 1 || bindings[0] != condition.GetRHS().Cast<BoundColumnRefExpression>().Binding() ||
	    input->type == LogicalOperatorType::LOGICAL_DISTINCT) {
		return;
	}
	vector<unique_ptr<Expression>> targets;
	targets.push_back(condition.GetRHS().Copy());
	auto distinct = make_uniq<LogicalDistinct>(std::move(targets), DistinctType::DISTINCT);
	if (input->has_estimated_cardinality) {
		distinct->SetEstimatedCardinality(input->estimated_cardinality);
	}
	distinct->AddChild(std::move(input));
	input = std::move(distinct);
}

void JoinBuildDeduplication::Optimize(unique_ptr<LogicalOperator> &op, ClientContext &context) {
	if (GetMode(context) == JoinBuildDeduplicationMode::LOGICAL) {
		InsertBuildDistinct(op);
	}
}

} // namespace duckdb
