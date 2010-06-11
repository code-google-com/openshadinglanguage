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
#include "oslexec_pvt.h"
#include "oslops.h"
#include "../liboslcomp/oslcomp_pvt.h"
#include "runtimeoptimize.h"

using namespace OSL;
using namespace OSL::pvt;

#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {


static ustring op_abs("abs");
static ustring op_ceil("ceil");
static ustring op_color("color");
static ustring op_compref("compref");
static ustring op_cos("cos");
static ustring op_cross("cross");
static ustring op_dot("dot");
static ustring op_dowhile("dowhile");
static ustring op_end("end");
static ustring op_eq("eq");
static ustring op_erf("erf");
static ustring op_erfc("erfc");
static ustring op_exp("exp");
static ustring op_exp2("exp2");
static ustring op_expm1("expm1");
static ustring op_fabs("fabs");
static ustring op_for("for");
static ustring op_ge("ge");
static ustring op_gt("gt");
static ustring op_if("if");
static ustring op_le("le");
static ustring op_length("length");
static ustring op_log10("log10");
static ustring op_log2("log2");
static ustring op_logb("logb");
static ustring op_lt("lt");
static ustring op_luminance("luminance");
static ustring op_neg("neg");
static ustring op_neq("neq");
static ustring op_nop("nop");
static ustring op_normalize("normalize");
static ustring op_printf("printf");
static ustring op_sin("sin");
static ustring op_sqrt("sqrt");
static ustring op_vector("vector");
static ustring op_while("while");



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
/// checking.
static const char *llvm_helper_function_table[] = {
    "fabsf", "ff",

    "osl_printf", "xs*",
    "osl_closure_clear", "xC",
    "osl_closure_assign", "xCC",
    "osl_add_closure_closure", "xCCC",
    "osl_mul_closure_float", "xCCf",
    "osl_mul_closure_color", "xCCc",
    "osl_mul_mm", "xmmm",
    "osl_mul_mf", "xmmf",
    "osl_mul_m_ff", "xmff",
    "osl_div_mm", "xmmm",
    "osl_div_mf", "xmmf",
    "osl_div_fm", "xmfm",
    "osl_div_m_ff", "xmff",
    NULL
};



const llvm::StructType *
RuntimeOptimizer::getShaderGlobalType ()
{
    std::vector<const llvm::Type*> vec3_types(3, llvm_type_float());
    const llvm::StructType* vec3_type = llvm::StructType::get(llvm_context(), vec3_types);
    // NOTE(boulos): Bool is a C++ concept that maps to int in C.
    const llvm::Type* bool_type = llvm_type_int();

    std::vector<const llvm::Type*> sg_types;
    // P, dPdx, dPdy, I, dIdx, dIdy, N, Ng
    for (int i = 0; i < 8; i++) {
        sg_types.push_back(vec3_type);
    }
    // u, v, dudx, dudy, dvdx, dvdy
    for (int i = 0; i < 6; i++) {
        sg_types.push_back(llvm_type_float());
    }
    // dPdu, dPdv
    for (int i = 0; i < 2; i++) {
        sg_types.push_back(vec3_type);
    }
    // time, dtime
    for (int i = 0; i < 2; i++) {
        sg_types.push_back(llvm_type_float());
    }
    // dPdtime, Ps, dPsdx, dPdsy
    for (int i = 0; i < 4; i++) {
        sg_types.push_back(vec3_type);
    }
    // void* renderstate, object2common, shader2common
    for (int i = 0; i < 3; i++) {
        sg_types.push_back(llvm_type_void_ptr());
    }
    // ClosureColor* (treat as void for now?)
    for (int i = 0; i < 1; i++) {
        sg_types.push_back(llvm_type_void_ptr());
    }
    // surfacearea
    for (int i = 0; i < 1; i++) {
        sg_types.push_back(llvm_type_float());
    }
    // iscameraray, isshadowray, flipHandedness
    for (int i = 0; i < 3; i++) {
        sg_types.push_back(bool_type);
    }

    return llvm::StructType::get (llvm_context(), sg_types);
}



/// Convert the name of a global (and its derivative index) into the
/// field number of the ShaderGlobals struct.
static int
ShaderGlobalNameToIndex (ustring name, int deriv)
{
    int sg_index = -1;
    if (name == Strings::P) {
        switch (deriv) {
        case 0: sg_index = 0; break; // P
        case 1: sg_index = 1; break; // dPdx
        case 2: sg_index = 2; break; // dPdy
        default: break;
        }
    } else if (name == Strings::I) {
        switch (deriv) {
        case 0: sg_index = 3; break; // I
        case 1: sg_index = 4; break; // dIdx
        case 2: sg_index = 5; break; // dIdy
        default: break;
        }
    } else if (name == Strings::N) {
        sg_index = 6;
    } else if (name == Strings::Ng) {
        sg_index = 7;
    } else if (name == Strings::u) {
        switch (deriv) {
        case 0: sg_index = 8; break; // u
        case 1: sg_index = 10; break; // dudx
        case 2: sg_index = 11; break; // dudy
        default: break;
        }
    } else if (name == Strings::v) {
        switch (deriv) {
        case 0: sg_index = 9; break; // v
        case 1: sg_index = 12; break; // dvdx
        case 2: sg_index = 13; break; // dvdy
        default: break;
        }
        //extern ustring P, I, N, Ng, dPdu, dPdv, u, v, time, dtime, dPdtime, Ps;
    } else if (name == Strings::dPdu) {
        sg_index = 14;
    } else if (name == Strings::dPdv) {
        sg_index = 15;
    } else if (name == Strings::time) {
        sg_index = 16;
    } else if (name == Strings::dtime) {
        sg_index = 17;
    } else if (name == Strings::dPdtime) {
        sg_index = 18;
    } else if (name == Strings::Ps) {
        switch (deriv) {
        case 0: sg_index = 19; break; // Ps
        case 1: sg_index = 20; break; // dPsdx
        case 2: sg_index = 21; break; // dPsdy
        default: break;
        }
    }
    return sg_index;
}



