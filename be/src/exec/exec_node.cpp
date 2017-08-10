// Modifications copyright (C) 2017, Baidu.com, Inc.
// Copyright 2017 The Apache Software Foundation

// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "exec/exec_node.h"

#include <sstream>
#include <thrift/protocol/TDebugProtocol.h>
#include <unistd.h>

#include "codegen/llvm_codegen.h"
#include "codegen/codegen_anyval.h"
#include "common/object_pool.h"
#include "common/status.h"
#include "exprs/expr_context.h"
#include "exec/aggregation_node.h"
#include "exec/partitioned_aggregation_node.h"
#include "exec/csv_scan_node.h"
#include "exec/pre_aggregation_node.h"
#include "exec/hash_join_node.h"
#include "exec/broker_scan_node.h"
#include "exec/cross_join_node.h"
#include "exec/empty_set_node.h"
#include "exec/mysql_scan_node.h"
#include "exec/schema_scan_node.h"
#include "exec/exchange_node.h"
#include "exec/merge_join_node.h"
#include "exec/merge_node.h"
#include "exec/olap_rewrite_node.h"
#include "exec/olap_scan_node.h"
#include "exec/topn_node.h"
#include "exec/sort_node.h"
#include "exec/spill_sort_node.h"
#include "exec/analytic_eval_node.h"
#include "exec/select_node.h"
#include "exec/union_node.h"
#include "runtime/descriptors.h"
#include "runtime/mem_pool.h"
#include "runtime/mem_tracker.h"
#include "runtime/row_batch.h"
#include "runtime/runtime_state.h"
#include "util/debug_util.h"
#include "util/runtime_profile.h"

using llvm::Function;
using llvm::PointerType;
using llvm::Type;
using llvm::Value;
using llvm::LLVMContext;
using llvm::BasicBlock;

