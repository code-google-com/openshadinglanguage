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

#include <llvm/Support/IRBuilder.h>

using namespace OSL;
using namespace OSL::pvt;
using namespace llvm;

#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {

typedef std::map<std::string, AllocaInst*> AllocationMap;
typedef std::map<int, BasicBlock*> BasicBlockMap;


const StructType *
getShaderGlobalType (LLVMContext &llvm_context)
{
    std::vector<const Type*> vec3_types(3, Type::getFloatTy(llvm_context));
    const StructType* vec3_type = StructType::get(llvm_context, vec3_types);
    const Type* float_type = Type::getFloatTy(llvm_context);
    // NOTE(boulos): Bool is a C++ concept that maps to int in C.
    const Type* bool_type = Type::getInt32Ty(llvm_context);
    const PointerType* void_ptr_type = Type::getInt8PtrTy(llvm_context);

    std::vector<const Type*> sg_types;
    // P, dPdx, dPdy, I, dIdx, dIdy, N, Ng
    for (int i = 0; i < 8; i++) {
        sg_types.push_back(vec3_type);
    }
    // u, v, dudx, dudy, dvdx, dvdy
    for (int i = 0; i < 6; i++) {
        sg_types.push_back(float_type);
    }
    // dPdu, dPdv
    for (int i = 0; i < 2; i++) {
        sg_types.push_back(vec3_type);
    }
    // time, dtime
    for (int i = 0; i < 2; i++) {
        sg_types.push_back(float_type);
    }
    // dPdtime, Ps, dPsdx, dPdsy
    for (int i = 0; i < 4; i++) {
        sg_types.push_back(vec3_type);
    }
    // void* renderstate, object2common, shader2common
    for (int i = 0; i < 3; i++) {
        sg_types.push_back(void_ptr_type);
    }
    // ClosureColor* (treat as void for now?)
    for (int i = 0; i < 1; i++) {
        sg_types.push_back(void_ptr_type);
    }
    // surfacearea
    for (int i = 0; i < 1; i++) {
        sg_types.push_back(float_type);
    }
    // iscameraray, isshadowray, flipHandedness
    for (int i = 0; i < 3; i++) {
        sg_types.push_back(bool_type);
    }

    return StructType::get (llvm_context, sg_types);
}



int
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



bool
SkipSymbol (const Symbol& s)
{
    if (s.symtype() == SymTypeOutputParam)
        return true;

    if (s.typespec().simpletype().basetype == TypeDesc::STRING)
        return true;

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



extern "C" void
llvm_osl_printf (const char* format_str, ...)
{
    va_list args;
    va_start(args, format_str);

#if 0
    // NOTE(boulos): Don't need this anymore since we handle it when
    // "upconverting" the format string to handle triples and such.
    std::string my_format;
    const char* fmt_ptr = format_str;
    while (*fmt_ptr != '\0') {
        if (*fmt_ptr == '\\') {
            // It's a slash, escape it for the printf
            ++fmt_ptr;  // skip the backslash
            switch (*fmt_ptr) {
            case 'n' : my_format += '\n';     break;
            case 'r' : my_format += '\r';     break;
            case 't' : my_format += '\t';     break;
            default:   my_format += *fmt_ptr;  break;  // Catches '\\' also!
            }
        } else {
            my_format += *fmt_ptr;
        }
        fmt_ptr++;
    }
    vprintf(my_format.c_str(), args);
#else
    vprintf(format_str, args);
#endif
    va_end(args);
}



Value *
getLLVMSymbolBase (const Symbol &sym, AllocationMap &named_values,
                   Value *sg_ptr)
{
    Symbol* dealiased = sym.dealias();
    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values.find(mangled_name);
    if (map_iter == named_values.end()) {
        printf("ERROR: Couldn't find symbol '%s' (unmangled = '%s'). Did you forget to allocate it?\n",
               mangled_name.c_str(), dealiased->name().c_str());
        return 0;
    }
    return map_iter->second;
}



Value *
getOrAllocateLLVMSymbol (LLVMContext& llvm_context, const Symbol& sym,
                         AllocationMap& named_values, Value* sg_ptr,
                         Function* f)
{
    Symbol* dealiased = sym.dealias();
    std::string mangled_name = dealiased->mangled();
    AllocationMap::iterator map_iter = named_values.find(mangled_name);

    if (map_iter == named_values.end()) {
        bool has_derivs = sym.has_derivs();
        bool is_float = sym.typespec().is_floatbased();
        int num_components = sym.typespec().simpletype().aggregate;
        int total_size = num_components * (has_derivs ? 3 : 1);
        IRBuilder<> tmp_builder(&f->getEntryBlock(),
                                f->getEntryBlock().begin());
        printf("Making a type with %d %ss for symbol '%s'\n", total_size, (is_float) ? "float" : "int", mangled_name.c_str());
        AllocaInst* allocation = 0;
        if (total_size == 1) {
            ConstantInt* type_len = ConstantInt::get(llvm_context, APInt(32, total_size));
            allocation = tmp_builder.CreateAlloca((is_float) ? Type::getFloatTy(llvm_context) : Type::getInt32Ty(llvm_context), type_len, mangled_name.c_str());
        } else {
            std::vector<const Type*> types(total_size, (is_float) ? Type::getFloatTy(llvm_context) : Type::getInt32Ty(llvm_context));
            StructType* struct_type = StructType::get(llvm_context, types);
            ConstantInt* num_structs = ConstantInt::get(llvm_context, APInt(32, 1));
            allocation = tmp_builder.CreateAlloca(struct_type, num_structs, mangled_name.c_str());
        }

        outs() << "Allocation = " << *allocation << "\n";
        named_values[mangled_name] = allocation;
        return allocation;
    }
    return map_iter->second;
}



Value *
LLVMLoadShaderGlobal (const Symbol& sym, int component, int deriv,
                      Value* sg_ptr, IRBuilder<>& builder)
{
    int sg_index = ShaderGlobalNameToIndex(sym.name(), deriv);
    printf("Global '%s' has sg_index = %d\n", sym.name().c_str(), sg_index);
    if (sg_index == -1) {
        printf("Warning unhandled global '%s'", sym.name().c_str());
        return NULL;
    }

    int num_elements = sym.typespec().simpletype().aggregate;
    int real_component = std::min (num_elements, component);

    Value* field = builder.CreateConstGEP2_32 (sg_ptr, 0, sg_index);
    if (num_elements == 1) {
        return builder.CreateLoad (field);
    } else {
        Value* element = builder.CreateConstGEP2_32(field, 0, real_component);
        return builder.CreateLoad (element);
    }
}



Value *
LLVMStoreShaderGlobal (Value* val, const Symbol& sym, int component,
                       int deriv, Value* sg_ptr, IRBuilder<>& builder)
{
    printf("WARNING: Store to shaderglobal unsupported!\n");
    return NULL;
}



void
llvm_useparam_op (const Symbol& sym, int component, int deriv, 
                  Value* sg_ptr, IRBuilder<>& builder, float* fdata,
                  int* idata, ustring* sdata)
{
    // If the param is connected, we need the following sequence:
    // if (!initialized[param]) {
    //   if (!connected_layer[param].already_run()) {
    //     call ConnectedLayer() with sg_ptr;
    //   }
    //   write heap_data[param] into local_params[param];
    // }
}



Value *
LoadParam (const Symbol& sym, int component, int deriv, Value* sg_ptr,
           IRBuilder<>& builder, float* fdata, int* idata, ustring* sdata)
{
    // The local value of the param should have already been filled in
    // by a useparam. So at this point, we just need the following:
    //
    //   return local_params[param];
    //
    return NULL;
}



Value *
loadLLVMValue (LLVMContext &llvm_context, const Symbol& sym, int component,
               int deriv, AllocationMap& named_values, Value* sg_ptr,
               IRBuilder<>& builder)
{
    // Regardless of what object this is, if it doesn't have derivs but we're asking for them, return 0
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        // Asking for the value of our derivative, but we don't have one. Return 0.
        if (sym.typespec().is_floatbased()) {
            return ConstantFP::get (llvm_context, APFloat(0.f));
        } else {
            return ConstantInt::get (llvm_context, APInt(32, 0));
        }
    }

    // Handle Globals (and eventually Params) separately since they have
    // aliasing stuff and use a different layout than locals.
    if (sym.symtype() == SymTypeGlobal)
        return LLVMLoadShaderGlobal (sym, component, deriv, sg_ptr, builder);

    // Get the pointer of the aggregate (the alloca)
    int num_elements = sym.typespec().simpletype().aggregate;
    int real_component = std::min(num_elements, component);
    int index = real_component + deriv * num_elements;
    printf("Looking up index %d (comp_%d -> realcomp_%d +  %d * %d) for symbol '%s'\n", index, component, real_component, deriv, num_elements, sym.mangled().c_str());
    Value* aggregate = getLLVMSymbolBase(sym, named_values, sg_ptr);
    if (!aggregate) return 0;
    if (num_elements == 1 && !has_derivs) {
        // The thing is just a scalar
        return builder.CreateLoad(aggregate);
    } else {
#if 1
        Value* ptr = builder.CreateConstGEP2_32(aggregate, 0, index);
        outs() << "aggregate = " << *aggregate << " and GEP = " << *ptr << "\n";
        return builder.CreateLoad(ptr);
#else
        return builder.CreateExtractValue(aggregate, index);
#endif
    }
}



void
storeLLVMValue (Value* new_val, const Symbol& sym, int component, int deriv,
                AllocationMap& named_values, Value* sg_ptr,
                IRBuilder<>& builder)
{
    bool has_derivs = sym.has_derivs();
    if (!has_derivs && deriv != 0) {
        printf("ERROR: Tried to store to symbol '%s', component %d, deriv_idx %d but doesn't have derivatives\n", sym.name().c_str(), component, deriv);
        return;
    }

    Value* aggregate = getLLVMSymbolBase(sym, named_values, sg_ptr);
    if (!aggregate)
        return;

    int num_elements = sym.typespec().simpletype().aggregate;
    if (num_elements == 1 && !has_derivs) {
        builder.CreateStore(new_val, aggregate);
    } else {
        int real_component = std::min(num_elements, component);
        int index = real_component + deriv * num_elements;
#if 1
        printf("Storing value into index %d (comp_%d -> realcomp_%d +  %d * %d) for symbol '%s'\n", index, component, real_component, deriv, num_elements, sym.mangled().c_str());
        Value* ptr = builder.CreateConstGEP2_32(aggregate, 0, index);
        builder.CreateStore(new_val, ptr);
#else
        builder.CreateInsertValue(aggregate, new_val, index);
#endif
    }
}



