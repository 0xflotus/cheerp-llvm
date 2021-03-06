//===-- Utility.cpp - The Cheerp JavaScript generator --------------------===//
//
//                     Cheerp: The C++ compiler for the Web
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
// Copyright 2011-2015 Leaning Technologies
//
//===----------------------------------------------------------------------===//

#include <sstream>
#include "llvm/Cheerp/Registerize.h"
#include "llvm/Cheerp/Utility.h"
#include "llvm/Cheerp/GEPOptimizer.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/FormattedStream.h"

using namespace llvm;

namespace cheerp {

bool isNopCast(const Value* val)
{
	const CallInst * newCall = dyn_cast<const CallInst>(val);
	if(newCall && newCall->getCalledFunction())
	{
		unsigned int id = newCall->getCalledFunction()->getIntrinsicID();
		
		if ( Intrinsic::cheerp_upcast_collapsed == id ||
			Intrinsic::cheerp_cast_user == id )
			return true;
		
		if ( Intrinsic::cheerp_downcast == id )
		{
			Type* t = newCall->getArgOperand(0)->getType()->getPointerElementType();

			// Special case downcasts from a type to itself, they are used to support pointers to member functions
			if ( TypeSupport::isClientType(t) ||
				(isa<ConstantInt>( newCall->getArgOperand(1) ) && getIntFromValue( newCall->getArgOperand(1) ) == 0 && newCall->getArgOperand(0)->getType() != newCall->getType()))
				return true;
		}
		
	}
	return false;
}

bool isValidVoidPtrSource(const Value* val, std::set<const PHINode*>& visitedPhis)
{
	if (DynamicAllocInfo::getAllocType(val) != DynamicAllocInfo::not_an_alloc )
		return true;
	const PHINode* newPHI=dyn_cast<const PHINode>(val);
	if(newPHI)
	{
		if(visitedPhis.count(newPHI))
		{
			//Assume true, if needed it will become false later on
			return true;
		}
		visitedPhis.insert(newPHI);
		for(unsigned i=0;i<newPHI->getNumIncomingValues();i++)
		{
			if(!isValidVoidPtrSource(newPHI->getIncomingValue(i),visitedPhis))
			{
				visitedPhis.erase(newPHI);
				return false;
			}
		}
		visitedPhis.erase(newPHI);
		return true;
	}
	return false;
}

bool isInlineable(const Instruction& I, const PointerAnalyzer& PA)
{
	//Beside a few cases, instructions with a single use may be inlined
	//TODO: Find out a better heuristic for inlining, it seems that computing
	//may be faster even on more than 1 use
	bool hasMoreThan1Use = I.hasNUsesOrMore(2);
	// Do not inline the instruction if the use is in another block
	// If this happen the instruction may have been hoisted outside a loop and we want to keep it there
	auto isUserInOtherBlock = [](const Instruction& I)
	{
		return !I.use_empty() && cast<Instruction>(I.use_begin()->getUser())->getParent()!=I.getParent();
	};
	// On wasm it is efficient to inline constant geps, but only if the offset is positve
	// NOTE: This only checks the first index as an approximation, we would need DataLayout
	//       to compute the full offset exactly
	auto isPositiveOffsetGep = [](const GetElementPtrInst& gep)
	{
		if (!gep.hasAllConstantIndices())
			return false;
		ConstantInt* firstOffset = dyn_cast<ConstantInt>(gep.getOperand(1));
		if (!firstOffset)
			return false;
		return firstOffset->getSExtValue() >= 0;
	};
	if(I.getOpcode()==Instruction::GetElementPtr)
	{
		if(PA.getPointerKind(&I) == RAW)
		{
			// Geps with constant indices can be compactly encoded.
			auto& gep = cast<GetElementPtrInst>(I);
			if (isPositiveOffsetGep(gep))
				return true;
			if (hasMoreThan1Use || isUserInOtherBlock(I))
				return false;
			return true;
		}

		// For generic JS, computing the gep in a local will not result
		// in smaller code due to the overhead of additional type casts.
		//
		// Note that geps that are used in equal pointer comparisons should
		// always be inlined. See also the assertions in
		// |CheerpWriter::compileEqualPointersComparison|.
		if (I.getNumOperands() == 2)
			return true;

		if (PA.getPointerKind(&I) == COMPLETE_OBJECT) {
			auto type = cast<GetElementPtrInst>(I).getType()->getElementType();
			// Always inline geps to immutable fields of a complete object.
			if (TypeSupport::isImmutableType(type))
				return true;

			return !hasMoreThan1Use;
		}

		// Split regular, regular, and byte layout are always inlined.
		return true;
	}
	else if(I.getOpcode()==Instruction::BitCast)
	{
		if(PA.getPointerKind(&I) == RAW)
		{
			if(GetElementPtrInst* gep = dyn_cast<GetElementPtrInst>(I.getOperand(0)))
			{
				if (isPositiveOffsetGep(*gep))
					return true;
			}
			return !hasMoreThan1Use || !isa<Instruction>(I.getOperand(0)) || !isInlineable(*cast<Instruction>(I.getOperand(0)), PA);
		}

		if (PA.getPointerKind(&I) == COMPLETE_OBJECT) {
			// Never inline if the source is REGULAR (forces conversion to CO)
			if(PA.getPointerKind(I.getOperand(0)) == REGULAR)
				return false;

			return !hasMoreThan1Use;
		}

		// Split regular, regular, and byte layout are always inlined.
		return true;
	}
	else if(I.getOpcode()==Instruction::Trunc)
	{
		return !hasMoreThan1Use || !isa<Instruction>(I.getOperand(0)) || !isInlineable(*cast<Instruction>(I.getOperand(0)), PA);
	}
	else if(const IntrinsicInst* II=dyn_cast<IntrinsicInst>(&I))
	{
		// Special handling for instrinsics
		switch(II->getIntrinsicID())
		{
			case Intrinsic::cheerp_cast_user:
			case Intrinsic::cheerp_upcast_collapsed:
			case Intrinsic::cheerp_make_regular:
				return true;
			default:
				break;
		}
		return false;
	}
	else if((I.getOpcode()==Instruction::FCmp || I.getOpcode()==Instruction::ICmp) && hasMoreThan1Use)
	{
		return !I.getOperand(0)->getType()->isPointerTy();
	}
	else if(!hasMoreThan1Use)
	{
		if(isUserInOtherBlock(I))
			return false;
		switch(I.getOpcode())
		{
			// A few opcodes if immediately used in a store or return can be inlined
			case Instruction::Call:
			case Instruction::Load:
			{
				if(I.use_empty())
					return false;
				// We can only inline COMPLETE_OBJECT and RAW pointers, other kinds may actually require multiple accesses while rendering
				// NOTE: When RAW pointers are converted to REGULAR/SPLIT_REGULAR only one access (the offset part) is used, the base is a constant HEAP*
				if(I.getType()->isPointerTy())
				{
					POINTER_KIND k = PA.getPointerKind(&I);
					if(k != COMPLETE_OBJECT && k != RAW)
						return false;
				}

				const bool hasSideEffects = I.mayHaveSideEffects();
				// Skip up to N instructions, looking for the final not-inlineable user of this load/call
				// If we find no interfering instructions along the way it is safe to inline
				// TODO: Currently we assume that crossing an instruction implies reordering, but in reality this actually depends on rendering
				//       for example call(a, b) in JS guarantees that all the expression for 'a' in evaluated before 'b'
				uint32_t maxSkip = 10;
				const Instruction* curInst = &I;
				const Instruction* nextInst = &I;
				while(1)
				{
					nextInst = nextInst->getNextNode();
					if(nextInst == nullptr)
					{
						// We have reached the end of the block without finding the final user, can't inline
						break;
					}
					else if(curInst->user_back() == nextInst)
					{
						// Reached the direct user
						if(nextInst->getOpcode() == Instruction::BitCast ||
							nextInst->getOpcode() == Instruction::Trunc)
						{
							// Avoid interacting with the bitcast/trunc logic for now
							break;
						}
						else if(isa<IntrinsicInst>(nextInst))
						{
							// Avoid interacting with intrinsics logic for now
							break;
						}
						else if(!isInlineable(*nextInst, PA))
						{
							// Not inlineable, it is safe to inline
							return true;
						}
						else if(nextInst->getOpcode() == Instruction::Call || nextInst->getOpcode() == Instruction::Load)
						{
							// Inlineable and this logic has already been done
							return true;
						}
						else if(!nextInst->hasOneUse())
						{
							break;
						}
						else
						{
							// It is inlineable, if it has only one user we can keep going
							curInst = nextInst;
						}
					}
					else if(hasSideEffects && nextInst->mayReadOrWriteMemory())
					{
						// Do not reorder side-effectfull calls over anything which may read memory
						break;
					}
					else if(nextInst->mayHaveSideEffects())
					{
						// This instruction is not the user and has side effects, give up
						break;
					}
					else if(--maxSkip == 0)
					{
						// Can't skip anymore
						break;
					}
				}
				return false;
			}
			case Instruction::Invoke:
			case Instruction::Ret:
			case Instruction::LandingPad:
			case Instruction::Store:
			case Instruction::InsertValue:
			case Instruction::PHI:
			case Instruction::Resume:
			case Instruction::Br:
			case Instruction::Alloca:
			case Instruction::Switch:
			case Instruction::Unreachable:
			case Instruction::VAArg:
				return false;
			case Instruction::Add:
			case Instruction::Sub:
			case Instruction::Mul:
			case Instruction::And:
			case Instruction::Or:
			case Instruction::Xor:
			case Instruction::Trunc:
			case Instruction::FPToSI:
			case Instruction::SIToFP:
			case Instruction::SDiv:
			case Instruction::SRem:
			case Instruction::Shl:
			case Instruction::AShr:
			case Instruction::LShr:
			case Instruction::FAdd:
			case Instruction::FDiv:
			case Instruction::FRem:
			case Instruction::FSub:
			case Instruction::FPTrunc:
			case Instruction::FPExt:
			case Instruction::FMul:
			case Instruction::FCmp:
			case Instruction::ICmp:
			case Instruction::ZExt:
			case Instruction::SExt:
			case Instruction::Select:
			case Instruction::ExtractValue:
			case Instruction::URem:
			case Instruction::UDiv:
			case Instruction::UIToFP:
			case Instruction::FPToUI:
			case Instruction::PtrToInt:
			case Instruction::IntToPtr:
				return true;
			default:
				llvm::report_fatal_error(Twine("Unsupported opcode: ",StringRef(I.getOpcodeName())), false);
				return true;
		}
	}
	return false;
}

bool isWasmIntrinsic(const llvm::Function* F)
{
	return false
#define WASM_INTRINSIC(name, opcode, symbol) \
		|| F->getName() == symbol
WASM_INTRINSIC_LIST(WASM_INTRINSIC)
#undef WASM_INTRINSIC
	;
}

uint32_t getIntFromValue(const Value* v)
{
	if(!ConstantInt::classof(v))
	{
		llvm::errs() << "Expected constant int found " << *v << "\n";
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
		return 0;
	}

	const ConstantInt* i=cast<const ConstantInt>(v);
	return i->getZExtValue();
}

std::string valueObjectName(const Value* v)
{
	std::ostringstream os;
	if (const Instruction * p = dyn_cast<const Instruction>(v) )
		os << " instruction " << p->getOpcodeName() << "\n";
	else if (const Constant * p = dyn_cast<const Constant>(v) )
	{
		os << " constant " << p->getName().str() << "(";
		
		// Feel free to find a way to avoid this obscenity
		if (isa<const BlockAddress>(p))
			os << "BlockAddress";
		else if (isa<const ConstantAggregateZero>(p))
			os << "ConstantAggregateZero";
		else if (isa<const ConstantArray>(p))
			os << "ConstantArray";
		else if (isa<const ConstantDataSequential>(p))
			os << "ConstantDataSequential";
		else if (const ConstantExpr * pc = dyn_cast<const ConstantExpr>(p))
		{
			os << "ConstantExpr [" << pc->getOpcodeName() <<"]";
		}
		else if (isa<const ConstantFP>(p))
			os << "ConstantFP";
		else if (isa<const ConstantInt>(p))
			os << "ConstantInt";
		else if (isa<const ConstantPointerNull>(p))
			os << "ConstantPointerNull";
		else if (isa<const ConstantStruct>(p))
			os << "ConstantStruct";
		else if (isa<const ConstantVector>(p))
			os << "ConstantVector";
		else if (isa<const GlobalAlias>(p))
			os << "GlobalAlias";
		else if (isa<const GlobalValue>(p))
			os << "GlobalValue";
		else if (isa<const UndefValue>(p))
			os << "UndefValue";
		else
			os << "Unknown";
		os << ")\n";
	}
	else if ( isa<const Operator>(p) )
		os << " operator " << p->getName().str() << "\n";
	return os.str();
}

bool hasNonLoadStoreUses( const Value* v)
{
	for(const Use& U: v->uses())
	{
		const User* user = U.getUser();
		if (isa<LoadInst>(user))
			continue;
		if (isa<StoreInst>(user) && U.getOperandNo()==1)
			continue;
		return true;
	}
	return false;
}

Type* getGEPContainerType(const User* gep)
{
	SmallVector< const Value*, 8 > indices(std::next(gep->op_begin()), std::prev(gep->op_end()));
	Type* basePointerType = gep->getOperand(0)->getType();
	Type* containerType = GetElementPtrInst::getIndexedType(basePointerType,
			makeArrayRef(const_cast<Value* const*>(indices.begin()),
				     const_cast<Value* const*>(indices.end())));
	return containerType;
}

bool TypeSupport::isDerivedStructType(StructType* derivedType, StructType* baseType)
{
	if(derivedType->getNumElements() < baseType->getNumElements())
		return false;
	// If a type is derived should begin with the same fields as the base type
	for(uint32_t i=0;i<baseType->getNumElements();i++)
	{
		if(derivedType->getElementType(i)!=baseType->getElementType(i))
			return false;
	}
	return true;
}

bool TypeSupport::getBasesInfo(const Module& module, const StructType* t, uint32_t& firstBase, uint32_t& baseCount)
{
	const NamedMDNode* basesNamedMeta = getBasesMetadata(t, module);
	if(!basesNamedMeta)
	{
		// Before giving up, check if the direct base has any bases
		if(t->getDirectBase())
			return getBasesInfo(module,t->getDirectBase(),firstBase,baseCount);
		return false;
	}

	MDNode* basesMeta=basesNamedMeta->getOperand(0);
	assert(basesMeta->getNumOperands()>=1);
	firstBase=getIntFromValue(cast<ConstantAsMetadata>(basesMeta->getOperand(0))->getValue());
	baseCount = 0;

	assert(firstBase < t->getNumElements());
	baseCount = t->getNumElements()-firstBase;
	return true;
}

bool TypeSupport::useWrapperArrayForMember(const PointerAnalyzer& PA, StructType* st, uint32_t memberIndex) const
{
	uint32_t firstBase, baseCount;
	if(getBasesInfo(st, firstBase, baseCount))
	{
		if(st->getDirectBase() && memberIndex < st->getDirectBase()->getNumElements())
			return useWrapperArrayForMember(PA, st->getDirectBase(), memberIndex);
		if(memberIndex >= firstBase && memberIndex < (firstBase+baseCount) && st->getElementType(memberIndex)->isStructTy())
			return false;
	}
	// We don't want to use the wrapper array if the downcast array is alredy available
	TypeAndIndex baseAndIndex(st, memberIndex, TypeAndIndex::STRUCT_MEMBER);
	assert(PA.getPointerKindForMember(baseAndIndex)!=SPLIT_REGULAR);
	return PA.getPointerKindForMember(baseAndIndex)==REGULAR;
}

char TypeSupport::getPrefixCharForMember(const PointerAnalyzer& PA, llvm::StructType* st, uint32_t memberIndex) const
{
	bool useWrapperArray = useWrapperArrayForMember(PA, st, memberIndex);
	Type* elementType = st->getElementType(memberIndex);
	if(useWrapperArray)
		return 'a';
	else if(elementType->isIntegerTy())
		return 'i';
	else if(elementType->isFloatTy() || elementType->isDoubleTy())
		return 'd';
	else
		return 'a';
}

bool TypeSupport::isJSExportedType(StructType* st, const Module& m)
{
	return m.getNamedMetadata(llvm::Twine(st->getName(),"_methods"))!=NULL;
}

std::pair<StructType*, StringRef> TypeSupport::getJSExportedTypeFromMetadata(StringRef name, const Module& module)
{
	StringRef mangledName = name.drop_front(6).drop_back(8);

	demangler_iterator demangler( mangledName );

	StringRef jsClassName = *demangler++;

	if ( demangler != demangler_iterator() )
	{
		Twine errorString("Class: ",jsClassName);

		for ( ; demangler != demangler_iterator(); ++ demangler )
			errorString.concat("::").concat(*demangler);

		errorString.concat(" is not a valid [[jsexport]] class (not in global namespace)\n");

		llvm::report_fatal_error( errorString );
	}

	assert( jsClassName.end() > name.begin() && std::size_t(jsClassName.end() - name.begin()) <= name.size() );
	StructType * t = module.getTypeByName( StringRef(name.begin(), jsClassName.end() - name.begin() ) );
	assert(t);
	return std::make_pair(t, jsClassName);
}

bool TypeSupport::isSimpleType(Type* t, bool forceTypedArrays)
{
	switch(t->getTypeID())
	{
		case Type::IntegerTyID:
		case Type::FloatTyID:
		case Type::DoubleTyID:
		case Type::PointerTyID:
			return true;
		case Type::StructTyID:
		{
			// Union are considered simple because they use a single DataView object
			if(TypeSupport::hasByteLayout(t))
				return true;
			break;
		}
		case Type::ArrayTyID:
		{
			ArrayType* at=static_cast<ArrayType*>(t);
			Type* et=at->getElementType();
			// When a single typed array object is used, we consider this array as simple
			if(isTypedArrayType(et, forceTypedArrays) && at->getNumElements()>1)
				return true;
			if(TypeSupport::hasByteLayout(t))
				return true;
			break;
		}
		default:
			assert(false);
	}
	return false;
}

uint32_t TypeSupport::getAlignmentAsmJS(const llvm::DataLayout& dl, llvm::Type* t)
{
	uint32_t alignment = 8;
	// it the type is an array, look at the element type
	while (t->isArrayTy())
	{
		t = t->getArrayElementType();
	}
	// NOTE: we could compute the real minimum alignment with a
	//       recursive scan of the struct, but instead we just
	//       align to 8 bytes
	if (t->isStructTy())
	{
		alignment = 8;
	}
	else
	{
		alignment = dl.getTypeAllocSize(t);
	}

	return alignment;
}

DynamicAllocInfo::DynamicAllocInfo( ImmutableCallSite callV, const DataLayout* DL, bool forceTypedArrays ) : call(callV), type( getAllocType(callV) ), castedType(nullptr), forceTypedArrays(forceTypedArrays)
{
	if ( isValidAlloc() )
	{
		castedType = computeCastedType();
		typeSize = DL->getTypeAllocSize(castedType->getPointerElementType());
	}
}

DynamicAllocInfo::AllocType DynamicAllocInfo::getAllocType( ImmutableCallSite callV )
{
	// The alloc type is always not_an_alloc in asmjs, since we don't need
	// thr DynamicAllocInfo functionality
	if (callV->getParent()->getParent()->getSection() == StringRef("asmjs"))
		return not_an_alloc;
	DynamicAllocInfo::AllocType ret = not_an_alloc;
	if (callV.isCall() || callV.isInvoke() )
	{
		if (const Function * f = callV.getCalledFunction() )
		{
			if (f->getName() == "malloc")
				ret = malloc;
			else if (f->getName() == "calloc")
				ret = calloc;
			else if (f->getIntrinsicID() == Intrinsic::cheerp_allocate ||
			         f->getIntrinsicID() == Intrinsic::cheerp_allocate_array)
				ret =  cheerp_allocate;
			else if (f->getIntrinsicID() == Intrinsic::cheerp_reallocate)
				ret = cheerp_reallocate;
			else if (f->getName() == "_Znwj")
				ret = opnew;
			else if (f->getName() == "_Znaj")
				ret = opnew_array;
		}
	}
	// As above, allocations of asmjs types are considered not_an_alloc
	if (ret != not_an_alloc && TypeSupport::isAsmJSPointer(callV->getType()))
		return not_an_alloc;
	return ret;
}

PointerType * DynamicAllocInfo::computeCastedType() const 
{
	assert(isValidAlloc() );
	
	if ( type == cheerp_allocate || type == cheerp_reallocate )
	{
		assert( call.getType()->isPointerTy() );
		return cast<PointerType>(call.getType());
	}
	
	auto getTypeForUse = [](const User * U) -> Type *
	{
		if ( isa<BitCastInst>(U) )
			return U->getType();
		else if ( const IntrinsicInst * ci = dyn_cast<IntrinsicInst>(U) )
			if ( ci->getIntrinsicID() == Intrinsic::cheerp_cast_user )
				return U->getType();
		return nullptr;
	};
	
	auto firstNonNull = std::find_if(
		call->user_begin(),
		call->user_end(),
		getTypeForUse);
	
	// If there are no casts, use i8*
	if ( call->user_end() == firstNonNull )
	{
		return cast<PointerType>(Type::getInt8PtrTy(call->getContext()));
	}
	
	assert( getTypeForUse(*firstNonNull)->isPointerTy() );
	
	PointerType * pt = cast<PointerType>( getTypeForUse(*firstNonNull) );
	
	// Check that all uses are the same
	if (! std::all_of( 
		std::next(firstNonNull),
		call->user_end(),
		[&]( const User * U ) { return getTypeForUse(U) == pt; }) )
	{
		call->getParent()->getParent()->dump();
		llvm::errs() << "Can not deduce valid type for allocation instruction: " << call->getName() << '\n';
		llvm::errs() << "In function: " << call->getParent()->getParent()->getName() << "\n";
		llvm::errs() << "Allocation instruction: "; call->dump();
		llvm::errs() << "Pointer: "; pt->dump();
		llvm::errs() << "Usage:\n";
		for (auto u = call->user_begin(); u != call->user_end(); u++)
		{
			u->dump();
		}
		llvm::report_fatal_error("Unsupported code found, please report a bug", false);
	}
	
	return pt;
}

const Value * DynamicAllocInfo::getByteSizeArg() const
{
	assert( isValidAlloc() );

	if ( calloc == type )
	{
		assert( call.arg_size() == 2 );
		return call.getArgument(1);
	}
	else if ( cheerp_reallocate == type )
	{
		assert( call.arg_size() == 2 );
		return call.getArgument(1);
	}

	assert( call.arg_size() == 1 );
	return call.getArgument(0);
}

const Value * DynamicAllocInfo::getNumberOfElementsArg() const
{
	assert( isValidAlloc() );
	
	if ( type == calloc )
	{
		assert( call.arg_size() == 2 );
		return call.getArgument(0);
	}
	return nullptr;
}

const Value * DynamicAllocInfo::getMemoryArg() const
{
	assert( isValidAlloc() );
	
	if ( type == cheerp_reallocate )
	{
		assert( call.arg_size() == 2 );
		return call.getArgument(0);
	}
	return nullptr;
}

bool DynamicAllocInfo::sizeIsRuntime() const
{
	assert( isValidAlloc() );
	if ( getAllocType() == calloc && !isa<ConstantInt> (getNumberOfElementsArg() ) )
		return true;
	if ( isa<ConstantInt>(getByteSizeArg()) )
		return false;
	return true;
}

bool DynamicAllocInfo::useCreateArrayFunc() const
{
	if( !TypeSupport::isTypedArrayType( getCastedType()->getElementType(), forceTypedArrays ) )
	{
		if( sizeIsRuntime() || type == cheerp_reallocate)
			return true;
		// Should also use createArray if allocating many elements
		uint32_t byteSize = cast<ConstantInt>(getByteSizeArg())->getZExtValue();
		return byteSize/typeSize > 8;
	}
	return false;
}

bool DynamicAllocInfo::useCreatePointerArrayFunc() const
{
	if (getCastedType()->getElementType()->isPointerTy() )
	{
		assert( !TypeSupport::isTypedArrayType( getCastedType()->getElementType(), forceTypedArrays) );
		if( sizeIsRuntime() || type == cheerp_reallocate)
			return true;
		// Should also use createPointerArray if allocating many elements
		uint32_t byteSize = cast<ConstantInt>(getByteSizeArg())->getZExtValue();
		return byteSize/typeSize > 8;
	}
	return false;
}

bool DynamicAllocInfo::useTypedArray() const
{
	return TypeSupport::isTypedArrayType( getCastedType()->getElementType(), forceTypedArrays);
}

void EndOfBlockPHIHandler::runOnPHI(PHIRegs& phiRegs, uint32_t regId, const llvm::Instruction* incoming, llvm::SmallVector<std::pair<const PHINode*, /*selfReferencing*/bool>, 4>& orderedPHIs)
{
	auto it=phiRegs.find(regId);
	if(it==phiRegs.end())
		return;
	PHIRegData& regData=it->second;
	if(regData.status==PHIRegData::VISITED)
		return;
	else if(regData.status==PHIRegData::VISITING)
	{
		// Report the recursive dependency to the user
		handleRecursivePHIDependency(incoming);
		return;
	}
	// Not yet visited
	regData.status=PHIRegData::VISITING;
	for(auto& reg: regData.incomingRegs)
		runOnPHI(phiRegs, reg.first, reg.second, orderedPHIs);
	// Add the PHI to orderedPHIs only after eventual dependencies have been added
	orderedPHIs.push_back(std::make_pair(regData.phiInst, regData.selfReferencing));
	regData.status=PHIRegData::VISITED;
}

void EndOfBlockPHIHandler::runOnEdge(const Registerize& registerize, const BasicBlock* fromBB, const BasicBlock* toBB)
{
	BasicBlock::const_iterator I=toBB->begin();
	BasicBlock::const_iterator IE=toBB->end();
	PHIRegs phiRegs;
	llvm::SmallVector<std::pair<const PHINode*, /*selfReferencing*/bool>, 4> orderedPHIs;
	for(;I!=IE;++I)
	{
		// Gather the dependency graph between registers for PHIs and incoming values
		// Also add PHIs which are always safe to the orderedPHIs vector
		const PHINode* phi=dyn_cast<PHINode>(I);
		if(!phi)
			break;
		if(phi->use_empty())
			continue;
		const Value* val=phi->getIncomingValueForBlock(fromBB);
		const Instruction* I=dyn_cast<Instruction>(val);
		if(!I)
		{
			orderedPHIs.push_back(std::make_pair(phi, /*selfReferencing*/false));
			continue;
		}
		uint32_t phiReg = registerize.getRegisterId(phi);
		setRegisterUsed(phiReg);
		// This instruction may depend on multiple registers
		llvm::SmallVector<std::pair<uint32_t, const Instruction*>, 2> incomingRegisters;
		llvm::SmallVector<std::pair<const Instruction*, /*dereferenced*/bool>, 4> instQueue;
		instQueue.push_back(std::make_pair(I, false));
		bool mayNeedSelfRef = phi->getType()->isPointerTy() && PA.getPointerKind(phi) == SPLIT_REGULAR && !PA.getConstantOffsetForPointer(phi);
		bool selfReferencing = false;
		while(!instQueue.empty())
		{
			std::pair<const Instruction*, bool> incomingInst = instQueue.pop_back_val();
			if(!isInlineable(*incomingInst.first, PA))
			{
				uint32_t incomingValueId = registerize.getRegisterId(incomingInst.first);
				if(incomingValueId==phiReg)
				{
					if(mayNeedSelfRef &&
						PA.getPointerKind(incomingInst.first) == SPLIT_REGULAR && // If the incoming inst is not SPLIT_REGULAR there is no collision risk
						!PA.getConstantOffsetForPointer(incomingInst.first) && // If the offset part is constant we can reorder the operation to avoid a collision
						incomingInst.second) // If the register is not dereferenced there is no conflict as base and offset are not used together
					{
						selfReferencing = true;
					}
					continue;
				}
				setRegisterUsed(incomingValueId);
				incomingRegisters.push_back(std::make_pair(incomingValueId, incomingInst.first));
			}
			else
			{
				// TODO: Loads when inlined should go here
				bool dereferenced = incomingInst.second || (mayNeedSelfRef && isa<GetElementPtrInst>(incomingInst.first) && incomingInst.first->getNumOperands() > 2);
				for(const Value* op: incomingInst.first->operands())
				{
					const Instruction* opI = dyn_cast<Instruction>(op);
					if(!opI)
						continue;
					instQueue.push_back(std::make_pair(opI, dereferenced));
				}
			}
		}
		if(incomingRegisters.empty())
			orderedPHIs.push_back(std::make_pair(phi, selfReferencing));
		else
			phiRegs.insert(std::make_pair(phiReg, PHIRegData(phi, std::move(incomingRegisters), selfReferencing)));
	}
	for(auto it: phiRegs)
	{
		if(it.second.status!=PHIRegData::VISITED)
			runOnPHI(phiRegs, it.first, nullptr, orderedPHIs);
	}
	// Notify the user for each PHI, in the right order to avoid accidental overwriting
	for(uint32_t i=orderedPHIs.size();i>0;i--)
	{
		const PHINode* phi=orderedPHIs[i-1].first;
		const Value* val=phi->getIncomingValueForBlock(fromBB);
		handlePHI(phi, val, orderedPHIs[i-1].second);
	}
}

const ConstantArray* ModuleGlobalConstructors(Module& M)
{
	GlobalVariable* var = M.getGlobalVariable("llvm.global_ctors");
	if (!var || !var->hasInitializer())
		return nullptr;

	if (!isa<ConstantArray>(var->getInitializer()))
		return nullptr;

	return cast<ConstantArray>(var->getInitializer());
}

bool needsSecondaryName(const Value* V, const PointerAnalyzer& PA)
{
	if(!V->getType()->isPointerTy())
		return false;
	if(PA.getPointerKind(V) == SPLIT_REGULAR && !PA.getConstantOffsetForPointer(V))
		return true;
	return false;
}

Instruction* findCommonInsertionPoint(Instruction* I, DominatorTree* DT, Instruction* currentInsertionPoint, Instruction* user)
{
	if(PHINode* phi = dyn_cast<PHINode>(user))
	{
		// It must dominate all incoming blocks that has the value as an incoming value
		for(unsigned i = 0; i < phi->getNumIncomingValues(); i++)
		{
			if(phi->getIncomingValue(i) != I)
				continue;
			BasicBlock* incomingBlock = phi->getIncomingBlock(i);
			currentInsertionPoint = findCommonInsertionPoint(I, DT, currentInsertionPoint, incomingBlock->getTerminator());
		}
		return currentInsertionPoint;
	}
	if(!currentInsertionPoint || DT->dominates(user, currentInsertionPoint))
		return user;
	else if(DT->dominates(currentInsertionPoint, user))
		return currentInsertionPoint;
	else if(currentInsertionPoint->getParent() == user->getParent())
	{
		// Check relative order, find it currentInsertionPoint is above user
		Instruction* it = currentInsertionPoint;
		while(it)
		{
			if(it == user)
			{
				// user is after currentInsertionPoint
				return currentInsertionPoint;
			}
			it = it->getNextNode();
		}
		// user is above currentInsertionPoint
		return user;
	}
	else // Find a common dominator
	{
		BasicBlock* common = DT->findNearestCommonDominator(currentInsertionPoint->getParent(),user->getParent());
		return common->getTerminator();
	}
}

const Instruction* getUniqueIncomingInst(const Value* v, const PointerAnalyzer& PA)
{
	while(const Instruction* I = dyn_cast<Instruction>(v))
	{
		if(!isInlineable(*I, PA))
			return I;
		else if(I->getOpcode() == Instruction::Trunc)
			v = I->getOperand(0);
		else if(I->getOpcode() == Instruction::BitCast && PA.getPointerKind(I) == RAW)
		{
			// TODO: Expand this logic to support other cases where a bitcast is a nop (when no kind conversion is required?)
			v = I->getOperand(0);
		}
		else
			break;
	}
	return nullptr;
}

}

namespace llvm
{

void initializeCheerpOpts(PassRegistry &Registry)
{
	initializeAllocaArraysPass(Registry);
	initializeAllocaMergingPass(Registry);
	initializeGlobalDepsAnalyzerPass(Registry);
	initializeIdenticalCodeFoldingPass(Registry);
	initializePointerAnalyzerPass(Registry);
	initializeRegisterizePass(Registry);
	initializeStructMemFuncLoweringPass(Registry);
	initializeReplaceNopCastsAndByteSwapsPass(Registry);
	initializeTypeOptimizerPass(Registry);
	initializeDelayInstsPass(Registry);
	initializePreExecutePass(Registry);
	initializeExpandStructRegsPass(Registry);
	initializeFreeAndDeleteRemovalPass(Registry);
	initializeGEPOptimizerPass(Registry);
	initializeAllocaStoresExtractorPass(Registry);
	initializeFixIrreducibleControlFlowPass(Registry);
}

}
