#pragma once

#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include "proto/onnx/core/common/common.h"

// #include "gsl/pointers"
#include "gsl/gsl_util"

#include "proto/onnx/core/common/status.h"
#include "proto/onnx/core/common/const_pointer_container.h"
#include "proto/onnx/core/graph/constants.h"
#include "proto/onnx/core/graph/graph_nodes.h"
#include "proto/onnx/onnx/defs/schema.h"
#include "proto/onnx/core/graph/utils.h"
#include "proto/onnx/onnx/onnx_pb.h"

using namespace onnx;

// TODO - Evaluate switching the types below to support transparent comparators and enable
// lookups based on gsl::cstring_span<> and std::string_view.  This would reduces allocations
// converting to std::string, but requires conversion to std::map<std::string, foo, std::less<>>
// instead of std::unordered_map<std::string, foo, [std::less<foo>]>.

typedef std::unordered_map<std::string, AttributeProto> NodeAttributes;

namespace ONNXIR
{
typedef size_t NodeIndex;
typedef int64_t Version;
typedef ValueInfoProto NodeArgInfo;
typedef std::unordered_map<std::string, const TensorProto*> InitializedTensorSet;
typedef std::unordered_map<std::string, TypeProto> ArgNameToTypeMap;
typedef const std::string& ProviderType;

class Graph;
class GraphBase;
class Node;
class OpSignature;
class LotusOpSchemaRegistry;

namespace Test
{
struct NodeTestHelper;
}

// Node argument definition, for both input and output,
// including arg name, arg type (contains both type and shape).
//
// Design Question: in my (Ke's) opinion, shape should not be part of type.
// We may align the protobuf design with our operator registry interface,
// which has type specified for each operator, but no shape. Well, shape
// should be inferred with a separate shape inference function given
// input shapes, or input tensor data sometimes.
// With shape as part of type (current protobuf design),
// 1) we'll have to split the "TypeProto" into type and shape in this internal
// representation interface so that it could be easily used when doing type
// inference and matching with operator registry.
// 2) SetType should be always called before SetShape, otherwise, SetShape()
// will fail. Because shape is located in a TypeProto.
// Thoughts?
//
class NodeArg
{
public:
    // Constructor by specifying node arg name and type&shape which is
    // optional. This is called when loading a <Graph> from <GraphProto>
    // normally.
    NodeArg(const std::string& name,
            const TypeProto* p_arg_type);

    NodeArg(NodeArg&& other) = default;

    // Get node arg name.
    const std::string& Name() const noexcept;

    // Get node arg type.
    const DataType Type() const noexcept;
    const TypeProto* TypeAsProto() const noexcept;

    // Get node arg shape.
    // Return null pointer if there's no shape specified.
    const TensorShapeProto* Shape() const;

    // Set node arg shape.
    // Shape could only be set after setting type since shape information
    // now is part of TypeProto.
    void SetShape(const TensorShapeProto& shape);

    // Get node arg info proto.
    const NodeArgInfo& ToProto() const noexcept
    {
        return node_arg_info_;
    }

    // Indicates whether <*this> node arg exists or not.
    // Optional inputs are allowed in ONNX. Empty arg name represents
    // a non-existing input argument.
    bool Exists() const noexcept;

private:
    LOTUS_DISALLOW_COPY_AND_ASSIGN(NodeArg);
    friend class Graph;

    void SetType(DataType p_type);
    void SetType(const TypeProto& type_proto);

    NodeArg& operator=(NodeArg&& other) = delete;

    // Node arg PType.
    DataType type_;

    // Node arg name, type and shape.
    NodeArgInfo node_arg_info_;

    // Flag indicates whether <*this> node arg exists or not.
    bool exists_;
};

// A node representation class.
class Node
{
public:
    ~Node() = default;

    // An edge end. It could be input or output edge end of a node.
    // For node's input edge end, it's the source end, as the destination
    // end is the node itself.
    // For node's ouput edge end, it's the destination end, as the source
    // end is the node itself.
    class EdgeEnd
    {
    public:
        // Constructor.
        // An EdgeEnd contains a Node and NodeArg.
        // TODO: Can these values ever be null?
        EdgeEnd(const Node& node, const NodeArg& node_arg) noexcept;