static bool
SkipSymbol (const Symbol& s)
{
    if (s.symtype() == SymTypeOutputParam)
        return true;

//    if (s.typespec().simpletype().basetype == TypeDesc::STRING)
//        return true;

    if (s.typespec().is_closure())
        return true;

    if (s.typespec().is_structure())
        return true;

    if (s.symtype() == SymTypeParam) {
        // Skip connections
        if (s.valuesource() == Symbol::ConnectedVal) return true;
        // Skip user-data
        if (!s.lockgeom()) return true;

        // Skip params with init ops
        if (s.initbegin() != s.initend()) return true;
    }

    return false;
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
    llvm::Value *val = builder().CreateGlobalString(s.c_str());
    val = builder().CreateConstGEP2_32 (val, 0, 0);
    return val;
}



const llvm::Type *
RuntimeOptimizer::llvm_type (const TypeSpec &typespec)
{
    if (typespec.is_closure())
        return llvm_type_void_ptr();
    TypeDesc t = typespec.simpletype();
    if (t == TypeDesc::FLOAT)
        return llvm_type_float();
    if (t == TypeDesc::INT)
        return llvm_type_int();
    if (t == TypeDesc::STRING)
        return llvm_type_string();
    if (t.aggregate == 3 || t.aggregate == 16)
        return llvm_type_float_ptr();
    if (t == TypeDesc::NONE)
        return llvm_type_void();
    std::cerr << "Bad llvm_type(" << typespec.c_str() << ")\n";
    ASSERT (0 && "not handling this type yet");
    return NULL;
}



void
RuntimeOptimizer::llvm_zero_derivs (Symbol &sym)
{
    if (sym.has_derivs() &&
            (sym.typespec().is_float() || sym.typespec().is_triple())) {
        int num_components = sym.typespec().simpletype().aggregate;
        llvm::Value *zero = llvm_constant (0.0f);
        for (int i = 0;  i < num_components;  ++i)
            storeLLVMValue (zero, sym, i, 1);  // clear dx
        for (int i = 0;  i < num_components;  ++i)
            storeLLVMValue (zero, sym, i, 2);  // clear dy
    }
}



extern "C" void
osl_printf (const char* format_str, ...)
{
    // FIXME -- no, no, we need to take a ShadingSys ref and go through
    // the preferred output mechanisms.
    va_list args;
    va_start (args, format_str);
#if 0
    // Make super sure we know we are excuting LLVM-generated code!
    std::string newfmt = std::string("llvm: ") + format_str;
    format_str = newfmt.c_str();
#endif
    vprintf (format_str, args);
    va_end (args);
}



/// Given the OSL symbol, return the llvm::Value* corresponding to the
/// start of that symbol (first element, first component, and just the
/// plain value if it has derivatives).
llvm::Value *
RuntimeOptimizer::getLLVMSymbolBase (const Symbol &sym)
{
    Symbol* dealiased = sym.dealias();
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
RuntimeOptimizer::getOrAllocateLLVMSymbol (const Symbol& sym,
                                           llvm::Function* f)
{
    Symbol* dealiased = sym.dealias();
    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values().find(mangled_name);

    if (map_iter == named_values().end()) {
        bool has_derivs = sym.has_derivs();
        int num_components = sym.typespec().simpletype().aggregate;
        int total_size = num_components * (has_derivs ? 3 : 1);
        llvm::IRBuilder<> tmp_builder (&f->getEntryBlock(), f->getEntryBlock().begin());
        //shadingsys().info ("Making a type with %d %ss for symbol '%s'\n", total_size, (is_float) ? "float" : "int", mangled_name.c_str());
        llvm::AllocaInst* allocation = 0;
        if (total_size == 1) {
            llvm::ConstantInt* type_len = llvm::ConstantInt::get(llvm_context(), llvm::APInt(32, total_size));
            allocation = tmp_builder.CreateAlloca(llvm_type(sym.typespec().simpletype()), type_len, mangled_name.c_str());
        } else {
            std::vector<const llvm::Type*> types(total_size, llvm_type(sym.typespec().simpletype()));
            llvm::StructType* struct_type = llvm::StructType::get(llvm_context(), types);
            llvm::ConstantInt* num_structs = llvm::ConstantInt::get(llvm_context(), llvm::APInt(32, 1));
            allocation = tmp_builder.CreateAlloca(struct_type, num_structs, mangled_name.c_str());
        }

        //outs() << "Allocation = " << *allocation << "\n";
        named_values()[mangled_name] = allocation;
        return allocation;
    }
    return map_iter->second;
}



llvm::Value *
RuntimeOptimizer::LLVMLoadShaderGlobal (const Symbol& sym, int component,
                                        int deriv, bool ptr)
{
    int sg_index = ShaderGlobalNameToIndex (sym.name(), deriv);
    //shadingsys().info ("Global '%s' has sg_index = %d\n", sym.name().c_str(), sg_index);
    if (sg_index == -1) {
        shadingsys().error ("Warning unhandled global '%s'", sym.name().c_str());
        return NULL;
    }

    int num_elements = sym.typespec().simpletype().aggregate;
    int real_component = std::min (num_elements, component);

    llvm::Value* field = builder().CreateConstGEP2_32 (sg_ptr(), 0, sg_index);
    if (num_elements == 1) {
        return ptr ? field : builder().CreateLoad (field);
    } else {
        llvm::Value* element = builder().CreateConstGEP2_32(field, 0, real_component);
        return ptr ? element : builder().CreateLoad (element);
    }
}



llvm::Value *
RuntimeOptimizer::LLVMStoreShaderGlobal (llvm::Value* val, const Symbol& sym,
                                         int component, int deriv)
{
    shadingsys().error ("WARNING: Store to shaderglobal unsupported!\n");
    return NULL;
}



void
llvm_useparam_op (RuntimeOptimizer &rop, const Symbol& sym, int component, int deriv, 
                  float* fdata, int* idata, ustring* sdata)
{
    // If the param is connected, we need the following sequence:
    // if (!initialized[param]) {
    //   if (!connected_layer[param].already_run()) {
    //     call ConnectedLayer() with sg_ptr;
    //   }
    //   write heap_data[param] into local_params[param];
    // }
}



llvm::Value *
RuntimeOptimizer::LoadParam (const Symbol& sym, int component, int deriv,
                             float* fdata, int* idata, ustring* sdata)
{
    // The local value of the param should have already been filled in
    // by a useparam. So at this point, we just need the following:
    //
    //   return local_params[param];
    //
    return NULL;
}



llvm::Value *
RuntimeOptimizer::loadLLVMValue (const Symbol& sym, int component,
                                 int deriv, TypeDesc cast)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Regardless of what object this is, if it doesn't have derivs but
        // we're asking for them, return 0.  Integers don't have derivs
        // so we don't need to worry about that case.
        ASSERT (sym.typespec().is_floatbased() && cast != TypeDesc::TypeInt);
        return llvm_constant (0.0f);
    }

    // Handle Globals (and eventually Params) separately since they have
    // aliasing stuff and use a different layout than locals.
    if (sym.symtype() == SymTypeGlobal)
        return LLVMLoadShaderGlobal (sym, component, deriv);

    // Get the pointer of the aggregate (the alloca)
    int num_elements = sym.typespec().simpletype().aggregate;
    int real_component = std::min(num_elements, component);
    int index = real_component + deriv * num_elements;
    //shadingsys().info ("Looking up index %d (comp_%d -> realcomp_%d +  %d * %d) for symbol '%s'\n", index, component, real_component, deriv, num_elements, sym.mangled().c_str());
    llvm::Value* result = getLLVMSymbolBase (sym);
    if (!result)
        return 0;  // Error
    if (num_elements == 1 && !has_derivs) {
        // The thing is just a scalar
        result = builder().CreateLoad (result);
    } else {
        llvm::Value* ptr = builder().CreateConstGEP2_32 (result, 0, index);
        result = builder().CreateLoad (ptr);
    }

    // Handle int<->float type casting
    if (sym.typespec().is_floatbased() && cast == TypeDesc::TypeInt)
        result = llvm_float_to_int (result);
    else if (sym.typespec().is_int() && cast == TypeDesc::TypeFloat)
        result = llvm_int_to_float (result);

    return result;
}



llvm::Value *
RuntimeOptimizer::load_llvm_ptr (const Symbol& sym, int deriv)
{
    ASSERT (sym.has_derivs() || deriv == 0);  // doesn't support derivs

    // Handle Globals (and eventually Params) separately since they have
    // aliasing stuff and use a different layout than locals.
    if (sym.symtype() == SymTypeGlobal) {
        return LLVMLoadShaderGlobal (sym, 0, deriv, true);
    }

    // Get the pointer of the aggregate (the alloca)
    int num_elements = sym.typespec().simpletype().aggregate;
    int index = deriv * num_elements;

    llvm::Value* result = getLLVMSymbolBase (sym);
    if (!result)
        return 0;  // Error

    llvm::Value* ptr = builder().CreateConstGEP2_32 (result, 0, index);
    return ptr;
}



bool
RuntimeOptimizer::storeLLVMValue (llvm::Value* new_val, const Symbol& sym,
                                  int component, int deriv)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        shadingsys().error ("Tried to store to symbol '%s', component %d, deriv_idx %d but doesn't have derivatives\n", sym.name().c_str(), component, deriv);
        return false;
    }

    llvm::Value* aggregate = getLLVMSymbolBase (sym);
    if (!aggregate)
        return false;

    int num_elements = sym.typespec().simpletype().aggregate;
    if (num_elements == 1 && !has_derivs) {
        builder().CreateStore (new_val, aggregate);
    } else {
        int real_component = std::min(num_elements, component);
        int index = real_component + deriv * num_elements;
        llvm::Value* ptr = builder().CreateConstGEP2_32 (aggregate, 0, index);
        builder().CreateStore (new_val, ptr);
    }
    return true;
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name,
                                      llvm::Value **args, int nargs)
{
    const char *argcodes = NULL;
    for (int i = 0;  llvm_helper_function_table[i];  i += 2)
        if (! strcmp (name, llvm_helper_function_table[i]))
            argcodes = llvm_helper_function_table[i+1];
    ASSERT (argcodes != NULL && "unfound external function");

    llvm::Function *func = llvm_module()->getFunction (name);
    return builder().CreateCall (func, args, args+nargs);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, 
                                      const Symbol **symargs, int nargs)
{
    std::vector<llvm::Value *> valargs;
    valargs.resize ((size_t)nargs);
    for (int i = 0;  i < nargs;  ++i) {
        const Symbol &s = *(symargs[i]);
        if (s.typespec().is_closure())
            valargs[i] = loadLLVMValue (s);
        else if (s.typespec().simpletype().aggregate > 1)
            valargs[i] = load_llvm_ptr (s);
        else
            valargs[i] = loadLLVMValue (s);
    }
    return llvm_call_function (name, &valargs[0], (int)valargs.size());
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, const Symbol &A)
{
    const Symbol *args[1];
    args[0] = &A;
    return llvm_call_function (name, args, 1);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, const Symbol &A,
                                      const Symbol &B)
{
    const Symbol *args[2];
    args[0] = &A;
    args[1] = &B;
    return llvm_call_function (name, args, 2);
}