Function *
getPrintfFunc (Module* module)
{
    Function* printf_func = module->getFunction("llvm_osl_printf");
    //outs() << "llvm_osl_printf func = " << *printf_func << "\n";
    return printf_func;
}



void
llvm_printf_op (llvm::LLVMContext &context, ShaderInstance* inst,
                const Opcode& op, AllocationMap& named_values, Value* sg_ptr,
                IRBuilder<>& builder, Function* llvm_printf_func)
{
    // Prepare the args for the call
    SmallVector<Value*, 16> call_args;
    Symbol& format_sym = *inst->argsymbol(op.firstarg() + 0);
    if (!format_sym.is_constant()) {
        printf("WARNING: printf must currently have constant format\n");
        return;
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
                printf("ERROR: Mismatch between format string and arguments");
                return;
            }

            std::string ourformat (oldfmt, format);  // straddle the format
            // Doctor it to fix mismatches between format and data
            Symbol& sym (*inst->argsymbol(op.firstarg() + 1 + arg));
            if (SkipSymbol(sym)) {
                printf("WARNING: symbol type for '%s' unsupported for printf\n", sym.mangled().c_str());
                return;
            }
            TypeDesc simpletype (sym.typespec().simpletype());
#if 0
            char code = *format;
            if (sym.typespec().is_closure() && code != 's') {
                ourformat[ourformat.length()-1] = 's';
            } else if (simpletype.basetype == TypeDesc::FLOAT &&
                       code != 'f' && code != 'g') {
                ourformat[ourformat.length()-1] = 'g';
            } else if (simpletype.basetype == TypeDesc::INT && code != 'd') {
                ourformat[ourformat.length()-1] = 'd';
            } else if (simpletype.basetype == TypeDesc::STRING && code != 's') {
                ourformat[ourformat.length()-1] = 's';
            }
            s += exec->format_symbol (ourformat, sym, whichpoint);
#else
            int num_components = simpletype.numelements() * simpletype.aggregate;
            // NOTE(boulos): Only for debug mode do the derivatives get printed...
            for (int i = 0; i < num_components; i++) {
                if (i != 0) s += " ";
                s += ourformat;
                
                Value* loaded = loadLLVMValue (context, sym, i, 0, named_values, sg_ptr, builder);
                // NOTE(boulos): Annoyingly varargs makes it so that things need
                // to be upconverted from float to double (who knew?)
                if (sym.typespec().is_floatbased()) {
                    call_args.push_back(builder.CreateFPExt(loaded, Type::getDoubleTy(inst->shadingsys().getLLVMContext())));
                } else {
                    call_args.push_back(loaded);
                }
            }
#endif
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


    //printf("llvm printf. Original string = '%s', new string = '%s'",
    //     format_ustring.c_str(), s.c_str());

    Value* llvm_str = builder.CreateGlobalString(s.c_str());
    Value* llvm_ptr = builder.CreateConstGEP2_32(llvm_str, 0, 0);
    //outs() << "Type of format string (CreateGlobalString) = " << *llvm_str << "\n";
    //outs() << "llvm_ptr after GEP = " << *llvm_ptr << "\n";
    call_args[0] = llvm_ptr;

    // Get llvm_osl_printf from the module
    builder.CreateCall(llvm_printf_func, call_args.begin(), call_args.end());
    //outs() << "printf_call = " << *printf_call << "\n";
}



Value *
llvm_float_to_int (llvm::LLVMContext &llvm_context, Value* fval,
                   IRBuilder<>& builder)
{
    return builder.CreateFPToSI(fval, Type::getInt32Ty(llvm_context));
}



Value *
llvm_int_to_float (llvm::LLVMContext &llvm_context, Value* ival,
                   IRBuilder<>& builder)
{
    return builder.CreateSIToFP(ival, Type::getFloatTy(llvm_context));
}



