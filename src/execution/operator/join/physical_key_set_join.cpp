#include "duckdb/execution/operator/join/physical_key_set_join.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

PhysicalKeySetJoin::PhysicalKeySetJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op, PhysicalOperator &left,
                                       PhysicalOperator &right)
    : PhysicalComparisonJoin(physical_plan, op, PhysicalOperatorType::KEY_SET_JOIN, std::move(op.conditions),
                             op.join_type, op.estimated_cardinality),
      projection_map(FillProjectionMap(left, op.left_projection_map)) {
	children.push_back(left);
	children.push_back(right);
	filter_pushdown = std::move(op.filter_pushdown);
	D_ASSERT(conditions.size() == 1 && conditions[0].GetComparisonType() == ExpressionType::COMPARE_EQUAL);
	D_ASSERT(join_type == JoinType::SEMI || join_type == JoinType::ANTI || join_type == JoinType::MARK);
}

class KeySetJoinGlobalState : public GlobalSinkState {
public:
	KeySetJoinGlobalState(ClientContext &context, const PhysicalKeySetJoin &op) {
		if (op.filter_pushdown) {
			filter_state = op.filter_pushdown->GetGlobalState(context, op);
		}
	}

	mutex combine_lock;
	unique_ptr<GroupedAggregateHashTable> keys;
	unique_ptr<JoinFilterGlobalState> filter_state;
	bool has_null = false;
};

class KeySetJoinLocalState : public LocalSinkState {
public:
	KeySetJoinLocalState(ClientContext &context, const PhysicalKeySetJoin &op, KeySetJoinGlobalState &gstate)
	    : executor(context, op.conditions[0].GetRHS()), addresses(LogicalType::POINTER) {
		vector<LogicalType> types {op.conditions[0].GetRHS().GetReturnType()};
		input_keys.Initialize(BufferAllocator::Get(context), types);
		keys = make_uniq<GroupedAggregateHashTable>(context, BufferAllocator::Get(context), types);
		if (op.filter_pushdown) {
			filter_state = op.filter_pushdown->GetLocalState(*gstate.filter_state);
		}
	}

	ExpressionExecutor executor;
	DataChunk input_keys;
	Vector addresses;
	unique_ptr<GroupedAggregateHashTable> keys;
	unique_ptr<JoinFilterLocalState> filter_state;
	bool has_null = false;
};

unique_ptr<GlobalSinkState> PhysicalKeySetJoin::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<KeySetJoinGlobalState>(context, *this);
}

unique_ptr<LocalSinkState> PhysicalKeySetJoin::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<KeySetJoinLocalState>(context.client, *this, sink_state->Cast<KeySetJoinGlobalState>());
}

SinkResultType PhysicalKeySetJoin::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	auto &lstate = input.local_state.Cast<KeySetJoinLocalState>();
	lstate.input_keys.Reset();
	lstate.executor.Execute(chunk, lstate.input_keys);
	if (filter_pushdown) {
		filter_pushdown->Sink(lstate.input_keys, *lstate.filter_state);
	}
	if (join_type == JoinType::MARK && !lstate.has_null) {
		auto validity = lstate.input_keys.data[0].Validity();
		for (idx_t i = 0; i < lstate.input_keys.size(); i++) {
			if (!validity.IsValid(i)) {
				lstate.has_null = true;
				break;
			}
		}
	}
	lstate.keys->FindOrCreateGroups(lstate.input_keys, lstate.addresses);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType PhysicalKeySetJoin::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	auto &gstate = input.global_state.Cast<KeySetJoinGlobalState>();
	auto &lstate = input.local_state.Cast<KeySetJoinLocalState>();
	{
		lock_guard<mutex> guard(gstate.combine_lock);
		gstate.has_null = gstate.has_null || lstate.has_null;
		if (!gstate.keys) {
			gstate.keys = std::move(lstate.keys);
		} else {
			gstate.keys->Combine(*lstate.keys);
			lstate.keys.reset();
		}
		if (filter_pushdown) {
			filter_pushdown->Combine(*gstate.filter_state, *lstate.filter_state);
		}
	}
	context.thread.profiler.Flush(*this);
	QueryProfiler::Get(context.client).Flush(context.thread.profiler);
	return SinkCombineResultType::FINISHED;
}

SinkFinalizeType PhysicalKeySetJoin::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                              OperatorSinkFinalizeInput &input) const {
	auto &gstate = input.global_state.Cast<KeySetJoinGlobalState>();
	if (filter_pushdown) {
		(void)filter_pushdown->Finalize(context, *gstate.filter_state, *this);
	}
	if ((!gstate.keys || gstate.keys->Count() == 0) && EmptyResultIfRHSIsEmpty()) {
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}
	return SinkFinalizeType::READY;
}

class KeySetJoinOperatorState : public CachingOperatorState {
public:
	KeySetJoinOperatorState(ClientContext &context, const PhysicalKeySetJoin &op)
	    : executor(context, op.conditions[0].GetLHS()), found(STANDARD_VECTOR_SIZE), selected(STANDARD_VECTOR_SIZE) {
		input_keys.Initialize(BufferAllocator::Get(context), {op.conditions[0].GetLHS().GetReturnType()});
		vector<LogicalType> projected_types;
		for (auto column : op.projection_map) {
			projected_types.push_back(op.children[0].get().GetTypes()[column]);
		}
		empty_probe.InitializeEmpty(projected_types);
	}

	ExpressionExecutor executor;
	DataChunk input_keys;
	DataChunk empty_probe;
	AggregateHTLookupState lookup;
	SelectionVector found;
	SelectionVector selected;
	bool matches[STANDARD_VECTOR_SIZE];
};

unique_ptr<OperatorState> PhysicalKeySetJoin::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<KeySetJoinOperatorState>(context.client, *this);
}

OperatorResultType PhysicalKeySetJoin::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                       GlobalOperatorState &gstate_p, OperatorState &state_p) const {
	auto &state = state_p.Cast<KeySetJoinOperatorState>();
	const auto &gstate = sink_state->Cast<KeySetJoinGlobalState>();
	if (!gstate.keys || gstate.keys->Count() == 0) {
		if (EmptyResultIfRHSIsEmpty()) {
			return OperatorResultType::FINISHED;
		}
		state.empty_probe.ReferenceColumns(input, projection_map);
		ConstructEmptyJoinResult(join_type, false, state.empty_probe, chunk);
		return OperatorResultType::NEED_MORE_INPUT;
	}
	state.input_keys.Reset();
	state.executor.Execute(input, state.input_keys);
	auto validity = state.input_keys.data[0].Validity();
	std::fill_n(state.matches, input.size(), false);
	const auto found_count = gstate.keys->LookupGroups(state.input_keys, state.lookup, state.found);
	for (idx_t i = 0; i < found_count; i++) {
		const auto row = state.found.get_index(i);
		state.matches[row] = validity.IsValid(row);
	}

	if (join_type == JoinType::MARK) {
		for (idx_t i = 0; i < projection_map.size(); i++) {
			chunk.data[i].Reference(input.data[projection_map[i]]);
		}
		{
			auto writer = FlatVector::Writer<bool>(chunk.data.back(), input.size());
			for (idx_t i = 0; i < input.size(); i++) {
				if (!validity.IsValid(i) || (!state.matches[i] && gstate.has_null)) {
					writer.WriteNull();
				} else {
					writer.WriteValue(state.matches[i]);
				}
			}
		}
		chunk.CheckCardinality(input.size());
	} else {
		idx_t count = 0;
		for (idx_t i = 0; i < input.size(); i++) {
			if (state.matches[i] == (join_type == JoinType::SEMI)) {
				state.selected.set_index(count++, i);
			}
		}
		chunk.ReferenceColumns(input, projection_map);
		chunk.Slice(state.selected, count);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

} // namespace duckdb