        // Get the <Node*> that this edge end refers to.
        const Node& GetNode() const noexcept;

        // Get the <NodeArg*> that this edge end refers to.
        const NodeArg& GetNodeArg() const noexcept;

    private:
        const Node* node_;
        const NodeArg* node_arg_;
    };

    // Get node index.
    NodeIndex Index() const noexcept;

    // Get node name.
    const std::string& Name() const noexcept;

    // Get node operator type.
    const std::string& OpType() const noexcept;

    // Get the domain of the OperatorSet that specifies the operator named by <op_type_>.
    const std::string& Domain() const noexcept;

    // Get the OperatorSchema this node refers to. ValidateOpType() must have been called previously.
    // May be null in the future.
    const OpSchema* Op() const noexcept;

    // Get node description.
    const std::string& Description() const noexcept;

    // read only access. requires special wrapper to apply const to the NodeArg
    const ConstPointerContainer<std::vector<NodeArg*>> InputDefs() const noexcept
    {
        return ConstPointerContainer<std::vector<NodeArg*>>(definitions_.input_defs);
    }

    const std::vector<int>& InputArgCount() const noexcept
    {
        return definitions_.input_arg_count;
    }

    // read only access. requires special wrapper to apply const to the NodeArg
    const ConstPointerContainer<std::vector<NodeArg*>> OutputDefs() const noexcept
    {
        return ConstPointerContainer<std::vector<NodeArg*>>(definitions_.output_defs);
    }

    // CNTK_TODO: need a way to edit a graph - e.g. to insert a node.
    std::vector<NodeArg*>& Mutable_OutputDefs()
    {
        return definitions_.output_defs;
    }

    using NodeConstIterator = std::set<const Node*>::const_iterator;

    // Functions defined to traverse a Graph as below.
    // Read all input nodes of <*this>.

    // Beginning of input nodes. Iterator should have no nullptr values.
    NodeConstIterator InputNodesBegin() const noexcept
    {
        return relationships_.input_nodes.cbegin();
    };
    // End of input nodes.
    NodeConstIterator InputNodesEnd() const noexcept
    {
        return relationships_.input_nodes.cend();
    }

    // Beginning of output nodes. Iterator should have no nullptr values.
    NodeConstIterator OutputNodesBegin() const noexcept
    {
        return relationships_.output_nodes.cbegin();
    }
    // End of output nodes.
    NodeConstIterator OutputNodesEnd() const noexcept
    {
        return relationships_.output_nodes.cend();
    }

    const std::set<std::string>& ControlInputs() const noexcept
    {
        return relationships_.control_inputs;
    }

    // Add a node attribute with specified attribute name and value.
    void AddAttribute(const std::string& attr_name, const AttributeProto& value);

#define ADD_ATTR_INTERFACES(TypeName)               \
    void AddAttribute(const std::string& attr_name, \
                      const TypeName& value);       \
    void AddAttribute(const std::string& attr_name, \
                      const std::vector<TypeName>& values);

    ADD_ATTR_INTERFACES(int64_t)
    ADD_ATTR_INTERFACES(float)
    ADD_ATTR_INTERFACES(std::string)
    ADD_ATTR_INTERFACES(TensorProto)
    ADD_ATTR_INTERFACES(GraphProto)

    // Clear specified node attribute.
    bool ClearAttribute(const std::string& attr_name);

    // Get node attributes.
    const NodeAttributes& GetAttributes() const noexcept;

    // Indicates on which we will run this node in runtime.
    // Executor will decide which device that this node will run against
    // and set it properly.
    // TODO: may change the return value type to be an ENUM.
    ProviderType GetExecutionProviderType() const noexcept;
    void SetExecutionProviderType(ProviderType execution_provider_type);

    // Get the corresponding <NodeProto>.
    void ToProto(NodeProto& proto) const;

    // iterate through all input/output defs
    void ForEachDef(std::function<void(const ONNXIR::NodeArg*, bool is_input)> func) const;

