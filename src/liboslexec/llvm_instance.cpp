/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "llvm_headers.h"
#include <llvm/Analysis/Verifier.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/UnifyFunctionExitNodes.h>

#include "oslexec_pvt.h"
#include "oslops.h"
#include "../liboslcomp/oslcomp_pvt.h"
#include "runtimeoptimize.h"

/*
This whole file is concerned with taking our post-optimized OSO
intermediate code and translating it into LLVM IR code so we can JIT it
and run it directly, for an expected huge speed gain over running our
interpreter.

Schematically, we want to create code that resembles the following:

    // Assume 2 layers. 
    struct GroupData_1 {
        // Array of ints telling if we have already run each layer
        int layer_run[nlayers];
        // For each layer in the group, we declare all shader params
        // whose values are not known -- they have init ops, or are
        // interpolated from the geom, or are connected to other layers.
        float param_0_foo;   // number is layer ID
        float param_1_bar;
    };

    // Name of layer entry is $layer_ID
    void $layer_0 (SingleShaderGlobal *sg, GroupData_1 *group)
    {
        // Only run if not already done.  Then mark as run.
        if (group->layer_run[0])
            return;
        group->layer_run[0] = 1;

        // Declare locals, temps, constants, params with known values.
        // Make them all look like stack memory locations:
        float *x = alloca (sizeof(float));
        // ...and so on for all the other locals & temps...

        // then run the shader body:
        *x = sg->u * group->param_2_bar;
        group->param_1_foo = *x;
    }

    void $layer_0 (SingleShaderGlobal *sg, GroupData_1 *group)
    {
        if (group->layer_run[0])
            return;
        group->layer_run[0] = 1;
        // ...
        $layer_0 (sg, group);    // because we need its outputs
        *y = sg->u * group->$param_2_bar;
    }

    void $group_1 (SingleShaderGlobal *sg, GroupData_1 *group)
    {
        group->layer_run[...] = 0;
        // Run just the unconditional layers
        $layer_1 (sg, group);
    }

*/

extern int osl_llvm_compiled_ops_size;
extern char osl_llvm_compiled_ops_block[];

using namespace OSL;
using namespace OSL::pvt;

#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {


static ustring op_abs("abs");
static ustring op_bitand("bitand");
static ustring op_bitor("bitor");
static ustring op_ceil("ceil");
static ustring op_color("color");
static ustring op_compl("compl");
static ustring op_dowhile("dowhile");
static ustring op_end("end");
static ustring op_eq("eq");
static ustring op_error("error");
static ustring op_fabs("fabs");
static ustring op_floor("floor");
static ustring op_for("for");
static ustring op_format("format");
static ustring op_ge("ge");
static ustring op_gt("gt");
static ustring op_if("if");
static ustring op_le("le");
static ustring op_lt("lt");
static ustring op_min("min");
static ustring op_neq("neq");
static ustring op_nop("nop");
static ustring op_normal("normal");
static ustring op_point("point");
static ustring op_printf("printf");
static ustring op_round("round");
static ustring op_shl("shl");
static ustring op_shr("shr");
static ustring op_step("step");
static ustring op_trunc("trunc");
static ustring op_vector("vector");
static ustring op_warning("warning");
static ustring op_xor("xor");



/// Macro that defines the arguments to LLVM IR generating routines
///
#define LLVMGEN_ARGS     RuntimeOptimizer &rop, int opnum

/// Macro that defines the full declaration of an LLVM generator.
/// 
#define LLVMGEN(name)  bool name (LLVMGEN_ARGS)

/// Function pointer to an LLVM IR-generating routine
///
typedef bool (*OpLLVMGen) (LLVMGEN_ARGS);



/// Table of all functions that we may call from the LLVM-compiled code.
/// Alternating name and argument list, much like we use in oslc's type
/// checking.  Note that nothing that's compiled into llvm_ops.cpp ought
/// to need a declaration here.
static const char *llvm_helper_function_table[] = {
    "osl_closure_clear", "xC",
    "osl_closure_assign", "xCC",
    "osl_add_closure_closure", "xCCC",
    "osl_mul_closure_float", "xCCf",
    "osl_mul_closure_color", "xCCc",

    NULL
};



const llvm::Type *
RuntimeOptimizer::llvm_type_sg ()
{
    if (m_llvm_type_sg)
        return m_llvm_type_sg;

    // Derivs look like arrays of 3 values
    const llvm::Type *float_deriv = llvm_type (TypeDesc(TypeDesc::FLOAT, TypeDesc::SCALAR, 3));
    const llvm::Type *triple_deriv = llvm_type (TypeDesc(TypeDesc::FLOAT, TypeDesc::VEC3, 3));
    std::vector<const llvm::Type*> sg_types;
    sg_types.push_back (triple_deriv);        // P, dPdx, dPdy
    sg_types.push_back (triple_deriv);        // I, dIdx, dIdy
    sg_types.push_back (llvm_type_triple());  // N
    sg_types.push_back (llvm_type_triple());  // Ng
    sg_types.push_back (float_deriv);         // u, dudx, dudy
    sg_types.push_back (float_deriv);         // v, dvdx, dvdy
    sg_types.push_back (llvm_type_triple());  // dPdu
    sg_types.push_back (llvm_type_triple());  // dPdv
    sg_types.push_back (llvm_type_float());   // time
    sg_types.push_back (llvm_type_float());   // dtime
    sg_types.push_back (llvm_type_triple());  // dPdtime
    sg_types.push_back (triple_deriv);        // Ps

    sg_types.push_back(llvm_type_void_ptr()); // opaque render state*
    sg_types.push_back(llvm_type_void_ptr()); // ShadingContext*
    sg_types.push_back(llvm_type_void_ptr()); // object2common
    sg_types.push_back(llvm_type_void_ptr()); // shader2common
    sg_types.push_back(llvm_type_void_ptr()); // Ci

    sg_types.push_back (llvm_type_float());   // surfacearea
    sg_types.push_back (llvm_type_int());     // iscameraray
    sg_types.push_back (llvm_type_int());     // isshadowray
    sg_types.push_back (llvm_type_int());     // flipHandedness

    return m_llvm_type_sg = llvm::StructType::get (llvm_context(), sg_types);
}



const llvm::Type *
RuntimeOptimizer::llvm_type_sg_ptr ()
{
    return llvm::PointerType::get (llvm_type_sg(), 0);
}



const llvm::Type *
RuntimeOptimizer::llvm_type_groupdata ()
{
    // If already computed, return it
    if (m_llvm_type_groupdata)
        return m_llvm_type_groupdata;

    std::vector<const llvm::Type*> fields;

    // First, add the array that tells if each layer has run
    fields.push_back (llvm::ArrayType::get(llvm_type_int(), m_group.nlayers()));
    size_t offset = m_group.nlayers() * sizeof(int);

    // For each layer in the group, add entries for all params that are
    // connected or interpolated, and output params.  Also mark those
    // symbols with their offset within the group struct.
#ifdef DEBUG
    std::cerr << "Group param struct:\n";
#endif
    m_param_order_map.clear ();
    int order = 1;
    for (int layer = 0;  layer < m_group.nlayers();  ++layer) {
        ShaderInstance *inst = m_group[layer];
        BOOST_FOREACH (Symbol &sym, inst->symbols()) {
            if (sym.symtype() == SymTypeParam ||
                  sym.symtype() == SymTypeOutputParam) {
                TypeSpec ts = sym.typespec();
                int arraylen = std::max (1, sym.typespec().arraylength());
                int n = arraylen * (sym.has_derivs() ? 3 : 1);
                ts.make_array (n);
                fields.push_back (llvm_type (ts));

                // Alignment
                size_t align = sym.typespec().is_closure() ? sizeof(void*) :
                    sym.typespec().simpletype().basesize();
                if (offset & (align-1))
                    offset += align - (offset & (align-1));
#ifdef DEBUG
                std::cerr << "  " << inst->layername() << " " << sym.mangled()
                          << " " << ts.c_str() << ", field " << order 
                          << ", offset " << offset << "\n";
#endif
                sym.dataoffset ((int)offset);
                offset += n * int(sym.size());

                m_param_order_map[&sym] = order;
                ++order;
            }
        }
    }
    m_group.llvm_groupdata_size (offset);

    m_llvm_type_groupdata = llvm::StructType::get (llvm_context(), fields);

#ifdef DEBUG
    llvm::errs() << "\nGroup struct = " << *m_llvm_type_groupdata << "\n";
    llvm::errs() << "  size = " << offset << "\n";
#endif

    return m_llvm_type_groupdata;
}



const llvm::Type *
RuntimeOptimizer::llvm_type_groupdata_ptr ()
{
    return llvm::PointerType::get (llvm_type_groupdata(), 0);
}



/// Convert the name of a global (and its derivative index) into the
/// field number of the ShaderGlobals struct.
static int
ShaderGlobalNameToIndex (ustring name)
{
    static ustring fields[] = {
        Strings::P, Strings::I, Strings::N, Strings::Ng,
        Strings::u, Strings::v, Strings::dPdu, Strings::dPdv,
        Strings::time, Strings::dtime, Strings::dPdtime, Strings::Ps,
        ustring("renderstate"), ustring("shadingcontext"),
        ustring("object2common"), ustring("shader2common"),
        Strings::Ci,
        ustring("surfacearea"), ustring("iscameraray"),
        ustring("isshadowray"), ustring("flipHandedness")
    };

    for (int i = 0;  i < int(sizeof(fields)/sizeof(fields[0]));  ++i)
        if (name == fields[i])
            return i;
    return -1;
}



llvm::Value *
RuntimeOptimizer::llvm_constant (float f)
{
    return llvm::ConstantFP::get (llvm_context(), llvm::APFloat(f));
}



llvm::Value *
RuntimeOptimizer::llvm_constant (int i)
{
    return llvm::ConstantInt::get (llvm_context(), llvm::APInt(32,i));
}



llvm::Value *
RuntimeOptimizer::llvm_constant (ustring s)
{
    // Create a const size_t with the ustring contents
    size_t bits = sizeof(size_t)*8;
    llvm::Value *str = llvm::ConstantInt::get (llvm_context(),
                               llvm::APInt(bits,size_t(s.c_str()), true));
    // Then cast the int to a char*.
    return builder().CreateIntToPtr (str, llvm_type_string());
}



