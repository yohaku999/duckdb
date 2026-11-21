//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_build_deduplication.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {

class ClientContext;
class LogicalOperator;
struct LogicalType;

enum class JoinBuildDeduplicationMode : uint8_t { OFF, LOGICAL, PHYSICAL };

//! Experimental alternatives selected with SET VARIABLE join_build_deduplication.
class JoinBuildDeduplication {
public:
	static JoinBuildDeduplicationMode GetMode(ClientContext &context);
	static bool SupportsKey(const LogicalType &type);
	static void Optimize(unique_ptr<LogicalOperator> &op, ClientContext &context);
};

} // namespace duckdb