    // iterate through all input defs
    void ForEachInputDef(std::function<void(const ONNXIR::NodeArg*)> func) const;

    // iterate through all output defs
    void ForEachOutputDef(std::function<void(const ONNXIR::NodeArg*)> func) const;

    // Replaces defs
    void ReplaceDefs(const std::map<ONNXIR::NodeArg*, ONNXIR::NodeArg*>& replacements);

    // Node definitions. Really a struct but we want to prevent accidental copies.
    class Definitions
    {
    public:
        Definitions() noexcept {}

        // Node inputs' definition.
        std::vector<NodeArg*> input_defs;
        // The number of inputs for each argument of the operator or function which
        // this node refers.
        // For example, <input_defs_> has 10 elements (inputs), and <input_arg_count_>
        // is {4, 6}. This means that 4 elements (inputs) of <input_defs_> map to the
        // first argument of the operator or function, and the other 6 map to the
        // second argument.
        std::vector<int> input_arg_count;

        // Node outputs' definition.
        std::vector<NodeArg*> output_defs;

    private:
        LOTUS_DISALLOW_COPY_ASSIGN_AND_MOVE(Definitions);
    };

    class Relationships
    {
    public:
        Relationships() noexcept {}

        // Node input edges.
        std::set<EdgeEnd*> input_edges;
        // Node output edges.
        std::set<EdgeEnd*> output_edges;

        // Node input nodes, besides input nodes mentioned in <inputs_> above,
        // it also contains all control input nodes;
        std::set<const Node*> input_nodes;
        // Control input nodes' names.
        std::set<std::string> control_inputs;
        // Node's output nodes.
        std::set<const Node*> output_nodes;

    private:
        LOTUS_DISALLOW_COPY_ASSIGN_AND_MOVE(Relationships);
    };

private:
    LOTUS_DISALLOW_COPY_ASSIGN_AND_MOVE(Node);

    // NOTE: These friendship relationships should ONLY be used for calling the following methods
    // so that the Node can maintain its internal invariants as well as possible.
    // Node::Node
    // Node::Init
    // Node::MutableDefinitions
    // Node::MutableRelationships
    // Node::ValdiateVersion
    // All other calls should be made through the public Node interface.
    // Friend classes should NOT be directly accessing any member variables.
    friend class GraphBase;
    friend class Graph;
    friend struct Test::NodeTestHelper;

    Node(NodeIndex index, GraphBase& graph)
        : index_(index),
          graph_(&graph) {}

    void Init(const std::string& name,
              const std::string& op_type,
              const std::string& description,
              const std::vector<NodeArg*>& input_args,
              const std::vector<NodeArg*>& output_args,
              const NodeAttributes* attributes,
              const std::string& domain);

    // internal only method to allow selected classes to directly alter
    // the input/output definitions and arg counts
    Definitions& MutableDefinitions() noexcept;

    // internal only method to allow selected classes to directly alter
    // the links between nodes.
    Relationships& MutableRelationships() noexcept;

    const Definitions& GetDefinitions() const noexcept
    {
        return definitions_;
    }
    const Relationships& GetRelationships() const noexcept
    {
        return relationships_;
    }

    // validate and update the input arg count
    Status UpdateInputArgCount();

    // Node index. Default to impossible value rather than 0.
    NodeIndex index_ = std::numeric_limits<NodeIndex>::max();

    // Node name.
    std::string name_;

    // Node operator type.
    std::string op_type_;

    // OperatorSet domain of <op_type_).
    std::string domain_;

    // OperatorSchema that <*this> node refers to.
    const OpSchema* op_ = nullptr;

    // Node doc string.
    std::string description_;

    // input/output defs and arg count
    Definitions definitions_;

    // Relationships between this node and others in the graph
    Relationships relationships_;

    // Device.
    std::string execution_provider_type_;

    // Map from attribute name to attribute.
    // This allows attribute adding and removing.
    NodeAttributes attributes_;