const llvm::Type *
RuntimeOptimizer::llvm_type (const TypeSpec &typespec)
{
    if (typespec.is_closure())
        return llvm_type_void_ptr();
    TypeDesc t = typespec.simpletype().elementtype();
    const llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = llvm_type_float();
    else if (t == TypeDesc::INT)
        lt = llvm_type_int();
    else if (t == TypeDesc::STRING)
        lt = llvm_type_string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = llvm_type_triple();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = llvm_type_matrix();
    else if (t == TypeDesc::NONE)
        lt = llvm_type_void();
    else {
        std::cerr << "Bad llvm_type(" << typespec.c_str() << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (typespec.is_array())
        lt = llvm::ArrayType::get (lt, typespec.simpletype().numelements());
    return lt;
}



const llvm::Type *
RuntimeOptimizer::llvm_pass_type (const TypeSpec &typespec)
{
    if (typespec.is_closure())
        return llvm_type_void_ptr();
    TypeDesc t = typespec.simpletype().elementtype();
    const llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = llvm_type_float();
    else if (t == TypeDesc::INT)
        lt = llvm_type_int();
    else if (t == TypeDesc::STRING)
        lt = llvm_type_string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = llvm_type_triple_ptr();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = llvm_type_matrix_ptr();
    else if (t == TypeDesc::NONE)
        lt = llvm_type_void();
    else if (t == TypeDesc::PTR)
        lt = llvm_type_void_ptr();
    else {
        std::cerr << "Bad llvm_pass_type(" << typespec.c_str() << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (t.arraylen) {
        ASSERT (0 && "should never pass an array directly as a parameter");
    }
    return lt;
}



void
RuntimeOptimizer::llvm_zero_derivs (const Symbol &sym)
{
    // FIXME - handle arrays
    if (sym.has_derivs() &&
            (sym.typespec().is_float() || sym.typespec().is_triple())) {
        int num_components = sym.typespec().simpletype().aggregate;
        llvm::Value *zero = llvm_constant (0.0f);
        for (int i = 0;  i < num_components;  ++i)
            llvm_store_value (zero, sym, 1, NULL, i);  // clear dx
        for (int i = 0;  i < num_components;  ++i)
            llvm_store_value (zero, sym, 2, NULL, i);  // clear dy
    }
}



/// Given the OSL symbol, return the llvm::Value* corresponding to the
/// start of that symbol (first element, first component, and just the
/// plain value if it has derivatives).
llvm::Value *
RuntimeOptimizer::getLLVMSymbolBase (const Symbol &sym)
{
    Symbol* dealiased = sym.dealias();

    if (sym.symtype() == SymTypeGlobal) {
        // Special case for globals -- they live in the shader globals struct
        int sg_index = ShaderGlobalNameToIndex (sym.name());
        ASSERT (sg_index >= 0);
        llvm::Value *result = builder().CreateConstGEP2_32 (sg_ptr(), 0, sg_index);
        // No derivs?  We're one indirection too few?
        result = builder().CreatePointerCast (result, llvm::PointerType::get(llvm_type(sym.typespec()), 0));
        return result;
    }

    if (sym.symtype() == SymTypeParam || sym.symtype() == SymTypeOutputParam) {
        // Special case for params -- they live in the group data
        int fieldnum = m_param_order_map[&sym];
        llvm::Value *result = builder().CreateConstGEP2_32 (groupdata_ptr(), 0,
                                                            fieldnum);
        // No derivs?  We're one indirection too few?
        result = builder().CreatePointerCast (result, llvm::PointerType::get(llvm_type(sym.typespec()), 0));
        return result;
    }

    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values().find (mangled_name);
    if (map_iter == named_values().end()) {
        shadingsys().error ("Couldn't find symbol '%s' (unmangled = '%s'). Did you forget to allocate it?",
                            mangled_name.c_str(), dealiased->name().c_str());
        return 0;
    }
    return map_iter->second;
}



llvm::Value *
RuntimeOptimizer::getOrAllocateLLVMSymbol (const Symbol& sym)
{
    Symbol* dealiased = sym.dealias();
    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values().find(mangled_name);

    if (map_iter == named_values().end()) {
        // shadingsys().info ("Making a type with %d %ss for symbol '%s'\n",
        //           total_size, sym.typespec().c_str(), mangled_name.c_str());
        const llvm::Type *alloctype = sym.typespec().is_array()
                                  ? llvm_type (sym.typespec().elementtype())
                                  : llvm_type (sym.typespec());
        int arraylen = std::max (1, sym.typespec().arraylength());
        int n = arraylen * (sym.has_derivs() ? 3 : 1);
        llvm::ConstantInt* numalloc = (llvm::ConstantInt*)llvm_constant(n);
        llvm::AllocaInst* allocation = builder().CreateAlloca(alloctype, numalloc, sym.mangled());

        // llvm::outs() << "Allocation = " << *allocation << "\n";
        named_values()[mangled_name] = allocation;
        return allocation;
    }
    return map_iter->second;
}



llvm::Value *
RuntimeOptimizer::llvm_get_pointer (const Symbol& sym, int deriv,
                                    llvm::Value *arrayindex)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0)
        ASSERT (0 && "llvm_get_pointer: ask for derivs when there aren't any");

    // Start with the initial pointer to the variable's memory location
    llvm::Value* result = getLLVMSymbolBase (sym);
    if (!result)
        return NULL;  // Error

    // If it's an array or we're dealing with derivatives, step to the
    // right element.
    TypeDesc t = sym.typespec().simpletype();
    if (t.arraylen || has_derivs) {
        int d = deriv * std::max(1,t.arraylen);
        if (arrayindex)
            arrayindex = builder().CreateAdd (arrayindex, llvm_constant(d));
        else
            arrayindex = llvm_constant(d);
        result = builder().CreateGEP (result, arrayindex);
    }

    return result;
}



llvm::Value *
RuntimeOptimizer::llvm_load_value (const Symbol& sym, int deriv,
                                   llvm::Value *arrayindex, int component,
                                   TypeDesc cast)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Regardless of what object this is, if it doesn't have derivs but
        // we're asking for them, return 0.  Integers don't have derivs
        // so we don't need to worry about that case.
        return llvm_constant (0.0f);
    }

    if (sym.is_constant()) {
        // Shortcut for simple float & int constants
        if (sym.typespec().is_float())
            return llvm_constant (*(float *)sym.data());
        if (sym.typespec().is_int())
            return llvm_constant (*(int *)sym.data());
    }

    // Start with the initial pointer to the value's memory location
    llvm::Value* result = llvm_get_pointer (sym, deriv, arrayindex);
    if (!result)
        return NULL;  // Error

    // Special case: a matrix is stored as a struct whose one field
    // is an array of the values, so it needs an extra GEP.
    TypeDesc t = sym.typespec().simpletype();
    if (t.aggregate == TypeDesc::MATRIX44)
        result = builder().CreateConstGEP2_32 (result, 0, 0);

    // If it's multi-component (triple or matrix), step to the right field
    if (! sym.typespec().is_closure() && t.aggregate > 1)
        result = builder().CreateConstGEP2_32 (result, 0, component);

    // Now grab the value
    result = builder().CreateLoad (result);

    // Handle int<->float type casting
    if (sym.typespec().is_floatbased() && cast == TypeDesc::TypeInt)
        result = llvm_float_to_int (result);
    else if (sym.typespec().is_int() && cast == TypeDesc::TypeFloat)
        result = llvm_int_to_float (result);

    return result;
}



llvm::Value *
RuntimeOptimizer::llvm_load_component_value (const Symbol& sym, int deriv,
                                             llvm::Value *component)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Regardless of what object this is, if it doesn't have derivs but
        // we're asking for them, return 0.  Integers don't have derivs
        // so we don't need to worry about that case.
        ASSERT (sym.typespec().is_floatbased() && 
                "can't ask for derivs of an int");
        return llvm_constant (0.0f);
    }

    // Start with the initial pointer to the value's memory location
    llvm::Value* result = llvm_get_pointer (sym, deriv);
    if (!result)
        return NULL;  // Error

    // Special case: a matrix is stored as a struct whose one field
    // is an array of the values, so it needs an extra GEP.
    TypeDesc t = sym.typespec().simpletype();
    if (t.aggregate == TypeDesc::MATRIX44)
        result = builder().CreateConstGEP2_32 (result, 0, 0);

    ASSERT (t.aggregate != TypeDesc::SCALAR);
    result = builder().CreateConstGEP1_32 (result, 0);  // Find the memory
    result = builder().CreateGEP (result, component);   // Find the component

    // Now grab the value
    return builder().CreateLoad (result);
}



bool
RuntimeOptimizer::llvm_store_value (llvm::Value* new_val, const Symbol& sym,
                                    int deriv, llvm::Value* arrayindex,
                                    int component)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Attempt to store deriv in symbol that doesn't have it is just a nop
        return true;
    }

    // Let llvm_get_pointer do most of the heavy lifting to get us a
    // pointer to where our data lives.
    llvm::Value *result = llvm_get_pointer (sym, deriv, arrayindex);
    if (!result)
        return false;  // Error

    // Special case: a matrix is stored as a struct whose one field
    // is an array of the values, so it needs an extra GEP.
    TypeDesc t = sym.typespec().simpletype();
    if (t.aggregate == TypeDesc::MATRIX44)
        result = builder().CreateConstGEP2_32 (result, 0, 0);

    // If it's multi-component (triple or matrix), step to the right field
    if (! sym.typespec().is_closure() && t.aggregate > 1)
        result = builder().CreateConstGEP2_32 (result, 0, component);

    // Finally, store the value.
    builder().CreateStore (new_val, result);
    return true;
}



bool
RuntimeOptimizer::llvm_store_component_value (llvm::Value* new_val,
                                              const Symbol& sym, int deriv,
                                              llvm::Value* component)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Attempt to store deriv in symbol that doesn't have it is just a nop
        return true;
    }

    // Let llvm_get_pointer do most of the heavy lifting to get us a
    // pointer to where our data lives.
    llvm::Value *result = llvm_get_pointer (sym, deriv);
    if (!result)
        return false;  // Error

    // Special case: a matrix is stored as a struct whose one field
    // is an array of the values, so it needs an extra GEP.
    TypeDesc t = sym.typespec().simpletype();
    if (t.aggregate == TypeDesc::MATRIX44)
        result = builder().CreateConstGEP2_32 (result, 0, 0);

    ASSERT (t.aggregate != TypeDesc::SCALAR);
    result = builder().CreateConstGEP1_32 (result, 0);  // Find the memory
    result = builder().CreateGEP (result, component);   // Find the component

    // Finally, store the value.
    builder().CreateStore (new_val, result);
    return true;
}



llvm::Value *
RuntimeOptimizer::layer_run_ptr (int layer)
{
    llvm::Value *layer_run = builder().CreateConstGEP2_32 (groupdata_ptr(), 0, 0);
    return builder().CreateConstGEP2_32 (layer_run, 0, layer);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name,
                                      llvm::Value **args, int nargs)
{
    llvm::Function *func = llvm_module()->getFunction (name);
    if (! func)
        std::cerr << "Couldn't find function " << name << "\n";
    ASSERT (func);
#if 0
    llvm::outs() << "llvm_call_function " << *func << "\n";
    llvm::outs() << "\nargs:\n";
    for (int i = 0;  i < nargs;  ++i)
        llvm::outs() << "\t" << *(args[i]) << "\n";
#endif
    return builder().CreateCall (func, args, args+nargs);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, 
                                      const Symbol **symargs, int nargs,
                                      bool deriv_ptrs)
{
    std::vector<llvm::Value *> valargs;
    valargs.resize ((size_t)nargs);
    for (int i = 0;  i < nargs;  ++i) {
        const Symbol &s = *(symargs[i]);
        if (s.typespec().is_closure())
            valargs[i] = loadLLVMValue (s);
        else if (s.typespec().simpletype().aggregate > 1 ||
                 (deriv_ptrs && s.has_derivs()))
            valargs[i] = llvm_void_ptr (s);
        else
            valargs[i] = loadLLVMValue (s);
    }
    return llvm_call_function (name, &valargs[0], (int)valargs.size());
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, const Symbol &A,
                                      bool deriv_ptrs)
{
    const Symbol *args[1];
    args[0] = &A;
    return llvm_call_function (name, args, 1, deriv_ptrs);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, const Symbol &A,
                                      const Symbol &B, bool deriv_ptrs)
{
    const Symbol *args[2];
    args[0] = &A;
    args[1] = &B;
    return llvm_call_function (name, args, 2, deriv_ptrs);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, const Symbol &A,
                                      const Symbol &B, const Symbol &C,
                                      bool deriv_ptrs)
{
    const Symbol *args[3];
    args[0] = &A;
    args[1] = &B;
    args[2] = &C;
    return llvm_call_function (name, args, 3, deriv_ptrs);
}



