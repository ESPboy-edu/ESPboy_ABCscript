#include "ards_compiler.hpp"

#include <algorithm>
#include <assert.h>

namespace ards
{

static void implicit_conversion(compiler_type_t& ta, compiler_type_t& tb)
{
    if(ta == tb) return;
    if(ta.is_signed == tb.is_signed)
    {
        if(ta.prim_size < tb.prim_size)
            ta = tb;
    }
    else if(ta.is_signed)
    {
        // a signed, b unsigned
        if(ta.prim_size <= tb.prim_size)
            ta = tb;
        else
            tb = ta;
    }
}

void compiler_t::type_annotate(ast_node_t& a, compiler_frame_t const& frame)
{
    if(!errs.empty()) return;
    switch(a.type)
    {
    case AST::OP_UNARY:
    {
        assert(a.children.size() == 2);
        type_annotate(a.children[1], frame);
        auto op = a.children[0].data;
        if(!a.children[0].comp_type.is_prim())
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) + "\" is not a primitive type",
                a.children[0].line_info });
            break;
        }
        if(op == "!")
            a.comp_type = TYPE_BOOL;
        else if(op == "-")
            a.comp_type = a.children[1].comp_type;
        else
        {
            assert(false);
        }
        break;
    }
    case AST::OP_ASSIGN:
    {
        for(auto& child : a.children)
            type_annotate(child, frame);
        assert(a.children.size() == 2);
        a.comp_type = a.children[0].comp_type;
        break;
    }
    case AST::OP_EQUALITY:
    case AST::OP_RELATIONAL:
    case AST::OP_ADDITIVE:
    case AST::OP_MULTIPLICATIVE:
    {
        // C-style implicit conversion rules
        assert(a.children.size() == 2);
        for(auto& child : a.children)
            type_annotate(child, frame);
        auto t0 = a.children[0].comp_type;
        auto t1 = a.children[1].comp_type;
        if(!t0.is_prim())
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) + "\" is not a primitive type",
                a.children[0].line_info });
            break;
        }
        if(!t1.is_prim())
        {
            errs.push_back({
                "\"" + std::string(a.children[1].data) + "\" is not a primitive type",
                a.children[1].line_info });
            break;
        }
        t0.is_bool = false;
        t1.is_bool = false;
        if(t0 != t1)
        {
            implicit_conversion(t0, t1);
            implicit_conversion(t1, t0);

            if(t0 != a.children[0].comp_type)
            {
                auto child = std::move(a.children[0]);
                a.children[0] = { {}, AST::OP_CAST };
                a.children[0].comp_type = t0;
                a.children[0].children.push_back({});
                a.children[0].children.back().comp_type = t0;
                a.children[0].children.emplace_back(std::move(child));
            }
            if(t1 != a.children[1].comp_type)
            {
                auto child = std::move(a.children[1]);
                a.children[1] = { {}, AST::OP_CAST };
                a.children[1].comp_type = t1;
                a.children[1].children.push_back({});
                a.children[1].children.back().comp_type = t1;
                a.children[1].children.emplace_back(std::move(child));
            }
        }
        if(a.type == AST::OP_EQUALITY || a.type == AST::OP_RELATIONAL)
            a.comp_type = TYPE_BOOL;
        else
            a.comp_type = a.children[0].comp_type;
        break;
    }
    {
        assert(a.children.size() == 2);
        for(auto& child : a.children)
            type_annotate(child, frame);
        a.comp_type.is_signed =
            a.children[0].comp_type.is_signed ||
            a.children[1].comp_type.is_signed;
        a.comp_type.prim_size = std::max(
            a.children[0].comp_type.prim_size,
            a.children[1].comp_type.prim_size);
        break;
    }
    case AST::INT_CONST:
        // already done during parsing
        break;
    case AST::IDENT:
    {
        std::string name(a.data);
        for(auto it = frame.scopes.rbegin(); it != frame.scopes.rend(); ++it)
        {
            auto jt = it->locals.find(name);
            if(jt != it->locals.end())
            {
                a.comp_type = jt->second.type;
                return;
            }
        }
        auto it = globals.find(name);
        if(it != globals.end())
        {
            a.comp_type = it->second.type;
            return;
        }
        errs.push_back({ "Undefined variable \"" + name + "\"", a.line_info });
        break;
    }
    case AST::FUNC_CALL:
    {
        assert(a.children.size() == 2);
        for(size_t i = 0; i < a.children[1].children.size(); ++i)
            type_annotate(a.children[1].children[i], frame);
        auto f = resolve_func(a.children[0]);
        a.comp_type = f.decl.return_type;
        break;
    }
    case AST::ARRAY_INDEX:
    {
        assert(a.children.size() == 2);
        type_annotate(a.children[0], frame);
        type_annotate(a.children[1], frame);
        auto t0 = a.children[0].comp_type;
        auto t1 = a.children[1].comp_type;
        auto tt = t0.ref_type();
        if(tt != compiler_type_t::ARRAY &&
            tt != compiler_type_t::ARRAY_REF)
        {
            errs.push_back({
                "\"" + std::string(a.children[0].data) +
                "\" is not an array or array reference", a.line_info });
            break;
        }
        a.comp_type.type = compiler_type_t::REF;
        a.comp_type.prim_size = 2;
        a.comp_type.children.push_back(t0.children[0]);
        break;
    }
    default:
        break;
    }
}

}