namespace palo {

const std::string ExecNode::ROW_THROUGHPUT_COUNTER = "RowsReturnedRate";

int ExecNode::get_node_id_from_profile(RuntimeProfile* p) {
    return p->metadata();
}

ExecNode::RowBatchQueue::RowBatchQueue(int max_batches) :
    BlockingQueue<RowBatch*>(max_batches) {
}

ExecNode::RowBatchQueue::~RowBatchQueue() {
    DCHECK(cleanup_queue_.empty());
}

void ExecNode::RowBatchQueue::AddBatch(RowBatch* batch) {
  if (!blocking_put(batch)) {
    std::lock_guard<std::mutex> lock(lock_);
    cleanup_queue_.push_back(batch);
  }
}

bool ExecNode::RowBatchQueue::AddBatchWithTimeout(RowBatch* batch,
    int64_t timeout_micros) {
    // return blocking_put_with_timeout(batch, timeout_micros);
    return blocking_put(batch);
}

RowBatch* ExecNode::RowBatchQueue::GetBatch() {
  RowBatch* result = NULL;
  if (blocking_get(&result)) return result;
  return NULL;
}

int ExecNode::RowBatchQueue::Cleanup() {
  int num_io_buffers = 0;

  // RowBatch* batch = NULL;
  // while ((batch = GetBatch()) != NULL) {
  //   num_io_buffers += batch->num_io_buffers();
  //   delete batch;
  // }

  lock_guard<std::mutex> l(lock_);
  for (std::list<RowBatch*>::iterator it = cleanup_queue_.begin();
      it != cleanup_queue_.end(); ++it) {
    // num_io_buffers += (*it)->num_io_buffers();
    delete *it;
  }
  cleanup_queue_.clear();
  return num_io_buffers;
}

ExecNode::ExecNode(ObjectPool* pool, const TPlanNode& tnode, const DescriptorTbl& descs) :
        _id(tnode.node_id),
        _type(tnode.node_type),
        _pool(pool),
        _tuple_ids(tnode.row_tuples),
        _row_descriptor(descs, tnode.row_tuples, tnode.nullable_tuples),
        _debug_phase(TExecNodePhase::INVALID),
        _debug_action(TDebugAction::WAIT),
        _limit(tnode.limit),
        _num_rows_returned(0),
        _rows_returned_counter(NULL),
        _rows_returned_rate(NULL),
        _memory_used_counter(NULL),
        _is_closed(false){
    init_runtime_profile(print_plan_node_type(tnode.node_type));
}

ExecNode::~ExecNode() {
}

void ExecNode::push_down_predicate(
        RuntimeState* state, std::list<ExprContext*>* expr_ctxs) {
    for (int i = 0; i < _children.size(); ++i) {
        _children[i]->push_down_predicate(state, expr_ctxs);
        if (expr_ctxs->size() == 0) {
            return;
        }
    }

    std::list<ExprContext*>::iterator iter = expr_ctxs->begin();
    while (iter != expr_ctxs->end()) {
        if ((*iter)->root()->is_bound(&_tuple_ids)) {
            // LOG(INFO) << "push down success expr is " << (*iter)->debug_string()
            //          << " and node is " << debug_string();
            (*iter)->prepare(state, row_desc(), _expr_mem_tracker.get());
            (*iter)->open(state);
            _conjunct_ctxs.push_back(*iter);
            iter = expr_ctxs->erase(iter);
        } else {
            ++iter;
        }
    }
}

Status ExecNode::init(const TPlanNode& tnode) {
    RETURN_IF_ERROR(
        Expr::create_expr_trees(_pool, tnode.conjuncts, &_conjunct_ctxs));
    return Status::OK;
}

Status ExecNode::prepare(RuntimeState* state) {
    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::PREPARE));
    DCHECK(_runtime_profile.get() != NULL);
    _rows_returned_counter =
        ADD_COUNTER(_runtime_profile, "RowsReturned", TUnit::UNIT);
    _memory_used_counter =
        ADD_COUNTER(_runtime_profile, "MemoryUsed", TUnit::BYTES);
    _rows_returned_rate = runtime_profile()->add_derived_counter(
                              ROW_THROUGHPUT_COUNTER, TUnit::UNIT_PER_SECOND,
                              boost::bind<int64_t>(&RuntimeProfile::units_per_second,
                                                   _rows_returned_counter,
                                                   runtime_profile()->total_time_counter()),
                              "");
    _mem_tracker.reset(new MemTracker(-1, _runtime_profile->name(), state->instance_mem_tracker()));
    _expr_mem_tracker.reset(new MemTracker(-1, "Exprs", _mem_tracker.get()));

    RETURN_IF_ERROR(Expr::prepare(_conjunct_ctxs, state, row_desc(), expr_mem_tracker()));
    // TODO(zc):
    // AddExprCtxsToFree(_conjunct_ctxs);

    for (int i = 0; i < _children.size(); ++i) {
        RETURN_IF_ERROR(_children[i]->prepare(state));
    }

    return Status::OK;
}

Status ExecNode::open(RuntimeState* state) {
    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::OPEN));
    return Expr::open(_conjunct_ctxs, state);
}

Status ExecNode::close(RuntimeState* state) {
    if (_is_closed) {
        return Status::OK;
    }
    _is_closed = true;
    RETURN_IF_ERROR(exec_debug_action(TExecNodePhase::CLOSE));

    if (_rows_returned_counter != NULL) {
        COUNTER_SET(_rows_returned_counter, _num_rows_returned);
    }

    Status result;

    for (int i = 0; i < _children.size(); ++i) {
        result.add_error(_children[i]->close(state));
    }
    Expr::close(_conjunct_ctxs, state);

    return result;
}

void ExecNode::add_runtime_exec_option(const std::string& str) {
    lock_guard<mutex> l(_exec_options_lock);

    if (_runtime_exec_options.empty()) {
        _runtime_exec_options = str;
    } else {
        _runtime_exec_options.append(", ");
        _runtime_exec_options.append(str);
    }

    runtime_profile()->add_info_string("ExecOption", _runtime_exec_options);
}

Status ExecNode::create_tree(ObjectPool* pool, const TPlan& plan,
                            const DescriptorTbl& descs, ExecNode** root) {
    if (plan.nodes.size() == 0) {
        *root = NULL;
        return Status::OK;
    }

    int node_idx = 0;
    RETURN_IF_ERROR(create_tree_helper(pool, plan.nodes, descs, NULL, &node_idx, root));

    if (node_idx + 1 != plan.nodes.size()) {
        // TODO: print thrift msg for diagnostic purposes.
        return Status(
                   "Plan tree only partially reconstructed. Not all thrift nodes were used.");
    }

    return Status::OK;
}

Status ExecNode::create_tree_helper(
    ObjectPool* pool,
    const vector<TPlanNode>& tnodes,
    const DescriptorTbl& descs,
    ExecNode* parent,
    int* node_idx,
    ExecNode** root) {
    // propagate error case
    if (*node_idx >= tnodes.size()) {
        // TODO: print thrift msg
        return Status("Failed to reconstruct plan tree from thrift.");
    }
    const TPlanNode& tnode = tnodes[*node_idx];

    int num_children = tnodes[*node_idx].num_children;
    ExecNode* node = NULL;
    RETURN_IF_ERROR(create_node(pool, tnodes[*node_idx], descs, &node));

    // assert(parent != NULL || (node_idx == 0 && root_expr != NULL));
    if (parent != NULL) {
        parent->_children.push_back(node);
    } else {
        *root = node;
    }

    for (int i = 0; i < num_children; i++) {
        ++*node_idx;
        RETURN_IF_ERROR(create_tree_helper(pool, tnodes, descs, node, node_idx, NULL));

        // we are expecting a child, but have used all nodes
        // this means we have been given a bad tree and must fail
        if (*node_idx >= tnodes.size()) {
            // TODO: print thrift msg
            return Status("Failed to reconstruct plan tree from thrift.");
        }
    }

    RETURN_IF_ERROR(node->init(tnode));

    // build up tree of profiles; add children >0 first, so that when we print
    // the profile, child 0 is printed last (makes the output more readable)
    for (int i = 1; i < node->_children.size(); ++i) {
        node->runtime_profile()->add_child(node->_children[i]->runtime_profile(), true, NULL);
    }

    if (!node->_children.empty()) {
        node->runtime_profile()->add_child(node->_children[0]->runtime_profile(), false, NULL);
    }

    return Status::OK;
}