llvm::Value *
RuntimeOptimizer::llvm_call_function (const char *name, const Symbol &A,
                                      const Symbol &B, const Symbol &C)
{
    const Symbol *args[3];
    args[0] = &A;
    args[1] = &B;
    args[2] = &C;
    return llvm_call_function (name, args, 3);
}



LLVMGEN (llvm_gen_printf)
{
    Opcode &op (rop.inst()->ops()[opnum]);

    // Prepare the args for the call
    llvm::SmallVector<llvm::Value*, 16> call_args;
    Symbol& format_sym = *rop.opargsym (op, 0);
    if (!format_sym.is_constant()) {
        rop.shadingsys().warning ("printf must currently have constant format\n");
        return false;
    }

    // We're going to need to adjust the format string as we go, but I'd
    // like to reserve a spot for the char*.
    call_args.push_back(NULL);

    ustring format_ustring = *((ustring*)format_sym.data());
    const char* format = format_ustring.c_str();
    std::string s;
    int arg = 0;
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
            Symbol& sym (*rop.opargsym (op, 1 + arg));
            if (SkipSymbol(sym)) {
                rop.shadingsys().warning ("symbol type for '%s' unsupported for printf\n", sym.mangled().c_str());
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


    //shadingsys().info ("llvm printf. Original string = '%s', new string = '%s'",
    //     format_ustring.c_str(), s.c_str());

    llvm::Value* llvm_str = rop.builder().CreateGlobalString(s.c_str());
    llvm::Value* llvm_ptr = rop.builder().CreateConstGEP2_32(llvm_str, 0, 0);
    //outs() << "Type of format string (CreateGlobalString) = " << *llvm_str << "\n";
    //outs() << "llvm_ptr after GEP = " << *llvm_ptr << "\n";
    call_args[0] = llvm_ptr;

    rop.llvm_call_function ("osl_printf", &call_args[0], (int)call_args.size());
    //outs() << "printf_call = " << *printf_call << "\n";
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
        } if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_mul_mf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_mul_mm", Result, A, B);
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
        } if (A.typespec().is_matrix()) {
            if (B.typespec().is_float())
                rop.llvm_call_function ("osl_div_mf", Result, A, B);
            else if (B.typespec().is_matrix())
                rop.llvm_call_function ("osl_div_mm", Result, A, B);
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



// Simple (pointwise) unary ops (Neg, Abs, Sqrt, Ceil, Floor, ..., Log2,
// Log10, Erf, Erfc, IsNan/IsInf/IsFinite)
LLVMGEN (llvm_gen_unary_op)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src = *rop.opargsym (op, 1);
    if (SkipSymbol(dst) ||
        SkipSymbol(src))
        return false;

    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();
    bool src_float = src.typespec().is_floatbased();
    const llvm::Type* float_ty = rop.llvm_type_float();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        llvm::Value* src_load = rop.loadLLVMValue (src, i, 0);
        if (!src_load) return false;

        llvm::Value* src_val = src_load;

        // Perform the op
        llvm::Value* result = 0;
        ustring opname = op.opname();

        if (opname == op_neg) {
            result = (src_float) ? rop.builder().CreateFNeg(src_val) : rop.builder().CreateNeg(src_val);
        } else if (opname == op_abs ||
                   opname == op_fabs) {
            if (src_float) {
                // Call fabsf
                result = rop.llvm_call_function ("fabsf", &src_val, 1);
            } else {
                // neg_version = -x
                // result = (x < 0) ? neg_version : x
                llvm::Value* negated = rop.builder().CreateNeg(src_val);
                llvm::Value* cond = rop.builder().CreateICmpSLT(src_val, rop.llvm_constant(0));
                result = rop.builder().CreateSelect(cond, negated, src_val);
            }
        } else if (opname == op_sqrt && src_float) {
            result = rop.builder().CreateCall(llvm::Intrinsic::getDeclaration(rop.llvm_module(), llvm::Intrinsic::sqrt, &float_ty, 1), src_val);
        } else if (opname == op_sin && src_float) {
            result = rop.builder().CreateCall(llvm::Intrinsic::getDeclaration(rop.llvm_module(), llvm::Intrinsic::sin, &float_ty, 1), src_val);
        } else if (opname == op_cos && src_float) {
            result = rop.builder().CreateCall(llvm::Intrinsic::getDeclaration(rop.llvm_module(), llvm::Intrinsic::cos, &float_ty, 1), src_val);
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



extern "C" void
osl_closure_clear (ClosureColor *r, float f)
{
    r->clear ();
}

extern "C" void
osl_closure_assign (ClosureColor *r, ClosureColor *x)
{
    *r = *x;
}

extern "C" void
osl_add_closure_closure (ClosureColor *r, ClosureColor *a, ClosureColor *b)
{
    r->add (*a, *b);
}

extern "C" void
osl_mul_closure_float (ClosureColor *r, ClosureColor *a, float b)
{
    *r = *a;
    *r *= b;
}

extern "C" void
osl_mul_closure_color (ClosureColor *r, ClosureColor *a, Color3 *b)
{
    *r = *a;
    *r *= *b;
}


extern "C" void
osl_mul_mm (Matrix44 *r, Matrix44 *a, Matrix44 *b)
{
    *r = (*a) * (*b);
}

extern "C" void
osl_mul_mf (Matrix44 *r, Matrix44 *a, float b)
{
    *r = (*a) * b;
}

extern "C" void
osl_mul_m_ff (Matrix44 *r, float a, float b)
{
    float f = a * b;
    *r = Matrix44 (f,0,0,0, 0,f,0,0, 0,0,f,0, 0,0,0,f);
}

extern "C" void
osl_div_mm (Matrix44 *r, Matrix44 *a, Matrix44 *b)
{
    *r = (*a) * b->inverse();
}

extern "C" void
osl_div_mf (Matrix44 *r, Matrix44 *a, float b)
{
    *r = (*a) * (1.0f/b);
}

extern "C" void
osl_div_fm (Matrix44 *r, float a, Matrix44 *b)
{
    *r = a * b->inverse();
}

extern "C" void
osl_div_m_ff (Matrix44 *r, float a, float b)
{
    float f = (b == 0) ? 0.0f : (a / b);
    *r = Matrix44 (f,0,0,0, 0,f,0,0, 0,0,f,0, 0,0,0,f);
}




// Simple assignment
LLVMGEN (llvm_gen_assign)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& Result (*rop.opargsym (op, 0));
    Symbol& Src (*rop.opargsym (op, 1));
    ASSERT (! Result.typespec().is_structure() &&
            ! Result.typespec().is_array());
    ASSERT (! Src.typespec().is_structure() &&
            ! Src.typespec().is_array());

    if (Result.typespec().is_closure() || Src.typespec().is_closure()) {
        llvm::Value* call_args[2];
        int nargs = 0;
        const char *funcname;
        call_args[nargs++] = rop.loadLLVMValue (Result);
        call_args[nargs++] = rop.loadLLVMValue (Src);
        if (Src.typespec().is_closure())
            funcname = "osl_closure_clear";
        else
            funcname = "osl_closure_assign";
        rop.llvm_call_function (funcname, call_args, nargs);
        return false;
    }
    
    if (Result.typespec().is_matrix() && Src.typespec().is_int_or_float()) {
        // Handle m=f, m=i separately
        llvm::Value *src = rop.loadLLVMValue (Src, 0, 0, TypeDesc::FLOAT /*cast*/);
        // m=f sets the diagonal components to f, the others to zero
        llvm::Value *zero = rop.llvm_constant (0.0f);
        for (int i = 0;  i < 4;  ++i)
            for (int j = 0;  j < 4;  ++j)
                rop.storeLLVMValue (i==j ? src : zero, Result, i*4+j);
        ASSERT (! Result.has_derivs() && "matrices shouldn't have derivs");
        return true;
    }

    // The following code handles f=f, f=i, v=v, v=f, v=i, m=m, s=s.
    // Remember that loadLLVMValue will automatically convert scalar->triple.
    TypeDesc rt = Result.typespec().simpletype();
    TypeDesc basetype = TypeDesc::BASETYPE(rt.basetype);
    int num_components = rt.aggregate;
    for (int i = 0; i < num_components; ++i) {
        llvm::Value* src_val = rop.loadLLVMValue (Src, i, 0, basetype);
        if (!src_val)
            return false;
        rop.storeLLVMValue (src_val, Result, i, 0);
    }

    // Handle derivatives
    if (Result.has_derivs()) {
        if (Src.has_derivs()) {
            // src and result both have derivs -- copy them
            for (int d = 1;  d <= 2;  ++d) {
                for (int i = 0; i < num_components; ++i) {
                    llvm::Value* val = rop.loadLLVMValue (Src, i, d);
                    rop.storeLLVMValue (val, Result, i, d);
                }
            }
        } else {
            // Result wants derivs but src didn't have them -- zero them
            rop.llvm_zero_derivs (Result);
        }
    }
    return true;
}



// Component reference
LLVMGEN (llvm_gen_compref)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst = *rop.opargsym (op, 0);
    Symbol& src = *rop.opargsym (op, 1);
    Symbol& index = *rop.opargsym (op, 2);
    if (SkipSymbol(dst) ||
        SkipSymbol(src) ||
        SkipSymbol(index))
        return false;

    bool dst_derivs = dst.has_derivs();
    int num_components = src.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();
    bool src_float = src.typespec().is_floatbased();

    // Get src component index
    if (!index.is_constant()) {
        rop.shadingsys().info ("punting on non-constant index for now. annoying\n");
        // FIXME
        return false;
    }
    int const_index = *((int*)index.data());
    if (const_index < 0 || const_index >= num_components) {
        rop.shadingsys().warning ("index out of range for object (idx = %d, num_comp = %d)\n", const_index, num_components);
        return false;
    }

    llvm::Value* src_val = rop.loadLLVMValue (src, const_index, 0);
    if (!src_val) return false;

    // Perform the assignment
    if (dst_float && !src_float) {
        // need int -> float
        src_val = rop.builder().CreateSIToFP(src_val, rop.llvm_type_float());
    } else if (!dst_float && src_float) {
        // float -> int
        src_val = rop.builder().CreateFPToSI(src_val, rop.llvm_type_int());
    }

    // compref is: scalar = vector[int]
    rop.storeLLVMValue (src_val, dst, 0, 0);

    if (dst_derivs) {
        // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
        rop.shadingsys().info ("punting on derivatives for now\n");
        // FIXME
    }
    return true;
}