// Simple (pointwise) binary ops (+-*/)
void
llvm_binary_op (llvm::LLVMContext &llvm_context, const Opcode& op,
                const Symbol& dst, const Symbol& src1, const Symbol& src2,
                AllocationMap& named_values, Value* sg_ptr,
                IRBuilder<>& builder)
{
    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;
    bool is_float = dst.typespec().is_floatbased();

    bool src1_float = src1.typespec().is_floatbased();
    bool src2_float = src2.typespec().is_floatbased();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        Value* src1_load = loadLLVMValue(llvm_context, src1, i, 0, named_values, sg_ptr, builder);
        Value* src2_load = loadLLVMValue(llvm_context, src2, i, 0, named_values, sg_ptr, builder);

        if (!src1_load) return;
        if (!src2_load) return;

        Value* src1_val = src1_load;
        Value* src2_val = src2_load;

        bool need_float_op = src1_float || src2_float;
        if (need_float_op) {
            // upconvert int -> float for the op if necessary
            if (src1_float && !src2_float) {
                src2_val = llvm_int_to_float(llvm_context, src2_load, builder);
            } else if (!src1_float && src2_float) {
                src1_val = llvm_int_to_float(llvm_context, src1_load, builder);
            } else {
                // both floats, do nothing
            }
        }

        // Perform the op
        Value* result = 0;
        static ustring op_add("add");
        static ustring op_sub("sub");
        static ustring op_mul("mul");
        static ustring op_div("div");
        static ustring op_mod("mod");
        ustring opname = op.opname();

        // Upconvert the value if necessary fr

        if (opname == op_add) {
            result = (need_float_op) ? builder.CreateFAdd(src1_val, src2_val) : builder.CreateAdd(src1_val, src2_val);
        } else if (opname == op_sub) {
            result = (need_float_op) ? builder.CreateFSub(src1_val, src2_val) : builder.CreateSub(src1_val, src2_val);
        } else if (opname == op_mul) {
            result = (need_float_op) ? builder.CreateFMul(src1_val, src2_val) : builder.CreateMul(src1_val, src2_val);
        } else if (opname == op_div) {
            result = (need_float_op) ? builder.CreateFDiv(src1_val, src2_val) : builder.CreateSDiv(src1_val, src2_val);
        } else if (opname == op_mod) {
            result = (need_float_op) ? builder.CreateFRem(src1_val, src2_val) : builder.CreateSRem(src1_val, src2_val);
        } else {
            // Don't know how to handle this.
            printf("WARNING: Don't know how to handle op '%s', eliding the store\n", opname.c_str());
        }

        // Store the result
        if (result) {
            // if our op type doesn't match result, convert
            if (is_float && !need_float_op) {
                // Op was int, but we need to store float
                result = llvm_int_to_float (llvm_context, result, builder);
            } else if (!is_float && need_float_op) {
                // Op was float, but we need to store int
                result = llvm_float_to_int (llvm_context, result, builder);
            } // otherwise just fine
            storeLLVMValue(result, dst, i, 0, named_values, sg_ptr, builder);
        }

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            printf("punting on derivatives for now\n");
        }
    }
}



// Simple (pointwise) unary ops (Neg, Abs, Sqrt, Ceil, Floor, ..., Log2,
// Log10, Erf, Erfc, IsNan/IsInf/IsFinite)
void
llvm_unary_op (llvm::LLVMContext &llvm_context, const Opcode& op,
               const Symbol& dst, const Symbol& src,
               AllocationMap& named_values, Value* sg_ptr, IRBuilder<>& builder)
{
    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();
    bool src_float = src.typespec().is_floatbased();

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        Value* src_load = loadLLVMValue(llvm_context, src, i, 0, named_values, sg_ptr, builder);
        if (!src_load) return;

        Value* src_val = src_load;

        // Perform the op
        Value* result = 0;
        static ustring op_neg("neg");
#if 0
        static ustring op_abs("abs");
        static ustring op_fabs("fabs");
        static ustring op_sqrt("sqrt");
        static ustring op_ceil("ceil");

        static ustring op_log2("log2");
        static ustring op_log10("log10");
        static ustring op_logb("logb");

        static ustring op_exp("exp");
        static ustring op_exp2("exp2");
        static ustring op_expm1("expm1");

        static ustring op_erf("erf");
        static ustring op_erfc("erfc");
#endif

        ustring opname = op.opname();

        if (opname == op_neg) {
            result = (src_float) ? builder.CreateFNeg(src_val) : builder.CreateNeg(src_val);
        } else {
            // Don't know how to handle this.
            printf("WARNING: Don't know how to handle op '%s', eliding the store\n", opname.c_str());
        }

        // Store the result
        if (result) {
            // if our op type doesn't match result, convert
            if (dst_float && !src_float) {
                // Op was int, but we need to store float
                result = llvm_int_to_float (llvm_context, result, builder);
            } else if (!dst_float && src_float) {
                // Op was float, but we need to store int
                result = llvm_float_to_int (llvm_context, result, builder);
            } // otherwise just fine
            storeLLVMValue(result, dst, i, 0, named_values, sg_ptr, builder);
        }

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            printf("punting on derivatives for now\n");
        }
    }
}



// Simple assignment
void
llvm_assign_op (llvm::LLVMContext &llvm_context, const Opcode& op,
                const Symbol& dst, const Symbol& src,
                AllocationMap& named_values, Value* sg_ptr,
                IRBuilder<>& builder)
{
    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool dst_float = dst.typespec().is_floatbased();
    bool src_float = src.typespec().is_floatbased();

    printf("assigning '%s' (mangled = '%s') to '%s' (mangled = '%s')\n", src.name().c_str(), src.mangled().c_str(), dst.name().c_str(), dst.mangled().c_str());

    for (int i = 0; i < num_components; i++) {
        // Get src component i
        Value* src_val = loadLLVMValue(llvm_context, src, i, 0, named_values, sg_ptr, builder);
        if (!src_val) return;

        // Perform the assignment
        if (dst_float && !src_float) {
            // need int -> float
            src_val = builder.CreateSIToFP(src_val, Type::getFloatTy(llvm_context));
        } else if (!dst_float && src_float) {
            // float -> int
            src_val = builder.CreateFPToSI(src_val, Type::getInt32Ty(llvm_context));
        }
        storeLLVMValue(src_val, dst, i, 0, named_values, sg_ptr, builder);

        if (dst_derivs) {
            // mul results in <a * b, a * b_dx + b * a_dx, a * b_dy + b * a_dy>
            printf("punting on derivatives for now\n");
        }
    }
}