Status ExecNode::create_node(ObjectPool* pool, const TPlanNode& tnode,
                            const DescriptorTbl& descs, ExecNode** node) {
    std::stringstream error_msg;

    switch (tnode.node_type) {
        VLOG(2) << "tnode:\n" << apache::thrift::ThriftDebugString(tnode);

    case TPlanNodeType::CSV_SCAN_NODE:
        *node = pool->add(new CsvScanNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::MYSQL_SCAN_NODE:
        *node = pool->add(new MysqlScanNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::SCHEMA_SCAN_NODE:
        *node = pool->add(new SchemaScanNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::OLAP_SCAN_NODE:
        *node = pool->add(new OlapScanNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::AGGREGATION_NODE:
        if (config::enable_partitioned_aggregation) {
            *node = pool->add(new PartitionedAggregationNode(pool, tnode, descs));
        } else {
            *node = pool->add(new AggregationNode(pool, tnode, descs));
        }
        return Status::OK;

        /*case TPlanNodeType::PRE_AGGREGATION_NODE:
          *node = pool->add(new PreAggregationNode(pool, tnode, descs));
          return Status::OK;*/
    case TPlanNodeType::HASH_JOIN_NODE:
        *node = pool->add(new HashJoinNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::CROSS_JOIN_NODE:
        *node = pool->add(new CrossJoinNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::MERGE_JOIN_NODE:
        *node = pool->add(new MergeJoinNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::EMPTY_SET_NODE:
        *node = pool->add(new EmptySetNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::EXCHANGE_NODE:
        *node = pool->add(new ExchangeNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::SELECT_NODE:
        *node = pool->add(new SelectNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::OLAP_REWRITE_NODE:
        *node = pool->add(new OlapRewriteNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::SORT_NODE:
        if (tnode.sort_node.use_top_n) {
            *node = pool->add(new TopNNode(pool, tnode, descs));
        } else {
            *node = pool->add(new SpillSortNode(pool, tnode, descs));
        }

        return Status::OK;
    case TPlanNodeType::ANALYTIC_EVAL_NODE:
        *node = pool->add(new AnalyticEvalNode(pool, tnode, descs));
        break;

    case TPlanNodeType::MERGE_NODE:
        *node = pool->add(new MergeNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::UNION_NODE:
        *node = pool->add(new UnionNode(pool, tnode, descs));
        return Status::OK;

    case TPlanNodeType::BROKER_SCAN_NODE:
        *node = pool->add(new BrokerScanNode(pool, tnode, descs));
        return Status::OK;

    default:
        map<int, const char*>::const_iterator i =
            _TPlanNodeType_VALUES_TO_NAMES.find(tnode.node_type);
        const char* str = "unknown node type";

        if (i != _TPlanNodeType_VALUES_TO_NAMES.end()) {
            str = i->second;
        }

        error_msg << str << " not implemented";
        return Status(error_msg.str());
    }

    return Status::OK;
}

void ExecNode::set_debug_options(
    int node_id, TExecNodePhase::type phase, TDebugAction::type action,
    ExecNode* root) {
    if (root->_id == node_id) {
        root->_debug_phase = phase;
        root->_debug_action = action;
        return;
    }

    for (int i = 0; i < root->_children.size(); ++i) {
        set_debug_options(node_id, phase, action, root->_children[i]);
    }
}

std::string ExecNode::debug_string() const {
    std::stringstream out;
    this->debug_string(0, &out);
    return out.str();
}

void ExecNode::debug_string(int indentation_level, std::stringstream* out) const {
    *out << " conjuncts=" << Expr::debug_string(_conjuncts);
    *out << " id=" << _id;
    *out << " type=" << print_plan_node_type(_type);
    *out << " tuple_ids=[";
    for (auto id : _tuple_ids) {
        *out << id << ", ";
    }
    *out << "]";

    for (int i = 0; i < _children.size(); ++i) {
        *out << "\n";
        _children[i]->debug_string(indentation_level + 1, out);
    }
}

bool ExecNode::eval_conjuncts(ExprContext* const* ctxs, int num_ctxs, TupleRow* row) {
    for (int i = 0; i < num_ctxs; ++i) {
        BooleanVal v = ctxs[i]->get_boolean_val(row);
        if (v.is_null || !v.val) {
            return false;
        }
    }
    return true;
}

void ExecNode::collect_nodes(TPlanNodeType::type node_type, vector<ExecNode*>* nodes) {
    if (_type == node_type) {
        nodes->push_back(this);
    }

    for (int i = 0; i < _children.size(); ++i) {
        _children[i]->collect_nodes(node_type, nodes);
    }
}

void ExecNode::collect_scan_nodes(vector<ExecNode*>* nodes) {
    collect_nodes(TPlanNodeType::OLAP_SCAN_NODE, nodes);
    collect_nodes(TPlanNodeType::BROKER_SCAN_NODE, nodes);
}

void ExecNode::init_runtime_profile(const std::string& name) {
    std::stringstream ss;
    ss << name << " (id=" << _id << ")";
    _runtime_profile.reset(new RuntimeProfile(_pool, ss.str()));
    _runtime_profile->set_metadata(_id);
}

Status ExecNode::exec_debug_action(TExecNodePhase::type phase) {
    DCHECK(phase != TExecNodePhase::INVALID);

    if (_debug_phase != phase) {
        return Status::OK;
    }

    if (_debug_action == TDebugAction::FAIL) {
        return Status(TStatusCode::INTERNAL_ERROR, "Debug Action: FAIL");
    }

    if (_debug_action == TDebugAction::WAIT) {
        while (true) {
            sleep(1);
        }
    }

    return Status::OK;
}

// Codegen for EvalConjuncts.  The generated signature is
// For a node with two conjunct predicates
// define i1 @EvalConjuncts(%"class.impala::ExprContext"** %ctxs, i32 %num_ctxs,
//                          %"class.impala::TupleRow"* %row) #20 {
// entry:
//   %ctx_ptr = getelementptr %"class.impala::ExprContext"** %ctxs, i32 0
//   %ctx = load %"class.impala::ExprContext"** %ctx_ptr
//   %result = call i16 @Eq_StringVal_StringValWrapper3(
//       %"class.impala::ExprContext"* %ctx, %"class.impala::TupleRow"* %row)
//   %is_null = trunc i16 %result to i1
//   %0 = ashr i16 %result, 8
//   %1 = trunc i16 %0 to i8
//   %val = trunc i8 %1 to i1
//   %is_false = xor i1 %val, true
//   %return_false = or i1 %is_null, %is_false
//   br i1 %return_false, label %false, label %continue
//
// continue:                                         ; preds = %entry
//   %ctx_ptr2 = getelementptr %"class.impala::ExprContext"** %ctxs, i32 1
//   %ctx3 = load %"class.impala::ExprContext"** %ctx_ptr2
//   %result4 = call i16 @Gt_BigIntVal_BigIntValWrapper5(
//       %"class.impala::ExprContext"* %ctx3, %"class.impala::TupleRow"* %row)
//   %is_null5 = trunc i16 %result4 to i1
//   %2 = ashr i16 %result4, 8
//   %3 = trunc i16 %2 to i8
//   %val6 = trunc i8 %3 to i1
//   %is_false7 = xor i1 %val6, true
//   %return_false8 = or i1 %is_null5, %is_false7
//   br i1 %return_false8, label %false, label %continue1
//
// continue1:                                        ; preds = %continue
//   ret i1 true
//
// false:                                            ; preds = %continue, %entry
//   ret i1 false
// }
Function* ExecNode::codegen_eval_conjuncts(
        RuntimeState* state, const std::vector<ExprContext*>& conjunct_ctxs, const char* name) {
    Function* conjunct_fns[conjunct_ctxs.size()];
    for (int i = 0; i < conjunct_ctxs.size(); ++i) {
        Status status =
            conjunct_ctxs[i]->root()->get_codegend_compute_fn(state, &conjunct_fns[i]);
        if (!status.ok()) {
            VLOG_QUERY << "Could not codegen EvalConjuncts: " << status.get_error_msg();
            return NULL;
        }
    }
    LlvmCodeGen* codegen = NULL;
    if (!state->get_codegen(&codegen).ok()) {
        return NULL;
    }

    // Construct function signature to match
    // bool EvalConjuncts(Expr** exprs, int num_exprs, TupleRow* row)
    Type* tuple_row_type = codegen->get_type(TupleRow::_s_llvm_class_name);
    Type* expr_ctx_type = codegen->get_type(ExprContext::_s_llvm_class_name);

    DCHECK(tuple_row_type != NULL);
    DCHECK(expr_ctx_type != NULL);

    PointerType* tuple_row_ptr_type = PointerType::get(tuple_row_type, 0);
    PointerType* expr_ctx_ptr_type = PointerType::get(expr_ctx_type, 0);

    LlvmCodeGen::FnPrototype prototype(codegen, name, codegen->get_type(TYPE_BOOLEAN));
    prototype.add_argument(
        LlvmCodeGen::NamedVariable("ctxs", PointerType::get(expr_ctx_ptr_type, 0)));
    prototype.add_argument(
        LlvmCodeGen::NamedVariable("num_ctxs", codegen->get_type(TYPE_INT)));
    prototype.add_argument(LlvmCodeGen::NamedVariable("row", tuple_row_ptr_type));

    LlvmCodeGen::LlvmBuilder builder(codegen->context());
    Value* args[3];
    Function* fn = prototype.generate_prototype(&builder, args);
    Value* ctxs_arg = args[0];
    Value* tuple_row_arg = args[2];

    if (conjunct_ctxs.size() > 0) {
        LLVMContext& context = codegen->context();
        BasicBlock* false_block = BasicBlock::Create(context, "false", fn);

        for (int i = 0; i < conjunct_ctxs.size(); ++i) {
            BasicBlock* true_block = BasicBlock::Create(context, "continue", fn, false_block);

            Value* ctx_arg_ptr = builder.CreateConstGEP1_32(ctxs_arg, i, "ctx_ptr");
            Value* ctx_arg = builder.CreateLoad(ctx_arg_ptr, "ctx");
            Value* expr_args[] = { ctx_arg, tuple_row_arg };

            // Call conjunct_fns[i]
            CodegenAnyVal result = CodegenAnyVal::create_call_wrapped(
                codegen, &builder, conjunct_ctxs[i]->root()->type(),
                conjunct_fns[i], expr_args, "result", NULL);

            // Return false if result.is_null || !result
            Value* is_null = result.get_is_null();
            Value* is_false = builder.CreateNot(result.get_val(), "is_false");
            Value* return_false = builder.CreateOr(is_null, is_false, "return_false");
            builder.CreateCondBr(return_false, false_block, true_block);

            // Set insertion point for continue/end
            builder.SetInsertPoint(true_block);
        }
        builder.CreateRet(codegen->true_value());

        builder.SetInsertPoint(false_block);
        builder.CreateRet(codegen->false_value());
    } else {
        builder.CreateRet(codegen->true_value());
    }

    return codegen->finalize_function(fn);
}

}