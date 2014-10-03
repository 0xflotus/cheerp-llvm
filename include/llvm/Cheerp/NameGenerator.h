//===-- Cheerp/NameGenerator.h - Cheerp name generator code ----------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2013 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#ifndef _CHEERP_NAME_GENERATOR_H
#define _CHEERP_NAME_GENERATOR_H

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Cheerp/Registerize.h"
#include <unordered_map>

namespace cheerp {

class GlobalDepsAnalyzer;

// This class is responsible for generate unique names for a llvm::Value
// The class is dependent on registerize to work properly
class NameGenerator
{
public:
	/**
	 * This initialize the namegenerator by collecting
	 * all the global variable names
	 */
	explicit NameGenerator( const GlobalDepsAnalyzer &, const Registerize &, bool makeReadableNames = true );

	/**
	 * Return the computed name for the given variable.
	 * This function can be called only if the passed value is not an inlined instruction
	 */
	llvm::StringRef getName(const llvm::Value* v) const
	{
		assert(namemap.count(v) );
		assert(! namemap.at(v).empty() );
		if(!edgeContext.isNull())
			return getNameForEdge(v);
		return namemap.at(v);
	}

	/**
	 * Same as getName, but supports the required temporary variables in edges between blocks
	 * It uses the current edge context.
	*/
	llvm::StringRef getNameForEdge(const llvm::Value* v) const;

	void setEdgeContext(const llvm::BasicBlock* fromBB, const llvm::BasicBlock* toBB)
	{
		assert(edgeContext.isNull());
		edgeContext.fromBB=fromBB;
		edgeContext.toBB=toBB;
	}

	void clearEdgeContext()
	{
		edgeContext.clear();
	}

	// Filter the original string so that it no longer contains invalid JS characters.
	static llvm::SmallString<4> filterLLVMName( llvm::StringRef, bool isGlobalName );

private:
	void generateCompressedNames( const GlobalDepsAnalyzer & );
	void generateReadableNames( const GlobalDepsAnalyzer & );
	
	// Determine if an instruction actually needs a name
	bool needsName(const llvm::Instruction &) const;

	const Registerize& registerize;
	std::unordered_map<const llvm::Value*, llvm::SmallString<4> > namemap;
	struct InstOnEdge
	{
		const llvm::BasicBlock* fromBB;
		const llvm::BasicBlock* toBB;
		uint32_t registerId;
		bool operator==(const InstOnEdge& r) const
		{
			return fromBB==r.fromBB && toBB==r.toBB && registerId==r.registerId;
		}
		struct Hash
		{
			size_t operator()(const InstOnEdge& i) const
			{
				return std::hash<const llvm::BasicBlock*>()(i.fromBB) ^
					std::hash<const llvm::BasicBlock*>()(i.toBB) ^
					std::hash<uint32_t>()(i.registerId);
			}
		};
	};
	typedef std::unordered_map<InstOnEdge, llvm::SmallString<8>, InstOnEdge::Hash > EdgeNameMapTy;
	EdgeNameMapTy edgeNamemap;
	struct EdgeContext
	{
		const llvm::BasicBlock* fromBB;
		const llvm::BasicBlock* toBB;
		EdgeContext():fromBB(NULL), toBB(NULL)
		{
		}
		bool isNull() const
		{
			return fromBB==NULL;
		}
		void clear()
		{
			fromBB=NULL;
			toBB=NULL;
		}
	};
	EdgeContext edgeContext;
};

}

#endif //_CHEERP_NAME_GENERATOR_H