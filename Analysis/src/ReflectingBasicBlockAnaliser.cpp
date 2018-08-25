#include "input-dependency/Analysis/ReflectingBasicBlockAnaliser.h"

#include "input-dependency/Analysis/IndirectCallSitesAnalysis.h"
#include "input-dependency/Analysis/value_dependence_graph.h"

#include "llvm/ADT/SCCIterator.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/OperandTraits.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace input_dependency {

namespace {

void resolve_value_to_input_dep(ValueDepInfo& to_resolve)
{
    to_resolve.updateCompositeValueDep(DepInfo(DepInfo::INPUT_DEP));
}

void resolve_value(ValueDepInfo& to_resolve, const std::vector<llvm::Value*>& depends_on_vals, const DepInfo& dep_info)
{
    if (dep_info.isInputDep()) {
        resolve_value_to_input_dep(to_resolve);
        return;
    }
    to_resolve.mergeDependencies(dep_info);
    std::for_each(depends_on_vals.begin(), depends_on_vals.end(),
                  [&to_resolve] (llvm::Value* val) { if (!llvm::dyn_cast<llvm::GlobalVariable>(val)) {
                      to_resolve.getValueDependencies().erase(val);} });
    if (to_resolve.getDependency() == DepInfo::VALUE_DEP && to_resolve.getValueDependencies().empty()) {
        to_resolve.setDependency(dep_info.getDependency());
    } else {
        to_resolve.mergeDependency(dep_info.getDependency());
    }
    for (auto& elem_dep : to_resolve.getCompositeValueDeps()) {
        resolve_value(elem_dep, depends_on_vals, dep_info);
    }
}

void resolveCompundNodeDeps(value_dependence_graph::nodeT& node,
                            DependencyAnaliser::ValueDependencies& value_dependencies,
                            std::list<value_dependence_graph::nodeT>& leaves)
{
    const auto& node_values = node->get_values();
    bool is_input_dep = false;
    ArgumentSet all_arguments;
    ValueSet all_values;
    DepInfo::Dependency dep = DepInfo::UNKNOWN;
    for (auto& node_val : node_values) {
        auto val_pos = value_dependencies.find(node_val);
        assert(val_pos != value_dependencies.end());
        auto& val_dep = val_pos->second.getValueDep();
        if (val_dep.isInputDep()) {
            is_input_dep = true;
            break;
        }
        const auto& args = val_dep.getArgumentDependencies();
        all_arguments.insert(args.begin(), args.end());
        const auto& values = val_dep.getValueDependencies();
        all_values.insert(values.begin(), values.end());
        dep = std::max(dep, val_dep.getDependency());
    }
    if (is_input_dep) {
        for (auto node_val : node_values) {
            auto val_pos = value_dependencies.find(node_val);
            resolve_value_to_input_dep(val_pos->second);
        }
        for (auto& dep_node : node->get_dependent_values()) {
            if (dep_node->is_root()) {
                dep_node->remove_depends_on(node);
                continue;
            }
            auto& dep_values = dep_node->get_values();
            for (auto& dep_val : dep_values) {
                auto dep_val_pos = value_dependencies.find(dep_val);
                assert(dep_val_pos != value_dependencies.end());
                resolve_value_to_input_dep(dep_val_pos->second);
            }
            dep_node->clear_depends_on_values();
            leaves.push_front(dep_node);
        }
    } else {
        // all values contain values in a cycle, remove those
        // TODO: case of globals
        std::for_each(node_values.begin(), node_values.end(), [&all_values] (llvm::Value* val) { all_values.erase(val); });
        if (dep == DepInfo::VALUE_DEP && all_values.empty()) {
            dep = DepInfo::INPUT_INDEP;
        }
        DepInfo dep_info(dep);
        dep_info.setArgumentDependencies(all_arguments);
        dep_info.setValueDependencies(all_values);

        for (auto node_val : node_values) {
            auto val_pos = value_dependencies.find(node_val);
            assert(!val_pos->second.getValueDep().isInputDep());
            // Note this may make input-indep element to input dep
            val_pos->second.updateCompositeValueDep(dep_info);
        }
        for (auto& dep_node : node->get_dependent_values()) {
            dep_node->remove_depends_on(node);
            if (dep_node->is_root()) {
                continue;
            }
            auto remove_values = node_values;
            if (dep_node->is_compound()) {
                for (auto& dep_val : dep_node->get_values()) {
                    remove_values.push_back(dep_val);
                    auto dep_val_pos = value_dependencies.find(dep_val);
                    assert(dep_val_pos != value_dependencies.end());
                    resolve_value(dep_val_pos->second, remove_values, dep_info);
                }
            } else {
                auto dep_val = dep_node->get_value();
                auto dep_val_pos = value_dependencies.find(dep_val);
                assert(dep_val_pos != value_dependencies.end());
                remove_values.push_back(dep_val);
                resolve_value(dep_val_pos->second, remove_values, dep_info);
            }
            if (dep_node->is_leaf()) {
                leaves.push_front(dep_node);
            }
        }
    }
}

void resolveNodeDeps(value_dependence_graph::nodeT& node,
                     DependencyAnaliser::ValueDependencies& value_dependencies,
                     std::list<value_dependence_graph::nodeT>& leaves)
{
    llvm::Value* node_val = node->get_value();
    assert(node_val != nullptr);
    auto val_pos = value_dependencies.find(node->get_value());
    auto& val_dep = val_pos->second.getValueDep();
    if (!llvm::dyn_cast<llvm::GlobalVariable>(val_pos->first)) {
        val_dep.getValueDependencies().erase(val_pos->first);
    }
    if (val_dep.getValueDependencies().empty() && val_dep.isValueDep()) {
        val_dep.setDependency(DepInfo::INPUT_INDEP);
    }
    //assert(!val_dep.isValueDep() || val_dep.isOnlyGlobalValueDependent());
    for (auto& dep_node : node->get_dependent_values()) {
        dep_node->remove_depends_on(node);
        if (dep_node->is_root()) {
            continue;
        }
        auto dep_vals = dep_node->get_values();
        for (const auto& dep_val : dep_vals) {
            auto dep_val_pos = value_dependencies.find(dep_val);
            assert(dep_val_pos != value_dependencies.end());
            resolve_value(dep_val_pos->second, {val_pos->first, dep_val_pos->first}, val_dep);
        }
        if (val_dep.isInputDep()) {
            dep_node->clear_depends_on_values();
        }
        if (dep_node->is_leaf()) {
            leaves.push_front(dep_node);
        }
    }
}

void resolveDependencies(value_dependence_graph::node_set& nodes,
                         DependencyAnaliser::ValueDependencies& value_dependencies)
{
    std::unordered_set<value_dependence_graph::nodeT> processed;
    std::list<value_dependence_graph::nodeT> leaves(nodes.begin(), nodes.end());
    while (!leaves.empty()) {
        auto leaf = leaves.back();
        leaves.pop_back();
        if (!processed.insert(leaf).second) {
            continue;
        }
        if (leaf->is_root()) {
            continue;
        } else if (leaf->is_compound()) {
            resolveCompundNodeDeps(leaf, value_dependencies, leaves);
        } else {
            resolveNodeDeps(leaf, value_dependencies, leaves);
        }
    }
}

} // unnamed namespace