    GraphBase* graph_;
};

// TODO: Graph base class.
// It should cover the common things between function and graph.
// Move these common things from Graph to GraphBase.
// 1. Graph does not have attributes, while function has.
// 2. Graph does have initializers, while function does not.
// 3. Graph does have value_info, while function does not.
class GraphBase
{
public:
    // Resolve <*this> graph to ensure it's in a good shape with full
    // functionality.
    // 1. Run through all validation rules.
    //    a. Node name and node output's names should be unique.
    //    b. Attribute match between node and op definition.
    //    c. Input/Output match between node and op definition.
    //    d. Graph is acyclic and sort nodes in topological order.
    // 2. Check & Setup inner nodes' dependency.
    // 3. Cleanup function definition lists.
    // Returns resolving status.
    virtual Status Resolve() = 0;

    // Getter and Setter for graph name.
    virtual const std::string& Name() const noexcept = 0;
    virtual void SetName(const std::string& name) = 0;

    virtual const std::string& Description() const noexcept = 0;
    virtual void SetDescription(const std::string& description) = 0;

    // Graph inputs. Should have no nullptr values.
    const std::vector<const NodeArg*>& GetInputs() const noexcept
    {
        return graph_inputs_;
    }

    // Graph outputs. Should have no nullptr values.
    const std::vector<const NodeArg*>& GetOutputs() const noexcept
    {
        return graph_outputs_;
    }

    // Get const Node given specific node index. May return nullptr if node as been freed.
    const Node* GetNode(NodeIndex node_index) const
    {
        return NodeAtIndexImpl(node_index);
    }

    // Mutable node at index. May return nullptr if node has been freed.
    Node* GetNode(NodeIndex node_index)
    {
        return NodeAtIndexImpl(node_index);
    }

    GraphNodes& Nodes() noexcept
    {
        return iterable_nodes_;
    }

    const GraphNodes& Nodes() const noexcept
    {
        return iterable_nodes_;
    }

    // Max NodeIndex in the Graph
    int MaxNodeIndex() const noexcept
    {
        return gsl::narrow_cast<int>(nodes_.size());
    }

    // Number of nodes in the <Graph>.
    // This is smaller than MaxNodeIndex(), since there may be nodes
    // removed during optimization.
    int NumberOfNodes() const noexcept
    {
        return num_of_nodes_;
    }

    // Create NodeArg owned by the graph
    NodeArg& CreateOwnedNodeArg(const std::string& name, const TypeProto* p_arg_type)
    {
        if (p_arg_type != nullptr)
        {
            for (auto& a : owned_node_args_)
            {
                if (a->Name() == name)
                {
                    //const TensorShapeProto* shape = a->Shape();
                    //if (p_arg_type->has_tensor_type())
                    //{
                    //    const ::onnx::TypeProto_Tensor& typeProto_Tensor = p_arg_type->tensor_type();
                    //    const ::onnx::TensorShapeProto& tensorShapeProto = typeProto_Tensor.shape();
                    //    LOTUS_ENFORCE(shape->dim_size() == tensorShapeProto.dim_size());
                    //    for (int dim = 0; dim < shape->dim_size(); dim++)
                    //    {
                    //        LOTUS_ENFORCE(shape->dim()[dim].dim_value() == tensorShapeProto.dim()[dim].dim_value());
                    //    }
                    //}
                    return *a;
                }
            }
        }
        owned_node_args_.push_back(std::make_unique<NodeArg>(name, p_arg_type));
        return *owned_node_args_.back();
    }

    // Add node to <*this> graph.
    Node* AddNode(const std::string& name,
                  const std::string& op_type,
                  const std::string& description,
                  const std::vector<NodeArg*>& input_args,
                  const std::vector<NodeArg*>& output_args,
                  const NodeAttributes* attributes = nullptr,
                  const std::string& domain = "");

    /**
  Copy node and add to graph.
  @param other Node to copy
  @param returns Pointer to node that was created and inserted.
  */
    Node* AddNode(const Node& other);

    /**
  Remove node and free it.
  */
    bool RemoveNode(NodeIndex node_index);

