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

#include "meta_state_machine.h"
#ifdef BAIDU_INTERNAL
#include "raft/util.h"
#include "raft/storage.h"
#else
#include <braft/util.h>
#include <braft/storage.h>
#endif
#include "cluster_manager.h"
#include "privilege_manager.h"
#include "schema_manager.h"
#include "namespace_manager.h"
#include "database_manager.h"
#include "table_manager.h"
#include "region_manager.h"
#include "meta_util.h"
#include "rocks_wrapper.h"

namespace baikaldb {
DECLARE_int32(healthy_check_interval_times);
DECLARE_int32(store_heart_beat_interval_us);
DECLARE_int32(balance_periodicity);

void MetaStateMachine::store_heartbeat(google::protobuf::RpcController* controller,
                                        const pb::StoreHeartBeatRequest* request,
                                        pb::StoreHeartBeatResponse* response,
                                        google::protobuf::Closure* done) {
    TimeCost time_cost;
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl =
            static_cast<brpc::Controller*>(controller);
    uint64_t log_id = 0;
    if (cntl->has_log_id()) {
        log_id = cntl->log_id();
    }
    if (!_is_leader.load()) {
        DB_WARNING("NOT LEADER, logid:%lu", log_id);
        response->set_errcode(pb::NOT_LEADER);
        response->set_errmsg("not leader");
        response->set_leader(_node.leader_id().to_string());
        return;
    }
    response->set_errcode(pb::SUCCESS);
    response->set_errmsg("success");
    TimeCost step_time_cost;
    ClusterManager::get_instance()->process_instance_heartbeat_for_store(request->instance_info());
    int64_t instance_time = step_time_cost.get_time();
    step_time_cost.reset();

    ClusterManager::get_instance()->process_peer_heartbeat_for_store(request, response);
    int64_t peer_balance_time = step_time_cost.get_time();
    step_time_cost.reset();

    SchemaManager::get_instance()->process_schema_heartbeat_for_store(request, response);
    int64_t schema_time = step_time_cost.get_time(); 
    step_time_cost.reset();

    SchemaManager::get_instance()->process_peer_heartbeat_for_store(request, response, log_id);
    int64_t peer_time = step_time_cost.get_time();
    step_time_cost.reset();

    SchemaManager::get_instance()->process_leader_heartbeat_for_store(request, response, log_id);
    int64_t leader_time = step_time_cost.get_time();

    SELF_TRACE("store:%s heart beat, time_cost: %ld, "
                "instance_time: %ld, peer_balance_time: %ld, schema_time: %ld,"
                " peer_time: %ld, leader_time: %ld "
                "response: %s, log_id: %lu", 
                request->instance_info().address().c_str(),
                time_cost.get_time(),
                instance_time, peer_balance_time, schema_time, peer_time, leader_time,
                response->ShortDebugString().c_str(), log_id);
}

void MetaStateMachine::baikal_heartbeat(google::protobuf::RpcController* controller,
                                        const pb::BaikalHeartBeatRequest* request,
                                        pb::BaikalHeartBeatResponse* response,
                                        google::protobuf::Closure* done) {
    TimeCost time_cost;
    brpc::ClosureGuard done_guard(done);
    brpc::Controller* cntl =
        static_cast<brpc::Controller*>(controller);
    uint64_t log_id = 0; 
    if (cntl->has_log_id()) {
        log_id = cntl->log_id();
    }
    if (!_is_leader.load()) {
        DB_WARNING("NOT LEADER, logid:%lu", log_id);
        response->set_errcode(pb::NOT_LEADER);
        response->set_errmsg("not leader");
        response->set_leader(_node.leader_id().to_string());
        return;
    }
    response->set_errcode(pb::SUCCESS);
    response->set_errmsg("success");
    ClusterManager::get_instance()->process_baikal_heartbeat(request, response);
    PrivilegeManager::get_instance()->process_baikal_heartbeat(request, response);
    SchemaManager::get_instance()->process_baikal_heartbeat(request, response, log_id);
    SELF_TRACE("baikaldb:%s heart beat, time_cost: %ld, response: %s, log_id: %lu", 
                butil::endpoint2str(cntl->remote_side()).c_str(),
                time_cost.get_time(),
                response->ShortDebugString().c_str(),
                log_id);
}

void MetaStateMachine::on_apply(braft::Iterator& iter) {
    for (; iter.valid(); iter.next()) {
        braft::Closure* done = iter.done();
        brpc::ClosureGuard done_guard(done);
        if (done) {
            ((MetaServerClosure*)done)->raft_time_cost = ((MetaServerClosure*)done)->time_cost.get_time();
        }
        butil::IOBufAsZeroCopyInputStream wrapper(iter.data());
        pb::MetaManagerRequest request;
        if (!request.ParseFromZeroCopyStream(&wrapper)) {
            DB_FATAL("parse from protobuf fail when on_apply");
            if (done) {
                if (((MetaServerClosure*)done)->response) {
                    ((MetaServerClosure*)done)->response->set_errcode(pb::PARSE_FROM_PB_FAIL);
                    ((MetaServerClosure*)done)->response->set_errmsg("parse from protobuf fail");
                }
                braft::run_closure_in_bthread(done_guard.release());
            }
            continue;
        }
        if (done && ((MetaServerClosure*)done)->response) {
            ((MetaServerClosure*)done)->response->set_op_type(request.op_type());
        }
        DB_NOTICE("on applye, term:%ld, index:%ld, request op_type:%s", 
                    iter.term(), iter.index(), 
                    pb::OpType_Name(request.op_type()).c_str());
        switch (request.op_type()) {
        case pb::OP_ADD_LOGICAL: {
            ClusterManager::get_instance()->add_logical(request, done);
            break;
        }
        case pb::OP_ADD_PHYSICAL: {
            ClusterManager::get_instance()->add_physical(request, done);
            break;
        }
        case pb::OP_ADD_INSTANCE: {
            ClusterManager::get_instance()->add_instance(request, done);
            break;
        }
        case pb::OP_DROP_PHYSICAL: {
            ClusterManager::get_instance()->drop_physical(request, done);
            break;
        }
        case pb::OP_DROP_LOGICAL: {
            ClusterManager::get_instance()->drop_logical(request, done);
            break;
        }
        case pb::OP_DROP_INSTANCE: {
            ClusterManager::get_instance()->drop_instance(request, done);
            break;
        }
        case pb::OP_UPDATE_INSTANCE: {
            ClusterManager::get_instance()->update_instance(request, done);
            break;
        }
        case pb::OP_MOVE_PHYSICAL: {
            ClusterManager::get_instance()->move_physical(request, done);
            break;
        }
        case pb::OP_CREATE_USER: {
             PrivilegeManager::get_instance()->create_user(request, done);
            break;
        }
        case pb::OP_DROP_USER: {
             PrivilegeManager::get_instance()->drop_user(request, done);
            break;
        }
        case pb::OP_ADD_PRIVILEGE: {
             PrivilegeManager::get_instance()->add_privilege(request, done);
            break;
        }
        case pb::OP_DROP_PRIVILEGE: {
             PrivilegeManager::get_instance()->drop_privilege(request, done);
            break;
        }
        case pb::OP_CREATE_NAMESPACE: {
            NamespaceManager::get_instance()->create_namespace(request, done);
            break;
        }
        case pb::OP_DROP_NAMESPACE: {
            NamespaceManager::get_instance()->drop_namespace(request, done);
            break;
        }
        case pb::OP_MODIFY_NAMESPACE: {
            NamespaceManager::get_instance()->modify_namespace(request, done);
            break;
        }
        case pb::OP_CREATE_DATABASE: {
            DatabaseManager::get_instance()->create_database(request, done);
            break;
        }
        case pb::OP_DROP_DATABASE: {
            DatabaseManager::get_instance()->drop_database(request, done);
            break;
        }
        case pb::OP_MODIFY_DATABASE: {
            DatabaseManager::get_instance()->modify_database(request, done);
            break;
        }
        case pb::OP_CREATE_TABLE: {
            TableManager::get_instance()->create_table(request, done);
            break;
        }
        case pb::OP_DROP_TABLE: {
            TableManager::get_instance()->drop_table(request, done);
            break;
        }
        case pb::OP_RENAME_TABLE: {
            TableManager::get_instance()->rename_table(request, done);
            break;
        }
        case pb::OP_ADD_FIELD: {
            TableManager::get_instance()->add_field(request, done);
            break;
        }
        case pb::OP_DROP_FIELD: {
            TableManager::get_instance()->drop_field(request, done);
            break;
        }
        case pb::OP_RENAME_FIELD: {
            TableManager::get_instance()->rename_field(request, done);
            break;
        }
        case pb::OP_MODIFY_FIELD: {
            TableManager::get_instance()->modify_field(request, done);
            break;
        }
        case pb::OP_UPDATE_BYTE_SIZE: {
            TableManager::get_instance()->update_byte_size(request, done);
            break;
        }
        case pb::OP_DROP_REGION: {
            RegionManager::get_instance()->drop_region(request, done);
            break;
        }
        case pb::OP_UPDATE_REGION: {
            RegionManager::get_instance()->update_region(request, done);
            break;
        }
        case pb::OP_RESTORE_REGION: {
            RegionManager::get_instance()->restore_region(request, done);
            break;
        }
        case pb::OP_SPLIT_REGION: {
            RegionManager::get_instance()->split_region(request, done);
            break;
        }
        default: {
            DB_FATAL("unsupport request type, type:%d", request.op_type());
            IF_DONE_SET_RESPONSE(done, pb::UNSUPPORT_REQ_TYPE, "unsupport request type");
        }
        }
        if (done) {
            braft::run_closure_in_bthread(done_guard.release());
        }
    }
}
void MetaStateMachine::on_snapshot_save(braft::SnapshotWriter* writer, braft::Closure* done) {
    DB_WARNING("start on shnapshot save");
    DB_WARNING("max_namespace_id: %ld, max_database_id: %ld,"
                " max_table_id:%ld, max_region_id:%ld when on snapshot save", 
                NamespaceManager::get_instance()->get_max_namespace_id(),
                DatabaseManager::get_instance()->get_max_database_id(),
                TableManager::get_instance()->get_max_table_id(),
                RegionManager::get_instance()->get_max_region_id());
    //创建snapshot
    rocksdb::ReadOptions read_options;
    read_options.prefix_same_as_start = false;
    read_options.total_order_seek = true;
    auto iter = RocksWrapper::get_instance()->new_iterator(read_options, 
                    RocksWrapper::get_instance()->get_meta_info_handle());
    iter->SeekToFirst();
    Bthread bth(&BTHREAD_ATTR_SMALL);
    std::function<void()> save_snapshot_function = [this, done, iter, writer]() {
            save_snapshot(done, iter, writer);
        };
    bth.run(save_snapshot_function);
}

void MetaStateMachine::save_snapshot(braft::Closure* done,
                                    rocksdb::Iterator* iter,
                                    braft::SnapshotWriter* writer) {
    brpc::ClosureGuard done_guard(done);
    std::unique_ptr<rocksdb::Iterator> iter_lock(iter);

    std::string snapshot_path = writer->get_path();
    std::string sst_file_path = snapshot_path + "/meta_info.sst";
    
    rocksdb::Options option = RocksWrapper::get_instance()->get_options(
                RocksWrapper::get_instance()->get_meta_info_handle());
    rocksdb::SstFileWriter sst_writer(rocksdb::EnvOptions(), option,
                                      RocksWrapper::get_instance()->get_meta_info_handle());
    DB_WARNING("snapshot path:%s", snapshot_path.c_str());
    //Open the file for writing 
    auto s = sst_writer.Open(sst_file_path);
    if (!s.ok()) {
        DB_WARNING("Error while opening file %s, Error: %s", sst_file_path.c_str(),
                    s.ToString().c_str());
        done->status().set_error(EINVAL, "Fail to open SstFileWriter");
        return;
    }
    for (; iter->Valid(); iter->Next()) {
        auto res = sst_writer.Put(iter->key(), iter->value());
        if (!res.ok()) {
            DB_WARNING("Error while adding Key: %s, Error: %s",
                    iter->key().ToString().c_str(),
                    s.ToString().c_str());
            done->status().set_error(EINVAL, "Fail to write SstFileWriter");
            return;
        }
    }
    //close the file
    s = sst_writer.Finish();
    if (!s.ok()) {
        DB_WARNING("Error while finishing file %s, Error: %s", sst_file_path.c_str(),
           s.ToString().c_str());
        done->status().set_error(EINVAL, "Fail to finish SstFileWriter");
        return;
    }
    if (writer->add_file("/meta_info.sst") != 0) {
        done->status().set_error(EINVAL, "Fail to add file");
        DB_WARNING("Error while adding file to writer");
        return;
    }
}

int MetaStateMachine::on_snapshot_load(braft::SnapshotReader* reader) {
    DB_WARNING("start on shnapshot load");
    //先删除数据
    std::string remove_start_key(MetaServer::CLUSTER_IDENTIFY);
    rocksdb::WriteOptions options;
    auto status = RocksWrapper::get_instance()->remove_range(options, 
                    RocksWrapper::get_instance()->get_meta_info_handle(),
                    remove_start_key, 
                    MetaServer::MAX_IDENTIFY);
    if (!status.ok()) {
        DB_FATAL("remove_range error when on snapshot load: code=%d, msg=%s",
            status.code(), status.ToString().c_str());
        return -1;
    } else {
        DB_WARNING("remove range success when on snapshot load:code:%d, msg=%s",
                status.code(), status.ToString().c_str());
    }
    DB_WARNING("clear data success");
    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> iter(RocksWrapper::get_instance()->new_iterator(read_options,
                                            RocksWrapper::get_instance()->get_meta_info_handle()));
    iter->Seek(MetaServer::CLUSTER_IDENTIFY);
    for (; iter->Valid(); iter->Next()) {
        DB_WARNING("iter key:%s, iter value:%s when on snapshot load", 
                    iter->key().ToString().c_str(), iter->value().ToString().c_str());
    }
    std::vector<std::string> files;
    reader->list_files(&files);
    for (auto& file : files) {
        DB_WARNING("snapshot load file:%s", file.c_str());
        if (file == "/meta_info.sst") {
            std::string snapshot_path = reader->get_path();
            snapshot_path.append("/meta_info.sst");
        
            //恢复文件
            rocksdb::IngestExternalFileOptions ifo;
            auto res = RocksWrapper::get_instance()->ingest_external_file(
                            RocksWrapper::get_instance()->get_meta_info_handle(), 
                            {snapshot_path}, 
                            ifo);
            if (!res.ok()) {
              DB_WARNING("Error while ingest file %s, Error %s",
                     snapshot_path.c_str(), res.ToString().c_str());
              return -1; 
                    
            }
            //恢复内存状态
            ClusterManager::get_instance()->load_snapshot();
            PrivilegeManager::get_instance()->load_snapshot();
            SchemaManager::get_instance()->load_snapshot();    
        }
    }
    return 0;
}

void MetaStateMachine::on_leader_start() {
    DB_WARNING("leader start at new term");
    ClusterManager::get_instance()->reset_instance_status();
    RegionManager::get_instance()->reset_region_status();
    _leader_start_timestmap = butil::gettimeofday_us();
    if (!_healthy_check_start) {
        std::function<void()> fun = [this]() {
                    healthy_check_function();};
        _bth.run(fun);
        _healthy_check_start = true;
    } else {
        DB_FATAL("store check thread has already started");    
    }
    CommonStateMachine::on_leader_start();
    _is_leader.store(true);
}

void MetaStateMachine::healthy_check_function() {
    DB_WARNING("start healthy check function");
    static int64_t count = 0;
    int64_t sleep_time_count = 
        FLAGS_healthy_check_interval_times * FLAGS_store_heart_beat_interval_us / 1000; //ms为单位               
    while (_node.is_leader()) { 
        int time = 0;                                                                     
        while (time < sleep_time_count) {
            if (!_node.is_leader()) {
                return; 
            }
            bthread_usleep(1000);                                                         
            ++time;                                                                       
        }
        SELF_TRACE("start healthy check(region and store), count: %ld", count);
        ++count;
        //store的相关信息目前存在cluster中
        ClusterManager::get_instance()->store_healthy_check_function();
        //region多久没上报心跳了
        RegionManager::get_instance()->region_healthy_check_function();
    }
    return;
}

void MetaStateMachine::on_leader_stop() {
    _is_leader.store(false);
    _load_balance = false;
    _unsafe_decision = false;
    if (_healthy_check_start) {
        _bth.join();
        _healthy_check_start = false;
        DB_WARNING("healthy check bthread join");
    }
    DB_WARNING("leader stop");
    CommonStateMachine::on_leader_stop();
}

bool MetaStateMachine::whether_can_decide() {
    return _node.is_leader() &&
            ((butil::gettimeofday_us()- _leader_start_timestmap) >
                2 * FLAGS_balance_periodicity * FLAGS_store_heart_beat_interval_us );
}
}//namespace
/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */
