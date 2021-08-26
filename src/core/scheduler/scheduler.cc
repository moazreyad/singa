/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "singa/core/scheduler.h"

#include <algorithm>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>

#include "singa/core/device.h"
#include "singa/utils/safe_queue.h"

namespace singa {

void Node::AddInEdge(Edge *in_edge) { in_edges_.push_back(in_edge); }

void Node::AddOutEdge(Edge *out_edge) { out_edges_.push_back(out_edge); }

void Edge::SetBlock(Block *blk) { blk_ = blk; }

void Edge::SetSrcNode(Node *src_node) { src_node_ = src_node; }

void Edge::SetDstNode(Node *dst_node) { dst_node_ = dst_node; }

Graph::Graph(Device *device) : device_(device) {}

Graph::~Graph() { Reset(); }

Node *BlkInfo::used_node(const size_t idx) const {
  CHECK_LT(idx, used_nodes_.size());
  return used_nodes_[idx];
}

Node *Graph::node(const size_t idx) const {
  CHECK_LT(idx, nodes_.size());
  return nodes_[idx];
}

Edge *Graph::edge(const size_t idx) const {
  CHECK_LT(idx, edges_.size());
  return edges_[idx];
}

BlkInfo *Graph::block(Block *blk) const {
  auto it = blocks_.find(blk);
  CHECK(it != blocks_.end());
  return it->second;
}

Node *Graph::begin_node(const size_t idx) const {
  CHECK_LT(idx, begin_nodes_.size());
  return begin_nodes_[idx];
}

const NodeVec &Graph::next_nodes(const size_t idx) const {
  CHECK_LT(idx, next_nodes_.size());
  return next_nodes_[idx];
}

const BlockVec &Graph::free_blocks(const size_t idx) const {
  CHECK_LT(idx, free_blocks_.size());
  return free_blocks_[idx];
}

void Graph::Reset() {
  for (auto it : nodes_) {
    delete it;
  }
  nodes_.clear();

  for (auto it : edges_) {
    delete it;
  }
  edges_.clear();

  for (auto it : blocks_) {
    delete it.second;
  }
  blocks_.clear();

  leaf_blocks_.clear();

  iteration_ = 0;

  time_elapsed_ = 0;

  dirty_ = false;

  in_serial_ = false;
}

void Graph::Debug() {
  if (dirty_) Analyze();

  int w = 0;
  size_t max_in_num = 0, max_out_num = 0, max_next_num = 0, max_free_num = 0;
  for (auto &it : nodes_) {
    if (it->op_name_ == "Waiting") continue;
    max_in_num = std::max(max_in_num, it->in_edges_.size());
    max_out_num = std::max(max_out_num, it->out_edges_.size());
  }

  for (auto &it : next_nodes_) {
    max_next_num = std::max(max_next_num, it.size());
  }

  for (auto &it : free_blocks_) {
    max_free_num = std::max(max_free_num, it.size());
  }

  size_t max_size = std::max(nodes_.size(), blocks_.size());
  for (size_t i = max_size; i > 0; i /= 10, ++w) {
  }

  std::stringstream ss;
  ss << "begin nodes:[";
  for (size_t i = 0; i < begin_nodes_.size(); ++i) {
    ss << begin_nodes_[i]->id_ << " ";
  }
  ss << "]" << std::endl;

  size_t size = 0;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    ss << "OP[" << std::setw(w) << i;
    auto node = nodes_[i];

    string name;
    if (node->op_name_.size() > 16) {
      name = node->op_name_.substr(0, 13) + "...";
    } else {
      name = node->op_name_;
    }

    ss << "] Inputs:[";
    size = node->in_edges_.size();
    for (size_t j = 0; j < std::max(max_in_num, size); ++j) {
      if (j < size)
        ss << std::setw(w) << blocks_[node->in_edges_[j]->blk_]->id_ << " ";
      else
        ss << std::setw(w + 1) << " ";
    }

    ss << "] Outputs:[";
    size = node->out_edges_.size();
    for (size_t j = 0; j < std::max(max_out_num, size); ++j) {
      if (j < size)
        ss << std::setw(w) << blocks_[node->out_edges_[j]->blk_]->id_ << " ";
      else
        ss << std::setw(w + 1) << " ";
    }

    ss << "] Next nodes:[";
    size = next_nodes_[i].size();
    for (size_t j = 0; j < max_next_num; ++j) {
      if (j < size)
        ss << std::setw(w) << next_nodes_[i][j]->id_ << " ";
      else
        ss << std::setw(w + 1) << " ";
    }

    ss << "] Free blocks:[";
    size = free_blocks_[i].size();
    for (size_t j = 0; j < max_free_num; ++j) {
      if (j < size)
        ss << std::setw(w) << blocks_[free_blocks_[i][j]]->id_ << " ";
      else
        ss << std::setw(w + 1) << " ";
    }
    ss << "]" << std::endl;
  }

  size_t max_used_num = 0;
  std::vector<BlkInfo *> blkInfos;
  blkInfos.resize(blocks_.size());

  for (auto it : blocks_) {
    blkInfos[it.second->id_] = it.second;
    max_used_num = std::max(max_used_num, it.second->used_nodes_.size());
  }

  for (auto it : blkInfos) {
    auto blkInfo = it;
    ss << "Block[" << std::setw(w) << blkInfo->id_ << "] addr[" << std::setw(10)
       << blkInfo->blk_ << "] size[" << std::setw(10) << blkInfo->blk_->size()
       << "] graph_ref[" << std::setw(w) << blkInfo->graph_ref_
       << "] ref_count[" << std::setw(w) << blkInfo->blk_->ref_count() << "] ";
    switch (blkInfo->type_) {
      case BlockType::kInput:
        ss << "type[input] ";
        break;
      case BlockType::kParam:
        ss << "type[param] ";
        break;
      case BlockType::kInter:
        ss << "type[inter] ";
        break;
      case BlockType::kEnd:
        ss << "type[_end_] ";
        break;
      default:
        break;
    }
    int id = -1;
    if (blkInfo->write_edge_) {
      id = blkInfo->write_edge_->src_node_->id_;
    }
    ss << " write_node[" << std::setw(w) << id;

    ss << "] used_nodes[";
    size = blkInfo->used_nodes_.size();
    for (size_t i = 0; i < max_used_num; ++i) {
      if (i < size)
        ss << std::setw(w) << blkInfo->used_nodes_[i]->id_ << " ";
      else
        ss << std::setw(w + 1) << " ";
    }
    ss << "]" << std::endl;
  }

  printf("%s", ss.str().c_str());
}

void Graph::PrintTimeProfiling() {
  std::stringstream ss;

  // verbosity level: 1 -> forward and backward propagation time
  if (device_->verbosity() == 1) {
    bool forward = true;
    float forward_time = 0;
    float backward_time = 0;
    float time_elapsed;

    for (size_t i = 0; i < nodes_.size(); ++i)
      if (nodes_[i]->time_elapsed() > 0) {
        if (forward == true)
          // check the op of cross entropy backward, after that are backward ops
          // note that the function is more accurate when either
          // SoftmaxCrossEntropy or Softmax is used
          if (nodes_[i]->op_name().find("Backward") != std::string::npos)
            forward = false;
        // when forward becomes false, it starts the backward propagation

        time_elapsed = (nodes_[i]->time_elapsed()) /
                       (iteration_ - device_->skip_iteration());

        if (forward == true) forward_time += time_elapsed;
      }

    backward_time = (time_elapsed_ / (iteration_ - device_->skip_iteration())) -
                    forward_time;

    ss << std::endl << "Time Profiling:" << std::endl;
    ss << "Forward Propagation Time : " << forward_time << " sec" << std::endl;
    ss << "Backward Propagation Time : " << backward_time << " sec"
       << std::endl;
  }

  // verbosity level: 2 -> each operation time (OP_ID, operation name, time)
  if (device_->verbosity() == 2) {
    ss << std::endl << "Time Profiling:" << std::endl;
    for (size_t i = 0; i < nodes_.size(); ++i)
      if (nodes_[i]->time_elapsed() > 0)
        ss << "OP_ID" << nodes_[i]->id_ << ". " << nodes_[i]->op_name() << " : "
           << (nodes_[i]->time_elapsed()) / (iteration_) << " sec" << std::endl;
  }

  // verbosity level: 3 -> Distributed training operations
  if (device_->verbosity() == 3) {
    ss << std::endl << "Time Profiling:" << std::endl;
    for (size_t i = 0; i < nodes_.size(); ++i)
      if ((nodes_[i]->op_name().find("Dist") != std::string::npos) &&
          (nodes_[i]->time_elapsed() > 0))
        ss << "OP_ID" << nodes_[i]->id_ << ". " << nodes_[i]->op_name() << " : "
           << (nodes_[i]->time_elapsed()) / (iteration_) << " sec" << std::endl;
  }

  printf("%s", ss.str().c_str());
}

