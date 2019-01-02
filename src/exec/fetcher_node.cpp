// Copyright (c) 2018 Baidu, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime_state.h"
#include "fetcher_node.h"
#include <gflags/gflags.h>
#ifdef BAIDU_INTERNAL
#include <baidu/rpc/channel.h>
#else
#include <brpc/channel.h>
#endif
#include "insert_node.h"
#include "network_socket.h"
#include "schema_factory.h"
#include "proto/store.interface.pb.h"

namespace baikaldb {

DEFINE_int32(retry_interval_us, 500 * 1000, "retry interval ");
DEFINE_int32(single_store_concurrency, 20, "max request for one store");

int FetcherNode::init(const pb::PlanNode& node) {
    int ret = 0;
    ret = ExecNode::init(node);
    if (ret < 0) {
        DB_WARNING("ExecNode::init fail, ret:%d", ret);
        return ret;
    }

    _op_type = node.derive_node().fetcher_node().op_type();
    for (auto& expr : node.derive_node().fetcher_node().slot_order_exprs()) {
        ExprNode* order_expr = nullptr;
        ret = ExprNode::create_tree(expr, &order_expr);
        if (ret < 0) {
            //如何释放资源
            return ret;
        }
        _slot_order_exprs.push_back(order_expr);
    }
    for (auto asc : node.derive_node().fetcher_node().is_asc()) {
        _is_asc.push_back(asc);
    }
    for (auto null_first : node.derive_node().fetcher_node().is_null_first()) {
        _is_null_first.push_back(null_first);
    }
    return 0;
}

int FetcherNode::send_request(
        RuntimeState* state, pb::RegionInfo& info, std::vector<SmartRecord>* records,
        int64_t region_id, uint64_t log_id, int retry_times, int start_seq_id) {
    if (_error) {
        DB_WARNING("recieve error, need not requeset to region_id: %ld", region_id);
        return -1;
    }
    //DB_WARNING("region_info; txn: %ld, %s, %lu", _txn_id, info.ShortDebugString().c_str(), records.size());
    if (retry_times >= 5) {
        DB_WARNING("region_id: %ld, txn_id: %lu, log_id:%lu rpc error; retry:%d", 
            region_id, state->txn_id, log_id, retry_times);
        return -1;
    }
    auto rand_peer_func = [this](pb::RegionInfo& info) -> std::string {
        uint32_t i = butil::fast_rand() % info.peers_size();
        return info.peers(i);
    };
    auto other_peer_to_leader_func = [this, rand_peer_func](pb::RegionInfo& info) {
        DB_WARNING("region_id:%ld choose rand old leader:%s", info.region_id(), info.leader().c_str());
        auto peer = rand_peer_func(info);
        if (peer != info.leader()) {
            info.set_leader(peer);
            return;
        }
        for (auto& peer : info.peers()) {
            if (peer != info.leader()) {
                info.set_leader(peer);
                break;
            }
        }
    };
    TimeCost cost;
    auto client_conn = state->client_conn();
    SchemaFactory* schema_factory = SchemaFactory::get_instance();
    pb::StoreReq req;
    pb::StoreRes res;
    brpc::Controller cntl;
    cntl.set_log_id(log_id);
    if (info.leader() == "0.0.0.0:0" || info.leader() == "") {
        info.set_leader(rand_peer_func(info));
        //other_peer_to_leader_func(info);
    }
    req.set_op_type(_op_type);
    req.set_region_id(region_id);
    req.set_region_version(info.version());
    req.set_log_id(log_id);
    for (auto& desc : state->tuple_descs()) {
        req.add_tuples()->CopyFrom(desc);
    }

    pb::TransactionInfo* txn_info = req.add_txn_infos();
    txn_info->set_txn_id(state->txn_id);
    txn_info->set_seq_id(state->seq_id);
    txn_info->set_autocommit(state->autocommit());
    for (int id : client_conn->need_rollback_seq) {
        txn_info->add_need_rollback_seq(id);
    }
    // for exec next_statement_after_begin, begin must be added
    if (client_conn->cache_plans.size() == 1 && state->autocommit() == false) {
        start_seq_id = 1;
    }
    // for autocommit prepare, begin and dml must be added
    if (state->autocommit() && _op_type == pb::OP_PREPARE) {
        start_seq_id = 1;
    }
    txn_info->set_start_seq_id(start_seq_id);
    txn_info->set_optimize_1pc(state->optimize_1pc());

    // DB_WARNING("txn_id: %lu, start_seq_id: %d, autocommit:%d", _txn_id, start_seq_id, state->autocommit());
    // 将缓存的plan中seq_id >= start_seq_id的部分追加到request中
    // rollback cmd does not need to send cache
    if (start_seq_id >= 0 && _op_type != pb::OP_ROLLBACK && _op_type != pb::OP_COMMIT) {
        for (auto& pair : client_conn->cache_plans) {
            auto& plan_item = pair.second;
            if (pair.first < start_seq_id || pair.first >= state->seq_id) {
                continue;
            }
            if (_op_type == pb::OP_PREPARE && plan_item.op_type() == pb::OP_PREPARE) {
                continue;
            }
            if (plan_item.tuples_size() > 0 && plan_item.tuples(0).table_id() != info.table_id()) {
                DB_WARNING("TransactionNote: cache_item table_id mismatch");
                continue;
            }
            txn_info->add_cache_plans()->CopyFrom(plan_item);
        }
    }
    // save region id for txn commit/rollback
    {
        std::lock_guard<std::mutex> lck(client_conn->region_lock);
        if (client_conn->region_infos.count(region_id) == 0) {
            client_conn->region_infos.insert(std::make_pair(region_id, info));
        }
    }
    {
        // 加锁处理insert node冲突问题
        std::lock_guard<std::mutex> lck(_region_lock);
        //只对insert数据做region拆分
        if ((_op_type == pb::OP_PREPARE && state->autocommit())
                || _op_type == pb::OP_INSERT) {
            InsertNode* insert_node = static_cast<InsertNode*>(
                    _children[0]->get_node(pb::INSERT_NODE));
            pb::InsertNode* pb_node = nullptr;
            if (insert_node != nullptr) {
                pb_node = insert_node->mutable_pb_node()->
                    mutable_derive_node()->mutable_insert_node();
            } else if (!state->autocommit()) {
                DB_WARNING("no insert/replace node");
                return -1;
            }
            if (pb_node != nullptr && records != nullptr) {   // only for replace and insert node
                pb_node->clear_records();
                for (auto& record : *records) {
                    std::string* str = pb_node->add_records();
                    record->encode(*str);
                }
            }
        }
        ExecNode::create_pb_plan(req.mutable_plan(), _children[0]);
    }
    brpc::Channel channel;
    brpc::ChannelOptions option;
    option.max_retry = 1;
    option.connect_timeout_ms = 3000; 
    option.timeout_ms = -1;
    int ret = 0;
    std::string addr;
    if (_op_type == pb::OP_SELECT) {
        //addr = rand_peer_func(info);
        //还是发给leader，但是不是leader也不失败
        addr = info.leader();
        //choose_opt_instance(info, addr);
        req.set_select_without_leader(true);
    } else {
        addr = info.leader();
    }
    ret = channel.Init(addr.c_str(), &option);
    if (ret != 0) {
        DB_WARNING("channel init failed, addr:%s, ret:%d, region_id: %ld, log_id:%lu", 
                addr.c_str(), ret, region_id, log_id);
        return -1;
    }
    pb::StoreService_Stub(&channel).query(&cntl, &req, &res, NULL);

    DB_WARNING("wait region_id: %ld version:%ld time:%ld log_id:%lu txn_id: %lu, ip:%s", 
            region_id, info.version(), cost.get_time(), log_id, state->txn_id,
            butil::endpoint2str(cntl.remote_side()).c_str());
    if (cntl.Failed()) {
        DB_WARNING("call failed region_id: %ld, error:%s, log_id:%lu", 
                region_id, cntl.ErrorText().c_str(), log_id);
        other_peer_to_leader_func(info);
        //schema_factory->update_leader(info);
        bthread_usleep(FLAGS_retry_interval_us);
        return send_request(state, info, records, region_id, log_id, retry_times + 1, start_seq_id);
    }
    if (res.errcode() == pb::NOT_LEADER) {
        int last_seq_id = res.has_last_seq_id()? res.last_seq_id() : 0;
        DB_WARNING("NOT_LEADER, region_id: %ld, retry:%d, new_leader:%s, log_id:%lu", 
                region_id, retry_times, res.leader().c_str(), log_id);

        if (res.leader() != "0.0.0.0:0") {
            info.set_leader(res.leader());
            schema_factory->update_leader(info);
            std::lock_guard<std::mutex> lck(client_conn->region_lock);
            client_conn->region_infos[region_id].set_leader(res.leader());
        } else {
            other_peer_to_leader_func(info);
        }
        bthread_usleep(FLAGS_retry_interval_us);
        return send_request(state, info, records, region_id, log_id, retry_times + 1, last_seq_id + 1);
    }
    if (res.errcode() == pb::TXN_FOLLOW_UP) {
        int last_seq_id = res.has_last_seq_id()? res.last_seq_id() : 0;
        DB_WARNING("TXN_FOLLOW_UP, region_id: %ld, retry:%d, log_id:%lu, op:%d, last_seq_id:%d", 
                region_id, retry_times, log_id, _op_type, last_seq_id + 1);
        //对于commit，store返回TXN_FOLLOW_UP不能重发缓存命令，需要手工处理
        //对于rollback, 直接忽略返回成功
        //其他命令需要重发缓存
        if (_op_type == pb::OP_COMMIT) {
            DB_FATAL("TransactionError: commit returns TXN_FOLLOW_UP: region_id: %ld, log_id:%lu, txn_id: %lu",
                region_id, log_id, state->txn_id);
            return -1;
        } else if (_op_type == pb::OP_ROLLBACK) {
            return 0;
        }
        return send_request(state, info, records, region_id, log_id, retry_times + 1, last_seq_id + 1);
    }
    //todo 需要处理分裂情况
    if (res.errcode() == pb::VERSION_OLD) {
        DB_WARNING("VERSION_OLD, region_id: %ld, retry:%d, now:%s, log_id:%lu", 
                region_id, retry_times, info.ShortDebugString().c_str(), log_id);
        if (res.regions_size() >= 2) {
            auto regions = res.regions();
            regions.Clear();
            for (auto r : res.regions()) {
                DB_WARNING("version region:%s", r.ShortDebugString().c_str());
                if (end_key_compare(r.end_key(), info.end_key()) > 0) {
                    DB_WARNING("region:%ld r.end_key:%s > info.end_key:%s", 
                            r.region_id(),
                            str_to_hex(r.end_key()).c_str(),
                            str_to_hex(info.end_key()).c_str());
                    continue;
                }
                *regions.Add() = r;
            }
            schema_factory->update_regions(regions);
            //auto orgin_info = res.regions(0);
            //auto new_info = res.regions(1);
            // 为了方便，串行执行
            // 靠store自己过滤数据
            bthread_usleep(FLAGS_retry_interval_us);
            if (_op_type == pb::OP_PREPARE && client_conn->transaction_has_write()) {
                state->set_optimize_1pc(false);
                DB_WARNING("TransactionNote: disable optimize_1pc due to split: txn_id: %lu, seq_id: %d, region_id: %ld", 
                state->txn_id, state->seq_id, region_id);
            }

            for (auto& r : regions) {
                auto r_copy = r;
                if (r_copy.region_id() != region_id) {
                    // Commit operator needs infinite try outside fetcher_node until success,
                    // update cached region_info in current connection may lead to partial update,
                    // further leading to some new regions missing commit.
                    // So we DO NOT update cached region_info for Commit.
                    if (_op_type != pb::OP_COMMIT) {
                        // update cached region_info in current connection
                        // update new_region info
                        std::lock_guard<std::mutex> lck(client_conn->region_lock);
                        client_conn->region_infos[r.region_id()] = r_copy;
                    }
                    ret = send_request(state, r_copy, records, r_copy.region_id(), log_id, retry_times + 1, 1);
                } else {
                    if (res.leader() != "0.0.0.0:0") {
                        DB_WARNING("region_id: %ld set new_leader: %s when old_version", 
                                region_id, r_copy.leader().c_str());
                        r_copy.set_leader(res.leader());
                    }
                    if (_op_type != pb::OP_COMMIT) {
                        // update cached region_info in current connection
                        // update old_region info
                        std::lock_guard<std::mutex> lck(client_conn->region_lock);
                        client_conn->region_infos[region_id].set_end_key(r_copy.end_key());
                        client_conn->region_infos[region_id].set_version(r_copy.version());
                        if (r_copy.leader() != "0.0.0.0:0") {
                            client_conn->region_infos[region_id].set_leader(r_copy.leader());
                        }
                    }
                    ret = send_request(state, r_copy, records, r_copy.region_id(), log_id, retry_times + 1, start_seq_id);
                }
                if (ret < 0) {
                    DB_WARNING("retry failed, region_id: %ld, log_id:%lu, txn_id: %lu", 
                            r_copy.region_id(), log_id, state->txn_id);
                    return ret;
                }
            }
            return 0;
        }
        return -1;
    }
    if (res.errcode() == pb::REGION_NOT_EXIST || res.errcode() == pb::INTERNAL_ERROR) {
        DB_WARNING("REGION_NOT_EXIST, region_id:%ld, retry:%d, new_leader:%s, log_id:%lu", 
                region_id, retry_times, res.leader().c_str(), log_id);
        other_peer_to_leader_func(info);
        bthread_usleep(FLAGS_retry_interval_us);
        return send_request(state, info, records, region_id, log_id, retry_times + 1, start_seq_id);
    }
    if (res.errcode() != pb::SUCCESS) {
        DB_WARNING("errcode:%d, msg:%s, failed, region_id:%ld, log_id:%lu", 
                res.errcode(), res.errmsg().c_str(), region_id, log_id);
        if (res.has_mysql_errcode()) {
            state->error_code = (MysqlErrCode)res.mysql_errcode();
            state->error_msg.str(res.errmsg());
        }
        return -1;
    }
    if (_op_type != pb::OP_SELECT) {
        _affected_rows += res.affected_rows();
        return 0;
    }
    if (res.leader() != "0.0.0.0:0" && res.leader() != "" && res.leader() != info.leader()) {
        info.set_leader(res.leader());
        schema_factory->update_leader(info);
        std::lock_guard<std::mutex> lck(client_conn->region_lock);
        client_conn->region_infos[region_id].set_leader(res.leader());
    }
    cost.reset();
    std::shared_ptr<RowBatch> batch = std::make_shared<RowBatch>();
    for (auto& pb_row : *res.mutable_row_values()) {
        std::unique_ptr<MemRow> row = state->mem_row_desc()->fetch_mem_row();
        for (int i = 0; i < res.tuple_ids_size(); i++) {
            int32_t tuple_id = res.tuple_ids(i);
            row->from_string(tuple_id, pb_row.tuple_values(i));
        }
        batch->move_row(std::move(row));
    }
    {
        std::lock_guard<std::mutex> lck(_region_lock);
        _start_key_sort[info.start_key()] = region_id;
        _region_batch[region_id] = batch;
    }
    DB_WARNING("parse region:%ld time:%ld rows:%u log_id:%lu ", 
            region_id, cost.get_time(), batch->size(), log_id);
    return 0;
}

int FetcherNode::push_cmd_to_cache(RuntimeState* state) {
    if (state->txn_id == 0) {
        return 0;
    }
    auto client = state->client_conn();
    // cache dml cmd in baikaldb before sending to store
    if (_op_type != pb::OP_INSERT 
            && _op_type != pb::OP_DELETE 
            && _op_type != pb::OP_UPDATE 
            && _op_type != pb::OP_BEGIN) {
        return 0;
    }
    // TODO: 缓存的plan中，insert record全部插入，暂时不做拆分
    if (_op_type == pb::OP_INSERT) {
        InsertNode* insert_node = static_cast<InsertNode*>(_children[0]->get_node(pb::INSERT_NODE));
        pb::InsertNode* pb_node = nullptr;
        if (insert_node != nullptr) {
            pb_node = insert_node->mutable_pb_node()->mutable_derive_node()->mutable_insert_node();
        } else if (!state->autocommit()) {
            DB_WARNING("no insert/replace node");
            return -1;
        }
        if (pb_node != nullptr) {   // only for replace and insert node
            pb_node->clear_records();
            for (auto& kv : _insert_region_ids) {
                for (auto& record : kv.second) {
                    std::string* str = pb_node->add_records();
                    record->encode(*str);
                }
            }
        }
    }
    pb::CachePlan plan_item;
    plan_item.set_op_type(_op_type);
    plan_item.set_seq_id(state->seq_id);
    pb::Plan* plan = plan_item.mutable_plan();
    ExecNode::create_pb_plan(plan, _children[0]);
    for (auto& desc : state->tuple_descs()) {
        plan_item.add_tuples()->CopyFrom(desc);
    }
    //DB_WARNING("add cmd to cache: %s", plan_item.ShortDebugString().c_str());
    client->cache_plans.insert({state->seq_id, plan_item});
    return 0;
}
void FetcherNode::choose_opt_instance(pb::RegionInfo& info, std::string& addr) {
    SchemaFactory* schema_factory = SchemaFactory::get_instance();
    std::string baikaldb_logical_room = schema_factory->get_logical_room();
    if (baikaldb_logical_room.empty()) {
        return;
    }
    std::vector<std::string> candicate_peers;
    for (auto& peer: info.peers()) {
        std::string logical_room = schema_factory->logical_room_for_instance(peer);
        if (!logical_room.empty()  && logical_room == baikaldb_logical_room) {
            candicate_peers.push_back(peer);
        }  
    }
    if (candicate_peers.size() > 0) {
        uint32_t i = butil::fast_rand() % candicate_peers.size();
        addr = candicate_peers[i];
    }
}

int FetcherNode::open(RuntimeState* state) {
    int ret = 0;
    auto client_conn = state->client_conn();
    if (client_conn == nullptr) {
        DB_WARNING("connection is nullptr: %lu, %d", state->txn_id, state->seq_id);
        return -1;
    }
    _error = false;
    //fetcher 的孩子运行在store上，可以认为无孩子
    for (auto expr : _slot_order_exprs) {
        ret = expr->open();
        if (ret < 0) {
            DB_WARNING("Expr::open fail:%d", ret);
            return ret;
        }
    }
    _mem_row_compare = std::make_shared<MemRowCompare>(_slot_order_exprs, _is_asc, _is_null_first);
    _sorter = std::make_shared<Sorter>(_mem_row_compare.get());

    // for txn control cmd, send to all relevant regions instead of only the current dml regions.
    if (_op_type == pb::OP_ROLLBACK || _op_type == pb::OP_PREPARE || _op_type == pb::OP_COMMIT) {
        _region_infos = client_conn->region_infos;
    }
    // 构造并发送请求
    std::map<std::string, std::set<int64_t>> send_region_ids_map; // leader ip => region_ids
    for (auto& pair : _region_infos) {
        send_region_ids_map[pair.second.leader()].insert(pair.first);
    }
    //DB_WARNING("send_region_ids: %lu, cached:%ld", send_region_ids.size(), client_conn->region_infos.size());
    if (send_region_ids_map.size() == 0) {
        push_cmd_to_cache(state);
        if (_op_type == pb::OP_PREPARE) {
            state->set_optimize_1pc(true);
        }
        return 0;
    }
    if ((_op_type == pb::OP_INSERT || _op_type == pb::OP_UPDATE || _op_type == pb::OP_DELETE)
            && state->autocommit() && state->txn_id != 0) {
        push_cmd_to_cache(state);
        client_conn->region_infos.insert(_region_infos.begin(), _region_infos.end());
        return 0;
    }
    uint64_t log_id = state->log_id();
    TimeCost cost;
    // when prepare txn, 2pc degenerates to 1pc when there is only 1 region or txn has no write
    if (_op_type == pb::OP_PREPARE) {
        if ((send_region_ids_map.size() == 1 && send_region_ids_map.begin()->second.size() == 1)
                || !client_conn->transaction_has_write()) {
            state->set_optimize_1pc(true);
            DB_WARNING("enable optimize_1pc: txn_id: %lu, seq_id: %d", state->txn_id, state->seq_id);
        }
    }

    BthreadCond store_cond; // 不同store发请全并发
    _affected_rows = 0;
    for (auto& pair : send_region_ids_map) {
        store_cond.increase();
        auto store_thread = [this, state, pair, log_id, &store_cond]() {
            ON_SCOPE_EXIT([&store_cond]{store_cond.decrease_signal();});
            BthreadCond cond(-FLAGS_single_store_concurrency); // 单store内并发数
            for (auto region_id : pair.second) {
                // 这两个资源后续不会分配新的，因此不需要加锁
                pb::RegionInfo* info = nullptr;
                if (_region_infos.count(region_id) != 0) {
                    info = &_region_infos[region_id];
                } else {
                    std::lock_guard<std::mutex> lck(state->client_conn()->region_lock);
                    info = &(state->client_conn()->region_infos[region_id]);
                }
                cond.increase();
                cond.wait();
                std::vector<SmartRecord>* records = nullptr;
                if (_insert_region_ids.count(region_id) == 1) {
                    records = &_insert_region_ids[region_id];
                }
                auto req_thread = [this, state, info, records, region_id, log_id, &cond]() {
                    ON_SCOPE_EXIT([&cond]{cond.decrease_signal();});
                    int ret = send_request(state, *info, records, region_id, log_id, 0, state->seq_id);
                    if (ret < 0) {
                        DB_WARNING("rpc error, region_id:%ld, log_id:%lu", region_id, log_id);
                        _error = true;
                    }
                };
                Bthread bth(&BTHREAD_ATTR_SMALL);
                bth.run(req_thread);
            }
            cond.wait(-FLAGS_single_store_concurrency);
        };
        Bthread bth(&BTHREAD_ATTR_SMALL);
        bth.run(store_thread);
    }
    store_cond.wait();
    if (_error) {
        DB_FATAL("fetcher node open fail, log_id:%lu, txn_id: %lu, seq_id: %d", 
            log_id, state->txn_id, state->seq_id);
        if (_op_type == pb::OP_INSERT || _op_type == pb::OP_DELETE || _op_type == pb::OP_UPDATE) {
            client_conn->need_rollback_seq.insert(state->seq_id);
        }
        return -1;
    }
    DB_WARNING("fetcher time:%ld, txn_id: %lu, log_id:%lu, batch_size:%lu", 
            cost.get_time(), state->txn_id, log_id, _region_batch.size());
    // 默认按主键排序，也就是按region的key排序
    if (_op_type == pb::OP_SELECT) {
        for (auto& pair : _start_key_sort) {
            auto& batch = _region_batch[pair.second];
            if (batch != NULL && batch->size() != 0) {
                _sorter->add_batch(batch);
            }
        }
        // 无sort节点时不会排序，按顺序输出
        _sorter->merge_sort();
    }
    // cache dml cmd in baikaldb before sending to store_affected_rows
    push_cmd_to_cache(state);
    return _affected_rows.load();
}

int FetcherNode::get_next(RuntimeState* state, RowBatch* batch, bool* eos) {
    if (reached_limit()) {
        *eos = true;
        return 0;
    }
    int ret = 0;
    ret = _sorter->get_next(batch, eos);
    if (ret < 0) {
        DB_WARNING("sort get_next fail");
        return ret;
    }
    _num_rows_returned += batch->size();
    //DB_WARNING("_num_rows_returned:%ld", _num_rows_returned);
    if (reached_limit()) {
        *eos = true;
        _num_rows_returned = _limit;
        return 0;
    }
    return 0;
}
}

/* vim: set ts=4 sw=4 sts=4 tw=100 */
