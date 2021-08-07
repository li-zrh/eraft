// MIT License

// Copyright (c) 2021 Colin

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <Kv/concurrency_queue.h>
#include <Kv/peer_msg_handler.h>
#include <Kv/utils.h>
#include <Logger/logger.h>
#include <RaftCore/raw_node.h>
#include <RaftCore/util.h>

#include <cassert>

namespace kvserver {

PeerMsgHandler::PeerMsgHandler(std::shared_ptr<Peer> peer,
                               std::shared_ptr<GlobalContext> ctx) {
  this->peer_ = peer;
  this->ctx_ = ctx;
}

PeerMsgHandler::~PeerMsgHandler() {}

void PeerMsgHandler::Read() {}

void PeerMsgHandler::HandleProposal(eraftpb::Entry* entry,
                                    std::function<void(Proposal*)> handle) {
  Logger::GetInstance()->DEBUG_NEW(
      "handle_proposal size " + std::to_string(this->peer_->proposals_.size()),
      __FILE__, __LINE__, "PeerMsgHandler::HandleProposal");
  if (this->peer_->proposals_.size() > 0) {
    Proposal p = this->peer_->proposals_[0];
    if (p.index_ == entry->index()) {
      if (p.term_ != entry->term()) {
        Logger::GetInstance()->DEBUG_NEW("stale req", __FILE__, __LINE__,
                                         "PeerMsgHandler::HandleProposal");
      } else {
        handle(&p);
      }
    }
    this->peer_->proposals_.erase(this->peer_->proposals_.begin());
  }
}

std::shared_ptr<std::string> PeerMsgHandler::GetRequestKey(
    raft_cmdpb::Request* req) {
  std::string key;
  switch (req->cmd_type()) {
    case raft_cmdpb::CmdType::Get: {
      key = req->get().key();
      break;
    }
    case raft_cmdpb::CmdType::Put: {
      key = req->put().key();
      break;
    }
    case raft_cmdpb::CmdType::Delete: {
      key = req->delete_().key();
      break;
    }
    default:

      break;
  }
  return std::make_shared<std::string>(key);
}

void PeerMsgHandler::Handle(Proposal* p) {}

std::shared_ptr<rocksdb::WriteBatch> PeerMsgHandler::ProcessRequest(
    eraftpb::Entry* entry, raft_cmdpb::RaftCmdRequest* msg,
    std::shared_ptr<rocksdb::WriteBatch> wb) {
  auto req = msg->requests()[0];
  std::shared_ptr<std::string> key = GetRequestKey(&req);
  Logger::GetInstance()->DEBUG_NEW("process request key: " + *key, __FILE__,
                                   __LINE__, "PeerMsgHandler::ProcessRequest");
  if (key != nullptr) {
    // TODO: check key in region
    // this->HandleProposal(entry, );
  }
  switch (req.cmd_type()) {
    case raft_cmdpb::CmdType::Get: {
      break;
    }
    case raft_cmdpb::CmdType::Put: {
      wb->Put(
          Assistant::GetInstance()->KeyWithCF(req.put().cf(), req.put().key()),
          req.put().value());
      Logger::GetInstance()->DEBUG_NEW(
          "process put request with cf " + req.put().cf() + " key " +
              req.put().key() + " value " + req.put().value(),
          __FILE__, __LINE__, "PeerMsgHandler::ProcessRequest");
      break;
    }
    case raft_cmdpb::CmdType::Delete: {
      wb->Delete(Assistant::GetInstance()->KeyWithCF(req.delete_().cf(),
                                                     req.delete_().key()));
      Logger::GetInstance()->DEBUG_NEW(
          "process delete request with cf " + req.delete_().cf() + " key " +
              req.delete_().key(),
          __FILE__, __LINE__, "PeerMsgHandler::ProcessRequest");
      break;
    }
    case raft_cmdpb::CmdType::Snap: {
      break;
    }
    default:
      break;
  }

  this->HandleProposal(
      entry,
      [&](Proposal* p) {  // 以引用形式捕获所有外部变量
        raft_cmdpb::RaftCmdResponse* resp = new raft_cmdpb::RaftCmdResponse();
        switch (req.cmd_type()) {
          case raft_cmdpb::CmdType::Get: {
            // update apply state to db
            this->peer_->peerStorage_->applyState_->set_applied_index(
                entry->index());
            std::string applyKey(Assistant::GetInstance()
                                     ->ApplyStateKey(this->peer_->regionId_)
                                     .begin(),
                                 Assistant::GetInstance()
                                     ->ApplyStateKey(this->peer_->regionId_)
                                     .end());
            Assistant::GetInstance()->SetMeta(
                wb.get(), applyKey, *this->peer_->peerStorage_->applyState_);
            this->peer_->peerStorage_->engines_->kvDB_->Write(
                rocksdb::WriteOptions(), &*wb);
            // get value from db
            std::string value = Assistant::GetInstance()->GetCF(
                this->peer_->peerStorage_->engines_->kvDB_, req.get().cf(),
                req.get().key());
            raft_cmdpb::Response* sp = resp->add_responses();
            sp->set_cmd_type(raft_cmdpb::CmdType::Get);
            sp->mutable_get()->set_value(value);
            break;
          }
          case raft_cmdpb::CmdType::Put: {
            raft_cmdpb::Response* sp = resp->add_responses();
            sp->set_cmd_type(raft_cmdpb::CmdType::Put);
            raft_cmdpb::PutResponse* psp = new raft_cmdpb::PutResponse();
            sp->set_allocated_put(psp);
            break;
          }
          case raft_cmdpb::CmdType::Delete: {
            raft_cmdpb::Response* sp = resp->add_responses();
            sp->set_cmd_type(raft_cmdpb::CmdType::Delete);
            raft_cmdpb::DeleteResponse* deleteResp =
                new raft_cmdpb::DeleteResponse();
            sp->set_allocated_delete_(deleteResp);
            break;
          }
          case raft_cmdpb::CmdType::Snap: {
            // TODO: snap
            break;
          }
          default:
            break;
        }
        p->cb_->Done(resp);
      });

  return wb;
}

void PeerMsgHandler::ProcessAdminRequest(
    eraftpb::Entry* entry, raft_cmdpb::RaftCmdRequest* req,
    std::shared_ptr<rocksdb::WriteBatch> wb) {}

size_t PeerMsgHandler::SearchRegionPeer(std::shared_ptr<metapb::Region> region,
                                        uint64_t id) {
  size_t i = 0;
  for (auto peer : region->peers()) {
    if (peer.id() == id) {
      return i;
    }
    ++i;
  }
  return region->peers().size();
}

void PeerMsgHandler::ProcessConfChange(
    eraftpb::Entry* entry, eraftpb::ConfChange* cc,
    std::shared_ptr<rocksdb::WriteBatch> wb) {
  raft_cmdpb::RaftCmdRequest msg;
  msg.ParseFromString(cc->context());

  auto region = this->peer_->Region();
  // TODO: check region epoch

  switch (cc->change_type()) {
    case eraftpb::ConfChangeType::AddNode: {
      auto n = this->SearchRegionPeer(region, cc->node_id());
      if (n == region->peers().size()) {
        auto p = msg.admin_request().change_peer().peer();
        auto addPeer = region->add_peers();
        addPeer->set_id(p.id());
        addPeer->set_store_id(p.store_id());
        // incr conf ver
        uint64_t confVer = region->region_epoch().conf_ver();
        auto epoch = region->mutable_region_epoch();
        epoch->set_conf_ver(++confVer);
        // store region state
        Assistant::GetInstance()->WriteRegionState(
            wb, region, raft_serverpb::PeerState::Normal);
        auto storeMeta = this->ctx_->storeMeta_;
        storeMeta->mutex_.lock();
        storeMeta->regions_[region->id()] = &*region;
        storeMeta->mutex_.unlock();
        this->peer_->InsertPeerCache(addPeer);
      }
      break;
    }
    case eraftpb::ConfChangeType::RemoveNode: {
      if (cc->node_id() == this->peer_->meta_->id()) {
        this->DestoryPeer();
        return;
      }
      size_t n = this->SearchRegionPeer(region, cc->node_id());
      if (n < region->peers().size()) {
        // erase no n peer
        auto peers = region->mutable_peers();
        peers->erase(peers->begin() + n);
        // incr conf ver
        uint64_t confVer = region->region_epoch().conf_ver();
        auto epoch = region->mutable_region_epoch();
        epoch->set_conf_ver(++confVer);
        // store region state
        Assistant::GetInstance()->WriteRegionState(
            wb, region, raft_serverpb::PeerState::Normal);
        auto storeMeta = this->ctx_->storeMeta_;
        storeMeta->mutex_.lock();
        storeMeta->regions_[region->id()] = &*region;
        storeMeta->mutex_.unlock();
        this->peer_->RemovePeerCache(cc->node_id());
      }
      break;
    }
    default:
      break;
  }
  this->peer_->raftGroup_->ApplyConfChange(*cc);
  this->HandleProposal(entry, [&](Proposal* p) {
    raft_cmdpb::AdminResponse* adminResp = new raft_cmdpb::AdminResponse;
    adminResp->set_cmd_type(raft_cmdpb::AdminCmdType::ChangePeer);
    raft_cmdpb::RaftCmdResponse* raftCmdResp = new raft_cmdpb::RaftCmdResponse;
    raftCmdResp->set_allocated_admin_response(adminResp);
    p->cb_->Done(raftCmdResp);
  });
}

std::shared_ptr<rocksdb::WriteBatch> PeerMsgHandler::Process(
    eraftpb::Entry* entry, std::shared_ptr<rocksdb::WriteBatch> wb) {
  if (entry->entry_type() == eraftpb::EntryType::EntryConfChange) {
    eraftpb::ConfChange* cc = new eraftpb::ConfChange();
    // cc->ParseFromString(entry->data());
    this->ProcessConfChange(entry, cc, wb);
    return wb;
  }
  kvrpcpb::RawPutRequest* msg = new kvrpcpb::RawPutRequest();
  msg->ParseFromString(entry->data());
  Logger::GetInstance()->DEBUG_NEW(
      "Process Entry DATA: cf " + msg->cf() + " key " + msg->key() + " val " +
          msg->value(),
      __FILE__, __LINE__, "eerMsgHandler::Process");
  // write to kv db
  auto status =
      wb->Put(Assistant().GetInstance()->KeyWithCF(msg->cf(), msg->key()),
              msg->value());
  if (!status.ok()) {
    Logger::GetInstance()->DEBUG_NEW(
        "err: when process entry data() cf " + msg->cf() + " key " +
            msg->key() + " val " + msg->value() + ")",
        __FILE__, __LINE__, "eerMsgHandler::Process");
  }
  return wb;
}

void PeerMsgHandler::HandleRaftReady() {
  Logger::GetInstance()->DEBUG_NEW("handle raft ready ", __FILE__, __LINE__,
                                   "PeerMsgHandler::HandleRaftReady");
  if (this->peer_->stopped_) {
    return;
  }
  if (this->peer_->raftGroup_->HasReady()) {
    Logger::GetInstance()->DEBUG_NEW("raft group is ready!", __FILE__, __LINE__,
                                     "PeerMsgHandler::HandleRaftReady");
    eraft::DReady rd = this->peer_->raftGroup_->EReady();
    auto result = this->peer_->peerStorage_->SaveReadyState(
        std::make_shared<eraft::DReady>(rd));
    if (result != nullptr) {
    }
    this->peer_->Send(this->ctx_->trans_, rd.messages);
    if (rd.committedEntries.size() > 0) {
      Logger::GetInstance()->DEBUG_NEW(
          "rd.committedEntries.size() " +
              std::to_string(rd.committedEntries.size()),
          __FILE__, __LINE__, "PeerMsgHandler::HandleRaftReady");
      // std::vector<Proposal> oldProposal = this->peer_->proposals_;
      std::shared_ptr<rocksdb::WriteBatch> kvWB =
          std::make_shared<rocksdb::WriteBatch>();
      for (auto entry : rd.committedEntries) {
        Logger::GetInstance()->DEBUG_NEW(
            "COMMIT_ENTRY" + eraft::EntryToString(entry), __FILE__, __LINE__,
            "PeerMsgHandler::HandleRaftReady");
        kvWB = this->Process(&entry, kvWB);
        if (this->peer_->stopped_) {
          return;
        }
      }
      //
      // TODO: a bug
      auto lastEnt = rd.committedEntries[rd.committedEntries.size() - 1];
      this->peer_->peerStorage_->applyState_->set_applied_index(
          lastEnt.index());
      this->peer_->peerStorage_->applyState_->mutable_truncated_state()
          ->set_index(lastEnt.index());
      this->peer_->peerStorage_->applyState_->mutable_truncated_state()
          ->set_term(lastEnt.term());
      Logger::GetInstance()->DEBUG_NEW(
          "write to db applied index: " + std::to_string(lastEnt.index()) +
              " truncated state index: " + std::to_string(lastEnt.index()) +
              " truncated state term " + std::to_string(lastEnt.term()),
          __FILE__, __LINE__, "PeerMsgHandler::HandleRaftReady");
      Assistant::GetInstance()->SetMeta(
          kvWB.get(),
          Assistant::GetInstance()->ApplyStateKey(this->peer_->regionId_),
          *this->peer_->peerStorage_->applyState_);
      this->peer_->peerStorage_->engines_->kvDB_->Write(rocksdb::WriteOptions(),
                                                        kvWB.get());
    }
    this->peer_->raftGroup_->Advance(rd);
  }
}

void PeerMsgHandler::HandleMsg(Msg m) {
  Logger::GetInstance()->DEBUG_NEW("handle raft msg type " + m.MsgToString(),
                                   __FILE__, __LINE__,
                                   "PeerMsgHandler::HandleMsg");
  switch (m.type_) {
    case MsgType::MsgTypeRaftMessage: {
      if (m.data_ != nullptr) {
        try {
          auto raftMsg = static_cast<raft_serverpb::RaftMessage*>(m.data_);
          switch (raftMsg->raft_msg_type()) {
            case raft_serverpb::RaftMsgNormal: {
              if (raftMsg == nullptr) {
                Logger::GetInstance()->DEBUG_NEW("err: nil message", __FILE__,
                                                 __LINE__,
                                                 "PeerMsgHandler::HandleMsg");
                return;
              }
              if (!this->OnRaftMsg(raftMsg)) {
                Logger::GetInstance()->DEBUG_NEW("err: on handle raft msg",
                                                 __FILE__, __LINE__,
                                                 "PeerMsgHandler::HandleMsg");
              }
              break;
            }
            case raft_serverpb::RaftMsgClientCmd: {
              std::shared_ptr<kvrpcpb::RawPutRequest> put =
                  std::make_shared<kvrpcpb::RawPutRequest>();
              put->set_key(raftMsg->data());
              Logger::GetInstance()->DEBUG_NEW("PROPOSE NEW: " + put->key(),
                                               __FILE__, __LINE__,
                                               "PeerMsgHandler::HandleMsg");
              this->ProposeRaftCommand(put.get());
              break;
            }
            case raft_serverpb::RaftTransferLeader: {
              std::shared_ptr<raft_cmdpb::TransferLeaderRequest> tranLeader =
                  std::make_shared<raft_cmdpb::TransferLeaderRequest>();
              tranLeader->ParseFromString(raftMsg->data());
              Logger::GetInstance()->DEBUG_NEW(
                  "transfer leader with peer id = " +
                      std::to_string(tranLeader->peer().id()),
                  __FILE__, __LINE__, "PeerMsgHandler::HandleMsg");
              this->peer_->raftGroup_->TransferLeader(tranLeader->peer().id());
              break;
            }
            default:
              break;
          }
        } catch (const std::exception& e) {
          std::cerr << e.what() << '\n';
        }
      }
      break;
    }
    case MsgType::MsgTypeTick: {
      this->OnTick();
      break;
    }
    case MsgType::MsgTypeSplitRegion: {
      break;
    }
    case MsgType::MsgTypeRegionApproximateSize: {
      break;
    }
    case MsgType::MsgTypeGcSnap: {
      break;
    }
    case MsgType::MsgTypeStart: {
      this->StartTicker();
      break;
    }
    default:
      break;
  }
}

bool PeerMsgHandler::CheckStoreID(raft_cmdpb::RaftCmdRequest* req,
                                  uint64_t storeID) {
  auto peer = req->header().peer();
  if (peer.store_id() == storeID) {
    return true;
  }
  return false;  // store not match
}

bool PeerMsgHandler::PreProposeRaftCommand(raft_cmdpb::RaftCmdRequest* req) {
  if (!this->CheckStoreID(req, this->peer_->meta_->store_id())) {
    // check store id, make sure that msg is dispatched to the right place
    return false;
  }
  auto regionID = this->peer_->regionId_;
  auto leaderID = this->peer_->LeaderId();
  if (!this->peer_->IsLeader()) {
    auto leader = this->peer_->GetPeerFromCache(leaderID);
    // log err not leader
    return false;
  }
  if (!this->CheckPeerID(req, this->peer_->PeerId())) {
    // peer_id must be the same as peer's
    return false;
  }
  // check whether the term is stale.
  if (!this->CheckTerm(req, this->peer_->Term())) {
    return false;
  }
  return true;  // no err
}

bool PeerMsgHandler::CheckPeerID(raft_cmdpb::RaftCmdRequest* req,
                                 uint64_t peerID) {
  auto peer = req->header().peer();
  if (peer.id() == peerID) {
    return true;
  }
  return false;
}

bool PeerMsgHandler::CheckTerm(raft_cmdpb::RaftCmdRequest* req, uint64_t term) {
  auto header = req->header();
  if (header.term() == 0 || term <= header.term() + 1) {
    return true;
  }
  return false;
}

bool PeerMsgHandler::CheckKeyInRegion(std::string key,
                                      std::shared_ptr<metapb::Region> region) {
  if (key.compare(region->start_key()) >= 0 &&
      (region->end_key().size() == 0 || key.compare(region->end_key()) < 0)) {
    return true;
  } else {
    // key is not in range [`start_key`, `end_key`)
    return false;
  }
}

void PeerMsgHandler::ProposeAdminRequest(raft_cmdpb::RaftCmdRequest* msg,
                                         Callback* cb) {}

void PeerMsgHandler::ProposeRequest(kvrpcpb::RawPutRequest* put) {
  Logger::GetInstance()->DEBUG_NEW(
      "PROPOSE TEST KEY ================= " + put->key(), __FILE__, __LINE__,
      "PeerMsgHandler::ProposeRequest");
  std::string data = put->key();
  this->peer_->raftGroup_->Propose(data);
}

void PeerMsgHandler::ProposeRaftCommand(kvrpcpb::RawPutRequest* put) {
  // TODO: check put
  this->ProposeRequest(put);
}

void PeerMsgHandler::OnTick() {
  Logger::GetInstance()->DEBUG_NEW(
      "NODE STATE:" +
          eraft::StateToString(this->peer_->raftGroup_->raft->state_),
      __FILE__, __LINE__, "PeerMsgHandler::OnTick");

  if (this->peer_->stopped_) {
    return;
  }

  this->OnRaftBaseTick();
  QueueContext::GetInstance()->regionIdCh_.Push(this->peer_->regionId_);
}

void PeerMsgHandler::StartTicker() {
  Logger::GetInstance()->DEBUG_NEW("start ticker", __FILE__, __LINE__,
                                   "PeerMsgHandler::StartTicker");
  QueueContext::GetInstance()->regionIdCh_.Push(this->peer_->regionId_);
}

void PeerMsgHandler::OnRaftBaseTick() { this->peer_->raftGroup_->Tick(); }

void PeerMsgHandler::ScheduleCompactLog(uint64_t firstIndex,
                                        uint64_t truncatedIndex) {}

bool PeerMsgHandler::OnRaftMsg(raft_serverpb::RaftMessage* msg) {
  // if(!this->ValidateRaftMessage(msg))
  // {
  //     return false;
  // }
  Logger::GetInstance()->DEBUG_NEW(
      "on raft msg from " + std::to_string(msg->message().from()) + " to " +
          std::to_string(msg->message().to()) + " index " +
          std::to_string(msg->message().index()) + " term " +
          std::to_string(msg->message().term()) + " type " +
          eraft::MsgTypeToString(msg->message().msg_type()) + " ents.size " +
          std::to_string(msg->message().entries_size()),
      __FILE__, __LINE__, "PeerMsgHandler::OnRaftMsg");

  if (!msg->has_message()) {
    Logger::GetInstance()->DEBUG_NEW("nil message", __FILE__, __LINE__,
                                     "PeerMsgHandler::OnRaftMsg");
    return false;
  }

  if (this->peer_->stopped_) {
    return false;
  }
  eraftpb::Message newMsg;
  newMsg.set_from(msg->message().from());
  newMsg.set_to(msg->message().to());
  newMsg.set_index(msg->message().index());
  newMsg.set_term(msg->message().term());
  newMsg.set_commit(msg->message().commit());
  newMsg.set_log_term(msg->message().log_term());
  newMsg.set_reject(msg->message().reject());
  newMsg.set_msg_type(msg->message().msg_type());
  newMsg.set_temp_data(msg->message().temp_data());
  Logger::GetInstance()->DEBUG_NEW(
      "RECIVED ENTRY DATA = " + msg->message().temp_data(), __FILE__, __LINE__,
      "RaftContext::OnRaftMsg");
  for (auto e : msg->message().entries()) {
    eraftpb::Entry* newE = newMsg.add_entries();
    newE->set_index(e.index());
    newE->set_data(e.data());
    newE->set_entry_type(e.entry_type());
    newE->set_term(e.term());
  }

  this->peer_->raftGroup_->Step(newMsg);

  return true;
}

bool PeerMsgHandler::ValidateRaftMessage(raft_serverpb::RaftMessage* msg) {
  auto to = msg->to_peer();

  if (to.store_id() != this->peer_->storeID()) {
    return false;
  }

  //
  // TODO: check region repoch
  //
  return true;
}

bool PeerMsgHandler::CheckMessage(raft_serverpb::RaftMessage* msg) {
  auto fromEpoch = msg->region_epoch();
  auto isVoteMsg =
      (msg->message().msg_type() == eraftpb::MessageType::MsgRequestVote);
  auto fromStoreID = msg->from_peer().store_id();

  auto region = this->peer_->Region();
  auto target = msg->to_peer();

  if (target.id() < this->peer_->PeerId()) {
    return true;
  } else if (target.id() > this->peer_->PeerId()) {
    return true;
  }

  return false;
}

void PeerMsgHandler::HandleStaleMsg(Transport& trans,
                                    raft_serverpb::RaftMessage* msg,
                                    metapb::RegionEpoch* curEpoch,
                                    bool needGC) {
  auto regionID = msg->region_id();
  auto fromPeer = msg->from_peer();
  auto toPeer = msg->to_peer();
  auto msgType = msg->message().msg_type();

  if (!needGC) {
    return;
  }

  std::shared_ptr<raft_serverpb::RaftMessage> gcMsg =
      std::make_shared<raft_serverpb::RaftMessage>();
  gcMsg->set_region_id(regionID);
  gcMsg->set_allocated_from_peer(&fromPeer);
  gcMsg->set_allocated_to_peer(&toPeer);
  gcMsg->set_allocated_region_epoch(curEpoch);
  gcMsg->set_is_tombstone(true);
  trans.Send(gcMsg);
}

void PeerMsgHandler::HandleGCPeerMsg(raft_serverpb::RaftMessage* msg) {}

bool PeerMsgHandler::CheckSnapshot(raft_serverpb::RaftMessage& msg)  // TODO
{}

void PeerMsgHandler::DestoryPeer() {}

metapb::Region* PeerMsgHandler::FindSiblingRegion() {}

void PeerMsgHandler::OnRaftGCLogTick() {}

void PeerMsgHandler::OnSplitRegionCheckTick() {}

void PeerMsgHandler::OnPrepareSplitRegion(metapb::RegionEpoch* regionEpoch,
                                          std::string splitKey, Callback* cb) {}

bool PeerMsgHandler::ValidateSplitRegion(metapb::RegionEpoch* epoch,
                                         std::string splitKey) {}

void PeerMsgHandler::OnApproximateRegionSize(uint64_t size) {}

void PeerMsgHandler::OnSchedulerHeartbeatTick() {}

void PeerMsgHandler::OnGCSnap()  //  TODO:
{}

}  // namespace kvserver