void Graph::TimeProfilingDoExec(Node *curNode) {
  if ((device_->verbosity() > 0) && (curNode->op_name_ != "Waiting") &&
      (iteration_ >= device_->skip_iteration()))
    device_->TimeProfilingDoExec(std::move(curNode->op_), 0, curNode);
  else
    device_->DoExec(std::move(curNode->op_), 0);
}

void Graph::EvaluateTimeElapsed(const TimePoint &start) {
  if ((device_->verbosity() > 0) && (iteration_ > device_->skip_iteration())) {
    device_->Sync();
    std::chrono::duration<float> duration =
        std::chrono::high_resolution_clock::now() - start;
    time_elapsed_inc(duration.count());
    for (size_t i = 0; i < nodes_.size(); ++i) {
      Node *curNode = nodes_[i];
      if (curNode->op_name_ != "Waiting") {
        device_->EvaluateTimeElapsed(curNode);
      }
    }
  }
}

void Graph::TakeStartTime(TimePoint &start) {
  if ((device_->verbosity() > 0) && (iteration_ >= device_->skip_iteration())) {
    device_->Sync();
    start = std::chrono::high_resolution_clock::now();
  }
}

void Graph::RunGraph() {
  in_serial_ = false;
  if (dirty_) Analyze();

  TimePoint start;
  SafeQueue<Node *> node_queue;

  // activate nodes
  for (auto it : begin_nodes_) {
    node_queue.Push(it);
  }

  TakeStartTime(start);

  // run graph
  while (node_queue.Size()) {
    // step 1: pop the first element, get the node corresponding to the index
    Node *curNode = nullptr;
    node_queue.Pop(curNode);
    int curIndex = curNode->id_;

    // step 2: execute the operation
    TimeProfilingDoExec(curNode);

    // step 3: release some blocks' data that won't be used later
    for (auto it : free_blocks_[curIndex]) {
      it->free_data();
    }

    /*
    if (free_blocks_[curIndex].size()) {
      CBData *cb_data = new CBData(this, curNode);
      cudaStreamAddCallback(device_->ctx_.stream, Graph::Callback, (void
    *)(cb_data), 0);
    }
    */

    // step 4: activate the following nodes
    for (auto it : next_nodes_[curIndex]) {
      node_queue.Push(it);
    }
  }

  // increment iteration counter
  step();
  EvaluateTimeElapsed(start);
}

void Graph::RunInSerial() {
  in_serial_ = true;
  if (dirty_) Analyze();

  TimePoint start;
  TakeStartTime(start);

  for (size_t i = 0; i < nodes_.size(); ++i) {
    Node *curNode = nodes_[i];

    // step 1: execute the operation
    TimeProfilingDoExec(curNode);

    // step 2: release some blocks' data that won't be used later
    for (auto it : free_blocks_[i]) {
      it->free_data();
    }

    /*
    // Wait for calculation to complete and then recyle the data
    CBData *cb_data = new CBData(this, curNode);
    CHECK(cudaStreamAddCallback(device_->ctx_.stream, Graph::Callback, (void
    *)(cb_data), 0));
    */
  }

  // increment iteration counter
  step();
  EvaluateTimeElapsed(start);
}

void Graph::AddOperation(OpFunc &&op, const BlockVec &read_blocks,
                         const BlockVec &write_blocks, string op_name) {
  dirty_ = true;

  // if the size of both read_blocks and write_blocks is zero,
  // this operation is used for synchronization
  if (read_blocks.size() == 0 && write_blocks.size() == 0) {
    AddSyncOp(std::move(op), op_name);
    return;
  }

  // create new node
  Node *node = new Node(nodes_.size(), std::move(op), op_name);

  // create edges for read_blocks
  for (size_t i = 0; i < read_blocks.size(); ++i) {
    Block *blk = read_blocks[i];
    Node *src_node = nullptr;
    BlkInfo *blkInfo = nullptr;

    // update leaf blocks
    auto iter = leaf_blocks_.find(blk);
    if (iter != leaf_blocks_.end()) {
      leaf_blocks_.erase(iter);
    }

    // check if the block is already in the computational graph
    auto it = blocks_.find(blk);
    if (it == blocks_.end()) {
      blkInfo = new BlkInfo(blocks_.size(), blk, BlockType::kInput);
      blocks_[blk] = blkInfo;
    } else {
      blkInfo = it->second;
      if (blkInfo->type_ == BlockType::kEnd) {
        blkInfo->type_ = BlockType::kInter;
      }

      // update the existing edge, update dst node and create new edge
      Edge *write_edge = blkInfo->write_edge_;
      if (write_edge) {
        if (!write_edge->dst_node_) {
          // change the dst node of the write_edge
          write_edge->dst_node_ = node;
          node->AddInEdge(write_edge);
          blkInfo->graph_ref_ += 1;
          continue;
        } else {
          src_node = write_edge->src_node_;
        }
      }
    }

    // create new edge for new block
    Edge *edge = new Edge(edges_.size(), blk, src_node, node);
    blkInfo->graph_ref_ += 1;
    if (src_node) {
      src_node->AddOutEdge(edge);
    }

    node->AddInEdge(edge);
    edges_.push_back(edge);
  }

  // update last node for write_blocks
  for (size_t i = 0; i < write_blocks.size(); ++i) {
    Block *blk = write_blocks[i];
    BlkInfo *blkInfo = nullptr;

    // update leaf blocks
    leaf_blocks_.insert(blk);

    auto it = blocks_.find(blk);
    if (it == blocks_.end()) {
      blkInfo = new BlkInfo(blocks_.size(), blk, BlockType::kEnd);
      blocks_[blk] = blkInfo;
    } else {
      blkInfo = it->second;
      if (blkInfo->type_ == BlockType::kInput) {
        blkInfo->type_ = BlockType::kParam;
      }

      Edge *write_edge = blkInfo->write_edge_;
      if (write_edge) {
        if (!write_edge->dst_node_) {
          write_edge->dst_node_ = node;
          node->AddInEdge(write_edge);
        } else {
          Node *lastNode = write_edge->src_node_;
          auto outEdges = lastNode->out_edges();
          for (auto outEdge : outEdges) {
            if (outEdge->blk_ == blk && outEdge->dst_node_ != node) {
              Edge *edge =
                  new Edge(edges_.size(), blk, outEdge->dst_node_, node);
              outEdge->dst_node_->AddOutEdge(edge);
              node->AddInEdge(edge);
            }
          }
        }
      }
    }

    // create new edge for new block
    Edge *edge = new Edge(edges_.size(), blk, node, nullptr);
    blkInfo->write_edge_ = edge;
    blkInfo->graph_ref_ += 1;

    node->AddOutEdge(edge);
    edges_.push_back(edge);
  }

  // add node into nodes
  nodes_.push_back(node);
}

void Graph::AddSyncOp(function<void(Context *)> &&op, string op_name) {
  // create new node
  Node *node = new Node(nodes_.size(), std::move(op), op_name);

  for (auto it : leaf_blocks_) {
    Block *blk = it;
    BlkInfo *blkInfo = blocks_[blk];
    Edge *edge = nullptr;

    if (blkInfo->type_ == BlockType::kEnd) {
      blkInfo->type_ = BlockType::kInter;
    }

    Edge *write_edge = blkInfo->write_edge_;
    if (!write_edge->dst_node_) {
      // change the dst node of the write_edge
      write_edge->dst_node_ = node;
      edge = write_edge;
    } else {
      Node *src_node = write_edge->src_node_;
      edge = new Edge(edges_.size(), blk, src_node, node);
      src_node->AddOutEdge(edge);
      edges_.push_back(edge);
    }

    node->AddInEdge(edge);

    // fake edges, no need to add the graph ref
    edge = new Edge(edges_.size(), blk, node, nullptr);
    blkInfo->write_edge_ = edge;

    node->AddOutEdge(edge);
    edges_.push_back(edge);
  }

  // add node into nodes
  nodes_.push_back(node);
}

void Graph::Analyze() {
  begin_nodes_.clear();
  next_nodes_.clear();
  next_nodes_.resize(nodes_.size());
  free_blocks_.clear();
  free_blocks_.resize(nodes_.size());

  for (auto &it : blocks_) {
    it.second->used_nodes_.clear();
  }

  AnalyzeNodes();

  AnalyzeEdges();

  dirty_ = false;

  // Debug();
}

void Graph::AnalyzeNodes() {
  if (in_serial_) {
    begin_nodes_.push_back(nodes_[0]);

    for (size_t i = 0; i < nodes_.size(); ++i) {
      Node *curNode = nodes_[i];

      next_nodes_[i].clear();
      if (i + 1 < nodes_.size()) {
        next_nodes_[i].push_back(nodes_[i + 1]);
      }

      BlockSet blks;
      for (size_t j = 0; j < curNode->in_edges_.size(); ++j) {
        blks.insert(curNode->in_edges_[j]->blk_);
      }
      for (size_t j = 0; j < curNode->out_edges_.size(); ++j) {
        blks.insert(curNode->out_edges_[j]->blk_);
      }

      for (auto &it : blks) {
        blocks_[it]->used_nodes_.push_back(curNode);
      }
    }
  } else {
    // init node ref
    std::vector<int> node_ref_;
    node_ref_.resize(nodes_.size());
    for (size_t i = 0; i < nodes_.size(); ++i) {
      node_ref_[i] = nodes_[i]->in_edges_.size();
    }

    // find all input edges and decrease ref count of nodes
    for (size_t i = 0; i < edges_.size(); ++i) {
      Node *src_node = edges_[i]->src_node_;
      if (!src_node) {
        Node *node = edges_[i]->dst_node_;
        int nodeId = node->id_;
        node_ref_[nodeId] -= 1;
      }
    }

    // activate nodes
    SafeQueue<Node *> node_queue;
    for (size_t i = 0; i < node_ref_.size(); ++i) {
      if (node_ref_[i] == 0) {
        begin_nodes_.push_back(nodes_[i]);
        node_queue.Push(nodes_[i]);
      }
    }

    // run graph
    while (node_queue.Size()) {
      // step 1: pop the first element, get the node corresponding to the index
      Node *curNode = nullptr;
      node_queue.Pop(curNode);
      int curIndex = curNode->id_;

      // step 2: decrease ref count of nodes and activate nodes
      next_nodes_[curIndex].clear();
      for (size_t i = 0; i < curNode->out_edges_.size(); ++i) {
        Edge *edge = curNode->out_edges_[i];
        Node *nextNode = edge->dst_node_;

        if (!nextNode) {
          continue;
        }

        int nodeId = nextNode->id_;
        node_ref_[nodeId] -= 1;
        if (node_ref_[nodeId] <= 0) {
          node_queue.Push(nextNode);
          next_nodes_[curIndex].push_back(nextNode);
        }
      }

      // step 3: push_back curNode to the used_nodes_ of relevant blocks
      BlockSet blks;
      for (size_t j = 0; j < curNode->in_edges_.size(); ++j) {
        blks.insert(curNode->in_edges_[j]->blk_);
      }
      for (size_t j = 0; j < curNode->out_edges_.size(); ++j) {
        blks.insert(curNode->out_edges_[j]->blk_);
      }

      for (auto &it : blks) {
        blocks_[it]->used_nodes_.push_back(curNode);
      }
    }
  }
}

void Graph::AnalyzeEdges() {
  for (auto &it : blocks_) {
    Block *blk = it.first;
    BlkInfo *blkInfo = it.second;

    if (blkInfo->used_nodes_.size()) {
      int node_id = blkInfo->used_nodes_.back()->id_;
      BlockType type = blkInfo->type_;

      // if the block belongs to a inter tensor
      // and isn't refered on the Python Side
      if ((type == BlockType::kInter || type == BlockType::kEnd) &&
          blkInfo->graph_ref_ >= blk->ref_count()) {
        free_blocks_[node_id].push_back(blk);
      }
    }
  }
}

void Graph::FreeLoop() {
  int id = 0;
  for (;;) {
    free_queue_.Pop(id);
    if (id == -1) {
      break;
    } else {
      for (auto it : free_blocks_[id]) {
        it->free_data();
      }
    }
  }
}

/*
void CUDART_CB Graph::Callback(cudaStream_t stream, cudaError_t status,
                               void *data) {
  CBData *cb_data = (CBData *)data;
  Graph *graph = cb_data->graph_;
  graph->free_queue_.Push(cb_data->node_->id_);
  delete cb_data;
}
*/

}  // namespace singa