LLVMGEN (llvm_gen_useparam)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    // Prep the args that will be used for all earlier-layer invocations
    llvm::Value *args[2];
    args[0] = rop.sg_ptr ();
    args[1] = rop.groupdata_ptr ();

    // If we have multiple params needed on this statement, don't waste
    // time checking the same upstream layer more than once.
    std::vector<int> already_run;

    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol& sym = *rop.opargsym (op, i);
        int symindex = rop.inst()->arg (op.firstarg()+i);
        if (sym.valuesource() != Symbol::ConnectedVal)
            continue;  // Nothing to do

        for (int c = 0;  c < rop.inst()->nconnections();  ++c) {
            const Connection &con (rop.inst()->connection (c));
            // If the connection gives a value to this param
            if (con.dst.param == symindex) {
                if (std::find (already_run.begin(), already_run.end(), con.srclayer) != already_run.end())
                    continue;  // already ran that one
                // If the earlier layer it comes from has not yet
                // been executed, do so now.
                //if (! execlayers[con.srclayer].executed())
                //    run_connected_layer (con.srclayer);
                ShaderInstance *parent = rop.group()[con.srclayer];
                llvm::Value *executed = rop.builder().CreateLoad (rop.layer_run_ptr(con.srclayer));
                executed = rop.builder().CreateICmpEQ (executed, rop.llvm_constant(0));
                // Make code that looks like:
                //   if (groupdata->run[parentlayer] == 0) {
                //       parent_layer (sg, groupdata);
                //       groupdata->run[parentlayer] = 1;
                //   }
                llvm::BasicBlock *then_block = llvm::BasicBlock::Create (rop.llvm_context(), "", rop.layer_func());
                llvm::BasicBlock *after_block = llvm::BasicBlock::Create (rop.llvm_context(), "", rop.layer_func());
                rop.builder().CreateCondBr (executed, then_block, after_block);
                rop.builder().SetInsertPoint (then_block);
                std::string name = Strutil::format ("%s_%d", parent->layername().c_str(), parent->id());
                rop.llvm_call_function (name.c_str(), args, 2);
                rop.builder().CreateBr (after_block);
                rop.builder().SetInsertPoint (after_block);
                already_run.push_back (con.srclayer);  // mark it
            }
        }
    }
    return true;
}



// Used for printf, error, warning, format
LLVMGEN (llvm_gen_printf)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    // Prepare the args for the call
    
    // Which argument is the format string?  Usually 0, but for op
    // format(), the formatting string is argument #1.
    int format_arg = (op.opname() == "format" ? 1 : 0);
    Symbol& format_sym = *rop.opargsym (op, format_arg);

    std::vector<llvm::Value*> call_args;
    if (!format_sym.is_constant()) {
        rop.shadingsys().warning ("%s must currently have constant format\n",
                                  op.opname().c_str());
        return false;
    }

    // For some ops, we push the shader globals pointer
    if (op.opname() == op_printf || op.opname() == op_error ||
            op.opname() == op_warning)
        call_args.push_back (rop.sg_void_ptr());

    // We're going to need to adjust the format string as we go, but I'd
    // like to reserve a spot for the char*.
    size_t new_format_slot = call_args.size();
    call_args.push_back(NULL);

    ustring format_ustring = *((ustring*)format_sym.data());
    const char* format = format_ustring.c_str();
    std::string s;
    int arg = format_arg + 1;
    while (*format != '\0') {
        if (*format == '%') {
            if (format[1] == '%') {
                // '%%' is a literal '%'
                s += "%%";
                format += 2;  // skip both percentages
                continue;
            }
            const char *oldfmt = format;  // mark beginning of format
            while (*format &&
                   *format != 'c' && *format != 'd' && *format != 'e' &&
                   *format != 'f' && *format != 'g' && *format != 'i' &&
                   *format != 'm' && *format != 'n' && *format != 'o' &&
                   *format != 'p' && *format != 's' && *format != 'u' &&
                   *format != 'v' && *format != 'x' && *format != 'X')
                ++format;
            ++format; // Also eat the format char
            if (arg >= op.nargs()) {
                rop.shadingsys().error ("Mismatch between format string and arguments");
                return false;
            }

            std::string ourformat (oldfmt, format);  // straddle the format
            // Doctor it to fix mismatches between format and data
            Symbol& sym (*rop.opargsym (op, arg));
            if (sym.typespec().is_closure ()) {
                rop.shadingsys().warning ("symbol type for '%s' unsupported for %s\n", sym.mangled().c_str(), op.opname().c_str());
                return false;
            }
            TypeDesc simpletype (sym.typespec().simpletype());
            int num_components = simpletype.numelements() * simpletype.aggregate;
            // NOTE(boulos): Only for debug mode do the derivatives get printed...
            for (int i = 0; i < num_components; i++) {
                if (i != 0) s += " ";
                s += ourformat;

                llvm::Value* loaded = rop.loadLLVMValue (sym, i, 0);
                // NOTE(boulos): Annoyingly varargs makes it so that things need
                // to be upconverted from float to double (who knew?)
                if (sym.typespec().is_floatbased()) {
                    call_args.push_back (rop.builder().CreateFPExt(loaded, llvm::Type::getDoubleTy(rop.llvm_context())));
                } else {
                    call_args.push_back (loaded);
                }
            }
            ++arg;
        } else if (*format == '\\') {
            // Escape sequence
            ++format;  // skip the backslash
            switch (*format) {
            case 'n' : s += '\n';     break;
            case 'r' : s += '\r';     break;
            case 't' : s += '\t';     break;
            default:   s += *format;  break;  // Catches '\\' also!
            }
            ++format;
        } else {
            // Everything else -- just copy the character and advance
            s += *format++;
        }
    }

    // Some ops prepend things
    if (op.opname() == op_error || op.opname() == op_warning) {
        std::string prefix = Strutil::format ("Shader %s [%s]: ",
                                              op.opname().c_str(),
                                              rop.inst()->shadername().c_str());
        s = prefix + s;
    }

    // Now go back and put the new format string in its place
    call_args[new_format_slot] = rop.llvm_constant (s.c_str());

    // Construct the function name and call it.
    std::string opname = std::string("osl_") + op.opname().string();
    llvm::Value *ret = rop.llvm_call_function (opname.c_str(), &call_args[0],
                                               (int)call_args.size());

    // The format op returns a string value, put in in the right spot
    if (op.opname() == op_format)
        rop.llvm_store_value (ret, *rop.opargsym (op, 0));
    return true;
}



/// Convert a float llvm value to an integer.
///
llvm::Value *
RuntimeOptimizer::llvm_float_to_int (llvm::Value* fval)
{
    return builder().CreateFPToSI(fval, llvm_type_int());
}



/// Convert an integer llvm value to a float.
///
llvm::Value *
RuntimeOptimizer::llvm_int_to_float (llvm::Value* ival)
{
    return builder().CreateSIToFP(ival, llvm_type_float());
}



/// Generate IR code for simple a/b, but considering OSL's semantics
/// that x/0 = 0, not inf.
static llvm::Value *
llvm_make_safe_div (RuntimeOptimizer &rop, TypeDesc type,
                    llvm::Value *a, llvm::Value *b)
{
    if (type.basetype == TypeDesc::FLOAT) {
        llvm::Value *div = rop.builder().CreateFDiv (a, b);
        llvm::Value *zero = rop.llvm_constant (0.0f);
        llvm::Value *iszero = rop.builder().CreateFCmpOEQ (b, zero);
        return rop.builder().CreateSelect (iszero, zero, div);
    } else {
        llvm::Value *div = rop.builder().CreateSDiv (a, b);
        llvm::Value *zero = rop.llvm_constant (0);
        llvm::Value *iszero = rop.builder().CreateICmpEQ (b, zero);
        return rop.builder().CreateSelect (iszero, zero, div);
    }
}



LLVMGEN (llvm_gen_add)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    if (Result.typespec().is_closure()) {
        ASSERT (A.typespec().is_closure() && B.typespec().is_closure());
        rop.llvm_call_function ("osl_add_closure_closure", Result, A, B);
        return true;
    }

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    // The following should handle f+f, v+v, v+f, f+v, i+i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = is_float ? rop.builder().CreateFAdd(a, b)
                                  : rop.builder().CreateAdd(a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs() || B.has_derivs()) {
            for (int d = 1;  d <= 2;  ++d) {  // dx, dy
                for (int i = 0; i < num_components; i++) {
                    llvm::Value *a = rop.loadLLVMValue (A, i, d, type);
                    llvm::Value *b = rop.loadLLVMValue (B, i, d, type);
                    llvm::Value *r = rop.builder().CreateFAdd(a, b);
                    rop.storeLLVMValue (r, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_sub)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    ASSERT (! Result.typespec().is_closure() &&
            "subtraction of closures not supported");

    // The following should handle f-f, v-v, v-f, f-v, i-i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = is_float ? rop.builder().CreateFSub(a, b)
                                  : rop.builder().CreateSub(a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs() || B.has_derivs()) {
            for (int d = 1;  d <= 2;  ++d) {  // dx, dy
                for (int i = 0; i < num_components; i++) {
                    llvm::Value *a = rop.loadLLVMValue (A, i, d, type);
                    llvm::Value *b = rop.loadLLVMValue (B, i, d, type);
                    llvm::Value *r = rop.builder().CreateFSub(a, b);
                    rop.storeLLVMValue (r, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_mul)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    // multiplication involving closures
    if (Result.typespec().is_closure()) {
        if (A.typespec().is_closure() && B.typespec().is_float())
            rop.llvm_call_function ("osl_mul_closure_float", Result, A, B);
        else if (A.typespec().is_float() && B.typespec().is_closure())
            rop.llvm_call_function ("osl_mul_closure_float", Result, B, A);
        else if (A.typespec().is_closure() && B.typespec().is_color())
            rop.llvm_call_function ("osl_mul_closure_color", Result, A, B);
        else if (A.typespec().is_color() && B.typespec().is_closure())
            rop.llvm_call_function ("osl_mul_closure_color", Result, B, A);
        else
            ASSERT (0 && "Unknown closure multiplication variety");
        return true;
    }

    // multiplication involving matrices
    if (Result.typespec().is_matrix()) {
        if (A.typespec().is_float()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_mul_m_ff", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_mul_mf", Result, B, A);
            else ASSERT(0);
        } else if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_mul_mf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_mul_mm", Result, A, B);
            else ASSERT(0);
        } else ASSERT (0);
        if (Result.has_derivs())
            rop.llvm_zero_derivs (Result);
        return true;
    }

    // The following should handle f*f, v*v, v*f, f*v, i*i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = is_float ? rop.builder().CreateFMul(a, b)
                                  : rop.builder().CreateMul(a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs() || B.has_derivs()) {
            // Multiplication of duals: (a*b, a*b.dx + a.dx*b, a*b.dy + a.dy*b)
            for (int i = 0; i < num_components; i++) {
                llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
                llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
                llvm::Value *ax = rop.loadLLVMValue (A, i, 1, type);
                llvm::Value *bx = rop.loadLLVMValue (B, i, 1, type);
                llvm::Value *abx = rop.builder().CreateFMul(a, bx);
                llvm::Value *axb = rop.builder().CreateFMul(ax, b);
                llvm::Value *r = rop.builder().CreateFAdd(abx, axb);
                rop.storeLLVMValue (r, Result, i, 1);
            }

            for (int i = 0; i < num_components; i++) {
                llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
                llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
                llvm::Value *ay = rop.loadLLVMValue (A, i, 2, type);
                llvm::Value *by = rop.loadLLVMValue (B, i, 2, type);
                llvm::Value *aby = rop.builder().CreateFMul(a, by);
                llvm::Value *ayb = rop.builder().CreateFMul(ay, b);
                llvm::Value *r = rop.builder().CreateFAdd(aby, ayb);
                rop.storeLLVMValue (r, Result, i, 2);
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_div)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    ASSERT (! Result.typespec().is_closure());

    // division involving matrices
    if (Result.typespec().is_matrix()) {
        if (A.typespec().is_float()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_div_m_ff", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_div_fm", Result, A, B);
            else ASSERT (0);
        } else if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_div_mf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_div_mm", Result, A, B);
            else ASSERT (0);
        } else ASSERT (0);
        if (Result.has_derivs())
            rop.llvm_zero_derivs (Result);
        return true;
    }

    // The following should handle f/f, v/v, v/f, f/v, i/i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = llvm_make_safe_div (rop, type, a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs() || B.has_derivs()) {
            // Division of duals: (a/b, 1/b*(ax-a/b*bx), 1/b*(ay-a/b*by))
            for (int i = 0; i < num_components; i++) {
                llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
                llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
                llvm::Value *binv = llvm_make_safe_div (rop, type,
                                                   rop.llvm_constant(1.0f), b);
                llvm::Value *ax = rop.loadLLVMValue (A, i, 1, type);
                llvm::Value *bx = rop.loadLLVMValue (B, i, 1, type);
                llvm::Value *a_div_b = rop.builder().CreateFMul (a, binv);
                llvm::Value *a_div_b_mul_bx = rop.builder().CreateFMul (a_div_b, bx);
                llvm::Value *ax_minus_a_div_b_mul_bx = rop.builder().CreateFSub (ax, a_div_b_mul_bx);
                llvm::Value *r = rop.builder().CreateFMul (binv, ax_minus_a_div_b_mul_bx);
                rop.storeLLVMValue (r, Result, i, 1);
            }

            for (int i = 0; i < num_components; i++) {
                llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
                llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
                llvm::Value *binv = llvm_make_safe_div (rop, type,
                                                   rop.llvm_constant(1.0f), b);
                llvm::Value *ay = rop.loadLLVMValue (A, i, 2, type);
                llvm::Value *by = rop.loadLLVMValue (B, i, 2, type);
                llvm::Value *a_div_b = rop.builder().CreateFMul (a, binv);
                llvm::Value *a_div_b_mul_by = rop.builder().CreateFMul (a_div_b, by);
                llvm::Value *ay_minus_a_div_b_mul_by = rop.builder().CreateFSub (ay, a_div_b_mul_by);
                llvm::Value *r = rop.builder().CreateFMul (binv, ay_minus_a_div_b_mul_by);
                rop.storeLLVMValue (r, Result, i, 2);
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



/// Generate IR code for simple a mod b, but considering OSL's semantics
/// that x mod 0 = 0, not inf.
static llvm::Value *
llvm_make_safe_mod (RuntimeOptimizer &rop, TypeDesc type,
                    llvm::Value *a, llvm::Value *b)
{
    if (type.basetype == TypeDesc::FLOAT) {
        llvm::Value *mod = rop.builder().CreateFRem (a, b);
        llvm::Value *zero = rop.llvm_constant (0.0f);
        llvm::Value *iszero = rop.builder().CreateFCmpOEQ (b, zero);
        return rop.builder().CreateSelect (iszero, zero, mod);
    } else {
        llvm::Value *mod = rop.builder().CreateSRem (a, b);
        llvm::Value *zero = rop.llvm_constant (0);
        llvm::Value *iszero = rop.builder().CreateICmpEQ (b, zero);
        return rop.builder().CreateSelect (iszero, zero, mod);
    }
}



LLVMGEN (llvm_gen_mod)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;

    // The following should handle f%f, v%v, v%f, i%i
    // That's all that should be allowed by oslc.
    for (int i = 0; i < num_components; i++) {
        llvm::Value *a = rop.loadLLVMValue (A, i, 0, type);
        llvm::Value *b = rop.loadLLVMValue (B, i, 0, type);
        if (!a || !b)
            return false;
        llvm::Value *r = llvm_make_safe_mod (rop, type, a, b);
        rop.storeLLVMValue (r, Result, i, 0);
    }

    if (Result.has_derivs()) {
        ASSERT (is_float);
        if (A.has_derivs()) {
            // Modulus of duals: (a mod b, ax, ay)
            for (int d = 1;  d <= 2;  ++d) {
                for (int i = 0; i < num_components; i++) {
                    llvm::Value *deriv = rop.loadLLVMValue (A, i, d, type);
                    rop.storeLLVMValue (deriv, Result, i, d);
                }
            }
        } else {
            // Result has derivs, operands do not
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



LLVMGEN (llvm_gen_neg)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;
    for (int d = 0;  d < 3;  ++d) {  // dx, dy
        for (int i = 0; i < num_components; i++) {
            llvm::Value *a = rop.llvm_load_value (A, d, i, type);
            llvm::Value *r = is_float ? rop.builder().CreateFNeg(a)
                                      : rop.builder().CreateNeg(a);
            rop.llvm_store_value (r, Result, d, i);
        }
        if (! Result.has_derivs())
            break;
    }
    return true;
}



// Implementation for both min and max
LLVMGEN (llvm_gen_minmax)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& X = *rop.opargsym (op, 1);
    Symbol& Y = *rop.opargsym (op, 2);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;
    for (int i = 0; i < num_components; i++) {
        llvm::Value *x = rop.llvm_load_value (X, 0, i, type);
        llvm::Value *y = rop.llvm_load_value (Y, 0, i, type);
        llvm::Value *cond = NULL;
        if (op.opname() == op_min)
            cond = is_float ? rop.builder().CreateFCmpULT(x, y)
                            : rop.builder().CreateICmpSLT(x, y);
        else
            cond = is_float ? rop.builder().CreateFCmpULT(y, x)
                            : rop.builder().CreateICmpSLT(y, x);
        llvm::Value *val = rop.builder().CreateSelect (cond, x, y);
        rop.llvm_store_value (val, Result, 0, i);
        if (Result.has_derivs()) {
            for (int d = 1; d <= 2; ++d) {
                llvm::Value *xd = rop.llvm_load_value (X, d, i, type);
                llvm::Value *yd = rop.llvm_load_value (Y, d, i, type);
                llvm::Value *val = rop.builder().CreateSelect (cond, xd, yd);
                rop.llvm_store_value (val, Result, d, i);
            }
        }
    }
    return true;
}



// Implementation for clamp
LLVMGEN (llvm_gen_clamp)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& X = *rop.opargsym (op, 1);
    Symbol& Min = *rop.opargsym (op, 2);
    Symbol& Max = *rop.opargsym (op, 3);

    TypeDesc type = Result.typespec().simpletype();
    bool is_float = Result.typespec().is_floatbased();
    int num_components = type.aggregate;
    for (int i = 0; i < num_components; i++) {
        // First do the lower bound
        llvm::Value *val = rop.llvm_load_value (X, 0, i, type);
        llvm::Value *min = rop.llvm_load_value (Min, 0, i, type);
        llvm::Value *cond = is_float ? rop.builder().CreateFCmpULT(val, min)
                                     : rop.builder().CreateICmpSLT(val, min);
        val = rop.builder().CreateSelect (cond, min, val);
        llvm::Value *valdx=NULL, *valdy=NULL;
        if (Result.has_derivs()) {
            valdx = rop.llvm_load_value (X, 1, i, type);
            valdy = rop.llvm_load_value (X, 2, i, type);
            llvm::Value *mindx = rop.llvm_load_value (Min, 1, i, type);
            llvm::Value *mindy = rop.llvm_load_value (Min, 2, i, type);
            valdx = rop.builder().CreateSelect (cond, mindx, valdx);
            valdy = rop.builder().CreateSelect (cond, mindy, valdy);
        }
        // Now do the upper bound
        llvm::Value *max = rop.llvm_load_value (Max, 0, i, type);
        cond = is_float ? rop.builder().CreateFCmpUGT(val, max)
                        : rop.builder().CreateICmpSGT(val, max);
        val = rop.builder().CreateSelect (cond, max, val);
        if (Result.has_derivs()) {
            llvm::Value *maxdx = rop.llvm_load_value (Max, 1, i, type);
            llvm::Value *maxdy = rop.llvm_load_value (Max, 2, i, type);
            valdx = rop.builder().CreateSelect (cond, maxdx, valdx);
            valdy = rop.builder().CreateSelect (cond, maxdy, valdy);
        }
        rop.llvm_store_value (val, Result, 0, i);
        rop.llvm_store_value (valdx, Result, 1, i);
        rop.llvm_store_value (valdy, Result, 2, i);
    }
    return true;
}



LLVMGEN (llvm_gen_bitwise_binary_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    Symbol& B = *rop.opargsym (op, 2);
    ASSERT (Result.typespec().is_int() && A.typespec().is_int() && 
            B.typespec().is_int());

    llvm::Value *a = rop.loadLLVMValue (A);
    llvm::Value *b = rop.loadLLVMValue (B);
    if (!a || !b)
        return false;
    llvm::Value *r = NULL;
    if (op.opname() == op_bitand)
        r = rop.builder().CreateAnd (a, b);
    else if (op.opname() == op_bitor)
        r = rop.builder().CreateOr (a, b);
    else if (op.opname() == op_xor)
        r = rop.builder().CreateXor (a, b);
    else if (op.opname() == op_shl)
        r = rop.builder().CreateShl (a, b);
    else if (op.opname() == op_shr)
        r = rop.builder().CreateAShr (a, b);  // signed int -> arithmetic shift
    else
        return false;
    rop.storeLLVMValue (r, Result);
    return true;
}



// Simple (pointwise) unary ops (Abs, ..., 
LLVMGEN (llvm_gen_unary_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src = *rop.opargsym (op, 1);
    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();
    bool src_float = src.typespec().is_floatbased();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        llvm::Value* src_load = rop.loadLLVMValue (src, i, 0);
        if (!src_load) return false;

        llvm::Value* src_val = src_load;

        // Perform the op
        llvm::Value* result = 0;
        ustring opname = op.opname();

        if (opname == op_compl) {
            ASSERT (dst.typespec().is_int());
            result = rop.builder().CreateNot(src_val);
        } else {
            // Don't know how to handle this.
            rop.shadingsys().error ("Don't know how to handle op '%s', eliding the store\n", opname.c_str());
        }

        // Store the result
        if (result) {
            // if our op type doesn't match result, convert
            if (dst_float && !src_float) {
                // Op was int, but we need to store float
                result = rop.llvm_int_to_float (result);
            } else if (!dst_float && src_float) {
                // Op was float, but we need to store int
                result = rop.llvm_float_to_int (result);
            } // otherwise just fine
            rop.storeLLVMValue (result, dst, i, 0);
        }

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            rop.shadingsys().info ("punting on derivatives for now\n");
            // FIXME!!
        }
    }
    return true;
}



// Implementaiton of Simple assignment
static bool
llvm_assign_impl (RuntimeOptimizer &rop, Symbol &Result, Symbol &Src,
                  int arrayindex = -1)
{
    ASSERT (! Result.typespec().is_structure() &&
            ! Result.typespec().is_array());
    ASSERT (! Src.typespec().is_structure() &&
            ! Src.typespec().is_array());

    ASSERT (arrayindex == -1);  // FIXME -- do array elements later

    llvm::Value *arrind = arrayindex >= 0 ? rop.llvm_constant (arrayindex) : NULL;

    if (Result.typespec().is_closure() || Src.typespec().is_closure()) {
        ASSERT (arrayindex < 0 && "FIXME: arrays of closures not implemented");
        if (Src.typespec().is_closure())
            rop.llvm_call_function ("osl_closure_assign", Result, Src);
        else
            rop.llvm_call_function ("osl_closure_clear", Result);
        return true;
    }

    if (Result.typespec().is_matrix() && Src.typespec().is_int_or_float()) {
        // Handle m=f, m=i separately
        llvm::Value *src = rop.llvm_load_value (Src, 0, arrind, 0, TypeDesc::FLOAT /*cast*/);
        // m=f sets the diagonal components to f, the others to zero
        llvm::Value *zero = rop.llvm_constant (0.0f);
        for (int i = 0;  i < 4;  ++i)
            for (int j = 0;  j < 4;  ++j)
                rop.llvm_store_value (i==j ? src : zero, Result, 0, arrind, i*4+j);
        ASSERT (! Result.has_derivs() && "matrices shouldn't have derivs");
        return true;
    }

    // The following code handles f=f, f=i, v=v, v=f, v=i, m=m, s=s.
    // Remember that llvm_load_value will automatically convert scalar->triple.
    TypeDesc rt = Result.typespec().simpletype();
    TypeDesc basetype = TypeDesc::BASETYPE(rt.basetype);
    int num_components = rt.aggregate;
    for (int i = 0; i < num_components; ++i) {
        llvm::Value* src_val = rop.llvm_load_value (Src, 0, arrind, i, basetype);
        if (!src_val)
            return false;
        rop.llvm_store_value (src_val, Result, 0, arrind, i);
    }

    // Handle derivatives
    if (Result.has_derivs()) {
        if (Src.has_derivs()) {
            // src and result both have derivs -- copy them
            for (int d = 1;  d <= 2;  ++d) {
                for (int i = 0; i < num_components; ++i) {
                    llvm::Value* val = rop.llvm_load_value (Src, d, arrind, i);
                    rop.llvm_store_value (val, Result, d, arrind, i);
                }
            }
        } else {
            // Result wants derivs but src didn't have them -- zero them
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



// Simple assignment
LLVMGEN (llvm_gen_assign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    return llvm_assign_impl (rop, Result, Src);
}



// Vector component reference
LLVMGEN (llvm_gen_compref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Val = *rop.opargsym (op, 1);
    Symbol& Index = *rop.opargsym (op, 2);

    for (int d = 0;  d < 3;  ++d) {  // deriv
        llvm::Value *val = NULL; 
        // FIXME -- should we test for out-of-range component?
        if (Index.is_constant()) {
            val = rop.llvm_load_value (Val, d, *((int*)Index.data()));
        } else {
            llvm::Value *c = rop.llvm_load_value(Index, d);
            val = rop.llvm_load_component_value (Val, d, c);
        }
        rop.llvm_store_value (val, Result, d);
        if (! Result.has_derivs())  // skip the derivs if we don't need them
            break;
    }
    return true;
}



// Vector component assignment
LLVMGEN (llvm_gen_compassign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Index = *rop.opargsym (op, 1);
    Symbol& Val = *rop.opargsym (op, 2);

    for (int d = 0;  d < 3;  ++d) {  // deriv
        llvm::Value *val = rop.llvm_load_value (Val, d, 0, TypeDesc::TypeFloat);
        // FIXME -- should we test for out-of-range component?
        if (Index.is_constant()) {
            rop.llvm_store_value (val, Result, d, *((int*)Index.data()));
        } else {
            llvm::Value *c = rop.llvm_load_value(Index, d);
            rop.llvm_store_component_value (val, Result, d, c);
        }
        if (! Result.has_derivs())  // skip the derivs if we don't need them
            break;
    }
    return true;
}



// Matrix component reference
LLVMGEN (llvm_gen_mxcompref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& M = *rop.opargsym (op, 1);
    Symbol& Row = *rop.opargsym (op, 2);
    Symbol& Col = *rop.opargsym (op, 3);

    llvm::Value *val = NULL; 
    // FIXME -- should we test for out-of-range component?
    if (Row.is_constant() && Col.is_constant()) {
        int comp = 4 * ((int*)Row.data())[0] + ((int*)Col.data())[0];
        val = rop.llvm_load_value (M, 0, comp);
    } else {
        llvm::Value *row = rop.llvm_load_value (Row);
        llvm::Value *col = rop.llvm_load_value (Col);
        llvm::Value *comp = rop.builder().CreateMul (row, rop.llvm_constant(4));
        comp = rop.builder().CreateAdd (comp, col);
        val = rop.llvm_load_component_value (M, 0, comp);
    }
    rop.llvm_store_value (val, Result);
    rop.llvm_zero_derivs (Result);

    return true;
}



// Matrix component assignment
LLVMGEN (llvm_gen_mxcompassign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Row = *rop.opargsym (op, 1);
    Symbol& Col = *rop.opargsym (op, 2);
    Symbol& Val = *rop.opargsym (op, 3);

    llvm::Value *val = rop.llvm_load_value (Val, 0, 0, TypeDesc::TypeFloat);

    // FIXME -- should we test for out-of-range component?
    if (Row.is_constant() && Col.is_constant()) {
        int comp = 4 * ((int*)Row.data())[0] + ((int*)Col.data())[0];
        rop.llvm_store_value (val, Result, 0, comp);
    } else {
        llvm::Value *row = rop.llvm_load_value(Row);
        llvm::Value *col = rop.llvm_load_value(Col);
        llvm::Value *comp = rop.builder().CreateMul (row, rop.llvm_constant(4));
        comp = rop.builder().CreateAdd (comp, col);
        rop.llvm_store_component_value (val, Result, 0, comp);
    }
    return true;
}



// Array length
LLVMGEN (llvm_gen_arraylength)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& A = *rop.opargsym (op, 1);
    DASSERT (Result.typespec().is_int() && A.typespec().is_array());

    int len = A.typespec().arraylength();
    rop.llvm_store_value (rop.llvm_constant(len), Result);
    return true;
}



// Array reference
LLVMGEN (llvm_gen_aref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Src = *rop.opargsym (op, 1);
    Symbol& Index = *rop.opargsym (op, 2);

    // Get array index we're interested in
    llvm::Value *index = rop.loadLLVMValue (Index);
    if (! index)
        return false;
    // FIXME -- do range detection here for constant indices
    // Or should we always do range detection for safety?

    int num_components = Src.typespec().simpletype().aggregate;
    for (int d = 0;  d <= 2;  ++d) {
        for (int c = 0;  c < num_components;  ++c) {
            llvm::Value *val = rop.llvm_load_value (Src, d, index, c);
            rop.storeLLVMValue (val, Result, c, d);
        }
        if (! Result.has_derivs())
            break;
    }

    return true;
}



// Array assignment
LLVMGEN (llvm_gen_aassign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    Symbol& Index = *rop.opargsym (op, 1);
    Symbol& Src = *rop.opargsym (op, 2);

    // Get array index we're interested in
    llvm::Value *index = rop.loadLLVMValue (Index);
    if (! index)
        return false;
    // FIXME -- do range detection here for constant indices
    // Or should we always do range detection for safety?

    int num_components = Result.typespec().simpletype().aggregate;
    for (int d = 0;  d <= 2;  ++d) {
        for (int c = 0;  c < num_components;  ++c) {
            llvm::Value *val = rop.loadLLVMValue (Src, c, d);
            rop.llvm_store_value (val, Result, d, index, c);
        }
        if (! Result.has_derivs())
            break;
    }

    return true;
}



// Construct triple (point, vector, normal, color)
LLVMGEN (llvm_gen_construct_triple)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    bool using_space = (op.nargs() == 5);
    Symbol& Space = *rop.opargsym (op, 1);
    Symbol& X = *rop.opargsym (op, 1+using_space);
    Symbol& Y = *rop.opargsym (op, 2+using_space);
    Symbol& Z = *rop.opargsym (op, 3+using_space);
    ASSERT (Result.typespec().is_triple() && X.typespec().is_float() &&
            Y.typespec().is_float() && Z.typespec().is_float() &&
            (using_space == false || Space.typespec().is_string()));

    for (int d = 0;  d < 3;  ++d) {  // loop over derivs
        // First, copy the floats into the vector
        for (int c = 0;  c < 3;  ++c) {  // loop over components
            const Symbol& comp = *rop.opargsym (op, c+1+using_space);
            llvm::Value* val = rop.llvm_load_value (comp, d, NULL, 0, TypeDesc::TypeFloat);
            rop.llvm_store_value (val, Result, d, NULL, c);
        }
        // Do the transformation in-place, if called for
        if (using_space) {
            llvm::Value *args[3];
            args[0] = rop.sg_void_ptr ();  // shader globals
            args[1] = rop.llvm_void_ptr (Result, d);  // vector
            args[2] = rop.llvm_load_value (Space); // from
            if (op.opname() == op_vector || d > 0) {
                // NB derivs are like vecs
                rop.llvm_call_function ("osl_prepend_vector_from", args, 3);
            }
            else if (op.opname() == op_point)
                rop.llvm_call_function ("osl_prepend_point_from", args, 3);
            else if (op.opname() == op_normal)
                rop.llvm_call_function ("osl_prepend_normal_from", args, 3);
            else ASSERT(0 && "unsupported color ctr with color space name");
        }
        if (! Result.has_derivs())
            break;    // don't bother if Result doesn't have derivs
    }

    return true;
}



/// matrix constructor.  Comes in several varieties:
///    matrix (float)
///    matrix (space, float)
///    matrix (...16 floats...)
///    matrix (space, ...16 floats...)
///    matrix (fromspace, tospace)
LLVMGEN (llvm_gen_matrix)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result = *rop.opargsym (op, 0);
    int nargs = op.nargs();
    bool using_space = (nargs == 3 || nargs == 18);
    bool using_two_spaces = (nargs == 3 && rop.opargsym(op,2)->typespec().is_string());
    int nfloats = nargs - 1 - (int)using_space;
    ASSERT (nargs == 2 || nargs == 3 || nargs == 17 || nargs == 18);

    if (using_two_spaces) {
        llvm::Value *args[4];
        args[0] = rop.sg_void_ptr();  // shader globals
        args[1] = rop.llvm_void_ptr(Result);  // result
        args[2] = rop.llvm_load_value(*rop.opargsym (op, 1));  // from
        args[3] = rop.llvm_load_value(*rop.opargsym (op, 2));  // to
        rop.llvm_call_function ("osl_get_from_to_matrix", args, 4);
    } else {
        if (nfloats == 1) {
            for (int i = 0; i < 16; i++) {
                llvm::Value* src_val = ((i%4) == (i/4)) 
                    ? rop.llvm_load_value (*rop.opargsym(op,1+using_space))
                    : rop.llvm_constant(0.0f);
                rop.llvm_store_value (src_val, Result, 0, i);
            }
        } else if (nfloats == 16) {
            for (int i = 0; i < 16; i++) {
                llvm::Value* src_val = rop.llvm_load_value (*rop.opargsym(op,i+1+using_space));
                rop.llvm_store_value (src_val, Result, 0, i);
            }
        } else {
            ASSERT (0);
        }
        if (using_space) {
            llvm::Value *args[3];
            args[0] = rop.sg_void_ptr();  // shader globals
            args[1] = rop.llvm_void_ptr(Result);  // result
            args[2] = rop.llvm_load_value(*rop.opargsym (op, 1));  // from
            rop.llvm_call_function ("osl_prepend_matrix_from", args, 3);
        }
    }
    return true;
}



// Derivs
LLVMGEN (llvm_gen_Dx)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    for (int i = 0; i < Result.typespec().aggregate(); ++i) {
        llvm::Value* src_val = rop.llvm_load_value (Src, 1 /*dx*/, i);
        rop.storeLLVMValue (src_val, Result, i, 0);
    }

    // Don't have 2nd order derivs
    rop.llvm_zero_derivs (Result);
    return true;
}



