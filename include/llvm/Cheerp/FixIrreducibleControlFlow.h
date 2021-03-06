//===-- Cheerp/FixIrreducibleControlFlow.h - Cheerp optimization pass ---------------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2018 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#ifndef _CHEERP_FIX_IRREDUCIBLE_CONTROL_FLOW_H
#define _CHEERP_FIX_IRREDUCIBLE_CONTROL_FLOW_H

#include "llvm/Pass.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Dominators.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Support/raw_ostream.h"

#include <unordered_map>
#include <unordered_set>
#include <queue>

namespace llvm
{

/**
 * Transform multiple entry loops in single entry ones
 */
class FixIrreducibleControlFlow: public FunctionPass
{
public:
	static char ID;
	explicit FixIrreducibleControlFlow() : FunctionPass(ID) { }
	bool runOnFunction(Function &F) override;
	const char *getPassName() const override;

	virtual void getAnalysisUsage(AnalysisUsage&) const override;
private:
	/// A generalization of a basic block, containing either a single block
	/// or a set of metablocks dominated by the Entry
	class MetaBlock {
		BasicBlock *Entry;
		// The original predecessors of this metablock. The actual predecessor will
		// eventually be the dispatch block
		SmallPtrSet<BasicBlock *, 2> Preds;
		// The forward blocks that logically lead TOWARDS this metablock
		SmallPtrSet<BasicBlock *, 2> Forwards;
	public:
		explicit MetaBlock(BasicBlock* Entry, DominatorTree& DT): Entry(Entry)
		{
			for (auto Pred: make_range(pred_begin(Entry), pred_end(Entry)))
			{
				// Do not include loops internal to the metablock
				if (!DT.dominates(Entry, Pred))
				{
					Preds.insert(Pred);
				}
			}
		}

		BasicBlock *getEntry() const { return Entry; }

		const SmallPtrSetImpl<BasicBlock *> &predecessors() const {
			return Preds;
		}
		const SmallPtrSetImpl<BasicBlock *> &forwards() const {
			return Forwards;
		}

		void addForwardBlock(BasicBlock* Fwd) {
			Forwards.insert(Fwd);
		}
	};
public:
	class SubGraph;
	struct GraphNode {
		BasicBlock* Header;
		SmallVector<BasicBlock*, 2> Succs;
		SubGraph& Graph;
		explicit GraphNode(BasicBlock* BB, SubGraph& Graph);
	};
	class SubGraph {
	public:
		typedef SmallPtrSet<BasicBlock*, 8> BlockSet;
		typedef std::unordered_map<BasicBlock*, GraphNode> NodeMap;

		explicit SubGraph(BasicBlock* Entry, BlockSet Blocks): Entry(Entry), Blocks(std::move(Blocks))
		{
		}
		BasicBlock* getEntry() { return Entry; }
	private:
		GraphNode* getOrCreate(BasicBlock* BB)
		{
			auto it = Nodes.find(BB);
			if (it == Nodes.end())
			{
				it = Nodes.emplace(BB, GraphNode(BB, *this)).first;
			}
			return &it->second;
		}
		friend struct GraphTraits<SubGraph*>;
		friend struct GraphNode;

		BasicBlock* Entry;
		BlockSet Blocks;
		NodeMap Nodes;
	};
private:
	/// Utility class that performs the FixIrreducibleControlFlow logic on the
	// provided SCC
	class SCCVisitor {
	public:
		SCCVisitor(Function &F, const std::vector<GraphNode*>& SCC)
			: F(F), SCC(SCC)
		{}
		bool run(std::queue<SubGraph>& Queue);

	private:
		// Create the forward blocks and wire them to the dispatcher
		void fixPredecessor(MetaBlock& Meta, BasicBlock* Pred);
		// Move the PHIs at the entry of a metablock into the dispatcher
		void makeDispatchPHIs(const MetaBlock& Meta);
		// Main processing function
		void processBlocks();

	private:
		Function &F;
		DominatorTree DT;
		const std::vector<GraphNode*>& SCC;
		// The metabloks corresponding to the irreducible loop we identified
		std::vector<MetaBlock> MetaBlocks;
		// The new block that will become the single entry of the new loop
		BasicBlock* Dispatcher;
		// The value used by the dispatcher for forwarding to the next metablock
		PHINode* Label;
		// Map that associate the entries of the metablocks with their index in the
		// switch instruction in the dispatcher
		DenseMap<BasicBlock*, unsigned> Indices;
	};
	bool visitSubGraph(Function& F, std::queue<SubGraph>& Queue);
};

template <> struct GraphTraits<FixIrreducibleControlFlow::SubGraph*> {
	typedef FixIrreducibleControlFlow::GraphNode NodeType;
	typedef mapped_iterator<SmallVectorImpl<BasicBlock*>::iterator, std::function<FixIrreducibleControlFlow::GraphNode*(BasicBlock*)>> ChildIteratorType;

	static NodeType *getEntryNode(FixIrreducibleControlFlow::SubGraph* G) { return G->getOrCreate(G->Entry); }
	static inline ChildIteratorType child_begin(NodeType *N) {
		return ChildIteratorType(N->Succs.begin(), [N](BasicBlock* BB){ return N->Graph.getOrCreate(BB);});
	}
	static inline ChildIteratorType child_end(NodeType *N) {
		return ChildIteratorType(N->Succs.end(), [](BasicBlock* BB){ llvm_unreachable("dereferencing past-the-end iterator");return nullptr;});
	}
};


//===----------------------------------------------------------------------===//
//
// FixIrreducibleControlFlow
//
FunctionPass *createFixIrreducibleControlFlowPass();

}

#endif