ReflectingBasicBlockAnaliser::ReflectingBasicBlockAnaliser(
                        llvm::Function* F,
                        llvm::AAResults& AAR,
                        const VirtualCallSiteAnalysisResult& virtualCallsInfo,
                        const IndirectCallSitesAnalysisResult& indirectCallsInfo,
                        const Arguments& inputs,
                        const FunctionAnalysisGetter& Fgetter,
                        llvm::BasicBlock* BB)
                    : BasicBlockAnalysisResult(F, AAR, virtualCallsInfo, indirectCallsInfo, inputs, Fgetter, BB)
                    , m_isReflected(false)
{
}

void ReflectingBasicBlockAnaliser::reflect(const DependencyAnaliser::ValueDependencies& dependencies,
                                           const DepInfo& mandatory_deps)
{
    resolveValueDependencies(dependencies, mandatory_deps);
    for (auto& item : m_valueDependencies) {
        if (!item.second.getValueDep().isDefined()) {
            continue;
        }
        reflect(item.first, item.second);
    }
    // TODO: would not need this part remove if all instructions are collected together in one map
    for (auto& instrDep : m_instructionValueDependencies) {
        assert(instrDep.second.isValueDep());
        if (instrDep.second.isValueDep() && instrDep.second.getValueDependencies().empty()) {
            m_inputIndependentInstrs.insert(instrDep.first);
        } else {
            m_inputDependentInstrs[instrDep.first].mergeDependencies(instrDep.second);
        }
    }
    m_instructionValueDependencies.clear();
    m_valueDependentInstrs.clear();

    assert(m_valueDependentInstrs.empty());
    m_valueDependentOutArguments.clear();
    m_valueDependentFunctionCallArguments.clear();
    assert(m_valueDependentFunctionCallArguments.empty());
    if (!m_valueDependentFunctionInvokeArguments.empty()) {
        for (const auto& val : m_valueDependentFunctionInvokeArguments) {
            assert(llvm::dyn_cast<llvm::GlobalVariable>(val.first));
        }
    }
    m_valueDependentFunctionInvokeArguments.clear();
    assert(m_valueDependentFunctionInvokeArguments.empty());
    m_isReflected = true;
}