// Comparison ops (though other binary -> scalar ops like dot might end
// up being similar)
void
llvm_compare_op (llvm::LLVMContext &llvm_context, const Opcode& op,
                 const Symbol& dst, const Symbol& src1, const Symbol& src2,
                 AllocationMap& named_values, Value* sg_ptr,
                 IRBuilder<>& builder)
{
    bool dst_derivs = dst.has_derivs();
    int num_components = dst.typespec().simpletype().aggregate;

    bool src1_float = src1.typespec().is_floatbased();
    bool src2_float = src2.typespec().is_floatbased();

    Value* final_result = 0;

    for (int i = 0; i < num_components; i++) {
        // Get src1/2 component i
        Value* src1_load = loadLLVMValue(llvm_context, src1, i, 0, named_values, sg_ptr, builder);
        Value* src2_load = loadLLVMValue(llvm_context, src2, i, 0, named_values, sg_ptr, builder);

        if (!src1_load) return;
        if (!src2_load) return;

        Value* src1_val = src1_load;
        Value* src2_val = src2_load;

        bool need_float_op = src1_float || src2_float;
        if (need_float_op) {
            // upconvert int -> float for the op if necessary
            if (src1_float && !src2_float) {
                src2_val = llvm_int_to_float (llvm_context, src2_load, builder);
            } else if (!src1_float && src2_float) {
                src1_val = llvm_int_to_float (llvm_context, src1_load, builder);
            } else {
                // both floats, do nothing
            }
        }

        // Perform the op
        Value* result = 0;

        static ustring op_lt("lt");
        static ustring op_le("le");
        static ustring op_eq("eq");
        static ustring op_ge("ge");
        static ustring op_gt("gt");
        static ustring op_neq("neq");

        ustring opname = op.opname();

        // Upconvert the value if necessary fr

        if (opname == op_lt) {
            result = (need_float_op) ? builder.CreateFCmpULT(src1_val, src2_val) : builder.CreateICmpSLT(src1_val, src2_val);
        } else if (opname == op_le) {
            result = (need_float_op) ? builder.CreateFCmpULE(src1_val, src2_val) : builder.CreateICmpSLE(src1_val, src2_val);
        } else if (opname == op_eq) {
            result = (need_float_op) ? builder.CreateFCmpUEQ(src1_val, src2_val) : builder.CreateICmpEQ(src1_val, src2_val);
        } else if (opname == op_ge) {
            result = (need_float_op) ? builder.CreateFCmpUGE(src1_val, src2_val) : builder.CreateICmpSGE(src1_val, src2_val);
        } else if (opname == op_gt) {
            result = (need_float_op) ? builder.CreateFCmpUGT(src1_val, src2_val) : builder.CreateICmpSGT(src1_val, src2_val);
        } else if (opname == op_neq) {
            result = (need_float_op) ? builder.CreateFCmpUNE(src1_val, src2_val) : builder.CreateICmpNE(src1_val, src2_val);
        } else {
            // Don't know how to handle this.
            printf("WARNING: Don't know how to handle op '%s', eliding the store\n", opname.c_str());
        }

        if (result) {
            if (final_result) {
                // Combine the component bool based on the op
                if (opname != op_neq) {
                    // final_result &= result
                    final_result = builder.CreateAnd(final_result, result);
                } else {
                    // final_result |= result
                    final_result = builder.CreateOr(final_result, result);
                }
            } else {
                final_result = result;
            }
        }
    }

    if (final_result) {
        // Convert the single bit bool into an int for now.
        final_result = builder.CreateZExt(final_result, Type::getInt32Ty(llvm_context));

        bool is_float = dst.typespec().is_floatbased();
        if (is_float) {
            final_result = llvm_int_to_float (llvm_context, final_result, builder);
        }

        storeLLVMValue(final_result, dst, 0, 0, named_values, sg_ptr, builder);
        if (dst_derivs) {
            // deriv of conditional!?
            printf("punting on derivatives for now\n");
        }
    }
}