// Simple aggregate constructor (no conversion)
LLVMGEN (llvm_gen_construct_aggregate)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst = *rop.opargsym (op, 0);
    Symbol& arg1 = *rop.opargsym (op, 1);
    if (arg1.typespec().is_string()) {
        // Using a string to say what space we want, punt for now.
        return false;  // FIXME
    }
    // Otherwise, the args are just data.
    const Symbol* src_syms[16];
    for (int i = 1; i < op.nargs(); i++)
        src_syms[i-1] = rop.opargsym (op, i);

    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();

    for (int i = 0; i < num_components; i++) {
        const Symbol& src = *src_syms[i];
        bool src_float = src.typespec().is_floatbased();
        // Get src component 0 (it should be a scalar)
        llvm::Value* src_val = rop.loadLLVMValue (src, 0, 0);
        if (!src_val) return false;

        // Perform the assignment
        if (dst_float && !src_float) {
            // need int -> float
            src_val = rop.builder().CreateSIToFP(src_val, rop.llvm_type_float());
        } else if (!dst_float && src_float) {
            // float -> int
            src_val = rop.builder().CreateFPToSI(src_val, rop.llvm_type_int());
        }
        rop.storeLLVMValue (src_val, dst, i, 0);

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            rop.shadingsys().info ("punting on derivatives for now\n");
            // FIXME
        }
    }
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



// unary reduction ops (length, luminance, determinant (much more complicated...))
LLVMGEN (llvm_gen_unary_reduction)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src = *rop.opargsym (op, 1);
    if (SkipSymbol(dst) ||
        SkipSymbol(src))
        return false;

    bool dst_derivs = dst.has_derivs();
    // Loop over the source
    int num_components = src.typespec().simpletype().aggregate;

    llvm::Value* final_result = 0;
    ustring opname = op.opname();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        llvm::Value* src_load = rop.loadLLVMValue (src, i, 0);

        if (!src_load) return false;

        llvm::Value* src_val = src_load;

        // Perform the op
        llvm::Value* result = 0;

        if (opname == op_length) {
            result = rop.builder().CreateFMul(src_val, src_val);
        } else if (opname == op_luminance) {
            float coeff = 0.f;
            switch (i) {
            case 0: coeff = .2126f; break;
            case 1: coeff = .7152f; break;
            default: coeff = .0722f; break;
            }
            result = rop.builder().CreateFMul(src_val, llvm::ConstantFP::get(rop.llvm_context(), llvm::APFloat(coeff)));
        } else {
            // Don't know how to handle this.
            rop.shadingsys().error ("Don't know how to handle op '%s', eliding the store\n", opname.c_str());
        }

        if (result) {
            if (final_result) {
                final_result = rop.builder().CreateFAdd(final_result, result);
            } else {
                final_result = result;
            }
        }
    }

    if (final_result) {
        // Compute sqrtf(result) if it's length instead of luminance
        if (opname == op_length) {
            // Take sqrt
            const llvm::Type* float_ty = rop.llvm_type_float();
            final_result = rop.builder().CreateCall(llvm::Intrinsic::getDeclaration(rop.llvm_module(), llvm::Intrinsic::sqrt, &float_ty, 1), final_result);
        }

        rop.storeLLVMValue (final_result, dst, 0, 0);
        if (dst_derivs) {
            rop.shadingsys().info ("punting on derivatives for now\n");
            // FIXME
        }
    }
    return true;
}



// dot. This is could easily be a more general f(Agg, Agg) -> Scalar,
// but we don't seem to have any others.
LLVMGEN (llvm_gen_dot)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src1 = *rop.opargsym (op, 1);
    Symbol& src2 = *rop.opargsym (op, 2);
    if (SkipSymbol(dst) ||
        SkipSymbol(src1) ||
        SkipSymbol(src2))
        return false;

    bool dst_derivs = dst.has_derivs();
    // Loop over the sources
    int num_components = src1.typespec().simpletype().aggregate;

    llvm::Value* final_result = 0;

    for (int i = 0; i < num_components; i++) {
        // Get src1/src2 component i
        llvm::Value* src1_load = rop.loadLLVMValue (src1, i, 0);
        llvm::Value* src2_load = rop.loadLLVMValue (src2, i, 0);

        if (!src1_load || !src2_load) return false;

        llvm::Value* result = rop.builder().CreateFMul(src1_load, src2_load);

        if (final_result) {
            final_result = rop.builder().CreateFAdd(final_result, result);
        } else {
            final_result = result;
        }
    }

    rop.storeLLVMValue (final_result, dst, 0, 0);
    if (dst_derivs) {
        rop.shadingsys().info ("punting on derivatives for now\n");
        // FIXME
    }
    return true;
}



// cross.
LLVMGEN (llvm_gen_cross)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src1 = *rop.opargsym (op, 1);
    Symbol& src2 = *rop.opargsym (op, 2);
    if (SkipSymbol(dst) ||
        SkipSymbol(src1) ||
        SkipSymbol(src2))
        return false;

    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    for (int i = 0; i < num_components; i++) {
        // Get src1/src2 component for output i
        int src1_idx0[3] = { 1, 2, 0 };
        int src1_idx1[3] = { 2, 0, 1 };

        int src2_idx0[3] = { 2, 0, 1 };
        int src2_idx1[3] = { 1, 2, 0 };

        llvm::Value* src1_load0 = rop.loadLLVMValue (src1, src1_idx0[i], 0);
        llvm::Value* src1_load1 = rop.loadLLVMValue (src1, src1_idx1[i], 0);

        llvm::Value* src2_load0 = rop.loadLLVMValue (src2, src2_idx0[i], 0);
        llvm::Value* src2_load1 = rop.loadLLVMValue (src2, src2_idx1[i], 0);

        if (!src1_load0 || !src1_load1 || !src2_load0 || !src2_load1) return false;

        llvm::Value* prod0 = rop.builder().CreateFMul(src1_load0, src2_load0);
        llvm::Value* prod1 = rop.builder().CreateFMul(src1_load1, src2_load1);
        llvm::Value* result = rop.builder().CreateFSub(prod0, prod1);

        rop.storeLLVMValue (result, dst, i, 0);
        if (dst_derivs) {
            rop.shadingsys().info ("punting on derivatives for now\n");
            // FIXME
        }
    }
    return true;
}



// normalize. This is sort of like unary with a side product (length)
// that we need to then apply to the whole vector. TODO(boulos): Try
// to reuse the length code maybe?
LLVMGEN (llvm_gen_normalize)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& dst  = *rop.opargsym (op, 0);
    Symbol& src = *rop.opargsym (op, 1);
    if (SkipSymbol(dst) ||
        SkipSymbol(src))
        return false;

    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    llvm::Value* length_squared = 0;

    for (int i = 0; i < num_components; i++) {
        // Get src component i
        llvm::Value* src_load = rop.loadLLVMValue (src, i, 0);

        if (!src_load) return false;

        llvm::Value* src_val = src_load;

        // Perform the op
        llvm::Value* result = rop.builder().CreateFMul(src_val, src_val);

        if (length_squared) {
            length_squared = rop.builder().CreateFAdd(length_squared, result);
        } else {
            length_squared = result;
        }
    }

    // Take sqrt
    const llvm::Type* float_ty = rop.llvm_type_float();
    llvm::Value* length = rop.builder().CreateCall(llvm::Intrinsic::getDeclaration(rop.llvm_module(), llvm::Intrinsic::sqrt, &float_ty, 1), length_squared);
    // Compute 1/length
    llvm::Value* inv_length = rop.builder().CreateFDiv(llvm::ConstantFP::get(rop.llvm_context(), llvm::APFloat(1.f)), length);

    for (int i = 0; i < num_components; i++) {
        // Get src component i
        llvm::Value* src_load = rop.loadLLVMValue (src, i, 0);

        if (!src_load) return false;

        llvm::Value* src_val = src_load;

        // Perform the op (src_val * inv_length is the order in opvector.cpp)
        llvm::Value* result = rop.builder().CreateFMul(src_val, inv_length);

        rop.storeLLVMValue (result, dst, i, 0);
        if (dst_derivs) {
            rop.shadingsys().info ("punting on derivatives for now\n");
            // FIXME
        }
    }
    return true;
}



LLVMGEN (llvm_gen_if)
{
    Opcode &op (rop.inst()->ops()[opnum]);
    Symbol& cond = *rop.opargsym (op, 0);
    if (SkipSymbol(cond))
        return false;
    RuntimeOptimizer::BasicBlockMap& bb_map (rop.bb_map());

    // Load the condition variable
    llvm::Value* cond_load = rop.loadLLVMValue (cond, 0, 0);
    // Convert the int to a bool via truncation
    llvm::Value* cond_bool = rop.builder().CreateTrunc (cond_load, rop.llvm_type_bool());
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
    if (SkipSymbol(cond))
        return false;
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
    // Convert the int to a bool via truncation
    llvm::Value* cond_bool = rop.builder().CreateTrunc(cond_load, rop.llvm_type_bool());
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
RuntimeOptimizer::llvm_assign_initial_constant (const Symbol& sym)
{
    ASSERT (sym.is_constant() && ! sym.has_derivs());
    int num_components = sym.typespec().simpletype().aggregate;
    for (int i = 0; i < num_components; ++i) {
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

    INIT (assign);
    INIT (add);
    INIT (sub);
    INIT (mul);
    INIT (div);
    INIT (mod);
    INIT (dot);
    INIT (cross);
    INIT (normalize);
    INIT (compref);
    INIT2 (eq, llvm_gen_compare_op);
    INIT2 (neq, llvm_gen_compare_op);
    INIT2 (lt, llvm_gen_compare_op);
    INIT2 (le, llvm_gen_compare_op);
    INIT2 (gt, llvm_gen_compare_op);
    INIT2 (ge, llvm_gen_compare_op);
    INIT2 (neg, llvm_gen_unary_op);
    INIT2 (abs, llvm_gen_unary_op);
    INIT2 (fabs, llvm_gen_unary_op);
    INIT2 (sqrt, llvm_gen_unary_op);
    INIT2 (sin, llvm_gen_unary_op);
    INIT2 (cos, llvm_gen_unary_op);
    INIT2 (vector, llvm_gen_construct_aggregate);
    INIT2 (color, llvm_gen_construct_aggregate);
    INIT2 (length, llvm_gen_unary_reduction);
    INIT2 (luminance, llvm_gen_unary_reduction);
    INIT (if);
    INIT2 (for, llvm_gen_loop_op);
    INIT2 (while, llvm_gen_loop_op);
    INIT2 (dowhile, llvm_gen_loop_op);
    INIT (printf);

#undef INIT
#undef INIT2

    table_initialized = true;
}



llvm::Function*
RuntimeOptimizer::build_llvm_version ()
{
    initialize_llvm_stuff ();   // setup

    llvm::Module *all_ops (m_llvm_module);
    m_named_values.clear ();

    // I'd like our new function to take just a ShaderGlobals...
    char unique_layer_name[1024];
    sprintf (unique_layer_name, "%s_%d", inst()->layername().c_str(), inst()->id());
    const llvm::StructType* sg_type = getShaderGlobalType ();
    llvm::PointerType* sg_ptr_type = llvm::PointerType::get(sg_type, 0 /* Address space */);
    // Make a layer function: void layer_func(ShaderGlobal*)
    m_layer_func = llvm::cast<llvm::Function>(all_ops->getOrInsertFunction(unique_layer_name, llvm_type_void(), sg_ptr_type, NULL));
    const OpcodeVec& instance_ops (inst()->ops());
    llvm::Function::arg_iterator arg_it = m_layer_func->arg_begin();
    // Get shader globals pointer
    m_llvm_shaderglobals_ptr = arg_it++;

    llvm::BasicBlock* entry_bb = llvm::BasicBlock::Create (llvm_context(), "EntryBlock", m_layer_func);

    delete m_builder;
    m_builder = new llvm::IRBuilder<> (entry_bb);

    // Setup the symbols
    BOOST_FOREACH (Symbol &s, inst()->symbols()) {
        if (SkipSymbol(s))
            continue;
        // Don't allocate globals
        if (s.symtype() == SymTypeGlobal)
            continue;
        // Make space
        getOrAllocateLLVMSymbol (s, m_layer_func);
        if (s.is_constant())
            llvm_assign_initial_constant (s);
    }

    // All the symbols are stack allocated now.

    // Mark all the basic blocks, including allocating llvm::BasicBlock
    // records for each.
    find_basic_blocks (true);

    for (size_t opnum = 0; opnum < instance_ops.size(); ++opnum) {
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

        //rop.shadingsys().info ("op%03zu: %s\n", i, op.opname().c_str());

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

    builder().CreateRetVoid();

    llvm::errs() << "layer_func (" << unique_layer_name << ") after llvm  = " << *m_layer_func << "\n";

    // Now optimize the result
    shadingsys().FunctionOptimizer()->run(*m_layer_func);

    llvm::errs() << "layer_func (" << unique_layer_name << ") after opt  = " << *m_layer_func << "\n";

    inst()->llvm_version = m_layer_func;

    delete m_builder;
    m_builder = NULL;

    return m_layer_func;
}



void
RuntimeOptimizer::initialize_llvm_stuff ()
{
    // Set up aliases for types we use over and over
    m_llvm_type_float = llvm::Type::getFloatTy (*m_llvm_context);
    m_llvm_type_int = llvm::Type::getInt32Ty (*m_llvm_context);
    m_llvm_type_bool = llvm::Type::getInt1Ty (*m_llvm_context);
    m_llvm_type_void = llvm::Type::getVoidTy (*m_llvm_context);
    m_llvm_type_char_ptr = llvm::Type::getInt8PtrTy (*m_llvm_context);
    m_llvm_type_float_ptr = llvm::Type::getFloatPtrTy (*m_llvm_context);


    // Now we have things we only need to do once for each context.
    static spin_mutex mutex;
    static llvm::LLVMContext *initialized_context = NULL;
    spin_lock lock (mutex);
    if (initialized_context == m_llvm_context)
        return;   // already initialized for this context

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
                params.push_back (llvm_type (t));
            }
            types += advance;
        }
        llvm::FunctionType *func = llvm::FunctionType::get (llvm_type(rettype), params, varargs);
        m_llvm_module->getOrInsertFunction (funcname, func);
    }

#if 0
    std::vector<const llvm::Type*> params;
    llvm::FunctionType *func_type;
    params.clear();
    params.push_back (llvm::Type::getInt8PtrTy(*llvm_context()));
    func_type = llvm::FunctionType::get (llvm::Type::getVoidTy(*llvm_context()), params, true /* varargs */);
    m_llvm_module->getOrInsertFunction ("osl_printf", func_type);

    params.clear ();
    params.push_back (llvm::Type::getInt8PtrTy(*llvm_context()));
    func_type = llvm::FunctionType::get (llvm::Type::getFloatTy(*llvm_context()), params, false /* varargs */);
    m_llvm_module->getOrInsertFunction ("osl_closure_clear", func_type);
    
    params.clear ();
    params.push_back (llvm::Type::getInt8PtrTy(*llvm_context()));
    params.push_back (llvm::Type::getInt8PtrTy(*llvm_context()));
    func_type = llvm::FunctionType::get (llvm::Type::getFloatTy(*llvm_context()), params, false /* varargs */);
    m_llvm_module->getOrInsertFunction ("osl_closure_assign", func_type);
#endif

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

    //printf("Loading LLVM Bitcode\n");
    m_llvm_module = new llvm::Module ("oslmodule", *llvm_context());

    info ("Building an Execution Engine");
    std::string error_msg;
    m_llvm_exec = llvm::ExecutionEngine::create(m_llvm_module,
                                                false,
                                                &error_msg,
                                                llvm::CodeGenOpt::Default,
                                                false);
    if (! m_llvm_exec) {
        error ("Failed to create engine: %s\n", error_msg.c_str());
        return;
    }

    //shadingsys().info ("Disabling lazy JIT\n");
    //m_llvm_exec->DisableLazyCompilation();
    info ("Setting up pass managers");
    SetupLLVMOptimizer();
    //IPOOptimizer()->run(*all_ops);
    //shadingsys().info ("LLVM ready!\n");

    initialize_llvm_generator_table ();
}