void ReflectingBasicBlockAnaliser::addControlDependencies(ValueDepInfo& valueDepInfo)
{
}

void ReflectingBasicBlockAnaliser::addControlDependencies(DepInfo& depInfo)
{
}

DepInfo ReflectingBasicBlockAnaliser::getInstructionDependencies(llvm::Instruction* instr) const
{
    auto indeppos = m_inputIndependentInstrs.find(instr);
    if (indeppos != m_inputIndependentInstrs.end()) {
        return DepInfo(DepInfo::INPUT_INDEP);
    }
    auto valpos = m_instructionValueDependencies.find(instr);
    if (valpos != m_instructionValueDependencies.end()) {
        return valpos->second;
    }
    auto deppos = m_inputDependentInstrs.find(instr);
    if (deppos == m_inputDependentInstrs.end()) {
        return DepInfo(DepInfo::VALUE_DEP, ValueSet{instr});
    }
    assert(deppos != m_inputDependentInstrs.end());
    //assert(deppos->second.isInputDep() || deppos->second.isInputArgumentDep()
    //|| deppos->second.isOnlyGlobalValueDependent());
    return deppos->second;
}

void ReflectingBasicBlockAnaliser::setOutArguments(const ArgumentDependenciesMap& outArgs)
{
    BasicBlockAnalysisResult::setOutArguments(outArgs);
    for (const auto& out_arg : m_outArgDependencies) {
        for (const auto& value_dep : out_arg.second.getValueDependencies()) {
            if (!llvm::dyn_cast<llvm::GlobalVariable>(value_dep)) {
                m_valueDependentOutArguments[value_dep].insert(out_arg.first);
            }
        }
    }
}

DepInfo ReflectingBasicBlockAnaliser::getInstructionDependencies(llvm::Instruction* instr)
{
    auto deppos = m_inputDependentInstrs.find(instr);
    if (deppos != m_inputDependentInstrs.end()) {
        return deppos->second;
    }
    auto indeppos = m_inputIndependentInstrs.find(instr);
    if (indeppos != m_inputIndependentInstrs.end()) {
        return DepInfo(DepInfo::INPUT_INDEP);
    }
    auto valdeppos = m_instructionValueDependencies.find(instr);
    if (valdeppos != m_instructionValueDependencies.end()) {
        return valdeppos->second;
    }
    if (auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(instr)) {
        return getValueDependencies(allocaInst).getValueDep();
    }
    if (auto* loadInst = llvm::dyn_cast<llvm::LoadInst>(instr)) {
        return getLoadInstrDependencies(loadInst);
    }
    return determineInstructionDependenciesFromOperands(instr);

}

void ReflectingBasicBlockAnaliser::updateInstructionDependencies(llvm::Instruction* instr,
                                                                 const DepInfo& info)
{
    assert(info.isDefined());
    DepInfo localInfo = info;
    addControlDependencies(localInfo);
    auto* getElPtr = llvm::dyn_cast<llvm::GetElementPtrInst>(instr);
    DepInfo instrDepInfo = localInfo;
    if (getElPtr) {
        instrDepInfo.mergeDependencies(ValueSet{getElPtr->getOperand(0)});
    }
    if (instrDepInfo.isInputDep()) {
        m_inputDependentInstrs[instr] = DepInfo(DepInfo::INPUT_DEP);
    } else if (instrDepInfo.isValueDep()) {
        m_instructionValueDependencies[instr] = instrDepInfo;
        updateValueDependentInstructions(instrDepInfo, instr);
    } else if (instrDepInfo.isInputIndep()) {
        assert(instrDepInfo.getArgumentDependencies().empty());
        assert(instrDepInfo.getValueDependencies().empty());
        m_inputIndependentInstrs.insert(instr);
    } else {
        assert(instrDepInfo.isInputArgumentDep());
        m_inputDependentInstrs[instr] = instrDepInfo;
    }
    for (const auto& val : localInfo.getValueDependencies()) {
        auto pos = m_valueDependencies.find(val);
        if (pos == m_valueDependencies.end()) {
            auto initials_pos = m_initialDependencies.find(val);
            if (initials_pos != m_initialDependencies.end()) {
                m_valueDependencies[val] = initials_pos->second;
            }
        }
    }
}

void ReflectingBasicBlockAnaliser::updateAliasingOutArgDependencies(llvm::Value* val,
                                                                    const ValueDepInfo& info,
                                                                    int arg_idx)
{
    if (m_outArgDependencies.empty()) {
        return;
    }
    ValueDepInfo localInfo = info;
    addControlDependencies(localInfo);
    for (auto& arg : m_outArgDependencies) {
        if (arg_idx != -1 && arg_idx != arg.first->getArgNo()) {
            continue;
        }
        auto alias = m_AAR.alias(val, arg.first);
        if (alias == llvm::AliasResult::NoAlias) {
            continue;
        }
        //arg.second.updateValueDep(info);
        arg.second.mergeDependencies(localInfo);
        for (const auto& val : arg.second.getValueDependencies()) {
            m_valueDependentOutArguments[val].insert(arg.first);
            if (m_valueDependencies.find(val) == m_valueDependencies.end()) {
                m_valueDependencies[val] = m_initialDependencies[val];
            }
        }
    }
}

ValueDepInfo ReflectingBasicBlockAnaliser::getCompositeValueDependencies(llvm::Value* value, llvm::Instruction* element_instr)
{
    auto valueDepInfo = BasicBlockAnalysisResult::getCompositeValueDependencies(value, element_instr);
    valueDepInfo.getValueDependencies().insert(value);
    return valueDepInfo;
}

DepInfo ReflectingBasicBlockAnaliser::getLoadInstrDependencies(llvm::LoadInst* instr)
{
    auto* loadOp = instr->getPointerOperand();
    llvm::Value* loadedValue = getMemoryValue(loadOp);
    DepInfo info = BasicBlockAnalysisResult::getLoadInstrDependencies(instr);
    if (loadedValue == nullptr) {
        return info;
    }
    if (auto loadedInst = llvm::dyn_cast<llvm::Instruction>(loadedValue)) {
        auto alloca = llvm::dyn_cast<llvm::AllocaInst>(loadedInst);
        if (!alloca) {
            // or?
            info.mergeDependencies(getInstructionDependencies(loadedInst));
            return info;
        }
    }
    info.mergeDependencies(ValueSet{loadedValue});
    info.mergeDependency(DepInfo::VALUE_DEP);
    return info;
}

void ReflectingBasicBlockAnaliser::updateFunctionCallSiteInfo(llvm::CallInst* callInst, llvm::Function* F)
{
    BasicBlockAnalysisResult::updateFunctionCallSiteInfo(callInst, F);
    updateValueDependentCallArguments(callInst, F);
    updateValueDependentCallReferencedGlobals(callInst, F);
}

void ReflectingBasicBlockAnaliser::updateFunctionInvokeSiteInfo(llvm::InvokeInst* invokeInst, llvm::Function* F)
{
    BasicBlockAnalysisResult::updateFunctionInvokeSiteInfo(invokeInst, F);
    updateValueDependentInvokeArguments(invokeInst, F);
    updateValueDependentInvokeReferencedGlobals(invokeInst, F);
}

void ReflectingBasicBlockAnaliser::updateValueDependentInstructions(const DepInfo& info,
                                                                    llvm::Instruction* instr)
{
    for (const auto& val : info.getValueDependencies()) {
        m_valueDependentInstrs[val].insert(instr);
    }
}