void
llvm_if_op (llvm::LLVMContext &llvm_context, const Opcode& op,
            const Symbol& cond, AllocationMap& named_values, Value* sg_ptr,
            IRBuilder<>& builder, int op_index, BasicBlockMap& bb_map)
{
    // Load the condition variable
    Value* cond_load = loadLLVMValue (llvm_context, cond, 0, 0, named_values, sg_ptr, builder);
    // Convert the int to a bool via truncation
    Value* cond_bool = builder.CreateTrunc (cond_load, Type::getInt1Ty(llvm_context));
    // Branch on the condition, to our blocks
    BasicBlock* then_block = bb_map[op_index+1];
    BasicBlock* else_block = bb_map[op.jump(0)];
    BasicBlock* after_block = bb_map[op.jump(1)];
    builder.CreateCondBr (cond_bool, then_block, else_block);
    // Put an unconditional branch at the end of the Then and Else blocks
    if (then_block != after_block) {
        builder.SetInsertPoint (then_block);
        builder.CreateBr (after_block);
    }
    if (else_block != after_block) {
        builder.SetInsertPoint (else_block);
        builder.CreateBr (after_block);
    }
}



void
llvm_loop_op (llvm::LLVMContext &llvm_context, const Opcode& op,
              const Symbol& cond, AllocationMap& named_values,
              Value* sg_ptr, IRBuilder<>& builder,
              int op_index, BasicBlockMap& bb_map)
{
    // Branch on the condition, to our blocks
    BasicBlock* init_block = bb_map[op_index+1];
    BasicBlock* cond_block = bb_map[op.jump(0)];
    BasicBlock* body_block = bb_map[op.jump(1)];
    BasicBlock* step_block = bb_map[op.jump(2)];
    BasicBlock* after_block = bb_map[op.jump(3)];

    // Insert the unconditional jump to the LoopCond
    if (init_block != cond_block) {
        // There are some init ops, insert branch afterwards (but first jump to InitBlock)
        builder.CreateBr(init_block);
        builder.SetInsertPoint(init_block);
    }
    // Either we have init ops (and we'll jump to LoopCond afterwards)
    // or we don't and we need a terminator in the current block. If
    // we're a dowhile loop, we jump to the body block after init
    // instead of cond.
    static ustring op_dowhile("dowhile");
    if (op.opname() == op_dowhile) {
        builder.CreateBr(body_block);
    } else {
        builder.CreateBr(cond_block);
    }

    builder.SetInsertPoint(cond_block);
    // Load the condition variable (it will have been computed by now)
    Value* cond_load = loadLLVMValue(llvm_context, cond, 0, 0, named_values, sg_ptr, builder);
    // Convert the int to a bool via truncation
    Value* cond_bool = builder.CreateTrunc(cond_load, Type::getInt1Ty(llvm_context));
    // Jump to either LoopBody or AfterLoop
    builder.CreateCondBr(cond_bool, body_block, after_block);

    // Put an unconditional jump from Body into Step
    if (step_block != after_block) {
        builder.SetInsertPoint(body_block);
        builder.CreateBr(step_block);

        // Put an unconditional jump from Step to Cond
        builder.SetInsertPoint(step_block);
        builder.CreateBr(cond_block);
    } else {
        // Step is empty, probably a do_while or while loop. Jump from Body to Cond
        builder.SetInsertPoint(body_block);
        builder.CreateBr(cond_block);
    }
}



void
AssignInitialConstant (llvm::LLVMContext &llvm_context, const Symbol& sym,
                       AllocationMap& named_values, Value* sg_ptr,
                       IRBuilder<>& builder)
{
    int num_components = sym.typespec().simpletype().aggregate;
    bool is_float = sym.typespec().is_floatbased();
    if (sym.is_constant() && num_components == 1 && !sym.has_derivs()) {
        // Fill in the constant val
        // Setup initial store
        Value* init_val = 0;
        printf("Assigning initial value for symbol '%s' = ", sym.mangled().c_str());
        if (is_float) {
            float fval = *((float*)sym.data());
            init_val = ConstantFP::get(llvm_context, APFloat(fval));
            printf("%f\n", fval);
        } else {
            int ival = *((int*)sym.data());
            init_val = ConstantInt::get(llvm_context, APInt(32, ival));
            printf("%d\n", ival);
        }
        storeLLVMValue(init_val, sym, 0, 0, named_values, sg_ptr, builder);
    }
}



#if 1

llvm::Function*
ShaderInstance::buildLLVMVersion (llvm::LLVMContext& llvm_context,
                                  llvm::Module* all_ops)
{
    // I'd like our new function to take just a ShaderGlobals...
    char unique_layer_name[1024];
    sprintf(unique_layer_name, "%s_%d", layername().c_str(), id());
    const llvm::StructType* sg_type = getShaderGlobalType(llvm_context);
    llvm::PointerType* sg_ptr_type = PointerType::get(sg_type, 0 /* Address space */);
    // Make a layer function: void layer_func(ShaderGlobal*)
    llvm::Function* layer_func = cast<Function>(all_ops->getOrInsertFunction(unique_layer_name, Type::getVoidTy(llvm_context), sg_ptr_type, NULL));
    const OpcodeVec& instance_ops = ops();
    AllocationMap named_values;
    Function::arg_iterator arg_it = layer_func->arg_begin();
    Value* sg_ptr = arg_it++;

    BasicBlock* entry_bb = BasicBlock::Create(llvm_context, "EntryBlock", layer_func);
    IRBuilder<> builder(entry_bb);

    // Setup the symbols
    for (size_t i = 0; i < m_instsymbols.size(); i++) {
        const Symbol &s (*symbol(i));
        if (SkipSymbol(s)) continue;
        // Don't allocate globals
        if (s.symtype() == SymTypeGlobal) continue;
        // Make space
        getOrAllocateLLVMSymbol(llvm_context, s, named_values, sg_ptr, layer_func);
        if (s.is_constant())
            AssignInitialConstant(llvm_context, s, named_values, sg_ptr, builder);
    }

    // All the symbols are stack allocated now.

    // Go learn about the BasicBlock's we'll need to make. NOTE(boulos):
    // The definition of BasicBlock here follows the LLVM version which
    // differs from that in runtimeoptimize.cpp. In particular, the
    // instructions in a Then block are part of a new
    // BasicBlock. QUESTION(boulos): This actually should be true for
    // runtimeoptimize as well. If you happened to define a variable in
    // a condition (which is bad mojo, but legal) the aliasing stuff is
    // probably wrong...
    std::vector<bool> bb_start (instance_ops.size(), false);

    for (size_t i = 0; i < instance_ops.size(); i++) {
        static ustring op_if("if");
        static ustring op_for("for");
        static ustring op_while("while");
        static ustring op_dowhile("dowhile");

        const Opcode& op = instance_ops[i];
        if (op.opname() == op_if) {
            // For a true BasicBlock, since we are going to conditionally
            // jump into the ThenBlock, we need to label the next
            // instruction as starting ThenBlock.
            bb_start[i+1] = true;
            // The ElseBlock also can be jumped to
            bb_start[op.jump(0)] = true;
            // And ExitBlock
            bb_start[op.jump(1)] = true;
        } else if (op.opname() == op_for ||
                   op.opname() == op_while ||
                   op.opname() == op_dowhile) {
            bb_start[i+1] = true; // LoopInit
            bb_start[op.jump(0)] = true; // LoopCond
            bb_start[op.jump(1)] = true; // LoopBody
            bb_start[op.jump(2)] = true; // LoopStep
            bb_start[op.jump(3)] = true; // AfterLoop
        }
    }

    // Create a map from ops with bb_start=true to their BasicBlock*
    BasicBlockMap bb_map;
    for (size_t i = 0; i < instance_ops.size(); i++) {
        if (bb_start[i]) {
            bb_map[i] = BasicBlock::Create(llvm_context, "", layer_func);
        }
    }

    for (size_t i = 0; i < instance_ops.size(); i++) {
        const Opcode& op = instance_ops[i];

        if (bb_start[i]) {
            // If we start a new BasicBlock, point the builder there.
            BasicBlock* next_bb = bb_map[i];
            if (next_bb != entry_bb) {
                // If we're not the entry block (which is where all the
                // AllocaInstructions go), then start insertion at the
                // beginning of the block. This way we can insert instructions
                // before the possible jmp inserted at the end by an upstream
                // conditional (e.g. if/for/while/do)
                builder.SetInsertPoint(next_bb, next_bb->begin());
            } else {
                // Otherwise, use the end (IRBuilder default)
                builder.SetInsertPoint(next_bb);
            }
        }

        static ustring op_add("add");
        static ustring op_sub("sub");
        static ustring op_mul("mul");
        static ustring op_div("div");
        static ustring op_mod("mod");
        static ustring op_lt("lt");
        static ustring op_le("le");
        static ustring op_eq("eq");
        static ustring op_ge("ge");
        static ustring op_gt("gt");
        static ustring op_neq("neq");
        static ustring op_printf("printf");
        static ustring op_assign("assign");
        static ustring op_neg("neg");
        static ustring op_nop("nop");
        static ustring op_end("end");
        static ustring op_if("if");
        static ustring op_for("for");
        static ustring op_while("while");
        static ustring op_dowhile("dowhile");

        //printf("op%03zu: %s\n", i, op.opname().c_str());

        if (op.opname() == op_add ||
            op.opname() == op_sub ||
            op.opname() == op_mul ||
            op.opname() == op_div ||
            op.opname() == op_mod) {
            Symbol& dst = *argsymbol(op.firstarg() + 0);
            Symbol& src1 = *argsymbol(op.firstarg() + 1);
            Symbol& src2 = *argsymbol(op.firstarg() + 2);

            if (SkipSymbol(dst) ||
                SkipSymbol(src1) ||
                SkipSymbol(src2))
                continue;

            llvm_binary_op (llvm_context, op, dst, src1, src2, named_values, sg_ptr, builder);
        } else if (op.opname() == op_lt ||
                   op.opname() == op_le ||
                   op.opname() == op_eq ||
                   op.opname() == op_ge ||
                   op.opname() == op_gt ||
                   op.opname() == op_neq) {
            Symbol& dst = *argsymbol(op.firstarg() + 0);
            Symbol& src1 = *argsymbol(op.firstarg() + 1);
            Symbol& src2 = *argsymbol(op.firstarg() + 2);

            if (SkipSymbol(dst) ||
                SkipSymbol(src1) ||
                SkipSymbol(src2))
                continue;

            llvm_compare_op (llvm_context, op, dst, src1, src2, named_values, sg_ptr, builder);
        } else if (op.opname() == op_neg) {
            Symbol& dst = *argsymbol(op.firstarg() + 0);
            Symbol& src = *argsymbol(op.firstarg() + 1);
            if (SkipSymbol(dst) ||
                SkipSymbol(src))
                continue;

            llvm_unary_op (llvm_context, op, dst, src, named_values, sg_ptr, builder);
        } else if (op.opname() == op_printf) {
            llvm_printf_op(llvm_context, this, op, named_values, sg_ptr, builder, getPrintfFunc(all_ops));
        } else if (op.opname() == op_assign) {
            Symbol& dst = *argsymbol(op.firstarg() + 0);
            Symbol& src = *argsymbol(op.firstarg() + 1);
            if (SkipSymbol(dst) ||
                SkipSymbol(src)) continue;
            llvm_assign_op (llvm_context, op, dst, src, named_values, sg_ptr, builder);
        } else if (op.opname() == op_if ||
                   op.opname() == op_for ||
                   op.opname() == op_while ||
                   op.opname() == op_dowhile) {
            Symbol& cond = *argsymbol(op.firstarg() + 0);
            if (SkipSymbol(cond)) continue;
            if (op.opname() == op_if) {
                llvm_if_op (llvm_context, op, cond, named_values, sg_ptr, builder, i, bb_map);
            } else {
                llvm_loop_op (llvm_context, op, cond, named_values, sg_ptr, builder, i, bb_map);
            }
        } else if (op.opname() == op_nop ||
                   op.opname() == op_end) {
            // Skip this op, it does nothing...
        } else {
            printf("LLVMOSL: Unsupported op %s\n", op.opname().c_str());
        }
    }

    builder.CreateRetVoid();

    outs() << "layer_func (" << unique_layer_name << ") after llvm  = " << *layer_func << "\n";

    // Now optimize the result
    shadingsys().FunctionOptimizer()->run(*layer_func);

    outs() << "layer_func (" << unique_layer_name << ") after opt  = " << *layer_func << "\n";

    return layer_func;
}

#else // Old code based on reading the ops

llvm::Function *
ShaderInstance::buildLLVMVersion (llvm::LLVMContext& llvm_context,
                                  llvm::Module* all_ops)
{
    // grab the arg format from op_assign (all ops are the same)
    Function* op_assign = all_ops->getFunction("OP_assign");
    const FunctionType* opdecl = op_assign->getFunctionType();
    // Now we can grab the ShadingExecution* type
    const Type* exec_type = opdecl->getParamType(0);
    const Type* args_type = Type::getInt32PtrTy(llvm_context);
    // The layer function will just take a ShadingExecution* at runtime
    char unique_layer_name[1024];
    sprintf(unique_layer_name, "%s_%d", layername().c_str(), id());
    llvm::Function* layer_func = cast<Function>(all_ops->getOrInsertFunction(unique_layer_name, Type::getVoidTy(llvm_context), exec_type, args_type, NULL));

    Function::arg_iterator arg_iterator = layer_func->arg_begin();
    Value* exec_ptr = arg_iterator++;
    Value* args_base = arg_iterator++;

    const OpcodeVec& instance_ops = ops();
    //const std::vector<int>& instance_args = args();

    BasicBlock* bb = BasicBlock::Create(llvm_context, "EntryBlock", layer_func);

    //outs() << "layer (" << layername().c_str() << "). Making calls for " << instance_ops.size() << " ops.\n";

    for (size_t i = 0; i < instance_ops.size(); i++) {
        const Opcode& op = instance_ops[i];
        Function* opfunc = NULL;
        static ustring abs = ustring("abs");
        if (op.opname() == abs) {
            opfunc = all_ops->getFunction("OP_fabs");
        } else {
            char buf[1024];
            sprintf(buf, "OP_%s", op.opname().c_str());
            opfunc = all_ops->getFunction(buf);
            if (!opfunc) {
                printf("Didn't find op %s\n", buf);
            }
        }

        // Need to pass in ShadingExecution*, nargs and a pointer to the
        // args
        ConstantInt* const_first_arg = ConstantInt::get(llvm_context, APInt(32, op.firstarg()));
        ConstantInt* const_nargs = ConstantInt::get(llvm_context, APInt(32, op.nargs()));


        GetElementPtrInst* op_args_ptr = GetElementPtrInst::Create(args_base, const_first_arg, "", bb);

        Value* opfunc_args[] = {
            exec_ptr,
            const_nargs,
            op_args_ptr
        };

        CallInst::Create (opfunc, opfunc_args, opfunc_args + 3, "", bb);
    }

    // Add a return void statement
    ReturnInst::Create(llvm_context, bb);

    //outs() << "layer_func (" << layername().c_str() << ") before opt  = " << *layer_func << "\n";

    // Inline stuff first (our function is just gep + call)
    shadingsys().IPOOptimizer()->run (*all_ops);
    // Now optimize the result
    shadingsys().FunctionOptimizer()->run (*layer_func);

    //outs() << "layer_func (" << layername().c_str() << ") after opt  = " << *layer_func << "\n";

    return layer_func;
}

#endif

}; // namespace pvt
}; // namespace osl

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif
