#include "duckdb/planner/planner.hpp"

#include "duckdb/common/serializer.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/statement/pragma_statement.hpp"
#include "duckdb/parser/statement/prepare_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/operator/logical_prepare.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/parser/parsed_data/drop_info.hpp"

namespace duckdb {
using namespace std;

Planner::Planner(ClientContext &context) : binder(context), context(context) {
}

void Planner::CreatePlan(SQLStatement &statement) {
	vector<BoundParameterExpression *> bound_parameters;

	// first bind the tables and columns to the catalog
	context.profiler.StartPhase("binder");
	binder.parameters = &bound_parameters;
	auto bound_statement = binder.Bind(statement);
	context.profiler.EndPhase();

	// VerifyQuery(*bound_statement);

	this->read_only = binder.read_only;
	this->requires_valid_transaction = binder.requires_valid_transaction;
	this->names = bound_statement.names;
	this->types = bound_statement.types;
	this->plan = move(bound_statement.plan);

	// now create a logical query plan from the query
	// context.profiler.StartPhase("logical_planner");
	// LogicalPlanGenerator logical_planner(binder, context);
	// this->plan = logical_planner.CreatePlan(*bound_statement);
	// context.profiler.EndPhase();

	// set up a map of parameter number -> value entries
	for (auto &expr : bound_parameters) {
		// check if the type of the parameter could be resolved
		if (expr->return_type.id() == LogicalTypeId::INVALID) {
			throw BinderException("Could not determine type of parameters: try adding explicit type casts");
		}
		auto value = make_unique<Value>(expr->return_type);
		expr->value = value.get();
		// check if the parameter number has been used before
		if (value_map.find(expr->parameter_nr) == value_map.end()) {
			// not used before, create vector
			value_map[expr->parameter_nr] = vector<unique_ptr<Value>>();
		} else if (value_map[expr->parameter_nr].back()->type() != value->type()) {
			// used before, but types are inconsistent
			throw BinderException("Inconsistent types found for parameter with index %llu", expr->parameter_nr);
		}
		value_map[expr->parameter_nr].push_back(move(value));
	}
}

void Planner::CreatePlan(unique_ptr<SQLStatement> statement) {
	D_ASSERT(statement);
	switch (statement->type) {
	case StatementType::SELECT_STATEMENT:
	case StatementType::INSERT_STATEMENT:
	case StatementType::COPY_STATEMENT:
	case StatementType::DELETE_STATEMENT:
	case StatementType::UPDATE_STATEMENT:
	case StatementType::CREATE_STATEMENT:
	case StatementType::EXECUTE_STATEMENT:
	case StatementType::DROP_STATEMENT:
	case StatementType::ALTER_STATEMENT:
	case StatementType::TRANSACTION_STATEMENT:
	case StatementType::EXPLAIN_STATEMENT:
	case StatementType::VACUUM_STATEMENT:
	case StatementType::RELATION_STATEMENT:
	case StatementType::CALL_STATEMENT:
	case StatementType::EXPORT_STATEMENT:
	case StatementType::PRAGMA_STATEMENT:
		CreatePlan(*statement);
		break;
	case StatementType::PREPARE_STATEMENT: {
		auto &stmt = *reinterpret_cast<PrepareStatement *>(statement.get());
		auto statement_type = stmt.statement->type;
		// create a plan of the underlying statement
		CreatePlan(move(stmt.statement));
		// now create the logical prepare
		auto prepared_data = make_unique<PreparedStatementData>(statement_type);
		prepared_data->names = names;
		prepared_data->types = types;
		prepared_data->value_map = move(value_map);
		prepared_data->read_only = this->read_only;
		prepared_data->requires_valid_transaction = this->requires_valid_transaction;

		this->read_only = true;
		this->requires_valid_transaction = false;

		auto prepare = make_unique<LogicalPrepare>(stmt.name, move(prepared_data), move(plan));
		names = {"Success"};
		types = {LogicalType::BOOLEAN};
		plan = move(prepare);
		break;
	}
	default:
		throw NotImplementedException("Cannot plan statement of type %s!", StatementTypeToString(statement->type));
	}
}

// void Planner::VerifyQuery(BoundSQLStatement &statement) {
// 	if (!context.query_verification_enabled) {
// 		return;
// 	}
// 	if (statement.type != StatementType::SELECT_STATEMENT) {
// 		return;
// 	}
// 	auto &select = (BoundSelectStatement &)statement;
// 	VerifyNode(*select.node);
// }

// void Planner::VerifyNode(BoundQueryNode &node) {
// 	if (node.type == QueryNodeType::SELECT_NODE) {
// 		auto &select_node = (BoundSelectNode &)node;
// 		vector<unique_ptr<Expression>> copies;
// 		for (auto &expr : select_node.select_list) {
// 			VerifyExpression(*expr, copies);
// 		}
// 		if (select_node.where_clause) {
// 			VerifyExpression(*select_node.where_clause, copies);
// 		}
// 		for (auto &expr : select_node.groups) {
// 			VerifyExpression(*expr, copies);
// 		}
// 		if (select_node.having) {
// 			VerifyExpression(*select_node.having, copies);
// 		}
// 		for (auto &aggr : select_node.aggregates) {
// 			VerifyExpression(*aggr, copies);
// 		}
// 		for (auto &window : select_node.windows) {
// 			VerifyExpression(*window, copies);
// 		}

// 		// double loop to verify that (in)equality of hashes
// 		for (idx_t i = 0; i < copies.size(); i++) {
// 			auto outer_hash = copies[i]->Hash();
// 			for (idx_t j = 0; j < copies.size(); j++) {
// 				auto inner_hash = copies[j]->Hash();
// 				if (outer_hash != inner_hash) {
// 					// if hashes are not equivalent the expressions should not be equivalent
// 					D_ASSERT(!Expression::Equals(copies[i].get(), copies[j].get()));
// 				}
// 			}
// 		}
// 	} else {
// 		D_ASSERT(node.type == QueryNodeType::SET_OPERATION_NODE);
// 		auto &setop_node = (BoundSetOperationNode &)node;
// 		VerifyNode(*setop_node.left);
// 		VerifyNode(*setop_node.right);
// 	}
// }

// void Planner::VerifyExpression(Expression &expr, vector<unique_ptr<Expression>> &copies) {
// 	if (expr.HasSubquery()) {
// 		// can't copy subqueries
// 		return;
// 	}
// 	// verify that the copy of expressions works
// 	auto copy = expr.Copy();
// 	// copy should have identical hash and identical equality function
// 	D_ASSERT(copy->Hash() == expr.Hash());
// 	D_ASSERT(Expression::Equals(copy.get(), &expr));
// 	copies.push_back(move(copy));
// }

} // namespace duckdb