// Derivs
LLVMGEN (llvm_gen_Dy)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));

    for (int i = 0; i < Result.typespec().aggregate(); ++i) {
        llvm::Value* src_val = rop.llvm_load_value (Src, 2 /*dy*/, i);
        rop.storeLLVMValue (src_val, Result, i, 0);
    }

    // Don't have 2nd order derivs
    rop.llvm_zero_derivs (Result);
    return true;
}



// Comparison ops
LLVMGEN (llvm_gen_compare_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol &Result (*rop.opargsym (op, 0));
    Symbol &A (*rop.opargsym (op, 1));
    Symbol &B (*rop.opargsym (op, 2));
    ASSERT (Result.typespec().is_int() && ! Result.has_derivs());

    int num_components = std::max (A.typespec().aggregate(), B.typespec().aggregate());
    bool float_based = A.typespec().is_floatbased() || B.typespec().is_floatbased();
    TypeDesc cast (float_based ? TypeDesc::FLOAT : TypeDesc::UNKNOWN);
                                   
    llvm::Value* final_result = 0;
    ustring opname = op.opname();

    for (int i = 0; i < num_components; i++) {
        // Get A&B component i -- note that these correctly handle mixed
        // scalar/triple comparisons as well as int->float casts as needed.
        llvm::Value* a = rop.loadLLVMValue (A, i, 0, cast);
        llvm::Value* b = rop.loadLLVMValue (B, i, 0, cast);

        // Trickery for mixed matrix/scalar comparisons -- compare
        // on-diagonal to the scalar, off-diagonal to zero
        if (A.typespec().is_matrix() && !B.typespec().is_matrix()) {
            if ((i/4) != (i%4))
                b = rop.llvm_constant (0.0f);
        }
        if (! A.typespec().is_matrix() && B.typespec().is_matrix()) {
            if ((i/4) != (i%4))
                a = rop.llvm_constant (0.0f);
        }

        // Perform the op
        llvm::Value* result = 0;
        if (opname == op_lt) {
            result = float_based ? rop.builder().CreateFCmpULT(a, b) : rop.builder().CreateICmpSLT(a, b);
        } else if (opname == op_le) {
            result = float_based ? rop.builder().CreateFCmpULE(a, b) : rop.builder().CreateICmpSLE(a, b);
        } else if (opname == op_eq) {
            result = float_based ? rop.builder().CreateFCmpUEQ(a, b) : rop.builder().CreateICmpEQ(a, b);
        } else if (opname == op_ge) {
            result = float_based ? rop.builder().CreateFCmpUGE(a, b) : rop.builder().CreateICmpSGE(a, b);
        } else if (opname == op_gt) {
            result = float_based ? rop.builder().CreateFCmpUGT(a, b) : rop.builder().CreateICmpSGT(a, b);
        } else if (opname == op_neq) {
            result = float_based ? rop.builder().CreateFCmpUNE(a, b) : rop.builder().CreateICmpNE(a, b);
        } else {
            // Don't know how to handle this.
            ASSERT (0 && "Comparison error");
        }
        ASSERT (result);

        if (final_result) {
            // Combine the component bool based on the op
            if (opname != op_neq)        // final_result &= result
                final_result = rop.builder().CreateAnd(final_result, result);
            else                         // final_result |= result
                final_result = rop.builder().CreateOr(final_result, result);
        } else {
            final_result = result;
        }
    }
    ASSERT (final_result);

    // Convert the single bit bool into an int for now.
    final_result = rop.builder().CreateZExt (final_result, rop.llvm_type_int());
    rop.storeLLVMValue (final_result, Result, 0, 0);
    return true;
}



// int regex_search (string subject, string pattern)
// int regex_search (string subject, int results[], string pattern)
// int regex_match (string subject, string pattern)
// int regex_match (string subject, int results[], string pattern)
LLVMGEN (llvm_gen_regex)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    int nargs = op.nargs();
    ASSERT (nargs == 3 || nargs == 4);
    Symbol &Result (*rop.opargsym (op, 0));
    Symbol &Subject (*rop.opargsym (op, 1));
    bool do_match_results = (nargs == 4);
    bool fullmatch = (op.opname() == "regex_match");
    Symbol &Match (*rop.opargsym (op, 2));
    Symbol &Pattern (*rop.opargsym (op, 2+do_match_results));
    ASSERT (Result.typespec().is_int() && Subject.typespec().is_string() &&
            Pattern.typespec().is_string());
    ASSERT (!do_match_results || 
            (Match.typespec().is_array() &&
             Match.typespec().elementtype().is_int()));

    std::vector<llvm::Value*> call_args;
    // First arg is ShaderGlobals ptr
    call_args.push_back (rop.sg_void_ptr());
    // Next arg is subject string
    call_args.push_back (rop.llvm_load_value (Subject));
    // Pass the results array and length (just pass 0 if no results wanted).
    call_args.push_back (rop.llvm_void_ptr(Match));
    if (do_match_results)
        call_args.push_back (rop.llvm_constant(Match.typespec().arraylength()));
    else
        call_args.push_back (rop.llvm_constant(0));
    // Pass the regex match pattern
    call_args.push_back (rop.llvm_load_value (Pattern));
    // Pass whether or not to do the full match
    call_args.push_back (rop.llvm_constant(fullmatch));

    llvm::Value *ret = rop.llvm_call_function ("osl_regex_impl", &call_args[0],
                                               (int)call_args.size());
    rop.llvm_store_value (ret, Result);
    return true;
}



// Generic llvm code generation.  See the comments in llvm_ops.cpp for
// the full list of assumptions and conventions.  But in short:
//   1. All polymorphic and derivative cases implemented as functions in
//      llvm_ops.cpp -- no custom IR is needed.
//   2. Naming conention is: osl_NAME_{args}, where args is the
//      concatenation of type codes for all args including return value --
//      f/i/v/m/s for float/int/triple/matrix/string, and df/dv/dm for
//      duals.
//   3. The function returns scalars as an actual return value (that
//      must be stored), but "returns" aggregates or duals in the first
//      argument.
//   4. Duals and aggregates are passed as void*'s, float/int/string 
//      passed by value.
//   5. Note that this only works if triples are all treated identically,
//      this routine can't be used if it must be polymorphic based on
//      color, point, vector, normal differences.
//
LLVMGEN (llvm_gen_generic)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result  = *rop.opargsym (op, 0);
    std::vector<const Symbol *> args;
    bool any_deriv_args = false;
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        args.push_back (s);
        any_deriv_args |= (i > 0 && s->has_derivs() && !s->typespec().is_matrix());
    }

    // Special cases: functions that have no derivs -- suppress them
    if (any_deriv_args)
        if (op.opname() == op_floor || op.opname() == op_ceil ||
            op.opname() == op_round || op.opname() == op_step ||
            op.opname() == op_trunc)
            any_deriv_args = false;

    std::string name = std::string("osl_") + op.opname().string() + "_";
    for (int i = 0;  i < op.nargs();  ++i) {
        Symbol *s (rop.opargsym (op, i));
        if (any_deriv_args && Result.has_derivs() && s->has_derivs() && !s->typespec().is_matrix())
            name += "d";
        if (s->typespec().is_float())
            name += "f";
        else if (s->typespec().is_triple())
            name += "v";
        else if (s->typespec().is_matrix())
            name += "m";
        else if (s->typespec().is_string())
            name += "s";
        else if (s->typespec().is_int())
            name += "i";
        else ASSERT (0);
    }

    if (! Result.has_derivs() || ! any_deriv_args) {
        // Don't compute derivs -- either not needed or not provided in args
        if (Result.typespec().aggregate() == TypeDesc::SCALAR) {
            llvm::Value *r = rop.llvm_call_function (name.c_str(),
                                                     &(args[1]), op.nargs()-1);
            rop.llvm_store_value (r, Result);
        } else {
            rop.llvm_call_function (name.c_str(), &(args[0]), op.nargs());
        }
        rop.llvm_zero_derivs (Result);
    } else {
        // Cases with derivs
        ASSERT (Result.has_derivs() && any_deriv_args);
        rop.llvm_call_function (name.c_str(), &(args[0]), op.nargs(), true);
    }
    return true;
}



LLVMGEN (llvm_gen_if)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym (op, 0);
    RuntimeOptimizer::BasicBlockMap& bb_map (rop.bb_map());

    // Load the condition variable
    llvm::Value* cond_load = rop.loadLLVMValue (cond, 0, 0);
    // Convert the int to a bool via comparison to 0
    llvm::Value* cond_bool = rop.builder().CreateICmpNE (cond_load, rop.llvm_constant(0));
    
    // Branch on the condition, to our blocks
    llvm::BasicBlock* then_block = bb_map[opnum+1];
    llvm::BasicBlock* else_block = bb_map[op.jump(0)];
    llvm::BasicBlock* after_block = bb_map[op.jump(1)];
    rop.builder().CreateCondBr (cond_bool, then_block, else_block);
    // Put an unconditional branch at the end of the Then and Else blocks
    if (then_block != after_block) {
        rop.builder().SetInsertPoint (then_block);
        rop.builder().CreateBr (after_block);
    }
    if (else_block != after_block) {
        rop.builder().SetInsertPoint (else_block);
        rop.builder().CreateBr (after_block);
    }
    return true;
}



LLVMGEN (llvm_gen_loop_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym (op, 0);
    RuntimeOptimizer::BasicBlockMap& bb_map (rop.bb_map());

    // Branch on the condition, to our blocks
    llvm::BasicBlock* init_block = bb_map[opnum+1];
    llvm::BasicBlock* cond_block = bb_map[op.jump(0)];
    llvm::BasicBlock* body_block = bb_map[op.jump(1)];
    llvm::BasicBlock* step_block = bb_map[op.jump(2)];
    llvm::BasicBlock* after_block = bb_map[op.jump(3)];

    // Insert the unconditional jump to the LoopCond
    if (init_block != cond_block) {
        // There are some init ops, insert branch afterwards (but first jump to InitBlock)
        rop.builder().CreateBr(init_block);
        rop.builder().SetInsertPoint(init_block);
    }
    // Either we have init ops (and we'll jump to LoopCond afterwards)
    // or we don't and we need a terminator in the current block. If
    // we're a dowhile loop, we jump to the body block after init
    // instead of cond.
    if (op.opname() == op_dowhile) {
        rop.builder().CreateBr(body_block);
    } else {
        rop.builder().CreateBr(cond_block);
    }

    rop.builder().SetInsertPoint(cond_block);
    // Load the condition variable (it will have been computed by now)
    llvm::Value* cond_load = rop.loadLLVMValue (cond, 0, 0);
    // Convert the int to a bool via comparison to zero
    llvm::Value* cond_bool = rop.builder().CreateICmpNE (cond_load, rop.llvm_constant(0));
    // Jump to either LoopBody or AfterLoop
    rop.builder().CreateCondBr(cond_bool, body_block, after_block);

    // Put an unconditional jump from Body into Step
    if (step_block != after_block) {
        rop.builder().SetInsertPoint(body_block);
        rop.builder().CreateBr(step_block);

        // Put an unconditional jump from Step to Cond
        rop.builder().SetInsertPoint(step_block);
        rop.builder().CreateBr(cond_block);
    } else {
        // Step is empty, probably a do_while or while loop. Jump from Body to Cond
        rop.builder().SetInsertPoint(body_block);
        rop.builder().CreateBr(cond_block);
    }
    return true;
}



void
RuntimeOptimizer::llvm_assign_initial_value (const Symbol& sym)
{
    int num_components = sym.typespec().simpletype().aggregate;
    if (sym.typespec().is_array())
        num_components *= sym.typespec().arraylength();

    for (int i = 0; i < num_components; ++i) {
        if (sym.has_init_ops())
            break;   // FIXME - skip init ops for now

        // Fill in the constant val
        // Setup initial store
        llvm::Value* init_val = 0;
        // shadingsys.info ("Assigning initial value for symbol '%s' = ", sym.mangled().c_str());
        if (sym.typespec().is_floatbased())
            init_val = llvm_constant (((float*)sym.data())[i]);
        else if (sym.typespec().simpletype() == TypeDesc::TypeString)
            init_val = llvm_constant (((ustring*)sym.data())[i]);
        else if (sym.typespec().simpletype() == TypeDesc::TypeInt)
            init_val = llvm_constant (((int*)sym.data())[i]);
        storeLLVMValue (init_val, sym, i, 0);
    }
    if (sym.has_derivs())
        llvm_zero_derivs (sym);
}




static std::map<ustring,OpLLVMGen> llvm_generator_table;


static void
initialize_llvm_generator_table ()
{
    static spin_mutex table_mutex;
    static bool table_initialized = false;
    spin_lock lock (table_mutex);
    if (table_initialized)
        return;   // already initialized
#define INIT2(name,func) llvm_generator_table[ustring(#name)] = func
#define INIT(name) llvm_generator_table[ustring(#name)] = llvm_gen_##name;

    INIT (aassign);
    INIT2 (abs, llvm_gen_generic);
    INIT2 (acos, llvm_gen_generic);
    INIT (add);
    // INIT (and);
    // INIT (ashikhmin_velvet);
    // INIT (area);
    INIT (aref);
    INIT (arraylength);
    INIT2 (asin, llvm_gen_generic);
    INIT (assign);
    INIT2 (atan, llvm_gen_generic);
    INIT2 (atan2, llvm_gen_generic);
    // INIT (background);
    INIT2 (bitand, llvm_gen_bitwise_binary_op);
    INIT2 (bitor, llvm_gen_bitwise_binary_op);
    // INIT (bssrdf_cubic);
    // INIT (calculatenormal);
    INIT2 (ceil, llvm_gen_generic);
    // INIT (cellnoise);
    INIT (clamp);
    // INIT (cloth);
    // INIT (cloth_specular);
    INIT2 (color, llvm_gen_construct_triple);
    INIT (compassign);
    INIT2 (compl, llvm_gen_unary_op);
    INIT (compref);
    INIT2 (concat, llvm_gen_generic);
    INIT2 (cos, llvm_gen_generic);
    INIT2 (cosh, llvm_gen_generic);
    INIT2 (cross, llvm_gen_generic);
    INIT2 (degrees, llvm_gen_generic);
    INIT2 (determinant, llvm_gen_generic);
    // INIT (dielectric);
    // INIT (diffuse);
    INIT2 (distance, llvm_gen_generic);
    INIT (div);
    INIT2 (dot, llvm_gen_generic);
    INIT (Dx);
    INIT (Dy);
    INIT2 (dowhile, llvm_gen_loop_op);
    // INIT (emission);
    // INIT (end);
    INIT2 (endswith, llvm_gen_generic);
    INIT2 (eq, llvm_gen_compare_op);
    INIT2 (erf, llvm_gen_generic);
    INIT2 (erfc, llvm_gen_generic);
    INIT2 (error, llvm_gen_printf);
    INIT2 (exp, llvm_gen_generic);
    INIT2 (exp2, llvm_gen_generic);
    INIT2 (expm1, llvm_gen_generic);
    INIT2 (fabs, llvm_gen_generic);
    // INIT (fakefur_diffuse);
    // INIT (fakefur_specular);
    // INIT (fakefur_skin);
    // INIT (filterwidth);
    INIT2 (floor, llvm_gen_generic);
    INIT2 (fmod, llvm_gen_mod);
    INIT2 (for, llvm_gen_loop_op);
    INIT2 (format, llvm_gen_printf);
    // INIT (fresnel);
    INIT2 (ge, llvm_gen_compare_op);
    // INIT (getattribute);
    // INIT (getmessage);
    // INIT (gettextureinfo);
    INIT2 (gt, llvm_gen_compare_op);
    // INIT (hair_diffuse);
    // INIT (hair_specular);
    //stdosl.h  INIT (hypot);
    INIT (if);
    INIT2 (inversesqrt, llvm_gen_generic);
    // INIT (iscameraray);
    INIT2 (isfinite, llvm_gen_generic);
    INIT2 (isinf, llvm_gen_generic);
    INIT2 (isnan, llvm_gen_generic);
    // INIT (isshadowray);
    INIT2 (le, llvm_gen_compare_op);
    INIT2 (length, llvm_gen_generic);
    INIT2 (log, llvm_gen_generic);
    INIT2 (log10, llvm_gen_generic);
    INIT2 (log2, llvm_gen_generic);
    INIT2 (logb, llvm_gen_generic);
    INIT2 (lt, llvm_gen_compare_op);
    //stdosl.h   INIT (luminance);
    INIT (matrix);
    INIT (mxcompassign);
    INIT (mxcompref);
    INIT2 (max, llvm_gen_minmax);
    // INIT (microfacet_beckmann);
    // INIT (microfacet_beckmann_refraction);
    // INIT (microfacet_ggx);
    // INIT (microfacet_ggx_refraction);
    INIT2 (min, llvm_gen_minmax);
    //stdosl.h   INIT (mix);
    INIT (mod);
    INIT (mul);
    INIT (neg);
    INIT2 (neq, llvm_gen_compare_op);
    // INIT (noise);
    // INIT (nop);
    INIT2 (normal, llvm_gen_construct_triple);
    INIT2 (normalize, llvm_gen_generic);
    // INIT (or);
    // INIT (phong);
    // INIT (phong_ramp);
    // INIT (pnoise);
    INIT2 (point, llvm_gen_construct_triple);
    INIT2 (pow, llvm_gen_generic);
    INIT (printf);
    // INIT (psnoise);
    INIT2 (radians, llvm_gen_generic);
    // INIT (reflect);
    // INIT (reflection);
    // INIT (refract);
    // INIT (refraction);
    INIT2 (regex_match, llvm_gen_regex);
    INIT2 (regex_search, llvm_gen_regex);
    INIT2 (round, llvm_gen_generic);
    // INIT (setmessage);
    INIT2 (shl, llvm_gen_bitwise_binary_op);
    INIT2 (shr, llvm_gen_bitwise_binary_op);
    INIT2 (sign, llvm_gen_generic);
    INIT2 (sin, llvm_gen_generic);
    // INIT (sincos);
    INIT2 (sinh, llvm_gen_generic);
    INIT2 (smoothstep, llvm_gen_generic);
    // INIT (snoise);
    // INIT (spline);
    INIT2 (sqrt, llvm_gen_generic);
    INIT2 (startswith, llvm_gen_generic);
    INIT2 (step, llvm_gen_generic);
    INIT2 (strlen, llvm_gen_generic);
    INIT (sub);
    INIT2 (substr, llvm_gen_generic);
    // INIT (subsurface);
    // INIT (surfacearea);
    INIT2 (tan, llvm_gen_generic);
    INIT2 (tanh, llvm_gen_generic);
    // INIT (texture);
    INIT2 (transform,  llvm_gen_generic);
    INIT2 (transformn, llvm_gen_generic);
    INIT2 (transformv, llvm_gen_generic);
    // INIT (translucent);
    // INIT (transparent);
    INIT2 (transpose, llvm_gen_generic);
    INIT2 (trunc, llvm_gen_generic);
    INIT (useparam);
    INIT2 (vector, llvm_gen_construct_triple);
    // INIT (ward);
    INIT2 (warning, llvm_gen_printf);
    // INIT (westin_backscatter);
    // INIT (westin_sheen);
    INIT2 (while, llvm_gen_loop_op);
    INIT2 (xor, llvm_gen_bitwise_binary_op);

#undef INIT
#undef INIT2

    table_initialized = true;
}