    // Convenience method for adding a constant op
    Node* AddConstantNode(const std::string& name,
                          const std::string& description,
                          const std::vector<NodeArg*>& output_args,
                          const TensorProto& tensor_proto);

    // Add control edge into <*this> graph.
    // The <dst_node_index> node does not consume any data output by
    // <src_node_index>, but it's designed to be executed behind.
    bool AddControlEdge(NodeIndex src_node_index, NodeIndex dst_node_index);

    bool IsSourceNode(NodeIndex index) const noexcept;
    bool IsSinkNode(NodeIndex index) const noexcept;

    bool IsSourceNode(const Node& node) const noexcept
    {
        return source_node_index_ == node.Index();
    }

    bool IsSinkNode(const Node& node) const noexcept
    {
        return sink_node_index_ == node.Index();
    }

    const Node* SourceNode() const;
    const Node* SinkNode() const;

    // TODO(Task:135) See if GraphBase::GetNodesInTopologicalOrder can be made more correctly const
    // by forcing Resolve to have been called directly previously. Simple change is to return error if
    // GraphResolveNeeded is true.
    Status GetNodesInTopologicalOrder(/*out*/ const std::vector<NodeIndex>** pp_nodes) const;

    // Mark Graph as needing Resolve() to be called
    GraphBase& SetGraphResolveNeeded() noexcept
    {
        graph_resolve_needed_ = true;
        return *this;
    }

    bool GraphResolveNeeded() const noexcept
    {
        return graph_resolve_needed_;
    }

    GraphBase& SetGraphProtoSyncNeeded() noexcept
    {
        graph_proto_sync_needed_ = true;
        return *this;
    }

    bool GraphProtoSyncNeeded() const noexcept
    {
        return graph_proto_sync_needed_;
    }

    // Performs reverse DFS traversal from a set of nodes in 'from' up to
    // the SOURCE node. 'enter' is a visit function that will be invoked
    // on a node when it is visited but its parents haven't been. 'leave'
    // is the visit function invoked on the node after its parents have
    // all been visited. 'comp' is used to stable the traversal order.
    void ReverseDFSFrom(const std::vector<NodeIndex>& from,
                        const std::function<void(const Node*)>& enter,
                        const std::function<void(const Node*)>& leave,
                        const std::function<bool(const Node*, const Node*)>& comp = {}) const;

    void ReverseDFSFrom(const std::vector<const Node*>& from,
                        const std::function<void(const Node*)>& enter,
                        const std::function<void(const Node*)>& leave,
                        const std::function<bool(const Node*, const Node*)>& comp = {}) const;

    virtual ~GraphBase() = default;

protected:
    GraphBase() = default;
    GraphBase(bool graph_resolve_needed,
              bool graph_proto_sync_needed,
              const std::unordered_map<std::string, int>& domain_to_version,
              Version ir_version)
        : graph_resolve_needed_(graph_resolve_needed),
          graph_proto_sync_needed_(graph_proto_sync_needed),
          domain_to_version_(domain_to_version),
          ir_version_(ir_version) {}

    // Add source/sink nodes to <*this> graph.
    void AddSourceSinkNodes();

    // Add node with specified <node_proto>.
    Node* AddNode(const NodeProto& node_proto,
                  const ArgNameToTypeMap& name_to_type);

    NodeIndex SourceNodeIndex() const noexcept
    {
        return source_node_index_;
    }

    NodeIndex SinkNodeIndex() const noexcept
    {
        return sink_node_index_;
    }

    // The topological order of node index as last set by Resolve()
    const std::vector<NodeIndex>& NodesInTopologicalOrder() const noexcept
    {
        return nodes_in_topological_order_;
    }

    std::vector<NodeIndex>& NodesInTopologicalOrder() noexcept
    {
        return nodes_in_topological_order_;
    }

    // Mutable graph inputs.
    std::vector<const NodeArg*>& MutableInputs() noexcept
    {
        return graph_inputs_;
    }

    // Mutable graph outputs.
    std::vector<const NodeArg*>& MutableOutputs() noexcept
    {
        return graph_outputs_;
    }

    const std::unordered_map<std::string, int>& DomainToVersionMap() const noexcept
    {
        return domain_to_version_;
    }

    Version IrVersion() const noexcept
    {
        return ir_version_;
    }

    GraphBase& GraphResolveNeeded(bool needed) noexcept
    {
        graph_resolve_needed_ = needed;
        return *this;
    }

    GraphBase& GraphProtoSyncNeeded(bool needed) noexcept
    {
        graph_proto_sync_needed_ = needed;
        return *this;
    }

    // Build and verify node connection (edges).
    // Verify NodeArg name/type/shape matching correctly.
    Status BuildConnections(
        const std::unordered_map<std::string, Node*>& output_args,
        const std::unordered_map<std::string, NodeIndex>& node_name_to_index);

    Status VerifyNoDuplicateName(
        /*out*/ std::unordered_map<std::string, Node*>& output_args,
        /*out*/ std::unordered_map<std::string, NodeIndex>& node_name_to_index);

    // Check whether <*this> graph is acyclic.
    // Depth-first going thru the graph and check whether there's any back
    // edge.
    // <nodes_in_topological_order> returns nodes' indexes in toplogical
    // order if <Status> returned is "OK", otherwise it's undefined.
    Status CheckIsAcyclic(
        /*out*/ std::vector<NodeIndex>& nodes_in_topological_order) const;

    // Apply shape/type inference to a single node. This is a wrapper for
    // invoking ONNX-defined shape+type inference for a single node.
    // Returns the inferred shape+type for every output of the node in
    // output parameter inferredShapes.
    Status InferOutputTypesAndShapes(ONNXIR::Node& node,
                                     /*out*/ std::vector<TypeProto>& inferred_shapes);

private:
    // need custom versions to handle the unique_ptr's in nodes_
    LOTUS_DISALLOW_COPY_ASSIGN_AND_MOVE(GraphBase);

    Node* AllocateNode();

    /**
  Release the node. 
  @returns false if node_index was invalid.
  */
    bool ReleaseNode(NodeIndex node_index);

    Node* NodeAtIndexImpl(NodeIndex node_index) const
    {
        // if we are trying to access a node that doesn't exist there's (most likely) either a logic issue
        // or a graph consistency/correctness issue. use LOTUS_ENFORCE to prove that or uncover scenarios
        // where we actually expect attempts to retrieve a non-existent node.
        LOTUS_ENFORCE(node_index < nodes_.size(), "Validating no unexpected access using an invalid node_index.");
        return nodes_[node_index].get();
    }

    // Graph nodes.
    // Element in <nodes_> may be nullptr due to graph optimization.
    std::vector<std::unique_ptr<Node>> nodes_;

    // Wrapper of Graph nodes to provide iteration services that hide nullptr entries
    GraphNodes iterable_nodes_{nodes_};

    // Number of nodes.
    // Normally this is smaller than the size of <m_nodes>, as some
    // elements in <m_nodes> may be removed when doing graph optimization,
    // or some elements may be merged, etc.
    int num_of_nodes_ = 0;

    // default to impossible value and not 0
    NodeIndex source_node_index_ = std::numeric_limits<NodeIndex>::max();
    NodeIndex sink_node_index_ = std::numeric_limits<NodeIndex>::max();

    // A flag indicates whether <*this> graph needs to be resolved.
    bool graph_resolve_needed_ = false;

    bool graph_proto_sync_needed_ = false;

    // The topological order of node index.
    std::vector<NodeIndex> nodes_in_topological_order_;

    // Graph inputs.
    std::vector<const NodeArg*> graph_inputs_;

    // Graph outputs.
    std::vector<const NodeArg*> graph_outputs_;

    // Store NodeArg in this graph
    // QUESTION: what does the key represent here?
    std::unordered_map<std::string, NodeArg*> node_args_;

    // NodeArg instances that we own
    std::vector<std::unique_ptr<NodeArg>> owned_node_args_;

    // Node::EdgeEnd instances that we own
    std::vector<std::unique_ptr<Node::EdgeEnd>> owned_edges_;

    const std::unordered_map<std::string, int> domain_to_version_;

    // Model IR version.
    Version ir_version_;
};

// A graph representation class.
class Graph : public GraphBase
{
public:
    // Resolve <*this> graph to ensure it's in a good shape with full
    // functionality.
    // 1. Run through all validation rules.
    //    a. Node name and node output's names should be unique.
    //    b. Attribute match between node and op definition.
    //    c. Input/Output match between node and op definition.
    //    d. Graph is acyclic and sort nodes in topological order.
    // 2. Check & Setup inner nodes' dependency.
    // 3. Cleanup function definition lists.
    // Returns resolving status.
    Status Resolve() override;

    // Getter and Setter for graph name.
    const std::string& Name() const noexcept override;
    void SetName(const std::string& name) override;

    const std::string& Description() const noexcept override;
    void SetDescription(const std::string& description) override;

    // Add/Remove/Get initial tensors for some graph inputs.
    void AddInitializedTensor(const TensorProto& tensor_proto);
    void RemoveInitializedTensor(const std::string& tensor_name);
    bool GetInitializedTensor(const std::string& tensor_name, const TensorProto** value) const;
    const InitializedTensorSet& GetAllInitializedTensors() const noexcept;
    void CleanAllInitializedTensors() noexcept;

    // Get graph value infos.
    const std::vector<const NodeArg*>& GetValueInfo() const noexcept;

    // Serialize the <Graph> into <GraphProto>.
    const GraphProto& ToGraphProto();

private:
    LOTUS_DISALLOW_COPY_ASSIGN_AND_MOVE(Graph);

    // This friendship relationship should only be used to call Graph::Graph and Graph::LoadGraph
    // All other access should be via the public API.
    friend class Model;

    // Constructor: Given a <GraphProto> loaded from model file, construct
    // a <Graph> object.
    Graph(GraphProto* graph_proto,
          const std::unordered_map<std::string, int>& domain_to_version,
          Version ir_version,
          const LotusOpSchemaRegistry* local_registry = nullptr);

    // Constructor: Given a <GraphProto> loaded from model file, construct
    // a <Graph> object and Resolve() it.
    /*static Status LoadGraph(const GraphProto& graph_proto,
                          const std::unordered_map<std::string, int>& domain_to_version,
                          Version ir_version,
                          std::unique_ptr<Graph>& new_graph);*/

    Graph() = delete;

    enum class Type
    {
        // A main graph.
        Main = 1,
        // A sub graph (function).
        Sub = 2,
    };

    Status Resolve(bool no_proto_sync_required);

    Status InferAndVerifyTypeMatch(Node& node,
                                   const OpSchema& op,
                                   const std::unordered_map<std::string, Node*>& output_args);

    // Given nodes in topological order, infer and set type information
    // across <*this> graph if needed, and verify type/attribute
    // information match between node and op.
    Status VerifyNodeAndOpMatch(const std::vector<NodeIndex>& nodes_in_topological_order,
                                const std::unordered_map<std::string, Node*>& output_args);

    // Set graph inputs/outputs when resolving a graph..
    Status SetGraphInputsOutputs();

    // Sync graph inputs/outputs when serializing to proto.
    void SyncGraphInputsOutputs();

    //Verify if all the initializers are valid inputs
    void CleanInitializers();

    // GraphProto to store name, version, initializer.
    // When serializing <*this> Graph to a GraphProto, the nodes and
    // functions in <Graph> will also be fed into <graph_proto_> so that
    // it's consistent with <*this> graph.
    // This pointer is owned by parent model.
    GraphProto* graph_proto_;

    // The node which refers to <*this> graph (Function).
    // Node* node_;

    std::unordered_map<std::string, int> name_to_initial_tensorIndex_;
    InitializedTensorSet name_to_initial_tensor_;
    std::vector<int> removed_initializer_indexes_;

    Type graph_type_ = Type::Main;

    // Graph value_info.
    std::vector<const NodeArg*> value_info_;

    const LotusOpSchemaRegistry* local_registry_;
};
} // namespace ONNXIR
