/*
Copyright (c) 2009 Sony Pictures Imageworks, et al.
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

#include <vector>
#include <string>
#include <iostream>

#include <boost/foreach.hpp>

#include "OpenImageIO/dassert.h"
#include "OpenImageIO/strutil.h"

#include "oslcomp_pvt.h"
#include "symtab.h"
#include "ast.h"


#ifdef OSL_NAMESPACE
namespace OSL_NAMESPACE {
#endif

namespace OSL {
namespace pvt {   // OSL::pvt



int
OSLCompilerImpl::emitcode (const char *opname, size_t nargs, Symbol **args,
                           ASTNode *node)
{
//    std::cout << "\temit " << opname;
    int opnum = (int) m_ircode.size();
    Opcode op (ustring (opname), m_codegenmethod, m_opargs.size(), nargs);
    op.source (node->sourcefile(), node->sourceline());
    m_ircode.push_back (op);
    for (size_t i = 0;  i < nargs;  ++i) {
        ASSERT (args[i]);
        m_opargs.push_back (args[i]);
//        std::cout << " " << (args[i] ? args[i]->name() : ustring("<null>"));
    }
//    std::cout << "\n";
    return opnum;
}



Symbol *
OSLCompilerImpl::make_temporary (const TypeSpec &type)
{
    ustring name = ustring::format ("$tmp%d", ++m_next_temp);
    Symbol *s = new Symbol (name, type, SymTypeTemp);
    symtab().insert (s);
    return s;
}



Symbol *
OSLCompilerImpl::make_constant (ustring val)
{
    BOOST_FOREACH (ConstantSymbol *sym, m_const_syms) {
        if (sym->typespec().is_string() && sym->strval() == val)
            return sym;
    }
    // It's not a constant we've added before
    ustring name = ustring::format ("$const%d", ++m_next_const);
    ConstantSymbol *s = new ConstantSymbol (name, val);
    symtab().insert (s);
    m_const_syms.push_back (s);
    return s;
}



Symbol *
OSLCompilerImpl::make_constant (int val)
{
    BOOST_FOREACH (ConstantSymbol *sym, m_const_syms) {
        if (sym->typespec().is_int() && sym->intval() == val)
            return sym;
    }
    // It's not a constant we've added before
    ustring name = ustring::format ("$const%d", ++m_next_const);
    ConstantSymbol *s = new ConstantSymbol (name, val);
    symtab().insert (s);
    m_const_syms.push_back (s);
    return s;
}



Symbol *
OSLCompilerImpl::make_constant (float val)
{
    BOOST_FOREACH (ConstantSymbol *sym, m_const_syms) {
        if (sym->typespec().is_float() && sym->floatval() == val)
            return sym;
    }
    // It's not a constant we've added before
    ustring name = ustring::format ("$const%d", ++m_next_const);
    ConstantSymbol *s = new ConstantSymbol (name, val);
    symtab().insert (s);
    m_const_syms.push_back (s);
    return s;
}



int
ASTNode::emitcode (const char *opname, Symbol *arg0, 
                   Symbol *arg1, Symbol *arg2, Symbol *arg3)
{
    Symbol *args[4] = { arg0, arg1, arg2, arg3 };
    size_t nargs = (arg0 != NULL) + (arg1 != NULL) + 
                   (arg2 != NULL) + (arg3 != NULL);
    return m_compiler->emitcode (opname, nargs, args, this);
}



int
ASTNode::emitcode (const char *opname, size_t nargs, Symbol **args)
{
    return m_compiler->emitcode (opname, nargs, args, this);
}



Symbol *
ASTNode::codegen (Symbol *dest)
{
    codegen_children ();
    // FIXME -- nobody should ever call this
    std::cout << "codegen " << nodetypename() << " : " 
              << (opname() ? opname() : "") << "\n";
    return NULL;
}



void
ASTNode::codegen_children ()
{
    BOOST_FOREACH (ref &c, m_children) {
        codegen_list (c);
    }
}



void
ASTNode::codegen_list (ref node)
{
    while (node) {
        node->codegen ();
        node = node->next ();
    }
}



Symbol *
ASTshader_declaration::codegen (Symbol *dest)
{
    for (ref f = formals();  f;  f = f->next()) {
        ASSERT (f->nodetype() == ASTNode::variable_declaration_node);
        ASTvariable_declaration *v = (ASTvariable_declaration *) f.get();
        if (v->init()) {
            // If the initializer is a single literal, we will output
            // it as a constant in the symbol definition, no need for ops.
            if (v->init()->nodetype() == literal_node && ! v->init()->next())
                continue;

            m_compiler->codegen_method (v->name());
            v->codegen ();
        }
    }

    m_compiler->codegen_method (ustring ("main"));
    codegen_list (statements());
    return NULL;
}



Symbol *
ASTassign_expression::codegen (Symbol *dest)
{
    ASTindex *index = NULL;
    if (var()->nodetype() == index_node) {
        // Assigning to an individual component or array element
        index = (ASTindex *) var().get();
    }
    dest = index ? NULL : var()->codegen();
    if (m_op == Assign) {
        Symbol *operand = expr()->codegen (dest);
        // FIXME -- what about coerced types, do we need a temp and copy here?
        if (index)
            index->codegen_assign (operand);
        else if (operand != dest)
            emitcode ("assign", dest, operand);
    } else {
        Symbol *operand = expr()->codegen ();
        // FIXME -- what about coerced types, do we need a temp and copy here?
        if (index) {
            index->codegen_assign (operand);
            // FIXME -- wrong
        }
        else
            emitcode (opword(), dest, dest, operand);
    }

    return dest;
}



Symbol *
ASTvariable_declaration::codegen (Symbol *)
{
    if (init()) {
        Symbol *dest = init()->codegen (m_sym);
        if (dest != m_sym)
            emitcode ("assign", m_sym, dest);
    }        
    return m_sym;
}



Symbol *
ASTvariable_ref::codegen (Symbol *)
{
    return m_sym;
}



Symbol *
ASTpreincdec::codegen (Symbol *)
{
    Symbol *sym = var()->codegen ();
    Symbol *one = sym->typespec().is_int() ? m_compiler->make_constant(1)
                                           : m_compiler->make_constant(1.0f);
    emitcode (m_op == Incr ? "add" : "sub", sym, sym, one);
    // FIXME -- what if it's an indexed lvalue, like v[i]?
    return sym;
}



Symbol *
ASTpostincdec::codegen (Symbol *dest)
{
    Symbol *sym = var()->codegen ();
    Symbol *one = sym->typespec().is_int() ? m_compiler->make_constant(1)
                                           : m_compiler->make_constant(1.0f);
    if (! dest)
        dest = m_compiler->make_temporary (sym->typespec());
    emitcode ("assign", dest, sym);
    emitcode (m_op == Incr ? "add" : "sub", sym, sym, one);
    // FIXME -- what if it's an indexed lvalue, like v[i]?
    return dest;
}



Symbol *
ASTindex::codegen (Symbol *dest)
{
    Symbol *lv = lvalue()->codegen ();
    Symbol *ind = index()->codegen ();
    if (! dest)
        dest = m_compiler->make_temporary (typespec());
    if (lv->typespec().is_array()) {
        emitcode ("aref", dest, lv, ind);
    } else if (lv->typespec().is_triple()) {
        emitcode ("compref", dest, lv, ind);
    } else if (lv->typespec().is_matrix()) {
        Symbol *ind2 = index2()->codegen ();
        emitcode ("mxcompref", dest, lv, ind, ind2);
    } else {
        ASSERT (0);
    }
    return dest;
}



void
ASTindex::codegen_assign (Symbol *src)
{
    Symbol *lv = lvalue()->codegen ();
    Symbol *ind = index()->codegen ();
    if (lv->typespec().is_array()) {
        emitcode ("aassign", lv, ind, src);
    } else if (lv->typespec().is_triple()) {
        emitcode ("compassign", lv, ind, src);
    } else if (lv->typespec().is_matrix()) {
        Symbol *ind2 = index2()->codegen ();
        emitcode ("mxcompassign", lv, ind, ind2, src);
    } else {
        ASSERT (0);
    }
}



Symbol *
ASTconditional_statement::codegen (Symbol *)
{
    Symbol *condvar = cond()->codegen ();
    TypeSpec condtype = condvar->typespec();
    if (! condtype.is_int()) {
        // If they're not using an int as the condition, then it's an
        // implied comparison to zero.
        Symbol *tempvar = m_compiler->make_temporary (TypeDesc::TypeInt);
        Symbol *zerovar = condtype.is_string() ? 
                            m_compiler->make_constant (ustring("")) : 
                            m_compiler->make_constant (0.0f);
        emitcode ("ne", tempvar, condvar, zerovar);
        condvar = tempvar;
    }

    // Generate the op for the 'if' itself.  Record its label, so that we
    // can go back and patch it with the jump destinations.
    int ifop = emitcode ("if", condvar);

    // Generate the code for the 'true' and 'false' code blocks, recording
    // the jump destinations for 'else' and the next op after the if.
    m_compiler->next_op_label ();
    codegen_list (truestmt());
    int falselabel = m_compiler->next_op_label ();
    codegen_list (falsestmt());
    int donelabel = m_compiler->next_op_label ();

    // Fix up the 'if' to have the jump destinations.
    m_compiler->ircode(ifop).set_jump (falselabel, donelabel);

    // FIXME -- account for the fact that the first argument, unlike
    // almost all other ops, is read, not written
    return NULL;
}



Symbol *
ASTunary_expression::codegen (Symbol *dest)
{
    // Code generation for unary expressions (-x, !x, etc.)

    // Generate the code for our expression
    Symbol *esym = expr()->codegen ();

    if (m_op == Add) {
        // Special case: +x just returns x.
        return esym;
    }

    // If we were not given a requested destination, or if it is not of
    // the right type, make a temporary.
    if (dest == NULL || ! equivalent (dest->typespec(), typespec()))
        dest = m_compiler->make_temporary (typespec());

    // FIXME -- what about coerced types, do we need a temp and copy here?

    // Generate the opcode
    emitcode (opword(), dest, esym);

    return dest;
}



Symbol *
ASTbinary_expression::codegen (Symbol *dest)
{
    Symbol *lsym = left()->codegen ();
    Symbol *rsym = right()->codegen ();
    if (dest == NULL || ! equivalent (dest->typespec(), typespec()))
        dest = m_compiler->make_temporary (typespec());

    // FIXME -- what about coerced types, do we need a temp and copy here?

    // FIXME -- we want && and || to properly short-circuit

    emitcode (opword(), dest, lsym, rsym);
    return dest;
}



Symbol *
ASTtypecast_expression::codegen (Symbol *dest)
{
    Symbol *e = expr()->codegen ();

    // If the cast is a null operation -- they are already the same types,
    // or we're converting one triple to another -- just pass the expression.
    if (equivalent (typespec(), e->typespec()))
        return e;

    // Some actual conversion is necessary.  Generally, our "assign"
    // op can handle it all easily.
    if (dest == NULL || ! equivalent (dest->typespec(), typespec()))
        dest = m_compiler->make_temporary (typespec());
    emitcode ("assign", dest, e);
    return dest;
}



Symbol *
ASTtype_constructor::codegen (Symbol *dest)
{
    if (dest == NULL || ! equivalent (dest->typespec(), typespec()))
        dest = m_compiler->make_temporary (typespec());
    std::vector<Symbol *> argdest;
    argdest.push_back (dest);
    for (ref a = args();  a;  a = a->next()) {
        Symbol *argval = a->codegen();
        if (argval->typespec().is_int()) {
            // Coerce to float if it's an int
            if (a->nodetype() == literal_node) {
                // It's a literal int, so let's make a literal float
                int i = ((ASTliteral *)a.get())->intval ();
                argval = m_compiler->make_constant ((float)i);
            } else {
                // General case
                Symbol *tmp = argval;
                argval = m_compiler->make_temporary (TypeSpec(TypeDesc::FLOAT));
                emitcode ("assign", argval, tmp);
            }
        }
        argdest.push_back (argval);
    }
    emitcode (typespec().string().c_str(), argdest.size(), &argdest[0]);
    return dest;
}



Symbol *
ASTfunction_call::codegen (Symbol *dest)
{
    // FIXME -- this is very wrong, just a placeholder

    std::vector<Symbol *> argdest;
    if (! typespec().is_void()) {
        if (dest == NULL || ! equivalent (dest->typespec(), typespec()))
            dest = m_compiler->make_temporary (typespec());
        argdest.push_back (dest);
    }
    for (ref a = args();  a;  a = a->next()) {
        argdest.push_back (a->codegen());
    }
    emitcode (m_name.c_str(), argdest.size(), &argdest[0]);
    return dest;
}



Symbol *
ASTliteral::codegen (Symbol *dest)
{
    TypeSpec t = typespec();
    if (t.is_string())
        return m_compiler->make_constant (ustring(strval()));
    if (t.is_int())
        return m_compiler->make_constant (intval());
    if (t.is_float())
        return m_compiler->make_constant (floatval());
    ASSERT (0 && "Don't know how to generate code for this literal");
    return NULL;
}



}; // namespace pvt
}; // namespace OSL

#ifdef OSL_NAMESPACE
}; // end namespace OSL_NAMESPACE
#endif