void
ShadingSystemImpl::SetupLLVMOptimizer ()
{
    //m_opt_ipo = new PassManager();
    //m_opt_ipo->add(llvm::createFunctionInliningPass(2000));

    info ("Making FunctionPassManager");
    m_llvm_opt_function = new llvm::FunctionPassManager(m_llvm_module);
    info ("Adding TargetInfo");
    m_llvm_opt_function->add (new llvm::TargetData(*(m_llvm_exec->getTargetData())));
    // Now change things to registers
    info ("Adding mem2reg");
    m_llvm_opt_function->add (llvm::createPromoteMemoryToRegisterPass());
    // Combine instructions where possible
    info ("Adding instcomb");
    m_llvm_opt_function->add (llvm::createInstructionCombiningPass());
    // resassociate exprssions (a = x + (3 + y) -> a = x + y + 3)
    info ("Adding reassoc");
    m_llvm_opt_function->add (llvm::createReassociatePass());
    // eliminate common sub-expressions
    info ("Adding gvn");
    m_llvm_opt_function->add (llvm::createGVNPass());
    // Simplify the call graph if possible
    info ("Adding simpcfg");
    m_llvm_opt_function->add (llvm::createCFGSimplificationPass());

    info ("Adding DCE");
    m_llvm_opt_function->add (llvm::createAggressiveDCEPass());
    // Try to make stuff into registers one last time.
    info ("Adding mem2reg (again)");
    m_llvm_opt_function->add (llvm::createPromoteMemoryToRegisterPass());

    // Always add verifier?
    info ("Adding verifier");
    m_llvm_opt_function->add (llvm::createVerifierPass());

    info ("Performing init");
    m_llvm_opt_function->doInitialization();
}



}; // namespace pvt
}; // namespace osl

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif
