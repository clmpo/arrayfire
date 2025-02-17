/*******************************************************
 * Copyright (c) 2014, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#include <Array.hpp>
#include <Kernel.hpp>
#include <common/dispatch.hpp>
#include <common/half.hpp>
#include <common/jit/ModdimNode.hpp>
#include <common/jit/Node.hpp>
#include <common/jit/NodeIterator.hpp>
#include <common/kernel_cache.hpp>
#include <common/util.hpp>
#include <copy.hpp>
#include <debug_cuda.hpp>
#include <device_manager.hpp>
#include <err_cuda.hpp>
#include <kernel_headers/jit_cuh.hpp>
#include <math.hpp>
#include <platform.hpp>
#include <af/dim4.hpp>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using common::findModule;
using common::getFuncName;
using common::half;
using common::Node;
using common::Node_ids;
using common::Node_map_t;

using std::string;
using std::stringstream;
using std::to_string;
using std::vector;

namespace cuda {

static string getKernelString(const string &funcName,
                              const vector<Node *> &full_nodes,
                              const vector<Node_ids> &full_ids,
                              const vector<int> &output_ids, bool is_linear) {
    const std::string includeFileStr(jit_cuh, jit_cuh_len);

    const std::string paramTStr = R"JIT(
template<typename T>
struct Param {
    dim_t dims[4];
    dim_t strides[4];
    T *ptr;
};
)JIT";

    std::string typedefStr = "typedef unsigned int uint;\n";
    typedefStr += "typedef ";
    typedefStr += getFullName<dim_t>();
    typedefStr += " dim_t;\n";

    // Common CUDA code
    // This part of the code does not change with the kernel.

    static const char *kernelVoid = "extern \"C\" __global__ void\n";
    static const char *dimParams =
        "uint blocks_x, uint blocks_y, uint blocks_x_total, uint num_odims";

    static const char *loopStart = R"JIT(
    for (int blockIdx_x = blockIdx.x; blockIdx_x < blocks_x_total; blockIdx_x += gridDim.x) {
    )JIT";
    static const char *loopEnd   = "}\n\n";

    static const char *blockStart = "{\n\n";
    static const char *blockEnd   = "\n\n}";

    static const char *linearIndex = R"JIT(
        uint threadId = threadIdx.x;
        long long idx = blockIdx_x * blockDim.x * blockDim.y + threadId;
        if (idx >= outref.dims[3] * outref.strides[3]) return;
        )JIT";

    static const char *generalIndex = R"JIT(
        long long id0 = 0, id1 = 0, id2 = 0, id3 = 0;
        long blockIdx_y = blockIdx.z * gridDim.y + blockIdx.y;
        if (num_odims > 2) {
            id2 = blockIdx_x / blocks_x;
            id0 = blockIdx_x - id2 * blocks_x;
            id0 = threadIdx.x + id0 * blockDim.x;
            if (num_odims > 3) {
                id3 = blockIdx_y / blocks_y;
                id1 = blockIdx_y - id3 * blocks_y;
                id1 = threadIdx.y + id1 * blockDim.y;
            } else {
                id1 = threadIdx.y + blockDim.y * blockIdx_y;
            }
        } else {
            id3 = 0;
            id2 = 0;
            id1 = threadIdx.y + blockDim.y * blockIdx_y;
            id0 = threadIdx.x + blockDim.x * blockIdx_x;
        }

        bool cond = id0 < outref.dims[0] &&
                    id1 < outref.dims[1] &&
                    id2 < outref.dims[2] &&
                    id3 < outref.dims[3];

        if (!cond) { continue; }

        long long idx = outref.strides[3] * id3 +
                        outref.strides[2] * id2 +
                        outref.strides[1] * id1 + id0;
        )JIT";

    stringstream inParamStream;
    stringstream outParamStream;
    stringstream outWriteStream;
    stringstream offsetsStream;
    stringstream opsStream;
    stringstream outrefstream;

    for (int i = 0; i < static_cast<int>(full_nodes.size()); i++) {
        const auto &node     = full_nodes[i];
        const auto &ids_curr = full_ids[i];
        // Generate input parameters, only needs current id
        node->genParams(inParamStream, ids_curr.id, is_linear);
        // Generate input offsets, only needs current id
        node->genOffsets(offsetsStream, ids_curr.id, is_linear);
        // Generate the core function body, needs children ids as well
        node->genFuncs(opsStream, ids_curr);
    }

    outrefstream << "const Param<" << full_nodes[output_ids[0]]->getTypeStr()
                 << "> &outref = out" << output_ids[0] << ";\n";

    for (int id : output_ids) {
        // Generate output parameters
        outParamStream << "Param<" << full_nodes[id]->getTypeStr() << "> out"
                       << id << ", \n";
        // Generate code to write the output
        outWriteStream << "out" << id << ".ptr[idx] = val" << id << ";\n";
    }

    // Put various blocks into a single stream
    stringstream kerStream;
    kerStream << typedefStr;
    kerStream << includeFileStr << "\n\n";
    kerStream << paramTStr << "\n";
    kerStream << kernelVoid;
    kerStream << funcName;
    kerStream << "(\n";
    kerStream << inParamStream.str();
    kerStream << outParamStream.str();
    kerStream << dimParams;
    kerStream << ")\n";
    kerStream << blockStart;
    kerStream << outrefstream.str();
    kerStream << loopStart;
    if (is_linear) {
        kerStream << linearIndex;
    } else {
        kerStream << generalIndex;
    }
    kerStream << offsetsStream.str();
    kerStream << opsStream.str();
    kerStream << outWriteStream.str();
    kerStream << loopEnd;
    kerStream << blockEnd;

    return kerStream.str();
}

static CUfunction getKernel(const vector<Node *> &output_nodes,
                            const vector<int> &output_ids,
                            const vector<Node *> &full_nodes,
                            const vector<Node_ids> &full_ids,
                            const bool is_linear) {
    const string funcName =
        getFuncName(output_nodes, full_nodes, full_ids, is_linear);
    const size_t moduleKey = deterministicHash(funcName);

    // A forward lookup in module cache helps avoid recompiling the jit
    // source generated from identical jit-trees. It also enables us
    // with a way to save jit kernels to disk only once
    auto entry = findModule(getActiveDeviceId(), moduleKey);

    if (entry.get() == nullptr) {
        const string jitKer = getKernelString(funcName, full_nodes, full_ids,
                                              output_ids, is_linear);
        saveKernel(funcName, jitKer, ".cu");

        common::Source jit_src{jitKer.c_str(), jitKer.size(),
                               deterministicHash(jitKer)};

        return common::getKernel(funcName, {jit_src}, {}, {}, true).get();
    }
    return common::getKernel(entry, funcName, true).get();
}

template<typename T>
void evalNodes(vector<Param<T>> &outputs, const vector<Node *> &output_nodes) {
    size_t num_outputs = outputs.size();
    if (num_outputs == 0) { return; }

    int device         = getActiveDeviceId();
    dim_t *outDims     = outputs[0].dims;
    size_t numOutElems = outDims[0] * outDims[1] * outDims[2] * outDims[3];
    if (numOutElems == 0) { return; }

    // Use thread local to reuse the memory every time you are here.
    thread_local Node_map_t nodes;
    thread_local vector<Node *> full_nodes;
    thread_local vector<Node_ids> full_ids;
    thread_local vector<int> output_ids;

    // Reserve some space to improve performance at smaller sizes
    if (nodes.empty()) {
        nodes.reserve(1024);
        output_ids.reserve(output_nodes.size());
        full_nodes.reserve(1024);
        full_ids.reserve(1024);
    }

    for (auto &node : output_nodes) {
        int id = node->getNodesMap(nodes, full_nodes, full_ids);
        output_ids.push_back(id);
    }

    using common::ModdimNode;
    using common::NodeIterator;
    using jit::BufferNode;

    // find all moddims in the tree
    vector<std::shared_ptr<Node>> node_clones;
    for (auto *node : full_nodes) { node_clones.emplace_back(node->clone()); }

    for (common::Node_ids ids : full_ids) {
        auto &children = node_clones[ids.id]->m_children;
        for (int i = 0; i < Node::kMaxChildren && children[i] != nullptr; i++) {
            children[i] = node_clones[ids.child_ids[i]];
        }
    }

    for (auto &node : node_clones) {
        if (node->getOp() == af_moddims_t) {
            ModdimNode *mn = static_cast<ModdimNode *>(node.get());
            auto isBuffer  = [](const Node &ptr) { return ptr.isBuffer(); };

            NodeIterator<> it(node.get());
            auto new_strides = calcStrides(mn->m_new_shape);
            while (it != NodeIterator<>()) {
                it = find_if(it, NodeIterator<>(), isBuffer);
                if (it == NodeIterator<>()) { break; }

                BufferNode<T> *buf = static_cast<BufferNode<T> *>(&(*it));

                buf->m_param.dims[0]    = mn->m_new_shape[0];
                buf->m_param.dims[1]    = mn->m_new_shape[1];
                buf->m_param.dims[2]    = mn->m_new_shape[2];
                buf->m_param.dims[3]    = mn->m_new_shape[3];
                buf->m_param.strides[0] = new_strides[0];
                buf->m_param.strides[1] = new_strides[1];
                buf->m_param.strides[2] = new_strides[2];
                buf->m_param.strides[3] = new_strides[3];

                ++it;
            }
        }
    }

    full_nodes.clear();
    for (auto &node : node_clones) { full_nodes.push_back(node.get()); }

    bool is_linear = true;
    for (auto *node : full_nodes) {
        is_linear &= node->isLinear(outputs[0].dims);
    }

    CUfunction ker =
        getKernel(output_nodes, output_ids, full_nodes, full_ids, is_linear);

    int threads_x = 1, threads_y = 1;
    int blocks_x_ = 1, blocks_y_ = 1;
    int blocks_x = 1, blocks_y = 1, blocks_z = 1, blocks_x_total;

    cudaDeviceProp properties    = getDeviceProp(device);
    const long long max_blocks_x = properties.maxGridSize[0];
    const long long max_blocks_y = properties.maxGridSize[1];

    int num_odims = 4;
    while (num_odims >= 1) {
        if (outDims[num_odims - 1] == 1) {
            num_odims--;
        } else {
            break;
        }
    }

    if (is_linear) {
        threads_x = 256;
        threads_y = 1;

        blocks_x_total = divup(
            (outDims[0] * outDims[1] * outDims[2] * outDims[3]), threads_x);

        int repeat_x = divup(blocks_x_total, max_blocks_x);
        blocks_x     = divup(blocks_x_total, repeat_x);
    } else {
        threads_x = 32;
        threads_y = 8;

        blocks_x_ = divup(outDims[0], threads_x);
        blocks_y_ = divup(outDims[1], threads_y);

        blocks_x = blocks_x_ * outDims[2];
        blocks_y = blocks_y_ * outDims[3];

        blocks_z = divup(blocks_y, max_blocks_y);
        blocks_y = divup(blocks_y, blocks_z);

        blocks_x_total = blocks_x;
        int repeat_x   = divup(blocks_x_total, max_blocks_x);
        blocks_x       = divup(blocks_x_total, repeat_x);
    }

    vector<void *> args;

    for (const auto &node : full_nodes) {
        node->setArgs(0, is_linear,
                      [&](int /*id*/, const void *ptr, size_t /*size*/) {
                          args.push_back(const_cast<void *>(ptr));
                      });
    }

    for (size_t i = 0; i < num_outputs; i++) {
        args.push_back(static_cast<void *>(&outputs[i]));
    }

    args.push_back(static_cast<void *>(&blocks_x_));
    args.push_back(static_cast<void *>(&blocks_y_));
    args.push_back(static_cast<void *>(&blocks_x_total));
    args.push_back(static_cast<void *>(&num_odims));

    {
        using namespace cuda::kernel_logger;
        AF_TRACE("Launching : Blocks: [{}] Threads: [{}] ",
                 dim3(blocks_x, blocks_y, blocks_z),
                 dim3(threads_x, threads_y));
    }
    CU_CHECK(cuLaunchKernel(ker, blocks_x, blocks_y, blocks_z, threads_x,
                            threads_y, 1, 0, getActiveStream(), args.data(),
                            NULL));

    // Reset the thread local vectors
    nodes.clear();
    output_ids.clear();
    full_nodes.clear();
    full_ids.clear();
}

template<typename T>
void evalNodes(Param<T> out, Node *node) {
    vector<Param<T>> outputs;
    vector<Node *> output_nodes;

    outputs.push_back(out);
    output_nodes.push_back(node);
    evalNodes(outputs, output_nodes);
}

template void evalNodes<float>(Param<float> out, Node *node);
template void evalNodes<double>(Param<double> out, Node *node);
template void evalNodes<cfloat>(Param<cfloat> out, Node *node);
template void evalNodes<cdouble>(Param<cdouble> out, Node *node);
template void evalNodes<int>(Param<int> out, Node *node);
template void evalNodes<uint>(Param<uint> out, Node *node);
template void evalNodes<char>(Param<char> out, Node *node);
template void evalNodes<uchar>(Param<uchar> out, Node *node);
template void evalNodes<intl>(Param<intl> out, Node *node);
template void evalNodes<uintl>(Param<uintl> out, Node *node);
template void evalNodes<short>(Param<short> out, Node *node);
template void evalNodes<ushort>(Param<ushort> out, Node *node);
template void evalNodes<half>(Param<half> out, Node *node);

template void evalNodes<float>(vector<Param<float>> &out,
                               const vector<Node *> &node);
template void evalNodes<double>(vector<Param<double>> &out,
                                const vector<Node *> &node);
template void evalNodes<cfloat>(vector<Param<cfloat>> &out,
                                const vector<Node *> &node);
template void evalNodes<cdouble>(vector<Param<cdouble>> &out,
                                 const vector<Node *> &node);
template void evalNodes<int>(vector<Param<int>> &out,
                             const vector<Node *> &node);
template void evalNodes<uint>(vector<Param<uint>> &out,
                              const vector<Node *> &node);
template void evalNodes<char>(vector<Param<char>> &out,
                              const vector<Node *> &node);
template void evalNodes<uchar>(vector<Param<uchar>> &out,
                               const vector<Node *> &node);
template void evalNodes<intl>(vector<Param<intl>> &out,
                              const vector<Node *> &node);
template void evalNodes<uintl>(vector<Param<uintl>> &out,
                               const vector<Node *> &node);
template void evalNodes<short>(vector<Param<short>> &out,
                               const vector<Node *> &node);
template void evalNodes<ushort>(vector<Param<ushort>> &out,
                                const vector<Node *> &node);
template void evalNodes<half>(vector<Param<half>> &out,
                              const vector<Node *> &node);
}  // namespace cuda