void ReflectingBasicBlockAnaliser::updateValueDependentCallArguments(llvm::CallInst* callInst, llvm::Function* F)
{
    assert(F != nullptr);
    auto pos = m_functionCallInfo.find(F);
    if (pos == m_functionCallInfo.end()) {
        // is this possible?
        return;
    }
    const auto& dependencies = pos->second.getArgumentDependenciesForCall(callInst);
    for (const auto& dep : dependencies) {
        if (!dep.second.isValueDep()) {
            continue;
        }
        for (const auto& val : dep.second.getValueDependencies()) {
            m_valueDependentFunctionCallArguments[val][callInst].insert(dep.first);
            if (!llvm::dyn_cast<llvm::GlobalVariable>(val) && m_valueDependencies.find(val) == m_valueDependencies.end()) {
                m_valueDependencies[val] = m_initialDependencies[val];
            }
        }
    }
}

void ReflectingBasicBlockAnaliser::updateValueDependentInvokeArguments(llvm::InvokeInst* invokeInst, llvm::Function* F)
{
    assert(F != nullptr);
    auto pos = m_functionCallInfo.find(F);
    assert(pos != m_functionCallInfo.end());
    const auto& dependencies = pos->second.getArgumentDependenciesForInvoke(invokeInst);
    for (const auto& dep : dependencies) {
        if (!dep.second.isValueDep()) {
            continue;
        }
        for (const auto& val : dep.second.getValueDependencies()) {
            m_valueDependentFunctionInvokeArguments[val][invokeInst].insert(dep.first);
        }
    }
}

void ReflectingBasicBlockAnaliser::updateValueDependentCallReferencedGlobals(llvm::CallInst* callInst, llvm::Function* F)
{
    assert(F != nullptr);
    auto pos = m_functionCallInfo.find(F);
    assert(pos != m_functionCallInfo.end());
    const auto& dependencies = pos->second.getGlobalsDependenciesForCall(callInst);
    for (const auto& dep : dependencies) {
        if (!dep.second.isValueDep()) {
            continue;
        }
        for (const auto& val : dep.second.getValueDependencies()) {
            m_valueDependentCallGlobals[val][callInst].insert(dep.first);
        }
    }
}

void ReflectingBasicBlockAnaliser::updateValueDependentInvokeReferencedGlobals(llvm::InvokeInst* invokeInst, llvm::Function* F)
{
    assert(F != nullptr);
    auto pos = m_functionCallInfo.find(F);
    assert(pos != m_functionCallInfo.end());
    const auto& dependencies = pos->second.getGlobalsDependenciesForInvoke(invokeInst);
    for (const auto& dep : dependencies) {
        if (!dep.second.isValueDep()) {
            continue;
        }
        for (const auto& val : dep.second.getValueDependencies()) {
            m_valueDependentInvokeGlobals[val][invokeInst].insert(dep.first);
        }
    }
}

void ReflectingBasicBlockAnaliser::reflect(llvm::Value* value, const ValueDepInfo& deps)
{
    assert(deps.isDefined());
    if (deps.isValueDep()) {
        assert(deps.isOnlyGlobalValueDependent());
    }
    reflectOnInstructions(value, deps); // need to go trough instructions one more time and add to correspoinding set
    reflectOnOutArguments(value, deps.getValueDep());
    reflectOnCalledFunctionArguments(value, deps.getValueDep());
    reflectOnCalledFunctionReferencedGlobals(value, deps.getValueDep());
    reflectOnInvokedFunctionArguments(value, deps.getValueDep());
    reflectOnInvokedFunctionReferencedGlobals(value, deps.getValueDep());
    reflectOnReturnValue(value, deps.getValueDep());
}

void ReflectingBasicBlockAnaliser::reflectOnInstructions(llvm::Value* value, const ValueDepInfo& depInfo)
{
    auto instrDepPos = m_valueDependentInstrs.find(value);
    if (instrDepPos == m_valueDependentInstrs.end()) {
        return;
    }
    for (const auto& instr : instrDepPos->second) {
        auto instrPos = m_instructionValueDependencies.find(instr);
        assert(instrPos != m_instructionValueDependencies.end());
        reflectOnDepInfo(value, instrPos->second, depInfo.getValueDep(instr).getValueDep());
        assert(instrPos->second.isDefined());
        if (instrPos->second.isValueDep() && !instrPos->second.isOnlyGlobalValueDependent()) {
            continue;
        }
        if (instrPos->second.isOnlyGlobalValueDependent()) {
            m_inputDependentInstrs[instr].mergeDependencies(instrPos->second);
            continue;
        }
        if (instrPos->second.isInputDep() || instrPos->second.isInputArgumentDep()) {
            m_inputDependentInstrs[instr].mergeDependencies(instrPos->second.getArgumentDependencies());
            m_inputDependentInstrs[instr].mergeDependency(instrPos->second.getDependency());
        } else if (instrPos->second.isInputIndep()) {
            m_inputIndependentInstrs.insert(instr);
        }
        m_instructionValueDependencies.erase(instrPos);
    }
    m_valueDependentInstrs.erase(instrDepPos);
}

void ReflectingBasicBlockAnaliser::reflectOnOutArguments(llvm::Value* value, const DepInfo& depInfo)
{
    auto outArgPos = m_valueDependentOutArguments.find(value);
    if (outArgPos == m_valueDependentOutArguments.end()) {
        return;
    }
    for (const auto& outArg : outArgPos->second) {
        auto argPos = m_outArgDependencies.find(outArg);
        assert(argPos != m_outArgDependencies.end());
        reflectOnDepInfo(value, argPos->second, depInfo);
    }
    m_valueDependentOutArguments.erase(value);
}

void ReflectingBasicBlockAnaliser::reflectOnCalledFunctionArguments(llvm::Value* value, const DepInfo& depInfo)
{
    auto valPos = m_valueDependentFunctionCallArguments.find(value);
    if (valPos == m_valueDependentFunctionCallArguments.end()) {
        return;
    }

    for (const auto& fargs : valPos->second) {
        auto callInst = fargs.first;
        FunctionSet targets;
        auto calledF = callInst->getCalledFunction();
        if (calledF == nullptr) {
            if (m_virtualCallsInfo.hasVirtualCallCandidates(callInst)) {
                targets = m_virtualCallsInfo.getVirtualCallCandidates(callInst);
            } else if (m_indirectCallsInfo.hasIndirectTargets(callInst)) {
                targets = m_indirectCallsInfo.getIndirectTargets(callInst);
            } else {
                continue;
            }
        } else {
            targets.insert(calledF);
        }
        for (auto& F : targets) {
            auto Fpos = m_functionCallInfo.find(F);
            assert(Fpos != m_functionCallInfo.end());
            auto& callDeps = Fpos->second.getArgumentDependenciesForCall(callInst);
            for (auto& arg : fargs.second) {
                auto argPos = callDeps.find(arg);
                if (argPos == callDeps.end()) {
                    continue;
                }
                assert(argPos != callDeps.end());
                reflectOnDepInfo(value, argPos->second, depInfo);
                // TODO: need to delete if becomes input indep?
            }
        }
    }
    m_valueDependentFunctionCallArguments.erase(valPos);
}

void ReflectingBasicBlockAnaliser::reflectOnCalledFunctionReferencedGlobals(llvm::Value* value, const DepInfo& depInfo)
{
    auto valPos = m_valueDependentCallGlobals.find(value);
    if (valPos == m_valueDependentCallGlobals.end()) {
        return;
    }
 
    for (const auto& fargs : valPos->second) {
        auto callInst = fargs.first;
        FunctionSet targets;
        auto calledF = callInst->getCalledFunction();
        if (calledF == nullptr) {
            if (m_virtualCallsInfo.hasVirtualCallCandidates(callInst)) {
                targets = m_virtualCallsInfo.getVirtualCallCandidates(callInst);
            } else if (m_indirectCallsInfo.hasIndirectTargets(callInst)) {
                targets = m_indirectCallsInfo.getIndirectTargets(callInst);
            } else {
                continue;
            }
        } else {
            targets.insert(calledF);
        }

        for (auto& F : targets) {
            auto Fpos = m_functionCallInfo.find(F);
            if (Fpos == m_functionCallInfo.end()) {
                continue;
            }
            assert(Fpos != m_functionCallInfo.end());
            auto& callDeps = Fpos->second.getGlobalsDependenciesForCall(callInst);
            for (auto& global : fargs.second) {
                auto globPos = callDeps.find(global);
                if (globPos == callDeps.end()) {
                    continue;
                }
                assert(globPos != callDeps.end());
                reflectOnDepInfo(value, globPos->second, depInfo);
            }
        }
    }
    m_valueDependentCallGlobals.erase(valPos);
}