llvm::Function*
RuntimeOptimizer::build_llvm_instance (bool groupentry)
{
    // Make a layer function: void layer_func(ShaderGlobal*, GroupData*)
    // Note that the GroupData* is passed as a void*.
    std::string unique_layer_name = Strutil::format ("%s_%d", inst()->layername().c_str(), inst()->id());

    m_layer_func = llvm::cast<llvm::Function>(m_llvm_module->getOrInsertFunction(unique_layer_name,
                    llvm_type_void(), llvm_type_sg_ptr(),
                    llvm_type_groupdata_ptr(), NULL));
    const OpcodeVec& instance_ops (inst()->ops());
    llvm::Function::arg_iterator arg_it = m_layer_func->arg_begin();
    // Get shader globals pointer
    m_llvm_shaderglobals_ptr = arg_it++;
    m_llvm_groupdata_ptr = arg_it++;

    llvm::BasicBlock* entry_bb = llvm::BasicBlock::Create (llvm_context(), std::string("EntryBlock_")+unique_layer_name, m_layer_func);

    // Set up a new IR builder
    delete m_builder;
    m_builder = new llvm::IRBuilder<> (entry_bb);

    // Allocate the group data
//    m_llvm_groupdata_ptr = builder().CreateAlloca (groupdata_type,
//                                               llvm_constant(1), "$groupdata");

    // If this is the group entry point, clear all the "layer executed"
    // bits.  If it's not the group entry (but rather is an upstream
    // node), then set its bit!
    if (groupentry) {
        for (int i = 0;  i < group().nlayers();  ++i)
            builder().CreateStore (llvm_constant(0), layer_run_ptr(i));
    } else {
        builder().CreateStore (llvm_constant(1), layer_run_ptr(m_layer));
    }

    // Setup the symbols
    m_named_values.clear ();
    BOOST_FOREACH (Symbol &s, inst()->symbols()) {
        // Skip scalar int or float constants -- we always inline them
        if (s.is_constant() && !s.typespec().is_closure() &&
            (s.typespec().is_float() || s.typespec().is_int()))
            continue;
        // Allocate space for locals, temps, aggregate constants
        if (s.symtype() == SymTypeLocal || s.symtype() == SymTypeTemp ||
                s.symtype() == SymTypeConst)
            getOrAllocateLLVMSymbol (s);
        // Set initial value for params and constants
        if (s.symtype() == SymTypeParam || s.symtype() == SymTypeOutputParam ||
            s.is_constant())
            llvm_assign_initial_value (s);
    }
    // All the symbols are stack allocated now.

    // Mark all the basic blocks, including allocating llvm::BasicBlock
    // records for each.
    find_basic_blocks (true);

    for (int opnum = inst()->maincodebegin();
             opnum < inst()->maincodeend(); ++opnum) {
        const Opcode& op = instance_ops[opnum];
        if (m_bb_map[opnum]) {   // it's NULL when not the start of a BB
            // If we start a new BasicBlock, point the builder there.
            llvm::BasicBlock* next_bb = m_bb_map[opnum];
            if (next_bb != entry_bb) {
                // If we're not the entry block (which is where all the
                // AllocaInstructions go), then start insertion at the
                // beginning of the block. This way we can insert instructions
                // before the possible jmp inserted at the end by an upstream
                // conditional (e.g. if/for/while/do)
                builder().SetInsertPoint (next_bb, next_bb->begin());
            } else {
                // Otherwise, use the end (IRBuilder default)
                builder().SetInsertPoint (next_bb);
            }
        }

        std::map<ustring,OpLLVMGen>::const_iterator found;
        found = llvm_generator_table.find (op.opname());
        if (found != llvm_generator_table.end()) {
            bool ok = (*found->second) (*this, opnum);
            if (! ok)
                return NULL;
        } else if (op.opname() == op_nop ||
                   op.opname() == op_end) {
            // Skip this op, it does nothing...
        } else {
            m_shadingsys.error ("LLVMOSL: Unsupported op %s\n", op.opname().c_str());
            return NULL;
        }
    }

    // Transfer all of this layer's outputs into the downstream shader's
    // inputs.
    for (int layer = m_layer+1;  layer < group().nlayers();  ++layer) {
        ShaderInstance *child = m_group[layer];
        for (int c = 0;  c < child->nconnections();  ++c) {
            const Connection &con (child->connection (c));
            if (con.srclayer == m_layer) {
                ASSERT (con.src.arrayindex == -1 && con.src.channel == -1 &&
                        con.dst.arrayindex == -1 && con.dst.channel == -1 &&
                        "no support for individual element/channel connection");
                Symbol *srcsym (inst()->symbol (con.src.param));
                Symbol *dstsym (child->symbol (con.dst.param));
                if (srcsym->typespec().is_array()) {
                    for (int i = 0;  i < srcsym->typespec().arraylength();  ++i)
                        llvm_assign_impl (*this, *dstsym, *srcsym, i);
                } else {
                    // Not an array case
                    llvm_assign_impl (*this, *dstsym, *srcsym);
                }
            }
        }
    }

    // All done
    builder().CreateRetVoid();

#ifdef DEBUG
    llvm::errs() << "layer_func (" << unique_layer_name << ") after llvm  = " << *m_layer_func << "\n";
#endif

    delete m_builder;
    m_builder = NULL;

    return m_layer_func;
}



void
RuntimeOptimizer::build_llvm_group ()
{
    initialize_llvm_group ();

    llvm::Function *func = NULL;
    int nlayers = m_group.nlayers();
    for (int layer = 0;  layer < nlayers;  ++layer) {
        set_inst (layer);
        func = build_llvm_instance (layer == (nlayers-1));
        llvm_do_optimization (func, layer == (nlayers-1));
#ifdef DEBUG
        llvm::errs() << "func after opt  = " << *func << "\n";
#endif
    }

    m_group.llvm_compiled_version (func);
}



void
RuntimeOptimizer::initialize_llvm_group ()
{
    m_llvm_context = m_shadingsys.llvm_context ();
    m_llvm_module = m_shadingsys.m_llvm_module;
    ASSERT (m_llvm_context && m_llvm_module);

    llvm_setup_optimization_passes ();

    // Clear the shaderglobals and groupdata types -- they will be
    // created on demand.
    m_llvm_type_sg = NULL;
    m_llvm_type_groupdata = NULL;

    // Now we have things we only need to do once for each context.
    static spin_mutex mutex;
    static llvm::LLVMContext *initialized_context = NULL;
    spin_lock lock (mutex);
    if (initialized_context == m_llvm_context)
        return;   // already initialized for this context

    // Set up aliases for types we use over and over
    m_llvm_type_float = llvm::Type::getFloatTy (*m_llvm_context);
    m_llvm_type_int = llvm::Type::getInt32Ty (*m_llvm_context);
    m_llvm_type_bool = llvm::Type::getInt1Ty (*m_llvm_context);
    m_llvm_type_void = llvm::Type::getVoidTy (*m_llvm_context);
    m_llvm_type_char_ptr = llvm::Type::getInt8PtrTy (*m_llvm_context);
    m_llvm_type_float_ptr = llvm::Type::getFloatPtrTy (*m_llvm_context);

    // A triple is a struct composed of 3 floats
    std::vector<const llvm::Type*> triplefields(3, m_llvm_type_float);
    m_llvm_type_triple = llvm::StructType::get(llvm_context(), triplefields);
    m_llvm_type_triple_ptr = llvm::PointerType::get (m_llvm_type_triple, 0);

    // A matrix is a struct composed of a matrix of 16 floats
    const llvm::Type *float16 = llvm::ArrayType::get (m_llvm_type_float, 16);
    std::vector<const llvm::Type*> matrixfields(1, float16);
    m_llvm_type_matrix = llvm::StructType::get(llvm_context(), matrixfields);
    m_llvm_type_matrix_ptr = llvm::PointerType::get (m_llvm_type_matrix, 0);

    m_shadingsys.info ("Adding in extern functions");

    for (int i = 0;  llvm_helper_function_table[i];  i += 2) {
        const char *funcname = llvm_helper_function_table[i];
        bool varargs = false;
        const char *types = llvm_helper_function_table[i+1];
        int advance;
        TypeSpec rettype = OSLCompilerImpl::type_from_code (types, &advance);
        types += advance;
        std::vector<const llvm::Type*> params;
        while (*types) {
            TypeSpec t = OSLCompilerImpl::type_from_code (types, &advance);
            if (t.simpletype().basetype == TypeDesc::UNKNOWN) {
                if (*types == '*')
                    varargs = true;
                else
                    ASSERT (0);
            } else {
                params.push_back (llvm_pass_type (t));
            }
            types += advance;
        }
        llvm::FunctionType *func = llvm::FunctionType::get (llvm_type(rettype), params, varargs);
        m_llvm_module->getOrInsertFunction (funcname, func);
    }

    initialized_context = m_llvm_context;
}



void
ShadingSystemImpl::SetupLLVM ()
{
    // Setup already
    if (m_llvm_exec != NULL)
        return;
    info ("Setting up LLVM");
    m_llvm_context = new llvm::LLVMContext();
    info ("Initializing Native Target");
    llvm::InitializeNativeTarget();

#if 1
    //printf("Loading LLVM Bitcode\n");
    const char *data = osl_llvm_compiled_ops_block;
    llvm::MemoryBuffer *buf =
        llvm::MemoryBuffer::getMemBuffer (data, data + osl_llvm_compiled_ops_size);
    // Parse the bitcode into a Module
    std::string err;
    m_llvm_module = llvm::ParseBitcodeFile (buf, *llvm_context(), &err);
    delete buf;
#else
    m_llvm_module = new llvm::Module ("oslmodule", *llvm_context());
#endif

    info ("Building an Execution Engine");
    std::string error_msg;
    m_llvm_exec = llvm::ExecutionEngine::create(m_llvm_module,
                                                false /* force interpreter */,
                                                &error_msg,
                                                llvm::CodeGenOpt::Default,
                                                false);
    if (! m_llvm_exec) {
        error ("Failed to create engine: %s\n", error_msg.c_str());
        return;
    }

    initialize_llvm_generator_table ();
}



void
RuntimeOptimizer::llvm_setup_optimization_passes ()
{
    ASSERT (m_llvm_passes == NULL && m_llvm_func_passes == NULL);

    // Specify per-function passes
    //
    m_llvm_func_passes = new llvm::FunctionPassManager(llvm_module());
    llvm::FunctionPassManager &fpm (*m_llvm_func_passes);
    fpm.add (new llvm::TargetData(llvm_module()));
    // Change memory references to registers
    fpm.add (llvm::createPromoteMemoryToRegisterPass());
    // Combine instructions where possible -- peephole opts & bit-twiddling
    fpm.add (llvm::createInstructionCombiningPass());
    // Eliminate early returns
    fpm.add (llvm::createUnifyFunctionExitNodesPass());
    // resassociate exprssions (a = x + (3 + y) -> a = x + y + 3)
    fpm.add (llvm::createReassociatePass());
    // Eliminate common sub-expressions
    fpm.add (llvm::createGVNPass());
    // Simplify the call graph if possible (deleting unreachable blocks, etc.)
    fpm.add (llvm::createCFGSimplificationPass());
    // More dead code elimination
    fpm.add (llvm::createAggressiveDCEPass());
    // Try to make stuff into registers one last time.
    fpm.add (llvm::createPromoteMemoryToRegisterPass());
    // Always add verifier?
    fpm.add (llvm::createVerifierPass());

    // Specify module-wide (interprocedural optimization) passes
    //
    m_llvm_passes = new llvm::PassManager;
    llvm::PassManager &passes (*m_llvm_passes);
    passes.add (new llvm::TargetData(llvm_module()));
    // Inline small functions
    passes.add (llvm::createFunctionInliningPass());
    passes.add (llvm::createVerifierPass());
}



void
RuntimeOptimizer::llvm_do_optimization (llvm::Function *func,
                                        bool interproc)
{
    ASSERT (m_llvm_passes != NULL && m_llvm_func_passes != NULL);

    if (! func) {
        // If no particular function is specified, run function passes
        // on all functions in the module (just recursively call this
        // function).
        for (llvm::Module::iterator i = llvm_module()->begin();
                 i != llvm_module()->end(); ++i)
            llvm_do_optimization (&(*i));
        return;
    }

    m_llvm_func_passes->doInitialization();
    m_llvm_func_passes->run (*func);
    m_llvm_func_passes->doFinalization();

    if (interproc) {
        // Run module-wide (interprocedural optimization) passes
        m_llvm_passes->run (*llvm_module());

        // Since the passes above inlined function calls, among other
        // things, we should rerun our whole optimization set on the master
        // function now.
        ASSERT (func);
        m_llvm_func_passes->doInitialization ();
        m_llvm_func_passes->run (*func);
        m_llvm_func_passes->doFinalization ();
    }
}



}; // namespace pvt
}; // namespace osl

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif
