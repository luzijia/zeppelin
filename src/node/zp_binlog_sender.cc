// Copyright 2017 Qihoo
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http:// www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "src/node/zp_binlog_sender.h"

#include <glog/logging.h>
#include <google/protobuf/text_format.h>
#include <limits>
#include <memory>

#include "slash/include/env.h"

#include "include/zp_const.h"
#include "src/node/zp_data_server.h"
#include "src/node/zp_data_partition.h"

extern ZPDataServer* zp_data_server;

std::string ZPBinlogSendTaskName(const std::string& table,
    int32_t id, const Node& target) {
  char buf[256];
  snprintf(buf, sizeof(buf), "%s_%d_%s_%d", table.c_str(), id,
      target.ip.c_str(), target.port);
  return std::string(buf);
}

/**
 * ZPBinlogSendTask
 */
Status ZPBinlogSendTask::Create(uint64_t seq, const std::string &table,
    int32_t id, const std::string& binlog_prefix,
    const Node& target, uint32_t ifilenum, uint64_t ioffset,
    ZPBinlogSendTask** tptr) {
  *tptr = NULL;
  ZPBinlogSendTask* task = new ZPBinlogSendTask(seq, table, id, binlog_prefix,
      target, ifilenum, ioffset);
  Status s = task->Init();
  if (s.ok()) {
    *tptr = task;
  } else {
    delete task;
  }
  return s;
}

ZPBinlogSendTask::ZPBinlogSendTask(uint64_t seq, const std::string &table,
    int32_t id, const std::string& binlog_prefix, const Node& target,
    uint32_t ifilenum, uint64_t ioffset) :
  send_next(true),
  sequence_(seq),
  table_name_(table),
  partition_id_(id),
  node_(target),
  filenum_(ifilenum),
  offset_(ioffset),
  process_error_time_(0),
  pre_filenum_(0),
  pre_offset_(0),
  pre_has_content_(false),
  binlog_filename_(binlog_prefix),
  queue_(NULL),
  reader_(NULL) {
    name_ = ZPBinlogSendTaskName(table, partition_id_, target);
    pre_content_.reserve(1024 * 1024);
  }

ZPBinlogSendTask::~ZPBinlogSendTask() {
  if (reader_) {
    delete reader_;
  }
  if (queue_) {
    delete queue_;
  }
}

Status ZPBinlogSendTask::Init() {
  std::string confile = NewFileName(binlog_filename_, filenum_);
  if (!slash::NewSequentialFile(confile, &queue_).ok()) {
    return Status::IOError("ZPBinlogSendTask Init new sequtial file failed");
  }
  reader_ = new BinlogReader(queue_);
  Status s = reader_->Seek(offset_);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

// Return Status::OK if has something to be send
Status ZPBinlogSendTask::ProcessTask() {
  if (reader_ == NULL || queue_ == NULL) {
    return Status::InvalidArgument("Error Task");
  }

  // Check task position
  BinlogOffset boffset;
  std::shared_ptr<Partition> partition =
    zp_data_server->GetTablePartitionById(table_name_, partition_id_);
  if (partition == NULL
      || !partition->opened()) {
    return Status::InvalidArgument("Error no exist or closed partition");
  }
  partition->GetBinlogOffsetWithLock(&boffset);
  if (filenum_ == boffset.filenum && offset_ == boffset.offset) {
    // No more binlog item in current task, switch to others
    return Status::EndFile("no more binlog item");
  }
  // LOG(INFO) << "Processing a task" << table_name_
  // << "parititon: " << partition_id_;
  RecordPreOffset();

  uint64_t consume_len = 0;
  Status s = reader_->Consume(&consume_len, &pre_content_);
  if (s.IsEndFile()) {
    // Roll to next File
    std::string confile = NewFileName(binlog_filename_, filenum_ + 1);

    if (slash::FileExists(confile)) {
      LOG(INFO) << "BinlogSender to " << node_ << " roll to new binlog "
        << confile << ", Partition: " << table_name_ << "_" << partition_id_;
      delete reader_;
      reader_ = NULL;
      delete queue_;
      queue_ = NULL;

      s = slash::NewSequentialFile(confile, &(queue_));
      if (!s.ok()) {
        LOG(WARNING) << "Failed to roll to next binlog file:" << (filenum_ + 1)
          << " Error:" << s.ToString() << ", Partition: " << table_name_
          << "_" << partition_id_ << ", Send to " << node_;
        return s;
      }
      reader_ = new BinlogReader(queue_);
      filenum_++;
      offset_ = 0;
      return ProcessTask();
    } else {
      LOG(WARNING) << "Read end of binlog file, but no next binlog exist:"
        << (filenum_ + 1) << ", Partition: " << table_name_
        << "_" << partition_id_ << ", Send to " << node_;
      return s;
    }
  } else if (s.IsIncomplete()) {
    LOG(WARNING) << "ZPBinlogSendTask Consume Incomplete record: "
      << s.ToString() << ", table: " << table_name_ << ", partition:"
      << partition_id_ << ", Send to " << node_;
  } else if (!s.ok()) {
    LOG(WARNING) << "ZPBinlogSendTask failed to Consume: " << s.ToString()
      << ", table: " << table_name_ << ", partition:" << partition_id_
      << ", Send to " << node_ << ", skip to next block";
    reader_->SkipNextBlock(&consume_len);
  }

  pre_has_content_ = s.ok();

  offset_ += consume_len;

  // Return OK even Incomplete or something wrong when consume
  // So that the caller could do the later sendtopeer
  // pre_has_content_ could distinguish this from the consume success situation
  return Status::OK();
}

// Build LEASE SyncRequest
void ZPBinlogSendTask::BuildLeaseSyncRequest(int64_t lease_time,
    client::SyncRequest* msg) const {
  msg->set_sync_type(client::SyncType::LEASE);
  msg->set_epoch(zp_data_server->meta_epoch());
  client::Node *node = msg->mutable_from();
  node->set_ip(zp_data_server->local_ip());
  node->set_port(zp_data_server->local_port());

  client::SyncLease* lease = msg->mutable_sync_lease();
  lease->set_table_name(table_name_);
  lease->set_partition_id(partition_id_);
  lease->set_lease(lease_time);
}

// Build CMD or SKIP SyncRequest by ZPBinlogSendTask
void ZPBinlogSendTask::BuildCommonSyncRequest(client::SyncRequest *msg) const {
  // Common part
  msg->set_epoch(zp_data_server->meta_epoch());
  client::Node *node = msg->mutable_from();
  node->set_ip(zp_data_server->local_ip());
  node->set_port(zp_data_server->local_port());
  client::SyncOffset *sync_offset = msg->mutable_sync_offset();
  sync_offset->set_filenum(pre_filenum_);
  sync_offset->set_offset(pre_offset_);

  // Different part
  if (pre_has_content_) {
    msg->set_sync_type(client::SyncType::CMD);
    client::CmdRequest *req_ptr = msg->mutable_request();
    client::CmdRequest req;
    assert(!pre_content_.empty());
    req.ParseFromString(pre_content_);
    req_ptr->CopyFrom(req);
  } else {
    msg->set_sync_type(client::SyncType::SKIP);
    client::BinlogSkip* skip = msg->mutable_binlog_skip();
    skip->set_table_name(table_name_);
    skip->set_partition_id(partition_id_);
    skip->set_gap(offset_ - pre_offset_);
  }
}

/**
 * ZPBinlogSendTaskPool
 */
ZPBinlogSendTaskPool::ZPBinlogSendTaskPool()
  : next_sequence_(0) {
  pthread_rwlock_init(&tasks_rwlock_, NULL);
  task_ptrs_.reserve(1000);
  LOG(INFO) << "size: " << tasks_.size();
}

ZPBinlogSendTaskPool::~ZPBinlogSendTaskPool() {
  std::list<ZPBinlogSendTask*>::iterator it;
  for (it = tasks_.begin(); it != tasks_.end(); ++it) {
    delete *it;
  }
  pthread_rwlock_destroy(&tasks_rwlock_);
}

bool ZPBinlogSendTaskPool::TaskExist(const std::string& task_name) {
  slash::RWLock l(&tasks_rwlock_, false);
  if (task_ptrs_.find(task_name) == task_ptrs_.end()) {
    return false;
  }
  return true;
}

// Create and add a new Task
Status ZPBinlogSendTaskPool::AddNewTask(const std::string &table_name,
    int32_t id, const std::string& binlog_filename, const Node& target,
    uint32_t ifilenum, uint64_t ioffset, bool force) {
  ZPBinlogSendTask* task_ptr = NULL;
  Status s = ZPBinlogSendTask::Create(next_sequence_++,
      table_name, id, binlog_filename, target, ifilenum, ioffset, &task_ptr);
  if (!s.ok()) {
    return s;
  }
  if (force && TaskExist(task_ptr->name())) {
    RemoveTask(task_ptr->name());
  }
  s = AddTask(task_ptr);
  if (!s.ok()) {
    delete task_ptr;
  }
  LOG(INFO) << "Add BinlogTask for Table:" << task_ptr->name()
    << ", partition: " << task_ptr->partition_id()
    << ", target: " << task_ptr->node()
    << ", sequence: " << task_ptr->sequence()
    << ", filenum: " << task_ptr->filenum()
    << ", ioffset: " << task_ptr->offset()
    << ", result: " << s.ToString(); 
  return s;
}

Status ZPBinlogSendTaskPool::AddTask(ZPBinlogSendTask* task) {
  assert(task != NULL);
  slash::RWLock l(&tasks_rwlock_, true);
  if (task_ptrs_.find(task->name()) != task_ptrs_.end()) {
    return Status::Complete("Task already exist");
  }
  tasks_.push_back(task);
  // index point to the last one just push back
  task_ptrs_[task->name()].iter = tasks_.end();
  --(task_ptrs_[task->name()].iter);
  task_ptrs_[task->name()].sequence = task->sequence();  // the latest one
  task_ptrs_[task->name()].filenum_snap = task->filenum();  // current filenum
  return Status::OK();
}

Status ZPBinlogSendTaskPool::RemoveTask(const std::string &name) {
  slash::RWLock l(&tasks_rwlock_, true);
  ZPBinlogSendTaskIndex::iterator it = task_ptrs_.find(name);
  if (it == task_ptrs_.end()) {
    return Status::NotFound("Task not exist");
  }
  // Task has been FetchOut should be deleted when Pushback
  if (it->second.iter != tasks_.end()) {
    delete *(it->second.iter);
    tasks_.erase(it->second.iter);
  }
  task_ptrs_.erase(it);
  return Status::OK();
}

// Return the task filenum indicated by id and node
// max() when the task is not exist
// -1 when the task is exist but is processing now
int32_t ZPBinlogSendTaskPool::TaskFilenum(const std::string &name) {
  slash::RWLock l(&tasks_rwlock_, false);
  ZPBinlogSendTaskIndex::iterator it = task_ptrs_.find(name);
  if (it == task_ptrs_.end()) {
    return std::numeric_limits<int32_t>::max();
  }
  if (it->second.iter == tasks_.end()) {
    // The task is processing by some thread
    // return its snapshot of last time
    return it->second.filenum_snap;
  }
  return (*(it->second.iter))->filenum();
}

// Fetch one task out from the front of tasks_ list
// and live the its ptr point to the tasks_.end()
// to distinguish from task has been removed
Status ZPBinlogSendTaskPool::FetchOut(ZPBinlogSendTask** task_ptr) {
  slash::RWLock l(&tasks_rwlock_, true);
  if (tasks_.size() == 0) {
    return Status::NotFound("No more task");
  }
  *task_ptr = tasks_.front();
  tasks_.pop_front();
  // Do not remove from the task_ptrs_ map
  // When the same task put back we need to know it is a old one
  task_ptrs_[(*task_ptr)->name()].iter = tasks_.end();
  return Status::OK();
}

// PutBack the task who has been FetchOut
// return NotFound when the task is not exist in index map task_pts_
// which mean the task has been removed or its not a task fetch out before
Status ZPBinlogSendTaskPool::PutBack(ZPBinlogSendTask* task) {
  slash::RWLock l(&tasks_rwlock_, true);
  ZPBinlogSendTaskIndex::iterator it = task_ptrs_.find(task->name());
  if (it == task_ptrs_.end()              // task has been removed
      || it->second.iter != tasks_.end()
        || it->second.sequence != task->sequence()) {  // task belong to
                                                       // same partition exist
    LOG(INFO) << "Remove BinlogTask when put back for Table:" << task->name()
      << ", partition: " << task->partition_id()
      << ", target: " << task->node()
      << ", sequence: " << task->sequence()
      << ", filenum: " << task->filenum()
      << ", ioffset: " << task->offset();
    delete task;
    return Status::NotFound("Task may have been deleted");
  }
  tasks_.push_back(task);
  it->second.iter = tasks_.end();
  --(it->second.iter);
  it->second.filenum_snap = task->filenum();
  return Status::OK();
}

void ZPBinlogSendTaskPool::Dump() {
  slash::RWLock l(&tasks_rwlock_, false);
  ZPBinlogSendTaskIndex::iterator it = task_ptrs_.begin();
  for (; it != task_ptrs_.end(); ++it) {
    std::list<ZPBinlogSendTask*>::iterator tptr = it->second.iter;
    LOG(INFO) << "----------------------------";
    LOG(INFO) << "+Binlog Send Task" << it->first;
    LOG(INFO) << "  +Sequence  " << it->second.sequence;
    if (tptr != tasks_.end()) {
      LOG(INFO) << "  +filenum " << (*tptr)->filenum();
      LOG(INFO) << "  +offset " << (*tptr)->offset();
    } else {
      LOG(INFO) << "  +filenum " << it->second.filenum_snap;
      LOG(INFO) << "  +Being occupied";
    }
    LOG(INFO) << "----------------------------";
  }
}

/**
 * ZPBinlogSendThread
 */

ZPBinlogSendThread::ZPBinlogSendThread(ZPBinlogSendTaskPool *pool)
  : pink::Thread::Thread(),
  pool_(pool) {
    set_thread_name("ZPDataSyncSender");
  }

ZPBinlogSendThread::~ZPBinlogSendThread() {
  StopThread();
  auto iter = peers_.begin();
  while (iter != peers_.end()) {
    iter->second->Close();
    delete iter->second;
    iter++;
  }
  LOG(INFO) << "a BinlogSender thread " << thread_id() << " exit!";
  }

// Send LEASE SyncRequest to peer
bool ZPBinlogSendThread::RenewPeerLease(ZPBinlogSendTask* task) {
  // In terms of the most conservative estimation,
  // current task will be fetch out from the pool
  // and be process again after lease_time
  int64_t lease_time = (pool_->Size() * kBinlogTimeSlice)
    / zp_data_server->binlog_sender_count() + kBinlogRedundantLease;
  if (lease_time < kBinlogMinLease) {
    // Set lower limit to avoid frequentlly trysync
    lease_time = kBinlogMinLease;
  }

  client::SyncRequest sreq;
  task->BuildLeaseSyncRequest(lease_time, &sreq);
  Status s = SendToPeer(task->node(), sreq);
  if (!s.ok()) {
    LOG(WARNING) << "Failed to send lease to peer " << task->node()
      << ", table:" << task->table_name() << ", partition:"
      << task->partition_id()
      << ", filenum:" << task->pre_filenum()
      << ", offset:" << task->pre_offset()
      << ", sequence:" << task->sequence()
      << ", thread:" << pthread_self()
      << ", Error: " << s.ToString();
  }

  return s.ok();
}

Status ZPBinlogSendThread::SendToPeer(const Node &node,
    const client::SyncRequest &msg) {
  pink::Status res;
  std::string ip_port = slash::IpPortString(node.ip, node.port);

  //slash::MutexLock pl(&mutex_peers_);
  std::unordered_map<std::string, pink::PinkCli*>::iterator iter
    = peers_.find(ip_port);
  if (iter == peers_.end()) {
    pink::PinkCli *cli = pink::NewPbCli();
    res = cli->Connect(node.ip, node.port);
    if (!res.ok()) {
      cli->Close();
      delete cli;
      return Status::Corruption(res.ToString());
    }
    cli->set_send_timeout(1000);
    cli->set_recv_timeout(1000);
    iter = (peers_.insert(std::pair<std::string,
          pink::PinkCli*>(ip_port, cli))).first;
  }

  res = iter->second->Send(const_cast<client::SyncRequest*>(&msg));
  if (!res.ok()) {
    // Remove when second Failed, retry outside
    iter->second->Close();
    delete iter->second;
    peers_.erase(iter);
    return Status::Corruption(res.ToString());
  }
  return Status::OK();
}

void* ZPBinlogSendThread::ThreadMain() {
  // Wait until the server is availible
  while (!should_stop() && !zp_data_server->Availible()) {
    sleep(kBinlogSendInterval);
  }

  while (!should_stop()) {
    ZPBinlogSendTask* task = NULL;
    Status s = pool_->FetchOut(&task);
    if (!s.ok()) {
      // No task to be processed
      sleep(kBinlogSendInterval);
      continue;
    }
    
    if (slash::NowMicros() - task->process_error_time()
        < kBinlogSendInterval * 1000000) {
      // Fetch out the task who was processed failed not far before
      // Means no much availible task left in the task queue,
      // So sleep to avoid rapidly loop
      sleep(kBinlogSendInterval);
    }

    // Fetched one task, process it
    Status item_s = Status::OK();
    uint64_t time_begin = slash::NowMicros();
    while (!should_stop()) {
      if (task->send_next) {
        // Process ProcessTask
        item_s = task->ProcessTask();
        if (item_s.IsEndFile()) {
          RenewPeerLease(task);
        }

        if (!item_s.ok()) {
          pool_->PutBack(task);
          task->renew_process_error_time();
          break;
        }
        // ProcessTask OK here
      }

      // Construct SyncRequest
      client::SyncRequest sreq;
      task->BuildCommonSyncRequest(&sreq);

      // Send SyncRequest
      if (!sreq.IsInitialized()) {
        std::string text_format;
        google::protobuf::TextFormat::PrintToString(sreq, &text_format);
        LOG(WARNING) << "Ignore error SyncRequest to be sent to: "
          << task->node() << ": [" << text_format << "]"
          << ", table:" << task->table_name()
          << ", partition:" << task->partition_id()
          << ", filenum:" << task->pre_filenum()
          << ", offset:" << task->pre_offset()
          << ", next filenum:" << task->filenum()
          << ", next offset:" << task->offset()
          << ", sequence:" << task->sequence();
        task->send_next = false;
        sleep(kBinlogSendInterval);
      } else {
        item_s = SendToPeer(task->node(), sreq);
        if (!item_s.ok()) {
          LOG(ERROR) << "Failed to send to peer " << task->node()
            << ", table:" << task->table_name() << ", partition:"
            << task->partition_id()
            << ", filenum:" << task->pre_filenum()
            << ", offset:" << task->pre_offset()
            << ", sequence:" << task->sequence()
            << ", thread:" << pthread_self()
            << ", Error: " << item_s.ToString();
          task->send_next = false;
          sleep(kBinlogSendInterval);
        } else {
          task->send_next = true;
        }
      }

      // Check if need to switch task
      if (slash::NowMicros() - time_begin > kBinlogTimeSlice * 1000000) {
        // Switch Task
        RenewPeerLease(task);
        pool_->PutBack(task);
        break;
      }
    }
  }

  return NULL;
}