void ReflectingBasicBlockAnaliser::reflectOnInvokedFunctionArguments(llvm::Value* value, const DepInfo& depInfo)
{
    auto valPos = m_valueDependentFunctionInvokeArguments.find(value);
    if (valPos == m_valueDependentFunctionInvokeArguments.end()) {
        return;
    }

    for (const auto& fargs : valPos->second) {
        auto invokeInst = fargs.first;
        auto invokedF = invokeInst->getCalledFunction();
        FunctionSet targets;
        if (invokedF == nullptr) {
            if (m_virtualCallsInfo.hasVirtualCallCandidates(invokeInst)) {
                targets = m_virtualCallsInfo.getVirtualCallCandidates(invokeInst);
            } else if (m_indirectCallsInfo.hasIndirectTargets(invokeInst)) {
                targets = m_indirectCallsInfo.getIndirectTargets(invokeInst);
            } else {
                continue;
            }
        } else {
            targets.insert(invokedF);
        }
        for (auto& F : targets) {
            auto Fpos = m_functionCallInfo.find(F);
            assert(Fpos != m_functionCallInfo.end());
            auto& invokeDeps = Fpos->second.getArgumentDependenciesForInvoke(invokeInst);
            for (auto& arg : fargs.second) {
                auto argPos = invokeDeps.find(arg);
                assert(argPos != invokeDeps.end());
                reflectOnDepInfo(value, argPos->second, depInfo);
                // TODO: need to delete if becomes input indep?
            }
        }
    }
    m_valueDependentFunctionInvokeArguments.erase(valPos);
}

void ReflectingBasicBlockAnaliser::reflectOnInvokedFunctionReferencedGlobals(llvm::Value* value, const DepInfo& depInfo)
{
    auto valPos = m_valueDependentInvokeGlobals.find(value);
    if (valPos == m_valueDependentInvokeGlobals.end()) {
        return;
    }

    for (const auto& fargs : valPos->second) {
        auto invokeInst = fargs.first;
        auto F = invokeInst->getCalledFunction();
        auto Fpos = m_functionCallInfo.find(F);
        if (Fpos == m_functionCallInfo.end()) {
            return;
        }
        assert(Fpos != m_functionCallInfo.end());
        auto& invokeDeps = Fpos->second.getGlobalsDependenciesForInvoke(invokeInst);
        for (auto& arg : fargs.second) {
            auto argPos = invokeDeps.find(arg);
            assert(argPos != invokeDeps.end());
            reflectOnDepInfo(value, argPos->second, depInfo);
        }
    }
    m_valueDependentInvokeGlobals.erase(valPos);
}

void ReflectingBasicBlockAnaliser::reflectOnReturnValue(llvm::Value* value, const DepInfo& depInfo)
{
    if (!m_returnValueDependencies.isValueDep()) {
        return;
    }
    auto pos = m_returnValueDependencies.getValueDependencies().find(value);
    if (pos == m_returnValueDependencies.getValueDependencies().end()) {
        return;
    }
    reflectOnDepInfo(value, m_returnValueDependencies, depInfo);
}

void ReflectingBasicBlockAnaliser::reflectOnDepInfo(llvm::Value* value,
                                                    DepInfo& depInfoTo,
                                                    const DepInfo& depInfoFrom,
                                                    bool eraseAfterReflection)
{
    // note: this won't change pos dependency, if it is of maximum value input_dep
    if (!depInfoTo.isValueDep()) {
        return;
    }
    assert(depInfoTo.isValueDep());
    if (depInfoTo.getDependency() == DepInfo::VALUE_DEP) {
        depInfoTo.setDependency(depInfoFrom.getDependency());
    }
    depInfoTo.mergeDependencies(depInfoFrom);
    if (!eraseAfterReflection) {
        return;
    }
    auto& valueDeps = depInfoTo.getValueDependencies();
    auto valPos = valueDeps.find(value);
    if (valPos != valueDeps.end()) {
        const auto& valueDepsFrom = depInfoFrom.getValueDependencies();
        if (!llvm::dyn_cast<llvm::GlobalVariable>(value) || !(valueDepsFrom.size() == 1 && valueDepsFrom.find(value) !=
            valueDepsFrom.end())) {
            valueDeps.erase(valPos);
        }
    }
}

void ReflectingBasicBlockAnaliser::reflectOnDepInfo(llvm::Value* value,
                                                    ValueDepInfo& depInfoTo,
                                                    const DepInfo& depInfoFrom,
                                                    bool eraseAfterReflection)
{
    reflectOnDepInfo(value, depInfoTo.getValueDep(), depInfoFrom, eraseAfterReflection);
    for (auto& elem_info : depInfoTo.getCompositeValueDeps()) {
        reflectOnDepInfo(value, elem_info, depInfoFrom, eraseAfterReflection);
    }
}


void ReflectingBasicBlockAnaliser::resolveValueDependencies(const DependencyAnaliser::ValueDependencies& successorDependencies,
                                                            const DepInfo& mandatory_deps)
{
    for (auto& val_dep : m_valueDependencies) {
        val_dep.second.getValueDep().mergeDependencies(mandatory_deps);
    }
    for (const auto& dep : successorDependencies) {
        auto res = m_valueDependencies.insert(dep);
        if (!res.second) {
            res.first->second.getValueDep().mergeDependencies(dep.second.getValueDep());
        }
    }
    
    value_dependence_graph graph;
    graph.build(m_valueDependencies, m_initialDependencies);

    // write dot for value dependency graph
    //if (m_BB->getParent()->getName() == "id3_compat_fixup") {
    //    std::string name = m_BB->getParent()->getName();
    //    name += "_";
    //    name += m_BB->getName();
    //    graph.dump(name);
    //}

    resolveDependencies(graph.get_leaves(), m_valueDependencies);
    for (auto& item : m_valueDependencies) {
        if (item.second.isValueDep() && !item.second.isOnlyGlobalValueDependent()) {
            auto& value_dependencies = item.second.getValueDependencies();
            std::vector<llvm::Value*> to_erase;
            for (const auto& value : value_dependencies) {
                if (llvm::dyn_cast<llvm::GlobalVariable>(value)) {
                    continue;
                }
                auto pos = m_valueDependencies.find(value);
                if (pos == m_valueDependencies.end()) {
                    to_erase.push_back(value);
                    continue;
                }
                if (pos->second.isInputDep()) {
                    item.second.setDependency(DepInfo::INPUT_DEP);
                    value_dependencies.clear();
                    break;
                }
                item.second.mergeDependencies(pos->second.getArgumentDependencies());
                item.second.mergeDependency(pos->second.getDependency());
                to_erase.push_back(value);
            }
            std::for_each(to_erase.begin(), to_erase.end(),
                          [&value_dependencies] (llvm::Value* val) {value_dependencies.erase(val);});
            if (value_dependencies.empty() && item.second.getDependency() == DepInfo::VALUE_DEP) {
                item.second.setDependency(DepInfo::INPUT_INDEP);
            }
        }
        assert(!item.second.isValueDep() || item.second.isOnlyGlobalValueDependent());
//        if (auto* getElPtr = llvm::dyn_cast<llvm::GetElementPtrInst>(item.first)) {
//            llvm::Value* compositeValue = getElPtr->getOperand(0);
//            updateCompositeValueDependencies(compositeValue, getElPtr, item.second.getValueDep());
//        }
    }
}

DepInfo ReflectingBasicBlockAnaliser::getValueFinalDependencies(llvm::Value* value, ValueSet& processed)
{
    auto pos = m_valueDependencies.find(value);
    if (pos == m_valueDependencies.end()) {
        assert(llvm::dyn_cast<llvm::GlobalVariable>(value));
        processed.insert(value);
        return DepInfo(DepInfo::VALUE_DEP, ValueSet{value});
    }
    assert(pos != m_valueDependencies.end());
    auto& valDep = pos->second.getValueDep();
    if (valDep.getValueDependencies().empty()) {
        processed.insert(value);
        return DepInfo(valDep.getDependency(), ValueSet{value});
    }
    DepInfo depInfo(valDep.getDependency());
    for (auto val : valDep.getValueDependencies()) {
        if (val == value) {
            // ???
            processed.insert(value);
            depInfo.mergeDependencies(ValueSet{value});
            continue;
        }
        if (processed.find(val) != processed.end()) {
            continue;
        }
        processed.insert(value);
        const auto& deps = getValueFinalDependencies(val, processed);
        depInfo.mergeDependencies(deps);
    }
    return depInfo;
}


} // namespace input_dependency

